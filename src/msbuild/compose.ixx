module;
// 经典流头预热：本单元实例化 std::istringstream（AdditionalOptions 的
// 空白切词与 INCLUDE 直读都走字符串流），std 模块的导出声明面单独不足。
#include <istream>
#include <sstream>

/**
 * @module cg.msbuild.compose
 *
 * @brief MSBuild 求值通道的组装器：把求值元数据（cg.msbuild.eval 的
 *        工程级 cl_item_definition 与 TU 级 diff 存储）合成为 cl 方言的
 *        ClInvocation 列表——求值通道的 argv 生产侧（记录通道的 wrap
 *        捕获是它的对照真值臂）。
 *
 * 产出 std::vector<cg::msbuild::ClInvocation>（translate.ixx 的唯一载荷
 * 契约），无缝进 adapt_cl 与 classify_cl_pch：翻译层（/X 与 -imsvc 物化、
 * 兼容版本三级回退、/Zc:twoPhase- 透明追加）与 PCH 分类、旁车、索引尾
 * 全部原样复用。
 *
 * 组装纪律（三条，缺一不可）：
 * 1. 只发 MSBuild 真会发的开关——每个字段的开关拼写、连写/分写形态、
 *    否定形、属性默认档都经记录通道实测（本机 VS 18/v145 工具集对
 *    仓内夹具的 wrap 捕获），未实证的字段值域按"不发明"处理：未映射
 *    字段与未知枚举值只披露、不转译；
 * 2. 逐 TU 单命令——求值通道没有 MSBuild 的批处理趟（一次 cl 调用带
 *    多个位置源文件），每个 ClCompile item 恰一条 invocation、单个
 *    位置源文件；
 * 3. 过滤先行——build_in_solution 为假的工程整体跳过（sln 的 Build.0
 *    参与位），item 级 ExcludedFromBuild 非假跳过；两道过滤之后剩下的
 *    才是"构建参与 TU 集"。
 *
 * @internal
 * 字段面 37 项元数据（diff 存储短名 ↔ MSBuild metadata 名的平行表）与
 * 垫片侧 MsbuildEval.cs 的 FieldNames/MsbuildNames 数组同源；跨语言无法
 * 共享单一常量，此表是该字段面的 C++ 登记同步点（回放测试钉死键集，
 * 任何一侧漂移先红单测）。开关合成序 = MSBuild CL task 的 SwitchOrder
 * 实证归纳：模块前置族（/ifcSearchDir /reference /translateInclude）
 * 居首，/I 族、/Zi 族、警告族、/D 族（分写两 token——CL task 唯一的
 * 分写值开关族）、运行库族、/std 族、PCH 族（/Yc /Yu 连写头名 + /Fp
 * 连写）、产物族（/ifcOutput 分写、/Fo /Fd 连写）、/external:W 族、
 * 调用约定族、/TP//TC、/wd 与 /FI 族，AdditionalOptions 逐 token 垫尾、
 * 位置源文件收尾。与记录通道的差异面（CL task 属性默认值族 /c /nologo
 * /diagnostics:column /JMC /sdl /GS /fp /Ox /RTC /FS /Zc:forScope
 * /Zc:inline /FC /errorReport 与扫描趟开关 /scanDependencies
 * /sourceDependencies——求值面不可见、纯求值无扫描趟）不在此层补造，
 * 由对照判据显式登记（测试的投影规则同源声明）。
 */

export module cg.msbuild.compose;

import std;
import cg.util;
import cg.util.cstd;
import cg.msbuild.pipeline;  // ProjectCl（逐工程 cl）
import cg.msbuild.translate; // ClInvocation（载荷契约）
import cg.msbuild.eval;      // EvalProjectOut（求值产物）

export namespace cg::msbuild::compose
{

    /**
     * @brief TU 元数据字段面的一行登记（diff 存储短名 ↔ MSBuild 名）。
     */
    struct TuField
    {
        std::string_view tu_key;  ///< tus[] 的 diff 键（垫片侧 FieldNames）
        std::string_view msb_key; ///< cl_item_definition 键（MsbuildNames）
    };

    /**
     * @brief 组装器消费的字段面全集（37 项元数据；ExcludedFromBuild 是
     *        过滤位不走映射，合计 38 面口径）。
     *
     * @internal 与垫片侧 MsbuildEval.cs 的 FieldNames/MsbuildNames 平行
     * 数组同源——跨语言无法共享单一常量，本表是该字段面在 C++ 侧的
     * 登记同步点；回放测试钉死键集快照，任何一侧漂移先红单测。
     */
    inline constexpr TuField tu_fields[] = {
        {"defines", "PreprocessorDefinitions"},
        {"undefines", "UndefinePreprocessorDefinitions"},
        {"include_dirs", "AdditionalIncludeDirectories"},
        {"external_include", "ExternalIncludePath"},
        {"forced_includes", "ForcedIncludeFiles"},
        {"additional_options", "AdditionalOptions"},
        {"language_standard", "LanguageStandard"},
        {"language_standard_c", "LanguageStandard_C"},
        {"pch_use", "PrecompiledHeader"},
        {"pch_through", "PrecompiledHeaderFile"},
        {"pch_file", "PrecompiledHeaderOutputFile"},
        {"compile_as", "CompileAs"},
        {"conformance_mode", "ConformanceMode"},
        {"runtime_library", "RuntimeLibrary"},
        {"warning_level", "WarningLevel"},
        {"external_warning", "ExternalWarningLevel"},
        {"disable_warnings", "DisableSpecificWarnings"},
        {"treat_warnings_errors", "TreatWarningAsError"},
        {"multiproc", "MultiProcessorCompilation"},
        {"debug_format", "DebugInformationFormat"},
        {"eh", "ExceptionHandling"},
        {"function_level", "FunctionLevelLinking"},
        {"intrinsics", "IntrinsicFunctions"},
        {"string_pooling", "StringPooling"},
        {"minimal_rebuild", "MinimalRebuild"},
        {"browse_info", "BrowseInformation"},
        {"treat_wchar_builtin", "TreatWChar_tAsBuiltInType"},
        {"runtime_typeinfo", "RuntimeTypeInfo"},
        {"disable_lang_ext", "DisableLanguageExtensions"},
        {"calling_convention", "CallingConvention"},
        {"scan_modules", "ScanSourceForModuleDependencies"},
        {"translate_includes", "TranslateIncludes"},
        {"module_dependencies", "AdditionalModuleDependencies"},
        {"bmi_dirs", "AdditionalBMIDirectories"},
        {"module_output", "ModuleOutputFile"},
        {"object_file", "ObjectFileName"},
        {"program_database", "ProgramDataBaseFileName"},
    };

    /**
     * @brief 组装过程的披露日记（verbose 面）。
     *
     * 行语义三类：跳过（工程级参与位 / TU 级排除 / 坏载荷）、不代言
     * （未映射字段、未知枚举值——"compose: field X unmapped, skipped"
     * 形态）、已知不进命令行的字段说明（ExternalIncludePath 与
     * ScanSourceForModuleDependencies——记录通道实证 CL task 不为它们
     * 发常规趟开关，求值通道同形）。
     */
    struct ComposeDiary
    {
        std::vector<std::string> notes; ///< 逐条披露行（人读，非协议面）
    };

    /**
     * @brief 组装选项。
     */
    struct ComposeOptions
    {
        /**
         * @brief INCLUDE 环境覆盖（';' 列表原文）。
         *
         * @internal 缺省（nullopt）时直读进程 INCLUDE 一次、全体 invocation
         * 共享——记录通道 wrap 捕获的 INCLUDE 同样源自 ccg 进程环境
         * （MSBuild 子进程原样继承），两通道同源故直读即同值；测试用
         * 覆盖值钉死断言面（不受宿主环境影响）。
         */
        std::optional<std::string> include_env;
    };

    /**
     * @brief 把求值工程产物组装成 ClInvocation 列表。
     *
     * 输入两侧：projects = 求值会话的工程终态（raw_json 携带 37 字段面），
     * project_cls = 逐工程 cl 解析结果（pipeline::resolve_project_cls 的
     * 产物，sln 条目名与求值成员名同源对齐；匹配不到 cl 的工程披露跳过
     * ——组装器不发明编译器路径）。
     *
     * 失败语义：不抛异常——坏 raw_json 的工程披露跳过（求值会话本身
     * 已完成，工程级 error 已在 EvalProjectOut.error 承载，CLI 分派层
     * 按部分成功语义披露计数）；本层永远产出"可组装面"的全部条目。
     *
     * @param projects    求值工程产物（含失败工程——error 非空者披露跳过）。
     * @param project_cls 逐工程 cl（resolve_project_cls 产物）。
     * @param options     include_env 覆盖等。
     * @param diary       披露日记出参（可空 = 不收集）。
     */
    [[nodiscard]]
    auto compose_invocations(
        const std::vector<eval::EvalProjectOut>& projects,
        const std::vector<ProjectCl>&            project_cls,
        const ComposeOptions&                    options = {},
        ComposeDiary* diary = nullptr) -> std::vector<ClInvocation>;

} // namespace cg::msbuild::compose

// ---------------------------------------------------------------------------
// 实现。
// ---------------------------------------------------------------------------
namespace cg::msbuild::compose
{

    namespace
    {

        /** 模块开关拼写的单点清单锚（第二生产侧消费协议：组装器合成
         *  的模块开关必须按 cg.util.msvcswitch 的规范拼写发出——静态
         *  断言钉死成员资格，清单改名或组装器漂移先红编译）。 */
        static_assert(
            std::ranges::find(cg::util::msvcswitch::valued, "ifcOutput")
            != std::end(cg::util::msvcswitch::valued));
        static_assert(
            std::ranges::find(cg::util::msvcswitch::valued, "ifcSearchDir")
            != std::end(cg::util::msvcswitch::valued));
        static_assert(
            std::ranges::find(cg::util::msvcswitch::valued, "reference")
            != std::end(cg::util::msvcswitch::valued));

        /** 披露日记的便捷写行。 */
        class DiarySink
        {
        public:
            explicit DiarySink(ComposeDiary* diary) : diary_{diary} {}
            void note(std::string_view line) const
            {
                if (diary_ != nullptr)
                {
                    diary_->notes.emplace_back(line);
                }
            }

        private:
            ComposeDiary* diary_;
        };

        /** 一个 TU 的回放终值视图：itemdef 基线 + tus[] 的 diff 覆盖。 */
        class TuValues
        {
        public:
            TuValues(
                const cg::util::json::ValueView& item_definition,
                const cg::util::json::ValueView& tu_diff)
                : item_definition_{item_definition}, tu_diff_{tu_diff}
            {
            }

            /** 按字段表行取回放终值（diff 命中优先，缺省回 itemdef，
             *  再缺省空串——与垫片侧回放自检同一口径）。 */
            [[nodiscard]]
            auto value(const TuField& field) const -> std::string
            {
                if (tu_diff_.is_object())
                {
                    if (const auto v = tu_diff_.get(field.tu_key);
                        v && v->is_string())
                    {
                        return std::string{*v->as_string()};
                    }
                }
                if (item_definition_.is_object())
                {
                    if (const auto v = item_definition_.get(field.msb_key);
                        v && v->is_string())
                    {
                        return std::string{*v->as_string()};
                    }
                }
                return {};
            }

        private:
            const cg::util::json::ValueView& item_definition_;
            const cg::util::json::ValueView& tu_diff_;
        };

        /** 按字段表行从任意对象视图取字符串成员（top_props 提炼用）。 */
        [[nodiscard]]
        auto obj_str(const cg::util::json::ValueView& obj, std::string_view key)
            -> std::string
        {
            if (obj.is_object())
            {
                if (const auto v = obj.get(key); v && v->is_string())
                {
                    return std::string{*v->as_string()};
                }
            }
            return {};
        }

        /** metadata 的 ';' 列表拆分（MSBuild 列表语义：空段丢弃）。 */
        [[nodiscard]]
        auto split_list(std::string_view raw) -> std::vector<std::string>
        {
            std::vector<std::string> out;
            for (std::string_view part : cg::util::split(raw, ';'))
            {
                if (!part.empty())
                {
                    out.emplace_back(part);
                }
            }
            return out;
        }

        /** AdditionalOptions 的空白切词（MSBuild 把整串按空白拆 token
         *  逐个透传到命令行尾部）。 */
        [[nodiscard]]
        auto split_whitespace(std::string_view raw) -> std::vector<std::string>
        {
            std::vector<std::string> out;
            std::istringstream       in{std::string{raw}};
            std::string              token;
            while (in >> token)
            {
                out.push_back(std::move(token));
            }
            return out;
        }

        /** 值是否目录形态（尾随路径分隔符——CL task 对目录形态的产物
         *  字段按"目录 + 源文件名 + 后缀"拼值，文件形态原样透传）。 */
        [[nodiscard]]
        auto is_dir_like(std::string_view value) -> bool
        {
            return !value.empty()
                   && (value.back() == '\\' || value.back() == '/');
        }

        /** 源文件的全名（含扩展名——产物拼值用 mod.ixx.obj / mod.ixx.ifc
         *  形态，记录通道实证）。 */
        [[nodiscard]]
        auto file_name_of(std::string_view path) -> std::string
        {
            const std::size_t slash = path.find_last_of("/\\");
            return std::string{
                slash == std::string_view::npos ? path
                                                : path.substr(slash + 1)};
        }

        /** 源文件扩展名（小写、含点；模块单元判定用）。 */
        [[nodiscard]]
        auto lower_ext_of(std::string_view path) -> std::string
        {
            const std::string name = file_name_of(path);
            const std::size_t dot = name.find_last_of('.');
            if (dot == std::string::npos || dot + 1 == name.size())
            {
                return {};
            }
            return cg::util::ascii_lower(name.substr(dot));
        }

        /** boolean metadata 的真值判定（MSBuild 布尔字面量，精确
         *  匹配——未知拼写交给未映射披露，不猜语义）。 */
        [[nodiscard]]
        auto is_true(std::string_view value) -> bool
        {
            return cg::util::ascii_iequals(value, "true");
        }

        /** 三态 boolean 的字符串归类：true / false / 空（未知拼写
         *  返回 none 形态，由调用方披露）。 */
        enum class Tri
        {
            True,
            False,
            Absent,
            Unknown
        };

        [[nodiscard]]
        auto tri_of(std::string_view value) -> Tri
        {
            if (value.empty())
            {
                return Tri::Absent;
            }
            if (is_true(value))
            {
                return Tri::True;
            }
            if (cg::util::ascii_iequals(value, "false"))
            {
                return Tri::False;
            }
            return Tri::Unknown;
        }

        /** 正形/否定形 boolean 开关追加（true → 正形，false → 否定形，
         *  空不发；否定形是否合法按记录通道实证逐字段确定）；未知拼写
         *  不代言并披露一行。 */
        void emit_tri(
            std::vector<std::string>& argv,
            const DiarySink&          sink,
            std::string_view          field_name,
            std::string_view          raw_value,
            std::string_view          positive,
            std::string_view          negative,
            Tri                       state)
        {
            switch (state)
            {
            case Tri::True:
                argv.emplace_back(positive);
                break;
            case Tri::False:
                if (!negative.empty())
                {
                    argv.emplace_back(negative);
                }
                break;
            case Tri::Absent:
                break;
            case Tri::Unknown:
                sink.note(
                    std::format(
                        "compose: field {} '{}' unmapped, skipped",
                        field_name,
                        raw_value));
                break;
            }
        }

        /** /std: 值域映射（语言标准，C++ 与 C 两张）。 */
        [[nodiscard]]
        auto cpp_std_switch(std::string_view value) -> std::string
        {
            const std::pair<std::string_view, std::string_view> map[] = {
                {"stdcpp14", "/std:c++14"},
                {"stdcpp17", "/std:c++17"},
                {"stdcpp20", "/std:c++20"},
                {"stdcpp23", "/std:c++23"},
                {"stdcpp26", "/std:c++26"},
                {"stdcpplatest", "/std:c++latest"},
            };
            for (const auto& [meta, sw] : map)
            {
                if (cg::util::ascii_iequals(value, meta))
                {
                    return std::string{sw};
                }
            }
            return {};
        }

        [[nodiscard]]
        auto c_std_switch(std::string_view value) -> std::string
        {
            const std::pair<std::string_view, std::string_view> map[] = {
                {"stdc11", "/std:c11"},
                {"stdc17", "/std:c17"},
            };
            for (const auto& [meta, sw] : map)
            {
                if (cg::util::ascii_iequals(value, meta))
                {
                    return std::string{sw};
                }
            }
            return {};
        }

        /** WarningLevel → /W 开关；空值回落属性默认档 /W1（CL task 的
         *  ToolSwitch 默认，记录通道实证——工程未写警告级时 MSBuild
         *  实发 /W1）。EnableAllWarnings → /Wall。 */
        [[nodiscard]]
        auto warning_switch(std::string_view value) -> std::string
        {
            if (value.empty())
            {
                return "/W1";
            }
            if (cg::util::ascii_iequals(value, "EnableAllWarnings"))
            {
                return "/Wall";
            }
            const std::pair<std::string_view, std::string_view> map[] = {
                {"Level0", "/W0"},
                {"Level1", "/W1"},
                {"Level2", "/W2"},
                {"Level3", "/W3"},
                {"Level4", "/W4"},
            };
            for (const auto& [meta, sw] : map)
            {
                if (cg::util::ascii_iequals(value, meta))
                {
                    return std::string{sw};
                }
            }
            return {};
        }

        /** WarningLevel 档位数字（ExternalWarningLevel 继承档用）；
         *  EnableAllWarnings 记为 4（外部头警告级没有 all 档，最高档
         *  承接——推断面在注释与测试披露）。 */
        [[nodiscard]]
        auto warning_tier(std::string_view value) -> int
        {
            if (cg::util::ascii_iequals(value, "Level0"))
            {
                return 0;
            }
            if (cg::util::ascii_iequals(value, "Level1"))
            {
                return 1;
            }
            if (cg::util::ascii_iequals(value, "Level2"))
            {
                return 2;
            }
            if (cg::util::ascii_iequals(value, "Level3"))
            {
                return 3;
            }
            if (cg::util::ascii_iequals(value, "Level4")
                || cg::util::ascii_iequals(value, "EnableAllWarnings"))
            {
                return 4;
            }
            return 1; // 空值/未知：属性默认档（与 warning_switch 同源）
        }

        /** 单工程已知不进命令行的字段名（工程级披露一次，防 TU 级刷屏）。 */
        void note_known_unemitted(
            const cg::util::json::ValueView& item_definition,
            const DiarySink&                 sink)
        {
            if (!obj_str(item_definition, "ExternalIncludePath").empty())
            {
                sink.note(
                    "compose: field ExternalIncludePath not emitted "
                    "(CL task carries it only under the external-warning "
                    "pipeline; record channel shows no /external:I)");
            }
            if (const std::string v =
                    obj_str(item_definition, "ScanSourceForModuleDependencies");
                !v.empty() && !cg::util::ascii_iequals(v, "false"))
            {
                sink.note(
                    "compose: field ScanSourceForModuleDependencies not "
                    "emitted (drives the scan pass; evaluation channel has "
                    "no scan pass)");
            }
        }

    } // namespace

    auto compose_invocations(
        const std::vector<eval::EvalProjectOut>& projects,
        const std::vector<ProjectCl>&            project_cls,
        const ComposeOptions&                    options,
        ComposeDiary* diary) -> std::vector<ClInvocation>
    {
        namespace json = cg::util::json;
        const DiarySink           sink{diary};
        std::vector<ClInvocation> out;

        // INCLUDE 直读一次、全体共享（覆盖值供测试钉死）。
        const std::string include_env =
            options.include_env.has_value()
                ? *options.include_env
                : cg::util::cstd::get_env("INCLUDE").value_or("");

        // 逐工程 cl 按条目名建索引（resolve_project_cls 与求值成员清单
        // 同用 enumerate_sln_projects 的 name——同一键）。
        const auto find_cl =
            [&project_cls](const std::string& name) -> const ProjectCl*
        {
            for (const ProjectCl& e : project_cls)
            {
                if (e.project == name)
                {
                    return &e;
                }
            }
            return nullptr;
        };

        for (const eval::EvalProjectOut& project : projects)
        {
            if (!project.error.empty())
            {
                sink.note(
                    std::format(
                        "compose: project {} skipped (eval error: {})",
                        project.name,
                        project.error.substr(
                            0,
                            std::min<std::size_t>(project.error.size(), 160))));
                continue;
            }
            if (!project.build_in_solution)
            {
                sink.note(
                    std::format(
                        "compose: project {} skipped (Build.0 not set for this "
                        "configuration)",
                        project.name));
                continue;
            }
            const ProjectCl* pcl = find_cl(project.name);
            if (pcl == nullptr || pcl->cl.empty())
            {
                sink.note(
                    std::format(
                        "compose: project {} skipped (no cl resolved)",
                        project.name));
                continue;
            }

            // raw_json 二次解析：eval 的结构化面只提炼 5 字段，组装需要
            // 全部 37 项 + top_props（IntDir / MSBuildProjectDirectory）。
            std::optional<json::Document> parsed;
            try
            {
                parsed.emplace(json::Document::parse(project.raw_json));
            }
            catch (const std::exception& ex)
            {
                sink.note(
                    std::format(
                        "compose: project {} skipped (bad raw json: {})",
                        project.name,
                        ex.what()));
                continue;
            }
            const json::ValueView root = parsed->root();
            const auto        item_definition = root.get("cl_item_definition");
            const auto        top_props = root.get("top_props");
            const std::string project_dir =
                top_props ? obj_str(*top_props, "MSBuildProjectDirectory") : "";
            const std::string int_dir =
                top_props ? obj_str(*top_props, "IntDir") : "";
            const auto tus = root.get("tus");
            if (!item_definition || !tus || !tus->is_array()
                || project_dir.empty())
            {
                sink.note(
                    std::format(
                        "compose: project {} skipped (payload missing fields)",
                        project.name));
                continue;
            }

            // 模块感知预扫：工程内任一 TU 是模块单元（显式 CompileAs
            // 模块或模块扩展名命中）即感知——/ifcOutput 的发出条件
            // （记录通道实证：纯普通工程不发、感知工程每 TU 都发）。
            const bool module_aware = [&]
            {
                for (std::size_t i = 0; i < tus->size(); ++i)
                {
                    const auto t = tus->at(i);
                    if (!t || !t->is_object())
                    {
                        continue;
                    }
                    const TuValues    values{*item_definition, *t};
                    const std::string compile_as =
                        values.value({"compile_as", "CompileAs"});
                    if (cg::util::ascii_iequals(
                            compile_as,
                            "CompileAsCppModule"))
                    {
                        return true;
                    }
                    if (compile_as.empty()
                        || cg::util::ascii_iequals(compile_as, "Default"))
                    {
                        const std::string ext = lower_ext_of(
                            [&]() -> std::string
                            {
                                const std::string abs = obj_str(*t, "abs");
                                return !abs.empty() ? abs : obj_str(*t, "file");
                            }());
                        if (ext == ".ixx" || ext == ".cppm")
                        {
                            return true;
                        }
                    }
                }
                return false;
            }();

            for (std::size_t i = 0; i < tus->size(); ++i)
            {
                const auto tu = tus->at(i);
                if (!tu || !tu->is_object())
                {
                    continue;
                }
                const std::string tu_file = obj_str(*tu, "file");
                const std::string tu_abs = obj_str(*tu, "abs");
                if (tu_file.empty() && tu_abs.empty())
                {
                    continue;
                }
                const std::string excluded =
                    obj_str(*tu, "excluded_from_build");
                if (!excluded.empty()
                    && !cg::util::ascii_iequals(excluded, "false"))
                {
                    sink.note(
                        std::format(
                            "compose: {} skipped (ExcludedFromBuild={})",
                            tu_file,
                            excluded));
                    continue;
                }

                const TuValues    values{*item_definition, *tu};
                const std::string source = !tu_abs.empty() ? tu_abs : tu_file;
                const bool        interface_tu = [&]
                {
                    const std::string compile_as =
                        values.value({"compile_as", "CompileAs"});
                    if (cg::util::ascii_iequals(
                            compile_as,
                            "CompileAsCppModule"))
                    {
                        return true;
                    }
                    if (compile_as.empty()
                        || cg::util::ascii_iequals(compile_as, "Default"))
                    {
                        const std::string ext = lower_ext_of(source);
                        return ext == ".ixx" || ext == ".cppm";
                    }
                    return false;
                }();

                ClInvocation inv;
                inv.cwd = project_dir;
                inv.real_cl = pcl->cl;
                inv.include_env = include_env;
                std::vector<std::string>& argv = inv.argv_utf8;
                argv.push_back(pcl->cl);

                // —— 开关合成（序 = CL task SwitchOrder 实证归纳）——

                // 模块前置族（模块感知工程的常规趟也带：分写两 token）。
                for (const std::string& dir : split_list(values.value(
                         {"bmi_dirs", "AdditionalBMIDirectories"})))
                {
                    argv.emplace_back("/ifcSearchDir");
                    argv.push_back(dir);
                }
                for (const std::string& dep : split_list(values.value(
                         {"module_dependencies",
                          "AdditionalModuleDependencies"})))
                {
                    argv.emplace_back("/reference");
                    argv.push_back(dep);
                }
                if (is_true(values.value(
                        {"translate_includes", "TranslateIncludes"})))
                {
                    argv.emplace_back("/translateInclude");
                }

                // include 族（连写）。
                for (const std::string& dir : split_list(values.value(
                         {"include_dirs", "AdditionalIncludeDirectories"})))
                {
                    argv.push_back("/I" + dir);
                }

                // 调试信息族。
                {
                    const std::string v = values.value(
                        {"debug_format", "DebugInformationFormat"});
                    if (cg::util::ascii_iequals(v, "ProgramDatabase"))
                    {
                        argv.emplace_back("/Zi");
                    }
                    else if (cg::util::ascii_iequals(v, "EditAndContinue"))
                    {
                        argv.emplace_back("/ZI");
                    }
                    else if (cg::util::ascii_iequals(v, "OldStyle"))
                    {
                        argv.emplace_back("/Z7");
                    }
                    else if (!v.empty())
                    {
                        sink.note(
                            std::format(
                                "compose: field DebugInformationFormat '{}' "
                                "unmapped, skipped",
                                v));
                    }
                }

                // 警告级族（空值回落 /W1 属性默认档）。
                {
                    const std::string wl =
                        values.value({"warning_level", "WarningLevel"});
                    const std::string sw = warning_switch(wl);
                    if (!sw.empty())
                    {
                        argv.push_back(sw);
                    }
                    else
                    {
                        sink.note(
                            std::format(
                                "compose: field WarningLevel '{}' unmapped, "
                                "skipped",
                                wl));
                    }
                }

                // 警告即错误族（否定形 /WX-）。
                emit_tri(
                    argv,
                    sink,
                    "TreatWarningAsError",
                    values.value(
                        {"treat_warnings_errors", "TreatWarningAsError"}),
                    "/WX",
                    "/WX-",
                    tri_of(values.value(
                        {"treat_warnings_errors", "TreatWarningAsError"})));

                // 多处理器编译（无否定形拼写，false/空都不发）。
                if (is_true(values.value(
                        {"multiproc", "MultiProcessorCompilation"})))
                {
                    argv.emplace_back("/MP");
                }

                // 内建函数（true → /Oi；无否定形——false/空都不发）。
                if (is_true(values.value({"intrinsics", "IntrinsicFunctions"})))
                {
                    argv.emplace_back("/Oi");
                }

                // defines 族：CL task 唯一分写值开关族（"/D" "NAME"）。
                for (const std::string& name : split_list(
                         values.value({"defines", "PreprocessorDefinitions"})))
                {
                    argv.emplace_back("/D");
                    argv.push_back(name);
                }

                // undefines 族（连写）。
                for (const std::string& name : split_list(values.value(
                         {"undefines", "UndefinePreprocessorDefinitions"})))
                {
                    argv.push_back("/U" + name);
                }

                // 字符串池化（否定形 /GF-）。
                emit_tri(
                    argv,
                    sink,
                    "StringPooling",
                    values.value({"string_pooling", "StringPooling"}),
                    "/GF",
                    "/GF-",
                    tri_of(values.value({"string_pooling", "StringPooling"})));

                // 最小重建（否定形 /Gm-）。
                emit_tri(
                    argv,
                    sink,
                    "MinimalRebuild",
                    values.value({"minimal_rebuild", "MinimalRebuild"}),
                    "/Gm",
                    "/Gm-",
                    tri_of(
                        values.value({"minimal_rebuild", "MinimalRebuild"})));

                // 异常模型族。
                {
                    const std::string v =
                        values.value({"eh", "ExceptionHandling"});
                    if (cg::util::ascii_iequals(v, "Sync"))
                    {
                        argv.emplace_back("/EHsc");
                    }
                    else if (cg::util::ascii_iequals(v, "Async"))
                    {
                        argv.emplace_back("/EHa");
                    }
                    else if (cg::util::ascii_iequals(v, "SyncCThrow"))
                    {
                        argv.emplace_back("/EHcs");
                    }
                    else if (cg::util::ascii_iequals(v, "UnwindTables"))
                    {
                        argv.emplace_back("/EHs-");
                    }
                    else if (!v.empty())
                    {
                        sink.note(
                            std::format(
                                "compose: field ExceptionHandling '{}' unmapped, "
                                "skipped",
                                v));
                    }
                }

                // 运行库族。
                {
                    const std::string v =
                        values.value({"runtime_library", "RuntimeLibrary"});
                    if (cg::util::ascii_iequals(v, "MultiThreaded"))
                    {
                        argv.emplace_back("/MT");
                    }
                    else if (cg::util::ascii_iequals(v, "MultiThreadedDebug"))
                    {
                        argv.emplace_back("/MTd");
                    }
                    else if (cg::util::ascii_iequals(v, "MultiThreadedDll"))
                    {
                        argv.emplace_back("/MD");
                    }
                    else if (
                        cg::util::ascii_iequals(v, "MultiThreadedDebugDll"))
                    {
                        argv.emplace_back("/MDd");
                    }
                    else if (!v.empty())
                    {
                        sink.note(
                            std::format(
                                "compose: field RuntimeLibrary '{}' unmapped, "
                                "skipped",
                                v));
                    }
                }

                // 函数级链接（否定形 /Gy-）。
                emit_tri(
                    argv,
                    sink,
                    "FunctionLevelLinking",
                    values.value({"function_level", "FunctionLevelLinking"}),
                    "/Gy",
                    "/Gy-",
                    tri_of(values.value(
                        {"function_level", "FunctionLevelLinking"})));

                // 禁用语言扩展（true → 裸 /Za；无否定形，位置在
                // wchar_t 开关之前——记录通道实证）。
                if (is_true(values.value(
                        {"disable_lang_ext", "DisableLanguageExtensions"})))
                {
                    argv.emplace_back("/Za");
                }

                // wchar_t 内建（否定形 /Zc:wchar_t-）。
                emit_tri(
                    argv,
                    sink,
                    "TreatWChar_tAsBuiltInType",
                    values.value(
                        {"treat_wchar_builtin", "TreatWChar_tAsBuiltInType"}),
                    "/Zc:wchar_t",
                    "/Zc:wchar_t-",
                    tri_of(values.value(
                        {"treat_wchar_builtin", "TreatWChar_tAsBuiltInType"})));

                // 运行时类型信息（否定形 /GR-）。
                emit_tri(
                    argv,
                    sink,
                    "RuntimeTypeInfo",
                    values.value({"runtime_typeinfo", "RuntimeTypeInfo"}),
                    "/GR",
                    "/GR-",
                    tri_of(
                        values.value({"runtime_typeinfo", "RuntimeTypeInfo"})));

                // 语言标准（C++ 与 C 同位）。
                {
                    const std::string cpp = cpp_std_switch(values.value(
                        {"language_standard", "LanguageStandard"}));
                    if (!cpp.empty())
                    {
                        argv.push_back(cpp);
                    }
                    else if (
                        const std::string raw = values.value(
                            {"language_standard", "LanguageStandard"});
                        !raw.empty()
                        && !cg::util::ascii_iequals(raw, "Default"))
                    {
                        sink.note(
                            std::format(
                                "compose: field LanguageStandard '{}' unmapped, "
                                "skipped",
                                raw));
                    }
                    const std::string c = c_std_switch(values.value(
                        {"language_standard_c", "LanguageStandard_C"}));
                    if (!c.empty())
                    {
                        argv.push_back(c);
                    }
                    else if (
                        const std::string raw = values.value(
                            {"language_standard_c", "LanguageStandard_C"});
                        !raw.empty()
                        && !cg::util::ascii_iequals(raw, "Default"))
                    {
                        sink.note(
                            std::format(
                                "compose: field LanguageStandard_C '{}' unmapped, "
                                "skipped",
                                raw));
                    }
                }

                // 一致模式（false → /permissive；Default/空不发）。
                {
                    const std::string v =
                        values.value({"conformance_mode", "ConformanceMode"});
                    const Tri t = tri_of(v);
                    if (t == Tri::True)
                    {
                        argv.emplace_back("/permissive-");
                    }
                    else if (t == Tri::False)
                    {
                        argv.emplace_back("/permissive");
                    }
                    else if (t == Tri::Unknown)
                    {
                        sink.note(
                            std::format(
                                "compose: field ConformanceMode '{}' unmapped, "
                                "skipped",
                                v));
                    }
                }

                // PCH 族：三态开关联动头名与产物文件。
                {
                    const std::string use =
                        values.value({"pch_use", "PrecompiledHeader"});
                    const std::string through =
                        values.value({"pch_through", "PrecompiledHeaderFile"});
                    const std::string file = values.value(
                        {"pch_file", "PrecompiledHeaderOutputFile"});
                    if (cg::util::ascii_iequals(use, "Create"))
                    {
                        argv.push_back("/Yc" + through);
                    }
                    else if (cg::util::ascii_iequals(use, "Use"))
                    {
                        argv.push_back("/Yu" + through);
                    }
                    else if (
                        !use.empty()
                        && !cg::util::ascii_iequals(use, "NotUsing"))
                    {
                        sink.note(
                            std::format(
                                "compose: field PrecompiledHeader '{}' unmapped, "
                                "skipped",
                                use));
                    }
                    if (cg::util::ascii_iequals(use, "Create")
                        || cg::util::ascii_iequals(use, "Use"))
                    {
                        argv.push_back("/Fp" + file);
                    }
                }

                // 模块产物（分写；仅模块感知工程发——props 为全体工程布置
                // ModuleOutputFile 默认值（IntDir 形态），但记录通道实证
                // 纯普通工程不发该开关、含模块 TU 的工程每个 TU 都发；
                // 感知判据 = 工程内存在模块单元 TU。目录形态对接口单元
                // 拼 <全名>.ifc）。
                if (module_aware)
                {
                    const std::string v =
                        values.value({"module_output", "ModuleOutputFile"});
                    if (!v.empty())
                    {
                        argv.emplace_back("/ifcOutput");
                        if (is_dir_like(v) && interface_tu)
                        {
                            argv.push_back(v + file_name_of(source) + ".ifc");
                        }
                        else
                        {
                            argv.push_back(v);
                        }
                    }
                }

                // 对象产物（连写；目录形态对接口单元拼 <全名>.obj）。
                {
                    const std::string v =
                        values.value({"object_file", "ObjectFileName"});
                    if (!v.empty())
                    {
                        if (is_dir_like(v) && interface_tu)
                        {
                            argv.push_back(
                                "/Fo" + v + file_name_of(source) + ".obj");
                        }
                        else
                        {
                            argv.push_back("/Fo" + v);
                        }
                    }
                }

                // PDB 产物（连写原样）。
                {
                    const std::string v = values.value(
                        {"program_database", "ProgramDataBaseFileName"});
                    if (!v.empty())
                    {
                        argv.push_back("/Fd" + v);
                    }
                }

                // 浏览信息（true → /FR+IntDir；无否定形）。
                if (is_true(values.value({"browse_info", "BrowseInformation"})))
                {
                    argv.push_back("/FR" + int_dir);
                }

                // 外部警告级（继承档与属性默认档 W1 都按记录实证承接）。
                {
                    const std::string wl =
                        values.value({"warning_level", "WarningLevel"});
                    const std::string v = values.value(
                        {"external_warning", "ExternalWarningLevel"});
                    if (cg::util::ascii_iequals(v, "Level0")
                        || cg::util::ascii_iequals(v, "Level1")
                        || cg::util::ascii_iequals(v, "Level2")
                        || cg::util::ascii_iequals(v, "Level3")
                        || cg::util::ascii_iequals(v, "Level4"))
                    {
                        argv.push_back(
                            "/external:W" + std::to_string(warning_tier(v)));
                    }
                    else if (
                        v.empty()
                        || cg::util::ascii_iequals(v, "InheritWarningLevel"))
                    {
                        // 空 = 属性默认档；Inherit = 主警告级同档
                        // （含空主警告级回落 /W1 的同源默认）。
                        argv.push_back(
                            "/external:W" + std::to_string(warning_tier(wl)));
                    }
                    else if (!v.empty())
                    {
                        sink.note(
                            std::format(
                                "compose: field ExternalWarningLevel '{}' "
                                "unmapped, skipped",
                                v));
                    }
                }

                // 调用约定族。
                {
                    const std::string v = values.value(
                        {"calling_convention", "CallingConvention"});
                    if (cg::util::ascii_iequals(v, "Cdecl"))
                    {
                        argv.emplace_back("/Gd");
                    }
                    else if (cg::util::ascii_iequals(v, "FastCall"))
                    {
                        argv.emplace_back("/Gr");
                    }
                    else if (cg::util::ascii_iequals(v, "StdCall"))
                    {
                        argv.emplace_back("/Gz");
                    }
                    else if (cg::util::ascii_iequals(v, "VectorCall"))
                    {
                        argv.emplace_back("/Gv");
                    }
                    else if (!v.empty())
                    {
                        sink.note(
                            std::format(
                                "compose: field CallingConvention '{}' unmapped, "
                                "skipped",
                                v));
                    }
                }

                // 语言模式：显式/默认扩展名推断；接口单元的 /TP 垫末尾
                // （FixupCLCompileOptions 的追加位——记录通道实证接口
                // 单元的 /TP 在全部 ToolSwitch 之后）。
                std::string trailing_tp;
                {
                    const std::string v =
                        values.value({"compile_as", "CompileAs"});
                    const std::string ext = lower_ext_of(source);
                    if (cg::util::ascii_iequals(v, "CompileAsC"))
                    {
                        argv.emplace_back("/TC");
                    }
                    else if (cg::util::ascii_iequals(v, "CompileAsCpp"))
                    {
                        argv.emplace_back("/TP");
                    }
                    else if (cg::util::ascii_iequals(v, "CompileAsCppModule"))
                    {
                        argv.emplace_back("/interface");
                        trailing_tp = "/TP";
                    }
                    else if (v.empty() || cg::util::ascii_iequals(v, "Default"))
                    {
                        if (ext == ".c")
                        {
                            argv.emplace_back("/TC");
                        }
                        else if (interface_tu)
                        {
                            argv.emplace_back("/interface");
                            trailing_tp = "/TP";
                        }
                        else
                        {
                            argv.emplace_back("/TP");
                        }
                    }
                    else
                    {
                        sink.note(
                            std::format(
                                "compose: field CompileAs '{}' unmapped, skipped",
                                v));
                    }
                }

                // 禁用警告族（连写，语言模式之后）。
                for (const std::string& w : split_list(values.value(
                         {"disable_warnings", "DisableSpecificWarnings"})))
                {
                    argv.push_back("/wd" + w);
                }

                // 强制包含族（连写）。
                for (const std::string& f : split_list(values.value(
                         {"forced_includes", "ForcedIncludeFiles"})))
                {
                    argv.push_back("/FI" + f);
                }

                // AdditionalOptions：逐 token 垫在全部开关之后。
                for (const std::string& token : split_whitespace(values.value(
                         {"additional_options", "AdditionalOptions"})))
                {
                    argv.push_back(token);
                }

                if (!trailing_tp.empty())
                {
                    argv.push_back(std::move(trailing_tp));
                }

                // 位置源文件收尾（求值通道天然绝对化）。
                argv.push_back(source);
                out.push_back(std::move(inv));
            }

            // 已知不进常规趟命令行的字段（记录通道实证，工程级披露）。
            note_known_unemitted(*item_definition, sink);
        }
        return out;
    }

} // namespace cg::msbuild::compose
