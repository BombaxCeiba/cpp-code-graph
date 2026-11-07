module;
#include "../db/db.tables.h"
#include "../msbuild/capture.tables.h"

/**
 * @module cg.cli
 *
 * @brief ccg 的查询 CLI：图数据库之上的完整命令矩阵。
 *
 * @internal
 * 本伞形模块再导出整个 cg.cli 家族——parse/render/commands、共享脊柱
 * cg.cli.runners、以及运行器模块 cg.cli.index/update/msbuild/config/hook
 * （曾为分区、后为实现单元——现全部是接口模块）——并持有
 * run_main：argv -> Invocation -> cg.cli.commands 的分派，带顶层
 * try/catch 与退出码映射（0 成功、1 未找到/运行时错误、2 用法错误）。
 * 扇入放在此处，使任何 cg.cli 子模块都不必导入一个会反向导入自己的
 * 兄弟模块。db.tables.h 置于全局模块片段：本单元可达 cg.db（直接或经
 * 再导出的子模块），MSVC 要求 sqlpp23 表模型定义在此可见，以合并模块
 * 记录的模板特化（完整机理见 db.tables.h）。
 */
export module cg.cli;

export import cg.cli.parse;
export import cg.cli.render;
export import cg.cli.layout;
export import cg.cli.commands;
export import cg.cli.runners;
export import cg.cli.index;
export import cg.cli.update;
export import cg.cli.msbuild;
export import cg.cli.config;
export import cg.cli.hook;

import std;
import cg.util;
import cg.msbuild;

export namespace cg::cli
{

    namespace json = cg::util::json;

    /**
     * @brief 来自 main() 的入口；args 不含 argv[0]。自行打印一切输出并返回
     *        进程退出码。
     *
     * @internal
     * 包装模式（MSBuild 提取，cg.msbuild.wrap）：CCG_CLWRAP_LOG 已设置 =
     * MSBuild 把本进程当作 CL 工具调用；记录调用、转发给真正的编译器、
     * 以其退出码退出。环境变量缺席 = 普通 CLI（单次环境探测，别无副作用）。
     * 该检查先于任何 argv 访问：包装路径需要宽字符命令行而非此 span。
     * JSON 模式把错误报在 STDOUT，使调用方只解析一条流；
     * 退出码两种方式下不变。
     */
    [[nodiscard]]
    inline auto run_main(std::span<const std::string> args) -> int
    {
        if (const std::optional<int> wrap_exit = cg::msbuild::try_wrap_main())
        {
            return *wrap_exit;
        }
        Invocation inv;
        try
        {
            inv = parse_args(args);
        }
        catch (const UsageError& e)
        {
            std::println(std::cerr, "ccg: {}", e.what());
            std::print(std::cerr, "{}", e.usage());
            return 2;
        }
        try
        {
            return dispatch(inv);
        }
        catch (const UsageError& e)
        {
            std::println(std::cerr, "ccg: {}", e.what());
            std::print(std::cerr, "{}", e.usage());
            return 2;
        }
        catch (const std::exception& e)
        {
            if (inv.json)
            {
                json::Object o;
                o.set("error", e.what());
                o.set("partial", false);
                std::println("{}", json::stringify(std::move(o)));
            }
            else
            {
                std::println(std::cerr, "ccg: error: {}", e.what());
            }
            return 1;
        }
    }

    /** @brief 经典 (argc, argv) 入口的便利重载。 */
    [[nodiscard]]
    inline auto run_main(int argc, char* const* argv) -> int
    {
        std::vector<std::string> args;
        args.reserve(static_cast<std::size_t>(argc > 1 ? argc - 1 : 0));
        for (int i = 1; i < argc; ++i)
        {
            args.emplace_back(argv[i]);
        }
        return run_main(args);
    }

} // namespace cg::cli
