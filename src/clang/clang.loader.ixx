module;
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

/**
 * @module cg.clang.loader
 *
 * @brief LLVM 引擎库的运行时加载（按数据库解析的世界观）：
 * ccg 永不对 libclang 做链接期依赖。
 *
 * 本模块持有动态库加载的全部平台 API 与引擎 PATH 解析链；cg.clang.capi
 * 持有由本模块填充的 clang-c 符号表。项目中其他任何地方不得接触加载器
 * 平台 API。打开的库一律由 Library 对象持有，卸载只经 cg.util.handle 的
 * 动态库删除器发生；取库失败时系统分配的错误消息缓冲由带删除器的
 * 拥有型指针持有。
 *
 * @internal
 * 解析优先级（既定设计）：数据库的 llvm-path 配置，其次 $CCG_LLVM_ROOT，
 * 最后默认发现——PATH 上的 clang 驱动器，其工具链根提供
 * lib/libclang.`<ext>`（bin/ 布局下 dll 也位于驱动器旁；两处都探测）。
 * 显式根可以是库文件本身，或直接持有它、持于 bin/ 下、持于 lib/ 下的
 * 前缀。解析不到东西的根是可行动的硬错误，绝不静默落到另一个引擎。
 * 全局模块片段说明：Windows 用
 * LoadLibraryExW/GetProcAddress，POSIX 用 dlopen/dlsym；卸载库的调用
 * （FreeLibrary/dlclose）只出现在 cg.util.handle 的删除器里。两分支都已
 * 实现（跨平台要求），宏与 C 声明绝不穿越模块边界，故 include 置于全局
 * 模块片段。
 */
export module cg.clang.loader;

import std;
import cg.util;
import cg.util.cstd;

export namespace cg::clang::dylib
{

    /**
     * @brief 一个已打开的动态库句柄的拥有者：析构时卸载库。
     *
     * @internal
     * 只能移动，移动构造、移动赋值与析构都由成员（带删除器的拥有型指针）
     * 给出：被移动走的对象不持有库，析构不做任何事，所以三者都不抛。
     * 移动赋值先卸载自己原有的库再接管对方的，自己赋给自己保持原样。
     * 卸载库的平台调用只出现在通用句柄单元的删除器里，本类不直接调它。
     * 系统接口经出参交回库句柄时（按模块名取已装载库的句柄），先默认
     * 构造出空的拥有者，再把 release_and_get_address_of 交出的地址交给
     * 它。按设计要常驻到进程退出的库，在装载成功之后经 release 交出去
     * （见 cg.clang.capi.api 的引擎状态），此后本对象不再管它。
     */
    class Library
    {
    public:
        /** @brief 不持有库。 */
        Library() = default;

        Library(const Library&) = delete;
        auto operator=(const Library&) -> Library& = delete;
        Library(Library&&) noexcept = default;
        auto operator=(Library&&) noexcept -> Library& = default;
        ~Library() = default;

        /** @brief 接管一个已经打开的库；空指针表示没有库，对象保持为空。 */
        [[nodiscard]]
        static auto adopt(void* library) noexcept -> Library
        {
            return Library{Handle::adopt(library)};
        }

        /** @brief 库句柄的原生值，只供传给符号解析；不持有库时是空值。 */
        [[nodiscard]]
        auto get() const noexcept -> void*
        {
            return library_.get();
        }

        /**
         * @brief 交出所有权：返回库句柄的原生值，此后本对象不再持有它，
         *        析构也不会卸载它。
         */
        [[nodiscard]]
        auto release() noexcept -> void*
        {
            return library_.release();
        }

        /**
         * @brief 交出内部指针的地址，供系统接口把库句柄写进来；先卸载
         *        已持有的库。
         */
        [[nodiscard]]
        auto release_and_get_address_of() noexcept -> void**
        {
            return library_.release_and_get_address_of();
        }

    private:
        /** @brief 库句柄的拥有者；卸载库的调用只在它的删除器里。 */
        using Handle =
            cg::util::handle::OwnedPtr<void, cg::util::handle::ModuleDeleter>;

        explicit Library(Handle library) noexcept : library_{std::move(library)}
        {
        }

        Handle library_;
    };

    /**
     * @brief 打开 path 处的共享库。
     *
     * @internal
     * Windows 用 LOAD_WITH_ALTERED_SEARCH_PATH：引擎自身的目录解析其依赖
     * （引擎前缀绝不能静默绑定到另一工具链的 DLL）；POSIX 用 RTLD_NOW：
     * 缺失依赖在此处失败，而不是解析中途失败。失败时抛出携带平台错误
     * 文本的 std::runtime_error。
     */
    [[nodiscard]]
    auto open(const std::string& path) -> Library
    {
#if defined(_WIN32)
        const std::wstring wide = std::filesystem::path{path}.wstring();
        HMODULE            library = LoadLibraryExW(
            wide.c_str(),
            nullptr,
            LOAD_WITH_ALTERED_SEARCH_PATH);
        if (library == nullptr)
        {
            const DWORD err = GetLastError();
            namespace handle = cg::util::handle;
            handle::OwnedPtr<wchar_t, handle::LocalDeleter> message;
            // 系统分配的错误消息缓冲直接由拥有者接住：下面拼接错误文本
            // 的语句会分配内存，抛出时缓冲也随本作用域交还。
            const DWORD n = FormatMessageW(
                FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
                    | FORMAT_MESSAGE_IGNORE_INSERTS,
                nullptr,
                err,
                0,
                reinterpret_cast<LPWSTR>(&message),
                0,
                nullptr);
            std::string text = std::format("Windows error {}", err);
            if (n != 0 && message)
            {
                std::string decoded =
                    std::filesystem::path{message.get()}.string();
                while (!decoded.empty()
                       && (decoded.back() == '\r' || decoded.back() == '\n'
                           || decoded.back() == ' '))
                {
                    decoded.pop_back();
                }
                if (!decoded.empty())
                {
                    text = std::format("{} ({})", decoded, err);
                }
            }
            throw std::runtime_error(
                std::format("cannot load LLVM engine '{}': {}", path, text));
        }
        return Library::adopt(reinterpret_cast<void*>(library));
#else
        void* library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (library == nullptr)
        {
            const char* err = dlerror();
            throw std::runtime_error(
                std::format(
                    "cannot load LLVM engine '{}': {}",
                    path,
                    err != nullptr ? std::string{err}
                                   : std::string{"unknown"}));
        }
        return Library::adopt(library);
#endif
    }

    /** @brief 解析 library 里的一个符号；库未导出该符号时返回 nullptr。 */
    [[nodiscard]]
    auto symbol(const Library& library, const char* name) -> void*
    {
        void* handle = library.get();
#if defined(_WIN32)
        return reinterpret_cast<void*>(
            GetProcAddress(reinterpret_cast<HMODULE>(handle), name));
#else
        (void)dlerror(); // 清掉陈旧状态，使命中可辨别
        return dlsym(handle, name);
#endif
    }

} // namespace cg::clang::dylib

export namespace cg::clang::engine
{

    /** @brief 按平台参数化的引擎文件名与 PATH 约定。 */
    inline constexpr std::string_view library_file_name =
#if defined(_WIN32)
        "libclang.dll"
#else
        "libclang.so"
#endif
        ;

    inline constexpr char path_list_separator =
#if defined(_WIN32)
        ';'
#else
        ':'
#endif
        ;

    /** @brief 默认发现时逐 PATH 条目探测的驱动器可执行名。 */
    inline constexpr std::string_view discovery_driver_names[] = {
#if defined(_WIN32)
        "clang++.exe",
        "clang.exe"
#else
        "clang++",
        "clang"
#endif
    };

    /**
     * @brief 项目中一切引擎失败消息共用的可行动解析提示。
     *
     * @internal 覆盖加载器、分发表加载与 CLI 错误三处。
     */
    [[nodiscard]]
    auto resolution_hint() -> std::string_view
    {
        return "The engine is resolved per database, in order: the llvm-path "
               "config (`ccg config set llvm-path <prefix-or-libclang-file>`; "
               "`ccg config unset llvm-path` restores default resolution), "
               "then the CCG_LLVM_ROOT environment variable, then the clang "
               "toolchain found on PATH (the library is looked up beside the "
               "clang++/clang driver and under its root's lib/). One database "
               "is parsed by exactly one engine - a missing engine is never "
               "silently replaced by another.";
    }

    namespace detail
    {

        [[nodiscard]]
        auto normalized(const std::filesystem::path& p) -> std::string
        {
            return cg::util::norm_path(p.lexically_normal().generic_string());
        }

        /**
         * @brief 取 root 下的引擎库：root 本身是文件时即它，否则依次探测
         * root/`<library_file_name>`、root/bin/`<library_file_name>`、
         * root/lib/`<library_file_name>`；都不存在时返回 ""。
         */
        [[nodiscard]]
        auto library_under(const std::string& root) -> std::string
        {
            if (root.empty())
            {
                return {};
            }
            namespace fs = std::filesystem;
            std::error_code ec;
            const fs::path  base{root};
            if (fs::is_regular_file(base, ec))
            {
                return normalized(base);
            }
            for (const std::string_view sub : {"", "bin", "lib"})
            {
                const fs::path candidate = sub.empty()
                                               ? base / library_file_name
                                               : base / sub / library_file_name;
                if (fs::is_regular_file(candidate, ec))
                {
                    return normalized(candidate);
                }
            }
            return {};
        }

    } // namespace detail

    /**
     * @brief 纯净可测的解析核心。
     *
     * configured（数据库的 llvm-path，或代表它的会话覆盖）优先，其次
     * env_root（$CCG_LLVM_ROOT），最后对 path_env（$PATH）做发现。返回
     * 规范化的库路径；给定根解析不到或完全无根可解析时抛出携带可行动
     * 消息的 std::runtime_error。
     *
     * @internal
     * PATH 上的第一个 clang 驱动器就是唯一的工具链：其根没有引擎库时解析
     * 直接失败，而不尝试下一个 PATH 条目（确定性世界观，绝不静默换引擎）。
     * 默认发现的细节：PATH 条目合法地可以以分隔符或点结尾
     * （"`<toolchain>`/bin/"、"`<toolchain>`/bin/."），须先折叠——
     * parent_path() 会保留空的末尾元素，未修剪的 "`<toolchain>`/bin/" 会把
     * 工具链根推导成 "`<toolchain>`/bin" 自身，随后的驱动器旁探测会找进
     * bin/bin，漏掉合法的驱动器旁布局（与 library_under 的根处理不一致）。
     * 找到驱动器后，库位于驱动器旁（"" 条目）或工具链根的 bin/、lib/ 下。
     */
    [[nodiscard]]
    auto resolve_library_from(
        const std::string& configured,
        const std::string& env_root,
        const std::string& path_env) -> std::string
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        for (const std::string& root : {configured, env_root})
        {
            if (root.empty())
            {
                continue;
            }
            const std::string hit = detail::library_under(root);
            if (hit.empty())
            {
                throw std::runtime_error(
                    std::format(
                        "LLVM engine resolution failed for '{}': no {} at "
                        "'{}', '{}', '{}/bin', or '{}/lib'. {}",
                        root,
                        library_file_name,
                        root,
                        root,
                        root,
                        root,
                        resolution_hint()));
            }
            return hit;
        }

        std::string       entry;
        std::stringstream paths{path_env};
        while (std::getline(paths, entry, path_list_separator))
        {
            if (entry.empty())
            {
                continue;
            }
            fs::path dir{entry};
            {
                const fs::path folded = dir.lexically_normal();
                if (!folded.empty())
                {
                    dir = folded;
                }
                while (!dir.empty() && dir != dir.root_path()
                       && dir.filename().empty())
                {
                    dir = dir.parent_path();
                }
            }
            if (dir.empty())
            {
                continue;
            }
            for (const std::string_view name : discovery_driver_names)
            {
                const fs::path driver = dir / name;
                if (!fs::is_regular_file(driver, ec))
                {
                    continue;
                }
                const fs::path root = dir.parent_path();
                for (const std::string_view sub : {"", "bin", "lib"})
                {
                    const fs::path candidate =
                        sub.empty() ? dir / library_file_name
                                    : root / sub / library_file_name;
                    if (fs::is_regular_file(candidate, ec))
                    {
                        return detail::normalized(candidate);
                    }
                }
                throw std::runtime_error(
                    std::format(
                        "the clang driver '{}' found on PATH provides no {} "
                        "beside it or under '{}/bin' or '{}/lib'. {}",
                        driver.string(),
                        library_file_name,
                        root.string(),
                        root.string(),
                        resolution_hint()));
            }
        }
        std::string message =
            "no usable LLVM engine found (nothing configured and no clang++/"
            "clang on PATH). ";
        message += resolution_hint();
        throw std::runtime_error(message);
    }

    /** @brief 进程侧包装：实时读取 $CCG_LLVM_ROOT 与 $PATH。 */
    [[nodiscard]]
    auto resolve_library(const std::string& configured) -> std::string
    {
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
        return resolve_library_from(configured, env_root, path_env);
    }

} // namespace cg::clang::engine
