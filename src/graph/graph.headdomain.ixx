/**
 * @module cg.graph.headdomain
 *
 * @brief 头级缓存域（值语义域对象）：缓存实例、闭包摘要快照、主文件
 *        锚哈希面、包含差量放行集与指令摘要写回载荷。
 *
 * @internal cg.graph.build 以值成员嵌入并再导出本单元。headcache_ 以
 *           optional 真值嵌入（空 = 关闭）；HeadCache 构造注入的闭包
 *           快照指针指向同域兄弟成员，地址稳定性随 Indexer 从不搬移
 *           成立，域对象因此删除拷贝与移动。
 */

export module cg.graph.headdomain;

import std;
import cg.util;
import cg.db;
import cg.util.instrdigest;
import cg.graph.headcache;

export namespace cg::graph
{
    namespace detail
    {
        struct HeadCacheDomain
        {
            HeadCacheDomain() = default;
            HeadCacheDomain(const HeadCacheDomain&) = delete;
            HeadCacheDomain& operator=(const HeadCacheDomain&) = delete;
            HeadCacheDomain(HeadCacheDomain&&) = delete;
            HeadCacheDomain& operator=(HeadCacheDomain&&) = delete;

            /** @brief 本运行的缓存实例；空 = 关闭（CCG_HEADCACHE=0 / fb 波式
             *        分解在开 / 内存库外的极端形态按启用判定构造）。 */
            std::optional<headcache::HeadCache> headcache_;
            /** @brief 归属文件 → 闭包摘要（运行期只读快照，缓存键输入）。 */
            std::unordered_map<std::string, std::string> head_closure_of_;
            /** @brief 主文件路径 → 内容哈希（缓存主文件锚输入；无哈希者
             *         不参与缓存——桥不建、记录跳过）。 */
            std::unordered_map<std::string, std::string> hc_main_hash_of_;
            /** @brief 观察者增量合并（包含差量 add 方向）的站点文件集。 */
            std::unordered_set<cg::db::FileId> bump_feed_;
            /** @brief 指令摘要的写回载荷（提交后落 file_instr_digest）。 */
            std::vector<cg::db::FileInstrDigestRow> c1_digest_writes_;

            /**
             * @brief 指令摘要的单文件扫描与基线行落册。
             *
             * @return 文件可读时的摘要；不可读返回空值且不落行。
             */
            auto scan_instr_digest_(
                cg::db::FileId     fid,
                const std::string& path,
                bool               is_main,
                bool debug) -> std::optional<cg::util::instrdigest::InstrDigest>
            {
                std::optional<cg::util::instrdigest::InstrDigest> fresh =
                    cg::util::instrdigest::digest_of_file(path);
                if (!fresh.has_value())
                {
                    return std::nullopt;
                }
                c1_digest_writes_.push_back(
                    cg::db::FileInstrDigestRow{
                        .file_id = fid,
                        .d_narrow = fresh->narrow,
                        .d_all = fresh->all,
                        .d_include = fresh->include_only,
                        .n_narrow = fresh->n_narrow,
                        .n_all = fresh->n_all,
                        .n_include = fresh->n_include,
                        .ver = cg::db::head_cache_side_ver});
                if (debug)
                {
                    try
                    {
                        std::println(
                            std::cerr,
                            "C1 base file='{}' main={} n_narrow={}",
                            path,
                            is_main ? 1 : 0,
                            fresh->n_narrow);
                    }
                    catch (...)
                    {
                    }
                }
                return fresh;
            }
        };
    } // namespace detail
} // namespace cg::graph
