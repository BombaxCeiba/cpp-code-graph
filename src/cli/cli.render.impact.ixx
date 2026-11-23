module;
#include "../db/db.tables.h"

/**
 * @module cg.cli.render.impact
 *
 * @brief 影响面与调用轨迹渲染：impact/trace 的文本与 JSON。
 */
export module cg.cli.render.impact;

import std;
import cg.util;
import cg.db;
import cg.graph;
import cg.analysis.model;
import cg.cli.parse;
import cg.cli.render.base;
import cg.cli.render.dispatch;
import cg.cli.render.symbol;
import cg.cli.render.context;

export namespace cg::cli::render
{
    using cg::db::EdgeRow;
    using cg::db::EdgeType;
    using cg::db::GraphStore;
    using cg::db::Stats;
    using cg::db::SymbolKind;
    using cg::db::SymbolRow;
    using cg::graph::ContextView;
    using cg::graph::ImpactResult;
    using cg::graph::NeighborsResult;
    using cg::graph::TraceResult;

    namespace json = cg::util::json;

    /** @brief impact 的文本报告（风险阶梯与逐深度列表；调用点坐标口径）。 */
    [[nodiscard]]
    auto impact_text(
        const SymbolRow&                     target,
        cg::graph::Direction                 dir,
        const ImpactResult&                  result,
        GraphStore&                          store,
        std::optional<int>                   top,
        std::span<const cg::db::TypeHintRow> addr_taken) -> std::string
    {
        std::string out = std::format(
            "target   {} {}  {}\n",
            cg::db::name_of(target.kind),
            std::format("{}{}", target.qualified_name, params_suffix(target)),
            location_of(target));
        out += std::format("risk     {}\n", risk_name(result.risk));
        out += std::format(
            "score    {} (3*direct {} + transitive {} + fan-in {})"
            "\n",
            result.risk_score,
            result.direct,
            result.transitive,
            result.fan_in);
        std::string breakdown = std::format("direct: {}", result.direct);
        for (const auto& level : result.by_depth)
        {
            if (level.depth == 1)
            {
                continue;
            }
            breakdown +=
                std::format(", at depth {}: {}", level.depth, level.ids.size());
        }
        out += std::format("impacted {} ({})\n", result.impacted, breakdown);
        if (result.family.size() > 1)
        {
            out += std::format(
                "override family merged: {} members (--no-merge excludes)\n",
                result.family.size());
        }
        const bool dispatch_visible =
            result.dispatch_mode != cg::graph::DispatchMode::Off;
        if (dispatch_visible)
        {
            DispatchCounts counts;
            for (const auto& level : result.by_depth)
            {
                for (const cg::db::SymId id : level.ids)
                {
                    const auto st = result.dispatch_status.find(id);
                    counts.add(
                        st == result.dispatch_status.end()
                            ? DispatchStatus::None
                            : st->second);
                }
            }
            out += dispatch_summary_text(counts);
        }
        // 站点解析的候选前驱集（ImpactResult 无逐条目前驱，见
        // impact_site_of 的三要素注释）。
        std::unordered_set<cg::db::SymId>      candidates{target.id};
        std::unordered_map<cg::db::SymId, int> depth_of;
        for (const cg::db::SymId m : result.family)
        {
            candidates.insert(m);
        }
        for (const auto& level : result.by_depth)
        {
            for (const cg::db::SymId id : level.ids)
            {
                candidates.insert(id);
                depth_of.emplace(id, level.depth);
            }
        }
        const bool dir_up = dir == cg::graph::Direction::Up;
        bool       cut = false;
        for (const auto& level : result.by_depth)
        {
            out += std::format("  depth {}\n", level.depth);
            std::size_t shown = 0;
            for (const cg::db::SymId id : level.ids)
            {
                if (top.has_value() && shown >= static_cast<std::size_t>(*top))
                {
                    break;
                }
                ++shown;
                const auto  row = cg::db::get_symbol(store, id);
                const auto  fo = result.family_of.find(id);
                std::string note;
                if (dispatch_visible)
                {
                    const auto           st = result.dispatch_status.find(id);
                    const DispatchStatus status =
                        st == result.dispatch_status.end()
                            ? DispatchStatus::None
                            : st->second;
                    double confidence = 0.0;
                    if (const auto dc = result.dispatch_confidence.find(id);
                        dc != result.dispatch_confidence.end())
                    {
                        confidence = dc->second;
                    }
                    note = dispatch_note(status, confidence);
                }
                // 调用点坐标：链内一跳边的首个站点，回退定义行。
                std::string where;
                if (const auto site = impact_site_of(
                        store,
                        id,
                        level.depth,
                        dir_up,
                        candidates,
                        depth_of,
                        dispatch_visible))
                {
                    where = std::format("{}:{}", site->file, site->line);
                }
                else
                {
                    where = row ? location_of(*row) : "?";
                }
                out += std::format(
                    "    {:<12}{:<32} {}{}{}\n",
                    row ? cg::db::name_of(row->kind) : "?",
                    row ? row->qualified_name : "?",
                    where,
                    fo == result.family_of.end()
                        ? std::string{}
                        : family_note(store, fo->second),
                    note);
            }
            if (top.has_value() && shown < level.ids.size())
            {
                cut = true;
                out += std::format(
                    "    ... and {} more at depth {} (raise --top to list "
                    "them)\n",
                    level.ids.size() - shown,
                    level.depth);
            }
        }
        // truncated = 结果被截断（列出 < 找到）：--top 桶内截断处实算；
        // 深度前沿信号移至 deeper 注记（三要素定义见 deeper_note）。
        out += std::format("truncated: {}\n", yes_no(cut));
        out += deeper_note(result.truncated);
        out += address_taken_text(store, target, addr_taken);
        out += dispatch_stale_text(result.dispatch_stale_files);
        return out;
    }

    /** @brief impact 的 JSON 载荷。 */
    [[nodiscard]]
    auto impact_json(
        const SymbolRow&                     target,
        cg::graph::Direction                 dir,
        int                                  depth,
        const ImpactResult&                  result,
        GraphStore&                          store,
        std::optional<int>                   top,
        std::span<const cg::db::TypeHintRow> addr_taken) -> std::string
    {
        json::Object root;
        root.set("target", symbol_json(target));
        root.set("direction", direction_name(dir));
        root.set("depth", static_cast<std::int64_t>(depth));
        root.set("impactedCount", result.impacted);
        root.set("direct", result.direct);
        root.set("transitive", result.transitive);
        root.set("fanIn", result.fan_in);
        root.set("riskScore", result.risk_score);
        if (top.has_value())
        {
            root.set("top", static_cast<std::int64_t>(*top));
        }
        json::Array counts;
        for (const auto& level : result.by_depth)
        {
            counts.add(static_cast<std::int64_t>(level.ids.size()));
        }
        root.set("byDepthCounts", std::move(counts));
        // 站点解析的候选前驱集（ImpactResult 无逐条目前驱，见
        // impact_site_of 的三要素注释）。
        std::unordered_set<cg::db::SymId>      candidates{target.id};
        std::unordered_map<cg::db::SymId, int> depth_of;
        for (const cg::db::SymId m : result.family)
        {
            candidates.insert(m);
        }
        for (const auto& level : result.by_depth)
        {
            for (const cg::db::SymId id : level.ids)
            {
                candidates.insert(id);
                depth_of.emplace(id, level.depth);
            }
        }
        const bool dir_up = dir == cg::graph::Direction::Up;
        const bool dispatch_walked =
            result.dispatch_mode != cg::graph::DispatchMode::Off;
        bool        cut = false;
        json::Array by_depth;
        for (const auto& level : result.by_depth)
        {
            json::Object lv;
            lv.set("depth", static_cast<std::int64_t>(level.depth));
            std::size_t shown = 0;
            json::Array syms;
            for (const cg::db::SymId id : level.ids)
            {
                if (top.has_value() && shown >= static_cast<std::size_t>(*top))
                {
                    break;
                }
                ++shown;
                if (const auto row = cg::db::get_symbol(store, id))
                {
                    json::Object obj = symbol_json(*row);
                    if (result.dispatch_mode != cg::graph::DispatchMode::Off)
                    {
                        const auto st = result.dispatch_status.find(id);
                        const DispatchStatus status =
                            st == result.dispatch_status.end()
                                ? DispatchStatus::None
                                : st->second;
                        obj.set("dispatchStatus", dispatch_status_name(status));
                        if (status == DispatchStatus::Inferred)
                        {
                            if (const auto dc =
                                    result.dispatch_confidence.find(id);
                                dc != result.dispatch_confidence.end())
                            {
                                obj.set("dispatchConfidence", dc->second);
                            }
                        }
                    }
                    // callSite = 链内一跳的调用点坐标（缺失时不设字段，
                    // 定义坐标保留在符号对象本体）。
                    if (const auto site = impact_site_of(
                            store,
                            id,
                            level.depth,
                            dir_up,
                            candidates,
                            depth_of,
                            dispatch_walked))
                    {
                        json::Object spot;
                        spot.set("file", site->file);
                        spot.set("line", static_cast<std::int64_t>(site->line));
                        obj.set("callSite", std::move(spot));
                    }
                    syms.add(std::move(obj));
                }
            }
            if (top.has_value() && shown < level.ids.size())
            {
                cut = true;
            }
            lv.set("symbols", std::move(syms));
            by_depth.add(std::move(lv));
        }
        root.set("byDepth", std::move(by_depth));
        root.set("risk", risk_name(result.risk));
        // truncated = 结果被截断（列出 < 找到），--top 桶内截断处实算；前
        // 沿信号由 deeperEdgesExist 独立承载（deeper_note 三要素注释）。
        root.set("truncated", cut);
        root.set("deeperEdgesExist", result.truncated);
        if (result.dispatch_mode != cg::graph::DispatchMode::Off)
        {
            DispatchCounts dispatch_totals;
            for (const auto& level : result.by_depth)
            {
                for (const cg::db::SymId id : level.ids)
                {
                    const auto st = result.dispatch_status.find(id);
                    dispatch_totals.add(
                        st == result.dispatch_status.end()
                            ? DispatchStatus::None
                            : st->second);
                }
            }
            root.set(
                "dispatch",
                dispatch_summary_json(result.dispatch_mode, dispatch_totals));
        }
        if (result.family.size() > 1)
        {
            json::Array fam;
            for (const cg::db::SymId m : result.family)
            {
                if (const auto row = cg::db::get_symbol(store, m))
                {
                    fam.add(row->qualified_name);
                }
            }
            root.set("family", std::move(fam));
        }
        if (!addr_taken.empty())
        {
            root.set("addressTaken", address_taken_json(store, addr_taken));
        }
        dispatch_stale_json(root, result.dispatch_stale_files);
        return json::stringify(std::move(root));
    }

    /**
     * @brief trace 的文本路径。
     *
     * @internal 前置条件 result.found == true：未找到的文本报告由
     * run_trace 在 stderr 给出并退出 1（CLI 错误契约），JSON 模式的
     * {found:false} 载荷由 trace_json 渲染。found 为假时 chain 为空，
     * 本函数的跳数计算（chain.size() - 1）会下溢，绝不传入。
     */
    [[nodiscard]]
    auto trace_text(
        const SymbolRow&   from,
        const SymbolRow&   to,
        const TraceResult& result,
        GraphStore&        store) -> std::string
    {
        std::string out = std::format(
            "found: {} -> {}  ({} hops)\n",
            from.qualified_name,
            to.qualified_name,
            result.chain.size() - 1);
        const bool dispatch_visible =
            result.dispatch_mode != cg::graph::DispatchMode::Off
            && result.hops.size() == result.chain.size();
        DispatchCounts counts;
        bool           first = true;
        for (std::size_t i = 0; i < result.chain.size(); ++i)
        {
            const cg::db::SymId id = result.chain[i];
            const auto          row = cg::db::get_symbol(store, id);
            if (first)
            {
                out += std::format(
                    "  {:<34} {}\n",
                    row ? row->qualified_name : "?",
                    row ? location_of(*row) : "?");
                first = false;
            }
            else if (!dispatch_visible)
            {
                out += std::format(
                    "    --calls--> {:<30} {}\n",
                    row ? row->qualified_name : "?",
                    row ? location_of(*row) : "?");
            }
            else
            {
                const cg::graph::TraceHop& hop = result.hops[i];
                const std::string_view     kind =
                    hop.via == cg::db::EdgeType::VirtualDispatch ? "dispatch"
                                                                 : "calls";
                out += std::format(
                    "    --{}--> {:<28} {}",
                    kind,
                    row ? row->qualified_name : "?",
                    row ? location_of(*row) : "?");
                out += dispatch_note(hop.dispatch_status, hop.confidence);
                counts.add(hop.dispatch_status);
                out += '\n';
            }
        }
        if (dispatch_visible)
        {
            out += dispatch_summary_text(counts);
        }
        out += dispatch_stale_text(result.dispatch_stale_files);
        return out;
    }

    /** @brief trace 的 JSON 载荷。 */
    [[nodiscard]]
    auto trace_json(
        const SymbolRow&   from,
        const SymbolRow&   to,
        const TraceResult& result,
        GraphStore&        store) -> std::string
    {
        json::Object root;
        root.set("from", symbol_json(from));
        root.set("to", symbol_json(to));
        root.set("found", result.found);
        root.set(
            "length",
            static_cast<std::int64_t>(
                result.chain.empty() ? 0 : result.chain.size() - 1));
        json::Array hops;
        const bool  dispatch_visible =
            result.dispatch_mode != cg::graph::DispatchMode::Off
            && result.hops.size() == result.chain.size();
        DispatchCounts counts;
        for (std::size_t i = 0; i < result.chain.size(); ++i)
        {
            const cg::db::SymId id = result.chain[i];
            if (const auto row = cg::db::get_symbol(store, id))
            {
                json::Object hop = symbol_json(*row);
                if (dispatch_visible && i > 0)
                {
                    const cg::graph::TraceHop& h = result.hops[i];
                    hop.set(
                        "viaType",
                        h.via == cg::db::EdgeType::VirtualDispatch
                            ? std::string_view{"virtual_dispatch"}
                            : std::string_view{"calls"});
                    hop.set(
                        "dispatchStatus",
                        dispatch_status_name(h.dispatch_status));
                    if (h.dispatch_status == DispatchStatus::Inferred)
                    {
                        hop.set("dispatchConfidence", h.confidence);
                    }
                    counts.add(h.dispatch_status);
                }
                hops.add(std::move(hop));
            }
        }
        root.set("hops", std::move(hops));
        // trace 的 truncated 是真实截断：!found 且跳数上限处前沿非空——
        // "无路径"这个否定答案是截断搜索的产物（文本早已用 "within 10
        // hops" 表达同一事实），命中路径时恒 false。
        root.set("truncated", result.truncated);
        if (dispatch_visible)
        {
            root.set(
                "dispatch",
                dispatch_summary_json(result.dispatch_mode, counts));
        }
        dispatch_stale_json(root, result.dispatch_stale_files);
        return json::stringify(std::move(root));
    }
} // namespace cg::cli::render
