# MSVC 工具链构建（预设 `msvc-dev` / `msvc-dev-asan`）

MSVC 预设是 clang64 参考预设（`clang-dev`/`clang-dev-asan`）的增量兄弟：同一套
C++23 命名模块架构、同一套自拉取依赖，只是编译器不同。clang64 预设始终是
参考工具链，任何时候必须保持绿色。

## 前置条件

1. **装好 MSVC 工具集的 Visual Studio**（含 C++ 核心特性和模块组件：
   `<toolset>/modules/std.ixx` 必须存在——cmake/CcgUtils.cmake 会检查，
   不存在则报出可操作的错误信息）。
2. **CMake ≥ 4.4 + Ninja 已在 PATH 上。**
3. **一个 LLVM 安装**（提供 clang-c 头文件 + 运行期引擎）。VS 自带的
   clang-cl 组件**不附带** clang-c 头文件（已探明）。用 `CCG_LLVM_ROOT`
   指向它（任何带 `include/clang-c` 和 `bin/libclang.dll` 的官方 LLVM
   发布前缀均可）。机器本地的绝对路径放 `CMakeUserPresets.json`
   （**永不**写入入库的预设）。

## 配置 / 构建 / 测试

在 **x64 Native Tools Command Prompt**（或先执行 vcvars64）下运行，
使 `cl.exe` 在 PATH 上——预设按名称解析编译器，不钉绝对路径：

    cmake --preset msvc-dev
    cmake --build --preset msvc-dev
    ctest --preset msvc-dev

所有预设统一使用 **Ninja Multi-Config**：build/test 预设默认 Debug 配置，
换配置只需一个参数（`cmake --build --preset msvc-dev --config Release`、
`ctest --preset msvc-dev -C Release`）。单配置 Ninja 回退依旧受支持，但
必须显式导出编译数据库（多配置生成器不写 `compile_commands.json`，而
`ccg_module_e2e` 门禁读取它；缺省时该门禁会带明确的修复提示直接
FATAL）：

    cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Debug \
          -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ...

`msvc-dev-asan` 是 AddressSanitizer 孪生预设（`/fsanitize=address` 来自
`CCG_USE_ASAN` 在 Debug 配置的 AUTO 默认值；探针已验证——链接和运行期
解析自动完成，无需额外 DLL）。

注意事项：

- 保持 TMP/TEMP 在有空间的盘上（否则 cl D8050）；该错误是临时目录
  缺失或无效，不是源码问题。
- sqlite3 自拉取（sqlite.org amalgamation，SHA256 钉定）并在两种工具链
  上都构建在树内——无需手动安装（ccg_sqlite_smoke 门禁把关）。
- clang-tidy：找不到 clang-tidy 时（搜索 CCG_LLVM_ROOT/bin，其次是 VS
  自带 clang 工具），tidy 目标退化为文档化的空操作；在 MSVC 下不会
  导致 configure 失败。
- ccg_module_e2e（喂 BMI 的自索引）在 MSVC 构建树下跳过：MSVC 树产出
  IFC 格式的 BMI，任何 clang/libclang 发布都无法消费。该门禁在
  clang64 预设上运行。

## 已知 MSVC 工具链限制（14.38–14.51，截至撰写时）

MSVC 14.38–14.51 的 cl.exe 模块前端各有缺陷（单元 ICE → 消费者特化失联
→ 隐藏友元误归类，单调改良梯度）；
C2801 **仅实证于 14.51**：cl 在**消费者 TU 的跨 IFC 合并**阶段把
`<filesystem>` 路径迭代器的类内内联隐藏友元 `operator==` 误归类，报
`C2801`（"operator== 必须是非静态成员"；实为编译器 bug，非 STL bug）。
实测性质：

- 对内容体量敏感，不是固定的构造对：带 `cg.clang` 和完全相同 odr-use
  的小型 TU 编译干净；完整 cg.cli 运行器代码放在一个单元里失败；每个
  较小的组合都能编译。
- 对 import 顺序（9 种排列）、对去掉 cg.clang 与 cg.cli 全部导出自由
  函数的 inline、以及对分区/实现单元的排布方式均不敏感。
- 报错位置来自 IFC 记录的源位置：失败 TU 从未文本包含 `<filesystem>`，
  因此"只给失败 TU 打头文件补丁"无效。

**树内规避现状**：CLI 各运行器与其共享的索引主干均为内联定义的接口
模块（`cg.cli.index/update/msbuild/config/runners`，.ixx），树内已无
任何专为 C2801 而设的结构，C2801 由下节配置期 workaround（详见
cmake/C2801ApplyPatch.cmake）统一承担。

## C2801 配置期 workaround（std BMI 宿主的 `<filesystem>` 遮蔽）

**机制**。C2801 的决定性宿主**有且只有 std BMI**（四格交叉实验：
治 std BMI 即治愈，治别处无效）。std.ixx 的生产者按 CMake 版本分两条
路径（`cmake/CcgUtils.cmake` 顶部的版本分支）：

- **CMake ≥ 4.5**：`import std` 非实验化，CMake 给每个有模块扫描源的
  目标无条件挂 `@cmake_cxx_std` 合成目标（项目侧无开关可拒绝；自建
  std 供给与之并存即 Disagreement 双供应硬错误）。此时补丁经一份
  conforming 格式的影子 modules.json（`CMAKE_CXX_STDLIB_MODULES_JSON`
  覆盖点，`cmake/CcgC2801Workaround.cmake` 的 `ccg_c2801_wire_synth_std`）
  以 `local-arguments.include-directories` 进入合成目标里
  std.ixx/std.compat.ixx 的编译命令。该目录经合成目标的接口传播也会
  出现在消费者 TU 的命令上——补丁头与原头语义零变化（见下），std BMI
  与消费者处于同一种头形态，反而消除 ODR 风险。
- **CMake < 4.5**：项目自编工具集的 `modules/std.ixx`
  （`cg_std_modules` OBJECT 库；实验性合成库实测丢 STL charconv 内联
  静态量），补丁目录以 `BEFORE PRIVATE` 前置到该目标——全树只有
  std.ixx（及同目标里的空锚点 TU）看到遮蔽头，VS 系统文件、ccg 源码、
  其它任何 TU 均不动。

两条路径共用同一个补丁器：

- configure 期，`cmake/C2801ApplyPatch.cmake`（独立 `cmake -P` 脚本）
  把工具集 `include/filesystem` 拷入构建树 `c2801-wa/include/` 并施加
  v_a 变换：类内隐藏友元定义 → 类内模板友元声明 + 类外函数模板定义
  （语义零变化：代入模板实参后签名/函数体/ADL/重载决议全同）。
- `cmake/CcgC2801Workaround.cmake` 仅在补丁**通过验证**时接线，任何
  失败/退场形态都回落原头。

**防"补丁产出错误文件"的安全规则**（跨工具集版本 STL 漂移防护）：

1. **精确锚点恰好一次**：隐藏友元源块与类尾插入点两个文本锚点都必须
   在源文件中**恰好出现 1 次**，0 次或多次都拒绝动手（宁可不打也不
   打错）。
2. **cl 语法自检**：补丁产物先在 `c2801-wa/candidate/` 生成，用
   `cl /Zs`（含该算子 odr-use 的小 TU、遮蔽目录置前）验证；失败再跑
   "原头对照腿"区分是补丁坏了还是环境坏了；任何失败都撤销产物、
   回退原头——`include/` 里永远只有通过 cl 验证的头。
3. **sha256 戳记**（`c2801-wa/source.sha256` 记源与产物两个哈希）：
   源未变且产物未被碰 → 直接复用；源变或产物被动过 → 重拷重打重验；
   戳记损坏（截断/乱写）同样判为不可复用，走重打重写。工具集头被登记为
   `CMAKE_CONFIGURE_DEPENDS`（无论本轮判定结果如何都登记，回退态也能
   感知工具集的后续更新），工具集更新会触发重新 configure 自动重新
   判定；patcher 进程异常退出（rc≠0）时残留 status 一律不作为依据，
   防止把旧工具集时代的产物错配到新工具集。
4. **全分支可见**：configure 日志里所有 `ccg c2801-wa:` 前缀行 +
   状态文件 `c2801-wa/status.txt` + cl 转录 `c2801-wa/logs/`，绝不哑失败。

**开关**：`CCG_MsvcC2801Workaround`（cache option，默认 `ON`）。仅在
MSVC 分支加载（clang64 预设完全不加载该模块，零行为）；`OFF` = 回到
现状（不打遮蔽，std.ixx 直接用工具集原头）。

**回退语义**（`status.txt` 的 token ↔ 行为）：

- `patched-ok` / `reused-patched-ok`：遮蔽生效，std.ixx 用补丁头编译。
- `fell-back:pattern-absent`：工具集 `<filesystem>` 里找不到该隐藏友元
  块——**中性事件**。工具集更新、上游修复（DevCom 11063612 家族）
  到达后模式自然消失，属本 workaround 的**正常退场**。
  此时 std.ixx 用原头编译；若 C2801 仍复现则说明另有原因，按调查线处理。
- 其余 `fell-back:*`（`pattern-not-unique`、`anchor-class-tail-*`、
  `self-check-failed`、`cl-env-broken`、`cl-unavailable`、
  `source-missing`）：WARNING 级提示 + 回退原头，C2801 可能复现。

**验证方法**：

- configure 输出看 `-- ccg c2801-wa: patched-ok ...`（或
  `<build>/c2801-wa/status.txt`）。
- 产物形状：`diff --strip-trailing-cr` 比较 `<build>/c2801-wa/include/filesystem`
  与类外定义（out-of-line）版的 `filesystem`，应零差异（仅换行符差异）。
- 防漂移行为（复用 / 模式缺失 / 模式重复 / 源变更重打 / 产物篡改重建 /
  cl 不可用 / 自检失败撤回 / 戳记损坏自愈）由上述安全规则与状态文件
  保证。
- 上游修复发布后：升级工具集 → 观察 `pattern-absent` 自动退场 → 届时
  可整体移除本机制。

**C2801 治愈后暴露的链接段缺口（已全部修复）**：构建首次抵达链接段时
暴露出三案既有缺口（sqlpp 算子静态成员 / __tlregdtor 修饰名失配 /
GraphStore 析构双产出）并牵出一个真实悬垂引用，共四项修复，全部树内
落地并经全新擦除全量回归验证。四项修复：

1. **sqlpp23 算子静态成员 LNK2001×5**（`equal_to/logical_and/
   logical_or/greater/plus::symbol`）：MSVC 模块单元对"仅经
   `import sqlpp23.core;` 可见"的 inline 变量只发射 UNDEF 引用、不发射
   COMDAT 定义（树内的模块单元都不文本包含这三个 operator 头——表模型
   模块的 GMF 只有名字标签宏头），定义责任落在文本拥有这些头的
   `sqlpp23.core.cppm.obj`——而
   CMake 的 OBJECT→OBJECT 链接只传播 BMI/usage requirements、不传播
   目标文件，该 obj 从未出现在任何链接边上。**修复**：经典 TU 锚点
   `src/db/db.sqlpp.anchor.cpp`（`util.json.bridge.cpp` 先例形态，无
   module 声明），匿名命名空间内以**引用绑定**式 odr-use 锚定 47 个
   内联静态量：29 个运算符的 `symbol`、5 个连接类型的 `_name`、另外
   3 个非模板类的静态数据成员（`no_name_t::name`、`consistent_t::value`、
   `wrapped_static_assert::value`）与 10 个命名空间级内联变量对象（core
   的 9 个加 sqlite3 的 `fallback`）；锚定面按缺陷的受害面划定，不止
   当初浮出的那 5 个（实证要点：`&x` 取址数组形式在 /Od 会被
   常量折叠、不强制产出定义，引用绑定可靠），挂入 `cg_core` OBJECT
   源列表——cg_core 的 obj 骑在每条最终链接边上，一处生效全覆盖；
   clang 侧本就无这些符号，多余 COMDAT 可折叠、零影响。
2. **`__tlregdtor` LNK2019/LNK2001**：cl 14.51 在"函数体从 BMI
   re-emit"路径（module-attached inline 函数副本，如
   `cg.util` 的 `canonical_case_spelling` 函数局部 `thread_local`）把
   CRT TLS 析构注册辅助生成为 C++ 修饰名 `?__tlregdtor@@YAHXZ`，而
   CRT 四变体只提供 extern "C" 的 `__tlregdtor`——纯修饰名不匹配，
   任何 lib 组合都无法解析；经典 TU 同构造则是 C 修饰、正常链接。
   **修法（双重记录）**：历史上以纯链接层修复（零源码改动）——
   `src/CMakeLists.txt` MSVC 分支给 cg_core 加 INTERFACE 链接选项
   `/alternatename:?__tlregdtor@@YAHXZ=__tlregdtor`（只在符号最终
   未解析时生效，绝不覆盖 CRT 自己的定义）。**现状**：
   触发器已治本消灭——`canonical_case_spelling` 的函数局部
   `thread_local` 缓存重设计为显式 `cg::util::PathCaseCache` 对象
   （每个 extraction worker 持有一份），模块单元中不再存在任何
   function-local `thread_local`，`/alternatename` 链接选项已随之
   移除；若未来再现同形缺口，历史修法仍可按上式恢复，工具集
   升级对本案无影响。
3. **GraphStore 析构 LNK2005**：`db.store.ixx` 类外
   `GraphStore::~GraphStore() = default;`（全树唯一一处类外 = default）
   触发双重产出——模块本体 obj 发强非 COMDAT 定义，导入方 obj 从 BMI
   重建 defaulted 体并以 COMDAT 发出，link.exe 拒绝折叠。**修复**：
   改为用户提供空体 `~GraphStore() {}`（语义等价：该类本就非
   trivially destructible；BMI 不再携带"可再生成"信息，导入方回到
   正确 UNDF 引用形态）。clang 侧无影响。
4. **`cg_test_clang` 运行期失败（链接治愈后首次运行暴露）**：
   `clang.m4_cl_dialect` 的 `text_header_unit_imports` 返回长度正确、
   字节全为 0xdd 的名字——`scan_import_decls(buffer.str())` 把指向
   临时 std::string 的 string_view 带出调用，临时在 range-init
   full-expression 末尾已析构，循环体读到已释放内存（MSVC Debug 堆
   对释放块填充 0xdd，确定性暴露；clang 侧堆不擦除、侥幸通过）。
   **修复**：`clang.bmi.ixx` 该处改为具名局部 `const std::string
   text = buffer.str();` 再传入（消除 UB，两工具链行为不变）。

修复回归验证（现状）：Debug / Release 两配置全新擦除树全量构建零错误、
全链接，ctest 17 过 + `ccg_module_e2e` 设计性跳过（与 clang64 等价），
clang64 绊线全绿。

## 架构说明（db 层为什么长这样）

- `import std` 来自工具集自带的 `modules/std.ixx`，以 OBJECT 库形式
  编译（cmake/CcgUtils.cmake）：CMake 的实验性合成 std 静态存档在
  MSVC 下会丢失 STL charconv 内联静态量（所有 std::format 消费者
  LNK2001——探针实测）。MSVC 路径**不使用** `CXX_MODULE_STD` /
  实验性 UUID。
- sqlpp23 表模型是命名模块：图数据库的是 `cg.db.tables`（入口
  `src/db/db.tables.ixx` 自身不声明任何内容，只再导出
  `cg.db.tables.core`、`cg.db.tables.graph`、`cg.db.tables.sites`、
  `cg.db.tables.analysis` 四个子模块），采集库的是 `cg.msbuild.tables`
  （`src/msbuild/msbuild.tables.ixx`）；用到表模型名字的单元 `import`
  它们。这五个表模型模块单元的全局模块碎片里只放名字标签宏头
  `<sqlpp23/core/name/create_name_tag.h>`（模块不导出宏；这个头只有宏
  定义），机制经 `import sqlpp23.core;` 提供。表模型的写法是"名字标签
  上提"：列说明仍嵌套在表结构体里、全部名字不变，只把每个列说明的名字
  标签本体提到命名空间层次并导出——MSVC 14.44 与 14.51 的 IFC 读取器
  读到"导出类里嵌套两层的类带静态数据成员"就崩（C1001），列说明里的
  `_sqlpp_name_tag` 正是这种第二层嵌套类。同一翻译单元里不能既文本
  包含表模型定义、又导入表模型模块（两份同名声明并存，使用处报歧义）。
  GraphStore 用 pimpl（`std::unique_ptr<Impl>`）藏起连接、配置与预编译
  语句这些 sqlpp23 状态：导出类的数据成员里没有 sqlpp23 类型，但接口上
  仍有一个——`GraphStore::conn()` 返回 `sqlpp::sqlite3::connection&`，
  供 cg.db.query 的查询模块执行类型化语句，cg.analysis.external 也
  直接用它删除旧的 source=External 类型线索行。
- JSON 门面（`cg.util.json`）通过纯 C 桥接（`util.json.bridge.h/.cpp`）
  基于 yyjson C API 实现：在模块单元内使用任何 yyjson-cpp 类型会
  导致 MSVC IFC 写入器崩溃（C1001，多种构造变体均复现）。
- MSVC 特有的源码规则（探针实证）：`import std;` TU 的全局模块碎片中
  不得经典包含标准库头与运行库头，例外见下一条；模块作用域内不得出现
  pfr 反射调用点；导出的模块函数不得包装 GMF 静态内联 C 辅助函数；
  **内部分区**（无 `export` 的 `module cg.m:p;`）翻译单元
  必须用 `.cpp` 扩展名、不得用 `.ixx`——cl 单凭 `.ixx` 扩展名即推断
  接口语义，与依赖扫描器为非导出分区发出的 `/module:internalPartition`
  冲突，报 C7622（单 TU 可确定性复现；树内现已无内部分区，本条作为
  平台规则与历史记录保留）。
- 上一条里"不得经典包含"的例外只有 `import std;` 给不了的东西：src 下
  33 个带全局片段的单元里有 10 个，括号里是只能文本取得的原因。
  - `analysis.propagate.ixx`：`<cassert>`（`assert` 是宏）。
  - `analysis.snapshot.ixx`：`<cstddef>`（`offsetof` 是宏）。
  - `clang.loader.ixx`：`<dlfcn.h>`，只在非 Windows 分支（`dlopen` 一族
    是 C 声明）。
  - `db.store.ixx`：`<cstdio>`（`stderr` 是微软运行库头里的宏）。
  - `msbuild/eval.ixx`、`msbuild/compose.ixx`：`<istream>`、`<ostream>`、
    `<sstream>`（文件头写明 std 模块的导出声明面单独不足）。
  - `util/cstd.ixx`：`<cstdlib>`、`<ctime>`，只在 MSVC
    （`_dupenv_s`、`gmtime_s` 是微软 CRT 扩展名）。
  - `util/crashdump.ixx`：`<csignal>`，只在 Windows 分支（`SIGABRT` 是
    宏）；`<crtdbg.h>`、`<stdlib.h>`，只在 MSVC 分支（`_CrtSetReportMode`、
    `_set_purecall_handler`、`_set_abort_behavior` 等微软 CRT 的调试
    钩子与宏）。
  - `util/guard.ixx`：`<csetjmp>`、`<csignal>`，只在非 Windows 分支
    （`sigsetjmp`、`siglongjmp`、`sigaction` 与信号常量）。
  - `util/platform.ixx`：`<io.h>`，只在 Windows 分支（`_get_osfhandle`）；
    `<cerrno>`、`<cstdlib>`，只在非 Windows 分支（`errno` 与错误码是
    宏、`_exit` 是 C 函数）；`<csignal>`，只在非 Windows 分支（读代码
    没有找到使用处）；x86 上的 `<immintrin.h>`（MSVC）或 `<cpuid.h>`
    （其它编译器）是 CPU 能力探测用到的内建函数的声明。
- 名称标签宏头 `<sqlpp23/core/name/create_name_tag.h>` 只含宏，不算
  标准库头，出现在表模型模块的全局碎片里。这条规则同样管 tests，那里
  含这类头的测试单元有四个，理由同上：`clang.test.ixx` 的 `<cstdlib>`
  （`std::system`，文件里注明经全局片段包含）；`cli.test.ixx` 的
  `<cstdio>`、`<cstdlib>`、`<fcntl.h>`，Windows 分支另有 `<io.h>`、
  `<share.h>`，非 Windows 分支另有 `<unistd.h>`（进程内 hook 测试在
  文件描述符一级重定向输出）；`crashdump.probe.ixx` 的 `<cstdio>`
  （`stderr` 是宏）与 MSVC 分支的 `<stdio.h>`；`test_harness.ixx` 的
  `<cstdlib>`（`_putenv_s`）与 MSVC Debug 分支的 `<crtdbg.h>`。
