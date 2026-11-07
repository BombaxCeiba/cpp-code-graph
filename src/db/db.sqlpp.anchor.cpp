/**
 * @file db.sqlpp.anchor.cpp
 * @brief sqlpp23 运算符静态量的经典翻译单元锚点。
 *
 * @internal
 * 动机（MSVC 链接期缺口；先例为 cg_core 中的经典 TU
 * util.json.bridge.cpp）：MSVC 模块单元仅经
 * `import sqlpp23.core;` 触达这些内联变量时，会发射 UNDEF 引用却不发射
 * COMDAT 定义——三个运算符头在 db.tables.h 的文本全局模块片段闭包之外，
 * 定义职责被委托给文本上拥有它们的模块目标文件（sqlpp23.core.cppm.obj），
 * 而该目标文件永远到不了 ccg 的链接行：OBJECT->OBJECT 链接传播 BMI 与
 * 使用要求，但不传播目标文件。一个 odr-use 这些静态量的经典 TU 会向
 * cg_core 发射可折叠的 COMDAT，搭上每条最终链接边（ccg.exe 与全部
 * cg_test_*）。clang 侧会把读取常量折叠掉、根本不发射这些符号，因此
 * 多余的 COMDAT 在那里是惰性的。
 */

#include <sqlpp23/core/operator/arithmetic_expression.h> // plus、minus 等
#include <sqlpp23/core/operator/comparison_expression.h> // equal_to、greater 等
#include <sqlpp23/core/operator/logical_expression.h>    // logical_and/or/not

namespace
{
    /**
     * @brief 运算符 symbol 内联变量的 odr-use 锚点组。
     *
     * @internal
     * 引用绑定会 odr-use 内联变量并可靠地强制 COMDAT 发射；聚合初始化内的
     * 取地址在 /Od 下实测可被常量折叠、什么都不发射，故不采用。三个头里的
     * 全部运算符静态量都被锚定，而不只锚定实际浮出过未解析的五个
     * （equal_to、logical_and、logical_or、greater、plus）：避免未来的查询
     * 一次一个运算符地重新浮出同一缺口。sort_order/concat 路径没有
     * `symbol` 静态量，无需锚定。
     */
    [[maybe_unused]]
    const char* const& anchor_less = sqlpp::less::symbol;
    [[maybe_unused]]
    const char* const& anchor_less_equal = sqlpp::less_equal::symbol;
    [[maybe_unused]]
    const char* const& anchor_equal_to = sqlpp::equal_to::symbol;
    [[maybe_unused]]
    const char* const& anchor_not_equal_to = sqlpp::not_equal_to::symbol;
    [[maybe_unused]]
    const char* const& anchor_greater_equal = sqlpp::greater_equal::symbol;
    [[maybe_unused]]
    const char* const& anchor_greater = sqlpp::greater::symbol;
    [[maybe_unused]]
    const char* const& anchor_logical_and = sqlpp::logical_and::symbol;
    [[maybe_unused]]
    const char* const& anchor_logical_or = sqlpp::logical_or::symbol;
    [[maybe_unused]]
    const char* const& anchor_logical_not = sqlpp::logical_not::symbol;
    [[maybe_unused]]
    const char* const& anchor_plus = sqlpp::plus::symbol;
    [[maybe_unused]]
    const char* const& anchor_minus = sqlpp::minus::symbol;
    [[maybe_unused]]
    const char* const& anchor_multiplies = sqlpp::multiplies::symbol;
    [[maybe_unused]]
    const char* const& anchor_divides = sqlpp::divides::symbol;
    [[maybe_unused]]
    const char* const& anchor_negate = sqlpp::negate::symbol;
    [[maybe_unused]]
    const char* const& anchor_modulus = sqlpp::modulus::symbol;
} // namespace
