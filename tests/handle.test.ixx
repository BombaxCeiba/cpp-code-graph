module;
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
// 只为两条查询接口（枚举连接上的语句，证明没有遗留；生成一段库分配的
// 文本，交给拥有型对象）、连接类型名、打开旗标与结果码宏。测试里不出现
// 任何释放调用。
#include <sqlite3.h>

/**
 * @module cg.test.handle
 *
 * @brief C 接口资源拥有者的测试：操作系统句柄的拥有型封装，数据库原生
 *        预编译语句、库分配文本与执行助手，平台层取命令行参数与环境
 *        条目的两个函数，以及平台封装单元里取到句柄就交给拥有者的各条
 *        路径（子进程派生、文件锁、只读映射、原子写、崩溃转储的武装）。
 *
 * @internal
 * 两类证据。一类是不变量：接管、移动、交出、复位之后对象是否持有、句柄
 * 值是否还能查到；持有文件锁时锁文件在不在、移动之后归谁。另一类是
 * 资源计数探针：构造、移动、析构、栈展开的前后，本进程的句柄数、线程
 * 数、子进程数回到基线；平台封装单元的每条派生与文件路径走完一轮之
 * 后同样如此。取资源与查状态的调用只在测试的辅助函数里出现，释放一
 * 律经拥有者。崩溃转储的武装会挂上进程级处理器，所以放进本测试程序
 * 的子进程里做，由父进程读回它打印的计数。
 */
export module cg.test.handle;

import std;
import cg.util.handle;
import cg.util.platform;
import cg.util.crashdump;
import cg.db.native;
import sqlpp23.sqlite3;
import cg.test.harness;

namespace
{

    using R = cg::test::Result;
    namespace hd = cg::util::handle;
    namespace plat = cg::util::platform;
    namespace dbn = cg::db;

    /** @brief 构造即抛的哨兵：用来触发栈展开。 */
    struct Thrower
    {
        Thrower() { throw std::runtime_error{"handle test unwind"}; }
    };

    // -----------------------------------------------------------------------
    // 句柄一侧：取句柄、查状态、数资源。
    // -----------------------------------------------------------------------

    /** @brief 打开空设备，得到一个真实打开着的句柄（文件描述符）。 */
    [[nodiscard]]
    auto make_null_handle() -> hd::Native
    {
#if defined(_WIN32)
        return CreateFileW(
            L"NUL",
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
#else
        return ::open("/dev/null", O_RDONLY);
#endif
    }

    /** @brief 去打开一个肯定不存在的文件：接口交回它的失败值。 */
    [[nodiscard]]
    auto make_failed_file_handle() -> hd::Native
    {
#if defined(_WIN32)
        return CreateFileW(
            L"ccg-handle-test-missing\\file.txt",
            GENERIC_READ,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
#else
        return ::open("/ccg-handle-test-missing/file.txt", O_RDONLY);
#endif
    }

    /** @brief 一个句柄值此刻在本进程里是否还打开着。 */
    [[nodiscard]]
    auto still_open(hd::Native handle) -> bool
    {
#if defined(_WIN32)
        DWORD flags = 0;
        return handle != nullptr && GetHandleInformation(handle, &flags) != 0;
#else
        return handle >= 0 && ::fcntl(handle, F_GETFD) >= 0;
#endif
    }

    /** @brief 资源计数探针的读数。 */
    struct Counts
    {
        bool        valid = true; ///< 各项读数是否都取到了
        std::size_t handles = 0;  ///< 本进程的句柄数（文件描述符个数）
        std::size_t threads = 0;  ///< 本进程的线程数
        std::size_t children = 0; ///< 以本进程为父的子进程数
    };

#if defined(_WIN32)

    /** @brief 数一遍本进程的线程与子进程；进程快照由拥有型句柄持有。
     *          快照取不到时读数标记为无效。 */
    [[nodiscard]]
    auto process_counts() -> Counts
    {
        Counts          counts;
        const hd::Owned snapshot{hd::Owned::adopt_file(CreateToolhelp32Snapshot(
            TH32CS_SNAPTHREAD | TH32CS_SNAPPROCESS,
            0))};
        counts.valid = snapshot.valid();
        if (!counts.valid)
        {
            return counts;
        }
        const DWORD   pid = GetCurrentProcessId();
        THREADENTRY32 thread{};
        thread.dwSize = sizeof(thread);
        BOOL ok = Thread32First(snapshot.get(), &thread);
        counts.valid = ok != 0;
        for (; ok != 0; ok = Thread32Next(snapshot.get(), &thread))
        {
            counts.threads += thread.th32OwnerProcessID == pid ? 1 : 0;
        }
        PROCESSENTRY32W process{};
        process.dwSize = sizeof(process);
        ok = Process32FirstW(snapshot.get(), &process);
        counts.valid = counts.valid && ok != 0;
        for (; ok != 0; ok = Process32NextW(snapshot.get(), &process))
        {
            counts.children += process.th32ParentProcessID == pid ? 1 : 0;
        }
        return counts;
    }

    /** @brief 读一次本进程的句柄数、线程数与子进程数；任何一项读不到
     *          都标记为无效，免得断言拿 0 对 0 空过。 */
    [[nodiscard]]
    auto read_counts() -> Counts
    {
        Counts counts;
        DWORD  count = 0;
        counts.valid = GetProcessHandleCount(GetCurrentProcess(), &count) != 0;
        counts.handles = count;
        const Counts rest = process_counts();
        counts.threads = rest.threads;
        counts.children = rest.children;
        counts.valid = counts.valid && rest.valid;
        return counts;
    }

#else

    /**
     * @brief 读一次本进程打开着的文件描述符个数。
     *
     * @internal 数的是 /dev/fd 下的条目；遍历目录本身占一个描述符，前后
     *           口径一致，不影响比较。目录打不开时读数标记为无效。线程
     *           数与子进程数没有可移植的读法，这个分支不量，恒为 0。
     */
    [[nodiscard]]
    auto read_counts() -> Counts
    {
        Counts          counts;
        std::error_code ec;
        const auto begin = std::filesystem::directory_iterator{"/dev/fd", ec};
        counts.valid = !ec;
        counts.handles = ec ? 0
                            : static_cast<std::size_t>(std::ranges::distance(
                                  begin,
                                  std::filesystem::directory_iterator{}));
        return counts;
    }

#endif

    /** @brief 断言一次取用与放手之后三项计数都与基线相同。 */
    void check_back_to_baseline(
        R&                 r,
        const Counts&      baseline,
        const Counts&      after,
        const std::string& what)
    {
        cg::test::check(
            r,
            baseline.valid && after.valid,
            what + ": every count could be read before and after");
        cg::test::check(
            r,
            after.handles == baseline.handles,
            what + ": handle count is back to the baseline");
        cg::test::check(
            r,
            after.threads == baseline.threads,
            what + ": thread count is back to the baseline");
        cg::test::check(
            r,
            after.children == baseline.children,
            what + ": child process count is back to the baseline");
    }

    /** @brief 一轮完整的取用与放手：接管、移动、交出、再接管、复位。 */
    void run_handle_cycle()
    {
        hd::Owned first = hd::Owned::adopt_file(make_null_handle());
        hd::Owned second = std::move(first);
        hd::Owned third = hd::Owned::adopt_file(make_null_handle());
        third = std::move(second);
        hd::Owned& alias = third;
        third = std::move(alias);
        hd::Owned fourth = hd::Owned::adopt_file(third.release());
        fourth.reset();
#if defined(_WIN32)
        hd::Owned event =
            hd::Owned::adopt(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        event = hd::Owned::adopt(CreateEventW(nullptr, TRUE, FALSE, nullptr));
#endif
        hd::Owned missing = hd::Owned::adopt_file(make_failed_file_handle());
        missing.reset();
    }

    /** @brief 默认构造不持有句柄；接口的失败值一律接管成空对象。 */
    void owned_default_and_failed(R& r)
    {
        const std::size_t before = read_counts().handles;
        hd::Owned         empty;
        empty.reset();
        cg::test::check(r, !empty.valid(), "a default object holds nothing");
        cg::test::check(r, !still_open(empty.get()), "its raw value is empty");

#if defined(_WIN32)
        const hd::Owned null = hd::Owned::adopt_file(nullptr);
        cg::test::check(r, !null.valid(), "a null value adopts into nothing");
        const hd::Owned bogus = hd::Owned::adopt_file(INVALID_HANDLE_VALUE);
        cg::test::check(
            r,
            !bogus.valid(),
            "the invalid-handle value is treated as no handle");
#else
        const hd::Owned negative = hd::Owned::adopt_file(-1);
        cg::test::check(
            r,
            !negative.valid(),
            "a negative descriptor is treated as no descriptor");
#endif

        hd::Owned failed = hd::Owned::adopt_file(make_failed_file_handle());
        cg::test::check(
            r,
            !failed.valid(),
            "the failed-file value is treated as no handle");
        cg::test::check(r, !still_open(failed.get()), "no raw value is kept");
        failed.reset();
        cg::test::check(
            r,
            read_counts().handles == before,
            "dropping a failed value closes nothing and leaks nothing");
    }

    /** @brief 接管与立即关闭：持有时多一个句柄，复位后回到原数，且原句
     *          柄值已查不到。 */
    void owned_adopt_and_reset(R& r)
    {
        const std::size_t before = read_counts().handles;
        hd::Owned         handle = hd::Owned::adopt_file(make_null_handle());
        cg::test::check(
            r,
            handle.valid(),
            "adopt_file keeps a handle that was really opened");
        const hd::Native raw = handle.get();
        cg::test::check(r, still_open(raw), "the raw value is open");
        cg::test::check(
            r,
            read_counts().handles == before + 1,
            "holding the handle adds exactly one");
        handle.reset();
        cg::test::check(r, !handle.valid(), "reset leaves the object empty");
        cg::test::check(
            r,
            read_counts().handles == before,
            "reset closes the handle at once");
        cg::test::check(
            r,
            !still_open(raw),
            "the closed value is no longer open");
        handle.reset();
        cg::test::check(
            r,
            !handle.valid() && read_counts().handles == before,
            "resetting an empty object does nothing");
    }

    /** @brief 移动构造之后源对象为空，目标持有同一个句柄值，计数不变。 */
    void owned_move_empties_source(R& r)
    {
        const std::size_t before = read_counts().handles;
        hd::Owned         first = hd::Owned::adopt_file(make_null_handle());
        const hd::Native  raw = first.get();
        hd::Owned         second = std::move(first);
        // 被移动走的对象是有效的空对象，这里检查的正是它。
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move)
        cg::test::check(r, !first.valid(), "the moved-from object is empty");
        cg::test::check(
            r,
            second.valid() && second.get() == raw,
            "the target holds the very same handle value");
        cg::test::check(
            r,
            read_counts().handles == before + 1,
            "moving opens and closes nothing");
        const hd::Owned from_empty = std::move(first);
        cg::test::check(
            r,
            !from_empty.valid(),
            "moving an empty object yields an empty object");
        second.reset();
        cg::test::check(
            r,
            read_counts().handles == before,
            "the target's reset closes the handle");
    }

    /** @brief 移动赋值：先关掉目标原有的句柄再接管来源的；自己赋给
     *          自己保持原样；从空对象赋值等于清空。 */
    void owned_move_assign(R& r)
    {
        const std::size_t before = read_counts().handles;
        hd::Owned         left = hd::Owned::adopt_file(make_null_handle());
        hd::Owned         right = hd::Owned::adopt_file(make_null_handle());
        cg::test::check(
            r,
            read_counts().handles == before + 2,
            "two owners hold two handles");
        const hd::Native right_raw = right.get();
        left = std::move(right);
        // 被移动走的对象是有效的空对象，这里检查的正是它。
        cg::test::check(
            r,
            !right.valid() // NOLINT(clang-analyzer-cplusplus.Move)
                && left.valid() && left.get() == right_raw,
            "the target takes over the source's handle");
        cg::test::check(
            r,
            read_counts().handles == before + 1,
            "the target's own handle was closed first");
        hd::Owned& alias = left;
        left = std::move(alias);
        cg::test::check(
            r,
            left.valid() && left.get() == right_raw,
            "self move assignment keeps the handle");
        cg::test::check(
            r,
            read_counts().handles == before + 1,
            "self move assignment closes nothing");
        hd::Owned empty;
        left = std::move(empty);
        cg::test::check(
            r,
            !left.valid() && read_counts().handles == before,
            "assigning from an empty object closes the handle");
    }

    /** @brief 交出所有权：对象变空、不再关闭，句柄仍然打开；重新接管后
     *          才由复位关闭（测试自己不写关闭调用）。 */
    void owned_release_hands_out(R& r)
    {
        const std::size_t before = read_counts().handles;
        hd::Owned         handle = hd::Owned::adopt_file(make_null_handle());
        const hd::Native  raw = handle.get();
        cg::test::check(
            r,
            handle.release() == raw && !handle.valid(),
            "release returns the raw value and empties the object");
        handle.reset();
        cg::test::check(
            r,
            still_open(raw) && read_counts().handles == before + 1,
            "the released handle stays open");
        hd::Owned again = hd::Owned::adopt_file(raw);
        again.reset();
        cg::test::check(
            r,
            !still_open(raw) && read_counts().handles == before,
            "re-adopting and resetting closes it");
    }

    /** @brief 资源计数探针：先预热一轮再取基线，连续多轮取用与放手之后
     *          句柄数、线程数、子进程数与基线相同。 */
    void owned_counts_return_to_baseline(R& r)
    {
        run_handle_cycle();
        const Counts  baseline = read_counts();
        constexpr int rounds = 64;
        for (int i = 0; i < rounds; ++i)
        {
            run_handle_cycle();
        }
        const Counts after = read_counts();
        check_back_to_baseline(
            r,
            baseline,
            after,
            "handle cycle over 64 rounds");
    }

    /** @brief 取到句柄之后同一作用域里有对象构造抛出：栈展开把句柄关
     *          掉，计数回到原数。 */
    void owned_closed_on_unwind(R& r)
    {
        const std::size_t before = read_counts().handles;
        bool              unwound = false;
        try
        {
            [[maybe_unused]]
            const hd::Owned handle = hd::Owned::adopt_file(make_null_handle());
            [[maybe_unused]]
            const Thrower thrower;
        }
        catch (const std::runtime_error&)
        {
            unwound = true;
        }
        cg::test::check(r, unwound, "the sentinel threw");
        cg::test::check(
            r,
            read_counts().handles == before,
            "stack unwinding closed the handle");
    }

    // -----------------------------------------------------------------------
    // 数据库一侧：语句、文本、执行助手。
    // -----------------------------------------------------------------------

    /** @brief 打开一个内存库；连接随对象析构关闭，测试里不写关闭调用。
     *          打开旗标必须显式给出，连接器的默认旗标是 0，库会拒绝。 */
    [[nodiscard]]
    auto open_memory_db() -> sqlpp::sqlite3::connection
    {
        sqlpp::sqlite3::connection_config config;
        config.path_to_database = ":memory:";
        config.flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
        return sqlpp::sqlite3::connection{config};
    }

    /** @brief 连接上现存的原生语句条数（证明失败与展开没有遗留）。 */
    [[nodiscard]]
    auto statement_count(sqlite3* db) -> std::size_t
    {
        std::size_t count = 0;
        for (sqlite3_stmt* stmt = sqlite3_next_stmt(db, nullptr);
             stmt != nullptr;
             stmt = sqlite3_next_stmt(db, stmt))
        {
            ++count;
        }
        return count;
    }

    /** @brief 预编译失败不抛，带回库的错误文本；失败的预编译不在连接上
     *          留下半成品语句。 */
    void native_statement_prepare_failure(R& r)
    {
        sqlpp::sqlite3::connection db = open_memory_db();
        sqlite3*                   raw = db.native_handle();
        const std::size_t          base = statement_count(raw);
        const std::expected<dbn::NativeStatement, std::string> missing =
            dbn::NativeStatement::prepare(raw, "SELECT * FROM no_such_table;");
        cg::test::check(
            r,
            !missing.has_value(),
            "preparing against a missing table fails without throwing");
        cg::test::check(
            r,
            !missing.error().empty(),
            "the failure carries an error text");
        cg::test::check(
            r,
            missing.error().find("no_such_table") != std::string::npos,
            "the error text names the missing table");
        const std::expected<dbn::NativeStatement, std::string> blank =
            dbn::NativeStatement::prepare(raw, "   -- nothing to run");
        cg::test::check(
            r,
            !blank.has_value() && !blank.error().empty(),
            "text without a statement fails instead of yielding an empty "
            "object");
        cg::test::check(
            r,
            statement_count(raw) == base,
            "failed prepares leave no half-made statement behind");
    }

    /** @brief 步进与取列：逐行读到整数与文本，空值列读出空视图，走完
     *          后报结束；语句在对象销毁之前一直在连接上。 */
    void native_statement_step_and_columns(R& r)
    {
        sqlpp::sqlite3::connection db = open_memory_db();
        sqlite3*                   raw = db.native_handle();
        const std::size_t          base = statement_count(raw);
        cg::test::check(
            r,
            dbn::exec_native(
                raw,
                "CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT);"
                "INSERT INTO t (id, name) VALUES (1, 'alpha'), (2, NULL);")
                .has_value(),
            "the fixture table is created");
        std::expected<dbn::NativeStatement, std::string> select =
            dbn::NativeStatement::prepare(
                raw,
                "SELECT id, name FROM t ORDER BY id;");
        cg::test::check(r, select.has_value(), "the select prepares");
        cg::test::check(
            r,
            statement_count(raw) == base + 1,
            "the prepared statement is on the connection");
        const dbn::StepOutcome first = select->step();
        cg::test::check(r, first.has_row(), "the first step yields a row");
        cg::test::check(
            r,
            select->column_int64(0) == 1 && select->column_text(1) == "alpha",
            "integer and text columns read back");
        const dbn::StepOutcome second = select->step();
        cg::test::check(r, second.has_row(), "the second step yields a row");
        cg::test::check(
            r,
            select->column_int64(0) == 2 && select->column_text(1).empty(),
            "a NULL column reads as an empty view");
        const dbn::StepOutcome end = select->step();
        cg::test::check(
            r,
            end.finished() && !end.failed(),
            "the third step reports the end");
        cg::test::check(
            r,
            statement_count(raw) == base + 1,
            "the statement stays owned while the object lives");
    }

    /** @brief 绑定与复位：复位后绑定仍在；清除绑定后未再绑定的参数按
     *          空值参与运算。 */
    void native_statement_bind_and_reset(R& r)
    {
        sqlpp::sqlite3::connection db = open_memory_db();
        sqlite3*                   raw = db.native_handle();
        std::expected<dbn::NativeStatement, std::string> sum =
            dbn::NativeStatement::prepare(raw, "SELECT ?1 + ?2;");
        cg::test::check(
            r,
            sum.has_value(),
            "the parameterized select prepares");
        cg::test::check(
            r,
            sum->bind_int64(1, 20) == SQLITE_OK
                && sum->bind_int64(2, 22) == SQLITE_OK,
            "binding reports success");
        cg::test::check(
            r,
            sum->step().has_row() && sum->column_int64(0) == 42,
            "the bound values add up");
        cg::test::check(r, sum->reset() == SQLITE_OK, "reset reports success");
        cg::test::check(
            r,
            sum->step().has_row() && sum->column_int64(0) == 42,
            "bindings survive a reset");
        static_cast<void>(sum->reset());
        sum->clear_bindings();
        cg::test::check(r, sum->bind_int64(1, 5) == SQLITE_OK, "one rebinding");
        cg::test::check(
            r,
            sum->step().has_row() && sum->column_int64(0) == 0,
            "the cleared parameter reads as NULL, so the sum is NULL");
    }

    /** @brief 只能移动：移动后源为空，目标接管；移动赋值先销毁目标原有
     *          的语句；被移动走的对象上各成员函数按误用处理，不抛。 */
    void native_statement_move(R& r)
    {
        sqlpp::sqlite3::connection db = open_memory_db();
        sqlite3*                   raw = db.native_handle();
        const std::size_t          base = statement_count(raw);
        std::expected<dbn::NativeStatement, std::string> first =
            dbn::NativeStatement::prepare(raw, "SELECT 1;");
        std::expected<dbn::NativeStatement, std::string> second =
            dbn::NativeStatement::prepare(raw, "SELECT 2;");
        cg::test::check(
            r,
            statement_count(raw) == base + 2,
            "two statements are on the connection");
        dbn::NativeStatement moved = std::move(*first);
        cg::test::check(
            r,
            !first->valid(),
            "the moved-from statement is empty");
        cg::test::check(
            r,
            moved.valid() && statement_count(raw) == base + 2,
            "moving destroys nothing");
        *second = std::move(moved);
        // 被移动走的对象是有效的空对象，这里检查的正是它。
        cg::test::check(
            r,
            !moved.valid() // NOLINT(clang-analyzer-cplusplus.Move)
                && second->valid(),
            "move assignment hands the statement to the target");
        cg::test::check(
            r,
            statement_count(raw) == base + 1,
            "move assignment destroyed the target's old statement");
        cg::test::check(
            r,
            second->step().has_row() && second->column_int64(0) == 1,
            "the surviving statement is the moved-in one");
        dbn::NativeStatement& alias = *second;
        *second = std::move(alias);
        cg::test::check(
            r,
            second->valid() && statement_count(raw) == base + 1,
            "self move assignment keeps the statement");
        first->clear_bindings();
        cg::test::check(
            r,
            first->step().failed(),
            "stepping a moved-from statement reports misuse");
        cg::test::check(
            r,
            first->bind_int64(1, 1) == SQLITE_MISUSE
                && first->reset() == SQLITE_MISUSE,
            "binding and resetting a moved-from statement report misuse");
        cg::test::check(
            r,
            first->column_int64(0) == 0 && first->column_text(0).empty(),
            "reading columns of a moved-from statement yields empty values");
    }

    /** @brief 预编译成功后同一作用域里有对象构造抛出：栈展开销毁语
     *          句，连接上没有遗留。 */
    void native_statement_closed_on_unwind(R& r)
    {
        sqlpp::sqlite3::connection db = open_memory_db();
        sqlite3*                   raw = db.native_handle();
        const std::size_t          base = statement_count(raw);
        bool                       unwound = false;
        try
        {
            [[maybe_unused]]
            const std::expected<dbn::NativeStatement, std::string> held =
                dbn::NativeStatement::prepare(raw, "SELECT 3;");
            [[maybe_unused]]
            const Thrower thrower;
        }
        catch (const std::runtime_error&)
        {
            unwound = true;
        }
        cg::test::check(r, unwound, "the sentinel threw");
        cg::test::check(
            r,
            statement_count(raw) == base,
            "unwinding finalized the statement");
    }

    /** @brief 库分配的文本由拥有型对象持有：空指针为空，移动后源为空。 */
    void native_text_owns_and_moves(R& r)
    {
        const dbn::NativeText none{nullptr};
        cg::test::check(
            r,
            !none.valid() && none.view().empty(),
            "a null text is empty");
        dbn::NativeText owned{sqlite3_mprintf("%s-%d", "ccg", 7)};
        cg::test::check(
            r,
            owned.valid() && owned.view() == "ccg-7",
            "the library-made text reads back");
        dbn::NativeText moved = std::move(owned);
        // 被移动走的对象是有效的空对象，这里检查的正是它。
        cg::test::check(
            r,
            !owned.valid() // NOLINT(clang-analyzer-cplusplus.Move)
                && moved.view() == "ccg-7",
            "moving hands the text over and empties the source");
        moved = dbn::NativeText{nullptr};
        cg::test::check(
            r,
            !moved.valid() && moved.view().empty(),
            "assigning an empty text releases the old one");
    }

    /** @brief 执行助手：整段文本照常执行；失败时带回库的错误文本。 */
    void native_exec_reports_failures(R& r)
    {
        sqlpp::sqlite3::connection             db = open_memory_db();
        sqlite3*                               raw = db.native_handle();
        const std::expected<void, std::string> ok = dbn::exec_native(
            raw,
            "CREATE TABLE t (id INTEGER); INSERT INTO t VALUES (1);");
        cg::test::check(r, ok.has_value(), "a text with two statements runs");
        const std::expected<void, std::string> bad =
            dbn::exec_native(raw, "INSERT INTO no_such_table VALUES (1);");
        cg::test::check(
            r,
            !bad.has_value()
                && bad.error().find("no_such_table") != std::string::npos,
            "a failing text reports the library's error text");
    }

    // -----------------------------------------------------------------------
    // 平台层取值函数：命令行参数与环境条目。
    // -----------------------------------------------------------------------

    /** @brief 本测试程序的子进程角色参数：逐行回显命令行，退出码为 0。 */
    constexpr std::string_view ECHO_ARGS = "--echo-args-wide";

    /** @brief 肯定拉不起来的可执行名：给派生失败的路径用。 */
    constexpr std::string_view MISSING_EXE = "ccg-no-such-handle-test-exe";

    /** @brief 按换行切分文本；行尾的回车去掉，空行保留。 */
    [[nodiscard]]
    auto split_lines(std::string_view text) -> std::vector<std::string>
    {
        std::vector<std::string> lines;
        std::size_t              begin = 0;
        while (begin < text.size())
        {
            const std::size_t end =
                std::min(text.find('\n', begin), text.size());
            std::string_view line = text.substr(begin, end - begin);
            if (!line.empty() && line.back() == '\r')
            {
                line.remove_suffix(1);
            }
            lines.emplace_back(line);
            if (end == text.size())
            {
                break;
            }
            begin = end + 1;
        }
        return lines;
    }

    /** @brief 命令行参数取值函数：拉起一份本测试程序的子进程，带上含
     *          空格、中文、引号、尾部反斜杠、空串的参数；子进程把取到的
     *          参数逐行打印回来，必须原样往返。 */
    void platform_args_round_trip(R& r)
    {
        const std::vector<std::string> extra{
            "plain",
            "with space",
            "中文 路径",
            "quote\"inside",
            "trailing\\",
            "",
            "C:\\temp dir\\file.txt"};
        std::vector<std::string> args{std::string{ECHO_ARGS}};
        args.insert(args.end(), extra.begin(), extra.end());
        const std::optional<plat::CaptureExecOut> run =
            plat::capture_exec(plat::executable_path(), args);
        if (!run.has_value() || run->exit_code != 0)
        {
            cg::test::check(
                r,
                false,
                "the child copy of this test program ran and exited with 0");
            return;
        }
        std::vector<std::string> expected{
            plat::narrow_utf8(plat::executable_path().wstring()),
            std::string{ECHO_ARGS}};
        expected.insert(expected.end(), extra.begin(), extra.end());
        const std::vector<std::string> lines = split_lines(run->out);
        cg::test::check(
            r,
            lines.size() == expected.size(),
            "the child printed exactly one line per argument");
        const std::size_t count = std::min(lines.size(), expected.size());
        std::size_t       first_diff = count;
        for (std::size_t i = 0; i < count; ++i)
        {
            if (lines[i] != expected[i])
            {
                first_diff = i;
                break;
            }
        }
        cg::test::check(
            r,
            first_diff == count && lines.size() == expected.size(),
            "every argument read by the platform function round-trips "
            "unchanged (first differing argument index "
                + std::to_string(first_diff) + ")");
    }

    /** @brief 环境条目取值函数：设一个变量后列表里有它的"名字=值"，
     *          恢复之后消失。 */
    void platform_env_entries(R& r)
    {
        constexpr std::wstring_view expected =
            L"CCG_HANDLE_TEST_ENV=ccg-handle-test-value";
        const cg::test::EnvScope scope{"CCG_HANDLE_TEST_ENV"};
        scope.set("ccg-handle-test-value");
        bool found = false;
        for (const std::wstring& entry : plat::environment_entries_wide())
        {
            found = found || entry == expected;
        }
        cg::test::check(
            r,
            found,
            "the entry of the freshly set variable is in the list");
        scope.restore();
        bool gone = true;
        for (const std::wstring& entry : plat::environment_entries_wide())
        {
            gone = gone && entry != expected;
        }
        cg::test::check(
            r,
            gone,
            "the entry is gone once the variable is restored");
    }

    // -----------------------------------------------------------------------
    // 平台封装单元：句柄取到即交给拥有者之后，各条派生与文件路径走完
    // 一轮，本进程的资源计数必须回到基线。
    // -----------------------------------------------------------------------

    /** @brief 把一个判定折成 0 或 1，供逐轮累加用。 */
    [[nodiscard]]
    constexpr auto tally(bool condition) noexcept -> int
    {
        return condition ? 1 : 0;
    }

    /** @brief 读回一个文本文件的全部字节。 */
    [[nodiscard]]
    auto read_text(const std::filesystem::path& path) -> std::string
    {
        std::ifstream in{path, std::ios::binary};
        return {
            std::istreambuf_iterator<char>{in},
            std::istreambuf_iterator<char>{}};
    }

    /** @brief 原子写、原子替换、目录压缩与进程存活探针各走一轮。 */
    void run_file_helper_round(
        const std::filesystem::path& final_path,
        const std::filesystem::path& tmp)
    {
        (void)plat::atomic_write_file(tmp, "payload");
        (void)plat::atomic_replace(tmp, final_path);
        plat::enable_dir_compression(final_path.parent_path().string());
        (void)plat::process_alive(plat::current_pid());
        (void)plat::process_alive(0);
    }

    /** @brief 原子写与原子替换、目录压缩、进程存活探针：每轮之后文件内
     *          容正确，连续多轮之后句柄数回到基线。 */
    void platform_file_helpers_counts(R& r)
    {
        namespace fs = std::filesystem;
        const cg::test::ScratchDir scratch{
            fs::temp_directory_path()
            / ("ccg-handle-files-" + std::to_string(plat::make_nonce()))};
        const fs::path final_path = scratch.base / "final.txt";
        const fs::path tmp = scratch.base / "final.txt.abc.tmp";
        run_file_helper_round(final_path, tmp);
        const Counts  baseline = read_counts();
        constexpr int rounds = 32;
        for (int i = 0; i < rounds; ++i)
        {
            run_file_helper_round(final_path, tmp);
        }
        const Counts after = read_counts();
        cg::test::check(
            r,
            plat::process_alive(plat::current_pid()),
            "the test process is reported alive");
        cg::test::check(
            r,
            read_text(final_path) == "payload",
            "atomic write and replace leave the new bytes in place");
        check_back_to_baseline(
            r,
            baseline,
            after,
            "file helpers over 32 rounds");
    }

    /** @brief 文件锁：持有期间恰多一个句柄；被移动走的对象析构时不碰已
     *          经交出去的锁；连续取锁与放手之后计数回到基线，锁文件不
     *          残留。 */
    void platform_file_lock_counts(R& r)
    {
        namespace fs = std::filesystem;
        const cg::test::ScratchDir scratch{
            fs::temp_directory_path()
            / ("ccg-handle-lock-" + std::to_string(plat::make_nonce()))};
        const fs::path first_path = scratch.base / "first.lock";
        const fs::path second_path = scratch.base / "second.lock";
        {
            const plat::FileLock warm =
                plat::try_acquire_file_lock(first_path, "warm", 60'000);
        }
        const Counts baseline = read_counts();

        plat::FileLock lock =
            plat::try_acquire_file_lock(first_path, "first", 60'000);
        cg::test::check(r, lock.held(), "a fresh lock is held at once");
        cg::test::check(
            r,
            read_counts().handles == baseline.handles + 1,
            "holding the lock adds exactly one handle");
        cg::test::check(
            r,
            fs::exists(first_path),
            "holding the lock keeps the lock file");
        {
            const plat::FileLock second =
                plat::try_acquire_file_lock(first_path, "second", 60'000);
            cg::test::check(
                r,
                !second.held(),
                "a live holder excludes a second contender");
            cg::test::check(
                r,
                read_counts().handles == baseline.handles + 1,
                "a failed attempt opens nothing");
        }

        // 移动赋值：目标原有的锁先被放掉（句柄关闭、文件删除），再接管
        // 另一条路径上的新锁。
        lock = plat::try_acquire_file_lock(second_path, "second", 60'000);
        cg::test::check(
            r,
            lock.held() && !fs::exists(first_path) && fs::exists(second_path),
            "move assignment releases the old lock and takes the new one");
        cg::test::check(
            r,
            read_counts().handles == baseline.handles + 1,
            "move assignment leaves exactly one handle open");

        // 移动构造：源对象交出一切，源对象析构时不得动已经归别人的锁。
        plat::FileLock keeper;
        {
            plat::FileLock moved = std::move(lock);
            keeper = std::move(moved);
            // 被移动走的对象是有效的空对象，这里检查的正是它。
            // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move)
            const bool handed = !lock.held() && keeper.held();
            cg::test::check(
                r,
                handed,
                "moving hands the lock over and empties the sources");
        }
        cg::test::check(
            r,
            keeper.held() && fs::exists(second_path),
            "destroying the moved-from objects leaves the lock alone");
        cg::test::check(
            r,
            read_counts().handles == baseline.handles + 1,
            "moving opens and closes nothing");
        keeper.release();
        keeper.release();
        cg::test::check(
            r,
            !keeper.held() && !fs::exists(second_path)
                && read_counts().handles == baseline.handles,
            "release removes the lock file and closing it twice is harmless");

        constexpr int rounds = 32;
        for (int i = 0; i < rounds; ++i)
        {
            const plat::FileLock cycled =
                plat::try_acquire_file_lock(first_path, "cycle", 60'000);
        }
        check_back_to_baseline(
            r,
            baseline,
            read_counts(),
            "file lock over 32 rounds");
    }

    /** @brief 一个建成的只读映射在本进程里占几个句柄（文件描述符）：
     *          Windows 上是文件与映射两个句柄，其余平台是一个描述符。 */
    constexpr std::size_t MAP_HANDLES =
#if defined(_WIN32)
        2;
#else
        1;
#endif

    /** @brief 把一段映射字节按文本读出来。 */
    [[nodiscard]]
    auto as_text(std::span<const std::byte> bytes) -> std::string_view
    {
        return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    }

    /** @brief 只读映射的一轮：有内容的文件映射一次并走一遍移动构造与移
     *          动赋值；空文件与不存在的文件各构造一次（都不建成映射）。 */
    void run_map_round(const std::filesystem::path& content)
    {
        plat::ReadOnlyMap mapped{content};
        plat::ReadOnlyMap moved = std::move(mapped);
        moved = plat::ReadOnlyMap{content};
        moved = plat::ReadOnlyMap{};
        const plat::ReadOnlyMap blank{content.parent_path() / "blank.bin"};
        const plat::ReadOnlyMap missing{content.parent_path() / "no-such.bin"};
    }

    /** @brief 只读映射：有内容的文件映射后字节读得回来；空文件与不存在
     *          的文件构造后对象为空、不多占句柄；移动之后只有目标持有映
     *          射；连续多轮之后三项计数回到基线。 */
    void platform_read_only_map(R& r)
    {
        namespace fs = std::filesystem;
        const cg::test::ScratchDir scratch{
            fs::temp_directory_path()
            / ("ccg-handle-map-" + std::to_string(plat::make_nonce()))};
        const fs::path content = scratch.base / "content.bin";
        const fs::path blank = scratch.base / "blank.bin";
        {
            std::ofstream out{content, std::ios::binary};
            out << "mapped bytes";
        }
        {
            std::ofstream out{blank, std::ios::binary};
        }
        run_map_round(content);
        const Counts baseline = read_counts();

        plat::ReadOnlyMap mapped{content};
        cg::test::check(
            r,
            mapped.valid() && as_text(mapped.bytes()) == "mapped bytes",
            "a file with content maps and its bytes read back");
        cg::test::check(
            r,
            read_counts().handles == baseline.handles + MAP_HANDLES,
            "a mapping holds exactly its own handles");
        const plat::ReadOnlyMap empty{blank};
        cg::test::check(
            r,
            !empty.valid() && empty.bytes().empty()
                && read_counts().handles == baseline.handles + MAP_HANDLES,
            "an empty file leaves the object empty and opens nothing more");
        const plat::ReadOnlyMap absent{scratch.base / "no-such.bin"};
        cg::test::check(
            r,
            !absent.valid() && absent.bytes().empty()
                && read_counts().handles == baseline.handles + MAP_HANDLES,
            "a missing file leaves the object empty and opens nothing more");

        plat::ReadOnlyMap target = std::move(mapped);
        // 被移动走的对象是有效的空对象，这里检查的正是它。
        cg::test::check(
            r,
            !mapped.valid() // NOLINT(clang-analyzer-cplusplus.Move)
                && target.valid()
                && read_counts().handles == baseline.handles + MAP_HANDLES,
            "moving hands the mapping over and opens nothing");
        target = plat::ReadOnlyMap{content};
        cg::test::check(
            r,
            target.valid() && as_text(target.bytes()) == "mapped bytes"
                && read_counts().handles == baseline.handles + MAP_HANDLES,
            "move assignment releases the old mapping and takes the new one");
        target = plat::ReadOnlyMap{};
        cg::test::check(
            r,
            !target.valid() && read_counts().handles == baseline.handles,
            "assigning an empty object releases the mapping");

        constexpr int rounds = 32;
        for (int i = 0; i < rounds; ++i)
        {
            run_map_round(content);
        }
        check_back_to_baseline(
            r,
            baseline,
            read_counts(),
            "read-only map over 32 rounds");
    }

    /** @brief 只捕获执行：连续拉起子进程若干轮之后，三项计数回到基线。 */
    void platform_capture_exec_counts(R& r)
    {
        const std::filesystem::path exe = plat::executable_path();
        (void)plat::capture_exec(exe, {std::string{ECHO_ARGS}});
        const Counts  baseline = read_counts();
        constexpr int rounds = 16;
        int           finished = 0;
        for (int i = 0; i < rounds; ++i)
        {
            const std::optional<plat::CaptureExecOut> run =
                plat::capture_exec(exe, {std::string{ECHO_ARGS}});
            finished += tally(run.has_value() && run->exit_code == 0);
        }
        const Counts after = read_counts();
        cg::test::check(
            r,
            finished == rounds,
            "every child ran and exited with 0");
        check_back_to_baseline(
            r,
            baseline,
            after,
            "capture_exec over 16 rounds");
    }

    /** @brief 带超时的执行：子进程的继承句柄收窄必须成立（属性列表建在
     *          对象自带的缓冲上），派生失败如实报未启动；两条路径各走若
     *          干轮之后计数回到基线。 */
    void platform_capture_exec_timed_counts(R& r)
    {
        namespace fs = std::filesystem;
        const fs::path exe = plat::executable_path();
        constexpr auto budget = std::chrono::milliseconds{30'000};
        (void)plat::capture_exec_timed(exe, {std::string{ECHO_ARGS}}, budget);
        const Counts  baseline = read_counts();
        constexpr int rounds = 8;
        int           finished = 0;
        int           restricted = 0;
        int           not_started = 0;
        for (int i = 0; i < rounds; ++i)
        {
            const plat::TimedExecResult quick =
                plat::capture_exec_timed(exe, {std::string{ECHO_ARGS}}, budget);
            finished += tally(
                quick.started && !quick.timed_out && quick.exit_code == 0);
            restricted += tally(quick.restricted_handles);
            const plat::TimedExecResult missing = plat::capture_exec_timed(
                fs::path{MISSING_EXE},
                {"arg"},
                std::chrono::milliseconds{5'000});
            not_started += tally(
                !missing.started && !missing.timed_out
                && missing.exit_code == -1);
        }
        const Counts after = read_counts();
        cg::test::check(
            r,
            finished == rounds,
            "every timed child ran to completion");
        cg::test::check(
            r,
            restricted == rounds,
            "the child's inherited handles are restricted every time");
        cg::test::check(
            r,
            not_started == rounds,
            "a missing executable is reported as not started every time");
        check_back_to_baseline(
            r,
            baseline,
            after,
            "capture_exec_timed over 8 rounds");
    }

#if defined(_WIN32)

    /** @brief 配置化子进程捕获原语一轮的两个结果：成功派生的那次与派
     *          生失败的那次。 */
    struct RunChildRound
    {
        plat::ChildRunResult spawned;
        plat::ChildRunResult missing;
    };

    /** @brief 配置化子进程捕获原语的一轮：成功派生一次（喂空标准输入、
     *          两路都捕获），再对一个不存在的可执行派生失败一次。 */
    [[nodiscard]]
    auto run_run_child_round(const std::filesystem::path& exe) -> RunChildRound
    {
        const std::wstring missing_exe = plat::widen_utf8(MISSING_EXE);
        plat::ChildRunSpec spec;
        spec.cmdline = std::format(
            L"\"{}\" {}",
            exe.wstring(),
            plat::widen_utf8(ECHO_ARGS));
        spec.stdin_mode = plat::ChildStdin::EofPipe;
        plat::ChildRunSpec missing;
        missing.application = missing_exe;
        missing.cmdline = std::format(L"\"{}\" --probe", missing_exe);
        return {
            .spawned = plat::run_child(spec),
            .missing = plat::run_child(missing)};
    }

    /** @brief 配置化子进程捕获原语：读端按移动交给排水线程之后，两路输
     *          出与退出码照旧收齐，派生失败照旧报错误码；连续若干轮之
     *          后句柄数与线程数回到基线。 */
    void platform_run_child_counts(R& r)
    {
        const std::filesystem::path exe = plat::executable_path();
        (void)run_run_child_round(exe);
        const Counts  baseline = read_counts();
        constexpr int rounds = 8;
        int           captured = 0;
        int           refused = 0;
        for (int i = 0; i < rounds; ++i)
        {
            const RunChildRound round = run_run_child_round(exe);
            captured += tally(
                round.spawned.pipes_ok && round.spawned.started
                && round.spawned.exit_code == 0
                && round.spawned.out.find(ECHO_ARGS) != std::string::npos);
            refused += tally(
                round.missing.pipes_ok && !round.missing.started
                && round.missing.spawn_error != 0);
        }
        const Counts after = read_counts();
        cg::test::check(
            r,
            captured == rounds,
            "every child is captured on both streams and exits with 0");
        cg::test::check(
            r,
            refused == rounds,
            "every spawn failure reports not started with an error code");
        check_back_to_baseline(r, baseline, after, "run_child over 8 rounds");
    }

    /** @brief 本测试程序的子进程角色参数：武装崩溃转储两次，把前后的
     *          资源计数打印回来。 */
    constexpr std::string_view ARM_PROBE_ARG = "--crashdump-arm-probe";

    /** @brief 把一行资源计数打印出去：标签在前，后接有效位与三项读数。 */
    void print_count_line(std::string_view tag, const Counts& counts)
    {
        std::println(
            "{} {} {} {} {}",
            tag,
            counts.valid ? 1 : 0,
            counts.handles,
            counts.threads,
            counts.children);
    }

    /** @brief 子进程角色：设好转储目录后武装崩溃转储两次，把武装前、
     *          第一次后、第二次后的资源计数各打印一行。
     *
     * @internal 第一次武装把调试帮助库的加载也算在内，加载带进多少句柄
     *           随机器与已加载的依赖而定；库常驻之后再武装一次，这次的
     *           读数差才是武装自身的开销。武装两次只在这个用完即退的子
     *           进程里做。
     */
    int crashdump_arm_probe(const char* dump_dir)
    {
        const Counts             before = read_counts();
        const cg::test::EnvScope scope{"CCG_CRASH_DUMP_DIR"};
        scope.set(dump_dir);
        cg::util::crashdump::install_from_env();
        const Counts first = read_counts();
        cg::util::crashdump::install_from_env();
        const Counts second = read_counts();
        print_count_line("before", before);
        print_count_line("first", first);
        print_count_line("second", second);
        return 0;
    }

    /** @brief 解析子进程打印的一行资源计数；标签对不上或读不全时标记
     *          为无效。 */
    [[nodiscard]]
    auto parse_count_line(const std::string& line, std::string_view tag)
        -> Counts
    {
        Counts             counts;
        std::string        label;
        int                valid = 0;
        std::istringstream stream{line};
        if (!(stream >> label >> valid >> counts.handles >> counts.threads
              >> counts.children)
            || label != tag)
        {
            return Counts{.valid = false};
        }
        counts.valid = valid != 0;
        return counts;
    }

    /** @brief 崩溃转储的武装：库常驻之后，武装一次恰多两个事件句柄与一
     *          条常驻转储线程，线程句柄与别的什么都不留下。武装放进子
     *          进程做，因为它会挂上进程级处理器。 */
    void crashdump_install_counts(R& r)
    {
        namespace fs = std::filesystem;
        const cg::test::ScratchDir scratch{
            fs::temp_directory_path()
            / ("ccg-handle-crashdump-" + std::to_string(plat::make_nonce()))};
        const std::optional<plat::CaptureExecOut> run = plat::capture_exec(
            plat::executable_path(),
            {std::string{ARM_PROBE_ARG}, scratch.base.string()});
        cg::test::check(
            r,
            run.has_value() && run->exit_code == 0,
            "the child copy of this test program armed the crash dump and "
            "exited with 0");
        if (!run.has_value() || run->exit_code != 0)
        {
            return;
        }
        const std::vector<std::string> lines = split_lines(run->out);
        const Counts                   before =
            parse_count_line(lines.size() > 0 ? lines[0] : "", "before");
        const Counts first =
            parse_count_line(lines.size() > 1 ? lines[1] : "", "first");
        const Counts second =
            parse_count_line(lines.size() > 2 ? lines[2] : "", "second");
        cg::test::check(
            r,
            before.valid && first.valid && second.valid,
            "the child printed three readable count lines");
        cg::test::check(
            r,
            first.threads == before.threads + 1
                && first.handles >= before.handles + 2,
            "the first arming adds the dump thread and at least the two "
            "event handles");
        cg::test::check(
            r,
            second.handles == first.handles + 2,
            "with the library resident, arming keeps exactly the two event "
            "handles");
        cg::test::check(
            r,
            second.threads == first.threads + 1,
            "each arming leaves exactly one resident dump thread");
    }

#endif

    const cg::test::Registrar REG_OWNED_DEFAULT{
        "handle.owned_default_and_failed",
        &owned_default_and_failed};
    const cg::test::Registrar REG_OWNED_ADOPT{
        "handle.owned_adopt_and_reset",
        &owned_adopt_and_reset};
    const cg::test::Registrar REG_OWNED_MOVE{
        "handle.owned_move_empties_source",
        &owned_move_empties_source};
    const cg::test::Registrar REG_OWNED_ASSIGN{
        "handle.owned_move_assign",
        &owned_move_assign};
    const cg::test::Registrar REG_OWNED_RELEASE{
        "handle.owned_release_hands_out",
        &owned_release_hands_out};
    const cg::test::Registrar REG_OWNED_COUNTS{
        "handle.owned_counts_return_to_baseline",
        &owned_counts_return_to_baseline};
    const cg::test::Registrar REG_OWNED_UNWIND{
        "handle.owned_closed_on_unwind",
        &owned_closed_on_unwind};
    const cg::test::Registrar REG_NATIVE_PREPARE{
        "db.native_statement_prepare_failure",
        &native_statement_prepare_failure};
    const cg::test::Registrar REG_NATIVE_STEP{
        "db.native_statement_step_and_columns",
        &native_statement_step_and_columns};
    const cg::test::Registrar REG_NATIVE_BIND{
        "db.native_statement_bind_and_reset",
        &native_statement_bind_and_reset};
    const cg::test::Registrar REG_NATIVE_MOVE{
        "db.native_statement_move",
        &native_statement_move};
    const cg::test::Registrar REG_NATIVE_UNWIND{
        "db.native_statement_closed_on_unwind",
        &native_statement_closed_on_unwind};
    const cg::test::Registrar REG_NATIVE_TEXT{
        "db.native_text_owns_and_moves",
        &native_text_owns_and_moves};
    const cg::test::Registrar REG_NATIVE_EXEC{
        "db.native_exec_reports_failures",
        &native_exec_reports_failures};
    const cg::test::Registrar REG_PLATFORM_ARGS{
        "platform.process_args_round_trip",
        &platform_args_round_trip};
    const cg::test::Registrar REG_PLATFORM_ENV{
        "platform.environment_entries_wide",
        &platform_env_entries};
    const cg::test::Registrar REG_PLATFORM_FILE_HELPERS{
        "platform.file_helpers_counts",
        &platform_file_helpers_counts};
    const cg::test::Registrar REG_PLATFORM_FILE_LOCK{
        "platform.file_lock_counts",
        &platform_file_lock_counts};
    const cg::test::Registrar REG_PLATFORM_READ_ONLY_MAP{
        "platform.read_only_map",
        &platform_read_only_map};
    const cg::test::Registrar REG_PLATFORM_CAPTURE_EXEC{
        "platform.capture_exec_counts",
        &platform_capture_exec_counts};
    const cg::test::Registrar REG_PLATFORM_CAPTURE_TIMED{
        "platform.capture_exec_timed_counts",
        &platform_capture_exec_timed_counts};
#if defined(_WIN32)
    const cg::test::Registrar REG_PLATFORM_RUN_CHILD{
        "platform.run_child_counts",
        &platform_run_child_counts};
    const cg::test::Registrar REG_CRASHDUMP_INSTALL{
        "crashdump.install_counts",
        &crashdump_install_counts};
#endif

} // namespace

// 用 module-unit main 是有意为之：项目每个源文件都是具名模块单元；普通
// TU 的 main 会破坏这项全模块政策。
// NOLINTNEXTLINE(clang-diagnostic-main-attached-to-named-module)
export int main(int argc, char* const* argv)
{
    if (argc >= 2 && std::string_view{argv[1]} == ECHO_ARGS)
    {
        // 子进程角色：把平台层取到的命令行参数逐行按 UTF-8 打印回去。
        for (const std::wstring& arg : cg::util::platform::process_args_wide())
        {
            std::println("{}", cg::util::platform::narrow_utf8(arg));
        }
        return 0;
    }
#if defined(_WIN32)
    if (argc >= 3 && std::string_view{argv[1]} == ARM_PROBE_ARG)
    {
        // 子进程角色：武装崩溃转储，把前后的资源计数打印回去。
        return crashdump_arm_probe(argv[2]);
    }
#endif
    return cg::test::run_all();
}
