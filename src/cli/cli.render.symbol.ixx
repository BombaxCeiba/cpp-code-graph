/**
 * @module cg.cli.render.symbol
 *
 * @brief 符号与查询渲染：参数后缀、符号
 * JSON、匹配等级、query/members/implements/candidates 的文本与 JSON。
 */
export module cg.cli.render.symbol;

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
     * @brief 单个命中相对查询的层（1/2/3）：与 find_symbols 的层判据一致，
     *        层合并后渲染层逐行标注所属层（不再一个 tier 概括全部）。
     */
    [[nodiscard]]
    auto match_tier(std::string_view q, const SymbolRow& m) -> int
    {
        if (m.name == q || m.qualified_name == q || m.unique_key == q)
        {
            return 1;
        }
        if (m.qualified_name.ends_with(std::format("::{}", q)))
        {
            return 2;
        }
        return 3;
    }

    /** @brief query 的文本表格。 */
    [[nodiscard]]
    auto query_text(
        std::string_view           q,
        int                        tier,
        std::span<const SymbolRow> matches) -> std::string
    {
        std::string out = std::format(
            "{:<12}{:<33} {}\n",
            "kind",
            "qualified name",
            "location");
        // 命中层计数（层合并展示）：各命中层都点名，不再让上层挡下层。
        std::int64_t by_tier[4] = {0, 0, 0, 0};
        for (const SymbolRow& m : matches)
        {
            out += std::format(
                "{:<12}{:<33} {}\n",
                cg::db::name_of(m.kind),
                std::format("{}{}", m.qualified_name, params_suffix(m)),
                location_of(m));
            ++by_tier[match_tier(q, m)];
        }
        std::string tiers;
        for (const int t : {1, 2, 3})
        {
            if (by_tier[t] > 0)
            {
                tiers += tiers.empty() ? std::string{tier_name(t)}
                                       : std::format(" + {}", tier_name(t));
            }
        }
        if (tiers.empty())
        {
            tiers = std::string{tier_name(tier)}; // 空结果：沿用 none
        }
        out += std::format("{} matches (tier: {})\n", matches.size(), tiers);
        return out;
    }

    /** @brief query 的 JSON 载荷。 */
    [[nodiscard]]
    auto query_json(
        std::string_view           q,
        int                        tier,
        std::span<const SymbolRow> matches) -> std::string
    {
        json::Object root;
        root.set("query", q);
        root.set("tier", tier_name(tier));
        json::Array arr;
        for (const SymbolRow& m : matches)
        {
            json::Object o = symbol_json(m);
            // 层合并后逐命中标注所属层（机器消费方不再依赖单一 tier 字段）。
            o.set("matchTier", tier_name(match_tier(q, m)));
            arr.add(std::move(o));
        }
        root.set("matches", std::move(arr));
        return json::stringify(std::move(root));
    }

    /**
     * @brief members 的文本清单（声明序；外场反馈要求成员面不用回头读头文件）。
     *
     * @internal origin 分组：own 段在前，各基类段按 BFS 首见序随其后。
     */
    [[nodiscard]]
    auto members_text(
        const SymbolRow&                   record,
        std::span<const cg::db::MemberRow> rows) -> std::string
    {
        std::string out = std::format(
            "record   {}  {}\n",
            record.qualified_name,
            location_of(record));
        std::int64_t methods = 0;
        std::int64_t fields = 0;
        for (const auto& m : rows)
        {
            if (m.symbol.kind == SymbolKind::Field)
            {
                ++fields;
            }
            else if (cg::db::is_method_like(m.symbol.kind))
            {
                ++methods;
            }
        }
        out += std::format(
            "         {} members ({} methods, {} fields)\n",
            rows.size(),
            methods,
            fields);
        // 分组：own 先、基类按首见序；逐组渲染（无继承时单组=旧形态）。
        std::vector<std::string> group_order;
        std::unordered_map<std::string, std::vector<const cg::db::MemberRow*>>
            groups;
        for (const auto& m : rows)
        {
            if (!groups.contains(m.origin))
            {
                group_order.push_back(m.origin);
            }
            groups[m.origin].push_back(&m);
        }
        for (const std::string& origin : group_order)
        {
            if (origin != "own")
            {
                out += std::format("inherited from {}\n", origin);
            }
            out += std::format(
                "{:<12}{:<33} {}\n",
                "kind",
                "qualified name",
                "location");
            for (const auto* m : groups[origin])
            {
                out += std::format(
                    "{:<12}{:<33} {}\n",
                    cg::db::name_of(m->symbol.kind),
                    m->symbol.qualified_name,
                    location_of(m->symbol));
            }
        }
        return out;
    }

    /** @brief members 的 JSON 载荷（每成员带 origin：own 或基类限定名）。 */
    [[nodiscard]]
    auto members_json(
        const SymbolRow&                   record,
        std::span<const cg::db::MemberRow> rows) -> std::string
    {
        json::Object root;
        root.set("record", symbol_json(record));
        std::int64_t methods = 0;
        std::int64_t fields = 0;
        json::Array  arr;
        for (const auto& m : rows)
        {
            if (m.symbol.kind == SymbolKind::Field)
            {
                ++fields;
            }
            else if (cg::db::is_method_like(m.symbol.kind))
            {
                ++methods;
            }
            json::Object o = symbol_json(m.symbol);
            o.set("origin", m.origin);
            arr.add(std::move(o));
        }
        root.set("total", static_cast<std::int64_t>(rows.size()));
        root.set("methods", methods);
        root.set("fields", fields);
        root.set("members", std::move(arr));
        return json::stringify(std::move(root));
    }

    /** @brief --implements 结果行：派生类型 + 继承深度（1=直接）。 */
    struct ImplementRow
    {
        SymbolRow symbol;
        int       depth = 1;
    };

    /** @brief --implements 的文本清单（按 BFS 序，深度列分组可读）。 */
    [[nodiscard]]
    auto implements_text(
        const SymbolRow&              target,
        std::span<const ImplementRow> rows) -> std::string
    {
        std::string out = std::format(
            "interface {}  {}\n",
            target.qualified_name,
            location_of(target));
        out += std::format("          {} deriving types\n", rows.size());
        out += std::format(
            "{:<8}{:<12}{:<40} {}\n",
            "depth",
            "kind",
            "qualified name",
            "location");
        for (const ImplementRow& r : rows)
        {
            out += std::format(
                "{:<8}{:<12}{:<40} {}\n",
                r.depth,
                cg::db::name_of(r.symbol.kind),
                r.symbol.qualified_name,
                location_of(r.symbol));
        }
        return out;
    }

    /** @brief --implements 的 JSON 载荷。 */
    [[nodiscard]]
    auto implements_json(
        const SymbolRow&              target,
        std::span<const ImplementRow> rows) -> std::string
    {
        json::Object root;
        root.set("interface", symbol_json(target));
        root.set("total", static_cast<std::int64_t>(rows.size()));
        json::Array arr;
        for (const ImplementRow& r : rows)
        {
            json::Object o = symbol_json(r.symbol);
            o.set("depth", static_cast<std::int64_t>(r.depth));
            arr.add(std::move(o));
        }
        root.set("deriving", std::move(arr));
        return json::stringify(std::move(root));
    }

    /**
     * @brief 歧义目标清单（文本）：每行一条 "kind qualified file:line"。
     */
    [[nodiscard]]
    auto candidates_text(std::span<const SymbolRow> rows) -> std::string
    {
        std::string out;
        for (const SymbolRow& r : rows)
        {
            out += std::format(
                "{:<12}{}  {}\n",
                cg::db::name_of(r.kind),
                std::format("{}{}", r.qualified_name, params_suffix(r)),
                location_of(r));
        }
        return out;
    }

    /** @brief 歧义目标清单（JSON 数组）。 */
    [[nodiscard]]
    auto candidates_json(std::span<const SymbolRow> rows) -> json::Array
    {
        json::Array arr;
        for (const SymbolRow& r : rows)
        {
            arr.add(symbol_json(r));
        }
        return arr;
    }
} // namespace cg::cli::render
