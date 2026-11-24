/**
 * @module cg.db.query
 *
 * @brief 存储层的读侧：分层符号查找（层 1-3：精确、限定后缀、子串；
 *        命中层合并）、符号/文件行物化辅助、caller/callee 边聚合（每对
 *        MIN(site_line)、SUM(occurrences)）、对间原始边、取址证据行、
 *        记录成员、extends 关系、统计与表计数。
 *
 * @internal
 * 热形状读语句（edges_from/to/of）走 GraphStore 的预编译语句表；其
 * 余查询仍是每次调用现场构建的类型化 sqlpp23 语句。批量 IN 形状因参
 * 数个数可变无法进固定形状的预编译表，按块现场构建。
 */
export module cg.db.query;

export import cg.db.query.lookup;
export import cg.db.query.batch;
export import cg.db.query.members;
export import cg.db.query.stats;
export import cg.db.query.dumpkeys;
