@echo off
rem ccg UserPromptSubmit hook trampoline (Windows variant).
rem Forwards the hook event on stdin to the ccg handler subcommand, which
rem reports the accumulated batch conclusions (one merged, bounded
rem additionalContext) or stays silent while a batch still runs. ccg is
rem located by: CCG_BIN env override, then the release layout beside this
rem script, then PATH. The trampoline always exits 0 - a hook must never
rem fail the prompt it observes (the handler itself is silent-and-0 on
rem every skip path too).
setlocal
set "CCG_HOOK_HOME=%~dp0"
if defined CCG_BIN (
    "%CCG_BIN%" hook userpromptsubmit
    exit /b 0
)
if exist "%CCG_HOOK_HOME%..\..\ccg.exe" (
    call "%CCG_HOOK_HOME%..\..\ccg.exe" hook userpromptsubmit
    exit /b 0
)
where ccg >nul 2>nul || exit /b 0
call ccg hook userpromptsubmit
exit /b 0
