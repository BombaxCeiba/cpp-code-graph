module;
#include "db.tables.h"

/**
 * @module cg.db
 *
 * @brief 存储层的主接口：存储层的全部能力经此伞模块可达。
 *
 * @internal
 * 伞下为小模块划分（原分区方案定稿）：cg.db.schema（DDL 文本、
 * kind/type 字典、表模型、行类型）、cg.db.store（GraphStore：连接、
 * 字符串驻留、upsert、清除、事务）、cg.db.query（分层查找、边聚合、
 * 统计）。共享值类型放在 cg.db.schema，另两个模块按点分名导入。
 *
 * db.tables.h 置于全局模块片段的原因：db.ixx 这个单元（直接或经再导出
 * 的子模块）到达 cg.db 时，MSVC 要求 sqlpp23 表模型定义在此可见，以便
 * 合并模块记录的模板特化（完整机理与探针矩阵结论见 db.tables.h）。
 */
export module cg.db;

export import cg.db.schema;
export import cg.db.store;
export import cg.db.query;
