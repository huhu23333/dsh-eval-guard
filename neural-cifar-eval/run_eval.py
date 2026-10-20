#!/usr/bin/env python3
"""Standalone Neural-CIFAR evaluator.

Copies the workspace into a temporary build directory, runs the original
operator benchmark, then loads the already-trained model and measures CIFAR-10
validation accuracy/FPS at 4 threads and at all logical CPUs.

The hidden validation images/labels live beside this script in hidden/ and are
never written into the agent workspace.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from common import clean_copy

HIDDEN = HERE / 'hidden'
OPS = HERE / 'ops_eval.py'


def run(cmd, cwd, timeout=1800):
    env = dict(os.environ)
    env.pop('PYTHONPATH', None)
    env.pop('PYTHONHOME', None)
    return subprocess.run(cmd, cwd=str(cwd), capture_output=True, text=True,
                          encoding='utf-8', errors='replace', env=env, timeout=timeout)


def build_candidate(workspace: Path, root: Path):
    candidate = root / 'candidate'
    clean_copy(workspace, candidate)
    source = candidate
    if not (source / 'CMakeLists.txt').is_file() and (candidate / 'workspace' / 'CMakeLists.txt').is_file():
        source = candidate / 'workspace'
    if not (source / 'CMakeLists.txt').is_file():
        raise RuntimeError('CMakeLists.txt missing at workspace root or workspace/')
    build = source / 'build'
    if build.exists():
        shutil.rmtree(build, ignore_errors=True)
    build.mkdir(parents=True)
    cfg = run(['cmake', '-S', '.', '-B', 'build', '-DCMAKE_BUILD_TYPE=Release'], source, 180)
    if cfg.returncode != 0:
        raise RuntimeError('cmake configure failed\n' + (cfg.stdout + cfg.stderr)[-4000:])
    made = run(['cmake', '--build', 'build', '-j4'], source, 1800)
    if made.returncode != 0:
        raise RuntimeError('build failed\n' + (made.stdout + made.stderr)[-4000:])
    return source, build


def read_preds(path: Path, n: int) -> np.ndarray:
    a = np.frombuffer(path.read_bytes(), dtype=np.uint8).copy()
    if a.size != n:
        raise RuntimeError(f'prediction size {a.size} != {n}')
    return a


def model_metrics(source: Path, threads: int, budget: float | None = None) -> dict:
    infer = source / 'build' / 'infer'
    images = HIDDEN / 'val_images.bin'
    labels = HIDDEN / 'val_labels.bin'
    if not infer.is_file():
        return {'executable': False, 'accuracy': None, 'fps': None,
                'mean_seconds_per_image': None, 'error': 'build/infer missing',
                'meets_4thread_budget': None}
    if not images.is_file() or not labels.is_file():
        return {'executable': False, 'accuracy': None, 'fps': None,
                'mean_seconds_per_image': None, 'error': 'hidden validation data missing; run fetch_cifar.py',
                'meets_4thread_budget': None}
    n = labels.stat().st_size
    out = source / f'predictions_{threads}.bin'
    if out.exists():
        out.unlink()
    cmd = [str(infer), '--model', str(source / 'model'),
           '--images', str(images), '--out', str(out), '--threads', str(threads)]
    started = time.perf_counter()
    try:
        proc = run(cmd, source, 1200)
    except subprocess.TimeoutExpired:
        return {'executable': False, 'accuracy': None, 'fps': None,
                'mean_seconds_per_image': None, 'error': 'inference timeout',
                'meets_4thread_budget': None}
    elapsed = time.perf_counter() - started
    if proc.returncode != 0:
        return {'executable': False, 'accuracy': None, 'fps': None,
                'mean_seconds_per_image': None,
                'error': (proc.stderr or proc.stdout or 'infer failed')[-1000:],
                'meets_4thread_budget': None}
    try:
        pred = read_preds(out, n)
        true = np.frombuffer(labels.read_bytes(), dtype=np.uint8)
        acc = float((pred == true).mean() * 100.0)
    except Exception as exc:
        return {'executable': False, 'accuracy': None, 'fps': None,
                'mean_seconds_per_image': None, 'error': str(exc),
                'meets_4thread_budget': None}
    fps = n / elapsed if elapsed > 0 else None
    mean_s = elapsed / n if n else None
    result = {'executable': True, 'accuracy': acc, 'fps': fps,
              'mean_seconds_per_image': mean_s, 'images': n,
              'threads': threads, 'error': ''}
    if budget is not None:
        result['meets_4thread_budget'] = bool(mean_s is not None and mean_s <= budget)
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--workspace', required=True)
    args = parser.parse_args()
    workspace = Path(args.workspace).resolve()

    if not OPS.is_file():
        return fail({'task': 'neural-cifar', 'ok': False, 'error': 'ops_eval.py missing'})

    with tempfile.TemporaryDirectory(prefix='neural-cifar-eval-') as td:
        root = Path(td)
        # Reuse the existing FP32 operator evaluator, but run it in a temp
        # copy so its build output does not affect the model workspace.
        ops_result = root / 'ops.json'
        env = dict(os.environ)
        env['EVAL_RESULT_FILE'] = str(ops_result)
        env.pop('PYTHONPATH', None)
        env.pop('PYTHONHOME', None)
        try:
            proc = subprocess.run([sys.executable, '-X', 'utf8', str(OPS), '--workspace', str(workspace)],
                                  cwd=str(HERE), capture_output=True, text=True,
                                  encoding='utf-8', errors='replace', env=env, timeout=2400)
        except subprocess.TimeoutExpired:
            return fail({'task': 'neural-cifar', 'ok': False, 'error': 'operator evaluation timed out'})
        if proc.returncode != 0 or not ops_result.is_file():
            return fail({'task': 'neural-cifar', 'ok': False, 'error': 'operator evaluation failed',
                         'output': (proc.stdout + proc.stderr)[-4000:]})
        ops = json.loads(ops_result.read_text(encoding='utf-8'))

        try:
            source, _build = build_candidate(workspace, root)
        except Exception as exc:
            return fail({'task': 'neural-cifar', 'ok': False, 'error': str(exc)})

        if not (HIDDEN / 'val_images.bin').is_file() or not (HIDDEN / 'val_labels.bin').is_file():
            try:
                fetch = subprocess.run([sys.executable, '-X', 'utf8', str(HERE / 'fetch_cifar.py')],
                                       cwd=str(HERE), capture_output=True, text=True,
                                       encoding='utf-8', errors='replace', timeout=1800)
            except subprocess.TimeoutExpired:
                fetch = None
            if fetch is None or fetch.returncode != 0:
                return fail({'task': 'neural-cifar', 'ok': False,
                             'error': 'hidden validation data missing and fetch failed',
                             'output': (fetch.stdout + fetch.stderr)[-3000:] if fetch is not None else 'timeout'})

        model4 = model_metrics(source, 4, budget=0.02)
        modelall = model_metrics(source, os.cpu_count() or 1)

        print(f"all_operations_executable = {str(ops.get('all_operations_executable')).lower()}")
        print(f"max_weighted_mean_relative_error = {ops.get('max_weighted_mean_relative_error')}")
        print(f"speed_geomean = {ops.get('speed_geomean')}")
        print(f"model_4_thread = {json.dumps(model4, ensure_ascii=False)}")
        print(f"model_all_threads = {json.dumps(modelall, ensure_ascii=False)}")
        for row in ops.get('operations', []):
            print(f"{row.get('name')}: executable={row.get('executable')} pass={row.get('passed')} "
                  f"mre={row.get('weighted_mean_relative_error')} speed_gflops={row.get('speed_gflops')}")

        result = {
            'task': 'neural-cifar',
            'ok': True,
            'all_operations_executable': ops.get('all_operations_executable'),
            'max_weighted_mean_relative_error': ops.get('max_weighted_mean_relative_error'),
            'speed_geomean': ops.get('speed_geomean'),
            'model_4_thread': model4,
            'model_all_threads': modelall,
            'operations': ops.get('operations', []),
        }
        target = os.environ.get('EVAL_RESULT_FILE')
        if target:
            Path(target).write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
        else:
            print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0


def fail(payload: dict) -> int:
    target = os.environ.get('EVAL_RESULT_FILE')
    if target:
        Path(target).write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding='utf-8')
    else:
        print(json.dumps(payload, ensure_ascii=False, indent=2))
    return 1


if __name__ == '__main__':
    raise SystemExit(main())
