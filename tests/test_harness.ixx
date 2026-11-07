module;
// 环境变量写原语（_putenv_s 非标准 C，import std 不保证再导出）。
// filesystem/fstream/system_error 一律经 import std 取用：此处绝不
// 直接 include <filesystem>——项目的 C2801 垫片头会把经典定义与
// std 模块的定义混进同一单元（_Path_iterator 私有成员冲突实测）。
#include <cstdlib>
#if defined(_MSC_VER) && defined(_DEBUG)
// Debug CRT 的报告模式开关（仅 MSVC Debug 生效）：断言/abort 的默认
// 目的地是桌面弹窗，无人值守的批量测试会被模态框卡死。
#include <crtdbg.h>
#endif

export module cg.test.harness;

// 极简断言/注册式 harness（许可的第三方集合里没有外部测试
// 框架）。宏无法跨越模块边界，所以原草稿里的 CG_CHECK 宏换成
// 了 check()，用 std::source_location 默认实参携带位置——
// 信息等价，且模块纯净。

import std;

export namespace cg::test
{

    // 每个用例的累加器。
    struct Result
    {
        std::size_t              checks = 0;
        std::size_t              failed = 0;
        std::vector<std::string> failures;
    };

    using CaseFn = void (*)(Result&);

    struct Case
    {
        std::string name;
        CaseFn      fn;
    };

    // 函数局部静态：下面的 Registrar 对象在静态初始化期、
    // main 运行之前填充它。
    inline auto cases() -> std::vector<Case>&
    {
        static std::vector<Case> registry;
        return registry;
    }

    // 记录一次检查；调用点的 file:line 默认自动捕获。
    inline void check(
        Result&              r,
        bool                 cond,
        std::string_view     what,
        std::source_location loc = std::source_location::current())
    {
        ++r.checks;
        if (!cond)
        {
            ++r.failed;
            r.failures.push_back(
                std::format("{}:{}: {}", loc.file_name(), loc.line(), what));
        }
    }

    // 在静态初始化期自注册一个测试用例。
    struct Registrar
    {
        explicit Registrar(std::string_view name, CaseFn fn)
        {
            cases().push_back({.name = std::string(name), .fn = fn});
        }
    };

    /**
     * @brief 环境变量的作用域级保存/恢复夹具。
     *
     * @internal 构造时记下当前值（含"未设置"这一形态）；析构时恢复原值，
     * 原本不存在的变量按删除恢复（CRT 置空值即删除该变量），绝不用清空
     * 整个环境块的方式恢复——那会清掉用户配置。中途 return 也由析构兜底，
     * 不污染后续用例。写侧原语沿用既有用例的双平台写法（MSVC 的
     * _putenv_s / POSIX 的 setenv 与 unsetenv）。
     */
    struct EnvScope
    {
        std::string                name;
        std::optional<std::string> saved;

        explicit EnvScope(const char* var_name) : name(var_name)
        {
            if (const char* raw = std::getenv(var_name); raw != nullptr)
            {
                saved = std::string{raw};
            }
        }
        EnvScope(const EnvScope&) = delete;
        auto operator=(const EnvScope&) -> EnvScope& = delete;

        /** @brief 在作用域内设新值（随后析构仍恢复构造时的原值）。 */
        void set(std::string_view value) const
        {
#if defined(_WIN32)
            _putenv_s(name.c_str(), std::string{value}.c_str());
#else
            ::setenv(name.c_str(), std::string{value}.c_str(), 1);
#endif
        }

        /** @brief 立即恢复构造时的原值（中途复位用；析构再调一次幂等）。 */
        void restore() const
        {
#if defined(_WIN32)
            _putenv_s(name.c_str(), saved ? saved->c_str() : "");
#else
            if (saved)
            {
                ::setenv(name.c_str(), saved->c_str(), 1);
            }
            else
            {
                ::unsetenv(name.c_str());
            }
#endif
        }

        ~EnvScope() { restore(); }
    };

    /**
     * @brief 一次性临时目录夹具：构造即清旧建新，析构整树删除。
     *
     * @internal 用例样板的三件套（remove_all 旧残留 / create_directories /
     * 末尾 remove_all，含各早退路径的重复清理）收口为 RAII——中途 return
     * 由析构兜底。put 会建父目录（子目录文件可直接写）。ec 暴露为成员，
     * 供用例做 exists 探测等只读操作复用。
     */
    struct ScratchDir
    {
        std::filesystem::path   base;
        mutable std::error_code ec;

        explicit ScratchDir(std::filesystem::path dir) : base(std::move(dir))
        {
            std::filesystem::remove_all(base, ec);
            std::filesystem::create_directories(base, ec);
        }
        ScratchDir(const ScratchDir&) = delete;
        auto operator=(const ScratchDir&) -> ScratchDir& = delete;
        ~ScratchDir()
        {
            std::error_code ignored;
            std::filesystem::remove_all(base, ignored);
        }

        /** @brief 写文本文件（父目录不存在则先建）。 */
        void put(const std::filesystem::path& p, std::string_view text) const
        {
            if (p.has_parent_path())
            {
                std::filesystem::create_directories(p.parent_path(), ec);
            }
            std::ofstream out{p, std::ios::binary};
            out << text;
        }
    };

    /**
     * @brief 删除一个 SQLite 数据库的三件套：主库文件与同名 -wal/-shm
     *        侧车文件。
     *
     * @internal 各用例开档前的清场与收尾清理原先手写三份 remove；收纳为
     * 单点保证三件永远同删——漏删 -wal/-shm 会让下一个用例打开旧库时读到
     * 残留日志页。逐件以错误码形式忽略失败（文件本就不存在是常态，不是
     * 错误），侧车名按主库路径字符串拼接（与 SQLite 自身的伴生命名一致）。
     */
    inline void remove_sqlite_db(const std::filesystem::path& db_path)
    {
        std::error_code ec;
        const std::string p{db_path.string()};
        std::filesystem::remove(db_path, ec);
        std::filesystem::remove(p + "-wal", ec);
        std::filesystem::remove(p + "-shm", ec);
    }

    // 运行所有已注册用例，打印摘要，返回进程退出码
    // （仅当没有任何用例失败时为 0）。注册表为空要大声报错而非全绿：
    // 那几乎必然意味着注册环节坏了。
    inline auto run_all() -> int
    {
#if defined(_MSC_VER) && defined(_DEBUG)
        // Debug CRT 报告改走调试器通道（stderr 可捕获），abort 不再召唤
        // Watson/弹窗：失败以非零退出码与控制台文本呈现，批处理与
        // ctest 不会被模态框卡死。Release 构建本无此弹窗，零改动。
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG);
        _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_DEBUG);
        _set_abort_behavior(0, _CALL_REPORTFAULT);
#endif
        if (cases().empty())
        {
            std::println(std::cerr, "cg.test: no cases registered");
            return 1;
        }
        std::size_t failed_cases = 0;
        std::size_t total_checks = 0;
        std::size_t total_failures = 0;
        for (const Case& c : cases())
        {
            Result r;
            c.fn(r);
            total_checks += r.checks;
            total_failures += r.failed;
            const bool ok = r.failed == 0;
            if (!ok)
            {
                ++failed_cases;
            }
            std::println(
                "[{}] {} ({} checks, {} failed)",
                ok ? "PASS" : "FAIL",
                c.name,
                r.checks,
                r.failed);
            for (const std::string& f : r.failures)
            {
                std::println("       {}", f);
            }
        }
        std::println(
            "harness: {} case(s), {} failed case(s), {} check(s), {} failure(s)",
            cases().size(),
            failed_cases,
            total_checks,
            total_failures);
        return failed_cases == 0 ? 0 : 1;
    }

} // namespace cg::test
