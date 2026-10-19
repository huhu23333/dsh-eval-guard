#!/usr/bin/env python3
"""Single-workspace warehouse evaluator wrapper."""
from __future__ import annotations
import shutil, tempfile
from pathlib import Path
from common import EVAL_ROOT, candidate_row, emit, fail, parse_workspace, run_legacy

workspace = parse_workspace()
with tempfile.TemporaryDirectory(prefix='warehouse-eval-') as td:
    temp = Path(td)
    src = workspace / 'src'
    if not src.is_dir():
        raise SystemExit(fail({'task': 'warehouse', 'ok': False, 'error': 'missing src/ directory'}))
    (temp / 'candidate').mkdir()
    shutil.copytree(src, temp / 'candidate' / 'src')
    result = run_legacy(EVAL_ROOT / 'legacy' / 'warehouse.py', ['--submissions', str(temp), '--kit', str(EVAL_ROOT / 'arena' / 'warehouse' / 'workspace'), '--tier', 'quick'], temp / 'out')
    if not result.get('ok'):
        raise SystemExit(fail({'task': 'warehouse', 'ok': False, 'error': result.get('error', 'evaluation failed')}))
    row = candidate_row(result['summary'])
    emit({'task': 'warehouse', 'ok': True, 'score': row.get('speedup', 0), 'speedup': row.get('speedup', 0), 'correct': row.get('correct'), 'status': row.get('status')})
