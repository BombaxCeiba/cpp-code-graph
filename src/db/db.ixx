/**
 * @module cg.db
 *
 * @brief 存储层的主接口：存储层的全部能力经此伞模块可达。
 *
 * @internal
 * 伞下为小模块划分：cg.db.tables（sqlpp23 表模型，不经本伞再导出，用到
 * 表模型名字的单元自己导入）、cg.db.schema（DDL 文本、kind/type 字典、
 * 行类型）、cg.db.store（GraphStore：连接、字符串驻留、upsert、清除、
 * 事务）、cg.db.query（分层查找、边聚合、统计）。共享值类型放在
 * cg.db.schema，另两个模块按点分名导入。
 */
export module cg.db;

export import cg.db.schema;
export import cg.db.store;
export import cg.db.query;
