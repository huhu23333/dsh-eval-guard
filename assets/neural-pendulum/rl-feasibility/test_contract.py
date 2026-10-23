"""Contract checks for the v2 random-init / SAC feasibility round.

Checks, in order:
  1. simulator API returns three angles only -- no reward/loss/done/info;
  2. the 3000-step budget raises EpisodeLengthError and reset() restores it
     (single and batched simulators, including partial resets);
  3. training reset is pure noise U(-.05,.05) / U(-.02,.02) with no bias;
     the fixed control and the evaluation use [0.013,-0.008,0.021], omega=0;
  4. observation definition: a=wrap(theta-pi), da=wrap(a-a_prev)/dt with
     da(0)=0, frame=[cos a, sin a, da/5], H=4, 36 dims, theta not /pi;
  5. the observation builder cannot see omega (signature check) and the torch
     builder matches numpy;
  6. t_can/success bookkeeping holds a streak and resets on leaving upright;
  7. trainer-side reward is finite and the fixed-init evaluation runs.
"""
from __future__ import annotations

import inspect

import numpy as np
import torch

from evaluate import evaluate
from obs import DT, H, NumpyAngleObs, OBS_DIM, TorchAngleObs, wrap_to_pi
from sim import (EPISODE_STEPS, EpisodeLengthError, Params, PendulumSim,
                 TorchVecSim)
from task import (EVAL_THETA0, NumpyTask, TorchTracker, make_init,
                  sample_train_init)

p = Params()


def frame(a, da):
    cs = np.empty(6)
    cs[0::2] = np.cos(a)
    cs[1::2] = np.sin(a)
    return np.concatenate([cs, np.asarray(da) / 5.0])


print("obs_dim", OBS_DIM)
assert OBS_DIM == 36 and H == 4

# --- 1. angle-only API ------------------------------------------------------
sim = PendulumSim(p)
th0 = np.array([0.013, -0.008, 0.021])
out = sim.reset(th0, np.zeros(3))
assert isinstance(out, np.ndarray) and out.shape == (3,)
assert np.allclose(out, th0)
out = sim.step(np.array([0.1, -0.2]))
assert isinstance(out, np.ndarray) and out.shape == (3,)
assert len(inspect.signature(PendulumSim.step).parameters) == 2
assert "reward" not in inspect.getsource(PendulumSim.step).lower()
assert not hasattr(sim, "omega") and not hasattr(sim, "done")
print("single sim step returns only 3 angles; no public omega/done")

# --- 2. episode budget ------------------------------------------------------
short = PendulumSim(p, episode_steps=5)
short.reset(th0, np.zeros(3))
for _ in range(5):
    short.step(np.zeros(2))
try:
    short.step(np.zeros(2))
    raise AssertionError("expected EpisodeLengthError")
except EpisodeLengthError as e:
    print("single sim budget error:", str(e)[:60])
short.reset(th0, np.zeros(3))
short.step(np.zeros(2))

vec = TorchVecSim(p, n=2, episode_steps=5)
assert not hasattr(vec, "omega") and not hasattr(vec, "done")
vec.reset(torch.tensor([th0, th0]), torch.zeros(2, 3))
for _ in range(5):
    vec.step(torch.zeros(2, 2))
try:
    vec.step(torch.zeros(2, 2))
    raise AssertionError("expected batched EpisodeLengthError")
except EpisodeLengthError as e:
    print("vec sim budget error:", str(e)[:70])
# resetting only one env must still raise because env 1 is exhausted
vec.reset(torch.tensor([th0]), torch.zeros(1, 3), idx=torch.tensor([0]))
try:
    vec.step(torch.zeros(2, 2))
    raise AssertionError("expected error while env 1 exhausted")
except EpisodeLengthError:
    pass
vec.reset(torch.tensor([th0, th0]), torch.zeros(2, 3))
vec.step(torch.zeros(2, 2))
print("batched sim budget + partial reset contract OK")

# --- 3. reset distributions -------------------------------------------------
gen = torch.Generator().manual_seed(0)
th, om = sample_train_init(20000, gen)
assert th.abs().max() <= 0.05 + 1e-6 and om.abs().max() <= 0.02 + 1e-6
assert th.abs().max() > 0.049 and om.abs().max() > 0.019
assert th.mean().abs() < 0.002 and om.mean().abs() < 0.001, (th.mean(), om.mean())
print("random init ranges", float(th.min()), float(th.max()),
      float(om.min()), float(om.max()), "means", float(th.mean()), float(om.mean()))

thf, omf = make_init(4, "fixed")
assert np.allclose(thf.numpy(), np.tile(EVAL_THETA0, (4, 1)))
assert np.allclose(omf.numpy(), 0.0)
thf2, omf2 = make_init(4, "rand", gen)
print("fixed init", thf[0].tolist(), "eval", EVAL_THETA0.tolist())

# --- 4. observation definition ---------------------------------------------
th0 = np.array([0.013, -0.008, 0.021])
th1 = np.array([0.063, -0.048, 0.001])
b = NumpyAngleObs()
o0 = b.reset(th0)
a0 = wrap_to_pi(th0 - np.pi)
want0 = np.tile(frame(a0, np.zeros(3)), H)
assert np.allclose(o0, want0, atol=1e-6)
o1 = b.update(th1)
a1 = wrap_to_pi(th1 - np.pi)
da = wrap_to_pi(a1 - a0) / DT
want_last = frame(a1, da)
mat = o1.reshape(H, 9)
assert np.allclose(mat[-1], want_last, atol=1e-6)
assert np.allclose(mat[:-1], np.tile(want0.reshape(H, 9)[0], (H - 1, 1)), atol=1e-6)
# theta is not divided by pi: a=theta-pi, not theta/pi-pi
assert np.allclose(mat[-1, :2], [np.cos(a1[0]), np.sin(a1[0])], atol=1e-6)
assert not np.allclose(mat[-1, :2], [np.cos(th1[0] / np.pi - np.pi),
                                     np.sin(th1[0] / np.pi - np.pi)], atol=1e-3)
# da initialisation is exactly zero
assert np.allclose(o0.reshape(H, 9)[:, 6:], 0.0)
print("obs frame(last)", np.round(mat[-1], 4), "da", np.round(da, 4))

# wrap in the difference: crossing the -pi/pi boundary must not spike
b.reset(np.array([np.pi - 0.01, 0.0, 0.0]))
o = b.update(np.array([-np.pi + 0.01, 0.0, 0.0])).reshape(H, 9)
assert abs(o[-1, 6] - (0.02 / DT) / 5.0) < 1e-5, o[-1, 6]
print("da wrap OK")

# --- 5. no omega in observation API; torch matches numpy --------------------
assert list(inspect.signature(NumpyAngleObs.update).parameters) == ["self", "theta"]
assert list(inspect.signature(NumpyAngleObs.reset).parameters) == ["self", "theta"]
assert list(inspect.signature(TorchAngleObs.update).parameters) == ["self", "theta"]
tb = TorchAngleObs(1)
ot = tb.reset(torch.tensor(th0, dtype=torch.float32).unsqueeze(0))
assert np.allclose(ot.numpy().reshape(-1), o0, atol=1e-5)
ot = tb.update(torch.tensor(th1, dtype=torch.float32).unsqueeze(0))
assert np.allclose(ot.numpy().reshape(H, 9), mat, atol=1e-5)
print("numpy/torch obs builders match; no omega argument")

# --- 6. success / t_can bookkeeping ----------------------------------------
tr = TorchTracker(1)
tr.reset()
up_theta = torch.tensor([[np.pi + 0.01, np.pi - 0.01, np.pi + 0.02]])
for _ in range(1500):
    tr.update(up_theta, torch.zeros(1))
s = tr.summary(torch.tensor([0]))
assert abs(float(s["t_can"]) - 14.99) < 1e-2, float(s["t_can"])
assert abs(float(s["up_frac"]) - 1.0) < 1e-2, float(s["up_frac"])  # all steps up
assert float(s["ever_up"]) == 1.0 and abs(float(s["first_up_t"]) - 0.01) < 1e-3
tr.update(torch.tensor([[0.0, 0.0, 0.0]]), torch.zeros(1))  # leave upright
for _ in range(500):
    tr.update(up_theta, torch.zeros(1))
s = tr.summary(torch.tensor([0]))
assert abs(float(s["t_can"]) - 14.99) < 1e-2, float(s["t_can"])
print("tracker t_can", float(s["t_can"]), "ever_up", float(s["ever_up"]),
      "first_up_t", float(s["first_up_t"]))

# --- 7. trainer reward + fixed-init evaluation ------------------------------
task = NumpyTask(p)
theta = np.array([0.013, -0.008, 0.021])
_ = task.reset(theta)
_, r = task.step(theta, np.array([0.0, 0.0]))
assert np.isfinite(r)
print("initial trainer reward", float(r))

ev = evaluate(lambda obs: np.zeros(2), p, episode_steps=200)
assert ev["steps"] == 200 and np.isfinite(ev["ep_return"])
assert ev["t_can"] == 0.0 and ev["ever_up"] == 0.0
print("fixed-init eval (200 steps)", {k: ev[k] for k in
      ("t_can", "success", "ever_up", "ep_return", "final_ang_err")})

# torch/numpy physics agreement on a short rollout
s1 = PendulumSim(p)
t1 = s1.reset(th0, np.zeros(3))
s2 = TorchVecSim(p, n=1)
s2.reset(torch.tensor(th0, dtype=torch.float32).unsqueeze(0), torch.zeros(1, 3))
u = np.array([0.3, -0.4])
for _ in range(20):
    t1 = s1.step(u)
    t2 = s2.step(torch.tensor(u, dtype=torch.float32).unsqueeze(0)).numpy()[0]
assert np.allclose(t1, t2, atol=1e-4), (t1, t2)
print("numpy/torch physics agree after 20 steps:", np.round(t1, 6), np.round(t2, 6))
print("ALL CONTRACT CHECKS PASSED")
