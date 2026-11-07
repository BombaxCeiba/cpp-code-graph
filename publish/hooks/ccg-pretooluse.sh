#!/bin/sh
# ccg PreToolUse hook trampoline (POSIX variant).
# Forwards the hook event on stdin to the ccg handler subcommand. ccg is
# located by: CCG_BIN env override, then the release layout beside this
# script (the ccg binary sits two levels up from .ccg/hooks/), then PATH. The
# trampoline always exits 0 - a hook must never fail the tool call it
# observes (the handler itself is silent-and-0 on every skip path too).
set -u
self_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ccg_bin=${CCG_BIN:-}
if [ -z "$ccg_bin" ]; then
    for cand in "$self_dir/../../ccg" "$self_dir/../../ccg.exe"; do
        if [ -x "$cand" ]; then
            ccg_bin=$cand
            break
        fi
    done
fi
if [ -n "$ccg_bin" ]; then
    exec "$ccg_bin" hook pretooluse
fi
command -v ccg >/dev/null 2>&1 && exec ccg hook pretooluse
exit 0
