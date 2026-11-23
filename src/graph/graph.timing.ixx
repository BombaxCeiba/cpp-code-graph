/**
 * @module cg.graph.timing
 *
 * @brief 索引器的逐阶段墙钟累计账本（值语义域对象）。
 *
 * @internal 纯观测面：没有任何代码读这些值做决策，唯一读者是落盘与
 *           -v 汇总打印。cg.graph.build 以值成员嵌入并再导出本单元。
 */

export module cg.graph.timing;

import std;

export namespace cg::graph
{
    namespace detail
    {
        struct Timing
        {
            Timing() = default;
            Timing(const Timing&) = delete;
            Timing& operator=(const Timing&) = delete;
            Timing(Timing&&) = delete;
            Timing& operator=(Timing&&) = delete;

            /** @brief 支撑 -v 计时行的逐阶段墙钟累计（微秒）。 */
            double t_parse_us_ = 0;
            double t_visit_us_ = 0;
            /** @brief analysis.collect 走查累计（微秒；性能观测面）。 */
            double t_collect_us_ = 0;
            double t_store_a_us_ = 0;
            double t_purge_us_ = 0;
            double t_store_b_us_ = 0;
            double t_commit_us_ = 0;
            /** @brief 可达性清扫段墙钟累计（微秒；快路径跳过时近似为零）。 */
            double t_sweep_us_ = 0;
            /** @brief 贡献记账三段墙钟累计（微秒；旧票读/差集撤票+归零仲
             *         裁/新票落库——跨 storeB 尾与 commit 头的独立成本观测
             *         面，值随 meta 落库）。 */
            double t_contrib_read_us_ = 0;
            double t_contrib_diff_us_ = 0;
            double t_contrib_insert_us_ = 0;
        };
    } // namespace detail
} // namespace cg::graph
