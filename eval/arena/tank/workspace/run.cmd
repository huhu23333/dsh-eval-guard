@echo off
setlocal
chcp 65001 >nul
rem Find a Python 3.10+ and run the given script in this folder. No arguments: show the contest clock.
rem Usage: .\run.cmd arena.py check
rem (Keep this file ASCII-only except the last line: cmd.exe loses its place in a batch file
rem  that has multi-byte characters after the code page is switched.)
rem "call" is needed: python3 may be a .cmd shim, and without call control never comes back.
set "CHECK=import sys; v = sys.version_info; sys.exit(v[0] != 3 or v[1] not in range(10, 100))"
set "PY="
for %%P in (python python3) do if not defined PY (
  call %%P -c "%CHECK%" >nul 2>nul && set "PY=%%P"
)
if not defined PY (
  call py -3 -c "%CHECK%" >nul 2>nul && set "PY=py -3"
)
rem Not on PATH: look in the usual install locations.
if not defined PY for /d %%D in ("%LOCALAPPDATA%\Programs\Python\Python3*" "%ProgramFiles%\Python3*") do if not defined PY (
  if exist "%%~D\python.exe" call "%%~D\python.exe" -c "%CHECK%" >nul 2>nul && set "PY="%%~D\python.exe""
)
if not defined PY if exist "%USERPROFILE%\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe" (
  set "PY="%USERPROFILE%\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe""
)
if not defined PY goto nopython
pushd "%~dp0"
if "%~1"=="" (call %PY% -X utf8 clock.py) else (call %PY% -X utf8 %*)
set "CODE=%errorlevel%"
popd
exit /b %CODE%
:nopython
echo 没有找到 Python 3.10 以上的版本（试过 python、python3、py -3 和常见安装位置）。请把这个情况报告出来。 1>&2 & exit /b 1
