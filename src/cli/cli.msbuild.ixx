module;
#include "../db/db.tables.h"
#include "../msbuild/capture.tables.h"

/**
 * @module cg.cli.msbuild
 *
 * @brief `ccg msbuild` 提取运行器。
 *
 * @internal
 * 曾为实现单元；分区语法退役改为接口模块——无 BMI 单元触发 C2801 的规避
 * 手法随之退役（std BMI 宿主影子补丁已承载根治）。db.tables.h
 * 置于全局模块片段：本单元 导入 cg.db，MSVC 要求 sqlpp23
 * 表模型定义在此可见，以合并模块记录的 模板特化（完整机理见
 * db.tables.h）。capture.tables.h 同理：本单元 调用 cg.msbuild 的 capture
 * 读写（read_wrap_log 等导出内联函数，其
 * 语句构造在调用方单元实例化表模板），MSVC 要求 capture 表模型同样
 * 在本单元全局片段可见（机理与 db 侧同一探针矩阵结论）。
 */
export module cg.cli.msbuild;

import std;
import cg.util;
import cg.util.cstd;
import cg.db;
import cg.clang;
import cg.graph;
import cg.msbuild;
import cg.cli.parse;
import cg.cli.render;
import cg.cli.runners;
import cg.cli.layout;

export namespace cg::cli
{

    namespace detail
    {
        namespace fs = std::filesystem;
        namespace json = cg::util::json;

        /**
         * @brief `ccg msbuild --list`：列出注册表里逐配置的提取记录。
         *
         * 无注册表是运行时错误（先跑 'ccg msbuild `<project>`'）。与 `index`
         * 一致：允许存在失败 TU 时退出 0；运行时失败抛出（-> 退出 1）——
         * 吞掉退出码会让失败看似成功。
         */
        auto run_msbuild_list(
            const Invocation&  inv,
            const std::string& root,
            const std::string& registry_path) -> int
        {
            const auto registry = cg::msbuild::load_registry(registry_path);
            if (!registry)
            {
                throw std::runtime_error(
                    std::format(
                        "ccg msbuild: no registry at {} (run 'ccg msbuild <project>' "
                        "first; --list reads what a run wrote)",
                        registry_path));
            }
            if (inv.json)
            {
                json::Object out;
                out.set("root", root);
                out.set("default", registry->default_config);
                json::Array configs;
                for (const cg::msbuild::RegistryEntry& e : registry->entries)
                {
                    json::Object c;
                    cg::msbuild::registry_entry_to_json(e, c);
                    configs.add(std::move(c));
                }
                out.set("configs", std::move(configs));
                std::println("{}", json::stringify(std::move(out)));
                return 0;
            }
            std::println(
                "msbuild configurations under {} (default {})",
                root,
                registry->default_config.empty() ? std::string{"<none>"}
                                                 : registry->default_config);
            for (const cg::msbuild::RegistryEntry& e : registry->entries)
            {
                // mode 列：evaluate（默认求值）/record（对照真值臂）/
                // full-build；早于 mode 字段的旧条目显示 "?"。
                std::println(
                    "  {:<24} {:<10} tus {:<4} failed {:<3} db {}  extracted {}",
                    e.name,
                    e.mode.empty() ? std::string{"?"} : e.mode,
                    e.tus,
                    e.failed,
                    e.db,
                    e.extracted_at.empty() ? std::string{"?"} : e.extracted_at);
            }
            return 0;
        }

        /**
         * @brief 把 `ccg msbuild` 操作数解析为（输入, 项目根）。
         *
         * 提取只接受 .sln 文件：.vcxproj（单工程）与目录输入显式拒绝——
         * 可行动报错请用户传解决方案文件（暂不支持单工程输入）。--list 是
         * 纯注册表读：操作数只定位注册表根（目录即根；文件取其目录），缺省
         * 为 cwd。路径缺失是运行时错误；拒绝类是用法错误（退出 2）。
         */
        [[nodiscard]]
        auto resolve_msbuild_input(const std::string& operand, bool list_mode)
            -> std::pair<std::string, std::string>
        {
            std::error_code ec;
            const fs::path  abs = fs::absolute(fs::path{operand}, ec);
            if (ec || !fs::exists(abs, ec))
            {
                throw std::runtime_error("project path not found: " + operand);
            }
            const auto is_ext = [](const fs::path& p, std::string_view ext)
            { return cg::util::ascii_lower(p.extension().string()) == ext; };
            std::error_code sec;
            if (list_mode)
            {
                const fs::path anchor =
                    fs::is_directory(abs, sec) ? abs : abs.parent_path();
                const std::string root = cg::util::norm_path(
                    anchor.lexically_normal().generic_string());
                return {std::string{}, root};
            }
            if (fs::is_directory(abs, sec))
            {
                throw UsageError{
                    std::format(
                        "directory input is not supported - "
                        "pass the solution file (.sln): {}",
                        operand),
                    usage_for(Invocation::Cmd::Msbuild)};
            }
            if (is_ext(abs, ".vcxproj"))
            {
                throw UsageError{
                    std::format(
                        "single-project input is not supported "
                        "- pass the solution file (.sln): {}",
                        operand),
                    usage_for(Invocation::Cmd::Msbuild)};
            }
            if (!fs::is_regular_file(abs, sec) || !is_ext(abs, ".sln"))
            {
                throw UsageError{
                    std::format(
                        "msbuild input must be a .sln file: {}",
                        operand),
                    usage_for(Invocation::Cmd::Msbuild)};
            }
            const std::string input =
                cg::util::norm_path(abs.lexically_normal().generic_string());
            const std::string root = cg::util::norm_path(
                abs.parent_path().lexically_normal().generic_string());
            return {input, root};
        }

        /**
         * @brief `ccg msbuild` 运行器：逐配置走完整个提取管线。
         *
         * 管线（MSBuild 管线规格）：发现 MSBuild 与逐工程 cl，随后按通道
         * 三态分派——默认（无旗标）= 求值通道（evaluate）：对解决方案做
         * 工程求值（进程内装载 VS 自带 Microsoft.Build 引擎、枚举
         * ClCompile 条目与最终 metadata），argv 由 ccg 侧组装器合成，不跑
         * 构建、不桩化工具、无沙箱；--record 显式走记录管线（wrap 捕获的
         * 对照真值臂——测试/验证基础设施：跑包装的 -t:Rebuild、编译类工具
         * 全部桩化、生成器真跑、沙箱重定向 + 泄漏扫描）；--full-build 恢复
         * 真编（金标准对账逃生口）。三态共享骨架：输入/配置解析、
         * capture_claim 锁、工具链发现、翻译（adapt_cl/PCH 分类）、cdb 与
         * 旁车落盘、索引尾、注册表写回；中段（沙箱准备、run_extract、
         * 失败归类、wrap 日志读取、泄漏扫描）只属于构建通道，把日志翻译成
         * compile_commands.json、经共享索引尾部索引、把运行记入注册表。
         *
         * 求值失败语义（诚实失败，绝不静默换通道）：发现链不可推导（VS 根/
         * VCTargetsPath 推导失败）与通道级失败（垫片/CLR 不可用、错误码、
         * 看门狗超时、坏事件行）以及全工程失败都按失败保旧处理；部分工程
         * 失败继续（成功工程产物进组装、失败工程逐条 stderr 披露 + JSON
         * failed_projects 如实计数）；缺 import 的降级求值继续并披露 note
         * 行；冷检出的缺失源文件只做存在性披露、不拦截（索引侧 per-TU
         * 失败才是权威判定）。
         *
         * @internal
         * 默认级：显式选项 > 注册表默认 > Debug/x64（MSBuild 管线规格）；注册表
         * 损坏时即使只要默认值也高声抛出。未知条目名：词干在最后一个 '-'
         * 处拆分（平台名绝不含 '-'）。跨平台指令：提取管线按设计仅限
         * Windows（注册表/列表保持平台中立；此乃对一切会驱动 MSBuild 的路径
         * 的文档化不支持错误）。项目文件自设 CLToolExe/CLToolPath 时输给本次
         * 调用的 /p: 全局量——按 MSBuild 管线规格 警告。逐运行捕获日志用一次性
         * 记号名：MSBuild 的 tracker 把日志记进项目写 tlog，固定名会被后续
         * 项目的 Clean 在构建中途删掉（cg.msbuild.pipeline 细节）。全新捕获：
         * 早先运行的陈旧日志绝不混进本次（下方的空捕获检查只假设本次运行）；
         * 缓存目录首次运行可能尚不存在——那时修剪为空操作。诚实契约
         * （MSBuild 管线规格）按失败类分两半：exec 类（MSB3073/VCEnd——记录模
         * 式桩零写盘的预期产物）与 --full-build 的一切失败（wrapper 转发前已
         * 记录完整调用面，捕获可信）仍翻译已捕获的内容；记录模式的非 exec 类
         * 失败（评估/编排层坏：坏 XML、缺 import、MSBuild 崩溃）放弃本次语料
         * 刷新——cdb 不写、库不动、注册表不更新、退出码 1（失败保旧，绝无半
         * 库，联动触发方据此保留队列行）。非零退出被呈现、绝不吞掉。工具链发
         * 现优先复用注册表缓存（无显式覆盖/调参且路径仍在盘上时），失效即整
         * 轮重新发现。--export-only 在 cdb 后即停：它可
         * 直接供 clangd/cl-tidy 使用。cl 模式库的模块单元走自构建——
         * 与 `ccg index` 对 cl 模式 cdb 共享；工具集提示携带构建自身的工具集
         * 解析（VCToolsVersion 钉在记录的 argv 里不可见）。替换或追加使同根
         * 其他配置的条目保住、本配置绝不重复。详注打印拆成两次少参调用：
         * clang 23 的 std::format 实例化在本单元的 7 参 std::println 上确定性
         * ICE（MSVC 移植早期发现；拆分零代价，保留）。
         */
        inline auto run_msbuild(const Invocation& inv) -> int
        {
            const std::string operand = inv.positional.empty()
                                            ? std::string{"."}
                                            : inv.positional.front();
            const auto [input, root] =
                resolve_msbuild_input(operand, inv.msbuild_list);
            const std::string registry_path = root + "/.ccg/msbuild.json";

            if (inv.msbuild_list)
            {
                return run_msbuild_list(inv, root, registry_path);
            }

            std::vector<cg::msbuild::ConfigPlatform> configs;
            if (inv.msbuild_all)
            {
                configs = cg::msbuild::enumerate_configs(input);
                if (configs.empty())
                {
                    throw std::runtime_error(
                        "ccg msbuild: no Configuration|Platform declarations found "
                        "in "
                        + input);
                }
            }
            else
            {
                std::string config = inv.msbuild_config;
                std::string platform = inv.msbuild_platform;
                if (config.empty() || platform.empty())
                {
                    if (const auto registry =
                            cg::msbuild::load_registry(registry_path))
                    {
                        const std::string& want = registry->default_config;
                        if (!want.empty())
                        {
                            const auto entry = std::ranges::find_if(
                                registry->entries,
                                [&](const cg::msbuild::RegistryEntry& e)
                                { return e.name == want; });
                            if (entry != registry->entries.end())
                            {
                                if (config.empty())
                                {
                                    config = entry->configuration;
                                }
                                if (platform.empty())
                                {
                                    platform = entry->platform;
                                }
                            }
                            else
                            {
                                if (const std::size_t dash = want.rfind('-');
                                    dash != std::string::npos && dash > 0
                                    && dash + 1 < want.size())
                                {
                                    if (config.empty())
                                    {
                                        config = want.substr(0, dash);
                                    }
                                    if (platform.empty())
                                    {
                                        platform = want.substr(dash + 1);
                                    }
                                }
                            }
                        }
                    }
                }
                if (config.empty())
                {
                    config = "Debug";
                }
                if (platform.empty())
                {
                    platform = "x64";
                }
                configs.push_back(
                    cg::msbuild::ConfigPlatform{
                        .configuration = std::move(config),
                        .platform = std::move(platform)});
            }

#if !defined(_WIN32)
            static_cast<void>(input);
            throw std::runtime_error(
                "ccg msbuild: not supported on this platform - the MSBuild "
                "integration is Windows-only ('ccg msbuild --list' still works)");
#else
            if (const std::optional<std::string> text =
                    cg::msbuild::detail::read_text_file(input);
                text && text->find("CLToolExe") != std::string::npos)
            {
                std::println(
                    std::cerr,
                    "ccg msbuild: note: {} sets CLToolExe/CLToolPath; this "
                    "run's /p: overrides win for the invocation only",
                    input);
            }

            cg::msbuild::Registry registry;
            if (const auto existing = cg::msbuild::load_registry(registry_path))
            {
                registry = *existing;
            }

            // 布局并轨（裁量 b）：`<Config>`-`<Platform>` 成为配置目录名，db 与
            // compile_commands.json 入其内；注册表仍住 `<root>`/.ccg/
            // 根（跨配置 发现入口）；捕获日志是运行期工作区，留 .ccg/msbuild/
            // 下。
            const std::string layout_root =
                inv.db_explicit ? layout::absolute_norm_dir(inv.output_dir)
                                : cg::util::norm_path((fs::path{root} / ".ccg")
                                                          .lexically_normal()
                                                          .generic_string());

            // 通道三态分派（解析层已保证 --record 与 --full-build 互斥）：
            // 默认（无旗标）= 求值通道；--record 显式记录管线（对照真值
            // 臂，测试/验证设施）；--full-build 真编（金标准对账逃生口）。
            const bool full_build = inv.msbuild_full_build;
            const bool record_run = inv.msbuild_record;
            const bool evaluate   = !full_build && !record_run;
            // ExtractMode 只对构建通道有意义（求值分支不构造 ExtractParams）。
            const cg::msbuild::ExtractMode mode =
                full_build ? cg::msbuild::ExtractMode::FullBuild
                           : cg::msbuild::ExtractMode::Record;
            const std::string mode_text = full_build ? "full-build"
                                      : (record_run ? "record" : "evaluate");

            bool any_aborted = false;
            for (const cg::msbuild::ConfigPlatform& cp : configs)
            {
                const std::string stem = cg::msbuild::config_stem(cp);

                // capture_claim：同配置采集的长窗口互斥（MSBuild
                // 管线规格）。记录 模式一跑就是"清沙箱 + 逐 TU
                // 桩"的整轮，两个并发采集会互删日志 与互覆写 cdb；锁文件（pid +
                // 时间戳，CREATE_NEW 原子创建）与 update_claim
                // 同构——过期可偷、持有进程死亡即回收（自愈）。联
                // 动触发方在派生前只读探测同一把锁（util 的
                // file_lock_held_by_live_process），本处才是唯一获取点。锁住
                // `<root>`/.ccg/msbuild/ 下（捕获日志同级的运行期工作区）。生命
                // 周期=本轮循环体：翻译/索引/注册表全部完成后随析构释放；中途
                // continue/throw（真失败）同样释放，垂死残留按普通方式回收。
                std::error_code lock_dir_ec;
                fs::create_directories(
                    fs::path{root + "/.ccg/msbuild"},
                    lock_dir_ec);
                const std::string capture_lock_path =
                    root + "/.ccg/msbuild/capture-" + stem + ".lock";
                const cg::util::platform::FileLock capture_lock =
                    cg::util::platform::try_acquire_file_lock(
                        fs::path{capture_lock_path},
                        std::format(
                            "ccg-msbuild:pid:{}",
                            cg::util::platform::current_pid()),
                        900'000);
                if (!capture_lock.held())
                {
                    throw std::runtime_error(
                        std::format(
                            "ccg msbuild: another capture is running for {} (lock {} is "
                            "held by a live process) - retry when it finishes",
                            stem,
                            capture_lock_path));
                }

                std::string cdb_dir = root;
                cdb_dir += "/.ccg/msbuild/";
                cdb_dir += stem;
                const std::string config_dir =
                    layout::config_dir_under(layout_root, stem);
                const std::string cdb =
                    !inv.compile_db.empty()
                        ? inv.compile_db
                        : cg::util::norm_path(
                              (fs::path{config_dir} / "compile_commands.json")
                                  .lexically_normal()
                                  .generic_string());
                const std::string db = layout::db_under(config_dir);
                // 记录模式的捕获日志与沙箱/桩目录（求值通道全部不出现——
                // 不跑构建、不桩化工具，正是快的来源）：沙箱住配置目录
                // （.ccg 布局内、泄漏扫描的排除面），桩工具住捕获日志同级
                // 的瞬态区。
                const std::string log =
                    evaluate
                        ? std::string{}
                        : cg::msbuild::detail::new_capture_log_path(cdb_dir);
                const std::string sandbox_root =
                    record_run
                        ? cg::util::norm_path(
                              (fs::path{config_dir} / "msbuild-sandbox")
                                  .lexically_normal()
                                  .generic_string())
                        : std::string{};
                const std::string int_dir =
                    record_run ? sandbox_root + "/int/" : std::string{};
                const std::string out_dir =
                    record_run ? sandbox_root + "/out/" : std::string{};
                const std::string stub_tool_dir =
                    record_run ? cdb_dir + "/stub-tools" : std::string{};

                // 泄漏判定的基准尽早落锚（cl 解析的 MSBuild -getProperty 逐工程
                // 询问要数秒，快照若在其后就有一段"启动窗口盲区"——窗口内落进
                // 真实树的文件两边快照都在，新增/改写判定都看不见）。快照与金
                // 丝雀窗口起点都放询问之前：进程启动后任何时刻写进真实树的产
                // 物都在判定窗口内。
                std::vector<cg::msbuild::TreeFileState> tree_before;
                std::filesystem::file_time_type         run_start{};
                if (record_run)
                {
                    tree_before = cg::msbuild::snapshot_tree_states(
                        root,
                        {root + "/.ccg", layout_root});
                    run_start = std::filesystem::file_time_type::clock::now();
                }

                // 工具链发现缓存（项目文件事件联动）：注册表已存上次解析的 msbuild/cl
                // 绝对路径；路径仍在盘上、且本轮没有显式覆盖（--msbuild/--cl）
                // 与 MSBuild 调参（--msbuild-args 可能改写 VCToolsInstallDir 的
                // 求值结果）时直接复用，省掉 vswhere 与逐工程 -getProperty 的
                // 数秒。任何一环失效（路径消失、sln 成员对不上、注册表无该配
                // 置）即整轮重新发现，绝不半缓存。
                const cg::msbuild::RegistryEntry* prior = nullptr;
                {
                    const auto prior_slot = std::ranges::find(
                        registry.entries,
                        stem,
                        &cg::msbuild::RegistryEntry::name);
                    if (prior_slot != registry.entries.end())
                    {
                        prior = &*prior_slot;
                    }
                }
                std::string                         msbuild_exe;
                std::vector<cg::msbuild::ProjectCl> project_cls;
                bool                                discovery_cached = false;
                if (prior != nullptr && inv.msbuild_path.empty()
                    && inv.cl_path.empty() && inv.msbuild_args.empty())
                {
                    std::error_code cache_ec;
                    const bool msbuild_cached = !prior->msbuild.empty()
                                                && fs::is_regular_file(
                                                    fs::path{prior->msbuild},
                                                    cache_ec);
                    bool       cls_cached =
                        msbuild_cached && !prior->cl.empty()
                        && fs::is_regular_file(fs::path{prior->cl}, cache_ec);
                    std::vector<cg::msbuild::ProjectCl> rebuilt;
                    if (cls_cached)
                    {
                        // 成员事实是纯文本 sln 解析（无进程代价）：注册表只存
                        // 工程名与 cl，cl 映射的前缀键（dir）与披露字段
                        // （active_cfg/in_build）需从 sln 现场推导。
                        const std::vector<cg::msbuild::SlnProject> members =
                            cg::msbuild::enumerate_sln_projects(
                                input,
                                std::format(
                                    "{}|{}",
                                    cp.configuration,
                                    cp.platform));
                        const auto find_member =
                            [&members](
                                const std::string& name)
                            -> const cg::msbuild::SlnProject*
                        {
                            const auto member = std::ranges::find(
                                members,
                                name,
                                &cg::msbuild::SlnProject::name);
                            return member == members.end() ? nullptr
                                                           : &*member;
                        };
                        if (!prior->cl_projects.empty())
                        {
                            for (const cg::msbuild::ProjectClEntry& pc :
                                 prior->cl_projects)
                            {
                                if (pc.cl.empty())
                                {
                                    continue;
                                }
                                const cg::msbuild::SlnProject* member =
                                    find_member(pc.project);
                                if (member == nullptr
                                    || !fs::is_regular_file(
                                        fs::path{pc.cl},
                                        cache_ec))
                                {
                                    cls_cached = false;
                                    break;
                                }
                                cg::msbuild::ProjectCl e;
                                e.project = pc.project;
                                e.dir = cg::util::norm_path(
                                    fs::path{member->path}
                                        .parent_path()
                                        .lexically_normal()
                                        .generic_string());
                                e.cl = pc.cl;
                                e.resolution = pc.resolution;
                                e.active_cfg = member->active_cfg;
                                e.in_build = member->build;
                                rebuilt.push_back(std::move(e));
                            }
                        }
                        else
                        {
                            // 旧注册条目（逐工程记录之前的版本）：只有主 cl，
                            // 前缀键退化为解决方案目录（wrap 未命中时回退全局
                            // real_cl，语义等价）。
                            cg::msbuild::ProjectCl e;
                            e.project = fs::path{input}.stem().generic_string();
                            e.dir = cg::util::norm_path(
                                fs::path{input}
                                    .parent_path()
                                    .lexically_normal()
                                    .generic_string());
                            e.cl = prior->cl;
                            e.resolution = prior->cl_resolution;
                            rebuilt.push_back(std::move(e));
                        }
                    }
                    if (cls_cached && !rebuilt.empty())
                    {
                        msbuild_exe = prior->msbuild;
                        project_cls = std::move(rebuilt);
                        discovery_cached = true;
                    }
                }
                if (!discovery_cached)
                {
                    msbuild_exe = cg::msbuild::find_msbuild(inv.msbuild_path);
                    // 逐工程解析 cl（带配置的属性查询；.sln 按成员各归各——混合
                    // 工具集的版本感知主修）。
                    project_cls = cg::msbuild::resolve_project_cls(
                        msbuild_exe,
                        input,
                        cp.configuration,
                        cp.platform,
                        inv.cl_path,
                        inv.msbuild_args);
                }
                if (msbuild_exe.empty())
                {
                    throw std::runtime_error(
                        "MSBuild not found (Visual Studio + C++ workload required; "
                        "pass --msbuild PATH or set CCG_MSBUILD_EXE)");
                }
                std::string primary_cl;
                for (const cg::msbuild::ProjectCl& pc : project_cls)
                {
                    if (!pc.cl.empty())
                    {
                        primary_cl = pc.cl;
                        break;
                    }
                }
                if (primary_cl.empty())
                {
                    throw std::runtime_error(
                        "ccg msbuild: could not resolve cl.exe for " + stem
                        + " (tried MSBuild -getProperty:VCToolsInstallDir with "
                          "-p:Configuration/-p:Platform + bin/HostX86/"
                        + cg::msbuild::cl_target_dir(cp.platform)
                        + " join, then vswhere -find VC/Tools/MSVC; pass --cl PATH)");
                }
                const std::string cl_map =
                    cg::msbuild::build_cl_map(project_cls);
                if (inv.verbose)
                {
                    std::println(
                        std::cerr,
                        "ccg msbuild: {} [{}] -> db {}",
                        stem,
                        mode_text,
                        db);
                    if (evaluate)
                    {
                        std::println(std::cerr, "  cdb {}", cdb);
                    }
                    else
                    {
                        std::println(
                            std::cerr,
                            "  cdb {} (log {})",
                            cdb,
                            log);
                    }
                    std::println(std::cerr, "  msbuild {}", msbuild_exe);
                    if (discovery_cached)
                    {
                        std::println(
                            std::cerr,
                            "  toolchain discovery reused from the "
                            "registry ({}); paths probed, no re-discovery",
                            registry_path);
                    }
                    for (const cg::msbuild::ProjectCl& pc : project_cls)
                    {
                        if (pc.cl.empty())
                        {
                            std::println(
                                std::cerr,
                                "  cl <unresolved> for project {}",
                                pc.project);
                            continue;
                        }
                        std::println(
                            std::cerr,
                            "  cl {} (project {}, {})",
                            pc.cl,
                            pc.project,
                            pc.resolution.empty()
                                ? std::string{"unspecified"}
                                : pc.resolution
                                      + (pc.resolution == "vswhere-fallback"
                                             ? "; project pins not consulted"
                                             : ""));
                    }
                    if (record_run)
                    {
                        std::println(std::cerr, "  sandbox {}", sandbox_root);
                    }
                }

                std::error_code mkdir_ec;
                fs::create_directories(fs::path{cdb_dir}, mkdir_ec);
                fs::create_directories(fs::path{config_dir}, mkdir_ec);

                // —— 通道中段分派——求值分支产出组装的 ClInvocation；构建
                // 通道（record/full-build）的沙箱准备、包装提取、失败归类、
                // wrap 日志读取收进 !evaluate 守卫，泄漏扫描本就只在记录
                // 通道有意义。两分支产出统一的 invocations 进共享翻译段。
                std::vector<cg::msbuild::ClInvocation> invocations;
                std::size_t eval_project_total  = 0;
                std::size_t eval_project_failed = 0;
                // 刷新快照（内容哈希快照门）：求值分支在会话成功后构建，真
                // 正落盘在配置循环尾部（cdb 写出、索引尾、注册表条目全
                // 部成功之后）——失败保旧同步保快照，任何中途失败/异常
                // 都不会带着新快照留下旧语料。
                std::optional<cg::msbuild::eval::RefreshSnapshot>
                    pending_snapshot;
                cg::msbuild::WrapLog capture; // 构建通道填充；求值恒空
                if (evaluate)
                {
                    // —— 求值分支（默认通道）：发现链推导 → 会话驱动 →
                    // 组装器合成 argv。沙箱/桩/捕获日志/泄漏扫描全部不
                    // 出现：不跑构建、不桩化工具，这正是快的来源。——
                    const std::optional<cg::msbuild::eval::EvalRequest>
                        eval_request = cg::msbuild::eval::
                            build_evaluate_request(
                                msbuild_exe,
                                input,
                                cp.configuration,
                                cp.platform);
                    if (!eval_request.has_value())
                    {
                        // 发现链不可推导（VS 根回推 / VCTargetsPath 探测 /
                        // 版本段形态任一失败）：指引性报错（VCTargetsPath
                        // 失败时列出候选目录，探测失败的第一现场可读）
                        // + 失败保旧——--record 是用户显式取对照臂的动作，
                        // 绝不自动换通道。
                        const std::string vs_root =
                            cg::msbuild::eval::vs_root_of(msbuild_exe);
                        std::string hint;
                        if (vs_root.empty())
                        {
                            hint = std::format(
                                "cannot derive the VS installation root "
                                "from {} (path-shape walk and the vswhere "
                                "-property installationPath fallback both "
                                "failed)",
                                msbuild_exe);
                        }
                        else if (cg::msbuild::eval::vc_targets_dir_of(vs_root)
                                     .empty())
                        {
                            std::string candidates;
                            std::error_code list_ec;
                            for (const fs::directory_entry& d :
                                 fs::directory_iterator(
                                     fs::path{vs_root} / "MSBuild"
                                                       / "Microsoft" / "VC",
                                     list_ec))
                            {
                                const std::string name =
                                    d.path().filename().string();
                                if (name.size() < 2
                                    || (name[0] != 'v' && name[0] != 'V'))
                                {
                                    continue;
                                }
                                std::error_code props_ec;
                                if (!candidates.empty())
                                {
                                    candidates += ' ';
                                }
                                candidates += name;
                                candidates += fs::is_regular_file(
                                                 d.path()
                                                     / "Microsoft.Cpp."
                                                       "Default.props",
                                                 props_ec)
                                                 ? "(props-ok)"
                                                 : "(incomplete)";
                            }
                            hint = std::format(
                                "no VC targets directory under "
                                "{}/MSBuild/Microsoft/VC/ carries both "
                                "Microsoft.Cpp.Default.props and "
                                "Microsoft.Cpp.Common.props (candidates: "
                                "{})",
                                vs_root,
                                candidates.empty()
                                    ? std::string{"<none found>"}
                                    : candidates);
                        }
                        else
                        {
                            hint = std::format(
                                "cannot derive VisualStudioVersion from "
                                "the VS root layout {}",
                                vs_root);
                        }
                        std::println(
                            std::cerr,
                            "ccg msbuild: project refresh aborted; database "
                            "keeps the previous corpus (evaluation inputs "
                            "not derivable for {}: {} - evaluate is the "
                            "default channel; rerun with --record for the "
                            "wrapped-build capture)",
                            stem,
                            hint);
                        any_aborted = true;
                        continue;
                    }
                    if (inv.verbose)
                    {
                        std::println(
                            std::cerr,
                            "  vs root {}",
                            eval_request->vs_root);
                        std::println(
                            std::cerr,
                            "  vc targets {} (VS {})",
                            eval_request->vc_targets,
                            eval_request->vs_version);
                    }
                    const cg::msbuild::eval::EvalOutcome outcome =
                        cg::msbuild::eval::evaluate(*eval_request);
                    eval_project_total  = outcome.projects.size();
                    eval_project_failed = outcome.failed_projects;
                    // 工程级披露先于中止判定（通道级失败时已收到的工程
                    // 产物同样如实披露）：部分失败逐条 stderr；缺
                    // import 的降级求值 note 行。
                    for (const cg::msbuild::eval::EvalProjectOut& ep :
                         outcome.projects)
                    {
                        if (!ep.error.empty())
                        {
                            std::println(
                                std::cerr,
                                "ccg msbuild: eval project failure: {} ({})",
                                ep.name,
                                ep.error);
                        }
                        if (ep.degraded_missing_imports)
                        {
                            std::println(
                                std::cerr,
                                "ccg msbuild: note: evaluated with missing "
                                "imports ignored: {}",
                                ep.name);
                        }
                    }
                    if (!outcome.completed
                        || (outcome.ok_projects == 0
                            && outcome.failed_projects > 0))
                    {
                        // 通道级失败（垫片/CLR 不可用、错误码/看门狗
                        // 超时/坏事件行——error 携带装载诊断链）与全工程
                        // 失败（求值面整体不可信）同处置：失败保旧——
                        // 本配置 cdb 不写、库一行不动、注册表条目不更新，
                        // 退出 1 让联动触发方保留队列行；绝不静默换通道。
                        std::println(
                            std::cerr,
                            "ccg msbuild: project refresh aborted; database "
                            "keeps the previous corpus (evaluation failed "
                            "for {}: {})",
                            stem,
                            outcome.error.empty()
                                ? std::string{
                                      "every member project failed to "
                                      "evaluate"}
                                : outcome.error);
                        any_aborted = true;
                        continue;
                    }
                    cg::msbuild::compose::ComposeDiary compose_diary;
                    invocations = cg::msbuild::compose::compose_invocations(
                        outcome.projects,
                        project_cls,
                        cg::msbuild::compose::ComposeOptions{},
                        &compose_diary);
                    // 注册表披露的参与位与组装器过滤字面同源：求值通道以
                    // 会话回显覆写逐工程 active_cfg/in_build（compose 的
                    // build_in_solution 过滤即消费这同一份 EvalProjectOut；
                    // 构建通道沿用 resolve_project_cls 的现场解析）。
                    for (cg::msbuild::ProjectCl& pc : project_cls)
                    {
                        const auto echo = std::ranges::find(
                            outcome.projects,
                            pc.project,
                            &cg::msbuild::eval::EvalProjectOut::name);
                        if (echo == outcome.projects.end())
                        {
                            continue;
                        }
                        pc.active_cfg =
                            echo->active_platform.empty()
                                ? echo->active_cfg
                                : echo->active_cfg + "|"
                                      + echo->active_platform;
                        pc.in_build = echo->build_in_solution;
                    }
                    // 输入身份快照（哈希门的写侧素材）：文件哈希此刻与
                    // 求值所见同源；写回在循环尾部成功路径。
                    pending_snapshot = cg::msbuild::eval::
                        build_refresh_snapshot(
                            msbuild_exe,
                            primary_cl,
                            *eval_request,
                            outcome.projects);
                    if (inv.verbose)
                    {
                        for (const std::string& note : compose_diary.notes)
                        {
                            std::println(
                                std::cerr,
                                "ccg msbuild: {}",
                                note);
                        }
                        if (outcome.mismatches > 0)
                        {
                            std::println(
                                std::cerr,
                                "ccg msbuild: note: eval self-check "
                                "mismatches {} of {} field checks",
                                outcome.mismatches,
                                outcome.tu_field_checks);
                        }
                    }
                    // 冷检出存在性披露（仅披露不拦截）：生成的模块单元是
                    // 工程清单的显式 ClCompile 条目，求值照常看见；文件
                    // 缺失时索引侧 per-TU 失败才是权威判定，这里只提前
                    // 让人看懂（暖机检出零触发）。路径经宽形态中转——窄串
                    // 在 Windows 下按代码页误读非 ASCII 路径（夹具语料含
                    // ü 名目录）。
                    std::size_t missing_sources = 0;
                    for (const cg::msbuild::ClInvocation& ci : invocations)
                    {
                        if (ci.argv_utf8.empty())
                        {
                            continue;
                        }
                        std::error_code src_ec;
                        const fs::path  src{cg::util::platform::widen_utf8(
                            ci.argv_utf8.back())};
                        if (!fs::is_regular_file(src, src_ec))
                        {
                            ++missing_sources;
                        }
                    }
                    if (missing_sources > 0)
                    {
                        std::println(
                            std::cerr,
                            "ccg msbuild: note: evaluated {} TUs, {} "
                            "sources missing on disk (cold checkout? build "
                            "once so generators produce them)",
                            invocations.size(),
                            missing_sources);
                    }
                    // INCLUDE 同源披露：求值通道直读本进程 INCLUDE
                    // （与组装器同一次直读）。开发者提示符之外的 shell 里
                    // 它为空——记录通道的 wrap 捕获拿到的则是 MSBuild 子
                    // 进程内 VS 初始化过的完整 INCLUDE，两通道在冷 shell
                    // 下并不同源（实测：空环境父进程 + 记录通道捕获九条系
                    // 统 include 目录）。空值不拦截：cdb 照常产出，缺的是
                    // -imsvc 物化面，索引侧按 per-TU 失败如实呈现；此处只
                    // 给出可行动的修复路径。
                    if (cg::util::cstd::get_env("INCLUDE")
                            .value_or("")
                            .empty())
                    {
                        std::println(
                            std::cerr,
                            "ccg msbuild: note: INCLUDE is empty in this "
                            "process - the composed commands carry no "
                            "-imsvc system include materialization (the "
                            "wrapped build captures a VS-initialized "
                            "INCLUDE); run ccg from a VS developer prompt "
                            "or export INCLUDE");
                    }
                }

                if (!evaluate)
                {
                    // 构建通道（record/full-build）既有段照旧：日志修剪是
                    // wrap 捕获的工作区维护。
                    cg::msbuild::detail::prune_capture_logs(cdb_dir);

                    // 记录模式准备：清掉上一轮沙箱、建桩目录（真实树快照已在
                    // cl 解析前完成，见上；沙箱与桩目录都在泄漏扫描的排除面
                    // 内）。
                    if (record_run)
                    {
                        std::error_code rm_ec;
                        fs::remove_all(fs::path{sandbox_root}, rm_ec);
                        cg::msbuild::detail::make_stub_tool_dir(stub_tool_dir);
                    }

                    cg::msbuild::ExtractParams p;
                    p.msbuild = msbuild_exe;
                    p.input = input;
                    p.configuration = cp.configuration;
                    p.platform = cp.platform;
                    p.real_cl = primary_cl;
                    p.log_path = log;
                    p.cl_tool_exe = cg::msbuild::detail::current_exe_name();
                    p.cl_tool_path = cg::msbuild::detail::current_exe_dir();
                    p.extra_args = inv.msbuild_args;
                    p.mode = mode;
                    p.stub_tool_dir = stub_tool_dir;
                    p.int_dir = int_dir;
                    p.out_dir = out_dir;
                    p.cl_map = cl_map;
                    const cg::msbuild::ExtractRunResult extract =
                        cg::msbuild::run_extract(p);
                    const int msbuild_exit = extract.exit_code;
                    if (msbuild_exit != 0)
                    {
                        // 构建后步骤失败归类（tee 捕获的构建输出供判形）：
                        // MSB3073/exec 类（VCEnd/PostBuildEvent 找不到要复制
                        // 的产物）在记录模式属预期——替身零写盘，下游产物根本
                        // 不存在，复制/校验步骤必然失败；编译命令捕获不受影
                        // 响。退出码语义不变，仅提示归类。
                        const std::string build_text =
                            cg::util::ascii_lower(
                                extract.out + "\n" + extract.err);
                        const bool exec_failure =
                            build_text.find("msb3073") != std::string::npos
                            || (build_text.find("exited with code")
                                    != std::string::npos
                                && (build_text.find("postbuildevent")
                                        != std::string::npos
                                    || build_text.find("vcend")
                                           != std::string::npos));
                        if (exec_failure)
                        {
                            if (!full_build)
                            {
                                std::println(
                                    std::cerr,
                                    "ccg msbuild: note: MSBuild exited {} for "
                                    "{} on post-build step failures (MSB3073/"
                                    "exec class: VCEnd/PostBuildEvent copy or "
                                    "verify steps found their products missing "
                                    "- record-mode stubs write none; expected) "
                                    "- the captured compile commands are still "
                                    "translated and indexed",
                                    msbuild_exit,
                                    stem);
                            }
                            else
                            {
                                std::println(
                                    std::cerr,
                                    "ccg msbuild: MSBuild exited {} for {} - "
                                    "captured invocations are still translated "
                                    "and indexed",
                                    msbuild_exit,
                                    stem);
                            }
                        }
                        else if (!full_build
                                 && !inv.msbuild_allow_failure)
                        {
                            // --allow-failure：用户明示接受非零退出码（部署断
                            // 言/测试工程兜底检查等对沙盒不友好的自定义
                            // Target 是典型场景——编译命令捕获不受影响，捕获
                            // 数据可信）。失败保旧（联动契约，MSBuild 管线
                            // 规格）——仅记录模式：非 exec 类失败 = 项目评估/
                            // 编排层坏了（坏 XML、缺 import、 MSBuild 自身崩
                            // 溃），记录模式桩零写盘、本不该有构建期失败，此
                            // 时捕获内容不可信。放弃本配置的语料刷新——不写新
                            // cdb、库一行不动、注册表条目不更新；上一个完整好
                            // 态永远是库的当前态，绝无半库。退出码带 1 让联动
                            // 触发方把队列行保留到下一次事件重试。
                            std::println(
                                std::cerr,
                                "ccg msbuild: project refresh aborted; "
                                "database keeps the previous corpus (MSBuild "
                                "exited {} for {} - not an expected exec-class "
                                "failure)",
                                msbuild_exit,
                                stem);
                            any_aborted = true;
                            continue;
                        }
                        else
                        {
                            // --full-build 保持历史诚实契约：编译失败类（如
                            // #error TU）下 wrapper 已在转发前记录了完整调用
                            // 面，捕获可信，仍翻译已捕获内容（完整性门）。
                            // --allow-failure 走同一通 道，但额外回显
                            // MSBuild 原始输出（tee 的 stdout/stderr 后段）供
                            // 排查非编译类失败的具体原因。
                            std::println(
                                std::cerr,
                                "ccg msbuild: MSBuild exited {} for {} - "
                                "captured invocations are still translated "
                                "and indexed",
                                msbuild_exit,
                                stem);
                            if (inv.msbuild_allow_failure)
                            {
                                std::println(
                                    std::cerr,
                                    "ccg msbuild: --allow-failure: showing "
                                    "MSBuild raw output (last 80 lines):");
                                const std::string combined =
                                    extract.out + "\n" + extract.err;
                                std::size_t start = 0;
                                if (const std::size_t total =
                                        std::ranges::count(combined, '\n');
                                    total > 80)
                                {
                                    std::size_t seen = 0;
                                    for (std::size_t i = combined.size();
                                         i > 0;
                                         --i)
                                    {
                                        if (combined[i - 1] == '\n')
                                        {
                                            ++seen;
                                            if (seen == 80)
                                            {
                                                start = i;
                                                break;
                                            }
                                        }
                                    }
                                }
                                std::println(
                                    std::cerr,
                                    "{}",
                                    std::string_view{combined}.substr(start));
                            }
                        }
                    }

                    // 本运行 token 过滤：prune 删旧库是主路径，token 过滤是
                    // 纵深防御（prune 失败时上一轮的行绝不混进本次翻译）。
                    capture = cg::msbuild::read_wrap_log(
                        log,
                        cg::msbuild::detail::capture_token_of_log_path(log));
                    for (const std::string& err : capture.errors)
                    {
                        std::println(
                            std::cerr,
                            "ccg msbuild: wrap capture error: {}",
                            err);
                    }
                    invocations = std::move(capture.invocations);
                }

                // 沙箱泄漏扫描（仅记录通道——求值不建沙箱、full-build 产物
                // 落真树均不扫）：真实树新增/被改写的构建产物 → 具名
                // sandbox-leak 警告，摘要与 JSON 都如实标注，绝不静默。生成
                // 器正常产物（MIDL 的 tlb/生成头、CustomBuild 生成物）降级为
                // info 级汇总行——记录模式生成器真跑、产物按工程声明落真树，
                // 不是泄漏（真实 MSBuild 工程语料实测 164 条全为生成器正常
                // 产物）。改写形态覆盖"构建后步骤把产物复制到工程树既有文
                // 件"（新增 diff 抓不到）。
                std::vector<std::string> sandbox_leaks;
                std::vector<std::string> generated_sources;
                std::vector<std::string> generator_products;
                if (record_run)
                {
                    const std::vector<cg::msbuild::TreeFileState> tree_after =
                        cg::msbuild::snapshot_tree_states(
                            root,
                            {root + "/.ccg", layout_root});
                    const std::vector<std::string> fresh = [&]
                    {
                        std::vector<std::string> after;
                        for (const cg::msbuild::TreeFileState& s : tree_after)
                        {
                            after.push_back(s.rel);
                        }
                        std::vector<std::string> before;
                        for (const cg::msbuild::TreeFileState& s : tree_before)
                        {
                            before.push_back(s.rel);
                        }
                        return cg::msbuild::new_files_since(before, after);
                    }();
                    const std::vector<std::string> declared =
                        cg::msbuild::generator_declared_source_outputs(input);
                    const cg::msbuild::SandboxDiff diff =
                        cg::msbuild::classify_new_tree_files(
                            root,
                            fresh,
                            declared);
                    sandbox_leaks = diff.leaks;
                    generated_sources = diff.generated_sources;
                    generator_products = diff.generator_products;
                    // 改写面：声明过的生成器源码输出被重新生成属正常（真跑的
                    // 生成器），其余既有文件的改写计为泄漏（与新增面同白名单）。
                    const std::vector<std::string> overwritten =
                        cg::msbuild::overwritten_files_between(
                            tree_before,
                            tree_after);
                    if (!overwritten.empty())
                    {
                        const cg::msbuild::SandboxDiff rewrite_diff =
                            cg::msbuild::classify_new_tree_files(
                                root,
                                overwritten,
                                declared);
                        sandbox_leaks.insert(
                            sandbox_leaks.end(),
                            rewrite_diff.leaks.begin(),
                            rewrite_diff.leaks.end());
                        generator_products.insert(
                            generator_products.end(),
                            rewrite_diff.generator_products.begin(),
                            rewrite_diff.generator_products.end());
                    }
                    for (const std::string& leak : sandbox_leaks)
                    {
                        std::println(
                            std::cerr,
                            "ccg msbuild: sandbox-leak: {} (new or "
                            "overwritten build product in the project "
                            "tree outside {})",
                            leak,
                            sandbox_root);
                    }
                    // 金丝雀扩面：argv 声明的输出路径落在沙盒与 .ccg 之外时，
                    // 树差集看不见（根外硬编码输出静默逃逸实测）；树内路径与跑
                    // 前快照比对，树外路径退回运行窗口判定。no-op 语义下替身
                    // 零写盘，理论零触发——它只负责把"零写盘被破坏"叫出来。
                    const std::vector<std::string> reported_lower = [&]
                    {
                        std::vector<std::string> out;
                        for (const std::string& l : sandbox_leaks)
                        {
                            out.push_back(
                                cg::util::ascii_lower(
                                    cg::util::to_forward_slashes(
                                        root + "/" + l)));
                        }
                        return out;
                    }();
                    const std::vector<std::string> canary =
                        cg::msbuild::scan_declared_output_leaks(
                            cg::msbuild::capture_declared_outputs(capture),
                            tree_before,
                            root,
                            {sandbox_root, layout_root, root + "/.ccg"},
                            run_start,
                            reported_lower);
                    for (const std::string& leak : canary)
                    {
                        std::println(
                            std::cerr,
                            "ccg msbuild: sandbox-leak: {} (new or "
                            "overwritten build product outside {} at a "
                            "captured-argv output path)",
                            leak,
                            sandbox_root);
                    }
                    sandbox_leaks.insert(
                        sandbox_leaks.end(),
                        canary.begin(),
                        canary.end());
                    for (const std::string& gen : generated_sources)
                    {
                        std::println(
                            std::cerr,
                            "ccg msbuild: note: generated source in the "
                            "project tree (declared CustomBuild output): "
                            "{}",
                            gen);
                    }
                    // 生成器正常产物的 info 级汇总行：落在沙盒外但属生成器正
                    // 常产物（MIDL 的 tlb/生成头、CustomBuild 生成物）——不逐
                    // 条刷屏，一条汇总如实报数；真意外写入仍逐条报 leak。
                    if (!generator_products.empty())
                    {
                        std::println(
                            std::cerr,
                            "ccg msbuild: note: out-of-sandbox generator "
                            "products: {} (expected)",
                            generator_products.size());
                    }
                }

                std::vector<cg::msbuild::CdbEntry>          entries;
                std::vector<cg::msbuild::PchProducerRecord> pch_producers;
                std::vector<cg::msbuild::PchConsumerRecord> pch_consumers;
                std::vector<std::string>                    pch_not_using;
                for (const cg::msbuild::ClInvocation& ci : invocations)
                {
                    // 工具集版本不可解析时不追加兼容版本；verbose 下如实说明，
                    // 绝不静默假装精确。兼容版本按 INCLUDE 的逐 TU 工具集优先，
                    // 回退 real_cl，与 adapt_cl 的决议同源。
                    if (inv.verbose)
                    {
                        const std::string argv0 = ci.argv_utf8.empty()
                                                      ? std::string{}
                                                      : ci.argv_utf8[0];
                        if (cg::msbuild::detail::invocation_compat_version(
                                ci,
                                argv0)
                                .empty())
                        {
                            std::println(
                                std::cerr,
                                "ccg msbuild: note: cannot derive MSVC toolset "
                                "version from INCLUDE or real_cl ({}) - no "
                                "-fms-compatibility-version for {}",
                                ci.real_cl,
                                ci.argv_utf8.empty()
                                    ? std::string{"<unknown TU>"}
                                    : ci.argv_utf8.back());
                        }
                    }
                    const cg::msbuild::PchClassification cls =
                        cg::msbuild::classify_cl_pch(ci);
                    std::vector<cg::msbuild::CdbEntry> adapted =
                        cg::msbuild::adapt_cl(ci);
                    if (cls.is_producer && !cls.pch_key.empty()
                        && !cls.sources.empty())
                    {
                        cg::msbuild::PchProducerRecord rec;
                        rec.pch_key = cls.pch_key;
                        rec.source = cg::msbuild::detail::to_abs(
                            cls.sources.front(),
                            ci.cwd);
                        rec.umbrella = cls.pch_header;
                        if (cls.pch_header.find_first_of("/\\")
                            != std::string::npos)
                        {
                            rec.umbrella_path = cg::msbuild::detail::to_abs(
                                cls.pch_header,
                                ci.cwd);
                        }
                        rec.cwd = ci.cwd;
                        for (char& ch : rec.cwd)
                        {
                            if (ch == '\\')
                            {
                                ch = '/';
                            }
                        }
                        // 适配后的 argv 携带 /D /I 与 INCLUDE 环境投递的 -imsvc
                        // （producer 与 consumer 的 include
                        // 世界观一致）；再补回 适配器剥掉的 /Yc 与 /Fp 供重放。
                        rec.argv = cls.argv;
                        for (const cg::msbuild::CdbEntry& e : adapted)
                        {
                            if (e.file == rec.source)
                            {
                                rec.argv = e.arguments;
                                break;
                            }
                        }
                        if (!cls.pch_header.empty())
                        {
                            rec.argv.push_back("/Yc" + cls.pch_header);
                        }
                        else
                        {
                            rec.argv.push_back("/Yc");
                        }
                        rec.argv.push_back("/Fp" + cls.pch_key);
                        std::string canonical;
                        for (const std::string& a : rec.argv)
                        {
                            canonical += a + "\n";
                        }
                        canonical += rec.cwd;
                        rec.command_digest = cg::util::sha256_hex(
                            std::as_bytes(std::span{canonical}));
                        pch_producers.push_back(std::move(rec));
                    }
                    else if (cls.pch_use && !cls.pch_key.empty())
                    {
                        for (const cg::msbuild::CdbEntry& e : adapted)
                        {
                            cg::msbuild::PchConsumerRecord rec;
                            rec.file = e.file;
                            rec.pch_key = cls.pch_key;
                            rec.pch_header = cls.pch_header;
                            std::string canonical = e.directory + "\n";
                            for (const std::string& a : e.arguments)
                            {
                                canonical += a + "\n";
                            }
                            rec.command_digest = cg::util::sha256_hex(
                                std::as_bytes(std::span{canonical}));
                            pch_consumers.push_back(std::move(rec));
                        }
                    }
                    else
                    {
                        for (const cg::msbuild::CdbEntry& e : adapted)
                        {
                            pch_not_using.push_back(e.file);
                        }
                    }
                    entries.insert(
                        entries.end(),
                        std::make_move_iterator(adapted.begin()),
                        std::make_move_iterator(adapted.end()));
                }
                if (entries.empty())
                {
                    if (evaluate)
                    {
                        throw std::runtime_error(
                            std::format(
                                "ccg msbuild: no ClCompile items composed "
                                "for {} ({} member projects evaluated, {} "
                                "failed) - check that the solution declares "
                                "C++ projects building under this "
                                "configuration",
                                stem,
                                eval_project_total,
                                eval_project_failed));
                    }
                    // 报错自带 capture.db 完整路径：库完好、行在、零可用条目
                    // 时的第一现场（缺库/坏行分支 read_wrap_log 已各自带路径，
                    // 这里补齐最后一环——用户拿着路径即可直接开箱取证）。
                    throw std::runtime_error(
                        std::format(
                            "ccg msbuild: no compiler invocations captured "
                            "for {} (capture db: {}; log: {}) - was the "
                            "project already up to date? (-t:Rebuild is used "
                            "by default; check --msbuild-args)",
                            stem,
                            capture.db_path,
                            log));
                }
                cg::msbuild::write_compile_commands(cdb, entries);
                if (!pch_producers.empty() || !pch_consumers.empty())
                {
                    cg::msbuild::write_pch_sidecar(
                        cg::util::norm_path(
                            (fs::path{cdb}.parent_path() / "pch.json")
                                .lexically_normal()
                                .generic_string()),
                        cdb,
                        cp.configuration,
                        cp.platform,
                        pch_producers,
                        pch_consumers,
                        pch_not_using);
                }

                cg::msbuild::RegistryEntry e;
                e.name = stem;
                e.configuration = cp.configuration;
                e.platform = cp.platform;
                e.solution =
                    cg::msbuild::detail::relative_path_text(root, input);
                e.db = cg::msbuild::detail::relative_path_text(root, db);
                e.compile_commands =
                    cg::msbuild::detail::relative_path_text(root, cdb);
                e.msbuild = msbuild_exe;
                e.cl = primary_cl;
                e.cl_resolution = project_cls.empty()
                                      ? std::string{}
                                      : project_cls.front().resolution;
                for (const cg::msbuild::ProjectCl& pc : project_cls)
                {
                    if (!pc.cl.empty())
                    {
                        cg::msbuild::ProjectClEntry entry;
                        entry.project = pc.project;
                        entry.cl = pc.cl;
                        entry.resolution = pc.resolution;
                        entry.dir = pc.dir;
                        entry.active_cfg = pc.active_cfg;
                        entry.in_build = pc.in_build;
                        e.cl_projects.push_back(std::move(entry));
                    }
                }
                e.mode = mode_text;
                e.extracted_at = cg::util::cstd::iso8601_utc_timestamp();

                if (inv.msbuild_export_only)
                {
                    e.tus = static_cast<int>(entries.size());
                    e.failed = 0;
                    if (inv.json)
                    {
                        json::Object out;
                        out.set("config", cp.configuration);
                        out.set("platform", cp.platform);
                        out.set("mode", mode_text);
                        out.set("compile_commands", cdb);
                        out.set(
                            "tus",
                            static_cast<std::int64_t>(entries.size()));
                        if (evaluate)
                        {
                            // 部分失败的如实计数（成功工程产物照常组装）。
                            out.set(
                                "failed_projects",
                                static_cast<std::int64_t>(
                                    eval_project_failed));
                        }
                        if (record_run)
                        {
                            out.set("sandbox_root", sandbox_root);
                            json::Array leaks;
                            for (const std::string& l : sandbox_leaks)
                            {
                                leaks.add(l);
                            }
                            out.set("sandbox_leaks", std::move(leaks));
                            json::Array products;
                            for (const std::string& g : generator_products)
                            {
                                products.add(g);
                            }
                            out.set("generator_products", std::move(products));
                        }
                        std::println("{}", json::stringify(std::move(out)));
                    }
                    else
                    {
                        std::println(
                            "{} compile commands ({} TUs, {}) -> {}",
                            stem,
                            entries.size(),
                            mode_text,
                            cdb);
                        if (evaluate && eval_project_failed > 0)
                        {
                            std::println(
                                std::cerr,
                                "ccg msbuild: {} projects failed to "
                                "evaluate (see eval project failure lines "
                                "above); the remaining projects' commands "
                                "were composed",
                                eval_project_failed);
                        }
                        if (record_run && !sandbox_leaks.empty())
                        {
                            std::println(
                                std::cerr,
                                "ccg msbuild: sandbox leaks: {} (see "
                                "sandbox-leak warnings above)",
                                sandbox_leaks.size());
                        }
                    }
                }
                else
                {
                    Invocation run_inv = inv;
                    run_inv.db_path = db;
                    run_inv.layout_root = layout_root;
                    run_inv.config_name = stem;
                    run_inv.compile_db = cdb;
                    std::vector<std::string>               index_roots{root};
                    std::vector<cg::clang::CompileCommand> commands =
                        cg::clang::load(cdb, index_roots);
                    // 重放层旗标注入（--clang-compile-flags / 配置键）：cdb
                    // 文件 本身不动，注入只活在本次索引运行；先于模块路由与 PCH
                    // 规划。
                    apply_clang_compile_flags(run_inv, commands);
                    // 工具集提示（cl 模式库模块自构建）：逐 TU 的 INCLUDE
                    // 已物化为 -imsvc，按多数派工具集目录推导——钉 14.44 的工程
                    // 不再拿到 find_cl 单值的 14.51 提示；解析不出时回退主 cl
                    // 路径向上四级（既有行为）。
                    const std::string toolset_hint = [&]
                    {
                        std::vector<std::pair<std::string, int>> votes;
                        for (const cg::clang::CompileCommand& c : commands)
                        {
                            for (std::size_t i = 1; i < c.argv.size(); ++i)
                            {
                                if (c.argv[i - 1] != "-imsvc")
                                {
                                    continue;
                                }
                                const std::string r =
                                    cg::msbuild::msvc_toolset_root_of(
                                        c.argv[i]);
                                if (r.empty())
                                {
                                    continue;
                                }
                                const auto slot = std::ranges::find_if(
                                    votes,
                                    [&](const std::pair<std::string, int>& v)
                                    { return v.first == r; });
                                if (slot != votes.end())
                                {
                                    ++slot->second;
                                }
                                else
                                {
                                    votes.emplace_back(r, 1);
                                }
                            }
                        }
                        if (!votes.empty())
                        {
                            const auto best = std::ranges::max_element(
                                votes,
                                [](const std::pair<std::string, int>& a,
                                   const std::pair<std::string, int>& b)
                                { return a.second < b.second; });
                            std::error_code hec;
                            if (fs::is_directory(fs::path{best->first}, hec))
                            {
                                return cg::util::norm_path(
                                    fs::path{best->first}
                                        .lexically_normal()
                                        .generic_string());
                            }
                        }
                        if (primary_cl.empty())
                        {
                            return std::string{};
                        }
                        fs::path p = fs::path{primary_cl};
                        for (int i = 0; i < 4; ++i)
                        {
                            p = p.parent_path();
                        }
                        std::error_code hec;
                        return fs::is_directory(p, hec)
                                   ? cg::util::norm_path(
                                         p.lexically_normal().generic_string())
                                   : std::string{};
                    }();
                    EnginePrepared ms_engine;
                    route_cl_module_tus(
                        run_inv,
                        index_roots,
                        commands,
                        ms_engine,
                        toolset_hint);
                    cg::clang::PchPlanResult pch_plan;
                    const auto pch_data = [](const cg::clang::CompileCommand& c)
                    { return c.pch_involved || c.pch_create || c.pch_use; };
                    const std::string sidecar = cg::util::norm_path(
                        (fs::path{cdb}.parent_path() / "pch.json")
                            .lexically_normal()
                            .generic_string());
                    if (std::ranges::any_of(commands, pch_data)
                        || fs::is_regular_file(fs::path{sidecar}))
                    {
                        if (!ms_engine.valid)
                        {
                            ms_engine = prepare_index_engine(
                                run_inv,
                                read_db_engine_pin(run_inv));
                        }
                        cg::clang::PchOptions pch_opt;
                        pch_opt.ccg_config_dir =
                            layout::config_dir_of_db(run_inv.db_path);
                        pch_opt.compile_db_path = cdb;
                        pch_opt.sidecar_path = sidecar;
                        // --full 只影响索引全量；影子缓存仅由显式 --force
                        // 重建。
                        pch_opt.force = run_inv.force;
                        pch_opt.dry_run = run_inv.dry_run;
                        const std::vector<cg::clang::CompileCommand> coverage =
                            commands;
                        pch_plan = cg::clang::plan_pch(
                            std::move(commands),
                            coverage,
                            pch_opt);
                        commands = std::move(pch_plan.commands);
                        pch_plan.commands.clear();
                        for (const std::string& note : pch_plan.notes)
                        {
                            std::println(std::cerr, "ccg: note: {}", note);
                        }
                    }
                    const cg::graph::IndexSummary sum = run_index_tail(
                        run_inv,
                        std::move(index_roots),
                        cdb,
                        commands,
                        std::move(ms_engine),
                        &pch_plan);
                    e.tus = static_cast<int>(sum.tus);
                    e.failed = static_cast<int>(sum.failed);
                    if (record_run && !sandbox_leaks.empty())
                    {
                        std::println(
                            std::cerr,
                            "ccg msbuild: sandbox leaks: {} (see "
                            "sandbox-leak warnings above)",
                            sandbox_leaks.size());
                    }
                }

                const auto slot = std::ranges::find(
                    registry.entries,
                    stem,
                    &cg::msbuild::RegistryEntry::name);
                if (slot != registry.entries.end())
                {
                    *slot = std::move(e);
                }
                else
                {
                    registry.entries.push_back(std::move(e));
                }

                // 求值通道的快照写回（哈希门写侧）：本配置整轮成功（cdb、
                // 索引尾、注册表条目）后落盘。路径与捕获锁同区
                // （<root>/.ccg/msbuild/<stem>/，构建早前已建目录）；写
                // 失败按运行时错误抛出——快照缺席只让下一次联动多重采一
                // 轮，但吞错会让"哈希门永不生效"无声化。
                if (evaluate && pending_snapshot.has_value())
                {
                    cg::msbuild::eval::save_refresh_snapshot(
                        cg::msbuild::eval::refresh_snapshot_path(root, stem),
                        *pending_snapshot);
                }
            }

            if (registry.default_config.empty()
                || std::ranges::find(
                       registry.entries,
                       registry.default_config,
                       &cg::msbuild::RegistryEntry::name)
                       == registry.entries.end())
            {
                registry.default_config =
                    cg::msbuild::config_stem(configs.front());
            }
            cg::msbuild::save_registry(registry_path, registry);
            return any_aborted ? 1 : 0;
#endif // defined(_WIN32)
        }

    } // namespace detail

} // namespace cg::cli
