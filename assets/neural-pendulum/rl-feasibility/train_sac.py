"""SAC + 2x256 tanh MLP + sin/cos H=4 observation, angle-only simulator.

Experiments:
  --init rand   : theta_i ~ U(-.05,.05), omega_i ~ U(-.02,.02), no bias
  --init fixed  : training reset is exactly theta=[0.013,-0.008,0.021], omega=0

32 vectorised environments by default, replay buffer, auto entropy tuning,
shared-trunk twin Q, UTD = 0.25 (8 gradient steps per 32-env vector step).
The simulator exposes angles only; observation, reward, termination, success
and t_can all live in trainer-side code (obs.py / task.py).

TensorBoard scalars (tb/<exp>/):
  train/episode_return, train/episode_length, train/t_can, train/t_can_max,
  train/success_rate, train/ever_up_frac, train/first_up_time, train/up_frac,
  train/action_abs_mean, train/replay_size, train/steps_per_s, train/alpha,
  train/entropy, train/q_mean, train/q_target_mean,
  loss/actor, loss/critic, loss/alpha, loss/policy, loss/value, loss/entropy,
  eval/t_can, eval/success, eval/ep_return, eval/ever_up, eval/final_ang_err,
  eval/first_up_t, eval/up_frac, eval_final/*, time/elapsed_min
"""
from __future__ import annotations

import argparse
import copy
import json
import os
import time

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F
from torch.utils.tensorboard import SummaryWriter

torch.set_num_threads(int(os.environ.get("TORCH_THREADS", "4")))

from evaluate import evaluate, evaluate_starts, make_sac_policy
from models import SquashedGaussianActor, TwinQ
from obs import OBS_MODES, obs_spec
from sim import EPISODE_STEPS, Params, TorchVecSim
from task import (TORCH_REWARD_NOTE, TorchTask, get_reward_preset, make_init,
                  sample_train_init)


class Replay:
    def __init__(self, cap, dim, act_dim=2):
        self.cap = int(cap)
        self.obs = np.zeros((cap, dim), np.float32)
        self.act = np.zeros((cap, act_dim), np.float32)
        self.rew = np.zeros((cap, 1), np.float32)
        self.nobs = np.zeros((cap, dim), np.float32)
        self.done = np.zeros((cap, 1), np.float32)
        self.ptr = 0
        self.size = 0

    def push(self, obs, act, rew, nobs, done):
        n = obs.shape[0]
        idx = (self.ptr + np.arange(n)) % self.cap
        self.obs[idx] = obs
        self.act[idx] = act
        self.rew[idx] = np.asarray(rew).reshape(n, 1)
        self.nobs[idx] = nobs
        self.done[idx] = np.asarray(done).reshape(n, 1)
        self.ptr = (self.ptr + n) % self.cap
        self.size = min(self.cap, self.size + n)

    def sample(self, batch):
        idx = np.random.randint(0, self.size, size=batch)
        return (torch.from_numpy(self.obs[idx]),
                torch.from_numpy(self.act[idx]),
                torch.from_numpy(self.rew[idx]),
                torch.from_numpy(self.nobs[idx]),
                torch.from_numpy(self.done[idx]))


def soft_update(tgt, src, tau):
    with torch.no_grad():
        for a, b in zip(tgt.parameters(), src.parameters()):
            a.mul_(1 - tau).add_(tau * b)


def sac_update(actor, q, qt, act_opt, q_opt, log_alpha, alpha_opt, replay, args):
    o, ac, r, no, d = replay.sample(args.batch)
    with torch.no_grad():
        na, nlogp = actor.sample(no)
        q_next = qt(no, na).min(dim=1).values
        target = r.squeeze(-1) + args.gamma * (1.0 - d.squeeze(-1)) * (
            q_next - log_alpha.exp() * nlogp)
    q_pred = q(o, ac)
    critic_loss = 0.5 * F.mse_loss(q_pred, target.unsqueeze(1).expand(-1, 2))
    q_opt.zero_grad()
    critic_loss.backward()
    nn.utils.clip_grad_norm_(q.parameters(), 10.0)
    q_opt.step()

    for p_ in q.parameters():
        p_.requires_grad_(False)
    pa, logp = actor.sample(o)
    q_pi = q(o, pa).min(dim=1).values
    actor_loss = (log_alpha.exp().detach() * logp - q_pi).mean()
    act_opt.zero_grad()
    actor_loss.backward()
    nn.utils.clip_grad_norm_(actor.parameters(), 10.0)
    act_opt.step()
    for p_ in q.parameters():
        p_.requires_grad_(True)

    alpha_loss = -(log_alpha.exp() * (logp.detach() + args.target_entropy)).mean()
    alpha_opt.zero_grad()
    alpha_loss.backward()
    alpha_opt.step()

    soft_update(qt, q, args.tau)
    return dict(actor=float(actor_loss), critic=float(critic_loss),
                alpha=float(alpha_loss),
                q=float(q_pred.mean()), qt=float(target.mean()),
                entropy=float(-logp.detach().mean()))


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
    ap.add_argument("--envs", type=int, default=32)
    ap.add_argument("--minutes", type=float, default=30.0)
    ap.add_argument("--steps", type=int, default=10 ** 12)
    ap.add_argument("--hidden", type=int, default=256)
    ap.add_argument("--depth", type=int, default=2)
    ap.add_argument("--lr", type=float, default=3e-4)
    ap.add_argument("--gamma", type=float, default=0.99)
    ap.add_argument("--tau", type=float, default=0.005)
    ap.add_argument("--batch", type=int, default=256)
    ap.add_argument("--warmup", type=int, default=5000)
    ap.add_argument("--buffer", type=int, default=300000)
    ap.add_argument("--utd", type=float, default=0.25,
                    help="gradient steps per environment step")
    ap.add_argument("--target-entropy", type=float, default=-2.0)
    ap.add_argument("--alpha-init", type=float, default=0.2)
    ap.add_argument("--episode-steps", type=int, default=EPISODE_STEPS)
    ap.add_argument("--eval-every", type=int, default=20000)
    ap.add_argument("--eval-start", type=int, default=20000)
    ap.add_argument("--tb-root", type=str, default="tb")
    ap.add_argument("--ckpt-dir", type=str, default="checkpoints")
    ap.add_argument("--out", type=str, default="results")
    ap.add_argument("--tau-max", type=float, default=18.0)
    ap.add_argument("--damping", type=float, default=0.01)
    ap.add_argument("--log-every", type=int, default=10,
                    help="log every N vector steps")
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
    gen = torch.Generator().manual_seed(args.seed * 1000 + 11)
    updates_per_vec = max(1, int(round(args.utd * args.envs)))
    eff_utd = updates_per_vec / args.envs

    init_meta = dict(mode=args.init,
                     training=("theta_i ~ U(-0.05,0.05), omega_i ~ U(-0.02,0.02); no bias"
                               if args.init == "rand" else
                               ("theta=[0.013,-0.008,0.021], omega=0 (same as eval)"
                                if args.init == "fixed" else
                                f"curriculum: {args.up_frac:.2f} near upright "
                                f"(pi + U(-0.05,0.05)), rest near down")),
                     eval="theta=[0.013,-0.008,0.021], omega=0 (fixed, no noise)")
    meta = dict(algo="sac", exp=exp, seed=args.seed, envs=args.envs,
                obs=spec, obs_dim=obs_dim, init=init_meta,
                reward=reward_w, reward_preset=args.reward,
                reward_note=TORCH_REWARD_NOTE,
                updates_per_vector=updates_per_vec, effective_utd=eff_utd,
                args=vars(args))
    writer.add_text("config", json.dumps(meta, indent=2), 0)
    print(json.dumps(meta, indent=2), flush=True)

    actor = SquashedGaussianActor(obs_dim, 2, hidden=args.hidden, depth=args.depth)
    q = TwinQ(obs_dim, 2, hidden=args.hidden, depth=args.depth)
    qt = copy.deepcopy(q)
    act_opt = torch.optim.Adam(actor.parameters(), lr=args.lr)
    q_opt = torch.optim.Adam(q.parameters(), lr=args.lr)
    log_alpha = torch.log(torch.tensor(args.alpha_init)).clone().detach().requires_grad_(True)
    alpha_opt = torch.optim.Adam([log_alpha], lr=args.lr)

    replay = Replay(args.buffer, obs_dim, 2)
    th, om = make_init(args.envs, args.init, gen, up_frac=args.up_frac)
    theta = sim.reset(th, om)
    obs = task.reset(theta)

    t_start = time.time()
    total = 0
    n_updates = 0
    vec_steps = 0
    next_eval = args.eval_start
    best_t_can = -1.0
    best_eval = None
    best_path = None
    last_path = None
    eval_curve = []
    last = dict(actor=float("nan"), critic=float("nan"), alpha=float("nan"),
                q=float("nan"), qt=float("nan"), entropy=float("nan"))
    ep = {k: [] for k in ("return", "t_can", "ever_up", "first_up", "up_frac", "length")}

    def save_ckpt(path, ev):
        torch.save(dict(model=actor.state_dict(), q=q.state_dict(),
                        log_alpha=log_alpha.detach(), obs=spec,
                        obs_dim=obs_dim, init=init_meta, reward=reward_w,
                        reward_preset=args.reward,
                        args=vars(args), global_steps=total,
                        n_updates=n_updates, eval=ev, algo="sac"),
                   os.path.join(args.ckpt_dir, path))

    def log_eval(ev):
        nonlocal best_t_can, best_eval, best_path, last_path
        eval_curve.append(dict(global_steps=total, n_updates=n_updates, **ev))
        for k, v in ev.items():
            if isinstance(v, (int, float)):
                writer.add_scalar(f"eval/{k}", v, total)
        if (ev["t_can"] > best_t_can
                or (ev["t_can"] == best_t_can and best_eval is not None
                    and ev["ep_return"] > best_eval["ep_return"])):
            best_t_can = ev["t_can"]
            best_eval = dict(ev)
            best_path = exp + "_best.pt"
            save_ckpt(best_path, ev)
        last_path = exp + "_last.pt"
        save_ckpt(last_path, ev)

    while total < args.steps:
        if (time.time() - t_start) / 60.0 > args.minutes:
            print("time budget reached", flush=True)
            break
        if replay.size < args.warmup:
            u = torch.rand(args.envs, 2) * 2.0 - 1.0
        else:
            with torch.no_grad():
                u, _ = actor.sample(obs)
            u = torch.clamp(u, -1.0, 1.0)
        theta = sim.step(u)
        nobs, rew = task.step(theta, u)
        done = sim.steps >= sim.episode_steps
        replay.push(obs.detach().cpu().numpy(), u.detach().cpu().numpy(),
                    rew.detach().cpu().numpy(), nobs.detach().cpu().numpy(),
                    done.detach().cpu().numpy())
        if bool(done.any()):
            idx = done.nonzero().flatten()
            summ = task.summary(idx)
            ep["return"] += summ["ep_return"].tolist()
            ep["t_can"] += summ["t_can"].tolist()
            ep["ever_up"] += summ["ever_up"].tolist()
            ep["first_up"] += summ["first_up_t"].tolist()
            ep["up_frac"] += summ["up_frac"].tolist()
            ep["length"] += summ["length"].tolist()
            th, om = make_init(idx.numel(), args.init, gen, up_frac=args.up_frac)
            reset_theta = sim.reset(th, om, idx)
            task.reset(reset_theta, idx)
            nobs = task.obs
        obs = nobs
        total += args.envs
        vec_steps += 1

        if replay.size >= args.warmup:
            for _ in range(updates_per_vec):
                last = sac_update(actor, q, qt, act_opt, q_opt, log_alpha,
                                  alpha_opt, replay, args)
                n_updates += 1

        if vec_steps % args.log_every == 0:
            elapsed = time.time() - t_start
            if ep["return"]:
                writer.add_scalar("train/episode_return",
                                  float(np.mean(ep["return"])), total)
                writer.add_scalar("train/episode_length",
                                  float(np.mean(ep["length"])), total)
                writer.add_scalar("train/t_can", float(np.mean(ep["t_can"])), total)
                writer.add_scalar("train/t_can_max", float(np.max(ep["t_can"])), total)
                writer.add_scalar("train/success_rate",
                                  float(np.mean(np.asarray(ep["t_can"]) >= 10.0)), total)
                writer.add_scalar("train/ever_up_frac",
                                  float(np.mean(ep["ever_up"])), total)
                writer.add_scalar("train/up_frac",
                                  float(np.mean(ep["up_frac"])), total)
                fu = [x for x in ep["first_up"] if x > 0]
                if fu:
                    writer.add_scalar("train/first_up_time", float(np.mean(fu)), total)
                ep = {k: [] for k in ep}
            writer.add_scalar("train/replay_size", replay.size, total)
            writer.add_scalar("train/steps_per_s", total / max(elapsed, 1e-9), total)
            writer.add_scalar("train/alpha", float(log_alpha.detach().exp()), total)
            writer.add_scalar("train/action_abs_mean", float(u.abs().mean()), total)
            writer.add_scalar("train/action_std", float(u.std()), total)
            writer.add_scalar("loss/actor", last["actor"], total)
            writer.add_scalar("loss/critic", last["critic"], total)
            writer.add_scalar("loss/alpha", last["alpha"], total)
            writer.add_scalar("loss/policy", last["actor"], total)
            writer.add_scalar("loss/value", last["critic"], total)
            writer.add_scalar("loss/entropy", last["entropy"], total)
            writer.add_scalar("train/entropy", last["entropy"], total)
            writer.add_scalar("train/q_mean", last["q"], total)
            writer.add_scalar("train/q_target_mean", last["qt"], total)
            writer.add_scalar("train/global_steps", total, total)
            writer.add_scalar("time/elapsed_min", elapsed / 60.0, total)
            writer.flush()

        if total >= next_eval:
            next_eval += args.eval_every
            ev = evaluate(make_sac_policy(actor), p, args.episode_steps,
                          reward_w=reward_w, obs_mode=args.obs)
            log_eval(ev)
            print(f"steps {total/1e3:7.1f}k upd {n_updates:6d}  "
                  f"EVAL tc {ev['t_can']:6.2f} succ {ev['success']:.0f} "
                  f"everup {ev['ever_up']:.0f} angerr {ev['final_ang_err']:.3f} "
                  f"ret {ev['ep_return']:8.1f}  q {last['q']:8.2f} "
                  f"a {float(log_alpha.detach().exp()):.2f} "
                  f"({time.time()-t_start:.0f}s)", flush=True)
            writer.flush()

    ev_final = evaluate(make_sac_policy(actor), p, args.episode_steps,
                        reward_w=reward_w, obs_mode=args.obs)
    eval_curve.append(dict(global_steps=total, n_updates=n_updates, **ev_final))
    for k, v in ev_final.items():
        if isinstance(v, (int, float)):
            writer.add_scalar(f"eval_final/{k}", v, total)
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
        rob = evaluate_starts(make_sac_policy(actor), p, starts,
                              args.episode_steps, reward_w=reward_w,
                              obs_mode=args.obs)
        for k, v in rob.items():
            if isinstance(v, (int, float)):
                writer.add_scalar(f"robust/{k}", v, total)
        if best_path is not None:
            ck = torch.load(os.path.join(args.ckpt_dir, best_path),
                            map_location="cpu")
            actor_best = SquashedGaussianActor(obs_dim, 2, hidden=args.hidden,
                                               depth=args.depth)
            actor_best.load_state_dict(ck["model"])
            rob_best = evaluate_starts(make_sac_policy(actor_best), p, starts,
                                       args.episode_steps, reward_w=reward_w,
                                       obs_mode=args.obs)
            for k, v in rob_best.items():
                if isinstance(v, (int, float)):
                    writer.add_scalar(f"robust_best/{k}", v, total)

    wall = time.time() - t_start
    out = dict(algo="sac", exp=exp, seed=args.seed, envs=args.envs,
               obs=spec, obs_dim=obs_dim, init=init_meta,
               reward=reward_w, reward_preset=args.reward,
               reward_note=TORCH_REWARD_NOTE,
               args=vars(args), updates_per_vector=updates_per_vec,
               effective_utd=eff_utd, minutes_budget=args.minutes,
               global_steps=total, gradient_steps=n_updates,
               wall_time_s=wall, steps_per_s=total / max(wall, 1e-9),
               updates_per_s=n_updates / max(wall, 1e-9),
               final_eval=ev_final, best_eval=best_eval, best_t_can=best_t_can,
               eval_curve=eval_curve, robust=rob, robust_best=rob_best,
               final_train=last,
               checkpoints=dict(best=best_path, last=last_path,
                                final=final_path))
    with open(os.path.join(args.out, exp + ".json"), "w") as f:
        json.dump(out, f, indent=2)
    writer.flush()
    writer.close()
    print("DONE " + json.dumps(dict(
        exp=exp, init=args.init, seed=args.seed, envs=args.envs,
        global_steps=total, gradient_steps=n_updates, wall_s=round(wall, 1),
        steps_per_s=round(out["steps_per_s"], 1),
        eval_t_can=ev_final["t_can"], best_t_can=best_t_can,
        eval_return=ev_final["ep_return"])), flush=True)


if __name__ == "__main__":
    main()