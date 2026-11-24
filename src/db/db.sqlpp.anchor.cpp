/**
 * @file db.sqlpp.anchor.cpp
 * @brief sqlpp23 内联静态量的经典翻译单元锚点。
 *
 * @internal
 * 动机（MSVC 链接期缺口；先例为 cg_core 中的经典 TU
 * util.json.bridge.cpp）：MSVC 模块单元仅经 `import sqlpp23.core;` 或
 * `import sqlpp23.sqlite3;` 触达这些内联变量时，会发射 UNDEF 引用却不发射
 * COMDAT 定义——树内的模块单元都不文本包含声明它们的 sqlpp23 头（表模型
 * 模块的全局模块片段只有名字标签宏头），定义职责被委托给文本上拥有它们的
 * 模块目标文件（sqlpp23.core.cppm.obj），而该目标文件永远到不了 ccg 的
 * 链接行：OBJECT->OBJECT 链接传播 BMI 与使用要求，但不传播目标文件。一个
 * odr-use 这些静态量的经典 TU 会向 cg_core 发射可折叠的 COMDAT，搭上每条
 * 最终链接边（ccg.exe 与全部 cg_test_*）。clang 侧会把读取常量折叠掉、
 * 根本不发射这些符号，因此多余的 COMDAT 在那里是惰性的。
 */

#include <sqlpp23/core/basic/join_fwd.h>             // 连接类型的 _name
#include <sqlpp23/core/basic/star.h>                 // star
#include <sqlpp23/core/clause/select_flags.h>        // all、distinct、no_flag
#include <sqlpp23/core/consistent.h>                 // consistent_t::value
#include <sqlpp23/core/default_value.h>              // default_value
#include <sqlpp23/core/function/current_date.h>      // current_date
#include <sqlpp23/core/function/current_time.h>      // current_time
#include <sqlpp23/core/function/current_timestamp.h> // current_timestamp
#include <sqlpp23/core/name/name_tag.h>              // no_name_t::name
#include <sqlpp23/core/operator/arithmetic_expression.h> // plus、minus 等
#include <sqlpp23/core/operator/assign_expression.h>     // op_assign
#include <sqlpp23/core/operator/bit_expression.h>        // bit_and、bit_or 等
#include <sqlpp23/core/operator/comparison_expression.h> // equal_to、greater 等
#include <sqlpp23/core/operator/comparison_functions.h>  // like、in、is_null 等
#include <sqlpp23/core/operator/logical_expression.h>    // logical_and/or/not
#include <sqlpp23/core/reader.h>                         // read
#include <sqlpp23/core/wrapped_static_assert.h> // wrapped_static_assert
#include <sqlpp23/sqlite3/database/exception.h> // sqlite3::fallback

namespace
{

    /**
     * @brief sqlpp23 内联静态量的 odr-use 锚点组。
     *
     * @internal
     * 引用绑定会 odr-use 内联变量并可靠地强制 COMDAT 发射；聚合初始化内的
     * 取地址在 /Od 下实测可被常量折叠、什么都不发射，故不采用。锚定面按
     * 缺陷的受害面划定，不按当下浮出过未解析的符号：core 下全部非模板类
     * 的静态数据成员（运算符 symbol、连接 _name 与其余三个），加上全部非
     * 模板的命名空间级内联变量对象（core 的九个与 sqlite3 的 fallback）
     * ——它们的定义同样只经 import 可达。这样未来的查询无论用到哪个运算
     * 符、连接或字面量标记，都不会再逐个浮出同一缺口。
     *
     * 不在锚定面内的类别及理由：
     * - 命名空间级的 `static constexpr`（debug_enabled、
     *   quiet_auto_rollback、report_auto_rollback）是内部链接，每个翻译
     *   单元自给定义；
     * - 类模板的静态数据成员与变量模板（各种特征值 value、is_*_v 等）
     *   的定义段随实例化发射到使用方；
     * - sort_order / concat 路径没有 `symbol` 静态量。
     */

    // 比较运算符
    [[maybe_unused]]
    const char* const& g_anchor_less = sqlpp::less::symbol;
    [[maybe_unused]]
    const char* const& g_anchor_less_equal = sqlpp::less_equal::symbol;
    [[maybe_unused]]
    const char* const& g_anchor_equal_to = sqlpp::equal_to::symbol;
    [[maybe_unused]]
    const char* const& g_anchor_not_equal_to = sqlpp::not_equal_to::symbol;
    [[maybe_unused]]
    const char* const& g_anchor_greater_equal = sqlpp::greater_equal::symbol;
    [[maybe_unused]]
    const char* const& g_anchor_greater = sqlpp::greater::symbol;

    // 逻辑运算符
    [[maybe_unused]]
    const char* const& g_anchor_logical_and = sqlpp::logical_and::symbol;
    [[maybe_unused]]
    const char* const& g_anchor_logical_or = sqlpp::logical_or::symbol;
    [[maybe_unused]]
    const char* const& g_anchor_logical_not = sqlpp::logical_not::symbol;

    // 算术运算符
    [[maybe_unused]]
    const char* const& g_anchor_plus = sqlpp::plus::symbol;
    [[maybe_unused]]
    const char* const& g_anchor_minus = sqlpp::minus::symbol;
    [[maybe_unused]]
    const char* const& g_anchor_multiplies = sqlpp::multiplies::symbol;
    [[maybe_unused]]
    const char* const& g_anchor_divides = sqlpp::divides::symbol;
    [[maybe_unused]]
    const char* const& g_anchor_negate = sqlpp::negate::symbol;
    [[maybe_unused]]
    const char* const& g_anchor_modulus = sqlpp::modulus::symbol;

    // 位运算符
    [[maybe_unused]]
    const char* const& g_anchor_bit_and = sqlpp::bit_and::symbol;
    [[maybe_unused]]
    const char* const& g_anchor_bit_or = sqlpp::bit_or::symbol;
    [[maybe_unused]]
    const char* const& g_anchor_bit_xor = sqlpp::bit_xor::symbol;
    [[maybe_unused]]
    const char* const& g_anchor_bit_not = sqlpp::bit_not::symbol;
    [[maybe_unused]]
    const char* const& g_anchor_bit_shift_left = sqlpp::bit_shift_left::symbol;
    [[maybe_unused]]
    const char* const& g_anchor_bit_shift_right =
        sqlpp::bit_shift_right::symbol;

    // 赋值
    [[maybe_unused]]
    const char* const& g_anchor_op_assign = sqlpp::op_assign::symbol;

    // 比较函数
    [[maybe_unused]]
    const char* const& g_anchor_op_is_null = sqlpp::op_is_null::symbol;
    [[maybe_unused]]
    const char* const& g_anchor_op_is_not_null = sqlpp::op_is_not_null::symbol;
    [[maybe_unused]]
    const char* const& g_anchor_op_is_distinct_from =
        sqlpp::op_is_distinct_from::symbol;
    [[maybe_unused]]
    const char* const& g_anchor_op_is_not_distinct_from =
        sqlpp::op_is_not_distinct_from::symbol;
    [[maybe_unused]]
    const char* const& g_anchor_op_like = sqlpp::op_like::symbol;
    [[maybe_unused]]
    const char* const& g_anchor_operator_in = sqlpp::operator_in::symbol;
    [[maybe_unused]]
    const char* const& g_anchor_operator_not_in =
        sqlpp::operator_not_in::symbol;

    // 连接
    [[maybe_unused]]
    const char* const& g_anchor_cross_join = sqlpp::cross_join_t::_name;
    [[maybe_unused]]
    const char* const& g_anchor_inner_join = sqlpp::inner_join_t::_name;
    [[maybe_unused]]
    const char* const& g_anchor_left_outer_join =
        sqlpp::left_outer_join_t::_name;
    [[maybe_unused]]
    const char* const& g_anchor_right_outer_join =
        sqlpp::right_outer_join_t::_name;
    [[maybe_unused]]
    const char* const& g_anchor_full_outer_join =
        sqlpp::full_outer_join_t::_name;

    // 其余非模板类的静态数据成员
    [[maybe_unused]]
    const auto& g_anchor_no_name = sqlpp::no_name_t::name;
    [[maybe_unused]]
    const bool& g_anchor_consistent = sqlpp::consistent_t::value;
    [[maybe_unused]]
    const bool& g_anchor_wrapped_static_assert =
        sqlpp::wrapped_static_assert::value;

    // 命名空间级内联变量对象
    [[maybe_unused]]
    const sqlpp::star_t& g_anchor_star = sqlpp::star;
    [[maybe_unused]]
    const sqlpp::all_t& g_anchor_all = sqlpp::all;
    [[maybe_unused]]
    const sqlpp::distinct_t& g_anchor_distinct = sqlpp::distinct;
    [[maybe_unused]]
    const sqlpp::no_flag_t& g_anchor_no_flag = sqlpp::no_flag;
    [[maybe_unused]]
    const sqlpp::default_value_t& g_anchor_default_value = sqlpp::default_value;
    [[maybe_unused]]
    const sqlpp::current_date_t& g_anchor_current_date = sqlpp::current_date;
    [[maybe_unused]]
    const sqlpp::current_time_t& g_anchor_current_time = sqlpp::current_time;
    [[maybe_unused]]
    const sqlpp::current_timestamp_t& g_anchor_current_timestamp =
        sqlpp::current_timestamp;
    [[maybe_unused]]
    const sqlpp::reader_t& g_anchor_read = sqlpp::read;
    [[maybe_unused]]
    const auto& g_anchor_sqlite3_fallback = sqlpp::sqlite3::fallback;

} // namespace
