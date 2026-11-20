/**
 * @file symbits.kernels.avx2.cpp
 *
 * @brief SymBits 批量位运算的 AVX2 档（本 TU 整体以 AVX2 指令集编译）。
 *
 * @internal MSVC 无按函数 target 属性，向量指令只能以 TU 为单位启用
 *           （构建脚本对本文件单独施加 AVX2 开关）。主体循环每次
 *           4 个 word（256 位）：对齐装载/存储（load/store，dst 与 src
 *           基址由 SymBits 的 AlignedWordAllocator 保证 64 字节对齐，
 *           256 位块对齐由 64 整除自动满足）；不足 4 word 的尾部退回
 *           标量循环（尾部地址不保证块对齐，标量无对齐要求）。
 *           语义与标量档逐位相同（按位或的交换/结合律与分块无关），
 *           等价测试对两档输出做逐字比对。调用前置条件见
 *           symbits.kernels.h 的 or_words_avx2 说明（基址 64 字节
 *           对齐契约）。
 */
#include <immintrin.h>

#include "symbits.kernels.h"

namespace cg::analysis::propagate_detail
{

    void or_words_avx2(
        std::uint64_t*       dst,
        const std::uint64_t* src,
        std::size_t          count)
    {
        std::size_t i = 0;
        for (; i + 4 <= count; i += 4)
        {
            const __m256i lhs =
                _mm256_load_si256(reinterpret_cast<const __m256i*>(dst + i));
            const __m256i rhs =
                _mm256_load_si256(reinterpret_cast<const __m256i*>(src + i));
            _mm256_store_si256(
                reinterpret_cast<__m256i*>(dst + i),
                _mm256_or_si256(lhs, rhs));
        }
        for (; i < count; ++i)
        {
            dst[i] |= src[i];
        }
    }

} // namespace cg::analysis::propagate_detail
