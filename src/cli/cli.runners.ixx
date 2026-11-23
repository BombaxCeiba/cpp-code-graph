module;
#include "../db/db.tables.h"

/**
 * @module cg.cli.runners
 *
 * @brief CLI 运行器的共享索引脊柱：引擎准备、命令清单解析（编译数据
 *        库加载 / 模块自构建路由）、cl 模式模块路由、以及
 *        `ccg index` / `ccg update` / `ccg msbuild` 共享的索引尾部。
 *
 * @internal
 * 曾按分区语法拆分（cg.cli:runners）；分区语法退役后无 BMI 实现单元
 * 一并退役，拆分时从 cli.index.cpp 吸收了定义。消费者：cg.cli.index /
 * cg.cli.update / cg.cli.msbuild。定义为接口驻留（inline）——把运行器
 * 函数体留在无 BMI 实现单元里的 C2801 时代要求已过时：规避补丁对决定性的
 * std BMI 宿主做了影子补丁（MSVC 移植注记），而"仅声明"的安排只是为
 * 绕开该缺陷的体量敏感性。db.tables.h 置于全局模块片段：本单元导入
 * cg.db，MSVC 要求 sqlpp23 表模型定义在此可见，以合并模块记录的模板
 * 特化（完整机理见 db.tables.h）。
 */
export module cg.cli.runners;

import std;
import cg.util;
import cg.util.cstd;
import cg.db;
import cg.clang;
import cg.graph;
import cg.cli.parse;
import cg.cli.render;
import cg.cli.layout;

export import cg.cli.runners.engine;
export import cg.cli.runners.route;
export import cg.cli.runners.resolve;
export import cg.cli.runners.tail;
