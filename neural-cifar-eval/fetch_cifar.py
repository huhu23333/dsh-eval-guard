#!/usr/bin/env python3
"""Fetch the hidden CIFAR-10 validation split via the shared locked cache."""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', default=str(HERE / 'hidden'))
    parser.add_argument('--proxy', default=os.environ.get('EVAL_CIFAR_PROXY', ''))
    parser.add_argument('--url', default=os.environ.get('EVAL_CIFAR_URL', ''))
    args = parser.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    cmd = [sys.executable, '-X', 'utf8', str(HERE / 'cifar_cache.py'), '--copy-val', str(out)]
    if args.proxy:
        cmd += ['--proxy', args.proxy]
    if args.url:
        cmd += ['--url', args.url]
    return subprocess.run(cmd, cwd=str(HERE)).returncode


if __name__ == '__main__':
    raise SystemExit(main())
