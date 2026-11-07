// MsbuildEval.cs — MSBuild 工程求值会话（gt-dump 金标原型到 net48/C#5 的
// 垫片对象移植：对解决方案成员工程做 Microsoft.Build.Evaluation 纯求值、
// 枚举 ClCompile 条目与最终 metadata，按工程流式回报事件）。
// 仅 51 线编译本文件：VS 自带 Microsoft.Build.dll 是 net472 程序集，
// CoreCLR（7 线）装不下 .NET Framework 家族程序集，7 线的 ObjCreate
// 请求本类返回"不被本线承载"专用错误码。
//
// 对象协议（宿主侧 ObjCall 分派名与命名实参键）：
//  - Begin(request)：请求 JSON（sln 路径、配置对、VS 根、VCTargetsPath、
//    成员工程清单——ccg 侧发现链与 sln 解析预先算好递入，垫片不重复
//    实现 sln 解析）；成功回 {"ok":true,"projects":N}；
//  - NextEvent()：事件游标推进，每调用回一个事件 JSON——每工程先
//    progress（金标 [OK]/[FAIL] 行的结构化）后 project（工程终态一次性
//    载荷：top_props/cl_item_definition/tus/import_closure 等），全部
//    工程后一个 summary（协议完成标志）；summary 之后再调用是协议
//    违例，抛异常经 ObjCall 回 -13 与诊断文本。
//
// 硬约束（设计稿 1.3.4，写死在本垫片）：
//  1. 引用程序集只用 VS 自带（构造参数 assembly_dir，即 MSBuild.exe
//     同目录），绝不用 Rider 捆绑 SDK 或独立 MSBuild 发行包；
//  2. 依赖解析 handler 只做三步：Path.Combine → File.Exists →
//     Assembly.LoadFrom——绝不做 GetType/Activator/属性求值等任何
//     可再触发解析的操作（PS 5.1 宿主下 AssemblyResolve 重入翻
//     StackOverflowException 的前例机理）；
//  3. 运行期零编译（构建期一次编译，目标机只执行）。
//
// 求值内核是金标原型（dotnet10 file-based app）的 C#5 去语法糖移植：
// 顶层语句 → 类方法、字符串插值 → string.Format、?. 与范围索引 →
// 显式 null 判与 Substring、元组字段表 → 平行数组；反射调用面本身
// C#5 全兼容原样保留（含 MSB4019 降级腿按 arity 扫描定位五参构造器
// 的手法与 37 字段的 diff-vs-itemdef 存储 + 回放自检）。跨调用持状态：
// 程序集解析与反射类型缓存进程一次，ProjectCollection 每会话一个
//（多配置循环各开各的会话，互不串状态）。JSON 序列化用
// System.Web.Extensions 的 JavaScriptSerializer（自带库纪律，严禁手写）。
//
// 语言版本钉 C# 5（51 入口由 .NET Framework 自带 csc 编译）；禁字符串
// 插值、?.、表达式体、out 变量声明等一切新语法。
using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Text;
using System.Web.Script.Serialization;

namespace CcgCsHost
{
    /** MSBuild 求值会话对象：存续期间跨调用复用程序集解析结果与
     *  ProjectCollection（构造一次、Begin/NextEvent 多次、OnFinalRelease
     *  收尾）。 */
    public class MsbuildEval : ICshostObject
    {
        // JSON 收发上限放宽：单工程事件量级可到 MB 级（JavaScriptSerializer
        // 默认 2MB 序列化上限会提前拒发）。
        const int JsonLimit = 32 * 1024 * 1024;

        // —— 进程级一次装载（多会话复用；首个会话钉死解析根：进程级
        //    单 VS 实例，后续会话目录不同也不改根——改根会让已载程序集
        //    的依赖漂移）。 ——
        static readonly object s_gate = new object();
        static bool s_resolveRegistered;
        static string s_assemblyDir;
        static Assembly s_msbuild;
        // 反射类型与成员缓存（Microsoft.Build.dll LoadFrom 之后进程内
        // 不再变化；缓存消除逐工程逐字段的重复查找）。
        static Type s_tCollection;
        static Type s_tProject;
        static Type s_tRoot;
        static Type s_tLoadSettings;
        static MethodInfo s_loadProject3; // LoadProject(string, IDictionary<string,string>, string)
        static MethodInfo s_rootOpen;     // ProjectRootElement.Open(string, ProjectCollection)
        static ConstructorInfo s_ctor5;   // Project(xml, globalProps, toolsVersion, collection, loadSettings)

        // —— per-TU metadata 字段面（37 项：协议字段名与 MSBuild metadata
        //    名的平行数组——C#5 无值元组，金标元组表的去语法糖形态）。 ——
        static readonly string[] FieldNames = new string[] {
            "defines", "undefines", "include_dirs", "external_include",
            "forced_includes", "additional_options", "language_standard",
            "language_standard_c", "pch_use", "pch_through", "pch_file",
            "compile_as", "conformance_mode", "runtime_library",
            "warning_level", "external_warning", "disable_warnings",
            "treat_warnings_errors", "multiproc", "debug_format", "eh",
            "function_level", "intrinsics", "string_pooling",
            "minimal_rebuild", "browse_info", "treat_wchar_builtin",
            "runtime_typeinfo", "disable_lang_ext", "calling_convention",
            "scan_modules", "translate_includes", "module_dependencies",
            "bmi_dirs", "module_output", "object_file", "program_database",
        };
        static readonly string[] MsbuildNames = new string[] {
            "PreprocessorDefinitions", "UndefinePreprocessorDefinitions",
            "AdditionalIncludeDirectories", "ExternalIncludePath",
            "ForcedIncludeFiles", "AdditionalOptions", "LanguageStandard",
            "LanguageStandard_C", "PrecompiledHeader",
            "PrecompiledHeaderFile", "PrecompiledHeaderOutputFile",
            "CompileAs", "ConformanceMode", "RuntimeLibrary",
            "WarningLevel", "ExternalWarningLevel",
            "DisableSpecificWarnings", "TreatWarningAsError",
            "MultiProcessorCompilation", "DebugInformationFormat",
            "ExceptionHandling", "FunctionLevelLinking",
            "IntrinsicFunctions", "StringPooling", "MinimalRebuild",
            "BrowseInformation", "TreatWChar_tAsBuiltInType",
            "RuntimeTypeInfo", "DisableLanguageExtensions",
            "CallingConvention", "ScanSourceForModuleDependencies",
            "TranslateIncludes", "AdditionalModuleDependencies",
            "AdditionalBMIDirectories", "ModuleOutputFile",
            "ObjectFileName", "ProgramDataBaseFileName",
        };

        // 顶层属性白名单（金标 18 键 + SolutionDir 族 5 键——解决方案
        // 属性的求值侧补齐，披露与哈希输入两用）。
        static readonly string[] TopProps = new string[] {
            "PlatformToolset", "ConfigurationType", "CharacterSet",
            "UseOfMfc", "UseOfAtl", "WindowsTargetPlatformVersion",
            "TargetPlatformVersion", "WindowsSDKVersion", "VCToolsVersion",
            "VCToolsInstallDir", "VCToolsetsDir", "OutDir", "IntDir",
            "TargetName", "PreferredToolArchitecture",
            "WholeProgramOptimization", "MSBuildProjectDirectory",
            "EnableUnitySupport",
            "SolutionDir", "SolutionExt", "SolutionName",
            "SolutionFileName", "SolutionPath",
        };

        // item 计数的类型面（金标原样，含 Midl 重复项——大小写两拼写的
        // 真实工程里各数各的，忠实保留）。
        static readonly string[] ItemTypes = new string[] {
            "ClCompile", "ClInclude", "MIDL", "ResourceCompile", "None",
            "CustomBuild", "MASM", "Natvis", "Midl",
        };

        /** Begin 请求解析后的成员工程条目。 */
        sealed class ProjectSpec
        {
            public string Name;
            public string Vcxproj;
            public string Guid;
            public string Cfg;
            public string Plat;
            public bool Build;
        }

        readonly object m_collection;
        readonly JavaScriptSerializer m_json;
        List<ProjectSpec> m_projects;
        string m_solution;
        string m_slnDir;
        string m_cscc;
        string m_vsRoot;
        string m_vcTargets;
        string m_vsVersion;
        readonly List<string> m_pending = new List<string>();
        readonly Stopwatch m_all = new Stopwatch();
        int m_cursor;
        bool m_summarySent;
        int m_ok;
        int m_failed;
        int m_diffChecks;
        int m_diffErrors;

        MsbuildEval(object collection, JavaScriptSerializer json)
        {
            m_collection = collection;
            m_json = json;
        }

        /**
         * 静态工厂（ObjCreate 构造分派目标；assembly_dir = MSBuild.exe
         * 同目录）。解析器注册与引擎程序集 LoadFrom 进程各一次，会话
         * 重复创建只复用；ProjectCollection 每对象一个实例。反射类型
         * 与成员缓存与 LoadFrom 同批完成（金标求值六步全程纯反射）。
         */
        public static MsbuildEval Create(string assemblyDir)
        {
            if (assemblyDir == null || assemblyDir.Length == 0
                || !Directory.Exists(assemblyDir))
            {
                throw new ArgumentException(
                    "assembly_dir missing or not a directory: "
                    + (assemblyDir == null ? "<null>" : assemblyDir));
            }
            lock (s_gate)
            {
                if (!s_resolveRegistered)
                {
                    s_assemblyDir = assemblyDir;
                    AppDomain.CurrentDomain.AssemblyResolve +=
                        delegate(object sender, ResolveEventArgs e)
                        {
                            // 三步纪律：只拼路径、只探存在、只 LoadFrom。
                            string simple = new AssemblyName(e.Name).Name;
                            string path = Path.Combine(
                                s_assemblyDir, simple + ".dll");
                            return File.Exists(path)
                                ? Assembly.LoadFrom(path)
                                : null;
                        };
                    s_resolveRegistered = true;
                }
                if (s_msbuild == null)
                {
                    try
                    {
                        s_msbuild = Assembly.LoadFrom(
                            Path.Combine(
                                assemblyDir, "Microsoft.Build.dll"));
                        CacheReflection();
                    }
                    catch (Exception)
                    {
                        // 反射面半初始化回滚（不留 s_msbuild 已置而成员
                        // 缺席的状态——那会让后续会话跳过缓存步骤、
                        // 在求值期以 NRE 形态晚爆）。
                        s_msbuild = null;
                        throw;
                    }
                }
            }
            object collection = Activator.CreateInstance(s_tCollection);
            if (collection == null)
            {
                throw new InvalidOperationException(
                    "ProjectCollection instantiation failed");
            }
            JavaScriptSerializer json = new JavaScriptSerializer();
            json.MaxJsonLength = JsonLimit;
            return new MsbuildEval(collection, json);
        }

        /**
         * 反射类型与成员的一次性定位（类型数组精确查找与 arity 扫描
         * 并用——金标手法：类型数组查找会绊跨程序集的解析器，五参
         * 构造器按形参个数与首尾形参类型扫描定位）。
         *
         * @internal 装载入口用 VS 自带引擎（net472 家族）的三参重载
         * LoadProject(file, globalProperties, toolsVersion)：.NET
         * Framework 版 ProjectCollection 没有双参 (file, props) 重载
         * （那是 dotnet SDK 版引擎才有的形态）；toolsVersion 传 null
         * 走引擎默认，语义与金标"每工程注入 global props"等价，且
         * 保住单一 ProjectCollection 跨调用复用。
         */
        static void CacheReflection()
        {
            s_tCollection = s_msbuild.GetType(
                "Microsoft.Build.Evaluation.ProjectCollection");
            s_tProject = s_msbuild.GetType(
                "Microsoft.Build.Evaluation.Project");
            s_tRoot = s_msbuild.GetType(
                "Microsoft.Build.Construction.ProjectRootElement");
            s_tLoadSettings = s_msbuild.GetType(
                "Microsoft.Build.Evaluation.ProjectLoadSettings");
            if (s_tCollection == null || s_tProject == null
                || s_tRoot == null || s_tLoadSettings == null)
            {
                throw new InvalidOperationException(
                    "Microsoft.Build evaluation types not found in "
                    + s_msbuild.Location);
            }
            s_loadProject3 = s_tCollection.GetMethod(
                "LoadProject",
                new Type[] { typeof(string),
                             typeof(IDictionary<string, string>),
                             typeof(string) });
            s_rootOpen = s_tRoot.GetMethod(
                "Open", new Type[] { typeof(string), s_tCollection });
            if (s_loadProject3 == null || s_rootOpen == null)
            {
                throw new InvalidOperationException(
                    "LoadProject(3-arg)/Open reflection signatures not "
                    + "found in " + s_msbuild.Location);
            }
            foreach (ConstructorInfo c in s_tProject.GetConstructors())
            {
                ParameterInfo[] ps = c.GetParameters();
                if (ps.Length == 5 && ps[0].ParameterType == s_tRoot
                    && ps[3].ParameterType == s_tCollection
                    && ps[4].ParameterType == s_tLoadSettings)
                {
                    s_ctor5 = c;
                    break;
                }
            }
            if (s_ctor5 == null)
            {
                throw new InvalidOperationException(
                    "5-parameter Project ctor not found in "
                    + s_msbuild.Location);
            }
        }

        /**
         * 求值会话开启：校验并解析请求载荷（sln 路径、配置对、VS 根、
         * VCTargetsPath、成员清单），预拼 CurrentSolutionConfigurationContents，
         * 重置事件游标与统计。成功返回 {"ok":true,"projects":N}。
         */
        public string Begin(string request)
        {
            if (request == null || request.Length == 0)
            {
                throw new ArgumentException("Begin request payload empty");
            }
            Dictionary<string, object> parsed =
                m_json.Deserialize<Dictionary<string, object>>(request);
            if (parsed == null)
            {
                throw new ArgumentException(
                    "Begin request is not a JSON object");
            }
            m_solution = Str(parsed, "solution");
            m_vsRoot = Str(parsed, "vs_root");
            m_vcTargets = Str(parsed, "vc_targets");
            m_vsVersion = Str(parsed, "vs_version");
            string config = Str(parsed, "config");
            string platform = Str(parsed, "platform");
            if (m_solution.Length == 0 || config.Length == 0
                || platform.Length == 0 || m_vsRoot.Length == 0
                || m_vcTargets.Length == 0 || m_vsVersion.Length == 0)
            {
                throw new ArgumentException(
                    "Begin request missing one of solution/config/platform/"
                    + "vs_root/vc_targets/vs_version");
            }
            // VCTargetsPath 尾反斜杠硬约束（金标探针实证）；调用侧探测
            // 函数已保证，此处防御性补齐。
            char last = m_vcTargets[m_vcTargets.Length - 1];
            if (last != '\\' && last != '/')
            {
                m_vcTargets = m_vcTargets + "\\";
            }
            m_slnDir = Path.GetDirectoryName(Path.GetFullPath(m_solution));
            if (m_slnDir == null || m_slnDir.Length == 0)
            {
                throw new ArgumentException(
                    "solution path has no directory: " + m_solution);
            }

            m_projects = new List<ProjectSpec>();
            object arr;
            parsed.TryGetValue("projects", out arr);
            IEnumerable list = arr as IEnumerable;
            if (list != null)
            {
                foreach (object it in list)
                {
                    Dictionary<string, object> po =
                        it as Dictionary<string, object>;
                    if (po == null)
                    {
                        continue;
                    }
                    ProjectSpec spec = new ProjectSpec();
                    spec.Name = Str(po, "name");
                    spec.Vcxproj = Str(po, "vcxproj");
                    spec.Guid = Str(po, "guid");
                    spec.Cfg = Str(po, "cfg");
                    spec.Plat = Str(po, "plat");
                    if (spec.Cfg.Length == 0)
                    {
                        spec.Cfg = config;
                    }
                    if (spec.Plat.Length == 0)
                    {
                        spec.Plat = platform;
                    }
                    spec.Build = BoolOf(po, "build");
                    if (spec.Name.Length == 0 || spec.Vcxproj.Length == 0
                        || spec.Guid.Length == 0)
                    {
                        throw new ArgumentException(
                            "Begin project entry missing name/vcxproj/guid: "
                            + spec.Name);
                    }
                    m_projects.Add(spec);
                }
            }

            // CurrentSolutionConfigurationContents（金标的解决方案配置
            // XML 格式）。BuildProjectInSolution 金标硬编码 "True"——
            // Build.0 参与位是独立报告字段（build_in_solution），不进
            // 此 XML；未映射 ActiveCfg 的成员由调用侧过滤后才递入。
            StringBuilder scc = new StringBuilder();
            scc.AppendLine("<SolutionConfiguration>");
            foreach (ProjectSpec spec in m_projects)
            {
                scc.AppendLine(string.Format(
                    CultureInfo.InvariantCulture,
                    "<ProjectConfiguration Project=\"{0}\" "
                    + "AbsolutePath=\"{1}\" BuildProjectInSolution=\"{2}\">"
                    + "{3}|{4}</ProjectConfiguration>",
                    spec.Guid,
                    Path.GetFullPath(spec.Vcxproj),
                    "True",
                    spec.Cfg,
                    spec.Plat));
            }
            scc.Append("</SolutionConfiguration>");
            m_cscc = scc.ToString().Replace("&", "&amp;");

            m_cursor = 0;
            m_summarySent = false;
            m_ok = 0;
            m_failed = 0;
            m_diffChecks = 0;
            m_diffErrors = 0;
            m_pending.Clear();
            m_all.Reset();
            m_all.Start();

            Dictionary<string, object> ack =
                new Dictionary<string, object>();
            ack["ok"] = true;
            ack["projects"] = m_projects.Count;
            return m_json.Serialize(ack);
        }

        /**
         * 事件游标推进：待发队列非空则弹队首；否则求值下一个成员工程
         * 产出 progress + project 两事件；成员耗尽产出 summary（末事件、
         * 协议完成标志）。summary 已发后再调用抛异常（协议违例，经
         * ObjCall 回 -13）。
         */
        public string NextEvent()
        {
            if (m_projects == null)
            {
                throw new InvalidOperationException(
                    "NextEvent before Begin");
            }
            while (m_pending.Count == 0)
            {
                if (m_summarySent)
                {
                    throw new InvalidOperationException(
                        "no more events after summary");
                }
                if (m_cursor >= m_projects.Count)
                {
                    EmitSummary();
                    break;
                }
                EvaluateProject(m_projects[m_cursor]);
                m_cursor++;
            }
            string head = m_pending[0];
            m_pending.RemoveAt(0);
            return head;
        }

        /** 引用计数归零钩子：ProjectCollection.Dispose（纯反射）。 */
        public void OnFinalRelease()
        {
            if (m_collection == null)
            {
                return;
            }
            MethodInfo dispose = m_collection.GetType().GetMethod(
                "Dispose", Type.EmptyTypes);
            if (dispose != null)
            {
                dispose.Invoke(m_collection, null);
            }
        }

        // ------------------------------------------------------------------
        // 求值内核（金标六步的移植）。
        // ------------------------------------------------------------------

        /** 单工程求值：产出 progress 与 project 两事件入待发队列。 */
        void EvaluateProject(ProjectSpec spec)
        {
            Dictionary<string, object> progress =
                new Dictionary<string, object>();
            progress["type"] = "progress";
            progress["project"] = spec.Name;
            Dictionary<string, object> proj =
                new Dictionary<string, object>();
            proj["type"] = "project";
            proj["project"] = spec.Name;
            proj["name"] = spec.Name;
            proj["guid"] = spec.Guid;
            proj["vcxproj"] = spec.Vcxproj;
            proj["active_cfg"] = spec.Cfg;
            proj["active_platform"] = spec.Plat;
            proj["build_in_solution"] = spec.Build;
            try
            {
                Stopwatch sw = Stopwatch.StartNew();
                object project = LoadWithDegradedLeg(spec, proj);
                sw.Stop();
                proj["eval_ms"] = sw.ElapsedMilliseconds;

                // 顶层属性白名单。
                Dictionary<string, string> tp =
                    new Dictionary<string, string>();
                foreach (string p in TopProps)
                {
                    tp[p] = Val(project, p);
                }
                proj["top_props"] = tp;

                // ClCompile item definition（工程级编译设置的求值终值，
                // TU 级 diff 存储的基准）。
                object clDef = null;
                IEnumerable defs = GetProp(project, "ItemDefinitions")
                    as IEnumerable;
                if (defs != null)
                {
                    foreach (object d in defs)
                    {
                        if ((string)GetProp(d, "ItemType") == "ClCompile")
                        {
                            clDef = d;
                            break;
                        }
                    }
                }
                Dictionary<string, string> defDict =
                    new Dictionary<string, string>();
                if (clDef != null)
                {
                    for (int f = 0; f < MsbuildNames.Length; f++)
                    {
                        object md = Call(
                            clDef, "GetMetadata", MsbuildNames[f]);
                        object ev = md == null
                            ? null
                            : GetProp(md, "EvaluatedValue");
                        defDict[MsbuildNames[f]] =
                            ev == null ? "" : ev.ToString();
                    }
                }
                else
                {
                    for (int f = 0; f < MsbuildNames.Length; f++)
                    {
                        defDict[MsbuildNames[f]] = "";
                    }
                }
                proj["cl_item_definition"] = defDict;

                // item 计数。
                Dictionary<string, int> counts =
                    new Dictionary<string, int>();
                foreach (string t in ItemTypes)
                {
                    ICollection col = Call(project, "GetItems", t)
                        as ICollection;
                    if (col != null && col.Count > 0)
                    {
                        counts[t] = col.Count;
                    }
                }
                proj["item_counts"] = counts;

                // TU 清单：per-item 最终 metadata（求值器已并入 itemdef）
                // 按 diff-vs-itemdef 存储 + 回放自检（金标同款）。
                List<object> tus = new List<object>();
                IEnumerable items = Call(project, "GetItems", "ClCompile")
                    as IEnumerable;
                if (items != null)
                {
                    foreach (object it in items)
                    {
                        object incRaw = GetProp(it, "EvaluatedInclude");
                        string inc = incRaw == null ? "" : incRaw.ToString();
                        string absTu = inc;
                        try
                        {
                            absTu = Path.GetFullPath(Path.Combine(
                                Val(project, "MSBuildProjectDirectory"),
                                inc));
                        }
                        catch (Exception)
                        {
                            // 非法路径形态：保原样（金标同款兜底）。
                        }
                        Dictionary<string, object> tu =
                            new Dictionary<string, object>();
                        tu["file"] = inc;
                        tu["abs"] = absTu;
                        object exRaw = Call(
                            it, "GetMetadataValue", "ExcludedFromBuild");
                        string excluded = exRaw == null
                            ? ""
                            : exRaw.ToString();
                        if (excluded.Length > 0 && excluded != "false")
                        {
                            tu["excluded_from_build"] = excluded;
                        }
                        for (int f = 0; f < MsbuildNames.Length; f++)
                        {
                            object vRaw = Call(
                                it, "GetMetadataValue", MsbuildNames[f]);
                            string v = vRaw == null ? "" : vRaw.ToString();
                            if (v != defDict[MsbuildNames[f]])
                            {
                                tu[FieldNames[f]] = v;
                            }
                        }
                        // 回放自检：diff + itemdef 必须还原求值终值。
                        for (int f = 0; f < MsbuildNames.Length; f++)
                        {
                            object fRaw = Call(
                                it, "GetMetadataValue", MsbuildNames[f]);
                            string finalV =
                                fRaw == null ? "" : fRaw.ToString();
                            object dv;
                            string replay =
                                tu.TryGetValue(FieldNames[f], out dv)
                                    ? (string)dv
                                    : defDict[MsbuildNames[f]];
                            m_diffChecks++;
                            if (replay != finalV)
                            {
                                m_diffErrors++;
                            }
                        }
                        tus.Add(tu);
                    }
                }
                proj["tu_count"] = tus.Count;
                proj["tus"] = tus;

                // import 闭包（已解析形态）。
                List<string> imports = new List<string>();
                IEnumerable imps = GetProp(project, "Imports")
                    as IEnumerable;
                if (imps != null)
                {
                    foreach (object imp in imps)
                    {
                        object imported = GetProp(imp, "ImportedProject");
                        object full = imported == null
                            ? null
                            : GetProp(imported, "FullPath");
                        imports.Add(full == null ? "?" : full.ToString());
                    }
                }
                proj["import_closure_count"] = imports.Count;
                proj["import_closure"] = imports;

                m_ok++;
                progress["eval_ms"] = sw.ElapsedMilliseconds;
                progress["tu_count"] = tus.Count;
                progress["degraded"] =
                    proj.ContainsKey("degraded_missing_imports");
            }
            catch (Exception ex)
            {
                m_failed++;
                string msg = ex.GetType().Name + ": " + ex.Message
                    + " @ " + ex.StackTrace;
                proj["error"] = msg;
                progress["failed"] = true;
                progress["error"] = msg;
            }
            m_pending.Add(m_json.Serialize(progress));
            m_pending.Add(m_json.Serialize(proj));
        }

        /**
         * 工程装载主路径与降级腿：三参 LoadProject(file, global props,
         * toolsVersion=null) 正常装载；InvalidProjectFileException
         * （MSB4019/MSB4226 import 缺失）时按金标先例降级
         * IgnoreMissingImports 形态——ProjectRootElement.Open 后走
         * ProjectLoadSettings=97（1|32|64）的五参构造器，事件里标
         * degraded_missing_imports。
         */
        object LoadWithDegradedLeg(
            ProjectSpec spec, Dictionary<string, object> proj)
        {
            Dictionary<string, string> props = BaseProps(spec.Cfg, spec.Plat);
            try
            {
                return s_loadProject3.Invoke(
                    m_collection,
                    new object[] { spec.Vcxproj, props, null });
            }
            catch (TargetInvocationException ex)
            {
                Exception inner =
                    ex.InnerException != null ? ex.InnerException : ex;
                if (inner.GetType().Name != "InvalidProjectFileException")
                {
                    throw;
                }
                object root = s_rootOpen.Invoke(
                    null, new object[] { spec.Vcxproj, m_collection });
                object settings = Enum.ToObject(s_tLoadSettings, 97);
                object project = s_ctor5.Invoke(new object[] {
                    root, props, null, m_collection, settings });
                proj["degraded_missing_imports"] = true;
                return project;
            }
        }

        /** 求值全局属性（金标 15 键基座层：Rider 等价注入面）。 */
        Dictionary<string, string> BaseProps(string cfg, string plat)
        {
            Dictionary<string, string> d = new Dictionary<string, string>();
            d["Configuration"] = cfg;
            d["Platform"] = plat;
            d["SolutionDir"] = m_slnDir + "\\";
            d["SolutionExt"] = ".sln";
            d["SolutionName"] = Path.GetFileNameWithoutExtension(m_solution);
            d["SolutionFileName"] = Path.GetFileName(m_solution);
            d["SolutionPath"] = Path.GetFullPath(m_solution);
            d["VisualStudioVersion"] = m_vsVersion;
            d["BuildingInsideVisualStudio"] = "True";
            d["BuildProjectReferences"] = "False";
            d["BuildingByReSharper"] = "True";
            d["ResolveAssemblyReferencesSilent"] = "False";
            d["CurrentSolutionConfigurationContents"] = m_cscc;
            d["VCTargetsPath"] = m_vcTargets;
            d["VsInstallRoot"] = m_vsRoot;
            return d;
        }

        /** summary 事件（末事件、协议完成标志）与自检计数。 */
        void EmitSummary()
        {
            Dictionary<string, object> s = new Dictionary<string, object>();
            s["type"] = "summary";
            s["ok"] = m_ok;
            s["failed"] = m_failed;
            s["total_ms"] = m_all.ElapsedMilliseconds;
            Dictionary<string, object> sc =
                new Dictionary<string, object>();
            sc["tu_field_checks"] = m_diffChecks;
            sc["mismatches"] = m_diffErrors;
            s["self_check"] = sc;
            m_pending.Add(m_json.Serialize(s));
            m_summarySent = true;
        }

        // ------------------------------------------------------------------
        // 反射与请求解析小助手（金标形态；显式 null 判去 ?. 语法糖）。
        // ------------------------------------------------------------------

        static object GetProp(object o, string n)
        {
            PropertyInfo p = o.GetType().GetProperty(n);
            return p == null ? null : p.GetValue(o, null);
        }

        static object Call(object o, string m, params object[] args)
        {
            Type[] types = new Type[args.Length];
            for (int i = 0; i < args.Length; i++)
            {
                types[i] = args[i].GetType();
            }
            MethodInfo mi = o.GetType().GetMethod(m, types);
            return mi == null ? null : mi.Invoke(o, args);
        }

        static string Val(object project, string name)
        {
            object v = Call(project, "GetPropertyValue", name);
            return v == null ? "" : v.ToString();
        }

        static string Str(Dictionary<string, object> d, string key)
        {
            object v;
            d.TryGetValue(key, out v);
            string s = v as string;
            return s == null ? "" : s;
        }

        static bool BoolOf(Dictionary<string, object> d, string key)
        {
            object v;
            if (!d.TryGetValue(key, out v) || v == null)
            {
                return false;
            }
            if (v is bool)
            {
                return (bool)v;
            }
            return string.Equals(
                Convert.ToString(v),
                "true",
                StringComparison.OrdinalIgnoreCase);
        }
    }
}
