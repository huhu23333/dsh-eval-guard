"""Reconstruct results/<exp>.json for a finished run from its checkpoints,
TensorBoard event file and log.  Useful when the trainer crashed after saving
checkpoints but before writing the JSON (e.g. the PPO robustness bug).

Usage:
  python3 finalize_run.py --exp ppo_full_base_rand --algo ppo \
      --obs full --reward base --init rand --envs 64 --minutes 45

The fixed-state eval and the 8-start robustness check are re-run here with the
same code as the trainers, so the numbers are real, not copied from the log.
The eval curve is parsed from logs/<exp>.log and the wall time from TB.
"""
from __future__ import annotations

import argparse
import json
import os
import re
from pathlib import Path

import torch

from evaluate import (evaluate, evaluate_starts, make_ppo_policy,
                      make_sac_policy)
from models import ActorCritic, SquashedGaussianActor
from obs import obs_spec
from sim import EPISODE_STEPS, Params
from task import get_reward_preset, sample_train_init


def build_policy(ck, algo, obs_dim):
    a = ck.get("args") or {}
    hidden = int(a.get("hidden", 128 if algo == "ppo" else 256))
    depth = int(a.get("depth", 2))
    if algo == "ppo":
        net = ActorCritic(obs_dim, act_dim=2, hidden=hidden, depth=depth)
        net.load_state_dict(ck["model"])
        return make_ppo_policy(net.actor), net
    net = SquashedGaussianActor(obs_dim, 2, hidden=hidden, depth=depth)
    net.load_state_dict(ck["model"])
    return make_sac_policy(net), net


def parse_curve(log_path):
    rows = []
    if not os.path.exists(log_path):
        return rows
    pat = re.compile(
        r"iter\s+(\d+)\s+steps\s+([\d.]+)M.*?EVAL tc\s+([\d.]+)\s+"
        r"succ\s+(\d+)\s+everup\s+(\d+)\s+angerr\s+([\d.]+)\s+"
        r"ret\s+([-\d.]+)")
    pat_sac = re.compile(
        r"steps\s+([\d.]+)k\s+upd\s+(\d+).*?EVAL tc\s+([\d.]+)\s+"
        r"succ\s+(\d+)\s+everup\s+(\d+)\s+angerr\s+([\d.]+)\s+"
        r"ret\s+([-\d.]+)")
    for line in open(log_path, errors="ignore"):
        m = pat.search(line)
        if m:
            rows.append(dict(iter=int(m.group(1)),
                             global_steps=int(float(m.group(2)) * 1e6),
                             t_can=float(m.group(3)),
                             success=float(m.group(4)),
                             ever_up=float(m.group(5)),
                             final_ang_err=float(m.group(6)),
                             ep_return=float(m.group(7))))
            continue
        m = pat_sac.search(line)
        if m:
            rows.append(dict(global_steps=int(float(m.group(1)) * 1e3),
                             n_updates=int(m.group(2)),
                             t_can=float(m.group(3)),
                             success=float(m.group(4)),
                             ever_up=float(m.group(5)),
                             final_ang_err=float(m.group(6)),
                             ep_return=float(m.group(7))))
    return rows


def tb_last(tb_root, exp, tag):
    from tensorboard.backend.event_processing.event_accumulator import (
        EventAccumulator)
    d = Path(tb_root) / exp
    if not d.is_dir():
        return None
    ea = EventAccumulator(str(d), size_guidance={"scalars": 0})
    ea.Reload()
    if tag not in ea.Tags().get("scalars", []):
        return None
    return ea.Scalars(tag)[-1].value


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exp", required=True)
    ap.add_argument("--algo", required=True, choices=["ppo", "sac"])
    ap.add_argument("--obs", required=True)
    ap.add_argument("--reward", required=True)
    ap.add_argument("--init", required=True)
    ap.add_argument("--envs", type=int, required=True)
    ap.add_argument("--minutes", type=float, required=True)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--ckpt-dir", default="checkpoints")
    ap.add_argument("--tb-root", default="tb")
    ap.add_argument("--out", default="results")
    ap.add_argument("--log-dir", default="logs")
    ap.add_argument("--robust-starts", type=int, default=8)
    args = ap.parse_args()

    ck_final = torch.load(os.path.join(args.ckpt_dir, args.exp + "_final.pt"),
                          map_location="cpu")
    ck_best = torch.load(os.path.join(args.ckpt_dir, args.exp + "_best.pt"),
                         map_location="cpu")
    obs_dim = ck_final["obs_dim"]
    spec = ck_final.get("obs") or obs_spec(args.obs)
    reward_w = ck_final.get("reward") or get_reward_preset(args.reward)
    p = Params()
    pol_final, _ = build_policy(ck_final, args.algo, obs_dim)
    pol_best, _ = build_policy(ck_best, args.algo, obs_dim)
    ev_final = evaluate(pol_final, p, EPISODE_STEPS, reward_w=reward_w,
                        obs_mode=args.obs)
    ev_best = evaluate(pol_best, p, EPISODE_STEPS, reward_w=reward_w,
                       obs_mode=args.obs)

    g = torch.Generator().manual_seed(args.seed * 1000 + 123)
    th, om = sample_train_init(args.robust_starts, gen=g)
    starts = list(zip(th.numpy(), om.numpy()))
    rob = evaluate_starts(pol_final, p, starts, EPISODE_STEPS,
                          reward_w=reward_w, obs_mode=args.obs)
    rob_best = evaluate_starts(pol_best, p, starts, EPISODE_STEPS,
                               reward_w=reward_w, obs_mode=args.obs)

    curve = parse_curve(os.path.join(args.log_dir, args.exp + ".log"))
    elapsed = tb_last(args.tb_root, args.exp, "time/elapsed_min")
    wall = float(elapsed) * 60.0 if elapsed is not None else 0.0
    steps = ck_final.get("global_steps")
    if not steps:
        steps = tb_last(args.tb_root, args.exp, "train/global_steps")
    out = dict(algo=args.algo, exp=args.exp, seed=args.seed,
               envs=args.envs, obs=spec, obs_dim=obs_dim,
               init=ck_final.get("init"), reward=reward_w,
               reward_preset=args.reward, args=ck_final.get("args"),
               minutes_budget=args.minutes, global_steps=int(steps or 0),
               wall_time_s=wall,
               steps_per_s=(float(steps) / wall) if wall > 0 else None,
               final_eval=ev_final, best_eval=ev_best,
               best_t_can=ev_best["t_can"], eval_curve=curve,
               robust=rob, robust_best=rob_best,
               checkpoints=dict(best=args.exp + "_best.pt",
                                last=args.exp + "_last.pt",
                                final=args.exp + "_final.pt"),
               reconstructed=True,
               reconstruct_note=("results JSON reconstructed by finalize_run.py "
                                 "after the trainer crashed post-checkpoint; "
                                 "fixed eval and robustness were re-run."))
    with open(os.path.join(args.out, args.exp + ".json"), "w") as f:
        json.dump(out, f, indent=2)
    print(json.dumps(dict(exp=args.exp, steps=out["global_steps"],
                          wall_min=round(wall / 60.0, 1),
                          final_t_can=ev_final["t_can"],
                          best_t_can=ev_best["t_can"],
                          final_success=ev_final["success"],
                          best_success=ev_best["success"],
                          robust_final=rob["success_rate"],
                          robust_best=rob_best["success_rate"]), indent=2))


if __name__ == "__main__":
    main()
