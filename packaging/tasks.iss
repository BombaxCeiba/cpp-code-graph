; CPack Inno Setup injection: [Tasks] checkbox + [Registry] conditional
; HKCU Path write + bilingual wizard messages. This file is #include'd at
; the top of the CPack-generated setup script (CPACK_INNOSETUP_EXTRA_SCRIPTS
; in cmake/CPackSetup.cmake); sections may appear multiple times in an Inno
; script and are merged, so only the injected parts live here.
;
; The addtopath task implements the shim scheme (same shape as the manual
; scripts/ccg-install.cmd): two forwarding shims in %LOCALAPPDATA%\ccg\bin
; (ccg.cmd for cmd/PowerShell, extensionless ccg for Git Bash) plus one
; idempotent entry in the user Path. The registry write below only appends
; the shim directory; the shim files themselves are written by
; packaging/shim-code.pas after the files land.

[Tasks]
; The single optional item, checked by default (no "unchecked" flag).
Name: "addtopath"; Description: "{cm:TaskAddToPath}"; GroupDescription: "{cm:TaskGroupAdditional}"

[Registry]
; Append the shim directory to the USER Path (HKCU Environment), keeping
; REG_EXPAND_SZ so %VAR% references in the value stay unexpanded for the
; next writer. Never setx (its 1024-char truncation destroys long PATHs).
; Runs only when the task is selected and the entry is genuinely absent
; (idempotent); value assembled by AddToPath in shim-code.pas, which
; preserves the existing entries verbatim. {code:} functions must take a
; single String parameter (VS Code installer precedent), so the shim
; directory is passed in explicitly.
Root: HKCU; Subkey: "Environment"; ValueType: expandsz; ValueName: "Path"; ValueData: "{code:AddToPath|{localappdata}\ccg\bin}"; Tasks: addtopath; Check: NeedsAddToPath(ExpandConstant('{localappdata}\ccg\bin'))

[CustomMessages]
TaskAddToPath=Add ccg to PATH (forwarding shims in %LOCALAPPDATA%\ccg\bin)
TaskGroupAdditional=Additional tasks:
chinesesimplified.TaskAddToPath=把 ccg 加入 PATH（在 %LOCALAPPDATA%\ccg\bin 写入转发垫片）
chinesesimplified.TaskGroupAdditional=附加任务:

[Messages]
; Finish-page notes (both label variants, per-language overrides for the
; bundled Simplified Chinese). %n is a line break; [name] expands to the
; application name. LLVM is NOT bundled: indexing resolves a local LLVM
; installation (bin/libclang.dll) at run time. The ccg.cmd shim is
; written in the system ANSI code page, so a path the code page cannot
; represent turns into '?' there (the bash shim is UTF-8 and unaffected)
; - hence the last note.
FinishedLabel=[name] has been installed.%n%nNotes:%n- The PATH change (if selected) only applies to NEW terminals.%n- Code indexing requires a local LLVM installation containing bin\libclang.dll; LLVM is not bundled with this installer.%n- If the installation path contains characters the system ANSI code page cannot represent, the cmd/PowerShell shim may fail; install under a path covered by the system code page in that case.
FinishedLabelNoIcons=[name] has been installed.%n%nNotes:%n- The PATH change (if selected) only applies to NEW terminals.%n- Code indexing requires a local LLVM installation containing bin\libclang.dll; LLVM is not bundled with this installer.%n- If the installation path contains characters the system ANSI code page cannot represent, the cmd/PowerShell shim may fail; install under a path covered by the system code page in that case.
chinesesimplified.FinishedLabel=[name] 已安装完成。%n%n提示：%n- PATH 修改（若勾选）仅对新开的终端生效。%n- 代码索引需要本机安装了含 bin\libclang.dll 的 LLVM；安装包不附带 LLVM。%n- 若安装路径含系统代码页无法表示的字符，cmd/PowerShell 垫片可能失效；此场景请把 ccg 安装到系统代码页可表示的路径。
chinesesimplified.FinishedLabelNoIcons=[name] 已安装完成。%n%n提示：%n- PATH 修改（若勾选）仅对新开的终端生效。%n- 代码索引需要本机安装了含 bin\libclang.dll 的 LLVM；安装包不附带 LLVM。%n- 若安装路径含系统代码页无法表示的字符，cmd/PowerShell 垫片可能失效；此场景请把 ccg 安装到系统代码页可表示的路径。
