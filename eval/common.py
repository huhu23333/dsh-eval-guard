"""Shared helpers for single-workspace evaluator wrappers."""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

EVAL_ROOT = Path(__file__).resolve().parent


def parse_workspace() -> Path:
    parser = argparse.ArgumentParser(description='single-workspace evaluator')
    parser.add_argument('--workspace', required=True)
    args = parser.parse_args()
    return Path(args.workspace).resolve()


def emit(payload: dict) -> None:
    target = os.environ.get('EVAL_RESULT_FILE')
    if target is None or target == '':
        print(json.dumps(payload, ensure_ascii=False, indent=2))
        return
    Path(target).write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding='utf-8')


def clean_copy(source: Path, target: Path, ignore_venv: bool = False) -> None:
    def ignore(directory: str, names: list[str]) -> set[str]:
        if ignore_venv and Path(directory) == source:
            return {name for name in names if name == '.venv'}
        return set()
    shutil.copytree(source, target, symlinks=True, ignore=ignore)


def run_legacy(script: Path, args: list[str], out: Path) -> dict:
    cmd = [sys.executable, '-X', 'utf8', str(script), *args, '--out', str(out)]
    env = {k: v for k, v in os.environ.items() if k not in {'PYTHONPATH', 'PYTHONHOME'}}
    completed = subprocess.run(cmd, cwd=str(EVAL_ROOT), capture_output=True, text=True, encoding='utf-8', errors='replace', env=env, timeout=1800)
    if completed.returncode != 0:
        return {'ok': False, 'error': (completed.stderr or completed.stdout or 'evaluator failed')[-4000:]}
    summary = out / 'summary.json'
    if not summary.is_file():
        return {'ok': False, 'error': 'summary.json missing'}
    return {'ok': True, 'summary': json.loads(summary.read_text(encoding='utf-8'))}


def candidate_row(summary: dict, name: str = 'candidate') -> dict:
    rows = summary.get('results') or summary.get('participants') or summary.get('rows') or []
    for row in rows:
        if row.get('name') == name:
            return row
    return {}


def fail(payload: dict) -> int:
    emit(payload)
    return 1
