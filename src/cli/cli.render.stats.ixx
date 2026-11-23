module;
#include "../db/db.tables.h"

/**
 * @module cg.cli.render.stats
 *
 * @brief 统计与索引概览渲染：StatsInputs、stats/index 的文本与 JSON。
 */
export module cg.cli.render.stats;

import std;
import cg.util;
import cg.db;
import cg.graph;
import cg.analysis.model;
import cg.cli.parse;
import cg.cli.render.base;
import cg.cli.render.dispatch;

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
        // 分类计数与头级缓存命中口径。缓存关闭时 headcacheEnabled=0
        // 且命中键缺席。
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
} // namespace cg::cli::render
