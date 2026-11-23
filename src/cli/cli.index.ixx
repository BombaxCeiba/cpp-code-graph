module;
#include "../db/db.tables.h"

/**
 * @module cg.cli.index
 *
 * @brief `ccg index` 运行器：集中布局下的配置解析、
 *        档案复跑与多配置拆分。
 *
 * @internal
 * 曾为实现单元；模块布局扩展改为接口模块——无 BMI 单元触发 C2801 的规避
 * 手法随之退役（cmake/CcgC2801Workaround.cmake 持有该修复）。共享索引
 * 脊柱（引擎准备、命令清单解析、索引尾部）在 cg.cli.runners；本模块
 * 持有：产出文件夹+--config 的布局解析、profile.json 的记录与恢复、
 * 编译数据库按内部构建配置签名的拆分（一配置一目录一库，无例外）、
 * 逐配置经共享尾部落库。db.tables.h 置于全局模块片段：本单元导入
 * cg.db，MSVC 要求 sqlpp23 表模型定义在此可见，以合并模块记录的模板
 * 特化（完整机理见 db.tables.h）。
 */
export module cg.cli.index;

import std;
import cg.util;
import cg.db;
import cg.clang;
import cg.graph;
import cg.cli.parse;
import cg.cli.render;
import cg.cli.runners;
import cg.cli.layout;

export namespace cg::cli
{

    namespace detail
    {
        namespace fs = std::filesystem;

        /** @brief 是常规文件时给其规范化字符串路径，否则空。 */
        [[nodiscard]]
        auto regular_file_path(const fs::path& p) -> std::optional<std::string>
        {
            std::error_code ec;
            if (fs::is_regular_file(p, ec))
            {
                return p.lexically_normal().generic_string();
            }
            return std::nullopt;
        }

        /**
         * @brief 在一个根下搜索 compile_commands.json：根本身、然后
         * root/build、 再做一层浅扫描。
         */
        [[nodiscard]]
        auto find_compile_db(const std::string& root)
            -> std::optional<std::string>
        {
            const fs::path base{root};
            if (auto f = regular_file_path(base / "compile_commands.json"))
            {
                return f;
            }
            if (auto f =
                    regular_file_path(base / "build" / "compile_commands.json"))
            {
                return f;
            }
            std::error_code ec;
            for (const fs::directory_entry& entry :
                 fs::directory_iterator(base, ec))
            {
                if (!entry.is_directory(ec))
                {
                    continue;
                }
                if (auto f = regular_file_path(
                        entry.path() / "compile_commands.json"))
                {
                    return f;
                }
            }
            return std::nullopt;
        }

        /**
         * @brief 签名是否形似构建配置名（漏拆 advisory 的判别材料）。
         *
         * @internal
         * 拆分判别式（同文件跨签名重复）对"各配置源集完全不相交"的多配置库
         * 会漏拆——现实生成器（Ninja Multi-Config/VS）源集恒重叠，但兜底提示
         * 仍有价值：签名命中标准配置名（Debug/Release/RelWithDebInfo/
         * MinSizeRel，含自定义变体，忽略大小写与前后缀）时不拆分也输出
         * advisory note。源子路径偶与配置名形似只是多一条 stderr 提示，
         * 不改变裁决本身。
         */
        [[nodiscard]]
        auto looks_like_config_name(const std::string& sig) -> bool
        {
            const std::string lower = cg::util::ascii_lower(sig);
            return lower.find("debug") != std::string::npos
                   || lower.find("release") != std::string::npos
                   || lower.find("relwithdebinfo") != std::string::npos
                   || lower.find("minsizerel") != std::string::npos;
        }

        /**
         * @brief 编译数据库的内部配置分片（多配置拆分的裁决材料）。
         *
         * @internal
         * 签名沿用 cg::clang::db_config_signature（".dir/" 后的路径段）。单配置
         * 生成器（Ninja 等）的 ".dir/" 后是源子路径而非配置——那种"多签名"是
         * 噪声：真多配置库的判别式是同一文件以不同签名重复出现（一文件一配置
         * 一条目）。仅当存在跨签名重复的文件时才拆分；否则整库单配置（签名
         * 视为 ""，避免把源目录误当配置）。无签名条目（无法归属）在拆分时随
         * 每个配置同行——各库对其索引，绝不静默丢弃。多签名但不拆分、且任一
         * 签名形似配置名时，unsplit_signatures 记下全部签名供调用方出
         * advisory note（各配置源集不相交的多配置库是判别式的理论盲区）。
         */
        struct ConfigSplit
        {
            bool        split = false;    // 多配置：按签名各建一库
            std::string single_signature; // 单配置时的签名（可为 ""）
            std::vector<std::string>
                unsplit_signatures; // 不拆但形似配置的签名集
            std::map<std::string, std::vector<cg::clang::CompileCommand>> parts;
        };

        [[nodiscard]]
        auto split_by_build_config(
            const std::vector<cg::clang::CompileCommand>& commands)
            -> ConfigSplit
        {
            ConfigSplit                                            out;
            std::set<std::string>                                  signatures;
            std::unordered_map<std::string, std::set<std::string>> sigs_of_file;
            for (const cg::clang::CompileCommand& c : commands)
            {
                const std::string sig = cg::clang::db_config_signature(c);
                if (!sig.empty())
                {
                    signatures.insert(sig);
                    sigs_of_file[cg::util::norm_path(c.file)].insert(sig);
                }
            }
            if (signatures.size() <= 1)
            {
                out.single_signature =
                    signatures.empty() ? std::string{} : *signatures.begin();
                return out;
            }
            bool repeated = false;
            for (const auto& [file, sigs] : sigs_of_file)
            {
                if (sigs.size() >= 2)
                {
                    repeated = true;
                    break;
                }
            }
            if (!repeated)
            {
                // 单配置生成器的 .dir 噪声：签名不代表配置，整库单配置。若任一
                // 签名形似配置名，记下全部签名——各配置源集不相交的多配置库过
                // 不了重复文件判别式，至少让用户看见这层不确定性。
                for (const std::string& sig : signatures)
                {
                    if (looks_like_config_name(sig))
                    {
                        out.unsplit_signatures.assign(
                            signatures.begin(),
                            signatures.end());
                        break;
                    }
                }
                return out;
            }
            out.split = true;
            std::vector<cg::clang::CompileCommand> unattributed;
            for (const cg::clang::CompileCommand& c : commands)
            {
                const std::string sig = cg::clang::db_config_signature(c);
                if (!sig.empty())
                {
                    out.parts[sig].push_back(c);
                }
                else
                {
                    unattributed.push_back(c);
                }
            }
            for (auto& [name, part] : out.parts)
            {
                static_cast<void>(name);
                for (const cg::clang::CompileCommand& c : unattributed)
                {
                    part.push_back(c);
                }
            }
            return out;
        }

        /**
         * @brief 一次落库：布局解析后的配置目录 + 命令分片经共享索引尾部写入，
         *        随后记 meta 与配置档案。
         *
         * @internal
         * run_inv 进入本函数前已带本配置的 db 路径（解析期锚定 BMI 缓存）。
         * build_config 记入库 meta（update 的过滤签名）与档案；引擎身份从尾部
         * 落库后的 meta 读回（run_index_tail 已写 llvm_engine_path/version）。
         * 档案的有效旗标（复现本库的输入）从 effective 调用组装：根、
         * --compile-commands、--llvm-root、--jobs 与全量旗标。
         * --dry-run 在此短路：未经 resolve_loaded_commands（引擎加载、模块自
         * 建、PCH 规划都是重活——大仓实测 27 分钟），原始加载的命令清单直接
         * 进索引尾部的干跑计划（只读库的新鲜度判定，毫秒级）。
         */
        void index_into(
            const Invocation&               run_inv,
            const std::vector<std::string>& roots,
            const std::string&              db_file,
            const ResolvedCommands&         resolved,
            const std::string&              build_config,
            const Invocation&               effective)
        {
            if (run_inv.dry_run)
            {
                run_index_tail(
                    run_inv,
                    roots,
                    db_file,
                    resolved.commands,
                    EnginePrepared{},
                    nullptr);
                return;
            }
            run_index_tail(
                run_inv,
                roots,
                db_file,
                resolved.commands,
                resolved.engine,
                &resolved.pch);
            const std::string config_name = run_inv.config_name;
            const std::string config_dir =
                layout::config_dir_of_db(run_inv.db_path);
            layout::ConfigProfile profile;
            profile.config = config_name;
            profile.roots = roots;
            profile.compile_commands = db_file;
            profile.build_config = build_config;
            profile.llvm_root = effective.llvm_root;
            try
            {
                cg::db::GraphStore store{run_inv.db_path};
                if (build_config.empty())
                {
                    store.erase_meta("build_config");
                }
                else
                {
                    store.set_meta("build_config", build_config);
                }
                if (const auto p = store.meta("llvm_engine_path"))
                {
                    profile.engine_path = *p;
                }
                if (const auto v = store.meta("llvm_version"))
                {
                    profile.engine_version = *v;
                }
            }
            catch (const std::exception&)
            {
                // 引擎身份是档案的补充事实；读不回不影响复跑的正确性。
            }
            profile.argv = [&]
            {
                std::vector<std::string> argv{"--config", config_name};
                for (const std::string& root : roots)
                {
                    argv.push_back(root);
                }
                if (!effective.compile_db.empty())
                {
                    argv.insert(
                        argv.end(),
                        {"--compile-commands", effective.compile_db});
                }
                if (!effective.llvm_root.empty())
                {
                    argv.insert(
                        argv.end(),
                        {"--llvm-root", effective.llvm_root});
                }
                if (effective.jobs > 0)
                {
                    argv.insert(
                        argv.end(),
                        {"--jobs", std::to_string(effective.jobs)});
                }
                if (effective.full)
                {
                    argv.emplace_back("--full");
                }
                if (effective.paranoid)
                {
                    argv.emplace_back("--paranoid");
                }
                return argv;
            }();
            layout::save_profile(config_dir, profile);
        }

        /** @brief 解析结果为空时抛用法错误（库下无条目是可行动输入错）。 */
        void require_commands_non_empty(
            const ResolvedCommands&         resolved,
            const std::vector<std::string>& roots)
        {
            if (resolved.commands.empty())
            {
                throw UsageError{
                    std::format(
                        "no compile database entries under {} (looked for "
                        "compile_commands.json in the root, root/build, one "
                        "level deep; pass --compile-commands F)",
                        roots.front()),
                    usage_for(Invocation::Cmd::Index)};
            }
        }

        /** @brief 按配置名派生一配置一份的调用（db/根随配置目录）。 */
        [[nodiscard]]
        auto config_scoped_invocation(
            const Invocation&  effective,
            const LayoutChoice layout,
            const std::string& config_name) -> Invocation
        {
            Invocation per = effective;
            per.config_name = config_name;
            per.layout_root = layout.root;
            per.db_path = layout::db_under(
                layout::config_dir_under(layout.root, config_name));
            return per;
        }

        /**
         * @brief 一份配置分片的命令解析。
         *
         * @internal 干跑解析捷径：--dry-run 不走 resolve_loaded_commands
         * （引擎加载、模块自建扫描、影子 PCH 规划都是重活，实测大仓
         * 27 分钟无输出），原始加载的命令直接构成计划输入（resolve 的
         * 旗标注入对干跑计划无贡献——计划只看库内指纹与命令摘要）。
         */
        [[nodiscard]]
        auto resolve_part_commands(
            const Invocation&                             inv,
            const std::vector<std::string>&               roots,
            const std::vector<cg::clang::CompileCommand>& loaded,
            const std::string&                            db_file,
            const Invocation&                             per,
            std::vector<cg::clang::CompileCommand> part) -> ResolvedCommands
        {
            if (inv.dry_run)
            {
                ResolvedCommands out;
                out.commands = std::move(part);
                return out;
            }
            return resolve_loaded_commands(
                per,
                roots,
                std::move(part),
                loaded,
                db_file);
        }

        inline auto run_index(const Invocation& inv) -> int
        {
            const LayoutChoice layout = layout::resolve_index_layout(inv);

            // 档案恢复：命令行优先，缺席的输入从档案取回。
            std::optional<layout::ConfigProfile> profile =
                layout::load_profile(layout.config_dir);
            Invocation effective = inv;
            // db 路径在一切下游之前挂上：库钉窥探（read_db_engine_pin）与 BMI
            // 自建锚都从 inv.db_path 推导，解析期就得定。
            effective.db_path = layout.db;
            effective.layout_root = layout.root;
            effective.config_name = layout.config;
            if (profile.has_value())
            {
                if (effective.positional.empty() && !profile->roots.empty())
                {
                    effective.positional = profile->roots;
                }
                if (effective.compile_db.empty()
                    && !profile->compile_commands.empty())
                {
                    effective.compile_db = profile->compile_commands;
                }
                if (effective.llvm_root.empty() && !profile->llvm_root.empty())
                {
                    effective.llvm_root = profile->llvm_root;
                }
            }

            std::vector<std::string> roots;
            for (const std::string& raw : effective.positional)
            {
                std::error_code ec;
                const fs::path  abs = fs::absolute(fs::path{raw}, ec);
                if (ec || !fs::exists(abs, ec))
                {
                    throw std::runtime_error(
                        std::format("root path not found: {}", raw));
                }
                roots.push_back(
                    cg::util::norm_path(
                        abs.lexically_normal().generic_string()));
            }
            if (roots.empty())
            {
                throw UsageError{
                    std::format(
                        "index needs the project root directories (first run) - "
                        "after a configuration has been indexed once, its profile "
                        "under {} restores them for bare `ccg index --config {}` "
                        "re-runs",
                        layout.config_dir,
                        inv.config_name),
                    usage_for(Invocation::Cmd::Index)};
            }

            std::string db_file = effective.compile_db;
            if (db_file.empty())
            {
                for (const std::string& root : roots)
                {
                    if (auto found = find_compile_db(root))
                    {
                        db_file = *found;
                        break;
                    }
                }
            }

            // 编译数据库是唯一语料来源：C++ 不存在有意义的简单兜底（错图比
            // 缺图更危险），故无库的树一律是可行动的用法错误，绝不扫描。
            if (db_file.empty())
            {
                throw UsageError{
                    missing_compile_db_message(roots),
                    usage_for(Invocation::Cmd::Index)};
            }

            // 解析前就把 db 路径挂到本配置的调用上：BMI 自建锚
            // （SelfBuildOptions.ccg_config_dir）从 inv.db_path
            // 推导，命令路由期 （resolve_*）就得定。
            const std::vector<cg::clang::CompileCommand> loaded =
                cg::clang::load(db_file, roots);
            const ConfigSplit split = split_by_build_config(loaded);
            if (!split.split)
            {
                if (!split.unsplit_signatures.empty())
                {
                    std::string names;
                    for (const std::string& sig : split.unsplit_signatures)
                    {
                        if (!names.empty())
                        {
                            names += ", ";
                        }
                        names += sig;
                    }
                    std::println(
                        std::cerr,
                        "ccg: note: {} distinct '.dir/<seg>/' signatures ({}) share "
                        "no common file, so the database is treated as "
                        "single-configuration; if those segments are really build "
                        "configurations over disjoint source sets, index each from a "
                        "per-configuration compile database",
                        split.unsplit_signatures.size(),
                        names);
                }
                const Invocation per =
                    config_scoped_invocation(effective, layout, layout.config);
                const ResolvedCommands resolved = resolve_part_commands(
                    inv,
                    roots,
                    loaded,
                    db_file,
                    per,
                    loaded);
                require_commands_non_empty(resolved, roots);
                index_into(
                    per,
                    roots,
                    db_file,
                    resolved,
                    split.single_signature,
                    effective);
                return 0;
            }

            // 多配置编译数据库：按内部配置拆成多个配置目录各自成库。
            std::string names;
            for (const auto& [name, part] : split.parts)
            {
                if (!names.empty())
                {
                    names += ", ";
                }
                names += name;
            }
            std::println(
                std::cerr,
                "ccg: note: the compile database carries {} build "
                "configurations - splitting into one configuration "
                "directory per config under {} ({}); the passed --config "
                "'{}' applies to single-configuration databases",
                split.parts.size(),
                layout.root,
                names,
                inv.config_name);
            for (auto& [name, part] : split.parts)
            {
                const Invocation per =
                    config_scoped_invocation(effective, layout, name);
                const ResolvedCommands resolved = resolve_part_commands(
                    inv,
                    roots,
                    loaded,
                    db_file,
                    per,
                    std::move(part));
                require_commands_non_empty(resolved, roots);
                index_into(per, roots, db_file, resolved, name, effective);
            }
            return 0;
        }

    } // namespace detail

} // namespace cg::cli
