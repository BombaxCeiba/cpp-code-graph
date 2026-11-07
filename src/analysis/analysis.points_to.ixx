/**
 * @module cg.analysis.points_to
 *
 * @brief TypeOracle 接缝：全程序指针/别名分析的替换点。
 *
 * @internal 当前实现 StubTypeOracle 只回放传播状态已经给出的类型集；
 * 未来做过程间指针分析时，只在本模块内替换实现，analysis.propagate /
 * analysis.dispatch 的调用形态保持不变。依赖方向：本单元只依赖
 * cg.analysis.model。
 */
export module cg.analysis.points_to;

import std;
import cg.analysis.model;

export namespace cg::analysis
{

    /**
     * @brief 当前 stub：把线索自身的类型集当作 oracle 结果。
     *
     * 该形态让“未来替换为真正的指向分析”成为纯实现替换：dispatch 只问
     * oracle “这个变量的动态类型集是什么”，从不假设线索字段是最终答案。
     */
    class StubTypeOracle final : public TypeOracle
    {
    public:
        [[nodiscard]]
        auto points_to(const TypeHintRec& hint) const
            -> std::vector<std::string> override
        {
            return hint.type_keys;
        }
    };

} // namespace cg::analysis
