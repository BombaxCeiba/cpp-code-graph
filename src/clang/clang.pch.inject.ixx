

/**
 * @module cg.clang.pch.inject
 *
 * @brief 命令行改写域：引擎身份指纹、/Yc 与 include-pch 剥除、影子 PCH 注入。
 */
export module cg.clang.pch.inject;

import std;
import cg.util;
import cg.util.cstd;
import cg.clang.capi;
import cg.clang.driver;
import cg.clang.compile_db;
import cg.clang.bmi;
import cg.clang.pch.paths;
import cg.clang.pch.depfile;

export namespace cg::clang
{

    namespace detail
    {

        /** @brief 引擎身份：版本串、驱动器字节标记与身份键。 */
        struct EngineIdentity
        {
            std::string version; ///< libclang 版本（探询失败为空）
            std::string driver;  ///< 驱动器路径
            std::string stat;    ///< 驱动器 "size:mtime"（stat 失败为空）
            std::string key;     ///< 三元组身份哈希的前 16 字符
        };

        [[nodiscard]]
        inline auto engine_identity() -> EngineIdentity
        {
            const DriverInfo& info = driver_info();
            std::string       version;
            try
            {
                version = libclang_version();
            }
            catch (const std::exception&)
            {
                version.clear();
            }
            const DriverInfo& same = info;
            const std::string driver = same.driver;
            std::string       stat;
            if (!driver.empty())
            {
                const cg::clang::detail::BmiStat st =
                    cg::clang::detail::stat_source(driver);
                if (st.ok)
                {
                    stat = std::to_string(st.size) + ":"
                           + std::to_string(st.mtime);
                }
            }
            EngineIdentity out;
            out.version = std::move(version);
            out.driver = driver;
            out.stat = std::move(stat);
            out.key = hash_text(
                          "version=" + out.version + "\ndriver=" + driver
                          + "\nstat=" + out.stat)
                          .substr(0, 16);
            return out;
        }

        /** @brief 去掉 cl producer argv 的 `/Yc`（解析 producer TU 时用）。 */
        [[nodiscard]]
        inline auto strip_yc(std::vector<std::string> argv)
            -> std::vector<std::string>
        {
            std::vector<std::string> out;
            for (std::size_t i = 0; i < argv.size(); ++i)
            {
                const std::string& a = argv[i];
                if (a == "/Yc")
                {
                    if (cg::clang::detail::cl_header_value_follows(argv, i))
                    {
                        ++i;
                    }
                    continue;
                }
                if (a.starts_with("/Yc") && a.size() > 3)
                {
                    continue;
                }
                out.push_back(a);
            }
            return out;
        }

        /** @brief 从 argv 删除 `-Xclang -include-pch -Xclang V` 三元组。 */
        [[nodiscard]]
        inline auto strip_include_pch(std::vector<std::string> argv)
            -> std::vector<std::string>
        {
            std::vector<std::string> out;
            for (std::size_t i = 0; i < argv.size();)
            {
                if (argv[i] == "-Xclang" && i + 1 < argv.size()
                    && argv[i + 1] == "-include-pch")
                {
                    i += 2;
                    if (i < argv.size() && argv[i] == "-Xclang"
                        && i + 1 < argv.size())
                    {
                        i += 2;
                    }
                    else if (
                        i < argv.size()
                        && !cg::clang::detail::is_gnu_flag_token(argv[i]))
                    {
                        ++i;
                    }
                    continue;
                }
                out.push_back(argv[i]);
                ++i;
            }
            return out;
        }

        /** @brief 影子 PCH 的 "-Xclang -include-pch -Xclang" 注入三元组。 */
        [[nodiscard]]
        inline auto include_pch_tokens(const std::string& shadow)
            -> std::vector<std::string>
        {
            return {"-Xclang", "-include-pch", "-Xclang", shadow};
        }

        /** @brief 把影子 PCH 注入消费者 argv（保留其余旗标与顺序）。 */
        [[nodiscard]]
        inline auto inject_shadow(
            std::vector<std::string> argv,
            const std::string&       shadow) -> std::vector<std::string>
        {
            for (std::size_t i = 0; i + 1 < argv.size(); ++i)
            {
                if (argv[i] == "-Xclang" && argv[i + 1] == "-include-pch")
                {
                    std::vector<std::string> out;
                    out.insert(
                        out.end(),
                        argv.begin(),
                        argv.begin() + static_cast<std::ptrdiff_t>(i));
                    const std::vector<std::string> t =
                        include_pch_tokens(shadow);
                    out.insert(out.end(), t.begin(), t.end());
                    std::size_t j = i + 2;
                    if (j < argv.size() && argv[j] == "-Xclang"
                        && j + 1 < argv.size())
                    {
                        j += 2;
                    }
                    else if (
                        j < argv.size()
                        && !cg::clang::detail::is_gnu_flag_token(argv[j]))
                    {
                        ++j;
                    }
                    out.insert(
                        out.end(),
                        argv.begin() + static_cast<std::ptrdiff_t>(j),
                        argv.end());
                    return out;
                }
            }
            std::vector<std::string>       out = std::move(argv);
            const std::vector<std::string> t = include_pch_tokens(shadow);
            out.insert(out.end(), t.begin(), t.end());
            return out;
        }

    } // namespace detail
} // namespace cg::clang
