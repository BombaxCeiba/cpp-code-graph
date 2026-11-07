module;
#if defined(_WIN32)
#include <windows.h>
#else
#include <csetjmp>
#include <csignal>
#endif

/**
 * @module cg.util.guard
 *
 * @brief 硬件故障崩溃防护：Windows 走 SEH（本单元内的 __try），POSIX 走
 *        sigsetjmp/siglongjmp——崩溃防护家族（本模块与 cg.util.crashdump）
 *        是全工程仅有的两处触碰结构化异常或信号机制的地方。与
 *        cg.util.platform 并置（而非并入）以保持 OS 探针单元与故障
 *        捕获单元分别可审。
 *
 * @internal
 * 防护对象：外部加载的原生代码内的故障——实践中是动态加载的 libclang
 * 引擎，其 visit 阶段入口会在 C API 无法预先筛除的输入上解引用空指针
 * （依赖型成员指针形态，即 cg.clang 中声明类型遍历的 MemberPointer
 * 分支）。libclang 自身的崩溃恢复只武装 PARSE 阶段；引擎内部没有任何
 * 东西覆盖游标遍历，故 ccg 必须从外部设栏。
 * 防护命中的代价：从硬件故障恢复会跳过防护点与故障点之间每一帧的析构
 * 函数（SEH 展开与 siglongjmp 都不运行 C++ 析构）。调用方因此只能在
 * 防护下运行叶子形态的工作——单次引擎调用，或整段可整体丢弃其部分状态
 * 的抽取——绝不能是进行中对象必须被干净析构的工作。
 * 平台头的落点：崩溃防护所需的平台头遵循跨平台规则——每项关注恰一个
 * 守卫单元、两个分支皆实现；宏与 C 声明永不跨
 * 模块边界，故 include 住在全局模块片段。Windows 在此需要 windows.h：
 * SEH 帧（__try 拼法）就在本模块单元内编译（使该做法安全的语言选项
 * 处理记于 guarded_invoke 的说明）。
 */
export module cg.util.guard;

import std;

export namespace cg::util::guard
{

    namespace detail
    {

#if defined(_WIN32)

        /**
         * @brief SEH 帧的异常过滤器：只吞硬件故障。
         *
         * C++ 异常（ccg 全部目标 ABI 上错误码均为 0xE06D7363）返回
         * EXCEPTION_CONTINUE_SEARCH，正常抛掷/展开语义原样保留——被防护调用内的
         * std::bad_alloc 必须传播出去，不能被读成引擎崩溃。
         *
         * @internal
         * 列入的故障形态：EXCEPTION_ACCESS_VIOLATION 对应引擎空指针解引用形态；
         * EXCEPTION_IN_PAGE_ERROR 对应后备存储（页被换出）故障。
         * 0xC0000096（STATUS_PRIVILEGED_INSTRUCTION）采用数值拼法：MinGW 的
         * winnt.h 不定义该宏（MSVC 的定义），而本单元须在两套工具链上都能编译。
         */
        int fault_filter(unsigned long code)
        {
            switch (code)
            {
            case EXCEPTION_ACCESS_VIOLATION:
            case EXCEPTION_IN_PAGE_ERROR:
            case EXCEPTION_DATATYPE_MISALIGNMENT:
            case EXCEPTION_ILLEGAL_INSTRUCTION:
            case 0xC0000096UL:
                return EXCEPTION_EXECUTE_HANDLER;
            default:
                return EXCEPTION_CONTINUE_SEARCH;
            }
        }

        /**
         * @brief 在 SEH 帧内调用 fn；返回 0 表示完整运行，1 表示发生被吞掉的
         *        硬件故障。
         *
         * @internal
         * SEH 帧就在本模块单元内（经典 TU 桥接形态已退役）。__try 的 MS 拼法在
         * gnu-ABI 的 clang 工具链上需要 -fms-extensions，而语言选项是 C++20
         * 模块 pcm 兼容性标识的一部分——所以该标志必须搭载构建的每个模块目标
         * （含 CMake 合成的 std 模块），不容任何带标志/无标志的分裂
         * （cmake/CcgUtils.cmake 在非 MSVC 的 Windows 上按目录注入；MSVC 原生
         * 编译 __try）。相比逐源注入，两个保障使此形态安全：构建保持统一——
         * 标志全局生效，std.cppm 与所有消费者都在其下编译；对这样构建出的树的
         * 索引保持诚实——ccg 的 compile-db 路线按记录的语言选项向量分组
         * （cg.clang.bmi 的 pcm_langopt_vector），并为每组构建匹配的 std pcm
         * 变体。本函数必须不含带析构函数的局部变量：含 __try 的函数不得要求
         * 对象展开（C2712），且故障恢复跳过析构的代价只能落在被调用帧，绝不
         * 落在本帧。C 函数指针 + void* 蹦床使该性质在签名上直接可见。
         */
        int guarded_invoke(void (*fn)(void*), void* arg)
        {
            __try
            {
                fn(arg);
                return 0;
            }
            __except (fault_filter(GetExceptionCode()))
            {
                return 1;
            }
        }

#else // POSIX

        /**
         * @brief 本线程当前活动防护的恢复点。
         *
         * @internal
         * SIGSEGV/SIGBUS 交付给出故障的线程，故每线程槽位正是正确的汇合点：
         * 处理程序运行在武装防护的同一线程上。取命名空间作用域的 thread_local
         * （而非函数局部隐藏状态），因为处理程序必须能在没有调用上下文时
         * 到达它。
         */
        inline thread_local sigjmp_buf* t_active_guard = nullptr;

        /**
         * @brief 处理程序一次性进程级安装的标记。
         *
         * @internal 首次武装防护者赢得 compare_exchange；sigaction
         * 是进程全局的， 竞争的安装者收敛到同一处理程序。
         */
        inline std::atomic<bool> t_handler_installed{false};

        /**
         * @brief SIGSEGV/SIGBUS 的处理入口：恢复武装线程，或如实重抛。
         *
         * @internal
         * 存在活动防护时：siglongjmp 在武装时的 sigsetjmp 点恢复本线程。它是
         * 信号处理程序中被认可的非局部退出（恢复武装时保存的掩码，故被阻塞的
         * 故障信号随之解除阻塞）。防护点与故障点之间的 C++ 析构被跳过——这是
         * run_guarded 的成文契约。
         * 本线程无活动防护时：恢复默认处置并重新 raise，让进程死在真实故障点
         * （诚实崩溃，保留 core dump），而不是被静默吞掉。
         */
        void fault_handler(int sig)
        {
            if (t_active_guard != nullptr)
            {
                siglongjmp(*t_active_guard, 1);
            }
            ::signal(sig, SIG_DFL);
            ::raise(sig);
        }

        void install_fault_handler()
        {
            bool expected = false;
            if (!t_handler_installed.compare_exchange_strong(expected, true))
            {
                return;
            }
            struct sigaction sa;
            sigemptyset(&sa.sa_mask);
            sa.sa_handler = &fault_handler;
            sa.sa_flags =
                0; // 不设 SA_RESETHAND：处理程序须在自身被使用后仍存活
            ::sigaction(SIGSEGV, &sa, nullptr);
            ::sigaction(SIGBUS, &sa, nullptr);
        }

        /**
         * @brief guarded_invoke 的 POSIX 孪生：sigsetjmp/siglongjmp 栅栏。
         *
         * @return 0 表示 fn 完整运行，1 表示恢复自一次硬件故障。
         *
         * @internal
         * 嵌套：保存先前武装的防护，并在两条退出路径上都恢复。本平台的防护会
         * 嵌套（外层整次抽取防护内含内层单调用防护）；不恢复的话，第一个离场的
         * 内层防护——无论正常返回还是经 siglongjmp——都会把 t_active_guard 停在
         * nullptr，静默解除本线程的所有外围防护：外层区域随后的故障会错过其
         * 恢复点、以默认处置重新 raise。Windows 孪生无需这般照看——__try 帧
         * 自然展开。
         * sigsetjmp 必须是恰被求值一次的完整表达式的右侧（可移植用法规则）；
         * savemask=1 使配对的 siglongjmp 恢复信号掩码。
         * fn 经 C++ 异常离开时同样先恢复 t_active_guard 再重抛（见函数体）：
         * 重抛不改变"异常原样穿过防护帧"的契约，但指针不恢复会让它悬挂指向
         * 本栈帧失效的 resume——此后本线程任何故障都 siglongjmp 到野地址。
         */
        int guarded_invoke(void (*fn)(void*), void* arg)
        {
            install_fault_handler();
            sigjmp_buf  resume;
            sigjmp_buf* prev = t_active_guard;
            t_active_guard = &resume;
            const int arrived = sigsetjmp(resume, 1);
            if (arrived == 0)
            {
                try
                {
                    fn(arg);
                }
                catch (...)
                {
                    // 异常不归防护管（原样重抛），但恢复先于重抛：否则
                    // t_active_guard 悬挂、嵌套防护的 prev 链同失。
                    t_active_guard = prev;
                    throw;
                }
                t_active_guard = prev;
                return 0;
            }
            t_active_guard = prev;
            return 1;
        }

#endif // platform

    } // namespace detail

    /**
     * @brief 在崩溃防护下运行 fn。
     *
     * @return fn 完整运行返回 true；被防护区域发生硬件故障返回 false（故障
     *         已被吞——该调用正在构建的任何状态都须视为不确定并整体丢弃）。
     *         fn 自身抛出的异常不在此捕获：它们原样穿过防护帧展开（只有
     *         硬件故障归防护管）；POSIX 分支在重抛前恢复线程局部防护指针
     *         （Windows 的 __try 帧自然展开，无此状态）。
     *
     * @internal
     * 可重用性：故障过后防护机制保持武装且可用——后续被防护调用独立运行
     * （且可能再次故障）；在已知引擎故障形态上，引擎本身也会继续服务后续
     * 调用（针对依赖型成员指针的类类型查询验证过：一次被防护故障之后，
     * 下一个非依赖查询正常应答）。
     * AddressSanitizer 注记：ASAN 插桩的进程默认安装自己的 SEGV 拦截
     * （handle_segv=1），会在平台防护见到原生故障之前报告并终止。Release
     * 构建不受影响；ASAN 构建下测试防护路径本身时须设
     * ASAN_OPTIONS=handle_segv=0。ASAN 自身的内存错误报告与本防护无关，
     * 照常触发。
     */
    template <typename F>
    [[nodiscard]]
    bool run_guarded(F&& fn)
    {
        using Fn = std::remove_reference_t<F>;
        auto trampoline = +[](void* p) { (*static_cast<Fn*>(p))(); };
        return detail::guarded_invoke(trampoline, &fn) == 0;
    }

} // namespace cg::util::guard
