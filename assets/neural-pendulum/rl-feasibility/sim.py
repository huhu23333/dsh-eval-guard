"""3-link planar pendulum simulator with a strict angle-only API.

Physical model (verified in the sibling rounds):
  * 3 links, joints 1 and 3 actuated, joint 2 passive.
  * tau_max = 18 N m at each actuated joint, action u in [-1, 1]^2.
  * 100 Hz zero-order-hold control, RK4 with 4 substeps (dt = 2.5 ms).
  * 30 s episode = 3000 control steps.
  * theta_i is the absolute link angle from the DOWNWARD vertical
    (0 = down, +-pi = up); angles are NOT divided by pi anywhere.

Environment contract (deliberate, so that no training signal lives in the
simulator):
  reset(theta, omega) -> theta   (only the three angles)
  step(u)             -> theta   (only the three angles)
  There is no reward, loss, done, info, terminal flag, tip or omega in any
  return value.  A public theta property exists only for diagnostics and
  returns the same three angles.
  An episode is exactly episode_steps (default 3000) control steps.
  Calling step() when any environment in the batch has already used its
  budget raises EpisodeLengthError; reset() must be called first.

The internal omega is advanced by the physics and is deliberately not part
of the public API.  Training code that wants a velocity signal must derive it
from the returned angle stream (see obs.py).
"""
from __future__ import annotations

import numpy as np

N_LINKS = 3
DRIVE_JOINTS = (0, 2)          # zero-based: joint 1 and joint 3 actuated
DT = 0.01                      # control period (100 Hz)
N_ACT = 2
EPISODE_STEPS = 3000           # 30 s
UP_TOL = 0.05                  # success tolerance (rad)
BONUS_TOL = 0.15               # reward bonus tolerance (rad)


class Params:
    """Physical parameters of the 3-link pendulum (defaults = task spec)."""

    def __init__(self, m=(1.0, 1.0, 1.0), l=(0.5, 0.5, 0.5), g=9.81, b=0.01,
                 tau_max=18.0, r=None, I=None, drive=DRIVE_JOINTS):
        self.m = np.asarray(m, dtype=np.float64)
        self.l = np.asarray(l, dtype=np.float64)
        self.g = float(g)
        self.b = float(b)
        self.tau_max = float(tau_max)
        self.drive = tuple(drive)
        self.r = np.asarray(r, dtype=np.float64) if r is not None else 0.5 * self.l
        self.I = (np.asarray(I, dtype=np.float64) if I is not None
                  else self.m * self.l ** 2 / 12.0)

        n = N_LINKS
        self.gvec = np.array([self.m[i] * self.r[i] + self.l[i] * self.m[i + 1:].sum()
                              for i in range(n)])

        self.A = np.zeros((n, n))
        for i in range(n):
            for j in range(n):
                if i == j:
                    self.A[i, i] = (self.I[i] + self.m[i] * self.r[i] ** 2
                                    + self.l[i] ** 2 * self.m[i + 1:].sum())
                else:
                    a, bb = min(i, j), max(i, j)
                    self.A[i, j] = self.l[a] * self.gvec[bb]

        self.vcoef = self.gvec.copy()

        # Rayleigh damping matrix D^T D, eps_i = omega_i - omega_{i-1}
        D = np.zeros((n, n))
        for i in range(n):
            D[i, i] = 1.0
            if i > 0:
                D[i, i - 1] = -1.0
        self.Ldamp = D.T @ D
        self.L_total = float(self.l.sum())

    def tip_position(self, theta):
        return float(np.dot(self.l, np.sin(theta))), float(-np.dot(self.l, np.cos(theta)))

    def potential(self, theta):
        return -self.g * float(np.dot(self.vcoef, np.cos(theta)))

    def as_dict(self):
        return dict(m=self.m.tolist(), l=self.l.tolist(), r=self.r.tolist(),
                    I=self.I.tolist(), g=self.g, b=self.b,
                    tau_max=self.tau_max, drive=list(self.drive))


# ---------------------------------------------------------------------------
# numpy float64 physics
# ---------------------------------------------------------------------------
def generalized_torque(u, p: Params):
    """Physical joint-motor torques mapped to absolute-angle generalized forces.

    Joint 1 is external to ground, so its generalized force is +u1*tau_max.
    Joint 3 is an internal motor between links 2 and 3: its torque acts on the
    relative angle theta3-theta2, giving generalized force
        (tau1, -tau3, tau3)
    with tau2=0.  Do NOT use (tau1, 0, tau3): that would be a reactionless
    external torque on link 3 and contradicts joint 2 being passive.
    """
    tau = np.zeros(N_LINKS)
    u = np.asarray(u, dtype=np.float64)
    # Fixed mapping for this task: drive=(0,2), tau2=0.
    tau[0] = float(u[0]) * p.tau_max
    tau[1] = -float(u[1]) * p.tau_max
    tau[2] = float(u[1]) * p.tau_max
    return tau


def rhs(theta, omega, u, p: Params):
    d = theta[:, None] - theta[None, :]
    M = p.A * np.cos(d)
    C = (p.A * np.sin(d) * (omega[None, :] ** 2)).sum(axis=1)
    G = p.g * np.sin(theta) * p.gvec
    tau = generalized_torque(u, p) - p.b * (p.Ldamp @ omega)
    qdd = np.linalg.solve(M, tau - C - G)
    return omega, qdd


def rk4_step(theta, omega, u, p: Params, dt):
    k1t, k1w = rhs(theta, omega, u, p)
    k2t, k2w = rhs(theta + 0.5 * dt * k1t, omega + 0.5 * dt * k1w, u, p)
    k3t, k3w = rhs(theta + 0.5 * dt * k2t, omega + 0.5 * dt * k2w, u, p)
    k4t, k4w = rhs(theta + dt * k3t, omega + dt * k3w, u, p)
    theta = theta + (dt / 6.0) * (k1t + 2 * k2t + 2 * k3t + k4t)
    omega = omega + (dt / 6.0) * (k1w + 2 * k2w + 2 * k3w + k4w)
    return theta, omega


def integrate(theta, omega, u, p: Params, dt_ctrl=DT, n_sub=4):
    h = dt_ctrl / n_sub
    for _ in range(n_sub):
        theta, omega = rk4_step(theta, omega, u, p, h)
    return theta, omega


def total_energy(theta, omega, p: Params):
    d = theta[:, None] - theta[None, :]
    M = p.A * np.cos(d)
    T = 0.5 * float(omega @ M @ omega)
    V = -p.g * float(np.dot(p.vcoef, np.cos(theta)))
    return T + V, T, V


def energy_up(p: Params):
    return p.g * float(np.dot(p.vcoef, np.ones(N_LINKS)))


def energy_down(p: Params):
    return -p.g * float(np.dot(p.vcoef, np.ones(N_LINKS)))


def wrap_to_pi(a):
    return (a + np.pi) % (2.0 * np.pi) - np.pi


def upright_error(theta):
    return wrap_to_pi(theta - np.pi)


def tip_pos(theta, p: Params):
    return p.tip_position(theta)


# ---------------------------------------------------------------------------
# batched torch float32 physics (training)
# ---------------------------------------------------------------------------
def make_torch_consts(p: Params, device="cpu"):
    import torch
    # u @ tau_map must produce the absolute generalized force (tau1,-tau3,tau3).
    tau_map = torch.zeros(2, N_LINKS, dtype=torch.float32, device=device)
    tau_map[0, 0] = 1.0    # joint 1: +tau1
    tau_map[1, 1] = -1.0   # joint 3 reaction on link 2: -tau3
    tau_map[1, 2] = 1.0    # joint 3 on link 3: +tau3
    return dict(
        A=torch.tensor(p.A, dtype=torch.float32, device=device),
        gvec=torch.tensor(p.gvec, dtype=torch.float32, device=device),
        vcoef=torch.tensor(p.vcoef, dtype=torch.float32, device=device),
        Ldamp=torch.tensor(p.Ldamp, dtype=torch.float32, device=device),
        lt=torch.tensor(p.l, dtype=torch.float32, device=device),
        tau_map=tau_map,
        g=p.g, b=p.b, tau_max=p.tau_max,
    )


def torch_rhs(theta, omega, u, c):
    import torch
    d = theta.unsqueeze(2) - theta.unsqueeze(1)
    M = c["A"] * torch.cos(d)
    C = (c["A"] * torch.sin(d) * omega.unsqueeze(1) ** 2).sum(dim=2)
    G = c["g"] * torch.sin(theta) * c["gvec"]
    tau = u @ c["tau_map"] * c["tau_max"]
    tau = tau - c["b"] * (omega @ c["Ldamp"].t())
    qdd = torch.linalg.solve(M, (tau - C - G).unsqueeze(-1)).squeeze(-1)
    return omega, qdd


def torch_rk4_step(theta, omega, u, c, dt):
    k1t, k1w = torch_rhs(theta, omega, u, c)
    k2t, k2w = torch_rhs(theta + 0.5 * dt * k1t, omega + 0.5 * dt * k1w, u, c)
    k3t, k3w = torch_rhs(theta + 0.5 * dt * k2t, omega + 0.5 * dt * k2w, u, c)
    k4t, k4w = torch_rhs(theta + dt * k3t, omega + dt * k3w, u, c)
    theta = theta + (dt / 6.0) * (k1t + 2 * k2t + 2 * k3t + k4t)
    omega = omega + (dt / 6.0) * (k1w + 2 * k2w + 2 * k3w + k4w)
    return theta, omega


def torch_integrate(theta, omega, u, c, dt_ctrl=DT, n_sub=4):
    h = dt_ctrl / n_sub
    for _ in range(n_sub):
        theta, omega = torch_rk4_step(theta, omega, u, c, h)
    return theta, omega


def torch_tip(theta, c):
    import torch
    x = (torch.sin(theta) * c["lt"]).sum(-1)
    y = -(torch.cos(theta) * c["lt"]).sum(-1)
    return torch.stack([x, y], dim=-1)


# ---------------------------------------------------------------------------
# public simulator API
# ---------------------------------------------------------------------------
class EpisodeLengthError(RuntimeError):
    """Raised when step() is called after the 3000-step budget is used up."""


class PendulumSim:
    """Single float64 simulator.  Returns angles only."""

    def __init__(self, p=None, episode_steps: int = EPISODE_STEPS,
                 dt: float = DT, n_sub: int = 4):
        self.p = Params() if p is None else p
        self.episode_steps = int(episode_steps)
        self.dt = float(dt)
        self.n_sub = int(n_sub)
        self._theta = np.zeros(N_LINKS)
        self._omega = np.zeros(N_LINKS)
        self._steps = 0

    @property
    def steps(self):
        return self._steps

    @property
    def theta(self):
        return self._theta.copy()

    def reset(self, theta, omega):
        theta = np.asarray(theta, dtype=np.float64).reshape(N_LINKS)
        omega = np.asarray(omega, dtype=np.float64).reshape(N_LINKS)
        if not (np.isfinite(theta).all() and np.isfinite(omega).all()):
            raise ValueError("non-finite initial state")
        self._theta = theta.copy()
        self._omega = omega.copy()
        self._steps = 0
        return self._theta.copy()

    def step(self, u):
        if self._steps >= self.episode_steps:
            raise EpisodeLengthError(
                f"episode budget exhausted ({self.episode_steps} steps); call reset()")
        u = np.clip(np.asarray(u, dtype=np.float64).reshape(N_ACT), -1.0, 1.0)
        self._theta, self._omega = integrate(self._theta, self._omega, u,
                                             self.p, self.dt, self.n_sub)
        self._steps += 1
        return self._theta.copy()


class TorchVecSim:
    """Batched float32 simulator.  Returns angles only; no reward/done."""

    def __init__(self, p: Params, n: int, episode_steps: int = EPISODE_STEPS,
                 device="cpu", dt: float = DT, n_sub: int = 4):
        import torch
        self.torch = torch
        self.p = p
        self.n = int(n)
        self.episode_steps = int(episode_steps)
        self.device = device
        self.dt = float(dt)
        self.n_sub = int(n_sub)
        self.c = make_torch_consts(p, device)
        self.theta = torch.zeros(n, N_LINKS, device=device)
        self._omega = torch.zeros(n, N_LINKS, device=device)
        self.steps = torch.zeros(n, dtype=torch.long, device=device)

    def reset(self, theta, omega, idx=None):
        t = self.torch
        theta = t.as_tensor(theta, dtype=t.float32, device=self.device).reshape(-1, N_LINKS)
        omega = t.as_tensor(omega, dtype=t.float32, device=self.device).reshape(-1, N_LINKS)
        if idx is None:
            if theta.shape[0] != self.n:
                raise ValueError(f"expected {self.n} initial states, got {theta.shape[0]}")
            self.theta = theta.clone()
            self._omega = omega.clone()
            self.steps = t.zeros(self.n, dtype=t.long, device=self.device)
            return self.theta.clone()
        idx = t.as_tensor(idx, dtype=t.long, device=self.device).flatten()
        if theta.shape[0] != idx.numel():
            raise ValueError("theta subset size does not match idx")
        self.theta[idx] = theta
        self._omega[idx] = omega
        self.steps[idx] = 0
        return self.theta[idx].clone()

    def step(self, u):
        t = self.torch
        exhausted = self.steps >= self.episode_steps
        if bool(exhausted.any()):
            bad = exhausted.nonzero().flatten().tolist()
            raise EpisodeLengthError(
                "episode budget exhausted for envs "
                f"{bad[:8]}{'...' if len(bad) > 8 else ''} "
                f"({self.episode_steps} steps); call reset() first")
        u = t.clamp(t.as_tensor(u, dtype=t.float32, device=self.device), -1.0, 1.0)
        self.theta, self._omega = torch_integrate(self.theta, self._omega, u,
                                                 self.c, self.dt, self.n_sub)
        self.steps = self.steps + 1
        return self.theta.clone()
