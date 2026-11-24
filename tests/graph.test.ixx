export module cg.test.graph;

// 图构建器与遍历测试：把 sample1 迷你工程经真实 Indexer
// 索引进临时数据库，断言解析后的边
// （重写链、跨文件 tier-1 调用、重载可分性、
// 声明/定义合并），经直接 SQL 探针断言零重复行，
// id 稳定的按文件增量替换，以及遍历
// 算法（BFS 最短深度、环终止、影响面风险阶梯、
// trace 跳数链与不可达报告）。

import std;
import cg.util;
import cg.util.cstd;
import cg.clang;
import cg.db;
import cg.graph;
import cg.test.harness;

namespace
{

    using R = cg::test::Result;
    namespace db = cg::db;
    namespace fs = std::filesystem;

    constexpr std::string_view fixture_dir = CCG_SAMPLE1_DIR;

    [[nodiscard]]
    auto root() -> std::string
    {
        return cg::util::norm_path(fixture_dir);
    }

    [[nodiscard]]
    auto load_commands() -> std::vector<cg::clang::CompileCommand>
    {
        return cg::clang::load(
            std::string{fixture_dir} + "/compile_commands.json",
            {});
    }

    /// 邻居结果中某符号的最短深度（不在结果内给 0）。
    [[nodiscard]]
    auto depth_in(const cg::graph::NeighborsResult& res, cg::db::SymId id)
        -> int
    {
        for (const auto& e : res.entries)
        {
            if (e.id == id)
            {
                return e.depth;
            }
        }
        return 0;
    }

    [[nodiscard]]
    auto make_indexer(cg::db::GraphStore& store, bool force)
        -> cg::graph::Indexer
    {
        cg::graph::IndexOptions opt;
        opt.roots = {root()};
        opt.force = force;
        opt.progress = false;
        opt.compile_db_path =
            std::string{fixture_dir} + "/compile_commands.json";
        return cg::graph::Indexer{store, std::move(opt)};
    }

    [[nodiscard]]
    auto read_text(const fs::path& p) -> std::string
    {
        std::ifstream      in{p, std::ios::binary};
        std::ostringstream buf;
        buf << in.rdbuf();
        return buf.str();
    }

    void write_text(const fs::path& p, const std::string& text)
    {
        std::ofstream out{p, std::ios::binary | std::ios::trunc};
        out << text;
    }

    void copy_tree(const fs::path& src, const fs::path& dst)
    {
        std::error_code ec;
        (void)fs::remove_all(dst, ec);
        // fs::copy 的 recursive 只建 dst 自身，不建缺失的父目录——
        // 父目录缺席时整棵拷贝静默失败（ec 被吞），后续 compile
        // database 载入才会以"打不开文件"炸出。先补父目录。
        (void)fs::create_directories(dst.parent_path(), ec);
        fs::copy(src, dst, fs::copy_options::recursive, ec);
    }

    /**
     * @brief JSON 字符串字面量引用（反斜杠与双引号转义后加引号）。
     *
     * @internal 手拼 compile_commands.json 的端到端用例统一走这里——
     * 转义漏一处就生成坏库文本，不在各用例里保留逐字拷贝。
     */
    [[nodiscard]]
    auto q(std::string_view s) -> std::string
    {
        std::string out = "\"";
        for (const char c : s)
        {
            if (c == '\\' || c == '"')
            {
                out.push_back('\\');
            }
            out.push_back(c);
        }
        return out + "\"";
    }

    // 按限定名查唯一符号（找不到时以 0 + 报错信息大声失败）。
    [[nodiscard]]
    auto find_one(
        cg::db::GraphStore& store,
        std::string_view    q,
        R&                  r,
        std::string_view    what) -> cg::db::SymId
    {
        // 精确解析走短路档（与生产 resolve_unique_target 同口径）：合并档
        // 会把子串层命中并入（如 kMaxCircles 顶爆 "Circle"）。
        const db::FindResult fr = db::find_symbols(
            store,
            q,
            db::SymbolFilter{},
            /*merge_lower_tiers=*/false);
        if (fr.matches.size() == 1)
        {
            return fr.matches.front().id;
        }
        cg::test::check(
            r,
            false,
            std::format("expected exactly one match for {} ({})", what, q));
        return 0;
    }

    [[nodiscard]]
    auto name_of(cg::db::GraphStore& store, cg::db::SymId id) -> std::string
    {
        const auto row = db::get_symbol(store, id);
        return row ? row->qualified_name : std::string{"?"};
    }

    // ---------------------------------------------------------------------------

    void tier1_scan_imports_of_degraded_tus(R& r)
    {
        // 命令合成已知晓失败的 TU
        // （自构建降级）从不解析——索引器逐字报告
        // 预定类，且仍从 tier-1 扫描的 requires 落下
        // 该 TU 手写的 import（行号 0）。
        db::GraphStore                         store{":memory:"};
        std::vector<cg::clang::CompileCommand> commands = load_commands();
        cg::test::check(r, !commands.empty(), "sample commands load");
        cg::clang::CompileCommand& degraded = commands.front();
        degraded.preordained_failure =
            "std-bmi-build-failed: probe - TUs importing std degrade to tier-1";
        degraded.scan_imports = {"fake.module", "fake.module:part"};
        cg::graph::Indexer indexer = make_indexer(store, /*force=*/false);
        const cg::graph::IndexSummary sum = indexer.run(commands);
        cg::test::check(r, sum.failed == 1, "the degraded TU counts as failed");
        bool class_counted = false;
        for (const auto& [name, count] : sum.failure_classes)
        {
            if (name == "std-bmi-build-failed" && count == 1)
            {
                class_counted = true;
            }
        }
        cg::test::check(
            r,
            class_counted,
            "the named class is aggregated in the summary");
        const std::string path = cg::util::norm_path(degraded.file);
        std::map<db::FileId, std::string> path_of;
        for (const db::FileRow& fr : db::all_files(store))
        {
            path_of.emplace(fr.id, fr.path);
        }
        std::size_t scan_rows = 0;
        for (const db::ImportLink& link : db::all_imports(store))
        {
            if (path_of[link.file] == path
                && (link.module_name == "fake.module"
                    || link.module_name == "fake.module:part")
                && link.line == 0)
            {
                ++scan_rows;
            }
        }
        cg::test::check(
            r,
            scan_rows == 2,
            "both scan requires land as imports rows at line 0");
    }

    // 两类引擎故障失败类（访问阶段深度防御
    // 网的具名类）走同一预定通道：失败 TU
    // 计数、按类聚合，以及部分失败契约——
    // 出错的 TU 只赔自己；健康文件的行在本次运行中幸存，
    // 出错 TU 手写的 import 仍从 tier-1 扫描落下。
    void engine_fault_classes(R& r)
    {
        db::GraphStore                         store{":memory:"};
        std::vector<cg::clang::CompileCommand> commands = load_commands();
        cg::test::check(r, commands.size() == 3, "sample commands load");
        if (commands.size() < 3)
        {
            return;
        }
        // 在 canvas.cpp 与 main.cpp 上注入两类失败；circle.cpp
        // （健康 TU）持有 shape::Circle::area——它的幸存就是
        // 部分失败保留检查。
        std::size_t injected = 0;
        for (cg::clang::CompileCommand& c : commands)
        {
            if (c.file.ends_with("canvas.cpp"))
            {
                c.preordained_failure =
                    "engine-visit-crash: the parse engine faulted while "
                    "walking the AST (probe) - the TU is skipped and the "
                    "index run continues";
                c.scan_imports = {"probe.visit"};
                ++injected;
            }
            else if (c.file.ends_with("main.cpp"))
            {
                c.preordained_failure =
                    "engine-no-tu: the parse engine reported a completed "
                    "parse but produced no translation unit (probe)";
                c.scan_imports = {"probe.no_tu"};
                ++injected;
            }
        }
        cg::test::check(r, injected == 2, "both fault classes injected");
        if (injected != 2)
        {
            return;
        }
        cg::graph::Indexer indexer = make_indexer(store, /*force=*/false);
        const cg::graph::IndexSummary sum = indexer.run(commands);
        cg::test::check(r, sum.failed == 2, "both faulted TUs count as failed");
        bool visit_counted = false;
        bool no_tu_counted = false;
        for (const auto& [name, count] : sum.failure_classes)
        {
            if (name == "engine-visit-crash" && count == 1)
            {
                visit_counted = true;
            }
            if (name == "engine-no-tu" && count == 1)
            {
                no_tu_counted = true;
            }
        }
        cg::test::check(
            r,
            visit_counted && no_tu_counted,
            "each engine-fault class aggregates separately");
        // 部分失败保留：健康 TU 的行在运行中幸存。
        cg::test::check(
            r,
            find_one(store, "shape::Circle::area", r, "Circle::area") != 0,
            "the healthy TU's symbols are retained");
        cg::test::check(
            r,
            sum.symbols > 10,
            "the run still indexes real content");
        // tier-1 打捞：每个出错 TU 的扫描 requires 都落在行 0。
        std::size_t scan_rows = 0;
        for (const db::ImportLink& link : db::all_imports(store))
        {
            if ((link.module_name == "probe.visit"
                 || link.module_name == "probe.no_tu")
                && link.line == 0)
            {
                ++scan_rows;
            }
        }
        cg::test::check(
            r,
            scan_rows == 2,
            "both faulted TUs' scan requires land at line 0");
    }

    void index_and_resolve(R& r)
    {
        db::GraphStore     store{":memory:"};
        cg::graph::Indexer indexer = make_indexer(store, /*force=*/false);
        const cg::graph::IndexSummary sum = indexer.run(load_commands());
        cg::test::check(r, sum.tus == 3, "three TUs indexed");
        cg::test::check(
            r,
            sum.fresh == 3 && sum.stale == 0 && sum.skipped == 0,
            "first run counts everything fresh");
        cg::test::check(r, sum.failed == 0, "no failed TUs");
        cg::test::check(r, sum.unresolved == 0, "every project edge resolved");
        cg::test::check(r, sum.symbols > 30, "symbol count sane");
        cg::test::check(
            r,
            sum.edges_by_type[static_cast<int>(db::EdgeType::Calls)] >= 10,
            "call edges recorded");
        cg::test::check(
            r,
            sum.edges_by_type[static_cast<int>(db::EdgeType::Extends)] == 1,
            "exactly one extends edge (Circle -> Shape)");
        cg::test::check(
            r,
            sum.edges_by_type[static_cast<int>(db::EdgeType::Overrides)] == 3,
            "three overrides edges (area, describe, ~Circle)");
        cg::test::check(
            r,
            sum.edges_by_type[static_cast<int>(db::EdgeType::Specializes)] == 1,
            "one specializes edge (Box<T*> -> Box)");
        cg::test::check(
            r,
            sum.edges_by_type[static_cast<int>(db::EdgeType::TypeAlias)] == 1,
            "one type_alias edge (ShapeHandle -> Shape)");
        cg::test::check(r, sum.includes == 7, "seven include rows recorded");
        const db::Stats st = db::stats(store);
        cg::test::check(
            r,
            st.includes_angled == 1,
            "exactly one angled include (<vector>)");

        // 零重复行，在 SQL 里检查（SCHEMA 的 UNIQUE 约束）。
        cg::test::check(
            r,
            db::duplicate_symbol_keys(store) == 0,
            "no duplicate symbol unique_keys");
        cg::test::check(
            r,
            db::duplicate_edge_sites(store) == 0,
            "no duplicate edge sites");
        cg::test::check(
            r,
            db::duplicate_includes(store) == 0,
            "no duplicate include rows");

        // Overrides 解析：Circle::area -> Shape::area，置信度 1.0。
        const cg::db::SymId circle_area =
            find_one(store, "shape::Circle::area", r, "Circle::area");
        const cg::db::SymId shape_area =
            find_one(store, "shape::Shape::area", r, "Shape::area");
        if (circle_area && shape_area)
        {
            const auto overs =
                db::edges_of(store, circle_area, true, db::EdgeType::Overrides);
            cg::test::check(
                r,
                overs.size() == 1,
                "one overrides edge from Circle::area");
            if (overs.size() == 1)
            {
                cg::test::check(
                    r,
                    overs[0].dst == shape_area,
                    "overrides resolves to Shape::area");
                cg::test::check(
                    r,
                    overs[0].confidence == 1.0,
                    "overrides edge at tier-1 confidence");
            }
        }

        // 跨文件 tier-1：Canvas::total_area -> Circle::area。
        const cg::db::SymId total_area =
            find_one(store, "render::Canvas::total_area", r, "total_area");
        if (total_area && circle_area)
        {
            const auto calls =
                db::edges_of(store, total_area, true, db::EdgeType::Calls);
            bool hit = false;
            for (const db::EdgeRow& e : calls)
            {
                if (e.dst == circle_area)
                {
                    hit = true;
                    cg::test::check(
                        r,
                        e.confidence == 1.0,
                        "cross-file call resolved at tier 1");
                }
            }
            cg::test::check(
                r,
                hit,
                "Canvas::total_area -> Circle::area stored");
        }

        // 重载对经 tier-1 解析仍可区分。
        const db::FindResult scales =
            db::find_symbols(store, "scale", db::SymbolFilter{});
        cg::test::check(
            r,
            scales.tier == 1 && scales.matches.size() == 2,
            "both scale overloads found");
        if (scales.matches.size() == 2)
        {
            cg::test::check(
                r,
                scales.matches[0].sig_hash != scales.matches[1].sig_hash,
                "overloads distinguished by sig_hash");
            // int 重载在 main 里以 int 字面量被调用。
            const cg::db::SymId main_id = find_one(store, "main", r, "main");
            bool                int_call = false, double_call = false;
            for (const db::EdgeRow& e :
                 db::edges_of(store, main_id, true, db::EdgeType::Calls))
            {
                for (const db::SymbolRow& m : scales.matches)
                {
                    if (e.dst == m.id && m.param_types
                        && m.param_types->find("int") != std::string::npos)
                    {
                        int_call = true;
                    }
                    else if (
                        e.dst == m.id && m.param_types
                        && m.param_types->find("double") != std::string::npos)
                    {
                        double_call = true;
                    }
                }
            }
            cg::test::check(
                r,
                int_call && double_call,
                "main calls both overloads, each to its own node");
        }

        // 声明/定义合并：每个类外符号恰好一个节点。
        cg::test::check(
            r,
            find_one(store, "shape::Circle::describe", r, "Circle::describe")
                    != 0
                && find_one(store, "shape::pi", r, "pi") != 0,
            "out-of-line definitions merged with declarations");

        // 模板策略：只留主模板符号，get 只有一个节点。
        const db::FindResult gets =
            db::find_symbols(store, "get", db::SymbolFilter{});
        cg::test::check(
            r,
            gets.matches.size() == 2,
            "two get() nodes (Box and Box<T*> patterns)");
        bool unwrap_call_from_get = false;
        if (const cg::db::SymId unwrap =
                find_one(store, "shape::unwrap", r, "unwrap"))
        {
            for (const db::EdgeRow& e :
                 db::edges_of(store, unwrap, false, db::EdgeType::Calls))
            {
                if (name_of(store, e.src).find("Box") != std::string::npos)
                {
                    unwrap_call_from_get = true;
                }
            }
        }
        cg::test::check(
            r,
            unwrap_call_from_get,
            "Box::get -> unwrap recorded against primary template");

        // extends 边携带访问属性（public = 0 位）。
        if (const cg::db::SymId circle =
                find_one(store, "shape::Circle", r, "Circle"))
        {
            const auto bases = db::bases(store, circle);
            cg::test::check(
                r,
                bases.size() == 1 && bases[0].attrs == 0,
                "extends edge present, public access");
        }
    }

    void incremental_replacement(R& r)
    {
        db::GraphStore store{":memory:"};
        const auto     cmds = load_commands();

        const cg::graph::IndexSummary first =
            make_indexer(store, false).run(cmds);
        const cg::db::SymId area =
            find_one(store, "shape::Circle::area", r, "Circle::area");
        const auto st1 = db::stats(store);

        // 第二次运行：全部跳过，无任何变化。
        const cg::graph::IndexSummary second =
            make_indexer(store, false).run(cmds);
        cg::test::check(
            r,
            second.skipped == 3 && second.fresh == 0 && second.stale == 0,
            "second run skips all fresh files");
        cg::test::check(
            r,
            second.symbols == first.symbols && second.edges == first.edges
                && second.includes == first.includes,
            "counts stable across skip run");
        cg::test::check(
            r,
            find_one(store, "shape::Circle::area", r, "area") == area,
            "sym ids stable across skip run");

        // 强制重解析：stale 路径，按文件清除 + 重 upsert，id 稳定。
        const cg::graph::IndexSummary third =
            make_indexer(store, true).run(cmds);
        cg::test::check(
            r,
            third.stale == 3 && third.skipped == 0,
            "forced run re-parses everything");
        cg::test::check(
            r,
            third.unresolved == 0,
            "forced run still resolves all");
        const auto st3 = db::stats(store);
        cg::test::check(
            r,
            st3.symbols == st1.symbols && st3.edges == st1.edges
                && st3.includes == st1.includes,
            "per-file purge + re-index keeps counts stable");
        cg::test::check(
            r,
            find_one(store, "shape::Circle::area", r, "area") == area,
            "sym ids stable across forced re-index");
        cg::test::check(
            r,
            db::duplicate_symbol_keys(store) == 0
                && db::duplicate_edge_sites(store) == 0
                && db::duplicate_includes(store) == 0,
            "no duplicates after re-index");

        // 出现次数折叠：extends 点（在 circle.hpp）被三个 TU
        // 都观察到，重跑后其计数器 >= 3。
        if (const cg::db::SymId circle =
                find_one(store, "shape::Circle", r, "Circle"))
        {
            const auto bases = db::bases(store, circle);
            cg::test::check(
                r,
                bases.size() == 1 && bases[0].occurrences >= 3,
                "site occurrences counter folds re-observations");
        }
    }

    void traverse_walks(R& r)
    {
        db::GraphStore store{":memory:"};
        make_indexer(store, false).run(load_commands());

        const cg::db::SymId area =
            find_one(store, "shape::Circle::area", r, "Circle::area");
        const cg::db::SymId main_id = find_one(store, "main", r, "main");
        const cg::db::SymId pi = find_one(store, "shape::pi", r, "pi");
        if (!area || !main_id || !pi)
        {
            return;
        }

        // 深度 5 的 callers 完整展开链（main 在深度 4；
        // 深度 4 的节点本身必须被展开，以证明后面
        // 再无节点，因此 truncated 只在最后一个节点再深一层才为假）。
        const cg::graph::NeighborsResult deep =
            cg::graph::callers(store, area, 5);
        cg::test::check(r, deep.entries.size() == 4, "four transitive callers");
        cg::test::check(r, !deep.truncated, "depth 5 covers the whole chain");
        cg::test::check(
            r,
            depth_in(
                deep,
                find_one(store, "render::Canvas::total_area", r, "total_area"))
                    == 1
                && depth_in(
                       deep,
                       find_one(store, "app::area_all", r, "area_all"))
                       == 2
                && depth_in(
                       deep,
                       find_one(store, "app::render_all", r, "render_all"))
                       == 3
                && depth_in(deep, main_id) == 4,
            "shortest-depth annotation per caller");

        // 深度 2 在还有节点未展开时停下：truncated。
        const cg::graph::NeighborsResult capped =
            cg::graph::callers(store, area, 2);
        cg::test::check(
            r,
            capped.entries.size() == 2 && capped.truncated,
            "depth cap truncates and reports it");

        // 在种下的二元环上验证环终止。
        {
            const cg::db::FileId f =
                store.upsert_file("e:/synthetic/cycle.cpp");
            const cg::db::SymId fn_a = store.upsert_symbol(
                {.unique_key = "function||cycle_a|e:/synthetic/cycle.cpp",
                 .kind = db::SymbolKind::Function,
                 .name = "cycle_a",
                 .ns = "",
                 .sig_hash = std::nullopt,
                 .file_id = f,
                 .line = 1,
                 .col = 1});
            const cg::db::SymId fn_b = store.upsert_symbol(
                {.unique_key = "function||cycle_b|e:/synthetic/cycle.cpp",
                 .kind = db::SymbolKind::Function,
                 .name = "cycle_b",
                 .ns = "",
                 .sig_hash = std::nullopt,
                 .file_id = f,
                 .line = 2,
                 .col = 1});
            store.upsert_edge(
                {.src = fn_a,
                 .dst = fn_b,
                 .type = db::EdgeType::Calls,
                 .site_file = f,
                 .site_line = 5});
            store.upsert_edge(
                {.src = fn_b,
                 .dst = fn_a,
                 .type = db::EdgeType::Calls,
                 .site_file = f,
                 .site_line = 9});
            const cg::graph::NeighborsResult cyc =
                cg::graph::callers(store, fn_a, 10);
            cg::test::check(
                r,
                cyc.entries.size() == 1 && cyc.entries[0].id == fn_b,
                "cycle walk terminates, no self revisits");
        }

        // 影响面阶梯。
        const cg::graph::ImpactResult imp =
            cg::graph::impact(store, area, cg::graph::Direction::Up, 3);
        cg::test::check(
            r,
            imp.direct == 1 && imp.impacted == 3,
            "impact counts direct and transitive (depth 3)");
        cg::test::check(
            r,
            imp.risk == cg::graph::Risk::Low,
            "1 direct / 4 impacted lands at LOW");
        const cg::db::SymId unused =
            find_one(store, "shape::unused_helper", r, "unused_helper");
        if (unused)
        {
            const cg::graph::ImpactResult unknown =
                cg::graph::impact(store, unused, cg::graph::Direction::Up, 3);
            cg::test::check(
                r,
                unknown.risk == cg::graph::Risk::Unknown,
                "uncalled function reports UNKNOWN risk");
        }

        // trace：main -> pi 是 5 跳链；反方向不可达。
        const cg::graph::TraceResult path =
            cg::graph::trace(store, main_id, pi);
        cg::test::check(r, path.found, "trace finds main -> pi");
        if (path.found)
        {
            cg::test::check(
                r,
                path.chain.size() == 6,
                "trace chain has 6 nodes (5 hops)");
            cg::test::check(
                r,
                path.chain.front() == main_id && path.chain.back() == pi,
                "trace endpoints correct");
            bool ordered = true;
            for (std::size_t i = 0; i + 1 < path.chain.size(); ++i)
            {
                bool step = false;
                for (const db::EdgeRow& e : db::edges_of(
                         store,
                         path.chain[i],
                         true,
                         db::EdgeType::Calls))
                {
                    if (e.dst == path.chain[i + 1])
                    {
                        step = true;
                    }
                }
                ordered = ordered && step;
            }
            cg::test::check(r, ordered, "every trace hop is a real calls edge");
        }
        const cg::graph::TraceResult none =
            cg::graph::trace(store, pi, main_id);
        cg::test::check(
            r,
            !none.found,
            "reverse trace reports unreachable cleanly");

        // 上下文桶装配。
        const cg::graph::ContextView ctx = cg::graph::context(store, area);
        cg::test::check(r, ctx.symbol.id == area, "context anchors the symbol");
        bool in_call = false, out_override = false, out_pi = false;
        for (const auto& e : ctx.incoming)
        {
            if (e.other.qualified_name == "render::Canvas::total_area"
                && (e.edge.type == db::EdgeType::Calls
                    || e.edge.type == db::EdgeType::VirtualDispatch))
            {
                // total_area 的 c.area() 点静态解析
                // （static_unique_override，1.0）：读侧在该点把普通
                // calls 边替换成 virtual_dispatch 证据边，
                // 所以入桶的边两种形态皆可。
                in_call = true;
            }
        }
        for (const auto& e : ctx.outgoing)
        {
            if (e.edge.type == db::EdgeType::Overrides
                && e.other.qualified_name == "shape::Shape::area")
            {
                out_override = true;
            }
            if (e.edge.type == db::EdgeType::Calls
                && e.other.qualified_name == "shape::pi")
            {
                out_pi = true;
            }
        }
        cg::test::check(
            r,
            in_call && out_override && out_pi,
            "context buckets incoming calls + outgoing overrides/pi");
    }

    void file_db_persistence(R& r)
    {
        namespace fs = std::filesystem;
        const auto        path = fs::path{"ccg-graph-sample1.db"};
        const std::string p{path.string()};
        std::error_code   ec;
        fs::remove(path, ec);

        cg::graph::IndexSummary sum;
        {
            db::GraphStore          store{p};
            cg::graph::IndexOptions opt;
            opt.roots = {root()};
            opt.progress = false;
            opt.db_path = p;
            sum =
                cg::graph::Indexer{store, std::move(opt)}.run(load_commands());
        }
        cg::test::check(
            r,
            sum.db_size_bytes > 0,
            "db size reported for file dbs");
        {
            db::GraphStore store{p};
            const auto     st = db::stats(store);
            cg::test::check(
                r,
                st.symbols == sum.symbols && st.edges == sum.edges,
                "persisted db matches the index summary");
            // 跨重开的增量：全部跳过。
            cg::graph::IndexOptions opt;
            opt.roots = {root()};
            opt.progress = false;
            const cg::graph::IndexSummary again =
                cg::graph::Indexer{store, std::move(opt)}.run(load_commands());
            cg::test::check(r, again.skipped == 3, "reopen + rerun skips all");
        }

        fs::remove(path, ec);
        fs::remove(path.string() + "-wal", ec);
        fs::remove(path.string() + "-shm", ec);
    }

    // 环境变量门控的库转储（调试辅助：CCG_DUMP=1）。
    void dump_db()
    {
        db::GraphStore store{":memory:"};
        make_indexer(store, false).run(load_commands());
        const auto st = db::stats(store);
        std::println(
            "STATS symbols={} edges={} calls={} extends={} overrides={} "
            "alias={} spec={} includes={} angled={}",
            st.symbols,
            st.edges,
            st.edges_by_type[0],
            st.edges_by_type[1],
            st.edges_by_type[2],
            st.edges_by_type[3],
            st.edges_by_type[5],
            st.includes,
            st.includes_angled);
        for (const cg::db::IndexRow& row : db::all_symbol_index_rows(store))
        {
            std::println(
                "SYM id={} kind={} name=[{}] ns=[{}] parent=[{}] sig={}",
                row.id,
                static_cast<int>(row.kind),
                row.name,
                row.ns,
                row.parent,
                row.sig_hash.has_value() ? std::to_string(*row.sig_hash)
                                         : std::string("-"));
        }
        // 全部边：迭代符号及其出边。
        for (const cg::db::IndexRow& row : db::all_symbol_index_rows(store))
        {
            for (int t = 0; t < db::edge_type_count; ++t)
            {
                for (const db::EdgeRow& e :
                     db::edges_of(store, row.id, true, db::edge_type_of(t)))
                {
                    std::println(
                        "EDGE {} -> {} type={} conf={} occ={} site_line={}",
                        name_of(store, e.src),
                        name_of(store, e.dst),
                        static_cast<int>(e.type),
                        e.confidence,
                        e.occurrences,
                        e.site_line);
                }
            }
        }
    }

    // 解析池不得改变存进库的东西：同一夹具的两次
    /// 以给定并发数索引 sample2 语料并断言零失败。
    [[nodiscard]]
    auto pooled_index_run(
        R&                                            r,
        cg::db::GraphStore&                           store,
        std::string_view                              dir,
        int                                           jobs,
        const std::vector<cg::clang::CompileCommand>& cmds)
        -> cg::graph::IndexSummary
    {
        cg::graph::IndexOptions opt;
        opt.roots = {cg::util::norm_path(dir)};
        opt.force = true;
        opt.progress = false;
        opt.jobs = jobs;
        cg::graph::Indexer            indexer{store, std::move(opt)};
        const cg::graph::IndexSummary sum = indexer.run(cmds);
        cg::test::check(
            r,
            sum.failed == 0,
            "sample2 indexes clean (no failed TUs)");
        return sum;
    }

    // 全量运行（池 4 与池 1）产出相同的 symbol/edge/
    // include 行——连 id 也相同——因为工人只做提取；
    // 存储阶段总是按 compile-db 顺序消费提取结果。
    void parallel_pool_determinism(R& r)
    {
        constexpr std::string_view sample2_dir = CCG_SAMPLE2_DIR;
        const auto                 cmds = cg::clang::load(
            std::string{sample2_dir} + "/compile_commands.json",
            {});
        cg::test::check(
            r,
            cmds.size() >= 4,
            "sample2 fixture has enough TUs to interleave workers");
        if (cmds.size() < 4)
        {
            return;
        }

        db::GraphStore a{":memory:"}, b{":memory:"}, serial{":memory:"};
        const cg::graph::IndexSummary pool_a =
            pooled_index_run(r, a, sample2_dir, 4, cmds);
        const cg::graph::IndexSummary pool_b =
            pooled_index_run(r, b, sample2_dir, 4, cmds);
        (void)pooled_index_run(r, serial, sample2_dir, 1, cmds);

        cg::test::check(
            r,
            pool_a.symbols == pool_b.symbols && pool_a.edges == pool_b.edges
                && pool_a.includes == pool_b.includes
                && pool_a.imports == pool_b.imports,
            "summary counts identical across two pooled runs");
        cg::test::check(
            r,
            db::dump_symbols(a) == db::dump_symbols(b)
                && db::dump_edges(a) == db::dump_edges(b)
                && db::dump_includes(a) == db::dump_includes(b)
                && db::dump_imports(a) == db::dump_imports(b),
            "two pooled runs produce identical rows");
        cg::test::check(
            r,
            db::dump_symbols(a) == db::dump_symbols(serial)
                && db::dump_edges(a) == db::dump_edges(serial)
                && db::dump_includes(a) == db::dump_includes(serial)
                && db::dump_imports(a) == db::dump_imports(serial),
            "pooled run identical to serial run");
    }

    // ---------------------------------------------------------------------------
    // USES 边 + record 级影响面（专用 uses 夹具）。

    [[nodiscard]]
    auto load_uses_commands() -> std::vector<cg::clang::CompileCommand>
    {
        constexpr std::string_view uses_dir = CCG_USES_DIR;
        return cg::clang::load(
            std::string{uses_dir} + "/compile_commands.json",
            {});
    }

    /// 影响面直接层是否含限定名子串命中的符号。
    [[nodiscard]]
    auto direct_layer_has(
        cg::db::GraphStore&            store,
        const cg::graph::ImpactResult& res,
        std::string_view               q) -> bool
    {
        if (res.by_depth.empty())
        {
            return false;
        }
        for (const cg::db::SymId id : res.by_depth.front().ids)
        {
            const auto row = db::get_symbol(store, id);
            if (row && row->qualified_name.find(q) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }

    /// 是否存在 from -> to 的 USES 边。
    [[nodiscard]]
    auto has_uses_edge_to(
        cg::db::GraphStore& store,
        cg::db::SymId       from,
        cg::db::SymId       to) -> bool
    {
        for (const db::EdgeRow& e :
             db::edges_of(store, from, true, db::EdgeType::Uses))
        {
            if (e.dst == to)
            {
                return true;
            }
        }
        return false;
    }

    /// 邻居结果里限定名子串命中的首条入口（无命中给空入口）。
    [[nodiscard]]
    auto find_neighbor(
        const cg::graph::NeighborsResult& res,
        std::string_view                  q,
        cg::db::GraphStore&               s) -> cg::graph::NeighborEntry
    {
        for (const auto& e : res.entries)
        {
            const auto row = db::get_symbol(s, e.id);
            if (row && row->qualified_name.find(q) != std::string::npos)
            {
                return e;
            }
        }
        return cg::graph::NeighborEntry{};
    }

    /**
     * @brief uses 夹具的索引器（roots 与 compile-db 指向专用夹具目录）。
     */
    [[nodiscard]]
    auto make_uses_indexer(cg::db::GraphStore& store) -> cg::graph::Indexer
    {
        constexpr std::string_view uses_dir = CCG_USES_DIR;
        cg::graph::IndexOptions    opt;
        opt.roots = {cg::util::norm_path(uses_dir)};
        opt.progress = false;
        opt.compile_db_path = std::string{uses_dir} + "/compile_commands.json";
        return cg::graph::Indexer{store, std::move(opt)};
    }

    void uses_record_impact(R& r)
    {
        db::GraphStore                store{":memory:"};
        const cg::graph::IndexSummary sum =
            make_uses_indexer(store).run(load_uses_commands());
        cg::test::check(
            r,
            sum.failed == 0 && sum.unresolved == 0,
            "uses fixture indexes clean");
        cg::test::check(
            r,
            sum.edges_by_type[static_cast<int>(db::EdgeType::Uses)] >= 15,
            "USES edges recorded");

        // 依赖成员指针回归（fixtures/uses/dependent_mp.hpp）：
        // 未实例化模板模式的 'void (U::*)(void)' 形参
        // 是会让某些引擎构建在类类型查询里
        // 出错的输入类。查询跑在崩溃守卫后，所以
        // 整个 TU 的访问必须幸存——本检查就是回归
        // 栅栏：能走到这里就说明索引器进程活着穿过了
        // 依赖遍历——且模式自身的符号必须在库里
        // （被守卫吞掉的故障至多赔掉模式的
        // 持有 record 边，绝不赔 TU）。
        cg::test::check(
            r,
            find_one(
                store,
                "uses::probe_mp",
                r,
                "dependent member-pointer template")
                != 0,
            "dependent member-pointer pattern survives the walk");
        cg::test::check(
            r,
            find_one(
                store,
                "uses::dep_mp_witness",
                r,
                "dependent member-pointer witness")
                != 0,
            "dependent member-pointer witness indexed");

        // record 级影响面是真的：从 Payload 向上列出引用
        // 它的字段与函数（v1 的 record 只走 extends）。
        const cg::db::SymId payload =
            find_one(store, "uses::Payload", r, "Payload");
        if (!payload)
        {
            return;
        }
        const cg::graph::ImpactResult up =
            cg::graph::impact(store, payload, cg::graph::Direction::Up, 2);
        cg::test::check(
            r,
            up.direct >= 6,
            "record up-impact has direct USES hits");
        cg::test::check(
            r,
            direct_layer_has(store, up, "Carrier::by_class")
                && direct_layer_has(store, up, "Carrier::by_template")
                && direct_layer_has(store, up, "Carrier::by_std"),
            "USES-referencing fields impacted (field owns the edge)");
        cg::test::check(
            r,
            direct_layer_has(store, up, "uses::echo")
                && direct_layer_has(store, up, "uses::echo_tpl")
                && direct_layer_has(store, up, "uses::echo_std")
                && direct_layer_has(store, up, "main"),
            "USES-referencing params/returns/locals impacted");
        // 成员指针的类类型与函数类型的模板实参
        // （回调）也引用 record：声明类型遍历
        // 不得在 CXType_MemberPointer 或函数类型处终止。
        cg::test::check(
            r,
            direct_layer_has(store, up, "Carrier::by_member_ptr")
                && direct_layer_has(store, up, "uses::probe_member")
                && direct_layer_has(store, up, "uses::make_cb")
                && direct_layer_has(store, up, "uses::run_cb")
                && direct_layer_has(store, up, "uses::slot_ref"),
            "member-pointer and callback USES surface in record impact");
        cg::test::check(
            r,
            up.risk != cg::graph::Risk::Unknown,
            "used record gets a real risk band");
        cg::test::check(
            r,
            up.fan_in >= 1
                && up.risk_score == 3 * up.direct + up.transitive + up.fan_in,
            "risk score formula holds on fixture data");

        // 无人引用的 record 仍保有 v1 形态：UNKNOWN，而非
        // 捏造的档位（没有边不等于安全）。
        if (const cg::db::SymId idle =
                find_one(store, "uses::NothingReferences", r, "idle record"))
        {
            const cg::graph::ImpactResult before =
                cg::graph::impact(store, idle, cg::graph::Direction::Up, 3);
            cg::test::check(
                r,
                before.direct == 0 && before.risk == cg::graph::Risk::Unknown,
                "unreferenced record stays UNKNOWN");
        }

        // record 的向下影响面：基类 + 被用 record（经 USES 边）。
        const cg::db::SymId carrier =
            find_one(store, "uses::Carrier", r, "Carrier");
        if (carrier)
        {
            const cg::graph::ImpactResult down = cg::graph::impact(
                store,
                carrier,
                cg::graph::Direction::Down,
                1);
            bool payload_seen = false, holder_seen = false;
            for (const auto& level : down.by_depth)
            {
                for (const cg::db::SymId id : level.ids)
                {
                    const auto row = db::get_symbol(store, id);
                    if (row && row->qualified_name == "uses::Payload")
                    {
                        payload_seen = true;
                    }
                    if (row && row->qualified_name == "uses::Holder")
                    {
                        holder_seen = true;
                    }
                }
            }
            cg::test::check(
                r,
                payload_seen && holder_seen,
                "record down-impact includes used records");
        }

        // 点位折叠：echo 的形参与返回在同一签名行
        // 引用 Payload——一条存储边 occurrences 2；局部变量各占
        // 自己（更晚的）点位。
        const cg::db::SymId echo = find_one(store, "uses::echo", r, "echo");
        if (echo && payload)
        {
            std::int64_t folded_occ = 0;
            int          folded_edges = 0;
            int          total_to_payload = 0;
            int          min_site = std::numeric_limits<int>::max();
            const std::vector<db::EdgeRow> uses_rows =
                db::edges_of(store, echo, true, db::EdgeType::Uses);
            for (const db::EdgeRow& e : uses_rows)
            {
                if (e.dst != payload)
                {
                    continue;
                }
                ++total_to_payload;
                min_site = std::min(min_site, e.site_line);
            }
            for (const db::EdgeRow& e : uses_rows)
            {
                if (e.dst == payload && e.site_line == min_site)
                {
                    ++folded_edges;
                    folded_occ = e.occurrences;
                }
            }
            cg::test::check(
                r,
                folded_edges == 1 && folded_occ == 2,
                "param+return at one line fold into one site, occ 2");
            cg::test::check(
                r,
                total_to_payload == 4,
                "echo -> Payload: signature fold + three local sites");
        }

        // v1 遍历丢掉的两个形态，在边层面断言（不只是
        // 经影响面）：成员指针形参与函数类型
        // 模板实参各自铸出一条到 record 的 USES 边。
        const cg::db::SymId probe =
            find_one(store, "uses::probe_member", r, "probe_member");
        const cg::db::SymId run_cb =
            find_one(store, "uses::run_cb", r, "run_cb");
        if (probe && payload)
        {
            cg::test::check(
                r,
                has_uses_edge_to(store, probe, payload),
                "member-pointer parameter mints a USES edge");
        }
        if (run_cb && payload)
        {
            cg::test::check(
                r,
                has_uses_edge_to(store, run_cb, payload),
                "callback function-type argument mints a USES edge");
        }
    }

    void family_merge(R& r)
    {
        db::GraphStore store{":memory:"};
        make_uses_indexer(store).run(load_uses_commands());

        const cg::db::SymId base =
            find_one(store, "uses::A::step", r, "A::step");
        const cg::db::SymId mid =
            find_one(store, "uses::B::step", r, "B::step");
        const cg::db::SymId derived =
            find_one(store, "uses::C::step", r, "C::step");
        if (!base || !mid || !derived)
        {
            return;
        }

        // 基类合并后的 callers 含两个重写的
        // 派发点调用者，并标注其来源成员。
        const cg::graph::NeighborsResult base_merged =
            cg::graph::callers(store, base, 1);
        const auto base_call_base =
            find_neighbor(base_merged, "uses::call_base", store);
        const auto base_mid =
            find_neighbor(base_merged, "uses::call_mid", store);
        const auto base_derived =
            find_neighbor(base_merged, "uses::call_derived", store);
        // 精确模式把这些调用归到已证明的派发目标，
        // 调用者因此是推断出来的（家族来源记录了该目标）。
        cg::test::check(
            r,
            base_call_base.id != 0
                && base_call_base.dispatch_status
                       == cg::graph::DispatchStatus::Inferred
                && base_call_base.dispatch_confidence == 0.7,
            "dispatch-proven caller of the base is inferred");
        cg::test::check(
            r,
            base_mid.id != 0
                && base_mid.dispatch_status
                       == cg::graph::DispatchStatus::Inferred
                && base_derived.id != 0
                && base_derived.dispatch_status
                       == cg::graph::DispatchStatus::Inferred,
            "override dispatch callers merge in as inferred");
        cg::test::check(
            r,
            base_merged.family.size() == 3,
            "family view spans A->B->C");

        // 定向 impl_set：查具体重写时排除已证明
        // 派发到祖先/兄弟的调用者，保留已证明
        // 派发到本重写的。
        const cg::graph::NeighborsResult derived_merged =
            cg::graph::callers(store, derived, 1);
        const auto derived_base =
            find_neighbor(derived_merged, "uses::call_base", store);
        const auto derived_mid =
            find_neighbor(derived_merged, "uses::call_mid", store);
        cg::test::check(
            r,
            derived_base.id == 0,
            "sibling/ancestor dispatch caller is not pulled in");
        cg::test::check(
            r,
            derived_mid.id != 0
                && derived_mid.dispatch_status
                       == cg::graph::DispatchStatus::Inferred,
            "caller proven to dispatch to this override stays");

        // --no-merge / merge_family=false 恢复单符号视图。
        const cg::graph::NeighborsResult base_solo =
            cg::graph::callers(store, base, 1, /*merge_family=*/false);
        bool solo_has_mid = false;
        for (const auto& e : base_solo.entries)
        {
            const auto row = db::get_symbol(store, e.id);
            if (row
                && (row->qualified_name.find("call_mid") != std::string::npos
                    || row->qualified_name.find("call_derived")
                           != std::string::npos))
            {
                solo_has_mid = true;
            }
        }
        cg::test::check(
            r,
            !solo_has_mid && base_solo.family.size() == 1,
            "--no-merge view has no override callers");

        // 合并影响面以整个家族作种子：直接层 = 任一成员的调用者。
        const cg::graph::ImpactResult imp =
            cg::graph::impact(store, base, cg::graph::Direction::Up, 2);
        bool sees_derived_caller = false;
        if (!imp.by_depth.empty())
        {
            for (const cg::db::SymId id : imp.by_depth.front().ids)
            {
                const auto row = db::get_symbol(store, id);
                if (row
                    && row->qualified_name.find("call_derived")
                           != std::string::npos)
                {
                    sees_derived_caller = true;
                }
            }
        }
        cg::test::check(
            r,
            sees_derived_caller,
            "merged impact counts dispatch callers as direct");

        // 上下文合并家族的边并带来源标记。
        const cg::graph::ContextView ctx = cg::graph::context(store, mid);
        bool                         marked_incoming = false;
        for (const auto& e : ctx.incoming)
        {
            if (e.family_origin != 0)
            {
                marked_incoming = true;
            }
        }
        cg::test::check(
            r,
            ctx.family.size() == 3 && marked_incoming,
            "context merges family edges with origin markers");
    }

    [[nodiscard]]
    auto twin_rows_in(
        cg::db::GraphStore& store,
        std::string_view    name,
        std::string_view    tail)
        -> std::pair<int, std::pair<cg::db::SymId, cg::db::SymId>>;

    [[nodiscard]]
    auto call_targets_of(cg::db::GraphStore& store, cg::db::SymId from)
        -> std::vector<cg::db::SymId>;

    // ---------------------------------------------------------------------------
    // 内部链接的每 TU 作用域（外场反馈）：静态与
    // 匿名命名空间孪生——同名、同签名、每 TU
    // 一个定义——必须在自己的 TU 内解析。修复前按名级联
    // 把 TU A 的调用点接到 TU B 的定义上，铸出幻影
    // 跨模块依赖边（SampleStore -> SampleScheduler
    // 伪造反向边）。共享头的外部对照必须保持全局
    // 可解析：作用域绝不越界殃及单定义身份。

    [[nodiscard]]
    auto load_ilink_commands() -> std::vector<cg::clang::CompileCommand>
    {
        constexpr std::string_view ilink_dir = CCG_ILINK_DIR;
        return cg::clang::load(
            std::string{ilink_dir} + "/compile_commands.json",
            {});
    }

    void internal_linkage_twins(R& r)
    {
        constexpr std::string_view ilink_dir = CCG_ILINK_DIR;
        db::GraphStore             store{":memory:"};
        cg::graph::IndexOptions    opt;
        opt.roots = {cg::util::norm_path(ilink_dir)};
        opt.progress = false;
        opt.compile_db_path = std::string{ilink_dir} + "/compile_commands.json";
        const cg::graph::IndexSummary sum =
            cg::graph::Indexer{store, std::move(opt)}.run(
                load_ilink_commands());
        cg::test::check(
            r,
            sum.failed == 0,
            "ilinkage fixture indexes clean (no failed TUs)");
        cg::test::check(
            r,
            sum.unresolved == 0,
            "every ilinkage edge resolves - no over-restriction");

        // 每 TU 孪生行存在：seed_value / dampen / clone_leaf 各两行，
        // 每个定义 .cpp 一行。
        const auto [seed_rows, seed_ids] =
            twin_rows_in(store, "seed_value", "a.cpp");
        const auto [damp_rows, damp_ids] =
            twin_rows_in(store, "dampen", "a.cpp");
        const auto [leaf_rows, leaf_ids] =
            twin_rows_in(store, "clone_leaf", "a.cpp");
        cg::test::check(
            r,
            seed_rows == 2 && damp_rows == 2 && leaf_rows == 2,
            "each twin has one definition row per TU");
        if (seed_rows != 2 || damp_rows != 2 || leaf_rows != 2
            || !seed_ids.first || !seed_ids.second || !damp_ids.first
            || !damp_ids.second || !leaf_ids.first || !leaf_ids.second)
        {
            return;
        }

        const cg::db::SymId a_entry = find_one(store, "a_entry", r, "a_entry");
        const cg::db::SymId b_entry = find_one(store, "b_entry", r, "b_entry");
        if (!a_entry || !b_entry)
        {
            return;
        }

        // 跨 TU 接线必须不存在：两个入口的调用都不得落在
        // 另一 TU 的孪生上，三种孪生形态（自由静态、匿名
        // 命名空间、静态模板——第三种形态来自外场反馈案例）全如此。
        const std::vector<cg::db::SymId> a_out =
            call_targets_of(store, a_entry);
        const std::vector<cg::db::SymId> b_out =
            call_targets_of(store, b_entry);
        cg::test::check(
            r,
            !std::ranges::contains(a_out, seed_ids.second)
                && !std::ranges::contains(a_out, damp_ids.second)
                && !std::ranges::contains(a_out, leaf_ids.second),
            "a_entry never wires onto b.cpp's twins (no phantom edge)");
        cg::test::check(
            r,
            !std::ranges::contains(b_out, seed_ids.first)
                && !std::ranges::contains(b_out, damp_ids.first)
                && !std::ranges::contains(b_out, leaf_ids.first),
            "b_entry never wires onto a.cpp's twins");

        // 同文件解析保持有效：每个入口解析自己 TU 的
        // 孪生（这是修复不是丢弃——旧行为把它们当
        // 歧义丢弃或跨 TU 乱接）。
        cg::test::check(
            r,
            std::ranges::contains(a_out, seed_ids.first)
                && std::ranges::contains(a_out, damp_ids.first)
                && std::ranges::contains(a_out, leaf_ids.first),
            "a_entry resolves its own TU's twins");
        cg::test::check(
            r,
            std::ranges::contains(b_out, seed_ids.second)
                && std::ranges::contains(b_out, damp_ids.second)
                && std::ranges::contains(b_out, leaf_ids.second),
            "b_entry resolves its own TU's twins");

        // 孪生内的同文件调用者：seed_value@A 调 dampen@A（静态
        // 函数调进本文件的匿名命名空间）。
        const std::vector<cg::db::SymId> seed_a_out =
            call_targets_of(store, seed_ids.first);
        cg::test::check(
            r,
            std::ranges::contains(seed_a_out, damp_ids.first),
            "static-to-anonymous-ns call within one file resolves");

        // 外部对照：共享头文件的 inline 函数保持一行且
        // 从两个 TU 都可达——每 TU 作用域不得越界。
        const auto [scale_rows, scale_ids] =
            twin_rows_in(store, "shared_scale", "shared.hpp");
        cg::test::check(
            r,
            scale_rows == 1 && scale_ids.first != 0,
            "shared-header external function is a single row");
        if (scale_rows == 1)
        {
            cg::test::check(
                r,
                std::ranges::contains(a_out, scale_ids.first)
                    && std::ranges::contains(b_out, scale_ids.first),
                "both TUs keep resolving the shared external function");
        }
    }

    /// 同名孪生行的行数与两个 TU 侧符号号（无孪生时对应位为 0）。
    [[nodiscard]]
    auto twin_rows_in(
        cg::db::GraphStore& store,
        std::string_view    name,
        std::string_view    tail)
        -> std::pair<int, std::pair<cg::db::SymId, cg::db::SymId>>
    {
        int           rows = 0;
        cg::db::SymId in_a = 0;
        cg::db::SymId in_b = 0;
        for (const cg::db::IndexRow& row : db::all_symbol_index_rows(store))
        {
            if (row.name != name)
            {
                continue;
            }
            ++rows;
            const auto full = db::get_symbol(store, row.id);
            if (full && full->file.ends_with(tail))
            {
                in_a = row.id;
            }
            else if (full)
            {
                in_b = row.id;
            }
        }
        return std::pair{rows, std::pair{in_a, in_b}};
    }

    /// from 的全部 Calls 边目标。
    [[nodiscard]]
    auto call_targets_of(cg::db::GraphStore& store, cg::db::SymId from)
        -> std::vector<cg::db::SymId>
    {
        std::vector<cg::db::SymId> dsts;
        for (const db::EdgeRow& e :
             db::edges_of(store, from, true, db::EdgeType::Calls))
        {
            dsts.push_back(e.dst);
        }
        return dsts;
    }

    /// 合成语料的函数符号行（唯一键内嵌合成文件名）。
    [[nodiscard]]
    auto put_fn_symbol(
        db::GraphStore&      store,
        const cg::db::FileId f,
        const std::string&   file_stem,
        const std::string&   name,
        int                  line) -> cg::db::SymId
    {
        return store.upsert_symbol(
            {.unique_key = std::string{"function||"} + name + "|" + file_stem,
             .kind = db::SymbolKind::Function,
             .name = name,
             .ns = "",
             .sig_hash = std::nullopt,
             .file_id = f,
             .line = line,
             .col = 1});
    }

    /// 合成语料的 Calls 边。
    void put_call_edge(
        db::GraphStore&      store,
        const cg::db::FileId f,
        cg::db::SymId        from,
        cg::db::SymId        to,
        int                  line)
    {
        store.upsert_edge(
            {.src = from,
             .dst = to,
             .type = db::EdgeType::Calls,
             .site_file = f,
             .site_line = line});
    }

    // 合成边形态上的确定性风险档位：分项
    // （direct/transitive/fan-in）与全部四档，算两遍。
    void risk_bands(R& r)
    {
        db::GraphStore       store{":memory:"};
        const cg::db::FileId f = store.upsert_file("e:/synthetic/risk.cpp");
        const std::string    stem = "e:/synthetic/risk.cpp";

        // 一个直接叶子调用者：得分 3 -> LOW。
        const cg::db::SymId low_t =
            put_fn_symbol(store, f, stem, "low_target", 1);
        put_call_edge(
            store,
            f,
            put_fn_symbol(store, f, stem, "low_leaf", 2),
            low_t,
            20);

        // 三个直接叶子调用者：得分 9 -> MEDIUM。
        const cg::db::SymId med_t =
            put_fn_symbol(store, f, stem, "med_target", 10);
        for (int i = 0; i < 3; ++i)
        {
            put_call_edge(
                store,
                f,
                put_fn_symbol(
                    store,
                    f,
                    stem,
                    "med_leaf" + std::to_string(i),
                    11 + i),
                med_t,
                30 + i);
        }

        // 枢纽形态：目标 <- 枢纽 <- 三片叶子。direct = 1（枢纽），
        // transitive = 3（深度 2 的叶子），枢纽的 fan-in = 引用
        // 它的 3 片叶子：得分 3*1 + 3 + 3 = 9——三个分项
        // 全非零。
        const cg::db::SymId hub_t =
            put_fn_symbol(store, f, stem, "hub_target", 40);
        const cg::db::SymId hub = put_fn_symbol(store, f, stem, "hub", 41);
        put_call_edge(store, f, hub, hub_t, 50);
        for (int i = 0; i < 3; ++i)
        {
            put_call_edge(
                store,
                f,
                put_fn_symbol(
                    store,
                    f,
                    stem,
                    "hub_leaf" + std::to_string(i),
                    42 + i),
                hub,
                60 + i);
        }

        // 十一个直接叶子调用者：得分 33 -> HIGH。
        const cg::db::SymId high_t =
            put_fn_symbol(store, f, stem, "high_target", 70);
        for (int i = 0; i < 11; ++i)
        {
            put_call_edge(
                store,
                f,
                put_fn_symbol(
                    store,
                    f,
                    stem,
                    "high_leaf" + std::to_string(i),
                    71 + i),
                high_t,
                80 + i);
        }

        // 四十一个直接叶子调用者：得分 123 -> CRITICAL。
        const cg::db::SymId crit_t =
            put_fn_symbol(store, f, stem, "crit_target", 100);
        for (int i = 0; i < 41; ++i)
        {
            put_call_edge(
                store,
                f,
                put_fn_symbol(
                    store,
                    f,
                    stem,
                    "crit_leaf" + std::to_string(i),
                    101 + i),
                crit_t,
                120 + i);
        }

        // 无人调用：UNKNOWN（向上方向）。
        const cg::db::SymId idle_t =
            put_fn_symbol(store, f, stem, "idle_target", 150);

        struct Shape
        {
            cg::db::SymId   id;
            std::int64_t    direct;
            std::int64_t    transitive;
            std::int64_t    fan_in;
            std::int64_t    score;
            cg::graph::Risk risk;
        };
        const std::vector<Shape> shapes = {
            {.id = low_t,
             .direct = 1,
             .transitive = 0,
             .fan_in = 0,
             .score = 3,
             .risk = cg::graph::Risk::Low},
            {.id = med_t,
             .direct = 3,
             .transitive = 0,
             .fan_in = 0,
             .score = 9,
             .risk = cg::graph::Risk::Medium},
            {.id = hub_t,
             .direct = 1,
             .transitive = 3,
             .fan_in = 3,
             .score = 9,
             .risk = cg::graph::Risk::Medium},
            {.id = high_t,
             .direct = 11,
             .transitive = 0,
             .fan_in = 0,
             .score = 33,
             .risk = cg::graph::Risk::High},
            {.id = crit_t,
             .direct = 41,
             .transitive = 0,
             .fan_in = 0,
             .score = 123,
             .risk = cg::graph::Risk::Critical},
            {.id = idle_t,
             .direct = 0,
             .transitive = 0,
             .fan_in = 0,
             .score = 0,
             .risk = cg::graph::Risk::Unknown},
        };
        for (const Shape& s : shapes)
        {
            const cg::graph::ImpactResult a =
                cg::graph::impact(store, s.id, cg::graph::Direction::Up, 3);
            const cg::graph::ImpactResult b =
                cg::graph::impact(store, s.id, cg::graph::Direction::Up, 3);
            cg::test::check(
                r,
                a.direct == s.direct && a.transitive == s.transitive
                    && a.fan_in == s.fan_in && a.risk_score == s.score,
                std::string{"score components for "} + name_of(store, s.id));
            cg::test::check(
                r,
                a.risk == s.risk && a.risk == b.risk
                    && a.risk_score == b.risk_score,
                std::string{"band deterministic for "} + name_of(store, s.id));
        }
    }

    /// 路径以后缀定位的文件行（未命中给空行）。
    [[nodiscard]]
    auto file_row_with_suffix(cg::db::GraphStore& store, std::string_view tail)
        -> db::FileRow
    {
        for (const db::FileRow& fr : db::all_files(store))
        {
            if (fr.path.ends_with(tail))
            {
                return fr;
            }
        }
        return {};
    }

    /// changes 结果里限定名子串命中的消费者（无命中给空）。
    [[nodiscard]]
    auto find_consumer(
        const cg::graph::ChangesResult& res,
        cg::db::GraphStore&             store,
        std::string_view                q) -> const cg::graph::ChangesEntry*
    {
        for (const cg::graph::ChangesEntry& e : res.consumers)
        {
            if (name_of(store, e.id).find(q) != std::string::npos)
            {
                return &e;
            }
        }
        return nullptr;
    }

    // sample1 图上的 ccg changes：文件->符号映射、经
    // calls/extends/uses 加重写家族的向上传播、种子绝不
    // 列为消费者、深度封顶截断旗标、确定性摘要。
    void changes_walk(R& r)
    {
        db::GraphStore     store{":memory:"};
        cg::graph::Indexer indexer = make_indexer(store, /*force=*/false);
        (void)indexer.run(load_commands());

        // circle.hpp 映射到它定义的每个符号（record、typedef）。
        const db::FileRow circle_h = file_row_with_suffix(store, "circle.hpp");
        cg::test::check(
            r,
            !circle_h.path.empty(),
            "circle.hpp has a files row");
        const std::vector<db::SymbolRow> syms =
            db::symbols_of_file(store, circle_h.id);
        bool has_circle = false;
        bool has_typedef = false;
        for (const db::SymbolRow& s : syms)
        {
            has_circle = has_circle || s.qualified_name == "shape::Circle";
            has_typedef =
                has_typedef || s.qualified_name == "shape::ShapeHandle";
        }
        cg::test::check(
            r,
            has_circle && has_typedef && syms.size() == 3,
            "symbols_of_file maps circle.hpp to its 3 symbols");

        std::vector<cg::db::SymId> seeds;
        seeds.reserve(syms.size());
        for (const db::SymbolRow& s : syms)
        {
            seeds.push_back(s.id);
        }
        const cg::graph::ChangesResult res =
            cg::graph::changes(store, seeds, /*max_depth=*/3);

        // USES 消费者是直接层（引用 Circle 的 Canvas 字段/形参）。
        const cg::graph::ChangesEntry* field =
            find_consumer(res, store, "render::Canvas::circles_");
        cg::test::check(
            r,
            field != nullptr && field->depth == 1,
            "uses-referencing field is a direct consumer");
        cg::test::check(
            r,
            field != nullptr && name_of(store, field->seed) == "shape::Circle",
            "entries carry the changed symbol their chain came from");
        cg::test::check(
            r,
            find_consumer(res, store, "app::area_all") != nullptr
                && find_consumer(res, store, "app::area_all")->depth == 2,
            "transitive caller reached at depth 2");
        cg::test::check(
            r,
            find_consumer(res, store, "app::render_all") != nullptr
                && find_consumer(res, store, "app::render_all")->depth == 3,
            "call chain reaches depth 3");
        cg::test::check(
            r,
            res.consumers.size() == 6 && res.direct == 4 && res.transitive == 2,
            "summary counts (6 consumers, 4 direct)");
        cg::test::check(
            r,
            res.risk == cg::graph::Risk::Medium && res.risk_score == 16,
            "summary risk band and score");
        cg::test::check(
            r,
            res.truncated,
            "depth cap stops a live frontier (truncated flag)");
        bool seed_listed = false;
        for (const cg::graph::ChangesEntry& e : res.consumers)
        {
            seed_listed =
                seed_listed
                || std::ranges::find(res.seeds, e.id) != res.seeds.end();
        }
        cg::test::check(r, !seed_listed, "seeds never appear as consumers");

        // 深度 1 只留直接层，计数随之缩小。
        const cg::graph::ChangesResult direct_only =
            cg::graph::changes(store, seeds, /*max_depth=*/1);
        cg::test::check(
            r,
            direct_only.consumers.size() == 4 && direct_only.transitive == 0,
            "--depth 1 lists direct consumers only");

        // shape.hpp 种子把重写家族作为直接消费者浮出
        // （改虚函数契约会波及其重写）。
        const db::FileRow shape_h = file_row_with_suffix(store, "shape.hpp");
        std::vector<cg::db::SymId> shape_seeds;
        for (const db::SymbolRow& s : db::symbols_of_file(store, shape_h.id))
        {
            shape_seeds.push_back(s.id);
        }
        const cg::graph::ChangesResult fam =
            cg::graph::changes(store, shape_seeds, /*max_depth=*/1);
        const cg::graph::ChangesEntry* override_hit =
            [&]() -> const cg::graph::ChangesEntry*
        {
            for (const cg::graph::ChangesEntry& e : fam.consumers)
            {
                if (name_of(store, e.id) == "shape::Circle::area")
                {
                    return &e;
                }
            }
            return nullptr;
        }();
        cg::test::check(
            r,
            override_hit != nullptr && override_hit->depth == 1,
            "override family members are direct consumers");
    }

    /// 合成包含语料的文件行（合成目录前缀 + 名字）。
    [[nodiscard]]
    auto put_syn_file(
        db::GraphStore&    store,
        const std::string& dir,
        const std::string& name) -> cg::db::FileId
    {
        return store.upsert_file(dir + name);
    }

    /// 合成包含语料的 include 行。
    void put_include_row(
        db::GraphStore&    store,
        cg::db::FileId     from,
        const std::string& target,
        int                line,
        bool               angled)
    {
        store.upsert_include(
            {.file = from, .target = target, .line = line, .angled = angled});
    }

    // 合成 includes 表上的包含图环检测（无需解析）：
    // 双文件环、自包含、菱形负例、孤立
    // 文件、未解析目标计数、上限 + 总计的诚实性。
    void include_cycles_scc(R& r)
    {
        db::GraphStore       store{":memory:"};
        const cg::db::FileId x1 =
            put_syn_file(store, "e:/synthetic/cyc/", "x1.hpp");
        const cg::db::FileId x2 =
            put_syn_file(store, "e:/synthetic/cyc/", "x2.hpp");
        const cg::db::FileId d1 =
            put_syn_file(store, "e:/synthetic/cyc/", "d1.hpp");
        const cg::db::FileId d2 =
            put_syn_file(store, "e:/synthetic/cyc/", "d2.hpp");
        const cg::db::FileId d3 =
            put_syn_file(store, "e:/synthetic/cyc/", "d3.hpp");
        // diamond bottom: linked, never an edge endpoint here
        (void)put_syn_file(store, "e:/synthetic/cyc/", "d4.hpp");
        const cg::db::FileId s1 =
            put_syn_file(store, "e:/synthetic/cyc/", "s1.hpp");
        // counted by files_scanned, links to nothing
        (void)put_syn_file(store, "e:/synthetic/cyc/", "isolated.hpp");

        // x1 <-> x2（真环），菱形 d1 -> {d2, d3} -> d4（无环），
        // s1 -> s1（自包含），x1 上一个不可解析目标。
        put_include_row(store, x1, "x2.hpp", 1, false);
        put_include_row(store, x2, "x1.hpp", 1, false);
        put_include_row(store, d1, "d2.hpp", 1, false);
        put_include_row(store, d1, "d3.hpp", 2, false);
        put_include_row(store, d2, "d4.hpp", 1, false);
        put_include_row(store, d3, "d4.hpp", 1, false);
        put_include_row(store, s1, "s1.hpp", 1, false);
        put_include_row(store, x1, "nowhere/missing.hpp", 9, false);

        const cg::graph::CyclesResult all =
            cg::graph::include_cycles(store, 10);
        cg::test::check(
            r,
            all.groups_total == 2,
            "two cyclic clusters (pair + self-include)");
        cg::test::check(
            r,
            all.files_total == 3 && all.edges_total == 3,
            "totals: 3 files, 3 edges across all groups");
        cg::test::check(r, all.files_scanned == 8, "every file row scanned");
        cg::test::check(
            r,
            all.unresolved_links == 1,
            "unresolved include target counted");
        cg::test::check(
            r,
            all.groups.size() == 2 && !all.truncated,
            "cap 10 lists both groups");
        if (all.groups.size() == 2)
        {
            // 确定性顺序：成员路径最小者先（s1 < x1）。
            const cg::graph::CycleGroup& first = all.groups.front();
            const cg::graph::CycleGroup& second = all.groups.back();
            cg::test::check(
                r,
                first.files.size() == 1 && first.files.front() == s1
                    && first.edges.size() == 1
                    && first.edges.front().first == s1
                    && first.edges.front().second == s1,
                "self-include is a one-file group with a self edge");
            cg::test::check(
                r,
                second.files.size() == 2 && second.edges.size() == 2,
                "pair cycle lists both files and both edges");
        }

        // 上限 1：总计不变，只列一个分组，truncated 旗标置位。
        const cg::graph::CyclesResult capped =
            cg::graph::include_cycles(store, 1);
        cg::test::check(
            r,
            capped.groups.size() == 1 && capped.truncated
                && capped.groups_total == 2 && capped.files_total == 3,
            "cap limits the listing, never the totals");

        // 纯菱形库：一个分组都没有。
        db::GraphStore       clean{":memory:"};
        const cg::db::FileId a =
            put_syn_file(clean, "e:/synthetic/dia/", "a.hpp");
        const cg::db::FileId b =
            put_syn_file(clean, "e:/synthetic/dia/", "b.hpp");
        const cg::db::FileId c =
            put_syn_file(clean, "e:/synthetic/dia/", "c.hpp");
        // shared diamond bottom, only a link target
        (void)put_syn_file(clean, "e:/synthetic/dia/", "d.hpp");
        put_include_row(clean, a, "b.hpp", 1, false);
        put_include_row(clean, a, "c.hpp", 2, false);
        put_include_row(clean, b, "d.hpp", 1, false);
        put_include_row(clean, c, "d.hpp", 1, false);
        const cg::graph::CyclesResult dia =
            cg::graph::include_cycles(clean, 10);
        cg::test::check(
            r,
            dia.groups_total == 0 && dia.groups.empty()
                && dia.files_scanned == 4,
            "diamond shape is not a cycle");

        // 严格解析（环检查绝不捏造边）：include
        // 搜索路径以完整拼写作路径后缀来模拟
        // ——仅当恰好一个文件携带它时接受，绝不凭裸
        // 尾段，且匹配多个文件的拼写不产出
        // 边（计为歧义）。
        db::GraphStore       sfx{":memory:"};
        const cg::db::FileId main_h =
            put_syn_file(sfx, "e:/synthetic/sfx/", "src/main.hpp");
        const cg::db::FileId real_h =
            put_syn_file(sfx, "e:/synthetic/sfx/", "inc/real.hpp");
        (void)put_syn_file(sfx, "e:/synthetic/sfx/", "vendor/spdlog/logger.h");
        const cg::db::FileId core_logger =
            put_syn_file(sfx, "e:/synthetic/sfx/", "app/core/logger.h");
        (void)put_syn_file(sfx, "e:/synthetic/sfx/", "twin/a/util.hpp");
        (void)put_syn_file(sfx, "e:/synthetic/sfx/", "twin/b/util.hpp");
        // 真双文件环，两侧都经搜索路径 include
        // （两个目标都不在包含者目录里，精确
        // 拼接落空，只有唯一的全拼写后缀解析成功）。
        put_include_row(sfx, main_h, "inc/real.hpp", 1, false);
        put_include_row(sfx, real_h, "main.hpp", 1, false);
        // 参考语料幻影形态："spdlog/logger.h" 唯一后缀匹配
        // vendored 副本，但其裸尾段 "logger.h" 在保守的
        // 每后缀规则下也会折到 app/core/logger.h——
        // strict 绝不把 core_logger 接进 main 的出边（只有
        // 下方 core_logger -> main 是真的，闭不成环）。
        put_include_row(sfx, main_h, "spdlog/logger.h", 1, true);
        put_include_row(sfx, core_logger, "main.hpp", 1, false);
        // 歧义：拼写同时匹配 twin/a/util.hpp 与 twin/b/util.hpp。
        put_include_row(sfx, real_h, "util.hpp", 1, true);
        // 未解析：拼写什么都匹配不上。
        put_include_row(sfx, real_h, "missing/gone.hpp", 1, true);

        const cg::graph::CyclesResult strict =
            cg::graph::include_cycles(sfx, 10);
        cg::test::check(
            r,
            strict.groups_total == 1 && strict.files_scanned == 6,
            "suffix-resolved cycle found, one group");
        cg::test::check(
            r,
            strict.groups.size() == 1
                && strict.groups.front().files.size() == 2,
            "cycle group is exactly the two real participants");
        if (strict.groups.size() == 1)
        {
            const cg::graph::CycleGroup& g = strict.groups.front();
            bool has_main = false, has_real = false, has_core = false;
            for (const cg::db::FileId member : g.files)
            {
                has_main = has_main || member == main_h;
                has_real = has_real || member == real_h;
                has_core = has_core || member == core_logger;
            }
            cg::test::check(
                r,
                has_main && has_real && !has_core && g.edges.size() == 2,
                "no phantom member: core logger stays out of the "
                "cycle (unique-suffix edges only)");
        }
        cg::test::check(
            r,
            strict.ambiguous_links == 1,
            "multi-match spelling counted as ambiguous, no edge");
        cg::test::check(
            r,
            strict.unresolved_links == 1,
            "unmatched spelling still counted unresolved");
    }

    // 失败指纹门控（schema v7）：一个文件带两条编译条目，
    // 一成一败。失败条目在其世界状态
    // 未变时必须跳过，绝不连累兄弟的符号，且
    // 在 argv / 模块喂入 / force 改变世界时必须重试。也钉住
    // 整文件的 parse_status 聚合（只要一条活条目 Ok 就 Ok）
    // 以及瞬态退避。
    void failure_fingerprint_gating(R& r)
    {
        db::GraphStore                         store{":memory:"};
        std::vector<cg::clang::CompileCommand> commands = load_commands();
        cg::test::check(r, commands.size() == 3, "sample commands load");
        if (commands.size() != 3)
        {
            return;
        }
        cg::clang::CompileCommand failing;
        for (const cg::clang::CompileCommand& c : commands)
        {
            if (c.file.ends_with("canvas.cpp"))
            {
                failing = c;
            }
        }
        const std::string canvas_path = failing.file;
        failing.argv.push_back("-DFAILING_ENTRY=1");
        failing.preordained_failure =
            "parse-failed: probe - deterministic entry failure";
        commands.push_back(failing);

        const cg::graph::IndexSummary first =
            make_indexer(store, /*force=*/false).run(commands);
        cg::test::check(
            r,
            first.failed == 1,
            std::format(
                "first run fails exactly one entry ({})",
                first.failed));
        cg::test::check(
            r,
            find_one(
                store,
                "render::Canvas::total_area",
                r,
                "Canvas::total_area")
                != 0,
            "the successful sibling entry produced symbols");

        // 第二次运行：失败条目的世界状态未变 -> 跳过，
        // 兄弟的符号必须幸存（厄运隔离）。
        const cg::graph::IndexSummary second =
            make_indexer(store, /*force=*/false).run(commands);
        cg::test::check(
            r,
            second.stale == 0 && second.skipped == 3 && second.failed == 1,
            std::format(
                "unchanged failed entry skips (stale {}, "
                "skipped {}, failed {})",
                second.stale,
                second.skipped,
                second.failed));
        cg::test::check(
            r,
            find_one(
                store,
                "render::Canvas::total_area",
                r,
                "Canvas::total_area")
                != 0,
            "failure skip does not doom the sibling's symbols");
        {
            const auto failures = db::all_failures(store);
            cg::test::check(
                r,
                failures.size() == 1
                    && failures[0].failure_class == "parse-failed"
                    && failures[0].attempts == 1,
                "one failure row keyed by (file, digest)");
            const auto row = store.find_file(cg::util::norm_path(canvas_path));
            cg::test::check(
                r,
                row.has_value() && row->status == cg::util::ParseStatus::Ok,
                "whole-file status stays Ok while a live entry is "
                "Ok");
        }

        // 改失败条目的 argv 即改变其摘要 -> 新的
        // 指纹行，条目因此重试。
        commands.back().argv.push_back("-DCHANGED=2");
        const cg::graph::IndexSummary third =
            make_indexer(store, /*force=*/false).run(commands);
        // 重试的失败作业会给前向闭包下种（既有
        // 契约），所以 stale 可能超过那一个改动条目；
        // 要紧的是它没有跳过。
        cg::test::check(
            r,
            third.stale >= 1 && third.failed == 1,
            std::format(
                "argv change retries the entry (stale {}, "
                "failed {})",
                third.stale,
                third.failed));
        const cg::graph::IndexSummary fourth =
            make_indexer(store, /*force=*/false).run(commands);
        cg::test::check(
            r,
            fourth.stale == 0 && fourth.failed == 1,
            "the retried entry skips again once recorded");

        // 模块喂入生成是失败身份的一部分：挂上
        // 喂入会改变 feed_digest，哪怕 argv 原封不动。
        commands.back().module_feeds.push_back(
            {.name = "probe.module",
             .pcm = cg::util::norm_path("ccg-failure-probe.pcm")});
        const cg::graph::IndexSummary fifth =
            make_indexer(store, /*force=*/false).run(commands);
        cg::test::check(
            r,
            fifth.stale >= 1 && fifth.failed == 1,
            "feed digest change retries the entry");

        // --force 总是重试；兄弟仍保住其符号。
        const cg::graph::IndexSummary forced =
            make_indexer(store, /*force=*/true).run(commands);
        cg::test::check(
            r,
            forced.failed == 1
                && find_one(
                       store,
                       "render::Canvas::total_area",
                       r,
                       "Canvas::total_area")
                       != 0,
            "forced run retries without dooming the sibling");
        cg::test::check(
            r,
            db::duplicate_symbol_keys(store) == 0
                && db::duplicate_edge_sites(store) == 0,
            "batch dedup leaves no duplicate rows");
    }

    // 失败依赖保留：失败 TU 的 includes 行随失败清空，
    // 所以后续每次尝试算出的依赖闭包都是空的。
    // 失败写入是合并而非覆盖——空快照保住既有
    // 路径（新路径出现则并集）——所以修好被包含的
    /// 以既定选项跑一轮索引（Indexer 吞选项，每轮拷贝一份）。
    [[nodiscard]]
    auto run_index_commands(
        db::GraphStore&                               store,
        const cg::graph::IndexOptions&                opt,
        const std::vector<cg::clang::CompileCommand>& cmds)
        -> cg::graph::IndexSummary
    {
        cg::graph::IndexOptions o = opt;
        return cg::graph::Indexer{store, std::move(o)}.run(cmds);
    }

    // 头文件仍会把该 TU 拉进 `reached` 并重试。两个连续的
    // 编辑 -> 重试循环钉住保留不是一次性的。
    void failure_deps_retention(R& r)
    {
        const fs::path  base = fs::path{"ccg-incr-scratch"} / "failure-deps";
        std::error_code ec;
        (void)fs::remove_all(base, ec);
        copy_tree(fs::path{std::string{fixture_dir}}, base);

        const std::string cdb = (base / "compile_commands.json").string();
        std::vector<cg::clang::CompileCommand> commands =
            cg::clang::load(cdb, {});
        cg::test::check(r, !commands.empty(), "deps-retention commands load");
        if (commands.empty())
        {
            return;
        }

        db::GraphStore          store{":memory:"};
        cg::graph::IndexOptions opt;
        opt.roots = {cg::util::norm_path(base.generic_string())};
        opt.progress = false;
        opt.compile_db_path = cdb;

        std::size_t failing_index = commands.size();
        for (std::size_t i = 0; i < commands.size(); ++i)
        {
            if (commands[i].file.ends_with("canvas.cpp"))
            {
                failing_index = i;
            }
        }
        cg::test::check(
            r,
            failing_index != commands.size(),
            "canvas entry present in the fixture");
        if (failing_index == commands.size())
        {
            return;
        }
        cg::clang::CompileCommand& failing = commands[failing_index];

        const cg::graph::IndexSummary first =
            run_index_commands(store, opt, commands);
        cg::test::check(
            r,
            first.failed == 0,
            std::format("baseline run succeeds (failed {})", first.failed));

        // 确定性地弄失败个文件。argv 旗标给失败
        // 条目自己的摘要，它还没有指纹行，因此真的
        // 会重试（同摘要条目会当新条目直接快路径）；
        // 第一次失败仍看得到基线解析
        // 记下的 includes，所以其依赖快照非空。
        failing.argv.push_back("-DFAILING_ENTRY=1");
        failing.preordained_failure = "parse-failed: deps retention probe";
        const cg::graph::IndexSummary second =
            run_index_commands(store, opt, commands);
        cg::test::check(
            r,
            second.failed == 1 && second.stale >= 1,
            std::format(
                "first failure retries (stale {}, failed {})",
                second.stale,
                second.failed));
        {
            const auto rows = db::all_failures(store);
            cg::test::check(
                r,
                rows.size() == 1 && !rows.front().deps_json.empty()
                    && rows.front().deps_json.find("canvas.hpp")
                           != std::string::npos
                    && rows.front().attempts == 1,
                "first failure records the dependency closure");
        }

        // 循环 1：改依赖的内容（单纯 touch 不得
        // 重试：主文件的 content_hash 没变）。重试的
        // 失败这次算出空闭包；合并必须保留既有
        // 闭包。
        const fs::path dep = base / "canvas.hpp";
        write_text(dep, read_text(dep) + "\n// deps-retention edit 1\n");
        const cg::graph::IndexSummary third =
            run_index_commands(store, opt, commands);
        cg::test::check(
            r,
            third.failed == 1 && third.stale >= 1,
            std::format(
                "dependency edit retries (stale {}, failed "
                "{})",
                third.stale,
                third.failed));
        {
            const auto rows = db::all_failures(store);
            cg::test::check(
                r,
                rows.size() == 1 && !rows.front().deps_json.empty()
                    && rows.front().deps_json.find("canvas.hpp")
                           != std::string::npos
                    && rows.front().attempts == 2,
                "empty closure keeps the prior deps and advances "
                "attempts");
        }

        // 循环 2：第二次内容编辑必须再次重试——保留挺得住
        // 反复失败，而非一次重试后就垮掉。
        write_text(dep, read_text(dep) + "// deps-retention edit 2\n");
        const cg::graph::IndexSummary fourth =
            run_index_commands(store, opt, commands);
        cg::test::check(
            r,
            fourth.failed == 1 && fourth.stale >= 1,
            std::format(
                "second dependency edit retries (stale {}, "
                "failed {})",
                fourth.stale,
                fourth.failed));
        {
            const auto rows = db::all_failures(store);
            cg::test::check(
                r,
                rows.size() == 1 && !rows.front().deps_json.empty()
                    && rows.front().deps_json.find("canvas.hpp")
                           != std::string::npos
                    && rows.front().attempts == 3,
                "second retry retains the closure again");
        }

        // 不再编辑：保留的闭包在写入时重新 stat 过，
        // 世界未变，条目回落到失败跳过。
        const cg::graph::IndexSummary fifth =
            run_index_commands(store, opt, commands);
        cg::test::check(
            r,
            fifth.stale == 0 && fifth.failed == 1
                && fifth.skipped
                       == static_cast<std::int64_t>(commands.size()) - 1,
            std::format(
                "retained deps re-arm the failure skip (stale {}, "
                "skipped {}, failed {})",
                fifth.stale,
                fifth.skipped,
                fifth.failed));
        (void)fs::remove_all(base, ec);
    }

    // 瞬态失败类会退避：世界状态未变就一直跳过，
    // 直到 retry_after 到期再重试。
    void failure_transient_backoff(R& r)
    {
        db::GraphStore                         store{":memory:"};
        std::vector<cg::clang::CompileCommand> commands = load_commands();
        cg::test::check(r, !commands.empty(), "sample commands load");
        if (commands.empty())
        {
            return;
        }
        cg::clang::CompileCommand& c = commands.front();
        c.preordained_failure =
            "engine-visit-crash: probe - transient engine fault";
        (void)make_indexer(store, /*force=*/false).run(commands);
        const cg::graph::IndexSummary skipped =
            make_indexer(store, /*force=*/false).run(commands);
        cg::test::check(
            r,
            skipped.stale == 0 && skipped.failed == 1,
            std::format(
                "transient failure backs off (stale {}, "
                "failed {})",
                skipped.stale,
                skipped.failed));
        {
            const auto failures = db::all_failures(store);
            cg::test::check(
                r,
                failures.size() == 1
                    && failures[0].failure_class == "engine-visit-crash"
                    && failures[0].retry_after > 0,
                "transient failure records a retry deadline");
        }
        // 模拟退避到期：下一次运行重试并改写
        // 失败行（attempts 递增）。
        store.exec("UPDATE failures SET retry_after = 0");
        const cg::graph::IndexSummary retried =
            make_indexer(store, /*force=*/false).run(commands);
        cg::test::check(
            r,
            retried.stale == 1 && retried.failed == 1,
            std::format(
                "expired backoff retries (stale {}, failed "
                "{})",
                retried.stale,
                retried.failed));
        const auto failures = db::all_failures(store);
        cg::test::check(
            r,
            failures.size() == 1 && failures[0].attempts == 2,
            "retry increments attempts");
    }

    /// 合成动态派发语料的方法符号行（键前缀 method）。
    [[nodiscard]]
    auto put_method_symbol(
        db::GraphStore&      store,
        const cg::db::FileId f,
        const std::string&   file_stem,
        const std::string&   name,
        int                  line,
        std::uint32_t        flags = 0) -> cg::db::SymId
    {
        return store.upsert_symbol(
            {.unique_key = "method||" + name + "|" + file_stem,
             .kind = db::SymbolKind::Method,
             .name = name,
             .ns = "",
             .sig_hash = std::nullopt,
             .file_id = f,
             .line = line,
             .col = 1,
             .flags = flags});
    }

    /// 合成动态派发语料的调用者函数行（键前缀 function）。
    [[nodiscard]]
    auto put_dyn_caller(
        db::GraphStore&      store,
        const cg::db::FileId f,
        const std::string&   file_stem,
        const std::string&   name,
        int                  line) -> cg::db::SymId
    {
        return store.upsert_symbol(
            {.unique_key = "function||" + name + "|" + file_stem,
             .kind = db::SymbolKind::Function,
             .name = name,
             .ns = "",
             .sig_hash = std::nullopt,
             .file_id = f,
             .line = line,
             .col = 1});
    }

    /// 邻居结果中某符号的派发状态（不在结果内给 None）。
    [[nodiscard]]
    auto dispatch_status_of(
        const cg::graph::NeighborsResult& res,
        cg::db::SymId                     id) -> cg::graph::DispatchStatus
    {
        for (const auto& e : res.entries)
        {
            if (e.id == id)
            {
                return e.dispatch_status;
            }
        }
        return cg::graph::DispatchStatus::None;
    }

    /**
     * @brief 部分证据：同一站点同时有 >=0.7 与 0.4 目标时，0.4 不得作为证据；
     *        只有 0.4 的站点必须保持 family estimate，且不产生 inferred。
     */
    void dispatch_partial_evidence(R& r)
    {
        db::GraphStore       store{":memory:"};
        const cg::db::FileId f = store.upsert_file("e:/synthetic/dyn.cpp");
        const std::string    stem = "e:/synthetic/dyn.cpp";
        const cg::db::SymId  base = put_method_symbol(
            store,
            f,
            stem,
            "If::f",
            1,
            db::sym_flags::is_pure_virtual);
        const cg::db::SymId impl_b =
            put_method_symbol(store, f, stem, "B::f", 2);
        const cg::db::SymId impl_c =
            put_method_symbol(store, f, stem, "C::f", 3);
        store.upsert_edge(
            {.src = impl_b,
             .dst = base,
             .type = db::EdgeType::Overrides,
             .site_file = f,
             .site_line = 2});
        store.upsert_edge(
            {.src = impl_c,
             .dst = base,
             .type = db::EdgeType::Overrides,
             .site_file = f,
             .site_line = 3});

        const cg::db::SymId u1 = put_dyn_caller(store, f, stem, "u1", 10);
        const cg::db::SymId u2 = put_dyn_caller(store, f, stem, "u2", 20);
        store.upsert_edge(
            {.src = u1,
             .dst = base,
             .type = db::EdgeType::Calls,
             .site_file = f,
             .site_line = 10,
             .attrs = db::edge_attrs::dispatch_virtual});
        store.upsert_edge(
            {.src = u2,
             .dst = base,
             .type = db::EdgeType::Calls,
             .site_file = f,
             .site_line = 20,
             .attrs = db::edge_attrs::dispatch_virtual});
        // u1 的站点：0.7 -> B（证据）+ 0.4 -> C（部分证据，不得算数）。
        store.upsert_edge(
            {.src = u1,
             .dst = impl_b,
             .type = db::EdgeType::VirtualDispatch,
             .site_file = f,
             .site_line = 10,
             .attrs = db::edge_attrs::dispatch_inferred,
             .confidence = 0.7});
        store.upsert_edge(
            {.src = u1,
             .dst = impl_c,
             .type = db::EdgeType::VirtualDispatch,
             .site_file = f,
             .site_line = 10,
             .attrs = db::edge_attrs::dispatch_inferred,
             .confidence = 0.4});
        // u2 的站点：只有 0.4，不构成证据。
        store.upsert_edge(
            {.src = u2,
             .dst = impl_c,
             .type = db::EdgeType::VirtualDispatch,
             .site_file = f,
             .site_line = 20,
             .attrs = db::edge_attrs::dispatch_inferred,
             .confidence = 0.4});

        // 查基类：u1 有 0.7 目标 -> inferred；u2 只有 0.4 -> 家族估算。
        const cg::graph::NeighborsResult base_res =
            cg::graph::callers(store, base, 1);
        cg::test::check(
            r,
            dispatch_status_of(base_res, u1)
                == cg::graph::DispatchStatus::Inferred,
            "0.7 target at a partially proven site is evidence");
        cg::test::check(
            r,
            dispatch_status_of(base_res, u2)
                != cg::graph::DispatchStatus::Inferred,
            "0.4-only site is never inferred");
        // 查具体实现 C：u1 的 0.7 目标指向 B（不是 C）-> 不应拉入；
        // u2 的 0.4 不构成证据 -> 至多 family estimate。
        const cg::graph::NeighborsResult c_res =
            cg::graph::callers(store, impl_c, 1);
        cg::test::check(
            r,
            dispatch_status_of(c_res, u1) == cg::graph::DispatchStatus::None,
            "caller proven to dispatch elsewhere is not pulled in");
        cg::test::check(
            r,
            dispatch_status_of(c_res, u2)
                != cg::graph::DispatchStatus::Inferred,
            "0.4 target never counts as evidence for its impl");
    }

    const cg::test::Registrar reg_partial_evidence{
        "graph.dispatch-partial-evidence",
        &dispatch_partial_evidence};

    // cl 方言宽松特化的诊断驱动改写经真索引器落库：cl 条目（MSVC 接受、
    // clang 拒绝的类外成员特化）被编译器 FixIt 内存改写救回——TU 不再
    // 失败、统计字段分账、函数特化成为定义入图；GNU 条目改写不适用
    // （方言门禁），残余同类错误由良性白名单的宽特化家族兜底索引（带
    // 注记），失败类口径同样零记录。
    void dialect_fixit_stats(R& r)
    {
        namespace cc = cg::clang;
        if (cc::driver_info().driver.empty())
        {
            cg::test::check(
                r,
                true,
                "dialect fixit stats test skipped: no driver");
            return;
        }
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        const cg::test::ScratchDir scratch{
            fs::temp_directory_path()
            / ("ccg-graph-fixit-" + std::to_string(stamp))};
        const fs::path&   base = scratch.base;
        const std::string root = cg::util::norm_path(base.generic_string());
        // 三处同族形状：两条变量特化（声明形 + 初始化形）+ 一条函数特化。
        const std::string_view shape =
            "template <class T> struct A { static int v; };\n"
            "template <class T> int A<T>::v = 0;\n"
            "__declspec(selectany) int A<int>::v;\n"
            "__declspec(selectany) int A<long>::v = 3;\n"
            "template <typename T> struct Box { void Seal(); };\n"
            "void Box<int>::Seal() {}\n"
            "int main() { return 0; }\n";
        scratch.put(base / "spec_cl.cpp", shape);
        scratch.put(base / "spec_gnu.cpp", shape);
        const std::string db_text =
            "[{\"directory\":" + q(root)
            + ",\"file\":" + q(root + "/spec_cl.cpp")
            + ",\"arguments\":[\"cl.exe\",\"/c\",\"/I\"," + q(root)
            + ",\"/EHsc\",\"/std:c++20\"," + q(root + "/spec_cl.cpp")
            + "]},"
              "{\"directory\":"
            + q(root) + ",\"file\":" + q(root + "/spec_gnu.cpp")
            + ",\"arguments\":[\"clang++\",\"-I\"," + q(root)
            + ",\"-std=c++20\",\"-c\"," + q(root + "/spec_gnu.cpp") + "]}]";
        scratch.put(base / "compile_commands.json", db_text);
        const std::string cdb = cg::util::norm_path(
            (base / "compile_commands.json").generic_string());
        std::vector<cc::CompileCommand> commands = cc::load(cdb, {});
        cg::test::check(r, commands.size() == 2, "dialect fixit fixture loads");
        if (commands.size() != 2)
        {
            return;
        }
        db::GraphStore          store{":memory:"};
        cg::graph::IndexOptions opt;
        opt.roots = {root};
        opt.progress = false;
        opt.compile_db_path = cdb;
        const cg::graph::IndexSummary sum =
            cg::graph::Indexer{store, std::move(opt)}.run(commands);
        cg::test::check(
            r,
            sum.failed == 0,
            "neither entry fails: the cl entry is fixed via the compiler "
            "fixit, and the GNU entry's residual lenient-specialization "
            "errors are benign-whitelisted (template-spec-introducer "
            "family) under the benign-error policy");
        cg::test::check(
            r,
            sum.dialect_fixit_tus == 1 && sum.dialect_fixit_failed_tus == 0,
            "dialect rewrite counters book exactly the fixed cl TU");
        cg::test::check(
            r,
            sum.dialect_fixit_notes.size() == 1
                && sum.dialect_fixit_notes.front().find("spec_cl.cpp")
                       != std::string::npos
                && sum.dialect_fixit_notes.front().find("3 fixit edit(s)")
                       != std::string::npos,
            "the rewritten TU carries a diagnostic note with the "
            "edit count");
        cg::test::check(
            r,
            sum.benign_errored_tus == 1 && sum.benign_error_notes.size() == 1
                && sum.benign_error_notes.front().find("spec_gnu.cpp")
                       != std::string::npos
                && sum.benign_error_notes.front().find(
                       "template-spec-introducer")
                       != std::string::npos,
            "the no-rewrite GNU TU indexes as benign with the "
            "lenient-specialization family (never booked as a fixit)");
        bool lenient_booked = false;
        for (const auto& [name, count] : sum.failure_classes)
        {
            lenient_booked =
                lenient_booked
                || name == "msvc-permissive-lenient-specialization";
        }
        cg::test::check(
            r,
            !lenient_booked,
            "no lenient-specialization failure class remains (the "
            "rewrite healed the cl TU; the GNU TU is benign-indexed "
            "with a note instead of failing)");
        // 改写前恢复机制下函数特化不成为定义（不入图）；改写后成为定义
        // 入图——行号即书写行。
        const db::FindResult seal =
            db::find_symbols(store, "Seal", db::SymbolFilter{}, false);
        bool seal_def = false;
        for (const db::SymbolRow& row : seal.matches)
        {
            if (cg::util::norm_path(row.file)
                    == cg::util::norm_path(root + "/spec_cl.cpp")
                && row.line == 6)
            {
                seal_def = true;
            }
        }
        cg::test::check(
            r,
            seal_def,
            "the member-function specialization is a graph symbol "
            "after the fixit rewrite");
    }

    const cg::test::Registrar reg_dialect_fixit{
        "graph.dialect-fixit-stats",
        &dialect_fixit_stats};

    [[nodiscard]]
    auto plain_cdb_item(const std::string& root, const std::string& file)
        -> std::string;

    [[nodiscard]]
    auto tagged_cdb_item(
        const std::string& root,
        const std::string& file,
        const std::string& tag) -> std::string;

    [[nodiscard]]
    auto flip_fn_in_graph(
        db::GraphStore&    store,
        const std::string& root,
        const std::string& rel_file) -> bool;

    [[nodiscard]]
    auto commands_with_tag(
        const std::vector<cg::clang::CompileCommand>& commands,
        std::size_t                                   idx,
        const std::string& tag) -> std::vector<cg::clang::CompileCommand>;

    // "报错但 AST 完好"的良性错误索引：悬垂引用族（引用成员绑定临时
    // 对象，无旗标可压的硬错误）单独出现时 TU 照常索引（白名单拦截
    // 失败判定）——注记与计数留痕、失败类零记录、符号照常入图；与
    // 真错误混现时（未命中白名单）照常判失败（硬边界：绝不能把真
    // 失败洗成成功）。
    void benign_error_index(R& r)
    {
        namespace cc = cg::clang;
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        const cg::test::ScratchDir scratch{
            fs::temp_directory_path()
            / ("ccg-graph-benign-" + std::to_string(stamp))};
        const fs::path&   base = scratch.base;
        const std::string root = cg::util::norm_path(base.generic_string());
        // 悬垂引用最小形状：构造函数初始化列表把引用成员绑到字面量
        // 临时对象（-ast-dump 实测游标树完整产出，符号可提取）。
        const std::string_view dangling = "struct A {\n"
                                          "    const int& r;\n"
                                          "    A() : r(42) {}\n"
                                          "};\n"
                                          "int main() { return 0; }\n";
        scratch.put(base / "dang_ok.cpp", dangling);
        // 混合腿：同一悬垂引用形状 + 一条清单外真错误（未声明标识符）。
        scratch.put(
            base / "dang_mixed.cpp",
            "struct B {\n"
            "    const int& r;\n"
            "    B() : r(7) {}\n"
            "};\n"
            "int main() { return no_such_symbol; }\n");
        const std::string db_text =
            "[" + plain_cdb_item(root, root + "/dang_ok.cpp") + ","
            + plain_cdb_item(root, root + "/dang_mixed.cpp") + "]";
        scratch.put(base / "compile_commands.json", db_text);
        const std::string cdb = cg::util::norm_path(
            (base / "compile_commands.json").generic_string());
        std::vector<cc::CompileCommand> commands = cc::load(cdb, {});
        cg::test::check(r, commands.size() == 2, "benign error fixture loads");
        if (commands.size() != 2)
        {
            return;
        }
        db::GraphStore          store{":memory:"};
        cg::graph::IndexOptions opt;
        opt.roots = {root};
        opt.progress = false;
        opt.compile_db_path = cdb;
        const cg::graph::IndexSummary sum =
            cg::graph::Indexer{store, std::move(opt)}.run(commands);
        cg::test::check(
            r,
            sum.failed == 1,
            "only the mixed-error TU fails (the pure dangling-reference "
            "TU is indexed despite its error diagnostics)");
        cg::test::check(
            r,
            sum.benign_errored_tus == 1,
            "the benign-error counter books exactly the indexed "
            "dangling-reference TU (never a failed one)");
        cg::test::check(
            r,
            sum.benign_error_notes.size() == 1
                && sum.benign_error_notes.front().find("dang_ok.cpp")
                       != std::string::npos
                && sum.benign_error_notes.front().find(
                       "ignored 1 benign error(s) [dangling-reference-bind]")
                       != std::string::npos
                && sum.benign_error_notes.front().find(
                       "binds to a temporary object whose lifetime")
                       != std::string::npos,
            "the benign TU carries a note with the family name and the "
            "first diagnostic spelling");
        bool parse_failed = false;
        for (const auto& [name, count] : sum.failure_classes)
        {
            parse_failed = parse_failed || name == "parse-failed";
        }
        cg::test::check(
            r,
            parse_failed,
            "the mixed-error TU still books the parse-failed class "
            "(whitelist never launders a real failure)");
        // AST 完好的直接证据：悬垂引用 TU 的符号照常入图（结构体第 1
        // 行、构造函数第 3 行，行号即书写行）。
        const db::FindResult found =
            db::find_symbols(store, "A", db::SymbolFilter{}, false);
        bool struct_a = false;
        bool ctor_a = false;
        for (const db::SymbolRow& row : found.matches)
        {
            if (cg::util::norm_path(row.file)
                != cg::util::norm_path(root + "/dang_ok.cpp"))
            {
                continue;
            }
            if (row.line == 1 && row.kind == db::SymbolKind::Record)
            {
                struct_a = true;
            }
            if (row.line == 3 && row.kind == db::SymbolKind::Constructor)
            {
                ctor_a = true;
            }
        }
        cg::test::check(
            r,
            struct_a && ctor_a,
            "the dangling-reference TU's symbols are in the graph "
            "(struct A and its constructor)");
    }

    const cg::test::Registrar reg_benign_error{
        "graph.benign-error-index",
        &benign_error_index};

    // 失败残留 -> 重试翻绿的事务完整性（外键提交失败事故的回归）：
    // 上一轮全败的文件（失败记录在手、parse_status=Failed、符号已被全败
    // 清除）在本轮重试成功时，其符号与派生线索（owner 外键指向这些符号）
    // 必须同事务落库且提交不因外键失败回滚——全败清除（all_failed purge）
    // 绝不能把本 run 成功发射载荷的文件清掉，线索/站点的 owner 解析对
    // 已删除符号必须按"丢弃该行"让位而不是撞外键。四轮翻转（成功 ->
    // 注入失败 -> 摘除注入再成功 -> 再失败）钉住宏观行为。实测大库的
    // 事故发生在波式路径，同一组翻转在关波与 CCG_WAVE_SIZE=16 两臂
    // 各跑一遍，让修复 A 的账本（fb_ledger_）与关波（jobs 统计）
    // 两个对账分支都进回归。
    void failure_recovery_rounds(R& r, bool fb)
    {
        namespace cc = cg::clang;
        const cg::test::EnvScope wave_env{"CCG_WAVE_SIZE"};
        if (fb)
        {
            wave_env.set("16");
        }
        const std::string arm = fb ? "fb" : "classic";
        const auto        stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        const cg::test::ScratchDir scratch{
            fs::temp_directory_path()
            / ("ccg-graph-fk-recovery-" + arm + "-" + std::to_string(stamp))};
        const fs::path&   base = scratch.base;
        const std::string root = cg::util::norm_path(base.generic_string());
        // 函数 + 引用局部变量：局部变量槽让分析层产出指向该函数的
        // 类型线索（owner 外键指向函数符号行——正是翻绿场景里被
        // 误清又重发射的那类符号）。
        scratch.put(
            base / "flip.cpp",
            "struct S { int v = 1; };\n"
            "int flip(int x) {\n"
            "    const S& s = S{};\n"
            "    return x + s.v;\n"
            "}\n"
            "int main() { return flip(1); }\n");
        const std::string arm_tag = "[" + arm + "] ";
        const std::string db_text =
            "[" + tagged_cdb_item(root, root + "/flip.cpp", "-DVARIANT=1")
            + "]";
        scratch.put(base / "compile_commands.json", db_text);
        const std::string cdb = cg::util::norm_path(
            (base / "compile_commands.json").generic_string());
        std::vector<cc::CompileCommand> commands = cc::load(cdb, {});
        cg::test::check(
            r,
            commands.size() == 1,
            arm_tag + "recovery fixture loads one command");
        if (commands.size() != 1)
        {
            return;
        }
        db::GraphStore          store{":memory:"};
        cg::graph::IndexOptions opt;
        opt.roots = {root};
        opt.progress = false;
        opt.compile_db_path = cdb;
        // 轮 1：正常成功——函数符号入图。
        const cg::graph::IndexSummary first =
            run_index_commands(store, opt, commands);
        cg::test::check(r, first.failed == 0, arm_tag + "first run is clean");
        cg::test::check(
            r,
            flip_fn_in_graph(store, root, "flip.cpp"),
            arm_tag + "the flip function is in the graph after the clean run");

        // 轮 2：换 argv（新摘要必然重试）+ 注入失败——文件全败，符号
        // 被全败清除（合法语义），失败记录落库。
        std::vector<cc::CompileCommand> failing = commands;
        failing[0].argv.push_back("-DFAILING=1");
        failing[0].preordained_failure =
            "parse-failed: fk-recovery probe entry failure";
        const cg::graph::IndexSummary second =
            run_index_commands(store, opt, failing);
        cg::test::check(
            r,
            second.failed == 1,
            arm_tag + "second run fails the single entry");
        cg::test::check(
            r,
            !flip_fn_in_graph(store, root, "flip.cpp"),
            arm_tag + "the all-failed purge clears the file's symbols");

        // 轮 3：再换 argv（绕开"世界未变则跳过"）摘除注入——失败残留
        // 的文件重试翻绿：符号 + 指向它们的类型线索同事务落库，提交
        // 不得因外键失败回滚（翻绿前库内该文件零符号行、零线索行，
        // 全是新插行——fk-commit-failure-dig 案的触发形状）。
        std::vector<cc::CompileCommand> recovered = commands;
        recovered[0].argv.push_back("-DRECOVERED=1");
        const cg::graph::IndexSummary third =
            run_index_commands(store, opt, recovered);
        cg::test::check(
            r,
            third.failed == 0 && third.benign_errored_tus == 0,
            arm_tag + "third run re-indexes the recovered entry cleanly");
        cg::test::check(
            r,
            flip_fn_in_graph(store, root, "flip.cpp"),
            arm_tag + "the recovered entry's symbols survive the commit");
        cg::test::check(
            r,
            db::duplicate_symbol_keys(store) == 0
                && db::duplicate_edge_sites(store) == 0,
            arm_tag + "recovery leaves no duplicate rows");
        cg::test::check(
            r,
            store.symbol_exists(1) && !store.symbol_exists(1'000'000'000),
            arm_tag + "symbol_exists distinguishes present from absent rows");

        // 轮 4：再次全败（失败记录的 attempts 递增、符号再次清除）——
        // 翻转循环双向都稳定。
        std::vector<cc::CompileCommand> failing2 = recovered;
        failing2[0].argv.push_back("-DFAILING2=1");
        failing2[0].preordained_failure =
            "parse-failed: fk-recovery probe second failure";
        const cg::graph::IndexSummary fourth =
            run_index_commands(store, opt, failing2);
        cg::test::check(
            r,
            fourth.failed == 1 && !flip_fn_in_graph(store, root, "flip.cpp"),
            arm_tag + "a second all-failed round clears symbols again");
    }

    void failure_recovery_keeps_emitted_symbols(R& r)
    {
        failure_recovery_rounds(r, false);
        failure_recovery_rounds(r, true);
    }

    const cg::test::Registrar reg_failure_recovery{
        "graph.failure-recovery-keeps-emitted-symbols",
        &failure_recovery_keeps_emitted_symbols};

    // 实测大库事故的真实触发形状：上一条回归的四轮翻转走的是
    // "新摘要 -> 初始解析成功 -> 计划段统计正确"的路径，对修复 A 的对账
    // 拦截分支零区分力。本用例按代码次序钉住事故链：failure_skip 计划
    // （失败记录在手、世界未变）在计划段统计冻结成 all_failed_fids 之后，
    // 被陈旧闭包提升（本例经"清除同名孪生之一提升裸名歧义"把全部存量
    // 文件入种子集）重解析并成功——提交段必须以发射事实跳过对它的全败
    // 清除，否则刚发射的符号与指向它们的类型线索同事务内被删，提交以
    // 外键失败整轮回滚（或符号静默丢失）。同样关波/波式两臂各跑一遍。
    void name_lift_parse_scope_closed_rounds(R& r, bool fb)
    {
        namespace cc = cg::clang;
        const cg::test::EnvScope wave_env{"CCG_WAVE_SIZE"};
        if (fb)
        {
            wave_env.set("16");
        }
        const std::string arm = fb ? "fb" : "classic";
        const std::string arm_tag = "[" + arm + "] ";
        const auto        stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        const cg::test::ScratchDir scratch{
            fs::temp_directory_path()
            / ("ccg-graph-stale-lift-" + arm + "-" + std::to_string(stamp))};
        const fs::path&   base = scratch.base;
        const std::string root = cg::util::norm_path(base.generic_string());
        // a.cpp：翻绿主角（函数 + 局部变量引用，供分析层产出指向函数
        // 的类型线索——owner 外键正是事故中先被删后引用的那类符号）。
        scratch.put(
            base / "a.cpp",
            "struct S { int v = 1; };\n"
            "int flip(int x) {\n"
            "    const S& s = S{};\n"
            "    return x + s.v;\n"
            "}\n"
            "int main() { return flip(1); }\n");
        // x.cpp / y.cpp：同名 internal-linkage 孪生。轮 3 重写 y.cpp 摘
        // 掉其孪生 -> 名字 twin 从 2 候选变 1 -> purge_lifts_name_ambiguity
        // 为真 -> 全部存量文件入陈旧闭包种子集 -> a 的 failure_skip 计划
        // 被提升重解析。
        scratch.put(
            base / "x.cpp",
            "static int twin() { return 1; }\n"
            "int use_x() { return twin(); }\n");
        scratch.put(
            base / "y.cpp",
            "static int twin() { return 2; }\n"
            "int use_y() { return twin(); }\n");
        const std::string db_text =
            "[" + tagged_cdb_item(root, root + "/a.cpp", "-DA=1") + ","
            + tagged_cdb_item(root, root + "/x.cpp", "-DX=1") + ","
            + tagged_cdb_item(root, root + "/y.cpp", "-DY=1") + "]";
        scratch.put(base / "compile_commands.json", db_text);
        const std::string cdb = cg::util::norm_path(
            (base / "compile_commands.json").generic_string());
        std::vector<cc::CompileCommand> commands = cc::load(cdb, {});
        cg::test::check(
            r,
            commands.size() == 3,
            arm_tag + "stale-lift fixture loads three commands");
        if (commands.size() != 3)
        {
            return;
        }
        db::GraphStore          store{":memory:"};
        cg::graph::IndexOptions opt;
        opt.roots = {root};
        opt.progress = false;
        opt.compile_db_path = cdb;
        // 轮 1：三条目全部干净成功。
        const cg::graph::IndexSummary first =
            run_index_commands(store, opt, commands);
        cg::test::check(r, first.failed == 0, arm_tag + "first run is clean");
        cg::test::check(
            r,
            flip_fn_in_graph(store, root, "a.cpp"),
            arm_tag + "flip is in the graph after the clean run");
        // 轮 2：a 换摘要注入失败（世界其余未动）——a 全败、符号被合法
        // 清除、失败记录落库；摘要 A=2 同步获得指纹行（失败条目也写
        // 指纹），下一轮才能走 failure_skip 分支。
        std::vector<cc::CompileCommand> failing =
            commands_with_tag(commands, 0, "-DA=2");
        failing[0].preordained_failure =
            "parse-failed: stale-lift probe entry failure";
        const cg::graph::IndexSummary second =
            run_index_commands(store, opt, failing);
        cg::test::check(
            r,
            second.failed == 1,
            arm_tag + "second run fails the flip entry only");
        cg::test::check(
            r,
            !flip_fn_in_graph(store, root, "a.cpp"),
            arm_tag + "the all-failed purge clears flip's symbols");
        // 轮 3（层级正交化后的形态）：a 以与失败记录完全相同的摘要 A=2
        // 再次出现、注入摘除（世界对 a 未变 -> failure_skip）；y.cpp 改
        // 写摘掉其孪生（y 重解析 -> doomed 含 y::twin -> 名字 twin 歧义
        // 提升 -> 分析层升级开关置位）。解析范围=包含闭包∪配置变更，
        // 名字桶警报无权把 a 塞进解析范围——a 保持 failure_skip，其符
        // 号保持清除、失败行保留，重试由摘要变化驱动。提交事务整体不
        // 得因外键回滚。
        scratch.put(base / "y.cpp", "int other() { return 3; }\n");
        std::vector<cc::CompileCommand> lifted = failing;
        lifted[0].preordained_failure.clear();
        bool committed = true;
        try
        {
            (void)run_index_commands(store, opt, lifted);
        }
        catch (const std::exception&)
        {
            committed = false;
        }
        cg::test::check(
            r,
            committed,
            arm_tag
                + "the post-lift commit survives without a foreign-key "
                  "rollback");
        cg::test::check(
            r,
            !flip_fn_in_graph(store, root, "a.cpp"),
            arm_tag
                + "the name-bucket lift no longer re-parses the "
                  "failure_skip entry");
        cg::test::check(
            r,
            !db::all_failures(store).empty(),
            arm_tag + "the failure row survives until the digest changes");
        //   再换摘要注入失败——合法全败清除必须照常工作（防线不
        // 得过度保护）。
        std::vector<cc::CompileCommand> failing2 =
            commands_with_tag(commands, 0, "-DA=3");
        failing2[0].preordained_failure =
            "parse-failed: stale-lift probe second failure";
        const cg::graph::IndexSummary fourth =
            run_index_commands(store, opt, failing2);
        cg::test::check(
            r,
            fourth.failed == 1 && !flip_fn_in_graph(store, root, "a.cpp"),
            arm_tag + "a genuinely all-failed round still purges symbols");
    }

    void name_lift_parse_scope_closed(R& r)
    {
        name_lift_parse_scope_closed_rounds(r, false);
        name_lift_parse_scope_closed_rounds(r, true);
    }

    const cg::test::Registrar reg_name_lift_scope{
        "graph.name-lift-parse-scope-closed",
        &name_lift_parse_scope_closed};

    /// compile_commands 条目的 JSON 文本（clang++ 直调形态，无附加旗标）。
    [[nodiscard]]
    auto plain_cdb_item(const std::string& root, const std::string& file)
        -> std::string
    {
        return "{\"directory\":" + q(root) + ",\"file\":" + q(file)
               + ",\"arguments\":[\"clang++\",\"-I\"," + q(root)
               + ",\"-std=c++20\",\"-c\"," + q(file) + "]}";
    }

    /// compile_commands 条目的 JSON 文本（尾部追加变体标签旗标）。
    [[nodiscard]]
    auto tagged_cdb_item(
        const std::string& root,
        const std::string& file,
        const std::string& tag) -> std::string
    {
        return "{\"directory\":" + q(root) + ",\"file\":" + q(file)
               + ",\"arguments\":[\"clang++\",\"-I\"," + q(root)
               + ",\"-std=c++20\",\"-c\"," + q(file) + ",\"" + tag + "\"]}";
    }

    /// flip 函数是否在指定文件行上（全败清除/翻绿的观测面）。
    [[nodiscard]]
    auto flip_fn_in_graph(
        db::GraphStore&    store,
        const std::string& root,
        const std::string& rel_file) -> bool
    {
        const db::FindResult found =
            db::find_symbols(store, "flip", db::SymbolFilter{}, false);
        for (const db::SymbolRow& row : found.matches)
        {
            if (cg::util::norm_path(row.file)
                    == cg::util::norm_path(root + "/" + rel_file)
                && row.kind == db::SymbolKind::Function)
            {
                return true;
            }
        }
        return false;
    }

    /// 命令表副本的第 idx 条追加一个旗标（换摘要重试的载体）。
    [[nodiscard]]
    auto commands_with_tag(
        const std::vector<cg::clang::CompileCommand>& commands,
        std::size_t                                   idx,
        const std::string& tag) -> std::vector<cg::clang::CompileCommand>
    {
        std::vector<cg::clang::CompileCommand> cmds = commands;
        cmds[idx].argv.push_back(tag);
        return cmds;
    }

    /// 拼写到良性错误家族的判定直连。
    [[nodiscard]]
    auto family_of_spelling(std::string_view spelling) -> std::string
    {
        return std::string{
            cg::graph::detail::benign_error_family_of(std::string{spelling})};
    }

    /// 合成诊断对象（不经编译的判定层输入）。
    [[nodiscard]]
    auto make_diag(
        std::string_view        spelling,
        unsigned                line,
        const std::string&      file,
        cg::clang::DiagSeverity sev) -> cg::clang::Diagnostic
    {
        namespace cc = cg::clang;
        cc::Diagnostic d;
        d.severity = sev;
        d.spelling = std::string{spelling};
        d.location.file = file;
        d.location.line = line;
        return d;
    }

    // 良性白名单扩容的判定层单测（拼写面）：逐家族的拼写命中（正例
    // 文本取自真实编译器错误形态）与清单外反例（含硬否决的重载决议
    // 失败主形态与实体级二义拼写）。诊断对象全部合成，不经编译。
    void benign_whitelist_spelling_hits(R& r)
    {
        // 正例：拼写取自各家族真实错误形态的诊断文本。
        const std::pair<std::string_view, std::string_view> hits[] = {
            {"reference member '_callback' binds to a temporary object whose "
             "lifetime is shorter than the lifetime of the member",
             "dangling-reference-bind"},
            {"cannot initialize a parameter of type 'BSTR' (aka 'wchar_t *') "
             "with an rvalue of type 'const wchar_t *'",
             "const-char-ptr-drop"},
            {"cannot initialize a parameter of type 'TxtPtr' (aka 'char *') "
             "with an lvalue of type 'const char[2]'",
             "const-char-ptr-drop"},
            {"cannot initialize a parameter of type 'TxtPtr' (aka 'char *') "
             "with an rvalue of type 'const char *'",
             "const-char-ptr-drop"},
            // 形状钉子（合成文本）：家族只认形状不认类型名——任意名
            // 字的别名（aka 展开为字符指针）与裸字符指针（无 aka）各
            // 自命中。
            {"cannot initialize a parameter of type 'MyStr' (aka 'char *') "
             "with an rvalue of type 'const char *'",
             "const-char-ptr-drop"},
            {"cannot initialize a parameter of type 'char *' with an rvalue "
             "of type 'const char *'",
             "const-char-ptr-drop"},
            {"cannot initialize a parameter of type 'wchar_t *' with an "
             "lvalue of type 'const wchar_t[4]'",
             "const-char-ptr-drop"},
            {"reinterpret_cast from rvalue to reference type 'const int64_t "
             "&' (aka 'const long long &')",
             "rvalue-reinterpret-cast"},
            {"non-const lvalue reference to type 'demo::Widget' cannot "
             "bind to a temporary of type 'demo::Widget'",
             "nonconst-lvalue-ref-bind-temp"},
            {"missing 'template' keyword prior to dependent template name "
             "'From'",
             "missing-template-keyword"},
            {"conversion from 'const LONG' (aka 'const long') to 'Value' is "
             "ambiguous",
             "expr-conversion-ambiguous"},
            {"reference to 'byte' is ambiguous", "byte-name-ambiguous"},
            {"template specialization requires 'template<>'",
             "template-spec-introducer"},
            {"'selectany' can only be applied to data items with external "
             "linkage",
             "selectany-data-item"},
            {"value of type 'winrt::Windows::UI::Notifications::"
             "ToastTemplateType' is not implicitly convertible to 'int'",
             "enum-init-implicit-convert"},
            {"no viable conversion from 'CW2A' (aka 'CW2AEX<>') to "
             "'Json::Value'",
             "no-viable-conversion"},
            {"no viable conversion from '_bstr_t' to 'CAtlString'",
             "no-viable-conversion"},
            {"no viable conversion from 'CStringT<wchar_t, StrTraitMFC_DLL<"
             "wchar_t>>' to 'const std::wstring'",
             "no-viable-conversion"},
            {"illegal character encoding in string literal",
             "illegal-encoding-literal"},
        };
        for (const auto& [spelling, family] : hits)
        {
            cg::test::check(
                r,
                family_of_spelling(spelling) == family,
                std::format("whitelist hit: '{}' -> {}", spelling, family));
        }
        // 反例：清单外拼写必须全部未命中。硬否决边界（重载决议失败的
        // for-call-to 主形态与成员形态）与实体级二义（reference to 'X'
        // is ambiguous——除 byte 特异拼写外）都在此列。
        const std::string_view misses[] = {
            "no matching function for call to 'set'",
            "no matching function for call to 'AppendAll'",
            "no matching member function for call to 'SetToolTip'",
            "no matching constructor for initialization of 'std::string'",
            "redefinition of enumerator 'A'",
            "use of undeclared identifier 'no_such_symbol'",
            "expected ';' at end of declaration list",
            "cannot initialize a parameter of type 'int' with an rvalue of "
            "type 'std::string'",
            "cannot initialize a parameter of type 'BSTR' (aka 'wchar_t *') "
            "with an rvalue of type 'int'",
            // 形状反例（合成文本）：别名 aka 展开不是字符指针（int *）
            // 时，措辞骨架与来源全同也不得命中——钉死"形状而非措辞"。
            "cannot initialize a parameter of type 'Foo' (aka 'int *') "
            "with an rvalue of type 'const char *'",
            "reference to 'S' is ambiguous",
            "use of overloaded operator '=' is ambiguous (with operand types "
            "'CRect' and 'void')",
            "member 'm' found in multiple base classes of different types",
            "source file is not valid UTF-8",
        };
        for (const std::string_view spelling : misses)
        {
            cg::test::check(
                r,
                family_of_spelling(spelling).empty(),
                std::format("whitelist miss preserved: '{}'", spelling));
        }
    }

    // 良性白名单扩容的判定层单测（批护栏面）：宽签名家族护栏（全错
    // 同形态/条数上限/一行多错/类型定义行/Fatal 伴生）与多家族混批
    // 的注记汇总。诊断对象全部合成，不经编译。
    void benign_whitelist_batch_gates(R& r)
    {
        namespace cc = cg::clang;
        const cc::DiagSeverity err = cc::DiagSeverity::Error;
        const std::string      a = "file_a.cpp";
        const std::string      b = "file_b.cpp";
        // 供"文件必须真实存在"护栏正例使用的临时目录与多行夹具。
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path  base = fs::temp_directory_path()
                               / ("ccg-benign-sniff-" + std::to_string(stamp));
        std::error_code ec;
        (void)fs::remove_all(base, ec);
        fs::create_directories(base, ec);
        // 转换二义族三闸门。
        std::vector<cc::Diagnostic> six;
        for (unsigned line = 73; line <= 78; ++line)
        {
            six.push_back(make_diag(
                "conversion from 'const LONG' (aka 'const long') to "
                "'Value' is ambiguous",
                line,
                a,
                err));
        }
        cg::test::check(
            r,
            cg::graph::detail::all_errors_benign(six),
            "six conversion-ambiguity errors (same shape) are "
            "benign");
        std::vector<cc::Diagnostic> seventeen = six;
        for (unsigned line = 79; line <= 89; ++line)
        {
            seventeen.push_back(make_diag(
                "conversion from 'const LONG' (aka 'const long') to "
                "'Value' is ambiguous",
                line,
                a,
                err));
        }
        cg::test::check(
            r,
            !cg::graph::detail::all_errors_benign(seventeen),
            "17 conversion-ambiguity errors exceed the per-TU cap (16)");
        std::vector<cc::Diagnostic> solo_mix = six;
        solo_mix.push_back(make_diag(
            "reference member '_callback' binds to a temporary object "
            "whose lifetime is shorter than the lifetime of the member",
            512,
            a,
            err));
        cg::test::check(
            r,
            !cg::graph::detail::all_errors_benign(solo_mix),
            "conversion-ambiguity family requires all errors to share its "
            "spelling (solo gate rejects the mixed TU)");
        // 无可行转换族五重护栏（TU 有效由分类器前置门承担，其余四条
        // 在此验证）。定位文件必须真实存在：类型定义行护栏对读不到
        // 的文件按存疑拒绝（保守方向），正例因此用真实夹具。
        const fs::path lines = base / "lines.cpp";
        {
            std::ofstream out{lines, std::ios::binary};
            for (unsigned n = 1; n <= 120; ++n)
            {
                out << "int v" << n << " = f();\n";
            }
        }
        const std::string lines_path =
            cg::util::norm_path(lines.generic_string());
        std::vector<cc::Diagnostic> two_viable{
            make_diag(
                "no viable conversion from 'CAtlString' to 'std::wstring'",
                54,
                lines_path,
                err),
            make_diag(
                "no viable conversion from 'CAtlString' to 'std::wstring'",
                91,
                lines_path,
                err)};
        cg::test::check(
            r,
            cg::graph::detail::all_errors_benign(two_viable),
            "two point-wise no-viable-conversion errors on distinct lines "
            "are benign");
        std::vector<cc::Diagnostic> same_line{
            make_diag(
                "no viable conversion from 'CAtlString' to 'std::wstring'",
                54,
                lines_path,
                err),
            make_diag(
                "no viable conversion from 'CAtlString' to 'std::wstring'",
                54,
                lines_path,
                err)};
        cg::test::check(
            r,
            !cg::graph::detail::all_errors_benign(same_line),
            "two no-viable-conversion errors on one line read as statement "
            "collapse, not point-wise recovery");
        std::vector<cc::Diagnostic> eleven;
        for (unsigned line = 100; line < 111; ++line)
        {
            eleven.push_back(make_diag(
                "no viable conversion from 'CW2A' to 'std::string'",
                line,
                lines_path,
                err));
        }
        cg::test::check(
            r,
            !cg::graph::detail::all_errors_benign(eleven),
            "11 no-viable-conversion errors exceed the per-TU cap (10)");
        std::vector<cc::Diagnostic> with_fatal = two_viable;
        with_fatal.push_back(make_diag(
            "too many errors emitted, stopping now",
            0,
            "",
            cc::DiagSeverity::Fatal));
        cg::test::check(
            r,
            !cg::graph::detail::all_errors_benign(with_fatal),
            "a Fatal-severity companion (error storm / include abort) never "
            "qualifies as benign");
        cg::test::check(
            r,
            !cg::graph::detail::all_errors_benign({}),
            "an empty diagnostic list takes the normal success path (no "
            "benign note)");
        // 类型定义行护栏：真实临时文件（错误行行首 token 是类型定义
        // 关键字即拒；定位缺失按存疑拒）。目录 base 已在护栏段建好。
        const fs::path sniff = base / "sniff.cpp";
        {
            std::ofstream out{sniff, std::ios::binary};
            out << "int a = f();\r\n"
                << "struct S { int x; };\r\n"
                << "enum class E { A = other(); };\r\n"
                << "int b = g();\r\n";
        }
        const std::string sniff_path =
            cg::util::norm_path(sniff.generic_string());
        cg::test::check(
            r,
            cg::graph::detail::all_errors_benign({make_diag(
                "no viable conversion from 'CAtlString' to "
                "'std::wstring'",
                1,
                sniff_path,
                err)}),
            "a no-viable-conversion error on an expression line passes the "
            "type-definition-line gate");
        cg::test::check(
            r,
            !cg::graph::detail::all_errors_benign({make_diag(
                "no viable conversion from 'CAtlString' to "
                "'std::wstring'",
                2,
                sniff_path,
                err)}),
            "a no-viable-conversion error on a struct definition line is "
            "rejected (entity-level damage shape)");
        cg::test::check(
            r,
            !cg::graph::detail::all_errors_benign({make_diag(
                "no viable conversion from 'CAtlString' to "
                "'std::wstring'",
                3,
                sniff_path,
                err)}),
            "a no-viable-conversion error on an enum definition line is "
            "rejected");
        cg::test::check(
            r,
            !cg::graph::detail::all_errors_benign({make_diag(
                "no viable conversion from 'CAtlString' to "
                "'std::wstring'",
                9,
                sniff_path,
                err)}),
            "an out-of-range line is treated as unverifiable and rejected");
        cg::test::check(
            r,
            !cg::graph::detail::all_errors_benign({make_diag(
                "no viable conversion from 'CAtlString' to "
                "'std::wstring'",
                1,
                (base / "missing.cpp").generic_string(),
                err)}),
            "an unreadable location file is treated as unverifiable and "
            "rejected");
        (void)fs::remove_all(base, ec);
        // 多家族混批：全部命中白名单的窄签名家族自由组合；注记汇总
        // 按首现序去重点名每个家族。
        const std::vector<cc::Diagnostic> mixed{
            make_diag(
                "reinterpret_cast from rvalue to reference type 'const "
                "int64_t &' (aka 'const long long &')",
                6664,
                b,
                err),
            make_diag(
                "non-const lvalue reference to type 'demo::Widget' "
                "cannot bind to a temporary of type 'demo::Widget'",
                80,
                b,
                err),
            make_diag(
                "reinterpret_cast from rvalue to reference type 'const "
                "int64_t &' (aka 'const long long &')",
                6665,
                b,
                err)};
        cg::test::check(
            r,
            cg::graph::detail::all_errors_benign(mixed),
            "narrow-signature families combine freely when every error is "
            "whitelisted");
        cg::test::check(
            r,
            cg::graph::detail::benign_error_family_summary(mixed)
                == "rvalue-reinterpret-cast, nonconst-lvalue-ref-bind-temp",
            "the family summary names each family once in first-seen "
            "order");
    }

    const cg::test::Registrar reg_benign_spellings{
        "graph.benign-whitelist-spelling-hits",
        &benign_whitelist_spelling_hits};
    const cg::test::Registrar reg_benign_gates{
        "graph.benign-whitelist-batch-gates",
        &benign_whitelist_batch_gates};

    // 第二家族的端到端：右值 reinterpret_cast 与非 const 左值引用绑
    // 临时在同一 TU 混批（全部命中白名单）→ TU 照常索引，注记完整
    // 点名两个家族；既有悬垂族端到端（benign-error-index）不受影响。
    void benign_error_second_family(R& r)
    {
        namespace cc = cg::clang;
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        const cg::test::ScratchDir scratch{
            fs::temp_directory_path()
            / ("ccg-graph-benign2-" + std::to_string(stamp))};
        const fs::path&   base = scratch.base;
        const std::string root = cg::util::norm_path(base.generic_string());
        scratch.put(
            base / "mix_ok.cpp",
            "#include <string>\n"
            "int main()\n"
            "{\n"
            "    auto& s = std::string(\"x\");\n"
            "    (void)s;\n"
            "    const int& r = reinterpret_cast<const int&>(42);\n"
            "    (void)r;\n"
            "    return 0;\n"
            "}\n");
        const std::string db_text =
            "[" + plain_cdb_item(root, root + "/mix_ok.cpp") + "]";
        scratch.put(base / "compile_commands.json", db_text);
        const std::string cdb = cg::util::norm_path(
            (base / "compile_commands.json").generic_string());
        std::vector<cc::CompileCommand> commands = cc::load(cdb, {});
        cg::test::check(r, commands.size() == 1, "second-family fixture loads");
        if (commands.size() != 1)
        {
            return;
        }
        db::GraphStore          store{":memory:"};
        cg::graph::IndexOptions opt;
        opt.roots = {root};
        opt.progress = false;
        opt.compile_db_path = cdb;
        const cg::graph::IndexSummary sum =
            cg::graph::Indexer{store, std::move(opt)}.run(commands);
        cg::test::check(
            r,
            sum.failed == 0,
            "the mixed two-family TU (every error whitelisted) indexes "
            "successfully");
        cg::test::check(
            r,
            sum.benign_errored_tus == 1,
            "the benign counter books the mixed-family TU");
        cg::test::check(
            r,
            sum.benign_error_notes.size() == 1
                && sum.benign_error_notes.front().find(
                       "ignored 2 benign error(s) "
                       "[nonconst-lvalue-ref-bind-temp, "
                       "rvalue-reinterpret-cast]")
                       != std::string::npos
                && sum.benign_error_notes.front().find(
                       "cannot bind to a temporary")
                       != std::string::npos,
            "the note names both families in diagnostic order and quotes "
            "the first spelling");
        bool                 main_indexed = false;
        const db::FindResult found =
            db::find_symbols(store, "main", db::SymbolFilter{}, false);
        for (const db::SymbolRow& row : found.matches)
        {
            if (cg::util::norm_path(row.file)
                == cg::util::norm_path(root + "/mix_ok.cpp"))
            {
                main_indexed = true;
            }
        }
        cg::test::check(
            r,
            main_indexed,
            "the mixed-family TU's symbols are in the graph");
    }

    const cg::test::Registrar reg_benign_second_family{
        "graph.benign-error-second-family",
        &benign_error_second_family};
    const cg::test::Registrar reg_failure_fp{
        "graph.failure-fingerprint",
        &failure_fingerprint_gating};
    const cg::test::Registrar reg_failure_deps{
        "graph.failure-deps-retention",
        &failure_deps_retention};
    const cg::test::Registrar reg_failure_backoff{
        "graph.failure-backoff",
        &failure_transient_backoff};
    const cg::test::Registrar reg_index{"graph.index", &index_and_resolve};
    const cg::test::Registrar reg_tier1{
        "graph.tier1_scan_imports",
        &tier1_scan_imports_of_degraded_tus};
    const cg::test::Registrar reg_engine_faults{
        "graph.engine_fault_classes",
        &engine_fault_classes};
    const cg::test::Registrar reg_incremental{
        "graph.incremental",
        &incremental_replacement};
    const cg::test::Registrar reg_traverse{"graph.traverse", &traverse_walks};
    const cg::test::Registrar reg_persist{
        "graph.persistence",
        &file_db_persistence};
    const cg::test::Registrar reg_pool{
        "graph.pool-determinism",
        &parallel_pool_determinism};
    const cg::test::Registrar reg_uses_impact{
        "graph.uses-impact",
        &uses_record_impact};
    const cg::test::Registrar reg_family{"graph.family-merge", &family_merge};
    const cg::test::Registrar reg_ilink{
        "graph.internal-linkage",
        &internal_linkage_twins};
    const cg::test::Registrar reg_risk{"graph.risk-bands", &risk_bands};
    const cg::test::Registrar reg_changes{"graph.changes", &changes_walk};
    const cg::test::Registrar reg_cycles{"graph.cycles", &include_cycles_scc};

    /// 模块聚合结果是否含指定模块。
    [[nodiscard]]
    auto module_graph_has(
        const cg::graph::ModuleGraph& mg,
        const std::string&            m) -> bool
    {
        return std::ranges::find(mg.modules, m) != mg.modules.end();
    }

    // ccg graph 的数据面：调用闭包（BFS 最短深度、交叉边保留、截断诚实位）
    // 与模块级聚合（文件词干标签、同文件不出图、min-weight 过滤但计数从
    // 不被过滤截断）。
    void graph_export(R& r)
    {
        cg::test::check(
            r,
            cg::graph::module_label_of("src/cli/cli.parse.ixx") == "cli.parse"
                && cg::graph::module_label_of("a/b/util.h") == "util",
            "module label is the defining file's stem");

        db::GraphStore store{":memory:"};
        make_indexer(store, false).run(load_commands());
        const cg::db::SymId main_id = find_one(store, "main", r, "main");
        if (!main_id)
        {
            return;
        }

        const cg::graph::CallClosure c2 =
            cg::graph::call_closure(store, main_id, 2);
        std::unordered_set<cg::db::SymId> ids;
        for (const cg::graph::CallNode& n : c2.nodes)
        {
            ids.insert(n.id);
        }
        int                depth0 = 0;
        std::optional<int> render_all_depth;
        std::optional<int> area_all_depth;
        for (const cg::graph::CallNode& n : c2.nodes)
        {
            depth0 += n.depth == 0 ? 1 : 0;
            const std::string name = name_of(store, n.id);
            if (name == "app::render_all")
            {
                render_all_depth = n.depth;
            }
            if (name == "app::area_all")
            {
                area_all_depth = n.depth;
            }
        }
        cg::test::check(
            r,
            depth0 == 1,
            "closure carries exactly one depth-0 entry");
        cg::test::check(
            r,
            render_all_depth == 1 && area_all_depth == 2,
            "BFS shortest depths: render_all at 1, area_all at 2");
        cg::test::check(
            r,
            c2.truncated,
            "depth 2 caps a still-growing frontier (area_all fans onward)");
        bool ends_inside = !c2.edges.empty();
        bool entry_edge = false;
        for (const cg::graph::CallClosureEdge& e : c2.edges)
        {
            ends_inside =
                ends_inside && ids.contains(e.src) && ids.contains(e.dst);
            entry_edge = entry_edge || e.src == main_id;
        }
        cg::test::check(
            r,
            ends_inside,
            "every closure edge keeps both ends in the node set");
        cg::test::check(r, entry_edge, "the entry's out-edges are present");

        const cg::graph::CallClosure c6 =
            cg::graph::call_closure(store, main_id, 6);
        bool has_pi = false;
        for (const cg::graph::CallNode& n : c6.nodes)
        {
            has_pi = has_pi || name_of(store, n.id) == "shape::pi";
        }
        cg::test::check(
            r,
            has_pi && !c6.truncated,
            "depth 6 reaches shape::pi and the frontier rests");

        const cg::graph::CallClosure c0 =
            cg::graph::call_closure(store, main_id, 0);
        bool c0_has_pi = false;
        for (const cg::graph::CallNode& n : c0.nodes)
        {
            c0_has_pi = c0_has_pi || name_of(store, n.id) == "shape::pi";
        }
        cg::test::check(
            r,
            c0_has_pi && !c0.truncated && c0.nodes.size() >= c6.nodes.size(),
            "depth 0 = unlimited: full closure, never truncated");

        const cg::graph::ModuleGraph mg = cg::graph::module_graph(store, "", 1);
        cg::test::check(
            r,
            module_graph_has(mg, "main") && module_graph_has(mg, "canvas")
                && module_graph_has(mg, "circle")
                && module_graph_has(mg, "tmpl"),
            "module aggregation groups symbols by file stem");
        bool canvas_to_circle = false;
        bool self_loop_free = true;
        for (const cg::graph::ModuleGraphEdge& e : mg.edges)
        {
            self_loop_free = self_loop_free && e.from != e.to;
            canvas_to_circle =
                canvas_to_circle || (e.from == "canvas" && e.to == "circle");
        }
        cg::test::check(
            r,
            canvas_to_circle && self_loop_free,
            "cross-module aggregate edge exists, same-file calls stay out");
        cg::test::check(
            r,
            std::ranges::is_sorted(
                mg.edges,
                [](const cg::graph::ModuleGraphEdge& a,
                   const cg::graph::ModuleGraphEdge& b)
                { return a.from != b.from ? a.from < b.from : a.to < b.to; }),
            "module edges arrive deterministically sorted");

        const cg::graph::ModuleGraph filtered =
            cg::graph::module_graph(store, "", 100000);
        cg::test::check(
            r,
            filtered.edges.empty() && filtered.modules.empty()
                && filtered.edges_total > 0,
            "min-weight prunes edges and nodes, never the totals");

        const cg::graph::ModuleGraph scoped =
            cg::graph::module_graph(store, "circle", 1);
        cg::test::check(
            r,
            scoped.edges.empty(),
            "a single-file scope has no cross-module aggregate");
    }
    const cg::test::Registrar reg_graph_export{"graph.export", &graph_export};

} // namespace

// NOLINTNEXTLINE(clang-diagnostic-main-attached-to-named-module) 用 module-unit
// main 是有意为之：项目每个源文件都是具名模块单元；普通 TU
// 的 main 会破坏这项全模块政策
export int main()
{
    if (cg::util::cstd::get_env("CCG_DUMP"))
    {
        dump_db();
        return 0;
    }
    return cg::test::run_all();
}
