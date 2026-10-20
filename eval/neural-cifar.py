#!/usr/bin/env python3
"""Plugin wrapper for the standalone Neural-CIFAR evaluator."""
from __future__ import annotations
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
STANDALONE = HERE.parent / 'neural-cifar-eval' / 'run_eval.py'

if not STANDALONE.is_file():
    print('neural-cifar-eval/run_eval.py missing; copy the standalone evaluator folder first', file=sys.stderr)
    raise SystemExit(1)

os.execv(sys.executable, [sys.executable, '-X', 'utf8', str(STANDALONE), *sys.argv[1:]])
