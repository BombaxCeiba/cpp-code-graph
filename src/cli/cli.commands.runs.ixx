module;
#include "../db/db.tables.h"
#include "../msbuild/capture.tables.h"

/**
 * @module cg.cli.commands.runs
 *
 * @brief
 * 命令运行器域：query/context/members/neighbors/impact/trace/stats/changes/check/graph
 * 的执行体与 stdin 路径补充。
 */
export module cg.cli.commands.runs;

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

export namespace cg::cli
{

    namespace detail
    {

        /** @brief `ccg query` 运行器：按名查符号。 */
        auto run_query(cg::db::GraphStore& store, const Invocation& inv) -> int
        {
            const std::string& name = inv.positional.front();
            if (inv.implements)
            {
                // --implements：positional 当接口/基类，列 extends 派生树
                // （BFS 防环，深度列标注层级；与虚函数 override 家族无关）。
                const auto target = resolve_unique_target(store, inv, name);
                if (!target.has_value())
                {
                    return 1;
                }
                std::vector<render::ImplementRow>          rows;
                std::vector<cg::db::SymId>                 visited{target->id};
                std::vector<std::pair<cg::db::SymId, int>> pending{
                    {target->id, 1}};
                while (!pending.empty())
                {
                    const auto [id, depth] = pending.front();
                    pending.erase(pending.begin());
                    for (const cg::db::EdgeRow& e : cg::db::deriving(store, id))
                    {
                        if (std::ranges::find(visited, e.src) != visited.end())
                        {
                            continue;
                        }
                        visited.push_back(e.src);
                        if (const auto row = cg::db::get_symbol(store, e.src))
                        {
                            rows.push_back({*row, depth});
                        }
                        pending.emplace_back(e.src, depth + 1);
                    }
                }
                if (inv.json)
                {
                    std::println("{}", render::implements_json(*target, rows));
                }
                else
                {
                    std::print("{}", render::implements_text(*target, rows));
                }
                return 0;
            }
            const cg::db::FindResult found =
                cg::db::find_symbols(store, name, to_filter(inv));
            if (found.tier == 0)
            {
                std::vector<std::string> suggestions;
                if (!inv.kind.has_value() && !inv.file_substring.has_value())
                {
                    suggestions = suggest_similar(store, name, 5);
                }
                emit_not_found(inv, name, suggestions);
                return 1;
            }
            if (inv.json)
            {
                std::println(
                    "{}",
                    render::query_json(name, found.tier, found.matches));
            }
            else
            {
                std::print(
                    "{}",
                    render::query_text(name, found.tier, found.matches));
            }
            return 0;
        }

        /** @brief `ccg context` 运行器：360 度符号视图。 */
        auto run_context(cg::db::GraphStore& store, const Invocation& inv)
            -> int
        {
            const auto target =
                resolve_unique_target(store, inv, inv.positional.front());
            if (!target.has_value())
            {
                return 1;
            }
            cg::graph::ContextView view = cg::graph::context(
                store,
                target->id,
                /*merge_family=*/!inv.no_merge,
                inv.dispatch_mode);
            // --exclude-glob：结果端剔除（对端符号定义路径命中的边；库不动）。
            if (!inv.exclude_globs.empty())
            {
                const auto drop = [&](const cg::graph::ContextEdge& e)
                { return path_excluded(inv.exclude_globs, e.other.file); };
                std::erase_if(view.incoming, drop);
                std::erase_if(view.outgoing, drop);
            }
            // 取址证据（消费簇 1 契约）：与 callers 同款盲区标注——经函数/成员
            // 指针的调用 incoming 天然看不见，有证据行时点破。
            const std::vector<cg::db::TypeHintRow> addr =
                address_taken_sites(store, *target, view.family);
            if (inv.json)
            {
                std::println("{}", render::context_json(view, store, addr));
            }
            else
            {
                std::print("{}", render::context_text(store, view, addr));
            }
            return 0;
        }

        /**
         * @brief `ccg members` 运行器：记录成员清单（外场反馈要求成员面
         *        不用回头读头文件）。
         *
         * @internal 非 record 目标是可行动错误（点破 kind，附 --kind 习惯）；
         * record 无成员是合法答案（空清单、退出 0）。--kind/--file 沿用符号
         * 命令的过滤语义（to_filter 同判据）。
         */
        auto run_members(cg::db::GraphStore& store, const Invocation& inv)
            -> int
        {
            const auto target =
                resolve_unique_target(store, inv, inv.positional.front());
            if (!target.has_value())
            {
                return 1;
            }
            if (target->kind != cg::db::SymbolKind::Record)
            {
                emit_error(
                    inv,
                    std::format(
                        "{} is a {}, not a record; members lists a "
                        "record's methods and fields (resolve the "
                        "record with --kind record)",
                        target->qualified_name,
                        cg::db::name_of(target->kind)));
                return 1;
            }
            const cg::db::SymbolFilter     filter = to_filter(inv);
            std::vector<cg::db::MemberRow> rows;
            for (cg::db::MemberRow& m :
                 cg::db::members_with_origin(store, target->id))
            {
                if (filter.kind.has_value() && m.symbol.kind != *filter.kind)
                {
                    continue;
                }
                if (filter.file_substring.has_value()
                    && m.symbol.file.find(*filter.file_substring)
                           == std::string::npos)
                {
                    continue;
                }
                rows.push_back(std::move(m));
            }
            if (inv.json)
            {
                std::println("{}", render::members_json(*target, rows));
            }
            else
            {
                std::print("{}", render::members_text(*target, rows));
            }
            return 0;
        }

        /** @brief `ccg callers` / `ccg callees` 运行器。 */
        auto run_neighbors(cg::db::GraphStore& store, const Invocation& inv)
            -> int
        {
            const bool up = inv.cmd == Invocation::Cmd::Callers;
            const auto target =
                resolve_unique_target(store, inv, inv.positional.front());
            if (!target.has_value())
            {
                return 1;
            }
            cg::graph::NeighborsResult result =
                up ? cg::graph::callers(
                         store,
                         target->id,
                         inv.depth,
                         /*merge_family=*/!inv.no_merge,
                         inv.dispatch_mode)
                   : cg::graph::callees(
                         store,
                         target->id,
                         inv.depth,
                         /*merge_family=*/!inv.no_merge,
                         inv.dispatch_mode);
            // --exclude-glob：结果端剔除（定义路径命中的条目；索引与库不动）。
            if (!inv.exclude_globs.empty())
            {
                std::erase_if(
                    result.entries,
                    [&](const cg::graph::NeighborEntry& e)
                    {
                        const auto row = cg::db::get_symbol(store, e.id);
                        return row
                               && path_excluded(inv.exclude_globs, row->file);
                    });
            }
            // 取址证据（消费簇 1 契约）：callers 侧的调用盲区标注。
            std::vector<cg::db::TypeHintRow> addr;
            if (up)
            {
                addr = address_taken_sites(store, *target, result.family);
            }
            if (result.entries.empty())
            {
                // 零调用者是有效答案而非错误（外场反馈要求）：文本走 stdout
                // 正常结果形态（无 "ccg: error:" 前缀），--json
                // 输出完整正常载荷而非 error 对象；退出码维持
                // 1，以免破坏按码判错的既有脚本。死代码候选
                // 提示单列“经函数/成员指针调用不可见”——成员指针取址不建
                // 边之外，这是最常被误读成“没人用”的盲区；有取址证据行时
                // 附具体站点。
                if (!up)
                {
                    emit_error(inv, "no callees found");
                    return 1;
                }
                const bool off =
                    inv.dispatch_mode == cg::graph::DispatchMode::Off;
                // 置信标注（三种假零路径逐项实查）：指针调用是机制性盲区恒真；
                // sibling 仅虚目标且派发视图在用时可能；取址站点直接查库。
                const render::CallersBlindSpots blind{
                    .pointer_calls_invisible = true,
                    .sibling_dispatch_possible =
                        (target->flags & cg::db::sym_flags::is_virtual) != 0
                        && !off,
                    .member_address_taken = !addr.empty()};
                if (inv.json)
                {
                    std::println(
                        "{}",
                        render::neighbors_json(
                            true,
                            *target,
                            inv.depth,
                            result,
                            store,
                            inv.top,
                            addr,
                            &blind));
                }
                else
                {
                    std::string msg = std::format(
                        "no callers found — dead-code candidate unless one of "
                        "three invisible paths applies: indirect calls through "
                        "function/member pointers (never indexed); virtual "
                        "dispatch to a sibling override (target is virtual: "
                        "{}, use --no-dispatch for the static upper bound); "
                        "address taken at {} site(s) below",
                        blind.sibling_dispatch_possible ? "yes" : "no",
                        addr.size());
                    std::print(
                        "{}\n{}",
                        msg,
                        render::address_taken_text(store, *target, addr));
                }
                return 1;
            }
            if (inv.json)
            {
                std::println(
                    "{}",
                    render::neighbors_json(
                        up,
                        *target,
                        inv.depth,
                        result,
                        store,
                        inv.top,
                        addr));
            }
            else
            {
                std::print(
                    "{}",
                    render::neighbors_text(
                        up,
                        *target,
                        result,
                        store,
                        inv.top,
                        addr));
            }
            return 0;
        }

        /** @brief `ccg impact` 运行器：影响面。 */
        auto run_impact(cg::db::GraphStore& store, const Invocation& inv) -> int
        {
            const auto target =
                resolve_unique_target(store, inv, inv.positional.front());
            if (!target.has_value())
            {
                return 1;
            }
            cg::graph::ImpactResult result = cg::graph::impact(
                store,
                target->id,
                inv.direction,
                inv.depth,
                /*merge_family=*/!inv.no_merge,
                inv.dispatch_mode);
            // --exclude-glob：结果端剔除（定义路径命中的条目）；impacted/
            // direct/transitive 随过滤重算（计数与列表同口径），fan_in/
            // risk_score/risk 保持全量值——影响面的保守上界，过滤只治信噪比。
            if (!inv.exclude_globs.empty())
            {
                std::int64_t direct = 0;
                for (cg::graph::ImpactLevel& level : result.by_depth)
                {
                    std::erase_if(
                        level.ids,
                        [&](cg::db::SymId id)
                        {
                            const auto row = cg::db::get_symbol(store, id);
                            return row
                                   && path_excluded(
                                       inv.exclude_globs,
                                       row->file);
                        });
                    if (level.depth == 1)
                    {
                        direct = static_cast<std::int64_t>(level.ids.size());
                    }
                }
                std::erase_if(
                    result.by_depth,
                    [](const cg::graph::ImpactLevel& l)
                    { return l.ids.empty(); });
                std::int64_t impacted = 0;
                for (const cg::graph::ImpactLevel& level : result.by_depth)
                {
                    impacted += static_cast<std::int64_t>(level.ids.size());
                }
                result.impacted = impacted;
                result.direct = direct;
                result.transitive = impacted - direct;
            }
            // 取址证据（消费簇 1 契约）：只对 up（谁依赖它）有意义——down 是
            // 它依赖别人，不受成员指针调用盲区影响。
            std::vector<cg::db::TypeHintRow> addr;
            if (inv.direction == cg::graph::Direction::Up)
            {
                addr = address_taken_sites(store, *target, result.family);
            }
            if (inv.json)
            {
                std::println(
                    "{}",
                    render::impact_json(
                        *target,
                        inv.direction,
                        inv.depth,
                        result,
                        store,
                        inv.top,
                        addr));
            }
            else
            {
                std::print(
                    "{}",
                    render::impact_text(
                        *target,
                        inv.direction,
                        result,
                        store,
                        inv.top,
                        addr));
            }
            return 0;
        }

        /**
         * @brief `ccg trace` 运行器：两符号间最短调用路径。
         *
         * @internal 不可达是未找到结果：文本模式把退出 1 错误报在 stderr
         * （JSON 模式的错误一律走 stdout，调用方只解析一条流）；JSON 在
         * stdout 保留文档化的 {found:false} 载荷。
         */
        auto run_trace(cg::db::GraphStore& store, const Invocation& inv) -> int
        {
            const auto from =
                resolve_unique_target(store, inv, inv.positional[0]);
            if (!from.has_value())
            {
                return 1;
            }
            const auto to =
                resolve_unique_target(store, inv, inv.positional[1]);
            if (!to.has_value())
            {
                return 1;
            }
            const cg::graph::TraceResult result = cg::graph::trace(
                store,
                from->id,
                to->id,
                10,
                inv.dispatch_mode);
            if (!result.found && !inv.json)
            {
                std::println(
                    std::cerr,
                    "no path from {} to {} within 10 hops",
                    from->qualified_name,
                    to->qualified_name);
                return 1;
            }
            if (inv.json)
            {
                std::println(
                    "{}",
                    render::trace_json(*from, *to, result, store));
            }
            else
            {
                std::print("{}", render::trace_text(*from, *to, result, store));
            }
            return result.found ? 0 : 1;
        }

        /** @brief `ccg stats` 运行器：库摘要。 */
        auto run_stats(cg::db::GraphStore& store, const Invocation& inv) -> int
        {
            render::StatsInputs in;
            in.db_path = inv.db_path;
            if (inv.db_path != ":memory:")
            {
                std::error_code ec;
                if (const auto size = fs::file_size(fs::path{inv.db_path}, ec);
                    !ec)
                {
                    in.db_bytes = static_cast<std::int64_t>(size);
                }
                if (const auto size =
                        fs::file_size(fs::path{inv.db_path + "-wal"}, ec);
                    !ec)
                {
                    in.wal_bytes = static_cast<std::int64_t>(size);
                }
            }
            if (const auto root = store.meta("project_root"))
            {
                in.root = *root;
            }
            in.dispatch_stale_files =
                cg::util::parse_bounded(store.meta("dispatch_stale"), 0);
            // 槽侧快照的最近一次运行观测（meta analysis_snap_*）。
            if (const auto v = store.meta("analysis_snap_state"))
            {
                in.snap_state = *v;
            }
            if (const auto v = store.meta("analysis_snap_restore_us"))
            {
                in.snap_restore_us = std::atoll(v->c_str());
            }
            if (const auto v = store.meta("analysis_snap_dirty_files"))
            {
                in.snap_dirty_files = std::atoll(v->c_str());
            }
            if (const auto v = store.meta("analysis_snap_rebuilt_slots"))
            {
                in.snap_rebuilt_slots = std::atoll(v->c_str());
            }
            if (const auto v = store.meta("analysis_snap_bytes"))
            {
                in.snap_bytes = std::atoll(v->c_str());
            }
            in.st = cg::db::stats(store);
            // failed 点名（外场反馈要求）：files 行带着 parse_status
            // 与路径，一次扫描即得清单；stats 本身就是全库摘要命令，O(files)
            // 于此可接受。
            for (const cg::db::FileRow& f : cg::db::all_files(store))
            {
                if (f.status == cg::util::ParseStatus::Failed)
                {
                    in.failed_files.push_back(f.path);
                }
            }
            if (inv.json)
            {
                std::println("{}", render::stats_json(in));
            }
            else
            {
                std::print("{}", render::stats_text(in));
            }
            return 0;
        }

        /**
         * @brief 修剪一条喂入行并剥掉整段引号拼写（"a b.hpp" -> a b.hpp）；
         *        空结果由调用方跳过。
         */
        void clean_list_line(std::string& line)
        {
            line = cg::util::trim(line);
            if (line.empty())
            {
                return;
            }
            if (line.size() >= 2 && line.front() == '"' && line.back() == '"')
            {
                line = line.substr(1, line.size() - 2);
            }
        }

        /** @brief 从 stdin 逐行收路径：清洗后非空行进入清单。 */
        auto append_stdin_paths(std::vector<std::string>& raw) -> void
        {
            std::string line;
            while (std::getline(std::cin, line))
            {
                clean_list_line(line);
                if (!line.empty())
                {
                    raw.push_back(line);
                }
            }
        }

        /**
         * @brief `ccg changes` 运行器：变更文件的上游影响面。
         *
         * @internal
         * 输入模式：位置参数路径、--from-file 清单、stdin
         * （'-' 操作数、--from-file -、或无操作数而 stdin 已接管道——
         * `git diff --name-only | ccg changes`）。ccg 自身绝不运行任何版本
         * 控制工具。读已排空的 stdin 是空操作，故 stdin 读取器无需只读一次
         * 守卫。规范化与索引器完全一致（绝对化 + 词典序规范化 + norm_path），
         * 按规范化形去重、保留首个喂入拼写供报告。随后对库解析、映射到文件
         * 定义的全部符号、沿上游走查。
         */
        auto run_changes(cg::db::GraphStore& store, const Invocation& inv)
            -> int
        {
            std::vector<std::string> raw;
            for (const std::string& p : inv.positional)
            {
                if (p == "-")
                {
                    append_stdin_paths(raw);
                }
                else
                {
                    raw.push_back(p);
                }
            }
            if (!inv.from_file.empty())
            {
                if (inv.from_file == "-")
                {
                    append_stdin_paths(raw);
                }
                else
                {
                    std::ifstream in{fs::path{inv.from_file}};
                    if (!in)
                    {
                        throw std::runtime_error(
                            std::format(
                                "cannot read --from-file '{}'",
                                inv.from_file));
                    }
                    std::string line;
                    while (std::getline(in, line))
                    {
                        clean_list_line(line);
                        if (!line.empty())
                        {
                            raw.push_back(line);
                        }
                    }
                }
            }
            if (raw.empty())
            {
                if (cg::util::platform::stdin_is_piped())
                {
                    append_stdin_paths(raw);
                }
                else
                {
                    throw UsageError{
                        "no changed files given (pass file arguments, --from-file F, "
                        "or pipe a path list on stdin)",
                        usage_for(Invocation::Cmd::Changes)};
                }
            }

            std::vector<std::pair<std::string, std::string>>
                                            deduped; // (喂入, 规范)
            std::unordered_set<std::string> seen;
            for (const std::string& p : raw)
            {
                std::error_code ec;
                fs::path        abs = fs::absolute(fs::path{p}, ec);
                if (ec)
                {
                    abs = fs::path{p};
                }
                const std::string norm = cg::util::norm_path(
                    abs.lexically_normal().generic_string());
                if (seen.insert(norm).second)
                {
                    deduped.emplace_back(p, norm);
                }
            }

            render::ChangesInputs in;
            in.depth = inv.depth;
            std::vector<cg::db::SymId> seeds;
            for (const auto& [input, norm] : deduped)
            {
                ChangedFile f;
                f.input = input;
                if (const auto row = resolve_input_path(store, norm))
                {
                    f.known = true;
                    f.path = row->path;
                    std::error_code ec;
                    f.deleted = !fs::exists(fs::path{row->path}, ec) || ec;
                    const std::vector<cg::db::SymbolRow> syms =
                        cg::db::symbols_of_file(store, row->id);
                    f.symbols = static_cast<std::int64_t>(syms.size());
                    for (const cg::db::SymbolRow& s : syms)
                    {
                        seeds.push_back(s.id);
                    }
                }
                in.files.push_back(std::move(f));
            }
            in.result =
                cg::graph::changes(store, seeds, inv.depth, inv.dispatch_mode);

            const bool nothing_found =
                in.files.empty() || in.result.consumers.empty();
            if (inv.json)
            {
                std::println("{}", render::changes_json(in, store));
            }
            else
            {
                std::print("{}", render::changes_text(in, store));
                if (nothing_found)
                {
                    std::println(
                        std::cerr,
                        "ccg: nothing found (no known files or "
                        "no consumers)");
                }
            }
            return nothing_found ? 1 : 0;
        }

        /**
         * @brief `ccg check --cycles` 运行器：包含图环检测。
         *
         * @internal 发现环时用独立退出码 3（CI 门禁用；0 = 无环）。
         */
        auto run_check(cg::db::GraphStore& store, const Invocation& inv) -> int
        {
            if (!inv.cycles)
            {
                throw UsageError{
                    "check needs a mode; the only one today is --cycles",
                    usage_for(Invocation::Cmd::Check)};
            }
            const cg::graph::CyclesResult res = cg::graph::include_cycles(
                store,
                static_cast<std::size_t>(inv.list_cap));
            if (inv.json)
            {
                std::println("{}", render::cycles_json(res, store));
            }
            else
            {
                std::print("{}", render::cycles_text(res, store));
            }
            return res.groups_total > 0 ? 3 : 0;
        }

        /**
         * @brief `ccg graph` 运行器：调用图导出（dot / JSON）。
         *
         * @internal 两模式共用 --output 落盘路径；stdout 是机器可读载荷的默认
         * 目的地，写文件时一行注记走 stderr（stdout 保持纯净）。--json 与
         * --format json 等价（与查询族 --json 口径一致）；入口符号解析沿用
         * 层级解析与歧义候选报告（resolve_unique_target）。导出是忠实快照：
         * 无被调用者的入口仍产出单节点图、退出 0（与查询命令的"未找到"不同）。
         */
        auto run_graph(cg::db::GraphStore& store, const Invocation& inv) -> int
        {
            const bool  as_json = inv.json || inv.graph_format == "json";
            std::string payload;
            if (inv.graph_modules)
            {
                const cg::graph::ModuleGraph modules = cg::graph::module_graph(
                    store,
                    inv.file_substring.has_value()
                        ? std::string_view{*inv.file_substring}
                        : std::string_view{},
                    inv.min_weight);
                payload =
                    as_json
                        ? render::graph_modules_json(modules, inv.min_weight)
                        : render::graph_modules_dot(modules);
            }
            else
            {
                const auto target =
                    resolve_unique_target(store, inv, inv.graph_symbol);
                if (!target.has_value())
                {
                    return 1;
                }
                const cg::graph::CallClosure closure =
                    cg::graph::call_closure(store, target->id, inv.depth);
                payload = as_json ? render::graph_symbol_json(
                                        *target,
                                        inv.depth,
                                        closure,
                                        store)
                                  : render::graph_symbol_dot(
                                        *target,
                                        inv.depth,
                                        closure,
                                        store);
            }

            if (!payload.empty() && payload.back() != '\n')
            {
                payload += '\n'; // dot 自带尾换行；JSON 对齐查询族的行纪律
            }
            if (inv.output_file.empty())
            {
                std::print("{}", payload);
                return 0;
            }
            std::error_code ec;
            const fs::path  out_path{inv.output_file};
            if (!out_path.parent_path().empty())
            {
                fs::create_directories(out_path.parent_path(), ec);
            }
            std::ofstream out{out_path, std::ios::binary};
            if (!out)
            {
                throw std::runtime_error(
                    std::format("cannot write --output '{}'", inv.output_file));
            }
            out << payload;
            out.close();
            if (out.fail())
            {
                throw std::runtime_error(
                    std::format("cannot write --output '{}'", inv.output_file));
            }
            std::println(
                std::cerr,
                "ccg: wrote {} ({} bytes)",
                inv.output_file,
                payload.size());
            return 0;
        }

    } // namespace detail
} // namespace cg::cli
