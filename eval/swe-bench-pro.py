#!/usr/bin/env python3
"""Single-workspace SWE-Bench Pro evaluator wrapper."""
from __future__ import annotations
import os, tempfile
from pathlib import Path
from common import EVAL_ROOT, candidate_row, clean_copy, emit, fail, parse_workspace, restore_baseline, run_legacy, verify_venv

workspace = parse_workspace()
problem = verify_venv(workspace)
if problem is not None:
    raise SystemExit(fail({'task': 'swe-bench-pro', 'ok': False, 'error': problem}))
with tempfile.TemporaryDirectory(prefix='swe-pro-eval-') as td:
    temp = Path(td)
    candidate = temp / 'candidate' / 'workspace'
    candidate.parent.mkdir(parents=True)
    clean_copy(workspace, candidate, ignore_venv=True)
    venv = workspace / '.venv'
    if venv.is_dir():
        try:
            os.symlink(venv, candidate / '.venv')
        except FileExistsError:
            pass
    restore_baseline(candidate)
    result = run_legacy(EVAL_ROOT / 'legacy' / 'swe_bench_pro.py', ['--submissions', str(temp)], temp / 'out')
    if not result.get('ok'):
        raise SystemExit(fail({'task': 'swe-bench-pro', 'ok': False, 'error': result.get('error', 'evaluation failed')}))
    row = candidate_row(result['summary'])
    resolved = row.get('resolved') is True
    emit({'task': 'swe-bench-pro', 'ok': True, 'score': 100.0 if resolved else 0.0, 'resolved': resolved, 'passRate': row.get('score', 0), 'passed': row.get('passed', 0), 'failed': row.get('failed', 0), 'errors': row.get('errors', 0), 'f2pPassed': row.get('f2p_passed', 0), 'f2pTotal': row.get('f2p_total', 0), 'status': row.get('status')})
