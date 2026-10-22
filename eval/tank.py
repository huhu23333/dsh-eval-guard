#!/usr/bin/env python3
"""Single-workspace tank evaluator wrapper."""
from __future__ import annotations
import shutil, tempfile
from pathlib import Path
from common import EVAL_ROOT, candidate_row, emit, fail, parse_workspace, run_legacy

workspace = parse_workspace()
with tempfile.TemporaryDirectory(prefix='tank-eval-') as td:
    temp = Path(td)
    bot = workspace / 'submission' / 'bot.cpp'
    if not bot.is_file():
        raise SystemExit(fail({'task': 'tank', 'ok': False, 'error': 'missing submission/bot.cpp'}))
    (temp / 'candidate').mkdir()
    shutil.copy2(bot, temp / 'candidate' / 'bot.cpp')
    result = run_legacy(EVAL_ROOT / 'legacy' / 'tank.py', ['--submissions', str(temp), '--arena', str(EVAL_ROOT / 'arena' / 'tank'), '--tier', 'quick'], temp / 'out')
    if not result.get('ok'):
        raise SystemExit(fail({'task': 'tank', 'ok': False, 'error': result.get('error', 'evaluation failed')}))
    summary = result['summary']
    row = candidate_row(summary)
    if not row:
        raise SystemExit(fail({'task': 'tank', 'ok': False, 'error': 'candidate row missing from evaluator summary'}))
    wins = draws = losses = 0
    for pair in summary.get('pairwise') or []:
        if pair.get('a') == 'candidate':
            wins += int(pair.get('wins', 0)); draws += int(pair.get('draws', 0)); losses += int(pair.get('losses', 0))
        elif pair.get('b') == 'candidate':
            wins += int(pair.get('losses', 0)); draws += int(pair.get('draws', 0)); losses += int(pair.get('wins', 0))
    emit({'task': 'tank', 'ok': True, 'score': row.get('rating', 0), 'rating': row.get('rating', 0), 'wins': wins, 'losses': losses, 'draws': draws, 'games': row.get('games', 0)})
