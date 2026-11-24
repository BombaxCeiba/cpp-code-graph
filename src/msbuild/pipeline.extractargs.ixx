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
 * @module cg.msbuild.pipeline.extractargs
 *
 * @brief 被包装 Rebuild 的参数域：ExtractMode/ExtractParams 与 MSBuild
 * 命令行的纯文本构建器。
 */
export module cg.msbuild.pipeline.extractargs;

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
} // namespace cg::msbuild
