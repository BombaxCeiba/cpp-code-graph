# Installing ccg — release layout, PATH, and docs resolution

ccg ships as ONE flat directory (the *release layout*). Everything the
binary resolves at its own location sits beside it:

```
<dir>/ccg.exe          the binary (`ccg` on POSIX)
<dir>/.ccg/hooks/      Claude Code hook trampolines (cmd + sh variants)
<dir>/.ccg/.claude/    skills + plugin channel materials
<dir>/docs/            CLI.md, REALTIME.md, MSBUILD.md, SCHEMA.md,
                       DESIGN.md, INSTALL.md
<dir>/ccg-install.cmd  PATH installer (cmd / PowerShell)
<dir>/ccg-install.sh   PATH installer (bash, incl. Git Bash)
```

The shape is load-bearing, not cosmetic: `ccg hook install` registers the
trampolines at `<exe-dir>/.ccg/hooks/`, and the packaged manuals
(`CLI.md`, also `REALTIME.md`,
`MSBUILD.md`) live in the `docs/` directory relative to the binary. Splitting the
directory breaks both — keep `ccg`, `.ccg/`, and `docs/` siblings.

## 1. Producing an installed tree

```
cmake --preset clang-dev                     # or: msvc-dev
cmake --build build/clang-dev --config Release
cmake --install build/clang-dev --config Release --prefix <dir>
```

`<dir>` then holds the layout above. Dev trees need none of this: every
build already copies `.ccg/` and the runtime-referenced docs beside the
built executable (the POST_BUILD steps in `src/CMakeLists.txt`), so
`--help` pointers resolve next to a freshly built binary too.

## 2. Putting `ccg` on PATH

From the release directory, run the installer matching your shell:

```
<dir>\ccg-install.cmd            (cmd / PowerShell)
<dir>/ccg-install.sh             (bash)
```

```
ccg-install.cmd [release-dir] [--bin DIR] [--no-path]
ccg-install.sh  [release-dir] [--bin DIR] [--no-path]
```

| option | meaning |
|---|---|
| `release-dir` | directory holding `ccg` + `.ccg` + `docs`; default: the installer's own directory |
| `--bin DIR` | where the shims go; default `%LOCALAPPDATA%\ccg\bin` (cmd) / `~/.local/bin` (sh) |
| `--no-path` | write the shims, leave PATH untouched |

Each installer writes TWO shims that forward to the release binary by
absolute path — `ccg.cmd` (resolved by cmd.exe/PowerShell) and an
extensionless `ccg` (resolved first by bash) — then adds the bin
directory to PATH: the user `Path` registry value on Windows
(`[Environment]::SetEnvironmentVariable`, never `setx` and its
1024-character truncation), one guarded `export PATH` line appended to
`~/.bashrc` under bash.

Properties worth knowing:

- **Idempotent** — re-running refreshes the shims and never duplicates a
  PATH entry.
- **Rebuilds go live** — the shims forward by absolute path, so a new
  binary dropped into the same release directory needs no re-install.
- **New shells only** — PATH edits never affect the terminal you ran the
  installer in; open a new one, then `ccg --version`.
- **Runtime DLLs are NOT shimmed** — the libclang engine still resolves
  at run time through the per-database engine config (`ccg config`,
  `ccg index --llvm-root`), `CCG_LLVM_ROOT`, or PATH; an installer run
  does not change engine discovery.

With `ccg` on PATH, wire the Claude Code integration from a project root:

```
ccg hook install --project
```

(full hook contract: CLI.md § *ccg hook*).

## 3. The Windows installer (`setup.exe`)

Sections 1-2 are the source-build route. The installer automates both
steps and is the channel for machines that only consume ccg:
`ccg-setup-<version>.exe`, attached to the project's GitHub releases
(one per version tag).

- **Per-user install, no administrator** — the wizard installs into
  `%LOCALAPPDATA%\Programs\ccg` and never requests elevation; apart
  from the shim directory below, nothing lands outside the user
  profile. The installed directory is the same flat release layout
  as above.
- **The "Add ccg to PATH" task** — checked by default. It writes the
  two forwarding shims (`ccg.cmd` for cmd/PowerShell, extensionless
  `ccg` for bash) into `%LOCALAPPDATA%\ccg\bin` and idempotently
  appends that directory to the user `Path` — exactly what the
  section-2 installers do. Unchecked, the installer leaves PATH alone
  and it is yours to wire by hand (the section-2 scripts and
  section-4 fallbacks apply unchanged to the installed directory).
- **Uninstall** — removing ccg from Control Panel / Settings removes
  the program directory, the shim directory, and the PATH entry the
  task added.
- **Known boundary** — the `ccg.cmd` shim is written in the system
  ANSI code page: an install path containing characters that code
  page cannot represent breaks the cmd/PowerShell shim (the bash shim
  is UTF-8 and unaffected). Install under a pure-ASCII path.

## 4. Fallbacks and overrides

The shims are a convenience, not a requirement. All of these keep
working unchanged:

- **`CCG_BIN`** set to the binary's full path — honored by the hook
  trampolines before any PATH lookup; a shim-free pin.
- **The release directory itself on PATH** — adding `<dir>` to PATH by
  hand works with no shim at all; that IS the shipped layout.
- **Source tree** — the docs are plain markdown under `docs/` in the
  repository; the copy beside the binary exists so binary-only holders
  read the same texts `--help` points at.
