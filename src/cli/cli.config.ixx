/**
 * @module cg.cli.config
 *
 * @brief `ccg config` 命令（逐库钉住与设置的 get/set/unset 清单）。
 *
 * @internal
 * 曾为实现单元；模块布局扩展改为接口模块——无 BMI 单元触发 C2801 的规避
 * 手法随之退役（cmake/CcgC2801Workaround.cmake 持有该修复）。
 */
export module cg.cli.config;

import std;
import cg.util;
import cg.db;
import cg.clang;
import cg.cli.parse;
import cg.cli.runners;
import cg.cli.layout;

export namespace cg::cli
{

    namespace detail
    {
        namespace fs = std::filesystem;
        namespace json = cg::util::json;

        /**
         * @brief 设置 llvm-path 后打印的忠告文本。
         *
         * 该设置对整库全局：库中每个结果都由先前的引擎产出，须触发重建
         * （已批准的设计：忠告点名 `ccg index --full`）。
         */
        [[nodiscard]]
        auto llvm_path_set_advisory(std::string_view path) -> std::string
        {
            return std::format(
                "ccg: llvm-path set to {}. This setting is global for the "
                "database: every result in it was produced by the previous "
                "engine. Trigger a rebuild - `ccg index --full` now, or the next "
                "plain `ccg index` (it detects the engine change and forces a "
                "full re-index itself).",
                path);
        }

        /**
         * @brief 取消 llvm-path 后打印的忠告文本：引擎解析回落到发现链；
         *        全局设置后果同上面的 set 变体。
         */
        [[nodiscard]]
        auto llvm_path_unset_advisory() -> std::string_view
        {
            return "ccg: llvm-path cleared; engine resolution falls back to "
                   "$CCG_LLVM_ROOT, then PATH discovery. If that resolves a "
                   "different engine than before, every result in this database "
                   "was produced by the previous one - trigger a rebuild - "
                   "`ccg index --full` now, or the next plain `ccg index` (it "
                   "detects the engine change and forces a full re-index "
                   "itself).";
        }

        /**
         * @brief `ccg config` 运行器：get / set / unset。
         *
         * @internal
         * 已知键：llvm-path（引擎世界观）、两个 `ccg update` 协调旋钮
         * （update_lease_ms / update_busy_timeout_ms，排队租约协议）与
         * clang-compile-flags（重放层注入旗标，字符串 值），各自存于自己的 meta
         * 键下，未设置时用 schema 默认值。get 分支
         * 分行显示库内记录引擎（权威——上次索引实际加载的身份）与当前环境
         * 解析结果，两者不一致时醒目警告"裸跑 index 会换引擎触发全量重建"
         * （发现链上 PATH 会盖过库内引擎）。加载为尽力而为：引擎已丢失的库
         * 仍必须可读。协调旋钮为正整数毫秒；并发上限旋钮（index-jobs）为
         * 0（显式回自动）或 1-64。
         */
        /**
         * @brief 配置键的展示值：库内值缺失时以默认值占位显示。
         */
        inline auto knob_value_text(
            cg::db::GraphStore& store,
            const char*         meta_key,
            std::int64_t        fallback) -> std::string
        {
            const auto raw = store.meta(meta_key);
            if (!raw.has_value())
            {
                return std::format("{} (default)", fallback);
            }
            return *raw;
        }

        inline auto run_config(const Invocation& inv_in) -> int
        {
            if (inv_in.positional.empty())
            {
                throw UsageError{
                    "config needs an action: get, set, or unset",
                    usage_for(Invocation::Cmd::Config)};
            }
            Invocation         inv = inv_in;
            const std::string& action = inv.positional.front();
            const auto         usage = usage_for(Invocation::Cmd::Config);
            const bool         is_set = action == "set";
            const bool         is_get = action == "get";
            const bool is_unset = action == "unset" || action == "clear";
            // 用法校验（动作、操作数计数、键名）先于布局发现：坏形状必须是
            // 退出 2 的用法错误，绝不先撞上"库缺失"的运行时错误。
            if (!is_get && !is_set && !is_unset)
            {
                throw UsageError{
                    std::format(
                        "config action must be get, set, or "
                        "unset (got '{}')",
                        action),
                    usage};
            }
            if (is_get && inv.positional.size() > 2)
            {
                throw UsageError{"config get takes at most one key", usage};
            }
            if (is_set && inv.positional.size() != 3)
            {
                throw UsageError{
                    "config set needs a key and a value: config set llvm-path "
                    "<prefix-or-libclang-file>",
                    usage};
            }
            if (is_unset && inv.positional.size() != 2)
            {
                throw UsageError{
                    "config unset needs a key: config unset llvm-path",
                    usage};
            }
            struct KeyInfo
            {
                const char* cli;
                const char* meta;
            };
            static constexpr KeyInfo KEYS[] = {
                {.cli = "llvm-path", .meta = "llvm_path"},
                {.cli = "update-lease-ms", .meta = "update_lease_ms"},
                {.cli = "update-busy-timeout-ms",
                 .meta = "update_busy_timeout_ms"},
                {.cli = "msbuild-refresh-debounce-ms",
                 .meta = "msbuild_refresh_debounce_ms"},
                {.cli = "index-jobs", .meta = "index_jobs"},
                {.cli = "clang-compile-flags", .meta = "clang_compile_flags"},
                {.cli = "index-drain-rounds", .meta = "index_drain_rounds"},
                {.cli = "index-drain-budget-ms",
                 .meta = "index_drain_budget_ms"},
            };
            const auto key_info = [&](const std::string& key) -> const KeyInfo*
            {
                for (const KeyInfo& k : KEYS)
                {
                    if (key == k.cli)
                    {
                        return &k;
                    }
                }
                return nullptr;
            };
            const auto check_key = [&](const std::string& key) -> const KeyInfo&
            {
                if (const KeyInfo* k = key_info(key))
                {
                    return *k;
                }
                throw UsageError{
                    std::format(
                        "unknown config key '{}' (known keys: llvm-path, "
                        "update-lease-ms, update-busy-timeout-ms, "
                        "msbuild-refresh-debounce-ms, index-jobs, "
                        "clang-compile-flags, index-drain-rounds, "
                        "index-drain-budget-ms)",
                        key),
                    usage};
            };
            if (is_set || is_unset)
            {
                (void)check_key(inv.positional[1]);
            }
            if (is_get && inv.positional.size() == 2)
            {
                (void)check_key(inv.positional[1]);
            }
            // set 会创建库：显式 --config 的配置目录尚无 ccg.db 也放行（布局
            // 发现的宽容面只在创建型动作上打开）。
            const LayoutChoice choice =
                layout::resolve_query_layout(inv, is_set);
            inv.db_path = choice.db;
            inv.config_name = choice.config;
            inv.layout_root = choice.root;

            if (action == "get")
            {
                const KeyInfo* only = nullptr;
                if (inv.positional.size() == 2)
                {
                    only = &check_key(inv.positional[1]);
                }
                cg::db::GraphStore store = open_store_read(inv);
                const auto         configured = store.meta("llvm_path");
                // 库内记录引擎（权威）：上次索引运行实际加载并落库的身份
                // （llvm_engine_path / llvm_version）。与"当前环境解析结果"分行
                // 显示——发现链（PATH）会盖过库内旧引擎，两者不一致时裸跑
                // index 会换引擎并触发全量重建，必须醒目警告。
                const auto recorded_path = store.meta("llvm_engine_path");
                const auto recorded_version = store.meta("llvm_version");

                std::string engine_path = "(unresolved)";
                std::string engine_version = "(unresolved)";
                try
                {
                    const std::string resolved =
                        cg::clang::engine::resolve_library(
                            configured.value_or(std::string{}));
                    cg::clang::load_clang_library(resolved);
                    engine_path = resolved;
                    engine_version = cg::clang::libclang_version();
                }
                catch (const std::exception& e)
                {
                    engine_version = std::string{"(load failed) "} + e.what();
                }
                const bool engine_loaded = engine_path != "(unresolved)";
                const bool engine_mismatch =
                    engine_loaded && recorded_path.has_value()
                    && (*recorded_path != engine_path
                        || recorded_version.value_or(std::string{})
                               != engine_version);
                if (engine_mismatch)
                {
                    std::println(
                        std::cerr,
                        "ccg: warning: the recorded engine ({} {}) differs "
                        "from what this environment now resolves ({} {}) - "
                        "a bare `ccg index` will load the new engine and "
                        "force a FULL re-index (engine change = worldview "
                        "change)",
                        *recorded_path,
                        recorded_version.value_or(std::string{"<unknown>"}),
                        engine_path,
                        engine_version);
                }
                const bool want_engine =
                    only == nullptr
                    || only->meta == std::string_view{"llvm_path"};
                const bool want_lease =
                    only == nullptr
                    || only->meta == std::string_view{"update_lease_ms"};
                const bool want_busy =
                    only == nullptr
                    || only->meta == std::string_view{"update_busy_timeout_ms"};
                const bool want_debounce =
                    only == nullptr
                    || only->meta
                           == std::string_view{"msbuild_refresh_debounce_ms"};
                const bool want_jobs =
                    only == nullptr
                    || only->meta == std::string_view{"index_jobs"};
                const bool want_flags =
                    only == nullptr
                    || only->meta == std::string_view{"clang_compile_flags"};
                const bool want_drain_rounds =
                    only == nullptr
                    || only->meta == std::string_view{"index_drain_rounds"};
                const bool want_drain_budget =
                    only == nullptr
                    || only->meta == std::string_view{"index_drain_budget_ms"};
                // 并发上限的展示值：未配置时显示默认 0 并点明其含义（自动按
                // 核），与毫秒旋钮的 "N (default)" 同款、文案按本键语义替换。
                const auto jobs_value = [&]() -> std::string
                {
                    const auto raw = store.meta("index_jobs");
                    if (!raw.has_value())
                    {
                        return std::format(
                            "{} (default, auto by cores)",
                            cg::db::default_index_jobs);
                    }
                    return *raw;
                }();
                if (inv.json)
                {
                    json::Object o;
                    if (want_engine)
                    {
                        o.set(
                            "llvm-path",
                            configured.value_or(std::string{"(default)"}));
                        if (recorded_path.has_value())
                        {
                            o.set("recordedEngine", *recorded_path);
                            o.set(
                                "recordedVersion",
                                recorded_version.value_or(
                                    std::string{"<unknown>"}));
                        }
                        else
                        {
                            o.set("recordedEngine", "(not indexed yet)");
                        }
                        o.set("engine", engine_path);
                        o.set("version", engine_version);
                        o.set("engineMismatch", engine_mismatch);
                    }
                    if (want_lease)
                    {
                        o.set(
                            "update-lease-ms",
                            knob_value_text(
                                store,
                                "update_lease_ms",
                                cg::db::default_update_lease_ms));
                    }
                    if (want_busy)
                    {
                        o.set(
                            "update-busy-timeout-ms",
                            knob_value_text(
                                store,
                                "update_busy_timeout_ms",
                                cg::db::default_update_busy_timeout_ms));
                    }
                    if (want_debounce)
                    {
                        o.set(
                            "msbuild-refresh-debounce-ms",
                            knob_value_text(
                                store,
                                "msbuild_refresh_debounce_ms",
                                cg::db::default_msbuild_refresh_debounce_ms));
                    }
                    if (want_jobs)
                    {
                        o.set("index-jobs", jobs_value);
                    }
                    if (want_flags)
                    {
                        const auto raw = store.meta("clang_compile_flags");
                        o.set(
                            "clang-compile-flags",
                            raw.value_or(std::string{"(none)"}));
                    }
                    if (want_drain_rounds)
                    {
                        o.set(
                            "index-drain-rounds",
                            knob_value_text(
                                store,
                                "index_drain_rounds",
                                std::int64_t{8}));
                    }
                    if (want_drain_budget)
                    {
                        o.set(
                            "index-drain-budget-ms",
                            knob_value_text(
                                store,
                                "index_drain_budget_ms",
                                std::int64_t{300'000}));
                    }
                    std::println("{}", json::stringify(std::move(o)));
                    return 0;
                }
                if (want_engine)
                {
                    std::println(
                        "llvm-path: {}",
                        configured.value_or(
                            std::string{"(unset - default resolution)"}));
                    std::println(
                        "recorded engine: {}",
                        recorded_path.has_value()
                            ? std::format(
                                  "{} ({})",
                                  *recorded_path,
                                  recorded_version.value_or(
                                      std::string{"<unknown>"}))
                            : std::string{"(not indexed yet)"});
                    std::println("engine:    {}", engine_path);
                    std::println("version:   {}", engine_version);
                }
                if (want_lease)
                {
                    std::println(
                        "update-lease-ms: {}",
                        knob_value_text(
                            store,
                            "update_lease_ms",
                            cg::db::default_update_lease_ms));
                }
                if (want_busy)
                {
                    std::println(
                        "update-busy-timeout-ms: {}",
                        knob_value_text(
                            store,
                            "update_busy_timeout_ms",
                            cg::db::default_update_busy_timeout_ms));
                }
                if (want_debounce)
                {
                    std::println(
                        "msbuild-refresh-debounce-ms: {}",
                        knob_value_text(
                            store,
                            "msbuild_refresh_debounce_ms",
                            cg::db::default_msbuild_refresh_debounce_ms));
                }
                if (want_jobs)
                {
                    std::println("index-jobs: {}", jobs_value);
                }
                if (want_flags)
                {
                    const auto raw = store.meta("clang_compile_flags");
                    std::println(
                        "clang-compile-flags: {}",
                        raw.value_or(std::string{"(none)"}));
                }
                if (want_drain_rounds)
                {
                    std::println(
                        "index-drain-rounds: {}",
                        knob_value_text(
                            store,
                            "index_drain_rounds",
                            std::int64_t{8}));
                }
                if (want_drain_budget)
                {
                    std::println(
                        "index-drain-budget-ms: {}",
                        knob_value_text(
                            store,
                            "index_drain_budget_ms",
                            std::int64_t{300'000}));
                }
                return 0;
            }

            if (action == "set")
            {
                const std::string& key = inv.positional[1];
                const std::string& value = inv.positional[2];
                const KeyInfo&     k = check_key(key);
                if (key == "llvm-path")
                {
                    std::error_code ec;
                    const fs::path  abs = fs::absolute(fs::path{value}, ec);
                    if (ec || !fs::exists(abs, ec))
                    {
                        throw std::runtime_error(
                            std::format(
                                "llvm-path '{}' does not exist (expected an LLVM prefix "
                                "or the libclang library file)",
                                value));
                    }
                    const std::string norm = cg::util::norm_path(
                        abs.lexically_normal().generic_string());
                    cg::db::GraphStore store = open_store_create(inv);
                    if (const auto old = store.meta("llvm_path");
                        old.has_value() && *old == norm)
                    {
                        std::println(
                            std::cerr,
                            "ccg: llvm-path already {} (no change)",
                            norm);
                        return 0;
                    }
                    store.set_meta("llvm_path", norm);
                    std::println(std::cerr, "{}", llvm_path_set_advisory(norm));
                    if (inv.json)
                    {
                        json::Object o;
                        o.set("llvm-path", norm);
                        o.set("advisory", llvm_path_set_advisory(norm));
                        std::println("{}", json::stringify(std::move(o)));
                    }
                    return 0;
                }

                if (key == "clang-compile-flags")
                {
                    // 字符串键：按原样存（空格切分发生在消费侧的重放层注入，
                    // 与 --clang-compile-flags 的引号感知切词同一实现）。空值
                    // 没有含义——清空走 unset。
                    if (value.empty())
                    {
                        throw UsageError{
                            "clang-compile-flags needs the flags text (e.g. "
                            "`ccg config set clang-compile-flags \"-DFOO "
                            "-isystem <vendor>/include\"`; use `config unset "
                            "clang-compile-flags` to clear)",
                            usage};
                    }
                    cg::db::GraphStore store = open_store_create(inv);
                    if (const auto old = store.meta("clang_compile_flags");
                        old.has_value() && *old == value)
                    {
                        std::println(
                            std::cerr,
                            "ccg: clang-compile-flags already set to "
                            "'{}' (no change)",
                            value);
                        return 0;
                    }
                    store.set_meta("clang_compile_flags", value);
                    std::println(
                        std::cerr,
                        "ccg: clang-compile-flags = '{}'\n"
                        "ccg: every replayed argv changes - the next "
                        "`ccg index`/`ccg msbuild` re-parses the "
                        "database (argv digest mismatch)",
                        value);
                    if (inv.json)
                    {
                        json::Object o;
                        o.set("clang-compile-flags", value);
                        std::println("{}", json::stringify(std::move(o)));
                    }
                    return 0;
                }

                if (key == "index-jobs")
                {
                    // 并发上限键：与毫秒旋钮同一严格解析纪律（全串纯数字、荒谬
                    // 大数拒绝、错误文案带原始输入——绝不静默吞尾），但语义分
                    // 支：0 合法（显式回自动），1-64 生效；毫秒文案不适用。累
                    // 加中超 64 提前钳制（与 cli.parse 的 --jobs 解析体同款）。
                    if (value.empty()
                        || !std::ranges::all_of(
                            value,
                            [](char c) { return c >= '0' && c <= '9'; }))
                    {
                        throw std::runtime_error(
                            std::format(
                                "index-jobs expects 0 (auto) or an integer between "
                                "1 and 64 (got '{}')",
                                value));
                    }
                    unsigned long long parsed = 0;
                    for (const char c : value)
                    {
                        parsed = parsed * 10
                                 + static_cast<unsigned long long>(c - '0');
                        if (parsed > 64ull)
                        {
                            break;
                        }
                    }
                    if (parsed > 64ull)
                    {
                        throw std::runtime_error(
                            std::format(
                                "index-jobs expects 0 (auto) or an integer between "
                                "1 and 64 (got '{}')",
                                value));
                    }
                    const std::string  norm = std::to_string(parsed);
                    cg::db::GraphStore store = open_store_create(inv);
                    if (const auto old = store.meta(k.meta);
                        old.has_value() && *old == norm)
                    {
                        std::println(
                            std::cerr,
                            "ccg: index-jobs already {} (no change)",
                            norm);
                        return 0;
                    }
                    store.set_meta(k.meta, norm);
                    std::println(
                        std::cerr,
                        "ccg: index-jobs = {}{}",
                        norm,
                        parsed == 0 ? std::string{" (auto by cores)"}
                                    : std::format(
                                          " (workers for the module-BMI "
                                          "build and the parse pool of "
                                          "index/update/msbuild)"));
                    if (inv.json)
                    {
                        json::Object o;
                        o.set("index-jobs", norm);
                        std::println("{}", json::stringify(std::move(o)));
                    }
                    return 0;
                }

                // 数值键（毫秒旋钮）是用户手敲输入，与命令行数字选项
                // （cli.parse 的 --jobs/--top 族）同一严格语义：全串必须纯
                // 数字——正号、前导空白、小数点、尾垃圾（"5000abc"）、科学
                // 计数（"1e5"）、千分位（"12,000"）一律报错且绝不写库（此前
                // 宽容 atoll 会静默吞尾把 "1e5" 存成 1）。溢出串与零同理；
                // 错误文案带原始输入值，方便用户照着改。
                if (value.empty()
                    || !std::ranges::all_of(
                        value,
                        [](char c) { return c >= '0' && c <= '9'; }))
                {
                    throw std::runtime_error(
                        std::format(
                            "{} expects a positive number of milliseconds (got '{}')",
                            key,
                            value));
                }
                std::int64_t parsed = 0;
                bool         overflow = false;
                for (const char c : value)
                {
                    const int digit = c - '0';
                    if (parsed
                        > (std::numeric_limits<std::int64_t>::max() - digit)
                              / 10)
                    {
                        overflow = true;
                        break;
                    }
                    parsed = parsed * 10 + digit;
                }
                if (overflow || parsed <= 0)
                {
                    throw std::runtime_error(
                        std::format(
                            "{} expects a positive number of milliseconds (got '{}')",
                            key,
                            value));
                }
                cg::db::GraphStore store = open_store_create(inv);
                store.set_meta(k.meta, std::to_string(parsed));
                std::println(std::cerr, "ccg: {} = {} ms", key, parsed);
                if (inv.json)
                {
                    json::Object o;
                    o.set(key, parsed);
                    std::println("{}", json::stringify(std::move(o)));
                }
                return 0;
            }

            if (action == "unset" || action == "clear")
            {
                const KeyInfo&     k = check_key(inv.positional[1]);
                cg::db::GraphStore store = open_store_read(inv);
                if (!store.meta(k.meta).has_value())
                {
                    std::println(
                        std::cerr,
                        "ccg: no {} configured (already on the default)",
                        k.cli);
                    return 0;
                }
                store.erase_meta(k.meta);
                if (k.cli == std::string_view{"llvm-path"})
                {
                    std::println(std::cerr, "{}", llvm_path_unset_advisory());
                    if (inv.json)
                    {
                        json::Object o;
                        o.set("llvm-path", "(default)");
                        o.set("advisory", llvm_path_unset_advisory());
                        std::println("{}", json::stringify(std::move(o)));
                    }
                }
                else
                {
                    std::println(std::cerr, "ccg: {} back to default", k.cli);
                    if (inv.json)
                    {
                        json::Object o;
                        o.set(k.cli, "(default)");
                        std::println("{}", json::stringify(std::move(o)));
                    }
                }
                return 0;
            }
            // 前置用法校验已保证动作必为 get/set/unset/clear 之一，三支全覆盖。
            std::unreachable();
        }
    } // namespace detail

} // namespace cg::cli
