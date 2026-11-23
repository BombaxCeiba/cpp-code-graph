module;
#include "../db/db.tables.h"

/**
 * @module cg.graph.traverse
 *
 * @brief cg.db 之上的读侧图算法：带最短深度标注的 BFS 调用者/被调用者、
 *        带风险阶梯的影响面（直接/传递计数加受影响者的扇入）、最短调用
 *        路径 trace、上下文分组装配——以上均支持虚方法的覆盖族合并
 *        ——变更集传播（ccg changes）、包含图环检测（ccg check
 *        --cycles），以及调用闭包与模块级聚合图（ccg graph 导出）。
 *
 * @internal
 * 纯查询：唯一写入方是 cg.graph.build，它导入本模块以复用包含解析器
 * （两侧共用同一套规则，环检测使用其中的严格无歧义子集）。覆盖族 =
 * 被查询方法本身加上经 overrides 边连通的全部方法。db.tables.h 置于
 * 全局模块片段：本单元导入 cg.db，MSVC 要求 sqlpp23 表模型定义在此
 * 可见，以合并模块记录的模板特化（完整机理见 db.tables.h）。
 */
export module cg.graph.traverse;

import std;
import cg.util;
import cg.db;

export import cg.graph.traverse.model;
export import cg.graph.traverse.dispatchidx;
export import cg.graph.traverse.include;
export import cg.graph.traverse.walk;
export import cg.graph.traverse.queries;
export import cg.graph.traverse.context;
export import cg.graph.traverse.changes;
