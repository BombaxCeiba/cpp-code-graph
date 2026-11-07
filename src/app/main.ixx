module;
#include "../db/db.tables.h"
#include "../msbuild/capture.tables.h"

/**
 * @module app.main
 *
 * @brief 可执行入口点：通往完整 CLI（cg.cli）的薄蹦床。
 *
 * 全部参数解析、输出与退出码都住在 cg.cli 里。
 *
 * @internal
 * db.tables.h 放在全局模块片段：本单元会抵达 cg.db（直接或经再导出的
 * 子模块），MSVC 要求 sqlpp23 的表模型定义在此可见，模块才能合并其记录
 * 的模板特化（缘由详见 db.tables.h）。
 */
export module app.main;

import std;
import cg.cli;
import cg.util.crashdump;

// NOLINTNEXTLINE(clang-diagnostic-main-attached-to-named-module) module-unit 的
// main 是刻意为之：工程每个源文件都是命名模块单元；裸 TU
// 的 main 会破坏这条全模块策略
export int main(int argc, char* const* argv)
{
    // 崩溃转储防护先于一切 ccg 初始化挂上（仅当 CCG_CRASH_DUMP_DIR
    // 有值时它才武装回调；POSIX 上为空操作）。
    cg::util::crashdump::install_from_env();
    return cg::cli::run_main(argc, argv);
}
