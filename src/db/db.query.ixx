module;
#include "db.tables.h"

/**
 * @module cg.db.query
 *
 * @brief 存储层的读侧：分层符号查找（层 1-3：精确、限定后缀、子串；
 *        命中层合并）、符号/文件行物化辅助、caller/callee 边聚合（每对
 *        MIN(site_line)、SUM(occurrences)）、对间原始边、取址证据行、
 *        记录成员、extends 关系、统计与表计数。
 *
 * @internal
 * 热形状读语句（edges_from/to/of）走 GraphStore 的预编译语句表；其
 * 余查询仍是每次调用现场构建的类型化 sqlpp23 语句。批量 IN 形状因参
 * 数个数可变无法进固定形状的预编译表，按块现场构建。
 */
export module cg.db.query;

import std;
import cg.util;
import sqlpp23.core;
import sqlpp23.sqlite3;
import cg.db.schema;
import cg.db.store;

namespace cg::db
{

    namespace
    {

        /**
         * @brief 非限定名恰为 `name` 的符号（每个名字至多一条 str 行；多个符号
         *        可共享它）。
         */
        auto ids_where_name_is(GraphStore& store, std::string_view name)
            -> std::vector<SymId>
        {
            const tab::Str     str{};
            const tab::Symbols s{};
            std::vector<SymId> out;
            auto               str_rows = store.conn()(
                sqlpp::select(str.id).from(str).where(str.s == name));
            for (const auto& sr : str_rows)
            {
                auto sym_rows = store.conn()(
                    sqlpp::select(s.symId).from(s).where(s.nameId == sr.id));
                for (const auto& row : sym_rows)
                {
                    out.push_back(static_cast<SymId>(row.symId));
                }
            }
            return out;
        }

        /**
         * @brief unique_key 恰为 `key` 的符号（若有）——层 1 精确匹配的高手
         *        逃生口（按完整唯一键的精确文本匹配，与名字精确匹配同层）。
         */
        auto id_where_unique_key_is(GraphStore& store, std::string_view key)
            -> std::optional<SymId>
        {
            const tab::Str     str{};
            const tab::Symbols s{};
            auto               str_rows = store.conn()(
                sqlpp::select(str.id).from(str).where(str.s == key));
            // 结果循环跑到完成（DONE）再返回：中途 return 会把语句留在
            // 步进中，其隐式读事务钉住连接的读快照——并发提交后本连接
            // 的写升级以 BUSY_SNAPSHOT 永久瞬时失败（结果循环纪律）。
            std::optional<SymId> first;
            for (const auto& sr : str_rows)
            {
                auto sym_rows =
                    store.conn()(sqlpp::select(s.symId).from(s).where(
                        s.uniqueKeyId == sr.id));
                for (const auto& row : sym_rows)
                {
                    if (!first.has_value())
                    {
                        first = static_cast<SymId>(row.symId);
                    }
                }
            }
            return first;
        }

        /**
         * @brief 非限定名包含 `pattern` 的符号。
         *
         * @internal SQL LIKE 负责收窄候选；调用方在 C++ 里重新做大小写敏感
         * 检查，因为 SQLite 的 LIKE 默认对 ASCII 大小写不敏感。
         */
        auto ids_where_name_like(GraphStore& store, std::string_view pattern)
            -> std::vector<SymId>
        {
            const tab::Str     str{};
            const tab::Symbols s{};
            std::vector<SymId> out;
            auto               rows =
                store.conn()(sqlpp::select(s.symId)
                                 .from(s.inner_join(str).on(s.nameId == str.id))
                                 .where(str.s.like(std::string{pattern})));
            for (const auto& row : rows)
            {
                out.push_back(static_cast<SymId>(row.symId));
            }
            return out;
        }

        auto passes(const SymbolRow& row, const SymbolFilter& filter) -> bool
        {
            if (filter.kind.has_value() && row.kind != *filter.kind)
            {
                return false;
            }
            if (filter.file_substring.has_value()
                && row.file.find(*filter.file_substring) == std::string::npos)
            {
                return false;
            }
            return true;
        }

        auto materialize(
            GraphStore&            store,
            std::span<const SymId> ids,
            const SymbolFilter&    filter) -> std::vector<SymbolRow>
        {
            std::vector<SymbolRow> out;
            for (const SymId id : ids)
            {
                if (auto row = store.symbol_row(id);
                    row && passes(*row, filter))
                {
                    out.push_back(std::move(*row));
                }
            }
            return out;
        }

    } // namespace

    /**
     * @brief 分层查找（层合并版）。
     *
     * 层 1 精确：实参等于非限定名、完整限定名或 unique key。层 2 限定后
     * 缀：实参匹配限定名跨 "::" 边界的尾部。层 3 子串：名字或限定名的
     * ASCII 大小写不敏感子串。模糊层 4 落在 cg.cli。
     *
     * @internal
     * 定义（层级独占修复）：merge_lower_tiers = true（ccg query 的
     * 展示口径）时三层全部求值，命中按层 1 → 2 → 3 的顺序并入、按符号
     * id 去重，`tier` 报告最浅命中层（存在精确命中即 1）；false = 旧短路
     * 语义（首个有命中的层独占），供唯一目标解析与 hook 的限时子进程走
     * 零回归路径。旧独占语义曾让小写 "task" 查询被大小写敏感层 3 的
     * Pretask 命中独占、真正相关的 TaskRecord 无人接住；现在大小写不敏
     * 感层 3 同时召回两者，层 1 命中时下层的相关命中也一并列出，逐行所
     * 属层由渲染层按同判据标注。
     * soundness 边界：层 1/2 保持大小写敏感（精确语义不动）；层 3 只折
     * 叠 ASCII 字母（cg::util::ascii_icontains），两档共用——大小写不敏感是检索
     * 口径修复，合并与否只是展示选择；SQL LIKE 收窄覆盖层 3 的非限定名
     * 侧，限定名侧独有命中（查询末段不在任何候选名中）不进池——那是层
     * 4 模糊建议的地盘。
     * fallback 清单：三层全空 → tier 0（调用方出"你是不是想找"）；某层
     * 命中被 --kind/--file 全部滤掉 → 该层不参与（过滤后口径与旧行为
     * 一致）。
     */
    export auto find_symbols(
        GraphStore&         store,
        std::string_view    query,
        const SymbolFilter& filter,
        bool                merge_lower_tiers = true) -> FindResult
    {
        const auto             parts = cg::util::split_qualified(query);
        const std::string_view last = parts.empty() ? query : parts.back();

        std::vector<SymbolRow>    merged;
        std::unordered_set<SymId> seen;
        const auto                merge = [&](std::vector<SymbolRow>&& rows)
        {
            for (SymbolRow& row : rows)
            {
                if (seen.insert(row.id).second)
                {
                    merged.push_back(std::move(row));
                }
            }
        };
        int tier = 0;

        // 层 1 与层 2 的名字查询同参（ids_where_name_is(store, last)）：
        // 查一次共用，层 1 在其副本上并入 unique key 命中（层 2 口径不含
        // key 命中，保持原集合）。
        const std::vector<SymId> by_name = ids_where_name_is(store, last);

        // 行物化缓存：同一符号的行三层只查一次 store.symbol_row（每行
        // 多趟 SQL：主键行、字符串解析、路径与父链限定名；只读路径重复
        // 物化产出相同行）；层 3 的 LIKE 池沿池序只补查池中未见过的行。
        std::unordered_map<SymId, SymbolRow> row_of;
        const auto materialize_cached = [&](std::span<const SymId> ids)
        {
            std::vector<SymbolRow> out;
            out.reserve(ids.size());
            for (const SymId id : ids)
            {
                const SymbolRow* row = nullptr;
                if (auto it = row_of.find(id); it != row_of.end())
                {
                    row = &it->second;
                }
                else if (
                    auto fresh = store.symbol_row(id);
                    fresh && passes(*fresh, filter))
                {
                    row = &row_of.emplace(id, std::move(*fresh)).first->second;
                }
                if (row != nullptr)
                {
                    out.push_back(*row);
                }
            }
            return out;
        };

        {
            std::vector<SymId> candidates = by_name;
            if (const auto by_key = id_where_unique_key_is(store, query))
            {
                candidates.push_back(*by_key);
            }
            auto matches = materialize_cached(candidates);
            std::erase_if(
                matches,
                [&](const SymbolRow& row)
                {
                    return row.name != query && row.qualified_name != query
                           && row.unique_key != query;
                });
            if (!matches.empty())
            {
                tier = 1;
                if (!merge_lower_tiers)
                {
                    return {.tier = 1, .matches = std::move(matches)};
                }
                merge(std::move(matches));
            }
        }

        {
            const std::string suffix = std::string{"::"} + std::string{query};
            auto              matches = materialize_cached(by_name);
            std::erase_if(
                matches,
                [&](const SymbolRow& row)
                { return !row.qualified_name.ends_with(suffix); });
            if (!matches.empty())
            {
                if (tier == 0)
                {
                    tier = 2;
                }
                if (!merge_lower_tiers)
                {
                    return {.tier = tier, .matches = std::move(matches)};
                }
                merge(std::move(matches));
            }
        }

        {
            const std::string pattern = std::format("%{}%", last);
            auto              matches =
                materialize_cached(ids_where_name_like(store, pattern));
            std::erase_if(
                matches,
                [&](const SymbolRow& row)
                {
                    return !cg::util::ascii_icontains(row.name, query)
                           && !cg::util::ascii_icontains(
                               row.qualified_name,
                               query);
                });
            if (!matches.empty())
            {
                if (tier == 0)
                {
                    tier = 3;
                }
                merge(std::move(matches));
            }
        }

        return {.tier = tier, .matches = std::move(merged)};
    }

    export auto get_symbol(GraphStore& store, SymId id)
        -> std::optional<SymbolRow>
    {
        return store.symbol_row(id);
    }

    /**
     * @brief 批量轻量投影行：初筛路径要的七个字段，不做限定名。
     *
     * @internal
     * 与完整 SymbolRow 的差别：不查父行、不算限定名（环截断、最外层命名
     * 空间等保准确率语义只在 symbol_row 一侧），ns/sig_hash/flags/参数表
     * 也不物化。需要限定名的少数符号继续走 get_symbol/symbol_row 原路径
     * （原函数一行不动）。行序与输入 id 序一致（缺失 id 跳过），调用方对
     * 顺序的既有依赖（同名集的收集序）不受影响。
     */
    export struct SymbolBrief
    {
        SymId       id = 0;
        SymbolKind  kind = SymbolKind::Record;
        std::string name;
        std::string unique_key;
        std::string file; // 规范化路径文本（files 行缺失时为空，同 symbol_row）
        int         line = 0;
        int         col = 0;
    };

    /**
     * @brief 一批符号 id 的轻量投影：一条连接查询拉七字段（分块 IN）。
     *
     * @internal
     * 分块上限防 SQLite 变量数上限（与其他批量查询同一条红线）。str 名
     * 字/唯一键经
     * 外键恒在，用内连接；files 行用左连接——symbol_row 对缺失 files 行的
     * 宽容（file 留空、符号照常返回）在此逐字复刻。同一 str 表在一批查询
     * 里连接三次，表与结果列都显式别名（标签组在 db.tables.h 的 tab 命名
     * 空间）。
     */
    export auto symbol_briefs(GraphStore& store, std::span<const SymId> ids)
        -> std::vector<SymbolBrief>
    {
        constexpr std::size_t BRIEF_CHUNK = 400;
        const tab::Symbols    s{};
        const tab::Str        str{};
        const tab::Files      f{};
        const auto            name_table = str.as(tab::brief_str_name);
        const auto            key_table = str.as(tab::brief_str_key);
        const auto            path_table = str.as(tab::brief_str_path);
        std::unordered_map<SymId, SymbolBrief> found;
        found.reserve(ids.size());
        for (std::size_t base = 0; base < ids.size(); base += BRIEF_CHUNK)
        {
            const std::size_t count = std::min(BRIEF_CHUNK, ids.size() - base);
            const std::vector<std::int64_t> chunk(
                ids.begin() + static_cast<std::ptrdiff_t>(base),
                ids.begin() + static_cast<std::ptrdiff_t>(base + count));
            auto rows =
                store.conn()(sqlpp::select(
                                 s.symId,
                                 s.kindId,
                                 s.line,
                                 s.col,
                                 name_table.s.as(tab::brief_name),
                                 key_table.s.as(tab::brief_key),
                                 path_table.s.as(tab::brief_path))
                                 .from(s.inner_join(name_table)
                                           .on(s.nameId == name_table.id)
                                           .inner_join(key_table)
                                           .on(s.uniqueKeyId == key_table.id)
                                           .left_outer_join(f)
                                           .on(s.fileId == f.fileId)
                                           .left_outer_join(path_table)
                                           .on(f.pathId == path_table.id))
                                 .where(s.symId.in(chunk)));
            for (const auto& row : rows)
            {
                SymbolBrief b;
                b.id = static_cast<SymId>(row.symId);
                b.kind = symbol_kind_of(static_cast<std::int64_t>(row.kindId));
                b.name = std::string{row.brief_name};
                b.unique_key = std::string{row.brief_key};
                if (row.brief_path.has_value())
                {
                    b.file = std::string{*row.brief_path};
                }
                b.line = static_cast<int>(row.line);
                b.col = static_cast<int>(row.col);
                found.emplace(b.id, std::move(b));
            }
        }
        std::vector<SymbolBrief> out;
        out.reserve(ids.size());
        for (const SymId id : ids)
        {
            if (const auto it = found.find(id); it != found.end())
            {
                out.push_back(std::move(it->second));
            }
        }
        return out;
    }

    /**
     * @brief CLI 模糊层（第 4 层"你是不是想找"建议）的候选池：非限定名以
     *        大小写不敏感方式包含 `needle` 的每个符号。
     *
     * @internal SQLite LIKE 对 ASCII 大小写不敏感，因此收窄发生在 SQL 里。
     * 按相似度排序是调用方的事；str/symbols 连接放在这里，因为上面的共享
     * 辅助函数是模块内部的。
     */
    export auto symbols_name_like(
        GraphStore&         store,
        std::string_view    needle,
        const SymbolFilter& filter) -> std::vector<SymbolRow>
    {
        const std::string pattern = std::format("%{}%", needle);
        return materialize(store, ids_where_name_like(store, pattern), filter);
    }

    /**
     * @brief 出边（callee）按 (dst, type) 聚合；`types` 为空表示全部类型。
     *
     * @internal 预编译语句（GraphStore::edges_from_agg）：类型过滤在
     * 调用方做——(对端, 类型) 分组各自独立聚合，丢组与 SQL 层
     * WHERE type IN (...) 逐位等价；保序过滤不改变组序。
     */
    export auto edges_from(
        GraphStore&               store,
        SymId                     src,
        std::span<const EdgeType> types) -> std::vector<EdgeAggRow>
    {
        std::vector<EdgeAggRow> out = store.edges_from_agg(src);
        if (!types.empty())
        {
            std::erase_if(
                out,
                [&types](const EdgeAggRow& row)
                { return std::ranges::find(types, row.type) == types.end(); });
        }
        return out;
    }

    /**
     * @brief 入边（caller）按 (src, type) 聚合；`types` 为空表示全部类型。
     *
     * @internal 预编译语句（GraphStore::edges_to_agg）；类型过滤方式
     * 与行序约定同 edges_from。
     */
    export auto edges_to(
        GraphStore&               store,
        SymId                     dst,
        std::span<const EdgeType> types) -> std::vector<EdgeAggRow>
    {
        std::vector<EdgeAggRow> out = store.edges_to_agg(dst);
        if (!types.empty())
        {
            std::erase_if(
                out,
                [&types](const EdgeAggRow& row)
                { return std::ranges::find(types, row.type) == types.end(); });
        }
        return out;
    }

    /**
     * @brief 单一类型的原始边行，出边（src == id）或入边（dst == id）；
     *        用于需要展示 attrs/confidence/site 的场合（上下文渲染）。
     *
     * @internal 预编译语句（GraphStore::edges_of_raw）。
     */
    export auto
    edges_of(GraphStore& store, SymId id, bool outgoing, EdgeType type)
        -> std::vector<EdgeRow>
    {
        return store.edges_of_raw(id, outgoing, type);
    }

    /**
     * @brief 批量聚合行：self 是查询侧符号（出边查询的 src、入边
     *        查询的 dst）。
     */
    export struct EdgeAggBySym
    {
        SymId      self = 0;
        EdgeAggRow row;
    };

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

    /**
     * @brief 某一边类型的全部原始边行（按站点/置信度读取）。
     *
     * @internal 查询侧 dispatch 证据索引只关心 type_id = 7；部分索引
     * `edges_dispatch` 让这次全扫描只触及 dispatch 边（参考语料规模约一万行），
     * 绝不扫 26 万条普通边。
     */
    export auto edges_of_type(GraphStore& store, EdgeType type)
        -> std::vector<EdgeRow>
    {
        const tab::Edges     e{};
        std::vector<EdgeRow> out;
        auto rows = store.conn()(sqlpp::select(sqlpp::all_of(e))
                                     .from(e)
                                     .where(e.typeId == to_id(type)));
        for (const auto& row : rows)
        {
            out.push_back(to_edge_row(row, type));
        }
        return out;
    }

    /** @brief 某一边类型的当前行数（部分索引服务 dispatch 边的整库计数）。 */
    export auto count_edges_of_type(GraphStore& store, EdgeType type)
        -> std::int64_t
    {
        const tab::Edges e{};
        auto             rows =
            store.conn()(sqlpp::select(sqlpp::count(e.edgeId).as(tab::cnt))
                             .from(e)
                             .where(e.typeId == to_id(type)));
        std::int64_t cnt = 0;
        for (const auto& row : rows)
        {
            cnt = row.cnt; // 跑到 DONE：中途 return 会钉住隐式读快照
        }
        return cnt;
    }

    /** @brief 记录的直接成员（parent_id == record）。 */
    export auto members(GraphStore& store, SymId record)
        -> std::vector<SymbolRow>
    {
        const tab::Symbols s{};
        std::vector<SymId> ids;
        auto               rows = store.conn()(
            sqlpp::select(s.symId).from(s).where(s.parentId == record));
        for (const auto& row : rows)
        {
            ids.push_back(static_cast<SymId>(row.symId));
        }
        return materialize(store, ids, SymbolFilter{});
    }

    /**
     * @brief 一对符号之间某类型的全部原始边行，按站点行升序（调用点展示
     *        的取数面：callers/callees 条目用首行拿 (via, entry) 边的首个
     *        调用点坐标）。
     *
     * @internal 空结果 = 该对在该类型上无原始边行（家族合并或派发改写的
     * 条目可能查不到），调用方回退对端符号的定义行——诚实降级，不猜站点。
     */
    export auto
    edges_between(GraphStore& store, SymId src, SymId dst, EdgeType type)
        -> std::vector<EdgeRow>
    {
        const tab::Edges     e{};
        std::vector<EdgeRow> out;
        auto rows = store.conn()(sqlpp::select(sqlpp::all_of(e))
                                     .from(e)
                                     .where(
                                         e.srcId == src and e.dstId == dst
                                         and e.typeId == to_id(type))
                                     .order_by(e.siteLine.asc()));
        for (const auto& row : rows)
        {
            out.push_back(to_edge_row(row, type));
        }
        return out;
    }

    /**
     * @brief 取址证据行（消费簇 1 契约）：`&method` / `&free_function` 的
     *        出现站点，按 unique_key 查询。
     *
     * 契约（簇 1 生产、本查询消费；字段口径在集成阶段对齐）：`&Class::m`
     * 或 `&f` 取址在 type_hints 落一行，source = 6（HintSource::
     * AddressOfObject 的整数值；db 层不导入 cg.analysis，按整数比对）、
     * related_key = 被取址符号的 unique_key 文本，行自带站点坐标
     * (file_id, line)；owner_id 是取址发生处的函数（消费方仅作展示）。
     *
     * @internal
     * soundness 边界：证据行只证明"取址出现过"，绝不推出调用关系——经
     * 函数/成员指针的间接调用对 callers/impact 天然不可见，这正是标注要
     * 说的话，不是要补的边；related_key 按 unique_key 精确文本匹配，不做
     * 家族展开——&Base::m 的证据只挂在 Base::m 名下，派生 override 查不
     * 到（宁可少提示也不误归属；调用方自带 family 键集并集查询）。
     * fallback 清单：无 source=6 且 related_key 命中的行（簇 1 未落地或
     * 该符号从未被取址）→ 空，调用方不输出标注；既有 &obj 对象取址行
     * （related_key 为空）天然不命中。
     */
    export auto address_taken_hints(
        GraphStore&                       store,
        std::span<const std::string_view> unique_keys)
        -> std::vector<TypeHintRow>
    {
        std::vector<TypeHintRow> out;
        const tab::TypeHints     th{};
        // source 的 6 = HintSource::AddressOfObject（analysis.model.ixx）。
        constexpr int ADDRESS_OF_SOURCE = 6;
        for (const std::string_view key : unique_keys)
        {
            if (key.empty())
            {
                continue;
            }
            auto rows = store.conn()(sqlpp::select(sqlpp::all_of(th))
                                         .from(th)
                                         .where(
                                             th.source == ADDRESS_OF_SOURCE
                                             and th.relatedKey == key));
            for (const auto& row : rows)
            {
                TypeHintRow r = to_type_hint_row(row);
                r.id = static_cast<std::int64_t>(row.hintId);
                out.push_back(std::move(r));
            }
        }
        std::ranges::sort(
            out,
            [](const TypeHintRow& a, const TypeHintRow& b)
            {
                return a.file_id != b.file_id ? a.file_id < b.file_id
                                              : a.line < b.line;
            });
        return out;
    }

    /**
     * @brief 定义在 `file` 中的每个符号（ccg changes 把每个变更文件映射到
     *        这些符号——文件粒度、保守正确：该文件的全部符号集作上游遍历
     *        的种子；已删除的文件保有自己的行直到下一次索引运行，因此其
     *        原有符号仍可被找到）。
     */
    export auto symbols_of_file(GraphStore& store, FileId file)
        -> std::vector<SymbolRow>
    {
        const tab::Symbols s{};
        std::vector<SymId> ids;
        auto               rows = store.conn()(
            sqlpp::select(s.symId).from(s).where(s.fileId == file));
        for (const auto& row : rows)
        {
            ids.push_back(static_cast<SymId>(row.symId));
        }
        return materialize(store, ids, SymbolFilter{});
    }

    /** @brief 起自 `sym` 的 extends 边（它的基类）。 */
    export auto bases(GraphStore& store, SymId sym) -> std::vector<EdgeRow>
    {
        return edges_of(store, sym, true, EdgeType::Extends);
    }

    /** @brief 终于 `sym` 的 extends 边（从它派生的类型）。 */
    export auto deriving(GraphStore& store, SymId sym) -> std::vector<EdgeRow>
    {
        return edges_of(store, sym, false, EdgeType::Extends);
    }

    /** @brief 成员行带上来源标注：own 或继承自哪个基类。 */
    export struct MemberRow
    {
        SymbolRow   symbol;
        std::string origin; // "own" 或基类的 qualified_name
    };

    /**
     * @brief 成员面（含继承链）：own 成员 + 各层基类的成员，各标 origin。
     *
     * @internal 沿 extends 边逐层上溯（BFS，访问集防环，深度上限 8 防
     * 深链病态库）；同一符号经多条路径可达时记首见（菱形继承取最近层）。
     * 覆写成员在 own 与基类层各计一次（同名不同符号——如实展示，
     * 消费端按 qualified_name 自行对齐）。
     */
    export auto members_with_origin(GraphStore& store, SymId record)
        -> std::vector<MemberRow>
    {
        constexpr int                      kMaxBasesDepth = 8;
        std::vector<MemberRow>             out;
        std::vector<SymId>                 visited_bases{record};
        std::vector<std::pair<SymId, int>> pending{{record, 0}};
        while (!pending.empty())
        {
            const auto [id, depth] = pending.front();
            pending.erase(pending.begin());
            const auto rows = members(store, id);
            const bool own = id == record;
            const auto base = get_symbol(store, id);
            for (const SymbolRow& m : rows)
            {
                out.push_back(
                    {m,
                     own ? "own"
                         : (base ? base->qualified_name : "<anonymous base>")});
            }
            if (depth >= kMaxBasesDepth)
            {
                continue;
            }
            for (const EdgeRow& e : bases(store, id))
            {
                if (std::ranges::find(visited_bases, e.dst)
                    != visited_bases.end())
                {
                    continue;
                }
                visited_bases.push_back(e.dst);
                pending.emplace_back(e.dst, depth + 1);
            }
        }
        return out;
    }

    /**
     * @brief files 表是否有任一行（EXISTS 语义的存在性探针）。
     *
     * @internal 供"库是否为空"的快速判定（引擎身份比对前的库内容探测）：
     * stats() 要对四张表分组计数，每次编辑都会走到的路径只需这一比特。
     * LIMIT 1 的单列探针在首行命中即返回，代价与 files 行数无关。
     */
    export auto has_files(GraphStore& store) -> bool
    {
        const tab::Files f{};
        // sqlpp 结果是惰性流（begin/end 非常量），不能以 const 接收。
        auto rows = store.conn()(sqlpp::select(f.fileId).from(f).limit(1u));
        return rows.begin() != rows.end();
    }

    export auto stats(GraphStore& store) -> Stats
    {
        Stats               out;
        const tab::Files    f{};
        const tab::Symbols  s{};
        const tab::Edges    e{};
        const tab::Includes inc{};
        const tab::Str      str{};

        {
            auto rows = store.conn()(sqlpp::select(
                                         f.parseStatus,
                                         sqlpp::count(f.fileId).as(tab::cnt))
                                         .from(f)
                                         .group_by(f.parseStatus));
            for (const auto& row : rows)
            {
                const std::int64_t n = row.cnt;
                out.files += n;
                switch (static_cast<cg::util::ParseStatus>(
                    static_cast<int>(row.parseStatus)))
                {
                case cg::util::ParseStatus::Ok:
                    out.files_ok += n;
                    break;
                case cg::util::ParseStatus::Partial:
                    out.files_partial += n;
                    break;
                case cg::util::ParseStatus::Failed:
                    out.files_failed += n;
                    break;
                case cg::util::ParseStatus::Pending:
                    out.files_pending += n;
                    break;
                }
            }
        }
        {
            auto rows = store.conn()(
                sqlpp::select(s.kindId, sqlpp::count(s.symId).as(tab::cnt))
                    .from(s)
                    .group_by(s.kindId));
            for (const auto& row : rows)
            {
                const std::int64_t n = row.cnt;
                out.symbols += n;
                const auto kind = static_cast<std::int64_t>(row.kindId);
                if (kind >= 0 && kind < kind_count)
                {
                    out.symbols_by_kind[kind] += n;
                }
            }
        }
        {
            auto rows = store.conn()(
                sqlpp::select(e.typeId, sqlpp::count(e.edgeId).as(tab::cnt))
                    .from(e)
                    .group_by(e.typeId));
            for (const auto& row : rows)
            {
                const std::int64_t n = row.cnt;
                out.edges += n;
                const auto type = static_cast<std::int64_t>(row.typeId);
                if (type >= 0 && type < edge_type_count)
                {
                    out.edges_by_type[type] += n;
                }
            }
        }
        {
            auto rows = store.conn()(
                sqlpp::select(sqlpp::count(inc.fileId).as(tab::cnt)).from(inc));
            for (const auto& row : rows)
            {
                out.includes = row.cnt;
            }
            auto angled = store.conn()(
                sqlpp::select(sqlpp::count(inc.fileId).as(tab::cnt))
                    .from(inc)
                    .where(inc.angled == 1));
            for (const auto& row : angled)
            {
                out.includes_angled = row.cnt;
            }
        }
        {
            const tab::Imports imp{};
            auto               rows = store.conn()(
                sqlpp::select(sqlpp::count(imp.fileId).as(tab::cnt)).from(imp));
            for (const auto& row : rows)
            {
                out.imports = row.cnt;
            }
        }
        {
            auto rows = store.conn()(
                sqlpp::select(sqlpp::count(str.id).as(tab::cnt)).from(str));
            for (const auto& row : rows)
            {
                out.strings = row.cnt;
            }
        }
        {
            const tab::TypeHints th{};
            for (const auto& row : store.conn()(
                     sqlpp::select(sqlpp::count(th.hintId).as(tab::cnt))
                         .from(th)))
            {
                out.type_hints = row.cnt;
            }
            const tab::CallSites cs{};
            for (const auto& row : store.conn()(
                     sqlpp::select(sqlpp::count(cs.siteId).as(tab::cnt))
                         .from(cs)))
            {
                out.call_sites = row.cnt;
            }
        }
        // 下面两个 meta 是机器来源（写入侧恒为 std::to_string 纯数字），
        // 只可能被外部 sqlite 篡改或版本残留弄脏——解析用邻居同款宽容
        // 语义（atoll，畸形归 0）：stats 是全库摘要命令，绝不因单个 meta
        // 畸形而整单失败（此前 stoll 会让整条 ccg stats 以
        // "invalid stoll argument" 退出）。
        if (const auto v = store.meta("analysis_rounds"))
        {
            out.analysis_rounds = std::atoll(v->c_str());
        }
        if (const auto v = store.meta("analysis_work_items"))
        {
            out.analysis_work_items = std::atoll(v->c_str());
        }
        if (const auto v = store.meta("analysis_budget_exhausted"))
        {
            out.analysis_budget_exhausted = *v == "1";
        }
        if (const auto v = store.meta("app_version"))
        {
            out.app_version = *v;
        }
        if (const auto t = store.meta("last_index_at"))
        {
            out.last_index_at = t;
        }
        return out;
    }

    export auto symbol_count(GraphStore& store) -> std::int64_t
    {
        const tab::Symbols s{};
        auto               rows = store.conn()(
            sqlpp::select(sqlpp::count(s.symId).as(tab::cnt)).from(s));
        std::int64_t cnt = 0;
        for (const auto& row : rows)
        {
            cnt = row.cnt; // 跑到 DONE：中途 return 会钉住隐式读快照
        }
        return cnt;
    }

    export auto edge_count(GraphStore& store) -> std::int64_t
    {
        const tab::Edges e{};
        auto             rows = store.conn()(
            sqlpp::select(sqlpp::count(e.edgeId).as(tab::cnt)).from(e));
        std::int64_t cnt = 0;
        for (const auto& row : rows)
        {
            cnt = row.cnt; // 跑到 DONE：中途 return 会钉住隐式读快照
        }
        return cnt;
    }

    /**
     * @brief 每条 files 行，路径文本已解析（cg.graph.build 的删除源码差分
     *        拿它与已加载的编译数据库对照）。
     */
    export auto all_files(GraphStore& store) -> std::vector<FileRow>
    {
        const tab::Files     f{};
        std::vector<FileRow> out;
        auto rows = store.conn()(sqlpp::select(sqlpp::all_of(f)).from(f));
        for (const auto& row : rows)
        {
            FileRow fr;
            fr.id = static_cast<FileId>(row.fileId);
            fr.path = store.str_of(static_cast<StrId>(row.pathId));
            if (row.contentHash.has_value())
            {
                fr.content_hash = std::string{*row.contentHash};
            }
            fr.status = static_cast<cg::util::ParseStatus>(
                static_cast<int>(row.parseStatus));
            fr.error_count = static_cast<int>(row.errorCount);
            if (row.lastError.has_value())
            {
                fr.last_error = std::string{*row.lastError};
            }
            if (row.indexedAt.has_value())
            {
                fr.indexed_at = std::string{*row.indexedAt};
            }
            out.push_back(std::move(fr));
        }
        return out;
    }

    /**
     * @brief 在场 files 行的 id 全集（单条查询，仅主键列）。
     *
     * @internal 提交段开头的陈旧快照核对用：索引/更新的解析期在事务外拿过
     *           一份 file_id 快照（row_by_path 等），并发写者可能在期间删
     *           除过行；提交事务内以此集合作差，死 id 的引用改为复活/丢弃，
     *           绝不让旧 id 直接流进外键列（受控实验已证实该穿插会以
     *           FOREIGN KEY 整批回滚收场）。只选 id 列，不做路径文本解析。
     */
    export auto all_file_ids(GraphStore& store) -> std::unordered_set<FileId>
    {
        const tab::Files           f{};
        std::unordered_set<FileId> out;
        auto rows = store.conn()(sqlpp::select(f.fileId).from(f));
        for (const auto& row : rows)
        {
            out.insert(static_cast<FileId>(row.fileId));
        }
        return out;
    }

    /**
     * @brief 全部文件的路径映射（file_id -> path），单条查询两列物化。
     *
     * @internal 站点装载路径的文件名解析用：调用点按行内 file_id 直取路径
     *           （整表一次），替代逐行单键查询（行内无 file_id 反查场景时
     *           无单行查询必要；files 表在分析期只读，映射与逐行查询结果
     *           一致）。
     */
    export auto file_path_map(GraphStore& store)
        -> std::unordered_map<FileId, std::string>
    {
        const tab::Files                        f{};
        std::unordered_map<FileId, std::string> out;
        auto rows = store.conn()(sqlpp::select(f.fileId, f.pathId).from(f));
        for (const auto& row : rows)
        {
            out.emplace(
                static_cast<FileId>(row.fileId),
                store.str_of(static_cast<StrId>(row.pathId)));
        }
        return out;
    }

    /**
     * @brief 供内存解析索引使用的纤薄符号投影（cg.graph.build）：不做限定名
     *        遍历，整表一条查询。
     */
    export struct IndexRow
    {
        SymId                       id = 0;
        SymbolKind                  kind = SymbolKind::Record;
        std::string                 name;
        std::string                 ns;
        std::optional<SymId>        parent_id;
        std::string                 parent; // 父符号的非限定名，无父时为 ''
        std::optional<std::int64_t> sig_hash;
        std::uint32_t               flags = 0;
        FileId                      file_id = 0;
        std::string                 unique_key;
    };

    /**
     * @brief 整表读出 IndexRow。
     *
     * @internal 父名从刚加载的行解析（单次额外遍历）。
     */
    export auto all_symbol_index_rows(GraphStore& store)
        -> std::vector<IndexRow>
    {
        const tab::Symbols    s{};
        std::vector<IndexRow> out;
        auto                  rows = store.conn()(sqlpp::select(
                                                      s.symId,
                                                      s.kindId,
                                                      s.nameId,
                                                      s.nsId,
                                                      s.parentId,
                                                      s.sigHash,
                                                      s.flags,
                                                      s.fileId,
                                                      s.uniqueKeyId)
                                                      .from(s));
        for (const auto& row : rows)
        {
            IndexRow ir;
            ir.id = static_cast<SymId>(row.symId);
            ir.kind = symbol_kind_of(static_cast<std::int64_t>(row.kindId));
            ir.name = store.str_of(static_cast<StrId>(row.nameId));
            ir.ns = store.str_of(static_cast<StrId>(row.nsId));
            ir.unique_key = store.str_of(static_cast<StrId>(row.uniqueKeyId));
            if (row.parentId.has_value())
            {
                ir.parent_id = static_cast<SymId>(*row.parentId);
            }
            if (row.sigHash.has_value())
            {
                ir.sig_hash = static_cast<std::int64_t>(*row.sigHash);
            }
            ir.flags = static_cast<std::uint32_t>(
                static_cast<std::int64_t>(row.flags));
            ir.file_id = static_cast<FileId>(row.fileId);
            out.push_back(std::move(ir));
        }
        std::unordered_map<SymId, std::string*> names;
        names.reserve(out.size() * 2);
        for (IndexRow& ir : out)
        {
            names.emplace(ir.id, &ir.name);
        }
        for (IndexRow& ir : out)
        {
            if (ir.parent_id.has_value())
            {
                if (const auto it = names.find(*ir.parent_id);
                    it != names.end())
                {
                    ir.parent = *it->second;
                }
            }
        }
        return out;
    }

    /**
     * @brief 字符串已解析的完整符号行，供增量等价性门整表读取（一条
     *        select；调用方自行构建投影——下方 id 稳定的 dump 嵌入的是
     *        存储顺序的 id）。
     */
    export struct FullSymbolRow
    {
        SymId                       id = 0;
        SymbolKind                  kind = SymbolKind::Record;
        std::string                 name;
        std::string                 ns;
        std::string                 unique_key;
        std::optional<std::int64_t> sig_hash;
        FileId                      file_id = 0;
        int                         line = 0;
        int                         col = 0;
        std::optional<SymId>        parent_id;
        std::uint32_t               flags = 0;
        std::optional<std::string>  param_types;
        std::optional<std::string>  template_params;
    };

    export auto all_symbol_rows(GraphStore& store) -> std::vector<FullSymbolRow>
    {
        const tab::Symbols         s{};
        std::vector<FullSymbolRow> out;
        auto rows = store.conn()(sqlpp::select(sqlpp::all_of(s)).from(s));
        for (const auto& row : rows)
        {
            FullSymbolRow fr;
            fr.id = static_cast<SymId>(row.symId);
            fr.kind = symbol_kind_of(static_cast<std::int64_t>(row.kindId));
            fr.name = store.str_of(static_cast<StrId>(row.nameId));
            fr.ns = store.str_of(static_cast<StrId>(row.nsId));
            fr.unique_key = store.str_of(static_cast<StrId>(row.uniqueKeyId));
            if (row.sigHash.has_value())
            {
                fr.sig_hash = static_cast<std::int64_t>(*row.sigHash);
            }
            fr.file_id = static_cast<FileId>(row.fileId);
            fr.line = static_cast<int>(row.line);
            fr.col = static_cast<int>(row.col);
            if (row.parentId.has_value())
            {
                fr.parent_id = static_cast<SymId>(*row.parentId);
            }
            fr.flags = static_cast<std::uint32_t>(
                static_cast<std::int64_t>(row.flags));
            if (row.paramTypes.has_value())
            {
                fr.param_types = std::string{*row.paramTypes};
            }
            if (row.templateParams.has_value())
            {
                fr.template_params = std::string{*row.templateParams};
            }
            out.push_back(std::move(fr));
        }
        return out;
    }

    /**
     * @brief 一条 select 读出每条边行（遍历查询保持逐符号；这是门禁与
     *        审计的批量读取）。
     */
    export auto all_edges(GraphStore& store) -> std::vector<EdgeRow>
    {
        const tab::Edges     e{};
        std::vector<EdgeRow> out;
        auto rows = store.conn()(sqlpp::select(sqlpp::all_of(e)).from(e));
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
     * @brief 重复行探测组（完整性断言；测试与未来的 doctor）：每个函数返回
     *        对 schema 声明为 UNIQUE 的键持有超过一行的分组数。
     *
     * @internal upsert 保证这一点；保持在 SQL 里的直接 SQL 检查让证明保持
     * 诚实。
     */
    export auto duplicate_symbol_keys(GraphStore& store) -> std::int64_t
    {
        const tab::Symbols s{};
        auto rows = store.conn()(sqlpp::select(s.uniqueKeyId)
                                     .from(s)
                                     .group_by(s.uniqueKeyId)
                                     .having(sqlpp::count(s.symId) > 1));
        return std::ranges::distance(rows);
    }

    export auto duplicate_edge_sites(GraphStore& store) -> std::int64_t
    {
        const tab::Edges e{};
        auto             rows = store.conn()(
            sqlpp::select(e.srcId, e.dstId, e.typeId, e.siteFileId, e.siteLine)
                .from(e)
                .group_by(e.srcId, e.dstId, e.typeId, e.siteFileId, e.siteLine)
                .having(sqlpp::count(e.edgeId) > 1));
        return std::ranges::distance(rows);
    }

    export auto duplicate_includes(GraphStore& store) -> std::int64_t
    {
        const tab::Includes inc{};
        auto                rows =
            store.conn()(sqlpp::select(inc.fileId, inc.targetId, inc.line)
                             .from(inc)
                             .group_by(inc.fileId, inc.targetId, inc.line)
                             .having(sqlpp::count(inc.fileId) > 1));
        return std::ranges::distance(rows);
    }

    /**
     * @brief 一条 include 图边：包含方文件 -> 指令中书写的目标文本。
     *
     * @internal 目标解析成文件 id 是调用方的事（规则取决于包含方目录与
     * include 搜索路径；`angled` 告诉严格解析器是否搜索当前目录）。
     */
    export struct IncludeLink
    {
        FileId      includer = 0;
        std::string target;
        bool        angled = false;
    };

    export auto all_includes(GraphStore& store) -> std::vector<IncludeLink>
    {
        const tab::Includes      inc{};
        std::vector<IncludeLink> out;
        auto                     rows = store.conn()(
            sqlpp::select(inc.fileId, inc.targetId, inc.angled).from(inc));
        for (const auto& row : rows)
        {
            out.push_back(
                {.includer = static_cast<FileId>(row.fileId),
                 .target = store.str_of(static_cast<StrId>(row.targetId)),
                 .angled = row.angled != 0});
        }
        return out;
    }

    /**
     * @brief 每条 imports 行，模块文本已解析（模块图差分与增量等价面）。
     */
    export auto all_imports(GraphStore& store) -> std::vector<ImportLink>
    {
        const tab::Imports      imp{};
        std::vector<ImportLink> out;
        auto                    rows = store.conn()(
            sqlpp::select(imp.fileId, imp.moduleId, imp.line).from(imp));
        for (const auto& row : rows)
        {
            out.push_back(
                {.file = static_cast<FileId>(row.fileId),
                 .module_name = store.str_of(static_cast<StrId>(row.moduleId)),
                 .line = static_cast<int>(row.line)});
        }
        return out;
    }

    /**
     * @brief 每条新鲜度指纹，一条查询读全（增量过期检查拿它们与整张
     *        files 表的 stat() 结果对照）。
     */
    export auto all_fingerprints(GraphStore& store)
        -> std::vector<FingerprintRow>
    {
        const tab::Fingerprints     fp{};
        std::vector<FingerprintRow> out;
        auto rows = store.conn()(sqlpp::select(sqlpp::all_of(fp)).from(fp));
        for (const auto& row : rows)
        {
            out.push_back(
                {.file_id = static_cast<FileId>(row.fileId),
                 .mtime_ns = static_cast<std::int64_t>(row.mtimeNs),
                 .size = static_cast<std::int64_t>(row.sizeBytes),
                 .cmd_digest = std::string{row.cmdDigest}});
        }
        return out;
    }

    /**
     * @brief 全部失败条目（schema v7），供一次索引运行在 pass 1 之前快照。
     *
     * @internal 行按 (file_id, cmd_digest) 键控：同一文件的多条编译条目
     * 各自独立判定，一条失败不污染另一条成功的条目。
     */
    export auto all_failures(GraphStore& store) -> std::vector<FailureRow>
    {
        const tab::Failures     fl{};
        std::vector<FailureRow> out;
        auto rows = store.conn()(sqlpp::select(sqlpp::all_of(fl)).from(fl));
        for (const auto& row : rows)
        {
            FailureRow f;
            f.file_id = static_cast<FileId>(row.fileId);
            f.cmd_digest = std::string{row.cmdDigest};
            f.content_hash = std::string{row.contentHash};
            f.mtime_ns = static_cast<std::int64_t>(row.mtimeNs);
            f.size = static_cast<std::int64_t>(row.sizeBytes);
            f.engine_token = std::string{row.engineToken};
            f.feed_digest = std::string{row.feedDigest};
            f.roots_digest = std::string{row.rootsDigest};
            f.failure_class = std::string{row.failureClass};
            f.failure_text = std::string{row.failureText};
            f.deps_json = std::string{row.depsJson};
            f.attempts = static_cast<std::int64_t>(row.attempts);
            f.first_failed_at = std::string{row.firstFailedAt};
            f.last_failed_at = std::string{row.lastFailedAt};
            f.retry_after = static_cast<std::int64_t>(row.retryAfter);
            out.push_back(std::move(f));
        }
        return out;
    }

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

    /** @brief unique_key 恰为 `key` 的符号 id（无则 nullopt）。 */
    export auto symbol_id_by_key(GraphStore& store, std::string_view key)
        -> std::optional<SymId>
    {
        return id_where_unique_key_is(store, key);
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

    /** @brief 非限定名恰为 `name` 的全部符号 id（PreToolUse 注入的同名集）。 */
    export auto symbol_ids_by_name(GraphStore& store, std::string_view name)
        -> std::vector<SymId>
    {
        return ids_where_name_is(store, name);
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
