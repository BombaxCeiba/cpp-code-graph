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
 * @module cg.msbuild.pipeline.treestate
 *
 * @brief
 * 目录树状态域（_WIN32）：沙箱差异与树快照、文件状态探针、自造产物分类（生成器声明输出、midl
 * 产物、新文件归类）。
 */
export module cg.msbuild.pipeline.treestate;

import std;
import cg.util;
import cg.util.cstd;
import cg.msbuild.translate; // ClInvocation / CdbEntry（wrap 日志载荷类型）
import cg.msbuild.wrap;      // detail::env_wide / fs_path_from_utf8
import cg.msbuild.pipeline.configs;
import cg.msbuild.pipeline.hostfind;

export namespace cg::msbuild
{
    namespace json = cg::util::json;
#if defined(_WIN32)

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

    namespace detail
    {

        /**
         * @brief abs 是否位于 roots 中任一根目录之下。
         *
         * @internal 以 fs::relative 判定（含等值时相对路径为空的自身）；
         * 空根跳过，目录不可读的枝干按不在其下处理。
         */
        [[nodiscard]]
        inline auto path_under_any(
            const std::filesystem::path&    abs,
            const std::vector<std::string>& roots) -> bool
        {
            for (const std::string& r : roots)
            {
                if (r.empty())
                {
                    continue;
                }
                std::error_code             ec;
                const std::filesystem::path rel = std::filesystem::relative(
                    abs,
                    std::filesystem::path{r},
                    ec);
                if (!ec && !rel.empty() && rel != ".." && *rel.begin() != "..")
                {
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief 折叠小写路径 p 是否等于 b 或位于 b 之下（'/' 分隔前缀）。
         *
         * @internal b 为空不命中：空串作前缀会把一切路径判为命中。
         */
        [[nodiscard]]
        inline auto normalized_under(const std::string& p, const std::string& b)
            -> bool
        {
            if (b.empty())
            {
                return false;
            }
            return p == b
                   || (p.size() > b.size() && p.compare(0, b.size(), b) == 0
                       && p[b.size()] == '/');
        }

        /**
         * @brief 文件现状探测的取值：字节数与修改时间。
         */
        struct FileProbe
        {
            std::uintmax_t size = 0;
            std::int64_t   mtime_ns = 0;
        };

        /**
         * @brief 探测一个文件的现状；取不到修改时间（不存在、权限等）
         *        时为 nullopt。
         *
         * @internal 字节数取不到记 0（与树快照同口径），不构成失败。
         */
        [[nodiscard]]
        inline auto probe_file_state(const std::string& path)
            -> std::optional<FileProbe>
        {
            std::error_code                       ec;
            const std::filesystem::file_time_type mtime =
                std::filesystem::last_write_time(fs_path_from_utf8(path), ec);
            if (ec)
            {
                return std::nullopt;
            }
            FileProbe probe;
            probe.mtime_ns =
                static_cast<std::int64_t>(mtime.time_since_epoch().count());
            std::error_code size_ec;
            probe.size =
                std::filesystem::file_size(fs_path_from_utf8(path), size_ec);
            if (size_ec)
            {
                probe.size = 0;
            }
            return probe;
        }

        /**
         * @brief 解析单个工程文件声明的 CustomBuild Outputs，绝对路径
         *        保序去重并入 out。
         *
         * @internal 面向行的文本扫描：%(RootDir)%(Directory) 以条目同目录
         * 解析，$(MSBuildProjectDirectory)/$(ProjectDir) 以工程目录解析，
         * 含其余不可展开宏的条目跳过——白名单宁可窄，泄漏告警绝不静默
         * 吞掉生成物；工程不可读时静默跳过（不并入任何条目）。
         */
        inline void collect_custom_build_outputs(
            const std::string&        project_path,
            std::vector<std::string>& out)
        {
            const std::optional<std::string> text =
                read_text_file(project_path);
            if (!text)
            {
                return;
            }
            const std::string project_dir =
                std::filesystem::path{fs_path_from_utf8(project_path)}
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
        }

    } // namespace detail

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
        const fs::path             root_fs = detail::fs_path_from_utf8(root);
        std::error_code            top_ec;
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
                    if (detail::path_under_any(entry.path(), exclude))
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
        for (const std::string& p : projects)
        {
            detail::collect_custom_build_outputs(p, out);
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
        if (lower.ends_with(".tlb") || lower.ends_with("_i.c")
            || lower.ends_with("_p.c") || lower.ends_with("dlldata.c"))
        {
            return true;
        }
        if (!lower.ends_with(".h"))
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
                declared_rel.push_back(
                    cg::util::ascii_lower(rel.generic_string()));
            }
        }
        std::ranges::sort(declared_rel);
        SandboxDiff out;
        for (const std::string& f : fresh)
        {
            const std::string f_lower = cg::util::ascii_lower(f);
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

#endif // defined(_WIN32)
} // namespace cg::msbuild
