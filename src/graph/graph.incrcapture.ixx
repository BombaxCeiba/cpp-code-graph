module;
#include "../db/db.tables.h"

/**
 * @module cg.graph.incrcapture
 *
 * @brief 被删行下游的旧边见证捕获域（值语义域对象）。
 *
 * @internal db.tables.h 置于全局模块片段：本单元导入 cg.db，MSVC 要求
 *           sqlpp23 表模型定义在此可见（同 cg.graph.build 的纪律）。
 *           cg.graph.build 以值成员嵌入并再导出本单元。
 */

export module cg.graph.incrcapture;

import std;
import cg.db;

export namespace cg::graph
{
    namespace detail
    {
        /**
         * @brief 增量捕获：本批被 purge/erase 文件的旧边见证（评审
         *        红线"被删行下游"）。
         *
         * @internal store 阶段逐文件累积；非延迟批直接交给 dispatch_commit，
         *           延迟批落 meta（dispatch_prev_*）给收敛批。服务依赖
         *           （store 与 dispatch 启用判定）经参数传入，域对象不
         *           持有服务引用。
         */
        struct IncrCapture
        {
            IncrCapture() = default;
            IncrCapture(const IncrCapture&) = delete;
            IncrCapture& operator=(const IncrCapture&) = delete;
            IncrCapture(IncrCapture&&) = delete;
            IncrCapture& operator=(IncrCapture&&) = delete;

            std::vector<std::pair<std::string, std::string>> incr_prev_slots_;
            std::vector<std::string>                         incr_prev_callees_;
            std::vector<std::string>                         incr_doomed_keys_;
            std::vector<std::string>                         incr_doomed_names_;

            /** @brief purge/erase 前捕获该文件的派生数据旧边见证（只读不删）。
             */
            void capture_analysis_purge_(
                cg::db::GraphStore&            store,
                bool                           dispatch_enabled,
                cg::db::FileId                 fid,
                std::span<const cg::db::StrId> fresh)
            {
                if (!dispatch_enabled)
                {
                    return;
                }
                const cg::db::PurgeCapture cap =
                    store.capture_analysis_purge(fid, fresh);
                incr_prev_slots_.insert(
                    incr_prev_slots_.end(),
                    cap.slots.begin(),
                    cap.slots.end());
                incr_prev_callees_.insert(
                    incr_prev_callees_.end(),
                    cap.callee_keys.begin(),
                    cap.callee_keys.end());
                incr_doomed_keys_.insert(
                    incr_doomed_keys_.end(),
                    cap.doomed_keys.begin(),
                    cap.doomed_keys.end());
                incr_doomed_names_.insert(
                    incr_doomed_names_.end(),
                    cap.doomed_names.begin(),
                    cap.doomed_names.end());
            }
        };
    } // namespace detail
} // namespace cg::graph
