#!/usr/bin/env python3
"""Independent evaluation for dsh-eval-guard tank submissions.

Reads submissions/<name>/{bot.cpp,run/,cost.json}, builds a round-robin against
the initial baseline (optionally the 14 archived models), fits Bradley-Terry
ratings anchored at the baseline, and writes Markdown/JSON/CSV reports.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import json
import math
import re
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent
DEFAULT_ARENA = ROOT.parent / "meow-ai-arena" / "tank"
TIERS = {"quick": 20, "full": 80, "formal": 200}
SUMMARY = re.compile(r"汇总：.*?(\d+)-(\d+)-(\d+)（胜-平-负）")

STATIC_PATTERNS = [
    (re.compile(r"#\s*include\s*<(fstream|filesystem|thread|future|unistd\.h|windows\.h|winsock2?\.h|sys/|netinet/|arpa/|dlfcn\.h|signal\.h)>"), "可疑头文件"),
    (re.compile(r"\b(fopen|freopen|ifstream|ofstream|fstream|filesystem::)"), "文件 IO"),
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


@dataclass
class PairResult:
    a: str
    b: str
    wins: int
    draws: int
    losses: int


def run_pair(a: Participant, b: Participant, games: int, seed: int, workers: int, workspace: Path) -> PairResult:
    command = [
        sys.executable, "-X", "utf8", str(workspace / "arena.py"), "batch",
        str(a.source), str(b.source), "--games", str(games), "--seed", str(seed),
        "--workers", str(workers),
    ]
    completed = subprocess.run(command, cwd=workspace, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=900)
    if completed.returncode != 0:
        raise RuntimeError("pair " + a.name + " vs " + b.name + " failed:\n" + completed.stdout[-2000:] + completed.stderr[-2000:])
    lines = [line for line in completed.stdout.strip().splitlines() if "汇总：" in line]
    if not lines:
        raise RuntimeError("pair " + a.name + " vs " + b.name + " produced no summary")
    match = SUMMARY.search(lines[-1])
    if match is None:
        raise RuntimeError("cannot parse summary: " + lines[-1])
    w, d, l = (int(part) for part in match.groups())
    return PairResult(a.name, b.name, w, d, l)


def load_json(path: Path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return None


def load_submission(directory: Path) -> Participant:
    bot = directory / "bot.cpp"
    if not bot.is_file():
        raise FileNotFoundError(str(bot))
    run_dir = directory / "run"
    runner = load_json(run_dir / "run.json") or {}
    usage = load_json(run_dir / "usage.json")
    cost = load_json(directory / "cost.json")
    violations = []
    ledger = run_dir / "violations.jsonl"
    if ledger.is_file():
        for line in ledger.read_text(encoding="utf-8", errors="replace").splitlines():
            line = line.strip()
            if line:
                try:
                    violations.append(json.loads(line))
                except Exception:
                    pass
    return Participant(directory.name, bot, "submission", directory, runner, usage, cost, violations)


def history_participants(arena: Path) -> list[Participant]:
    bots = sorted((arena / "bots" / "half2").glob("*.cpp"))
    return [Participant("history:" + bot.stem, bot, "history") for bot in bots]


def static_scan(source: Path) -> dict:
    text = source.read_text(encoding="utf-8", errors="replace")
    findings = []
    for pattern, label in STATIC_PATTERNS:
        for match in pattern.finditer(text):
            line = text.count("\n", 0, match.start()) + 1
            findings.append({"label": label, "line": line, "text": match.group(0)})
    return {"clean": len(findings) == 0, "findings": findings}


def price_for(runner: dict | None, usage: dict | None, prices: dict) -> tuple[float, str]:
    if usage is None:
        return 0.0, "missing"
    model = str((runner or {}).get("model") or "")
    provider = str((runner or {}).get("provider") or "")
    table = prices.get(provider + "/" + model) or prices.get(model) or {}
    def rate(key: str) -> float:
        try:
            return float(table.get(key, 0) or 0)
        except Exception:
            return 0.0
    cost = (usage.get("inputTokens", 0) / 1_000_000.0) * rate("input")
    cost += (usage.get("outputTokens", 0) / 1_000_000.0) * rate("output")
    cost += (usage.get("cacheReadTokens", 0) / 1_000_000.0) * rate("cacheRead")
    cost += (usage.get("cacheWriteTokens", 0) / 1_000_000.0) * rate("cacheWrite")
    return cost, "computed"


def money(participant: Participant, prices: dict) -> tuple[float, str, str]:
    if participant.cost is not None:
        try:
            return float(participant.cost.get("total", 0) or 0), "manual", str(participant.cost.get("currency", ""))
        except Exception:
            pass
    value, source = price_for(participant.run, participant.usage, prices)
    return value, source, "USD"


def fit_bt(results: list[PairResult], names: list[str], anchor: str) -> dict[str, float]:
    index = {name: i for i, name in enumerate(names)}
    n = len(names)
    wins = [0.0] * n
    games = [[0.0] * n for _ in range(n)]
    for result in results:
        ia, ib = index[result.a], index[result.b]
        wins[ia] += result.wins + 0.5 * result.draws
        wins[ib] += result.losses + 0.5 * result.draws
        count = result.wins + result.draws + result.losses
        games[ia][ib] += count
        games[ib][ia] += count
    # One virtual draw per pair prevents divergence.
    for i in range(n):
        for j in range(i + 1, n):
            games[i][j] += 1
            games[j][i] += 1
            wins[i] += 0.5
            wins[j] += 0.5
    free = [i for i, name in enumerate(names) if name != anchor]
    ratings = [0.0] * n
    def p(i: int, j: int) -> float:
        return 1.0 / (1.0 + 10.0 ** ((ratings[j] - ratings[i]) / 400.0))
    for _ in range(80):
        grad = [0.0] * n
        hess = [[0.0] * n for _ in range(n)]
        for i in range(n):
            for j in range(n):
                if i == j or games[i][j] == 0:
                    continue
                probability = p(i, j)
                weight = games[i][j] * probability * (1.0 - probability)
                hess[i][i] -= weight
                if i != j:
                    hess[i][j] += weight
        # Gradient: wins over opponents minus expected wins.
        for i in range(n):
            total = 0.0
            for j in range(n):
                if i == j or games[i][j] == 0:
                    continue
                total += games[i][j] * p(i, j)
            grad[i] = wins[i] - total
        size = len(free)
        matrix = [[hess[free[r]][free[c]] for c in range(size)] + [-grad[free[r]]] for r in range(size)]
        # Gaussian elimination with ridge.
        for r in range(size):
            matrix[r][r] -= 1e-6
        for col in range(size):
            pivot = max(range(col, size), key=lambda r: abs(matrix[r][col]))
            if abs(matrix[pivot][col]) < 1e-12:
                continue
            matrix[col], matrix[pivot] = matrix[pivot], matrix[col]
            scale = matrix[col][col]
            for c in range(col, size + 1):
                matrix[col][c] /= scale
            for r in range(size):
                if r == col:
                    continue
                factor = matrix[r][col]
                if factor == 0:
                    continue
                for c in range(col, size + 1):
                    matrix[r][c] -= factor * matrix[col][c]
        delta = [matrix[r][size] for r in range(size)]
        if max((abs(value) for value in delta), default=0.0) < 1e-6:
            break
        for pos, i in enumerate(free):
            ratings[i] += max(-200.0, min(200.0, delta[pos]))
        anchor_index = index[anchor]
        ratings[anchor_index] = 0.0
    return {name: ratings[index[name]] for name in names}


def render_matrix(names: list[str], results: list[PairResult]) -> str:
    wins = {(r.a, r.b): (r.wins, r.draws, r.losses) for r in results}
    header = "| 行 对 列 | " + " | ".join(names) + " |\n|---|" + "---|" * len(names) + "\n"
    lines = [header]
    for a in names:
        cells = []
        for b in names:
            if a == b:
                cells.append("·")
            elif (a, b) in wins:
                w, d, l = wins[(a, b)]
                cells.append(str(w) + "-" + str(d) + "-" + str(l))
            else:
                w, d, l = wins.get((b, a), (0, 0, 0))
                cells.append(str(l) + "-" + str(d) + "-" + str(w))
        lines.append("| " + a + " | " + " | ".join(cells) + " |\n")
    return "".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description="Evaluate dsh-eval-guard tank submissions")
    parser.add_argument("--submissions", default=str(ROOT / "submissions"))
    parser.add_argument("--arena", default=str(DEFAULT_ARENA))
    parser.add_argument("--tier", choices=sorted(TIERS), default="quick")
    parser.add_argument("--seed", type=int, default=9001)
    parser.add_argument("--workers", type=int, default=2)
    parser.add_argument("--pair-workers", type=int, default=2)
    parser.add_argument("--with-history", action="store_true")
    parser.add_argument("--out", default="")
    args = parser.parse_args()

    submissions_root = Path(args.submissions).resolve()
    arena = Path(args.arena).resolve()
    workspace = arena / "workspace"
    if not (workspace / "arena.py").is_file():
        print("arena workspace not found: " + str(workspace), file=sys.stderr)
        return 2
    directories = [path for path in sorted(submissions_root.iterdir()) if path.is_dir() and (path / "bot.cpp").is_file()] if submissions_root.is_dir() else []
    if not directories:
        print("no submissions found under " + str(submissions_root), file=sys.stderr)
        return 2
    prices_path = ROOT / "prices.json"
    prices = load_json(prices_path) or {}

    participants: list[Participant] = [Participant("baseline", workspace / "baseline.cpp", "baseline")]
    participants += [load_submission(directory) for directory in directories]
    if args.with_history:
        participants += history_participants(arena)
    names = [participant.name for participant in participants]
    games = TIERS[args.tier]
    pairs = [(a, b) for i, a in enumerate(participants) for b in participants[i + 1:]]
    print("tier=" + args.tier + " games/pair=" + str(games) + " participants=" + str(len(participants)) + " pairs=" + str(len(pairs)), flush=True)
    results: list[PairResult] = []
    started = time.time()
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.pair_workers)) as pool:
        futures = {pool.submit(run_pair, a, b, games, args.seed, args.workers, workspace): (a, b) for a, b in pairs}
        done = 0
        for future in concurrent.futures.as_completed(futures):
            a, b = futures[future]
            try:
                results.append(future.result())
            except Exception as error:
                print("FAILED " + a.name + " vs " + b.name + ": " + str(error), file=sys.stderr)
            done += 1
            if done % 5 == 0 or done == len(pairs):
                print("  pairs " + str(done) + "/" + str(len(pairs)), flush=True)
    elapsed = time.time() - started
    ratings = fit_bt(results, names, "baseline")

    rows = []
    for participant in participants:
        score = 0.0
        total = 0.0
        for result in results:
            if result.a == participant.name:
                score += result.wins + 0.5 * result.draws
                total += result.wins + result.draws + result.losses
            elif result.b == participant.name:
                score += result.losses + 0.5 * result.draws
                total += result.wins + result.draws + result.losses
        amount, price_source, currency = money(participant, prices)
        usage = participant.usage or {}
        violations = participant.violations
        counts = {}
        for violation in violations:
            kind = str(violation.get("kind", "unknown"))
            counts[kind] = counts.get(kind, 0) + 1
        scan = static_scan(participant.source) if participant.kind != "baseline" else {"clean": True, "findings": []}
        rows.append({
            "name": participant.name,
            "kind": participant.kind,
            "rating": ratings.get(participant.name, 0.0),
            "scoreRate": (score / total) if total else 0.0,
            "games": int(total),
            "cost": amount,
            "currency": currency,
            "priceSource": price_source,
            "wallTimeMs": usage.get("wallTimeMs", 0),
            "inputTokens": usage.get("inputTokens", 0),
            "outputTokens": usage.get("outputTokens", 0),
            "toolCalls": usage.get("toolCalls", 0),
            "violations": len(violations),
            "violationKinds": counts,
            "staticClean": scan["clean"],
            "staticFindings": scan["findings"],
            "model": (participant.run or {}).get("model"),
            "provider": (participant.run or {}).get("provider"),
        })
    rows.sort(key=lambda row: row["rating"], reverse=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    out = Path(args.out).resolve() if args.out else ROOT / "results" / stamp
    out.mkdir(parents=True, exist_ok=True)

    pair_rows = ["a,b,wins,draws,losses\n"]
    for result in sorted(results, key=lambda item: (item.a, item.b)):
        pair_rows.append(result.a + "," + result.b + "," + str(result.wins) + "," + str(result.draws) + "," + str(result.losses) + "\n")
    (out / "pairwise.csv").write_text("".join(pair_rows), encoding="utf-8")

    summary = {
        "generatedAt": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "tier": args.tier,
        "gamesPerPair": games,
        "seed": args.seed,
        "arena": str(arena),
        "elapsedSeconds": round(elapsed, 2),
        "participants": rows,
        "pairwise": [result.__dict__ for result in results],
        "matrix": render_matrix(names, results),
        "ratingMethod": "Bradley-Terry, baseline anchored at 0, 400 points per 10x, virtual draw per pair",
        "priceSources": sorted({row["priceSource"] for row in rows}),
    }
    (out / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    lines = ["# tank-eval 结果", ""]
    lines.append("- 时间：" + summary["generatedAt"])
    lines.append("- 档位：" + args.tier + "（每对 " + str(games) + " 局，种子 " + str(args.seed) + "）")
    lines.append("- 参赛：" + str(len(participants)) + "（包含初始基准" + ("、14 个历史模型" if args.with_history else "") + "）")
    lines.append("- 评测耗时：" + str(round(elapsed, 1)) + " 秒")
    lines.append("")
    lines.append("## 排行榜")
    lines.append("")
    lines.append("| 名次 | 名称 | 实力值 | 得分率 | 成本 | 价格来源 | 违规 | 静态扫描 |")
    lines.append("|---:|---|---:|---:|---:|---|---:|---|")
    for rank, row in enumerate(rows, 1):
        static = "未发现" if row["staticClean"] else "发现 " + str(len(row["staticFindings"])) + " 处"
        lines.append("| " + str(rank) + " | " + row["name"] + " | " + f"{row['rating']:+.0f}" + " | " +
                     f"{row['scoreRate']:.1%}" + " | " + f"{row['cost']:.4g} {row['currency']}" + " | " +
                     row["priceSource"] + " | " + str(row["violations"]) + " | " + static + " |")
    lines.append("")
    lines.append("## 两两胜平负")
    lines.append("")
    lines.append(summary["matrix"])
    lines.append("")
    lines.append("## 成本与用量")
    lines.append("")
    lines.append("| 名称 | 模型 | wall time | input | output | tools | 说明 |")
    lines.append("|---|---|---:|---:|---:|---:|---|")
    for row in rows:
        minutes = row["wallTimeMs"] / 60000.0 if row["wallTimeMs"] else 0.0
        note = "手动写入" if row["priceSource"] == "manual" else ("按 token 计算" if row["priceSource"] == "computed" else "无成本数据")
        lines.append("| " + row["name"] + " | " + str(row["model"] or "-") + " | " +
                     f"{minutes:.1f} min" + " | " + str(row["inputTokens"]) + " | " + str(row["outputTokens"]) + " | " +
                     str(row["toolCalls"]) + " | " + note + " |")
    lines.append("")
    lines.append("## 违规与静态扫描")
    lines.append("")
    for row in rows:
        if row["kind"] == "baseline":
            continue
        kinds = ", ".join(key + "=" + str(value) for key, value in sorted(row["violationKinds"].items())) or "无"
        findings = row["staticFindings"]
        if findings:
            detail = "; ".join(item["label"] + "@" + str(item["line"]) for item in findings[:8])
        else:
            detail = "未发现"
        lines.append("- " + row["name"] + "：违规 " + str(row["violations"]) + " 次（" + kinds + "）；静态扫描：" + detail)
    lines.append("")
    lines.append("## 复现")
    lines.append("")
    lines.append("~~~bash")
    lines.append("python3 eval.py --tier " + args.tier + " --seed " + str(args.seed) + " --with-history" if args.with_history else "python3 eval.py --tier " + args.tier + " --seed " + str(args.seed))
    lines.append("~~~")
    (out / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("\nwrote " + str(out / "summary.md"), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
