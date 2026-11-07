module;
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifdef _MSC_VER
#include <stdio.h>
#endif
#include <cstdio> // stderr 是宏：import std 不传递宏，须在全局片段取得
#endif

/**
 * @module cg.test.crashprobe
 *
 * @brief ccg_crashdump_matrix 闸门的崩溃形态探针：启动第一行挂载
 *        cg_core 里真实的 cg.util.crashdump（与 ccg 的 main 完全同
 *        形），随后按命令行形态参数触发一次致命故障。
 *
 * @internal
 * 形态集：av（野指针写）/ so（无限递归）/ abort / terminate（未捕获
 * 异常）/ purecall（构造期纯虚调用）/ heapcor（堆破坏异常码模拟）/
 * multiav（两工作线程加主线程同秒崩溃——并发单写者闸的用例）/
 * fastfail（快速失败——断言不可拦的负向形态）。MSVC 另有 invarg
 * （UCRT 非法参数）。进程以 0 正常返回只发生在形态未被触发时（驱动
 * 脚本把它判失败）。本探针是测试数据面：崩溃点之前允许 CRT——
 * 崩溃态写盘纪律只约束崩溃后的路径，那部分全部住在
 * cg.util.crashdump 里。平台头与 MSVC 扩展头住在全局模块片段，
 * 宏不跨模块边界。
 */
export module cg.test.crashprobe;

import std;
import cg.util.crashdump;

namespace
{

    volatile int g_recursion_depth = 0;

    // 无穷递归是栈溢出形态的刻意构造：clang 的 -Winfinite-recursion
    // 与 MSVC 的 C4717 在此同属静态分析误报（探针要的就是耗尽栈），
    // 双编译器各自压制，构建须保持零警告。
#if defined(_MSC_VER)
#pragma warning(disable : 4717)
#endif
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winfinite-recursion"
#endif
    int recurse_to_overflow()
    {
        // volatile 数组 + 调用后的真实读取：Release 优化器（尾调用
        // 消除与死存储消除）绝无机会把递归折成不耗栈的循环。
        volatile char pad[4096];
        pad[0] = static_cast<char>(g_recursion_depth);
        g_recursion_depth = g_recursion_depth + 1;
        recurse_to_overflow();
        return pad[0];
    }
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

    struct PureBase
    {
        PureBase();
        virtual void hook() = 0;
        void poke() { hook(); }
    };
    // 此刻 vptr 仍指向基类——hook 槽位是纯虚桩
    PureBase::PureBase() { poke(); }
    struct PureDerived : PureBase
    {
        void hook() override {}
    };

    DWORD WINAPI crash_worker(LPVOID)
    {
        volatile int* target =
            reinterpret_cast<volatile int*>(0x5678);
        *target = 1;
        return 0;
    }

}

// NOLINTNEXTLINE(clang-diagnostic-main-attached-to-named-module) module-unit 的
// main 是刻意为之：与 app.main 同策略（工程每个源文件都是命名模块单元）
export int main(int argc, char* const* argv)
{
    if (argc < 2)
    {
        std::fputs("usage: ccg_crashprobe "
                   "<av|so|abort|terminate|purecall|heapcor|multiav|"
                   "fastfail|invarg>\n",
                   stderr);
        return 2;
    }
    cg::util::crashdump::install_from_env();
    const std::string_view form = argv[1];

    if (form == "multiav")
    {
        HANDLE workers[2] = {
            CreateThread(nullptr, 0, &crash_worker, nullptr, 0,
                         nullptr),
            CreateThread(nullptr, 0, &crash_worker, nullptr, 0,
                         nullptr)};
        Sleep(50);
        volatile int* target =
            reinterpret_cast<volatile int*>(0x1234);
        *target = 42;
        WaitForMultipleObjects(2, workers, TRUE, 5000);
    }
    else if (form == "heapcor")
    {
        RaiseException(0xC0000374L, EXCEPTION_NONCONTINUABLE, 0,
                       nullptr);
    }
    else if (form == "av")
    {
        volatile int* target =
            reinterpret_cast<volatile int*>(0x1234);
        *target = 42;
    }
    else if (form == "so")
    {
        recurse_to_overflow();
    }
    else if (form == "abort")
    {
        std::abort();
    }
    else if (form == "terminate")
    {
        throw 42;
    }
    else if (form == "purecall")
    {
        PureDerived derived;
        (void)derived;
    }
    else if (form == "fastfail")
    {
        RaiseFailFastException(
            nullptr, nullptr, FAIL_FAST_GENERATE_EXCEPTION_ADDRESS);
    }
    else if (form == "invarg")
    {
#ifdef _MSC_VER
        char buffer[8];
        _snprintf_s(buffer, 0, 8, "%s", "way too long");
#endif
    }
    else
    {
        std::fputs("ccg_crashprobe: unknown form\n", stderr);
        return 2;
    }
    std::fputs("ccg_crashprobe: form returned without crashing\n",
               stderr);
    return 0;
}
