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
 * @module cg.msbuild.pipeline.hostfind
 *
 * @brief Windows 宿主发现域（_WIN32）：子进程捕获原语、MSBuild 与 cl
 * 的定位、解决方案项目枚举与项目级 cl 解析。
 */
export module cg.msbuild.pipeline.hostfind;

import std;
import cg.util;
import cg.util.cstd;
import cg.msbuild.translate; // ClInvocation / CdbEntry（wrap 日志载荷类型）
import cg.msbuild.wrap;      // detail::env_wide / fs_path_from_utf8
import sqlpp23.core;
import sqlpp23.sqlite3;
import cg.msbuild.pipeline.configs;
import cg.msbuild.pipeline.extractargs;

export namespace cg::msbuild
{
    namespace json = cg::util::json;
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
         * @brief 环境块条目 entry 是否以 "NAME=" 开头（名字比较不区分
         *        大小写）。
         *
         * @internal Windows 环境名语义如此——用户块里 PATH 常拼写为
         * Path，大小写敏感比较会让覆盖/删除双双漏配。
         */
        [[nodiscard]]
        inline auto env_entry_named(
            std::wstring_view entry,
            std::wstring_view name) -> bool
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
        }

        /**
         * @brief 被包装 Rebuild 的子环境块：当前块减去 wrap 变量（防御性——以
         * 防杂散值漏进我们自己的环境）与 remove 名单，加新鲜覆盖。
         *
         * @internal 双 NUL 结尾、CREATE_UNICODE_ENVIRONMENT 布局。remove 只
         * 从本子环境块剔除（构建子树专用；ccg 进程与探测子进程的环境不
         * 动）。
         */
        [[nodiscard]]
        inline auto build_child_env(
            const std::vector<std::pair<std::wstring, std::wstring>>& overrides,
            const std::vector<std::wstring>& remove = {}) -> std::wstring
        {
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
                        if (env_entry_named(view, name))
                        {
                            dropped = true;
                            break;
                        }
                    }
                    if (!dropped)
                    {
                        for (const std::wstring& name : remove)
                        {
                            if (env_entry_named(view, name))
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
        std::string name;          // sln 条目名（"projA"）
        std::string path;          // 绝对 .vcxproj（正斜杠）
        std::string guid;          // "{...}"
        std::string active_cfg;    // "Debug|x64"，无映射 ""
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
            key.erase(std::remove(key.begin(), key.end(), '{'), key.end());
            key.erase(std::remove(key.begin(), key.end(), '}'), key.end());
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

#endif // defined(_WIN32)
} // namespace cg::msbuild
