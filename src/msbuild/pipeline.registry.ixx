module;
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

/**
 * @module cg.msbuild.pipeline.registry
 *
 * @brief 项目注册表域：CL 条目与注册表结构的 JSON
 * 读写、原子落盘、项目刷新目标解析。
 */
export module cg.msbuild.pipeline.registry;

import std;
import cg.util;
import cg.util.cstd;
import cg.msbuild.translate; // ClInvocation / CdbEntry（wrap 日志载荷类型）
import cg.msbuild.wrap;      // detail::env_wide / fs_path_from_utf8
import cg.msbuild.pipeline.configs;

export namespace cg::msbuild
{
    namespace json = cg::util::json;
    /**
     * @brief 注册表 cl_projects 的一条逐工程记录。
     *
     * @internal resolution 与配置级 cl_resolution 同词表（override |
     * project-property | vswhere-fallback）：逐工程的"如实标注"随注册表
     * 持久化，不再只在 verbose 打印。dir/active_cfg/in_build 是披露性
     * 增强：值取自 enumerate_sln_projects 的单点解析（组装器过滤 sln
     * 成员所用同一份事实），只在确定知道时写键——缺席 = 未知，消费侧
     * 绝不把未知当 false。
     */
    struct ProjectClEntry
    {
        std::string project; // 工程名
        std::string cl;      // 绝对 cl.exe，失败时 ""
        std::string
            resolution; // override | project-property | vswhere-fallback | ""
        /** @brief 工程目录（绝对、正斜杠；cl 映射前缀键同源）。"" = 未记录。 */
        std::string dir;
        /** @brief 成员在解决方案配置下的 ActiveCfg（"Debug|x64"）。"" =
         * 未记录。 */
        std::string active_cfg;
        /** @brief Build.0 参与位（该配置下解决方案是否真编它）。
         *         nullopt = 未记录（旧文件键缺席），不是 false。 */
        std::optional<bool> in_build;
    };

    /**
     * @brief `<root>`/.ccg/msbuild.json 的一条逐配置记录。
     *
     * @internal 路径字段尽可能存相对项目根的拼写（此布局下构造即 ASCII
     * 安全）；注册表是信息性的——每个 db 经 meta 自描述。
     */
    struct RegistryEntry
    {
        std::string name;          // "Debug-x64"
        std::string configuration; // "Debug"
        std::string platform;      // "x64"
        std::string solution;      // 项目输入（sln/vcxproj）
        std::string db;            // ".ccg/Debug-x64.db"
        std::string
            compile_commands; // ".ccg/msbuild/Debug-x64/compile_commands.json"
        std::string msbuild;  // 解析到的 MSBuild.exe
        std::string cl;       // 解析到的 cl.exe（主工程；逐工程见 cl_projects）
        /** @brief cl 解析来源：override | project-property | vswhere-fallback
         * （vswhere 命中时工程钉子未被求值，--list/verbose 如实注明）。 */
        std::string cl_resolution;
        /** @brief .sln 逐工程 cl 记录（工程名、cl 绝对路径、解析来源）；
         * 混合工具集各归各。 */
        std::vector<ProjectClEntry> cl_projects;
        /** @brief 本次提取的模式："evaluate"（默认，工程求值 + ccg 侧组装）
         *  | "record"（显式 --record，wrap 捕获的对照真值臂）| "full-build"。
         *  宽松读取：历史条目的任何其它拼写原样保留、绝不拒载。 */
        std::string mode;
        std::string extracted_at; // ISO-8601 UTC，未知时 ""
        int         tus = 0;
        int         failed = 0;
    };

    /**
     * @brief 解析后的注册表。
     *
     * @internal version/generator 由本写出器固定（1 / "msbuild"）；未知的
     * 未来版本仍可解析（字段宽松读取），手改或更新的文件只会降级、绝不
     * 崩溃。
     */
    struct Registry
    {
        int                        version = 1;
        std::string                generator = "msbuild";
        std::string                default_config; // 配置词干，"" = 未记录
        std::vector<RegistryEntry> entries;
    };

    namespace detail
    {

        [[nodiscard]]
        inline auto string_member(
            const json::ValueView& object,
            std::string_view       key) -> std::string
        {
            if (const auto v = object.get(key); v && v->is_string())
            {
                return std::string{*v->as_string()};
            }
            return {};
        }

        [[nodiscard]]
        inline auto int_member(
            const json::ValueView& object,
            std::string_view       key) -> int
        {
            if (const auto v = object.get(key); v && v->is_number())
            {
                if (const auto n = v->as_int())
                {
                    return static_cast<int>(*n);
                }
            }
            return 0;
        }

        /**
         * @brief target 相对 root 的通用分隔符拼写。
         *
         * 二者共享根时给相对拼写，否则给绝对正斜杠拼写。
         *
         * @internal 相对优先是注册表契约（ASCII 安全路径绕开已知的 \uXXXX
         * 解析缺口，并让文件在树移动后保持机器可移植）。
         */
        [[nodiscard]]
        inline auto relative_path_text(
            const std::string& root,
            const std::string& target) -> std::string
        {
            namespace fs = std::filesystem;
            std::error_code ec;
            const fs::path  rel =
                fs::relative(fs::path{target}, fs::path{root}, ec);
            if (ec || rel.empty()
                || (rel.begin() != rel.end() && *rel.begin() == ".."))
            {
                return cg::util::to_forward_slashes(target);
            }
            return rel.generic_string();
        }

    } // namespace detail

    /**
     * @brief RegistryEntry 的全部字段写进 JSON 对象（键序即写出序）。
     *
     * save_registry（注册表文件）与 `ccg msbuild --list`（stdout JSON）共用
     * ——两处的字段集与键名曾各自手写，改一处忘另一处即两份输出分叉。
     * cl_projects 非空时才写键（空数组省略），与既有文件格式逐字节一致。
     */
    inline void registry_entry_to_json(
        const RegistryEntry& e,
        json::Object&        entry)
    {
        entry.set("name", e.name);
        entry.set("configuration", e.configuration);
        entry.set("platform", e.platform);
        entry.set("solution", e.solution);
        entry.set("db", e.db);
        entry.set("compile_commands", e.compile_commands);
        entry.set("msbuild", e.msbuild);
        entry.set("cl", e.cl);
        entry.set("cl_resolution", e.cl_resolution);
        if (!e.cl_projects.empty())
        {
            json::Array cps;
            for (const ProjectClEntry& pc : e.cl_projects)
            {
                json::Object o;
                o.set("project", pc.project);
                o.set("cl", pc.cl);
                o.set("resolution", pc.resolution);
                // 披露键只在确定知道时写出（缺席 = 未知，非 false）。
                if (!pc.dir.empty())
                {
                    o.set("dir", pc.dir);
                }
                if (!pc.active_cfg.empty())
                {
                    o.set("active_cfg", pc.active_cfg);
                }
                if (pc.in_build.has_value())
                {
                    o.set("in_build", *pc.in_build);
                }
                cps.add(std::move(o));
            }
            entry.set("cl_projects", std::move(cps));
        }
        entry.set("mode", e.mode);
        entry.set("extracted_at", e.extracted_at);
        entry.set("tus", e.tus);
        entry.set("failed", e.failed);
    }

    /**
     * @brief JSON 对象宽松读回 RegistryEntry（字段缺席/类型不符取默认值）。
     *
     * load_registry 的逐字段映射单点；宽容语义（string_member/int_member）
     * 使手改或未来版本的注册表只降级、不崩溃。
     */
    [[nodiscard]]
    inline auto registry_entry_from_json(const json::ValueView& entry)
        -> RegistryEntry
    {
        RegistryEntry e;
        e.name = detail::string_member(entry, "name");
        e.configuration = detail::string_member(entry, "configuration");
        e.platform = detail::string_member(entry, "platform");
        e.solution = detail::string_member(entry, "solution");
        e.db = detail::string_member(entry, "db");
        e.compile_commands = detail::string_member(entry, "compile_commands");
        e.msbuild = detail::string_member(entry, "msbuild");
        e.cl = detail::string_member(entry, "cl");
        e.cl_resolution = detail::string_member(entry, "cl_resolution");
        if (const auto cps = entry.get("cl_projects"); cps && cps->is_array())
        {
            const std::size_t n = cps->size();
            for (std::size_t j = 0; j < n; ++j)
            {
                const auto v = cps->at(j);
                if (!v || !v->is_object())
                {
                    continue;
                }
                ProjectClEntry pc;
                pc.project = detail::string_member(*v, "project");
                pc.cl = detail::string_member(*v, "cl");
                pc.resolution = detail::string_member(*v, "resolution");
                pc.dir = detail::string_member(*v, "dir");
                pc.active_cfg = detail::string_member(*v, "active_cfg");
                if (const auto b = v->get("in_build"); b && b->is_bool())
                {
                    pc.in_build = *b->as_bool();
                }
                e.cl_projects.push_back(std::move(pc));
            }
        }
        e.mode = detail::string_member(entry, "mode");
        e.extracted_at = detail::string_member(entry, "extracted_at");
        e.tus = detail::int_member(entry, "tus");
        e.failed = detail::int_member(entry, "failed");
        return e;
    }

    /**
     * @brief 读取 `<root>`/.ccg/msbuild.json。
     *
     * 文件不存在时返回 nullopt（首跑行为）。存在但不是有效 JSON 时抛
     * std::runtime_error——损坏的注册表要响亮，绝不静默忽略。
     */
    [[nodiscard]]
    inline auto load_registry(const std::string& path)
        -> std::optional<Registry>
    {
        const std::optional<std::string> text = detail::read_text_file(path);
        if (!text)
        {
            return std::nullopt;
        }
        Registry registry;
        try
        {
            const json::Document  doc = json::Document::parse(*text);
            const json::ValueView root = doc.root();
            if (!root.is_object())
            {
                throw std::runtime_error("registry root is not an object");
            }
            registry.version = detail::int_member(root, "version");
            if (registry.version == 0)
            {
                registry.version = 1;
            }
            registry.generator = detail::string_member(root, "generator");
            registry.default_config = detail::string_member(root, "default");
            if (const auto configs = root.get("configs");
                configs && configs->is_array())
            {
                const std::size_t count = configs->size();
                for (std::size_t i = 0; i < count; ++i)
                {
                    const auto entry_view = configs->at(i);
                    if (!entry_view || !entry_view->is_object())
                    {
                        continue;
                    }
                    registry.entries.push_back(
                        registry_entry_from_json(*entry_view));
                }
            }
        }
        catch (const std::exception& e)
        {
            throw std::runtime_error(
                "ccg msbuild: registry " + path + " is not valid: " + e.what());
        }
        return registry;
    }

    namespace detail
    {

        /**
         * @brief msbuild 流水线写的一切产物（注册表、compile_commands.json）
         * 的"暂存到 tmp + 原子替换"。
         *
         * @internal 先创建父目录（首跑时 .ccg/msbuild/`<C>`-`<P>`/ 缓存目录尚不
         * 存在）。Windows 用 MoveFileExW，其余平台 rename(2) 语义。
         */
        inline void atomic_write_text(
            const std::string& path,
            const std::string& text,
            std::string_view   what)
        {
            if (const auto parent = fs_path_from_utf8(path).parent_path();
                !parent.empty())
            {
                std::error_code ec;
                std::filesystem::create_directories(
                    parent,
                    ec); // 尽力而为，失败由写报告
            }
            const std::string tmp = path + ".tmp";
            {
                std::ofstream out{
                    fs_path_from_utf8(tmp),
                    std::ios::binary | std::ios::trunc};
                if (!out)
                {
                    throw std::runtime_error(
                        std::format(
                            "ccg msbuild: cannot write {}: {}",
                            what,
                            tmp));
                }
                out.write(
                    text.data(),
                    static_cast<std::streamsize>(text.size()));
            }
#if defined(_WIN32)
            if (MoveFileExW(
                    cg::util::platform::widen_utf8(tmp).c_str(),
                    cg::util::platform::widen_utf8(path).c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
                == 0)
            {
                throw std::runtime_error(
                    std::format(
                        "ccg msbuild: cannot replace {}: {}",
                        what,
                        path));
            }
#else
            std::error_code ec;
            std::filesystem::rename(
                fs_path_from_utf8(tmp),
                fs_path_from_utf8(path),
                ec);
            if (ec)
            {
                throw std::runtime_error(
                    std::format(
                        "ccg msbuild: cannot replace {}: {}: {}",
                        what,
                        path,
                        ec.message()));
            }
#endif
        }

    } // namespace detail

    /**
     * @brief 原子地写出注册表：经 cg.util.json 写出器序列化（UTF-8）。
     *
     * @internal 调用方按注册表契约传入已相对化（relative_path_text）的
     * 路径字段。
     */
    inline void save_registry(const std::string& path, const Registry& registry)
    {
        json::Object root;
        root.set("version", registry.version);
        root.set("generator", registry.generator);
        root.set("default", registry.default_config);
        json::Array configs;
        for (const RegistryEntry& e : registry.entries)
        {
            json::Object entry;
            registry_entry_to_json(e, entry);
            configs.add(std::move(entry));
        }
        root.set("configs", std::move(configs));
        detail::atomic_write_text(
            path,
            json::stringify(std::move(root)) + "\n",
            "registry");
    }

    /**
     * @brief 一次项目文件事件应重采集的目标配置（注册表反查的结果）。
     */
    struct ProjectRefreshTarget
    {
        std::string stem;          // "Debug-x64"（注册表条目名 = 配置目录名）
        std::string configuration; // "Debug"
        std::string platform;      // "x64"
        std::string solution;      // 解决方案绝对路径（正斜杠、规范化）
    };

    /**
     * @brief 把一条待刷新路径映射到注册表里应重采集的配置条目。
     *
     * 联动判定的唯一收口（实时变更队列 peek 出的项目文件事件）：update 持有者
     * 对 peek 出的每个项目文件行调用本函数。注册表条目按本库配置名匹配
     * （name == config_name——update 天然逐库逐配置，多配置各刷各的库，
     * 互不放大）；行匹配规则为"行的路径等于注册表记录的 solution，或行的
     * 路径位于项目根之下"（.props/.targets 挂在根内任意处都能触发；根外
     * 的属性文件不触发——注册表无法证明它们属于本工程）。
     *
     * @param registry 已加载的注册表（load_registry 的结果；调用方负责
     *                "无注册表即不联动"的平台中立门）。
     * @param project_root 项目根（update 侧来自库 meta project_root；根的
     *                拼写以正斜杠/规范化为准）。
     * @param config_name 本库配置名（-o/--config 解析出的配置目录名）。
     * @param line_path 待判定的队列行路径（已规范化）。
     * @return 命中时给出目标配置与 solution 绝对路径；注册表缺席、无同名
     *         条目或行不匹配时为 nullopt（调用方按未跟踪出队处理，避免
     *         队列永久堆积）。
     *
     * @internal 相对/绝对拼写的判定沿用 relative_path_text 的写出契约：
     * 注册表内 solution 字段与根共享根时存相对拼写、否则存绝对拼写；这里
     * 反向还原。比较全部走 ASCII 折叠（Windows 路径大小写不敏感的既定
     * 纪律）。
     */
    [[nodiscard]]
    inline auto project_refresh_target_for(
        const Registry&    registry,
        const std::string& project_root,
        std::string_view   config_name,
        std::string_view   line_path) -> std::optional<ProjectRefreshTarget>
    {
        if (config_name.empty() || project_root.empty() || line_path.empty())
        {
            return std::nullopt;
        }
        const auto entry = std::ranges::find(
            registry.entries,
            config_name,
            &RegistryEntry::name);
        if (entry == registry.entries.end() || entry->solution.empty())
        {
            return std::nullopt;
        }
        // 注册表 solution 字段还原为绝对路径：绝对拼写（盘符或前导 '/'）
        // 原样规范化，相对拼写挂到项目根下。
        const bool stored_absolute = [&]
        {
            const std::string_view s = entry->solution;
            if (s.find(':') != std::string_view::npos && s.size() >= 2)
            {
                return true; // 盘符形态（绝对 Windows 路径）
            }
            return !s.empty() && s.front() == '/';
        }();
        std::string solution = stored_absolute
                                   ? std::string{entry->solution}
                                   : project_root + "/" + entry->solution;
        std::ranges::replace(solution, '\\', '/');
        const std::string solution_fold =
            cg::util::ascii_lower(cg::util::norm_path(solution));
        const std::string line_fold =
            cg::util::ascii_lower(cg::util::norm_path(line_path));
        const std::string root_fold =
            cg::util::ascii_lower(cg::util::norm_path(project_root));
        const bool matches = [&]
        {
            if (line_fold == solution_fold)
            {
                return true;
            }
            const std::string prefix =
                root_fold.back() == '/' ? root_fold : root_fold + "/";
            return line_fold.size() > prefix.size()
                   && line_fold.compare(0, prefix.size(), prefix) == 0;
        }();
        if (!matches)
        {
            return std::nullopt;
        }
        ProjectRefreshTarget target;
        target.stem = entry->name;
        target.configuration = entry->configuration;
        target.platform = entry->platform;
        target.solution = cg::util::norm_path(solution);
        return target;
    }
} // namespace cg::msbuild
