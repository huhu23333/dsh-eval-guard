#!/usr/bin/env python3
"""Independent evaluation for bomberman bot submissions.

Reads submissions/<name>/{bot.cpp,run/,cost.json}, then runs the official
tournament/run.py with the baseline anchor and all submissions. A missing
run/ directory is fine: only the bot is evaluated.
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
DEFAULT_TASK = ROOT.parent / "meow-ai-arena" / "bomberman"
TIERS = {"quick": (["2"], 1), "full": (["2", "5", "10"], 1), "formal": (["2", "5", "10"], 3)}
STATIC_PATTERNS = [
    (re.compile(r"#\s*include\s*<(fstream|filesystem|unistd\.h|windows\.h|winsock2?\.h|sys/|netinet/|arpa/|dlfcn\.h)>"), "可疑头文件"),
    (re.compile(r"\b(fopen|freopen|ifstream|ofstream|filesystem::)\b"), "文件 IO"),
    (re.compile(r"\b(system|popen|execv?p?e?|fork|posix_spawn|CreateProcess)\b"), "启动进程"),
    (re.compile(r"\b(socket|connect|bind|listen|accept|sendto|recvfrom|getaddrinfo)\b"), "网络调用"),
    (re.compile(r"\b(std::thread|pthread_create|_beginthread)\b"), "线程"),
]


def load_json(path: Path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return None


def static_scan(source: Path) -> list:
    findings = []
    for number, line in enumerate(source.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        for pattern, label in STATIC_PATTERNS:
            if pattern.search(line):
                findings.append({"line": number, "kind": label, "text": line.strip()[:160]})
    return findings


def money(directory: Path, run: dict, usage: dict, cost: dict, prices: dict):
    if isinstance(cost, dict):
        try:
            return float(cost.get("total", 0) or 0), "manual", str(cost.get("currency", "USD"))
        except (TypeError, ValueError):
            pass
    if not usage:
        return 0.0, "missing", "USD"
    provider = str(run.get("provider") or "")
    model = str(run.get("model") or "")
    table = prices.get(provider + "/" + model) or prices.get(model) or {}
    if not table:
        return 0.0, "missing", "USD"
    def rate(name: str) -> float:
        return float(table.get(name, 0) or 0) / 1_000_000.0
    total = float(usage.get("inputTokens", 0) or 0) * rate("input")
    total += float(usage.get("outputTokens", 0) or 0) * rate("output")
    total += float(usage.get("cacheReadTokens", 0) or 0) * rate("cacheRead")
    total += float(usage.get("cacheWriteTokens", 0) or 0) * rate("cacheWrite")
    return total, "tokens", "USD"


def main() -> int:
    parser = argparse.ArgumentParser(description="bomberman-eval")
    parser.add_argument("--tier", choices=sorted(TIERS), default="quick")
    parser.add_argument("--submissions", default=str(ROOT / "submissions"))
    parser.add_argument("--task", default=str(DEFAULT_TASK))
    parser.add_argument("--with-history", action="store_true")
    parser.add_argument("--out", default="")
    parser.add_argument("--workers", type=int, default=2)
    args = parser.parse_args()

    task = Path(args.task).resolve()
    workspace = task / "workspace"
    if not (task / "tournament" / "run.py").is_file():
        print("bomberman task not found:", task, file=sys.stderr)
        return 2
    submissions = Path(args.submissions).resolve()
    dirs = [p for p in sorted(submissions.iterdir()) if p.is_dir() and (p / "bot.cpp").is_file()] if submissions.is_dir() else []
    if not dirs:
        print("no submissions found under", submissions, file=sys.stderr)
        return 2

    out = Path(args.out).resolve() if args.out else ROOT / "results" / time.strftime("%Y%m%d-%H%M%S")
    out.mkdir(parents=True, exist_ok=True)
    run_dir = out / "tournament"
    names = {}
    programs = {"Base": str(workspace / "baseline.cpp")}
    bots = ["Base=" + programs["Base"]]
    for directory in dirs:
        name = re.sub(r"[^A-Za-z0-9_.-]+", "-", directory.name).strip("-") or "submission"
        names[name] = directory
        programs[name] = str(directory / "bot.cpp")
        bots.append(name + "=" + programs[name])
    if args.with_history:
        for bot in sorted((task / "bots").glob("*.cpp")):
            history = "history-" + bot.stem
            programs[history] = str(bot)
            bots.append(history + "=" + programs[history])

    sizes, seeds = TIERS[args.tier]
    cmd = [sys.executable, "-X", "utf8", str(task / "tournament" / "run.py"), "--output", str(run_dir),
           "--bots", *bots, "--sizes", *sizes, "--seeds", str(seeds), "--workers", str(max(1, args.workers))]
    print("tier=%s sizes=%s seeds=%d bots=%d" % (args.tier, sizes, seeds, len(bots)), flush=True)
    completed = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=3600)
    if completed.returncode != 0:
        print(completed.stdout[-4000:], completed.stderr[-4000:], file=sys.stderr)
        return 2
    ratings_doc = load_json(run_dir / "bt.json") or {}
    if ratings_doc.get("unavailable"):
        print("bomberman rating unavailable: " + str(ratings_doc.get("unavailable")), file=sys.stderr)
        return 2
    ratings = ratings_doc.get("ratings", {})
    prices = load_json(ROOT / "bomberman-prices.json") or load_json(ROOT / "prices.json") or {}

    rows = []
    for name, program in programs.items():
        directory = names.get(name)
        if directory is not None:
            sub_run = directory / "run"
            run = load_json(sub_run / "run.json") or {}
            usage = load_json(sub_run / "usage.json") or {}
            cost = load_json(directory / "cost.json") or load_json(sub_run / "cost.json")
            amount, source, currency = money(directory, run, usage, cost, prices)
            violations = 0
            if (sub_run / "violations.jsonl").is_file():
                violations = len([line for line in (sub_run / "violations.jsonl").read_text(encoding="utf-8").splitlines() if line.strip()])
            scan = static_scan(directory / "bot.cpp")
            kind = "submission"
        else:
            run = {}
            amount, source, currency = 0.0, "missing", "USD"
            violations = 0
            scan = static_scan(Path(program)) if Path(program).suffix == ".cpp" else []
            kind = "baseline" if name == "Base" else "history"
        rows.append({
            "name": name,
            "kind": kind,
            "rating": round(float(ratings.get(name, 0) or 0), 2),
            "cost": round(amount, 4), "costSource": source, "currency": currency,
            "violations": violations,
            "static": scan,
            "run": run,
        })
    rows.sort(key=lambda row: (-row["rating"], row["name"]))
    payload = {"generatedAt": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "tier": args.tier, "sizes": sizes, "seeds": seeds, "results": rows}
    (out / "summary.json").write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    lines = ["# bomberman-eval 结果", "", "- 时间：%s" % payload["generatedAt"], "- 档位：%s（人数 %s，种子数 %d）" % (args.tier, ",".join(sizes), seeds), "", "## 排行榜", "", "| 名次 | 名称 | BT 实力值 | 成本 | 价格来源 | 违规 | 静态扫描 |", "|---:|---|---:|---:|---|---:|---|"]
    for rank, row in enumerate(rows, 1):
        scan = "未发现" if not row["static"] else "%d 项" % len(row["static"])
        lines.append("| %d | %s | %+.2f | %.4f %s | %s | %d | %s |" % (rank, row["name"], row["rating"], row["cost"], row["currency"], row["costSource"], row["violations"], scan))
    lines += ["", "## 说明", "", "- 使用官方 tournament/run.py 与 BT 评分；Base（baseline）为 0 分锚点。", "- 没有 run/ 时仍评测 bot，只是成本/用量显示 missing。", "- 静态扫描只报告，不取消资格。"]
    (out / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("wrote", out / "summary.md")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
