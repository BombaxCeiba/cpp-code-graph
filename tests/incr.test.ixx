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
// 无重复子图）。

import std;
import cg.util;
import cg.clang;
import cg.db;
import cg.graph;
import cg.test.harness;

namespace
{

    using R = cg::test::Result;
    namespace fs = std::filesystem;
    namespace db = cg::db;

    constexpr std::string_view fixture_dir = CCG_SAMPLE2_DIR;

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
                    s.name,
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

    void expect_equal(
        R&                 r,
        const fs::path&    inc,
        const fs::path&    full,
        std::string_view   label,
        const std::string& strip_inc,
        const std::string& strip_full)
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
                    a.symbols.at(d.first),
                    d.second));
            return;
        }
        if (a.edges != b.edges)
        {
            cg::test::check(
                r,
                false,
                std::format(
                    "[{}] edges differ: '{}'",
                    label,
                    multiset_diff(a.edges, b.edges)));
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
        R&               r,
        const fs::path&  base,
        const fs::path&  tree,
        const fs::path&  db_inc,
        std::string_view label,
        std::string_view sub = "full",
        bool             force = false)
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
            cg::util::norm_path(tree2.generic_string()));
        (void)fs::remove_all(tree2, ec);
        (void)fs::remove(db_full, ec);
    }

    // 单编辑场景：基线索引、把编辑应用到树上、增量
    // 重索引，再对编辑后树的副本做从零全量索引，
    // 最后规范化比较。
    void scenario(
        R&                                          r,
        const std::string&                          name,
        const std::function<void(const fs::path&)>& edit,
        std::int64_t                                min_stale = 1)
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

        compare_full_reference(r, base, tree, db_inc, name);
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
            });
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
                && second.stale == first.tus,
            std::format(
                "purge-lift-hdr escalates to full coverage "
                "(stale {}, want {})",
                second.stale,
                first.tus));

        compare_full_reference(r, base, tree, db_inc, "purge-lift-hdr");
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

    // ------------------------------------------------ 分析暖启动 ----------
    //
    // 增量类型传播/派发路径（analysis_slots 暖启动 + 脏
    // 闭包重算）必须守住比上方图投影更强的不变量：
    // 穿过一串编辑，派生数据——每个
    // call_sites 解析列与持久化的槽位终态——
    // 始终等于同一最终树的从零全量重索引。
    // static_type_id 刻意不进投影：它读的
    // receiver_types 列被先前的分析扩成了静态+动态——
    // 既有的一次对二次分析不对称（旧的全量重算增量路径上同样如此）。
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

        const auto compare_full = [&](std::string_view label)
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
                cg::util::norm_path(tree2.generic_string()));
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
        compare_full("class-delete");

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
        compare_full("no-delta");
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

} // namespace

// NOLINTNEXTLINE(clang-diagnostic-main-attached-to-named-module) 用 module-unit
// main 是有意为之：项目每个源文件都是具名模块单元；普通 TU
// 的 main 会破坏这项全模块政策
export int main() { return cg::test::run_all(); }
