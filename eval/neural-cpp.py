#!/usr/bin/env python3
"""Evaluate the pure C++ Neural-CPP task.

The public workspace contains only the API contract and build skeleton. Hidden
C++ tests are injected from the plugin asset directory and generate their data
from fixed seeds at run time; no hidden data is copied into the candidate tree.
"""
from __future__ import annotations

import math
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from common import EVAL_ROOT, child_env, clean_copy, emit, fail, parse_workspace


def run(cmd: list[str], cwd: Path, timeout: int = 600) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, cwd=str(cwd), capture_output=True, text=True,
                          encoding='utf-8', errors='replace', env=child_env(), timeout=timeout)


def main() -> int:
    workspace = parse_workspace()
    hidden = EVAL_ROOT.parent / 'assets' / 'neural-cpp' / 'hidden' / 'test_neural.cpp'
    if not hidden.is_file():
        return fail({'task': 'neural-cpp', 'ok': False, 'error': 'hidden test missing'})
    # Snapshot the hidden test BEFORE the candidate build runs: an agent-authored
    # CMakeLists can otherwise overwrite the asset and substitute a passing test.
    hidden_source = hidden.read_bytes()

    with tempfile.TemporaryDirectory(prefix='neural-cpp-eval-') as td:
        root = Path(td)
        candidate = root / 'candidate'
        clean_copy(workspace, candidate)
        # The first task installation copied workspace.zip into the session
        # root; some agents extracted it into workspace/ instead of the root.
        # Accept both layouts so already-produced workspaces remain evaluable.
        source = candidate
        if not (source / 'CMakeLists.txt').is_file() and (candidate / 'workspace' / 'CMakeLists.txt').is_file():
            source = candidate / 'workspace'
        if not (source / 'CMakeLists.txt').is_file():
            return fail({'task': 'neural-cpp', 'ok': False, 'error': 'CMakeLists.txt missing',
                         'output': 'Expected CMakeLists.txt at workspace root or workspace/CMakeLists.txt'})
        build = source / 'build'
        if build.exists():
            shutil.rmtree(build, ignore_errors=True)
        build.mkdir()

        cmake = shutil.which('cmake')
        compiler = shutil.which('g++')
        if cmake is None or compiler is None:
            return fail({'task': 'neural-cpp', 'ok': False, 'error': 'cmake/g++ not available'})

        cfg = run([cmake, '-S', '.', '-B', 'build', '-DCMAKE_BUILD_TYPE=Release'], source, 180)
        if cfg.returncode != 0:
            return fail({'task': 'neural-cpp', 'ok': False, 'error': 'cmake configure failed',
                         'output': (cfg.stdout + cfg.stderr)[-5000:]})
        bld = run([cmake, '--build', 'build', '-j4'], source, 1800)
        if bld.returncode != 0:
            return fail({'task': 'neural-cpp', 'ok': False, 'error': 'build failed',
                         'output': (bld.stdout + bld.stderr)[-5000:]})
        lib = build / 'libnn.a'
        if not lib.is_file():
            return fail({'task': 'neural-cpp', 'ok': False, 'error': 'build/libnn.a missing'})

        # Compile the in-memory snapshot, not the on-disk asset the build could edit.
        hidden_copy = root / 'test_neural.cpp'
        hidden_copy.write_bytes(hidden_source)
        exe = root / 'test_neural'
        cc = run([compiler, '-std=c++17', '-O3', '-pthread',
                  '-I', str(source / 'include'), str(hidden_copy), str(lib),
                  '-lm', '-o', str(exe)], root, 300)
        if cc.returncode != 0:
            return fail({'task': 'neural-cpp', 'ok': False, 'error': 'hidden test compile failed',
                         'output': (cc.stdout + cc.stderr)[-5000:]})

        try:
            proc = run([str(exe)], root, 600)
        except subprocess.TimeoutExpired:
            return fail({'task': 'neural-cpp', 'ok': False, 'error': 'hidden tests timed out'})

        output = proc.stdout + proc.stderr
        passed = failed = 0
        gflops = 0.0
        metrics: list[dict] = []
        stats: list[dict] = []
        for line in output.splitlines():
            parts = line.strip().split()
            if len(parts) == 4 and parts[0] == 'TEST' and parts[2] == 'PASS':
                passed += 1
            elif len(parts) == 4 and parts[0] == 'TEST' and parts[2] == 'FAIL':
                failed += 1
            elif len(parts) == 3 and parts[0] == 'SUMMARY':
                try:
                    passed, failed = int(parts[1]), int(parts[2])
                except ValueError:
                    pass
            elif len(parts) == 2 and parts[0] == 'PERF_GFLOPS':
                try:
                    gflops = float(parts[1])
                except ValueError:
                    pass
            elif parts and parts[0] in {'METRIC', 'STAT'}:
                row: dict[str, object] = {}
                for item in parts[1:]:
                    if '=' not in item:
                        continue
                    key, value = item.split('=', 1)
                    try:
                        row[key] = float(value)
                    except ValueError:
                        row[key] = value
                if parts[0] == 'METRIC':
                    metrics.append(row)
                else:
                    stats.append(row)

        total = passed + failed
        accuracy = (100.0 * passed / total) if total else 0.0
        metric_scores: list[float] = []
        for row in metrics:
            for key in ('fwd_accuracy', 'bwd_accuracy'):
                value = row.get(key)
                if isinstance(value, (int, float)) and float(value) >= 0:
                    metric_scores.append(float(value))
        matrix_accuracy = sum(metric_scores) / len(metric_scores) if metric_scores else 0.0
        matrix_sizes: dict[str, list[int]] = {}
        speed_samples: list[float] = []
        speed_by_dtype: dict[str, list[float]] = {}
        for row in metrics:
            dtype = str(row.get('dtype', ''))
            try:
                m = int(float(row.get('m', 0)))
                k = int(float(row.get('k', 0)))
                n = int(float(row.get('n', 0)))
            except (TypeError, ValueError):
                m = k = n = 0
            if m > 0 and k > 0 and n > 0:
                matrix_sizes.setdefault(dtype, [m, k, n])
                ops = 2.0 * m * k * n
                for key in ('fwd_ms', 'bwd_ms'):
                    value = row.get(key)
                    if isinstance(value, (int, float)) and float(value) > 0:
                        gflops = ops / (float(value) * 1e6)
                        speed_samples.append(gflops)
                        speed_by_dtype.setdefault(dtype, []).append(gflops)
        if speed_samples:
            speed_geomean = math.exp(sum(math.log(v) for v in speed_samples) / len(speed_samples))
        else:
            speed_geomean = 0.0
        speed_geomean_by_dtype = {
            dtype: round(math.exp(sum(math.log(v) for v in vals) / len(vals)), 3)
            for dtype, vals in speed_by_dtype.items() if vals
        }
        core_ok = total > 0 and failed == 0 and proc.returncode == 0
        resolved = core_ok and matrix_accuracy >= 95.0
        score = round(0.5 * accuracy + 0.5 * matrix_accuracy, 3)
        emit({
            'task': 'neural-cpp',
            'ok': True,
            'score': score,
            'accuracy': accuracy,
            'coreAccuracy': accuracy,
            'matrixAccuracy': round(matrix_accuracy, 3),
            'speedGeomeanGflops': round(speed_geomean, 3),
            'speedGeomeanByDtype': speed_geomean_by_dtype,
            'matrixSizes': matrix_sizes,
            'resolved': resolved,
            'passed': passed,
            'failed': failed,
            'speedGflops': round(gflops, 3),
            'metrics': metrics,
            'stats': stats,
            'returncode': proc.returncode,
            'output': output[-4000:],
        })
        return 0 if resolved else 1


if __name__ == '__main__':
    raise SystemExit(main())
