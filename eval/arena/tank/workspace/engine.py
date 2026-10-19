"""坦克大战的裁判接口。这里没有任何规则：规则只在 tank.hpp 里。

本文件做三件事，都是把活儿转给用 tank.hpp 编出来的小程序：
  Game          平台要求的对局接口。每回合把双方的命令转给裁判核心（judge/referee.cpp），取回新的局面。
  expand        把回放展开成逐回合的局面和事件（给 show 和回放查看器用）。
  run_fast      快速模式：调用 judge/fast.cpp 编出来的对局程序。

文本的格式（局面、命令、事件）是 tank.hpp 定的，这里只做拆字段，不做任何结算。
"""
from __future__ import annotations

import math
import queue
import shutil
import subprocess
import threading
import zlib
from pathlib import Path

import core

REPLAY_FORMAT = "tank-replay-1"
FRAMES_FORMAT = "tank-frames-1"
TYPE_NAMES = ["fast", "fire", "armor"]


class EngineError(core.MatchError):
    """裁判核心出了问题（起不来、回复看不懂）。属于裁判这一侧的错，不算在选手头上。"""


# ====================================================================
# 裁判核心进程
# ====================================================================

class Referee:
    """一个常驻的裁判核心进程。一问一答，按行通信。"""

    def __init__(self):
        try:
            exe = core.build_tool("referee")
        except RuntimeError as exc:
            raise EngineError(f"裁判核心编译不了：{exc}")
        try:
            self.proc = subprocess.Popen([str(exe)], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        except OSError as exc:
            raise EngineError(f"裁判核心无法启动：{exe}（{exc}）")
        core.adopt(self.proc)
        words = self.ask("HELLO").split()
        if len(words) < 9 or words[0] != "OK":
            raise EngineError("裁判核心的应答不对")
        self.info = {"engine": words[1], "budget_kind": core.BUDGET_KIND, "first_ms": int(words[2]),
                     "turn_ms": int(words[3]), "wall_first_ms": int(words[4]), "wall_turn_ms": int(words[5]),
                     "units_per_ms": int(words[6]) if core.BUDGET_KIND == "cpu" else 1,
                     "units_source": words[7] if core.BUDGET_KIND == "cpu" else "wall", "styles": words[8:]}
        core.configure_budget(self.info["units_per_ms"])

    def send(self, text):
        try:
            self.proc.stdin.write(text.encode("utf-8"))
            self.proc.stdin.flush()
        except (OSError, ValueError) as exc:
            raise EngineError(f"裁判核心已退出（{exc}）")

    def line(self):
        raw = self.proc.stdout.readline()
        if not raw:
            raise EngineError("裁判核心已退出")
        return raw.decode("utf-8", errors="replace").rstrip("\r\n")

    def ask(self, request):
        self.send(request + "\n")
        return self.line()

    def until(self, last):
        """一直读到内容为 last 的那一行（含）。"""
        out = []
        while True:
            text = self.line()
            out.append(text)
            if text == last:
                return out

    def close(self):
        core.stop_process(self.proc)


_idle = []
_idle_lock = threading.Lock()


def _acquire():
    with _idle_lock:
        if _idle:
            return _idle.pop()
    return Referee()


def _release(ref):
    with _idle_lock:
        _idle.append(ref)


def shutdown():
    """结束本进程起的所有空闲裁判核心。"""
    with _idle_lock:
        refs, _idle[:] = list(_idle), []
    for r in refs:
        r.close()


def info():
    """引擎版本、预算、地图风格名（从裁判核心读来，也就是从 tank.hpp 读来）。"""
    ref = _acquire()
    try:
        return dict(ref.info)
    finally:
        _release(ref)


def style_number(style):
    """地图风格：-1 或 auto = 按种子轮换；也可以写风格名或编号。"""
    if style is None or str(style) in ("-1", "auto", ""):
        return -1
    names = info()["styles"]
    if str(style) in names:
        return names.index(str(style))
    if str(style).isdigit() and int(style) < len(names):
        return int(style)
    raise ValueError(f"地图风格只能是 auto、{'、'.join(names)} 或 0–{len(names) - 1}")


# ====================================================================
# 文本拆字段（格式见 RULES.md / tank.hpp 第 8 节）
# ====================================================================

def split_state(text):
    """完整局面文本 → (布局段, 回合段)。"""
    at = text.index("TURN ")
    return text[:at], text[at:]


def parse_layout(text):
    """布局段 → {"params", "size", "bases", "spawns", "supply"}。"""
    params, out = {}, {}
    for row in text.splitlines():
        w = row.split()
        if not w:
            continue
        if w[0] == "P":
            params[w[1]] = w[3:] if w[1] == "tiebreak" else int(w[2])
        elif w[0] == "BASES":
            v = list(map(int, w[1:]))
            out["bases"] = [v[0:2], v[2:4]]
        elif w[0] == "SPAWNS":
            v = list(map(int, w[1:]))
            out["spawns"] = [v[k:k + 2] for k in range(0, 12, 2)]
        elif w[0] == "SUPPLY":
            v = list(map(int, w[2:]))
            out["supply"] = [v[k:k + 2] for k in range(0, len(v), 2)]
    out["params"] = params
    out["size"] = params.get("size", 15)
    return out


def parse_turn(text):
    """回合段 → 字典。只是拆字段。"""
    rows = text.splitlines() if isinstance(text, str) else list(text)
    f = {"tanks": [], "bullets": [], "stars": [], "map": []}
    k = 0
    while k < len(rows):
        w = rows[k].split()
        k += 1
        if not w:
            continue
        head = w[0]
        if head == "TURN":
            f.update(turn=int(w[1]), done=w[2] == "1", winner=int(w[3]), reason=w[4])
        elif head == "BASEHP":
            f["base_hp"] = [int(w[1]), int(w[2])]
        elif head == "KILLS":
            f["kills"] = [int(w[1]), int(w[2])]
        elif head == "STARCOUNT":
            f["stars_eaten"] = [int(w[1]), int(w[2])]
        elif head == "BASEDMG":
            f["base_dmg"] = [int(w[1]), int(w[2])]
        elif head == "MAP":
            while k < len(rows) and not rows[k].startswith("TANKS"):
                f["map"].append(rows[k].strip())
                k += 1
        elif head == "T":
            i = int(w[1])
            f["tanks"].append({"id": i, "seat": i // 3, "type": TYPE_NAMES[i % 3], "x": int(w[2]), "y": int(w[3]),
                               "hp": int(w[4]), "level": int(w[5]), "alive": w[6] == "1"})
        elif head == "B":
            f["bullets"].append({"x": int(w[1]), "y": int(w[2]), "dir": w[3], "owner": int(w[4]),
                                 "steel": w[5] == "1"})
        elif head == "STARS":
            for _ in range(int(w[1])):
                f["stars"].append(list(map(int, rows[k].split())))
                k += 1
    return f


# 事件行 → 字典。每种事件的字段名按 tank.hpp 的 event_text 的顺序。
_EVENT_FIELDS = {
    "fire": ["tank", "dir"],
    "blocked": ["tank", "sub"],
    "cancel": ["how", "x", "y", "sub", "owner1", "owner2"],
    "hit": ["tank", "by", "x", "y", "sub", "swap"],
    "base_hit": ["seat", "by", "sub"],
    "wall": ["x", "y", "kind", "sub"],
    "death": ["tank", "x", "y", "by"],
    "respawn": ["tank", "x", "y"],
    "bullet_cleared": ["x", "y", "tank"],
    "star": ["tank", "x", "y", "level"],
    "supply": ["x", "y"],
    "base_regen": ["seat"],
}


def parse_event(text):
    w = text.split()
    ev = {"type": w[0], "text": text}
    for name, value in zip(_EVENT_FIELDS.get(w[0], []), w[1:]):
        ev[name] = int(value) if value.lstrip("-").isdigit() else value
    return ev


# ====================================================================
# Game：平台接口
# ====================================================================

class Game:
    """一局坦克大战。所有结算都在裁判核心里（tank.hpp），这里只转手。"""

    TIME_LIMITS = None      # 思考预算 (首回合秒数, 之后每回合秒数)，第一次开局时从裁判核心读来
    WALL_CAPS = None        # 按 CPU 计时时的墙上保险上限，同上

    def __init__(self, n=2, seed=1, style=-1, start=None):
        if n != 2:
            raise ValueError("坦克大战是两方对战")
        self.seed, self.style = int(seed), int(style)
        self.ref = _acquire()
        try:
            if Game.TIME_LIMITS is None:
                Game.TIME_LIMITS = (self.ref.info["first_ms"] / 1000.0, self.ref.info["turn_ms"] / 1000.0)
                Game.WALL_CAPS = (self.ref.info["wall_first_ms"] / 1000.0, self.ref.info["wall_turn_ms"] / 1000.0)
            self._seat_line = f"BUDGET {self.ref.info['budget_kind']} {self.ref.info['units_per_ms']}"
            if start is None:
                if self.ref.ask(f"NEW {self.seed} {self.style}") != "OK":
                    raise EngineError("裁判核心开局失败")
                self.start = "\n".join(self.ref.until("END")) + "\n"
            else:
                self.ref.send("LOAD\n" + start)
                answer = self.ref.line()
                if answer != "OK":
                    raise EngineError(f"起始局面读不进去：{answer}")
                self.start = start
        except BaseException:
            self._drop()
            raise
        self.layout_text, self.turn_text = split_state(self.start)
        self._orders = []           # 每回合 [0 号方命令, 1 号方命令]，实际执行的
        self._checked = [None, None]
        self.forfeited = []
        self._read_turn_line()

    def _read_turn_line(self):
        w = self.turn_text.split(None, 5)
        self.turn, self.done, self.winner, self.reason = int(w[1]), w[2] == "1", int(w[3]), w[4]

    def _drop(self):
        """裁判核心状态不明，不再放回空闲池。"""
        if self.ref is not None:
            self.ref.close()
            self.ref = None

    def close(self):
        if self.ref is not None:
            _release(self.ref)
            self.ref = None

    def encode(self, i):
        """发给第 i 方的输入：开局是 "SEAT i BUDGET 计时方式 系数" 加完整局面，之后每回合只发回合段。"""
        if self.turn == 0:
            return f"SEAT {i} {self._seat_line}\n{self.start}"
        return self.turn_text

    def decode(self, i, line):
        """检查第 i 方的一行回复。合法返回规范写法（如 "UL FU S"），不合法抛出 ValueError。"""
        line = " ".join(line.split("\n")[0].split())
        try:
            answer = self.ref.ask(f"CHECK {i} {line}")
        except EngineError:
            self._drop()
            raise
        if answer.startswith("OK "):
            self._checked[i] = answer[3:]
            return self._checked[i]
        if answer.startswith("ERR"):
            raise ValueError(answer[4:] or "格式不对")
        self._drop()
        raise EngineError(f"裁判核心的回复看不懂：{answer[:80]}")

    def step(self, orders):
        """结算一回合。orders[i] 是 decode 的返回值；None 表示这一回合三辆车都不动。"""
        applied = []
        for i in range(2):
            o = orders[i] if orders is not None and i < len(orders) else None
            if o is None:
                # 这一方不动。decode 可能已经在裁判核心里记了一条，用一条"都不动"盖掉。
                self.decode(i, f"{self.turn + 1} S S S")
                o = "S S S"
            elif self._checked[i] != o:
                o = self.decode(i, f"{self.turn + 1} {o}")
            applied.append(o)
        try:
            if self.ref.ask("STEP") != "OK":
                raise EngineError("裁判核心结算失败")
            self.turn_text = "\n".join(self.ref.until("END")) + "\n"
        except EngineError:
            self._drop()
            raise
        self._checked = [None, None]
        self._orders.append(applied)
        self._read_turn_line()
        return {"turn": self.turn, "orders": applied}

    def forfeit(self, seats):
        """这些座位因故障判负：对局就此结束，这一回合不结算。两方同时故障算平局。"""
        self.forfeited = sorted(set(seats))
        self.done = True
        self.winner = -1 if len(self.forfeited) >= 2 else 1 - self.forfeited[0]
        self.reason = "fault"

    def header(self):
        inf = self.ref.info if self.ref else info()
        return {"format": REPLAY_FORMAT, "engine": inf["engine"], "seed": self.seed, "style": self.style,
                "budget": {"kind": inf["budget_kind"], "first_ms": inf["first_ms"], "turn_ms": inf["turn_ms"],
                           "wall_first_ms": inf["wall_first_ms"], "wall_turn_ms": inf["wall_turn_ms"],
                           "units_per_ms": inf["units_per_ms"], "max_strikes": core.MAX_STRIKES},
                "start": self.start}

    def frame(self):
        return parse_turn(self.turn_text)

    def result(self):
        f = parse_turn(self.turn_text)
        score = [0.5, 0.5] if self.winner < 0 else [1.0 if self.winner == i else 0.0 for i in range(2)]
        return {"score": score, "winner": self.winner, "reason": self.reason, "turns": self.turn,
                "base_hp": f["base_hp"], "kills": f["kills"], "stars_eaten": f["stars_eaten"],
                "base_dmg": f["base_dmg"], "forfeited": list(self.forfeited), "final": self.turn_text}

    def signature(self):
        """对局签名：逐回合实际执行的命令 + 终局局面（+ 谁因故障判负）。和 judge/fast.cpp 的算法相同。"""
        return signature_of(self._orders, self.turn_text, self.forfeited)


def signature_of(orders, final_text, forfeited=()):
    log = "".join(f"{a}|{b}\n" for a, b in orders)
    tail = final_text
    if forfeited:
        tail += "FAULT " + ("1" if 0 in forfeited else "0") + ("1" if 1 in forfeited else "0") + "\n"
    return f"{zlib.crc32(log.encode()):08x}{zlib.crc32(tail.encode()):08x}"


# ====================================================================
# 回放展开
# ====================================================================

def expand(replay):
    """把回放展开成逐回合的局面和事件。返回的字典可以直接存成 JSON 给查看器用（格式见 README）。

    局面由回放里的起始局面和每回合的命令重新结算得到，用的是同一个引擎。"""
    layout_text, _ = split_state(replay["start"])
    lay = parse_layout(layout_text)
    ref = _acquire()
    try:
        ref.send("LOAD\n" + replay["start"])
        answer = ref.line()
        if answer != "OK":
            raise EngineError(f"回放的起始局面读不进去：{answer}")
        first = "\n".join(ref.ask("STATE").splitlines() + ref.until("END"))
        _, turn_text = split_state(first + "\n")

        def with_info(frame, rows):
            for row in rows:
                w = row.split()
                if w and w[0] == "X":
                    frame["tanks"][int(w[1])].update(respawn_in=int(w[2]), reload_left=int(w[3]),
                                                     bullets_out=int(w[4]), bullet_cap=int(w[5]),
                                                     can_fire=w[6] == "1")
            return frame

        ref.send("INFO\n")
        frame = with_info(parse_turn(turn_text), ref.until("DONE"))
        frame.update(orders=None, used_ms=None, events=[], faults=[])
        frames = [frame]
        faults_by_turn = {}
        for f in replay.get("faults", []):
            faults_by_turn.setdefault(f["turn"], []).append(f)
        used = replay.get("used_ms") or []
        for k, pair in enumerate(replay["orders"]):
            turn = frames[-1]["turn"] + 1
            for seat in range(2):
                answer = ref.ask(f"CHECK {seat} {turn} {pair[seat]}")
                if not answer.startswith("OK"):
                    raise EngineError(f"回放第 {turn} 回合的命令不合法：{pair[seat]}")
            if ref.ask("STEPX") != "OK":
                raise EngineError("裁判核心结算失败")
            rows = ref.until("DONE")
            events = [parse_event(r[3:]) for r in rows if r.startswith("EV ")]
            body = [r for r in rows if not r.startswith("EV ")]
            end = body.index("END")
            frame = with_info(parse_turn(body[:end + 1]), body[end + 1:])
            frame.update(orders=[p.split() for p in pair], used_ms=used[k] if k < len(used) else None,
                         events=events, faults=faults_by_turn.get(turn, []))
            frames.append(frame)
            final_text = "\n".join(body[:end + 1]) + "\n"
        if not replay["orders"]:
            final_text = turn_text
    except EngineError:
        ref.close()
        ref = None
        raise
    finally:
        if ref is not None:
            _release(ref)
    result = replay.get("result", {})
    recorded = result.get("final")
    return {
        "format": FRAMES_FORMAT, "engine": replay.get("engine"), "mode": replay.get("mode"),
        "seed": replay.get("seed"), "style": replay.get("style"), "names": replay.get("names"),
        "budget": replay.get("budget"), "params": lay["params"], "size": lay["size"], "bases": lay["bases"],
        "spawns": lay["spawns"], "supply": lay["supply"],
        "result": {k: v for k, v in result.items() if k != "final"},
        "faults": replay.get("faults", []), "stderr": replay.get("stderr", []),
        "consistent": recorded is None or recorded == final_text,
        "frames": frames,
    }


# ====================================================================
# 快速模式
# ====================================================================

class FastError(RuntimeError):
    """快速模式里有 bot 崩溃、卡死或抛异常，整批停止。message 里写明是谁、哪一局。"""


def _parse_usage(text):
    first, avg, slowest, over, illegal = text.split(":")
    return {"first_ms": float(first), "avg_ms": float(avg), "max_ms": float(slowest), "over": int(over),
            "illegal": int(illegal)}


def run_fast(lib_a, lib_b, jobs, workers=1, log=False, hang_seconds=60, names=("A", "B")):
    """快速模式打一批。jobs 是 [(种子, 风格, 换边)]，换边为 0 时 A 坐 0 号位。返回与 jobs 等长的结果列表。

    每个结果：{"seed", "style", "swap", "turns", "winner"（座位，-1 平）, "reason", "sig", "usage": [A, B]}，
    log=True 时另有 "orders"（逐回合实际执行的命令）、"final"（终局回合段）、"faults"。
    任何一方崩溃、卡死、抛异常：抛出 FastError。"""
    lib_a, lib_b = Path(lib_a), Path(lib_b)
    if lib_a.resolve() == lib_b.resolve():
        # 同一个文件在一个进程里只能加载一次，自己打自己时给第二份另起一个文件名。
        twin = lib_b.with_name(lib_b.stem + "-twin" + lib_b.suffix)
        if not twin.is_file():
            tmp = twin.with_name(f"tmp-{threading.get_ident()}-{twin.name}")
            shutil.copyfile(lib_b, tmp)
            try:
                tmp.replace(twin)
            except OSError:
                if not twin.is_file():
                    raise
        lib_b = twin
    try:
        exe = core.build_tool("fast")
    except RuntimeError as exc:
        raise EngineError(f"快速模式的对局程序编译不了：{exc}")
    results = [None] * len(jobs)
    workers = max(1, min(workers, len(jobs) or 1))
    size = max(1, min(200, math.ceil(len(jobs) / workers)))
    chunks = queue.Queue()
    for at in range(0, len(jobs), size):
        chunks.put(list(range(at, min(len(jobs), at + size))))
    failure, running, lock = [], [], threading.Lock()

    def describe(k):
        seed, style, swap = jobs[k]
        seats = f"{names[1]} 坐 0 号位、{names[0]} 坐 1 号位" if swap else f"{names[0]} 坐 0 号位、{names[1]} 坐 1 号位"
        return f"第 {k + 1} 局（种子 {seed}，{seats}）"

    def work():
        while not failure:
            try:
                ids = chunks.get_nowait()
            except queue.Empty:
                return
            cmd = [str(exe), str(lib_a), str(lib_b), "--hang", str(hang_seconds)] + (["--log"] if log else []) + (["--wall"] if core.BUDGET_KIND == "wall" else [])
            text = "".join(f"{k} {jobs[k][0]} {jobs[k][1]} {jobs[k][2]}\n" for k in ids)
            try:
                proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            except OSError as exc:
                failure.append(EngineError(f"快速模式的对局程序无法启动：{exe}（{exc}）"))
                return
            core.adopt(proc)
            with lock:
                running.append(proc)
            out, _ = proc.communicate(text.encode())
            with lock:
                running.remove(proc)
            if failure:
                return
            current, orders, faults, final, finished = None, [], [], None, False
            rows = out.decode("utf-8", errors="replace").splitlines()
            at = 0
            while at < len(rows):
                w = rows[at].split(None, 3)
                at += 1
                if not w:
                    continue
                if w[0] == "B":
                    current, orders, faults, final = int(w[1]), [], [], None
                elif w[0] == "O":
                    orders.append(rows[at - 1].split(None, 2)[2].split("|"))
                elif w[0] == "F":
                    faults.append({"turn": int(w[1]), "seat": int(w[2]), "kind": "illegal", "detail": w[3]})
                elif w[0] == "FINAL":
                    end = rows.index("END", at)
                    final = "\n".join(rows[at:end + 1]) + "\n"
                    at = end + 1
                elif w[0] == "G":
                    g = rows[at - 1].split()
                    k = int(g[1])
                    results[k] = {"seed": jobs[k][0], "style": jobs[k][1], "swap": jobs[k][2], "turns": int(g[5]),
                                  "winner": int(g[6]), "reason": g[7], "sig": g[8],
                                  "usage": [_parse_usage(g[9]), _parse_usage(g[10])], "faults": faults}
                    if log:
                        results[k].update(orders=orders, final=final)
                    current = None
                elif w[0] == "X":
                    x = rows[at - 1].split(None, 3)
                    who = {"A": names[0], "B": names[1]}.get(x[2], "不确定是哪一方")
                    where = describe(int(x[1])) if x[1].lstrip("-").isdigit() and int(x[1]) >= 0 else "开局前"
                    failure.append(FastError(f"{where}：{who} 出事了：{x[3] if len(x) > 3 else ''}"))
                    return
                elif w[0] == "DONE":
                    finished = True
            if not finished:
                where = describe(current) if current is not None else "某一局"
                failure.append(FastError(f"{where}：对局进程异常退出（{core.describe_exit(proc.returncode)}），"
                                         f"不确定是哪一方造成的。可以用 play 在正式模式下重打这一局来确认。"))
                return

    threads = [threading.Thread(target=work, daemon=True) for _ in range(workers)]
    try:
        for t in threads:
            t.start()
        while any(t.is_alive() for t in threads):
            for t in threads:
                t.join(timeout=0.2)
            if failure:
                break
    finally:
        with lock:
            for proc in list(running):      # 只结束自己起的进程
                try:
                    proc.kill()
                except Exception:
                    pass
    if failure:
        raise failure[0]
    return results
