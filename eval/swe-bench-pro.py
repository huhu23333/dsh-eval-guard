#!/usr/bin/env python3
"""Single-workspace SWE-Bench Pro evaluator wrapper."""
from __future__ import annotations
import os, tempfile
from pathlib import Path
from common import EVAL_ROOT, candidate_row, clean_copy, emit, fail, parse_workspace, run_legacy

workspace = parse_workspace()
with tempfile.TemporaryDirectory(prefix='swe-pro-eval-') as td:
    temp = Path(td)
    candidate = temp / 'candidate' / 'workspace'
    candidate.parent.mkdir(parents=True)
    clean_copy(workspace, candidate, ignore_venv=True)
    venv = workspace / '.venv'
    if venv.is_dir():
        os.symlink(venv, candidate / '.venv')
    result = run_legacy(EVAL_ROOT / 'legacy' / 'swe_bench_pro.py', ['--submissions', str(temp)], temp / 'out')
    if not result.get('ok'):
        raise SystemExit(fail({'task': 'swe-bench-pro', 'ok': False, 'error': result.get('error', 'evaluation failed')}))
    row = candidate_row(result['summary'])
    passed = row.get('passed', 0)
    failed = row.get('failed', 0)
    errors = row.get('errors', 0)
    resolved = failed == 0 and errors == 0 and passed > 0
    emit({'task': 'swe-bench-pro', 'ok': True, 'score': 100.0 if resolved else 0.0, 'resolved': resolved, 'passRate': row.get('score', 0), 'passed': passed, 'failed': failed, 'errors': errors, 'status': row.get('status')})
