

/**
 * @module cg.clang.pch.paths
 *
 * @brief PCH
 * 域路径与工具判定基础：路径折叠/绝对性/来源解析、命令行旗标取值、去重压栈、文件与目录原语、cl
 * 编译器判定。
 */
export module cg.clang.pch.paths;

import std;
import cg.util;
import cg.util.cstd;
import cg.clang.capi;
import cg.clang.driver;
import cg.clang.compile_db;
import cg.clang.bmi;

export namespace cg::clang
{

    namespace detail
    {

        [[nodiscard]]
        inline auto fold(std::string_view path) -> std::string
        {
            return cg::util::ascii_lower(cg::util::norm_path(path));
        }

        [[nodiscard]]
        inline auto is_absolute_path(std::string_view path) -> bool
        {
            if (path.size() >= 3 && path[1] == ':'
                && ((path[0] >= 'A' && path[0] <= 'Z')
                    || (path[0] >= 'a' && path[0] <= 'z')))
            {
                return true;
            }
            return path.starts_with('/') || path.starts_with("\\\\");
        }

        [[nodiscard]]
        inline auto resolve_from(std::string_view value, std::string_view dir)
            -> std::string
        {
            if (value.empty())
            {
                return {};
            }
            if (is_absolute_path(value))
            {
                return cg::util::norm_path(value);
            }
            return cg::util::norm_path(
                (std::filesystem::path{std::string{dir}} / std::string{value})
                    .lexically_normal()
                    .generic_string());
        }

        /** @brief 旗标 token 的值段：分写取下一 token，连写剥尾段。 */
        [[nodiscard]]
        inline auto flag_value(
            const std::vector<std::string>& argv,
            std::size_t                     i,
            std::string_view prefix) -> std::optional<std::string>
        {
            const std::string& a = argv[i];
            if (a == prefix && i + 1 < argv.size())
            {
                return argv[i + 1];
            }
            if (a.starts_with(prefix) && a.size() > prefix.size())
            {
                return a.substr(prefix.size());
            }
            return std::nullopt;
        }

        /** @brief 非空目录按序去重追加。 */
        inline void push_nonempty_unique(
            std::vector<std::string>& sink,
            const std::string&        dir)
        {
            if (!dir.empty())
            {
                cg::util::push_unique(sink, dir);
            }
        }

        /** @brief 路径按目录解析为绝对后非空去重追加。 */
        inline void push_resolved_unique(
            std::vector<std::string>& sink,
            std::string_view          directory,
            std::string_view          raw)
        {
            const std::string abs = resolve_from(raw, directory);
            if (!abs.empty())
            {
                cg::util::push_unique(sink, abs);
            }
        }

        [[nodiscard]]
        inline auto is_file(const std::string& path) -> bool
        {
            std::error_code ec;
            return std::filesystem::is_regular_file(
                std::filesystem::path{path},
                ec);
        }

        /**
         * @brief 整文件读入（二进制口径）：util 单点转发的既有名字别名。
         *
         * @internal manifest/sidecar 读取面沿用本文件旧名，实现收编进
         * cg::util::read_text_file（语义注释见彼处）。
         */
        [[nodiscard]]
        inline auto read_text(const std::string& path)
            -> std::optional<std::string>
        {
            return cg::util::read_text_file(path);
        }

        [[nodiscard]]
        inline auto create_dir(const std::string& dir) -> bool
        {
            namespace fs = std::filesystem;
            std::error_code ec;
            if (fs::is_directory(fs::path{dir}, ec))
            {
                return true;
            }
            fs::create_directories(fs::path{dir}, ec);
            if (ec)
            {
                return false;
            }
            cg::util::platform::enable_dir_compression(dir);
            return true;
        }

        /**
         * @brief 编译器基名是否 cl 方言（cl.exe / clang-cl.exe）。
         *
         * @internal 外来 MSVC cdb 的 argv[0] 是 `cl.exe`，不带
         * `--driver-mode=cl`；loader 已从 `/Yc` `/Yu` `/Fp` 推断出方言，故
         * 结构化字段优先，基名判定是记录缺失时的兜底。
         */
        [[nodiscard]]
        inline auto compiler_is_cl(std::string_view compiler) -> bool
        {
            std::string base =
                cg::util::ascii_lower(cg::util::path_basename(compiler));
            if (base.ends_with(".exe"))
            {
                base.resize(base.size() - 4);
            }
            return base == "cl" || base == "clang-cl";
        }

        [[nodiscard]]
        inline auto is_cl_command(const CompileCommand& cc) -> bool
        {
            return cc.cl_mode
                   || std::ranges::find(cc.argv, "--driver-mode=cl")
                          != cc.argv.end()
                   || compiler_is_cl(cc.compiler);
        }

    } // namespace detail
} // namespace cg::clang
