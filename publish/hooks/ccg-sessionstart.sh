#!/bin/sh
# ccg SessionStart hook trampoline (POSIX variant).
# Forwards the hook event on stdin to the ccg handler subcommand, which
# prints one bounded line of code-database status (silent when no db
# exists for the working directory). ccg is located by: CCG_BIN env
# override, then the release layout beside this script, then PATH. Always
# exits 0.
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
    exec "$ccg_bin" hook sessionstart
fi
command -v ccg >/dev/null 2>&1 && exec ccg hook sessionstart
exit 0
