export module cg.test.util;

// cg.util 的单元测试：FNV-1a 参考向量、限定名
// 拆分/拼接往返、路径规范化、Interner 去重/稳定性。

import std;
import cg.util;
import cg.util.cstd;
import cg.test.harness;

namespace
{

    using R = cg::test::Result;

    void fnv1a64_known_vectors(R& r)
    {
        cg::test::check(
            r,
            cg::util::fnv1a64("") == 0xcbf29ce484222325ULL,
            "empty input hashes to the FNV offset basis");
        cg::test::check(
            r,
            cg::util::fnv1a64("a") == 0xaf63dc4c8601ec8cULL,
            "reference vector for \"a\"");
        cg::test::check(
            r,
            cg::util::fnv1a64("foobar") == 0x85944171f73967e8ULL,
            "reference vector for \"foobar\"");
        cg::test::check(
            r,
            cg::util::fnv1a64("ab") != cg::util::fnv1a64("ba"),
            "hash is byte-order sensitive");
    }

    void qualified_round_trip(R& r)
    {
        const std::string_view q = "render::Canvas::draw";
        const auto             parts = cg::util::split_qualified(q);
        cg::test::check(r, parts.size() == 3, "component count");
        cg::test::check(
            r,
            parts[0] == "render" && parts[1] == "Canvas" && parts[2] == "draw",
            "component values");
        cg::test::check(
            r,
            cg::util::join_qualified(parts) == q,
            "join(split(q)) round-trips");

        cg::test::check(
            r,
            cg::util::split_qualified("").empty(),
            "empty input splits to nothing");
        const auto single = cg::util::split_qualified("Circle");
        cg::test::check(
            r,
            single.size() == 1 && single[0] == "Circle",
            "unqualified name is a single component");
        cg::test::check(
            r,
            cg::util::join_qualified({}) == "",
            "join of nothing is empty");
    }

    void norm_path_cases(R& r)
    {
        using cg::util::norm_path;
        cg::test::check(
            r,
            norm_path(R"(E:\Foo\Bar\baz.hpp)") == "e:/Foo/Bar/baz.hpp",
            "backslashes convert, drive letter lowercases");
        cg::test::check(
            r,
            norm_path("E://Foo\\\\Bar//baz.hpp") == "e:/Foo/Bar/baz.hpp",
            "duplicate separators collapse");
        cg::test::check(
            r,
            norm_path("src\\util/util.ixx") == "src/util/util.ixx",
            "relative path, mixed separators");
        cg::test::check(
            r,
            norm_path("C:/x") == "c:/x",
            "lowercase drive stays lowercase");
        cg::test::check(
            r,
            norm_path("c:/x") == "c:/x",
            "already-lowercase drive is unchanged (idempotent)");
        cg::test::check(
            r,
            norm_path(norm_path("C:\\A\\b.hpp")) == "c:/A/b.hpp",
            "norm_path is idempotent");
        cg::test::check(r, norm_path("") == "", "empty stays empty");
    }

    void interner_behavior(R& r)
    {
        using cg::util::Interner;
        int               stores = 0;
        Interner::id_type next_id = 100;
        Interner          it(
            [](std::string_view) -> std::optional<Interner::id_type>
            {
                return std::nullopt; // 纯内存测试：无预存物
            },
            [&](std::string_view) -> Interner::id_type
            {
                ++stores;
                return next_id++;
            });

        const auto a1 = it.intern("alpha");
        const auto a2 = it.intern("alpha"); // dedup: must not hit store again
        const auto b = it.intern("beta");
        cg::test::check(r, a1 == a2, "same string interns to the same id");
        cg::test::check(r, a1 != b, "different strings get different ids");
        cg::test::check(r, stores == 2, "store called once per unique string");
        cg::test::check(r, it.size() == 2, "interner tracks unique strings");
        cg::test::check(
            r,
            it.lookup("alpha") == a1 && it.lookup("beta") == b,
            "lookup finds the interned ids");
        cg::test::check(
            r,
            !it.lookup("gamma").has_value(),
            "unknown lookup is empty");

        // 自持副本安全：从临时缓冲驻留必须活过
        // 缓冲被覆写（长度足以击穿短字符串优化存储）。
        std::string tmp = "long-lived-interned-string-0123456789";
        const auto  owned = it.intern(std::string_view{tmp});
        tmp.assign(48, 'x');
        cg::test::check(
            r,
            it.str(owned) == "long-lived-interned-string-0123456789",
            "interned strings are owned copies");

        const auto c1 = it.intern("gamma");
        const auto c2 = it.intern(std::string_view{"gamma"});
        cg::test::check(
            r,
            c1 == c2,
            "ids stay stable across further interning");
        cg::test::check(
            r,
            stores == 4,
            "only genuinely new strings are stored");
    }

    void atomic_replace_cases(R& r)
    {
        namespace fs = std::filesystem;
        namespace platform = cg::util::platform;
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path dir =
            fs::temp_directory_path() / ("ccg-atomic-" + std::to_string(stamp));
        std::error_code ec;
        fs::create_directories(dir, ec);
        const fs::path final_path = dir / "payload.bin";
        const auto     read = [](const fs::path& p)
        {
            std::ifstream      in{p, std::ios::binary};
            std::ostringstream buffer;
            buffer << in.rdbuf();
            return buffer.str();
        };
        {
            std::ofstream out{final_path, std::ios::binary};
            out << "old";
        }

        const fs::path tmp = platform::unique_temp_path(final_path);
        cg::test::check(
            r,
            tmp.parent_path() == final_path.parent_path(),
            "tmp lives in the final's directory (same volume)");
        cg::test::check(
            r,
            tmp.filename().generic_string().starts_with("payload.bin.")
                && tmp.filename().generic_string().ends_with(".tmp"),
            "tmp name is <final>.<pid>.<nonce>.tmp");
        cg::test::check(
            r,
            tmp != platform::unique_temp_path(final_path),
            "successive temp names are unique");

        {
            std::ofstream out{tmp, std::ios::binary};
            out << "new";
        }
        cg::test::check(
            r,
            platform::atomic_replace(tmp, final_path),
            "atomic replace succeeds");
        cg::test::check(
            r,
            !fs::exists(tmp, ec) && read(final_path) == "new",
            "final holds the replacement and tmp is gone");

        // 缺 tmp：直接失败，不碰目标。
        cg::test::check(
            r,
            !platform::atomic_replace(dir / "missing.tmp", final_path),
            "missing tmp reports failure");
        cg::test::check(
            r,
            read(final_path) == "new",
            "failed replace keeps the old final");

        // 目标被占用打开：Windows 拒绝替换并保住旧
        // 字节；POSIX 的 rename(2) 对打开的读取者仍成功。
        {
            std::ofstream out{final_path, std::ios::binary | std::ios::trunc};
            out << "held";
        }
        const fs::path tmp2 = platform::unique_temp_path(final_path);
        {
            std::ofstream out{tmp2, std::ios::binary};
            out << "replacement";
        }
        {
            std::ifstream hold{final_path, std::ios::binary};
            const bool    replaced = platform::atomic_replace(tmp2, final_path);
#if defined(_WIN32)
            cg::test::check(r, !replaced, "open target blocks replacement");
            cg::test::check(
                r,
                read(final_path) == "held",
                "failed replacement preserves the old bytes");
            cg::test::check(
                r,
                fs::exists(tmp2, ec),
                "failed replacement leaves the tmp for the caller");
            fs::remove(tmp2, ec);
#else
            cg::test::check(
                r,
                replaced,
                "POSIX rename replaces while a reader is open");
            cg::test::check(
                r,
                read(final_path) == "replacement",
                "POSIX reader sees the new bytes");
#endif
        }

        // 清单式写入：完整序列化后原子替换。
        const fs::path manifest = dir / "manifest.json";
        cg::test::check(
            r,
            platform::atomic_write_file(manifest, "{\"a\":1}"),
            "atomic write succeeds");
        cg::test::check(
            r,
            read(manifest) == "{\"a\":1}",
            "atomic write lands the complete payload");
        fs::remove_all(dir, ec);
    }

    // 长路径：final 本身仍 < MAX_PATH，但 tmp 后缀把它推过 260。Windows 上
    // CRT/ofstream 与未加 `\\?\` 的 Win32 调用都会失败；原子助手必须用扩展
    // 长度路径完成写入、替换与失败清理。
    void atomic_long_path(R& r)
    {
        namespace fs = std::filesystem;
        namespace platform = cg::util::platform;
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path base =
            fs::temp_directory_path() / ("ccg-long-" + std::to_string(stamp));
        std::error_code ec;
        fs::create_directories(base, ec);
        if (ec)
        {
            cg::test::check(r, true, "long path test skipped: no base dir");
            return;
        }
        fs::path deep = base;
        // 目标：deep ≈ 251、final ≈ 253（< 260），tmp ≈ 270（> 260）。
        while (deep.generic_string().size() + 6 < 252)
        {
            deep /= "level";
        }
        fs::create_directories(deep, ec);
        if (ec)
        {
            cg::test::check(r, true, "long path test skipped: deep tree");
            fs::remove_all(base, ec);
            return;
        }
        const fs::path final_path = deep / "f";
        const fs::path tmp = platform::unique_temp_path(final_path);
        const bool     straddles = final_path.generic_string().size() < 260
                                   && tmp.generic_string().size() > 260;
        if (!straddles)
        {
            cg::test::check(
                r,
                true,
                "long path test skipped: fixture does not straddle "
                "MAX_PATH on this temp root");
            fs::remove_all(base, ec);
            return;
        }
        cg::test::check(
            r,
            platform::atomic_write_file(final_path, "payload"),
            "atomic write succeeds when tmp exceeds MAX_PATH");
        {
            std::ifstream      in{final_path, std::ios::binary};
            std::ostringstream buffer;
            buffer << in.rdbuf();
            cg::test::check(
                r,
                buffer.str() == "payload",
                "payload landed through the extended-length path");
        }
        platform::remove_file(final_path);
        cg::test::check(
            r,
            !fs::exists(final_path, ec),
            "remove_file handles the long final path");
        fs::remove_all(base, ec);
    }

    // FileLock：独占创建、活持有者排斥，与孤儿
    // 恢复。CREATE_NEW 与第一次 WriteFile 之间崩溃会留下
    // 零字节锁；恢复绝不依赖从未写出的内容，
    // 否则摘要积压永远卡死。
    void file_lock_cases(R& r)
    {
        namespace fs = std::filesystem;
        namespace platform = cg::util::platform;
        using platform::try_acquire_file_lock;
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path  dir = fs::temp_directory_path()
                              / ("ccg-filelock-" + std::to_string(stamp));
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec)
        {
            cg::test::check(r, true, "file lock test skipped: no base dir");
            return;
        }
        const fs::path lock = dir / "flush.lock";
        const auto     write_lock = [&](const std::string& text)
        {
            std::ofstream out{lock, std::ios::binary | std::ios::trunc};
            out << text;
        };
        // 锁内容时间戳与被测锁实现同源：cg::util::cstd::epoch_ms 单点。

        {
            const auto first = try_acquire_file_lock(lock, "first", 60'000);
            cg::test::check(r, first.held(), "first contender takes the lock");
            const auto second = try_acquire_file_lock(lock, "second", 60'000);
            cg::test::check(
                r,
                !second.held(),
                "a live holder excludes a second contender");
        }
        cg::test::check(
            r,
            !fs::exists(lock, ec),
            "release removes the lock file");

        write_lock(std::format(
            "999999999\n{}\nstale\n",
            cg::util::cstd::epoch_ms()));
        {
            const auto dead = try_acquire_file_lock(lock, "dead", 60'000);
            cg::test::check(
                r,
                dead.held(),
                "a dead holder's lock is reclaimed");
        }
        cg::test::check(
            r,
            !fs::exists(lock, ec),
            "reclaimed lock is re-released");

        write_lock(
            std::format(
                "{}\n{}\nlive\n",
                platform::current_pid(),
                cg::util::cstd::epoch_ms() + 60'000));
        {
            const auto live = try_acquire_file_lock(lock, "live", 60'000);
            cg::test::check(
                r,
                !live.held(),
                "a live pid's lock is never reclaimed");
        }
        platform::remove_file(lock);

        // pid 不可解析但时间戳可解析且已过期。
        write_lock(std::format(
            "\n{}\nage\n",
            cg::util::cstd::epoch_ms() - 120'000));
        {
            const auto aged = try_acquire_file_lock(lock, "aged", 60'000);
            cg::test::check(
                r,
                aged.held(),
                "an unparseable pid with an old stamp is reclaimed");
        }
        cg::test::check(r, !fs::exists(lock, ec), "aged lock is re-released");

        // 空锁：CREATE_NEW / WriteFile 崩溃窗口。把 mtime
        // 回拨同时也覆盖 POSIX 的保守分支。
        write_lock("");
        fs::last_write_time(
            lock,
            fs::file_time_type::clock::now() - std::chrono::seconds{120},
            ec);
        ec.clear();
        {
            const auto empty = try_acquire_file_lock(lock, "empty", 60'000);
            cg::test::check(
                r,
                empty.held(),
                "an empty lock is reclaimed instead of wedging the backlog");
        }
        cg::test::check(
            r,
            !fs::exists(lock, ec),
            "empty-lock reclaim cleans up");

#if !defined(_WIN32)
        // POSIX：新出现的空锁可能正处在一个活
        // 创建者的创建/写入窗口（无共享屏障），所以只靠 mtime 守。
        write_lock("");
        {
            auto fresh = try_acquire_file_lock(lock, "fresh", 60'000);
            cg::test::check(
                r,
                !fresh.held(),
                "a fresh empty lock is not stolen on POSIX");
        }
        platform::remove_file(lock);
#endif

        // 并发竞争者：独占创建恰好选出
        // 一个赢家，只要尝试相互交叠（赢家持有到每个
        // 其它线程都试过为止）。
        {
            constexpr int            contenders = 8;
            std::atomic<int>         winners{0};
            std::atomic<int>         losers{0};
            std::vector<std::thread> threads;
            threads.reserve(contenders);
            for (int i = 0; i < contenders; ++i)
            {
                threads.emplace_back(
                    [&]
                    {
                        const auto held =
                            try_acquire_file_lock(lock, "race", 60'000);
                        if (!held.held())
                        {
                            ++losers;
                            return;
                        }
                        ++winners;
                        const auto deadline = std::chrono::steady_clock::now()
                                              + std::chrono::seconds{10};
                        while (losers.load() < contenders - 1
                               && std::chrono::steady_clock::now() < deadline)
                        {
                            std::this_thread::yield();
                        }
                    });
            }
            for (auto& thread : threads)
            {
                thread.join();
            }
            cg::test::check(
                r,
                winners.load() == 1,
                "exactly one concurrent contender wins the lock");
        }
        cg::test::check(
            r,
            !fs::exists(lock, ec),
            "the winning contender releases the lock");

        fs::remove_all(dir, ec);
    }

    const cg::test::Registrar reg_file_lock{"util.file_lock", &file_lock_cases};
    const cg::test::Registrar reg_fnv{"util.fnv1a64", &fnv1a64_known_vectors};

    /**
     * @brief sha256_hex/sha256_file_hex 的已知向量对照与文件形态一致性。
     *
     * @internal 向量取 FIPS 180-4 的三例标准消息（空串/abc/448 位消息）；
     *           文件封装与内存形态必须对同一字节产出同一哈希——这是
     *           sha256_file_hex 流式化改造的行为锚点。
     */
    void sha256_known_vectors(R& r)
    {
        const auto hex_of = [](std::string_view s)
        { return cg::util::sha256_hex(std::as_bytes(std::span{s})); };
        cg::test::check(
            r,
            hex_of("")
                == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
            "sha256 of the empty string");
        cg::test::check(
            r,
            hex_of("abc")
                == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "sha256 of abc");
        cg::test::check(
            r,
            hex_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
                == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
            "sha256 of the 448-bit message block message");
        // 文件封装：同一字节（含内嵌 NUL 与高位字节）经磁盘往返后哈希一致。
        const std::string payload =
            "streaming hash contract sample\x00\x01\x7f";
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch();
        const std::filesystem::path tmp =
            std::filesystem::temp_directory_path()
            / ("ccg-sha256-" + std::to_string(stamp.count()) + ".bin");
        {
            std::ofstream out{tmp, std::ios::binary};
            out.write(
                payload.data(),
                static_cast<std::streamsize>(payload.size()));
        }
        cg::test::check(
            r,
            cg::util::sha256_file_hex(tmp.string()) == hex_of(payload),
            "sha256_file_hex matches the in-memory hash");
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
    }

    const cg::test::Registrar reg_sha256{
        "util.sha256_vectors",
        &sha256_known_vectors};
    const cg::test::Registrar reg_atomic{
        "util.atomic_replace",
        &atomic_replace_cases};
    const cg::test::Registrar reg_long_path{
        "util.atomic_long_path",
        &atomic_long_path};
    const cg::test::Registrar reg_qualified{
        "util.split_join_qualified",
        &qualified_round_trip};
    const cg::test::Registrar reg_norm_path{"util.norm_path", &norm_path_cases};
    const cg::test::Registrar reg_interner{"util.interner", &interner_behavior};

    // is_project_file：门控捕获刷新联动的工程/属性文件分类。
    // 清单里每个扩展名都要命中（ASCII 折叠——
    // 漏掉一个 .VCXPROJ 事件会静默切断整条联动），而
    // 语料文件或长相相似者不得命中。
    void is_project_file_cases(R& r)
    {
        using cg::util::is_project_file;
        for (const std::string_view p :
             {"a.sln",
              "p/App.vcxproj",
              "s/Shared.vcxitems",
              "Dir/Props.props",
              "x/Build.targets",
              "proj/Filters.filters"})
        {
            cg::test::check(
                r,
                is_project_file(p),
                std::format("'{}' is a project file", p));
        }
        cg::test::check(
            r,
            is_project_file("E:/SRC/P.VCXPROJ"),
            "uppercase extension still matches (ASCII fold)");
        cg::test::check(
            r,
            is_project_file("p\\App.Vcxproj"),
            "backslash spelling and mixed case still match");
        for (const std::string_view p :
             {"a.cpp",
              "a.hpp",
              "a.ixx",
              "a.cppm",
              "makefile",
              "notes.props.txt",
              "backup.vcxproj.bak",
              "vcxproj",
              "project.slnx"})
        {
            cg::test::check(
                r,
                !is_project_file(p),
                std::format("'{}' is not a project file", p));
        }
        cg::test::check(
            r,
            !is_project_file(""),
            "empty path is not a project file");
    }

    const cg::test::Registrar reg_is_project_file{
        "util.is_project_file",
        &is_project_file_cases};

    // collapse_dot_segments：clang 经 include 拼写报告头文件位置
    // （"a/../b/x.h"）；光标路径在变成文件身份前必须折叠。
    // 本用例类回归那个把每个 ".." 拼写的带引号 include
    // 搅成堆噪声的悬空视图缺陷。
    void collapse_dot_segments_cases(R& r)
    {
        using cg::util::collapse_dot_segments;
        cg::test::check(
            r,
            collapse_dot_segments("x:/ccg-fake/proj/src/../inc/animals.hpp")
                == "x:/ccg-fake/proj/inc/animals.hpp",
            "absolute path: src/.. cancels");
        cg::test::check(
            r,
            collapse_dot_segments(R"(X:\ccg-fake\proj\src\..\inc\a.hpp)")
                == "x:/ccg-fake/proj/inc/a.hpp",
            "backslash spelling collapses after norm_path");
        cg::test::check(
            r,
            collapse_dot_segments("e:/a/b/c/../../d/e.hpp") == "e:/a/d/e.hpp",
            "chained .. segments cancel pairwise");
        cg::test::check(
            r,
            collapse_dot_segments("../x.hpp") == "../x.hpp",
            "leading climb-out survives");
        cg::test::check(
            r,
            collapse_dot_segments("../..") == "../..",
            "leading climb-outs survive uncancelled");
        cg::test::check(
            r,
            collapse_dot_segments("e:/x/../..") == "",
            "a drive letter is an ordinary lexical segment: a "
            "climb-out past it empties the stack (unreachable for "
            "real clang paths)");
        cg::test::check(
            r,
            collapse_dot_segments("e:/a/./b.hpp") == "e:/a/b.hpp",
            "single-dot segment drops");
        cg::test::check(
            r,
            collapse_dot_segments("e:/a..b/c.hpp") == "e:/a..b/c.hpp",
            "dots inside a name are not a .. segment");
        cg::test::check(
            r,
            collapse_dot_segments("e:/plain/dir/x.hpp") == "e:/plain/dir/x.hpp",
            "dot-free path returns normalized unchanged");
        cg::test::check(
            r,
            collapse_dot_segments("") == "",
            "empty path stays empty");
    }

    const cg::test::Registrar reg_collapse{
        "util.collapse_dot_segments",
        &collapse_dot_segments_cases};

// 下方故意制造的故障不得被 ASAN 插桩（消毒器
// 自己的写检查会在平台守卫看到故障前
// 报告并终止）；MSVC 构建无需该属性。
#if defined(__clang__)
#define CCG_TEST_NO_ASAN [[clang::no_sanitize("address")]]
#else
#define CCG_TEST_NO_ASAN
#endif

    // ASAN 注意事项：插桩运行默认拦截 SEGV
    // （handle_segv=1），会在平台守卫介入前对被守卫的
    // NULL 写报告并终止。本可执行文件的 ctest 注册
    // 设了 ASAN_OPTIONS=handle_segv=0（非 ASAN 构建无视
    // 该变量）；在 ASAN 下手工跑测试二进制时请自行导出
    // 该选项。内存错误报告两种情况下都不受影响。

    CCG_TEST_NO_ASAN void deliberate_null_store()
    {
        // volatile NULL 写：故意制造硬件故障。能走到
        // 函数末尾意味着守卫没接住。
        volatile int* p = nullptr;
        // NOLINTNEXTLINE(clang-analyzer-core.NullDereference)
        // 空指针存储是刻意的硬件故障：本函数验证崩溃防护生效，解引用必然且应当失败
        *p = 1;
    }

    void crash_guard_cases(R& r)
    {
        // 干净调用放行且其效果可见。
        volatile int sink = 0;
        const bool   clean = cg::util::guard::run_guarded([&] { sink = 1; });
        cg::test::check(
            r,
            clean && sink == 1,
            "crash guard passes a clean call through");

        // 被守卫的故障返回 false（绝不抛出、绝不杀死
        // 进程），调用点之后的代码继续跑。
        // reached_fault 必须是 volatile：平台守卫在处理器处
        // 恢复，跳过末尾的 "reached_fault = false" 写入，但
        // 没有优化器建模那条恢复边——thin-LTO 内联掉
        // 守卫机械后，开头的 "true" 写入看着像死存储
        // （同一变量、中间无读）而被删掉，静默
        // 翻转本检查，而守卫本身还在正常工作。
        volatile bool reached_fault = false;
        const bool    survived = !cg::util::guard::run_guarded(
            [&]
            {
                reached_fault = true;
                deliberate_null_store();
                reached_fault = false; // 故障触发时这行不可达
            });
        cg::test::check(
            r,
            survived && reached_fault,
            "crash guard swallows a hardware fault and reports it");

        // 守卫机械在故障后立即可复用。
        const bool again = cg::util::guard::run_guarded([&] { sink = 2; });
        cg::test::check(
            r,
            again && sink == 2,
            "crash guard reusable after a fault");
    }

    const cg::test::Registrar reg_guard{"util.crash_guard", &crash_guard_cases};

    // 定时子进程执行（util.platform）：测试二进制以特殊的
    // "sleep 子进程"模式重执行自身，同一闸门因此在
    // Windows 与 POSIX 上都跑。完成路径必须捕获两个流与退出
    // 码；超时路径必须分离（started、timed_out、无退出码）
    // 且不杀子进程。
    void timed_exec_cases(R& r)
    {
        namespace platform = cg::util::platform;
        const std::filesystem::path exe = platform::executable_path();
        cg::test::check(r, !exe.empty(), "the test binary path resolves");
        if (exe.empty())
        {
            return;
        }
        const platform::TimedExecResult quick = platform::capture_exec_timed(
            exe,
            {"--ccg-test-sleep", "5"},
            std::chrono::milliseconds{30'000});
        cg::test::check(
            r,
            quick.started && !quick.timed_out,
            "timed exec waits out a short child");
        cg::test::check(
            r,
            quick.exit_code == 7,
            "timed exec returns the child exit code");
        cg::test::check(
            r,
            quick.out.find("child-ok") != std::string::npos,
            "timed exec captures stdout");
        cg::test::check(
            r,
            quick.err.find("child-err") != std::string::npos,
            "timed exec captures stderr");
        // HIGH 回归守卫：子进程继承的句柄集必须
        // 收窄到三个重定向文件。否则分离的更新
        // 子进程一直握着宿主的 stdio 管道，hook 的调用者
        // 要等后台刷新结束才见得到 EOF。
        cg::test::check(
            r,
            quick.restricted_handles,
            "timed exec restricts the child's inherited handles");

        const platform::TimedExecResult slow = platform::capture_exec_timed(
            exe,
            {"--ccg-test-sleep", "1500"},
            std::chrono::milliseconds{50});
        cg::test::check(
            r,
            slow.started && slow.timed_out,
            "timed exec gives up waiting at the deadline");
        cg::test::check(
            r,
            slow.restricted_handles,
            "the detached child's inherited handles stay restricted");
        cg::test::check(
            r,
            slow.exit_code == -1,
            "a timed-out child reports no exit code");
        cg::test::check(
            r,
            slow.out.empty() && slow.err.empty(),
            "the timeout path returns no half-read output");

        // POSIX 上 fork 先成功、execv 后失败；exec 时关闭的哨兵
        // 必须把它变成 started=false，而非幻影的
        // "子进程跑过并以 127 退出"。Windows 上 CreateProcess 此处直接失败。
        const platform::TimedExecResult missing = platform::capture_exec_timed(
            std::filesystem::path{"ccg-no-such-executable-for-tests"},
            {"arg"},
            std::chrono::milliseconds{5'000});
        cg::test::check(
            r,
            !missing.started && !missing.timed_out && missing.exit_code == -1,
            "an exec failure is reported as not started");
    }

    const cg::test::Registrar reg_timed{"util.timed_exec", &timed_exec_cases};

    // 配置化子进程捕获原语 run_child 的行为测试。合一前的四份
    // 骨架（驱动器讯问、vswhere 讯问、被包装 Rebuild、wrap 透传）全部
    // 刻意差异参数化进了这一个原语；本套件按四种配置逐路钉行为——
    // 捕获内容/派生失败/超时终止/环境块/透传，外加并发排水的死锁回归
    // （先灌 128 KiB stderr 再写唯一一行 stdout，顺序排水在此挂死）。
    void run_child_cases(R& r)
    {
#if defined(_WIN32)
        namespace platform = cg::util::platform;
        const std::filesystem::path exe = platform::executable_path();
        cg::test::check(r, !exe.empty(), "the test binary path resolves");
        if (exe.empty())
        {
            return;
        }

        // 捕获形态（vswhere 讯问配置）：两路捕获、空 stdin、无限等待。
        {
            platform::ChildRunSpec spec;
            spec.cmdline =
                std::format(L"\"{}\" --ccg-test-sleep 5", exe.wstring());
            spec.stdin_mode = platform::ChildStdin::NullHandle;
            const platform::ChildRunResult run = platform::run_child(spec);
            cg::test::check(
                r,
                run.pipes_ok && run.started && !run.timed_out,
                "the capture shape starts and waits out the child");
            cg::test::check(
                r,
                run.exit_code == 7,
                "the capture shape returns the child exit code");
            cg::test::check(
                r,
                run.out.find("child-ok") != std::string::npos
                    && run.err.find("child-err") != std::string::npos,
                "the capture shape captures both streams");
        }

        // 派生失败路径：不存在的可执行必须报 started=false 且携带 OS
        // 错误码（wrap/被包装 Rebuild 的调用方把它编进各自的错误报告）。
        {
            platform::ChildRunSpec spec;
            spec.application =
                std::wstring{L"ccg-no-such-run-child-target.exe"};
            spec.cmdline = L"\"ccg-no-such-run-child-target.exe\" --probe";
            const platform::ChildRunResult run = platform::run_child(spec);
            cg::test::check(
                r,
                run.pipes_ok && !run.started && run.spawn_error != 0,
                "a spawn failure reports not started with the OS "
                "error");
        }

        // 超时终止形态（驱动器讯问配置）：EofPipe 喂空 stdin + kill_after
        // 到点终止，终止码是 TerminateProcess 的约定值 WAIT_TIMEOUT(258)。
        {
            platform::ChildRunSpec spec;
            spec.cmdline =
                std::format(L"\"{}\" --ccg-test-sleep 8000", exe.wstring());
            spec.stdin_mode = platform::ChildStdin::EofPipe;
            spec.kill_after = std::chrono::milliseconds{250};
            const auto begin = std::chrono::steady_clock::now();
            const platform::ChildRunResult run = platform::run_child(spec);
            const auto                     elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - begin);
            cg::test::check(
                r,
                run.started && run.timed_out,
                "the kill_after shape gives up at the deadline");
            cg::test::check(
                r,
                run.exit_code == 258,
                "the terminated child reports the WAIT_TIMEOUT "
                "exit code");
            cg::test::check(
                r,
                elapsed < std::chrono::milliseconds{4'000},
                "the timeout path returns promptly");
        }

        // 并发排水死锁回归：洪水子进程先写满 128 KiB stderr 再写它仅有
        // 的一行 stdout；顺序排空（先 stdout 后 stderr）在此死锁、测试
        // 挂到超时，两路各一条排水线程则完整收下两路。
        {
            platform::ChildRunSpec spec;
            spec.cmdline =
                std::format(L"\"{}\" --ccg-test-flood", exe.wstring());
            const platform::ChildRunResult run = platform::run_child(spec);
            cg::test::check(
                r,
                run.started && run.exit_code == 0
                    && run.out.find("flood-done") != std::string::npos,
                "concurrent drains survive a 128 KiB stderr flood");
            cg::test::check(
                r,
                run.err.size() == 128 * 1024,
                "the flooded stderr is captured in full");
        }

        // 环境块定制形态（被包装 Rebuild 配置）：子进程看到定制块里的
        // 变量（块整体替换继承环境；SystemRoot 给 CRT、PATH 给测试二进制
        // 可能依赖的运行库 DLL——clang 树的测试进程就靠 PATH 上的
        // msys64/clang64/bin 装载）。
        {
            std::wstring block;
            const auto   append_var = [&](const char* name)
            {
                if (const auto v = cg::util::cstd::get_env(name))
                {
                    block += platform::widen_utf8(std::string{name} + "=" + *v);
                    block.push_back(L'\0');
                }
            };
            append_var("SystemRoot");
            append_var("PATH");
            block += L"CCG_RUN_CHILD_PROBE=delivered-by-block";
            block.push_back(L'\0');
            block.push_back(L'\0'); // 双 NUL 结尾的 UNICODE 环境块
            platform::ChildRunSpec spec;
            spec.cmdline = std::format(
                L"\"{}\" --ccg-test-echo-env CCG_RUN_CHILD_PROBE",
                exe.wstring());
            spec.env_block = std::move(block);
            spec.stdin_mode = platform::ChildStdin::ParentHandle;
            spec.create_no_window = false;
            const platform::ChildRunResult run = platform::run_child(spec);
            cg::test::check(
                r,
                run.started && run.exit_code == 0
                    && run.out.find("delivered-by-block") != std::string::npos,
                "a custom environment block reaches the child");
        }

        // 透传形态（wrap 配置）：不建管道、不设 STARTF_USESTDHANDLES，
        // 退出码原样传播、两路输出零捕获。
        {
            platform::ChildRunSpec spec;
            spec.cmdline =
                std::format(L"\"{}\" --ccg-test-sleep 5", exe.wstring());
            spec.inherit_parent_stdio = true;
            spec.create_no_window = false;
            const platform::ChildRunResult run = platform::run_child(spec);
            cg::test::check(
                r,
                run.started && run.exit_code == 7,
                "the passthrough shape propagates the child exit "
                "code");
            cg::test::check(
                r,
                run.out.empty() && run.err.empty(),
                "the passthrough shape captures nothing");
        }
#else
        cg::test::check(r, true, "run_child is Windows-only (skip)");
#endif
    }

    const cg::test::Registrar reg_run_child{"util.run_child", &run_child_cases};

    // HIGH 回归守卫：分离的子进程不得继承宿主的
    // stdout 管道。测试二进制自己在子进程里扮演宿主；
    // harness 持有管道读端。若孙进程泄漏管道
    // 写端，capture_exec 要等 sleep 结束才能见到 EOF。
    void detached_child_handle_isolation(R& r)
    {
        namespace platform = cg::util::platform;
        namespace fs = std::filesystem;
        const fs::path exe = platform::executable_path();
        cg::test::check(r, !exe.empty(), "the test binary path resolves");
        if (exe.empty())
        {
            return;
        }
        const fs::path  marker = fs::path{"ccg-handle-isolation-marker.txt"};
        std::error_code ec;
        fs::remove(marker, ec);
        std::string out;
        int         code = -1;
        const auto  start = std::chrono::steady_clock::now();
        const bool  ran = platform::capture_exec(
            exe,
            {"--ccg-test-spawn-sleeper", "1500", marker.generic_string()},
            out,
            code);
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);
        cg::test::check(
            r,
            ran && code == 0 && out.find("host-ok") != std::string::npos,
            "the host helper runs to completion");
        cg::test::check(
            r,
            elapsed < std::chrono::milliseconds{500},
            "the host pipe reaches EOF well before the detached "
            "child finishes");
        cg::test::check(
            r,
            !fs::exists(marker, ec),
            "the detached child is still running at EOF");
        bool finished = false;
        for (int attempt = 0; attempt < 40; ++attempt)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
            if (fs::exists(marker, ec))
            {
                finished = true;
                break;
            }
        }
        cg::test::check(
            r,
            finished,
            "the detached child survived the host's exit and "
            "finished its work");
        fs::remove(marker, ec);
    }

    const cg::test::Registrar reg_handles{
        "util.detached_handle_isolation",
        &detached_child_handle_isolation};

    // trim：唯一的共享空白剥离器（空格、tab、CR、LF），
    // 取代了三份各自为政的本地副本（msbuild 行扫描、
    // 驱动输出、类型拼写）。
    void trim_cases(R& r)
    {
        cg::test::check(
            r,
            cg::util::trim("  a b \t\r\n") == "a b",
            "strips leading/trailing whitespace of every kind");
        cg::test::check(
            r,
            cg::util::trim("\t x \t") == "x",
            "strips tabs on both sides");
        cg::test::check(
            r,
            cg::util::trim("no-changes") == "no-changes",
            "already-trimmed text passes through");
        cg::test::check(
            r,
            cg::util::trim("   \t\r\n") == "",
            "all-whitespace input yields the empty string");
        cg::test::check(
            r,
            cg::util::trim("") == "",
            "empty input yields the empty string");
        cg::test::check(
            r,
            cg::util::trim(" a  b ") == "a  b",
            "interior whitespace is preserved");
    }

    // ascii_lower：唯一的共享 ASCII 折叠（与区域设置无关的字节），
    // 取代了路径折叠、方言旗标与 db
    // 文件路径匹配器里的本地副本。
    void ascii_lower_cases(R& r)
    {
        cg::test::check(
            r,
            cg::util::ascii_lower("AbC") == "abc",
            "ASCII uppercase folds to lowercase");
        cg::test::check(
            r,
            cg::util::ascii_lower("path/MI X.cpp") == "path/mi x.cpp",
            "mixed-case path folds");
        const std::string raw{"\xC3\xBC"};
        cg::test::check(
            r,
            cg::util::ascii_lower(raw) == raw,
            "non-ASCII bytes pass through unchanged");
        cg::test::check(
            r,
            cg::util::ascii_lower("") == "",
            "empty input yields the empty string");
    }

    const cg::test::Registrar reg_trim{"util.trim", &trim_cases};
    const cg::test::Registrar reg_ascii_lower{
        "util.ascii_lower",
        &ascii_lower_cases};

    // quote_for_command_line：MSVC CRT argv 规则的合成方向，
    // tokenize_command 的镜像（唯一共享副本，取代
    // driver-spawn / wrap / platform-capture 三份）。
    void quote_for_command_line_cases(R& r)
    {
        cg::test::check(
            r,
            cg::util::platform::quote_for_command_line(L"plain") == L"plain",
            "plain token needs no quotes");
        cg::test::check(
            r,
            cg::util::platform::quote_for_command_line(L"") == L"\"\"",
            "empty token becomes an empty quoted argument");
        cg::test::check(
            r,
            cg::util::platform::quote_for_command_line(L"a b") == L"\"a b\"",
            "spaced token is wrapped");
        cg::test::check(
            r,
            cg::util::platform::quote_for_command_line(L"a\\b\"c")
                == L"\"a\\b\\\"c\"",
            "backslash before a non-quote character stays single");
        cg::test::check(
            r,
            cg::util::platform::quote_for_command_line(L"a\\\"c")
                == L"\"a\\\\\\\"c\"",
            "backslashes double before an embedded quote");
        cg::test::check(
            r,
            cg::util::platform::quote_for_command_line(L"dir\\") == L"dir\\",
            "unquoted token keeps its trailing backslash as-is");
        cg::test::check(
            r,
            cg::util::platform::quote_for_command_line(L"dir x\\")
                == L"\"dir x\\\\\"",
            "trailing backslash run doubles inside quotes");
        const std::wstring tokens[] = {
            L"",
            L"plain",
            L"a b",
            L"a\\b",
            L"a\\b\"c",
            L"trailing\\",
            L"tab\there"};
        for (const std::wstring& token : tokens)
        {
            const std::wstring quoted =
                cg::util::platform::quote_for_command_line(token);
            const std::string narrow{quoted.begin(), quoted.end()};
            const auto        reparsed = cg::util::tokenize_command(narrow);
            const std::string want{token.begin(), token.end()};
            cg::test::check(
                r,
                reparsed.size() == 1 && reparsed[0] == want,
                std::format("quote round-trip for '{}'", want));
        }
    }

    const cg::test::Registrar reg_quote_cmdline{
        "util.quote_for_command_line",
        &quote_for_command_line_cases};

    // widen_utf8/narrow_utf8：唯一的共享 UTF-8 <-> UTF-16 边界对
    // （取代 wrap 侧与 platform lambda 的副本）。
    void utf8_widen_narrow_cases(R& r)
    {
        const std::string  utf8{"path/\xC3\xBC n\xC3\xAF code-\xCE\xA9"};
        const std::wstring wide = cg::util::platform::widen_utf8(utf8);
        cg::test::check(r, !wide.empty(), "multi-byte UTF-8 widens");
        cg::test::check(
            r,
            cg::util::platform::narrow_utf8(wide) == utf8,
            "widen/narrow round-trips non-ASCII text");
        cg::test::check(
            r,
            cg::util::platform::widen_utf8("").empty(),
            "empty input widens to empty");
        cg::test::check(
            r,
            cg::util::platform::narrow_utf8(L"").empty(),
            "empty input narrows to empty");
        cg::test::check(
            r,
            cg::util::platform::widen_utf8("plain") == L"plain",
            "ASCII widens verbatim");
    }

    const cg::test::Registrar reg_widen_narrow{
        "util.widen_narrow_utf8",
        &utf8_widen_narrow_cases};

    // ascii_iequals / ascii_icontains：ASCII 大小写不敏感比较的共享单点
    // （取代 msbuild 管线与 db 检索的本地副本）。
    void ascii_compare_cases(R& r)
    {
        using cg::util::ascii_icontains;
        using cg::util::ascii_iequals;
        cg::test::check(
            r,
            ascii_iequals("Debug|x64", "debug|X64"),
            "mixed case compares equal");
        cg::test::check(
            r,
            !ascii_iequals("ab", "abc"),
            "different lengths never equal");
        cg::test::check(
            r,
            !ascii_iequals("ab", "ac"),
            "same length different bytes unequal");
        cg::test::check(r, ascii_iequals("", ""), "empty equals empty");
        const std::string raw{"\xC3\xBC"};
        cg::test::check(
            r,
            !ascii_iequals(raw, "\xC3\xBD"),
            "non-ASCII bytes compare by value (no Unicode folding)");
        cg::test::check(
            r,
            ascii_icontains("TaskRecord.h", "task"),
            "case-insensitive substring hits");
        cg::test::check(
            r,
            ascii_icontains("anything", ""),
            "empty needle is contained everywhere");
        cg::test::check(
            r,
            !ascii_icontains("ab", "abc"),
            "needle longer than haystack misses");
        cg::test::check(
            r,
            ascii_icontains("pretask", "TASK"),
            "interior case-insensitive match still hits");
        cg::test::check(
            r,
            ascii_icontains("pretask", "pre")
                && ascii_icontains("pretask", "ASK"),
            "prefix and suffix match case-insensitively");
    }

    const cg::test::Registrar reg_ascii_cmp{
        "util.ascii_compare",
        &ascii_compare_cases};

    // to_forward_slashes：纯分隔符统一（不动大小写、不折叠连跑）——与
    // norm_path 的存储规范化语义严格区分。
    void to_forward_slashes_cases(R& r)
    {
        using cg::util::to_forward_slashes;
        cg::test::check(
            r,
            to_forward_slashes(R"(E:\Foo\Bar\a.hpp)") == "E:/Foo/Bar/a.hpp",
            "backslashes convert to forward slashes");
        cg::test::check(
            r,
            to_forward_slashes("E://Foo\\\\a") == "E://Foo//a",
            "separator runs convert verbatim, case preserved");
        cg::test::check(r, to_forward_slashes("") == "", "empty stays empty");
    }

    const cg::test::Registrar reg_forward_slashes{
        "util.to_forward_slashes",
        &to_forward_slashes_cases};

    // path_basename：两种分隔符都认的末段提取。
    void path_basename_cases(R& r)
    {
        cg::test::check(
            r,
            cg::util::path_basename("e:/a/b/Cat.hpp") == "Cat.hpp",
            "forward-slash tail");
        cg::test::check(
            r,
            cg::util::path_basename(R"(e:\a\b\Cat.hpp)") == "Cat.hpp",
            "backslash tail");
        const std::string_view whole = cg::util::path_basename("Cat.hpp");
        cg::test::check(
            r,
            whole == "Cat.hpp",
            "no separator is the whole string");
        cg::test::check(
            r,
            cg::util::path_basename("e:/a/b/").empty(),
            "trailing separator yields an empty tail");
    }

    const cg::test::Registrar reg_basename{
        "util.path_basename",
        &path_basename_cases};

    // split / join / push_unique：切分按单字符、拼接按区间、追加保序去重。
    void split_join_push_unique_cases(R& r)
    {
        const auto parts = cg::util::split("a;b;;c", ';');
        cg::test::check(
            r,
            parts.size() == 4 && parts[0] == "a" && parts[1] == "b"
                && parts[2] == "" && parts[3] == "c",
            "split keeps empty segments");
        cg::test::check(
            r,
            cg::util::split("", ';').empty(),
            "empty input splits to nothing");
        cg::test::check(
            r,
            cg::util::split("solo", ';').size() == 1,
            "separator-free input is one segment");

        const std::vector<std::string> words{"alpha", "beta", "gamma"};
        cg::test::check(
            r,
            cg::util::join(words, ", ") == "alpha, beta, gamma",
            "join with a multi-char separator");
        cg::test::check(
            r,
            cg::util::join(words, "") == "alphabetagamma",
            "join with an empty separator concatenates");
        cg::test::check(
            r,
            cg::util::join(std::vector<std::string>{}, "-") == "",
            "join of nothing is empty");
        const auto dotted = cg::util::join(
            std::vector<int>{14, 51, 36231}
                | std::views::transform([](int v)
                                        { return std::to_string(v); }),
            ".");
        cg::test::check(
            r,
            dotted == "14.51.36231",
            "integer segments join via to_string transform");

        std::vector<std::string> sink;
        cg::util::push_unique(sink, std::string{"one"});
        cg::util::push_unique(sink, std::string{"two"});
        cg::util::push_unique(sink, std::string{"one"});
        cg::test::check(
            r,
            sink.size() == 2 && sink[0] == "one" && sink[1] == "two",
            "push_unique keeps first-seen order and drops repeats");
        std::vector<int> nums;
        for (int v : {3, 1, 3, 2, 1})
        {
            cg::util::push_unique(nums, v);
        }
        cg::test::check(
            r,
            nums.size() == 3 && nums[0] == 3 && nums[1] == 1 && nums[2] == 2,
            "push_unique works on non-string element types");
    }

    const cg::test::Registrar reg_split_join{
        "util.split_join_push_unique",
        &split_join_push_unique_cases};

    // strip_cr / for_each_line：逐行消费的单点（CRLF 剥除、getline 语义）。
    void line_iteration_cases(R& r)
    {
        std::string with_cr{"abc\r"};
        cg::util::strip_cr(with_cr);
        cg::test::check(
            r,
            with_cr == "abc",
            "strip_cr removes one trailing CR");
        cg::util::strip_cr(with_cr);
        cg::test::check(
            r,
            with_cr == "abc",
            "strip_cr leaves CR-free text alone");
        std::string no_cr{""};
        cg::util::strip_cr(no_cr);
        cg::test::check(r, no_cr.empty(), "strip_cr on empty is a no-op");

        std::vector<std::string> lines;
        cg::util::for_each_line(
            "a\r\nb\nc",
            [&](std::string_view line) { lines.emplace_back(line); });
        cg::test::check(
            r,
            lines.size() == 3 && lines[0] == "a" && lines[1] == "b"
                && lines[2] == "c",
            "for_each_line strips CR and keeps the unterminated tail");
        lines.clear();
        cg::util::for_each_line(
            "x\n",
            [&](std::string_view line) { lines.emplace_back(line); });
        cg::test::check(
            r,
            lines.size() == 1 && lines[0] == "x",
            "no phantom empty line after a trailing newline");
        lines.clear();
        cg::util::for_each_line(
            "\n",
            [&](std::string_view line) { lines.emplace_back(line); });
        cg::test::check(
            r,
            lines.size() == 1 && lines[0].empty(),
            "a lone newline is one empty line (getline semantics)");
        lines.clear();
        cg::util::for_each_line(
            "",
            [&](std::string_view) { lines.emplace_back(); });
        cg::test::check(r, lines.empty(), "empty text yields no lines");
    }

    const cg::test::Registrar reg_lines{
        "util.line_iteration",
        &line_iteration_cases};

    // parse_bounded：meta/env knob 的"缺失/非数字/非正数回退默认"模板。
    void parse_bounded_cases(R& r)
    {
        using cg::util::parse_bounded;
        cg::test::check(
            r,
            parse_bounded(std::nullopt, 60'000) == 60'000,
            "missing source falls back");
        cg::test::check(
            r,
            parse_bounded(std::optional<std::string>{"1500"}, 60) == 1500,
            "a positive number parses");
        cg::test::check(
            r,
            parse_bounded(std::optional<std::string>{"0"}, 60) == 60,
            "zero falls back (knobs must be positive)");
        cg::test::check(
            r,
            parse_bounded(std::optional<std::string>{"-5"}, 60) == 60,
            "negative falls back");
        cg::test::check(
            r,
            parse_bounded(std::optional<std::string>{"abc"}, 60) == 60,
            "non-numeric falls back");
        cg::test::check(
            r,
            parse_bounded(std::optional<std::string>{""}, 60) == 60,
            "empty string falls back");
        cg::test::check(
            r,
            parse_bounded(std::optional<std::string>{"12abc"}, 60) == 12,
            "numeric prefix parses (atoll-compatible contract)");
    }

    const cg::test::Registrar reg_parse_bounded{
        "util.parse_bounded",
        &parse_bounded_cases};

    // parse_dotted_version：点分数字版本段（MSVC 工具集键）的单点。
    void parse_dotted_version_cases(R& r)
    {
        using cg::util::parse_dotted_version;
        const auto v = parse_dotted_version("14.51.36231");
        cg::test::check(
            r,
            v.size() == 3 && v[0] == 14 && v[1] == 51 && v[2] == 36231,
            "three segments parse");
        const auto w = parse_dotted_version("14.51.36231.7");
        cg::test::check(r, w.size() == 4 && w[3] == 7, "four segments parse");
        cg::test::check(
            r,
            parse_dotted_version("14").size() == 1,
            "single segment parses");
        cg::test::check(
            r,
            parse_dotted_version("").empty(),
            "empty text is rejected");
        cg::test::check(
            r,
            parse_dotted_version("14.").empty(),
            "trailing separator (empty segment) is rejected");
        cg::test::check(
            r,
            parse_dotted_version("14a.2").empty(),
            "non-numeric segment is rejected");
        cg::test::check(
            r,
            parse_dotted_version("14..2").empty(),
            "empty interior segment is rejected");
    }

    const cg::test::Registrar reg_dotted{
        "util.parse_dotted_version",
        &parse_dotted_version_cases};

    // utf8_seq_len / utf8_is_continuation：码点边界微逻辑的单点。
    void utf8_boundary_cases(R& r)
    {
        using cg::util::utf8_is_continuation;
        using cg::util::utf8_seq_len;
        cg::test::check(r, utf8_seq_len('a') == 1, "ASCII lead is 1 byte");
        cg::test::check(
            r,
            utf8_seq_len(static_cast<char>(0xC3)) == 2,
            "two-byte lead");
        cg::test::check(
            r,
            utf8_seq_len(static_cast<char>(0xE4)) == 3,
            "three-byte lead");
        cg::test::check(
            r,
            utf8_seq_len(static_cast<char>(0xF0)) == 4,
            "four-byte lead");
        cg::test::check(
            r,
            utf8_seq_len(static_cast<char>(0x80)) == 1,
            "stray continuation byte counts as one (byte-wise "
            "recovery policy)");
        cg::test::check(
            r,
            utf8_is_continuation(static_cast<char>(0x80)),
            "10xxxxxx is a continuation byte");
        cg::test::check(
            r,
            !utf8_is_continuation('A'),
            "ASCII byte is not a continuation");
        cg::test::check(
            r,
            !utf8_is_continuation(static_cast<char>(0xC3)),
            "lead byte is not a continuation");

        // 长度分类在真实多字节文本上的码点计数（utf8_length 语义的缩影）。
        const std::string text{"a\xC3\xBC\xE4\xB8\xAD\xF0\x9F\x98\x80"};
        std::size_t       codepoints = 0;
        for (std::size_t i = 0; i < text.size();)
        {
            const std::size_t len = utf8_seq_len(text[i]);
            i += len;
            ++codepoints;
        }
        cg::test::check(
            r,
            codepoints == 4 && text.size() == 10,
            "seq lengths walk 1+2+3+4-byte sequences exactly");
    }

    const cg::test::Registrar reg_utf8{
        "util.utf8_boundary",
        &utf8_boundary_cases};

    // string_hash：unordered_map 配 equal_to<> 后的 string_view 异构直查。
    void string_hash_cases(R& r)
    {
        std::unordered_map<
            std::string,
            int,
            cg::util::string_hash,
            std::equal_to<>>
            table;
        table.emplace("e:/a/b.hpp", 1);
        table.emplace("e:/c/d.hpp", 2);
        const std::string_view key{"e:/a/b.hpp"};
        const auto             it = table.find(key);
        cg::test::check(
            r,
            it != table.end() && it->second == 1,
            "string_view lookups hit without building a key copy");
        cg::test::check(
            r,
            table.find(std::string_view{"missing"}) == table.end(),
            "string_view miss is a clean end()");
        cg::test::check(
            r,
            table.contains(key),
            "contains works through the transparent lookup");
    }

    const cg::test::Registrar reg_string_hash{
        "util.string_hash",
        &string_hash_cases};

    // read_text_file：整文件读入单点（二进制口径、缺文件 nullopt）。
    void read_text_file_cases(R& r)
    {
        namespace fs = std::filesystem;
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path tmp = fs::temp_directory_path()
                             / ("ccg-read-" + std::to_string(stamp) + ".txt");
        {
            std::ofstream out{tmp, std::ios::binary};
            out << "line1\r\nline2\x00tail";
        }
        const auto text = cg::util::read_text_file(tmp.string());
        cg::test::check(
            r,
            text.has_value() && *text == "line1\r\nline2\x00tail",
            "binary read keeps CR, LF and embedded NUL verbatim");
        cg::test::check(
            r,
            !cg::util::read_text_file((tmp.string() + ".no-such-file").c_str())
                 .has_value(),
            "missing file returns nullopt");
        std::error_code ec;
        fs::remove(tmp, ec);
    }

    const cg::test::Registrar reg_read_text{
        "util.read_text_file",
        &read_text_file_cases};

    // 扩展名分类单点：五张历史表收编后的集合与折叠语义钉子。
    void source_class_cases(R& r)
    {
        using C = cg::util::SourceFileClass;
        const auto cls = [](std::string_view p)
        { return cg::util::classify_source_path(p); };
        // 分类逐值：折叠对完整路径与裸 token 都成立。
        cg::test::check(
            r,
            cls("e:/src/a.cpp") == C::CppSource,
            "cpp class for a full path");
        cg::test::check(
            r,
            cls("a.CC") == C::CppSource,
            "folding covers uppercase extensions");
        cg::test::check(
            r,
            cls("a.c++") == C::CppSource,
            "c++ spelling folds to the cpp class");
        cg::test::check(r, cls("a.C") == C::CSource, "c folds to the c class");
        cg::test::check(
            r,
            cls("a.ixx") == C::ModuleUnit && cls("a.CPPM") == C::ModuleUnit
                && cls("a.mpp") == C::ModuleUnit
                && cls("a.mxx") == C::ModuleUnit
                && cls("a.c++m") == C::ModuleUnit,
            "module unit class covers all six spellings");
        cg::test::check(
            r,
            cls("a.h") == C::Header && cls("a.HPP") == C::Header,
            "header class folds");
        cg::test::check(
            r,
            cls("a.inc") == C::ForcedInclude,
            "forced-include stub is its own class");
        cg::test::check(
            r,
            cls("a.tlh") == C::ComGenerated && cls("a.tli") == C::ComGenerated,
            "com-generated headers are their own class");
        cg::test::check(
            r,
            cls("a.cpp.txt") == C::None && cls("Makefile") == C::None,
            "non-source tails classify as none");
        // 具名子集的精确集合（各历史表的原集合钉死）。
        cg::test::check(
            r,
            cg::util::is_tu_input_file("x.CPP")
                && cg::util::is_tu_input_file("x.mpp")
                && cg::util::is_tu_input_file("x.c")
                && !cg::util::is_tu_input_file("x.h")
                && !cg::util::is_tu_input_file("x.inc")
                && !cg::util::is_tu_input_file("x.tlh"),
            "tu-input subset is sources+module units only");
        cg::test::check(
            r,
            cg::util::is_indexable_source_file("x.tlh")
                && cg::util::is_indexable_source_file("x.inl")
                && !cg::util::is_indexable_source_file("x.inc")
                && cg::util::is_indexable_source_file("x.cpp"),
            "indexable subset adds headers and com, not .inc");
        cg::test::check(
            r,
            cg::util::is_hook_corpus_file("x.INC")
                && cg::util::is_hook_corpus_file("x.CPP")
                && !cg::util::is_hook_corpus_file("x.tlh")
                && !cg::util::is_hook_corpus_file("x.sln"),
            "hook corpus subset adds .inc, not com or project files");
        cg::test::check(
            r,
            cg::util::is_cpp_source_file("x.cxx")
                && !cg::util::is_cpp_source_file("x.c")
                && !cg::util::is_cpp_source_file("x.h"),
            "cpp-source subset is the four cpp spellings");
        cg::test::check(
            r,
            cg::util::is_module_unit_file("x.IXX")
                && !cg::util::is_module_unit_file("x.cpp"),
            "module-unit subset is the six module spellings");
    }

    const cg::test::Registrar reg_source_class{
        "util.source_file_class",
        &source_class_cases};

    // 路径比较键（比较规范化的具名变体）。
    void path_compare_key_cases(R& r)
    {
        cg::test::check(
            r,
            cg::util::path_compare_key("E:\\Src\\A.CPP") == "e:/src/a.cpp",
            "compare key folds case and separators");
        cg::test::check(
            r,
            cg::util::path_compare_key("e:/src/a.cpp")
                == cg::util::path_compare_key("E:\\SRC\\A.CPP"),
            "both sides fold to the same key");
        cg::test::check(
            r,
            cg::util::path_compare_key("a//b") == "a//b",
            "compare key keeps separator runs (not a storage key)");
        cg::test::check(
            r,
            cg::util::path_compare_key("").empty(),
            "empty input folds to the empty key");
    }

    void worker_pool_cases(R& r)
    {
        namespace pool = cg::util::pool;

        // 工人规模解析单点：请求值生效、作业数截断、零作业保底一人。
        cg::test::check(
            r,
            pool::resolve_worker_count(4, 10) == 4,
            "requested worker count is honored");
        cg::test::check(
            r,
            pool::resolve_worker_count(4, 3) == 3,
            "worker count is capped by the job count");
        cg::test::check(
            r,
            pool::resolve_worker_count(1, 5) == 1,
            "a single requested worker stays one");
        cg::test::check(
            r,
            pool::resolve_worker_count(64, 64) == 64,
            "the upper bound is reachable at equal job count");
        cg::test::check(
            r,
            pool::resolve_worker_count(4, 0) == 1,
            "zero jobs still keep the floor of one worker");
        const int auto_count = pool::auto_worker_count();
        cg::test::check(
            r,
            auto_count >= 1 && auto_count <= 12,
            "auto worker count stays within the calibrated band");
        cg::test::check(
            r,
            pool::resolve_worker_count(0, 64) == auto_count,
            "requested 0 resolves to the auto worker count");

        // N 作业 M 工人：每个作业恰执行一次、工人号落在 [1, M]。
        {
            constexpr std::size_t n = 96;
            std::vector<int>      hits(n, 0);
            std::mutex            hits_mx;
            int                   max_wid = 0;
            int                   min_wid = 99;
            std::mutex            wid_mx;
            pool::PoolRun         spec;
            spec.count = n;
            spec.requested = 4;
            spec.task = [&](std::size_t i, int wid)
            {
                {
                    const std::scoped_lock lock{hits_mx};
                    ++hits[i];
                }
                {
                    const std::scoped_lock lock{wid_mx};
                    max_wid = std::max(max_wid, wid);
                    min_wid = std::min(min_wid, wid);
                }
            };
            pool::run(std::move(spec));
            std::size_t total = 0;
            bool        each_once = true;
            for (int h : hits)
            {
                total += static_cast<std::size_t>(h);
                each_once = each_once && h == 1;
            }
            cg::test::check(
                r,
                total == n && each_once,
                "every job runs exactly once across the pool");
            cg::test::check(
                r,
                min_wid >= 1 && max_wid <= 4,
                "worker ids stay within the requested band");
        }

        // 完成回调在池锁内串行：done 恰为 1..N 的排列，且绝无重入。
        {
            constexpr std::size_t    n = 64;
            std::vector<std::size_t> done_seq;
            std::atomic<int>         reentered{0};
            std::atomic<bool>        in_callback{false};
            pool::PoolRun            spec;
            spec.count = n;
            spec.requested = 6;
            spec.task = [](std::size_t, int) {};
            spec.on_done = [&](std::size_t done)
            {
                // 池的打印锁必须保证同一时刻只有一个线程在回调里：
                // exchange 探测重入即可，无需任何测试侧锁。
                if (in_callback.exchange(true))
                {
                    ++reentered;
                }
                done_seq.push_back(done); // 锁内安全；池外读取
                in_callback.store(false);
            };
            pool::run(std::move(spec));
            cg::test::check(
                r,
                reentered.load() == 0,
                "the completion callback never re-enters");
            bool is_permutation = done_seq.size() == n;
            if (is_permutation)
            {
                std::vector<std::size_t> seen(n, 0);
                for (std::size_t d : done_seq)
                {
                    if (d < 1 || d > n || seen[d - 1] != 0)
                    {
                        is_permutation = false;
                        break;
                    }
                    seen[d - 1] = 1;
                }
            }
            cg::test::check(
                r,
                is_permutation,
                "done counts arrive as an exact 1..N permutation");
        }

        // count==0 空跑：不启线程、不跑任务、不进回调。
        {
            bool          touched = false;
            pool::PoolRun spec;
            spec.count = 0;
            spec.task = [&](std::size_t, int) { touched = true; };
            spec.on_done = [&](std::size_t) { touched = true; };
            pool::run(std::move(spec));
            cg::test::check(
                r,
                !touched,
                "a zero-count run does nothing at all");
        }

        // 单任务退化：无论请求多少工人，作业恰跑一次。
        {
            int           runs = 0;
            pool::PoolRun spec;
            spec.count = 1;
            spec.requested = 8;
            spec.task = [&](std::size_t i, int)
            {
                if (i == 0)
                {
                    ++runs;
                }
            };
            pool::run(std::move(spec));
            cg::test::check(
                r,
                runs == 1,
                "a single job runs exactly once regardless of "
                "the requested worker count");
        }

        // 取活顺序谓词（LPT 形态）：权重大的作业先被领取。
        {
            constexpr std::size_t n = 12;
            std::vector<int>      weight(n, 0);
            for (std::size_t i = 0; i < n; ++i)
            {
                weight[i] = static_cast<int>((n - i) % 5); // 权重交错，非下标序
            }
            std::vector<std::size_t> take_order;
            std::mutex               order_mx;
            pool::PoolRun            spec;
            spec.count = n;
            spec.requested = 3;
            spec.before = [&](std::size_t a, std::size_t b)
            {
                // 全序谓词：权重降序，平局按下标升序。
                if (weight[a] != weight[b])
                {
                    return weight[a] > weight[b];
                }
                return a < b;
            };
            spec.task = [&](std::size_t i, int)
            {
                const std::scoped_lock lock{order_mx};
                take_order.push_back(i);
            };
            pool::run(std::move(spec));
            bool ordered = take_order.size() == n;
            for (std::size_t k = 1; ordered && k < n; ++k)
            {
                const std::size_t prev = take_order[k - 1];
                const std::size_t cur = take_order[k];
                ordered = weight[prev] > weight[cur]
                          || (weight[prev] == weight[cur] && prev < cur);
            }
            cg::test::check(
                r,
                ordered,
                "the before predicate drives the pickup order");
        }

        // 异常纪律（契约=任务体不抛、池不捕获）：任务体把异常面收编为
        // 失败记名（BMI 池任务体的同款纪律），池照常排空全部作业——
        // 一个任务的软失败绝不塌池、不株连同池其它任务。
        {
            constexpr std::size_t n = 30;
            std::atomic<int>      recorded_failures{0};
            std::atomic<int>      completed{0};
            pool::PoolRun         spec;
            spec.count = n;
            spec.requested = 4;
            spec.task = [&](std::size_t i, int)
            {
                try
                {
                    if (i % 7 == 0)
                    {
                        throw std::runtime_error{"injected"};
                    }
                }
                catch (const std::exception&)
                {
                    ++recorded_failures; // 记名后正常返回，作业仍算完成
                }
                ++completed;
            };
            pool::run(std::move(spec));
            cg::test::check(
                r,
                completed.load() == static_cast<int>(n),
                "the pool drains every job despite soft failures");
            std::size_t expected_failures = 0;
            for (std::size_t i = 0; i < n; ++i)
            {
                if (i % 7 == 0)
                {
                    ++expected_failures;
                }
            }
            cg::test::check(
                r,
                recorded_failures.load() == static_cast<int>(expected_failures),
                "each failing job records its own name");
        }
    }

    const cg::test::Registrar reg_compare_key{
        "util.path_compare_key",
        &path_compare_key_cases};

    const cg::test::Registrar reg_worker_pool{
        "util.worker_pool",
        &worker_pool_cases};

    // cg.util.treesit 门面的行为闸门。树形断言按 tree-sitter-bash v0.25.1
    // 的实际解析产物钉死，语法版本升级时这里是
    // 第一道红灯；断言只锁提取层将要依赖的结构事实（heredoc 正文是专用
    // 节点、注释内引号失活、残缺输入产出 ERROR 节点）。
    void treesit_bash_parse_cases(R& r)
    {
        namespace ts = cg::util::treesit;

        ts::Parser parser{ts::bash_language()};

        constexpr std::string_view src = "rg 'AcmeException' src/db";
        ts::Tree                   tree = parser.parse(src);
        const ts::Node             root = tree.root_node();
        cg::test::check(r, !root.is_null(), "parse yields a root node");
        cg::test::check(r, root.type() == "program", "root type is program");
        cg::test::check(
            r,
            !root.has_error(),
            "well-formed command yields a healthy tree");
        cg::test::check(
            r,
            root.start_byte() == 0 && root.end_byte() == src.size(),
            "root spans the whole source");
        cg::test::check(r, root.start_point().row == 0, "root starts on row 0");

        const ts::Node command = root.child(0);
        cg::test::check(
            r,
            command.type() == "command",
            "program's first child is the command");
        cg::test::check(
            r,
            command.named_child_count() == 3,
            "command_name + quoted pattern + path word");
        const ts::Node name = command.named_child(0);
        cg::test::check(
            r,
            name.type() == "command_name" && name.text(src) == "rg",
            "command_name reads back as rg");
        const ts::Node pattern = command.named_child(1);
        cg::test::check(
            r,
            pattern.type() == "raw_string",
            "single-quoted pattern parses as raw_string");
        cg::test::check(
            r,
            pattern.text(src) == "'AcmeException'",
            "raw text keeps the surrounding quotes");
        const ts::Node path = command.named_child(2);
        cg::test::check(
            r,
            path.type() == "word" && path.text(src) == "src/db",
            "bare path argument parses as word");

        cg::test::check(
            r,
            name.parent() == command,
            "parent() walks back to the command");
        cg::test::check(
            r,
            pattern.is_named() && !pattern.is_error() && !pattern.is_missing(),
            "ordinary nodes are plain named nodes");

        std::uint32_t visited = 0;
        ts::for_each_child(command, [&visited](ts::Node) { ++visited; });
        cg::test::check(
            r,
            visited == command.child_count(),
            "for_each_child visits every direct child");

        std::vector<std::string_view> preorder;
        ts::for_each_descendant(
            command,
            [&](ts::Node n) { preorder.push_back(n.type()); });
        cg::test::check(
            r,
            !preorder.empty() && preorder.front() == "command",
            "for_each_descendant is preorder (self first)");
        cg::test::check(
            r,
            preorder.size() > command.child_count(),
            "descendant walk reaches below the first level");
    }

    void treesit_heredoc_shape_cases(R& r)
    {
        namespace ts = cg::util::treesit;

        ts::Parser                 parser{ts::bash_language()};
        constexpr std::string_view src = "cat <<EOF\nrg evil\nEOF\nrg real";
        ts::Tree                   tree = parser.parse(src);
        cg::test::check(
            r,
            !tree.root_node().has_error(),
            "heredoc input parses cleanly");

        bool             has_heredoc_body = false;
        std::uint32_t    commands = 0;
        std::string_view body_text;
        ts::Point        body_start{};
        ts::for_each_descendant(
            tree.root_node(),
            [&](ts::Node n)
            {
                if (n.type() == "heredoc_body")
                {
                    has_heredoc_body = true;
                    body_text = n.text(src);
                    body_start = n.start_point();
                }
                if (n.type() == "command")
                {
                    ++commands;
                }
            });
        cg::test::check(
            r,
            has_heredoc_body,
            "heredoc body becomes a dedicated node");
        cg::test::check(
            r,
            body_text == "rg evil\n",
            "heredoc body text is the verbatim payload");
        cg::test::check(
            r,
            body_start.row == 1,
            "heredoc body starts on the second line");
        cg::test::check(
            r,
            commands == 2,
            "heredoc payload does not parse as a command");
    }

    void treesit_comment_cases(R& r)
    {
        namespace ts = cg::util::treesit;

        ts::Parser                 parser{ts::bash_language()};
        constexpr std::string_view src = "echo ok # trailing \"quote\"";
        ts::Tree                   tree = parser.parse(src);
        cg::test::check(
            r,
            !tree.root_node().has_error(),
            "trailing comment parses cleanly");
        bool has_comment = false;
        bool has_string = false;
        ts::for_each_descendant(
            tree.root_node(),
            [&](ts::Node n)
            {
                if (n.type() == "comment")
                {
                    has_comment = true;
                }
                if (n.type() == "string" || n.type() == "raw_string")
                {
                    has_string = true;
                }
            });
        cg::test::check(r, has_comment, "comment becomes a dedicated node");
        cg::test::check(
            r,
            !has_string,
            "quoted text inside a comment stays inert");
    }

    void treesit_error_diag_cases(R& r)
    {
        namespace ts = cg::util::treesit;

        ts::Parser parser{ts::bash_language()};

        constexpr std::string_view bad = "echo \"unterminated";
        ts::Tree                   broken = parser.parse(bad);
        cg::test::check(
            r,
            broken.root_node().has_error(),
            "truncated quote marks the tree");
        const auto diags = ts::collect_diagnostics(broken);
        cg::test::check(
            r,
            !diags.empty(),
            "diagnostics are collected for broken input");
        bool saw_error_node = false;
        bool error_span_checked = false;
        for (const ts::Diag& d : diags)
        {
            cg::test::check(
                r,
                d.node.start_byte() <= d.node.end_byte(),
                "diag span is ordered");
            if (d.kind == ts::DiagKind::ErrorNode)
            {
                saw_error_node = true;
                if (!error_span_checked)
                {
                    error_span_checked = true;
                    cg::test::check(
                        r,
                        d.node.text(bad) == "\"unterminated",
                        "ERROR node spans the unterminated quote text");
                }
            }
        }
        cg::test::check(
            r,
            saw_error_node,
            "at least one ERROR node is reported");

        ts::Tree good = parser.parse("echo ok");
        cg::test::check(
            r,
            !good.root_node().has_error(),
            "healthy tree reports no error flag");
        cg::test::check(
            r,
            ts::collect_diagnostics(good).empty(),
            "healthy tree collects no diagnostics");
    }

    void treesit_lifecycle_cases(R& r)
    {
        namespace ts = cg::util::treesit;

        ts::Parser parser{ts::bash_language()};

        ts::Tree a = parser.parse("echo one");
        ts::Tree b = std::move(a);
        cg::test::check(
            r,
            !b.root_node().is_null(),
            "moved-to tree keeps the syntax tree");
        cg::test::check(
            r,
            a.root_node().is_null(),
            "moved-from tree is an empty shell");
        a = std::move(b);
        cg::test::check(
            r,
            !a.root_node().is_null(),
            "move assignment transfers ownership");

        ts::Parser other = std::move(parser);
        ts::Tree   c = other.parse("echo two");
        cg::test::check(
            r,
            !c.root_node().is_null(),
            "moved parser keeps parsing");

        const ts::Tree empty = other.parse("");
        cg::test::check(
            r,
            !empty.root_node().is_null(),
            "empty input still yields a root");
        cg::test::check(
            r,
            empty.root_node().named_child_count() == 0,
            "empty input has no named children");

        const ts::Tree again = other.parse("echo three");
        cg::test::check(
            r,
            again.root_node().child_count() > 0,
            "one parser serves repeated parses");

        const ts::Node root = again.root_node();
        const ts::Node copy = root;
        cg::test::check(r, copy == root, "node copies stay equal");
        cg::test::check(
            r,
            copy.child(0) == root.child(0),
            "copies resolve children alike");
    }

    const cg::test::Registrar reg_ts_parse{
        "util.treesit_parse",
        &treesit_bash_parse_cases};
    const cg::test::Registrar reg_ts_heredoc{
        "util.treesit_heredoc",
        &treesit_heredoc_shape_cases};
    const cg::test::Registrar reg_ts_comment{
        "util.treesit_comment",
        &treesit_comment_cases};
    const cg::test::Registrar reg_ts_diag{
        "util.treesit_diagnostics",
        &treesit_error_diag_cases};
    const cg::test::Registrar reg_ts_lifecycle{
        "util.treesit_lifecycle",
        &treesit_lifecycle_cases};

} // namespace

// NOLINTNEXTLINE(clang-diagnostic-main-attached-to-named-module) 用 module-unit
// main 是有意为之：项目每个源文件都是具名模块单元；普通 TU
// 的 main 会破坏这项全模块政策
export int main(int argc, char* const* argv)
{
    if (argc >= 3 && std::string_view{argv[1]} == "--ccg-test-sleep")
    {
        std::this_thread::sleep_for(
            std::chrono::milliseconds{std::atoll(argv[2])});
        if (argc >= 4)
        {
            // 只在 sleep 之后写下的标记：它的存在证明
            // 分离子进程在宿主管道 EOF 后仍在运行。
            std::ofstream marker(argv[3], std::ios::binary);
            marker << "done";
        }
        std::println("child-ok");
        std::println(std::cerr, "child-err");
        return 7;
    }
    if (argc >= 3 && std::string_view{argv[1]} == "--ccg-test-spawn-sleeper")
    {
        // 句柄隔离测试的宿主角色：拉起一个分离的 sleep
        // 子进程然后退出。我们的 stdout 是 harness 读的管道。
        const std::string ms{argv[2]};
        const std::string marker = argc >= 4 ? argv[3] : std::string{};
        const cg::util::platform::TimedExecResult spawned =
            cg::util::platform::capture_exec_timed(
                cg::util::platform::executable_path(),
                {"--ccg-test-sleep", ms, marker},
                std::chrono::milliseconds{50});
        if (!spawned.started)
        {
            std::println(std::cerr, "host-spawn-failed");
            return 1;
        }
        std::println("host-ok");
        return 0;
    }
    if (argc >= 2 && std::string_view{argv[1]} == "--ccg-test-flood")
    {
        // 捕获原语并发排水闸门的洪水子进程：先写满 128 KiB stderr
        // （远超任何匿名管道缓冲）再写仅有的一行 stdout——顺序排水
        // 的父进程在此死锁（floodstub 同款形态，见 tests/floodstub.c）。
        static const char chunk[] = "0123456789abcdef0123456789abcdef";
        for (int n = 0; n < 4096; ++n)
        {
            std::cerr.write(
                chunk,
                static_cast<std::streamsize>(sizeof(chunk) - 1));
        }
        std::cerr.flush();
        std::println("flood-done");
        return 0;
    }
    if (argc >= 3 && std::string_view{argv[1]} == "--ccg-test-echo-env")
    {
        // 捕获原语环境块闸门的子进程：打印点名变量的值（未设时
        // <unset>），证明定制环境块真的到达了子进程。
        const auto value = cg::util::cstd::get_env(argv[2]);
        std::println("{}", value ? *value : std::string{"<unset>"});
        return 0;
    }
    return cg::test::run_all();
}
