"""Plot selected eval t_can curves (PPO left, SAC right)."""
from __future__ import annotations

import json
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parent
RES = ROOT / "results"

PPO = ["ppo_full_base_rand", "ppo_full_base_rand_s1",
       "ppo_full_base_rand_s2", "ppo_angle_base_rand", "ppo_angle_h8_rand",
       "ppo_full_h1_rand", "ppo_full_h1_mix", "ppo_full_base_fixed"]
SAC = ["sac_full_base_rand", "sac_full_base_fixed", "sac_angle_base_rand"]


def load(exp):
    p = RES / (exp + ".json")
    if not p.exists():
        return None
    with open(p) as f:
        return json.load(f)


def main():
    fig, axes = plt.subplots(1, 2, figsize=(14, 5.0), sharey=True)
    for ax, names, title in [(axes[0], PPO, "PPO"), (axes[1], SAC, "SAC")]:
        for exp in names:
            r = load(exp)
            if not r:
                continue
            c = r.get("eval_curve") or []
            if not c:
                continue
            x = [v["global_steps"] / 1e6 for v in c]
            y = [v["t_can"] for v in c]
            ax.plot(x, y, lw=1.3, label=f"{exp} (best {r['best_t_can']:.2f}s)")
        ax.axhline(10.0, color="k", ls=":", lw=1)
        ax.set_title(f"{title}: fixed-initial eval t_can")
        ax.set_xlabel("environment steps (M)")
        ax.set_ylabel("t_can (s)")
        ax.grid(alpha=0.3)
        ax.legend(fontsize=7)
    fig.tight_layout()
    out = RES / "curves_selected.png"
    fig.savefig(out, dpi=130)
    plt.close(fig)
    print(out)


if __name__ == "__main__":
    main()
