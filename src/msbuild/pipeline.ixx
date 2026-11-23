module;
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
// read_wrap_log 经 sqlpp23 读 capture.db：表模型必须以经典头形态在本
// 单元全局片段可见（MSVC 上定义进命名模块再跨模块导入的表模板会让
// IFC 读取器崩溃），故 include capture.tables.h。残留的 sqlite3 C API
// 面是白名单接缝：SQLITE_OPEN_* 旗标宏（宏不跨模块边界）与
// sqlite3_busy_timeout（连接器无超时旋钮），include 面由 cg_core 的
// PUBLIC cg::sqlite3 链接传递；extern "C" 声明重复无 ODR 风险。
#include <sqlite3.h>

#include "capture.tables.h"

/**
 * @module cg.msbuild.pipeline
 *
 * @brief `ccg msbuild` 的发现、编排与注册表。
 *
 * 定位 MSBuild（vswhere、环境/选项覆盖），定位真实 cl（MSBuild
 * -getProperty:VCToolsInstallDir 加"末条路径行"规则、vswhere 兜底、
 * --cl 覆盖；横幅版本供 -fms-compatibility-version），运行喂 wrap
 * 模式的被包装 -t:Rebuild（src/msbuild/wrap.ixx），从 .sln/.vcxproj
 * 输入枚举 Configuration|Platform 对，并把逐配置注册表持久化到
 * `<root>`/.ccg/msbuild.json（ASCII 安全相对路径、原子替换）。
 *
 * @internal
 * 全部是文本 + 进程管道：图引擎零改动（既定决策——一个配置一个 db
 * 文件，互不污染）。全局模块片段说明：发现/编排半边的 Win32 头——
 * 可执行自身的定位（GetModuleFileNameW）与文件属性探测；子进程派生
 * （vswhere/MSBuild/cl 讯问、被包装 Rebuild）已收敛到 cg.util.platform
 * 的捕获原语 run_child；纯文本半边（配置枚举、注册表、路径拼接、
 * 输出解析）到处可编译且有单元测试。
 */
export module cg.msbuild.pipeline;

import std;
import cg.util;
import cg.util.cstd;
import cg.msbuild.translate; // ClInvocation / CdbEntry（wrap 日志载荷类型）
import cg.msbuild.wrap;      // detail::env_wide / fs_path_from_utf8
import sqlpp23.core;
import sqlpp23.sqlite3;

export import cg.msbuild.pipeline.configs;
export import cg.msbuild.pipeline.registry;
export import cg.msbuild.pipeline.wraplog;
export import cg.msbuild.pipeline.extractargs;
export import cg.msbuild.pipeline.hostfind;
export import cg.msbuild.pipeline.treestate;
export import cg.msbuild.pipeline.outputs;
export import cg.msbuild.pipeline.extract;
