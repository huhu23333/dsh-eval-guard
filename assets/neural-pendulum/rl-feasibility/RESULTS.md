# RESULTS — fixed-physics 3-link pendulum RL feasibility

Work dir: `feasibility/neural-pendulum-fixedenv`.
Started 2026-10-23 01:13 CST; experiment matrix completed 06:35 CST.
All numbers below are new runs in the corrected environment; no old
`(tau1, 0, tau3)` result is reused.

## TL;DR

* **PPO and SAC both learn the corrected dynamics from the mandated random
  near-down resets and reach t_can >= 10 s at the fixed evaluation state.**
  * PPO (64 env, full 36D, base reward): first success at **11.33M env steps /
    16.0 min**; best **26.85 s**; final 26.61 s; best-checkpoint robustness
    **8/8**.
  * SAC (32 env, full 36D, base reward): first success at **0.70M env steps /
    24.7 min**; best **28.75 s**; final 28.21 s; best-checkpoint robustness
    **6/8** (final 8/8).
* **The learnability boundary is set by observation information, not by the
  algorithm alone.**
  * `full` H=4 (36D: cos/sin + da/5) works for both PPO and SAC.
  * `angle` H=4 (24D, no da): PPO best 0.97 s, SAC best 0.04 s — fails.
  * `full_h1` (9D) and `angle_h1` (6D): fail completely (best <= 0.06 s);
    even a 50% upright curriculum does not rescue H=1.
  * `angle_h8` (48D): best 1.31 s — longer history helps a little but does
    not replace `da/5`.
* **Reward boundary:** base works; sparse and strong energy shaping fail.
  * `sparse`: 19.12M steps, best t_can 0, policy converges to tiny actions.
  * `energy`: two designs fail; energy chasing with manual `da` has a
    degenerate high-kinetic-energy spin optimum.
* **Init boundary is algorithm-dependent.** Random init is required for PPO
  (fixed-init PPO: best 1.19 s, robust 0/8). SAC fixed-init *does* succeed
  (best 28.86 s, robust 8/8) because its replay covers the state space; PPO's
  on-policy batch of 64 identical states does not.
* Three PPO seeds all reach >26 s at their best checkpoint (26.85 / 27.17 /
  28.46 s) but the final checkpoint is unreliable (seed 2 final 0.00 s).
  Use `_best.pt`, not the last model.
* A non-RL pole-placement stabiliser with a 0.5 low-pass torque filter holds
  **29.99 s** from the fixed upright offset and succeeds on 32/32 random
  upright offsets.  The hold is physically feasible with the corrected
  mapping; it is not an RL result (privileged omega).

## 1. Environment and contract

* Simulator returns angles only: `reset(theta,omega)->theta`,
  `step(u)->theta`.  No reward, done, info, omega or tip is exposed.
* Corrected physical torque mapping:
  `generalized_torque = (tau1, -tau3, tau3)` (joint 3 is the internal motor
  between links 2 and 3; joint 2 is passive).  `sim.generalized_torque` and
  `sim.make_torch_consts` both implement it.  Do not revert to
  `(tau1, 0, tau3)`.
* 100 Hz ZOH, RK4 with 4 substeps, 30 s episodes (3000 steps),
  tau_max = 18 N m, damping 0.01, success tolerance 0.05 rad, t_tgt = 10 s.
* `test_contract.py` passes; the Python sim was cross-validated against
  `assets/neural-pendulum/sim_eval` by the Lead (~5e-6).
* Training reset (random arm): theta_i ~ U(-0.05,0.05),
  omega_i ~ U(-0.02,0.02), independent, no fixed bias.
  Fixed arm: theta = [0.013,-0.008,0.021], omega = 0.
  Curriculum arm: 50% near down, 50% theta = pi + U(-0.05,0.05).
* Evaluation: fixed theta = [0.013,-0.008,0.021], omega = 0, deterministic
  actions, full 3000 steps.  t_can is the longest continuous interval with
  max_i |wrap(theta_i - pi)| < 0.05 rad.  Eval always uses the **same** obs
  mode and reward preset as training; no reward switch at eval.

## 2. Code changes in this directory

1. `obs.py`: switchable modes
   `full` 36D, `angle` 24D, `full_h1` 9D, `angle_h1` 6D,
   `full_h2` 18D, `angle_h2` 12D, plus `angle_h8` 48D.
   `da` is always computed internally because the reward uses it;
   `diff=False` only drops it from the policy input.
2. `task.py`: reward presets `base`, `sparse`, `energy` (exact weights
   below), bounded energy error, and `--init mix` curriculum support.
3. `train_ppo.py` / `train_sac.py`: `--obs`, `--reward`, `--init`,
   `--up-frac`; eval uses the same mode/reward; live TB flush every
   iteration/log tick; 8 extra deterministic rollouts from the training
   reset distribution for final and best checkpoints.
4. `evaluate.py`: obs-mode aware, plus `evaluate_starts`.
5. `reference_controller.py`: non-RL local stabilisability proof.
6. `finalize_run.py`: rebuild a results JSON from checkpoints + log when a
   trainer crashes after saving checkpoints (used once for PPO seed 0).
7. `summarize.py`, `plot_selected.py`: aggregate table/JSON/curves.

## 3. Observation modes

| --obs | frame | H | dim | contents |
|---|---|---|---|---|
| full | [cos a_i, sin a_i, da_i/5] | 4 | 36 | baseline |
| angle | [cos a_i, sin a_i] | 4 | 24 | no explicit velocity |
| full_h1 | [cos a_i, sin a_i, da_i/5] | 1 | 9 | single frame |
| angle_h1 | [cos a_i, sin a_i] | 1 | 6 | single frame, no velocity |
| full_h2 | [cos a_i, sin a_i, da_i/5] | 2 | 18 | optional |
| angle_h2 | [cos a_i, sin a_i] | 2 | 12 | optional |
| angle_h8 | [cos a_i, sin a_i] | 8 | 48 | longer history follow-up |

`a_i = wrap_to_pi(theta_i - pi)`, `da_i = wrap_to_pi(a_i(t)-a_i(t-1))/dt`,
`da(0)=0`.  Theta is in radians, never divided by pi.  History is oldest
first, reset repeats the first frame.  The eval builder is the same class and
mode as training.

## 4. Reward presets (exact)

```
r = exp    * exp(-2 s2)
  + sharp  * exp(-50 s2)
  - ang    * s2
  - vel    * sum da^2
  - act    * sum u^2
  + up     * 1[max_i |a_i| < 0.15]
  + tip    * (tip_y + L)/(2L)
  - energy * |E - E_up| / (E_up - E_down)
```

All terms are trainer-side functions of theta, the manual difference da and
the action u; no simulator omega is read.  `E` uses da as velocity.  The
energy error is clamped at 3 to keep the reward bounded at high spin.

```json
base   = {"exp":1.0,"sharp":2.0,"ang":0.05,"vel":0.001,"act":0.01,"up":3.0,"tip":0.3,"energy":0.01}
sparse = {"exp":0.2,"sharp":0.0,"ang":0.0,"vel":0.001,"act":0.01,"up":3.0,"tip":0.0,"energy":0.0}
energy = {"exp":1.0,"sharp":2.0,"ang":0.05,"vel":0.0,"act":0.005,"up":3.0,"tip":0.0,"energy":0.5}
```

`sparse`: at the near-down reset the only dense term is ~1e-26, so it is an
intentionally hard exploration test.  `energy`: base angle terms + strong
energy shaping + no velocity penalty; an earlier energy-dominant design
(`exp=.2, sharp=.5, ang=.01, vel=.0002`) is archived under
`failed/energy_unbounded/`.

## 5. Training protocol

PPO: 2x128 tanh actor/critic, 64 envs, horizon 600, 8 epochs,
minibatch 2048, lr 3e-4 linearly decayed to 20%, gamma .99, lambda .95,
clip .2, entropy .003, vf .5, target KL .03, grad clip .5.

SAC: 2x256 tanh squashed-Gaussian actor, shared-trunk twin Q, 32 envs,
batch 256, replay 300k, auto-alpha (target entropy -2), gamma .99,
tau .005, lr 3e-4, UTD 0.25 (8 gradient updates per 32-env vector step).

Both: CPU PyTorch, 4 threads per process, at most 2 training processes
concurrently.  Eval every 5 PPO iterations (192k steps) or every 20k SAC
steps.  Best checkpoint by eval t_can (ties by return).  Final checkpoint at
the time budget.  TensorBoard flushed during training (not only at the end).
At the end, 8 deterministic rollouts from the training distribution are run
for the final and best checkpoints.

## 6. Complete experiment matrix (fixed-initial deterministic eval)

`best trob` = success rate / mean t_can of the best checkpoint on the 8
training-distribution starts.  `first succ` = first eval snapshot with
t_can >= 10 s.

| exp | algo | obs | reward | init | env steps | wall | best t_can | final t_can | ever_up | best trob | first succ |
|---|---|---|---|---|---|---|---|---|---|---|---|
| ppo_full_base_rand | PPO | full 36 | base | rand | 35.98M | 45.1 min | **26.85 s** | 26.61 s | yes | 8/8, 26.86 s | **11.33M** |
| ppo_full_base_rand_s1 | PPO | full 36 | base | rand | 29.03M | 40.8 min | **27.17 s** | 0.23 s | yes | 8/8, 26.78 s | 13.63M |
| ppo_full_base_rand_s2 | PPO | full 36 | base | rand | 34.21M | 40.3 min | **28.46 s** | 0.00 s | no | 8/8, 28.03 s | 14.40M |
| sac_full_base_rand | SAC | full 36 | base | rand | 1.42M | 50.6 min | **28.75 s** | 28.21 s | yes | 6/8, 20.89 s | **0.70M** |
| ppo_angle_base_rand | PPO | angle 24 | base | rand | 31.76M | 40.6 min | 0.97 s | 0.04 s | yes | 0/8 | - |
| sac_angle_base_rand | SAC | angle 24 | base | rand | 1.42M | 50.6 min | 0.04 s | 0.00 s | no | 0/8 | - |
| ppo_full_h1_rand | PPO | full_h1 9 | base | rand | 25.42M | 35.3 min | 0.06 s | 0.00 s | no | 0/8 | - |
| ppo_angle_h1_rand | PPO | angle_h1 6 | base | rand | 20.97M | 30.4 min | 0.00 s | 0.00 s | no | 0/8 | - |
| ppo_angle_h8_rand | PPO | angle_h8 48 | base | rand | 30.11M | 40.4 min | 1.31 s | 0.05 s | yes | 0/8 | - |
| ppo_full_h1_mix | PPO | full_h1 9 | base | mix 50% up | 21.39M | 30.5 min | 0.02 s | 0.00 s | no | 0/8 | - |
| ppo_angle_mix | PPO | angle 24 | base | mix 50% up | 23.50M | 30.5 min | 0.25 s | 0.00 s | yes | 0/8 | - |
| ppo_full_sparse_rand | PPO | full 36 | sparse | rand | 19.12M | 30.5 min | 0.00 s | 0.00 s | no | 0/8 | - |
| ppo_full_energy_rand | PPO | full 36 | energy | rand | 24.50M | 35.5 min | 0.00 s | 0.00 s | no | 0/8 | - |
| ppo_full_base_fixed | PPO | full 36 | base | fixed | 21.93M | 30.5 min | 1.19 s | 0.00 s | no | 0/8 | - |
| sac_full_base_fixed | SAC | full 36 | base | fixed | 1.41M | 50.4 min | **28.86 s** | 27.62 s | yes | 8/8, 23.45 s | **0.52M** |

Raw machine-readable table: `results/table.md`; per-run JSON:
`results/<exp>.json`; plots: `results/curves_selected.png`.

## 7. Results by question

### Q1. Are PPO and SAC learnable? Steps/time to t_can >= 10 s

**Yes, both, with full 36D observations and base reward.**

| algo | envs | first success | wall to first success | steps/s | budget | best | final | best-checkpoint robustness |
|---|---|---|---|---|---|---|---|---|
| PPO | 64 | 11.33M | 16.0 min | ~13.3k | 45 min / 35.98M | 26.85 s | 26.61 s | 8/8 (mean 26.86 s) |
| SAC | 32 | 0.70M | 24.7 min | ~470 | 50 min / 1.42M | 28.75 s | 28.21 s | 6/8 (mean 20.89 s) |

The first successful snapshots were 26.65 s (PPO at 11.33M) and 16.74 s
(SAC at 0.70M).  SAC needs ~16x fewer environment steps but ~28x more
wall-clock per step, so the time to first success is comparable (16 vs
25 min).  PPO is more stable
at the final snapshot; SAC's final policy is also strong (8/8, 28.21 s).
PPO eval t_can is intermittent during learning (0 <-> 26 s between successive
192k-step evals), so a single eval snapshot is not a reliable success
criterion; both `_best.pt` and the success-snapshot count are reported.

### Q2. Angle-only vs full

**Removing the explicit da/5 channel is a hard failure in this budget.**

| exp | algo | obs | best t_can | final t_can | best robust |
|---|---|---|---|---|---|
| ppo_full_base_rand | PPO | full 36 | **26.85 s** | 26.61 s | 8/8 |
| ppo_angle_base_rand | PPO | angle 24 | 0.97 s | 0.04 s | 0/8 |
| sac_full_base_rand | SAC | full 36 | **28.75 s** | 28.21 s | 6/8 |
| sac_angle_base_rand | SAC | angle 24 | 0.04 s | 0.00 s | 0/8 |

PPO angle-only can swing up (ever_up=1, first up ~1.2 s) but cannot hold;
its best up_frac is 3.8%.  SAC angle-only only briefly touches upright
(best t_can 0.04 s, a single 0.04 s window at ~9.23 s; final ever_up 0) and
its final return is negative.  H=4 cos/sin history does not substitute for
the manually differentiated velocity.  This is a budget
statement, not an information-theoretic impossibility, but 40-50 min and
20-32M steps is a generous budget in this study.

### Q3. H=4 -> H=1 and reduced observations

**H=1 destroys learnability even with explicit da; longer history only
partially substitutes for da.**

| exp | obs | dim | best t_can | final t_can | ever_up | best robust |
|---|---|---|---|---|---|---|
| ppo_full_base_rand | full H=4 | 36 | **26.85 s** | 26.61 s | yes | 8/8 |
| ppo_full_h1_rand | full H=1 | 9 | 0.06 s | 0.00 s | no | 0/8 |
| ppo_angle_base_rand | angle H=4 | 24 | 0.97 s | 0.04 s | yes | 0/8 |
| ppo_angle_h8_rand | angle H=8 | 48 | 1.31 s | 0.05 s | yes | 0/8 |
| ppo_angle_h1_rand | angle H=1 | 6 | 0.00 s | 0.00 s | no | 0/8 |
| ppo_full_h1_mix | full H=1 + 50% upright | 9 | 0.02 s | 0.00 s | no | 0/8 |

The 9D full_h1 observation has the instantaneous full state (cos/sin+da/5)
but no temporal window.  It never reaches upright in 25.4M steps.  A
curriculum with half the resets already near upright also fails (21.4M
steps), so the H=1 failure is representational (no phase/acceleration
history), not only exploration.  Angle H=8 improves the best t_can from 0.97
to 1.31 s but is still far from success; explicit da + H=4 is the only
working combination.

### Q4. Reward preset

| exp | reward | env steps | best t_can | final t_can | ever_up | note |
|---|---|---|---|---|---|---|
| ppo_full_base_rand | base | 35.98M | **26.85 s** | 26.61 s | yes | works |
| ppo_full_sparse_rand | sparse | 19.12M | 0.00 s | 0.00 s | no | policy -> tiny actions |
| ppo_full_energy_rand | energy v2 | 24.50M | 0.00 s | 0.00 s | no | spinning optimum |
| (failed v1) | energy unbounded | 6.7M | 0.00 s | 0.00 s | no | value loss ~5e7 |

**base** is the only working reward.  **sparse** has no usable gradient from
the near-down reset: `0.2 exp(-2 s2)` is ~1e-26 at s2~30, and the 0.15 rad
bonus is never reached by exploration; the policy converges to mean |u|~0.01
and return -0.39.  This is an exploration failure, not a crash.

**energy** failed in two designs.  v1 used an unbounded energy error and
blew up the value loss; v2 clamped it and kept the base angle terms but still
never reached upright in 24.5M steps (0/24.5M, final return -1948).  The
reason is degenerate: total mechanical energy `E = T + V` can be driven to
`E_up` by spinning fast at the wrong angles, and `T` computed from the
manual `da` is noisy/large exactly there.  A strong energy term rewards that
spin instead of swing-up.  The base reward keeps energy weight at 0.01;
raising it is not a shortcut.

### Q5. Fixed-init vs random-init (algorithm-dependent)

| exp | algo | init | env steps | best t_can | final t_can | best-checkpoint robustness |
|---|---|---|---|---|---|---|
| ppo_full_base_rand | PPO | random | 35.98M | **26.85 s** | 26.61 s | 8/8 (26.86 s) |
| ppo_full_base_fixed | PPO | fixed | 21.93M | **1.19 s** | 0.00 s | 0/8 (mean return -99,380) |
| sac_full_base_rand | SAC | random | 1.42M | **28.75 s** | 28.21 s | 6/8 (20.89 s) |
| sac_full_base_fixed | SAC | fixed | 1.41M | **28.86 s** | 27.62 s | 8/8 (23.45 s) |

PPO fixed-init **fails**, even at the exact evaluation state (best 1.19 s),
and is catastrophic on the 8 distribution starts (mean return -99k).  PPO's
on-policy batch contains 64 copies of one state, so it never sees the
balance-recovery states it needs.

SAC fixed-init **succeeds** (best 28.86 s, final 27.62 s, 8/8 on the
distribution starts, first success at 0.52M).  SAC's replay buffer is filled
with trajectories generated by its stochastic policy from that one start, so
it does see off-state recovery data.  This is the opposite of the old
`(tau1,0,tau3)` round, where fixed-init SAC failed and fixed-init PPO looked
better at the fixed state.  The corrected internal-motor dynamics changes
which algorithm can exploit a single start.

### Seed variance (PPO full/base/rand)

| seed | env steps | first success | best t_can | final t_can | best-checkpoint robustness |
|---|---|---|---|---|---|
| 0 | 35.98M | 11.33M | 26.85 s | 26.61 s | 8/8 (26.86 s) |
| 1 | 29.03M | 13.63M | 27.17 s | 0.23 s | 8/8 (26.78 s) |
| 2 | 34.21M | 14.40M | 28.46 s | 0.00 s | 8/8 (28.03 s) |

All three seeds solve the task at their best checkpoint (8/8 robust), but the
final checkpoint is unreliable (seed 2 final 0.00 s).  First success ranges
11.3-14.4M steps (16-20 min).  **Always evaluate and ship `_best.pt`; the
last model is not the policy.**

## 8. Non-RL feasibility reference

`reference_controller.py` → `results/reference_controller.json`:

* Linearise at upright, pole placement with poles -5..-10, full state
  (theta + simulator omega), torque command low-pass filtered with alpha=0.5.
* Fixed start theta = pi + [0.013,-0.008,0.021], omega=0:
  **t_can = 29.99 s**, final angle error ~2e-14, saturation 0%.
* 32 random upright offsets U(-0.05,0.05): **32/32 success**, mean t_can
  24.79 s.
* Without the low-pass filter the same controller chatters and holds only
  ~1.1 s; smooth torque commands matter for the 0.05 rad tolerance.

This proves the upright hold is physically reachable with
`(tau1,-tau3,tau3)` and |u|<=1.  It uses privileged omega and starts near
upright, so it is not comparable to the RL numbers; it is an existence check
and a hint that action smoothness matters.

## 9. Failures / implementation notes (honest list)

* PPO seed 0's results JSON was reconstructed by `finalize_run.py`.  The
  trainer crashed **after** saving checkpoints and TB, in the post-training
  robustness code (passed `ActorCritic` instead of `ActorCritic.actor` to
  `make_ppo_policy`).  Training was unaffected.  The bug is fixed in
  `train_ppo.py`; all later PPO runs wrote JSON directly.
* The first energy preset produced unbounded rewards and a value-loss blow-up
  (~5e7).  It was killed at 6.7M steps and archived under
  `failed/energy_unbounded/`.  The second energy design is the one in the
  matrix; it fails for a genuine reward-design reason, not a crash.
* Sparse reward's no-op policy is a real exploration failure, not a logging
  artefact: final mean |u| ~0.01, return -0.39, ever_up 0.
* Some runs show value-loss spikes (PPO fixed, PPO angle) without NaNs; the
  policies still train/plateau.  These are recorded in TB.
* No simulator physics was changed.  The torque mapping was not reverted.

## 10. Artifacts

* TensorBoard: `tb/<exp>/` (live event files, all 15 runs).
* Logs: `logs/<exp>.log` (stdout, contains eval lines and DONE summary).
* Results: `results/<exp>.json`, aggregate `results/summary.json`,
  `results/table.md`, `results/curves.png`, `results/curves_selected.png`.
* Checkpoints: `checkpoints/<exp>_{best,last,final}.pt`.
* Smoke tests: `smoke_archive/`; 6-min probe: `probes/`.
* Failed/abandoned runs: `failed/energy_unbounded/`, `failed/energy_collapse/`.
* Reference controller: `results/reference_controller.json`.

## 11. Reproduction

```bash
cd feasibility/neural-pendulum-fixedenv
export OMP_NUM_THREADS=4 MKL_NUM_THREADS=4 TORCH_THREADS=4
python3 test_contract.py
python3 train_ppo.py --exp ppo_full_base_rand --obs full --reward base \
  --init rand --envs 64 --horizon 600 --minutes 45 --eval-every 5 --seed 0
python3 train_sac.py --exp sac_full_base_rand --obs full --reward base \
  --init rand --envs 32 --minutes 50 --eval-every 20000 --log-every 10 --seed 0
python3 summarize.py
python3 plot_selected.py
```

## 12. Recommendations / next steps

1. **For the actual C++ task, the controller should estimate velocity from
   angles and use a 4-frame history (or an explicit recurrent state).**  The
   3D observation is not enough by itself; the agent must reconstruct phase.
   `full` 36D is the strongest configuration in this study.
2. **Train from random near-down resets, not one fixed state.**  For PPO this
   is the difference between success and failure; even for SAC, random init
   is more robust across the distribution.
3. **Use base reward; do not chase sparse or strong energy shaping.**  If a
   sparse objective is required, add a curriculum or demonstration warm start;
   pure sparse PPO does nothing in 19M steps.
4. **Select checkpoints by eval t_can and ship the best, not the last.**
   Final models collapse in 2 of 3 PPO seeds.
5. **If more compute is available**, try (a) a recurrent/GRU policy on the 3D
   angle stream, (b) an action low-pass filter or action-rate penalty, and
   (c) a two-stage curriculum (balance near upright, then swing-up) with
   warm-started PPO.  These target the two observed bottlenecks: velocity
   estimation and balance-phase coverage.
