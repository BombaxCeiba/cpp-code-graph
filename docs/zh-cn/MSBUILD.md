# MSBUILD.md — `ccg msbuild`：为 Visual Studio / MSBuild 解决方案建立索引

`ccg msbuild` 用一条命令把一个 `.sln` 变成已建立索引的 ccg 数据库：
它推导出每个翻译单元的编译命令，把它们写进一份
`compile_commands.json`，再送入普通的 `ccg index` 管线。该特性在设计上
仅限 Windows（见 Cross-platform 一节）。

通道共三态，按旗标分派：

- **求值模式（默认）**：完全不跑构建。ccg 通过进程内的 C# 对象宿主
  （`ccg-cshost51.dll` 垫片）加载 VS 自带的 `Microsoft.Build.dll`，
  对每个成员工程做*求值*——与 MSBuild 自己构建前做的求值完全同源
  ——读出最终的 `ClCompile` 条目及其 metadata（37 字段：定义、
  include 目录、语言标准、PCH 设置、模块产物……），再由 ccg 侧
  按这些 metadata *组装* argv（遵循 MSBuild CL task 自己的开关
  顺序）。典型解决方案秒级完成（设计锚点：59 工程 / 1655 TU 的
  解决方案 4.5 秒），不产生任何构建产物、没有沙箱、没有桩。
  见 2a 节。
- **记录模式（`--record`，测试/验证设施）**：完整的 MSBuild 构建图
  真实执行（依赖调度、`CustomBuild` 与 `midl` 生成器都会真正运行
  ——正是这一点保证了 TU 集合的完整），但编译类工具被替换为桩
  ——`cl`、`link`、`lib`、`rc`、`mt`、`bscmake`、`xdcmake`
  会记录自己完整的 argv（RSP 已展开）且什么也不写（一次零磁盘的
  记录运行：桩产物没有任何消费者，而且写出去会覆盖坐落在
  硬编码路径上的真实 PDB/lib/obj/PCH 文件）。记录模式服务于
  wrap 捕获层的回归测试，以及对照真值臂的生产（同一夹具两通道
  各跑一遍、比对两份编译数据库——记录臂是 MSBuild *实际发出*
  的命令，求值臂是 ccg *推导*出的命令）。见 2b 节。
- **`--full-build`**：真实编译（引入记录模式之前的历史默认）——
  每个 cl 都真跑；需要真实编译的裁决时用作金标准交叉核对。
  见 2b 节。

## 1. 每个配置一个数据库文件（模型）

一个 MSBuild `Configuration|Platform` = 一份 `compile_commands.json` = 一个
ccg 配置目录。`Debug|x64` 成为词干 `Debug-x64`（`|` 在
Windows 文件名中非法），而该词干本身就是配置目录的
名字（集中式布局；`-o DIR` 覆盖布局文件夹，注册表仍锚定在
项目根）：

```
<project-root>/                root = directory of the .sln
  .ccg/                        layout folder (default; -o DIR overrides)
    msbuild.json               registry (cross-config discovery, section 4)
    Debug-x64/                 one configuration directory per pair
      ccg.db                   the configuration's database
      compile_commands.json    clangd/cl-tidy usable as-is
      bmi-self-.../            the configuration's BMI cache subtree
      capture.db               SQLite capture table (raw cl invocations;
                               record mode only)
    Release-x64/...
    msbuild/Debug-x64/         transient extraction area (record mode: stub
                               tools, the CCG_CLWRAP_LOG anchor path;
                               evaluate mode: the refresh snapshot)
    msbuild/capture-<stem>.lock  per-config capture mutex (all channels)
```

为什么按配置分数据库、而不是加一列配置：同一个项目各配置的
图有约 90-100% 完全相同，只有 `#ifdef` 门控的定义不同。共享的
db 会让每个公共符号对 `query context callers callees impact trace`
都变成歧义的多重匹配（tier-1 歧义规则会打印候选并以退出码 1 结束），
把各配置中相同的调用点悄悄合并，还会让一次 Release 导入
骑在过期的 Debug 旗标上。分开的文件让每个 db
在每个配置下
都平凡且可证明地正确——而且可以独立删除或 VACUUM。本特性
不改动 schema 与引擎。

每个 db 通过自己的 `meta` 表自描述（`project_root`、
`compile_db_path`）；下文的注册表只是列表与默认值上的便利设施，
从不承担查询的关键路径。`ccg query add -o
<root>/.ccg --config Debug-x64` 可以直接作用于任意一个按配置的
db（恰好只含一个配置的布局文件夹会自动选中它，依据共享的
发现规则）。

## 2. 管线（一次运行做什么）

第 1-3 步（输入、MSBuild 与逐项目 cl 的解析）为全部通道共享；通道
分派发生在第 4 步——求值（默认）以工程求值 + ccg 侧组装替换掉包装
构建（见 2a 节），`--record`/`--full-build` 则按历史形态跑包装的
`-t:Rebuild`（见 2b 节）。第 5-7 步（翻译、索引、注册表）共享：
两通道的调用走**同一**翻译层——这正是两份编译数据库可直接逐位比对
的原因。

1. **解析输入**：只接受 `.sln` 文件。`.vcxproj` 或目录
   操作数会被一条可操作的用法错误拒绝——请传入解决方案
   文件；单项目输入暂不支持（退出码 2）。
2. **解析 MSBuild**：显式 `--msbuild PATH` 或 `$CCG_MSBUILD_EXE`，
   否则 `vswhere.exe -latest -products * -requires
   Microsoft.Component.MSBuild -find MSBuild/**/Bin/MSBuild.exe`
   （vswhere 在 `Program Files (x86)` 下查找，另有 PATH 回退）。
   找不到 → 退出码 1 并附覆盖提示。
3. **逐项目解析真实的 cl.exe**：显式 `--cl PATH`，否则用
   `MSBuild <input> -getProperty:VCToolsInstallDir -nologo
   -p:Configuration=C -p:Platform=P` 的结果与
   `bin/HostX86/<target>/cl.exe` 拼接（x64/Win32→x86/ARM64 映射）。
   该查询中必须给出配置对：`VCToolsVersion` /
   `PlatformToolset` 的固定值存放在按配置的 PropertyGroup 里，
   缺了 `-p:Configuration`/`-p:Platform` 时所有 Condition 都
   不命中，解析会悄悄回退到默认工具集（一个固定在 14.44 的
   项目被解析到 14.51——被上报的 "wrong cl.exe
   version" 问题的根因）。`.sln` 会直接拒绝
   `-getProperty`，因此 ccg 枚举成员 `.vcxproj` 项目，并对每个
   成员运行同一个配置感知的查询（各成员的 `ActiveCfg` 取自
   `ProjectConfigurationPlatforms`），产出按项目的 cl 映射；
   vswhere 的"最高工具集胜出"回退是每个项目的最后手段，
   verbose 与注册表会报告解析来源。额外的
   `--msbuild-args` 跟在配置属性之后随行，因此显式的 `/p:`
   仍能覆盖。
4. **抽取**——通道分派点。求值模式（默认）在本步执行 2a 节的
   求值会话 + 组装器；本步其余文字描述构建通道
   （`--record`/`--full-build`，见 2b 节）：运行包装后的
   `-t:Rebuild`，子进程环境携带 `CCG_CLWRAP_LOG=<capture
   log>`、`CCG_CLWRAP_REAL_CL=<主 cl>`，以及——对多项目
   解决方案——`CCG_CLWRAP_CL`，一张 `<project dir>	<cl>`
   前缀映射表，包装器在每一次调用时都会查询它，按 cwd（最长
   前缀）挑选 cl，让混合工具集的解决方案用各自项目的
   编译器编译（或在记录模式下记录）每一个 TU。
   `CCG_CLWRAP_LOG` 的值只是触发契约与推导
   锚点：捕获到的数据本身落在
   `<layout>/<C>-<P>/capture.db`（一张 SQLite `capture` 表，WAL +
   busy-timeout，每个被包装的调用一行，含 `{argv, cwd,
   INCLUDE, real_cl}`——宽字符命令行、unicode 精确、`@rsp`
   已展开；错误行则携带 `message`）。默认使用 `-t:Rebuild`，
   以便捕获覆盖每一个 TU（普通 Build 只记录
   被重新调用的 cl）。
5. **翻译**：产出 `compile_commands.json`（arguments
   形态）——两通道共用同一层（记录：捕获日志；求值：组装出的
   调用）。cl 模式下旗标逐字保留：条目以
   `clang-cl --driver-mode=cl` 开头，后跟记录到的 cl 旗标，只做
   那些已被证明必要的编辑——`/I /external:I /FI` 的值
   相对于记录到的工作目录绝对化，丢弃 `/Fo /Fd /Fp /Yc
   /Yu` 以及两个会硬失败的拼写 `/Gf` `/u-`，批量的
   源文件拆成每个 TU 一条，非 C/C++ 的调用
   （rc/midl/...）被过滤掉。单独一条 `/D` 定义会被拼接成
   一个记号（`"/D", "NDEBUG"` → `/DNDEBUG`）：clang 的响应文件按
   空白切分，两个记号会被读成两个参数。两处
   语义等价的追加（每一处都体现在生成的 cdb 中；已
   声明该选项的条目保持原样）：捕获的 cl argv 未声明
   模板模式（`/permissive-`、`/Zc:twoPhase`、
   `/Zc:twoPhase-`；大小写不敏感）的条目在尾部追加
   `/Zc:twoPhase-`——它是 MSVC 默认延迟模板解析在
   clang-cl 的官方拼写（`-fdelayed-template-parsing` 的
   正式别名）；未声明源编码（无 `/utf-8`、无 `/source-charset`）的条目
   追加 `-finput-charset=<system ANSI codepage>`（运行时探测——Windows
   `GetACP` 映射为字符集名，例如 CP936 对应 GBK；从不
   硬编码），因为 cl 按 ANSI 代码页读取无 BOM 的源文件，
   而 clang 默认 UTF-8。带 BOM 的文件不受
   该旗标影响（由 BOM 决定；由 `tests/clang.test.ixx` 中
   GBK/UTF-8 × BOM/无 BOM 四象限夹具钉死）。
   捕获到的 `INCLUDE` 环境会被
   **完整物化**：每一个非空条目（来自 `<IncludePath>` 属性 /
   VC++ Include Directories 的项目目录、MSVC 工具集、
   `VC/Auxiliary/VS`、Windows SDK 与 NETFXSDK）都按捕获顺序
   以 `-imsvc DIR` 对的形式追加在
   所有记录到的旗标之后，并插入 `/X`。`/X` 使 argv 成为系统头文件的
   唯一来源：clang-cl 停止自动发现工具集，并忽略索引
   shell 的 `INCLUDE`。按 TU 捕获的 `INCLUDE` 工具集
   目录（`.../VC/Tools/MSVC/<ver>/...`）给出
   `-fms-compatibility-version=19.<minor>.<build>`，`INCLUDE`
   中没有工具集目录时回退到该
   调用的 `real_cl` 路径。这正是让混合工具集 `.sln` 逐 TU
   正确的机制：被包装的 `real_cl` 对整个构建是单值（最高
   工具集），只有每个项目自己的 `INCLUDE` 暴露其真实头文件。若原始 cl
   命令已带 `/X`（IgnoreStandardIncludePath；`/x`、`-X`
   与 `-x` 是 cl 的同一选项），则 cl 本身就忽略了 `INCLUDE`，
   因此这些目录不再加回；拼写会规范化为 `/X`，
   因为 clang-cl 只认识这一拼写。
   加载器丢弃 `clang-cl` 记号，其余部分按原生命令重放。
6. **索引**：走与 `ccg index` 完全相同的代码路径（同一存储、同一
   增量引擎、同一摘要输出），项目根作为索引根，
   `<layout>/<C>-<P>/ccg.db` 作为 db。`--export-only`
   在第 5 步之后停止。
7. **更新注册表**（见第 4 节）。`--all` 对输入声明的每个
   `Configuration|Platform` 重复第 2-6 步。

### 2a. 求值模式（默认）

默认通道从不运行 MSBuild 的构建图。真正运行的是 MSBuild 在任何
target 执行之前做的*求值*，由 ccg 进程内驱动：

- **引擎宿主**：求值引擎是 VS 自带的 `Microsoft.Build.dll`
  （.NET Framework 4.7.2 程序集），经 ccg 自带的 C# 对象宿主
  （`ccg-cshost51.dll`——同时服务 Select-String 快路径的同一垫片；
  装载在 CLR 4 线上，Windows 10 1903+ 的机器都以 .NET Framework
  4.8 在席承载）在进程内加载。ccg 为每个配置建一个
  `MsbuildEval` 会话对象，经它逐成员工程拉取事件——没有逐工程
  子进程、不编译、没有桩、没有沙箱、没有捕获库。程序集目录是
  常规发现链（第 2 步）找到的 `MSBuild.exe` 所在目录；
  `VCTargetsPath` 从 VS 安装根推导（`MSBuild/Microsoft/VC/` 下
  同时含两个守卫 props 的最高 `vNNN` 目录，存在性验证、尾反斜杠）。
- **过滤**：`.sln` 里 `Build.0` 参与位为否的工程整体跳过（某解决
  方案配置不构建某工程，则该工程无条目——正确行为而非缺口）；
  `ExcludedFromBuild` 为真的 `ClCompile` 条目逐条跳过。
- **组装器**：每个剩余条目组装成一条调用——argv[0] 是该工程的
  cl（第 3 步的逐项目解析），旗标由条目的求值终值 metadata
  （37 字段）按 MSBuild CL task 自己的开关顺序合成：
  `PreprocessorDefinitions` → 保序的 `/D` 串、
  `AdditionalIncludeDirectories` → 保序的 `/I` 串、
  `LanguageStandard` → `/std:`、`PrecompiledHeader` 三件 →
  `/Yc`/`/Yu` + `/Fp`、`ModuleOutputFile` → `/ifcOutput <值>`
  （带值成对形态，名单单点见 `cg.util.msvcswitch`）、
  `ObjectFileName` → `/Fo`、`AdditionalOptions` 原样透传，等等。
  未知/未映射的 metadata 字段只**披露**不代言（verbose 的
  `compose: field X unmapped, skipped`）——组装器绝不发明 CL task
  不会发出的开关。`.ixx` 接口单元带 `/interface`（显式
  `CompileAs` 模块，或 `Default` + `ModulesSupported` + 扩展名）；
  每个模块单元单趟单条——记录通道的第二趟（扫描趟）在本通道
  结构性不存在。
- **INCLUDE**：求值通道直接读 ccg 进程自己的 `INCLUDE` 做
  `-imsvc` 物化。VS 开发者提示符（或导出的 `INCLUDE`）给完整
  系统目录集；INCLUDE 为空时语料照常组装但缺 `-imsvc` 面
  （运行会如实说明，索引侧按 per-TU 失败呈现——记录通道在
  这里不同：其包装器捕获的是 MSBuild 子进程内 VS 初始化过的
  INCLUDE）。要完整保真，请在 VS 开发者提示符里跑 ccg。
- **暖机前置条件**：求值看得见工程*声明*的每个 ClCompile 条目
  ——包括作为显式工程条目的生成模块单元——但文件要在盘上索引
  才能解析。冷检出（生成器从未跑过）会提前披露
  （`evaluated N TUs, M sources missing on disk — build once so
  generators produce them`）；先构建一次再索引。未声明的构建期
  生成 TU（CustomBuild 步骤产出但无人列出的 `.cpp`）按构造不在
  求值语料内——记录通道看得见它们（其生成器真跑）；这也是
  `--record` 的文档化用途之一。
- **失败分类，绝不静默**：工程级求值错误只失败该工程（其余照常，
  失败工程逐行披露并在 `--json` 里以 `failed_projects` 计数）；
  缺 import（MSB4019）降级为忽略缺失 import 的求值并给
  `evaluated with missing imports ignored: <工程>` 注意行；通道级
  失败——垫片/CLR 不可用、求值错误码、看门狗超时
  （`CCG_EVAL_TIMEOUT_MS`，默认 300 秒——锚点是 59 工程 4.5 秒，
  66 倍余量）、或全工程失败——中止刷新并保留既有语料（不写
  cdb、库一行不动、注册表不更新、退出码 1）。任何失败都**绝不
  自动换通道**：生产路径只有求值一条道，`--record` 是用户的显式
  动作。
- **刷新快照（R11 哈希门）**：每次成功求值采集的尾部写
  `<root>/.ccg/msbuild/<stem>/snapshot.json`——产出这份语料的
  输入身份：`.sln`、全部成员 `.vcxproj`、全部工程 import 闭包
  （求值免费产出）逐文件的 sha256，加上 INCLUDE 哈希、MSBuild/cl
  路径、推导出的 VCTargetsPath、ccg 版本。`ccg update` 的项目
  文件联动在重采前读它：输入全等 ⇒ 事件被吸收
  （`"projectRefresh": "skipped-unchanged"`，不派生采集）；任何
  不等 ⇒ 走真实刷新。快照只在成功后写——失败保旧同步保快照。

### 2b. 记录模式（--record）与 --full-build

记录模式是显式 `--record` 通道——测试/验证设施。它复用上文第 4
步完全相同的抽取管线，只改变被包装工具在记录日志之后的行为：

- 桩工具家族：ccg.exe 被硬链接（跨卷则复制）到
  `<root>/.ccg/msbuild/<C>-<P>/stub-tools/`，并冠以各工具名——
  `cl.exe`、`clang-cl.exe`（ClangCL 工具集）、`link.exe`、`lib.exe`、
  `rc.exe`、`mt.exe`、`ml64.exe`、`ml.exe`、`armasm.exe`、`marmasm.exe`、
  `bscmake.exe`、`xdcmake.exe`。MSBuild 通过
  `ToolPath` 属性指向它们（`-p:CLToolPath=…` 等——刻意不
  固定 `CLToolExe`，让工具集自己的默认名
  `cl.exe` 或 `clang-cl.exe` 得以保留），且桩目录被前置到
  子进程 `PATH`（按裸名解析的工具会落在桩上）。当前
  MSVC 上有一个已知缺口：MASM 目标按绝对
  工具集路径解析 `ml64.exe`，PATH 前置拦不住它，因此
  `ml64` 会真跑——它的 `.obj` 是 MSBuild 管理的产物，
  与生成器的输出一样落在沙箱里（无害；真实的
  `.asm` 从不触碰项目树）。每个桩记录 argv 且
  什么都不写（零磁盘契约）：记录运行中桩产物没有
  消费者（link/lib/mt 也是桩；`midl` 与生成器是真实的、
  与它们无关），而且写出去会覆盖坐落在硬编码路径上的真实 PDB/lib/obj/PCH
  文件（曾观察到用户下一次真实构建出现 LNK1201/C1041）。需要
  这些产物存在的下游 MSBuild 步骤（部署
  复制、时间戳检查）转而以显眼的方式失败——
  该失败正是记录模式的预期行为（见下方
  post-build 说明），而不是该拿占位文件去
  掩盖的东西。
- `midl.exe` 与 CustomBuild/CustomBuildStep/pre-/post-build 事件真跑。
  midl 生成的头文件/`.c` 文件是编译输入——
  把它们换成桩会破坏解析——而生成器正是保证
  TU 集合完整的关键（设计期枚举看不见那些只在
  构建期间才物化的 TU）。
- **沙箱**：`-p:IntDir`/`-p:OutDir` 把所有 MSBuild 管理的产物
  重定向到 `<layout>/<C>-<P>/msbuild-sandbox/{int,out}/`。真实
  项目树在运行前做快照、运行后做扫描；沙箱外（以及
  `.ccg` 外）出现的任何新文件都会以具名的 `sandbox-leak`
  警告报告到 stderr 和 `--json`（`sandbox_leaks`
  列表）——从不悄悄丢弃。仅仅被原地
  覆盖的文件（post-build 复制到一个已存在的文件——没有
  新文件出现）也算泄漏。落在沙箱外的生成器产物是
  预期的，不算泄漏：midl 真跑（它生成的头文件是
  编译输入），CustomBuild 生成器把声明的输出写进
  树中——CustomBuild 声明的、带
  源文件扩展名的输出列为注意事项（它们是索引输入），其余的（声明为
  非源文件的输出、midl 的 `.tlb` / `_i.c` / `_p.c` / `dlldata.c` /
  `.idl` 旁边的生成头文件）合并为一行
  信息 `out-of-sandbox generator products: N (expected)`（JSON：
  `generator_products`）。真正意外的写入保持
  逐文件的泄漏警告。在树比对之上，一只金丝雀（canary）对从
  捕获 argv 中解析出的每个输出路径（全部五族——
  `/Fd`、`/PDB`、lib 的 `/OUT`、`/Fo`、`/Fp`）做交叉核对：位于
  沙箱与 `.ccg` 之外、相对运行前快照是新的或被覆盖的
  路径（解决方案根之外的路径退回
  为运行窗口内修改时间的测试）以同样方式报告。在零磁盘
  契约下金丝雀永远不该触发——它存在的意义是一旦
  零写入被破坏就当场报出。硬编码逃逸
  （AdditionalOptions 里字面的
  `/Fo<projectdir>/hardobj/`、`Link` 的 `OutputFile` 在
  `$(OutDir)` 之外、CustomBuild 步骤写进
  源码树）正是它要抓的东西。项目自己的
  `<OutDir>`/`<IntDir>` 属性按 MSBuild 的优先级败给
  命令行的 `/p:` 覆盖——这正是预期的沙箱
  行为，而当 `Link OutputFile` 与之矛盾时 MSBuild 自己会给出警告
  （MSB8012）。
- **post-build 步骤失败被归类，而不是显得致命**：当
  MSBuild 以 MSB3073/exec 类错误退出 1（VCEnd/PostBuildEvent
  复制或校验步骤）时，运行会作为注意事项说明——记录模式下
  预期如此：桩不写产物，消费产物的步骤自然找不到
  可复制或可校验的东西；捕获到的编译命令仍会被
  翻译并索引。退出码语义不变。那些吵闹的错误
  行本身会从控制台 tee 中被过滤掉
  （MSB3073 / `exited with code` 行，先缓冲并计数）并
  汇总为一行——`post-build/exec failure lines on empty
  record-mode products suppressed: N (expected in record mode)`——
  于是大型解决方案不再滚动刷出成千上万条预期中的失败；
  全文仍被累积，上面的归类逻辑仍然
  能看到它。`--full-build` 原样 tee 全部内容。为此不注入
  任何 MSBuild 属性。
- **非 exec 类失败中止刷新并保留上一次的已索引内容（corpus）（记录模式）**：在
  记录模式下，任何其他非零 MSBuild 退出
  （项目 XML 畸形、缺少 import、MSBuild 自己崩溃）都意味着
  求值/编排层根本没有正常运行——
  桩不写产物，因此记录模式的构建不存在合法的
  构建期失败——捕获不可信。ccg 不写
  新的 cdb、不碰 db、不更新该配置的
  注册表条目，打印
  `project refresh aborted; database keeps the previous corpus`，
  并以退出码 1 结束。db 永远停留在最后一个完整良好的状态——
  绝不会出现只刷新到一半的语料。这也是
  `ccg update` 项目文件联动所依赖的
  契约（见 REALTIME.md）：
  一次失败的触发式重捕获会保留排队行，下一次
  编辑重试它。`--full-build` 对其全部失败保持历史上的诚实
  契约（包装器先记录再转发，因此即使真实编译失败也留下完整的
  捕获：仍会被翻译并索引）。
- **同一配置的捕获互斥**：
  `<root>/.ccg/msbuild/capture-<stem>.lock`（pid + 时间戳，
  原子创建）在某个配置的整个捕获/翻译/索引/
  注册表窗口期间被持有。同一配置的第二次捕获
  会显式失败（`another capture is running ... retry when it finishes`，
  退出码 1）；持有者死亡或崩溃留下的锁会被自动回收
  （pid 存活探测 + 时间戳过期判定）。`ccg update`
  联动在派生捕获之前只探测（只读）这把锁——锁本身归捕获
  所有。
- **工具集发现在注册表中缓存**：一次未显式给出
  `--msbuild`/`--cl`/`--msbuild-args` 的捕获，会复用注册表记录的
  MSBuild/cl 路径（前提是它们仍存在于磁盘上）（省去逐项目的
  `-getProperty` 秒数）；任何未命中（路径消失、注册表
  条目缺失、存在显式覆盖）都会重新运行完整发现。
  verbose 会打印 `toolchain discovery reused from the registry`。
- 记录模式无法检测编译错误——没有任何 cl 真正运行。诚实
  契约转移到加载器（解析失败以 `failed:` 呈现，与 `ccg index`
  完全一致）。需要真实编译的裁决时请使用
  `--full-build`。

`--full-build` 与历史行为逐字节一致：只有 `CLToolExe`/
`CLToolPath` 指向 ccg.exe，每一次 cl 调用都被转发给真实
编译器（通过同一个按项目的前缀映射选择——混合工具集
解决方案现在用各自的 cl 编译每个项目），没有沙箱、没有泄漏
扫描。`--full-build` 的代价是一次真实的 `-t:Rebuild`。

### 退出码

| 退出码 | 含义 |
|---|---|
| 0 | 成功——允许存在失败的 TU（与 `ccg index` 一致）；db 可用 |
| 1 | 运行时错误——找不到 MSBuild/cl、捕获为空、注册表损坏、非 exec 构建失败、求值通道失败（输入不可推导 / 看门狗超时 / 全工程失败——刷新中止，保留上一次已索引内容）、另一捕获持有锁 |
| 2 | 用法错误——未知选项、缺失/非法的 `--platform`、`--all` 与 `-o/--config/--compile-commands` 组合、`--list` 与构建选项组合、`--record` 与 `--full-build` 组合、找不到项目 |

### 跨平台（设计上仅限 Windows）

MSBuild 特性在设计上仅限 Windows。翻译器、注册表
格式与 CLI 解析是平台中立的，到处都能编译；所有
Windows API（vswhere 与注册表访问）都住在 `src/msbuild/` 的
`_WIN32` 守卫之后（派生子进程与取命令行经平台单元调用），非
Windows 路径返回不支持错误。在非
Windows 机器上，`ccg msbuild --list`（纯注册表读取）可用，
而每个会抽取的调用都会干净地失败并报
`ccg msbuild: not supported on this platform`（退出码 1）——绝不会是编译
错误，也不会是悄悄的空操作。

## 3. 用法

```
ccg msbuild <path.sln> [--config C] [--platform P]
            [--all] [--export-only] [--record] [--full-build]
            [--msbuild-args "extra /p:..."]
            [--cl PATH] [--msbuild PATH] [-o PATH] [--compile-commands F]
            [--force] [-v] [--json]
ccg msbuild --list [<path>]
```

- `--config C` / `--platform P` —— 要构建哪个配置对。默认：注册表的
  `default` 条目，其次 `Debug` / `x64`。取值会成为文件名的
  一部分，因此空白字符与 `/\:*?"<>|` 被拒绝（退出码 2）。
- `--all` —— 解决方案声明的每个 `Configuration|Platform`（`.sln` 的
  SolutionConfigurationPlatforms），各建一个 db，顺序构建。不能与
  `--config`、`--platform`、`-o` 或 `--compile-commands` 组合
  （它们无法点名 N 个文件）。
- `--export-only` —— 写完 `compile_commands.json` 后停止；打印
  路径（文本）或 `{"config","platform","mode","compile_commands",
  "tus","failed_projects","sandbox_root","sandbox_leaks"}`（JSON；
  `failed_projects` 字段仅求值模式存在，沙箱字段仅
  记录模式存在）。该文件可直接供 clangd 或 cl-tidy 使用。
- `--record` —— 显式走包装构建捕获通道（见 2b 节）：测试/验证
  设施（wrap 层回归、对照真值臂、对 MSBuild 实际命令行的人工
  取证）。与 `--full-build` 互斥。
- `--full-build` —— 真实编译（见 2b 节）：
  每个 cl 都真跑（MSBuild 的真实编译裁决会呈现出来）。
  求值是默认；`--record` 与 `--full-build` 是两条显式构建通道。
- `--msbuild-args S` —— 额外的 MSBuild 参数，逐字拼接在
  命令行末尾（你的引号原样保留）。
- `--cl PATH` / `--msbuild PATH` —— 跳过对应的发现。
- `--clang-compile-flags F` —— 索引时追加到每条重放 argv 的
  额外 clang 旗标（可重复；生成的 compile_commands.json 不会
  被修改——见 CLI.md 的 `ccg index`）；该旗标缺省时，按数据库的
  `clang-compile-flags` 配置键生效。
- `-o DIR` —— 输出 FOLDER（布局文件夹；配置目录
  `<Config>-<Platform>/` 连同其中的 `ccg.db` 都在其中）。
- `--force` —— 在 schema 版本不匹配时重建 db 的显式拼写；
  msbuild 路线现在默认就会重建（升级
  不再需要手动 `--force`）。
- `--json` —— 机器可读输出；配合 `--all` 时每个配置一个
  摘要对象，一行一个，按构建顺序。
- `--list` —— 打印注册表（见第 4 节）；操作数（默认：
  当前目录）只用于定位注册表根。注册表缺失 →
  退出码 1（首次运行提示），绝不是一张空表。

带上 `-v` 时，每个配置在构建之前会把自己的解析计划行（db/cdb/log
路径、MSBuild 与各项目 cl 路径及其解析来源）打印到
stderr。

## 4. 注册表（`<root>/.ccg/msbuild.json`）

只由 `ccg msbuild` 写入；可手工编辑；缺失即首次运行
行为。路径在可能时按项目根的相对路径存储。

```json
{
  "version": 1,
  "generator": "msbuild",
  "default": "Debug-x64",
  "configs": [
    {
      "name": "Debug-x64",
      "configuration": "Debug",
      "platform": "x64",
      "solution": "DemoSln.sln",
      "db": ".ccg/Debug-x64/ccg.db",
      "compile_commands": ".ccg/Debug-x64/compile_commands.json",
      "msbuild": "C:/.../MSBuild.exe",
      "cl": "C:/.../HostX86/x64/cl.exe",
      "cl_resolution": "project-property",
      "cl_projects": [ {"project": "projA",
                        "cl": "C:/.../14.51.../cl.exe",
                        "resolution": "project-property",
                        "dir": "C:/src/DemoSln/projA",
                        "active_cfg": "Debug|x64",
                        "in_build": true},
                       {"project": "projB",
                        "cl": "C:/.../14.44.../cl.exe",
                        "resolution": "project-property",
                        "dir": "C:/src/DemoSln/projB",
                        "active_cfg": "Debug|Win32",
                        "in_build": false} ],
      "mode": "evaluate",
      "extracted_at": "2026-10-12T12:00:00Z",
      "tus": 5, "failed": 1
    }
  ]
}
```

一次运行会替换（或追加）自己配置的条目并保留其余
条目；`default` 指向最近一次运行中仍列在表里的第一个
配置。`cl` 是主项目的编译器；
`cl_resolution` 说明它是怎么找到的（`override` | `project-property` |
`vswhere-fallback`——回退从未咨询过项目的固定值）；
`cl_projects` 为 `.sln` 输入携带按项目的映射
（混合工具集解决方案逐成员不同；每个条目的 `resolution`
使用与 `cl_resolution` 相同的词表，因此按项目的诚实记录
在重启后仍然保留）；每个条目还披露 `dir`（工程目录，
绝对正斜杠路径）、`active_cfg`（成员在该解决方案配置下
映射到的 ActiveCfg，如 `Debug|x64`）与 `in_build`
（Build.0 参与位——`false` 表示该配置下解决方案加载它
但不编它，与提取时过滤成员用的是同一份 sln 解析事实）。
三键只在确定知道时写出：更老的条目没有它们（读取按
未知处理，绝不当作 `false`），`.vcxproj` 输入没有
ActiveCfg/Build.0 概念，也不携带它们。`mode` 是 `evaluate`（默认通道）、
`record` 或 `full-build`（没有该字段的更老条目按其历史
通道读取）。所有新字段都是增量式的：更老的注册表原样
解析。损坏的注册表是显式错误，从不
悄悄忽略。不带 `--config` 的 `ccg msbuild` 通过条目
表（configuration + platform 字段）解析 `default`，
最后退回为在最后一个 `-` 处切分词干。

## 5. 失败模式

| 失败 | 行为 |
|---|---|
| 找不到 MSBuild / vswhere | `MSBuild not found (Visual Studio + C++ workload required; pass --msbuild PATH or set CCG_MSBUILD_EXE)`，退出码 1 |
| cl 解析失败 | 错误信息列出尝试过什么（`-getProperty:VCToolsInstallDir` + HostX86 拼接、vswhere find），并附 `--cl` 覆盖提示，退出码 1 |
| `.vcxproj` / 目录输入 | 可操作的用法错误，请求传入解决方案文件（单项目输入暂不支持），退出码 2 |
| 求值输入不可推导（求值） | 指引性错误（`cannot derive the VS installation root ...` / 无 VC targets 目录并列出候选）+ 失败保旧，退出码 1 |
| 求值通道失败（求值） | 垫片/CLR 不可用（E1）、通道错误码或坏事件行、看门狗超时（`CCG_EVAL_TIMEOUT_MS`，默认 300 秒）、或全工程失败（E3）→ `project refresh aborted; database keeps the previous corpus`，什么都不写（cdb/库/注册表/快照），退出码 1——绝不静默换通道 |
| 部分成员工程求值失败（E4） | 失败工程逐行披露（`eval project failure: <名> (<错误>)`）、计入 `--json` 的 `failed_projects`，成功工程的命令照常组装并索引（退出码 0） |
| 缺 import（求值，E5） | 降级为 `IgnoreMissingImports` 求值（`evaluated with missing imports ignored: <工程>` 注意行），全量语料照常组装 |
| 构建失败（编译错误，`--full-build`） | 仍翻译捕获日志——包装器在转发之前记录每一次调用，因此 cl 运行失败的 TU 也被捕获——索引能解析的部分，退出码 0，并在 stderr 上呈现 `MSBuild exited N for <config>` |
| 记录模式编译错误 | 一个都检测不到——没有 cl 运行；加载器把无法解析的 TU 呈现为 `failed:`（退出码 0，与 `ccg index` 一致） |
| sandbox-leak（记录模式） | 项目树中沙箱外的每个新文件或被覆盖文件都是一条具名 `ccg msbuild: sandbox-leak: <path>` 警告（stderr + `--json` 的 `sandbox_leaks`）；生成器产物（CustomBuild 声明的输出、midl 的 tlb/生成的头文件）是预期的——源文件类输出列为注意事项，其余合并为一行 `out-of-sandbox generator products: N (expected)` 信息（JSON `generator_products`）；捕获 argv 中沙箱/`.ccg` 之外的输出路径以同样方式做金丝雀核对（零磁盘契约下理论上永不触发）；运行仍以退出码 0 结束——泄漏报告本身就是产物 |
| 0 次 cl 调用的日志（构建通道） | 硬错误，完整点名捕获 db：`no compiler invocations captured for <config> (capture db: <full path>; log: <CCG_CLWRAP_LOG value>) — was the project already up to date? (-t:Rebuild is used by default; check --msbuild-args)`，退出码 1；求值侧的对应错误点名成员数（`no ClCompile items composed for <config> ...`） |
| 捕获日志错误行 | 无法展开 `@rsp` 或派生 cl 的被包装调用会在 `capture.db` 写入一行错误（kind=`error`、`message` 列）；运行把每一条报告到 stderr，并索引已捕获的部分 |
| 项目设置了 CLToolExe/CLToolPath | 该次调用中，运行注入的 `/p:` 全局胜出；ccg 在项目文件中看到该属性时会打印注意事项 |
| 已有按配置 db 上 schema_version 不匹配 | 默认重建（一条注意事项点名旧版本）；`--force` 仍是显式拼写 |

### 求值通道错误码（`rc`）

上面的"求值通道失败（求值）"一行中止时打印 `evaluation failed
for <config>: <error>`，其中 `<error>` 可能携带裸数字——
`Begin failed rc=-13 payload=...`、`NextEvent failed rc=-14 payload=...`、
`MsbuildEval session open failed, rc=-30 detail=...`。这些
`rc` 值来自承载求值会话的 C# 对象宿主通道（CLR 4 线上的
`ccg-cshost51.dll`）：`0` 表示成功，正数是句柄 token，每个负数都是
下表中的一个码。`-1`..`-20` 由垫片自身回传；`-30`/`-31` 是宿主侧
补充（引擎线整体装不起来，或请求根本装不进通道）——区间隔离让
归因一目了然。本表为全量清单；同一条消息还会携带诊断文本
（`payload=` / `detail=`），点名具体原因。

| rc | 常量名 | 含义 | 典型触发场景 |
|---|---|---|---|
| `-1` | `ErrChannel` | 通道异常（共享内存/结构体桥） | 传输本身失败——每次调用临时建立的共享内存映射建不出或映不进，或进程内 CLR 调用在垫片代码运行之前就失败；资源压力所致，与载荷无关 |
| `-2` | `ErrPayload` | 命令载荷非法（坏 JSON、缺实参、长度越界） | ccg 发出的 JSON 解析失败或缺必需的键——实际成因几乎总是不同版本的 ccg 可执行文件与垫片 dll 混装在同一布局里 |
| `-3` | `ErrUnknownClass` | `ObjCreate` 类名未知 | 构造请求点名的类该垫片不承载——同样是版本混装 |
| `-4` | `ErrConstruct` | 会话构造抛异常（异常文本随同一条消息回传） | `MsbuildEval` 建不起来——典型是解析到的 MSBuild 目录里没有 VS 自带的 `Microsoft.Build` 程序集 |
| `-5` | `ErrTableFull` | 垫片句柄表满 | 防御性兜底码：句柄表以含代次的 token 为键，同槽多代可共存，1024 只是槽号位宽而非并发对象数上限；仅当全部 1024 个槽位的代次计数都被推进到溢出/回绕出正 token 的区间（每槽数百万次登记-拆除循环之后）才可能触发，正常运行不可达——见到此码即垫片侧句柄表状态异常，而非负载 |
| `-6` | `ErrAbi` | 结构体桥 ABI 不匹配（尺寸/版本判据） | 仅 7 线（Select-String 面）会产生；求值通道钉在 51 线，绝不会报出此码 |
| `-10` | `ErrBadHandle` | 句柄值形态非法（非数字/超出 int 范围） | 协议误用或版本混装 |
| `-11` | `ErrStaleHandle` | 句柄已失效（已拆除，或从未存在） | 会话拆除（引用计数归零——旧 token 永久失效、绝不复用）之后又发起调用，或调用已被看门狗放弃的会话 |
| `-12` | `ErrUnknownMethod` | 方法名未知或签名不符协议 | 会话对象上不存在所请求的方法——版本混装 |
| `-13` | `ErrMethodFailure` | 会话方法执行抛异常（诊断文本随同一条消息回传） | 会话协议违例（`Begin` 请求为空/畸形、summary 事件之后再调 `NextEvent`）或垫片内部意外错误；**单个**工程求值失败不是此码——那是一条事件，走 E4 的逐工程披露 |
| `-14` | `ErrOverflow` | 结果超出该句柄所属档位的通道视图容量 | 单个工程的事件载荷装不进求值会话 4MB 的结果视图（大档容量按数百 TU × 37 字段设计） |
| `-20` | `ErrNotOnLine` | 类不被本线承载 | `MsbuildEval` 的构造请求到达了 7 线（CoreCLR 装不下 net472 的 MSBuild 程序集）；求值通道绝不向 7 线路由——此码的存在是让误路由显式失败 |
| `-30` | `LineUnavailable` | 宿主侧：目标线的引擎不可用 | 51 线 CLR 宿主装不起来——即 E1 形态；`detail=` 文本点名卡住的那一步（可执行文件同目录缺垫片 dll、CLR 4 启动失败）与修复途径 |
| `-31` | `CmdTooLarge` | 宿主侧：命令载荷超出通道档位容量 | `Begin` 请求——它携带完整的成员工程清单——超出求值会话 64K 字符的命令区；工程数极多的解决方案 |

### 失败类别分诊表（加载器侧）

记录模式无法检测编译错误（没有 cl 运行）——加载器的
`failed:` TU 类别就是诚实通道。对策表位于
CLI.md（*Failure-class → remedy map* 一节）；与 MSBuild 相关的
单元格如下，且本路线的自动对齐都已就位：

| 失败形态 / 类别 | 对策 |
|---|---|
| varargs 形态（`cannot pass object of non-trivial type ... through variadic function`） | `--clang-compile-flags "-Wno-non-pod-varargs"`（在一个大型 MSVC 代码库上验证有效） |
| GBK/Shift-JIS/... 源文件上的源编码乱码 | 本路线已自动对齐（对未声明编码的条目追加 `-finput-charset=<system ANSI codepage>`；带 BOM 的文件不受影响）。普通编译 db 需要走手工路线：转换为 UTF-8（建议带 BOM）或注入 `-finput-charset=<codepage>` |
| 延迟模板解析残留（`msvc-permissive-unchecked-member-call`、`msvc-permissive-unchecked-ctor`） | `--clang-compile-flags "-fdelayed-template-parsing"`（翻译层已追加 `/Zc:twoPhase-`；此处覆盖残余形态） |
| `msvc-permissive-unchecked-constexpr`（constexpr 绑定到 dllimport/运行期值的模板场景） | **不存在开关——修源码** |
| `msvc-permissive-lenient-specialization`（类模板成员定义缺少 `template<>`） | ccg 自动改写（cl 方言，由编译器自己的 fixit 诊断驱动，仅内存中——从不触碰磁盘；见 CLI.md） |
| lambda 捕获遮蔽形态（MSVC 绑定外部实体，而 clang 绑定捕获项） | **不存在开关——修源码** |

## 6. 已知限制与副作用

- **tlog 一次性重建**：包装构建会重命名 MSBuild 的增量
  跟踪文件（`CL.command.1.tlog` 等——以跟踪器启动的
  工具命名），因此该项目你下一次正常构建会重建
  一次。无害，且无法从 MSBuild 外部修复。
- **Rebuild 覆盖 obj/bin 输出（仅 `--full-build`）**：记录模式
  把每个 MSBuild 管理的产物重定向进沙箱
  （`<layout>/<C>-<P>/msbuild-sandbox/`）；项目真实的
  树只会看到泄漏（会被报告）。`--full-build` 运行真实的
  `-t:Rebuild`，重新生成项目自己的 `build/` 输出——在
  乎这件事时请在临时副本里构建。
- **`VCToolsVersion` 固定值必须是完整的四段版本**
  （`14.44.35207`，而不是 `14.44`）：当前 MSBuild 对短的固定值
  报 MSB8070 失败——项目根本构建不起来，无论在 ccg 还是
  单独构建下。这个与机器无关的 MSBuild 行为很可能是
  "我固定了工具集但构建/版本感知一片混乱"
  这类报告的来源。
- **兼容版本 build 段**：`-fms-compatibility-version`
  的 `<minor>.<build>` 取自工具集目录号
  （`14.44.35207` → `19.44.35207`），它可能与 cl 二进制
  自己的 `_MSC_FULL_VER` build 段不同（例如目录 `35207` 对
  二进制 `194435229`）。clang 只看 major.minor，因此只有
  `_MSC_FULL_VER` 宏的 build 段受影响；该差异被
  如实记录，而不是掩盖。
- **非 ASCII TU 路径**：抽取端到端 unicode 干净（宽字符
  argv、UTF-8 日志），但 ccg 对路径含非 ASCII 的 TU 的
  摄取目前会失败（JSON 读取器缺少 `\uXXXX` 解码，且窄
  `fs::path` 走 ANSI 代码页）。这样的 TU 会被报告为
  `failed` 且退出码 0——诚实、确定，也是
  未来单独修复的主题。
- **捕获日志名携带一个令牌**（`clwrap-<ns>.jsonl`）：MSBuild 的
  文件跟踪器会把日志记进项目的 write-tlog，因此固定的
  名字会被多项目解决方案中后来项目的 Clean
  在构建中途删掉。
  该令牌继续存在于 `CCG_CLWRAP_LOG`
  值（推导锚点）以及 `capture.db` 的 `token` 列中——
  读取器按当前运行的令牌过滤，过期行绝不可能漏进
  某次翻译；捕获 db 本身在每次运行开始时被清理。
- **构建通道默认 `-t:Rebuild`**：完整 TU 覆盖需要完整的
  构建图；记录模式让这件事很便宜（没有编译遍——只有
  图遍历加生成器）。`--full-build` 下它是真实的完整
  编译。普通 Build 模式（只捕获被重新调用的 TU）留作
  以后的优化。默认的求值通道根本不构建——其 TU 覆盖就是
  求值出的 `ClCompile` 集合。
- **系统头文件自包含**：`ccg msbuild` 写出的条目携带
  `/X` 以及按顺序排列的完整 `INCLUDE` 作为
  `-imsvc` 对（记录：从 MSBuild 子进程捕获；求值：直读 ccg
  进程——完整系统目录集请在 VS 开发者提示符里跑）。因此索引
  该 cdb 不再依赖 shell 的
  `INCLUDE`/`CPATH` 或 clang-cl 自动发现：纯 shell、vcvars
  shell 与只有捕获名的环境产出同一张图（求值通道冷 shell 的
  条目不带 `-imsvc` 并如实说明——见 2a 节）。某个 TU 仍因
  缺少 include 而失败时，其失败文本会报告 `(N include dirs fed)`，
  于是短搜索列表可见，而不是
  莫名其妙。
- **外来 MSVC cdb 的覆盖边界**（手工编写或第三方的
  `compile_commands.json`，含原生 `cl.exe` 条目）：三种情况。
  1. *重建* —— 当编译器记号是包含
     `VC\Tools\MSVC\<x.y.z>\` 的绝对路径时，ccg 推导工具集与目标
     架构，定位 VS 安装（cl 路径自己的安装根，否则
     `vswhere`），运行 `VC\Auxiliary\Build\vcvars<arch>.bat
     -vcvars_ver=<x.y.z>`，并像 ccg 产出的路径那样精确物化捕获的
     `INCLUDE`（`/X` + `-imsvc` + 兼容版本）。
     即使原始命令已带 `/X`，兼容版本也从 cl 路径追加——否则
     `#if _MSC_VER` 分支会悄悄用上 clang-cl 的默认。只有
     INCLUDE 物化以命令不带 `/X` 为条件。
     捕获按（工具集，架构）组合缓存，且以
     10 秒为上限；对没有 cl 条目的项目完全跳过。
  2. *仅方言* —— 不带工具集线索的裸 `cl.exe`/`clang-cl` 会被注入
     `--driver-mode=cl`（于是重放不再悄悄使用
     libc++），而原生 `cl.exe` 条目被报告为
     `system-includes: auto-detected`，因为其头文件仍来自
     clang-cl 发现。
  3. *自动检测回退* —— 线索存在、但 vcvars 重建
     失败时，摘要与 `--json` 输出同样会说
     `system-includes: auto-detected`（JSON：`systemIncludes`）。ccg
     绝不会在 argv 并非自包含时
     假装它自包含。
- **cl 下的 PCH 闭包顺序**：文本闭包遵循真实
  驱动器顺序 `/I → CPATH → resource dir → -imsvc → INCLUDE`。自包含
  argv（含 `/X`、`/x` 或 `-X`——cl 的同一个
  IgnoreStandardIncludePath 选项）只使用 argv 目录加
  clang resource 目录；它从不读取索引进程的
  `INCLUDE`/`CPATH`，也从不追加自动发现的工具集链。
  这保证 `stddef.h`/`stdint.h`/`vadefs.h` 解析到
  resource 头文件，而不是 `-imsvc` 里同名的冒牌文件。
