/**
 * @module cg.util.regex
 *
 * @brief 钩子查找路径的正则匹配层：一个编译好的模式对象加三个查询
 *        （全名字匹配、部分匹配、能否匹配空串）。匹配步数上限在此显式
 *        设定并随每次匹配调用下发；本单元不碰数据库、不碰截止探针。
 *
 * @internal
 * 全匹配语义靠匹配调用的两个选项位实现（从名字起点开始、必须到名字
 * 末尾），部分匹配不带选项位，都不改写模式本体。引擎的解释器档是回溯
 * 引擎：带嵌套量词的模式在不命中的长名字上会指数回溯，步数上限把单个
 * 名字的最坏耗时钉住；触限的名字按不命中处理（宁漏勿拖）。
 */
export module cg.util.regex;

import std;
import cg.util.pcre2;

export namespace cg::util::regex
{

    /**
     * @brief 单次匹配调用的回溯位置数上限。
     *
     * @internal
     * 取值依据（名字不超过 128 字节、模式不超过 64 字节的口径）：
     * 常见窄模式（字面、字符类、交替、单层通配）在真库名字上用到的
     * 不足一百步。病态形态单名最坏约十到二十微秒。触限的名字各用掉
     * 约上限步，整表耗时随上限近似线性上涨：上限放大十六倍，所测两
     * 条宽模式的整表耗时涨到六到十三倍。
     * 全匹配锚定在名字起点，所测形态的真命中在一千步内全部找到。部
     * 分匹配不同：嵌套量词、多捕获加反向引用的模式，真命中需要的步
     * 数没有上界。一万五千名与三万九千名的两张真库名字表上，上限一
     * 千丢掉两成到五成的真命中；多捕获加反向引用的模式上限提到八到
     * 十六倍才找回全部，嵌套量词的模式提到六十四倍仍有真命中找不回。
     * 所以上限不因丢命中而上调：一千步内判不完的名字按不命中处理，
     * 数目交给上层在出口如实披露。
     * 部分匹配沿用同一个值。引擎对未锚定的模式按名字的每个起点各自
     * 重新计数：某个起点一触限，整个名字就按触限结束，用掉的步数不
     * 超过上限；更慢的是每个起点都不触限却各用很多步的形态，最坏是
     * "名字长度乘上限"。
     */
    inline constexpr std::uint32_t MATCH_STEP_LIMIT = 1'000;

    /** @brief 一次匹配调用的结局。 */
    enum class Outcome : std::uint8_t
    {
        Hit,     ///< 模式命中：全匹配时覆盖整个名字，部分匹配时覆盖其中一段。
        Miss,    ///< 模式不命中名字。
        Aborted, ///< 步数触限、内存不足等错误；调用方按不命中处理。
    };

    /**
     * @brief 编译好的正则模式，拥有模式、匹配数据块与匹配上下文。
     *
     * @internal
     * 匹配数据块按模式定型一次、逐名复用；步数上限写在匹配上下文里，
     * 每次匹配调用都带上。只能移动，不能拷贝。三个查询共用同一个引擎
     * 调用点，结局折算只有一处。
     */
    class Pattern
    {
    public:
        /**
         * @brief 编译模式；方言外的形态返回空，不抛异常。
         *
         * @param pattern 模式原文，按字节解释（不开 UTF 与统一字符属性）。
         */
        [[nodiscard]]
        static auto compile(std::string_view pattern) noexcept
            -> std::optional<Pattern>;

        Pattern(const Pattern&) = delete;
        auto operator=(const Pattern&) -> Pattern& = delete;
        Pattern(Pattern&& other) noexcept;
        auto operator=(Pattern&& other) noexcept -> Pattern&;
        ~Pattern();

        /**
         * @brief 全名字匹配：名字从起点到末尾被模式整体覆盖才算命中。
         *
         * @internal 返回值分三类：命中、不命中、触限等错误。调用方对
         *           第三类按不命中处理。被移动走的对象永不命中。
         */
        [[nodiscard]]
        auto full_match(std::string_view name) -> Outcome;

        /**
         * @brief 部分匹配：模式覆盖名字中的任意一段即命中。
         *
         * @internal 结局三分与全匹配相同；模式自带的行首行尾锚与词边界
         *           照常生效。被移动走的对象永不命中。
         */
        [[nodiscard]]
        auto partial_match(std::string_view name) -> Outcome;

        /**
         * @brief 模式能否匹配空串：按部分匹配语义用空串试一次。
         *
         * @internal 只依赖部分匹配这一个查询，不用引擎的专有接口。调用
         *           方拿它当前置闸：能匹配空串的模式对任何名字都成立，
         *           名字维度的命中没有信息。试探触限等错误按"不能"处
         *           理；被移动走的对象恒为"不能"。
         */
        [[nodiscard]]
        auto matches_empty() -> bool;

    private:
        /** @brief 接管一份编译好的模式；匹配数据块与匹配上下文随后补齐。 */
        explicit Pattern(pcre2_code_8* code) noexcept : code_{code} {}

        /** @brief 释放三项引擎资源。 */
        auto release() noexcept -> void;

        /**
         * @brief 引擎一次匹配调用的返回值折成结局。
         *
         * @internal 非负即命中；不命中与触限等错误按引擎的返回值分开，
         *           后者一律折成"放弃"。被移动走的对象没有模式，引擎
         *           回报空指针错误，同样落在"放弃"里。
         */
        [[nodiscard]]
        static auto outcome_of(int rc) noexcept -> Outcome;

        /** @brief 带选项位调用引擎：两个匹配查询共用这一个调用点。 */
        auto match(std::string_view name, std::uint32_t options) -> Outcome;

        pcre2_code_8*          code_ = nullptr;
        pcre2_match_data_8*    data_ = nullptr;
        pcre2_match_context_8* context_ = nullptr;
    };

    auto Pattern::compile(std::string_view pattern) noexcept
        -> std::optional<Pattern>
    {
        int           error_code = 0;
        std::size_t   error_offset = 0;
        pcre2_code_8* code = pcre2_compile_8(
            reinterpret_cast<const std::uint8_t*>(pattern.data()),
            pattern.size(),
            0,
            &error_code,
            &error_offset,
            nullptr);
        if (code == nullptr)
        {
            return std::nullopt;
        }
        // 对象一拿到模式就接管释放：后面任一步失败，已拿到的部分由析
        // 构函数释放。
        Pattern out{code};
        out.data_ = pcre2_match_data_create_from_pattern_8(code, nullptr);
        if (out.data_ == nullptr)
        {
            return std::nullopt;
        }
        out.context_ = pcre2_match_context_create_8(nullptr);
        if (out.context_ == nullptr)
        {
            return std::nullopt;
        }
        pcre2_set_match_limit_8(out.context_, MATCH_STEP_LIMIT);
        return out;
    }

    Pattern::Pattern(Pattern&& other) noexcept
        : code_{std::exchange(other.code_, nullptr)},
          data_{std::exchange(other.data_, nullptr)},
          context_{std::exchange(other.context_, nullptr)}
    {
    }

    auto Pattern::operator=(Pattern&& other) noexcept -> Pattern&
    {
        if (this != &other)
        {
            release();
            code_ = std::exchange(other.code_, nullptr);
            data_ = std::exchange(other.data_, nullptr);
            context_ = std::exchange(other.context_, nullptr);
        }
        return *this;
    }

    Pattern::~Pattern() { release(); }

    auto Pattern::release() noexcept -> void
    {
        pcre2_match_context_free_8(context_);
        pcre2_match_data_free_8(data_);
        pcre2_code_free_8(code_);
    }

    auto Pattern::outcome_of(int rc) noexcept -> Outcome
    {
        if (rc >= 0)
        {
            return Outcome::Hit;
        }
        if (rc == cg::util::pcre2::ERROR_NOMATCH)
        {
            return Outcome::Miss;
        }
        return Outcome::Aborted;
    }

    auto Pattern::match(std::string_view name, std::uint32_t options) -> Outcome
    {
        const int rc = pcre2_match_8(
            code_,
            reinterpret_cast<const std::uint8_t*>(name.data()),
            name.size(),
            0,
            options,
            data_,
            context_);
        return outcome_of(rc);
    }

    auto Pattern::full_match(std::string_view name) -> Outcome
    {
        return match(
            name,
            cg::util::pcre2::ANCHORED | cg::util::pcre2::ENDANCHORED);
    }

    auto Pattern::partial_match(std::string_view name) -> Outcome
    {
        return match(name, 0);
    }

    auto Pattern::matches_empty() -> bool
    {
        // 显式给出非空指针的空串：不依赖引擎对空指针主题的宽容处理。
        return partial_match(std::string_view{"", 0}) == Outcome::Hit;
    }

} // namespace cg::util::regex
