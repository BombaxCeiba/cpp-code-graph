#pragma once
/**
 * @file symbits.kernels.h
 * @brief SymBits 批量位运算的三档原语声明（标量基线/AVX2/AVX-512）。
 *
 * @internal
 * 【必须是经典头】本头同时被两个世界包含：cg.analysis.propagate 的
 * 全局模块片段（取三档函数与函数指针类型的声明）与经典翻译单元
 * （提供各档定义；向量档的 TU 以整体 /arch 编译）。MSVC 没有按函数
 * 的 target 属性，同一翻译单元内无法混编标量与向量档，分档只能分
 * TU——每档一个经典 TU、各自整体施加本档指令集开关，是本工程既定
 * 的分发形态。头内只放声明，不放定义：定义在各档 TU 里，经链接期
 * 解析，符号名三档不同，热路径由 cg.analysis.propagate 内的函数指针
 * （探测一次、进程内固定）选择。当前语料下向量收益是微秒级，本三档
 * 的意义在架构完备与语料预埋；任何一档的语义必须与标量基线逐位
 * 相同（等价测试逐字比对），向量档绝不引入任何可观测差异。
 */
#include <cstddef>
#include <cstdint>

namespace cg::analysis::propagate_detail
{

    /**
     * @brief 批量位运算档位（进程启动后首次使用时探测一次，此后固定）。
     *
     * @internal 档位只由 CPU 与 OS 能力决定，与输入数据无关；取值顺序
     *           即能力包含顺序（Avx512 机器必然可安全执行 Avx2 档）。
     */
    enum class BitsTier : std::uint8_t
    {
        Scalar = 0,
        Avx2 = 1,
        Avx512 = 2,
    };

    /**
     * @brief 按位或原语的统一形态：对 i < count 逐 word 做 dst[i] |= src[i]。
     *
     * @internal 契约：dst 与 src 不得部分重叠（dst == src 时按位或自身为
     *           恒等，安全）；src 只读；dst 的 [0, count) 区间必写、区间
     *           外不动；dst 与 src 的基址必须 64 字节对齐（SymBits 存储
     *           经 AlignedWordAllocator 保证——向量档主循环以对齐版指令
     *           整块读写，非对齐基址是硬件异常，任何新调用方须持有同一
     *           对齐契约；标量档无对齐要求）。三档实现必须满足同一契约
     *           并产生逐位相同的结果。
     */
    using OrWordsFn = void (*)(
        std::uint64_t*       dst,
        const std::uint64_t* src,
        std::size_t          count);

    /** @brief 标量基线档：逐 word 标量或（任何机器可执行，其余档的对照基准）。
     */
    void or_words_scalar(
        std::uint64_t*       dst,
        const std::uint64_t* src,
        std::size_t          count);

    /**
     * @brief AVX2 档：每次 4 个 word（256 位）的向量或，尾部标量收尾。
     *
     * @internal 前置条件：本机探测确认 AVX2（CPU 与 OS 双重确认）之后才
     *           可调用；等价测试直接调用前必须先过档位门，否则在旧机器
     *           上执行向量指令会触发非法指令异常。
     */
    void or_words_avx2(
        std::uint64_t*       dst,
        const std::uint64_t* src,
        std::size_t          count);

    /**
     * @brief AVX-512 档：每次 8 个 word（512 位）的向量或，尾部标量收尾。
     *
     * @internal 前置条件与 AVX2 档同型：探测确认本机支持本档后才可调用。
     */
    void or_words_avx512(
        std::uint64_t*       dst,
        const std::uint64_t* src,
        std::size_t          count);

    /**
     * @brief 档位的稳定名字（"scalar"/"avx2"/"avx512"，未知值为 "scalar"）。
     *
     * @internal 仅供测试与诊断输出；不进入任何落库观测面。
     */
    [[nodiscard]]
    const char* bits_tier_name(BitsTier tier);

} // namespace cg::analysis::propagate_detail
