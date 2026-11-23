module;
#include "../db/db.tables.h"
#include "../msbuild/capture.tables.h"

/**
 * @module cg.cli.commands.matchers
 *
 * @brief
 * 查询匹配域：过滤器构造、未命中/歧义/错误发射、编辑距离与模糊键、通配匹配、路径排除、取址站点。
 */
export module cg.cli.commands.matchers;

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
} // namespace cg::cli
