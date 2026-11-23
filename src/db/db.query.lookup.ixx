module;
#include "db.tables.h"

/**
 * @module cg.db.query.lookup
 *
 * @brief 分层符号查找域：层 1-3
 * 合并查找、单符号读取、符号摘要与名称模糊、单符号边进出聚合（含行物化辅助）。
 */
export module cg.db.query.lookup;

import std;
import cg.util;
import sqlpp23.core;
import sqlpp23.sqlite3;
import cg.db.schema;
import cg.db.store;

export namespace cg::db::query_detail
{

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
            if (auto row = store.symbol_row(id); row && passes(*row, filter))
            {
                out.push_back(std::move(*row));
            }
        }
        return out;
    }

} // namespace cg::db::query_detail

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

    } // namespace

    namespace
    {

        /// 层命中合并器：按符号 id 去重后并入 merged。
        struct TierMerger
        {
            std::vector<SymbolRow>&    merged;
            std::unordered_set<SymId>& seen;

            void take(std::vector<SymbolRow>&& rows)
            {
                for (SymbolRow& row : rows)
                {
                    if (seen.insert(row.id).second)
                    {
                        merged.push_back(std::move(row));
                    }
                }
            }
        };

        /// 批量物化过筛行，同一符号只查一次 symbol_row（行物化每行多趟
        /// SQL：主键行、字符串解析、路径与父链限定名；只读路径重复物化
        /// 产出相同行）。过筛成功的行进 row_of 缓存；被滤掉的行不缓存，
        /// 换过滤口径重查时仍会重新拉取。
        auto materialize_cached_rows(
            GraphStore&                           store,
            const SymbolFilter&                   filter,
            std::unordered_map<SymId, SymbolRow>& row_of,
            std::span<const SymId>                ids) -> std::vector<SymbolRow>
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
                    fresh && query_detail::passes(*fresh, filter))
                {
                    row = &row_of.emplace(id, std::move(*fresh)).first->second;
                }
                if (row != nullptr)
                {
                    out.push_back(*row);
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
        TierMerger                merge{merged, seen};
        int                       tier = 0;

        // 层 1 与层 2 的名字查询同参（ids_where_name_is(store, last)）：
        // 查一次共用，层 1 在其副本上并入 unique key 命中（层 2 口径不含
        // key 命中，保持原集合）。
        const std::vector<SymId> by_name = ids_where_name_is(store, last);

        // 行物化缓存（见 materialize_cached_rows 的口径）：层 3 的 LIKE
        // 池沿池序只补查池中未见过的行。
        std::unordered_map<SymId, SymbolRow> row_of;

        {
            std::vector<SymId> candidates = by_name;
            if (const auto by_key = id_where_unique_key_is(store, query))
            {
                candidates.push_back(*by_key);
            }
            auto matches =
                materialize_cached_rows(store, filter, row_of, candidates);
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
                merge.take(std::move(matches));
            }
        }

        {
            const std::string suffix = std::string{"::"} + std::string{query};
            auto              matches =
                materialize_cached_rows(store, filter, row_of, by_name);
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
                merge.take(std::move(matches));
            }
        }

        {
            const std::string pattern = std::format("%{}%", last);
            auto              matches = materialize_cached_rows(
                store,
                filter,
                row_of,
                ids_where_name_like(store, pattern));
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
                merge.take(std::move(matches));
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
        return query_detail::materialize(
            store,
            ids_where_name_like(store, pattern),
            filter);
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

    /** @brief unique_key 恰为 `key` 的符号 id（无则 nullopt）。 */
    export auto symbol_id_by_key(GraphStore& store, std::string_view key)
        -> std::optional<SymId>
    {
        return id_where_unique_key_is(store, key);
    }

    /** @brief 非限定名恰为 `name` 的全部符号 id（PreToolUse 注入的同名集）。 */
    export auto symbol_ids_by_name(GraphStore& store, std::string_view name)
        -> std::vector<SymId>
    {
        return ids_where_name_is(store, name);
    }

} // namespace cg::db
