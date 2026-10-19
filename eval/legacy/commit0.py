#!/usr/bin/env python3
"""Evaluate Commit0 single-library submissions.

Submission layout:
  submissions/<name>/workspace/   (copy of the agent workspace, including .venv)

Score is the unit-test pass rate. Missing run/ statistics are skipped.
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
SUMMARY = re.compile(r"(\d+) passed")
FAILED = re.compile(r"(\d+) failed")
ERRORS = re.compile(r"(\d+) error")


def load_json(path: Path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return None


def find_workspace(directory: Path) -> Path | None:
    for candidate in (directory / "workspace", directory):
        if (candidate / "tests").is_dir() and (candidate / "TASK.md").is_file():
            return candidate
    return None


def run_tests(workspace: Path) -> tuple[int, int, int, str]:
    venv = workspace / ".venv" / "bin" / "python"
    python = str(venv) if venv.is_file() else "python3"
    env = {k: v for k, v in __import__("os").environ.items() if k not in {"PYTHONPATH", "PYTHONHOME"}}
    completed = subprocess.run([python, "-m", "pytest", "tests", "-q", "--tb=no"], cwd=workspace,
                               capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=1800, env=env)
    output = (completed.stdout or "") + "\n" + (completed.stderr or "")
    passed = int(SUMMARY.search(output).group(1)) if SUMMARY.search(output) else 0
    failed = int(FAILED.search(output).group(1)) if FAILED.search(output) else 0
    errors = int(ERRORS.search(output).group(1)) if ERRORS.search(output) else 0
    if completed.returncode != 0 and passed == 0 and failed == 0 and errors == 0:
        errors = 1
    return passed, failed, errors, output[-3000:]


def main() -> int:
    parser = argparse.ArgumentParser(description="commit0-eval")
    parser.add_argument("--submissions", default=str(ROOT / "submissions"))
    parser.add_argument("--out", default="")
    args = parser.parse_args()
    root = Path(args.submissions).resolve()
    directories = [p for p in sorted(root.iterdir()) if p.is_dir()] if root.is_dir() else []
    if not directories:
        print("no submissions under", root, file=sys.stderr)
        return 2
    out = Path(args.out).resolve() if args.out else ROOT / "results" / time.strftime("%Y%m%d-%H%M%S")
    out.mkdir(parents=True, exist_ok=True)
    rows = []
    for directory in directories:
        workspace = find_workspace(directory)
        if workspace is None:
            rows.append({"name": directory.name, "status": "missing-workspace", "score": 0.0, "passed": 0, "failed": 0, "errors": 0})
            continue
        try:
            passed, failed, errors, detail = run_tests(workspace)
            total = passed + failed + errors
            score = 100.0 * passed / total if total else 0.0
            rows.append({"name": directory.name, "status": "ok", "score": round(score, 2), "passed": passed, "failed": failed, "errors": errors, "detail": detail})
        except Exception as error:
            rows.append({"name": directory.name, "status": "error", "score": 0.0, "passed": 0, "failed": 0, "errors": 1, "detail": str(error)})
    rows.sort(key=lambda row: (-row["score"], row["name"]))
    payload = {"generatedAt": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "benchmark": "commit0", "instance": "commit-0/tinydb", "results": rows}
    (out / "summary.json").write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    lines = ["# Commit0 · tinydb 排行", "", "- 指标：单元测试通过率（passed / passed+failed+errors）", "", "| 名次 | 名称 | 得分 | 通过 | 失败 | 错误 | 状态 |", "|---:|---|---:|---:|---:|---:|---|"]
    for rank, row in enumerate(rows, 1):
        lines.append("| %d | %s | %.1f | %d | %d | %d | %s |" % (rank, row["name"], row["score"], row["passed"], row["failed"], row["errors"], row["status"]))
    (out / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("wrote", out / "summary.md")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
