module;
#include "../db/db.tables.h"

/**
 * @module cg.cli.render.dispatch
 *
 * @brief 派发观测渲染：状态与模式名、置信度附注、派发计数摘要文本与 JSON、stale
 * 提示。
 */
export module cg.cli.render.dispatch;

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
