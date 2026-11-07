---
name: ccg-index
description: "Use when 用户需要为 C++ 项目建代码图索引、增量更新、配置解析引擎、索引 MSBuild 解决方案，或设置 ccg 的 Claude Code hook 让数据库自动保持常新。Examples: \"给这个项目建索引\", \"代码图怎么更新\", \"ccg hook 怎么装\", \"换一个 LLVM 引擎\", \"索引这个 sln\""
---

# ccg 索引管理与 db 常新

索引是其它一切 ccg 命令的前置：没有配置库时任何查询命令都报 `no configuration found ...` 或 `cannot open database ... file not found`（退出码 1）。本技能覆盖建库、增量、引擎配置、MSBuild 路线、近实时刷新与 hook 安装。

## 何时使用

- "给这个项目建索引 / 重建索引"
- "代码图落后了 / 怎么更新"
- "装上 hook 让 db 自动刷新"
- "配置/更换解析引擎（llvm-path）"
- "索引 MSBuild 解决方案（.sln）"

## 前置检查

1. 确认 ccg 可用：`ccg --version`（PATH 发现；`CCG_BIN` 可覆盖）。
2. 判断库是否已存在：在项目根跑 `ccg stats`。
   - 有输出 → 库已在（或列候选配置），走增量路线。
   - 退出码 1 报 no configuration found → 首次建库，走下节。
   - 报 multiple configurations → 该文件夹下有多个配置，查询族命令须加 `--config <名>`。

## 解析引擎选择（首次索引前必读）

`ccg index` 需要一个 LLVM/clang 安装前缀作为解析引擎：**一个前缀、其 bin 下同时有 clang 驱动（clang/clang++/clang-cl）与 libclang 动态库（libclang.dll/.so）**，版本决定整套解析世界观（同一 db 恒由同一引擎解析，换引擎自动触发全量重建）。**首次为某项目建索引前，强烈推荐先询问用户确认要用哪个 clang 目录**，话术要点：

- 要的是"一个 LLVM/clang 安装前缀"——bin 下同时有 clang 驱动与 libclang（.dll/.so），缺一不可。
- 典型例子：独立 LLVM 安装（如 `C:/Program Files/LLVM` 或自编译安装前缀）；msys2 的 clang64 环境（`/c/msys64/clang64`，bin 下两者齐备）；**坑：Visual Studio 内置的 LLVM 组件通常只有 clang 驱动、不带 libclang**，指它会在引擎加载期失败。
- **若不指定，默认使用 PATH 上的 clang/clang-cl**（由驱动器反查其安装前缀找 libclang；PATH 上没有 clang 时报可行动错误）。
- 确认后两种给法：本次会话 `ccg index --llvm-root <前缀> ...`；长期钉住 `ccg config set llvm-path <前缀>`（db 级钉，优先于 --llvm-root）。

## 建库：ccg index

```
ccg index [<roots>...] --config <名> [--compile-commands F] [--llvm-root DIR]
           [--full] [--dry-run] [--paranoid] [--force] [--jobs N]
           [-o DIR] [-v] [--json]
```

- **`--config <名>` 永不可省**（创建与复跑都要）：它命名配置目录 `<文件夹>/<名>/`，`ccg.db`、BMI 缓存、`profile.json` 全住里面。`-o` 只许给**产出文件夹**（默认 `$CCG_DB` 或 `./.ccg`）；给文件路径是响亮的用法错误。
- **配置档案（profile.json）**：首次成功运行记录 roots、compile-commands 路径、引擎身份与创建参数；此后 `ccg index --config <名>` 裸复跑即从档案恢复全部输入（命令行显式值优先并回写档案）。
- **多配置 compile db 自动拆分**：compile_commands.json 内含多个构建配置（多配置生成器如 Ninja Multi-Config）时，index 按内部配置拆成多个配置目录各自成库（目录名=内部配置名，如 `.ccg/Release/`），一配置一 db、无例外。
- **compile db 是前提**：显式 `--compile-commands` > 每个 root 自查（root、root/build、一层浅扫）；找不到就退出 2，并给出三条可行动提示（CMake `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`、`ccg msbuild`、`--compile-commands <file>`）。**绝不递归扫源文件兜底**：C++ 没有有意义的简单兜底，错图比缺图危险。
- **增量是默认**：对已有 db，只有主文件或传递 include 闭包变化（或编译命令 digest 变化）的 TU 才重解析。`--full` 全量；`--force` 与 `--full` 同为全量重建入口（旧别名，另会在 db 的 schema_version 不匹配时直接重建 db 文件）；`--dry-run` 毫秒级打印计划不写库——在任何引擎加载、模块自建扫描、影子 PCH 规划之前短路，只按已有库的逐条目新鲜度报"脏/跳过 TU 数 + 估算工作量"（不解析，include 结构变化要等下次真跑才进清扫估计）；`--paranoid` 忽略 mtime 快路径全部做哈希；`--jobs N`（1-64，模块 BMI 构建与解析池共用同一上限，两阶段不重叠、峰值并发即 N）只影响并行度不影响结果——优先级：`--jobs` 命令行 > 库配置 `index-jobs` > 自动（`min(核数-2, 12)`）。
- **输出要点**：`fresh/stale/skipped/failed` 计数、符号/边分解、耗时与 db 大小。**failed > 0 时退出码仍是 0**（db 可用）；诊断入口是输出尾部的具名 failure classes 行（如 `module-bmi-missing`、`parse-failed`）。
- 首次索引大型项目后跑一次 `ccg stats` 确认规模合理。

### 查询侧的库发现规则（贯穿一切命令）

`CCG_DB` > `-o` 文件夹 > 默认 `./.ccg/`（`CCG_DB` 也可直接指某个配置目录）。**文件夹下唯一配置时 `--config` 可省略自动选中；多配置必须显式 `--config <名>`（报错会列出候选）**。文件夹下无任何配置时命令报错，并提示用 `ccg index --config <名> <项目根>` 创建配置。

### 模块项目（C++20 modules）

有 compile db 的模块 TU 若记录命令缺模块旗标，ccg 自动改走 **BMI 自建**：缓存在**配置目录**下 `.ccg/<config>/bmi-self-<platform>/<engine_key>/<config_key>/`。要点：

- 这是派生数据，可随 .gitignore 处理。
- 不要手动删 engine 目录，除非有意更换引擎——引擎变更会自动整目录作废重建。
- 33MB 级的 std pcm 跨配置复用：新配置首跑会从兄弟配置目录拷贝同一生产者产出（同命令摘要），不重编。

## 引擎配置：ccg config

```
ccg config get [key]        # 无键时打印全部键：llvm-path、recorded engine（库内记录，权威）、
                            # engine/version（当前环境解析）、两个 update 旋钮行、index-jobs
ccg config set llvm-path <前缀或libclang文件>
ccg config unset llvm-path
```

- 发现优先级：db 配置 > `--llvm-root` > `$CCG_LLVM_ROOT` > PATH 发现。
- **引擎分行显示**：`config get` 把"库内记录引擎"（权威——库里的结果是它产的）与"当前环境解析到的引擎"分开两行；两者不一致时（PATH 上别的 clang 盖过库内引擎）打醒目警告"裸跑 index 会换引擎触发全量重建"。
- `llvm-path` 必须存在，否则退出码 1。
- **改/清 llvm-path 都会打重建忠告**：库里所有结果都是旧引擎产的，需要 `ccg index --full --config <名>`（或裸复跑，它检测到引擎变更会自动强制全量）。
- 另两个键：`update-lease-ms`、`update-busy-timeout-ms`（正整数，见下节）。
- `set index-jobs N`：`index`/`update`/`msbuild` 共用的并行 worker 上限（模块 BMI 构建与 TU 解析池都受它管；两阶段不重叠，峰值并发即 N，BMI 构建层内另按层宽封顶）。`0` = 自动（默认，`min(核数-2, 12)`），`1`-`64` 生效，非此范围退出 1；`--jobs N` 命令行对本次运行优先。改它只改速度，绝不触发任何重解析（任何 N 存库行完全一致）。
- 解析旗标注入见下节 `clang-compile-flags`（索引失败率高的工程先看它）。

## 解析旗标注入：clang-compile-flags

clang 与 MSVC 的模板语义分歧是常态（MSVC 默认不检查未实例化的模板体，clang 在定义处即查——错误文本如 `no matching constructor`、`call to non-static member function without an object argument`）。三层机制，按排查顺序：

1. **翻译层等效旗标（自动、透明）**：MSBuild 路线翻译 cl 命令行时，凡**未显式声明模板模式**（无 `/permissive-`、无 `/Zc:twoPhase` 系列）的条目，末尾自动补 `/Zc:twoPhase-`——这是 MSVC 默认延迟解析在 clang-cl 里的**官方等效拼写**，写在 compile_commands.json 里看得见（数据库里写什么引擎就跑什么）；显式声明过模板模式的条目原样翻译。
2. **配置键救援（排障必备）**：仍失败的 TU 会记 `msvc-permissive-*` 失败类（方言分歧具名标签，不是工程代码的错）。遇到它就走标准排障流程：
   ```
   ccg config set clang-compile-flags "-fdelayed-template-parsing"
   ccg index --config <名>
   ```
   为什么需要：clang 的延迟解析只是 MSVC 默认行为的近似（子集），射程外的形状必须显式加码。一次性用法 `--clang-compile-flags "..."`（index / update / msbuild 都接受、可重复）；注入在重放层**末尾**追加、不改 compile_commands.json 文件，`ccg msbuild` 重新生成时配置不丢；优先级：命令行 > 配置键 > 无。**改旗标后跑普通增量即可**——注入旗标计入编译命令摘要，全部 TU 的摘要随之变化，增量机制自动全量重解析（`--full` 冗余）。
3. **失败如实记名（不兜底）**：配置后仍过不了的形状保持失败、绝不静默放宽——出问题时打开编译数据库逐字核对实际喂给引擎的参数即可归属。

## 失败类速查：失败形态 → 对策

索引输出尾部的 `failure classes:` 行是排障入口（完整表在随包手册里——CLI 与 MSBuild 两份，装在二进制旁的 `docs/` 目录；下面是最常见的几类）：

| 失败形态 | 对策 |
|---|---|
| varargs 形态（`cannot pass object of non-trivial type ... through variadic function`） | `--clang-compile-flags "-Wno-non-pod-varargs"`（大仓实测有效） |
| 编码乱码（GBK 等源文件的 invalid-UTF-8/乱码诊断） | MSBuild 路线已自动对齐（无编码声明的条目末尾补 `-finput-charset=<系统 ANSI 代码页>`，带 BOM 文件不受影响）；普通 cdb 路线手动处理：转 UTF-8（建议带 BOM）或自己注入 `-finput-charset=<代码页>` |
| 延迟解析残余（`msvc-permissive-unchecked-member-call` / `-ctor`） | 走 `--clang-compile-flags "-fdelayed-template-parsing"` 排障流（翻译层已自动补 `/Zc:twoPhase-`，此为射程外残余） |
| `msvc-permissive-unchecked-constexpr`（模板里 constexpr 绑 dllimport/运行期值） | **无开关，需源码修**（clang 在定义处检查类模板 static constexpr 成员，声明级规则延迟解析盖不住；HEADER_WIDGET 宏 constexpr→const 一处生效） |
| `msvc-permissive-lenient-specialization`（类模板成员类外定义缺 `template<>`） | ccg 自动改写（cl 方言，诊断驱动，不落盘）；总结行 `dialect rewrites: N TU(s) fixed via compiler fixit` 计数，一次重解析后仍失败的残余照常记本类 |
| lambda 初始化捕获形态（MSVC 绑外层实体、clang 绑捕获名） | **无开关；容忍 + info 即可**（错误恢复良好，边缺失局部化无污染；全仓 2 处） |

## 近实时刷新：ccg update

```
ccg update [<file>...] [--drain] [-o DIR] [--config <名>] [--json]
```

- **前置：db 必须已存在**（先 index）。库定位同查询侧发现规则。
- 一次性命令、无守护进程、无锁文件；**db 本身是协调器**（pending_updates 队列 + 租约）。语义：入队（短事务）→ 抢租约 → 持有者循环 **peek** 队列（不删除）→ 引擎批次 → **批后清除**：只删除"本批覆盖集内且内容哈希与批前快照一致"的条目，直到队列为空。批运行期间的新编辑（哈希变了）保留脏标记，下一批再跑——宁可多跑一次，绝不丢更新。未跟踪文件（不在 db/compile db）记 note 后直接出队。并发调用在活租约下直接退出 0；崩溃持有者（PID 已不存在）的租约会被下一次调用立即接管，超时过期同样可被偷走，零残留。
- 覆盖集 = 本次实际重解析的主 TU（含失败的尝试）∪ include 闭包传播到的既有文件。
- 头文件种子会传播到 include 闭包（改一个 .hpp 可能重析多个 TU）。
- 未知文件（不在库中）只在 stderr 记 note，退出码 0。
- 空队列 `--drain`：文本 `nothing to do (queue empty)`，JSON `{"updated":0,"queued":0}`。
- 会话收尾建议跑一次 `ccg update --drain` 把最后的事件折进去。

## MSBuild 路线（Windows 专属）

```
ccg msbuild <path.sln> [--config C] [--platform P]
            [--all] [--export-only] [--full-build] [-o DIR] [--json]
ccg msbuild --list [<path>]
```

- 走完整个 MSBuild 构建图并捕获全部 cl 调用后翻译成 compile_commands.json；**默认记录模式**——依赖调度、CustomBuild/midl 生成器真实执行（保证 TU 集合完备），但编译类工具（cl/link/lib/rc/mt/masm）全部桩化：只记录 argv、写满足时间戳依赖的桩产物，**不真编译**；MSBuild 产物重定向到配置目录下的 `msbuild-sandbox/`，逃逸进真实源码树的新增文件按 `sandbox-leak` 具名警告如实上报；生成器正常产物（MIDL 的 tlb/生成头、CustomBuild 生成物）不逐条报，收进一行 info 汇总 `out-of-sandbox generator products: N (expected)`。`<Config>-<Platform>` 即配置目录名，db 与 compile_commands.json 都住 `<layout>/<Config>-<Platform>/`（layout = `-o DIR`，默认 `<root>/.ccg`）；注册表 `.ccg/msbuild.json`（项目根，跨配置发现入口）。
- `--full-build` 才真编译（旧默认行为，金标准对账用）：每个 cl 真跑、MSBuild 编译判定如实上浮、无沙箱；记录模式与 `--full-build` 的捕获翻译后逐字节一致。
- **新鲜度语义**：每次运行都以 `MSBuild -t:Rebuild` 走完整构建图后再索引——cdb 是“运行那一刻”的快照，Rebuild 保证捕获覆盖全部 TU（普通 Build 只记录被重新调用的 cl，会漏 TU）。因此：源码内容改动可由 `ccg update`/hook 增量跟上；**新增/删除源文件、改工程属性或宏、换配置必须重跑 `ccg msbuild`**（或 `--export-only` 刷新 cdb）。索引阶段本身仍是增量的（mtime/size → sha256 → argv digest）。
- **配置名**：`<配置名>|<架构名>` 对应 `--config <配置名> --platform <架构名>`，目录名把 `|` 换成 `-`（`<配置名>-<架构名>`）。值会进文件名，含空格或 `/\:*?"<>|` 一律拒绝（退出 2）。不指定时取注册表 `default`，否则 `Debug` / `x64`。
- 查询时用 `-o <root>/.ccg --config Debug-x64` 之类显式指向该配置。
- `--export-only` 只导出 compile_commands.json（clangd/cl-tidy 可直接用）；`--list` 打印注册表。
- 注意退出码：目录里没有 .sln/.vcxproj 时 `--list` 与构建都是退出码 2（用法错误）；sln 已找到但注册表缺失是退出码 1。
- 非 Windows 平台：`--list` 可用，其余干净报 not supported。

## hook 安装：db 常新

```
ccg hook install --project   # 写本仓 .claude/settings.json（在目标仓库根目录跑）
ccg hook install --user      # 写用户主目录的 ~/.claude/settings.json
ccg hook status --project | --user
ccg hook uninstall --project | --user [--force]   # 默认 dry-run，--force 落地
```

装上后的效果：

- **PostToolUse**（Edit/Write/MultiEdit/Update 之后）：自动 `ccg update` 该文件并注入一行影响面摘要（受影响符号数、风险级、最相关消费者）。库定位走 `CCG_DB` > `.ccg/` 发现（同查询侧规则），无库时静默。
- **超时不阻塞会话**：注册条目的宿主 timeout 是 10s；处理器内部最多等 update 9s。到点**只 detach、绝不 kill**——update 在后台继续跑完（Windows 脱离 job、POSIX setsid；输出走 delete-on-close 临时文件）。此时注入 `[ccg] 已记录 <file>（索引更新在后台继续；影响面基于既有索引）`，影响面从既有 db 尽力生成；该文件的队列标记由后台批次按"覆盖集 + 内容哈希未变"规则清除，不会丢。`CCG_HOOK_UPDATE_TIMEOUT_MS` 仅供测试覆盖 9s 内部预算；PreToolUse 查找注入的受限正则路径另有运行期开关 `CCG_PRETOOLUSE_REGEX`（默认开启，详见下条）。
- **SessionStart**：会话启动注入一行 db 状态（存在/规模/索引时间）；无 db 时静默。它同时兜底清理崩溃/会话结束残留的积压结论。
- **UserPromptSubmit**：用户下一次输入前，如果刷新批次已经跑完且队列为空，把批次期间积压的编辑结论**一次性**注入（每个文件一行：符号、直接/传递消费者数、风险级、最相关消费者），总预算 1200B，超长截断为"等 N 项"；批次仍在跑时毫秒级静默返回。同一积压绝不会重复注入。
- **批次期间的快速标脏**：有活租约时 PostToolUse 不派生 `ccg update`、不算影响面，只把文件入队并写一条 journal（`<ccg.db 目录>/hook-digest/`）后立即返回；写锁被批次持有时走 `need_drain` 旁路，批次收敛后再合并入队。积压只存路径/编辑落点，结论始终基于批次完成后的最新库计算。
- **PreToolUse**（Grep / Bash / PowerShell / Search 四类查找之前，外加 Read / Glob 纯投递载体）：Grep 的 pattern 与 Search 的载荷字段直读；Bash / PowerShell 的命令文本先过微秒级触发词预筛（非查找命令零成本退场），再由解析层提取 `rg` / `Select-String` 的查找词与路径提示。四方归一后：pattern 按顶层 `|` 拆分（`rg -e a -e b` / `-Pattern a,b` 这类单请求多 pattern 是同构的 OR 形态，一并救活；分支 ≤8、fixed 字面绝不拆），逐分支剥成标识符链并过噪声词表——存活单分支走现行单链查询，命中才注入该符号的五行事实（定义坐标、同名消歧、top3 调用者/零调用者措辞、派发置信分档）；多分支存活走多词并查（合计命中 ≤5 给多词候选清单，超限只给计数披露行，1400B 预算守恒）；零存活的非噪声形态在环境变量 `CCG_PRETOOLUSE_REGEX=1`（每进程读一次并缓存；`0` 禁用；未设置或其它值回落内置默认，当前为启用；开关只闸正则路径）开启且过护栏（pattern ≤64B、禁病态量词形态、每事件一次全表全匹配扫描）时走受限正则路径。未命中、非查找命令或形态有歧义（多查找单元、多路径/通配路径、`-F`/`-SimpleMatch` 字面不是裸标识符链）一律零输出——只读不动库。**静默出口顺带机会式 digest 捎带**：零命中/弃权/超时的那次调用会尝试投递编辑积压的结论（与 PostToolUse 同款 Opportunistic flush——无活租约且队列收敛才投，绝不派生子进程、绝不等待；journal 目录不存在时一次 stat 即退场）；Read / Glob 不做意图提取不做符号查询，进处理器即走这条快速投递检查——无人值守（agent 编辑完转纯查询/总结）时末批积压靠它们投递。命中注入的出口不捎带（1400B 注入信封不与 digest 混排）。投递计数在 `ccg hook status --json` 的 `digest.preToolUseFlushes`。

安装纪律（ccg 已内建，知悉即可）：settings.json 合并是元素级的，用户既有条目、注释、格式原样保留；自有条目可识别（卸载只删自己的）；脚本路径从 ccg 可执行文件自身位置推导（发行布局 `ccg + .ccg/`），settings 里允许绝对路径。**升级已装过的 hook**（如 ccg 新版把 PreToolUse 监听面从只认 Grep 扩成 `Grep|Bash|PowerShell|Search`，再扩成 `Grep|Bash|PowerShell|Search|Read|Glob`）：重跑 `ccg hook install`（自家条目按命令认领并改写为新 matcher）**并重启 Claude Code 会话**（宿主在会话启动时快照 hooks）才生效，缺一步都还是旧监听面。**"该文件的改动是否已入库"最终以 `ccg update <file>` 或 `--drain` 的 summary 为准**；SessionStart 时队列可能有上一会话的残留，drain 一次即收敛。

## 常见坑

- **忘给 `--config`**：index 期直接用法错误退出 2；查询期多配置下报候选清单退出 1。
- **failed > 0 仍 exit 0**：索引不因个别 TU 失败而中断，先看 failure classes 再决定是否重跑。
- **模板代码报 `no matching constructor` / `call to non-static member function without an object argument`**：MSVC 与 clang 的模板查找差异——翻译层已对未声明模板模式的条目自动补 `/Zc:twoPhase-`，仍失败的按「解析旗标注入」节配置 `clang-compile-flags` 救援。
- **无 compile_commands.json 是错误**：`ccg index` 退出 2 并给出三条修复提示；`ccg update` 对没记录 db 的库退出 1。先让构建导出数据库，没有"简单兜底"可言。
- **换引擎后忘了全量**：`config set llvm-path` 的忠告是认真的；裸复跑也会自动检测并强制全量。
- **MSYS2/Git-Bash 路径转换**：在 Git-Bash 里给 ccg 传以 `/` 开头的参数会被 msys 根路径转换改写（如 `--llvm-root /opt/llvm` 被改成 Windows 根下的路径）。传 Windows 形态路径，或设置 `MSYS2_ARG_CONV_EXCL` 防御。
- **`config get --json` 的两个 update 旋钮值是字符串**（如 `"300000 (default)"`），机器消费按字符串解析。

## Checklist

- [ ] 首次建库前与用户确认解析引擎目录（一个 LLVM 前缀：clang 驱动 + libclang；不指定则用 PATH 上的 clang/clang-cl）
- [ ] `ccg stats` 确认库存在（或明确首次建库）
- [ ] 建库带 `--config <名>`；建库后 `stats`/`--json` 核对规模与 failed 数
- [ ] 引擎变更后安排 `index --full --config <名>`
- [ ] 需要 db 常新时安装 hook 并 `ccg hook status` 验证
- [ ] 会话收尾 `ccg update --drain`

## 边界

- ccg 绝不调用版本控制工具；hook 只观察编辑事件并更新自己的 db。
- `--dry-run` 不写库；`--jobs` 不改变结果只改变速度。
- 不要发明速查表之外的旗标；不确定时 `ccg help <命令>` 看权威用法。
