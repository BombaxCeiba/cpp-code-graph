/**
 * @module cg.cli.lookup.name
 *
 * @brief 钩子查找路径对符号名的两处形态判定：限定链从哪里切开、一个
 *        名字原样当作查找模式还能不能查回来。
 *
 * 纯字符串判定，不碰库、不读环境变量。符号名表存的是裸名，其中有两
 * 类名字容易被读错形态：模板实例名（尖括号里带双冒号，例如
 * nodes_of<sqlpp::value_t<T>>），以及带内嵌路径的描述型长名。
 */
export module cg.cli.lookup.name;

import std;

namespace
{

    /** @brief 是否正则语法里在字符类之外带特殊含义的字符。 */
    [[nodiscard]]
    constexpr auto is_regex_special(char c) -> bool
    {
        constexpr std::string_view special = "\\^$*+?()[]{}|";
        return special.find(c) != std::string_view::npos;
    }

    /** @brief 是否控制字符（放进注入文本会破坏行结构）。 */
    [[nodiscard]]
    constexpr auto is_control_char(char c) -> bool
    {
        const auto value = static_cast<unsigned char>(c);
        return value < 0x20 || value == 0x7F;
    }

    /** @brief 整段是一个裸标识符：字母或下划线起头，其余为字母数字或
     *          下划线。 */
    [[nodiscard]]
    auto is_bare_identifier(std::string_view name) -> bool
    {
        if (name.empty()
            || !(
                std::isalpha(static_cast<unsigned char>(name[0]))
                || name[0] == '_'))
        {
            return false;
        }
        return std::ranges::all_of(
            name.substr(1),
            [](char c)
            {
                return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
            });
    }
}

export namespace cg::cli::detail
{

    /**
     * @brief 限定链分隔位置：尖括号之外最后一个双冒号的下标。
     *
     * @internal 查找路径把 A::b 形态拆成父链与裸名。模板实例名里的双冒号
     *           属于模板实参，不是分隔符：按最后一个双冒号切，会把
     *           nodes_of<sqlpp::value_t<T>> 切成父链 nodes_of<sqlpp 与裸名
     *           value_t<T>>，装载为空。尖括号按出现顺序计深度，多出的右
     *           尖括号不把深度压成负数。
     *
     * @param name 符号名或标识符链。
     * @return 分隔用双冒号的下标；尖括号之外没有双冒号时为空下标。
     */
    [[nodiscard]]
    auto chain_separator_of(std::string_view name) -> std::size_t
    {
        std::size_t depth = 0;
        std::size_t separator = std::string_view::npos;
        for (std::size_t i = 0; i + 1 < name.size(); ++i)
        {
            const char c = name[i];
            if (c == '<')
            {
                ++depth;
            }
            else if (c == '>')
            {
                depth = depth > 0 ? depth - 1 : 0;
            }
            else if (c == ':' && name[i + 1] == ':' && depth == 0)
            {
                separator = i;
            }
        }
        return separator;
    }

    /**
     * @brief 名字原样当作查找模式还能不能查回来：只看名字自身的形态。
     *
     * @internal 查找入口对模式有两道前置。裸标识符走标识符链查询，没有
     *           长度护栏，多长都查得回来。其余形态走受限正则路径：模式
     *           不能超过长度护栏；原样当正则用时必须还匹配名字自身，所
     *           以不能含正则特殊字符（点号通配任意字符，名字自身仍在命
     *           中之内）；装载按限定链切分，所以尖括号之外不能有双冒号。
     *           控制字符会破坏注入文本的行结构，一律不算。噪声词由调用
     *           方结合词表排除，本函数不看词表。
     *
     * @param name        符号名。
     * @param pattern_max 受限正则路径的模式字节长度上限。
     * @return 形态上能原样重查为真。
     */
    [[nodiscard]]
    auto requeryable_name_shape(std::string_view name, std::size_t pattern_max)
        -> bool
    {
        if (is_bare_identifier(name))
        {
            return true;
        }
        if (name.empty() || name.size() > pattern_max)
        {
            return false;
        }
        for (const char c : name)
        {
            if (is_regex_special(c) || is_control_char(c))
            {
                return false;
            }
        }
        return chain_separator_of(name) == std::string_view::npos;
    }

} // namespace cg::cli::detail
