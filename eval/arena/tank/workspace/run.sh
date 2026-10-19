#!/bin/sh
# 找一个 Python 3.10 以上，在本目录运行给定的脚本。不带参数时显示比赛用时。
# 用法：sh run.sh arena.py check
cd "$(dirname -- "$0")" || exit 1
[ "$#" -eq 0 ] && set -- clock.py
ok='import sys; sys.exit(sys.version_info < (3, 10))'
for py in python3 python; do
    if command -v "$py" >/dev/null 2>&1 && "$py" -c "$ok" >/dev/null 2>&1; then
        exec "$py" -X utf8 "$@"
    fi
done
# Windows 上只装了 py 启动器的情况。
if command -v py >/dev/null 2>&1 && py -3 -c "$ok" >/dev/null 2>&1; then
    exec py -3 -X utf8 "$@"
fi
# PATH 里没有：再看几个常见的安装位置。
for py in "$LOCALAPPDATA"/Programs/Python/Python3*/python.exe "$HOME"/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe /usr/local/bin/python3 /usr/bin/python3; do
    if [ -x "$py" ] && "$py" -c "$ok" >/dev/null 2>&1; then
        exec "$py" -X utf8 "$@"
    fi
done
echo '没有找到 Python 3.10 以上的版本（试过 python3、python、py -3 和常见安装位置）。请把这个情况报告出来。' >&2
exit 1
