module;
#include "../db/db.tables.h"
#include "../msbuild/capture.tables.h"

/**
 * @module cg.cli.commands
 *
 * @brief 查询侧命令运行器与共享存储助手：解析后的 Invocation、
 *        cg.db / cg.graph 与渲染器之间的胶合层。
 *
 * 持有退出码契约（0 成功、1 未找到/歧义/运行时错误、2 用法）
 * 与符号名解析层级（含第 4 层"你是不是想找"建议）。
 *
 * @internal
 * index/update/msbuild/config/hook 运行器住在各自的接口模块
 * （cg.cli.index/update/msbuild/config/hook；共享脊柱与存储/扫描管道
 * 在 cg.cli.runners——模块布局定稿后）；下方的 dispatch 恰好导入那些模块，
 * 即运行侧所需的扇入点。本单元刻意不导入 cg.clang。db.tables.h 置于
 * 全局模块片段：本单元导入 cg.db，MSVC 要求 sqlpp23 表模型定义在此
 * 可见，以合并模块记录的模板特化（完整机理见 db.tables.h）。
 */
export module cg.cli.commands;

import std;
import cg.util;
import cg.db;
import cg.graph;
import cg.msbuild;
import cg.cli.parse;
import cg.cli.render;
import cg.cli.runners;
import cg.cli.layout;
import cg.cli.index;
import cg.cli.update;
import cg.cli.msbuild;
import cg.cli.config;
import cg.cli.hook;

export import cg.cli.commands.matchers;
export import cg.cli.commands.targets;
export import cg.cli.commands.runs;
export import cg.cli.commands.entry;
