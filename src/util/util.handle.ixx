module;
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// objbase 提供最基的接口类型与任务内存的释放函数（接口指针删除器用）。
#include <objbase.h>
#else
#include <dirent.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

/**
 * @module cg.util.handle
 *
 * @brief 操作系统句柄的拥有型封装，以及供拥有型指针使用的删除器（系统
 *        缓冲、运行库堆内存、环境块、动态库句柄、共享内存视图、跨语言
 *        接口分配的内存、组件对象接口指针、目录流）。
 *
 * @internal
 * 本单元与 cg.util.platform 同为直接调用系统接口的封装单元：句柄的关闭
 * 调用只出现在拥有者的释放函数里，释放系统缓冲与运行库堆内存、卸载动
 * 态库、解除映射、放手接口指针的调用只出现在删除器的函数体里。其他单
 * 元取到句柄或缓冲后立即交到这里，业务代码不写释放调用。删除器只供带
 * 删除器的拥有型指针 OwnedPtr 使用，它保证空指针不调删除器，所以删除
 * 器里不判空。OwnedPtr 自成单元 cg.util.ownedptr，由本单元再导出。两
 * 个单元都只依赖标准库，宿主桥接、崩溃转储、引擎装载三个单元可以直接
 * 导入本单元，不必导入整个平台模块。拥有型句柄与拥有型指针都不分配堆
 * 内存，可以放在钩子与子进程派生的路径上。非 Windows 平台以文件描述符
 * 做等价封装，负值表示接口没有交出描述符。
 */
export module cg.util.handle;

import std;
// 带删除器的拥有型指针自成单元；在这里再导出，导入句柄单元的单元同时
// 取得到删除器与持有它们的拥有型指针。
export import cg.util.ownedptr;

export namespace cg::util::handle
{

#if defined(_WIN32)
    /// 平台的原生句柄值：Windows 上是内核句柄。
    using Native = HANDLE;
#else
    /// 平台的原生句柄值：文件描述符。
    using Native = int;
#endif

    namespace detail
    {

#if defined(_WIN32)
        /** @brief 不持有句柄时成员的值。 */
        [[nodiscard]]
        constexpr auto empty() noexcept -> Native
        {
            return nullptr;
        }

        /** @brief 值是一个打开着的句柄（空指针不是）。 */
        [[nodiscard]]
        constexpr auto holds(Native handle) noexcept -> bool
        {
            return handle != nullptr;
        }

        /** @brief 值是创建文件一类接口交回的失败值。 */
        [[nodiscard]]
        constexpr auto failed_file(Native handle) noexcept -> bool
        {
            return handle == INVALID_HANDLE_VALUE;
        }
#else
        /** @brief 不持有句柄时成员的值。 */
        [[nodiscard]]
        constexpr auto empty() noexcept -> Native
        {
            return -1;
        }

        /** @brief 值是一个打开着的文件描述符（负值不是）。 */
        [[nodiscard]]
        constexpr auto holds(Native handle) noexcept -> bool
        {
            return handle >= 0;
        }

        /** @brief 创建文件一类接口交回的失败值同样是负数，已被上一条
         *          判定覆盖；这里恒为否。 */
        [[nodiscard]]
        constexpr auto failed_file(Native /*handle*/) noexcept -> bool
        {
            return false;
        }
#endif

    } // namespace detail

    /**
     * @brief 一个操作系统句柄的拥有者：析构时关闭句柄。
     *
     * @internal
     * 只能移动。默认构造的与被移动走的对象都不持有句柄，对它们调用释放
     * 函数不做任何事，所以析构、移动构造与移动赋值都不抛。移动赋值先关
     * 掉自己原有的句柄再接管对方的，自己赋给自己保持原样。关闭句柄的
     * 调用只在 reset 一处，析构与移动赋值都经它。
     */
    class Owned
    {
    public:
        /** @brief 不持有句柄。 */
        Owned() = default;

        Owned(const Owned&) = delete;
        auto operator=(const Owned&) -> Owned& = delete;
        Owned(Owned&& other) noexcept;
        auto operator=(Owned&& other) noexcept -> Owned&;
        ~Owned() { reset(); }

        /**
         * @brief 接管一个句柄；空指针表示接口没有交出句柄，对象保持为空。
         *
         * @internal
         * 创建进程、管道、事件、线程、映射一类接口失败时返回空指针。
         */
        [[nodiscard]]
        static auto adopt(Native handle) noexcept -> Owned;

        /**
         * @brief 接管一个创建文件得来的句柄；无效值句柄表示接口没有交出
         *        句柄，对象保持为空。
         *
         * @internal
         * 创建文件、打开文件、进程快照一类接口失败时返回的不是空指针而
         * 是无效值句柄。两个接管函数只差失败值的判定；无效值句柄交给
         * adopt 会被当成真句柄，析构时去关一个从来没打开过的句柄。
         */
        [[nodiscard]]
        static auto adopt_file(Native handle) noexcept -> Owned;

        /** @brief 是否持有句柄。 */
        [[nodiscard]]
        auto valid() const noexcept -> bool;

        /** @brief 句柄的原生值，只供传给系统调用；不持有句柄时是空值。 */
        [[nodiscard]]
        auto get() const noexcept -> Native;

        /** @brief 立即关闭句柄，对象变为空；本来就不持有则不做任何事。 */
        auto reset() noexcept -> void;

        /**
         * @brief 交出所有权：返回句柄的原生值，此后本对象不再持有它，
         *        析构也不会关闭它。
         */
        [[nodiscard]]
        auto release() noexcept -> Native;

    private:
        /** @brief 接管一个已经判定过失败值的句柄。 */
        explicit Owned(Native handle) noexcept : handle_(handle) {}

        Native handle_ = detail::empty();
    };

    Owned::Owned(Owned&& other) noexcept
        : handle_{std::exchange(other.handle_, detail::empty())}
    {
    }

    auto Owned::operator=(Owned&& other) noexcept -> Owned&
    {
        if (this != &other)
        {
            reset();
            handle_ = std::exchange(other.handle_, detail::empty());
        }
        return *this;
    }

    auto Owned::adopt(Native handle) noexcept -> Owned
    {
        return Owned{detail::holds(handle) ? handle : detail::empty()};
    }

    auto Owned::adopt_file(Native handle) noexcept -> Owned
    {
        const bool usable =
            detail::holds(handle) && !detail::failed_file(handle);
        return Owned{usable ? handle : detail::empty()};
    }

    auto Owned::valid() const noexcept -> bool
    {
        return detail::holds(handle_);
    }

    auto Owned::get() const noexcept -> Native { return handle_; }

    auto Owned::reset() noexcept -> void
    {
        if (!detail::holds(handle_))
        {
            return;
        }
#if defined(_WIN32)
        static_cast<void>(::CloseHandle(handle_));
#else
        static_cast<void>(::close(handle_));
#endif
        handle_ = detail::empty();
    }

    auto Owned::release() noexcept -> Native
    {
        return std::exchange(handle_, detail::empty());
    }

    /**
     * @brief 运行库堆内存的删除器，供拥有型指针持有。
     *
     * @internal
     * 运行库交出来的缓冲（微软运行库读取环境变量的安全变体一类）要经
     * 运行库的释放函数交还。
     */
    struct HeapDeleter
    {
        auto operator()(void* memory) const noexcept -> void
        {
            std::free(memory);
        }
    };

    /**
     * @brief 动态库句柄的删除器，供拥有型指针持有。
     *
     * @internal
     * 装载动态库得到的模块句柄，交还时卸载库。按设计要常驻到进程退出的
     * 库不归它管：那种库在装载成功后交出所有权，登记进进程级状态。
     */
    struct ModuleDeleter
    {
        auto operator()(void* module) const noexcept -> void
        {
#if defined(_WIN32)
            static_cast<void>(::FreeLibrary(static_cast<HMODULE>(module)));
#else
            static_cast<void>(::dlclose(module));
#endif
        }
    };

    /**
     * @brief 映射视图的删除器，供拥有型指针持有。
     *
     * @internal
     * 映射文件得到的视图，交还时解除映射。非 Windows 分支解除映射的调
     * 用要带上映射长度，长度记在删除器里，构造时必须给出：默认的零会
     * 让解除映射因参数非法而失败，映射一直留到进程退出。
     */
    struct ViewDeleter
    {
#if defined(_WIN32)
        auto operator()(void* view) const noexcept -> void
        {
            static_cast<void>(::UnmapViewOfFile(view));
        }
#else
        std::size_t size = 0; ///< 映射的长度（字节）

        auto operator()(void* view) const noexcept -> void
        {
            static_cast<void>(::munmap(view, size));
        }
#endif
    };

#if defined(_WIN32)

    /**
     * @brief 系统分配的内存块的删除器，供拥有型指针持有。
     *
     * @internal 命令行参数数组一类由系统分配、要经本地内存释放函数交还
     *           的块。
     */
    struct LocalDeleter
    {
        auto operator()(void* memory) const noexcept -> void
        {
            static_cast<void>(::LocalFree(memory));
        }
    };

    /**
     * @brief 环境块的删除器，供拥有型指针持有。
     *
     * @internal 环境块由系统按进程环境复制出来，交还时走专门的释放函数。
     */
    struct EnvBlockDeleter
    {
        auto operator()(wchar_t* block) const noexcept -> void
        {
            static_cast<void>(::FreeEnvironmentStringsW(block));
        }
    };

    /**
     * @brief 跨语言接口分配的内存的删除器，供拥有型指针持有。
     *
     * @internal
     * 组件对象一类的接口经出参交回的内存块（调用方负责释放的那种），
     * 交还时走任务内存的释放函数。
     */
    struct CoTaskMemDeleter
    {
        auto operator()(void* memory) const noexcept -> void
        {
            ::CoTaskMemFree(memory);
        }
    };

    /**
     * @brief 组件对象接口指针的删除器，供拥有型指针持有。
     *
     * @internal
     * 经接口工厂取得的接口指针归调用方持有，放手时给它的引用计数减一。
     * 形参取最基的接口类型，派生接口的指针可以直接传进来。
     */
    struct ComReleaser
    {
        auto operator()(IUnknown* interface_pointer) const noexcept -> void
        {
            static_cast<void>(interface_pointer->Release());
        }
    };

#else

    /**
     * @brief 目录流的删除器，供拥有型指针持有。
     *
     * @internal 打开目录交出的目录流要经关闭目录的调用交还。
     */
    struct DirDeleter
    {
        auto operator()(DIR* dir) const noexcept -> void
        {
            static_cast<void>(::closedir(dir));
        }
    };

#endif

} // namespace cg::util::handle
