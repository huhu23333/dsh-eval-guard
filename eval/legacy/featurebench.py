#!/usr/bin/env python3
"""Evaluate a FeatureBench single-feature submission.

Submission layout:
  submissions/<name>/workspace/   (copy of the agent workspace, including .venv)

The hidden feature test file is copied from the plugin asset; scoring checks the
fail-to-pass file and every pass-to-pass file explicitly (a skipped or removed
test does not count as resolved).
"""
from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
ASSET = ROOT.parents[1] / "assets" / "featurebench"
PASSED = re.compile(r"(\d+) passed")
FAILED = re.compile(r"(\d+) failed")
ERRORS = re.compile(r"(\d+) error")
RESULT = re.compile(r"^(PASSED|FAILED|ERROR|SKIPPED|XFAIL|XPASS) (\S+)", re.MULTILINE)


def load_json(path: Path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return None


def find_workspace(directory: Path) -> Path | None:
    for candidate in (directory / "workspace", directory):
        if (candidate / "seaborn").is_dir() and (candidate / "tests").is_dir():
            return candidate
    return None


def install_hidden_test(workspace: Path) -> str:
    source = ASSET / "hidden" / "test_regression.py"
    if not source.is_file():
        return "hidden regression test is missing"
    shutil.copy2(source, workspace / "tests" / "test_regression.py")
    return ""


def matches(node: str, selectors: list[str]) -> bool:
    for selector in selectors:
        if node == selector or node.startswith(selector + "::") or node.startswith(selector + "/"):
            return True
    return False


def run_tests(workspace: Path, instance: dict) -> tuple[int, int, int, str, dict]:
    venv = workspace / ".venv" / "bin" / "python"
    python = str(venv) if venv.is_file() else "python3"
    fail_to_pass = list(instance.get("FAIL_TO_PASS", []))
    pass_to_pass = list(instance.get("PASS_TO_PASS", []))
    tests = fail_to_pass + pass_to_pass
    env = {k: v for k, v in __import__("os").environ.items() if k not in {"PYTHONPATH", "PYTHONHOME"}}
    completed = subprocess.run([python, "-m", "pytest", *tests, "-q", "--tb=no", "-rA"], cwd=workspace,
                               capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=1800, env=env)
    output = (completed.stdout or "") + "\n" + (completed.stderr or "")
    passed = int(PASSED.search(output).group(1)) if PASSED.search(output) else 0
    failed = int(FAILED.search(output).group(1)) if FAILED.search(output) else 0
    errors = int(ERRORS.search(output).group(1)) if ERRORS.search(output) else 0
    if completed.returncode != 0 and passed == 0 and failed == 0 and errors == 0:
        errors = 1
    outcomes = {node: status for status, node in RESULT.findall(output)}
    f2p_total = f2p_passed = 0
    p2p_total = p2p_passed = 0
    for node, status in outcomes.items():
        if matches(node, fail_to_pass):
            f2p_total += 1
            f2p_passed += 1 if status == "PASSED" else 0
        elif matches(node, pass_to_pass):
            p2p_total += 1
            p2p_passed += 1 if status == "PASSED" else 0
    resolved = (
        len(fail_to_pass) > 0
        and f2p_total > 0
        and f2p_passed == f2p_total
        and p2p_total > 0
        and p2p_passed == p2p_total
        and completed.returncode == 0
    )
    return passed, failed, errors, output[-3000:], {
        "resolved": resolved,
        "f2p_total": f2p_total,
        "f2p_passed": f2p_passed,
        "p2p_total": p2p_total,
        "p2p_passed": p2p_passed,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="featurebench-eval")
    parser.add_argument("--submissions", default=str(ROOT / "submissions"))
    parser.add_argument("--out", default="")
    args = parser.parse_args()
    instance = load_json(ASSET / "instance.json") or {}
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
            rows.append({"name": directory.name, "status": "missing-workspace", "score": 0.0, "passed": 0, "failed": 0, "errors": 0, "resolved": False})
            continue
        try:
            hidden_error = install_hidden_test(workspace)
            if hidden_error:
                rows.append({"name": directory.name, "status": "hidden-test-failed", "score": 0.0, "passed": 0, "failed": 0, "errors": 1, "resolved": False, "detail": hidden_error})
                continue
            passed, failed, errors, detail, extra = run_tests(workspace, instance)
            total = passed + failed + errors
            score = 100.0 * passed / total if total else 0.0
            rows.append({"name": directory.name, "status": "ok", "score": round(score, 2), "passed": passed, "failed": failed, "errors": errors, "detail": detail, **extra})
        except Exception as error:
            rows.append({"name": directory.name, "status": "error", "score": 0.0, "passed": 0, "failed": 0, "errors": 1, "resolved": False, "detail": str(error)})
    rows.sort(key=lambda row: (-row["score"], row["name"]))
    payload = {"generatedAt": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "benchmark": "featurebench", "instance": instance.get("instance_id"), "results": rows}
    (out / "summary.json").write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    lines = ["# FeatureBench · 单 feature 排行", "", "- 实例：%s" % instance.get("instance_id", ""), "- 指标：隐藏 feature 测试与全部回归文件通过才 resolved", "", "| 名次 | 名称 | 得分 | 通过 | 失败 | 错误 | 状态 |", "|---:|---|---:|---:|---:|---:|---|"]
    for rank, row in enumerate(rows, 1):
        lines.append("| %d | %s | %.1f | %d | %d | %d | %s |" % (rank, row["name"], row["score"], row["passed"], row["failed"], row["errors"], row["status"]))
    (out / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("wrote", out / "summary.md")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
