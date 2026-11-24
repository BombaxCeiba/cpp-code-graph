/**
 * @module cg.cli.render.context
 *
 * @brief 邻域与上下文渲染：context 边块与方向桶（detail 域）、context/neighbors
 * 的文本与 JSON、深度前沿提示。
 */
export module cg.cli.render.context;

import std;
import cg.util;
import cg.db;
import cg.graph;
import cg.analysis.model;
import cg.cli.parse;
import cg.cli.render.base;

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

    namespace detail
    {
        /**
         * @brief context 文本的一个边块：标题行 + 每边一行。
         *
         * @internal 标签槽 14 列；virtual_dispatch 标签长一格，为它显式补
         *           一个分隔符，不改默认（非派发）情况的字节。入边标签与
         *           类型名不同（edge_label 的 incoming 侧）。
         */
        auto context_edges_block_text(
            std::string&                               out,
            GraphStore&                                store,
            cg::graph::DispatchMode                    dispatch_mode,
            std::string_view                           title,
            const std::vector<cg::graph::ContextEdge>& edges) -> void
        {
            if (edges.empty())
            {
                return;
            }
            out += std::format("\n{}\n", title);
            for (const cg::graph::ContextEdge& e : edges)
            {
                // virtual_dispatch 比 14 列的标签槽长一个字符；
                // 为它显式补一个分隔符，而不改动任何默认
                // （非派发）情况下的字节。
                std::string label =
                    std::string{edge_label(e.edge.type, title == "incoming")};
                if (e.edge.type == cg::db::EdgeType::VirtualDispatch)
                {
                    label += ' ';
                }
                const std::string note =
                    dispatch_mode == cg::graph::DispatchMode::Off
                        ? std::string{}
                        : dispatch_note(e.dispatch_status, e.edge.confidence);
                out += std::format(
                    "  {:<14}{:<33} {}{}{}\n",
                    label,
                    e.other.qualified_name,
                    site_text(store, e.edge),
                    family_note(store, e.family_origin),
                    note);
            }
        }

        /**
         * @brief context JSON 的一个方向桶：边按类型分组，每类型一个数组。
         *
         * @internal 边按类型分组到达（cg.graph 的 context 以 id 序遍历
         *           类型）；按首见顺序每类型发一个数组。
         */
        [[nodiscard]]
        auto context_edges_bucket(
            const ContextView& view,
            GraphStore&        store,
            bool               incoming) -> json::Object
        {
            json::Object                               obj;
            const std::vector<cg::graph::ContextEdge>& edges =
                incoming ? view.incoming : view.outgoing;
            std::vector<std::pair<EdgeType, json::Array>> groups;
            for (const cg::graph::ContextEdge& e : edges)
            {
                if (groups.empty() || groups.back().first != e.edge.type)
                {
                    groups.emplace_back(e.edge.type, json::Array{});
                }
                json::Object entry;
                entry.set("uid", e.other.id);
                entry.set(
                    "kind",
                    std::string_view{cg::db::name_of(e.other.kind)});
                entry.set("qualified_name", e.other.qualified_name);
                // 坐标系修正（外场反馈）：file 与 line 必须同源——line
                // 一直是站点（使用点）行号，file 也取站点文件，与文本输出
                // site_text 对齐；e.other.file
                // 是对端符号的定义文件，与使用行拼接是
                // 两套坐标混用。站点行缺失时与 site_text 一致给 "?"。
                const auto site = store.file(e.edge.site_file);
                entry.set("file", site ? site->path : std::string{"?"});
                entry.set("line", static_cast<std::int64_t>(e.edge.site_line));
                entry.set("occurrences", e.edge.occurrences);
                entry.set("confidence", e.edge.confidence);
                if (view.dispatch_mode != cg::graph::DispatchMode::Off)
                {
                    entry.set(
                        "dispatchStatus",
                        dispatch_status_name(e.dispatch_status));
                    if (e.dispatch_status == DispatchStatus::Inferred)
                    {
                        entry.set("dispatchConfidence", e.edge.confidence);
                    }
                }
                if (e.family_origin != 0)
                {
                    const auto origin =
                        cg::db::get_symbol(store, e.family_origin);
                    entry.set(
                        "familyOrigin",
                        origin ? origin->qualified_name : std::string{"?"});
                }
                groups.back().second.add(std::move(entry));
            }
            for (auto& [type, array] : groups)
            {
                obj.set(cg::db::name_of(type), std::move(array));
            }
            return obj;
        }
    } // namespace detail

    /**
     * @brief context 的文本视图。
     *
     * @internal 事实行汇总：旗标、模板参数、记录成员计数；参数类型随限定名
     * 括号展示（与 query/candidates 同口径），取址证据行与 callers 同款。
     */
    [[nodiscard]]
    auto context_text(
        GraphStore&                          store,
        const ContextView&                   view,
        std::span<const cg::db::TypeHintRow> addr_taken) -> std::string
    {
        const SymbolRow& s = view.symbol;
        std::string      out = std::format(
            "symbol  {} {}\n",
            cg::db::name_of(s.kind),
            std::format("{}{}", s.qualified_name, params_suffix(s)));
        if (s.parent.empty())
        {
            out += std::format("        {}\n", location_of(s));
        }
        else
        {
            out += std::format(
                "        {}  parent: {}\n",
                location_of(s),
                s.parent);
        }

        std::string facts;
        if (const auto flags = flag_names(s.flags); !flags.empty())
        {
            facts += std::format("flags: {}", cg::util::join(flags, ", "));
        }
        if (s.template_params.has_value())
        {
            facts += std::format(
                "{}template {}",
                facts.empty() ? "" : "   ",
                *s.template_params);
        }
        if (s.kind == SymbolKind::Record)
        {
            std::int64_t methods = 0;
            std::int64_t fields = 0;
            for (const SymbolRow& m : cg::db::members(store, s.id))
            {
                if (m.kind == SymbolKind::Field)
                {
                    ++fields;
                }
                else if (cg::db::is_method_like(m.kind))
                {
                    ++methods;
                }
            }
            facts += std::format(
                "{}methods {}   fields {}",
                facts.empty() ? "" : "   ",
                methods,
                fields);
        }
        if (!facts.empty())
        {
            out += std::format("        {}\n", facts);
        }

        // 外部边界 RTA 标注（root set 线，context
        // 展示用）：接口跨索引边界的事实行。
        if (s.kind == SymbolKind::Record)
        {
            if (const auto boundary = external_boundary_row(store, s))
            {
                out += std::format(
                    "external boundary: {} — instances may be created "
                    "outside the index; library-internal reach: {} "
                    "types\n",
                    external_boundary_phrase(boundary->first),
                    boundary->second);
            }
        }

        if (view.dispatch_mode != cg::graph::DispatchMode::Off)
        {
            DispatchCounts counts;
            // 统计行只数 incoming（外场反馈）：outgoing 的静态关系
            // （extends/type_alias 等）不是“谁派发调用了它”的事实，混入
            // 会把 0 incoming 显示成 “1 static”。
            for (const cg::graph::ContextEdge& e : view.incoming)
            {
                counts.add(e.dispatch_status);
            }
            // 降噪：无 inferred 且无 family estimate 的符号没有派发故事，
            // 不再打印统计行（全零行只刷屏）。
            if (counts.inferred > 0 || counts.family_estimate > 0)
            {
                out += dispatch_summary_text(counts);
            }
        }

        detail::context_edges_block_text(
            out,
            store,
            view.dispatch_mode,
            "incoming",
            view.incoming);
        detail::context_edges_block_text(
            out,
            store,
            view.dispatch_mode,
            "outgoing",
            view.outgoing);
        if (view.family.size() > 1)
        {
            out += std::format(
                "\noverride family merged ({} members; --no-merge excludes)\n",
                view.family.size());
            for (const cg::db::SymId m : view.family)
            {
                const auto row = cg::db::get_symbol(store, m);
                out += std::format(
                    "  {}{}\n",
                    row ? row->qualified_name : "?",
                    m == view.symbol.id ? "  (queried)" : "");
            }
        }
        out += address_taken_text(store, view.symbol, addr_taken);
        out += dispatch_stale_text(view.dispatch_stale_files);
        return out;
    }

    /**
     * @brief context 的 JSON 载荷。
     *
     * @internal incoming/outgoing 两桶各由 detail::context_edges_bucket
     *           按边类型分组产出。
     */
    [[nodiscard]]
    auto context_json(
        const ContextView&                   view,
        GraphStore&                          store,
        std::span<const cg::db::TypeHintRow> addr_taken) -> std::string
    {
        json::Object root;
        root.set("symbol", symbol_json(view.symbol));
        // 外部边界 RTA 标注（root set 线，context 展示；与文本输出口径一致）。
        if (view.symbol.kind == SymbolKind::Record)
        {
            if (const auto boundary = external_boundary_row(store, view.symbol))
            {
                json::Object o;
                o.set("kind", external_boundary_phrase(boundary->first));
                o.set(
                    "libraryInternalReach",
                    static_cast<std::int64_t>(boundary->second));
                root.set("externalBoundary", std::move(o));
            }
        }

        root.set("incoming", detail::context_edges_bucket(view, store, true));
        root.set("outgoing", detail::context_edges_bucket(view, store, false));
        if (view.dispatch_mode != cg::graph::DispatchMode::Off)
        {
            DispatchCounts counts;
            // 与文本口径一致：只数 incoming，无派发信号不设对象。
            for (const cg::graph::ContextEdge& e : view.incoming)
            {
                counts.add(e.dispatch_status);
            }
            if (counts.inferred > 0 || counts.family_estimate > 0)
            {
                root.set(
                    "dispatch",
                    dispatch_summary_json(view.dispatch_mode, counts));
            }
        }
        if (view.family.size() > 1)
        {
            json::Array fam;
            for (const cg::db::SymId m : view.family)
            {
                if (const auto row = cg::db::get_symbol(store, m))
                {
                    json::Object o;
                    o.set("uid", row->id);
                    o.set("qualified_name", row->qualified_name);
                    o.set("queried", row->id == view.symbol.id);
                    fam.add(std::move(o));
                }
            }
            root.set("family", std::move(fam));
        }
        if (!addr_taken.empty())
        {
            root.set("addressTaken", address_taken_json(store, addr_taken));
        }
        dispatch_stale_json(root, view.dispatch_stale_files);
        return json::stringify(std::move(root));
    }

    /** @brief callers/callees 的文本表格（按深度分节；调用点坐标口径）。 */
    [[nodiscard]]
    auto neighbors_text(
        bool                                 up,
        const SymbolRow&                     target,
        const NeighborsResult&               result,
        GraphStore&                          store,
        std::optional<int>                   top,
        std::span<const cg::db::TypeHintRow> addr_taken) -> std::string
    {
        std::string out = std::format(
            "{}  ({}, {})\n",
            std::format("{}{}", target.qualified_name, params_suffix(target)),
            cg::db::name_of(target.kind),
            location_of(target));
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
            for (const auto& entry : result.entries)
            {
                counts.add(entry.dispatch_status);
            }
            out += dispatch_summary_text(counts);
        }
        int               depth = 0;
        const std::size_t total = result.entries.size();
        std::size_t       shown = 0;
        for (const auto& entry : result.entries)
        {
            if (top.has_value() && shown >= static_cast<std::size_t>(*top))
            {
                break;
            }
            ++shown;
            if (entry.depth != depth)
            {
                depth = entry.depth;
                out += std::format("  depth {}\n", depth);
            }
            const auto        row = cg::db::get_symbol(store, entry.id);
            const std::string name = row ? row->qualified_name : "?";
            // 双行号口径（实测）：主坐标=对端符号定义
            // 行（与 query/impact/trace 一致），调用点移入 calls: 段（同文件
            // 省文件名）；边行缺失时只有定义坐标（诚实降级，不冒充调用点）。
            const cg::db::SymId pred =
                entry.via_id != 0 ? entry.via_id : target.id;
            std::string where;
            if (const auto site =
                    up ? call_site_of(store, entry.id, pred, entry.via)
                       : call_site_of(store, pred, entry.id, entry.via))
            {
                const std::string def_where =
                    row ? location_of(*row) : std::string{"?"};
                if (row && site->file == row->file)
                {
                    where = std::format("{}  calls:{}", def_where, site->line);
                }
                else
                {
                    where = std::format(
                        "{}  calls:{}:{}",
                        def_where,
                        site->file,
                        site->line);
                }
            }
            else
            {
                where = row ? location_of(*row) : "?";
            }
            if (entry.occurrences > 1)
            {
                where += std::format(" x{}", entry.occurrences);
            }
            out += std::format("    {:<32} {}", name, where);
            if (entry.depth > 1)
            {
                const auto via = cg::db::get_symbol(store, entry.via_id);
                out +=
                    std::format("   (via {})", via ? via->qualified_name : "?");
            }
            out += family_note(store, entry.family_origin);
            if (dispatch_visible)
            {
                out += dispatch_note(
                    entry.dispatch_status,
                    entry.dispatch_confidence);
            }
            out += '\n';
        }
        if (top.has_value() && shown < total)
        {
            out += std::format(
                "    ... and {} more (raise --top to list them)\n",
                total - shown);
        }
        // truncated = 结果被截断（列出 < 找到）：--top 截断处实算为 yes；
        // 深度前沿信号移至 deeper 注记（三要素定义见 deeper_note）。
        out += std::format(
            "{} {} total (truncated: {})\n",
            total,
            up ? "callers" : "callees",
            yes_no(top.has_value() && shown < total));
        out += deeper_note(result.truncated);
        out += address_taken_text(store, target, addr_taken);
        out += dispatch_stale_text(result.dispatch_stale_files);
        return out;
    }

    /** @brief callers/callees 的 JSON 载荷。 */
    [[nodiscard]]
    auto neighbors_json(
        bool                                 up,
        const SymbolRow&                     target,
        int                                  depth,
        const NeighborsResult&               result,
        GraphStore&                          store,
        std::optional<int>                   top,
        std::span<const cg::db::TypeHintRow> addr_taken,
        const CallersBlindSpots*             blind = nullptr) -> std::string
    {
        json::Object root;
        root.set("target", symbol_json(target));
        root.set("depth", static_cast<std::int64_t>(depth));
        root.set("direction", up ? "up" : "down");
        root.set("total", static_cast<std::int64_t>(result.entries.size()));
        if (blind != nullptr)
        {
            // 零调用者置信标注：三种假零路径逐项实查（外场反馈要求的载荷完备性）。
            json::Object reason;
            reason.set(
                "pointer_calls_invisible",
                blind->pointer_calls_invisible);
            reason.set(
                "sibling_dispatch_possible",
                blind->sibling_dispatch_possible);
            reason.set("member_address_taken", blind->member_address_taken);
            root.set("no_callers_reason", std::move(reason));
        }
        if (top.has_value())
        {
            root.set("top", static_cast<std::int64_t>(*top));
        }
        const std::size_t total = result.entries.size();
        std::size_t       shown = 0;
        json::Array       entries;
        for (const auto& e : result.entries)
        {
            if (top.has_value() && shown >= static_cast<std::size_t>(*top))
            {
                break;
            }
            ++shown;
            const auto   row = cg::db::get_symbol(store, e.id);
            json::Object o;
            // file/line = 调用点坐标（与 context_json 同口径）；
            // 边行缺失时回退对端符号的定义坐标。
            const cg::db::SymId pred = e.via_id != 0 ? e.via_id : target.id;
            const auto site = up ? call_site_of(store, e.id, pred, e.via)
                                 : call_site_of(store, pred, e.id, e.via);
            if (row)
            {
                o.set("uid", row->id);
                o.set("kind", std::string_view{cg::db::name_of(row->kind)});
                o.set("qualified_name", row->qualified_name);
            }
            else
            {
                o.set("uid", e.id);
            }
            if (site.has_value())
            {
                o.set("file", site->file);
                o.set("line", static_cast<std::int64_t>(site->line));
            }
            else if (row)
            {
                o.set("file", row->file);
                o.set("line", static_cast<std::int64_t>(row->line));
            }
            // 对端符号定义坐标（双行号补齐：line 保持调用点=向后兼容，
            // def_line/def_file 与 query/impact 的符号坐标同口径）。
            if (row && site.has_value())
            {
                o.set("def_file", row->file);
                o.set("def_line", static_cast<std::int64_t>(row->line));
            }
            // col/parent 取对端符号定义行（对齐 impact byDepth 的符号对象；
            // 上面的 file/line 仍是调用点坐标，两套口径并存是刻意的）。
            if (row)
            {
                o.set("col", static_cast<std::int64_t>(row->col));
                if (row->parent.empty())
                {
                    o.set_null("parent");
                }
                else
                {
                    o.set("parent", row->parent);
                }
            }
            o.set("depth", static_cast<std::int64_t>(e.depth));
            if (const auto via = cg::db::get_symbol(store, e.via_id);
                via && e.depth > 1)
            {
                o.set("via", via->qualified_name);
            }
            else
            {
                o.set_null("via");
            }
            if (e.family_origin != 0)
            {
                const auto origin = cg::db::get_symbol(store, e.family_origin);
                o.set(
                    "familyOrigin",
                    origin ? origin->qualified_name : std::string{"?"});
            }
            o.set("occurrences", e.occurrences);
            if (result.dispatch_mode != cg::graph::DispatchMode::Off)
            {
                o.set(
                    "dispatchStatus",
                    dispatch_status_name(e.dispatch_status));
                if (e.dispatch_status == DispatchStatus::Inferred)
                {
                    o.set("viaType", std::string_view{"virtual_dispatch"});
                    o.set("confidence", e.dispatch_confidence);
                    o.set("dispatchConfidence", e.dispatch_confidence);
                }
            }
            entries.add(std::move(o));
        }
        // truncated = 结果被截断（列出 < 找到），--top 截断处实算；前沿信
        // 号由 deeperEdgesExist 独立承载（deeper_note 三要素注释）。
        root.set("truncated", top.has_value() && shown < total);
        root.set("deeperEdgesExist", result.truncated);
        root.set("entries", std::move(entries));
        if (result.dispatch_mode != cg::graph::DispatchMode::Off)
        {
            DispatchCounts counts;
            for (const auto& e : result.entries)
            {
                counts.add(e.dispatch_status);
            }
            root.set(
                "dispatch",
                dispatch_summary_json(result.dispatch_mode, counts));
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
} // namespace cg::cli::render
