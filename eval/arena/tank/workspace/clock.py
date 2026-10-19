"""查看比赛用时。只读，不写任何文件。

上半场从解压出本文件的那一刻算起；中场包解压后（出现 halftime/START）显示下半场，
从解压出那个文件的那一刻算起。两个半场都是 30 分钟，最早交卷时间是第 20 分钟，45 分钟强制收卷。
"""
import sys
import time
from pathlib import Path

EARLIEST, TARGET, LIMIT = 20, 30, 45  # 分钟：最早交卷时间、本半场的时间、强制收卷
HERE = Path(__file__).resolve().parent


def born(path):
    """文件在本机被创建（也就是解压出来）的时刻。不能用修改时间：解压会把它恢复成打包时的值。"""
    st = Path(path).stat()
    return getattr(st, "st_birthtime", None) or st.st_ctime  # Windows 为创建时间；Linux 上新建文件的 ctime 即解压时刻


SKIP_DIRS = {"build", "history", "replays", "submission", "judge", "opponents", "__pycache__", ".git", "node_modules"}


def _someone_elses(d, here):
    """这个目录是不是另一份比赛文件（里面有它自己的 clock.py 或 workspace.zip）：是的话不进去找。"""
    try:
        if (d / "workspace.zip").is_file():
            return True
        return (d / "clock.py").is_file() and d.resolve() != here.resolve()
    except OSError:
        return True


def _search(root, here, depth):
    """在 root 往下最多 depth 层目录里找 halftime/START。"""
    found, level = [], [root]
    for _ in range(depth + 1):
        deeper = []
        for d in level:
            marker = d / "halftime" / "START"
            if marker.is_file():
                found.append(marker)
            try:
                subs = [x for x in d.iterdir() if x.is_dir() and x.name not in SKIP_DIRS]
            except OSError:
                continue
            deeper += [x for x in subs if not _someone_elses(x, here)]
        level = deeper
    return found


def find_marker(here=HERE):
    """找中场包的开赛标记。标准位置是 halftime/START。

    解压时多套或少套了一层目录，或者解压到了旁边一个以压缩包命名的文件夹里（有的解压命令默认这样做），也能找到：
    在本目录和上一层目录里往下找几层（本目录自己被多套了一层时，再往上一层）。找到几个时取最早解压出来的那个。"""
    here = Path(here)
    for rel in ("halftime/START", "halftime/halftime/START", "workspace/halftime/START",
                "halftime/workspace/halftime/START", "../halftime/START"):
        p = here / rel
        if p.is_file():
            return p
    roots = [here, here.parent]
    if here.parent.name.lower() == here.name.lower():
        roots.append(here.parent.parent)
    found = []
    for root in roots:
        try:
            found += _search(root, here, 3)
        except OSError:
            pass
    return min(found, key=born) if found else None


def message(half, used):
    if used < EARLIEST:
        return (f"{half} 已用 {used:.1f} 分钟。最早交卷时间是第 {EARLIEST} 分钟（还有 {EARLIEST - used:.1f} 分钟），"
                f"{TARGET} 分钟是本半场的时间，{LIMIT} 分钟强制收卷。")
    if used < TARGET:
        return (f"{half} 已用 {used:.1f} 分钟。已过最早交卷时间（第 {EARLIEST} 分钟），"
                f"距 {TARGET} 分钟还有 {TARGET - used:.1f} 分钟，{LIMIT} 分钟强制收卷。")
    if used < LIMIT:
        return (f"{half} 已用 {used:.1f} 分钟，已过 {TARGET} 分钟，请尽快收尾。"
                f"{LIMIT} 分钟强制收卷，还剩 {LIMIT - used:.1f} 分钟。")
    return f"{half} 已用 {used:.1f} 分钟，已到 {LIMIT} 分钟，强制收卷：请立即停止修改并提交。"


def status(here=HERE, clock_file=None, now=None):
    now = time.time() if now is None else now
    marker = find_marker(here)
    half, start = ("下半场", born(marker)) if marker else ("上半场", born(clock_file or Path(__file__).resolve()))
    text = message(half, max(0.0, now - start) / 60)
    if marker and marker.resolve() != (here / "halftime" / "START").resolve():
        text += (f"\n提示：中场包解压到了 {marker.parent}，标准位置是 {here / 'halftime'}。计时不受影响；"
                 "可以把整个 halftime 文件夹移到标准位置（移动不会改变计时）。")
    return text


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass
    print(status())
