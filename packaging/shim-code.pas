{ CPack Inno Setup injection (CPACK_INNOSETUP_CODE_FILES): the Pascal half
  of the addtopath task. Included at the bottom of the generated script, so
  no [Code] header here and no section switches.

  Responsibilities:
  - AddToPath/NeedsAddToPath: value + guard for the [Registry] entry in
    tasks.iss (idempotent append of the shim directory to the user Path,
    REG_EXPAND_SZ preserved, existing entries kept verbatim).
  - CurStepChanged(ssPostInstall): write the two forwarding shims into
    %LOCALAPPDATA%\ccg\bin when the task is selected - ccg.cmd with CRLF
    (cmd/PowerShell) and the extensionless ccg with LF only (Git Bash
    refuses a CRLF shebang). Same shape as scripts/ccg-install.cmd.
  - CurUninstallStepChanged(usUninstall): remove the shim directory (and
    the empty parent), then filter the shim entry out of the user Path by
    semicolon-splitting into an array and writing the rejoined value back
    with RegWriteExpandStringValue (keeps REG_EXPAND_SZ). Known boundary,
    same as the VS Code installer precedent: entries the user added
    manually pointing at the identical directory are removed too. The
    ChangesEnvironment=yes [Setup] directive makes Windows broadcast the
    environment change after (un)install.

  Encoding facts (verified against Inno Setup 7.1.1 sources and live
  byte-level runs of these very functions):
  - SaveStringToFile transcribes through the system ANSI code page
    (prototype: const S: AnsiString). For ccg.cmd that is correct: cmd
    reads batch files in the system code page, so the two agree. For the
    bash shim it is wrong whenever the install path is non-ASCII (Git
    Bash reads UTF-8).
  - SaveStringsToUTF8FileWithoutBOM writes UTF-8 without a BOM but ends
    every array element with CRLF, so the bash shim is emitted as ONE
    array element with embedded LF line breaks; the writer then appends
    a single trailing CRLF. The trailing CR-only line is never reached:
    a successful exec replaces the shell process. Only a failed exec
    falls through to a harmless "\r: command not found".
  - Path entry comparison expands %VAR% references on both sides before
    matching (Inno has no ExpandEnvironmentStrings script function - it
    is a Windows API, not a support function - so expansion is done with
    GetEnv over the percent-split chunks). This makes the %LOCALAPPDATA%
    \ccg\bin unexpanded form equal to the expanded form for both the
    install idempotence check and the uninstall filter, so no duplicate
    entry is appended and no unexpanded twin survives uninstall. Entries
    are kept verbatim when not matching; undefined variables keep their
    literal %name% text, matching Windows ExpandEnvironmentStrings. }

const
  CcgShimSubDir = '\ccg\bin';

function GetShimDir(): String;
begin
  Result := ExpandConstant('{localappdata}') + CcgShimSubDir;
end;

{ Split a Path value on semicolons. Empty entries (';;', leading or
  trailing ';') are preserved as empty strings so a rebuild of the value
  keeps the user's text intact. }
function SplitPathEntries(const Path: String): TArrayOfString;
var
  Rest: String;
  Semi, N: Integer;
begin
  SetArrayLength(Result, 0);
  Rest := Path;
  while Rest <> '' do begin
    Semi := Pos(';', Rest);
    N := GetArrayLength(Result);
    SetArrayLength(Result, N + 1);
    if Semi = 0 then begin
      Result[N] := Rest;
      Rest := '';
    end
    else begin
      Result[N] := Copy(Rest, 1, Semi - 1);
      Delete(Rest, 1, Semi);
    end;
  end;
end;

{ Expand %VAR% references using the installer process environment.
  Chunks between percent signs alternate literal / variable name. An
  empty or undefined variable keeps its literal %name% text (the Windows
  ExpandEnvironmentStrings behaviour), so garbage stays recognizable
  instead of silently vanishing. }
function ExpandPercentVars(const S: String): String;
var
  Chunks: TArrayOfString;
  Rest, Name, Val: String;
  P, N, I: Integer;
begin
  Result := '';
  Rest := S;
  SetArrayLength(Chunks, 0);
  while Rest <> '' do begin
    P := Pos('%', Rest);
    N := GetArrayLength(Chunks);
    SetArrayLength(Chunks, N + 1);
    if P = 0 then begin
      Chunks[N] := Rest;
      Rest := '';
    end
    else begin
      Chunks[N] := Copy(Rest, 1, P - 1);
      Delete(Rest, 1, P);
    end;
  end;
  for I := 0 to GetArrayLength(Chunks) - 1 do begin
    if (I mod 2) = 1 then begin
      Name := Chunks[I];
      Val := GetEnv(Uppercase(Name));
      if (Name = '') or (Val = '') then
        Result := Result + '%' + Name + '%'
      else
        Result := Result + Val;
    end
    else
      Result := Result + Chunks[I];
  end;
end;

{ True when some Path entry equals ShimDir after %VAR% expansion on
  both sides (case-insensitive). ShimDir itself comes from the expanded
  localappdata constant, so only the Path entries need expanding. }
function PathHasEntry(const Path, ShimDir: String): Boolean;
var
  Parts: TArrayOfString;
  I: Integer;
begin
  Result := False;
  Parts := SplitPathEntries(Path);
  for I := 0 to GetArrayLength(Parts) - 1 do
    if CompareText(ExpandPercentVars(Parts[I]), ShimDir) = 0 then begin
      Result := True;
      exit;
    end;
end;

function NeedsAddToPath(ShimDir: String): Boolean;
var
  Path: String;
begin
  if RegQueryStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', Path) then
    Result := not PathHasEntry(Path, ShimDir)
  else
    Result := True;
end;

function AddToPath(ShimDir: String): String;
var
  Path: String;
begin
  if not RegQueryStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', Path) then
    Path := '';
  if Path = '' then
    Result := ShimDir
  else if Copy(Path, Length(Path), 1) = ';' then
    Result := Path + ShimDir
  else
    Result := Path + ';' + ShimDir;
end;

procedure WriteShims();
var
  ShimDir, ExePath, FwdPath, CmdShim: String;
  BashLines: TArrayOfString;
begin
  ShimDir := GetShimDir();
  ExePath := ExpandConstant('{app}\ccg.exe');
  FwdPath := ExePath;
  StringChangeEx(FwdPath, '\', '/', True);
  { ccg.cmd: system ANSI code page + CRLF, matching how cmd itself reads
    batch files. Known boundary: a path the ANSI code page cannot
    represent turns into '?' here - the finish page tells users to
    install under a path their system code page covers. }
  CmdShim := '@rem ccg shim written by the ccg installer' + #13#10 +
             '@"' + ExePath + '" %*' + #13#10;
  { ccg (bash shim): UTF-8 without BOM so Git Bash decodes non-ASCII
    install paths correctly; LF line breaks inside a single array
    element because the writer appends CRLF after each element. }
  SetArrayLength(BashLines, 1);
  BashLines[0] := '#!/bin/sh' + #10 +
                  'exec "' + FwdPath + '" "$@"' + #10;
  if not ForceDirectories(ShimDir) then
    RaiseException('ccg installer: cannot create shim directory ''' +
      ShimDir + '''');
  if not SaveStringToFile(ShimDir + '\ccg.cmd', CmdShim, False) then
    RaiseException('ccg installer: cannot write ''' + ShimDir + '\ccg.cmd''');
  if not SaveStringsToUTF8FileWithoutBOM(ShimDir + '\ccg', BashLines, False) then
    RaiseException('ccg installer: cannot write ''' + ShimDir + '\ccg''');
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep <> ssPostInstall then
    exit;
  if not WizardIsTaskSelected('addtopath') then
    exit;
  WriteShims();
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Path, NewPath, ShimDir: String;
  Parts: TArrayOfString;
  I, Kept: Integer;
begin
  if CurUninstallStep <> usUninstall then
    exit;

  { Shims first: the bin directory and its files, then the now-empty
    parent (RemoveDir only succeeds on an empty directory, so a ccg
    directory reused by anything else is left alone). }
  ShimDir := GetShimDir();
  DelTree(ShimDir, True, True, True);
  RemoveDir(ExpandConstant('{localappdata}') + '\ccg');

  { Filter the shim entry out of the user Path. Split on semicolons,
    drop entries that equal the shim directory after %VAR% expansion on
    both sides (case-insensitive; this also removes the unexpanded
    %LOCALAPPDATA% form), keep everything else verbatim, rejoin, and
    write back only when something actually changed - keeps
    REG_EXPAND_SZ and leaves the value alone when the task was never
    selected. }
  if not RegQueryStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', Path) then
    exit;
  Parts := SplitPathEntries(Path);
  NewPath := '';
  Kept := 0;
  for I := 0 to GetArrayLength(Parts) - 1 do begin
    if CompareText(ExpandPercentVars(Parts[I]), ShimDir) <> 0 then begin
      if Kept > 0 then
        NewPath := NewPath + ';';
      NewPath := NewPath + Parts[I];
      Kept := Kept + 1;
    end;
  end;
  if NewPath <> Path then
    RegWriteExpandStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', NewPath);
end;
