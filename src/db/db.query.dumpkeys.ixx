module;
#include "db.tables.h"

/**
 * @module cg.db.query.dumpkeys
 *
 * @brief 键定位与转储域：文件符号键、子符号、dump
 * 明细、按名/键定位、名字区间、caller 边行、虚派发站点与间接站点、实参目标行。
 */
export module cg.db.query.dumpkeys;

import std;
import cg.util;
import sqlpp23.core;
import sqlpp23.sqlite3;
import cg.db.schema;
import cg.db.store;
import cg.db.query.lookup;
import cg.db.query.batch;

namespace cg::db
{

    /**
     * @brief 定义在 `file` 中的每个符号的 (sym_id, unique_key_id) 对——按
     *        文件替换时，拿它们与新抽取的键分类，在触碰任何行之前找出
     *        注定被删的符号。
     */
    export struct FileSymbolKey
    {
        SymId id = 0;
        StrId key = 0;
    };

    export auto file_symbol_keys(GraphStore& store, FileId file)
        -> std::vector<FileSymbolKey>
    {
        const tab::Symbols         s{};
        std::vector<FileSymbolKey> out;
        auto rows = store.conn()(sqlpp::select(s.symId, s.uniqueKeyId)
                                     .from(s)
                                     .where(s.fileId == file));
        for (const auto& row : rows)
        {
            out.push_back(
                {.id = static_cast<SymId>(row.symId),
                 .key = static_cast<StrId>(row.uniqueKeyId)});
        }
        return out;
    }

    /**
     * @brief parent_id 在 `ids` 之中的符号（父级联展开的一步：清除一条
     *        记录会级联到定义在别处的成员）。
     */
    export auto child_symbols(GraphStore& store, std::span<const SymId> ids)
        -> std::vector<SymId>
    {
        if (ids.empty())
        {
            return {};
        }
        const std::vector<std::int64_t> id_list{ids.begin(), ids.end()};
        const tab::Symbols              s{};
        std::vector<SymId>              out;
        auto                            rows = store.conn()(
            sqlpp::select(s.symId).from(s).where(s.parentId.in(id_list)));
        for (const auto& row : rows)
        {
            out.push_back(static_cast<SymId>(row.symId));
        }
        return out;
    }

    /**
     * @brief 整表文本投影组（等价性门禁）：symbols/edges/includes 每行的
     *        规范文本，按主键排序（includes：按内容排序，该表无暴露 id）。
     *
     * @internal
     * 每个字符串覆盖行 id 与每个存储列，因此比较两份 dump 即证明两次索引
     * 运行产出相同的行——并行解析池背后的确定性契约（同样输入、同样行、
     * 任意工人顺序）。
     */
    export auto dump_symbols(GraphStore& store) -> std::vector<std::string>
    {
        const tab::Symbols                         s{};
        std::vector<std::pair<SymId, std::string>> rows_out;
        auto rows = store.conn()(sqlpp::select(sqlpp::all_of(s)).from(s));
        for (const auto& row : rows)
        {
            rows_out.emplace_back(
                static_cast<SymId>(row.symId),
                std::format(
                    "{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}",
                    static_cast<std::int64_t>(row.symId),
                    store.str_of(static_cast<StrId>(row.uniqueKeyId)),
                    static_cast<std::int64_t>(row.kindId),
                    store.str_of(static_cast<StrId>(row.nameId)),
                    store.str_of(static_cast<StrId>(row.nsId)),
                    row.sigHash.has_value() ? std::format("{}", *row.sigHash)
                                            : std::string{"~"},
                    static_cast<std::int64_t>(row.fileId),
                    static_cast<std::int64_t>(row.line),
                    static_cast<std::int64_t>(row.col),
                    row.parentId.has_value() ? std::format("{}", *row.parentId)
                                             : std::string{"~"},
                    static_cast<std::int64_t>(row.flags),
                    std::format(
                        "{}|{}",
                        row.paramTypes.has_value()
                            ? std::string_view{*row.paramTypes}
                            : "~",
                        row.templateParams.has_value()
                            ? std::string_view{*row.templateParams}
                            : "~")));
        }
        std::ranges::sort(
            rows_out,
            [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<std::string> out;
        out.reserve(rows_out.size());
        for (auto& [id, text] : rows_out)
        {
            out.push_back(std::move(text));
        }
        return out;
    }

    export auto dump_edges(GraphStore& store) -> std::vector<std::string>
    {
        const tab::Edges                            e{};
        std::vector<std::pair<EdgeId, std::string>> rows_out;
        auto rows = store.conn()(sqlpp::select(sqlpp::all_of(e)).from(e));
        for (const auto& row : rows)
        {
            rows_out.emplace_back(
                static_cast<EdgeId>(row.edgeId),
                std::format(
                    "{}|{}|{}|{}|{}|{}|{}|{}|{}",
                    static_cast<std::int64_t>(row.edgeId),
                    static_cast<std::int64_t>(row.srcId),
                    static_cast<std::int64_t>(row.dstId),
                    static_cast<std::int64_t>(row.typeId),
                    static_cast<std::int64_t>(row.siteFileId),
                    static_cast<std::int64_t>(row.siteLine),
                    static_cast<std::int64_t>(row.attrs),
                    static_cast<double>(row.confidence),
                    static_cast<std::int64_t>(row.occurrences)));
        }
        std::ranges::sort(
            rows_out,
            [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<std::string> out;
        out.reserve(rows_out.size());
        for (auto& [id, text] : rows_out)
        {
            out.push_back(std::move(text));
        }
        return out;
    }

    export auto dump_includes(GraphStore& store) -> std::vector<std::string>
    {
        const tab::Includes      inc{};
        std::vector<std::string> out;
        auto rows = store.conn()(sqlpp::select(sqlpp::all_of(inc)).from(inc));
        for (const auto& row : rows)
        {
            const auto fr = store.file(static_cast<FileId>(row.fileId));
            out.push_back(
                std::format(
                    "{}|{}|{}|{}",
                    fr ? fr->path : std::string{"?"},
                    store.str_of(static_cast<StrId>(row.targetId)),
                    static_cast<std::int64_t>(row.line),
                    static_cast<std::int64_t>(row.angled)));
        }
        std::ranges::sort(out);
        return out;
    }

    /**
     * @brief 每条 imports 行的规范文本，按内容排序（增量等价 dump 面的
     *        模块一半）。
     */
    export auto dump_imports(GraphStore& store) -> std::vector<std::string>
    {
        const tab::Imports       imp{};
        std::vector<std::string> out;
        auto rows = store.conn()(sqlpp::select(sqlpp::all_of(imp)).from(imp));
        for (const auto& row : rows)
        {
            const auto fr = store.file(static_cast<FileId>(row.fileId));
            out.push_back(
                std::format(
                    "{}|{}|{}",
                    fr ? fr->path : std::string{"?"},
                    store.str_of(static_cast<StrId>(row.moduleId)),
                    static_cast<std::int64_t>(row.line)));
        }
        std::ranges::sort(out);
        return out;
    }

    /** @brief schema v10 类型线索整表读取（确定性顺序由 store 保证）。 */
    export auto all_type_hints(GraphStore& store) -> std::vector<TypeHintRow>
    {
        return store.all_type_hints();
    }

    /** @brief schema v10 调用点整表读取（确定性顺序由 store 保证）。 */
    export auto all_call_sites(GraphStore& store) -> std::vector<CallSiteRow>
    {
        return store.all_call_sites();
    }

    /**
     * @brief 按 str.id 升序拉取一段"被符号名引用的 str 行"——受限正则
     *        路径的分段全表扫描输入。
     *
     * @internal 返回 str.id 严格大于 `after` 的前 `limit` 行连接产物
     *           （str.id 升序，只取被 symbols.nameId 引用的行）；返回空段
     *           = 全表拉尽。调用侧循环拉段、段间查截止探针后可安全放弃
     *           后续段——每段 SQL 独立跑完，满足全店不变式（结果循环跑到
     *           DONE）。不做 DISTINCT/GROUP BY（SQL 端去重实测慢 4 倍；
     *           同名符号共享 str 行，连接会按符号数产出重复行，行序按
     *           str.id 排列使重复相邻，由调用侧按 str 行 id 归并）。
     */
    export auto symbol_names_span(
        GraphStore& store,
        StrId       after,
        std::size_t limit) -> std::vector<std::pair<StrId, std::string>>
    {
        const tab::Str                             str{};
        const tab::Symbols                         s{};
        std::vector<std::pair<StrId, std::string>> out;
        auto                                       rows =
            store.conn()(sqlpp::select(str.id, str.s)
                             .from(s.inner_join(str).on(s.nameId == str.id))
                             .where(str.id > after)
                             .order_by(str.id.asc())
                             .limit(static_cast<std::uint64_t>(limit)));
        for (const auto& row : rows)
        {
            out.emplace_back(static_cast<StrId>(row.id), std::string{row.s});
        }
        return out;
    }

    /**
     * @brief 指向 `dst` 的调用类原始边行（calls + virtual_dispatch 合并）。
     *
     * @internal 按 edge_id 升序（＝插入序）：调用方聚合并列出现次数时，稳
     * 定排序的并列次序必须与“先见到者先列”的既定口径一致（PreToolUse 注
     * 入的 callers top3 字节锁依赖这一顺序）。
     */
    export auto caller_edge_rows(GraphStore& store, SymId dst)
        -> std::vector<EdgeRow>
    {
        const tab::Edges     e{};
        std::vector<EdgeRow> out;
        auto                 rows =
            store.conn()(sqlpp::select(sqlpp::all_of(e))
                             .from(e)
                             .where(
                                 e.dstId == dst
                                 and e.typeId.in(
                                     std::vector<std::int64_t>{
                                         to_id(EdgeType::Calls),
                                         to_id(EdgeType::VirtualDispatch)}))
                             .order_by(e.edgeId.asc()));
        for (const auto& row : rows)
        {
            out.push_back(
                {.id = static_cast<EdgeId>(row.edgeId),
                 .src = static_cast<SymId>(row.srcId),
                 .dst = static_cast<SymId>(row.dstId),
                 .type = edge_type_of(static_cast<std::int64_t>(row.typeId)),
                 .site_file = static_cast<FileId>(row.siteFileId),
                 .site_line = static_cast<int>(row.siteLine),
                 .attrs = static_cast<std::uint32_t>(
                     static_cast<std::int64_t>(row.attrs)),
                 .confidence = row.confidence,
                 .occurrences = static_cast<std::int64_t>(row.occurrences)});
        }
        return out;
    }

    /**
     * @brief 以 `method_id` 为派发方法名的虚调用点（is_virtual = 1）。
     *
     * @internal 走 call_sites_method 部分索引；PreToolUse 注入的 dispatch 行
     * （站点数 / 已解析数 / 候选置信分档）只消费这些行。
     */
    export auto virtual_sites_of_method(GraphStore& store, SymId method_id)
        -> std::vector<CallSiteRow>
    {
        const tab::CallSites     cs{};
        std::vector<CallSiteRow> out;
        auto                     rows = store.conn()(
            sqlpp::select(
                cs.siteId,
                cs.callerId,
                cs.methodId,
                cs.methodKey,
                cs.isVirtual,
                cs.fileId,
                cs.line,
                cs.col,
                cs.resolved,
                cs.candidates,
                cs.fallbackReason)
                .from(cs)
                .where(cs.methodId == method_id and cs.isVirtual == 1)
                .order_by(cs.siteId.asc()));
        for (const auto& row : rows)
        {
            CallSiteRow r;
            r.id = static_cast<std::int64_t>(row.siteId);
            r.caller_id = static_cast<SymId>(row.callerId);
            r.method_id = method_id;
            r.method_key = std::string{row.methodKey};
            r.is_virtual = true;
            r.file_id = static_cast<FileId>(row.fileId);
            r.line = static_cast<int>(row.line);
            r.col = static_cast<int>(row.col);
            r.resolved = static_cast<std::int64_t>(row.resolved) != 0;
            r.candidates = std::string{row.candidates};
            r.fallback_reason = std::string{row.fallbackReason};
            out.push_back(std::move(r));
        }
        return out;
    }

    /** @brief indirect 消费行：间接站点 + 调用者 key 文本（消费 join 用）。 */
    export struct IndirectSiteRow
    {
        SymId       caller_id = 0;
        std::string caller_key;
        FileId      file_id = 0;
        int         line = 0;
        int         callee_param = -1;
        std::string callee_expr;
        std::string receiver_owner_key;
        std::string arg_targets; // JSON 数组
    };

    /** @brief 间接调用站点（带被调用表达式锚点）+ 调用者 unique_key 文本。 */
    export auto indirect_site_rows(GraphStore& store)
        -> std::vector<IndirectSiteRow>
    {
        const tab::CallSites         cs{};
        const tab::Symbols           s{};
        const tab::Str               str{};
        std::vector<IndirectSiteRow> out;
        auto                         rows = store.conn()(
            sqlpp::select(
                cs.callerId,
                str.s,
                cs.fileId,
                cs.line,
                cs.calleeParam,
                cs.calleeExpr,
                cs.receiverOwnerKey,
                cs.argTargets)
                .from(cs.inner_join(s)
                          .on(cs.callerId == s.symId)
                          .inner_join(str)
                          .on(s.uniqueKeyId == str.id))
                .where(cs.indirect == 1 and cs.calleeExpr != "")
                .order_by(cs.callerId.asc(), cs.fileId.asc(), cs.line.asc()));
        for (const auto& row : rows)
        {
            IndirectSiteRow r;
            r.caller_id = static_cast<SymId>(row.callerId);
            r.caller_key = std::string{row.s};
            r.file_id = static_cast<FileId>(row.fileId);
            r.line = static_cast<int>(row.line);
            r.callee_param = static_cast<int>(row.calleeParam);
            r.callee_expr = std::string{row.calleeExpr};
            r.receiver_owner_key = std::string{row.receiverOwnerKey};
            r.arg_targets = std::string{row.argTargets};
            out.push_back(std::move(r));
        }
        return out;
    }

    /** @brief 全体带函数目标的调用点（形参直配数据源）。
     *
     * @internal 键语义：消费侧要回答"调用 runner 的人传了什么目标"——
     * 索引必须按【被调者 callee_id】，不是 caller（后期修正：原按
     * caller_id 索引把 call_runner 的实参错配给 runner 自己的站点）。
     */
    export auto arg_target_rows(GraphStore& store)
        -> std::vector<std::pair<SymId, std::string>>
    {
        const tab::CallSites                       cs{};
        std::vector<std::pair<SymId, std::string>> out;
        auto                                       rows =
            store.conn()(sqlpp::select(cs.calleeId, cs.argTargets)
                             .from(cs)
                             .where(cs.argTargets != "[]" and cs.calleeId != 0)
                             .order_by(cs.calleeId.asc()));
        for (const auto& row : rows)
        {
            if (!row.calleeId.has_value())
            {
                continue; // callee 未解析的调用点：无被调者可索引
            }
            out.emplace_back(
                static_cast<SymId>(*row.calleeId),
                std::string{row.argTargets});
        }
        return out;
    }
} // namespace cg::db
