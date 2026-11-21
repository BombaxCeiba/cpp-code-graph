module;
#include "../db/db.tables.h"

/**
 * @module cg.cli.render
 *
 * @brief 每个命令的输出渲染器：stdout 上的人类可读表格与经
 *        cg.util.json 写门面构建的 --json 载荷。
 *
 * CLI 规格文档 第 2 节的示例即格式契约；每个 JSON 载荷在适用处携带
 * error/partial/truncated 字段（第 1 节）。truncated 只表"结果被截
 * 断"；--depth 上限处的 BFS 前沿信号由 deeperEdgesExist 独立承载
 * （语义见 deeper_note 的三要素注释）。
 *
 * @internal
 * db.tables.h 置于全局模块片段：本单元导入 cg.db，MSVC 要求 sqlpp23
 * 表模型定义在此可见，以合并模块记录的模板特化（完整机理见
 * db.tables.h）。
 */
export module cg.cli.render;

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
        const auto                    add = [&](bool on, std::string_view name)
        {
            if (on)
            {
                out.push_back(name);
            }
        };
        add((flags & f::is_struct) != 0, "struct");
        add((flags & f::is_union) != 0, "union");
        add((flags & f::is_template) != 0, "template");
        add((flags & f::is_partial_spec) != 0, "partial-spec");
        add((flags & f::is_virtual) != 0, "virtual");
        add((flags & f::is_pure_virtual) != 0, "pure-virtual");
        add((flags & f::is_static) != 0, "static");
        add((flags & f::is_constexpr) != 0, "constexpr");
        add((flags & f::is_const) != 0, "const");
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

    using cg::graph::DispatchMode;
    using cg::graph::DispatchStatus;

    /** @internal 逐状态的计数（文本头与 JSON 汇总共用）。 */
    struct DispatchCounts
    {
        std::int64_t inferred = 0;
        std::int64_t family_estimate = 0;
        std::int64_t none = 0;

        void add(DispatchStatus status)
        {
            switch (status)
            {
            case DispatchStatus::Inferred:
                ++inferred;
                break;
            case DispatchStatus::FamilyEstimate:
                ++family_estimate;
                break;
            case DispatchStatus::None:
                ++none;
                break;
            }
        }
    };

    /** @brief JSON 的 dispatchStatus 取值（用户可见的唯一状态词汇）。 */
    [[nodiscard]]
    auto dispatch_status_name(DispatchStatus status) -> std::string_view
    {
        switch (status)
        {
        case DispatchStatus::Inferred:
            return "inferred";
        case DispatchStatus::FamilyEstimate:
            return "family_estimate";
        case DispatchStatus::None:
            return "none";
        }
        return "none";
    }

    [[nodiscard]]
    auto dispatch_mode_name(DispatchMode mode) -> std::string_view
    {
        switch (mode)
        {
        case DispatchMode::Off:
            return "off";
        case DispatchMode::Precise:
            return "precise";
        case DispatchMode::Fallback:
            return "fallback";
        case DispatchMode::Additive:
            return "additive";
        }
        return "precise";
    }

    /**
     * @brief 单条结果的派发标注。
     *
     * 用户可见文本禁止出现需要内部上下文的家族回退简写：
     * 有证据时给数值置信度，没有证据时给自解释的 family estimate。
     */
    [[nodiscard]]
    auto dispatch_note(DispatchStatus status, double confidence) -> std::string
    {
        switch (status)
        {
        case DispatchStatus::Inferred:
            return std::format("   [dispatch {:.2f}]", confidence);
        case DispatchStatus::FamilyEstimate:
            return "   [dispatch unknown · family estimate]";
        case DispatchStatus::None:
            return {};
        }
        return {};
    }

    /**
     * @brief 文本头：家族估算自解释行 + 状态计数。
     *
     * 仅当调用方确认模式不是 Off 时调用（Off 必须逐字节保持旧输出）。
     */
    [[nodiscard]]
    auto dispatch_summary_text(const DispatchCounts& counts) -> std::string
    {
        std::string out;
        if (counts.family_estimate > 0)
        {
            out = "family estimate: dynamic type unproven; estimated from the "
                  "whole override family, may include non-target "
                  "implementations\n";
        }
        out += std::format(
            "dispatch: {} inferred, {} family estimate, {} static "
            "(--no-dispatch for the static view)\n",
            counts.inferred,
            counts.family_estimate,
            counts.none);
        return out;
    }

    /** @brief JSON 汇总对象 `dispatch`。 */
    [[nodiscard]]
    auto dispatch_summary_json(DispatchMode mode, const DispatchCounts& counts)
        -> json::Object
    {
        json::Object o;
        o.set("mode", dispatch_mode_name(mode));
        o.set("threshold", cg::graph::kDispatchConfidenceFloor);
        o.set("inferred", counts.inferred);
        o.set("familyEstimate", counts.family_estimate);
        o.set("static", counts.none);
        return o;
    }

    /**
     * @brief 延迟收敛的滞后标注行（绝不静默旧派发数据）。
     *
     * dispatch_stale_files > 0（编辑批延迟了 RTA/类型传播/dispatch、收敛批未跑）
     * 时返回一行人话；已收敛（0/负）返回空串，输出与旧版逐字节一致。
     */
    [[nodiscard]]
    auto dispatch_stale_text(std::int64_t stale_files) -> std::string
    {
        if (stale_files <= 0)
        {
            return {};
        }
        return std::format(
            "dispatch analysis lags the index by {} file{} — virtual dispatch "
            "data below predates those edits (run `ccg update --drain` to "
            "converge)\n",
            stale_files,
            stale_files == 1 ? "" : "s");
    }

    /** @brief 同一滞后事实的 JSON 形态（>0 才写，键名与 hook 解析共享）。 */
    inline void dispatch_stale_json(
        json::Object& root,
        std::int64_t  stale_files)
    {
        if (stale_files > 0)
        {
            root.set("dispatchStaleFiles", stale_files);
        }
    }

    /**
     * @brief 参数类型的逗号连接后缀（限定名后括号内；非函数/未记录为空）。
     *
     * @internal symbols.param_types 存 JSON 字符串数组文本，与 type_set 共用
     * cg::analysis::parse_string_array 解析；无参函数的合法形态是 "()"。
     */
    [[nodiscard]]
    auto params_suffix(const SymbolRow& s) -> std::string
    {
        if (!s.param_types.has_value())
        {
            return {};
        }
        const std::vector<std::string> parts =
            cg::analysis::parse_string_array(*s.param_types);
        return std::format("({})", cg::util::join(parts, ", "));
    }

    /** @brief params 的 JSON 字符串数组（前置条件：s.param_types 有值）。 */
    [[nodiscard]]
    auto params_json(const SymbolRow& s) -> json::Array
    {
        json::Array arr;
        for (const std::string& p :
             cg::analysis::parse_string_array(*s.param_types))
        {
            arr.add(p);
        }
        return arr;
    }

    /**
     * @brief 单个符号的 JSON 对象（query/target/from/to/hops 共享）。
     */
    [[nodiscard]]
    auto symbol_json(const SymbolRow& s) -> json::Object
    {
        json::Object o;
        o.set("uid", s.id);
        o.set("kind", std::string_view{cg::db::name_of(s.kind)});
        o.set("name", s.name);
        o.set("qualified_name", s.qualified_name);
        // 参数类型：非函数/未记录与 parent 同惯例给 null；无参函数是空数组
        // （"()"），二者语义不同。
        if (s.param_types.has_value())
        {
            o.set("params", params_json(s));
        }
        else
        {
            o.set_null("params");
        }
        o.set("file", s.file);
        o.set("line", static_cast<std::int64_t>(s.line));
        o.set("col", static_cast<std::int64_t>(s.col));
        if (s.parent.empty())
        {
            o.set_null("parent");
        }
        else
        {
            o.set("parent", s.parent);
        }
        return o;
    }

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

    /** @brief callers 零结果时的盲区三要素（置信标注）。 */
    struct CallersBlindSpots
    {
        bool pointer_calls_invisible = true; // 机制性盲区：恒真
        bool sibling_dispatch_possible = false;
        bool member_address_taken = false;
    };

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

        // 外部边界 RTA 标注（root set 线，context 展示用）：接口跨索引边界的事实行。
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

        const auto block = [&](std::string_view                           title,
                               const std::vector<cg::graph::ContextEdge>& edges)
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
                    view.dispatch_mode == cg::graph::DispatchMode::Off
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
        };
        block("incoming", view.incoming);
        block("outgoing", view.outgoing);
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
     * @internal 边按类型分组到达（cg.graph 的 context 以 id 序遍历类型）；
     * 按首见顺序每类型发一个数组。
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

        const auto bucket = [&](bool incoming)
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
        };
        root.set("incoming", bucket(true));
        root.set("outgoing", bucket(false));
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

    /**
     * @brief "还能更深"注记行：truncated 语义修正后的 BFS 前沿信号承接。
     *
     * 定义：truncated 只在结果被截断（找到的条目多于列出的条目）时为
     *        true；graph 层的同名历史位（--depth 上限处前沿非空）不是
     *        截断——列出的条目本身是完整的，缺的只是更深的层。
     *
     * soundness 边界：走查本身没有条目上限（找到即全部到达）；
     *        neighbors/impact 的 --top 是展示端截断，截断处 truncated 按
     *        "列出 < 找到"实算，计数仍覆盖全部找到的条目；changes/
     *        call_closure 无条目上限仍恒 false；cycles 的 --max 另行渲染
     *        "N of M groups"。本注记独立表达 --depth 上限处的前沿信号，
     *        与截断互不替代。
     *
     * fallback 清单：文本输出 = 本注记行（仅前沿非空时出现）；--json =
     *        deeperEdgesExist 布尔位（恒出现，与 truncated 并列），机器
     *        消费方以此区分"结果被截"与"还能更深"。
     */
    [[nodiscard]]
    auto deeper_note(bool frontier_live) -> std::string
    {
        if (!frontier_live)
        {
            return {};
        }
        return "deeper edges exist: yes (listed entries are complete; raise "
               "--depth to expand)\n";
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

    /**
     * @brief stats 渲染在 cg::db::Stats 之外所需的数据：文件尺寸与索引时
     *        记录的项目根。
     */
    struct StatsInputs
    {
        std::string  db_path;
        std::int64_t db_bytes = 0;
        std::int64_t wal_bytes = 0;
        std::string  root; // meta project_root（缺省 ""）
        Stats        st;
        /** @brief parse_status = failed 的文件路径（stats
         * 点名，外场反馈要求）。 */
        std::vector<std::string> failed_files;
        /** @brief 延迟收敛：派发数据滞后文件数（meta dispatch_stale）。 */
        std::int64_t dispatch_stale_files = 0;
        /**
         * @brief 槽侧快照状态（meta analysis_snap_*；无快照批保持缺省）。
         * @internal state 空串 = 从未写过快照；"hit" = 最近批恢复命中；
         *           "miss:`<原因>`" = 最近批弃快照走全表读。
         */
        std::string  snap_state;
        std::int64_t snap_restore_us = 0;
        std::int64_t snap_dirty_files = 0;
        std::int64_t snap_rebuilt_slots = 0;
        std::int64_t snap_bytes = 0;
    };

    /** @brief stats 的文本摘要。 */
    [[nodiscard]]
    auto stats_text(const StatsInputs& in) -> std::string
    {
        const Stats& st = in.st;
        std::string  out;
        out += std::format(
            "{:<17}{} (schema {}, app {})\n",
            "db",
            in.db_path,
            cg::db::schema_version,
            st.app_version.empty() ? std::string{"?"} : st.app_version);
        const std::string indexed_at =
            st.last_index_at.value_or(std::string{"never"});
        out += std::format(
            "{:<17}{}{}\n",
            "indexed at",
            indexed_at,
            in.root.empty() ? "" : "   root " + in.root);
        out += std::format(
            "{:<17}{} total: {} ok, {} partial, {} failed, {} "
            "pending\n",
            "files",
            st.files,
            st.files_ok,
            st.files_partial,
            st.files_failed,
            st.files_pending);
        // failed
        // 点名（外场反馈要求）：只有计数时覆盖面受损无从评估；首行顶标签、
        // 续行同缩进对齐（本文件 stats 行风格）。--json 同步 failedFiles。
        for (std::size_t i = 0; i < in.failed_files.size(); ++i)
        {
            out += std::format(
                "{:<17}{}\n",
                i == 0 ? std::string{"failed files"} : std::string{},
                in.failed_files[i]);
        }

        out += std::format("{:<17}{}\n", "symbols", st.symbols);
        {
            std::string row;
            for (int k = 0; k < cg::db::kind_count; ++k)
            {
                if (st.symbols_by_kind[k] == 0)
                {
                    continue;
                }
                row += std::format(
                    "{} {}  ",
                    cg::db::name_of(static_cast<SymbolKind>(k)),
                    st.symbols_by_kind[k]);
            }
            if (!row.empty())
            {
                out += std::format("   {}\n", row);
            }
        }
        out += std::format("{:<17}{}\n", "edges", st.edges);
        {
            std::string row;
            for (int t = 0; t < cg::db::edge_type_count; ++t)
            {
                if (st.edges_by_type[t] == 0)
                {
                    continue;
                }
                row += std::format(
                    "{} {}  ",
                    cg::db::name_of(static_cast<EdgeType>(t)),
                    st.edges_by_type[t]);
            }
            if (!row.empty())
            {
                out += std::format("   {}\n", row);
            }
        }
        out += std::format(
            "{:<17}{} (angled {}, quoted {})\n",
            "includes",
            st.includes,
            st.includes_angled,
            st.includes - st.includes_angled);
        out += std::format("{:<17}{}\n", "imports", st.imports);
        if (st.type_hints != 0 || st.call_sites != 0 || st.analysis_rounds != 0)
        {
            out += std::format(
                "{:<17}{} type hints, {} call sites\n",
                "analysis",
                st.type_hints,
                st.call_sites);
            out += std::format(
                "{:<17}{} rounds, {} work items{}\n",
                "fixed point",
                st.analysis_rounds,
                st.analysis_work_items,
                st.analysis_budget_exhausted ? " (budget exhausted)" : "");
            if (in.dispatch_stale_files > 0)
            {
                out += std::format(
                    "{:<17}{} file{} stale (deferred; run `ccg update --drain`)\n",
                    "dispatch",
                    in.dispatch_stale_files,
                    in.dispatch_stale_files == 1 ? "" : "s");
            }
            // 槽侧快照（恢复路径的命中与耗时；miss 时一并给原因）。
            if (!in.snap_state.empty())
            {
                out += std::format(
                    "{:<17}{} ({}, {} us restore, {} dirty, {} rebuilt)\n",
                    "slot snapshot",
                    in.snap_state,
                    human_bytes(in.snap_bytes),
                    in.snap_restore_us,
                    in.snap_dirty_files,
                    in.snap_rebuilt_slots);
            }
        }
        out += std::format("{:<17}{} interned\n", "strings", st.strings);
        out += std::format(
            "{:<17}{}   wal {}\n",
            "db size",
            human_bytes(in.db_bytes),
            human_bytes(in.wal_bytes));
        return out;
    }

    /** @brief stats 的 JSON 载荷。 */
    [[nodiscard]]
    auto stats_json(const StatsInputs& in) -> std::string
    {
        const Stats& st = in.st;
        json::Object root;
        json::Object db;
        db.set("path", in.db_path);
        db.set("schema", static_cast<std::int64_t>(cg::db::schema_version));
        db.set("app", st.app_version);
        if (st.last_index_at.has_value())
        {
            db.set("indexedAt", *st.last_index_at);
        }
        else
        {
            db.set_null("indexedAt");
        }
        if (in.root.empty())
        {
            db.set_null("root");
        }
        else
        {
            db.set("root", in.root);
        }
        db.set("sizeBytes", in.db_bytes);
        db.set("walBytes", in.wal_bytes);
        root.set("db", std::move(db));

        json::Object files;
        files.set("total", st.files);
        files.set("ok", st.files_ok);
        files.set("partial", st.files_partial);
        files.set("failed", st.files_failed);
        files.set("pending", st.files_pending);
        if (!in.failed_files.empty())
        {
            json::Array failed;
            for (const std::string& p : in.failed_files)
            {
                failed.add(p);
            }
            files.set("failedFiles", std::move(failed));
        }
        root.set("files", std::move(files));

        json::Object symbols;
        symbols.set("total", st.symbols);
        json::Object by_kind;
        for (int k = 0; k < cg::db::kind_count; ++k)
        {
            if (st.symbols_by_kind[k] != 0)
            {
                by_kind.set(
                    cg::db::name_of(static_cast<SymbolKind>(k)),
                    st.symbols_by_kind[k]);
            }
        }
        symbols.set("byKind", std::move(by_kind));
        root.set("symbols", std::move(symbols));

        json::Object edges;
        edges.set("total", st.edges);
        json::Object by_type;
        for (int t = 0; t < cg::db::edge_type_count; ++t)
        {
            if (st.edges_by_type[t] != 0)
            {
                by_type.set(
                    cg::db::name_of(static_cast<EdgeType>(t)),
                    st.edges_by_type[t]);
            }
        }
        edges.set("byType", std::move(by_type));
        root.set("edges", std::move(edges));

        json::Object includes;
        includes.set("total", st.includes);
        includes.set("angled", st.includes_angled);
        includes.set("quoted", st.includes - st.includes_angled);
        root.set("includes", std::move(includes));

        root.set("imports", st.imports);

        if (st.type_hints != 0 || st.call_sites != 0 || st.analysis_rounds != 0)
        {
            json::Object analysis;
            analysis.set("typeHints", st.type_hints);
            analysis.set("callSites", st.call_sites);
            analysis.set("fixedPointRounds", st.analysis_rounds);
            analysis.set("fixedPointWorkItems", st.analysis_work_items);
            analysis.set("budgetExhausted", st.analysis_budget_exhausted);
            if (in.dispatch_stale_files > 0)
            {
                analysis.set("dispatchStaleFiles", in.dispatch_stale_files);
            }
            if (!in.snap_state.empty())
            {
                json::Object snap;
                snap.set("state", in.snap_state);
                snap.set("restoreUs", in.snap_restore_us);
                snap.set("dirtyFiles", in.snap_dirty_files);
                snap.set("rebuiltSlots", in.snap_rebuilt_slots);
                snap.set("bytes", in.snap_bytes);
                analysis.set("slotSnapshot", std::move(snap));
            }
            root.set("analysis", std::move(analysis));
        }

        json::Object strings;
        strings.set("interned", st.strings);
        root.set("strings", std::move(strings));
        return json::stringify(std::move(root));
    }

    /** @brief index/update 的文本摘要（干跑显示计划行）。 */
    [[nodiscard]]
    auto index_text(
        const cg::graph::IndexSummary& sum,
        std::string_view               db_path) -> std::string
    {
        const std::string roots = cg::util::join(sum.roots, ", ");
        std::string       out = std::format(
            "indexing {} TUs under {} (db: {}, mode: {}{})\n",
            sum.tus,
            roots,
            db_path,
            sum.mode.empty() ? std::string{"full"} : sum.mode,
            sum.dry_run ? ", dry run" : "");
        // 去重后实际 job 数（原始条目可能因 (file, digest) 重复而多于 job；
        // sum.tus 保持原始条目语义，供统计行与既有断言使用）。
        const std::int64_t jobs =
            sum.fresh + sum.stale + sum.skipped + sum.failed;
        const bool all_skipped =
            jobs > 0 && sum.skipped == jobs && sum.fresh == 0 && sum.stale == 0;
        out += std::format(
            "  fresh: {}  stale: {}  skipped: {}  failed: {}\n",
            sum.fresh,
            sum.stale,
            all_skipped ? std::format("all ({})", sum.skipped)
                        : std::format("{}", sum.skipped),
            sum.failed);
        if (sum.dry_run)
        {
            out += std::format(
                "  plan: {} of {} TUs to re-parse (~{} main-file bytes), "
                "purged files: {}\n",
                sum.stale + sum.fresh,
                sum.tus,
                human_bytes(sum.dirty_bytes),
                sum.purged_files);
            out += std::format(
                "  elapsed: {}\n",
                human_elapsed(sum.elapsed_seconds));
            return out;
        }
        std::string types;
        for (int t = 0; t < cg::db::edge_type_count; ++t)
        {
            if (sum.edges_by_type[t] == 0)
            {
                continue;
            }
            types += std::format(
                ", {} {}",
                cg::db::name_of(static_cast<EdgeType>(t)),
                sum.edges_by_type[t]);
        }
        out += std::format(
            "  symbols: {}  edges: {} ({})\n",
            sum.symbols,
            sum.edges,
            types.empty() ? std::string{"none"} : types.substr(2));
        out += std::format(
            "  includes: {}  imports: {}\n",
            sum.includes,
            sum.imports);
        if (!sum.system_includes.empty())
        {
            // 系统头来源降级必须响亮：绝不静默假装 argv 自包含。
            out += std::format("  system-includes: {}\n", sum.system_includes);
        }
        if (sum.failed > 0 && !sum.failure_classes.empty())
        {
            std::string classes;
            for (const auto& [name, count] : sum.failure_classes)
            {
                classes += std::format(", {} x{}", name, count);
            }
            out += std::format(
                "  failure classes: {}\n",
                classes.empty() ? std::string{"none"} : classes.substr(2));
        }
        out += std::format("  unresolved edges dropped: {}\n", sum.unresolved);
        if (sum.type_hints != 0 || sum.call_sites != 0
            || sum.dispatch_edges != 0)
        {
            out += std::format(
                "  analysis: {} type hints, {} call sites, {} dispatch edges, "
                "{} fallback sites; fixed point {} rounds, {} work items{}\n",
                sum.type_hints,
                sum.call_sites,
                sum.dispatch_edges,
                sum.dispatch_fallback_sites,
                sum.analysis_rounds,
                sum.analysis_work_items,
                sum.analysis_budget_exhausted
                    ? " (fixed-point budget exhausted)"
                    : "");
        }
        if (sum.analysis_failed_tus != 0)
        {
            out += std::format(
                "  analysis skipped for {} TU(s) (engine fault; graph rows kept)\n",
                sum.analysis_failed_tus);
        }
        if (sum.dispatch_stale_files > 0)
        {
            out += std::format(
                "  dispatch analysis deferred: {} file{} stale (run `ccg update "
                "--drain` to converge)\n",
                sum.dispatch_stale_files,
                sum.dispatch_stale_files == 1 ? "" : "s");
        }
        if (sum.pch_built != 0 || sum.pch_reused != 0 || sum.pch_fallbacks != 0
            || sum.pch_unusable != 0 || sum.pch_replay_degraded != 0
            || sum.pch_fallback_ok != 0 || sum.pch_fallback_failed != 0)
        {
            // 文本回退两种结局分账（本轮以回退 argv 实际解析的 TU）：救回
            // 与仍失败，总数恒等于两者之和（索引器运行口径聚合）；仍失败的
            // 失败类是 pch-fallback-failed（与纯 unusable 不混计）。
            out += std::format(
                "  pch: {} built, {} reused, {} text fallbacks "
                "({} fallback parse ok, {} still failed), {} unusable, "
                "{} replay-degraded\n",
                sum.pch_built,
                sum.pch_reused,
                sum.pch_fallbacks,
                sum.pch_fallback_ok,
                sum.pch_fallback_failed,
                sum.pch_unusable,
                sum.pch_replay_degraded);
        }
        if (!sum.pch_notes.empty())
        {
            out += "  pch notes:\n";
            for (const std::string& note : sum.pch_notes)
            {
                out += std::format("    {}\n", note);
            }
        }
        if (sum.dialect_fixit_tus != 0 || sum.dialect_fixit_failed_tus != 0)
        {
            // cl 方言宽松特化的诊断驱动改写：compiler fixit 给点、内存
            // 改写、不落盘；仍失败的 TU 照常计入失败类（口径不变）。
            out += std::format(
                "  dialect rewrites: {} TU(s) fixed via compiler fixit{}\n",
                sum.dialect_fixit_tus,
                sum.dialect_fixit_failed_tus != 0
                    ? std::format(
                          ", {} reparse still failed",
                          sum.dialect_fixit_failed_tus)
                    : std::string{});
        }
        if (!sum.dialect_fixit_notes.empty())
        {
            out += "  dialect rewrite notes:\n";
            for (const std::string& note : sum.dialect_fixit_notes)
            {
                out += std::format("    {}\n", note);
            }
        }
        if (sum.encoding_scrub_tus != 0 || sum.encoding_scrub_failed_tus != 0)
        {
            // 字面量内系统 ANSI 码页坏字节的内存转义消毒（MSVC 透传宽容
            // 形态）；仍失败的 TU 照常计入失败类（msvc-lenient-source-
            // encoding）。
            out += std::format(
                "  encoding scrubs: {} TU(s) literal bytes escaped{}\n",
                sum.encoding_scrub_tus,
                sum.encoding_scrub_failed_tus != 0
                    ? std::format(
                          ", {} reparse still failed",
                          sum.encoding_scrub_failed_tus)
                    : std::string{});
        }
        if (!sum.encoding_scrub_notes.empty())
        {
            out += "  encoding scrub notes:\n";
            for (const std::string& note : sum.encoding_scrub_notes)
            {
                out += std::format("    {}\n", note);
            }
        }
        if (sum.benign_errored_tus != 0)
        {
            // "报错但 AST 完好"的 TU 照常索引（良性错误白名单家族）：
            // 与失败类分账——这是被索引成功的 TU，不是失败。
            out += std::format(
                "  benign errors: {} TU(s) indexed despite error "
                "diagnostics (AST intact)\n",
                sum.benign_errored_tus);
        }
        if (!sum.benign_error_notes.empty())
        {
            out += "  benign error notes:\n";
            for (const std::string& note : sum.benign_error_notes)
            {
                out += std::format("    {}\n", note);
            }
        }
        out += std::format(
            "  elapsed: {}   db size: {}\n",
            human_elapsed(sum.elapsed_seconds),
            human_bytes(sum.db_size_bytes));
        return out;
    }

    /**
     * @brief index/update 的 JSON 载荷。
     *
     * extra 附加一个调用方专属成员（update 的项目文件联动把结论放在
     * "projectRefresh" 下——TU 批与联动同轮时摘要与状态必须同载荷出现，
     * 消费者按"末行机器行"约定只读一行）。
     */
    [[nodiscard]]
    auto index_json(
        const cg::graph::IndexSummary&                     sum,
        std::optional<std::pair<std::string, std::string>> extra = std::nullopt,
        std::optional<std::pair<std::string, json::Array>> extra_array =
            std::nullopt) -> std::string
    {
        json::Object root;
        root.set("tus", sum.tus);
        root.set("mode", sum.mode.empty() ? std::string{"full"} : sum.mode);
        root.set("dryRun", sum.dry_run);
        root.set("fresh", sum.fresh);
        root.set("stale", sum.stale);
        root.set("skipped", sum.skipped);
        root.set("failed", sum.failed);
        root.set("purgedFiles", sum.purged_files);
        if (sum.dry_run)
        {
            root.set("dirtyBytes", sum.dirty_bytes);
            root.set("elapsedSeconds", sum.elapsed_seconds);
            return json::stringify(std::move(root));
        }
        root.set("symbols", sum.symbols);
        root.set("edges", sum.edges);
        json::Object by_type;
        for (int t = 0; t < cg::db::edge_type_count; ++t)
        {
            if (sum.edges_by_type[t] != 0)
            {
                by_type.set(
                    cg::db::name_of(static_cast<EdgeType>(t)),
                    sum.edges_by_type[t]);
            }
        }
        root.set("edgesByType", std::move(by_type));
        root.set("includes", sum.includes);
        root.set("imports", sum.imports);
        if (!sum.system_includes.empty())
        {
            root.set("systemIncludes", sum.system_includes);
        }
        if (sum.failed > 0 && !sum.failure_classes.empty())
        {
            json::Array classes;
            for (const auto& [name, count] : sum.failure_classes)
            {
                json::Object c;
                c.set("class", name);
                c.set("count", count);
                classes.add(std::move(c));
            }
            root.set("failureClasses", std::move(classes));
        }
        root.set("unresolvedDropped", sum.unresolved);
        // 指令门槛与头级缓存观测面（启用判定的数据出口）：指令门槛门控
        // 分类计数与头级缓存命中口径。缓存关闭时 headcacheEnabled=0 且命中键缺席。
        {
            json::Object a1;
            a1.set("c1NormalChanges", sum.c1_normal_changes);
            a1.set("c1InstrChanges", sum.c1_instr_changes);
            a1.set("c1FirstSeen", sum.c1_first_seen);
            a1.set("headcacheEnabled", sum.headcache_enabled);
            if (sum.headcache_enabled != 0)
            {
                a1.set("headcacheLookups", sum.headcache_lookups);
                a1.set("headcacheHits", sum.headcache_hits);
                a1.set("headcacheRecords", sum.headcache_records);
            }
            root.set("a1", std::move(a1));
        }
        // 解析期租约心跳的续租计数（真库实测事故修复的观测面）：大库解
        // 析以十分钟计而租约只有百秒级，此计数非零即证明解析段确实有人
        // 在续租（未启用/干跑/内存库保持缺席，不输出键）。
        if (sum.parse_lease_beats > 0)
        {
            root.set("parseLeaseBeats", sum.parse_lease_beats);
        }
        if (sum.type_hints != 0 || sum.call_sites != 0
            || sum.dispatch_edges != 0)
        {
            json::Object analysis;
            analysis.set("typeHints", sum.type_hints);
            analysis.set("callSites", sum.call_sites);
            analysis.set("dispatchEdges", sum.dispatch_edges);
            analysis.set("dispatchFallbackSites", sum.dispatch_fallback_sites);
            analysis.set("fixedPointRounds", sum.analysis_rounds);
            analysis.set("fixedPointWorkItems", sum.analysis_work_items);
            if (sum.analysis_budget_exhausted)
            {
                analysis.set("budgetExhausted", true);
            }
            if (sum.analysis_failed_tus != 0)
            {
                analysis.set("failedTus", sum.analysis_failed_tus);
            }
            root.set("analysis", std::move(analysis));
        }
        // 延迟收敛：编辑批把 RTA/类型传播/dispatch 推迟到收敛批，滞后文件数
        // 必须出现在载荷里——hook 与脚本按此显式告知"派发数据旧了一会"。
        dispatch_stale_json(root, sum.dispatch_stale_files);
        if (sum.pch_built != 0 || sum.pch_reused != 0 || sum.pch_fallbacks != 0
            || sum.pch_unusable != 0 || sum.pch_replay_degraded != 0
            || sum.pch_fallback_ok != 0 || sum.pch_fallback_failed != 0)
        {
            json::Object pch;
            pch.set("built", sum.pch_built);
            pch.set("reused", sum.pch_reused);
            pch.set("textFallbacks", sum.pch_fallbacks);
            pch.set("fallbackParseOk", sum.pch_fallback_ok);
            pch.set("fallbackStillFailed", sum.pch_fallback_failed);
            pch.set("unusable", sum.pch_unusable);
            pch.set("replayDegraded", sum.pch_replay_degraded);
            root.set("pch", std::move(pch));
        }
        if (!sum.pch_notes.empty())
        {
            json::Array notes;
            for (const std::string& note : sum.pch_notes)
            {
                notes.add(note);
            }
            root.set("pchNotes", std::move(notes));
        }
        if (sum.dialect_fixit_tus != 0 || sum.dialect_fixit_failed_tus != 0)
        {
            root.set("dialectRewrites", sum.dialect_fixit_tus);
            if (sum.dialect_fixit_failed_tus != 0)
            {
                root.set(
                    "dialectRewriteStillFailed",
                    sum.dialect_fixit_failed_tus);
            }
        }
        if (!sum.dialect_fixit_notes.empty())
        {
            json::Array notes;
            for (const std::string& note : sum.dialect_fixit_notes)
            {
                notes.add(note);
            }
            root.set("dialectRewriteNotes", std::move(notes));
        }
        if (sum.encoding_scrub_tus != 0 || sum.encoding_scrub_failed_tus != 0)
        {
            root.set("encodingScrubs", sum.encoding_scrub_tus);
            if (sum.encoding_scrub_failed_tus != 0)
            {
                root.set(
                    "encodingScrubStillFailed",
                    sum.encoding_scrub_failed_tus);
            }
        }
        if (!sum.encoding_scrub_notes.empty())
        {
            json::Array notes;
            for (const std::string& note : sum.encoding_scrub_notes)
            {
                notes.add(note);
            }
            root.set("encodingScrubNotes", std::move(notes));
        }
        if (sum.benign_errored_tus != 0)
        {
            root.set("benignErroredTus", sum.benign_errored_tus);
        }
        if (!sum.benign_error_notes.empty())
        {
            json::Array notes;
            for (const std::string& note : sum.benign_error_notes)
            {
                notes.add(note);
            }
            root.set("benignErroredNotes", std::move(notes));
        }
        root.set("elapsedSeconds", sum.elapsed_seconds);
        root.set("dbSizeBytes", sum.db_size_bytes);
        json::Array roots;
        for (const std::string& r : sum.roots)
        {
            roots.add(r);
        }
        root.set("roots", std::move(roots));
        if (extra.has_value())
        {
            root.set(extra->first, extra->second);
        }
        if (extra_array.has_value())
        {
            root.set(extra_array->first, std::move(extra_array->second));
        }
        return json::stringify(std::move(root));
    }

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
        const auto path_of = [&store](cg::db::FileId id)
        {
            const auto row = store.file(id);
            return row ? row->path : std::string{"?"};
        };
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
                out += std::format("  {} -> {}\n", path_of(src), path_of(dst));
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
        const auto path_of = [&store](cg::db::FileId id)
        {
            const auto row = store.file(id);
            return row ? row->path : std::string{"?"};
        };
        json::Array groups;
        for (const cg::graph::CycleGroup& g : res.groups)
        {
            json::Object o;
            json::Array  files;
            for (const cg::db::FileId id : g.files)
            {
                files.add(path_of(id));
            }
            o.set("files", std::move(files));
            json::Array edges;
            for (const auto& [src, dst] : g.edges)
            {
                json::Object edge;
                edge.set("from", path_of(src));
                edge.set("to", path_of(dst));
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
        const auto name_of_or = [&store](cg::db::SymId id)
        {
            const auto row = cg::db::get_symbol(store, id);
            return row ? row->qualified_name : std::format("sym{}", id);
        };
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
            const std::string id = dot_escape(name_of_or(n.id));
            out += n.id == target.id
                       ? std::format("  \"{}\" [fillcolor=\"#ffd166\"];\n", id)
                       : std::format("  \"{}\";\n", id);
        }
        for (const cg::graph::CallClosureEdge& e : closure.edges)
        {
            out += std::format(
                "  \"{}\" -> \"{}\";\n",
                dot_escape(name_of_or(e.src)),
                dot_escape(name_of_or(e.dst)));
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
