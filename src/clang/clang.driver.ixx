module;
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/**
 * @module cg.clang.driver
 *
 * @brief 运行时工具链驱动器发现与讯问（可移植性契约：任何构建输入
 * 与二进制不含机器绝对工具链路径）：ccg 绝不烤死 include 路径。
 *
 * 驱动器被讯问其自身的默认值：-print-resource-dir 与 `-E -v -x <lang>
 * -` 在 stderr 报告的 include 搜索列表。该报告列表是补充项的事实
 * 来源——因此驱动器必须描述与已加载 LLVM 引擎相同的安装（按数据库
 * 世界观）。
 *
 * @internal
 * 解析顺序：先锚定在已加载引擎库位置的候选（其目录，再按 bin/ 与
 * lib/ 布局取安装前缀），然后显式覆盖——CCG_LLVM_ROOT 环境变量或
 * `ccg index --llvm-root` 选项（CLI 仅在该 flag 实际选定引擎时设置
 * 后者；数据库的 llvm-path 配置始终优先于它）——最后 PATH 搜索。
 * env/PATH 两级保留为纯引擎布局（旁边没有驱动器的孤零 libclang.dll）
 * 的兜底；覆盖指向无驱动器的前缀时查找失败而非静默下落。结果按已
 * 加载引擎缓存（键为 loaded_clang_library_path()）：一个进程内引擎
 * 会在索引收尾之间于静止期切换（按数据库世界观，如 `ccg msbuild
 * --all`），为前一引擎构建的缓存解析绝不能补充经由新引擎的解析。
 * 全局模块片段说明：windows.h 是允许置于全局模块片段的系统头
 * ——驱动器发现需要 Windows 模块 API（哪个
 * DLL 已加载）与窄宽互转（MultiByteToWideChar 的 CP_ACP 面）；子进程
 * 讯问已收敛到 cg.util.platform 的捕获原语 run_child。
 */
export module cg.clang.driver;

import std;
import cg.util;
import cg.util.cstd;
import cg.clang.capi;

export namespace cg::clang
{

    /**
     * @brief 驱动器讯问所得的关于本安装的全部信息。
     *
     * @internal include 搜索列表完全按驱动器报告的原样保留（顺序不变），
     * 分别对应 C++ 与 C 翻译单元。
     */
    struct DriverInfo
    {
        std::string driver;       // clang++/clang 可执行文件；未找到时为 ""
        std::string resource_dir; // -print-resource-dir 输出；失败时为 ""
        std::vector<std::string> cxx_include_dirs;
        std::vector<std::string> c_include_dirs;
        // `--driver-mode=cl` 下的搜索列表。cl 方言不读 libc++，而是走
        // MSVC STL/Windows Kits（或记录的 INCLUDE），与 GNU 模式完全不同，
        // 必须单独讯问，否则闭包会哈希错头文件集合。
        std::vector<std::string> cl_cxx_include_dirs;
        std::vector<std::string> cl_c_include_dirs;
    };

    namespace detail
    {

        /**
         * @brief 进程级驱动器状态：可选的显式根（--llvm-root，首次解析前设置）
         * 加缓存的解析结果。
         *
         * @internal 缓存键为已加载引擎（cached_engine）：引擎在静止期切换时
         * driver_info() 丢弃缓存，使补充项始终描述正在解析的安装、绝不描述
         * 先前加载的。由一把互斥锁保护：索引工作线程池会并发调用
         * driver_info()。
         */
        struct DriverState
        {
            std::mutex                mutex;
            std::string               root_override; // "" = 无覆盖
            bool                      override_set = false;
            std::optional<DriverInfo> cached;
            std::string cached_engine; // cached 解析时所针对的已加载库路径
        };

        auto driver_state() -> DriverState&
        {
            static DriverState state;
            return state;
        }

        struct ProcResult
        {
            bool        started = false;
            int         exit_code = -1;
            std::string out;
            std::string err;
        };

        /**
         * @brief Windows API 边界的窄字符与宽字符互转（刻意 CP_ACP，非
         * UTF-8）。
         *
         * 输入来自 ANSI 代码页的世界（PATH 环境值、旧式进程输出），不是
         * UTF-8 载荷；UTF-8 边界一律用 cg::util::platform::widen_utf8——两者
         * 代码页语义不同，合并即错，故并存且不得互相替换。
         */
        [[nodiscard]]
        auto to_wide(std::string_view text) -> std::wstring
        {
            if (text.empty())
            {
                return {};
            }
            const int len = MultiByteToWideChar(
                CP_ACP,
                0,
                text.data(),
                static_cast<int>(text.size()),
                nullptr,
                0);
            std::wstring wide(static_cast<std::size_t>(len), L'\0');
            MultiByteToWideChar(
                CP_ACP,
                0,
                text.data(),
                static_cast<int>(text.size()),
                wide.data(),
                len);
            return wide;
        }

        /**
         * @brief 运行 `exe args...`：喂空 stdin 并捕获 stdout 与 stderr。
         *
         * @internal
         * cg::util::platform::run_child 捕获原语的驱动器配置层：EofPipe 喂空
         * stdin（写端派生后立即关闭，子进程即时见 EOF）、两路并发排水、
         * CREATE_NO_WINDOW、到点终止。并发排水与超时的死锁教训（失败的接口
         * 单元向空 stdout 的 stderr 写数兆诊断，顺序排空会挂死整个索引运行）
         * 沉淀在原语的行为清单里，此处只保留驱动器特有的差异：
         * lpCurrentDirectory 逐子进程传递（线程池并发重放多个 producer，
         * 进程 cwd 是共享状态，绝不能 chdir）；无路径分隔符的可执行名
         * （cmd.exe、PATH 上的 vswhere.exe）必须交给 CreateProcess 的标准
         * PATH 搜索（application 点名时不做搜索，裸名会被当作相对当前目录
         * 而失败）。等待上限默认 600 秒，宽于最慢的合法调用（完整 std.pcm
         * 构建约 2 秒；模板错误级联加的是秒级不是分钟级）；超时则终止子
         * 进程，非零退出码经调用方常规失败路径呈现——任何驱动器运行都不得
         * 永久挂起索引。
         */
        [[nodiscard]]
        auto run_process(
            const std::wstring&              exe,
            const std::vector<std::wstring>& args,
            const std::wstring&              cwd = {},
            std::chrono::milliseconds timeout = std::chrono::milliseconds{
                600'000}) -> ProcResult
        {
            std::wstring cmdline = L"\"" + exe + L"\"";
            for (const std::wstring& arg : args)
            {
                cmdline +=
                    L" " + cg::util::platform::quote_for_command_line(arg);
            }
            cg::util::platform::ChildRunSpec spec;
            spec.cmdline = std::move(cmdline);
            if (exe.find_first_of(L"/\\") != std::wstring::npos)
            {
                spec.application = exe;
            }
            spec.cwd = cwd;
            spec.stdin_mode = cg::util::platform::ChildStdin::EofPipe;
            spec.kill_after = timeout;
            const cg::util::platform::ChildRunResult run =
                cg::util::platform::run_child(spec);
            ProcResult result;
            result.started = run.started && run.pipes_ok;
            result.exit_code = run.exit_code;
            result.out = std::move(run.out);
            result.err = std::move(run.err);
            return result;
        }

        /**
         * @brief 引擎库路径的目录（空时返回 ""）。
         *
         * @internal 下方一切引擎派生驱动器候选的锚点——它取代了旧的“链接器
         * 绑定了哪个 libclang”问题：cg.clang.capi 记录加载器打开的路径，
         * 因此引擎的安装也命名了驱动器的安装。
         */
        [[nodiscard]]
        auto library_dir_of(const std::string& library_path) -> std::string
        {
            if (library_path.empty())
            {
                return {};
            }
            return std::filesystem::path{library_path}
                .parent_path()
                .lexically_normal()
                .generic_string();
        }

        /** @brief dir 下 clang++.exe / clang.exe 中首个存在的候选。 */
        [[nodiscard]]
        auto driver_under(const std::string& dir) -> std::string
        {
            namespace fs = std::filesystem;
            if (dir.empty())
            {
                return {};
            }
            std::error_code ec;
            for (const char* name : {"clang++.exe", "clang.exe"})
            {
                const fs::path candidate = fs::path{dir} / name;
                if (fs::is_regular_file(candidate, ec))
                {
                    return candidate.lexically_normal().generic_string();
                }
            }
            return {};
        }

        /**
         * @brief PATH 扫描核心。
         *
         * @internal 手工扫描，绝不搜索当前工作目录；以 PATH 文本作参数，
         * 保持解析链核心纯净可测。
         */
        [[nodiscard]]
        auto driver_on_path_from(const std::string& path_env) -> std::string
        {
            namespace fs = std::filesystem;
            if (path_env.empty())
            {
                return {};
            }
            std::error_code   ec;
            std::string       entry;
            std::stringstream env{path_env};
            while (std::getline(env, entry, ';'))
            {
                if (entry.empty())
                {
                    continue;
                }
                const std::string hit = driver_under(entry);
                if (!hit.empty())
                {
                    return hit;
                }
            }
            return {};
        }

        /**
         * @brief 锚定在 LLVM 引擎库所在目录（解析实际经由的安装）的驱动器
         * 候选。
         *
         * 依次尝试：目录本身；bin/ 与 lib/ 安装布局下取安装前缀与其 bin/；
         * 库位于前缀根时取该目录自己的 bin/。无匹配时返回 ""。
         */
        [[nodiscard]]
        auto driver_near(const std::string& library_dir) -> std::string
        {
            namespace fs = std::filesystem;
            if (library_dir.empty())
            {
                return {};
            }
            if (const std::string hit = driver_under(library_dir); !hit.empty())
            {
                return hit;
            }
            const fs::path base{library_dir};
            if (base.filename() == "bin" || base.filename() == "lib")
            {
                const fs::path    prefix = base.parent_path();
                const std::string hit = driver_under(prefix.generic_string());
                return !hit.empty()
                           ? hit
                           : driver_under((prefix / "bin").generic_string());
            }
            return driver_under((base / "bin").generic_string());
        }

        /**
         * @brief 一个显式 ROOT 拼写下的驱动器。
         *
         * 依次尝试前缀本身与其 bin/；root 是文件（`ccg config set llvm-path`
         * 接受的引擎库文件拼写）时改用锚定该文件目录的候选。root 未命中
         * 驱动器时返回 ""。
         */
        [[nodiscard]]
        auto driver_from_root(const std::string& root) -> std::string
        {
            namespace fs = std::filesystem;
            if (root.empty())
            {
                return {};
            }
            std::error_code ec;
            const fs::path  base{root};
            if (fs::is_regular_file(base, ec))
            {
                return driver_near(base.parent_path().generic_string());
            }
            const std::string hit = driver_under(root);
            return !hit.empty() ? hit
                                : driver_under((base / "bin").generic_string());
        }

        /**
         * @brief 从 `-E -v` 的 stderr 块中提取 include 搜索列表。
         *
         * 取 include 搜索标记之间的块；保留报告顺序、路径规范化。
         *
         * @internal 行首单个空格是报告缩进；其余空白行（及引号 include 标记
         * 回显）跳过。
         */
        [[nodiscard]]
        auto include_dirs_from_report(std::string_view stderr_text)
            -> std::vector<std::string>
        {
            constexpr std::string_view begin_marker =
                "#include <...> search starts here:";
            constexpr std::string_view end_marker = "End of search list.";
            const std::size_t          begin = stderr_text.find(begin_marker);
            if (begin == std::string_view::npos)
            {
                return {};
            }
            const std::size_t      end = stderr_text.find(end_marker, begin);
            const std::string_view block = stderr_text.substr(
                begin + begin_marker.size(),
                end == std::string_view::npos
                    ? std::string_view::npos
                    : end - (begin + begin_marker.size()));

            std::vector<std::string> dirs;
            std::string_view         rest = block;
            while (!rest.empty())
            {
                const std::size_t nl = rest.find('\n');
                std::string_view  line = rest.substr(
                    0,
                    nl == std::string_view::npos ? rest.size() : nl);
                rest = nl == std::string_view::npos ? std::string_view{}
                                                    : rest.substr(nl + 1);
                if (!line.empty() && line.front() == ' ')
                {
                    line.remove_prefix(1);
                }
                while (!line.empty()
                       && (line.front() == ' ' || line.front() == '\t'))
                {
                    line.remove_prefix(1);
                }
                if (line.empty())
                {
                    continue;
                }
                if (const std::size_t fw = line.find(" (framework directory)");
                    fw != std::string_view::npos)
                {
                    line = line.substr(0, fw);
                }
                if (line.empty() || line.front() == '#')
                {
                    continue;
                }
                std::string dir{line};
                while (!dir.empty()
                       && (dir.back() == '\r' || dir.back() == ' '
                           || dir.back() == '\t'))
                {
                    dir.pop_back();
                }
                if (!dir.empty())
                {
                    dirs.push_back(cg::util::norm_path(dir));
                }
            }
            return dirs;
        }

        /**
         * @brief 纯净可测的解析链核心（engine::resolve_library_from 的驱动器
         * 孪生）。
         *
         * 显式覆盖根胜出且成为唯一来源（前缀、bin/ 目录、引擎库文件三种拼写
         * 皆可）；否则先锚定已加载引擎库位置的候选，再环境根，再 path_env 上
         * 的首个驱动器。返回驱动器可执行路径；无可解析时返回 ""。
         *
         * @internal 引擎锚点优先于环境，保证补充项落在实际解析的安装上
         * （按数据库世界观）；env/PATH 保留为纯引擎布局的兜底。
         */
        [[nodiscard]]
        auto chain_driver_from(
            const std::string& override_root,
            const std::string& engine_library,
            const std::string& env_root,
            const std::string& path_env) -> std::string
        {
            if (!override_root.empty())
            {
                return driver_from_root(override_root);
            }
            std::string driver;
            if (!engine_library.empty())
            {
                driver = driver_near(library_dir_of(engine_library));
            }
            if (driver.empty())
            {
                driver = driver_from_root(env_root);
            }
            if (driver.empty())
            {
                driver = driver_on_path_from(path_env);
            }
            return driver;
        }

        /**
         * @brief 对一个已解析的驱动器可执行文件做完整讯问。
         *
         * -print-resource-dir 加上完全由它报告的 C++/C include 搜索列表。
         * driver 为空时返回空 DriverInfo——由调用方上报可行动错误。
         */
        [[nodiscard]]
        auto interrogate_driver(const std::string& driver) -> DriverInfo
        {
            DriverInfo info;
            if (driver.empty())
            {
                return info;
            }
            info.driver = driver;

            const std::wstring exe = to_wide(driver);
            const ProcResult   resource =
                run_process(exe, {L"-print-resource-dir"});
            if (resource.started && resource.exit_code == 0)
            {
                info.resource_dir =
                    cg::util::norm_path(cg::util::trim(resource.out));
            }

            const ProcResult cxx_report =
                run_process(exe, {L"-E", L"-v", L"-x", L"c++", L"-"});
            if (cxx_report.started && cxx_report.exit_code == 0)
            {
                info.cxx_include_dirs =
                    include_dirs_from_report(cxx_report.err);
            }
            const ProcResult c_report =
                run_process(exe, {L"-E", L"-v", L"-x", L"c", L"-"});
            if (c_report.started && c_report.exit_code == 0)
            {
                info.c_include_dirs = include_dirs_from_report(c_report.err);
            }
            // cl 方言单独讯问：同一二进制、不同 driver 模式，搜索链完全不同。
            const ProcResult cl_cxx_report = run_process(
                exe,
                {L"--driver-mode=cl", L"-E", L"-v", L"-x", L"c++", L"-"});
            if (cl_cxx_report.started && cl_cxx_report.exit_code == 0)
            {
                info.cl_cxx_include_dirs =
                    include_dirs_from_report(cl_cxx_report.err);
            }
            const ProcResult cl_c_report = run_process(
                exe,
                {L"--driver-mode=cl", L"-E", L"-v", L"-x", L"c", L"-"});
            if (cl_c_report.started && cl_c_report.exit_code == 0)
            {
                info.cl_c_include_dirs =
                    include_dirs_from_report(cl_c_report.err);
            }
            return info;
        }

    } // namespace detail

    /**
     * @brief 设置显式驱动器根覆盖（`ccg index --llvm-root` 的值）。
     *
     * 仅当该 flag 实际选定引擎时传入——数据库的 llvm-path 配置始终优先
     * 于它。根为空串时清除覆盖、回到引擎锚定发现。覆盖一经设置即压过
     * CCG_LLVM_ROOT 与发现链；须在首次解析前运行（CLI 在索引开始前设置）。
     */
    void set_driver_root_override(const std::string& root)
    {
        auto&                  state = detail::driver_state();
        const std::scoped_lock lock{state.mutex};
        state.root_override = root;
        state.override_set = true;
        state.cached.reset();
        state.cached_engine.clear();
    }

    /**
     * @brief 一次性解析 + 讯问，按已加载引擎缓存。
     *
     * 链条：--llvm-root 覆盖（CLI 设置时）、锚定在已加载 libclang.dll
     * 自身位置的候选、$CCG_LLVM_ROOT、PATH 搜索。引擎锚点优先于环境，
     * 使补充项始终描述正在解析的安装；env/PATH 保留为纯引擎布局的兜底。
     *
     * @internal 已加载引擎在静止期变化（同进程后续索引收尾切换了世界观，
     * 例如 `ccg msbuild --all` 每配置库一个收尾）时丢弃缓存、针对新引擎
     * 重新解析——驱动器绝不继续描述前一个安装。未找到时返回的
     * DriverInfo 的 driver() 为空，消费方以可行动错误失败（绝不烤死
     * 路径）。
     */
    [[nodiscard]]
    auto driver_info() -> const DriverInfo&
    {
        auto&                  state = detail::driver_state();
        const std::scoped_lock lock{state.mutex};
        const std::string      engine_library = loaded_clang_library_path();
        if (state.cached.has_value() && state.cached_engine != engine_library)
        {
            state.cached.reset();
        }
        if (!state.cached.has_value())
        {
            std::string root;
            if (state.override_set)
            {
                root = state.root_override;
            }
            std::string env_root;
            if (const auto env = cg::util::cstd::get_env("CCG_LLVM_ROOT"))
            {
                env_root = *env;
            }
            std::string path_env;
            if (const auto env = cg::util::cstd::get_env("PATH"))
            {
                path_env = *env;
            }
            state.cached = detail::interrogate_driver(
                detail::chain_driver_from(
                    root,
                    engine_library,
                    env_root,
                    path_env));
            state.cached_engine = engine_library;
        }
        return *state.cached;
    }

    /**
     * @brief 以显式根解析（测试/逃生口入口）。
     *
     * 根压过引擎锚点、环境与 PATH 链并成为唯一来源——未命中驱动器的根
     * 解析为空 DriverInfo，而非静默回落。
     */
    [[nodiscard]]
    auto resolve_driver(const std::string& root) -> DriverInfo
    {
        return detail::interrogate_driver(detail::driver_from_root(root));
    }

    /**
     * @brief driver_info() 的纯净可测核心。
     *
     * 同一条链（覆盖 > 引擎锚定候选 > 环境根 > PATH），以显式输入代替
     * 进程状态。返回的 DriverInfo 携带解析到的驱动器路径；对非可执行
     * 探针目标的讯问只会得到空的补充列表。
     */
    [[nodiscard]]
    auto resolve_driver_chain_from(
        const std::string& override_root,
        const std::string& engine_library,
        const std::string& env_root,
        const std::string& path_env) -> DriverInfo
    {
        return detail::interrogate_driver(
            detail::chain_driver_from(
                override_root,
                engine_library,
                env_root,
                path_env));
    }

    /**
     * @brief 给定语言与方言的 TU 应兜底使用的 include 列表。
     *
     * @internal `cl_mode` 为真时返回 `--driver-mode=cl` 的搜索链（MSVC
     * STL/Windows Kits），否则返回 GNU 链（libc++）。混用会让闭包和
     * 文本回退都指向错误的头文件集合。
     */
    [[nodiscard]]
    auto driver_include_dirs(bool cxx_tu, bool cl_mode = false)
        -> const std::vector<std::string>&
    {
        const DriverInfo& info = driver_info();
        if (cl_mode)
        {
            return cxx_tu ? info.cl_cxx_include_dirs : info.cl_c_include_dirs;
        }
        return cxx_tu ? info.cxx_include_dirs : info.c_include_dirs;
    }

    /**
     * @brief 从 cl.exe 绝对路径反推的 MSVC 工具集线索。
     *
     * @internal 外来 cdb 没有捕获 INCLUDE 时的唯一可信线索：路径形如
     * `...\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\cl.exe`。只做纯文本
     * 解析（正反斜杠、大小写不敏感），平台无关、可单测。
     */
    struct MsvcToolsetPath
    {
        std::string version;     // "14.44.35207"；无法解析时 ""
        std::string target_arch; // "x64"/"x86"/"arm64"/"arm"
        std::string vs_root;     // `.../VC/Tools/MSVC` 之前的安装根
        std::string vcvars_bat;  // 按目标架构选出的 vcvars 脚本路径
    };

    /**
     * @brief 解析 cl.exe 路径里的工具集版本与目标架构。
     *
     * 版本必须至少三段纯数字（`14.minor.build`）；否则整个结果为空（调用
     * 方据此只做方言修正，见 MSBuild 管线规格 的覆盖边界）。
     */
    [[nodiscard]]
    auto parse_msvc_toolset_path(std::string_view cl_path) -> MsvcToolsetPath
    {
        MsvcToolsetPath   out;
        const std::string forward = cg::util::to_forward_slashes(cl_path);
        const std::string lower = cg::util::ascii_lower(forward);
        constexpr std::string_view marker{"/vc/tools/msvc/"};
        const std::size_t          at = lower.find(marker);
        if (at == std::string::npos)
        {
            return out;
        }
        const std::size_t      begin = at + marker.size();
        const std::size_t      end = forward.find('/', begin);
        const std::string_view version = std::string_view{forward}.substr(
            begin,
            end == std::string::npos ? std::string::npos : end - begin);
        // 版本段必须是点分纯数字且至少 14.minor.build。
        std::size_t dots = 0;
        bool        digit_seen = false;
        for (const char c : version)
        {
            if (c == '.')
            {
                if (!digit_seen)
                {
                    return MsvcToolsetPath{};
                }
                ++dots;
                digit_seen = false;
            }
            else if (c >= '0' && c <= '9')
            {
                digit_seen = true;
            }
            else
            {
                return MsvcToolsetPath{};
            }
        }
        if (dots < 2 || !digit_seen)
        {
            return MsvcToolsetPath{};
        }
        out.version = std::string{version};
        out.vs_root = forward.substr(0, at);
        // .../bin/`<Hostx64>`/`<x64>`/cl.exe：bin 之后两段分别是 host 与
        // target。
        const std::size_t bin = lower.find("/bin/", end);
        if (bin != std::string::npos)
        {
            const std::size_t host_begin = bin + 5;
            const std::size_t host_end = forward.find('/', host_begin);
            const std::size_t arch_begin = host_end == std::string::npos
                                               ? std::string::npos
                                               : host_end + 1;
            const std::size_t arch_end = arch_begin == std::string::npos
                                             ? std::string::npos
                                             : forward.find('/', arch_begin);
            if (arch_begin != std::string::npos)
            {
                out.target_arch = cg::util::ascii_lower(forward.substr(
                    arch_begin,
                    arch_end == std::string::npos ? std::string::npos
                                                  : arch_end - arch_begin));
            }
        }
        const std::string_view script = [&]() -> std::string_view
        {
            if (out.target_arch == "x86")
            {
                return "vcvars32.bat";
            }
            if (out.target_arch == "arm64")
            {
                return "vcvarsarm64.bat";
            }
            if (out.target_arch == "arm")
            {
                return "vcvarsarm.bat";
            }
            return "vcvars64.bat"; // x64 与未知架构的默认
        }();
        if (!out.vs_root.empty() && !out.target_arch.empty())
        {
            out.vcvars_bat = std::format(
                "{}/VC/Auxiliary/Build/{}",
                out.vs_root,
                std::string{script});
        }
        return out;
    }

    /**
     * @brief 把 `14.minor.build[.rev]` 工具集版本映射为 clang 的 MSVC
     * 兼容版本 `19.minor.build`；段数不足时返回空。
     *
     * @internal 与 msbuild 适配器的规则同源：实测完整三段精确复现
     * `_MSC_FULL_VER`（14.44.35207 -> 194435207）。
     */
    [[nodiscard]]
    auto msvc_compat_version(std::string_view toolset_version) -> std::string
    {
        std::vector<int> parts;
        std::size_t      start = 0;
        while (start <= toolset_version.size())
        {
            const std::size_t      dot = toolset_version.find('.', start);
            const std::string_view part = toolset_version.substr(
                start,
                dot == std::string_view::npos ? std::string_view::npos
                                              : dot - start);
            if (part.empty())
            {
                return {};
            }
            int value = 0;
            for (const char c : part)
            {
                if (c < '0' || c > '9')
                {
                    return {};
                }
                value = value * 10 + (c - '0');
            }
            parts.push_back(value);
            if (dot == std::string_view::npos)
            {
                break;
            }
            start = dot + 1;
        }
        if (parts.size() < 3)
        {
            return {};
        }
        return std::format("19.{}.{}", parts[1], parts[2]);
    }

    /**
     * @brief 一次 vcvars 捕获的结果。
     *
     * @internal 失败是数据：调用方保持自动探测并如实上报
     * `system-includes: auto-detected`，绝不静默假装精确。
     */
    struct MsvcIncludeCapture
    {
        bool        ok = false;
        std::string include_env; // 成功时 vcvars 子进程里的 INCLUDE
        std::string reason;      // 失败原因（诊断用）
    };

    namespace detail
    {

        /** @brief INCLUDE 捕获缓存键（工具集版本|目标架构）与结果台账。 */
        struct MsvcEnvCache
        {
            std::mutex                                          mutex;
            std::unordered_map<std::string, MsvcIncludeCapture> entries;
        };

        auto msvc_env_cache() -> MsvcEnvCache&
        {
            static MsvcEnvCache cache;
            return cache;
        }

        /** @brief vswhere 的规范位置；缺席时退回 PATH 搜索。 */
        [[nodiscard]]
        auto vswhere_executable() -> std::string
        {
            if (const auto pf = cg::util::cstd::get_env("ProgramFiles(x86)");
                pf.has_value() && !pf->empty())
            {
                std::string candidate =
                    *pf
                    + "\\Microsoft Visual Studio\\Installer\\"
                      "vswhere.exe";
                std::error_code ec;
                if (std::filesystem::is_regular_file(candidate, ec))
                {
                    return candidate;
                }
            }
            return "vswhere.exe";
        }

        /** @brief `vswhere -property installationPath` 的首个非空行。 */
        [[nodiscard]]
        auto query_vs_install_root() -> std::string
        {
            const ProcResult r = run_process(
                to_wide(vswhere_executable()),
                {L"-latest",
                 L"-products",
                 L"*",
                 L"-requires",
                 L"Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                 L"-property",
                 L"installationPath"},
                {},
                std::chrono::milliseconds{10'000});
            if (!r.started || r.exit_code != 0)
            {
                return {};
            }
            std::string_view text{r.out};
            while (!text.empty())
            {
                const std::size_t nl = text.find('\n');
                std::string_view  line = text.substr(
                    0,
                    nl == std::string_view::npos ? text.size() : nl);
                if (!line.empty() && line.back() == '\r')
                {
                    line.remove_suffix(1);
                }
                if (!line.empty())
                {
                    return std::string{line};
                }
                if (nl == std::string_view::npos)
                {
                    break;
                }
                text.remove_prefix(nl + 1);
            }
            return {};
        }

        /** @brief 从 `set INCLUDE` 输出里取 INCLUDE 值。 */
        [[nodiscard]]
        auto parse_include_line(std::string_view text) -> std::string
        {
            while (!text.empty())
            {
                const std::size_t nl = text.find('\n');
                std::string_view  line = text.substr(
                    0,
                    nl == std::string_view::npos ? text.size() : nl);
                if (!line.empty() && line.back() == '\r')
                {
                    line.remove_suffix(1);
                }
                constexpr std::string_view prefix{"INCLUDE="};
                if (line.size() >= prefix.size()
                    && line.substr(0, prefix.size()) == prefix)
                {
                    return std::string{line.substr(prefix.size())};
                }
                if (nl == std::string_view::npos)
                {
                    break;
                }
                text.remove_prefix(nl + 1);
            }
            return {};
        }

        /**
         * @brief 从 cdb 目录的 CMakeCache.txt 读目标 Windows SDK 版本。
         *
         * @internal 辅助来源（`CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION`，回退
         * `CMAKE_SYSTEM_VERSION`）。vcvars 不提供可靠的 SDK 选择开关（实测
         * 预置 WindowsSDKVersion 会被覆盖），因此它只用于校验：重建出的
         * INCLUDE 不含请求的 SDK 时宁可降级为自动探测并如实标注。
         */
        [[nodiscard]]
        auto cmake_sdk_version(std::string_view build_dir) -> std::string
        {
            if (build_dir.empty())
            {
                return {};
            }
            std::ifstream in{
                std::filesystem::path{
                    std::string{build_dir} + "/CMakeCache.txt"},
                std::ios::binary};
            if (!in)
            {
                return {};
            }
            std::string line;
            std::string fallback;
            while (std::getline(in, line))
            {
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }
                const auto value_after =
                    [&line](std::string_view key) -> std::string
                {
                    if (!line.starts_with(key))
                    {
                        return {};
                    }
                    const std::size_t eq = line.find('=');
                    return eq == std::string::npos ? std::string{}
                                                   : line.substr(eq + 1);
                };
                if (std::string v = value_after(
                        "CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION:");
                    !v.empty())
                {
                    return v;
                }
                if (std::string v = value_after("CMAKE_SYSTEM_VERSION:");
                    !v.empty())
                {
                    fallback = std::move(v);
                }
            }
            return fallback;
        }

        /**
         * @brief 用 vcvars 子进程复现某个工具集版本的 INCLUDE 环境。
         *
         * 先按 cl 路径推出的 VS 根找 `VC\Auxiliary\Build\vcvars*.bat`；找不到
         * 时用 vswhere 定位安装根。脚本经唯一临时 .cmd 调用（cmd.exe 对引号
         * 的解析规则与 CRT argv 不同，直接把带空格路径塞进 /c 会碎）。
         * `-vcvars_ver` 先试完整版本，失败再试 `14.minor`（部分 vcvars 版本
         * 只接受两段）。每个（版本, 架构）只做一次并缓存。
         */
        [[nodiscard]]
        auto capture_msvc_include_once(
            const MsvcToolsetPath& toolset,
            const std::string&     want_sdk) -> MsvcIncludeCapture
        {
            namespace fs = std::filesystem;
            MsvcIncludeCapture out;
            std::string        vcvars = toolset.vcvars_bat;
            std::error_code    ec;
            if (!vcvars.empty() && !fs::is_regular_file(vcvars, ec))
            {
                vcvars.clear();
            }
            if (vcvars.empty())
            {
                const std::string root = query_vs_install_root();
                if (root.empty())
                {
                    out.reason =
                        "vswhere could not locate a Visual Studio install";
                    return out;
                }
                const std::string script =
                    toolset.target_arch == "x86"     ? "vcvars32.bat"
                    : toolset.target_arch == "arm64" ? "vcvarsarm64.bat"
                    : toolset.target_arch == "arm"   ? "vcvarsarm.bat"
                                                     : "vcvars64.bat";
                vcvars = std::format("{}/VC/Auxiliary/Build/{}", root, script);
                if (!fs::is_regular_file(vcvars, ec))
                {
                    out.reason =
                        std::format("vcvars script not found under {}", root);
                    return out;
                }
            }
            // 唯一临时名：并发索引进程/线程绝不互相覆盖。
            static std::atomic<std::uint64_t> sequence{0};
            const auto                        stamp =
                std::chrono::steady_clock::now().time_since_epoch().count();
            const fs::path tmp = fs::temp_directory_path(ec)
                                 / std::format(
                                     "ccg-vcvars-{}-{}.cmd",
                                     stamp,
                                     sequence.fetch_add(1));
            std::string    vcvars_native = vcvars;
            for (char& c : vcvars_native)
            {
                if (c == '/')
                {
                    c = '\\';
                }
            }
            const auto invoke = [&](const std::string& vcvars_ver) -> ProcResult
            {
                std::string script = "@echo off\r\ncall \"";
                script += vcvars_native;
                script += "\" -vcvars_ver=";
                script += vcvars_ver;
                script +=
                    " >nul 2>&1\r\nif errorlevel 1 exit /b 1\r\nset INCLUDE\r\n";
                {
                    std::ofstream file{tmp, std::ios::binary | std::ios::trunc};
                    if (!file)
                    {
                        return {};
                    }
                    file << script;
                }
                return run_process(
                    L"cmd.exe",
                    {L"/d", L"/s", L"/c", to_wide(tmp.generic_string())},
                    {},
                    std::chrono::milliseconds{10'000});
            };
            std::string last_reason;
            auto        try_version = [&](const std::string& ver) -> bool
            {
                const ProcResult r = invoke(ver);
                if (!r.started || r.exit_code != 0)
                {
                    last_reason = std::format(
                        "vcvars start={} rc={} out={} err={}",
                        static_cast<int>(r.started),
                        r.exit_code,
                        r.out.size(),
                        r.err.size());
                    return false;
                }
                std::string include = parse_include_line(r.out);
                if (include.empty())
                {
                    last_reason = std::format(
                        "vcvars rc=0 but no INCLUDE line (out={})",
                        r.out.size());
                    return false;
                }
                // vcvars 对未知 -vcvars_ver 会静默只给 SDK 目录（rc=0）：必须
                // 确认请求的工具集 include 真的在集合里，否则宁可降级。
                const std::string lower = cg::util::ascii_lower(
                    cg::util::to_forward_slashes(include));
                const std::string want =
                    "/vc/tools/msvc/" + cg::util::ascii_lower(ver);
                if (lower.find(want) == std::string::npos)
                {
                    last_reason = "vcvars did not select the requested toolset";
                    return false;
                }
                // CMakeCache 的目标 SDK 校验：vcvars 不能可靠地钉 SDK，版本
                // 不符时宁可不假装精确。
                if (!want_sdk.empty()
                    && lower.find(want_sdk) == std::string::npos)
                {
                    last_reason = std::format(
                        "vcvars SDK mismatch (wanted {})",
                        want_sdk);
                    return false;
                }
                out.ok = true;
                out.include_env = std::move(include);
                out.reason.clear();
                return true;
            };
            // 14.44.35207 -> 先完整，再 14.44。
            const std::vector<std::string> candidates = [&]
            {
                std::vector<std::string> v{toolset.version};
                const std::size_t        first = toolset.version.find('.');
                if (first != std::string::npos)
                {
                    const std::size_t second =
                        toolset.version.find('.', first + 1);
                    if (second != std::string::npos)
                    {
                        v.push_back(toolset.version.substr(0, second));
                    }
                }
                return v;
            }();
            for (const std::string& ver : candidates)
            {
                if (try_version(ver))
                {
                    break;
                }
            }
            fs::remove(tmp, ec);
            if (!out.ok)
            {
                out.reason = std::format(
                    "vcvars did not produce an INCLUDE for toolset {} ({}): {}",
                    toolset.version,
                    toolset.target_arch,
                    last_reason);
            }
            return out;
        }

    } // namespace detail

    /**
     * @brief 捕获某个 cl.exe 对应工具集/架构的完整 INCLUDE 环境。
     *
     * 结果按（工具集版本, 目标架构, 目标 SDK）缓存；失败也缓存（一次
     * 运行的预算内绝不重复启动 vcvars）。build_dir 非空时读其
     * CMakeCache.txt 取目标 SDK 并校验重建结果。无工具集线索时直接失败，
     * 调用方只做方言修正。
     */
    [[nodiscard]]
    auto capture_msvc_include_env(
        std::string_view cl_path,
        std::string_view build_dir = {}) -> MsvcIncludeCapture
    {
        const MsvcToolsetPath toolset = parse_msvc_toolset_path(cl_path);
        if (toolset.version.empty())
        {
            MsvcIncludeCapture out;
            out.reason = "no MSVC toolset version in the compiler path";
            return out;
        }
        const std::string want_sdk = detail::cmake_sdk_version(build_dir);
        const std::string key = std::format(
            "{}|{}|{}",
            toolset.version,
            toolset.target_arch,
            want_sdk);
        detail::MsvcEnvCache&  cache = detail::msvc_env_cache();
        const std::scoped_lock lock{cache.mutex};
        if (const auto it = cache.entries.find(key); it != cache.entries.end())
        {
            return it->second;
        }
        MsvcIncludeCapture captured =
            detail::capture_msvc_include_once(toolset, want_sdk);
        return cache.entries.emplace(key, std::move(captured)).first->second;
    }

    /**
     * @brief 从 clang 版本文本取可比较的版本 token。
     *
     * （"clang version 23.1.3 (https://…)" -> "23.1.3"；文本不是 clang
     * 版本行时返回 ""。）
     *
     * @internal BMI 生产者校验的两侧共用此解析，保证同口径比较：引擎侧
     * 读 clang_getClangVersion() 输出，生产者侧读 `--version`。
     */
    [[nodiscard]]
    auto clang_version_token(std::string_view text) -> std::string
    {
        constexpr std::string_view marker = "clang version ";
        const std::size_t          at = text.find(marker);
        if (at == std::string_view::npos)
        {
            return {};
        }
        std::size_t begin = at + marker.size();
        std::size_t end = begin;
        while (end < text.size() && text[end] != ' ' && text[end] != '\n'
               && text[end] != '\r' && text[end] != '\t')
        {
            ++end;
        }
        return std::string{text.substr(begin, end - begin)};
    }

    /**
     * @brief 任意可执行文件对 `--version` 报告的编译器版本。
     *
     * 非 clang 编译器、文件缺失、任何失败都返回 ""。work_dir 解析相对的
     * 编译器拼写（记录的编译目录）。按解析到的可执行文件缓存；线程安全。
     *
     * @internal 编译数据库的 argv[0] 是其模块 flag 所喂每一个 BMI 的生产者，
     * BMI 契约要求在投喂前校验生产者 == 已加载引擎（pcm 格式锁：pcm 内
     * 嵌生产者版本串，加载侧逐位校验，版本不符即拒绝——引擎侧
     * 一律不得投喂另一 clang 构建产出的 pcm）。
     */
    [[nodiscard]]
    auto compiler_version(
        const std::string& compiler,
        const std::string& work_dir) -> std::string
    {
        if (compiler.empty())
        {
            return {};
        }
        namespace fs = std::filesystem;
        fs::path        exe{compiler};
        std::error_code ec;
        if (exe.is_relative())
        {
            exe = fs::path{work_dir} / exe;
        }
        const std::string      key = exe.lexically_normal().generic_string();
        static std::mutex      mutex;
        const std::scoped_lock lock{mutex};
        static std::unordered_map<std::string, std::string> cache;
        if (const auto it = cache.find(key); it != cache.end())
        {
            return it->second;
        }
        std::string version;
        if (fs::is_regular_file(exe, ec))
        {
            const detail::ProcResult probe =
                detail::run_process(detail::to_wide(key), {L"--version"});
            if (probe.started && probe.exit_code == 0)
            {
                const std::size_t nl = probe.out.find('\n');
                version = clang_version_token(
                    std::string_view{probe.out}.substr(0, nl));
            }
        }
        return cache.emplace(key, std::move(version)).first->second;
    }

} // namespace cg::clang
