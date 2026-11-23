module;
#include "db.tables.h"

/**
 * @module cg.db.query.batch
 *
 * @brief 批量边查询域：按块构建的 IN 形状批量进出边、双向边与成员种类批量装载。
 */
export module cg.db.query.batch;

import std;
import cg.util;
import sqlpp23.core;
import sqlpp23.sqlite3;
import cg.db.schema;
import cg.db.store;
import cg.db.query.lookup;

namespace cg::db
{

    /**
     * @internal 批量 IN 分块上限：防 SQLite 绑定变量数上限（默认 999）；
     * 与 symbol_briefs 的分块常量同值同理由。
     */
    inline constexpr std::size_t EDGE_BATCH_CHUNK = 400;

    /**
     * @brief 一批符号的出边聚合（callee 方向；每符号段的聚合语义与
     *        edges_from 逐符号版逐字相同）。
     *
     * @internal 参数个数随批可变，进不了固定形状的预编译表，按块
     * 现场构建。结果行按 (self, 对端, 类型) 升序——按 self 分段消费时段内
     * 行序与逐符号版（索引自然序）逐位一致（冻结语料上四形状 900 符号
     * 全等实证）；显式排序不依赖 SQLite 计划细节。类型过滤在消费端做，
     * 与逐符号版等价（丢组 = SQL 层 WHERE type IN）。
     */
    export auto edges_from_batch(
        GraphStore&               store,
        std::span<const SymId>    srcs,
        std::span<const EdgeType> types) -> std::vector<EdgeAggBySym>
    {
        const tab::Edges          e{};
        std::vector<EdgeAggBySym> out;
        for (std::size_t base = 0; base < srcs.size(); base += EDGE_BATCH_CHUNK)
        {
            const std::size_t count =
                std::min(EDGE_BATCH_CHUNK, srcs.size() - base);
            const std::vector<std::int64_t> chunk(
                srcs.begin() + static_cast<std::ptrdiff_t>(base),
                srcs.begin() + static_cast<std::ptrdiff_t>(base + count));
            auto rows =
                store.conn()(sqlpp::select(
                                 e.srcId,
                                 e.dstId,
                                 e.typeId,
                                 sqlpp::sum(e.occurrences).as(tab::occ_sum),
                                 sqlpp::min(e.siteLine).as(tab::site_min),
                                 sqlpp::max(e.confidence).as(tab::max_conf),
                                 sqlpp::max(e.attrs).as(tab::max_attrs))
                                 .from(e)
                                 .where(e.srcId.in(chunk))
                                 .group_by(e.srcId, e.dstId, e.typeId));
            for (const auto& row : rows)
            {
                out.push_back(
                    {.self = static_cast<SymId>(row.srcId),
                     .row = {
                         .other = static_cast<SymId>(row.dstId),
                         .type = edge_type_of(
                             static_cast<std::int64_t>(row.typeId)),
                         .occurrences_sum = row.occ_sum.value_or(0),
                         .min_site_line = row.site_min.value_or(0),
                         .confidence = row.max_conf.value_or(1.0),
                         .attrs = static_cast<std::uint32_t>(
                             static_cast<std::int64_t>(
                                 row.max_attrs.value_or(0)))}});
            }
        }
        if (!types.empty())
        {
            std::erase_if(
                out,
                [&types](const EdgeAggBySym& r)
                {
                    return std::ranges::find(types, r.row.type) == types.end();
                });
        }
        std::ranges::sort(
            out,
            [](const EdgeAggBySym& a, const EdgeAggBySym& b)
            {
                if (a.self != b.self)
                {
                    return a.self < b.self;
                }
                if (a.row.other != b.row.other)
                {
                    return a.row.other < b.row.other;
                }
                return to_id(a.row.type) < to_id(b.row.type);
            });
        return out;
    }

    /**
     * @brief 一批符号的入边聚合（caller 方向）；行序约定与等价性论证同
     *        edges_from_batch（(self, 对端, 类型) 升序）。
     */
    export auto edges_to_batch(
        GraphStore&               store,
        std::span<const SymId>    dsts,
        std::span<const EdgeType> types) -> std::vector<EdgeAggBySym>
    {
        const tab::Edges          e{};
        std::vector<EdgeAggBySym> out;
        for (std::size_t base = 0; base < dsts.size(); base += EDGE_BATCH_CHUNK)
        {
            const std::size_t count =
                std::min(EDGE_BATCH_CHUNK, dsts.size() - base);
            const std::vector<std::int64_t> chunk(
                dsts.begin() + static_cast<std::ptrdiff_t>(base),
                dsts.begin() + static_cast<std::ptrdiff_t>(base + count));
            auto rows =
                store.conn()(sqlpp::select(
                                 e.dstId,
                                 e.srcId,
                                 e.typeId,
                                 sqlpp::sum(e.occurrences).as(tab::occ_sum),
                                 sqlpp::min(e.siteLine).as(tab::site_min),
                                 sqlpp::max(e.confidence).as(tab::max_conf),
                                 sqlpp::max(e.attrs).as(tab::max_attrs))
                                 .from(e)
                                 .where(e.dstId.in(chunk))
                                 .group_by(e.dstId, e.srcId, e.typeId));
            for (const auto& row : rows)
            {
                out.push_back(
                    {.self = static_cast<SymId>(row.dstId),
                     .row = {
                         .other = static_cast<SymId>(row.srcId),
                         .type = edge_type_of(
                             static_cast<std::int64_t>(row.typeId)),
                         .occurrences_sum = row.occ_sum.value_or(0),
                         .min_site_line = row.site_min.value_or(0),
                         .confidence = row.max_conf.value_or(1.0),
                         .attrs = static_cast<std::uint32_t>(
                             static_cast<std::int64_t>(
                                 row.max_attrs.value_or(0)))}});
            }
        }
        if (!types.empty())
        {
            std::erase_if(
                out,
                [&types](const EdgeAggBySym& r)
                {
                    return std::ranges::find(types, r.row.type) == types.end();
                });
        }
        std::ranges::sort(
            out,
            [](const EdgeAggBySym& a, const EdgeAggBySym& b)
            {
                if (a.self != b.self)
                {
                    return a.self < b.self;
                }
                if (a.row.other != b.row.other)
                {
                    return a.row.other < b.row.other;
                }
                return to_id(a.row.type) < to_id(b.row.type);
            });
        return out;
    }

    /**
     * @brief 批量原始边行：self 是查询侧符号（出边查询的 src、入边
     *        查询的 dst），单一类型。
     */
    export struct EdgeRowBySym
    {
        SymId   self = 0;
        EdgeRow row;
    };

    /**
     * @brief 一批符号的单一类型原始边行；行序约定同 edges_of 逐符号版
     *        （出边段内按 (对端, 站点文件, 站点行)，入边段内按 edge_id）。
     */
    export auto edges_of_batch(
        GraphStore&            store,
        std::span<const SymId> ids,
        bool                   outgoing,
        EdgeType               type) -> std::vector<EdgeRowBySym>
    {
        const tab::Edges          e{};
        std::vector<EdgeRowBySym> out;
        for (std::size_t base = 0; base < ids.size(); base += EDGE_BATCH_CHUNK)
        {
            const std::size_t count =
                std::min(EDGE_BATCH_CHUNK, ids.size() - base);
            const std::vector<std::int64_t> chunk(
                ids.begin() + static_cast<std::ptrdiff_t>(base),
                ids.begin() + static_cast<std::ptrdiff_t>(base + count));
            if (outgoing)
            {
                auto rows = store.conn()(
                    sqlpp::select(
                        e.srcId,
                        e.edgeId,
                        e.dstId,
                        e.siteFileId,
                        e.siteLine,
                        e.attrs,
                        e.confidence,
                        e.occurrences)
                        .from(e)
                        .where(e.srcId.in(chunk) and e.typeId == to_id(type)));
                for (const auto& row : rows)
                {
                    out.push_back(
                        {.self = static_cast<SymId>(row.srcId),
                         .row = to_edge_row(row, type)});
                }
            }
            else
            {
                auto rows = store.conn()(
                    sqlpp::select(
                        e.dstId,
                        e.edgeId,
                        e.srcId,
                        e.siteFileId,
                        e.siteLine,
                        e.attrs,
                        e.confidence,
                        e.occurrences)
                        .from(e)
                        .where(e.dstId.in(chunk) and e.typeId == to_id(type)));
                for (const auto& row : rows)
                {
                    out.push_back(
                        {.self = static_cast<SymId>(row.dstId),
                         .row = to_edge_row(row, type)});
                }
            }
        }
        if (outgoing)
        {
            std::ranges::sort(
                out,
                [](const EdgeRowBySym& a, const EdgeRowBySym& b)
                {
                    if (a.self != b.self)
                    {
                        return a.self < b.self;
                    }
                    if (a.row.dst != b.row.dst)
                    {
                        return a.row.dst < b.row.dst;
                    }
                    if (a.row.site_file != b.row.site_file)
                    {
                        return a.row.site_file < b.row.site_file;
                    }
                    return a.row.site_line < b.row.site_line;
                });
        }
        else
        {
            std::ranges::sort(
                out,
                [](const EdgeRowBySym& a, const EdgeRowBySym& b)
                {
                    if (a.self != b.self)
                    {
                        return a.self < b.self;
                    }
                    return a.row.id < b.row.id;
                });
        }
        return out;
    }

    /**
     * @brief 批量成员行：记录的直接成员的 (id, kind)——记录展开只
     *        需要这两个字段。
     */
    export struct MemberKindRow
    {
        SymId      parent = 0;
        SymId      id = 0;
        SymbolKind kind = SymbolKind::Record;
    };

    /**
     * @brief 一批记录的全部直接成员（段内按 sym_id 升序，与 members 逐
     *        记录版的返回序一致）；本函数不筛 kind，调用方按需过滤。
     */
    export auto member_kinds_batch(
        GraphStore&            store,
        std::span<const SymId> parents) -> std::vector<MemberKindRow>
    {
        const tab::Symbols         s{};
        std::vector<MemberKindRow> out;
        for (std::size_t base = 0; base < parents.size();
             base += EDGE_BATCH_CHUNK)
        {
            const std::size_t count =
                std::min(EDGE_BATCH_CHUNK, parents.size() - base);
            const std::vector<std::int64_t> chunk(
                parents.begin() + static_cast<std::ptrdiff_t>(base),
                parents.begin() + static_cast<std::ptrdiff_t>(base + count));
            auto rows =
                store.conn()(sqlpp::select(s.parentId, s.symId, s.kindId)
                                 .from(s)
                                 .where(s.parentId.in(chunk)));
            for (const auto& row : rows)
            {
                // parentId 参与了 WHERE IN 匹配，此处必非空（NULL 不等于任何
                // 绑定值），安全解引用。
                out.push_back(
                    {.parent = static_cast<SymId>(*row.parentId),
                     .id = static_cast<SymId>(row.symId),
                     .kind = symbol_kind_of(
                         static_cast<std::int64_t>(row.kindId))});
            }
        }
        std::ranges::sort(
            out,
            [](const MemberKindRow& a, const MemberKindRow& b)
            {
                if (a.parent != b.parent)
                {
                    return a.parent < b.parent;
                }
                return a.id < b.id;
            });
        return out;
    }
} // namespace cg::db
