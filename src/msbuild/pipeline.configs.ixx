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
 * @module cg.msbuild.pipeline.configs
 *
 * @brief 输入配置域：Configuration|Platform 对、配置枚举、cl
 * 目标目录与工具目录路径、输出尾行的路径解析。
 */
export module cg.msbuild.pipeline.configs;

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

        /**
         * @brief 拆出 "C|P" 对并入列；同对已存在时跳过。
         */
        inline void push_config_unique(
            std::vector<ConfigPlatform>& out,
            const std::string&           pair)
        {
            const std::optional<ConfigPlatform> cp =
                split_config_platform(pair);
            if (!cp)
            {
                return;
            }
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

        /**
         * @brief 行文本是否形似绝对路径（盘符根或 UNC 前缀）。
         */
        [[nodiscard]]
        inline auto line_looks_like_path(std::string_view line) -> bool
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
                    detail::push_config_unique(
                        out,
                        cg::util::trim(line.substr(0, eq)));
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
                    detail::push_config_unique(
                        out,
                        line.substr(begin, end - begin));
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
        std::size_t start = 0;
        while (start <= output.size())
        {
            const std::size_t      end = output.find('\n', start);
            const std::string_view line = output.substr(
                start,
                end == std::string_view::npos ? std::string_view::npos
                                              : end - start);
            const std::string trimmed = cg::util::trim(line);
            if (!trimmed.empty() && detail::line_looks_like_path(trimmed))
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
} // namespace cg::msbuild
