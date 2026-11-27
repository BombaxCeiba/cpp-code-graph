module;
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <csignal>
#ifdef _MSC_VER
#include <crtdbg.h>
#include <stdlib.h>
#endif
#endif

/**
 * @module cg.util.crashdump
 *
 * @brief 进程级崩溃自动转储：环境变量 CCG_CRASH_DUMP_DIR 有值时，
 *        在启动最早期武装全套崩溃回调，任何可拦的致命故障都把完整
 *        内存转储写进该目录——崩溃防护家族（cg.util.guard 与本模块）
 *        的一员：全工程仅这两处触碰结构化异常或信号机制。
 *
 * @internal
 * 可拦面：访问违例、栈溢出、堆破坏、未捕获 C++ 异常（进程级过滤器
 * 的二阶段分发 + set_terminate 双保险）、abort 家族（SIGABRT 处理器；
 * MSVC 侧另挂纯虚调用与 UCRT 非法参数处理器——后者不挂则默认走
 * 快速失败不可拦）。
 * 不可拦面（如实定界，不夸大）：快速失败族——__fastfail /
 * RaiseFailFastException / MSVC 栈保护 cookie 校验失败——按 Windows
 * 设计绕过一切用户态回调（实测连向量化异常处理器也拦不到），只有
 * 系统级 WER 的内核路径能收，与本机制互补不冲突。
 * 架构：崩溃线程的处理器只发一个事件（栈需求几十字节），由启动期
 * 创建、栈完好的常驻转储线程写盘——栈溢出时崩溃线程只剩残栈，
 * 原地写盘的探针实测全部只留 0 字节文件，专用转储线程是唯一可靠
 * 形态。写完以原异常码 TerminateProcess：退出码语义与不挂载时
 * 完全一致（外壳显示的退出码不变）。
 * 崩溃态纪律：处理器与写盘路径只用系统 API + 静态/栈上缓冲——
 * CRT 堆可能已被堆破坏形态波及，std::string / malloc / new 一律
 * 禁入；路径拼接、目录创建、文件写盘全部走 kernel32/user32 直呼。
 * 路径上取到的文件、事件与线程句柄都交给 cg.util.handle 的拥有型对
 * 象：它是栈上的一个指针大小的值，析构只调关闭句柄，不分配内存，不
 * 违反本纪律。调试帮助库的模块句柄不在此列：它按设计常驻，直接登记
 * 在模块级变量里（登记处见 g_dbghelp 旁的说明）。
 * 与既有恢复层的边界：cg.util.guard 的 run_guarded 帧内吞掉的
 * first-chance 硬件故障到不了进程级过滤器（不误转储）；栈溢出不在
 * 其白名单、会传播过来（正确转储）；libclang 的向量化恢复层只在
 * PARSE 上下文内吞异常，上下文外放行，零冲突。ASAN 插桩的进程里
 * ASAN 自己的拦截器先于本机制报告并终止（其报告即诊断材料）。
 * POSIX 分支为空实现：崩溃走系统 core dump 的既有契约。
 * 平台头遵循跨平台规则（与 cg.util.guard 相同）：windows.h 与 CRT
 * 头住在全局模块片段，宏与 C 声明永不跨模块边界。
 */
export module cg.util.crashdump;

import std;
import cg.util.handle;

export namespace cg::util::crashdump
{

#if defined(_WIN32)

    namespace detail
    {

        /**
         * @brief MiniDumpWriteDump 的动态解析原型。
         *
         * @internal 不引入 dbghelp.h：cl 与 LLVM-mingw 两套工具链的
         * 该头版本与伴随宏集不一致，而崩溃路径只需要这一个入口；
         * GetProcAddress 解耦后链接期对 dbghelp 零依赖（第三参在 SDK
         * 原型中即 32 位位集，按 int 书写）。
         */
        using MiniDumpWriteDumpFn =
            HRESULT(WINAPI*)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);

        // 转储类型位（dbghelp 的 MINIDUMP_TYPE 值，数值拼写免头文件）：
        // 完整内存是用户点名的形态——依赖型崩溃的堆上定位线索全在
        // 其中；其余位补充句柄表、卸载模块、线程信息等离线诊断面。
        constexpr int dump_type_full =
            0x00000002    // MiniDumpWithFullMemory
            | 0x00000004  // MiniDumpWithHandleData
            | 0x00000020  // MiniDumpWithUnloadedModules
            | 0x00000100  // MiniDumpWithProcessThreadData
            | 0x00001000  // MiniDumpWithThreadInfo
            | 0x00008000; // MiniDumpWithFullAuxiliaryState

        // 进程级状态：全部按崩溃态可访问的形态持有（静态存储 + 原子/
        // 裸指针），绝不依赖 CRT 堆。
        inline MiniDumpWriteDumpFn g_mini_dump = nullptr;
        inline HANDLE              g_wake = nullptr; // 自动复位：唤醒转储线程
        inline HANDLE              g_done = nullptr; // 自动复位：写盘完成回执
        inline volatile LONG       g_owner = 0;      // 并发崩溃的单写者闸

        // 按设计常驻、直到进程退出都不释放的资源，在此集中登记：
        // - g_dbghelp：调试帮助库的模块句柄。武装时预载一次，写盘兜底
        //   时可能再载一次；库由加载器按引用计数持有，本模块从不卸载
        //   ——转储函数指针指向库里的代码，卸载后崩溃路径一调用就是访
        //   问违例。
        // - g_wake 与 g_done：唤醒与回执两个自动复位事件句柄。常驻转储
        //   线程在 g_wake 上无限等待；它们在转储线程建成之前由栈上的拥
        //   有型对象持有，线程建成之后所有权交给这两个变量，进程退出时
        //   由系统一并回收。
        // - 常驻转储线程本身：线程句柄随武装函数里的栈对象在函数返回时关闭，
        //   线程靠事件驱动，进程退出时被一并收割。
        inline HMODULE g_dbghelp = nullptr;

        /**
         * @brief stderr 直写：WriteFile 句柄直通，不经 stdio——CRT 的
         *        stdio 缓冲在崩溃态不可信。
         */
        void report(const char* text)
        {
            DWORD written = 0;
            WriteFile(
                GetStdHandle(STD_ERROR_HANDLE),
                text,
                static_cast<DWORD>(lstrlenA(text)),
                &written,
                nullptr);
        }

        /**
         * @brief 逐级创建目录：全系统 API，任一级失败都继续——最终的
         *        CreateFileW 会把真实错误码报到 stderr。
         *
         * @internal 跳过前三字符内的分隔符（盘符根的裸反斜杠——对根
         * 建目录只会得到拒绝访问噪声；UNC 前缀的开头反斜杠同理跳过）。
         */
        void ensure_directory(const wchar_t* dir)
        {
            wchar_t partial[MAX_PATH];
            DWORD   i = 0;
            for (; dir[i] != L'\0' && i < MAX_PATH - 1; ++i)
            {
                partial[i] = dir[i];
                partial[i + 1] = L'\0';
                const bool separator = dir[i] == L'\\' || dir[i] == L'/';
                if (separator && i >= 3)
                    CreateDirectoryW(partial, nullptr);
            }
            if (i > 0)
                CreateDirectoryW(partial, nullptr);
        }

        /**
         * @brief 拼装 <目录>\\ccg-<进程号>-<年月日>-<时分秒>.dmp 到
         *        调用方缓冲；失败返回 false。
         *
         * @internal 目录值此刻直读（系统 API，不碰 CRT 堆）。进程号 +
         * 秒级时间戳保证跨进程不撞名；同进程同秒的二次崩溃由调用方
         * 的单写者闸收敛。全部字符串操作走 kernel32（lstrcpyW 家族）
         * 与 user32（wsprintfW），无 CRT 字符串例程。
         */
        bool build_dump_path(wchar_t* path, DWORD capacity)
        {
            wchar_t     dir[MAX_PATH + 4];
            const DWORD len =
                GetEnvironmentVariableW(L"CCG_CRASH_DUMP_DIR", dir, MAX_PATH);
            if (len == 0 || len >= MAX_PATH)
                return false;
            DWORD end = lstrlenW(dir);
            if (end > 0 && dir[end - 1] != L'\\' && dir[end - 1] != L'/')
            {
                dir[end] = L'\\';
                dir[end + 1] = L'\0';
                ++end;
            }
            ensure_directory(dir);
            SYSTEMTIME st;
            GetLocalTime(&st);
            wchar_t name[64];
            wsprintfW(
                name,
                L"ccg-%lu-%04u%02u%02u-%02u%02u%02u.dmp",
                GetCurrentProcessId(),
                st.wYear,
                st.wMonth,
                st.wDay,
                st.wHour,
                st.wMinute,
                st.wSecond);
            if (end + lstrlenW(name) + 1 > capacity)
                return false;
            lstrcpyW(path, dir);
            lstrcatW(path, name);
            return true;
        }

        /**
         * @brief 唯一写盘点，只在转储线程上运行。
         *
         * @internal 任何失败路径都以 stderr 一行报错后返回——调用方
         * 随后置位完成事件，崩溃线程按原异常码终止：不吞退出码，也
         * 不让任何人悬挂。
         */
        void write_dump()
        {
            wchar_t path[MAX_PATH + 64];
            if (!build_dump_path(path, MAX_PATH + 64))
            {
                report(
                    "ccg: CCG_CRASH_DUMP_DIR unusable, no crash "
                    "dump\r\n");
                return;
            }
            MiniDumpWriteDumpFn write = g_mini_dump;
            if (write == nullptr)
            {
                // 安装期预载失败的临期补载（dbghelp 是系统组件，此路
                // 几乎不可达）；崩溃期 LoadLibrary 有 loader lock 与
                // 堆已坏的风险，故它只是兜底而非设计路径。模块句柄
                // 登记进进程级状态，按设计常驻不卸载。
                g_dbghelp = LoadLibraryA("dbghelp.dll");
                if (g_dbghelp != nullptr)
                {
                    write = reinterpret_cast<MiniDumpWriteDumpFn>(
                        GetProcAddress(g_dbghelp, "MiniDumpWriteDump"));
                }
                if (write == nullptr)
                {
                    report(
                        "ccg: dbghelp.dll unavailable, no crash "
                        "dump\r\n");
                    return;
                }
            }
            // 转储文件句柄交给栈上的拥有型对象，随本函数返回关闭。
            const handle::Owned file = handle::Owned::adopt_file(CreateFileW(
                path,
                GENERIC_WRITE,
                0,
                nullptr,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                nullptr));
            if (!file.valid())
            {
                char message[160];
                wsprintfA(
                    message,
                    "ccg: crash dump CreateFile failed "
                    "gle=%lu\r\n",
                    GetLastError());
                report(message);
                return;
            }
            // 异常参数（MINIDUMP_EXCEPTION_INFORMATION）刻意恒传
            // NULL：双构建实测传它时 MiniDumpWriteDump 返回成功却
            // 一个字节都不写（0 字节文件），NULL 则稳定产出完整
            // 转储；崩溃线程的现场本来就在它自己的栈里（full
            // memory 位把整条栈收进转储），离线阅读不损失信息。
            // 留档此结论，避免将来有人"补全"这个参数。
            const HRESULT hr = write(
                GetCurrentProcess(),
                GetCurrentProcessId(),
                file.get(),
                dump_type_full,
                nullptr,
                nullptr,
                nullptr);
            if (hr < 0)
            {
                char message[96];
                wsprintfA(
                    message,
                    "ccg: MiniDumpWriteDump failed "
                    "hr=0x%08lX\r\n",
                    static_cast<unsigned long>(hr));
                report(message);
            }
        }

        /**
         * @brief 常驻转储线程主体：醒来一次写一份，随即回执并驻眠。
         *
         * @internal 进程的终止永远由崩溃线程执行（保留原异常码
         * 语义），本线程随后被一并收割。
         */
        DWORD WINAPI dump_thread_main(LPVOID)
        {
            for (;;)
            {
                WaitForSingleObject(g_wake, INFINITE);
                write_dump();
                SetEvent(g_done);
            }
        }

        /**
         * @brief 崩溃线程侧统一入口：处理器栈需求被压到几个系统调用。
         *
         * @internal 首个到达者（原子闸）唤醒转储线程并等回执——30 秒
         * 上限防写盘卡死（网络路径等）留下僵尸进程；并发崩溃的后到者
         * 永久驻眠，由首位的进程终止一并收割（多线程同秒崩溃因此恰好
         * 产生一份转储）。异常现场不经此传递：崩溃线程的栈整体会被
         * 转储收走（见 write_dump 的留档结论）。
         */
        void trigger_dump_and_terminate(unsigned long exit_code)
        {
            if (InterlockedCompareExchange(&g_owner, 1, 0) == 0)
            {
                SetEvent(g_wake);
                WaitForSingleObject(g_done, 30000);
                TerminateProcess(GetCurrentProcess(), exit_code);
            }
            for (;;)
                Sleep(15000); // 后到者：驻眠等收割
        }

        /**
         * @brief 进程级未处理异常过滤器（主拦截层）。
         *
         * @internal SEH 二阶段分发——被内层防护（run_guarded 的
         * __try 帧、libclang 的 PARSE 恢复）吞掉的 first-chance 故障
         * 根本到不了这里，不误转储；到达即无人认领的真实崩溃，以
         * 原异常码终止。
         */
        LONG WINAPI on_unhandled_exception(EXCEPTION_POINTERS* pointers)
        {
            trigger_dump_and_terminate(
                pointers != nullptr && pointers->ExceptionRecord != nullptr
                    ? pointers->ExceptionRecord->ExceptionCode
                    : 1u);
            return EXCEPTION_EXECUTE_HANDLER; // 不可达（上方已终止）
        }

        /**
         * @brief abort 家族的唯一出口：abort()、非 MSVC 工具链上的
         *        纯虚调用与栈保护失败都汇入 SIGABRT。
         *
         * @internal 该路径无异常记录可传（转储仍含全部线程现场）；
         * 退出码 3 对齐 CRT abort 的惯例退出码。
         */
        void on_sigabrt(int) { trigger_dump_and_terminate(3); }

        /**
         * @brief terminate 语义点：未捕获 C++ 异常正常会先以
         *        0xE06D7363 到达进程级过滤器，这里是更浅栈的直接
         *        保险（双保险）。
         */
        void on_terminate() { trigger_dump_and_terminate(0xE06D7363u); }

#ifdef _MSC_VER

        /**
         * @brief 纯虚调用（MSVC 的快路径；非 MSVC 工具链走
         *        abort→SIGABRT）。
         */
        void __cdecl on_pure_virtual()
        {
            trigger_dump_and_terminate(0xE0000002u);
        }

        /**
         * @brief UCRT 非法参数：不挂处理器则默认走快速失败——
         *        按定界不可拦，挂上才转入可拦面。
         */
        void __cdecl on_invalid_parameter(
            const wchar_t*,
            const wchar_t*,
            const wchar_t*,
            unsigned,
            UINT_PTR)
        {
            trigger_dump_and_terminate(0xE0000003u);
        }

#endif // _MSC_VER

        /**
         * @brief 武装全部回调与转储线程（环境变量有值时的全部动作）。
         */
        void install()
        {
            wchar_t     dir[MAX_PATH];
            const DWORD len =
                GetEnvironmentVariableW(L"CCG_CRASH_DUMP_DIR", dir, MAX_PATH);
            // 空/未设置/超长：不武装任何回调——零配置零行为变化。
            if (len == 0 || len >= MAX_PATH)
                return;

            // dbghelp 预载 + 动态解析：崩溃现场再加载要过 loader
            // lock 且堆可能已坏——启动期做完，风险归零。模块句柄登记
            // 进进程级状态，按设计常驻不卸载。
            g_dbghelp = LoadLibraryA("dbghelp.dll");
            if (g_dbghelp != nullptr)
            {
                g_mini_dump = reinterpret_cast<MiniDumpWriteDumpFn>(
                    GetProcAddress(g_dbghelp, "MiniDumpWriteDump"));
            }

            // 主线程预留栈余量：栈溢出残栈上的处理器至少能完成
            // 事件通知那几个调用。
            ULONG reserve_kb = 256;
            SetThreadStackGuarantee(&reserve_kb);

            // g_wake 必须自动复位：转储线程醒来即消费信号，写盘后
            // 回到等待不会被残留的信号再唤醒（手动复位若全程不复位，
            // 恰一份语义就只剩崩溃线程 TerminateProcess 的赛跑赢面）。
            // 两个事件先由栈上的拥有型对象持有：只建成一个时，建成的
            // 那个随栈对象关闭，不再留下无人使用的事件。
            handle::Owned wake = handle::Owned::adopt(
                CreateEventW(nullptr, FALSE, FALSE, nullptr));
            handle::Owned done = handle::Owned::adopt(
                CreateEventW(nullptr, FALSE, FALSE, nullptr));
            if (!wake.valid() || !done.valid())
            {
                return; // 事件创建失败：挂载放弃（零行为）
            }

            // 转储线程一启动就读取模块级状态，所以先把两个事件句柄登记
            // 进去，再创建线程；此时所有权仍在栈对象手里。
            g_wake = wake.get();
            g_done = done.get();
            const handle::Owned thread = handle::Owned::adopt(CreateThread(
                nullptr,
                0,
                &dump_thread_main,
                nullptr,
                0,
                nullptr));
            if (!thread.valid())
            {
                // 转储线程缺席：收回登记，两个事件随栈对象关闭，不挂
                // 任何处理器。
                g_wake = nullptr;
                g_done = nullptr;
                return;
            }
            // 线程句柄随栈对象在函数返回时关闭。全部建成：两个事件的
            // 所有权显式交给模块级状态，此后按设计常驻。
            static_cast<void>(wake.release());
            static_cast<void>(done.release());

            SetUnhandledExceptionFilter(&on_unhandled_exception);
            signal(SIGABRT, &on_sigabrt);
            std::set_terminate(&on_terminate);
#ifdef _MSC_VER
            _set_purecall_handler(&on_pure_virtual);
            _set_invalid_parameter_handler(&on_invalid_parameter);
            // Debug 版 CRT 在非法参数校验进处理器之前先发 _CRT_ASSERT
            // 断言报告，其默认模式是模态消息框——无人值守即永久挂起，
            // 处理器永远等不到调用。改道调试器输出通道（非阻塞）后
            // 链路才真正闭合；Release 版 CRT 无此弹框路径。
            _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG);
            // 清 abort 行为位的两位，缺一不可：_CALL_REPORTFAULT 是
            // Watson 快速失败上报（不清则 abort 绕过 SIGABRT 处理器
            // 直达不可拦面）；_WRITE_ABORT_MSG 在 Debug 版 CRT 默认
            // 置位，驱动 abort 在进入信号处理器之前先弹无人值守即
            // 永久挂起的模态消息框（Release 版默认无此位）。两位同清
            // 后 abort 静默直入 raise(SIGABRT)，SIGABRT 处理器接管。
            _set_abort_behavior(0, _CALL_REPORTFAULT | _WRITE_ABORT_MSG);
#endif
        }

    } // namespace detail

    /**
     * @brief 读取 CCG_CRASH_DUMP_DIR，有值时武装全套崩溃回调。
     *
     * 设计为 main 的第一行调用：CRT 已就绪（std 可用）、loader lock
     * 已释放（可安全 LoadLibrary）、先于一切业务初始化。任何失败
     * （事件/线程创建不了、dbghelp 缺席）都只是少武装——绝不让
     * 启动失败。
     */
    void install_from_env() noexcept { detail::install(); }

#else // POSIX

    /**
     * @brief POSIX 上的空操作：崩溃走系统 core dump 的既有契约（见
     *        cg.util.guard 的 run_guarded 说明），本模块零行为。
     */
    void install_from_env() noexcept {}

#endif // platform

} // namespace cg::util::crashdump
