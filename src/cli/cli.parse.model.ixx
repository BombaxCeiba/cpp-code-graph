/**
 * @module cg.cli.parse.model
 *
 * @brief
 * 命令行域的导出类型：UsageError、Invocation（命令与全部旗标载荷）、ChangedFile。
 */
export module cg.cli.parse.model;

import std;
import cg.util;
import cg.util.cstd;
import cg.db;
import cg.graph;

export namespace cg::cli
{

    /**
     * @brief 用法错误：携带原因与随后打印的用法块（用法
     *        错误走 stderr、退出 2）。
     */
    class UsageError : public std::runtime_error
    {
    public:
        UsageError(std::string_view why, std::string_view usage)
            : std::runtime_error(std::string{why}), usage_(usage)
        {
        }
        [[nodiscard]]
        auto usage() const noexcept -> const std::string&
        {
            return usage_;
        }

    private:
        std::string usage_;
    };

    /** @brief 校验完毕后命令运行器所需的一切。 */
    struct Invocation
    {
        enum class Cmd : std::uint8_t
        {
            Help,
            Version,
            Index,
            Update,
            Msbuild,
            Config,
            Hook,
            Query,
            Context,
            Callers,
            Callees,
            Impact,
            Trace,
            Stats,
            Changes,
            Check,
            Graph,
            Members,
        };
        Cmd                      cmd = Cmd::Help;
        std::vector<std::string> positional; // index 根/符号名/changes 变更文件
        /**
         * @brief 产出文件夹（-o、$CCG_DB 或 ./.ccg），解析前的拼写。
         *
         * 布局规则：-o 只许指定文件夹；每配置一个子目录
         * `<folder>`/`<config>`/，ccg.db 与派生物同居其中。db_path 由布局解析
         * （cg.cli.layout）在分派期填入最终的库文件路径。
         */
        std::string output_dir;
        std::string config_name; // --config（"" = 发现规则自动选）
        std::string db_path;     // 解析后的库文件（<folder>/<config>/ccg.db）
        std::string layout_root; // 解析后的产出文件夹（hook 子进程转发用）
        std::string compile_db;  // index：显式 --compile-commands
        std::string llvm_root;   // index：显式 --llvm-root 前缀
        std::string from_file;   // changes：--from-file 清单路径
        /**
         * index/update/msbuild：--clang-compile-flags（可重复）累计的注入
         * 旗标 token（按出现序拼接，后写覆盖先写——驱动器同名旗标后者胜）。
         *
         * 重放层把它们追加到每条重放 argv 的末尾；非空时压制库配置键
         * clang-compile-flags（命令行本次 > 配置键 > 无）。
         */
        std::vector<std::string> clang_compile_flags;
        bool                     json = false;
        bool                     verbose = false;
        bool force = false;        // index / hook uninstall（--force）
        bool full = false;         // 仅 index（--full）
        bool dry_run = false;      // 仅 index
        bool paranoid = false;     // 仅 index
        bool cycles = false;       // check：--cycles 模式
        bool hook_user = false;    // hook：目标 ~/.claude/settings.json
        bool hook_project = false; // hook：目标 ./.claude/settings.json
        /**
         * index/update/msbuild：--jobs 并发上限旗标（1-64）。
         *
         * 0 = 未传旗标：生效值经 cg::cli::detail::resolve_jobs 合成——
         * 旗标 > 库配置键 index-jobs（`ccg config set`，0 = 显式回自动）
         * > 自动按核（clamp(cores-2, 1, 12)）。生效值统管两段工人池
         * （BMI 自建段与 TU 解析段）。
         */
        int                  jobs = 0;
        int                  depth = 1;     // 逐命令默认值在此施加
        int                  list_cap = 20; // check：列出的环簇上限
        cg::graph::Direction direction = cg::graph::Direction::Up;
        bool                 no_merge = false; // 符号命令：关闭覆盖族合并
        /**
         * @brief 查询侧的动态派发证据策略。
         *
         * @internal 默认 Precise（B 精确模式）：confidence >= 0.7（1.0/0.7）
         * 的 virtual_dispatch 证据替换调用点上的家族过近似；0.4 弱推断与
         * 0.0 标记不算证据，按整个 override 家族估算。
         * `--no-dispatch` 回到与引入 dispatch 之前逐字节一致的纯静态视图，
         * `--dispatch-fallback` 额外显示 0.0 家族标记。
         */
        cg::graph::DispatchMode dispatch_mode =
            cg::graph::DispatchMode::Precise;
        std::optional<cg::db::SymbolKind> kind;
        std::optional<std::string>        file_substring;
        /**
         * @brief callers/callees/impact 的 --top N：每个结果区至多列出 N 条
         *        （默认无限制；截断处以 "… and N more" 行标明，truncated 实
         *        算为 true，计数仍覆盖全部找到的条目）。
         */
        std::optional<int> top;
        /**
         * @brief query 专属：--implements 模式（positional 当接口/基类名，
         *        列派生树；类型继承=extends 边，与虚函数 override 家族无关）。
         */
        bool implements = false;
        /**
         * @brief --exclude-glob G（可重复）：查询结果端的路径排除——定义
         *        路径命中任一 glob 的条目从结果中剔除（索引侧全量收录不动，
         *        这正是"全量收录 + 用户主动过滤"的配套面）。
         */
        std::vector<std::string> exclude_globs;
        /** @brief 仅 msbuild（MSBuild 管线规格）：`ccg msbuild` 的选项集。 */
        std::string msbuild_config;      // --config C（""=注册表默认/Debug）
        std::string msbuild_platform;    // --platform P（""=x64）
        std::string msbuild_args;        // --msbuild-args S，逐字 MSBuild 尾参
        std::string cl_path;             // --cl PATH，跳过 cl 发现
        std::string msbuild_path;        // --msbuild PATH，跳过 vswhere
        bool        msbuild_all = false; // --all：全部声明的 C|P 对
        bool        msbuild_export_only = false; // --export-only：cdb 后即停
        bool        msbuild_list = false;        // --list：打印注册表并退出
        /** @brief --record：显式走记录管线（wrap 捕获的对照真值臂——
         * 测试/验证基础设施身份的入口；与 --full-build 互斥）。默认
         * （无旗标）= 求值通道：对解决方案做工程求值、ccg 侧组装 argv，
         * 不跑构建、不桩化工具。 */
        bool msbuild_record = false;
        /** @brief --full-build：真编（构建图真跑、cl 真编译，金标准对账
         * 逃生口；与 --record 互斥）。 */
        bool msbuild_full_build = false;
        /** @brief --allow-failure：接受 MSBuild 非零退出码仍落盘捕获数据
         *  （部署断言/测试兜底检查对沙盒产物不友好的场景；仅对跑构建的
         * 通道有意义——--record/--full-build，默认求值通道无 MSBuild
         * 退出码，解析层组合拒绝）。 */
        bool msbuild_allow_failure = false;
        bool db_explicit = false;     // 见过 -o
        bool config_explicit = false; // 见过 --config（index 必给）
        bool drain = false; // update：--drain——无自有文件也处理待办队列
        /**
         * @brief index/msbuild 的提交段租约排队（事务内心跳方案）。
         *
         * @internal 默认排队等待（上界 wait_limit_ms）；--wait=&lt;ms&gt; 收紧
         * 上界；--no-wait 快速失败。解析层只记偏好，协调发生在 Indexer 的
         * 提交段（IndexOptions::Coordination）。
         */
        bool         no_wait = false;
        std::int64_t wait_limit_ms = 1'800'000; // 默认 30 分钟排队上界
        bool wait_cap_seen = false; // 见过 --wait=<ms>（与 --no-wait 互斥判定）
        std::string graph_symbol;   // graph：--symbol 入口符号名
        bool        graph_modules = false; // graph：--modules 模式
        std::string graph_format = "dot";  // graph：--format dot|json
        std::string output_file;           // graph：--output 载荷落盘路径
        int         min_weight = 1;        // graph --modules：聚合边权重下限
    };

    /**
     * @brief ccg changes 的一个变更文件条目，已对数据库解析。
     *
     * 携带喂入时的拼写、解析到时的库路径文本、文件是否已从盘上消失
     * （删除——其符号在库中存活到下次索引运行）、以及该文件定义的符号数。
     */
    struct ChangedFile
    {
        std::string  input;           // 喂入时的精确拼写（规范化前）
        std::string  path;            // 库路径文本；未知时 ""
        bool         known = false;   // 已解析到 files 行
        bool         deleted = false; // 行已知、文件盘上缺失
        std::int64_t symbols = 0;
    };
} // namespace cg::cli
