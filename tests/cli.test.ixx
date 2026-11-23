module;
// db.tables.h 放在全局模块片段：本单元 import cg.db，而 MSVC 要求 sqlpp23
// 的表模型定义在此处可见，才能合并该模块记录的模板特化（完整缘由见
// db.tables.h）。 capture.tables.h 在 cg.msbuild 侧同理：本单元 import
// cg.cli，其再导出链携带会实例化 capture 表模板的读写 inline 函数。
#include "../src/db/db.tables.h"
#include "../src/msbuild/capture.tables.h"
// 进程内 hook 测试用的原始 fd 重定向（std::print 写向
// C 的 stdout 流，换 iostream 缓冲区捕获不到它）。
#include <cstdio>
#include <cstdlib>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <share.h> // _SH_DENYNO：_sopen_s 的共享模式（_open 的安全替代）
#else
#include <fcntl.h>
#include <unistd.h>
#endif

export module cg.test.cli;

// CLI 测试：实参解析（正确形态、用法错误 -> 退出码 2）、
// 符号解析分层（含歧义与 tier-4 建议）、JSON
// 渲染器经 cg.util.json 往返、关键行的文本
// 渲染快照，以及 run_main 退出码。

import std;
import cg.util;
import cg.util.cstd;
import cg.db;
import cg.clang;
import cg.graph;
import cg.cli;
import cg.test.harness;

namespace
{

    using R = cg::test::Result;
    namespace db = cg::db;
    using cg::cli::Invocation;

    constexpr std::string_view fixture_dir = CCG_SAMPLE1_DIR;

    // 一个共享夹具库，经真实 Indexer 索引一次
    // （形态同 cg_test_graph；约 33 符号、18 边、0 未解析）。
    auto fixture_store() -> db::GraphStore&
    {
        static std::optional<db::GraphStore> store;
        if (!store.has_value())
        {
            store.emplace(":memory:");
            const std::string compile_db =
                std::string{fixture_dir} + "/compile_commands.json";
            cg::graph::IndexOptions opt;
            opt.roots = {cg::util::norm_path(fixture_dir)};
            opt.progress = false;
            opt.compile_db_path = compile_db;
            cg::graph::Indexer indexer{*store, std::move(opt)};
            (void)indexer.run(cg::clang::load(compile_db, {}));
        }
        return *store;
    }

    [[nodiscard]]
    auto one_match(const std::string& q, int want_tier) -> bool
    {
        const db::FindResult fr =
            db::find_symbols(fixture_store(), q, db::SymbolFilter{});
        return fr.tier == want_tier && fr.matches.size() == 1;
    }

    [[nodiscard]]
    auto make_inv(std::vector<std::string> args) -> Invocation
    {
        return cg::cli::parse_args(args);
    }

    /**
     * @brief 把一个 C 文件描述符在作用域内重定向到文件的 RAII 夹具。
     *
     * @internal std::print 写向 C 的 stdout/stderr 流，iostream 缓冲区捕获
     *           不到，config/hook 用例以此接管 fd 0/1/2（readable=true 按
     *           只读打开，供 stdin 注入）。构造时冲刷并保存原描述符、以
     *           目标文件接管；析构冲刷并恢复，中途 return 也不污染同进程
     *           内后续用例的输出流。目标打开失败时静默保持原状，后续读
     *           文件的断言自然失败。写侧原语沿用既有用例的双平台写法
     *           （MSVC 的 _dup/_sopen_s/_dup2 与 POSIX 的 dup/open/dup2）。
     */
    class FdRedirect
    {
    public:
        FdRedirect(int fd, const std::filesystem::path& to, bool readable)
            : fd_{fd}, saved_{redirect(fd_, to, readable)}
        {
        }
        FdRedirect(const FdRedirect&) = delete;
        auto operator=(const FdRedirect&) -> FdRedirect& = delete;
        ~FdRedirect() { restore(); }

        /** @brief 立即冲刷并恢复原描述符（读捕获文件前复位用；析构幂等）。 */
        void restore()
        {
            if (saved_ < 0 || restored_)
            {
                return;
            }
            restored_ = true;
            flush(fd_);
#if defined(_WIN32)
            _dup2(saved_, fd_);
            _close(saved_);
#else
            ::dup2(saved_, fd_);
            ::close(saved_);
#endif
        }

    private:
        int  fd_;
        int  saved_;
        bool restored_ = false;

        /** @brief 冲刷 fd 对应的 C 流（stdin 无对应可冲刷缓冲）。 */
        static void flush(int fd)
        {
            if (fd == 1)
            {
                std::fflush(stdout);
            }
            else if (fd == 2)
            {
                std::fflush(stderr);
            }
        }

        /** @brief 保存原描述符并以目标文件接管；返回保存的描述符。 */
        static int redirect(
            int                          fd,
            const std::filesystem::path& to,
            bool                         readable)
        {
            flush(fd);
#if defined(_WIN32)
            const int saved = _dup(fd);
            int       opened = -1;
            if (readable)
            {
                _sopen_s(
                    &opened,
                    to.generic_string().c_str(),
                    _O_RDONLY | _O_BINARY,
                    _SH_DENYNO,
                    0);
            }
            else
            {
                _sopen_s(
                    &opened,
                    to.generic_string().c_str(),
                    _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY,
                    _SH_DENYNO,
                    0600);
            }
            if (opened >= 0)
            {
                _dup2(opened, fd);
                _close(opened);
            }
#else
            const int saved = ::dup(fd);
            const int opened =
                readable
                    ? ::open(to.c_str(), O_RDONLY)
                    : ::open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (opened >= 0)
            {
                ::dup2(opened, fd);
                ::close(opened);
            }
#endif
            return saved;
        }
    };

    // ------------------------------------------------------------------ 解析
    // ----

    void parse_ok_shapes(R& r)
    {
        const auto index = make_inv(
            {"index",
             "some/dir",
             "-o",
             "x/y",
             "--config",
             "main",
             "-v",
             "--json"});
        cg::test::check(
            r,
            index.cmd == Invocation::Cmd::Index,
            "index command");
        cg::test::check(r, index.positional.size() == 1, "index roots count");
        cg::test::check(
            r,
            index.output_dir == "x/y",
            "-o sets the output folder");
        cg::test::check(
            r,
            index.config_name == "main" && index.config_explicit,
            "--config sets the configuration name");
        cg::test::check(r, index.verbose && index.json, "-v/--json recorded");
        const auto qc = make_inv({"query", "x", "--config", "self"});
        cg::test::check(
            r,
            qc.config_name == "self",
            "--config parses for query commands");

        const auto ccdb = make_inv(
            {"index",
             "root",
             "--config",
             "main",
             "--compile-commands=f.json",
             "--force"});
        cg::test::check(
            r,
            ccdb.compile_db == "f.json" && ccdb.force,
            "inline --opt=value and --force parse");

        const auto modes = make_inv(
            {"index",
             "root",
             "--config",
             "main",
             "--full",
             "--dry-run",
             "--paranoid"});
        cg::test::check(
            r,
            modes.full && modes.dry_run && modes.paranoid,
            "--full/--dry-run/--paranoid parse for index");

        cg::test::check(
            r,
            make_inv({"index", "root", "--config", "m"}).jobs == 0,
            "--jobs defaults to auto (0)");
        cg::test::check(
            r,
            make_inv({"index", "root", "--config", "m", "--jobs", "7"}).jobs
                    == 7
                && make_inv({"index", "root", "--config", "m", "--jobs=1"}).jobs
                       == 1,
            "--jobs value and inline form parse");
        cg::test::check(
            r,
            make_inv({"update", "a.cpp", "--jobs", "3"}).jobs == 3
                && make_inv({"msbuild", "p", "--jobs=5"}).jobs == 5,
            "--jobs parses for update and msbuild (unified cap)");

        const auto llvm_root = make_inv(
            {"index",
             "root",
             "--config",
             "m",
             "--llvm-root",
             "/opt/llvm",
             "--llvm-root=/other/llvm"});
        cg::test::check(
            r,
            llvm_root.llvm_root == "/other/llvm",
            "--llvm-root value, inline form, last one wins");

        const auto callers = make_inv(
            {"callers",
             "shape::Circle::area",
             "--depth",
             "2",
             "--kind",
             "method"});
        cg::test::check(r, callers.depth == 2, "--depth value");
        cg::test::check(
            r,
            callers.kind == db::SymbolKind::Method,
            "--kind method");
        cg::test::check(
            r,
            make_inv({"callers", "x"}).depth == 1,
            "callers depth defaults to 1");
        cg::test::check(
            r,
            make_inv({"impact", "x"}).depth == 3,
            "impact depth defaults to 3");
        cg::test::check(
            r,
            make_inv({"graph", "--symbol", "main", "--depth", "0"}).depth == 0,
            "graph --depth 0 means unlimited");
        cg::test::check(
            r,
            make_inv({"impact", "x", "--direction", "down"}).direction
                == cg::graph::Direction::Down,
            "--direction down");
        cg::test::check(
            r,
            make_inv({"query", "x", "--file", "circle"}).file_substring
                == std::optional<std::string>{"circle"},
            "--file substring");
        cg::test::check(
            r,
            make_inv({"query", "x", "--kind", "enum"}).kind
                == db::SymbolKind::Enumeration,
            "--kind enum maps to the enumeration kind");
        cg::test::check(
            r,
            make_inv({"trace", "a", "b"}).positional.size() == 2,
            "trace takes two symbols");
        cg::test::check(
            r,
            make_inv({"callers", "x", "--no-merge"}).no_merge
                && !make_inv({"context", "x"}).no_merge
                && !make_inv({"callers", "x"}).no_merge,
            "--no-merge parses for family commands, default off");
        cg::test::check(
            r,
            make_inv({"callers", "x"}).dispatch_mode
                    == cg::graph::DispatchMode::Precise
                && make_inv({"callers", "x", "--no-dispatch"}).dispatch_mode
                       == cg::graph::DispatchMode::Off
                && make_inv({"trace", "a", "b", "--dispatch-fallback"})
                           .dispatch_mode
                       == cg::graph::DispatchMode::Fallback
                && make_inv({"changes", "f", "--no-dispatch"}).dispatch_mode
                       == cg::graph::DispatchMode::Off,
            "dispatch mode defaults to precise; both switches parse on all "
            "covered commands");
        if (!cg::util::cstd::get_env("CCG_DB").has_value())
        {
            cg::test::check(
                r,
                make_inv({"stats"}).output_dir == "./.ccg",
                "output folder defaults to ./.ccg without $CCG_DB");
        }
        const auto help = make_inv({"callers", "x", "-h"});
        cg::test::check(
            r,
            help.cmd == Invocation::Cmd::Help
                && help.positional == std::vector<std::string>{"callers"},
            "-h mid-command switches to per-command help");
    }

    void parse_usage_errors(R& r)
    {
        const std::vector<std::vector<std::string>> bad = {
            {},
            {"bogus"},
            {"--bogus"},
            {"query"},
            {"query", "a", "b"},
            {"stats", "extra"},
            {"query", "a", "--kind", "methods"},
            {"callers", "a", "--depth", "0"},
            {"callers", "a", "--depth", "11"},
            {"callers", "a", "--depth", "x"},
            {"impact", "a", "--direction", "sideways"},
            {"trace", "only-one"},
            {"stats", "--depth", "2"},
            {"callers", "a", "--no-dispatch", "--dispatch-fallback"},
            {"query", "a", "--no-dispatch"},
            {"stats", "--dispatch-fallback"},
            {"query", "a", "--force"},
            {"query", "a", "--file"},
            {"help", "bogus"},
            {"query", "a", "--jobs", "4"},
            {"index", "root", "--config", "m", "--jobs", "0"},
            {"index", "root", "--config", "m", "--jobs", "65"},
            {"index", "root", "--config", "m", "--jobs", "x"},
            {"index",
             "root",
             "--config",
             "m",
             "--jobs",
             "99999999999999999999999999"},
            {"callers", "a", "--depth", "99999999999999999999999999"},
            {"query", "a", "--llvm-root", "/opt/llvm"},
            {"index", "root", "--config", "m", "--llvm-root"},
            {"query", "a", "--full"},
            {"stats", "--dry-run"},
            {"query", "a", "--paranoid"},
            {"query", "a", "--no-merge"},
            {"index", "root", "--config", "m", "--no-merge"},
            {"graph"},
            {"graph", "--symbol", "a", "--modules"},
            {"graph", "--symbol"},
            {"graph", "--symbol", "a", "extra"},
            {"graph", "--symbol", "a", "--format", "svg"},
            {"graph", "--symbol", "a", "--min-weight", "3"},
            {"graph", "--modules", "--depth", "2"},
            {"graph", "--modules", "--min-weight", "0"},
            {"query", "a", "--modules"},
            {"stats", "--symbol", "x"},
            {"stats", "--output", "g.dot"},
            {"index", "root"},
            {"stats", "-o", "x/y.db"},
            {"query", "x", "-o", ":memory:"},
        };
        for (const auto& args : bad)
        {
            bool threw = false;
            try
            {
                (void)make_inv(args);
            }
            catch (const cg::cli::UsageError&)
            {
                threw = true;
            }
            catch (...)
            {
                cg::test::check(
                    r,
                    false,
                    "wrong exception for " + std::to_string(args.size()));
            }
            std::string rendered;
            for (const auto& a : args)
            {
                rendered += rendered.empty() ? a : (" " + a);
            }
            cg::test::check(r, threw, "usage error for '" + rendered + "'");
        }

        // 异常携带可打印的用法块。
        try
        {
            (void)make_inv({"query"});
        }
        catch (const cg::cli::UsageError& e)
        {
            cg::test::check(
                r,
                e.usage().find("usage: ccg query") != std::string::npos,
                "usage block names the command");
        }
    }

    // ------------------------------------------------------------ 分层匹配
    // ------

    void resolution_tiers(R& r)
    {
        cg::test::check(
            r,
            one_match("shape::Circle::area", 1),
            "tier 1: exact qualified name");
        cg::test::check(
            r,
            one_match("Circle::area", 2),
            "tier 2: qualified suffix");
        cg::test::check(
            r,
            one_match("total_area", 1),
            "tier 1: exact unqualified name");
        const db::FindResult substring =
            db::find_symbols(fixture_store(), "Circ", db::SymbolFilter{});
        cg::test::check(
            r,
            substring.tier == 3 && !substring.matches.empty(),
            "tier 3: substring");
        const db::FindResult none =
            db::find_symbols(fixture_store(), "areaa", db::SymbolFilter{});
        cg::test::check(r, none.tier == 0, "tiers 1-3 all miss for a typo");

        // unique-key 逃生门是 tier-1 命中。
        const auto area = db::find_symbols(
            fixture_store(),
            "shape::Circle::area",
            db::SymbolFilter{});
        if (area.matches.size() == 1)
        {
            const db::FindResult by_key = db::find_symbols(
                fixture_store(),
                area.matches.front().unique_key,
                db::SymbolFilter{});
            cg::test::check(
                r,
                by_key.tier == 1 && by_key.matches.size() == 1,
                "unique key matches at tier 1");
        }

        // 歧义："area" 在 Circle 与 Shape 上都有（tier-1 多重命中）。
        // tier-1 语义断言走短路档（合并档会并入 total_area/area_all 子串层）。
        const db::FindResult amb = db::find_symbols(
            fixture_store(),
            "area",
            db::SymbolFilter{},
            /*merge_lower_tiers=*/false);
        cg::test::check(
            r,
            amb.tier == 1 && amb.matches.size() == 2,
            "tier-1 multi-hit for overloaded-name targets");

        Invocation inv = make_inv({"context", "area"});
        inv.json = true; // error 载荷走 stdout，让测试 stderr 干净
        cg::test::check(
            r,
            !cg::cli::resolve_unique_target(fixture_store(), inv, "area")
                 .has_value(),
            "ambiguous target resolves to nothing");
        inv.kind = db::SymbolKind::Method; // 仍是 2（两个都是方法）
        cg::test::check(
            r,
            !cg::cli::resolve_unique_target(fixture_store(), inv, "area")
                 .has_value(),
            "--kind alone does not disambiguate two methods");
        inv.kind.reset();
        inv.file_substring =
            "circle.cpp"; // the definition site of Circle::area
        const auto narrowed =
            cg::cli::resolve_unique_target(fixture_store(), inv, "area");
        cg::test::check(
            r,
            narrowed.has_value()
                && narrowed->qualified_name == "shape::Circle::area",
            "--file narrows the ambiguity to one target");
        cg::test::check(
            r,
            !cg::cli::resolve_unique_target(fixture_store(), inv, "nosuch")
                 .has_value(),
            "unknown target resolves to nothing");
    }

    void tier4_suggestions(R& r)
    {
        const std::vector<std::string> got =
            cg::cli::suggest_similar(fixture_store(), "areaa", 5);
        bool has_area = false;
        for (const std::string& s : got)
        {
            has_area = has_area || s == "area";
        }
        cg::test::check(r, has_area, "'areaa' suggests 'area'");
        cg::test::check(r, got.size() <= 5, "suggestions capped");
        const std::vector<std::string> none =
            cg::cli::suggest_similar(fixture_store(), "zzzzqqqq", 5);
        cg::test::check(r, none.empty(), "no pool for a garbage query");
    }

    /**
     * @brief --exclude-glob 匹配器回归钉：中部 `**`、隐式 `**` 目录前缀、单
     *        `*` 不跨段、`?`、ASCII 折叠与字面 `[...]`，外加 path_excluded
     *        的并集语义。
     *
     * @internal 定义=--exclude-glob 的 glob 语义（编译式：`**` 跨段、
     * 单 `*` 不跨段；中部 `**` 一句的配套钉）：所有
     * 断言逐条钉住该语义，实现漂移（如把 `**` 退化成段内 `*`）即红。
     * soundness 边界：只钉纯函数匹配器与展示端过滤入口，不触索引与库。
     * fallback 清单：空模式拒绝发生在解析期，不在此测。
     */
    void glob_exclude_filter(R& r)
    {
        using cg::cli::detail::glob_match;
        using cg::cli::detail::path_excluded;

        cg::test::check(
            r,
            glob_match("src/**/test.cpp", "src/a/b/test.cpp"),
            "mid ** crosses segments");
        cg::test::check(
            r,
            glob_match("src/**/test.cpp", "src/test.cpp"),
            "mid ** matches zero segments");
        cg::test::check(
            r,
            glob_match("**/build/**", "a/b/build/x/y.obj"),
            "leading **/ matches at any depth");
        cg::test::check(
            r,
            glob_match("**", "any/thing.cpp"),
            "bare ** swallows everything incl. /");
        cg::test::check(
            r,
            !glob_match("*.cpp", "a/b.cpp"),
            "single * never crosses /");
        cg::test::check(
            r,
            glob_match("*.cpp", "b.cpp"),
            "single * matches within one segment");
        cg::test::check(
            r,
            glob_match("a?c/x.cpp", "abc/x.cpp")
                && !glob_match("a?c/x.cpp", "a/c/x.cpp"),
            "? matches exactly one non-slash char");
        cg::test::check(
            r,
            glob_match("BUILD/**", "build/out"),
            "ASCII case-insensitive fold");
        cg::test::check(
            r,
            !glob_match("x[0-9]/y", "x7/y")
                && glob_match("x[0-9]/y", "x[0-9]/y"),
            "no character classes: [...] compares literally");

        const std::vector<std::string> globs{"_deps/**", "build/**"};
        cg::test::check(
            r,
            path_excluded(globs, "e:/p/_deps/spdlog/x.hpp"),
            "_deps/** covers the whole vendored subtree");
        cg::test::check(
            r,
            path_excluded(globs, "e:/p/nested/build/a.obj"),
            "unanchored build/** hits at any depth");
        cg::test::check(
            r,
            !path_excluded(globs, "e:/p/src/a.cpp"),
            "non-matching path stays listed");
        cg::test::check(
            r,
            !path_excluded({}, "e:/p/build/a.obj"),
            "no globs: nothing is excluded");
    }

    // ------------------------------------------------------------ json 渲染
    // -----

    namespace json = cg::util::json;

    [[nodiscard]]
    auto parse_doc(const std::string& text) -> bool
    {
        try
        {
            (void)json::Document::parse(text);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    void json_round_trips(R& r)
    {
        db::GraphStore& store = fixture_store();

        // query：形态（tier、匹配数）幸存。
        const db::FindResult fr =
            db::find_symbols(store, "area", db::SymbolFilter{});
        const std::string qj =
            cg::cli::render::query_json("area", fr.tier, fr.matches);
        const json::Document doc = json::Document::parse(qj);
        const auto           tier = doc.root().get("tier");
        const auto           matches = doc.root().get("matches");
        cg::test::check(
            r,
            tier.has_value() && tier->is_string()
                && tier->as_string() == std::string_view{"exact"},
            "query json tier field");
        cg::test::check(
            r,
            matches.has_value() && matches->is_array()
                && matches->size() == fr.matches.size(),
            "query json matches count");
        if (matches.has_value() && matches->size() >= 1)
        {
            const auto first = matches->at(0);
            cg::test::check(
                r,
                first.has_value() && first->get("uid").has_value()
                    && first->get("qualified_name").has_value(),
                "match entries carry uid + qualified_name");
        }

        // context（上下文）
        const db::FindResult area =
            db::find_symbols(store, "shape::Circle::area", db::SymbolFilter{});
        const cg::graph::ContextView view =
            cg::graph::context(store, area.matches.front().id);
        const std::string cj = cg::cli::render::context_json(view, store, {});
        const json::Document cdoc = json::Document::parse(cj);
        cg::test::check(
            r,
            cdoc.root().get("symbol").has_value()
                && cdoc.root().get("incoming").has_value()
                && cdoc.root().get("outgoing").has_value(),
            "context json top-level shape");

        // impact（影响面）
        const cg::graph::ImpactResult imp = cg::graph::impact(
            store,
            area.matches.front().id,
            cg::graph::Direction::Up,
            3);
        const std::string ij = cg::cli::render::impact_json(
            area.matches.front(),
            cg::graph::Direction::Up,
            3,
            imp,
            store,
            std::nullopt,
            {});
        const json::Document idoc = json::Document::parse(ij);
        const auto           risk = idoc.root().get("risk");
        const auto           count = idoc.root().get("impactedCount");
        cg::test::check(
            r,
            risk.has_value() && risk->as_string() == std::string_view{"LOW"},
            "impact json risk is LOW on the fixture");
        cg::test::check(
            r,
            count.has_value() && count->as_int() == std::int64_t{3},
            "impact json impactedCount counts unique symbols");

        // trace（路径）
        const db::FindResult from =
            db::find_symbols(store, "main", db::SymbolFilter{});
        const db::FindResult to =
            db::find_symbols(store, "shape::pi", db::SymbolFilter{});
        const cg::graph::TraceResult tr = cg::graph::trace(
            store,
            from.matches.front().id,
            to.matches.front().id,
            10);
        const std::string tj = cg::cli::render::trace_json(
            from.matches.front(),
            to.matches.front(),
            tr,
            store);
        const json::Document tdoc = json::Document::parse(tj);
        const auto           hops = tdoc.root().get("hops");
        cg::test::check(
            r,
            tdoc.root().get("found")->as_bool() == true && hops.has_value()
                && hops->size() == std::size_t{6},
            "trace json: found with 5 hops = 6 chain symbols");

        // stats + index + error 载荷
        cg::cli::render::StatsInputs in;
        in.db_path = ":memory:";
        in.st = db::stats(store);
        const std::string    sj = cg::cli::render::stats_json(in);
        const json::Document sdoc = json::Document::parse(sj);
        const auto           total = sdoc.root().get("symbols");
        cg::test::check(
            r,
            total.has_value()
                && total->get("total")->as_int() == std::int64_t{35},
            "stats json symbol total");

        cg::graph::IndexSummary sum;
        sum.tus = 3;
        sum.fresh = 3;
        sum.symbols = 33;
        sum.edges = 18;
        sum.edges_by_type[static_cast<int>(db::EdgeType::Calls)] = 11;
        sum.elapsed_seconds = 1.25;
        sum.db_size_bytes = 90000;
        sum.roots = {"E:/fixture"};
        const std::string    xj = cg::cli::render::index_json(sum);
        const json::Document xdoc = json::Document::parse(xj);
        cg::test::check(
            r,
            xdoc.root().get("tus")->as_int() == std::int64_t{3}
                && xdoc.root().get("edgesByType")->get("calls")->as_int()
                       == std::int64_t{11},
            "index json fields");
        cg::test::check(
            r,
            parse_doc(cg::cli::render::error_json("boom", std::nullopt)),
            "error json parses");
    }

    // ------------------------------------------------------------ 文本渲染
    // ------

    void text_renders(R& r)
    {
        db::GraphStore&      store = fixture_store();
        const db::FindResult fr =
            db::find_symbols(store, "area", db::SymbolFilter{});
        const std::string qt =
            cg::cli::render::query_text("area", fr.tier, fr.matches);
        cg::test::check(
            r,
            qt.find("4 matches (tier: exact + substring)") != std::string::npos,
            "query text footer names every merged tier");
        cg::test::check(
            r,
            qt.find("shape::Circle::area") != std::string::npos,
            "query text rows");
        cg::test::check(
            r,
            cg::cli::render::candidates_text(fr.matches)
                    .find("shape::Shape::area")
                != std::string::npos,
            "candidates text lists every match");

        // Impact / neighbors 需要跨文件调用链，所以显式
        // 指向 Circle::area（其定义点在 circle.cpp）。
        const db::FindResult area =
            db::find_symbols(store, "shape::Circle::area", db::SymbolFilter{});
        const auto                    target = area.matches.front();
        const cg::graph::ImpactResult imp =
            cg::graph::impact(store, target.id, cg::graph::Direction::Up, 3);
        const std::string it = cg::cli::render::impact_text(
            target,
            cg::graph::Direction::Up,
            imp,
            store,
            std::nullopt,
            {});
        cg::test::check(
            r,
            it.find("risk     LOW") != std::string::npos,
            "impact text risk line");
        cg::test::check(
            r,
            it.find("impacted 3 (direct: 1") != std::string::npos,
            "impact text impacted line");

        const cg::graph::NeighborsResult nb =
            cg::graph::callers(store, target.id, 2);
        const std::string nt = cg::cli::render::neighbors_text(
            true,
            target,
            nb,
            store,
            std::nullopt,
            {});
        cg::test::check(
            r,
            nt.find("depth 1") != std::string::npos
                && nt.find("depth 2") != std::string::npos,
            "neighbors text depth sections");
        cg::test::check(
            r,
            nt.find("(via render::Canvas::total_area)") != std::string::npos,
            "neighbors text via origin");

        const db::FindResult from =
            db::find_symbols(store, "main", db::SymbolFilter{});
        const db::FindResult to =
            db::find_symbols(store, "shape::pi", db::SymbolFilter{});
        const cg::graph::TraceResult tr = cg::graph::trace(
            store,
            from.matches.front().id,
            to.matches.front().id,
            10);
        const std::string tt = cg::cli::render::trace_text(
            from.matches.front(),
            to.matches.front(),
            tr,
            store);
        cg::test::check(
            r,
            tt.find("found: main -> shape::pi  (5 hops)") != std::string::npos,
            "trace text found line");
        cg::test::check(
            r,
            tt.find("--calls--> shape::pi") != std::string::npos,
            "trace text hop chain");

        // 全跳过的索引运行渲染 "skipped: all (N)"。
        cg::graph::IndexSummary sum;
        sum.tus = 3;
        sum.skipped = 3;
        const std::string xt = cg::cli::render::index_text(sum, "ccg.db");
        cg::test::check(
            r,
            xt.find("skipped: all (3)") != std::string::npos,
            "index text skipped-all phrasing");

        // PCH 的逐 TU note（失败回退 / closure-unresolvable）必须在摘要
        // 与 JSON 中真实可见，而不只是计数。
        cg::graph::IndexSummary pch_sum;
        pch_sum.tus = 2;
        pch_sum.pch_built = 1;
        pch_sum.pch_fallbacks = 1;
        pch_sum.pch_notes = {
            "E:/p/main.cpp: pch-build-failed: boom - text fallback",
            "E:/p/pch.cxx: pch-closure-unresolvable: a non-literal "
            "#include ... is rebuilt every run"};
        const std::string pt = cg::cli::render::index_text(pch_sum, "ccg.db");
        cg::test::check(
            r,
            pt.find("pch notes:") != std::string::npos
                && pt.find("pch-build-failed") != std::string::npos
                && pt.find("closure-unresolvable") != std::string::npos,
            "index text renders PCH fallback and "
            "closure-unresolvable notes");
        const std::string    pj = cg::cli::render::index_json(pch_sum);
        const json::Document pdoc = json::Document::parse(pj);
        const auto           pnotes = pdoc.root().get("pchNotes");
        cg::test::check(
            r,
            pnotes && pnotes->is_array() && pnotes->size() == 2
                && std::string_view{*pnotes->at(1)->as_string()}.find(
                       "closure-unresolvable")
                       != std::string_view::npos,
            "index json renders pchNotes (not just counters)");
    }

    // -------------------------------------------------------------- 退出码
    // --------

    /// 花括号实参表便捷重载：span 形参不接受花括号初始化。
    auto run_main(std::initializer_list<std::string> args) -> int
    {
        const std::vector<std::string> argv{args};
        return cg::cli::run_main(argv);
    }

    /// 既有向量实参的直通重载。
    auto run_main(std::span<const std::string> args) -> int
    {
        return cg::cli::run_main(args);
    }

    void run_main_exit_codes(R& r)
    {
        cg::test::check(r, run_main({"bogus"}) == 2, "unknown command exits 2");
        cg::test::check(r, run_main({}) == 2, "no args exits 2");
        cg::test::check(r, run_main({"query"}) == 2, "missing operand exits 2");
        cg::test::check(
            r,
            run_main({"stats", "--depth", "1"}) == 2,
            "option invalid for the command exits 2");
        cg::test::check(
            r,
            run_main({"stats", "-o", "no/such/ccg-dir", "--config", "missing"})
                == 1,
            "missing database exits 1");
        cg::test::check(r, run_main({"--version"}) == 0, "--version exits 0");
        cg::test::check(r, run_main({"help"}) == 0, "help exits 0");
        cg::test::check(
            r,
            run_main({"help", "query"}) == 0,
            "per-command help exits 0");
        cg::test::check(
            r,
            run_main({"query", "--help"}) == 0,
            "--help on a subcommand exits 0");
        cg::test::check(
            r,
            run_main({"check", "-o", "no/such/ccg-dir"}) == 2,
            "check without a mode exits 2 (usage, not runtime)");
        cg::test::check(
            r,
            run_main(
                {"check",
                 "--cycles",
                 "-o",
                 "no/such/ccg-dir",
                 "--config",
                 "missing"})
                == 1,
            "check --cycles on a missing db exits 1");
        cg::test::check(
            r,
            run_main(
                {"changes",
                 "some.cpp",
                 "-o",
                 "no/such/ccg-dir",
                 "--config",
                 "missing"})
                == 1,
            "changes on a missing db exits 1");
        cg::test::check(
            r,
            run_main({"changes", "x", "--depth", "11"}) == 2,
            "changes depth out of range exits 2");
        cg::test::check(
            r,
            run_main({"check", "--cycles", "--max", "0"}) == 2,
            "check --max 0 exits 2");
        cg::test::check(
            r,
            run_main({"update"}) == 2,
            "bare update (no files, no --drain) exits 2");
        cg::test::check(
            r,
            run_main(
                {"update",
                 "x.cpp",
                 "-o",
                 "no/such/ccg-dir",
                 "--config",
                 "missing"})
                == 1,
            "update on a missing db exits 1");
        cg::test::check(
            r,
            run_main({"help", "update"}) == 0,
            "help update exits 0");
    }

    // ---------------------------------------------------- changes/check 解析
    // ------

    void parse_changes_check(R& r)
    {
        const auto changes = make_inv({"changes", "a.cpp", "b.hpp", "-o", "x"});
        cg::test::check(
            r,
            changes.cmd == Invocation::Cmd::Changes
                && changes.positional.size() == 2,
            "changes takes many files");
        cg::test::check(r, changes.depth == 3, "changes depth defaults to 3");
        cg::test::check(
            r,
            make_inv({"changes", "a.cpp", "--depth", "1"}).depth == 1,
            "changes --depth parses");
        cg::test::check(
            r,
            make_inv({"changes", "a.cpp", "--from-file", "list.txt"}).from_file
                == "list.txt",
            "--from-file records the list path");
        cg::test::check(
            r,
            make_inv({"changes", "-"}).positional.size() == 1,
            "'-' stays a positional (reads stdin at run time)");

        const auto check = make_inv({"check", "--cycles"});
        cg::test::check(
            r,
            check.cmd == Invocation::Cmd::Check && check.cycles,
            "check --cycles parses");
        cg::test::check(r, check.list_cap == 20, "--max defaults to 20");
        cg::test::check(
            r,
            make_inv({"check", "--cycles", "--max", "3"}).list_cap == 3,
            "--max value parses");

        const std::vector<std::vector<std::string>> bad = {
            {"changes", "a", "--kind", "method"},
            {"changes", "a", "--from-file"},
            {"changes", "a", "--from-file", ""},
            {"changes", "a", "--no-merge"},
            {"changes", "a", "--depth", "11"},
            {"check", "extra"},
            {"check", "--cycles", "--depth", "2"},
            {"check", "--max", "5"},
            {"check", "--cycles", "--max", "0"},
            {"check", "--cycles", "--max", "1001"},
            {"check", "--cycles", "--max", "x"},
            {"check", "--from-file", "f"},
        };
        for (const auto& args : bad)
        {
            bool threw = false;
            try
            {
                (void)make_inv(args);
            }
            catch (const cg::cli::UsageError&)
            {
                threw = true;
            }
            std::string rendered;
            for (const auto& a : args)
            {
                rendered += rendered.empty() ? a : (" " + a);
            }
            cg::test::check(r, threw, "usage error for '" + rendered + "'");
        }
    }

    // ------------------------------------------- changes 路径解析 + 渲染
    // ----------

    void changes_resolution_and_render(R& r)
    {
        db::GraphStore& store = fixture_store();

        // 精确的规范化拼写可解析。
        const std::string exact =
            cg::util::norm_path(std::string{fixture_dir} + "/circle.hpp");
        const auto hit = cg::cli::resolve_input_path(store, exact);
        cg::test::check(
            r,
            hit.has_value() && hit->path == exact,
            "exact normalized path resolves to itself");

        // 盘符大小写漂移会漏过大小写不敏感
        // 那一遍（已删除文件没有磁盘拼写可规范化）。
        std::string cased = exact;
        cased[0] = 'E';
        const auto folded =
            cg::cli::resolve_input_path(store, cg::util::norm_path(cased));
        cg::test::check(
            r,
            folded.has_value() && folded->path == exact,
            "case-different path resolves to the db spelling");

        // 唯一后缀能救回从别的工作目录喂进来的清单。
        const auto suffixed = cg::cli::resolve_input_path(
            store,
            "e:/elsewhere/repo/sample1/circle.hpp");
        cg::test::check(
            r,
            suffixed.has_value() && suffixed->path == exact,
            "unique path-suffix match resolves");

        // 什么都匹配不上 -> nullopt（有报告，绝不静默丢弃）。
        cg::test::check(
            r,
            !cg::cli::resolve_input_path(
                 store,
                 "e:/elsewhere/definitely-not.cpp")
                 .has_value(),
            "unknown path resolves to nothing");

        // 渲染器对合成载荷往返。
        cg::cli::render::ChangesInputs in;
        in.depth = 2;
        cg::cli::ChangedFile known;
        known.input = "src/a.hpp";
        known.path = "e:/repo/src/a.hpp";
        known.known = true;
        known.deleted = false;
        known.symbols = 4;
        cg::cli::ChangedFile missing;
        missing.input = "src/gone.cpp";
        missing.known = false;
        in.files = {known, missing};
        const db::FindResult area =
            db::find_symbols(store, "shape::Circle::area", db::SymbolFilter{});
        const cg::db::SymId seed = area.matches.front().id;
        in.result.seeds = {seed};
        in.result.consumers.push_back(
            {.id = seed,
             .depth = 1,
             .seed = seed,
             .via_id = seed,
             .fan_in = 2,
             .risk = cg::graph::Risk::Medium});
        in.result.direct = 1;
        in.result.transitive = 0;
        in.result.fan_in = 2;
        in.result.risk_score = 5;
        in.result.risk = cg::graph::Risk::Low;

        const std::string text = cg::cli::render::changes_text(in, store);
        cg::test::check(
            r,
            text.find("known 1, deleted 0, unknown 1") != std::string::npos,
            "changes text file-table summary");
        cg::test::check(
            r,
            text.find("not in the database") != std::string::npos,
            "changes text marks unknown files");
        cg::test::check(
            r,
            text.find("risk MEDIUM") != std::string::npos,
            "changes text per-consumer risk");
        const json::Document doc =
            json::Document::parse(cg::cli::render::changes_json(in, store));
        const auto files = doc.root().get("files");
        cg::test::check(
            r,
            files.has_value() && files->size() == 2
                && files->at(1)->get("status")->as_string()
                       == std::string_view{"unknown"},
            "changes json files array with status");
        cg::test::check(
            r,
            doc.root().get("consumers")->at(0)->get("viaSeed")->as_string()
                == std::string_view{"shape::Circle::area"},
            "changes json viaSeed attribution");

        // 环渲染器往返（自建一次性库：只有文件行）。
        db::GraphStore          cyc_store{":memory:"};
        cg::graph::CyclesResult cyc;
        cyc.groups_total = 1;
        cyc.files_total = 2;
        cyc.edges_total = 2;
        cyc.files_scanned = 5;
        const cg::db::FileId h1 = cyc_store.upsert_file("e:/repo/cyc/one.hpp");
        const cg::db::FileId h2 = cyc_store.upsert_file("e:/repo/cyc/two.hpp");
        cyc.groups.push_back(
            {.files = {h1, h2}, .edges = {{h1, h2}, {h2, h1}}});
        const std::string ctext = cg::cli::render::cycles_text(cyc, cyc_store);
        cg::test::check(
            r,
            ctext.find("include cycles   1 group (2 files, 2 edges)")
                != std::string::npos,
            "cycles text header");
        cg::test::check(
            r,
            ctext.find("one.hpp -> e:/repo/cyc/two.hpp") != std::string::npos,
            "cycles text edge lines");
        const json::Document cdoc =
            json::Document::parse(cg::cli::render::cycles_json(cyc, cyc_store));
        cg::test::check(
            r,
            cdoc.root().get("cycles")->as_bool() == true
                && cdoc.root().get("groupsTotal")->as_int() == std::int64_t{1},
            "cycles json totals");
        cg::test::check(
            r,
            cdoc.root()
                    .get("groups")
                    ->at(0)
                    ->get("edges")
                    ->at(0)
                    ->get("from")
                    ->as_string()
                == std::string_view{"e:/repo/cyc/one.hpp"},
            "cycles json edge objects");
        cg::test::check(
            r,
            cdoc.root().get("ambiguousIncludes")->as_int() == std::int64_t{0},
            "cycles json reports the ambiguous-includes counter");
        cyc.groups_total = 0;
        cyc.groups.clear();
        cg::test::check(
            r,
            cg::cli::render::cycles_text(cyc, cyc_store).find("none")
                != std::string::npos,
            "no-cycles text");
        cyc.ambiguous_links = 2;
        cg::test::check(
            r,
            cg::cli::render::cycles_text(cyc, cyc_store)
                    .find("ambiguous includes 2")
                != std::string::npos,
            "ambiguous includes surfaced in text (no edge from them)");
    }

    // `ccg msbuild` 解析矩阵（解析闸门的单元半边）：
    // 全选项集往返，每条组合规则都以退出码 2 形态拒绝。
    void parse_msbuild(R& r)
    {
        const auto full = make_inv(
            {"msbuild",
             "DemoSln.sln",
             "--config",
             "Debug",
             "--platform",
             "x64",
             "--export-only",
             "--msbuild-args",
             "-p:Foo=1",
             "--cl",
             "C:/cl.exe",
             "--msbuild",
             "C:/MSBuild.exe",
             "-o",
             "d",
             "-v",
             "--json"});
        cg::test::check(
            r,
            full.cmd == Invocation::Cmd::Msbuild,
            "msbuild command recognized");
        cg::test::check(
            r,
            full.msbuild_config == "Debug" && full.msbuild_platform == "x64"
                && full.msbuild_export_only && full.msbuild_args == "-p:Foo=1"
                && full.cl_path == "C:/cl.exe"
                && full.msbuild_path == "C:/MSBuild.exe" && full.db_explicit
                && full.verbose && full.json,
            "full msbuild option set round-trips");
        cg::test::check(
            r,
            full.positional.size() == 1,
            "the project operand stays positional");

        const auto all = make_inv({"msbuild", "proj/", "--all"});
        cg::test::check(
            r,
            all.msbuild_all && !all.db_explicit,
            "--all parses without -o");
        const auto all_o = make_inv({"msbuild", "proj/", "--all", "-o", "out"});
        cg::test::check(
            r,
            all_o.msbuild_all && all_o.db_explicit,
            "--all accepts a layout folder via -o (folder semantics)");
        const auto listed = make_inv({"msbuild", "--list"});
        cg::test::check(
            r,
            listed.msbuild_list && listed.positional.empty(),
            "--list takes no operand (cwd is the search dir)");
        const auto inline_cfg = make_inv({"msbuild", "p", "--config=Release"});
        cg::test::check(
            r,
            inline_cfg.msbuild_config == "Release",
            "--config= inline spelling");
        const auto evaluate = make_inv({"msbuild", "p"});
        cg::test::check(
            r,
            !evaluate.msbuild_record && !evaluate.msbuild_full_build,
            "evaluation is the default channel (no --record/--full-build)");
        const auto record = make_inv({"msbuild", "p", "--record"});
        cg::test::check(
            r,
            record.msbuild_record && !record.msbuild_full_build,
            "--record opts into the wrapped-capture channel");
        const auto rec_allow =
            make_inv({"msbuild", "p", "--record", "--allow-failure"});
        cg::test::check(
            r,
            rec_allow.msbuild_record && rec_allow.msbuild_allow_failure,
            "--record combines with --allow-failure (a build channel)");
        const auto fullbuild = make_inv({"msbuild", "p", "--full-build"});
        cg::test::check(
            r,
            fullbuild.msbuild_full_build,
            "--full-build opts into real compilation");

        const std::vector<std::vector<std::string>> bad = {
            {"msbuild"},
            {"msbuild", "p", "extra"},
            {"msbuild", "p", "--platform", "x6/4"},
            {"msbuild", "p", "--platform", "x 64"},
            {"msbuild", "p", "--platform", ""},
            {"msbuild", "p", "--config", "Debug|x64"},
            {"msbuild", "p", "--all", "--config", "Debug"},
            {"msbuild", "--list", "p", "--full-build"},
            {"msbuild", "--list", "p", "--record"},
            {"msbuild", "p", "--record", "--full-build"},
            {"msbuild", "p", "--allow-failure"},
            {"msbuild", "p", "--all", "--platform", "x64"},
            {"msbuild", "p", "--all", "--compile-commands", "c.json"},
            {"msbuild", "--list", "--all"},
            {"msbuild", "--list", "p", "--export-only"},
            {"msbuild", "--list", "p", "--config", "Debug"},
            {"msbuild", "p", "--bogus"},
            {"msbuild", "p", "--msbuild-args"},
            {"msbuild", "p", "--msbuild-args="},
            {"msbuild", "p", "--jobs", "65"},
            {"msbuild", "p", "--full"},
            {"msbuild", "p", "--dry-run"},
            {"msbuild", "p", "--llvm-root", "C:/llvm"},
        };
        for (const auto& args : bad)
        {
            bool threw = false;
            try
            {
                (void)make_inv(args);
            }
            catch (const cg::cli::UsageError&)
            {
                threw = true;
            }
            cg::test::check(
                r,
                threw,
                "usage error: ccg " +
                    [&]
                    {
                        std::string s;
                        for (const std::string& a : args)
                        {
                            s += a + ' ';
                        }
                        return s;
                    }());
        }

        cg::test::check(
            r,
            cg::cli::usage_for(Invocation::Cmd::Msbuild)
                    .find("usage: ccg msbuild")
                != std::string::npos,
            "msbuild usage block exists (help topic)");
        cg::test::check(
            r,
            cg::cli::name_of(Invocation::Cmd::Msbuild) == "msbuild",
            "name_of(msbuild)");
    }

    // ----------------------------------------------------------- config 命令
    // ------

    void config_command(R& r)
    {
        cg::test::check(
            r,
            make_inv({"config", "get"}).cmd == Invocation::Cmd::Config,
            "config parses into its command");
        cg::test::check(
            r,
            run_main({"config", "get", "llvm-path", "extra"}) == 2,
            "config rejects a 4th operand (usage)");
        cg::test::check(
            r,
            run_main({"config"}) == 2,
            "config without action exits 2");
        cg::test::check(
            r,
            run_main({"config", "frobnicate"}) == 2,
            "unknown config action exits 2");
        cg::test::check(
            r,
            run_main({"config", "set", "llvm-path"}) == 2,
            "config set without a value exits 2");
        cg::test::check(
            r,
            run_main({"config", "set", "no-such-key", "x"}) == 2,
            "unknown config key exits 2");
        cg::test::check(
            r,
            run_main({"config", "unset"}) == 2,
            "config unset without a key exits 2");
        cg::test::check(
            r,
            run_main(
                {"config",
                 "get",
                 "-o",
                 "no/such/ccg-dir",
                 "--config",
                 "missing"})
                == 1,
            "config get on a missing db exits 1");
        cg::test::check(
            r,
            run_main(
                {"config", "set", "llvm-path", "no/such/ccg-missing-prefix"})
                == 1,
            "config set with a nonexistent path exits 1");

        // 提示必须点名重建触发器（已批准设计：变更后
        // 的提示点名 `ccg index --full`）。
        const std::string set_adv =
            cg::cli::detail::llvm_path_set_advisory("E:/some/llvm");
        const std::string_view unset_adv =
            cg::cli::detail::llvm_path_unset_advisory();
        cg::test::check(
            r,
            set_adv.find("ccg index --full") != std::string::npos
                && set_adv.find("E:/some/llvm") != std::string::npos,
            "set advisory names the path and the rebuild command");
        cg::test::check(
            r,
            unset_adv.find("ccg index --full") != std::string::npos,
            "unset advisory names the rebuild command");

        // 临时库上的往返：set 建库并存下
        // 规范化路径；store API 读回；erase 恢复
        // 缺席（= 默认解析）状态。
        namespace fs = std::filesystem;
        cg::test::ScratchDir layout_guard{
            fs::temp_directory_path() / "ccg-cli-config-test"};
        const fs::path& layout = layout_guard.base;
        const fs::path  db = layout / "main" / "ccg.db";
        cg::test::check(
            r,
            run_main(
                {"config",
                 "set",
                 "llvm-path",
                 std::string{fixture_dir},
                 "-o",
                 layout.generic_string(),
                 "--config",
                 "main"})
                == 0,
            "config set creates the config dir db and exits 0");
        {
            db::GraphStore store{db.string()};
            const auto     v = store.meta("llvm_path");
            cg::test::check(
                r,
                v.has_value()
                    && *v
                           == cg::util::norm_path(
                               fs::path{fixture_dir}
                                   .lexically_normal()
                                   .generic_string()),
                "llvm-path round-trips through db meta, normalized");
        }
        cg::test::check(
            r,
            run_main(
                {"config",
                 "unset",
                 "llvm-path",
                 "-o",
                 layout.generic_string(),
                 "--config",
                 "main"})
                == 0,
            "config unset exits 0");
        {
            db::GraphStore store{db.string()};
            cg::test::check(
                r,
                !store.meta("llvm_path").has_value(),
                "unset removes the row (default resolution)");
        }

        // config get 分开显示引擎行：库记录的引擎
        // （产出语料时的权威身份）与当前环境
        // 解析并排；JSON 携带 recordedEngine/
        // recordedVersion 与 engineMismatch 旗标。
        {
            db::GraphStore store{db.string()};
            store.set_meta("llvm_engine_path", "E:/recorded/libclang.dll");
            store.set_meta("llvm_version", "clang version 23.1.3");
        }
        {
            const fs::path cap = layout / "get.json";
            FdRedirect     cap_stdout{1, cap, /*readable=*/false};
            const int      rc = run_main(
                {"config",
                 "get",
                 "--json",
                 "-o",
                 layout.generic_string(),
                 "--config",
                 "main"});
            cap_stdout.restore();
            cg::test::check(r, rc == 0, "config get --json exits 0");
            const json::Document doc = json::Document::parse(
                cg::util::read_text_file(cap.string()).value_or("{}"));
            const json::ValueView root = doc.root();
            const auto            rec = root.get("recordedEngine");
            const auto            rec_v = root.get("recordedVersion");
            const auto            mismatch = root.get("engineMismatch");
            cg::test::check(
                r,
                rec && rec->is_string()
                    && *rec->as_string()
                           == std::string_view{"E:/recorded/libclang.dll"}
                    && rec_v
                    && *rec_v->as_string()
                           == std::string_view{"clang version 23.1.3"},
                "get reports the db-recorded engine identity");
            cg::test::check(
                r,
                mismatch && mismatch->is_bool(),
                "get reports engineMismatch as a flag");
        }

        // `ccg update` 协调旋钮（排队租约协议）：整数
        // 键存于各自的 meta 名下；非正值是
        // 运行时错误，未设置恢复 schema 默认值。
    }

    // update 协调旋钮（排队租约协议）的 set/get/unset 面：毫秒键
    // 非正值拒绝、畸形矩阵全拒且不写库、报错文案带原始输入。
    void config_update_knobs(R& r)
    {
        namespace fs = std::filesystem;
        cg::test::ScratchDir layout_guard{
            fs::temp_directory_path() / "ccg-cli-config-update"};
        const fs::path& layout = layout_guard.base;
        const fs::path  db = layout / "main" / "ccg.db";
        cg::test::check(
            r,
            run_main(
                {"config",
                 "set",
                 "update-lease-ms",
                 "120000",
                 "-o",
                 layout.generic_string(),
                 "--config",
                 "main"})
                == 0,
            "config set update-lease-ms exits 0");
        cg::test::check(
            r,
            run_main(
                {"config",
                 "set",
                 "update-busy-timeout-ms",
                 "30000",
                 "-o",
                 layout.generic_string(),
                 "--config",
                 "main"})
                == 0,
            "config set update-busy-timeout-ms exits 0");
        {
            db::GraphStore store{db.string()};
            cg::test::check(
                r,
                store.meta("update_lease_ms").value_or("") == "120000",
                "update-lease-ms round-trips through db meta");
            cg::test::check(
                r,
                store.meta("update_busy_timeout_ms").value_or("") == "30000",
                "update-busy-timeout-ms round-trips through db meta");
        }
        cg::test::check(
            r,
            run_main(
                {"config",
                 "set",
                 "update-lease-ms",
                 "0",
                 "-o",
                 layout.generic_string(),
                 "--config",
                 "main"})
                == 1,
            "config set update-lease-ms 0 exits 1 (positive only)");
        // 数值键严格化（行为变更波）：用户手敲输入与命令行数字选项同一
        // 语义——全串纯数字。畸形矩阵全部退出非 0 且不写库（此前宽容
        // atoll 会静默吞尾："5000abc"存 5000、"1e5"存 1、"12,000"存 12，
        // 均退出 0——用当树二进制端到端实测复现过）。
        for (const std::string bad :
             {"120000abc",
              "+120000",
              " 120000",
              "1e5",
              "12,000",
              "-5",
              "0",
              "",
              "999999999999999999999999"})
        {
            cg::test::check(
                r,
                run_main(
                    {"config",
                     "set",
                     "update-lease-ms",
                     bad,
                     "-o",
                     layout.generic_string(),
                     "--config",
                     "main"})
                    != 0,
                std::format(
                    "config set update-lease-ms '{}' is "
                    "rejected (strict integer semantics)",
                    bad));
        }
        {
            // 回归钉：矩阵全拒后库内旧值必须保持 120000——防"报错了
            // 但已写库"（现状实测："1e5" 曾把 90000 覆盖成 1）。
            db::GraphStore store{db.string()};
            cg::test::check(
                r,
                store.meta("update_lease_ms").value_or("") == "120000",
                "rejected malformed set never wrote the db");
        }
        {
            // 错误文案必须带原始输入值（用户照着改），捕 stderr 验证一条
            // 代表形态。
            const fs::path cap = layout / "set-bad.err";
            FdRedirect     cap_stderr{2, cap, /*readable=*/false};
            const int      rc = run_main(
                {"config",
                 "set",
                 "update-lease-ms",
                 "5000abc",
                 "-o",
                 layout.generic_string(),
                 "--config",
                 "main"});
            cap_stderr.restore();
            cg::test::check(r, rc != 0, "config set '5000abc' exits nonzero");
            cg::test::check(
                r,
                cg::util::read_text_file(cap.string())
                        .value_or("")
                        .find("5000abc")
                    != std::string::npos,
                "rejection message quotes the original input");
        }
        cg::test::check(
            r,
            run_main(
                {"config",
                 "get",
                 "update-lease-ms",
                 "-o",
                 layout.generic_string(),
                 "--config",
                 "main"})
                == 0,
            "config get update-lease-ms exits 0");
        cg::test::check(
            r,
            run_main(
                {"config",
                 "unset",
                 "update-lease-ms",
                 "-o",
                 layout.generic_string(),
                 "--config",
                 "main"})
                == 0,
            "config unset update-lease-ms exits 0");
        {
            db::GraphStore store{db.string()};
            cg::test::check(
                r,
                !store.meta("update_lease_ms").has_value(),
                "unset removes the knob row (schema default)");
        }
    }

    // index-jobs 与 clang-compile-flags 两键的 set/get/unset 面：0-64
    // 边界与畸形矩阵、字符串键空值用法错误。
    void config_index_jobs_and_flags(R& r)
    {
        namespace fs = std::filesystem;
        cg::test::ScratchDir layout_guard{
            fs::temp_directory_path() / "ccg-cli-config-jobs"};
        const fs::path& layout = layout_guard.base;
        const fs::path  db = layout / "main" / "ccg.db";
        // index-jobs（并发上限键）：与毫秒旋钮同一严格解析纪律，但 0 合法
        // （显式回自动）、1-64 生效——与 update-lease-ms 的"非正值拒绝"分支
        // 相对照。写库值规范化（to_string），读回即生效形态。
        cg::test::check(
            r,
            run_main(
                {"config",
                 "set",
                 "index-jobs",
                 "3",
                 "-o",
                 layout.generic_string(),
                 "--config",
                 "main"})
                == 0,
            "config set index-jobs 3 exits 0");
        {
            db::GraphStore store{db.string()};
            cg::test::check(
                r,
                store.meta("index_jobs").value_or("") == "3",
                "index-jobs round-trips through db meta");
        }
        cg::test::check(
            r,
            run_main(
                {"config",
                 "set",
                 "index-jobs",
                 "64",
                 "-o",
                 layout.generic_string(),
                 "--config",
                 "main"})
                == 0,
            "config set index-jobs 64 (upper bound) exits 0");
        cg::test::check(
            r,
            run_main(
                {"config",
                 "set",
                 "index-jobs",
                 "1",
                 "-o",
                 layout.generic_string(),
                 "--config",
                 "main"})
                == 0,
            "config set index-jobs 1 (lower bound) exits 0");
        cg::test::check(
            r,
            run_main(
                {"config",
                 "set",
                 "index-jobs",
                 "0",
                 "-o",
                 layout.generic_string(),
                 "--config",
                 "main"})
                == 0,
            "config set index-jobs 0 exits 0 (explicit auto)");
        {
            db::GraphStore store{db.string()};
            cg::test::check(
                r,
                store.meta("index_jobs").value_or("") == "0",
                "index-jobs 0 is stored (contrast: ms knobs reject 0)");
        }
        // 畸形矩阵（复用毫秒键的严格化路径）：全串纯数字之外一律拒绝，
        // 越界（65）与空串同理；全部退出非 0 且不写库（防"报错了但已写"）。
        for (const std::string bad :
             {"3abc",
              "+3",
              " 3",
              "1e5",
              "12,000",
              "-5",
              "65",
              "",
              "999999999999999999999999"})
        {
            cg::test::check(
                r,
                run_main(
                    {"config",
                     "set",
                     "index-jobs",
                     bad,
                     "-o",
                     layout.generic_string(),
                     "--config",
                     "main"})
                    != 0,
                std::format(
                    "config set index-jobs '{}' is rejected "
                    "(strict integer semantics)",
                    bad));
        }
        {
            db::GraphStore store{db.string()};
            cg::test::check(
                r,
                store.meta("index_jobs").value_or("") == "0",
                "rejected malformed index-jobs never wrote the db");
        }
        cg::test::check(
            r,
            run_main(
                {"config",
                 "get",
                 "index-jobs",
                 "-o",
                 layout.generic_string(),
                 "--config",
                 "main"})
                == 0,
            "config get index-jobs exits 0");
        cg::test::check(
            r,
            run_main(
                {"config",
                 "unset",
                 "index-jobs",
                 "-o",
                 layout.generic_string(),
                 "--config",
                 "main"})
                == 0,
            "config unset index-jobs exits 0");
        {
            db::GraphStore store{db.string()};
            cg::test::check(
                r,
                !store.meta("index_jobs").has_value(),
                "unset index-jobs restores the schema default (auto)");
        }
        // clang-compile-flags（字符串键）：set/get/unset round-trip；空值
        // 是用法错误（清空走 unset）；重放层经 db meta 消费。
        cg::test::check(
            r,
            run_main(
                {"config",
                 "set",
                 "clang-compile-flags",
                 "-DFOO -isystem C:/vendor/include",
                 "-o",
                 layout.generic_string(),
                 "--config",
                 "main"})
                == 0,
            "config set clang-compile-flags exits 0");
        {
            db::GraphStore store{db.string()};
            cg::test::check(
                r,
                store.meta("clang_compile_flags").value_or("")
                    == "-DFOO -isystem C:/vendor/include",
                "clang-compile-flags round-trips through db meta");
        }
        cg::test::check(
            r,
            run_main(
                {"config",
                 "get",
                 "clang-compile-flags",
                 "-o",
                 layout.generic_string(),
                 "--config",
                 "main"})
                == 0,
            "config get clang-compile-flags exits 0");
        cg::test::check(
            r,
            run_main(
                {"config",
                 "set",
                 "clang-compile-flags",
                 "",
                 "-o",
                 layout.generic_string(),
                 "--config",
                 "main"})
                == 2,
            "config set clang-compile-flags '' is a usage error");
        cg::test::check(
            r,
            run_main(
                {"config",
                 "unset",
                 "clang-compile-flags",
                 "-o",
                 layout.generic_string(),
                 "--config",
                 "main"})
                == 0,
            "config unset clang-compile-flags exits 0");
    }

    // ------------------------------- 并发上限覆盖链（resolve_jobs
    // 三态）--------

    /// 路径的规范化字符串形态（词法规范化 + norm_path）。
    [[nodiscard]]
    auto norm_lex(const std::filesystem::path& p) -> std::string
    {
        return cg::util::norm_path(p.lexically_normal().generic_string());
    }

    /// 根下的 cdb 发现结果（未发现给空串；命中按规范化形态返回）。
    [[nodiscard]]
    auto find_compile_db_norm(const std::filesystem::path& root) -> std::string
    {
        const auto hit =
            cg::cli::detail::find_compile_db(root.generic_string());
        return hit.has_value() ? norm_lex(std::filesystem::path{*hit})
                               : std::string{};
    }

    /// 直接写库的 index_jobs meta（手改库形态的读侧防御用例）。
    void set_index_jobs_meta(
        const std::filesystem::path& db_path,
        const char*                  raw)
    {
        db::GraphStore writer{db_path.string()};
        writer.set_meta("index_jobs", raw);
    }

    void resolve_jobs_priority(R& r)
    {
        // 覆盖链：--jobs 旗标 > 库配置键 index_jobs > 自动按核。自动档基
        // 准取 cg::util::pool::auto_worker_count（单一事实源），不在本测试
        // 里复算 clamp 公式——调参变化不该连坐测试。
        namespace fs = std::filesystem;
        cg::test::ScratchDir layout_guard{
            fs::temp_directory_path() / "ccg-cli-jobs-test"};
        const fs::path    db = layout_guard.base / "main" / "ccg.db";
        const std::string o = layout_guard.base.generic_string();
        cg::test::check(
            r,
            run_main(
                std::vector<std::string>{
                    "config",
                    "set",
                    "index-jobs",
                    "4",
                    "-o",
                    o,
                    "--config",
                    "main"})
                == 0,
            "resolve_jobs fixture: config set index-jobs 4 exits 0");
        {
            // 三态之一（默认自动）：旗标缺席、库缺失（首次 index 形态）必
            // 须落自动档——窥探连接打不开不是错误面。
            Invocation bare = make_inv({"index", "r", "--config", "m"});
            bare.db_path = (layout_guard.base / "missing" / "ccg.db").string();
            cg::test::check(
                r,
                cg::cli::detail::resolve_jobs(bare)
                    == cg::util::pool::auto_worker_count(),
                "no flag, no db -> auto by cores");
        }
        {
            // 三态之二（配置生效）：旗标缺席、库配置 4 在场——路由阶段的
            // 自带只读连接与索引尾部的已开连接两种形态读同值。
            Invocation inv = make_inv({"index", "r", "--config", "m"});
            inv.db_path = db.string();
            cg::test::check(
                r,
                cg::cli::detail::resolve_jobs(inv) == 4,
                "the index-jobs config key applies without a flag");
            db::GraphStore store{db.string()};
            cg::test::check(
                r,
                cg::cli::detail::resolve_jobs(inv, &store) == 4,
                "an already-open store reads the same value");
            // 三态之首（旗标压配置）：显式 --jobs 直接胜出，不再读库。
            Invocation flagged =
                make_inv({"index", "r", "--config", "m", "--jobs", "8"});
            flagged.db_path = db.string();
            cg::test::check(
                r,
                cg::cli::detail::resolve_jobs(flagged) == 8,
                "the --jobs flag overrides the config key");
            cg::test::check(
                r,
                cg::cli::detail::resolve_jobs(flagged, &store) == 8,
                "the flag wins with an open store too");
        }
        {
            // 读侧防御：手改库写出的畸形 meta 按未配置处理（0 = 显式回自
            // 动、越界、非数字、空串），边界 1/64 恰好生效。
            Invocation inv = make_inv({"index", "r", "--config", "m"});
            inv.db_path = db.string();
            for (const char* raw :
                 {"0", "65", "abc", "-3", "1e5", "12,000", ""})
            {
                set_index_jobs_meta(db, raw);
                db::GraphStore store{db.string()};
                cg::test::check(
                    r,
                    cg::cli::detail::resolve_jobs(inv, &store)
                        == cg::util::pool::auto_worker_count(),
                    std::format(
                        "meta index_jobs '{}' falls back "
                        "to auto",
                        raw));
            }
            for (const std::pair<const char*, int> ok :
                 {std::pair{"1", 1}, std::pair{"64", 64}})
            {
                set_index_jobs_meta(db, ok.first);
                db::GraphStore store{db.string()};
                cg::test::check(
                    r,
                    cg::cli::detail::resolve_jobs(inv, &store) == ok.second,
                    std::format("meta index_jobs '{}' is honored", ok.first));
            }
        }
    }

    // --------------------- 注入重放旗标 + msbuild schema 重建 ---------------

    void injected_flags_and_schema_rebuild(R& r)
    {
        // 解析层：可重复、按出现序拼接、引号感知切词；index/update/msbuild
        // 接受，查询命令拒绝。
        const auto once = make_inv(
            {"index",
             "root",
             "--config",
             "c",
             "--clang-compile-flags",
             "-DFOO"});
        cg::test::check(
            r,
            once.clang_compile_flags == std::vector<std::string>{"-DFOO"},
            "--clang-compile-flags records one token");
        const auto twice = make_inv(
            {"index",
             "root",
             "--config",
             "c",
             "--clang-compile-flags",
             "-DFOO -isystem C:/v/i",
             "--clang-compile-flags",
             "-DBAR"});
        cg::test::check(
            r,
            twice.clang_compile_flags
                == std::vector<
                    std::string>{"-DFOO", "-isystem", "C:/v/i", "-DBAR"},
            "repeated --clang-compile-flags accumulate in order "
            "(quoted value splits into tokens)");
        cg::test::check(
            r,
            make_inv({"update", "a.cpp", "--clang-compile-flags", "-DX"})
                    .clang_compile_flags
                == std::vector<std::string>{"-DX"},
            "update accepts --clang-compile-flags");
        cg::test::check(
            r,
            !make_inv({"msbuild", "p", "--clang-compile-flags", "-DX"})
                 .clang_compile_flags.empty(),
            "msbuild accepts --clang-compile-flags");
        bool query_rejected = false;
        try
        {
            (void)make_inv({"query", "foo", "--clang-compile-flags", "-DX"});
        }
        catch (const cg::cli::UsageError&)
        {
            query_rejected = true;
        }
        cg::test::check(
            r,
            query_rejected,
            "query rejects --clang-compile-flags (usage error)");
        bool empty_rejected = false;
        try
        {
            (void)make_inv(
                {"index", "r", "--config", "c", "--clang-compile-flags", " "});
        }
        catch (const cg::cli::UsageError&)
        {
            empty_rejected = true;
        }
        cg::test::check(
            r,
            empty_rejected,
            "--clang-compile-flags with no tokens is a usage error");

        // 重放层注入：每条 argv（含 PCH 文本回退副本）末尾追加；无旗标时
        // 逐字节不变。
        cg::clang::CompileCommand cmd;
        cmd.file = "e:/x/a.cpp";
        cmd.argv = {"-std=c++20", "-I", "e:/x"};
        cmd.pch_fallback_argv = {"-std=c++20"};
        std::vector<cg::clang::CompileCommand> one{cmd};
        Invocation no_flags = make_inv({"index", "r", "--config", "c"});
        no_flags.db_path = ":memory:";
        cg::cli::detail::apply_clang_compile_flags(no_flags, one);
        cg::test::check(
            r,
            one.front().argv == cmd.argv
                && one.front().pch_fallback_argv == cmd.pch_fallback_argv,
            "no flags -> argv byte-for-byte unchanged");

        Invocation cli_flags = make_inv(
            {"index",
             "r",
             "--config",
             "c",
             "--clang-compile-flags",
             "-DCLI1",
             "--clang-compile-flags",
             "-DCLI2"});
        cli_flags.db_path = ":memory:";
        cg::cli::detail::apply_clang_compile_flags(cli_flags, one);
        cg::test::check(
            r,
            one.front().argv
                    == std::vector<
                        std::
                            string>{"-std=c++20", "-I", "e:/x", "-DCLI1", "-DCLI2"}
                && one.front().pch_fallback_argv
                       == std::vector<
                           std::string>{"-std=c++20", "-DCLI1", "-DCLI2"},
            "flags append to the tail of every replayed argv "
            "(and the PCH text-fallback twin)");

        // 优先级（命令行 > 配置键 > 无）与配置键持久化消费。
        namespace fs = std::filesystem;
        cg::test::ScratchDir layout_guard{
            fs::temp_directory_path() / "ccg-cli-flags-test"};
        const fs::path& layout = layout_guard.base;
        const fs::path  db = layout / "main" / "ccg.db";
        std::error_code ec;
        fs::create_directories(db.parent_path(), ec);
        {
            db::GraphStore store{db.string()};
            store.set_meta("clang_compile_flags", "-DCFG");
        }
        Invocation from_cfg = make_inv({"index", "r", "--config", "c"});
        from_cfg.db_path = db.generic_string();
        cg::test::check(
            r,
            cg::cli::detail::resolve_clang_compile_flags(from_cfg)
                == std::vector<std::string>{"-DCFG"},
            "config key alone injects its tokens");
        cg::test::check(
            r,
            cg::cli::detail::resolve_clang_compile_flags(cli_flags)
                == std::vector<std::string>{"-DCLI1", "-DCLI2"},
            "command-line flags beat the config key for the run");
        Invocation no_source = make_inv({"index", "r", "--config", "c"});
        no_source.db_path = db.string() + "-missing.db";
        cg::test::check(
            r,
            cg::cli::detail::resolve_clang_compile_flags(no_source).empty(),
            "no CLI flags and no db -> no injection");

        // msbuild 的 schema 失配默认重建（不再要求 --force）；index 保持
        // 既有契约（无 --force 拒绝打开）。
        const fs::path schema_db = layout / "schema" / "ccg.db";
        fs::create_directories(schema_db.parent_path(), ec);
        {
            db::GraphStore store{schema_db.string()};
            store.set_meta("schema_version", "11"); // 模拟旧代库
        }
        Invocation msbuild_inv = make_inv({"msbuild", "p"});
        msbuild_inv.db_path = schema_db.string();
        bool msbuild_recovered = false;
        try
        {
            db::GraphStore rebuilt =
                cg::cli::detail::open_store_create(msbuild_inv);
            msbuild_recovered = rebuilt.meta("schema_version").value_or("")
                                == std::to_string(db::schema_version);
        }
        catch (const std::exception&)
        {
        }
        cg::test::check(
            r,
            msbuild_recovered,
            "ccg msbuild recreates a schema-mismatched db by default");
        // 重建后再老化一次：index 无 --force 必须拒绝（既有契约不回退）。
        {
            db::GraphStore store{schema_db.string()};
            store.set_meta("schema_version", "11");
        }
        Invocation index_inv = make_inv({"index", "r", "--config", "schema"});
        index_inv.db_path = schema_db.string();
        bool index_threw = false;
        try
        {
            (void)cg::cli::detail::open_store_create(index_inv);
        }
        catch (const db::SchemaMismatchError&)
        {
            index_threw = true;
        }
        cg::test::check(
            r,
            index_threw,
            "ccg index without --force still refuses a "
            "schema-mismatched db");

        // 摘要行：文本回退两结局可读出。总数恒等于两结局之和（索引器按
        // 运行口径聚合；规划口径与运行口径曾在同一行打架——
        // 实测词句 "333 text fallbacks (155 parse ok, 450 still failed)"）。
        cg::graph::IndexSummary sum;
        sum.pch_fallbacks = 605;
        sum.pch_fallback_ok = 155;
        sum.pch_fallback_failed = 450;
        const std::string text =
            cg::cli::render::index_text(sum, "e:/x/ccg.db");
        cg::test::check(
            r,
            text.find(
                "605 text fallbacks (155 fallback parse ok, "
                "450 still failed)")
                != std::string::npos,
            "summary line separates fallback-recovered from "
            "still-failed and the total stays the identity "
            "ok + failed");
    }

    // -------------------------------------------------- update：解析 + 锁
    // --------

    void parse_update(R& r)
    {
        const auto up = make_inv({"update", "a.cpp", "b.hpp", "-o", "x"});
        cg::test::check(
            r,
            up.cmd == Invocation::Cmd::Update && up.positional.size() == 2
                && !up.drain,
            "update takes many files, drain defaults off");
        cg::test::check(
            r,
            make_inv({"update", "--drain"}).drain,
            "update --drain parses with no files");
        cg::test::check(
            r,
            make_inv({"update", "a.cpp", "--drain"}).drain
                && make_inv({"update", "a.cpp", "--drain"}).positional.size()
                       == 1,
            "update accepts files AND --drain together");
        cg::test::check(
            r,
            make_inv({"update", "a.cpp", "--compile-commands", "f.json"})
                    .compile_db
                == "f.json",
            "update --compile-commands overrides the recorded db");
        cg::test::check(
            r,
            make_inv({"update", "a.cpp", "--llvm-root", "L"}).llvm_root == "L",
            "update --llvm-root parses");

        const std::vector<std::vector<std::string>> bad = {
            {"update"},
            {"update", "a.cpp", "--paranoid"},
            {"update", "a.cpp", "--full"},
            {"update", "a.cpp", "--dry-run"},
            {"update", "a.cpp", "--jobs", "0"},
            {"update", "a.cpp", "--depth", "2"},
            {"query", "--drain", "x"},
            {"index", "--drain", "root"},
            {"update", "a.cpp", "--from-file", "f"},
        };
        for (const auto& args : bad)
        {
            bool threw = false;
            try
            {
                (void)make_inv(args);
            }
            catch (const cg::cli::UsageError&)
            {
                threw = true;
            }
            std::string rendered;
            for (const auto& a : args)
            {
                rendered += rendered.empty() ? a : (" " + a);
            }
            cg::test::check(r, threw, "usage error for '" + rendered + "'");
        }
    }

    // 库协调 CLI 面（排队租约 + 偷取仲裁协议）：文件清单规范化
    // （ccg changes 约定）、每进程 claim 令牌，与 RAII
    // claim 守卫的持有者范围释放。enqueue/take/claim/lease
    // 语义本身在 cg_test_db（db.realtime_coordination）
    // 里对着两个连接做单元测试。
    void update_coordination(R& r)
    {
        namespace fsn = std::filesystem;
        cg::test::ScratchDir scratch{
            fsn::absolute(fsn::path{"ccg-cli-update-scratch"})};
        const fsn::path&  base = scratch.base;
        const std::string db_path = norm_lex(base / "ccg.db");

        // normalize_update_files：绝对化 + 词法规范化 + norm_path，
        // 按规范化形态去重，保留喂入顺序。
        const std::string a_norm = norm_lex(base / "a.cpp");
        const std::string b_norm = norm_lex(fsn::absolute(fsn::path{"b.cpp"}));
        const std::vector<std::string> files =
            cg::cli::detail::normalize_update_files(
                {(base / "a.cpp").generic_string(),
                 (base / "." / "a.cpp").generic_string(), // same file, './'
                 "b.cpp"});                               // relative spelling
        cg::test::check(
            r,
            files.size() == 2 && files[0] == a_norm && files[1] == b_norm,
            "normalize_update_files dedupes normalized spellings "
            "and keeps feed order");
        cg::test::check(
            r,
            cg::cli::detail::normalize_update_files({}).empty(),
            "normalize_update_files of no files is empty");

        // claim 令牌点名进程且每次调用唯一（同一
        // 进程的两次更新持有不同 claim）。
        const std::string t1 = cg::cli::detail::update_claim_token();
        const std::string t2 = cg::cli::detail::update_claim_token();
        cg::test::check(
            r,
            t1 != t2 && t1.find("pid:") != std::string::npos,
            "claim tokens are unique per call and name the pid");

        // 守卫在作用域结束时释放我们自己的 claim 行——只放自己的：
        // 为已过期且被抢的 claim 而设的守卫必须
        // 留着抢夺者的行不动。
        {
            db::GraphStore store{db_path};
            cg::test::check(
                r,
                store.claim_update(60'000, "guard-holder"),
                "claim for the guard test is granted");
            {
                const cg::cli::detail::UpdateClaimGuard guard{
                    store,
                    "guard-holder"};
            }
            cg::test::check(
                r,
                !store.update_claim_holder().has_value(),
                "the guard's destructor released the claim");
        }
        {
            db::GraphStore store{db_path};
            cg::test::check(
                r,
                store.claim_update(0, "stale-holder"),
                "a zero-lease claim is granted (instantly stale)");
            db::GraphStore thief{db_path};
            cg::test::check(
                r,
                thief.claim_update(60'000, "thief"),
                "the thief steals the expired claim");
            {
                const cg::cli::detail::UpdateClaimGuard guard{
                    store,
                    "stale-holder"};
            }
            const auto row = thief.update_claim_holder();
            cg::test::check(
                r,
                row.has_value() && row->first == "thief",
                "a stale holder's guard does not delete the "
                "thief's claim");
        }
    }

    // ------------------------------------------------------------- hook 编辑器
    // ---- `ccg hook` 设置编辑器核心，全部在内存中演练（文件/ 拉起两面走
    // e2e.cmake 的 hook 闸门配真实 二进制）：带 span 的 JSONC
    // 解析、所有权探针，与 install/uninstall 编辑对的三个契约点——收敛
    // （重复安装不再加条目）、保留（用户邻居
    // 与注释幸存）与零残留（卸载恢复
    // 安装前文档）。

    namespace hk = cg::cli::detail;

    [[nodiscard]]
    auto parse_settings(const std::string& text)
        -> std::optional<hk::SettingsNode>
    {
        hk::SettingsParser parser{text};
        return parser.parse_document();
    }

    // 在内存文档上镜像 hook_install 的编辑半边：先
    // 扫出自家条目，再追加新条目，缺席时组合出
    // "hooks" 成员（按规格搭脚手架会铸出重复键）。
    [[nodiscard]]
    auto sim_hook_install(
        const std::string&              text,
        const std::vector<std::string>& commands) -> std::string
    {
        const auto root = parse_settings(text);
        if (!root.has_value() || root->type != hk::SettingsNode::Type::Object)
        {
            return text;
        }
        hk::SettingsEditor    editor{text};
        const hk::SweepResult sweep =
            hk::sweep_own_entries(*root, editor, /*prune_empty=*/false);
        if (root->member("hooks") == nullptr)
        {
            std::string events;
            for (std::size_t i = 0; i < std::size(hk::HOOK_SPECS); ++i)
            {
                events += std::format(
                    "{}\"{}\": [{}]",
                    i == 0 ? "" : ",",
                    hk::HOOK_SPECS[i].event,
                    hk::entry_text(hk::HOOK_SPECS[i], commands[i]));
            }
            editor.insert(
                root->keys.empty() ? root->after_open : root->values.back().end,
                "\"hooks\": {" + events + "}",
                root->keys.empty());
        }
        else
        {
            for (std::size_t i = 0; i < std::size(hk::HOOK_SPECS); ++i)
            {
                hk::append_entry(
                    *root,
                    editor,
                    hk::HOOK_SPECS[i],
                    commands[i],
                    sweep.last_kept[i]);
            }
        }
        return editor.render();
    }

    // 镜像 hook_uninstall 的清扫（元素级移除加空容器
    // 修剪）于一份内存文档。
    [[nodiscard]]
    auto sim_hook_uninstall(const std::string& text) -> std::string
    {
        const auto root = parse_settings(text);
        if (!root.has_value() || root->type != hk::SettingsNode::Type::Object)
        {
            return text;
        }
        hk::SettingsEditor editor{text};
        (void)hk::sweep_own_entries(*root, editor, /*prune_empty=*/true);
        return editor.render();
    }

    // 按受管事件数自家命令条目，与 hook_status
    // 做的遍历相同。
    [[nodiscard]]
    auto count_own_entries(const std::optional<hk::SettingsNode>& root)
        -> std::vector<int>
    {
        std::vector<int> counts(std::size(hk::HOOK_SPECS), 0);
        if (!root.has_value() || root->type != hk::SettingsNode::Type::Object)
        {
            return counts;
        }
        const hk::SettingsNode* hooks = root->member("hooks");
        if (hooks == nullptr || hooks->type != hk::SettingsNode::Type::Object)
        {
            return counts;
        }
        for (std::size_t s = 0; s < std::size(hk::HOOK_SPECS); ++s)
        {
            const hk::SettingsNode* event =
                hooks->member(hk::HOOK_SPECS[s].event);
            if (event == nullptr
                || event->type != hk::SettingsNode::Type::Array)
            {
                continue;
            }
            for (const hk::SettingsNode& entry : event->elements)
            {
                if (entry.type != hk::SettingsNode::Type::Object)
                {
                    continue;
                }
                const hk::SettingsNode* commands = entry.member("hooks");
                if (commands == nullptr
                    || commands->type != hk::SettingsNode::Type::Array)
                {
                    continue;
                }
                for (const hk::SettingsNode& command : commands->elements)
                {
                    if (command.type != hk::SettingsNode::Type::Object)
                    {
                        continue;
                    }
                    const hk::SettingsNode* text = command.member("command");
                    if (text != nullptr
                        && text->type == hk::SettingsNode::Type::String
                        && hk::is_our_command(text->text, hk::HOOK_SPECS[s]))
                    {
                        ++counts[s];
                    }
                }
            }
        }
        return counts;
    }

    void hook_settings_editor(R& r)
    {
        // 所有权探针：设置文件可能携带的两种拼写——
        // 插件通道的 `ccg hook <action>` 与弹跳脚本
        // 路径——都算自家的；邻居与其它事件不算。
        const hk::HookSpec& ptu = hk::HOOK_SPECS[0];
        cg::test::check(
            r,
            hk::is_our_command("ccg hook posttooluse", ptu),
            "needle accepts the plugin-channel spelling");
        cg::test::check(
            r,
            hk::is_our_command(
                "\"/opt/ccg-home/.ccg/hooks/ccg-posttooluse.cmd\"",
                ptu),
            "needle accepts the trampoline-script spelling");
        cg::test::check(
            r,
            !hk::is_our_command("ccg hook sessionstart", ptu)
                && !hk::is_our_command("echo user-hook", ptu),
            "needle rejects foreign and sibling-event commands");

        // 第三个受管事件与前两个动作无前缀
        // 碰撞（install/uninstall 必须认得两种拼写且只认
        // 自己的事件）。
        const hk::HookSpec& ups = hk::HOOK_SPECS[2];
        cg::test::check(
            r,
            std::string_view{ups.event} == "UserPromptSubmit"
                && hk::is_our_command("ccg hook userpromptsubmit", ups)
                && hk::is_our_command(
                    "\"/opt/ccg-home/.ccg/hooks/ccg-userpromptsubmit.cmd\"",
                    ups)
                && !hk::is_our_command("ccg hook posttooluse", ups)
                && !hk::is_our_command("ccg hook sessionstart", ups)
                && !hk::is_our_command("ccg hook userpromptsubmit", ptu),
            "needle handles the UserPromptSubmit trampoline without prefix "
            "collisions");

        // 第四个受管事件（PreToolUse 六工具监听）保有自己的探针与
        // 匹配器；matcher 覆盖 Grep 原生 + Bash/PowerShell 命令行 +
        // Search 载荷直读 + Read/Glob 纯投递载体，条目不得与任何邻居
        // 相撞。
        const hk::HookSpec& ptu_grep = hk::HOOK_SPECS[3];
        cg::test::check(
            r,
            std::string_view{ptu_grep.event} == "PreToolUse"
                && std::string_view{ptu_grep.matcher}
                       == "Grep|Bash|PowerShell|Search|Read|Glob"
                && hk::is_our_command("ccg hook pretooluse", ptu_grep)
                && hk::is_our_command(
                    "\"/opt/ccg-home/.ccg/hooks/ccg-pretooluse.sh\"",
                    ptu_grep)
                && !hk::is_our_command("ccg hook posttooluse", ptu_grep)
                && !hk::is_our_command("ccg hook pretooluse", ptu),
            "needle handles the PreToolUse trampoline and matcher");

        // 语料闸门：C/C++ 源刷新图，其余一切
        // 沉默。大写 .CPP 同样命中（修复：旧表字节精确比较把
        // 大写扩展静默漏判，钩子对整个文件失明）。
        cg::test::check(
            r,
            hk::is_corpus_file("dir/a.cpp") && hk::is_corpus_file("b.hpp")
                && hk::is_corpus_file("c.ixx") && hk::is_corpus_file("d.cppm")
                && hk::is_corpus_file("e.CPP"),
            "corpus extensions gate the PostToolUse refresh");
        cg::test::check(
            r,
            !hk::is_corpus_file("readme.md")
                && !hk::is_corpus_file("trampoline.cmd")
                && !hk::is_corpus_file("no-extension"),
            "non-corpus extensions stay silent");

        // 有界上下文：名字掐头、路径掐尾。
        cg::test::check(
            r,
            hk::clip("abcdef", 3) == "abc…",
            "clip cuts with an ellipsis");
        cg::test::check(
            r,
            hk::clip("ab", 3) == "ab"
                && hk::clip_tail("/long/path/file.cpp", 8) == "…file.cpp",
            "short strings pass through; long paths keep the tail");

        // JSONC 解析：注释、尾逗号、空文档、span。
        const auto empty_root = parse_settings("");
        const auto blank_root = parse_settings("// only a comment\n");
        cg::test::check(
            r,
            !empty_root.has_value() && !blank_root.has_value(),
            "empty and comment-only documents parse to nullopt");
        const auto doc = parse_settings(
            "{\n  // line comment\n  \"a\": [1, 2,], /* block */ \"b\": {\n}\n}");
        cg::test::check(
            r,
            doc.has_value() && doc->keys.size() == 2
                && doc->values[0].type == hk::SettingsNode::Type::Array
                && doc->values[0].elements.size() == 2,
            "JSONC dialect parses comments and trailing commas");
        bool threw = false;
        try
        {
            (void)parse_settings("{\"a\": }");
        }
        catch (const std::runtime_error&)
        {
            threw = true;
        }
        cg::test::check(
            r,
            threw,
            "malformed settings throw (installer "
            "refuses, never rewrites)");
        threw = false;
        try
        {
            (void)parse_settings("{\"a\": 1} trailing");
        }
        catch (const std::runtime_error& e)
        {
            threw = std::string_view{e.what()}.find("trailing")
                    != std::string_view::npos;
        }
        cg::test::check(r, threw, "trailing content after the root is refused");

        // 对用户维护文档的安装/卸载往返：
        // 注释、键序与邻居用户钩子必须幸存。
        const std::vector<std::string> commands{
            "\"/opt/ccg-home/.ccg/hooks/ccg-posttooluse.cmd\"",
            "\"/opt/ccg-home/.ccg/hooks/ccg-sessionstart.cmd\"",
            "\"/opt/ccg-home/.ccg/hooks/ccg-userpromptsubmit.cmd\"",
            "\"/opt/ccg-home/.ccg/hooks/ccg-pretooluse.cmd\""};
        const std::string user = R"({
  // user settings with comments
  "model": "sonnet",
  "hooks": {
    "PostToolUse": [
      {
        // my own hook, keep it
        "matcher": "Bash",
        "hooks": [
          {"type": "command", "command": "echo user-hook", "timeout": 5}
        ]
      }
    ]
  }
}
)";
        const std::string once = sim_hook_install(user, commands);
        cg::test::check(
            r,
            once.find("// user settings with comments") != std::string::npos
                && once.find("// my own hook, keep it") != std::string::npos
                && once.find("\"model\": \"sonnet\"") != std::string::npos,
            "install preserves user comments and settings");
        cg::test::check(
            r,
            once.find("echo user-hook") != std::string::npos,
            "install preserves the sibling user hook");
        cg::test::check(
            r,
            count_own_entries(parse_settings(once))
                == std::vector<int>{1, 1, 1, 1},
            "install lands exactly one entry per managed event");

        // 收敛：往已安装的文档上再安装是替换
        // 而非累积，且替换到达不动点。
        const std::string twice = sim_hook_install(once, commands);
        const std::string thrice = sim_hook_install(twice, commands);
        cg::test::check(
            r,
            twice != once || thrice == twice,
            "re-install replaces (fresh bytes) then converges");
        cg::test::check(
            r,
            thrice == twice,
            "the installed document is a fixed point");
        cg::test::check(
            r,
            count_own_entries(parse_settings(twice))
                == std::vector<int>{1, 1, 1, 1},
            "re-install does not accumulate own entries");

        // 零残留：卸载逐字节恢复用户文档
        // （保 span 编辑器只碰过自己加的东西）。
        const std::string back = sim_hook_uninstall(twice);
        cg::test::check(
            r,
            back == user,
            "uninstall restores the pre-install document exactly");
        cg::test::check(
            r,
            count_own_entries(parse_settings(back))
                == std::vector<int>{0, 0, 0, 0},
            "uninstall leaves no own entries behind");

        // 只装过自家条目的文档回归其安装前的
        // 裸形态（空根在真实安装器里完全绕过
        // 编辑器——fresh_document——所以编辑器自己的
        // 脚手架路径从带其它成员的根起步）。
        const std::string other = "{\n  \"model\": \"opus\"\n}\n";
        const std::string grown = sim_hook_install(other, commands);
        cg::test::check(
            r,
            count_own_entries(parse_settings(grown))
                == std::vector<int>{1, 1, 1, 1},
            "install scaffolds the hooks member into a bare doc");
        const std::string reverted = sim_hook_uninstall(grown);
        cg::test::check(
            r,
            reverted == other,
            "uninstall prunes the scaffolded hooks member back to "
            "the bare document");
    }

    // hook 摘要单元：批次结论装配器待在其字节
    // 预算内且 UTF-8 合法、日志按规范化路径一组
    // 存最新编辑 span，且 claim_snapshot 是纯读器
    // （lease + pid 存活 + 有序待处理队列）。
    void hook_digest_units(R& r)
    {
        namespace fsn = std::filesystem;

        // 30 行中文必须截到 <= 1200 字节、保住头行，
        // end with the "等 N 项" count, and remain valid UTF-8 (a JSON
        // stringify 非法字节则返回空——生产注入器 防的就是这种失败模式）。
        std::vector<hk::DigestRow> rows;
        for (int i = 0; i < 30; ++i)
        {
            hk::DigestRow row;
            row.path_display = std::format("src/模块{}.cpp", i);
            row.display = hk::basename_of(row.path_display);
            row.symbol =
                std::format("非常长的命名空间::项目::实现::类::函数{}", i);
            row.kind = "method";
            row.uid = i + 1;
            row.direct = 3 + i;
            row.transitive = 9 + i;
            row.fan_in = 2;
            row.risk = "HIGH";
            row.consumers = {"消费者.cpp:10 call", "其它.cpp:20 use"};
            rows.push_back(std::move(row));
        }
        const std::string text = hk::assemble_digest(rows, 0, 1200);
        cg::test::check(
            r,
            text.size() <= 1200,
            "assemble_digest respects the byte budget");
        cg::test::check(
            r,
            text.starts_with("[ccg] 批次完成：30 个文件"),
            "assemble_digest keeps the batch header");
        cg::test::check(
            r,
            text.find(" 等 ") != std::string::npos,
            "assemble_digest appends the omitted-item count");
        json::Object encoded;
        encoded.set("text", text);
        cg::test::check(
            r,
            !json::stringify(std::move(encoded)).empty(),
            "assemble_digest output is valid UTF-8");

        // Two distinct unindexed files render the same body (both "未被索引"):
        // 显示名必须在摘要去重前赋好，路径必须
        // 作摘要的键，否则一条结论被静默丢弃，
        // other is reported as "等 N 项".
        {
            std::vector<hk::DigestRow> unindexed;
            for (const char* path : {"src/ghost_one.cpp", "src/ghost_two.cpp"})
            {
                hk::DigestRow row;
                row.path_display = path;
                row.norm = std::string{"e:/proj/"} + path;
                row.tracked = false;
                unindexed.push_back(std::move(row));
            }
            auto [kept, collapsed] =
                hk::finalize_digest_rows(std::move(unindexed));
            cg::test::check(
                r,
                kept.size() == 2 && collapsed == 0,
                "distinct unindexed files are not collapsed by "
                "summary dedup");
            cg::test::check(
                r,
                kept.size() == 2 && !kept[0].display.empty()
                    && !kept[1].display.empty()
                    && kept[0].display != kept[1].display,
                "finalize assigns display names before rendering");
            cg::test::check(
                r,
                kept.size() == 2
                    && hk::render_digest_row(kept[0])
                           != hk::render_digest_row(kept[1]),
                "distinct files keep distinct rendered conclusions");
        }
        // 不同目录同基名：掐尾的相对
        // 路径让两条结论可分辨。
        {
            std::vector<hk::DigestRow> same_base;
            for (const char* path : {"src/one/util.cpp", "src/two/util.cpp"})
            {
                hk::DigestRow row;
                row.path_display = path;
                row.norm = std::string{"e:/proj/"} + path;
                row.tracked = false;
                same_base.push_back(std::move(row));
            }
            auto [kept, collapsed] =
                hk::finalize_digest_rows(std::move(same_base));
            cg::test::check(
                r,
                kept.size() == 2 && collapsed == 0
                    && kept[0].display != kept[1].display,
                "same-basename files keep distinguishable display "
                "names");
        }
        // 同路径同摘要仍折叠成一条结论。
        {
            std::vector<hk::DigestRow> repeated;
            for (int i = 0; i < 2; ++i)
            {
                hk::DigestRow row;
                row.path_display = "src/one.cpp";
                row.norm = "e:/proj/src/one.cpp";
                row.uid = i + 1;
                row.direct = 0;
                row.transitive = 0;
                row.risk = "LOW";
                repeated.push_back(std::move(row));
            }
            auto [kept, collapsed] =
                hk::finalize_digest_rows(std::move(repeated));
            cg::test::check(
                r,
                kept.size() == 1 && collapsed == 1,
                "a repeated summary within one file collapses");
        }

        // 日志：同一规范化路径的两次编辑折叠成一组，
        // 携带两个文件与最新 span；不同路径保持
        // 隔离分组；坏文件被丢弃，绝不注入。
        std::error_code      ec;
        cg::test::ScratchDir scratch{
            fsn::temp_directory_path(ec) / "ccg-digest-unit"};
        const fsn::path&  base = scratch.base;
        const std::string db_path = (base / "ccg.db").generic_string();
        hk::DigestEntry   first;
        first.path = "a.cpp";
        first.norm = "e:/proj/a.cpp";
        first.span.have_line = true;
        first.span.line = 3;
        first.span.lines = 1;
        first.ts = 1;
        hk::write_digest_entry(db_path, first);
        hk::DigestEntry newer = first;
        newer.span.line = 9;
        newer.ts = 2;
        newer.need_drain = true;
        hk::write_digest_entry(db_path, newer);
        hk::DigestEntry other;
        other.path = "b.cpp";
        other.norm = "e:/proj/b.cpp";
        other.span.have_line = true;
        other.span.line = 5;
        other.ts = 3;
        hk::write_digest_entry(db_path, other);
        scratch.put(base / "hook-digest" / "pending-bad.json", "{ truncated");
        const fsn::path                    dir = hk::digest_dir_for(db_path);
        const std::vector<hk::DigestGroup> groups = hk::load_digest_groups(dir);
        std::size_t                        a_files = 0;
        bool                               a_newest = false;
        bool                               a_drain = false;
        bool                               b_seen = false;
        for (const hk::DigestGroup& group : groups)
        {
            if (group.norm == "e:/proj/a.cpp")
            {
                a_files = group.files.size();
                a_newest = group.span.line == 9;
                a_drain = group.need_drain;
            }
            else if (group.norm == "e:/proj/b.cpp")
            {
                b_seen = true;
            }
        }
        cg::test::check(
            r,
            groups.size() == 2 && a_files == 2 && a_newest && a_drain && b_seen,
            "journal groups by normalized path, keeps every file, "
            "and uses the newest span");
        cg::test::check(
            r,
            !fsn::exists(dir / "pending-bad.json", ec),
            "an unreadable journal entry is removed (never injected)");

        // claim_snapshot：pending 按路径排序、过期 lease 判死、
        // 孤儿 pid 判死，活 claim 点名其 pid。
        {
            db::GraphStore store{db_path};
            cg::test::check(
                r,
                !store.claim_snapshot().live,
                "claim_snapshot: no claim is not live");
            store.enqueue_pending(std::vector<std::string>{"z.cpp", "a.cpp"});
            const cg::db::ClaimSnapshot queued = store.claim_snapshot();
            cg::test::check(
                r,
                queued.has_claim == false && queued.pending.size() == 2
                    && queued.pending[0] == "a.cpp"
                    && queued.pending[1] == "z.cpp",
                "claim_snapshot reads pending in path order");
            (void)store.claim_update(0, "stale-holder");
            cg::test::check(
                r,
                !store.claim_snapshot().live,
                "claim_snapshot: an expired lease is not live");
            (void)store.claim_update(60'000, "ccg-update:pid:999999999:t1");
            cg::test::check(
                r,
                !store.claim_snapshot().live,
                "claim_snapshot: an orphan pid is not live");
            const std::string holder = std::format(
                "ccg-update:pid:{}:t2",
                cg::util::platform::current_pid());
            (void)store.claim_update(60'000, holder);
            const cg::db::ClaimSnapshot live = store.claim_snapshot();
            cg::test::check(
                r,
                live.live && live.pid == cg::util::platform::current_pid(),
                "claim_snapshot: a live holder is live");
        }

        // 投递标记：捎带投递完成后落一个 flushed-<来源>- 空标记，
        // 计数只认整前缀（别的来源/畸形名不计），status 的
        // digest.preToolUseFlushes 与老化清理都锚定它。
        {
            hk::record_digest_delivery(db_path, "pretooluse");
            hk::record_digest_delivery(db_path, "pretooluse");
            hk::record_digest_delivery(db_path, "posttooluse");
            std::ofstream foreign{
                hk::digest_dir_for(db_path) / "flushed-garbage",
                std::ios::binary | std::ios::trunc};
            (void)foreign;
            cg::test::check(
                r,
                hk::count_digest_deliveries(
                    hk::digest_dir_for(db_path),
                    "pretooluse")
                    == 2,
                "delivery markers are counted per source prefix");
            cg::test::check(
                r,
                hk::count_digest_deliveries(
                    hk::digest_dir_for(db_path),
                    "posttooluse")
                    == 1,
                "delivery markers keep sources isolated");
            cg::test::check(
                r,
                hk::count_digest_deliveries(
                    fsn::temp_directory_path(ec) / "ccg-no-such-digest-dir",
                    "pretooluse")
                    == 0,
                "a missing journal directory counts zero deliveries");
            cg::test::check(
                r,
                hk::count_digest_deliveries(
                    hk::digest_dir_for(db_path),
                    "garbage")
                    == 0,
                "a bare prefix does not swallow longer source names");
        }
    }

    // ccg graph 解析：两种模式互斥，各自的旋钮
    // （--depth / --min-weight）恰属一种模式，共享的
    // 消歧器仍能收窄入口解析。
    void parse_graph(R& r)
    {
        const auto sym =
            make_inv({"graph", "--symbol", "run_main", "--depth", "3"});
        cg::test::check(
            r,
            sym.cmd == Invocation::Cmd::Graph && sym.graph_symbol == "run_main"
                && sym.depth == 3,
            "--symbol mode records symbol and depth");
        cg::test::check(
            r,
            make_inv({"graph", "--symbol", "x"}).depth == 2,
            "graph depth defaults to 2");
        const auto mods = make_inv({"graph", "--modules"});
        cg::test::check(
            r,
            mods.graph_modules && mods.min_weight == 1
                && mods.graph_format == "dot",
            "--modules mode defaults: weight floor 1, dot format");
        cg::test::check(
            r,
            make_inv({"graph", "--symbol", "x", "--format", "json"})
                    .graph_format
                == "json",
            "--format json parses");
        cg::test::check(
            r,
            make_inv({"graph", "--modules", "--min-weight=3"}).min_weight == 3,
            "--min-weight inline form parses");
        cg::test::check(
            r,
            make_inv({"graph", "--symbol", "x", "--output", "g.dot"})
                    .output_file
                == "g.dot",
            "--output records the payload path");
        cg::test::check(
            r,
            make_inv({"graph", "--symbol", "x", "--kind", "method"}).kind
                == db::SymbolKind::Method,
            "--kind narrows the entry resolution");
        cg::test::check(
            r,
            make_inv({"graph", "--modules", "--file", "src/"}).file_substring
                == std::optional<std::string>{"src/"},
            "--file scopes the module aggregation");
        const auto halp = make_inv({"graph", "-h"});
        cg::test::check(
            r,
            halp.cmd == Invocation::Cmd::Help
                && halp.positional == std::vector<std::string>{"graph"},
            "-h switches to per-command help");
    }

    // ccg graph 渲染器：dot 骨架 + 入口高亮 + 已知边，
    // 以及经 cg.util.json 往返的 JSON 载荷。
    void graph_render(R& r)
    {
        db::GraphStore&      store = fixture_store();
        const db::FindResult fr =
            db::find_symbols(store, "main", db::SymbolFilter{});
        const auto                   target = fr.matches.front();
        const cg::graph::CallClosure c =
            cg::graph::call_closure(store, target.id, 2);
        const std::string dot =
            cg::cli::render::graph_symbol_dot(target, 2, c, store);
        cg::test::check(
            r,
            dot.starts_with("digraph ccg_call_graph {"),
            "dot skeleton opens the digraph");
        cg::test::check(
            r,
            dot.find("\"main\" [fillcolor=\"#ffd166\"];") != std::string::npos,
            "the entry node is highlighted");
        cg::test::check(
            r,
            dot.find("\"main\" -> \"app::render_all\";") != std::string::npos,
            "a known closure edge renders");
        const std::string js =
            cg::cli::render::graph_symbol_json(target, 2, c, store);
        const json::Document doc = json::Document::parse(js);
        cg::test::check(
            r,
            doc.root().get("target").has_value()
                && doc.root().get("nodes").has_value()
                && doc.root().get("edges").has_value()
                && doc.root().get("truncated").has_value(),
            "symbol json top-level shape");
        const auto nodes = doc.root().get("nodes");
        cg::test::check(
            r,
            nodes.has_value() && nodes->is_array()
                && nodes->size() == c.nodes.size(),
            "symbol json node count matches the closure");

        const cg::graph::ModuleGraph mg = cg::graph::module_graph(store, "", 1);
        const std::string mdot = cg::cli::render::graph_modules_dot(mg);
        cg::test::check(
            r,
            mdot.starts_with("digraph ccg_module_graph {"),
            "module dot skeleton opens the digraph");
        cg::test::check(
            r,
            mdot.find("\"canvas\" -> \"circle\"") != std::string::npos,
            "a known aggregate edge renders");
        const std::string    mjs = cg::cli::render::graph_modules_json(mg, 3);
        const json::Document mdoc = json::Document::parse(mjs);
        const auto           total = mdoc.root().get("edgesTotal");
        const auto           floor = mdoc.root().get("minWeight");
        cg::test::check(
            r,
            total.has_value() && total->as_int() > 0 && floor.has_value()
                && floor->as_int() == std::int64_t{3},
            "module json reports totals and the weight floor");
    }

    // ------------------------------------------------- hook 更新预算 --------

    void hook_update_budget(R& r)
    {
        using namespace std::chrono;
        using cg::cli::detail::hook_update_timeout;
        using cg::cli::detail::run_child_json_timed;
        // 绝对截止：等待预算 = 总额 - 进程年龄 - 预留。
        // 基线是真实的进程创建时刻（Windows 含
        // 映像加载/DLL 初始化时间），不是模块静态初始化。
        // 注入变量全部经 EnvScope 保存/恢复（析构兜底，中途
        // return 不污染后续用例；原值存在则恢复原值）。
        const cg::test::EnvScope total_budget{"CCG_HOOK_TOTAL_BUDGET_MS"};
        const cg::test::EnvScope post_reserve{"CCG_HOOK_POST_RESERVE_MS"};
        const cg::test::EnvScope wait_override{"CCG_HOOK_UPDATE_TIMEOUT_MS"};
        const cg::test::EnvScope fail_inject{"CCG_HOOK_TEST_FAIL_CHILD_START"};
        const cg::test::EnvScope stall_inject{"CCG_HOOK_TEST_STALL_MS"};
        const auto               elapsed =
            cg::util::platform::process_elapsed_since_creation();
        total_budget.set(std::to_string(elapsed.count() + 1'300));
        post_reserve.set("1000");
        const auto budget = hook_update_timeout();
        cg::test::check(
            r,
            budget > milliseconds{0} && budget <= milliseconds{450},
            "the wait budget is the remaining absolute deadline "
            "minus the post-processing reserve");

        // 遗留覆盖只能收紧等待，绝不放宽。
        wait_override.set("1");
        cg::test::check(
            r,
            hook_update_timeout() <= milliseconds{1},
            "the explicit wait override never extends the deadline");
        wait_override.restore();

        // 失败注入：子进程报为未启动（此情形
        // hook 绝不得宣称"已索引"）。
        fail_inject.set("1");
        const cg::cli::detail::TimedChildJson failed = run_child_json_timed(
            {"update", "x.cpp", "--json"},
            milliseconds{1'000});
        cg::test::check(
            r,
            !failed.started && !failed.timed_out && !failed.doc,
            "the failure injection reports an unstarted child");
        cg::test::check(
            r,
            !cg::cli::detail::run_child_json({"update", "x.cpp"}).has_value(),
            "the failure injection also covers run_child_json");
        fail_inject.restore();

        // 停顿注入：本会跑 60 秒的子进程最多
        // 只等算出的预算，然后分离。
        stall_inject.set("60000");
        const auto stall_budget = hook_update_timeout();
        cg::test::check(
            r,
            stall_budget > milliseconds{0},
            "a positive wait budget remains for the stall test");
        const auto                            started = steady_clock::now();
        const cg::cli::detail::TimedChildJson stalled =
            run_child_json_timed({"update", "x.cpp", "--json"}, stall_budget);
        const auto waited =
            duration_cast<milliseconds>(steady_clock::now() - started);
        cg::test::check(
            r,
            stalled.started && stalled.timed_out,
            "an over-budget child is detached, not killed");
        cg::test::check(
            r,
            waited <= stall_budget + milliseconds{250},
            "the hook waits no longer than its absolute budget");
    }

    // started=false 分支必须如实说明且仍把文件
    // 入队等下一次排空。以进程内注入子进程启动
    // 失败的方式演练（把测试二进制当 ccg 拉起会递归）。
    void hook_start_failure_handler(R& r)
    {
        namespace fsn = std::filesystem;
        cg::test::ScratchDir scratch{
            fsn::absolute(fsn::path{"ccg-cli-hook-start-scratch"})};
        const fsn::path& base = scratch.base;
        std::error_code  ec;
        fsn::create_directories(base / "rt", ec);
        const std::string db_path = (base / "rt" / "ccg.db").generic_string();
        const fsn::path   edited = (base / "edited.cpp").lexically_normal();
        {
            db::GraphStore store{db_path};
            store.set_busy_timeout_ms(5'000);
            // 文件级门要求被编辑文件已在库的语料面（files 表）：先登记
            // 再喂事件，本用例才走得到 started=false 的兜底分支。
            store.upsert_file(
                cg::util::norm_path(
                    fsn::absolute(edited).lexically_normal().generic_string()));
        }
        scratch.put(edited, "int edited_fn() { return 1; }\n");
        const std::string payload = std::format(
            R"({{"hook_event_name":"PostToolUse",)"
            R"("tool_name":"Write","tool_input":{{"file_path":)"
            R"("{}"}}}})",
            edited.generic_string());
        const fsn::path payload_file = base / "payload.json";
        const fsn::path capture_file = base / "stdout.txt";
        scratch.put(payload_file, payload);

        // 重定向 stdin（is_piped + 解析器输入）与 fd 1（std::print）。
        FdRedirect               redir_in{0, payload_file, /*readable=*/true};
        FdRedirect               redir_out{1, capture_file, /*readable=*/false};
        std::istringstream       input{payload};
        auto*                    old_cin = std::cin.rdbuf(input.rdbuf());
        const cg::test::EnvScope fail_inject{"CCG_HOOK_TEST_FAIL_CHILD_START"};
        fail_inject.set("1");

        const std::vector<std::string> hook_args{
            "hook",
            "posttooluse",
            "-o",
            base.generic_string(),
            "--config",
            "rt"};
        const int rc = run_main(hook_args);

        std::cin.rdbuf(old_cin);
        redir_out.restore();
        redir_in.restore();

        std::string captured;
        {
            std::ifstream in(capture_file, std::ios::binary);
            captured.assign(
                std::istreambuf_iterator<char>{in},
                std::istreambuf_iterator<char>{});
        }
        cg::test::check(
            r,
            rc == 0,
            "the hook exits 0 even when the update "
            "child could not be started");
        cg::test::check(
            r,
            captured.find("hookSpecificOutput") != std::string::npos,
            "the started=false branch still injects context");
        cg::test::check(
            r,
            captured.find("未能启动索引更新") != std::string::npos,
            "the injection says the update could not be started");
        cg::test::check(
            r,
            captured.find("已入队待下次刷新") != std::string::npos,
            "the injection reports the fallback enqueue");

        {
            db::GraphStore store{db_path};
            store.set_busy_timeout_ms(5'000);
            cg::test::check(
                r,
                store.claim_update(60'000, "T1"),
                "claim for the fallback-enqueue check");
            const cg::db::PendingPeek peek = store.peek_pending(60'000, "T1");
            const std::string         expected = cg::util::norm_path(
                fsn::absolute(edited).lexically_normal().generic_string());
            cg::test::check(
                r,
                !peek.claim_lost && peek.files.size() == 1
                    && peek.files[0] == expected,
                "the file was enqueued for the next drain");
            (void)store.release_update("T1");
        }
    }

    // 进程内直连：查询/影响面/changes 的直连等价在 :memory: 迷你库上
    // 演练——字段形状、唯一解析与歧义文案、uid 校验、文件级回退。
    // indexedAt 新鲜度解析：合法时间戳出小时数；MSVC 的
    // from_stream 会把 2 月 30 日这类日历上不存在的日子静默
    // 规范化成下月日期而不是置失败，靠 format 往返比对拦下，
    // 缺字段与尾随字符也一并拒绝。
    /// 以 '|' 连接分支列表。
    [[nodiscard]]
    auto join_pipes(const std::vector<std::string>& parts) -> std::string
    {
        std::string out;
        for (const std::string& p : parts)
        {
            if (!out.empty())
            {
                out += "|";
            }
            out += p;
        }
        return out;
    }

    /// ISO8601 时间戳解析是否拒绝该输入。
    [[nodiscard]]
    auto iso8601_age_rejects(std::string_view s) -> bool
    {
        return !cg::cli::detail::iso8601_age_hours(s).has_value();
    }

    void hook_iso8601_age(R& r)
    {
        namespace hk = cg::cli::detail;

        // 合法：纪元零点距今必然远超 40 万小时。
        const auto zero = hk::iso8601_age_hours("1970-01-01T00:00:00Z");
        cg::test::check(
            r,
            zero.has_value() && *zero > 400'000,
            "epoch zero parses with a huge age");
        // 合法：写入口径自产时间戳，解析到的年龄应当为 0。
        const auto now_age =
            hk::iso8601_age_hours(cg::util::cstd::iso8601_utc_timestamp());
        cg::test::check(
            r,
            now_age.has_value() && *now_age == 0,
            "a just-minted stamp is 0 hours old");
        // 合法闰日。
        cg::test::check(
            r,
            hk::iso8601_age_hours("2024-02-29T00:00:00Z").has_value(),
            "leap day is a real date");
        // 日历上不存在的日子（from_stream 规范化后必然对不上原串）。
        cg::test::check(
            r,
            iso8601_age_rejects("2026-02-30T10:30:00Z"),
            "Feb 30 is rejected");
        cg::test::check(
            r,
            iso8601_age_rejects("2026-02-29T10:30:00Z"),
            "Feb 29 of a non-leap year is rejected");
        cg::test::check(
            r,
            iso8601_age_rejects("2100-02-29T00:00:00Z"),
            "Feb 29 of a century non-leap year is rejected");
        cg::test::check(
            r,
            iso8601_age_rejects("2026-04-31T00:00:00Z"),
            "Apr 31 is rejected");
        cg::test::check(
            r,
            iso8601_age_rejects("2026-11-31T00:00:00Z"),
            "Nov 31 is rejected");
        // 字段值域与缺字段。
        cg::test::check(
            r,
            iso8601_age_rejects("2026-13-01T00:00:00Z"),
            "month 13 is rejected");
        cg::test::check(
            r,
            iso8601_age_rejects("2026-11-14T24:00:00Z"),
            "hour 24 is rejected");
        cg::test::check(
            r,
            iso8601_age_rejects("2026-11-14T10:60:00Z"),
            "minute 60 is rejected");
        cg::test::check(
            r,
            iso8601_age_rejects("2026-11-14T10:30Z"),
            "a stamp missing seconds is rejected");
        cg::test::check(
            r,
            iso8601_age_rejects("2026-11-14T10:30:00"),
            "a stamp missing the Z suffix is rejected");
        // 尾随字符、纪元前与垃圾输入。
        cg::test::check(
            r,
            iso8601_age_rejects("2026-11-14T10:30:00Zx"),
            "trailing bytes after Z are rejected");
        cg::test::check(
            r,
            iso8601_age_rejects("1969-12-31T23:59:59Z"),
            "pre-1970 stamps are rejected");
        cg::test::check(
            r,
            iso8601_age_rejects("not a timestamp"),
            "garbage is rejected");
    }

    void hook_inprocess_queries(R& r)
    {
        using cg::cli::detail::hook_changes;
        using cg::cli::detail::HookQueryDb;
        using cg::cli::detail::query_symbols;
        using cg::cli::detail::symbol_impact;
        HookQueryDb db;
        db.store = std::make_unique<db::GraphStore>(":memory:");
        db.alive = true;
        db::GraphStore&  seed = *db.store;
        const db::FileId fa = seed.upsert_file("e:/hook/a.cpp");
        const db::FileId fb = seed.upsert_file("e:/hook/b.cpp");
        const db::SymId  caller = seed.upsert_symbol(
            {.unique_key = "function||caller|e:/hook/a.cpp",
             .kind = db::SymbolKind::Function,
             .name = "caller",
             .ns = "",
             .sig_hash = std::nullopt,
             .file_id = fa,
             .line = 1,
             .col = 5});
        const db::SymId callee = seed.upsert_symbol(
            {.unique_key = "function||callee|e:/hook/b.cpp",
             .kind = db::SymbolKind::Function,
             .name = "callee",
             .ns = "shape",
             .sig_hash = std::nullopt,
             .file_id = fb,
             .line = 2,
             .col = 6});
        seed.upsert_edge(
            {.src = caller,
             .dst = callee,
             .type = db::EdgeType::Calls,
             .site_file = fa,
             .site_line = 9});
        // 同文件同名重载：唯一解析必须拒绝（旧“目标无法解析”文案）。
        const db::SymId ov1 = seed.upsert_symbol(
            {.unique_key = "function||ov|e:/hook/a.cpp|1",
             .kind = db::SymbolKind::Function,
             .name = "ov",
             .ns = "",
             .sig_hash = 1,
             .file_id = fa,
             .line = 20,
             .col = 5});
        const db::SymId ov2 = seed.upsert_symbol(
            {.unique_key = "function||ov|e:/hook/a.cpp|2",
             .kind = db::SymbolKind::Function,
             .name = "ov",
             .ns = "",
             .sig_hash = 2,
             .file_id = fa,
             .line = 24,
             .col = 5});
        (void)ov1;
        (void)ov2;

        const cg::cli::detail::SymbolQuery hits =
            query_symbols(db, "callee", "b.cpp");
        cg::test::check(
            r,
            !hits.yielded && hits.symbols.size() == 1
                && hits.symbols[0].uid == callee
                && hits.symbols[0].kind == "function"
                && hits.symbols[0].name == "callee"
                && hits.symbols[0].qualified_name == "shape::callee"
                && hits.symbols[0].file == "e:/hook/b.cpp"
                && hits.symbols[0].line == 2,
            "the in-process query mirrors `ccg query --file`");

        const cg::cli::detail::SymbolQuery caller_hit =
            query_symbols(db, "caller", "a.cpp");
        cg::test::check(
            r,
            caller_hit.symbols.size() == 1
                && caller_hit.symbols[0].uid == caller,
            "the caller resolves for impact");
        const cg::cli::detail::ImpactInfo impact =
            symbol_impact(db, caller_hit.symbols.front());
        cg::test::check(
            r,
            impact.attempted && impact.ok,
            "the in-process impact succeeds");
        cg::test::check(
            r,
            impact.direct == 0 && impact.impacted == 0,
            "an upstream walk from the caller finds nothing");
        const cg::cli::detail::ImpactInfo downstream =
            symbol_impact(db, hits.symbols.front());
        cg::test::check(
            r,
            downstream.ok && downstream.direct == 1 && downstream.impacted == 1
                && downstream.direct_symbols.size() == 1
                && downstream.direct_symbols[0].uid == caller,
            "the in-process impact reports the direct consumer");

        const cg::cli::detail::SymbolQuery ov_hit =
            query_symbols(db, "ov", "a.cpp");
        cg::test::check(
            r,
            ov_hit.symbols.size() == 2,
            "both overloads match by name");
        const cg::cli::detail::ImpactInfo ambiguous =
            symbol_impact(db, ov_hit.symbols.front());
        cg::test::check(
            r,
            ambiguous.attempted && !ambiguous.ok
                && ambiguous.reason.find("目标无法解析") != std::string::npos,
            "same-name overloads keep the ambiguous-target wording");

        const auto known = hook_changes(db, "e:/hook/b.cpp");
        cg::test::check(
            r,
            known.has_value() && known->any_known
                && known->canonical == "e:/hook/b.cpp" && known->direct == 1
                && known->consumers.size() == 1
                && known->consumers[0].qualified_name == "caller",
            "the in-process changes report file-level consumers");
        const auto unknown = hook_changes(db, "e:/hook/nowhere.cpp");
        cg::test::check(
            r,
            unknown.has_value() && !unknown->any_known
                && unknown->canonical.empty() && unknown->consumers.empty(),
            "an untracked file reports unknown without consumers");
    }

    // 编译数据库是唯一语料来源：
    // `ccg index` 对没有它的树以可操作错误拒绝，
    // 发现顺序保持 根 -> 根/build -> 一层子目录，
    // `ccg update` 拒绝没记录它的库，扩展名闸门
    // 计数器跳过嵌套构建树与点目录。
    void compile_db_required(R& r)
    {
        namespace fsn = std::filesystem;
        cg::test::ScratchDir scratch{
            fsn::absolute(fsn::temp_directory_path() / "ccg-cli-nocdb")};
        const fsn::path& base = scratch.base;
        std::error_code  ec;
        const auto       norm = [](const fsn::path& p)
        { return cg::util::norm_path(p.lexically_normal().generic_string()); };

        // 对带源码却无 cdb 的树，`ccg index` 经 UsageError
        // 退出并携带三条可操作修法。
        const fsn::path tree = base / "tree";
        scratch.put(tree / "main.cpp", "int main() { return 0; }\n");
        const fsn::path outdir = base / "out";
        bool            usage = false;
        std::string     message;
        try
        {
            (void)cg::cli::detail::run_index(make_inv(
                {"index",
                 tree.generic_string(),
                 "-o",
                 outdir.generic_string(),
                 "--config",
                 "nocdb"}));
        }
        catch (const cg::cli::UsageError& e)
        {
            usage = true;
            message = e.what();
        }
        catch (const std::exception& e)
        {
            message = e.what();
        }
        cg::test::check(
            r,
            usage,
            "index without a compile db is a usage error");
        cg::test::check(
            r,
            message.find("no compile_commands.json found under")
                    != std::string::npos
                && message.find("CMAKE_EXPORT_COMPILE_COMMANDS=ON")
                       != std::string::npos
                && message.find("ccg msbuild") != std::string::npos
                && message.find("--compile-commands <file>")
                       != std::string::npos,
            "the refusal carries all three fix hints");
        cg::test::check(
            r,
            !fsn::exists(outdir / "nocdb" / "ccg.db", ec),
            "the refused run creates no database");

        // 发现顺序：根本身压过 root/build 与一层
        // 子目录；root/build 压过任意子目录。
        cg::test::check(
            r,
            find_compile_db_norm(tree).empty(),
            "no compile db -> discovery finds nothing");
        scratch.put(tree / "build" / "compile_commands.json", "[]");
        cg::test::check(
            r,
            find_compile_db_norm(tree)
                == norm_lex(tree / "build" / "compile_commands.json"),
            "discovery falls back to <root>/build");
        scratch.put(tree / "generated" / "compile_commands.json", "[]");
        cg::test::check(
            r,
            find_compile_db_norm(tree)
                == norm_lex(tree / "build" / "compile_commands.json"),
            "<root>/build outranks a one-level subdirectory");
        scratch.put(tree / "compile_commands.json", "[]");
        cg::test::check(
            r,
            find_compile_db_norm(tree)
                == norm_lex(tree / "compile_commands.json"),
            "<root> itself outranks <root>/build");
        const fsn::path shallow = base / "shallow";
        scratch.put(shallow / "generated" / "compile_commands.json", "[]");
        cg::test::check(
            r,
            find_compile_db_norm(shallow)
                == norm_lex(shallow / "generated" / "compile_commands.json"),
            "discovery scans one level of subdirectories");

        // 对未记录编译数据库的库（库文件在而 compile_db_path meta
        // 缺席）跑 `ccg update` 以可行动错误拒绝。
        {
            // GraphStore 不会创建父目录：库文件落在子目录时先建目录，
            // 否则 SQLite 打不开路径直接抛异常。
            fsn::create_directories(base / "noupdate", ec);
            {
                db::GraphStore fresh{
                    (base / "noupdate" / "ccg.db").generic_string()};
            }
            bool        refused = false;
            std::string refusal;
            try
            {
                // update 的库路径由 dispatch 的布局解析填充；直调
                // run_update 会因 db_path 空在更早的检查上拒绝，拿不到
                // 这段要断言的 compile-db 提示。
                (void)cg::cli::dispatch(make_inv(
                    {"update",
                     "x.cpp",
                     "-o",
                     base.generic_string(),
                     "--config",
                     "noupdate"}));
            }
            catch (const std::runtime_error& e)
            {
                refused = true;
                refusal = e.what();
            }
            catch (const std::exception& e)
            {
                refusal = e.what();
            }
            cg::test::check(
                r,
                refused,
                "update on a db with no recorded compile db is an error");
            cg::test::check(
                r,
                refusal.find("no compile_commands.json found under")
                    != std::string::npos,
                "the update refusal carries the compile-db hints");
        }
    }

    const cg::test::Registrar reg_nocdb{
        "cli.index.compile_db_required",
        &compile_db_required};

    /**
     * @brief cl 方言纯头单元语料：路由期 warning 披露 + 失败形态不变。
     *
     * 语料整库零命名模块、cl 风格命令行、消费者源文本书写
     * `import "x.hpp";`——cl 模式路由无可路由单元提前返回，头单元降
     * 级通道（活在自建链里）不进场，消费者对逐字重放的 argv 解析必
     * 败。不修复该形态，索引开始前以 warning 讲清场景、后
     * 果与涉及的 TU，并披露 db 方言（编译数据库路线）已支持同形态的
     * 分叉。两层断言：warning 在路由期冒出且只点名携带头单元 import
     * 的 TU；索引行为与既有现状一致（仅消费者 TU 一条经典
     * parse-failed，非导入的经典 cl 条目照常成功，零新失败类）。
     */
    void cl_route_pure_header_unit_corpus_warning(R& r)
    {
        namespace fsn = std::filesystem;
        cg::test::ScratchDir scratch{
            fsn::absolute(fsn::temp_directory_path() / "ccg-cli-cl-hu")};
        const fsn::path& base = scratch.base;

        // 语料：纯头文件 + 两个消费者（import 它的 / 纯经典的），零
        // 命名模块单元——警告必须只点名前者。
        const fsn::path tree = base / "tree";
        scratch.put(
            tree / "math_api.hpp",
            "inline int math_answer() { return 42; }\n");
        scratch.put(
            tree / "consumer.cpp",
            "import \"math_api.hpp\";\n"
            "int use_math() { return math_answer(); }\n");
        scratch.put(
            tree / "plain.cpp",
            "#include \"math_api.hpp\"\n"
            "int use_plain() { return math_answer(); }\n");

        // cl 方言编译数据库（MSVC 风格命令行：/c、/EHsc、/std:、/I）。
        std::error_code   abs_ec;
        const fsn::path   abs = fsn::absolute(tree, abs_ec).lexically_normal();
        const std::string t = cg::util::norm_path(abs.generic_string());
        scratch.put(
            tree / "compile_commands.json",
            std::format(
                "[\n"
                " {{\"directory\": \"{0}\", \"arguments\": [\"clang-cl\", "
                "\"--driver-mode=cl\", \"/c\", \"/EHsc\", \"/std:c++20\", "
                "\"/I\", \"{0}\", \"{0}/consumer.cpp\"], \"file\": "
                "\"{0}/consumer.cpp\"}},\n"
                " {{\"directory\": \"{0}\", \"arguments\": [\"clang-cl\", "
                "\"--driver-mode=cl\", \"/c\", \"/EHsc\", \"/std:c++20\", "
                "\"/I\", \"{0}\", \"{0}/plain.cpp\"], \"file\": "
                "\"{0}/plain.cpp\"}}\n"
                "]\n",
                t));

        // 路由经 CLI 主干（真实 `ccg index` 的同一条路）；stderr 由 fd
        // 捕获——warning 必须在索引开始前（路由期）冒出来。
        const std::string   cdb = (abs / "compile_commands.json").string();
        const std::string   root = t;
        const std::string   db_abs = (base / "ccg.db").generic_string();
        cg::cli::Invocation inv = make_inv(
            {"index",
             root,
             "--compile-commands",
             cdb,
             "--config",
             "cl-headerunit"});
        inv.db_path = db_abs;
        std::vector<cg::clang::CompileCommand> commands =
            cg::clang::load(cdb, std::vector<std::string>{root});
        const fsn::path                   cap = base / "stderr.txt";
        cg::cli::detail::ResolvedCommands resolved;
        {
            FdRedirect redir{2, cap, /*readable=*/false};
            resolved = cg::cli::detail::resolve_loaded_commands(
                inv,
                {root},
                std::move(commands),
                {},
                cdb);
        }
        std::string captured;
        {
            std::ifstream in{cap, std::ios::binary};
            captured.assign(
                std::istreambuf_iterator<char>{in},
                std::istreambuf_iterator<char>{});
        }
        cg::test::check(
            r,
            captured.find("ccg: warning:") != std::string::npos,
            "[cl-headerunit] the cl route warns for a pure header-unit "
            "corpus");
        cg::test::check(
            r,
            captured.find("header-unit imports") != std::string::npos
                && captured.find("will fail to parse") != std::string::npos,
            "[cl-headerunit] the warning states scenario and consequence");
        cg::test::check(
            r,
            captured.find("math_api.hpp") != std::string::npos,
            "[cl-headerunit] the warning shows the actual detected "
            "header spelling, not a fixed placeholder");
        cg::test::check(
            r,
            captured.find("compile-database route") != std::string::npos,
            "[cl-headerunit] the warning discloses the dialect divergence");
        cg::test::check(
            r,
            captured.find(t + "/consumer.cpp") != std::string::npos,
            "[cl-headerunit] the warning names the affected TU");
        cg::test::check(
            r,
            captured.find("plain.cpp") == std::string::npos,
            "[cl-headerunit] the warning leaves non-importing cl entries "
            "unnamed");

        // 行为不变（warning 只加可观测性）：仅消费者 TU 一条经典
        // parse-failed，纯经典 cl 条目照常成功，失败类聚合零新类别。
        db::GraphStore          store{db_abs};
        cg::graph::IndexOptions opt;
        opt.roots = {root};
        opt.compile_db_path = cdb;
        opt.db_path = db_abs;
        opt.progress = false;
        cg::graph::Indexer            indexer{store, std::move(opt)};
        const cg::graph::IndexSummary summary = indexer.run(resolved.commands);
        cg::test::check(
            r,
            summary.tus == 2 && summary.failed == 1,
            std::format(
                "[cl-headerunit] exactly the importing TU fails (tus {}, "
                "failed {})",
                summary.tus,
                summary.failed));
        cg::test::check(
            r,
            summary.failure_classes.size() == 1
                && summary.failure_classes[0].first == "parse-failed"
                && summary.failure_classes[0].second == 1,
            std::format(
                "[cl-headerunit] the failure keeps its classic parse-failed "
                "class (aggregate size {}, head '{}')",
                summary.failure_classes.size(),
                summary.failure_classes.empty()
                    ? std::string{"<none>"}
                    : summary.failure_classes.front().first));
        // 失败文本与既有现状同形：引擎对未知头单元的原始报错（与
        // 编译数据库路线修复前的失败形态基线同一诊断家族）。
        const std::vector<db::FailureRow> failures = db::all_failures(store);
        cg::test::check(
            r,
            failures.size() == 1
                && failures.front().failure_text.find("header unit")
                       != std::string::npos,
            std::format(
                "[cl-headerunit] one failure row with the engine's raw "
                "header-unit diagnostic (rows {}, text head '{}')",
                failures.size(),
                failures.empty()
                    ? std::string{"<none>"}
                    : failures.front().failure_text.substr(0, 80)));
    }

    const cg::test::Registrar reg_cl_headerunit{
        "cli.index.cl_route.pure_header_unit_corpus_warning",
        &cl_route_pure_header_unit_corpus_warning};
    const cg::test::Registrar reg_hook_budget{
        "cli.hook.update_budget",
        &hook_update_budget};
    const cg::test::Registrar reg_hook_start{
        "cli.hook.start_failure",
        &hook_start_failure_handler};
    const cg::test::Registrar reg_hook_iso_age{
        "cli.hook.iso8601_age",
        &hook_iso8601_age};
    const cg::test::Registrar reg_hook_inproc{
        "cli.hook.inprocess_queries",
        &hook_inprocess_queries};
    const cg::test::Registrar reg_parse_ok{"cli.parse.ok", &parse_ok_shapes};
    const cg::test::Registrar reg_parse_bad{
        "cli.parse.usage_errors",
        &parse_usage_errors};
    const cg::test::Registrar reg_parse_graph{"cli.parse.graph", &parse_graph};
    const cg::test::Registrar reg_parse_ms{"cli.parse.msbuild", &parse_msbuild};
    const cg::test::Registrar reg_tiers{"cli.resolve.tiers", &resolution_tiers};
    const cg::test::Registrar reg_fuzzy{
        "cli.resolve.suggestions",
        &tier4_suggestions};
    const cg::test::Registrar reg_glob{
        "cli.glob.exclude_filter",
        &glob_exclude_filter};
    const cg::test::Registrar reg_json{"cli.render.json", &json_round_trips};

    // 成员面（含继承）+ --implements 派生树 + 零调用者置信三要素。
    void members_and_implements(R& r)
    {
        db::GraphStore& store = fixture_store();

        // members_with_origin：Circle = own（scale/radius/…）+ 继承自 Shape。
        const auto found =
            db::find_symbols(store, "shape::Circle", db::SymbolFilter{});
        std::optional<db::SymbolRow> circle;
        for (const db::SymbolRow& m : found.matches)
        {
            if (m.kind == db::SymbolKind::Record)
            {
                circle = m;
                break;
            }
        }
        cg::test::check(r, circle.has_value(), "Circle record resolves");
        if (!circle.has_value())
        {
            return;
        }
        const std::vector<db::MemberRow> rows =
            db::members_with_origin(store, circle->id);
        const auto own = std::count_if(
            rows.begin(),
            rows.end(),
            [](const db::MemberRow& m) { return m.origin == "own"; });
        const auto inherited = std::count_if(
            rows.begin(),
            rows.end(),
            [](const db::MemberRow& m) { return m.origin == "shape::Shape"; });
        cg::test::check(r, own > 0, "members: own group non-empty");
        // 基类成员入组：Shape 的 area/describe/~Shape 至少可见一个。
        bool saw_base_area = false;
        for (const db::MemberRow& m : rows)
        {
            if (m.origin == "shape::Shape" && m.symbol.name == "area")
            {
                saw_base_area = true;
            }
        }
        cg::test::check(
            r,
            inherited > 0 && saw_base_area,
            "members: inherited group carries base methods");

        // members_json：每成员带 origin 字段。
        const json::Document mdoc =
            json::Document::parse(cg::cli::render::members_json(*circle, rows));
        const auto members = mdoc.root().get("members");
        cg::test::check(
            r,
            members.has_value() && members->is_array()
                && members->size() == rows.size(),
            "members json count matches");
        if (members.has_value() && members->size() != 0)
        {
            cg::test::check(
                r,
                members->at(0).has_value()
                    && members->at(0)->get("origin").has_value(),
                "members json entries carry origin");
        }

        // implements：Shape 的直接派生含 Circle（depth=1）。
        const auto shape =
            db::find_symbols(store, "shape::Shape", db::SymbolFilter{});
        std::optional<db::SymbolRow> base;
        for (const db::SymbolRow& m : shape.matches)
        {
            if (m.kind == db::SymbolKind::Record)
            {
                base = m;
                break;
            }
        }
        cg::test::check(r, base.has_value(), "Shape record resolves");
        if (base.has_value())
        {
            std::vector<cg::cli::render::ImplementRow> impl_rows;
            for (const db::EdgeRow& e : db::deriving(store, base->id))
            {
                if (const auto row = db::get_symbol(store, e.src);
                    row && row->kind == db::SymbolKind::Record)
                {
                    impl_rows.push_back({*row, 1});
                }
            }
            bool saw_circle = false;
            for (const auto& ir : impl_rows)
            {
                saw_circle = saw_circle || ir.symbol.name == "Circle";
            }
            cg::test::check(
                r,
                saw_circle,
                "implements: Shape deriving includes Circle");
            const json::Document idoc = json::Document::parse(
                cg::cli::render::implements_json(*base, impl_rows));
            cg::test::check(
                r,
                idoc.root().get("interface").has_value()
                    && idoc.root().get("deriving").has_value()
                    && idoc.root().get("deriving")->is_array(),
                "implements json shape");
        }

        // 零调用者置信（json 三要素）：unused_helper 图上零入边。
        const auto uh =
            db::find_symbols(store, "unused_helper", db::SymbolFilter{});
        if (!uh.matches.empty())
        {
            const cg::graph::NeighborsResult no = cg::graph::callers(
                store,
                uh.matches.front().id,
                1,
                true,
                cg::graph::DispatchMode::Precise);
            cg::test::check(
                r,
                no.entries.empty(),
                "unused_helper has no graph callers");
            const cg::cli::render::CallersBlindSpots blind{
                .pointer_calls_invisible = true,
                .sibling_dispatch_possible = false,
                .member_address_taken = false};
            const json::Document ndoc = json::Document::parse(
                cg::cli::render::neighbors_json(
                    true,
                    uh.matches.front(),
                    1,
                    no,
                    store,
                    std::nullopt,
                    {},
                    &blind));
            const auto reason = ndoc.root().get("no_callers_reason");
            cg::test::check(
                r,
                reason.has_value() && reason->is_object(),
                "no_callers_reason object present");
            if (reason.has_value())
            {
                cg::test::check(
                    r,
                    reason->get("pointer_calls_invisible").has_value()
                        && reason->get("sibling_dispatch_possible").has_value()
                        && reason->get("member_address_taken").has_value(),
                    "no_callers_reason carries all three blind spots");
            }
        }
    }
    const cg::test::Registrar reg_members_impl{
        "cli.render.members_implements",
        &members_and_implements};
    const cg::test::Registrar reg_text{"cli.render.text", &text_renders};
    const cg::test::Registrar reg_graph_render{
        "cli.render.graph",
        &graph_render};
    const cg::test::Registrar reg_exits{
        "cli.run_main.exit_codes",
        &run_main_exit_codes};
    const cg::test::Registrar reg_config{"cli.config_command", &config_command};
    const cg::test::Registrar reg_config_knobs{
        "cli.config_update_knobs",
        &config_update_knobs};
    const cg::test::Registrar reg_config_jobs{
        "cli.config_index_jobs_and_flags",
        &config_index_jobs_and_flags};
    const cg::test::Registrar reg_resolve_jobs{
        "cli.resolve_jobs",
        &resolve_jobs_priority};
    const cg::test::Registrar reg_flags_schema{
        "cli.injected_flags_and_schema_rebuild",
        &injected_flags_and_schema_rebuild};
    const cg::test::Registrar reg_cc_parse{
        "cli.parse.changes_check",
        &parse_changes_check};
    const cg::test::Registrar reg_cc_resolve{
        "cli.changes.resolve",
        &changes_resolution_and_render};
    const cg::test::Registrar reg_up_parse{"cli.parse.update", &parse_update};
    const cg::test::Registrar reg_up_coord{
        "cli.update.coordination",
        &update_coordination};
    const cg::test::Registrar reg_hook{
        "cli.hook.settings_editor",
        &hook_settings_editor};
    const cg::test::Registrar reg_digest{"cli.hook.digest", &hook_digest_units};

    // PreToolUse Grep 注入单元：V7.1 渲染器对着
    // 注入格式规格的字节锁定措辞（identity/def/
    // same-name/callers/dispatch 行、零调用措辞 v2 双分支、
    // 999 出现上限、形参裁剪）钉死，预算阶梯以
    // 规格的硬截断标记收尾，Grep 模式过滤器只接受
    // 整标识符（链式）正则。
    void pretooluse_injection_units(R& r)
    {
        using hk::InjectionFacts;
        using hk::InjectionSameName;

        // 零调用者、无同名竞争：仅声明后缀。
        {
            InjectionFacts f;
            f.name = "SerializeCaptureResult";
            f.kind = "function";
            f.qualified = "Acme::Core::Debug::SerializeCaptureResult";
            f.parent = "Acme::Core::Debug";
            f.params = {"AcmeResult", "const DebugImageWriteResult &"};
            f.rel_file = "acme/Core/Debug/src/TraceDecorator.cpp";
            f.line = 64;
            f.col = 10;
            const std::string text = hk::render_injection_v71(f);
            const std::string want =
                "[ccg] Acme::Core::Debug::SerializeCaptureResult(AcmeResult,"
                "const DebugImageWriteRe…) — function of Acme::Core::Debug\n"
                "[ccg] def: acme/Core/Debug/src/TraceDecorator.cpp:64:10\n"
                "[ccg] callers(top3): 0 edges in call graph for this exact "
                "symbol; answer with no callers for it. Grep hits are only this "
                "symbol's own definition/declaration lines — a header "
                "declaration is not a call site\n"
                "[ccg] dispatch: none (direct calls only)";
            cg::test::check(
                r,
                text == want,
                "V7.1 render: zero-caller declaration-only branch");
        }
        // 零调用者带一个可调用同名竞争：调用形态后缀。
        {
            InjectionFacts f;
            f.name = "Check";
            f.kind = "method";
            f.qualified =
                "Acme::Core::PluginGateway::ScriptHost::PythonResult::Check";
            f.parent = "Acme::Core::PluginGateway::ScriptHost::PythonResult";
            f.params = {};
            f.rel_file = "acme/Core/PluginGateway/src/ScriptHost.cpp";
            f.line = 328;
            f.col = 10;
            f.same_name_callable = 1;
            f.same_name_others.push_back(
                InjectionSameName{
                    "Napi::Maybe::Check",
                    "build/mingw-debug/_deps/nodeaddonapi-src/napi-inl.h",
                    576});
            const std::string text = hk::render_injection_v71(f);
            const std::string want =
                "[ccg] Acme::Core::PluginGateway::ScriptHost::PythonResult"
                "::Check(void) — method of Acme::Core::PluginGateway::"
                "ScriptHost::PythonResult\n"
                "[ccg] def: acme/Core/PluginGateway/src/ScriptHost.cpp:"
                "328:10\n"
                "[ccg] same-name(1): Napi::Maybe::Check @build/mingw-debug/"
                "_deps/nodeaddonapi-src/napi-inl.h:576 — hits at those coords "
                "belong to other symbols, not this one\n"
                "[ccg] callers(top3): 0 edges in call graph for this exact "
                "symbol; answer with no callers for it. Grep call-shape rows "
                "like x->Check(...) or y.Check(...) invoke the 1 other "
                "same-name symbols' impls (see same-name line), not this one\n"
                "[ccg] dispatch: none (direct calls only)";
            cg::test::check(
                r,
                text == want,
                "V7.1 render: zero-caller call-shape branch");
        }
        // 调用者 + 同名 top3 带尾部 + 派发分层 + reason 裁剪。
        {
            InjectionFacts f;
            f.name = "what";
            f.kind = "method";
            f.qualified = "AcmeException::what";
            f.parent = "AcmeException";
            f.params = {};
            f.rel_file = "include/acme/AcmeException.hpp";
            f.line = 174;
            f.col = 17;
            f.same_name_callable = 12;
            f.same_name_others = {
                InjectionSameName{
                    "Exceptions::PythonException::what",
                    "acme/Core/Exceptions/src/PythonException.cpp",
                    144},
                InjectionSameName{
                    "Utils::AcmeJsonImplRefExpiredException::what",
                    "acme/Core/Utils/include/acme/Core/Utils/AcmeJsonImpl.h",
                    35},
                InjectionSameName{
                    "Utils::AcmeJsonImplJsonIsNotArray::what",
                    "acme/Core/Utils/include/acme/Core/Utils/"
                    "AcmeJsonImpl.h",
                    44},
                InjectionSameName{
                    "Swig::DirectorException::what",
                    "build/mingw-debug/SwigCpp/"
                    "AcmeJAVA_wrap.cxx",
                    5398},
                InjectionSameName{
                    "Swig::DirectorException::what",
                    "build/mingw-debug/SwigCpp/"
                    "AcmePYTHON_wrap.cxx",
                    8492},
                InjectionSameName{
                    "lua_Debug::what",
                    "build/mingw-debug/_deps/acme_lua-src/lua.h",
                    479}};
            hk::InjectionCaller c1;
            // 调用者喂进按 key 推导的限定形态（全局函数的
            // key 父链为空，显示名就是裸名）。
            c1.qualified = "AcmeMakeAcmeGuid";
            c1.first_rel_file = "acme/Core/PluginGateway/src/IAcmeBase.cpp";
            c1.first_line = 29;
            c1.occ = 4;
            hk::InjectionCaller c2;
            c2.qualified =
                "Acme::Core::PluginGateway::AcmeInputFactoryVectorImpl::"
                "InternalFind";
            c2.first_rel_file =
                "acme/Core/PluginGateway/src/IAcmeInputFactoryVectorImpl.cpp";
            c2.first_line = 22;
            c2.occ = 2;
            hk::InjectionCaller c3;
            c3.qualified = "Acme::Core::PluginGateway::ComponentFactory::"
                           "FindInterface";
            c3.first_rel_file =
                "acme/Core/PluginGateway/src/ComponentFactory.cpp";
            c3.first_line = 60;
            c3.occ = 2;
            f.callers = {c1, c2, c3};
            f.dispatch.present = true;
            f.dispatch.sites = 8;
            f.dispatch.resolved = 0;
            hk::InjectionCandidate top;
            top.display_source =
                "Acme::Core::Exceptions::PythonException::what";
            top.loc = "acme/Core/Exceptions/src/PythonException.cpp:144";
            top.confidence = "fallback";
            top.reason = "no_dynamic_type_evidence";
            f.dispatch.candidates.push_back(std::move(top));
            const std::string text = hk::render_injection_v71(f);
            const std::string want =
                "[ccg] AcmeException::what(void) — method of AcmeException\n"
                "[ccg] def: include/acme/AcmeException.hpp:174:17\n"
                "[ccg] same-name(12): Exceptions::PythonException::what "
                "@acme/Core/Exceptions/src/PythonException.cpp:144; Utils::"
                "AcmeJsonImplRefExpiredException::what @acme/Core/Utils/include/"
                "acme/Core/Utils/AcmeJsonImpl.h:35; Utils::AcmeJsonImplJsonIsNot"
                "Array::what @acme/Core/Utils/include/acme/Core/Utils/AcmeJsonImpl"
                ".h:44 (+3 more) — hits at those coords belong to other "
                "symbols, not this one\n"
                "[ccg] callers(top3): AcmeMakeAcmeGuid (acme/Core/"
                "PluginGateway/src/IAcmeBase.cpp:29, x4); "
                "AcmeInputFactoryVectorImpl::InternalFind (acme/Core/"
                "PluginGateway/src/IAcmeInputFactoryVectorImpl.cpp:22, "
                "x2); ComponentFactory::FindInterface (acme/Core/"
                "PluginGateway/src/ComponentFactory.cpp:60, x2)\n"
                "[ccg] dispatch: 8 virtual sites, 0 resolved; 1 impl candidates "
                "by confidence [fallback x1]; top: PythonException::what "
                "[fallback] (no_dynamic_type_evidence) @acme/Core/Exceptions/"
                "src/PythonException.cpp:144";
            cg::test::check(
                r,
                text == want,
                "V7.1 render: full five-line shape with dispatch "
                "tiers");
        }
        // 出现上限：x999，绝不出 x1000+。
        {
            InjectionFacts f;
            f.name = "g";
            f.kind = "function";
            f.qualified = "g";
            f.parent = "";
            f.rel_file = "a.cpp";
            f.line = 1;
            f.col = 5;
            hk::InjectionCaller heavy;
            heavy.qualified = "heavy_caller";
            heavy.first_rel_file = "b.cpp";
            heavy.first_line = 7;
            heavy.occ = 1200;
            f.callers.push_back(std::move(heavy));
            const std::string text = hk::render_injection_v71(f);
            cg::test::check(
                r,
                text.find("heavy_caller (b.cpp:7, x999)") != std::string::npos,
                "V7.1 render: occurrence suffix caps at x999");
        }
        // 预算阶梯地板：超预算文本无 ';' 也无 '(+' 尾巴
        // 时走到硬截断步并留在预算内。
        {
            InjectionFacts f;
            f.name = std::string(400, 'F');
            f.kind = "function";
            f.qualified = f.name; // 单段：short-qual 步是空操作
            f.parent = "";
            f.rel_file = std::string(400, 'r') + ".cpp";
            f.line = 1;
            f.col = 1;
            hk::InjectionCaller wide;
            wide.qualified = std::string(500, 'C');
            wide.first_rel_file = std::string(450, 'd') + ".hpp";
            wide.first_line = 2;
            wide.occ = 1;
            f.callers.push_back(std::move(wide));
            const std::string text = hk::render_injection_v71(f);
            cg::test::check(
                r,
                text.size() <= hk::PTU_INJECTION_BUDGET,
                "V7.1 degrade ladder keeps the byte budget");
            cg::test::check(
                r,
                text.ends_with(" …[ccg 截断]"),
                "V7.1 degrade ladder ends in the truncation marker");
        }
    }

    // Grep 模式过滤器与受限正则路径：整标识符（链）在装饰剥离后幸存、
    // 顶层交替拆分与分支级合并、形态护栏拒指数回溯形状。
    void grep_pattern_filter_units(R& r)
    {
        // Grep 模式过滤器：整标识符（链）在装饰
        // 剥离后幸存；交替/调用形态/点号不行。
        cg::test::check(
            r,
            hk::grep_pattern_identifier("IsFalsySwitchWord")
                == "IsFalsySwitchWord",
            "pattern filter: bare identifier passes");
        cg::test::check(
            r,
            hk::grep_pattern_identifier("\\bIsOk\\b") == "IsOk",
            "pattern filter: word-boundary decorations stripped");
        cg::test::check(
            r,
            hk::grep_pattern_identifier("(?<![A-Za-z0-9_])IsOk(?![A-Za-z0-9_])")
                == "IsOk",
            "pattern filter: lookaround guards stripped");
        cg::test::check(
            r,
            hk::grep_pattern_identifier(
                "^SerializeCaptureResult"
                "$")
                == "SerializeCaptureResult",
            "pattern filter: anchors stripped");
        cg::test::check(
            r,
            hk::grep_pattern_identifier("AcmeException::what")
                == "AcmeException::what",
            "pattern filter: qualified chain passes");
        cg::test::check(
            r,
            hk::grep_pattern_identifier("foo|bar").empty()
                && hk::grep_pattern_identifier("x->Check(...)").empty()
                && hk::grep_pattern_identifier("draw.*shape").empty()
                && hk::grep_pattern_identifier("").empty(),
            "pattern filter: non-identifier regexes are dropped");
        // 调用形尾巴（定调动作的常见写法）剥到链；前导分组不剥。
        cg::test::check(
            r,
            hk::grep_pattern_identifier("IsOk\\(") == "IsOk"
                && hk::grep_pattern_identifier("IsOk()") == "IsOk"
                && hk::grep_pattern_identifier("AcmeException::what\\(")
                       == "AcmeException::what"
                && hk::grep_pattern_identifier("\\bIsOk\\(\\)") == "IsOk",
            "pattern filter: call-shape tails stripped to the chain");
        cg::test::check(
            r,
            hk::grep_pattern_identifier("(IsOk)").empty(),
            "pattern filter: leading groups are not stripped");
        // 顶层交替拆分（汇合判定树的第一步）：括号深度 0 的裸 | 才切；
        // 转义竖线、括号内竖线、空分支、不平衡括号、超限分支与空输入
        // 一律整体静默（返回空列表）。
        {
            cg::test::check(
                r,
                hk::split_top_alternation("pi")
                    == std::vector<std::string>{"pi"},
                "split: no alternation returns the whole pattern");
            const std::vector<std::string> two =
                hk::split_top_alternation("pi|area");
            cg::test::check(
                r,
                two == std::vector<std::string>({"pi", "area"}),
                "split: top-level pipe cuts branches");
            cg::test::check(
                r,
                hk::split_top_alternation("a|b|c")
                    == std::vector<std::string>({"a", "b", "c"}),
                "split: three branches");
            cg::test::check(
                r,
                hk::split_top_alternation("f(a|b)")
                        == std::vector<std::string>{"f(a|b)"}
                    && hk::split_top_alternation("(a|b)")
                           == std::vector<std::string>{"(a|b)"},
                "split: pipes inside groups stay in their branch");
            cg::test::check(
                r,
                hk::split_top_alternation("foo\\|bar")
                    == std::vector<std::string>{"foo\\|bar"},
                "split: escaped pipe is a literal, not a cut");
            cg::test::check(
                r,
                hk::split_top_alternation("a||b").empty()
                    && hk::split_top_alternation("a|").empty()
                    && hk::split_top_alternation("|a").empty(),
                "split: empty branch rejects the whole pattern");
            cg::test::check(
                r,
                hk::split_top_alternation("a|b|c|d|e|f|g|h|i").empty(),
                "split: nine branches reject the whole pattern");
            cg::test::check(
                r,
                hk::split_top_alternation("").empty()
                    && hk::split_top_alternation("   ").empty(),
                "split: blank input rejects the whole pattern");
            cg::test::check(
                r,
                hk::split_top_alternation("a)b").empty()
                    && hk::split_top_alternation("(a|b").empty(),
                "split: unbalanced parens reject the whole pattern");
        }
        // 汇合判定树的分支级语义：噪声分支丢弃、ident 去重保序、多
        // pattern 合并（rg -e pi -e area 等价交替）、正则路径 pattern
        // 取"非噪声分支原始文本重拼"（绝不取用户原文整条）。
        {
            const auto one = hk::classify_lookup_patterns({"pi|class"});
            cg::test::check(
                r,
                one.has_value() && one->alive == std::vector<std::string>{"pi"}
                    && one->non_noise_raw == std::vector<std::string>{"pi"},
                "classify: noise branch is dropped, survivor stays");
            const auto dup = hk::classify_lookup_patterns({"pi|pi"});
            cg::test::check(
                r,
                dup.has_value() && dup->alive.size() == 1
                    && dup->alive.front() == "pi",
                "classify: duplicate branches dedup to one ident");
            const auto noise = hk::classify_lookup_patterns({"class|std"});
            cg::test::check(
                r,
                noise.has_value() && noise->alive.empty() && noise->noise_only
                    && noise->non_noise_raw.empty(),
                "classify: all-noise branches never reach the regex path");
            const auto mixed = hk::classify_lookup_patterns({"class|Get\\w+"});
            cg::test::check(
                r,
                mixed.has_value() && mixed->alive.empty() && !mixed->noise_only
                    && join_pipes(mixed->non_noise_raw) == "Get\\w+",
                "classify: regex pattern re-joins non-noise raw branches "
                "only");
            const auto multi = hk::classify_lookup_patterns({"pi", "area"});
            cg::test::check(
                r,
                multi.has_value()
                    && multi->alive == std::vector<std::string>{"pi", "area"}
                    && join_pipes(multi->non_noise_raw) == "pi|area",
                "classify: multi-pattern request merges like alternation");
            cg::test::check(
                r,
                !hk::classify_lookup_patterns({"pi|a|b|c|d|e|f|g|h"})
                     .has_value(),
                "classify: over-limit merged branches stay silent");
        }
        // 受限正则路径的形态护栏：长度上限、纯装饰拒、相邻量词、右括号
        // 后紧跟量词的一切组合（含构造合法不抛异常的指数回溯形态——机械
        // 检测是唯一防线）；(ab)+ 无害形态同拒是保守口径的既定代价。
        cg::test::check(
            r,
            hk::regex_pattern_allowed(std::string(61, 'G') + "\\w+")
                && !hk::regex_pattern_allowed(std::string(62, 'G') + "\\w+"),
            "regex guard: 64-byte boundary");
        cg::test::check(
            r,
            !hk::regex_pattern_allowed("a**")
                && !hk::regex_pattern_allowed("a*+")
                && !hk::regex_pattern_allowed("a+?")
                && !hk::regex_pattern_allowed("a?{2}"),
            "regex guard: adjacent quantifiers reject");
        cg::test::check(
            r,
            !hk::regex_pattern_allowed("(a*)*")
                && !hk::regex_pattern_allowed("(\\w+)+"),
            "regex guard: quantified group holding quantifiers rejects");
        cg::test::check(
            r,
            !hk::regex_pattern_allowed("(a|aa)+")
                && !hk::regex_pattern_allowed("(\\w|\\w\\w)+")
                && !hk::regex_pattern_allowed("(ab)+"),
            "regex guard: any quantifier right after a closing paren "
            "rejects");
        cg::test::check(
            r,
            hk::regex_pattern_allowed("Get\\w+")
                && hk::regex_pattern_allowed("on_\\w+")
                && hk::regex_pattern_allowed("[A-Z]\\w+")
                && hk::regex_pattern_allowed("^(Get|Set)\\w+$"),
            "regex guard: bounded family shapes pass");
        cg::test::check(
            r,
            !hk::regex_pattern_allowed("^$")
                && !hk::regex_pattern_allowed("^^^")
                && !hk::regex_pattern_allowed("^\\b$")
                && !hk::regex_pattern_allowed(""),
            "regex guard: pure-decoration patterns reject");
    }

    // 多词候选清单渲染：首行披露、term 序优先、(+K more) 尾注、
    // 首行/条目域显示截断、硬截档保险丝与 1400B 恒有界。
    void injection_candidate_list_units(R& r)
    {
        {
            std::vector<hk::MultiLookupEntry> entries;
            entries.push_back(
                hk::MultiLookupEntry{
                    "pi",
                    "shape::pi",
                    "function",
                    3,
                    "circle.cpp",
                    8});
            entries.push_back(
                hk::MultiLookupEntry{
                    "area",
                    "shape::Circle::area",
                    "method",
                    7,
                    "circle.cpp",
                    14});
            const std::string text =
                hk::render_multi_lookup_list("pi|area", "2 terms", entries);
            cg::test::check(
                r,
                text.find("\"pi|area\" matched 2 symbols across 2 terms")
                    != std::string::npos,
                "multi list: first line discloses pattern and term count");
            cg::test::check(
                r,
                text.find("[ccg] - shape::pi — function, 3 refs @circle.cpp:8")
                    < text.find(
                        "[ccg] - shape::Circle::area — method, 7 refs "
                        "@circle.cpp:14"),
                "multi list: term order outranks global refs order");
            cg::test::check(
                r,
                text.find("re-grep with one term to pin its facts")
                    != std::string::npos,
                "multi list: closing line teaches the pinning move");
        }
        {
            // term 内 refs 降序（并列走路径/行号升序）。
            std::vector<hk::MultiLookupEntry> entries;
            entries.push_back(
                hk::MultiLookupEntry{
                    "scale",
                    "shape::Circle::scale",
                    "method",
                    1,
                    "b.cpp",
                    2});
            entries.push_back(
                hk::MultiLookupEntry{
                    "scale",
                    "shape::Circle::scale",
                    "method",
                    9,
                    "a.cpp",
                    1});
            const std::string text =
                hk::render_multi_lookup_list("scale|scale", "1 terms", entries);
            cg::test::check(
                r,
                text.find("9 refs @a.cpp:1") < text.find("1 refs @b.cpp:2"),
                "multi list: within a term, refs descend");
        }
        {
            // 超上限条目：shown 封顶、(+K more) 尾注挂最后一个保留条目。
            std::vector<hk::MultiLookupEntry> entries;
            for (int i = 0; i < 7; ++i)
            {
                entries.push_back(
                    hk::MultiLookupEntry{
                        "t" + std::to_string(i),
                        "ns" + std::to_string(i) + "::g",
                        "function",
                        9 - i,
                        "f" + std::to_string(i) + ".cpp",
                        i + 1});
            }
            const std::string text =
                hk::render_multi_lookup_list("g|h", "2 terms", entries);
            cg::test::check(
                r,
                text.find("matched 7 symbols across 2 terms")
                        != std::string::npos
                    && text.find("  (+2 more)") != std::string::npos,
                "multi list: over-cap entries collapse to a (+K more) tail");
        }
        {
            // 条目域显示截断（qualified 96B / rel_file 64B）与首行 pattern
            // 64B 截断：构造超长字段断言输出有界、截断串在场而原文整串
            // 不在场。
            const std::string long_qual = "a::" + std::string(120, 'q') + "::z";
            const std::string long_file = std::string(120, 'p') + ".cpp";
            std::vector<hk::MultiLookupEntry> entries;
            entries.push_back(
                hk::MultiLookupEntry{
                    "pi",
                    long_qual,
                    "function",
                    0,
                    long_file,
                    1});
            const std::string long_pattern(100, 'x');
            const std::string text =
                hk::render_multi_lookup_list(long_pattern, "1 terms", entries);
            cg::test::check(
                r,
                text.size() <= hk::PTU_INJECTION_BUDGET
                    && text.find(std::string(100, 'x')) == std::string::npos
                    && text.find(long_qual) == std::string::npos
                    && text.find(long_file) == std::string::npos,
                "multi list: over-long pattern and entry fields are clipped");
        }
        {
            // 硬截档保险丝：条目域截断后正常路径走不到这里，但保险丝必
            // 须机械可证——以无界 terms_note（调用方合成、渲染器不截断）
            // 构造超预算文本，断言 ≤1400B 且以截断标记收尾。
            std::vector<hk::MultiLookupEntry> entries;
            entries.push_back(
                hk::MultiLookupEntry{
                    "pi",
                    "shape::pi",
                    "function",
                    3,
                    "circle.cpp",
                    8});
            const std::string text = hk::render_multi_lookup_list(
                "pi",
                std::string(3000, 'T'),
                entries);
            cg::test::check(
                r,
                text.size() <= hk::PTU_INJECTION_BUDGET
                    && text.ends_with(" …[ccg 截断]"),
                "multi list: hard truncation fuse caps at the budget");
        }
        // 超限计数披露行：单行、symbols/names 两种口径。
        cg::test::check(
            r,
            hk::render_overmatch_disclosure("pi|area", 7)
                == "[ccg] \"pi|area\" matched 7 symbols — too many to "
                   "list; narrow the pattern",
            "overmatch disclosure: symbols wording");
        cg::test::check(
            r,
            hk::render_overmatch_disclosure("\\w+", 42, "names")
                    .find("matched 42 names — too many to list")
                != std::string::npos,
            "overmatch disclosure: names wording");
        cg::test::check(
            r,
            hk::render_overmatch_disclosure(std::string(100, 'y'), 9)
                    .find(std::string(100, 'y'))
                == std::string::npos,
            "overmatch disclosure: pattern display is clipped");
        // 开关三态语义归 e2e 进程级用例；单测只钉"回落默认"这一态
        // （宿主残留由 ctest ENVIRONMENT 显式置空值压过——Windows 环境
        // 名查找大小写不敏感，小写变体同样被覆盖）。
        cg::test::check(
            r,
            hk::pretooluse_regex_enabled() == hk::PTU_REGEX_DEFAULT_ENABLED,
            "regex switch: unset or foreign value falls back to the "
            "built-in default");
        // 候选清单渲染：种子形态（N 计数、refs 降序、(+M more) 尾注挂在
        // 最后一个候选行、末行提示用头部候选的限定名）。
        {
            hk::CandidateListFacts f;
            f.name = "what";
            f.entries = {
                hk::InjectionListEntry{
                    "Ort::Exception::what",
                    "method",
                    18,
                    "ort/onnxruntime_cxx_api.h",
                    59},
                hk::InjectionListEntry{
                    "Acme::Core::Utils::AcmeJsonImplRefExpiredException::what",
                    "method",
                    11,
                    "acme/Core/Utils/include/acme/Core/Utils/AcmeJsonImpl.h",
                    35},
                hk::InjectionListEntry{
                    "AcmeException::what",
                    "method",
                    8,
                    "include/acme/AcmeException.hpp",
                    174},
                hk::InjectionListEntry{
                    "sol::error::what",
                    "method",
                    3,
                    "deps/sol/error.hpp",
                    82},
                hk::InjectionListEntry{
                    "lua_Debug::what",
                    "method",
                    1,
                    "deps/acme_lua/lua.h",
                    479},
                hk::InjectionListEntry{
                    "sixth::what",
                    "method",
                    0,
                    "sixth.cpp",
                    1}};
            const std::string text = hk::render_candidate_list(f);
            const std::string want =
                "[ccg] \"what\" matches 6 same-name symbols (no dominant one) "
                "— candidates by references:\n"
                "[ccg] - Ort::Exception::what — method, 18 refs @"
                "ort/onnxruntime_cxx_api.h:59\n"
                "[ccg] - Acme::Core::Utils::AcmeJsonImplRefExpiredException::"
                "what — method, 11 refs @acme/Core/Utils/include/acme/Core/"
                "Utils/AcmeJsonImpl.h:35\n"
                "[ccg] - AcmeException::what — method, 8 refs @include/acme/"
                "AcmeException.hpp:174\n"
                "[ccg] - sol::error::what — method, 3 refs @deps/sol/"
                "error.hpp:82\n"
                "[ccg] - lua_Debug::what — method, 1 refs @deps/acme_lua/"
                "lua.h:479  (+1 more)\n"
                "[ccg] re-grep with a qualified name (e.g. Ort::Exception::"
                "what) or a path filter to pin one symbol\n"
                "[ccg] full same-name list in one shot: ccg query what";
            cg::test::check(
                r,
                text == want,
                "candidate list: seed shape with head-name hint");
        }
        // 候选数 ≤ 5：无尾注；预算超限：从尾部裁候选行、尾注计数增大、
        // 首行与提示行保留。
        {
            hk::CandidateListFacts f;
            f.name = "g";
            f.entries = {
                hk::InjectionListEntry{"a::g", "function", 9, "a.cpp", 1},
                hk::InjectionListEntry{"b::g", "function", 8, "b.cpp", 2},
                hk::InjectionListEntry{"c::g", "function", 7, "c.cpp", 3}};
            const std::string text = hk::render_candidate_list(f);
            cg::test::check(
                r,
                text.find("matches 3 same-name symbols") != std::string::npos
                    && text.find("(+") == std::string::npos
                    && text.find("[ccg] - c::g — function, 7 refs @c.cpp:3")
                           != std::string::npos,
                "candidate list: no tail when entries fit the cap");
            hk::CandidateListFacts wide;
            wide.name = "g";
            for (int i = 0; i < 5; ++i)
            {
                wide.entries.push_back(
                    hk::InjectionListEntry{
                        "ns" + std::to_string(i) + "::g",
                        "function",
                        9 - i,
                        std::string(300, 'p') + std::to_string(i) + ".cpp",
                        i + 1});
            }
            const std::string squeezed = hk::render_candidate_list(wide);
            cg::test::check(
                r,
                squeezed.size() <= hk::PTU_INJECTION_BUDGET
                    && squeezed.find("matches 5 same-name symbols")
                           != std::string::npos
                    && squeezed.find("re-grep with a qualified name")
                           != std::string::npos,
                "candidate list: over-budget rows drop, head and hint stay");
        }
    }

    // 注入层共享词汇微钉：噪声 token、限定名裁剪、码点裁剪、
    // 置信档排序与路径预筛归一化。
    void injection_helper_vocabulary_units(R& r)
    {
        cg::test::check(
            r,
            hk::is_noise_token("auto") && !hk::is_noise_token("IsOk"),
            "pattern filter: C++ keywords are noise tokens");
        // short_qual / shorten_cp 微钉（共享裁剪词汇）。
        cg::test::check(
            r,
            hk::short_qual("Napi::Maybe::Check", 2) == "Maybe::Check"
                && hk::short_qual("Acme::IsOk", 3) == "Acme::IsOk"
                && hk::short_qual("a::b::c::d", 2) == "c::d",
            "short_qual keeps the last N segments");
        cg::test::check(
            r,
            hk::shorten_cp("const std::filesystem::path &", 24)
                    == "const std::filesystem::…"
                && hk::shorten_cp("short", 24) == "short"
                && hk::shorten_cp(" pad ", 24) == "pad",
            "shorten_cp clips to limit-1 code points plus the ellipsis");
        cg::test::check(
            r,
            hk::confidence_rank("high") == 3
                && hk::confidence_rank("medium") == 2
                && hk::confidence_rank("low") == 1
                && hk::confidence_rank("fallback") == 1,
            "confidence ranks: high>medium>low, unknown as low");
        // 路径预筛归一化四形态：裸目录 / 尾正斜杠 / 尾反斜杠 / 大写混合
        // 必须同形——尾斜杠不去会让锚定匹配三形态全落空（整筛成空=静默）。
        cg::test::check(
            r,
            hk::norm_path_filter("acme") == "acme"
                && hk::norm_path_filter("acme/") == "acme"
                && hk::norm_path_filter("acme\\") == "acme"
                && hk::norm_path_filter("Acme\\Plugins\\") == "acme/plugins"
                && hk::norm_path_filter("e:/Src/Ref/") == "e:/src/ref",
            "path filter normalizes separators, case and trailing slashes");
    }

    const cg::test::Registrar reg_ptu{
        "cli.hook.pretooluse_injection",
        &pretooluse_injection_units};
    const cg::test::Registrar reg_pattern_filter{
        "cli.hook.grep_pattern_filter",
        &grep_pattern_filter_units};
    const cg::test::Registrar reg_candidate_list{
        "cli.hook.injection_candidate_list",
        &injection_candidate_list_units};
    const cg::test::Registrar reg_helper_vocab{
        "cli.hook.injection_helper_vocabulary",
        &injection_helper_vocabulary_units};

    /**
     * @brief 结构化查找请求的归一化闸门（四工具监听的 shell 臂汇合点）。
     *
     * @internal 多意图形态（多请求/多路径/通配路径）静默放弃；多 pattern
     *           （OR 语义与顶层交替同构）按交替分支同款护栏转发并查；
     *           fixed 字面串必须剥装饰恒等（裸标识符链）才转发且绝不拆；
     *           AfterCd 的 cwd 提示收窄路径过滤。
     */
    /// 单意图查找请求的提取产出形态。
    [[nodiscard]]
    auto make_single_lookup(
        std::string              pattern,
        std::vector<std::string> paths = {},
        bool fixed = false) -> cg::util::shellscan::ShellExtractOutcome
    {
        namespace scan = cg::util::shellscan;
        scan::ShellExtractOutcome outcome;
        scan::ShellRequest        request;
        request.patterns = {std::move(pattern)};
        request.single = true;
        request.fixed = fixed;
        request.paths = std::move(paths);
        outcome.requests.push_back(std::move(request));
        return outcome;
    }

    void normalize_lookup_cases(R& r)
    {
        namespace scan = cg::util::shellscan;
        // 单意图正则请求：pattern 原样转发（装饰留给意图判定剥）。
        {
            const auto lookup = hk::normalize_shell_lookup(
                make_single_lookup("pi", {"src/db"}));
            cg::test::check(
                r,
                lookup.has_value() && lookup->pattern == "pi"
                    && lookup->path_filter == "src/db",
                "normalize: single request passes through");
        }
        {
            const auto lookup =
                hk::normalize_shell_lookup(make_single_lookup("IsOk()"));
            cg::test::check(
                r,
                lookup.has_value() && lookup->pattern == "IsOk()",
                "normalize: regex semantics keep call shape");
        }
        // fixed 字面串：裸链恒等转发；含元字符的字面在找非符号文本。
        {
            const auto lookup =
                hk::normalize_shell_lookup(make_single_lookup("pi", {}, true));
            cg::test::check(
                r,
                lookup.has_value() && lookup->pattern == "pi",
                "normalize: fixed bare chain forwards");
        }
        cg::test::check(
            r,
            !hk::normalize_shell_lookup(make_single_lookup("IsOk()", {}, true))
                 .has_value(),
            "normalize: fixed literal with call shape drops");
        cg::test::check(
            r,
            !hk::normalize_shell_lookup(make_single_lookup("^pi$", {}, true))
                 .has_value(),
            "normalize: fixed literal with anchors drops");
        // 多意图与歧义形态：多请求、多 pattern、多路径、通配路径。
        {
            scan::ShellExtractOutcome two = make_single_lookup("foo");
            two.requests.push_back(make_single_lookup("bar").requests.front());
            cg::test::check(
                r,
                !hk::normalize_shell_lookup(two).has_value(),
                "normalize: multiple search units drop (multi-intent)");
        }
        {
            // 多 pattern 翻转：rg -e a -e b 是 OR 语义的"多词并查"，与顶层
            // 交替同构——非 fixed 形态转发（首 pattern + extra_patterns），
            // 汇合判定层按交替分支同款护栏消费。
            scan::ShellExtractOutcome multi = make_single_lookup("foo");
            multi.requests.front().patterns.push_back("bar");
            multi.requests.front().single = false;
            const auto forwarded = hk::normalize_shell_lookup(multi);
            cg::test::check(
                r,
                forwarded.has_value() && forwarded->pattern == "foo"
                    && forwarded->extra_patterns
                           == std::vector<std::string>{"bar"},
                "normalize: multi-pattern request forwards as one "
                "multi-term lookup");
        }
        {
            // 多 pattern 合计上限与交替分支同口径：9 个 pattern 拒。
            scan::ShellExtractOutcome nine = make_single_lookup("p1");
            for (int i = 2; i <= 9; ++i)
            {
                nine.requests.front().patterns.push_back(
                    "p" + std::to_string(i));
            }
            nine.requests.front().single = false;
            cg::test::check(
                r,
                !hk::normalize_shell_lookup(nine).has_value(),
                "normalize: nine patterns drop (branch cap)");
        }
        {
            // fixed 字面多 pattern：多意图字面查找，绝不拆交替。
            scan::ShellExtractOutcome fixed2 =
                make_single_lookup("a", {}, true);
            fixed2.requests.front().patterns.push_back("b");
            fixed2.requests.front().single = false;
            cg::test::check(
                r,
                !hk::normalize_shell_lookup(fixed2).has_value(),
                "normalize: fixed multi-pattern drops (never split)");
        }
        cg::test::check(
            r,
            !hk::normalize_shell_lookup(make_single_lookup("a|b", {}, true))
                 .has_value(),
            "normalize: fixed literal with a pipe drops");
        {
            // 引擎显式否决单意图（single=false 的单 pattern 请求）仍拒：
            // 各提取层对多 pattern 均按数量派生 single，该形态只能来自引
            // 擎 JSON 直写——引擎语义漂移的防线。
            scan::ShellExtractOutcome dissent = make_single_lookup("pi");
            dissent.requests.front().single = false;
            cg::test::check(
                r,
                !hk::normalize_shell_lookup(dissent).has_value(),
                "normalize: engine-dissented single-pattern request drops");
        }
        cg::test::check(
            r,
            !hk::normalize_shell_lookup(
                 make_single_lookup("pi", {"src", "include"}))
                 .has_value(),
            "normalize: multiple paths drop (range ambiguous)");
        cg::test::check(
            r,
            !hk::normalize_shell_lookup(make_single_lookup("pi", {"*.cpp"}))
                 .has_value(),
            "normalize: wildcard path drops (range unknown)");
        {
            scan::ShellExtractOutcome shadowed = make_single_lookup("pi");
            shadowed.skip_all = "alias-shadow";
            cg::test::check(
                r,
                !hk::normalize_shell_lookup(shadowed).has_value(),
                "normalize: aliased-rg context drops the whole line");
        }
        {
            scan::ShellExtractOutcome empty_pattern;
            empty_pattern.requests.push_back(scan::ShellRequest{});
            cg::test::check(
                r,
                !hk::normalize_shell_lookup(empty_pattern).has_value(),
                "normalize: request without patterns drops");
        }
        // AfterCd 的 cwd 提示：无路径时单独成过滤串；相对路径拼接；
        // 绝对路径过滤串与 cd 无关。
        {
            auto outcome = make_single_lookup("pi");
            outcome.requests.front().path_kind = scan::PathKind::AfterCd;
            outcome.requests.front().cwd_hint = "src";
            const auto lookup = hk::normalize_shell_lookup(outcome);
            cg::test::check(
                r,
                lookup.has_value() && lookup->path_filter == "src",
                "normalize: cd hint becomes the path filter");
        }
        {
            auto outcome = make_single_lookup("pi", {"include"});
            outcome.requests.front().path_kind = scan::PathKind::AfterCd;
            outcome.requests.front().cwd_hint = "src";
            const auto lookup = hk::normalize_shell_lookup(outcome);
            cg::test::check(
                r,
                lookup.has_value() && lookup->path_filter == "src/include",
                "normalize: cd hint prefixes a relative path filter");
        }
        {
            auto outcome = make_single_lookup("pi", {"e:/abs/dir"});
            outcome.requests.front().path_kind = scan::PathKind::AfterCd;
            outcome.requests.front().cwd_hint = "src";
            const auto lookup = hk::normalize_shell_lookup(outcome);
            cg::test::check(
                r,
                lookup.has_value() && lookup->path_filter == "e:/abs/dir",
                "normalize: absolute path filter ignores cd hint");
        }
        // 管道中段（stdin 语义）：无路径提示，全库口径。
        {
            auto outcome = make_single_lookup("pi");
            outcome.requests.front().path_kind = scan::PathKind::StdinPipe;
            const auto lookup = hk::normalize_shell_lookup(outcome);
            cg::test::check(
                r,
                lookup.has_value() && lookup->path_filter.empty(),
                "normalize: stdin-pipe keeps the whole-library scope");
        }
    }

    const cg::test::Registrar reg_normalize{
        "cli.hook.normalize_lookup",
        &normalize_lookup_cases};

    /**
     * @brief is_callable_kind 双重载的口径镜像对账。
     *
     * @internal 字符串版（磁盘声明定位/编辑后行号事实）与 SymbolKind 版
     * （候选清单过滤与 refs 口径）各自演化，历史上析构函数只在一侧——
     * 被漏的一侧会把 `p->~T()` 的显式调用行按非可调用处理。这里按
     * name_of 全值域逐 kind 对账，任一侧增删成员即红。
     */
    void hook_callable_kind_mirror(R& r)
    {
        using K = cg::db::SymbolKind;
        for (int i = 0; i < static_cast<int>(cg::db::kind_count); ++i)
        {
            const K                k = static_cast<K>(i);
            const std::string_view name = cg::db::name_of(k);
            cg::test::check(
                r,
                hk::is_callable_kind(k) == hk::is_callable_kind(name),
                std::format("callable mirror holds for kind {}", name));
        }
        cg::test::check(
            r,
            hk::is_callable_kind(K::Function) && hk::is_callable_kind(K::Method)
                && hk::is_callable_kind(K::Constructor)
                && hk::is_callable_kind(K::Destructor)
                && !hk::is_callable_kind(K::Record)
                && !hk::is_callable_kind(K::Enumeration)
                && !hk::is_callable_kind(K::EnumConstant)
                && !hk::is_callable_kind(K::Field)
                && !hk::is_callable_kind(K::Variable)
                && !hk::is_callable_kind(K::TypeAlias),
            "callable set is exactly fn/method/ctor/dtor");
    }

    const cg::test::Registrar reg_callable_mirror{
        "cli.hook.callable_kind_mirror",
        &hook_callable_kind_mirror};

    // 扩展名判定收编：钩子语料表旧版做字节精确比较，大写 .CPP
    // 被静默漏判（钩子对整个文件失明）。收编到 util 分类单点后按折叠后缀
    // 判定——大小写变体必须命中，非语料仍排除。
    void hook_corpus_file_folding(R& r)
    {
        cg::test::check(
            r,
            hk::is_corpus_file("e:/Src/Main.CPP"),
            "uppercase .CPP is corpus (the silent miss, fixed)");
        cg::test::check(
            r,
            hk::is_corpus_file("e:/inc/Widget.H"),
            "uppercase .H is corpus");
        cg::test::check(
            r,
            hk::is_corpus_file("mod.IXX"),
            "uppercase .IXX is corpus");
        cg::test::check(
            r,
            !hk::is_corpus_file("notes.md")
                && !hk::is_corpus_file("CMakeLists.txt")
                && !hk::is_corpus_file("app.py"),
            "non-corpus suffixes stay silent");
        cg::test::check(
            r,
            !hk::is_corpus_file("proj.sln"),
            "project files ride the recollect channel, not corpus");
    }

    const cg::test::Registrar reg_corpus_folding{
        "cli.hook.corpus_file_folding",
        &hook_corpus_file_folding};

    // ------------------------------------------------ hook 词法扫描器金样 ----
    // mask_literals 与 code_fingerprint 的逐字符期望值：两份五状态扫描器
    // 的口径差异（原始字符串识别与否）就钉在这里——状态转移层的任何
    // 重排/合并让输出漂移一格都会红。期望值全部为现状行为的金样（含
    // 非法前缀 `XR"` 后 `R"` 恰好自闭合两字符、指纹对原始字符串刻意
    // 不识别这类"怪但承重"的形态）。
    void hook_scanner_golden(R& r)
    {

        // 掩码：注释/字面量内容变空格，字节长度与换行原样。
        const std::string src1 = "int x; // note\nint y; /* block */ z;";
        const std::string want1 = "int x; " + std::string(7, ' ') + "\n"
                                  + "int y; " + std::string(11, ' ') + " z;";
        cg::test::check(
            r,
            hk::mask_literals(src1) == want1
                && hk::mask_literals(src1).size() == src1.size(),
            "mask blanks comments, keeps code and line structure");
        const std::string src8 = "a /* x\r\ny */ b";
        cg::test::check(
            r,
            hk::mask_literals(src8)
                == "a " + std::string(4, ' ') + "\r\n" + std::string(4, ' ')
                       + " b",
            "mask preserves CR/LF inside block comments");

        // 掩码：字符串/字符字面量连同引号与转义对整体变空格。
        const std::string src2 = "f(\"a b\"); c='\\''; d=\"\\\"\";";
        cg::test::check(
            r,
            hk::mask_literals(src2)
                    == "f(" + std::string(5, ' ')
                           + "); c=" + std::string(4, ' ')
                           + "; d=" + std::string(4, ' ') + ";"
                && hk::mask_literals(src2).size() == src2.size(),
            "mask covers quotes and escape pairs");

        // 掩码：原始字符串整体识别（含定界符与后续代码的分界）。
        cg::test::check(
            r,
            hk::mask_literals("auto s = R\"(no // code here)\"; int t;")
                == "auto s = " + std::string(20, ' ') + "; int t;",
            "mask recognizes bare raw strings");
        // 编码前缀（u8R/uR/UR/LR）与前缀字符一并掩码；自定义定界符可用。
        cg::test::check(
            r,
            hk::mask_literals("x=u8R\"ab(c)ab\";y=1;")
                == "x=" + std::string(12, ' ') + ";y=1;",
            "mask includes the u8 encoding prefix");
        cg::test::check(
            r,
            hk::mask_literals("x=uR\"(c)\"; z;")
                == "x=" + std::string(7, ' ') + "; z;",
            "mask includes the u encoding prefix");
        // 定界符恰 16 字符合法；17 字符退化为非原始字符串路径。
        cg::test::check(
            r,
            hk::mask_literals("R\"abcdefghijklmnop(x)abcdefghijklmnop\" t;")
                == std::string(38, ' ') + " t;",
            "a 16-char raw delimiter is still raw");
        cg::test::check(
            r,
            hk::mask_literals("R\"abcdefghijklmnopq(x)abcdefghijklmnopq\" t;")
                == std::string(2, ' ') + "abcdefghijklmnopq(x)abcdefghijklmnopq"
                       + std::string(4, ' '),
            "a 17-char delimiter falls back to the plain-string path");

        // 掩码：非法前缀 XR" 的怪现状——R" 两字符恰好自成一个字符串，
        // 括号部分作为代码存活，直到下一个引号重新开串。
        cg::test::check(
            r,
            hk::mask_literals("int XR\"(a)\"; int b;")
                == "int X  (a) " + std::string(8, ' '),
            "illegal raw prefix keeps parens as code (golden)");

        // 掩码：未闭合形态一律吞到文本末尾，绝不越界。
        cg::test::check(
            r,
            hk::mask_literals("int a; R\"(never closed")
                == "int a; " + std::string(15, ' '),
            "unclosed raw string masks to EOF");
        cg::test::check(
            r,
            hk::mask_literals("x='\\") == "x=  ",
            "trailing backslash at EOF is masked, not skipped over");

        // 指纹：注释整段丢弃、代码空白丢弃、字面量逐字保留（含空格）。
        cg::test::check(
            r,
            hk::code_fingerprint("a; // note\nb;\t/* z */ c") == "a;b;c",
            "fingerprint drops comments and code whitespace");
        cg::test::check(
            r,
            hk::code_fingerprint("f(\"a  b\") ; g('x')")
                == "f(\"a  b\");g('x')",
            "fingerprint keeps string/char literals verbatim");
        cg::test::check(
            r,
            hk::code_fingerprint("x; //\"c\" ; y") == "x;",
            "line comment swallows embedded quotes");
        cg::test::check(
            r,
            hk::code_fingerprint("x = \"//\"; y") == "x=\"//\";y",
            "string-embedded slashes are not a comment");
        // 指纹的存在意义：纯注释/空白编辑不改变指纹。
        cg::test::check(
            r,
            hk::code_fingerprint("a=1;//x") == hk::code_fingerprint("a=1;  "),
            "comment-only edits leave the fingerprint unchanged");
        cg::test::check(
            r,
            hk::code_fingerprint("int f() {\n}\n// done")
                == hk::code_fingerprint("int f(){}"),
            "whitespace-only edits leave the fingerprint unchanged");

        // 指纹：原始字符串刻意不识别（与掩码侧的口径差异显式保留）。
        cg::test::check(
            r,
            hk::code_fingerprint("R\"( // note )\"") == "R\"( // note )\"",
            "fingerprint does not special-case raw strings");
        // 同一输入两副面孔：内部引号让指纹把后续代码当串内容、掩码则按
        // 原始字符串整体抹掉——这正是两策略差异的浓缩样本。
        cg::test::check(
            r,
            hk::code_fingerprint("R\"(q\")\" int x;") == "R\"(q\")\" int x;",
            "fingerprint mis-parses inner quotes verbatim (golden)");
        cg::test::check(
            r,
            hk::mask_literals("R\"(q\")\" int x;")
                == std::string(7, ' ') + " int x;",
            "mask closes the raw string at its true delimiter");

        // 掩码的下游用途：引用扫描在掩码文本上找独立标识符。
        cg::test::check(
            r,
            hk::line_has_identifier("int old = 1; // old", "old"),
            "identifier found in code survives masking");
        cg::test::check(
            r,
            !hk::line_has_identifier("f(\"old\"); // old", "old"),
            "identifiers inside literals/comments are ignored");
        cg::test::check(
            r,
            !hk::line_has_identifier("sold = 1;", "old"),
            "substring hits do not count as identifiers");
    }

    const cg::test::Registrar reg_scanner_golden{
        "cli.hook.scanner_golden",
        &hook_scanner_golden};

} // namespace

// NOLINTNEXTLINE(clang-diagnostic-main-attached-to-named-module) 用 module-unit
// main 是有意为之：项目每个源文件都是具名模块单元；普通 TU
// 的 main 会破坏这项全模块政策
export int main() { return cg::test::run_all(); }
