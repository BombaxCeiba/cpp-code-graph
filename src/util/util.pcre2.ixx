module;
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h> // 第三方头文件，仅限全局模块片段

/**
 * @module cg.util.pcre2
 *
 * @brief PCRE2 正则引擎 C 接口的再导出：按 8 位码元宽度包含引擎头
 *        文件，把匹配层用到的函数、类型与常量原样导出，自身不含任何
 *        实现。
 *
 * @internal
 * 头文件是纯声明，只出现在本单元的全局模块片段。函数与不透明类型的
 * 真实名都带宽度后缀（头文件里的无后缀名是宏，宏不随模块导出）；选项
 * 位与返回值同样是宏，这里以同值常量重新给出。本单元不进伞模块：
 * 引擎只经本单元与匹配层可达。库以静态库链接，目标公开给出头文件路
 * 径与静态链接宏。
 */
export module cg.util.pcre2;

import std;

export {
    using ::pcre2_code_8;
    using ::pcre2_code_free_8;
    using ::pcre2_compile_8;
    using ::pcre2_config_8;
    using ::pcre2_match_8;
    using ::pcre2_match_context_8;
    using ::pcre2_match_context_create_8;
    using ::pcre2_match_context_free_8;
    using ::pcre2_match_data_8;
    using ::pcre2_match_data_create_from_pattern_8;
    using ::pcre2_match_data_free_8;
    using ::pcre2_set_match_limit_8;
}

export namespace cg::util::pcre2
{

    /// 配置查询项：即时编译是否编入库。
    inline constexpr std::uint32_t CONFIG_JIT = PCRE2_CONFIG_JIT;

    /// 匹配选项：模式须从名字的起点开始匹配。
    inline constexpr std::uint32_t ANCHORED = PCRE2_ANCHORED;

    /// 匹配选项：模式须匹配到名字的末尾。
    inline constexpr std::uint32_t ENDANCHORED = PCRE2_ENDANCHORED;

    /// 匹配接口的返回值：不命中。
    inline constexpr int ERROR_NOMATCH = PCRE2_ERROR_NOMATCH;

    /// 匹配接口的返回值：步数上限触限。
    inline constexpr int ERROR_MATCHLIMIT = PCRE2_ERROR_MATCHLIMIT;

} // namespace cg::util::pcre2
