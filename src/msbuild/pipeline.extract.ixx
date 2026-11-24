module;
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

/**
 * @module cg.msbuild.pipeline.extract
 *
 * @brief 采集执行域（_WIN32）：采集日志清理、失败行判定与输出引流、一次被包装
 * Rebuild 的运行器。
 */
export module cg.msbuild.pipeline.extract;

import std;
import cg.util;
import cg.util.cstd;
import cg.msbuild.translate; // ClInvocation / CdbEntry（wrap 日志载荷类型）
import cg.msbuild.wrap;      // detail::env_wide / fs_path_from_utf8
import cg.msbuild.pipeline.configs;
import cg.msbuild.pipeline.registry;
import cg.msbuild.pipeline.wraplog;
import cg.msbuild.pipeline.extractargs;
import cg.msbuild.pipeline.hostfind;
import cg.msbuild.pipeline.treestate;
import cg.msbuild.pipeline.outputs;

export namespace cg::msbuild
{
    namespace json = cg::util::json;
#if defined(_WIN32)

    namespace detail
    {

        /**
         * @brief 一次抽取运行的捕获 token 路径值：CCG_CLWRAP_LOG 指向
         * clwrap-`<纳秒>`.jsonl。
         *
         * @internal
         * jsonl 文件本身已退役（捕获数据落推导出的 capture.db）：该路径值如今
         * 只承担两个角色——wrap 模式的触发契约（env 已设 = wrap 进程）与
         * capture.db 的推导锚点（写读两侧经同一函数映射）。逐运行 token 保留：
         * 它进入 capture 表的 token 列，标记行属于哪次运行。历史注记：旧
         * jsonl 时代的逐 token 名是为躲 MSBuild 文件追踪器的 tlog 记账（固定
         * 名会被下一次 -t:Rebuild 的 Clean 中途删除）；capture.db 固定名在
         * 记录模式下由沙箱化的 IntDir 保护（tlog 一轮一清），真编模式仍可能
         * 被多项目 Clean 命中——运行前的 prune 先删掉它，读侧只看本次行。
         */
        [[nodiscard]]
        inline auto new_capture_log_path(const std::string& dir) -> std::string
        {
            const auto ticks =
                std::chrono::system_clock::now().time_since_epoch();
            return std::format("{}/clwrap-{}.jsonl", dir, ticks.count());
        }

        /** @brief 删除一个陈旧采集工件；仍在盘上却删不掉时打印告警。 */
        inline void remove_capture_artifact(const std::string& target)
        {
            std::error_code rm_ec;
            std::filesystem::remove(fs_path_from_utf8(target), rm_ec);
            if (rm_ec)
            {
                std::error_code probe;
                if (std::filesystem::exists(fs_path_from_utf8(target), probe))
                {
                    std::println(
                        std::cerr,
                        "ccg msbuild: cannot remove stale capture "
                        "artifact {}: {}",
                        target,
                        rm_ec.message());
                }
            }
        }

        /**
         * @brief 清除先前运行遗留的捕获存储（崩溃或中断的运行会留下一个；
         * 新运行绝不读它们）。
         *
         * @internal 两件事：删除推导出的 capture.db（含崩溃残留的 -wal/-shm
         * 边车——读侧另有 token 过滤纵深防御，双保险下旧行绝不混进本次捕
         * 获）与清理旧版本写入的 clwrap-*.jsonl 存量（迁移清理，幂等）。
         * compile_commands.json 与逐配置缓存目录中的其他内容一律不动。删除
         * 失败（句柄占用/ACL 等）告警一行而非静默：读侧的 token 过滤会兜住
         * 正确性，但该信号值得看见。
         */
        inline void prune_capture_logs(const std::string& dir)
        {
            std::error_code ec;
            for (const std::filesystem::directory_entry& entry :
                 std::filesystem::directory_iterator(
                     fs_path_from_utf8(dir),
                     ec))
            {
                std::error_code leaf_ec;
                if (!entry.is_regular_file(leaf_ec))
                {
                    continue;
                }
                const std::string name =
                    entry.path().filename().generic_string();
                if (name.starts_with("clwrap-") && name.ends_with(".jsonl"))
                {
                    remove_capture_artifact(entry.path().generic_string());
                }
            }
            const std::string db = capture_db_of_log_path(dir);
            remove_capture_artifact(db);
            remove_capture_artifact(db + "-wal");
            remove_capture_artifact(db + "-shm");
        }

    } // namespace detail

    namespace detail
    {

        /**
         * @brief 构建后步骤失败行判形（记录模式滤噪）。
         *
         * MSB3073（MSBuild 对 exec 任务 "exited with code" 的包装错误）与
         * VCEnd/ exec 任务直接报出的 "exited with code" 行；大小写不敏感。
         */
        [[nodiscard]]
        inline auto is_post_build_failure_line(std::string_view line) -> bool
        {
            const std::string lower = cg::util::ascii_lower(line);
            return lower.find("error msb3073") != std::string::npos
                   || lower.find("exited with code") != std::string::npos;
        }

        /**
         * @brief 一路构建输出流的排水钩子：逐字节转发，记录模式先按行
         * 判形再滤噪。
         *
         * @internal 行缓冲状态在 on_chunk 与 on_finish 间共享（残行判形
         * 在收尾）；转发流经指针传递（std::cout/std::cerr 恒存活，且按值
         * 捕获引用形参在 MSVC 下会尝试复制流对象——删除的拷贝构造）。
         */
        inline auto make_output_tap(
            std::ostream& forward,
            std::int64_t& suppressed,
            bool suppress_exec_failures) -> cg::util::platform::DrainTap
        {
            const auto          line = std::make_shared<std::string>();
            std::ostream* const target = &forward;
            return cg::util::platform::DrainTap{
                .on_chunk =
                    [target, suppress_exec_failures, &suppressed, line](
                        const char* data,
                        const DWORD size)
                {
                    if (!suppress_exec_failures)
                    {
                        target->write(data, static_cast<std::streamsize>(size));
                        return;
                    }
                    line->append(data, size);
                    std::size_t start = 0;
                    for (std::size_t nl = line->find('\n', start);
                         nl != std::string::npos;
                         nl = line->find('\n', start))
                    {
                        const std::string_view row{
                            line->data() + start,
                            nl - start + 1};
                        if (is_post_build_failure_line(row))
                        {
                            ++suppressed;
                        }
                        else
                        {
                            target->write(
                                row.data(),
                                static_cast<std::streamsize>(row.size()));
                        }
                        start = nl + 1;
                    }
                    line->erase(0, start);
                },
                .on_finish =
                    [target, suppress_exec_failures, &suppressed, line]()
                {
                    // 流尾无换行的残行同样判形一次（绝不丢字节）。
                    if (suppress_exec_failures && !line->empty())
                    {
                        if (is_post_build_failure_line(*line))
                        {
                            ++suppressed;
                        }
                        else
                        {
                            target->write(
                                line->data(),
                                static_cast<std::streamsize>(line->size()));
                        }
                    }
                    target->flush();
                },
            };
        }

    } // namespace detail

    /**
     * @brief 一次被包装 Rebuild 的运行结果。
     *
     * @internal
     * out/err 是构建子进程两路流的逐字节累积——tee 语义（每路一条排水
     * 线程边转发到本进程对应流边收集；两路必须并发排水，子进程先灌满一
     * 路管道再等另一路 EOF 时顺序排水会死锁，run_capture 审计同款教训）。
     * 退出码逐字返回（MSBuild 失败是调用方诚实报告的输入，不是 wrap 层
     * 错误）；文本供调用方把 MSB3073/exec 类构建后步骤失败归类为记录模
     * 式的预期信息。记录模式下 MSB3073/VCEnd/exec "exited with code" 类
     * 错误行不透传刷屏（suppressed_post_build_lines 计数，结尾一行汇总）
     * ——累积全文不受过滤影响。
     */
    struct ExtractRunResult
    {
        int         exit_code = -1;
        std::string out; // 子进程 stdout（逐字节累积）
        std::string err; // 子进程 stderr（逐字节累积）
        // 记录模式被滤噪的构建后步骤失败行数（两路合计）。
        std::int64_t suppressed_post_build_lines = 0;
    };

    /**
     * @brief 运行被包装的 Rebuild：ToolPath 家族指向桩目录（记录模式）或
     * CLToolExe/CLToolPath 指向运行中的 ccg.exe（真编）、为每个子工具调用
     * 设好 wrap 模式环境变量对。
     *
     * 构建输出经管道 tee 给用户（流式转发保住既有可见性）并逐字节累积返
     * 回。wrap 日志的父目录先创建，使第一个 cl 调用就能追加。记录模式追
     * 加注入：CCG_CLWRAP_STUB=1（桩化零写盘）、CCG_CLWRAP_CL 前缀映射
     * （混合工具集逐工程选 cl）、PATH 前置桩目录（MASM 等经裸名解析的工
     * 具与不认 ToolPath 的调用面由此接管——桩目录只含家族名副本，不会遮
     * 蔽任何其他可执行）。
     */
    [[nodiscard]]
    inline auto run_extract(const ExtractParams& p) -> ExtractRunResult
    {
        const std::filesystem::path log_fs =
            detail::fs_path_from_utf8(p.log_path);
        std::error_code ec;
        std::filesystem::create_directories(log_fs.parent_path(), ec);

        // 预建 capture.db（单一连接 WAL + 幂等 DDL）：库与表先于一切写者
        // 存在，桩进程并发冷启动不再落入"多进程同窗 CREATE 新库 + 切
        // WAL"的竞态窗口（WAL 切换遇其它活动连接直接回 SQLITE_BUSY，曾
        // 由此静默丢整 invocation 行）。写者仍有自建兜底，预建失败只告警
        // （capture_db_ensure）。
        detail::capture_db_ensure(detail::capture_db_of_log_path(p.log_path));

        std::wstring cmdline = cg::util::platform::quote_for_command_line(
            cg::util::platform::widen_utf8(p.msbuild));
        for (const std::string& arg : build_extract_args(p))
        {
            cmdline += L' ';
            cmdline += cg::util::platform::quote_for_command_line(
                cg::util::platform::widen_utf8(arg));
        }
        if (!p.extra_args.empty())
        {
            cmdline += L' ';
            cmdline += cg::util::platform::widen_utf8(p.extra_args); // 逐字拼接
        }

        std::vector<std::pair<std::wstring, std::wstring>> overrides{
            {L"CCG_CLWRAP_LOG", cg::util::platform::widen_utf8(p.log_path)},
            {L"CCG_CLWRAP_REAL_CL", cg::util::platform::widen_utf8(p.real_cl)}};
        if (!p.cl_map.empty())
        {
            overrides.emplace_back(
                L"CCG_CLWRAP_CL",
                cg::util::platform::widen_utf8(p.cl_map));
        }
        if (p.mode == ExtractMode::Record)
        {
            overrides.emplace_back(L"CCG_CLWRAP_STUB", L"1");
            std::wstring path_prefix =
                cg::util::platform::widen_utf8(p.stub_tool_dir);
            if (const std::optional<std::wstring> current =
                    detail::env_wide(L"PATH"))
            {
                path_prefix += L";";
                path_prefix += *current;
            }
            overrides.emplace_back(L"PATH", std::move(path_prefix));
        }
        // 构建子树专用净化：NoDefaultCurrentDirectoryInExePath（部分 shell/
        // 启动器会设）会让子进程按裸名解析 exe 时不再搜索当前目录，定制
        // 构建步骤常依赖该行为而失败。仅从本构建子树剔除——ccg 进程自身
        // 与 find_msbuild/find_cl 的探测子进程不受影响。
        std::wstring env_block = detail::build_child_env(
            overrides,
            {L"NoDefaultCurrentDirectoryInExePath"});

        // 每路一条排水钩子（挂进捕获原语）：逐字节转发到本进程对应流
        // （构建输出保持流式可见），累积由原语完成、钩子只见增量；转发
        // 失败（下游已关闭）不致命——累积继续。记录模式滤噪：MSB3073/
        // VCEnd/exec "exited with code" 类错误行不透传（替身零写盘，下游
        // 产物根本不存在，复制/校验步骤必然失败——按行缓冲判形、计数
        // 收尾汇总）；累积全文照收，退出码语义不变。真编模式逐字节透传
        // （构建输出的可见性是金标准对账的输入）。
        const bool   suppress_exec_failures = p.mode == ExtractMode::Record;
        std::int64_t suppressed_out = 0;
        std::int64_t suppressed_err = 0;

        // 被包装 Rebuild 的原语配置：stdin 继续继承本进程的（与旧的无管道
        // 形态等价）；两路输出改管道；环境块经 CREATE_UNICODE_ENVIRONMENT
        // 定制（上面 build_child_env 的产物）；不设 CREATE_NO_WINDOW——构建
        // 子进程与本进程共享控制台是既有形态。
        cg::util::platform::ChildRunSpec spec;
        spec.cmdline = std::move(cmdline);
        spec.env_block = std::move(env_block);
        spec.stdin_mode = cg::util::platform::ChildStdin::ParentHandle;
        spec.create_no_window = false;
        spec.out_tap = detail::make_output_tap(
            std::cout,
            suppressed_out,
            suppress_exec_failures);
        spec.err_tap = detail::make_output_tap(
            std::cerr,
            suppressed_err,
            suppress_exec_failures);
        const cg::util::platform::ChildRunResult run =
            cg::util::platform::run_child(spec);
        if (!run.pipes_ok)
        {
            throw std::runtime_error(
                std::format(
                    "ccg msbuild: cannot create output pipes for MSBuild '{}'",
                    p.msbuild));
        }
        if (!run.started)
        {
            throw std::runtime_error(
                std::format(
                    "ccg msbuild: cannot start MSBuild '{}' (error {})",
                    p.msbuild,
                    run.spawn_error));
        }

        ExtractRunResult result;
        result.out = std::move(run.out);
        result.err = std::move(run.err);
        result.exit_code = run.exit_code;
        result.suppressed_post_build_lines = suppressed_out + suppressed_err;
        if (suppress_exec_failures && result.suppressed_post_build_lines > 0)
        {
            std::println(
                std::cerr,
                "ccg msbuild: note: {} post-build/exec failure line{} "
                "on empty record-mode product{} suppressed (expected "
                "in record mode; the full text is kept in the "
                "captured build output)",
                result.suppressed_post_build_lines,
                result.suppressed_post_build_lines == 1 ? "" : "s",
                result.suppressed_post_build_lines == 1 ? "" : "s");
        }
        return result;
    }

#endif // defined(_WIN32)
} // namespace cg::msbuild
