/**
 * @module cg.cli.render.changes
 *
 * @brief 变更环与图导出渲染：changes/cycles 文本与 JSON、dot
 * 转义与符号/模块图输出、错误载荷。
 */
export module cg.cli.render.changes;

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

    /**
     * @brief changes 渲染所需的数据：已解析的逐文件表、走查结果与实际
     *        使用的深度。
     */
    struct ChangesInputs
    {
        std::vector<ChangedFile> files; // 已解析的变更文件表
        int                      depth = 3;
        cg::graph::ChangesResult result;
    };

    /** @brief changes 的文本报告。 */
    [[nodiscard]]
    auto changes_text(const ChangesInputs& in, GraphStore& store) -> std::string
    {
        const cg::graph::ChangesResult& res = in.result;
        std::int64_t                    known = 0;
        std::int64_t                    deleted = 0;
        std::int64_t                    unknown = 0;
        for (const ChangedFile& f : in.files)
        {
            if (!f.known)
            {
                ++unknown;
            }
            else if (f.deleted)
            {
                ++deleted;
            }
            else
            {
                ++known;
            }
        }
        std::string out = std::format(
            "changes  {} file{} fed (known {}, deleted {}, unknown {})  depth {}\n",
            in.files.size(),
            in.files.size() == 1 ? "" : "s",
            known,
            deleted,
            unknown,
            in.depth);
        for (const ChangedFile& f : in.files)
        {
            out += std::format(
                "  {:<44}{:<9}{}{}\n",
                f.input,
                f.known ? (f.deleted ? "deleted" : "present") : "unknown",
                f.known ? std::format("{} symbols", f.symbols)
                        : std::string{"not in the database"},
                f.known && f.path != f.input ? std::format("  [{}]", f.path)
                                             : std::string{});
        }
        out += std::format(
            "consumers {} (direct {}, transitive {})\n",
            res.consumers.size(),
            res.direct,
            res.transitive);
        out += std::format("risk     {}\n", risk_name(res.risk));
        out += std::format(
            "score    {} (3*direct {} + transitive {} + fan-in {})"
            "\n",
            res.risk_score,
            res.direct,
            res.transitive,
            res.fan_in);
        const bool dispatch_visible =
            res.dispatch_mode != cg::graph::DispatchMode::Off;
        if (dispatch_visible)
        {
            DispatchCounts counts;
            for (const cg::graph::ChangesEntry& e : res.consumers)
            {
                counts.add(e.dispatch_status);
            }
            out += dispatch_summary_text(counts);
        }
        int depth = 0;
        for (const cg::graph::ChangesEntry& e : res.consumers)
        {
            if (e.depth != depth)
            {
                depth = e.depth;
                out += std::format("  depth {}\n", depth);
            }
            const auto        row = cg::db::get_symbol(store, e.id);
            const std::string name = row ? row->qualified_name : "?";
            out += std::format(
                "    {:<12}{:<34}{}",
                row ? cg::db::name_of(row->kind) : "?",
                name,
                row ? location_of(*row) : std::string{"?"});
            if (e.depth > 1)
            {
                const auto via = cg::db::get_symbol(store, e.via_id);
                out += std::format(
                    "  (via {})",
                    via ? via->qualified_name : std::string{"?"});
            }
            out += std::format("  risk {}", risk_name(e.risk));
            if (dispatch_visible)
            {
                out += dispatch_note(e.dispatch_status, e.dispatch_confidence);
            }
            out += '\n';
        }
        // truncated = 结果被截断（列出 < 找到）：无条目上限，恒 no；深度
        // 前沿信号移至 deeper 注记（三要素定义见 deeper_note）。
        out += std::format("truncated: {}\n", yes_no(false));
        out += deeper_note(res.truncated);
        out += dispatch_stale_text(res.dispatch_stale_files);
        return out;
    }

    /** @brief changes 的 JSON 载荷。 */
    [[nodiscard]]
    auto changes_json(const ChangesInputs& in, GraphStore& store) -> std::string
    {
        const cg::graph::ChangesResult& res = in.result;
        json::Object                    root;
        root.set("depth", static_cast<std::int64_t>(in.depth));
        json::Array files;
        for (const ChangedFile& f : in.files)
        {
            json::Object o;
            o.set("input", f.input);
            if (f.known)
            {
                o.set("path", f.path);
            }
            else
            {
                o.set_null("path");
            }
            o.set(
                "status",
                f.known ? (f.deleted ? "deleted" : "present")
                        : std::string{"unknown"});
            o.set("symbols", f.symbols);
            files.add(std::move(o));
        }
        root.set("files", std::move(files));
        root.set("seeds", static_cast<std::int64_t>(res.seeds.size()));
        json::Array consumers;
        for (const cg::graph::ChangesEntry& e : res.consumers)
        {
            json::Object o;
            const auto   row = cg::db::get_symbol(store, e.id);
            if (row)
            {
                o.set("uid", row->id);
                o.set("kind", std::string_view{cg::db::name_of(row->kind)});
                o.set("qualified_name", row->qualified_name);
                o.set("file", row->file);
                o.set("line", static_cast<std::int64_t>(row->line));
            }
            else
            {
                o.set("uid", e.id);
            }
            o.set("depth", static_cast<std::int64_t>(e.depth));
            const auto seed = cg::db::get_symbol(store, e.seed);
            if (e.seed != 0 && seed)
            {
                o.set("viaSeed", seed->qualified_name);
            }
            else
            {
                o.set_null("viaSeed");
            }
            o.set("fanIn", e.fan_in);
            o.set("risk", risk_name(e.risk));
            if (res.dispatch_mode != cg::graph::DispatchMode::Off)
            {
                o.set(
                    "dispatchStatus",
                    dispatch_status_name(e.dispatch_status));
                if (e.dispatch_status == DispatchStatus::Inferred)
                {
                    o.set("viaType", std::string_view{"virtual_dispatch"});
                    o.set("dispatchConfidence", e.dispatch_confidence);
                }
            }
            consumers.add(std::move(o));
        }
        root.set("consumers", std::move(consumers));
        root.set("direct", res.direct);
        root.set("transitive", res.transitive);
        root.set("fanIn", res.fan_in);
        root.set("riskScore", res.risk_score);
        root.set("risk", risk_name(res.risk));
        // truncated = 结果被截断（列出 < 找到），无条目上限故恒 false；
        // 前沿信号由 deeperEdgesExist 独立承载（deeper_note 三要素注释）。
        root.set("truncated", false);
        root.set("deeperEdgesExist", res.truncated);
        if (res.dispatch_mode != cg::graph::DispatchMode::Off)
        {
            DispatchCounts counts;
            for (const cg::graph::ChangesEntry& e : res.consumers)
            {
                counts.add(e.dispatch_status);
            }
            root.set(
                "dispatch",
                dispatch_summary_json(res.dispatch_mode, counts));
        }
        dispatch_stale_json(root, res.dispatch_stale_files);
        return json::stringify(std::move(root));
    }

    /** @brief cycles 的文本报告（逐簇列出包含边）。 */
    [[nodiscard]]
    auto cycles_text(const cg::graph::CyclesResult& res, GraphStore& store)
        -> std::string
    {
        std::string out;
        if (res.groups_total == 0)
        {
            out += std::format(
                "include cycles   none ({} files scanned)\n",
                res.files_scanned);
            if (res.ambiguous_links > 0)
            {
                out += std::format(
                    "ambiguous includes {} (spelling matches several files; "
                    "no cycle edge from them)\n",
                    res.ambiguous_links);
            }
            return out;
        }
        out += std::format(
            "include cycles   {} group{} ({} files, {} edges)\n",
            res.groups_total,
            res.groups_total == 1 ? "" : "s",
            res.files_total,
            res.edges_total);
        std::size_t listed = 0;
        for (const cg::graph::CycleGroup& g : res.groups)
        {
            ++listed;
            out += std::format(
                "cycle {} ({} file{})\n",
                listed,
                g.files.size(),
                g.files.size() == 1 ? "" : "s");
            for (const auto& [src, dst] : g.edges)
            {
                out += std::format(
                    "  {} -> {}\n",
                    detail::cycle_file_path(store, src),
                    detail::cycle_file_path(store, dst));
            }
        }
        out += std::format(
            "truncated: {} ({} of {} groups shown)\n",
            yes_no(res.truncated),
            res.groups.size(),
            res.groups_total);
        if (res.ambiguous_links > 0)
        {
            out += std::format(
                "ambiguous includes {} (spelling matches several files; "
                "no cycle edge from them)\n",
                res.ambiguous_links);
        }
        return out;
    }

    /** @brief cycles 的 JSON 载荷。 */
    [[nodiscard]]
    auto cycles_json(const cg::graph::CyclesResult& res, GraphStore& store)
        -> std::string
    {
        json::Object root;
        root.set("cycles", res.groups_total > 0);
        root.set("groupsTotal", res.groups_total);
        root.set("filesInCycles", res.files_total);
        root.set("edgesInCycles", res.edges_total);
        root.set("filesScanned", res.files_scanned);
        root.set("unresolvedIncludes", res.unresolved_links);
        root.set("ambiguousIncludes", res.ambiguous_links);
        root.set("truncated", res.truncated);
        json::Array groups;
        for (const cg::graph::CycleGroup& g : res.groups)
        {
            json::Object o;
            json::Array  files;
            for (const cg::db::FileId id : g.files)
            {
                files.add(detail::cycle_file_path(store, id));
            }
            o.set("files", std::move(files));
            json::Array edges;
            for (const auto& [src, dst] : g.edges)
            {
                json::Object edge;
                edge.set("from", detail::cycle_file_path(store, src));
                edge.set("to", detail::cycle_file_path(store, dst));
                edges.add(std::move(edge));
            }
            o.set("edges", std::move(edges));
            groups.add(std::move(o));
        }
        root.set("groups", std::move(groups));
        return json::stringify(std::move(root));
    }

    /**
     * @brief dot 字符串字面量转义：引号与反斜杠前置反斜杠，控制字符落成
     *        空格（dot 语法对二进制垃圾零容忍；UTF-8 字节原样透传，
     *        中文标识符安全）。
     */
    [[nodiscard]]
    auto dot_escape(std::string_view s) -> std::string
    {
        std::string out;
        out.reserve(s.size());
        for (const char c : s)
        {
            if (c == '"' || c == '\\')
            {
                out += '\\';
                out += c;
            }
            else
            {
                out += static_cast<unsigned char>(c) < 0x20 ? ' ' : c;
            }
        }
        return out;
    }

    /**
     * @brief graph --symbol 的 dot 输出。
     *
     * 节点 id 即限定名；入口高亮（fillcolor），节点/边已按确定性顺序排好。
     */
    [[nodiscard]]
    auto graph_symbol_dot(
        const SymbolRow&              target,
        int                           depth,
        const cg::graph::CallClosure& closure,
        GraphStore&                   store) -> std::string
    {
        std::string out = "digraph ccg_call_graph {\n";
        out += "  rankdir=LR;\n";
        out += std::format(
            "  graph [label=\"call closure of {} at depth {} (source: calls "
            "edges)\"];\n",
            dot_escape(target.qualified_name),
            depth);
        out += "  node [shape=box, style=\"rounded,filled\", "
               "fillcolor=\"#e8f0fe\"];\n  edge [color=\"#5f6368\"];\n";
        for (const cg::graph::CallNode& n : closure.nodes)
        {
            const std::string id =
                dot_escape(detail::closure_symbol_name(store, n.id));
            out += n.id == target.id
                       ? std::format("  \"{}\" [fillcolor=\"#ffd166\"];\n", id)
                       : std::format("  \"{}\";\n", id);
        }
        for (const cg::graph::CallClosureEdge& e : closure.edges)
        {
            out += std::format(
                "  \"{}\" -> \"{}\";\n",
                dot_escape(detail::closure_symbol_name(store, e.src)),
                dot_escape(detail::closure_symbol_name(store, e.dst)));
        }
        out += "}\n";
        return out;
    }

    /**
     * @brief graph --symbol 的 JSON 载荷（agent 消费）。
     *
     * 诚实位一对：truncated = 结果被截断（闭包无条目上限，恒 false）；
     * deeperEdgesExist 承载 --depth 上限处的前沿信号（语义见
     * deeper_note 的三要素注释）。
     */
    [[nodiscard]]
    auto graph_symbol_json(
        const SymbolRow&              target,
        int                           depth,
        const cg::graph::CallClosure& closure,
        GraphStore&                   store) -> std::string
    {
        json::Object root;
        root.set("target", symbol_json(target));
        root.set("depth", static_cast<std::int64_t>(depth));
        root.set("nodeCount", static_cast<std::int64_t>(closure.nodes.size()));
        root.set("edgeCount", static_cast<std::int64_t>(closure.edges.size()));
        json::Array nodes;
        for (const cg::graph::CallNode& n : closure.nodes)
        {
            json::Object o;
            if (const auto row = cg::db::get_symbol(store, n.id))
            {
                o.set("uid", row->id);
                o.set("kind", std::string_view{cg::db::name_of(row->kind)});
                o.set("qualified_name", row->qualified_name);
                o.set("file", row->file);
                o.set("line", static_cast<std::int64_t>(row->line));
            }
            else
            {
                o.set("uid", n.id);
            }
            o.set("depth", static_cast<std::int64_t>(n.depth));
            nodes.add(std::move(o));
        }
        root.set("nodes", std::move(nodes));
        json::Array edges;
        for (const cg::graph::CallClosureEdge& e : closure.edges)
        {
            json::Object o;
            const auto   src = cg::db::get_symbol(store, e.src);
            const auto   dst = cg::db::get_symbol(store, e.dst);
            o.set(
                "from",
                src ? src->qualified_name : std::format("sym{}", e.src));
            o.set(
                "to",
                dst ? dst->qualified_name : std::format("sym{}", e.dst));
            o.set("fromUid", e.src);
            o.set("toUid", e.dst);
            o.set("occurrences", e.occurrences);
            edges.add(std::move(o));
        }
        root.set("edges", std::move(edges));
        root.set("truncated", false);
        root.set("deeperEdgesExist", closure.truncated);
        return json::stringify(std::move(root));
    }

    /**
     * @brief graph --modules 的 dot 输出。
     *
     * 边标签 = 聚合权重，笔宽随权重加粗（1 + w/20，封顶 4）。
     */
    [[nodiscard]]
    auto graph_modules_dot(const cg::graph::ModuleGraph& g) -> std::string
    {
        std::string out = "digraph ccg_module_graph {\n";
        out += "  rankdir=LR;\n";
        out += "  node [shape=box, style=\"rounded,filled\", "
               "fillcolor=\"#d7ffd9\"];\n  edge [color=\"#5f6368\"];\n";
        for (const std::string& m : g.modules)
        {
            out += std::format("  \"{}\";\n", dot_escape(m));
        }
        for (const cg::graph::ModuleGraphEdge& e : g.edges)
        {
            out += std::format(
                "  \"{}\" -> \"{}\" [label=\"{}\", penwidth={:.1}];\n",
                dot_escape(e.from),
                dot_escape(e.to),
                e.weight,
                std::min(1.0 + static_cast<double>(e.weight) / 20.0, 4.0));
        }
        out += "}\n";
        return out;
    }

    /**
     * @brief graph --modules 的 JSON 载荷。
     *
     * edgesTotal 恒为权重过滤前的聚合边总数（计数不受 --min-weight 影响）。
     */
    [[nodiscard]]
    auto graph_modules_json(const cg::graph::ModuleGraph& g, int min_weight)
        -> std::string
    {
        json::Object root;
        root.set("moduleCount", static_cast<std::int64_t>(g.modules.size()));
        root.set("edgeCount", static_cast<std::int64_t>(g.edges.size()));
        root.set("edgesTotal", g.edges_total);
        root.set("minWeight", static_cast<std::int64_t>(min_weight));
        json::Array mods;
        for (const std::string& m : g.modules)
        {
            mods.add(m);
        }
        root.set("modules", std::move(mods));
        json::Array edges;
        for (const cg::graph::ModuleGraphEdge& e : g.edges)
        {
            json::Object o;
            o.set("from", e.from);
            o.set("to", e.to);
            o.set("weight", e.weight);
            edges.add(std::move(o));
        }
        root.set("edges", std::move(edges));
        return json::stringify(std::move(root));
    }

    /**
     * @brief JSON 错误载荷（JSON 错误走 STDOUT，使调用方
     *        从一条流解析一切；退出码不变）。
     */
    [[nodiscard]]
    auto error_json(
        std::string_view           message,
        std::optional<json::Array> candidates) -> std::string
    {
        json::Object root;
        root.set("error", message);
        root.set("partial", false);
        if (candidates.has_value())
        {
            root.set("candidates", std::move(*candidates));
        }
        return json::stringify(std::move(root));
    }
} // namespace cg::cli::render
