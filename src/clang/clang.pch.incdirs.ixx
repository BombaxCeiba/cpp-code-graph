

/**
 * @module cg.clang.pch.incdirs
 *
 * @brief include 目录族：按类目分组、资源目录、自足 cl 判定、CPATH/INCLUDE
 * 环境切分、强制 include。
 */
export module cg.clang.pch.incdirs;

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

        /**
         * @brief 把 argv 的 include 目录拆成“命令行”与 `-imsvc` 两类。
         *
         * @internal cl 方言的真实搜索顺序是
         * `/I → CPATH → resource dir → -imsvc → INCLUDE`；闭包必须按这个
         * 顺序构造，否则 resource dir（stddef.h/stdint.h/vadefs.h）会被
         * `-imsvc` 的同名假头遮蔽，得到错误的闭包哈希（已证的陈旧影子复用
         * 入口）。两类内部保持 argv 顺序并各自去重。
         */
        struct IncludeDirsByKind
        {
            std::vector<std::string> normal; // -I/-isystem//I//external:I …
            std::vector<std::string> imsvc;  // -imsvc（MSBuild INCLUDE 物化）
        };

        [[nodiscard]]
        inline auto include_dirs_by_kind(
            const std::vector<std::string>& argv,
            const std::string&              directory) -> IncludeDirsByKind
        {
            IncludeDirsByKind out;
            for (std::size_t i = 0; i < argv.size(); ++i)
            {
                if (const auto v = flag_value(argv, i, "-imsvc"))
                {
                    push_nonempty_unique(
                        out.imsvc,
                        resolve_from(*v, directory));
                    continue;
                }
                for (const std::string_view prefix :
                     {"-I", "-isystem", "-iquote", "-idirafter"})
                {
                    if (const auto v = flag_value(argv, i, prefix))
                    {
                        push_nonempty_unique(
                            out.normal,
                            resolve_from(*v, directory));
                    }
                }
                for (const std::string_view prefix : {"/I", "/external:I"})
                {
                    if (const auto v = flag_value(argv, i, prefix))
                    {
                        push_nonempty_unique(
                            out.normal,
                            resolve_from(*v, directory));
                    }
                }
            }
            return out;
        }

        /** @brief clang resource include
         * 目录（`-print-resource-dir`/include）。 */
        [[nodiscard]]
        inline auto resource_include_dir() -> std::string
        {
            const DriverInfo& info = driver_info();
            if (info.resource_dir.empty())
            {
                return {};
            }
            return cg::util::norm_path(
                (std::filesystem::path{info.resource_dir} / "include")
                    .lexically_normal()
                    .generic_string());
        }

        /**
         * @brief argv 是否自包含系统头（cl 方言 + `/X`）。
         *
         * @internal ccg 生成的 cdb 是 `/X` + 全量 `-imsvc`；此形态下
         * clang-cl 不读进程 INCLUDE/CPATH、也不做工具集自动探测，闭包只能
         * 用 argv 目录 + resource dir。`/x`、`-X`、`-x` 与 `/X` 同义——判定
         * 复用 cg.clang.compile_db 的同一助手。
         */
        [[nodiscard]]
        inline auto is_self_contained_cl(const std::vector<std::string>& argv)
            -> bool
        {
            return std::ranges::any_of(
                argv,
                cg::clang::detail::is_ignore_standard_include);
        }

        /** @brief 拆分环境变量里的目录列表（按平台路径分隔符）。 */
        [[nodiscard]]
        inline auto split_include_path_env(
            const std::optional<std::string>& raw) -> std::vector<std::string>
        {
            std::vector<std::string> out;
            if (!raw.has_value())
            {
                return out;
            }
#ifdef _WIN32
            constexpr char separator = ';';
#else
            constexpr char separator = ':';
#endif
            std::string_view text{*raw};
            while (!text.empty())
            {
                const std::size_t      pos = text.find(separator);
                const std::string_view item =
                    pos == std::string_view::npos ? text : text.substr(0, pos);
                if (!item.empty())
                {
                    std::string norm = cg::util::norm_path(item);
                    if (!norm.empty())
                    {
                        cg::util::push_unique(out, std::move(norm));
                    }
                }
                if (pos == std::string_view::npos)
                {
                    break;
                }
                text.remove_prefix(pos + 1);
            }
            return out;
        }

        /**
         * @brief CPATH 与语言相关环境变量贡献的目录（不含 cl 的 INCLUDE）。
         *
         * @internal cl 真实顺序里 CPATH 在 resource dir 之前，而 INCLUDE 在
         * `-imsvc` 之后；两者必须分开插入，不能合并成一个列表。
         */
        [[nodiscard]]
        inline auto cpath_include_dirs(bool cxx, std::string_view directory)
            -> std::vector<std::string>
        {
            std::vector<std::string> out;
            // CPATH 是语言中立的；CPLUS_INCLUDE_PATH / C_INCLUDE_PATH 只对
            // 对应语言生效（实测：C++ TU 不搜索 C_INCLUDE_PATH）。不裁剪会
            // 让闭包哈希一个真实编译根本不会读的同名头。
            for (const std::string& dir :
                 split_include_path_env(cg::util::cstd::get_env("CPATH")))
            {
                push_resolved_unique(out, directory, dir);
            }
            for (const std::string& dir : split_include_path_env(
                     cg::util::cstd::get_env(
                         cxx ? "CPLUS_INCLUDE_PATH" : "C_INCLUDE_PATH")))
            {
                push_resolved_unique(out, directory, dir);
            }
            return out;
        }

        /** @brief 进程 INCLUDE 环境贡献的目录（cl 方言）。 */
        [[nodiscard]]
        inline auto include_env_dirs(std::string_view directory)
            -> std::vector<std::string>
        {
            std::vector<std::string> out;
            for (const std::string& dir :
                 split_include_path_env(cg::util::cstd::get_env("INCLUDE")))
            {
                const std::string abs = resolve_from(dir, directory);
                if (!abs.empty())
                {
                    cg::util::push_unique(out, abs);
                }
            }
            return out;
        }

        /**
         * @brief 从记录的 argv 收集强制包含文件（已按记录目录绝对化）。
         *
         * @internal PCH 构建输入的一部分：`/FI`、`/FI:`、`-include` 与
         * `-Xclang -include -Xclang V` 都必须进入闭包，否则被强制包含的头
         * 修改后缓存键不变，影子会无限期陈旧复用。
         */
        [[nodiscard]]
        inline auto forced_includes_of(
            const std::vector<std::string>& argv,
            const std::string& directory) -> std::vector<std::string>
        {
            std::vector<std::string> out;
            for (std::size_t i = 0; i < argv.size(); ++i)
            {
                const std::string& a = argv[i];
                // -include / -imacros 的两级转发形态：`-Xclang `<flag>` -Xclang
                // V`。
                if (a == "-Xclang" && i + 3 < argv.size()
                    && (argv[i + 1] == "-include" || argv[i + 1] == "-imacros")
                    && argv[i + 2] == "-Xclang")
                {
                    push_resolved_unique(out, directory, argv[i + 3]);
                    i += 3;
                    continue;
                }
                if ((a == "-include" || a == "-imacros")
                    && cg::clang::detail::gnu_value_follows(argv, i))
                {
                    push_resolved_unique(out, directory, argv[i + 1]);
                    ++i;
                    continue;
                }
                if (a.starts_with("-imacros="))
                {
                    push_resolved_unique(
                        out,
                        directory,
                        a.substr(std::string_view{"-imacros="}.size()));
                    continue;
                }
                if (a == "-FI" && i + 1 < argv.size())
                {
                    push_resolved_unique(out, directory, argv[i + 1]);
                    ++i;
                    continue;
                }
                if (a == "/FI" && cg::clang::detail::cl_value_follows(argv, i))
                {
                    push_resolved_unique(out, directory, argv[i + 1]);
                    ++i;
                    continue;
                }
                if (a.starts_with("/FI") && a.size() > 3)
                {
                    std::string_view value{a.data() + 3, a.size() - 3};
                    if (!value.empty() && value.front() == ':')
                    {
                        value.remove_prefix(1);
                    }
                    push_resolved_unique(out, directory, value);
                    continue;
                }
            }
            return out;
        }

        /**
         * @brief 缓存闭包（依赖集）的来源与组装。
         *
         * @internal 闭包不再由文本扫描收集——文本扫描对 `#include MACRO`、
         * `#include_next` 与搜索路径模型有固有盲区（MSVC 生态不区分头文件
         * 名大小写、宏拼头名都是常态）。现在的来源是影子构建本身：构建
         * clang 进程的同一遍预处理顺带发射依赖清单（cl 方言走
         * `/showIncludes` 的 stdout，gnu 方言走 `-MD -MF` 依赖文件），宏
         * 展开、搜索路径、大小写由真编译器按构建语义消解。缓存复用条件 =
         * 台账闭包逐文件字节重哈希全部一致（读不到即失配）。
         */

    } // namespace detail
} // namespace cg::clang
