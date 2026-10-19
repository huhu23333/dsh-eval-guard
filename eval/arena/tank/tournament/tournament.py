"""正式赛（坦克大战，1 对 1）：冻结赛程 → 跑（可中断、可续跑）→ 评分。

  python tournament.py plan     赛事配置.json            冻结赛程；已有赛程时拒绝覆盖
  python tournament.py run      赛事目录 [--workers N]    开赛前统一编译，绑核，逐批测速；随时可以中断，再运行接着跑
  python tournament.py rate     赛事目录                 输出 成绩.md、成绩.json
  python tournament.py status   赛事目录                 进度、顶替、测速标记
  python tournament.py cpus     赛事配置.json            按本机拓扑和配置算出的绑核方案（不运行任何对局）
  python tournament.py estimate [--programs 14] ...      N（每对的地图数）与耗时的对照表

赛事配置（JSON，路径相对配置文件所在目录）：
  title          这场比赛叫什么（写进成绩表）
  kit            选手包目录的副本（tank.hpp、tank_main.cpp、core.py、engine.py、judge/）。必须是纯英文路径
  build_dir      编译结果放哪里。必须在允许运行新程序的位置（本机是 C:/bench 下）
  out            赛事目录：赛程、结果、回放、清单、成绩都在这里
  compiler       g++ 的路径（可选；不写就按 core.find_compiler 的顺序找）
  players        {名字: 源码或可执行文件}
  substitutes    {名字: [替补, ...], "*": [替补, ...]}。某个程序没有文件或编译不过时，依次试它名下的替补，再试 "*" 的。
                 替补写路径，或 {"path": 路径, "label": "说明"}。都不行这场比赛才停下
  anchor         基准的名字，实力值以它为 0
  unranked       不排名次的名字（基准自动不排；陪练写在这里）
  labels         {名字: 显示时附带的说明}，比如 {"baseline": "基准"}
  display        {名字: 显示名}。成绩表里写成“显示名（名字）”；没有写的直接显示名字。只影响显示，文件名、结果文件里都还是名字
  maps_per_pair  每对打几张图（每张图换边各一局，所以每对的局数是它的两倍）
  pairs          不写 = 全配对；写成 [[a, b], ...] 就只打这些对
  seed           赛程的随机种子（不写就随机抽，抽到的值写进赛程冻结）
  avoid_seeds    别的赛程文件的列表：不使用它们用过的地图种子
  maps_from      另一场比赛的赛事目录：不抽地图，第 m 张图用那场比赛第 m 张图的种子和风格（图数不能比那场多）
  borrow         {"from": 另一场比赛的赛事目录, "names": [名字, ...]}：这些程序两两之间的对局不重打，直接借用那场比赛
                 已有的结果（要求 maps_from 指向同一场，图才对得上）。冻结赛程时把借来的结果抄进 借用结果.jsonl；
                 评分时和自己打的结果合在一起。run 时核对：引擎和裁判的 7 个文件、编译器、这些程序的源码哈希都和
                 那场比赛冻结的一致，不一致就拒绝（借来的结果和现打的结果必须是同一套东西打出来的）
  styles         地图风格数（默认 4），图按序号轮换风格
  workers        同时打几局。0 = 绑核方案给出几个位置就用几个
  pin            {"mode": "big" | "big+small" | "none"}。big：只用大核，一个物理核只放一个 bot（默认）；
                 big+small：小核也用，同一局的两个 bot 一定在同种核上；none：不绑核。
                 也可以直接写 {"slots": [[0 号位的逻辑处理器, 1 号位的], ...]}
  batch_maps     几张图算一批（默认 1）。每批开始前、结束后各测一次速
  speed          {"enabled": true, "tolerance": 0.10, "repeat": 3, "warmup_seconds": 15, "reference_batches": 3}
                 warmup_seconds：每次 run 开打前先在 bot 要用的处理器上连续测速这么多秒再取读数（冷机时的读数偏快）；
                 reference_batches：已完成的批次达到这个数之后，"偏离基准"里的基准取这些批次开始时读数的中位数
  replays        是否保存回放（默认 true，gzip）
  boot           区间重抽样的次数（默认 1000）
  retries        裁判这一侧出错时一局最多重打几次（默认 2）
  compile_workers 统一编译时同时编几个（默认 4）

赛事目录里的文件：
  赛程.json      冻结的配置、种子、每一局的图和座位
  清单.json      第一次 run 时冻结：每个程序的哈希和顶替状态、选手包各文件的哈希、编译器、拓扑和绑核方案；
                 之后每次 run 追加一段起止时间。续跑前逐项核对，不一致就拒绝
  结果.jsonl     每局一行
  回放/          每局一个 .json.gz（和选手本地的回放是同一种格式）
  测速.jsonl     每批前后的测速和判定
  借用结果.jsonl 配置里写了 borrow 时：从另一场比赛抄来的结果（每行多一个 borrowed_from）
  裁判错误.jsonl 裁判这一侧出错、重打仍不成的局（这些局没有结果，下次 run 会再打）
  作废.jsonl     run --redo-flagged 作废掉的结果
  编译记录/      编译不过的程序的编译器输出
  成绩.md、成绩.json
"""
from __future__ import annotations

import argparse
import hashlib
import importlib
import json
import math
import os
import queue
import secrets
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import bt  # noqa: E402
import schedule  # noqa: E402

SCHEDULE, RESULTS, MANIFEST, SPEED = "赛程.json", "结果.jsonl", "清单.json", "测速.jsonl"
ERRORS, VOID, REPLAYS, BUILDLOG = "裁判错误.jsonl", "作废.jsonl", "回放", "编译记录"
BORROWED = "借用结果.jsonl"
KIT_FILES = ["tank.hpp", "tank_main.cpp", "core.py", "engine.py", "judge/referee.cpp", "judge/cycles.hpp",
             "judge/speed.cpp"]
GAME_TIMEOUT = 1200         # 一局的墙上时间超过这么多秒，认为裁判进程卡死，结束它重打
CROWDED_RATIO = 1.15        # 测速时"墙上时间 / CPU 时间"超过它，认为这个处理器上有别的任务


class Refuse(SystemExit):
    """拒绝执行：把原因说清楚再退出。"""

    def __init__(self, text):
        super().__init__("拒绝执行：" + text)


def setup_console():
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass


def sha_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def write_json(path, data):
    """先写临时文件再改名，中途被打断也不会留下半个文件。"""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(f"tmp-{os.getpid()}-{path.name}")
    tmp.write_text(json.dumps(data, ensure_ascii=False, indent=1), encoding="utf-8")
    replace_file(tmp, path)


def replace_file(tmp, path):
    """改名覆盖。Windows 上目标文件正被别的进程打开着读时会被拒绝，等一下再试（最多约 3 秒）。"""
    for attempt in range(60):
        try:
            os.replace(tmp, path)
            return
        except PermissionError:
            if attempt == 59:
                raise
            time.sleep(0.05)


def append_line(path, record):
    """追加一行 JSON：一次系统调用写完，几个进程同时写也不会互相打断。"""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    line = (json.dumps(record, ensure_ascii=False) + "\n").encode("utf-8")
    fd = os.open(path, os.O_WRONLY | os.O_APPEND | os.O_CREAT | getattr(os, "O_BINARY", 0), 0o644)
    try:
        os.write(fd, line)
    finally:
        os.close(fd)


def read_lines(path):
    path = Path(path)
    if not path.is_file():
        return []
    return [json.loads(x) for x in path.read_text(encoding="utf-8").splitlines() if x.strip()]


def stamp(t=None):
    return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(t))


# ====================================================================
# 选手包（引擎、主循环、裁判）
# ====================================================================

_kit = {}


def load_kit(kit, compiler=None):
    """从选手包目录的副本里导入 core 和 engine。返回 (core, engine)。"""
    kit = Path(kit).resolve()
    if not all(ord(ch) < 128 for ch in str(kit)):
        raise Refuse(f"选手包目录必须是纯英文路径（编译器打不开含中文的路径）：{kit}")
    for name in KIT_FILES:
        if not (kit / name).is_file():
            raise Refuse(f"选手包目录不完整，缺 {name}：{kit}")
    if compiler:
        if not Path(compiler).is_file():
            raise Refuse(f"配置里的编译器不存在：{compiler}")
        os.environ["CXX"] = str(Path(compiler).resolve())
    if _kit.get("path") == kit:
        return _kit["core"], _kit["engine"]
    if "core" in sys.modules or "engine" in sys.modules:
        loaded = Path(sys.modules.get("core", sys.modules.get("engine")).__file__).resolve().parent
        if loaded != kit:
            raise Refuse(f"core / engine 已经从别处导入（{loaded}），不能再换成 {kit}")
    sys.path.insert(0, str(kit))
    core = importlib.import_module("core")
    engine = importlib.import_module("engine")
    if Path(core.__file__).resolve().parent != kit:
        raise Refuse(f"导入的 core.py 不是选手包里的那一份：{core.__file__}")
    _kit.update(path=kit, core=core, engine=engine)
    return core, engine


def kit_hashes(kit):
    return {name: sha_file(Path(kit) / name) for name in KIT_FILES}


def compiler_info(core):
    cxx = core.find_compiler()
    if not cxx:
        raise Refuse("没有找到 C++ 编译器。在配置里用 compiler 指明 g++ 的路径。")
    env = dict(os.environ)
    env["PATH"] = str(Path(cxx).resolve().parent) + os.pathsep + env.get("PATH", "")
    try:
        r = subprocess.run([cxx, "--version"], capture_output=True, encoding="utf-8", errors="replace", timeout=60,
                           env=env, stdin=subprocess.DEVNULL)
        version = (r.stdout or r.stderr).splitlines()[0].strip()
    except (OSError, subprocess.TimeoutExpired, IndexError) as exc:
        raise Refuse(f"编译器无法运行：{cxx}（{exc}）")
    return {"path": str(cxx), "version": version, "sha256": sha_file(cxx), "flags": " ".join(core.CXX_FLAGS)}


# ====================================================================
# plan：冻结赛程
# ====================================================================

def resolve_config(cfg, base):
    """把配置里的相对路径换成绝对路径（相对配置文件所在目录）。"""
    base = Path(base)

    def full(p):
        return str((base / p).resolve())

    out = dict(cfg)
    for key in ("kit", "build_dir", "out", "compiler"):
        if out.get(key):
            out[key] = full(out[key])
    out["players"] = {n: full(p) for n, p in cfg["players"].items()}
    subs = {}
    for name, chain in (cfg.get("substitutes") or {}).items():
        subs[name] = []
        for item in chain if isinstance(chain, list) else [chain]:
            item = {"path": item} if isinstance(item, str) else dict(item)
            item["path"] = full(item["path"])
            item.setdefault("label", Path(item["path"]).name)
            subs[name].append(item)
    out["substitutes"] = subs
    out["avoid_seeds"] = [full(p) for p in cfg.get("avoid_seeds") or []]
    if out.get("maps_from"):
        out["maps_from"] = full(out["maps_from"])
    if out.get("borrow"):
        out["borrow"] = {**out["borrow"], "from": full(out["borrow"]["from"])}
    return out


def map_table(games):
    """赛程里的地图：{图的序号: (种子, 风格)}。"""
    return {g["map"]: (g["map_seed"], g["style"]) for g in games}


def borrow_rows(cfg, games):
    """按配置里的 borrow 从另一场比赛取结果。返回 (结果行的列表, 写进赛程的摘要)。对不上就拒绝。

    取的是 names 里的程序两两之间、图的序号在本场范围内的全部对局；要求每一对在每张图上正好换边两局、
    这些图在两场比赛里的种子和风格完全相同、本场的赛程里没有再排这些对。"""
    spec = cfg["borrow"]
    src = Path(spec["from"])
    names = sorted(set(spec.get("names") or []))
    if len(names) < 2:
        raise Refuse("borrow.names 至少要有两个程序")
    for n in names:
        if n not in cfg["players"]:
            raise Refuse(f"borrow.names 里的 {n} 不在参赛名单里")
    if src.resolve() == Path(cfg["out"]).resolve():
        raise Refuse("borrow.from 不能是这场比赛自己")
    if not (src / SCHEDULE).is_file() or not (src / RESULTS).is_file() or not (src / MANIFEST).is_file():
        raise Refuse(f"要借用结果的那场比赛不完整（缺赛程、结果或清单）：{src}")
    theirs, mine = map_table(read_json(src / SCHEDULE)["games"]), map_table(games)
    for m, value in sorted(mine.items()):
        if theirs.get(m) != value:
            raise Refuse(f"第 {m} 张图在两场比赛里不是同一张（这里 {value}，那场 {theirs.get(m)}），不能借用结果。"
                         "借用结果时 maps_from 要指向同一场比赛。")
    wanted = set(names)
    clash = sorted({tuple(sorted(g["seats"])) for g in games if set(g["seats"]) <= wanted})
    if clash:
        raise Refuse(f"这些对已经借用了结果，赛程里不应该再排：{clash[:5]}")
    rows = [r for r in read_lines(src / RESULTS) if r["map"] in mine and set(r["seats"]) <= wanted]
    count = {}
    for r in rows:
        if (r["map_seed"], r["style"]) != mine[r["map"]]:
            raise Refuse(f"那场比赛的结果 {r['id']} 记的地图和它的赛程对不上")
        count[r["map"], tuple(r["seats"])] = count.get((r["map"], tuple(r["seats"])), 0) + 1
    need = [(m, a, b) for m in mine for a in names for b in names if a != b]
    bad = [k for k in need if count.get((k[0], (k[1], k[2])), 0) != 1]
    if bad:
        raise Refuse(f"那场比赛里这些对局不是正好一局（图, 0 号位, 1 号位），借不全：{bad[:5]}"
                     f"{' ……' if len(bad) > 5 else ''}（共 {len(bad)} 处）")
    rows = [{**r, "borrowed_from": str(src)} for r in rows]
    return rows, {"from": str(src), "names": names, "rows": len(rows), "source_results_sha": sha_file(src / RESULTS)}


def plan(cfg, base=".", quiet=False):
    """cfg：配置字典。冻结赛程，返回赛事目录。已有赛程时拒绝。"""
    cfg = resolve_config(cfg, base)
    for key in ("kit", "build_dir", "out", "players", "maps_per_pair"):
        if not cfg.get(key):
            raise Refuse(f"赛事配置里缺 {key}")
    out = Path(cfg["out"])
    if (out / SCHEDULE).exists():
        raise Refuse(f"赛程已存在，不覆盖：{out / SCHEDULE}\n要重排请换一个赛事目录（或者确认这场比赛不要了，手工删掉整个目录）。")
    names = sorted(cfg["players"])
    anchor = cfg.get("anchor")
    if anchor is not None and anchor not in names:
        raise Refuse(f"基准 {anchor} 不在参赛名单里")
    for n in cfg.get("unranked") or []:
        if n not in names:
            raise Refuse(f"unranked 里的 {n} 不在参赛名单里")
    for n in cfg["substitutes"]:
        if n != "*" and n not in names:
            raise Refuse(f"substitutes 里的 {n} 不在参赛名单里")
    seed = cfg.get("seed")
    if seed is None:
        seed = secrets.randbits(48)
    avoid = set()
    for path in cfg["avoid_seeds"]:
        avoid |= {g["map_seed"] for g in read_json(path)["games"]}
    map_seeds = None
    if cfg.get("maps_from"):
        src = Path(cfg["maps_from"]) / SCHEDULE
        if not src.is_file():
            raise Refuse(f"maps_from 指的那场比赛没有赛程：{src}")
        theirs = map_table(read_json(src)["games"])
        if sorted(theirs) != list(range(len(theirs))):
            raise Refuse(f"{src} 里图的序号不连续，没法照着用")
        if int(cfg["maps_per_pair"]) > len(theirs):
            raise Refuse(f"要打 {cfg['maps_per_pair']} 张图，而 maps_from 指的那场比赛只有 {len(theirs)} 张")
        styles = int(cfg.get("styles", 4))
        if any(theirs[m][1] != m % styles for m in theirs):
            raise Refuse(f"{src} 里图的风格不是按序号轮换 {styles} 种排的，没法照着用")
        map_seeds = [theirs[m][0] for m in range(len(theirs))]
        if avoid & set(map_seeds[:int(cfg["maps_per_pair"])]):
            raise Refuse("maps_from 给的地图和 avoid_seeds 里要避开的地图有重复")
    try:
        games = schedule.make_schedule(names, int(cfg["maps_per_pair"]), seed, cfg.get("pairs"),
                                       int(cfg.get("styles", 4)), avoid, map_seeds)
    except ValueError as exc:
        raise Refuse(f"赛程排不出来：{exc}")
    borrowed = None
    if cfg.get("borrow"):
        borrowed, cfg["borrow"] = borrow_rows(cfg, games)
    doc = {**cfg, "seed": seed, "planned": stamp(), "audit": schedule.audit(games),
           "games_sha": schedule.games_hash(games), "games": games}
    if borrowed is not None:
        # 先写借来的结果、再写赛程：赛程在，借用结果就一定是完整的。
        out.mkdir(parents=True, exist_ok=True)
        tmp = out / f"tmp-{os.getpid()}-{BORROWED}"
        tmp.write_bytes(b"".join((json.dumps(r, ensure_ascii=False) + "\n").encode("utf-8") for r in borrowed))
        replace_file(tmp, out / BORROWED)
        doc["borrow"]["sha256"] = sha_file(out / BORROWED)
    write_json(out / SCHEDULE, doc)
    if not quiet:
        a = doc["audit"]
        print(f"已冻结赛程：{out / SCHEDULE}")
        print(f"  {len(names)} 个程序，{a['pairs']} 对，{a['maps']} 张图，共 {a['games']} 局；"
              f"每对 {a['games_per_pair_min']}–{a['games_per_pair_max']} 局，"
              f"换边{'均衡' if a['swap_balanced'] else '不均衡'}，各风格图数 {a['maps_per_style']}，赛程种子 {seed}")
        if cfg.get("maps_from"):
            print(f"  地图照搬自 {cfg['maps_from']}（前 {a['maps']} 张，种子和风格相同）")
        if borrowed is not None:
            print(f"  借用 {len(borrowed)} 局已有的结果（{'、'.join(doc['borrow']['names'])} 两两之间，不重打）：{out / BORROWED}")
    return out


def borrowed_rows(out, doc):
    """这场比赛借来的结果（没有借用返回空列表）。文件被动过就拒绝。"""
    spec = doc.get("borrow")
    if not spec:
        return []
    path = Path(out) / BORROWED
    if not path.is_file() or sha_file(path) != spec.get("sha256"):
        raise Refuse(f"{path} 不见了或者被改过（和冻结赛程时记的哈希不一样）。")
    return read_lines(path)


def check_borrow(doc, kit, compiler, entries):
    """借来的结果和现打的结果必须是同一套东西打出来的。返回不一致的说明列表（空 = 一致）。"""
    spec = doc.get("borrow")
    if not spec:
        return []
    theirs = read_json(Path(spec["from"]) / MANIFEST)["frozen"]
    bad = []
    for name, digest in theirs["kit"].items():
        if kit.get(name) != digest:
            bad.append(f"选手包里的 {name} 和那场比赛用的不是同一份（那场 {digest[:12]}，现在 {str(kit.get(name))[:12]}）")
    for key, text in (("sha256", "编译器"), ("flags", "编译参数")):
        if theirs["compiler"][key] != compiler[key]:
            bad.append(f"{text}和那场比赛不一样：那场 {theirs['compiler'][key]}，现在 {compiler[key]}")
    for name in spec["names"]:
        old, new = theirs["programs"].get(name), entries.get(name)
        if old is None:
            bad.append(f"那场比赛里没有 {name}")
        elif new is None or old["used_sha"] != new["used_sha"]:
            bad.append(f"{name} 的源码和那场比赛上场的不是同一份（那场 {old['used_sha'][:12]}，"
                       f"现在 {str((new or {}).get('used_sha'))[:12]}）")
    return bad


# ====================================================================
# 绑核与测速
# ====================================================================

def pin_plan(core, cfg, workers=0):
    """按本机拓扑和配置算出绑核方案。

    返回 {"mode", "slots": [{"class": 说明, "cpus": [[0 号位的逻辑处理器], [1 号位的]]} 或 None, ...],
          "referee_cpus": 裁判进程用的逻辑处理器（bot 用到的物理核全部让开）或 None, "note"}。"""
    pin = cfg.get("pin") or {}
    mode = pin.get("mode", "big")
    workers = int(workers or cfg.get("workers") or 0)
    topo = core.cpu_topology()
    everything = sorted(c for k in topo for c in k["cpus"])
    note = ""
    if pin.get("slots"):
        slots = [{"class": "指定", "cpus": [[int(a)] if isinstance(a, int) else [int(x) for x in a],
                                           [int(b)] if isinstance(b, int) else [int(x) for x in b]]}
                 for a, b in pin["slots"]]
        mode = "slots"
    elif mode == "none" or not topo:
        if mode != "none":
            note = "这个系统上读不到处理器拓扑，没有绑核。"
        return {"mode": "none", "slots": [None] * (workers or 4), "referee_cpus": None, "note": note}
    else:
        if mode not in ("big", "big+small"):
            raise Refuse(f"pin.mode 只能是 big、big+small、none，收到 {mode}")
        classes = sorted({k["class"] for k in topo}, reverse=True)
        slots = []
        for cls in classes if mode == "big+small" else classes[:1]:
            label = "同构核" if len(classes) == 1 else ("大核" if cls == classes[0] else "小核")
            cpus = [k["cpus"][0] for k in topo if k["class"] == cls]     # 一个物理核只取一个逻辑处理器
            slots += [{"class": label, "cpus": [[cpus[i]], [cpus[i + 1]]]} for i in range(0, len(cpus) - 1, 2)]
    if any(c > 63 for s in slots for side in s["cpus"] for c in side):
        raise Refuse("绑核只支持编号 0–63 的逻辑处理器")
    if workers:
        if workers > len(slots):
            note = f"要求同时打 {workers} 局，但绑核方案只有 {len(slots)} 个位置（不超卖），按 {len(slots)} 局跑。"
        slots = slots[:workers]
    used = {c for s in slots for side in s["cpus"] for c in side}
    blocked = set(used)
    for k in topo:                      # bot 所在物理核的另一个超线程也空出来
        if used & set(k["cpus"]):
            blocked |= set(k["cpus"])
    referee = [c for c in everything if c not in blocked and c <= 63]
    return {"mode": mode, "slots": slots, "referee_cpus": referee or None, "note": note}


def describe_plan(plan_):
    if plan_["mode"] == "none":
        return f"不绑核，同时 {len(plan_['slots'])} 局"
    parts = [f"{s['class']} {'+'.join(map(str, s['cpus'][0]))}:{'+'.join(map(str, s['cpus'][1]))}" for s in plan_["slots"]]
    ref = plan_["referee_cpus"]
    return (f"同时 {len(parts)} 局，每局两个 bot 各占一个物理核：" + "  ".join(parts)
            + (f"；裁判进程放在其余 {len(ref)} 个逻辑处理器上" if ref else ""))


def measure_speed(core, plan_, repeat=3):
    """在 bot 要用的每个逻辑处理器上同时各跑一次测速程序。返回 {"time", "cpus": {编号: {...}}, "by_class": {...}}。"""
    try:
        exe = core.build_tool("speed")
    except RuntimeError as exc:
        raise Refuse(f"测速程序编译不了：{exc}")
    targets = []
    for s in plan_["slots"]:
        if s is None:
            continue
        for side in s["cpus"]:
            targets.append((side[0], s["class"]))
    if not targets:
        targets = [(None, "不绑核")]
    results = {}

    def one(cpu):
        cmd = [str(exe), "--repeat", str(repeat)] + (["--cpu", str(cpu)] if cpu is not None else [])
        try:
            r = subprocess.run(cmd, capture_output=True, encoding="utf-8", errors="replace", timeout=300,
                               stdin=subprocess.DEVNULL)
            words = dict(w.split("=", 1) for w in r.stdout.split()[1:] if "=" in w)
            results[cpu] = {"ghz": float(words["ghz"]), "engine_ms": float(words["engine_ms"]),
                            "engine_cpu_ms": float(words["engine_cpu_ms"]), "loop_ms": float(words["loop_ms"])}
        except Exception as exc:        # 测速失败不该让比赛停下，记下来
            results[cpu] = {"error": f"{type(exc).__name__}: {exc}"[:200]}

    threads = [threading.Thread(target=one, args=(cpu,)) for cpu, _ in targets]
    began = time.time()
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    by_class = {}
    for cpu, label in targets:
        m = results[cpu]
        if "error" not in m:
            by_class.setdefault(label, []).append(m)
    summary = {label: {"cpus": len(ms), "ghz": round(statistics.median(m["ghz"] for m in ms), 3),
                       "engine_ms": round(statistics.median(m["engine_ms"] for m in ms), 2)}
               for label, ms in by_class.items()}
    # 墙上时间比 CPU 时间长出一截，说明这个处理器上还有别的任务在跑（测速程序被挤下去过）。
    crowded = sorted(cpu for cpu, _ in targets
                     if cpu is not None and "error" not in results[cpu] and results[cpu]["engine_cpu_ms"] > 0
                     and results[cpu]["engine_ms"] / results[cpu]["engine_cpu_ms"] > CROWDED_RATIO)
    return {"time": stamp(began), "epoch": round(began, 1), "seconds": round(time.time() - began, 1),
            "cpus": {str(cpu): results[cpu] for cpu, _ in targets}, "by_class": summary, "crowded": crowded,
            "errors": sum(1 for m in results.values() if "error" in m)}


def warm_up(core, plan_, seconds, repeat=3):
    """开打前预热：在 bot 要用的处理器上连续测速 seconds 秒，读数丢掉不用。返回 {"seconds", "readings": [各次的读数]}。

    为什么要：机器歇着的时候第一次测速偏快（第一场比赛：冷机 71 毫秒，跑起来之后稳定在 81 上下），
    拿它当基准，后面的批次会成片地"偏离基准"。"""
    began, readings = time.time(), []
    while seconds > 0 and time.time() - began < seconds:
        m = measure_speed(core, plan_, repeat)
        readings.append({label: v["engine_ms"] for label, v in m["by_class"].items()})
        if not m["by_class"]:
            break
    return {"seconds": round(time.time() - began, 1), "readings": readings}


def speed_baseline(rows, opening, min_batches=3):
    """这一批"开始时的读数"要和谁比。返回 (基准读数, 说明)。

    rows：测速.jsonl 里已有的记录。作废的、开始时测到处理器上有别的任务的不算；其余批次达到 min_batches 个时，基准是它们开始时读数的中位数
    （各类核、主频和固定工作量用时分别取中位数）；不够时用开赛时（预热之后）的那次读数 opening。
    用中位数的理由：单独哪一次读数偏了（冷机、别的任务抢了一下）不会把基准带偏；机器状态真的整体变了
    （比如续跑时电源模式换了）则新的批次会一直被标记，直到它们成为多数。"""
    usable = [r["before"] for r in rows if not r.get("voided") and r.get("before") and r["before"].get("by_class")
              and not r["before"].get("crowded")]       # 测的时候处理器上有别的任务：这个读数不是机器本身的状态，不进基准
    if len(usable) < max(1, int(min_batches)):
        return opening, "开赛时预热后的读数"
    by_class = {}
    for label in sorted({k for m in usable for k in m["by_class"]}):
        ms = [m["by_class"][label] for m in usable if label in m["by_class"]]
        by_class[label] = {"cpus": ms[-1].get("cpus"), "ghz": round(statistics.median(x["ghz"] for x in ms), 3),
                           "engine_ms": round(statistics.median(x["engine_ms"] for x in ms), 2)}
    return {"by_class": by_class, "batches": len(usable)}, f"已完成的 {len(usable)} 批开始时读数的中位数"


def replay_speed_flags(rows, tolerance, min_batches=3, opening=None):
    """拿一份已有的测速记录，按现在的规则重新判一遍（回算用，不改任何文件）。

    opening：当作"开赛时预热后的读数"的那一次；不给就用第一批开始时的读数。
    返回 [{"batch", "change", "drift", "reference", "why", "flagged"}, ...]。"""
    out, seen = [], []
    opening = opening or (rows[0]["before"] if rows else None)
    for r in rows:
        ref, kind = speed_baseline(seen, opening, min_batches)
        change, drift = speed_change(r["before"], r["after"]), speed_change(ref, r["before"])
        why = []
        if change is not None and change > tolerance:
            why.append(f"批内前后相差 {100 * change:.1f}%")
        if drift is not None and drift > tolerance:
            why.append(f"开始时偏离基准 {100 * drift:.1f}%（基准：{kind}）")
        if r.get("crowded"):
            why.append(f"逻辑处理器 {r['crowded']} 上有别的任务在跑")
        out.append({"batch": r["batch"], "change": change, "drift": drift, "reference": kind, "why": why, "flagged": bool(why)})
        seen.append(r)
    return out


def speed_change(a, b):
    """两次测速之间的最大相对变化（各类核的实际主频、固定工作量用时，取变化最大的一项）。算不了返回 None。"""
    worst = None
    for label, x in (a or {}).get("by_class", {}).items():
        y = (b or {}).get("by_class", {}).get(label)
        if not y:
            continue
        for key in ("ghz", "engine_ms"):
            if x[key] > 0 and y[key] > 0:
                d = abs(y[key] / x[key] - 1.0)
                worst = d if worst is None else max(worst, d)
    return worst


def speed_text(m):
    if not m or not m.get("by_class"):
        return "没有测到"
    return ("；".join(f"{label} {v['ghz']:.2f} GHz、固定工作量 {v['engine_ms']:.0f} 毫秒" for label, v in m["by_class"].items())
            + (f"（逻辑处理器 {m['crowded']} 上有别的任务在跑）" if m.get("crowded") else ""))


# ====================================================================
# 编译与顶替
# ====================================================================

def resolve_programs(core, doc, out, quiet=False):
    """在主进程里统一编译。某个程序没有文件或编译不过时依次试替补，并把状态明明白白记下来。

    返回 {名字: {"entered", "entered_sha", "status": "正常" | "顶替", "problems": [...],
                 "used", "used_sha", "used_label", "exe", "exe_sha"}}。全部候选都不行就停下。"""
    build = Path(doc["build_dir"])
    if not all(ord(ch) < 128 for ch in str(build)):
        raise Refuse(f"编译目录必须是纯英文路径：{build}")
    entries, cache = {}, {}

    def attempt(path):
        """返回 (可执行文件, 源文件哈希, 问题, 详情)。同一个文件只编一次。"""
        if path not in cache:
            p = Path(path)
            if not p.is_file():
                cache[path] = (None, None, "没有文件", "")
            else:
                digest = sha_file(p)
                if not core.is_source(p):
                    cache[path] = (p, digest, None, "")
                else:
                    try:
                        cache[path] = (core.build_bot(p, "exe", build), digest, None, "")
                    except RuntimeError as exc:
                        cache[path] = (None, digest, "编译失败", str(exc))
        return cache[path]

    # 报名的程序先几个一起编（编译器是外部进程，互不影响；同一个目标并行编也不会互相踩，见 core._run_cxx）。
    # 替补用到时再编。
    threads = max(1, int(doc.get("compile_workers", 4)))
    pending = queue.Queue()
    for path in sorted(set(doc["players"].values())):
        pending.put(path)

    def warm():
        while True:
            try:
                attempt(pending.get_nowait())
            except queue.Empty:
                return

    pool = [threading.Thread(target=warm, daemon=True) for _ in range(threads)]
    for t in pool:
        t.start()
    for t in pool:
        t.join()

    fatal = []
    for name in sorted(doc["players"]):
        entered = doc["players"][name]
        chain = [{"path": entered, "label": None}] + doc["substitutes"].get(name, []) + doc["substitutes"].get("*", [])
        e = {"entered": entered, "entered_sha": None, "status": None, "problems": []}
        for k, cand in enumerate(chain):
            exe, digest, problem, detail = attempt(cand["path"])
            if k == 0:
                e["entered_sha"] = digest
            if problem:
                e["problems"].append({"path": cand["path"], "sha256": digest, "problem": problem,
                                      "label": cand["label"] or "报名的程序"})
                if detail:
                    log = out / BUILDLOG / f"{core.safe_name(name)}-{len(e['problems'])}.txt"
                    log.parent.mkdir(parents=True, exist_ok=True)
                    log.write_text(f"{name}：{cand['path']}\n\n{detail}\n", encoding="utf-8")
                    e["problems"][-1]["log"] = str(log)
                continue
            e.update(status="正常" if k == 0 else "顶替", used=cand["path"], used_sha=digest,
                     used_label=cand["label"], exe=str(exe), exe_sha=sha_file(exe))
            break
        if e["status"] is None:
            fatal.append(f"{name}：" + "；".join(f"{p['label']} {p['problem']}（{p['path']}）" for p in e["problems"]))
        entries[name] = e
        if not quiet and e["status"] == "顶替":
            why = "；".join(f"{p['label']}{p['problem']}" for p in e["problems"])
            print(f"  顶替：{name} —— {why}，由 {e['used_label']} 上场", flush=True)
    if fatal:
        raise Refuse("下面的程序连替补都上不了场，比赛没有开始：\n  " + "\n  ".join(fatal)
                     + f"\n编译器的输出在 {out / BUILDLOG}。修好（或者在配置的 substitutes 里给出能编译的替补）后重新运行。")
    return entries


FROZEN_KEYS = ("entered_sha", "status", "used_sha")


def check_frozen(manifest, doc, kit, compiler, entries):
    """续跑前核对：赛程、选手包、编译器、每个程序。返回不一致的说明列表（空 = 一致）和只需提醒的列表。"""
    frozen = manifest["frozen"]
    bad, notes = [], []
    if frozen["schedule_sha"] != doc["games_sha"]:
        bad.append("赛程变了（赛程.json 里的对局和第一次运行时不同）")
    for name, digest in frozen["kit"].items():
        if kit.get(name) != digest:
            bad.append(f"选手包里的 {name} 变了（引擎或裁判和已经打完的那些局不是同一份）")
    for key, text in (("sha256", "编译器换了"), ("flags", "编译参数变了")):
        if frozen["compiler"][key] != compiler[key]:
            bad.append(f"{text}：原来 {frozen['compiler'][key]}，现在 {compiler[key]}")
    for name in sorted(set(frozen["programs"]) | set(entries)):
        old, new = frozen["programs"].get(name), entries.get(name)
        if old is None or new is None:
            bad.append(f"参赛名单变了：{name}")
            continue
        if old["entered_sha"] != new["entered_sha"]:
            bad.append(f"{name} 报名的程序变了：原来 {str(old['entered_sha'])[:12]}，现在 {str(new['entered_sha'])[:12]}")
        elif old["status"] != new["status"] or old["used_sha"] != new["used_sha"]:
            bad.append(f"{name} 实际上场的程序变了：原来 {old['status']} {str(old['used_sha'])[:12]}，"
                       f"现在 {new['status']} {str(new['used_sha'])[:12]}")
        elif old["exe_sha"] != new["exe_sha"]:
            notes.append(f"{name} 的可执行文件重新编译过（文件哈希不同，源码、引擎、编译器都没变）")
    return bad, notes


# ====================================================================
# run：跑比赛
# ====================================================================

class Runner:
    """管理若干个裁判进程（每个并行位一个），逐局派活。"""

    def __init__(self, doc, out, entries, plan_, session, on_row):
        self.doc, self.out, self.entries, self.plan = doc, Path(out), entries, plan_
        self.session, self.on_row = session, on_row
        self.core = _kit["core"]
        self.stop = threading.Event()
        self.procs, self.lock = [], threading.Lock()
        self.errors = 0

    def start_worker(self):
        p = subprocess.Popen([sys.executable, "-X", "utf8", str(HERE / "tournament.py"), "_worker", self.doc["kit"]],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.core.adopt(p)
        if self.plan["referee_cpus"]:
            try:
                self.core.set_affinity(p, self.plan["referee_cpus"])
            except Exception:
                pass
        with self.lock:
            self.procs.append(p)
        return p

    def play_one(self, proc, game, slot):
        """让一个裁判进程打一局。返回 (进程, 结果或 {"error"})。"""
        pin = self.plan["slots"][slot]
        job = {"cmds": [[self.entries[n]["exe"]] for n in game["seats"]], "names": game["seats"],
               "seed": game["map_seed"], "style": game["style"], "affinity": pin["cpus"] if pin else None,
               "replay": str(self.out / REPLAYS / f"{game['id']}.json.gz") if self.doc.get("replays", True) else None}
        timer = None
        try:
            if proc is None or proc.poll() is not None:
                proc = self.start_worker()
            timer = threading.Timer(GAME_TIMEOUT, proc.kill)
            timer.daemon = True
            timer.start()
            proc.stdin.write((json.dumps(job) + "\n").encode("utf-8"))
            proc.stdin.flush()
            raw = proc.stdout.readline()
            if not raw:
                raise RuntimeError(f"裁判进程异常退出（代码 {proc.poll()}）")
            return proc, json.loads(raw.decode("utf-8"))
        except Exception as exc:
            self.core.stop_process(proc)
            return None, {"error": f"{type(exc).__name__}: {exc}"}
        finally:
            if timer:
                timer.cancel()

    def run_batch(self, games, batch):
        todo = queue.Queue()
        for g in games:
            todo.put(g)
        retries = int(self.doc.get("retries", 2))

        def work(slot):
            proc = None
            while not self.stop.is_set():
                try:
                    g = todo.get_nowait()
                except queue.Empty:
                    break
                answer = None
                for attempt in range(retries + 1):
                    began = time.time()
                    proc, answer = self.play_one(proc, g, slot)
                    if "error" not in answer or self.stop.is_set():
                        break
                if self.stop.is_set() and (answer is None or "error" in answer):
                    break
                pin = self.plan["slots"][slot]
                if "error" in answer:
                    with self.lock:
                        self.errors += 1
                    append_line(self.out / ERRORS, {"id": g["id"], "seats": g["seats"], "map_seed": g["map_seed"],
                                                    "time": stamp(), "attempts": retries + 1, "error": answer["error"][:500]})
                    continue
                row = {"id": g["id"], "map": g["map"], "map_seed": g["map_seed"], "style": g["style"],
                       "seats": g["seats"], **answer, "batch": batch, "session": self.session, "slot": slot,
                       "cpus": pin["cpus"] if pin else None, "time": stamp(began)}
                self.on_row(row)
            if proc is not None:
                try:
                    proc.stdin.close()
                    proc.wait(timeout=5)
                except Exception:
                    self.core.stop_process(proc)

        threads = [threading.Thread(target=work, args=(k,), daemon=True) for k in range(len(self.plan["slots"]))]
        try:
            for t in threads:
                t.start()
            while any(t.is_alive() for t in threads):
                for t in threads:
                    t.join(timeout=0.2)
        except KeyboardInterrupt:
            self.close()
            raise

    def close(self):
        self.stop.set()
        with self.lock:
            for p in self.procs:        # 只结束自己起的进程；它们起的裁判核心和 bot 在同一个作业对象里，会跟着结束
                self.core.stop_process(p)


def worker_main(kit):
    """一个并行位：从标准输入逐行读任务，打完一局回一行。"""
    core, engine = load_kit(kit)
    out = sys.stdout.buffer
    try:
        for raw in sys.stdin.buffer:
            job = json.loads(raw.decode("utf-8"))
            began = time.perf_counter()
            try:
                result, replay = core.run_match(engine.Game, job["cmds"], job["names"], job["seed"],
                                                record=bool(job.get("replay")), affinity=job.get("affinity"),
                                                style=job["style"])
                if job.get("replay"):
                    core.save_replay(replay, job["replay"])
                answer = {"winner": result["winner"], "score": result["score"], "turns": result["turns"],
                          "reason": result["reason"], "sig": result["sig"],
                          "base_hp": result["base_hp"], "kills": result["kills"],
                          "bots": [{"dead": b["dead"], "timeouts": b["timeouts"], "illegal": b["illegal"],
                                    "first_ms": b["first_ms"], "avg_ms": b["avg_ms"], "max_ms": b["max_ms"]}
                                   for b in result["bots"]],
                          "seconds": round(time.perf_counter() - began, 3)}
            except Exception as exc:        # 裁判这一侧的错：报给主进程，这一局不算数
                answer = {"error": f"{type(exc).__name__}: {exc}"}
            out.write((json.dumps(answer, ensure_ascii=False) + "\n").encode("utf-8"))
            out.flush()
    except KeyboardInterrupt:
        pass
    finally:
        engine.shutdown()


def load_event(out):
    out = Path(out)
    if not (out / SCHEDULE).is_file():
        raise Refuse(f"这里没有赛程：{out}（先 plan）")
    doc = read_json(out / SCHEDULE)
    if schedule.games_hash(doc["games"]) != doc["games_sha"]:
        raise Refuse(f"{out / SCHEDULE} 里的对局和它自己记的指纹对不上，赛程被改过。")
    return out, doc


def void_flagged(out, doc, quiet=False):
    """把测速标记过的批次的结果挪进 作废.jsonl，让它们重打。返回作废的局数。"""
    flagged = {r["batch"] for r in read_lines(out / SPEED) if r.get("flagged") and not r.get("voided")}
    if not flagged:
        return 0
    rows = read_lines(out / RESULTS)
    keep = [r for r in rows if r["batch"] not in flagged]
    gone = [r for r in rows if r["batch"] in flagged]
    for r in gone:
        append_line(out / VOID, {**r, "voided": stamp(), "why": "所在批次测速前后不一致"})
        replay = out / REPLAYS / f"{r['id']}.json.gz"
        if replay.is_file():
            (out / REPLAYS / "作废").mkdir(parents=True, exist_ok=True)
            os.replace(replay, out / REPLAYS / "作废" / f"{r['id']}-{int(time.time())}.json.gz")
    tmp = out / f"tmp-{os.getpid()}-{RESULTS}"
    tmp.write_bytes(b"".join((json.dumps(r, ensure_ascii=False) + "\n").encode("utf-8") for r in keep))
    os.replace(tmp, out / RESULTS)
    speed = read_lines(out / SPEED)
    for r in speed:
        if r.get("flagged") and r["batch"] in flagged:
            r["voided"] = stamp()
    tmp = out / f"tmp-{os.getpid()}-{SPEED}"
    tmp.write_bytes(b"".join((json.dumps(r, ensure_ascii=False) + "\n").encode("utf-8") for r in speed))
    os.replace(tmp, out / SPEED)
    if not quiet:
        print(f"已作废 {len(gone)} 局（批次 {sorted(flagged)}），接下来重打。", flush=True)
    return len(gone)


def run(out, workers=0, redo_flagged=False, quiet=False):
    """跑比赛。可以随时中断；再次运行会核对哈希后接着跑。返回摘要字典。"""
    out, doc = load_event(out)
    core, engine = load_kit(doc["kit"], doc.get("compiler"))
    say = (lambda *a: None) if quiet else (lambda *a: print(*a, flush=True))
    if redo_flagged:
        void_flagged(out, doc, quiet)
    done = {r["id"] for r in read_lines(out / RESULTS)}
    todo = [g for g in doc["games"] if g["id"] not in done]
    title = doc.get("title") or out.name
    say(f"【{title}】共 {len(doc['games'])} 局，已完成 {len(done)}，本次要打 {len(todo)}。")

    # —— 统一编译，核对 ——
    began = time.time()
    inf = engine.info()
    if any(g["style"] >= len(inf["styles"]) for g in doc["games"]):
        raise Refuse(f"赛程里的地图风格编号超出了引擎的 {len(inf['styles'])} 种")
    compiler = compiler_info(core)
    say(f"编译器 {compiler['version']}，参数 {compiler['flags']}；引擎 {inf['engine']}。统一编译 {len(doc['players'])} 个程序……")
    entries = resolve_programs(core, doc, out, quiet)
    kit = kit_hashes(doc["kit"])
    bad = check_borrow(doc, kit, compiler, entries)
    if bad:
        raise Refuse(f"这场比赛借用了 {doc['borrow']['from']} 的结果，但现在这一套和那场比赛的不一样，没有开打：\n  - "
                     + "\n  - ".join(bad))
    if doc.get("borrow"):
        borrowed_rows(out, doc)
        say(f"  借用的 {doc['borrow']['rows']} 局已核对：引擎和裁判的文件、编译器、{len(doc['borrow']['names'])} 个程序的源码都和那场比赛一致。")
    manifest_path = out / MANIFEST
    if manifest_path.is_file():
        manifest = read_json(manifest_path)
        bad, notes = check_frozen(manifest, doc, kit, compiler, entries)
        if bad:
            raise Refuse(f"和这场比赛第一次运行时冻结的状态不一致，不能接着跑（{manifest_path}）：\n  - " + "\n  - ".join(bad)
                         + "\n已经打完的局是用原来那一套打的。把文件恢复原样后再运行；"
                           "如果确实要用新的一套，换一个赛事目录重新 plan。")
        for n in notes:
            say("  提醒：" + n)
        manifest["frozen"]["programs"] = {n: {**manifest["frozen"]["programs"][n], "exe": e["exe"], "exe_sha": e["exe_sha"]}
                                          for n, e in entries.items()}
    else:
        if done:
            raise Refuse(f"{out} 里已经有结果，却没有 {MANIFEST}，没法核对这些结果是用什么打的。")
        manifest = {"title": title, "created": stamp(),
                    "frozen": {"schedule_sha": doc["games_sha"], "kit": kit, "compiler": compiler, "programs": entries},
                    "engine": inf["engine"], "budget": {k: inf[k] for k in ("budget_kind", "first_ms", "turn_ms",
                                                                              "wall_first_ms", "wall_turn_ms",
                                                                              "units_per_ms", "units_source")},
                    "config": {k: v for k, v in doc.items() if k not in ("games", "audit")},
                    "topology": core.cpu_topology(), "sessions": [], "speed_reference": None}
    plan_ = pin_plan(core, doc, workers)
    say("绑核：" + describe_plan(plan_) + ("\n  " + plan_["note"] if plan_["note"] else ""))
    session = len(manifest["sessions"]) + 1
    for s in manifest["sessions"]:
        if not s.get("end") and not s.get("killed"):
            # 上一次运行被强行结束，没来得及写收尾：按结果文件把它打了几局、打到什么时候补上。
            mine = [r for r in read_lines(out / RESULTS) if r["session"] == s["session"]]
            last = max((time.mktime(time.strptime(r["time"], "%Y-%m-%d %H:%M:%S")) + r["seconds"] for r in mine),
                       default=s["start_epoch"])
            s.update(killed=True, interrupted=True, played=len(mine), seconds=round(max(0.0, last - s["start_epoch"]), 1))
            say(f"  提醒：第 {s['session']} 次运行没有正常结束（被强行中断）。它打完的 {len(mine)} 局都在，结果不受影响。")
    record = {"session": session, "start": stamp(), "start_epoch": round(time.time(), 1), "end": None,
              "plan": plan_, "compile_seconds": round(time.time() - began, 1), "played": 0, "errors": 0,
              "interrupted": False, "python": sys.version.split()[0]}
    manifest["sessions"].append(record)
    write_json(manifest_path, manifest)
    summary = {"out": str(out), "entries": entries, "played": 0, "errors": 0, "flagged": [], "interrupted": False,
               "remaining": len(todo)}
    if not todo:
        record.update(end=stamp(), end_epoch=round(time.time(), 1))
        write_json(manifest_path, manifest)
        say("没有要打的局。")
        return summary

    # —— 逐批：测速 → 打 → 测速 ——
    speed_cfg = {"enabled": True, "tolerance": 0.10, "repeat": 3, "warmup_seconds": 15, "reference_batches": 3,
                 **(doc.get("speed") or {})}
    per_batch = max(1, int(doc.get("batch_maps", 1)))
    batches = {}
    for g in todo:
        batches.setdefault(g["map"] // per_batch, []).append(g)
    lock = threading.Lock()
    started, total = time.time(), len(todo)
    last_print = [time.time()]

    def on_row(row):
        with lock:
            append_line(out / RESULTS, row)
            record["played"] += 1
            n = record["played"]
            now = time.time()
            if now - last_print[0] >= 15 or n == total:
                last_print[0] = now
                left = (total - n) * (now - started) / n
                say(f"  {len(done) + n}/{len(doc['games'])} 局  已用 {(now - started) / 60:.1f} 分钟，约剩 {left / 60:.1f} 分钟")

    runner = Runner(doc, out, entries, plan_, session, on_row)
    try:
        if speed_cfg["enabled"]:
            # 预热之后再取这次运行的第一个读数（冷机时的读数偏快，不能当基准）。
            record["warmup"] = warm_up(core, plan_, float(speed_cfg["warmup_seconds"]), speed_cfg["repeat"])
            opening = measure_speed(core, plan_, speed_cfg["repeat"])
            record["speed_opening"] = opening
            if manifest["speed_reference"] is None:
                manifest["speed_reference"] = opening
            write_json(manifest_path, manifest)
            say(f"预热 {record['warmup']['seconds']:.0f} 秒后测速：{speed_text(opening)}")
            if opening.get("crowded"):
                say(f"  ！bot 要用的逻辑处理器 {opening['crowded']} 上现在有别的任务在跑。正式比赛应当先把它们停掉；"
                    "继续跑的话这些批次会被标记。")
        for batch in sorted(batches):
            games = batches[batch]
            before = measure_speed(core, plan_, speed_cfg["repeat"]) if speed_cfg["enabled"] else None
            reference, ref_kind = speed_baseline(read_lines(out / SPEED), manifest["speed_reference"],
                                                 speed_cfg["reference_batches"])
            t0 = time.time()
            runner.run_batch(games, batch)
            after = measure_speed(core, plan_, speed_cfg["repeat"]) if speed_cfg["enabled"] else None
            change = speed_change(before, after)
            drift = speed_change(reference, before)
            tol = float(speed_cfg["tolerance"])
            crowded = sorted(set((before or {}).get("crowded", [])) | set((after or {}).get("crowded", [])))
            why = []
            if change is not None and change > tol:
                why.append(f"批内前后相差 {100 * change:.1f}%")
            if drift is not None and drift > tol:
                why.append(f"开始时偏离基准 {100 * drift:.1f}%（基准：{ref_kind}）")
            if crowded:
                why.append(f"逻辑处理器 {crowded} 上有别的任务在跑")
            append_line(out / SPEED, {"batch": batch, "session": session, "games": len(games),
                                      "seconds": round(time.time() - t0, 1), "before": before, "after": after,
                                      "change": None if change is None else round(change, 4),
                                      "drift_from_reference": None if drift is None else round(drift, 4),
                                      "reference": {"kind": ref_kind, "by_class": (reference or {}).get("by_class")},
                                      "crowded": crowded, "tolerance": tol, "flagged": bool(why), "why": why})
            if why:
                summary["flagged"].append(batch)
                say(f"  ！批次 {batch} 被测速标记（{'；'.join(why)}；阈值 {100 * tol:.0f}%）：前 {speed_text(before)}；后 {speed_text(after)}。"
                    "结果照常保留。")
    except KeyboardInterrupt:
        record["interrupted"] = summary["interrupted"] = True
        say("\n已中断，自己起的进程已清理。打完的局都已保存，再次运行会接着跑。")
    finally:
        runner.close()
        record.update(end=stamp(), end_epoch=round(time.time(), 1), errors=runner.errors,
                      seconds=round(time.time() - started, 1))
        write_json(manifest_path, manifest)
    left = len(doc["games"]) - len(done) - record["played"]
    summary.update(played=record["played"], errors=runner.errors, remaining=left, seconds=record["seconds"])
    if not summary["interrupted"]:
        say(f"本次打了 {record['played']} 局，用时 {record['seconds'] / 60:.1f} 分钟"
            + (f"；有 {runner.errors} 局裁判这一侧出错没有结果（见 {ERRORS}），再运行一次会重打" if runner.errors else "")
            + (f"；还剩 {left} 局" if left else "，全部打完。"))
    return summary


# ====================================================================
# rate：评分
# ====================================================================

REASONS = {"base": "基地被毁", "hp": "到回合上限比基地血量", "kills": "到回合上限比击毁数", "stars": "到回合上限比吃星数",
           "damage": "到回合上限比基地伤害", "draw": "到回合上限平局", "fault": "故障判负"}


def reason_text(reason):
    if reason.startswith("both_"):
        return "两个基地同回合被毁"
    return REASONS.get(reason, reason)


def bt_rows(rows, key=None):
    """结果行 → bt.py 要的格式。key 给了就用 (key, 图号) 作地图标识（几场比赛合并时用）。"""
    return [{"map": r["map"] if key is None else (key, r["map"]), "a": r["seats"][0], "b": r["seats"][1],
             "score": r["score"][0]} for r in rows]


def collect_stats(rows, names):
    """每个程序的胜平负、故障、用量，以及两两之间的胜平负矩阵。"""
    stats = {n: {"games": 0, "w": 0, "d": 0, "l": 0, "forfeits": 0, "faulty_games": 0, "timeout_turns": 0,
                 "illegal_turns": 0, "turns": 0, "max_ms": 0.0, "avg_ms_sum": 0.0, "first_ms_max": 0.0} for n in names}
    matrix = {a: {b: [0, 0, 0] for b in names if b != a} for a in names}
    for r in rows:
        for seat, n in enumerate(r["seats"]):
            other = r["seats"][1 - seat]
            s, b = stats[n], r["bots"][seat]
            k = 1 if r["winner"] < 0 else (0 if r["winner"] == seat else 2)
            s["games"] += 1
            s["wdl"[k]] += 1
            if n != other:
                matrix[n][other][k] += 1
            s["forfeits"] += bool(b["dead"])
            s["faulty_games"] += bool(b["dead"] or b["timeouts"] or b["illegal"])
            s["timeout_turns"] += b["timeouts"]
            s["illegal_turns"] += b["illegal"]
            s["turns"] += r["turns"]
            s["max_ms"] = max(s["max_ms"], b["max_ms"])
            s["first_ms_max"] = max(s["first_ms_max"], b["first_ms"])
            s["avg_ms_sum"] += b["avg_ms"]
    for s in stats.values():
        g = s["games"]
        s["avg_turns"] = round(s.pop("turns") / g, 1) if g else None
        s["avg_ms"] = round(s.pop("avg_ms_sum") / g, 3) if g else None
        s["score_rate"] = round((s["w"] + 0.5 * s["d"]) / g, 4) if g else None
    return stats, matrix


def show_name(name, labels, display=None):
    """成绩表里怎么写一个程序：有显示名的是“显示名（名字）”，再带上说明（基准、出题方参照）。"""
    shown = (display or {}).get(name)
    text = f"{shown}（{name}）" if shown and shown != name else name
    return f"{text}（{labels[name]}）" if labels.get(name) else text


def display_of(data):
    return {n: p.get("display") for n, p in data["players"].items()}


def fmt(v, digits=0):
    if v is None:
        return "—"
    v = round(v, digits)
    return f"{v + 0.0 if v else 0.0:.{digits}f}"        # 不显示 "-0"


def rate(out, boot=None, quiet=False, write=True, only=None, unranked=None, labels=None, display=None, title=None):
    """评分。返回成绩字典（也就是 成绩.json 的内容）。

    only：只看这些程序（它们之间的对局），别的程序和有它们参加的对局都不算——"某位选手的视角"用它。
    这时通常同时给 unranked / labels / display / title 来盖掉赛程里的设定，并且 write=False（不覆盖整场的成绩文件）。"""
    out, doc = load_event(out)
    own_rows = read_lines(out / RESULTS)
    rows = own_rows + borrowed_rows(out, doc)
    names = sorted(doc["players"])
    if only is not None:
        unknown = sorted(set(only) - set(names))
        if unknown:
            raise Refuse(f"only 里的 {unknown} 不在参赛名单里")
        names = sorted(set(only))
        rows = [r for r in rows if set(r["seats"]) <= set(names)]
    anchor = doc.get("anchor")
    if anchor is None:
        raise Refuse("这场比赛没有指定基准（anchor），定不了零点。")
    if not rows:
        raise Refuse(f"{out} 里还没有结果。")
    labels = (doc.get("labels") or {}) if labels is None else labels
    display = (doc.get("display") or {}) if display is None else display
    unranked = set((doc.get("unranked") or []) if unranked is None else unranked) | {anchor}
    boot = int(boot if boot is not None else doc.get("boot", 1000))
    def ordered(r):
        order_ = sorted(names, key=lambda n: (-(r[n]["rating"] if r[n]["rating"] is not None else -1e9), n))
        return order_, [n for n in order_ if n not in unranked and r[n]["rating"] is not None]

    try:
        # 先只拟合一次定出名次，再重抽样：相邻两名之差的区间要在每次重抽样里直接算，所以得先知道谁和谁相邻。
        point, _ = bt.rate(bt_rows(rows), names, anchor, boot=0)
        _, ranked = ordered(point)
        adjacent = list(zip(ranked, ranked[1:]))
        ratings, boots, diffs = bt.rate(bt_rows(rows), names, anchor, boot=boot, diffs=adjacent)
    except ValueError as exc:
        raise Refuse(f"评分算不了：{exc}")
    stats, matrix = collect_stats(rows, names)
    order, ranked = ordered(ratings)
    groups = bt.tie_groups_by_diff(ranked, diffs)
    rank, tie = {}, {}
    for g in groups:
        first = len(rank) + 1
        for n in g:
            rank[n] = len(rank) + 1
            tie[n] = [first, first + len(g) - 1] if len(g) > 1 else None
    manifest = read_json(out / MANIFEST) if (out / MANIFEST).is_file() else None
    programs = manifest["frozen"]["programs"] if manifest else {}
    speed = read_lines(out / SPEED)
    flagged = sorted({r["batch"] for r in speed if r.get("flagged") and not r.get("voided")})
    errors = read_lines(out / ERRORS)
    done_ids = {r["id"] for r in own_rows}       # 借来的结果用的是那场比赛的局号，不能拿来对本场的赛程
    scheduled = [g for g in doc["games"] if only is None or set(g["seats"]) <= set(names)]
    missing = [g["id"] for g in scheduled if g["id"] not in done_ids]
    n_borrowed = sum(1 for r in rows if r.get("borrowed_from"))
    reasons = {}
    for r in rows:
        reasons[r["reason"]] = reasons.get(r["reason"], 0) + 1
    draws = sum(1 for r in rows if r["winner"] < 0)
    seat0 = sum(1 for r in rows if r["winner"] == 0)
    players = {}
    for n in names:
        e = programs.get(n, {})
        players[n] = {"rank": rank.get(n), "tie": tie.get(n), **ratings[n], **stats[n],
                      "label": labels.get(n), "display": display.get(n), "ranked": n not in unranked,
                      "status": e.get("status"), "used_label": e.get("used_label"),
                      "problems": [p["problem"] for p in e.get("problems", [])]}
    data = {"title": title or doc.get("title") or out.name, "rated": stamp(), "anchor": anchor, "names": names, "order": order,
            "games": len(rows), "expected_games": len(scheduled) + n_borrowed, "complete": not missing,
            "borrowed_games": n_borrowed, "borrowed_from": (doc.get("borrow") or {}).get("from") if n_borrowed else None,
            "all_pairs": all(sum(c) > 0 for row in matrix.values() for c in row.values()),
            "maps": len({r["map"] for r in rows}), "boot": boots, "players": players, "matrix": matrix,
            "adjacent": [{"high": a, "low": b, "diff": diffs[a, b]["diff"], "diff_low": diffs[a, b]["low"],
                          "diff_high": diffs[a, b]["high"],
                          "tied": diffs[a, b]["low"] is not None and diffs[a, b]["low"] <= 0.0} for a, b in adjacent],
            "overall": {"avg_turns": round(sum(r["turns"] for r in rows) / len(rows), 1),
                        "draw_rate": round(draws / len(rows), 4), "draws": draws,
                        "seat0_wins": seat0, "seat1_wins": len(rows) - draws - seat0,
                        "fault_games": sum(1 for r in rows if r["reason"] == "fault"), "reasons": reasons,
                        "seconds_per_game": round(sum(r["seconds"] for r in rows) / len(rows), 3)},
            "substitutions": [{"name": n, "used_label": e["used_label"],
                               "why": [f"{p['label']}{p['problem']}" for p in e["problems"]]}
                              for n, e in sorted(programs.items()) if e.get("status") == "顶替" and n in names],
            "flagged_batches": flagged,
            "judge_errors": len({e["id"] for e in errors if only is None or set(e["seats"]) <= set(names)} - done_ids)}
    text = render_scores(data)
    if write:
        (out / "成绩.md").write_text(text, encoding="utf-8")
        write_json(out / "成绩.json", data)
    if not quiet:
        print(text)
    return data


def rank_text(p):
    if p["rank"] is None:
        return "—"
    if p["tie"]:
        return f"{p['rank']}（{p['tie'][0]}–{p['tie'][1]} 并列）"
    return str(p["rank"])


def ranking_table(data, mark=None):
    """名次表（Markdown 行的列表）。mark：要加粗的名字。"""
    lines = ["| 名次 | 选手 | 实力值 | 95% 区间 | 胜 | 平 | 负 | 得分率 | 故障判负局 | 有故障的局 | 平均局长 | 每回合用量：平均 / 最大 |",
             "|---|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---|"]
    labels = {n: p["label"] for n, p in data["players"].items()}
    for n in data["order"]:
        p = data["players"][n]
        name = show_name(n, labels, display_of(data)) + ("（由" + str(p["used_label"]) + "顶替）" if p["status"] == "顶替" else "")
        if n == mark:
            name = f"**{name}**"
        ci = "—" if p["low"] is None or n == data["anchor"] else f"{fmt(p['low'])} ~ {fmt(p['high'])}"
        lines.append(f"| {rank_text(p)} | {name} | {fmt(p['rating'])} | {ci} | {p['w']} | {p['d']} | {p['l']} | "
                     f"{'—' if p['score_rate'] is None else format(100 * p['score_rate'], '.1f') + '%'} | "
                     f"{p['forfeits']} | {p['faulty_games']} | {fmt(p['avg_turns'], 0)} | "
                     f"{fmt(p['avg_ms'], 1)} / {fmt(p['max_ms'], 1)} |")
    return lines


TIE_RULE = ("并列的判法：把选手按实力值排好，看相邻两名“实力值之差”的 95% 区间（每次重抽样里直接算两人的差）。"
            "这个区间含 0，说明现有的对局分不出这两位谁强，标为并列；并列可以连成一串。"
            "表里每位选手自己的区间是他相对基准的实力值的区间，只用来看这个数有多准，不用来判并列："
            "两位选手各自的区间有重叠，不等于他们并列。")


def adjacent_table(data):
    """相邻名次之差（Markdown 行的列表，末尾带一个空行）。旧的成绩文件里没有这一项时返回空列表。"""
    rows = data.get("adjacent")
    if not rows:
        return []
    labels = {n: p["label"] for n, p in data["players"].items()}
    lines = ["## 相邻名次之差", "", "| 相邻的两名 | 实力值之差 | 差的 95% 区间 | 判定 |", "|---|---:|---|---|"]
    for r in rows:
        a, b = data["players"][r["high"]], data["players"][r["low"]]
        ci = "算不出（图太少）" if r["diff_low"] is None else f"{fmt(r['diff_low'])} ~ {fmt(r['diff_high'])}"
        lines.append(f"| 第 {a['rank']} 名 {show_name(r['high'], labels, display_of(data))} − "
                     f"第 {b['rank']} 名 {show_name(r['low'], labels, display_of(data))} | "
                     f"{fmt(r['diff'])} | {ci} | {'并列（区间含 0）' if r['tied'] else '分得开'} |")
    return lines + [""]


def matrix_table(data):
    labels = {n: p["label"] for n, p in data["players"].items()}
    order = data["order"]
    lines = ["| 行 对 列 | " + " | ".join(order) + " |", "|---|" + "---|" * len(order)]
    for a in order:
        cells = []
        for b in order:
            if a == b:
                cells.append("·")
            else:
                w, d, l = data["matrix"][a][b]
                cells.append(f"{w}-{d}-{l}" if w + d + l else "")
        lines.append(f"| {show_name(a, labels, display_of(data))} | " + " | ".join(cells) + " |")
    return lines


def render_scores(data):
    o = data["overall"]
    pairing = ("每张图上每一对换边各打一局" if data.get("all_pairs", True)
               else "每张图上要交手的每一对换边各打一局；不是全配对，没有交手的在下面的两两表里是空格")
    lines = [f"# {data['title']} · 成绩", "",
             f"{data['games']} 局（{data['maps']} 张图，{pairing}）。"
             + (f"其中 {data['borrowed_games']} 局不是这次打的，借用自 {data['borrowed_from']}（同样的图、同样的程序）。"
                if data.get("borrowed_games") else "")
             + f"Bradley–Terry 拟合：胜 1、平 0.5，每对加一局虚拟平局；{data['anchor']} = 0，强 10 倍 = +400。"
             f"区间是按地图整块重抽样 {data['boot']} 次的 95% 区间。", "", TIE_RULE, ""]
    if not data["complete"]:
        lines += [f"**注意：赛程共 {data['expected_games']} 局，只完成了 {data['games']} 局，以下是中途成绩。**", ""]
    lines += ranking_table(data)
    lines += ["", f"全场：平均每局 {o['avg_turns']} 回合，平局率 {100 * o['draw_rate']:.1f}%"
                  f"（{o['draws']} 局），0 号位胜 {o['seat0_wins']} 局、1 号位胜 {o['seat1_wins']} 局，"
                  f"因故障结束 {o['fault_games']} 局。结束方式："
                  + "，".join(f"{reason_text(k)} {v}" for k, v in sorted(o["reasons"].items(), key=lambda kv: -kv[1])) + "。",
              "", "“故障判负局”是这个程序崩溃、退出或连续 3 次超预算 / 输出不合法而被判负的局数；"
                  "“有故障的局”还包括出现过超预算或不合法输出但没有判负的局。用量的单位是 CPU 毫秒。",
              ""]
    lines += adjacent_table(data)
    lines += ["## 两两胜平负（行的程序 对 列的程序：胜-平-负）", ""]
    lines += matrix_table(data)
    notes = []
    for s in data["substitutions"]:
        notes.append(f"- 顶替：{s['name']} —— {'；'.join(s['why'])}，由 {s['used_label']} 上场。")
    if data["flagged_batches"]:
        notes.append(f"- 测速前后不一致的批次：{data['flagged_batches']}（详见 测速.jsonl）。这些批次的结果仍计入。")
    if data["judge_errors"]:
        notes.append(f"- 有 {data['judge_errors']} 局因裁判这一侧出错没有结果，不计入。")
    if notes:
        lines += ["", "## 需要注意的", ""] + notes
    return "\n".join(lines) + "\n"


def status(out):
    out, doc = load_event(out)
    rows = read_lines(out / RESULTS)
    print(f"【{doc.get('title') or out.name}】{len(rows)}/{len(doc['games'])} 局")
    if (out / MANIFEST).is_file():
        m = read_json(out / MANIFEST)
        for n, e in sorted(m["frozen"]["programs"].items()):
            if e["status"] == "顶替":
                print(f"  顶替：{n} —— " + "；".join(f"{p['label']}{p['problem']}" for p in e["problems"]) + f"，由 {e['used_label']} 上场")
        for s in m["sessions"]:
            print(f"  第 {s['session']} 次运行：{s['start']} → {s['end'] or '没有正常结束'}，打了 {s['played']} 局"
                  + ("，被中断" if s["interrupted"] else "") + f"，{describe_plan(s['plan'])}")
    for r in read_lines(out / SPEED):
        if r.get("flagged"):
            print(f"  测速标记：批次 {r['batch']}（{'；'.join(r.get('why') or [])}）" + ("，已作废重打" if r.get("voided") else ""))
    errors = read_lines(out / ERRORS)
    if errors:
        print(f"  裁判错误记录 {len(errors)} 条，最后一条：{errors[-1]['error'][:120]}")


# ====================================================================
# estimate：耗时估算
# ====================================================================

# 实测常数：彩排三场比赛 928 局的拟合（主办方/彩排数据分析.md 第二部分；省电状态、只用大核同时 4 局）。
# 一局的墙上时间 ≈ 0.02 秒 + 首回合用量 + 回合数 ×（慢的一方的每回合平均用量 + 1.0 毫秒），残差的标准差 0.026 秒。
OVERHEAD_GAME = 0.02        # 每局固定开销（秒）：起两个 bot 进程、收尾、存回放
OVERHEAD_TURN = 0.001       # 每回合裁判自己的开销（秒）。彩排之前按 3 毫秒估，把彩排的总用时高估了 28%
FULL_BUDGET_MS = 52.5       # "用满 50"按 52.5 算：用量很高时每回合的额外开销更大（复核时量过 45.1 → 墙上 47.9），多留 5%
SPEED_TEST = 0.7            # 每批前后各一次测速（秒）
REHEARSAL_TURNS = 53.4      # 彩排实测的平均局长（回合）；92% 的局不超过 70 回合，打满 300 回合的占 0.86%
REHEARSAL_P99_TURNS = 136   # 彩排局长的 99% 分位：估"批尾等最长的一局"用
WORST_TURNS = 70            # "最坏"口径用的平均局长


def game_seconds(turns, turn_ms, first_ms):
    """一局的墙上时间：两个 bot 同时想，所以每回合等的是较慢的一方。"""
    return OVERHEAD_GAME + first_ms / 1000.0 + turns * (turn_ms / 1000.0 + OVERHEAD_TURN)


def estimate_seconds(pairs, maps, slots, per_game, batch_maps=1, longest_game=0.0):
    """总用时 = 各局用时之和 ÷ 并行数 + 每批（前后两次测速 + 批尾等最后几局时别的并行位空着，约半局最长的）。

    用它回算彩排的三场比赛是 110、107、87 秒，实际 114、105、84 秒。"""
    games = pairs * maps * 2
    batches = math.ceil(maps / batch_maps)
    return math.ceil(games / slots) * per_game + batches * (2 * SPEED_TEST + 0.5 * longest_game), games


def estimate_cases(turns=WORST_TURNS):
    """(栏目名, 一局的秒数, 批内最长的一局的秒数)。前三栏是"彩排实测局长"口径，后两栏是"最坏"口径。"""
    real = REHEARSAL_TURNS
    cases = []
    for label, ms, first in ((f"实测局长 {real:g} 回合，每回合约 5（像彩排里用得少的选手）", 5.0, 15.0),
                             (f"实测局长 {real:g} 回合，每回合约 25（像彩排里用得多的选手）", 25.0, 35.0),
                             (f"实测局长 {real:g} 回合，全部用满 50", FULL_BUDGET_MS, 60.0)):
        cases.append((label, game_seconds(real, ms, first), game_seconds(REHEARSAL_P99_TURNS, ms, first)))
    for label, first in ((f"最坏：平均 {turns:g} 回合，全部用满 50", 60.0),
                         (f"最坏，且首回合也用满 2000", 2000.0)):
        cases.append((label, game_seconds(turns, FULL_BUDGET_MS, first), game_seconds(300, FULL_BUDGET_MS, first)))
    return cases


def estimate(programs=14, slots=4, turns=WORST_TURNS, max_n=40, pairs=None):
    head = f"{programs} 个程序全配对，" if pairs is None else ""
    pairs = pairs if pairs is not None else programs * (programs - 1) // 2
    cases = estimate_cases(turns)
    print(f"{head}{pairs} 对；同时 {slots} 局；每张图 {pairs * 2} 局。单位：分钟。")
    print("两种口径：“实测局长”按彩排实测的平均局长算（三栏是选手每回合用量的三种情况）；“最坏”按更长的局、全部用满预算算。"
          "式子是彩排 928 局拟合出来的（每回合的额外开销约 1 毫秒）。按每一对都是选手的用量算，没有单独扣掉基准、陪练那几对，略偏保守。")
    print("| N（每对的图数） | 总局数 | " + " | ".join(c[0] for c in cases) + " |")
    print("|---:|---:|" + "---:|" * len(cases))
    for n in [x for x in (1, 2, 3, 4, 5, 6, 8, 10, 12, 15, 20, 25, 30, 36, 40, 50, 60, 64, 80, 100) if x <= max_n]:
        cells = []
        for _, per, longest in cases:
            sec, games = estimate_seconds(pairs, n, slots, per, longest_game=longest)
            cells.append(f"{sec / 60:.1f}")
        print(f"| {n} | {games} | " + " | ".join(cells) + " |")
    return cases


# ====================================================================

def main():
    setup_console()
    if sys.argv[1:2] == ["_worker"]:
        return worker_main(sys.argv[2])
    ap = argparse.ArgumentParser(description="坦克大战正式赛", formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("plan", help="冻结赛程")
    p.add_argument("config")
    p = sub.add_parser("run", help="跑比赛（可中断续跑）")
    p.add_argument("out")
    p.add_argument("--workers", type=int, default=0, help="同时打几局（默认按配置；0 = 绑核方案的全部位置）")
    p.add_argument("--redo-flagged", action="store_true", help="先把测速标记过的批次作废，再重打")
    p = sub.add_parser("rate", help="评分")
    p.add_argument("out")
    p.add_argument("--boot", type=int, default=None)
    p = sub.add_parser("status", help="进度")
    p.add_argument("out")
    p = sub.add_parser("cpus", help="看绑核方案")
    p.add_argument("config")
    p = sub.add_parser("estimate", help="N 与耗时的对照表")
    p.add_argument("--programs", type=int, default=14)
    p.add_argument("--pairs", type=int, default=None, help="直接给对数（不是全配对时用）")
    p.add_argument("--slots", type=int, default=4, help="同时打几局")
    p.add_argument("--turns", type=int, default=WORST_TURNS, help="“最坏”口径用的平均局长")
    p.add_argument("--max-n", type=int, default=64)
    a = ap.parse_args()
    if a.cmd == "plan":
        plan(read_json(a.config), Path(a.config).resolve().parent)
    elif a.cmd == "run":
        s = run(a.out, a.workers, a.redo_flagged)
        if s["interrupted"]:
            raise SystemExit(130)
        if s["remaining"]:
            raise SystemExit(2)
    elif a.cmd == "rate":
        rate(a.out, a.boot)
    elif a.cmd == "status":
        status(a.out)
    elif a.cmd == "cpus":
        cfg = resolve_config(read_json(a.config), Path(a.config).resolve().parent)
        core, _ = load_kit(cfg["kit"], cfg.get("compiler"))
        for k in core.cpu_topology():
            print(f"  物理核 {k['core']:2d}  效率等级 {k['class']}  逻辑处理器 {k['cpus']}")
        print(describe_plan(pin_plan(core, cfg)))
    else:
        estimate(a.programs, a.slots, a.turns, a.max_n, a.pairs)


if __name__ == "__main__":
    main()
