/**
 * @module cg.test.lookup
 *
 * @brief 符号名形态判定与按名装载测试：限定链的分隔位置（模板实参里的
 *        双冒号不是分隔符）、名字能否原样重查的形态判定，以及模板实例
 *        名按名装载时整名就是裸名。
 *
 * 形态判定是纯字符串函数，不碰库；装载用内存库里现场写入的符号。
 */

export module cg.test.lookup;

import std;
import cg.db;
import cg.cli;
import cg.cli.lookup.name;
import cg.test.harness;

namespace
{

    using R = cg::test::Result;
    namespace hk = cg::cli::detail;

    constexpr std::size_t NPOS = std::string_view::npos;
    constexpr std::size_t PATTERN_MAX = 64;

    /** @brief 内存库夹具：一个文件里的若干符号，父链各不相同。 */
    struct SymbolTable
    {
        cg::db::GraphStore store{":memory:"};
        cg::db::FileId     file = 0;
        std::size_t        rows = 0;

        SymbolTable() { file = store.upsert_file("e:/src/lookup.cpp"); }

        /** @brief 写入一个符号：种类、父链与名字进唯一键。 */
        auto add(
            cg::db::SymbolKind kind,
            std::string_view   parent,
            std::string_view   name) -> void
        {
            const std::string key = std::format(
                "{}|{}|{}|e:/src/lookup.cpp#{}",
                cg::db::name_of(kind),
                parent,
                name,
                rows);
            store.upsert_symbol(
                {.unique_key = key,
                 .kind = kind,
                 .name = name,
                 .ns = "",
                 .sig_hash = std::nullopt,
                 .file_id = file,
                 .line = 1,
                 .col = 5});
            ++rows;
        }
    };

    /** @brief 装载结果里各条目的名字，保持装载序。 */
    [[nodiscard]]
    auto loaded_names(const std::vector<hk::PreToolUseBrief>& briefs)
        -> std::vector<std::string>
    {
        std::vector<std::string> out;
        out.reserve(briefs.size());
        for (const hk::PreToolUseBrief& brief : briefs)
        {
            out.push_back(brief.name);
        }
        return out;
    }

    /** @brief 限定链分隔位置：尖括号之外最后一个双冒号。 */
    void separator_cases(R& r)
    {
        cg::test::check(
            r,
            hk::chain_separator_of("") == NPOS
                && hk::chain_separator_of("run") == NPOS,
            "separator: a bare name has no separator");
        cg::test::check(
            r,
            hk::chain_separator_of("Alpha::lookup") == 5
                && hk::chain_separator_of("a::b::c") == 4,
            "separator: the last double colon of a plain chain");
        cg::test::check(
            r,
            hk::chain_separator_of("nodes_of<sqlpp::value_t<T>>") == NPOS
                && hk::chain_separator_of("Box<delta::Item>") == NPOS,
            "separator: a double colon inside template arguments is not a "
            "separator");
        cg::test::check(
            r,
            hk::chain_separator_of("Box<a::b>::c") == 9
                && hk::chain_separator_of("x<y>::z") == 4,
            "separator: a double colon after closed template arguments is "
            "the separator");
        cg::test::check(
            r,
            hk::chain_separator_of("a>::b") == 2
                && hk::chain_separator_of("operator<<a::b") == NPOS,
            "separator: a stray closing bracket does not go negative and "
            "unclosed brackets keep the depth");
    }

    /** @brief 名字原样当查找模式的形态判定。 */
    void requeryable_cases(R& r)
    {
        const std::string long_identifier(100, 'L');
        const std::string long_template = std::string(59, 'B') + "<a::b>";
        cg::test::check(
            r,
            hk::requeryable_name_shape("note_create", PATTERN_MAX)
                && hk::requeryable_name_shape(long_identifier, PATTERN_MAX),
            "re-queryable: a bare identifier qualifies at any length");
        cg::test::check(
            r,
            hk::requeryable_name_shape("Box<delta::Item>", PATTERN_MAX)
                && hk::requeryable_name_shape("~value_t", PATTERN_MAX)
                && hk::requeryable_name_shape("<lambda_1>", PATTERN_MAX)
                && hk::requeryable_name_shape("a.b", PATTERN_MAX)
                && hk::requeryable_name_shape("operator bool", PATTERN_MAX)
                && hk::requeryable_name_shape("operator<<", PATTERN_MAX)
                && hk::requeryable_name_shape("变量甲", PATTERN_MAX),
            "re-queryable: literal characters only, template arguments "
            "included");
        cg::test::check(
            r,
            !hk::requeryable_name_shape("operator()", PATTERN_MAX)
                && !hk::requeryable_name_shape("operator[]", PATTERN_MAX)
                && !hk::requeryable_name_shape("operator|", PATTERN_MAX)
                && !hk::requeryable_name_shape(
                    "(anonymous struct at a.cpp:1:1)",
                    PATTERN_MAX)
                && !hk::requeryable_name_shape("a|b", PATTERN_MAX)
                && !hk::requeryable_name_shape("x[y]", PATTERN_MAX)
                && !hk::requeryable_name_shape("a\\d", PATTERN_MAX)
                && !hk::requeryable_name_shape("a\nb", PATTERN_MAX),
            "re-queryable: regex metacharacters and control characters "
            "disqualify");
        cg::test::check(
            r,
            !hk::requeryable_name_shape(long_template, PATTERN_MAX)
                && long_template.size() == 65,
            "re-queryable: past the pattern length limit only a bare "
            "identifier qualifies");
        cg::test::check(
            r,
            !hk::requeryable_name_shape("Box<a::b>::c", PATTERN_MAX)
                && !hk::requeryable_name_shape(
                    "operator testing::internal::X",
                    PATTERN_MAX)
                && !hk::requeryable_name_shape("", PATTERN_MAX),
            "re-queryable: a chain separator outside brackets or an empty "
            "name disqualifies");
    }

    /** @brief 按名装载：模板实例名整名是裸名；纯标识符链照旧按父链
     *          过滤。 */
    void load_cases(R& r)
    {
        SymbolTable table;
        table.add(cg::db::SymbolKind::Record, "", "Box<delta::Item>");
        table.add(cg::db::SymbolKind::Function, "Alpha", "lookup");
        table.add(cg::db::SymbolKind::Function, "Beta", "lookup");
        cg::test::check(
            r,
            loaded_names(
                hk::load_briefs_for_ident(table.store, "Box<delta::Item>"))
                == std::vector<std::string>{"Box<delta::Item>"},
            "load: a template instance name loads as a whole");
        cg::test::check(
            r,
            hk::load_briefs_for_ident(table.store, "value_t<T>>").empty()
                && hk::load_briefs_for_ident(
                       table.store,
                       "Box<delta::Item>::missing")
                       .empty(),
            "load: the mis-split bare name and a chain that matches nothing "
            "load no symbol of this name");
        cg::test::check(
            r,
            loaded_names(hk::load_briefs_for_ident(table.store, "lookup"))
                    == std::vector<std::string>{"lookup", "lookup"}
                && loaded_names(
                       hk::load_briefs_for_ident(table.store, "Alpha::lookup"))
                       == std::vector<std::string>{"lookup"},
            "load: a bare name loads every same-name symbol, a qualified "
            "chain narrows to its parent");
    }

    const cg::test::Registrar REG_SEPARATOR{
        "lookup.separator",
        &separator_cases};
    const cg::test::Registrar REG_REQUERYABLE{
        "lookup.requeryable",
        &requeryable_cases};
    const cg::test::Registrar REG_LOAD{"lookup.load", &load_cases};

} // namespace

// 用 module-unit main 是有意为之：项目每个源文件都是具名模块单元；普通
// TU 的 main 会破坏这项全模块政策。
// NOLINTNEXTLINE(clang-diagnostic-main-attached-to-named-module)
export int main() { return cg::test::run_all(); }
