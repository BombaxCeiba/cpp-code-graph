/**
 * @module cg.graph.headcache
 *
 * @brief 头级缓存本体：非主文件的抽取产物分区按
 *        「归属文件 × 编译变体键 × 包含闭包摘要」缓存，翻译单元走查命中
 *        头子树时跳过递归、回放缓存分区。
 *
 * 键的封闭性（正确性红线：符号漂移零容忍——缓存键必须封闭所有影响产物
 * 语义的输入；拿不准的形态宁可 miss）：
 * - 编译变体键 = parse_digest（重放 argv 摘要 + 模块覆盖层纪元 + PCH 规
 *   划身份）。加载器已从 argv 剥掉条目自身的主文件 token，同旗标的不同
 *   TU 共享同一变体键；-I/-D/-U/-std/方言开关全部在 argv 内。
 * - 主文件锚 = 翻译单元主文件路径 + 内容哈希。分区回放必须是「同主文件
 *   的重解析」：若干边发射依赖整 TU 可见性——friend_of 只在友元定义在
 *   本 TU 可见时发射、uses/extends 对只见前向声明的目标退到按名延迟形
 *   态——同一头在不同主文件的 TU 里产出不同边集，跨 TU 回放会把 A 的
 *   可见性漏进 B（incr 等价测试的 friend_of 实证）。主文件锚同时把
 *   「主文件在包含点前 #define」的宏近似洞整个封死（宏近似方案的
 *   已知残留不再可达：主文件字节恰是键的一部分）。
 * - 包含闭包摘要 = 该文件在库内联合包含图上的前向闭包（含自身）逐文件
 *   「路径 × 内容哈希」的 sha256。闭包内任何文件的内容变化都会改键
 *   （本文件的 #include 目标搬家、被包含头的类改命名空间都会让缓存产物
 *   里的目标键失效——闭包摘要把这整族输入封闭进键）。闭包身份未知的
 *   文件（本运行新见、尚无内容哈希）一律 miss。
 * - 纪元（引擎记号 + 根集摘要 + 载荷格式版本）在行版本戳层面校验：不
 *   匹配的行载入即弃（换引擎版本/换根集后旧缓存一次性作废）。
 *
 * 已知边界（如实呈报）：主文件锚放弃跨 TU 共享（Rider 的精确宏上下文键
 * 才能安全跨 TU——无宏查询回调下我们的近似做不到），收益上限从结构上界
 * 97.3% 收窄到「同 TU 重解析」场景（重复全量、--force、悬垂边第二遍解
 * 析、爆发式重解析轮）；
 * 每（文件×变体）最多保留 main_cap 个主文件锚（先到先得，超限拒绝记录
 * ——大扇出头的锚集覆盖受限是启用判定实测口径的一部分）。
 *
 * PCH/强制包含/命名模块 TU 不参与缓存（这些输入（PCH 字节、pcm 内容）
 * 不在任何内容哈希内——宁可 miss）：is_module_tu 或 pch.identity 非空的
 * 条目查询恒 miss、记录恒跳过。
 *
 * db.tables.h 置于全局模块片段：本单元导入 cg.db，MSVC 要求 sqlpp23 表
 * 模型定义在此可见，以合并模块记录的模板特化（完整机理见 db.tables.h；
 * graph.traverse 同款纪律）。
 */
module;
#include "../db/db.tables.h"

export module cg.graph.headcache;

import std;
import cg.util;
import cg.util.cstd;
import cg.db;
import cg.extract.model;

export namespace cg::graph::headcache
{

    /** @brief 单文件放共享的变体数上限（保守阈值）。 */
    inline constexpr std::size_t variant_cap = 8;

    /**
     * @brief 单（文件×变体）保留的主文件锚数上限（先到先得）。
     *
     * @internal 主文件锚放弃跨 TU 共享后的容量对冲：大扇出头（500+ 观察
     * 者）的全锚记录会让载荷随观察者数线性膨胀；先到先得 + 编译数据库
     * 序稳定，使重复全量/爆发式重解析轮里「同样的前 main_cap 个主文
     * 件」确定性地命中。
     */
    inline constexpr std::size_t main_cap = 64;

    /** @brief 全缓存载荷预算（字节估计；超预算停止记录，既有条目保留）。 */
    inline constexpr std::size_t payload_budget = 512u << 20;

    /**
     * @brief 观测统计快照（启用判定的命中率口径）。
     *
     * @internal 计数在缓存内为原子量（工人线程并发查询递增）；本结构是
     * 读取侧的普通值快照。
     */
    struct Stats
    {
        std::int64_t lookups = 0;       // 查询总次数（TU × 遇到的非主文件数）
        std::int64_t hits = 0;          // 命中并回放的分区数
        std::int64_t misses = 0;        // miss（含闭包未知/变体超限/PCH 模块）
        std::int64_t records = 0;       // 本运行落账的分区数
        std::int64_t files = 0;         // 缓存内不同文件数
        std::int64_t capped_files = 0;  // 因变体超限放弃共享的文件数
        std::int64_t loaded_rows = 0;   // 从库载入的行数
        std::int64_t stored_rows = 0;   // 落盘行数
        std::int64_t payload_bytes = 0; // 缓存载荷字节估计
    };

    /**
     * @brief 行版本戳：侧版本号与纪元串（引擎记号+根集摘要）的联合哈希。
     *
     * 落盘行带此值，载入按当前值过滤——换引擎或换根集后旧行整批作废
     * （分析槽 ver 列先例：缓存行按版本行级过滤，旧库无感）。
     */
    [[nodiscard]]
    inline auto row_ver_of(const std::string& epoch) -> std::int64_t
    {
        const std::string key =
            std::to_string(cg::db::head_cache_side_ver) + "#" + epoch;
        const std::string h =
            cg::util::sha256_hex(std::as_bytes(std::span{key}));
        return static_cast<std::int64_t>(
            std::stoull(h.substr(0, 16), nullptr, 16));
    }

    namespace detail
    {

        /** @brief 载荷编码：长度前缀字节串 + 定宽整数的极简写读。 */
        struct BlobWriter
        {
            std::string buf;
            void u8(std::uint8_t v) { buf.push_back(static_cast<char>(v)); }
            void u32(std::uint32_t v)
            {
                for (int i = 0; i < 4; ++i)
                {
                    buf.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
                }
            }
            void i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
            void i64(std::int64_t v)
            {
                const auto u = static_cast<std::uint64_t>(v);
                for (int i = 0; i < 8; ++i)
                {
                    buf.push_back(static_cast<char>((u >> (8 * i)) & 0xff));
                }
            }
            void str(std::string_view s)
            {
                u32(static_cast<std::uint32_t>(s.size()));
                buf += s;
            }
        };

        /** @brief 载荷解码；越界一律返回无值（调用方按 miss 处理）。 */
        struct BlobReader
        {
            std::string_view buf;
            std::size_t      pos = 0;
            bool             bad = false;

            /** @brief 读 1 字节；越界置粘性失败位并返回无值。 */
            [[nodiscard]]
            auto take_u8() -> std::optional<std::uint8_t>
            {
                if (bad || pos + 1 > buf.size())
                {
                    bad = true;
                    return std::nullopt;
                }
                return static_cast<std::uint8_t>(buf[pos++]);
            }

            /** @brief 读 4 字节小端；越界置粘性失败位并返回无值。 */
            [[nodiscard]]
            auto take_u32() -> std::optional<std::uint32_t>
            {
                if (bad || pos + 4 > buf.size())
                {
                    bad = true;
                    return std::nullopt;
                }
                std::uint32_t v = 0;
                for (int i = 0; i < 4; ++i)
                {
                    v |= static_cast<std::uint8_t>(buf[pos + i]) << (8 * i);
                }
                pos += 4;
                return v;
            }

            /** @brief 读 4 字节小端按有符号解释。 */
            [[nodiscard]]
            auto take_i32() -> std::optional<std::int32_t>
            {
                const std::optional<std::uint32_t> u = take_u32();
                if (!u)
                {
                    return std::nullopt;
                }
                return static_cast<std::int32_t>(*u);
            }

            /** @brief 读 8 字节小端；越界置粘性失败位并返回无值。 */
            [[nodiscard]]
            auto take_i64() -> std::optional<std::int64_t>
            {
                if (bad || pos + 8 > buf.size())
                {
                    bad = true;
                    return std::nullopt;
                }
                std::uint64_t u = 0;
                for (int i = 0; i < 8; ++i)
                {
                    u |= static_cast<std::uint64_t>(
                             static_cast<std::uint8_t>(buf[pos + i]))
                         << (8 * i);
                }
                pos += 8;
                return static_cast<std::int64_t>(u);
            }

            /** @brief 读长度前缀字节串；长度越界置粘性失败位并返回无值。 */
            [[nodiscard]]
            auto take_str() -> std::optional<std::string>
            {
                const std::optional<std::uint32_t> n = take_u32();
                if (!n || pos + *n > buf.size())
                {
                    bad = true;
                    return std::nullopt;
                }
                std::string s{buf.data() + pos, *n};
                pos += *n;
                return s;
            }
        };

        /** @brief 分区载荷格式版本（不匹配的行按 miss 丢弃）。
         *
         * @internal v2：头部补编码归属文件路径（v1 缺它，载入侧的
         * 「载荷文件名 vs 行 file_id 路径」一致性校验恒假——17,167 行
         * 全部被静默丢弃、命中恒零，沙盒实测抓的实锤）。
         */
        inline constexpr std::uint8_t payload_format = 3;

        [[nodiscard]]
        inline auto encode_partition(
            const cg::extract::HeadPartition& p,
            const std::string&                main_path,
            const std::string&                main_hash) -> std::string
        {
            BlobWriter w;
            w.u8(payload_format);
            w.str(p.file);
            w.str(main_path);
            w.str(main_hash);
            w.u32(static_cast<std::uint32_t>(p.symbols.size()));
            for (std::size_t si = 0; si < p.symbols.size(); ++si)
            {
                const cg::extract::SymbolRec& s = p.symbols[si];
                w.u8(static_cast<std::uint8_t>(s.kind));
                w.str(s.name);
                w.str(s.ns);
                w.str(s.scope);
                w.str(s.parent);
                w.str(s.file);
                w.i32(static_cast<std::int32_t>(s.line));
                w.i32(static_cast<std::int32_t>(s.col));
                w.i32(static_cast<std::int32_t>(s.flags));
                w.u32(static_cast<std::uint32_t>(s.param_types.size()));
                for (const std::string& t : s.param_types)
                {
                    w.str(t);
                }
                w.u32(static_cast<std::uint32_t>(s.template_params.size()));
                for (const std::string& t : s.template_params)
                {
                    w.str(t);
                }
                w.u8(s.const_method ? 1 : 0);
                w.str(s.unique_key);
                w.str(
                    si < p.symbol_regions.size() ? p.symbol_regions[si]
                                                 : p.file);
            }
            w.u32(static_cast<std::uint32_t>(p.edges.size()));
            for (const cg::extract::EdgeRec& e : p.edges)
            {
                w.str(e.src_key);
                w.u8(e.dst.is_key ? 1 : 0);
                w.str(e.dst.key);
                w.str(e.dst.meta.name);
                w.str(e.dst.meta.ns);
                w.str(e.dst.meta.parent);
                w.u8(e.dst.meta.sig_hash.has_value() ? 1 : 0);
                w.i64(
                    e.dst.meta.sig_hash.has_value() ? *e.dst.meta.sig_hash : 0);
                w.u8(e.dst.meta.const_method ? 1 : 0);
                w.u8(static_cast<std::uint8_t>(e.dst.meta.kind));
                w.u8(e.dst.meta.arg_dep ? 1 : 0);
                w.u8(static_cast<std::uint8_t>(e.kind));
                w.str(e.site_file);
                w.i32(static_cast<std::int32_t>(e.site_line));
                w.i32(static_cast<std::int32_t>(e.attrs));
                w.i32(e.occurrences);
            }
            w.u32(static_cast<std::uint32_t>(p.includes.size()));
            for (const cg::extract::IncludeRec& i : p.includes)
            {
                w.str(i.file);
                w.str(i.target);
                w.i32(static_cast<std::int32_t>(i.line));
                w.u8(i.angled ? 1 : 0);
            }
            w.i64(p.external_dropped);
            w.i64(p.operator_dropped);
            return std::move(w.buf);
        }

        /** @brief 解码产物：分区本体与两个身份串。 */
        struct DecodedPartition
        {
            cg::extract::HeadPartition partition;
            std::string                main_path;
            std::string                main_hash;
        };

        /**
         * @brief 载荷解码；任一字段越界或格式版本不符返回无值。
         *
         * @internal 读序即编码序；粘性失败位使失败后的读取立即失败，
         *           字段可按序全部读出后统一判定（pos/bad 终态与短路
         *           逐字段判等价）。
         */
        [[nodiscard]]
        inline auto decode_partition(std::string_view blob)
            -> std::optional<DecodedPartition>
        {
            BlobReader                         r{blob};
            DecodedPartition                   out;
            const std::optional<std::uint8_t>  fmt = r.take_u8();
            const std::optional<std::string>   file = r.take_str();
            const std::optional<std::string>   main_path = r.take_str();
            const std::optional<std::string>   main_hash = r.take_str();
            const std::optional<std::uint32_t> nsym = r.take_u32();
            if (!fmt || *fmt != payload_format || !file || !main_path
                || !main_hash || !nsym || *nsym > (1u << 24))
            {
                return std::nullopt;
            }
            out.partition.file = std::move(*file);
            out.main_path = std::move(*main_path);
            out.main_hash = std::move(*main_hash);
            out.partition.symbols.resize(*nsym);
            out.partition.symbol_regions.clear();
            out.partition.symbol_regions.resize(*nsym);
            for (std::size_t si = 0; si < out.partition.symbols.size(); ++si)
            {
                cg::extract::SymbolRec&           s = out.partition.symbols[si];
                const std::optional<std::uint8_t> kind = r.take_u8();
                const std::optional<std::string>  name = r.take_str();
                const std::optional<std::string>  ns = r.take_str();
                const std::optional<std::string>  scope = r.take_str();
                const std::optional<std::string>  parent = r.take_str();
                const std::optional<std::string>  sfile = r.take_str();
                const std::optional<std::int32_t> line = r.take_i32();
                const std::optional<std::int32_t> col = r.take_i32();
                const std::optional<std::int32_t> flags = r.take_i32();
                const std::optional<std::uint32_t> np = r.take_u32();
                const std::optional<std::uint32_t> nt = r.take_u32();
                const std::optional<std::uint8_t>  cm = r.take_u8();
                const std::optional<std::string>   unique_key = r.take_str();
                const std::optional<std::string>   region = r.take_str();
                if (!kind || *kind > 9 || !name || !ns || !scope || !parent
                    || !sfile || !line || !col || !flags || !np
                    || *np > (1u << 16) || !nt || *nt > (1u << 16) || !cm
                    || !unique_key || !region)
                {
                    return std::nullopt;
                }
                s.kind = static_cast<cg::extract::SymKind>(*kind);
                s.name = std::move(*name);
                s.ns = std::move(*ns);
                s.scope = std::move(*scope);
                s.parent = std::move(*parent);
                s.file = std::move(*sfile);
                s.line = static_cast<int>(*line);
                s.col = static_cast<int>(*col);
                s.flags = static_cast<std::uint32_t>(*flags);
                s.const_method = *cm != 0;
                s.unique_key = std::move(*unique_key);
                out.partition.symbol_regions[si] = std::move(*region);
                s.param_types.resize(*np);
                for (std::string& t : s.param_types)
                {
                    if (const std::optional<std::string> v = r.take_str(); !v)
                    {
                        return std::nullopt;
                    }
                    else
                    {
                        t = std::move(*v);
                    }
                }
                s.template_params.resize(*nt);
                for (std::string& t : s.template_params)
                {
                    if (const std::optional<std::string> v = r.take_str(); !v)
                    {
                        return std::nullopt;
                    }
                    else
                    {
                        t = std::move(*v);
                    }
                }
            }
            const std::optional<std::uint32_t> nedge = r.take_u32();
            if (!nedge || *nedge > (1u << 26))
            {
                return std::nullopt;
            }
            out.partition.edges.resize(*nedge);
            for (cg::extract::EdgeRec& e : out.partition.edges)
            {
                const std::optional<std::string>  src_key = r.take_str();
                const std::optional<std::uint8_t> is_key = r.take_u8();
                const std::optional<std::string>  dst_key = r.take_str();
                const std::optional<std::string>  dst_name = r.take_str();
                const std::optional<std::string>  dst_ns = r.take_str();
                const std::optional<std::string>  dst_parent = r.take_str();
                const std::optional<std::uint8_t> has_sig = r.take_u8();
                const std::optional<std::int64_t> sig = r.take_i64();
                const std::optional<std::uint8_t> cm = r.take_u8();
                const std::optional<std::uint8_t> mk = r.take_u8();
                const std::optional<std::uint8_t> ad = r.take_u8();
                const std::optional<std::uint8_t> ek = r.take_u8();
                const std::optional<std::string>  site_file = r.take_str();
                const std::optional<std::int32_t> line = r.take_i32();
                const std::optional<std::int32_t> attrs = r.take_i32();
                const std::optional<std::int32_t> occ = r.take_i32();
                if (!src_key || !is_key || !dst_key || !dst_name || !dst_ns
                    || !dst_parent || !has_sig || !sig || !cm || !mk || *mk > 9
                    || !ad || !ek || *ek > 6 || !site_file || !line || !attrs
                    || !occ)
                {
                    return std::nullopt;
                }
                e.src_key = std::move(*src_key);
                e.dst.is_key = *is_key != 0;
                e.dst.key = std::move(*dst_key);
                e.dst.meta.name = std::move(*dst_name);
                e.dst.meta.ns = std::move(*dst_ns);
                e.dst.meta.parent = std::move(*dst_parent);
                if (*has_sig != 0)
                {
                    e.dst.meta.sig_hash = *sig;
                }
                e.dst.meta.const_method = *cm != 0;
                e.dst.meta.kind = static_cast<cg::extract::SymKind>(*mk);
                e.dst.meta.arg_dep = *ad != 0;
                e.kind = static_cast<cg::extract::EdgeKind>(*ek);
                e.site_file = std::move(*site_file);
                e.site_line = static_cast<int>(*line);
                e.attrs = static_cast<std::uint32_t>(*attrs);
                e.occurrences = *occ;
            }
            const std::optional<std::uint32_t> ninc = r.take_u32();
            if (!ninc || *ninc > (1u << 24))
            {
                return std::nullopt;
            }
            out.partition.includes.resize(*ninc);
            for (cg::extract::IncludeRec& i : out.partition.includes)
            {
                const std::optional<std::string>  ifile = r.take_str();
                const std::optional<std::string>  target = r.take_str();
                const std::optional<std::int32_t> line = r.take_i32();
                const std::optional<std::uint8_t> ang = r.take_u8();
                if (!ifile || !target || !line || !ang)
                {
                    return std::nullopt;
                }
                i.file = std::move(*ifile);
                i.target = std::move(*target);
                i.line = static_cast<int>(*line);
                i.angled = *ang != 0;
            }
            const std::optional<std::int64_t> external_dropped = r.take_i64();
            const std::optional<std::int64_t> operator_dropped = r.take_i64();
            if (!external_dropped || !operator_dropped || r.bad)
            {
                return std::nullopt;
            }
            out.partition.external_dropped = *external_dropped;
            out.partition.operator_dropped = *operator_dropped;
            return out;
        }

        /// 按文件名取分区骨架（visited_files 建立的索引）；无分区返回
        /// nullptr（该文件的产物不进任何分区）。
        [[nodiscard]]
        inline auto partition_of(
            const std::unordered_map<std::string, std::size_t>& index,
            std::vector<cg::extract::HeadPartition>&            parts,
            const std::string& f) -> cg::extract::HeadPartition*
        {
            const auto it = index.find(f);
            return it == index.end() ? nullptr : &parts[it->second];
        }

    } // namespace detail

    /**
     * @brief 一次索引运行的头缓存实例：查询桥（工人线程并发查）+ 落账
     *        （主线程，解析波次之间）+ 库持久化（提交段事务内）。
     *
     * @internal 条目一经发布（lookup 可见）不再原地改写：同键重记录整体
     * 替换 map 槽（旧分区指针失效——查询方在桥互斥下取指针、走查线程在
     * 子树跳过期间使用；替换只发生在波次间（主线程、无在飞走查），安全。
     */
    class HeadCache
    {
    public:
        /**
         * @param epoch 纪元串（引擎记号 + 根集摘要）：行版本戳的原料，
         *              不符的库内行载入即弃。
         * @param closure_of 归属文件路径 → 闭包摘要（运行期只读快照）。
         */
        HeadCache(
            std::string                                         epoch,
            const std::unordered_map<std::string, std::string>* closure_of)
            : epoch_{std::move(epoch)}, closure_of_{closure_of},
              row_ver_{row_ver_of(epoch_)}
        {
        }

        /**
         * @brief 查询桥：绑定一个 TU 的完整键（变体 + 主文件锚），栈上
         *        构造、随用随弃。
         */
        struct Bridge final : public cg::extract::HeadCacheBridge
        {
            const HeadCache* owner = nullptr;
            std::string      variant;
            std::string      main_anchor; // 主文件路径 + 哈希

            [[nodiscard]]
            auto lookup(const std::string& file) const
                -> const cg::extract::HeadPartition* override
            {
                return owner->lookup(file, variant, main_anchor);
            }
        };

        /** @brief 生成一个 TU 的查询桥（variant = parse_digest）。 */
        [[nodiscard]]
        auto bridge_for(
            const std::string& variant,
            const std::string& main_path,
            const std::string& main_hash) const -> Bridge
        {
            Bridge b;
            b.owner = this;
            b.variant = variant;
            b.main_anchor = main_path;
            b.main_anchor += '\x1f';
            b.main_anchor += main_hash;
            return b;
        }

        /**
         * @brief 查一个文件在（变体 × 主文件锚）下的缓存分区。
         *
         * miss 条件（宁可 miss）：文件未记录、变体未记录、主文件锚未
         * 记录、变体超限、闭包摘要未知或不匹配。
         */
        [[nodiscard]]
        auto lookup(
            const std::string& file,
            const std::string& variant,
            const std::string& main_anchor) const
            -> const cg::extract::HeadPartition*
        {
            ++a_lookups_;
            const bool dbg =
                cg::util::cstd::get_env("CCG_DEBUG_C1").has_value();
            const auto cit = closure_of_->find(file);
            if (cit == closure_of_->end())
            {
                ++a_misses_;
                if (dbg)
                {
                    std::println(
                        std::cerr,
                        "HCMISS no-closure file='{}'",
                        file);
                }
                return nullptr;
            }
            const std::string& closure = cit->second;
            std::shared_lock   lock{mx_};
            const auto         fit = files_.find(file);
            if (fit == files_.end() || fit->second.capped)
            {
                ++a_misses_;
                if (dbg)
                {
                    std::println(std::cerr, "HCMISS no-file file='{}'", file);
                }
                return nullptr;
            }
            const auto vit = fit->second.by_variant.find(variant);
            if (vit == fit->second.by_variant.end())
            {
                ++a_misses_;
                if (dbg)
                {
                    std::println(
                        std::cerr,
                        "HCMISS no-variant file='{}'",
                        file);
                }
                return nullptr;
            }
            const VariantEntry& ve = vit->second;
            if (ve.closure != closure)
            {
                ++a_misses_;
                if (dbg)
                {
                    std::println(
                        std::cerr,
                        "HCMISS closure file='{}' run={:.16} "
                        "entry={:.16}",
                        file,
                        closure,
                        ve.closure);
                }
                return nullptr;
            }
            const auto mit = ve.by_main.find(main_anchor);
            if (mit == ve.by_main.end())
            {
                ++a_misses_;
                if (dbg)
                {
                    std::println(std::cerr, "HCMISS anchor file='{}'", file);
                }
                return nullptr;
            }
            ++a_hits_;
            return &mit->second.partition;
        }

        /**
         * @brief 落账一个成功 TU 抽取的全部非主文件分区（主文件锚钉死）。
         *
         * main_file 分区永不缓存。变体数超限的文件置 capped（其后恒
         * miss）。同一（文件×变体）的主文件锚数超 main_cap 即拒绝新锚
         * （先到先得，编译数据库序稳定可复现）。超出载荷预算即停止记录
         * （既有条目保留可用）。
         */
        void record(
            const std::string&            variant,
            const std::string&            main_path,
            const std::string&            main_hash,
            const cg::extract::TuExtract& ex)
        {
            if (main_path.empty() || main_hash.empty())
            {
                return; // 主文件锚不完整：整批不记录（宁漏）
            }
            // 按 visited_files 建分区骨架（零产出文件也要有空分区可命中）
            std::unordered_map<std::string, std::size_t> index;
            std::vector<cg::extract::HeadPartition>      parts;
            for (const std::string& f : ex.visited_files)
            {
                if (f == ex.main_file)
                {
                    continue;
                }
                index.emplace(f, parts.size());
                parts.emplace_back();
                parts.back().file = f;
            }
            for (std::size_t si = 0; si < ex.symbols.size(); ++si)
            {
                // 分区归属跟随发射区（游标所在文件），而非符号的归属文件
                // ——基类实例注册从使用点所在区发射、归属定义处文件；跳过
                // 某区必须连同它在别处的产物一起跳过（回放面反之）。
                const std::string& region = si < ex.symbol_regions.size()
                                                ? ex.symbol_regions[si]
                                                : ex.symbols[si].file;
                if (cg::extract::HeadPartition* p =
                        detail::partition_of(index, parts, region))
                {
                    p->symbols.push_back(ex.symbols[si]);
                    p->symbol_regions.push_back(region);
                }
            }
            for (const cg::extract::EdgeRec& e : ex.edges)
            {
                if (cg::extract::HeadPartition* p =
                        detail::partition_of(index, parts, e.site_file))
                {
                    p->edges.push_back(e);
                }
            }
            for (const cg::extract::IncludeRec& i : ex.includes)
            {
                if (cg::extract::HeadPartition* p =
                        detail::partition_of(index, parts, i.file))
                {
                    p->includes.push_back(i);
                }
            }
            for (const auto& [f, drops] : ex.dropped_by_file)
            {
                if (cg::extract::HeadPartition* p =
                        detail::partition_of(index, parts, f))
                {
                    p->external_dropped = drops.first;
                    p->operator_dropped = drops.second;
                }
            }
            std::string main_anchor = main_path;
            main_anchor += '\x1f';
            main_anchor += main_hash;
            std::unique_lock lock{mx_};
            for (cg::extract::HeadPartition& p : parts)
            {
                if (payload_bytes_ >= payload_budget)
                {
                    return; // 预算触顶：后续记录整批放弃
                }
                const auto cit = closure_of_->find(p.file);
                if (cit == closure_of_->end())
                {
                    continue; // 闭包身份未知：不记录（宁漏）
                }
                FileEntry& fe = files_.try_emplace(p.file).first->second;
                if (!fe.capped && fe.by_variant.size() >= variant_cap
                    && !fe.by_variant.contains(variant))
                {
                    fe.capped = true;
                    ++a_capped_files_;
                    continue;
                }
                VariantEntry& ve = fe.by_variant[variant];
                if (ve.by_main.size() >= main_cap
                    && !ve.by_main.contains(main_anchor))
                {
                    continue; // 主锚超限：拒绝新锚（既有锚仍可命中）
                }
                Entry& e = ve.by_main[main_anchor];
                payload_bytes_ -= e.bytes;
                ve.closure = cit->second;
                e.closure = cit->second;
                e.main_path = main_path;
                e.main_hash = main_hash;
                e.bytes = estimate_bytes(p);
                e.partition = std::move(p);
                e.dirty_persist = true;
                fe.persist_paths = true;
                ++a_records_;
                payload_bytes_ += e.bytes;
            }
        }

        /** @brief 从库载入（行版本戳 = 侧版本#纪元 联合哈希，不符即弃）。 */
        void load(
            cg::db::GraphStore&                                    store,
            const std::unordered_map<cg::db::FileId, std::string>& path_of)
        {
            std::int64_t dbg_rows = 0, dbg_no_path = 0, dbg_decode = 0,
                         dbg_closure = 0, dbg_ok = 0;
            const bool   dbg =
                cg::util::cstd::get_env("CCG_DEBUG_C1").has_value();
            const auto loaded = store.head_cache_load(row_ver_);
            for (const cg::db::HeadCacheRow& row : loaded)
            {
                ++dbg_rows;
                const auto pit = path_of.find(row.file_id);
                const auto mit = path_of.find(row.main_id);
                if (pit == path_of.end() || mit == path_of.end())
                {
                    ++dbg_no_path;
                    continue;
                }
                std::optional<detail::DecodedPartition> decoded =
                    detail::decode_partition(row.payload);
                if (!decoded || decoded->main_path != mit->second
                    || decoded->partition.file != pit->second)
                {
                    ++dbg_decode;
                    continue; // 解码失败/拼写身份不符：丢弃（可再生）
                }
                cg::extract::HeadPartition p = std::move(decoded->partition);
                const std::string&         main_path = decoded->main_path;
                const std::string&         main_hash = decoded->main_hash;
                const auto                 cit = closure_of_->find(p.file);
                if (cit == closure_of_->end())
                {
                    ++dbg_closure;
                    continue;
                }
                ++dbg_ok;
                FileEntry& fe = files_.try_emplace(p.file).first->second;
                if (fe.by_variant.size() >= variant_cap
                    && !fe.by_variant.contains(row.variant_digest))
                {
                    continue;
                }
                VariantEntry& ve = fe.by_variant[row.variant_digest];
                if (ve.by_main.size() >= main_cap)
                {
                    continue;
                }
                std::string main_anchor = main_path;
                main_anchor += '\x1f';
                main_anchor += main_hash;
                Entry& e = ve.by_main[main_anchor];
                ve.closure = row.closure_digest;
                e.closure = row.closure_digest;
                e.main_path = main_path;
                e.main_hash = main_hash;
                e.bytes = estimate_bytes(p);
                e.partition = std::move(p);
                e.dirty_persist = false;
                ++a_loaded_rows_;
                payload_bytes_ += e.bytes;
            }
            if (dbg)
            {
                try
                {
                    std::println(
                        std::cerr,
                        "HEADCACHE load dbg rows={} no_path={} decode={}"
                        " closure={} ok={}",
                        dbg_rows,
                        dbg_no_path,
                        dbg_decode,
                        dbg_closure,
                        dbg_ok);
                }
                catch (...)
                {
                }
            }
        }

        /**
         * @brief 提交段持久化：变体失效 + 增量落盘。
         *
         * @param resolve 路径 → 当前 FileId（0 = 无行，跳过落盘）。
         * @param invalidate 内容变脏/抹除的文件 id 集（该文件全部变体行
         *                   删除——头内容变脏则全部变体条目失效）。
         */
        void persist(
            cg::db::GraphStore&                                      store,
            const std::function<cg::db::FileId(const std::string&)>& resolve,
            const std::vector<cg::db::FileId>&                       invalidate)
        {
            std::vector<cg::db::HeadCacheRow> rows;
            {
                std::shared_lock lock{mx_};
                for (const auto& [path, fe] : files_)
                {
                    if (!fe.persist_paths)
                    {
                        continue;
                    }
                    const cg::db::FileId fid = resolve(path);
                    if (fid == 0)
                    {
                        continue;
                    }
                    for (const auto& [variant, ve] : fe.by_variant)
                    {
                        for (const auto& [anchor, entry] : ve.by_main)
                        {
                            if (!entry.dirty_persist)
                            {
                                continue;
                            }
                            const cg::db::FileId main_id =
                                resolve(entry.main_path);
                            if (main_id == 0)
                            {
                                continue;
                            }
                            rows.push_back(
                                cg::db::HeadCacheRow{
                                    .file_id = fid,
                                    .main_id = main_id,
                                    .variant_digest = variant,
                                    .closure_digest = entry.closure,
                                    .payload = detail::encode_partition(
                                        entry.partition,
                                        entry.main_path,
                                        entry.main_hash),
                                    .ver = row_ver_});
                        }
                    }
                }
            }
            a_stored_rows_ = static_cast<std::int64_t>(
                store.head_cache_store(rows, invalidate));
        }

        /** @brief 观测统计快照。 */
        [[nodiscard]]
        auto stats() const -> Stats
        {
            Stats s;
            s.lookups = a_lookups_.load();
            s.hits = a_hits_.load();
            s.misses = a_misses_.load();
            s.records = a_records_.load();
            s.capped_files = a_capped_files_.load();
            s.loaded_rows = a_loaded_rows_.load();
            s.stored_rows = a_stored_rows_.load();
            s.payload_bytes = payload_bytes_.load();
            {
                std::shared_lock lock{mx_};
                s.files = static_cast<std::int64_t>(files_.size());
            }
            return s;
        }

        /** @brief 观测串（启用判定 stderr 行的公共格式）。 */
        [[nodiscard]]
        auto debug_line() const -> std::string
        {
            const Stats s = stats();
            return std::format(
                "HEADCACHE lookups={} hits={} misses={} hit_rate={:.1f}% "
                "records={} files={} capped={} loaded={} stored={} "
                "payload_mb={:.1f}",
                s.lookups,
                s.hits,
                s.misses,
                s.lookups > 0 ? 100.0 * static_cast<double>(s.hits)
                                    / static_cast<double>(s.lookups)
                              : 0.0,
                s.records,
                s.files,
                s.capped_files,
                s.loaded_rows,
                s.stored_rows,
                static_cast<double>(s.payload_bytes) / (1024.0 * 1024.0));
        }

    private:
        struct Entry
        {
            std::string                closure;
            std::string                main_path;
            std::string                main_hash;
            cg::extract::HeadPartition partition;
            std::size_t                bytes = 0;
            bool                       dirty_persist = false;
        };
        struct VariantEntry
        {
            std::string                            closure;
            std::unordered_map<std::string, Entry> by_main;
        };
        struct FileEntry
        {
            std::unordered_map<std::string, VariantEntry> by_variant;
            bool                                          capped = false;
            bool persist_paths = false; // 本运行动过（需要落盘判定）
        };

        [[nodiscard]]
        static auto estimate_bytes(const cg::extract::HeadPartition& p)
            -> std::size_t
        {
            std::size_t n = 64;
            for (const cg::extract::SymbolRec& s : p.symbols)
            {
                n += s.name.size() + s.ns.size() + s.scope.size()
                     + s.parent.size() + s.file.size() + s.unique_key.size()
                     + 96;
                for (const std::string& t : s.param_types)
                {
                    n += t.size() + 8;
                }
                for (const std::string& t : s.template_params)
                {
                    n += t.size() + 8;
                }
            }
            for (const cg::extract::EdgeRec& e : p.edges)
            {
                n += e.src_key.size() + e.dst.key.size()
                     + e.dst.meta.name.size() + e.dst.meta.ns.size()
                     + e.dst.meta.parent.size() + e.site_file.size() + 80;
            }
            for (const cg::extract::IncludeRec& i : p.includes)
            {
                n += i.file.size() + i.target.size() + 24;
            }
            return n;
        }

        std::string                                         epoch_;
        std::int64_t                                        row_ver_ = 0;
        const std::unordered_map<std::string, std::string>* closure_of_ =
            nullptr;
        mutable std::shared_mutex                  mx_;
        std::unordered_map<std::string, FileEntry> files_;
        mutable std::atomic<std::int64_t>          a_lookups_{0};
        mutable std::atomic<std::int64_t>          a_hits_{0};
        mutable std::atomic<std::int64_t>          a_misses_{0};
        mutable std::atomic<std::int64_t>          a_records_{0};
        mutable std::atomic<std::int64_t>          a_capped_files_{0};
        mutable std::atomic<std::int64_t>          a_loaded_rows_{0};
        mutable std::atomic<std::int64_t>          a_stored_rows_{0};
        mutable std::atomic<std::int64_t>          payload_bytes_{0};
    };

} // namespace cg::graph::headcache
