/**
 * @module cg.cli.render.disclose
 *
 * @brief 工具调用前钩子注入文本的显示截断原语、预算保险丝、超限分级
 *        信封与步数上限披露行。
 *
 * 纯字符串组装：不碰库、不碰截止探针，预算硬顶由参数传入；钩子本体
 * 只留调用点。
 */
export module cg.cli.render.disclose;

import std;
import cg.util;

export namespace cg::cli::detail
{
    /**
     * @brief 字节上限的 UTF-8 序列安全前缀（不切断多字节序列中段）。
     *
     * @internal 与 utf8_prefix（码点数口径）互补：按字节预算判定的截
     *           断（注入硬顶与微小上限分支）必须把截点落在序列边界
     *           上，尾部残缺序列停在其起点；非法引导字节按 1 字节消
     *           费（utf8_seq_len 口径）。
     */
    [[nodiscard]]
    inline auto utf8_trunc_bytes(std::string_view text, std::size_t max_bytes)
        -> std::string_view
    {
        if (text.size() <= max_bytes)
        {
            return text;
        }
        std::size_t keep = 0;
        std::size_t i = 0;
        while (i < text.size())
        {
            const std::size_t len = cg::util::utf8_seq_len(text[i]);
            if (i + len > text.size() || i + len > max_bytes)
            {
                break;
            }
            i += len;
            keep = i;
        }
        return text.substr(0, keep);
    }

    /**
     * @brief 显示截断：超预算时截到上限内（UTF-8 安全）并加省略号后缀。
     *
     * @internal cap 按"截后内容 + 3 字节省略号 ≤ cap"执行（U+2026 为
     *           3 字节 UTF-8 序列），保证显示串自身恒有界。
     */
    [[nodiscard]]
    auto display_clip(std::string_view text, std::size_t cap) -> std::string
    {
        if (text.size() <= cap)
        {
            return std::string{text};
        }
        return std::string{utf8_trunc_bytes(text, cap - 3)} + "…";
    }

    /**
     * @brief 预算保险丝：文本超过预算时按码点边界做字节硬截并加截断
     *        标记；预算不小于 20 字节时返回值恒不超过预算。
     *
     * @internal 标记" …[ccg 截断]"占 16 字节，截断点因此比预算提前
     *           20 字节。
     */
    [[nodiscard]]
    inline auto fuse_to_budget(std::string text, std::size_t budget)
        -> std::string
    {
        if (text.size() <= budget)
        {
            return text;
        }
        constexpr std::size_t tail_room = 20;
        const std::size_t     cap = budget > tail_room ? budget - tail_room : 0;
        return std::string{utf8_trunc_bytes(text, cap)} + " …[ccg 截断]";
    }

    /**
     * @brief 步数上限披露行：另有若干名字因引擎步数上限未判定。
     *
     * @internal 只在未判定数不为零时有内容；零时返回空串，调用方不必
     *           再分支。
     *
     * @param undetermined 步数触限、按不命中处理的名字数。
     * @return 披露行；未判定数为零时为空串。
     */
    [[nodiscard]]
    inline auto render_undetermined_note(std::size_t undetermined)
        -> std::string
    {
        if (undetermined == 0)
        {
            return {};
        }
        return std::format(
            "[ccg] 另有 {} 个名字因步数上限未判定，没有计入",
            undetermined);
    }

    /**
     * @brief 把披露行接在注入文本之后，整体不超过预算。
     *
     * @internal 披露行为空时原样返回。接上之后超预算的，先把正文截到
     *           给披露行留出的余量之内（带截断标记），披露行本身完整
     *           留在末尾——它披露的是"命中不全"，不能被截掉。
     *
     * @param text   注入正文。
     * @param note   披露行；空串表示没有要披露的内容。
     * @param budget 注入文本的字节硬顶，须容得下披露行与截断标记。
     * @return 不超过预算的注入文本。
     */
    [[nodiscard]]
    auto append_note_within_budget(
        std::string      text,
        std::string_view note,
        std::size_t      budget) -> std::string
    {
        if (note.empty())
        {
            return text;
        }
        const std::size_t room =
            budget > note.size() + 1 ? budget - note.size() - 1 : 0;
        return fuse_to_budget(std::move(text), room) + "\n" + std::string{note};
    }

    /**
     * @brief 超限分级信封：命中名超过列名上限时不逐条列出，报命中
     *        总数、字典序最小的前几个能原样重查的名字与一行收窄建议。
     *
     * @internal 进信封的名额只从能原样重查的命中名里按字典序选取，不
     *           按扫描序：扫描序就是字符串表的插入序，取决于建库时的
     *           写入顺序；字典序只依赖命中名集合本身，同一查询在任何
     *           库副本上得到同一份信封。名额不让给其余名字，是因为字
     *           节序里左括号排在字母前，带内嵌路径的描述型长名会把名
     *           额占满，而它们原样重查会被长度护栏拒掉或装载切错——
     *           "挑一个名字原样重查"那句建议就落空。能原样重查的名字
     *           一个都没有时不列名字，建议行去掉原样重查那半句。未判
     *           定数不为零时首行如实注明，命中总数因此只是下界。最坏
     *           构造（列名上限个 128 字节的名字）在注入预算内，不触保
     *           险丝；更长的输入由保险丝兜底。
     *
     * @param pattern      模式原文（首行与建议行内做 64 字节显示截断）。
     * @param total        命中名总数。
     * @param requeryable  命中名里能原样重查的那些。
     * @param shown_max    列名上限，同时是进信封的名额。
     * @param undetermined 因步数上限未判定的名字数。
     * @param budget       注入文本的字节硬顶。
     */
    [[nodiscard]]
    auto render_overmatch_envelope(
        std::string_view                  pattern,
        std::size_t                       total,
        std::span<const std::string_view> requeryable,
        std::size_t                       shown_max,
        std::size_t                       undetermined,
        std::size_t                       budget) -> std::string
    {
        // 只把字典序最小的前 count 个排到队首，其余名字不参与排序
        // （命中可多达上万名）。
        std::vector<std::string_view> ordered{
            requeryable.begin(),
            requeryable.end()};
        const std::size_t count = std::min(shown_max, ordered.size());
        std::ranges::partial_sort(
            ordered,
            ordered.begin() + static_cast<std::ptrdiff_t>(count));
        const std::string pattern_disp = display_clip(pattern, 64);
        std::string       head =
            std::format("[ccg] \"{}\" 匹配 {} 个符号名", pattern_disp, total);
        if (undetermined > 0)
        {
            head +=
                std::format("（另有 {} 个名字因步数上限未判定）", undetermined);
        }
        head += "，数量过多不逐条列出；";
        head +=
            count > 0
                ? std::format("能原样重查的名字里字典序最小的 {} 个：", count)
                : "没有能原样重查的名字。";
        std::string advice = std::format(
            "[ccg] 收窄建议：给 \"{}\" 加一段更长的字面片段（例如把 "
            "\\w+ 写成 run_\\w+）后重查",
            pattern_disp);
        if (count > 0)
        {
            advice += "，或从上面挑一个名字原样重查";
        }
        std::vector<std::string> lines;
        lines.reserve(count + 2);
        lines.push_back(std::move(head));
        for (std::size_t i = 0; i < count; ++i)
        {
            lines.push_back(std::format("[ccg] - {}", ordered[i]));
        }
        lines.push_back(std::move(advice));
        return fuse_to_budget(cg::util::join(lines, "\n"), budget);
    }
} // namespace cg::cli::detail
