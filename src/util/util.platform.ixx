module;
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#include <winioctl.h>
#include <psapi.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
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
 * 独立成分区是为了让 OS API 的使用集中在恰一个单元里（跨平台规则）：
 * 每个助手都有 Windows 与 POSIX 两个分支，工程
 * 其他任何地方都不得直接 include 平台头。全局模块片段里这些平台头是
 * cg.util 触碰 OS API 的唯一场所（跨平台指令要求两分支皆实现）；宏与
 * C 声明永不跨模块边界，故 include 住在全局模块片段。
 */
export module cg.util.platform;

import std;
import cg.util.cstd;

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
     * 用作 `ccg update` 认领持有者令牌的人类可读一半（租约协议：令牌只指名持有者）：
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
        const HANDLE handle = OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE,
            static_cast<DWORD>(pid));
        if (handle == nullptr)
        {
            return GetLastError() == ERROR_ACCESS_DENIED;
        }
        DWORD      code = 0;
        const bool queried = GetExitCodeProcess(handle, &code) != 0;
        CloseHandle(handle);
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

        const HANDLE handle = CreateFileW(
            tmp_native.c_str(),
            DELETE | SYNCHRONIZE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (handle != INVALID_HANDLE_VALUE)
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
                handle,
                file_rename_info_ex,
                info,
                static_cast<DWORD>(bytes));
            CloseHandle(handle);
            if (renamed != 0)
            {
                return true;
            }
        }
        // 回退：目标被打开且不允许删除时这里同样失败——返回 false 并保留
        // 旧文件，由调用方清理 tmp。
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
        const HANDLE handle = CreateFileW(
            detail::extended_win_path(tmp).c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (handle == INVALID_HANDLE_VALUE)
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
                    handle,
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
        CloseHandle(handle);
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
     *           独占句柄都在析构/移动赋值中关闭，解除映射先于关文件。
     */
    class ReadOnlyMap
    {
    public:
        ReadOnlyMap() = default;
        /** @brief 打开并映射整个文件；任何一步失败则对象保持空（valid 假）。 */
        explicit ReadOnlyMap(const std::filesystem::path& path)
        {
#if defined(_WIN32)
            const HANDLE file = CreateFileW(
                detail::extended_win_path(path).c_str(),
                GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                return;
            }
            LARGE_INTEGER size{};
            if (GetFileSizeEx(file, &size) == 0 || size.QuadPart <= 0)
            {
                CloseHandle(file);
                return;
            }
            const HANDLE mapping =
                CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
            if (mapping == nullptr)
            {
                CloseHandle(file);
                return;
            }
            void* view = MapViewOfFile(
                mapping,
                FILE_MAP_READ,
                0,
                0,
                static_cast<std::size_t>(size.QuadPart));
            if (view == nullptr)
            {
                CloseHandle(mapping);
                CloseHandle(file);
                return;
            }
            file_ = file;
            mapping_ = mapping;
            view_ = view;
            size_ = static_cast<std::size_t>(size.QuadPart);
#else
            const int fd = ::open(path.c_str(), O_RDONLY);
            if (fd < 0)
            {
                return;
            }
            struct stat st{};
            if (::fstat(fd, &st) != 0 || st.st_size <= 0)
            {
                ::close(fd);
                return;
            }
            void* view = ::mmap(
                nullptr,
                static_cast<std::size_t>(st.st_size),
                PROT_READ,
                MAP_PRIVATE,
                fd,
                0);
            if (view == MAP_FAILED)
            {
                ::close(fd);
                return;
            }
            fd_ = fd;
            view_ = view;
            size_ = static_cast<std::size_t>(st.st_size);
#endif
        }

        ~ReadOnlyMap() { release(); }
        ReadOnlyMap(const ReadOnlyMap&) = delete;
        ReadOnlyMap& operator=(const ReadOnlyMap&) = delete;

        ReadOnlyMap(ReadOnlyMap&& other) noexcept
            :
#if defined(_WIN32)
              file_(other.file_), mapping_(other.mapping_),
#else
              fd_(other.fd_),
#endif
              view_(other.view_), size_(other.size_)
        {
#if defined(_WIN32)
            other.file_ = INVALID_HANDLE_VALUE;
            other.mapping_ = nullptr;
#else
            other.fd_ = -1;
#endif
            other.view_ = nullptr;
            other.size_ = 0;
        }

        ReadOnlyMap& operator=(ReadOnlyMap&& other) noexcept
        {
            if (this != &other)
            {
                release();
#if defined(_WIN32)
                file_ = other.file_;
                mapping_ = other.mapping_;
                other.file_ = INVALID_HANDLE_VALUE;
                other.mapping_ = nullptr;
#else
                fd_ = other.fd_;
                other.fd_ = -1;
#endif
                view_ = other.view_;
                size_ = other.size_;
                other.view_ = nullptr;
                other.size_ = 0;
            }
            return *this;
        }

        /** @brief 映射是否有效（空文件或打开失败时假）。 */
        [[nodiscard]]
        bool valid() const noexcept
        {
            return view_ != nullptr;
        }

        /** @brief 映射字节（valid 假时空视图）。 */
        [[nodiscard]]
        auto bytes() const noexcept -> std::span<const std::byte>
        {
            return {static_cast<const std::byte*>(view_), size_};
        }

        void release() noexcept
        {
            if (view_ != nullptr)
            {
#if defined(_WIN32)
                (void)UnmapViewOfFile(view_);
                CloseHandle(mapping_);
                mapping_ = nullptr;
                CloseHandle(file_);
                file_ = INVALID_HANDLE_VALUE;
#else
                (void)::munmap(view_, size_);
                ::close(fd_);
                fd_ = -1;
#endif
                view_ = nullptr;
                size_ = 0;
            }
        }

    private:
#if defined(_WIN32)
        HANDLE file_ = INVALID_HANDLE_VALUE;
        HANDLE mapping_ = nullptr;
#else
        int fd_ = -1;
#endif
        void*       view_ = nullptr;
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
     */
    class FileLock
    {
    public:
        FileLock() = default;
        ~FileLock() { release(); }
        FileLock(const FileLock&) = delete;
        FileLock& operator=(const FileLock&) = delete;
        FileLock(FileLock&& other) noexcept
            : path_(std::move(other.path_)), held_(other.held_)
#if defined(_WIN32)
              ,
              handle_(other.handle_)
#else
              ,
              fd_(other.fd_)
#endif
        {
            other.held_ = false;
#if defined(_WIN32)
            other.handle_ = INVALID_HANDLE_VALUE;
#else
            other.fd_ = -1;
#endif
        }
        FileLock& operator=(FileLock&& other) noexcept
        {
            if (this != &other)
            {
                release();
                path_ = std::move(other.path_);
                held_ = other.held_;
                other.held_ = false;
#if defined(_WIN32)
                handle_ = other.handle_;
                other.handle_ = INVALID_HANDLE_VALUE;
#else
                fd_ = other.fd_;
                other.fd_ = -1;
#endif
            }
            return *this;
        }

        [[nodiscard]]
        auto held() const noexcept -> bool
        {
            return held_;
        }

        void release()
        {
            if (!held_)
            {
                return;
            }
            held_ = false;
#if defined(_WIN32)
            if (handle_ != INVALID_HANDLE_VALUE)
            {
                CloseHandle(handle_);
                handle_ = INVALID_HANDLE_VALUE;
            }
            if (!path_.empty())
            {
                (void)DeleteFileW(detail::extended_win_path(path_).c_str());
            }
#else
            if (fd_ >= 0)
            {
                (void)::close(fd_);
                fd_ = -1;
            }
            if (!path_.empty())
            {
                (void)::unlink(path_.native().c_str());
            }
#endif
            path_.clear();
        }

    private:
        friend auto try_acquire_file_lock(
            const std::filesystem::path&,
            std::string_view,
            std::int64_t) -> FileLock;
        std::filesystem::path path_;
        bool                  held_ = false;
#if defined(_WIN32)
        HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
        int fd_ = -1;
#endif
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
        const auto create = [&](FileLock& lock) -> bool
        {
#if defined(_WIN32)
            const std::wstring native = detail::extended_win_path(lock_path);
            const HANDLE       handle = CreateFileW(
                native.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (handle == INVALID_HANDLE_VALUE)
            {
                return false;
            }
            DWORD written = 0;
            (void)WriteFile(
                handle,
                content.data(),
                static_cast<DWORD>(content.size()),
                &written,
                nullptr);
            lock.handle_ = handle;
#else
            const int fd = ::open(
                lock_path.native().c_str(),
                O_CREAT | O_EXCL | O_WRONLY,
                0600);
            if (fd < 0)
            {
                return false;
            }
            (void)::write(fd, content.data(), content.size());
            lock.fd_ = fd;
#endif
            lock.path_ = lock_path;
            lock.held_ = true;
            return true;
        };

        FileLock lock;
        if (create(lock))
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
        (void)create(lock);
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
        const HANDLE handle = CreateFileW(
            wide.c_str(),
            FILE_ALL_ACCESS,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS,
            nullptr);
        if (handle == INVALID_HANDLE_VALUE)
        {
            return;
        }
        constexpr USHORT format = COMPRESSION_FORMAT_DEFAULT;
        DWORD            returned = 0;
        (void)DeviceIoControl(
            handle,
            FSCTL_SET_COMPRESSION,
            const_cast<USHORT*>(&format),
            sizeof(format),
            nullptr,
            0,
            &returned,
            nullptr);
        CloseHandle(handle);
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
     * @brief 以直接子进程运行 exe（args 为 UTF-8），只捕获 STDOUT。
     *
     * 中间没有 shell，也就没有引号方言要斗。stderr 留在父进程的 stderr
     * 上：ccg 的机器模式在 stdout 上恰打印一行 JSON，调用方消费的仅此
     * 而已。
     *
     * @return 进程无法孵化时返回 false；否则 out 持捕获到的字节、
     *         exit_code 持子进程状态（平台无法表达时为 -1）。
     *
     * @internal
     * 参数不得内嵌引号或 NUL（调用方早在到达这里之前就会拒绝）。Windows
     * 分支逐参数做 UTF-8 → UTF-16 转换（hook 载荷以 UTF-8 JSON 到达）。
     */
    [[nodiscard]]
    inline auto capture_exec(
        const std::filesystem::path&    exe,
        const std::vector<std::string>& args,
        std::string&                    out,
        int&                            exit_code) -> bool
    {
        out.clear();
        exit_code = -1;
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
        HANDLE read_end = nullptr;
        HANDLE write_end = nullptr;
        if (!CreatePipe(&read_end, &write_end, &inheritable, 0))
        {
            return false;
        }
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = write_end;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        // 空标准句柄兜底：宿主无控制台（计划任务/服务式拉起）时
        // GetStdHandle 返回空句柄，STARTF_USESTDHANDLES 把它直传给子进程
        // 会让对方 CRT 启动即崩（微软文档登记形态）。NUL 设备读写各开一份
        // 可继承句柄顶上（同文件 capture_exec_timed 的先例）。
        HANDLE null_input = nullptr;
        HANDLE null_error = nullptr;
        if (si.hStdInput == nullptr || si.hStdInput == INVALID_HANDLE_VALUE)
        {
            null_input = CreateFileW(
                L"NUL",
                GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                &inheritable,
                OPEN_EXISTING,
                0,
                nullptr);
            if (null_input == INVALID_HANDLE_VALUE)
            {
                null_input = nullptr; // 兜底失败时维持原句柄
            }
            else
            {
                si.hStdInput = null_input;
            }
        }
        if (si.hStdError == nullptr || si.hStdError == INVALID_HANDLE_VALUE)
        {
            null_error = CreateFileW(
                L"NUL",
                GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                &inheritable,
                OPEN_EXISTING,
                0,
                nullptr);
            if (null_error == INVALID_HANDLE_VALUE)
            {
                null_error = nullptr; // 兜底失败时维持原句柄
            }
            else
            {
                si.hStdError = null_error;
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
        CloseHandle(write_end);
        if (!spawned)
        {
            CloseHandle(read_end);
            if (null_input != nullptr)
            {
                CloseHandle(null_input);
            }
            if (null_error != nullptr)
            {
                CloseHandle(null_error);
            }
            return false;
        }
        char  buffer[4096];
        DWORD got = 0;
        while (ReadFile(read_end, buffer, sizeof(buffer), &got, nullptr)
               && got != 0)
        {
            out.append(buffer, got);
        }
        CloseHandle(read_end);
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 0;
        if (GetExitCodeProcess(pi.hProcess, &code))
        {
            exit_code = static_cast<int>(code);
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (null_input != nullptr)
        {
            CloseHandle(null_input);
        }
        if (null_error != nullptr)
        {
            CloseHandle(null_error);
        }
        return true;
#else
        int fds[2];
        if (::pipe(fds) != 0)
        {
            return false;
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
            ::close(fds[0]);
            ::dup2(fds[1], STDOUT_FILENO);
            ::close(fds[1]);
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
            ::close(fds[0]);
            ::close(fds[1]);
            return false;
        }
        ::close(fds[1]);
        char    buffer[4096];
        ssize_t got = 0;
        while ((got = ::read(fds[0], buffer, sizeof(buffer))) > 0)
        {
            out.append(buffer, static_cast<std::size_t>(got));
        }
        ::close(fds[0]);
        int status = 0;
        if (::waitpid(pid, &status, 0) == pid && WIFEXITED(status))
        {
            exit_code = WEXITSTATUS(status);
        }
        return true;
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
    [[nodiscard]]
    inline auto capture_exec_timed(
        const std::filesystem::path&    exe,
        const std::vector<std::string>& args,
        std::chrono::milliseconds       timeout) -> TimedExecResult
    {
        TimedExecResult result;
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
        const auto make_temp = [&inheritable]() -> HANDLE
        {
            std::error_code             ec;
            const std::filesystem::path dir =
                std::filesystem::temp_directory_path(ec);
            if (ec)
            {
                return INVALID_HANDLE_VALUE;
            }
            wchar_t name[MAX_PATH + 1] = {};
            if (GetTempFileNameW(dir.wstring().c_str(), L"ccg", 0, name) == 0)
            {
                return INVALID_HANDLE_VALUE;
            }
            const HANDLE handle = CreateFileW(
                name,
                GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                &inheritable,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                nullptr);
            if (handle == INVALID_HANDLE_VALUE)
            {
                DeleteFileW(name);
            }
            return handle;
        };
        const HANDLE out_file = make_temp();
        const HANDLE err_file = make_temp();
        const HANDLE dev_null = CreateFileW(
            L"NUL",
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            &inheritable,
            OPEN_EXISTING,
            0,
            nullptr);
        if (out_file == INVALID_HANDLE_VALUE || err_file == INVALID_HANDLE_VALUE
            || dev_null == INVALID_HANDLE_VALUE)
        {
            if (out_file != INVALID_HANDLE_VALUE)
            {
                CloseHandle(out_file);
            }
            if (err_file != INVALID_HANDLE_VALUE)
            {
                CloseHandle(err_file);
            }
            if (dev_null != INVALID_HANDLE_VALUE)
            {
                CloseHandle(dev_null);
            }
            return result;
        }

        // 句柄白名单：bInheritHandles=TRUE 让子进程拿到重定向句柄，但没有
        // PROC_THREAD_ATTRIBUTE_HANDLE_LIST 时它同时继承宿主的 stdout/
        // stderr 管道（以及任何其它可继承句柄）。hook 退出后宿主读管道要
        // 等后台 update 结束才见 EOF。白名单把可继承集收窄到这三个文件。
        STARTUPINFOEXW si{};
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdOutput = out_file;
        si.StartupInfo.hStdError = err_file;
        si.StartupInfo.hStdInput = dev_null;
        LPPROC_THREAD_ATTRIBUTE_LIST attributes = nullptr;
        bool                         listed = false;
        // lpValue 必须存活到 DeleteProcThreadAttributeList（属性列表只保存
        // 指针而非拷贝），故句柄数组放在函数作用域而不是属性初始化块内。
        HANDLE inherited_handles[] = {out_file, err_file, dev_null};
        {
            SIZE_T bytes = 0;
            // 首射只取所需缓冲区大小（文档化的 ERROR_INSUFFICIENT_BUFFER
            // 探测）；属性列表的内存在 CreateProcess 返回前必须保持有效。
            (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
            if (bytes > 0)
            {
                attributes = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
                    std::malloc(bytes));
            }
            if (attributes != nullptr
                && InitializeProcThreadAttributeList(attributes, 1, 0, &bytes))
            {
                listed = UpdateProcThreadAttribute(
                             attributes,
                             0,
                             PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                             inherited_handles,
                             sizeof(inherited_handles),
                             nullptr,
                             nullptr)
                         != 0;
                if (!listed)
                {
                    DeleteProcThreadAttributeList(attributes);
                }
            }
        }
        // 属性列表必须挂到 STARTUPINFOEXW::lpAttributeList 上，否则
        // CreateProcess 根本看不到白名单（只初始化不挂接 = 白名单静默失效，
        // 全继承语义照旧）。未列为扩展时保持 STARTUPINFOW 的旧 ABI（cb 与
        // 创建旗标必须一致）。
        si.lpAttributeList = listed ? attributes : nullptr;
        si.StartupInfo.cb =
            listed ? sizeof(STARTUPINFOEXW) : sizeof(STARTUPINFOW);
        const DWORD extended = listed ? EXTENDED_STARTUPINFO_PRESENT : 0;
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
        if (listed)
        {
            DeleteProcThreadAttributeList(attributes);
        }
        std::free(attributes);
        result.restricted_handles = listed;
        CloseHandle(dev_null);
        if (!spawned)
        {
            CloseHandle(out_file);
            CloseHandle(err_file);
            return result;
        }
        result.started = true;

        const DWORD wait_ms = static_cast<DWORD>(std::min<std::int64_t>(
            timeout.count(),
            static_cast<std::int64_t>(0xFFFFFFFFLL)));
        const DWORD waited = WaitForSingleObject(pi.hProcess, wait_ms);
        const auto  read_back = [](HANDLE handle, std::string& sink)
        {
            SetLastError(
                ERROR_SUCCESS); // INVALID_SET_FILE_POINTER 也是合法偏移
            if (SetFilePointer(handle, 0, nullptr, FILE_BEGIN)
                    == INVALID_SET_FILE_POINTER
                && GetLastError() != ERROR_SUCCESS)
            {
                return;
            }
            char  buffer[4096];
            DWORD got = 0;
            while (ReadFile(handle, buffer, sizeof(buffer), &got, nullptr)
                   && got != 0)
            {
                sink.append(buffer, got);
            }
        };
        if (waited == WAIT_OBJECT_0)
        {
            DWORD code = 0;
            if (GetExitCodeProcess(pi.hProcess, &code))
            {
                result.exit_code = static_cast<int>(code);
            }
            read_back(out_file, result.out);
            read_back(err_file, result.err);
        }
        else if (waited == WAIT_TIMEOUT)
        {
            result.timed_out = true;
        }
        // 超时分支刻意不读输出、不等待：本进程的两个读端一关，子进程的
        // 独立写端继续存活，临时文件在它退出时随最后一个句柄一起删除。
        CloseHandle(out_file);
        CloseHandle(err_file);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return result;
#else
        const auto make_temp = []() -> int
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
                const int fd =
                    ::open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
                if (fd >= 0)
                {
                    // 先 unlink：父进程超时/退出都不会留下可被他人看到的
                    // 文件；fd 仍可读写，空间随最后一个引用关闭而释放。
                    ::unlink(name.c_str());
                    return fd;
                }
                if (errno != EEXIST)
                {
                    break;
                }
            }
            return -1;
        };
        const int out_fd = make_temp();
        const int err_fd = make_temp();
        if (out_fd < 0 || err_fd < 0)
        {
            if (out_fd >= 0)
            {
                ::close(out_fd);
            }
            if (err_fd >= 0)
            {
                ::close(err_fd);
            }
            return result;
        }

        // exec 失败哨兵：写端带 FD_CLOEXEC，exec 成功即在子进程里关闭，
        // 父进程读端看到 EOF；失败时子进程先把 errno 写进来。没有它，
        // fork 成功而 execv 失败会以 exit 127/空输出伪装成"已启动"。
        int        exec_ctl[2] = {-1, -1};
        const bool have_exec_ctl = ::pipe(exec_ctl) == 0;
        if (have_exec_ctl)
        {
            (void)::fcntl(exec_ctl[0], F_SETFD, FD_CLOEXEC);
            (void)::fcntl(exec_ctl[1], F_SETFD, FD_CLOEXEC);
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
                ::close(exec_ctl[0]);
            }
            // 新会话：脱离父进程的进程组与控制终端，父进程退出后继续运行。
            (void)::setsid();
            const int dev_null = ::open("/dev/null", O_RDONLY);
            if (dev_null >= 0)
            {
                ::dup2(dev_null, STDIN_FILENO);
                if (dev_null > STDERR_FILENO)
                {
                    ::close(dev_null);
                }
            }
            ::dup2(out_fd, STDOUT_FILENO);
            ::dup2(err_fd, STDERR_FILENO);
            if (out_fd > STDERR_FILENO)
            {
                ::close(out_fd);
            }
            if (err_fd > STDERR_FILENO)
            {
                ::close(err_fd);
            }
            // 关闭 3 号以上全部 fd：detached 子进程不得继承宿主的管道/套接
            // 字写端，否则宿主读端要等后台 update 退出才见到 EOF。stdin/
            // stdout/stderr 已换成 /dev/null 与临时文件。Linux 用
            // /proc/self/fd 精确枚举；其它 POSIX 扫描 RLIMIT_NOFILE。
#if defined(__linux__)
            if (DIR* fds = ::opendir("/proc/self/fd"))
            {
                const int scan = ::dirfd(fds);
                while (const dirent* entry = ::readdir(fds))
                {
                    const int fd = std::atoi(entry->d_name);
                    if (fd > STDERR_FILENO && fd != scan
                        && (!have_exec_ctl || fd != exec_ctl[1]))
                    {
                        ::close(fd);
                    }
                }
                ::closedir(fds);
            }
            else
#endif
            {
                long limit = ::sysconf(_SC_OPEN_MAX);
                if (limit < 0 || limit > 65536)
                {
                    limit = 65536;
                }
                for (int fd = STDERR_FILENO + 1; fd < static_cast<int>(limit);
                     ++fd)
                {
                    if (have_exec_ctl && fd == exec_ctl[1])
                    {
                        continue; // 哨兵写端：exec 成功时由 FD_CLOEXEC 关闭
                    }
                    ::close(fd);
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
                (void)::write(exec_ctl[1], &exec_errno, sizeof(exec_errno));
            }
            ::_exit(127);
        }
        if (have_exec_ctl)
        {
            ::close(exec_ctl[1]); // 父进程只留读端
        }
        if (pid < 0)
        {
            if (have_exec_ctl)
            {
                ::close(exec_ctl[0]);
            }
            ::close(out_fd);
            ::close(err_fd);
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
        const auto read_back = [](int fd, std::string& sink)
        {
            if (::lseek(fd, 0, SEEK_SET) < 0)
            {
                return;
            }
            char    buffer[4096];
            ssize_t got = 0;
            while ((got = ::read(fd, buffer, sizeof(buffer))) > 0)
            {
                sink.append(buffer, static_cast<std::size_t>(got));
            }
        };
        // 哨兵判定：4 字节 errno = execv 失败（子进程从未变成目标程序）；
        // EOF（0 字节）或 EAGAIN（超时前还没轮到它 exec）都按已启动处理。
        bool exec_failed = false;
        if (have_exec_ctl)
        {
            int     exec_errno = 0;
            ssize_t got = -1;
            do
            {
                got = ::read(exec_ctl[0], &exec_errno, sizeof(exec_errno));
            } while (got < 0 && errno == EINTR);
            exec_failed = got == static_cast<ssize_t>(sizeof(exec_errno));
            ::close(exec_ctl[0]);
        }
        if (reaped)
        {
            if (WIFEXITED(status))
            {
                result.exit_code = WEXITSTATUS(status);
            }
            read_back(out_fd, result.out);
            read_back(err_fd, result.err);
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
        ::close(out_fd);
        ::close(err_fd);
        return result;
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
     */
    [[nodiscard]]
    inline auto run_child(const ChildRunSpec& spec) -> ChildRunResult
    {
        ChildRunResult      result;
        SECURITY_ATTRIBUTES inheritable{
            sizeof(SECURITY_ATTRIBUTES),
            nullptr,
            TRUE};
        HANDLE     in_read = nullptr;
        HANDLE     in_write = nullptr;
        HANDLE     out_read = nullptr;
        HANDLE     out_write = nullptr;
        HANDLE     err_read = nullptr;
        HANDLE     err_write = nullptr;
        const bool use_std_handles = !spec.inherit_parent_stdio;
        bool       pipes_ok = true;
        if (use_std_handles && spec.stdin_mode == ChildStdin::EofPipe
            && !CreatePipe(&in_read, &in_write, &inheritable, 0))
        {
            pipes_ok = false;
        }
        if (pipes_ok && use_std_handles && spec.capture_stdout
            && !CreatePipe(&out_read, &out_write, &inheritable, 0))
        {
            pipes_ok = false;
        }
        if (pipes_ok && use_std_handles && spec.capture_stderr
            && !CreatePipe(&err_read, &err_write, &inheritable, 0))
        {
            pipes_ok = false;
        }
        if (!pipes_ok)
        {
            // 部分建成的管道一并关闭（合一前的骨架在此泄漏先建的一端）。
            if (in_read != nullptr)
            {
                CloseHandle(in_read);
            }
            if (in_write != nullptr)
            {
                CloseHandle(in_write);
            }
            if (out_read != nullptr)
            {
                CloseHandle(out_read);
            }
            if (out_write != nullptr)
            {
                CloseHandle(out_write);
            }
            if (err_read != nullptr)
            {
                CloseHandle(err_read);
            }
            if (err_write != nullptr)
            {
                CloseHandle(err_write);
            }
            return result;
        }
        result.pipes_ok = true;
        // 我方持有的读端不继承进子进程（排水端是本进程私有的）。
        if (in_write != nullptr)
        {
            SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
        }
        if (out_read != nullptr)
        {
            SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
        }
        if (err_read != nullptr)
        {
            SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);
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
                startup.hStdInput = in_read;
                break;
            case ChildStdin::NullHandle:
            default:
                startup.hStdInput = nullptr;
                break;
            }
            startup.hStdOutput = out_write != nullptr
                                     ? out_write
                                     : GetStdHandle(STD_OUTPUT_HANDLE);
            startup.hStdError = err_write != nullptr
                                    ? err_write
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
        if (in_read != nullptr)
        {
            CloseHandle(in_read);
        }
        if (in_write != nullptr)
        {
            CloseHandle(in_write);
        }
        if (out_write != nullptr)
        {
            CloseHandle(out_write);
        }
        if (err_write != nullptr)
        {
            CloseHandle(err_write);
        }
        if (!ok)
        {
            if (out_read != nullptr)
            {
                CloseHandle(out_read);
            }
            if (err_read != nullptr)
            {
                CloseHandle(err_read);
            }
            result.spawn_error = spawn_error;
            return result;
        }
        result.started = true;

        // 逐字排水循环（合一前四份骨架的共同核心）：阻塞读本流到 EOF、
        // 逐块累积、可选钩子、收尾关端。两路并发由各持一条线程保证。
        const auto drain =
            [](HANDLE pipe, std::string& sink, const DrainTap& tap)
        {
            char  buffer[4096];
            DWORD got = 0;
            for (;;)
            {
                if (!ReadFile(pipe, buffer, sizeof(buffer), &got, nullptr)
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
            CloseHandle(pipe);
            if (tap.on_finish)
            {
                tap.on_finish();
            }
        };
        std::optional<std::jthread> out_reader;
        std::optional<std::jthread> err_reader;
        if (out_read != nullptr)
        {
            out_reader.emplace(
                drain,
                out_read,
                std::ref(result.out),
                std::cref(spec.out_tap));
        }
        if (err_read != nullptr)
        {
            err_reader.emplace(
                drain,
                err_read,
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
            if (WaitForSingleObject(process.hProcess, ceiling) == WAIT_TIMEOUT)
            {
                result.timed_out = true;
                TerminateProcess(process.hProcess, WAIT_TIMEOUT);
                WaitForSingleObject(process.hProcess, 5'000);
            }
        }
        else
        {
            WaitForSingleObject(process.hProcess, INFINITE);
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
        GetExitCodeProcess(process.hProcess, &code);
        result.exit_code = static_cast<int>(code);
        CloseHandle(process.hProcess);
        CloseHandle(process.hThread);
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
