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
 * @module cg.msbuild.pipeline.outputs
 *
 * @brief 声明产物域（_WIN32）：各工具角色的输出旗标表、capture
 * 日志的声明产物提取与树外泄漏扫描。
 */
export module cg.msbuild.pipeline.outputs;

import std;
import cg.util;
import cg.util.cstd;
import cg.msbuild.translate; // ClInvocation / CdbEntry（wrap 日志载荷类型）
import cg.msbuild.wrap;      // detail::env_wide / fs_path_from_utf8
import sqlpp23.core;
import sqlpp23.sqlite3;
import cg.msbuild.pipeline.configs;
import cg.msbuild.pipeline.wraplog;
import cg.msbuild.pipeline.treestate;

export namespace cg::msbuild
{
    namespace json = cg::util::json;
#if defined(_WIN32)

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
        std::string root_l = cg::util::ascii_lower(root);
        while (root_l.size() > 3 && root_l.back() == '/')
        {
            root_l.pop_back();
        }
        std::vector<std::string> exempt_l;
        for (const std::string& e : exempt)
        {
            std::string l = cg::util::ascii_lower(e);
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
        std::vector<std::string> leaks;
        for (const std::string& path : declared)
        {
            const std::string path_l = cg::util::ascii_lower(path);
            if (std::ranges::find(already_reported, path_l)
                != already_reported.end())
            {
                continue;
            }
            bool exempt_hit = false;
            for (const std::string& e : exempt_l)
            {
                if (detail::normalized_under(path_l, e))
                {
                    exempt_hit = true;
                    break;
                }
            }
            if (exempt_hit)
            {
                continue;
            }
            if (detail::normalized_under(path_l, root_l))
            {
                // 树内：与跑前快照精确比对（新增/改写）。
                const std::string    rel_l = path_l.substr(root_l.size() + 1);
                const TreeFileState* before = nullptr;
                for (const TreeFileState& s : tree_before)
                {
                    if (cg::util::ascii_lower(s.rel) == rel_l)
                    {
                        before = &s;
                        break;
                    }
                }
                const std::optional<detail::FileProbe> now =
                    detail::probe_file_state(path);
                if (!now)
                {
                    // 缺失文件不被创建正是 no-op 的预期，不是泄漏。
                    continue;
                }
                if (before == nullptr || before->size != now->size
                    || before->mtime_ns != now->mtime_ns)
                {
                    leaks.push_back(path);
                }
            }
            else
            {
                // 树外：快照无从覆盖，退回运行窗口判定（修改时间落在
                // 运行窗口内 = 本轮新建/改写；陈旧文件不误报）。
                const std::optional<detail::FileProbe> now =
                    detail::probe_file_state(path);
                if (now && now->mtime_ns >= window_start_count)
                {
                    leaks.push_back(path);
                }
            }
        }
        return leaks;
    }

#endif // defined(_WIN32)
} // namespace cg::msbuild
