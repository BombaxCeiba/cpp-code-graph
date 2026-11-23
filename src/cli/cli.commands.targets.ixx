module;
#include "../db/db.tables.h"
#include "../msbuild/capture.tables.h"

/**
 * @module cg.cli.commands.targets
 *
 * @brief 目标解析域：相似名建议、唯一目标判定、文件清单去重与输入路径解析。
 */
export module cg.cli.commands.targets;

import std;
import cg.util;
import cg.db;
import cg.graph;
import cg.msbuild;
import cg.cli.parse;
import cg.cli.render;
import cg.cli.runners;
import cg.cli.layout;
import cg.cli.index;
import cg.cli.update;
import cg.cli.msbuild;
import cg.cli.config;
import cg.cli.hook;
import cg.cli.commands.matchers;

export namespace cg::cli
{

    /**
     * @brief 第 4 层建议：在 LIKE 收窄的候选池上做
     *        大小写、分隔符不敏感的编辑距离，取最好的 max 个非限定名、
     *        最近者在前。用于"你是不是想找"提示。
     *
     * @internal
     * 定义（外场反馈修正后的建议规则）：候选池 = 前导 3 字符针 + "少一错"
     * 前缀针 + 完整末段针的 LIKE 并集；排序 = 查询末段 fuzzy 键对候选
     * 名 fuzzy 键的编辑距离升序，同名去重，零距离者保留且必居首。
     * soundness 边界：本函数只在层 1-3 全空（tier 0）时被调用，零距离
     * 候选不可能与层 1-3 的命中重复——大小写/分隔符差异正是层 1-3 全
     * 空而零距离命中的形态；池针是子串 LIKE，前 3 字符拼错的查询三针
     * 全落空。
     * fallback 清单：池空 → 返回空建议（不做全库扫描兜底，成本失控，
     * 宁缺勿滥）；限定名拼错命名空间 → 完整末段针救回目标符号。
     */
    [[nodiscard]]
    auto suggest_similar(
        cg::db::GraphStore& store,
        std::string_view    query,
        std::size_t         max) -> std::vector<std::string>
    {
        const auto             parts = cg::util::split_qualified(query);
        const std::string_view last = parts.empty() ? query : parts.back();
        if (last.empty())
        {
            return {};
        }

        std::vector<std::string_view> needles;
        if (last.size() >= 3)
        {
            needles.push_back(last.substr(0, 3));
        }
        if (last.size() > 2)
        {
            needles.push_back(last.substr(0, last.size() - 1));
        }
        // 完整末段针：库内精确/包含匹配符号必入池——限定名拼错
        // 命名空间（查 "A::X" 而 X 实际挂在别处）时层 1-3 全空，这根针
        // 是救回 X 的唯一途径；与 3 字符针去重。
        cg::util::push_unique(needles, last);

        std::unordered_map<cg::db::SymId, cg::db::SymbolRow> pool;
        for (const std::string_view needle : needles)
        {
            for (cg::db::SymbolRow& row : cg::db::symbols_name_like(
                     store,
                     needle,
                     cg::db::SymbolFilter{}))
            {
                pool.emplace(row.id, std::move(row));
            }
        }
        if (pool.empty())
        {
            return {};
        }

        // 距离锚取查询末段而非整串：整串带着命名空间字母去比非
        // 限定名，距离被限定符长度系统性抬高，排序失真。
        const std::string key = detail::fuzzy_key(last);
        std::vector<std::pair<std::size_t, const cg::db::SymbolRow*>> ranked;
        ranked.reserve(pool.size());
        for (const auto& [id, row] : pool)
        {
            ranked.emplace_back(
                detail::edit_distance(key, detail::fuzzy_key(row.name)),
                &row);
        }
        std::ranges::sort(
            ranked,
            [](const auto& a, const auto& b)
            {
                return a.first != b.first ? a.first < b.first
                                          : a.second->name < b.second->name;
            });

        std::vector<std::string> out;
        for (const auto& [distance, row] : ranked)
        {
            if (out.size() >= max)
            {
                break;
            }
            // 零距离候选不跳过（修正核心）：本函数只在层 1-3 全空时被
            // 调用，"精确匹配已在别处存在"的前提不成立——零距离者（大小
            // 写/分隔符差异，如 "mywidget" vs "MyWidget"）正是被旧规则
            // 丢弃的最佳建议，现在必居首位。
            (void)distance;
            if (std::ranges::find(out, row->name) == out.end())
            {
                out.push_back(row->name);
            }
        }
        return out;
    }

    /**
     * @brief 把一个符号参数解析到唯一目标（层合并版判据）。
     *
     * 零命中或歧义时打印统一错误形状（JSON 模式的错误载荷走 stdout、
     * 文本模式走 stderr；歧义情形附候选清单）并返回空；调用方将其映
     * 射为退出 1。
     *
     * @internal
     * 定义：find_symbols 现合并三层命中，matches 变多不再自动
     * 等于歧义——解析优先看精确子集（name/qualified_name/unique_key 恰
     * 等于查询的命中）：子集恰一 → 直接用（substring 噪声不产生歧义报
     * 错，唯一精确命中的行为与合并前逐字节一致）；子集多个 → 歧义，候
     * 选只报精确子集；子集空（无精确命中）→ 全部 matches 参与判定
     * （层 2/3 的旧语义）。
     * soundness 边界：精确判据与层 1 的收集判据逐字相同，三层去重后精
     * 确子集恰好重建层 1 的命中集。
     * fallback 清单：无。
     */
    [[nodiscard]]
    auto resolve_unique_target(
        cg::db::GraphStore& store,
        const Invocation&   inv,
        std::string_view    name) -> std::optional<cg::db::SymbolRow>
    {
        const cg::db::FindResult found = cg::db::find_symbols(
            store,
            name,
            detail::to_filter(inv),
            /*merge_lower_tiers=*/false);
        if (found.tier == 0)
        {
            std::vector<std::string> suggestions;
            if (!inv.kind.has_value() && !inv.file_substring.has_value())
            {
                suggestions = suggest_similar(store, name, 5);
            }
            detail::emit_not_found(inv, name, suggestions);
            return std::nullopt;
        }
        if (found.tier == 1)
        {
            // 短路档下层不参与，matches 即层 1 命中集；精确过滤是双保险
            // （判据与层 1 收集逐字相同）。
            std::vector<cg::db::SymbolRow> exact;
            for (const cg::db::SymbolRow& m : found.matches)
            {
                if (m.name == name || m.qualified_name == name
                    || m.unique_key == name)
                {
                    exact.push_back(m);
                }
            }
            if (exact.size() > 1)
            {
                detail::emit_ambiguous(inv, name, exact);
                return std::nullopt;
            }
            if (exact.size() == 1)
            {
                return exact.front();
            }
        }
        if (found.matches.size() > 1)
        {
            detail::emit_ambiguous(inv, name, found.matches);
            return std::nullopt;
        }
        return found.matches.front();
    }

    /**
     * @brief 把一条规范化变更文件路径（ccg changes）解析到 files 行。
     *
     * 先精确文本；再大小写不敏感全路径（库保有自己的拼写——盘上规范
     * 大小写——而已删除文件没有可重拼的盘上大小写）；最后唯一的大小写
     * 不敏感路径后缀匹配（相对另一工作目录喂入的路径清单）。
     *
     * @internal 歧义或未匹配返回空——调用方报告该路径，绝不静默丢弃。
     * 后缀轮遍历喂入路径在 '/' 边界上的每个后缀、对文件路径按段边界
     * 匹配。
     */
    /** @brief 按 id 去重地把文件行收进 matches。 */
    auto push_unique_file(
        std::vector<cg::db::FileRow>& matches,
        const cg::db::FileRow&        f) -> void
    {
        if (std::ranges::find(matches, f.id, &cg::db::FileRow::id)
            == matches.end())
        {
            matches.push_back(f);
        }
    }

    [[nodiscard]]
    auto resolve_input_path(
        cg::db::GraphStore& store,
        std::string_view    normalized) -> std::optional<cg::db::FileRow>
    {
        if (auto row = store.find_file(normalized))
        {
            return row;
        }
        const std::string want = cg::util::path_compare_key(normalized);
        const std::vector<cg::db::FileRow> files = cg::db::all_files(store);

        std::optional<cg::db::FileRow> hit;
        bool                           ambiguous = false;
        for (const cg::db::FileRow& f : files)
        {
            if (cg::util::path_compare_key(f.path) == want)
            {
                if (hit.has_value())
                {
                    ambiguous = true;
                }
                else
                {
                    hit = f;
                }
            }
        }
        if (hit.has_value() && !ambiguous)
        {
            return hit;
        }

        std::vector<cg::db::FileRow> matches;
        for (std::size_t i = want.find('/'); i != std::string::npos;
             i = want.find('/', i + 1))
        {
            const std::string suffix = want.substr(i + 1);
            if (suffix.empty())
            {
                continue;
            }
            for (const cg::db::FileRow& f : files)
            {
                const std::string have = cg::util::path_compare_key(f.path);
                if (have == suffix
                    || (have.size() > suffix.size() && have.ends_with(suffix)
                        && have[have.size() - suffix.size() - 1] == '/'))
                {
                    push_unique_file(matches, f);
                }
            }
        }
        if (matches.size() == 1)
        {
            return matches.front();
        }
        return std::nullopt;
    }
} // namespace cg::cli
