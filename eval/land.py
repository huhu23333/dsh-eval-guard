#!/usr/bin/env python3
"""Single-workspace land evaluator wrapper."""
from __future__ import annotations
import shutil, tempfile
from pathlib import Path
from common import EVAL_ROOT, candidate_row, emit, fail, parse_workspace, run_legacy

workspace = parse_workspace()
with tempfile.TemporaryDirectory(prefix='land-eval-') as td:
    temp = Path(td)
    bot = workspace / 'submission' / 'bot.cpp'
    if not bot.is_file():
        raise SystemExit(fail({'task': 'land', 'ok': False, 'error': 'missing submission/bot.cpp'}))
    (temp / 'candidate').mkdir()
    shutil.copy2(bot, temp / 'candidate' / 'bot.cpp')
    result = run_legacy(EVAL_ROOT / 'legacy' / 'land.py', ['--submissions', str(temp), '--task', str(EVAL_ROOT / 'arena' / 'land'), '--tier', 'quick'], temp / 'out')
    if not result.get('ok'):
        raise SystemExit(fail({'task': 'land', 'ok': False, 'error': result.get('error', 'evaluation failed')}))
    row = candidate_row(result['summary'])
    if not row:
        raise SystemExit(fail({'task': 'land', 'ok': False, 'error': 'candidate row missing from evaluator summary'}))
    emit({'task': 'land', 'ok': True, 'score': row.get('score', row.get('rating', 0)), 'rating': row.get('rating', row.get('theta', 0)), 'theta': row.get('theta', 0)})
