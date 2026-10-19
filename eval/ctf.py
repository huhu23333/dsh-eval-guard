#!/usr/bin/env python3
"""Single-workspace tank evaluator wrapper."""
from __future__ import annotations
import shutil, tempfile
from pathlib import Path
from common import EVAL_ROOT, candidate_row, emit, fail, parse_workspace, run_legacy

workspace = parse_workspace()
with tempfile.TemporaryDirectory(prefix='ctf-eval-') as td:
    temp = Path(td)
    bot = workspace / 'submission' / 'bot.cpp'
    if not bot.is_file():
        raise SystemExit(fail({'task': 'ctf', 'ok': False, 'error': 'missing submission/bot.cpp'}))
    (temp / 'candidate').mkdir()
    shutil.copy2(bot, temp / 'candidate' / 'bot.cpp')
    result = run_legacy(EVAL_ROOT / 'legacy' / 'ctf.py', ['--submissions', str(temp), '--task', str(EVAL_ROOT / 'arena' / 'ctf'), '--tier', 'quick'], temp / 'out')
    if not result.get('ok'):
        raise SystemExit(fail({'task': 'ctf', 'ok': False, 'error': result.get('error', 'evaluation failed')}))
    row = candidate_row(result['summary'])
    emit({'task': 'ctf', 'ok': True, 'score': row.get('score', row.get('rating', row.get('theta', 0))), 'rating': row.get('rating', row.get('theta', 0)), 'wins': row.get('wins', 0), 'losses': row.get('losses', 0), 'draws': row.get('draws', 0)})
