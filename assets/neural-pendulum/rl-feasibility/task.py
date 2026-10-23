"""Trainer-side task wrapper: observation, reward, episode bookkeeping.

Nothing in this file is part of the simulator API.  The simulator returns only
the three angles; this module turns that angle stream into

  * the policy observation (obs.py),
  * the training reward,
  * success / t_can / ever_up / episode-return bookkeeping,
  * reset initial-state distributions.

Reward (entirely constructed here, never in the environment):
    a      = wrap_to_pi(theta - pi)
    s2     = sum_i a_i^2
    upf    = 1[max_i |a_i| < 0.15]
    tip_y  = -sum_i l_i cos(theta_i)                  (function of theta)
    E      ~ 0.5 da^T M(theta) da - g sum_i gvec_i cos(theta_i)
             (da is the manual finite-difference velocity from obs.py)
    r = exp(-2 s2) + 2 exp(-50 s2) - 0.05 s2
        - 0.001 sum da^2 - 0.01 sum u^2 + 3 upf
        + 0.3 (tip_y + L)/(2L) - 0.01 |E - E_up|/(E_up - E_down)

The last two shaping terms are functions of the angle stream (and da), not
privileged simulator outputs.  Because the simulator exposes no omega, da is
used wherever the reference reward used omega; this is documented in
RESULTS.md.

Reset distributions:
  training random : theta_i ~ U(-0.05, 0.05), omega_i ~ U(-0.02, 0.02)
                    (no fixed bias of any kind)
  training fixed  : theta = [0.013, -0.008, 0.021], omega = 0
  evaluation      : theta = [0.013, -0.008, 0.021], omega = 0 (no noise)
"""
from __future__ import annotations

import numpy as np
import torch

from obs import (DT, H, NumpyAngleObs, OBS_MODES, TorchAngleObs,
                 make_numpy_obs, make_torch_obs, obs_spec, wrap_to_pi)
from sim import (BONUS_TOL, N_ACT, N_LINKS, Params, UP_TOL, energy_down,
                 energy_up, make_torch_consts, tip_pos, torch_tip, total_energy)

TRAIN_THETA_HALF = 0.05
TRAIN_OMEGA_HALF = 0.02
EVAL_THETA0 = np.array([0.013, -0.008, 0.021], dtype=np.float64)
EVAL_OMEGA0 = np.zeros(N_LINKS, dtype=np.float64)

DEFAULT_W = dict(exp=1.0, sharp=2.0, ang=0.05, vel=0.001, act=0.01, up=3.0,
                 tip=0.3, energy=0.01)

# Reward presets.  Every term is a trainer-side function of theta, the manual
# difference da and the action; the keys map 1:1 onto the reward formula in
# reward_np / TorchReward.  No simulator omega is read.
#
# base   : the original dense shaping above.
# sparse : up-focused, almost no dense shaping:
#            r = 0.2 exp(-2 s2) + 3.0 1[max|a| < 0.15]
#                - 0.001 sum da^2 - 0.01 sum u^2
#          (no angle-progress, tip or energy terms)
# energy : strong energy shaping toward the upright energy, weaker angle and
#          velocity penalties:
#            r = 0.2 exp(-2 s2) + 0.5 exp(-50 s2) - 0.01 s2
#                - 0.0002 sum da^2 - 0.005 sum u^2
#                + 3.0 1[max|a| < 0.15]
#                - 0.5 |E - E_up| / (E_up - E_down)
#          E is the trainer-side mechanical energy using da as velocity.
SPARSE_W = dict(exp=0.2, sharp=0.0, ang=0.0, vel=0.001, act=0.01, up=3.0,
                tip=0.0, energy=0.0)
ENERGY_W = dict(exp=1.0, sharp=2.0, ang=0.05, vel=0.0, act=0.005, up=3.0,
                tip=0.0, energy=0.5)

REWARD_PRESETS = {
    "base": dict(DEFAULT_W),
    "sparse": dict(SPARSE_W),
    "energy": dict(ENERGY_W),
}


# The energy error is |E-E_up|/span.  At high spin rates E grows without bound,
# and with weight 0.5 (energy preset) the raw term produced huge negative
# rewards and value-loss blow-up (~5e7).  Clamp it: the gradient near upright
# is unchanged, but violent spins cannot dominate the return.
E_ERR_CLAMP = 3.0


def get_reward_preset(name: str) -> dict:
    if name not in REWARD_PRESETS:
        raise ValueError(f"unknown reward preset {name!r}; "
                         f"choose from {sorted(REWARD_PRESETS)}")
    return dict(REWARD_PRESETS[name])

OBS_SPEC = obs_spec("full")

TORCH_REWARD_NOTE = (
    "Reward is built in task.py from theta and the manual difference da only. "
    "The simulator returns angles only (no reward/done/omega/tip). tip_y and "
    "the energy-like term are trainer-side functions of theta (and da), not "
    "simulator outputs.")


def reward_np(theta, da, u, p: Params, w=None):
    w = DEFAULT_W if w is None else w
    theta = np.asarray(theta, dtype=np.float64).reshape(N_LINKS)
    da = np.asarray(da, dtype=np.float64).reshape(N_LINKS)
    u = np.asarray(u, dtype=np.float64).reshape(N_ACT)
    a = wrap_to_pi(theta - np.pi)
    s2 = float(a @ a)
    upf = float(np.abs(a).max() < BONUS_TOL)
    y = tip_pos(theta, p)[1]
    L = p.L_total
    e = total_energy(theta, da, p)[0]
    e_err = min(E_ERR_CLAMP,
                abs(e - energy_up(p)) / max(1e-6, energy_up(p) - energy_down(p)))
    return (w["exp"] * np.exp(-2.0 * s2)
            + w["sharp"] * np.exp(-50.0 * s2)
            - w["ang"] * s2
            - w["vel"] * float(da @ da)
            - w["act"] * float(u @ u)
            + w["up"] * upf
            + w["tip"] * (y + L) / (2.0 * L)
            - w["energy"] * e_err)


class TorchReward:
    def __init__(self, p: Params, w=None, device="cpu"):
        self.p = p
        self.w = dict(DEFAULT_W if w is None else w)
        self.c = make_torch_consts(p, device)
        self.e_up = energy_up(p)
        self.e_span = max(1e-6, energy_up(p) - energy_down(p))
        self.L = p.L_total

    def __call__(self, theta, da, u):
        t = torch
        a = t.remainder(theta - np.pi + np.pi, 2 * np.pi) - np.pi
        s2 = (a * a).sum(1)
        upf = (a.abs().amax(1) < BONUS_TOL).float()
        y = torch_tip(theta, self.c)[:, 1]
        M = self.c["A"] * t.cos(theta.unsqueeze(2) - theta.unsqueeze(1))
        E = 0.5 * (da * t.linalg.solve(M, da.unsqueeze(-1)).squeeze(-1)).sum(1)
        E = E - self.c["g"] * (t.cos(theta) * self.c["vcoef"]).sum(1)
        e_err = ((E - self.e_up).abs() / self.e_span).clamp(max=E_ERR_CLAMP)
        w = self.w
        return (w["exp"] * t.exp(-2.0 * s2)
                + w["sharp"] * t.exp(-50.0 * s2)
                - w["ang"] * s2
                - w["vel"] * (da * da).sum(1)
                - w["act"] * (u * u).sum(1)
                + w["up"] * upf
                + w["tip"] * (y + self.L) / (2.0 * self.L)
                - w["energy"] * e_err)


class NumpyTracker:
    def __init__(self, up_tol=UP_TOL):
        self.up_tol = up_tol
        self.t = 0.0
        self.ep_return = 0.0
        self.cur_up_start = -1.0
        self.t_can = 0.0
        self.ever_up = False
        self.first_up_t = -1.0
        self.up_steps = 0

    def reset(self):
        self.t = 0.0
        self.ep_return = 0.0
        self.cur_up_start = -1.0
        self.t_can = 0.0
        self.ever_up = False
        self.first_up_t = -1.0
        self.up_steps = 0

    def update(self, theta, r):
        a = wrap_to_pi(np.asarray(theta, dtype=np.float64).reshape(N_LINKS) - np.pi)
        up = bool(np.abs(a).max() < self.up_tol)
        self.t += DT
        self.ep_return += float(r)
        if up and not self.ever_up:
            self.ever_up = True
            self.first_up_t = self.t
        if up:
            self.up_steps += 1
            if self.cur_up_start < 0:
                self.cur_up_start = self.t
            self.t_can = max(self.t_can, self.t - self.cur_up_start)
        else:
            self.cur_up_start = -1.0

    def summary(self):
        return dict(ep_return=float(self.ep_return), t_can=float(self.t_can),
                    ever_up=float(self.ever_up),
                    first_up_t=float(self.first_up_t),
                    up_frac=float(self.up_steps * DT / max(self.t, 1e-6)),
                    length=int(round(self.t / DT)))


class TorchTracker:
    def __init__(self, n, up_tol=UP_TOL, device="cpu"):
        self.torch = torch
        self.n = int(n)
        self.up_tol = up_tol
        self.device = device
        self.t = torch.zeros(n, device=device)
        self.ep_return = torch.zeros(n, device=device)
        self.cur_up_start = torch.full((n,), -1.0, device=device)
        self.t_can = torch.zeros(n, device=device)
        self.ever_up = torch.zeros(n, dtype=torch.bool, device=device)
        self.first_up_t = torch.full((n,), -1.0, device=device)
        self.up_steps = torch.zeros(n, device=device)

    def reset(self, idx=None):
        t = self.torch
        if idx is None:
            idx = t.arange(self.n, device=self.device)
        idx = t.as_tensor(idx, dtype=t.long, device=self.device).flatten()
        self.t[idx] = 0.0
        self.ep_return[idx] = 0.0
        self.cur_up_start[idx] = -1.0
        self.t_can[idx] = 0.0
        self.ever_up[idx] = False
        self.first_up_t[idx] = -1.0
        self.up_steps[idx] = 0.0

    def update(self, theta, r):
        t = self.torch
        a = t.remainder(theta - np.pi + np.pi, 2 * np.pi) - np.pi
        up = a.abs().amax(1) < self.up_tol
        self.t = self.t + DT
        self.ep_return = self.ep_return + r
        newly = up & (~self.ever_up)
        self.ever_up = self.ever_up | up
        self.first_up_t = t.where(newly, self.t, self.first_up_t)
        self.up_steps = self.up_steps + up.float()
        start = t.where(up & (self.cur_up_start < 0), self.t, self.cur_up_start)
        self.cur_up_start = t.where(up, start, t.full_like(start, -1.0))
        streak = t.where(self.cur_up_start >= 0, self.t - self.cur_up_start,
                         t.zeros_like(self.t))
        self.t_can = t.maximum(self.t_can, streak)

    def summary(self, idx):
        t = self.torch
        idx = t.as_tensor(idx, dtype=t.long, device=self.device).flatten()
        return dict(ep_return=self.ep_return[idx].clone(),
                    t_can=self.t_can[idx].clone(),
                    ever_up=self.ever_up[idx].float().clone(),
                    first_up_t=self.first_up_t[idx].clone(),
                    up_frac=self.up_steps[idx] * DT / self.t[idx].clamp(min=1e-6),
                    length=(self.t[idx] / DT).round().long())


def sample_train_init(n, gen=None, device="cpu"):
    """Pure uniform noise, no fixed bias: theta U(-.05,.05), omega U(-.02,.02)."""
    t = torch
    theta = (t.rand(n, N_LINKS, generator=gen, device=device) * 2.0 - 1.0) * TRAIN_THETA_HALF
    omega = (t.rand(n, N_LINKS, generator=gen, device=device) * 2.0 - 1.0) * TRAIN_OMEGA_HALF
    return theta, omega


def sample_upright_init(n, gen=None, device="cpu"):
    """Curriculum-only: small perturbation around upright (theta = pi)."""
    t = torch
    theta = np.pi + (t.rand(n, N_LINKS, generator=gen, device=device) * 2.0 - 1.0) * TRAIN_THETA_HALF
    omega = (t.rand(n, N_LINKS, generator=gen, device=device) * 2.0 - 1.0) * TRAIN_OMEGA_HALF
    return theta, omega


def fixed_init(n, device="cpu"):
    theta = torch.tensor(EVAL_THETA0, dtype=torch.float32, device=device).repeat(n, 1)
    omega = torch.zeros(n, N_LINKS, dtype=torch.float32, device=device)
    return theta, omega


def make_init(n, mode="rand", gen=None, device="cpu", up_frac=0.5):
    if mode == "rand":
        return sample_train_init(n, gen=gen, device=device)
    if mode == "fixed":
        return fixed_init(n, device=device)
    if mode == "mix":
        # Curriculum: a fraction up_frac of resets start near upright so the
        # balance phase is actually visited; the rest start near down.
        n_up = int(round(float(up_frac) * n))
        n_down = n - n_up
        th_d, om_d = sample_train_init(n_down, gen=gen, device=device)
        th_u, om_u = sample_upright_init(n_up, gen=gen, device=device)
        theta = torch.cat([th_d, th_u], dim=0)
        omega = torch.cat([om_d, om_u], dim=0)
        perm = torch.randperm(n, generator=gen, device=device)
        return theta[perm], omega[perm]
    raise ValueError(mode)


class NumpyTask:
    def __init__(self, p: Params, reward_w=None, obs_mode="full"):
        self.p = p
        self.w = dict(DEFAULT_W if reward_w is None else reward_w)
        self.obs_mode = obs_mode
        self.builder = make_numpy_obs(obs_mode)
        self.tracker = NumpyTracker()

    def reset(self, theta):
        self.tracker.reset()
        return self.builder.reset(theta)

    def step(self, theta, u):
        obs = self.builder.update(theta)
        r = reward_np(theta, self.builder.da, u, self.p, self.w)
        self.tracker.update(theta, r)
        return obs, r

    @property
    def obs(self):
        return self.builder.obs


class TorchTask:
    def __init__(self, p: Params, n: int, reward_w=None, device="cpu",
                 obs_mode="full"):
        self.p = p
        self.n = int(n)
        self.w = dict(DEFAULT_W if reward_w is None else reward_w)
        self.obs_mode = obs_mode
        self.builder = make_torch_obs(n, obs_mode, device=device)
        self.reward = TorchReward(p, self.w, device)
        self.tracker = TorchTracker(n, device=device)

    def reset(self, theta, idx=None):
        self.tracker.reset(idx)
        return self.builder.reset(theta, idx)

    def step(self, theta, u):
        obs = self.builder.update(theta)
        r = self.reward(theta, self.builder.da, u)
        self.tracker.update(theta, r)
        return obs, r

    @property
    def obs(self):
        return self.builder.obs()

    def summary(self, idx):
        return self.tracker.summary(idx)
