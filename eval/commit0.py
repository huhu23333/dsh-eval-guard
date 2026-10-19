#!/usr/bin/env python3
"""Single-workspace Commit0 evaluator wrapper."""
from __future__ import annotations
import os, shutil, tempfile
from pathlib import Path
from common import EVAL_ROOT, candidate_row, clean_copy, emit, fail, parse_workspace, run_legacy

workspace = parse_workspace()
with tempfile.TemporaryDirectory(prefix='commit0-eval-') as td:
    temp = Path(td)
    candidate = temp / 'candidate' / 'workspace'
    candidate.parent.mkdir(parents=True)
    clean_copy(workspace, candidate, ignore_venv=True)
    venv = workspace / '.venv'
    if venv.is_dir():
        os.symlink(venv, candidate / '.venv')
    result = run_legacy(EVAL_ROOT / 'legacy' / 'commit0.py', ['--submissions', str(temp)], temp / 'out')
    if not result.get('ok'):
        raise SystemExit(fail({'task': 'commit0', 'ok': False, 'error': result.get('error', 'evaluation failed')}))
    row = candidate_row(result['summary'])
    emit({'task': 'commit0', 'ok': True, 'score': row.get('score', 0), 'passed': row.get('passed', 0), 'failed': row.get('failed', 0), 'errors': row.get('errors', 0), 'status': row.get('status')})
