/**
 * @module cg.util.pool
 *
 * @brief 通用工人池：固定规模的 jthread 池 + 互斥游标取活 + 锁内完成
 *        回调，连同工人规模的两个解析单点（自动按核、请求值截断）。
 *
 * 只做并发编排，不含任何业务语义：任务体、取活顺序谓词、进度行内容
 * 全部由消费方注入。线程资源账：每个工人恰一个池内 jthread；任务体
 * 若再派生子进程（每个子进程另起两条排水线程），进程内峰值线程数约
 * 为工人数的三倍——现代 Windows 无压力，账记在此免得后人重算。
 */
export module cg.util.pool;

import std;

export namespace cg::util::pool
{

    /**
     * @brief 自动工人池规模：cores-2，再 clamp 到 [1, 12]。
     *
     * @internal
     * 解析+遍历占真实语料墙钟的约 96% 且跨核扩展，但超过约 12 个工人
     * 无益（AST 内存线性增长而最长 TU 封顶完工时间），并留两个核给
     * 操作系统与写入线程。
     */
    [[nodiscard]]
    inline auto auto_worker_count() -> int
    {
        const unsigned cores = std::thread::hardware_concurrency();
        const int      by_cores = cores > 2 ? static_cast<int>(cores) - 2 : 1;
        return std::clamp(by_cores, 1, 12);
    }

    /**
     * @brief 期望工人数的解析单点：requested>0 用之，否则自动按核；再
     *        截到 [1, job_count]。
     *
     * job_count 是调用方口径下"可供并行的作业数"（解析波传全量作业数、
     * 分层构建传层宽）——作业数天然封顶工人数（层宽 3 时不开 8 路）。
     */
    [[nodiscard]]
    inline auto resolve_worker_count(int requested, std::size_t job_count)
        -> int
    {
        const int want = requested > 0 ? requested : auto_worker_count();
        return static_cast<int>(std::max<std::size_t>(
            1,
            std::min<std::size_t>(static_cast<std::size_t>(want), job_count)));
    }

    /** @brief 一场池运行的完整规约（并发骨架之外的一切由消费方注入）。 */
    struct PoolRun
    {
        /** @brief 任务数（任务下标 0..count-1）；0 时 run 直接返回。 */
        std::size_t count = 0;

        /**
         * @brief 期望工人数；0 = auto_worker_count()。
         *
         * 池照期望开工人数（保底 1），不再按 count 二次截断——分多波
         * 复用同一作业向量的调用方（波次间向量继续增长、每场池只跑尾
         * 部增量）应传 resolve_worker_count(请求值, 全量作业数) 的结
         * 果，使各波次的开工人数与以全量为基数的截断一致；多余的工
         * 人领不到任务立即退出，无副作用。
         */
        int requested = 0;

        /**
         * @brief 取活顺序谓词：真 = 左参数的任务先被领取；空 = 恒等序。
         *
         * 排序不稳定，谓词必须自带全序（典型形态"大小降序、下标升序
         * 平局"），否则两个等价任务的相对顺序随实现摆动。
         */
        std::function<bool(std::size_t, std::size_t)> before = nullptr;

        /**
         * @brief 任务体（参数为任务下标与工人号，工人号从 1 起）。
         *
         * 约定不抛：池不捕获任务异常，异常逃出任务体即在工人线程内终
         * 止整个进程——兜底责任在任务体自身（解析任务的完整重放路径
         * 内部已把异常面收编为失败文本，硬件故障另有守卫机制兜底）。
         */
        std::function<void(std::size_t, int)> task;

        /**
         * @brief 完成回调（池内互斥下调用，done 从 1 起计数）——进度行
         *        的唯一出口。
         *
         * 在任务线程上紧随对应任务调用；进度行所需的任务数据由消费方
         * 经线程局部变量或按工人号独占的槽从任务体递过来。
         */
        std::function<void(std::size_t done)> on_done = nullptr;
    };

    /**
     * @brief 跑一场池：count 个任务按取活顺序分派给工人们，全部 join 后
     *        返回（返回点即天然屏障）。
     *
     * count==0 直接返回（不启线程）；任务数少于工人数时多余工人领不到
     * 任务立即退出，单任务即单工人退化形态。
     */
    inline void run(PoolRun&& spec)
    {
        if (spec.count == 0)
        {
            return;
        }
        const int want =
            spec.requested > 0 ? spec.requested : auto_worker_count();
        const std::size_t worker_count =
            std::max<std::size_t>(1, static_cast<std::size_t>(want));

        std::vector<std::size_t> order(spec.count);
        std::ranges::iota(order, std::size_t{0});
        if (spec.before)
        {
            std::ranges::sort(order, spec.before);
        }

        std::mutex       cursor_mx; // 守护取活游标（与打印锁分离）
        std::size_t      next_job = 0;
        std::mutex       print_mx; // 守护 on_done（进度行的互斥出口）
        std::atomic<int> completed = 0;

        const auto worker = [&](const int wid)
        {
            for (;;)
            {
                std::size_t i;
                {
                    const std::scoped_lock lock{cursor_mx};
                    if (next_job >= order.size())
                    {
                        return;
                    }
                    i = order[next_job++];
                }
                spec.task(i, wid);
                const int n = ++completed;
                if (spec.on_done)
                {
                    const std::scoped_lock lock{print_mx};
                    spec.on_done(static_cast<std::size_t>(n));
                }
            }
        };

        {
            std::vector<std::jthread> workers;
            workers.reserve(worker_count);
            for (std::size_t w = 0; w < worker_count; ++w)
            {
                workers.emplace_back(worker, static_cast<int>(w + 1));
            }
        } // jthread 析构即 join；队列排空后工人退出
    }

} // namespace cg::util::pool
