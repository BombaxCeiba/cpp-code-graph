# ccg 的 Claude Code 集成

ccg 是一个 C++ 代码图命令行工具（符号/调用/继承/使用关系入库，提供查询、影响分析、调用链追踪）。本目录是它随发行包附带的 Claude Code 集成物料：一组技能（skill）、一个插件（plugin）和三个 hook 脚本。

## 发行布局

发行包是一个自足的目录：

```
<发行目录>/
  ccg（或 ccg.exe）
  .ccg/
    hooks/        # hook 脚本（Windows .cmd + POSIX .sh 双变体）
    .claude/
      README.md   # 本文件
      skills/     # ccg-* 技能家族（单一事实源）
      plugin/     # 插件（plugin.json + commands + hooks；构建时折入 skills）
```

源码树中这些物料位于 `publish/` 下，构建时由构建系统幂等拷贝到可执行文件旁边的 `.ccg/`。技能正文里不写死任何机器路径：ccg 一律通过 `PATH` 发现（`CCG_BIN` 环境变量可指定显式路径）。

## 安装

三选一（可叠加；hook 的效果完全一致，只是生效通道不同）。

### 途径一：项目级技能 + hook（推荐给单仓使用）

```sh
# 1) 把技能拷进目标仓库（每台机器每个仓库一次）
cp -r <发行目录>/.ccg/.claude/skills/ccg-*  <目标仓库>/.claude/skills/

# 2) 在目标仓库根目录安装 hook（写入 <目标仓库>/.claude/settings.json）
ccg hook install --project
```

### 途径二：用户级（所有会话生效）

```sh
cp -r <发行目录>/.ccg/.claude/skills/ccg-*  ~/.claude/skills/
ccg hook install --user      # 写 ~/.claude/settings.json
```

### 途径三：插件（hooks 随插件自动生效，不改 settings.json）

用 `<发行目录>/.ccg/.claude/plugin/` 作为本地插件目录安装（或把它放进你的插件市场仓库）。插件自带：

- `hooks/hooks.json`——装上即生效的 PostToolUse/SessionStart/UserPromptSubmit hooks，命令直接调 `ccg hook posttooluse` / `ccg hook sessionstart` / `ccg hook userpromptsubmit`（要求 ccg 在 PATH 上，或设 `CCG_BIN`）；
- `commands/`——`/ccg:index` 与 `/ccg:impact` 两条 slash 命令（薄入口，转发到同名技能）；
- `skills/`——构建时从 `skills/` 折入的同一套技能（源码树里插件目录不含技能副本，单一事实源在 `skills/`）。

装好后用 `ccg hook status --project`（或 `--user`）核对。

## hook 的注入效果

安装（任一途径）后：

- **每次 Edit/Write/MultiEdit/Update 之后**（PostToolUse）：自动把该文件的改动折入代码图数据库（`ccg update` 的小批 drain），并向会话注入一行影响面摘要，形如
  `[ccg] 已入库 canvas.hpp（重析 3 TU）；受影响符号 4（直接 3），整体风险 MEDIUM；最相关: shape::Circle (circle.hpp:12)、...`
- **会话启动时**（SessionStart）：注入一行数据库状态（存在/规模/索引时间）；项目还没有数据库时保持静默。
- **批次运行期间**（PostToolUse 快速路径）：编辑只入队并写一条 journal 后立即返回，不派生刷新、不算影响面。
- **用户下一次输入前**（UserPromptSubmit）：批次跑完且队列为空后，把积压期间的结论一次性注入（每个文件一行：符号、直接/传递消费者数、风险级、最相关消费者），1200B 截断为"等 N 项"；仍在跑时静默。注入后清空，绝不重复。

数据库不存在时 hook 永不报错、零输出——先跑 `ccg index` 建库（见 `ccg-index` 技能）。非 C/C++ 文件的编辑同样静默跳过。

## 卸载

```sh
ccg hook uninstall --project          # 默认 dry-run，打印将删除什么
ccg hook uninstall --project --force  # 实际落地
ccg hook uninstall --user --force     # 用户级同理
```

卸载只删除 ccg 自己的条目：settings.json 里用户既有的 hook、注释、格式原样保留；删空的容器一并清掉，零残留。技能目录按安装途径手动删除（`<仓库>/.claude/skills/ccg-*` 或 `~/.claude/skills/ccg-*`）；插件按插件管理方式移除。

## 验证

```sh
ccg --version                       # ccg 可达（PATH 或 CCG_BIN）
ccg hook status --project           # 三个事件各 1 条、脚本 present
ccg index .                         # 建库（详见 ccg-index 技能）
echo '{"hook_event_name":"SessionStart"}' | ccg hook sessionstart
# → stdout 一行 {"hookSpecificOutput":{...,"additionalContext":"[ccg] 代码图数据库就绪..."}}
```

技能家族一览：`ccg-guide`（总览/路由）、`ccg-index`（索引/引擎/hook）、`ccg-exploring`（代码走读）、`ccg-impact`（影响分析）、`ccg-debugging`（调试溯源）。在 Claude Code 里也自动以 `/ccg-guide` 等技能名调用。
