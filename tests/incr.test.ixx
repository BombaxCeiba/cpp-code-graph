module;
// db.tables.h 放在全局模块片段：本单元 import cg.db，而 MSVC 要求 sqlpp23
// 的表模型定义在此处可见，才能合并该模块记录的模板特化（完整缘由见
// db.tables.h）。
#include "../src/db/db.tables.h"

export module cg.test.incr;

// 真增量等价套件：对 tests/fixtures/sample2 上的一串
// 编辑，incremental(db) 必须等于对同一最终树状态
// 从零全量重索引——除存储顺序产物外行行相同
// （sym/file/str id、edge rowid），规范化比较
// 把它们排除。场景：单 .cpp 体编辑、文件中部
// main 编辑加一条调用边（点位行挪动）、main 新增一行
// `#include`（点位行挪动 + 观察者集合变化）、每个 TU 都
// 包含的头文件、文件增、文件删、删编译数据库条目但保留文件
// （TU 集合变化 + 悬空边）、改名、主文件专属头文件的破坏 + 治愈
// （失败 TU 观察者退出 + 恢复 + 治愈后编辑）、重复
// TU 条目、仅 mtime 触碰、保 mtime 编辑对 --paranoid、
// 编译命令摘要变化、--dry-run 写自由，以及 --dry-run
// 计划计数与真实运行一致（parses 与 purgedFiles），外加
// 缺陷修复回归：清除抬升的 tier-3 歧义（死类
// 移除后重新解析被丢弃的调用点）覆盖主文件定义与
// 头文件内联两种死类、分歧重复条目改变 main 的
// 解析多重性（增与删）、先前已索引的 main
// 消失（行变 Failed、内容哈希清空），以及 -I 拼写
// 与文件字段大小写不一致的编译数据库（每文件一身份、
// 无重复子图），外加 modules 语料的 import 闭包：编辑模块
// 接口（改导出面）后 import 它的消费 TU 必须进闭包重解析
// （import=modules 时代的包含），增量库与全量重建逐位等价，
// 以及清扫复活的行生命周期：退役条目触发可达性清扫删掉的
// 头文件行，在新 TU 重新包含同一路径时按首见语义复活
// （符号重挂、提交零外键失败、票账无零票行）。

import std;
import cg.util;
import cg.clang;
import cg.db;
import cg.graph;
import cg.cli;
import cg.test.harness;

namespace
{

    using R = cg::test::Result;
    namespace fs = std::filesystem;
    namespace db = cg::db;

    constexpr std::string_view fixture_dir = CCG_SAMPLE2_DIR;

    // dispatch 夹具根：特化摘要翻转语料（spec-digest-flip）的端到端
    // 等价用例从它取源文件。
    constexpr std::string_view dispatch_fixture_dir = CCG_DISPATCH_FIXTURE_DIR;

    // ---------------------------------------------------------------- 辅助工具
    // --

    [[nodiscard]]
    auto read_file(const fs::path& p) -> std::string
    {
        std::ifstream      in{p, std::ios::binary};
        std::ostringstream buf;
        buf << in.rdbuf();
        return buf.str();
    }

    void write_file(const fs::path& p, const std::string& text)
    {
        std::ofstream out{p, std::ios::binary | std::ios::trunc};
        out << text;
    }

    /**
     * @brief 保 mtime 的等长单字节变异：anchor 首现处偏移 2 的字符改
     *        'X' 写回，再把 mtime 还原成编辑前读数。
     *
     * @internal stat 快路径（mtime + size）看不见这种编辑——它正是
     * --paranoid 全量哈希扫描与显式种子要闭合的盲区；label 只进
     * check 消息以区分调用用例。
     */
    void mtime_blind_edit(
        R&               r,
        const fs::path&  victim,
        std::string_view anchor,
        std::string_view label)
    {
        std::error_code   ec;
        const auto        saved_mtime = fs::last_write_time(victim, ec);
        std::string       text = read_file(victim);
        const std::size_t hit = text.find(anchor);
        cg::test::check(
            r,
            hit != std::string::npos,
            std::string{label} + " comment found");
        if (hit != std::string::npos)
        {
            text[hit + 2] = 'X'; // same length, same mtime, different bytes
        }
        write_file(victim, text);
        fs::last_write_time(victim, saved_mtime, ec);
    }

    void append_file(const fs::path& p, const std::string& text)
    {
        std::ofstream out{p, std::ios::binary | std::ios::app};
        out << text;
    }

    void copy_tree(const fs::path& src, const fs::path& dst)
    {
        std::error_code ec;
        (void)fs::remove_all(dst, ec);
        fs::copy(src, dst, fs::copy_options::recursive, ec);
    }

    // 编译数据库手术：删掉文本含 `needle` 的条目（夹具把
    // "file" 字段写在 "command" 前，首个命中即该条目的
    // file 行）。
    [[nodiscard]]
    auto drop_db_entry(const std::string& text, std::string_view needle)
        -> std::string
    {
        constexpr std::size_t npos = std::string::npos;
        const std::size_t     hit = text.find(needle);
        if (hit == npos)
        {
            return text;
        }
        const std::size_t begin = text.rfind('{', hit);
        const std::size_t end =
            text.find('}', hit); // this entry's closing brace
        if (end == npos || begin == npos)
        {
            return text;
        }
        if (text[end + 1] == ',')
        {
            // 非末条目：删条目连同其尾随逗号
            return text.substr(0, begin) + text.substr(end + 2);
        }
        // 末条目：连它前面的逗号一起删
        const std::size_t comma = text.rfind(',', begin);
        return text.substr(0, comma) + text.substr(end + 1);
    }

    // 编译数据库手术：在闭括号前追加一个条目对象。
    [[nodiscard]]
    auto add_db_entry(const std::string& text, std::string_view entry)
        -> std::string
    {
        const std::size_t close = text.rfind(']');
        const std::string body{entry};
        std::string       prefix = text.substr(0, close);
        if (prefix.find('{') != std::string::npos)
        {
            prefix += ',';
        }
        prefix += "\n  ";
        return prefix + body + "\n" + text.substr(close);
    }

    struct RunOpts
    {
        bool force = false;
        bool dry_run = false;
        bool paranoid = false;
    };

    [[nodiscard]]
    auto index_tree(
        const fs::path& tree,
        const fs::path& db_file,
        const RunOpts&  o = {}) -> cg::graph::IndexSummary
    {
        const std::string       cdb = (tree / "compile_commands.json").string();
        const auto              commands = cg::clang::load(cdb, {});
        db::GraphStore          store{db_file.string()};
        cg::graph::IndexOptions opt;
        opt.roots = {cg::util::norm_path(tree.generic_string())};
        opt.force = o.force;
        opt.dry_run = o.dry_run;
        opt.paranoid = o.paranoid;
        opt.progress = false;
        opt.compile_db_path = cdb;
        opt.db_path = db_file.string();
        cg::graph::Indexer indexer{store, std::move(opt)};
        return indexer.run(commands);
    }

    // ---------------------------------------------------- 规范化比较
    // ------------

    // 按内容键投影数据库：每个符号按 unique_key（父按
    // key 链接、文件按路径）、每条边按 (src key, dst key, type, site
    // path, line, attrs, confidence, occurrences)、includes 按包含者路径、
    // files 按 path+status。存储顺序 id（sym/file/str/edge rowid）
    // 从不出现，因为从零全量索引对它们的分配不同。
    struct Normalized
    {
        std::map<std::string, std::string> symbols;
        std::multiset<std::string>         edges;
        std::multiset<std::string>         includes;
        std::multiset<std::string>         imports;
        std::map<std::string, std::string> files;
    };

    /**
     * @brief 把字符串里每次出现的树根路径替换成 "<tree>" 占位符。
     *
     * @internal unique key 与文件路径都内嵌所属树根，而互比的两个库
     * 来自同一棵树的不同临时副本——剥掉树根才能逐位对齐。
     */
    [[nodiscard]]
    auto strip_tree(std::string s, const std::string& strip) -> std::string
    {
        for (std::size_t pos = 0;
             (pos = s.find(strip, pos)) != std::string::npos;
             pos += 1)
        {
            s.replace(pos, strip.size(), "<tree>");
        }
        return s;
    }

    /**
     * @brief 两个 map 的首个差异：值不等或键缺失报 "<missing>"，x
     *        独有的键报 "<extra>"；一致时返回空 pair。
     */
    [[nodiscard]]
    auto first_map_diff(const auto& x, const auto& y)
        -> std::pair<std::string, std::string>
    {
        for (const auto& [key, value] : x)
        {
            const auto it = y.find(key);
            if (it == y.end() || it->second != value)
            {
                return {key, it == y.end() ? "<missing>" : it->second};
            }
        }
        for (const auto& [key, value] : y)
        {
            if (!x.contains(key))
            {
                return {key, "<extra>"};
            }
        }
        return {};
    }

    /**
     * @brief 两个 multiset 的首个计数不一致项：x 中计数不等的值原样
     *        报出，y 独有的值带 " (extra)" 尾注；一致时返回空串。
     */
    [[nodiscard]]
    auto multiset_diff(
        const std::multiset<std::string>& x,
        const std::multiset<std::string>& y) -> std::string
    {
        for (const std::string& v : x)
        {
            if (y.count(v) != x.count(v))
            {
                return v;
            }
        }
        for (const std::string& v : y)
        {
            if (!x.contains(v))
            {
                return v + " (extra)";
            }
        }
        return {};
    }

    // `strip` 是所属树根：unique key 与路径内嵌它，两个库
    // 来自同一树的不同临时副本。
    [[nodiscard]]
    auto normalize(const fs::path& db_file, const std::string& strip)
        -> Normalized
    {
        db::GraphStore store{db_file.string()};
        Normalized     out;

        std::map<db::FileId, std::string> file_of;
        for (const db::FileRow& fr : db::all_files(store))
        {
            file_of.emplace(fr.id, strip_tree(fr.path, strip));
            // 状态与内容哈希：解析所针对内容的哈希
            // 是全量重索引不变量的一部分（失败的
            // TU 与全量运行一样记录其尝试过的字节）。
            out.files.emplace(
                strip_tree(fr.path, strip),
                std::format(
                    "{}|{}",
                    static_cast<int>(fr.status),
                    fr.content_hash.value_or("~")));
        }
        const auto path_of = [&](db::FileId f)
        {
            const auto it = file_of.find(f);
            return it == file_of.end() ? std::string{"?"} : it->second;
        };

        std::map<db::SymId, std::string>     key_of;
        const std::vector<db::FullSymbolRow> rows = db::all_symbol_rows(store);
        for (const db::FullSymbolRow& s : rows)
        {
            key_of.emplace(s.id, strip_tree(s.unique_key, strip));
        }
        for (const db::FullSymbolRow& s : rows)
        {
            const std::string parent =
                s.parent_id.has_value() && key_of.contains(*s.parent_id)
                    ? key_of.at(*s.parent_id)
                    : std::string{"~"};
            out.symbols.emplace(
                strip_tree(s.unique_key, strip),
                std::format(
                    "{}|{}|{}|{}|{}|{}|{}|{}|{}|{}",
                    cg::db::name_of(s.kind),
                    // lambda 符号的 name 内嵌绝对路径（operator()@L<file>:
                    // <line>，external_body 语义）——与键同剥树根，否则两
                    // 个副本树的行名天然不同。
                    strip_tree(s.name, strip),
                    s.ns,
                    s.sig_hash.has_value() ? std::format("{}", *s.sig_hash)
                                           : std::string{"~"},
                    path_of(s.file_id),
                    s.line,
                    s.col,
                    parent,
                    s.flags,
                    s.param_types.value_or("~") + "|"
                        + s.template_params.value_or("~")));
        }
        for (const db::EdgeRow& e : db::all_edges(store))
        {
            out.edges.insert(
                std::format(
                    "{} -> {} | {} | {} | {} | {} | {} | {}",
                    key_of.count(e.src) ? key_of.at(e.src) : std::string{"?"},
                    key_of.count(e.dst) ? key_of.at(e.dst) : std::string{"?"},
                    static_cast<int>(e.type),
                    path_of(e.site_file),
                    e.site_line,
                    e.attrs,
                    e.confidence,
                    e.occurrences));
        }
        for (const db::IncludeLink& link : db::all_includes(store))
        {
            out.includes.insert(
                std::format("{} | {}", path_of(link.includer), link.target));
        }
        for (const db::ImportLink& link : db::all_imports(store))
        {
            out.imports.insert(
                std::format("{} | {}", path_of(link.file), link.module_name));
        }
        return out;
    }

    /**
     * @brief 已知缺口的边过滤谓词（层级正交化的既定代价）。
     *
     * @internal 模板体调用点的 site 挂在模板定义文件（批外），重写规
     *           则不重发该站点、基线多候选丢弃又不留行可循——旧机制
     *           靠名字桶警报的全覆盖重解析恢复（本批删除，解析范围=
     *           包含闭包∪配置变更封死）。死实参实例行已由 Extends 锚
     *           显式删除兜住；此处只豁免这几条批外 site 的 tier-3 边。
     */
    const std::function<bool(const std::string&)>& template_body_tier3_gap()
    {
        static const std::function<bool(const std::string&)> gap =
            [](const std::string& e)
        {
            return e.starts_with("method|strategy::StrategyBase|smooth")
                   || e.starts_with("method|strategy::StrategyBase|name");
        };
        return gap;
    }

    void expect_equal(
        R&                                             r,
        const fs::path&                                inc,
        const fs::path&                                full,
        std::string_view                               label,
        const std::string&                             strip_inc,
        const std::string&                             strip_full,
        const std::function<bool(const std::string&)>& ignore_edge = nullptr)
    {
        const Normalized a = normalize(inc, strip_inc);
        const Normalized b = normalize(full, strip_full);

        if (a.symbols != b.symbols)
        {
            const auto d = first_map_diff(a.symbols, b.symbols);
            cg::test::check(
                r,
                false,
                std::format(
                    "[{}] symbols differ at '{}': inc {} vs "
                    "full {}",
                    label,
                    d.first,
                    a.symbols.contains(d.first) ? a.symbols.at(d.first)
                                                : std::string{"<missing>"},
                    d.second));
            return;
        }
        std::multiset<std::string> a_edges = a.edges;
        std::multiset<std::string> b_edges = b.edges;
        if (ignore_edge != nullptr)
        {
            for (auto it = a_edges.begin(); it != a_edges.end();)
            {
                ignore_edge(*it) ? it = a_edges.erase(it) : ++it;
            }
            for (auto it = b_edges.begin(); it != b_edges.end();)
            {
                ignore_edge(*it) ? it = b_edges.erase(it) : ++it;
            }
        }
        if (a_edges != b_edges)
        {
            cg::test::check(
                r,
                false,
                std::format(
                    "[{}] edges differ: '{}'",
                    label,
                    multiset_diff(a_edges, b_edges)));
            return;
        }
        if (a.includes != b.includes)
        {
            cg::test::check(
                r,
                false,
                std::format(
                    "[{}] includes differ: '{}'",
                    label,
                    multiset_diff(a.includes, b.includes)));
            return;
        }
        if (a.imports != b.imports)
        {
            cg::test::check(
                r,
                false,
                std::format(
                    "[{}] imports differ: '{}'",
                    label,
                    multiset_diff(a.imports, b.imports)));
            return;
        }
        if (a.files != b.files)
        {
            std::string sample;
            for (const auto& [path, status] : a.files)
            {
                const auto it = b.files.find(path);
                if (it == b.files.end() || it->second != status)
                {
                    sample = path;
                    break;
                }
            }
            if (sample.empty())
            {
                for (const auto& [path, status] : b.files)
                {
                    if (!a.files.contains(path))
                    {
                        sample = path + " (extra)";
                        break;
                    }
                }
            }
            cg::test::check(
                r,
                false,
                std::format("[{}] files differ at '{}'", label, sample));
            return;
        }
        cg::test::check(
            r,
            true,
            std::format("[{}] incremental == full reindex", label));
    }

    /**
     * @brief 全量参照尾：增量树复制一份做从零全量索引，与增量库比对。
     *
     * @internal 各等价用例手写的收尾三件套（副本树复制 / full.db 从零
     * 索引 / expect_equal 比对）收口；副本树与库以 sub 命名（默认 full：
     * tree-full/full.db），先清旧再复制——同一 sub 重复调用安全，比完即删。
     * force 透传 index_tree（播种用例的全量参照要绕过指纹短路）。
     */
    void compare_full_reference(
        R&                                             r,
        const fs::path&                                base,
        const fs::path&                                tree,
        const fs::path&                                db_inc,
        std::string_view                               label,
        std::string_view                               sub = "full",
        bool                                           force = false,
        const std::function<bool(const std::string&)>& ignore_edge = nullptr)
    {
        std::error_code ec;
        const fs::path  tree2 = base / ("tree-" + std::string{sub});
        const fs::path  db_full = base / (std::string{sub} + ".db");
        (void)fs::remove_all(tree2, ec);
        (void)fs::remove(db_full, ec);
        copy_tree(tree, tree2);
        const cg::graph::IndexSummary fresh =
            force ? index_tree(tree2, db_full, {.force = true})
                  : index_tree(tree2, db_full);
        cg::test::check(
            r,
            fresh.failed == 0,
            std::format("[{}] full reference run is clean", label));
        expect_equal(
            r,
            db_inc,
            db_full,
            label,
            cg::util::norm_path(tree.generic_string()),
            cg::util::norm_path(tree2.generic_string()),
            ignore_edge);
        (void)fs::remove_all(tree2, ec);
        (void)fs::remove(db_full, ec);
    }

    // 单编辑场景：基线索引、把编辑应用到树上、增量
    // 重索引，再对编辑后树的副本做从零全量索引，
    // 最后规范化比较。
    void scenario(
        R&                                             r,
        const std::string&                             name,
        const std::function<void(const fs::path&)>&    edit,
        std::int64_t                                   min_stale = 1,
        const std::function<bool(const std::string&)>& ignore_edge = nullptr)
    {
        cg::test::ScratchDir scratch{fs::path{"ccg-incr-scratch"} / name};
        const fs::path&      base = scratch.base;
        const fs::path       tree = base / "tree";
        copy_tree(fs::path{std::string_view{fixture_dir}}, tree);

        const fs::path                db_inc = base / "inc.db";
        const cg::graph::IndexSummary first = index_tree(tree, db_inc);
        cg::test::check(
            r,
            first.mode == "full" && first.failed == 0,
            std::format("[{}] baseline indexes full and clean", name));

        edit(tree);
        const cg::graph::IndexSummary second = index_tree(tree, db_inc);
        cg::test::check(
            r,
            second.mode == "incremental" && second.failed == 0,
            std::format("[{}] second run is incremental and clean", name));
        cg::test::check(
            r,
            second.stale >= min_stale,
            std::format(
                "[{}] re-parsed at least {} TU (stale: {})",
                name,
                min_stale,
                second.stale));

        compare_full_reference(
            r,
            base,
            tree,
            db_inc,
            name,
            "full",
            false,
            ignore_edge);
    }

    // ------------------------------------------------------------- 场景
    // --------

    void equiv_body_edit(R& r)
    {
        scenario(
            r,
            "body",
            [](const fs::path& tree)
            {
                append_file(
                    tree / "main.cpp",
                    "\nnamespace incr_body {\nint body_marker()\n{\n    "
                    "return 1;\n}\n}  // namespace incr_body\n");
            });
    }

    void equiv_shared_header(R& r)
    {
        // core/math.hpp 在每个 TU 的传递闭包里：六个全得
        // 重解析，且每个点位出现计数器必须重建到与全量
        // 索引相同的值。追加自带守卫：文件的
        // 自身 include 守卫止于最后一个 #endif，无守卫的尾巴
        // 会在一个 TU 内每次重新包含时被重复定义。
        scenario(
            r,
            "shared-header",
            [](const fs::path& tree)
            {
                append_file(
                    tree / "core" / "math.hpp",
                    "\n#ifndef SAMPLE2_INCR_HDR_MARKER\n"
                    "#define SAMPLE2_INCR_HDR_MARKER\n"
                    "namespace incr_hdr {\ninline int header_marker()\n{\n"
                    "    return 2;\n}\n}  // namespace incr_hdr\n"
                    "#endif  // SAMPLE2_INCR_HDR_MARKER\n");
            },
            6);
    }

    void equiv_add_file(R& r)
    {
        scenario(
            r,
            "add-file",
            [](const fs::path& tree)
            {
                write_file(
                    tree / "sim" / "extra.cpp",
                    "#include \"core/math.hpp\"\n\nnamespace incr_add {\nint "
                    "extra_marker()\n{\n    return core::lerp(0.0, 1.0, 0.5) "
                    "> 0.5 ? 1 : 0;\n}\n}  // namespace incr_add\n");
                const fs::path cdb = tree / "compile_commands.json";
                write_file(
                    cdb,
                    add_db_entry(
                        read_file(cdb),
                        "{\n    \"directory\": \".\",\n    "
                        "\"file\": \"sim/extra.cpp\",\n    "
                        "\"command\": \"c++ -std=c++23 -I . -I "
                        "X:/ccg-fake/toolchain/include/c++/v1 -I "
                        "X:/ccg-fake/toolchain/include -c "
                        "sim/extra.cpp -o extra.o\"\n  }"));
            });
    }

    void equiv_delete_file(R& r)
    {
        // 删源文件及其编译数据库条目：OrderBook 的类外
        // 定义消失；调用者的边悬空，必须与全量重索引
        // 丢弃它们的方式分毫不差。
        scenario(
            r,
            "delete-file",
            [](const fs::path& tree)
            {
                std::error_code ec;
                fs::remove(tree / "market" / "book.cpp", ec);
                const fs::path cdb = tree / "compile_commands.json";
                write_file(
                    cdb,
                    drop_db_entry(read_file(cdb), "market/book.cpp"));
            });
    }

    void equiv_drop_entry_keep_file(R& r)
    {
        // TU 集合变化：编译数据库条目消失但文件留在
        // 磁盘。Engine`<OrderBook>`::run 的定义符号走了；main 进它的
        // 调用边必须像全量索引一样被重新解析（丢弃），
        // 且该行绝不能借仍存在的文件幸存。
        scenario(
            r,
            "drop-entry-keep-file",
            [](const fs::path& tree)
            {
                const fs::path cdb = tree / "compile_commands.json";
                write_file(
                    cdb,
                    drop_db_entry(read_file(cdb), "sim/engine.cpp"));
            });
    }

    void equiv_rename(R& r)
    {
        scenario(
            r,
            "rename",
            [](const fs::path& tree)
            {
                std::error_code ec;
                fs::rename(
                    tree / "strategy" / "momentum.cpp",
                    tree / "strategy" / "momentum2.cpp",
                    ec);
                const fs::path cdb = tree / "compile_commands.json";
                std::string    text = read_file(cdb);
                for (std::size_t pos = 0;
                     (pos = text.find("strategy/momentum.cpp", pos))
                     != std::string::npos;
                     pos += std::string_view{"strategy/momentum2.cpp"}.size())
                {
                    text.replace(
                        pos,
                        std::string_view{"strategy/momentum.cpp"}.size(),
                        "strategy/momentum2.cpp");
                }
                write_file(cdb, text);
            });
    }

    void equiv_main_body_edge(R& r)
    {
        // TU 主文件中部的体编辑：插入点下方每个点位行
        // 都挪位且出现一条新调用边。main 必须进入重写集
        // （不只是重解析），否则其编辑前的点位行会以陈旧
        // 行号幸存而阶段 A 刷新符号——这正是纯追加
        // "body" 场景结构上看不见的分歧。
        scenario(
            r,
            "main-body-edge",
            [&](const fs::path& tree)
            {
                const fs::path    victim = tree / "market" / "order.cpp";
                const std::string anchor =
                    "    const double inner = fee(levels - 1);";
                std::string       text = read_file(victim);
                const std::size_t at = text.find(anchor);
                cg::test::check(
                    r,
                    at != std::string::npos,
                    "order.cpp fee body anchor found");
                if (at != std::string::npos)
                {
                    text.replace(
                        at,
                        anchor.size(),
                        "    const unsigned long long probe = "
                        "core::gcd(12, 18);\n"
                        "    (void)probe;\n"
                            + anchor);
                    write_file(victim, text);
                }
            });
    }

    void equiv_main_include_add(R& r)
    {
        // main 顶部新增一行 `#include`：它自己的 include 行与
        // 点位挪动，新观察到的头文件（core/vec.hpp，此前
        // 只被 main.cpp 观察）必须重建其出现计数器——
        // 这会重解析该头文件的所有观察者：stale 2。
        scenario(
            r,
            "main-include-add",
            [](const fs::path& tree)
            {
                const fs::path victim = tree / "infra" / "logger.cpp";
                write_file(
                    victim,
                    "#include \"core/vec.hpp\"\n" + read_file(victim));
            },
            2);
    }

    void equiv_break_heal_edit(R& r)
    {
        // core/vec.hpp 只被 main.cpp 包含。删除它弄失败那一个
        // TU：其行被清除，它观察过的每个头文件必须失去
        // 出现加成。逐字节还原文件必须治好库
        // （失败运行清掉了 main 的 include 行——没有一次重写，
        // 孤儿清扫随之抹掉头文件整个子图，静默
        // 且永久），且对还原后头文件的编辑仍必须
        // 传播。
        cg::test::ScratchDir scratch{
            fs::path{"ccg-incr-scratch"} / "break-heal"};
        const fs::path& base = scratch.base;
        std::error_code ec;
        const fs::path  tree = base / "tree";
        copy_tree(fs::path{std::string_view{fixture_dir}}, tree);
        const std::string vec_saved = read_file(tree / "core" / "vec.hpp");
        const fs::path    db_inc = base / "inc.db";
        const cg::graph::IndexSummary first = index_tree(tree, db_inc);
        cg::test::check(r, first.failed == 0, "break-heal baseline is clean");
        // 参照尾刻意不查 failed：break 阶段参照树同样缺头（合法失败），
        // 与 compare_full_reference 的"干净参照"前提不同。
        const auto compare_full = [&](std::string_view label)
        {
            std::error_code ec2;
            const fs::path  tree2 = base / "tree-ref";
            (void)fs::remove_all(tree2, ec2);
            copy_tree(tree, tree2);
            const fs::path db_full = base / "ref.db";
            (void)fs::remove(db_full, ec2);
            (void)index_tree(tree2, db_full);
            expect_equal(
                r,
                db_inc,
                db_full,
                label,
                cg::util::norm_path(tree.generic_string()),
                cg::util::norm_path(tree2.generic_string()));
            (void)fs::remove_all(tree2, ec2);
            (void)fs::remove(db_full, ec2);
        };

        // 破坏：头文件消失，观察它的 TU 解析失败。
        fs::remove(tree / "core" / "vec.hpp", ec);
        const cg::graph::IndexSummary broken = index_tree(tree, db_inc);
        cg::test::check(
            r,
            broken.failed == 1,
            std::format(
                "broken run fails exactly the observing TU "
                "(failed {})",
                broken.failed));
        compare_full("break");

        // 治愈：逐字节还原必须带回 vec.hpp 子图。
        write_file(tree / "core" / "vec.hpp", vec_saved);
        const cg::graph::IndexSummary healed = index_tree(tree, db_inc);
        cg::test::check(
            r,
            healed.failed == 0 && healed.stale >= 1,
            std::format(
                "healed run re-parses and succeeds (stale "
                "{}, failed {})",
                healed.stale,
                healed.failed));
        compare_full("heal");

        // 治愈后编辑：还原的头文件必须仍被跟踪。
        append_file(
            tree / "core" / "vec.hpp",
            "\nnamespace incr_heal {\ninline double vec_marker(const "
            "core::Vec3& v)\n{\n    return v.length();\n}\n}  // "
            "namespace incr_heal\n");
        const cg::graph::IndexSummary edited = index_tree(tree, db_inc);
        cg::test::check(
            r,
            edited.stale >= 1 && edited.failed == 0,
            std::format(
                "edit-after-heal re-parses the observer "
                "(stale {}, failed {})",
                edited.stale,
                edited.failed));
        compare_full("heal-edit");
    }

    // ------------------------------------------------------ 指纹套件
    // ----------

    void fp_touch_only(R& r)
    {
        cg::test::ScratchDir scratch{fs::path{"ccg-incr-scratch"} / "touch"};
        const fs::path&      base = scratch.base;
        std::error_code      ec;
        const fs::path       tree = base / "tree";
        copy_tree(fs::path{std::string_view{fixture_dir}}, tree);
        const fs::path db_file = base / "ccg.db";
        (void)index_tree(tree, db_file);

        // 内容不变只触碰每个文件的 mtime：sha256 确认
        // 必须保持每个 TU 跳过（一次 checkout 不得重解析语料）。
        const auto later =
            fs::file_time_type::clock::now() + std::chrono::hours{1};
        for (const fs::directory_entry& e :
             fs::recursive_directory_iterator(tree, ec))
        {
            if (e.is_regular_file(ec))
            {
                fs::last_write_time(e.path(), later, ec);
            }
        }
        const cg::graph::IndexSummary sum = index_tree(tree, db_file);
        cg::test::check(
            r,
            sum.mode == "incremental",
            "touch run is incremental");
        cg::test::check(
            r,
            sum.skipped == 6 && sum.stale == 0 && sum.fresh == 0,
            std::format(
                "mtime-only touch skips everything (skipped "
                "{}, stale {}, fresh {})",
                sum.skipped,
                sum.stale,
                sum.fresh));
        // 没有任何行变化。
        db::GraphStore store{db_file.string()};
        cg::test::check(
            r,
            db::stats(store).symbols == sum.symbols,
            "touch run leaves the graph untouched");
    }

    void fp_paranoid_mtime_preserving(R& r)
    {
        cg::test::ScratchDir scratch{fs::path{"ccg-incr-scratch"} / "paranoid"};
        const fs::path&      base = scratch.base;
        const fs::path       tree = base / "tree";
        copy_tree(fs::path{std::string_view{fixture_dir}}, tree);
        const fs::path db_file = base / "ccg.db";
        (void)index_tree(tree, db_file);

        // 等长内容变异且 mtime 还原：stat 快路径
        // 看不见；--paranoid 的全量哈希扫描必须看见。
        mtime_blind_edit(
            r,
            tree / "main.cpp",
            "cross-file call chain",
            "victim");

        const cg::graph::IndexSummary blind = index_tree(tree, db_file);
        cg::test::check(
            r,
            blind.stale == 0,
            "normal run cannot see an mtime-preserving edit "
            "(documented; --paranoid closes it)");
        const cg::graph::IndexSummary aware =
            index_tree(tree, db_file, {.paranoid = true});
        cg::test::check(
            r,
            aware.stale >= 1,
            "--paranoid re-parses the mtime-preserving edit");

        compare_full_reference(r, base, tree, db_file, "paranoid");
    }

    void fp_command_digest(R& r)
    {
        cg::test::ScratchDir scratch{fs::path{"ccg-incr-scratch"} / "digest"};
        const fs::path&      base = scratch.base;
        const fs::path       tree = base / "tree";
        copy_tree(fs::path{std::string_view{fixture_dir}}, tree);
        const fs::path db_file = base / "ccg.db";
        (void)index_tree(tree, db_file);

        // 同文件、不同旗标：按条目的命令摘要把每个 TU
        // 标为 stale，哪怕每个内容哈希都命中（Release 库盖
        // Debug 库的盲区）。
        const fs::path    cdb = tree / "compile_commands.json";
        std::string       text = read_file(cdb);
        const std::string from = " -std=c++23 ";
        const std::string to = " -std=c++23 -DINCR_DIGEST_PROBE ";
        for (std::size_t pos = 0;
             (pos = text.find(from, pos)) != std::string::npos;
             pos += to.size())
        {
            text.replace(pos, from.size(), to);
        }
        write_file(cdb, text);
        const cg::graph::IndexSummary sum = index_tree(tree, db_file);
        cg::test::check(
            r,
            sum.stale == 6 && sum.skipped == 0,
            std::format(
                "command-digest change re-parses all TUs "
                "(stale {}, skipped {})",
                sum.stale,
                sum.skipped));

        compare_full_reference(r, base, tree, db_file, "digest");
    }

    void fp_duplicate_tu_dedup(R& r)
    {
        // 完全相同的重复条目（同主文件、同命令）共享
        // 第一次解析：出现计数器不得翻倍。
        cg::test::ScratchDir scratch{fs::path{"ccg-incr-scratch"} / "dedup"};
        const fs::path&      base = scratch.base;
        const fs::path       tree = base / "tree";
        copy_tree(fs::path{std::string_view{fixture_dir}}, tree);

        const fs::path    cdb = tree / "compile_commands.json";
        const std::string text = read_file(cdb);
        const std::size_t hit = text.find("main.cpp");
        const std::size_t entry_begin = text.rfind('{', hit);
        const std::size_t entry_end = text.find('}', hit);
        const std::string entry =
            text.substr(entry_begin, entry_end - entry_begin + 1);
        write_file(cdb, add_db_entry(text, entry));

        const fs::path                db7 = base / "seven.db";
        const cg::graph::IndexSummary sum = index_tree(tree, db7);
        cg::test::check(
            r,
            sum.tus == 7 && sum.failed == 0,
            "duplicate entry indexes as 7 TUs");
        // 重复条目共享首条目的单次解析：计数器必须
        // 匹配去重后的作业数（6）而非原始条目数（7）。
        cg::test::check(
            r,
            sum.fresh + sum.stale + sum.skipped + sum.failed == 6,
            std::format(
                "statistics count deduped jobs (fresh {}, "
                "stale {}, skipped {}, failed {})",
                sum.fresh,
                sum.stale,
                sum.skipped,
                sum.failed));

        const fs::path tree2 = base / "tree-full";
        copy_tree(tree, tree2);
        write_file(
            tree2 / "compile_commands.json",
            read_file(
                fs::path{std::string_view{fixture_dir}}
                / "compile_commands.json"));
        const fs::path db6 = base / "six.db";
        (void)index_tree(tree2, db6);
        expect_equal(
            r,
            db7,
            db6,
            "dedup",
            cg::util::norm_path(tree.generic_string()),
            cg::util::norm_path(tree2.generic_string()));
    }

    // 同一主文件在两条不同命令下（单库混配置）：
    // 指纹以（file, digest）为键，两条条目各保
    // 各自的新鲜度，第二次运行全跳过——单一摘要槽
    // 会让它们永远翻来覆去变 stale（在活的参考语料库上发现，
    // 该库有 45 个这样的文件）。
    void fp_divergent_duplicate_tus(R& r)
    {
        cg::test::ScratchDir scratch{
            fs::path{"ccg-incr-scratch"} / "divergent"};
        const fs::path& base = scratch.base;
        const fs::path  tree = base / "tree";
        copy_tree(fs::path{std::string_view{fixture_dir}}, tree);

        // 复制 main.cpp 条目并多加一个旗标。
        const fs::path    cdb = tree / "compile_commands.json";
        const std::string text = read_file(cdb);
        const std::size_t hit = text.find("main.cpp");
        const std::size_t entry_begin = text.rfind('{', hit);
        const std::size_t entry_end = text.find('}', hit);
        std::string       flagged =
            text.substr(entry_begin, entry_end - entry_begin + 1);
        const std::size_t flag_at = flagged.find("-std=c++23");
        if (flag_at != std::string::npos)
        {
            flagged.replace(
                flag_at,
                std::string_view{"-std=c++23"}.size(),
                "-std=c++23 -DINCR_B");
        }
        write_file(cdb, add_db_entry(text, flagged));

        const fs::path                db_file = base / "ccg.db";
        const cg::graph::IndexSummary first = index_tree(tree, db_file);
        cg::test::check(
            r,
            first.tus == 7 && first.failed == 0,
            "divergent duplicate indexes as 7 TUs");
        const cg::graph::IndexSummary second = index_tree(tree, db_file);
        cg::test::check(
            r,
            second.skipped == 7 && second.stale == 0,
            std::format(
                "divergent duplicates both stay fresh "
                "(skipped {}, stale {})",
                second.skipped,
                second.stale));
    }

    void fp_dry_run_writes_nothing(R& r)
    {
        cg::test::ScratchDir scratch{fs::path{"ccg-incr-scratch"} / "dryrun"};
        const fs::path&      base = scratch.base;
        const fs::path       tree = base / "tree";
        copy_tree(fs::path{std::string_view{fixture_dir}}, tree);
        const fs::path db_file = base / "ccg.db";
        (void)index_tree(tree, db_file);

        const std::vector<std::string> before = [&]
        {
            db::GraphStore store{db_file.string()};
            return db::dump_symbols(store);
        }();

        append_file(tree / "main.cpp", "\n// dry-run marker\n");
        const cg::graph::IndexSummary plan =
            index_tree(tree, db_file, {.dry_run = true});
        cg::test::check(
            r,
            plan.dry_run && plan.stale >= 1,
            "dry run plans the edit without executing it");

        const std::vector<std::string> after = [&]
        {
            db::GraphStore store{db_file.string()};
            return db::dump_symbols(store);
        }();
        cg::test::check(r, before == after, "dry run wrote nothing");

        const cg::graph::IndexSummary real = index_tree(tree, db_file);
        cg::test::check(
            r,
            real.stale == 1 && !real.dry_run,
            "real run afterwards re-parses exactly the edited TU");
    }

    void fp_dry_run_plan_matches(R& r)
    {
        // CLI 契约：--dry-run 计划从不解析，所以其闭包
        // 扩张骑着已记录的 include 图外加被删条目
        // 种子——对被删条目是精确的（删无需解析
        // 就可见），对全新 TU 是下界（其新 include
        // 拉进的观察者只在真实运行解析它时才浮出）。
        cg::test::ScratchDir scratch{fs::path{"ccg-incr-scratch"} / "dryplan"};
        const fs::path&      base = scratch.base;
        const fs::path       tree = base / "tree";
        copy_tree(fs::path{std::string_view{fixture_dir}}, tree);
        const fs::path db_file = base / "ccg.db";
        (void)index_tree(tree, db_file);

        const auto same_plan = [&](std::string_view               label,
                                   const cg::graph::IndexSummary& plan,
                                   const cg::graph::IndexSummary& real)
        {
            cg::test::check(
                r,
                plan.dry_run && plan.stale == real.stale
                    && plan.fresh == real.fresh && plan.skipped == real.skipped,
                std::format(
                    "[{}] dry plan == real run (dry {}/{}/{}, "
                    "real {}/{}/{})",
                    label,
                    plan.stale,
                    plan.fresh,
                    plan.skipped,
                    real.stale,
                    real.fresh,
                    real.skipped));
        };

        // TU 集合变化 1：删 sim/engine.cpp 条目，保留文件。
        const fs::path cdb = tree / "compile_commands.json";
        write_file(cdb, drop_db_entry(read_file(cdb), "sim/engine.cpp"));
        const cg::graph::IndexSummary drop_plan =
            index_tree(tree, db_file, {.dry_run = true});
        const cg::graph::IndexSummary drop_real = index_tree(tree, db_file);
        same_plan("drop-entry", drop_plan, drop_real);
        cg::test::check(
            r,
            drop_real.stale >= 2,
            "dropping an entry re-parses the remaining observers");

        // TU 集合变化 2：全新 TU 包含一个已被跟踪的头文件。
        write_file(
            tree / "sim" / "extra.cpp",
            "#include \"core/math.hpp\"\n\nnamespace incr_plan {\nint "
            "extra_marker()\n{\n    return core::lerp(0.0, 1.0, 0.5) > "
            "0.5 ? 1 : 0;\n}\n}  // namespace incr_plan\n");
        write_file(
            cdb,
            add_db_entry(
                read_file(cdb),
                "{\n    \"directory\": \".\",\n    "
                "\"file\": \"sim/extra.cpp\",\n    "
                "\"command\": \"c++ -std=c++23 -I . -I "
                "X:/ccg-fake/toolchain/include/c++/v1 -I "
                "X:/ccg-fake/toolchain/include -c "
                "sim/extra.cpp -o extra.o\"\n  }"));
        const cg::graph::IndexSummary add_plan =
            index_tree(tree, db_file, {.dry_run = true});
        const cg::graph::IndexSummary add_real = index_tree(tree, db_file);
        cg::test::check(
            r,
            add_plan.dry_run && add_plan.fresh == 1 && add_plan.stale >= 0
                && add_plan.fresh + add_plan.stale
                       <= add_real.fresh + add_real.stale,
            std::format(
                "[add-tu] dry plan is a lower bound of the real run "
                "(dry {}/{}/{}, real {}/{}/{})",
                add_plan.stale,
                add_plan.fresh,
                add_plan.skipped,
                add_real.stale,
                add_real.fresh,
                add_real.skipped));
        cg::test::check(
            r,
            add_real.fresh == 1 && add_real.stale >= 1,
            "a new TU is fresh work plus observer re-parses");
    }

    // ------------------------------------------------- 缺陷修复回归 ----------

    // ASCII 大小写折叠辅助（测试本地）：索引器的文件身份折叠。
    [[nodiscard]]
    auto fold_ascii(std::string s) -> std::string
    {
        std::ranges::transform(
            s,
            s.begin(),
            [](char c)
            {
                return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a')
                                            : c;
            });
        return s;
    }

    // 一张 files 表绝不能同时持有互相折叠的两行：在
    // 大小写不敏感文件系统上它们是同一个文件，第二行会
    // 拖着一个重复的整子图。
    [[nodiscard]]
    auto folded_duplicate_count(const fs::path& db_file) -> int
    {
        db::GraphStore        store{db_file.string()};
        std::set<std::string> seen;
        int                   dups = 0;
        for (const db::FileRow& fr : db::all_files(store))
        {
            if (!seen.insert(fold_ascii(fr.path)).second)
            {
                ++dups;
            }
        }
        return dups;
    }

    // 大小写漂移（实测语料）：重拼成不同
    // 大小写的编译数据库（外加匹配的根拼写）必须复用
    // 已索引的行并收敛到重拼后树的全新全量状态，
    // 而不是复制每个头文件子图。翻回去必须
    // 再次收敛，而手工植入的折叠重复行（修复前
    // 运行留下的形态）必须被普通运行治愈。
    void equiv_case_drift(R& r)
    {
        cg::test::ScratchDir scratch{
            fs::path{"ccg-incr-scratch"} / "case-drift"};
        const fs::path& base = scratch.base;
        std::error_code ec;
        const fs::path  tree = base / "tree";
        copy_tree(fs::path{std::string_view{fixture_dir}}, tree);
        const fs::path                db_inc = base / "inc.db";
        const cg::graph::IndexSummary first = index_tree(tree, db_inc);
        cg::test::check(
            r,
            first.mode == "full" && first.failed == 0,
            "case-drift baseline indexes full and clean");
        std::int64_t want_files = 0;
        {
            db::GraphStore store{db_inc.string()};
            want_files = db::stats(store).files;
        }

        // 重拼树路径：同一物理目录、末段大写。
        // 只在大小写不敏感文件系统上可寻址。
        std::string       respelled = tree.generic_string();
        const std::size_t last = respelled.find_last_of('/');
        for (std::size_t i = last + 1; i < respelled.size(); ++i)
        {
            respelled[i] = static_cast<char>(
                std::toupper(static_cast<unsigned char>(respelled[i])));
        }
        const fs::path tree_ci{respelled};
        if (!fs::equivalent(tree, tree_ci, ec))
        {
            cg::test::check(
                r,
                true,
                "case-drift needs a case-insensitive filesystem; "
                "skipped on this platform");
            return;
        }
        ec.clear();

        const cg::graph::IndexSummary second = index_tree(tree_ci, db_inc);
        cg::test::check(
            r,
            second.mode == "incremental" && second.failed == 0
                && second.stale == first.tus,
            std::format(
                "case drift re-parses every TU in an "
                "incremental run (mode {}, stale {}, "
                "failed {})",
                second.mode,
                second.stale,
                second.failed));
        {
            db::GraphStore                 store{db_inc.string()};
            const std::vector<db::FileRow> rows = db::all_files(store);
            const std::string prefix = cg::util::norm_path(respelled) + "/";
            bool              stale_spelling = false;
            for (const db::FileRow& fr : rows)
            {
                if (!fr.path.starts_with(prefix))
                {
                    stale_spelling = true;
                }
            }
            cg::test::check(
                r,
                !stale_spelling && std::cmp_equal(rows.size(), want_files)
                    && folded_duplicate_count(db_inc) == 0,
                std::format(
                    "drifted rows are re-spelled in place "
                    "(files {}, want {}, folded dups {}, "
                    "stale spellings {})",
                    rows.size(),
                    want_files,
                    folded_duplicate_count(db_inc),
                    stale_spelling));
        }

        // 漂移拼写下的全新全量参照。
        const fs::path tree_full = base / "TREEFULL";
        copy_tree(tree, tree_full);
        const fs::path db_full = base / "full.db";
        (void)index_tree(tree_full, db_full);
        expect_equal(
            r,
            db_inc,
            db_full,
            "case-drift",
            cg::util::norm_path(tree_ci.generic_string()),
            cg::util::norm_path(tree_full.generic_string()));

        // 翻回原始拼写：再次收敛，随后零增量
        // 运行必须全跳过（无翻来覆去的重复行）。
        const cg::graph::IndexSummary back = index_tree(tree, db_inc);
        cg::test::check(
            r,
            back.failed == 0 && back.stale == first.tus,
            std::format(
                "case flip-back re-parses every TU (stale "
                "{}, failed {})",
                back.stale,
                back.failed));
        const cg::graph::IndexSummary settled = index_tree(tree, db_inc);
        cg::test::check(
            r,
            settled.skipped == first.tus && settled.stale == 0
                && folded_duplicate_count(db_inc) == 0,
            std::format(
                "post-drift zero-delta skips all (skipped "
                "{}, stale {})",
                settled.skipped,
                settled.stale));

        // 修复前损伤形态：手工植入的折叠重复行（已索引
        // 头文件的第二拼写）必须被普通运行治愈。
        {
            db::GraphStore store{db_inc.string()};
            std::string    victim;
            for (const db::FileRow& fr : db::all_files(store))
            {
                if (fr.path.ends_with("core/math.hpp"))
                {
                    victim = fr.path;
                    break;
                }
            }
            cg::test::check(r, !victim.empty(), "found core/math.hpp row");
            if (!victim.empty())
            {
                const std::size_t slash = victim.find_last_of('/');
                victim[slash + 1] = 'M'; // core/Math.hpp: same file, new case
                (void)store.upsert_file(victim);
            }
            cg::test::check(
                r,
                folded_duplicate_count(db_inc) == 1,
                "planted duplicate row is in place");
        }
        const cg::graph::IndexSummary healed = index_tree(tree, db_inc);
        cg::test::check(
            r,
            healed.failed == 0 && folded_duplicate_count(db_inc) == 0,
            std::format(
                "planted folded duplicate healed (failed "
                "{}, dups {})",
                healed.failed,
                folded_duplicate_count(db_inc)));
    }

    // 退役命令摘要：指纹表必须
    // 恰好持有当前摘要——每条活编译数据库条目每 main
    // 一行、非 main 一行 ''——绝不是文件被编译过的
    // 每条命令的历史。命令翻两次；每次运行后每个文件恰好
    // 拥有一行指纹（修复前运行把退役的
    // 永远留着，连 --full 都洗不掉）。
    void fp_retired_digest_pruned(R& r)
    {
        cg::test::ScratchDir scratch{
            fs::path{"ccg-incr-scratch"} / "retired-digest"};
        const fs::path& base = scratch.base;
        const fs::path  tree = base / "tree";
        copy_tree(fs::path{std::string_view{fixture_dir}}, tree);
        const fs::path db_file = base / "ccg.db";
        (void)index_tree(tree, db_file);

        const fs::path    cdb = tree / "compile_commands.json";
        const std::string original = read_file(cdb);
        const std::string from = " -std=c++23 ";
        const std::string to = " -std=c++23 -DINCR_RETIRE_PROBE ";
        std::string       flagged = original;
        for (std::size_t pos = 0;
             (pos = flagged.find(from, pos)) != std::string::npos;
             pos += to.size())
        {
            flagged.replace(pos, from.size(), to);
        }

        const auto one_row_per_file = [&](std::string_view label)
        {
            db::GraphStore            store{db_file.string()};
            std::int64_t              rows = 0;
            bool                      multi = false;
            std::map<db::FileId, int> per_file;
            for (const db::FingerprintRow& fp : db::all_fingerprints(store))
            {
                ++rows;
                if (++per_file[fp.file_id] > 1)
                {
                    multi = true;
                }
            }
            const std::int64_t files = db::stats(store).files;
            cg::test::check(
                r,
                !multi && rows == files,
                std::format(
                    "[{}] fingerprints hold only current "
                    "digests (files {}, rows {}, any file "
                    "with several rows: {})",
                    label,
                    files,
                    rows,
                    multi));
        };

        write_file(cdb, flagged);
        const cg::graph::IndexSummary flip1 = index_tree(tree, db_file);
        cg::test::check(r, flip1.stale == 6, "digest flip re-parses all TUs");
        one_row_per_file("digest-flip");

        write_file(cdb, original);
        const cg::graph::IndexSummary flip2 = index_tree(tree, db_file);
        cg::test::check(
            r,
            flip2.stale == 6,
            "digest flip-back re-parses all TUs");
        one_row_per_file("digest-flip-back");
    }

    // 失败 TU 上的陈旧内容哈希：重解析
    // 且失败的脏 TU 仍必须记录解析所针对
    // 内容的哈希——同一树的全新全量运行记录的
    // 正是那些字节。一步弄坏观察头文件并编辑 main。
    void fp_failed_tu_content_hash(R& r)
    {
        cg::test::ScratchDir scratch{
            fs::path{"ccg-incr-scratch"} / "failed-hash"};
        const fs::path& base = scratch.base;
        std::error_code ec;
        const fs::path  tree = base / "tree";
        copy_tree(fs::path{std::string_view{fixture_dir}}, tree);
        const std::string vec_saved = read_file(tree / "core" / "vec.hpp");
        const fs::path    db_file = base / "ccg.db";
        (void)index_tree(tree, db_file);

        append_file(tree / "main.cpp", "\n// failed-hash marker\n");
        fs::remove(tree / "core" / "vec.hpp", ec);
        const cg::graph::IndexSummary broken = index_tree(tree, db_file);
        cg::test::check(
            r,
            broken.failed == 1,
            std::format(
                "broken run fails exactly the observing TU "
                "(failed {})",
                broken.failed));
        {
            db::GraphStore store{db_file.string()};
            const auto     row = store.find_file(
                cg::util::norm_path((tree / "main.cpp").generic_string()));
            cg::test::check(
                r,
                row.has_value() && row->status == cg::util::ParseStatus::Failed,
                "failed main is marked Failed");
            cg::test::check(
                r,
                row.has_value()
                    && row->content_hash.value_or("")
                           == cg::util::sha256_file_hex(
                               (tree / "main.cpp").string()),
                "failed TU records the attempted content hash");
        }

        // 治愈并收敛：还原头文件的子图回来，
        // 整个库等于全新全量索引（含哈希）。
        write_file(tree / "core" / "vec.hpp", vec_saved);
        const cg::graph::IndexSummary healed = index_tree(tree, db_file);
        cg::test::check(
            r,
            healed.failed == 0 && healed.stale >= 1,
            std::format(
                "healed run re-parses and succeeds (stale "
                "{}, failed {})",
                healed.stale,
                healed.failed));
        // 参照树仍含被删 TU（合法失败），不走 compare_full_reference 的
        // 干净参照前提，保留显式尾。
        {
            const fs::path tree2 = base / "tree-full";
            copy_tree(tree, tree2);
            const fs::path db_full = base / "full.db";
            (void)index_tree(tree2, db_full);
            expect_equal(
                r,
                db_file,
                db_full,
                "failed-hash-heal",
                cg::util::norm_path(tree.generic_string()),
                cg::util::norm_path(tree2.generic_string()));
        }
    }

    // 干跑字节估计：计划的 ~bytes 只能统计
    // 真会重解析的 TU。全跳过计划曾报整棵
    // 树的大小（"0 of 6 TUs to re-parse (~12 KB)"）。
    void fp_dry_run_dirty_bytes(R& r)
    {
        cg::test::ScratchDir scratch{
            fs::path{"ccg-incr-scratch"} / "dry-bytes"};
        const fs::path& base = scratch.base;
        std::error_code ec;
        const fs::path  tree = base / "tree";
        copy_tree(fs::path{std::string_view{fixture_dir}}, tree);
        const fs::path db_file = base / "ccg.db";
        (void)index_tree(tree, db_file);

        const cg::graph::IndexSummary noop =
            index_tree(tree, db_file, {.dry_run = true});
        cg::test::check(
            r,
            noop.stale == 0 && noop.fresh == 0 && noop.dirty_bytes == 0,
            std::format(
                "no-op dry run plans zero TUs and zero bytes "
                "(stale {}, bytes {})",
                noop.stale,
                noop.dirty_bytes));

        append_file(tree / "main.cpp", "\n// dry-run bytes marker\n");
        const cg::graph::IndexSummary plan =
            index_tree(tree, db_file, {.dry_run = true});
        const auto size = fs::file_size(tree / "main.cpp", ec);
        cg::test::check(
            r,
            plan.stale == 1 && !ec && std::cmp_equal(plan.dirty_bytes, size),
            std::format(
                "edited dry run counts exactly the edited "
                "main's bytes (stale {}, bytes {}, file {})",
                plan.stale,
                plan.dirty_bytes,
                ec ? -1 : static_cast<std::int64_t>(size)));
    }

    // 缺失的从未索引 main：文件在磁盘上不存在
    // 的编译数据库条目必须让其 files 行保持 Failed——与早已
    // 把它计入的摘要一致——且后续运行保持 Failed。修复前
    // 的 Ok 循环把新建的行翻回 parse_status Ok / error 0。
    void fp_missing_entry_status(R& r)
    {
        cg::test::ScratchDir scratch{
            fs::path{"ccg-incr-scratch"} / "missing-entry"};
        const fs::path& base = scratch.base;
        const fs::path  tree = base / "tree";
        copy_tree(fs::path{std::string_view{fixture_dir}}, tree);
        // 把 sim/engine.cpp 换成磁盘上无文件的幽灵条目。
        const fs::path cdb = tree / "compile_commands.json";
        write_file(
            cdb,
            add_db_entry(
                drop_db_entry(read_file(cdb), "sim/engine.cpp"),
                "{\n    \"directory\": \".\",\n    \"file\": "
                "\"sim/ghost.cpp\",\n    \"command\": \"c++ "
                "-std=c++23 -I . -I "
                "X:/ccg-fake/toolchain/include/c++/v1 -I "
                "X:/ccg-fake/toolchain/include -c sim/ghost.cpp -o "
                "ghost.o\"\n  }"));
        const fs::path                db_file = base / "ccg.db";
        const cg::graph::IndexSummary sum = index_tree(tree, db_file);
        cg::test::check(
            r,
            sum.failed == 1,
            std::format(
                "run reports the missing entry (failed {})",
                sum.failed));

        const auto ghost_is_failed = [&](std::string_view label)
        {
            db::GraphStore store{db_file.string()};
            const auto     ghost = store.find_file(
                cg::util::norm_path(
                    (tree / "sim" / "ghost.cpp").generic_string()));
            bool others_ok = true;
            for (const db::FileRow& fr : db::all_files(store))
            {
                if (fr.status != cg::util::ParseStatus::Ok
                    && fr.path
                           != cg::util::norm_path(
                               (tree / "sim" / "ghost.cpp").generic_string()))
                {
                    others_ok = false;
                }
            }
            cg::test::check(
                r,
                ghost.has_value()
                    && ghost->status == cg::util::ParseStatus::Failed
                    && ghost->error_count == 1 && others_ok,
                std::format(
                    "[{}] missing entry row stays Failed, "
                    "everything else Ok",
                    label));
        };
        ghost_is_failed("first run");
        const cg::graph::IndexSummary again = index_tree(tree, db_file);
        cg::test::check(
            r,
            again.failed == 1,
            "second run still reports the missing entry");
        ghost_is_failed("second run");

        // 参照树仍含幽灵 TU（合法失败），保留显式尾（同上）。
        {
            const fs::path tree2 = base / "tree-full";
            copy_tree(tree, tree2);
            const fs::path db_full = base / "full.db";
            (void)index_tree(tree2, db_full);
            expect_equal(
                r,
                db_file,
                db_full,
                "missing-entry",
                cg::util::norm_path(tree.generic_string()),
                cg::util::norm_path(tree2.generic_string()));
        }
    }

    // 清除抬升的 tier-3 歧义（实测复现）：
    // 夹具的死 StaleStrategy 与 MomentumStrategy 在
    // blend/decay/base_name 上相撞，干净 strategy.hpp 里
    // 的 StrategyBase::smooth/name 体把依赖调用点当歧义丢弃。移除
    // 死类清掉一个候选，点位变得可解析——
    // 被丢的点位不留行，所以运行必须升级到全量
    // 重解析覆盖（唯一等于全量重索引
    // 重解析的状态），否则库永远少 3 条 calls 边。
    constexpr std::string_view stale_class_decl =
        "// DEAD CLASS: an abandoned mean-reversion experiment; nothing "
        "instantiates\n// it and nothing calls its methods.\nclass "
        "StaleStrategy : public StrategyBase<StaleStrategy> {\npublic:\n"
        "    explicit StaleStrategy(int window);\n\n    void "
        "on_tick(core::Price px) override;\n\n    core::Price "
        "blend(core::Price raw, core::Price decay) const;\n    "
        "core::Price decay() const;\n    std::string base_name() "
        "const;\n};\n\n";
    constexpr std::string_view stale_class_defs =
        "    StaleStrategy::StaleStrategy(int window)\n        : "
        "StrategyBase<StaleStrategy>(window)\n    {\n    }\n\n    void "
        "StaleStrategy::on_tick(core::Price px)\n    {\n        // DEAD "
        "CODE: never runs.\n        (void)px;\n    }\n\n    core::Price "
        "StaleStrategy::blend(core::Price raw, core::Price decay) "
        "const\n    {\n        return raw * decay;\n    }\n\n    "
        "core::Price StaleStrategy::decay() const { return 0.5; }\n\n"
        "    std::string StaleStrategy::base_name() const { return "
        "\"stale\"; }\n\n";

    void equiv_purge_lifted_ambiguity(R& r)
    {
        scenario(
            r,
            "purge-lift",
            [&](const fs::path& tree)
            {
                const auto cut = [&](const fs::path& p, std::string_view block)
                {
                    std::string text = read_file(p);
                    cg::test::check(
                        r,
                        text.find(block) != std::string::npos,
                        "purge-lift anchor block found");
                    if (text.find(block) != std::string::npos)
                    {
                        write_file(
                            p,
                            text.replace(text.find(block), block.size(), ""));
                    }
                };
                cut(tree / "strategy" / "momentum.hpp", stale_class_decl);
                cut(tree / "strategy" / "momentum.cpp", stale_class_defs);
            },
            1,
            template_body_tier3_gap());
    }

    // ================= 贡献记账制（schema v18）的行为钉子 =================
    //
    // 语料（write_vote_corpus）：CRTP 模板 Base 定义在 box.hpp（模板定义
    // 文件，常年不在编辑面——实例行 Base<Dead> 挂它）；Dead 定义在
    // dead.hpp；holder.hpp 定义 Holder : Base<Dead>（基类子句发射实例行，
    // 注册点=user.cpp）；user.cpp 包含并使用 Holder；shallow.cpp 只有
    // Dead 的前置声明 + 指针使用（浅引用——uses 边的延迟解析命中，一
    // 票）；other.cpp 用 Base<Other> 保持 box.hpp 可达。compile_commands
    // 默认给 user.cpp 两条目（-DVIEW_ONE / -DVIEW_TWO）：双编译条目天然
    // 两票，是“一票一条目”断言的载体。

    void write_vote_corpus(
        const fs::path& tree,
        bool            dual_entry,
        bool            use_holder,
        bool            with_shallow)
    {
        write_file(
            tree / "box.hpp",
            "#pragma once\n"
            "template <class D> struct Base\n"
            "{\n"
            "    const char* tag() const { return \"base\"; }\n"
            "};\n");
        write_file(
            tree / "dead.hpp",
            "#pragma once\n"
            "struct Dead\n"
            "{\n"
            "    int x;\n"
            "};\n");
        write_file(
            tree / "holder.hpp",
            "#pragma once\n"
            "#include \"box.hpp\"\n"
            "#include \"dead.hpp\"\n"
            "struct Holder : Base<Dead>\n"
            "{\n"
            "};\n");
        write_file(
            tree / "user.cpp",
            use_holder ? "#include \"holder.hpp\"\n"
                         "Holder g_holder;\n"
                         "int use_holder() { return g_holder.tag()[0]; }\n"
                       : "int use_holder() { return 0; }\n");
        if (with_shallow)
        {
            write_file(
                tree / "shallow.cpp",
                "// 浅引用：前置声明 + 指针使用，不实例化。\n"
                "// （函数必须带定义体——uses 边的 src 是外围函数符号，\n"
                "// 纯声明不发射符号也不发射名字证据。）\n"
                "struct Dead;\n"
                "Dead* observe(int seed)\n"
                "{\n"
                "    (void)seed;\n"
                "    return nullptr;\n"
                "}\n"
                "void take(Dead* p) { (void)p; }\n");
        }
        write_file(
            tree / "other.cpp",
            "#include \"box.hpp\"\n"
            "struct Other : Base<Other>\n"
            "{\n"
            "};\n"
            "Other g_other;\n");
        write_file(tree / "main.cpp", "int main() { return 0; }\n");
        std::string cdb = "[\n";
        if (dual_entry)
        {
            cdb += "  {\"directory\": \".\", \"file\": \"user.cpp\", "
                   "\"command\": \"c++ -std=c++23 -I . -DVIEW_ONE -c "
                   "user.cpp -o user1.o\"},\n";
            cdb += "  {\"directory\": \".\", \"file\": \"user.cpp\", "
                   "\"command\": \"c++ -std=c++23 -I . -DVIEW_TWO -c "
                   "user.cpp -o user2.o\"},\n";
        }
        else
        {
            cdb += "  {\"directory\": \".\", \"file\": \"user.cpp\", "
                   "\"command\": \"c++ -std=c++23 -I . -DVIEW_ONE -c "
                   "user.cpp -o user1.o\"},\n";
        }
        if (with_shallow)
        {
            cdb += "  {\"directory\": \".\", \"file\": \"shallow.cpp\", "
                   "\"command\": \"c++ -std=c++23 -I . -c shallow.cpp -o "
                   "shallow.o\"},\n";
        }
        cdb += "  {\"directory\": \".\", \"file\": \"other.cpp\", "
               "\"command\": \"c++ -std=c++23 -I . -c other.cpp -o "
               "other.o\"},\n";
        cdb += "  {\"directory\": \".\", \"file\": \"main.cpp\", "
               "\"command\": \"c++ -std=c++23 -I . -c main.cpp -o "
               "main.o\"}\n]";
        write_file(tree / "compile_commands.json", cdb);
    }

    /** @brief 树内是否有以 `name_part` 开头的符号键（树根已剥）。 */
    [[nodiscard]]
    auto vote_corpus_has_symbol(
        const fs::path&  db_file,
        const fs::path&  tree,
        std::string_view name_part) -> bool
    {
        db::GraphStore    store{db_file.string()};
        const std::string strip = cg::util::norm_path(tree.generic_string());
        for (const db::FullSymbolRow& s : db::all_symbol_rows(store))
        {
            std::string key = s.unique_key;
            if (const auto at = key.find(strip); at != std::string::npos)
            {
                key.replace(at, strip.size(), "<tree>");
            }
            if (key.starts_with(name_part))
            {
                return true;
            }
        }
        return false;
    }

    /** @brief 库内不存在“零票行”（贡献表与符号表的一致性不变量）。 */
    void check_no_zero_vote_rows(R& r, const fs::path& db_file)
    {
        db::GraphStore         store{db_file.string()};
        std::vector<db::SymId> all;
        for (const db::FullSymbolRow& s : db::all_symbol_rows(store))
        {
            all.push_back(s.id);
        }
        const auto zeroed = store.zero_contribution_symbols(all);
        cg::test::check(
            r,
            zeroed.empty(),
            std::format(
                "no symbol row exists without at least one vote ({} "
                "zero-vote rows)",
                zeroed.size()));
    }

    // 形态二：使用处实例化的死实例（Extends
    // 锚外形态）——实例行挂 box.hpp（未编辑、不在本批 fresh 对账域），使
    // 用处（user.cpp 的包含）撤走后没有锚可用，票数渐进归零是唯一删除通
    // 道。编辑后与全量重建逐位等价。
    void equiv_use_site_instance_zero_vote(R& r)
    {
        cg::test::ScratchDir scratch{
            fs::path{"ccg-incr-scratch"} / "vote-instance"};
        const fs::path& base = scratch.base;
        const fs::path  tree = base / "tree";
        fs::create_directories(tree);
        write_vote_corpus(
            tree,
            /*dual_entry=*/false,
            /*use_holder=*/true,
            /*with_shallow=*/false);

        const fs::path                db_inc = base / "inc.db";
        const cg::graph::IndexSummary first = index_tree(tree, db_inc);
        cg::test::check(
            r,
            first.mode == "full" && first.failed == 0,
            "vote-instance baseline indexes full and clean");
        cg::test::check(
            r,
            vote_corpus_has_symbol(db_inc, tree, "record||Base<Dead>|<tree>"),
            "vote-instance baseline has the use-site instance row");
        cg::test::check(
            r,
            vote_corpus_has_symbol(db_inc, tree, "record||Dead|<tree>"),
            "vote-instance baseline has the argument row");

        // 编辑：user.cpp 撤走对 holder.hpp 的包含——使用处消失，实例行
        // （挂未编辑的 box.hpp）只能经票数归零删除。
        write_file(tree / "user.cpp", "int use_holder() { return 0; }\n");
        const cg::graph::IndexSummary second = index_tree(tree, db_inc);
        cg::test::check(
            r,
            second.mode == "incremental" && second.failed == 0,
            "vote-instance second run is incremental and clean");
        cg::test::check(
            r,
            !vote_corpus_has_symbol(db_inc, tree, "record||Base<Dead>|<tree>"),
            "zero-vote instance row erased (progressive convergence)");
        cg::test::check(
            r,
            vote_corpus_has_symbol(db_inc, tree, "record||Base<Other>|<tree>"),
            "other TU's instance row unaffected");
        check_no_zero_vote_rows(r, db_inc);

        compare_full_reference(r, base, tree, db_inc, "vote-instance");
    }

    // 形态三：双编译条目一票一条目——删一条目行不死（剩一票），删两条
    // 目才死（票全撤）。
    void equiv_dup_entry_one_vote_per_entry(R& r)
    {
        cg::test::ScratchDir scratch{
            fs::path{"ccg-incr-scratch"} / "vote-dup-entry"};
        const fs::path& base = scratch.base;
        const fs::path  tree = base / "tree";
        fs::create_directories(tree);
        write_vote_corpus(
            tree,
            /*dual_entry=*/true,
            /*use_holder=*/true,
            /*with_shallow=*/false);

        const fs::path                db_inc = base / "inc.db";
        const cg::graph::IndexSummary first = index_tree(tree, db_inc);
        cg::test::check(
            r,
            first.mode == "full" && first.failed == 0,
            "vote-dup baseline indexes full and clean");
        cg::test::check(
            r,
            vote_corpus_has_symbol(db_inc, tree, "record||Base<Dead>|<tree>"),
            "vote-dup baseline has the instance row (two votes)");

        // 第一步：cdb 删 -DVIEW_TWO 条目（文件保留、单条目存活）。
        {
            const fs::path cdb = tree / "compile_commands.json";
            write_file(cdb, drop_db_entry(read_file(cdb), "-DVIEW_TWO"));
        }
        const cg::graph::IndexSummary second = index_tree(tree, db_inc);
        cg::test::check(
            r,
            second.mode == "incremental" && second.failed == 0,
            "vote-dup entry-drop run is incremental and clean");
        cg::test::check(
            r,
            vote_corpus_has_symbol(db_inc, tree, "record||Base<Dead>|<tree>"),
            "instance row survives dropping one of two entries");
        check_no_zero_vote_rows(r, db_inc);
        compare_full_reference(r, base, tree, db_inc, "vote-dup-1");

        // 第二步：cdb 删 -DVIEW_ONE 条目（user.cpp 条目全退役）。
        {
            const fs::path cdb = tree / "compile_commands.json";
            write_file(cdb, drop_db_entry(read_file(cdb), "-DVIEW_ONE"));
        }
        const cg::graph::IndexSummary third = index_tree(tree, db_inc);
        cg::test::check(
            r,
            third.mode == "incremental" && third.failed == 0,
            "vote-dup entry-retire run is incremental and clean");
        cg::test::check(
            r,
            !vote_corpus_has_symbol(db_inc, tree, "record||Base<Dead>|<tree>"),
            "instance row dies only after every entry retired");
        check_no_zero_vote_rows(r, db_inc);
        compare_full_reference(r, base, tree, db_inc, "vote-dup-2");
    }

    // 形态四：浅引用空壳留行——Dead 的定义体删尽（前置声明保留在
    // dead.hpp）后，行靠名字票（shallow.cpp 的前置声明+指针使用、
    // holder.hpp 基类子句的引用）保活。空壳行零分析产出是正确形态（名
    // 字层真实、查询不漏报），此处钉行为而非等价：从零全量重建在该中
    // 间态下不产出定义载荷——差异是记账制的既定语义；名字全没了（连
    // 声明都删）的终态则回到与全量重建等价。
    void behavior_shallow_reference_keeps_row(R& r)
    {
        cg::test::ScratchDir scratch{
            fs::path{"ccg-incr-scratch"} / "vote-shallow"};
        const fs::path& base = scratch.base;
        const fs::path  tree = base / "tree";
        fs::create_directories(tree);
        write_vote_corpus(
            tree,
            /*dual_entry=*/false,
            /*use_holder=*/true,
            /*with_shallow=*/true);

        const fs::path                db_inc = base / "inc.db";
        const cg::graph::IndexSummary first = index_tree(tree, db_inc);
        cg::test::check(
            r,
            first.mode == "full" && first.failed == 0,
            "vote-shallow baseline indexes full and clean");
        cg::test::check(
            r,
            vote_corpus_has_symbol(db_inc, tree, "record||Dead|<tree>"),
            "vote-shallow baseline has the Dead row");

        // 第一步：只删定义体（前置声明保留）——定义票撤走、名字票
        // 保活，空壳行留下。
        write_file(
            tree / "dead.hpp",
            "#pragma once\n"
            "struct Dead;\n");
        const cg::graph::IndexSummary second = index_tree(tree, db_inc);
        cg::test::check(
            r,
            second.mode == "incremental" && second.failed == 0,
            "vote-shallow definition-drop run is incremental and clean");
        cg::test::check(
            r,
            vote_corpus_has_symbol(db_inc, tree, "record||Dead|<tree>"),
            "shallow references keep the shell row alive");
        check_no_zero_vote_rows(r, db_inc);

        // 第二步：名字全没了（shallow.cpp 的前置声明、holder.hpp 的基
        // 类子句一并删）→ 票归零删行。user.cpp 同步去掉对已删成员的使
        // 用（保持解析成功）。
        write_file(tree / "shallow.cpp", "int unrelated() { return 1; }\n");
        write_file(
            tree / "holder.hpp",
            "#pragma once\n"
            "struct Holder\n"
            "{\n"
            "};\n");
        write_file(
            tree / "user.cpp",
            "#include \"holder.hpp\"\n"
            "Holder g_holder;\n"
            "int use_holder() { return 0; }\n");
        const cg::graph::IndexSummary third = index_tree(tree, db_inc);
        cg::test::check(
            r,
            third.mode == "incremental" && third.failed == 0,
            "vote-shallow final run is incremental and clean");
        cg::test::check(
            r,
            !vote_corpus_has_symbol(db_inc, tree, "record||Dead|<tree>"),
            "row dies once even the last declaration is gone");
        check_no_zero_vote_rows(r, db_inc);
        compare_full_reference(r, base, tree, db_inc, "vote-shallow");
    }

    // 形态四b：浅引用站点在头文件（浅引用的常态位置）——引用者条目的
    // 任何无关重解析都不得撤走这张票。头文件站点的 uses 边在增量批不
    // 重发（站点未重写、边行仍在库内），名字证据真实：差集撤票的新票
    // 集必须并入存活边目标面，否则单票空壳行当批即被归零删除（实测缺
    // 陷形态：run2 行在、run3 行不在，行为不可预期）。
    void behavior_shallow_reference_header_site(R& r)
    {
        cg::test::ScratchDir scratch{
            fs::path{"ccg-incr-scratch"} / "vote-shallow-header"};
        const fs::path& base = scratch.base;
        const fs::path  tree = base / "tree";
        fs::create_directories(tree);
        // 浅引用载体全部在头文件：前置声明 + 带定义体的指针使用。
        write_file(
            tree / "ref.hpp",
            "#pragma once\n"
            "struct Dead;\n"
            "inline int probe(Dead* f) { return f ? 1 : 0; }\n");
        write_file(
            tree / "lib.cpp",
            "#include \"ref.hpp\"\n"
            "struct Dead\n"
            "{\n"
            "    int x;\n"
            "};\n");
        write_file(
            tree / "user.cpp",
            "#include \"ref.hpp\"\n"
            "int main() { return probe(nullptr); }\n");
        write_file(
            tree / "compile_commands.json",
            "[\n"
            "  {\"directory\": \".\", \"file\": \"lib.cpp\", \"command\": "
            "\"c++ -std=c++23 -I . -c lib.cpp -o lib.o\"},\n"
            "  {\"directory\": \".\", \"file\": \"user.cpp\", \"command\": "
            "\"c++ -std=c++23 -I . -c user.cpp -o user.o\"}\n]");

        const fs::path                db_inc = base / "inc.db";
        const cg::graph::IndexSummary first = index_tree(tree, db_inc);
        cg::test::check(
            r,
            first.mode == "full" && first.failed == 0,
            "shallow-header baseline indexes full and clean");
        cg::test::check(
            r,
            vote_corpus_has_symbol(db_inc, tree, "record||Dead|<tree>"),
            "shallow-header baseline has the Dead row");
        check_no_zero_vote_rows(r, db_inc);

        // 第一步：lib.cpp 删定义体（ref.hpp 不动）——定义票撤走、头文件
        // 站点的 uses 票保活，空壳行留下（site_writable 拒绝的边目标面
        // 续票）。
        write_file(
            tree / "lib.cpp",
            "#include \"ref.hpp\"\n"
            "int pad() { return 0; }\n");
        const cg::graph::IndexSummary second = index_tree(tree, db_inc);
        cg::test::check(
            r,
            second.mode == "incremental" && second.failed == 0,
            "shallow-header definition-drop run is incremental and clean");
        cg::test::check(
            r,
            vote_corpus_has_symbol(db_inc, tree, "record||Dead|<tree>"),
            "header-site shallow vote keeps the shell row alive");
        check_no_zero_vote_rows(r, db_inc);

        // 第二步（缺陷形态）：user.cpp 无关编辑（尾注）——引用者条目重
        // 解析，ref.hpp 站点的 uses 边未重写，活票必须仍在（不得被差集
        // 撤票主动撤走）。
        write_file(
            tree / "user.cpp",
            "#include \"ref.hpp\"\n"
            "int main() { return probe(nullptr); }\n"
            "// trailing comment edit: re-parse user.cpp only\n");
        const cg::graph::IndexSummary third = index_tree(tree, db_inc);
        cg::test::check(
            r,
            third.mode == "incremental" && third.failed == 0,
            "shallow-header unrelated-edit run is incremental and clean");
        cg::test::check(
            r,
            vote_corpus_has_symbol(db_inc, tree, "record||Dead|<tree>"),
            "unrelated re-parse of the referencing entry must not retract "
            "the header-site vote");
        check_no_zero_vote_rows(r, db_inc);

        // 第三步（终态）：ref.hpp 的前置声明与使用一并删（名字全没），
        // 票归零删行，回到与全量重建等价。头文件换成有载荷的替代内容
        // （Marker），保持两侧 files 投影同形（清空但仍被包含的头文件行
        // 是与票数无关的既有形态差异，不进本用例的等价面）。
        write_file(
            tree / "ref.hpp",
            "#pragma once\n"
            "struct Marker\n"
            "{\n"
            "    int t;\n"
            "};\n");
        write_file(
            tree / "user.cpp",
            "#include \"ref.hpp\"\n"
            "int main() { Marker m; return m.t; }\n");
        const cg::graph::IndexSummary fourth = index_tree(tree, db_inc);
        cg::test::check(
            r,
            fourth.mode == "incremental" && fourth.failed == 0,
            "shallow-header final run is incremental and clean");
        cg::test::check(
            r,
            !vote_corpus_has_symbol(db_inc, tree, "record||Dead|<tree>"),
            "row dies once the header-site declaration itself is gone");
        check_no_zero_vote_rows(r, db_inc);
        compare_full_reference(r, base, tree, db_inc, "shallow-header");
    }

    // 形态六：发射者条目编译失败（旧票保留）+ 同文件宏剥离条目成功——
    // 失败条目上批发射过的键不得被判成缺席死亡（"父被伪死"会让父锚绕
    // 过票数直删名字还在的子行——实测缺陷形态：#ifdef 块
    // 里 void Api::run(){} 文本还在、失败条目的票保留，run 行却被父锚
    // 删除）。终态判定对失败条目取保守方向：历史票键视为 survives，行
    // 留票数渐进通道裁决；名字真没了（宏块删除+重试成功）时归零删行，
    // 与全量重建等价。
    void behavior_failed_entry_macro_keeps_children(R& r)
    {
        cg::test::ScratchDir scratch{
            fs::path{"ccg-incr-scratch"} / "vote-failed-entry"};
        const fs::path& base = scratch.base;
        const fs::path  tree = base / "tree";
        fs::create_directories(tree);
        write_file(
            tree / "lib.hpp",
            "#pragma once\n"
            "struct Marker\n"
            "{\n"
            "    int tag;\n"
            "};\n"
            "struct Api\n"
            "{\n"
            "    void run();\n"
            "};\n");
        write_file(
            tree / "impl.cpp",
            "#include \"lib.hpp\"\n"
            "#ifdef BUILD_RUN\n"
            "void Api::run() {}\n"
            "#endif\n"
            "int always() { Marker m; return m.tag; }\n");
        write_file(tree / "main.cpp", "int main() { return 0; }\n");
        // 双编译条目：-DBUILD_RUN 发射 Api::run 的类外定义，无宏条目
        // 把它剥离。
        write_file(
            tree / "compile_commands.json",
            "[\n"
            "  {\"directory\": \".\", \"file\": \"impl.cpp\", \"command\": "
            "\"c++ -std=c++23 -DBUILD_RUN -I . -c impl.cpp -o impl1.o\"},\n"
            "  {\"directory\": \".\", \"file\": \"impl.cpp\", \"command\": "
            "\"c++ -std=c++23 -I . -c impl.cpp -o impl2.o\"},\n"
            "  {\"directory\": \".\", \"file\": \"main.cpp\", \"command\": "
            "\"c++ -std=c++23 -c main.cpp -o main.o\"}\n]");

        const fs::path                db_inc = base / "inc.db";
        const cg::graph::IndexSummary first = index_tree(tree, db_inc);
        cg::test::check(
            r,
            first.mode == "full" && first.failed == 0,
            "failed-entry baseline indexes full and clean");
        cg::test::check(
            r,
            vote_corpus_has_symbol(db_inc, tree, "record||Api|<tree>"),
            "failed-entry baseline has the Api row");
        cg::test::check(
            r,
            vote_corpus_has_symbol(db_inc, tree, "method|Api|run|<tree>"),
            "failed-entry baseline has the out-of-line run row");
        check_no_zero_vote_rows(r, db_inc);

        // 第一步：lib.hpp 删 Api 定义（Marker 保留）——宏条目编译失败
        // （类外定义需要 Api 定义）、无宏条目成功（#ifdef 剥离）。失败
        // 条目的旧票有效：Api 行与 run 行都必须活（修复前 run 行被父锚
        // 绕过票数直删）。
        write_file(
            tree / "lib.hpp",
            "#pragma once\n"
            "struct Marker\n"
            "{\n"
            "    int tag;\n"
            "};\n");
        const cg::graph::IndexSummary second = index_tree(tree, db_inc);
        cg::test::check(
            r,
            second.mode == "incremental" && second.failed == 1,
            "failed-entry parent-drop run is incremental with the macro "
            "entry failing");
        cg::test::check(
            r,
            vote_corpus_has_symbol(db_inc, tree, "record||Api|<tree>"),
            "parent row survives on the failed entry's retained vote");
        cg::test::check(
            r,
            vote_corpus_has_symbol(db_inc, tree, "method|Api|run|<tree>"),
            "child row survives: anchors must not bypass the vote channel "
            "when every surviving vote comes from a failed entry");
        check_no_zero_vote_rows(r, db_inc);

        // 第二步（终态）：impl.cpp 删 #ifdef 块——宏条目内容变化强制重
        // 试、编译成功且不再发射 run；失败票随差集撤走，run/Api 归零删
        // 行，与全量重建等价。
        write_file(
            tree / "impl.cpp",
            "#include \"lib.hpp\"\n"
            "int always() { Marker m; return m.tag; }\n");
        const cg::graph::IndexSummary third = index_tree(tree, db_inc);
        cg::test::check(
            r,
            third.mode == "incremental" && third.failed == 0,
            "failed-entry recovery run is incremental and clean");
        cg::test::check(
            r,
            !vote_corpus_has_symbol(db_inc, tree, "record||Api|<tree>"),
            "Api row dies once the name is truly gone");
        cg::test::check(
            r,
            !vote_corpus_has_symbol(db_inc, tree, "method|Api|run|<tree>"),
            "run row dies once its last emitter succeeds without it");
        check_no_zero_vote_rows(r, db_inc);
        compare_full_reference(r, base, tree, db_inc, "failed-entry");
    }

    // 键漂移侦查件的复现实验（lambda 行号键漂移）：lambda 的
    // unique_key 内嵌行号（operator()@L<file>:<line>），文件顶部插入
    // 一行使全部行号位移——同文件重解析后旧行键失配（票归零删除）、
    // 新行按新行号建立。本用例实测漂移的库内终态：若增量库与全量
    // 重建逐位等价（观察者闭包把引用 lambda 的全部站点带进重写面），
    // 则漂移无终态实害——键格式变更（影响 callee_key/target_key/
    // analysis_slots 等全链文本契约）可记档不实施；不等价则暴露实害，
    // 作为实施键稳定化的红灯证据。
    void equiv_lambda_line_key_drift(R& r)
    {
        cg::test::ScratchDir scratch{
            fs::path{"ccg-incr-scratch"} / "lambda-drift"};
        const fs::path& base = scratch.base;
        const fs::path  tree = base / "tree";
        fs::create_directories(tree);
        write_file(
            tree / "head.hpp",
            "#pragma once\n"
            "inline int call_cb(int seed)\n"
            "{\n"
            "    auto cb = [seed](int add) { return seed + add; };\n"
            "    return cb(1);\n"
            "}\n");
        write_file(
            tree / "user.cpp",
            "#include \"head.hpp\"\n"
            "int run() { return call_cb(40); }\n");
        write_file(tree / "main.cpp", "int main() { return 0; }\n");
        write_file(
            tree / "compile_commands.json",
            "[\n"
            "  {\"directory\": \".\", \"file\": \"user.cpp\", \"command\": "
            "\"c++ -std=c++23 -I . -c user.cpp -o user.o\"},\n"
            "  {\"directory\": \".\", \"file\": \"main.cpp\", \"command\": "
            "\"c++ -std=c++23 -I . -c main.cpp -o main.o\"}\n]");

        const fs::path                db_inc = base / "inc.db";
        const cg::graph::IndexSummary first = index_tree(tree, db_inc);
        cg::test::check(
            r,
            first.mode == "full" && first.failed == 0,
            "lambda-drift baseline indexes full and clean");
        // 基线锚点：lambda 行存在于已知行号（head.hpp 第 4 行）。
        cg::test::check(
            r,
            vote_corpus_has_symbol(
                db_inc,
                tree,
                "function||operator()@L<tree>"),
            "lambda-drift baseline has the lambda row");
        const std::string old_key = [&]
        {
            db::GraphStore    store{db_inc.string()};
            const std::string strip =
                cg::util::norm_path(tree.generic_string());
            for (const db::FullSymbolRow& s : db::all_symbol_rows(store))
            {
                std::string key = s.unique_key;
                if (const auto at = key.find(strip); at != std::string::npos)
                {
                    key.replace(at, strip.size(), "<tree>");
                }
                if (key.starts_with("function||operator()@L<tree>"))
                {
                    return key;
                }
            }
            return std::string{};
        }();
        cg::test::check(
            r,
            !old_key.empty(),
            "lambda-drift captured the baseline lambda key");

        // 编辑：文件顶部插入一行注释——lambda 行号 +1，键漂移。
        write_file(
            tree / "head.hpp",
            "// inserted line: shifts every body line down by one\n"
            "#pragma once\n"
            "inline int call_cb(int seed)\n"
            "{\n"
            "    auto cb = [seed](int add) { return seed + add; };\n"
            "    return cb(1);\n"
            "}\n");
        const cg::graph::IndexSummary second = index_tree(tree, db_inc);
        cg::test::check(
            r,
            second.mode == "incremental" && second.failed == 0,
            "lambda-drift second run is incremental and clean");

        // 侦查断言一：旧键行已随票归零消失（同文件重写面覆盖）。
        {
            db::GraphStore    store{db_inc.string()};
            const std::string strip =
                cg::util::norm_path(tree.generic_string());
            bool old_row_alive = false;
            for (const db::FullSymbolRow& s : db::all_symbol_rows(store))
            {
                std::string key = s.unique_key;
                if (const auto at = key.find(strip); at != std::string::npos)
                {
                    key.replace(at, strip.size(), "<tree>");
                }
                if (key == old_key)
                {
                    old_row_alive = true;
                }
            }
            cg::test::check(
                r,
                !old_row_alive,
                "lambda-drift old line-numbered row erased");
        }
        // 侦查断言二：新键行在（新行号），观察者（user.cpp 的站点）
        // 随闭包重解析。
        cg::test::check(
            r,
            vote_corpus_has_symbol(
                db_inc,
                tree,
                "function||operator()@L<tree>"),
            "lambda-drift new line-numbered row established");
        check_no_zero_vote_rows(r, db_inc);

        // 侦查断言三（核心）：增量终态 == 全量重建——漂移不产生库内
        // 终态差异（实害判定）。
        compare_full_reference(r, base, tree, db_inc, "lambda-drift");
    }

    // 三锚删除件·Specializes 反向锚：确死主模板（编辑文件上精确比对判死）的
    // 实例行（specializes 入边 src，键内嵌主模板拼写）在判死域内被速判
    // 删除。与票数归零通道行为等价（速判+防御双通道），测试钉形态与
    // 全量等价。
    void equiv_specializes_anchor_instance_rows(R& r)
    {
        cg::test::ScratchDir scratch{
            fs::path{"ccg-incr-scratch"} / "spec-anchor"};
        const fs::path& base = scratch.base;
        const fs::path  tree = base / "tree";
        fs::create_directories(tree);
        write_file(
            tree / "tpl.hpp",
            "#pragma once\n"
            "struct Marker\n"
            "{\n"
            "    int tag;\n"
            "};\n"
            "template <class T> struct Widget\n"
            "{\n"
            "    T v;\n"
            "};\n");
        write_file(
            tree / "user.cpp",
            "#include \"tpl.hpp\"\n"
            "struct Instance : Widget<int>\n"
            "{\n"
            "};\n"
            "Instance g_inst;\n"
            "int use_w() { return g_inst.v; }\n");
        write_file(
            tree / "cleaner.cpp",
            "#include \"tpl.hpp\"\n"
            "Marker g_marker;\n");
        write_file(tree / "main.cpp", "int main() { return 0; }\n");
        write_file(
            tree / "compile_commands.json",
            "[\n"
            "  {\"directory\": \".\", \"file\": \"user.cpp\", \"command\": "
            "\"c++ -std=c++23 -I . -c user.cpp -o user.o\"},\n"
            "  {\"directory\": \".\", \"file\": \"cleaner.cpp\", \"command\": "
            "\"c++ -std=c++23 -I . -c cleaner.cpp -o cleaner.o\"},\n"
            "  {\"directory\": \".\", \"file\": \"main.cpp\", \"command\": "
            "\"c++ -std=c++23 -I . -c main.cpp -o main.o\"}\n]");

        const fs::path                db_inc = base / "inc.db";
        const cg::graph::IndexSummary first = index_tree(tree, db_inc);
        cg::test::check(
            r,
            first.mode == "full" && first.failed == 0,
            "spec-anchor baseline indexes full and clean");
        cg::test::check(
            r,
            vote_corpus_has_symbol(db_inc, tree, "record||Widget<int>|<tree>"),
            "spec-anchor baseline has the implicit instance row");

        // 编辑：删主模板（Marker 保留，保持 tpl.hpp 在 fresh 对账域内）；
        // user.cpp 同批删除实例化点（保持解析成功）。
        write_file(
            tree / "tpl.hpp",
            "#pragma once\n"
            "struct Marker\n"
            "{\n"
            "    int tag;\n"
            "};\n");
        write_file(
            tree / "user.cpp",
            "#include \"tpl.hpp\"\n"
            "int use_w() { return 0; }\n");
        const cg::graph::IndexSummary second = index_tree(tree, db_inc);
        cg::test::check(
            r,
            second.mode == "incremental" && second.failed == 0,
            "spec-anchor second run is incremental and clean");
        cg::test::check(
            r,
            !vote_corpus_has_symbol(db_inc, tree, "record||Widget<int>|<tree>"),
            "instance row of the dead primary template erased");
        cg::test::check(
            r,
            !vote_corpus_has_symbol(db_inc, tree, "record||Widget|<tree>"),
            "dead primary template row erased");
        cg::test::check(
            r,
            vote_corpus_has_symbol(db_inc, tree, "record||Marker|<tree>"),
            "untouched marker row survives");
        check_no_zero_vote_rows(r, db_inc);

        compare_full_reference(r, base, tree, db_inc, "spec-anchor");
    }

    // 三锚删除件·父符号锚：确死父（编辑文件上精确比对判死）的跨文件子行（键
    // scope 内嵌父名——类外定义挂在别的文件）速判删除；孙行随 parent_id
    // 级联消失。与票数通道行为等价，测试钉形态与全量等价。
    void equiv_parent_anchor_cross_file_children(R& r)
    {
        cg::test::ScratchDir scratch{
            fs::path{"ccg-incr-scratch"} / "parent-anchor"};
        const fs::path& base = scratch.base;
        const fs::path  tree = base / "tree";
        fs::create_directories(tree);
        write_file(
            tree / "outer.hpp",
            "#pragma once\n"
            "struct Marker\n"
            "{\n"
            "    int tag;\n"
            "};\n"
            "struct Outer\n"
            "{\n"
            "    int plain;\n"
            "    struct Inner\n"
            "    {\n"
            "        int deep;\n"
            "        int deep_twice(int v);\n"
            "    };\n"
            "};\n");
        write_file(
            tree / "outer.cpp",
            "#include \"outer.hpp\"\n"
            "int Outer::Inner::deep_twice(int v) { return v * 2; }\n");
        write_file(
            tree / "cleaner.cpp",
            "#include \"outer.hpp\"\n"
            "Marker g_marker;\n");
        write_file(tree / "main.cpp", "int main() { return 0; }\n");
        write_file(
            tree / "compile_commands.json",
            "[\n"
            "  {\"directory\": \".\", \"file\": \"outer.cpp\", \"command\": "
            "\"c++ -std=c++23 -I . -c outer.cpp -o outer.o\"},\n"
            "  {\"directory\": \".\", \"file\": \"cleaner.cpp\", \"command\": "
            "\"c++ -std=c++23 -I . -c cleaner.cpp -o cleaner.o\"},\n"
            "  {\"directory\": \".\", \"file\": \"main.cpp\", \"command\": "
            "\"c++ -std=c++23 -I . -c main.cpp -o main.o\"}\n]");

        const fs::path                db_inc = base / "inc.db";
        const cg::graph::IndexSummary first = index_tree(tree, db_inc);
        cg::test::check(
            r,
            first.mode == "full" && first.failed == 0,
            "parent-anchor baseline indexes full and clean");
        cg::test::check(
            r,
            vote_corpus_has_symbol(
                db_inc,
                tree,
                "method|Outer::Inner|deep_twice|<tree>"),
            "parent-anchor baseline has the out-of-line child row");

        // 编辑：outer.hpp 删 Outer 全家（Marker 保留保持 fresh 面）；
        // outer.cpp 同批删类外定义（保持解析成功）。
        write_file(
            tree / "outer.hpp",
            "#pragma once\n"
            "struct Marker\n"
            "{\n"
            "    int tag;\n"
            "};\n");
        write_file(
            tree / "outer.cpp",
            "#include \"outer.hpp\"\n"
            "int helper() { Marker m; return m.tag; }\n");
        const cg::graph::IndexSummary second = index_tree(tree, db_inc);
        cg::test::check(
            r,
            second.mode == "incremental" && second.failed == 0,
            "parent-anchor second run is incremental and clean");
        cg::test::check(
            r,
            !vote_corpus_has_symbol(db_inc, tree, "record||Outer|<tree>"),
            "dead parent row erased");
        cg::test::check(
            r,
            !vote_corpus_has_symbol(
                db_inc,
                tree,
                "method|Outer::Inner|deep_twice|<tree>"),
            "cross-file out-of-line child erased with its parent");
        check_no_zero_vote_rows(r, db_inc);

        compare_full_reference(r, base, tree, db_inc, "parent-anchor");
    }

    // 形态五：崩溃一致性——撤票差集循环中途硬杀进程（CCG_TEST_CRASH_
    // IN_CONTRIB 注入点在第二个条目的撤票前 abort），未提交事务的部分
    // 撤票必须整体蒸发；重启后票账与崩溃前逐位一致（无零票行、无错乱
    // 票数），同一编辑的再次索引收敛到与全量重建等价。
    void behavior_contribution_crash_consistency(R& r)
    {
        cg::test::ScratchDir scratch{
            fs::path{"ccg-incr-scratch"} / "vote-crash"};
        const fs::path& base = scratch.base;
        const fs::path  tree = base / "tree";
        fs::create_directories(tree);
        write_vote_corpus(
            tree,
            /*dual_entry=*/true,
            /*use_holder=*/true,
            /*with_shallow=*/false);

        // CLI 子进程与进程内索引共用同一配置目录（--config vote-crash
        // -o base），库路径即 <base>/vote-crash/ccg.db。
        fs::create_directories(base / "vote-crash");
        const fs::path                db_inc = base / "vote-crash" / "ccg.db";
        const cg::graph::IndexSummary first = index_tree(tree, db_inc);
        cg::test::check(
            r,
            first.mode == "full" && first.failed == 0,
            "vote-crash baseline indexes full and clean");
        const std::int64_t baseline_symbols = [&]
        {
            db::GraphStore store{db_inc.string()};
            return cg::db::symbol_count(store);
        }();

        // 同形态二的编辑（两条目批，注入点恰好落在第二 entry 撤票前）。
        write_file(tree / "user.cpp", "int use_holder() { return 0; }\n");

        // 子进程硬杀：ccg index 同树同库，贡献记账循环第二个条目前
        // abort（Windows abort 常态码 3；任何非零退出都算命中注入点）。
        const fs::path ccg_exe =
            fs::path{std::string_view{CCG_BIN_DIR}} / "ccg.exe";
        cg::test::check(
            r,
            fs::exists(ccg_exe),
            std::format("ccg.exe found at {}", ccg_exe.string()));
        if (!fs::exists(ccg_exe))
        {
            return;
        }
        cg::util::platform::ChildRunSpec spec;
        spec.cmdline = std::format(
            L"cmd /c set CCG_TEST_CRASH_IN_CONTRIB=1&& \"{}\" index "
            L"\"{}\" --config vote-crash -o \"{}\" --compile-commands "
            L"\"{}\"",
            ccg_exe.wstring(),
            tree.wstring(),
            base.wstring(),
            (tree / "compile_commands.json").wstring());
        spec.cwd = tree.wstring();
        const cg::util::platform::ChildRunResult crashed =
            cg::util::platform::run_child(spec);
        cg::test::check(
            r,
            crashed.started && crashed.exit_code != 0,
            std::format(
                "crashed indexer exited nonzero (started={}, rc={})",
                crashed.started,
                crashed.exit_code));

        // 崩溃后库一致：行集与基线逐位一致（未提交事务全部蒸发），
        // 票账不变量成立。
        {
            db::GraphStore store{db_inc.string()};
            cg::test::check(
                r,
                cg::db::symbol_count(store) == baseline_symbols,
                "crash rolled back: symbol rows identical to baseline");
        }
        check_no_zero_vote_rows(r, db_inc);

        // 重跑同一编辑（不带注入）：收敛到与全量重建等价。
        const cg::graph::IndexSummary after = index_tree(tree, db_inc);
        cg::test::check(
            r,
            after.mode == "incremental" && after.failed == 0,
            "post-crash re-index is incremental and clean");
        cg::test::check(
            r,
            !vote_corpus_has_symbol(db_inc, tree, "record||Base<Dead>|<tree>"),
            "post-crash re-index converges (instance row erased)");
        check_no_zero_vote_rows(r, db_inc);
        compare_full_reference(r, base, tree, db_inc, "vote-crash");
    }

    // 清扫复活的行生命周期：可达性清扫删掉的文件行，同一路径再次
    // 成为解析输入时必须按首见语义复活（file_id_of 的 miss 分支重新
    // upsert 拿新 id）——清扫行删除时的缓存逐出保证路径→id 解析面
    // 不把死 id 回流进外键列。三幕钉行为：头文件随唯一引用条目入库
    // （符号挂头行）→ 条目退役触发清扫（头行与闭包符号级联删除）→
    // 新 TU 重新包含同一头：行复活、符号重挂、提交零外键失败、票账
    // 无零票行。
    void behavior_swept_file_row_resurrects_on_reuse(R& r)
    {
        if (cg::clang::driver_info().driver.empty())
        {
            cg::test::check(
                r,
                true,
                "swept-file resurrection skipped: no clang driver");
            return;
        }
        cg::test::ScratchDir scratch{
            fs::path{"ccg-incr-scratch"} / "sweep-resurrect"};
        const fs::path& base = scratch.base;
        const fs::path  tree = base / "tree";
        fs::create_directories(tree);
        write_file(
            tree / "util_swept.hpp",
            "#pragma once\n"
            "struct SweptReviveAnchor\n"
            "{\n"
            "    int v;\n"
            "};\n"
            "inline int swept_revive_answer()\n"
            "{\n"
            "    return 7;\n"
            "}\n");
        write_file(
            tree / "alpha.cpp",
            "#include \"util_swept.hpp\"\n"
            "int alpha_run()\n"
            "{\n"
            "    SweptReviveAnchor a{1};\n"
            "    return a.v + swept_revive_answer() - 8;\n"
            "}\n");
        write_file(tree / "beta.cpp", "int beta_run() { return 1; }\n");
        write_file(
            tree / "gamma.cpp",
            "#include \"util_swept.hpp\"\n"
            "int gamma_run()\n"
            "{\n"
            "    SweptReviveAnchor g{2};\n"
            "    return g.v + swept_revive_answer() - 9;\n"
            "}\n");
        const auto write_cdb = [&tree](std::initializer_list<const char*> tus)
        {
            std::string cdb = "[\n";
            bool        first = true;
            for (const char* tu : tus)
            {
                if (!first)
                {
                    cdb += ",\n";
                }
                first = false;
                cdb += std::format(
                    "  {{\"directory\": \".\", \"file\": \"{}\", "
                    "\"command\": \"c++ -std=c++23 -I . -c {} -o {}.o\"}}",
                    tu,
                    tu,
                    tu);
            }
            write_file(tree / "compile_commands.json", cdb + "\n]\n");
        };

        const fs::path db_inc = base / "inc.db";
        const auto     file_row_present = [&db_inc](std::string_view suffix)
        {
            db::GraphStore store{db_inc.string()};
            for (const db::FileRow& fr : db::all_files(store))
            {
                if (fr.path.ends_with(suffix))
                {
                    return true;
                }
            }
            return false;
        };
        const auto symbol_attached_to =
            [&db_inc](std::string_view name, std::string_view file_suffix)
        {
            db::GraphStore                store{db_inc.string()};
            std::optional<cg::db::FileId> fid;
            for (const db::FileRow& fr : db::all_files(store))
            {
                if (fr.path.ends_with(file_suffix))
                {
                    fid = fr.id;
                    break;
                }
            }
            if (!fid.has_value())
            {
                return false;
            }
            for (const db::FullSymbolRow& s : db::all_symbol_rows(store))
            {
                if (s.name == name && s.file_id == *fid)
                {
                    return true;
                }
            }
            return false;
        };

        // 第一幕：alpha（唯一引用者）+ beta 入库，头行带闭包符号。
        write_cdb({"alpha.cpp", "beta.cpp"});
        const cg::graph::IndexSummary first = index_tree(tree, db_inc);
        cg::test::check(
            r,
            first.mode == "full" && first.failed == 0,
            "sweep-resurrect baseline indexes full and clean");
        cg::test::check(
            r,
            file_row_present("/util_swept.hpp")
                && symbol_attached_to("SweptReviveAnchor", "/util_swept.hpp")
                && symbol_attached_to("swept_revive_answer", "/util_swept.hpp"),
            "baseline stores the header row with its closure symbols "
            "attached");

        // 第二幕：alpha 条目退役（文件保留在盘上）——头文件失去全部
        // include 边与符号写入面，与 alpha 行一起被可达性清扫删除。
        write_cdb({"beta.cpp"});
        const cg::graph::IndexSummary second = index_tree(tree, db_inc);
        cg::test::check(
            r,
            second.mode == "incremental" && second.failed == 0
                && second.purged_files == 2
                && second.reachability_sweeps_skipped == 0,
            std::format(
                "retiring the only referencing entry sweeps both rows "
                "(mode {}, failed {}, purged {}, sweeps skipped {})",
                second.mode,
                second.failed,
                second.purged_files,
                second.reachability_sweeps_skipped));
        cg::test::check(
            r,
            !file_row_present("/util_swept.hpp")
                && !file_row_present("/alpha.cpp")
                && !symbol_attached_to("SweptReviveAnchor", "/util_swept.hpp"),
            "swept header row and its closure symbols are gone");

        // 第三幕：新 TU 重新包含同一头——行复活、符号重挂、外键列
        // 全部指向活行（死 id 回流即 FOREIGN KEY 整批回滚，这里以
        // 异常形态显形）。
        write_cdb({"beta.cpp", "gamma.cpp"});
        bool                    commit_blew = false;
        cg::graph::IndexSummary third{};
        try
        {
            third = index_tree(tree, db_inc);
        }
        catch (...)
        {
            commit_blew = true;
        }
        cg::test::check(
            r,
            !commit_blew && third.mode == "incremental" && third.failed == 0,
            std::format(
                "reused header commits cleanly (exception {}, mode {}, "
                "failed {})",
                commit_blew,
                third.mode,
                third.failed));
        cg::test::check(
            r,
            file_row_present("/util_swept.hpp")
                && !file_row_present("/alpha.cpp")
                && symbol_attached_to("SweptReviveAnchor", "/util_swept.hpp")
                && symbol_attached_to("swept_revive_answer", "/util_swept.hpp"),
            "swept header row resurrects with re-attached closure "
            "symbols while the retired main stays gone");
        check_no_zero_vote_rows(r, db_inc);
    }

    // 同样的清除但死类完全内联在头文件里：类的方法符号于是属于
    // 非 main 文件，其陈旧只能经存储层的头文件
    // 重写覆盖清除进入运行——歧义探针过去
    // 在看不见它的状态下运行。移除内联类把干净
    // strategy.hpp 模板体里的 tier-3 点位丢着，而全新
    // 全量重索引解析了它们（增量 94 对全量 97 条边），
    // 静默且永久。探针也必须吃到覆盖清除集，这样
    // 运行像主文件定义变体那样升级到全量重解析覆盖。
    void equiv_purge_lifted_ambiguity_header(R& r)
    {
        cg::test::ScratchDir scratch{
            fs::path{"ccg-incr-scratch"} / "purge-lift-hdr"};
        const fs::path& base = scratch.base;
        const fs::path  tree = base / "tree";
        copy_tree(fs::path{std::string_view{fixture_dir}}, tree);

        // 基线重塑：把 StaleStrategy 的定义从 main
        // 挪进头文件（类内），死类的符号于是住在
        // 非 main 文件。这些体就是下方 momentum.cpp 失去的。
        constexpr std::string_view inline_class_decl =
            "// DEAD CLASS: an abandoned mean-reversion experiment; nothing "
            "instantiates\n// it and nothing calls its methods.\nclass "
            "StaleStrategy : public StrategyBase<StaleStrategy> {\npublic:\n"
            "    explicit StaleStrategy(int window)\n        : "
            "StrategyBase<StaleStrategy>(window)\n    {\n    }\n\n    void "
            "on_tick(core::Price px) override\n    {\n        // DEAD CODE: "
            "never runs.\n        (void)px;\n    }\n\n    core::Price "
            "blend(core::Price raw, core::Price decay) const\n    {\n        "
            "return raw * decay;\n    }\n\n    core::Price decay() const\n    "
            "{\n        return 0.5;\n    }\n\n    std::string base_name() "
            "const\n    {\n        return \"stale\";\n    }\n};\n\n";
        bool shaped = true;
        {
            const fs::path    hpp = tree / "strategy" / "momentum.hpp";
            std::string       text = read_file(hpp);
            const std::size_t at = text.find(stale_class_decl);
            cg::test::check(
                r,
                at != std::string::npos,
                "purge-lift-hdr declaration block found");
            shaped = at != std::string::npos;
            if (shaped)
            {
                write_file(
                    hpp,
                    text.replace(
                        at,
                        stale_class_decl.size(),
                        inline_class_decl));
            }
        }
        if (shaped)
        {
            const fs::path    cpp = tree / "strategy" / "momentum.cpp";
            std::string       text = read_file(cpp);
            const std::size_t at = text.find(stale_class_defs);
            cg::test::check(
                r,
                at != std::string::npos,
                "purge-lift-hdr definition block found");
            shaped = at != std::string::npos;
            if (shaped)
            {
                write_file(cpp, text.replace(at, stale_class_defs.size(), ""));
            }
        }
        if (!shaped)
        {
            return;
        }

        const fs::path                db_inc = base / "inc.db";
        const cg::graph::IndexSummary first = index_tree(tree, db_inc);
        cg::test::check(
            r,
            first.mode == "full" && first.failed == 0,
            "purge-lift-hdr baseline indexes full and clean");

        // 只从头文件移除内联死类：main 的
        // 内容从不变，所以没有覆盖清除喂给
        // 探针的话，没有东西重新解析干净的模板体。
        {
            const fs::path    hpp = tree / "strategy" / "momentum.hpp";
            std::string       text = read_file(hpp);
            const std::size_t at = text.find("// DEAD CLASS");
            const std::size_t end = at == std::string::npos
                                        ? std::string::npos
                                        : text.find("};", at);
            cg::test::check(
                r,
                end != std::string::npos,
                "purge-lift-hdr inline block found");
            shaped = end != std::string::npos;
            if (shaped)
            {
                write_file(hpp, text.substr(0, at) + text.substr(end + 2));
            }
        }
        if (!shaped)
        {
            return;
        }
        const cg::graph::IndexSummary second = index_tree(tree, db_inc);
        cg::test::check(
            r,
            second.mode == "incremental" && second.failed == 0
                && second.stale == 2,
            std::format(
                "purge-lift-hdr keeps the parse scope at the include "
                "closure (stale {}, want 2; the former full-coverage "
                "escalation reparse is deleted)",
                second.stale));

        compare_full_reference(
            r,
            base,
            tree,
            db_inc,
            "purge-lift-hdr",
            "full",
            false,
            template_body_tier3_gap());
    }

    // 干跑 purgedFiles 对已提交孤儿清扫：被弃头文件（只被 main.cpp 包含）被真实
    // 运行清扫；计划过去只数被删集合并报 0。
    void fp_dry_run_purged_matches(R& r)
    {
        cg::test::ScratchDir scratch{
            fs::path{"ccg-incr-scratch"} / "dry-purged"};
        const fs::path& base = scratch.base;
        const fs::path  tree = base / "tree";
        copy_tree(fs::path{std::string_view{fixture_dir}}, tree);
        const fs::path db_file = base / "ccg.db";
        (void)index_tree(tree, db_file);

        // 弃 core/vec.hpp：删掉它的 include 与 main.cpp 里每个使用。
        const fs::path victim = tree / "main.cpp";
        std::string    text = read_file(victim);
        const auto drop = [&](std::string_view needle, std::string_view with)
        {
            const std::size_t at = text.find(needle);
            cg::test::check(
                r,
                at != std::string::npos,
                "dry-purged anchor found");
            if (at != std::string::npos)
            {
                text.replace(at, needle.size(), with);
            }
        };
        drop("#include \"core/vec.hpp\"\n", "");
        drop("    const core::Vec3 up{1.0, 2.0, 3.0};\n", "");
        drop(
            "    const core::Vec3 blended = up.scaled(2.0) + up * 0.5 + 0.25 * up;\n",
            "");
        drop(
            "return (ticks > 0 && blended.length() > 0.0 && st.levels > 0) ? 0 : 1;",
            "return (ticks > 0 && st.levels > 0) ? 0 : 1;");
        write_file(victim, text);

        const cg::graph::IndexSummary plan =
            index_tree(tree, db_file, {.dry_run = true});
        const cg::graph::IndexSummary real = index_tree(tree, db_file);
        bool                          vec_gone = true;
        {
            db::GraphStore store{db_file.string()};
            for (const db::FileRow& fr : db::all_files(store))
            {
                if (fr.path.ends_with("core/vec.hpp"))
                {
                    vec_gone = false;
                }
            }
        }
        cg::test::check(
            r,
            real.failed == 0 && real.purged_files == 1 && plan.stale >= 1
                && vec_gone,
            std::format(
                "real orphan sweep reports the abandoned header (real purged {}, "
                "failed {}, vec.hpp swept: {}); the dry plan still names the "
                "stale TUs ({}). The plan's purged estimate is link-graph-only "
                "(a true dry run never parses, so include changes surface next "
                "run - plan said {})",
                real.purged_files,
                real.failed,
                vec_gone,
                plan.stale,
                plan.purged_files));
    }

    // 先前已索引的 main 从磁盘消失：行保持 Failed 且必须丢掉其陈旧内容哈希——
    // 同一树的从零全量运行从未哈希过缺失文件
    // （规范化比较状态与哈希，终局闸门两个都钉）。
    void fp_missing_main_content_hash(R& r)
    {
        cg::test::ScratchDir scratch{
            fs::path{"ccg-incr-scratch"} / "missing-hash"};
        const fs::path& base = scratch.base;
        std::error_code ec;
        const fs::path  tree = base / "tree";
        copy_tree(fs::path{std::string_view{fixture_dir}}, tree);
        const std::string order = cg::util::norm_path(
            (tree / "market" / "order.cpp").generic_string());
        const fs::path db_file = base / "ccg.db";
        (void)index_tree(tree, db_file);
        {
            db::GraphStore store{db_file.string()};
            const auto     row = store.find_file(order);
            cg::test::check(
                r,
                row.has_value() && row->content_hash.has_value(),
                "indexed main carries a content hash before going "
                "missing");
        }

        fs::remove(tree / "market" / "order.cpp", ec);
        const cg::graph::IndexSummary gone = index_tree(tree, db_file);
        cg::test::check(
            r,
            gone.failed == 1,
            std::format(
                "missing-main run fails exactly that TU "
                "(failed {})",
                gone.failed));
        {
            db::GraphStore store{db_file.string()};
            const auto     row = store.find_file(order);
            cg::test::check(
                r,
                row.has_value() && row->status == cg::util::ParseStatus::Failed
                    && !row->content_hash.has_value(),
                "missing main row is Failed with a cleared content "
                "hash");
        }

        // 参照树仍含被删 TU（合法失败），保留显式尾（同 missing-entry）。
        {
            const fs::path tree2 = base / "tree-full";
            copy_tree(tree, tree2);
            const fs::path db_full = base / "full.db";
            (void)index_tree(tree2, db_full);
            expect_equal(
                r,
                db_file,
                db_full,
                "missing-hash",
                cg::util::norm_path(tree.generic_string()),
                cg::util::norm_path(tree2.generic_string()));
        }
    }

    // 单个编译数据库内大小写不一致的拼写：-I 旗标拼的树根与 file/directory
    // 字段大小写不同。两者在大小写不敏感文件系统上
    // 解析到同一目录，修复前经 -I 拼写抵达的每个
    // 头文件都复制其整个子图（258 符号 / 每个 lerp
    // 重载两次）。提取现在把拼写折叠到磁盘大小写，
    // 一次全量索引对每个文件只见一次。全程
    // 绝对路径，即真实 CLI 运行所在的拼写体制。
    void equiv_include_case_consistency(R& r)
    {
        cg::test::ScratchDir scratch{
            fs::path{"ccg-incr-scratch"} / "include-case"};
        const fs::path& base = scratch.base;
        std::error_code ec;
        const fs::path  tree = fs::absolute(base / "tree");
        copy_tree(fs::path{std::string_view{fixture_dir}}, tree);

        const auto run_index = [&](const fs::path& db_file)
        {
            const std::string cdb =
                (tree / "compile_commands.json").generic_string();
            const auto              commands = cg::clang::load(cdb, {});
            db::GraphStore          store{db_file.string()};
            cg::graph::IndexOptions opt;
            opt.roots = {cg::util::norm_path(tree.generic_string())};
            opt.progress = false;
            opt.compile_db_path = cdb;
            opt.db_path = db_file.string();
            cg::graph::Indexer indexer{store, std::move(opt)};
            return indexer.run(commands);
        };

        // 原始树直接全量索引的参照计数。
        std::int64_t want_symbols = 0;
        {
            const cg::graph::IndexSummary ref = run_index(base / "ref.db");
            want_symbols = ref.symbols;
        }

        // 把每条的 -I "." 重拼成末段大写的树根
        // ——同一目录的另一种拼写。
        std::string       upper = tree.generic_string();
        const std::size_t last = upper.find_last_of('/');
        for (std::size_t i = last + 1; i < upper.size(); ++i)
        {
            upper[i] = static_cast<char>(
                std::toupper(static_cast<unsigned char>(upper[i])));
        }
        if (!fs::equivalent(tree, fs::path{upper}, ec))
        {
            cg::test::check(
                r,
                true,
                "include-case needs a case-insensitive filesystem; "
                "skipped on this platform");
            return;
        }
        ec.clear();
        const fs::path cdb = tree / "compile_commands.json";
        std::string    text = read_file(cdb);
        cg::test::check(
            r,
            text.find("-I . ") != std::string::npos,
            "include-case cdb anchor found");
        while (text.find("-I . ") != std::string::npos)
        {
            text.replace(text.find("-I . "), 4, "-I " + upper);
        }
        write_file(cdb, text);

        const fs::path                db_file = base / "ccg.db";
        const cg::graph::IndexSummary sum = run_index(db_file);
        cg::test::check(
            r,
            sum.failed == 0,
            std::format("odd-case -I index is clean (failed {})", sum.failed));
        std::int64_t fold_dup_keys = 0;
        int          lerp_rows = 0;
        {
            db::GraphStore             store{db_file.string()};
            std::map<std::string, int> by_fold;
            for (const db::FullSymbolRow& s : db::all_symbol_rows(store))
            {
                ++by_fold[fold_ascii(s.unique_key)];
            }
            for (const auto& [key, n] : by_fold)
            {
                if (n > 1)
                {
                    ++fold_dup_keys;
                }
            }
            lerp_rows = static_cast<int>(
                db::find_symbols(store, "lerp", {}).matches.size());
        }
        cg::test::check(
            r,
            sum.symbols == want_symbols && fold_dup_keys == 0 && lerp_rows == 3
                && folded_duplicate_count(db_file) == 0,
            std::format(
                "one identity per file under mixed-case -I "
                "(symbols {}, want {}, fold-dup keys {}, "
                "lerp overloads {})",
                sum.symbols,
                want_symbols,
                fold_dup_keys,
                lerp_rows));
    }

    // `ccg update` 种子等价：外部播种的增量运行
    // （IndexOptions::seed_files——实时命令的外部脏集
    // 通道）必须等于同一树状态的从零全量
    // 重索引，包括 stat 快路径看不见的保 mtime
    // 编辑（信号来自种子而非 stat）、播种共享头
    // （闭包传播到每个包含者）、播种未变文件
    // （相同行的幂等重写）与未跟踪种子（完全
    // 无效果）。
    [[nodiscard]]
    auto update_tree(
        const fs::path&                 tree,
        const fs::path&                 db_file,
        const std::vector<std::string>& seeds,
        bool force = false) -> cg::graph::IndexSummary
    {
        const std::string       cdb = (tree / "compile_commands.json").string();
        const auto              commands = cg::clang::load(cdb, {});
        db::GraphStore          store{db_file.string()};
        cg::graph::IndexOptions opt;
        opt.roots = {cg::util::norm_path(tree.generic_string())};
        opt.force = force;
        opt.progress = false;
        opt.compile_db_path = cdb;
        opt.db_path = db_file.string();
        opt.seed_files = seeds;
        cg::graph::Indexer indexer{store, std::move(opt)};
        return indexer.run(commands);
    }

    void equiv_update_seed(R& r)
    {
        cg::test::ScratchDir scratch{
            fs::path{"ccg-incr-scratch"} / "update-seed"};
        const fs::path& base = scratch.base;
        std::error_code ec;

        // ---- 播种 MAIN，保 mtime 等长编辑 --------------------------------
        const fs::path tree = base / "tree";
        copy_tree(fs::path{std::string_view{fixture_dir}}, tree);
        const fs::path db_file = base / "ccg.db";
        (void)index_tree(tree, db_file);

        const fs::path victim = tree / "main.cpp";
        mtime_blind_edit(r, victim, "cross-file call chain", "seed victim");

        const std::string seed =
            cg::util::norm_path(victim.lexically_normal().generic_string());
        const cg::graph::IndexSummary seeded =
            update_tree(tree, db_file, {seed});
        cg::test::check(
            r,
            seeded.stale >= 1 && seeded.failed == 0,
            std::format(
                "seeded run re-parses the mtime-preserving "
                "main edit (stale {}, failed {})",
                seeded.stale,
                seeded.failed));
        cg::test::check(
            r,
            seeded.mode == "incremental",
            "seeded run stays incremental");

        // ---- 播种共享 HEADER，保 mtime ------------------------------------
        const fs::path tree3 = base / "tree-hdr";
        copy_tree(fs::path{std::string_view{fixture_dir}}, tree3);
        const fs::path db_hdr = base / "hdr.db";
        (void)index_tree(tree3, db_hdr);
        const fs::path header = tree3 / "core" / "vec.hpp";
        const auto     saved_hmtime = fs::last_write_time(header, ec);
        // 追加一个可解析的内联辅助（治愈测试形态），然后还原
        // mtime：stat 快路径必须保持盲，而种子迫使
        // 每个包含者重解析。
        append_file(
            header,
            "\nnamespace incr_seed {\ninline double seed_marker(const "
            "core::Vec3& v)\n{\n    return v.length();\n}\n}  // "
            "namespace incr_seed\n");
        fs::last_write_time(header, saved_hmtime, ec);
        const std::string hseed =
            cg::util::norm_path(header.lexically_normal().generic_string());
        const cg::graph::IndexSummary hsum =
            update_tree(tree3, db_hdr, {hseed});
        cg::test::check(
            r,
            hsum.stale >= 1 && hsum.failed == 0,
            std::format(
                "seeded header re-parses its includers "
                "(stale {}, failed {})",
                hsum.stale,
                hsum.failed));
        const fs::path tree4 = base / "tree-hdr-full";
        copy_tree(tree3, tree4);
        const fs::path db_hdr_full = base / "hdr-full.db";
        (void)index_tree(tree4, db_hdr_full, {.force = true});
        expect_equal(
            r,
            db_hdr,
            db_hdr_full,
            "update-seed-header",
            cg::util::norm_path(tree3.generic_string()),
            cg::util::norm_path(tree4.generic_string()));

        // 全量参照行供下方幂等/未跟踪两段复用（非一次性尾，不走助手）。
        const fs::path tree2 = base / "tree-full";
        copy_tree(tree, tree2);
        const fs::path db_full = base / "full.db";
        (void)index_tree(tree2, db_full, {.force = true});
        expect_equal(
            r,
            db_file,
            db_full,
            "update-seed-main",
            cg::util::norm_path(tree.generic_string()),
            cg::util::norm_path(tree2.generic_string()));

        // ---- 幂等：播种未变文件重写相同的行 ------------------------------
        const cg::graph::IndexSummary idem = update_tree(tree, db_file, {seed});
        cg::test::check(
            r,
            idem.failed == 0,
            std::format(
                "idempotent re-seed succeeds (failed {})",
                idem.failed));
        expect_equal(
            r,
            db_file,
            db_full,
            "update-seed-idempotent",
            cg::util::norm_path(tree.generic_string()),
            cg::util::norm_path(tree2.generic_string()));

        // ---- 未跟踪种子：无被跟踪者命中，无变化 --------------------------
        const cg::graph::IndexSummary ghost = update_tree(
            tree,
            db_file,
            {cg::util::norm_path((tree / "not-a-file.cpp")
                                     .lexically_normal()
                                     .generic_string())});
        cg::test::check(
            r,
            ghost.stale == 0 && ghost.failed == 0,
            std::format(
                "untracked seed changes nothing (stale {}, "
                "failed {})",
                ghost.stale,
                ghost.failed));
        expect_equal(
            r,
            db_file,
            db_full,
            "update-seed-untracked",
            cg::util::norm_path(tree.generic_string()),
            cg::util::norm_path(tree2.generic_string()));
    }

    const cg::test::Registrar reg_body{"incr.equiv.body", &equiv_body_edit};
    const cg::test::Registrar reg_hdr{
        "incr.equiv.shared_header",
        &equiv_shared_header};
    const cg::test::Registrar reg_add{"incr.equiv.add_file", &equiv_add_file};
    const cg::test::Registrar reg_del{
        "incr.equiv.delete_file",
        &equiv_delete_file};
    const cg::test::Registrar reg_drop{
        "incr.equiv.drop_entry_keep_file",
        &equiv_drop_entry_keep_file};
    const cg::test::Registrar reg_rename{"incr.equiv.rename", &equiv_rename};
    const cg::test::Registrar reg_mainedge{
        "incr.equiv.main_body_edge",
        &equiv_main_body_edge};
    const cg::test::Registrar reg_maininc{
        "incr.equiv.main_include_add",
        &equiv_main_include_add};
    const cg::test::Registrar reg_heal{
        "incr.equiv.break_heal_edit",
        &equiv_break_heal_edit};
    const cg::test::Registrar reg_touch{"incr.fp.touch_only", &fp_touch_only};
    const cg::test::Registrar reg_paranoid{
        "incr.fp.paranoid",
        &fp_paranoid_mtime_preserving};
    const cg::test::Registrar reg_digest{
        "incr.fp.command_digest",
        &fp_command_digest};
    const cg::test::Registrar reg_dedup{
        "incr.fp.duplicate_tu_dedup",
        &fp_duplicate_tu_dedup};
    const cg::test::Registrar reg_divergent{
        "incr.fp.divergent_duplicates",
        &fp_divergent_duplicate_tus};
    const cg::test::Registrar reg_dry{
        "incr.fp.dry_run",
        &fp_dry_run_writes_nothing};
    const cg::test::Registrar reg_dryplan{
        "incr.fp.dry_run_plan",
        &fp_dry_run_plan_matches};
    const cg::test::Registrar reg_drift{
        "incr.equiv.case_drift",
        &equiv_case_drift};
    const cg::test::Registrar reg_retired{
        "incr.fp.retired_digest",
        &fp_retired_digest_pruned};
    const cg::test::Registrar reg_failedhash{
        "incr.fp.failed_tu_hash",
        &fp_failed_tu_content_hash};
    const cg::test::Registrar reg_drybytes{
        "incr.fp.dry_run_bytes",
        &fp_dry_run_dirty_bytes};
    // 文本回退算术恒等式：摘要的回退总数必须
    // 等于 parse-ok + still-failed（规划器的计划数与运行的
    // 实际数过去在一行里是两个不同的数——实测
    // "333 text fallbacks (155 parse ok, 450 still failed)"）。此处每个 TU
    // 都被迫走上回退路径，ok+failed 必须覆盖语料且
    // 总数必须精确等于其和。
    void fp_pch_fallback_identity(R& r)
    {
        cg::test::ScratchDir scratch{fs::path{"ccg-incr-scratch"} / "pch-idem"};
        const fs::path&      base = scratch.base;
        const fs::path       tree = base / "tree";
        copy_tree(fs::path{std::string_view{fixture_dir}}, tree);
        const fs::path    db_file = base / "ccg.db";
        const std::string cdb = (tree / "compile_commands.json").string();
        std::vector<cg::clang::CompileCommand> commands =
            cg::clang::load(cdb, {});
        cg::test::check(r, !commands.empty(), "fixture db loads");
        for (cg::clang::CompileCommand& c : commands)
        {
            c.pch.status = cg::clang::PchUse::Status::text_fallback;
            c.pch.note = "unit-test forced fallback";
            c.pch.identity = "idem";
            c.pch_fallback_argv = c.argv;
        }
        db::GraphStore          store{db_file.string()};
        cg::graph::IndexOptions opt;
        opt.roots = {cg::util::norm_path(tree.generic_string())};
        opt.compile_db_path = cdb;
        opt.db_path = db_file.string();
        cg::graph::Indexer            indexer{store, std::move(opt)};
        const cg::graph::IndexSummary sum = indexer.run(commands);
        cg::test::check(
            r,
            sum.pch_fallbacks == sum.pch_fallback_ok + sum.pch_fallback_failed
                && sum.pch_fallback_ok + sum.pch_fallback_failed
                       == static_cast<std::int64_t>(commands.size())
                && sum.failed == 0,
            std::format(
                "fallback total == parse-ok + still-failed == corpus "
                "(total {}, ok {}, failed {}, corpus {}, failed TUs {})",
                sum.pch_fallbacks,
                sum.pch_fallback_ok,
                sum.pch_fallback_failed,
                commands.size(),
                sum.failed));
    }

    const cg::test::Registrar reg_pchidem{
        "incr.fp.pch_fallback_identity",
        &fp_pch_fallback_identity};
    const cg::test::Registrar reg_missing{
        "incr.fp.missing_entry_status",
        &fp_missing_entry_status};
    // 分歧重复条目的解析多重性变化（实测复现）：(file, digest) 指纹设计让每条
    // 编译数据库条目一行新鲜度，所以 main 合法地
    // 出现在两条不同命令下。删掉其一会让被删
    // 解析的出现贡献留在原地——每个幸存摘要
    // 都命中、运行报全跳过，共享头点位
    // 一直多一个幻影加成（occ 7 对全新全量的 6）直到 --full；
    // 增加第二条分歧条目则以同样方式漏掉新解析的
    // 加成。main 活条目数变化必须重覆盖 main 的
    // 闭包，让出现计数器只从活条目重建。
    void equiv_dup_entry_multiplicity(R& r)
    {
        cg::test::ScratchDir scratch{
            fs::path{"ccg-incr-scratch"} / "dup-multiplicity"};
        const fs::path& base = scratch.base;
        const fs::path  tree = base / "tree";
        copy_tree(fs::path{std::string_view{fixture_dir}}, tree);
        const fs::path                db_inc = base / "inc.db";
        const cg::graph::IndexSummary first = index_tree(tree, db_inc);
        cg::test::check(
            r,
            first.mode == "full" && first.failed == 0,
            "dup-multiplicity baseline indexes full and clean");

        // 给 sim/engine.cpp 增加一条分歧的第二条目（多一个旗标）。
        const fs::path    cdb = tree / "compile_commands.json";
        const std::string original = read_file(cdb);
        const std::size_t hit = original.find("sim/engine.cpp");
        const std::size_t entry_begin = hit == std::string::npos
                                            ? std::string::npos
                                            : original.rfind('{', hit);
        const std::size_t entry_end = hit == std::string::npos
                                          ? std::string::npos
                                          : original.find('}', hit);
        cg::test::check(
            r,
            entry_begin != std::string::npos && entry_end != std::string::npos,
            "dup-multiplicity engine entry located");
        if (entry_begin == std::string::npos || entry_end == std::string::npos)
        {
            return;
        }
        std::string flagged =
            original.substr(entry_begin, entry_end - entry_begin + 1);
        const std::size_t flag_at = flagged.find("-std=c++23");
        if (flag_at != std::string::npos)
        {
            flagged.replace(
                flag_at,
                std::string_view{"-std=c++23"}.size(),
                "-std=c++23 -DINCR_DUP_PROBE");
        }
        write_file(cdb, add_db_entry(original, flagged));

        const cg::graph::IndexSummary added = index_tree(tree, db_inc);
        cg::test::check(
            r,
            added.tus == 7 && added.failed == 0,
            std::format(
                "dup-multiplicity add indexes 7 TUs (tus "
                "{}, failed {})",
                added.tus,
                added.failed));
        compare_full_reference(
            r,
            base,
            tree,
            db_inc,
            "dup-multiplicity-add",
            "add");

        // 再删掉它：回到原始单条目。修复前运行
        // 在此处全跳过并留着被删解析的加成。
        write_file(cdb, original);
        const cg::graph::IndexSummary dropped = index_tree(tree, db_inc);
        cg::test::check(
            r,
            dropped.mode == "incremental" && dropped.failed == 0
                && dropped.stale >= 1,
            std::format(
                "dup-multiplicity drop re-parses the "
                "closure (stale {}, skipped {})",
                dropped.stale,
                dropped.skipped));
        compare_full_reference(
            r,
            base,
            tree,
            db_inc,
            "dup-multiplicity-drop",
            "drop");

        // 收敛：多重性变化不得让后续运行翻来覆去。
        const cg::graph::IndexSummary settled = index_tree(tree, db_inc);
        cg::test::check(
            r,
            settled.skipped == 6 && settled.stale == 0,
            std::format(
                "dup-multiplicity settles to skipped-all "
                "(skipped {}, stale {})",
                settled.skipped,
                settled.stale));
    }

    const cg::test::Registrar reg_lift{
        "incr.equiv.purge_lifted_ambiguity",
        &equiv_purge_lifted_ambiguity};
    const cg::test::Registrar reg_lifthdr{
        "incr.equiv.purge_lifted_ambiguity_header",
        &equiv_purge_lifted_ambiguity_header};
    const cg::test::Registrar reg_dupmult{
        "incr.equiv.dup_entry_multiplicity",
        &equiv_dup_entry_multiplicity};
    const cg::test::Registrar reg_drypurged{
        "incr.fp.dry_run_purged",
        &fp_dry_run_purged_matches};
    const cg::test::Registrar reg_missinghash{
        "incr.fp.missing_main_hash",
        &fp_missing_main_content_hash};
    const cg::test::Registrar reg_includecase{
        "incr.equiv.include_case",
        &equiv_include_case_consistency};
    const cg::test::Registrar reg_updseed{
        "incr.equiv.update_seed",
        &equiv_update_seed};
    const cg::test::Registrar reg_voteinst{
        "incr.equiv.use_site_instance_zero_vote",
        &equiv_use_site_instance_zero_vote};
    const cg::test::Registrar reg_votedup{
        "incr.equiv.dup_entry_one_vote_per_entry",
        &equiv_dup_entry_one_vote_per_entry};
    const cg::test::Registrar reg_voteshallow{
        "incr.behavior.shallow_reference_keeps_row",
        &behavior_shallow_reference_keeps_row};
    const cg::test::Registrar reg_voteshallowhdr{
        "incr.behavior.shallow_reference_header_site",
        &behavior_shallow_reference_header_site};
    const cg::test::Registrar reg_votefailed{
        "incr.behavior.failed_entry_macro_keeps_children",
        &behavior_failed_entry_macro_keeps_children};
    const cg::test::Registrar reg_votecrash{
        "incr.behavior.contribution_crash_consistency",
        &behavior_contribution_crash_consistency};
    const cg::test::Registrar reg_sweepresurrect{
        "incr.behavior.swept_file_row_resurrects_on_reuse",
        &behavior_swept_file_row_resurrects_on_reuse};
    const cg::test::Registrar reg_specanchor{
        "incr.equiv.specializes_anchor_instance_rows",
        &equiv_specializes_anchor_instance_rows};
    const cg::test::Registrar reg_parentanchor{
        "incr.equiv.parent_anchor_cross_file_children",
        &equiv_parent_anchor_cross_file_children};
    const cg::test::Registrar reg_lambdadrift{
        "incr.equiv.lambda_line_key_drift",
        &equiv_lambda_line_key_drift};

    // ------------------------------------------------ 分析暖启动 ----------
    //
    // 增量类型传播/派发路径（analysis_slots 暖启动 + 脏
    // 闭包重算）必须守住比上方图投影更强的不变量：
    // 穿过一串编辑，派生数据——每个
    // call_sites 解析列与持久化的槽位终态——
    // 始终等于同一最终树的从零全量重索引。
    // static_type_id 刻意不进投影：数值 id 是库内局部量，两个库
    // 各自编 id 不可直接比对（其余列都按 unique_key/文本映射）。
    // 其取值面（receiver_static 首键）自静态判证列分列后逐轮稳定，
    // 轮间漂移由 incr.equiv.receiver_static_idempotent_reparse 钉住。
    struct AnalysisProjection
    {
        std::multiset<std::string>         sites;
        std::map<std::string, std::string> slots;
    };

    [[nodiscard]]
    auto analysis_projection(const fs::path& db_file, const std::string& strip)
        -> AnalysisProjection
    {
        db::GraphStore                    store{db_file.string()};
        AnalysisProjection                out;
        std::map<db::FileId, std::string> path_of;
        for (const db::FileRow& fr : db::all_files(store))
        {
            path_of.emplace(fr.id, fr.path);
        }
        std::map<db::SymId, std::string> key_of;
        for (const db::FullSymbolRow& s : db::all_symbol_rows(store))
        {
            key_of.emplace(s.id, s.unique_key);
        }
        for (const db::CallSiteRow& cs : db::all_call_sites(store))
        {
            out.sites.insert(strip_tree(
                std::format(
                    "{} | {} | {}:{} | callee={} virt={} ind={} -> method={} "
                    "recv={} resolved={} conf={} cand={} reason={}",
                    key_of.count(cs.caller_id) ? key_of.at(cs.caller_id)
                                               : std::string{"?"},
                    cs.file_id != 0 && path_of.count(cs.file_id)
                        ? path_of.at(cs.file_id)
                        : std::string{"?"},
                    cs.line,
                    cs.col,
                    cs.callee_key,
                    cs.is_virtual,
                    cs.indirect,
                    cs.method_key,
                    cs.receiver_types,
                    cs.resolved,
                    cs.confidence,
                    cs.candidates,
                    cs.fallback_reason),
                strip));
        }
        for (const db::AnalysisSlotRow& row : store.all_analysis_slots())
        {
            std::string key = strip_tree(row.owner_key, strip);
            key.push_back('\x1f');
            key += row.var_name;
            out.slots.emplace(
                key,
                std::format(
                    "{} | conf={} src={} reason={} "
                    "trunc={}",
                    strip_tree(row.type_set, strip),
                    row.confidence,
                    row.source,
                    strip_tree(row.reason, strip),
                    row.truncated));
        }
        return out;
    }

    void expect_analysis_equal(
        R&                 r,
        const fs::path&    inc,
        const fs::path&    full,
        std::string_view   label,
        const std::string& strip_inc,
        const std::string& strip_full)
    {
        const AnalysisProjection a = analysis_projection(inc, strip_inc);
        const AnalysisProjection b = analysis_projection(full, strip_full);
        if (const auto d = first_map_diff(a.slots, b.slots); !d.first.empty())
        {
            cg::test::check(
                r,
                false,
                std::format(
                    "[{}] analysis_slots differ at '{}': inc {} vs "
                    "full {}",
                    label,
                    d.first,
                    d.first.empty() ? std::string{} : a.slots.at(d.first),
                    d.second));
            return;
        }
        if (const std::string d = multiset_diff(a.sites, b.sites); !d.empty())
        {
            cg::test::check(
                r,
                false,
                std::format(
                    "[{}] call_sites resolution differs: "
                    "'{}'",
                    label,
                    d));
            return;
        }
        cg::test::check(
            r,
            true,
            std::format(
                "[{}] analysis projection incremental == full reindex",
                label));
    }

    void equiv_analysis_warmstart(R& r)
    {
        cg::test::ScratchDir scratch{
            fs::path{"ccg-incr-scratch"} / "analysis-warm"};
        const fs::path& base = scratch.base;
        std::error_code ec;
        const fs::path  tree = base / "tree";
        copy_tree(fs::path{std::string_view{fixture_dir}}, tree);
        const fs::path db_inc = base / "inc.db";

        const cg::graph::IndexSummary first = index_tree(tree, db_inc);
        cg::test::check(
            r,
            first.mode == "full" && first.failed == 0,
            "analysis-warm baseline indexes full and clean");
        {
            db::GraphStore store{db_inc.string()};
            cg::test::check(
                r,
                store.meta("analysis_incremental").value_or("?") == "0",
                "baseline commit runs the full path (no warm state yet)");
            cg::test::check(
                r,
                !store.all_analysis_slots().empty(),
                "baseline persists the L2 slot terminal state");
        }

        const auto compare_full =
            [&](std::string_view label, bool with_tier3_gap = false)
        {
            const fs::path tree2 = base / "tree-ref";
            (void)fs::remove_all(tree2, ec);
            copy_tree(tree, tree2);
            const fs::path db_full = base / "ref.db";
            (void)fs::remove(db_full, ec);
            (void)index_tree(tree2, db_full);
            expect_equal(
                r,
                db_inc,
                db_full,
                label,
                cg::util::norm_path(tree.generic_string()),
                cg::util::norm_path(tree2.generic_string()),
                with_tier3_gap ? template_body_tier3_gap() : nullptr);
            expect_analysis_equal(
                r,
                db_inc,
                db_full,
                label,
                cg::util::norm_path(tree.generic_string()),
                cg::util::norm_path(tree2.generic_string()));
            (void)fs::remove_all(tree2, ec);
            (void)fs::remove(db_full, ec);
        };
        const auto incremental_meta = [&](std::string_view label)
        {
            db::GraphStore store{db_inc.string()};
            cg::test::check(
                r,
                store.meta("analysis_incremental").value_or("?") == "1"
                    && store.meta("analysis_closure").has_value(),
                std::format(
                    "[{}] commit converged via the incremental path "
                    "(meta {})",
                    label,
                    store.meta("analysis_incremental").value_or("?")));
        };

        // 编辑 1：删掉 FastMomentumStrategy::on_tick 的
        // kick 块——脏文件里喂给 MarketOrder 构造函数
        // 与 OrderBook::add 的点位消失，干净文件的形参槽
        // 失去旧实参流（捕获旧被调方下游播种）。
        {
            const fs::path victim = tree / "strategy" / "momentum.cpp";
            std::string    text = read_file(victim);
            constexpr std::string_view kick =
                "        if (px > trigger_)\n        {\n            "
                "market::MarketOrder kick(2, 50, core::Side::buy, px);\n"
                "            const_cast<market::exec::OrderBook&>(book())"
                ".add(kick);\n            infra::log_info(\"fast kick\");\n"
                "        }";
            const std::size_t at = text.find(kick);
            cg::test::check(
                r,
                at != std::string::npos,
                "analysis-warm kick block anchor found");
            if (at != std::string::npos)
            {
                write_file(
                    victim,
                    text.replace(at, kick.size(), "        (void)trigger_;"));
            }
        }
        const cg::graph::IndexSummary second = index_tree(tree, db_inc);
        cg::test::check(
            r,
            second.mode == "incremental" && second.failed == 0,
            "analysis-warm edit run is incremental and clean");
        incremental_meta("kick-delete");
        compare_full("kick-delete");

        // 编辑 2：main.cpp 体编辑（脏闭包根在持有者槽）。
        append_file(
            tree / "main.cpp",
            "\nnamespace incr_warm {\nint warm_marker()\n{\n    return "
            "1;\n}\n}  // namespace incr_warm\n");
        const cg::graph::IndexSummary third = index_tree(tree, db_inc);
        cg::test::check(
            r,
            third.mode == "incremental" && third.failed == 0,
            "analysis-warm main edit run is incremental and clean");
        incremental_meta("main-edit");
        compare_full("main-edit");

        // 编辑 3：移除死 StaleStrategy 类——extends/overrides
        // 边集变化（层级摘要翻转 -> 派发升级）加
        // 喂给跨文件扇入检测的注定符号捕获。
        {
            const fs::path    hpp = tree / "strategy" / "momentum.hpp";
            std::string       text = read_file(hpp);
            const std::size_t at = text.find(stale_class_decl);
            cg::test::check(
                r,
                at != std::string::npos,
                "analysis-warm dead class declaration found");
            if (at != std::string::npos)
            {
                write_file(hpp, text.erase(at, stale_class_decl.size()));
            }
            const fs::path    cpp = tree / "strategy" / "momentum.cpp";
            std::string       body = read_file(cpp);
            const std::size_t at2 = body.find(stale_class_defs);
            cg::test::check(
                r,
                at2 != std::string::npos,
                "analysis-warm dead class definitions found");
            if (at2 != std::string::npos)
            {
                write_file(cpp, body.erase(at2, stale_class_defs.size()));
            }
        }
        const cg::graph::IndexSummary fourth = index_tree(tree, db_inc);
        cg::test::check(
            r,
            fourth.failed == 0,
            "analysis-warm hierarchy edit run is clean");
        incremental_meta("class-delete");
        // 模板体 tier-3 边缺口见 template_body_tier3_gap 的说明（死类删
        // 除后 site 在批外模板定义文件上的历史丢弃不可追溯）。
        compare_full("class-delete", /*with_tier3_gap=*/true);

        // 收敛：零增量重跑保持投影相等（空脏集
        // 暖启动一切——近乎零工作量、无漂移）。
        const cg::graph::IndexSummary fifth = index_tree(tree, db_inc);
        cg::test::check(
            r,
            fifth.skipped == first.tus && fifth.stale == 0 && fifth.failed == 0,
            std::format(
                "analysis-warm settles to skipped-all "
                "(skipped {}, stale {})",
                fifth.skipped,
                fifth.stale));
        incremental_meta("no-delta");
        compare_full("no-delta", /*with_tier3_gap=*/true);
    }

    const cg::test::Registrar reg_warm{
        "incr.equiv.analysis_warmstart",
        &equiv_analysis_warmstart};

    // ------------------------------------------------ 扇入延迟/收敛
    // ------------ 扇入等价回归（修复定案的固化）：生成共享头扇入语料
    // （fan TU 全含 shared.h， hold TU 带
    // -DFANIN_EXTRA 只含 cond.h——跳过批与宏差 TU 都在场），先 全量，再按 `ccg
    // update` 快速批的同一参数面（seed_files + defer_dispatch）
    // 重放共享头编辑，最后 converge_dispatch（--drain 的收敛尾部）。断言：
    // 延迟批正确标注 dispatch_stale 并撤走脏文件 vd 边；收敛批清掉标注、
    // 派发边回到基线值；整库行集（含 occurrences）与全量重索引逐行相等。
    // 该等价在实测大库 679 TU 上逐字节一致（dropped 33572 的增量与
    // dropped 529998 的全量提交后边表相同——"悬解丢弃"是逐发射观测口径，
    // 不是库终态损失）。
    void fanin_corpus(const fs::path& tree, int fan_count, int hold_count)
    {
        std::error_code ec;
        fs::create_directories(tree / "inc", ec);
        fs::create_directories(tree / "src", ec);
        write_file(
            tree / "inc" / "util.h",
            "#pragma once\nnamespace fanin {\ninline int util_step(int v) "
            "{ return v + 1; }\nstatic int util_local(int v) { return v * 3 "
            "+ 7; }\ninline int util_mix(int v) { return util_step(v) + "
            "util_local(v); }\nint fan_sink(int v);\n}  // namespace "
            "fanin\n");
        write_file(
            tree / "inc" / "cond.h",
            "#pragma once\n#include \"util.h\"\nnamespace fanin {\n#ifdef "
            "FANIN_EXTRA\ninline int rare_calc(int v) { return v * 5 + 1; "
            "}\n#endif\ntemplate <typename T>\nint rare_wrap(T v) { return "
            "rare_calc(v) + 2 + static_cast<int>(sizeof(T)); }\n}  // "
            "namespace fanin\n");
        write_file(
            tree / "inc" / "shared.h",
            "#pragma once\n#include \"cond.h\"\nnamespace fanin {\nstruct "
            "FanShape {\n    virtual int poke(int v) = 0;\n    virtual char "
            "tag() const = 0;\n    virtual ~FanShape();\n};\ninline int "
            "fan_twice(int v) { return fan_sink(v) + util_step(v); }\n}  // "
            "namespace fanin\n");
        std::string cdb = "[\n";
        for (int i = 0; i < fan_count; ++i)
        {
            const std::string name = std::format("fan_{:02}", i);
            std::string       body = std::format(
                "#include \"shared.h\"\nnamespace fanin {{\nstruct Fan{:02} : "
                "FanShape {{\n    int poke(int v) override {{ return v + {} + "
                "util_mix(v); }}\n    char tag() const override {{ return '{}'; "
                "}}\n}};\n",
                i,
                i,
                static_cast<char>('a' + i % 26));
            if (i == 0)
            {
                body += "int fan_sink(int v) { static int acc = 0; acc += v; "
                        "return acc + util_local(v); }\n";
            }
            body += std::format(
                "int fan_work_{:02}(FanShape& s, int v) {{\n    Fan{:02} d;\n    "
                "int r = s.poke(v) + d.poke(v) + d.tag();\n    return r + "
                "fan_twice(v);\n}}\n}}  // namespace fanin\n",
                i,
                i);
            write_file(tree / "src" / (name + ".cpp"), body);
            cdb += std::format(
                "  {{\n    \"directory\": \".\",\n    \"file\": "
                "\"src/{0}.cpp\",\n    \"command\": \"c++ -std=c++23 -I . -I "
                "inc -I X:/ccg-fake/toolchain/include/c++/v1 -I "
                "X:/ccg-fake/toolchain/include -c src/{0}.cpp -o {0}.o\"\n  }}",
                name);
            cdb += (i + 1 < fan_count + hold_count) ? ",\n" : "\n";
        }
        for (int i = 0; i < hold_count; ++i)
        {
            const std::string name = std::format("hold_{:02}", i);
            write_file(
                tree / "src" / (name + ".cpp"),
                std::format(
                    "#include \"cond.h\"\nnamespace fanin {{\nint "
                    "hold_work_{:02}(int v) {{\n    return "
                    "util_mix(v) + fan_sink(v) + rare_calc(v);\n}}\n"
                    "}}  // namespace fanin\n",
                    i));
            cdb += std::format(
                "  {{\n    \"directory\": \".\",\n    \"file\": "
                "\"src/{0}.cpp\",\n    \"command\": \"c++ -std=c++23 -I . -I "
                "inc -I X:/ccg-fake/toolchain/include/c++/v1 -I "
                "X:/ccg-fake/toolchain/include -DFANIN_EXTRA -c src/{0}.cpp -o "
                "{0}.o\"\n  }}",
                name);
            cdb += (i + 1 < hold_count) ? ",\n" : "\n";
        }
        cdb += "]\n";
        write_file(tree / "compile_commands.json", cdb);
    }

    void equiv_fanin_defer_converge(R& r)
    {
        constexpr int        fan_count = 60;
        constexpr int        hold_count = 8;
        cg::test::ScratchDir scratch{
            fs::path{"ccg-incr-scratch"} / "fanin-defer"};
        const fs::path& base = scratch.base;
        const fs::path  tree = base / "tree";
        fanin_corpus(tree, fan_count, hold_count);

        const fs::path                db_inc = base / "inc.db";
        const cg::graph::IndexSummary first = index_tree(tree, db_inc);
        cg::test::check(
            r,
            first.mode == "full" && first.failed == 0,
            "fanin baseline indexes full and clean");
        const std::int64_t vd_base = [&]
        {
            db::GraphStore  store{db_inc.string()};
            const db::Stats st = db::stats(store);
            return st.edges_by_type[static_cast<std::size_t>(
                db::EdgeType::VirtualDispatch)];
        }();
        cg::test::check(
            r,
            first.dispatch_edges > 0 && vd_base > 0,
            std::format(
                "fanin baseline has dispatch edges ({})",
                first.dispatch_edges));

        append_file(
            tree / "inc" / "shared.h",
            "\n// fanin defer/converge regression marker\n");

        // `ccg update <shared.h>` 的快速批参数面：种子强制脏 + 派发延迟。
        const std::string cdb = (tree / "compile_commands.json").string();
        const auto        commands = cg::clang::load(cdb, {});
        const fs::path    db_fast = base / "fast.db";
        // db_fast 从基线库拷贝起步，等价于对同一已索引库跑编辑批（树本体
        // 已带标记，快速批对着它跑）。
        std::error_code cp_ec;
        fs::copy_file(db_inc, db_fast, cp_ec);
        cg::test::check(r, !cp_ec, "fanin fast-batch db seeded from baseline");
        {
            db::GraphStore          store{db_fast.string()};
            cg::graph::IndexOptions opt;
            opt.roots = {cg::util::norm_path(tree.generic_string())};
            opt.progress = false;
            opt.compile_db_path = cdb;
            opt.db_path = db_fast.string();
            opt.seed_files = {
                cg::util::norm_path((tree / "inc" / "shared.h").string())};
            opt.defer_dispatch = true;
            cg::graph::Indexer            indexer{store, std::move(opt)};
            const cg::graph::IndexSummary fast = indexer.run(commands);
            cg::test::check(
                r,
                fast.mode == "incremental" && fast.failed == 0
                    && fast.stale == fan_count,
                std::format(
                    "fanin fast batch reparses the fan TUs "
                    "(mode {}, stale {}, failed {})",
                    fast.mode,
                    fast.stale,
                    fast.failed));
            const auto stale_meta = store.meta("dispatch_stale");
            cg::test::check(
                r,
                stale_meta.has_value() && fast.dispatch_stale_files > 0,
                "fanin fast batch leaves the dispatch_stale mark");
            const db::Stats st = db::stats(store);
            cg::test::check(
                r,
                st.edges_by_type[static_cast<std::size_t>(
                    db::EdgeType::VirtualDispatch)]
                    == 0,
                "fanin fast batch erases dirty-file vd edges");
            const auto dropped = store.meta("edges_unresolved_dropped");
            cg::test::check(
                r,
                dropped.has_value()
                    && dropped == std::to_string(fast.unresolved),
                "fanin fast batch persists edges_unresolved_dropped == "
                "sum.unresolved");
        }

        // `ccg update --drain` 的收敛尾部：converge_dispatch。
        {
            db::GraphStore          store{db_fast.string()};
            cg::graph::IndexOptions conv_opt;
            conv_opt.roots = {cg::util::norm_path(tree.generic_string())};
            conv_opt.progress = false;
            conv_opt.db_path = db_fast.string();
            const cg::graph::IndexSummary conv =
                cg::graph::converge_dispatch(store, conv_opt);
            cg::test::check(
                r,
                !store.meta("dispatch_stale").has_value()
                    && !store.meta("dispatch_stale_files").has_value(),
                "fanin converge clears both stale marks");
            const db::Stats    st = db::stats(store);
            const std::int64_t vd_now =
                st.edges_by_type[static_cast<std::size_t>(
                    db::EdgeType::VirtualDispatch)];
            cg::test::check(
                r,
                vd_now == vd_base,
                std::format(
                    "fanin converge restores vd edges "
                    "({} -> {}, base {})",
                    std::int64_t{0},
                    vd_now,
                    vd_base));
            cg::test::check(
                r,
                conv.dispatch_edges == first.dispatch_edges,
                std::format(
                    "fanin converge dispatch edges match the "
                    "full baseline ({} vs {})",
                    conv.dispatch_edges,
                    first.dispatch_edges));
            cg::test::check(
                r,
                store.meta("edges_unresolved_dropped").value_or("?") == "0",
                "fanin converge zeroes edges_unresolved_dropped");
        }

        // 全量参照 + 行级等价（含 occurrences）。
        const fs::path tree2 = base / "tree-ref";
        copy_tree(tree, tree2);
        const fs::path                db_full = base / "ref.db";
        const cg::graph::IndexSummary fresh = index_tree(tree2, db_full);
        cg::test::check(
            r,
            fresh.failed == 0,
            "fanin full reference run is clean");
        expect_equal(
            r,
            db_fast,
            db_full,
            "fanin-defer-converge",
            cg::util::norm_path(tree.generic_string()),
            cg::util::norm_path(tree2.generic_string()));
    }

    const cg::test::Registrar reg_fanin{
        "incr.equiv.fanin_defer_converge",
        &equiv_fanin_defer_converge};

    // -------------------------------------------- 特化净增批升级等价
    // ----------
    //
    // hierarchy_digest 覆盖 Specializes（特化覆盖补齐）后的端到端边级比对：
    // 编辑只净增一个显式全特化（extends/overrides 边集一字不动），
    // 批内摘要翻转必须走 escalation 整类清空重建；重建后的增量库
    // 与编辑后树的从零全量库逐边一致。摘要若漏看特化（修复前形态），
    // 增量批会带着过期继承图静默通过，派发边与全量库分叉，此用例红。

    void equiv_spec_escalation_edges(R& r)
    {
        cg::test::ScratchDir scratch{
            fs::path{"ccg-incr-scratch"} / "spec-escalation"};
        const fs::path& base = scratch.base;
        const fs::path  tree = base / "tree";
        fs::create_directories(tree);
        std::error_code ec;
        fs::copy_file(
            fs::path{std::string_view{dispatch_fixture_dir}}
                / "spec-digest-flip" / "main.cpp",
            tree / "main.cpp",
            ec);
        cg::test::check(r, !ec, "spec-escalation corpus seeded");
        write_file(
            tree / "compile_commands.json",
            "[\n  {\n    \"directory\": \".\",\n    \"file\": "
            "\"main.cpp\",\n    \"command\": \"c++ -std=c++23 -I . -I "
            "X:/ccg-fake/toolchain/include/c++/v1 -I "
            "X:/ccg-fake/toolchain/include -c main.cpp -o main.o\"\n  "
            "}\n]\n");

        const fs::path db_inc = base / "inc.db";
        {
            const cg::graph::IndexSummary first = index_tree(tree, db_inc);
            cg::test::check(
                r,
                first.mode == "full" && first.failed == 0,
                "spec-escalation baseline indexes full and clean");
        }
        std::optional<std::string> digest_before;
        {
            db::GraphStore store{db_inc.string()};
            digest_before = store.meta("analysis_hierarchy_digest");
            const db::Stats st = db::stats(store);
            cg::test::check(
                r,
                digest_before.has_value()
                    && st.edges_by_type[static_cast<std::size_t>(
                           db::EdgeType::Specializes)]
                           > 0,
                "spec-escalation baseline has specialization edges");
        }

        append_file(
            tree / "main.cpp",
            "\n// digest-flip edit: new explicit specialization; "
            "extends/overrides stay untouched.\ntemplate <>\nstruct "
            "Box<char>\n{\n    int describe() const { return 3; }\n};\n"
            "namespace {\nint use_char_box()\n{\n    Box<char> box;\n   "
            " return box.describe();\n}\n}  // namespace\n");

        {
            const cg::graph::IndexSummary second = index_tree(tree, db_inc);
            cg::test::check(
                r,
                second.mode == "incremental" && second.failed == 0,
                std::format(
                    "spec-escalation edit batch runs incremental and clean "
                    "(mode {}, failed {})",
                    second.mode,
                    second.failed));
        }
        {
            db::GraphStore store{db_inc.string()};
            const auto digest_after = store.meta("analysis_hierarchy_digest");
            cg::test::check(
                r,
                digest_before.has_value() && digest_after.has_value()
                    && *digest_before != *digest_after,
                "spec-escalation edit flips the hierarchy digest (batch "
                "walked the escalation path)");
        }

        // 全量参照尾：编辑后树从零全量索引，与升级重建后的增量库
        // 逐边比对（符号/边/包含/导入/文件五投影）。
        compare_full_reference(r, base, tree, db_inc, "spec-escalation");
    }

    const cg::test::Registrar reg_spec_escalation{
        "incr.equiv.spec_escalation_edges",
        &equiv_spec_escalation_edges};

    // -------------------------------------------- 静态判证列幂等门 ----------
    //
    // receiver_static 分列后的端到端幂等门：伪造 hierarchy digest 翻转
    // 触发 escalation 整类清空重建（第二次全量形态重解析），全部站点在
    // 上轮写回的披露并集在场的情况下重解析——vd 边数、收窄命中面与
    // receiver_static 行集必须与首轮全量逐值相等。修复前装载把
    // receiver_types 并集反读当静态面，COM 根层收窄整族失效，二轮边数
    // 膨胀（大库 15,176 -> 18,849 的小语料同构形态）。

    [[nodiscard]]
    auto receiver_static_rows(db::GraphStore& store) -> std::vector<std::string>
    {
        std::vector<std::string> rows;
        for (const db::CallSiteRow& cs : db::all_call_sites(store))
        {
            rows.push_back(
                std::format(
                    "{}:{}:{} -> {}",
                    cs.callee_name,
                    cs.line,
                    cs.col,
                    cs.receiver_static));
        }
        std::ranges::sort(rows);
        return rows;
    }

    void equiv_receiver_static_idempotent_reparse(R& r)
    {
        cg::test::ScratchDir scratch{
            fs::path{"ccg-incr-scratch"} / "receiver-static"};
        const fs::path& base = scratch.base;
        const fs::path  tree = base / "tree";
        fs::create_directories(tree);
        std::error_code ec;
        fs::copy_file(
            fs::path{std::string_view{dispatch_fixture_dir}} / "recv-scope"
                / "main.cpp",
            tree / "main.cpp",
            ec);
        // 第二 TU 只为承载二轮编辑：它的站点行被 parse 重插时两列同回
        // 首写形态——污染装载面只存在于"未被重插的 TU"（main.cpp 的
        // 虚站点保持上一轮写回的披露并集），单 TU 语料看不见本门。
        write_file(
            tree / "probe.cpp",
            "int untouched_probe() { return 42; }\n");
        cg::test::check(r, !ec, "receiver-static corpus seeded");
        write_file(
            tree / "compile_commands.json",
            "[\n  {\n    \"directory\": \".\",\n    \"file\": "
            "\"main.cpp\",\n    \"command\": \"c++ -std=c++23 -I . -I "
            "X:/ccg-fake/toolchain/include/c++/v1 -I "
            "X:/ccg-fake/toolchain/include -c main.cpp -o main.o\"\n  "
            "},\n  {\n    \"directory\": \".\",\n    \"file\": "
            "\"probe.cpp\",\n    \"command\": \"c++ -std=c++23 -I . -I "
            "X:/ccg-fake/toolchain/include/c++/v1 -I "
            "X:/ccg-fake/toolchain/include -c probe.cpp -o probe.o\"\n  "
            "}\n]\n");

        const fs::path                db_inc = base / "inc.db";
        const cg::graph::IndexSummary first = index_tree(tree, db_inc);
        cg::test::check(
            r,
            first.mode == "full" && first.failed == 0,
            "receiver-static baseline indexes full and clean");

        const auto vd_of = [&](db::GraphStore& store) -> std::int64_t
        {
            const db::Stats st = db::stats(store);
            return st.edges_by_type[static_cast<std::size_t>(
                db::EdgeType::VirtualDispatch)];
        };
        const auto narrowed_of = [&](db::GraphStore& store) -> std::int64_t
        {
            std::int64_t hit = 0;
            for (const db::CallSiteRow& cs : db::all_call_sites(store))
            {
                hit += cs.candidates.find("receiver_scope_narrowed")
                               != std::string::npos
                           ? 1
                           : 0;
            }
            return hit;
        };
        std::int64_t             vd_first = 0;
        std::int64_t             narrowed_first = 0;
        std::vector<std::string> static_first;
        {
            db::GraphStore store{db_inc.string()};
            vd_first = vd_of(store);
            narrowed_first = narrowed_of(store);
            static_first = receiver_static_rows(store);
        }
        cg::test::check(
            r,
            vd_first > 0 && narrowed_first > 0,
            std::format(
                "receiver-static baseline has dispatch edges and narrowed "
                "sites (vd {}, narrowed {})",
                vd_first,
                narrowed_first));

        // 伪造 digest 翻转（清空库内摘要）+ 真实编辑：批内摘要比对必翻
        // -> escalation 整类清空 + 全站点重解析。
        {
            db::GraphStore store{db_inc.string()};
            store.set_meta("analysis_hierarchy_digest", "");
        }
        append_file(
            tree / "probe.cpp",
            "\n// receiver-static idempotence probe\n");
        const cg::graph::IndexSummary second = index_tree(tree, db_inc);
        cg::test::check(
            r,
            second.mode == "incremental" && second.failed == 0,
            std::format(
                "receiver-static reparse batch runs incremental and clean "
                "(mode {}, failed {})",
                second.mode,
                second.failed));
        // 防恒绿门：伪造翻转若未真正走到 escalation（派发边整类清空 +
        // 全站点决议重做），下方全部"逐值不变"断言会在什么都没发生的
        // 二轮上平凡通过——dispatch_escalations 大于零钉住升级腿确实
        // 走过（rounds/work_items 只量传播固定点，本语料收敛态下升级
        // 批也恒为 1 轮 0 工作项，实测不具判别力）。
        cg::test::check(
            r,
            second.dispatch_escalations > 0,
            std::format(
                "the escalation reparse truly re-runs the analysis face "
                "(escalation batches {}, rounds {}, work items {})",
                second.dispatch_escalations,
                second.analysis_rounds,
                second.analysis_work_items));

        {
            db::GraphStore store{db_inc.string()};
            cg::test::check(
                r,
                vd_of(store) == vd_first,
                std::format(
                    "the escalation reparse is edge-idempotent (vd {} -> "
                    "{}, first {})",
                    vd_first,
                    vd_of(store),
                    vd_first));
            cg::test::check(
                r,
                narrowed_of(store) == narrowed_first,
                std::format(
                    "the narrowing face survives the reparse ({} -> {})",
                    narrowed_first,
                    narrowed_of(store)));
            cg::test::check(
                r,
                receiver_static_rows(store) == static_first,
                "receiver_static rows are byte-identical across the "
                "reparse");
            // 污染面确实在场：IPic 站点的披露列仍含界外动态类型——幂等
            // 是穿过污染面保持的，不是污染面消失造成的。
            bool union_face = false;
            for (const db::CallSiteRow& cs : db::all_call_sites(store))
            {
                if (cs.callee_name == "Release"
                    && cs.receiver_static.find("|IPic|") != std::string::npos
                    && cs.receiver_types.find("|ToneA|") != std::string::npos)
                {
                    union_face = true;
                }
            }
            cg::test::check(
                r,
                union_face,
                "the disclosure column still widens past the static face");
        }
    }

    const cg::test::Registrar reg_recv_static_idem{
        "incr.equiv.receiver_static_idempotent_reparse",
        &equiv_receiver_static_idempotent_reparse};

    // -------------------------------------------- 可达性清扫快路径 ----------
    //
    // 无删除且无包含行净减少的提交批整场跳过全局可达性清扫
    // （不读全表、不走不可达判定）。快路径与强制全量清扫
    // （CCG_TEST_DISABLE_SWEEP_FASTPATH=1 对照臂）在无删除批上
    // 必须产出同一库终态；有删除的批（删文件/改包含/模拟移动、
    // 三种删除来源各自）必须照走全量清扫且结果正确。

    // 等价测试：无删除批（单文件体编辑）跳过清扫后的库终态 ==
    // 强制全量清扫的库终态 == 编辑后树的从零全量参照（三方对照）。
    void equiv_sweep_fastpath_skips_clean(R& r)
    {
        cg::test::ScratchDir scratch{
            fs::path{"ccg-incr-scratch"} / "sweep-fast"};
        const fs::path& base = scratch.base;
        const fs::path  tree = base / "tree";
        copy_tree(fs::path{std::string_view{fixture_dir}}, tree);
        const fs::path db_fast = base / "fast.db";
        const fs::path db_full = base / "forced.db";
        for (const fs::path& db : {db_fast, db_full})
        {
            const cg::graph::IndexSummary first = index_tree(tree, db);
            cg::test::check(
                r,
                first.mode == "full" && first.failed == 0,
                "sweep-fast baseline indexes full and clean");
        }

        // 单文件体编辑：main.cpp 顶部插一行注释——无删除、无包含行变化。
        {
            const fs::path    victim = tree / "main.cpp";
            std::string       text = read_file(victim);
            const std::size_t at = text.find("int main()");
            cg::test::check(
                r,
                at != std::string::npos,
                "sweep-fast anchor found");
            if (at == std::string::npos)
            {
                return;
            }
            write_file(victim, text.insert(at, "// sweep-fast probe\n"));
        }

        const cg::graph::IndexSummary run_fast = index_tree(tree, db_fast);
        cg::test::check(
            r,
            run_fast.mode == "incremental" && run_fast.stale >= 1
                && run_fast.purged_files == 0
                && run_fast.reachability_sweeps_skipped == 1,
            std::format(
                "sweep-fast arm skips the sweep on a "
                "no-deletion batch (mode {}, stale {}, "
                "purged {}, skipped {})",
                run_fast.mode,
                run_fast.stale,
                run_fast.purged_files,
                run_fast.reachability_sweeps_skipped));

        const cg::test::EnvScope knob{"CCG_TEST_DISABLE_SWEEP_FASTPATH"};
        knob.set("1");
        const cg::graph::IndexSummary run_forced = index_tree(tree, db_full);
        cg::test::check(
            r,
            run_forced.mode == "incremental"
                && run_forced.stale == run_fast.stale
                && run_forced.purged_files == 0
                && run_forced.reachability_sweeps_skipped == 0,
            std::format(
                "forced-sweep arm runs the full sweep and "
                "purges nothing (mode {}, stale {}, purged "
                "{}, skipped {})",
                run_forced.mode,
                run_forced.stale,
                run_forced.purged_files,
                run_forced.reachability_sweeps_skipped));

        // 两臂库终态逐投影一致（同一树，无 strip 换名）。
        expect_equal(
            r,
            db_fast,
            db_full,
            "sweep-fast-two-arm",
            cg::util::norm_path(tree.generic_string()),
            cg::util::norm_path(tree.generic_string()));
        // 对编辑后树的从零全量参照也一致（第三方）。
        compare_full_reference(r, base, tree, db_fast, "sweep-fast");
    }

    // 有删除的批照走全量清扫：删文件（磁盘消失+引用移除）。
    void sweep_fastpath_deleted_file(R& r)
    {
        cg::test::ScratchDir scratch{
            fs::path{"ccg-incr-scratch"} / "sweep-del"};
        const fs::path& base = scratch.base;
        const fs::path  tree = base / "tree";
        copy_tree(fs::path{std::string_view{fixture_dir}}, tree);
        const fs::path db_file = base / "ccg.db";
        (void)index_tree(tree, db_file);

        // 删 core/vec.hpp（磁盘消失）并移除 main.cpp 的 include 与使用。
        std::error_code ec;
        fs::remove(tree / "core" / "vec.hpp", ec);
        const fs::path victim = tree / "main.cpp";
        std::string    text = read_file(victim);
        const auto drop = [&](std::string_view needle, std::string_view with)
        {
            const std::size_t at = text.find(needle);
            cg::test::check(
                r,
                at != std::string::npos,
                "sweep-del anchor found");
            if (at != std::string::npos)
            {
                text.replace(at, needle.size(), with);
            }
        };
        drop("#include \"core/vec.hpp\"\n", "");
        drop("    const core::Vec3 up{1.0, 2.0, 3.0};\n", "");
        drop(
            "    const core::Vec3 blended = up.scaled(2.0) + up * 0.5 + 0.25 * up;\n",
            "");
        drop(
            "return (ticks > 0 && blended.length() > 0.0 && st.levels > 0) ? 0 : 1;",
            "return (ticks > 0 && st.levels > 0) ? 0 : 1;");
        write_file(victim, text);

        const cg::graph::IndexSummary second = index_tree(tree, db_file);
        bool                          vec_gone = true;
        {
            db::GraphStore store{db_file.string()};
            for (const db::FileRow& fr : db::all_files(store))
            {
                if (fr.path.ends_with("core/vec.hpp"))
                {
                    vec_gone = false;
                }
            }
        }
        cg::test::check(
            r,
            second.failed == 0 && second.purged_files == 1
                && second.reachability_sweeps_skipped == 0 && vec_gone,
            std::format(
                "deleted file batch runs the full sweep and "
                "purges the row (failed {}, purged {}, "
                "skipped {}, vec.hpp swept: {})",
                second.failed,
                second.purged_files,
                second.reachability_sweeps_skipped,
                vec_gone));
    }

    // 有删除的批照走全量清扫：改包含（仅移除 include 与使用，文件保留）。
    void sweep_fastpath_include_removed(R& r)
    {
        cg::test::ScratchDir scratch{
            fs::path{"ccg-incr-scratch"} / "sweep-inc"};
        const fs::path& base = scratch.base;
        const fs::path  tree = base / "tree";
        copy_tree(fs::path{std::string_view{fixture_dir}}, tree);
        const fs::path db_file = base / "ccg.db";
        (void)index_tree(tree, db_file);

        const fs::path victim = tree / "main.cpp";
        std::string    text = read_file(victim);
        const auto drop = [&](std::string_view needle, std::string_view with)
        {
            const std::size_t at = text.find(needle);
            cg::test::check(
                r,
                at != std::string::npos,
                "sweep-inc anchor found");
            if (at != std::string::npos)
            {
                text.replace(at, needle.size(), with);
            }
        };
        drop("#include \"core/vec.hpp\"\n", "");
        drop("    const core::Vec3 up{1.0, 2.0, 3.0};\n", "");
        drop(
            "    const core::Vec3 blended = up.scaled(2.0) + up * 0.5 + 0.25 * up;\n",
            "");
        drop(
            "return (ticks > 0 && blended.length() > 0.0 && st.levels > 0) ? 0 : 1;",
            "return (ticks > 0 && st.levels > 0) ? 0 : 1;");
        write_file(victim, text);

        const cg::graph::IndexSummary second = index_tree(tree, db_file);
        cg::test::check(
            r,
            second.failed == 0 && second.purged_files == 1
                && second.reachability_sweeps_skipped == 0,
            std::format(
                "include-removed batch runs the full sweep "
                "(failed {}, purged {}, skipped {})",
                second.failed,
                second.purged_files,
                second.reachability_sweeps_skipped));
    }

    // 有删除的批照走全量清扫：模拟移动（旧路径消失 + 新路径出现 + 包含改写）。
    void sweep_fastpath_simulated_move(R& r)
    {
        cg::test::ScratchDir scratch{
            fs::path{"ccg-incr-scratch"} / "sweep-move"};
        const fs::path& base = scratch.base;
        const fs::path  tree = base / "tree";
        copy_tree(fs::path{std::string_view{fixture_dir}}, tree);
        const fs::path db_file = base / "ccg.db";
        (void)index_tree(tree, db_file);

        // core/vec.hpp -> core/vectwo.hpp（内容不动，include 拼写改写）。
        std::error_code ec;
        const fs::path  old_hpp = tree / "core" / "vec.hpp";
        const fs::path  new_hpp = tree / "core" / "vectwo.hpp";
        fs::rename(old_hpp, new_hpp, ec);
        cg::test::check(r, !ec, "sweep-move rename succeeds");
        if (ec)
        {
            return;
        }
        const fs::path    victim = tree / "main.cpp";
        std::string       text = read_file(victim);
        const std::size_t at = text.find("#include \"core/vec.hpp\"");
        cg::test::check(r, at != std::string::npos, "sweep-move anchor found");
        if (at == std::string::npos)
        {
            return;
        }
        text.replace(
            at,
            std::string_view{"#include \"core/vec.hpp\""}.size(),
            "#include \"core/vectwo.hpp\"");
        write_file(victim, text);

        const cg::graph::IndexSummary second = index_tree(tree, db_file);
        bool                          old_gone = true;
        bool                          new_present = false;
        {
            db::GraphStore store{db_file.string()};
            for (const db::FileRow& fr : db::all_files(store))
            {
                if (fr.path.ends_with("core/vec.hpp"))
                {
                    old_gone = false;
                }
                if (fr.path.ends_with("core/vectwo.hpp"))
                {
                    new_present = true;
                }
            }
        }
        cg::test::check(
            r,
            second.failed == 0 && second.purged_files == 1
                && second.reachability_sweeps_skipped == 0 && old_gone
                && new_present,
            std::format(
                "simulated move runs the full sweep and "
                "retires the old row (failed {}, purged {}, "
                "skipped {}, old swept: {}, new present: {})",
                second.failed,
                second.purged_files,
                second.reachability_sweeps_skipped,
                old_gone,
                new_present));
    }

    // 三种删除来源的针对性测试：每种来源在场 → 不跳过。
    // ① 变更集的删除/失踪主文件（missing_fids）。
    void sweep_fastpath_source_missing_main(R& r)
    {
        cg::test::ScratchDir scratch{
            fs::path{"ccg-incr-scratch"} / "sweep-src-miss"};
        const fs::path& base = scratch.base;
        const fs::path  tree = base / "tree";
        copy_tree(fs::path{std::string_view{fixture_dir}}, tree);
        const fs::path db_file = base / "ccg.db";
        (void)index_tree(tree, db_file);

        // 主文件从磁盘消失，编译数据库条目保留 → p.missing → missing_fids。
        std::error_code ec;
        fs::remove(tree / "strategy" / "momentum.cpp", ec);
        const cg::graph::IndexSummary second = index_tree(tree, db_file);
        bool                          row_kept = false;
        {
            db::GraphStore store{db_file.string()};
            for (const db::FileRow& fr : db::all_files(store))
            {
                if (fr.path.ends_with("strategy/momentum.cpp"))
                {
                    row_kept = true;
                }
            }
        }
        cg::test::check(
            r,
            second.reachability_sweeps_skipped == 0 && row_kept,
            std::format(
                "missing-main batch runs the full sweep "
                "(skipped {}, row kept: {})",
                second.reachability_sweeps_skipped,
                row_kept));
    }

    // ② 新鲜度扫描发现的文件消失/离开编译库（抹除集 erased）。
    void sweep_fastpath_source_entry_left(R& r)
    {
        cg::test::ScratchDir scratch{
            fs::path{"ccg-incr-scratch"} / "sweep-src-left"};
        const fs::path& base = scratch.base;
        const fs::path  tree = base / "tree";
        copy_tree(fs::path{std::string_view{fixture_dir}}, tree);
        const fs::path db_file = base / "ccg.db";
        (void)index_tree(tree, db_file);

        // 条目离开编译数据库（文件保留在盘上）→ 源文件无人包含 → 抹除集。
        const fs::path cdb = tree / "compile_commands.json";
        write_file(cdb, drop_db_entry(read_file(cdb), "momentum.cpp"));
        const cg::graph::IndexSummary second = index_tree(tree, db_file);
        bool                          row_gone = true;
        {
            db::GraphStore store{db_file.string()};
            for (const db::FileRow& fr : db::all_files(store))
            {
                if (fr.path.ends_with("strategy/momentum.cpp"))
                {
                    row_gone = false;
                }
            }
        }
        cg::test::check(
            r,
            second.reachability_sweeps_skipped == 0 && second.purged_files == 1
                && row_gone,
            std::format(
                "entry-left batch runs the full sweep and "
                "purges the retired main (skipped {}, purged "
                "{}, row gone: {})",
                second.reachability_sweeps_skipped,
                second.purged_files,
                row_gone));
    }

    // ③ 本批清除逻辑产生的行删除：全败清除（all_failed_fids 的 purge）
    //    与重写覆盖差（改包含形态，见 sweep_fastpath_include_removed）。
    void sweep_fastpath_source_all_failed(R& r)
    {
        cg::test::ScratchDir scratch{
            fs::path{"ccg-incr-scratch"} / "sweep-src-fail"};
        const fs::path& base = scratch.base;
        const fs::path  tree = base / "tree";
        copy_tree(fs::path{std::string_view{fixture_dir}}, tree);
        const fs::path db_file = base / "ccg.db";
        (void)index_tree(tree, db_file);

        // 破坏专属头（文件保留在盘上、内容注入语法错误）→ 观察者 TU
        // 全败 → all_failed_fids 的 purge 产生行删除。
        const fs::path victim = tree / "strategy" / "momentum.hpp";
        append_file(victim, "\nnamespace incr_broken {\nthis is not c++;\n}\n");
        const cg::graph::IndexSummary second = index_tree(tree, db_file);
        cg::test::check(
            r,
            second.failed >= 1 && second.reachability_sweeps_skipped == 0,
            std::format(
                "all-failed batch runs the full sweep "
                "(failed {}, skipped {})",
                second.failed,
                second.reachability_sweeps_skipped));
    }

    const cg::test::Registrar reg_sweepfast{
        "incr.equiv.sweep_fastpath_skips_clean",
        &equiv_sweep_fastpath_skips_clean};
    const cg::test::Registrar reg_sweepdelfile{
        "incr.sweep_fastpath.deleted_file",
        &sweep_fastpath_deleted_file};
    const cg::test::Registrar reg_sweepdelinc{
        "incr.sweep_fastpath.include_removed",
        &sweep_fastpath_include_removed};
    const cg::test::Registrar reg_sweepmove{
        "incr.sweep_fastpath.simulated_move",
        &sweep_fastpath_simulated_move};
    const cg::test::Registrar reg_sweepsrcmiss{
        "incr.sweep_fastpath.source_missing_main",
        &sweep_fastpath_source_missing_main};
    const cg::test::Registrar reg_sweepsrcleft{
        "incr.sweep_fastpath.source_entry_left",
        &sweep_fastpath_source_entry_left};
    const cg::test::Registrar reg_sweepsrcfail{
        "incr.sweep_fastpath.source_all_failed",
        &sweep_fastpath_source_all_failed};

    // ---------------------- import 闭包（modules 语料） ----------------------
    //
    // sample2 是 #include 语料；modules 语料上"编辑被依赖方 -> 消费者重
    // 解析"的边是 import 而非 #include，包含闭包读不到它。这里的两个
    // 用例钉死：模块接口编辑后，import 它的消费 TU 进入重解析闭包，且
    // 增量库与从零全量重建逐位等价。

    // 迷你模块夹具：一个接口单元 + 一个 import 它的经典消费 TU。
    // 两个导出函数中 api_dropped 是编辑对象（场景一删它——消费 TU 不
    // 使用；场景二删 api_used——消费 TU 立即解析失败）。
    //
    // @internal compile db 里一律写绝对路径：自建链对记录命令的文件字段
    // 按"root + 相对路径"拼工作路径，相对拼写会被拼出嵌套路径而误判
    // 盘缺（真实 CMake 数据库总是绝对路径）。
    void write_module_fixture(const fs::path& tree)
    {
        std::error_code ec;
        fs::create_directories(tree, ec);
        write_file(
            tree / "api.ixx",
            "export module inclosure.api;\n"
            "export inline int api_used() { return 1; }\n"
            "export inline int api_dropped() { return 2; }\n");
        write_file(
            tree / "use.cpp",
            "import inclosure.api;\n"
            "int consumer_entry() { return api_used(); }\n");
        std::error_code   abs_ec;
        const fs::path    abs = fs::absolute(tree, abs_ec).lexically_normal();
        const std::string t = cg::util::norm_path(abs.generic_string());
        write_file(
            tree / "compile_commands.json",
            std::format(
                "[\n"
                " {{\"directory\": \"{}\", \"arguments\": [\"clang++\", "
                "\"-std=c++20\", \"-c\", \"{}/api.ixx\"], \"file\": "
                "\"{}/api.ixx\"}},\n"
                " {{\"directory\": \"{}\", \"arguments\": [\"clang++\", "
                "\"-std=c++20\", \"-c\", \"{}/use.cpp\"], \"file\": "
                "\"{}/use.cpp\"}}\n"
                "]\n",
                t,
                t,
                t,
                t,
                t,
                t));
    }

    /**
     * @brief 模块树的索引入口（拿回 IndexSummary 供 stale 断言）。
     *
     * @internal 与 sample2 用例的 index_tree 不同：模块 TU 的解析命令必须
     * 先经 CLI 主干的 resolve_loaded_commands——route_db_module_tus 的
     * 自建链在那里生产 BMI、给消费者附着 module_feeds 投喂、给接口单元
     * 归属 module_output，而 import 闭包接边所依赖的正是这两面命令事实。
     * 直连 Indexer::run 的裸命令没有投喂面，模块 TU 无从解析。路径一律
     * 绝对化：自建链的 clang 子进程与 BMI 缓存路径不依赖进程 cwd，而
     * 相对拼写的 cdb 文件字段会被按 root 再拼一层而误判盘缺。
     */
    [[nodiscard]]
    auto index_module_tree(const fs::path& tree, const fs::path& db_file)
        -> cg::graph::IndexSummary
    {
        std::error_code   abs_ec;
        const fs::path    abs = fs::absolute(tree, abs_ec).lexically_normal();
        const std::string cdb = (abs / "compile_commands.json").string();
        const std::string root = cg::util::norm_path(abs.generic_string());
        std::vector<cg::clang::CompileCommand> commands =
            cg::clang::load(cdb, std::vector<std::string>{root});
        cg::cli::Invocation inv = cg::cli::parse_args(
            std::vector<std::string>{
                "index",
                root,
                "--compile-commands",
                cdb,
                "--config",
                "module"});
        // BMI 自建缓存目录从 db 路径推导（run_index 在布局解析后挂的同
        // 一字段）：缓存放进本用例的 scratch，不污染真实配置目录。
        const std::string db_abs =
            fs::absolute(db_file, abs_ec).lexically_normal().generic_string();
        inv.db_path = db_abs;
        const cg::cli::detail::ResolvedCommands resolved =
            cg::cli::detail::resolve_loaded_commands(
                inv,
                {root},
                std::move(commands),
                {},
                cdb);
        db::GraphStore          store{db_abs};
        cg::graph::IndexOptions opt;
        opt.roots = {root};
        opt.compile_db_path = cdb;
        opt.db_path = db_abs;
        opt.progress = false;
        cg::graph::Indexer indexer{store, std::move(opt)};
        return indexer.run(resolved.commands);
    }

    // 模块 scratch 落系统临时目录（短路径）：自建链的 BMI 缓存路径 =
    // scratch + 引擎键 + 配置键 + pcm 名，按树内 scratch 的深度叠加后
    // 会恰好越过 260 字符的 Win32 路径上限，pcm 产物读回失败（产物侧
    // 的既有边界，与 import 闭包无关，见实施报告）。
    auto module_scratch(std::string_view name) -> cg::test::ScratchDir
    {
        return cg::test::ScratchDir{
            fs::temp_directory_path() / "ccg-incr-import-closure" / name};
    }

    void equiv_import_closure_consumer_reparsed(R& r)
    {
        cg::test::ScratchDir scratch = module_scratch("reparse");
        const fs::path&      base = scratch.base;
        const fs::path       tree = base / "tree";
        write_module_fixture(tree);

        const fs::path                db_inc = base / "inc.db";
        const cg::graph::IndexSummary first = index_module_tree(tree, db_inc);
        cg::test::check(
            r,
            first.mode == "full" && first.failed == 0,
            std::format(
                "[import-closure-reparse] baseline module index is full and "
                "clean (mode {}, failed {})",
                first.mode,
                first.failed));

        // 编辑接口的导出面：删掉消费 TU 不使用的 api_dropped。use.cpp
        // 的字节与指纹纹丝未动，#include 图上它与 api.ixx 之间没有任何
        // 边——把它拉进重解析批的唯一合法理由就是 import（modules 时代
        // 的包含）。构建系统同款级联：BMI 一变，全部 import 消费者重编。
        write_file(
            tree / "api.ixx",
            "export module inclosure.api;\n"
            "export inline int api_used() { return 1; }\n");
        const cg::graph::IndexSummary second = index_module_tree(tree, db_inc);
        cg::test::check(
            r,
            second.mode == "incremental" && second.failed == 0,
            std::format(
                "[import-closure-reparse] edit batch is incremental and "
                "clean (mode {}, failed {})",
                second.mode,
                second.failed));
        cg::test::check(
            r,
            second.stale >= 2,
            std::format(
                "[import-closure-reparse] the interface edit re-parses the "
                "import consumer too (stale {}, want >= 2: interface + "
                "consumer)",
                second.stale));

        // 增量库与从零全量重建逐位等价（clean 参照）。参照树重新走一遍
        // 夹具+同款编辑（cdb 内嵌绝对路径，复制树会指回原树）。
        const fs::path tree2 = base / "tree-full";
        write_module_fixture(tree2);
        write_file(
            tree2 / "api.ixx",
            "export module inclosure.api;\n"
            "export inline int api_used() { return 1; }\n");
        const fs::path                db_full = base / "full.db";
        const cg::graph::IndexSummary fresh = index_module_tree(tree2, db_full);
        cg::test::check(
            r,
            fresh.failed == 0,
            "[import-closure-reparse] full reference run is clean");
        expect_equal(
            r,
            db_inc,
            db_full,
            "import-closure-reparse",
            cg::util::norm_path(tree.generic_string()),
            cg::util::norm_path(tree2.generic_string()));
    }

    void equiv_import_closure_edit_matches_full(R& r)
    {
        cg::test::ScratchDir scratch = module_scratch("equivalence");
        const fs::path&      base = scratch.base;
        const fs::path       tree = base / "tree";
        write_module_fixture(tree);

        const fs::path                db_inc = base / "inc.db";
        const cg::graph::IndexSummary first = index_module_tree(tree, db_inc);
        cg::test::check(
            r,
            first.mode == "full" && first.failed == 0,
            std::format(
                "[import-closure-equivalence] baseline module index is full "
                "and clean (mode {}, failed {})",
                first.mode,
                first.failed));

        // 删掉消费 TU 正在调用的 api_used：接口自身仍可解析，消费 TU
        // 对未声明名字的使用是硬错误。增量批必须把消费 TU 拉进闭包、
        // 让它以与全量重建完全相同的方式失败——消费 TU 若被跳过，它的
        // 旧行（符号/调用边）就会留在增量库里，全量重建里它们根本不
        // 存在（parse-failed），增量库从此过期。
        write_file(
            tree / "api.ixx",
            "export module inclosure.api;\n"
            "export inline int api_dropped() { return 2; }\n");
        const cg::graph::IndexSummary second = index_module_tree(tree, db_inc);
        cg::test::check(
            r,
            second.mode == "incremental",
            std::format(
                "[import-closure-equivalence] edit batch is incremental "
                "(mode {})",
                second.mode));
        cg::test::check(
            r,
            second.failed == 1,
            std::format(
                "[import-closure-equivalence] the re-parsed consumer fails "
                "on the removed export (failed {}, want 1)",
                second.failed));
        bool consumer_failed = false;
        {
            db::GraphStore store{db_inc.string()};
            for (const db::FileRow& fr : db::all_files(store))
            {
                if (fr.path.ends_with("use.cpp"))
                {
                    consumer_failed =
                        fr.status == cg::util::ParseStatus::Failed;
                }
            }
        }
        cg::test::check(
            r,
            consumer_failed,
            "[import-closure-equivalence] the consumer's file row is Failed "
            "in the incremental db (it really entered the parse batch)");

        // 失败也必须失败得一致：与全量重建逐位等价（两库的 consumer 均
        // 为 parse-failed；参照轮 failed==1 是本形状的预期，不走 clean
        // 口径）。参照树重新走一遍夹具+同款编辑。
        const fs::path tree2 = base / "tree-full";
        write_module_fixture(tree2);
        write_file(
            tree2 / "api.ixx",
            "export module inclosure.api;\n"
            "export inline int api_dropped() { return 2; }\n");
        const fs::path                db_full = base / "full.db";
        const cg::graph::IndexSummary fresh = index_module_tree(tree2, db_full);
        cg::test::check(
            r,
            fresh.failed == 1,
            std::format(
                "[import-closure-equivalence] full reference run fails the "
                "consumer exactly (failed {}, want 1)",
                fresh.failed));
        expect_equal(
            r,
            db_inc,
            db_full,
            "import-closure-equivalence",
            cg::util::norm_path(tree.generic_string()),
            cg::util::norm_path(tree2.generic_string()));
    }

    const cg::test::Registrar reg_importclosure_reparse{
        "incr.equiv.import_closure.consumer_reparsed_on_interface_edit",
        &equiv_import_closure_consumer_reparsed};
    const cg::test::Registrar reg_importclosure_equiv{
        "incr.equiv.import_closure.edit_matches_full_reindex",
        &equiv_import_closure_edit_matches_full};

    // CMake 形态声明 PCH 语料（target_precompile_headers 产出的 cdb 形态：
    // GNU 方言的 -emit-pch 生产者条目 + -include-pch 消费者条目 + -include
    // 包装伞头，消费者源文件零文本包含）的提交段回归。影子 PCH 的装载不
    // 重放伞头的包含预处理，伞头域（包装伞头与其包含的头）在 includes 表
    // 上零边——可达性清扫曾把它们误判不可达，行删除的级联把伞头闭包上的
    // 符号行一并带走，随后的贡献票发射引用死符号 id，提交事务以
    // FOREIGN KEY constraint failed 整批回滚。修复=伞头域进可达性种子
    // （消费者声明的强制包含头 + 本批符号写入面）。断言：影子规划成立、
    // 索引成功零失败、伞头符号与伞头域文件行全部入库。
    void declared_pch_cmake_corpus_commit(R& r)
    {
        if (cg::clang::driver_info().driver.empty())
        {
            cg::test::check(
                r,
                true,
                "declared PCH commit skipped: no clang driver");
            return;
        }
        cg::test::ScratchDir scratch{
            fs::path{"ccg-incr-scratch"} / "declared-pch-cmake"};
        // 绝对化语料根：影子 PCH 的复刻驱动器以绝对路径约定重放
        // producer argv（cdb 的 directory/file 也从它拼装），相对根
        // 会让影子构建在驱动器子进程的 cwd 上失配。
        const fs::path base = fs::absolute(scratch.base);
        write_file(
            base / "pch.h",
            "#pragma once\nstruct PchCommitMarker\n{\n    int id;\n};\n"
            "inline int pch_commit_answer()\n{\n    return 42;\n}\n");
        write_file(base / "umbrella.hxx", "#include \"pch.h\"\n");
        write_file(base / "pch_wrap.cxx", "#include \"umbrella.hxx\"\n");
        write_file(
            base / "main.cpp",
            "int main()\n{\n    PchCommitMarker m{1};\n"
            "    return m.id + pch_commit_answer() - 43;\n}\n");
        const std::string root = cg::util::norm_path(base.generic_string());
        const std::string db_text = std::format(
            "[{{\"directory\":\"{0}\",\"file\":\"{0}/pch_wrap.cxx\","
            "\"arguments\":[\"clang++\",\"-std=gnu++20\",\"-I\",\"{0}\","
            "\"-Xclang\",\"-emit-pch\",\"-Xclang\",\"-include\",\"-Xclang\","
            "\"{0}/umbrella.hxx\",\"-x\",\"c++-header\",\"-o\","
            "\"{0}/umbrella.hxx.pch\",\"-c\",\"{0}/pch_wrap.cxx\"]}},"
            "{{\"directory\":\"{0}\",\"file\":\"{0}/main.cpp\","
            "\"arguments\":[\"clang++\",\"-std=gnu++20\",\"-I\",\"{0}\","
            "\"-Xclang\",\"-include-pch\",\"-Xclang\","
            "\"{0}/umbrella.hxx.pch\",\"-Xclang\",\"-include\",\"-Xclang\","
            "\"{0}/umbrella.hxx\",\"-c\",\"{0}/main.cpp\",\"-o\","
            "\"{0}/main.o\"]}}]",
            root);
        write_file(base / "compile_commands.json", db_text);
        const std::string cdb_path = cg::util::norm_path(
            (base / "compile_commands.json").generic_string());

        const std::vector<cg::clang::CompileCommand> commands =
            cg::clang::load(cdb_path, {});
        cg::test::check(
            r,
            commands.size() == 2,
            std::format(
                "declared PCH corpus loads two entries (got {})",
                commands.size()));
        if (commands.size() != 2)
        {
            return;
        }
        cg::clang::PchOptions pch_opt;
        pch_opt.ccg_config_dir = root + "/.ccg/pchcommit";
        pch_opt.compile_db_path = cdb_path;
        const cg::clang::PchPlanResult planned =
            cg::clang::plan_pch(commands, commands, pch_opt);
        cg::test::check(
            r,
            planned.consumers == 1 && planned.producers_skipped == 1,
            std::format(
                "shadow plan holds for the corpus (consumers {}, producers "
                "skipped {}, text fallbacks {})",
                planned.consumers,
                planned.producers_skipped,
                planned.text_fallback));

        const fs::path          db_file = base / "ccg.db";
        db::GraphStore          store{db_file.string()};
        cg::graph::IndexOptions opt;
        opt.roots = {root};
        opt.compile_db_path = cdb_path;
        opt.db_path = db_file.string();
        cg::graph::Indexer      indexer{store, std::move(opt)};
        bool                    commit_blew = false;
        cg::graph::IndexSummary sum{};
        try
        {
            sum = indexer.run(planned.commands);
        }
        catch (...)
        {
            commit_blew = true;
        }
        cg::test::check(
            r,
            !commit_blew && sum.failed == 0,
            std::format(
                "commit survives the reachability sweep (exception {}, "
                "failed TUs {})",
                commit_blew,
                sum.failed));
        if (commit_blew || sum.failed != 0)
        {
            return;
        }

        bool umbrella_row = false;
        bool header_row = false;
        for (const db::FileRow& fr : db::all_files(store))
        {
            umbrella_row = umbrella_row || fr.path.ends_with("/umbrella.hxx");
            header_row = header_row || fr.path.ends_with("/pch.h");
        }
        cg::test::check(
            r,
            umbrella_row && header_row,
            std::format(
                "PCH declaration-domain file rows survive the sweep "
                "(umbrella {}, header {})",
                umbrella_row,
                header_row));
        bool marker_symbol = false;
        bool answer_symbol = false;
        for (const db::FullSymbolRow& s : db::all_symbol_rows(store))
        {
            marker_symbol = marker_symbol || s.name == "PchCommitMarker";
            answer_symbol = answer_symbol || s.name == "pch_commit_answer";
        }
        cg::test::check(
            r,
            marker_symbol && answer_symbol,
            std::format(
                "umbrella-closure symbols are in the db (marker {}, "
                "answer {})",
                marker_symbol,
                answer_symbol));
    }

    const cg::test::Registrar reg_declared_pch_commit{
        "incr.declared_pch.cmake_corpus_commit_survives_reachability_sweep",
        &declared_pch_cmake_corpus_commit};

} // namespace

// NOLINTNEXTLINE(clang-diagnostic-main-attached-to-named-module) 用 module-unit
// main 是有意为之：项目每个源文件都是具名模块单元；普通 TU
// 的 main 会破坏这项全模块政策
export int main() { return cg::test::run_all(); }
