"""对战平台核心（坦克大战版）：编译 bot、启动 bot 进程、逐回合收发并计量思考预算、运行单局、读写回放。

这里不含任何游戏规则。游戏模块（engine.py）提供一个 Game 类：
    Game(n, seed, **选项)      n 个参赛方（坦克大战恒为 2），seed 决定地图
    game.done、game.turn       是否结束、已结算的回合数
    game.TIME_LIMITS           思考预算：(首回合秒数, 之后每回合秒数)。单位见下面"思考预算"一节
    game.WALL_CAPS             （可选）按 CPU 计时时的墙上保险上限：(首回合秒数, 之后每回合秒数)
    game.encode(i)             发给第 i 方的本回合输入（文本，以换行结尾）
    game.decode(i, line)       把第 i 方的一行回复解析成命令；格式不对时抛出 ValueError
    game.step(orders)          orders[i] 为第 i 方的命令（None 表示这一回合不动），返回 {"turn", "orders"}
    game.forfeit(seats)        这些座位因故障判负，对局就此结束
    game.header()              回放头部（引擎版本、种子、起始局面、预算）
    game.result()              终局：score（每方得分）、winner、reason、turns 等
    game.signature()           对局签名：两局的签名相同，就是逐回合完全相同的两局
    game.close()               释放资源

bot 可以是 .cpp 源码（自动编译，按内容哈希缓存）或已经编译好的可执行文件。
"""
from __future__ import annotations

import gzip
import hashlib
import json
import os
import queue
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

MAX_STRIKES = 3          # 连续失败（超时、输出不合法）达到这个次数判负。与 judge/fast.cpp 里的同名常量一致
MAX_LINE = 4096          # bot 一行回复的长度上限
STDERR_MAX_LINES = 2000  # 每局每个 bot 的调试输出最多保留多少行
STDERR_MAX_CHARS = 200_000
STDERR_LINE_CHARS = 500
CXX_FLAGS = ["-std=c++20", "-O2"] + (["-static"] if os.name == "nt" else [])
EXE = ".exe" if os.name == "nt" else ""
DLL = ".dll" if os.name == "nt" else ".so"

# 选手包所在目录：tank.hpp、tank_main.cpp、judge/ 都在这里。默认是本文件所在目录。
KIT = Path(__file__).resolve().parent


def set_kit(path):
    """本文件不在选手包目录里时（比如主办方从 平台/ 目录使用），先指明选手包目录。"""
    global KIT
    KIT = Path(path).resolve()


def default_build_dir():
    return KIT / "build"


def setup_console():
    """让标准输出、标准错误按 utf-8 写，中文在 Windows 控制台和管道里都不乱码。"""
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass


# ====================================================================
# 思考预算：裁判侧的"缝"
# ====================================================================
# 预算的单位是"CPU 毫秒"：bot 进程自己消耗的 CPU 量（所有线程合计），被挂起、等输入的时间不算。
#   Windows   QueryProcessCycleTime(进程句柄)，周期数除以"每 CPU 毫秒多少周期"。这个系数由裁判核心测定
#             （judge/cycles.hpp），engine.py 在开局时原样告诉 bot，两边用同一个数。
#   Linux     进程的 CPU 时间时钟（按分析实现，未实测）。
#   其他系统，或环境变量 TANK_BUDGET=wall（调试用）：退回墙上时钟，从发出局面到收到回复。
# 计费区间：上一回合收到回复 → 这一回合收到回复（首回合从进程启动算起），所以两回合之间偷着算的也计入。
# 等回复时每 POLL_SECONDS 读一次用量，超了当场判；另有墙上保险上限，只用来兜住睡眠、阻塞这类不耗 CPU 的情况。
# 预算和保险上限的数值由 tank.hpp 给出，engine.py 从裁判核心读来，这里不重复写。
#
# 换计时方式只改这里的 meter_read（裁判侧）和 tank.hpp 第 9 节的 detail::budget_reading（bot 侧）。

POLL_SECONDS = 0.005
UNITS_PER_MS = 1.0          # Windows：每 CPU 毫秒多少周期；由 configure_budget 设定
_k32 = None


def _pick_budget_kind():
    if os.environ.get("TANK_BUDGET", "").lower() == "wall":
        return "wall"
    if os.name == "nt":
        return "cpu"
    if sys.platform.startswith("linux") and hasattr(time, "clock_gettime"):
        try:
            time.clock_gettime(((~os.getpid()) << 3) | 2)
            return "cpu"
        except OSError:
            pass
    print("（这个系统上没有可用的 CPU 计时，思考预算按墙上时钟计。）", file=sys.stderr)
    return "wall"


BUDGET_KIND = _pick_budget_kind()      # "cpu" 或 "wall"


def configure_budget(units_per_ms):
    """设定"每 CPU 毫秒多少个读数单位"（Windows 上是周期数）。engine.py 从裁判核心读到后调用。"""
    global UNITS_PER_MS
    UNITS_PER_MS = float(units_per_ms)


def _kernel32():
    global _k32
    if _k32 is None:
        import ctypes
        from ctypes import wintypes
        k = ctypes.WinDLL("kernel32", use_last_error=True)
        k.QueryProcessCycleTime.argtypes = [wintypes.HANDLE, ctypes.POINTER(ctypes.c_uint64)]
        k.QueryProcessCycleTime.restype = wintypes.BOOL
        k.SetProcessAffinityMask.argtypes = [wintypes.HANDLE, ctypes.c_size_t]
        k.SetProcessAffinityMask.restype = wintypes.BOOL
        _k32 = k
    return _k32


def meter_read(proc):
    """【缝 · 裁判侧】某个 bot 进程此刻的读数（毫秒，只增不减）。一回合的用量 = 两次读数之差。

    按 CPU 计时：进程从启动起消耗的 CPU 毫秒，所有线程合计；进程正在运行时也是实时的。
    按墙上时钟：当前时刻。"""
    if BUDGET_KIND == "cpu":
        if os.name == "nt":
            import ctypes
            c = ctypes.c_uint64()
            if not _kernel32().QueryProcessCycleTime(int(proc._handle), ctypes.byref(c)):
                raise MatchError("读不到 bot 进程的 CPU 用量")
            return c.value / UNITS_PER_MS
        return time.clock_gettime(((~proc.pid) << 3) | 2) * 1000.0     # Linux：进程的 CPU 时间时钟（未实测）
    return time.perf_counter() * 1000.0


def set_affinity(proc, cpus):
    """把一个自己起的进程绑到给定的逻辑处理器上（编号列表）。空列表或 None 表示不绑。"""
    if not cpus:
        return
    if os.name == "nt":
        mask = 0
        for c in cpus:
            mask |= 1 << int(c)
        if not _kernel32().SetProcessAffinityMask(int(proc._handle), mask):
            raise MatchError(f"绑不到逻辑处理器 {list(cpus)}（只支持编号 0–63）")
    elif hasattr(os, "sched_setaffinity"):
        os.sched_setaffinity(proc.pid, set(int(c) for c in cpus))       # 未实测
    else:
        raise MatchError("这个系统上不支持绑定处理器")


def cpu_topology():
    """本机的处理器拓扑（只读）。返回 [{"core": 物理核序号, "class": 效率等级, "cpus": [逻辑处理器编号]}]。

    效率等级越大性能越高：大小核混合的处理器上，大核的等级比小核高；同构处理器上全是 0。
    一个物理核有两个逻辑处理器就是开了超线程。只在 Windows 上实现，其他系统返回空列表。"""
    if os.name != "nt":
        return []
    import ctypes
    from ctypes import wintypes
    k = ctypes.WinDLL("kernel32", use_last_error=True)
    k.GetLogicalProcessorInformationEx.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.POINTER(wintypes.DWORD)]
    size = wintypes.DWORD(0)
    k.GetLogicalProcessorInformationEx(0, None, ctypes.byref(size))        # 0 = RelationProcessorCore
    buf = (ctypes.c_ubyte * size.value)()
    if not k.GetLogicalProcessorInformationEx(0, buf, ctypes.byref(size)):
        return []
    raw, out, at = bytes(buf), [], 0
    while at < size.value:
        length = int.from_bytes(raw[at + 4:at + 8], "little")
        # 记录头 8 字节；PROCESSOR_RELATIONSHIP：Flags(1) EfficiencyClass(1) Reserved(20) GroupCount(2)，
        # 接着每组 16 字节：Mask(8) Group(2) Reserved(6)。
        eff = raw[at + 9]
        groups = int.from_bytes(raw[at + 30:at + 32], "little")
        cpus = []
        for g in range(groups):
            base = at + 32 + 16 * g
            mask = int.from_bytes(raw[base:base + 8], "little")
            group = int.from_bytes(raw[base + 8:base + 10], "little")
            cpus += [64 * group + b for b in range(64) if mask >> b & 1]
        out.append({"core": len(out), "class": eff, "cpus": cpus})
        at += length
    return out


# ====================================================================
# 子进程管理：只管自己起的进程
# ====================================================================
# Windows 上把自己起的每个子进程放进一个"作业对象"，本进程不论怎么结束（包括被强行结束），
# 系统都会把这些子进程一并结束。不按名字找进程，不碰别人的进程。

_job = None
_job_lock = threading.Lock()


def _make_job():
    import ctypes
    from ctypes import wintypes

    class BASIC(ctypes.Structure):
        _fields_ = [("PerProcessUserTimeLimit", ctypes.c_int64), ("PerJobUserTimeLimit", ctypes.c_int64),
                    ("LimitFlags", wintypes.DWORD), ("MinimumWorkingSetSize", ctypes.c_size_t),
                    ("MaximumWorkingSetSize", ctypes.c_size_t), ("ActiveProcessLimit", wintypes.DWORD),
                    ("Affinity", ctypes.c_size_t), ("PriorityClass", wintypes.DWORD),
                    ("SchedulingClass", wintypes.DWORD)]

    class IO(ctypes.Structure):
        _fields_ = [(name, ctypes.c_uint64) for name in
                    ("ReadOperationCount", "WriteOperationCount", "OtherOperationCount",
                     "ReadTransferCount", "WriteTransferCount", "OtherTransferCount")]

    class EXTENDED(ctypes.Structure):
        _fields_ = [("BasicLimitInformation", BASIC), ("IoInfo", IO), ("ProcessMemoryLimit", ctypes.c_size_t),
                    ("JobMemoryLimit", ctypes.c_size_t), ("PeakProcessMemoryUsed", ctypes.c_size_t),
                    ("PeakJobMemoryUsed", ctypes.c_size_t)]

    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.CreateJobObjectW.restype = wintypes.HANDLE
    k32.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
    k32.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
    k32.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
    job = k32.CreateJobObjectW(None, None)
    if not job:
        return None
    info = EXTENDED()
    info.BasicLimitInformation.LimitFlags = 0x2000      # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    if not k32.SetInformationJobObject(job, 9, ctypes.byref(info), ctypes.sizeof(info)):
        return None
    # 子进程崩溃时不弹系统对话框（错误模式会被子进程继承）。
    k32.SetErrorMode(0x0001 | 0x0002 | 0x8000)
    return k32, job


def adopt(proc):
    """登记一个自己起的子进程：本进程结束时它会被一并结束。失败不影响运行。"""
    global _job
    if os.name != "nt":
        return
    try:
        with _job_lock:
            if _job is None:
                _job = _make_job() or False
        if _job:
            k32, job = _job
            k32.AssignProcessToJobObject(job, int(proc._handle))
    except Exception:
        pass


def stop_process(proc):
    """结束一个自己起的子进程并回收。"""
    if proc is None:
        return
    try:
        if proc.poll() is None:
            proc.kill()
        proc.wait(timeout=3)
    except Exception:
        pass
    for pipe in (proc.stdin, proc.stdout, proc.stderr):
        try:
            if pipe:
                pipe.close()
        except Exception:
            pass


# ====================================================================
# 编译
# ====================================================================

def find_compiler():
    cxx = os.environ.get("CXX") or shutil.which("g++") or shutil.which("clang++")
    if cxx:
        return cxx
    if os.name == "nt":
        here = Path(__file__).resolve()
        for base in [*here.parents, Path.home() / "Desktop" / "测试"]:
            guess = base / "00_项目管理" / "联调" / "toolchain" / "w64devkit" / "bin" / "g++.exe"
            if guess.is_file():
                return str(guess)
    return None


def sha_of(data):
    return hashlib.sha256(data).hexdigest()


def _key(*parts):
    h = hashlib.sha256()
    for p in parts:
        b = p if isinstance(p, bytes) else str(p).encode("utf-8")
        h.update(len(b).to_bytes(8, "little"))
        h.update(b)
    return h.hexdigest()[:16]


def _compiler_id(cxx):
    try:
        st = os.stat(cxx)
        return f"{cxx}|{st.st_size}|{int(st.st_mtime)}"
    except OSError:
        return str(cxx)


def _ascii(path):
    return all(ord(ch) < 128 for ch in str(path))


def _run_cxx(args, out, shown=()):
    """运行一次编译器，产物先写临时文件再改名，所以并行编译同一个目标也不会互相踩坏。"""
    cxx = find_compiler()
    if not cxx:
        raise RuntimeError("没有找到 C++ 编译器（g++ 或 clang++）")
    out = Path(out)
    if out.is_file():
        return out
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_name(f"tmp-{os.getpid()}-{threading.get_ident()}-{out.name}")
    env = dict(os.environ)
    env["PATH"] = str(Path(cxx).resolve().parent) + os.pathsep + env.get("PATH", "")
    try:
        r = subprocess.run([cxx, *CXX_FLAGS, *[str(a) for a in args], "-o", str(tmp)], capture_output=True,
                           encoding="utf-8", errors="replace", timeout=300, env=env, stdin=subprocess.DEVNULL)
    except subprocess.TimeoutExpired:
        raise RuntimeError("编译超过 300 秒")
    except OSError as exc:
        raise RuntimeError(f"编译器无法运行：{exc}")
    if r.returncode != 0:
        try:
            tmp.unlink()
        except OSError:
            pass
        msg = (r.stderr or r.stdout or "")[-4000:]
        for real, name in shown:
            msg = msg.replace(str(real), str(name)).replace(str(real).replace("\\", "/"), str(name))
        hint = ""
        if not _ascii(out) or not all(_ascii(a) for a in args):
            hint = "\n（路径里有非英文字符，这个编译器可能打不开。请把整个目录放到纯英文路径下再试。）"
        raise RuntimeError("编译失败：\n" + msg.rstrip() + hint)
    try:
        os.replace(tmp, out)
    except OSError:
        if not out.is_file():       # 别的进程已经放好了同名文件（内容相同）就算成功
            raise
        try:
            tmp.unlink()
        except OSError:
            pass
    return out


def _replace(tmp, path, same_content=False):
    """改名覆盖。Windows 上目标文件正被别的进程开着、或者另一个同时运行的命令正往同一个名字上改名时会被拒绝
    （WinError 5）：等一下再试，最多约 3 秒。same_content=True 表示同名的目标内容必然相同（文件名里带着内容的哈希），
    别的进程已经放好了就算成功。"""
    for attempt in range(60):
        try:
            os.replace(tmp, path)
            return
        except OSError:
            if same_content and Path(path).is_file():
                try:
                    Path(tmp).unlink()
                except OSError:
                    pass
                return
            if attempt == 59:
                raise
            time.sleep(0.05)


def _glue_object(kind, build):
    """主循环 tank_main.cpp 编成目标文件：kind 为 "exe"（正式模式）或 "dll"（快速模式）。"""
    main, hpp = KIT / "tank_main.cpp", KIT / "tank.hpp"
    key = _key(main.read_bytes(), hpp.read_bytes(), " ".join(CXX_FLAGS), kind, _compiler_id(find_compiler()), os.name)
    flags = ["-c", "-I", KIT]
    if kind == "dll":
        flags += ["-DTANK_FAST_DLL"] + ([] if os.name == "nt" else ["-fPIC"])
    return _run_cxx([*flags, main], build / f"tank_main-{kind}-{key}.o")


def _bot_object(source, kind, build):
    """bot 源码编成目标文件。正式模式和快速模式用同一个目标文件（Windows 上），所以两边是同一份机器码。"""
    source = Path(source)
    data = source.read_bytes()
    pic = kind == "dll" and os.name != "nt"
    key = _key(data, (KIT / "tank.hpp").read_bytes(), " ".join(CXX_FLAGS), _compiler_id(find_compiler()), os.name, pic)
    obj = build / f"bot-{key}.o"
    if obj.is_file():
        return obj, key
    # 把源码抄一份到编译目录再编：文件名固定是英文的，编译途中源文件被改也不影响。
    build.mkdir(parents=True, exist_ok=True)
    copy = build / f"bot-{key}.cpp"
    if not copy.is_file():
        tmp = copy.with_name(f"tmp-{os.getpid()}-{threading.get_ident()}-{copy.name}")
        tmp.write_bytes(data)
        _replace(tmp, copy, same_content=True)
    flags = ["-c", "-I", KIT, "-I", source.resolve().parent] + (["-fPIC"] if pic else [])
    _run_cxx([*flags, copy], obj, shown=[(copy, source), (copy.name, source.name)])
    return obj, key


def build_bot(source, kind="exe", build_dir=None):
    """把一份 bot 源码编成可执行文件（kind="exe"，正式模式）或动态库（kind="dll"，快速模式）。

    按源码内容、tank.hpp、主循环、编译参数的哈希缓存，都没变就不重编。
    产物的文件名不含源文件名（bot 能从命令行看到自己的文件名）。失败时抛出 RuntimeError，附编译器的输出。"""
    build = Path(build_dir) if build_dir else default_build_dir()
    obj, key = _bot_object(source, kind, build)
    glue = _glue_object(kind, build)
    out = build / f"bot-{_key(key, glue.name)}{EXE if kind == 'exe' else DLL}"
    if out.is_file():
        return out
    return _run_cxx(([] if kind == "exe" else ["-shared"]) + [obj, glue], out)


def build_tool(name, build_dir=None):
    """编译 judge/ 下的工具（referee、fast），按内容哈希缓存。"""
    build = Path(build_dir) if build_dir else default_build_dir()
    source = KIT / "judge" / f"{name}.cpp"
    key = _key(source.read_bytes(), (KIT / "tank.hpp").read_bytes(), " ".join(CXX_FLAGS),
               _compiler_id(find_compiler()), os.name)
    libs = [] if os.name == "nt" else ["-ldl", "-pthread"]
    return _run_cxx([source, *libs], build / f"{name}-{key}{EXE}")


def is_source(path):
    return Path(path).suffix.lower() in (".cpp", ".cc", ".cxx")


def command_for(program, build_dir=None):
    """返回正式模式下启动这个 bot 的命令。源码先编译；其余当作可执行文件。"""
    p = Path(program)
    if is_source(p):
        return [str(build_bot(p, "exe", build_dir))]
    return [str(p.resolve())]


# ====================================================================
# bot 进程
# ====================================================================

EXIT_CODES = {0xC0000005: "非法内存访问", 0xC00000FD: "栈溢出", 0xC0000094: "除以零", 0xC000001D: "非法指令",
              0xC0000409: "栈缓冲区溢出或 abort", 3: "abort 或未接住的异常"}


def describe_exit(code):
    if code is None:
        return "进程已退出"
    code &= 0xFFFFFFFF
    note = EXIT_CODES.get(code)
    text = f"0x{code:08X}" if code > 255 else str(code)
    return f"进程已退出（代码 {text}{'，' + note if note else ''}）"


class MatchError(RuntimeError):
    """裁判这一侧的问题（程序起不来、裁判核心出错等），不算在任何选手头上。"""


class Bot:
    """一个 bot 进程。每局起一个新的，工作目录是一个全新的临时目录。"""

    def __init__(self, command, limits_ms, caps_ms=None, affinity=None):
        self.first_limit, self.turn_limit = limits_ms              # 预算（毫秒）：首回合、之后每回合
        self.first_cap, self.turn_cap = caps_ms or limits_ms       # 墙上保险上限（只在按 CPU 计时时起作用）
        self.charged = 0.0        # 已经计过费的读数：上一回合收到回复（或判超时）那一刻的
        self.strikes = 0
        self.dead = None          # 判负原因
        self.faults = []          # [{"turn", "kind", "detail"}]
        self.calls = 0
        self.turn = 0             # 正在等哪一回合的回复（给调试输出打回合标记用）
        self.used = []            # 每回合的预算用量（毫秒）
        self.stderr = []          # [[回合, 一行]]
        self.stderr_dropped = 0
        self._stderr_chars = 0
        self.lines = queue.Queue()
        self.proc = None
        self.dir = Path(tempfile.mkdtemp(prefix="tank_"))
        try:
            self.proc = subprocess.Popen(command, cwd=self.dir, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                         stderr=subprocess.PIPE)
        except OSError as exc:
            shutil.rmtree(self.dir, ignore_errors=True)
            raise MatchError(f"程序无法启动：{command[0]}（{exc}）")
        adopt(self.proc)
        try:
            set_affinity(self.proc, affinity)
        except Exception:
            stop_process(self.proc)
            shutil.rmtree(self.dir, ignore_errors=True)
            raise
        self._threads = [threading.Thread(target=self._read_stdout, daemon=True),
                         threading.Thread(target=self._read_stderr, daemon=True)]
        for t in self._threads:
            t.start()

    def _read_stdout(self):
        try:
            while True:
                raw = self.proc.stdout.readline(MAX_LINE + 1)
                if not raw:
                    break
                self.lines.put((meter_read(self.proc), raw))
        except (OSError, ValueError):
            pass
        self.lines.put((meter_read(self.proc), None))

    def _read_stderr(self):
        try:
            while True:
                raw = self.proc.stderr.readline(65536)
                if not raw:
                    break
                if len(self.stderr) >= STDERR_MAX_LINES or self._stderr_chars >= STDERR_MAX_CHARS:
                    self.stderr_dropped += 1
                    continue
                text = raw.decode("utf-8", errors="replace").rstrip("\r\n")[:STDERR_LINE_CHARS]
                self._stderr_chars += len(text)
                self.stderr.append([self.turn, text])
        except (OSError, ValueError):
            pass

    def send(self, text, turn):
        """写入本回合的输入。返回 (计费起点的读数, 发出时的墙上时刻)；bot 已判负时返回 None。"""
        if self.dead:
            return None
        self.turn = turn
        try:
            self.proc.stdin.write(text.encode("utf-8"))
            self.proc.stdin.flush()
        except (OSError, ValueError):
            pass        # 进程多半已经退出；交给 receive 从输出那一头确认，原因更准确
        # 按 CPU 计时：从上一回合收到回复时的读数算起（首回合从 0，即进程启动算起）。
        # 按墙上时钟：从此刻算起。
        start = self.charged if BUDGET_KIND == "cpu" else meter_read(self.proc)
        return start, time.perf_counter()

    def receive(self, started, turn):
        """在预算内读到本回合的一行回复就返回它；否则记一次失败并返回 None。"""
        if self.dead or started is None:
            self.used.append(None)
            return None
        reading0, wall0 = started
        first = self.calls == 0
        self.calls += 1
        limit = self.first_limit if first else self.turn_limit
        cpu = BUDGET_KIND == "cpu"
        cap = (self.first_cap if first else self.turn_cap) if cpu else limit
        deadline = wall0 + cap / 1000.0

        def over(reading, detail):
            self.charged = reading
            self.used.append(round(reading - reading0, 3))
            return self.strike(turn, "timeout", detail)

        while True:
            wait = max(0.0, deadline - time.perf_counter())
            try:
                reading, raw = self.lines.get(timeout=min(wait, POLL_SECONDS) if cpu else wait)
            except queue.Empty:
                now = meter_read(self.proc)
                if cpu and now - reading0 > limit:
                    return over(now, f"超出预算（{limit:g} CPU 毫秒）")       # 死循环、算得太久在这里被发现
                if time.perf_counter() >= deadline:
                    return over(now, f"超过墙上保险上限（{cap:g} 毫秒没有回复）" if cpu else f"超出预算（{limit:g} 毫秒）")
                continue
            if raw is None:
                self.used.append(round(reading - reading0, 3))
                code = None
                try:
                    code = self.proc.wait(timeout=2)
                except Exception:
                    pass
                self.kill(turn, "exit", describe_exit(code))
                return None
            line = raw.decode("utf-8", errors="replace")
            head = line.split(None, 1)[0] if line.strip() else ""
            if head.isdigit() and int(head) < turn:
                continue                    # 以前某个超时回合迟到的回复，丢掉
            if reading - reading0 > limit:
                return over(reading, f"超出预算（{limit:g} {'CPU ' if cpu else ''}毫秒）")
            self.charged = reading
            self.used.append(round(reading - reading0, 3))
            if len(raw) > MAX_LINE:
                return self.strike(turn, "illegal", f"一行输出超过 {MAX_LINE} 字节")
            return line

    def ok(self):
        self.strikes = 0

    def strike(self, turn, kind, detail):
        self.strikes += 1
        self.faults.append({"turn": turn, "kind": kind, "detail": detail})
        if self.strikes >= MAX_STRIKES and not self.dead:
            self.dead = f"连续 {MAX_STRIKES} 次失败，最后一次：{detail}"
        return None

    def kill(self, turn, kind, detail):
        if not self.dead:
            self.dead = detail
            self.faults.append({"turn": turn, "kind": kind, "detail": detail})

    def close(self):
        stop_process(self.proc)
        for t in getattr(self, "_threads", []):
            t.join(timeout=1)
        for _ in range(5):
            shutil.rmtree(self.dir, ignore_errors=True)
            if not self.dir.exists():
                break
            time.sleep(0.05)

    def report(self):
        later = [u for u in self.used[1:] if u is not None]
        return {"dead": self.dead, "faults": len(self.faults),
                "timeouts": sum(f["kind"] == "timeout" for f in self.faults),
                "illegal": sum(f["kind"] == "illegal" for f in self.faults),
                "first_ms": self.used[0] if self.used and self.used[0] is not None else 0.0,
                "avg_ms": round(sum(later) / len(later), 3) if later else 0.0,
                "max_ms": max(later) if later else 0.0,
                "stderr_tail": [text for _, text in self.stderr[-10:]]}


def run_match(game_cls, commands, names, seed, record=True, affinity=None, **options):
    """正式模式打一局。commands[i]（由 command_for 得到）坐第 i 号位。返回 (结果, 回放或 None)。

    affinity：可选，[0 号位的逻辑处理器编号列表, 1 号位的]，把两个 bot 进程分别绑在这些处理器上；默认不绑。

    bot 的超时、非法输出、崩溃记在它自己头上；裁判这一侧出的错抛出异常（MatchError 等），不产生结果。"""
    game = game_cls(len(commands), seed, **options)
    bots = []
    orders_log, faults = [], []
    try:
        limits = tuple(1000.0 * x for x in game.TIME_LIMITS)
        caps = tuple(1000.0 * x for x in getattr(game, "WALL_CAPS", None) or game.TIME_LIMITS)
        for i, c in enumerate(commands):
            bots.append(Bot(c, limits, caps, affinity[i] if affinity else None))
        while not game.done:
            turn = game.turn + 1
            texts = [game.encode(i) for i in range(len(bots))]
            starts = [b.send(texts[i], turn) for i, b in enumerate(bots)]
            orders = [None] * len(bots)
            for i, b in enumerate(bots):
                line = b.receive(starts[i], turn)
                if line is None:
                    continue
                try:
                    orders[i] = game.decode(i, line)
                    b.ok()
                except ValueError as exc:
                    b.strike(turn, "illegal", f"输出不合法：{exc}（收到：{line.strip()[:80]}）")
            out = [i for i, b in enumerate(bots) if b.dead]
            if out:
                game.forfeit(out)       # 判负的那一回合不结算
                break
            orders_log.append(game.step(orders)["orders"])
        result = game.result()
        header = game.header()
        signature = game.signature()
    finally:
        for b in bots:
            b.close()
        game.close()
    for i, b in enumerate(bots):
        faults += [{"seat": i, **f} for f in b.faults]
    faults.sort(key=lambda f: (f["turn"], f["seat"]))
    result.update(seed=seed, names=list(names), bots=[b.report() for b in bots], sig=signature,
                  n_faults=len(faults))
    if not record:
        return result, None
    n = max(len(b.used) for b in bots)
    used = [[(b.used[t] if t < len(b.used) else None) for b in bots] for t in range(n)]
    replay = {**header, "mode": "formal", "names": list(names), "orders": orders_log, "used_ms": used,
              "faults": faults, "affinity": affinity, "stderr": [b.stderr for b in bots],
              "stderr_dropped": [b.stderr_dropped for b in bots], "result": result}
    return result, replay


# ====================================================================
# 回放文件
# ====================================================================

def save_replay(replay, path):
    """保存回放（JSON，utf-8）。文件名以 .gz 结尾时用 gzip 压缩。返回实际路径。"""
    path = Path(path)
    if path.suffix not in (".json", ".gz"):
        path = path.with_name(path.name + ".json")
    path.parent.mkdir(parents=True, exist_ok=True)
    raw = json.dumps(replay, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    if path.suffix == ".gz":
        raw = gzip.compress(raw)
    tmp = path.with_name(f"tmp-{os.getpid()}-{path.name}")
    tmp.write_bytes(raw)
    _replace(tmp, path)
    return path


def load_replay(path):
    raw = Path(path).read_bytes()
    if raw[:2] == b"\x1f\x8b":
        raw = gzip.decompress(raw)
    return json.loads(raw.decode("utf-8"))


def safe_name(text, fallback="x"):
    """把任意文字变成只含英文字母、数字、下划线、短横线的文件名片段。"""
    return re.sub(r"[^A-Za-z0-9_-]+", "_", str(text)).strip("_")[:40] or fallback
