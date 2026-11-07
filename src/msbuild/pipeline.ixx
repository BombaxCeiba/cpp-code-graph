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
// read_wrap_log 经 sqlpp23 读 capture.db：表模型必须以经典头形态在本
// 单元全局片段可见（MSVC 上定义进命名模块再跨模块导入的表模板会让
// IFC 读取器崩溃），故 include capture.tables.h。残留的 sqlite3 C API
// 面是白名单接缝：SQLITE_OPEN_* 旗标宏（宏不跨模块边界）与
// sqlite3_busy_timeout（连接器无超时旋钮），include 面由 cg_core 的
// PUBLIC cg::sqlite3 链接传递；extern "C" 声明重复无 ODR 风险。
#include <sqlite3.h>

#include "capture.tables.h"

/**
 * @module cg.msbuild.pipeline
 *
 * @brief `ccg msbuild` 的发现、编排与注册表。
 *
 * 定位 MSBuild（vswhere、环境/选项覆盖），定位真实 cl（MSBuild
 * -getProperty:VCToolsInstallDir 加"末条路径行"规则、vswhere 兜底、
 * --cl 覆盖；横幅版本供 -fms-compatibility-version），运行喂 wrap
 * 模式的被包装 -t:Rebuild（src/msbuild/wrap.ixx），从 .sln/.vcxproj
 * 输入枚举 Configuration|Platform 对，并把逐配置注册表持久化到
 * `<root>`/.ccg/msbuild.json（ASCII 安全相对路径、原子替换）。
 *
 * @internal
 * 全部是文本 + 进程管道：图引擎零改动（既定决策——一个配置一个 db
 * 文件，互不污染）。全局模块片段说明：发现/编排半边的 Win32 头——
 * 可执行自身的定位（GetModuleFileNameW）与文件属性探测；子进程派生
 * （vswhere/MSBuild/cl 讯问、被包装 Rebuild）已收敛到 cg.util.platform
 * 的捕获原语 run_child；纯文本半边（配置枚举、注册表、路径拼接、
 * 输出解析）到处可编译且有单元测试。
 */
export module cg.msbuild.pipeline;

import std;
import cg.util;
import cg.util.cstd;
import cg.msbuild.translate; // ClInvocation / CdbEntry（wrap 日志载荷类型）
import cg.msbuild.wrap;      // detail::env_wide / fs_path_from_utf8
import sqlpp23.core;
import sqlpp23.sqlite3;

export namespace cg::msbuild
{

    namespace json = cg::util::json;

    /** @brief 输入项目的一个 MSBuild Configuration|Platform 轴。 */
    struct ConfigPlatform
    {
        std::string configuration; // "Debug"
        std::string platform;      // "x64"、"Win32"、"ARM64"……
    };

    /**
     * @brief 一个配置的注册表文件词干（"Debug|x64" -> "Debug-x64"）。
     *
     * @internal '|' 在 Windows 文件名中非法，注册表布局以 '-' 代替。
     */
    [[nodiscard]]
    inline auto config_stem(const ConfigPlatform& cp) -> std::string
    {
        return std::format("{}-{}", cp.configuration, cp.platform);
    }

    namespace detail
    {

        /**
         * @brief 整文件读入（二进制口径）：util 单点转发的既有名字别名。
         *
         * @internal 调用面（enumerate_configs 等）历史上就叫这个名字，收编
         * util::read_text_file 后保留薄别名，避免四五个调用点改拼写。
         */
        [[nodiscard]]
        inline auto read_text_file(const std::string& path)
            -> std::optional<std::string>
        {
            return cg::util::read_text_file(path);
        }

        /**
         * @brief ASCII 大小写不敏感相等：util 单点的既有名字别名。
         *
         * @internal 本文件十余个调用点沿用 detail::ascii_iequals 拼写，实现
         * 收编进 cg::util::ascii_iequals（折叠口径与注释见彼处）。
         */
        [[nodiscard]]
        inline auto ascii_iequals(std::string_view a, std::string_view b)
            -> bool
        {
            return cg::util::ascii_iequals(a, b);
        }

        /** @brief "Debug|x64" -> {"Debug","x64"}；无可用分隔符时 nullopt。 */
        [[nodiscard]]
        inline auto split_config_platform(std::string_view value)
            -> std::optional<ConfigPlatform>
        {
            const std::size_t bar = value.find('|');
            if (bar == std::string_view::npos || bar == 0
                || bar + 1 >= value.size())
            {
                return std::nullopt;
            }
            return ConfigPlatform{
                .configuration = std::string{value.substr(0, bar)},
                .platform = std::string{value.substr(bar + 1)}};
        }

    } // namespace detail

    /**
     * @brief 输入声明的 Configuration|Platform 对（夹具验证过）。
     *
     * .sln 取 GlobalSection(SolutionConfigurationPlatforms) 条目；.vcxproj
     * 取 ItemGroup 的 ProjectConfiguration Include="C|P" 属性。面向行的
     * 文本扫描（确定性、不求值）。输入不可读或无可识别声明时为空。
     *
     * @internal 解决方案配置行形如 `Debug|x64 = Debug|x64`，取等号左侧的
     * 轴名；项目配置行形如 `<ProjectConfiguration Include="Debug|x64">`。
     */
    [[nodiscard]]
    inline auto enumerate_configs(const std::string& input)
        -> std::vector<ConfigPlatform>
    {
        std::vector<ConfigPlatform>      out;
        const std::optional<std::string> text = detail::read_text_file(input);
        if (!text)
        {
            return out;
        }
        const std::string ext =
            std::filesystem::path{input}.extension().string();
        const bool is_sln = detail::ascii_iequals(ext, ".sln");
        const bool is_vcproj = detail::ascii_iequals(ext, ".vcxproj");
        if (!is_sln && !is_vcproj)
        {
            return out;
        }

        const auto push_unique = [&out](const std::string& pair)
        {
            if (const std::optional<ConfigPlatform> cp =
                    detail::split_config_platform(pair))
            {
                for (const ConfigPlatform& seen : out)
                {
                    if (seen.configuration == cp->configuration
                        && seen.platform == cp->platform)
                    {
                        return;
                    }
                }
                out.push_back(*cp);
            }
        };

        std::istringstream lines{*text};
        std::string        line;
        bool               in_solution_section = false;
        while (std::getline(lines, line))
        {
            cg::util::strip_cr(line);
            if (is_sln)
            {
                if (!in_solution_section)
                {
                    if (line.find(
                            "GlobalSection(SolutionConfigurationPlatforms)")
                        != std::string::npos)
                    {
                        in_solution_section = true;
                    }
                    continue;
                }
                if (line.find("EndGlobalSection") != std::string::npos)
                {
                    in_solution_section = false;
                    continue;
                }
                const std::size_t eq = line.find('=');
                if (eq != std::string::npos)
                {
                    push_unique(cg::util::trim(line.substr(0, eq)));
                }
            }
            else
            {
                constexpr std::string_view marker{
                    "ProjectConfiguration Include=\""};
                const std::size_t at = line.find(marker);
                if (at == std::string::npos)
                {
                    continue;
                }
                const std::size_t begin = at + marker.size();
                const std::size_t end = line.find('"', begin);
                if (end != std::string::npos && end > begin)
                {
                    push_unique(line.substr(begin, end - begin));
                }
            }
        }
        return out;
    }

    /**
     * @brief 一个项目平台对应的 MSVC 目标目录名。
     *
     * x64->x64、Win32/x86->x86、ARM64->ARM64；其余原样作为目标目录名
     * 透传（工具链布局恰好如此拼写该目录）。
     */
    [[nodiscard]]
    inline auto cl_target_dir(std::string_view platform) -> std::string
    {
        if (detail::ascii_iequals(platform, "Win32")
            || detail::ascii_iequals(platform, "x86"))
        {
            return "x86";
        }
        if (detail::ascii_iequals(platform, "ARM64"))
        {
            return "ARM64";
        }
        return std::string{platform};
    }

    /**
     * @brief 给定项目平台下、某 VCToolsInstallDir 值中的 cl.exe 绝对
     * 路径。
     *
     * @internal 二进制日志 ground truth 表明 MSBuild 对每个目标默认主机
     * 都是 HostX86（32 位主机也编译 x64 目标），故拼接为 `<tools>`/bin/
     * HostX86/`<target>`/cl.exe。容忍工具目录的尾分隔符与反斜杠拼写（输出
     * 一律正斜杠）。
     */
    [[nodiscard]]
    inline auto cl_under_tools_dir(
        std::string_view tools_dir,
        std::string_view platform) -> std::string
    {
        std::string dir{tools_dir};
        while (!dir.empty() && (dir.back() == '/' || dir.back() == '\\'))
        {
            dir.pop_back();
        }
        std::ranges::replace(dir, '\\', '/');
        return std::format(
            "{}/bin/HostX86/{}/cl.exe",
            dir,
            cl_target_dir(platform));
    }

    /**
     * @brief `-getProperty:<name>` 运行的取值。
     *
     * @internal 求值输出可能在属性值周围夹带噪声行，既定规则是取最后一
     * 条非空的、形如绝对文件系统路径的行（盘符根或 UNC）。无合格行时
     * 返回 ""。
     */
    [[nodiscard]]
    inline auto parse_last_path_line(std::string_view output) -> std::string
    {
        std::string best;
        const auto  looks_like_path = [](std::string_view line)
        {
            if (line.size() >= 3)
            {
                const char c0 = line[0];
                const bool letter =
                    (c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z');
                if (letter && line[1] == ':'
                    && (line[2] == '/' || line[2] == '\\'))
                {
                    return true;
                }
            }
            return line.starts_with("\\\\");
        };
        std::size_t start = 0;
        while (start <= output.size())
        {
            const std::size_t      end = output.find('\n', start);
            const std::string_view line = output.substr(
                start,
                end == std::string_view::npos ? std::string_view::npos
                                              : end - start);
            const std::string trimmed = cg::util::trim(line);
            if (!trimmed.empty() && looks_like_path(trimmed))
            {
                best = trimmed;
            }
            if (end == std::string_view::npos)
            {
                break;
            }
            start = end + 1;
        }
        return best;
    }

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
        /** @brief 成员在解决方案配置下的 ActiveCfg（"Debug|x64"）。"" = 未记录。 */
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
                if (const auto b = v->get("in_build");
                    b && b->is_bool())
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

    /**
     * @brief 一份解析后的 wrap 模式捕获：记录到的编译器调用加包装器写
     * 下的每条 error 行（rsp 展开失败、环境变量缺失），以及读侧跳过的
     * 坏行计数。
     *
     * @internal 错误是数据不是异常：由调用方上报，诚实的 TU 图景来自
     * 实际捕获到的内容；坏行同理——计数加 stderr 告警，绝不令整轮翻译
     * 失败。
     */
    struct WrapLog
    {
        std::vector<ClInvocation> invocations;
        std::vector<std::string>  errors;
        int bad_rows = 0;    // 解析失败被跳过的 capture 行（stderr 已告警）
        std::string db_path; // 实际读取的 capture.db（零捕获报错时指路）
    };

    namespace detail
    {

        /**
         * @brief 消费一条预编译 capture SELECT：逐行分拣进 WrapLog。
         *
         * @internal 迭代中途的失败（连接器以异常报告 step 错误）降级为一行
         * stderr 告警并保留已收集的行——与旧直调形态"step 非 ROW 即 break、
         * 部分结果保留"逐字对齐。可空列（include_env/message）的 NULL 读作
         * 空串，同旧 column_text 对 nullptr 的折返。argv_json 解析失败按坏行
         * 计数并继续（错误是数据不是异常）。
         */
        template <typename Sel>
        inline void consume_capture_rows(
            sqlpp::sqlite3::connection& db,
            Sel&                        sel,
            WrapLog&                    out,
            const std::string&          db_path)
        {
            try
            {
                for (const auto& r : db(sel))
                {
                    const std::string kind{r.kind};
                    if (kind == "error")
                    {
                        out.errors.push_back(
                            r.message.has_value() ? std::string{*r.message}
                                                  : std::string{});
                        continue;
                    }
                    const std::string argv_json{r.argvJson};
                    ClInvocation      inv;
                    try
                    {
                        const json::Document doc =
                            json::Document::parse(argv_json);
                        const json::ValueView argv_view = doc.root();
                        if (!argv_view.is_array())
                        {
                            throw std::runtime_error(
                                "argv_json is not an array");
                        }
                        const std::size_t count = argv_view.size();
                        for (std::size_t i = 0; i < count; ++i)
                        {
                            if (const auto v = argv_view.at(i);
                                v && v->is_string())
                            {
                                inv.argv_utf8.emplace_back(*v->as_string());
                            }
                        }
                    }
                    catch (const std::exception&)
                    {
                        ++out.bad_rows;
                        continue;
                    }
                    inv.cwd = std::string{r.cwd};
                    inv.include_env = r.includeEnv.has_value()
                                          ? std::string{*r.includeEnv}
                                          : std::string{};
                    // 逐次调用的真实编译器：混合工具集下每个 TU
                    // 的兼容版本来源。
                    inv.real_cl = std::string{r.realCl};
                    out.invocations.push_back(std::move(inv));
                }
            }
            catch (const std::exception& e)
            {
                std::println(
                    std::cerr,
                    "ccg msbuild: capture db read failed at {}: {}",
                    db_path,
                    e.what());
            }
        }

    } // namespace detail

    /**
     * @brief 读取一份 wrap 模式捕获（capture.db 的 capture 表）。
     *
     * path 参数接受 CCG_CLWRAP_LOG 的值（jsonl 名，经与写侧同一函数
     * detail::capture_db_of_log_path 推导出 db 位置）或 capture.db 直接
     * 路径。db 缺失读作空捕获（调用方上报"零捕获"）；行内 argv_json 解析
     * 失败按坏行计数、stderr 告警（含 capture.db 完整路径）并继续——
     * 单行损坏不再令整轮翻译抛错。token 非空时只读本运行 token 的行：
     * prune 删旧库是主路径，token 过滤是纵深防御（prune 因句柄占用/ACL
     * 失败时，上一轮的行绝不混入本次翻译）；空 token（默认）读全表，供
     * 测试与直读调用面。读侧永远可以信任返回值里带出的 db_path。
     *
     * @internal 只读打开（不带 CREATE：库缺失时它就是"空捕获"信号，连接
     * 器把 open 失败报告为异常，就地捕获降级）。READWRITE 而非 READONLY
     * 是刻意的：崩溃残留的 WAL 需要恢复权才能 checkpoint，且本进程对
     * .ccg 目录本就有写权。查询经 sqlpp23 类型化语句：本 pin "参数名字
     * 标签每语句只能出现一次"，旧 WHERE (?1='' OR token=?1) 的同参数双绑
     * 不可表达，按 token 空否走两条预编译变体（无 WHERE 全表 / token 等
     * 值过滤），语义与旧单语句逐字一致；表缺失时 prepare 报异常，降级
     * 文案与旧 sqlite3_errmsg 口径相同。
     */
    [[nodiscard]]
    inline auto read_wrap_log(
        const std::string& path,
        const std::string& token = {}) -> WrapLog
    {
        WrapLog           out;
        const std::string db_path = detail::capture_db_of_log_path(path);
        out.db_path = db_path;
        std::optional<sqlpp::sqlite3::connection> db;
        try
        {
            sqlpp::sqlite3::connection_config cfg;
            cfg.path_to_database = db_path;
            cfg.flags = SQLITE_OPEN_READWRITE;
            db.emplace(cfg);
        }
        catch (const std::exception&)
        {
            std::println(
                std::cerr,
                "ccg msbuild: capture db absent or unreadable: {}",
                db_path);
            return out;
        }
        sqlite3_busy_timeout(db->native_handle(), 5000);
        const cap::Capture c{};
        try
        {
            // 六列清单单点：两条变体只在 WHERE/参数绑定位形上分叉（参数
            // 名字标签每语句一次的限制见函数注释），列集演化不再两处同步。
            auto base = sqlpp::select(
                            c.kind,
                            c.argvJson,
                            c.cwd,
                            c.includeEnv,
                            c.realCl,
                            c.message)
                            .from(c);
            if (token.empty())
            {
                auto sel = db->prepare(base.order_by(c.id.asc()));
                detail::consume_capture_rows(*db, sel, out, db_path);
            }
            else
            {
                auto sel =
                    db->prepare(base.where(c.token == sqlpp::parameter(c.token))
                                    .order_by(c.id.asc()));
                sel.parameters.token = token;
                detail::consume_capture_rows(*db, sel, out, db_path);
            }
        }
        catch (const std::exception& e)
        {
            std::println(
                std::cerr,
                "ccg msbuild: capture db {} has no capture "
                "table: {}",
                db_path,
                e.what());
            return out;
        }
        if (out.bad_rows != 0)
        {
            std::println(
                std::cerr,
                "ccg msbuild: capture db {} has {} bad row(s) skipped "
                "(counted, not fatal)",
                db_path,
                out.bad_rows);
        }
        return out;
    }

    /**
     * @brief 把适配器条目序列化为 compile_commands.json（arguments 形状
     * ——加载器 documented 的输入；argv[0] "clang-cl" 按设计在加载时被
     * 丢弃）。
     *
     * @internal 每条目一个对象、经共享写出器保持转义统一；顶层由手工拼
     * 接——写出器的根按构造只能是对象。与注册表同法原子替换。
     */
    inline void write_compile_commands(
        const std::string&           path,
        const std::vector<CdbEntry>& entries)
    {
        std::string text;
        text.reserve(entries.size() * 256 + 16);
        text += "[\n";
        for (std::size_t i = 0; i < entries.size(); ++i)
        {
            const CdbEntry& e = entries[i];
            json::Array     args;
            for (const std::string& a : e.arguments)
            {
                args.add(a);
            }
            json::Object entry;
            entry.set("directory", e.directory);
            entry.set("file", e.file);
            entry.set("arguments", std::move(args));
            text += json::stringify(std::move(entry));
            if (i + 1 < entries.size())
            {
                text += ',';
            }
            text += '\n';
        }
        text += "]\n";
        detail::atomic_write_text(path, text, "compile_commands.json");
    }

    /**
     * @brief MSBuild PCH sidecar 的一台 producer（`/Yc` 调用）。
     */
    struct PchProducerRecord
    {
        std::string              pch_key;       // 绝对 /Fp
        std::string              source;        // producer TU（绝对）
        std::string              umbrella;      // /Yc 头拼写
        std::string              umbrella_path; // 可解析时绝对，否则 ""
        std::string              cwd;           // 记录的构建目录
        std::vector<std::string> argv; // 展开后的逐字 argv（含 argv[0]）
        std::string              command_digest;
    };

    struct PchConsumerRecord
    {
        std::string file;    // consumer TU（绝对）
        std::string pch_key; // 绝对 /Fp
        std::string pch_header;
        std::string command_digest;
    };

    /**
     * @brief 与 compile_commands.json 同批原子写 PCH sidecar。
     *
     * @internal cdb 保持剥离 `/Yc /Yu /Fp`（clangd/第三方兼容；producer
     * 不会被索引两次）。sidecar 携带 `cdb_sha256`，加载侧校验不符即忽略
     * ——两份世界观绝不混用。raw wrap log 每次提取前会被删除，因此这是
     * 唯一的持久捕获。
     */
    inline void write_pch_sidecar(
        const std::string&                    path,
        const std::string&                    cdb_path,
        const std::string&                    config,
        const std::string&                    platform,
        const std::vector<PchProducerRecord>& producers,
        const std::vector<PchConsumerRecord>& consumers,
        const std::vector<std::string>&       not_using)
    {
        namespace json = cg::util::json;
        std::string cdb_sha;
        try
        {
            cdb_sha = cg::util::sha256_file_hex(cdb_path);
        }
        catch (const std::exception&)
        {
            return; // cdb 未写成：没有可信 sidecar 可写
        }
        json::Object root;
        root.set("version", static_cast<std::int64_t>(1));
        root.set("generated_by", "ccg msbuild");
        root.set("config", config);
        root.set("platform", platform);
        root.set("cdb_sha256", cdb_sha);
        json::Array producer_list;
        for (const PchProducerRecord& p : producers)
        {
            json::Object o;
            o.set("pch_key", p.pch_key);
            o.set("source", p.source);
            o.set("umbrella", p.umbrella);
            o.set("umbrella_path", p.umbrella_path);
            o.set("cwd", p.cwd);
            o.set("command_digest", p.command_digest);
            json::Array args;
            for (const std::string& a : p.argv)
            {
                args.add(a);
            }
            o.set("argv", std::move(args));
            producer_list.add(std::move(o));
        }
        root.set("producers", std::move(producer_list));
        json::Array consumer_list;
        for (const PchConsumerRecord& c : consumers)
        {
            json::Object o;
            o.set("file", c.file);
            o.set("pch_key", c.pch_key);
            o.set("pch_header", c.pch_header);
            o.set("command_digest", c.command_digest);
            consumer_list.add(std::move(o));
        }
        root.set("consumers", std::move(consumer_list));
        json::Array unused;
        for (const std::string& f : not_using)
        {
            unused.add(f);
        }
        root.set("not_using", std::move(unused));
        detail::atomic_write_text(
            path,
            json::stringify(std::move(root)),
            "pch.json");
    }

    /**
     * @brief 一次被包装 Rebuild 的运行模式。
     *
     * Record（默认）：编译类工具全部桩化——构建图照常执行（CustomBuild/
     * midl 等生成器真跑，保证 TU 集合完备），cl/link/lib/rc/mt/masm 只记
     * argv、零写盘空跑，不发生真编译。FullBuild（--full-build）：现行真编
     * 行为——只包装 CL，逐次转发真实 cl。
     */
    enum class ExtractMode
    {
        FullBuild,
        Record
    };

    /**
     * @brief 被包装 Rebuild 需要的一切。
     *
     * @internal 下方的构建器是纯文本；Windows 运行器从运行中的镜像填充
     * cl_tool_exe/path 并完成派生。
     */
    struct ExtractParams
    {
        std::string msbuild; // 解析到的 MSBuild.exe（绝对）
        std::string input;   // .sln/.vcxproj（绝对）
        std::string configuration;
        std::string platform;
        std::string real_cl;      // 绝对 cl.exe（CCG_CLWRAP_REAL_CL 全局回退）
        std::string log_path;     // wrap 模式 JSONL 目的地
        std::string cl_tool_exe;  // ccg.exe 文件名（CLToolExe，FullBuild 用）
        std::string cl_tool_path; // ccg.exe 目录（CLToolPath，FullBuild 用）
        std::string extra_args;   // 逐字命令行尾巴，无则 ""
        ExtractMode mode = ExtractMode::FullBuild; // 默认=真编（现行行为）
        std::string stub_tool_dir; // 记录模式：桩工具目录（ccg 家族名副本）
        std::string int_dir;       // 记录模式：沙箱 IntDir（绝对、尾分隔符）
        std::string out_dir;       // 记录模式：沙箱 OutDir（绝对、尾分隔符）
        std::string cl_map;        // "<项目目录>\t<cl>" 行列表（'\n' 连接）
    };

    namespace detail
    {

        /**
         * @brief 一个 -p:`<Name>`=`<Value>` token。
         *
         * @internal 值永不内嵌引号——token 始终拼写 -p:Name=`<value>`，含空白
         * 的值由派生处的命令行组装对整个 token 做外层 CRT 引用，MSBuild 的
         * argv 解析剥掉外层引号后拿到干净的属性值。实测否决了旧计划结论
         * "含空格的 /p: 值要加引号"：MSBuild 收到 argv 后不再剥引号，内嵌
         * 引号会字面留在属性值里，展开进属性函数即 MSB4184"路径中具有非法
         * 字符"——工程路径含空格时记录模式整体失败。沙箱的 IntDir/OutDir 值
         * 刻意用正斜杠拼写，回避值以反斜杠收尾时的引用边缘情况。
         */
        [[nodiscard]]
        inline auto property_token(
            std::string_view name,
            std::string_view value) -> std::string
        {
            return std::format("-p:{}={}", name, value);
        }

    } // namespace detail

    /**
     * @brief 一次被包装 Rebuild 的标准 MSBuild 参数向量（纯函数，供单元
     * 测试与详细计划行）。
     *
     * -t:Rebuild（v1 默认——完整 TU 覆盖；普通 Build 只记录被重新调用的
     * cl）、配置对、把编译类工具调用改道进 wrap 模式的 ToolPath 家族、
     * 再加经研究证实的单 MSBuild/verbosity/无横幅组合。
     *
     * FullBuild 形状与历史行为逐 token 一致（CLToolExe/CLToolPath 指向
     * ccg.exe 自身）。Record 形状：只覆盖各工具的 ToolPath（不点名
     * ToolExe——MSVC 工具集的默认 cl.exe 与 ClangCL 的 clang-cl.exe 各自
     * 命中桩目录里的同名副本）、注入沙箱 IntDir/OutDir；MASM 等裸名工具
     * 由运行器的 PATH 前置接管（不经属性面）。
     *
     * @internal extra_args 不在向量内：它在组装好的命令行末尾逐字拼接，
     * 使调用方侧的引用得以保全。
     */
    [[nodiscard]]
    inline auto build_extract_args(const ExtractParams& p)
        -> std::vector<std::string>
    {
        std::vector<std::string> args;
        args.emplace_back(p.input);
        args.emplace_back("-t:Rebuild");
        args.emplace_back(
            detail::property_token("Configuration", p.configuration));
        args.emplace_back(detail::property_token("Platform", p.platform));
        if (p.mode == ExtractMode::Record)
        {
            // 编译类工具家族全部改道桩目录；midl 刻意不在其中（真跑保
            // 编译输入图）。CL 不点名 ToolExe：保留工具集默认名（cl.exe /
            // clang-cl.exe），桩目录内同名副本接管。
            args.emplace_back(
                detail::property_token("CLToolPath", p.stub_tool_dir));
            args.emplace_back(
                detail::property_token("LinkToolPath", p.stub_tool_dir));
            args.emplace_back(
                detail::property_token("LibToolPath", p.stub_tool_dir));
            args.emplace_back(
                detail::property_token("RCToolPath", p.stub_tool_dir));
            args.emplace_back(
                detail::property_token("MtToolPath", p.stub_tool_dir));
            args.emplace_back(
                detail::property_token("BSCMakeToolPath", p.stub_tool_dir));
            args.emplace_back(
                detail::property_token("XDCMakeToolPath", p.stub_tool_dir));
            if (!p.int_dir.empty())
            {
                args.emplace_back(detail::property_token("IntDir", p.int_dir));
            }
            if (!p.out_dir.empty())
            {
                args.emplace_back(detail::property_token("OutDir", p.out_dir));
            }
        }
        else
        {
            args.emplace_back(
                detail::property_token("CLToolExe", p.cl_tool_exe));
            args.emplace_back(
                detail::property_token("CLToolPath", p.cl_tool_path));
        }
        args.emplace_back("-m:1");
        args.emplace_back("-v:m");
        args.emplace_back("-nologo");
        return args;
    }

#if defined(_WIN32)

    namespace detail
    {

        struct RunResult
        {
            bool        started = false;
            int         exit_code = -1;
            std::string out; // 子进程 stdout（UTF-8）
        };

        /**
         * @brief 运行 `exe args_text`，捕获 stdout（等待前排空到 EOF，
         * 驱动器讯问风格）并继承环境；stderr 由原语并发排空后丢弃。
         *
         * @internal
         * cg::util::platform::run_child 捕获原语的讯问配置层：两路捕获、无
         * stdin（子进程不需要）、无限等待、继承环境、CREATE_NO_WINDOW、
         * 裸名 PATH 搜索（application 不点名）。常规子进程都安静（vswhere
         * 查找、-getProperty 值、cl /? 横幅），但 --cl 允许任何可执行文件成为
         * 横幅探针，故两根管道按构造并发排空——绝不顺序：子进程先向 stderr
         * 推送超过管道缓冲的内容再等 stdout EOF 时，先 stdout 的排空会令两个
         * 进程都死锁（审计发现；ccg_msbuild_capture 门控经真实二进制驱动过
         * 128 KiB 的 stderr 洪水）。并发排水沉淀在原语的行为清单里；排空的
         * stderr 文本不进结果——本层的调用面都只消费 stdout。
         */
        [[nodiscard]]
        inline auto run_capture(
            const std::wstring& exe,
            const std::wstring& args_text) -> RunResult
        {
            cg::util::platform::ChildRunSpec spec;
            spec.cmdline = std::format(
                L"{} {}",
                cg::util::platform::quote_for_command_line(exe),
                args_text);
            spec.stdin_mode = cg::util::platform::ChildStdin::NullHandle;
            spec.create_no_window = true;
            const cg::util::platform::ChildRunResult run =
                cg::util::platform::run_child(spec);
            RunResult result;
            result.started = run.started && run.pipes_ok;
            result.exit_code = run.exit_code;
            result.out = std::move(run.out);
            return result;
        }

        /** @brief 首条非空 stdout 行（vswhere 每行打印一个查找结果）。 */
        [[nodiscard]]
        inline auto first_output_line(const std::string& output) -> std::string
        {
            std::istringstream lines{output};
            std::string        line;
            while (std::getline(lines, line))
            {
                cg::util::strip_cr(line);
                if (!line.empty())
                {
                    return line;
                }
            }
            return {};
        }

        /**
         * @brief 被包装 Rebuild 的子环境块：当前块减去 wrap 变量（防御性——以
         * 防杂散值漏进我们自己的环境）与 remove 名单，加新鲜覆盖。
         *
         * @internal 双 NUL 结尾、CREATE_UNICODE_ENVIRONMENT 布局。名字比较不
         * 区分大小写（Windows 环境名语义如此——用户块里 PATH 常拼写为
         * Path，大小写敏感比较会让覆盖/删除双双漏配）。remove 只从本子环境
         * 块剔除（构建子树专用；ccg 进程与探测子进程的环境不动）。
         */
        [[nodiscard]]
        inline auto build_child_env(
            const std::vector<std::pair<std::wstring, std::wstring>>& overrides,
            const std::vector<std::wstring>& remove = {}) -> std::wstring
        {
            const auto named =
                [](std::wstring_view entry, std::wstring_view name)
            {
                if (entry.size() <= name.size())
                {
                    return false;
                }
                for (std::size_t i = 0; i < name.size(); ++i)
                {
                    const wchar_t la =
                        entry[i] >= L'A' && entry[i] <= L'Z'
                            ? static_cast<wchar_t>(entry[i] - L'A' + L'a')
                            : entry[i];
                    const wchar_t lb =
                        name[i] >= L'A' && name[i] <= L'Z'
                            ? static_cast<wchar_t>(name[i] - L'A' + L'a')
                            : name[i];
                    if (la != lb)
                    {
                        return false;
                    }
                }
                return entry[name.size()] == L'=';
            };
            std::wstring block;
            const LPWCH  current = GetEnvironmentStringsW();
            if (current != nullptr)
            {
                for (LPWCH entry = current; *entry != L'\0';)
                {
                    const std::wstring_view view{entry};
                    bool                    dropped = false;
                    for (const auto& [name, value] : overrides)
                    {
                        static_cast<void>(value);
                        if (named(view, name))
                        {
                            dropped = true;
                            break;
                        }
                    }
                    if (!dropped)
                    {
                        for (const std::wstring& name : remove)
                        {
                            if (named(view, name))
                            {
                                dropped = true;
                                break;
                            }
                        }
                    }
                    if (!dropped)
                    {
                        block += view;
                        block.push_back(L'\0');
                    }
                    entry += view.size() + 1;
                }
                FreeEnvironmentStringsW(current);
            }
            for (const auto& [name, value] : overrides)
            {
                block += name;
                block.push_back(L'=');
                block += value;
                block.push_back(L'\0');
            }
            block.push_back(L'\0');
            return block;
        }

        /** @brief 运行中镜像的目录（CLToolPath），UTF-8、原生分隔符。 */
        [[nodiscard]]
        inline auto current_exe_dir() -> std::string
        {
            std::wstring path(MAX_PATH, L'\0');
            const DWORD  len =
                GetModuleFileNameW(nullptr, path.data(), MAX_PATH);
            if (len == 0 || len >= MAX_PATH)
            {
                return {};
            }
            path.resize(len);
            return cg::util::platform::narrow_utf8(
                std::filesystem::path{path}
                    .parent_path()
                    .lexically_normal()
                    .wstring());
        }

        /** @brief 运行中镜像的文件名（CLToolExe）。 */
        [[nodiscard]]
        inline auto current_exe_name() -> std::string
        {
            std::wstring path(MAX_PATH, L'\0');
            const DWORD  len =
                GetModuleFileNameW(nullptr, path.data(), MAX_PATH);
            if (len == 0 || len >= MAX_PATH)
            {
                return "ccg.exe";
            }
            path.resize(len);
            return cg::util::platform::narrow_utf8(
                std::filesystem::path{path}.filename().wstring());
        }

        /**
         * @brief 记录模式的桩工具目录：ccg 镜像以家族名（cl.exe/link.exe/…）
         * 的硬链接（同卷）或副本住进 dir。
         *
         * @internal 同名且同大小的既有副本跳过（幂等）；任何名字建不出来都
         * 响亮抛出——半桩化的记录模式会静默真编，比失败更糟。
         */
        inline void make_stub_tool_dir(const std::string& dir)
        {
            std::wstring exe(MAX_PATH, L'\0');
            const DWORD len = GetModuleFileNameW(nullptr, exe.data(), MAX_PATH);
            if (len == 0 || len >= MAX_PATH)
            {
                throw std::runtime_error(
                    "ccg msbuild: cannot locate the running ccg.exe for the stub "
                    "tool dir");
            }
            exe.resize(len);
            std::error_code ec;
            const auto      exe_size =
                std::filesystem::file_size(std::filesystem::path{exe}, ec);
            if (ec)
            {
                throw std::runtime_error(
                    "ccg msbuild: cannot size the running ccg.exe for the stub "
                    "tool dir");
            }
            const std::filesystem::path dir_fs = fs_path_from_utf8(dir);
            std::filesystem::create_directories(dir_fs, ec);
            std::vector<std::string> missing;
            for (const std::string_view name : stub_tool_family)
            {
                const std::filesystem::path target =
                    dir_fs
                    / std::wstring{
                        cg::util::platform::widen_utf8(std::string{name})};
                std::error_code size_ec;
                const auto      existing =
                    std::filesystem::file_size(target, size_ec);
                if (!size_ec && existing == exe_size)
                {
                    continue;
                }
                std::filesystem::remove(target, ec);
                std::error_code link_ec;
                std::filesystem::create_hard_link(
                    target,
                    std::filesystem::path{exe},
                    link_ec);
                if (link_ec)
                {
                    std::error_code copy_ec;
                    std::filesystem::copy_file(
                        std::filesystem::path{exe},
                        target,
                        std::filesystem::copy_options::overwrite_existing,
                        copy_ec);
                    if (copy_ec)
                    {
                        missing.push_back(std::string{name});
                    }
                }
            }
            if (!missing.empty())
            {
                std::string joined;
                for (const std::string& m : missing)
                {
                    joined += joined.empty() ? "" : ", ";
                    joined += m;
                }
                throw std::runtime_error(
                    std::format(
                        "ccg msbuild: cannot create stub tool copies in {}: {}",
                        dir,
                        joined));
            }
        }

        [[nodiscard]]
        inline auto file_exists_wide(const std::wstring& path) -> bool
        {
            const DWORD attrs = GetFileAttributesW(path.c_str());
            return attrs != INVALID_FILE_ATTRIBUTES
                   && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
        }

        /**
         * @brief 解析 vswhere 的可执行名：规范位置（x86 program-files 环境
         * 锚）存在给绝对路径，缺席时退回裸名 "vswhere.exe"（交给
         * CreateProcessW 的 PATH 搜索）。
         *
         * @internal 锚点从环境解析——绝不拼写死盘符路径（可移植性门控）。
         * 回退语义单点收在这：find_msbuild 与 find_cl 的讯问入口共用，两处
         * 不再各写一份。
         */
        [[nodiscard]]
        inline auto resolve_vswhere() -> std::wstring
        {
            const std::optional<std::wstring> pf_x86 =
                env_wide(L"ProgramFiles(x86)");
            if (pf_x86 && !pf_x86->empty())
            {
                const std::wstring canonical =
                    *pf_x86
                    + L"\\Microsoft Visual Studio\\Installer\\vswhere.exe";
                if (file_exists_wide(canonical))
                {
                    return canonical;
                }
            }
            return L"vswhere.exe"; // CreateProcessW 会搜索 PATH
        }

    } // namespace detail

    /**
     * @brief 解析到的 cl.exe。
     *
     * @internal 不携带横幅版本：兼容版本已改由逐 TU 的 INCLUDE 工具集
     * 目录（回退 real_cl 路径）推导，逐工程解析下每成员跑一次 `cl /?`
     * 探针是无人消费的死开销。
     */
    struct ClInfo
    {
        std::string path; // 绝对 cl.exe，未解析到时 ""
        /** @brief 解析来源（诚实报告用）："override" | "project-property" |
         * "vswhere-fallback"。 */
        std::string resolution;
    };

    /**
     * @brief 定位 MSBuild.exe。
     *
     * 优先级：显式选项/环境覆盖（CCG_MSBUILD_EXE），然后 vswhere
     * （-latest -products * -requires Microsoft.Component.MSBuild
     * -find MSBuild 任意层级的 Bin/MSBuild.exe）；规范 Installer 位置
     * 缺席时 vswhere 自身退回 PATH 搜索。什么都解析不到时为 ""（CLI 层
     * 上报错误）。
     */
    [[nodiscard]]
    inline auto find_msbuild(const std::string& override_msbuild) -> std::string
    {
        if (!override_msbuild.empty())
        {
            return override_msbuild;
        }
        if (const std::optional<std::wstring> env_msbuild =
                detail::env_wide(L"CCG_MSBUILD_EXE"))
        {
            return cg::util::platform::narrow_utf8(*env_msbuild);
        }
        const std::wstring      vswhere = detail::resolve_vswhere();
        const detail::RunResult r = detail::run_capture(
            vswhere,
            L"-latest -products * -requires Microsoft.Component.MSBuild "
            L"-find MSBuild/**/Bin/MSBuild.exe");
        if (!r.started || r.exit_code != 0)
        {
            return {};
        }
        return detail::first_output_line(r.out);
    }

    /**
     * @brief 为一个输入+配置+平台定位真实 cl.exe。
     *
     * 优先级：显式 --cl 覆盖；`MSBuild <input> -getProperty:
     * VCToolsInstallDir -nologo -p:Configuration=`<C>` -p:Platform=`<P>`（仅
     * 求值、不构建）拼接 bin/HostX86/`<target>`/cl.exe；vswhere 兜底
     * （-find VC/Tools/MSVC 下任意版本、任意 Host 目录、任意深度的
     * cl.exe，最高工具集版本胜出，同版本优先 HostX86/`<target>`）。
     *
     * @internal
     * 属性查询必须携带配置对：VCToolsVersion/PlatformToolset 钉子写在
     * per-configuration PropertyGroup（标准模板位置），不带配置求值时所有
     * Condition 不命中、落回默认工具集（钉了 14.44 却解析到 14.51 的根因；
     * 补上 -p:Configuration/-p:Platform 后实测即刻正确）。extra_args 随后
     * 拼接，用户显式 /p: 继续可覆盖。-getProperty 主路径需要单个 PROJECT：
     * .sln 输入上 MSBuild 直接拒绝属性求值（MSB1063），故解决方案在
     * resolve_project_cls 里逐成员工程求值，此处 sln 输入径直走 vswhere
     * 兜底（降级为最后防线）。求值输出可能夹带噪声：最后一条路径形行是
     * 属性值。版本挑选：最高工具集版本胜出；同版本时优先 MSBuild 自己会
     * 选的 HostX86/`<target>` 组合而非其他主机。主机目录大小写随安装而异
     * （Hostx86/HostX86），比较不区分大小写。
     */
    [[nodiscard]]
    inline auto find_cl(
        const std::string& msbuild,
        const std::string& input,
        const std::string& configuration,
        const std::string& platform,
        const std::string& override_cl,
        const std::string& extra_args = {}) -> ClInfo
    {
        if (!override_cl.empty())
        {
            return ClInfo{.path = override_cl, .resolution = "override"};
        }
        const bool input_is_project =
            !detail::ascii_iequals(
                std::filesystem::path{input}.extension().string(),
                ".sln")
            && !input.empty();
        if (!msbuild.empty() && input_is_project)
        {
            std::wstring query = cg::util::platform::quote_for_command_line(
                                     cg::util::platform::widen_utf8(input))
                                 + L" -getProperty:VCToolsInstallDir -nologo";
            if (!configuration.empty())
            {
                query +=
                    L" "
                    + cg::util::platform::widen_utf8(
                        detail::property_token("Configuration", configuration));
            }
            if (!platform.empty())
            {
                query += L" "
                         + cg::util::platform::widen_utf8(
                             detail::property_token("Platform", platform));
            }
            if (!extra_args.empty())
            {
                query += L" " + cg::util::platform::widen_utf8(extra_args);
            }
            const detail::RunResult r = detail::run_capture(
                cg::util::platform::widen_utf8(msbuild),
                query);
            const std::string tools_dir = parse_last_path_line(r.out);
            if (!tools_dir.empty())
            {
                const std::string cl_path =
                    cl_under_tools_dir(tools_dir, platform);
                if (detail::file_exists_wide(
                        cg::util::platform::widen_utf8(cl_path)))
                {
                    return ClInfo{
                        .path = cl_path,
                        .resolution = "project-property"};
                }
            }
        }
        const std::wstring      vswhere = detail::resolve_vswhere();
        const detail::RunResult find = detail::run_capture(
            vswhere,
            L"-latest -products * -find VC/Tools/MSVC/*/bin/Host*/**/cl.exe");
        if (!find.started || find.exit_code != 0)
        {
            return {};
        }
        const std::string preferred_suffix =
            std::format("/bin/HostX86/{}/cl.exe", cl_target_dir(platform));
        const std::string preferred_lower =
            cg::util::ascii_lower(preferred_suffix);
        std::string        best;
        bool               best_preferred = false;
        std::vector<int>   best_version;
        std::istringstream lines{find.out};
        std::string        line;
        while (std::getline(lines, line))
        {
            cg::util::strip_cr(line);
            if (line.empty())
            {
                continue;
            }
            const std::vector<int> version = msvc_toolset_version_of(line);
            if (version.empty())
            {
                continue;
            }
            const std::string forward = cg::util::to_forward_slashes(line);
            const bool        preferred =
                cg::util::ascii_lower(forward).ends_with(preferred_lower);
            const bool take =
                best.empty() || version > best_version
                || (version == best_version && preferred && !best_preferred);
            if (take)
            {
                best = line;
                best_preferred = preferred;
                best_version = version;
            }
        }
        if (best.empty())
        {
            return {};
        }
        return ClInfo{.path = best, .resolution = "vswhere-fallback"};
    }

    /**
     * @brief .sln 的一个成员工程（.vcxproj）。
     *
     * @internal active_cfg 是该成员在指定解决方案配置下的
     * ProjectConfigurationPlatforms ActiveCfg 映射（"C|P"），无映射时为 ""
     * （调用方回退解决方案自身的配置对）。build 是同配置下 Build.0 行的
     * 参与位——无该行的工程被加载但不编（组装器与注册表披露共用本字段，
     * 单点解析杜绝两处口径分歧）。
     */
    struct SlnProject
    {
        std::string name;       // sln 条目名（"projA"）
        std::string path;       // 绝对 .vcxproj（正斜杠）
        std::string guid;       // "{...}"
        std::string active_cfg; // "Debug|x64"，无映射 ""
        bool        build = false; // Build.0 参与位（该配置下真编）
    };

    /**
     * @brief 枚举 .sln 的 .vcxproj 成员工程并解析指定配置的 ActiveCfg 与
     *        Build.0 参与位。
     *
     * 面向行的文本扫描（与 enumerate_configs 同风格：确定性、不求值）。
     * 解不了配置映射的成员 active_cfg 留空；.csproj 等非 VC 成员跳过。
     * Build.0 参与位同趟解析（guid 归一化对齐——该行的 guid 大小写/花括号
     * 形态不保证与 Project 行一致），是求值清单与注册表披露共用的单点
     * 事实源。
     */
    [[nodiscard]]
    inline auto enumerate_sln_projects(
        const std::string& input,
        const std::string& solution_cfg) -> std::vector<SlnProject>
    {
        std::vector<SlnProject>          out;
        const std::optional<std::string> text = detail::read_text_file(input);
        if (!text)
        {
            return out;
        }
        if (!detail::ascii_iequals(
                std::filesystem::path{input}.extension().string(),
                ".sln"))
        {
            return out;
        }
        const std::string base =
            std::filesystem::path{detail::fs_path_from_utf8(input)}
                .parent_path()
                .generic_string();

        std::istringstream lines{*text};
        std::string        line;
        std::vector<std::tuple<std::string, std::string, std::string>> cfg_map;
        std::set<std::string> build_flags;
        const std::string     want =
            cg::util::ascii_lower(cg::util::trim(solution_cfg));
        while (std::getline(lines, line))
        {
            cg::util::strip_cr(line);
            if (const std::size_t at = line.find("Project(\"");
                at != std::string::npos)
            {
                // Project("{TYPE}") = "name", "rel\path.vcxproj", "{GUID}"
                // ——取 '=' 后的逗号分隔引号串（name/path/guid）。
                const std::size_t eq = line.find('=');
                if (eq == std::string::npos)
                {
                    continue;
                }
                std::vector<std::string> quoted;
                const std::string        tail = line.substr(eq + 1);
                for (std::size_t i = 0; i < tail.size();)
                {
                    const std::size_t b = tail.find('"', i);
                    if (b == std::string::npos)
                    {
                        break;
                    }
                    const std::size_t e = tail.find('"', b + 1);
                    if (e == std::string::npos)
                    {
                        break;
                    }
                    quoted.push_back(tail.substr(b + 1, e - b - 1));
                    i = e + 1;
                }
                if (quoted.size() < 2)
                {
                    continue;
                }
                std::string rel = quoted[1];
                std::ranges::replace(rel, '\\', '/');
                if (!detail::ascii_iequals(
                        std::filesystem::path{rel}.extension().string(),
                        ".vcxproj"))
                {
                    continue; // 解决方案文件夹 / 非 VC 工程
                }
                SlnProject p;
                p.name = quoted[0];
                p.path = (std::filesystem::path{base} / rel)
                             .lexically_normal()
                             .generic_string();
                p.guid = quoted.size() >= 3 ? quoted[2] : std::string{};
                out.push_back(std::move(p));
                continue;
            }
            // {GUID}.SolutionCfg.ActiveCfg = ProjectCfg（可能带缩进）
            if (const std::size_t dot_active = line.find(".ActiveCfg");
                dot_active != std::string::npos)
            {
                const std::size_t first = line.find_first_not_of(" 	");
                if (first == std::string::npos || line[first] != '{'
                    || first >= dot_active)
                {
                    continue;
                }
                const std::size_t brace = line.find('}');
                const std::size_t eq = line.find('=');
                if (brace == std::string::npos || brace >= dot_active
                    || eq == std::string::npos || eq < dot_active)
                {
                    continue;
                }
                const std::string guid = line.substr(first, brace + 1 - first);
                const std::string sol = cg::util::trim(
                    line.substr(brace + 2, dot_active - brace - 2));
                const std::string proj = cg::util::trim(line.substr(eq + 1));
                if (!sol.empty() && !proj.empty())
                {
                    cfg_map.emplace_back(guid, sol, proj);
                }
                continue;
            }
            // {GUID}.SolutionCfg.Build.0 = ProjectCfg——该配置下工程真
            // 参与构建；无此行的工程被加载但不编。
            if (const std::size_t dot_build = line.find(".Build.0");
                dot_build != std::string::npos)
            {
                const std::size_t first = line.find_first_not_of(" 	");
                if (first == std::string::npos || line[first] != '{'
                    || first >= dot_build)
                {
                    continue;
                }
                const std::size_t brace = line.find('}');
                const std::size_t eq = line.find('=');
                if (brace == std::string::npos || brace >= dot_build
                    || eq == std::string::npos || eq < dot_build)
                {
                    continue;
                }
                const std::string sol = cg::util::trim(
                    line.substr(brace + 2, dot_build - brace - 2));
                if (sol.empty() || !cg::util::ascii_iequals(sol, want))
                {
                    continue;
                }
                std::string guid = line.substr(first, brace + 1 - first);
                guid.erase(
                    std::remove(guid.begin(), guid.end(), '{'),
                    guid.end());
                guid.erase(
                    std::remove(guid.begin(), guid.end(), '}'),
                    guid.end());
                build_flags.insert(cg::util::ascii_lower(guid));
            }
        }
        // 解析指定解决方案配置下的 ActiveCfg（大小写不敏感比较）与
        // Build.0 参与位（归一化 guid 对齐）。
        for (SlnProject& p : out)
        {
            for (const auto& [guid, sol, proj_cfg] : cfg_map)
            {
                if (guid != p.guid)
                {
                    continue;
                }
                if (cg::util::ascii_iequals(sol, want))
                {
                    p.active_cfg = proj_cfg;
                    break;
                }
            }
            std::string key = cg::util::ascii_lower(p.guid);
            key.erase(
                std::remove(key.begin(), key.end(), '{'),
                key.end());
            key.erase(
                std::remove(key.begin(), key.end(), '}'),
                key.end());
            p.build = build_flags.contains(key);
        }
        return out;
    }

    /**
     * @brief 一个工程解析到的 cl（诚实报告的载体）。
     *
     * @internal active_cfg/in_build 是 sln 输入下的成员披露事实
     * （enumerate_sln_projects 单点解析）；vcxproj 输入无 sln 配置映射
     * 概念，两字段保持默认（注册表写侧据此省略键）。
     */
    struct ProjectCl
    {
        std::string project; // 工程名（sln 条目名或 vcxproj 词干）
        std::string dir;     // 工程目录（绝对、正斜杠；cl 映射的前缀键）
        std::string cl;      // 绝对 cl.exe，失败时 ""
        std::string
            resolution; // override | project-property | vswhere-fallback | ""
        std::string active_cfg; ///< 成员 ActiveCfg（"Debug|x64"），vcxproj ""
        std::optional<bool> in_build; ///< Build.0 参与位；nullopt = 无此事实
    };

    /**
     * @brief 逐工程解析 cl：.vcxproj 单条目；.sln 按成员工程逐个求值。
     *
     * .sln 主路径：对每个成员 .vcxproj 跑带配置的 getProperty:VCToolsInstallDir
     * （成员自身的 ActiveCfg 配置，无映射回退解决方案配置对）——vswhere
     * 兜底降为成员级最后防线，sln 内的 VCToolsVersion 钉子不再被无视。
     * @internal 此前 .sln 永远走 vswhere"最高版本胜出"，混合工具集 sln 里
     * 钉 14.44 的工程被 14.51 覆盖（真编偷换编译器二进制的根因之一）。
     */
    [[nodiscard]]
    inline auto resolve_project_cls(
        const std::string& msbuild,
        const std::string& input,
        const std::string& configuration,
        const std::string& platform,
        const std::string& override_cl,
        const std::string& extra_args = {}) -> std::vector<ProjectCl>
    {
        std::vector<ProjectCl> out;
        const std::string      stem =
            std::filesystem::path{detail::fs_path_from_utf8(input)}
                .stem()
                .generic_string();
        const std::string dir =
            std::filesystem::path{detail::fs_path_from_utf8(input)}
                .parent_path()
                .generic_string();
        if (!detail::ascii_iequals(
                std::filesystem::path{input}.extension().string(),
                ".sln"))
        {
            const ClInfo info = find_cl(
                msbuild,
                input,
                configuration,
                platform,
                override_cl,
                extra_args);
            out.push_back(
                ProjectCl{
                    .project = stem,
                    .dir = dir,
                    .cl = info.path,
                    .resolution = info.resolution});
            return out;
        }
        const std::string solution_cfg =
            std::format("{}|{}", configuration, platform);
        for (const SlnProject& member :
             enumerate_sln_projects(input, solution_cfg))
        {
            std::string cfg = configuration;
            std::string plat = platform;
            if (const std::optional<ConfigPlatform> mapped =
                    detail::split_config_platform(member.active_cfg))
            {
                cfg = mapped->configuration;
                plat = mapped->platform;
            }
            const ClInfo info = find_cl(
                msbuild,
                member.path,
                cfg,
                plat,
                override_cl,
                extra_args);
            ProjectCl e;
            e.project = member.name;
            e.dir =
                std::filesystem::path{detail::fs_path_from_utf8(member.path)}
                    .parent_path()
                    .generic_string();
            e.cl = info.path;
            e.resolution = info.resolution;
            e.active_cfg = member.active_cfg;
            e.in_build = member.build;
            out.push_back(std::move(e));
        }
        if (out.empty())
        {
            // 无 VC 成员（或解不出）：退化为 sln 级 vswhere 兜底单条目。
            const ClInfo info =
                find_cl(msbuild, input, "", platform, override_cl, extra_args);
            out.push_back(
                ProjectCl{
                    .project = stem,
                    .dir = dir,
                    .cl = info.path,
                    .resolution = info.resolution});
        }
        return out;
    }

    /**
     * @brief CCG_CLWRAP_CL 前缀映射文本（"`<项目目录>`\t`<cl>`" 行，'\n'
     * 连接）。
     *
     * 解析失败的成员跳过（wrap 侧未命中时回退全局 real_cl）。
     */
    [[nodiscard]]
    inline auto build_cl_map(const std::vector<ProjectCl>& projects)
        -> std::string
    {
        std::string text;
        for (const ProjectCl& p : projects)
        {
            if (p.dir.empty() || p.cl.empty())
            {
                continue;
            }
            text += std::format("{}\t{}\n", p.dir, p.cl);
        }
        return text;
    }

    /**
     * @brief 路径里的 MSVC 工具集根目录（`…/VC/Tools/MSVC/<a.b.c.d>`）。
     *
     * 兼容 cl.exe 路径与 include 目录两种形态；无工具集段时返回 ""。
     * 工具集提示（自构建）从逐 TU 的 INCLUDE 物化目录推导时使用。
     */
    [[nodiscard]]
    inline auto msvc_toolset_root_of(std::string_view path) -> std::string
    {
        const std::string          forward = cg::util::to_forward_slashes(path);
        const std::string          lower = cg::util::ascii_lower(forward);
        constexpr std::string_view marker{"/vc/tools/msvc/"};
        const std::size_t          at = lower.find(marker);
        if (at == std::string::npos)
        {
            return {};
        }
        const std::size_t begin = at + marker.size();
        const std::size_t end = forward.find('/', begin);
        return end == std::string::npos ? forward : forward.substr(0, end);
    }

    /** @brief 沙箱泄漏扫描的结果（分类后的新增文件）。 */
    struct SandboxDiff
    {
        std::vector<std::string> leaks;             // 真实树新增的构建产物
        std::vector<std::string> generated_sources; // 生成器声明的源码类输出
        // 沙盒外但属生成器正常产物的文件（MIDL 的 tlb/生成头、CustomBuild
        // 生成物）：记录模式生成器真跑、产物按工程声明落真树，info 级汇总
        // 即可，不是泄漏。
        std::vector<std::string> generator_products;
    };

    /** @brief 树快照的一格：相对路径 + 身份（字节数与修改时间）。 */
    struct TreeFileState
    {
        std::string    rel; // 相对 root、正斜杠
        std::uintmax_t size = 0;
        std::int64_t   mtime_ns = 0;
    };

    /**
     * @brief 递归快照 root 下常规文件的身份（路径 + 字节数 + 修改时间，
     *        按 rel 序）。
     *
     * 扫描跳过 .ccg（ccg 布局：沙箱、捕获日志、桩工具都在里面）与 .git；
     * exclude 下（含子树）也跳过（-o 布局覆盖在项目树内时排除沙箱所在
     * 布局根）。目录不可读的枝干跳过（尽力而为——泄漏告警宁可少报分支
     * 不误报）。多记的身份让泄漏判定看得见"被改写"——构建后步骤把桩产
     * 物复制到工程树既有文件上时不产生新文件，只有 size/mtime 变化
     *（新增 diff 抓不到的形态）。
     */
    [[nodiscard]]
    inline auto snapshot_tree_states(
        const std::string&              root,
        const std::vector<std::string>& exclude) -> std::vector<TreeFileState>
    {
        namespace fs = std::filesystem;
        std::vector<TreeFileState> out;
        const auto                 under_any =
            [](const fs::path& abs, const std::vector<std::string>& roots)
        {
            for (const std::string& r : roots)
            {
                if (r.empty())
                {
                    continue;
                }
                std::error_code ec;
                const fs::path  rel = fs::relative(abs, fs::path{r}, ec);
                if (!ec && !rel.empty() && rel != ".." && *rel.begin() != "..")
                {
                    return true;
                }
            }
            return false;
        };
        const fs::path  root_fs = detail::fs_path_from_utf8(root);
        std::error_code top_ec;
        if (!fs::is_directory(root_fs, top_ec))
        {
            return out;
        }
        std::vector<fs::path> stack{root_fs};
        while (!stack.empty())
        {
            const fs::path dir = stack.back();
            stack.pop_back();
            std::error_code iter_ec;
            for (const fs::directory_entry& entry :
                 fs::directory_iterator(dir, iter_ec))
            {
                std::error_code leaf_ec;
                if (entry.is_directory(leaf_ec))
                {
                    const std::string name =
                        entry.path().filename().generic_string();
                    if (name == ".ccg" || name == ".git")
                    {
                        continue;
                    }
                    if (under_any(entry.path(), exclude))
                    {
                        continue;
                    }
                    stack.push_back(entry.path());
                    continue;
                }
                if (!entry.is_regular_file(leaf_ec))
                {
                    continue;
                }
                std::error_code rel_ec;
                const fs::path  rel =
                    fs::relative(entry.path(), root_fs, rel_ec);
                if (rel_ec || rel.empty())
                {
                    continue;
                }
                TreeFileState state;
                state.rel = rel.generic_string();
                std::error_code size_ec;
                state.size = entry.file_size(size_ec);
                if (size_ec)
                {
                    state.size = 0;
                }
                std::error_code          mtime_ec;
                const fs::file_time_type mtime =
                    entry.last_write_time(mtime_ec);
                state.mtime_ns = mtime_ec
                                     ? 0
                                     : static_cast<std::int64_t>(
                                           mtime.time_since_epoch().count());
                out.push_back(std::move(state));
            }
        }
        std::ranges::sort(
            out,
            [](const TreeFileState& a, const TreeFileState& b)
            { return a.rel < b.rel; });
        return out;
    }

    /**
     * @brief after 相对 before 新增的条目（两边均已按序）。
     *
     * 只看"新增"不看"存在"：Rebuild 的 Clean 会删真实树历史产物，删除与
     * 覆盖不是泄漏信号。
     */
    [[nodiscard]]
    inline auto new_files_since(
        const std::vector<std::string>& before,
        const std::vector<std::string>& after) -> std::vector<std::string>
    {
        std::vector<std::string> out;
        std::set_difference(
            after.begin(),
            after.end(),
            before.begin(),
            before.end(),
            std::back_inserter(out));
        return out;
    }

    /**
     * @brief 两边都在、但身份（字节数或修改时间）变了的条目。
     *
     * 真跑的生成器/构建后步骤把产物复制到工程树既有文件上时不产生新文
     * 件，只有改写——新增 diff 抓不到这一形态；身份变化把它纳入泄漏判定
     * 考量。声明过的生成器源码输出会再由 classify_new_tree_files 的白名
     * 单面剔除。
     */
    [[nodiscard]]
    inline auto overwritten_files_between(
        const std::vector<TreeFileState>& before,
        const std::vector<TreeFileState>& after) -> std::vector<std::string>
    {
        std::vector<std::string> out;
        std::size_t              i = 0;
        std::size_t              j = 0;
        while (i < before.size() && j < after.size())
        {
            if (before[i].rel < after[j].rel)
            {
                ++i;
            }
            else if (after[j].rel < before[i].rel)
            {
                ++j;
            }
            else
            {
                if (before[i].size != after[j].size
                    || before[i].mtime_ns != after[j].mtime_ns)
                {
                    out.push_back(after[j].rel);
                }
                ++i;
                ++j;
            }
        }
        return out;
    }

    namespace detail
    {

        /**
         * @brief 索引输入类源码扩展名测试（生成器白名单的准入面）。
         *
         * @internal 判定收编至 util 的扩展分类单点（is_indexable_source_file
         * ——含 .tlh/.tli 生成头、不含 .inc，集合与折叠规则只此一处）。
         */
        [[nodiscard]]
        inline auto has_generated_source_ext(std::string_view name) -> bool
        {
            return cg::util::is_indexable_source_file(name);
        }

    } // namespace detail

    /**
     * @brief 解析工程里自定义生成步骤声明的输出（vcxproj 的 CustomBuild
     * Outputs；.sln 输入枚举成员逐个解析）。
     *
     * 面向行的文本扫描：CustomBuild Include 条目携带 %(RootDir)%(Directory)
     * 上下文（生成物与被生成物同目录的常见形态）；$(MSBuildProjectDirectory)
     * /$(ProjectDir) 展开为工程目录；其余不可展开宏（$(Configuration) 等）
     * 的条目跳过——白名单宁可窄，泄漏告警绝不静默吞掉生成物。
     */
    [[nodiscard]]
    inline auto generator_declared_source_outputs(const std::string& input)
        -> std::vector<std::string>
    {
        std::vector<std::string> out;
        std::vector<std::string> projects;
        if (detail::ascii_iequals(
                std::filesystem::path{input}.extension().string(),
                ".sln"))
        {
            for (const SlnProject& member : enumerate_sln_projects(input, ""))
            {
                projects.push_back(member.path);
            }
        }
        else
        {
            projects.push_back(input);
        }
        const auto parse_one = [&out](const std::string& project_path)
        {
            const std::optional<std::string> text =
                detail::read_text_file(project_path);
            if (!text)
            {
                return;
            }
            const std::string project_dir =
                std::filesystem::path{detail::fs_path_from_utf8(project_path)}
                    .parent_path()
                    .generic_string();
            std::istringstream lines{*text};
            std::string        line;
            std::string        current_item; // 最近的 CustomBuild Include 条目
            while (std::getline(lines, line))
            {
                if (const std::size_t at = line.find("<CustomBuild Include=\"");
                    at != std::string::npos)
                {
                    const std::size_t begin = at + 22;
                    const std::size_t end = line.find('"', begin);
                    if (end != std::string::npos && end > begin)
                    {
                        current_item = line.substr(begin, end - begin);
                    }
                    continue;
                }
                const std::size_t outs = line.find("<Outputs>");
                if (outs == std::string::npos)
                {
                    continue;
                }
                const std::size_t begin = outs + 9;
                const std::size_t end = line.find("</Outputs>", begin);
                if (end == std::string::npos || end <= begin)
                {
                    continue;
                }
                std::string values = line.substr(begin, end - begin);
                // 逐 ';' 条目展开宏后相对工程目录（或条目同目录）解析。
                for (const std::string_view raw_entry :
                     cg::util::split(values, ';'))
                {
                    std::string entry = cg::util::trim(raw_entry);
                    if (!entry.empty() && entry.front() == '"'
                        && entry.back() == '"' && entry.size() >= 2)
                    {
                        entry = entry.substr(1, entry.size() - 2);
                    }
                    if (!entry.empty() && entry.find("$(") == std::string::npos)
                    {
                        std::string base_dir = project_dir;
                        // %(RootDir)%(Directory) → 条目所在目录。
                        const std::size_t rd =
                            entry.find("%(RootDir)%(Directory)");
                        if (rd != std::string::npos)
                        {
                            const std::string item =
                                cg::util::to_forward_slashes(current_item);
                            const std::string item_dir =
                                std::filesystem::path{item}
                                    .parent_path()
                                    .generic_string();
                            entry = entry.substr(0, rd) + entry.substr(rd + 22);
                            if (!item_dir.empty())
                            {
                                base_dir = item_dir;
                            }
                        }
                        const std::string abs =
                            (std::filesystem::path{base_dir} / entry)
                                .lexically_normal()
                                .generic_string();
                        cg::util::push_unique(out, abs);
                    }
                }
            }
        };
        for (const std::string& p : projects)
        {
            parse_one(p);
        }
        return out;
    }

    /**
     * @brief 新增文件是否是 MIDL 生成器的正常产物形态。
     *
     * midl 在记录模式真跑（生成头是编译输入，桩化会破坏 TU 集合），其
     * 产物按工程声明落在真实树——.tlb 类型库、_i.c/_p.c/dlldata.c 接口/
     * 代理存根、以及同名 .idl 在场时的生成头。这些不是泄漏：判定按形态
     * 白名单（后缀特征 + 同目录同名 .idl 的存在性证明），其余任何后缀的
     * 新增仍走泄漏通道。
     */
    [[nodiscard]]
    inline auto is_midl_product(const std::string& root, const std::string& rel)
        -> bool
    {
        const std::string lower = cg::util::ascii_lower(rel);
        const auto        ends = [&lower](std::string_view s)
        {
            return lower.size() >= s.size()
                   && lower.substr(lower.size() - s.size()) == s;
        };
        if (ends(".tlb") || ends("_i.c") || ends("_p.c") || ends("dlldata.c"))
        {
            return true;
        }
        if (!ends(".h"))
        {
            return false;
        }
        // 生成头判定取目录级证据（同目录存在任一 .idl）：MIDL 的 /h 输出
        // 名不必与输入同名（vcxproj 的 HeaderFileName 可任意起名，实测
        // app.idl → app_gen.h），同名要求会漏；目录里有 .idl 时该目录的
        // 新增 .h 按 MIDL 生成头归类（降级 info 而非泄漏，不丢数据）。
        const std::filesystem::path p =
            std::filesystem::path{detail::fs_path_from_utf8(root)}
            / detail::fs_path_from_utf8(rel);
        std::error_code ec;
        for (const std::filesystem::directory_entry& e :
             std::filesystem::directory_iterator(p.parent_path(), ec))
        {
            if (e.path().extension().generic_string() == ".idl")
            {
                return true;
            }
        }
        return false;
    }

    /**
     * @brief 把新增文件分类为泄漏、生成器源码输出或生成器正常产物。
     *
     * fresh 是相对项目根的新增路径；declared 是生成器声明的绝对输出路径。
     * 规则：声明的输出折成相对根后与新增条目命中（大小写不敏感——
     * Windows 文件系统）且是索引输入类源码 → generated_sources（它是 TU
     * 源，落源码树语义正确）；声明命中但非源码类（CustomBuild 的 .bin 等
     * 生成物）或 MIDL 产物形态（tlb/生成头）→ generator_products（记录模
     * 式生成器真跑的预期落点，info 级汇总）；其余新增文件一律 leaks。已知
     * 构建后缀只是提示面，不缩小泄漏判定：任意后缀的真实树新增都可能污染
     * 源码树。
     */
    [[nodiscard]]
    inline auto classify_new_tree_files(
        const std::string&              root,
        const std::vector<std::string>& fresh,
        const std::vector<std::string>& declared) -> SandboxDiff
    {
        const auto lower_of = [](std::string_view s)
        { return cg::util::ascii_lower(s); };
        // 声明输出折成相对根拼写（根外的声明——硬编码绝对路径——保留
        // 原样，永不命中相对新增面，照实计入泄漏通道）。
        std::vector<std::string> declared_rel;
        for (const std::string& d : declared)
        {
            std::error_code             ec;
            const std::filesystem::path rel = std::filesystem::relative(
                std::filesystem::path{detail::fs_path_from_utf8(d)},
                std::filesystem::path{detail::fs_path_from_utf8(root)},
                ec);
            if (!ec && !rel.empty() && *rel.begin() != "..")
            {
                declared_rel.push_back(lower_of(rel.generic_string()));
            }
        }
        std::ranges::sort(declared_rel);
        SandboxDiff out;
        for (const std::string& f : fresh)
        {
            const std::string f_lower = lower_of(f);
            const bool        declared_hit =
                std::ranges::binary_search(declared_rel, f_lower);
            if (declared_hit && detail::has_generated_source_ext(f))
            {
                out.generated_sources.push_back(f);
            }
            else if (declared_hit || is_midl_product(root, f))
            {
                out.generator_products.push_back(f);
            }
            else
            {
                out.leaks.push_back(f);
            }
        }
        return out;
    }

    namespace detail
    {

        /**
         * @brief 一个桩角色的输出旗标表（泄漏金丝雀内部用）。
         *
         * @internal 底层数组必须是静态寿命——局部 std::initializer_list 变量
         * 赋值后其底层数组在语句结束即亡，随后遍历是悬垂未定义行为（实测段
         * 错误），故角色表用 inline constexpr 数组 + span 引用。
         */
        struct OutputFlags
        {
            std::span<const std::string_view> joined;
            std::span<const std::string_view> separate;
            bool                              strip_resource_id = false;
        };

        inline constexpr std::string_view cl_out_joined[] = {
            "/Fo",
            "/Fp",
            "/Fd"};
        inline constexpr std::string_view cl_out_separate[] = {
            "/Fo",
            "/Fp",
            "/Fd"};
        inline constexpr std::string_view link_out_joined[] =
            {"/OUT:", "/PDB:", "/IMPLIB:", "/MAP:"};
        inline constexpr std::string_view link_out_separate[] =
            {"/OUT", "/PDB", "/IMPLIB", "/MAP"};
        inline constexpr std::string_view lib_out_joined[] = {"/OUT:"};
        inline constexpr std::string_view lib_out_separate[] = {"/OUT"};
        inline constexpr std::string_view rc_out_joined[] = {"/fo"};
        inline constexpr std::string_view rc_out_separate[] = {"/fo"};
        inline constexpr std::string_view mt_out_joined[] =
            {"/out:", "/fo:", "/outputresource:"};
        inline constexpr std::string_view mt_out_separate[] = {"/out", "/fo"};
        inline constexpr std::string_view masm_out_joined[] = {"/Fo"};
        inline constexpr std::string_view masm_out_separate[] = {"/Fo"};
        inline constexpr std::string_view bsc_out_joined[] = {"/o", "/Of"};
        inline constexpr std::string_view bsc_out_separate[] = {"/o", "/Of"};
        inline constexpr std::string_view xdc_out_joined[] = {"/Fo:", "/Fo"};
        inline constexpr std::string_view xdc_out_separate[] = {"/Fo"};

        /** @brief 角色到输出旗标表；家族外（unknown）无表。 */
        [[nodiscard]]
        inline auto output_flags_of(StubRole role) -> std::optional<OutputFlags>
        {
            switch (role)
            {
            case StubRole::cl:
                return OutputFlags{
                    std::span{cl_out_joined},
                    std::span{cl_out_separate}};
            case StubRole::link:
                return OutputFlags{
                    std::span{link_out_joined},
                    std::span{link_out_separate}};
            case StubRole::lib:
                return OutputFlags{
                    std::span{lib_out_joined},
                    std::span{lib_out_separate}};
            case StubRole::rc:
                return OutputFlags{
                    std::span{rc_out_joined},
                    std::span{rc_out_separate}};
            case StubRole::mt:
                return OutputFlags{
                    std::span{mt_out_joined},
                    std::span{mt_out_separate},
                    true};
            case StubRole::masm:
                return OutputFlags{
                    std::span{masm_out_joined},
                    std::span{masm_out_separate}};
            case StubRole::bscmake:
                return OutputFlags{
                    std::span{bsc_out_joined},
                    std::span{bsc_out_separate}};
            case StubRole::xdcmake:
                return OutputFlags{
                    std::span{xdc_out_joined},
                    std::span{xdc_out_separate}};
            case StubRole::unknown:
                return std::nullopt;
            }
            return std::nullopt;
        }

        /**
         * @brief 泄漏金丝雀内部用：argv 里带值旗标的值收集（连写前缀与分写两
         * 形）。
         *
         * 与适配器同一立场：非规范拼写不重解释，同名 flag 出现几次收几条
         *（金丝雀只看路径不挑最后一条）；分写形式的值 token 以 '/'、'-' 开头
         * 时视为旗标而非值；值首尾引号由 to_abs 的清洗剥去。
         */
        [[nodiscard]]
        inline auto output_flag_values(
            const std::vector<std::string>&   argv,
            std::span<const std::string_view> joined,
            std::span<const std::string_view> separate)
            -> std::vector<std::string>
        {
            std::vector<std::string> out;
            const std::size_t        n = argv.size();
            for (std::size_t i = 1; i < n; ++i)
            {
                const std::string& a = argv[i];
                bool               matched = false;
                for (const std::string_view p : joined)
                {
                    if (a.starts_with(p) && a.size() > p.size())
                    {
                        out.push_back(a.substr(p.size()));
                        matched = true;
                        break;
                    }
                }
                if (!matched)
                {
                    for (const std::string_view p : separate)
                    {
                        if (a == p && i + 1 < n && !argv[i + 1].empty()
                            && argv[i + 1][0] != '/' && argv[i + 1][0] != '-')
                        {
                            out.push_back(argv[i + 1]);
                            ++i;
                            break;
                        }
                    }
                }
            }
            return out;
        }

    } // namespace detail

    /**
     * @brief 记录模式金丝雀的输入面：捕获 argv 里字面声明的全部输出路径。
     *
     * 逐调用按 argv[0] 基名取桩角色（与写侧家族表同一词汇），收该角色输
     * 出旗标的值（cl 的 /Fo /Fp /Fd、link 的 /OUT /PDB /IMPLIB /MAP、
     * lib 的 /OUT、rc 的 /fo、mt 的 /out /fo /outputresource（";#N" 资源
     * id 截断）、masm/xdcmake 的 /Fo、bscmake 的 /o /Of），按记录 cwd 绝
     * 对化、保序去重。刻意不做任何缺省名推导（no-op 语义下替身零写盘，
     * 产物推导机器已删）——这份清单的唯一消费者是泄漏金丝雀
     * scan_declared_output_leaks：树快照差集只看得见解决方案根目录内的
     * 新增/改写，argv 声明的输出落在根外时静默逃逸（实测）。
     */
    [[nodiscard]]
    inline auto capture_declared_outputs(const WrapLog& capture)
        -> std::vector<std::string>
    {
        std::vector<std::string> out;
        for (const ClInvocation& inv : capture.invocations)
        {
            if (inv.argv_utf8.empty())
            {
                continue;
            }
            const std::optional<detail::OutputFlags> flags =
                detail::output_flags_of(
                    detail::stub_role_of_basename(
                        detail::path_basename_lower(inv.argv_utf8[0])));
            if (!flags)
            {
                continue;
            }
            for (std::string v : detail::output_flag_values(
                     inv.argv_utf8,
                     flags->joined,
                     flags->separate))
            {
                if (flags->strip_resource_id)
                {
                    const std::size_t semi = v.find(';');
                    if (semi != std::string::npos)
                    {
                        v.resize(semi);
                    }
                }
                if (v.find_first_not_of('"') == std::string::npos)
                {
                    continue; // 空值/纯引号不是路径
                }
                out.push_back(detail::to_abs(v, inv.cwd));
            }
        }
        std::vector<std::string> unique;
        for (std::string& p : out)
        {
            cg::util::push_unique(unique, std::move(p));
        }
        return unique;
    }

    /**
     * @brief 泄漏扫描扩面（金丝雀）：argv 声明的输出路径在沙盒与 .ccg 之
     *        外发生新建/改写时的具名报告。
     *
     * 树快照差集（classify_new_tree_files / overwritten_files_between 的
     * 调用面）只覆盖解决方案根目录内的新增/改写；argv 声明的输出落在根外
     * 时静默逃逸（实测）。本函数把 declared 的每条路径与跑前状态比对：
     * 树内路径用跑前快照判"新增/改写"；树外路径快照无从覆盖，退回运行窗
     * 口判定——跑后仍存在且修改时间落在运行窗口内
     * （不早于 run_start 减文件系统时钟粒度容差）即视为本轮新建/改写。exempt
     * 是沙盒与 .ccg 布局等豁免根（含子树）； already_reported
     * 是树差集已报告路径的小写绝对拼写，命中跳过（同一 路径不双报）。no-op
     * 语义下替身零写盘，本函数理论零触发——存在的
     * 意义是把"零写盘被破坏"第一时间叫出来。
     */
    [[nodiscard]]
    inline auto scan_declared_output_leaks(
        const std::vector<std::string>&        declared,
        const std::vector<TreeFileState>&      tree_before,
        const std::string&                     root,
        const std::vector<std::string>&        exempt,
        const std::filesystem::file_time_type& run_start,
        const std::vector<std::string>&        already_reported)
        -> std::vector<std::string>
    {
        const auto lower_of = [](std::string_view s)
        { return cg::util::ascii_lower(s); };
        const auto under = [](const std::string& p, const std::string& b)
        {
            if (b.empty())
            {
                return false;
            }
            return p == b
                   || (p.size() > b.size() && p.compare(0, b.size(), b) == 0
                       && p[b.size()] == '/');
        };
        std::string root_l = lower_of(root);
        while (root_l.size() > 3 && root_l.back() == '/')
        {
            root_l.pop_back();
        }
        std::vector<std::string> exempt_l;
        for (const std::string& e : exempt)
        {
            std::string l = lower_of(e);
            while (l.size() > 3 && l.back() == '/')
            {
                l.pop_back();
            }
            if (!l.empty())
            {
                exempt_l.push_back(std::move(l));
            }
        }
        // 文件系统时间戳比系统钟滞后（Windows 的时间片插值粒度，实测可
        // 达数百微秒）——窗口判定带 2 秒容差，避免"跑后新建的文件被判成
        // 跑前"的漏报；陈旧文件的年龄远大于容差，不误报。容差必须用
        // std::chrono 时长算（file_clock 的 tick 周期平台各异，MSVC 是
        // 100ns 一档，按纳秒硬编码会把容差放大百倍）。
        const std::int64_t window_start_count =
            (run_start - std::chrono::seconds{2}).time_since_epoch().count();
        const auto state_of = [](const std::string& path,
                                 bool&              exists,
                                 std::uintmax_t&    size,
                                 std::int64_t&      mtime_ns)
        {
            std::error_code                       ec;
            const std::filesystem::file_time_type mtime =
                std::filesystem::last_write_time(
                    detail::fs_path_from_utf8(path),
                    ec);
            if (ec)
            {
                exists = false;
                return;
            }
            exists = true;
            mtime_ns =
                static_cast<std::int64_t>(mtime.time_since_epoch().count());
            std::error_code size_ec;
            size = std::filesystem::file_size(
                detail::fs_path_from_utf8(path),
                size_ec);
            if (size_ec)
            {
                size = 0;
            }
        };
        std::vector<std::string> leaks;
        for (const std::string& path : declared)
        {
            const std::string path_l = lower_of(path);
            if (std::ranges::find(already_reported, path_l)
                != already_reported.end())
            {
                continue;
            }
            bool exempt_hit = false;
            for (const std::string& e : exempt_l)
            {
                if (under(path_l, e))
                {
                    exempt_hit = true;
                    break;
                }
            }
            if (exempt_hit)
            {
                continue;
            }
            if (under(path_l, root_l))
            {
                // 树内：与跑前快照精确比对（新增/改写）。
                const std::string    rel_l = path_l.substr(root_l.size() + 1);
                const TreeFileState* before = nullptr;
                for (const TreeFileState& s : tree_before)
                {
                    if (lower_of(s.rel) == rel_l)
                    {
                        before = &s;
                        break;
                    }
                }
                bool           exists = false;
                std::uintmax_t size = 0;
                std::int64_t   mtime_ns = 0;
                state_of(path, exists, size, mtime_ns);
                if (!exists)
                {
                    // 缺失文件不被创建正是 no-op 的预期，不是泄漏。
                    continue;
                }
                if (before == nullptr || before->size != size
                    || before->mtime_ns != mtime_ns)
                {
                    leaks.push_back(path);
                }
            }
            else
            {
                // 树外：快照无从覆盖，退回运行窗口判定（修改时间落在
                // 运行窗口内 = 本轮新建/改写；陈旧文件不误报）。
                bool           exists = false;
                std::uintmax_t size = 0;
                std::int64_t   mtime_ns = 0;
                state_of(path, exists, size, mtime_ns);
                if (exists && mtime_ns >= window_start_count)
                {
                    leaks.push_back(path);
                }
            }
        }
        return leaks;
    }

    namespace detail
    {

        /**
         * @brief 一次抽取运行的捕获 token 路径值：CCG_CLWRAP_LOG 指向
         * clwrap-`<纳秒>`.jsonl。
         *
         * @internal
         * jsonl 文件本身已退役（捕获数据落推导出的 capture.db）：该路径值如今
         * 只承担两个角色——wrap 模式的触发契约（env 已设 = wrap 进程）与
         * capture.db 的推导锚点（写读两侧经同一函数映射）。逐运行 token 保留：
         * 它进入 capture 表的 token 列，标记行属于哪次运行。历史注记：旧
         * jsonl 时代的逐 token 名是为躲 MSBuild 文件追踪器的 tlog 记账（固定
         * 名会被下一次 -t:Rebuild 的 Clean 中途删除）；capture.db 固定名在
         * 记录模式下由沙箱化的 IntDir 保护（tlog 一轮一清），真编模式仍可能
         * 被多项目 Clean 命中——运行前的 prune 先删掉它，读侧只看本次行。
         */
        [[nodiscard]]
        inline auto new_capture_log_path(const std::string& dir) -> std::string
        {
            const auto ticks =
                std::chrono::system_clock::now().time_since_epoch();
            return std::format("{}/clwrap-{}.jsonl", dir, ticks.count());
        }

        /**
         * @brief 清除先前运行遗留的捕获存储（崩溃或中断的运行会留下一个；
         * 新运行绝不读它们）。
         *
         * @internal 两件事：删除推导出的 capture.db（含崩溃残留的 -wal/-shm
         * 边车——读侧另有 token 过滤纵深防御，双保险下旧行绝不混进本次捕
         * 获）与清理旧版本写入的 clwrap-*.jsonl 存量（迁移清理，幂等）。
         * compile_commands.json 与逐配置缓存目录中的其他内容一律不动。删除
         * 失败（句柄占用/ACL 等）告警一行而非静默：读侧的 token 过滤会兜住
         * 正确性，但该信号值得看见。
         */
        inline void prune_capture_logs(const std::string& dir)
        {
            const auto remove_loud = [](const std::string& target)
            {
                std::error_code rm_ec;
                std::filesystem::remove(fs_path_from_utf8(target), rm_ec);
                if (rm_ec)
                {
                    std::error_code probe;
                    if (std::filesystem::exists(
                            fs_path_from_utf8(target),
                            probe))
                    {
                        std::println(
                            std::cerr,
                            "ccg msbuild: cannot remove stale capture "
                            "artifact {}: {}",
                            target,
                            rm_ec.message());
                    }
                }
            };
            std::error_code ec;
            for (const std::filesystem::directory_entry& entry :
                 std::filesystem::directory_iterator(
                     fs_path_from_utf8(dir),
                     ec))
            {
                std::error_code leaf_ec;
                if (!entry.is_regular_file(leaf_ec))
                {
                    continue;
                }
                const std::string name =
                    entry.path().filename().generic_string();
                if (name.starts_with("clwrap-") && name.ends_with(".jsonl"))
                {
                    remove_loud(entry.path().generic_string());
                }
            }
            const std::string db = capture_db_of_log_path(dir);
            remove_loud(db);
            remove_loud(db + "-wal");
            remove_loud(db + "-shm");
        }

    } // namespace detail

    namespace detail
    {

        /**
         * @brief 构建后步骤失败行判形（记录模式滤噪）。
         *
         * MSB3073（MSBuild 对 exec 任务 "exited with code" 的包装错误）与
         * VCEnd/ exec 任务直接报出的 "exited with code" 行；大小写不敏感。
         */
        [[nodiscard]]
        inline auto is_post_build_failure_line(std::string_view line) -> bool
        {
            const std::string lower = cg::util::ascii_lower(line);
            return lower.find("error msb3073") != std::string::npos
                   || lower.find("exited with code") != std::string::npos;
        }

    } // namespace detail

    /**
     * @brief 一次被包装 Rebuild 的运行结果。
     *
     * @internal
     * out/err 是构建子进程两路流的逐字节累积——tee 语义（每路一条排水
     * 线程边转发到本进程对应流边收集；两路必须并发排水，子进程先灌满一
     * 路管道再等另一路 EOF 时顺序排水会死锁，run_capture 审计同款教训）。
     * 退出码逐字返回（MSBuild 失败是调用方诚实报告的输入，不是 wrap 层
     * 错误）；文本供调用方把 MSB3073/exec 类构建后步骤失败归类为记录模
     * 式的预期信息。记录模式下 MSB3073/VCEnd/exec "exited with code" 类
     * 错误行不透传刷屏（suppressed_post_build_lines 计数，结尾一行汇总）
     * ——累积全文不受过滤影响。
     */
    struct ExtractRunResult
    {
        int         exit_code = -1;
        std::string out; // 子进程 stdout（逐字节累积）
        std::string err; // 子进程 stderr（逐字节累积）
        // 记录模式被滤噪的构建后步骤失败行数（两路合计）。
        std::int64_t suppressed_post_build_lines = 0;
    };

    /**
     * @brief 运行被包装的 Rebuild：ToolPath 家族指向桩目录（记录模式）或
     * CLToolExe/CLToolPath 指向运行中的 ccg.exe（真编）、为每个子工具调用
     * 设好 wrap 模式环境变量对。
     *
     * 构建输出经管道 tee 给用户（流式转发保住既有可见性）并逐字节累积返
     * 回。wrap 日志的父目录先创建，使第一个 cl 调用就能追加。记录模式追
     * 加注入：CCG_CLWRAP_STUB=1（桩化零写盘）、CCG_CLWRAP_CL 前缀映射
     * （混合工具集逐工程选 cl）、PATH 前置桩目录（MASM 等经裸名解析的工
     * 具与不认 ToolPath 的调用面由此接管——桩目录只含家族名副本，不会遮
     * 蔽任何其他可执行）。
     */
    [[nodiscard]]
    inline auto run_extract(const ExtractParams& p) -> ExtractRunResult
    {
        const std::filesystem::path log_fs =
            detail::fs_path_from_utf8(p.log_path);
        std::error_code ec;
        std::filesystem::create_directories(log_fs.parent_path(), ec);

        // 预建 capture.db（单一连接 WAL + 幂等 DDL）：库与表先于一切写者
        // 存在，桩进程并发冷启动不再落入"多进程同窗 CREATE 新库 + 切
        // WAL"的竞态窗口（WAL 切换遇其它活动连接直接回 SQLITE_BUSY，曾
        // 由此静默丢整 invocation 行）。写者仍有自建兜底，预建失败只告警
        // （capture_db_ensure）。
        detail::capture_db_ensure(detail::capture_db_of_log_path(p.log_path));

        std::wstring cmdline = cg::util::platform::quote_for_command_line(
            cg::util::platform::widen_utf8(p.msbuild));
        for (const std::string& arg : build_extract_args(p))
        {
            cmdline += L' ';
            cmdline += cg::util::platform::quote_for_command_line(
                cg::util::platform::widen_utf8(arg));
        }
        if (!p.extra_args.empty())
        {
            cmdline += L' ';
            cmdline += cg::util::platform::widen_utf8(p.extra_args); // 逐字拼接
        }

        std::vector<std::pair<std::wstring, std::wstring>> overrides{
            {L"CCG_CLWRAP_LOG", cg::util::platform::widen_utf8(p.log_path)},
            {L"CCG_CLWRAP_REAL_CL", cg::util::platform::widen_utf8(p.real_cl)}};
        if (!p.cl_map.empty())
        {
            overrides.emplace_back(
                L"CCG_CLWRAP_CL",
                cg::util::platform::widen_utf8(p.cl_map));
        }
        if (p.mode == ExtractMode::Record)
        {
            overrides.emplace_back(L"CCG_CLWRAP_STUB", L"1");
            std::wstring path_prefix =
                cg::util::platform::widen_utf8(p.stub_tool_dir);
            if (const std::optional<std::wstring> current =
                    detail::env_wide(L"PATH"))
            {
                path_prefix += L";";
                path_prefix += *current;
            }
            overrides.emplace_back(L"PATH", std::move(path_prefix));
        }
        // 构建子树专用净化：NoDefaultCurrentDirectoryInExePath（部分 shell/
        // 启动器会设）会让子进程按裸名解析 exe 时不再搜索当前目录，定制
        // 构建步骤常依赖该行为而失败。仅从本构建子树剔除——ccg 进程自身
        // 与 find_msbuild/find_cl 的探测子进程不受影响。
        std::wstring env_block = detail::build_child_env(
            overrides,
            {L"NoDefaultCurrentDirectoryInExePath"});

        // 每路一条排水钩子（挂进捕获原语）：逐字节转发到本进程对应流
        // （构建输出保持流式可见），累积由原语完成、钩子只见增量；转发
        // 失败（下游已关闭）不致命——累积继续。记录模式滤噪：MSB3073/
        // VCEnd/exec "exited with code" 类错误行不透传（替身零写盘，下游
        // 产物根本不存在，复制/校验步骤必然失败——按行缓冲判形、计数
        // 收尾汇总）；累积全文照收，退出码语义不变。真编模式逐字节透传
        // （构建输出的可见性是金标准对账的输入）。
        const bool   suppress_exec_failures = p.mode == ExtractMode::Record;
        std::int64_t suppressed_out = 0;
        std::int64_t suppressed_err = 0;
        const auto   make_tap =
            [&](std::ostream& forward, std::int64_t& suppressed)
        {
            // 行缓冲状态在 on_chunk 与 on_finish 间共享（残行判形在收尾）；
            // 转发流经指针捕获（std::cout/std::cerr 恒存活，且按值捕获
            // 引用形参在 MSVC 下会尝试复制流对象——删除的拷贝构造）。
            const auto          line = std::make_shared<std::string>();
            std::ostream* const target = &forward;
            return cg::util::platform::DrainTap{
                .on_chunk =
                    [target, suppress_exec_failures, &suppressed, line](
                        const char* data,
                        const DWORD size)
                {
                    if (!suppress_exec_failures)
                    {
                        target->write(data, static_cast<std::streamsize>(size));
                        return;
                    }
                    line->append(data, size);
                    std::size_t start = 0;
                    for (std::size_t nl = line->find('\n', start);
                         nl != std::string::npos;
                         nl = line->find('\n', start))
                    {
                        const std::string_view row{
                            line->data() + start,
                            nl - start + 1};
                        if (detail::is_post_build_failure_line(row))
                        {
                            ++suppressed;
                        }
                        else
                        {
                            target->write(
                                row.data(),
                                static_cast<std::streamsize>(row.size()));
                        }
                        start = nl + 1;
                    }
                    line->erase(0, start);
                },
                .on_finish =
                    [target, suppress_exec_failures, &suppressed, line]()
                {
                    // 流尾无换行的残行同样判形一次（绝不丢字节）。
                    if (suppress_exec_failures && !line->empty())
                    {
                        if (detail::is_post_build_failure_line(*line))
                        {
                            ++suppressed;
                        }
                        else
                        {
                            target->write(
                                line->data(),
                                static_cast<std::streamsize>(line->size()));
                        }
                    }
                    target->flush();
                },
            };
        };

        // 被包装 Rebuild 的原语配置：stdin 继续继承本进程的（与旧的无管道
        // 形态等价）；两路输出改管道；环境块经 CREATE_UNICODE_ENVIRONMENT
        // 定制（上面 build_child_env 的产物）；不设 CREATE_NO_WINDOW——构建
        // 子进程与本进程共享控制台是既有形态。
        cg::util::platform::ChildRunSpec spec;
        spec.cmdline = std::move(cmdline);
        spec.env_block = std::move(env_block);
        spec.stdin_mode = cg::util::platform::ChildStdin::ParentHandle;
        spec.create_no_window = false;
        spec.out_tap = make_tap(std::cout, suppressed_out);
        spec.err_tap = make_tap(std::cerr, suppressed_err);
        const cg::util::platform::ChildRunResult run =
            cg::util::platform::run_child(spec);
        if (!run.pipes_ok)
        {
            throw std::runtime_error(
                std::format(
                    "ccg msbuild: cannot create output pipes for MSBuild '{}'",
                    p.msbuild));
        }
        if (!run.started)
        {
            throw std::runtime_error(
                std::format(
                    "ccg msbuild: cannot start MSBuild '{}' (error {})",
                    p.msbuild,
                    run.spawn_error));
        }

        ExtractRunResult result;
        result.out = std::move(run.out);
        result.err = std::move(run.err);
        result.exit_code = run.exit_code;
        result.suppressed_post_build_lines = suppressed_out + suppressed_err;
        if (suppress_exec_failures && result.suppressed_post_build_lines > 0)
        {
            std::println(
                std::cerr,
                "ccg msbuild: note: {} post-build/exec failure line{} "
                "on empty record-mode product{} suppressed (expected "
                "in record mode; the full text is kept in the "
                "captured build output)",
                result.suppressed_post_build_lines,
                result.suppressed_post_build_lines == 1 ? "" : "s",
                result.suppressed_post_build_lines == 1 ? "" : "s");
        }
        return result;
    }

#endif // defined(_WIN32)

} // namespace cg::msbuild
