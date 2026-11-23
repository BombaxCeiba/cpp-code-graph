module;
#include "../db/db.tables.h"
#include "../msbuild/capture.tables.h"

/**
 * @module cg.cli.commands.entry
 *
 * @brief 命令入口域：查询库解析与 dispatch 主分派。
 */
export module cg.cli.commands.entry;

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
import cg.cli.commands.matchers;
import cg.cli.commands.targets;
import cg.cli.commands.runs;

export namespace cg::cli
{

    /**
     * @brief 运行一条已解析的调用。
     *
     * 抛 UsageError（-> 退出 2）与 std::exception（-> 退出 1，由
     * run_main 渲染）。
     *
     * @internal
     * 布局解析（cg.cli.layout）在此收口：一切读侧命令（查询族/stats/
     * changes/check/graph/update）先把 Invocation 解析到具体的
     * `<文件夹>`/`<配置>`/ccg.db（CCG_DB > -o > ./.ccg 发现；单配置自动选中、
     * 多配置点名候选、旧 ./ccg.db 给可行动提示）再进运行器；index/msbuild
     * 各自经 cg.cli.layout 解析自己的产出文件夹与配置。
     */
    /** @brief 把查询族布局解析结果（db/配置/根）写回调用对象。 */
    auto resolve_query_db_into(Invocation& inv) -> void
    {
        const LayoutChoice choice = layout::resolve_query_layout(inv);
        inv.db_path = choice.db;
        inv.config_name = choice.config;
        inv.layout_root = choice.root;
    }

    [[nodiscard]]
    auto dispatch(const Invocation& invocation) -> int
    {
        Invocation inv = invocation;
        switch (inv.cmd)
        {
        case Invocation::Cmd::Help:
        {
            if (!inv.positional.empty())
            {
                const auto cmd = command_of(inv.positional.front());
                std::print(
                    "{}",
                    cmd.has_value() ? usage_for(*cmd) : top_usage());
            }
            else
            {
                std::print("{}", top_usage());
            }
            return 0;
        }
        case Invocation::Cmd::Version:
            std::println("ccg {}", cg::util::version);
            return 0;
        case Invocation::Cmd::Index:
            return detail::run_index(inv);
        case Invocation::Cmd::Update:
            resolve_query_db_into(inv);
            return detail::run_update(inv);
        case Invocation::Cmd::Msbuild:
            return detail::run_msbuild(inv);
        case Invocation::Cmd::Config:
            return detail::run_config(inv);
        case Invocation::Cmd::Hook:
            return detail::run_hook(inv);
        case Invocation::Cmd::Query:
        case Invocation::Cmd::Context:
        case Invocation::Cmd::Members:
        case Invocation::Cmd::Callers:
        case Invocation::Cmd::Callees:
        case Invocation::Cmd::Impact:
        case Invocation::Cmd::Trace:
        {
            resolve_query_db_into(inv);
            cg::db::GraphStore store = detail::open_store_read(inv);
            switch (inv.cmd)
            {
            case Invocation::Cmd::Query:
                return detail::run_query(store, inv);
            case Invocation::Cmd::Context:
                return detail::run_context(store, inv);
            case Invocation::Cmd::Members:
                return detail::run_members(store, inv);
            case Invocation::Cmd::Callers:
            case Invocation::Cmd::Callees:
                return detail::run_neighbors(store, inv);
            case Invocation::Cmd::Impact:
                return detail::run_impact(store, inv);
            default:
                return detail::run_trace(store, inv);
            }
        }
        case Invocation::Cmd::Stats:
        {
            resolve_query_db_into(inv);
            cg::db::GraphStore store = detail::open_store_read(inv);
            return detail::run_stats(store, inv);
        }
        case Invocation::Cmd::Changes:
        {
            resolve_query_db_into(inv);
            cg::db::GraphStore store = detail::open_store_read(inv);
            return detail::run_changes(store, inv);
        }
        case Invocation::Cmd::Check:
        {
            resolve_query_db_into(inv);
            cg::db::GraphStore store = detail::open_store_read(inv);
            return detail::run_check(store, inv);
        }
        case Invocation::Cmd::Graph:
        {
            resolve_query_db_into(inv);
            cg::db::GraphStore store = detail::open_store_read(inv);
            return detail::run_graph(store, inv);
        }
        }
        throw UsageError{"unhandled command", top_usage()};
    }
} // namespace cg::cli
