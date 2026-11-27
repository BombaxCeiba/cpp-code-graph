/**
 * @module cg.test.disclose
 *
 * @brief 注入文本的显示截断原语测试：超限分级信封的首行、进信封的
 *        名额、字典序前几名、建议行、未判定名字的披露、最坏字节构造，
 *        预算保险丝，以及接在正文之后的披露行。
 *
 * 被测函数是纯字符串组装：名字池现场构造，不碰库、不读环境变量、
 * 不依赖时钟。
 */

export module cg.test.disclose;

import std;
import cg.util;
import cg.cli;
import cg.cli.render.disclose;
import cg.test.harness;

namespace
{

    using R = cg::test::Result;
    namespace hk = cg::cli::detail;

    /// 注入文本的首行。
    [[nodiscard]]
    auto first_line_of(std::string_view text) -> std::string_view
    {
        return text.substr(0, text.find('\n'));
    }

    /// 注入文本的末行。
    [[nodiscard]]
    auto last_line_of(std::string_view text) -> std::string_view
    {
        return text.substr(text.rfind('\n') + 1);
    }

    /// 文本的 UTF-8 序列是否恰好铺满（截断点落在序列边界上）。
    [[nodiscard]]
    auto utf8_tiles_exactly(std::string_view text) -> bool
    {
        std::size_t i = 0;
        while (i < text.size())
        {
            const std::size_t len = cg::util::utf8_seq_len(text[i]);
            if (i + len > text.size())
            {
                return false;
            }
            i += len;
        }
        return true;
    }

    /// 一组名字的视图（信封只收视图）。
    [[nodiscard]]
    auto views_of(const std::vector<std::string>& names)
        -> std::vector<std::string_view>
    {
        return {names.begin(), names.end()};
    }

    /// 六个刚过线的名字 note_0 到 note_5。
    [[nodiscard]]
    auto six_names() -> std::vector<std::string>
    {
        std::vector<std::string> six;
        six.reserve(6);
        for (int i = 0; i < 6; ++i)
        {
            six.push_back("note_" + std::to_string(i));
        }
        return six;
    }

    /// 渲染分级信封：列名上限与预算取钩子本体所用之值。
    [[nodiscard]]
    auto envelope_of(
        std::string_view                     pattern,
        std::size_t                          total,
        const std::vector<std::string_view>& requeryable,
        std::size_t                          undetermined = 0) -> std::string
    {
        return hk::render_overmatch_envelope(
            pattern,
            total,
            requeryable,
            hk::PTU_LOOKUP_HITS_MAX,
            undetermined,
            hk::PTU_INJECTION_BUDGET);
    }

    // 超限分级信封：命中总数、能原样重查的名字里字典序最小的前几名、
    // 建议行、未判定名字的披露、名额、最坏字节构造。
    void envelope_cases(R& r)
    {
        const std::vector<std::string> six = six_names();
        {
            // 七个名字乱序给出：首行报总数，条目是字典序最小的五个且
            // 升序，第六、七名不出现，建议行复述模式并教原样重查。
            const std::vector<std::string> names{
                "note_golf",
                "note_bravo",
                "note_alpha",
                "note_delta",
                "note_echo",
                "note_foxtrot",
                "note_hotel"};
            const std::string text =
                envelope_of("note_[a-z]+", names.size(), views_of(names));
            const std::string want =
                "[ccg] \"note_[a-z]+\" 匹配 7 个符号名，数量过多不逐条"
                "列出；能原样重查的名字里字典序最小的 5 个：\n"
                "[ccg] - note_alpha\n"
                "[ccg] - note_bravo\n"
                "[ccg] - note_delta\n"
                "[ccg] - note_echo\n"
                "[ccg] - note_foxtrot\n"
                "[ccg] 收窄建议：给 \"note_[a-z]+\" 加一段更长的字面片段"
                "（例如把 \\w+ 写成 run_\\w+）后重查，或从上面挑一个名字"
                "原样重查";
            cg::test::check(
                r,
                text == want,
                "overmatch envelope: total, lexicographic head and advice "
                "line are exact");
        }
        {
            // 刚过线（六名）：前五进信封，第六名不出现。
            const std::string text =
                envelope_of("note_\\d", six.size(), views_of(six));
            cg::test::check(
                r,
                first_line_of(text)
                        == "[ccg] \"note_\\d\" 匹配 6 个符号名，数量过多"
                           "不逐条列出；能原样重查的名字里字典序最小的 "
                           "5 个："
                    && text.find("[ccg] - note_4\n") != std::string::npos
                    && text.find("note_5") == std::string::npos,
                "overmatch envelope: six hits list five and hide the sixth");
        }
        {
            // 几十名与上万名：总数如实披露，进信封的仍是字典序前五。
            std::vector<std::string> fifty;
            fifty.reserve(50);
            for (int i = 0; i < 50; ++i)
            {
                fifty.push_back(std::format("note_{:02}", i));
            }
            std::vector<std::string> huge;
            huge.reserve(13815);
            for (int i = 0; i < 13815; ++i)
            {
                huge.push_back(std::format("note_{:05}", i));
            }
            const std::string mid =
                envelope_of("note_\\d+", fifty.size(), views_of(fifty));
            const std::string all =
                envelope_of("note_\\d+", huge.size(), views_of(huge));
            cg::test::check(
                r,
                first_line_of(mid).find("匹配 50 个符号名") != std::string::npos
                    && mid.find(
                           "[ccg] - note_00\n[ccg] - note_01\n[ccg] - "
                           "note_02\n[ccg] - note_03\n[ccg] - note_04\n")
                           != std::string::npos
                    && mid.find("note_05") == std::string::npos
                    && first_line_of(all).find("匹配 13815 个符号名")
                           != std::string::npos
                    && all.find("[ccg] - note_00000\n") != std::string::npos
                    && all.find("note_00005") == std::string::npos,
                "overmatch envelope: mid and full-table tiers keep the head "
                "at five and report the true total");
        }
        {
            // 名额跟随传入的列名上限。
            const std::string text = hk::render_overmatch_envelope(
                "note_\\d",
                six.size(),
                views_of(six),
                3,
                0,
                hk::PTU_INJECTION_BUDGET);
            cg::test::check(
                r,
                text.find("字典序最小的 3 个：") != std::string::npos
                    && text.find("[ccg] - note_2\n") != std::string::npos
                    && text.find("note_3") == std::string::npos,
                "overmatch envelope: the head size follows the cap");
        }
        {
            // 名额只给能原样重查的名字：命中总数是调用方给的全部命中
            // （含不能重查的那些），条目只从能重查的子集里按字典序取。
            const std::vector<std::string> requeryable{
                "note_delta",
                "note_bravo"};
            const std::string text = envelope_of("e", 4, views_of(requeryable));
            cg::test::check(
                r,
                first_line_of(text)
                        == "[ccg] \"e\" 匹配 4 个符号名，数量过多不逐条列出；"
                           "能原样重查的名字里字典序最小的 2 个："
                    && text.find("[ccg] - note_bravo\n[ccg] - note_delta\n")
                           != std::string::npos,
                "overmatch envelope: the head comes from the re-queryable "
                "names only and the total counts every hit");
        }
        {
            // 一个能原样重查的名字都没有：不列名字，首行如实说明，建议
            // 行去掉原样重查那半句。
            const std::string text = envelope_of("\\(anon", 6, {});
            const std::string want =
                "[ccg] \"\\(anon\" 匹配 6 个符号名，数量过多不逐条列出；"
                "没有能原样重查的名字。\n"
                "[ccg] 收窄建议：给 \"\\(anon\" 加一段更长的字面片段"
                "（例如把 \\w+ 写成 run_\\w+）后重查";
            cg::test::check(
                r,
                text == want,
                "overmatch envelope: without a re-queryable name nothing is "
                "listed and the advice drops that half");
        }
        {
            // 未判定数不为零：首行紧跟总数注明，命中总数因此只是下界；
            // 为零时首行没有这一段。
            const std::string noted =
                envelope_of("note_\\w", 8, views_of(six), 3);
            const std::string plain = envelope_of("note_\\w", 8, views_of(six));
            cg::test::check(
                r,
                first_line_of(noted).starts_with(
                    "[ccg] \"note_\\w\" 匹配 8 个符号名"
                    "（另有 3 个名字因步数上限未判定），数量过多不逐条列出；")
                    && first_line_of(plain).starts_with(
                        "[ccg] \"note_\\w\" 匹配 8 个符号名，数量过多")
                    && plain.find("步数上限") == std::string::npos,
                "overmatch envelope: the first line discloses the "
                "undetermined names right after the total");
        }
        {
            // 最坏字节构造：64 字节模式加六个 128 字节的名字——留在
            // 注入预算内，不触保险丝（没有截断标记），七行齐全。
            std::vector<std::string> names;
            names.reserve(6);
            for (int i = 0; i < 6; ++i)
            {
                names.push_back(
                    std::string(127, 'n') + static_cast<char>('a' + i));
            }
            const std::string pattern = std::string(61, 'p') + "\\w+";
            const std::string text =
                envelope_of(pattern, names.size(), views_of(names));
            cg::test::check(
                r,
                pattern.size() == 64 && names.front().size() == 128
                    && text.size() <= hk::PTU_INJECTION_BUDGET
                    && text.find("…[ccg 截断]") == std::string::npos
                    && std::ranges::count(text, '\n') == 6,
                "overmatch envelope: worst-case bytes stay inside the "
                "budget unfused");
        }
        {
            // 保险丝：预算压到刚过首行前段时在首行内硬截、以截断标记
            // 收尾，截点落在 UTF-8 序列边界上（首行是多字节中文）。
            const std::string text = hk::render_overmatch_envelope(
                "note_\\d",
                six.size(),
                views_of(six),
                hk::PTU_LOOKUP_HITS_MAX,
                0,
                100);
            constexpr std::string_view marker = " …[ccg 截断]";
            cg::test::check(
                r,
                text.size() <= 100 && text.ends_with(marker)
                    && utf8_tiles_exactly(
                        std::string_view{text}.substr(
                            0,
                            text.size() - marker.size())),
                "overmatch envelope: the budget fuse caps the text on a "
                "code point boundary");
        }
        {
            // 模式显示截断：超长模式在首行与建议行里都以 61 字节加省
            // 略号示人，原文整串不出现。
            const std::string long_pattern(100, 'y');
            const std::string text =
                envelope_of(long_pattern, six.size(), views_of(six));
            const std::string shown = "\"" + std::string(61, 'y') + "…\"";
            cg::test::check(
                r,
                first_line_of(text).starts_with("[ccg] " + shown)
                    && last_line_of(text).find(shown) != std::string::npos
                    && text.find(std::string(100, 'y')) == std::string::npos,
                "overmatch envelope: over-long pattern is clipped in the "
                "first and the advice line");
        }
        {
            // 保险丝本体：预算内原样返回；超预算硬截、加标记、不超预算。
            const std::string fits(hk::PTU_INJECTION_BUDGET, 'a');
            const std::string over(hk::PTU_INJECTION_BUDGET + 100, 'a');
            const std::string fused =
                hk::fuse_to_budget(over, hk::PTU_INJECTION_BUDGET);
            cg::test::check(
                r,
                hk::fuse_to_budget(fits, hk::PTU_INJECTION_BUDGET) == fits
                    && fused.size() <= hk::PTU_INJECTION_BUDGET
                    && fused.ends_with(" …[ccg 截断]"),
                "budget fuse: passthrough under budget, capped with the "
                "marker over it");
        }
    }

    // 步数上限披露行：零时为空串，不为零时是一整行；接在正文之后时
    // 整体不超预算，披露行本身不被截掉。
    void note_cases(R& r)
    {
        cg::test::check(
            r,
            hk::render_undetermined_note(0).empty()
                && hk::render_undetermined_note(1)
                       == "[ccg] 另有 1 个名字因步数上限未判定，没有计入"
                && hk::render_undetermined_note(1234)
                       == "[ccg] 另有 1234 个名字因步数上限未判定，没有计入",
            "undetermined note: empty for zero, one exact line otherwise");
        const std::string     note = hk::render_undetermined_note(7);
        const std::string     body = "[ccg] body line\n[ccg] another line";
        constexpr std::size_t budget = 1400;
        const std::string     fitting(1300, 'a');
        const std::string     appended =
            hk::append_note_within_budget(fitting, note, budget);
        const std::string fused =
            hk::append_note_within_budget(std::string(1390, 'a'), note, budget);
        cg::test::check(
            r,
            hk::append_note_within_budget(body, "", budget) == body,
            "undetermined note: an empty note leaves the text untouched");
        cg::test::check(
            r,
            appended == fitting + "\n" + note,
            "undetermined note: a fitting text keeps its bytes and gets the "
            "note as its last line");
        cg::test::check(
            r,
            fused.size() <= budget && fused.ends_with("\n" + note)
                && fused.find("…[ccg 截断]\n") != std::string::npos,
            "undetermined note: an over-long text is fused to make room and "
            "the note stays whole at the end");
    }

    const cg::test::Registrar REG_ENVELOPE{
        "disclose.envelope",
        &envelope_cases};
    const cg::test::Registrar REG_NOTE{"disclose.note", &note_cases};

} // namespace

// 用 module-unit main 是有意为之：项目每个源文件都是具名模块单元；普通
// TU 的 main 会破坏这项全模块政策。
// NOLINTNEXTLINE(clang-diagnostic-main-attached-to-named-module)
export int main() { return cg::test::run_all(); }
