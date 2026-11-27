module;
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#include <winioctl.h>
#include <psapi.h>
#include <shellapi.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#if defined(__APPLE__)
#include <crt_extern.h>
#include <mach-o/dyld.h>
#endif
// 进程环境块的全局变量：各平台的头文件未必声明它，这里自行声明。
extern "C" char** environ;
#endif
// x86 家族的 CPU 能力探测头（供批量位运算三档的分派探测；非 x86 不
// 定义 CG_CPUID_X86，探测恒回标量档）。
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64)                \
    || defined(_M_IX86)
#define CG_CPUID_X86 1
#if defined(_MSC_VER)
#include <immintrin.h>
#else
#include <cpuid.h>
#endif
#endif

/**
 * @module cg.util.platform
 *
 * @brief 平台守卫下的运行时探针集。
 *
 * @internal
 * 直接调用系统接口并取用、释放资源的只有封装单元：本单元与句柄、宿主
 * 桥接、崩溃转储、引擎装载等单元；另有少数单元为了不涉及资源的系统
 * 调用（取当前目录、读环境变量、探测文件属性一类）而包含平台头。
 * 本单元的每个助手都有 Windows 与 POSIX 两个分支（跨平台规则
 * 要求两分支皆实现）；宏与 C 声明永不跨模块边界，所以平台头住在全局
 * 模块片段里。本单元取到的句柄与文件描述符一律立即交给 cg.util.handle
 * 的拥有型对象，关闭调用只出现在拥有者那里；必须提前关闭的用显式复位。
 */
export module cg.util.platform;

import std;
import cg.util.cstd;
import cg.util.handle;

export namespace cg::util::platform
{

    /**
     * @brief 系统当前 ANSI 代码页对应的输入字符集名（clang -finput-charset
     *        的取值），探不到或映射未知时为空串。
     *
     * 运行时探测（Windows GetACP），绝不硬编码：同一二进制装在不同区域的
     * 机器上各自拿到正确的字符集。常见页给规范名，未知页回退 "CP`<n>`"
     * （iconv 家族的通用别名拼写）。65001（UTF-8 系统）返回空——追加
     * -finput-charset=UTF-8 没有信息量，调用方按"已对齐"处理。非 Windows
     * 无系统 ANSI 代码页概念，恒空（调用方不加旗标）。
     */
    [[nodiscard]]
    inline auto ansi_input_charset() -> std::string
    {
#if defined(_WIN32)
        const UINT cp = GetACP();
        switch (cp)
        {
        case 936:
            return "GBK";
        case 950:
            return "Big5";
        case 932:
            return "Shift_JIS";
        case 949:
            return "EUC-KR";
        case 65001:
            return {};
        default:
            if (cp >= 1250 && cp <= 1258)
            {
                return std::format("windows-{}", cp);
            }
            return std::format("CP{}", cp);
        }
#else
        return {};
#endif
    }

    /**
     * @brief stdin 是管道或常规文件重定向而非交互式控制台时为 true。
     *
     * ccg changes 在该情形下把 stdin 读作变更路径列表；交互式调用绝不能
     * 挂在 stdin 上等待。
     *
     * @internal
     * 控制台的判定用 GetConsoleMode 而非 _isatty：Windows 上 NUL 设备
     * （以及一切字符设备）也会让 _isatty 返回非零，否则 `ccg changes < NUL`
     * 会被误判成交互式。int 转指针正是 _get_osfhandle 的 CRT 惯用法；悲观
     * 告警不适用于每进程只跑一次的句柄探测。
     */
    [[nodiscard]]
    inline auto stdin_is_piped() -> bool
    {
#if defined(_WIN32)
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        const auto console = reinterpret_cast<HANDLE>(_get_osfhandle(0));
        if (console == INVALID_HANDLE_VALUE)
        {
            return true; // 连 stdin 句柄都没有：必非控制台
        }
        DWORD mode = 0;
        return GetConsoleMode(console, &mode) == 0;
#else
        return isatty(STDIN_FILENO) == 0;
#endif
    }

    /**
     * @brief 调用进程的进程 id。
     *
     * 用作 `ccg update`
     * 认领持有者令牌的人类可读一半（租约协议：令牌只指名持有者）：
     * 只指名持有认领的是谁，从不是存活检查（租约时间戳才是权威——已死
     * 持有者的认领会过期并被窃取）。
     */
    [[nodiscard]]
    inline auto current_pid() -> std::int64_t
    {
#if defined(_WIN32)
        return static_cast<std::int64_t>(GetCurrentProcessId());
#else
        return static_cast<std::int64_t>(::getpid());
#endif
    }

    /**
     * @brief 调用进程当前工作集大小（MB），取不到为 -1。
     *
     * 波尾软裁剪的打点面：Windows 经 GetProcessMemoryInfo 读
     * WorkingSetSize；POSIX 分支恒回 -1（该平台无裁剪协同，打印侧
     * 据此隐藏打点字段）。
     */
    [[nodiscard]]
    inline auto working_set_mb() -> double
    {
#if defined(_WIN32)
        PROCESS_MEMORY_COUNTERS counters{};
        if (GetProcessMemoryInfo(
                GetCurrentProcess(),
                &counters,
                sizeof(counters))
            == 0)
        {
            return -1.0;
        }
        return static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0);
#else
        return -1.0;
#endif
    }

    /**
     * @brief 软裁剪：把调用进程工作集里能挪走的页全部还回操作系统。
     *
     * 只降物理驻留（挪走的页进系统备用名单，下次访问经软缺页微秒级
     * 取回），不释放任何分配——提交内存不变。必须在内存最低点调用
     * （波式拆解+释放刚完成时）：波峰调用会把活数据一并踢出，热页
     * 软缺页重填白付代价。尽力而为，失败静默（锁定页等残留属正常）。
     * POSIX 分支空操作。
     */
    inline void trim_working_set()
    {
#if defined(_WIN32)
        (void)::EmptyWorkingSet(::GetCurrentProcess());
#endif
    }

    /**
     * @brief 一次性用途的不碰撞混合数（时钟 ^ 单调计数 × 黄金比例 ^ pid）。
     *
     * 仓内 nonce 配方的唯一定义（收敛昔日临时文件名与 hook 挂起文件名
     * 各写一份的分叉）；消费方按用途截取低位（12 位十六进制等）。
     */
    [[nodiscard]]
    inline auto make_nonce() -> std::uint64_t
    {
        static std::atomic<std::uint64_t> counter{0};
        const std::uint64_t               sequence =
            counter.fetch_add(1, std::memory_order_relaxed);
        const auto ticks = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        return ticks ^ (sequence * 0x9e3779b97f4a7c15ULL)
               ^ (static_cast<std::uint64_t>(current_pid()) << 32);
    }

    namespace detail
    {

        /**
         * @brief 本模块静态初始化时记录的时刻——POSIX 上无 /proc 时的兜底基线。
         *
         * @internal
         * 模块静态初始化先于 main，但镜像加载与动态库初始化已经发生，所以它
         * 只是"进程已存活时长"的下界（会低估启动阶段），不是真实创建时刻。
         */
        inline const auto EARLIEST_STATIC_INIT =
            std::chrono::steady_clock::now();

        [[nodiscard]]
        inline auto earliest_static_elapsed() -> std::chrono::milliseconds
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - EARLIEST_STATIC_INIT);
        }

#if defined(_WIN32)
        /** @brief FILETIME 的 100 ns 刻度合并为 64 位值。 */
        [[nodiscard]]
        inline auto filetime_ticks(const FILETIME& value) -> std::uint64_t
        {
            ULARGE_INTEGER combined{};
            combined.LowPart = value.dwLowDateTime;
            combined.HighPart = value.dwHighDateTime;
            return combined.QuadPart;
        }

        /**
         * @brief GetProcessTimes 的创建时刻与当前系统时间的差额（毫秒）。
         *
         * @internal 系统时钟回拨时 current <= creation，返回 0——这本身不能
         * 当作"进程还很年轻"，调用方必须与单调时钟基准取上界。
         */
        [[nodiscard]]
        inline auto creation_wall_elapsed_ms() -> std::uint64_t
        {
            FILETIME created{};
            FILETIME exited{};
            FILETIME kernel{};
            FILETIME user{};
            if (GetProcessTimes(
                    GetCurrentProcess(),
                    &created,
                    &exited,
                    &kernel,
                    &user)
                == 0)
            {
                return 0;
            }
            FILETIME now{};
            GetSystemTimeAsFileTime(&now);
            const std::uint64_t creation = filetime_ticks(created);
            const std::uint64_t current = filetime_ticks(now);
            if (current <= creation)
            {
                return 0;
            }
            // FILETIME 的 1 ms = 10,000 个 100 ns 刻度。
            return (current - creation) / 10'000;
        }

        /**
         * @brief 静态初始化时的创建基准快照。
         *
         * @internal 墙钟差额在静态初始化时已经包含镜像加载与 DLL 初始化；
         * 此后再叠加单调时钟（QPC 与 GetTickCount64 取较大者）的增量，就是
         * 系统时钟被回拨时仍然可靠、只增不减的保守上界。
         */
        struct CreationBaseline
        {
            std::uint64_t wall_elapsed_ms = 0;
            std::uint64_t tick_ms = 0;
            std::uint64_t qpc_ticks = 0;
            std::uint64_t qpc_frequency = 0;
        };

        // 非 inline：仅本模块单元内消费的具名常量（const 命名空间作用域
        // 隐含内部链接）；MSVC 模块单元对 import-only inline 变量不发射
        // COMDAT（见 db.sqlpp.anchor.cpp 先例），去掉 inline 使符号在本地
        // obj 内解析，clang 侧行为不变。
        const auto CREATION_BASELINE = []
        {
            CreationBaseline baseline;
            baseline.wall_elapsed_ms = creation_wall_elapsed_ms();
            baseline.tick_ms = GetTickCount64();
            LARGE_INTEGER counter{};
            LARGE_INTEGER frequency{};
            if (QueryPerformanceCounter(&counter) != 0
                && QueryPerformanceFrequency(&frequency) != 0
                && frequency.QuadPart > 0)
            {
                baseline.qpc_ticks =
                    static_cast<std::uint64_t>(counter.QuadPart);
                baseline.qpc_frequency =
                    static_cast<std::uint64_t>(frequency.QuadPart);
            }
            return baseline;
        }();

        /** @brief QPC 自静态基准以来的毫秒增量；不可用时为 0。 */
        [[nodiscard]]
        inline auto qpc_elapsed_ms() -> std::uint64_t
        {
            if (CREATION_BASELINE.qpc_frequency == 0)
            {
                return 0;
            }
            LARGE_INTEGER counter{};
            if (QueryPerformanceCounter(&counter) == 0)
            {
                return 0;
            }
            const std::uint64_t current =
                static_cast<std::uint64_t>(counter.QuadPart);
            if (current < CREATION_BASELINE.qpc_ticks)
            {
                return 0;
            }
            return static_cast<std::uint64_t>(
                (static_cast<double>(current - CREATION_BASELINE.qpc_ticks)
                 * 1000.0)
                / static_cast<double>(CREATION_BASELINE.qpc_frequency));
        }
#endif

    } // namespace detail

    /**
     * @brief 本进程从**真实创建时刻**起已经过的时长。
     *
     * `ccg hook` 的绝对截止以此为基准：镜像加载、DLL/共享库初始化与
     * 杀软扫描都算在预算之内，而不是从模块静态初始化才开始计时。
     *
     * @internal
     * Windows：GetProcessTimes 返回的创建时间（FILETIME，UTC）与
     * GetSystemTimeAsFileTime 的当前时间相减；两者都是 100 ns 刻度，
     * 差额包含镜像加载与 DLL 初始化。该差额完全依赖墙钟，系统时钟被
     * 回拨时会读出 0 甚至负数——若直接采用会把已用预算重置。因此与
     * 静态初始化时的基准（当时的墙钟差额 + QPC/GetTickCount64 单调
     * 增量）取较大者：回拨时继续沿用保守上界（只增不减），正常时两者
     * 一致。GetProcessTimes 不可用时退回静态初始化基准（此时等于
     * detail::EARLIEST_STATIC_INIT 的单调增量，是下界）。
     *
     * Linux：/proc/self/stat 的 starttime（字段 22，自开机起的时钟滴答）
     * 与 /proc/uptime 的开机时长相减，得到真实已存活时长。comm 字段可能
     * 含空格与右括号，故从最后一个 ')' 之后开始数字段。
     *
     * 其它 POSIX（含 macOS）：没有可靠的免权限创建时刻接口，退回模块
     * 静态初始化——这是 best-effort，只保证是从本进程内可观察到的尽早
     * 时刻起算，仍可能低估镜像加载时间。调用方（hook 预算）据此留出
     * 更大的安全余量即可；平台差异已在各平台实现处如实标注。
     */
    [[nodiscard]]
    inline auto process_elapsed_since_creation() -> std::chrono::milliseconds
    {
#if defined(_WIN32)
        const std::uint64_t wall_elapsed = detail::creation_wall_elapsed_ms();
        // 单调增量：GetTickCount64 与 QPC 交叉校验，取较大者，避免任一
        // 来源的倒退/精度问题把基准拉小。
        std::uint64_t monotonic_elapsed = 0;
        if (const std::uint64_t ticks = GetTickCount64();
            ticks >= detail::CREATION_BASELINE.tick_ms)
        {
            monotonic_elapsed = ticks - detail::CREATION_BASELINE.tick_ms;
        }
        monotonic_elapsed =
            std::max(monotonic_elapsed, detail::qpc_elapsed_ms());
        const std::uint64_t conservative =
            detail::CREATION_BASELINE.wall_elapsed_ms + monotonic_elapsed;
        // 系统时钟回拨让 wall_elapsed 变小/归零时，保守上界继续计时；
        // 正常时钟下墙钟差额与它一致或更大（前跳方向也取较大者）。
        const std::uint64_t elapsed = std::max(wall_elapsed, conservative);
        return std::chrono::milliseconds{
            static_cast<std::chrono::milliseconds::rep>(elapsed)};
#elif defined(__linux__)
        std::ifstream stat_file{"/proc/self/stat"};
        std::string   stat_line;
        if (std::getline(stat_file, stat_line))
        {
            const std::size_t comm_end = stat_line.rfind(')');
            if (comm_end != std::string::npos)
            {
                std::istringstream fields{stat_line.substr(comm_end + 1)};
                std::string        token;
                bool               complete = true;
                for (int field = 3; field <= 22; ++field)
                {
                    if (!(fields >> token))
                    {
                        complete = false;
                        break;
                    }
                }
                if (complete)
                {
                    const long long start_ticks = std::atoll(token.c_str());
                    const long      hz = ::sysconf(_SC_CLK_TCK);
                    std::ifstream   uptime_file{"/proc/uptime"};
                    double          uptime_seconds = 0.0;
                    if (hz > 0 && (uptime_file >> uptime_seconds))
                    {
                        const double elapsed_seconds =
                            uptime_seconds
                            - static_cast<double>(start_ticks)
                                  / static_cast<double>(hz);
                        if (elapsed_seconds <= 0.0)
                        {
                            return std::chrono::milliseconds{0};
                        }
                        return std::chrono::duration_cast<
                            std::chrono::milliseconds>(
                            std::chrono::duration<double>{elapsed_seconds});
                    }
                }
            }
        }
        return detail::earliest_static_elapsed();
#else
        return detail::earliest_static_elapsed();
#endif
    }

    /**
     * @brief 给定进程 id 的进程当前是否仍存活。
     *
     * `ccg update` 的声明持有者记号内嵌 pid（`update_claim_token`）：租约
     * 存在只为覆盖活着的持有者——持有进程被宿主超时杀死后，其租约仍是
     * 未来时间戳，单看 `lease_until` 会把已死进程当作活持有者，队列因此
     * 停滞直到租约自然到期。本探针让下一个更新者立即接管孤儿声明。
     *
     * @internal
     * 只作否定判断：报 "死" 必须是确定的（Windows 打开进程成功且退出码
     * 非 STILL_ACTIVE；POSIX kill(pid, 0) 给出 ESRCH）。权限不足、查询失败
     * 一律按 "活" 处理——把权限墙误判成死亡会抢走真正持有者的声明，比多
     * 等一个租约糟得多。pid <= 0 是无效 id，报死（调用方据此忽略解析失败）
     * 但 holder 无 pid 的调用方根本不会走到这里。
     * Windows 分支：PROCESS_QUERY_LIMITED_INFORMATION 是 GetExitCodeProcess
     * 所需的最小权限；ERROR_ACCESS_DENIED 说明进程存在但被保护，按活处理。
     */
    [[nodiscard]]
    inline auto process_alive(std::int64_t pid) -> bool
    {
        if (pid <= 0)
        {
            return false;
        }
#if defined(_WIN32)
        const handle::Owned process = handle::Owned::adopt(OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE,
            static_cast<DWORD>(pid)));
        if (!process.valid())
        {
            return GetLastError() == ERROR_ACCESS_DENIED;
        }
        DWORD      code = 0;
        const bool queried = GetExitCodeProcess(process.get(), &code) != 0;
        return queried && code == STILL_ACTIVE;
#else
        if (pid > static_cast<std::int64_t>(std::numeric_limits<pid_t>::max()))
        {
            return false; // 超出 pid_t：必不存在（也避免负值变成进程组）
        }
        if (::kill(static_cast<pid_t>(pid), 0) == 0)
        {
            return true;
        }
        return errno == EPERM;
#endif
    }

    namespace detail
    {

#if defined(_WIN32)
        /**
         * @brief Win32 扩展长度路径（`\\?\` / `\\?\UNC\` 前缀）。
         *
         * @internal `\\?\` 关闭 Win32 路径规范化，因此必须先绝对化并把正斜杠
         * 换成反斜杠；它绕过 MAX_PATH（260），长 tmp 清理与长目标重命名都
         * 需要。仅用于 Win32 API（CreateFileW/MoveFileExW/DeleteFileW），
         * CRT 的文件流仍受自身限制。
         */
        [[nodiscard]]
        inline auto extended_win_path(const std::filesystem::path& p)
            -> std::wstring
        {
            std::error_code       ec;
            std::filesystem::path abs = std::filesystem::absolute(p, ec);
            if (ec)
            {
                abs = p;
            }
            std::wstring native = abs.wstring();
            for (wchar_t& c : native)
            {
                if (c == L'/')
                {
                    c = L'\\';
                }
            }
            if (native.starts_with(L"\\\\?\\"))
            {
                return native;
            }
            if (native.starts_with(L"\\\\"))
            {
                return std::format(L"\\\\?\\UNC\\{}", native.substr(2));
            }
            return std::format(L"\\\\?\\{}", native);
        }
#endif

    } // namespace detail

    /**
     * @brief 删除单个文件（失败静默）；Windows 上走扩展长度路径。
     *
     * @internal 供原子写/缓存的失败路径清理 tmp：普通 `std::filesystem::
     * remove` 在超过 MAX_PATH 的路径上会失败并留下垃圾；DeleteFileW +
     * `\\?\` 前缀不受该限制。
     */
    inline void remove_file(const std::filesystem::path& p)
    {
#if defined(_WIN32)
        if (DeleteFileW(detail::extended_win_path(p).c_str()) != 0)
        {
            return;
        }
#endif
        std::error_code ec;
        std::filesystem::remove(p, ec);
    }

    /**
     * @brief 文件元数据（修改时间计数值与字节数；ok=false 表示读不到
     *        或目标是目录）。
     *
     * @internal mtime_ticks 的纪元随平台实现（Windows 为 FILETIME 的
     * 100ns 计数），只做同平台缓存指纹自比较，不做跨平台换算。
     */
    struct FileMeta
    {
        bool         ok{};
        std::int64_t mtime_ticks{};
        std::int64_t size_bytes{};
    };

    /**
     * @brief 读文件元数据（失败 ok=false）；Windows 上走扩展长度路径。
     *
     * @internal 与写侧（remove_file 等）同规则：模块缓存目录叠加构建树
     * 后可能超 MAX_PATH，此时 MSVC STL 的属性查询虽吃 `\\?\` 前缀、
     * libc++ 侧却落 CRT stat 不认前缀——统一改 GetFileAttributesExW，
     * 双工具链对超长路径行为一致（pcm 写成功而尺寸读回失败的误报即出
     * 在此）。
     */
    [[nodiscard]]
    inline auto file_meta(const std::filesystem::path& p) -> FileMeta
    {
#if defined(_WIN32)
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (GetFileAttributesExW(
                detail::extended_win_path(p).c_str(),
                GetFileExInfoStandard,
                &data)
            == 0)
        {
            return {};
        }
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            return {};
        }
        ULARGE_INTEGER ticks{};
        ticks.LowPart = data.ftLastWriteTime.dwLowDateTime;
        ticks.HighPart = data.ftLastWriteTime.dwHighDateTime;
        return FileMeta{
            .ok = true,
            .mtime_ticks = static_cast<std::int64_t>(ticks.QuadPart),
            .size_bytes = (static_cast<std::int64_t>(data.nFileSizeHigh) << 32)
                          | static_cast<std::int64_t>(data.nFileSizeLow)};
#else
        std::error_code ec;
        const auto      mtime = std::filesystem::last_write_time(p, ec);
        if (ec)
        {
            return {};
        }
        const auto size = std::filesystem::file_size(p, ec);
        if (ec)
        {
            return {};
        }
        return FileMeta{
            .ok = true,
            .mtime_ticks =
                static_cast<std::int64_t>(mtime.time_since_epoch().count()),
            .size_bytes = static_cast<std::int64_t>(size)};
#endif
    }

    /**
     * @brief 与 final 同目录的唯一临时文件名 `<final>.<nonce>.tmp`。
     *
     * @internal 并发写者绝不共用固定 tmp 名（旧 PCH 清单的
     * `manifest.json.tmp` 会互踩）。nonce 取自 make_nonce（仓内唯一配方）；
     * 同目录保证随后的原子替换不跨卷。名字保持 12 位十六进制（约 17 字符），
     * 使常见深路径下的 tmp 仍低于 MAX_PATH——超长路径由 Win32 扩展长度前缀
     * 兜底，但不该让 tmp 主动变长。
     */
    [[nodiscard]]
    inline auto unique_temp_path(const std::filesystem::path& final_path)
        -> std::filesystem::path
    {
        std::filesystem::path tmp = final_path;
        // 12 位十六进制已含 pid（nonce 的 bit32-47）与进程内单调计数；
        // 刻意不用 16 位 + pid 文本，避免 tmp 自身把路径推过 MAX_PATH。
        tmp += std::format(".{:012x}.tmp", make_nonce() & 0xFFFFFFFFFFFFULL);
        return tmp;
    }

    /**
     * @brief 原子地把 tmp 替换到 final；失败时**保留旧 final**。
     *
     * 绝不先删除目标：Windows 上先删后换的窗口会让并发读者看到缺失。
     * Linux 用 rename(2)。Windows 首选 FileRenameInfoEx(22) +
     * REPLACE_IF_EXISTS|POSIX_SEMANTICS（目标被以 FILE_SHARE_DELETE 打开时
     * 仍可替换；旧 mmap 持有者继续读旧字节），失败回退
     * MoveFileExW(MOVEFILE_REPLACE_EXISTING)；两者都失败即返回 false，
     * 旧文件原样保留。
     *
     * @return true 表示 final 现在是 tmp 的字节；false 表示未做任何改变。
     */
    [[nodiscard]]
    inline auto atomic_replace(
        const std::filesystem::path& tmp,
        const std::filesystem::path& final_path) -> bool
    {
#if defined(_WIN32)
        // 本地布局与 FILE_RENAME_INFO 一致：Flags 联合、RootDirectory、
        // FileNameLength、尾随文件名。
        struct RenameInfo
        {
            DWORD  flags;
            HANDLE root_directory;
            DWORD  file_name_length;
            WCHAR  file_name[1];
        };
        constexpr DWORD replace_if_exists = 0x00000001;
        constexpr DWORD posix_semantics = 0x00000002;
        constexpr auto  file_rename_info_ex =
            static_cast<FILE_INFO_BY_HANDLE_CLASS>(22);

        // 扩展长度前缀：重命名目标常比 tmp 更长，且两者都可能越过
        // MAX_PATH；Win32 API 认 `\\?\`，CRT 不认。
        const std::wstring tmp_native = detail::extended_win_path(tmp);
        const std::wstring target_native =
            detail::extended_win_path(final_path);

        handle::Owned source = handle::Owned::adopt_file(CreateFileW(
            tmp_native.c_str(),
            DELETE | SYNCHRONIZE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr));
        if (source.valid())
        {
            // sizeof 已含 file_name[1] 与尾部填充；额外字符数为
            // target_native.size()（再加结尾 NUL 也在尾部填充内）。
            const std::size_t bytes =
                sizeof(RenameInfo) + target_native.size() * sizeof(WCHAR);
            std::vector<std::byte> storage(bytes);
            auto* info = reinterpret_cast<RenameInfo*>(storage.data());
            info->flags = replace_if_exists | posix_semantics;
            info->root_directory = nullptr;
            info->file_name_length =
                static_cast<DWORD>(target_native.size() * sizeof(WCHAR));
            std::copy(
                target_native.begin(),
                target_native.end(),
                info->file_name);
            info->file_name[target_native.size()] = L'\0';
            const BOOL renamed = SetFileInformationByHandle(
                source.get(),
                file_rename_info_ex,
                info,
                static_cast<DWORD>(bytes));
            if (renamed != 0)
            {
                return true;
            }
        }
        // 回退：先关掉 tmp 的句柄再改名（与首选路径的先后一致）。目标
        // 被打开且不允许删除时这里同样失败——返回 false 并保留旧文件，
        // 由调用方清理 tmp。
        source.reset();
        return MoveFileExW(
                   tmp_native.c_str(),
                   target_native.c_str(),
                   MOVEFILE_REPLACE_EXISTING)
               != 0;
#else
        return std::rename(tmp.c_str(), final_path.c_str()) == 0;
#endif
    }

    /**
     * @brief 完整字节序列化后经唯一 tmp 原子替换 final。
     *
     * 写 tmp 失败、或替换失败时删除 tmp 并返回 false；旧 final 保留。
     */
    [[nodiscard]]
    inline auto atomic_write_file(
        const std::filesystem::path& final_path,
        std::string_view             bytes) -> bool
    {
        const std::filesystem::path tmp = unique_temp_path(final_path);
#if defined(_WIN32)
        // 用 CreateFileW + 扩展长度路径写 tmp：std::ofstream 走 CRT，
        // 路径超过 MAX_PATH 时会在写入阶段就失败，原子替换根本没机会发生。
        handle::Owned file = handle::Owned::adopt_file(CreateFileW(
            detail::extended_win_path(tmp).c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr));
        if (!file.valid())
        {
            return false;
        }
        bool        ok = true;
        std::size_t offset = 0;
        while (offset < bytes.size())
        {
            const DWORD chunk = static_cast<DWORD>(
                std::min<std::size_t>(bytes.size() - offset, 1U << 20U));
            DWORD written = 0;
            if (WriteFile(
                    file.get(),
                    bytes.data() + offset,
                    chunk,
                    &written,
                    nullptr)
                    == 0
                || written != chunk)
            {
                ok = false;
                break;
            }
            offset += chunk;
        }
        // 必须先关句柄再善后：失败分支要删临时文件，而句柄是零共享打开
        // 的，开着的时候删不掉。
        file.reset();
        if (!ok)
        {
            remove_file(tmp);
            return false;
        }
#else
        {
            std::ofstream out{tmp, std::ios::binary | std::ios::trunc};
            if (!out)
            {
                return false;
            }
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            out.flush();
            if (!out)
            {
                out.close();
                remove_file(tmp);
                return false;
            }
        }
#endif
        if (!atomic_replace(tmp, final_path))
        {
            remove_file(tmp);
            return false;
        }
        return true;
    }

    /**
     * @brief 常规文件的只读内存映射（RAII，move-only）。
     *
     * 映射建立后经 bytes() 以只读字节视图随机访问；与写者的并发安全由
     * tmp+rename 原子替换协议背书（替换发生时旧映射继续读旧字节，见
     * atomic_replace 的注释），本类不做任何额外协调。
     *
     * @internal Windows：CreateFileW(FILE_SHARE_READ|WRITE|DELETE 共享打开，
     *           防止与并发写者的 tmp 写入互斥失败）+ CreateFileMappingW
     *           (PAGE_READONLY) + MapViewOfFile(FILE_MAP_READ)；POSIX：
     *           open(O_RDONLY) + mmap(PROT_READ, MAP_PRIVATE)。空文件不建
     *           映射（MapViewOfFile 对零长度失败），bytes() 恒可安全调用。
     *           文件句柄、映射句柄与视图都由拥有型对象持有：构造函数每
     *           取到一步就交给局部拥有者，全部建成才转移给成员，中途失
     *           败的对象不持有任何句柄，回滚由局部拥有者的析构完成；析
     *           构、移动构造与移动赋值因此都是默认的。
     */
    class ReadOnlyMap
    {
    public:
        ReadOnlyMap() = default;

        /** @brief 映射视图的拥有者；析构时解除映射。 */
        using View = handle::OwnedPtr<void, handle::ViewDeleter>;

        /** @brief 打开并映射整个文件；任何一步失败则对象保持空（valid 假）。 */
        explicit ReadOnlyMap(const std::filesystem::path& path)
        {
#if defined(_WIN32)
            handle::Owned file = handle::Owned::adopt_file(CreateFileW(
                detail::extended_win_path(path).c_str(),
                GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr));
            if (!file.valid())
            {
                return;
            }
            LARGE_INTEGER size{};
            if (GetFileSizeEx(file.get(), &size) == 0 || size.QuadPart <= 0)
            {
                return;
            }
            handle::Owned mapping = handle::Owned::adopt(CreateFileMappingW(
                file.get(),
                nullptr,
                PAGE_READONLY,
                0,
                0,
                nullptr));
            if (!mapping.valid())
            {
                return;
            }
            View view = View::adopt(MapViewOfFile(
                mapping.get(),
                FILE_MAP_READ,
                0,
                0,
                static_cast<std::size_t>(size.QuadPart)));
            if (!view)
            {
                return;
            }
            size_ = static_cast<std::size_t>(size.QuadPart);
            file_ = std::move(file);
            mapping_ = std::move(mapping);
            view_ = std::move(view);
#else
            handle::Owned file =
                handle::Owned::adopt(::open(path.c_str(), O_RDONLY));
            if (!file.valid())
            {
                return;
            }
            struct stat st{};
            if (::fstat(file.get(), &st) != 0 || st.st_size <= 0)
            {
                return;
            }
            const auto  size = static_cast<std::size_t>(st.st_size);
            void* const raw =
                ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, file.get(), 0);
            if (raw == MAP_FAILED)
            {
                return;
            }
            View view = View::adopt(raw, handle::ViewDeleter{.size = size});
            size_ = size;
            file_ = std::move(file);
            view_ = std::move(view);
#endif
        }

        ~ReadOnlyMap() = default;
        ReadOnlyMap(const ReadOnlyMap&) = delete;
        ReadOnlyMap& operator=(const ReadOnlyMap&) = delete;

        ReadOnlyMap(ReadOnlyMap&&) noexcept = default;
        ReadOnlyMap& operator=(ReadOnlyMap&&) noexcept = default;

        /** @brief 映射是否有效（空文件或打开失败时假）。 */
        [[nodiscard]]
        bool valid() const noexcept
        {
            return view_.valid();
        }

        /** @brief 映射字节（valid 假时空视图）。 */
        [[nodiscard]]
        auto bytes() const noexcept -> std::span<const std::byte>
        {
            return {static_cast<const std::byte*>(view_.get()), size_};
        }

    private:
        /** @brief 成员按文件、映射、视图的次序声明：析构次序相反，解除
         *          映射先于关映射、关文件。 */
        handle::Owned file_;
#if defined(_WIN32)
        handle::Owned mapping_;
#endif
        View        view_;
        std::size_t size_ = 0;
    };

    /**
     * @brief 跨进程独占锁的 RAII 句柄（move-only）。
     *
     * @internal
     * 锁文件的存在即被占用；持有者进程死亡时操作系统关闭句柄，锁文件
     * 成为可回收的孤儿（由 `try_acquire_file_lock` 的 pid 探活识别）。
     * 析构关闭句柄后删除锁文件——只在确实持有时删，故不会误删新持有者
     * 的锁（新持有者只有在旧文件被 unlink 后才可能 CREATE_NEW 成功）。
     * 是否持有只看句柄成员：路径与句柄在创建函数末尾一前一后就位，任
     * 一步抛出都由局部拥有者关掉句柄，所以「创建成功即持有」成立，不
     * 另设持有标记。路径成员存的是已转换好的原生路径，释放函数因此
     * 不分配内存。
     */
    class FileLock
    {
    public:
        FileLock() = default;
        ~FileLock() { release(); }
        FileLock(const FileLock&) = delete;
        FileLock& operator=(const FileLock&) = delete;
        FileLock(FileLock&& other) noexcept
            : path_(std::move(other.path_)), handle_(std::move(other.handle_))
        {
            other.path_.clear();
        }
        FileLock& operator=(FileLock&& other) noexcept
        {
            if (this != &other)
            {
                release();
                path_ = std::move(other.path_);
                handle_ = std::move(other.handle_);
                other.path_.clear();
            }
            return *this;
        }

        /** @brief 是否持有锁（创建了锁文件且句柄仍开着）。 */
        [[nodiscard]]
        auto held() const noexcept -> bool
        {
            return handle_.valid();
        }

        /** @brief 释放锁：先关句柄，再删锁文件；不抛、不分配。 */
        void release() noexcept
        {
            // 先关句柄再删文件：Windows 上句柄以零共享模式打开，开着时
            // 删不掉。
            handle_.reset();
            if (!path_.empty())
            {
#if defined(_WIN32)
                (void)DeleteFileW(path_.c_str());
#else
                (void)::unlink(path_.c_str());
#endif
                path_.clear();
            }
        }

    private:
        friend auto try_acquire_file_lock(
            const std::filesystem::path&,
            std::string_view,
            std::int64_t) -> FileLock;

        /// 原子创建锁文件并写入持有者三行内容（O_EXCL / CREATE_NEW：
        /// 并发争抢恰有一个赢家）；已存在时返回 false 且本对象不变。
        bool create_exclusive(
            const std::filesystem::path& lock_path,
            const std::string&           content)
        {
#if defined(_WIN32)
            std::wstring  native = detail::extended_win_path(lock_path);
            handle::Owned handle = handle::Owned::adopt_file(CreateFileW(
                native.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL,
                nullptr));
#else
            std::string   native = lock_path.native();
            handle::Owned handle = handle::Owned::adopt(
                ::open(native.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600));
#endif
            if (!handle.valid())
            {
                return false;
            }
#if defined(_WIN32)
            DWORD written = 0;
            (void)WriteFile(
                handle.get(),
                content.data(),
                static_cast<DWORD>(content.size()),
                &written,
                nullptr);
#else
            (void)::write(handle.get(), content.data(), content.size());
#endif
            // 先记路径（可能分配内存，此时句柄仍归局部拥有者，抛出就随
            // 它关闭），最后把句柄转移给成员：两个成员要么同时就位，
            // 要么都不动。
            path_ = std::move(native);
            handle_ = std::move(handle);
            return true;
        }

#if defined(_WIN32)
        /** @brief 锁文件的原生路径：已转换的扩展长度路径，删除时直接
         *          用，不再分配。 */
        using NativePath = std::wstring;
#else
        /** @brief 锁文件的原生路径。 */
        using NativePath = std::string;
#endif
        NativePath    path_;
        handle::Owned handle_;
    };

    /**
     * @brief 非阻塞地获取 `lock_path` 上的跨进程独占锁。
     *
     * @internal
     * 写者永不取这把锁（它只保护"一次触发只发一条合并结论"的消费者
     * 互斥），所以失败路径必须即时返回空句柄。创建是原子的 O_EXCL /
     * CREATE_NEW：并发争抢恰有一个赢家。
     *
     * 孤儿回收：读到的 pid 仍存活则不回收；pid 已死、pid 缺失但时间戳
     * 超龄，或 pid 与时间戳都无法解析且锁文件 mtime 比 `stale_ms` 更老，
     * 才 unlink 后重试一次——两个回收者同时 unlink 时仍只有一个能
     * CREATE_NEW 成功。Windows 上另一个进程以 `dwShareMode = 0` 持有时，
     * 读取本身失败 = 被占，同样不回收；反之读取成功且内容为空则证明创建者
     * 已死（崩溃落在 CREATE_NEW 与首次 WriteFile 之间），必须立即回收。
     */
    [[nodiscard]]
    inline auto try_acquire_file_lock(
        const std::filesystem::path& lock_path,
        std::string_view             token,
        std::int64_t                 stale_ms) -> FileLock
    {
        const std::string content = std::format(
            "{}\n{}\n{}",
            current_pid(),
            cg::util::cstd::epoch_ms(),
            token);

        FileLock lock;
        if (lock.create_exclusive(lock_path, content))
        {
            return lock;
        }

        // 已存在：读 pid/时间戳，判定是否可回收。
        std::int64_t pid = 0;
        std::int64_t stamp = 0;
        bool         readable = false;
        {
#if defined(_WIN32)
            std::ifstream in{
                std::filesystem::path{detail::extended_win_path(lock_path)},
                std::ios::binary};
#else
            std::ifstream in{lock_path, std::ios::binary};
#endif
            if (in)
            {
                std::string line;
                if (std::getline(in, line))
                {
                    try
                    {
                        pid = std::stoll(line);
                    }
                    catch (...)
                    {
                        pid = 0;
                    }
                }
                if (std::getline(in, line))
                {
                    try
                    {
                        stamp = std::stoll(line);
                    }
                    catch (...)
                    {
                        stamp = 0;
                    }
                }
                readable = true;
            }
        }
        if (!readable)
        {
            return lock; // 共享冲突/不可读：按被占处理，绝不误抢
        }
        bool reclaim = false;
        if (pid > 0)
        {
            reclaim = !process_alive(pid);
        }
        else if (stamp > 0)
        {
            reclaim = cg::util::cstd::epoch_ms() - stamp > stale_ms;
        }
        else
        {
            // pid 与时间戳都无法解析（空文件或半写）：没有任何内部时间
            // 线索，只能用文件本身的时间兜底。崩溃可能恰好落在 CREATE_NEW
            // 与首次 WriteFile 之间，这道兜底决定锁会不会永久卡住。
#if defined(_WIN32)
            // 创建者以 dwShareMode = 0 打开且写完才关闭；本次读取既然成功，
            // 创建者已不在（活着的持有者会让上面的 ifstream 打开失败并按
            // “被占”提前返回），空锁就是崩溃残留，立即回收。
            reclaim = true;
#else
            // POSIX 的 create 与 write 之间没有共享屏障，另开进程可能读到
            // 尚未落笔的空窗口；仍按 mtime + stale_ms 保守判定。
            std::error_code mtime_ec;
            const auto      mtime =
                std::filesystem::last_write_time(lock_path, mtime_ec);
            if (!mtime_ec)
            {
                const auto age =
                    std::filesystem::file_time_type::clock::now() - mtime;
                reclaim = age > std::chrono::milliseconds{stale_ms};
            }
#endif
        }
        if (!reclaim)
        {
            return lock;
        }
#if defined(_WIN32)
        (void)DeleteFileW(detail::extended_win_path(lock_path).c_str());
#else
        (void)::unlink(lock_path.native().c_str());
#endif
        (void)lock.create_exclusive(lock_path, content);
        return lock;
    }

    /**
     * @brief 只读探测：lock_path 上的文件锁是否被一个存活进程持有。
     *
     * 与 try_acquire_file_lock 同一套判活规则，但绝不创建锁文件——供"想
     * 知道别人是否在跑、自己不参赛"的调用方使用（例如采集联动方在派生
     * 采集进程前探测互斥，而真正的锁由采集执行方获取）。
     *
     * @return 锁不存在时 false；pid 仍存活时 true；pid 已死或锁内容无法
     *         解析且按时间戳/文件年龄超龄时，顺手回收（删除后返回 false）；
     *         Windows 下读取因共享冲突失败（活持有者以零共享模式打开）时
     *         按被占处理返回 true。
     */
    [[nodiscard]]
    inline auto file_lock_held_by_live_process(
        const std::filesystem::path& lock_path,
        std::int64_t                 stale_ms) -> bool
    {
        std::error_code ec;
        if (!std::filesystem::exists(lock_path, ec) || ec)
        {
            return false;
        }
        std::int64_t pid = 0;
        std::int64_t stamp = 0;
        bool         readable = false;
        {
#if defined(_WIN32)
            std::ifstream in{
                std::filesystem::path{detail::extended_win_path(lock_path)},
                std::ios::binary};
#else
            std::ifstream in{lock_path, std::ios::binary};
#endif
            if (in)
            {
                std::string line;
                if (std::getline(in, line))
                {
                    try
                    {
                        pid = std::stoll(line);
                    }
                    catch (...)
                    {
                        pid = 0;
                    }
                }
                if (std::getline(in, line))
                {
                    try
                    {
                        stamp = std::stoll(line);
                    }
                    catch (...)
                    {
                        stamp = 0;
                    }
                }
                readable = true;
            }
        }
        if (!readable)
        {
            return true; // 共享冲突/不可读：按被占处理，绝不误抢
        }
        bool reclaim = false;
        if (pid > 0)
        {
            reclaim = !process_alive(pid);
        }
        else if (stamp > 0)
        {
            reclaim = cg::util::cstd::epoch_ms() - stamp > stale_ms;
        }
        else
        {
#if defined(_WIN32)
            // 与 try_acquire_file_lock 同理：零共享模式下读取成功即证明
            // 创建者已死，空锁是崩溃残留，立即回收。
            reclaim = true;
#else
            std::error_code mtime_ec;
            const auto      mtime =
                std::filesystem::last_write_time(lock_path, mtime_ec);
            if (!mtime_ec)
            {
                const auto age =
                    std::filesystem::file_time_type::clock::now() - mtime;
                reclaim = age > std::chrono::milliseconds{stale_ms};
            }
#endif
        }
        if (!reclaim)
        {
            return true;
        }
#if defined(_WIN32)
        (void)DeleteFileW(detail::extended_win_path(lock_path).c_str());
#else
        (void)::unlink(lock_path.native().c_str());
#endif
        return false;
    }

    /**
     * @brief 对已存在的目录尽力开启 NTFS 压缩（调用点携带已规范化的
     *        路径文本）。
     *
     * 动机：BMI 缓存目录每份配置持有 30-40 MB 的 pcm 且压缩收益可观。
     * 每种失败都是静默无操作：压缩从不是正确性依赖，非 Windows 平台则
     * 什么都不做。
     *
     * @internal
     * Windows 分支：目录被打上 FSCTL_SET_COMPRESSION 标志前，卷必须先经
     * GetVolumeInformationW（FILE_FILE_COMPRESSION）声明支持压缩；此后
     * 创建的文件才会继承该标志。任意绝对路径的卷根就是其头三个字符
     * （"`<盘符>`:\"）；相对拼法无法探测。目录句柄需要
     * FILE_FLAG_BACKUP_SEMANTICS。NTFS 上实测（本机验证）：更窄的
     * 访问掩码（FILE_WRITE_DATA、+FILE_WRITE_ATTRIBUTES、+WRITE_DAC）对
     * 目录上的 FSCTL_SET_COMPRESSION 一律被拒以 ERROR_ACCESS_DENIED，而
     * 完整目录访问集成功——目录是 ccg 自己新建的缓存，请求完整访问没有
     * 代价。
     */
    inline void enable_dir_compression(const std::string& dir)
    {
#if defined(_WIN32)
        const std::wstring wide = std::filesystem::path{dir}.wstring();
        if (wide.size() < 3)
        {
            return;
        }
        DWORD              flags = 0;
        const std::wstring root = wide.substr(0, 3);
        if (!GetVolumeInformationW(
                root.c_str(),
                nullptr,
                0,
                nullptr,
                nullptr,
                &flags,
                nullptr,
                0))
        {
            return;
        }
        if ((flags & FILE_FILE_COMPRESSION) == 0)
        {
            return; // 文件系统不支持压缩（exFAT、ReFS、FAT32 等）
        }
        const handle::Owned handle = handle::Owned::adopt_file(CreateFileW(
            wide.c_str(),
            FILE_ALL_ACCESS,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS,
            nullptr));
        if (!handle.valid())
        {
            return;
        }
        constexpr USHORT format = COMPRESSION_FORMAT_DEFAULT;
        DWORD            returned = 0;
        (void)DeviceIoControl(
            handle.get(),
            FSCTL_SET_COMPRESSION,
            const_cast<USHORT*>(&format),
            sizeof(format),
            nullptr,
            0,
            &returned,
            nullptr);
#else
        (void)dir;
#endif
    }

    /**
     * @brief 当前可执行文件的绝对路径。
     *
     * `ccg hook` 用它推导运行中二进制周边的发布布局（随其一同发布的
     * .ccg/ 目录），使 hook 注册绝不固化源码树路径。平台查询失败时返回
     * 空路径（调用方报告可操作的错误；不做回退猜测）。
     *
     * @internal
     * Windows 分支采用增长循环：首射 MAX_PATH 大小，GetModuleFileNameW
     * 报告截断时倍增重试。Linux 与暴露 /proc 的 BSD 走规范的 self 符号
     * 链接（/proc/self/exe）。
     */
    [[nodiscard]]
    inline auto executable_path() -> std::filesystem::path
    {
#if defined(_WIN32)
        std::wstring buffer(MAX_PATH, L'\0');
        for (;;)
        {
            const DWORD len = GetModuleFileNameW(
                nullptr,
                buffer.data(),
                static_cast<DWORD>(buffer.size()));
            if (len == 0)
            {
                return {};
            }
            if (len < buffer.size() - 1)
            {
                buffer.resize(len);
                return std::filesystem::path{buffer};
            }
            buffer.resize(buffer.size() * 2);
        }
#elif defined(__APPLE__)
        uint32_t size = 0;
        if (_NSGetExecutablePath(nullptr, &size) != 0 || size == 0)
        {
            return {};
        }
        std::string buffer(size, '\0');
        if (_NSGetExecutablePath(buffer.data(), &size) != 0)
        {
            return {};
        }
        return std::filesystem::path{buffer}.lexically_normal();
#else
        std::error_code             ec;
        const std::filesystem::path self =
            std::filesystem::read_symlink("/proc/self/exe", ec);
        if (!ec && !self.empty())
        {
            return self;
        }
        return {};
#endif
    }

    /**
     * @brief UTF-8 文本转 UTF-16 宽文本（OS API 边界的仓内唯一定义）。
     *
     * 消费面是 Windows 宽字符 API（进程派生、环境变量、路径）；空输入与
     * 转换失败都返回空串（调用方按空值处理，绝不部分转换）。
     *
     * @internal POSIX 分支是逐字节恒等拓宽——当前全部消费面都在 Windows
     * 分支内（POSIX 进程派生直接用 char*）；真实的 POSIX 转换需求出现时
     * 在此接 mbrtowc 家族，消费方代码不变。
     */
    [[nodiscard]]
    inline auto widen_utf8(std::string_view value) -> std::wstring
    {
#if defined(_WIN32)
        if (value.empty())
        {
            return {};
        }
        const int chars = MultiByteToWideChar(
            CP_UTF8,
            0,
            value.data(),
            static_cast<int>(value.size()),
            nullptr,
            0);
        if (chars <= 0)
        {
            return {};
        }
        std::wstring out(static_cast<std::size_t>(chars), L'\0');
        MultiByteToWideChar(
            CP_UTF8,
            0,
            value.data(),
            static_cast<int>(value.size()),
            out.data(),
            chars);
        return out;
#else
        return std::wstring{value.begin(), value.end()};
#endif
    }

    /**
     * @brief UTF-16 宽文本转 UTF-8 文本（OS API 边界的仓内唯一定义）。
     *
     * 空输入与转换失败都返回空串；与 widen_utf8 互为反向。
     *
     * @internal POSIX 分支逐字节收窄，理由同 widen_utf8。
     */
    [[nodiscard]]
    inline auto narrow_utf8(std::wstring_view value) -> std::string
    {
#if defined(_WIN32)
        if (value.empty())
        {
            return {};
        }
        const int bytes = WideCharToMultiByte(
            CP_UTF8,
            0,
            value.data(),
            static_cast<int>(value.size()),
            nullptr,
            0,
            nullptr,
            nullptr);
        if (bytes <= 0)
        {
            return {};
        }
        std::string out(static_cast<std::size_t>(bytes), '\0');
        WideCharToMultiByte(
            CP_UTF8,
            0,
            value.data(),
            static_cast<int>(value.size()),
            out.data(),
            bytes,
            nullptr,
            nullptr);
        return out;
#else
        return std::string{value.begin(), value.end()};
#endif
    }

    /**
     * @brief 按 MSVC CRT 规则把单个 argv 记号转回命令行拼写
     *        （cg::util::tokenize_command 的镜像，合方向）。
     *
     * 记号为空、或含空白（空格/制表符）、或含引号时整体加引号；2n 个
     * 反斜杠后跟引号产 n 个加转义引号；尾部反斜杠串翻倍（否则与收尾
     * 引号配对成转义引号，破坏记号边界）。仓内 CRT 引号合方向的唯一定义
     * （替换昔日驱动器 spawn、wrap 命令行、平台捕获执行三处的分叉），
     * 与进程派生同住本单元（拆方向的 tokenize_command 在 cg.util 核心）。
     */
    [[nodiscard]]
    inline auto quote_for_command_line(std::wstring_view token) -> std::wstring
    {
        if (!token.empty()
            && token.find_first_of(L" \t\"") == std::wstring_view::npos)
        {
            return std::wstring{token};
        }
        std::wstring out{L"\""};
        std::size_t  backslashes = 0;
        for (const wchar_t c : token)
        {
            if (c == L'\\')
            {
                ++backslashes;
                continue;
            }
            if (c == L'"')
            {
                out.append(backslashes * 2 + 1, L'\\');
                out.push_back(L'"');
                backslashes = 0;
                continue;
            }
            out.append(backslashes, L'\\');
            backslashes = 0;
            out.push_back(c);
        }
        out.append(backslashes * 2, L'\\');
        out.push_back(L'"');
        return out;
    }

    /**
     * @brief 本进程命令行的参数列表（宽字符），含首个参数，即程序自身
     *        的拼写。
     *
     * @internal
     * Windows 分支把命令行的宽字符原文按与入口函数相同的切分规则拆开；
     * 系统分配的参数数组由带删除器的拥有型指针持有，调用方只拿到列表，
     * 取不到时返回空列表。Linux 分支读 /proc/self/cmdline，macOS 分支
     * 读运行库保存的参数表，其余平台返回空列表；这三个分支都按字节
     * 拓宽（口径同 widen_utf8）。
     */
    [[nodiscard]]
    inline auto process_args_wide() -> std::vector<std::wstring>
    {
#if defined(_WIN32)
        int        argc = 0;
        const auto argv = handle::OwnedPtr<LPWSTR, handle::LocalDeleter>::adopt(
            ::CommandLineToArgvW(::GetCommandLineW(), &argc));
        if (!argv || argc <= 0)
        {
            return {};
        }
        std::vector<std::wstring> args;
        args.reserve(static_cast<std::size_t>(argc));
        for (int i = 0; i < argc; ++i)
        {
            args.emplace_back(argv.get()[i]);
        }
        return args;
#elif defined(__APPLE__)
        char**    argv = *_NSGetArgv();
        const int argc = *_NSGetArgc();

        std::vector<std::wstring> args;
        for (int i = 0; argv != nullptr && i < argc; ++i)
        {
            args.push_back(widen_utf8(argv[i]));
        }
        return args;
#elif defined(__linux__)
        std::ifstream stream{"/proc/self/cmdline", std::ios::binary};
        std::string   piece;

        std::vector<std::wstring> args;
        while (std::getline(stream, piece, '\0'))
        {
            args.push_back(widen_utf8(piece));
        }
        return args;
#else
        return {};
#endif
    }

    /**
     * @brief 当前进程环境块的条目列表，每条是"名字=值"形式的原文，含以
     *        等号开头的隐藏条目。
     *
     * @internal
     * Windows 分支取到的环境块由带删除器的拥有型指针持有，条目逐个拷进
     * 列表后随作用域交还；取不到时返回空列表。非 Windows 分支遍历运行
     * 库的环境块全局变量，按字节拓宽（口径同 widen_utf8）。
     */
    [[nodiscard]]
    inline auto environment_entries_wide() -> std::vector<std::wstring>
    {
#if defined(_WIN32)
        const auto block =
            handle::OwnedPtr<wchar_t, handle::EnvBlockDeleter>::adopt(
                ::GetEnvironmentStringsW());
        if (!block)
        {
            return {};
        }
        std::vector<std::wstring> entries;
        for (const wchar_t* entry = block.get(); *entry != L'\0';
             entry += std::wcslen(entry) + 1)
        {
            entries.emplace_back(entry);
        }
        return entries;
#else
        std::vector<std::wstring> entries;
        for (char** entry = ::environ; entry != nullptr && *entry != nullptr;
             ++entry)
        {
            entries.push_back(widen_utf8(*entry));
        }
        return entries;
#endif
    }

    namespace detail
    {

        /**
         * @brief 一对管道端：读端与写端，各自是拥有型句柄。
         *
         * @internal
         * 子进程派生专用的小件。建管道的接口经两个出参交回裸句柄，建管
         * 道的函数一拿到就把两端交给拥有者；调用方从头到尾只接触拥有
         * 型对象，派生之后必须提前关闭的一端用显式复位。只能移动。
         */
        struct PipePair
        {
            handle::Owned read;
            handle::Owned write;

            /** @brief 两端都建成了。 */
            [[nodiscard]]
            auto valid() const noexcept -> bool
            {
                return read.valid() && write.valid();
            }
        };

#if defined(_WIN32)

        /**
         * @brief 建一对两端都可继承的管道；有一端没建成，两端都为空。
         *
         * @internal 不抛、不分配堆内存：子进程派生的热路径专用。
         */
        [[nodiscard]]
        inline auto make_pipe(SECURITY_ATTRIBUTES& inheritable) noexcept
            -> PipePair
        {
            HANDLE   raw_read = nullptr;
            HANDLE   raw_write = nullptr;
            PipePair pair;
            if (::CreatePipe(&raw_read, &raw_write, &inheritable, 0) != 0)
            {
                pair.read = handle::Owned::adopt(raw_read);
                pair.write = handle::Owned::adopt(raw_write);
            }
            return pair;
        }

        /**
         * @brief 以可继承的方式打开空设备；失败返回空对象。
         *
         * @internal 给没有标准句柄的子进程顶上用的读端或写端，访问权
         * 由调用方给。不抛、不分配堆内存。
         */
        [[nodiscard]]
        inline auto open_inheritable_null_device(
            DWORD                access,
            SECURITY_ATTRIBUTES& inheritable) noexcept -> handle::Owned
        {
            return handle::Owned::adopt_file(
                ::CreateFileW(
                    L"NUL",
                    access,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    &inheritable,
                    OPEN_EXISTING,
                    0,
                    nullptr));
        }

#else

        /**
         * @brief 建一对管道；有一端没建成，两端都为空。
         *
         * @internal 不抛、不分配堆内存：子进程派生的热路径专用。
         */
        [[nodiscard]]
        inline auto make_pipe() noexcept -> PipePair
        {
            int      raw[2] = {-1, -1};
            PipePair pair;
            if (::pipe(raw) == 0)
            {
                pair.read = handle::Owned::adopt(raw[0]);
                pair.write = handle::Owned::adopt(raw[1]);
            }
            return pair;
        }

#endif

    } // namespace detail

    /// capture_exec 的产出：out 为捕获到的 stdout 字节，exit_code 为
    /// 子进程状态（平台无法表达时为 -1）。
    struct CaptureExecOut
    {
        std::string out;
        int         exit_code = -1;
    };

    /**
     * @brief 以直接子进程运行 exe（args 为 UTF-8），只捕获 STDOUT。
     *
     * 中间没有 shell，也就没有引号方言要斗。stderr 留在父进程的 stderr
     * 上：ccg 的机器模式在 stdout 上恰打印一行 JSON，调用方消费的仅此
     * 而已。
     *
     * @return 进程无法孵化时返回 nullopt；否则持捕获到的字节与子进程
     *         状态（平台无法表达时 exit_code 为 -1）。
     *
     * @internal
     * 参数不得内嵌引号或 NUL（调用方早在到达这里之前就会拒绝）。Windows
     * 分支逐参数做 UTF-8 → UTF-16 转换（hook 载荷以 UTF-8 JSON 到达）。
     * 两个分支里管道端一取到就交给拥有型对象：派生之后我方的写端显式
     * 复位（否则读端永远等不到文件尾），其余各端随作用域关闭。
     */
    [[nodiscard]]
    inline auto capture_exec(
        const std::filesystem::path&    exe,
        const std::vector<std::string>& args) -> std::optional<CaptureExecOut>
    {
#if defined(_WIN32)
        std::wstring cmdline = quote_for_command_line(exe.wstring());
        for (const std::string& arg : args)
        {
            cmdline += L" ";
            cmdline += quote_for_command_line(widen_utf8(arg));
        }
        SECURITY_ATTRIBUTES inheritable{
            sizeof(SECURITY_ATTRIBUTES),
            nullptr,
            TRUE};
        detail::PipePair pipe = detail::make_pipe(inheritable);
        if (!pipe.valid())
        {
            return std::nullopt;
        }
        CaptureExecOut captured;
        STARTUPINFOW   si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = pipe.write.get();
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        // 空标准句柄兜底：宿主无控制台（计划任务/服务式拉起）时
        // GetStdHandle 返回空句柄，STARTF_USESTDHANDLES 把它直传给子进程
        // 会让对方 CRT 启动即崩（微软文档登记形态）。NUL 设备读写各开一份
        // 可继承句柄顶上（同文件 capture_exec_timed 的先例）。
        handle::Owned null_input;
        handle::Owned null_error;
        if (si.hStdInput == nullptr || si.hStdInput == INVALID_HANDLE_VALUE)
        {
            null_input =
                detail::open_inheritable_null_device(GENERIC_READ, inheritable);
            if (null_input.valid())
            {
                si.hStdInput = null_input.get(); // 兜底失败时维持原句柄
            }
        }
        if (si.hStdError == nullptr || si.hStdError == INVALID_HANDLE_VALUE)
        {
            null_error = detail::open_inheritable_null_device(
                GENERIC_WRITE,
                inheritable);
            if (null_error.valid())
            {
                si.hStdError = null_error.get();
            }
        }
        PROCESS_INFORMATION pi{};
        const BOOL          spawned = CreateProcessW(
            nullptr,
            cmdline.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &si,
            &pi);
        // 派生之后立刻关我方的写端：留着它，读端永远等不到文件尾。
        pipe.write.reset();
        if (!spawned)
        {
            return std::nullopt;
        }
        // 子进程与它的主线程句柄立即接管，随本函数返回关闭。
        const handle::Owned process = handle::Owned::adopt(pi.hProcess);
        const handle::Owned main_thread = handle::Owned::adopt(pi.hThread);
        char                buffer[4096];
        DWORD               got = 0;
        while (ReadFile(pipe.read.get(), buffer, sizeof(buffer), &got, nullptr)
               && got != 0)
        {
            captured.out.append(buffer, got);
        }
        WaitForSingleObject(process.get(), INFINITE);
        DWORD code = 0;
        if (GetExitCodeProcess(process.get(), &code))
        {
            captured.exit_code = static_cast<int>(code);
        }
        return captured;
#else
        CaptureExecOut   captured;
        detail::PipePair pipe = detail::make_pipe();
        if (!pipe.valid())
        {
            return std::nullopt;
        }
        const std::string        exe_native = exe.native();
        std::vector<std::string> storage;
        storage.reserve(args.size() + 1);
        storage.push_back(exe_native);
        for (const std::string& arg : args)
        {
            storage.push_back(arg);
        }
        const pid_t pid = ::fork();
        if (pid == 0)
        {
            // 子进程：我方的读端不要；写端挪到标准输出之后关闭，随即
            // 执行新程序。
            pipe.read.reset();
            ::dup2(pipe.write.get(), STDOUT_FILENO);
            pipe.write.reset();
            std::vector<char*> argv;
            argv.reserve(storage.size() + 1);
            for (const std::string& arg : storage)
            {
                argv.push_back(const_cast<char*>(arg.c_str()));
            }
            argv.push_back(nullptr);
            ::execv(exe_native.c_str(), argv.data());
            ::_exit(127); // exec 失败：子进程内没有可为之事
        }
        if (pid < 0)
        {
            return std::nullopt;
        }
        // 父进程只留读端：留着写端，读端永远等不到文件尾。
        pipe.write.reset();
        char    buffer[4096];
        ssize_t got = 0;
        while ((got = ::read(pipe.read.get(), buffer, sizeof(buffer))) > 0)
        {
            captured.out.append(buffer, static_cast<std::size_t>(got));
        }
        int status = 0;
        if (::waitpid(pid, &status, 0) == pid && WIFEXITED(status))
        {
            captured.exit_code = WEXITSTATUS(status);
        }
        return captured;
#endif
    }

    /**
     * @brief 带超时的子进程执行结果。
     *
     * @internal
     * started 表示子进程已孵化；timed_out 表示等待到点仍未完成——此时
     * 子进程**没有被杀死**，仍在写它自己的临时输出文件（写端句柄随子
     * 进程存活）。超时分支返回的 out/err 为空：调用方无从得知已写出的
     * 字节，也不应等待。
     */
    struct TimedExecResult
    {
        bool        started = false;
        bool        timed_out = false;
        int         exit_code = -1;
        std::string out;
        std::string err;
        // 子进程的继承句柄是否已被收窄到恰好的重定向文件（Windows：
        // PROC_THREAD_ATTRIBUTE_HANDLE_LIST 应用成功；POSIX：exec 前关闭了
        // 3 号以上全部 fd）。false 表示走了旧的全继承回退——诊断与测试
        // 据此识别宿主管道泄漏风险，而不是把失败静默成"看起来正常"。
        bool restricted_handles = false;
    };

    /**
     * @brief 以直接子进程运行 exe（args 为 UTF-8），stdout/stderr 重定向
     *        到临时文件，最多等待 timeout。
     *
     * 与 capture_exec 的区别是"可放弃等待"：到点关闭本进程侧的全部句柄
     * 但不杀子进程，子进程继续在后台跑完（它持有的写端句柄让临时文件
     * 存活到它退出）。临时文件一律 delete-on-close / 先 unlink 后用：
     * 超时后父进程退出也不会在盘上留下无主文件。
     *
     * @return started = 无法孵化时为 false（此时 timed_out 也为 false）；
     *         正常退出时 exit_code 有效、out/err 为完整捕获；
     *         timed_out = 等待到点（exit_code 为 -1）。
     *
     * @internal
     * Windows 分支：句柄以 bInheritHandle=TRUE 创建，子进程获得自己的
     * 文件对象引用；父进程在超时时关闭自己的副本，DELETE_ON_CLOSE 的
     * 删除要等最后一个句柄（子进程的）关闭。bInheritHandles=TRUE 是子
     * 进程拿到这三个句柄所必需，但它同时会把本进程所有可继承句柄（最
     * 要命的是宿主的 stdout/stderr 管道）一并交给后台子进程：hook 进程
     * 退出后宿主读管道要等后台 update 退出才见到 EOF，一次编辑被拖十几
     * 秒。因此用 STARTUPINFOEXW + PROC_THREAD_ATTRIBUTE_HANDLE_LIST 把
     * 可继承集收窄到 out/err/dev_null 三个句柄；属性列表创建失败时回退
     * 旧的全继承写法，并在 restricted_handles 里如实记录。CreateProcess
     * 首射 CREATE_BREAKAWAY_FROM_JOB | DETACHED_PROCESS——父进程若身处
     * 带 kill-on-close 的 job 对象，脱离 job 是子进程在父进程退出后继续
     * 存活的前提；job 不允许 breakaway 时回退到 CREATE_NO_WINDOW。
     * POSIX 分支：子进程在 exec 前 setsid()，脱离父进程的会话与控制
     * 终端，并关闭 3 号以上全部 fd（宿主的管道写端不得被后台子进程持
     * 有）；父进程用 waitpid(WNOHANG) 轮询，不 kill、超时后不再 wait。
     */
    namespace detail
    {

#if defined(_WIN32)

        /// 重定向用的临时文件（DELETE_ON_CLOSE、句柄可继承）；取到句柄
        /// 就交给拥有型对象返回，创建失败返回空对象。
        [[nodiscard]]
        auto make_temp_capture_file(SECURITY_ATTRIBUTES& inheritable)
            -> handle::Owned
        {
            std::error_code             ec;
            const std::filesystem::path dir =
                std::filesystem::temp_directory_path(ec);
            if (ec)
            {
                return {};
            }
            wchar_t name[MAX_PATH + 1] = {};
            if (GetTempFileNameW(dir.wstring().c_str(), L"ccg", 0, name) == 0)
            {
                return {};
            }
            handle::Owned handle = handle::Owned::adopt_file(CreateFileW(
                name,
                GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                &inheritable,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                nullptr));
            if (!handle.valid())
            {
                DeleteFileW(name);
            }
            return handle;
        }

        /// 回读临时文件全部内容（读前复位于文件头；定位失败返回空串）。
        [[nodiscard]]
        std::string read_back_capture_file(HANDLE handle)
        {
            SetLastError(
                ERROR_SUCCESS); // INVALID_SET_FILE_POINTER 也是合法偏移
            if (SetFilePointer(handle, 0, nullptr, FILE_BEGIN)
                    == INVALID_SET_FILE_POINTER
                && GetLastError() != ERROR_SUCCESS)
            {
                return {};
            }
            std::string out;
            char        buffer[4096];
            DWORD       got = 0;
            while (ReadFile(handle, buffer, sizeof(buffer), &got, nullptr)
                   && got != 0)
            {
                out.append(buffer, got);
            }
            return out;
        }

        /**
         * @brief 进程启动属性列表：在自带的定长缓冲上建列表，析构时让
         *        系统回收列表的内部资源。
         *
         * @internal
         * 子进程派生的热路径专用，不分配堆内存：缓冲是对象里的字节数组，
         * 系统为单个属性只要几十字节。缓冲不够或系统拒绝时对象为空，
         * 调用方据此回退到全继承的旧写法。句柄白名单的数组不归本类持有
         * （系统只保存指针），调用方要让数组与本对象都活到派生结束。本
         * 类不可移动：列表建在哪个对象的缓冲里，就一直在那里用到析构。
         * 关闭列表的调用只在 reset 一处，析构与构造失败回滚都经它。
         */
        class ProcAttrList
        {
        public:
            ProcAttrList(const ProcAttrList&) = delete;
            ProcAttrList& operator=(const ProcAttrList&) = delete;
            ProcAttrList(ProcAttrList&&) = delete;
            ProcAttrList& operator=(ProcAttrList&&) = delete;
            ~ProcAttrList() { reset(); }

            /**
             * @brief 建一个只含句柄白名单的属性列表；建不成时对象为空。
             *
             * @internal 不抛：只调系统接口，缓冲用自己的。
             */
            explicit ProcAttrList(std::span<HANDLE> handles) noexcept
            {
                auto* raw =
                    reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer_);
                SIZE_T bytes = sizeof(buffer_);
                if (::InitializeProcThreadAttributeList(raw, 1, 0, &bytes) == 0)
                {
                    return; // 缓冲不够：对象保持为空
                }
                list_ = raw;
                if (::UpdateProcThreadAttribute(
                        list_,
                        0,
                        PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                        handles.data(),
                        handles.size_bytes(),
                        nullptr,
                        nullptr)
                    == 0)
                {
                    reset(); // 白名单挂不上：回收列表，对象保持为空
                }
            }

            /** @brief 是否建成了可以交给派生调用的列表。 */
            [[nodiscard]]
            auto valid() const noexcept -> bool
            {
                return list_ != nullptr;
            }

            /** @brief 列表的原生指针，只供挂到启动结构上；空对象给空值。 */
            [[nodiscard]]
            auto get() const noexcept -> LPPROC_THREAD_ATTRIBUTE_LIST
            {
                return list_;
            }

        private:
            /** @brief 回收列表，对象变为空；本来就不持有则不做任何事。 */
            void reset() noexcept
            {
                if (list_ != nullptr)
                {
                    ::DeleteProcThreadAttributeList(list_);
                    list_ = nullptr;
                }
            }

            /** @brief 缓冲容量（字节）：远大于单个属性的实际需求。 */
            static constexpr std::size_t BUFFER_CAPACITY = 256;

            alignas(std::max_align_t) std::byte buffer_[BUFFER_CAPACITY] = {};
            LPPROC_THREAD_ATTRIBUTE_LIST list_ = nullptr;
        };

        /// capture_exec_timed 的 Windows 分支：句柄白名单与脱离 job 的
        /// 机制说明随 capture_exec_timed 的文档块。
        [[nodiscard]]
        auto capture_exec_timed_windows(
            std::wstring&             cmdline,
            std::chrono::milliseconds timeout) -> TimedExecResult
        {
            TimedExecResult result;

            SECURITY_ATTRIBUTES inheritable{
                sizeof(SECURITY_ATTRIBUTES),
                nullptr,
                TRUE};
            // 三个句柄取到即接管；哪一个没建成都直接返回，已建成的随
            // 析构关闭。
            const handle::Owned out_file = make_temp_capture_file(inheritable);
            const handle::Owned err_file = make_temp_capture_file(inheritable);
            handle::Owned       dev_null =
                open_inheritable_null_device(GENERIC_READ, inheritable);
            if (!out_file.valid() || !err_file.valid() || !dev_null.valid())
            {
                return result;
            }

            // 句柄白名单：bInheritHandles=TRUE 让子进程拿到重定向句柄，但没有
            // PROC_THREAD_ATTRIBUTE_HANDLE_LIST 时它同时继承宿主的 stdout/
            // stderr 管道（以及任何其它可继承句柄）。hook 退出后宿主读管道要
            // 等后台 update 结束才见 EOF。白名单把可继承集收窄到这三个文件。
            STARTUPINFOEXW si{};
            si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            si.StartupInfo.hStdOutput = out_file.get();
            si.StartupInfo.hStdError = err_file.get();
            si.StartupInfo.hStdInput = dev_null.get();
            // 系统要的是裸句柄值，而且只保存指针不拷贝：白名单数组必须活到
            // 派生结束，所以放在函数作用域而不是属性对象的构造处。
            HANDLE inherited_handles[] = {
                out_file.get(),
                err_file.get(),
                dev_null.get()};
            // 属性列表同理活到派生结束；建不成时对象为空，下面按全继承回退。
            const ProcAttrList attributes{inherited_handles};
            // 属性列表必须挂到 STARTUPINFOEXW::lpAttributeList 上，否则
            // CreateProcess 根本看不到白名单（只初始化不挂接 = 白名单静默失效，
            // 全继承语义照旧）。未列为扩展时保持 STARTUPINFOW 的旧 ABI（cb 与
            // 创建旗标必须一致）。
            si.lpAttributeList =
                attributes.valid() ? attributes.get() : nullptr;
            si.StartupInfo.cb = attributes.valid() ? sizeof(STARTUPINFOEXW)
                                                   : sizeof(STARTUPINFOW);
            const DWORD extended =
                attributes.valid() ? EXTENDED_STARTUPINFO_PRESENT : 0;
            PROCESS_INFORMATION pi{};
            const DWORD detached = CREATE_BREAKAWAY_FROM_JOB | DETACHED_PROCESS;
            BOOL        spawned = CreateProcessW(
                nullptr,
                cmdline.data(),
                nullptr,
                nullptr,
                TRUE,
                detached | extended,
                nullptr,
                nullptr,
                &si.StartupInfo,
                &pi);
            if (!spawned)
            {
                // job 不允许 breakaway，或平台拒绝 DETACHED_PROCESS 组合：退回
                // 无窗口但仍在 job 内的普通派生（父进程正常退出时子进程通常
                // 仍然存活；只有 kill-on-close 的 job 会连带杀死它）。
                spawned = CreateProcessW(
                    nullptr,
                    cmdline.data(),
                    nullptr,
                    nullptr,
                    TRUE,
                    CREATE_NO_WINDOW | extended,
                    nullptr,
                    nullptr,
                    &si.StartupInfo,
                    &pi);
            }
            result.restricted_handles = attributes.valid();
            dev_null.reset();
            if (!spawned)
            {
                return result;
            }
            // 子进程与它的主线程句柄立即接管，随本函数返回关闭。
            const handle::Owned process = handle::Owned::adopt(pi.hProcess);
            const handle::Owned main_thread = handle::Owned::adopt(pi.hThread);
            result.started = true;

            const DWORD wait_ms = static_cast<DWORD>(std::min<std::int64_t>(
                timeout.count(),
                static_cast<std::int64_t>(0xFFFFFFFFLL)));
            const DWORD waited = WaitForSingleObject(process.get(), wait_ms);
            if (waited == WAIT_OBJECT_0)
            {
                DWORD code = 0;
                if (GetExitCodeProcess(process.get(), &code))
                {
                    result.exit_code = static_cast<int>(code);
                }
                result.out = read_back_capture_file(out_file.get());
                result.err = read_back_capture_file(err_file.get());
            }
            else if (waited == WAIT_TIMEOUT)
            {
                result.timed_out = true;
            }
            // 超时分支刻意不读输出、不等待：本进程的两个读端一关，子进程的
            // 独立写端继续存活，临时文件在它退出时随最后一个句柄一起删除。
            return result;
        }

#else

        /// 重定向用的临时文件描述符（先 unlink 后用：进程退出
        /// 不留盘上文件）；取到描述符就交给拥有型对象返回，创建
        /// 失败返回空对象。
        [[nodiscard]]
        auto make_temp_capture_fd() -> handle::Owned
        {
            std::error_code       ec;
            std::filesystem::path dir =
                std::filesystem::temp_directory_path(ec);
            if (ec)
            {
                dir = "/tmp";
            }
            for (unsigned attempt = 0; attempt < 64; ++attempt)
            {
                const std::string name = std::format(
                    "{}/ccg-exec-{}-{}.tmp",
                    dir.string(),
                    static_cast<long long>(::getpid()),
                    attempt);
                handle::Owned fd = handle::Owned::adopt(
                    ::open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600));
                if (fd.valid())
                {
                    // 先 unlink：父进程超时/退出都不会留下可被他人看到的
                    // 文件；描述符仍可读写，空间随最后一个引用关闭而释放。
                    ::unlink(name.c_str());
                    return fd;
                }
                if (errno != EEXIST)
                {
                    break;
                }
            }
            return {};
        }

        /// 回读临时文件描述符的全部内容（读前复位于文件头；定位失败
        /// 返回空串）。
        [[nodiscard]]
        std::string read_back_capture_fd(int fd)
        {
            if (::lseek(fd, 0, SEEK_SET) < 0)
            {
                return {};
            }
            std::string out;
            char        buffer[4096];
            ssize_t     got = 0;
            while ((got = ::read(fd, buffer, sizeof(buffer))) > 0)
            {
                out.append(buffer, static_cast<std::size_t>(got));
            }
            return out;
        }

        /// capture_exec_timed 的 POSIX 分支：会话脱离与 fd 收窄的机制
        /// 说明随 capture_exec_timed 的文档块。
        [[nodiscard]]
        auto capture_exec_timed_posix(
            const std::filesystem::path&    exe,
            const std::vector<std::string>& args,
            std::chrono::milliseconds       timeout) -> TimedExecResult
        {
            TimedExecResult result;

            // 两个临时文件描述符取到即接管：哪一个没建成都直接返回，
            // 已建成的随析构关闭。
            handle::Owned out_fd = make_temp_capture_fd();
            handle::Owned err_fd = make_temp_capture_fd();
            if (!out_fd.valid() || !err_fd.valid())
            {
                return result;
            }

            // exec 失败哨兵：写端带 FD_CLOEXEC，exec 成功即在子进程里关闭，
            // 父进程读端看到 EOF；失败时子进程先把 errno 写进来。没有它，
            // fork 成功而 execv 失败会以 exit 127/空输出伪装成"已启动"。
            PipePair   exec_ctl = make_pipe();
            const bool have_exec_ctl = exec_ctl.valid();
            if (have_exec_ctl)
            {
                (void)::fcntl(exec_ctl.read.get(), F_SETFD, FD_CLOEXEC);
                (void)::fcntl(exec_ctl.write.get(), F_SETFD, FD_CLOEXEC);
            }

            const std::string        exe_native = exe.native();
            std::vector<std::string> storage;
            storage.reserve(args.size() + 1);
            storage.push_back(exe_native);
            for (const std::string& arg : args)
            {
                storage.push_back(arg);
            }
            const pid_t pid = ::fork();
            if (pid == 0)
            {
                if (have_exec_ctl)
                {
                    exec_ctl.read.reset();
                }
                // 新会话：脱离父进程的进程组与控制终端，父进程退出后继续运行。
                (void)::setsid();
                handle::Owned dev_null =
                    handle::Owned::adopt(::open("/dev/null", O_RDONLY));
                if (dev_null.valid())
                {
                    ::dup2(dev_null.get(), STDIN_FILENO);
                    if (dev_null.get() > STDERR_FILENO)
                    {
                        dev_null.reset();
                    }
                }
                ::dup2(out_fd.get(), STDOUT_FILENO);
                ::dup2(err_fd.get(), STDERR_FILENO);
                if (out_fd.get() > STDERR_FILENO)
                {
                    out_fd.reset();
                }
                if (err_fd.get() > STDERR_FILENO)
                {
                    err_fd.reset();
                }
                // 关闭 3 号以上全部 fd：detached 子进程不得继承宿主的管道/套接
                // 字写端，否则宿主读端要等后台 update 退出才见到 EOF。stdin/
                // stdout/stderr 已换成 /dev/null 与临时文件。Linux 用
                // /proc/self/fd 精确枚举；其它 POSIX 扫描 RLIMIT_NOFILE。
                // 继承来的描述符不是这里取用的资源，逐个交给拥有型对象再
                // 立即复位，是为了让关闭调用只出现在拥有者那里。
#if defined(__linux__)
                if (handle::OwnedPtr<DIR, handle::DirDeleter> fds =
                        handle::OwnedPtr<DIR, handle::DirDeleter>::adopt(
                            ::opendir("/proc/self/fd")))
                {
                    const int scan = ::dirfd(fds.get());
                    while (const dirent* entry = ::readdir(fds.get()))
                    {
                        const int fd = std::atoi(entry->d_name);
                        if (fd > STDERR_FILENO && fd != scan
                            && (!have_exec_ctl || fd != exec_ctl.write.get()))
                        {
                            handle::Owned::adopt(fd).reset();
                        }
                    }
                }
                else
#endif
                {
                    long limit = ::sysconf(_SC_OPEN_MAX);
                    if (limit < 0 || limit > 65536)
                    {
                        limit = 65536;
                    }
                    for (int fd = STDERR_FILENO + 1;
                         fd < static_cast<int>(limit);
                         ++fd)
                    {
                        if (have_exec_ctl && fd == exec_ctl.write.get())
                        {
                            continue; // 哨兵写端：exec 成功时由 FD_CLOEXEC 关闭
                        }
                        handle::Owned::adopt(fd).reset();
                    }
                }
                std::vector<char*> argv;
                argv.reserve(storage.size() + 1);
                for (const std::string& arg : storage)
                {
                    argv.push_back(const_cast<char*>(arg.c_str()));
                }
                argv.push_back(nullptr);
                ::execv(exe_native.c_str(), argv.data());
                if (have_exec_ctl)
                {
                    const int exec_errno = errno;
                    (void)::write(
                        exec_ctl.write.get(),
                        &exec_errno,
                        sizeof(exec_errno));
                }
                ::_exit(127);
            }
            if (have_exec_ctl)
            {
                // 父进程只留读端：自己攥着写端，读端就永远等不到文件尾。
                exec_ctl.write.reset();
            }
            if (pid < 0)
            {
                return result;
            }
            result.started = true;
            // exec 前的关闭循环保证子进程只拿到 0/1/2。
            result.restricted_handles = true;

            const auto start = std::chrono::steady_clock::now();
            int        status = 0;
            bool       reaped = false;
            for (;;)
            {
                const pid_t waited = ::waitpid(pid, &status, WNOHANG);
                if (waited == pid)
                {
                    reaped = true;
                    break;
                }
                if (waited < 0 && errno != EINTR)
                {
                    break; // 无法再观察（ECHILD 等）：按已分离处理
                }
                if (std::chrono::steady_clock::now() - start >= timeout)
                {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
            }
            // 哨兵判定：4 字节 errno = execv 失败（子进程从未变成目标程序）；
            // EOF（0 字节）或 EAGAIN（超时前还没轮到它 exec）都按已启动处理。
            bool exec_failed = false;
            if (have_exec_ctl)
            {
                int     exec_errno = 0;
                ssize_t got = -1;
                do
                {
                    got = ::read(
                        exec_ctl.read.get(),
                        &exec_errno,
                        sizeof(exec_errno));
                } while (got < 0 && errno == EINTR);
                exec_failed = got == static_cast<ssize_t>(sizeof(exec_errno));
            }
            if (reaped)
            {
                if (WIFEXITED(status))
                {
                    result.exit_code = WEXITSTATUS(status);
                }
                result.out = read_back_capture_fd(out_fd.get());
                result.err = read_back_capture_fd(err_fd.get());
            }
            else
            {
                result.timed_out = true; // 不 kill、不 wait：让它跑完
            }
            if (exec_failed
                || (!have_exec_ctl && reaped && result.exit_code == 127
                    && result.out.empty() && result.err.empty()))
            {
                // 没有可观察的输出、且退出码正是 execv 失败约定值：保守地
                // 报"未启动"，绝不向 hook 谎报"已入库"。
                result.started = false;
                result.timed_out = false;
                result.exit_code = -1;
                result.out.clear();
                result.err.clear();
            }
            return result;
        }

#endif

    } // namespace detail

    [[nodiscard]]
    inline auto capture_exec_timed(
        const std::filesystem::path&    exe,
        const std::vector<std::string>& args,
        std::chrono::milliseconds       timeout) -> TimedExecResult
    {
#if defined(_WIN32)
        std::wstring cmdline = quote_for_command_line(exe.wstring());
        for (const std::string& arg : args)
        {
            cmdline += L" ";
            cmdline += quote_for_command_line(widen_utf8(arg));
        }
        return detail::capture_exec_timed_windows(cmdline, timeout);
#else
        return detail::capture_exec_timed_posix(exe, args, timeout);
#endif
    }

#if defined(_WIN32)

    /**
     * @brief 子进程 stdin 的三种刻意形态（捕获原语的参数之一）。
     *
     * @internal
     * 三档分别对应合一前的三份骨架：NullHandle 是 vswhere/MSBuild 讯问形态
     * （子进程没有 stdin）；ParentHandle 是被包装 Rebuild 形态（构建子进程
     * 继续继承本进程的 stdin，与旧的无管道形态等价）；EofPipe 是驱动器讯问
     * 形态（专用管道在派生后立刻关闭双端，子进程的 stdin 即时见到 EOF——
     * "喂空 stdin"正是关闭 in_write 这个动作）。
     */
    enum class ChildStdin
    {
        NullHandle,
        ParentHandle,
        EofPipe,
    };

    /**
     * @brief 单条捕获流的排水钩子（均可空）。
     *
     * @internal
     * 被包装 Rebuild 的流式转发与记录模式滤噪由此挂接：on_chunk 在每个
     * 4 KiB 块累积进结果字符串之后、从排水线程里回调；on_finish 在该流
     * EOF 收尾时回调（流尾无换行残行的判形与 flush 放在这里）。两路流各
     * 持一份互不相交的钩子状态，回调只会在该流自己的排水线程上发生。
     */
    struct DrainTap
    {
        std::function<void(const char* data, DWORD size)> on_chunk;
        std::function<void()>                             on_finish;
    };

    /**
     * @brief 捕获原语的完整配置：四份骨架的全部刻意差异都参数化在此。
     *
     * @internal
     * cmdline 由调用方完成全部引号拼装传入（各调用面的引号方言不同：
     * 驱动器给 exe 裸引号、vswhere 拼预引号的 args_text、被包装 Rebuild
     * 逐参数 quote 后追加逐字 extra）。application 为 nullopt 时
     * lpApplicationName 传 nullptr——裸名（cmd.exe、PATH 上的 vswhere.exe）
     * 必须交给 CreateProcess 的标准 PATH 搜索；有值时禁用搜索按全路径
     * 派生。env_block 为空串时继承本进程环境；非空时必须是双 NUL 结尾的
     * CREATE_UNICODE_ENVIRONMENT 布局（调用方负责构造）。kill_after 有值
     * 表示带硬上限的等待，到点终止子进程（驱动器形态：任何运行都不得
     * 永久挂起索引）；无值表示无限等待（讯问/构建/转发三形态）。超时值
     * 非正数按 0 处理（即刻判超时），上限被钳到 0x7FFFFFFF 毫秒。
     */
    struct ChildRunSpec
    {
        std::wstring                cmdline;
        std::optional<std::wstring> application;
        std::wstring                cwd;
        std::wstring                env_block;
        ChildStdin                  stdin_mode = ChildStdin::NullHandle;
        bool                        capture_stdout = true;
        bool                        capture_stderr = true;
        /// wrap 透传形态：不建管道、不设 STARTF_USESTDHANDLES，子进程按
        /// 句柄继承直通本进程的 stdio（此刻 capture/stdin 字段被忽略）。
        bool                                     inherit_parent_stdio = false;
        bool                                     create_no_window = true;
        std::optional<std::chrono::milliseconds> kill_after;
        DrainTap                                 out_tap;
        DrainTap                                 err_tap;
    };

    /**
     * @brief 捕获原语的运行结果。
     *
     * @internal
     * pipes_ok=false 表示管道创建失败（此时 started 必为 false；被包装
     * Rebuild 的调用方据此抛"无法建管道"，与派生失败区分）。spawn_error
     * 是派生失败时的 GetLastError（在关闭任何句柄之前取好；wrap 形态的
     * 调用方把它编进错误行）。timed_out 仅在 kill_after 有值且到点时为
     * 真——终止路径的 exit_code 是 TerminateProcess 的约定码。
     */
    struct ChildRunResult
    {
        bool        pipes_ok = false;
        bool        started = false;
        bool        timed_out = false;
        int         exit_code = -1;
        DWORD       spawn_error = 0;
        std::string out;
        std::string err;
    };

    /**
     * @brief 配置化子进程捕获原语：派生 + 捕获（或透传）+ 等待（或到点
     *        终止）+ 收割，四份骨架合一后的唯一实现点。
     *
     * 行为清单（与合一前各骨架逐条对齐）：
     * - 我方读端与 in_write 不继承（子进程只拿到属于自己的管道端副本）；
     * - 派生后立刻关闭我方全部写端与 in_read（喂空 stdin / 令 EOF 可达）；
     * - 派生失败：GetLastError 先取，再关全部残余句柄，started=false；
     * - 两路捕获流并发排水（每路一条线程；先等 stdout 后排 stderr 的顺序
     *   排空在 stderr 洪水填满管道时死锁——审计教训，绝不能回到顺序形态）；
     * - 排水先于等待汇合：无限等待的形态在两路 EOF 后收割退出码，带
     *   kill_after 的形态先限时等待、到点 TerminateProcess 再补 5 秒等待，
     *   两种次序下排水线程都在收割退出码之前 join 完毕；
     * - 进程/线程句柄最终一律关闭（成功与失败路径同样）。
     *
     * @internal
     * 仅 Windows：合一前的四份骨架（驱动器讯问、vswhere 讯问、被包装
     * Rebuild、wrap 透传）全部是 Windows-only 调用面，本原语是对它们的
     * 收敛而非新平台功能；可移植的捕获族仍是 capture_exec /
     * capture_exec_timed（两分支皆实现，供 hook 路径跨平台使用）。
     * 管道端取到即交给拥有型对象，派生之后要提前关闭的四端用显式复位；
     * 读端按移动交给排水线程，线程结束时析构关闭。排水线程创建失败时
     * 已移进线程构造函数的读端随构造失败被销毁，先建成的那条排水线程
     * 随它的线程对象析构时汇合；异常照旧外抛，但不再带着没关闭的句柄。
     */
    [[nodiscard]]
    inline auto run_child(const ChildRunSpec& spec) -> ChildRunResult
    {
        ChildRunResult      result;
        SECURITY_ATTRIBUTES inheritable{
            sizeof(SECURITY_ATTRIBUTES),
            nullptr,
            TRUE};
        const bool use_std_handles = !spec.inherit_parent_stdio;
        // 管道取到即交给拥有者：哪一条没建成都直接返回，已建成的各端随
        // 本作用域的析构关闭。
        detail::PipePair in;
        detail::PipePair out;
        detail::PipePair err;
        bool             pipes_ok = true;
        if (use_std_handles && spec.stdin_mode == ChildStdin::EofPipe)
        {
            in = detail::make_pipe(inheritable);
            pipes_ok = in.valid();
        }
        if (pipes_ok && use_std_handles && spec.capture_stdout)
        {
            out = detail::make_pipe(inheritable);
            pipes_ok = out.valid();
        }
        if (pipes_ok && use_std_handles && spec.capture_stderr)
        {
            err = detail::make_pipe(inheritable);
            pipes_ok = err.valid();
        }
        if (!pipes_ok)
        {
            return result;
        }
        result.pipes_ok = true;
        // 我方持有的读端不继承进子进程（排水端是本进程私有的）。
        if (in.write.valid())
        {
            SetHandleInformation(in.write.get(), HANDLE_FLAG_INHERIT, 0);
        }
        if (out.read.valid())
        {
            SetHandleInformation(out.read.get(), HANDLE_FLAG_INHERIT, 0);
        }
        if (err.read.valid())
        {
            SetHandleInformation(err.read.get(), HANDLE_FLAG_INHERIT, 0);
        }

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        if (use_std_handles)
        {
            startup.dwFlags = STARTF_USESTDHANDLES;
            switch (spec.stdin_mode)
            {
            case ChildStdin::ParentHandle:
                startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
                break;
            case ChildStdin::EofPipe:
                startup.hStdInput = in.read.get();
                break;
            case ChildStdin::NullHandle:
            default:
                startup.hStdInput = nullptr;
                break;
            }
            startup.hStdOutput = out.write.valid()
                                     ? out.write.get()
                                     : GetStdHandle(STD_OUTPUT_HANDLE);
            startup.hStdError = err.write.valid()
                                    ? err.write.get()
                                    : GetStdHandle(STD_ERROR_HANDLE);
        }
        DWORD flags = 0;
        if (spec.create_no_window)
        {
            flags |= CREATE_NO_WINDOW;
        }
        if (!spec.env_block.empty())
        {
            flags |= CREATE_UNICODE_ENVIRONMENT;
        }
        // CreateProcessW 可能写入命令行与环境块缓冲，本进程只持有 const
        // 配置，两处都拷出可写副本。
        std::wstring        cmdline = spec.cmdline;
        std::wstring        env_block = spec.env_block;
        PROCESS_INFORMATION process{};
        const BOOL          ok = CreateProcessW(
            spec.application ? spec.application->c_str() : nullptr,
            cmdline.data(),
            nullptr,
            nullptr,
            TRUE,
            flags,
            env_block.empty() ? nullptr : env_block.data(),
            spec.cwd.empty() ? nullptr : spec.cwd.c_str(),
            &startup,
            &process);
        const DWORD spawn_error = ok ? 0 : GetLastError();
        // 我方写端一律即刻关闭（子进程持有自己的副本）；EofPipe 的 in_read
        // 也关——关 in_write 正是令子进程 stdin 见 EOF 的动作。
        in.read.reset();
        in.write.reset();
        out.write.reset();
        err.write.reset();
        if (!ok)
        {
            result.spawn_error = spawn_error;
            return result;
        }
        result.started = true;
        // 子进程与它的主线程句柄立即接管，随本函数返回关闭。
        const handle::Owned child = handle::Owned::adopt(process.hProcess);
        const handle::Owned child_main_thread =
            handle::Owned::adopt(process.hThread);

        // 逐字排水循环（合一前四份骨架的共同核心）：阻塞读本流到 EOF、
        // 逐块累积、可选钩子、收尾关端。两路并发由各持一条线程保证。读端
        // 按值接收，归排水线程所有，线程结束时析构关闭。
        const auto drain =
            [](handle::Owned pipe, std::string& sink, const DrainTap& tap)
        {
            char  buffer[4096];
            DWORD got = 0;
            for (;;)
            {
                if (!ReadFile(pipe.get(), buffer, sizeof(buffer), &got, nullptr)
                    || got == 0)
                {
                    break;
                }
                sink.append(buffer, got);
                if (tap.on_chunk)
                {
                    tap.on_chunk(buffer, got);
                }
            }
            pipe.reset();
            if (tap.on_finish)
            {
                tap.on_finish();
            }
        };
        std::optional<std::jthread> out_reader;
        std::optional<std::jthread> err_reader;
        if (out.read.valid())
        {
            out_reader.emplace(
                drain,
                std::move(out.read),
                std::ref(result.out),
                std::cref(spec.out_tap));
        }
        if (err.read.valid())
        {
            err_reader.emplace(
                drain,
                std::move(err.read),
                std::ref(result.err),
                std::cref(spec.err_tap));
        }

        if (spec.kill_after.has_value())
        {
            const std::int64_t raw = spec.kill_after->count();
            const DWORD        ceiling = static_cast<DWORD>(
                raw <= 0 ? 0
                         : std::min<std::int64_t>(
                               raw,
                               static_cast<std::int64_t>(0x7FFFFFFF)));
            if (WaitForSingleObject(child.get(), ceiling) == WAIT_TIMEOUT)
            {
                result.timed_out = true;
                TerminateProcess(child.get(), WAIT_TIMEOUT);
                WaitForSingleObject(child.get(), 5'000);
            }
        }
        else
        {
            WaitForSingleObject(child.get(), INFINITE);
        }
        if (out_reader.has_value())
        {
            out_reader->join();
        }
        if (err_reader.has_value())
        {
            err_reader->join();
        }
        DWORD code = 0;
        GetExitCodeProcess(child.get(), &code);
        result.exit_code = static_cast<int>(code);
        return result;
    }

#endif // defined(_WIN32)

    /**
     * @brief 用户的家目录（Windows 读 USERPROFILE，其余平台读 HOME）。
     *
     * 两个变量都未设置时返回空路径——调用方报告可操作的错误，而不是猜测
     * 一个回退位置。
     */
    [[nodiscard]]
    inline auto home_directory() -> std::filesystem::path
    {
#if defined(_WIN32)
        if (const auto env = cg::util::cstd::get_env("USERPROFILE"))
        {
            return std::filesystem::path{*env};
        }
#else
        if (const auto env = cg::util::cstd::get_env("HOME"))
        {
            return std::filesystem::path{*env};
        }
#endif
        return {};
    }

    namespace detail
    {

#if defined(CG_CPUID_X86)
        /**
         * @brief CPUID 四寄存器读取（x86 家族探测的内部工具）。
         *
         * @internal 两个编译器分支：MSVC 用 `<intrin.h>` 的
         * __cpuidex；GCC/Clang 用 `<cpuid.h>` 的 __get_cpuid_count（返回 0
         * 表示叶号超出该 处理器的最大叶，此时 out 保持调用方给的初值——探测按"无
         *           能力"处理，宁漏勿猜）。
         */
        inline void cpuid_regs(
            std::uint32_t leaf,
            std::uint32_t subleaf,
            std::uint32_t (&out)[4])
        {
#if defined(_MSC_VER)
            // __cpuidex 的数组参数是 int[4]：以局部 int 数组中转后按位回拷
            // （CPUID 各寄存器的位型解读全部在调用方，此处只搬运比特）。
            int regs[4] = {0, 0, 0, 0};
            __cpuidex(regs, static_cast<int>(leaf), static_cast<int>(subleaf));
            for (std::size_t i = 0; i < 4; ++i)
            {
                out[i] = static_cast<std::uint32_t>(regs[i]);
            }
#else
            (void)__get_cpuid_count(
                leaf,
                subleaf,
                &out[0],
                &out[1],
                &out[2],
                &out[3]);
#endif
        }

        /**
         * @brief 扩展控制寄存器 XCR0 的当前值。
         *
         * @internal 前置条件：CPUID.1:ECX 的 OSXSAVE 位已确认——xgetbv 指令
         *           只在操作系统启用 xsave 后才可安全执行。MSVC 分支用
         *           `<intrin.h>` 的 _xgetbv 内建；GCC/Clang 分支用内联汇编
         *           （编译器族的 xgetbv 内建要求 TU 开 -mxsave，探测 TU 是
         *           全工程统一指令集的模块单元，不开该开关）。
         */
        [[nodiscard]]
        inline auto read_xcr0() -> std::uint64_t
        {
#if defined(_MSC_VER)
            return static_cast<std::uint64_t>(_xgetbv(0));
#else
            std::uint32_t low = 0;
            std::uint32_t high = 0;
            asm volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
            return (static_cast<std::uint64_t>(high) << 32)
                   | static_cast<std::uint64_t>(low);
#endif
        }
#endif // defined(CG_CPUID_X86)

    } // namespace detail

    /**
     * @brief x86 批量位运算的能力快照（默认指令集之外的可用档）。
     *
     * @internal 只报告 CPU 与操作系统双重确认过的档位；任何一环存疑都
     *           报 false（宁漏勿猜：误报会让旧机器在执行向量指令时收到
     *           非法指令异常）。avx2 = CPUID.7:EBX 位 5 且 XCR0 的 XMM 与
     *           YMM 状态位都已由操作系统启用；avx512 在 avx2 之上再加
     *           CPUID.7:EBX 的 AVX512F/VL/BW/DQ 位（覆盖向量档翻译单元在
     *           其指令集开关下可能生成的常用子集，含尾部标量循环被自动
     *           向量化的形态）与 XCR0 的 opmask、ZMM_Hi256、Hi16_ZMM 状态
     *           位。非 x86 家族恒双 false，分派侧因此固定在标量档。
     */
    struct CpuBitOpsCaps
    {
        bool avx2 = false;
        bool avx512 = false;
    };

    /**
     * @brief 探测本机批量位运算档位（每次调用现探，无缓存）。
     *
     * 调用方（cg.analysis.propagate 的档位分派）以函数内静态量把探测
     * 结果固定成进程内一次；本函数自身保持无状态，便于测试与诊断
     * 独立复探。
     */
    [[nodiscard]]
    inline CpuBitOpsCaps cpu_bitops_caps()
    {
        CpuBitOpsCaps caps;
#if defined(CG_CPUID_X86)
        std::uint32_t feature1[4] = {0, 0, 0, 0};
        detail::cpuid_regs(1, 0, feature1);
        // CPUID.1:ECX 位 27 = OSXSAVE（OS 已启用 xsave 及扩展状态管理）。
        if ((feature1[2] & (1u << 27)) == 0)
        {
            return caps;
        }
        const std::uint64_t xcr0 = detail::read_xcr0();
        // 位 1 = XMM、位 2 = YMM：两者齐备才允许执行任何 256 位向量指令。
        if ((xcr0 & 0x6) != 0x6)
        {
            return caps;
        }
        std::uint32_t feature7[4] = {0, 0, 0, 0};
        detail::cpuid_regs(7, 0, feature7);
        const std::uint32_t ebx = feature7[1];
        caps.avx2 = (ebx & (1u << 5)) != 0;
        // 位 5 = opmask、位 6 = ZMM_Hi256、位 7 = Hi16_ZMM。
        const bool zmm_state = (xcr0 & 0xE0) == 0xE0;
        caps.avx512 = caps.avx2 && zmm_state && (ebx & (1u << 16)) != 0
                      &&                         // AVX512F
                      (ebx & (1u << 31)) != 0 && // AVX512VL
                      (ebx & (1u << 30)) != 0 && // AVX512BW
                      (ebx & (1u << 17)) != 0;   // AVX512DQ
#endif
        return caps;
    }

} // namespace cg::util::platform
