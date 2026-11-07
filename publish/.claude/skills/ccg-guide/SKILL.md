---
name: ccg-guide
description: "Use when 用户想了解 ccg 本身——它是什么、有哪些命令、核心概念（数据库/引擎/缓存）如何理解，或不知道该用哪个 ccg 技能。Examples: \"ccg 怎么用\", \"这个项目有哪些代码图命令\", \"CCG_DB 是什么\", \"ccg 的数据库存在哪\""
---

# ccg 总览与命令速查

ccg 是一个 C++ 代码图命令行工具：把项目的符号（函数/类/方法/字段……）和它们之间的关系（调用、继承、使用、重写）解析进一个 SQLite 数据库，然后提供查询、影响分析、调用链追踪等命令。本技能是全家族的路由表和概念底座；具体工作流见文末的路由表。

## 何时使用

- 用户问"ccg 是什么 / 有哪些命令 / 数据库在哪 / 引擎是什么"
- 用户想配置或理解 ccg 的行为（环境变量、引擎、缓存）
- 你不确定该用哪个 ccg-* 技能，需要先看路由表

## 发现 ccg 与数据库

- **ccg 可执行文件**：一律通过 `PATH` 发现；环境变量 `CCG_BIN` 指定显式路径时可覆盖。本文所有命令都假设 `ccg` 在 PATH 上。
- **数据库（db）发现**：`$CCG_DB` > `-o DIR`（产出文件夹）> 默认 `./.ccg/`；文件夹下每配置一个子目录 `<config>/ccg.db`。**唯一配置时 `--config` 可省略自动选中；多配置必须显式（报错列候选）**；`$CCG_DB` 也可直接指某个配置目录。跨目录工作时用 `-o DIR` 或 `export CCG_DB`。
- 体检一条命令：`ccg stats` 打印 db 路径、schema 版本、文件/符号/边计数、db 大小。db 不存在时退出码 1。

## 核心概念

| 概念 | 含义 |
|---|---|
| db（数据库） | 每配置一个 SQLite 文件（`<文件夹>/<配置>/ccg.db`，默认 `./.ccg/<配置>/`），存全部符号、边、include 关系与配置。查询命令都是对它的只读操作 |
| 引擎 | ccg 运行时加载的 libclang 动态库，负责解析。发现链：db 内 `llvm-path` 配置 > `--llvm-root` > `$CCG_LLVM_ROOT` > PATH 发现。引擎身份记录在 db 里，换引擎自动触发全量重索引 |
| BMI 自建缓存 | 索引 C++ 模块项目时，ccg 在项目根的 `.ccg/bmi-self-*` 目录下自建模块 BMI（.pcm）缓存。这是派生数据，可加入 .gitignore；除非更换引擎，不要手动删除 |
| 符号 | 函数/方法/记录（类/结构）/构造/析构/字段/变量/枚举/枚举常量/类型别名，共 11 种 kind |
| 边 | 符号间的关系：calls（调用）、extends（继承）、overrides（重写）、uses（类型使用）等 |
| override 家族 | 虚方法及其全部重写构成一个家族。默认查询合并整个家族的边（输出带 `[family: ...]` 标记），`--no-merge` 只查单符号 |
| 动态派发证据 | virtual_dispatch 边默认参与查询，证据阈值为 0.7（1.0/0.7）：有证明的调用点按证据目标归属，不再按整个 override 家族过近似；0.4 弱推断低于阈值、不命名动态目标 |
| 家族估算 | 动态类型无法证明时，按该虚方法的整个 override 家族估算（可能包含并非实际目标的实现），输出标 `[dispatch unknown · family estimate]`；`--no-dispatch` 是纯静态保守上界，`--dispatch-fallback` 额外显示 0.0 标记 |

## 命令速查

| 命令 | 一句话 |
|---|---|
| `ccg index <roots>...` | 建库/增量索引（自动发现 compile_commands.json；找不到则报错退出，不兜底） |
| `ccg update [<file>...]` | 近实时刷新改动的文件（db 协调队列；`--drain` 清空队列） |
| `ccg msbuild <path.sln>` | 索引 MSBuild 解决方案（Windows 专属，每配置一个 db；仅接受 .sln——.vcxproj/目录输入被拒绝并提示传 .sln） |
| `ccg config get/set/unset` | per-db 配置（llvm-path 引擎路径、index-jobs 并行上限、update 协调旋钮） |
| `ccg query <name>` | 按名字找符号（多命中是罗列，永不歧义报错）；`--implements` 反转为"谁派生自它"（类型继承树，含间接，depth 列标层级） |
| `ccg context <sym>` | 符号 360 度视图（incoming/outgoing 全边） |
| `ccg callers <sym>` / `ccg callees <sym>` | 谁调用它 / 它调用谁（`--depth` 1-10） |
| `ccg members <record>` | 类的成员清单（own + 沿继承链的 inherited 分组，`--kind/--file` 过滤） |
| `ccg impact <sym>` | 改动影响面与风险分级（up=谁依赖它，down=它依赖谁） |
| `ccg trace <a> <b>` | 两符号间最短调用路径 |
| `ccg changes <file>...` | 我改的文件影响谁（VCS 无关，喂文件列表） |
| `ccg check --cycles` | include 环检测（退出码 3 可作 CI 门禁） |
| `ccg graph --symbol <sym>` / `--modules` | 调用图导出（Graphviz dot / JSON；`--symbol` 入口展开默认深度 2，`--depth 0` 为不限深度全闭包；`--modules` 按定义文件聚合，`--min-weight` 降噪） |
| `ccg stats` | db 摘要 |
| `ccg hook install/uninstall/status` | 管理 Claude Code 集成（hooks 注入） |

## 通用规则（所有命令共享）

- **机器模式**：`--json` 时 stdout 恰好一行合法 JSON，进度与提示全走 stderr——脚本和 hook 可以放心单行 parse。
- **退出码**：0 成功（含 `truncated` 结果）；1 未找到/无路径/目标歧义/运行时错误；2 用法错误；3 = `check --cycles` 发现环。
- **符号消歧**：`query/context/callers/callees/impact/trace` 与 `graph --symbol` 都接受 `--kind K` 与 `--file SUBSTR` 收窄。歧义时报错会列出候选表（kind、限定名、file:line）。
- **显示 nit**：文本表格的列宽固定，长路径/长限定名会错位溢出——数据本身完整，机器模式 `--json` 无此问题。不要把表格错位当成数据损坏上报。

## skill 路由表

| 场景 | 技能 |
|---|---|
| 建库、增量、引擎配置、MSBuild、update 刷新 | `ccg-index` |
| 读懂一段代码：找符号、看关系、顺调用链 | `ccg-exploring` |
| 改动前评估安全性：影响面、风险分级、改动文件波及 | `ccg-impact` |
| 调试溯源：症状符号与根因候选间的连通性验证 | `ccg-debugging` |
| 概念/命令/路由问题 | 本技能 |

## hook 与"db 常新"

装上 Claude Code hook（`ccg hook install --project` 或 `--user`，或通过插件）后：每次 Edit/Write 之后 db 自动折入该文件的改动，会话中得到一行影响面摘要；会话启动时注入一行 db 状态；批次运行期间积压的编辑会在批次跑完后由 `UserPromptSubmit` 一次性汇总（每个文件一行，含符号/直接与传递消费者数/风险级）；每次字符串查找（Grep，或 Bash/PowerShell 里 `rg`/`Select-String` 形态的命令、Search 工具载荷——非查找命令零成本静默）之前，`PreToolUse` 钩子只读注入命中符号的五行事实或同名候选清单，其静默出口与 Read/Glob 事件还会机会式投递上面提到的积压结论（无人值守时末批积压靠这个到账）。详见 `ccg-index` 的 hook 章节。没有 hook 时，`ccg update <file>` 手动刷新、`ccg update --drain` 收尾。

## 边界

- 本技能只教 ccg 自身存在的能力；上面速查表之外的命令、旗标不存在，不要发明。
- ccg 与版本控制系统完全无关：它绝不调用 git，"哪些文件改了"由你喂给它（`ccg changes`）。
- ccg 不修改任何源代码；它只读代码、写自己的 db 与缓存。
