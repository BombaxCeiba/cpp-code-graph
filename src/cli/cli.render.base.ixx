/**
 * @module cg.cli.render.base
 *
 * @brief
 * 渲染层共享原语：位置/等级/方向等枚举名、字节与耗时的人类可读化、站点与边标签文本、调用与影响站点摘取、符号
 * JSON 原子、深度前沿注记与派发观测摘要（状态名/置信度附注/计数/滞后提示）。
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

        /** @brief 文件 id 的路径文本；查不到时给 "?"。 */
        [[nodiscard]]
        auto cycle_file_path(GraphStore& store, cg::db::FileId id)
            -> std::string
        {
            const auto row = store.file(id);
            return row ? row->path : std::string{"?"};
        }

        /** @brief 符号 id 的限定名；查不到时给 "sym<id>" 占位。 */
        [[nodiscard]]
        auto closure_symbol_name(GraphStore& store, cg::db::SymId id)
            -> std::string
        {
            const auto row = cg::db::get_symbol(store, id);
            return row ? row->qualified_name : std::format("sym{}", id);
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

    using cg::graph::DispatchMode;
    using cg::graph::DispatchStatus;

    /** @brief callers 零结果时的盲区三要素（置信标注）。 */
    struct CallersBlindSpots
    {
        bool pointer_calls_invisible = true; // 机制性盲区：恒真
        bool sibling_dispatch_possible = false;
        bool member_address_taken = false;
    };

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

    /** @brief 单个符号的 JSON 对象（query/target/from/to/hops 共享）。 */
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
     * dispatch_stale_files > 0（编辑批延迟了
     * RTA/类型传播/dispatch、收敛批未跑）
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
} // namespace cg::cli::render
