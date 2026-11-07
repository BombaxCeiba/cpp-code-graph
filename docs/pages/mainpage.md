# cpp-code-graph（ccg）{#mainpage}

@brief C++23 代码知识图谱引擎——以命名模块组织的 clang/libclang 前端、图引擎与 SQLite 存储一体的小型工具链。

- 模块总览见「Modules」页：每个 `cg.*` 模块的中文简介挂在各 `.ixx` 的 `export module` 声明前（`@module` 块）。
- 接口契约（`@brief`/`@param`/`@return`）渲染于各模块页；实现内幕（`@internal` 段）不进本站，仅存源码。
- 设计文档在仓库 `docs/en/` 目录（[DESIGN.md](../en/DESIGN.md)、[SCHEMA.md](../en/SCHEMA.md)、[MSVC.md](../en/MSVC.md) 等）。
