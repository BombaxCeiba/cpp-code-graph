/**
 * @module cg.clang.pch
 *
 * @brief 构建系统记录的 PCH 的“影子 PCH”规划：发现 producer/consumer、
 * 复刻 producer argv（同源驱动器 + DetailedPreprocessingRecord）构建
 * 影子缓存、按闭包哈希失效、把消费者改写为消费影子。
 *
 * @internal
 * 只有构建系统自己已经在用 PCH 的工程才进入本模块；无 PCH 标记的
 * 条目逐字节原样返回（零变化门禁）。生产命令只认构建系统记录：
 * CMake/GNU 的 `-x c++-header` + `-emit-pch`/`.pch` 条目，或 MSBuild
 * sidecar `pch.json` 的 `/Yc` 调用。没有记录就文本回退——绝不拿消费者
 * 伞头合成 recipe。
 */
export module cg.clang.pch;

import std;
import cg.util;
import cg.util.cstd;
import cg.clang.capi;
import cg.clang.driver;
import cg.clang.compile_db;
import cg.clang.bmi;

export namespace cg::clang
{

    /**
     * @brief PCH 影子规划旋钮。
     */
    struct PchOptions
    {
        bool        replay_degraded = false; // CCG_PCH_MODE=replay-degraded
        bool        force = false;           // --force/--full：重建影子
        bool        dry_run = false;         // 只计划、不构建
        bool        purge = false;           // 仅全量索引允许清理孤儿
        std::string ccg_config_dir;          // <folder>/<config>/
        std::string compile_db_path; // 全量 cdb（发现 producer 的兜底来源）
        std::string sidecar_path;    // MSBuild pch.json（可选）
    };

    /**
     * @brief 一次规划的结果。
     */
    struct PchPlanResult
    {
        std::vector<CompileCommand> commands; // 已重写；GNU producer 已剔除
        std::vector<std::string>    notes;
        /** @brief 逐 TU 的可读判定（"`<file>`: `<note>`"），供摘要/JSON 消费。
         */
        std::vector<std::string> tu_notes;
        std::size_t              built = 0;
        std::size_t              reused = 0;
        /** @brief 文本回退的消费者数（不含 replay-degraded）。 */
        std::size_t text_fallback = 0;
        /** @brief 显式 opt-in 消费记录 PCH（非 DPR，图不完整）的消费者数。 */
        std::size_t replay_degraded = 0;
        std::size_t unusable = 0;
        std::size_t producers_skipped = 0;
        std::size_t consumers = 0;
        /** @brief 从兄弟配置 donor 复制（未本地重建）的影子数。 */
        std::size_t donated = 0;
        std::string cache_dir;
    };

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

        /** @brief sidecar 里的一台 producer（MSBuild `/Yc` 调用）。 */
        struct SidecarProducer
        {
            std::string              pch_key;
            std::string              source;
            std::string              umbrella;
            std::string              umbrella_path;
            std::string              cwd;
            std::vector<std::string> argv;
        };

        struct SidecarConsumer
        {
            std::string file;
            std::string pch_key;
            std::string pch_header;
        };

        struct Sidecar
        {
            std::vector<SidecarProducer> producers;
            std::vector<SidecarConsumer> consumers;
        };

        /**
         * @brief 读取并校验 MSBuild sidecar。
         *
         * @internal `cdb_sha256` 不符 → 忽略 sidecar、纯文本回退（cdb 被重新
         * 生成而 sidecar 落后时，绝不让两份世界观混用）。
         */
        [[nodiscard]]
        inline auto load_sidecar(
            const std::string& path,
            const std::string& cdb_path) -> std::optional<Sidecar>
        {
            namespace json = cg::util::json;
            const std::optional<std::string> text = read_text(path);
            if (!text)
            {
                return std::nullopt;
            }
            try
            {
                const json::Document  doc = json::Document::parse(*text);
                const json::ValueView root = doc.root();
                if (!root.is_object())
                {
                    return std::nullopt;
                }
                if (const auto v = root.get("version");
                    v && v->is_number() && *v->as_int() != 1)
                {
                    return std::nullopt;
                }
                std::string expect;
                if (const auto v = root.get("cdb_sha256"); v && v->is_string())
                {
                    expect = std::string{*v->as_string()};
                }
                std::string actual;
                try
                {
                    actual = cg::util::sha256_file_hex(cdb_path);
                }
                catch (const std::exception&)
                {
                    return std::nullopt;
                }
                if (expect.empty() || expect != actual)
                {
                    return std::nullopt;
                }
                Sidecar                              out;
                const std::optional<json::ValueView> producers =
                    root.get("producers");
                if (producers && producers->is_array())
                {
                    for (std::size_t i = 0; i < producers->size(); ++i)
                    {
                        const auto e = producers->at(i);
                        if (!e || !e->is_object())
                        {
                            continue;
                        }
                        SidecarProducer p;
                        const auto      str = [&e](std::string_view k)
                        {
                            const auto v = e->get(k);
                            return v && v->is_string()
                                       ? std::string{*v->as_string()}
                                       : std::string{};
                        };
                        p.pch_key = cg::util::norm_path(str("pch_key"));
                        p.source = cg::util::norm_path(str("source"));
                        p.umbrella = str("umbrella");
                        p.umbrella_path =
                            cg::util::norm_path(str("umbrella_path"));
                        p.cwd = cg::util::norm_path(str("cwd"));
                        if (const auto av = e->get("argv");
                            av && av->is_array())
                        {
                            for (std::size_t k = 0; k < av->size(); ++k)
                            {
                                if (const auto x = av->at(k);
                                    x && x->is_string())
                                {
                                    p.argv.emplace_back(*x->as_string());
                                }
                            }
                        }
                        if (!p.pch_key.empty() && !p.source.empty())
                        {
                            out.producers.push_back(std::move(p));
                        }
                    }
                }
                const std::optional<json::ValueView> consumers =
                    root.get("consumers");
                if (consumers && consumers->is_array())
                {
                    for (std::size_t i = 0; i < consumers->size(); ++i)
                    {
                        const auto e = consumers->at(i);
                        if (!e || !e->is_object())
                        {
                            continue;
                        }
                        SidecarConsumer c;
                        const auto      str = [&e](std::string_view k)
                        {
                            const auto v = e->get(k);
                            return v && v->is_string()
                                       ? std::string{*v->as_string()}
                                       : std::string{};
                        };
                        c.file = cg::util::norm_path(str("file"));
                        c.pch_key = cg::util::norm_path(str("pch_key"));
                        c.pch_header = str("pch_header");
                        if (!c.file.empty() && !c.pch_key.empty())
                        {
                            out.consumers.push_back(std::move(c));
                        }
                    }
                }
                return out;
            }
            catch (const std::exception&)
            {
                return std::nullopt;
            }
        }

        /** @brief 一条已发现的生产记录（GNU cdb 或 MSBuild sidecar）。 */
        struct ProducerRec
        {
            std::string              key; // fold(绝对 PCH 路径 / pch_key)
            std::string              display_key; // 原始绝对路径
            std::string              input;       // producer TU（绝对）
            std::string              directory;   // 记录的构建目录
            std::string              umbrella;    // 伞头拼写（诊断/闭包种子）
            std::vector<std::string> forced;      // 记录的强制包含（绝对）
            std::vector<std::string>
                 argv; // 重放 argv（无 argv[0]、无输出旗标）
            bool cl_mode = false;
            bool cxx = true; // producer TU 的语言（影响搜索目录与环境变量）
            bool from_sidecar = false;
        };

        /**
         * @brief producer TU 是否为 C++。
         *
         * @internal 优先级：显式 `-x`/`/Tc`/`/Tp` > 输入文件扩展名 > 默认 C++。
         * 语言决定使用哪条驱动器搜索链（C++ vs C）以及哪个环境变量
         * （CPLUS_INCLUDE_PATH vs C_INCLUDE_PATH）。
         */
        [[nodiscard]]
        inline auto producer_is_cxx(
            const std::vector<std::string>& argv,
            const std::string&              input) -> bool
        {
            for (std::size_t i = 0; i < argv.size(); ++i)
            {
                const std::string& a = argv[i];
                if (a == "-x" && i + 1 < argv.size())
                {
                    const std::string& lang = argv[i + 1];
                    if (lang == "c" || lang == "c-header"
                        || lang == "cpp-output")
                    {
                        return false;
                    }
                    if (lang == "c++" || lang == "c++-header")
                    {
                        return true;
                    }
                }
                if (a == "/Tc" || a.starts_with("/Tc"))
                {
                    return false;
                }
                if (a == "/Tp" || a.starts_with("/Tp"))
                {
                    return true;
                }
            }
            const std::string ext = cg::util::ascii_lower(
                std::filesystem::path{input}.extension().string());
            return !(ext == ".c" || ext == ".i");
        }

        /** @brief 从 argv 收集 include 搜索目录（已绝对化）。 */
        [[nodiscard]]
        inline auto include_dirs_of(
            const std::vector<std::string>& argv,
            const std::string& directory) -> std::vector<std::string>
        {
            std::vector<std::string> out;
            const auto               add = [&out](const std::string& dir)
            {
                if (!dir.empty())
                {
                    cg::util::push_unique(out, dir);
                }
            };
            for (std::size_t i = 0; i < argv.size(); ++i)
            {
                const std::string& a = argv[i];
                const auto         value =
                    [&](std::string_view prefix) -> std::optional<std::string>
                {
                    if (a == prefix && i + 1 < argv.size())
                    {
                        return argv[i + 1];
                    }
                    if (a.starts_with(prefix) && a.size() > prefix.size())
                    {
                        return a.substr(prefix.size());
                    }
                    return std::nullopt;
                };
                for (const std::string_view prefix :
                     {"-I", "-isystem", "-iquote", "-idirafter", "-imsvc"})
                {
                    if (const auto v = value(prefix))
                    {
                        add(resolve_from(*v, directory));
                    }
                }
                for (const std::string_view prefix : {"/I", "/external:I"})
                {
                    if (const auto v = value(prefix))
                    {
                        add(resolve_from(*v, directory));
                    }
                }
            }
            return out;
        }

        /** @brief 按顺序合并目录并去重。 */
        inline void append_include_dirs(
            std::vector<std::string>&       out,
            const std::vector<std::string>& dirs)
        {
            for (const std::string& dir : dirs)
            {
                if (!dir.empty())
                {
                    cg::util::push_unique(out, dir);
                }
            }
        }

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
            const auto        add =
                [](std::vector<std::string>& sink, const std::string& dir)
            {
                if (!dir.empty())
                {
                    cg::util::push_unique(sink, dir);
                }
            };
            for (std::size_t i = 0; i < argv.size(); ++i)
            {
                const std::string& a = argv[i];
                const auto         value =
                    [&](std::string_view prefix) -> std::optional<std::string>
                {
                    if (a == prefix && i + 1 < argv.size())
                    {
                        return argv[i + 1];
                    }
                    if (a.starts_with(prefix) && a.size() > prefix.size())
                    {
                        return a.substr(prefix.size());
                    }
                    return std::nullopt;
                };
                if (const auto v = value("-imsvc"))
                {
                    add(out.imsvc, resolve_from(*v, directory));
                    continue;
                }
                for (const std::string_view prefix :
                     {"-I", "-isystem", "-iquote", "-idirafter"})
                {
                    if (const auto v = value(prefix))
                    {
                        add(out.normal, resolve_from(*v, directory));
                    }
                }
                for (const std::string_view prefix : {"/I", "/external:I"})
                {
                    if (const auto v = value(prefix))
                    {
                        add(out.normal, resolve_from(*v, directory));
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
            const auto               add =
                [&out, directory](const std::vector<std::string>& dirs)
            {
                for (const std::string& dir : dirs)
                {
                    const std::string abs = resolve_from(dir, directory);
                    if (!abs.empty())
                    {
                        cg::util::push_unique(out, abs);
                    }
                }
            };
            // CPATH 是语言中立的；CPLUS_INCLUDE_PATH / C_INCLUDE_PATH 只对
            // 对应语言生效（实测：C++ TU 不搜索 C_INCLUDE_PATH）。不裁剪会
            // 让闭包哈希一个真实编译根本不会读的同名头。
            add(split_include_path_env(cg::util::cstd::get_env("CPATH")));
            add(split_include_path_env(
                cg::util::cstd::get_env(
                    cxx ? "CPLUS_INCLUDE_PATH" : "C_INCLUDE_PATH")));
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
            const auto add = [&out, &directory](std::string_view raw)
            {
                if (raw.empty())
                {
                    return;
                }
                const std::string abs = resolve_from(raw, directory);
                if (!abs.empty())
                {
                    cg::util::push_unique(out, abs);
                }
            };
            for (std::size_t i = 0; i < argv.size(); ++i)
            {
                const std::string& a = argv[i];
                // -include / -imacros 的两级转发形态：`-Xclang `<flag>` -Xclang
                // V`。
                if (a == "-Xclang" && i + 3 < argv.size()
                    && (argv[i + 1] == "-include" || argv[i + 1] == "-imacros")
                    && argv[i + 2] == "-Xclang")
                {
                    add(argv[i + 3]);
                    i += 3;
                    continue;
                }
                if ((a == "-include" || a == "-imacros")
                    && cg::clang::detail::gnu_value_follows(argv, i))
                {
                    add(argv[i + 1]);
                    ++i;
                    continue;
                }
                if (a.starts_with("-imacros="))
                {
                    add(a.substr(std::string_view{"-imacros="}.size()));
                    continue;
                }
                if (a == "-FI" && i + 1 < argv.size())
                {
                    add(argv[i + 1]);
                    ++i;
                    continue;
                }
                if (a == "/FI" && cg::clang::detail::cl_value_follows(argv, i))
                {
                    add(argv[i + 1]);
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
                    add(value);
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

        /// clang-cl `/showIncludes` 的固定行前缀（clang 不随 locale 本地化）。
        constexpr std::string_view SHOW_INCLUDES_PREFIX =
            "Note: including file: ";

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
            const auto             flush = [&]()
            {
                if (!token.empty())
                {
                    deps.push_back(std::move(token));
                }
                token.clear();
            };
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
                    flush();
                    continue;
                }
                token.push_back(ch);
            }
            flush();
            return deps;
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
            const auto push = [&](const std::string& raw)
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
            };
            for (const std::string& seed : seeds)
            {
                push(seed);
            }
            for (const std::string& dep : deps)
            {
                push(dep);
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

        [[nodiscard]]
        inline auto engine_identity(std::string& engine_key) -> std::string
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
            engine_key = hash_text(
                             "version=" + version + "\ndriver=" + driver
                             + "\nstat=" + stat)
                             .substr(0, 16);
            return version + "\n" + driver + "\n" + stat;
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

        /** @brief 把影子 PCH 注入消费者 argv（保留其余旗标与顺序）。 */
        [[nodiscard]]
        inline auto inject_shadow(
            std::vector<std::string> argv,
            const std::string&       shadow) -> std::vector<std::string>
        {
            const auto triple = [&shadow]()
            {
                return std::vector<std::string>{
                    "-Xclang",
                    "-include-pch",
                    "-Xclang",
                    shadow};
            };
            for (std::size_t i = 0; i + 1 < argv.size(); ++i)
            {
                if (argv[i] == "-Xclang" && argv[i + 1] == "-include-pch")
                {
                    std::vector<std::string> out;
                    out.insert(
                        out.end(),
                        argv.begin(),
                        argv.begin() + static_cast<std::ptrdiff_t>(i));
                    const std::vector<std::string> t = triple();
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
            const std::vector<std::string> t = triple();
            out.insert(out.end(), t.begin(), t.end());
            return out;
        }

        /** @brief 清单条目。 */
        struct PchEntry
        {
            std::string                                      slot_key;
            std::string                                      pch_key;
            std::string                                      producer_file;
            std::string                                      producer_input;
            std::string                                      producer_cwd;
            std::string                                      producer_digest;
            std::string                                      dialect;
            std::string                                      closure_hash;
            std::vector<std::pair<std::string, std::string>> closure;
            /** @brief 从兄弟配置复制时的 donor 影子绝对路径（本地构建为空）。
             */
            std::string  donor;
            std::int64_t pch_size = 0;
            /** @brief 影子字节的 sha256：同路径内容替换必须失配（内容
             * 面有效性判据）。 */
            std::string pch_hash;
        };

        struct PchManifest
        {
            std::string           engine;
            std::string           engine_key;
            std::vector<PchEntry> entries;
        };

        [[nodiscard]]
        inline auto pch_manifest_to_json(const PchManifest& m) -> std::string
        {
            namespace json = cg::util::json;
            json::Object root;
            root.set("engine", m.engine);
            root.set("engine_key", m.engine_key);
            json::Array entries;
            for (const PchEntry& e : m.entries)
            {
                json::Object o;
                o.set("slot_key", e.slot_key);
                o.set("pch_key", e.pch_key);
                o.set("producer_file", e.producer_file);
                o.set("producer_input", e.producer_input);
                o.set("producer_cwd", e.producer_cwd);
                o.set("producer_digest", e.producer_digest);
                o.set("dialect", e.dialect);
                o.set("closure_hash", e.closure_hash);
                o.set("donor", e.donor);
                o.set("pch_size", e.pch_size);
                o.set("pch_hash", e.pch_hash);
                json::Array closure;
                for (const auto& [path, hash] : e.closure)
                {
                    json::Object c;
                    c.set("path", path);
                    c.set("hash", hash);
                    closure.add(std::move(c));
                }
                o.set("closure", std::move(closure));
                entries.add(std::move(o));
            }
            root.set("entries", std::move(entries));
            return json::stringify(std::move(root));
        }

        [[nodiscard]]
        inline auto pch_manifest_from_json(const std::string& text)
            -> std::optional<PchManifest>
        {
            namespace json = cg::util::json;
            try
            {
                const json::Document  doc = json::Document::parse(text);
                const json::ValueView root = doc.root();
                if (!root.is_object())
                {
                    return std::nullopt;
                }
                PchManifest out;
                const auto  str = [&root](std::string_view key)
                {
                    const auto v = root.get(key);
                    return v && v->is_string() ? std::string{*v->as_string()}
                                               : std::string{};
                };
                out.engine = str("engine");
                out.engine_key = str("engine_key");
                const auto entries = root.get("entries");
                if (!entries || !entries->is_array())
                {
                    return std::nullopt;
                }
                for (std::size_t i = 0; i < entries->size(); ++i)
                {
                    const auto e = entries->at(i);
                    if (!e || !e->is_object())
                    {
                        continue;
                    }
                    PchEntry   entry;
                    const auto estr = [&e](std::string_view key)
                    {
                        const auto v = e->get(key);
                        return v && v->is_string()
                                   ? std::string{*v->as_string()}
                                   : std::string{};
                    };
                    entry.slot_key = estr("slot_key");
                    entry.pch_key = estr("pch_key");
                    entry.producer_file = estr("producer_file");
                    entry.producer_input = estr("producer_input");
                    entry.producer_cwd = estr("producer_cwd");
                    entry.producer_digest = estr("producer_digest");
                    entry.dialect = estr("dialect");
                    entry.closure_hash = estr("closure_hash");
                    entry.donor = estr("donor");
                    entry.pch_hash = estr("pch_hash");
                    if (const auto v = e->get("pch_size"); v && v->is_number())
                    {
                        entry.pch_size = *v->as_int();
                    }
                    if (const auto closure = e->get("closure");
                        closure && closure->is_array())
                    {
                        for (std::size_t k = 0; k < closure->size(); ++k)
                        {
                            const auto c = closure->at(k);
                            if (!c || !c->is_object())
                            {
                                continue;
                            }
                            const auto p = c->get("path");
                            const auto h = c->get("hash");
                            if (p && p->is_string() && h && h->is_string())
                            {
                                entry.closure.emplace_back(
                                    *p->as_string(),
                                    *h->as_string());
                            }
                        }
                    }
                    if (!entry.slot_key.empty() && !entry.pch_key.empty())
                    {
                        out.entries.push_back(std::move(entry));
                    }
                }
                return out;
            }
            catch (const std::exception&)
            {
                return std::nullopt;
            }
        }

        /**
         * @brief 影子构建的重放 argv。
         *
         * @internal
         * 与记录 argv 的最小偏离：输出旗标全部重定向（`/Fo`、`/Fd` 入 slot
         * 临时目录）、构建副作用旗（`-MD/-MMD/-MF`、`-MJ`、
         * `--serialize-diagnostics`、`/sourceDependencies`、`-ftime-trace`、
         * `/showIncludes`、`/MP`、`/Gm`、`/GL`、`/analyze`、`/WX`）剥离、追加
         * `-Xclang -detailed-preprocessing-record`。其余旗标逐字保留。影子构建
         * 必须只读：任何会把文件写进构建树的记录旗标都不得原样重放。
         */
        [[nodiscard]]
        inline auto replay_argv(
            const ProducerRec& p,
            const std::string& shadow,
            const std::string& slot_dir) -> std::vector<std::string>
        {
            std::vector<std::string> argv;
            argv.push_back(driver_info().driver);
            if (p.cl_mode)
            {
                argv.emplace_back("--driver-mode=cl");
            }
            // cl 的 `/Yc` producer 是普通编译命令上的旗标；loader 在 PCH 感知
            // 解析中去掉 `/c`，重放时必须补回，否则 clang-cl 会走编译+链接。
            if (p.cl_mode)
            {
                const bool has_c =
                    std::ranges::find(p.argv, "/c") != p.argv.end()
                    || std::ranges::find(p.argv, "-c") != p.argv.end();
                if (!has_c)
                {
                    argv.emplace_back("/c");
                }
            }
            for (std::size_t i = 0; i < p.argv.size(); ++i)
            {
                const std::string& a = p.argv[i];
                // loader 可能已把 cl 方言写进 argv；重放只在最前面放一次。
                if (a == "--driver-mode=cl")
                {
                    continue;
                }
                if (a == "-MD" || a == "-MMD" || a == "/showIncludes"
                    || a == "/MP" || a == "/Gm" || a == "/GL" || a == "/analyze"
                    || a == "/WX" || a == "-ftime-trace"
                    || a.starts_with("-ftime-trace=")
                    || a.starts_with("-Werror"))
                {
                    continue;
                }
                if (a == "-MF")
                {
                    ++i;
                    continue;
                }
                // 预处理器/汇编/调试转储：都会在记录目录里写文件。剥离掉对
                // AST 形状没有影响（影子只需要 PCH 字节）。
                if (a == "-save-temps" || a.starts_with("-save-temps=")
                    || a == "--save-temps" || a.starts_with("--save-temps=")
                    || a.starts_with("-fdump-")
                    || a == "-fsave-optimization-record")
                {
                    continue;
                }
                if (a == "-foptimization-record-file")
                {
                    ++i;
                    continue;
                }
                if (a.starts_with("-foptimization-record-file="))
                {
                    continue;
                }
                if (a == "/P" || a == "/EP" || a == "/E" || a == "/FA")
                {
                    continue;
                }
                if (a == "/Fi" || a == "/Fa" || a == "/Fe" || a == "/Fm")
                {
                    // 分写值只有在下一 token 不是旗标时才是输出路径（cl
                    // 型守卫）。
                    if (cg::clang::detail::cl_value_follows(p.argv, i))
                    {
                        ++i;
                    }
                    continue;
                }
                // /Fi* /FA* /Fa* /Fe* /Fm*：/FA 可无值（/FA、/FAs、/FAcs）。
                if ((a.starts_with("/Fi") || a.starts_with("/FA")
                     || a.starts_with("/Fa") || a.starts_with("/Fe")
                     || a.starts_with("/Fm"))
                    && a.size() > 3)
                {
                    continue;
                }
                // 输出副作用旗：值与旗标一并剥离，绝不写进构建树。
                if (a == "-MJ")
                {
                    ++i;
                    continue;
                }
                if (a.starts_with("-MJ") && a.size() > 3)
                {
                    continue;
                }
                if (a == "--serialize-diagnostics")
                {
                    ++i;
                    continue;
                }
                if (a.starts_with("--serialize-diagnostics="))
                {
                    continue;
                }
                // MSVC 模块带值开关（/ifcOutput /scanDependencies
                // /sourceDependencies /exportHeader /ifcSearchDir
                // /reference）： 拼写归属经 msvc_module_switch_classify 对
                // cg.util.msvcswitch 单点清单判定（全仓一份——本分支此前只
                // 认识 /sourceDependencies 一份手写拼写，与剥离层各自维护正是
                // 参数逃逸复盘暴露的结构性缺口；大小写与前缀放宽与剥离层同口
                // 径）。冒号连写形态的值已随 token 整体剥除；两段式吞值保持
                // PCH 守卫——紧随 token 是旗标或源文件时绝不吞（源文件是重
                // 放的输入，不是开关的值）。剥除的是 clang 家工具无对应实现
                // 的模块开关与其产物路径（裸值形态的 .ifc 路径若穿透，会被
                // clang-cl 当作额外输入文件）。
                if (const cg::clang::detail::MsvcSwitchMatch mod =
                        cg::clang::detail::msvc_module_switch_classify(a);
                    mod.kind == cg::clang::detail::MsvcModuleSwitchKind::valued)
                {
                    if (!mod.joined
                        && cg::clang::detail::cl_header_value_follows(
                            p.argv,
                            i))
                    {
                        ++i; // 紧随其后的输出文件/路径值
                    }
                    continue;
                }
                if (a == "/Fo" || a == "/Fd")
                {
                    ++i;
                    continue;
                }
                if ((a.starts_with("/Fo") || a.starts_with("/Fd"))
                    && a.size() > 3)
                {
                    continue;
                }
                if (a == "/Fp")
                {
                    ++i;
                    continue;
                }
                if (a.starts_with("/Fp") && a.size() > 3)
                {
                    continue;
                }
                argv.push_back(a);
            }
            argv.emplace_back("-Xclang");
            argv.emplace_back("-detailed-preprocessing-record");
            if (p.cl_mode)
            {
                argv.push_back(std::format("/Fp{}", shadow));
                argv.push_back(std::format("/Fo{}/producer.obj", slot_dir));
                argv.push_back(std::format("/Fd{}/producer.pdb", slot_dir));
            }
            else
            {
                argv.emplace_back("-o");
                argv.push_back(shadow);
            }
            // 索引语境的告警降级（与 clang.tu parse() 的 extra 同款）：
            // non-pod-varargs 是 DefaultError（与 /WX 无关——上面的剥离名单已
            // 去掉 /WX 与 -Werror*），非平凡类型传 varargs 的合法 MSVC producer
            // 头会让影子构建直接失败；只降级错误升级、保留警告本体，两个方言
            // 都接受该开关。
            argv.emplace_back("-Wno-error=non-pod-varargs");
            // /WX- 兜底追加在所有记录旗标之后（后写胜出），兜住剥离白名单未
            // 覆盖的等价写法，确保影子构建永不被警告升级阻断；GNU 驱动不接受
            // /WX-，严格门控在 cl 方言。
            if (p.cl_mode)
            {
                argv.emplace_back("/WX-");
            }
            // 记录 argv 已含输入（MSBuild sidecar 的逐字 argv）时不再追加；
            // GNU cdb 的 loader 丢弃了位置输入，这里补回。
            bool              has_input = false;
            const std::string want = cg::util::norm_path(p.input);
            for (const std::string& a : argv)
            {
                if (!a.empty() && a[0] != '-' && a[0] != '/'
                    && cg::util::norm_path(
                           (std::filesystem::path{p.directory} / a)
                               .lexically_normal()
                               .generic_string())
                           == want)
                {
                    has_input = true;
                    break;
                }
            }
            if (!has_input)
            {
                argv.push_back(p.input);
            }
            return argv;
        }

    } // namespace detail

    /**
     * @brief 让指定的影子 PCH 失效：原子重写清单并删除 slot 目录。
     *
     * @param shadow_paths 盘上影子路径
     *        （`.../pch/<engine_key>/<slot_key>/shadow.pch`）。
     * @return 从清单移除的条目数。
     *
     * @internal pch-unusable 说明缓存影子无法被本引擎加载；继续复用只会
     * 永远失败。重读-修改-原子替换清单（无锁、并发安全到"最多丢一次
     * 重建"），随后删 slot 目录，下一次规划必定本地重建。
     */
    [[nodiscard]]
    inline auto invalidate_cached_pch_artifacts(
        const std::vector<std::string>& shadow_paths) -> std::size_t
    {
        namespace fs = std::filesystem;
        struct Target
        {
            std::string engine_dir;
            std::string slot;
        };
        std::map<std::string, std::vector<Target>> groups;
        for (const std::string& raw : shadow_paths)
        {
            const fs::path shadow = fs::path{cg::util::norm_path(
                fs::path{raw}.lexically_normal().generic_string())};
            const fs::path slot_dir = shadow.parent_path();
            const fs::path engine_dir = slot_dir.parent_path();
            // 只接受 `<config>`/pch/`<engine_key>`/`<slot>`/shadow.pch 布局。
            if (engine_dir.parent_path().filename().generic_string() != "pch")
            {
                continue;
            }
            groups[cg::util::norm_path(
                       engine_dir.lexically_normal().generic_string())]
                .push_back(
                    Target{
                        engine_dir.lexically_normal().generic_string(),
                        slot_dir.filename().generic_string()});
        }
        std::size_t removed_entries = 0;
        for (const auto& [engine_dir, targets] : groups)
        {
            std::unordered_set<std::string> slots;
            for (const Target& target : targets)
            {
                slots.insert(target.slot);
            }
            const fs::path manifest_path{
                fs::path{engine_dir} / "manifest.json"};
            if (const auto text =
                    detail::read_text(manifest_path.generic_string()))
            {
                if (auto parsed = detail::pch_manifest_from_json(*text);
                    parsed.has_value())
                {
                    const std::size_t before = parsed->entries.size();
                    std::erase_if(
                        parsed->entries,
                        [&](const detail::PchEntry& e)
                        { return slots.contains(e.slot_key); });
                    removed_entries += before - parsed->entries.size();
                    if (parsed->entries.size() != before)
                    {
                        // 合并并发写者的新增 slot（被失效的 slot 不复活）。
                        std::unordered_set<std::string> present;
                        for (const detail::PchEntry& e : parsed->entries)
                        {
                            present.insert(e.slot_key);
                        }
                        if (const auto fresh = detail::read_text(
                                manifest_path.generic_string()))
                        {
                            if (const auto disk =
                                    detail::pch_manifest_from_json(*fresh);
                                disk.has_value())
                            {
                                for (detail::PchEntry e : disk->entries)
                                {
                                    if (slots.contains(e.slot_key)
                                        || !present.insert(e.slot_key).second)
                                    {
                                        continue;
                                    }
                                    parsed->entries.push_back(std::move(e));
                                }
                            }
                        }
                        (void)cg::util::platform::atomic_write_file(
                            manifest_path,
                            detail::pch_manifest_to_json(*parsed));
                    }
                }
            }
            for (const Target& target : targets)
            {
                std::error_code ec;
                fs::remove_all(fs::path{engine_dir} / target.slot, ec);
            }
        }
        return removed_entries;
    }

    /**
     * @brief 规划 PCH 消费：发现、构建/复用影子、改写消费者 argv。
     *
     * @param commands 本次要解析的条目（update 时可能是过滤后的子集）。
     * @param coverage 全量编译数据库条目（发现 producer 的权威集合）。
     * @param opt PCH 扫描选项（配置目录等输入，donor 定位的前提）。
     */
    [[nodiscard]]
    inline auto plan_pch(
        std::vector<CompileCommand>        commands,
        const std::vector<CompileCommand>& coverage,
        const PchOptions&                  opt) -> PchPlanResult
    {
        namespace fs = std::filesystem;
        using detail::fold;
        PchPlanResult out;
        out.commands = std::move(commands);

        const auto has_data = [](const CompileCommand& c)
        { return c.pch_involved || c.pch_create || c.pch_use; };
        std::optional<detail::Sidecar> sidecar;
        const bool                     sidecar_file =
            !opt.sidecar_path.empty() && detail::is_file(opt.sidecar_path);
        if (sidecar_file)
        {
            sidecar =
                detail::load_sidecar(opt.sidecar_path, opt.compile_db_path);
        }
        const bool cmd_data = std::ranges::any_of(out.commands, has_data)
                              || std::ranges::any_of(coverage, has_data);
        const bool sidecar_data =
            sidecar.has_value()
            && (!sidecar->producers.empty() || !sidecar->consumers.empty());
        const auto mode = cg::util::cstd::get_env("CCG_PCH_MODE");
        const bool disabled = mode.has_value() && *mode == "off";
        if (disabled)
        {
            return out;
        }
        if (!cmd_data && !sidecar_data)
        {
            // 无 PCH 数据：零变化门禁——绝不建 pch 目录/清单。无效或陈旧
            // 的 pch.json 在此被静默忽略：它不得让无 PCH 工程留下任何
            // PCH 痕迹（note、空 manifest、缓存目录都不能出现）。
            return out;
        }
        const bool replay_degraded =
            opt.replay_degraded
            || (mode.has_value() && *mode == "replay-degraded");

        // 全量 cdb 重新加载（仅当本配置的条目里能看见消费者而 producer
        // 可能因根过滤缺席时）。发现只认构建系统记录。
        std::vector<CompileCommand> discovery = coverage;
        const bool                  coverage_has_consumer = std::ranges::any_of(
            discovery,
            [](const CompileCommand& c) { return c.pch_use; });
        if (!coverage_has_consumer && !opt.compile_db_path.empty())
        {
            try
            {
                discovery = cg::clang::load(opt.compile_db_path, {});
            }
            catch (const std::exception&)
            {
                discovery = coverage;
            }
        }

        const auto consumer_of = [](const std::string& path)
        { return fold(path); };

        std::unordered_map<std::string, detail::ProducerRec> producers;
        const auto                                           add_producer =
            [&producers](detail::ProducerRec rec, bool overwrite)
        {
            if (rec.key.empty())
            {
                return;
            }
            const auto it = producers.find(rec.key);
            if (it == producers.end())
            {
                producers.emplace(rec.key, std::move(rec));
            }
            else if (overwrite)
            {
                it->second = std::move(rec);
            }
        };

        for (const CompileCommand& c : discovery)
        {
            if (!c.pch_create || c.pch_output.empty())
            {
                continue;
            }
            detail::ProducerRec rec;
            rec.display_key = cg::util::norm_path(c.pch_output);
            rec.key = fold(c.pch_output);
            rec.input = c.file;
            rec.directory = c.directory;
            rec.umbrella = c.pch_header;
            rec.forced = c.forced_includes;
            rec.argv = c.argv;
            // loader 的结构化 forced_includes 不含 `-imacros`（它不是
            // 强制包含而是宏注入），这里再从记录的 argv 补一次种子；两者
            // 求并集，重复无害。
            for (const std::string& f :
                 detail::forced_includes_of(rec.argv, rec.directory))
            {
                if (std::ranges::find(rec.forced, f) == rec.forced.end())
                {
                    rec.forced.push_back(f);
                }
            }
            // loader 已从 /Yc//Yu//Fp 或 --driver-mode=cl 推断方言；重放与
            // producer 跳过判定必须用同一信号（raw MSVC cdb 没有
            // --driver-mode=cl，只靠 argv[0]=cl.exe）。
            rec.cl_mode = detail::is_cl_command(c);
            rec.cxx = detail::producer_is_cxx(rec.argv, rec.input);
            add_producer(std::move(rec), false);
        }

        // MSBuild sidecar：producers 覆盖 cdb（cdb 保持剥离 /Yc /Fp）。
        std::vector<detail::SidecarConsumer> sidecar_consumers;
        if (sidecar_file)
        {
            if (sidecar.has_value())
            {
                for (const detail::SidecarProducer& p : sidecar->producers)
                {
                    detail::ProducerRec rec;
                    rec.display_key = p.pch_key;
                    rec.key = fold(p.pch_key);
                    rec.input = p.source;
                    rec.directory = p.cwd;
                    rec.umbrella = p.umbrella;
                    rec.argv.clear();
                    for (std::size_t i = 1; i < p.argv.size(); ++i)
                    {
                        if (i == 1 && p.argv[i] == "--driver-mode=cl")
                        {
                            continue; // 重放自行补模式旗标
                        }
                        rec.argv.push_back(p.argv[i]);
                    }
                    // 旁车 argv 是 /Yc 命令的逐字记录，没有过 cdb 载入的
                    // 宏值归一化；与消费者命令行共用同一函数后，影子 PCH
                    // 烙进的宏值才能与之按字节一致（装载期宏值比对任一侧
                    // 带原始空白即拒载、整组 TU 降级文本重放）。
                    cg::clang::detail::normalize_macro_values(rec.argv);
                    // `/FI` 与 umbrella 是彼此独立的 PCH 构建输入（自建
                    // 夹具实证）。逐字 argv 里的强制包含必须进入闭包，否则改
                    // 了被强制包含的头仍会复用陈旧影子。
                    rec.forced = detail::forced_includes_of(rec.argv, p.cwd);
                    if (!p.umbrella_path.empty()
                        && std::ranges::find(rec.forced, p.umbrella_path)
                               == rec.forced.end())
                    {
                        rec.forced.push_back(p.umbrella_path);
                    }
                    rec.cl_mode = true;
                    rec.cxx = detail::producer_is_cxx(rec.argv, rec.input);
                    rec.from_sidecar = true;
                    add_producer(std::move(rec), true);
                }
                sidecar_consumers = sidecar->consumers;
            }
            // 无效/陈旧的 sidecar 静默忽略：cdb 自身的记录（若有）继续
            // 生效；没有 cdb 记录时就是纯文本解析，不留 PCH 痕迹。
        }
        // 旁车消费者按文件归属索引（同文件多记录后者胜）：只留识别面
        // （关联键与伞头），file 是数据源组织信息，留在 SidecarConsumer
        // 不进裁决。
        std::unordered_map<std::string, cg::clang::detail::SidecarPchFacts>
            sidecar_by_file;
        for (const detail::SidecarConsumer& c : sidecar_consumers)
        {
            sidecar_by_file.insert_or_assign(
                fold(c.file),
                cg::clang::detail::SidecarPchFacts{
                    .pch_key = c.pch_key,
                    .pch_header = c.pch_header});
        }

        // 影子缓存根：`<config>`/pch/`<engine_key>`/
        const DriverInfo& dinfo = driver_info();
        if (dinfo.driver.empty())
        {
            out.notes.push_back(
                "pch: no clang driver found - text fallback (a shadow PCH must "
                "be built with the engine's own driver)");
        }
        std::string       engine_key;
        const std::string engine_version = detail::engine_identity(engine_key);
        std::string       engine_dir;
        if (!opt.ccg_config_dir.empty())
        {
            engine_dir = cg::util::norm_path(
                (fs::path{opt.ccg_config_dir} / "pch" / engine_key)
                    .lexically_normal()
                    .generic_string());
            out.cache_dir = engine_dir;
        }

        detail::PchManifest manifest;
        const std::string   manifest_path =
            engine_dir.empty()
                ? std::string{}
                : cg::util::norm_path((fs::path{engine_dir} / "manifest.json")
                                          .lexically_normal()
                                          .generic_string());
        if (!manifest_path.empty())
        {
            if (const auto text = detail::read_text(manifest_path))
            {
                if (auto parsed = detail::pch_manifest_from_json(*text);
                    parsed.has_value())
                {
                    manifest = std::move(*parsed);
                    if (manifest.engine != engine_version)
                    {
                        // 引擎变化 = 世界观变化：整目录作废。
                        out.notes.push_back(
                            "pch: engine changed - shadow cache wiped");
                        if (!opt.dry_run)
                        {
                            std::error_code ec;
                            for (const fs::directory_entry& e :
                                 fs::directory_iterator(engine_dir, ec))
                            {
                                fs::remove_all(e.path(), ec);
                            }
                        }
                        manifest = detail::PchManifest{};
                    }
                }
            }
        }
        manifest.engine = engine_version;
        manifest.engine_key = engine_key;

        std::unordered_map<std::string, detail::PchEntry> prior;
        for (detail::PchEntry& e : manifest.entries)
        {
            prior.insert_or_assign(e.slot_key, e);
        }
        std::unordered_set<std::string> live_slots;

        /**
         * @brief 跨配置 donor：兄弟配置目录里同一 slot/pch/闭包的影子。
         *
         * @internal 影子字节是引擎 + producer 命令 + 闭包的纯函数，故同
         * engine_key 下的字节可跨配置重定位。命中条件保守：名单里的
         * slot_key/pch_key/closure_hash 全等且 donor 字节的 sha256 与台账
         * 一致；绝不共享句柄，只复制。
         */
        const auto donor_for = [&](std::string_view  slot_key,
                                   std::string&      donor_path,
                                   detail::PchEntry& donor_entry) -> bool
        {
            if (opt.ccg_config_dir.empty())
            {
                return false;
            }
            const fs::path cfg =
                fs::path{opt.ccg_config_dir}.lexically_normal();
            std::error_code lec;
            for (const fs::directory_entry& sibling :
                 fs::directory_iterator(cfg.parent_path(), lec))
            {
                std::error_code sec;
                if (!sibling.is_directory(sec)
                    || fold(sibling.path().lexically_normal().generic_string())
                           == fold(cfg.generic_string()))
                {
                    continue;
                }
                const fs::path donor_engine =
                    sibling.path() / "pch" / engine_key;
                const fs::path  donor_manifest = donor_engine / "manifest.json";
                std::error_code mec;
                if (!fs::is_regular_file(donor_manifest, mec))
                {
                    continue;
                }
                const auto text = detail::read_text(
                    donor_manifest.lexically_normal().generic_string());
                if (!text)
                {
                    continue;
                }
                const auto other = detail::pch_manifest_from_json(*text);
                if (!other.has_value() || other->engine != engine_version)
                {
                    continue;
                }
                for (const detail::PchEntry& e : other->entries)
                {
                    // 匹配只看生产者身份（slot_key 同 = 同引擎/方言/摘要）；
                    // 闭包与 pch_key 由调用方按本盘现状重验——donor 的依赖集
                    // 描述的是 donor 盘，必须逐文件重哈希通过才采用。
                    if (e.slot_key != slot_key || e.pch_hash.empty()
                        || e.pch_size <= 0 || e.closure.empty())
                    {
                        continue; // donor 记录必须带非零尺寸与闭包
                    }
                    const fs::path donor =
                        donor_engine / std::string{slot_key} / "shadow.pch";
                    std::error_code cec;
                    const auto      size = fs::file_size(donor, cec);
                    if (cec || size == 0)
                    {
                        continue;
                    }
                    const std::string hash = cg::clang::detail::hash_source(
                        donor.lexically_normal().generic_string());
                    if (hash.empty() || hash != e.pch_hash)
                    {
                        continue;
                    }
                    donor_path = cg::util::norm_path(
                        donor.lexically_normal().generic_string());
                    donor_entry = e;
                    return true;
                }
            }
            return false;
        };

        // 只构建被消费者引用的 producer。命令侧的引用面经唯一裁决点
        // （resolve_pch_consumer：cdb 令牌识别与旁车记录合并）得出；
        // 旁车记录自身的引用声明全量保留——其 TU 可能不在 discovery
        // （根过滤外），producer 的引用不能因命令缺席而丢失。
        std::unordered_map<std::string, bool> referenced;
        for (const CompileCommand& c : discovery)
        {
            const cg::clang::detail::SidecarPchFacts* facts = nullptr;
            if (const auto it = sidecar_by_file.find(fold(c.file));
                it != sidecar_by_file.end())
            {
                facts = &it->second;
            }
            const cg::clang::detail::PchConsumerVerdict verdict =
                cg::clang::detail::resolve_pch_consumer(c, facts);
            if (verdict.use && !verdict.path.empty())
            {
                referenced[consumer_of(verdict.path)] = true;
            }
        }
        for (const detail::SidecarConsumer& c : sidecar_consumers)
        {
            referenced[fold(c.pch_key)] = true;
        }

        struct ShadowUse
        {
            std::string path;
            std::string identity;
            std::string note;
            bool        ok = false;
        };
        std::unordered_map<std::string, ShadowUse> shadows;

        for (const auto& [key, rec] : producers)
        {
            if (!referenced.contains(key))
            {
                continue;
            }
            ShadowUse use;
            if (dinfo.driver.empty() || engine_dir.empty())
            {
                use.note = "pch-producer-missing: no engine driver/cache dir";
                shadows.emplace(key, std::move(use));
                continue;
            }
            std::vector<std::string> inc_dirs;
            if (rec.cl_mode)
            {
                // cl 方言真实顺序（实测）：
                //   /I → CPATH → resource dir → -imsvc → INCLUDE
                // 自包含 argv（`/X` + 全量 -imsvc）下资源目录之外的一切
                // 环境/自动探测都必须关掉：闭包只用 argv 目录 + resource。
                const detail::IncludeDirsByKind kinds =
                    detail::include_dirs_by_kind(rec.argv, rec.directory);
                detail::append_include_dirs(inc_dirs, kinds.normal);
                const bool self_contained =
                    detail::is_self_contained_cl(rec.argv);
                if (!self_contained)
                {
                    detail::append_include_dirs(
                        inc_dirs,
                        detail::cpath_include_dirs(rec.cxx, rec.directory));
                }
                const std::string resource = detail::resource_include_dir();
                if (!resource.empty())
                {
                    inc_dirs.push_back(resource);
                }
                detail::append_include_dirs(inc_dirs, kinds.imsvc);
                if (!self_contained)
                {
                    detail::append_include_dirs(
                        inc_dirs,
                        detail::include_env_dirs(rec.directory));
                    // 自动探测链是回放的最后一层兜底；resource 已在前面，
                    // 去重使其不会重复。
                    detail::append_include_dirs(
                        inc_dirs,
                        cg::clang::driver_include_dirs(rec.cxx, true));
                }
            }
            else
            {
                inc_dirs = detail::include_dirs_of(rec.argv, rec.directory);
                // GNU 链：命令行 → CPATH/语言变量 → 驱动器默认（libc++）。
                detail::append_include_dirs(
                    inc_dirs,
                    detail::cpath_include_dirs(rec.cxx, rec.directory));
                detail::append_include_dirs(
                    inc_dirs,
                    cg::clang::driver_include_dirs(rec.cxx, false));
            }
            std::vector<std::string> seeds = rec.forced;
            if (!rec.input.empty())
            {
                seeds.push_back(rec.input);
            }
            if (!rec.umbrella.empty())
            {
                // 伞头先按记录目录解析；MSBuild 记录的伞头常是裸名，回退到
                // 记录的 include 搜索目录。
                std::string umbrella_path =
                    detail::resolve_from(rec.umbrella, rec.directory);
                if (!detail::is_file(umbrella_path))
                {
                    for (const std::string& dir : inc_dirs)
                    {
                        const std::string candidate =
                            detail::resolve_from(rec.umbrella, dir);
                        if (detail::is_file(candidate))
                        {
                            umbrella_path = candidate;
                            break;
                        }
                    }
                }
                if (detail::is_file(umbrella_path))
                {
                    seeds.push_back(std::move(umbrella_path));
                }
            }
            const std::string pd = detail::producer_digest(rec);
            const std::string slot_key = detail::hash_text(
                                             std::format(
                                                 "pch-v3\n{}\n{}\n{}",
                                                 engine_key,
                                                 rec.cl_mode ? "cl" : "gnu",
                                                 pd))
                                             .substr(0, 16);
            // 依赖集（缓存闭包）来自上一轮影子构建的 clang 真实解析：台账
            // 闭包逐文件字节重哈希（读不到即失配），全部一致 pch_key 才可
            // 能匹配。首跑/台账缺失走构建路径——构建运行附带依赖发射（同一
            // 遍预处理：宏 #include、搜索路径与大小写由真编译器按构建语义
            // 消解）。
            std::vector<std::pair<std::string, std::string>> closure_files;
            bool closure_from_ledger = false;
            if (const auto pit = prior.find(slot_key);
                pit != prior.end() && !pit->second.closure.empty())
            {
                closure_from_ledger = true;
                closure_files.reserve(pit->second.closure.size());
                for (const auto& dep : pit->second.closure)
                {
                    closure_files.emplace_back(
                        dep.first,
                        cg::clang::detail::hash_source(dep.first));
                }
            }
            std::string       closure_hash = detail::sha_of_list(closure_files);
            std::string       pch_key = detail::hash_text(
                                            std::format(
                                                "pch-id-v3\n{}\n{}\ndpr=1",
                                                slot_key,
                                                closure_hash))
                                            .substr(0, 32);
            const std::string slot_dir =
                cg::util::norm_path((fs::path{engine_dir} / slot_key)
                                        .lexically_normal()
                                        .generic_string());
            const std::string shadow =
                cg::util::norm_path((fs::path{slot_dir} / "shadow.pch")
                                        .lexically_normal()
                                        .generic_string());

            std::error_code fsec;
            const auto      fsize = fs::file_size(fs::path{shadow}, fsec);
            bool            reuse = false;
            std::string     current_hash;
            if (!opt.force && closure_from_ledger && !fsec && fsize > 0)
            {
                if (const auto it = prior.find(slot_key);
                    it != prior.end() && it->second.pch_key == pch_key
                    && it->second.closure_hash == closure_hash
                    && it->second.pch_size == static_cast<std::int64_t>(fsize)
                    && !it->second.pch_hash.empty())
                {
                    // 同路径内容替换（构建系统的 PCH 被外部改写）必须失配：
                    // 字节哈希是身份，mtime 不是。
                    current_hash = cg::clang::detail::hash_source(shadow);
                    if (!current_hash.empty()
                        && current_hash == it->second.pch_hash)
                    {
                        reuse = true;
                    }
                }
            }
            if (reuse)
            {
                use.ok = true;
                use.path = shadow;
                // 身份只认字节哈希：内容相同的重建（mtime 变化）不得让
                // 全部消费者 TU 变 stale。
                use.identity = detail::hash_text(
                                   std::format(
                                       "{}|{}|{}",
                                       engine_key,
                                       pch_key,
                                       current_hash))
                                   .substr(0, 32);
                ++out.reused;
                live_slots.insert(slot_key);
                shadows.emplace(key, std::move(use));
                continue;
            }
            // 跨配置 donor：本配置台账 miss（首跑）时从兄弟配置复制同一
            // 生产者产出，避免重复构建。donor 记录自带闭包——逐文件在本盘
            // 重哈希全部通过才采用（依赖集漂移即弃用，落回本地构建）。
            detail::PchEntry donor_entry;
            std::string      donor_shadow;
            bool             donor_adopted = false;
            if (!opt.force && !opt.dry_run
                && donor_for(slot_key, donor_shadow, donor_entry))
            {
                std::vector<std::pair<std::string, std::string>> adopted;
                adopted.reserve(donor_entry.closure.size());
                bool deps_ok = true;
                for (const auto& dep : donor_entry.closure)
                {
                    const std::string h =
                        cg::clang::detail::hash_source(dep.first);
                    if (h.empty() || h != dep.second)
                    {
                        deps_ok = false;
                        break;
                    }
                    adopted.emplace_back(dep.first, h);
                }
                if (deps_ok)
                {
                    closure_files = std::move(adopted);
                    closure_from_ledger = true;
                    closure_hash = detail::sha_of_list(closure_files);
                    pch_key = detail::hash_text(
                                  std::format(
                                      "pch-id-v3\n{}\n{}\ndpr=1",
                                      slot_key,
                                      closure_hash))
                                  .substr(0, 32);
                    donor_adopted = true;
                }
            }
            if (donor_adopted)
            {
                if (!detail::create_dir(slot_dir))
                {
                    use.note =
                        "pch-build-failed: cannot create cache directory";
                    shadows.emplace(key, std::move(use));
                    continue;
                }
                // 复制到唯一 tmp，校验尺寸/哈希后才原子替换正式影子：
                // 半截 donor 副本绝不落到正式名上。
                const fs::path donor_tmp =
                    cg::util::platform::unique_temp_path(fs::path{shadow});
                std::error_code cec;
                fs::copy_file(
                    fs::path{donor_shadow},
                    donor_tmp,
                    fs::copy_options::overwrite_existing,
                    cec);
                if (!cec)
                {
                    const auto stat = cg::clang::detail::stat_source(
                        donor_tmp.generic_string());
                    const std::string copied_hash =
                        cg::clang::detail::hash_source(
                            donor_tmp.generic_string());
                    // 复制后尺寸必须与 donor 台账一致；有哈希则复核。
                    const bool size_ok = stat.ok && stat.size > 0
                                         && stat.size == donor_entry.pch_size;
                    const bool hash_ok = !copied_hash.empty()
                                         && copied_hash == donor_entry.pch_hash;
                    if (size_ok && hash_ok
                        && cg::util::platform::atomic_replace(
                            donor_tmp,
                            fs::path{shadow}))
                    {
                        detail::PchEntry entry = donor_entry;
                        entry.producer_file = rec.display_key;
                        entry.producer_input = rec.input;
                        entry.producer_cwd = rec.directory;
                        entry.producer_digest = pd;
                        entry.dialect = rec.cl_mode ? "cl" : "gnu";
                        // 记录 donor 来源，台账可审计跨配置复制。
                        entry.donor = donor_shadow;
                        entry.pch_size = stat.size;
                        // 复制后按本地字节重算，台账永远描述本配置盘上的影子。
                        entry.pch_hash = copied_hash;
                        entry.pch_key = pch_key;
                        prior.insert_or_assign(slot_key, entry);
                        live_slots.insert(slot_key);
                        use.ok = true;
                        use.path = shadow;
                        use.identity = detail::hash_text(
                                           std::format(
                                               "{}|{}|{}",
                                               engine_key,
                                               pch_key,
                                               entry.pch_hash))
                                           .substr(0, 32);
                        ++out.reused;
                        ++out.donated;
                        out.tu_notes.push_back(
                            std::format(
                                "{}: pch-donor: shadow copied from {}",
                                rec.input,
                                donor_shadow));
                        shadows.emplace(key, std::move(use));
                        continue;
                    }
                    // donor 字节不符/替换失败：删 tmp，落回本地重建
                    // （正式影子从未被触碰）。
                    cg::util::platform::remove_file(donor_tmp);
                }
                // 复制失败：落回本地构建，不把失败写进台账。
            }
            if (opt.dry_run)
            {
                use.ok = true;
                use.path = shadow;
                use.identity = "dry:" + pch_key;
                ++out.built;
                live_slots.insert(slot_key);
                shadows.emplace(key, std::move(use));
                continue;
            }
            if (!detail::create_dir(slot_dir))
            {
                use.note = "pch-build-failed: cannot create cache directory";
                shadows.emplace(key, std::move(use));
                continue;
            }
            // 影子也先写唯一 tmp，exit 0 且尺寸 >0 才原子替换正式名：
            // 失败时旧影子原样保留，绝不出现半截缓存。
            const fs::path shadow_tmp_path =
                cg::util::platform::unique_temp_path(fs::path{shadow});
            const std::string shadow_tmp = shadow_tmp_path.generic_string();
            std::vector<std::string> replay =
                detail::replay_argv(rec, shadow_tmp, slot_dir);
            // 依赖发射：与影子构建同一遍预处理（宏 #include、搜索路径、
            // 大小写由真编译器消解）。cl 方言走 /showIncludes 的 stdout；
            // gnu 方言走 -MD -MF，依赖文件落在 slot 缓存目录（过渡产物）。
            std::string dep_make;
            if (rec.cl_mode)
            {
                replay.emplace_back("/showIncludes");
            }
            else
            {
                dep_make = (fs::path{slot_dir} / "deps.d").generic_string();
                replay.emplace_back("-MD");
                replay.emplace_back("-MF");
                replay.emplace_back(dep_make);
            }
            if (cg::util::cstd::get_env("CCG_PCH_DEBUG").has_value())
            {
                std::print(std::cerr, "PCH-REPLAY {}:", rec.input);
                for (const std::string& a : replay)
                {
                    std::print(std::cerr, " [{}]", a);
                }
                std::println(std::cerr);
            }
            const std::vector<std::string> replay_args{
                replay.begin() + 1,
                replay.end()};
            const cg::clang::detail::ProcResult proc =
                cg::clang::detail::run_tool(
                    replay.front(),
                    replay_args,
                    rec.directory);
            std::error_code sec;
            std::int64_t    produced_size = 0;
            if (proc.started && proc.exit_code == 0
                && fs::is_regular_file(shadow_tmp_path, sec))
            {
                const auto size = fs::file_size(shadow_tmp_path, sec);
                if (!sec)
                {
                    produced_size = static_cast<std::int64_t>(size);
                }
            }
            const bool built = produced_size > 0
                               && cg::util::platform::atomic_replace(
                                   shadow_tmp_path,
                                   fs::path{shadow});
            if (!built)
            {
                cg::util::platform::remove_file(shadow_tmp_path);
                use.note = std::format(
                    "pch-build-failed: {}",
                    proc.started ? detail::first_line_of(
                                       proc.err.empty() ? proc.out : proc.err)
                                 : std::string{"driver did not start"});
                if (use.note.ends_with(": "))
                {
                    use.note += "unknown error";
                }
                shadows.emplace(key, std::move(use));
                continue;
            }
            const auto stat = cg::clang::detail::stat_source(shadow);
            // 构建期发射的依赖清单 → 真实闭包（种子 + 全部被包含文件，
            // 大小写折叠去重、逐文件字节哈希），并据此重定身份键。
            std::vector<std::string> emitted_deps;
            if (rec.cl_mode)
            {
                emitted_deps = detail::parse_show_includes(proc.out);
            }
            else if (!dep_make.empty())
            {
                if (const auto dep_text = detail::read_text(dep_make))
                {
                    emitted_deps = detail::parse_make_depfile(*dep_text);
                }
                cg::util::platform::remove_file(
                    fs::path{dep_make}); // 过渡产物，不留缓存目录
            }
            detail::PchEntry entry;
            entry.slot_key = slot_key;
            entry.producer_file = rec.display_key;
            entry.producer_input = rec.input;
            entry.producer_cwd = rec.directory;
            entry.producer_digest = pd;
            entry.dialect = rec.cl_mode ? "cl" : "gnu";
            entry.closure = detail::assemble_closure(seeds, emitted_deps);
            entry.closure_hash = detail::sha_of_list(entry.closure);
            pch_key = detail::hash_text(
                          std::format(
                              "pch-id-v3\n{}\n{}\ndpr=1",
                              slot_key,
                              entry.closure_hash))
                          .substr(0, 32);
            entry.pch_key = pch_key;
            entry.donor.clear();
            entry.pch_size = stat.ok ? stat.size : 0;
            entry.pch_hash = cg::clang::detail::hash_source(shadow);
            prior.insert_or_assign(slot_key, entry);
            live_slots.insert(slot_key);
            use.ok = true;
            use.path = shadow;
            use.identity = detail::hash_text(
                               std::format(
                                   "{}|{}|{}",
                                   engine_key,
                                   pch_key,
                                   entry.pch_hash))
                               .substr(0, 32);
            ++out.built;
            shadows.emplace(key, std::move(use));
        }

        // 消费者改写（只动本批 commands）。GNU producer 是生成的
        // `<target>`.dir/cmake_pch.hxx.cxx（内容只有注释），从解析集剔除。
        std::vector<CompileCommand> rewritten;
        rewritten.reserve(out.commands.size());
        for (CompileCommand& c : out.commands)
        {
            if (c.pch_create)
            {
                if (!detail::is_cl_command(c))
                {
                    ++out.producers_skipped;
                    continue;
                }
                c.argv = detail::strip_yc(c.argv);
                c.pch.status = PchUse::Status::none;
                c.pch.identity.clear();
                rewritten.push_back(std::move(c));
                continue;
            }
            // 消费者身份经唯一裁决点合并（cdb 令牌识别 vs 旁车记录），
            // 不再就地拼。裁决结果整组写回：cdb 已判定的条目函数原样
            // 返回其识别面（等价于原代码的跳过不写），旁车命中的条目
            // 取得 pch_key/pch_header 并补 pch_involved——与原合并语义
            // 逐条一致。
            const cg::clang::detail::SidecarPchFacts* facts = nullptr;
            if (const auto it = sidecar_by_file.find(fold(c.file));
                it != sidecar_by_file.end())
            {
                facts = &it->second;
            }
            const cg::clang::detail::PchConsumerVerdict verdict =
                cg::clang::detail::resolve_pch_consumer(c, facts);
            c.pch_use = verdict.use;
            c.pch_path = verdict.path;
            c.pch_header = verdict.header;
            c.pch_involved = c.pch_involved || verdict.sidecar_sourced;
            if (!c.pch_use)
            {
                if (c.pch_involved)
                {
                    // 隐式 PCH（`-include` 头旁有 .pch/.gch）：loader 已把
                    // -include 改写为转发形式绕开自动加载；没有记录的
                    // producer → 文本回退。
                    c.pch.status = PchUse::Status::text_fallback;
                    c.pch.identity = "text";
                    c.pch.note =
                        "pch-producer-missing: implicit PCH beside a "
                        "-include header - text fallback (the -include flag "
                        "was rewritten to bypass the implicit load)";
                    ++out.text_fallback;
                    out.tu_notes.push_back(
                        std::format(
                            "{}: {}",
                            cg::util::norm_path(c.file),
                            c.pch.note));
                }
                rewritten.push_back(std::move(c));
                continue;
            }
            ++out.consumers;
            const std::string key = consumer_of(c.pch_path);
            const auto        sit = shadows.find(key);
            const bool linked = producers.contains(key) && sit != shadows.end()
                                && sit->second.ok;
            if (linked)
            {
                c.argv = detail::inject_shadow(c.argv, sit->second.path);
                c.pch_fallback_argv = detail::strip_include_pch(c.argv);
                c.pch.status = PchUse::Status::shadow;
                c.pch.identity = sit->second.identity;
                c.pch.shadow_path = sit->second.path;
                c.pch.note.clear();
                rewritten.push_back(std::move(c));
                continue;
            }
            const auto pit = producers.find(key);
            if (pit == producers.end())
            {
                // 没有记录的生产命令：禁止合成 recipe，直接文本回退。
                if (replay_degraded && !c.pch_path.empty()
                    && detail::is_file(c.pch_path))
                {
                    c.argv =
                        detail::inject_shadow(std::move(c.argv), c.pch_path);
                    c.pch.status = PchUse::Status::replay_degraded;
                    c.pch.identity = "replay-degraded";
                    c.pch.note = "replay-degraded (graph incomplete: no "
                                 "DetailedPreprocessingRecord)";
                    ++out.replay_degraded;
                    out.tu_notes.push_back(
                        std::format(
                            "{}: {}",
                            cg::util::norm_path(c.file),
                            c.pch.note));
                    rewritten.push_back(std::move(c));
                    continue;
                }
                c.argv = detail::strip_include_pch(std::move(c.argv));
                c.pch_fallback_argv = c.argv;
                c.pch.status = PchUse::Status::text_fallback;
                c.pch.identity = "text";
                const bool recorded_exists =
                    !c.pch_path.empty() && detail::is_file(c.pch_path);
                c.pch.note =
                    recorded_exists
                        ? "pch-rejected: the recorded PCH was not produced by "
                          "this engine - never consumed directly; text fallback"
                        : (c.pch_path.empty()
                               ? "pch-producer-missing: no recorded producer "
                                 "command - text fallback (recipe synthesis is "
                                 "forbidden)"
                               : "pch-missing: the recorded PCH (and a "
                                 "producer command) is absent - text fallback");
                ++out.text_fallback;
                out.tu_notes.push_back(
                    std::format(
                        "{}: {}",
                        cg::util::norm_path(c.file),
                        c.pch.note));
                rewritten.push_back(std::move(c));
                continue;
            }
            c.argv = detail::strip_include_pch(std::move(c.argv));
            c.pch_fallback_argv = c.argv;
            c.pch.status = PchUse::Status::text_fallback;
            c.pch.identity = "text";
            const std::string reason =
                sit != shadows.end() && !sit->second.note.empty()
                    ? sit->second.note
                    : std::string{"pch-build-failed"};
            c.pch.note = std::format("{} - text fallback", reason);
            ++out.text_fallback;
            out.tu_notes.push_back(
                std::format("{}: {}", cg::util::norm_path(c.file), c.pch.note));
            rewritten.push_back(std::move(c));
        }
        out.commands = std::move(rewritten);

        // 清单与孤儿清理。
        if (!manifest_path.empty() && !opt.dry_run)
        {
            // 宽限期：只有显式 --force/--full（opt.purge）且目标 mtime
            // 超过 1 小时才允许删除；增量/update 从不删缓存产物。
            constexpr auto grace = std::chrono::hours{1};
            const auto     older_than_grace = [&](const fs::path& p)
            {
                std::error_code ec;
                const auto      mtime = fs::last_write_time(p, ec);
                if (ec)
                {
                    return false;
                }
                return fs::file_time_type::clock::now() - mtime > grace;
            };
            detail::PchManifest next;
            next.engine = engine_version;
            next.engine_key = engine_key;
            std::unordered_set<std::string> dropped;
            for (const auto& [slot, entry] : prior)
            {
                if (live_slots.contains(slot))
                {
                    next.entries.push_back(entry);
                    continue;
                }
                if (!opt.purge)
                {
                    // 增量：结转所有旧条目，不删除任何目录。
                    next.entries.push_back(entry);
                    continue;
                }
                const fs::path slot_dir =
                    fs::path{engine_dir} / std::string{slot};
                if (older_than_grace(slot_dir))
                {
                    dropped.insert(slot);
                }
                else
                {
                    next.entries.push_back(entry);
                }
            }
            std::unordered_set<std::string> present;
            for (const detail::PchEntry& e : next.entries)
            {
                present.insert(e.slot_key);
            }
            // 乐观合并：并发规划器可能刚写入了它自己的 slot 条目；重读
            // 盘上清单并入未知条目，绝不复活本趟显式丢弃的 slot。
            if (const auto text = detail::read_text(manifest_path))
            {
                if (const auto disk = detail::pch_manifest_from_json(*text);
                    disk.has_value() && disk->engine == engine_version)
                {
                    for (detail::PchEntry e : disk->entries)
                    {
                        if (present.contains(e.slot_key)
                            || dropped.contains(e.slot_key))
                        {
                            continue;
                        }
                        present.insert(e.slot_key);
                        next.entries.push_back(std::move(e));
                    }
                }
            }
            if (detail::create_dir(engine_dir))
            {
                const std::string text = detail::pch_manifest_to_json(next);
                if (!cg::util::platform::atomic_write_file(
                        fs::path{manifest_path},
                        text))
                {
                    out.notes.push_back(
                        "pch: manifest write failed (the previous manifest was "
                        "kept)");
                }
            }
            if (opt.purge)
            {
                // 引用集来自重新读取的完整清单（写入后的盘上版本），
                // 目标必须超过宽限期才删除。清单读不回时不做任何回收：
                // 没有完整引用集就无法证明某个 slot 是孤儿。
                std::unordered_set<std::string> survivors;
                bool                            have_disk_manifest = false;
                if (const auto text = detail::read_text(manifest_path))
                {
                    if (const auto disk = detail::pch_manifest_from_json(*text);
                        disk.has_value() && disk->engine == engine_version)
                    {
                        for (const detail::PchEntry& e : disk->entries)
                        {
                            survivors.insert(e.slot_key);
                        }
                        have_disk_manifest = true;
                    }
                }
                for (const detail::PchEntry& e : next.entries)
                {
                    survivors.insert(e.slot_key);
                }
                if (have_disk_manifest)
                {
                    std::error_code ec;
                    for (const fs::directory_entry& e :
                         fs::directory_iterator(engine_dir, ec))
                    {
                        std::error_code isec;
                        if (!e.is_directory(isec))
                        {
                            continue;
                        }
                        const std::string slot =
                            e.path().filename().generic_string();
                        if (survivors.contains(slot)
                            || !older_than_grace(e.path()))
                        {
                            continue;
                        }
                        fs::remove_all(e.path(), ec);
                    }
                }
            }
        }

        out.notes.push_back(
            std::format(
                "shadow PCH: {} built, {} reused ({} donor), {} text fallbacks, "
                "{} unusable, {} replay-degraded, "
                "{} consumer TU(s), {} producer TU(s) skipped (cache {})",
                out.built,
                out.reused,
                out.donated,
                out.text_fallback,
                out.unusable,
                out.replay_degraded,
                out.consumers,
                out.producers_skipped,
                engine_dir.empty() ? std::string{"<none>"} : engine_dir));
        return out;
    }

} // namespace cg::clang
