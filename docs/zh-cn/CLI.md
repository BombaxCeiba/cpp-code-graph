# cpp-code-graph v1 — CLI 参考（`ccg.exe`）

## 1. 用法概要与共享规则

```
ccg <command> [arguments] [options]
```

| 共享选项 | 含义 |
|---|---|
| `-o DIR` | 输出 FOLDER（文件夹）。每个配置各占其中一个子目录，各自存放自己的 `ccg.db` 及派生产物（BMI 缓存、profile）。默认：设置了 `$CCG_DB` 则用它，否则用 `./.ccg`。传入 FILE（文件）路径是一个醒目的用法错误 |
| `--config NAME` | 选择（对 `index` 则是创建）配置 `<folder>/<name>/`。`ccg index` 必填（绝不可省略）。查询侧命令在文件夹里恰好只有一个配置时可以省略——唯一的配置自动选中；有多个配置时，命令以退出码 1 退出并列出候选。`$CCG_DB` 可以指向该文件夹，也可以直接指向某个配置目录 |
| `--json` | 机器可读输出（stdout 上是合法 JSON）——所有会打印结果的命令都接受，包括 `index` 和 `stats` |
| `-v, --verbose` | 进度信息走 stderr（绝不走 stdout） |
| `--version` | 打印版本号，退出码 0 |
| `--help`、`ccg help [cmd]` | 用法说明，退出码 0 |

接受符号的命令（`query context callers callees impact trace
members`）还接受消歧参数：`--file SUBSTR`（按定义路径子串过滤）与
`--kind K`（`record function method constructor
destructor field variable enum enum_constant type_alias`）。
感知覆写家族的命令（`context callers callees impact`）另接受
`--no-merge` 以关闭覆写家族合并（§2）。

查询行的结果整形：`context callers callees impact` 接受
`--exclude-glob G`（可重复；丢弃远端符号定义在匹配路径中的结果条目
——规则见下文），`callers callees impact` 接受 `--top N`
（列表上限；一行 `... and N more` 标记截断处，报告的计数始终覆盖
找到的全部内容）。

### 退出码

| 退出码 | 含义 |
|---|---|
| 0 | 成功——包括 `truncated`（已截断）的结果（计数仍然完整） |
| 1 | 未找到 / 无路径 / 目标歧义 / 运行时错误 / `partial`（部分）结果 / `changes`：什么都没找到 |
| 2 | 用法错误（未知命令或选项、缺少操作数、非法值） |
| 3 | `check --cycles`：发现环（独立的门禁退出码；0 = 干净） |

采用的诚实契约：`--json` 载荷在适用时携带 `error`（字符串）与
`partial`/`truncated`（布尔）字段；`error` 非空或
`partial:true` 强制退出码 1；单独的 `truncated:true` 不会。列表上限
（`callers`/`callees`/`impact` 的 `--top N`，以及
`check --cycles --max`）从不削减报告的计数；`truncated` 只在
列表上限确实截断了输出的地方报 `yes`（活跃的 `--depth` 扩展前沿
是另一个独立的旗标——`deeperEdgesExist`——见 callers 一节）。

### 文档与 PATH

用法与错误文本引用 `docs/*.md` 文件，这些文件位于二进制
旁边——构建树会把它们拷到可执行文件旁，`cmake --install`
以扁平发布布局装载它们（INSTALL.md）。要让裸命令 `ccg` 在任意
目录可调用，请在二进制旁边运行 shim 安装器：Windows 上是
`ccg-install.cmd`，bash 下是 `ccg-install.sh`（INSTALL.md 第 2 节）。

### 符号名匹配（接受 `<sym>` 的命令）

匹配分三层，从左到右依次评估：

1. **精确**——实参等于符号的非限定名（`Circle`）、或其完全
   限定名（`shape::Circle`、`render::Canvas::draw`）、或其完整
   唯一键（`draw|shape|src/...|30`——高级用户的逃生通道，精确
   文本匹配）。区分大小写。
2. **限定名后缀**——实参跨 `::` 边界匹配限定名的尾部
   （`Canvas::draw` 匹配 `render::Canvas::draw`；`shape::Circle`
   匹配 `a::shape::Circle`）。区分大小写。
3. **子串**——名字或限定名的 ASCII 大小写不敏感子串（`circ`
   → `Circle`、`CircleCollider`；小写 `task` 同时找到 `Pretask`
   和 `TaskRecord`）。

三层始终全部评估，命中去重后合并（层级互斥已取消）：`ccg query`
列出每个命中并报告哪些层贡献了命中（`3 matches (tier: exact)`、
`5 matches (tier: exact + substring)`）。符号目标解析
（`context callers callees impact trace members`）先看精确
子集——名字、限定名或唯一键等于实参的命中：恰好一个 → 直接解析
（子串噪音绝不会把唯一的精确命中变成歧义报告）；多于一个 → 命令
打印候选列表（每行一条 `kind  qualified_name  file:line`；
JSON 得到一个 `candidates` 数组）并以退出码 1 退出；一个都
没有 → 全部合并命中参与歧义判定。

当第 1–3 层全部落空时，运行第 4 层**模糊建议**：大小写不敏感、
分隔符不敏感的相似度比较（剥掉 `_` 与 `::`），在 LIKE 预筛后的
候选池上进行，按实参最后一段的编辑距离排序，保留最好的 5 个
（`no symbol matching 'drawx' (did you mean: draw?)`）。第 4 层
命中只是建议——它们绝不单独解析出目标。

重载说明：v1 按名字匹配，不按形参列表（`draw(int)` 无法解析）。
`--kind` 只在重载的类别不同时才能区分它们（method 与
function）；同一文件里同类别的重载（如相邻的两个自由函数）在
v1 中无法区分——`--file` 在那里帮不上忙，因为两个候选命中同一
路径子串。感知签名的匹配是已文档化的后续事项（schema 已携带
`sig_hash`）。

### 路径过滤（`--exclude-glob G`）

`context callers callees impact` 丢弃远端符号（边的另一端，或
受影响条目自身）定义在命中 `G` 的路径中的结果条目。该过滤只
作用于结果侧——数据库里的内容不会被删除，报告的总数跟随过滤
后的视图。`G` 可重复；任一命中即排除。语法：

- `*` 匹配单个路径段内的任意字符序列（绝不跨 `/`）；
- `?` 恰好匹配一个非 `/` 字符；
- `**` 跨段边界——既可作前缀（`build/**`）也可出现在模式
  中间（`src/**/test.cpp` 同时匹配 `src/a/b/test.cpp` 与
  `src/test.cpp`）；裸 `**` 匹配一切；
- 匹配是 ASCII 大小写不敏感的（路径以正斜杠比较；Windows 上
  盘符大小写不稳定）；
- 以 `/` 开头的模式锚定到路径开头；否则隐式前置 `**/`
  （`_deps/*` 在任意深度匹配）；
- 不支持字符类（`[...]` 按字面比较）；空模式是用法错误；
  什么都没匹配到的路径只是不会被丢弃而已。

## 2. 命令

### ccg index — 构建或更新数据库

```
ccg index [<paths>...] --config <name> [--compile-commands F]
                   [--llvm-root DIR] [--clang-compile-flags F] [-o DIR]
                   [--full] [--dry-run]
                   [--paranoid] [--force] [--jobs N]
                   [--wait[=<ms>]] [--no-wait] [-v]
```

- `<paths>` — 一个或多个项目根目录（重跑时可省略：记录的
  profile 会恢复它们）。只有主文件位于某个根之下的 TU 才被索引
  （用于边准入的项目允许列表来自同一组根）。
- `--config <name>` — 必填，绝不可省略。命名配置目录
  `<folder>/<name>/`，其中存放 `ccg.db`、BMI 缓存子树与
  `profile.json`。首次成功运行记录一个 profile（根、
  compile-commands 路径、引擎身份、生效 argv）；裸命令
  `ccg index --config <name>` 重跑时从 profile 恢复全部输入，
  命令行值优先于 profile 并更新它。
- **多配置编译数据库拆分**——一个携带多个内部构建配置的
  compile_commands.json（依据所记录命令的 `.dir/<Config>/`
  签名判定；Ninja Multi-Config 等多配置生成器会记录每一个配置）
  在索引时被拆分为每个内部配置一个配置目录、各一个数据库——
  没有例外。拆分目录以内部配置命名（如 `.ccg/Debug/`、
  `.ccg/Release/`），传入的 `--config` 名适用于单配置数据库，
  每个拆分库在 meta 中记录自己的构建签名（`ccg update` 依此
  过滤重新加载的编译数据库，兄弟配置绝不混入同一个库）。离开了
  编译数据库的配置只是不再被刷新——其目录留在磁盘上（陈旧
  隔离），直到用户删除它。拆分判据是同一个 FILE 在不同签名下
  反复出现（单配置生成器把源码子路径放在 `.dir/` 之后，不得
  误认作配置）；各配置索引互不相交源码的多配置库因此能通过该
  判据——当未拆分的签名看起来仍像配置名（Debug/Release/...）
  时，运行保持单配置并在 stderr 上以一条提示说明（要拆分请从
  各自的编译数据库分别索引每个配置）。
- `--compile-commands F` — compile_commands.json 的路径。
  **编译数据库是前置条件**：C++ 没有有意义的简单回退，所以
  `ccg index` 绝不猜测索引语料（一张错误的图比缺失的图更危险）。
  省略时在每个根下搜索 `compile_commands.json`（根本身，然后
  `root/build`，然后一层子目录）；仍未找到 → 退出码 2，并给出
  点名三种修法的可操作错误（从构建导出——CMake：
  `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`；用 `ccg msbuild`
  索引 Visual Studio 解决方案；或传 `--compile-commands <file>`）。
- **模块 BMI 自构建（compile-db 路线）**——编译数据库没有按
  原样重放路由的模块单元（`.ixx/.cppm/...`）通过自构建索引：
  ccg 为引擎所处的 land（标准库阵营）发现 std 模块源码（libc++
  的 `std.cppm` 走驱动自带的 `libc++.modules.json`，libstdc++ 的
  `bits/std.cc` 走驱动或 `g++`，MSVC STL 的 `modules/std.ixx`
  走驱动 include 列表自动探测到的工具集——下限 14.44；该 land
  的 clang-cl 生产向量随 M4 波到来，在此之前以一个具名类别
  降级），按（引擎, 配置, 工具集）各构建一次 `std` +
  `std.compat`，用 `clang-scan-deps`（位于驱动旁边）对每个单元
  做 P1689 扫描，以显式 `-fmodule-file=<dep>=<pcm>` 映射（分区按
  其 `mod:part` 拼写）按拓扑序把接口 BMI DAG 构建为精简 BMI
  （REDUCED BMI），并解析为其导入传递闭包供料的每个 TU。缓存
  位于 `<folder>/<config>/bmi-self-<platform>/<engine_key>/`（即
  ccg 配置目录——创建目录时请求 NTFS 压缩，尽力而为，绝非
  正确性依赖），形式是引擎级的一份 `manifest.json` 台账，加上
  每个构建配置一个 `<config_key>/` 子目录，其中存放 `std.pcm`、
  `std.compat.pcm` 及以其纯模块名命名的接口 pcm（无指纹后缀——
  配置维度就是子目录；manifest 把每个 pcm 的来源记录为
  `"<config_key>/<module>.pcm"` 外加源内容散列、生产命令摘要、
  直接 requires、以及引擎与工具集身份）：后续运行先对每个接口
  做 stat 检查再做散列确认，复用未变化的 pcm，只在编辑点的下游
  级联重建，清除离开 DAG 的模块，在某配置不再有库条目供料时
  通过删除其整个子目录来退役该配置，并在引擎版本变化或检测到
  改造前扁平 pcm 布局时清空整个引擎目录（`--force`/`--full`
  重建一切）。std 或接口 BMI 构建失败绝不终止运行：受影响的
  TU 以具名失败类别（`std-bmi-build-failed`、
  `module-build-failed`、`module-dag-cycle`、
  `self-build-unavailable`）降级到第 1 档，仍会从 P1689 扫描的
  requires 落地其编写的导入（第 0 行的 imports 行 = 扫描来源）。
  已知边界（诚实记账，留给未来一波）：分区单元本身（`module
  m:part;` 生产者）尚不是自构建生产者——P1689 扫描确实报告
  `provides m:part (partition)`，但自构建 DAG 不在分区名下注册
  生产者节点，分区 TU 按经典单元解析（无 `-x c++-module`、无
  模块输出），导入 `:part` 的宿主 TU 报告
  `module-not-found 'm:part'`（在 gnu 方言下观察到相同表现——
  一个与方言无关的既有缺口，不是模块各波的回归）。

- **MSVC/MSBuild 模块 land（模块波 M4）**——cl 方言（`ccg msbuild`
  适配器的形态，`--driver-mode=cl` 条目）的编译数据库，其模块单元
  以 clang-cl 方言经自构建路由：`std.pcm` 由工具集自带的
  `modules/std.ixx` 生产（`-clang:-o` BMI 命名、`-imsvc DIR`
  空格形态的 include 根——冒号形态被静默丢弃并回退到最新工具集的
  头文件），P1689 扫描跑在 cl 拼写的条目上（命令内的
  `--driver-mode=cl` 旗标，`.ccg/` 内有一个 `clang-cl.exe` 垫片
  兜底），接口按 cl 模式自底向上构建，消费者以固定的
  `--target=x86_64-pc-windows-msvc` 加三条轴（`-Xclang
  -fexternc-nounwind`、`-fstack-protector-strong`、
  `-fms-compatibility`）解析。工具集先从记录的 include 目录解析，
  然后是 msbuild 管线自己的 cl 发现（`-p:VCToolsVersion` 的钉定
  会传导下去），最后是最新安装的工具集——工具集 + SDK 身份是
  pcm 缓存键的一部分，所以更换工具集会清空并重建。语言轴跟随
  记录的 `/std:` 值（下限 c++20；cl 模式不存在 `/std:c++23`，
  也绝不发出）。混合 TU（`import std` + 文本 STL include）绝不
  见到 `std.pcm`——导入行被工具集伞头文件
  `__msvc_all_public_headers.hpp` 覆盖（按规则避开了重定义冲突）；
  头文件单元导入（`import "hdr";`）降级为文本 include，仅有
  头文件单元依赖的 purview 接口再降级为经典解析（模块声明同宽
  置空）。`CCG_MSVC_GNU_VECTOR=1` 把消费者解析切换到已文档化的
  gnu 模式四旗标回退向量，作用于同一批 cl 构建的 pcm。经典 cl
  条目保持原样重放（msbuild 波的行为）并只获得供料。工具集下限
  是 14.44——更老的工具集以具名的 `std-bmi-build-failed` 类别
  降级。环境旋钮：`$CCG_FALLBACK_FLAGS`——追加到无记录授权单元
  的逐 TU 基线向量末尾的额外原始 argv 记号（`;` 分隔：
  `-isystem;<dir>;-DNAME=VALUE`），自构建生产与扫描同样适用，
  用于第三方依赖根与构建形态的宏定义；`$CCG_STD_SOURCE` /
  `$CCG_STD_COMPAT_SOURCE`——显式 std 模块接口源码，覆盖 land
  发现；`$CCG_GXX`——libstdc++ 发现通道的显式 `g++`。
- **编译数据库之上的模块 BMI 权威（db 路线）**——统一路由规则：
  一个模块的 BMI 生产命令就是它的权威命令——编译数据库为其记录
  的每个单元所记录的 argv；逐 TU 基线旗标只服务于没有记录条目的
  单元。两条路线实现它：上面的 cl 路线与此处的 db 路线。对
  clang/gcc 方言的编译数据库，其记录命令——经 `@modmap` 响应文件
  展开后——仍不带 `-fmodule-file` 供料的条目（Ninja Multi-Config
  等多配置生成器把每个配置的命令都记进 `compile_commands.json`，
  但磁盘上只有已构建配置的 `.modmap` rsp 文件与 BMI）原样重放会
  静默地 `rc=4` 硬失败，所以它们改道自构建：每个模块接口用其记录
  的 argv 加依赖闭包的 `-fmodule-file` 供料加发射接口的增项
  （`-x c++-module`、`--precompile-reduced-bmi`、`-o`）生产其
  pcm，消费者用同样的记录 argv 加其导入闭包供料解析，std 模块
  来自引擎自己的 land（route-B 发现）按记录的 `-std` 轴构建——
  这是一份有意的保真权衡，文档化为引擎一致性优先：构建自身的
  `std.pcm` 只对已构建的配置存在，异种风味或方言的 pcm 绝不被
  供料（msvc-stl-land 的引擎以具名类别为 std 降级，而不是混合
  方言）。每个构建配置跑一趟自构建（依据所记录 modmap 路径的
  `.dir/<Config>/` 签名）：各配置记录的 argv 不同，所以其 pcm
  住在各自的 `<config_key>/` 子目录里——manifest 为每个（模块,
  生产命令摘要）保留一个条目，摘要覆盖记录 argv 与编译器记号，
  于是被编辑的库命令（改了宏或旗标）会精确重建该配置的模块
  （纯文件名不变，覆写）并级联下游，而兄弟配置复用；离开了库的
  配置整体退役（其子目录被回收）。供料齐全的条目（已构建的
  配置）保持原样重放，没有编写导入的经典 TU 绝不被改道。一个
  仍到达解析的无供料带导入 TU（路由残余）以具名的
  `module-feeds-absent` 类别失败，而不是裸的 `error code 4`。

- `--llvm-root DIR` — 显式 LLVM 前缀，其中的 `clang++`/`clang`
  驱动用于逐 TU 基线旗标（DESIGN.md 第 10.6 节）与隐式 include
  发现——并且仅当数据库没有配置 `llvm-path` 时，对本次调用而言
  它也是解析引擎（已配置的 `llvm-path` 总是压过该旗标，落选的
  旗标以 stderr 上的一条提示通告）。默认：锚定在已加载引擎库上
  的驱动（其目录，然后其安装前缀），否则设置了 `$CCG_LLVM_ROOT`
  就用它，否则 PATH 上的第一个。若完全找不到驱动，`ccg index`
  以点名 `CCG_LLVM_ROOT` / `--llvm-root` / `llvm-path` 的错误
  失败（绝不是猜出来的内嵌路径）。
- `--clang-compile-flags F` — 追加到每个重放 argv 末尾的额外
  clang 旗标（感知引号的拆分，如 `--clang-compile-flags "-DFOO
  -isystem <vendor>/include"`；可重复——后出现者覆盖先出现者，
  驱动末旗标获胜）。注入只发生在重放层：compile_commands.json
  绝不被改写（经 `ccg msbuild` 重新生成编译数据库会保留配置），
  记录 argv 的 TU 与合成基线的 TU 都被覆盖，追加的旗标折入逐
  条目 argv 摘要——改动它们会让下一次运行重解析数据库。逐库
  默认值可用 `ccg config set clang-compile-flags "..."` 设置；
  优先级是命令行（本次运行）> 配置键 > 无。典型用途：补放 MSVC
  接受而记录的 clang argv 缺失的重放旗标（`$CCG_FALLBACK_FLAGS`
  逃生口只作用于回退 TU，从不触及记录 argv）。`ccg update` 与
  `ccg msbuild` 接受同一旗标。MSVC 与 clang 模板分歧的标准排障
  路径（失败类别 `msvc-permissive-*`）：设置 `ccg config set
  clang-compile-flags "-fdelayed-template-parsing"`，然后重跑
  `ccg index --full`——这是必要的，因为 clang 的延迟解析只是
  近似 MSVC 的宽松默认，残余形态会诚实失败、需要显式旗标。有
  一个残余家族完全不需要旗标：宽松特化形态
  （`msvc-permissive-lenient-specialization`）在 cl 方言条目上
  依据编译器自己的 fixit 在内存中改写（见下方补救对照表），
  这些 TU 因此自动转绿。
- **LLVM 引擎（逐库解析世界观）**——`ccg` 不链接 libclang；
  引擎（`libclang.dll` / `libclang.so`）在运行时加载，每个数据库
  一个世界观。解析顺序：数据库的 `llvm-path` 配置（`ccg config
  set llvm-path`），然后 `--llvm-root`，然后 `$CCG_LLVM_ROOT`，
  然后默认发现——PATH 上找到的 clang 驱动命名工具链，其根提供
  库（驱动旁边以及 `lib/` 之下）。已加载引擎报告的路径与版本串
  在每次索引运行时记入库中；当任一者与记录不符——不同的引擎、
  升级过的工具链、甚至同一路径而内容变了——运行自动切换为全量
  重索引并附一条通知（跨世界观没有增量捷径）。缺失或不可加载的
  引擎是一个点名各旋钮的可操作错误（退出码 1），绝不是静默回退
  到另一个引擎。
- `--full` — 就地全量重索引（对全新数据库这也是隐式模式）。
  逐文件替换使 `--full` 运行等价于重建数据库：每个被跟踪文件都
  可重写，所以该运行还能治愈此前（有缺陷或崩溃的）运行可能留下
  的行。
- `--dry-run` — 只打印计划（模式、总/脏 TU 计数、以主文件字节
  估计的重解析工作量——只计将真正重解析的 TU，绝不算跳过的——
  以及清除文件估计）而不写盘，并在毫秒级返回：该运行在任何引擎
  加载、驱动解析、模块自构建扫描或影子 PCH 规划之前短路（这些
  在大型语料上要花几十分钟）。什么都不解析——计划是对既有数据库
  的逐条目新鲜度裁定（stat mtime+size、sha256 确认、编译命令
  摘要）；无操作的库保持纯 stat 检查，被编辑过的主文件仍恰好
  显示为一个陈旧 TU。因为不解析，include 增量展开与清除文件
  估计来自记录的 include 图（本次编辑放弃的 include 会在下一次
  真实运行的清扫中浮现）。缺失的数据库绝不被创建；已有的数据库
  只被读取（打不开的数据库——schema 不匹配——按一个内存替身
  规划，即一切都算新工作）。
- `--paranoid` — 忽略 mtime 快速路径，对每个文件做散列（能抓住
  保留 mtime 的拷贝；更慢，O(bytes) 而非 O(stat)）。
- `--force` — `--full` 的旧别名，额外在数据库文件的
  `schema_version` 不匹配时重建数据库文件（§SCHEMA 4）。

- `--jobs N` — 模块 BMI 构建与 TU 解析池共用的并行工作者数，
  1–64（一个上限同时治理两个阶段；两阶段从不重叠，所以峰值并发
  就是 N）。默认：逐数据库的 `index-jobs` 配置键，否则 `auto` =
  `min(cores-2, 12)`；旗标对其所在运行获胜（`ccg update`/`ccg
  msbuild` 以相同优先级接受同一旗标）。BMI 构建中每层的工作者数
  还受层宽限制。陈旧 TU 按主文件大小最长优先交给解析工作者；
  每个工作者拥有自己的 libclang 索引。工作者数不改变结果：任何
  N 存下的行都相同。
- `--wait[=<ms>]` / `--no-wait` — 启动租约接管（**REALTIME.md**
  §4）。刚以写模式打开数据库之后，运行立即窃取更新流程使用的
  同一个 `update_claim`（活跃持有者被顶替；它在其下一次心跳时
  优雅让位——不会有第二个写者幸存）。窃取需要写锁：当并发的
  批次持有写锁时，每次重试打印一行 `waiting to take over the
  update claim (pid …, queue …)`（1/2/5/10 秒封顶的退避），上限
  30 分钟；`--wait=<ms>` 收紧上限。提交事务内每 60 秒为认领
  心跳一次；提交中被窃取的认领把整段干净回滚（`nothing
  written`）。提交之后，索引期间被钩子入队的事件由有限的追赶
  轮次在进程内收敛（三段输出：检测提示、完整未截断的文件列表、
  以及点名完成轮次与剩余事件的结果行；旋钮
  `index-drain-rounds` / `index-drain-budget-ms`，默认 8 /
  300000 毫秒）；剩余部分（轮次/时间预算、项目文件行）交给常规
  机制——一个分离的 `ccg update --drain`。`--no-wait` 保持
  快速失败行为（以退出码 1 退出而不是接管活跃持有者，并指向
  `ccg update --drain`）。`ccg msbuild` 共享这两个旗标与整个
  接管/排空尾巴。

行为：对已存在的数据库，`ccg index` 默认增量——先看构建系统
新鲜度（stat mtime+size；stat 变化时 sha256 确认；逐条目编译
命令摘要，所以 Debug 数据库之上的 Release 数据库会重解析而不是
静默跳过），然后脏集经记录的 include 图扩散：传递 include 闭包
触及变更/删除文件的任何 TU 重解析，被直接编辑的 TU 主文件重写
自己的站点行（边、include、行号），边引用了被清除符号的 TU
重新解析，失败的 TU 从共享头文件出现次数计数器中撤下自己的
观察者贡献，而当前没有 TU 能到达的行（被删源码、被弃的编译库
条目、被弃头文件）被清除。重复的编译库条目（同一主文件 + 同一
命令）共享一次解析。增量运行与对同一树状态从头重索引行等价
（SCHEMA §7；由 ctest 钉死）。失败的 TU 被记录
（`parse_status=failed`，其先前的行被清除）且绝不中止运行。
打开 `schema_version` 不匹配的数据库会被拒绝并给出建议
`ccg index --force` 的错误（项目未发布期间不维护跨版本迁移；
§SCHEMA 4）。带 `-v` 时，逐 TU 进度行携带工作者 id
（`[w2 41/660]`）且可能交错。

文本摘要走 stdout（即使有失败 TU 也以退出码 0 结束——数据库
可用；首行说明模式）：

```
$ ccg index . --compile-commands build/compile_commands.json
indexing 660 TUs under E:/src/widget (db: .ccg/main/ccg.db, mode: incremental)
  fresh: 0  stale: 12  skipped: 648  failed: 0
  symbols: 18411  edges: 50237 (calls 44891, extends 1523, overrides 1502, ...)
  includes: 15203  imports: 135
  unresolved edges dropped: 311
  elapsed: 41.3s   db size: 9.4 MB
```

`imports` 计数具名模块导入关系（schema v5）：在模块 TU 中编写、
且以其构建的 BMI 供料解析的每个 `import <name>;` 一行
（`@...modmap` 响应文件与 `CXXModules.json` BMI 映射两者都被
消费）。当有 TU 失败时，一行 `failure classes:` 汇总具名的逐 TU
类别，而不是一个含混的计数——`module-bmi-missing`（被供料的
BMI 不在磁盘上；点名模块 + pcm）、`bmi-config-mismatch`（clang
静默拒绝了某个 pcm；点名 ccg 自身记录键的差异轴——pcm 生产者
版本对已加载引擎，或消费者对接口单元的 `-std`）、
`module-not-found`、`partition-not-fed`、`two-stdlib-clash`、
`stale-cascade-crash`、`module-feeds-absent`（TU 编写了导入但其
记录命令完全不带供料——未构建的配置，或被剥除/缺失的
`.modmap`），加上自构建降级类别（`std-bmi-build-failed`、
`module-build-failed`、`module-dag-cycle`、
`self-build-unavailable`）——以及经典诊断的 `parse-failed`。
模块 TU 的解析带一个内存中的导出置空覆盖层（同宽 `export` 置空，
绝不触碰 `export module`/`export import`/`export { ... }` 块或
字面量、注释与预处理指令）：libclang 把 `export namespace`/逐
声明 `export` 包装器背后的一切藏进一个不可访问的游标，没有覆盖
层时这些文件产出零符号。该覆盖层仅用于解析——磁盘上的文件绝不
被修改——且经典（非模块）TU 绝不会见到它。

**失败类别 → 补救对照表。** 当 TU 失败时，下列类别名就是分诊
入口（`--clang-compile-flags` 机制——命令行或 `ccg config set
clang-compile-flags`——向每个重放 argv 注入旗标；MSBuild 路线
的条目已自动携带 MSBUILD.md 第 2 节第 5 步的对齐）：

| 失败形态 / 类别 | 补救 |
|---|---|
| varargs 形态（`cannot pass object of non-trivial type ... through variadic function`；经典 cl 接受它） | `--clang-compile-flags "-Wno-non-pod-varargs"`（在大型 MSVC 代码库上验证有效） |
| 源码编码乱码（GBK/Shift-JIS/... 源码上的无效 UTF-8 / 乱码诊断） | MSBuild 路线自动对齐（对未声明编码的条目追加 `-finput-charset=<system ANSI codepage>`；带 BOM 的文件不受影响）；普通编译数据库则要么把源码转成 UTF-8（推荐带 BOM），要么自行注入 `-finput-charset=<你的代码页>` |
| 延迟模板解析残余（`msvc-permissive-unchecked-member-call`、`msvc-permissive-unchecked-ctor`） | 上面的旗标注入分诊流程：经 `--clang-compile-flags` 注入 `-fdelayed-template-parsing` |
| `msvc-permissive-unchecked-constexpr`（模板内绑定到 dllimport/运行期值的 `constexpr`） | **不存在开关——修源码**（clang 在定义处检查类模板的 `static constexpr` 成员；延迟解析覆盖不了声明级规则；肇事的 `constexpr` 宏（形如 `HEADER_WIDGET`）以一行 constexpr→const 修改一次性治愈全部用点） |
| `msvc-permissive-lenient-specialization`（类外类模板成员定义缺少 `template<>` 引入符） | ccg 自动改写（仅 cl 方言，由诊断驱动、依据编译器自己的 fixit，在内存中进行——磁盘上的文件绝不触碰；摘要行报告 `dialect rewrites: N TU(s) fixed via compiler fixit`）。单次重解析后仍失败的残余出现保持该类别 |
| lambda 初始化捕获形态（MSVC 绑定外层实体，而 clang 绑定捕获项） | **无开关；容忍 + 提示**（错误恢复良好——缺失的边保持局部、无污染；初始化捕获形态，审计过的语料全仓 2 处） |

调试环境开关（内部诊断；每进程评估一次——请在启动 `ccg` 之前
设置。开着其中任何一个的运行是诊断运行，不是性能基准。Windows
注意：那边环境变量查找不区分大小写，所以不同大小写的变体——
例如全小写拼写——同样会生效，请只设置一种拼写）：

| 开关 | 效果 |
|---|---|
| `CCG_DEBUG_DIAGS=1` | 把每次解析尝试的每条错误级诊断打印到 stderr——包括影子 PCH 尝试（其诊断本会被运行内的文本回退重试替换掉）（无 DIAG 行而降级的 TU 属于 NULL-TU/加载失败类别，不是代码错误） |
| `CCG_DEBUG_ARGV=1` | 打印每次解析尝试的完整重放 argv（PCH 注入形态与剥除回退的形态各占一块） |
| `CCG_PCH_DEBUG=1` | 打印每次影子 PCH 构建的完整生产者重放命令 |
| `CCG_CRASH_DUMP_DIR=<目录>` | 仅 Windows。设置后 `ccg` 在启动最早期挂上崩溃回调：进程崩溃时把完整内存转储（full memory dump——全部线程栈、堆、句柄表与模块信息）写进该目录，文件名 `ccg-<进程号>-<年月日>-<时分秒>.dmp`；目录不存在会自动逐级创建，写失败把错误码报进 stderr 并以原崩溃退出码终止（不吞退出码；纯虚函数调用与 UCRT 非法参数两形态本无系统异常码，退出码为可辨析的应用自定义码 0xE0000002/0xE0000003）。不设置则什么都不挂、行为与从前完全一致。拦得住的崩溃形态：访问违例、栈溢出、堆破坏、未捕获的 C++ 异常、`abort`、纯虚函数调用（MSVC 上另有 UCRT 非法参数）；拦不住的（按 Windows 设计绕过一切用户态回调直达内核终止）：快速失败族——`__fastfail` 类指令与栈保护 cookie 校验失败——这些要靠系统级 WER LocalDumps 兜底（见 DESIGN.md）。POSIX 上不生效（崩溃走系统 core dump） |

`--dry-run` 改为打印计划：

```
$ ccg index . --dry-run
indexing 660 TUs under E:/src/widget (db: .ccg/main/ccg.db, mode: incremental, dry run)
  fresh: 0  stale: 12  skipped: 648  failed: 0
  plan: 12 of 660 TUs to re-parse (~318.2 KB main-file bytes), purged files: 1
  elapsed: 2.1s
```

`--json` 以对象形式发射相同字段（`mode`、`dryRun`，以及——
对干跑——用 `dirtyBytes` 代替图总量）。

### ccg update — 变更文件的实时刷新（数据库协调队列）

```
ccg update [<file>...] [--drain] [--compile-commands F] [--llvm-root DIR]
           [--clang-compile-flags F] [--jobs N]
           [-o DIR] [--config NAME] [-v] [--json]
```

一条面向编辑器钩子/代理驱动索引的一次性刷新命令——**绝没有
守护进程，也绝没有锁文件**。它把给定文件作为外部播下的脏文件
喂给普通增量引擎；静止下来的数据库等于 `ccg index` 对同一树状态
会得出的结论（信号是种子而不是 stat——一次保住了 mtime+size 的
编辑仍会重解析，因为事件说了文件变了）。

- `<file>...` — 变更的源文件（主文件或头文件；播下的头文件
  传播到 include 闭包到达它的每个 TU）。数据库与编译命令都不
  认识的文件会在 stderr 上报告，其余时候被忽略（尚无任何文件
  include 的全新头文件按构造就对图不可见）。
- **项目文件**（`.sln`/`.vcxproj`/`.vcxitems`/`.props`/
  `.targets`/`.filters`）被单独分类：它们绝不走 TU 解析路径。
  当数据库的项目根有 msbuild 注册表且该行是注册的解决方案或
  位于该根之下时，持有者先过**哈希门**（R11）：重推上次成功求值
  采集写下的刷新快照（`.ccg/msbuild/<stem>/snapshot.json`——sln、
  全部成员 vcxproj 与全部 import 闭包逐文件的 sha256，加上
  INCLUDE 哈希、MSBuild/cl 路径、VCTargetsPath 与 ccg 版本）；
  输入全等 ⇒ 事件被吸收、什么都不派生。否则持有者触发对这一个
  数据库配置的重新捕获（默认走求值通道——经
  `msbuild-refresh-debounce-ms` 防抖，默认 3000；带互斥体探测；
  失败时保留原语料并保留该行——完整协议见 **REALTIME.md**
  §6），捕获自身的增量尾巴索引真正变化的 TU。`--json` 以
  `"projectRefresh": "captured" | "skipped-unchanged" | "busy" |
  "failed" | "untracked"`
  报告结果。没有注册表时该行作为未跟踪条目出队（不堆积；非
  Windows 机器上项目事件因此绝不触发任何事）。
- `--drain` — 即使没有自己的文件也把待处理队列处理到空；
  Stop 钩子的尾巴收敛旗标。
- `--compile-commands F` — 仅本次运行覆盖数据库记录的编译
  数据库（记录文件失踪是运行时错误：刷新看不到语料旗标）。
- `--clang-compile-flags F` — 本次运行的重放层旗标注入，语义
  同 `ccg index`（可重复，追加到每个重放 argv 末尾；缺省时适用
  数据库的 `clang-compile-flags` 配置键）。
- `--jobs N` — 模块 BMI 构建与解析池的并行工作者数，1–64，
  优先级同 `ccg index`：旗标对其所在运行获胜，否则数据库的
  `index-jobs` 配置键，否则 auto。
- 数据库必须已存在（先 `ccg index`）；根、编译数据库、LLVM
  引擎钉定都读自已记录索引运行的元数据，优先级规则与
  `ccg index` 相同（引擎身份变化强制全覆盖，即世界观变化）。
  记录了"没有编译数据库"的数据库是错误（退出码 1）——update
  绝不扫描源码；请带编译数据库重跑 `ccg index`。

并发协议（逐数据库；**数据库本身就是协调者**——schema v6 表
`pending_updates` + `update_claim`）：

1. **入队** — 一个短事务把自己的规范化路径插入
   `pending_updates`（`path` 主键为同一文件的并发入队去重）。
   连接的 `busy_timeout`（`update-busy-timeout-ms`，默认
   600000）使入队能等过并发的索引批次而不是失败。
2. **认领** — 对 `update_claim` 单例的一个短 `BEGIN IMMEDIATE`
   事务。无行、租约已过期、或持有者进程被证实已死
   （`update-lease-ms`，默认 300000；持有者记号内嵌其 pid）：
   本进程成为持有者。活跃租约：立即以退出码 0 退出——持有者的
   排空循环覆盖排队的工作。
3. **持有者循环** — 重复 { 一个短 IMMEDIATE 事务：刷新租约 +
   **窥取**整个队列（一次读——不删除任何东西）；记录每个被
   跟踪路径的内容散列；在不开任何协调事务的情况下把批次送进
   普通增量引擎；然后**清除已覆盖项**——只删除未被跟踪的条目，
   或本批次以未变内容散列覆盖的条目 } 直到一次窥取为空；释放
   认领（仅自己的行）并以退出码 0 退出。批次运行期间落地的编辑
   保住其标记到下一批次（无丢失更新）。每个批次是一次增量运行
   （摘要形状与 `ccg index` 的完全一致；`--json` 同样，丢失认领
   则改报 `{"queued":N,"claimed":false}`）。

投递是至少一次且幂等的（重新播种已是最新的文件会重写相同的
行）。崩溃的持有者不留任何需要清理的东西：其租约过期，下一个
updater 自动窃取认领。由一个随后死掉的运行排队的工作坐在队列
里，直到下一个事件或一次 `--drain`——任何无守护进程队列的普通
尾巴。`ccg index` 只在其提交阶段周围加入同一协议（排队用
`--wait`/`--no-wait` 旗标，§ *ccg index*；**REALTIME.md** §4）：
批次运行时，索引运行排在持有者后面而不是相撞；索引提交阶段
持有认领时，编辑器钩子走快速路径，正如它们在更新批次后面所做
的一样。编辑器/代理钩子接线（PostToolUse 拉起 + 可选 Stop 钩子
`--drain`）与示例脚本：**REALTIME.md**。

### ccg msbuild — 索引 MSBuild 解决方案（Windows）

```
ccg msbuild <path.sln> [--config C] [--platform P]
            [--all] [--export-only] [--record] [--full-build]
            [--msbuild-args S] [--cl PATH]
            [--msbuild PATH] [-o DIR] [--compile-commands F]
            [--clang-compile-flags F] [--jobs N] [--force]
            [-v] [--json]
ccg msbuild --list [<path>]
```

从 Visual Studio 解决方案到已索引数据库的一条命令。默认通道是
**求值（evaluate）**：不跑构建——ccg 经进程内的 C# 对象宿主
（VS 自带的 `Microsoft.Build.dll`）对每个成员工程做求值，读出
最终的 `ClCompile` 条目与 metadata，自己组装 argv（典型解决
方案秒级；无沙箱、无桩、无捕获库；完整 `-imsvc` 集请在 VS
开发者提示符里跑）。`--record` 显式走包装构建捕获通道——
**测试/验证设施**：构建图执行、生成器真实运行，编译工具被换成
桩（不真正编译任何源码，桩完全不写任何东西，一次零磁盘记录
运行；MSBuild 管理的输出进沙箱，逃逸进项目树的写入报告为
`sandbox-leak` 警告，落在沙箱之外的生成器产物——midl 的
tlb/生成的头文件、CustomBuild 输出——合并成一行
`out-of-sandbox generator products: N (expected)` 信息）。两通道
的调用经同一翻译层产出 clangd 可用的 `compile_commands.json`，
再喂给普通索引
管线——每个配置一个数据库文件，每对（配置, 平台）一个配置
目录，位于 `<layout>/<Config>-<Platform>/`，存放 `ccg.db` 与其
`compile_commands.json`（layout = `-o DIR`，否则 `<root>/.ccg`），
外加项目根部一个描述该集合的微型 `.ccg/msbuild.json` 注册表
（跨配置发现的入口；注册表的 `mode` 列为
`evaluate`/`record`/`full-build`）。用 `-o <folder> --config
<stem>` 查询其中
任何一个（`ccg query add -o .ccg --config Debug-x64`）。成功的
求值采集还会写刷新快照
（`.ccg/msbuild/<stem>/snapshot.json`），供 `ccg update` 的项目
文件哈希门消费（`skipped-unchanged` 吸收）。

- `--config`/`--platform` — 要构建的（配置, 平台）对（默认：
  注册表默认值，否则 `Debug`/`x64`）；值必须路径安全（它们用来
  命名数据库文件）。
- `--all` — 输入声明的每一个 Configuration|Platform，各一个
  数据库（拒绝 `-o`、`--config`、`--platform`、
  `--compile-commands`）。
- `--export-only` — 写完 compile_commands.json 即停（打印其
  路径；`--json` → `{"config","platform","mode","compile_commands",
  "tus","failed_projects","sandbox_root","sandbox_leaks"}`——
  `failed_projects` 仅求值模式有，sandbox 字段仅记录模式有）。
- `--record` — 显式走包装构建捕获通道（见 MSBUILD.md §2b）：
  wrap 层回归、对照真值臂、对 MSBuild 实际命令行的人工取证。
  与 `--full-build` 互斥。
- `--full-build` — 真实编译：每个 cl 真的运行，
  MSBuild 的编译裁决浮现出来（`MSBuild exited N`），无沙箱。
  把它当金标准交叉校验用（翻译之后，记录模式与全构建的捕获
  逐字节一致）。
- `--msbuild-args S` — 逐字拼接的额外 MSBuild 参数；
  `--cl PATH`/`--msbuild PATH` 跳过相应的发现。
- `--clang-compile-flags F` — 重放层旗标注入，语义同
  `ccg index`（追加到每个重放 argv；生成的 compile_commands.json
  不被修改）。schema 不匹配的数据库在这里默认被重建（ccg 升级
  后无需手动 `--force`）。
- `--jobs N` — 模块 BMI 构建与解析池的并行工作者数，1–64，
  优先级同 `ccg index`（旗标 > 数据库的 `index-jobs` 配置键 >
  auto）；逐数据库默认值经 `ccg config set index-jobs` 设置。
- `--list` — 打印注册表；操作数（默认：当前目录）只用于定位
  注册表根。注册表缺失 → 错误，退出码 1。
- 退出码：0 成功（允许失败 TU，同 `index`——失败的构建仍会索引
  已捕获的内容并浮现 `MSBuild exited N`）；1 运行时（无 MSBuild、
  空捕获）；2 用法。

设计上仅限 Windows：解析、翻译与注册表是平台中立的，但抽取
需要 MSBuild——在别处 `--list` 可用，其余一切以
`not supported on this platform` 干净失败。完整管线、注册表
格式、失败形态与注意事项（tlog 只重建一次、非 ASCII TU 路径
摄取限制）：**MSBUILD.md**。

### ccg config — 逐数据库设置（引擎 + 更新协调）

```
ccg config get [key] [-o DIR] [--config NAME] [--json]
ccg config set <key> <value> [-o DIR] [--config NAME]
ccg config unset <key> [-o DIR] [--config NAME]
```

一个数据库只被恰好一个 LLVM 引擎解析——它的解析世界观。引擎
身份（解析出的库路径 + 版本串）与其产出的结果一起记录，并在
编译命令摘要与 TU 集合增量之外参与索引新鲜度判定。

- `get [key]` — 打印配置。不带键时，打印每个键外加引擎行，且
  分开打印：数据库记录的引擎（产出该语料的权威身份——
  `recorded engine`）与当前环境的解析（存储的 `llvm-path`——
  未设置时是发现链——一次全新解析会加载的库，以及引擎一旦加载
  后的版本串）。两者不同时（例如 PATH 发现现在找到 22.1.8 而
  记录的是 23.1.3），一条醒目的警告说明裸 `ccg index` 会做什么：
  加载新引擎并强制全量重索引。`--json` →
  `{"llvm-path","recordedEngine","recordedVersion","engine",
  "version","engineMismatch"}`，外加获取全部时的下面两个更新
  旋钮与 `index-jobs`；损坏的引擎被就地报告——读配置从不依赖
  可加载的引擎，从未索引过的数据库在记录行显示
  `(not indexed yet)`。数据库缺失 → 退出码 1。
- `set llvm-path <value>` — 解析所用的 LLVM 安装：要么一个
  LLVM 前缀（库直接在其下、`bin/` 下、`lib/` 下查找），要么
  libclang 库文件本身。路径必须存在（否则退出码 1）。该值以
  规范化形式存进数据库的 meta 表。同一引擎的 `unset` 等价拼写
  会被识别（解析到同一个库不记录任何变化）。
- `unset llvm-path`（别名 `clear`）— 移除该设置；解析回退到
  `$CCG_LLVM_ROOT`，然后 PATH 发现。
- `set update-lease-ms N` / `set update-busy-timeout-ms N` —
  `ccg update` 的协调旋钮（REALTIME.md）：无心跳下的认领租约
  寿命（必须超过最坏情况的单独一批；默认 300000），以及更新
  协调事务等出并发写锁的时长（默认 600000；每个 ccg 连接在
  打开时已装好 30 秒默认忙超时——SCHEMA.md 第 1 节——所以该
  旋钮只需覆盖分钟级的引擎批次）。只接受正整数（否则退出码
  1）；`unset` 恢复默认。
- `set index-jobs N` — 统一的工作者上限：`index`/`update`/
  `msbuild` 的模块 BMI 构建与 TU 解析池的并行工作者数（两阶段
  从不重叠，所以峰值并发即 N；BMI 构建还按层宽限制每层的
  工作者数）。`0` = 显式回到 auto，`1`–`64` 生效——严格整数
  语义（否则退出码 1）。默认 0 = 按核心数 auto
  （`min(cores-2, 12)`）；`--jobs N` 命令行对其所在运行获胜。
  改它绝不重解析任何东西：工作者数不改变存储的行。
- `set msbuild-refresh-debounce-ms N` — 更新持有者看到排队的
  项目文件后、拉起重新捕获之前等待的静默窗口（默认 3000；窗口
  内出现的新队列行会重置它）。IDE 对 `.vcxproj`/`.props` 的
  保存风暴折叠成一次捕获；见 **REALTIME.md** §6。
- `set clang-compile-flags "F…"` — 由 `index`/`update`/
  `msbuild` 追加到每个重放 argv 的额外 clang 旗标（空格分隔、
  感知引号）。生成的 compile_commands.json 文件绝不被修改；
  `--clang-compile-flags` 命令行对其所在运行获胜。设置/修改它
  会使下一次索引运行重解析数据库（旗标进入逐条目 argv 摘要）。

**忠告：** 修改或清除 `llvm-path` 会打印一条重建通知——该设置
对整个数据库全局生效，其中的每个结果都由先前的引擎产出。请
运行 `ccg index --full`（或裸 `ccg index` 即可：引擎身份检查
自己就会把下一次运行强制为全量）。

退出码：0 正常，1 运行时错误（路径不存在、`get`/`unset` 时
数据库缺失），2 用法（未知动作/键、缺少操作数）。

### ccg hook — 管理 Claude Code 集成

```
ccg hook install --project|--user [--json]
ccg hook uninstall --project|--user [--force] [--json]
ccg hook status --project|--user [--json]
```

在 Claude Code 的 `settings.json` 里注册或移除 ccg 的条目：一个
`PostToolUse` 钩子（匹配器 `Edit|Write|MultiEdit|Update`）在每次
编辑之后经 `ccg update` 刷新数据库，并以一行波及面摘要作答；一个
`SessionStart` 钩子以一行数据库状态作答；一个 `UserPromptSubmit`
钩子报告在刷新批次运行期间堆积起来的编辑结论；一个
`PreToolUse` 钩子（匹配器 `Grep|Bash|PowerShell|Search|Read|Glob`）
在搜索运行之前，对每一个点名了被索引符号的字符串搜索意图，回答
该符号的图事实——Grep 的 `pattern`，Bash 或 PowerShell 里的
`rg`/`Select-String` 命令（一道微秒级的触发词预筛在任何解析开销
之前丢弃非搜索命令），或 Search 工具载荷的字段。Read 与 Glob
完全不携带搜索意图：它们只是投递载体——处理器跳过意图提取与
符号查询，只运行下述的积压投递检查。PreToolUse 处理器的每一次
静默退出（零命中、一道提取闸门婉拒、一个截止期、或任何
Read/Glob 调用）都顺手通过下一个 PostToolUse 编辑所用的同一个
`hook_digest_flush(Opportunistic)` 投递钩子摘要积压——没有
journal 目录、有争用的锁、活跃的认领租约、未收敛的队列、空的
积压，全都以零代价返回，且绝不拉起子进程。这正是让批量编辑会话
的最后一批在模型转入纯查询/阅读（没有进一步编辑、没有用户提示）
之后仍能到达模型的原因；注入了符号事实的命中不搭车（1400 字节
的注入信封保持不混合，积压等待下一次静默退出）。所有应答都采用
`{hookSpecificOutput:{hookEventName, additionalContext}}` 形状，
并且在无可奉告时保持静默（零输出、退出码 0）——工作上下文中
没有数据库、非语料文件、非编辑工具、仍在运行的批次、或未点名
任何被索引符号的搜索。

**Grep 期注入（V7.1）。** PreToolUse 钩子把 Grep 模式剥成一个
裸标识符（允许 `A::b` 链；锚点、`\b`、环视守卫以及 `f\(` 之类
调用形态的尾巴被容忍），并经查询侧快速路径在符号表中做只读
查找。带顶层分支的模式（`pi|area`）在深度为零的 `|` 处拆分，
每个分支经过同一个标识符闸门与噪声词过滤（至多 8 个分支；
`-F` 的字面竖线绝不拆分）；多个 `rg -e`/`-Pattern` 值是同一
OR 形态，以同样方式合并。存活下来的分支一起加载各自的候选
（总命中上限为 5——超过上限时钩子以一行计数披露作答而不是给
列表），按符号去重，在同样 1400 字节的预算内渲染多词候选列表
并带截断披露；单一存活（或仅单命中）的分支降级为下面的精确
单链答案。带元字符但没有存活分支的模式，在
`CCG_PRETOOLUSE_REGEX` 开关允许（默认开）且形态通过护栏（模式
长度 ≤ 64、无 `(a*)*` 之类病态量词形态、每事件一次全表全匹配
扫描、扫描之前与期间的截止期探测）时，可走**受限正则路径**。
Bash 与 PowerShell 命令把抽取出的 `rg`/`Select-String` 模式
送进同一道闸门：多请求或通配路径的命令保持静默，`-F`/
`-SimpleMatch` 字面量仅当它本身是裸标识符链时通过（固定多模式
的命令同样保持静默）。**唯一**命中（恰好一个符号的裸名字，或
解析到唯一符号的限定链）注入五行有界内容：身份行（限定名、
形参列表、类别、父链）、定义坐标、同名消歧行（带坐标的前 3
个对手——"那些坐标处的命中属于其他符号"）、带各自最高频调用
点的前 3 个调用者（零调用者的符号得到明确的"以无调用者作答"
措辞，而不是防御性猜测）、以及派发行（虚站点数、已解决数、
按置信档位列出的实现候选）。**多**命中（裸名字对应几个同名
符号，一个重载家族）不猜测目标：它注入一个候选列表——按引用
数排序的可调用同名符号（前 5，`(+N more)` 尾巴）及其定义，
外加一条用限定名重新 grep（示例 = 头号候选）或路径过滤的提示。
Grep 的 `path` 输入按定义位置预筛候选，可把列表折叠成五行的
形态。两种格式都以各自的固定降级规则遵守 1400 字节预算。

Grep 期注入的运行时环境开关（钩子运行时，不是上面的索引诊断
表；每进程读取一次并缓存——钩子对每个工具事件都是新进程，所以
这至多是每事件一次解析，热路径上的重复检查是纯内存读取）：

| 开关 | 效果 |
|---|---|
| `CCG_PRETOOLUSE_REGEX=1` | 启用 PreToolUse 注入的受限正则路径：没有存活标识符分支（且并非全是噪声分支）的元字符模式，以 `std::regex` 全表全匹配扫描比对符号名表，每事件一次，由模式长度 ≤ 64、病态量词拒绝、名字长度跳过、以及扫描之前与期间的截止期探测共同守护 |
| `CCG_PRETOOLUSE_REGEX=0` | 停用正则路径（此类模式保持静默） |
| 未设置或任何其他值 | 回退到内建默认值（当前为启用——闭卷配对评测发现正则路径无害，因此默认随产品开启），在此处与 REALTIME.md 中均有文档 |

该开关只治理正则路径——分支拆分与多模式查找永远开启。Windows
注意：那边环境变量查找不区分大小写，所以不同大小写的变体
（例如全小写拼写）同样会生效——请只设置一种拼写，不要导出
大小写变体。

**快速路径。** 当一次编辑到达时，另一个 `ccg update` 正持有
活跃的认领租约，PostToolUse 钩子不拉起刷新，也不查询影响面：
它把文件入队进 `pending_updates`（主键去重）并在
`<ccg.db dir>/hook-digest/` 下写一条 journal 条目，随即返回
（远低于一秒）。若 SQLite 的单一写者正忙（批次持有写锁），入队
退化为一条 `need_drain` journal 条目，批次收敛后并入队列——它
绝不在没入队时宣称入队过。

**批次结论。** 一旦不再有活跃认领且 `pending_updates` 为空，
下一个 `UserPromptSubmit`（或下一次 PostToolUse 编辑、PreToolUse
处理器的任何一次静默退出——包括每一次 Read 与 Glob 调用——或
作为崩溃兜底的 SessionStart）认领 journal 条目，针对*已刷新的*
数据库重算每个文件的影响面（符号、直接/传递消费者、风险档、
头部消费者），并注入一条封顶 1200 字节的合并 `additionalContext`。
重复的路径、符号与渲染摘要被折叠；已注入的条目被删除且绝不
重播。认领与发送之间崩溃会损失一轮，而不是重复报告。每次经
PreToolUse 静默退出完成的投递在 journal 目录留下一个
`flushed-pretooluse-*` 标记（24 小时后老化删除）；`ccg hook
status --json` 把它们的数量报告为 `digest.preToolUseFlushes`。

- `--project` 目标是 `./.claude/settings.json`（从目标仓库根
  运行）；`--user` 目标是家目录的 `~/.claude/settings.json`。
  必须恰好给出一个。
- 编辑是逐元素且保持原有跨度的：用户条目、注释、键序、以及
  ccg 自有条目之外的格式逐字节幸存；ccg 的条目按其命令被识别
  （`.ccg/hooks/ccg-posttooluse` / `ccg-sessionstart` /
  `ccg-userpromptsubmit` / `ccg-pretooluse` 脚本名，或插件通道
  写入的 `ccg hook posttooluse` / `ccg hook sessionstart` /
  `ccg hook userpromptsubmit` / `ccg hook pretooluse` 拼写）。
  无法解析的文件被拒绝，绝不被改写。重跑 `install` 会收敛
  （自有条目被替换，而不是被复制）。
- **既有安装的升级路径** — 例如 PreToolUse 匹配器从 `Grep`
  长成 `Grep|Bash|PowerShell|Search` 之后：重跑 `ccg hook
  install`（自有条目按其命令被识别——无论带着什么匹配器——并
  被改写为当前规格），然后重启 Claude Code 会话——宿主在会话
  开始时给钩子拍快照，所以会话中途改动的条目在重新启动（或在
  `/hooks` 菜单里复核）之前不会生效。两步都必需：不重装则
  设置里留着旧匹配器，不重启则新写的匹配器没有被加载。
- 各命令引用由本可执行文件位置派生的跳板脚本——构建的
  POST_BUILD 步骤（或发布包）放在它旁边的 `.ccg` 目录；脚本
  缺失会中止安装，而不是注册一条死路径。脚本按 `$CCG_BIN`、
  然后自己旁边的发布布局、然后 PATH 定位 ccg。
- `uninstall` 默认试运行（`--force` 才实际执行），并恰好移除
  ccg 自己的命令对象；被掏空的匹配器条目、被掏空的事件数组、
  以及被掏空的 `hooks` 对象随之被修剪（零残留），邻居不动。
- 内部动作 `ccg hook posttooluse` / `ccg hook sessionstart` /
  `ccg hook userpromptsubmit` / `ccg hook pretooluse` 是那些
  脚本转发到的 stdin-JSON 入口（它们不接受范围旗标；
  `-o DIR`/`$CCG_DB`/`--config` 与处处一样选择数据库）。
  `ccg hook status --json` 额外为运维人员报告一个
  `digest: {dir, pending, claimed, needDrain}` 摘要。

退出码：0 正常（包括"无可卸载"），1 运行时错误（脚本缺失、
设置不可读/可写但被拒绝），2 用法（动作缺失/未知、范围缺失或
重复）。

### ccg query — 查找符号

```
ccg query <name> [--kind K] [--file SUBSTR] [--implements] [--json]
```

`--implements` 翻转查找方向：`<name>` 被当作接口或基类型，命令
沿 extends 边列出每一个派生类型（直接优先、传递包含，`depth`
列标记每一层；`--json` 镜像为 `interface`/`deriving` 载荷）。
这是类型继承检索——虚覆写家族是另一个轴（见 `impact` 的覆写
家族合并）。

```
$ ccg query draw
kind      qualified name                    location
method    render::Canvas::draw              src/render/canvas.hpp:42
method    render::Canvas::draw              src/render/canvas.hpp:58
function  shape::draw_all                   src/draw.cpp:12
3 matches (tier: exact)
```

`--json`：

```json
{"query":"draw","tier":"exact","matches":[
 {"uid":1041,"kind":"method","name":"draw","qualified_name":"render::Canvas::draw",
  "file":"src/render/canvas.hpp","line":42,"col":9,"parent":"render::Canvas","matchTier":"exact"},
 {"uid":1042,"kind":"method","name":"draw","qualified_name":"render::Canvas::draw",
  "file":"src/render/canvas.hpp","line":58,"col":9,"parent":"render::Canvas","matchTier":"exact"},
 {"uid":2210,"kind":"function","name":"draw_all","qualified_name":"shape::draw_all",
  "file":"src/draw.cpp","line":12,"col":6,"parent":null,"matchTier":"exact"}]}
```

所有层级零命中 → `no symbol matching 'drawx' (did you mean: draw?)`，
退出码 1（模糊建议）。精确多重命中歧义 → 候选列举，退出码 1
（§1）。层级绝不互斥：精确命中与子串命中都出现，层级行点名
每一个有贡献的层级；JSON 中每个条目携带自己的 `matchTier`
（`exact` / `qualified suffix` / `substring`），顶层 `tier` 点名
最浅的有贡献层级。

### ccg context — 符号的 360 度视图

```
ccg context <sym> [--kind K] [--file SUBSTR] [--exclude-glob G] [--no-merge] [--no-dispatch|--dispatch-fallback] [--config NAME] [-o DIR] [--json]
```

`--exclude-glob G`（可重复）丢弃远端符号定义在匹配路径中的边
（§1 路径过滤规则）；数据库不受影响。

先打印符号块（类别、限定名、定义位置、父级、旗标、模板/形参
信息），然后是按边类型分组的进入与外出边，每条被记录的边行
一行：在多个站点被到达的远端符号，每个站点行出现一次（原始
`edges` 行——把重复折叠成一条带出现次数的行是后续事项），且
每行携带该边首个使用点坐标（file:line）。

```
$ ccg context shape::Circle
symbol  record shape::Circle
        src/shapes/circle.hpp:18  parent: shape::Shape (extends)
        template  no   methods 6   fields 3

incoming
  calls        shape::draw_all                src/draw.cpp:14
  calls        shape::area_all                src/draw.cpp:27
  used by      render::Canvas::add            src/render/canvas.hpp:21
  extends from shape::Ellipse                 src/shapes/ellipse.hpp:9

outgoing
  extends      shape::Shape                   src/shapes/shape.hpp:11
  calls        shape::pi                      src/shapes/circle.hpp:31
  type_alias   shape::CircleRef               src/shapes/circle.hpp:24
```

`uses` 边是类型使用关系（见下文）。入站行打印为 `used by`。

`--json` 形状：
`{"symbol":{...},"incoming":{"calls":[...],"extends":[...]},"outgoing":{...}}`，
每个边条目为
`{uid, kind, qualified_name, file, line, occurrences, confidence}`
——`file`/`line` 是该边的使用点坐标，即文本行打印的同一来源，
不是远端符号的定义位置（两者都来自站点行；缺站点时给 `"?"`）
——外加合并行上的 `familyOrigin`（见下文）、`dispatchStatus`
（见下文）、以及默认模式下顶层的一个 `dispatch` 汇总对象。

#### 覆写家族合并（context / callers / callees / impact / trace / changes）

查询虚方法默认合并其覆写家族：该方法加上经 `overrides` 边连接
的一切（它覆写的虚函数，以及它的每一个覆写，传递到底）。每个
合并行都标记其来源——文本输出中是 `[family: ns::Class::method]`，
JSON 中是 `"familyOrigin": "ns::Class::method"`——`--no-merge`
只查询符号本身。非虚或无家族的符号不受影响。带合并家族的输出
还会列出成员（文本：一行 `override family merged`；JSON：一个
`family` 数组）。

#### 动态派发证据（默认开启）

schema v11 存储由静态类型分析推导的 `virtual_dispatch` 边：
穿过接口指针/引用的虚调用被解析为从推断出的动态类型集合可达的
具体覆写。查询默认使用该证据（精确模式，B 语义）：

- 带置信度 >= 0.7 证据（1.0 / 0.7）的调用点被归因于已证明的
  派发目标，而不是家族过近似。静态 `calls` 行是被替换，而不是
  被复制。具体查询不再拉入派发到兄弟覆写的调用者；基类/接口
  查询仍包含每一个覆写调用者，因为证据目标属于被查符号的实现
  集合；
- 无证据的调用点保持静态关系，且当它只是经家族成员到达时，
  被标记为**家族估计**；
- 正向派发边是一等调用关系（不只是过滤器），所以家族中没有
  静态 `calls` 边的目标仍能被找到；
- 多个推断出的实现显示为同一点的多个行——工具绝不挑出单个
  猜测的目标；
- 未解析的站点发出一条家族级 0.0 标记边（目标 = 静态解析出的
  被调用者）；其封顶后的候选集住在 `call_sites.candidates`，
  截断标记在 `fallback_reason` 中；
- 间接调用（函数指针/成员指针）不发出派发边，在 `call_sites`
  中以 `fallback_reason=indirect_call` 报告。

标记与机器可读状态（仅有的用户可见词表）：

| 证据 | 文本 | JSON `dispatchStatus` |
|---|---|---|
| 已证明的目标，置信度 1.0 / 0.7 | `[dispatch 1.00]` / `[dispatch 0.70]` | `"inferred"`（外加 `dispatchConfidence`） |
| 无已证明的动态证据（包括仅有 0.4 弱推断）；整个覆写家族估计 | `[dispatch unknown · family estimate]` | `"family_estimate"` |
| 直接静态关系，无派发不确定性 | 无标记 | `"none"` |

当存在任何家族估计时，文本头部追加一行自释说明：

```
family estimate: dynamic type unproven; estimated from the whole override family, may include non-target implementations
```

随后是计数：

```
dispatch: N inferred, M family estimate, K static (--no-dispatch for the static view)
```

JSON 以一个顶层对象携带同样的计数：
`"dispatch":{"mode":"precise","threshold":0.7,"inferred":N,"familyEstimate":M,"static":K}`。
只有 `inferred` 条目携带 `dispatchConfidence`；家族估计绝不暴露
数字置信度（0.0 是标记，不是测量）。

置信度取值有 `1.0`（高：`new Impl` 之类直接局部证据）、`0.7`
（中：单类型 L1/L2 传播）、`0.4`（低：多个动态候选、解析歧义）
与 `0.0`（标记：无动态证据——绝不当作证据，绝不显示为测量出的
置信度）。默认证据阈值是 **0.7**：只有 1.0 / 0.7 替换家族近似。
0.4 弱推断不点名动态目标；该站点保持家族估计。0.0 标记——
包括 0.4 的阈下行——绝不增加调用关系。

#### 派发解析词表（`call_sites`）

CLI 只呈现上面的阶梯；更细粒度的"为什么"住在 `call_sites` 表里
（`fallback_reason`、候选 JSON 中的 `candidates[].reason`），
供对数据库做程序化审计：

| 记号 | 含义 |
|---|---|
| `static_unique_override` | 无动态证据，但每个接收者静态类型都是图 final 或具体类，且覆写家族恰有一个非纯实现——仅凭静态证明即解析为 1.0（候选的 `reason`） |
| `static_narrowed` | `fallback_reason` 后缀：候选被收窄到家族 ∩（所有静态类型的后代子树之并集），以 0.4 显示仅供展示——绝不构成派发边 |
| `subtree_overflow` | 静态类型子树超过 256 个节点（或深度 16）；保留整个家族而不是有损收窄，后缀会如此说明 |
| `receiver_scope_narrowed` | 候选 `reason` 后缀：COM 根层生命周期方法（AddRef/Release/QueryInterface）的整族披露形态下，候选被接收者静态类型的 extends 后代闭包收窄——闭包之外的成员对该站点是运行时不可能目标（动态类型集被 may 扇投/族写回的假类型污染时防稀释）；全部候选出闭包时回退全候选 |
| `..._nearest_converged` | 多个动态类型，其最近覆写全部汇聚到同一实现——该站点升级到 0.7，而 `reason` 保留其基名（`conditional_merge_nearest_converged`、`direct_new_nearest_converged`、...） |
| `slot_type_truncated` | `fallback_reason` 后缀：接收者的类型集合在 32 个类型的槽位上限处被截断，动态证据可能不完整。消费须知：该标记叠加在低档多候选（0.4）站点上时，候选里的噪声占比高——类型集可能被虚族扇投/族写回的 may 并集灌入近乎全库的类型，消费端应结合 `receiver_scope_narrowed` / 静态可达面过滤或降权 |
| `candidates_truncated` | 候选列举被封顶（64 条） |

槽位截断的可观测性仅限 SQL——**没有**
`analysis_truncated_slots` 这个 meta 键（meta 表的分析观测键只带
`analysis_rounds` / `analysis_work_items` / `analysis_budget_exhausted`
与 `analysis_prop_*` 计时/计数系列）。这样统计受影响的站点：

```
SELECT COUNT(*) FROM call_sites
 WHERE fallback_reason LIKE '%slot_type_truncated%';
```

族边计数的双数字口径（meta 键）：`analysis_prop_family_pf_edges` /
`analysis_prop_family_wb_edges` 是**预解析段的通道对数**——静态面
裁剪后零投递的对也计入；实际收到族投递的槽数是
`analysis_prop_family_pf_slots` / `analysis_prop_family_wb_slots`
（终态物化段统计）。两者可差一个量级（通道在、证据不在），
估计实际投递规模时以槽数为准。

C 风格转换注记（勘误）：早期一份现场报告声称 SWIG 生成的代码
一律写 C 风格转换。被审计的快照实际写的是 `reinterpret_cast`；
CStyleCast 处理（保留操作数的证据，绝不采纳转换目标）是防御性
覆盖，不是对某个已观察形态的修复。

#### 派发 CLI 开关（context / callers / callees / impact / trace / changes）

- 默认——如上所述的精确模式。
- `--no-dispatch` — 纯静态视图。这是保守上界（整个覆写家族），
  且与派发功能上线前的默认输出逐字节一致；需要证明自己没有
  漏掉调用者时用它。
- `--dispatch-fallback` — 精确模式加阈下标记（0.4 弱推断与
  0.0 家族标记），显示为 `family_estimate`。对 `callers` /
  `callees` / `impact`，它只注释静态关系已到达的行；`context`
  额外列出阈下边。在每个命令里成员集合都不变——标记绝不增加
  或删除符号。与 `--no-dispatch` 互斥。

`trace` 把正向派发边当作一等调用跳（0.0 标记绝不参与寻路）。
`changes` 把正向派发调用者加为消费者（状态 `inferred`）；覆写
家族传播保持家族估计。残余的过近似：`changes` 仍沿 overrides
边双向传播，所以改一个具体实现时，兄弟的家族估计消费者可能
现身。

### ccg members — 记录成员列举（自有 + 继承）

```
ccg members <record> [--kind K] [--file SUBSTR] [--config NAME] [-o DIR] [--json]
```

列出记录的成员——方法、构造函数、析构函数、字段、嵌套类型——
带 `file:line`，按声明顺序（回答"这个 struct 有什么"不必重读
头文件）。直接成员在前（`own`）；沿 extends 链继承的成员随后，
归在 `inherited from <base>` 组头之下（BFS 序、防环、深度上限
8；`--json` 逐成员携带 `origin`）。一个覆写一次出现在 `own`
中、一次出现在其基之下——不同的符号，如实展示。

```
$ ccg members shape::Circle
record   shape::Circle  src/shapes/circle.hpp:18
         6 members (3 methods, 2 fields)
kind         qualified name                   location
method       shape::Circle::area              src/shapes/circle.hpp:30
...
```

计数行：`N members (M methods, F fields)`，其中 N 统计每一个
直接成员——嵌套记录、枚举、枚举常量与类型别名计入 N，但既不计
入 M 也不计入 F，所以记录声明了嵌套类型时 `N = M + F` 不成立。
`--kind`/`--file` 像每个符号命令一样收窄列举（与计数）。

退出码：对没有成员的记录给 0 与一份空列举（那是有效答案）；
实参无法解析、或解析到非记录时给 1（错误点名实际类别并指向
`--kind record`）。

`--json`：
`{"record":{...},"total":6,"methods":3,"fields":2,"members":[
{uid,kind,name,qualified_name,file,line,col,parent},...]}`。

### ccg callers / ccg callees — 调用关系（带 --depth 的传递扩展）

```
ccg callers <sym> [--depth N] [--top N] [--kind K] [--file SUBSTR] [--exclude-glob G] [--no-merge] [--no-dispatch|--dispatch-fallback] [--config NAME] [-o DIR] [--json]
ccg callees <sym> [--depth N] [--top N] ...same options...
```

`--depth N`（默认 1，最大 10）：1 = 直接调用者；2 = 调用者的
调用者；每个条目标注其 BFS 深度。输出是按深度分层的树：

`--top N`：至多列出 N 个条目（默认无限）。一行
`... and N more (raise --top to list them)` 标记截断处；
`N callers total` 行与所有其他计数始终覆盖找到的全部条目，
`truncated` 恰在这种截断处报 `yes`。

`--exclude-glob G`（可重复）：丢弃定义路径命中 `G` 的条目
（§1 路径过滤规则——`build/**`、`_deps/*`、模式中间的 `**`
都可用）；总数跟随过滤后的视图。

坐标：每个条目打印的 `file:line` 是该条目与其展开源之间边上的
第一个调用点（深度 1 时是目标本身，更深时是 `via` 符号）——
与 `context` 相同的调用点坐标词表，不是条目的定义行。当该边
没有记录站点时，诚实地回退为打印条目的定义位置；`xN` 标记该
站点的 N 次出现。

```
$ ccg callers shape::Circle::area --depth 2
shape::Circle::area  (method, src/shapes/circle.hpp:30)
  depth 1
    shape::Circle::draw           src/shapes/circle.hpp:33
    shape::area_all               src/draw.cpp:27
  depth 2
    app::main                    src/app/main.cpp:18   (via shape::area_all)
    shape::render_all            src/draw.cpp:40       (via shape::area_all)
5 callers total (truncated: no)
```

虚方法默认合并其覆写家族（行带 `[family: ...]` 标记；见
context）。`--no-merge` 恢复单符号视图。正向派发证据在该调用
点替换家族过近似；无证据的行标记为家族估计。要静态上界请用
`--no-dispatch`。

深度 > 1 时可能经多条路径重访同一符号；每个符号列在其最浅
深度处一次，带一个 `via` 来源。环终止游走（已访问集合）。
`truncated` 只在列表真的被 `--top` 截断时为真（深度展开绝不
置位）；活跃的 `--depth` 扩展前沿单独报告为
`deeper edges exist: yes
(listed entries are complete; raise --depth to expand)`
（文本）或 `deeperEdgesExist`（JSON），两个旗标都不影响退出码。
没有任何调用者的直接查询 → `no callers found — dead-code
candidate unless one of
three invisible paths applies: indirect calls through function/member
pointers (never indexed); virtual dispatch to a sibling override (target is
virtual: yes/no, use --no-dispatch for the static upper bound); address
taken at N site(s) below`（有则附取地址证据行），退出码 1。
`--json` 把它镜像为一个带三个盲区布尔量的 `no_callers_reason`
对象（`pointer_calls_invisible`、`sibling_dispatch_possible`、
`member_address_taken`）。

空结果的 `callees` 刻意保持朴素错误形态
（`ccg: error: no callees found`，退出码 1）而不是死代码注记——
被调用侧为零通常意味着查询选错了符号、或函数体在未索引的文件
里，那是查询问题而不是答案；`callers` 侧为零保持普通（退出码
1）的答案，因为死代码本身是一个发现。

取地址注解（`callers` / `callees` / `impact`）：当被查符号
（或其合并覆写家族的成员）被取过地址——`&Class::method`、
`&free_fn`——输出以一条证据行收尾：

```
address-taken: &ns::sym appears at src/wire.cpp:57 (+2 more sites); calls through function or member pointers are not visible
```

这解释了此类使用为何从不显示为调用者：证据行只证明地址在某处
被取走；它绝不会被变成调用边。JSON 以
`"addressTaken":[{"file":...,"line":...},...]` 携带站点（为空时
省略）。

### ccg impact — 波及面

```
ccg impact <sym> [--direction up|down] [--depth N] [--top N] [--exclude-glob G] [--no-merge] [--no-dispatch|--dispatch-fallback] [--config NAME] [-o DIR] [--json]
```

- `--direction up`（默认）：谁依赖这个符号——传递调用者、扩展
  它的类型、声明类型引用它的符号（`uses` 边）。`down`：这个
  符号依赖谁——传递被调用者、基类、被使用的记录（记录的向下
  走还包括其字段使用的记录：布局依赖）。
- `--depth N` 默认 3，最大 10。
- `--top N`：每个深度桶至多 N 个条目（默认无限）；`... and N
  more` 行标记每一次截断，而 `impacted` / `direct` /
  `transitive` 继续统计找到的一切；恰在某桶被截断时
  `truncated` 为 `yes`。
- `--exclude-glob G`（可重复）：丢弃定义路径命中 `G` 的受影响
  条目（§1 路径过滤规则）；受影响/受影响子计数跟随过滤后的
  视图。
- `--no-merge`：只查询符号本身；否则虚方法报告整个覆写家族的
  波及面（行带标记，见 context）。
- 风险分由精确派发结果计算；家族估计绝不抬高档位。
  `--no-dispatch` 改算静态上界分。
- 坐标：每个列出的条目打印其链内单跳边的第一个调用点（与
  `callers`/`callees`/`context` 相同的调用点词表），无记录站点
  时回退到条目的定义行。取地址注解（上文）在这里同样适用。

```
$ ccg impact shape::Circle::area --direction up --depth 3 --top 2
target   method shape::Circle::area  src/shapes/circle.hpp:30
risk     HIGH
score    47 (3*direct 2 + transitive 35 + fan-in 6)
impacted 37 (direct: 2, at depth 2: 9, at depth 3: 26)
  depth 1
    method   shape::Circle::draw      src/shapes/circle.hpp:33
    function shape::area_all          src/draw.cpp:27
  depth 2
    function shape::render_all        src/draw.cpp:40
    function app::main                src/app/main.cpp:18
    ... and 7 more at depth 2 (raise --top to list them)
  depth 3
    ... and 26 more at depth 3 (raise --top to list them)
truncated: yes
```

#### USES 边（类型使用）

每个被声明类型引用的项目记录都发出一条 `uses` 边：字段的类型、
函数的形参类型与返回类型、以及函数体内的局部变量与命名空间
作用域变量——经规范化之后。规范化剥掉 cv/引用/指针/数组包装
（`const Widget&`、`Widget*`、`Widget[4]` 都引用 `Widget`），
把 typedef 解析到其底层记录，把模板特化折叠到主模板的节点上
（`Box<int>` 引用 `Box`），递归进模板实参（`std::vector<Widget>`
引用 `Widget`），沿成员指针走到其类类型（`int Widget::*` 引用
`Widget`），并穿过结果 + 形参走函数类型
（`std::function<Gadget()>` 引用 `Gadget`；回调形参
`std::function<void(const Widget&)>` 引用 `Widget`）。来自索引
根之外的记录——标准库、系统头文件——绝不创建 USES 边（它们
自身的引用被静默丢弃；只有其模板实参内的项目记录会浮现）。
不完整的项目类型（前向声明、定义在另一个 TU）按名字延迟解析。
当多个声明在同一行引用同一记录（一个形参与返回类型）时，站点
折叠成一条边、`occurrences` 统计重复次数；`edges.attrs` 的位
0-2 记录站点风味（字段 / 形参 / 返回 / 局部 / 变量）。

这正是让记录级 impact 变得真实的东西：`ccg impact <record>`
把 USES 引用的字段与函数列为直接影响，而 v1 原来只遍历
`extends`。

#### 风险模型

```
score = 3 * direct + transitive + fan_in
  direct     = depth-1 impacted count
  transitive = impacted - direct          (depth 2+)
  fan_in     = distinct inbound-edge sources (calls / extends / uses)
               summed over the directly impacted symbols
```

一个直接消费者的权重是远处传递消费者的三倍；扇入捕捉经高连通
消费者的波及放大。档位：

| 风险 | 规则 |
|---|---|
| UNKNOWN | direction `up` 且已解析直接影响为零——没有边不等于安全 |
| LOW | score ≤ 8 |
| MEDIUM | 9 – 30 |
| HIGH | 31 – 120 |
| CRITICAL | score > 120 |

档位划分是确定性的：同一数据库状态对同一符号总给出相同的分数
与档位。

`--json`：
`{"target":{...},"direction":"up","depth":3,"impactedCount":37,"direct":2,"transitive":35,"fanIn":6,"riskScore":47,"byDepthCounts":[2,9,26],"byDepth":[{"depth":1,"symbols":[...]},...],"risk":"HIGH","truncated":false,"deeperEdgesExist":true}`
（合并了覆写家族时另加 `"family":[...]`）。

### ccg trace — 两符号间最短调用路径

```
ccg trace <a> <b> [--kind K] [--file SUBSTR] [--no-dispatch|--dispatch-fallback] [--json]
```

在 `calls` 边加正向 `virtual_dispatch` 边上做 BFS（方向 a→b）。
已证明的派发跳标注为 `--dispatch-->`；阈下（0.4/0.0）标记绝不
参与寻路。`--no-dispatch` 只跟静态调用（派发功能上线前的行为）。
打印跳链或报告不可达。

```
$ ccg trace app::main shape::pi
found: app::main -> shape::pi  (3 hops)
  app::main            src/app/main.cpp:18
    --calls--> shape::render_all      src/draw.cpp:40
    --calls--> shape::Circle::draw    src/shapes/circle.hpp:33
    --calls--> shape::pi              src/shapes/math.hpp:7
```

不可达：`no path from app::main to shape::pi within 10 hops`，
退出码 1。任一端点未解析 → 错误 + 退出码 1。消歧参数作用于
两个端点：与某端点自身类别/定义文件不匹配的 `--kind`/`--file`
会使该端点未解析（`no symbol matching ...`），所以当两个端点
类别或文件不同时，请改用更长的限定名来消歧。`--json`：
`{"from":{...},"to":{...},"found":true,"length":3,"hops":[{...}],"truncated":false}`。

### ccg changes — 你所改文件的受影响消费者

```
ccg changes [<file>...] [--from-file LIST] [--depth N] [--no-dispatch|--dispatch-fallback] [--config NAME] [-o DIR] [--json]
```

ccg 与版本控制系统无关：它绝不调用 git 或任何其他版本控制
工具。你找到变更文件（git diff、IDE、什么都行）并把列表喂
进来；ccg 把它映射到图上。

- `<file>` — 变更文件路径，绝对路径或相对工作目录的路径。
  `-` 作为操作数表示从 stdin 读列表。
- `--from-file LIST` — 从 LIST 每行读一个路径（`-` = stdin）；
  空行跳过，完全加引号的行（`"a b.hpp"`）去掉引号，尾部
  `\r`（CRLF 管道）被剥除。路径是与 `<file>` 的并集，规范化
  后去重。
- stdin — 没有 `<file>` 也没有 `--from-file`、且 stdin 被管道
  接入时（`git diff --name-only | ccg changes`），从 stdin 读
  列表。交互式调用（stdin 是真实控制台）反而是用法错误，而不
  是挂起。注意 NUL 设备在 `_isatty` 下算控制台；ccg 用
  `GetConsoleMode` 探测，所以 `ccg changes < NUL` 读到空列表。
- `--depth N` — 上游传播深度 1–10，默认 3。
- `--no-dispatch` — 只静态传播（保守上界）。`--dispatch-fallback`
  出于对称被接受；0.0 标记绝不增加消费者。

**路径处理。** 每条喂入的路径按与索引器完全相同的方式规范化
（相对工作目录取绝对、词法规范化、反斜杠 → 正斜杠、盘符
小写），然后解析到一条 `files` 行：精确文本，然后大小写不
敏感的全路径（被删文件保留其库内拼写——磁盘上没有可据以
规范化的字母大小写），然后唯一的大小写不敏感路径后缀匹配
（喂入相对另一工作目录的列表时用得上）。歧义或未匹配的路径
在输出中报告为 `unknown` / `"status":"unknown"`——绝不静默
丢弃。

**映射与传播。** 每个解析到的文件映射到定义在其中的每一个
符号（v1 是文件粒度、保守正确；符号属于其定义文件，所以类外
方法算在其 `.cpp` 下）。被删文件保留其行与符号在库里，直到
下一次 `ccg index` 运行清除它们——对着重索引前的库跑
`changes` 以捕捉删除。消费者沿 `calls`（调用者）、`extends`
（派生类型）、`uses`（其声明类型引用了变更记录的字段、形参、
返回、局部、变量）与 `overrides` 双向向上游传播——变更虚方法
的覆写家族是直接消费者。正向 `virtual_dispatch` 调用者是一等
消费者（`dispatchStatus: "inferred"`）；0.0 家族标记绝不增加
消费者。覆写家族传播保持家族估计（`dispatchStatus:
"family_estimate"`），所以改一个具体覆写仍可能列出兄弟的
家族估计消费者。消费者按所有种子上的最浅深度列出；定义在
变更文件中的符号是变更本身，绝不作为消费者出现。

**风险。** 逐消费者：`score = (depth 1 ? 3 : 1) + fan_in`，
其中 fan_in 是该消费者在 calls/extends/uses 上的不同入站来源
数；档位用与 impact 相同的阶梯（LOW ≤ 8 < MEDIUM ≤ 30 <
HIGH ≤ 120 < CRITICAL）。汇总风险用 impact 的公式
`3*direct + transitive + fan_in`（扇入在直接消费者上求和）；
没有直接消费者时为 UNKNOWN。

**退出码。** 0 = 找到消费者；1 = 什么都没找到（没有已知文件
或没有消费者——两种情况下载荷仍以文本与 JSON 打印，让未知
路径表可见）；2 = 用法错误。

```
$ ccg changes src/shapes/circle.hpp src/gone.cpp
changes  2 files fed (known 1, deleted 1, unknown 0)  depth 3
  src/shapes/circle.hpp                      present  3 symbols
  src/gone.cpp                               deleted  2 symbols
consumers 6 (direct 4, transitive 2)
risk     MEDIUM
score    16 (3*direct 4 + transitive 2 + fan-in 2)
  depth 1
    field       render::Canvas::circles_     src/render/canvas.hpp:20  risk LOW
    method      render::Canvas::total_area   src/render/canvas.cpp:13  risk LOW
    ...
  depth 2
    function   app::area_all                 src/app/main.cpp:9   (via render::Canvas::total_area)  risk LOW
truncated: no
deeper edges exist: yes (listed entries are complete; raise --depth to expand)
```

`truncated` 对这些游走保持 false（没有列表上限——找到的一切
都列出）；`deeper edges exist: yes` / JSON 的
`deeperEdgesExist:true` 表示 `--depth` 用尽时游走仍在扩展
（下一层可能加、也可能不加新消费者）。`--json`：
`{"depth":3,"files":[{"input","path","status","symbols"}],"seeds":N,"consumers":[{"uid","kind","qualified_name","file","line","depth","viaSeed","fanIn","risk"}],"direct","transitive","fanIn","riskScore","risk","truncated","deeperEdgesExist"}`
未匹配的输入带 `path:null` 与 `"status":"unknown"`。

### ccg check --cycles — include 图环检测

```
ccg check --cycles [--max N] [--config NAME] [-o DIR] [--json]
```

从记录的 `includes` 表构建包含图并报告每一个环簇：大于一个
文件的强连通分量，或包含自身的单个文件。每个簇列出其成员
文件与内部 include 边，所以两文件环打印两个方向，自包含的头
文件打印其自环。菱形（共享底层、无回边）不是环。

目标解析是增量引擎规则集的严格无歧义子集（§index——引擎把
多匹配的 include 扇出到每个候选，这对新鲜度是对的、对门禁是
错的）：带引号的相对目标先对 include 者的目录解析（对带引号
的 include 当前目录获胜，恰与编译器的搜索顺序一致），否则以
完整的 include 拼写做大小写不敏感的路径后缀匹配——只在恰好
一个被索引文件带它时接受。拼写匹配多个文件（比如 `logger.h`
折叠到无关的 `*/Logger.h` 文件）或零个文件的 include 不贡献
环边——一条伪造的边会以任何源码编辑都修不掉的幻影环现身——
而是被计数：无匹配计 `unresolvedIncludes`，多匹配计
`ambiguousIncludes`（N > 0 时文本输出打印一行
`ambiguous includes N`）。

- `--max N` — 列出的环组数，1–1000，默认 20。`*Total`
  计数器始终覆盖找到的每一个组；只有列举被封顶。
- 退出码：**0 = 无环**，**3 = 发现环**（可直接当 CI 门禁用），
  1 = 运行时错误（数据库缺失），2 = 用法错误（不带
  `--cycles` 的 `ccg check`——今天唯一的模式）。

```
$ ccg check --cycles
include cycles   1 group (2 files, 2 edges)
cycle 1 (2 files)
  e:/src/lib/a.hpp -> e:/src/lib/b.hpp
  e:/src/lib/b.hpp -> e:/src/lib/a.hpp
truncated: no (1 of 1 groups shown)

$ ccg check --cycles
include cycles   none (48 files scanned)
```

`--json`：
`{"cycles":true,"groupsTotal":1,"filesInCycles":2,"edgesInCycles":2,"filesScanned":48,"unresolvedIncludes":0,"ambiguousIncludes":0,"truncated":false,"groups":[{"files":["..."],"edges":[{"from":"...","to":"..."}]}]}`
（`unresolvedIncludes` 统计严格不匹配任何文件行的 include
目标；`ambiguousIncludes` 统计匹配多个的目标）。

### ccg graph — 调用图导出（dot / JSON）

```
ccg graph (--symbol <sym> | --modules) [--depth N] [--min-weight N]
          [--kind K] [--file SUBSTR] [--format dot|json] [--output FILE]
          [--config NAME] [-o DIR] [--json]
```

面向人类（Graphviz dot，默认）与代理（JSON）导出调用图。两个
互斥模式：

- `--symbol <sym>` — 从入口符号沿 `calls` 边（它调用什么）
  做调用图扩展，BFS 深度由 `--depth N` 封顶（默认 2，1-10；
  `--depth 0` = 无限，BFS 直到扩展前沿为空；2 是可读性的
  甜点位，3 在仓库规模上已经爆炸）。闭包保留两端都在其内的
  每一条 `calls` 边，所以交叉边幸存——导出的是真实的扩展，
  不是生成树。`--kind`/`--file` 像每个符号命令一样收窄入口
  解析。
- `--modules` — 模块级聚合。一个模块是定义文件的词干
  （`cli.parse.ixx` → `cli.parse`）；每个不同的符号级 `calls`
  对计一条边权；同文件调用绝不出现。`--min-weight N`（默认
  1 = 全保留；仓库规模上 3 读起来舒服）丢弃弱的聚合边——节点
  缩到保留边的端点，但 JSON 的 `edgesTotal` 始终报告未过滤的
  计数。`--file SUBSTR` 限定哪些文件参与（例如 `--file src/`
  只留项目自己的模块、把依赖缓存排除在外）。

```
$ ccg graph --symbol cg::cli::dispatch --depth 2
digraph ccg_call_graph {
  rankdir=LR;
  graph [label="call closure of cg::cli::dispatch at depth 2 (source: calls edges)"];
  node [shape=box, style="rounded,filled", fillcolor="#e8f0fe"];
  edge [color="#5f6368"];
  "cg::cli::dispatch" [fillcolor="#ffd166"];
  "cg::cli::top_usage";
  "cg::cli::usage_for";
  "cg::cli::command_of";
  "cg::cli::detail::run_query";
  ...
  "cg::cli::dispatch" -> "cg::cli::detail::run_query";
  ...
}
```

- `--format F`：`dot`（默认）或 `json`；共享的 `--json` 旗标
  蕴含 json（查询家族对齐）。
- `--output FILE` 把载荷写入文件而不是 stdout；stdout 保持
  干净，一行提示走 stderr。节点与边标签做 dot 转义（引号/
  反斜杠；UTF-8 原样通过，所以非 ASCII 标识符是安全的）。

`--json`（`--symbol`）：
`{"target":{...},"depth":2,"nodeCount":109,"edgeCount":225,"nodes":[{"uid":..,"kind":"function","qualified_name":"...","file":"...","line":12,"depth":1},...],"edges":[{"from":"...","to":"...","fromUid":7,"toUid":9,"occurrences":2},...],"truncated":false,"deeperEdgesExist":true}`。
`--json`（`--modules`）：
`{"moduleCount":29,"edgeCount":49,"edgesTotal":108,"minWeight":3,"modules":["cli.commands",...],"edges":[{"from":"cli.commands","to":"cli.render","weight":22},...]}`。

退出码跟随家族，外加一条导出特有的注记：0 成功——没有
被调用者的入口仍导出单节点图（导出是忠实快照，不是查找）；
1 未知/歧义符号、数据库缺失、或 `--output` 不可写；2 用法
（无模式、双模式、`--modules` 带 `--depth`、`--symbol` 带
`--min-weight`、非法 `--format`）。

### ccg stats — 数据库摘要

```
ccg stats [--json]
```

```
$ ccg stats
db               .ccg/main/ccg.db (schema 1, app 0.1.0)
indexed at       2026-10-11T09:41:02Z   root E:/src/widget
files            898 total: 874 ok, 0 partial, 22 failed, 2 pending
symbols          18411
                   method 6102   function 5988   field 2511   record 1531
                   constructor 902  destructor 601  enum 401  enum_constant 288
                   variable 201  type_alias 86
edges            50237
                   calls 44891  extends 1523  overrides 1502  type_alias 1502
                   friend_of 780  specializes 139  uses 8541
includes         15203 (angled 11022, quoted 4181)
imports          135
strings          33902 interned (~1.9 MB)
db size          9.4 MB   wal 0.2 MB
```

`--json` 镜像相同字段
（`{"db":{...},"files":{...},"symbols":{"total":N,"byKind":{...}},"edges":{...},"includes":{...},"strings":{...}}`）。
对不存在/打不开的数据库做 Stats → 错误，退出码 1。

## 3. 错误输出（统一）

文本模式：退出码 1 时在 stderr 上输出 `ccg: error: <message>`；
用法错误在 stderr 打印该命令的用法块并以退出码 2 退出。JSON
模式（`--json`）：错误作为 `{"error":"<message>","partial":false}`
走 stdout（歧义符号目标另加 `"candidates":[...]`），让调用方
能从同一条流解析一切；退出码不变。

## 4. 后续事项（明确不在 v1）

`search`/`query` 概念排序（BM25/FTS）、`serve`/`mcp`、`watch`、
感知签名的符号匹配、多根注册表、import 环检测（等模块支持落地
之后；`check --cycles` 届时在 import 图上长出 `--imports` 模式）。
不要在 v1 里构建这些；schema 与 CLI 形状为每一个都留了位置。
