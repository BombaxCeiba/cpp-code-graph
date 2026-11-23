

/**
 * @module cg.clang.pch.model
 *
 * @brief PCH 域的导出类型：PchOptions 与 PchPlanResult。
 */
export module cg.clang.pch.model;

import std;
import cg.util;
import cg.util.cstd;
import cg.clang.capi;
import cg.clang.driver;
import cg.clang.compile_db;
import cg.clang.bmi;

export namespace cg::clang
{

    /**
     * @brief PCH 影子规划旋钮。
     */
    struct PchOptions
    {
        bool        replay_degraded = false; // CCG_PCH_MODE=replay-degraded
        bool        force = false;           // --force/--full：重建影子
        bool        dry_run = false;         // 只计划、不构建
        bool        purge = false;           // 仅全量索引允许清理孤儿
        std::string ccg_config_dir;          // <folder>/<config>/
        std::string compile_db_path; // 全量 cdb（发现 producer 的兜底来源）
        std::string sidecar_path;    // MSBuild pch.json（可选）
    };

    /**
     * @brief 一次规划的结果。
     */
    struct PchPlanResult
    {
        std::vector<CompileCommand> commands; // 已重写；GNU producer 已剔除
        std::vector<std::string>    notes;
        /** @brief 逐 TU 的可读判定（"`<file>`: `<note>`"），供摘要/JSON 消费。
         */
        std::vector<std::string> tu_notes;
        std::size_t              built = 0;
        std::size_t              reused = 0;
        /** @brief 文本回退的消费者数（不含 replay-degraded）。 */
        std::size_t text_fallback = 0;
        /** @brief 显式 opt-in 消费记录 PCH（非 DPR，图不完整）的消费者数。 */
        std::size_t replay_degraded = 0;
        std::size_t unusable = 0;
        std::size_t producers_skipped = 0;
        std::size_t consumers = 0;
        /** @brief 从兄弟配置 donor 复制（未本地重建）的影子数。 */
        std::size_t donated = 0;
        std::string cache_dir;
    };
} // namespace cg::clang
