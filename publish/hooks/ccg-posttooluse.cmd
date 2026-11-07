@echo off
rem ccg PostToolUse hook trampoline (Windows variant).
rem Forwards the hook event on stdin to the ccg handler subcommand. ccg is
rem located by: CCG_BIN env override, then the release layout beside this
rem script (ccg.exe sits two levels up from .ccg\hooks\), then PATH. The
rem trampoline always exits 0 - a hook must never fail the tool call it
rem observes (the handler itself is silent-and-0 on every skip path too).
setlocal
set "CCG_HOOK_HOME=%~dp0"
if defined CCG_BIN (
    "%CCG_BIN%" hook posttooluse
    exit /b 0
)
if exist "%CCG_HOOK_HOME%..\..\ccg.exe" (
    call "%CCG_HOOK_HOME%..\..\ccg.exe" hook posttooluse
    exit /b 0
)
where ccg >nul 2>nul || exit /b 0
call ccg hook posttooluse
exit /b 0
