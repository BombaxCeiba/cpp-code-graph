module;
// 矩阵直转测试表（矩阵源见 tests/shell-matrix.inc）。
#include "shell-matrix.inc"
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// 时间戳自刷新用例需要篡改缓存文件的 LastWriteTime（纯 Win32 探测）。
#include <windows.h>
#endif

/**
 * @module cg.test.shellscan
 *
 * @brief shell 查找意图提取层的矩阵与行为测试（PreToolUse 四工具
 *        监听）：Bash/PowerShell 用例矩阵逐条比对（矩阵 62+40
 *        条直转加自造补充）、Search 三候选字段直读、加载前预筛与
 *        降级链注入。
 *
 * Bash 矩阵纯 tree-sitter 路径（跨平台恒跑）；PowerShell 矩阵经进程
 * 内引擎宿主，引擎不可用的环境整组跳过（本机与 CI 上的 Windows 必
 * 有 5.1 主路径，全量生效）。对象协议用例是被测对象为原语本身的协议
 * 测试：经协议测试面直调三个原语，建出的垫片侧对象与文件句柄都由测试
 * 里的拥有型对象持有，拆除的返回值照旧由断言检查。
 */

export module cg.test.shellscan;

import std;
import cg.util;
import cg.util.json;
import cg.test.harness;

namespace
{

    using R = cg::test::Result;
    namespace scan = cg::util::shellscan;
    namespace host = cg::util::cshost;
    // 对象协议原语的协议测试面：只有这里的协议用例直调原语（被测对象就
    // 是原语本身），生产代码与别的测试不得走它。
    namespace proto = cg::util::cshost::protocol_test;
    namespace mx = cg::test::shellmatrix;

    /// 引擎枚举到矩阵字符串口径。
    [[nodiscard]]
    auto engine_name(scan::Engine engine) -> std::string_view
    {
        switch (engine)
        {
        case scan::Engine::Rg:
            return "rg";
        case scan::Engine::SelectString:
            return "select-string";
        case scan::Engine::SearchField:
            return "search";
        }
        return {};
    }

    [[nodiscard]]
    auto path_kind_name(scan::PathKind kind) -> std::string_view
    {
        switch (kind)
        {
        case scan::PathKind::None:
            return "none";
        case scan::PathKind::StdinPipe:
            return "stdin-pipe";
        case scan::PathKind::AfterCd:
            return "after-cd";
        }
        return {};
    }

    /// 矩阵用例断言标签（用例号 + 事项 + 原始命令）。
    [[nodiscard]]
    auto case_label(const mx::MatrixCase& c, std::string_view what)
        -> std::string
    {
        return std::format("[{}] {} cmd={}", c.id, what, c.cmd);
    }

    /// 矩阵用例内单条请求的断言标签（在用例标签上追加请求下标）。
    [[nodiscard]]
    auto request_label(
        const mx::MatrixCase& c,
        std::size_t           i,
        std::string_view      what) -> std::string
    {
        return case_label(c, std::format("请求[{}] {}", i, what));
    }

    /// 一条矩阵用例与实际产出的逐字段比对。
    void expect_case(
        R&                               r,
        const mx::MatrixCase&            c,
        const scan::ShellExtractOutcome& got)
    {
        cg::test::check(
            r,
            got.skip == c.skip,
            case_label(c, std::format("skip '{}' vs '{}'", got.skip, c.skip)));
        cg::test::check(
            r,
            got.skip_all == c.skip_all,
            case_label(
                c,
                std::format(
                    "skip_all '{}' vs '{}'",
                    got.skip_all,
                    c.skip_all)));
        cg::test::check(
            r,
            got.requests.size() == c.requests.size(),
            case_label(
                c,
                std::format(
                    "请求数 {} vs 期望 {}",
                    got.requests.size(),
                    c.requests.size())));
        const std::size_t count =
            std::min(got.requests.size(), c.requests.size());
        for (std::size_t i = 0; i < count; ++i)
        {
            const scan::ShellRequest& actual = got.requests[i];
            const mx::ExpectRequest&  want = c.requests[i];
            cg::test::check(
                r,
                engine_name(actual.engine) == want.engine,
                request_label(
                    c,
                    i,
                    std::format("engine '{}'", engine_name(actual.engine))));
            cg::test::check(
                r,
                actual.patterns == want.patterns,
                request_label(c, i, "patterns"));
            cg::test::check(
                r,
                actual.single == want.single,
                request_label(c, i, "single"));
            cg::test::check(
                r,
                actual.fixed == want.fixed,
                request_label(c, i, "fixed"));
            cg::test::check(
                r,
                actual.paths == want.paths,
                request_label(c, i, "paths"));
            cg::test::check(
                r,
                actual.globs == want.globs,
                request_label(c, i, "globs"));
            cg::test::check(
                r,
                actual.types == want.types,
                request_label(c, i, "types"));
            cg::test::check(
                r,
                path_kind_name(actual.path_kind) == want.path_kind,
                request_label(
                    c,
                    i,
                    std::format(
                        "path_kind '{}'",
                        path_kind_name(actual.path_kind))));
            cg::test::check(
                r,
                actual.cwd_hint == want.cwd_hint,
                request_label(c, i, "cwd_hint"));
        }
    }

    /// 引擎是否本机可用（用于 PowerShell 组与桥接用例的前置判定）。
    [[nodiscard]]
    auto engine_ready() -> bool
    {
        return host::parse_via_engine("Select-String probe_ready").has_value();
    }

    void bash_matrix_cases(R& r)
    {
        for (const mx::MatrixCase& c : mx::bash_cases)
        {
            if (c.needs_engine && !engine_ready())
            {
                continue; // 桥接用例：无引擎环境跳过（宁漏方向）
            }
            expect_case(r, c, scan::extract_bash_queries(c.cmd));
        }
    }

    void ps_matrix_cases(R& r)
    {
        if (!engine_ready())
        {
            // 环境断言：本仓支持的 Windows 环境必有 5.1 主路径；完全无引擎
            // 时记录一条可见说明而非静默通过。
            cg::test::check(
                r,
                false,
                std::format("降级链诊断: {}", host::last_detail()));
            return;
        }
        for (const mx::MatrixCase& c : mx::ps_cases)
        {
            expect_case(r, c, scan::extract_ps_queries(c.cmd));
        }
    }

    /// Search 工具载荷 JSON 到提取产出的直读解析。
    [[nodiscard]]
    auto search_outcome_of(std::string_view json) -> scan::ShellExtractOutcome
    {
        const cg::util::json::Document doc =
            cg::util::json::Document::parse(json);
        return scan::search_tool_queries(doc.root());
    }

    void search_field_cases(R& r)
    {
        {
            // 三候选字段按序取第一个存在的字符串值。
            const auto out = search_outcome_of(R"({"query":"foo"})");
            cg::test::check(r, out.requests.size() == 1, "query 命中");
            cg::test::check(
                r,
                out.requests.size() == 1
                    && out.requests[0].patterns
                           == std::vector<std::string>{"foo"},
                "query 文本");
            cg::test::check(
                r,
                out.requests.size() == 1
                    && engine_name(out.requests[0].engine) == "search",
                "engine=search");
        }
        {
            const auto out = search_outcome_of(R"({"pattern":"bar"})");
            cg::test::check(
                r,
                out.requests.size() == 1
                    && out.requests[0].patterns
                           == std::vector<std::string>{"bar"},
                "pattern 兜底命中");
        }
        {
            const auto out = search_outcome_of(R"({"q":"baz"})");
            cg::test::check(
                r,
                out.requests.size() == 1
                    && out.requests[0].patterns
                           == std::vector<std::string>{"baz"},
                "q 兜底命中");
        }
        {
            // 非字符串 query 让位给后续候选字段。
            const auto out =
                search_outcome_of(R"({"query":123,"pattern":"real"})");
            cg::test::check(
                r,
                out.requests.size() == 1
                    && out.requests[0].patterns
                           == std::vector<std::string>{"real"},
                "非字符串跳过");
        }
        {
            const auto out = search_outcome_of(R"({"other":1})");
            cg::test::check(
                r,
                out.requests.empty() && out.skip.empty(),
                "无候选字段静默");
        }
        {
            const auto out = search_outcome_of(R"({"query":""})");
            cg::test::check(r, out.skip == "no-pattern", "空文本 no-pattern");
        }
        {
            std::string long_json =
                R"({"query":")" + std::string(257, 'a') + R"("})";
            const auto out = search_outcome_of(long_json);
            cg::test::check(r, out.skip == "too-long", "超长 too-long");
        }
        {
            const auto out = search_outcome_of("{\"query\":\"a\\nb\"}");
            cg::test::check(
                r,
                out.skip == "control-char",
                "控制字符 control-char");
        }
        {
            // 载荷不是对象：静默空手。
            const auto out = search_outcome_of(R"(["query"])");
            cg::test::check(
                r,
                out.requests.empty() && out.skip.empty(),
                "非对象静默");
        }
    }

    void prescreen_cases(R& r)
    {
        // 未提及查找 cmdlet 的命令：预筛直接空手（不付引擎成本）。
        for (const std::string_view cmd :
             {"git status",
              "Get-ChildItem -Recurse",
              "echo hello",
              "npm run build",
              "$x = 1; Write-Host $x"})
        {
            const auto out = scan::extract_ps_queries(cmd);
            cg::test::check(
                r,
                out.requests.empty() && out.skip.empty(),
                std::format("预筛静默: {}", cmd));
        }
        // 预筛吞吐：非命中路径微秒级（1000 次远低于 100ms，宽松防抖）。
        const auto    begin = std::chrono::steady_clock::now();
        constexpr int rounds = 1000;
        for (int i = 0; i < rounds; ++i)
        {
            static_cast<void>(scan::extract_ps_queries("git status"));
        }
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - begin);
        cg::test::check(
            r,
            elapsed.count() < 100,
            std::format("预筛 {} 次 {}ms", rounds, elapsed.count()));
        // 引擎稳态观测（只记录不断言，数字归档进进度文件）：20 次完整
        // 解析的中位毫秒。
        if (host::parse_via_engine("Select-String steady_probe").has_value())
        {
            std::vector<long long> samples;
            for (int i = 0; i < 20; ++i)
            {
                const auto t0 = std::chrono::steady_clock::now();
                static_cast<void>(
                    host::parse_via_engine("Select-String -Pattern probe"));
                samples.push_back(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count());
            }
            std::ranges::sort(samples);
            std::println(
                "shellscan: 引擎稳态中位 {}us / max {}us（路由 {}）",
                samples[samples.size() / 2],
                samples.back(),
                static_cast<int>(host::last_route()));
        }
        // Bash 快速否决：无 rg 的命令同样零请求零 skip。
        for (const std::string_view cmd :
             {"git status", "ls -la", "echo done", "make -j8"})
        {
            const auto out = scan::extract_bash_queries(cmd);
            cg::test::check(
                r,
                out.requests.empty() && out.skip.empty(),
                std::format("Bash 静默: {}", cmd));
        }
    }

    void bash_prescreen_cases(R& r)
    {
        // 触发词命中：rg/sls 要求词边界，select-string 按子串。
        for (const std::string_view cmd :
             {"rg pi",
              "RG -n pi",
              "Rg -F 'pi' src",
              "cat x.cpp | rg foo",
              "rg.exe foo",
              "/usr/bin/rg foo",
              "cd sub && rg pi",
              "sls foo",
              "SLS -Pattern foo",
              "Select-String -Pattern pi",
              "select-string foo",
              "pwsh -Command 'Select-String pi'",
              "echo rg"})
        {
            cg::test::check(
                r,
                scan::bash_mentions_search(cmd),
                std::format("预筛命中: {}", cmd));
        }
        // 非查找命令与无边界子串：不命中（假命中白付解析、假漏安静跳过，
        // 两个方向都安全；这里的断言钉住词边界不误伤普通单词）。
        for (const std::string_view cmd :
             {"git status",
              "ls -la",
              "make -j8",
              "grep foo",
              "Get-ChildItem",
              "energy",
              "echo large",
              "slsm foo",
              "npm run build"})
        {
            cg::test::check(
                r,
                !scan::bash_mentions_search(cmd),
                std::format("预筛不命中: {}", cmd));
        }
        // 吞吐：非命中路径微秒级（1000 次远低于 100ms，宽松防抖）。
        const auto    begin = std::chrono::steady_clock::now();
        constexpr int rounds = 1000;
        for (int i = 0; i < rounds; ++i)
        {
            static_cast<void>(scan::bash_mentions_search(
                "cmake --build build/msvc-dev --config Release"));
        }
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - begin);
        cg::test::check(
            r,
            elapsed.count() < 100,
            std::format("Bash 预筛 {} 次 {}ms", rounds, elapsed.count()));
    }

    void ps_degrade_chain_cases(R& r)
    {
        // 注入全禁：快速否决，不崩、不抛。
        {
            cg::test::EnvScope forbid{"CCG_PSHOST_FORBID"};
            forbid.set("all");
            host::reset_for_tests();
            const auto out = host::parse_via_engine("sls foo");
            cg::test::check(r, !out.has_value(), "全禁后通道不可用");
            cg::test::check(
                r,
                host::last_route() == host::Route::None,
                "全禁后路由为 None");
            cg::test::check(
                r,
                std::string_view{host::last_detail()}.find("none")
                    != std::string_view::npos,
                std::format("诊断含 none: {}", host::last_detail()));
        }
        // 解除注入并复位：本机环境应恢复出可用路由（5.1 主路径兜底）。
        {
            host::reset_for_tests();
            const auto out = host::parse_via_engine("sls foo");
            cg::test::check(r, out.has_value(), "复位后通道恢复");
            cg::test::check(
                r,
                host::last_route() != host::Route::None,
                std::format(
                    "复位后路由非 None: {}",
                    static_cast<int>(host::last_route())));
        }
        // 只禁 hostfxr 路线：5.1 主路径兜底必须接住。
        {
            cg::test::EnvScope forbid{"CCG_PSHOST_FORBID"};
            forbid.set("hostfxr");
            host::reset_for_tests();
            const auto out =
                host::parse_via_engine("Select-String -Pattern foo");
            cg::test::check(r, out.has_value(), "禁 hostfxr 后 5.1 兜底");
            cg::test::check(
                r,
                host::last_route() == host::Route::Clr51,
                std::format(
                    "禁 hostfxr 后路由=5.1: 诊断 {}",
                    host::last_detail()));
        }
        // 复原（不污染进程内后续使用；矩阵组已在前面跑完）。
        host::reset_for_tests();
        static_cast<void>(
            host::parse_via_engine("Select-String -Pattern done"));
    }

    const cg::test::Registrar reg_bash_matrix{
        "shellscan.bash_matrix",
        &bash_matrix_cases};
    const cg::test::Registrar reg_ps_matrix{
        "shellscan.ps_matrix",
        &ps_matrix_cases};
    const cg::test::Registrar reg_search_fields{
        "shellscan.search_fields",
        &search_field_cases};
    const cg::test::Registrar reg_prescreen{
        "shellscan.prescreen",
        &prescreen_cases};
    const cg::test::Registrar reg_bash_prescreen{
        "shellscan.bash_prescreen",
        &bash_prescreen_cases};
    void store_refresh_cases(R& r)
    {
#if defined(_WIN32)
        // 商店版复制缓存的时间戳自刷新（任务书钉死的行为）：篡改缓存中
        // pwsh.exe 的 LastWriteTime 后，下一次引擎使用必须整目录重摆
        // （时间戳回齐源文件）。
        if (!engine_ready())
        {
            return;
        }
        std::wstring base(512, L'\0');
        const DWORD  n =
            GetEnvironmentVariableW(L"LOCALAPPDATA", base.data(), 512);
        if (n == 0 || n >= 512)
        {
            cg::test::check(r, false, "LOCALAPPDATA 缺失");
            return;
        }
        const std::filesystem::path marker =
            std::filesystem::path{base.substr(0, n)} / L"ccg" / L"cshost"
            / L"store" / L"pwsh" / L"pwsh.exe";
        if (!std::filesystem::exists(marker))
        {
            // 本机降级链未走商店版路线（例如 MSI pwsh 机器）：无缓存可
            // 篡改，行为用例不适用。
            return;
        }
        // 文件句柄一取到就交给拥有者；打开失败时断言失败并返回。
        cg::util::handle::Owned file =
            cg::util::handle::Owned::adopt_file(CreateFileW(
                marker.c_str(),
                FILE_WRITE_ATTRIBUTES,
                0,
                nullptr,
                OPEN_EXISTING,
                0,
                nullptr));
        if (!file.valid())
        {
            cg::test::check(r, false, "打开缓存时间戳失败");
            return;
        }
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        // 往后拨 1 天（86.4e7 百纳秒），确保与源不一致。
        ULARGE_INTEGER stamp{};
        stamp.LowPart = now.dwLowDateTime;
        stamp.HighPart = now.dwHighDateTime;
        stamp.QuadPart += 864000000000ULL;
        FILETIME shifted{};
        shifted.dwLowDateTime = stamp.LowPart;
        shifted.dwHighDateTime = stamp.HighPart;
        const BOOL shifted_ok =
            SetFileTime(file.get(), nullptr, nullptr, &shifted);
        // 必须在这里就关：句柄是以零共享方式打开的，开着的时候下面的
        // 自刷新删不掉这个文件。
        file.reset();
        cg::test::check(r, shifted_ok != 0, "篡改缓存时间戳");
        if (!shifted_ok)
        {
            return;
        }
        host::reset_for_tests();
        const auto out = host::parse_via_engine("sls foo");
        cg::test::check(r, out.has_value(), "自刷新后引擎可用");
        // 重摆后缓存时间戳应与源回齐（hardlink 与源共享文件数据但
        // 目录项独立；回齐即证明整目录重摆发生）。
        file = cg::util::handle::Owned::adopt_file(CreateFileW(
            marker.c_str(),
            FILE_READ_ATTRIBUTES,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr));
        if (!file.valid())
        {
            cg::test::check(r, false, "重读缓存时间戳失败");
            return;
        }
        FILETIME after{};
        GetFileTime(file.get(), nullptr, nullptr, &after);
        ULARGE_INTEGER after_ui{};
        after_ui.LowPart = after.dwLowDateTime;
        after_ui.HighPart = after.dwHighDateTime;
        cg::test::check(
            r,
            after_ui.QuadPart != stamp.QuadPart,
            "缓存时间戳已重摆回源值");
#endif // defined(_WIN32)
    }

    const cg::test::Registrar reg_degrade{
        "shellscan.ps_degrade_chain",
        &ps_degrade_chain_cases};
    const cg::test::Registrar reg_store_refresh{
        "shellscan.ps_store_refresh",
        &store_refresh_cases};

    // ------------------------------------------------------------------
    // cshost 对象协议（垫片在场时跑）：双侧生命周期与协议错误面。
    // ------------------------------------------------------------------

    /// 命名实参表的便捷构造（ObjCall 的 "script" 实参）。
    [[nodiscard]]
    auto script_argv(std::string_view script) -> std::vector<host::ObjArg>
    {
        return {{"script", script}};
    }

    /**
     * @brief 协议测试里垫片侧对象的拥有者：持有一个协议面与一个令牌，离开
     *        作用域时经协议测试面拆除；不参与引用计数。
     *
     * @internal
     * 被测对象是原语本身，建立与拆除的返回值都要由断言原样检查，所以建
     * 立的结果直接交给构造函数，拆除经 destroy 成员调原语、把返回码原样
     * 交回并清零令牌；对已经拆除的令牌再调一次原语的断言照旧直调。令牌
     * 不大于零表示没有建成（原语交回的是错误码），析构不去拆。拆除是跨
     * 语言调用，内部会分配内存；析构里的那一次包住异常，从析构逃逸等于
     * 终止进程。
     */
    class ProtoObject
    {
    public:
        /** @brief 接管建立原语交回的结果：正数是令牌，其余是错误码。 */
        ProtoObject(host::Face face, std::int32_t token) noexcept
            : face_{face}, token_{token}
        {
        }

        ProtoObject(const ProtoObject&) = delete;
        auto operator=(const ProtoObject&) -> ProtoObject& = delete;
        ProtoObject(ProtoObject&&) = delete;
        auto operator=(ProtoObject&&) -> ProtoObject& = delete;

        ~ProtoObject()
        {
            try
            {
                static_cast<void>(destroy());
            }
            catch (...)
            {
                // 吞掉之后拆除没有做成，垫片侧的条目随进程退出回收。
            }
        }

        /**
         * @brief 经协议测试面拆除当前持有的对象，清零令牌，原样交回原语
         *        的返回码。
         *
         * @internal 没有持有对象时不调原语，交回 0（与拆除成功的返回码相
         *           同）；是否建成要在拆除之前另行断言。
         */
        auto destroy() -> std::int32_t
        {
            if (token_ <= 0)
            {
                return 0;
            }
            const std::int32_t token = token_;
            token_ = 0;
            return proto::obj_destroy(face_, token);
        }

    private:
        host::Face   face_;
        std::int32_t token_;
    };

    /**
     * 生命周期与引用计数：往返三件、计数归零才真析构、孤儿释放
     * （abandon 只弃计数不拆除）、陈旧句柄被拒（拆除后/槽复用后）、
     * 协议错误码面。全程钉 51 线（FORBID hostfxr）保证形态确定。
     */
    void csobj_lifecycle_cases(R& r)
    {
#if defined(_WIN32)
        if (!engine_ready())
        {
            cg::test::check(
                r,
                false,
                std::format("引擎不可用: {}", host::last_detail()));
            return;
        }
        cg::test::EnvScope forbid{"CCG_PSHOST_FORBID"};
        forbid.set("hostfxr");
        host::reset_for_tests();
        const host::Face     face = host::Face::SelectString;
        const host::ObjClass cls = host::ObjClass::SelectStringCore;
        const auto           argv = script_argv("Select-String -Pattern foo");

        // 往返：create → call → destroy，拆除后旧 token 响亮拒绝。建出的
        // 对象一取到就交给拥有者；拆除的返回值经它的 destroy 成员断言。
        {
            const std::int32_t token = proto::obj_create(face, cls, L"");
            ProtoObject        owner{face, token};
            cg::test::check(r, token > 0, std::format("token>0: {}", token));
            const auto out = proto::obj_call(face, cls, token, "Extract", argv);
            cg::test::check(
                r,
                out.ok() && out.payload.find("requests") != std::string::npos,
                std::format("Extract 成功: rc={}", out.rc));
            cg::test::check(
                r,
                owner.destroy() == static_cast<std::int32_t>(host::ObjErr::Ok),
                "拆除返回 0");
            const auto stale =
                proto::obj_call(face, cls, token, "Extract", argv);
            cg::test::check(
                r,
                stale.rc
                    == static_cast<std::int32_t>(host::ObjErr::StaleHandle),
                std::format("拆除后旧句柄被拒: rc={}", stale.rc));
            // 对已经拆除的令牌再调一次原语：断言的是协议拒绝旧句柄，拆
            // 的是失效的令牌，不交还任何东西，所以照旧直调。
            cg::test::check(
                r,
                proto::obj_destroy(face, token)
                    == static_cast<std::int32_t>(host::ObjErr::StaleHandle),
                "二次拆除同为 stale（拆除恰一次）");
        }

        // 引用计数语义：拷贝持活（析构一份不拆），最后一份析构才拆。
        {
            std::optional<host::CsRef> ref = host::open_select_string();
            cg::test::check(r, ref.has_value() && ref->valid(), "会话句柄有效");
            if (ref.has_value())
            {
                const std::int32_t token = ref->token();
                {
                    const host::CsRef copy{*ref}; // AddRef
                } // copy 析构：计数 2→1，垫片侧不得拆除
                const auto alive =
                    proto::obj_call(face, cls, token, "Extract", argv);
                cg::test::check(
                    r,
                    alive.ok(),
                    std::format("计数未归零句柄仍活: rc={}", alive.rc));
                ref.reset(); // 归零 → 拆除
                const auto dead =
                    proto::obj_call(face, cls, token, "Extract", argv);
                cg::test::check(
                    r,
                    dead.rc
                        == static_cast<std::int32_t>(host::ObjErr::StaleHandle),
                    "最后一份析构后才真拆除");
            }
        }

        // 孤儿释放：abandon 后 call 拒绝、归零不拆除（垫片侧条目仍在，
        // 垫片孤儿线程语义的证据面），显式拆除仍可达（测试收尾）。
        {
            std::optional<host::CsRef> ref = host::open_select_string();
            cg::test::check(r, ref.has_value(), "孤儿用例会话建立");
            if (ref.has_value())
            {
                const std::int32_t token = ref->token();
                ref->abandon();
                const auto refused = ref->call("Extract", argv);
                cg::test::check(
                    r,
                    refused.rc
                        == static_cast<std::int32_t>(host::ObjErr::StaleHandle),
                    "abandon 后 call 拒绝");
                ref.reset(); // 归零但短路：不 ObjDestroy
                // 孤儿从此没有拥有者；拆除是断言（原语仍可达），经拥有者做。
                ProtoObject owner{face, token};
                const auto  orphan =
                    proto::obj_call(face, cls, token, "Extract", argv);
                cg::test::check(
                    r,
                    orphan.ok(),
                    std::format("孤儿路径垫片侧未拆除: rc={}", orphan.rc));
                cg::test::check(
                    r,
                    owner.destroy()
                        == static_cast<std::int32_t>(host::ObjErr::Ok),
                    "孤儿条目显式拆除收尾");
            }
        }

        // 代次失效：槽位转一圈复用后，旧 token 不复燃（垫片句柄表
        // 低 10 位槽号 + 高位代次；1100 次创建销毁必然跨圈）。
        {
            const std::int32_t first = proto::obj_create(face, cls, L"");
            ProtoObject        first_owner{face, first};
            cg::test::check(r, first > 0, "代次用例首句柄");
            cg::test::check(
                r,
                first_owner.destroy()
                    == static_cast<std::int32_t>(host::ObjErr::Ok),
                "首句柄拆除");
            bool all_live = true;
            for (int i = 0; i < 1100; ++i)
            {
                const std::int32_t t = proto::obj_create(face, cls, L"");
                ProtoObject        owner{face, t};
                if (t <= 0
                    || owner.destroy()
                           != static_cast<std::int32_t>(host::ObjErr::Ok))
                {
                    all_live = false;
                    break;
                }
            }
            cg::test::check(r, all_live, "槽复用循环 1100 次全活");
            const auto relic =
                proto::obj_call(face, cls, first, "Extract", argv);
            cg::test::check(
                r,
                relic.rc
                    == static_cast<std::int32_t>(host::ObjErr::StaleHandle),
                std::format("槽复用后旧 token 不复燃: rc={}", relic.rc));
        }

        // 协议错误码面：坏句柄 / 从未存在的 token / 未知方法 /
        // 缺实参 / 拆除钩子不在分派面 / 求值类构造失败。
        {
            cg::test::check(
                r,
                proto::obj_call(face, cls, 0, "Extract", argv).rc
                    == static_cast<std::int32_t>(host::ObjErr::BadHandle),
                "零值句柄 BadHandle");
            cg::test::check(
                r,
                proto::obj_call(face, cls, 424242, "Extract", argv).rc
                    == static_cast<std::int32_t>(host::ObjErr::StaleHandle),
                "从未存在的 token StaleHandle");
            const std::int32_t token = proto::obj_create(face, cls, L"");
            cg::test::check(r, token > 0, "错误码用例句柄");
            if (token > 0)
            {
                const ProtoObject owner{face, token};
                cg::test::check(
                    r,
                    proto::obj_call(face, cls, token, "NoSuchMethod", {}).rc
                        == static_cast<std::int32_t>(
                            host::ObjErr::UnknownMethod),
                    "未知方法 UnknownMethod");
                cg::test::check(
                    r,
                    proto::obj_call(face, cls, token, "Extract", {}).rc
                        == static_cast<std::int32_t>(host::ObjErr::Payload),
                    "缺 script 实参 Payload");
                cg::test::check(
                    r,
                    proto::obj_call(face, cls, token, "OnFinalRelease", {}).rc
                        == static_cast<std::int32_t>(
                            host::ObjErr::UnknownMethod),
                    "拆除钩子不在分派面");
                // 没有显式拆除：对象随拥有者离开这个块时收尾。
            }
            cg::test::check(
                r,
                proto::obj_create(
                    face,
                    host::ObjClass::MsbuildEval,
                    L"Z:\\ccg-no-such-dir")
                    == static_cast<std::int32_t>(host::ObjErr::Construct),
                "MsbuildEval 坏目录 Construct");
        }

        host::reset_for_tests();
#endif // defined(_WIN32)
    }

    /**
     * 7 线承载面（hostfxr 线在场的机器跑）：SelectStringCore 全流程
     * 可用、MsbuildEval 请求返回"不被本线承载"专用码。
     */
    void csobj_line7_cases(R& r)
    {
#if defined(_WIN32)
        if (!engine_ready())
        {
            return;
        }
        cg::test::EnvScope forbid{"CCG_PSHOST_FORBID"};
        forbid.set("clr51"); // 逼降级链只剩 hostfxr 候选
        host::reset_for_tests();
        if (!host::parse_via_engine("sls line7_probe").has_value())
        {
            // hostfxr 线不可用的机器：该形态无法构造，跳过（打印
            // 降级链诊断以便区分"机器无线"与"线假性 miss"）。
            std::println("csobj_line7: skip, detail={}", host::last_detail());
            host::reset_for_tests();
            return;
        }
        cg::test::check(
            r,
            host::last_route() != host::Route::Clr51,
            "7 线形态已就位");
        const host::Face     face = host::Face::SelectString;
        const host::ObjClass cls = host::ObjClass::SelectStringCore;
        const auto           argv = script_argv("Select-String -Pattern foo");
        const std::int32_t   token = proto::obj_create(face, cls, L"");
        ProtoObject          owner{face, token};
        cg::test::check(r, token > 0, "7 线 SelectStringCore 建立");
        if (token > 0)
        {
            const auto out = proto::obj_call(face, cls, token, "Extract", argv);
            cg::test::check(
                r,
                out.ok(),
                std::format("7 线 Extract: {}", out.rc));
            cg::test::check(
                r,
                owner.destroy() == static_cast<std::int32_t>(host::ObjErr::Ok),
                "7 线拆除");
        }
        cg::test::check(
            r,
            proto::obj_create(face, host::ObjClass::MsbuildEval, L"")
                == static_cast<std::int32_t>(host::ObjErr::NotOnLine),
            "7 线请求 MsbuildEval 返回 NotOnLine");
        host::reset_for_tests();
#endif // defined(_WIN32)
    }

    /**
     * 句柄表与计数并发：多线程同句柄 ObjCall 与 CsRef 拷贝/析构
     * 交错，全部调用成功且会话句柄终态仍活（垫片 ConcurrentDictionary
     * 与 C++ 原子计数的联合面）。
     */
    void csobj_concurrent_cases(R& r)
    {
#if defined(_WIN32)
        if (!engine_ready())
        {
            return;
        }
        cg::test::EnvScope forbid{"CCG_PSHOST_FORBID"};
        forbid.set("hostfxr");
        host::reset_for_tests();
        std::optional<host::CsRef> ref = host::open_select_string();
        cg::test::check(r, ref.has_value(), "并发用例会话建立");
        if (!ref.has_value())
        {
            host::reset_for_tests();
            return;
        }
        const auto       argv = script_argv("Select-String -Pattern foo");
        std::atomic<int> failures{0};
        std::vector<std::thread> workers;
        constexpr int            kThreads = 8;
        constexpr int            kRounds = 30;
        for (int w = 0; w < kThreads; ++w)
        {
            workers.emplace_back(
                [&ref, &argv, &failures]
                {
                    for (int i = 0; i < kRounds; ++i)
                    {
                        if (!ref->call("Extract", argv).ok())
                        {
                            failures.fetch_add(1);
                        }
                        const host::CsRef copy{*ref}; // 并发 AddRef
                        if (!copy.call("Extract", argv).ok())
                        {
                            failures.fetch_add(1);
                        }
                    } // copy 并发 Release（计数不归零）
                });
        }
        for (std::thread& worker : workers)
        {
            worker.join();
        }
        cg::test::check(
            r,
            failures.load() == 0,
            std::format("并发失败计数 {}", failures.load()));
        cg::test::check(
            r,
            ref->call("Extract", argv).ok(),
            "并发后会话句柄仍活");
        ref.reset(); // 归零拆除
        host::reset_for_tests();
#endif // defined(_WIN32)
    }

    const cg::test::Registrar reg_csobj_lifecycle{
        "shellscan.csobj_lifecycle",
        &csobj_lifecycle_cases};
    const cg::test::Registrar reg_csobj_line7{
        "shellscan.csobj_line7",
        &csobj_line7_cases};
    const cg::test::Registrar reg_csobj_concurrent{
        "shellscan.csobj_concurrent",
        &csobj_concurrent_cases};

} // namespace

// NOLINTNEXTLINE(clang-diagnostic-main-attached-to-named_module) 用 module-unit
// main 是有意为之：项目每个源文件都是具名模块单元；普通 TU
// 的 main 会破坏这项全模块政策
export int main() { return cg::test::run_all(); }
