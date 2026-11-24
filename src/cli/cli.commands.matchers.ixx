/**
 * @module cg.cli.commands.matchers
 *
 * @brief
 * 查询匹配域，命令族的底层共享模块：过滤器构造、未命中/歧义/错误发射、编辑距离与模糊键、
 * 相似名建议、通配匹配、路径排除、唯一目标判定、变更文件路径解析、取址站点。
 *
 * @internal
 * 族内的共用函数全部住在本模块：runs 只导入本模块，entry 只导入 runs，兄弟片
 * 之间没有互相导入的链条。
 */
export module cg.cli.commands.matchers;

import std;
import cg.util;
import cg.db.schema;
import cg.db.store;
import cg.db.query.lookup;
import cg.db.query.members;
import cg.db.query.stats;
import cg.cli.parse.model;
import cg.cli.render.symbol;

export namespace cg::cli
{

    namespace detail
    {

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
            return !t.empty()
                   && cg::util::ascii_lower_char(c)
                          == cg::util::ascii_lower_char(t.front())
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
