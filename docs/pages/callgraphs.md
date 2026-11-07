# 调用图（ccg graph 导出）{#callgraphs}

@brief 模块级调用总图与关键入口的执行流展开——数据源是本仓库自索引数据库（.ccg/main/ccg.db 的 calls 边）。

本页各图由 `ccg graph` 命令从仓库根的自索引数据库导出，`docs` 构建目标在 Doxygen 之前自动生成到构建树的 `docs/doxygen/dot/` 目录（接线见 `cmake/Docs.cmake`）。机器未安装 graphviz（HAVE_DOT=NO）或自索引库缺失时优雅跳过，本页退化为纯文字说明。

Doxygen 的词法解析器看不懂 `import cg.x;` 进来的跨模块调用目标，原生 CALL_GRAPH 在本命名模块项目上产出为零；ccg 的 libclang 级 calls 边是唯一权威数据源，故调用图一律经此注入。

## 模块级调用总图

符号按定义文件聚合为模块（文件名词干，与命名模块名一致），calls 边按模块对去重计数为权重；权重低于 3 的弱边已降噪，节点集随之收缩到保留边的端点。

@dotfile modules.dot "模块级调用总图（calls 边按定义文件聚合，权重 ≥ 3）"

## 入口执行流：dispatch

命令分派点 `cg::cli::dispatch`（Invocation -> 运行器的扇出枢纽）起深度 2 的调用展开——第一层是全部命令运行器（run_query/run_context/run_neighbors/run_impact/run_trace/run_stats/run_changes/run_check/run_graph…），第二层进入各运行器的共享脊柱。

@dotfile dispatch.dot "dispatch 起深度 2 的执行流"

## 入口执行流：run_index

`ccg index` 运行器 `cg::cli::detail::run_index` 起深度 2 的调用展开——编译数据库发现、回退 TU 扫描与共享索引尾部的交接链路。

@dotfile run-index.dot "run_index 起深度 2 的执行流"
