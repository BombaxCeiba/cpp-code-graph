module;
// 经典流头预热：util::read_text_file 的导出内联体（istream::read）在
// 本接口单元的实例化点需要经典 <istream> 的完整定义在场（std 模块
// 的导出声明面单独不足）。
#include <istream>
#include <ostream>

/**
 * @module cg.msbuild.eval
 *
 * @brief MSBuild 求值通道的宿主侧客户端：发现链推导（程序集目录、VS 根、
 *        VCTargetsPath、VisualStudioVersion）与 cg.util.cshost 桥上的
 *        MsbuildEval 会话驱动（Begin 请求组装、NextEvent 事件流消费、
 *        看门狗限时与失败语义）。
 *
 * 求值面钉 51 线（VS 自带 Microsoft.Build.dll 是 net472 程序集，只有
 * CLR4 家族装得下）：会话经 cshost::try_open_msbuild_eval 建立（失败时
 * 带回错误码，供重试与诊断），不走 Select-String
 * 面的四候选降级链；51 线不可用（垫片缺席 / CLR 起不来）时 evaluate 以
 * 指引性诊断失败（装载失败形态），调用方按失败保旧处理。
 *
 * 事件流协议（垫片侧 MsbuildEval 类）：每工程先 progress（求值结论行）
 * 后 project（工程终态载荷：top_props/cl_item_definition/tus/import_closure），
 * 末事件 summary 是协议完成标志。本模块只把求值元数据过桥拿到结构化
 * 形态；argv 组装是组装器（cg.msbuild.compose）的职责，不在此层。
 *
 * 看门狗（进程内同步调用无法硬杀）：每次通道调用在工作线程执行、会话
 * 线程限时等待（CCG_EVAL_TIMEOUT_MS 可配，默认 300 秒——金标 4.5 秒 /
 * 59 工程锚的 66 倍余量）；超时放弃等待并 abandon 会话（此后不拆除——
 * 垫片孤儿线程仍在使用对象，拆除等于对托管对象的 use-after-free，
 * 对象与孤儿线程一起活到进程退出）。
 *
 * 刷新快照（内容哈希快照门）：本模块还持有求值产物的身份记录——成功求值
 * 采集的尾部把输入集（sln + 成员 vcxproj + import 闭包 + INCLUDE 哈希 +
 * 工具链路径 + ccg 版本）写进 <root>/.ccg/msbuild/<stem>/snapshot.json，
 * ccg update 的项目文件联动读它做"要不要重采"的判定（全等 = 事件被现有
 * 语料吸收，skipped-unchanged）。快照只在采集成功后写——失败保旧同步
 * 保快照。
 *
 * @internal 发现链全部输入来自既有链，不新增注册表路线：程序集目录 =
 * dirname(find_msbuild(...))（vswhere 的 -find 通配命中 Bin/MSBuild.exe，
 * 同目录即 VS 自带 net472 布局）；VS 根 = msbuild.exe 路径沿父链回推
 * （形态意外时回退 vswhere -property installationPath）；VCTargetsPath =
 * <VS根>/MSBuild/Microsoft/VC/ 下 vNNN 目录降序的存在性验证（两个守卫
 * props 同在者胜出，尾加反斜杠是求值硬约束）；成员清单 = pipeline 的
 * enumerate_sln_projects（ActiveCfg 与 Build.0 参与位同趟解析——sln
 * 解析留在 C++ 侧单点，垫片不重复实现）。
 */

export module cg.msbuild.eval;

import std;
import cg.util;
import cg.util.cstd;
import cg.msbuild.pipeline; // find_msbuild / enumerate_sln_projects / SlnProject

export namespace cg::msbuild::eval
{

    /** @brief Begin 请求里的成员工程（ccg 侧 sln 解析的强类型产物）。 */
    struct EvalProjectIn
    {
        std::string name;    ///< sln 条目名
        std::string vcxproj; ///< 绝对 .vcxproj（正斜杠）
        std::string guid;    ///< "{...}"（sln 原样，含花括号）
        std::string cfg;     ///< 成员 ActiveCfg 的配置段（"Debug"）
        std::string plat;    ///< 成员 ActiveCfg 的平台段（"x64"）
        bool build = true;   ///< Build.0 参与位（披露字段；组装侧过滤依据）
    };

    /**
     * @brief 一次求值会话的完整请求（Begin 载荷的强类型面 + ObjCreate
     *        构造参数）。
     */
    struct EvalRequest
    {
        std::string solution;     ///< sln 绝对路径
        std::string config;       ///< 解决方案配置（"Debug"）
        std::string platform;     ///< 解决方案平台（"x64"）
        std::string vs_root;      ///< VS 安装根（VsInstallRoot 注入值）
        std::string vc_targets;   ///< VCTargetsPath（尾反斜杠）
        std::string vs_version;   ///< VisualStudioVersion（"18.0"）
        std::string assembly_dir; ///< MSBuild.exe 同目录（ObjCreate 构造参数）
        std::vector<EvalProjectIn> projects; ///< 成员清单（ActiveCfg 非空者）
    };

    /** @brief 单 TU 的枚举产物（37 字段完整面留在工程级 raw_json）。 */
    struct EvalTu
    {
        std::string file;                ///< EvaluatedInclude 原样
        std::string abs;                 ///< 绝对化（工程目录合成）
        std::string excluded_from_build; ///< 非空且非 "false" 时的原样值
    };

    /**
     * @brief 单工程求值产物（project 事件的结构化面 + 原始 JSON 全文）。
     */
    struct EvalProjectOut
    {
        std::string name;
        std::string guid;
        std::string vcxproj;
        std::string active_cfg;
        std::string active_platform;
        bool        build_in_solution = true;
        bool        degraded_missing_imports = false;
        std::string error; ///< 空 = 成功；非空 = 工程级失败（不中断会话）
        // —— 断言与披露用的关键字段提炼（完整 37 字段面在 raw_json）——
        std::string platform_toolset; ///< top_props 的 PlatformToolset
        std::string
            language_standard; ///< cl_item_definition 的 LanguageStandard
        std::string
            defines; ///< cl_item_definition 的 PreprocessorDefinitions 原样
        std::size_t         tu_count = 0;
        std::vector<EvalTu> tus;
        /** @brief 已解析 import 闭包的绝对路径（降级腿未解析的条目是
         *         "?"——快照构建侧跳过该占位）。 */
        std::vector<std::string> import_closure;
        std::string raw_json; ///< project 事件 JSON 全文（组装器与取证面）
    };

    /**
     * @brief 一轮求值会话的终态。
     *
     * completed 为真（拉到 summary）时 ok/failed_projects 是 summary 计数，
     * projects 含每个成员的结构化产物（失败工程带 error 字段，不中断）。
     * completed 为假 = 通道级失败（通道失败形态：线不可用 / 通道错误码 / 看门狗
     * 超时 / 坏事件行），error 携带诊断文本，timed_out 单独标记看门狗
     * 放弃等待（会话已 abandon，不再拆除）。
     */
    struct EvalOutcome
    {
        bool                        completed = false;
        bool                        timed_out = false;
        std::size_t                 ok_projects = 0;
        std::size_t                 failed_projects = 0;
        std::vector<EvalProjectOut> projects;
        std::string                 error;
        std::size_t tu_field_checks = 0; ///< summary 自检：回放校验次数
        std::size_t mismatches = 0;      ///< summary 自检：失配次数（须为 0）
    };

    // -----------------------------------------------------------------------
    // 发现链推导（纯文本与目录探测，零注册表路线）。
    // -----------------------------------------------------------------------

    /**
     * @brief VS 安装根推导。
     *
     * 主路径是 msbuild.exe 路径沿父链回推：VS 布局里 MSBuild 目录直接
     * 挂在实例根下（<VS根>/MSBuild/<ver>/Bin/MSBuild.exe，BuildTools 与
     * Community 两种段深都覆盖），找到名为 MSBuild 的目录后用其父下
     * MSBuild/Microsoft/VC 子树存在性验证（与 vc_targets_dir_of 的探测
     * 锚同源）；形态意外（路径不是 VS 布局）时回退 vswhere
     * -latest -products * -property installationPath（run_capture 同形态
     * 的讯问）。两者都失败返回 ""（build_evaluate_request 据此拒绝）。
     */
    [[nodiscard]]
    auto vs_root_of(const std::string& msbuild_exe) -> std::string;

    /**
     * @brief VCTargetsPath 推导（尾反斜杠是求值硬约束）。
     *
     * 列 <vs_root>/MSBuild/Microsoft/VC/ 下 vNNN 目录按名字降序（三位
     * 定宽，名字序即版本序），取第一个同时含 Microsoft.Cpp.Default.props
     * 与 Microsoft.Cpp.Common.props 者——"存在性验证"而非"版本号最高即
     * 对"（不完整的高版本 VC targets 目录必须跳过）。无候选返回 ""。
     */
    [[nodiscard]]
    auto vc_targets_dir_of(const std::string& vs_root) -> std::string;

    /**
     * @brief VisualStudioVersion 推导：VS 根主版本号 + ".0"。
     *
     * 布局 <...>/<主版本>/<版别>（18/Community → "18.0"）取倒数第二段；
     * 形态意外（根下无版本段）返回 ""。
     */
    [[nodiscard]]
    auto vs_version_of(const std::string& vs_root) -> std::string;

    /**
     * @brief 组装 Begin 请求（发现链全推导 + sln 成员解析 + Build.0 位）。
     *
     * @param msbuild_exe find_msbuild 的结果（空串直接返回 nullopt）。
     * @param sln         .sln 绝对路径。
     * @param config      解决方案配置（"Debug"）。
     * @param platform    解决方案平台（"x64"）。
     *
     * @return 完整请求；发现链不可推导（VS 根 / VCTargetsPath 任一为空）
     *         返回 std::nullopt。ActiveCfg 无映射的成员不进清单（求值
     *         无法定配置，Rider 形态跳过）；Build.0 参与位照实进字段。
     */
    [[nodiscard]]
    auto build_evaluate_request(
        const std::string& msbuild_exe,
        const std::string& sln,
        const std::string& config,
        const std::string& platform) -> std::optional<EvalRequest>;

    /**
     * @brief 驱动一轮完整求值会话（open → Begin → NextEvent 流 → summary）。
     *
     * 成功路径：拉到 summary 后会话经 CsRef 析构归零拆除（垫片侧
     * OnFinalRelease 释放 ProjectCollection）。失败路径全部带着已收
     * 集的工程产物返回（completed=false + error 文本），不抛异常——
     * 失败保旧语义的裁定权在调用方（CLI 分派层）。
     */
    [[nodiscard]]
    auto evaluate(const EvalRequest& request) -> EvalOutcome;

    // -----------------------------------------------------------------------
    // 刷新快照（求值产物的身份哈希；ccg update 联动的哈希门成对消费）。
    // -----------------------------------------------------------------------

    /** @brief 快照的单文件条目：规范化路径 + 内容哈希（写侧与读侧都把
     *         缺失文件记 ""——两侧同缺即视为未变化，文件回来即失配）。 */
    struct RefreshSnapshotFile
    {
        std::string path;
        std::string sha256;
    };

    /** @brief 环境身份四元组（全部参与门比对）。 */
    struct RefreshSnapshotEnv
    {
        std::string include;    ///< 当前进程 INCLUDE 的 sha256（十六进制）
        std::string msbuild;    ///< MSBuild.exe 绝对路径（正斜杠规范化）
        std::string cl;         ///< 主工程 cl.exe 绝对路径（正斜杠规范化）
        std::string vc_targets; ///< VCTargetsPath（尾反斜杠原生形态）
    };

    /**
     * @brief 一次成功求值采集的输入身份记录。
     *
     * 写侧 = `ccg msbuild`（求值通道）尾部；读侧 = `ccg update` 的项目
     * 文件联动（哈希门）。文件集 = sln + 全部成员 vcxproj + 全部工程
     * import 闭包（求值腿免费产出），环境集 = INCLUDE 哈希 + msbuild/
     * cl/VCTargetsPath 路径，另加 ccg 版本——这些合起来是"这份语料由
     * 哪些输入决定"的完整答案，全等即重采无收益。
     */
    struct RefreshSnapshot
    {
        std::vector<RefreshSnapshotFile> files;
        RefreshSnapshotEnv               env;
        std::string                      ccg_version;
        std::string                      written_at; ///< ISO-8601 UTC
    };

    /**
     * @brief 快照路径：`<root>/.ccg/msbuild/<stem>/snapshot.json`。
     *
     * 写侧与读侧共用本单点（与捕获锁同区的运行期工作区）；stem 是注册表
     * 条目名（如 "Debug-x64"）。
     */
    [[nodiscard]]
    auto refresh_snapshot_path(
        const std::string& project_root,
        const std::string& stem) -> std::string;

    /**
     * @brief 从一次求值会话的产物构建刷新快照。
     *
     * 文件集合并 sln、逐成员 vcxproj 与逐工程 import 闭包，norm_path
     * 规范化后去重排序（确定性输出）；每文件内容哈希，缺失文件记 ""
     * （不拦截——写侧只记录事实）。INCLUDE 与三条工具链路径进 env 身
     * 份四元组。
     */
    [[nodiscard]]
    auto build_refresh_snapshot(
        const std::string&                 msbuild_exe,
        const std::string&                 cl,
        const EvalRequest&                 request,
        const std::vector<EvalProjectOut>& projects) -> RefreshSnapshot;

    /**
     * @brief 快照序列化落盘（暂存 + 原子替换，父目录按需创建）。
     *
     * 只在采集成功后由调用方调用——失败保旧同步保快照。
     */
    auto save_refresh_snapshot(
        const std::string&     path,
        const RefreshSnapshot& snapshot) -> void;

    /**
     * @brief 读取快照。文件缺席或不是有效 JSON 都返回 nullopt（读侧
     *         按无快照处理 = 触发重采，绝不因坏快照中止更新）。
     */
    [[nodiscard]]
    auto load_refresh_snapshot(const std::string& path)
        -> std::optional<RefreshSnapshot>;

    /**
     * @brief 哈希门判定：快照与当前输入全等吗？
     *
     * @param snapshot          读到的快照。
     * @param registry_msbuild  注册表记录的 MSBuild.exe（下一轮采集的发现
     *                          缓存复用值——缓存失效即路径消失时返回不等，
     *                          让采集重新发现而不是拿旧身份跳过）。
     * @param registry_cl       注册表记录的主 cl（同上口径）。
     * @param current_vc_targets 按当前 msbuild 现场推导的 VCTargetsPath
     *                          （vs_root_of + vc_targets_dir_of，与采集侧
     *                          build_evaluate_request 同一条推导链）。
     *
     * @return 空串 = 全等（事件可被现有语料吸收）；非空 = 第一处失配的
     *         人读原因（verbose 披露用）。零文件条目视为形坏，返回不等。
     */
    [[nodiscard]]
    auto refresh_snapshot_unchanged(
        const RefreshSnapshot& snapshot,
        const std::string&     registry_msbuild,
        const std::string&     registry_cl,
        const std::string&     current_vc_targets) -> std::string;

} // namespace cg::msbuild::eval

// ---------------------------------------------------------------------------
// 实现。
// ---------------------------------------------------------------------------
namespace cg::msbuild::eval
{

    namespace
    {

        namespace fs = std::filesystem;
        namespace plat = cg::util::platform;

        /** utf-8 路径文本到 fs::path（Windows 下窄串按代码页误读非 ASCII
         *  路径——夹具语料含 ü 名目录，必须经宽形态中转）。 */
        [[nodiscard]]
        auto fs_path_from_utf8(std::string_view utf8) -> fs::path
        {
#if defined(_WIN32)
            return fs::path{plat::widen_utf8(utf8)};
#else
            return fs::path{utf8};
#endif
        }

        /** 目录名形态的守卫 props 探测锚（与 vc_targets_dir_of 共用）。 */
        [[nodiscard]]
        auto dir_exists(const fs::path& dir) -> bool
        {
            std::error_code ec;
            return fs::is_directory(dir, ec);
        }

        [[nodiscard]]
        auto file_exists(const fs::path& p) -> bool
        {
            std::error_code ec;
            return fs::is_regular_file(p, ec);
        }

        /** 讯问子进程（vswhere 回退专用；run_capture 同形态：安静、
         *  捕获 stdout、无 stdin、继承环境）。 */
        struct RunResult
        {
            bool        started = false;
            int         exit_code = -1;
            std::string out;
        };

        auto run_capture(const std::wstring& exe, const std::wstring& args)
            -> RunResult
        {
            plat::ChildRunSpec spec;
            spec.cmdline =
                std::format(L"{} {}", plat::quote_for_command_line(exe), args);
            spec.stdin_mode = plat::ChildStdin::NullHandle;
            spec.create_no_window = true;
            const plat::ChildRunResult run = plat::run_child(spec);
            RunResult                  result;
            result.started = run.started && run.pipes_ok;
            result.exit_code = run.exit_code;
            result.out = std::move(run.out);
            return result;
        }

        /** 首条非空 stdout 行。 */
        [[nodiscard]]
        auto first_output_line(const std::string& output) -> std::string
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

        /** vswhere 可执行名（规范 Installer 位置优先，缺席退回裸名交
         *  PATH 搜索；环境读取经 cstd 封装——MSVC 侧走安全变体）。 */
        [[nodiscard]]
        auto resolve_vswhere() -> std::wstring
        {
            if (const std::optional<std::string> x86 =
                    cg::util::cstd::get_env("ProgramFiles(x86)");
                x86.has_value() && !x86->empty())
            {
                const fs::path  candidate = fs_path_from_utf8(*x86)
                                            / L"Microsoft Visual Studio"
                                            / L"Installer" / L"vswhere.exe";
                std::error_code ec;
                if (fs::is_regular_file(candidate, ec))
                {
                    return candidate.wstring();
                }
            }
            return L"vswhere.exe";
        }

        /** vswhere 回退讯问（-property installationPath 的首行）。 */
        [[nodiscard]]
        auto vswhere_installation_path() -> std::string
        {
            const RunResult r = run_capture(
                resolve_vswhere(),
                L"-latest -products * -property installationPath");
            if (!r.started || r.exit_code != 0)
            {
                return {};
            }
            return first_output_line(r.out);
        }

        /** active_cfg（"Debug|x64"）拆两段；无 '|' 时整体作配置、平台空。 */
        [[nodiscard]]
        auto split_cfg(const std::string& active_cfg)
            -> std::pair<std::string, std::string>
        {
            const std::size_t bar = active_cfg.find('|');
            if (bar == std::string::npos)
            {
                return {active_cfg, {}};
            }
            return {active_cfg.substr(0, bar), active_cfg.substr(bar + 1)};
        }

        /** Begin 请求 JSON（json 门面构造；载荷键序对齐垫片侧解析）。 */
        [[nodiscard]]
        auto build_begin_payload(const EvalRequest& req) -> std::string
        {
            namespace json = cg::util::json;
            json::Array projects;
            for (const EvalProjectIn& p : req.projects)
            {
                json::Object entry;
                entry.set("name", p.name);
                entry.set("vcxproj", p.vcxproj);
                entry.set("guid", p.guid);
                entry.set("cfg", p.cfg);
                entry.set("plat", p.plat);
                entry.set("build", p.build);
                projects.add(std::move(entry));
            }
            json::Object root;
            root.set("schema", 1);
            root.set("solution", req.solution);
            root.set("config", req.config);
            root.set("platform", req.platform);
            root.set("vs_root", req.vs_root);
            root.set("vc_targets", req.vc_targets);
            root.set("vs_version", req.vs_version);
            root.set("projects", std::move(projects));
            return json::stringify(std::move(root));
        }

        /** 看门狗预算：CCG_EVAL_TIMEOUT_MS（毫秒，正值生效）；默认
         *  300 秒。 */
        [[nodiscard]]
        auto timeout_budget() -> std::chrono::milliseconds
        {
            using std::chrono::milliseconds;
            if (const std::optional<std::string> raw =
                    cg::util::cstd::get_env("CCG_EVAL_TIMEOUT_MS");
                raw.has_value() && !raw->empty())
            {
                try
                {
                    const long long v = std::stoll(*raw);
                    if (v > 0)
                    {
                        return milliseconds{v};
                    }
                }
                catch (const std::exception&)
                {
                    // 非数值覆盖：按默认预算（不因配置笔误放大等待）。
                }
            }
            return milliseconds{300'000};
        }

        /**
         * 值语义的通道调用载荷（孤儿线程自持：超时放弃等待后线程仍在
         * 跑，全部引用必须自持有——span/string_view 禁止跨线程存续）。
         */
        struct CallPack
        {
            cg::util::cshost::CsRef session; // 拷贝 = AddRef（线程安全）
            std::string             method;
            std::vector<std::pair<std::string, std::string>> args;
        };

        /**
         * 工作线程执行通道调用、会话线程限时等待。正常返回调用结果；
         * 超时返回 std::nullopt（工作线程 detach 成孤儿——其通道调用
         * 继续跑至完成或进程退出，垫片对象经 abandon 短路保留）。
         */
        auto watched_call(
            CallPack                              pack,
            std::chrono::steady_clock::time_point deadline)
            -> std::optional<cg::util::cshost::ObjOutcome>
        {
            std::promise<cg::util::cshost::ObjOutcome> promise;
            std::future<cg::util::cshost::ObjOutcome>  future =
                promise.get_future();
            std::thread worker(
                [pack = std::move(pack), promise = std::move(promise)]() mutable
                {
                    std::vector<cg::util::cshost::ObjArg> argv;
                    argv.reserve(pack.args.size());
                    for (const auto& [name, value] : pack.args)
                    {
                        argv.push_back({name, value});
                    }
                    promise.set_value(pack.session.call(pack.method, argv));
                });
            if (future.wait_until(deadline) == std::future_status::ready)
            {
                worker.join();
                return future.get();
            }
            worker.detach(); // 孤儿线程：设计记档成本（无正确性影响）
            return std::nullopt;
        }

        /** project 事件 JSON → 结构化产物（解析失败按坏行处理）。 */
        [[nodiscard]]
        auto parse_project_event(
            const std::string&               payload,
            const cg::util::json::ValueView& root) -> EvalProjectOut
        {
            namespace json = cg::util::json;
            EvalProjectOut p;
            p.raw_json = payload;
            const auto str = [&root](std::string_view key) -> std::string
            {
                const auto v = root.get(key);
                if (!v || !v->is_string())
                {
                    return {};
                }
                return std::string{*v->as_string()};
            };
            p.name = str("name");
            p.guid = str("guid");
            p.vcxproj = str("vcxproj");
            p.active_cfg = str("active_cfg");
            p.active_platform = str("active_platform");
            if (const auto b = root.get("build_in_solution"); b && b->is_bool())
            {
                p.build_in_solution = *b->as_bool();
            }
            if (const auto d = root.get("degraded_missing_imports");
                d && d->is_bool())
            {
                p.degraded_missing_imports = *d->as_bool();
            }
            p.error = str("error");
            if (const auto tp = root.get("top_props"); tp && tp->is_object())
            {
                if (const auto v = tp->get("PlatformToolset");
                    v && v->is_string())
                {
                    p.platform_toolset = std::string{*v->as_string()};
                }
            }
            if (const auto def = root.get("cl_item_definition");
                def && def->is_object())
            {
                if (const auto v = def->get("LanguageStandard");
                    v && v->is_string())
                {
                    p.language_standard = std::string{*v->as_string()};
                }
                if (const auto v = def->get("PreprocessorDefinitions");
                    v && v->is_string())
                {
                    p.defines = std::string{*v->as_string()};
                }
            }
            if (const auto tus = root.get("tus"); tus && tus->is_array())
            {
                const std::size_t n = tus->size();
                p.tus.reserve(n);
                for (std::size_t i = 0; i < n; ++i)
                {
                    const auto it = tus->at(i);
                    if (!it || !it->is_object())
                    {
                        continue;
                    }
                    EvalTu tu;
                    if (const auto v = it->get("file"); v && v->is_string())
                    {
                        tu.file = std::string{*v->as_string()};
                    }
                    if (const auto v = it->get("abs"); v && v->is_string())
                    {
                        tu.abs = std::string{*v->as_string()};
                    }
                    if (const auto v = it->get("excluded_from_build");
                        v && v->is_string())
                    {
                        tu.excluded_from_build = std::string{*v->as_string()};
                    }
                    p.tus.push_back(std::move(tu));
                }
            }
            if (const auto c = root.get("tu_count"); c && c->is_number())
            {
                if (const auto v = c->as_int())
                {
                    p.tu_count = static_cast<std::size_t>(*v);
                }
            }
            if (const auto closure = root.get("import_closure");
                closure && closure->is_array())
            {
                const std::size_t n = closure->size();
                p.import_closure.reserve(n);
                for (std::size_t i = 0; i < n; ++i)
                {
                    const auto it = closure->at(i);
                    if (!it || !it->is_string())
                    {
                        continue;
                    }
                    p.import_closure.push_back(std::string{*it->as_string()});
                }
            }
            return p;
        }

    } // namespace

    auto vs_root_of(const std::string& msbuild_exe) -> std::string
    {
        if (msbuild_exe.empty())
        {
            return {};
        }
        // 沿父链上溯（VS 布局内 MSBuild.exe 到 MSBuild 目录至多隔 2 层
        // Bin/<ver> 段；上限 5 层兜底异常深布局）找名为 MSBuild 的目录。
        fs::path cur = fs_path_from_utf8(msbuild_exe).parent_path();
        for (int depth = 0; depth < 5 && cur.has_parent_path(); ++depth)
        {
            if (cg::util::ascii_iequals(cur.filename().string(), "MSBuild"))
            {
                const fs::path candidate = cur.parent_path();
                if (dir_exists(candidate / "MSBuild" / "Microsoft" / "VC"))
                {
                    return candidate.generic_string();
                }
            }
            cur = cur.parent_path();
        }
        return vswhere_installation_path();
    }

    auto vc_targets_dir_of(const std::string& vs_root) -> std::string
    {
        if (vs_root.empty())
        {
            return {};
        }
        const fs::path vc_root =
            fs_path_from_utf8(vs_root) / "MSBuild" / "Microsoft" / "VC";
        if (!dir_exists(vc_root))
        {
            return {};
        }
        std::vector<std::string> names;
        std::error_code          ec;
        for (const fs::directory_entry& entry :
             fs::directory_iterator(vc_root, ec))
        {
            std::string name = entry.path().filename().string();
            if (name.size() >= 2 && (name[0] == 'v' || name[0] == 'V')
                && dir_exists(entry.path()))
            {
                names.push_back(std::move(name));
            }
        }
        if (names.empty())
        {
            return {};
        }
        std::ranges::sort(names, std::ranges::greater{});
        for (const std::string& name : names)
        {
            const fs::path dir = vc_root / fs_path_from_utf8(name);
            if (file_exists(dir / "Microsoft.Cpp.Default.props")
                && file_exists(dir / "Microsoft.Cpp.Common.props"))
            {
                // 尾反斜杠是 VCTargetsPath 的求值硬约束（金标探针实证）；
                // 返回原生形态（Windows 属性值惯例）。
                return dir.string() + "\\";
            }
        }
        return {};
    }

    auto vs_version_of(const std::string& vs_root) -> std::string
    {
        if (vs_root.empty())
        {
            return {};
        }
        // 布局 <...>/<主版本>/<版别>：倒数第二段是主版本号。
        const fs::path root = fs_path_from_utf8(vs_root);
        const fs::path parent = root.parent_path();
        if (parent.empty())
        {
            return {};
        }
        const std::string segment = parent.filename().string();
        if (segment.empty()
            || !std::ranges::all_of(
                segment,
                [](char c) { return c >= '0' && c <= '9'; }))
        {
            return {};
        }
        return segment + ".0";
    }

    auto build_evaluate_request(
        const std::string& msbuild_exe,
        const std::string& sln,
        const std::string& config,
        const std::string& platform) -> std::optional<EvalRequest>
    {
        if (msbuild_exe.empty() || sln.empty() || config.empty()
            || platform.empty())
        {
            return std::nullopt;
        }
        EvalRequest req;
        req.solution = sln;
        req.config = config;
        req.platform = platform;
        req.assembly_dir =
            fs_path_from_utf8(msbuild_exe).parent_path().generic_string();
        req.vs_root = vs_root_of(msbuild_exe);
        if (req.vs_root.empty())
        {
            return std::nullopt;
        }
        req.vc_targets = vc_targets_dir_of(req.vs_root);
        if (req.vc_targets.empty())
        {
            return std::nullopt;
        }
        req.vs_version = vs_version_of(req.vs_root);
        if (req.vs_version.empty())
        {
            return std::nullopt;
        }

        const std::string solution_cfg = std::format("{}|{}", config, platform);
        for (const SlnProject& member :
             enumerate_sln_projects(sln, solution_cfg))
        {
            if (member.active_cfg.empty())
            {
                continue; // 无 ActiveCfg 映射：求值无法定配置，跳过
            }
            EvalProjectIn in;
            in.name = member.name;
            in.vcxproj = member.path;
            in.guid = member.guid;
            std::tie(in.cfg, in.plat) = split_cfg(member.active_cfg);
            in.build = member.build;
            req.projects.push_back(std::move(in));
        }
        return req;
    }

    auto evaluate(const EvalRequest& request) -> EvalOutcome
    {
        namespace cshost = cg::util::cshost;
        EvalOutcome out;
#if !defined(_WIN32)
        out.error = "evaluation channel requires the 51-line CLR host "
                    "(Windows only)";
        return out;
#else
        std::expected<cshost::CsRef, std::int32_t> opened =
            cshost::try_open_msbuild_eval(
                plat::widen_utf8(request.assembly_dir));
        if (!opened)
        {
            // 开门失败带着真实错误码回来。失败后重试一次：首次装载
            // CLR 与引擎装配的暂时性失败会自愈；仍失败则错误码进诊断。
            opened = cshost::try_open_msbuild_eval(
                plat::widen_utf8(request.assembly_dir));
        }
        if (!opened)
        {
            out.error = std::format(
                "MsbuildEval session open failed, rc={} detail={}",
                opened.error(),
                std::string_view{
                    cshost::face_detail(cshost::Face::Evaluation)});
            return out;
        }
        cshost::CsRef session = std::move(*opened);

        const auto deadline =
            std::chrono::steady_clock::now() + timeout_budget();

        const std::string begin_json = build_begin_payload(request);
        CallPack          begin_pack;
        begin_pack.session = session;
        begin_pack.method = "Begin";
        begin_pack.args.emplace_back("request", begin_json);
        std::optional<cshost::ObjOutcome> begin =
            watched_call(std::move(begin_pack), deadline);
        if (!begin.has_value())
        {
            session.abandon();
            out.timed_out = true;
            out.error = "eval session timed out in Begin "
                        "(watchdog abandoned; CCG_EVAL_TIMEOUT_MS)";
            return out;
        }
        if (!begin->ok())
        {
            out.error = std::format(
                "Begin failed rc={} payload={}",
                begin->rc,
                begin->payload);
            return out;
        }

        for (;;)
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                session.abandon();
                out.timed_out = true;
                out.error = "eval session timed out (watchdog abandoned; "
                            "CCG_EVAL_TIMEOUT_MS)";
                return out;
            }
            CallPack pack;
            pack.session = session;
            pack.method = "NextEvent";
            std::optional<cshost::ObjOutcome> event =
                watched_call(std::move(pack), deadline);
            if (!event.has_value())
            {
                session.abandon();
                out.timed_out = true;
                out.error = "eval session timed out in NextEvent "
                            "(watchdog abandoned; CCG_EVAL_TIMEOUT_MS)";
                return out;
            }
            if (!event->ok())
            {
                out.error = std::format(
                    "NextEvent failed rc={} payload={}",
                    event->rc,
                    event->payload);
                return out;
            }
            std::optional<cg::util::json::Document> doc;
            try
            {
                doc.emplace(cg::util::json::Document::parse(event->payload));
            }
            catch (const std::exception& ex)
            {
                out.error = std::format(
                    "bad event line ({}): {}",
                    ex.what(),
                    event->payload.substr(
                        0,
                        std::min<std::size_t>(event->payload.size(), 400)));
                return out;
            }
            const cg::util::json::ValueView root = doc->root();
            const auto                      type = root.get("type");
            if (!type || !type->is_string())
            {
                out.error =
                    "event line missing type field: "
                    + event->payload.substr(
                        0,
                        std::min<std::size_t>(event->payload.size(), 400));
                return out;
            }
            const std::string_view kind = *type->as_string();
            if (kind == "progress")
            {
                continue; // 进度事件：结论已被随后的 project 事件承载
            }
            if (kind == "project")
            {
                out.projects.push_back(
                    parse_project_event(event->payload, root));
                continue;
            }
            if (kind == "summary")
            {
                out.completed = true;
                if (const auto v = root.get("ok"); v && v->is_number())
                {
                    if (const auto n = v->as_int())
                    {
                        out.ok_projects = static_cast<std::size_t>(*n);
                    }
                }
                if (const auto v = root.get("failed"); v && v->is_number())
                {
                    if (const auto n = v->as_int())
                    {
                        out.failed_projects = static_cast<std::size_t>(*n);
                    }
                }
                if (const auto sc = root.get("self_check");
                    sc && sc->is_object())
                {
                    if (const auto v = sc->get("tu_field_checks");
                        v && v->is_number())
                    {
                        if (const auto n = v->as_int())
                        {
                            out.tu_field_checks = static_cast<std::size_t>(*n);
                        }
                    }
                    if (const auto v = sc->get("mismatches");
                        v && v->is_number())
                    {
                        if (const auto n = v->as_int())
                        {
                            out.mismatches = static_cast<std::size_t>(*n);
                        }
                    }
                }
                return out;
            }
            out.error = std::format("unknown event type '{}'", kind);
            return out;
        }
#endif
    }

    // -----------------------------------------------------------------------
    // 刷新快照（哈希门）实现。
    // -----------------------------------------------------------------------

    namespace
    {

        /** 文件内容哈希（宽形态中转读盘——窄串在 Windows 下按代码页
         *  误读非 ASCII 路径）；缺失/不可读记 ""（与读侧同判）。 */
        [[nodiscard]]
        auto snapshot_file_hash(const std::string& path) -> std::string
        {
            std::ifstream in{fs_path_from_utf8(path), std::ios::binary};
            if (!in)
            {
                return {};
            }
            std::string bytes;
            char        chunk[65536];
            while (in.read(chunk, sizeof(chunk)) || in.gcount() > 0)
            {
                bytes.append(chunk, static_cast<std::size_t>(in.gcount()));
                if (!in)
                {
                    break;
                }
            }
            return cg::util::sha256_hex(std::as_bytes(std::span{bytes}));
        }

        /** INCLUDE 环境的 sha256（缺失环境按空串；原始字节口径——
         *  排版差异（大小写/分号）算变，宁多重采不漏变）。 */
        [[nodiscard]]
        auto include_env_hash() -> std::string
        {
            const std::string include =
                cg::util::cstd::get_env("INCLUDE").value_or("");
            return cg::util::sha256_hex(std::as_bytes(std::span{include}));
        }

    } // namespace

    auto refresh_snapshot_path(
        const std::string& project_root,
        const std::string& stem) -> std::string
    {
        return project_root + "/.ccg/msbuild/" + stem + "/snapshot.json";
    }

    auto build_refresh_snapshot(
        const std::string&                 msbuild_exe,
        const std::string&                 cl,
        const EvalRequest&                 request,
        const std::vector<EvalProjectOut>& projects) -> RefreshSnapshot
    {
        // 输入集 = sln + 成员 vcxproj + import 闭包；norm_path 规范化后
        // 进 set（去重 + 字典序 = 确定性输出）。闭包里 "?" 是降级腿的
        // 未解析占位，不是路径，跳过。
        std::set<std::string> inputs;
        inputs.insert(cg::util::norm_path(request.solution));
        for (const EvalProjectIn& p : request.projects)
        {
            inputs.insert(cg::util::norm_path(p.vcxproj));
        }
        for (const EvalProjectOut& p : projects)
        {
            for (const std::string& imported : p.import_closure)
            {
                if (imported.empty() || imported == "?")
                {
                    continue;
                }
                inputs.insert(cg::util::norm_path(imported));
            }
        }

        RefreshSnapshot snapshot;
        snapshot.files.reserve(inputs.size());
        for (const std::string& path : inputs)
        {
            snapshot.files.push_back(
                RefreshSnapshotFile{
                    .path = path,
                    .sha256 = snapshot_file_hash(path)});
        }
        snapshot.env.include = include_env_hash();
        snapshot.env.msbuild = cg::util::norm_path(msbuild_exe);
        snapshot.env.cl = cg::util::norm_path(cl);
        snapshot.env.vc_targets = request.vc_targets;
        snapshot.ccg_version = std::string{cg::util::version};
        snapshot.written_at = cg::util::cstd::iso8601_utc_timestamp();
        return snapshot;
    }

    auto save_refresh_snapshot(
        const std::string&     path,
        const RefreshSnapshot& snapshot) -> void
    {
        namespace json = cg::util::json;
        json::Array files;
        for (const RefreshSnapshotFile& f : snapshot.files)
        {
            json::Object entry;
            entry.set("path", f.path);
            entry.set("sha256", f.sha256);
            files.add(std::move(entry));
        }
        json::Object env;
        env.set("include", snapshot.env.include);
        env.set("msbuild", snapshot.env.msbuild);
        env.set("cl", snapshot.env.cl);
        env.set("vc_targets", snapshot.env.vc_targets);
        json::Object root;
        root.set("files", std::move(files));
        root.set("env", std::move(env));
        root.set("ccg_version", snapshot.ccg_version);
        root.set("written_at", snapshot.written_at);
        cg::msbuild::detail::atomic_write_text(
            path,
            json::stringify(std::move(root)),
            "refresh snapshot");
    }

    auto load_refresh_snapshot(const std::string& path)
        -> std::optional<RefreshSnapshot>
    {
        namespace json = cg::util::json;
        const std::optional<std::string> text = cg::util::read_text_file(path);
        if (!text.has_value())
        {
            return std::nullopt;
        }
        try
        {
            const json::Document  doc = json::Document::parse(*text);
            const json::ValueView root = doc.root();
            if (!root.is_object())
            {
                return std::nullopt;
            }
            RefreshSnapshot snapshot;
            if (const auto files = root.get("files");
                files && files->is_array())
            {
                const std::size_t count = files->size();
                for (std::size_t i = 0; i < count; ++i)
                {
                    const auto entry = files->at(i);
                    if (!entry || !entry->is_object())
                    {
                        continue;
                    }
                    RefreshSnapshotFile f;
                    if (const auto v = entry->get("path"); v && v->is_string())
                    {
                        f.path = std::string{*v->as_string()};
                    }
                    if (f.path.empty())
                    {
                        continue;
                    }
                    if (const auto v = entry->get("sha256");
                        v && v->is_string())
                    {
                        f.sha256 = std::string{*v->as_string()};
                    }
                    snapshot.files.push_back(std::move(f));
                }
            }
            if (const auto env = root.get("env"); env && env->is_object())
            {
                const auto str = [&env](std::string_view key) -> std::string
                {
                    const auto v = env->get(key);
                    if (!v || !v->is_string())
                    {
                        return {};
                    }
                    return std::string{*v->as_string()};
                };
                snapshot.env.include = str("include");
                snapshot.env.msbuild = str("msbuild");
                snapshot.env.cl = str("cl");
                snapshot.env.vc_targets = str("vc_targets");
            }
            if (const auto v = root.get("ccg_version"); v && v->is_string())
            {
                snapshot.ccg_version = std::string{*v->as_string()};
            }
            if (const auto v = root.get("written_at"); v && v->is_string())
            {
                snapshot.written_at = std::string{*v->as_string()};
            }
            return snapshot;
        }
        catch (const std::exception&)
        {
            // 坏行 = 无快照（读侧触发重采，绝不因坏快照中止更新）。
            return std::nullopt;
        }
    }

    auto refresh_snapshot_unchanged(
        const RefreshSnapshot& snapshot,
        const std::string&     registry_msbuild,
        const std::string&     registry_cl,
        const std::string&     current_vc_targets) -> std::string
    {
        if (snapshot.files.empty())
        {
            return "snapshot carries no file entries (corrupt?)";
        }
        if (snapshot.ccg_version != cg::util::version)
        {
            return std::format(
                "ccg version changed: {} -> {}",
                snapshot.ccg_version,
                cg::util::version);
        }
        if (snapshot.env.include != include_env_hash())
        {
            return "INCLUDE environment changed";
        }
        // msbuild/cl 的"当前值"口径 = 下一轮采集实际会用的值：发现缓存
        // 复用注册表路径（路径仍在盘上时），缓存失效（路径消失）即整轮
        // 重新发现——两种情形都算"变"，让采集落新身份。
        std::error_code ec;
        if (registry_msbuild.empty()
            || !fs::is_regular_file(fs_path_from_utf8(registry_msbuild), ec))
        {
            return std::format(
                "recorded MSBuild path no longer resolves: {}",
                registry_msbuild.empty() ? std::string{"<empty>"}
                                         : registry_msbuild);
        }
        if (snapshot.env.msbuild != cg::util::norm_path(registry_msbuild))
        {
            return std::format(
                "MSBuild path changed: {} -> {}",
                snapshot.env.msbuild,
                registry_msbuild);
        }
        if (registry_cl.empty()
            || !fs::is_regular_file(fs_path_from_utf8(registry_cl), ec))
        {
            return std::format(
                "recorded cl path no longer resolves: {}",
                registry_cl.empty() ? std::string{"<empty>"} : registry_cl);
        }
        if (snapshot.env.cl != cg::util::norm_path(registry_cl))
        {
            return std::format(
                "cl path changed: {} -> {}",
                snapshot.env.cl,
                registry_cl);
        }
        if (snapshot.env.vc_targets != current_vc_targets)
        {
            return std::format(
                "VCTargetsPath changed: {} -> {}",
                snapshot.env.vc_targets,
                current_vc_targets);
        }
        std::size_t changed = 0;
        std::string first;
        for (const RefreshSnapshotFile& f : snapshot.files)
        {
            if (snapshot_file_hash(f.path) == f.sha256)
            {
                continue;
            }
            if (first.empty())
            {
                first = f.path;
            }
            ++changed;
        }
        if (changed > 0)
        {
            return std::format(
                "{} input file{} changed since the snapshot (first: {})",
                changed,
                changed == 1 ? "" : "s",
                first);
        }
        return {};
    }

} // namespace cg::msbuild::eval
