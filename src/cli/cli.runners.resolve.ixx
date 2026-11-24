/**
 * @module cg.cli.runners.resolve
 *
 * @brief 编译命令解析域：编译库装载与缺失诊断、按构建配置过滤。
 */
export module cg.cli.runners.resolve;

import std;
import cg.util;
import cg.clang.compile_db.model;
import cg.clang.compile_db.bmi;
import cg.clang.bmi;
import cg.clang.pch.model;
import cg.clang.pch.plan;
import cg.cli.parse.model;
import cg.cli.layout;
import cg.cli.runners.engine;

export namespace cg::cli
{

    namespace detail
    {

        // 前置声明（定义在下方）。
        [[nodiscard]]
        inline auto resolve_loaded_commands(
            const Invocation&                             inv,
            const std::vector<std::string>&               roots,
            std::vector<cg::clang::CompileCommand>        commands,
            const std::vector<cg::clang::CompileCommand>& coverage,
            const std::string& db_file) -> ResolvedCommands;

        /**
         * @brief 缺失编译数据库时的可行动错误文案。
         *
         * 两行之外的每条提示都是一条可执行的修复路径：让构建自己导出数据库、
         * 用 MSBuild 适配器索引解决方案、或显式点名数据库文件。
         */
        [[nodiscard]]
        inline auto missing_compile_db_message(
            const std::vector<std::string>& roots) -> std::string
        {
            const std::string root =
                roots.empty() ? std::string{"<root>"} : roots.front();
            return std::format(
                "no compile_commands.json found under {}\n"
                "  hint: have your build export one (CMake: "
                "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON)\n"
                "  hint: or index a Visual Studio solution with `ccg msbuild`\n"
                "  hint: or pass the database explicitly with "
                "`--compile-commands <file>`",
                root);
        }

        /**
         * @fn cg::cli::detail::resolve_loaded_commands
         * @brief 已加载命令清单的解析（index 拆分路线与 update 过滤后共用）。
         *
         * coverage 是 PCH 发现的额外基准：拆分路线只喂本配置的分片，update
         * 用全量清单做发现。MSBuild 没提到的文件一概不管（孤儿、条件排除、
         * 他配置的文件不扫不提——模块单元只能经编译数据库条目索引，缺条目
         * 的单元不构成 ccg 的行动项）。
         */
        [[nodiscard]]
        inline auto resolve_loaded_commands(
            const Invocation&                             inv,
            const std::vector<std::string>&               roots,
            std::vector<cg::clang::CompileCommand>        commands,
            const std::vector<cg::clang::CompileCommand>& coverage,
            const std::string& db_file) -> ResolvedCommands
        {
            ResolvedCommands out;
            out.commands = std::move(commands);
            // 旗标注入先于一切路由：记录 argv 携带注入旗标进入 cl/db 两条模块
            // 路由（BMI 生产与消费者解析同旗标世界）与 PCH 规划。
            apply_clang_compile_flags(inv, out.commands);
            route_cl_module_tus(inv, roots, out.commands, out.engine);
            route_db_module_tus(inv, roots, out.commands, out.engine);

            // PCH 影子规划：entry 级门控在任何 PCH 标记/ sidecar 存在时才进入。
            // update 用全量 coverage 做发现，且禁止 purge。
            const auto pch_data = [](const cg::clang::CompileCommand& c)
            { return c.pch_involved || c.pch_create || c.pch_use; };
            const std::string sidecar =
                db_file.empty()
                    ? std::string{}
                    : cg::util::norm_path(
                          (fs::path{db_file}.parent_path() / "pch.json")
                              .lexically_normal()
                              .generic_string());
            const bool pch_possible =
                std::ranges::any_of(out.commands, pch_data)
                || std::ranges::any_of(coverage, pch_data)
                || (!sidecar.empty() && fs::is_regular_file(fs::path{sidecar}));
            if (pch_possible)
            {
                // 影子必须由当前 libclang 的同源驱动器构建：先准备引擎，使
                // driver_info() 锚定在做解析的安装上。
                if (!out.engine.valid)
                {
                    out.engine =
                        prepare_index_engine(inv, read_db_engine_pin(inv));
                }
                cg::clang::PchOptions pch_opt;
                pch_opt.ccg_config_dir = layout::config_dir_of_db(inv.db_path);
                pch_opt.compile_db_path = db_file;
                pch_opt.sidecar_path = sidecar;
                // `--full` 是索引全量，不是影子缓存强制作废；只有显式 --force
                // 才重建影子（否则内容相同的重建会让全部消费者无谓重解析）。
                pch_opt.force = inv.force;
                pch_opt.dry_run = inv.dry_run;
                pch_opt.purge = inv.cmd == Invocation::Cmd::Index
                                && (inv.force || inv.full);
                out.pch = cg::clang::plan_pch(
                    std::move(out.commands),
                    coverage,
                    pch_opt);
                out.commands = std::move(out.pch.commands);
                out.pch.commands.clear();
                out.pch.commands.shrink_to_fit();
                for (const std::string& note : out.pch.notes)
                {
                    std::println(std::cerr, "ccg: note: {}", note);
                }
            }
            // 喂给恢复收尾（原公共尾部；模块单元计数提示已随功能退役——
            // MSBuild 没提的文件一概不管）。
            if (const std::size_t fed =
                    cg::clang::supply_module_feeds(out.commands);
                fed > 0)
            {
                std::println(
                    std::cerr,
                    "ccg: note: restored BMI feeds for {} module TU{} from "
                    "CXXModules.json (the .modmap channel was empty)",
                    fed,
                    fed == 1 ? "" : "s");
            }
            return out;
        }

        /**
         * @brief 按库记录的内部构建配置签名过滤命令清单（拆分库的 update 面）。
         *
         * 拆分语义下一库一配置：库 meta 的 build_config 记录其签名，
         * update 重载全量编译数据库时只保留本配置的条目（无签名条目随行——
         * 无法归属的条目对各配置都对）。空签名（单配置库）不过滤。
         */
        [[nodiscard]]
        inline auto filter_by_build_config(
            std::vector<cg::clang::CompileCommand> commands,
            const std::string&                     build_config)
            -> std::vector<cg::clang::CompileCommand>
        {
            if (build_config.empty())
            {
                return commands;
            }
            std::erase_if(
                commands,
                [&](const cg::clang::CompileCommand& c)
                {
                    const std::string sig = cg::clang::db_config_signature(c);
                    return !sig.empty() && sig != build_config;
                });
            return commands;
        }

    } // namespace detail
} // namespace cg::cli
