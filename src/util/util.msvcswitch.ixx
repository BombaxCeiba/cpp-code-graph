/**
 * @module cg.util.msvcswitch
 *
 * @brief MSVC 模块开关规范拼写清单（全仓单一事实源）。
 *
 * @internal
 * 消费方三处，全部经本清单取名单、绝不自带副本：cg.msbuild.translate
 * 的分值开关投影（cdb 忠实成对保留）、cg.clang.bmi 的剥离层与归类判定
 * （喂 clang 家工具前成对剥除）、cg.clang.pch 的影子重放剥离分支。
 * 拼写按 MSBuild 规范记录（无 '/' 前缀、无值段）；各消费方按自己的方言
 * 惯例做前缀与大小写适配。暂基准 = 剥离层的历史六项全集；若后续上游
 * 审计产出机读清单，以对表校正为准。clang 家工具对 /ifcOutput 族无
 * 对应实现（产物位置由 DAG 节点与闭包注入决定），故这些开关只在 cdb
 * 里忠实记录、绝不进入引擎 argv。
 */
export module cg.util.msvcswitch;

import std;

export namespace cg::util::msvcswitch
{

    /**
     * @brief 带值模块开关（两段式 "X 值" 与冒号连写 "X:value" 都算）。
     *
     * @internal ifcOutput/scanDependencies/sourceDependencies 是 .ixx 双趟
     * 命令的实发开关；ifcSearchDir/reference 是消费 TU 的搜索与名单开关；
     * exportHeader 是头文件单元产物开关（语料未实发不影响入表——清单对齐
     * 全集，语料实证不是入表的必要条件）。
     */
    inline constexpr std::string_view valued[] = {
        "ifcOutput",
        "scanDependencies",
        "sourceDependencies",
        "exportHeader",
        "ifcSearchDir",
        "reference"};

    /** @brief 无值单 token 模块开关（TU 性质标记，无值段可吞）。 */
    inline constexpr std::string_view single[] = {
        "interface",
        "internalPartition"};

} // namespace cg::util::msvcswitch
