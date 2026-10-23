"""PPO + 2x128 tanh MLP + sin/cos H=4 observation, angle-only simulator.

Experiments:
  --init rand   : every training reset draws theta_i ~ U(-.05,.05) and
                  omega_i ~ U(-.02,.02), with no fixed bias.
  --init fixed  : every training reset is exactly the evaluation state
                  theta = [0.013,-0.008,0.021], omega = 0.

The simulator returns only theta and raises EpisodeLengthError after 3000
steps.  Everything else -- observation, reward, done, success, t_can, return
-- is built here and in task.py, never in the simulator.

TensorBoard scalars (tb/<exp>/):
  train/episode_return, train/episode_length, train/t_can, train/t_can_max,
  train/success_rate, train/ever_up_frac, train/first_up_time,
  train/mean_step_reward, train/action_abs_mean, train/std, train/lr,
  train/approx_kl, train/clip_frac,
  loss/policy, loss/value, loss/entropy,
  eval/t_can, eval/success, eval/ep_return, eval/ever_up, eval/final_ang_err,
  eval/first_up_t, eval/up_frac, eval_final/*,
  train/global_steps, time/elapsed_min
"""
from __future__ import annotations

import argparse
import json
import os
import time

import numpy as np
import torch
import torch.nn as nn
from torch.utils.tensorboard import SummaryWriter

torch.set_num_threads(int(os.environ.get("TORCH_THREADS", "4")))

from evaluate import evaluate, evaluate_starts, make_ppo_policy
from models import ActorCritic
from obs import OBS_MODES, obs_spec
from sim import EPISODE_STEPS, Params, TorchVecSim
from task import (EVAL_THETA0, TORCH_REWARD_NOTE, TorchTask,
                  get_reward_preset, make_init, sample_train_init)


def collect(sim, task, ac, T, args, gen, ep_stats):
    B = sim.n
    obs = task.obs
    obs_buf = torch.zeros(T, B, obs.shape[-1])
    act_buf = torch.zeros(T, B, 2)
    logp_buf = torch.zeros(T, B)
    rew_buf = torch.zeros(T, B)
    done_buf = torch.zeros(T, B)
    val_buf = torch.zeros(T, B)
    for t in range(T):
        with torch.no_grad():
            dist = ac.dist(obs)
            a = dist.sample()
            logp = dist.log_prob(a).sum(-1)
            v = ac.value(obs)
        theta = sim.step(a)
        nobs, rew = task.step(theta, a)
        done = sim.steps >= sim.episode_steps
        obs_buf[t] = obs
        act_buf[t] = a
        logp_buf[t] = logp
        rew_buf[t] = rew
        done_buf[t] = done.float()
        val_buf[t] = v
        if bool(done.any()):
            idx = done.nonzero().flatten()
            summ = task.summary(idx)
            ep_stats["return"] += summ["ep_return"].tolist()
            ep_stats["t_can"] += summ["t_can"].tolist()
            ep_stats["ever_up"] += summ["ever_up"].tolist()
            ep_stats["first_up"] += summ["first_up_t"].tolist()
            ep_stats["up_frac"] += summ["up_frac"].tolist()
            ep_stats["length"] += summ["length"].tolist()
            th, om = make_init(idx.numel(), args.init, gen, up_frac=args.up_frac)
            reset_theta = sim.reset(th, om, idx)
            task.reset(reset_theta, idx)
            nobs = task.obs
        obs = nobs
    with torch.no_grad():
        last_v = ac.value(obs)
    return dict(obs=obs_buf, act=act_buf, logp=logp_buf, rew=rew_buf,
                done=done_buf, val=val_buf, last_v=last_v, last_obs=obs)


def gae(rb, vb, db, last_v, gamma, lam):
    T, B = rb.shape
    adv = torch.zeros(T, B)
    last = torch.zeros(B)
    for t in reversed(range(T)):
        next_v = last_v if t == T - 1 else vb[t + 1]
        nonterm = 1.0 - db[t]
        delta = rb[t] + gamma * next_v * nonterm - vb[t]
        last = delta + gamma * lam * nonterm * last
        adv[t] = last
    return adv, adv + vb


def ppo_update(ac, opt, data, args):
    T, B = data["rew"].shape
    obs = data["obs"].reshape(T * B, -1)
    act = data["act"].reshape(T * B, ac.act_dim)
    old_logp = data["logp"].reshape(T * B)
    adv = data["adv"].reshape(T * B)
    ret = data["ret"].reshape(T * B)
    adv = (adv - adv.mean()) / (adv.std() + 1e-8)
    n = obs.shape[0]
    stats = dict(pg=[], v=[], ent=[], kl=[], cf=[])
    stop = False
    ep = 0
    for ep in range(args.epochs):
        idx = torch.randperm(n)
        for s in range(0, n, args.minibatch):
            mb = idx[s:s + args.minibatch]
            dist = ac.dist(obs[mb])
            logp = dist.log_prob(act[mb]).sum(-1)
            ratio = (logp - old_logp[mb]).exp()
            a = adv[mb]
            pg = torch.max(-a * ratio,
                           -a * torch.clamp(ratio, 1 - args.clip, 1 + args.clip)).mean()
            v = ac.value(obs[mb])
            v_loss = 0.5 * ((v - ret[mb]) ** 2).mean()
            ent = dist.entropy().sum(-1).mean()
            loss = pg + args.vf_coef * v_loss - args.ent * ent
            opt.zero_grad()
            loss.backward()
            nn.utils.clip_grad_norm_(ac.parameters(), 0.5)
            opt.step()
            with torch.no_grad():
                kl = ((ratio - 1) - (logp - old_logp[mb])).mean().item()
                cf = ((ratio - 1).abs() > args.clip).float().mean().item()
            stats["pg"].append(pg.item()); stats["v"].append(v_loss.item())
            stats["ent"].append(ent.item()); stats["kl"].append(kl)
            stats["cf"].append(cf)
            if kl > 2.0 * args.target_kl:
                stop = True
                break
        if stop:
            break
    out = {k: float(np.mean(v)) for k, v in stats.items()}
    out["epochs"] = ep + 1
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exp", type=str, required=True)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--init", type=str, default="rand",
                    choices=["rand", "fixed", "mix"],
                    help="rand=down noise, fixed=eval state, "
                         "mix=curriculum with up_frac near upright")
    ap.add_argument("--up-frac", type=float, default=0.5,
                    help="fraction of resets near upright when --init mix")
    ap.add_argument("--obs", type=str, default="full",
                    choices=sorted(OBS_MODES),
                    help="observation mode (see obs.py)")
    ap.add_argument("--reward", type=str, default="base",
                    choices=["base", "sparse", "energy"],
                    help="reward preset (see task.py REWARD_PRESETS)")
    ap.add_argument("--envs", type=int, default=64)
    ap.add_argument("--horizon", type=int, default=600)
    ap.add_argument("--minutes", type=float, default=25.0)
    ap.add_argument("--iters", type=int, default=10 ** 9)
    ap.add_argument("--lr", type=float, default=3e-4)
    ap.add_argument("--gamma", type=float, default=0.99)
    ap.add_argument("--lam", type=float, default=0.95)
    ap.add_argument("--clip", type=float, default=0.2)
    ap.add_argument("--epochs", type=int, default=8)
    ap.add_argument("--minibatch", type=int, default=2048)
    ap.add_argument("--ent", type=float, default=0.003)
    ap.add_argument("--vf-coef", type=float, default=0.5)
    ap.add_argument("--target-kl", type=float, default=0.03)
    ap.add_argument("--hidden", type=int, default=128)
    ap.add_argument("--depth", type=int, default=2)
    ap.add_argument("--episode-steps", type=int, default=EPISODE_STEPS)
    ap.add_argument("--eval-every", type=int, default=5)
    ap.add_argument("--tb-root", type=str, default="tb")
    ap.add_argument("--ckpt-dir", type=str, default="checkpoints")
    ap.add_argument("--out", type=str, default="results")
    ap.add_argument("--tau-max", type=float, default=18.0)
    ap.add_argument("--damping", type=float, default=0.01)
    ap.add_argument("--log-every", type=int, default=1)
    ap.add_argument("--robust-starts", type=int, default=8,
                    help="deterministic extra rollouts from the training "
                         "reset distribution at the end (0 disables)")
    args = ap.parse_args()

    torch.manual_seed(args.seed)
    np.random.seed(args.seed)
    for d in (args.tb_root, args.ckpt_dir, args.out):
        os.makedirs(d, exist_ok=True)
    exp = args.exp
    writer = SummaryWriter(os.path.join(args.tb_root, exp))
    p = Params(tau_max=args.tau_max, b=args.damping)
    spec = obs_spec(args.obs)
    obs_dim = spec["dim"]
    reward_w = get_reward_preset(args.reward)
    sim = TorchVecSim(p, n=args.envs, episode_steps=args.episode_steps)
    task = TorchTask(p, n=args.envs, reward_w=reward_w, obs_mode=args.obs)
    gen = torch.Generator().manual_seed(args.seed * 1000 + 7)

    init_meta = dict(mode=args.init,
                     training=("theta_i ~ U(-0.05,0.05), omega_i ~ U(-0.02,0.02); no bias"
                               if args.init == "rand" else
                               ("theta=[0.013,-0.008,0.021], omega=0 (same as eval)"
                                if args.init == "fixed" else
                                f"curriculum: {args.up_frac:.2f} near upright "
                                f"(pi + U(-0.05,0.05)), rest near down")),
                     eval="theta=[0.013,-0.008,0.021], omega=0 (fixed, no noise)")
    meta = dict(algo="ppo", exp=exp, seed=args.seed, envs=args.envs,
                horizon=args.horizon, obs=spec, obs_dim=obs_dim,
                init=init_meta, reward=reward_w, reward_preset=args.reward,
                reward_note=TORCH_REWARD_NOTE, args=vars(args))
    writer.add_text("config", json.dumps(meta, indent=2), 0)
    print(json.dumps(meta, indent=2), flush=True)

    ac = ActorCritic(obs_dim, act_dim=2, hidden=args.hidden, depth=args.depth)
    opt = torch.optim.Adam(ac.parameters(), lr=args.lr, eps=1e-5)
    sched = torch.optim.lr_scheduler.LinearLR(opt, start_factor=1.0,
                                              end_factor=0.2, total_iters=100000)

    th, om = make_init(args.envs, args.init, gen, up_frac=args.up_frac)
    theta = sim.reset(th, om)
    task.reset(theta)

    t_start = time.time()
    global_steps = 0
    it = 0
    best_t_can = -1.0
    best_eval = None
    best_path = None
    last_path = None
    eval_curve = []
    last = {}
    ep_stats = {k: [] for k in ("return", "t_can", "ever_up", "first_up",
                                "up_frac", "length")}

    def save_ckpt(path, ev):
        torch.save(dict(model=ac.state_dict(), opt=opt.state_dict(),
                        obs_dim=obs_dim, obs=spec, init=init_meta,
                        reward=reward_w, reward_preset=args.reward,
                        args=vars(args),
                        iter=it, global_steps=global_steps, eval=ev,
                        algo="ppo"), os.path.join(args.ckpt_dir, path))

    def log_eval(ev, tag=""):
        nonlocal best_t_can, best_eval, best_path
        eval_curve.append(dict(iter=it, global_steps=global_steps, **ev))
        for k, v in ev.items():
            if isinstance(v, (int, float)):
                writer.add_scalar(f"eval/{k}", v, global_steps)
        if (ev["t_can"] > best_t_can
                or (ev["t_can"] == best_t_can and best_eval is not None
                    and ev["ep_return"] > best_eval["ep_return"])):
            best_t_can = ev["t_can"]
            best_eval = dict(ev)
            best_path = exp + "_best.pt"
            save_ckpt(best_path, ev)

    while it < args.iters:
        if (time.time() - t_start) / 60.0 > args.minutes:
            print("time budget reached", flush=True)
            break
        data = collect(sim, task, ac, args.horizon, args, gen, ep_stats)
        adv, ret = gae(data["rew"], data["val"], data["done"], data["last_v"],
                       args.gamma, args.lam)
        data["adv"], data["ret"] = adv, ret
        stats = ppo_update(ac, opt, data, args)
        sched.step()
        global_steps += args.horizon * args.envs
        it += 1
        last = stats
        mean_train_ret = (float(np.mean(ep_stats["return"]))
                          if ep_stats["return"] else float("nan"))

        if ep_stats["return"]:
            writer.add_scalar("train/episode_return",
                              float(np.mean(ep_stats["return"])), global_steps)
            writer.add_scalar("train/episode_length",
                              float(np.mean(ep_stats["length"])), global_steps)
            writer.add_scalar("train/t_can", float(np.mean(ep_stats["t_can"])),
                              global_steps)
            writer.add_scalar("train/t_can_max", float(np.max(ep_stats["t_can"])),
                              global_steps)
            writer.add_scalar("train/success_rate",
                              float(np.mean(np.asarray(ep_stats["t_can"]) >= 10.0)),
                              global_steps)
            writer.add_scalar("train/ever_up_frac",
                              float(np.mean(ep_stats["ever_up"])), global_steps)
            writer.add_scalar("train/up_frac",
                              float(np.mean(ep_stats["up_frac"])), global_steps)
            fu = [x for x in ep_stats["first_up"] if x > 0]
            if fu:
                writer.add_scalar("train/first_up_time", float(np.mean(fu)),
                                  global_steps)
            ep_stats = {k: [] for k in ep_stats}
        writer.add_scalar("train/mean_step_reward", float(data["rew"].mean()), global_steps)
        writer.add_scalar("train/action_abs_mean", float(data["act"].abs().mean()), global_steps)
        writer.add_scalar("train/action_std", float(data["act"].std()), global_steps)
        writer.add_scalar("train/std", float(ac.effective_log_std().exp().mean()),
                          global_steps)
        writer.add_scalar("train/lr", opt.param_groups[0]["lr"], global_steps)
        writer.add_scalar("train/approx_kl", stats["kl"], global_steps)
        writer.add_scalar("train/clip_frac", stats["cf"], global_steps)
        writer.add_scalar("train/global_steps", global_steps, global_steps)
        writer.add_scalar("loss/policy", stats["pg"], global_steps)
        writer.add_scalar("loss/value", stats["v"], global_steps)
        writer.add_scalar("loss/entropy", stats["ent"], global_steps)
        writer.add_scalar("time/elapsed_min", (time.time() - t_start) / 60.0,
                          global_steps)

        if it % args.eval_every == 0 or it == 1:
            ev = evaluate(make_ppo_policy(ac.actor), p, args.episode_steps,
                          reward_w=reward_w, obs_mode=args.obs)
            log_eval(ev)
            last_path = exp + "_last.pt"
            save_ckpt(last_path, ev)
            print(f"iter {it:4d} steps {global_steps/1e6:5.2f}M  "
                  f"train_ret {mean_train_ret:8.1f} "
                  f"EVAL tc {ev['t_can']:6.2f} succ {ev['success']:.0f} "
                  f"everup {ev['ever_up']:.0f} angerr {ev['final_ang_err']:.3f} "
                  f"ret {ev['ep_return']:8.1f}  pi {stats['pg']:+.3f} "
                  f"v {stats['v']:.2f} ent {stats['ent']:.2f} kl {stats['kl']:.3f} "
                  f"({time.time()-t_start:.0f}s)", flush=True)
        elif it % args.log_every == 0:
            print(f"iter {it:4d} steps {global_steps/1e6:5.2f}M  "
                  f"train_ret {mean_train_ret:8.1f} "
                  f"pi {stats['pg']:+.3f} v {stats['v']:.2f} ent {stats['ent']:.2f} "
                  f"kl {stats['kl']:.3f} ({time.time()-t_start:.0f}s)", flush=True)
        writer.flush()

    ev_final = evaluate(make_ppo_policy(ac.actor), p, args.episode_steps,
                        reward_w=reward_w, obs_mode=args.obs)
    eval_curve.append(dict(iter=it, global_steps=global_steps, **ev_final))
    for k, v in ev_final.items():
        if isinstance(v, (int, float)):
            writer.add_scalar(f"eval_final/{k}", v, global_steps)
    final_path = exp + "_final.pt"
    save_ckpt(final_path, ev_final)
    last_path = exp + "_last.pt"
    save_ckpt(last_path, ev_final)

    # Extra deterministic rollouts from the training reset distribution.  This
    # is NOT the mandated fixed-state eval; it checks generalisation.
    rob = None
    rob_best = None
    if args.robust_starts > 0:
        g = torch.Generator().manual_seed(args.seed * 1000 + 123)
        th_r, om_r = sample_train_init(args.robust_starts, gen=g)
        starts = list(zip(th_r.numpy(), om_r.numpy()))
        rob = evaluate_starts(make_ppo_policy(ac.actor), p, starts,
                              args.episode_steps, reward_w=reward_w,
                              obs_mode=args.obs)
        for k, v in rob.items():
            if isinstance(v, (int, float)):
                writer.add_scalar(f"robust/{k}", v, global_steps)
        if best_path is not None:
            ck = torch.load(os.path.join(args.ckpt_dir, best_path),
                            map_location="cpu")
            ac_best = ActorCritic(obs_dim, act_dim=2, hidden=args.hidden,
                                  depth=args.depth)
            ac_best.load_state_dict(ck["model"])
            rob_best = evaluate_starts(make_ppo_policy(ac_best.actor), p, starts,
                                       args.episode_steps, reward_w=reward_w,
                                       obs_mode=args.obs)
            for k, v in rob_best.items():
                if isinstance(v, (int, float)):
                    writer.add_scalar(f"robust_best/{k}", v, global_steps)

    wall = time.time() - t_start
    out = dict(algo="ppo", exp=exp, seed=args.seed, envs=args.envs,
               horizon=args.horizon, obs=spec, obs_dim=obs_dim,
               init=init_meta, reward=reward_w, reward_preset=args.reward,
               reward_note=TORCH_REWARD_NOTE, args=vars(args),
               minutes_budget=args.minutes, global_steps=global_steps,
               iters=it, wall_time_s=wall,
               steps_per_s=global_steps / max(wall, 1e-9),
               final_eval=ev_final, best_eval=best_eval,
               best_t_can=best_t_can, eval_curve=eval_curve,
               robust=rob, robust_best=rob_best,
               final_train=dict(policy_loss=last.get("pg"),
                                value_loss=last.get("v"),
                                entropy=last.get("ent"),
                                approx_kl=last.get("kl")),
               checkpoints=dict(best=best_path, last=last_path,
                                final=final_path))
    with open(os.path.join(args.out, exp + ".json"), "w") as f:
        json.dump(out, f, indent=2)
    writer.flush()
    writer.close()
    print("DONE " + json.dumps(dict(
        exp=exp, init=args.init, seed=args.seed, global_steps=global_steps,
        wall_s=round(wall, 1), steps_per_s=round(out["steps_per_s"], 1),
        eval_t_can=ev_final["t_can"], best_t_can=best_t_can,
        eval_return=ev_final["ep_return"])), flush=True)


if __name__ == "__main__":
    main()