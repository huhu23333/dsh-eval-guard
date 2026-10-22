# neural-pendulum-eval

Standalone evaluator for the **Neural-Pendulum** task. Copy this whole folder to
a more performance-stable Linux machine to rerun the evaluation without the DSH
plugin UI.

## Contents

- `run_eval.py` — builds the latest submission, runs the operator oracle, then
  drives `build/control` against the fixed-initial-state simulator and emits a
  staged result (`no_submission` / `compile` / `infra` / `controller` /
  `simulation` / `scoring` / `completed`).
- `ops_eval.py` — operator oracle: PyTorch references computed in-process,
  weighted mean relative error compared with per-operation thresholds. Same
  operations, cases, seeds and thresholds as Neural-FP32 / Neural-CIFAR.
- `sim_eval` — trusted simulator binary for the evaluation initial state.
  Identical to the workspace `tools/sim`.

## Requirements

- Python 3 with `numpy` and `torch` (CPU) for the operator oracle.
- `cmake`, `g++`, `unzip`.

## Run

```bash
python3 run_eval.py --workspace /path/to/staged/workspace
```

The workspace must contain a host-staged `.eval/` directory:

```
.eval/
  meta.json                 # {"started_at": <unix seconds>, "t_tgt": 10}
  events.jsonl              # host submit/test events, used for the time integral
  submissions/<id>/         # snapshot with CMakeLists.txt, src/, ...
```

When `EVAL_RESULT_FILE` is set, the JSON result is written there; otherwise it
is printed to stdout. The first stdout lines always summarize the operator
oracle:

```
all_operations_executable = ...
all_operations_passed = ...
max_weighted_mean_relative_error = ...
speed_geomean = ...
```

followed by one row per operation. Only when every operation is executable and
within threshold does the evaluator run the controller. Operator speed is
reported but never gates completion.

Budgets: build 8 min, operator oracle 4 min total (60 s per CLI invocation),
controller 6 min. The host-side `eval_test` tool kills the evaluator after
20 minutes.

The evaluator reads simulator/controller responses through a buffered binary
line reader with per-read deadlines (a text wrapper can hide already-buffered
lines from a selector). Simulator/controller stderr is discarded so a verbose
controller cannot deadlock on a full pipe, and those children never inherit the
host `EVAL_*` variables.
