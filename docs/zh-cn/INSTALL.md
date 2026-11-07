# 安装 ccg —— 发布布局、PATH 与文档解析

ccg 以单个扁平目录（即*发布布局*）交付。二进制程序在自身
所在位置解析的一切内容都摆放在它旁边：

```
<dir>/ccg.exe          the binary (`ccg` on POSIX)
<dir>/.ccg/hooks/      Claude Code hook trampolines (cmd + sh variants)
<dir>/.ccg/.claude/    skills + plugin channel materials
<dir>/docs/            CLI.md, REALTIME.md, MSBUILD.md, SCHEMA.md,
                       DESIGN.md, INSTALL.md
<dir>/ccg-install.cmd  PATH installer (cmd / PowerShell)
<dir>/ccg-install.sh   PATH installer (bash, incl. Git Bash)
```

这一目录形状是承重结构，不是装饰：`ccg hook install` 会把
跳板脚本（trampoline）注册到 `<exe-dir>/.ccg/hooks/`，随包
发布的手册（`CLI.md`，以及 `REALTIME.md`、
`MSBUILD.md`）则存放在相对二进制程序的 `docs/` 目录里。把
目录拆散会同时破坏这两件事——请让 `ccg`、`.ccg/` 与 `docs/` 保持同级。

## 1. 生成已安装目录树

```
cmake --preset clang-dev                     # or: msvc-dev
cmake --build build/clang-dev --config Release
cmake --install build/clang-dev --config Release --prefix <dir>
```

此后 `<dir>` 中就是上面这套布局。开发目录树不需要这些步骤：每次
构建都已经把 `.ccg/` 和运行时引用的文档复制到构建出的可执行文件
旁边（即 `src/CMakeLists.txt` 里的 POST_BUILD 步骤），因此
`--help` 指向的内容在刚构建出的二进制程序旁边同样能解析。

## 2. 将 `ccg` 纳入 PATH

在发布目录下，运行与你所用 shell 相匹配的安装器：

```
<dir>\ccg-install.cmd            (cmd / PowerShell)
<dir>/ccg-install.sh             (bash)
```

```
ccg-install.cmd [release-dir] [--bin DIR] [--no-path]
ccg-install.sh  [release-dir] [--bin DIR] [--no-path]
```

| 选项 | 含义 |
|---|---|
| `release-dir` | 存放 `ccg` + `.ccg` + `docs` 的目录；默认：安装器自身所在目录 |
| `--bin DIR` | 垫片（shim）的存放位置；默认 `%LOCALAPPDATA%\ccg\bin`（cmd）/ `~/.local/bin`（sh） |
| `--no-path` | 只写入垫片，不改动 PATH |

每个安装器都会写入两个垫片，它们按绝对路径转发到发布版二进制
程序——一个是 `ccg.cmd`（由 cmd.exe/PowerShell 解析），另一个是
不带扩展名的 `ccg`（bash 最先解析它）——然后把 bin 目录加入
PATH：在 Windows 上写入用户级 `Path` 注册表值（用
`[Environment]::SetEnvironmentVariable`，绝不用 `setx` 及其
1024 字符截断问题）；在 bash 下则向 `~/.bashrc` 追加一行
带守卫的 `export PATH`。

值得了解的特性：

- **幂等**——重复运行会刷新垫片，也绝不会重复添加
  PATH 条目。
- **重建即时生效**——垫片按绝对路径转发，把新的二进制
  程序放进同一发布目录后无需重新安装。
- **仅对新 shell 生效**——修改 PATH 不会影响你运行安装器的
  那个终端；另开一个新终端，再执行 `ccg --version`。
- **运行时 DLL 并不经垫片转发**——libclang 引擎仍在运行时通过
  每个数据库各自的引擎配置（`ccg config`、
  `ccg index --llvm-root`）、`CCG_LLVM_ROOT` 或 PATH 解析；运行
  安装器并不会改变引擎发现方式。

在 `ccg` 已进入 PATH 之后，从项目根目录接入 Claude Code 集成：

```
ccg hook install --project
```

（完整钩子契约：CLI.md § *ccg hook*。）

## 3. Windows 安装器（`setup.exe`）

第 1、2 节是源码构建路线。安装器把这两步自动化，是面向
只使用 ccg 的机器的渠道：`ccg-setup-<版本>.exe`，挂在项目的
GitHub release 页上（每个版本 tag 一份）。

- **按用户安装、无需管理员**——向导把 ccg 装进
  `%LOCALAPPDATA%\Programs\ccg`，全程不请求管理员权限；除下述
  垫片目录外，不在用户配置文件之外落任何东西。装出的目录
  就是上文那套扁平发布布局。
- **「把 ccg 加入 PATH」勾选项**——默认勾选。它向
  `%LOCALAPPDATA%\ccg\bin` 写入两个转发垫片（cmd/PowerShell 用的
  `ccg.cmd`、bash 用的无扩展名 `ccg`），并把该目录幂等追加进
  用户 `Path`——与第 2 节的手动安装器所做的事完全一致。取消
  勾选则安装器不改动 PATH，由你手动配置（第 2 节脚本与第 4 节
  回退方式对已安装目录照常适用）。
- **卸载**——在控制面板/设置的「应用」里卸载 ccg，会一并移除
  程序目录、垫片目录以及勾选项写入的 PATH 条目。
- **已知边界**——`ccg.cmd` 垫片以系统 ANSI 代码页写入：安装
  路径含该代码页无法表示的字符时，cmd/PowerShell 垫片会失效
  （bash 垫片是 UTF-8，不受影响）。请把 ccg 安装到纯 ASCII
  路径。

## 4. 回退与覆盖方式

垫片是一种便利，而非硬性要求。以下这些方式全都
照常可用：

- **`CCG_BIN`** 设为二进制程序的完整路径——钩子跳板会在任何
  PATH 查找之前优先采用它；一种不依赖垫片的固定方式。
- **发布目录本身就在 PATH 上**——手工把 `<dir>` 加入 PATH 即可，
  完全不需要垫片；那本身就是随包发布的布局。
- **源码树**——这些文档就是仓库中 `docs/` 下的普通 markdown；
  二进制程序旁边的那份副本之所以存在，是为了让只拿到
  二进制程序的人也能读到 `--help` 所指向的同一批文本。
