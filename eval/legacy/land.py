#!/usr/bin/env python3
"""Independent evaluation for land (territory) bot submissions.

Reads submissions/<name>/{bot.cpp,run/,cost.json}, then runs the official
round_robin.py + rate.py over the submissions and archive players. A missing
run/ directory is fine: only the bot is evaluated.
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
DEFAULT_TASK = ROOT.parent / "meow-ai-arena" / "land"
TIERS = {"quick": (2, 0), "full": (6, 10), "formal": (12, 20)}
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
    parser = argparse.ArgumentParser(description="land-eval")
    parser.add_argument("--tier", choices=sorted(TIERS), default="quick")
    parser.add_argument("--submissions", default=str(ROOT / "submissions"))
    parser.add_argument("--task", default=str(DEFAULT_TASK))
    parser.add_argument("--with-history", action="store_true")
    parser.add_argument("--out", default="")
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()

    task = Path(args.task).resolve()
    workspace = task / "workspace"
    if not (workspace / "engine" / "referee.py").is_file():
        print("land task not found:", task, file=sys.stderr)
        return 2
    submissions = Path(args.submissions).resolve()
    dirs = [p for p in sorted(submissions.iterdir()) if p.is_dir() and (p / "bot.cpp").is_file()] if submissions.is_dir() else []
    if not dirs:
        print("no submissions found under", submissions, file=sys.stderr)
        return 2

    out = Path(args.out).resolve() if args.out else ROOT / "results" / time.strftime("%Y%m%d-%H%M%S")
    out.mkdir(parents=True, exist_ok=True)
    entries = out / "entries"
    entries.mkdir(exist_ok=True)
    names = {}
    for directory in dirs:
        name = re.sub(r"[^A-Za-z0-9_.-]+", "-", directory.name).strip("-") or "submission"
        target = entries / name
        target.mkdir(exist_ok=True)
        shutil.copy2(directory / "bot.cpp", target / "bot.cpp")
        names[name] = directory
    if args.with_history:
        for bot in sorted((task / "bots").glob("*/bot.cpp")):
            target = entries / ("history-" + bot.parent.name.replace("/", "-"))
            target.mkdir(exist_ok=True)
            shutil.copy2(bot, target / "bot.cpp")

    maps, melee = TIERS[args.tier]
    results_jsonl = out / "results.jsonl"
    cmd = [sys.executable, "-X", "utf8", str(task / "tournament" / "round_robin.py"),
           "--entries", str(entries), "--out", str(results_jsonl), "--maps", str(maps),
           "--melee-rounds", str(melee), "--jobs", str(max(1, args.jobs))]
    if melee == 0:
        cmd += ["--only", "duel"]
    print("tier=%s maps=%d melee=%d entries=%d" % (args.tier, maps, melee, len(list(entries.iterdir()))), flush=True)
    completed = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=1800)
    if completed.returncode != 0:
        print(completed.stdout[-4000:], completed.stderr[-4000:], file=sys.stderr)
        return 2
    ratings_prefix = out / "ratings"
    rate = subprocess.run([sys.executable, "-X", "utf8", str(task / "tournament" / "rate.py"), str(results_jsonl),
                           "--entries", str(entries), "--out", str(ratings_prefix)], capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=1800)
    if rate.returncode != 0:
        print(rate.stdout[-4000:], rate.stderr[-4000:], file=sys.stderr)
        return 2
    ratings = load_json(Path(str(ratings_prefix) + ".json")) or {}
    if ratings.get("unavailable"):
        print("land rating unavailable: " + str(ratings.get("unavailable")), file=sys.stderr)
        return 2
    models = ratings.get("models", {})
    prices = load_json(ROOT / "land-prices.json") or load_json(ROOT / "prices.json") or {}

    rows = []
    all_names = list(names) + [name for name in models if name not in names]
    for name in all_names:
        directory = names.get(name)
        if directory is not None:
            run_dir = directory / "run"
            run = load_json(run_dir / "run.json") or {}
            usage = load_json(run_dir / "usage.json") or {}
            cost = load_json(directory / "cost.json") or load_json(run_dir / "cost.json")
            amount, source, currency = money(directory, run, usage, cost, prices)
            violations = 0
            if (run_dir / "violations.jsonl").is_file():
                violations = len([line for line in (run_dir / "violations.jsonl").read_text(encoding="utf-8").splitlines() if line.strip()])
            scan = static_scan(directory / "bot.cpp")
            kind = "submission"
        else:
            run = {}
            amount, source, currency = 0.0, "missing", "USD"
            violations = 0
            candidate = entries / name / "bot.cpp"
            scan = static_scan(candidate) if candidate.is_file() else []
            kind = "baseline" if name.startswith("anchor-") else "history"
        model = models.get(name, {})
        rows.append({
            "name": name,
            "kind": kind,
            "score": round(float(model.get("score", 0) or 0), 2),
            "theta": round(float(model.get("modes", {}).get("duel", {}).get("theta", 0) or 0), 3),
            "cost": round(amount, 4), "costSource": source, "currency": currency,
            "violations": violations,
            "static": scan,
            "run": run,
        })
    rows.sort(key=lambda row: (-row["score"], -row["theta"], row["name"]))
    payload = {"generatedAt": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "tier": args.tier, "maps": maps, "melee": melee, "results": rows}
    (out / "summary.json").write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    lines = ["# land-eval 结果", "", "- 时间：%s" % payload["generatedAt"], "- 档位：%s（单挑 %d 张图，混战 %d 轮）" % (args.tier, maps, melee), "", "## 排行榜", "", "| 名次 | 名称 | 总分 | 单挑 theta | 成本 | 价格来源 | 违规 | 静态扫描 |", "|---:|---|---:|---:|---:|---|---:|---|"]
    for rank, row in enumerate(rows, 1):
        scan = "未发现" if not row["static"] else "%d 项" % len(row["static"])
        lines.append("| %d | %s | %.1f | %.2f | %.4f %s | %s | %d | %s |" % (rank, row["name"], row["score"], row["theta"], row["cost"], row["currency"], row["costSource"], row["violations"], scan))
    lines += ["", "## 说明", "", "- 使用圈地官方 round_robin.py + rate.py；锚点 greedy/random/sparring 自动加入。", "- 没有 run/ 时仍评测 bot，只是成本/用量显示 missing。", "- 静态扫描只报告，不取消资格。"]
    (out / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("wrote", out / "summary.md")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
