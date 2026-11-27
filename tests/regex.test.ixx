/**
 * @module cg.test.regex
 *
 * @brief 正则匹配层测试：编译成败与方言边界、全匹配与部分匹配两种
 *        语义、空串试探、与标准库正则的命中集合等价性、步数上限、
 *        移动语义与引擎档位。
 *
 * 名字池现场构造；标准库正则只作等价性的参照，不进生产路径。步数上限
 * 用例为了拿引擎的原始返回码直接调引擎接口，取到的资源由测试内的拥有
 * 型对象持有，交还只出现在它的析构里。
 */

export module cg.test.regex;

import std;
import cg.util.pcre2;
import cg.util.regex;
import cg.test.harness;

namespace
{

    using R = cg::test::Result;
    namespace rx = cg::util::regex;
    namespace pc = cg::util::pcre2;

    /**
     * @brief 测试内直接调引擎接口时，三项引擎资源的拥有者。
     *
     * @internal
     * 步数上限用例要拿引擎的原始返回码，只能绕过封装类直接调接口。取
     * 到的编译好的模式、匹配数据块、匹配上下文逐项交给本类，交还只在析
     * 构里发生，测试代码里没有人手配对的取与放。不拷贝也不移动：每一项
     * 取到就交给它，随所在作用域一并交还。
     */
    class RawResources
    {
    public:
        RawResources() = default;

        RawResources(const RawResources&) = delete;
        auto operator=(const RawResources&) -> RawResources& = delete;
        RawResources(RawResources&&) = delete;
        auto operator=(RawResources&&) -> RawResources& = delete;

        ~RawResources()
        {
            pcre2_match_context_free_8(context);
            pcre2_match_data_free_8(data);
            pcre2_code_free_8(code);
        }

        pcre2_code_8*          code = nullptr;
        pcre2_match_data_8*    data = nullptr;
        pcre2_match_context_8* context = nullptr;
    };

    /** @brief 匹配语义：全名字覆盖，或名字里的任意一段。 */
    enum class Semantics : std::uint8_t
    {
        Full,
        Partial
    };

    /** @brief 现场构造的符号名池：裸名、限定名、模板名、描述型名字与
     *          各类边界形态，外加一批成族生成的名字。 */
    [[nodiscard]]
    auto make_name_pool() -> std::vector<std::string>
    {
        std::vector<std::string> names{
            "run",
            "Run",
            "RUN",
            "rerun",
            "run_update",
            "run_pretooluse_update",
            "xrun_update",
            "run_pretooluse_update_x",
            "indirect_commit",
            "add_types",
            "addXtypes",
            "k_max_types",
            "kFoo",
            "Kfoo",
            "area",
            "pi",
            "error",
            "Error",
            "std",
            "map",
            "shape::Circle::area",
            "shape::Circle::radius",
            "cg::util::join",
            "std::map<int,int>",
            "std::vector<T>",
            "(anonymous union at a.ixx:3:1)",
            "~arithmetic_expression<Lhs, Operator, Rhs>",
            "operator+",
            "a",
            "q",
            "h",
            "e",
            "_",
            "C",
            "M1",
            "A2",
            "R3",
            "radius_",
            "ninja: build stopped",
            "ccg.db",
            "ccg.db-wal",
            "TempProj",
            "struct TempProj",
            "using TempProj",
            "export auto value = [",
            "export inline auto value",
            "export class Foo;",
            "class Foo;",
            "Microsoft.Build.dll",
            "Microsoft.Build.Framework.dll",
            "k_max_rounds =",
            "Microsoft.Build.Utilities.Core.dll"};
        for (int i = 0; i < 120; ++i)
        {
            names.push_back("note_cr" + std::to_string(i));
            names.push_back("get_thing" + std::to_string(i));
            names.push_back("is_" + std::to_string(i));
            names.push_back("Thing" + std::to_string(i));
            names.push_back(
                "ns" + std::to_string(i) + "::fn" + std::to_string(i));
            names.push_back("note_" + std::to_string(i) + "_types");
            names.push_back("on_" + std::to_string(i));
        }
        names.push_back(std::string(114, 'x') + "_types");
        names.push_back(std::string(120, 'x') + "types");
        names.push_back(std::string(100, 'x') + "y");
        return names;
    }

    /** @brief 标准库正则一侧的结果：是否编译通过、匹配是否中途抛出
     *          （标准库实现自带的复杂度上限），以及命中序列。匹配中途
     *          抛出时命中序列只到抛出点为止，是完整序列的前缀。 */
    struct StdHits
    {
        bool                     compiled = false;
        bool                     threw = false;
        std::vector<std::string> hits;
    };

    [[nodiscard]]
    auto std_hits(
        const std::vector<std::string>& names,
        std::string_view                pattern,
        Semantics                       semantics) -> StdHits
    {
        StdHits    out;
        std::regex re;
        try
        {
            re = std::regex{std::string{pattern}, std::regex::ECMAScript};
            out.compiled = true;
        }
        catch (const std::regex_error&)
        {
            return out;
        }
        for (const std::string& name : names)
        {
            try
            {
                const bool hit = semantics == Semantics::Full
                                     ? std::regex_match(name, re)
                                     : std::regex_search(name, re);
                if (hit)
                {
                    out.hits.push_back(name);
                }
            }
            catch (const std::regex_error&)
            {
                out.threw = true;
                break;
            }
        }
        return out;
    }

    /** @brief 匹配层的命中序列与触限的名字数。 */
    struct LayerHits
    {
        std::vector<std::string> hits;
        std::size_t              aborted = 0;
    };

    [[nodiscard]]
    auto layer_hits(
        const std::vector<std::string>& names,
        std::string_view                pattern,
        Semantics                       semantics) -> LayerHits
    {
        LayerHits out;
        auto      engine = rx::Pattern::compile(pattern);
        if (!engine.has_value())
        {
            return out;
        }
        for (const std::string& name : names)
        {
            const rx::Outcome outcome = semantics == Semantics::Full
                                            ? engine->full_match(name)
                                            : engine->partial_match(name);
            if (outcome == rx::Outcome::Hit)
            {
                out.hits.push_back(name);
            }
            else if (outcome == rx::Outcome::Aborted)
            {
                ++out.aborted;
            }
        }
        return out;
    }

    /** @brief 一批窄模式在一个语义下与标准库正则逐条等价：两边都编译
     *          通过、标准库不中途抛出、匹配层零触限、命中序列相同。 */
    void check_narrow_equivalence(
        R&                                r,
        const std::vector<std::string>&   names,
        std::span<const std::string_view> patterns,
        std::string_view                  label,
        Semantics                         semantics)
    {
        for (const std::string_view pattern : patterns)
        {
            const StdHits   want = std_hits(names, pattern, semantics);
            const LayerHits got = layer_hits(names, pattern, semantics);
            cg::test::check(
                r,
                want.compiled && !want.threw && got.aborted == 0
                    && got.hits == want.hits,
                std::format(
                    "equivalence {}: {} -> std {} / layer {} / aborted {}",
                    label,
                    pattern,
                    want.hits.size(),
                    got.hits.size(),
                    got.aborted));
        }
    }

    /** @brief 名字是否全由 ASCII 字母组成（字母类的独立判据）。 */
    [[nodiscard]]
    auto ascii_letters_only(std::string_view name) -> bool
    {
        return !name.empty()
               && std::ranges::all_of(
                   name,
                   [](char c)
                   {
                       return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
                   });
    }

    /** @brief 中间夹零字节的字节序列名字；以空字符结尾的构造读不出全长，
     *          所以按显式长度构造。 */
    const std::string NAME_WITH_NUL{"a\0b", 3};

    /** @brief 中间夹高位字节的字节序列名字。
     *
     * @internal 十六进制转义会贪吃到后续的十六进制数字字符，字面量
     *           必须断开。 */
    const std::string NAME_WITH_HIGH_BYTE{
        "a\xff"
        "b",
        3};

    /** @brief 编译成败：公共语法面与引擎扩展编译通过，非法与方言外形态
     *        返回空。 */
    void compile_cases(R& r)
    {
        constexpr std::string_view accepted[] = {
            "",
            "add_types",
            "[a-z]+_types",
            "run_.*update",
            "(ab)+",
            "a+?",
            "a*+",
            "a{1,3}?",
            "(?i)run",
            "\\p{L}+",
            "\\p{Lu}+",
            "[[:alpha:]]+_types",
            "run(?=_update)",
            "run(?!_update)",
            "(a*)*",
            "(a|aa)+b",
            "(\\w+)+",
            "\\Arun\\z",
            "\\Arun\\Z",
            "(?<name>a)",
            "(?'name'a)",
            "(?#comment)a",
            "(?<=a)b",
            "(?<!a)b",
            "(a)\\1",
            "\\x41",
            "\\x{41}",
            "\\o{101}",
            "\\cA",
            "[\\b]",
            "[]a]",
            "[a-]",
            "\\Qa.b\\E",
            "\\e",
            "\\a",
            "(*FAIL)",
            "ccg.db(-wal)?$",
            "(const )?auto [a-z_]+ = \\["};
        for (const std::string_view pattern : accepted)
        {
            cg::test::check(
                r,
                rx::Pattern::compile(pattern).has_value(),
                std::format("compile accepts {}", pattern));
        }
        constexpr std::string_view rejected[] = {
            "(",          ")",         "[a",        "a{2,1}",
            "*a",         "+a",        "?a",        "a**",
            "a?{2}",      "a\\",       "\\q",       "\\y",
            "\\u0041",    "\\x{FFFF}", "[a-\\d]",   "[\\d-a]",
            "[[:foo:]]a", "\\p{Foo}",  "(?(1)a|b)", "(?<name>a)(?<name>b)"};
        for (const std::string_view pattern : rejected)
        {
            cg::test::check(
                r,
                !rx::Pattern::compile(pattern).has_value(),
                std::format("compile rejects {}", pattern));
        }
    }

    /** @brief 全匹配语义：首尾双锚定、显式长度、空串与任意字节。 */
    void full_match_cases(R& r)
    {
        auto engine = rx::Pattern::compile("run_.*update");
        cg::test::check(r, engine.has_value(), "full match: pattern compiles");
        cg::test::check(
            r,
            engine->full_match("run_update") == rx::Outcome::Hit
                && engine->full_match("run_pretooluse_update")
                       == rx::Outcome::Hit,
            "full match: wildcard covers the middle");
        cg::test::check(
            r,
            engine->full_match("xrun_update") == rx::Outcome::Miss
                && engine->full_match("run_update_x") == rx::Outcome::Miss,
            "full match: both ends are anchored");
        cg::test::check(
            r,
            engine->full_match("run_update\n") == rx::Outcome::Miss,
            "full match: end anchoring is the very end, not before a newline");
        auto dollar = rx::Pattern::compile("run_update$");
        cg::test::check(
            r,
            dollar.has_value()
                && dollar->full_match("run_update") == rx::Outcome::Hit
                && dollar->full_match("run_update\n") == rx::Outcome::Miss,
            "full match: the end anchor option wins over the dollar anchor");
        auto star = rx::Pattern::compile("a*");
        cg::test::check(
            r,
            star.has_value() && star->full_match("") == rx::Outcome::Hit,
            "full match: an empty name matches an empty-able pattern");
        auto any = rx::Pattern::compile("a.b");
        cg::test::check(
            r,
            any.has_value()
                && any->full_match(NAME_WITH_NUL) == rx::Outcome::Hit
                && any->full_match(NAME_WITH_HIGH_BYTE) == rx::Outcome::Hit,
            "full match: names are byte sequences of explicit length");
        auto lit = rx::Pattern::compile("add_types");
        cg::test::check(
            r,
            lit.has_value() && lit->full_match("add_types") == rx::Outcome::Hit
                && lit->full_match("add_types ") == rx::Outcome::Miss
                && lit->full_match(" add_types") == rx::Outcome::Miss,
            "full match: a literal must cover the whole name");
    }

    /** @brief 部分匹配语义：片段落在名字里的任意位置即命中，模式自带
     *          的锚照常生效，名字是显式长度的字节序列。 */
    void partial_match_cases(R& r)
    {
        auto engine = rx::Pattern::compile("run_.*update");
        cg::test::check(
            r,
            engine.has_value()
                && engine->partial_match("run_pretooluse_update")
                       == rx::Outcome::Hit
                && engine->partial_match("xrun_update_y") == rx::Outcome::Hit,
            "partial match: the fragment may sit anywhere in the name");
        cg::test::check(
            r,
            engine->partial_match("run_pretooluse") == rx::Outcome::Miss
                && engine->partial_match("") == rx::Outcome::Miss,
            "partial match: a name without the fragment misses, so does "
            "the empty name");
        auto head = rx::Pattern::compile("^run_");
        cg::test::check(
            r,
            head.has_value()
                && head->partial_match("run_update") == rx::Outcome::Hit
                && head->partial_match("xrun_update") == rx::Outcome::Miss,
            "partial match: the start anchor only matches at the start");
        auto tail = rx::Pattern::compile("update$");
        cg::test::check(
            r,
            tail.has_value()
                && tail->partial_match("run_update") == rx::Outcome::Hit
                && tail->partial_match("run_update_x") == rx::Outcome::Miss,
            "partial match: the end anchor only matches at the end");
        auto dot = rx::Pattern::compile("a.b");
        cg::test::check(
            r,
            dot.has_value()
                && dot->partial_match("x" + NAME_WITH_NUL + "y")
                       == rx::Outcome::Hit
                && dot->partial_match(NAME_WITH_HIGH_BYTE) == rx::Outcome::Hit,
            "partial match: names are byte sequences of explicit length");
    }

    /** @brief 空串试探：能匹配空串的模式判为可空，需要字符的模式判为
     *          不可空，被移动走的对象恒为不可空。 */
    void empty_gate_cases(R& r)
    {
        // 前三条是行过滤器的常见写法（匹配行尾的数字串、行首的可选引
        // 号），其余八条各代表一种可空构造；合计十一条，与端到端闸门
        // 的十一条相同。
        constexpr std::string_view nullable[] = {
            "[0-9.]*$",
            "^[0-9]*",
            "^\"?",
            "\\d*",
            "\\d?",
            "7?",
            "\\s*",
            "\\W*",
            "(zzqq|)?",
            "(foo)?",
            "(?:zzqq)*"};
        std::size_t nullable_count = 0;
        for (const std::string_view pattern : nullable)
        {
            auto       engine = rx::Pattern::compile(pattern);
            const bool ok = engine.has_value() && engine->matches_empty();
            nullable_count += ok ? 1 : 0;
            cg::test::check(
                r,
                ok,
                std::format(
                    "empty gate: {} matches the empty string",
                    pattern));
        }
        cg::test::check(
            r,
            nullable_count == 11,
            "empty gate: the eleven empty-matching patterns all report "
            "matchable");
        constexpr std::string_view not_nullable[] = {
            "a+",
            "foo",
            "[0-9]",
            "(foo)+",
            "(a*)*b",
            "x*?y",
            "\\d",
            "(?i)run",
            "analysis\\.dispatch",
            "5\\.77|584",
            "(a*)*[bc]"};
        for (const std::string_view pattern : not_nullable)
        {
            auto engine = rx::Pattern::compile(pattern);
            cg::test::check(
                r,
                engine.has_value() && !engine->matches_empty(),
                std::format(
                    "empty gate: {} needs at least one character",
                    pattern));
        }
        auto        moved_from = rx::Pattern::compile("x*");
        rx::Pattern moved = std::move(*moved_from);
        cg::test::check(
            r,
            !moved_from->matches_empty() && moved.matches_empty(),
            "empty gate: the moved-from object reports no, the moved-to "
            "object carries the answer");
    }

    /** @brief 与标准库正则的命中集合逐条等价（公共方言面的历史形态），
     *         全匹配与部分匹配两种语义各比一遍。窄模式另断言零触限；
     *         平铺通配允许在不命中的长名上触限。 */
    void equivalence_cases(R& r)
    {
        const std::vector<std::string> names = make_name_pool();
        // 转义反斜杠较多的几条写成原始字符串字面量，其余保持普通字面量。
        constexpr std::string_view narrow[] = {
            "/Yu|strip.*pch|pch.*strip|/Fp",
            R"(k_[a-z]|k_[a-z0-9_]+|(const|constexpr).{0,40}\bk_[a-z])",
            R"(\bk_[a-z][a-z0-9_]*|\bk_[a-z0-9_]+|\bk_[a-z])",
            "TempProj\\s*$|struct TempProj|using TempProj",
            "\\bk[A-Z][A-Za-z0-9_]*",
            "(\\.exe|error|Error|ninja: build stopped)",
            "^[MARC]",
            "indirect.*commit",
            "run_.*update",
            "(add_types)",
            "add.?types",
            "[a-z]+_types",
            "^\\w+$",
            "\\w+",
            "on_\\w+",
            "\\w*ccg\\w*",
            "get\\w+",
            "is_\\w+",
            "[a-z]+",
            "struct Subtree",
            "add_types.*slot",
            "k_max_rounds\\s*=",
            "[0-9.]*$",
            "^[0-9]*",
            "^\"?",
            "analysis\\.dispatch",
            "\\.ixx",
            "5\\.77|584",
            ".ccg",
            "ccg.db(-wal)?$",
            "/LTCG(:INCREMENTAL)?\\b",
            R"(^Microsoft\.Build(\.Framework|\.Utilities\.Core)?\.dll$)",
            "(const )?auto [a-z_]+ = \\[",
            R"(^export (inline )?(auto|\[\[nodiscard\]\].*auto) [a-z_]+)",
            "^(export )?(class|struct) [A-Za-z_]+;",
            "k_max_types|cap.*56|56.*cap",
            "\\.ixx|ifc|module.*interface|import std",
            R"(cl_parse_argv\(\)\s*(const)?\s*\{)",
            "run(?=_update)",
            "run(?!_update)",
            "[[:alpha:]]+_types",
            "\\d{2,4}",
            "a\\.b",
            "(ab)+",
            "a+?",
            "a{1,3}?",
            "[A-Z]\\w+",
            "^(Get|Set)\\w+$",
            "(a*)*",
            "(a|aa)+b",
            "\\w+_\\d+",
            "[ns][0-9]+::fn[0-9]+"};
        check_narrow_equivalence(r, names, narrow, "full", Semantics::Full);
        check_narrow_equivalence(
            r,
            names,
            narrow,
            "partial",
            Semantics::Partial);
        // 平铺通配：命中序列须以标准库的序列为前缀（标准库实现可能先
        // 触到自己的复杂度上限而抛出），不命中的长名允许触限。
        constexpr std::string_view flat[] = {
            ".*.*_types",
            ".*types",
            ".*.*.*types"};
        for (const std::string_view pattern : flat)
        {
            const StdHits   want = std_hits(names, pattern, Semantics::Full);
            const LayerHits got = layer_hits(names, pattern, Semantics::Full);
            cg::test::check(
                r,
                want.compiled && std::ranges::starts_with(got.hits, want.hits),
                std::format(
                    "equivalence (flat wildcards): {} -> std {}{} / layer {} "
                    "/ aborted {}",
                    pattern,
                    want.hits.size(),
                    want.threw ? " (std threw)" : "",
                    got.hits.size(),
                    got.aborted));
        }
    }

    /** @brief 方言边界：引擎扩展形态编译通过且命中符合预期。 */
    void dialect_cases(R& r)
    {
        const std::vector<std::string> names = make_name_pool();
        cg::test::check(
            r,
            !std_hits(names, "(?i)run", Semantics::Full).compiled,
            "dialect: std rejects the inline flag");
        const LayerHits folded = layer_hits(names, "(?i)run", Semantics::Full);
        cg::test::check(
            r,
            folded.aborted == 0
                && folded.hits
                       == std::vector<std::string>({"run", "Run", "RUN"}),
            "dialect: inline caseless flag matches all three spellings");
        cg::test::check(
            r,
            !std_hits(names, "\\p{L}+", Semantics::Full).compiled,
            "dialect: std rejects the letter class");
        std::vector<std::string> letters;
        for (const std::string& name : names)
        {
            if (ascii_letters_only(name))
            {
                letters.push_back(name);
            }
        }
        const LayerHits letter_hits =
            layer_hits(names, "\\p{L}+", Semantics::Full);
        cg::test::check(
            r,
            letter_hits.aborted == 0 && letter_hits.hits == letters
                && letters.size() > 10,
            std::format(
                "dialect: letter class equals the ASCII-letter predicate ({})",
                letters.size()));
        const LayerHits anchors =
            layer_hits(names, "\\Arun\\z", Semantics::Full);
        cg::test::check(
            r,
            anchors.hits == std::vector<std::string>({"run"}),
            "dialect: start and end anchors are accepted");
        const LayerHits possessive =
            layer_hits(names, "run.*+update", Semantics::Full);
        cg::test::check(
            r,
            possessive.hits.empty(),
            "dialect: a possessive wildcard never gives back the tail");
        const LayerHits lazy =
            layer_hits(names, "run_.*?update", Semantics::Full);
        cg::test::check(
            r,
            lazy.hits.size() == 2,
            "dialect: a lazy wildcard still spans the whole name");
    }

    /** @brief 步数上限：病态形态触限按不命中，真命中照常找到。 */
    void limit_cases(R& r)
    {
        const std::string worst = std::string(40, 'a') + "d";
        auto              engine = rx::Pattern::compile("(a+)+[bc]");
        cg::test::check(r, engine.has_value(), "limit: pattern compiles");
        cg::test::check(
            r,
            engine->full_match(worst) == rx::Outcome::Aborted,
            "limit: exponential backtracking is cut off");
        cg::test::check(
            r,
            engine->full_match(std::string(40, 'a') + "c") == rx::Outcome::Hit,
            "limit: a true hit is still found");
        // 部分匹配共用同一个上限：在第一个起点上就指数回溯的名字同样
        // 被截断，起点上直接命中的名字照常命中。
        cg::test::check(
            r,
            engine->partial_match(worst) == rx::Outcome::Aborted
                && engine->partial_match(std::string(40, 'a') + "c")
                       == rx::Outcome::Hit,
            "limit: partial matching is bounded by the same step limit");
        const auto  started = std::chrono::steady_clock::now();
        std::size_t aborted = 0;
        for (int i = 0; i < 200; ++i)
        {
            aborted += engine->full_match(std::string(22, 'a') + "d")
                       == rx::Outcome::Aborted;
        }
        const auto elapsed = std::chrono::steady_clock::now() - started;
        cg::test::check(
            r,
            aborted == 200,
            "limit: every cut-off call reports the aborted outcome");
        cg::test::check(
            r,
            elapsed < std::chrono::seconds{2},
            "limit: cut-off calls stay far below the unlimited cost");
        // 直接走引擎接口：同一名字、同一上限，返回值就是步数触限。三项
        // 资源逐项取到就交给测试内的拥有者。
        int          error_code = 0;
        std::size_t  error_offset = 0;
        RawResources raw;
        raw.code = pcre2_compile_8(
            reinterpret_cast<const std::uint8_t*>("(a+)+[bc]"),
            9,
            0,
            &error_code,
            &error_offset,
            nullptr);
        cg::test::check(r, raw.code != nullptr, "limit: raw compile succeeds");
        raw.data = pcre2_match_data_create_from_pattern_8(raw.code, nullptr);
        raw.context = pcre2_match_context_create_8(nullptr);
        pcre2_set_match_limit_8(raw.context, rx::MATCH_STEP_LIMIT);
        const std::string subject = std::string(22, 'a') + "d";
        const int         rc = pcre2_match_8(
            raw.code,
            reinterpret_cast<const std::uint8_t*>(subject.data()),
            subject.size(),
            0,
            pc::ANCHORED | pc::ENDANCHORED,
            raw.data,
            raw.context);
        cg::test::check(
            r,
            rc == pc::ERROR_MATCHLIMIT,
            std::format("limit: raw call reports the step limit (rc {})", rc));
        auto flat = rx::Pattern::compile(".*.*_types");
        cg::test::check(
            r,
            flat.has_value()
                && flat->full_match(std::string(122, 'x') + "_types")
                       == rx::Outcome::Hit
                && flat->full_match(std::string(122, 'x') + "types")
                       != rx::Outcome::Hit,
            "limit: two flat wildcards land a longest true hit");
        auto grouped = rx::Pattern::compile("(x+x)+y");
        cg::test::check(
            r,
            grouped.has_value()
                && grouped->full_match(std::string(100, 'x') + "y")
                       == rx::Outcome::Hit,
            "limit: a nested quantifier still lands a long true hit");
    }

    /** @brief 移动语义：资源随移动转移，被移动走的对象永不命中。 */
    void move_cases(R& r)
    {
        auto first = rx::Pattern::compile("run_.*update");
        cg::test::check(r, first.has_value(), "move: pattern compiles");
        rx::Pattern moved = std::move(*first);
        cg::test::check(
            r,
            moved.full_match("run_update") == rx::Outcome::Hit,
            "move: the moved-to object carries the engine resources");
        cg::test::check(
            r,
            first->full_match("run_update") == rx::Outcome::Aborted
                && first->partial_match("xrun_update") == rx::Outcome::Aborted,
            "move: the moved-from object reports an error under both "
            "semantics");
        auto second = rx::Pattern::compile("zzz");
        *second = std::move(moved);
        cg::test::check(
            r,
            second->full_match("run_update") == rx::Outcome::Hit
                && second->full_match("zzz") == rx::Outcome::Miss,
            "move: move assignment replaces the old pattern");
        *second = std::move(*second);
        cg::test::check(
            r,
            second->full_match("run_update") == rx::Outcome::Hit,
            "move: self move assignment keeps the pattern");
        std::vector<rx::Pattern> many;
        many.push_back(std::move(*second));
        cg::test::check(
            r,
            many.front().full_match("run_update") == rx::Outcome::Hit,
            "move: patterns travel through containers");
    }

    /** @brief 引擎档位：即时编译没有编入库，匹配走解释器。 */
    void tier_cases(R& r)
    {
        std::uint32_t jit = 1;
        const int     rc = pcre2_config_8(pc::CONFIG_JIT, &jit);
        cg::test::check(
            r,
            rc == 0 && jit == 0,
            std::format(
                "tier: just-in-time compilation is not built ({})",
                jit));
        cg::test::check(
            r,
            rx::MATCH_STEP_LIMIT > 0 && rx::MATCH_STEP_LIMIT < 10'000'000,
            "tier: the step limit is set explicitly below the engine default");
    }

    /** @brief 引擎的三个释放函数对空指针不做任何事：封装类的释放函数
     *          不判空，编译失败与被移走的对象都靠这一点。 */
    void null_release_cases(R& r)
    {
        {
            // 三项都没有取到的拥有者：析构对三个释放函数各传一次空指针。
            const RawResources empty;
        }
        cg::test::check(
            r,
            true,
            "null: releasing never-acquired engine resources is harmless");
    }

    const cg::test::Registrar REG_COMPILE{"regex.compile", &compile_cases};
    const cg::test::Registrar REG_FULL_MATCH{
        "regex.full_match",
        &full_match_cases};
    const cg::test::Registrar REG_PARTIAL_MATCH{
        "regex.partial_match",
        &partial_match_cases};
    const cg::test::Registrar REG_EMPTY_GATE{
        "regex.empty_gate",
        &empty_gate_cases};
    const cg::test::Registrar REG_EQUIVALENCE{
        "regex.equivalence",
        &equivalence_cases};
    const cg::test::Registrar REG_DIALECT{"regex.dialect", &dialect_cases};
    const cg::test::Registrar REG_LIMIT{"regex.limit", &limit_cases};
    const cg::test::Registrar REG_MOVE{"regex.move", &move_cases};
    const cg::test::Registrar REG_TIER{"regex.tier", &tier_cases};
    const cg::test::Registrar REG_NULL_RELEASE{
        "regex.null_release",
        &null_release_cases};

} // namespace

// 用 module-unit main 是有意为之：项目每个源文件都是具名模块单元；普通
// TU 的 main 会破坏这项全模块政策。
// NOLINTNEXTLINE(clang-diagnostic-main-attached-to-named-module)
export int main() { return cg::test::run_all(); }
