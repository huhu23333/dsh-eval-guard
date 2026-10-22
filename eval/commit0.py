#!/usr/bin/env python3
"""Single-workspace Commit0 evaluator wrapper."""
from __future__ import annotations
import os, tempfile
from pathlib import Path
from common import EVAL_ROOT, candidate_row, clean_copy, emit, fail, parse_workspace, restore_baseline, run_legacy, verify_venv

workspace = parse_workspace()
problem = verify_venv(workspace)
if problem is not None:
    raise SystemExit(fail({'task': 'commit0', 'ok': False, 'error': problem}))
with tempfile.TemporaryDirectory(prefix='commit0-eval-') as td:
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
    result = run_legacy(EVAL_ROOT / 'legacy' / 'commit0.py', ['--submissions', str(temp)], temp / 'out')
    if not result.get('ok'):
        raise SystemExit(fail({'task': 'commit0', 'ok': False, 'error': result.get('error', 'evaluation failed')}))
    row = candidate_row(result['summary'])
    emit({'task': 'commit0', 'ok': True, 'score': row.get('score', 0), 'passed': row.get('passed', 0), 'failed': row.get('failed', 0), 'errors': row.get('errors', 0), 'status': row.get('status')})
