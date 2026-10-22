"""Shared helpers for single-workspace evaluator wrappers."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

EVAL_ROOT = Path(__file__).resolve().parent

# Variables the host injects for the evaluator itself. They must not leak into
# the legacy evaluator or into any candidate process it starts (a candidate
# that learns EVAL_RESULT_FILE could overwrite the result).
HOST_EVAL_KEYS = frozenset({
    'EVAL_RESULT_FILE', 'EVAL_JOB_DIR', 'EVAL_INSTALL_TOKEN',
    'EVAL_BASELINE_DIR', 'EVAL_VENV_MANIFEST',
})


def child_env() -> dict[str, str]:
    """Environment for trusted legacy evaluators and their candidate children."""
    return {
        key: value for key, value in os.environ.items()
        if key not in HOST_EVAL_KEYS and key not in {'PYTHONPATH', 'PYTHONHOME'}
    }


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


def clean_copy(source: Path, target: Path, ignore_venv: bool = False,
               ignore_names: tuple[str, ...] = ()) -> None:
    ignored = set(ignore_names)

    def ignore(directory: str, names: list[str]) -> set[str]:
        if Path(directory) == source:
            skip = set(ignored)
            if ignore_venv:
                skip.add('.venv')
            return {name for name in names if name in skip}
        return set()
    shutil.copytree(source, target, symlinks=True, ignore=ignore)


def run_legacy(script: Path, args: list[str], out: Path) -> dict:
    cmd = [sys.executable, '-X', 'utf8', str(script), *args, '--out', str(out)]
    completed = subprocess.run(cmd, cwd=str(EVAL_ROOT), capture_output=True, text=True, encoding='utf-8', errors='replace', env=child_env(), timeout=1800)
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


def parse_pytest_results(output: str) -> dict[str, str]:
    """Map pytest node ids to PASSED/FAILED/ERROR/SKIPPED from -rA output."""
    import re
    pattern = re.compile(r'^(PASSED|FAILED|ERROR|SKIPPED|XFAIL|XPASS) (\S+)', re.MULTILINE)
    results: dict[str, str] = {}
    for match in pattern.finditer(output):
        results[match.group(2)] = match.group(1)
    return results


def matches_node(node: str, selector: str) -> bool:
    """Whether a pytest node id belongs to a file/node selector."""
    if node == selector:
        return True
    return node.startswith(selector + '::') or node.startswith(selector + '/')



# ---------------------------------------------------------------------------
# Pristine test material and venv integrity
# ---------------------------------------------------------------------------

def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    digest.update(path.read_bytes())
    return digest.hexdigest()


def sha256_tree(root: Path) -> str:
    digest = hashlib.sha256()

    def walk(current: Path) -> None:
        for entry in sorted(current.iterdir(), key=lambda item: item.name):
            digest.update(str(entry.relative_to(root)).encode('utf-8'))
            if entry.is_dir():
                walk(entry)
            elif entry.is_file():
                digest.update(entry.read_bytes())
            elif entry.is_symlink():
                digest.update(b'link:' + os.readlink(entry).encode('utf-8'))

    walk(root)
    return digest.hexdigest()


def baseline_dir() -> Path | None:
    raw = os.environ.get('EVAL_BASELINE_DIR', '')
    if raw == '':
        return None
    path = Path(raw)
    return path if path.is_dir() else None


def restore_baseline(candidate: Path) -> None:
    """Overwrite candidate test material with the pristine install-time copy.

    Also removes conftest.py files the agent added (the pristine snapshot keeps
    the project's own ones), so a workspace hook cannot rewrite pytest results.
    """
    baseline = baseline_dir()
    if baseline is None:
        return
    candidate = candidate.resolve()
    baseline = baseline.resolve()
    for hook in candidate.rglob('conftest.py'):
        if hook.is_symlink():
            hook.unlink()
            continue
        try:
            relative = hook.relative_to(candidate)
        except ValueError:
            continue
        if not (baseline / relative).is_file():
            hook.unlink()
    for entry in baseline.iterdir():
        if entry.name in {'manifest.json', 'venv.json'}:
            continue
        destination = candidate / entry.name
        if destination.exists() or destination.is_symlink():
            if destination.is_dir() and not destination.is_symlink():
                shutil.rmtree(destination, ignore_errors=True)
            else:
                destination.unlink()
        if entry.is_dir():
            shutil.copytree(entry, destination, symlinks=True)
        else:
            shutil.copy2(entry, destination)


def verify_venv(workspace: Path) -> str | None:
    """Verify the venv control plane against the install-time fingerprint.

    Returns a human-readable reason when the venv was tampered with, else None.
    """
    baseline = baseline_dir()
    if baseline is None:
        return None
    manifest_path = baseline / 'venv.json'
    if not manifest_path.is_file():
        return None
    try:
        manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    except Exception:
        return 'venv manifest is corrupt'
    venv = workspace / '.venv'
    if not venv.is_dir():
        return 'the evaluator requires the installed .venv; it is missing'
    expected_python = manifest.get('python')
    if isinstance(expected_python, str) and expected_python != '':
        python = venv / 'bin' / 'python'
        try:
            actual = str(python.resolve())
        except OSError:
            actual = ''
        if actual != expected_python:
            return 'the .venv python interpreter was replaced'
    for relative, expected in (manifest.get('files') or {}).items():
        target = venv / relative
        if not target.is_file():
            return f'the .venv file {relative} is missing'
        try:
            actual = sha256_file(target)
        except OSError as error:
            return f'cannot read the .venv file {relative}: {error}'
        if actual != expected:
            return f'the .venv file {relative} was modified'
    expected_pytest = manifest.get('pytest')
    if isinstance(expected_pytest, str) and expected_pytest != '':
        found = False
        for site in venv.glob('lib*/python*/site-packages/pytest'):
            found = True
            try:
                actual = sha256_tree(site)
            except OSError as error:
                return f'cannot read the venv pytest package: {error}'
            if actual != expected_pytest:
                return 'the venv pytest package was modified'
        if not found:
            return 'the venv pytest package disappeared'
    return None
