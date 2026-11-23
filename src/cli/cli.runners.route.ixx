module;
#include "../db/db.tables.h"

/**
 * @module cg.cli.runners.route
 *
 * @brief 模块 TU 路由域：clang 侧与数据库侧的模块翻译单元分派路由。
 */
export module cg.cli.runners.route;

import std;
import cg.util;
import cg.util.cstd;
import cg.db;
import cg.clang;
import cg.graph;
import cg.cli.parse;
import cg.cli.render;
import cg.cli.layout;
import cg.cli.runners.engine;

export namespace cg::cli
{

    namespace detail
    {

        /**
         * @brief cl 模式路由（`ccg index` 对 cl 模式编译数据库与
         *        `ccg msbuild` 共享）。
         *
         * MSVC 构建的 .ifc BMI 无法供 clang 引擎使用，故 cl 模式库的模块单元
         * 离开重放、改走自构建（clang-cl 生产 + 解析）。与 db 路线同机制
         * （"有 db 用记录 argv"原则）：模块单元记录的编译命令作为扫描/
         * BMI/解析三处的权威 argv 传入（SelfBuildOptions::recorded +
         * db_mode）， 其中的 MSVC
         * 模块开关（/interface、/ifcOutput、/reference…）由自建
         * 链的剥离层剥除，/reference 名单与扫描器结果取并集注入影子 pcm——
         * 记录命令里本来就有工程 /I，项目 include 随记录 argv自动在场。
         * 工具集钉定轴、msvc-stl std 供应链、-fms-compatibility-version 物化
         * 等既有 cl 适配保留复用；合成路径（cl_base_argv/cl_parse_argv）保留
         * 给无记录条目的单元与 std 生产作兜底。经典条目按 msbuild 波的行为
         * 原样保留并就地获得喂给（重放 argv 同样过剥离层）。先加载引擎：自
         * 构建的生产者锁与驱动器锚定都要读已加载引擎的身份（引擎先行的
         * 顺序不变式）。
         *
         * @internal
         * 模块 TU 记录的宏定义塑造其头文件（合成的解析必须复现它们，
         * 静默随行）。链的语言轴取自记录的 /std: 值（精确匹配纪律：经典
         * 条目逐字重放它们，故 pcm 必须以同一 std 构建）；最高记录值胜出，
         * 模块需要 >=c++20，故无任何 /std:（或低于 c++20）的库以 c++20 垫底。
         * 同一文件的多条库条目（两个目标建同一单元）：首条记录命令为权威
         * （与 db 路线同判定——重复重放会把同一文件解析两遍）。无可路由单元
         * 时恢复被移出的经典条目；此时若 cl 条目源文本携带头单元 import，
         * 本路线不支持该形态（自建链里的头单元降级通道整个未进场），先于
         * 索引以 warning 披露场景、后果与涉及的 TU（见下方检测块）。
         */
        inline void route_cl_module_tus(
            const Invocation&                       inv,
            const std::vector<std::string>&         roots,
            std::vector<cg::clang::CompileCommand>& commands,
            EnginePrepared&                         engine,
            const std::string&                      msvc_toolset_hint = {})
        {
            const bool any_cl = std::ranges::any_of(
                commands,
                [](const auto& c)
                {
                    return std::ranges::find(c.argv, "--driver-mode=cl")
                           != c.argv.end();
                });
            if (!any_cl)
            {
                return;
            }
            std::vector<std::string>                         module_files;
            std::map<std::string, cg::clang::CompileCommand> recorded;
            std::vector<cg::clang::CompileCommand>           classic;
            std::vector<std::string>                         cl_dirs;
            std::vector<std::string>                         cl_defines;
            for (cg::clang::CompileCommand& c : commands)
            {
                if (cg::clang::is_module_tu(c))
                {
                    const std::string norm = cg::util::norm_path(c.file);
                    if (std::ranges::find(module_files, norm)
                        == module_files.end())
                    {
                        module_files.push_back(norm);
                        recorded.emplace(norm, c);
                    }
                    for (std::size_t i = 0; i + 1 < c.argv.size(); ++i)
                    {
                        if (c.argv[i] == "/D"
                            && std::ranges::find(cl_defines, c.argv[i + 1])
                                   == cl_defines.end())
                        {
                            cl_defines.push_back(c.argv[i + 1]);
                        }
                    }
                    continue;
                }
                for (std::size_t i = 0; i + 1 < c.argv.size(); ++i)
                {
                    if ((c.argv[i] == "-imsvc" || c.argv[i] == "/I")
                        && std::ranges::find(cl_dirs, c.argv[i + 1])
                               == cl_dirs.end())
                    {
                        cl_dirs.push_back(c.argv[i + 1]);
                    }
                }
                classic.push_back(std::move(c));
            }
            if (module_files.empty())
            {
                // 纯头单元语料的可观测性披露：整库零命名模块时自建链不
                // 进场地，cl 条目文本书写的 `import "x.hpp";` 消费者对逐
                // 字重放的裸 argv 解析必败（引擎以原始报错拒绝未知头单
                // 元），而用户拿到报错无从联想是本路线暂不支持该形态。
                // 不修复、只告知：warning 一次讲清场景（MSVC 风格命令行
                // 语料含头单元 import）、后果（这些 TU 将解析失败）与两
                // 方言分叉（编译数据库路线已支持同形态——头单元 import
                // 被改写为文本 #include 后可解析）。检测复用既有判据、
                // 不新造词法：text_has_import 做行级预过滤（零 import 行
                // 的语料零词法扫描），命中者经头单元降级半边同源的词法
                // 扫描器确认头单元拼写。warning 只加可观测性，绝不改路
                // 由行为。
                std::vector<std::string> header_unit_consumers;
                std::string              first_header_unit_spelling;
                for (const cg::clang::CompileCommand& c : classic)
                {
                    if (std::ranges::find(c.argv, "--driver-mode=cl")
                            == c.argv.end()
                        || !cg::clang::text_has_import(c.file))
                    {
                        continue;
                    }
                    const std::string norm = cg::util::norm_path(c.file);
                    const std::vector<std::string> imports =
                        cg::clang::detail::text_header_unit_imports(c.file);
                    if (!imports.empty()
                        && std::ranges::find(header_unit_consumers, norm)
                               == header_unit_consumers.end())
                    {
                        header_unit_consumers.push_back(norm);
                        if (first_header_unit_spelling.empty()
                            && !imports.front().empty())
                        {
                            first_header_unit_spelling = imports.front();
                        }
                    }
                }
                if (!header_unit_consumers.empty())
                {
                    const std::string listed =
                        cg::util::join(header_unit_consumers, ", ");
                    // 示例拼写取首个实际检测到的头名（非固定示意），
                    // 后果措辞如实覆盖 `#if 0` 编译剔除形态——词法判据
                    // 看不见条件编译，被剔除的 import 不会真的失败。
                    std::println(
                        std::cerr,
                        "ccg: warning: this MSVC-style command-line corpus "
                        "contains header-unit imports (import \"{}\";) "
                        "in {} file{}: {} - the MSVC command-line route "
                        "cannot index header-unit imports yet, so these "
                        "translation units will fail to parse (unless the "
                        "import is compiled out); the "
                        "compile-database route (clang/gcc command lines) "
                        "already supports this shape by rewriting "
                        "header-unit imports into plain #include lines",
                        first_header_unit_spelling,
                        header_unit_consumers.size(),
                        header_unit_consumers.size() == 1 ? "" : "s",
                        listed);
                }
                commands = std::move(classic);
                return;
            }
            engine = prepare_index_engine(inv, read_db_engine_pin(inv));
            cg::clang::SelfBuildOptions self_build;
            self_build.root = roots.front();
            self_build.ccg_config_dir = layout::config_dir_of_db(inv.db_path);
            self_build.force = inv.force || inv.full;
            self_build.dry_run = inv.dry_run;
            self_build.cl_mode = true;
            self_build.cl_include_dirs = std::move(cl_dirs);
            self_build.cl_defines = std::move(cl_defines);
            self_build.msvc_toolset_hint = msvc_toolset_hint;
            self_build.db_mode = true;
            self_build.db_config_label = "cl";
            self_build.jobs = resolve_jobs(inv);
            for (auto& [file, rec] : recorded)
            {
                self_build.recorded.emplace(file, std::move(rec));
            }
            std::string recorded_std = "c++20";
            const auto  note_std = [&recorded_std](const std::string& a)
            {
                if (!a.starts_with("/std:c++"))
                {
                    return;
                }
                const std::string value = "c++" + a.substr(8);
                if (value == "c++latest")
                {
                    recorded_std = "c++2d";
                }
                else if (value > recorded_std && value >= "c++20")
                {
                    recorded_std = value;
                }
            };
            for (const cg::clang::CompileCommand& c : classic)
            {
                for (const std::string& a : c.argv)
                {
                    note_std(a);
                }
            }
            for (const auto& [file, rec] : self_build.recorded)
            {
                for (const std::string& a : rec.argv)
                {
                    note_std(a);
                }
            }
            self_build.std_value = recorded_std;
            cg::clang::SelfBuildResult built = cg::clang::self_build_modules(
                self_build,
                module_files,
                classic);
            for (const std::string& note : built.notes)
            {
                std::println(std::cerr, "ccg: note: {}", note);
            }
            commands = std::move(classic);
            commands.insert(
                commands.end(),
                built.commands.begin(),
                built.commands.end());
        }

        /**
         * @brief 编译数据库模块路由，clang/gcc 方言（数据库路线）。
         *
         * 记录命令（响应文件展开之后）不含模块喂给的模块单元（或携带导入的
         * 经典 TU）无法逐字重放（多配置生成器记录每个配置的命令，而盘上只有
         * 已构建配置的 .modmap rsp 文件与 BMI；导入随后遇不到 BMI，libclang
         * 以零诊断死 rc=4）。此类单元离开重放、改走自构建，以记录的 argv 为
         * 生产/解析权威（统一规则：模块的 BMI 生产命令就是其权威命令——取
         * 其记录的 argv；无记录条目的单元才用基线旗标向量）。
         * 喂给齐备的条目（已构建配置）留在逐字重放上，无撰写导入的经典 TU
         * 永不被碰。在 cl 方言路由之后运行：那条路线留下的按构造即非
         * cl 方言。每个构建配置一次自构建（按记录 .modmap 路径的
         * ".dir/`<Config>`/" 签名）：各配置记录的 argv 不同，其 BMI 变体分别
         * 生产与缓存、绝不喂给另一配置的消费者。
         *
         * @internal
         * 逐配置的记录命令权威覆盖库中全部模块单元——不只是被路由的无喂给
         * 者：一次 pass 构建其单元的依赖闭包，而本身带喂给重放的依赖（已构建
         * 配置自己的条目）在无喂给兄弟导入它时仍在此产出其 pcm——用它的记录
         * argv，绝不用基线向量。前置拷贝：下方分区会把条目移出 commands。
         * 路由条目按构建配置签名与记录语言选项向量（PCM LangOpts 白名单，
         * cg::clang::pcm_langopt_vector）分组：同配置的单元可能记录不同的
         * 白名单语言选项，而一个 std pcm 恰好服务一种 LangOpts 形状——每个
         * （配置, 向量）组跑自己的自构建 pass 与自己的 std pcm 变体。全同
         * 向量的配置坍缩为统一旗标世界的单 pass；跨组 pcm 导入作为命名的
         * module-build-failed 行失败而非静默混形状；失败组的 std 供给只降级
         * 该组（每组一 pass，组间绝不共享状态）。变体计数护栏：健康的树记录
         * 少量向量（每配置一个乘以每个真实旗标分叉一个——CMake 构建被其单一
         * 合成 std 目标锁成全同）。散布众多向量的库几乎总是陈旧或手改的，
         * 而每个向量花一次完整 std pcm 构建加一个缓存槽；仅劝告性提示——
         * 绝不阻塞，只诚实陈述代价。同配置同文件的两条库条目（两个目标建
         * 同一单元）：首条记录命令为
         * 权威；重复重放会把同一文件解析两遍——丢弃（索引器自身的
         * （文件, 摘要）去重同理）。链的语言轴取自该配置命令记录的 -std 值
         * （std pcm 与无权威单元的基线对齐它）；最高记录值胜出、以 c++20
         * 垫底（模块语法最低），gnu++ 拼写映射到 c++ 孪生。经典条目被就地
         * 增强（喂给追加进其记录 argv）。
         */
        /**
         * @brief 路由分组的复合标签：构建配置签名，白名单非空时附加
         * langopt 后缀。
         */
        inline auto route_group_label(
            const std::string&              config,
            const std::vector<std::string>& langopts) -> std::string
        {
            if (langopts.empty())
            {
                return config;
            }
            std::string joined;
            for (const std::string& flag : langopts)
            {
                joined += joined.empty() ? flag : " " + flag;
            }
            return std::format("{}~langopt({})", config, joined);
        }

        inline void route_db_module_tus(
            const Invocation&                       inv,
            const std::vector<std::string>&         roots,
            std::vector<cg::clang::CompileCommand>& commands,
            EnginePrepared&                         engine)
        {
            std::vector<cg::clang::CompileCommand> keep;
            std::vector<cg::clang::CompileCommand> routed;
            std::map<
                std::string,
                std::map<std::string, cg::clang::CompileCommand>>
                recorded_by_config;
            for (const cg::clang::CompileCommand& c : commands)
            {
                if (cg::clang::is_module_tu(c))
                {
                    recorded_by_config[cg::clang::db_config_signature(c)]
                        .emplace(cg::util::norm_path(c.file), c);
                }
            }
            for (cg::clang::CompileCommand& c : commands)
            {
                const bool cl = std::ranges::find(c.argv, "--driver-mode=cl")
                                != c.argv.end();
                const bool moduleish = cg::clang::is_module_tu(c)
                                       || cg::clang::text_has_import(c.file);
                if (!cl && c.module_feeds.empty() && moduleish)
                {
                    routed.push_back(std::move(c));
                }
                else
                {
                    keep.push_back(std::move(c));
                }
            }
            commands = std::move(keep);
            if (routed.empty())
            {
                return;
            }
            engine = prepare_index_engine(inv, read_db_engine_pin(inv));

            struct RouteGroup
            {
                std::string              config;   // 构建配置签名
                std::vector<std::string> langopts; // 本组的白名单向量
                std::vector<cg::clang::CompileCommand> entries;
            };
            std::map<std::string, RouteGroup> groups; // 键：复合标签
            for (cg::clang::CompileCommand& c : routed)
            {
                const std::string config = cg::clang::db_config_signature(c);
                std::vector<std::string> langopts =
                    cg::clang::pcm_langopt_vector(c.argv);
                const std::string label = route_group_label(config, langopts);
                RouteGroup&       g = groups[label];
                g.config = config;
                g.langopts = std::move(langopts);
                g.entries.push_back(std::move(c));
            }
            if (groups.size() > 8)
            {
                std::println(
                    std::cerr,
                    "ccg: note: {} std pcm variants (language-option "
                    "groups) in this compile db - each variant builds and "
                    "caches its own std pcm; expected only a few, so check "
                    "for a stale or split-flags database",
                    groups.size());
            }

            std::size_t routed_count = routed.size();
            for (auto& [label, group] : groups)
            {
                std::vector<std::string>               module_files;
                std::vector<cg::clang::CompileCommand> classic;
                cg::clang::SelfBuildOptions            self_build;
                for (cg::clang::CompileCommand& c : group.entries)
                {
                    if (cg::clang::is_module_tu(c))
                    {
                        const std::string norm = cg::util::norm_path(c.file);
                        if (std::ranges::find(module_files, norm)
                            == module_files.end())
                        {
                            module_files.push_back(norm);
                            continue;
                        }
                        --routed_count;
                        continue;
                    }
                    classic.push_back(std::move(c));
                }
                if (const auto rit = recorded_by_config.find(group.config);
                    rit != recorded_by_config.end())
                {
                    for (const auto& [file, cc] : rit->second)
                    {
                        self_build.recorded.emplace(file, cc);
                    }
                }
                self_build.db_config_label = label;
                self_build.langopt_axes = group.langopts;
                if (module_files.empty() && classic.empty())
                {
                    continue;
                }
                self_build.root = roots.front();
                self_build.ccg_config_dir =
                    layout::config_dir_of_db(inv.db_path);
                self_build.force = inv.force || inv.full;
                self_build.dry_run = inv.dry_run;
                self_build.db_mode = true;
                self_build.jobs = resolve_jobs(inv);
                std::string recorded_std = "c++20";
                const auto  note_std = [&recorded_std](const std::string& a)
                {
                    std::string value;
                    if (a.starts_with("-std=c++"))
                    {
                        value = "c++" + a.substr(8);
                    }
                    else if (a.starts_with("-std=gnu++"))
                    {
                        value = "c++" + a.substr(10);
                    }
                    else
                    {
                        return;
                    }
                    if (value > recorded_std && value >= "c++20")
                    {
                        recorded_std = value;
                    }
                };
                for (const cg::clang::CompileCommand& c : classic)
                {
                    for (const std::string& a : c.argv)
                    {
                        note_std(a);
                    }
                }
                for (const auto& [file, rec] : self_build.recorded)
                {
                    for (const std::string& a : rec.argv)
                    {
                        note_std(a);
                    }
                }
                self_build.std_value = recorded_std;

                cg::clang::SelfBuildResult built =
                    cg::clang::self_build_modules(
                        self_build,
                        module_files,
                        classic);
                for (const std::string& note : built.notes)
                {
                    std::println(std::cerr, "ccg: note: {}", note);
                }
                commands.insert(
                    commands.end(),
                    built.commands.begin(),
                    built.commands.end());
                commands.insert(
                    commands.end(),
                    std::make_move_iterator(classic.begin()),
                    std::make_move_iterator(classic.end()));
            }
            if (routed_count > 0)
            {
                std::println(
                    std::cerr,
                    "ccg: note: routed {} module/importing TU{} without "
                    "recorded BMI feeds through the self-build (compile-db "
                    "route, recorded argv authority)",
                    routed_count,
                    routed_count == 1 ? "" : "s");
            }
        }

    } // namespace detail
} // namespace cg::cli
