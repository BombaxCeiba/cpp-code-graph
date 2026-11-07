/**
 * @module cg.cli.layout
 *
 * @brief .ccg 集中布局的路径推导与配置档案。
 *
 * 产出文件夹（-o，默认 $CCG_DB 或 ./.ccg）之下【每配置一个子目录】
 * `<folder>`/`<config>`/：ccg.db、BMI 缓存子树与 profile.json 全住里面；
 * 引擎无关共享物（clang-cl 垫片、msbuild.json 注册表）留文件夹根。本模块
 * 是配置目录推导的唯一收口——仓内其余代码经此处的函数取路径，绝不散落
 * 自行拼接。
 *
 * 发现规则（查询侧）：CCG_DB > -o 文件夹 > 默认 ./.ccg/；文件夹下唯一
 * 配置时 --config 可省略自动选中，多配置必须显式（报错列出候选）；直接
 * 指到配置目录（内含 ccg_db）同样受认；空布局时报错并给出创建指引。
 */
export module cg.cli.layout;

import std;
import cg.util;
import cg.util.cstd;
import cg.cli.parse;

export namespace cg::cli
{

    /**
     * @brief 一次布局解析的结果：产出文件夹、配置名与该配置的一切路径。
     */
    struct LayoutChoice
    {
        std::string root;       // 产出文件夹（绝对、规范化）
        std::string config;     // 选中的配置名
        std::string config_dir; // root/<config>/
        std::string db;         // config_dir/ccg.db
    };

    namespace layout
    {
        namespace fs = std::filesystem;

        /** @brief 文件夹内库文件的名字（ccg.db 与派生物同居配置目录）。
         */
        inline constexpr std::string_view db_file_name = "ccg.db";
        /** @brief 配置档案的名字（裁量
         * c：root/compile-commands/引擎身份/创建参数）。
         */
        inline constexpr std::string_view profile_file_name = "profile.json";

        /**
         * @brief 解析后的产出文件夹（绝对 + 词典序规范化 + norm_path）。
         *
         * 相对拼写按当前工作目录绝对化；失败时原样规范化返回（后续存在性检查
         * 自会给出可行动报错）。
         */
        [[nodiscard]]
        inline auto absolute_norm_dir(const std::string& dir) -> std::string
        {
            std::error_code ec;
            const fs::path  abs = fs::absolute(fs::path{dir}, ec);
            const fs::path  base = ec ? fs::path{dir} : abs;
            return cg::util::norm_path(
                base.lexically_normal().generic_string());
        }

        /** @brief 配置目录 =
         * `<文件夹>`/`<配置>`（仓内唯一收口，禁止他处手拼）。 */
        [[nodiscard]]
        inline auto config_dir_under(
            const std::string& root,
            const std::string& config) -> std::string
        {
            return cg::util::norm_path(
                (fs::path{root} / config).lexically_normal().generic_string());
        }

        /** @brief 配置目录内的库文件路径。 */
        [[nodiscard]]
        inline auto db_under(const std::string& config_dir) -> std::string
        {
            return cg::util::norm_path((fs::path{config_dir} / db_file_name)
                                           .lexically_normal()
                                           .generic_string());
        }

        /**
         * @brief 拥有某个库文件的配置目录（BMI 缓存等派生物的锚）。
         *
         * @internal 库文件恒为
         * `<配置目录>`/ccg.db，故配置目录即其父目录；空路径
         * 返回空串（调用方自行回退）。
         */
        [[nodiscard]]
        inline auto config_dir_of_db(std::string_view db_file) -> std::string
        {
            if (db_file.empty())
            {
                return {};
            }
            return cg::util::norm_path(
                fs::path{std::string{db_file}}
                    .parent_path()
                    .lexically_normal()
                    .generic_string());
        }

        /**
         * @brief 一个产出文件夹下的配置清单：内含 ccg.db 或 profile.json 的
         *        子目录名（词典序）。
         *
         * @internal 以内容判定而非名单过滤：文件夹根的引擎无关共享物
         * （msbuild/ 捕获日志区、hooks/ 发行蹦床、顶层 bmi-self-* 缓存）顶层
         * 都没有这两样，天然出局。
         */
        [[nodiscard]]
        inline auto list_configs(const std::string& root_dir)
            -> std::vector<std::string>
        {
            std::vector<std::string> out;
            std::error_code          ec;
            for (const fs::directory_entry& entry :
                 fs::directory_iterator(fs::path{root_dir}, ec))
            {
                std::error_code lec;
                if (!entry.is_directory(lec))
                {
                    continue;
                }
                const fs::path  dir = entry.path();
                std::error_code bec;
                std::error_code pec;
                if (fs::is_regular_file(dir / db_file_name, bec)
                    || fs::is_regular_file(dir / profile_file_name, pec))
                {
                    out.push_back(dir.filename().generic_string());
                }
            }
            std::ranges::sort(out);
            return out;
        }

        /**
         * @brief 解析产出文件夹的公共段：-o > $CCG_DB > ./.ccg，拒绝文件形态。
         *
         * @internal CCG_DB 与 -o 同一语义：取值必须是产出文件夹（或单个配置
         * 目录）；指到文件时响亮报错，绝不静默当库用。
         */
        [[nodiscard]]
        inline auto resolve_root_dir(const Invocation& inv) -> std::string
        {
            std::string dir = "./.ccg";
            bool        from_env = false;
            if (const auto env = cg::util::cstd::get_env("CCG_DB"))
            {
                if (!env->empty())
                {
                    dir = *env;
                    from_env = true;
                }
            }
            if (inv.db_explicit)
            {
                dir = inv.output_dir;
                from_env = false;
            }
            const std::string norm = absolute_norm_dir(dir);
            std::error_code   ec;
            if (fs::is_regular_file(fs::path{norm}, ec))
            {
                throw std::runtime_error(
                    std::format(
                        "'{}' names the FILE '{}' - it must be the output FOLDER "
                        "holding one directory per configuration "
                        "(<folder>/<config>/ccg.db)",
                        std::string{from_env ? "$CCG_DB" : "-o"},
                        norm));
            }
            return norm;
        }

        /**
         * @brief 查询侧发现（query 族/update/config/hook 处理器）：配置可省略时
         *        自动选中唯一配置，多配置点名候选，空布局给出创建指引。
         *
         * allow_missing_explicit 供会创建库的调用方（config set）：显式
         * --config 的配置目录尚无 ccg.db 也放行（由后续 open_store_create
         * 落地）。
         *
         * @internal 文件夹直接是一个配置目录（内含 ccg.db）时按该配置解析——
         * $CCG_DB 语义（d）允许直接指配置目录；显式 --config 与之不符则报错，
         * 防止"文件夹套配置"的嵌套误用。
         */
        [[nodiscard]]
        inline auto resolve_query_layout(
            const Invocation& inv,
            bool              allow_missing_explicit = false) -> LayoutChoice
        {
            const std::string root = resolve_root_dir(inv);
            std::error_code   ec;
            if (fs::is_regular_file(fs::path{root} / db_file_name, ec))
            {
                const std::string config =
                    fs::path{root}.filename().generic_string();
                if (inv.config_explicit && inv.config_name != config)
                {
                    throw std::runtime_error(
                        std::format(
                            "the folder '{}' is itself the configuration '{}' (it holds "
                            "ccg.db), but --config names '{}' - point -o/$CCG_DB at the "
                            "PARENT folder instead",
                            root,
                            config,
                            inv.config_name));
                }
                return {
                    .root = config_dir_of_db(root),
                    .config = config,
                    .config_dir = root,
                    .db = db_under(root)};
            }
            const std::vector<std::string> configs = list_configs(root);
            if (inv.config_explicit)
            {
                const std::string config = inv.config_name;
                const bool        known =
                    std::ranges::find(configs, config) != configs.end();
                if (!known && !allow_missing_explicit)
                {
                    std::string have = configs.empty()
                                           ? "no configurations found"
                                           : "found: " + [&]
                    {
                        std::string joined;
                        for (const std::string& c : configs)
                        {
                            joined += joined.empty() ? c : ", " + c;
                        }
                        return joined;
                    }();
                    throw std::runtime_error(
                        std::format(
                            "cannot open database '{}': file not found (no configuration "
                            "'{}' under {}; {})",
                            db_under(config_dir_under(root, config)),
                            config,
                            root,
                            have));
                }
                const std::string dir = config_dir_under(root, config);
                return {
                    .root = root,
                    .config = config,
                    .config_dir = dir,
                    .db = db_under(dir)};
            }
            if (configs.empty())
            {
                throw std::runtime_error(
                    std::format(
                        "no configuration found under {} - create one with `ccg index "
                        "--config <name> <project root>`",
                        root));
            }
            if (configs.size() > 1)
            {
                std::string joined;
                for (const std::string& c : configs)
                {
                    joined += joined.empty() ? c : ", " + c;
                }
                throw std::runtime_error(
                    std::format(
                        "multiple configurations under {}: {} - pass --config <name> "
                        "(or point $CCG_DB at one configuration directory)",
                        root,
                        joined));
            }
            const std::string config = configs.front();
            const std::string dir = config_dir_under(root, config);
            return {
                .root = root,
                .config = config,
                .config_dir = dir,
                .db = db_under(dir)};
        }

        /** @brief 查询侧发现的安静形态（hook 处理器）：解析失败返回空而非抛出。
         */
        [[nodiscard]]
        inline auto try_resolve_query_layout(const Invocation& inv)
            -> std::optional<LayoutChoice>
        {
            try
            {
                return resolve_query_layout(inv);
            }
            catch (const std::exception&)
            {
                return std::nullopt;
            }
        }

        /**
         * @brief index 侧布局：--config 必填（parse 期已保证），产出文件夹不得
         *        本身是配置目录。
         */
        [[nodiscard]]
        inline auto resolve_index_layout(const Invocation& inv) -> LayoutChoice
        {
            const std::string root = resolve_root_dir(inv);
            std::error_code   ec;
            if (fs::is_regular_file(fs::path{root} / db_file_name, ec))
            {
                const std::string config =
                    fs::path{root}.filename().generic_string();
                throw std::runtime_error(
                    std::format(
                        "the folder '{}' is itself the configuration '{}' (it holds "
                        "ccg.db) - pass the PARENT folder to -o so the new "
                        "configuration lands beside it",
                        root,
                        config));
            }
            const std::string dir = config_dir_under(root, inv.config_name);
            return {
                .root = root,
                .config = inv.config_name,
                .config_dir = dir,
                .db = db_under(dir)};
        }

        /**
         * @brief 配置档案：首次 index 记录全部输入，简单复跑（`ccg index
         *        --config <名>`）据此恢复（裁量 c）。
         *
         * 命令行与档案冲突时命令行优先并回写档案（save 侧由调用方落实）。
         */
        struct ConfigProfile
        {
            std::string              config; // 配置名
            std::vector<std::string> roots;  // 项目根（绝对规范化）
            std::string
                compile_commands; // 记录的编译数据库路径（""=发现/回退）
            std::string
                build_config;      // 拆分语义下的内部构建配置签名（""=单配置）
            std::string llvm_root; // 创建时的 --llvm-root（""=未给）
            std::string engine_path;    // 引擎身份：已解析库路径
            std::string engine_version; // 引擎身份：clang_getClangVersion
            std::vector<std::string>
                        argv;        // 创建参数摘要（复现本次运行的有效旗标）
            std::string created_utc; // 档案写入时刻（人读）
        };

        /** @brief
         * 读配置目录内的档案；缺失或畸形返回空（复跑按命令行参数回退）。 */
        [[nodiscard]]
        inline auto load_profile(const std::string& config_dir)
            -> std::optional<ConfigProfile>
        {
            namespace json = cg::util::json;
            std::error_code ec;
            const fs::path  path = fs::path{config_dir} / profile_file_name;
            if (!fs::is_regular_file(path, ec))
            {
                return std::nullopt;
            }
            try
            {
                const auto text = cg::util::read_text_file(path.string());
                if (!text.has_value())
                {
                    return std::nullopt;
                }
                const json::Document doc = json::Document::parse(*text);
                const auto           root = doc.root();
                ConfigProfile        p;
                const auto str_of = [&](const char* key) -> std::string
                {
                    const auto v = root.get(key);
                    if (v.has_value() && v->is_string())
                    {
                        if (const auto s = v->as_string())
                        {
                            return std::string{*s};
                        }
                    }
                    return {};
                };
                p.config = str_of("config");
                p.compile_commands = str_of("compile_commands");
                p.build_config = str_of("build_config");
                p.llvm_root = str_of("llvm_root");
                p.created_utc = str_of("created_utc");
                if (const auto roots = root.get("roots");
                    roots.has_value() && roots->is_array())
                {
                    for (std::size_t i = 0; i < roots->size(); ++i)
                    {
                        const auto e = roots->at(i);
                        if (e->is_string())
                        {
                            if (const auto s = e->as_string())
                            {
                                p.roots.push_back(std::string{*s});
                            }
                        }
                    }
                }
                if (const auto argv = root.get("argv");
                    argv.has_value() && argv->is_array())
                {
                    for (std::size_t i = 0; i < argv->size(); ++i)
                    {
                        const auto e = argv->at(i);
                        if (e->is_string())
                        {
                            if (const auto s = e->as_string())
                            {
                                p.argv.push_back(std::string{*s});
                            }
                        }
                    }
                }
                if (const auto engine = root.get("engine");
                    engine.has_value() && engine->is_object())
                {
                    const auto path_v = engine->get("path");
                    const auto version_v = engine->get("version");
                    if (path_v.has_value() && path_v->is_string())
                    {
                        if (const auto s = path_v->as_string())
                        {
                            p.engine_path = std::string{*s};
                        }
                    }
                    if (version_v.has_value() && version_v->is_string())
                    {
                        if (const auto s = version_v->as_string())
                        {
                            p.engine_version = std::string{*s};
                        }
                    }
                }
                return p;
            }
            catch (const std::exception&)
            {
                return std::nullopt;
            }
        }

        /** @brief 写配置档案（目录不存在则创建；失败抛出）。 */
        inline void save_profile(
            const std::string&   config_dir,
            const ConfigProfile& p)
        {
            namespace json = cg::util::json;
            std::error_code ec;
            fs::create_directories(fs::path{config_dir}, ec);
            json::Object o;
            o.set("config", p.config);
            json::Array roots;
            for (const std::string& r : p.roots)
            {
                roots.add(r);
            }
            o.set("roots", std::move(roots));
            o.set("compile_commands", p.compile_commands);
            o.set("build_config", p.build_config);
            o.set("llvm_root", p.llvm_root);
            json::Object engine;
            engine.set("path", p.engine_path);
            engine.set("version", p.engine_version);
            o.set("engine", std::move(engine));
            json::Array argv;
            for (const std::string& a : p.argv)
            {
                argv.add(a);
            }
            o.set("argv", std::move(argv));
            o.set(
                "created_utc",
                std::format(
                    "{:%Y-%m-%dT%H:%M:%SZ}",
                    std::chrono::floor<std::chrono::seconds>(
                        std::chrono::system_clock::now())));
            std::ofstream out{
                fs::path{config_dir} / profile_file_name,
                std::ios::binary | std::ios::trunc};
            if (!out)
            {
                throw std::runtime_error(
                    "cannot write profile under " + config_dir);
            }
            out << json::stringify(std::move(o));
        }

    } // namespace layout

} // namespace cg::cli
