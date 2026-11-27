/**
 * @module cg.test.scan
 *
 * @brief 符号名表两层扫描测试：两遍各自的命中集合、相邻重复归并、名字
 *        长度上限、空串试探闸、分开关、步数触限名字的计数，以及段间
 *        与段内截止探针在两遍里的干净放弃。
 *
 * 名字池现场写进内存库；截止判定用可调用对象注入，按调用序号触发，
 * 不依赖时钟。
 */

export module cg.test.scan;

import std;
import cg.db;
import cg.util.regex;
import cg.cli.scan.names;
import cg.test.harness;

namespace
{

    using R = cg::test::Result;
    namespace rx = cg::util::regex;
    namespace sn = cg::cli::detail;

    using Found = std::optional<sn::NameScan>;

    /** @brief 一段文本重复若干次拼成的字符串。 */
    [[nodiscard]]
    auto repeated(std::string_view piece, int count) -> std::string
    {
        std::string out;
        out.reserve(piece.size() * static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i)
        {
            out += piece;
        }
        return out;
    }

    // 模式 (.+)(.+)zzqq 下四种结局的名字。全匹配与部分匹配都命中的
    // 短名字、两种语义都不命中的短名字、两种语义都触限的长名字（没有
    // zzqq，两个贪婪分组的切分方式多过步数上限）、只在全匹配上触限的
    // 长名字（真命中在名字前部，尾巴很长：锚定后要把所有切分试完才
    // 肯定不成立，不锚定时在第一个起点上很快命中）。
    constexpr std::string_view HIT_NAME = "abzzqq";
    constexpr std::string_view MISS_NAME = "aaaa";
    const std::string          CUT_BOTH = repeated("aq", 50);
    const std::string          CUT_FULL_ONLY =
        std::string(60, 'a') + "zzqq" + std::string(30, 'b');

    /** @brief 内存库夹具：一个文件里的若干函数符号，名字逐个写入。 */
    struct NameTable
    {
        cg::db::GraphStore store{":memory:"};
        cg::db::FileId     file = 0;
        std::size_t        rows = 0;

        NameTable() { file = store.upsert_file("e:/src/scan.cpp"); }

        /** @brief 写入一个函数符号；同名符号各占一行、共享一个名字。 */
        auto add(std::string_view name) -> void
        {
            const std::string key =
                std::format("function||{}|e:/src/scan.cpp#{}", name, rows);
            store.upsert_symbol(
                {.unique_key = key,
                 .kind = cg::db::SymbolKind::Function,
                 .name = name,
                 .ns = "",
                 .sig_hash = std::nullopt,
                 .file_id = file,
                 .line = 1,
                 .col = 5});
            ++rows;
        }
    };

    /** @brief 期望值：有值的扫描结果（命中名与未判定数）。 */
    [[nodiscard]]
    auto scan_of(
        std::initializer_list<std::string_view> matched,
        std::size_t                             undetermined = 0) -> Found
    {
        return Found{sn::NameScan{
            .matched = std::vector<std::string>{matched.begin(), matched.end()},
            .undetermined = undetermined}};
    }

    /** @brief 实际结果与期望逐项相同（有无值、命中名与顺序、未判定
     *          数）。 */
    [[nodiscard]]
    auto scan_is(const Found& actual, const Found& expected) -> bool
    {
        if (actual.has_value() != expected.has_value())
        {
            return false;
        }
        return !expected.has_value()
               || (actual->undetermined == expected->undetermined
                   && actual->matched == expected->matched);
    }

    /** @brief 截止判定的测试替身：数被调用的次数，从第 trip_at 次调用起
     *          返回真；默认值永不触发。 */
    struct Probe
    {
        int calls = 0;
        int trip_at = std::numeric_limits<int>::max();

        [[nodiscard]]
        auto operator()() -> bool
        {
            return ++calls >= trip_at;
        }
    };

    /** @brief 段行数取 2 时一张四行名字表每遍的探针数：三个段前探针
     *          （两段有行、末段为空）。 */
    constexpr int PROBES_PER_PASS_SMALL = 3;

    /** @brief 名字装进一段的表每遍的探针数：段前一探、空的末段前一探。 */
    constexpr int PROBES_PER_PASS_ONE_SPAN = 2;

    /** @brief 一遍扫描：两种语义的命中集合、相邻重复归并、名字长度
     *          上限、空表。 */
    void scan_cases(R& r)
    {
        NameTable table;
        for (const std::string_view name :
             {"run", "rerun", "run_update", "xrun", "RUN"})
        {
            table.add(name);
        }
        table.add("run"); // 同名的第二个符号：行相邻、名字只算一次
        auto literal = rx::Pattern::compile("run");
        cg::test::check(r, literal.has_value(), "scan: pattern compiles");
        Probe       probe;
        const Found full = sn::scan_symbol_names(
            table.store,
            *literal,
            sn::MatchMode::Full,
            probe,
            2);
        cg::test::check(
            r,
            scan_is(full, scan_of({"run"})),
            "scan: full matching returns the exactly covered name and no "
            "undetermined name");
        // 六行、每段两行：三个有行的段加一个空的末段，各探一次。
        cg::test::check(
            r,
            probe.calls == 4,
            "scan: one probe before every segment, including the empty one");
        probe.calls = 0;
        const Found part = sn::scan_symbol_names(
            table.store,
            *literal,
            sn::MatchMode::Partial,
            probe,
            2);
        cg::test::check(
            r,
            scan_is(part, scan_of({"run", "rerun", "run_update", "xrun"})),
            "scan: partial matching returns every name holding the fragment "
            "in string-table order");
        // 名字长度上限：超过 128 字节的名字不参与匹配。
        table.add(std::string(129, 'a'));
        table.add(std::string(128, 'b'));
        auto        letters = rx::Pattern::compile("[ab]+");
        const Found bounded = sn::scan_symbol_names(
            table.store,
            *letters,
            sn::MatchMode::Full,
            Probe{},
            1000);
        cg::test::check(
            r,
            scan_is(bounded, scan_of({std::string(128, 'b')})),
            "scan: a name over the length limit is skipped, one at the "
            "limit is matched");
        NameTable empty;
        probe.calls = 0;
        const Found none = sn::scan_symbol_names(
            empty.store,
            *literal,
            sn::MatchMode::Full,
            probe,
            2);
        cg::test::check(
            r,
            scan_is(none, scan_of({})) && probe.calls == 1,
            "scan: an empty table costs one probe and yields no hits");
    }

    /** @brief 两层匹配：第一层优先、第二层的触发条件、空串闸与分开关。 */
    void layered_cases(R& r)
    {
        NameTable table;
        for (const std::string_view name :
             {"run", "rerun", "run_update", "xrun"})
        {
            table.add(name);
        }
        Probe       probe;
        auto        exact = rx::Pattern::compile("run");
        const Found first =
            sn::match_symbol_names(table.store, *exact, true, probe, 2);
        cg::test::check(
            r,
            scan_is(first, scan_of({"run"}))
                && probe.calls == PROBES_PER_PASS_SMALL,
            "layered: a first-layer hit returns at once and the partial "
            "layer never runs");
        cg::test::check(
            r,
            exact->partial_match("rerun") == rx::Outcome::Hit,
            "layered: control, the partial layer alone would have matched "
            "more names");
        auto prefix = rx::Pattern::compile("^run_");
        probe.calls = 0;
        const Found rescued =
            sn::match_symbol_names(table.store, *prefix, true, probe, 2);
        cg::test::check(
            r,
            scan_is(rescued, scan_of({"run_update"}))
                && probe.calls == 2 * PROBES_PER_PASS_SMALL,
            "layered: zero full hits hand over to the partial layer, which "
            "rescans with its own probes");
        auto nullable = rx::Pattern::compile("x*");
        probe.calls = 0;
        const Found gated =
            sn::match_symbol_names(table.store, *nullable, true, probe, 2);
        cg::test::check(
            r,
            scan_is(gated, scan_of({})) && probe.calls == PROBES_PER_PASS_SMALL,
            "layered: a pattern that matches the empty string ends after "
            "one scan");
        probe.calls = 0;
        const Found off =
            sn::match_symbol_names(table.store, *prefix, false, probe, 2);
        cg::test::check(
            r,
            scan_is(off, scan_of({})) && probe.calls == PROBES_PER_PASS_SMALL,
            "layered: the switch off keeps the single-layer behaviour");
        auto absent = rx::Pattern::compile("^zz");
        probe.calls = 0;
        const Found silent =
            sn::match_symbol_names(table.store, *absent, true, probe, 2);
        cg::test::check(
            r,
            scan_is(silent, scan_of({}))
                && probe.calls == 2 * PROBES_PER_PASS_SMALL,
            "layered: zero hits in both layers is an empty result after two "
            "scans");
    }

    /** @brief 步数触限的名字：两种语义下各数一次，按不命中处理；两层
     *          编排交出的是产出最终命中名那一遍的数目。 */
    void undetermined_cases(R& r)
    {
        auto wide = rx::Pattern::compile("(.+)(.+)zzqq");
        cg::test::check(r, wide.has_value(), "undetermined: pattern compiles");
        // 四种结局在同一张表上并存：命中一个、不命中一个、触限两个。
        NameTable table;
        table.add(HIT_NAME);
        table.add(MISS_NAME);
        table.add(CUT_BOTH);
        table.add(CUT_FULL_ONLY);
        const Found full = sn::scan_symbol_names(
            table.store,
            *wide,
            sn::MatchMode::Full,
            Probe{},
            1000);
        cg::test::check(
            r,
            scan_is(full, scan_of({HIT_NAME}, 2)),
            "undetermined: full matching counts a cut-off name once per "
            "name and keeps it out of the hits");
        const Found part = sn::scan_symbol_names(
            table.store,
            *wide,
            sn::MatchMode::Partial,
            Probe{},
            1000);
        cg::test::check(
            r,
            scan_is(part, scan_of({HIT_NAME, CUT_FULL_ONLY}, 1)),
            "undetermined: partial matching counts only the names that "
            "still hit the step limit");
        // 第一层有命中即返回：未判定数是第一层的数目，第二层不跑。
        Probe       probe;
        const Found first =
            sn::match_symbol_names(table.store, *wide, true, probe, 1000);
        cg::test::check(
            r,
            scan_is(first, scan_of({HIT_NAME}, 2))
                && probe.calls == PROBES_PER_PASS_ONE_SPAN,
            "undetermined: a first-layer hit carries the first layer's "
            "count");
        // 去掉第一层命中的名字：第一层零命中后交给第二层，交出的是第
        // 二层的数目（第一层触限的两个名字在第二层里有一个已经命中）。
        NameTable second_table;
        second_table.add(MISS_NAME);
        second_table.add(CUT_BOTH);
        second_table.add(CUT_FULL_ONLY);
        probe.calls = 0;
        const Found rescued = sn::match_symbol_names(
            second_table.store,
            *wide,
            true,
            probe,
            1000);
        cg::test::check(
            r,
            scan_is(rescued, scan_of({CUT_FULL_ONLY}, 1))
                && probe.calls == 2 * PROBES_PER_PASS_ONE_SPAN,
            "undetermined: the two-layer match reports the count of the "
            "layer that produced the hits");
    }

    /** @brief 截止探针：段间与段内两个探针点在两遍里都干净放弃，探针
     *          的调用序号钉住扫描停在哪一处。 */
    void deadline_cases(R& r)
    {
        NameTable small;
        for (const std::string_view name :
             {"run", "rerun", "run_update", "xrun"})
        {
            small.add(name);
        }
        auto prefix = rx::Pattern::compile("^run_");
        cg::test::check(r, prefix.has_value(), "deadline: pattern compiles");
        // 第一层：第一个段前探针。
        Probe       probe{.trip_at = 1};
        const Found at_entry =
            sn::match_symbol_names(small.store, *prefix, true, probe, 2);
        cg::test::check(
            r,
            !at_entry.has_value() && probe.calls == 1,
            "deadline: the probe before the first segment abandons");
        // 第一层：第二段之前（已扫完第一段的两行）。
        probe = Probe{.trip_at = 2};
        const Found between =
            sn::match_symbol_names(small.store, *prefix, true, probe, 2);
        cg::test::check(
            r,
            !between.has_value() && probe.calls == 2,
            "deadline: the probe between two segments abandons before the "
            "next segment is fetched");
        // 第二层：第一层三探走完，第二层第一个段前探针是第四次调用。
        probe = Probe{.trip_at = 4};
        const Found second_between =
            sn::match_symbol_names(small.store, *prefix, true, probe, 2);
        cg::test::check(
            r,
            !second_between.has_value() && probe.calls == 4,
            "deadline: the partial layer carries the between-segment probe");
        // 大表：一段 600 行，段内探针在 256 与 512 个名字之后各一次；每遍
        // "段前一探、段内两探、末段空探"共四次调用。阈值都取到第二个段
        // 内探针：只去掉段内探针时，同一序号的调用会换成分段处的探针或
        // 第二层的探针，结果与调用数才都对不上。
        NameTable big;
        for (int i = 0; i < 600; ++i)
        {
            big.add("nm" + std::to_string(i));
        }
        auto absent = rx::Pattern::compile("^nm[0-9]x");
        // 第一层的段内探针（分开关关闭，只有第一层）：第二个段内探针是
        // 第三次调用。
        probe = Probe{.trip_at = 3};
        const Found in_loop =
            sn::match_symbol_names(big.store, *absent, false, probe, 1000);
        cg::test::check(
            r,
            !in_loop.has_value() && probe.calls == 3,
            "deadline: the first layer's in-loop probes fire after 256 and "
            "512 names");
        // 第二层的段内探针：第一层四次调用走完，第二层第二个段内探针是
        // 第七次调用。
        probe = Probe{.trip_at = 7};
        const Found second_in_loop =
            sn::match_symbol_names(big.store, *absent, true, probe, 1000);
        cg::test::check(
            r,
            !second_in_loop.has_value() && probe.calls == 7,
            "deadline: the partial layer carries the in-loop probe");
        probe = Probe{};
        const Found complete =
            sn::match_symbol_names(big.store, *absent, true, probe, 1000);
        cg::test::check(
            r,
            scan_is(complete, scan_of({})) && probe.calls == 8,
            "deadline: with no deadline each scan probes exactly four "
            "times");
    }

    const cg::test::Registrar REG_SCAN{"scan.single", &scan_cases};
    const cg::test::Registrar REG_LAYERED{"scan.layered", &layered_cases};
    const cg::test::Registrar REG_UNDETERMINED{
        "scan.undetermined",
        &undetermined_cases};
    const cg::test::Registrar REG_DEADLINE{"scan.deadline", &deadline_cases};

} // namespace

// 用 module-unit main 是有意为之：项目每个源文件都是具名模块单元；普通
// TU 的 main 会破坏这项全模块政策。
// NOLINTNEXTLINE(clang-diagnostic-main-attached-to-named-module)
export int main() { return cg::test::run_all(); }
