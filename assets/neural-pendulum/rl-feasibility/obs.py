"""Policy-side angle observation for the 3-link pendulum (switchable modes).

The simulator only exposes the three joint angles theta.  The observation is
built entirely from that angle stream (no omega, no tip, no energy):

    a_i(t)  = wrap_to_pi(theta_i(t) - pi)                  (upright-relative)
    da_i(t) = wrap_to_pi(a_i(t) - a_i(t-1)) / DT,          da_i(0) = 0 at reset

    frame(t) = [cos a1, sin a1, cos a2, sin a2, cos a3, sin a3,
                da1/5, da2/5, da3/5]                        (9 dims, diff=True)
    frame(t) = [cos a1, sin a1, cos a2, sin a2, cos a3, sin a3]
                                                            (6 dims, diff=False)

    obs(t)   = H frames, oldest first, flattened

Modes (--obs):
    full      H=4, diff=True   -> 36 dims  (the original observation)
    angle     H=4, diff=False  -> 24 dims  (cos/sin only, no manual velocity)
    full_h1   H=1, diff=True   ->  9 dims  (single frame incl. da/5)
    angle_h1  H=1, diff=False  ->  6 dims  (single frame cos/sin only)
    full_h2   H=2, diff=True   -> 18 dims  (extra mode, optional)
    angle_h2  H=2, diff=False  -> 12 dims  (extra mode, optional)

Even when diff=False the builder still computes da internally, because the
trainer-side reward uses it; the channel is only left out of the policy
observation.  theta is used in radians exactly as produced by the simulator
(never divided by pi).  The history is initialised by repeating the reset
frame H times.
"""
from __future__ import annotations

import numpy as np

DT = 0.01
H = 4
VEL_SCALE = 5.0

# mode -> (history length, whether the manual-difference velocity is observed)
OBS_MODES = {
    "full": dict(h=4, diff=True),
    "angle": dict(h=4, diff=False),
    "full_h1": dict(h=1, diff=True),
    "angle_h1": dict(h=1, diff=False),
    "full_h2": dict(h=2, diff=True),
    "angle_h2": dict(h=2, diff=False),
    "angle_h8": dict(h=8, diff=False),   # extra: can longer history replace da?
}


def frame_dim(diff: bool) -> int:
    return 9 if diff else 6


def obs_spec(mode: str = "full") -> dict:
    if mode not in OBS_MODES:
        raise ValueError(f"unknown obs mode {mode!r}; choose from {sorted(OBS_MODES)}")
    c = OBS_MODES[mode]
    fd = frame_dim(c["diff"])
    notes = ("cos/sin(a) + da/5, H=%d, a=wrap(theta-pi), da is the manual "
             "finite difference, da(0)=0" % c["h"])
    if not c["diff"]:
        notes = ("cos/sin(a) only (NO velocity channel), H=%d, "
                 "a=wrap(theta-pi)" % c["h"])
    return dict(mode=mode, enc="sincos", diff=bool(c["diff"]), h=int(c["h"]),
                vel_scale=VEL_SCALE, dt=DT, frame_dim=fd,
                dim=int(c["h"] * fd), notes=notes)


# Backwards-compatible constants for the original full observation.
FRAME_DIM = frame_dim(True)
OBS_DIM = obs_spec("full")["dim"]


def wrap_to_pi(a):
    return (a + np.pi) % (2.0 * np.pi) - np.pi


def frame_np(a, da, diff=True):
    a = np.asarray(a, dtype=np.float64)
    cs = np.empty(6, dtype=np.float64)
    cs[0::2] = np.cos(a)
    cs[1::2] = np.sin(a)
    if not diff:
        return cs
    return np.concatenate([cs, np.asarray(da, dtype=np.float64) / VEL_SCALE])


class NumpyAngleObs:
    """Float64 single-env builder.  reset/update take theta only."""

    def __init__(self, h: int = H, vel_scale: float = VEL_SCALE, dt: float = DT,
                 diff: bool = True):
        self.h = int(h)
        self.diff = bool(diff)
        self.fdim = frame_dim(self.diff)
        self.vel_scale = float(vel_scale)
        self.dt = float(dt)
        self.a_prev = np.zeros(3)
        self.da = np.zeros(3)
        self.frames = np.zeros((self.h, self.fdim))
        self._obs = np.zeros(self.h * self.fdim, dtype=np.float32)

    def _frame(self, a, da):
        cs = np.empty(6, dtype=np.float64)
        cs[0::2] = np.cos(a)
        cs[1::2] = np.sin(a)
        if not self.diff:
            return cs
        return np.concatenate([cs, da / self.vel_scale])

    def _flatten(self):
        self._obs = self.frames.reshape(-1).astype(np.float32)
        return self._obs

    def reset(self, theta):
        a = wrap_to_pi(np.asarray(theta, dtype=np.float64).reshape(3) - np.pi)
        self.a_prev = a.copy()
        self.da = np.zeros(3)
        f0 = self._frame(a, self.da)
        self.frames = np.tile(f0, (self.h, 1))
        return self._flatten()

    @property
    def obs(self):
        return self._obs

    def update(self, theta):
        a = wrap_to_pi(np.asarray(theta, dtype=np.float64).reshape(3) - np.pi)
        self.da = wrap_to_pi(a - self.a_prev) / self.dt
        self.a_prev = a.copy()
        f = self._frame(a, self.da)
        if self.h > 1:
            self.frames = np.roll(self.frames, -1, axis=0)
        self.frames[-1] = f
        return self._flatten()


class TorchAngleObs:
    """Batched float32 builder.  reset/update take theta only."""

    def __init__(self, n: int, h: int = H, vel_scale: float = VEL_SCALE,
                 dt: float = DT, device="cpu", diff: bool = True):
        import torch
        self.torch = torch
        self.n = int(n)
        self.h = int(h)
        self.diff = bool(diff)
        self.fdim = frame_dim(self.diff)
        self.vel_scale = float(vel_scale)
        self.dt = float(dt)
        self.device = device
        self.a_prev = torch.zeros(n, 3, device=device)
        self.da = torch.zeros(n, 3, device=device)
        self.frames = torch.zeros(n, self.h, self.fdim, device=device)

    def _frame(self, a, da):
        t = self.torch
        cs = t.stack([t.cos(a), t.sin(a)], dim=2).reshape(a.shape[0], 6)
        if not self.diff:
            return cs
        return t.cat([cs, da / self.vel_scale], dim=1)

    def obs(self):
        return self.frames.reshape(self.n, -1)

    def reset(self, theta, idx=None):
        t = self.torch
        theta = t.as_tensor(theta, dtype=t.float32, device=self.device).reshape(-1, 3)
        if idx is None:
            a = t.remainder(theta - np.pi + np.pi, 2 * np.pi) - np.pi
            self.a_prev = a.clone()
            self.da = t.zeros_like(a)
            f0 = self._frame(a, self.da)
            self.frames = f0.unsqueeze(1).expand(-1, self.h, -1).clone()
            return self.obs()
        idx = t.as_tensor(idx, dtype=t.long, device=self.device).flatten()
        a = t.remainder(theta - np.pi + np.pi, 2 * np.pi) - np.pi
        self.a_prev[idx] = a
        self.da[idx] = 0.0
        f0 = self._frame(a, t.zeros_like(a))
        self.frames[idx] = f0.unsqueeze(1).expand(-1, self.h, -1)
        return self.obs()

    def update(self, theta):
        t = self.torch
        theta = t.as_tensor(theta, dtype=t.float32, device=self.device).reshape(-1, 3)
        a = t.remainder(theta - np.pi + np.pi, 2 * np.pi) - np.pi
        self.da = (t.remainder(a - self.a_prev + np.pi, 2 * np.pi) - np.pi) / self.dt
        self.a_prev = a.clone()
        f = self._frame(a, self.da)
        if self.h > 1:
            self.frames = t.cat([self.frames[:, 1:], f.unsqueeze(1)], dim=1)
        else:
            self.frames[:, -1] = f
        return self.obs()


def make_numpy_obs(mode: str = "full", **kw):
    c = OBS_MODES[mode]
    return NumpyAngleObs(h=c["h"], diff=c["diff"], **kw)


def make_torch_obs(n: int, mode: str = "full", **kw):
    c = OBS_MODES[mode]
    return TorchAngleObs(n, h=c["h"], diff=c["diff"], **kw)
