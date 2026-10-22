# neural-cifar-eval

Standalone evaluator for the **Neural-CIFAR** task. Copy this whole folder to a
more performance-stable Linux machine to rerun the model evaluation without the
DSH plugin UI.

## Contents

- `run_eval.py` — builds the candidate, runs the original operator metrics,
  then measures CIFAR-10 validation accuracy/FPS at 4 threads and all threads.
- `ops_eval.py` / `common.py` — self-contained operator evaluator.
- `fetch_cifar.py` — downloads the official CIFAR-10 test split and writes
  `hidden/val_images.bin` + `hidden/val_labels.bin`.
- `hidden/` — hidden validation data; not part of the agent workspace.

## Prepare hidden validation

```bash
python3 fetch_cifar.py
# optional proxy:
python3 fetch_cifar.py --proxy http://127.0.0.1:7897
```

CIFAR-10 cache is shared under `~/.dsh/eval-guard/cache/cifar10/` and is
protected by `.download.lock`. If the lock exists, another DSH instance is
downloading; wait for it or delete the lock file manually. A complete cache
with no lock is reused directly. Downloads print progress.

## Run

```bash
python3 run_eval.py --workspace /path/to/session/workspace
```

The candidate workspace must contain `build/nn_task`, `build/train`,
`build/infer`, and a trained `model/` directory. The evaluator does not train;
it loads the already-trained model.

The first output lines are always:

```text
all_operations_executable = ...
max_weighted_mean_relative_error = ...
speed_geomean = ...
model_4_thread = ...
model_all_threads = ...
```

Then every operator row is printed. There is no unified score and no resolved
gate. The 4-thread 0.02 s/image requirement is reported as
`meets_4thread_budget`, but does not change any PASS/FAIL result.

## Notes

- PyTorch is required on the evaluation machine for the operator oracle.
- The 4-thread and all-thread model runs use the same trained model.
- The validation data and labels never enter the candidate workspace.
- The label file is read into the evaluator's memory; `infer` receives a copy of the
  images in an isolated temporary directory, so it cannot derive the labels path
  from its `--images` argument. Candidate/spawned processes also lose the host
  `EVAL_*` handle variables.
- FPS and mean seconds/image include process startup and model loading.
