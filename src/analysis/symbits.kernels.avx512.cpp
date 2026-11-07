/**
 * @file symbits.kernels.avx512.cpp
 *
 * @brief SymBits 批量位运算的 AVX-512 档（本 TU 整体以 AVX-512 指令集
 *        编译）。
 *
 * @internal 与 AVX2 档同型的分 TU 理由；主体循环每次 8 个 word
 *           （512 位），尾部标量收尾。探测侧在启用本档前同时确认
 *           CPUID 的 F/VL/BW/DQ 位与操作系统 XCR0 状态位，覆盖编译器
 *           在本 TU 内可能生成的常用 AVX-512 指令子集（尾部标量循环
 *           的自动向量化也在同一开关约束下）。语义与标量档逐位相同，
 *           等价测试做逐字比对。调用前置条件见
 *           symbits.kernels.h 的 or_words_avx512 说明。
 */
#include <immintrin.h>

#include "symbits.kernels.h"

namespace cg::analysis::propagate_detail
{

    void or_words_avx512(
        std::uint64_t*       dst,
        const std::uint64_t* src,
        std::size_t          count)
    {
        std::size_t i = 0;
        for (; i + 8 <= count; i += 8)
        {
            const __m512i lhs =
                _mm512_loadu_si512(reinterpret_cast<const void*>(dst + i));
            const __m512i rhs =
                _mm512_loadu_si512(reinterpret_cast<const void*>(src + i));
            _mm512_storeu_si512(
                reinterpret_cast<void*>(dst + i),
                _mm512_or_si512(lhs, rhs));
        }
        for (; i < count; ++i)
        {
            dst[i] |= src[i];
        }
    }

} // namespace cg::analysis::propagate_detail
