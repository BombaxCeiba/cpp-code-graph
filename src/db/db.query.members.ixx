module;
#include "db.tables.h"

/**
 * @module cg.db.query.members
 *
 * @brief
 * 边型与成员域：按类型取边与计数、记录成员、取址证据、文件符号与继承关系、成员来源。
 */
export module cg.db.query.members;

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
     * @brief 某一边类型的全部原始边行（按站点/置信度读取）。
     *
     * @internal `edges_type(type_id)` 普通索引把按类型取行变成索引直达：
     * dispatch 证据整读（type_id=7，参考语料约一万行）与 Extends/
     * Specializes 继承图装载（约六千行）都只触及该类型的边行，绝不扫
     * 全表近 29 万行边。
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

    /** @brief 某一边类型的当前行数（edges_type 索引服务整库计数）。 */
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
        return query_detail::materialize(store, ids, SymbolFilter{});
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
        return query_detail::materialize(store, ids, SymbolFilter{});
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
} // namespace cg::db
