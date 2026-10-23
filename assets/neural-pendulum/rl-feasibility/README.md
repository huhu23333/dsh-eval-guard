# Neural-Pendulum RL feasibility harness (dev-only reference)

This directory is a **development/reference** copy of the reinforcement-learning
environment used to confirm that the corrected Neural-Pendulum dynamics are
learnable.  It is **not** part of the candidate runtime, is not copied into the
task workspace, and is not required by the evaluator.

## Fixed physics

The Python simulator here uses the same physical model as
`assets/neural-pendulum/sim.cpp` / `sim_eval`:

- three-link planar pendulum, 100 Hz ZOH, RK4 with 4 substeps;
- physical joint-motor mapping in absolute-angle coordinates:
  `(tau1, -tau3, tau3)` (joint 3 is the internal motor between links 2 and 3,
  joint 2 is passive);
- corrected Coriolis/centrifugal term
  `C_i = sum_j a_ij sin(theta_i-theta_j) omega_j^2`;
- 30 s episodes, success tolerance 0.05 rad, t_tgt = 10 s.

The Python simulator was cross-checked against `../../sim_eval` on the fixed
initial state and identical action sequences; maximum observation error was
about 5e-6 (simulator output has 6 significant digits).

## What was run

15 formal runs (PPO/SAC, observation ablations, reward ablations, fixed vs
random init; 3 PPO seeds) in the corrected environment.  Full report:
`RESULTS.md`; machine table: `results/table.md`; aggregate:
`results/summary.json`; selected curves: `results/curves_selected.png`.

Core result:

- PPO full 36D (cos/sin + manual-difference da/5, H=4), base reward, random
  init: first success 11.33M env steps / 16 min; best t_can 26.85 s.
- SAC full 36D, base reward, random init: first success 0.70M env steps;
  best t_can 28.75 s.
- Angle-only (no da) and H=1 observations fail; sparse/energy rewards fail in
  this budget; PPO needs random init, SAC can also learn from fixed init.
- A non-RL pole-placement reference with 0.5 low-pass holds 29.99 s from the
  fixed upright offset and succeeds on 32/32 random upright offsets, proving
  the hold is physically feasible.

## Run

Requires Python 3 with CPU PyTorch on the development host.  This is **not**
available to the candidate sandbox and is not a submission dependency.

```bash
cd assets/neural-pendulum/rl-feasibility
TORCH_THREADS=4 python3 test_contract.py
TORCH_THREADS=4 python3 train_ppo.py --exp local_ppo --init rand --obs full \
    --reward base --envs 64 --minutes 30 --tb-root tb --ckpt-dir checkpoints --out results
tensorboard --logdir tb
```

TensorBoard event files are written live during training.  The experiment
matrix and per-run JSON/TB/log files from the full sweep are kept outside the
npm package; only the source, report, summary, table and selected curve are
bundled here to keep the plugin package small.
