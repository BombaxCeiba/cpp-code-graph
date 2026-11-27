/**
 * @module cg.cli.scan.names
 *
 * @brief 钩子受限正则路径的符号名表扫描：按段物化符号名、逐名匹配，
 *        先按全匹配扫一遍，零命中再按部分匹配重扫一遍；段间与段内
 *        都带截止探针，触线即整体放弃。
 *
 * 只碰库里的符号名表与匹配层：截止判定由调用方以可调用对象传入，本
 * 单元不读环境变量、不读时钟，也不决定命中之后的出口；钩子本体只留
 * 调用点。
 */
export module cg.cli.scan.names;

import std;
import cg.util.regex;
import cg.db.schema;
import cg.db.store;
import cg.db.query.dumpkeys;

export namespace cg::cli::detail
{

    /** @brief 受限正则路径扫描时跳过的名字字节长度上限（超长模板
     *         实例名跳过：匹配步数随名字长度增长，长名只会白耗步数
     *         上限）。 */
    inline constexpr std::size_t PTU_REGEX_NAME_MAX = 128;

    /** @brief 受限正则路径全表物化的每段行数（段间查截止探针，触线
     *         即放弃后续段——每段查询独立跑完，结果循环不中途弃置）。 */
    inline constexpr std::size_t PTU_REGEX_SPAN_ROWS = 20000;

    /** @brief 段内活探针的节拍：扫描循环每处理多少个名字查一次截止。 */
    inline constexpr std::size_t PTU_REGEX_PROBE_EVERY = 256;

    /** @brief 一遍扫描采用的匹配语义。 */
    enum class MatchMode : std::uint8_t
    {
        Full,    ///< 全匹配：名字从起点到末尾被模式整体覆盖。
        Partial, ///< 部分匹配：模式覆盖名字中的任意一段。
    };

    /**
     * @brief 扫描结果：命中名，以及因步数上限未判定的名字数。
     *
     * @internal 触限的名字按不命中处理，真实结局未知——其中可能有真
     *           命中。命中数因此只是下界；未判定数不为零时，出口必须
     *           如实披露。
     */
    struct NameScan
    {
        std::vector<std::string> matched;          ///< 升序去重的命中名
        std::size_t              undetermined = 0; ///< 步数触限的名字数
    };

    /**
     * @brief 符号名表的一遍分段扫描：逐段物化、逐名匹配、归并相邻
     *        重复，收集命中名并累计步数触限的名字数。
     *
     * @internal 段间探针先于每一段的物化查询，段内每 PTU_REGEX_PROBE_EVERY
     *           个名字再探一次；任一探针触发即返回空值，后续段不再物
     *           化。只返回命中名，不驻留全量名字——重扫时重新物化。命
     *           中名按字符串表行号升序排列，同名符号共享一行、按行号
     *           归并去重。超过名字长度上限的名字直接跳过。库查询与内
     *           存分配抛出的异常原样上抛，由调用方兜底。
     *
     * @param store            已打开的库。
     * @param pattern          编译好的模式。
     * @param mode             全匹配或部分匹配。
     * @param deadline_reached 截止判定，返回真即放弃。
     * @param span_rows        每段物化的行数。
     * @return 命中名与触限名字数，零命中时命中名为空；空值表示探针
     *         触发。
     */
    template <typename Deadline>
        requires std::invocable<Deadline&>
    [[nodiscard]]
    auto scan_symbol_names(
        cg::db::GraphStore&       store,
        cg::util::regex::Pattern& pattern,
        MatchMode                 mode,
        Deadline&&                deadline_reached,
        std::size_t span_rows = PTU_REGEX_SPAN_ROWS) -> std::optional<NameScan>
    {
        NameScan      scan;
        cg::db::StrId cursor = 0;
        cg::db::StrId prev_id = -1;
        std::size_t   probed = 0;
        while (true)
        {
            // 段间截止探针：先于每一段的物化查询，触发即放弃整个查询
            // （宁漏勿拖）。
            if (deadline_reached())
            {
                return std::nullopt;
            }
            const std::vector<std::pair<cg::db::StrId, std::string>> span =
                cg::db::symbol_names_span(store, cursor, span_rows);
            if (span.empty())
            {
                break;
            }
            for (const auto& [id, name] : span)
            {
                cursor = id;
                if (id == prev_id)
                {
                    continue; // 同名符号共享字符串表的一行：相邻重复
                }
                prev_id = id;
                if (name.size() <= PTU_REGEX_NAME_MAX)
                {
                    const cg::util::regex::Outcome outcome =
                        mode == MatchMode::Full ? pattern.full_match(name)
                                                : pattern.partial_match(name);
                    if (outcome == cg::util::regex::Outcome::Hit)
                    {
                        scan.matched.push_back(name);
                    }
                    else if (outcome == cg::util::regex::Outcome::Aborted)
                    {
                        ++scan.undetermined;
                    }
                }
                // 段内活探针（按处理的名字数计）：慢而不抛的扫描也要
                // 在探针余量内被放弃。
                if (++probed >= PTU_REGEX_PROBE_EVERY)
                {
                    probed = 0;
                    if (deadline_reached())
                    {
                        return std::nullopt;
                    }
                }
            }
        }
        return scan;
    }

    /**
     * @brief 两层匹配：先按全匹配扫一遍；零命中、且模式不能匹配空串
     *        时，再按部分匹配重扫一遍。
     *
     * @internal 第一层是现行语义（模式须覆盖整个名字），有命中即返回，
     *           第二层不付任何代价。第二层有两道前置：调用方传入的分
     *           开关，以及空串试探闸——能匹配空串的模式对任何名字都成
     *           立，名字维度的命中没有信息，整体按零命中处理。两遍各
     *           自重新物化，段间与段内探针两遍都带；任一遍触线即返回
     *           空值。返回的触限名字数属于产出最终命中名的那一遍：另
     *           一遍触限的名字已经在这一遍里重新判定过。
     *
     * @param store            已打开的库。
     * @param pattern          编译好的模式。
     * @param partial_enabled  第二层的开关：为假时只做第一层。
     * @param deadline_reached 截止判定，返回真即放弃。
     * @param span_rows        每段物化的行数。
     * @return 命中名与触限名字数，两层都零命中（或被空串闸拦下）时
     *         命中名为空；空值表示探针触发。
     */
    template <typename Deadline>
        requires std::invocable<Deadline&>
    [[nodiscard]]
    auto match_symbol_names(
        cg::db::GraphStore&       store,
        cg::util::regex::Pattern& pattern,
        bool                      partial_enabled,
        Deadline&&                deadline_reached,
        std::size_t span_rows = PTU_REGEX_SPAN_ROWS) -> std::optional<NameScan>
    {
        std::optional<NameScan> matched = scan_symbol_names(
            store,
            pattern,
            MatchMode::Full,
            deadline_reached,
            span_rows);
        if (!matched.has_value() || !matched->matched.empty())
        {
            return matched;
        }
        if (!partial_enabled || pattern.matches_empty())
        {
            return matched;
        }
        return scan_symbol_names(
            store,
            pattern,
            MatchMode::Partial,
            deadline_reached,
            span_rows);
    }

} // namespace cg::cli::detail
