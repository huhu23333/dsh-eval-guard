#!/usr/bin/env python3
"""Independent evaluation for dsh-eval-guard warehouse submissions.

Reads submissions/<name>/{src/*.cpp,run/,cost.json}, compiles with the fixed
g++ flags, compares every public workload byte-for-byte with the expected
output, measures CPU time against the baseline, and writes a leaderboard plus
cost/violation/static-scan reports.

The evaluator is independent of the DSH plugin: copy the run/ directory from
$DSH_HOME/eval-guard/runs/<run-id>/ next to src/.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import resource
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent
DEFAULT_KIT = ROOT.parent / "meow-ai-arena" / "warehouse" / "workspace"
WORKLOADS = ["pub_small", "pub_wide", "pub_crowded", "pub_dynamic", "pub_flood"]
TIERS = {"quick": ["pub_small"], "full": WORKLOADS, "formal": WORKLOADS}
REPEATS = {"quick": 1, "full": 2, "formal": 3}
BUILD_FLAGS = ["-O2", "-std=c++20"]
MEMORY_LIMIT = 1024 * 1024 * 1024

STATIC_PATTERNS = [
    (re.compile(r"#\s*include\s*<(fstream|filesystem|unistd\.h|windows\.h|winsock2?\.h|sys/|netinet/|arpa/|dlfcn\.h)>"), "可疑头文件"),
    (re.compile(r"\b(fopen|freopen|ifstream|ofstream|filesystem::)\b"), "文件 IO"),
    (re.compile(r"\b(system|popen|execv?p?e?|fork|posix_spawn|CreateProcess)\b"), "启动进程"),
    (re.compile(r"\b(socket|connect|bind|listen|accept|sendto|recvfrom|getaddrinfo)\b"), "网络调用"),
    (re.compile(r"\b(std::thread|pthread_create|_beginthread)\b"), "线程"),
]


@dataclass
class Participant:
    name: str
    source: Path
    kind: str
    submission: Path | None = None
    run: dict | None = None
    usage: dict | None = None
    cost: dict | None = None
    violations: list = field(default_factory=list)
    compile_error: str = ""
    outputs: dict = field(default_factory=dict)
    cpus: dict = field(default_factory=dict)
    speedups: dict = field(default_factory=dict)
    scores: dict = field(default_factory=dict)


def load_json(path: Path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return None


def normalized(data: bytes) -> bytes:
    return data.replace(b"\r\n", b"\n")


def digest(data: bytes) -> str:
    return hashlib.sha256(normalized(data)).hexdigest()


def load_submission(directory: Path) -> Participant:
    src = directory / "src"
    if not src.is_dir() or not list(src.glob("*.cpp")):
        raise RuntimeError("missing src/*.cpp")
    run_dir = directory / "run"
    run = load_json(run_dir / "run.json") or {}
    usage = load_json(run_dir / "usage.json") or {}
    violations = []
    try:
        violations = [line for line in (run_dir / "violations.jsonl").read_text(encoding="utf-8").splitlines() if line.strip()]
    except OSError:
        pass
    cost = load_json(directory / "cost.json") or load_json(run_dir / "cost.json")
    return Participant(directory.name, src, "submission", directory, run, usage, cost, violations)


def compile_sources(name: str, source: Path, out: Path) -> str:
    sources = sorted(str(p) for p in source.glob("*.cpp"))
    if not sources:
        return "no .cpp files"
    out.parent.mkdir(parents=True, exist_ok=True)
    cmd = ["g++", *BUILD_FLAGS, "-o", str(out), *sources]
    completed = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if completed.returncode != 0:
        return ("compile failed:\n" + completed.stderr[-4000:]).strip()
    return ""


def _limit_memory():
    try:
        resource.setrlimit(resource.RLIMIT_AS, (MEMORY_LIMIT, MEMORY_LIMIT))
    except (ValueError, OSError):
        pass


def run_once(exe: Path, input_path: Path, timeout: float) -> dict:
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    started = time.perf_counter()
    timed_out = False
    output = b""
    code = None
    try:
        with input_path.open("rb") as fin:
            completed = subprocess.run(
                [str(exe)], stdin=fin, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                timeout=timeout, preexec_fn=_limit_memory,
            )
        output = completed.stdout
        code = completed.returncode
    except subprocess.TimeoutExpired as error:
        timed_out = True
        output = error.stdout or b""
        code = None
    wall = time.perf_counter() - started
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    cpu = (after.ru_utime - before.ru_utime) + (after.ru_stime - before.ru_stime)
    return {"cpu": max(cpu, 1e-6), "wall": wall, "output": output, "code": code, "timed_out": timed_out}


def measure(exe: Path, input_path: Path, repeats: int, timeout: float) -> dict:
    runs = [run_once(exe, input_path, timeout) for _ in range(repeats)]
    runs.sort(key=lambda item: item["cpu"])
    return runs[len(runs) // 2]


def static_scan(source: Path) -> list[dict]:
    findings = []
    for path in sorted(list(source.glob("*.cpp")) + list(source.glob("*.h")) + list(source.glob("*.hpp"))):
        try:
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            continue
        for number, line in enumerate(lines, 1):
            for pattern, label in STATIC_PATTERNS:
                if pattern.search(line):
                    findings.append({"file": path.name, "line": number, "kind": label, "text": line.strip()[:160]})
    return findings


def price_for(participant: Participant, prices: dict) -> tuple[float, str]:
    usage = participant.usage or {}
    if not usage:
        return 0.0, "missing"
    run = participant.run or {}
    provider = str(run.get("provider") or "")
    model = str(run.get("model") or "")
    table = prices.get(provider + "/" + model) or prices.get(model) or {}
    if not table:
        return 0.0, "missing"
    def rate(name: str) -> float:
        return float(table.get(name, 0) or 0) / 1_000_000.0
    amount = float(usage.get("inputTokens", 0) or 0) * rate("input")
    amount += float(usage.get("outputTokens", 0) or 0) * rate("output")
    amount += float(usage.get("cacheReadTokens", 0) or 0) * rate("cacheRead")
    amount += float(usage.get("cacheWriteTokens", 0) or 0) * rate("cacheWrite")
    return amount, "tokens"


def money(participant: Participant, prices: dict) -> tuple[float, str, str]:
    if isinstance(participant.cost, dict):
        try:
            amount = float(participant.cost.get("total", 0) or 0)
        except (TypeError, ValueError):
            amount = 0.0
        return amount, "manual", str(participant.cost.get("currency", "USD"))
    amount, source = price_for(participant, prices)
    currency = "USD"
    return amount, source, currency


def geometric(values: list[float]) -> float:
    good = [value for value in values if value > 0]
    if not good:
        return 0.0
    return math.exp(sum(math.log(value) for value in good) / len(good))


def main() -> int:
    parser = argparse.ArgumentParser(description="warehouse-eval")
    parser.add_argument("--tier", choices=sorted(TIERS), default="quick")
    parser.add_argument("--submissions", default=str(ROOT / "submissions"))
    parser.add_argument("--out", default="")
    parser.add_argument("--kit", default=str(DEFAULT_KIT))
    parser.add_argument("--only", default="", help="comma-separated workload names")
    parser.add_argument("--repeat", type=int, default=0, help="override runs per workload")
    parser.add_argument("--timeout", type=float, default=120.0)
    args = parser.parse_args()

    kit = Path(args.kit).resolve()
    baseline_src = kit / "baseline"
    workloads_dir = kit / "workloads" / "public"
    if not baseline_src.is_dir() or not workloads_dir.is_dir():
        print("kit is missing baseline/ or workloads/public/:", kit, file=sys.stderr)
        return 2

    names = list(TIERS[args.tier])
    if args.only:
        wanted = {item.strip() for item in args.only.split(",") if item.strip()}
        names = [name for name in names if name in wanted]
    repeats = args.repeat or REPEATS[args.tier]
    if not names:
        print("no workloads selected", file=sys.stderr)
        return 2

    out_dir = Path(args.out).resolve() if args.out else ROOT / "results" / time.strftime("%Y%m%d-%H%M%S")
    out_dir.mkdir(parents=True, exist_ok=True)
    build_dir = out_dir / "build"
    prices = load_json(ROOT / "prices.json") or {}

    print("tier=%s workloads=%s repeats=%d" % (args.tier, ",".join(names), repeats))
    baseline = Participant("baseline", baseline_src, "baseline")
    baseline.compile_error = compile_sources("baseline", baseline_src, build_dir / "baseline")
    if baseline.compile_error:
        print(baseline.compile_error, file=sys.stderr)
        return 2

    baseline_cpu = {}
    expected = {}
    for name in names:
        input_path = workloads_dir / (name + ".in")
        output_path = workloads_dir / (name + ".out")
        if not input_path.is_file() or not output_path.is_file():
            print("missing workload:", name, file=sys.stderr)
            return 2
        expected[name] = normalized(output_path.read_bytes())
        result = measure(build_dir / "baseline", input_path, repeats, args.timeout)
        baseline_cpu[name] = result["cpu"]
        ok = normalized(result["output"]) == expected[name] and result["code"] == 0 and not result["timed_out"]
        print("  baseline %-14s %s %.3fs" % (name, "ok" if ok else "MISMATCH", result["cpu"]))
        baseline.outputs[name] = ok

    submissions_dir = Path(args.submissions).resolve()
    participants: list[Participant] = []
    if submissions_dir.is_dir():
        for directory in sorted(path for path in submissions_dir.iterdir() if path.is_dir()):
            try:
                participant = load_submission(directory)
            except Exception as error:
                print("skip %s: %s" % (directory.name, error))
                continue
            participant.compile_error = compile_sources(participant.name, participant.source, build_dir / participant.name)
            if participant.compile_error:
                print("  %-14s compile failed" % participant.name)
            participants.append(participant)
    if not participants:
        print("no submissions under", submissions_dir)
        return 1

    for participant in participants:
        if participant.compile_error:
            continue
        exe = build_dir / participant.name
        for name in names:
            result = measure(exe, workloads_dir / (name + ".in"), repeats, args.timeout)
            correct = (
                not result["timed_out"] and result["code"] == 0
                and normalized(result["output"]) == expected[name]
            )
            participant.outputs[name] = correct
            participant.cpus[name] = result["cpu"]
            participant.speedups[name] = (baseline_cpu[name] / result["cpu"]) if correct else 0.0
            print("  %-14s %-14s %s %.3fs %.1fx" % (
                participant.name, name, "ok" if correct else "WRONG",
                result["cpu"], participant.speedups[name],
            ))

    for name in names:
        best = max([1.0] + [participant.speedups.get(name, 0.0) for participant in participants])
        for participant in participants:
            speedup = participant.speedups.get(name, 0.0)
            if participant.compile_error or speedup <= 0 or best <= 1.0:
                participant.scores[name] = 0.0
            else:
                participant.scores[name] = 100.0 * math.log(speedup) / math.log(best)

    def summary_row(participant: Participant) -> dict:
        amount, cost_source, currency = money(participant, prices)
        correct = sum(1 for name in names if participant.outputs.get(name))
        speedup = geometric([participant.speedups.get(name, 0.0) for name in names])
        score = sum(participant.scores.get(name, 0.0) for name in names) / len(names)
        scan = static_scan(participant.source)
        return {
            "name": participant.name,
            "kind": participant.kind,
            "score": round(score, 2),
            "correct": correct,
            "total": len(names),
            "geomeanSpeedup": round(speedup, 2),
            "cost": round(amount, 4),
            "costSource": cost_source,
            "currency": currency,
            "violations": len(participant.violations),
            "static": scan,
            "compileError": participant.compile_error,
        }

    rows = [summary_row(participant) for participant in participants]
    rows.sort(key=lambda row: (-row["score"], -row["correct"], -row["geomeanSpeedup"], row["name"]))
    payload = {
        "generatedAt": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "tier": args.tier,
        "workloads": names,
        "repeats": repeats,
        "baseline": {"name": "baseline", "cpu": {name: round(value, 6) for name, value in baseline_cpu.items()}},
        "results": rows,
    }
    (out_dir / "summary.json").write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    lines = [
        "# warehouse-eval 结果",
        "",
        "- 时间：%s" % payload["generatedAt"],
        "- 档位：%s（%d 次/负载，取中位数）" % (args.tier, repeats),
        "- 负载：%s" % ", ".join(names),
        "",
        "## 排行榜",
        "",
        "| 名次 | 名称 | 得分 | 正确 | 几何平均加速 | 成本 | 价格来源 | 违规 | 静态扫描 |",
        "|---:|---|---:|---:|---:|---:|---|---:|---|",
    ]
    for rank, row in enumerate(rows, 1):
        scan = "未发现" if not row["static"] else "%d 项" % len(row["static"])
        lines.append("| %d | %s | %.1f | %d/%d | %.2fx | %.4f %s | %s | %d | %s |" % (
            rank, row["name"], row["score"], row["correct"], row["total"], row["geomeanSpeedup"],
            row["cost"], row["currency"], row["costSource"], row["violations"], scan,
        ))
    lines += ["", "## 逐负载", "", "| 名称 | 负载 | 结果 | 加速比 | 得分 |", "|---|---|---:|---:|---:|"]
    for participant in participants:
        for name in names:
            lines.append("| %s | %s | %s | %.2fx | %.1f |" % (
                participant.name, name, "正确" if participant.outputs.get(name) else "错误",
                participant.speedups.get(name, 0.0), participant.scores.get(name, 0.0),
            ))
    lines += ["", "## 说明", "", "- 基准始终参加计时；正确性以 workloads/public/*.out 的逐字节比较为准（CRLF 视为 LF）。", "- 只有正确且比基线快的提交能拿加速分；得分按每条负载所有提交中的最高加速比归一化，再取平均。", "- 静态扫描只报告，不取消资格。", "- 没有数据时成本为 0 并标记来源 missing。"]
    (out_dir / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")

    with (out_dir / "workloads.csv").open("w", encoding="utf-8") as handle:
        handle.write("name,workload,correct,baselineCpu,candidateCpu,speedup,score\n")
        for participant in participants:
            for name in names:
                handle.write("%s,%s,%s,%.6f,%.6f,%.3f,%.2f\n" % (
                    participant.name, name, int(bool(participant.outputs.get(name))),
                    baseline_cpu[name], participant.cpus.get(name, 0.0), participant.speedups.get(name, 0.0), participant.scores.get(name, 0.0),
                ))
    print("wrote", out_dir / "summary.md")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
