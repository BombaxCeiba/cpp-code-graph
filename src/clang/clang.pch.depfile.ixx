

/**
 * @module cg.clang.pch.depfile
 *
 * @brief 依赖清单域：/showIncludes 与 make depfile
 * 解析、闭包装配、列表摘要与生产者摘要。
 */
export module cg.clang.pch.depfile;

import std;
import cg.util;
import cg.util.cstd;
import cg.clang.capi;
import cg.clang.driver;
import cg.clang.compile_db;
import cg.clang.bmi;
import cg.clang.pch.paths;
import cg.clang.pch.sidecar;

export namespace cg::clang
{

    namespace detail
    {

        /// clang-cl `/showIncludes` 的固定行前缀（clang 不随 locale 本地化）。
        constexpr std::string_view SHOW_INCLUDES_PREFIX =
            "Note: including file: ";

        /** @brief 分词缓冲非空即入表并清空。 */
        inline void flush_dep_token(
            std::vector<std::string>& deps,
            std::string&              token)
        {
            if (!token.empty())
            {
                deps.push_back(std::move(token));
            }
            token.clear();
        }

        /**
         * @brief 解析 `/showIncludes` 的 stdout，提取全部被包含文件路径。
         */
        [[nodiscard]]
        inline auto parse_show_includes(const std::string& tool_out)
            -> std::vector<std::string>
        {
            std::vector<std::string> deps;
            std::size_t              at = 0;
            while (at < tool_out.size())
            {
                std::size_t end = tool_out.find('\n', at);
                if (end == std::string::npos)
                {
                    end = tool_out.size();
                }
                std::string_view line{tool_out.data() + at, end - at};
                at = end + 1;
                if (!line.starts_with(SHOW_INCLUDES_PREFIX))
                {
                    continue;
                }
                line.remove_prefix(SHOW_INCLUDES_PREFIX.size());
                while (!line.empty()
                       && (line.back() == '\r' || line.back() == ' '
                           || line.back() == '\t'))
                {
                    line.remove_suffix(1);
                }
                while (!line.empty()
                       && (line.front() == ' ' || line.front() == '\t'))
                {
                    line.remove_prefix(1);
                }
                if (!line.empty())
                {
                    deps.emplace_back(line);
                }
            }
            return deps;
        }

        /**
         * @brief 解析 make 风格依赖文件（`-MD -MF` 产物），提取依赖路径。
         *
         * @internal 规则形如 `out: dep1 dep2 \`（续行）；首条规则冒号前是
         * 产物路径，其后（含续行）是依赖。路径中的空格以 `\ ` 转义。
         * 产物路径可带 Windows 盘符前缀（单字母加冒号），该冒号属于
         * 盘符而非规则分隔符，定位分隔冒号时跳过它。
         */
        [[nodiscard]]
        inline auto parse_make_depfile(const std::string& text)
            -> std::vector<std::string>
        {
            std::vector<std::string> deps;
            std::string              joined;
            std::size_t              at = 0;
            while (at < text.size())
            {
                std::size_t end = text.find('\n', at);
                if (end == std::string::npos)
                {
                    end = text.size();
                }
                std::string_view line{text.data() + at, end - at};
                at = end + 1;
                if (!line.empty() && line.back() == '\r')
                {
                    line.remove_suffix(1);
                }
                const bool continues = !line.empty() && line.back() == '\\';
                if (continues)
                {
                    line.remove_suffix(1);
                }
                joined.append(line);
                if (!continues)
                {
                    joined.push_back('\n');
                }
            }
            const std::size_t      eol = joined.find('\n');
            const std::string_view rule{
                joined.data(),
                eol == std::string::npos ? joined.size() : eol};
            // 产物若以盘符（单 ASCII 字母加冒号）开头，首冒号属于盘符而
            // 非规则分隔符，分隔冒号从盘符之后再找。
            const bool has_drive =
                rule.size() >= 2
                && std::isalpha(static_cast<unsigned char>(rule[0]))
                && rule[1] == ':';
            const std::size_t colon = rule.find(':', has_drive ? 2 : 0);
            if (colon == std::string_view::npos)
            {
                return deps;
            }
            const std::string_view rest = rule.substr(colon + 1);
            std::string            token;
            for (std::size_t i = 0; i < rest.size(); ++i)
            {
                const char ch = rest[i];
                if (ch == '\\' && i + 1 < rest.size()
                    && (rest[i + 1] == ' ' || rest[i + 1] == '\\'))
                {
                    token.push_back(rest[++i]);
                    continue;
                }
                if (ch == ' ' || ch == '\t')
                {
                    flush_dep_token(deps, token);
                    continue;
                }
                token.push_back(ch);
            }
            flush_dep_token(deps, token);
            return deps;
        }

        /** @brief 路径规范化后按大小写折叠键首次入表，连带字节哈希。 */
        inline void push_closure_file(
            std::vector<std::pair<std::string, std::string>>& files,
            std::unordered_set<std::string>&                  seen,
            const std::string&                                raw)
        {
            if (raw.empty())
            {
                return;
            }
            const std::string path = cg::util::norm_path(raw);
            if (!seen.insert(fold(path)).second)
            {
                return;
            }
            files.emplace_back(path, cg::clang::detail::hash_source(path));
        }

        /**
         * @brief 组装缓存闭包：种子（伞头/强制包含/producer 输入）+ 构建发
         *        射的依赖清单；按大小写折叠去重（MSVC 生态不区分头文件名
         *        大小写）后逐文件取字节哈希。
         */
        [[nodiscard]]
        inline auto assemble_closure(
            const std::vector<std::string>& seeds,
            const std::vector<std::string>& deps)
            -> std::vector<std::pair<std::string, std::string>>
        {
            std::vector<std::pair<std::string, std::string>> files;
            std::unordered_set<std::string>                  seen;
            for (const std::string& seed : seeds)
            {
                push_closure_file(files, seen, seed);
            }
            for (const std::string& dep : deps)
            {
                push_closure_file(files, seen, dep);
            }
            std::ranges::sort(
                files,
                [](const auto& a, const auto& b) { return a.first < b.first; });
            return files;
        }

        [[nodiscard]]
        inline auto sha_of_list(
            const std::vector<std::pair<std::string, std::string>>& items)
            -> std::string
        {
            std::string buf;
            for (const auto& [path, hash] : items)
            {
                buf += std::to_string(path.size());
                buf.push_back(':');
                buf += path;
                buf.push_back('\n');
                buf += hash;
                buf.push_back('\n');
            }
            return hash_text(buf);
        }

        [[nodiscard]]
        inline auto producer_digest(const ProducerRec& p) -> std::string
        {
            std::string buf = "pch-producer-v2\n";
            for (const std::string& a : p.argv)
            {
                buf += std::to_string(a.size());
                buf.push_back(':');
                buf += a;
                buf.push_back('\n');
            }
            buf += "dir:" + p.directory + "\n";
            buf += "input:" + p.input + "\n";
            buf += p.cl_mode ? "mode:cl\n" : "mode:gnu\n";
            buf += "dpr:1\n";
            return hash_text(buf);
        }

    } // namespace detail
} // namespace cg::clang
