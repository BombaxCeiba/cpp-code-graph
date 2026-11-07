module;
#include "../db/db.tables.h"
#include "../msbuild/capture.tables.h"

/**
 * @module cg.cli.commands
 *
 * @brief 查询侧命令运行器与共享存储助手：解析后的 Invocation、
 *        cg.db / cg.graph 与渲染器之间的胶合层。
 *
 * 持有退出码契约（0 成功、1 未找到/歧义/运行时错误、2 用法）
 * 与符号名解析层级（含第 4 层"你是不是想找"建议）。
 *
 * @internal
 * index/update/msbuild/config/hook 运行器住在各自的接口模块
 * （cg.cli.index/update/msbuild/config/hook；共享脊柱与存储/扫描管道
 * 在 cg.cli.runners——模块布局定稿后）；下方的 dispatch 恰好导入那些模块，
 * 即运行侧所需的扇入点。本单元刻意不导入 cg.clang。db.tables.h 置于
 * 全局模块片段：本单元导入 cg.db，MSVC 要求 sqlpp23 表模型定义在此
 * 可见，以合并模块记录的模板特化（完整机理见 db.tables.h）。
 */
export module cg.cli.commands;

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

export namespace cg::cli
{
    namespace detail
    {

        namespace fs = std::filesystem;
        namespace json = cg::util::json;

        /** @brief 把 Invocation 的过滤选项折成 cg::db::SymbolFilter。 */
        [[nodiscard]]
        auto to_filter(const Invocation& inv) -> cg::db::SymbolFilter
        {
            cg::db::SymbolFilter f;
            f.kind = inv.kind;
            if (inv.file_substring.has_value())
            {
                f.file_substring = std::string_view{*inv.file_substring};
            }
            return f;
        }

        /** @brief 打印"未找到符号"错误（文本/JSON 两种形状，含建议）。 */
        void emit_not_found(
            const Invocation&               inv,
            std::string_view                name,
            const std::vector<std::string>& suggestions)
        {
            if (inv.json)
            {
                json::Object o;
                o.set("error", std::format("no symbol matching '{}'", name));
                o.set("partial", false);
                if (!suggestions.empty())
                {
                    json::Array arr;
                    for (const std::string& s : suggestions)
                    {
                        arr.add(s);
                    }
                    o.set("suggestions", std::move(arr));
                }
                std::println("{}", json::stringify(std::move(o)));
                return;
            }
            std::string msg = std::format("no symbol matching '{}'", name);
            if (!suggestions.empty())
            {
                msg += " (did you mean: ";
                for (std::size_t i = 0; i < suggestions.size(); ++i)
                {
                    if (i != 0)
                    {
                        msg += ", ";
                    }
                    msg += suggestions[i];
                }
                msg += "?)";
            }
            std::println(std::cerr, "ccg: error: {}", msg);
        }

        /** @brief 打印"符号歧义"错误并附候选清单。 */
        void emit_ambiguous(
            const Invocation&                  inv,
            std::string_view                   name,
            std::span<const cg::db::SymbolRow> matches)
        {
            const std::string message = std::format(
                "ambiguous symbol '{}': {} match{}, narrow with "
                "--file/--kind or a longer qualified name",
                name,
                matches.size(),
                matches.size() == 1 ? "" : "es");
            if (inv.json)
            {
                json::Object o;
                o.set("error", message);
                o.set("partial", false);
                o.set("candidates", render::candidates_json(matches));
                std::println("{}", json::stringify(std::move(o)));
                return;
            }
            std::println(std::cerr, "ccg: error: {}", message);
            std::print(std::cerr, "{}", render::candidates_text(matches));
        }

        /** @brief 打印一条普通错误（文本/JSON 两种形状）。 */
        void emit_error(const Invocation& inv, std::string_view message)
        {
            if (inv.json)
            {
                json::Object o;
                o.set("error", message);
                o.set("partial", false);
                std::println("{}", json::stringify(std::move(o)));
            }
            else
            {
                std::println(std::cerr, "ccg: error: {}", message);
            }
        }

        /**
         * @brief Levenshtein 距离，双行动态规划。
         *
         * @internal 输入是短名字，O(n*m) 足够。
         */
        [[nodiscard]]
        auto edit_distance(std::string_view a, std::string_view b)
            -> std::size_t
        {
            std::vector<std::size_t> prev(b.size() + 1), cur(b.size() + 1);
            for (std::size_t j = 0; j <= b.size(); ++j)
            {
                prev[j] = j;
            }
            for (std::size_t i = 1; i <= a.size(); ++i)
            {
                cur[0] = i;
                for (std::size_t j = 1; j <= b.size(); ++j)
                {
                    const std::size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
                    cur[j] = std::min(
                        {prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
                }
                std::swap(prev, cur);
            }
            return prev[b.size()];
        }

        /**
         * @brief 大小写与分隔符不敏感的比较键（第 4 层"你是不是想找"
         *        模糊建议：剥掉 '_' 与 "::"，转小写）。
         */
        [[nodiscard]]
        auto fuzzy_key(std::string_view s) -> std::string
        {
            std::string out;
            out.reserve(s.size());
            for (const char c : cg::util::ascii_lower(s))
            {
                if (c != '_' && c != ':')
                {
                    out.push_back(c);
                }
            }
            return out;
        }

        /**
         * @brief 结果端路径排除的 glob 匹配（--exclude-glob，外场反馈）。
         *
         * 定义：`**` 匹配任意字符（含 '/'）；`*` 匹配一节内的非 '/' 字符；
         * `?` 匹配单个非 '/' 字符；比较对 ASCII 字母不敏感（库内路径已规范化
         * 为正斜杠，Windows 盘符大小写不稳）。模式不以 '/' 开头时隐式前置
         * "任意深度目录"段（两个星号紧跟一个斜杠）——`"build/**"` 排除任意深度
         * 的 build/ 子树，无需写出绝对前缀；以 '/' 开头则锚定完整路径。
         *
         * soundness 边界：只影响展示端（命中条目从结果剔除），绝不动索引与
         * 库内数据——全量收录、用户主动过滤的配套面；多次给出取并集（任一
         * 命中即排除）。
         *
         * fallback 清单：方括号字符类等高级语法不支持（按字面比较）；空模式在
         * 解析期已拒绝；匹配不上的路径原样保留（宁多列不漏列）。
         */
        [[nodiscard]]
        auto glob_match(std::string_view p, std::string_view t) -> bool
        {
            const auto fold = [](char c)
            { return cg::util::ascii_lower_char(c); };
            if (p.starts_with("**"))
            {
                std::string_view rest = p.substr(2);
                if (!rest.empty() && rest.front() == '/')
                {
                    rest.remove_prefix(1);
                    // "**/x"：当前位置或任意 '/' 段边界之后匹配 x。
                    if (glob_match(rest, t))
                    {
                        return true;
                    }
                    for (std::size_t i = 0; i < t.size(); ++i)
                    {
                        if (t[i] == '/' && glob_match(rest, t.substr(i + 1)))
                        {
                            return true;
                        }
                    }
                    return false;
                }
                // 裸 "**"：吞任意字符（含 '/'），含空串。
                for (std::size_t i = 0; i <= t.size(); ++i)
                {
                    if (glob_match(rest, t.substr(i)))
                    {
                        return true;
                    }
                }
                return false;
            }
            if (p.empty())
            {
                return t.empty();
            }
            const char c = p.front();
            if (c == '*')
            {
                // 单 '*'：一节内的任意 run（不跨 '/'）。
                const std::string_view rest = p.substr(1);
                for (std::size_t len = 0; len <= t.size(); ++len)
                {
                    if (len > 0 && t[len - 1] == '/')
                    {
                        break;
                    }
                    if (glob_match(rest, t.substr(len)))
                    {
                        return true;
                    }
                }
                return false;
            }
            if (c == '?')
            {
                return !t.empty() && t.front() != '/'
                       && glob_match(p.substr(1), t.substr(1));
            }
            return !t.empty() && fold(c) == fold(t.front())
                   && glob_match(p.substr(1), t.substr(1));
        }

        /** @brief 路径是否命中任一排除 glob（未给模式时恒 false）。 */
        [[nodiscard]]
        auto path_excluded(
            std::span<const std::string> globs,
            std::string_view             path) -> bool
        {
            for (const std::string& g : globs)
            {
                if (g.starts_with('/'))
                {
                    if (glob_match(g, path))
                    {
                        return true;
                    }
                }
                else if (glob_match(std::string{"**/"} + g, path))
                {
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief 目标及其覆盖族的取址证据行（消费簇 1 契约，收集面）。
         *
         * @internal family 各成员的 unique_key 并入查询（&Base::m 与派生
         * override 的证据各挂各名下，db 侧不做家族展开——见
         * cg::db::address_taken_hints 的三要素注释）。
         */
        [[nodiscard]]
        auto address_taken_sites(
            cg::db::GraphStore&               store,
            const cg::db::SymbolRow&          target,
            const std::vector<cg::db::SymId>& family)
            -> std::vector<cg::db::TypeHintRow>
        {
            // 键先物化为 string：get_symbol 的返回是临时，view 不能悬垂其上。
            std::vector<std::string> owned;
            owned.reserve(family.size() + 1);
            owned.push_back(target.unique_key);
            for (const cg::db::SymId id : family)
            {
                if (const auto row = cg::db::get_symbol(store, id);
                    row
                    && std::ranges::find(owned, row->unique_key) == owned.end())
                {
                    owned.push_back(row->unique_key);
                }
            }
            const std::vector<std::string_view> keys{
                owned.begin(),
                owned.end()};
            return cg::db::address_taken_hints(store, keys);
        }

    } // namespace detail

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
        const auto                   add_unique = [&](const cg::db::FileRow& f)
        {
            if (std::ranges::find(matches, f.id, &cg::db::FileRow::id)
                == matches.end())
            {
                matches.push_back(f);
            }
        };
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
                    add_unique(f);
                }
            }
        }
        if (matches.size() == 1)
        {
            return matches.front();
        }
        return std::nullopt;
    }

    namespace detail
    {

        /** @brief `ccg query` 运行器：按名查符号。 */
        auto run_query(cg::db::GraphStore& store, const Invocation& inv) -> int
        {
            const std::string& name = inv.positional.front();
            if (inv.implements)
            {
                // --implements：positional 当接口/基类，列 extends 派生树
                // （BFS 防环，深度列标注层级；与虚函数 override 家族无关）。
                const auto target = resolve_unique_target(store, inv, name);
                if (!target.has_value())
                {
                    return 1;
                }
                std::vector<render::ImplementRow>          rows;
                std::vector<cg::db::SymId>                 visited{target->id};
                std::vector<std::pair<cg::db::SymId, int>> pending{
                    {target->id, 1}};
                while (!pending.empty())
                {
                    const auto [id, depth] = pending.front();
                    pending.erase(pending.begin());
                    for (const cg::db::EdgeRow& e : cg::db::deriving(store, id))
                    {
                        if (std::ranges::find(visited, e.src) != visited.end())
                        {
                            continue;
                        }
                        visited.push_back(e.src);
                        if (const auto row = cg::db::get_symbol(store, e.src))
                        {
                            rows.push_back({*row, depth});
                        }
                        pending.emplace_back(e.src, depth + 1);
                    }
                }
                if (inv.json)
                {
                    std::println("{}", render::implements_json(*target, rows));
                }
                else
                {
                    std::print("{}", render::implements_text(*target, rows));
                }
                return 0;
            }
            const cg::db::FindResult found =
                cg::db::find_symbols(store, name, to_filter(inv));
            if (found.tier == 0)
            {
                std::vector<std::string> suggestions;
                if (!inv.kind.has_value() && !inv.file_substring.has_value())
                {
                    suggestions = suggest_similar(store, name, 5);
                }
                emit_not_found(inv, name, suggestions);
                return 1;
            }
            if (inv.json)
            {
                std::println(
                    "{}",
                    render::query_json(name, found.tier, found.matches));
            }
            else
            {
                std::print(
                    "{}",
                    render::query_text(name, found.tier, found.matches));
            }
            return 0;
        }

        /** @brief `ccg context` 运行器：360 度符号视图。 */
        auto run_context(cg::db::GraphStore& store, const Invocation& inv)
            -> int
        {
            const auto target =
                resolve_unique_target(store, inv, inv.positional.front());
            if (!target.has_value())
            {
                return 1;
            }
            cg::graph::ContextView view = cg::graph::context(
                store,
                target->id,
                /*merge_family=*/!inv.no_merge,
                inv.dispatch_mode);
            // --exclude-glob：结果端剔除（对端符号定义路径命中的边；库不动）。
            if (!inv.exclude_globs.empty())
            {
                const auto drop = [&](const cg::graph::ContextEdge& e)
                { return path_excluded(inv.exclude_globs, e.other.file); };
                std::erase_if(view.incoming, drop);
                std::erase_if(view.outgoing, drop);
            }
            // 取址证据（消费簇 1 契约）：与 callers 同款盲区标注——经函数/成员
            // 指针的调用 incoming 天然看不见，有证据行时点破。
            const std::vector<cg::db::TypeHintRow> addr =
                address_taken_sites(store, *target, view.family);
            if (inv.json)
            {
                std::println("{}", render::context_json(view, store, addr));
            }
            else
            {
                std::print("{}", render::context_text(store, view, addr));
            }
            return 0;
        }

        /**
         * @brief `ccg members` 运行器：记录成员清单（外场反馈要求成员面
         *        不用回头读头文件）。
         *
         * @internal 非 record 目标是可行动错误（点破 kind，附 --kind 习惯）；
         * record 无成员是合法答案（空清单、退出 0）。--kind/--file 沿用符号
         * 命令的过滤语义（to_filter 同判据）。
         */
        auto run_members(cg::db::GraphStore& store, const Invocation& inv)
            -> int
        {
            const auto target =
                resolve_unique_target(store, inv, inv.positional.front());
            if (!target.has_value())
            {
                return 1;
            }
            if (target->kind != cg::db::SymbolKind::Record)
            {
                emit_error(
                    inv,
                    std::format(
                        "{} is a {}, not a record; members lists a "
                        "record's methods and fields (resolve the "
                        "record with --kind record)",
                        target->qualified_name,
                        cg::db::name_of(target->kind)));
                return 1;
            }
            const cg::db::SymbolFilter     filter = to_filter(inv);
            std::vector<cg::db::MemberRow> rows;
            for (cg::db::MemberRow& m :
                 cg::db::members_with_origin(store, target->id))
            {
                if (filter.kind.has_value() && m.symbol.kind != *filter.kind)
                {
                    continue;
                }
                if (filter.file_substring.has_value()
                    && m.symbol.file.find(*filter.file_substring)
                           == std::string::npos)
                {
                    continue;
                }
                rows.push_back(std::move(m));
            }
            if (inv.json)
            {
                std::println("{}", render::members_json(*target, rows));
            }
            else
            {
                std::print("{}", render::members_text(*target, rows));
            }
            return 0;
        }

        /** @brief `ccg callers` / `ccg callees` 运行器。 */
        auto run_neighbors(cg::db::GraphStore& store, const Invocation& inv)
            -> int
        {
            const bool up = inv.cmd == Invocation::Cmd::Callers;
            const auto target =
                resolve_unique_target(store, inv, inv.positional.front());
            if (!target.has_value())
            {
                return 1;
            }
            cg::graph::NeighborsResult result =
                up ? cg::graph::callers(
                         store,
                         target->id,
                         inv.depth,
                         /*merge_family=*/!inv.no_merge,
                         inv.dispatch_mode)
                   : cg::graph::callees(
                         store,
                         target->id,
                         inv.depth,
                         /*merge_family=*/!inv.no_merge,
                         inv.dispatch_mode);
            // --exclude-glob：结果端剔除（定义路径命中的条目；索引与库不动）。
            if (!inv.exclude_globs.empty())
            {
                std::erase_if(
                    result.entries,
                    [&](const cg::graph::NeighborEntry& e)
                    {
                        const auto row = cg::db::get_symbol(store, e.id);
                        return row
                               && path_excluded(inv.exclude_globs, row->file);
                    });
            }
            // 取址证据（消费簇 1 契约）：callers 侧的调用盲区标注。
            std::vector<cg::db::TypeHintRow> addr;
            if (up)
            {
                addr = address_taken_sites(store, *target, result.family);
            }
            if (result.entries.empty())
            {
                // 零调用者是有效答案而非错误（外场反馈要求）：文本走 stdout
                // 正常结果形态（无 "ccg: error:" 前缀），--json
                // 输出完整正常载荷而非 error 对象；退出码维持
                // 1，以免破坏按码判错的既有脚本。死代码候选
                // 提示单列“经函数/成员指针调用不可见”——成员指针取址不建
                // 边之外，这是最常被误读成“没人用”的盲区；有取址证据行时
                // 附具体站点。
                if (!up)
                {
                    emit_error(inv, "no callees found");
                    return 1;
                }
                const bool off =
                    inv.dispatch_mode == cg::graph::DispatchMode::Off;
                // 置信标注（三种假零路径逐项实查）：指针调用是机制性盲区恒真；
                // sibling 仅虚目标且派发视图在用时可能；取址站点直接查库。
                const render::CallersBlindSpots blind{
                    .pointer_calls_invisible = true,
                    .sibling_dispatch_possible =
                        (target->flags & cg::db::sym_flags::is_virtual) != 0
                        && !off,
                    .member_address_taken = !addr.empty()};
                if (inv.json)
                {
                    std::println(
                        "{}",
                        render::neighbors_json(
                            true,
                            *target,
                            inv.depth,
                            result,
                            store,
                            inv.top,
                            addr,
                            &blind));
                }
                else
                {
                    std::string msg = std::format(
                        "no callers found — dead-code candidate unless one of "
                        "three invisible paths applies: indirect calls through "
                        "function/member pointers (never indexed); virtual "
                        "dispatch to a sibling override (target is virtual: "
                        "{}, use --no-dispatch for the static upper bound); "
                        "address taken at {} site(s) below",
                        blind.sibling_dispatch_possible ? "yes" : "no",
                        addr.size());
                    std::print(
                        "{}\n{}",
                        msg,
                        render::address_taken_text(store, *target, addr));
                }
                return 1;
            }
            if (inv.json)
            {
                std::println(
                    "{}",
                    render::neighbors_json(
                        up,
                        *target,
                        inv.depth,
                        result,
                        store,
                        inv.top,
                        addr));
            }
            else
            {
                std::print(
                    "{}",
                    render::neighbors_text(
                        up,
                        *target,
                        result,
                        store,
                        inv.top,
                        addr));
            }
            return 0;
        }

        /** @brief `ccg impact` 运行器：影响面。 */
        auto run_impact(cg::db::GraphStore& store, const Invocation& inv) -> int
        {
            const auto target =
                resolve_unique_target(store, inv, inv.positional.front());
            if (!target.has_value())
            {
                return 1;
            }
            cg::graph::ImpactResult result = cg::graph::impact(
                store,
                target->id,
                inv.direction,
                inv.depth,
                /*merge_family=*/!inv.no_merge,
                inv.dispatch_mode);
            // --exclude-glob：结果端剔除（定义路径命中的条目）；impacted/
            // direct/transitive 随过滤重算（计数与列表同口径），fan_in/
            // risk_score/risk 保持全量值——影响面的保守上界，过滤只治信噪比。
            if (!inv.exclude_globs.empty())
            {
                std::int64_t direct = 0;
                for (cg::graph::ImpactLevel& level : result.by_depth)
                {
                    std::erase_if(
                        level.ids,
                        [&](cg::db::SymId id)
                        {
                            const auto row = cg::db::get_symbol(store, id);
                            return row
                                   && path_excluded(
                                       inv.exclude_globs,
                                       row->file);
                        });
                    if (level.depth == 1)
                    {
                        direct = static_cast<std::int64_t>(level.ids.size());
                    }
                }
                std::erase_if(
                    result.by_depth,
                    [](const cg::graph::ImpactLevel& l)
                    { return l.ids.empty(); });
                std::int64_t impacted = 0;
                for (const cg::graph::ImpactLevel& level : result.by_depth)
                {
                    impacted += static_cast<std::int64_t>(level.ids.size());
                }
                result.impacted = impacted;
                result.direct = direct;
                result.transitive = impacted - direct;
            }
            // 取址证据（消费簇 1 契约）：只对 up（谁依赖它）有意义——down 是
            // 它依赖别人，不受成员指针调用盲区影响。
            std::vector<cg::db::TypeHintRow> addr;
            if (inv.direction == cg::graph::Direction::Up)
            {
                addr = address_taken_sites(store, *target, result.family);
            }
            if (inv.json)
            {
                std::println(
                    "{}",
                    render::impact_json(
                        *target,
                        inv.direction,
                        inv.depth,
                        result,
                        store,
                        inv.top,
                        addr));
            }
            else
            {
                std::print(
                    "{}",
                    render::impact_text(
                        *target,
                        inv.direction,
                        result,
                        store,
                        inv.top,
                        addr));
            }
            return 0;
        }

        /**
         * @brief `ccg trace` 运行器：两符号间最短调用路径。
         *
         * @internal 不可达是未找到结果：文本模式把退出 1 错误报在 stderr
         * （JSON 模式的错误一律走 stdout，调用方只解析一条流）；JSON 在
         * stdout 保留文档化的 {found:false} 载荷。
         */
        auto run_trace(cg::db::GraphStore& store, const Invocation& inv) -> int
        {
            const auto from =
                resolve_unique_target(store, inv, inv.positional[0]);
            if (!from.has_value())
            {
                return 1;
            }
            const auto to =
                resolve_unique_target(store, inv, inv.positional[1]);
            if (!to.has_value())
            {
                return 1;
            }
            const cg::graph::TraceResult result = cg::graph::trace(
                store,
                from->id,
                to->id,
                10,
                inv.dispatch_mode);
            if (!result.found && !inv.json)
            {
                std::println(
                    std::cerr,
                    "no path from {} to {} within 10 hops",
                    from->qualified_name,
                    to->qualified_name);
                return 1;
            }
            if (inv.json)
            {
                std::println(
                    "{}",
                    render::trace_json(*from, *to, result, store));
            }
            else
            {
                std::print("{}", render::trace_text(*from, *to, result, store));
            }
            return result.found ? 0 : 1;
        }

        /** @brief `ccg stats` 运行器：库摘要。 */
        auto run_stats(cg::db::GraphStore& store, const Invocation& inv) -> int
        {
            render::StatsInputs in;
            in.db_path = inv.db_path;
            if (inv.db_path != ":memory:")
            {
                std::error_code ec;
                if (const auto size = fs::file_size(fs::path{inv.db_path}, ec);
                    !ec)
                {
                    in.db_bytes = static_cast<std::int64_t>(size);
                }
                if (const auto size =
                        fs::file_size(fs::path{inv.db_path + "-wal"}, ec);
                    !ec)
                {
                    in.wal_bytes = static_cast<std::int64_t>(size);
                }
            }
            if (const auto root = store.meta("project_root"))
            {
                in.root = *root;
            }
            in.dispatch_stale_files =
                cg::util::parse_bounded(store.meta("dispatch_stale"), 0);
            // 槽侧快照的最近一次运行观测（meta analysis_snap_*）。
            if (const auto v = store.meta("analysis_snap_state"))
            {
                in.snap_state = *v;
            }
            if (const auto v = store.meta("analysis_snap_restore_us"))
            {
                in.snap_restore_us = std::atoll(v->c_str());
            }
            if (const auto v = store.meta("analysis_snap_dirty_files"))
            {
                in.snap_dirty_files = std::atoll(v->c_str());
            }
            if (const auto v = store.meta("analysis_snap_rebuilt_slots"))
            {
                in.snap_rebuilt_slots = std::atoll(v->c_str());
            }
            if (const auto v = store.meta("analysis_snap_bytes"))
            {
                in.snap_bytes = std::atoll(v->c_str());
            }
            in.st = cg::db::stats(store);
            // failed 点名（外场反馈要求）：files 行带着 parse_status
            // 与路径，一次扫描即得清单；stats 本身就是全库摘要命令，O(files)
            // 于此可接受。
            for (const cg::db::FileRow& f : cg::db::all_files(store))
            {
                if (f.status == cg::util::ParseStatus::Failed)
                {
                    in.failed_files.push_back(f.path);
                }
            }
            if (inv.json)
            {
                std::println("{}", render::stats_json(in));
            }
            else
            {
                std::print("{}", render::stats_text(in));
            }
            return 0;
        }

        /**
         * @brief 修剪一条喂入行并剥掉整段引号拼写（"a b.hpp" -> a b.hpp）；
         *        空结果由调用方跳过。
         */
        void clean_list_line(std::string& line)
        {
            line = cg::util::trim(line);
            if (line.empty())
            {
                return;
            }
            if (line.size() >= 2 && line.front() == '"' && line.back() == '"')
            {
                line = line.substr(1, line.size() - 2);
            }
        }

        /**
         * @brief `ccg changes` 运行器：变更文件的上游影响面。
         *
         * @internal
         * 输入模式：位置参数路径、--from-file 清单、stdin
         * （'-' 操作数、--from-file -、或无操作数而 stdin 已接管道——
         * `git diff --name-only | ccg changes`）。ccg 自身绝不运行任何版本
         * 控制工具。读已排空的 stdin 是空操作，故 stdin 读取器无需只读一次
         * 守卫。规范化与索引器完全一致（绝对化 + 词典序规范化 + norm_path），
         * 按规范化形去重、保留首个喂入拼写供报告。随后对库解析、映射到文件
         * 定义的全部符号、沿上游走查。
         */
        auto run_changes(cg::db::GraphStore& store, const Invocation& inv)
            -> int
        {
            std::vector<std::string> raw;
            const auto               take_stdin = [&]()
            {
                std::string line;
                while (std::getline(std::cin, line))
                {
                    clean_list_line(line);
                    if (!line.empty())
                    {
                        raw.push_back(line);
                    }
                }
            };
            for (const std::string& p : inv.positional)
            {
                if (p == "-")
                {
                    take_stdin();
                }
                else
                {
                    raw.push_back(p);
                }
            }
            if (!inv.from_file.empty())
            {
                if (inv.from_file == "-")
                {
                    take_stdin();
                }
                else
                {
                    std::ifstream in{fs::path{inv.from_file}};
                    if (!in)
                    {
                        throw std::runtime_error(
                            std::format(
                                "cannot read --from-file '{}'",
                                inv.from_file));
                    }
                    std::string line;
                    while (std::getline(in, line))
                    {
                        clean_list_line(line);
                        if (!line.empty())
                        {
                            raw.push_back(line);
                        }
                    }
                }
            }
            if (raw.empty())
            {
                if (cg::util::platform::stdin_is_piped())
                {
                    take_stdin();
                }
                else
                {
                    throw UsageError{
                        "no changed files given (pass file arguments, --from-file F, "
                        "or pipe a path list on stdin)",
                        usage_for(Invocation::Cmd::Changes)};
                }
            }

            std::vector<std::pair<std::string, std::string>>
                                            deduped; // (喂入, 规范)
            std::unordered_set<std::string> seen;
            for (const std::string& p : raw)
            {
                std::error_code ec;
                fs::path        abs = fs::absolute(fs::path{p}, ec);
                if (ec)
                {
                    abs = fs::path{p};
                }
                const std::string norm = cg::util::norm_path(
                    abs.lexically_normal().generic_string());
                if (seen.insert(norm).second)
                {
                    deduped.emplace_back(p, norm);
                }
            }

            render::ChangesInputs in;
            in.depth = inv.depth;
            std::vector<cg::db::SymId> seeds;
            for (const auto& [input, norm] : deduped)
            {
                ChangedFile f;
                f.input = input;
                if (const auto row = resolve_input_path(store, norm))
                {
                    f.known = true;
                    f.path = row->path;
                    std::error_code ec;
                    f.deleted = !fs::exists(fs::path{row->path}, ec) || ec;
                    const std::vector<cg::db::SymbolRow> syms =
                        cg::db::symbols_of_file(store, row->id);
                    f.symbols = static_cast<std::int64_t>(syms.size());
                    for (const cg::db::SymbolRow& s : syms)
                    {
                        seeds.push_back(s.id);
                    }
                }
                in.files.push_back(std::move(f));
            }
            in.result =
                cg::graph::changes(store, seeds, inv.depth, inv.dispatch_mode);

            const bool nothing_found =
                in.files.empty() || in.result.consumers.empty();
            if (inv.json)
            {
                std::println("{}", render::changes_json(in, store));
            }
            else
            {
                std::print("{}", render::changes_text(in, store));
                if (nothing_found)
                {
                    std::println(
                        std::cerr,
                        "ccg: nothing found (no known files or "
                        "no consumers)");
                }
            }
            return nothing_found ? 1 : 0;
        }

        /**
         * @brief `ccg check --cycles` 运行器：包含图环检测。
         *
         * @internal 发现环时用独立退出码 3（CI 门禁用；0 = 无环）。
         */
        auto run_check(cg::db::GraphStore& store, const Invocation& inv) -> int
        {
            if (!inv.cycles)
            {
                throw UsageError{
                    "check needs a mode; the only one today is --cycles",
                    usage_for(Invocation::Cmd::Check)};
            }
            const cg::graph::CyclesResult res = cg::graph::include_cycles(
                store,
                static_cast<std::size_t>(inv.list_cap));
            if (inv.json)
            {
                std::println("{}", render::cycles_json(res, store));
            }
            else
            {
                std::print("{}", render::cycles_text(res, store));
            }
            return res.groups_total > 0 ? 3 : 0;
        }

        /**
         * @brief `ccg graph` 运行器：调用图导出（dot / JSON）。
         *
         * @internal 两模式共用 --output 落盘路径；stdout 是机器可读载荷的默认
         * 目的地，写文件时一行注记走 stderr（stdout 保持纯净）。--json 与
         * --format json 等价（与查询族 --json 口径一致）；入口符号解析沿用
         * 层级解析与歧义候选报告（resolve_unique_target）。导出是忠实快照：
         * 无被调用者的入口仍产出单节点图、退出 0（与查询命令的"未找到"不同）。
         */
        auto run_graph(cg::db::GraphStore& store, const Invocation& inv) -> int
        {
            const bool  as_json = inv.json || inv.graph_format == "json";
            std::string payload;
            if (inv.graph_modules)
            {
                const cg::graph::ModuleGraph modules = cg::graph::module_graph(
                    store,
                    inv.file_substring.has_value()
                        ? std::string_view{*inv.file_substring}
                        : std::string_view{},
                    inv.min_weight);
                payload =
                    as_json
                        ? render::graph_modules_json(modules, inv.min_weight)
                        : render::graph_modules_dot(modules);
            }
            else
            {
                const auto target =
                    resolve_unique_target(store, inv, inv.graph_symbol);
                if (!target.has_value())
                {
                    return 1;
                }
                const cg::graph::CallClosure closure =
                    cg::graph::call_closure(store, target->id, inv.depth);
                payload = as_json ? render::graph_symbol_json(
                                        *target,
                                        inv.depth,
                                        closure,
                                        store)
                                  : render::graph_symbol_dot(
                                        *target,
                                        inv.depth,
                                        closure,
                                        store);
            }

            if (!payload.empty() && payload.back() != '\n')
            {
                payload += '\n'; // dot 自带尾换行；JSON 对齐查询族的行纪律
            }
            if (inv.output_file.empty())
            {
                std::print("{}", payload);
                return 0;
            }
            std::error_code ec;
            const fs::path  out_path{inv.output_file};
            if (!out_path.parent_path().empty())
            {
                fs::create_directories(out_path.parent_path(), ec);
            }
            std::ofstream out{out_path, std::ios::binary};
            if (!out)
            {
                throw std::runtime_error(
                    std::format("cannot write --output '{}'", inv.output_file));
            }
            out << payload;
            out.close();
            if (out.fail())
            {
                throw std::runtime_error(
                    std::format("cannot write --output '{}'", inv.output_file));
            }
            std::println(
                std::cerr,
                "ccg: wrote {} ({} bytes)",
                inv.output_file,
                payload.size());
            return 0;
        }

    } // namespace detail

    /**
     * @brief 运行一条已解析的调用。
     *
     * 抛 UsageError（-> 退出 2）与 std::exception（-> 退出 1，由
     * run_main 渲染）。
     *
     * @internal
     * 布局解析（cg.cli.layout）在此收口：一切读侧命令（查询族/stats/
     * changes/check/graph/update）先把 Invocation 解析到具体的
     * `<文件夹>`/`<配置>`/ccg.db（CCG_DB > -o > ./.ccg 发现；单配置自动选中、
     * 多配置点名候选、旧 ./ccg.db 给可行动提示）再进运行器；index/msbuild
     * 各自经 cg.cli.layout 解析自己的产出文件夹与配置。
     */
    [[nodiscard]]
    auto dispatch(const Invocation& invocation) -> int
    {
        Invocation inv = invocation;
        const auto resolve_query_db = [&inv]
        {
            const LayoutChoice choice = layout::resolve_query_layout(inv);
            inv.db_path = choice.db;
            inv.config_name = choice.config;
            inv.layout_root = choice.root;
        };
        switch (inv.cmd)
        {
        case Invocation::Cmd::Help:
        {
            if (!inv.positional.empty())
            {
                const auto cmd = command_of(inv.positional.front());
                std::print(
                    "{}",
                    cmd.has_value() ? usage_for(*cmd) : top_usage());
            }
            else
            {
                std::print("{}", top_usage());
            }
            return 0;
        }
        case Invocation::Cmd::Version:
            std::println("ccg {}", cg::util::version);
            return 0;
        case Invocation::Cmd::Index:
            return detail::run_index(inv);
        case Invocation::Cmd::Update:
            resolve_query_db();
            return detail::run_update(inv);
        case Invocation::Cmd::Msbuild:
            return detail::run_msbuild(inv);
        case Invocation::Cmd::Config:
            return detail::run_config(inv);
        case Invocation::Cmd::Hook:
            return detail::run_hook(inv);
        case Invocation::Cmd::Query:
        case Invocation::Cmd::Context:
        case Invocation::Cmd::Members:
        case Invocation::Cmd::Callers:
        case Invocation::Cmd::Callees:
        case Invocation::Cmd::Impact:
        case Invocation::Cmd::Trace:
        {
            resolve_query_db();
            std::optional<cg::db::GraphStore> store;
            detail::open_store_read(inv, store);
            switch (inv.cmd)
            {
            case Invocation::Cmd::Query:
                return detail::run_query(*store, inv);
            case Invocation::Cmd::Context:
                return detail::run_context(*store, inv);
            case Invocation::Cmd::Members:
                return detail::run_members(*store, inv);
            case Invocation::Cmd::Callers:
            case Invocation::Cmd::Callees:
                return detail::run_neighbors(*store, inv);
            case Invocation::Cmd::Impact:
                return detail::run_impact(*store, inv);
            default:
                return detail::run_trace(*store, inv);
            }
        }
        case Invocation::Cmd::Stats:
        {
            resolve_query_db();
            std::optional<cg::db::GraphStore> store;
            detail::open_store_read(inv, store);
            return detail::run_stats(*store, inv);
        }
        case Invocation::Cmd::Changes:
        {
            resolve_query_db();
            std::optional<cg::db::GraphStore> store;
            detail::open_store_read(inv, store);
            return detail::run_changes(*store, inv);
        }
        case Invocation::Cmd::Check:
        {
            resolve_query_db();
            std::optional<cg::db::GraphStore> store;
            detail::open_store_read(inv, store);
            return detail::run_check(*store, inv);
        }
        case Invocation::Cmd::Graph:
        {
            resolve_query_db();
            std::optional<cg::db::GraphStore> store;
            detail::open_store_read(inv, store);
            return detail::run_graph(*store, inv);
        }
        }
        throw UsageError{"unhandled command", top_usage()};
    }
} // namespace cg::cli
