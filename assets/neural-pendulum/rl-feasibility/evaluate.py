"""Deterministic evaluation at the fixed initial state (and optional starts).

The evaluation uses the simulator API exactly like training (angles only), the
same observation builder / obs mode, and the same trainer-side reward.  There
is no action noise.

    default fixed state: theta = [0.013, -0.008, 0.021] rad, omega = 0
    optional starts    : drawn from the training reset distribution

The episode always runs the full 3000 steps so that t_can, ever_up, return and
the final angle error are comparable across algorithms.
"""
from __future__ import annotations

import numpy as np
import torch

from obs import DT, make_numpy_obs, wrap_to_pi
from sim import N_ACT, N_LINKS, UP_TOL, PendulumSim
from task import EVAL_OMEGA0, EVAL_THETA0, NumpyTask


def evaluate(policy_fn, p, episode_steps=3000, reward_w=None, obs_mode="full"):
    """Run one full deterministic episode at the fixed initial state.

    policy_fn(obs: np.ndarray[obs_dim]) -> np.ndarray[2]
    """
    sim = PendulumSim(p, episode_steps=episode_steps)
    task = NumpyTask(p, reward_w=reward_w, obs_mode=obs_mode)
    theta = sim.reset(EVAL_THETA0, EVAL_OMEGA0)
    obs = task.reset(theta)
    acts = np.zeros((episode_steps, N_ACT), dtype=np.float64)
    for k in range(episode_steps):
        u = np.clip(np.asarray(policy_fn(obs), dtype=np.float64).reshape(N_ACT),
                    -1.0, 1.0)
        acts[k] = u
        theta = sim.step(u)
        obs, _ = task.step(theta, u)
    summ = task.tracker.summary()
    a = wrap_to_pi(np.asarray(theta, dtype=np.float64) - np.pi)
    return dict(t_can=float(summ["t_can"]),
                success=float(summ["t_can"] >= 10.0),
                ever_up=float(summ["ever_up"]),
                first_up_t=float(summ["first_up_t"]),
                ep_return=float(summ["ep_return"]),
                final_ang_err=float(np.abs(a).max()),
                up_frac=float(summ["up_frac"]),
                mean_abs_action=float(np.mean(np.abs(acts))),
                action_std=float(np.std(acts)),
                mean_u1=float(acts[:, 0].mean()),
                mean_u3=float(acts[:, 1].mean()),
                steps=int(episode_steps))


def evaluate_starts(policy_fn, p, starts, episode_steps=3000, reward_w=None,
                    obs_mode="full"):
    """Deterministic rollouts from an explicit list of (theta, omega) starts."""
    rows = []
    for theta0, omega0 in starts:
        sim = PendulumSim(p, episode_steps=episode_steps)
        task = NumpyTask(p, reward_w=reward_w, obs_mode=obs_mode)
        theta = sim.reset(theta0, omega0)
        obs = task.reset(theta)
        for _ in range(episode_steps):
            u = np.clip(np.asarray(policy_fn(obs), dtype=np.float64).reshape(N_ACT),
                        -1.0, 1.0)
            theta = sim.step(u)
            obs, _ = task.step(theta, u)
        summ = task.tracker.summary()
        a = wrap_to_pi(np.asarray(theta, dtype=np.float64) - np.pi)
        rows.append(dict(t_can=float(summ["t_can"]),
                         success=float(summ["t_can"] >= 10.0),
                         ever_up=float(summ["ever_up"]),
                         ep_return=float(summ["ep_return"]),
                         final_ang_err=float(np.abs(a).max())))
    tc = np.array([r["t_can"] for r in rows], dtype=np.float64)
    su = np.array([r["success"] for r in rows], dtype=np.float64)
    re = np.array([r["ep_return"] for r in rows], dtype=np.float64)
    ae = np.array([r["final_ang_err"] for r in rows], dtype=np.float64)
    eu = np.array([r["ever_up"] for r in rows], dtype=np.float64)
    return dict(n=int(len(rows)),
                success_rate=float(su.mean()) if len(rows) else 0.0,
                ever_up_rate=float(eu.mean()) if len(rows) else 0.0,
                mean_t_can=float(tc.mean()) if len(rows) else 0.0,
                median_t_can=float(np.median(tc)) if len(rows) else 0.0,
                min_t_can=float(tc.min()) if len(rows) else 0.0,
                max_t_can=float(tc.max()) if len(rows) else 0.0,
                mean_return=float(re.mean()) if len(rows) else 0.0,
                mean_final_ang_err=float(ae.mean()) if len(rows) else 0.0,
                rows=rows)


def make_ppo_policy(actor):
    def policy(obs):
        with torch.no_grad():
            x = torch.as_tensor(obs, dtype=torch.float32).unsqueeze(0)
            return actor(x).squeeze(0).numpy()
    return policy


def make_sac_policy(actor):
    def policy(obs):
        with torch.no_grad():
            x = torch.as_tensor(obs, dtype=torch.float32).unsqueeze(0)
            a, _ = actor.sample(x, det=True)
            return a.squeeze(0).numpy()
    return policy
