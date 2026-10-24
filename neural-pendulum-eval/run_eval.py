#!/usr/bin/env python3
"""Standalone evaluator for the Neural-Pendulum task.

It takes the latest host-side submission, builds it once, runs the same
PyTorch-referenced operator oracle as Neural-FP32 / Neural-CIFAR (every
operation must be executable and within its error threshold), then drives the
candidate controller against the fixed-initial-state evaluation simulator and
computes the task's time-integral score from the host events staged under
.eval/.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import selectors
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import ops_eval  # noqa: E402

SIM = HERE / 'sim_eval'
HOST_EVAL_KEYS = frozenset({
    'EVAL_RESULT_FILE', 'EVAL_JOB_DIR', 'EVAL_INSTALL_TOKEN',
    'EVAL_BASELINE_DIR', 'EVAL_VENV_MANIFEST',
})


def child_env() -> dict[str, str]:
    """Environment for build/simulator/candidate children."""
    return {
        key: value for key, value in os.environ.items()
        if key not in HOST_EVAL_KEYS and key not in {'PYTHONPATH', 'PYTHONHOME'}
    }

DT = 0.01
MAX_STEPS = 3000
T_TGT = 10.0
UP_TOL = 0.05
EPISODES = 100
SUCCESS_TARGET = 0.8
# The score changes at the test request instant. This process is started for
# one request, so module import is the closest available request timestamp.
TEST_TIME = time.time()
# Hard budgets so a pathological submission cannot stall the evaluation job.
OPS_DEADLINE_SECONDS = 240.0
CONTROL_DEADLINE_SECONDS = 900.0


class LineReader:
    """Binary line reader with a per-read deadline.

    A selector on the raw fd plus an internal buffer avoids the classic bug of
    select() missing lines that a text wrapper already buffered.
    """

    def __init__(self, proc: subprocess.Popen) -> None:
        assert proc.stdout is not None
        self._fd = proc.stdout.fileno()
        self._buffer = b''
        self._selector = selectors.DefaultSelector()
        self._selector.register(self._fd, selectors.EVENT_READ)

    def readline(self, timeout: float) -> str:
        deadline = time.monotonic() + timeout
        while b'\n' not in self._buffer:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise RuntimeError('timeout')
            if not self._selector.select(remaining):
                raise RuntimeError('timeout')
            chunk = os.read(self._fd, 65536)
            if not chunk:
                raise RuntimeError('process closed')
            self._buffer += chunk
        line, self._buffer = self._buffer.split(b'\n', 1)
        return line.decode('utf-8', 'replace').strip()

    def close(self) -> None:
        try:
            self._selector.close()
        except Exception:
            pass


def write_line(proc: subprocess.Popen, obj: dict) -> None:
    assert proc.stdin is not None
    proc.stdin.write((json.dumps(obj) + '\n').encode('utf-8'))
    proc.stdin.flush()


def read_events(workspace: Path) -> list[dict]:
    path = workspace / '.eval' / 'events.jsonl'
    out: list[dict] = []
    if path.is_file():
        for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
            try:
                value = json.loads(line)
                if isinstance(value, dict):
                    out.append(value)
            except Exception:
                continue
    return out


def task_start(workspace: Path, events: list[dict]) -> float:
    meta = workspace / '.eval' / 'meta.json'
    if meta.is_file():
        try:
            return float(json.loads(meta.read_text(encoding='utf-8')).get('started_at'))
        except Exception:
            pass
    if events:
        return float(events[0].get('time', events[0].get('at', time.time())))
    return time.time()


def score_for(event: dict) -> float:
    stage = event.get('stage', '')
    if stage not in ('scoring', 'completed') and event.get('valid') is not True:
        return -1.0
    try:
        success_rate = float(event.get('success_rate'))
    except Exception:
        success_rate = float('nan')
    if math.isfinite(success_rate):
        return (min(max(success_rate, 0.0), SUCCESS_TARGET) - SUCCESS_TARGET) / SUCCESS_TARGET
    # Backward-compatible fallback for old single-episode events.
    try:
        t_can = float(event.get('t_can', 0.0))
    except Exception:
        return -1.0
    if not math.isfinite(t_can):
        return -1.0
    return (min(max(t_can, 0.0), T_TGT) - T_TGT) / T_TGT


def final_score(workspace: Path, events: list[dict], now: float | None = None) -> tuple[float, float | None]:
    """Return (integral, T_done); a negative score is penalized over time."""
    now = time.time() if now is None else now
    start = task_start(workspace, events)
    tests = [e for e in events if (e.get('event') or e.get('type')) == 'test']
    if not tests:
        return -1.0 * max(0.0, now - start), None
    done_at = None
    prev_at = start
    current = -1.0
    total = 0.0
    for e in tests:
        at = float(e.get('time', e.get('at', prev_at)))
        if at < prev_at:
            at = prev_at
        total += current * (at - prev_at)
        current = score_for(e)
        prev_at = at
        if e.get('completed'):
            done_at = at
            break
    end = done_at if done_at is not None else now
    total += current * max(0.0, end - prev_at)
    return total, done_at


def latest_submission(workspace: Path) -> Path | None:
    root = workspace / '.eval' / 'submissions'
    if not root.is_dir():
        return None
    dirs = [p for p in root.iterdir() if p.is_dir()]
    if not dirs:
        return None
    return max(dirs, key=lambda p: p.stat().st_mtime)


def submission_count(workspace: Path) -> int:
    root = workspace / '.eval' / 'submissions'
    return len(list(root.glob('*'))) if root.is_dir() else 0


def emit(result: dict) -> int:
    target = os.environ.get('EVAL_RESULT_FILE')
    text = json.dumps(result, ensure_ascii=False, indent=2)
    if target:
        Path(target).write_text(text, encoding='utf-8')
    else:
        print(text)
    return 0


def final_score_with_current(workspace: Path, events: list[dict], stage: str,
                             t_can: float, success: bool, success_rate: float,
                             episodes: int) -> tuple[float, float | None]:
    """Score including the test this process is performing at TEST_TIME."""
    current = {
        'event': 'test', 'time': TEST_TIME, 'stage': stage,
        't_can': float(t_can), 'success_rate': float(success_rate),
        'episodes': int(episodes), 'completed': bool(success),
    }
    return final_score(workspace, [*events, current])


def build_result(stage: str, workspace: Path, events: list[dict], error: str = '',
                 ops: dict | None = None, source: str = '', t_can: float = 0.0,
                 success: bool = False, success_rate: float = 0.0,
                 episodes: int = 0) -> dict:
    integral, done_at = final_score_with_current(workspace, events, stage, t_can, success,
                                                 success_rate, episodes)
    result = {
        'task': 'neural-pendulum',
        'ok': True,
        'stage': stage,
        'infra_ok': False,
        't_can': float(t_can),
        'success': bool(success),
        'success_rate': float(success_rate),
        'episodes': int(episodes),
        'error': error,
        'final_score': integral,
        'done_at': done_at,
        'submissions': submission_count(workspace),
        'source': source,
    }
    if ops is not None:
        result.update({
            'all_operations_executable': ops.get('all_operations_executable'),
            'all_operations_passed': ops.get('all_operations_passed'),
            'max_weighted_mean_relative_error': ops.get('max_weighted_mean_relative_error'),
            'speed_geomean': ops.get('speed_geomean'),
            'operations': ops.get('operations', []),
        })
    return result


def fail(message: str, stage: str, workspace: Path, events: list[dict],
         ops: dict | None = None) -> dict:
    return build_result(stage, workspace, events, error=message, ops=ops)


def main() -> int:
    parser = argparse.ArgumentParser(description='Neural-Pendulum evaluator')
    parser.add_argument('--workspace', required=True)
    args = parser.parse_args()
    workspace = Path(args.workspace).resolve()
    events = read_events(workspace)
    root = Path(tempfile.mkdtemp(prefix='neural-pendulum-eval-'))
    sim = None
    control = None
    sim_reader = None
    control_reader = None
    try:
        selected = latest_submission(workspace)
        if selected is None:
            return emit(fail('no submission', 'no_submission', workspace, events))
        source_name = str(selected.relative_to(workspace))

        try:
            source = ops_eval.prepare_source(selected, root)
            build = ops_eval.build_binaries(source)
        except Exception as exc:
            return emit(fail(str(exc), 'compile', workspace, events))

        ops_binary = build / 'nn_task'
        control_path = build / 'control'
        missing = [name for name, path in (('build/nn_task', ops_binary), ('build/control', control_path))
                   if not path.is_file()]
        if missing:
            return emit(fail('build output missing: ' + ', '.join(missing), 'compile', workspace, events))

        try:
            ops = ops_eval.evaluate_ops(ops_binary, root, deadline=time.monotonic() + OPS_DEADLINE_SECONDS)
        except Exception as exc:
            return emit(fail('operator evaluation failed: ' + str(exc), 'infra', workspace, events))
        ops_eval.print_summary(ops)
        if not ops['all_operations_passed']:
            return emit(fail(ops_eval.infrastructure_error(ops), 'infra', workspace, events, ops=ops))

        env = child_env()
        # sim_eval is the evaluator-only binary.  Every reset samples a fresh
        # hidden plant, initial state and observation-noise level; the success
        # check uses the true state, while the controller only receives obs.
        sim = subprocess.Popen([str(SIM)], stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, env=env)
        control = subprocess.Popen([str(control_path)], cwd=str(source),
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.DEVNULL, env=env)
        sim_reader = LineReader(sim)
        control_reader = LineReader(control)

        successes = 0
        t_can_total = 0.0
        completed = False
        error = ''
        stage = 'scoring'
        attempted = 0
        control_deadline = time.monotonic() + CONTROL_DEADLINE_SECONDS
        for ep in range(EPISODES):
            if time.monotonic() > control_deadline:
                error = 'control evaluation deadline exceeded'
                stage = 'simulation'
                break
            # A fresh hidden plant, random initial state and fresh observation
            # noise level for every evaluation episode.
            try:
                write_line(sim, {'cmd': 'reset'})
                reply = json.loads(sim_reader.readline(10.0))
            except Exception as exc:
                error = 'simulator reset failed: ' + str(exc)
                stage = 'simulation'
                break
            obs = reply.get('obs') if isinstance(reply, dict) else None
            true = reply.get('state', obs) if isinstance(reply, dict) else None
            try:
                write_line(control, {'type': 'reset'})
                ready = json.loads(control_reader.readline(10.0))
            except Exception as exc:
                error = 'controller reset failed: ' + str(exc)
                stage = 'controller'
                break
            if not isinstance(ready, dict) or ready.get('type') != 'ready':
                error = 'controller did not reply ready'
                stage = 'controller'
                break
            attempted += 1
            up_start = None
            t_can_ep = 0.0
            failed = False
            for step in range(MAX_STEPS):
                if time.monotonic() > control_deadline:
                    error = 'control evaluation deadline exceeded'
                    stage = 'simulation'
                    failed = True
                    break
                if not isinstance(obs, list) or len(obs) != 3:
                    error = 'simulator returned bad observation'
                    stage = 'simulation'
                    failed = True
                    break
                try:
                    angles = [float(x) for x in obs]
                    true_angles = [float(x) for x in true] if isinstance(true, list) and len(true) == 3 else angles
                except Exception:
                    error = 'simulator returned non-numeric observation'
                    stage = 'simulation'
                    failed = True
                    break
                if (not all(math.isfinite(x) for x in angles)
                        or not all(math.isfinite(x) for x in true_angles)):
                    error = 'simulator returned non-finite observation'
                    stage = 'simulation'
                    failed = True
                    break
                # Success is judged on the true state; the controller only sees
                # the noisy angles below.
                if max(abs(x) for x in true_angles) <= UP_TOL:
                    if up_start is None:
                        up_start = step * DT
                    t_can_ep = max(t_can_ep, step * DT - up_start)
                else:
                    up_start = None
                if t_can_ep >= T_TGT:
                    break
                write_line(control, {'type': 'step', 'step': step, 'state': angles})
                try:
                    action = json.loads(control_reader.readline(0.1))
                except Exception as exc:
                    error = 'controller did not reply in time: ' + str(exc)
                    stage = 'controller'
                    failed = True
                    break
                torque = action.get('torque') if isinstance(action, dict) else None
                if (not isinstance(torque, list) or len(torque) != 2
                        or not all(isinstance(v, (int, float)) and not isinstance(v, bool)
                                   and math.isfinite(v) for v in torque)
                        or any(abs(float(v)) > 1.0 for v in torque)):
                    error = 'controller returned invalid torque'
                    stage = 'controller'
                    failed = True
                    break
                write_line(sim, {'cmd': 'step', 'torque': [float(torque[0]), float(torque[1])]})
                try:
                    reply = json.loads(sim_reader.readline(10.0))
                except Exception as exc:
                    error = 'simulator did not reply: ' + str(exc)
                    stage = 'simulation'
                    failed = True
                    break
                if not isinstance(reply, dict):
                    error = 'simulator returned bad reply'
                    stage = 'simulation'
                    failed = True
                    break
                if 'error' in reply:
                    error = 'simulator error: ' + str(reply['error'])
                    stage = 'simulation'
                    failed = True
                    break
                obs = reply.get('obs')
                true = reply.get('state', obs)
            if failed:
                break
            if t_can_ep >= T_TGT:
                successes += 1
            t_can_total += t_can_ep
        if attempted == 0:
            if stage == 'scoring':
                stage = 'simulation'
            error = error or 'no evaluation episode could be executed'
            success_rate, t_can = 0.0, 0.0
        else:
            success_rate = successes / attempted
            t_can = t_can_total / attempted
        completed = (stage == 'scoring' and attempted == EPISODES
                     and success_rate >= SUCCESS_TARGET)
        stage = 'completed' if completed else stage
        result = build_result(stage, workspace, events, error=error, ops=ops, source=source_name,
                               t_can=t_can, success=completed, success_rate=success_rate,
                               episodes=attempted)
        result['infra_ok'] = True
        return emit(result)
    except Exception as exc:
        return emit(fail(str(exc), 'simulation', workspace, events))
    finally:
        for reader in (control_reader, sim_reader):
            if reader is not None:
                reader.close()
        for proc in (control, sim):
            if proc is not None:
                try:
                    proc.terminate()
                    proc.wait(timeout=2)
                except Exception:
                    try:
                        proc.kill()
                    except Exception:
                        pass
        shutil.rmtree(root, ignore_errors=True)


if __name__ == '__main__':
    raise SystemExit(main())
