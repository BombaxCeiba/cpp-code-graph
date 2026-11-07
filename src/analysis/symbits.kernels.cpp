/**
 * @file symbits.kernels.cpp
 *
 * @brief SymBits 批量位运算的标量基线档（默认指令集的经典翻译单元）。
 *
 * @internal 本 TU 不施加任何 /arch 开关，因此可在任何 x86-64 基线上
 *           执行，是三档对照的公共基准：向量档的输出必须与本档逐位
 *           相同。编译器对本循环的自动向量化不影响抽象语义，等价性
 *           由 C++ 语义而非具体指令保证。
 */
#include "symbits.kernels.h"

namespace cg::analysis::propagate_detail
{

    void or_words_scalar(
        std::uint64_t*       dst,
        const std::uint64_t* src,
        std::size_t          count)
    {
        for (std::size_t i = 0; i < count; ++i)
        {
            dst[i] |= src[i];
        }
    }

    const char* bits_tier_name(BitsTier tier)
    {
        switch (tier)
        {
        case BitsTier::Avx2:
            return "avx2";
        case BitsTier::Avx512:
            return "avx512";
        case BitsTier::Scalar:
            break;
        }
        return "scalar";
    }

} // namespace cg::analysis::propagate_detail
