"""Non-RL feasibility check: a pole-placement stabiliser around upright.

This is NOT a learned policy and NOT part of the RL results.  It answers a
single question: is the upright hold physically reachable with the fixed
torque mapping (tau1, -tau3, tau3) and |u| <= 1?

The controller uses privileged full state (theta and the simulator omega),
so it is an upper bound / existence proof only.  It is deliberately simple:
    u = clip(-K [wrap(theta-pi); omega], -1, 1)
with K from pole placement on the linearisation at upright.

Writes results/reference_controller.json.
"""
from __future__ import annotations

import json
from pathlib import Path

import numpy as np
from scipy.signal import place_poles

from sim import EPISODE_STEPS, N_ACT, Params, PendulumSim, make_torch_consts
from task import EVAL_OMEGA0, EVAL_THETA0, NumpyTracker, sample_train_init, wrap_to_pi

ROOT = Path(__file__).resolve().parent


def design(p: Params, poles=(-5.0, -6.0, -7.0, -8.0, -9.0, -10.0)):
    M = p.A
    K = np.diag(p.g * p.gvec)
    Minv = np.linalg.inv(M)
    B = np.zeros((3, 2))
    B[0, 0] = 1.0
    B[1, 1] = -1.0
    B[2, 1] = 1.0
    Be = Minv @ B
    A = np.zeros((6, 6))
    A[:3, 3:] = np.eye(3)
    A[3:, :3] = Minv @ K
    B6 = np.vstack([np.zeros((3, 2)), Be])
    res = place_poles(A, B6, np.asarray(poles, dtype=float))
    return res.gain_matrix


def rollout(p, gain, theta0, omega0, episode_steps=EPISODE_STEPS,
            alpha=0.5):
    sim = PendulumSim(p, episode_steps=episode_steps)
    tr = NumpyTracker()
    theta = sim.reset(theta0, omega0)
    tr.reset()
    sat = 0
    uf = np.zeros(2)
    for _ in range(episode_steps):
        x = np.r_[wrap_to_pi(theta - np.pi), sim._omega]
        raw = np.clip(-gain @ x, -1.0, 1.0)
        # first-order low-pass on the torque command: the unfiltered
        # pole-placement controller chatters at 100 Hz and only holds ~1 s.
        uf = alpha * raw + (1.0 - alpha) * uf
        sat += int(np.abs(uf).max() > 1.0)
        theta = sim.step(uf)
        tr.update(theta, 0.0)
    summ = tr.summary()
    return dict(t_can=float(summ["t_can"]),
                ever_up=float(summ["ever_up"]),
                final_ang_err=float(np.abs(wrap_to_pi(theta - np.pi)).max()),
                sat_frac=float(sat / episode_steps),
                final_theta=theta.tolist())


def main():
    p = Params()
    gain = design(p)
    # This is a LOCAL stabiliser: start near upright (theta = pi + offset).
    fixed_theta0 = np.pi + EVAL_THETA0
    fixed = rollout(p, gain, fixed_theta0, EVAL_OMEGA0)
    rng = np.random.default_rng(0)
    th = np.pi + rng.uniform(-0.05, 0.05, size=(32, 3))
    om = np.zeros((32, 3))
    rows = [rollout(p, gain, th[i], om[i]) for i in range(th.shape[0])]
    tc = np.array([r["t_can"] for r in rows])
    out = dict(
        note=("Non-RL existence check.  Pole-placement stabiliser using "
              "privileged full state (theta and simulator omega); not a "
              "learned policy.  Shows whether the upright hold is physically "
              "reachable with the fixed torque mapping.  The torque command is "
              "low-pass filtered (alpha=0.5); the unfiltered controller "
              "chatters and only holds ~1 s."),
        poles=[-5.0, -6.0, -7.0, -8.0, -9.0, -10.0],
        action_filter_alpha=0.5,
        gain=gain.tolist(),
        fixed_start=dict(theta0=fixed_theta0.tolist(),
                         offset_from_upright=EVAL_THETA0.tolist(),
                         omega0=EVAL_OMEGA0.tolist(), **fixed),
        random_starts=dict(n=len(rows), success_rate=float((tc >= 10.0).mean()),
                           mean_t_can=float(tc.mean()),
                           median_t_can=float(np.median(tc)),
                           max_t_can=float(tc.max()),
                           rows=rows),
    )
    out_path = ROOT / "results" / "reference_controller.json"
    with open(out_path, "w") as f:
        json.dump(out, f, indent=2)
    print(json.dumps(dict(fixed_t_can=fixed["t_can"],
                          fixed_final_err=fixed["final_ang_err"],
                          fixed_sat=fixed["sat_frac"],
                          random_success=out["random_starts"]["success_rate"],
                          random_mean_t_can=out["random_starts"]["mean_t_can"]),
                     indent=2))


if __name__ == "__main__":
    main()
