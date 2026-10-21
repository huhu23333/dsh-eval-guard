#!/usr/bin/env python3
"""Standalone evaluator for the Neural-Pendulum task.

It uses the latest preserved submission under .eval/submissions (falling back to
the live workspace), builds it from source, runs the public self-test, drives
the controller against a fixed-initial-state evaluation simulator, and computes
the task's time-integral score from .eval/events.jsonl.
"""
from __future__ import annotations

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
SIM = HERE / 'sim_eval'
DT = 0.01
MAX_STEPS = 3000
T_TGT = 10.0
UP_TOL = 0.05


def read_line(proc: subprocess.Popen, timeout: float) -> str:
    selector = selectors.DefaultSelector()
    selector.register(proc.stdout, selectors.EVENT_READ)
    try:
        if not selector.select(timeout):
            raise RuntimeError('timeout')
        line = proc.stdout.readline()
        if line == '':
            raise RuntimeError('process closed')
        return line.strip()
    finally:
        selector.close()


def write_line(proc: subprocess.Popen, obj: dict) -> None:
    assert proc.stdin is not None
    proc.stdin.write(json.dumps(obj) + '\n')
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
        t_can = float(event.get('t_can', 0.0))
    except Exception:
        return -1.0
    if not math.isfinite(t_can):
        return -1.0
    return (min(max(t_can, 0.0), T_TGT) - T_TGT) / T_TGT


def final_score(workspace: Path, events: list[dict], now: float | None = None) -> tuple[float, float | None]:
    """Return (integral, T_done); negative score is penalized over time."""
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


def archive_submissions(workspace: Path) -> int:
    root = os.environ.get('EVAL_SUBMISSIONS_ROOT', '')
    src_root = workspace / '.eval' / 'submissions'
    if not root or not src_root.is_dir():
        return 0
    target = Path(root)
    target.mkdir(parents=True, exist_ok=True)
    count = 0
    for source in src_root.iterdir():
        if not source.is_dir():
            continue
        destination = target / source.name
        if destination.exists():
            continue
        shutil.copytree(source, destination, symlinks=True)
        count += 1
    return count


def latest_submission(workspace: Path) -> Path | None:
    root = workspace / '.eval' / 'submissions'
    if not root.is_dir():
        return None
    dirs = [p for p in root.iterdir() if p.is_dir()]
    if not dirs:
        return None
    return max(dirs, key=lambda p: p.stat().st_mtime)


def fail(message: str, infra_ok: bool = False, workspace: Path | None = None) -> dict:
    result = {'infra_ok': infra_ok, 't_can': 0.0, 'success': False, 'error': message}
    if workspace is not None:
        events = read_events(workspace)
        result['final_score'], done = final_score(workspace, events)
        result['done_at'] = done
        result['submissions'] = len(list((workspace / '.eval' / 'submissions').glob('*'))) if (workspace / '.eval' / 'submissions').is_dir() else 0
    return result


def main() -> int:
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('--workspace', required=True)
    args = parser.parse_args()

    workspace = Path(args.workspace).resolve()
    events = read_events(workspace)
    root = Path(tempfile.mkdtemp(prefix='neural-pendulum-eval-'))
    candidate = root / 'candidate'
    result = {'infra_ok': False, 't_can': 0.0, 'success': False, 'error': ''}
    sim = None
    control = None
    try:
        archived = archive_submissions(workspace)
        selected = latest_submission(workspace)
        if selected is None:
            return emit(fail('no submission', workspace=workspace))
        source = selected
        shutil.copytree(source, candidate, symlinks=True)
        if not (candidate / 'CMakeLists.txt').is_file():
            return emit(fail('CMakeLists.txt missing', workspace=workspace))

        env = dict(os.environ)
        env.pop('PYTHONPATH', None)
        env.pop('PYTHONHOME', None)
        build = candidate / 'build'
        if build.exists():
            shutil.rmtree(build, ignore_errors=True)
        cfg = subprocess.run(
            ['cmake', '-S', '.', '-B', 'build', '-DCMAKE_BUILD_TYPE=Release'],
            cwd=str(candidate), capture_output=True, text=True, env=env, timeout=180,
        )
        if cfg.returncode != 0:
            return emit(fail('cmake configure failed\n' + (cfg.stdout + cfg.stderr)[-4000:], workspace=workspace))
        made = subprocess.run(
            ['cmake', '--build', 'build', '-j4'],
            cwd=str(candidate), capture_output=True, text=True, env=env, timeout=1800,
        )
        if made.returncode != 0:
            return emit(fail('build failed\n' + (made.stdout + made.stderr)[-4000:], workspace=workspace))

        lib = candidate / 'build' / 'libnn.a'
        if not lib.is_file():
            return emit(fail('build/libnn.a missing', workspace=workspace))
        hidden = HERE.parent / 'assets' / 'neural-pendulum' / 'hidden' / 'test_infra.cpp'
        if not hidden.is_file():
            return emit(fail('hidden infrastructure test missing', workspace=workspace))
        infra_exe = root / 'test_infra'
        compiler = shutil.which('g++')
        if compiler is None:
            return emit(fail('g++ missing', workspace=workspace))
        ic = subprocess.run([compiler, '-std=c++17', '-O3', '-pthread',
                             '-I', str(candidate / 'include'), str(hidden), str(lib),
                             '-lm', '-o', str(infra_exe)], cwd=str(root), capture_output=True,
                            text=True, env=env, timeout=300)
        if ic.returncode != 0:
            return emit(fail('hidden infrastructure test compile failed\n' + (ic.stdout + ic.stderr)[-4000:], workspace=workspace))
        it = subprocess.run([str(infra_exe)], cwd=str(root), capture_output=True,
                            text=True, env=env, timeout=600)
        if it.returncode != 0:
            return emit(fail('hidden infrastructure test failed\n' + (it.stdout + it.stderr)[-4000:], workspace=workspace))

        control_path = candidate / 'build' / 'control'
        if not control_path.is_file():
            return emit(fail('build/control missing', infra_ok=True, workspace=workspace))

        sim = subprocess.Popen([str(SIM), '--eval'], stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True, bufsize=1)
        control = subprocess.Popen([str(control_path)], cwd=str(candidate),
                                    stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, text=True, bufsize=1)

        write_line(sim, {'cmd': 'reset'})
        obs = json.loads(read_line(sim, 10.0))['obs']
        write_line(control, {'type': 'reset'})
        ready = json.loads(read_line(control, 10.0))
        if ready.get('type') != 'ready':
            return emit(fail('controller did not reply ready', infra_ok=True, workspace=workspace))

        t_can = 0.0
        up_start = None
        completed = False
        error = ''
        for step in range(MAX_STEPS):
            if not isinstance(obs, list) or len(obs) != 3:
                error = 'simulator returned bad observation'
                break
            if max(abs(float(x)) for x in obs) < UP_TOL:
                if up_start is None:
                    up_start = step * DT
                t_can = max(t_can, step * DT - up_start)
            else:
                up_start = None
            if t_can >= T_TGT:
                completed = True
                break

            write_line(control, {'type': 'step', 'step': step, 'state': obs})
            action = json.loads(read_line(control, 0.1))
            torque = action.get('torque')
            if (not isinstance(torque, list) or len(torque) != 2
                    or not all(isinstance(v, (int, float)) and math.isfinite(v) for v in torque)
                    or any(abs(float(v)) > 1.0 for v in torque)):
                error = 'controller returned invalid torque'
                break
            write_line(sim, {'cmd': 'step', 'torque': torque})
            reply = json.loads(read_line(sim, 10.0))
            if 'error' in reply:
                break
            obs = reply['obs']

        integral, done_at = final_score(workspace, events)
        result = {'infra_ok': True, 't_can': float(t_can),
                  'success': bool(completed), 'error': error,
                  'final_score': integral, 'done_at': done_at,
                  'submissions': len(list((workspace / '.eval' / 'submissions').glob('*'))) if (workspace / '.eval' / 'submissions').is_dir() else 0,
                  'archived_submissions': archived,
                  'source': str(selected.relative_to(workspace)) if selected is not None else 'live-workspace'}
        return emit(result)
    except Exception as exc:
        return emit(fail(str(exc), workspace=workspace))
    finally:
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


def emit(result: dict) -> int:
    target = os.environ.get('EVAL_RESULT_FILE')
    text = json.dumps(result, ensure_ascii=False, indent=2)
    if target:
        Path(target).write_text(text, encoding='utf-8')
    else:
        print(text)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
