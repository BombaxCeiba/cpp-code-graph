/**
 * @module cg.cli.parse
 *
 * @brief argv -> Invocation：CLI 规格文档 第 2 节完整命令矩阵的解析。
 *
 * 共享旗标、逐命令校验（未知选项 / 缺操作数 / 坏值 -> UsageError ->
 * 退出 2）、逐命令默认值、以及随退出 2（或 help）打印的用法文本。
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
