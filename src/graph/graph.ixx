module;
#include "../db/db.tables.h"

/**
 * @module cg.graph
 *
 * @brief 图层伞形模块：再导出批量导入器与查询侧算法的全部表面。
 *
 * cg.graph.build 是批量导入器（编译数据库 -> 经 cg.db 仓储写入
 * SQLite），cg.graph.traverse 承载查询侧算法。
 *
 * @internal
 * 两个子模块在此再导出（历史上曾是分区，后改制为接口模块），使伞形导入
 * 保持其历史表面不变。db.tables.h 置于全局模块片段：本单元可达 cg.db
 * （直接或经再导出的子模块），MSVC 要求 sqlpp23 表模型定义在此可见，
 * 以合并模块记录的模板特化（完整机理见 db.tables.h）。
 */
export module cg.graph;

export import cg.graph.build;
export import cg.graph.traverse;
