module;
#include "../db/db.tables.h"

/**
 * @module cg.cli.render.base
 *
 * @brief
 * 渲染层共享原语：位置/等级/方向等枚举名、字节与耗时的人类可读化、站点与边标签文本、调用与影响站点摘取。
 */
export module cg.cli.render.base;

import std;
import cg.util;
import cg.db;
import cg.graph;
import cg.analysis.model;
import cg.cli.parse;

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
        /** @brief 命中位旗标时把名字收进旗标清单。 */
        auto push_flag_name(
            std::vector<std::string_view>& out,
            bool                           on,
            std::string_view               name) -> void
        {
            if (on)
            {
                out.push_back(name);
            }
        }
    } // namespace detail

    /** @brief 符号的 "file:line" 位置文本。 */
    [[nodiscard]]
    auto location_of(const SymbolRow& s) -> std::string
    {
        return std::format("{}:{}", s.file, s.line);
    }

    [[nodiscard]]
    auto tier_name(int tier) -> std::string_view
    {
        switch (tier)
        {
        case 1:
            return "exact";
        case 2:
            return "qualified suffix";
        case 3:
            return "substring";
        default:
            return "none";
        }
    }

    [[nodiscard]]
    auto risk_name(cg::graph::Risk risk) -> std::string_view
    {
        switch (risk)
        {
        case cg::graph::Risk::Low:
            return "LOW";
        case cg::graph::Risk::Medium:
            return "MEDIUM";
        case cg::graph::Risk::High:
            return "HIGH";
        case cg::graph::Risk::Critical:
            return "CRITICAL";
        case cg::graph::Risk::Unknown:
            return "UNKNOWN";
        }
        return "UNKNOWN";
    }

    [[nodiscard]]
    auto direction_name(cg::graph::Direction d) -> std::string_view
    {
        return d == cg::graph::Direction::Up ? "up" : "down";
    }

    [[nodiscard]]
    auto yes_no(bool b) -> std::string_view
    {
        return b ? "yes" : "no";
    }

    [[nodiscard]]
    auto human_bytes(std::int64_t bytes) -> std::string
    {
        const auto n = static_cast<double>(bytes);
        if (bytes < 1024)
        {
            return std::format("{} B", bytes);
        }
        if (n < 1024.0 * 1024.0)
        {
            return std::format("{:.1f} KB", n / 1024.0);
        }
        if (n < 1024.0 * 1024.0 * 1024.0)
        {
            return std::format("{:.1f} MB", n / (1024.0 * 1024.0));
        }
        return std::format("{:.1f} GB", n / (1024.0 * 1024.0 * 1024.0));
    }

    [[nodiscard]]
    auto human_elapsed(double seconds) -> std::string
    {
        if (seconds < 60.0)
        {
            return std::format("{:.1}s", seconds);
        }
        const auto whole = static_cast<std::int64_t>(seconds);
        return std::format("{}m {}s", whole / 60, whole % 60);
    }

    [[nodiscard]]
    auto flag_names(std::uint32_t flags) -> std::vector<std::string_view>
    {
        namespace f = cg::db::sym_flags;
        std::vector<std::string_view> out;
        detail::push_flag_name(out, (flags & f::is_struct) != 0, "struct");
        detail::push_flag_name(out, (flags & f::is_union) != 0, "union");
        detail::push_flag_name(out, (flags & f::is_template) != 0, "template");
        detail::push_flag_name(
            out,
            (flags & f::is_partial_spec) != 0,
            "partial-spec");
        detail::push_flag_name(out, (flags & f::is_virtual) != 0, "virtual");
        detail::push_flag_name(
            out,
            (flags & f::is_pure_virtual) != 0,
            "pure-virtual");
        detail::push_flag_name(out, (flags & f::is_static) != 0, "static");
        detail::push_flag_name(
            out,
            (flags & f::is_constexpr) != 0,
            "constexpr");
        detail::push_flag_name(out, (flags & f::is_const) != 0, "const");
        return out;
    }

    /**
     * @brief 边行的站点路径文本（上下文渲染要显示调用站点）。
     */
    [[nodiscard]]
    auto site_text(GraphStore& store, const EdgeRow& e) -> std::string
    {
        if (const auto file = store.file(e.site_file))
        {
            return std::format("{}:{}", file->path, e.site_line);
        }
        return std::format("?{}", e.site_line);
    }

    /**
     * @brief 入边标签在若干处与类型名不同（如
     *        "extends from"、"overridden by"、"used by"）。
     */
    [[nodiscard]]
    auto edge_label(EdgeType type, bool incoming) -> std::string_view
    {
        if (!incoming)
        {
            return cg::db::name_of(type);
        }
        switch (type)
        {
        case EdgeType::Extends:
            return "extends from";
        case EdgeType::Overrides:
            return "overridden by";
        case EdgeType::TypeAlias:
            return "aliased by";
        case EdgeType::Specializes:
            return "specialized by";
        case EdgeType::Uses:
            return "used by";
        default:
            return cg::db::name_of(type);
        }
    }

    /**
     * @brief 覆盖族标记文本：合并行途经的家族成员（自身行与未合并视图
     *        为 ''）。
     */
    [[nodiscard]]
    auto family_note(cg::db::GraphStore& store, cg::db::SymId origin)
        -> std::string
    {
        if (origin == 0)
        {
            return {};
        }
        const auto row = cg::db::get_symbol(store, origin);
        return std::format("  [family: {}]", row ? row->qualified_name : "?");
    }

    /** @brief 一个调用点坐标（file:line 已解析）。 */
    struct SiteSpot
    {
        std::string file;
        int         line = 0;
    };

    /**
     * @brief (from, to, type) 之间的首个站点（用户口径：调用点坐标，与
     *        context 同口径；行号最小者优先）。
     *
     * @internal 空返回 = 该对在该类型上无原始边行（条目由家族合并或派发
     * 改写贡献），调用方回退对端符号的定义行——诚实降级，绝不猜站点。
     */
    [[nodiscard]]
    auto call_site_of(
        cg::db::GraphStore& store,
        cg::db::SymId       from,
        cg::db::SymId       to,
        EdgeType            type) -> std::optional<SiteSpot>
    {
        const std::vector<EdgeRow> rows =
            cg::db::edges_between(store, from, to, type);
        if (rows.empty())
        {
            return std::nullopt;
        }
        const auto file = store.file(rows.front().site_file);
        return SiteSpot{
            .file = file ? file->path : std::string{"?"},
            .line = rows.front().site_line};
    }

    /**
     * @brief impact 条目的链内一跳站点（ImpactResult 不带逐条目前驱）。
     *
     * @internal 候选前驱 = {被查询符号} ∪ 覆盖族 ∪ 结果集内全部符号；对
     * 条目 X 取它与某个更浅层候选之间（up：X 的出边，X 调用更浅者；down：
     * X 的入边）的首条边行。类型集 = {Calls, Extends, Uses}，非 Off 模式
     * 追加 VirtualDispatch（Off 的 walk 不含派发边，口径保持纯净）。
     * 找不到 → 空返回，调用方回退定义行——宁可少一行坐标也不展示链外边。
     */
    [[nodiscard]]
    auto impact_site_of(
        cg::db::GraphStore&                           store,
        cg::db::SymId                                 id,
        int                                           depth,
        bool                                          dir_up,
        const std::unordered_set<cg::db::SymId>&      candidates,
        const std::unordered_map<cg::db::SymId, int>& depth_of,
        bool dispatch_walked) -> std::optional<SiteSpot>
    {
        const EdgeType types[] = {
            EdgeType::Calls,
            EdgeType::Extends,
            EdgeType::Uses,
            EdgeType::VirtualDispatch};
        for (const EdgeType type : types)
        {
            if (type == EdgeType::VirtualDispatch && !dispatch_walked)
            {
                continue;
            }
            for (const EdgeRow& e :
                 cg::db::edges_of(store, id, /*outgoing=*/dir_up, type))
            {
                const cg::db::SymId other = dir_up ? e.dst : e.src;
                if (other == id || !candidates.contains(other))
                {
                    continue;
                }
                const auto it = depth_of.find(other);
                if (it != depth_of.end() && it->second >= depth)
                {
                    continue; // 只认更浅层（家族种子不在 depth_of，天然放行）
                }
                const auto file = store.file(e.site_file);
                return SiteSpot{
                    .file = file ? file->path : std::string{"?"},
                    .line = e.site_line};
            }
        }
        return std::nullopt;
    }

    /**
     * @brief 取址证据的展示行（消费簇 1
     * 契约；外场反馈要求成员指针调用的盲区不再沉默）。
     *
     * 证据行存在时输出 "&符号 出现于 file:line（+N 处）"与"经函数/成员
     * 指针的调用不可见"——它解释 callers/impact 为什么看不见这类使用，
     * 不是新增的调用边。空证据返回空串（不刷屏）。
     */
    [[nodiscard]]
    auto address_taken_text(
        cg::db::GraphStore&                  store,
        const SymbolRow&                     target,
        std::span<const cg::db::TypeHintRow> sites) -> std::string
    {
        if (sites.empty())
        {
            return {};
        }
        const cg::db::TypeHintRow& first = sites.front();
        const auto                 file = store.file(first.file_id);
        std::string                more;
        if (sites.size() > 1)
        {
            more = std::format(
                " (+{} more site{})",
                sites.size() - 1,
                sites.size() > 2 ? "s" : "");
        }
        return std::format(
            "address-taken: &{} appears at {}:{}{}; calls through function "
            "or member pointers are not visible\n",
            target.qualified_name,
            file ? file->path : std::string{"?"},
            first.line,
            more);
    }

    /** @brief 取址证据的 JSON 数组（file/line 站点对；空证据返回空数组）。 */
    [[nodiscard]]
    auto address_taken_json(
        cg::db::GraphStore&                  store,
        std::span<const cg::db::TypeHintRow> sites) -> json::Array
    {
        json::Array arr;
        for (const cg::db::TypeHintRow& r : sites)
        {
            json::Object o;
            const auto   file = store.file(r.file_id);
            o.set("file", file ? file->path : std::string{"?"});
            o.set("line", static_cast<std::int64_t>(r.line));
            arr.add(std::move(o));
        }
        return arr;
    }

    /**
     * @brief 外部边界 reason 词的人类短语（root set 线，context 展示用）。
     */
    [[nodiscard]]
    auto external_boundary_phrase(std::string_view reason) -> std::string_view
    {
        using cg::analysis::EXTERNAL_DIRECTOR;
        using cg::analysis::EXTERNAL_DLOPEN;
        if (reason == EXTERNAL_DIRECTOR)
        {
            return "swig director";
        }
        if (reason == EXTERNAL_DLOPEN)
        {
            return "dynamic library boundary";
        }
        return "external boundary";
    }

    /**
     * @brief 记录符号的外部边界 RTA 行（接口的 "`#external`" 槽）。
     *
     * @internal 展示契约：行存在 = 接口跨出索引边界（脚本/插件持有或
     * 提供实例），R(I)（type_set）是"库内可能实例化的实现"上界。可达 ≠
     * 此站点调它（排序与禁置信纪律）：本行只作 context 的事实行展示，绝不参与
     * 任何站点置信度。非记录符号或无行返回 nullopt（不刷屏）。
     */
    [[nodiscard]]
    auto external_boundary_row(
        cg::db::GraphStore& store,
        const SymbolRow&    target)
        -> std::optional<std::pair<std::string, std::size_t>>
    {
        if (target.kind != SymbolKind::Record)
        {
            return std::nullopt;
        }
        for (const cg::db::TypeHintRow& row :
             store.type_hints_of_owner(target.id))
        {
            if (row.owner_id == target.id
                && row.var_name == cg::analysis::external_slot)
            {
                return std::pair<std::string, std::size_t>{
                    row.fallback_reason,
                    cg::analysis::parse_string_array(row.type_set).size()};
            }
        }
        return std::nullopt;
    }
} // namespace cg::cli::render
