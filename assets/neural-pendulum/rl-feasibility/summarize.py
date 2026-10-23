"""Aggregate results/*.json and tb/<exp> into results/summary.json,
results/table.md and results/curves.png.  Missing runs simply do not appear.

Usage: python3 summarize.py [--tb-root tb] [--out results]
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parent
REQUIRED_TAGS = ["train/episode_return", "train/episode_length", "train/t_can",
                 "train/success_rate", "eval/t_can", "eval/success",
                 "eval/final_ang_err", "time/elapsed_min"]


def tb_status(tb_root, exp):
    from tensorboard.backend.event_processing.event_accumulator import (
        EventAccumulator)
    d = Path(tb_root) / exp
    if not d.is_dir():
        return dict(exp=exp, exists=False, tags=[], missing=REQUIRED_TAGS,
                    last=dict())
    ea = EventAccumulator(str(d), size_guidance={"scalars": 0})
    ea.Reload()
    tags = ea.Tags().get("scalars", [])
    last = {}
    for k in ("train/global_steps", "time/elapsed_min", "train/t_can",
              "train/success_rate", "eval/t_can", "eval/success"):
        if k in tags:
            last[k] = ea.Scalars(k)[-1].value
    return dict(exp=exp, exists=True, tags=sorted(tags),
                missing=[t for t in REQUIRED_TAGS if t not in tags],
                n_points=len(tags), last=last)


def load_results(res_dir):
    out = []
    for path in sorted(Path(res_dir).glob("*.json")):
        if path.name in ("summary.json", "tb_coverage.json", "robustness.json",
                          "reference_controller.json"):
            continue
        with open(path) as f:
            out.append(json.load(f))
    return out


def fmt(x, nd=2):
    if x is None:
        return "-"
    if isinstance(x, float):
        if not np.isfinite(x):
            return "nan"
        return f"{x:.{nd}f}"
    return str(x)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tb-root", default="tb")
    ap.add_argument("--out", default="results")
    args = ap.parse_args()
    results = load_results(args.out)
    rows, cov = [], []
    for r in results:
        ev = r.get("final_eval") or {}
        best = r.get("best_eval") or {}
        ck = r.get("checkpoints") or {}
        obs = r.get("obs") or {}
        init = r.get("init") or {}
        init_mode = init.get("mode") if isinstance(init, dict) else str(init)
        curve = r.get("eval_curve") or []
        first_succ = next((c["global_steps"] for c in curve
                           if c.get("t_can", 0.0) >= 10.0), None)
        rob = r.get("robust") or {}
        rob_best = r.get("robust_best") or {}
        c = tb_status(args.tb_root, r["exp"])
        cov.append(c)
        rows.append(dict(
            exp=r["exp"], algo=r.get("algo"), init=init_mode,
            obs=obs.get("mode", "-"), obs_dim=r.get("obs_dim"),
            reward=r.get("reward_preset", "-"),
            envs=r.get("envs"), global_steps=r.get("global_steps"),
            wall_min=(r.get("wall_time_s") or 0.0) / 60.0,
            steps_per_s=r.get("steps_per_s"),
            final_t_can=ev.get("t_can"), best_t_can=best.get("t_can"),
            final_success=ev.get("success"), final_ever_up=ev.get("ever_up"),
            final_return=ev.get("ep_return"), best_return=best.get("ep_return"),
            final_ang_err=ev.get("final_ang_err"),
            first_success_steps=first_succ,
            robust_success=rob.get("success_rate"),
            robust_mean_t_can=rob.get("mean_t_can"),
            robust_best_success=rob_best.get("success_rate"),
            robust_best_mean_t_can=rob_best.get("mean_t_can"),
            best_ckpt=ck.get("best"), final_ckpt=ck.get("final"),
            tb_tags=len(c.get("tags", [])), tb_missing=c.get("missing", []),
        ))

    table = ["# Fixed-env experiment table (auto-generated)",
             "",
             "| exp | algo | init | obs | dim | reward | steps | wall min | "
             "steps/s | final t_can | best t_can | final succ | final ever_up | "
             "final ret | final err | first succ @ steps | final robust succ | "
             "final robust t_can | best robust succ | best robust t_can |",
             "|" + "---|" * 20]
    for x in rows:
        fs = (f"{x['first_success_steps']/1e6:.2f}M"
              if x["first_success_steps"] else "-")
        table.append(
            f"| {x['exp']} | {x['algo']} | {x['init']} | {x['obs']} | "
            f"{x['obs_dim']} | {x['reward']} | "
            f"{(x['global_steps'] or 0)/1e6:.2f}M | {x['wall_min']:.1f} | "
            f"{fmt(x['steps_per_s'],0)} | {fmt(x['final_t_can'])} | "
            f"{fmt(x['best_t_can'])} | {fmt(x['final_success'],0)} | "
            f"{fmt(x['final_ever_up'],0)} | {fmt(x['final_return'],1)} | "
            f"{fmt(x['final_ang_err'],4)} | {fs} | "
            f"{fmt(x['robust_success'],2)} | {fmt(x['robust_mean_t_can'])} | "
            f"{fmt(x['robust_best_success'],2)} | "
            f"{fmt(x['robust_best_mean_t_can'])} |")
    table += ["", "## TensorBoard coverage", "",
              "| exp | tags | required missing | last global_steps | last elapsed min |",
              "|---|---|---|---|---|"]
    for c in cov:
        last = c.get("last") or {}
        table.append(f"| {c['exp']} | {len(c.get('tags', []))} | "
                     f"{', '.join(c.get('missing', [])) or 'none'} | "
                     f"{fmt(last.get('train/global_steps'),0)} | "
                     f"{fmt(last.get('time/elapsed_min'))} |")
    Path(args.out, "table.md").write_text("\n".join(table) + "\n")

    if results:
        fig, axes = plt.subplots(1, 2, figsize=(13, 4.6))
        for r in results:
            curve = r.get("eval_curve") or []
            if not curve:
                continue
            steps = [c["global_steps"] / 1e6 for c in curve]
            tcan = [c["t_can"] for c in curve]
            ret = [c["ep_return"] for c in curve]
            lab = r["exp"]
            axes[0].plot(steps, tcan, lw=1.2, label=lab)
            axes[1].plot(steps, ret, lw=1.2, label=lab)
        axes[0].axhline(10.0, color="k", ls=":", lw=1)
        axes[0].set_xlabel("env steps (M)")
        axes[0].set_ylabel("eval t_can (s)")
        axes[0].set_title("fixed-initial deterministic eval: t_can")
        axes[0].grid(alpha=0.3)
        axes[1].set_xlabel("env steps (M)")
        axes[1].set_ylabel("eval return (symlog)")
        axes[1].set_title("fixed-initial deterministic eval: return")
        axes[1].set_yscale("symlog", linthresh=1000.0)
        axes[1].grid(alpha=0.3, which="both")
        for ax in axes:
            ax.legend(fontsize=6, ncol=1)
        fig.tight_layout()
        fig.savefig(Path(args.out, "curves.png"), dpi=130)
        plt.close(fig)

    summary = dict(n_runs=len(rows), runs=rows, tb_coverage=cov)
    with open(Path(args.out, "summary.json"), "w") as f:
        json.dump(summary, f, indent=2)
    print(json.dumps(dict(n_runs=len(rows),
                          runs=[(x["exp"], x["final_t_can"], x["best_t_can"])
                                for x in rows]), indent=2))


if __name__ == "__main__":
    main()
