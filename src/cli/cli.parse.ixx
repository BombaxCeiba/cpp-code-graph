module;
#include "../db/db.tables.h"

/**
 * @module cg.cli.parse
 *
 * @brief argv -> Invocation：CLI 规格文档 第 2 节完整命令矩阵的解析。
 *
 * 共享旗标、逐命令校验（未知选项 / 缺操作数 / 坏值 -> UsageError ->
 * 退出 2）、逐命令默认值、以及随退出 2（或 help）打印的用法文本。
 *
 * @internal
 * db.tables.h 置于全局模块片段：本单元导入 cg.db，MSVC 要求 sqlpp23
 * 表模型定义在此可见，以合并模块记录的模板特化（完整机理见
 * db.tables.h）。
 */
export module cg.cli.parse;

import std;
import cg.util;
import cg.util.cstd;
import cg.db;
import cg.graph;

export import cg.cli.parse.model;
export import cg.cli.parse.usage;
export import cg.cli.parse.opts;
export import cg.cli.parse.parse;
