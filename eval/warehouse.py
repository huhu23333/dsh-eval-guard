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
    if not row:
        raise SystemExit(fail({'task': 'warehouse', 'ok': False, 'error': 'candidate row missing from evaluator summary'}))
    compile_error = row.get('compileError') or ''
    correct = row.get('correct', 0)
    total = row.get('total', 0)
    status = 'compile-failed' if compile_error else ('ok' if correct == total and total else 'incorrect')
    emit({
        'task': 'warehouse',
        'ok': True,
        'score': row.get('score', 0),
        'speedup': row.get('geomeanSpeedup', 0),
        'geomeanSpeedup': row.get('geomeanSpeedup', 0),
        'correct': correct,
        'total': total,
        'status': status,
        'error': compile_error,
    })
