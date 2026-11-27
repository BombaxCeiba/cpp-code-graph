# cpp-code-graph v1 —— 架构与设计

cpp-code-graph（可执行文件 `ccg.exe`）通过 libclang 解析 C++ 代码库，
抽取语义图（符号 + 边），以节省存储的方式将其存入 SQLite，
并在命令行上回答代码智能查询。v1 以同等规模的参考语料库为目标
（约 660 个 TU，同一 clang64 工具链），把全保真解析与存储节省
作为一等要求。

v1 的范围：index、query、context、callers、callees、impact、trace、stats。
Schema v11 增加了静态虚派发推断（`cg.analysis`）：当局部类型证据支持时，
接口指针/引用调用会被解析到具体实现；不支持时则如实地降级
（候选集 + 置信度 + 回退）。查询命令默认使用该证据（precise 模式）：
置信度 >= 0.7 的证据（1.0 / 0.7）会在调用点上取代对重写家族的过近似；
0.4 的弱推断与 0.0 的标记不是证据，缺少实证证据的结果会被标注为
家族估计。`--no-dispatch` 是字节级一致的静态上界视图。
范围之外（记录备查、不要去实现）：FTS/进程/社区覆盖层、PDG/污点、
detect-changes、增量监听模式、MCP/serve。

## 1. 模块 DAG 与分层

100% 具名模块，C++23，处处 `import std;`。每个项目源文件都是扩展名为
`.ixx` 的模块单元。第三方头文件只出现在全局模块片段（GMF）内部，
且每个头文件恰由一个模块拥有（单一所有者规则，见 §10）。只含宏、不含
声明的头不受这条约束：宏不能跨模块导出，用到的单元各自在全局模块片段
里包含它，不会产生重复的声明；名字标签宏头
`sqlpp23/core/name/create_name_tag.h` 现在就同时出现在 `cg.db.tables`
的四个子模块与 `cg.msbuild.tables` 的全局模块片段里。模块接口保持
小巧；主体内容放在每个总接口（umbrella）模块之下的小型点分名子模块中
（它们曾是分区；点分名模块重构化解了分区语法）。

```
                 app.main  (src/app/main.ixx, export main)
                     |
                  cg.cli  (src/cli/cli.ixx)
                 /     \
           cg.graph    cg.db* (sqlpp23/sqlite3 imported directly, see below)
          /   |    \
   cg.extract cg.db cg.util
        |
    cg.clang  (libclang only via cg.clang)
        |
    cg.util   (yyjson only via cg.util.json)
```
严格分层：一个模块只能导入表中所在层严格低于它的模块。没有环；
构建系统事实上强制了这一点，因为模块 BMI 编码了导入图。

sqlpp23 的两个模块（`sqlpp23.core`、`sqlpp23.sqlite3`）不设包装，由用到
它的单元直接导入：数据库层的 `cg.db.tables` 四个子模块、`cg.db.store`、
`cg.db.query` 的五个子模块，`cg.msbuild` 层的 `cg.msbuild.tables`、
`cg.msbuild.wrap`、`cg.msbuild.pipeline.wraplog`，以及
`cg.analysis.external`（在连接上执行一条删除语句）。`sqlite3.h` 由
`cg.db.store`、`cg.msbuild.wrap`、`cg.msbuild.pipeline.wraplog` 三个单元
的全局模块片段包含（tests 的 schema-mirror 测试单元也包含）。

| 模块 | 文件 | 导入（项目内） | GMF 第三方 include | 职责 |
|---|---|---|---|---|
| `cg.util` | `src/util/util.ixx`（+ `cg.util.json` = `src/util/util.json.ixx`、`cg.util.platform` = `src/util/util.platform.ixx`，均由总接口模块再导出） | — | `cpp_yyjson.hpp`（仅在 `cg.util.json` 中，带 §10.2 的 assert 垫片）；平台头（`io.h`/`windows.h` 或 `unistd.h`，仅在 `cg.util.platform` 中） | 版本串；FNV-1a 64；限定名拆分/拼接；路径规范化（正斜杠、小写盘符）；`PathCaseCache` + 磁盘大小写重拼写；内存 `Interner` 缓存；基于 yyjson-cpp 的最小 JSON 读/写门面；`cg.util.platform` 的控制台与管道 stdin 探测（OS API 唯一允许出现的地方） |
| `cg.util.pcre2`、`cg.util.regex` | `src/util/util.pcre2.ixx`、`src/util/util.regex.ixx`（不经总接口 `cg.util` 再导出） | `cg.util.regex` 导入 `cg.util.pcre2` | `pcre2.h`（仅在 `cg.util.pcre2` 中，8 位码元宽度） | `cg.util.pcre2` 是 PCRE2 C 接口的纯再导出（函数、不透明类型、选项与返回值常量；不设包装类、不含实现）；`cg.util.regex` 是钩子受限正则路径的匹配层：一个编译好的模式对象拥有匹配数据块与匹配上下文，给出全名字匹配（首尾两个锚定选项）、部分匹配（不带选项位）与能否匹配空串（按部分匹配语义用空串试一次）三个查询，每次匹配带显式的步数上限，结局三分（命中、不命中、触限等错误；扫描层把第三类按不命中处理并累计其数目，交出口披露） |
| `cg.cli.scan.names` | `src/cli/cli.scan.names.ixx`（不经总接口 `cg.cli` 再导出） | `cg.util.regex`、`cg.db.schema`、`cg.db.store`、`cg.db.query.dumpkeys`；`cg.cli.hook` 导入它 | — | 钩子受限正则路径的符号名表扫描：按段物化符号名、逐名匹配，先按全匹配扫一遍，零命中且模式不能匹配空串时再按部分匹配重扫一遍，返回命中名与步数触限的名字数（属于产出最终命中名的那一遍）；段间与段内截止探针由调用方以可调用对象注入，触线即整体放弃。不读环境变量、不读时钟、不决定命中之后的出口 |
| `cg.cli.lookup.name` | `src/cli/cli.lookup.name.ixx`（不经总接口 `cg.cli` 再导出） | 只导入标准库；`cg.cli.hook` 导入它 | — | 钩子查找路径对符号名的两处形态判定：限定链在尖括号之外最后一个双冒号处切开（模板实参里的双冒号不是分隔符）；名字能否原样当作查找模式重查回来（裸标识符不受长度护栏约束，其余形态不超过模式长度上限、不含正则特殊字符与控制字符、尖括号之外没有双冒号；噪声词由调用方结合词表排除）。纯字符串判定，不碰库 |
| `cg.cli.render.disclose` | `src/cli/cli.render.disclose.ixx`（不经总接口 `cg.cli` 再导出） | `cg.util`；`cg.cli.hook` 导入它 | — | 钩子注入文本的纯字符串组装：字节上限的 UTF-8 序列安全前缀、显示截断、预算保险丝、超限分级信封（命中总数、能原样重查的名字里字典序最小的前几名、一行收窄建议，未判定数不为零时首行注明）、步数上限披露行与把它接在正文之后的预算内拼接。不碰库、不碰截止探针 |
| `cg.db` | `src/db/db.ixx` + `cg.db.schema`（`db.schema.ixx`）、`cg.db.store`（`db.store.ixx`）、`cg.db.query`（`db.query.ixx`） | — | `sqlite3.h` | 经 sqlpp23 使用 SQLite：打开/PRAGMA/DDL、字符串驻留表、upsert（files/symbols/edges/includes/imports）、按文件清除、全部查找与遍历形态的查询、stats。对 libclang 一无所知 |
| `cg.db.tables` | `src/db/db.tables.ixx`（入口，自身不声明任何内容，只再导出下面四个子模块；不经总接口 `cg.db` 再导出）+ `cg.db.tables.core`（`db.tables.core.ixx`）、`cg.db.tables.graph`（`db.tables.graph.ixx`）、`cg.db.tables.sites`（`db.tables.sites.ixx`）、`cg.db.tables.analysis`（`db.tables.analysis.ixx`） | — | `sqlpp23/core/name/create_name_tag.h`（仅宏） | sqlpp23 表模型（命名空间 `cg::db::tab`），手写形状，表与表互不引用：`core` 是元数据与字符串表、kind/边类型字典、sqlite 目录表的只读模型、文件登记、指纹、失败条目、实时变更协调各表，加聚合与连接用的全部别名标签；`graph` 是符号、边、包含、导入四张表；`sites` 是类型线索与调用点两张表；`analysis` 是传播槽、透传包装、实例桥传入侧与体内调用侧、符号贡献登记五张表。用到表模型名字的单元自己 `import cg.db.tables;` |
| `cg.clang` | `src/clang/clang.ixx` + `cg.clang.loader`、`cg.clang.capi`（`clang.capi.ixx`）、`cg.clang.driver`、`cg.clang.tu`（`clang.tu.ixx`）、`cg.clang.compile_db`（`clang.compile_db.ixx`）、`cg.clang.bmi` | `cg.util` | `clang-c/Index.h`（仅在 `cg.clang.capi` 中） | `cg.clang.capi` 再导出 CX 类型 + RAII（`Str`、`Index`、`Tu`）+ 薄游标门面（kind/spelling/location/children/…）；`cg.clang.tu` 解析 + 诊断；`cg.clang.compile_db` 读取 compile_commands.json、对命令串分词、构建每 TU 的 argv |
| `cg.extract` | `src/extract/extract.ixx` + `cg.extract.model`（`extract.model.ixx`）、`cg.extract.visitor`（`extract.visitor.ixx`） | `cg.clang`、`cg.util` | — | `cg.extract.model` 的纯记录类型（`SymbolRec`、`EdgeRec`、`IncludeRec`、`ImportRec`、`TuExtract`）+ 那个唯一的 `unique_key` 构建器（单一事实来源，§4）；`cg.extract.visitor` 遍历一个 TU 的游标树并产出记录（具名模块 import 来自 `CXCursor_ModuleImportDecl`、主文件过滤）。纯函数：无 DB、无 I/O |
| `cg.analysis` | `src/analysis/analysis.ixx` + `cg.analysis.model`（`analysis.model.ixx`）、`cg.analysis.collect`（`analysis.collect.ixx`）、`cg.analysis.points_to`（`analysis.points_to.ixx`）、`cg.analysis.propagate`（`analysis.propagate.ixx`）、`cg.analysis.dispatch`（`analysis.dispatch.ixx`） | `cg.clang`、`cg.db`、`cg.util` | — | 动态派发分析（schema v11）。`model`：纯数据 + 置信度阶梯 + 字节一致的 `unique_key` 构建器（analysis 不能导入 `cg.extract`）；`collect`：第二次定向 AST 遍历，产出 `TuAnalysis`（L0 线索：声明、`new T`、聚合、`make_unique/shared`、转型、`&obj`、实参；纯数据，无游标离开工作线程）；`points_to`：`TypeOracle` 接缝（今天是桩，全程序指针/别名分析将来只替换该模块）；`propagate`：L1 函数内别名/返回值流 + L2 对已持久化提示/调用点的全局工作列表不动点（有界、确定）；`dispatch`：重写家族 ∩ 动态类型集合 → `virtual_dispatch` 边 + 候选集 + 如实回退。**依赖栅栏（既定设计）：analysis → 仅 db/clang；graph/cli → analysis；extract/db/clang 绝不导入 analysis**（由 `tests/portability_scan.cmake` 强制） |
| `cg.graph` | `src/graph/graph.ixx` + `cg.graph.build`（`graph.build.ixx`）、`cg.graph.traverse`（`graph.traverse.ixx`） | `cg.extract`、`cg.analysis`、`cg.db`、`cg.util` | — | `cg.graph.build` 批量导入（两阶段：先符号，后边解析级联）、真增量索引（指纹陈旧检查、include 闭包脏扩展、悬空边重解析、覆盖完备的按文件替换、带具名失败类别的模块 BMI 输入校验——SCHEMA.md §7）；`cg.graph.traverse` 面向 callers/callees/impact/trace 的 BFS/最短路径算法、变更集传播（changes）、include 环检查（在已解析 include 图上求 SCC；IncludeResolver 位于此模块，`cg.graph.build` 导入它，双方共享同一套解析规则） |
| `cg.cli` | `src/cli/cli.ixx`（总接口：run_main，再导出整个家族）+ `cg.cli.parse`（`cli.parse.ixx`）、`cg.cli.render`（`cli.render.ixx`）、`cg.cli.runners`（`cli.runners.ixx`，共享主干：引擎准备、命令列表解析、索引收尾、store 打开与模块单元扫描管道）、`cg.cli.commands`（`cli.commands.ixx`：查询执行器 + 分发，它导入四个 runner 模块——即汇聚点）以及 runner 模块 `cg.cli.index`/`cg.cli.update`/`cg.cli.msbuild`/`cg.cli.config`（各自内联持有入口函数与私有辅助函数） | `cg.graph`、`cg.db`、`cg.util`、（`cg.clang`、`cg.msbuild` 仅在 runner/主干模块中） | — | `cg.cli.parse`：argv → `Command` 变体 + 用法错误/exit 2；`cg.cli.render`：文本与 JSON 输出形态（JSON 经 `cg.util.json`）；`cg.cli.runners`：共享主干；`cg.cli.commands`：查询执行器 + 分发；四个 runner 模块内联持有各自入口（它们曾是实现单元——点分名模块重构废除了这种无 BMI 的拆分后，现在全部是接口）。分层无环：commands → runner 模块 → runners → parse/render；总接口再导出一切，故 `import cg.cli;` 不变 |
| `app.main` | `src/app/main.ixx` | `cg.cli` | — | `export int main()`（已验证模式；clang 的 `-Wmain-attached-to-named-module` 是良性噪声）。顶层 try/catch、退出码映射 |
| `cg.test.harness` | `tests/test_harness.ixx` | `cg.util` | — | 微型断言/计数测试框架（`CG_CHECK`、`cg::test::run(name, fn)` 注册表）；允许的第三方集合中没有外部测试框架 |

拆分规则：当任何单个 `.ixx` 将超过约 300 行时，拆出一个新的点分名子模块
（`cg.util` -> `cg.util.json`）；总接口文件以 `export import cg.util.json;`
再导出子模块，自身不声明任何大块内容。（历史上这些是模块分区；点分名
模块重构把分区语法化解为普通点分名模块，因此总接口的 `import cg.util;`
表面不变。）

### CMake 目标结构

- `cmake/CcgUtils.cmake` —— 辅助函数（模块文件集添加器、模块目标终结器、
  DLL 拷贝、测试注册；代码见 §8.5）。
- 对象库 `cg_core` —— 拥有唯一一个 `PUBLIC FILE_SET CXX_MODULES` 文件集，
  包含所有 `src/**/*.ixx`，`SYSTEM` 关闭，链接第三方集合。被可执行文件
  消费的带 PUBLIC CXX_MODULES 的对象库是在本机已验证的形态（sqlpp23
  自己的 `sqlpp23::sqlite3_module` 正是如此）。若它哪天出问题，已文档化
  的回退是按可执行文件直接列出所需 `.ixx` 文件的文件集（该回退形态同样
  经过端到端验证）。
- 可执行文件 `ccg`、`cg_test_util`、`cg_test_db`、`cg_test_clang`、
  `cg_test_extract`、`cg_test_graph`、`cg_test_cli` —— 各自链接 `cg_core`，
  测试额外链接 tests 文件集中的 `test_harness.ixx`。

## 2. 数据流

```
compile_commands.json
        |  cg.clang.compile_db  (JSON via cg.util.json; tokenize "command",
        |                       keep argv[1:], drop "-c" and "-o <path>")
        v
 per-TU: cg.clang.tu parse(argv, CXTranslationUnit_DetailedPreprocessingRecord)
        |                       (+ diagnostics, severity >= error collected)
        v
 cg.extract.visitor        one TuExtract = {symbols, edges, includes, errors}
        |                       (records carry structured callee metadata,
        |                        unique_key built by the ONE shared function)
        v
 cg.graph.build            freshness: stat (mtime+size) fast path, sha256
        |                       confirm on stat change, per-entry command
        |                       digest (SCHEMA.md §7); dirty-set expansion
        |                       through the includes graph; parallel
        |                       parse+extract of dirty TUs (worker pool)
        |                  phase A: upsert ALL symbols of the whole batch
        |                       (stable sym_id via ON CONFLICT unique_key),
        |                       build in-memory name index
        |                  phase B: resolve + upsert edges (3-tier cascade, §5),
        |                       upsert includes, per-file purge for re-index
        |                       (coverage-complete files only)
        v
 SQLite (cg.db)  --- cg.graph.traverse <--- cg.cli.commands <--- render
```
抽取是纯函数 `(argv) -> TuExtract`；只有 `cg.graph.build` 与 DB 通信。
正是这条接缝使将来的多线程索引（§7）与单元测试保持廉价。

## 3. 错误处理

- 错误在内部是异常（`std::runtime_error` 子类型；v1 没有自定义异常层级
  ——乏味胜过取巧）。模块接口是异常中性的；跨层没有任何捕获并吞掉。
- `app.main` 最后兜底捕获 `std::exception`：向 stderr 打印
  `ccg: error: <what>`，退出码 1。用法错误（未知命令/旗标、缺操作数）
  由 `cg.cli.parse` 以 `UsageError` 抛出，随用法文本打印，退出码 2。
- 可恢复的单 TU 失败绝不中止索引运行：解析失败的 TU 在 `files` 中记录
  `parse_status = failed` 及 `error_count` 与 `last_error`，其（可能不完整
  的）抽取被丢弃，运行继续；摘要报告失败的 TU；运行仍以 0 退出（DB 可用）。
  这与参考项目的行为一致。
- CLI 诚实契约：带非空 `error` 或 `partial:true` 的载荷
  以 1 退出；仅 `truncated:true` 以 0 退出（计数/风险仍在完整集合上计算）；
  列表截断上限绝不截断所报告的计数。

## 4. 符号身份与解析（extract + graph.build）

- `unique_key`（格式在参考语料库上实测，节点侧与边侧字节一致，因为
  `cg.extract.model` 中的同一个函数构建它）：
  `kind|namespace|name|defining_file_path[|p<sig_hash>][|c]`
  （`|c` 标记 const 方法；参数后缀仅用于函数类 kind）。它驻留进 `str`
  表；`symbols.unique_key_id` 为 UNIQUE，因此 sym_id 跨重索引稳定——按文件
  替换依赖这一点。
- 两阶段导入（§2）之所以存在，是因为 TU A 的调用目标位于 TU B；对着完整
  的符号索引解析消除了 TU 顺序敏感性。
- CALLS 解析级联（3 级，在参考语料库上实测；边上的 `confidence` 记录哪一
  级胜出：1.0 / 0.7 / 0.4）：
  1. 名字 + 父类 + `sig_hash` 精确匹配（重载正确）；
  2. 名字 + 父类，命名空间尾部精确匹配（绝不做子串匹配——`Base` 不得
     匹配 `Database`）；
  3. 项目允许清单中的裸唯一名。
  第 3 级后仍未解析 → 丢弃边，计入索引摘要（`unresolved edges: N`）。
  v1 不存储任何未解析的东西。
- 调用边卫生规则（在参考项目上实测）：跳过 `operator` 重载调用、编译器
  内部前缀（`_M_`、`__`、`_X`）、标准库容器方法；按项目路径允许
  清单对被调方做门控（定义文件位于已索引根之下的符号）。
- 调用方解析：先经 `semantic_parent` 链找外围函数，其次用最小外围行范围
  映射（MEMBER_REF_EXPR 的链可能跳过方法本身——在参考项目上实测）。行
  范围映射在同一次遍历中按 TU 构建。
- 仅定义节点（`is_definition()`），纯虚方法除外（天然只有声明）——防止
  .h 声明 + .cpp 定义产生双份节点。跨 TU 重复（每个 TU 重新解析共享头）
  先在按批的内存 unique_key 去重中收敛，再在 UPSERT 中再收敛一次。
- 命名空间不是节点类别（对参考项目的一个有意偏差）：
  符号携带 `ns_id`（驻留的完整命名空间路径）。命名空间节点会为一个对 v1
  查询零价值的节点类别增加节点和成员边；第 2 级中的命名空间尾部匹配就
  工作在驻留字符串上。记录成员关系同样不是边：`symbols.parent_id`（在
  参考项目上实测的一项经济性变通）。
  边表达语义关系（calls/extends/overrides/alias/friend），不表达作用域。

## 5. 边语义

| type | src → dst | notes |
|---|---|---|
| `calls` | 调用函数 → 被调函数 | CALL_EXPR.referenced 加 MEMBER_REF_EXPR.referenced（过滤到函数/方法 kind；补上接口/代理缺口）。每个不同调用点一行（`UNIQUE(src,dst,type,site_file,site_line)`；来自参考语料库的证据：合并调用点曾丢掉一个函数内 7/7 个调用点）；`occurrences` 计同一调用点跨 TU 的重复观测 |
| `extends` | 派生记录 → 基类记录 | 来自 CXXBaseSpecifier 子节点；attrs 位：访问权限（public/protected/private）、is_virtual |
| `overrides` | 方法 → 基类虚方法 | 沿 extends 链按虚签名匹配；析构函数名映射 `~D`→`~B` |
| `type_alias` | typedef/别名 → 目标记录 | 目标常在外部 → 第 3 级式解析，未解析则丢弃 |
| `friend_of` | 友元函数/类 → 授权记录 | |
| `specializes` | 显式特化/偏特化 → 主模板 | 仅当 §6 的探针证明干净时；否则 v1 不产出 |
| `uses` | 声明符号 → 被引用记录 | 声明类型引用：字段自身的类型、函数的参数/返回类型、函数体内的局部变量/命名空间作用域变量（函数拥有它们）。规范化：剥离 cv/引用/指针/数组、解析 typedef、特化折叠到主模板、遍历模板实参（std::vector<Widget> → Widget）。外部记录（std、系统头）不建边；不完整的项目类型按名延迟。attrs 位 0-2：出现位置类别（字段/参数/返回值/局部变量/变量）。支撑记录级影响面查询（CLI.md 中的 §5 注记） |

文件关系不是边（文件不是符号）：DEFINES 由 `symbols.file_id` 隐含；
include 关系放在自己独立的小表 `includes` 中（一个在参考语料库上实测的
形态），该表将来还支撑 `check` 式环查询。

## 6. 模板实例化策略

选定：**方案 1——仅主模板**（该策略经先行工程实测验证——它发布的
正是这一方案，并把实例化抽取器当死代码删除了）：

- 模板自身的游标就是节点；`flags.is_template` 加 `template_params`
  （JSON，来自 TEMPLATE_TYPE_PARAMETER 子节点）；偏特化打上
  `is_partial_spec` 标志。
- 隐式特化（`Foo<int>`）在 libclang 遍历中不产生游标——从 TYPE_REF 拼写
  合成节点（方案 2）已被参考项目证明徒劳；完整实例化图（方案 4）违反
  存储经济性。两者均被否决。
- 带守卫的探针（M3 期间）：在参考语料库上测量
  `clang_getSpecializedCursorTemplate`；若它对显式实例化与偏特化干净地
  解析，则以实参数量为边属性产出 `specializes` 边——仍是零合成节点。若
  噪声大或慢：放弃并记录偏差。理由：当下零风险的声明保真，外加一次廉价
  的 C API 调用换来 libclang 真正能看到的唯一实例化关系。

## 7. 线程策略

索引并行解析、串行入库：

- 一个工作线程池（`std::jthread`、`--jobs N`、默认 auto =
  `min(cores-2, 12)`）运行纯函数的每 TU 抽取；每个工作线程各自拥有自己
  的 `CXIndex` 与 `CXTranslationUnit` 生命周期（libclang：二者都不得跨
  线程），且每个 `CXString` 都在所属线程上于 `cg.clang`/`cg.extract`
  内部物化为 `std::string`，因此离开工作线程的只有纯 `TuExtract` 数据；
- 任务按最长优先（主文件大小）分发——语料分布是重尾的，最大的 TU 必须
  最先启动；
- 所有 DB 写入发生在调用线程上、线程池汇合之后，按 compile-db 顺序、
  在单个批量事务内——对任意工作线程数，存储的行都与串行运行一致（有一
  个 ctest 钉住这一点）；
- SQLite 以 threadsafe=1 编译（已验证）并以 WAL 打开；将来的多读者查询
  路径无需 schema 变更。

## 8. 第三方接线（3rdParty/CMakeLists.txt）

参考项目的函数风格（每包一个 `_cpm_add_*` / `_config_*` 函数、STATUS
消息、静态最小配置、版本钉住），CPM 形态。顶层 CMakeLists 依次执行：
`cmake_minimum_required(3.28)` → 版本条件的
`CMAKE_EXPERIMENTAL_CXX_IMPORT_STD` 门（§10.1）→
`project(cpp-code-graph LANGUAGES C CXX)`（yyjson 需要 C）→ C++23 全局
设置 → CPM 引导（缓存在 `.cpm-cache`，已预热）→ `add_subdirectory(3rdParty)`
→ `add_subdirectory(src)` → `include(CTest)` + tests。

```cmake
# 3rdParty/CMakeLists.txt — sketch; the full proven recipes live in the
# committed 3rdParty/CMakeLists.txt itself
set(GITHUB_MIRROR_URL "https://github.com" CACHE STRING "GitHub prefix override")

# System libs resolve through the ADAPTIVE discovery policy (section 8.6):
# compiler-derived root -> standard find_* search -> CCG_*_ROOT override
# (which wins when set) -> FATAL_ERROR naming the override. Sketch:

function(_config_sqlite3)               # system sqlite3 STATIC (user decision)
    # Raw file name beats find_package(SQLite3): the find module resolves
    # libsqlite3.dll.a on this MinGW-style toolchain (probe-measured).
    set(CCG_SQLITE3_ROOT "" CACHE PATH "sqlite3 prefix override")
    set(CG_SQLITE3_ROOT "" CACHE PATH "deprecated alias of CCG_SQLITE3_ROOT")
    # override set -> search ONLY under it; else CCG_TOOLCHAIN_ROOT (derived
    # from CMAKE_CXX_COMPILER) with NO_DEFAULT_PATH, else plain find_*.
    # Still not found -> FATAL_ERROR naming CCG_SQLITE3_ROOT.
    # sqlpp23's own find_package(SQLite3) picks these cache entries up:
    set(SQLite3_INCLUDE_DIR "${SQLITE3_INCLUDE_DIR}" CACHE PATH "" FORCE)
    set(SQLite3_LIBRARY "${SQLITE3_STATIC_LIBRARY}" CACHE FILEPATH "" FORCE)
    add_library(cg_sqlite3 INTERFACE); add_library(cg::sqlite3 ALIAS cg_sqlite3)
    target_include_directories(cg_sqlite3 INTERFACE "${SQLITE3_INCLUDE_DIR}")
    target_link_libraries(cg_sqlite3 INTERFACE "${SQLITE3_STATIC_LIBRARY}")
endfunction()

function(_config_libclang)              # system libclang via import lib (NOT libclang.a!)
    set(CCG_LLVM_ROOT "" CACHE PATH "LLVM/libclang prefix override")
    set(CG_LIBCLANG_ROOT "" CACHE PATH "deprecated alias of CCG_LLVM_ROOT")
    # same chain; import lib candidates libclang.dll.a (MSYS2) / libclang.lib
    # (LLVM layout); CCG_LIBCLANG_BIN (bin dir of the found prefix) feeds the
    # test PATH prepend and the opt-in DLL-copy closure.
    add_library(cg_libclang INTERFACE); add_library(cg::libclang ALIAS cg_libclang)
    target_include_directories(cg_libclang INTERFACE "${LIBCLANG_INCLUDE_DIR}")
    target_link_libraries(cg_libclang INTERFACE "${LIBCLANG_IMPLIB}")
endfunction()

# CPM packages — call order matters (pfr target must exist before cpp_yyjson
# validates CPPYYJSON_PFR_TARGET). Pins and OPTIONS verbatim below
# (all proven GREEN on this machine):
#   pfr        boostorg/pfr            boost-1.92.0                       -> Boost::pfr
#   yyjson     ibireme/yyjson          1f934d2aaee3e2d016fc1f271b6a01388581dcbf (static, tests/doc/misc/fuzzer OFF)
#   cpp_yyjson BombaxCeiba/yyjson-cpp  2ed9d20e485a5040e64165b8990ed0682dce458c (GIT_SUBMODULES "",
#              CPPYYJSON_PFR_TARGET=Boost::pfr, CPPYYJSON_USE_STD_FORMAT=ON, test/bench OFF)
#   sqlpp23    rbock/sqlpp23           7e8c9a7fde5a7bcc6a1cf694c29a4e9a886ffeb6
#              (BUILD_SQLITE3_CONNECTOR=ON, BUILD_WITH_MODULES=ON -> import sqlpp23.core / sqlpp23.sqlite3)
# All: GIT_SHALLOW TRUE, SYSTEM YES, EXCLUDE_FROM_ALL YES, URLs "${GITHUB_MIRROR_URL}/<org>/<repo>.git"

_config_sqlite3()
_config_libclang()
_cpm_add_pfr()
_cpm_add_yyjson()
_cpm_add_yyjson_cpp()
_cpm_add_sqlpp23()
```
消费规则（单一所有者、GMF include 位置）：

- `cg_core` 链接 `cg::sqlite3`、`cg::libclang`、`sqlpp23::sqlite3_module`
  （连带拖入 `sqlpp23::core_module`）、`cpp_yyjson::cpp_yyjson`、
  `cg::pcre2`（PCRE2 10.49，静态库，解释器档、不开即时编译；目标公开
  头文件目录与静态链接宏）。
- sqlpp23 被直接 import（`import sqlpp23.core; import sqlpp23.sqlite3;`）
  ——在本工具链上已验证；不需要包装 .ixx。原生 DDL/PRAGMA 经连接的调用
  运算符 `db(R"(...)")` 走；手写的 schema 结构在 GMF 中使用仅宏头文件
  （正是 sqlpp23 自己的模块测试模式）。这些坑是铁律（探针命中）：
  用 `std::int64_t` 不用 `int64_t`；select 结果以非 const 存储；
  `order_by(col.asc())`。
- sqlite3.h 被 `cg.db.store`、`cg.msbuild.wrap`、`cg.msbuild.pipeline.wraplog`
  三个单元的全局模块片段包含（tests 的 schema-mirror 测试单元也包含）。
  宏不跨模块边界，所以用到 SQLITE_OPEN_* 旗标宏的单元只能自己包含；
  `cg.db.store` 另直接调用预编译语句的绑定与执行、语句追踪等 sqlite3 C
  接口，`cg.msbuild.wrap` 另执行裸 SQL 语句并调用
  `sqlite3_busy_timeout`，`cg.msbuild.pipeline.wraplog` 只调用
  `sqlite3_busy_timeout`。src 下实际用到的旗标只有
  `SQLITE_OPEN_READWRITE` 与 `SQLITE_OPEN_CREATE`。
- yyjson-cpp 只能经 `cg.util.json` 的门面触达。
- PCRE2 只能经 `cg.util.pcre2` 的再导出触达；只有 `cg.util.regex` 与
  它的测试导入该单元。
- libclang 只能经 `cg.clang.capi` 的导出触达。
- SQLite 3.54.0 amalgamation 只是已文档化的升级路径——v1 不要接线。

### 8.5 cmake/CcgUtils.cmake

```cmake
function(ccg_add_modules target base_dir)          # append .ixx files as one PUBLIC module file set
    target_sources(${target} PUBLIC FILE_SET CXX_MODULES
                   BASE_DIRS ${base_dir} FILES ${ARGN})
endfunction()

function(ccg_add_dir_modules target dir)            # 子目录清单 src/<层>/modules.cmake 用：文件名相对 dir；新增单元写进所属层的清单
    ccg_add_modules(${target} "${CMAKE_CURRENT_SOURCE_DIR}" ${dir}/${ARGN})
endfunction()

function(ccg_module_target target)                 # C++23 + import std, once per module-owning target
    set_target_properties(${target} PROPERTIES CXX_MODULE_STD ON)
    target_compile_features(${target} PUBLIC cxx_std_23)
endfunction()

# Toolchain derivation (section 8.6 step 1) — computed at include time:
get_filename_component(CCG_TOOLCHAIN_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
get_filename_component(CCG_TOOLCHAIN_ROOT "${CCG_TOOLCHAIN_BIN}" DIRECTORY)

function(ccg_add_test name)                        # test exe: links cg_core + harness, PATH for runtime DLLs
    add_executable(${name} ${ARGN} tests/test_harness.ixx)
    ccg_add_modules(${name} "${CMAKE_SOURCE_DIR}" ${ARGN} tests/test_harness.ixx)
    # ... link cg_core; then prepend the DERIVED bin dirs (libclang bin +
    # compiler bin, normally the same directory):
    #   ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${CCG_LIBCLANG_BIN};..."
endfunction()

option(CCG_COPY_RUNTIME_DLLS "Copy toolchain runtime DLLs beside built executables" OFF)
# ccg_toolchain_dlls(out) globs the closure from the resolved LLVM prefix
# (libclang.dll / libc++.dll by name, libLLVM-* / libxml2-* / libffi-* /
# libiconv-* by pattern; missing entries skip with a STATUS line).
# ccg_copy_dlls_after_build(target dll...) — proven POST_BUILD copy_if_different loop,
# used only when CCG_COPY_RUNTIME_DLLS=ON.
```
（确切的函数体在仓库中；上面的形态是已验证的。）

### 8.6 工具链可移植性（发现策略）

任何构建输入或随附二进制都不含机器绝对的工具链路径（由
`ccg_portability_scan` ctest 钉住）。每个工具链位置都经同一个四步策略
解析：

1. **从实际使用的编译器推导。** configure 期：对 `${CMAKE_CXX_COMPILER}`
   做 `get_filename_component(... DIRECTORY)` 两次——编译器 `bin/` 的父
   目录即工具链根（cmake/CcgUtils.cmake 中的 `CCG_TOOLCHAIN_ROOT`）。
   这适用于任何 MSYS2 式前缀（clang64/ucrt64/mingw64）与标准 LLVM 布局；
   CMake 在 `project()` 期间把以 PATH 名字给出的编译器（CMakePresets.json
   里的 `"CMAKE_CXX_COMPILER": "clang++"`）解析为完整路径。运行期：已加载
   `libclang.dll` 的目录（Windows 模块 API；`windows.h` 的 include 位于
   `src/clang/clang.driver.ixx` 的全局模块片段中），然后一次 PATH 搜索。
2. **标准搜索回退。** configure 期：当第 1 步无命中时用普通
   `find_path`/`find_library`/`find_file`（不带 `NO_DEFAULT_PATH`）。
   运行期：`PATH` 上第一个 `clang++`/`clang`。
3. **覆盖变量优先。** configure 期：缓存变量 `CCG_LLVM_ROOT`、
   `CCG_SQLITE3_ROOT`（默认为空；一旦设置只搜索它们——不静默回退到另一
   个安装；`CG_LIBCLANG_ROOT` / `CG_SQLITE3_ROOT` 继续作为弃用别名工作）。
   运行期：环境变量 `CCG_LLVM_ROOT` 或 `ccg index --llvm-root DIR`——它们
   定位的是驱动器，绝不是裸 include 路径。
4. **失败要响亮。** 仍未解析 → 点名覆盖变量的可行动错误（configure：
   `message(FATAL_ERROR)`；运行期：抛出点名 `CCG_LLVM_ROOT` /
   `--llvm-root` 的错误）。绝不硬编码绝对路径，绝不带着错误猜测静默成功。

运行期的 include 知识来自**向驱动器询问**而非字面量：
`clang++ -print-resource-dir` 给出资源目录；`clang++ -E -v -x c++ -`
（以及 `-x c`）的 stderr 中 `#include <...> search starts here:` /
`End of search list.` 之间的块给出本安装的默认 include 搜索顺序。报告出的
列表喂给 `fallback_argv()`（逐字、按报告顺序）与 `driver_supplement` 的
隐式 include 注入（去掉资源 include 条目，它由 `-resource-dir` 旗标携带）。
结果在进程生命周期内缓存。

**机器本地的编译器逃生通道。** CMakePresets.json 按 PATH 名字解析编译器。
需要显式钉住的机器在项目根旁边创建自己的 `CMakeUserPresets.json`（绝不
提交；CMake 自动合并它），只覆盖编译器缓存变量：

```json
{
  "version": 6,
  "configurePresets": [
    {
      "name": "clang-dev-local",
      "inherits": "dev",
      "cacheVariables": {
        "CMAKE_C_COMPILER": "C:/opt/LLVM/bin/clang.exe",
        "CMAKE_CXX_COMPILER": "C:/opt/LLVM/bin/clang++.exe"
      }
    }
  ]
}
```
`cmake --preset clang-dev-local` 随即以该工具链配置；configure 期发现自动
跟上（第 1 步从钉住的编译器推导根），且当头文件/库位于别处时
`CCG_LLVM_ROOT`/`CCG_SQLITE3_ROOT` 仍可用。

### 8.7 AddressSanitizer 开关（CCG_USE_ASAN，cmake/CcgAsan.cmake）

sanitizer 是参考项目风格的构建选项（使用 ASAN 的惯用法），绝不是预设
旗标；任何旗标都不写入缓存 `CMAKE_*_FLAGS` 条目或工具链文件。一个三态
缓存 STRING 在 configure 时解析一次：

| `CCG_USE_ASAN` | 含义 |
|---|---|
| `''`（默认） | AUTO——当且仅当正在构建 `CCG_ASAN_CONFIGS` 之一时启用。单配置：`CMAKE_BUILD_TYPE` 不区分大小写比较；多配置：生成器配置集合中的任意一个。 |
| `ON` | 强制启用（仍只针对 `CCG_ASAN_CONFIGS` 配置）。 |
| `OFF` | 强制禁用（`clang-dev` 预设钉住此值：快速迭代树保持无 sanitizer；`cmake --preset clang-dev -DCCG_USE_ASAN=ON` 覆盖该钉住）。 |

`CCG_ASAN_CONFIGS`（默认 `Debug`）是被插桩集合，也是已文档化的逃生通道：
`cmake -G "Ninja Multi-Config" -DCCG_ASAN_CONFIGS="Debug;Release"` 也插桩
Release；仅强制 `CCG_USE_ASAN=ON` 绝不扩大集合（多配置的 ON 树仍只插桩
Debug，除非扩展列表；单配置的 ON 树若其构建类型不在列表中，会得到响亮的
WARNING，而不是静默未插桩、却号称"已消毒"的构建）。

编译旗标的传送方式：开关激活期间，旗标被追加到恰好 `CCG_ASAN_CONFIGS`
各项的按配置 `CMAKE_CXX_FLAGS_<CONFIG>` 变量——configure 期作用域使它们
不进入任何缓存条目、预设与工具链文件。它们最初以
`$<$<CONFIG:Debug>:...>` 生成器表达式搭载在项目目标上，但 CMake 4.4 按
消费者合成的模块 BMI 目标（`<provider>@synth_N`，为 `std.ixx` 与跨目录
消费的 sqlpp23 模块接口而建）在无配置的上下文中捕获消费目标的
`COMPILE_OPTIONS`，生成器表达式在那里错误求值为 TRUE，把
`/fsanitize=address` 泄漏到了 Release/RelWithDebInfo ninja 文件中的 6 条
合成 BMI 规则上（实测）。生成器即使对合成规则也按配置展开旗标变量，从而
恢复了该保证：

```cmake
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=address -fno-omit-frame-pointer")
target_link_options(t   PRIVATE $<$<CONFIG:Debug>:-fsanitize=address>)  # clang executables
```

因此在多配置树（Ninja Multi-Config）中只有列出的配置被插桩，Release 产物
不含 `__asan` 符号。变量传送的作用域副作用：树中每个 C++ 目标都带这些
旗标编译（含第三方 C++；C 目标如 sqlite/yyjson amalgamation 不受影响）
——覆盖比旧的"仅项目目标"作用域更深，而那些模块 BMI 本来就已通过合成
规则泄漏被插桩。`ccg_module_target()`（cmake/CcgUtils.cmake）仍为每个
项目目标调用 `ccg_asan_target()` 以覆盖链接侧：clang 可执行文件在链接时
得到 sanitizer，外加下文的运行时 DLL 拷贝；MSVC 可执行文件每目标无需
任何处理。预设完全不带 sanitizer 旗标：`clang-dev-asan` 预设不钉开关，
让 AUTO 插桩其 Ninja Multi-Config 树的 Debug 配置（`cmake --build
--preset clang-dev-asan` 构建该配置）；`clang-dev` 钉住
`CCG_USE_ASAN=OFF`（开关状态而非旗标——旗标只来自上面的选项逻辑）。

运行时 DLL：被插桩的可执行文件动态链接 ASAN 运行时（本 ABI 上为
`libclang_rt.asan_dynamic-x86_64.dll`）。它经 8.6 节策略发现、编译器优先：
编译器的 bin 目录（MSYS2 式前缀把它放在那里），然后编译器前缀下的
`lib/clang/<version>/lib/windows|bin`，再然后 `CCG_LLVM_ROOT` 覆盖前缀
作为附加根——覆盖绝不能遮蔽编译器自带的运行时，因为运行时 ABI 必须与
给代码插桩的编译器匹配。DLL 缺失是 WARNING，绝不致命（PATH 仍是开发期
回退）。找到时，它在 POST_BUILD 拷贝到每个被插桩可执行文件旁（与
`ccg_copy_dlls_after_build` 同一 `copy_if_different` 机制，用同一
`$<CONFIG:...>` 生成器表达式 + `COMMAND_EXPAND_LISTS` 按配置守卫，因此
在多配置树中构建未被插桩的配置时不拷贝任何东西）。

**已知第三方交互（正当的缓解措施，探针实测）。** 在 ASAN 下，libclang
的 SEH 崩溃恢复会拦截 ASAN 运行时良性的第一机会访问违例（其惰性影子页
提交）并误报为 `CXError_Crashed`（"libclang: crash detected during
parsing"）——这是未插桩的 `libclang.dll` 跑在已插桩进程内的产物，不是
项目内存错误（从不产生 ASAN 报告；cdb 显示被吞掉的异常正是 ASAN 运行时
自己的影子内存写，且关闭恢复后同一解析成功）。因此插桩的测试运行经测试
的 `ENVIRONMENT_MODIFICATION`（cmake/CcgUtils.cmake 的
`ccg_test_path_mods`，仅在 `CCG_USE_ASAN` 激活时应用）设置
`LIBCLANG_DISABLE_CRASH_RECOVERY=set:1`；ASAN 自己的异常处理器仍处理那些
访问违例。

**生成器限制（探针实测）。** 本项目已验证的多配置情形是
`Ninja Multi-Config`，与 `import std` 模块构建的 Ninja 家族约束一致。普通
Visual Studio 生成器探针（`cmake -G "Visual Studio 17 2022"`）在本机上
于任何模块问题之前即失败，报
`Generator "Visual Studio 17 2022" could not find any instance of Visual
Studio.`——未安装任何 VS 实例；无论如何，Ninja 家族仍是此处唯一受支持
的生成器家族。

### 8.8 clang-tidy 门禁（`tidy` 目标，cmake/CcgTidy.cmake）

检查集位于源码根的 `.clang-tidy`，改编自一个同级项目的配置（唯一改动：
`HeaderFilterRegex` 匹配本树的 `cpp-code-graph/(src|tests)/` 而非那个项目
的源目录——第三方与运行历史/构建树永不匹配）。一条命令经构建树的
`compile_commands.json` 跑遍每个项目模块单元（`src/**/*.ixx`、
`tests/*.ixx`）：

```
cmake --build --preset clang-dev --target tidy
```
该目标先重建每个拥有模块的目标（`cg_core`、`ccg`、测试可执行文件——由
`ccg_module_target` 以 `CCG_MODULE_TARGETS` 全局属性跟踪），因为
clang-tidy 解析模块单元时要读取这些目标产出的 BMI；随后 runner
（cmake/ccg-tidy.cmake）以 `-p <build dir>` 调用一个 clang-tidy，并对任何
项目代码发现项让目标 FAIL（clang-tidy 本身对纯警告退出 0），附按检查的
汇总。clang-tidy 23 在该模式下原生解析具名模块单元：编译数据库中每单元
的 `@<unit>.modmap` 响应文件携带 `-x c++-module` + `-fmodule-file=`
旗标，因此 `import std;`、项目模块与 sqlpp23 模块 BMI 全部可解析
（`-extra-arg=-Wno-unused-command-line-argument` 抑制
`-fmodule-output` 在 syntax-only 模式下制造的驱动器噪声）。依赖再分析由
runner 去重。

二进制发现遵循 8.6 节策略的 ASAN 运行时变体：先编译器自己的 bin 目录，
然后默认搜索，再然后 `CCG_LLVM_ROOT` 覆盖的 bin 作为附加提示——编译器
前缀永远获胜，因为 BMI 有版本锁（另一个 LLVM 构建的 clang-tidy 读不了
本编译器的 .pcm 文件）；预置 `CCG_CLANG_TIDY_EXECUTABLE` 可跳过搜索。
运行有意设计为串行：本工具链的 clang-tidy（23.1.3）没有原生 `-j`
工作进程选项（探针实测：被拒绝并提示 "Did you mean '-h'?"），而随附的
`run-clang-tidy` 包装器会给门禁添加 python 依赖。

## 9. DLL 运行时策略与按库（per-db）的 LLVM 引擎

已验证事实：任何真实 C++ 可执行文件在 OS 组件之外
恰好只需要 `libc++.dll`（1.55 MB，msys2 clang64 的 bin 目录）；sqlite3
静态链接（零 sqlite DLL 导入——已在导入表层面验证）。**ccg.exe 完全不
导入 libclang**（既定设计）：LLVM 引擎——其解析产出某个数据库的那个
libclang——是按库的世界观，且是运行时加载的（cg.clang.loader：Windows 上
`LoadLibraryExW(..., LOAD_WITH_ALTERED_SEARCH_PATH)` / POSIX 上
`dlopen(RTLD_NOW|RTLD_LOCAL)`），经 cg.clang.capi 中的 clang-c 分发表调度
（其他任何地方零直接 clang-c 调用；每个可执行文件的导入表都验证了这一
缺席）。

- 引擎解析顺序：数据库的 `llvm-path` 配置（`ccg config set llvm-path
  <prefix-or-libclang-file>`），然后 `--llvm-root`，然后 `$CCG_LLVM_ROOT`，
  然后默认发现——PATH 上的 clang 驱动器点名工具链，其根提供
  `libclang.dll`/`libclang.so`（在驱动器旁边以及 `bin/` 或 `lib/` 之下；
  PATH 条目可能带尾部分隔符或点）。解析不到任何东西的已配置根、或加载
  失败的库，是点名旋钮的可行动错误——绝不静默回退到另一个引擎。
  `--llvm-root` 是会话级覆盖，仅当数据库未钉引擎时生效：数据库的
  `llvm-path` 永远压过旗标，败给钉住的旗标会以一条说明公布（习惯性旗标
  不得静默改变刻意钉住的数据库的世界观）。
- 补充驱动器跟随引擎（一个世界观 = 一个安装）：驱动器解析先锚定在已加载
  库的位置——其目录，然后按 `bin/` 与 `lib/` 布局取安装前缀——之后才回退
  到 `$CCG_LLVM_ROOT` 与 PATH（这些回退为仅有引擎的布局而存在，例如旁边
  没有驱动器的孤立 `libclang.dll`）。`--llvm-root` 覆盖仅当旗标确实选中了
  引擎时才引导驱动器；补充信息绝不静默描述与正在解析的安装不同的另一个
  安装。两条顺序不变量在每条路径上保证这一点（已审计）：引擎在本次运行
  中第一个驱动器消费者之前被解析并加载——包括自构建的每 TU 基线向量，
  它们由被钉住/被旗标选中的引擎自己的驱动器构建——并且 cg.clang.driver
  中的驱动器缓存以 `loaded_clang_library_path()` 为键，因此静息态切换引擎
  （一个进程索引多个数据库，例如 `ccg msbuild --all`）会重新解析驱动器，
  而不是沿用上一个引擎的补充信息。
- 引擎身份与 argv 摘要、TU 集合增量并列为索引新鲜度的第三支柱：每次索引
  运行把解析出的库路径 + 已加载引擎的版本串记入 meta（`llvm_engine_path`、
  `llvm_version`）；解析出的引擎在任一值上不同则强制全量（世界观变更）。
  特性之前的数据库没有记录的身份——引擎未知——因此升级后第一次运行重
  解析一次，然后记录。
- 生命周期：第一个 `cg.clang::Index`（或显式 CLI 加载）完成解析 + 加载；
  句柄在进程生命周期内保持映射——有意地永不释放，因为 libclang 的静态
  初始化器会安装进程级全局崩溃恢复状态（Windows 上的向量化 SEH 处理器），
  FreeLibrary/dlclose 不会撤除它：实测的加载/卸载/重载循环在启用崩溃恢复
  时让下一次解析段错误，只有 `LIBCLANG_DISABLE_CRASH_RECOVERY=1` 才正常。
  Index 对象存活期间拒绝在进程中途切换引擎；静息态切换保持前一个句柄
  映射（其处理器入口仍指向已映射代码，因此多份并存无害）。
- 运行时其余部分的策略：**开发用 PATH，打包用选择性拷贝。**
  - 开发构建、ctest 与工作流 shell 依赖工具链 bin 目录（例如 msys2
    clang64 的 bin 目录）已在 PATH 上（已在工作流环境中验证）。测试注册
    还经 `ENVIRONMENT_MODIFICATION` 前置该目录，因此 ctest 无论调用方
    shell 如何都稳健——这同时喂给默认引擎发现。
  - `CCG_COPY_RUNTIME_DLLS=ON`（默认 OFF）启用 POST_BUILD 的
    `copy_if_different`，把 ccg 自己的运行时（`libc++.dll` 等）拷到每个
    可执行文件旁，得到可发布、独立可运行的 `ccg.exe`——绝不默认开启。
    以前的 ~194 MiB libclang.dll + libLLVM-* 闭包有意不再拷贝：引擎按
    数据库解析，绝不通过可执行文件旁的文件。
  - ASAN 运行时 DLL 是唯一常设例外（8.7 节）：凡 `CCG_USE_ASAN` 给某次
    构建插桩，约 0.6 MiB 的 `libclang_rt.asan_dynamic-x86_64.dll` 总会拷
    到每个被插桩可执行文件旁（按配置守卫），因为没有它，加载器会在启动时
    杀死进程，PATH 查找来不及帮忙。
  - sqlite3 在两种模式下都保持静态（经济性）。
  - 逃生通道保留在文档中、不接线：`-static-libstdc++` 可把 libc++ 并入
    exe，万一将来需要完全自包含的产物。

### 崩溃转储防护（进程级）

`main` 的第一行（先于一切 ccg 初始化）读一次 `CCG_CRASH_DUMP_DIR`：
有值才武装全套崩溃回调——零配置等于零行为变化，没有值时崩溃行为与
不挂载逐位一致。武装内容：dbghelp 预加载（崩溃现场再加载要过
loader lock 且堆可能已坏）、常驻转储线程、`SetUnhandledExceptionFilter`
（主拦截层）、`signal(SIGABRT)` 与 `std::set_terminate`（abort 家族的
唯一出口与 terminate 双保险）、主线程 `SetThreadStackGuarantee`；MSVC
侧另挂纯虚调用与 UCRT 非法参数处理器（后者不挂则默认走快速失败）。
MSVC 与 clang64（LLVM-mingw 纯 Win32 程序，无 POSIX 仿真层）双构建的
崩溃形态矩阵全部实测通过（见 tests/ 的 ccg_crashdump_matrix 闸门）；
矩阵覆盖面含 MSVC 的 Debug 配置——Debug 版 CRT 的 `abort()` 默认先弹
模态消息框，挂载侧须同时清 `_CALL_REPORTFAULT` 与 `_WRITE_ABORT_MSG`
两个行为位，abort 才静默进入 SIGABRT 处理器；同因的非法参数校验在进
处理器之前还前置一个默认模态的 `_CRT_ASSERT` 断言报告，须改道非模态
通道。

- **为什么是专用转储线程**：栈溢出时崩溃线程只剩残栈——探针实测
  单线程原地写转储（含 dbghelp 预加载、栈余量预留两种救法）五连
  全部只留 0 字节文件；改为崩溃线程只发一个事件、由栈完好的常驻
  线程写盘后十连全成——CrashPad/Breakpad 的同款架构。崩溃线程侧的
  处理逻辑压到几个系统调用（原子闸 + `SetEvent`）；30 秒回执上限防
  写盘卡死留下僵尸进程；并发崩溃由原子闸收敛——多线程同秒崩溃
  恰好产生一份转储（multiav 竞态十连实测）。写完以原异常码
  `TerminateProcess`，退出码语义与不挂载时一致。
- **可拦面（如实定界，不夸大）**：拦得住——访问违例、栈溢出、堆
  破坏（0xC0000374）、未捕获 C++ 异常（0xE06D7363）、abort、纯虚
  调用、UCRT 非法参数（须挂处理器，不挂则属快速失败）；拦不住——
  快速失败族（`__fastfail` / `RaiseFailFastException`、MSVC 栈保护
  cookie 校验失败），它们按 Windows 设计绕过一切用户态回调（实测
  连向量化异常处理器也拦不到），退出码 0xC0000409/0xC0000602。
  退出码口径：多数形态以原系统异常码终止；纯虚调用与 UCRT 非法
  参数两形态本无系统异常码，以可辨析的应用自定义码终止——
  0xE0000002（纯虚）/ 0xE0000003（非法参数）。
- **与既有恢复层的边界**：`cg.util.guard` 的 run_guarded 帧内吞掉的
  first-chance 硬件故障到不了进程级过滤器（不误转储）；栈溢出不在
  其白名单、会传播过来（正确转储）；libclang 的向量化恢复层只在
  PARSE 上下文内吞异常，上下文外放行——零冲突。
- **与系统级 WER 的互补**：WER LocalDumps 走内核路径，能收快速
  失败族（应用内拦不到的那部分）；本机制收其余全部且不依赖机器
  配置。两者并存无冲突：本机制写完转储以原异常码终止，异常已被
  处理，WER 不会再介入同一次崩溃。
- **崩溃态写盘纪律**：处理器与写盘路径只用系统 API + 静态/栈上
  缓冲（CRT 堆可能已被堆破坏波及，std::string/malloc 一律禁入）；
  转储文件名 `ccg-<进程号>-<年月日>-<时分秒>.dmp`（进程号 + 秒级
  时间戳保证跨进程不撞名）；目录逐级自动创建；写失败把 GetLastError
  报到 stderr。
- **ASAN 构建**：ASAN 拦截器先于本机制报告并终止，ASAN 构建下本
  机制大概率收不到——ASAN 自己的报告即是诊断材料。

## 10. 裁决与偏差

1. **CMake 是 4.4.4 而非 4.2.1**（探针实测）。import-std 门的 UUID 必须
   在 `project()` 之前按版本条件选择：

   ```cmake
   if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.3)
       set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "f35a9ac6-8463-4d38-8eec-5d6008153e7d")
   else()
       set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")
   endif()
   ```

   随之而来的 `(experimental)` configure 警告是预期噪声。按目标的
   `CXX_MODULE_STD ON` + 显式 `CMAKE_CXX_STANDARD 23` +
   `CMAKE_CXX_STANDARD_REQUIRED ON` 仍然必需。

2. **`cg.util.json` 中的 assert 垫片**：`cpp_yyjson.hpp` 使用 `assert`
   约 70 次却不 include `<cassert>`，而 `import std;` 无法导出宏。在
   `util.json.ixx` 的 GMF 中、include 之前放

   ```cpp
   #ifndef assert
   #define assert(expr) ((void)0)   // no-op, matches NDEBUG builds
   #endif
   ```

   第三方 assert 保持禁用（等价于 release 的行为）；项目代码任何地方都
   没有 `#include <assert.h>`。

3. **测试夹具是数据，不是源码。** `tests/fixtures/**` 下的文件是
   libclang 的解析输入（传统基于 `#include` 的 C++，因此不带 `-fmodules`
   也能解析），绝不参与我们的构建编译，绝不 import。全 `.ixx` 规则适用
   于编入项目的代码。夹具作为普通数据文件提交。

4. **`app.main` 内的 `export int main()`** 是纯模块可执行文件的已验证
   模式（良性的 `-Wmain-attached-to-named-module`）。

5. **不用 git，不写项目树之外**；`make` 若真要用，是 msys2 usr/bin 的
   `make.exe`（仍不在 Git Bash PATH 上——两次实测）。

6. **libclang 解析基线**（TU 没有可用条目时的每 TU 基线）：`-std=c++23`
   加本安装的 clang 驱动器报告的 include 搜索列表（`-E -v` 的 stderr
   询问，8.6 节；运行时发现并缓存——绝不烘焙路径）；资源目录来自
   `-print-resource-dir`，仅当某个树需要时加入重放。首选策略是逐字重放
   compile_commands。该基线不是目录回退扫描：`ccg index` 需要编译数据
   库，没有就报错退出（C++ 没有有意义的简单回退；错误的图比缺失的图更
   糟）。基线只服务自构建中被改路由绕开重放的单元（例如 cl 路由的模块
   单元）与 PCH 影子路径。

7. **按文件唯一抽取已结案——永远不要再规划。** 混合方案"对每个 TU 先做
   轻量计数遍历，再按唯一文件抽取一次符号/边"曾在来自四个项目的 14 个
   真实 TU 上实测（相同解析选项、`DetailedPreprocessingRecord |
   KeepGoing`、3 次热重复 + 1 次冷遍历）并被否决。相对
   `parse + extract` 的完整抽取模拟实测开销：两个真实参考语料库上 5.0%
   与 6.7%，ccg 自己的树上 8.1%。轻量遍历本身热态花费该抽取的 28%、冷态
   33%，却不产出任何图数据，因此最好情形的净节省约 0.64×E，即索引墙钟
   时间的 3–5%——而代价是改变 `occurrences` 语义，它在条件编译下的正确性
   无法证明。解析占墙钟的 92–95%；精力应花在那儿（PCH 支持），不是这儿。
   模块密集仓库（解析由 BMI 供给、抽取占墙钟 52%）是唯一实测到的例外，
   它们同样证明不了这个语义风险值得冒。若将来重访抽取工作，唯一可接受
   的形式是为 USR / 拼写 / 类型拼写查找做 `(file, offset)` 缓存，且不
   触碰出现计数。

## 11. 测试策略

- 每层一个测试可执行文件（`cg_test_*`），各是纯模块 exe，以
  `export int main()` 驱动 `cg.test.harness`。ctest 经预设运行。
- 纯层（util、带 `:memory:` 的 db、基于夹具的 extract、基于预置数据的
  存储的 graph、cli parse/render）直接做单元测试；端到端路径由 ctest
  `add_test` 调用真实 `ccg.exe` 来测试（参数列表、
  `PASS_REGULAR_EXPRESSION`、退出码 1 情形的 `WILL_FAIL`）。
- 夹具迷你项目（M3，传统 C++ 数据文件）：一个小型头/源文件树，覆盖 v1
  的每种节点类别与边类型，包括需要第 1 级解析的跨文件调用、一对重载、
  一个虚函数重写、一个模板 + 偏特化，以及放在单独夹具目录中的 include
  环，使普通夹具保持无环。
- 参考语料库（只读！）是 M3/M4 规模的冒烟测试：手工索引一个子集（例如
  某个目录的 TU）；完整 ~660 TU 索引是可选浸泡测试，绝不做 CI 门禁。

## 12. 存储经济性原则（完整细节见 SCHEMA.md）

每个重复字节都驻留（`str` 表）：名字、限定键、命名空间、文件路径、
include 目标。结构化事实放类型化列，绝不用 JSON（JSON 只用于真正稀疏的
按类别载荷：`param_types`、`template_params`）。枚举是微型 INTEGER id，
由预置数据的查找表支撑。作用域关系（成员、命名空间）是列，不是边。每个
不同调用点一行边，跨 TU 由 UNIQUE + 出现计数器去重。10 万行代码库的
行数与字节预算：见 SCHEMA.md §6。
