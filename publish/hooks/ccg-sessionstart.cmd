@echo off
rem ccg SessionStart hook trampoline (Windows variant).
rem Forwards the hook event on stdin to the ccg handler subcommand, which
rem prints one bounded line of code-database status (silent when no db
rem exists for the working directory). ccg is located by: CCG_BIN env
rem override, then the release layout beside this script, then PATH. Always
rem exits 0.
setlocal
set "CCG_HOOK_HOME=%~dp0"
if defined CCG_BIN (
    "%CCG_BIN%" hook sessionstart
    exit /b 0
)
if exist "%CCG_HOOK_HOME%..\..\ccg.exe" (
    call "%CCG_HOOK_HOME%..\..\ccg.exe" hook sessionstart
    exit /b 0
)
where ccg >nul 2>nul || exit /b 0
call ccg hook sessionstart
exit /b 0
