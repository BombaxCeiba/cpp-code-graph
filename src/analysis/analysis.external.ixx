/**
 * @module cg.analysis.external
 *
 * @brief 外部边界扫描与接口级 RTA（root set 线）：识别
 *        director / dlopen 边界上的接口，计算 R(I) = extends 后代闭包
 *        ∩ 全库实例化类型集，落 type_hints 的 source=External 保留槽行
 *        （var_name="`#external`"）。
 *
 * @internal 语义分级（消费契约）：context 展示标注（cli.render）；
 *           dispatch 候选排序 ∩R(I) 优先 + fallback_reason=
 *           external_rta_narrowed，hit==1 只展示唯一候选不发边；
 *           站点级置信严禁——'#' 前缀槽名永不匹配接收者变量名
 *           （propagate find_slot 天然防线）+ propagate 读入显式跳过
 *           source=External 行（双护栏）。dlopen/LoadLibrary 与
 *           SwigDirector 前缀、std 容器表同属已批第三类系统/工具链名单。
 *           红线：不读 compile db 边界外的文件（.i 文本扫描
 *           永久移除）；注入内容只含确定性事实——register* 前缀形参
 *           提及等猜测性关联一律不采；每条 dlopen_boundary 行必须可
 *           溯源到具体调用点（related_var 存 API 锚点
 *           "API名@文件:行:列"）。db.tables.h 置于全局模块片段：本单元
 *           调用 cg.db 的查询与 remove 语句，MSVC 要求 sqlpp23 表模型
 *           定义在此可见（完整机理见 db.tables.h）。
 */
module;
#include "../db/db.tables.h"

export module cg.analysis.external;

import std;
import cg.util;
import sqlpp23.core;
import cg.db;
import cg.analysis.model;

export namespace cg::analysis
{

    namespace external_detail
    {

        inline constexpr cg::db::EdgeType EXTENDS_TYPES[] = {
            cg::db::EdgeType::Extends};

        // dlopen API 名单与 is_dlopen_api 已迁至 cg::analysis::model
        // （DLOPEN_API_NAMES / is_dlopen_api，随 dlopen 零激活修复迁移：
        // collect 与 propagate 侧共用同一名单做界外放行与解析短路）。
        using cg::analysis::is_dlopen_api;

        /**
         * @brief SWIG director 生成类前缀（工具链约定，与系统名单同类）。
         *
         * 公开出处：SWIG 生成器为 director 类合成代理时统一冠此前缀，
         * 属生成器的公开命名契约（与 dlopen 名单同类的平台公开事实）。
         */
        inline constexpr std::string_view DIRECTOR_PREFIX = "SwigDirector";

        /** @brief R(I) 后代闭包节点上限（巨型接口封顶：只少不多，漏报方向）。
         */
        inline constexpr std::size_t MAX_CLOSURE_NODES = 1024;

    } // namespace external_detail

    /**
     * @brief 扫外部边界、计算 R(I) 并落 type_hints 的 source=External 行。
     *
     * @param store 当前连接（阶段 B 事务内；extends/overrides 边与阶段 A 的
     *              hints/call_sites 均已可见）。
     * @return 写入的 `#external` 行数（0 = 无边界命中）。
     *
     * @internal 幂等：开头整类删除旧 source=External 行（INSERT 冲突静默
     *           去重不会更新既有行，必须先删后写）。边界优先级
     *           director > dlopen_boundary（一个接口一个词，首个命中为准）。
     *           dlopen 证据 = 同一 caller 函数内"API 名单调用点 + 接口静态
     *           接收类型调用点"配对（collect 对指针转型目标永不采证，加载
     *           函数指针签名的可索引终点就是接口类型化站点；链断在中间
     *           不发行，宁漏勿猜），锚点随行写 related_var。I_all 口径 =
     *           全部非 Declared 行 type_set 的 key 并集（库内出现过动态证据
     *           的类型，含传播值——比 verdicts 模拟的 source 1/3/4 直收口径
     *           略宽，R(I) 偏大 → hit 偏多 → 收窄偏少，保守方向安全）。
     */
    [[nodiscard]]
    inline auto run_external_rta(cg::db::GraphStore& store) -> std::int64_t
    {
        using namespace external_detail;
        // 幂等：抹掉上一轮的 `#external` 行（其 R(I) 已陈旧）。
        {
            const cg::db::tab::TypeHints th{};
            store.conn()(sqlpp::delete_from(th).where(
                th.source == static_cast<int>(HintSource::External)));
        }

        const std::vector<cg::db::IndexRow> rows =
            cg::db::all_symbol_index_rows(store);
        std::unordered_map<std::int64_t, const cg::db::IndexRow*> by_id;
        by_id.reserve(rows.size());
        for (const cg::db::IndexRow& row : rows)
        {
            by_id.emplace(row.id, &row);
        }

        // 接口 = 直接成员含 pure virtual 方法的记录（与 dispatch 的
        // has_pure_member 图内口径一致：只看直接声明，继承的 pure 不追，
        // 不限方法 kind——纯虚析构同样构成接口性）。
        std::unordered_set<std::int64_t> iface_ids;
        for (const cg::db::IndexRow& row : rows)
        {
            if ((row.flags & cg::db::sym_flags::is_pure_virtual) != 0
                && row.parent_id.has_value())
            {
                const auto parent = by_id.find(*row.parent_id);
                if (parent != by_id.end()
                    && parent->second->kind == cg::db::SymbolKind::Record)
                {
                    iface_ids.insert(*row.parent_id);
                }
            }
        }

        // ── 边界扫描：reason 定序 director > dlopen ──
        std::unordered_map<std::int64_t, std::string_view> reason_of;
        const auto claim = [&](std::int64_t     iface,
                               std::string_view reason) -> bool
        { return reason_of.emplace(iface, reason).second; };

        // dlopen 行的证据锚点（iface -> "API名@文件:行:列"，指向加载调用点）。
        std::unordered_map<std::int64_t, std::string>   dlopen_anchors;
        std::unordered_map<cg::db::FileId, std::string> file_name_of;
        for (const cg::db::FileRow& file : cg::db::all_files(store))
        {
            file_name_of.emplace(
                file.id,
                std::string{cg::util::path_basename(file.path)});
        }

        // director：SwigDirector_* 生成类（索引内）extends 的脚本侧基接口。
        for (const cg::db::IndexRow& row : rows)
        {
            if (row.kind != cg::db::SymbolKind::Record
                || !row.name.starts_with(DIRECTOR_PREFIX))
            {
                continue;
            }
            for (const cg::db::EdgeAggRow& e :
                 cg::db::edges_from(store, row.id, EXTENDS_TYPES))
            {
                if (iface_ids.contains(e.other))
                {
                    claim(e.other, EXTERNAL_DIRECTOR);
                }
            }
        }

        // dlopen_boundary：加载链证据配对（同一 caller 函数内）。API 名单
        // 调用点是加载锚点；接口静态接收类型调用点是类型链的确定终点——
        // collect 对指针转型目标永不采证（soundness），故
        // reinterpret_cast 到 IPlugin*(*)() 这类签名的可索引终点就是其后
        // 的接口类型化站点。链断在中间（void* 直转无签名、接口只在别的
        // 函数里使用）不发行（宁漏勿猜）；锚点行（related_var）携带 API
        // 调用点位置，每行可溯源。
        {
            const std::vector<cg::db::CallSiteRow> sites =
                cg::db::all_call_sites(store);
            std::unordered_map<
                std::int64_t,
                std::vector<const cg::db::CallSiteRow*>>
                sites_by_caller;
            sites_by_caller.reserve(sites.size());
            std::vector<const cg::db::CallSiteRow*> api_sites;
            for (const cg::db::CallSiteRow& site : sites)
            {
                sites_by_caller[site.caller_id].push_back(&site);
                if (is_dlopen_api(site.callee_name))
                {
                    api_sites.push_back(&site);
                }
            }
            if (!api_sites.empty())
            {
                std::unordered_map<std::string, std::int64_t> id_of_key;
                id_of_key.reserve(rows.size());
                for (const cg::db::IndexRow& row : rows)
                {
                    id_of_key.emplace(row.unique_key, row.id);
                }
                for (const cg::db::CallSiteRow* anchor : api_sites)
                {
                    const auto fn = sites_by_caller.find(anchor->caller_id);
                    if (fn == sites_by_caller.end())
                    {
                        continue;
                    }
                    for (const cg::db::CallSiteRow* use : fn->second)
                    {
                        const auto hit_iface = [&](std::int64_t id)
                        {
                            if (!iface_ids.contains(id)
                                || !claim(id, EXTERNAL_DLOPEN))
                            {
                                return;
                            }
                            const auto f = file_name_of.find(anchor->file_id);
                            dlopen_anchors.emplace(
                                id,
                                std::format(
                                    "{}@{}:{}:{}",
                                    anchor->callee_name,
                                    f != file_name_of.end() ? f->second
                                                            : std::string{"?"},
                                    anchor->line,
                                    anchor->col));
                        };
                        if (use->static_type_id.has_value())
                        {
                            hit_iface(*use->static_type_id);
                        }
                        for (const std::string& key :
                             parse_string_array(use->receiver_types))
                        {
                            const auto it = id_of_key.find(key);
                            if (it != id_of_key.end())
                            {
                                hit_iface(it->second);
                            }
                        }
                    }
                }
            }
        }

        if (reason_of.empty())
        {
            return 0;
        }

        // I_all = 全部非 Declared 行 type_set 的 key 并集。
        std::unordered_set<std::string> instantiated;
        for (const cg::db::TypeHintRow& row : store.all_type_hints())
        {
            if (row.source == static_cast<int>(HintSource::Declared))
            {
                continue;
            }
            for (const std::string& key : parse_string_array(row.type_set))
            {
                instantiated.insert(key);
            }
        }

        // R(I) = extends 后代闭包（含自身）∩ I_all；封顶只少不多。
        std::int64_t written = 0;
        for (const auto& [iface, reason] : reason_of)
        {
            std::vector<std::string>         reach;
            std::unordered_set<std::int64_t> seen{iface};
            std::vector<std::int64_t>        frontier{iface};
            const auto                       emit = [&](std::int64_t id)
            {
                const auto it = by_id.find(id);
                if (it != by_id.end()
                    && instantiated.contains(it->second->unique_key))
                {
                    reach.push_back(it->second->unique_key);
                }
            };
            emit(iface);
            while (!frontier.empty() && seen.size() <= MAX_CLOSURE_NODES)
            {
                std::vector<std::int64_t> next;
                for (const std::int64_t cur : frontier)
                {
                    for (const cg::db::EdgeAggRow& e :
                         cg::db::edges_to(store, cur, EXTENDS_TYPES))
                    {
                        if (seen.insert(e.other).second)
                        {
                            emit(e.other);
                            next.push_back(e.other);
                        }
                    }
                }
                frontier = std::move(next);
            }
            std::ranges::sort(reach);
            reach.erase(std::unique(reach.begin(), reach.end()), reach.end());

            const auto iface_row = by_id.find(iface);
            const auto sym = cg::db::get_symbol(store, iface);
            if (iface_row == by_id.end() || !sym.has_value())
            {
                continue;
            }
            store.upsert_type_hint(
                {.owner_id = iface,
                 .var_name = external_slot,
                 .file_id = sym->file_id,
                 .line = sym->line,
                 .col = sym->col,
                 .type_set = json_string_array(reach),
                 .source = static_cast<int>(HintSource::External),
                 // 站点级置信严禁：行级 confidence 只作记录，恒 Low(2)。
                 .confidence = static_cast<int>(Confidence::Low),
                 .fallback_reason = std::string{reason},
                 .param_index = -1,
                 // dlopen 行携带加载调用点锚点（可溯源红线）；其余边界为空。
                 .related_var = dlopen_anchors.contains(iface)
                                    ? std::string_view{dlopen_anchors.at(iface)}
                                    : std::string_view{},
                 .related_key = {}});
            ++written;
        }
        return written;
    }

} // namespace cg::analysis
