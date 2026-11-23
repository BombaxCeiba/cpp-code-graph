module;
#include "../db/db.tables.h"

/**
 * @module cg.cli.render
 *
 * @brief 每个命令的输出渲染器：stdout 上的人类可读表格与经
 *        cg.util.json 写门面构建的 --json 载荷。
 *
 * CLI 规格文档 第 2 节的示例即格式契约；每个 JSON 载荷在适用处携带
 * error/partial/truncated 字段（第 1 节）。truncated 只表"结果被截
 * 断"；--depth 上限处的 BFS 前沿信号由 deeperEdgesExist 独立承载
 * （语义见 deeper_note 的三要素注释）。
 *
 * 渲染器按命令族分域在点分子模块实现，本单元聚合再导出，
 * 消费者经 import cg.cli.render 取得全部渲染器，可达面不变。
 *
 * @internal
 * db.tables.h 置于全局模块片段：本单元导入 cg.db，MSVC 要求 sqlpp23
 * 表模型定义在此可见，以合并模块记录的模板特化（完整机理见
 * db.tables.h）。
 */
export module cg.cli.render;

import std;
import cg.util;
import cg.db;
import cg.graph;
import cg.analysis.model;
import cg.cli.parse;

export import cg.cli.render.base;
export import cg.cli.render.dispatch;
export import cg.cli.render.symbol;
export import cg.cli.render.context;
export import cg.cli.render.impact;
export import cg.cli.render.stats;
export import cg.cli.render.changes;
