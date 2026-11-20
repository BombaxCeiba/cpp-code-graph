/**
 * @module cg.clang.bmi
 *
 * @brief 按目标供应 std 模块源 + 引擎锁定的 BMI 自建缓存。
 *
 * 树没有编译数据库时，模块单元无法骑构建产出的 BMI——本分区让 ccg
 * 自己产出它们：按标准库实现发现 std 模块源（libc++ 经驱动器自身的
 * libc++.modules.json；libstdc++ 的 bits/std.cc 经驱动器或 g++；MSVC
 * STL 的 modules/std.ixx 经驱动器 include 列表自动探测到的工具集），
 * 按 (引擎, 配置, 工具集) 一次性构建 std + std.compat 到
 *   `<配置目录>`/bmi-self-`<platform>`/`<engine_key>`/（配置目录 =
 * `<文件夹>`/`<配置>`/）
 * （建目录时尽力请求 NTFS 压缩），用 clang-scan-deps 做模块单元的
 * P1689 扫描，以显式 -fmodule-file=`<dep>`=`<pcm>` 映射按拓扑序构建接口
 * BMI DAG（分区用 mod:part 拼写——绝不用 prebuilt 路径），并交给调用
 * 方合成编译命令，其解析投喂每个 import 的传递闭包（分区与整模块
 * 再导出都必须在每次消费编译被投喂）。
 *
 * @internal
 * 关于缺失的 inline 关键字：本分区的导出自由函数刻意非 inline。其
 * 函数体 odr-use std::filesystem 路径算法（lexically_normal 等），其
 * hidden-friend 运算符随之被序列化进本分区的 IFC；只要消费者的合并
 * 同时拉入足够多的其他 IFC 内容，MSVC 14.38-14.51 就会错误合并那份
 * 附着副本（C2801，`<filesystem>` 路径迭代器上的 "operator== must be
 * a non-static member"）——二分定位结论：仅从引用集中去掉
 * cg.clang.bmi 就消除该错误。类外定义把实例化留在本翻译单元内、
 * 不进 IFC。（clang/gcc 不受影响：接口单元中导出的非 inline 定义是
 * 普通的附着函数。）
 * 正确性按构造键控：缓存身份 = 引擎版本 + 规范生产者 flag 向量 +
 * stdlib 源内容，绝无 clang 侧的陈旧校验；失效沿模块 DAG 向下级联
 * （引擎变更整目录作废、配置键变更新子树、接口源内容变更级联重建
 * 其全部下游、pcm 字节与 mtime 永不作有效性、std 源变更随配置键
 * 自动失效）。缓存目录内的 manifest.json 记
 * 录每个 pcm 的溯源，使后续运行复用未变接口、只重建编辑的下游、
 * 清除离开 DAG 的模块。
 */
export module cg.clang.bmi;

import std;
import cg.util;
import cg.util.cstd;
import cg.clang.capi;
import cg.clang.driver;
import cg.clang.tu;
import cg.clang.compile_db;

export namespace cg::clang
{

    /**
     * @brief 一个标准库实现提供的 std 模块源（发现结果）。
     *
     * @internal 生产者布线 flag（扁平 argv 尾巴，如 modules json 声明的
     * -isystem 对）；消费者解析骑驱动器补充项。toolset_id 是折进配置键
     * 的工具链身份；note 在标准库实现为空（未发现）时给人看解释。
     */
    struct StdModuleSource
    {
        std::string stdlib_impl; // "libc++" | "libstdc++" | "msvc-stl" | …
        std::string std_path;    // 绝对 std 模块接口源
        std::string compat_path; // 绝对 std.compat 源，无则 ""
        std::vector<std::string> wiring;
        std::string              toolset_id; // 折进配置键的工具链身份
        std::string              note;       // stdlib_impl 为空时的人类解释
    };

    /** @brief 一条 P1689 规则（每翻译单元）。 */
    struct ScanProvide
    {
        std::string name;   // 逻辑名，分区携带 "mod:part"
        std::string source; // 绝对接口源路径
        bool        is_interface = true;
    };
    struct ScanRequire
    {
        std::string name;
        std::string source; // 扫描器无法归属时为 ""
    };
    struct ScanRule
    {
        std::vector<ScanProvide> provides;
        std::vector<ScanRequire> requires_names;
    };

    /**
     * @brief 一条清单记录（每个产出的 pcm）。
     *
     * @internal hash/mtime/size 是源的 stat 快路径与内容哈希；command 是
     * 规范生产 argv 的 sha256；pcm 是相对 engine_key 目录的文件名；
     * config 在 db 路线下是构建配置签名（自建路线为 ""）。
     */
    struct BmiEntry
    {
        std::string              name;      // 模块名（含分区拼写）
        std::string              source;    // 绝对接口源
        std::string              hash;      // 源字节的 sha256
        std::int64_t             mtime = 0; // 源 stat 快路径（file_clock 计数）
        std::int64_t             size = 0;
        std::string              command;        // 规范生产 argv 的 sha256
        std::vector<std::string> requires_names; // 直接 P1689 requires
        std::string              pcm;            // 相对 engine_key 目录的文件名
        /**
         * @brief 产出 pcm 的字节数（复用前校验；0 表示旧清单未记录）。
         *
         * @internal 旧清单没有该字段 → 0 → 保守重建（不迁移、不猜测）。
         * 尺寸校验挡住 0 字节与半截产物；sha256 可选且刻意不启用（每次
         * 复用读 30+ MB 会吃掉 hook 的 10s 预算）。
         */
        std::int64_t pcm_size = 0;
        bool         std_module = false; // std/std.compat 条目
        std::string  config;             // db 路线：构建配置签名（自建为 ""）
    };

    /**
     * @brief 缓存清单：增量运行判定复用所需的一切。
     *
     * @internal engine 是生产者的完整 clang_getClangVersion()；engine_key
     * 是写出时的缓存目录名；config_key 是规范生产者 flag 向量哈希。
     */
    struct BmiManifest
    {
        std::string           engine;
        std::string           engine_key;
        std::string           config_key;
        std::string           toolset_id;
        std::vector<BmiEntry> entries;
    };

    /**
     * @brief 判定错误文本是否为模块归属冲突诊断（同一声明在一处附着于
     *        命名模块、在另一处附着于 global module 的重复声明）。
     *
     * clang 的三条拼写（"declaration of 'X' in the global module follows
     * declaration in module N"、"… in module M follows declaration in the
     * global module"、"… in module M follows declaration in module N"）共享
     * "follows declaration in" 尾巴，且必然点名 global module 或两个命名
     * 模块之一。检测按子串锚各自独立查找（折行安全：诊断在终端列宽处
     * 断行时锚仍齐）。
     *
     * 典型工程形状：接口生成头经 `export { #include ... }` 与全局片段两路
     * 包含同一接口名（或两份生成头各出一份声明），命名模块侧与 global
     * module 侧各得一次声明——MSVC 的模块实现宽容此形状、真实构建通过，
     * clang 拒绝。归属选择是工程侧导出面决策，索引器不代为改写，只点名
     * 诊断（见 module_attachment_conflict_failure）。
     */
    [[nodiscard]]
    inline auto is_module_attachment_conflict(std::string_view text) -> bool
    {
        return text.find("follows declaration in") != std::string_view::npos
               && (text.find("in the global module") != std::string_view::npos
                   || text.find("in module ") != std::string_view::npos);
    }

    /**
     * @brief 模块归属冲突的点名失败文本：编译器诊断首行 + 两条工程侧
     *        修法（导出面决策留给工程自身）。
     *
     * @param first_line 编译器诊断（回显到首行足够定位；完整拼写已由
     *                  is_module_attachment_conflict 在全文上判定）。
     * @param module_name 可得的模块名：BMI 自建侧已知、传入点名；消费侧
     *                    诊断自带模块名、传空则文本不重复点名。
     */
    [[nodiscard]]
    inline auto module_attachment_conflict_failure(
        std::string_view first_line,
        std::string_view module_name) -> std::string
    {
        // 嵌套 format 调用先落局部变量：直接作为外层 format 的参数会让
        // MSVC 对外层格式串的编译期校验失手（C7595 非常量表达式）。
        const std::string named =
            module_name.empty() ? std::string{}
                                : std::format("module '{}' vs ", module_name);
        return std::format(
            "module-attachment-conflict: {} - one declaration reached two "
            "module attachments ({}the global module and a named module; "
            "MSVC tolerates this shape, clang rejects it); project-side "
            "options: (a) move the offending generated header includes into "
            "the `module;` global fragment and re-export just the needed "
            "names with `export using`, or (b) include each generated "
            "header exactly once inside the purview (drop the "
            "`export {{ #include ... }}` / interface-definition double "
            "include)",
            first_line,
            named);
    }

    struct SelfBuildOptions
    {
        std::string root; // 项目根（扫描与合成命令的工作目录）
        /**
         * @brief ccg 配置目录（`<文件夹>`/`<配置>`/）：BMI 缓存子树与 clang-cl
         *        垫片的锚。
         *
         * @internal 缓存落
         * `<配置目录>`/bmi-self-`<平台>`/`<引擎键>`/`<配置键>`/；
         * 引擎无关共享的垫片落配置目录的父（布局文件夹根）。空值仅限无
         * CLI 语境的调用方：回退 `<root>`/.ccg/self/。
         */
        std::string ccg_config_dir;
        std::string std_value = "c++23"; // 整条链的语言轴
        bool        force = false;       // --force/--full：重建每个 pcm
        bool        dry_run = false;     // 仅计划：不生产、不写
        /**
         * @brief cl 方言路由：进来的编译命令以 cl 模式记录（msbuild 适配
         * 器发出的形状）。
         *
         * @internal 链随后切换到 clang-cl 方言（配方 A）：std.pcm 取自
         * MSVC 工具集自身的 modules/std.ixx，生产与扫描用 cl 拼写、消费
         * 者以 --driver-mode=cl 加钉住的 target 与 3 轴解析。生产者保持
         * 引擎自己的 clang、经命令内 --driver-mode=cl flag 驱动（版本锁
         * 仍成立——同一二进制）。
         */
        bool cl_mode = false;
        /**
         * @brief cl 命令记录的 include 目录（/I 与 -imsvc 值，已绝对）：
         * 构建实际编译所对的工具集事实——std 源发现键控于此。
         *
         * @internal 空值回落驱动器自身的报告。
         */
        std::vector<std::string> cl_include_dirs;
        /**
         * @brief 模块 TU 的 cl 命令记录的 /D 定义值。
         *
         * @internal 整形头的宏（如 UNICODE）骑在静默轴上；合成解析
         * 必须复现它们，否则项目头的解析与构建时不同。
         */
        std::vector<std::string> cl_defines;
        /**
         * @brief 构建编排自身解析出的 MSVC 工具集目录（msbuild 流水线
         * 的 cl 发现，`<toolset>`/bin/HostX/x/cl.exe）。
         *
         * @internal cl 适配器刻意从记录 argv 剥掉工具集根（clang-cl 自
         * 行配置它们），故 VCToolsVersion 钉住的构建只能经此提示显形——
         * 没有它，链会钉住最新安装的工具集、把 14.44 构建的 std.pcm 静
         * 默对着 14.51 头编译。
         */
        std::string msvc_toolset_hint;
        /**
         * @brief 编译数据库路线（clang/gcc 方言）：模块单元来自响应文件
         * 展开后不带模块投喂的编译数据库（多配置生成器记录每个配置的
         * 命令，而磁盘上只有已构建配置的 .modmap rsp 与 pcm）。
         *
         * @internal 权威规则：单元的 BMI 生产命令与消费者解析 argv 是它
         * 记录的 argv 加依赖闭包投喂加发接口增补——绝不是兜底 flag 向量
         * （那会产出与数据库所记录不同的程序配置的 BMI）。生产者二进制
         * 保持引擎自己的 clang（版本锁：跨版本 pcm 被拒 rc=4）。
         */
        bool db_mode = false;
        /**
         * @brief 逐模块单元文件的数据库记录命令（权威）。
         *
         * @internal 不在映射中的单元（cl 路由的单元、或记录缺席者）无权威
         * 可守、回落基线 argv。
         */
        std::unordered_map<std::string, CompileCommand> recorded;
        /**
         * @brief 本趟服务的构建配置签名（诊断用）。
         *
         * @internal 路由层按（配置，语言选项向量）组分趟运行；有记录向
         * 量时标签内嵌该组的向量。
         */
        std::string db_config_label;
        /**
         * @brief 本趟组的 PCM 语言选项轴（db 路线）。
         *
         * @internal 路由层按记录 argv 的白名单向量（pcm_langopt_vector）
         * 拆分每个构建配置的无投喂条目、逐组运行——每组拥有一个 std
         * pcm 变体，在同一 LangOpts 形状下生产与消费。跨组 import 以
         * 点名的 module-build-failed 行失败，而非静默混形状。全同向量
         * 的数据库坍缩为一组。兜底/cl 链上为空（彼处无记录权威）。
         */
        std::vector<std::string> langopt_axes;
        /**
         * @brief BMI 接口层内并行构建的期望工人数；0 = 自动按核。
         *
         * @internal 层内提交上限 = min(本值或自动值, 层宽)——层宽天然封顶
         * （一层 3 个接口绝不开 8 路）；层间屏障保证下游只喂上游已定案的
         * 传递闭包，语义与逐个串行构建完全一致。std/std.compat 伪节点保持
         * 分层循环之前的串行前置，不受本值影响。这是统一并发上限的第一
         * 段（第二段为 TU 解析池）；旗标与库配置的贯通接线住在 CLI 层。
         */
        int jobs = 0;
    };

    struct SelfBuildResult
    {
        /**
         * @brief 模块单元（接口与实现）的合成编译命令，拓扑序、投喂已
         * 附着；降级单元携带 preordained_failure + scan_imports 而非投喂。
         */
        std::vector<CompileCommand> commands;
        std::vector<std::string>    notes;        // 供 CLI 打印的 stderr 注记
        std::string                 cache_dir;    // bmi 引擎目录（诊断/测试）
        std::size_t                 built = 0;    // 本次产出的 pcm
        std::size_t                 reused = 0;   // 确认可复用的 pcm
        std::size_t                 purged = 0;   // 移除的条目（离开 DAG/配置）
        std::size_t                 degraded = 0; // 不会解析的 TU（tier-1）
    };

    namespace detail
    {

        [[nodiscard]]
        auto hash_text(std::string_view text) -> std::string
        {
            return cg::util::sha256_hex(std::as_bytes(std::span{text}));
        }

        /**
         * @brief pcm 的清单拼写：相对引擎目录的通用形式（重整布局下的
         * "`<config_key>`/`<module>`.pcm"）。
         */
        [[nodiscard]]
        auto pcm_rel(const std::string& engine_dir, const std::string& pcm)
            -> std::string
        {
            const std::filesystem::path rel =
                std::filesystem::path{pcm}.lexically_relative(
                    std::filesystem::path{engine_dir});
            return rel.generic_string();
        }

        /**
         * @brief 模块名的 pcm 文件名。
         *
         * @internal [A-Za-z0-9._-] 之外的字符变 '_'，且拼写被改动时追加短名
         * 哈希——"demo:core" 与字面的 "demo_core" 绝不相撞。
         */
        [[nodiscard]]
        auto sanitize_pcm_name(std::string_view name) -> std::string
        {
            std::string out;
            out.reserve(name.size());
            bool changed = false;
            for (const char c : name)
            {
                const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                                || (c >= '0' && c <= '9') || c == '.'
                                || c == '_' || c == '-';
                out += ok ? c : '_';
                changed = changed || !ok;
            }
            if (changed)
            {
                out += '-';
                out += hash_text(name).substr(0, 8);
            }
            return out;
        }

        /** @brief 规范化通用字符串上的目录前缀测试。 */
        [[nodiscard]]
        auto under_dir(std::string_view path, std::string_view dir) -> bool
        {
            if (dir.empty())
            {
                return false;
            }
            if (path == dir)
            {
                return true;
            }
            return path.starts_with(dir)
                   && (dir.back() == '/' || path[dir.size()] == '/');
        }

        /**
         * @brief 输出中首条形似诊断的行（失败文本）；error 行优先，输出无
         * 可用内容时为 ""。
         */
        [[nodiscard]]
        auto first_line_of(std::string_view text) -> std::string
        {
            std::string best;
            std::size_t at = 0;
            while (at < text.size())
            {
                std::size_t end = text.find('\n', at);
                if (end == std::string_view::npos)
                {
                    end = text.size();
                }
                std::string_view line = text.substr(at, end - at);
                at = end + 1;
                while (!line.empty()
                       && (line.front() == ' ' || line.front() == '\r'))
                {
                    line.remove_prefix(1);
                }
                if (line.empty())
                {
                    continue;
                }
                if (best.empty())
                {
                    best = std::string{line.substr(0, 200)};
                }
                if (line.find("error:") != std::string_view::npos)
                {
                    best = std::string{line.substr(0, 200)};
                    break;
                }
            }
            return best;
        }

        /**
         * @brief 解析 libc++.modules.json / libstdc++.modules.json 文本。
         *
         * 模块条目取 "logical-name"（或 "name"）与 "source-path"（或
         * "source"），相对 base_dir 解析；可选的
         * local-arguments.system-include-directories 收集为布线。缺席或畸形
         * 时路径为空。
         */
        struct ModulesJson
        {
            std::string              std_path;
            std::string              compat_path;
            std::vector<std::string> wiring;
        };
        [[nodiscard]]
        auto parse_modules_json(
            const std::string& text,
            const std::string& base_dir) -> ModulesJson
        {
            namespace fs = std::filesystem;
            namespace json = cg::util::json;
            ModulesJson out;
            try
            {
                const json::Document doc = json::Document::parse(text);
                const auto           root = doc.root();
                if (!root.is_object())
                {
                    return out;
                }
                const auto modules = root.get("modules");
                if (!modules || !modules->is_array())
                {
                    return out;
                }
                const auto resolve = [&](std::string_view rel)
                {
                    const fs::path p{std::string{rel}};
                    const fs::path abs =
                        p.is_absolute() ? p : fs::path{base_dir} / p;
                    return cg::util::norm_path(
                        abs.lexically_normal().generic_string());
                };
                for (std::size_t i = 0; i < modules->size(); ++i)
                {
                    const auto entry = modules->at(i);
                    if (!entry || !entry->is_object())
                    {
                        continue;
                    }
                    std::string name;
                    if (const auto v = entry->get("logical-name");
                        v && v->is_string())
                    {
                        name = std::string{*v->as_string()};
                    }
                    else if (
                        const auto n = entry->get("name"); n && n->is_string())
                    {
                        name = std::string{*n->as_string()};
                    }
                    std::string source;
                    if (const auto v = entry->get("source-path");
                        v && v->is_string())
                    {
                        source = std::string{*v->as_string()};
                    }
                    else if (
                        const auto s = entry->get("source");
                        s && s->is_string())
                    {
                        source = std::string{*s->as_string()};
                    }
                    if (name.empty() || source.empty())
                    {
                        continue;
                    }
                    if (const auto args = entry->get("local-arguments");
                        args && args->is_object())
                    {
                        const auto dirs =
                            args->get("system-include-directories");
                        if (dirs && dirs->is_array())
                        {
                            for (std::size_t d = 0; d < dirs->size(); ++d)
                            {
                                if (const auto dv = dirs->at(d);
                                    dv && dv->is_string())
                                {
                                    const std::string dir =
                                        resolve(*dv->as_string());
                                    cg::util::push_unique(out.wiring, dir);
                                }
                            }
                        }
                    }
                    if (name == "std")
                    {
                        out.std_path = resolve(source);
                    }
                    else if (name == "std.compat")
                    {
                        out.compat_path = resolve(source);
                    }
                }
            }
            catch (const std::exception&)
            {
                return ModulesJson{};
            }
            return out;
        }

        /**
         * @brief 解析 P1689 make 格式的扫描输出。
         *
         * "rules"（修订 0）或 "translation-units"（修订 1 形状）各携带
         * provides/requires。畸形文本产出空向量（调用方逐文件降级——绝不把
         * 语法错误信号当作 rc 采信）。
         */
        [[nodiscard]]
        auto parse_p1689(const std::string& text) -> std::vector<ScanRule>
        {
            namespace json = cg::util::json;
            std::vector<ScanRule> out;
            try
            {
                const json::Document doc = json::Document::parse(text);
                const auto           root = doc.root();
                if (!root.is_object())
                {
                    return out;
                }
                const auto read_unit = [&](const json::ValueView& unit)
                {
                    ScanRule rule;
                    if (const auto provides = unit.get("provides");
                        provides && provides->is_array())
                    {
                        for (std::size_t i = 0; i < provides->size(); ++i)
                        {
                            const auto p = provides->at(i);
                            if (!p || !p->is_object())
                            {
                                continue;
                            }
                            ScanProvide provide;
                            if (const auto v = p->get("logical-name");
                                v && v->is_string())
                            {
                                provide.name = std::string{*v->as_string()};
                            }
                            if (const auto v = p->get("source-path");
                                v && v->is_string())
                            {
                                provide.source =
                                    cg::util::norm_path(*v->as_string());
                            }
                            if (const auto v = p->get("is-interface");
                                v && v->is_bool())
                            {
                                provide.is_interface = *v->as_bool();
                            }
                            if (!provide.name.empty())
                            {
                                rule.provides.push_back(std::move(provide));
                            }
                        }
                    }
                    if (const auto requires_list = unit.get("requires");
                        requires_list && requires_list->is_array())
                    {
                        for (std::size_t i = 0; i < requires_list->size(); ++i)
                        {
                            const auto rq = requires_list->at(i);
                            if (!rq || !rq->is_object())
                            {
                                continue;
                            }
                            ScanRequire require;
                            if (const auto v = rq->get("logical-name");
                                v && v->is_string())
                            {
                                require.name = std::string{*v->as_string()};
                            }
                            if (const auto v = rq->get("source-path");
                                v && v->is_string())
                            {
                                require.source =
                                    cg::util::norm_path(*v->as_string());
                            }
                            if (!require.name.empty())
                            {
                                rule.requires_names.push_back(
                                    std::move(require));
                            }
                        }
                    }
                    out.push_back(std::move(rule));
                };
                if (const auto rules = root.get("rules");
                    rules && rules->is_array())
                {
                    for (std::size_t i = 0; i < rules->size(); ++i)
                    {
                        if (const auto r = rules->at(i); r && r->is_object())
                        {
                            read_unit(*r);
                        }
                    }
                }
                else if (
                    const auto tus = root.get("translation-units");
                    tus && tus->is_array())
                {
                    for (std::size_t i = 0; i < tus->size(); ++i)
                    {
                        if (const auto t = tus->at(i); t && t->is_object())
                        {
                            read_unit(*t);
                        }
                    }
                }
            }
            catch (const std::exception&)
            {
                return std::vector<ScanRule>{};
            }
            return out;
        }

        [[nodiscard]]
        auto manifest_to_json(const BmiManifest& m) -> std::string
        {
            namespace json = cg::util::json;
            json::Object root;
            root.set("engine", m.engine);
            root.set("engine_key", m.engine_key);
            root.set("config_key", m.config_key);
            root.set("toolset", m.toolset_id);
            json::Array entries;
            for (const BmiEntry& e : m.entries)
            {
                json::Object o;
                o.set("name", e.name);
                o.set("source", e.source);
                o.set("hash", e.hash);
                o.set("mtime", e.mtime);
                o.set("size", e.size);
                o.set("command", e.command);
                json::Array reqs;
                for (const std::string& r : e.requires_names)
                {
                    reqs.add(r);
                }
                o.set("requires", std::move(reqs));
                o.set("pcm", e.pcm);
                o.set("pcm_size", e.pcm_size);
                o.set("std", e.std_module);
                o.set("config", e.config);
                entries.add(std::move(o));
            }
            root.set("entries", std::move(entries));
            return json::stringify(std::move(root));
        }

        [[nodiscard]]
        auto manifest_from_json(const std::string& text)
            -> std::optional<BmiManifest>
        {
            namespace json = cg::util::json;
            BmiManifest out;
            try
            {
                const json::Document doc = json::Document::parse(text);
                const auto           root = doc.root();
                if (!root.is_object())
                {
                    return std::nullopt;
                }
                const auto str = [&root](std::string_view key)
                {
                    const auto v = root.get(key);
                    return v && v->is_string() ? std::string{*v->as_string()}
                                               : std::string{};
                };
                out.engine = str("engine");
                out.engine_key = str("engine_key");
                out.config_key = str("config_key");
                out.toolset_id = str("toolset");
                const auto entries = root.get("entries");
                if (!entries || !entries->is_array())
                {
                    return std::nullopt;
                }
                for (std::size_t i = 0; i < entries->size(); ++i)
                {
                    const auto e = entries->at(i);
                    if (!e || !e->is_object())
                    {
                        continue;
                    }
                    BmiEntry   entry;
                    const auto estr = [&e](std::string_view key)
                    {
                        const auto v = e->get(key);
                        return v && v->is_string()
                                   ? std::string{*v->as_string()}
                                   : std::string{};
                    };
                    entry.name = estr("name");
                    entry.source = estr("source");
                    entry.hash = estr("hash");
                    entry.command = estr("command");
                    entry.pcm = estr("pcm");
                    if (const auto v = e->get("mtime"); v && v->is_number())
                    {
                        entry.mtime = *v->as_int();
                    }
                    if (const auto v = e->get("size"); v && v->is_number())
                    {
                        entry.size = *v->as_int();
                    }
                    if (const auto v = e->get("pcm_size"); v && v->is_number())
                    {
                        entry.pcm_size = *v->as_int();
                    }
                    if (const auto v = e->get("std"); v && v->is_bool())
                    {
                        entry.std_module = *v->as_bool();
                    }
                    entry.config = estr("config");
                    if (const auto reqs = e->get("requires");
                        reqs && reqs->is_array())
                    {
                        for (std::size_t j = 0; j < reqs->size(); ++j)
                        {
                            if (const auto rv = reqs->at(j);
                                rv && rv->is_string())
                            {
                                entry.requires_names.emplace_back(
                                    *rv->as_string());
                            }
                        }
                    }
                    if (!entry.name.empty() && !entry.pcm.empty())
                    {
                        out.entries.push_back(std::move(entry));
                    }
                }
            }
            catch (const std::exception&)
            {
                return std::nullopt;
            }
            return out;
        }

        /**
         * @brief 以窄字符串参数运行工具（驱动器分区的进程运行器是宽字符串
         * 引擎，此处每个探针都经它）。
         */
        [[nodiscard]]
        auto run_tool(
            const std::string&              exe,
            const std::vector<std::string>& args,
            const std::string&              cwd = std::string{}) -> ProcResult
        {
            std::vector<std::wstring> wide;
            wide.reserve(args.size());
            for (const std::string& a : args)
            {
                wide.push_back(to_wide(a));
            }
            // cwd 由 CreateProcess 的 lpCurrentDirectory 逐子进程传递；调用方
            // 绝不能以 chdir 模拟（线程池共享进程 cwd）。
            return run_process(
                to_wide(exe),
                wide,
                cwd.empty() ? std::wstring{} : to_wide(cwd));
        }

        /**
         * @brief -print-file-name 解析：驱动器打印它会用的文件，不知道时把
         * 查询名原样回显（clang 经此查 libstdc++.modules.json 的已知 MSYS2
         * 行为）。
         *
         * @internal 只有存在的绝对路径才算数。
         */
        [[nodiscard]]
        auto print_file_name(const std::string& driver, const std::string& name)
            -> std::string
        {
            namespace fs = std::filesystem;
            const ProcResult r = run_tool(driver, {"-print-file-name=" + name});
            if (!r.started || r.exit_code != 0)
            {
                return {};
            }
            std::string out = std::string{
                std::string_view{r.out}.substr(0, r.out.find('\n'))};
            while (!out.empty() && (out.back() == '\r' || out.back() == ' '))
            {
                out.pop_back();
            }
            std::error_code ec;
            if (out.empty() || out == name
                || !fs::is_regular_file(fs::path{out}, ec))
            {
                return {};
            }
            return cg::util::norm_path(out);
        }

        /**
         * @brief 在 PATH 文本中扫 g++.exe（libstdc++ json 通道：clang 自己的
         * -print-file-name 在 MSYS2 上失败，g++ 为其安装作答）。
         */
        [[nodiscard]]
        auto gxx_on_path_from(const std::string& path_env) -> std::string
        {
            namespace fs = std::filesystem;
            if (path_env.empty())
            {
                return {};
            }
            std::error_code   ec;
            std::string       entry;
            std::stringstream env{path_env};
            while (std::getline(env, entry, ';'))
            {
                if (entry.empty())
                {
                    continue;
                }
                const fs::path candidate = fs::path{entry} / "g++.exe";
                if (fs::is_regular_file(candidate, ec))
                {
                    return candidate.lexically_normal().generic_string();
                }
            }
            return {};
        }

        /**
         * @brief 驱动器报告的 include 列表中首个
         * "VC/Tools/MSVC/`<版本>`/include" 根（自动探测 MSVC 工具链的 clang
         * 会携带它）——MSVC 地 std 源的发现 通道。
         *
         * @internal vswhere 看不见相关组件元数据；工具集路径才是事实。
         *
         * @return (工具集目录, 版本)。
         */
        [[nodiscard]]
        auto msvc_toolset_from_report(
            const std::vector<std::string>& cxx_include_dirs)
            -> std::pair<std::string, std::string>
        {
            constexpr std::string_view marker = "/VC/Tools/MSVC/";
            for (const std::string& dir : cxx_include_dirs)
            {
                const std::size_t at = dir.find(marker);
                if (at == std::string::npos || dir.size() < 8 + marker.size()
                    || dir.substr(dir.size() - 8) != "/include")
                {
                    continue;
                }
                const std::string toolset = dir.substr(0, dir.size() - 8);
                std::string       version = toolset.substr(at + marker.size());
                while (!version.empty() && version.back() == '/')
                {
                    version.pop_back();
                }
                if (!version.empty())
                {
                    return {toolset, version};
                }
            }
            return {};
        }

        /**
         * @brief 工具集下限（设计定案）：14.44 是自建支持的最老 MSVC STL
         * 世代；更老的工具集以零兼容投入降级。按 major.minor 数值比较。
         */
        [[nodiscard]]
        auto toolset_at_floor(const std::string& version) -> bool
        {
            const std::size_t dot = version.find('.');
            if (dot == std::string::npos)
            {
                return false;
            }
            const std::size_t dot2 = version.find('.', dot + 1);
            if (dot2 == std::string::npos)
            {
                return false;
            }
            const long major =
                std::strtol(version.substr(0, dot).c_str(), nullptr, 10);
            const long minor = std::strtol(
                version.substr(dot + 1, dot2 - dot - 1).c_str(),
                nullptr,
                10);
            return major > 14 || (major == 14 && minor >= 44);
        }

        /**
         * @brief 报告 include 列表中的 Windows SDK 身份（缓存键轴；列表无
         * Windows Kits 根时为 ""）。
         */
        [[nodiscard]]
        auto windows_sdk_from_report(
            const std::vector<std::string>& cxx_include_dirs) -> std::string
        {
            constexpr std::string_view kits = "/Windows Kits/";
            constexpr std::string_view include = "/Include/";
            for (const std::string& dir : cxx_include_dirs)
            {
                const std::size_t at = dir.find(kits);
                if (at == std::string::npos)
                {
                    continue;
                }
                const std::size_t inc = dir.find(include, at);
                if (inc == std::string::npos)
                {
                    continue;
                }
                std::string       sdk = dir.substr(inc + include.size());
                const std::size_t slash = sdk.find('/');
                if (slash != std::string::npos)
                {
                    sdk = sdk.substr(0, slash);
                }
                return sdk;
            }
            return {};
        }

        /**
         * @brief 从 MSVC 工具集目录组装 msvc-stl 的 StdModuleSource。
         *
         * @internal modules/std.ixx 不存在时返回 nullopt——失败文案由各
         * 调用方按自己的路由给出。sdk 身份与 wiring 均由调用方传入：发现
         * 层的 sdk 证据来自驱动器报告、wiring 留空待消费层按方言补，cl
         * 路由两者都用合并后的接线目录。toolset_id 与 14.44 地板文案在此
         * 是两条路由共享的唯一事实源（toolset_id 折进配置键，两处漂移会
         * 让同一工具链在缓存里分裂成两个身份）。
         */
        [[nodiscard]]
        auto msvc_stl_source(
            const std::string&              toolset,
            const std::string&              version,
            const std::vector<std::string>& sdk_dirs,
            std::vector<std::string> wiring) -> std::optional<StdModuleSource>
        {
            namespace fs = std::filesystem;
            const std::string std_ixx = cg::util::norm_path(
                (fs::path{toolset} / "modules" / "std.ixx").generic_string());
            std::error_code ec;
            if (!fs::is_regular_file(fs::path{std_ixx}, ec))
            {
                return std::nullopt;
            }
            StdModuleSource src;
            src.stdlib_impl = "msvc-stl";
            src.std_path = std_ixx;
            const std::string compat_ixx = cg::util::norm_path(
                (fs::path{toolset} / "modules" / "std.compat.ixx")
                    .generic_string());
            if (fs::is_regular_file(fs::path{compat_ixx}, ec))
            {
                src.compat_path = compat_ixx;
            }
            src.wiring = std::move(wiring);
            src.toolset_id = std::format(
                "msvc:{}:sdk:{}",
                version,
                windows_sdk_from_report(sdk_dirs));
            if (!toolset_at_floor(version))
            {
                src.note = std::format(
                    "MSVC toolset {} is below the 14.44 floor - the "
                    "std.ixx route is known-bad there (degrade tier)",
                    version);
            }
            return src;
        }

        /**
         * @brief 驱动器旁的 clang-scan-deps（与解析引擎同一安装），再 PATH。
         */
        [[nodiscard]]
        auto find_scan_deps(const std::string& driver) -> std::string
        {
            namespace fs = std::filesystem;
            std::error_code ec;
            const fs::path  beside =
                fs::path{driver}.parent_path() / "clang-scan-deps.exe";
            if (fs::is_regular_file(beside, ec))
            {
                return beside.lexically_normal().generic_string();
            }
            if (const auto env = cg::util::cstd::get_env("PATH"))
            {
                std::stringstream env_stream{*env};
                std::string       entry;
                while (std::getline(env_stream, entry, ';'))
                {
                    if (entry.empty())
                    {
                        continue;
                    }
                    const fs::path candidate =
                        fs::path{entry} / "clang-scan-deps.exe";
                    if (fs::is_regular_file(candidate, ec))
                    {
                        return candidate.lexically_normal().generic_string();
                    }
                }
            }
            return {};
        }

        /**
         * @brief 清单快路径的 stat() 身份（与索引器新鲜度扫描同形状）。
         */
        struct BmiStat
        {
            bool         ok = false;
            std::int64_t mtime = 0;
            std::int64_t size = 0;
        };
        [[nodiscard]]
        auto stat_source(const std::string& path) -> BmiStat
        {
            const cg::util::platform::FileMeta meta{
                cg::util::platform::file_meta(std::filesystem::path{path})};
            if (!meta.ok)
            {
                return {};
            }
            return {
                .ok = true,
                .mtime = meta.mtime_ticks,
                .size = meta.size_bytes};
        }

        [[nodiscard]]
        auto hash_source(const std::string& path) -> std::string
        {
            try
            {
                return cg::util::sha256_file_hex(path);
            }
            catch (const std::exception&)
            {
                return {};
            }
        }

        /**
         * @brief 源相对先前清单条目的新鲜度：先 stat 快路径，内容哈希作确认
         * 性决胜（pcm 字节与 mtime 永不是有效性；记录的源哈希才是）。
         *
         * @return 当前 stat 加有效哈希（快路径命中时用记录值免重读）；读不
         * 到时哈希为 ""。
         */
        struct SourceCheck
        {
            BmiStat     stat;
            std::string hash;
            bool        matches_prior = false; // 内容与条目一致
        };
        [[nodiscard]]
        auto check_source(const std::string& path, const BmiEntry* prior)
            -> SourceCheck
        {
            SourceCheck out;
            out.stat = stat_source(path);
            if (!out.stat.ok)
            {
                return out;
            }
            const bool stat_hit =
                prior != nullptr && prior->mtime == out.stat.mtime
                && prior->size == out.stat.size && !prior->hash.empty();
            if (stat_hit)
            {
                out.hash = prior->hash;
                out.matches_prior = true;
                return out;
            }
            out.hash = hash_source(path);
            out.matches_prior = prior != nullptr && !out.hash.empty()
                                && prior->hash == out.hash;
            return out;
        }

        /** @brief 清单条目的身份键：模块名 + 规范生产命令摘要。 */
        [[nodiscard]]
        inline auto bmi_entry_key(
            std::string_view name,
            std::string_view command) -> std::string
        {
            return std::format("{}\n{}", name, command);
        }

        /**
         * @brief 把 disk 中 target 尚不知道的条目并入 target（并集，不覆盖）。
         *
         * @param target 并集的目标清单（只增不覆盖的一方）。
         * @param disk 盘上读回的清单（并集的另一方）。
         * @param skip_keys 显式丢弃的条目键（如本趟失效/清扫的条目），绝不
         *        因并发写者仍在盘上而复活。
         * @return 实际并入的条目数。
         *
         * @internal 无锁清单的“不丢其它配置条目”核心：写者在原子替换前
         * 重读盘上清单并做并集，2/3 并发写者不会互相抹掉对方的配置条目。
         */
        [[nodiscard]]
        inline auto merge_manifest_entries(
            BmiManifest&                           target,
            const BmiManifest&                     disk,
            const std::unordered_set<std::string>& skip_keys = {})
            -> std::size_t
        {
            std::unordered_set<std::string> keys;
            keys.reserve(target.entries.size());
            for (const BmiEntry& e : target.entries)
            {
                keys.insert(bmi_entry_key(e.name, e.command));
            }
            std::size_t merged = 0;
            for (const BmiEntry& e : disk.entries)
            {
                const std::string key = bmi_entry_key(e.name, e.command);
                if (skip_keys.contains(key) || !keys.insert(key).second)
                {
                    continue;
                }
                target.entries.push_back(e);
                ++merged;
            }
            return merged;
        }

        /**
         * @brief pcm 头部可加载性校验：clang AST/PCM 魔数 "CPCH"。
         *
         * @internal 尺寸校验挡住 0 字节与半截产物，但挡不住“尺寸恰好相同、
         * 头部已被覆盖”的损坏（磁盘坏道、被外部工具改写）。这种 pcm 会被
         * 判定可复用并在下游以 bmi-config-mismatch 逐 TU 失败，而清单条目
         * 在批次结束前仍然合法——首轮必然降级。复用/捐赠判定加读 4 字节
         * 魔数，使损坏在解析前就等同缺失，自建在同一轮内重建，无需第二次
         * 运行才收敛。
         */
        [[nodiscard]]
        auto pcm_header_ok(const std::string& pcm) -> bool
        {
            std::ifstream in{std::filesystem::path{pcm}, std::ios::binary};
            if (!in)
            {
                return false;
            }
            char magic[4]{};
            in.read(magic, sizeof(magic));
            return in.gcount() == static_cast<std::streamsize>(sizeof(magic))
                   && std::string_view{magic, sizeof(magic)} == "CPCH";
        }

        /**
         * @brief 复用前的 pcm 盘上校验。
         *
         * @return 实际字节数（>0）当文件是常规文件、非空、头部魔数可加载、
         *         且与清单记录的 pcm_size 完全一致；否则 -1（不可复用/保守
         *         重建）。
         *
         * @internal 旧清单没有 pcm_size（0）时一律保守重建——不迁移、不猜
         * 尺寸。这是 0 字节/半截/头部损坏 pcm 不再被永久复用的关键门。
         */
        [[nodiscard]]
        auto cached_pcm_size(const std::string& pcm, std::int64_t recorded)
            -> std::int64_t
        {
            namespace fs = std::filesystem;
            if (recorded <= 0)
            {
                return -1;
            }
            const cg::util::platform::FileMeta meta{
                cg::util::platform::file_meta(fs::path{pcm})};
            if (!meta.ok || meta.size_bytes == 0)
            {
                return -1;
            }
            const auto value = meta.size_bytes;
            if (value != recorded || !pcm_header_ok(pcm))
            {
                return -1;
            }
            return value;
        }

        /** @brief 产物字节数；缺失/空/读不到均为 0（长路径感知）。 */
        [[nodiscard]]
        auto artifact_size(const std::string& path) -> std::int64_t
        {
            const cg::util::platform::FileMeta meta{
                cg::util::platform::file_meta(std::filesystem::path{path})};
            return meta.ok ? meta.size_bytes : 0;
        }

        /**
         * @brief donor 复制 + 目标尺寸复核。
         *
         * @internal donor 记录尺寸必须 >0（旧清单/空文件一律不捐）。复制后
         * stat 目标并与 donor 值比对，且头部魔数必须可加载；任一不符即删除
         * 目标并返回 false，调用方落回本地重建——绝不留下半截、错尺寸或
         * 头部损坏的缓存。
         */
        [[nodiscard]]
        auto copy_donor_verified(
            const std::string& donor,
            const std::string& target,
            std::int64_t       donor_size) -> bool
        {
            namespace fs = std::filesystem;
            if (donor_size <= 0)
            {
                return false;
            }
            std::error_code dir_ec;
            fs::create_directories(fs::path{target}.parent_path(), dir_ec);
            // 先复制到唯一 tmp，复核尺寸后才原子替换目标：并发读者绝不
            // 看到半截 donor 副本；不符时删 tmp，旧目标原样保留。
            const fs::path tmp =
                cg::util::platform::unique_temp_path(fs::path{target});
            std::error_code copy_ec;
            fs::copy_file(
                fs::path{donor},
                tmp,
                fs::copy_options::overwrite_existing,
                copy_ec);
            if (copy_ec)
            {
                cg::util::platform::remove_file(tmp);
                return false;
            }
            if (artifact_size(tmp.generic_string()) != donor_size
                || !pcm_header_ok(tmp.generic_string()))
            {
                cg::util::platform::remove_file(tmp);
                return false;
            }
            if (!cg::util::platform::atomic_replace(tmp, fs::path{target}))
            {
                cg::util::platform::remove_file(tmp);
                return false;
            }
            return true;
        }

        /**
         * @brief 记录 cl 目录与驱动器报告都不指名工具集时，从文件系统发现
         * MSVC 工具集。
         *
         * @internal
         * msbuild 适配器刻意剥掉工具集根（clang-cl 自行配置它们），而 msys2
         * 引擎的 cl 模式在干净机器上什么也自动探测不到；vswhere 看不见相关
         * 组件元数据，工具集路径才是事实。纯 std::filesystem：非 Windows 树
         * 上这些根不存在，链以点名类降级而不碰平台 API。VS 根从环境推导
         * （无机器绝对字面量；无这些变量的树上扫描无产出、cl 链以点名类
         * 降级）。版本排序取新优先：逐点数值段比较（14.51 > 14.44）。
         *
         * @return 新者优先、携带 modules/std.ixx 的工具集目录。
         */
        [[nodiscard]]
        auto discover_msvc_toolsets() -> std::vector<std::string>
        {
            namespace fs = std::filesystem;
            std::error_code ec;
            const auto      env_root = [](const char* name)
            {
                const auto value = cg::util::cstd::get_env(name);
                if (!value.has_value())
                {
                    return std::string{};
                }
                return std::format(
                    "{}/Microsoft Visual Studio",
                    cg::util::to_forward_slashes(*value));
            };
            std::vector<std::string> roots;
            for (const std::string& root :
                 {env_root("ProgramFiles"), env_root("ProgramFiles(x86)")})
            {
                if (!root.empty())
                {
                    roots.push_back(root);
                }
            }
            std::vector<std::string> versions; // 工具集版本串
            std::unordered_map<std::string, std::string> dir_of;
            for (const std::string& vs_root : roots)
            {
                for (const fs::directory_entry& edition :
                     fs::directory_iterator(vs_root, ec))
                {
                    std::error_code skuc;
                    for (const fs::directory_entry& sku :
                         fs::directory_iterator(edition.path(), skuc))
                    {
                        std::error_code vec;
                        for (const fs::directory_entry& toolset :
                             fs::directory_iterator(
                                 sku.path() / "VC" / "Tools" / "MSVC",
                                 vec))
                        {
                            std::error_code isc;
                            if (!toolset.is_directory(isc))
                            {
                                continue;
                            }
                            const std::string version =
                                toolset.path().filename().generic_string();
                            if (!fs::is_regular_file(
                                    toolset.path() / "modules" / "std.ixx",
                                    isc))
                            {
                                continue;
                            }
                            const std::string dir =
                                cg::util::norm_path(toolset.path()
                                                        .lexically_normal()
                                                        .generic_string());
                            if (!dir_of.contains(version))
                            {
                                dir_of.emplace(version, dir);
                                versions.push_back(version);
                            }
                        }
                    }
                }
            }
            std::ranges::sort(
                versions,
                [](const std::string& a, const std::string& b)
                {
                    std::size_t ia = 0;
                    std::size_t ib = 0;
                    while (ia < a.size() && ib < b.size())
                    {
                        std::size_t ja = a.find('.', ia);
                        std::size_t jb = b.find('.', ib);
                        if (ja == std::string::npos)
                        {
                            ja = a.size();
                        }
                        if (jb == std::string::npos)
                        {
                            jb = b.size();
                        }
                        const long na = std::strtol(
                            a.substr(ia, ja - ia).c_str(),
                            nullptr,
                            10);
                        const long nb = std::strtol(
                            b.substr(ib, jb - ib).c_str(),
                            nullptr,
                            10);
                        if (na != nb)
                        {
                            return na > nb;
                        }
                        ia = ja == a.size() ? ja : ja + 1;
                        ib = jb == b.size() ? jb : jb + 1;
                    }
                    return a.size() > b.size();
                });
            std::vector<std::string> out;
            out.reserve(versions.size());
            for (const std::string& v : versions)
            {
                out.push_back(dir_of.at(v));
            }
            return out;
        }

        /**
         * @brief /imsvc 布线用的 Windows SDK include 根（最新 SDK）：工具集
         * 编译所对的 ucrt/shared/um/winrt/cppwinrt 五件套。
         */
        [[nodiscard]]
        auto discover_windows_sdk_dirs() -> std::vector<std::string>
        {
            namespace fs = std::filesystem;
            std::error_code ec;
            const auto kits_env = cg::util::cstd::get_env("ProgramFiles(x86)");
            const std::string kits =
                kits_env.has_value() ? cg::util::to_forward_slashes(*kits_env)
                                     : std::string{};
            if (kits.empty())
            {
                return {};
            }
            const fs::path base =
                fs::path{kits} / "Windows Kits" / "10" / "Include";
            std::string newest;
            for (const fs::directory_entry& sdk :
                 fs::directory_iterator(base, ec))
            {
                std::error_code isd;
                if (!sdk.is_directory(isd))
                {
                    continue;
                }
                const std::string name = sdk.path().filename().generic_string();
                if (newest.empty() || name > newest)
                { // 点数值排序可用
                    newest = name;
                }
            }
            if (newest.empty())
            {
                return {};
            }
            constexpr std::string_view sub[] =
                {"ucrt", "shared", "um", "winrt", "cppwinrt"};
            std::vector<std::string> out;
            for (const std::string_view s : sub)
            {
                std::error_code isc;
                const fs::path  dir = base / newest / s;
                if (fs::is_directory(dir, isc))
                {
                    out.push_back(
                        cg::util::norm_path(
                            dir.lexically_normal().generic_string()));
                }
            }
            return out;
        }

        /**
         * @brief TU 文本书写的头单元 import 声明（名去引号、文件序）。
         *
         * @internal P1689 扫描不在规则的 requires 中报告头单元（实测：
         * `import "test.h";` 产出仅有 provides 的规则），故降级为 include 的
         * 检测经与 import_rewrite_overlay 同源的词法扫描器读源（含多行
         * import）。缓冲必须具名、不是调用点临时对象：扫描器返回刺入此文本
         * 的 string_view，按值传入的临时对象在范围初始化完整表达式末尾死
         * 亡——在循环体拷贝名字之前就被释放（MSVC 调试堆把已释放内存擦成
         * 0xdd，把 clang 上发布分配器的运气暴露出来）。
         */
        [[nodiscard]]
        auto text_header_unit_imports(const std::string& file)
            -> std::vector<std::string>
        {
            const auto bytes = cg::util::read_text_file(file);
            if (!bytes.has_value())
            {
                return {};
            }
            // 缓冲必须具名（注释见上）：扫描器返回刺入此文本的 string_view。
            const std::string        text = std::move(*bytes);
            std::vector<std::string> out;
            for (const ImportDeclSite& site : scan_import_decls(text))
            {
                if (site.is_header)
                {
                    out.emplace_back(site.name);
                }
            }
            return out;
        }

        /**
         * @brief gnu 模式 std 值的 cl 方言 /std: 拼写。
         *
         * @internal c++20 精确映射；严格更新者映射 /std:c++latest（= clang 23
         * cc1 的 c++2d；c++latest 链干净构建自己的 std.pcm）。/std:c++23 在
         * cl 模式不存在——它只被诊断成良性的未用参数告警，TU 却静默落到
         * c++14 默认、模块语法死掉（"unknown type name 'import'"）——故绝不
         * 发出它。
         *
         * @return (cl 拼写, gnu 孪生)。
         */
        [[nodiscard]]
        auto cl_std_of(const std::string& std_value)
            -> std::pair<std::string, std::string>
        {
            if (std_value == "c++20" || std_value == "c++2a")
            {
                return {"/std:c++20", "c++20"};
            }
            return {"/std:c++latest", "c++2d"};
        }

        /**
         * @brief include 目录表的 MSVC 工具链切片（工具集 + atlmfc + VS 辅助
         * + Windows SDK 根，保序去重）——std.ixx 生产与 cl 模式解析经
         * -imsvc 钉住的根。
         */
        [[nodiscard]]
        auto msvc_include_slice(const std::vector<std::string>& dirs)
            -> std::vector<std::string>
        {
            std::vector<std::string> out;
            for (const std::string& dir : dirs)
            {
                const std::string lower = cg::util::ascii_lower(dir);
                const bool        msvc_root =
                    lower.find("/vc/tools/msvc/") != std::string::npos
                    || lower.find("/vc/auxiliary/vs/") != std::string::npos
                    || lower.find("/windows kits/") != std::string::npos;
                if (msvc_root)
                {
                    cg::util::push_unique(out, dir);
                }
            }
            return out;
        }

        /**
         * @brief include 目录表的 MSVC STL 本体（工具集 include + atlmfc + VS
         * 辅助根；刻意丢掉 SDK 根）——"绝不投喂 std"规则的触发根。
         *
         * @internal 非 STL 头与 `import std` 混用无碍，故 SDK 根下解析的
         * `<windows.h>` 或 ucrt `<stdio.h>` 绝不能把 TU 降级到伞头层。
         */
        [[nodiscard]]
        auto stl_include_slice(const std::vector<std::string>& dirs)
            -> std::vector<std::string>
        {
            std::vector<std::string> out;
            for (const std::string& dir : dirs)
            {
                const std::string lower = cg::util::ascii_lower(dir);
                const bool        stl_root =
                    lower.find("/vc/tools/msvc/") != std::string::npos
                    || lower.find("/vc/auxiliary/vs/") != std::string::npos;
                if (stl_root)
                {
                    cg::util::push_unique(out, dir);
                }
            }
            return out;
        }

        /**
         * @brief P1689 require 名是否头单元：头单元逻辑名就是头路径拼写。
         */
        [[nodiscard]]
        auto looks_like_header_unit(const std::string& name) -> bool
        {
            if (name.size() >= 2
                && (name.front() == '"' || name.front() == '<'))
            {
                return true;
            }
            constexpr std::string_view ext[] =
                {".h", ".hh", ".hpp", ".hxx", ".inl"};
            for (const std::string_view e : ext)
            {
                if (name.ends_with(e))
                {
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief TU 文本是否携带解析进 STL 本体根之一的 `#include`——混合 TU
         * 探测器。
         *
         * @internal 工具集 include/atlmfc 才算，SDK 不算（`<windows.h>` 一类
         * 非 STL 头与 import std 混用无碍）。这样的 TU 在 MSVC 地绝不可被投
         * 喂 std.pcm：pcm 导出的 std:: 模板与文本包含的 STL 定义冲突。
         */
        [[nodiscard]]
        auto text_has_stl_include(
            const std::string&              file,
            const std::vector<std::string>& roots) -> bool
        {
            namespace fs = std::filesystem;
            if (roots.empty())
            {
                return false;
            }
            std::ifstream in{fs::path{file}, std::ios::binary};
            if (!in)
            {
                return false;
            }
            std::string     line;
            std::error_code ec;
            while (std::getline(in, line))
            {
                const std::size_t at = line.find("#include");
                if (at == std::string::npos)
                {
                    continue;
                }
                std::size_t k = at + 8;
                while (k < line.size() && (line[k] == ' ' || line[k] == '\t'))
                {
                    ++k;
                }
                if (k >= line.size() || (line[k] != '<' && line[k] != '"'))
                {
                    continue;
                }
                const char        close = line[k] == '<' ? '>' : '"';
                const std::size_t shut = line.find(close, k + 1);
                if (shut == std::string::npos)
                {
                    continue;
                }
                const std::string name = line.substr(k + 1, shut - k - 1);
                if (name.empty() || name.contains('/'))
                {
                    continue; // 相对路径是项目头，不是 STL
                }
                for (const std::string& root : roots)
                {
                    if (fs::is_regular_file(
                            fs::path{root} / fs::path{name},
                            ec))
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        /**
         * @brief 记录 argv 的 MSVC 模块开关剥离结果（剥离层）。
         *
         * @internal argv 是剥除后的向量（没有任何开关被剥时与输入逐字节相
         * 同）；references 是 /reference 声明的模块名单——构建系统对该 TU
         * 消费哪些模块的声明，喂给注入时与扫描器结果取并集；stripped 标记
         * 是否剥除过任何 token（gnu 方言路线恒 false，重放零变化由此保证）。
         */
        struct MsvcModuleStrip
        {
            std::vector<std::string> argv;
            std::vector<std::string> references;
            bool                     stripped = false;
        };

        /**
         * @brief /reference 值（"name=path.ifc" 或裸 "path.ifc"）的模块名。
         *
         * 无名字部分时以文件主干推断（与 cl 从 .ifc 文件名推模块名同义）；
         * 永不保留 .ifc 路径——clang 读不了 .ifc，喂给一律走影子 pcm。
         */
        [[nodiscard]]
        auto reference_module_name(std::string_view value) -> std::string
        {
            // "name=path" 形态：名字部分原样——模块名可以合法含点（std.compat、
            // mod.part），绝不截主干、绝不剥路径（名字部分本就不含路径分隔）。
            const std::size_t eq = value.find('=');
            if (eq != std::string_view::npos)
            {
                return std::string{value.substr(0, eq)};
            }
            // 裸 path 形态：以文件主干推断（与 cl 从 .ifc
            // 文件名推模块名同义）。
            std::string_view  name = value;
            const std::size_t slash = name.find_last_of("/\\");
            if (slash != std::string_view::npos)
            {
                name = name.substr(slash + 1);
            }
            const std::size_t dot = name.find_last_of('.');
            if (dot != std::string_view::npos && dot > 0)
            {
                name = name.substr(0, dot);
            }
            return std::string{name};
        }

        /**
         * @brief MSVC 模块开关规范拼写清单的本层视图（真实存储在
         * cg::util::msvcswitch 单点清单）。
         *
         * @internal single 是无值单 token（TU 性质标记）；valued 是带值开关
         * （两段式 "X 值" 与冒号连写 "X:value" 都算）。拼写按 MSBuild 规范
         * 记录；消费判定（msvc_module_switch_classify）大小写不敏感、'/' 与
         * '-' 前缀等价。名单全仓只此一份（cg.util.msvcswitch）：翻译层
         * （cg.msbuild.translate 的分值开关投影，cdb 成对保留的写入面）、
         * BMI 剥离层（本文件的 strip_msvc_module_switches）与 PCH 影子重放
         * （clang.pch.ixx 的 replay_argv）都读它，杜绝独立维护漂移——
         * /sourceDependencies 曾只在 PCH 侧被认识、/exportHeader 两边都没有
         * （参数逃逸复盘暴露的结构性缺口，两份清单自此合一，后又上提单点）。
         * 此处保留本层别名只为消费点就近可读，指向单点存储、绝不复制内容。
         */
        inline constexpr std::span<const std::string_view>
            msvc_module_single_switches{cg::util::msvcswitch::single};
        inline constexpr std::span<const std::string_view>
            msvc_module_valued_switches{cg::util::msvcswitch::valued};

        /**
         * @brief 一个 token 的 MSVC 模块开关归类。
         *
         * @internal none=不是模块开关；single=无值单 token；valued=带值开关
         * （值在下一 token 或冒号连写段内）。
         */
        enum class MsvcModuleSwitchKind
        {
            none,
            single,
            valued
        };

        /**
         * @brief msvc_module_switch_classify 的归类结果。
         *
         * @internal kind 为 none 时其余字段无意义；canonical 是命中的规范
         * 拼写（无前缀无值形态，指向共享清单的静态存储），供消费方区分具体
         * 旗标（如 /reference 记账）；joined 为真表示冒号连写形态
         * （"X:value"，值段在 joined_value），此时值已随 token 整体剥除，
         * 消费方绝不能再吞下一 token。
         */
        struct MsvcSwitchMatch
        {
            MsvcModuleSwitchKind kind = MsvcModuleSwitchKind::none;
            std::string_view     canonical;
            std::string_view     joined_value;
            bool                 joined = false;
        };

        /**
         * @brief 判定 token 属于哪种 MSVC 模块开关（共享清单的唯一读法）。
         *
         * @param token 原始 token（可带 '/' 或 '-' 前缀、任意 ASCII 大小写、
         * 冒号连写值段）。
         */
        [[nodiscard]]
        auto msvc_module_switch_classify(std::string_view token)
            -> MsvcSwitchMatch
        {
            MsvcSwitchMatch m;
            if (token.size() < 2
                || (token.front() != '/' && token.front() != '-'))
            {
                return m;
            }
            std::string_view spelling = token.substr(1);
            if (const std::size_t colon = spelling.find(':');
                colon != std::string_view::npos)
            {
                m.joined_value = spelling.substr(colon + 1);
                spelling = spelling.substr(0, colon);
                m.joined = true;
            }
            // 名字 ASCII 大小写不敏感比对（cl 选项本身大小写不敏感；清单按
            // MSBuild 规范拼写，这里放宽是为手写 cdb 的健壮性）。
            const auto ieq = [&](std::string_view flag)
            { return cg::util::ascii_iequals(spelling, flag); };
            if (!m.joined)
            {
                for (const std::string_view f : msvc_module_single_switches)
                {
                    if (ieq(f))
                    {
                        m.kind = MsvcModuleSwitchKind::single;
                        m.canonical = f;
                        return m;
                    }
                }
            }
            for (const std::string_view f : msvc_module_valued_switches)
            {
                if (ieq(f))
                {
                    m.kind = MsvcModuleSwitchKind::valued;
                    m.canonical = f;
                    return m;
                }
            }
            return m;
        }

        /**
         * @brief 从记录 argv 剥除 MSVC 模块开关（只剥、绝不译成 clang 参数）。
         *
         * 单 token：/interface /internalPartition（TU 性质由 P1689 扫描发现，
         * -x c++-module 由各消费点自行追加）。连值（两段式 "X 值" 与冒号连写
         * "X:value" 都兼容）：/ifcOutput /scanDependencies /sourceDependencies
         * /exportHeader /ifcSearchDir /reference——产物位置由 DAG 节点 pcm
         * 命名、喂给按闭包注入、.ifc 目录异格式不可搜索，全部不进入 clang
         * 家工具的 argv。翻译层修复后 cdb 中两段式带值（开关与值成对保留、
         * 剥离层与写入面共享同一份名单）；值缺席的裸拼写仍是防御面（手写
         * 或历史 cdb），本函数对两种形态都成对剥除、绝不剥一半留一半。
         * /sourceDependencies 是 MSBuild 的编译趟依赖直出开关、/exportHeader
         * 是头文件单元产物开关，两者与扫描趟的 /scanDependencies 同族。
         * /reference 的名字记账进 references。拼写归属由
         * msvc_module_switch_classify 对共享清单判定（前缀 '/' 与 '-' 等价、
         * ASCII 大小写不敏感）。
         */
        [[nodiscard]]
        auto strip_msvc_module_switches(const std::vector<std::string>& argv)
            -> MsvcModuleStrip
        {
            MsvcModuleStrip out;
            out.argv.reserve(argv.size());
            for (std::size_t i = 0; i < argv.size(); ++i)
            {
                const std::string&    t = argv[i];
                const MsvcSwitchMatch m = msvc_module_switch_classify(t);
                if (m.kind == MsvcModuleSwitchKind::none)
                {
                    out.argv.push_back(t);
                    continue;
                }
                out.stripped = true;
                if (m.kind == MsvcModuleSwitchKind::valued)
                {
                    std::string value;
                    if (m.joined)
                    {
                        value = std::string{m.joined_value};
                    }
                    else if (cg::clang::detail::cl_value_follows(argv, i))
                    {
                        // 两段式：值 token 一并吞掉。分写值守卫（cl 型单点，
                        // 见 compile_db 的守卫族）：以 '/' 或 '-' 开头的下一个
                        // token 是旗标（cl 世界的路径值带盘符，不以 '/'
                        // 开头）， 裸开关绝不吞它。
                        value = argv[++i];
                    }
                    if (m.canonical == "reference" && !value.empty())
                    {
                        const std::string name = reference_module_name(value);
                        if (!name.empty())
                        {
                            cg::util::push_unique(out.references, name);
                        }
                    }
                }
            }
            return out;
        }

        /**
         * @brief 接口 BMI 重放命令的参数向量组装（不含引擎可执行路径）。
         *
         * @internal 参数向量绝不携带引擎路径：run_tool 的首参（进程可执行
         * 栏）已携带它，run_process 又把它拼成命令行首 token（"exe" + 参
         * 数）——参数向量里再出现一份引擎路径，clang 就会见到双份 .exe，
         * 第二份是真实存在的文件、被当作额外输入；MSBuild 记录的 .ixx 命
         * 令必带 /TP（把一切输入当 C++ 源码），双输入与追加的 -clang:-o 相
         * 遇即报 cannot specify -o when generating multiple output files
         * （参数逃逸真凶）。recorded_stripped 非空时以剥离后的记录 argv 为权
         * 威（其中无 -std=/-std: 时按方言补一份），否则按方言合成基线；
         * dep_feeds 的每个依赖以 -fmodule-file=名=pcm 追加。源文件与输出参
         * 数由调用方追加（canonical 哈希因此覆盖"到依赖喂给为止"的前缀）。
         *
         * @param recorded_stripped 剥离后的记录 argv（无记录权威时传空指针）。
         * @param dialect_cl cl 方言为真（clang-cl 重放）。
         * @param cl_std cl 方言的 std 拼写（如 /std:c++20）。
         * @param std_value gnu 方言的 std 值（如 c++20）。
         * @param langopt_axes gnu 方言的语言轴旗标。
         * @param cl_base cl 方言合成基线（driver-mode/EHsc/std 已在函数内
         * 先行压入，基线中的重复项跳过）。
         * @param gnu_base gnu 方言合成基线（-std= 已由函数压入，跳过）。
         * @param dep_feeds 依赖闭包的 (模块名, pcm 路径) 对。
         */
        [[nodiscard]]
        auto bmi_replay_argv(
            const MsvcModuleStrip*          recorded_stripped,
            bool                            dialect_cl,
            std::string_view                cl_std,
            std::string_view                std_value,
            const std::vector<std::string>& langopt_axes,
            const std::vector<std::string>& cl_base,
            const std::vector<std::string>& gnu_base,
            const std::vector<std::pair<std::string, std::string>>& dep_feeds)
            -> std::vector<std::string>
        {
            std::vector<std::string> argv;
            if (recorded_stripped != nullptr)
            {
                argv.insert(
                    argv.end(),
                    recorded_stripped->argv.begin(),
                    recorded_stripped->argv.end());
                if (std::ranges::find_if(
                        argv,
                        [](const std::string& a)
                        {
                            return a.starts_with("-std=")
                                   || a.starts_with("/std:");
                        })
                    == argv.end())
                {
                    if (dialect_cl)
                    {
                        argv.emplace_back(cl_std);
                    }
                    else
                    {
                        argv.emplace_back(std::format("-std={}", std_value));
                    }
                }
            }
            else if (dialect_cl)
            {
                argv.emplace_back("--driver-mode=cl");
                argv.emplace_back("/EHsc");
                argv.emplace_back(cl_std);
                for (const std::string& a : cl_base)
                {
                    if (a == "--driver-mode=cl" || a == "/EHsc" || a == cl_std)
                    {
                        continue;
                    }
                    argv.push_back(a);
                }
            }
            else
            {
                argv.emplace_back(std::format("-std={}", std_value));
                for (const std::string& axis : langopt_axes)
                {
                    argv.push_back(axis);
                }
                for (const std::string& a : gnu_base)
                {
                    if (a.starts_with("-std="))
                    {
                        continue;
                    }
                    argv.push_back(a);
                }
            }
            argv.emplace_back("-x");
            argv.emplace_back("c++-module");
            argv.emplace_back("--precompile-reduced-bmi");
            for (const auto& [dep, pcm] : dep_feeds)
            {
                argv.emplace_back(std::format("-fmodule-file={}={}", dep, pcm));
            }
            return argv;
        }

        /**
         * @brief 模块单元文本强制包含的剥离结果（相邻发现配套项）。
         *
         * @internal argv 是剥除后的向量（没有命中时与输入逐字节相同）；
         * removed 记名被剥除的强制包含路径（值缺席的裸旗标剥 token 不记
         * 名），供逐文件诊断使用。
         */
        struct ForcedIncludeStrip
        {
            std::vector<std::string> argv;
            std::vector<std::string> removed;
        };

        /**
         * @brief 从模块单元的重放 argv 剥除文本强制包含（只剥、绝不改写）。
         *
         * 文本强制包含会把头内容注到翻译单元最前——先于 module 声明，clang
         * 对此硬错误（module declaration must occur at the start of the
         * translation unit，含注解-only 的空伞头同样触发）。序列化 PCH 不受
         * 此限（ASTReader 附件路径），因此剥的是文本形态：gnu 方言的转发
         * 对 "-Xclang -include -Xclang 值"（装载层绕开隐式 .pch 加载的改写
         * 形态）与裸 "-include 值"/"-include=值"；cl 方言的 "/FI 值" 与
         * "/FI值"（两段式与冒号无关的连写，前缀 '/'/'-' 等价、ASCII 大小写
         * 不敏感，与模块开关剥离同口径）。"-Xclang -include-pch" 是序列化
         * PCH 通道，不属于本剥离面——它由 PCH 规划注入或回退剥离。
         */
        [[nodiscard]]
        auto strip_module_text_forced_includes(
            const std::vector<std::string>& argv) -> ForcedIncludeStrip
        {
            const auto cl_flag_prefix = [](const std::string& t,
                                           std::string_view   name,
                                           std::string&       joined)
            {
                joined.clear();
                if (t.size() < name.size() + 1 || (t[0] != '/' && t[0] != '-'))
                {
                    return false;
                }
                if (!cg::util::ascii_iequals(
                        std::string_view{t}.substr(1, name.size()),
                        name))
                {
                    return false;
                }
                if (t.size() > name.size() + 1)
                {
                    joined = t.substr(name.size() + 1);
                }
                return true;
            };
            ForcedIncludeStrip out;
            out.argv.reserve(argv.size());
            for (std::size_t i = 0; i < argv.size();)
            {
                const std::string& t = argv[i];
                // 转发对 "-Xclang -include"（值形态 "-Xclang 值" 或裸 "值"，
                // 装载层恒用前者；值缺席的悬挂对也一并剥除）。
                if (t == "-Xclang" && i + 1 < argv.size()
                    && argv[i + 1] == "-include")
                {
                    i += 2;
                    if (i + 1 < argv.size() && argv[i] == "-Xclang")
                    {
                        out.removed.push_back(argv[i + 1]);
                        i += 2;
                    }
                    else if (
                        i < argv.size()
                        && !cg::clang::detail::is_gnu_flag_token(argv[i]))
                    {
                        out.removed.push_back(argv[i]);
                        ++i;
                    }
                    continue;
                }
                // 裸 gnu 形态（记录条目未走 PCH 感知解析时原样保留的形状）。
                if (t == "-include")
                {
                    if (cg::clang::detail::gnu_value_follows(argv, i))
                    {
                        out.removed.push_back(argv[i + 1]);
                        i += 2;
                    }
                    else
                    {
                        ++i; // 值缺席的裸旗标：剥 token 不记名
                    }
                    continue;
                }
                if (t.starts_with("-include="))
                {
                    out.removed.push_back(t.substr(9));
                    ++i;
                    continue;
                }
                // cl 形态 "/FI 值" 与 "/FI值"（两段式的值以 '/'、'-' 开头判定
                // 为旗标（cl 型守卫单点），绝不吞）。连写形态按前缀分治：
                // '/' 前缀无 gnu 命名空间冲突，名字大小写从宽（与模块开关
                // 剥离同口径）；'-' 前缀连写必须精确大写 "FI"——cl 的 '-' 与
                // '/' 前缀等价但选项名大小写敏感，小写 "-fi..." 连写落进
                // gnu 家 -f 旗标命名空间（-finput-charset=GBK 曾被当
                // "-FI nput-charset=GBK" 吞掉，模块单元从此按 UTF-8 解析
                // GBK 源码）；两段式为整 token 精确比对、无撞名面，'/' 与
                // '-' 双前缀保留。
                std::string joined;
                if (cl_flag_prefix(t, "FI", joined)
                    && (joined.empty() || t[0] == '/'
                        || t.compare(1, 2, "FI") == 0))
                {
                    if (!joined.empty())
                    {
                        out.removed.push_back(std::move(joined));
                        ++i;
                    }
                    else if (cg::clang::detail::cl_value_follows(argv, i))
                    {
                        out.removed.push_back(argv[i + 1]);
                        i += 2;
                    }
                    else
                    {
                        ++i; // 值缺席的裸旗标：剥 token 不记名
                    }
                    continue;
                }
                out.argv.push_back(t);
                ++i;
            }
            return out;
        }

    } // namespace detail

    /**
     * @brief 经典源文件是否携带 import 形行——决定哪些非模块 TU 需要
     * 逐文件 P1689 扫描、编译数据库路线上哪些记录条目不能逐字重放的
     * 廉价预过滤器（只有 import 的消费者需要合成投喂）。
     *
     * 导出给 CLI 路由层与索引器的投喂缺席守卫。
     */
    [[nodiscard]]
    auto text_has_import(const std::string& file) -> bool
    {
        std::ifstream in{std::filesystem::path{file}, std::ios::binary};
        if (!in)
        {
            return false;
        }
        std::string line;
        while (std::getline(in, line))
        {
            std::size_t at = 0;
            while (at < line.size()
                   && (line[at] == ' ' || line[at] == '\t' || line[at] == '\r'))
            {
                ++at;
            }
            std::string_view rest{line.data() + at, line.size() - at};
            if (rest.starts_with("export "))
            {
                rest.remove_prefix(7);
                while (!rest.empty() && rest.front() == ' ')
                {
                    rest.remove_prefix(1);
                }
            }
            if (rest.starts_with("import ") || rest.starts_with("import\t"))
            {
                return true;
            }
        }
        return false;
    }

    namespace detail
    {

        /**
         * @brief 单趟 clang-scan-deps P1689 扫描：编译数据库 json 落临时
         *        文件、run_tool 发射、解析规则集。
         *
         * @internal self_build_modules 的 scan_batch 与纯头单元语料的
         *           classic 降级通道共用同一发射形状（argv0 + 逐文件
         *           旗标 + 文件清单），差异只在调用方组装的旗标向量；
         *           抽出共享体使两处发射逐字节同源。
         *
         * @param root           编译数据库的 directory 字段（工作目录）。
         * @param argv0          扫描命令首 token（驱动器或垫片）。
         * @param scan_deps      clang-scan-deps 可执行路径。
         * @param files          待扫描文件（绝对路径）。
         * @param module_units_batch  true 时追加 -x c++-module（模块单元批）。
         * @param flags_per_file 与 files 对齐的扫描旗标（每文件一份）。
         * @param scan_err       可空；带回扫描器输出的首行（失败归因）。
         */
        [[nodiscard]]
        auto run_p1689_scan(
            const std::string&                           root,
            const std::string&                           argv0,
            const std::string&                           scan_deps,
            const std::vector<std::string>&              files,
            bool                                         module_units_batch,
            const std::vector<std::vector<std::string>>& flags_per_file,
            std::string* scan_err) -> std::vector<ScanRule>
        {
            namespace fs = std::filesystem;
            namespace json = cg::util::json;
            json::Array db;
            for (std::size_t i = 0; i < files.size(); ++i)
            {
                json::Object entry;
                entry.set("directory", root.empty() ? "." : root);
                json::Array argv_json;
                argv_json.add(argv0);
                for (const std::string& a : flags_per_file[i])
                {
                    argv_json.add(a);
                }
                if (module_units_batch)
                {
                    argv_json.add("-x");
                    argv_json.add("c++-module");
                }
                argv_json.add(files[i]);
                entry.set("arguments", std::move(argv_json));
                entry.set("file", files[i]);
                db.add(std::move(entry));
            }
            const std::string db_path = (fs::temp_directory_path()
                                         / std::format(
                                             "ccg-scan-{}.json",
                                             std::chrono::steady_clock::now()
                                                 .time_since_epoch()
                                                 .count()))
                                            .generic_string();
            {
                std::ofstream of{fs::path{db_path}, std::ios::binary};
                of << json::stringify(std::move(db));
            }
            const detail::ProcResult r = detail::run_tool(
                scan_deps,
                {"-compilation-database=" + db_path, "-format=p1689"});
            std::error_code ec;
            fs::remove(fs::path{db_path}, ec);
            if (scan_err != nullptr)
            {
                // 扫描器 stderr 首行不再被整体吞噬（拼接先例同 std 失败路
                // 径：err 优先，out 兜底）——降级文案用它归因。
                *scan_err =
                    r.started
                        ? detail::first_line_of(r.err.empty() ? r.out : r.err)
                        : std::string{"spawn failed"};
            }
            if (!r.started)
            {
                return {};
            }
            return detail::parse_p1689(r.out);
        }

        /**
         * @brief 纯头单元语料的 classic 降级半边（零命名模块单元通道）。
         *
         * @internal module_units 为空时 self_build_modules 无 BMI DAG 可
         *           建、无 std 可投喂，但 classic TU 文本书写的
         *           `import "x.hpp";` 仍需降级为 #include（与
         *           apply_degrade_rules 的头单元半边同一策略），否则 TU
         *           对裸 argv 解析必败（"not known to be a header
         *           unit"）。扫描规则门槛沿用 classic 半边：有 P1689 规则
         *           才进降级（规则的 requires 不报告头单元，头单元名单走
         *           文本扫描）；std 伞头半边依赖模块 DAG（nodes），本通道
         *           恒无节点、整段不适用。requires 逐名入 scan_imports
         *           （失败 TU 的 imports 行兜底），头单元拼写去引号后与
         *           文本扫描结果并集进 overlay_imports。
         *
         * @param opt      自建选项（root/std 值/db 路线标记）。
         * @param argv0    扫描命令首 token（驱动器）。
         * @param scan_deps clang-scan-deps 可执行路径。
         * @param classic  经典编译命令（就地增强：overlay 与 imports 面）。
         * @return 被（至少）附着了头单元降级的条目数。
         */
        auto degrade_classic_header_unit_imports(
            const SelfBuildOptions&      opt,
            const std::string&           argv0,
            const std::string&           scan_deps,
            std::vector<CompileCommand>& classic) -> std::size_t
        {
            std::unordered_map<std::string, ScanRule> classic_rules;
            for (const CompileCommand& cc : classic)
            {
                if (!text_has_import(cc.file))
                {
                    continue;
                }
                std::vector<std::string> flags;
                if (opt.db_mode)
                {
                    // P1689 伪预处理只关心模块语法；记录 argv 剥掉 MSVC
                    // 模块开关后作扫描旗标（gnu 方言下为恒等操作）。
                    flags = detail::strip_msvc_module_switches(cc.argv).argv;
                }
                else
                {
                    flags.emplace_back(std::format("-std={}", opt.std_value));
                    if (!opt.root.empty())
                    {
                        flags.emplace_back("-I");
                        flags.push_back(opt.root);
                    }
                }
                const std::string           norm = cg::util::norm_path(cc.file);
                const std::vector<ScanRule> rules = detail::run_p1689_scan(
                    opt.root,
                    argv0,
                    scan_deps,
                    {norm},
                    false,
                    {flags},
                    nullptr);
                if (rules.size() == 1)
                {
                    classic_rules.emplace(norm, rules.front());
                }
            }
            std::size_t touched = 0;
            for (CompileCommand& cc : classic)
            {
                const auto rit =
                    classic_rules.find(cg::util::norm_path(cc.file));
                if (rit == classic_rules.end())
                {
                    continue; // 无扫描规则：不是模块消费者，维持原样
                }
                const bool before = cc.overlay.header_unit_includes;
                for (const ScanRequire& rq : rit->second.requires_names)
                {
                    cc.scan_imports.push_back(rq.name);
                    if (looks_like_header_unit(rq.name))
                    {
                        cc.overlay.header_unit_includes = true;
                        std::string name = rq.name;
                        if (name.size() >= 2
                            && ((name.front() == '"' && name.back() == '"')
                                || (name.front() == '<' && name.back() == '>')))
                        {
                            name = name.substr(1, name.size() - 2);
                        }
                        cc.overlay_imports.push_back(std::move(name));
                    }
                }
                for (const std::string& name :
                     text_header_unit_imports(cc.file))
                {
                    cc.overlay.header_unit_includes = true;
                    cg::util::push_unique(cc.overlay_imports, name);
                }
                if (!before && cc.overlay.header_unit_includes)
                {
                    ++touched;
                }
            }
            return touched;
        }

    } // namespace detail

    /**
     * @brief 一条记录条目的构建配置签名：其记录 .modmap 响应文件路径中
     * ".dir/" 后的路径段。
     *
     * （CMake 的逐目标逐配置布局，如 "…/<target>.dir/Release/util/
     * util.ixx.obj.modmap" -> "Release"；无此段的布局为 ""。）
     *
     * @internal 编译数据库路线按此签名给无投喂条目分组，使每个配置的
     * BMI 变体各居其所（每配置一趟自建；变体的权威——记录 argv——各不
     * 相同）。任何稳定的逐配置判别式都行；一个配置就是 CMake 布局中的
     * 一个目录名。
     */
    [[nodiscard]]
    auto db_config_signature(const CompileCommand& cc) -> std::string
    {
        const auto segment_after_dir =
            [](std::string_view path) -> std::optional<std::string>
        {
            const std::size_t at = path.find(".dir/");
            if (at == std::string_view::npos)
            {
                return std::nullopt;
            }
            const std::size_t begin = at + 5;
            const std::size_t slash = path.find('/', begin);
            if (slash == std::string_view::npos || slash == begin)
            {
                return std::nullopt;
            }
            return std::string{path.substr(begin, slash - begin)};
        };
        if (!cc.module_map.empty())
        {
            return segment_after_dir(cc.module_map).value_or(std::string{});
        }
        // 经典条目（无 modmap）：同一 ".dir/`<段>`/" 判别式施于全部 argv 记号
        // （CMake 的 -o 目标路径同按 .dir/`<配置>`/ 布局）。单配置生成器
        // ".dir/" 后是源子路径的噪声，由调用方（拆分裁决）以"同文件跨签名
        // 重复出现"的判别式滤除。
        for (const std::string& token : cc.argv)
        {
            if (const auto seg = segment_after_dir(token))
            {
                return *seg;
            }
        }
        return {};
    }

    /**
     * @brief 为已加载引擎按标准库实现发现 std 模块源。
     *
     * 顺序：显式 $CCG_STD_SOURCE 覆盖（std 模块接口路径；
     * $CCG_STD_COMPAT_SOURCE 可选相伴）——然后 libc++（驱动器自身的
     * libc++.modules.json，以源位于驱动器安装前缀之下为守卫）——然后
     * libstdc++（经驱动器或 g++：$CCG_GXX，再 PATH；以驱动器报告的
     * include 列表确实含该 GCC 头根为守卫——默认 libc++ 或 MSVC 的驱动
     * 器绝不可被喂异风味的 std pcm）——然后驱动器自动探测到的工具集的
     * MSVC STL（modules/std.ixx，下限 14.44）。
     *
     * @internal GCC 头根是 bits/ 目录的父目录。工具集下限之下的标准库实现
     * 报告自身但不构建（降级层）。
     */
    [[nodiscard]]
    auto discover_std_source() -> StdModuleSource
    {
        namespace fs = std::filesystem;
        const DriverInfo& info = driver_info();
        if (info.driver.empty())
        {
            StdModuleSource none;
            none.note = "no clang driver resolved for the loaded engine";
            return none;
        }
        const std::string driver_prefix = [&]
        {
            fs::path p = fs::path{info.driver}.parent_path();
            if (p.filename() == "bin")
            {
                p = p.parent_path();
            }
            return p.lexically_normal().generic_string();
        }();

        if (const auto env = cg::util::cstd::get_env("CCG_STD_SOURCE"))
        {
            const std::string path = cg::util::norm_path(*env);
            std::error_code   ec;
            if (fs::is_regular_file(fs::path{path}, ec))
            {
                StdModuleSource src;
                src.stdlib_impl = "override";
                src.std_path = path;
                src.toolset_id = std::format("env:{}", path);
                if (const auto compat =
                        cg::util::cstd::get_env("CCG_STD_COMPAT_SOURCE"))
                {
                    const std::string cpath = cg::util::norm_path(*compat);
                    if (fs::is_regular_file(fs::path{cpath}, ec))
                    {
                        src.compat_path = cpath;
                    }
                }
                return src;
            }
        }

        if (const std::string json =
                detail::print_file_name(info.driver, "libc++.modules.json");
            !json.empty())
        {
            if (const auto json_text = cg::util::read_text_file(json))
            {
                const detail::ModulesJson parsed = detail::parse_modules_json(
                    *json_text,
                    fs::path{json}.parent_path().generic_string());
                if (!parsed.std_path.empty()
                    && detail::under_dir(parsed.std_path, driver_prefix))
                {
                    StdModuleSource src;
                    src.stdlib_impl = "libc++";
                    src.std_path = parsed.std_path;
                    src.compat_path = parsed.compat_path;
                    for (const std::string& dir : parsed.wiring)
                    {
                        src.wiring.emplace_back("-isystem");
                        src.wiring.push_back(dir);
                    }
                    src.toolset_id = std::format("libc++:{}", driver_prefix);
                    return src;
                }
            }
        }

        const auto try_libstdcxx =
            [&](const std::string& json,
                const std::string& origin) -> std::optional<StdModuleSource>
        {
            if (json.empty())
            {
                return std::nullopt;
            }
            const auto json_text = cg::util::read_text_file(json);
            if (!json_text.has_value())
            {
                return std::nullopt;
            }
            const detail::ModulesJson parsed = detail::parse_modules_json(
                *json_text,
                fs::path{json}.parent_path().generic_string());
            if (parsed.std_path.empty())
            {
                return std::nullopt;
            }
            const std::string cxx_root = cg::util::norm_path(
                fs::path{parsed.std_path}
                    .parent_path()
                    .parent_path()
                    .generic_string());
            for (const std::string& dir : info.cxx_include_dirs)
            {
                if (dir == cxx_root)
                {
                    StdModuleSource src;
                    src.stdlib_impl = "libstdc++";
                    src.std_path = parsed.std_path;
                    src.compat_path = parsed.compat_path;
                    src.toolset_id = origin;
                    src.toolset_id += ':';
                    src.toolset_id += cxx_root;
                    return src;
                }
            }
            return std::nullopt;
        };
        if (auto src = try_libstdcxx(
                detail::print_file_name(info.driver, "libstdc++.modules.json"),
                "driver"))
        {
            return *src;
        }
        std::string gxx;
        if (const auto env = cg::util::cstd::get_env("CCG_GXX"))
        {
            gxx = cg::util::norm_path(*env);
            std::error_code ec;
            if (!fs::is_regular_file(fs::path{gxx}, ec))
            {
                gxx.clear();
            }
        }
        if (gxx.empty())
        {
            if (const auto env = cg::util::cstd::get_env("PATH"))
            {
                gxx = detail::gxx_on_path_from(*env);
            }
        }
        if (!gxx.empty())
        {
            if (auto src = try_libstdcxx(
                    detail::print_file_name(gxx, "libstdc++.modules.json"),
                    "g++"))
            {
                return *src;
            }
        }

        const auto [toolset, version] =
            detail::msvc_toolset_from_report(info.cxx_include_dirs);
        if (!toolset.empty())
        {
            if (auto src = detail::msvc_stl_source(
                    toolset,
                    version,
                    info.cxx_include_dirs,
                    {}))
            {
                return *src;
            }
        }

        StdModuleSource none;
        none.note = "no std module source discovered (libc++ json via the "
                    "driver, libstdc++ json via the driver/g++, MSVC toolset "
                    "all failed for this engine's wiring)";
        return none;
    }

    namespace detail
    {

        /**
         * @brief 一个 DAG 节点（项目接口、分区、或 std 伪节点）。
         *
         * @internal direct 是直接 requires（已解析名）。拓扑走查后的解析
         * 状态：reusable = 磁盘有 pcm 且键匹配、无需重建；available = 已构建
         * 或可复用；pcm 是绝对路径（不可用时为 ""）；failure 是不可用时的
         * 点名类文本。std 伪节点的 source 为 ""。
         */
        struct BmiNode
        {
            std::string              name;
            std::string              source;
            bool                     interface = true;
            std::vector<std::string> direct;
            bool        reusable = false;  // 磁盘有 pcm + 键匹配，无需重建
            bool        available = false; // 已构建或可复用
            std::string pcm;               // 绝对 pcm 路径（不可用时为 ""）
            std::string failure;           // 不可用时的点名类文本
        };

        /**
         * @brief 接口节点的 Kahn 分层：每层收集"度数同时归零"的一批接口，
         *        层内保持名字序（确定性）；层间只依赖不交叉。
         *
         * @internal 同层两节点不可能有依赖边（u 传递依赖 v 则 u 的度数归零
         * 必然晚于 v，u 层 > v 层），互不在彼此传递闭包内——这是层内可并行
         * 构建、层间屏障后下游闭包定案的依据。未入任何层的节点在环上（或
         * 依赖环），由调用方按展平集标 module-dag-cycle（文本不变）。std
         * 伪节点绝不阻塞分层。
         */
        [[nodiscard]]
        auto topo_layers(
            const std::unordered_map<std::string, BmiNode>& nodes,
            const std::vector<std::string>&                 interface_names)
            -> std::vector<std::vector<std::string>>
        {
            std::unordered_map<std::string, std::size_t> degree;
            std::unordered_map<std::string, std::vector<std::string>>
                dependents;
            for (const std::string& name : interface_names)
            {
                const auto it = nodes.find(name);
                if (it == nodes.end())
                {
                    continue;
                }
                std::size_t pending = 0;
                for (const std::string& dep : it->second.direct)
                {
                    const auto dit = nodes.find(dep);
                    if (dit == nodes.end() || !dit->second.interface)
                    {
                        continue; // std 伪节点绝不阻塞分层
                    }
                    ++pending;
                    dependents[dep].push_back(name);
                }
                degree[name] = pending;
            }
            std::vector<std::string> ready;
            for (const auto& [name, pending] : degree)
            {
                if (pending == 0)
                {
                    ready.push_back(name);
                }
            }
            std::ranges::sort(ready);
            std::vector<std::vector<std::string>> layers;
            while (!ready.empty())
            {
                // 整层弹出：度数归零轮次即并行波次，层内名字序延续既有确定性
                // 纪律；弹层时对层内节点的 dependents 减度收集下层。
                layers.push_back(ready);
                std::vector<std::string> next;
                for (const std::string& name : ready)
                {
                    for (const std::string& follower : dependents[name])
                    {
                        if (--degree[follower] == 0)
                        {
                            next.push_back(follower);
                        }
                    }
                }
                std::ranges::sort(next);
                ready = std::move(next);
            }
            return layers;
        }

        /**
         * @brief 节点 requires 在节点表上的传递闭包（按调用方记忆化）。
         *
         * @internal 这是分区与 `export import` 再导出整模块都必须在每次消费
         * 编译被投喂的依据。
         */
        [[nodiscard]]
        auto closure_of(
            const std::string&                                         name,
            const std::unordered_map<std::string, BmiNode>&            nodes,
            std::unordered_map<std::string, std::vector<std::string>>& memo)
            -> std::vector<std::string>
        {
            if (const auto it = memo.find(name); it != memo.end())
            {
                return it->second;
            }
            std::vector<std::string> out;
            const auto               it = nodes.find(name);
            if (it != nodes.end())
            {
                std::unordered_set<std::string> seen;
                std::vector<std::string>        stack = it->second.direct;
                while (!stack.empty())
                {
                    const std::string dep = stack.back();
                    stack.pop_back();
                    if (!seen.insert(dep).second)
                    {
                        continue;
                    }
                    out.push_back(dep);
                    if (const auto dit = nodes.find(dep); dit != nodes.end())
                    {
                        for (const std::string& deeper : dit->second.direct)
                        {
                            stack.push_back(deeper);
                        }
                    }
                }
            }
            std::ranges::sort(out);
            memo.emplace(name, out);
            return out;
        }

        /**
         * @brief TU 的 P1689 扫描规则是否牵涉 std 模块（直接 require 或经依赖
         *        闭包传递）。
         *
         * 这是 std 伞头 overlay（`import std;` 行替换为工具链伞头文件、
         * std 系投喂丢弃）的第一道前提：不 import std 的 TU（哪怕文本包含
         * 整个 STL——stdafx 文本包含是合法且常见的 MSVC 形态）绝不进入伞头
         * 层，否则伞头替换无从发生而 std 系投喂已被抹除，解析反被拆台。
         * 头单元 require（名字即头路径拼写）与 std 无关，恒不触发。闭包传递
         * 是真实前提而非保守放大：依赖模块的 BMI 自建自 std 系 pcm，投喂它
         * 就传递加载 std.pcm，与 TU 文本包含的 STL 头冲突（two-stdlib-clash）。
         *
         * @param reqs 该 TU 的 P1689 requires（扫描规则原文；/reference 名单
         *             兜底由调用方并入后同样适用）
         * @param nodes 工程模块 DAG（含 std 伪节点）
         * @param memo  闭包记忆化（调用方持有，跨 TU 复用）
         */
        [[nodiscard]]
        auto requires_std(
            const std::vector<ScanRequire>&                            reqs,
            const std::unordered_map<std::string, BmiNode>&            nodes,
            std::unordered_map<std::string, std::vector<std::string>>& memo)
            -> bool
        {
            return std::ranges::any_of(
                reqs,
                [&](const ScanRequire& rq)
                {
                    if (looks_like_header_unit(rq.name))
                    {
                        return false;
                    }
                    if (rq.name == "std" || rq.name == "std.compat")
                    {
                        return true;
                    }
                    const std::vector<std::string> closure =
                        closure_of(rq.name, nodes, memo);
                    return std::ranges::find(closure, "std") != closure.end()
                           || std::ranges::find(closure, "std.compat")
                                  != closure.end();
                });
        }

        /**
         * @brief 记录 argv 可能携带的 PCM 语言选项轴白名单（编译数据库路线）。
         *
         * @internal
         * 其 cc1 形态落入 clang LangOptions 模块兼容身份的驱动器 flag 白名
         * 单。不带某 flag 产出的 std/BMI 不能被带着该 flag 记录的 TU import
         * ——clang 在 import 处拒绝 pcm（"configuration mismatch" AST-read 失
         * 败，在 -fms-extensions 形状上实测）。db 路线上记录 argv 是权威，
         * 故 ccg 自己合成的一切 argv（std pcm 生产、无权威单元的基线）必须
         * 携带相同轴，否则链给自己的记录命令喂一个不匹配的 std。
         * 依据（clang 源码，LangOptions.def 与 ASTReader 的语言选项校验）：
         * 模块文件恰好序列化未标记 Benign 的 LangOptions 条目、并在读取时
         * 校验它们——Benign 条目被 resetNonModularOptions 重置、从不比较。
         * 白名单命名对 C++ 模块构建要紧、且 CMake 式构建整树统一施加的非
         * Benign 条目的驱动器拼写：-fms-extensions（MicrosoftExt）、
         * -fms-compatibility（MSVCCompat）、-fms-compatibility-version=V
         * （MSCompatibilityVersion——唯一的值轴，按 flag 前缀匹配并整段捕
         * 获）、-fdelayed-template-parsing（clang 表中 Benign——保守保留为
         * 额外轴：整树统一记录它的构建仍干净分组，一个多余的 std 变体远比
         * 静默混形状便宜）、-frtti（RTTI）、-fexceptions（Exceptions/
         * CXXExceptions）、-fshort-wchar（WCharSize/WCharIsSigned）、
         * -fchar8_t（Char8）。不在内：宏（-D）与路径（-I/-isystem）选项
         * （它们整形预处理器、不整形 pcm 身份）与 -std（它骑自己的记录值
         * 轴）。按 TOKEN 匹配：条目精确相等，唯独以 '=' 结尾的条目匹配任意
         * 值延续（整 token 含值才是向量携带的）。
         */
        inline constexpr std::string_view PCM_LANGOPT_AXES[] = {
            "-fms-extensions",
            "-fno-ms-extensions",
            "-fms-compatibility",
            "-fno-ms-compatibility",
            "-fms-compatibility-version=",
            "-fdelayed-template-parsing",
            "-fno-delayed-template-parsing",
            "-frtti",
            "-fno-rtti",
            "-fexceptions",
            "-fno-exceptions",
            "-fshort-wchar",
            "-fchar8_t",
            "-fno-char8_t",
        };

    } // namespace detail

    /**
     * @brief 让指定的缓存 pcm 失效：原子重写清单并删除产物。
     *
     * @param pcm_paths 盘上 pcm 的绝对路径（调用方从失败的 feed 收集）。
     * @return 从清单中移除的条目数（产物删除是尽力而为）。
     *
     * @internal 打破“坏缓存永不收敛”：下游解析点名 bmi-config-mismatch /
     * module-bmi-missing 后，把对应条目从清单里原子移除并删掉 pcm，
     * 下一次自建/增量运行会把它当缺失重建。无锁：重读-修改-原子替换；
     * 并发写者最多丢一次重建，不会产生非法 JSON。引擎目录由路径推导
     * （`.../bmi-self-`<platform>`/`<engine_key>`/`<config_key>`/x.pcm`），找不到
     * 该布局的路径直接跳过（不是本缓存的产物）。
     */
    [[nodiscard]]
    auto invalidate_cached_bmi_artifacts(
        const std::vector<std::string>& pcm_paths) -> std::size_t
    {
        namespace fs = std::filesystem;
        // engine_dir -> 目标 pcm 的规范化绝对路径
        std::map<std::string, std::vector<std::string>> groups;
        for (const std::string& raw : pcm_paths)
        {
            const std::string path = cg::util::norm_path(
                fs::path{raw}.lexically_normal().generic_string());
            const fs::path p{path};
            const fs::path config_dir = p.parent_path();
            const fs::path engine_dir = config_dir.parent_path();
            if (engine_dir.empty() || config_dir.parent_path() == config_dir)
            {
                continue;
            }
            // 布局：.../bmi-self-`<platform>`/`<engine_key>`/`<config_key>`/x.pcm
            const std::string cache_family =
                engine_dir.parent_path().filename().generic_string();
            if (cache_family.empty()
                || cache_family.find("bmi-self-") == std::string::npos)
            {
                continue; // 不是 BMI 自建缓存的产物
            }
            groups[cg::util::norm_path(
                       engine_dir.lexically_normal().generic_string())]
                .push_back(path);
        }
        std::size_t removed_entries = 0;
        for (const auto& [engine_dir, targets] : groups)
        {
            const fs::path manifest_path{
                fs::path{engine_dir} / "manifest.json"};
            std::unordered_set<std::string> target_set(
                targets.begin(),
                targets.end());
            std::error_code read_ec;
            BmiManifest     manifest;
            bool            have_manifest = false;
            if (fs::is_regular_file(manifest_path, read_ec))
            {
                if (const auto manifest_text =
                        cg::util::read_text_file(manifest_path.string()))
                {
                    if (auto parsed =
                            detail::manifest_from_json(*manifest_text);
                        parsed.has_value())
                    {
                        manifest = std::move(*parsed);
                        have_manifest = true;
                    }
                }
            }
            if (have_manifest)
            {
                const std::size_t before = manifest.entries.size();
                std::erase_if(
                    manifest.entries,
                    [&](const BmiEntry& e)
                    {
                        const std::string abs = cg::util::norm_path(
                            (fs::path{engine_dir} / fs::path{e.pcm})
                                .lexically_normal()
                                .generic_string());
                        return target_set.contains(abs);
                    });
                removed_entries += before - manifest.entries.size();
                if (manifest.entries.size() != before)
                {
                    // 乐观合并：重读盘上清单并入并发写者的新增条目
                    // （被失效的键绝不复活）。读文本的 helper 返回即销毁
                    // 句柄——Windows 上未以 FILE_SHARE_DELETE 打开的句柄会
                    // 挡住随后的 atomic_replace，写前不留句柄。
                    std::unordered_set<std::string> keys;
                    for (const BmiEntry& e : manifest.entries)
                    {
                        keys.insert(std::format("{}\n{}", e.name, e.command));
                    }
                    if (const auto disk_text =
                            cg::util::read_text_file(manifest_path.string());
                        disk_text.has_value())
                    {
                        if (const auto disk =
                                detail::manifest_from_json(*disk_text);
                            disk.has_value())
                        {
                            for (const BmiEntry& e : disk->entries)
                            {
                                const std::string abs = cg::util::norm_path(
                                    (fs::path{engine_dir} / fs::path{e.pcm})
                                        .lexically_normal()
                                        .generic_string());
                                if (target_set.contains(abs))
                                {
                                    continue;
                                }
                                const std::string key =
                                    std::format("{}\n{}", e.name, e.command);
                                if (keys.insert(key).second)
                                {
                                    manifest.entries.push_back(e);
                                }
                            }
                        }
                    }
                    (void)cg::util::platform::atomic_write_file(
                        manifest_path,
                        detail::manifest_to_json(manifest));
                }
            }
            // 先重写清单再删产物：即使重写失败，缺失的 pcm 也会让下一次
            // 运行的尺寸校验判为不可复用而重建。
            for (const std::string& target : targets)
            {
                std::error_code rm;
                fs::remove(fs::path{target}, rm);
            }
        }
        return removed_entries;
    }

    /**
     * @brief 一个 argv 的 PCM 语言选项向量：argv 携带的每个白名单 token
     * （值拼写整段），去重排序——db 路线 std 变体拆分的组键。
     *
     * 全同向量的数据库坍缩为统一 flag 世界的单组；单元记录不同向量的
     * 数据库拆分，每组的趟（std pcm 生产、合成基线、消费者增强）恰好
     * 携带该组向量的轴。导出给 CLI 路由层做分组。
     */
    [[nodiscard]]
    auto pcm_langopt_vector(const std::vector<std::string>& argv)
        -> std::vector<std::string>
    {
        std::vector<std::string> hits;
        for (const std::string& arg : argv)
        {
            for (const std::string_view axis : detail::PCM_LANGOPT_AXES)
            {
                const bool hit =
                    arg == axis
                    || (axis.ends_with('=') && arg.starts_with(axis));
                if (hit)
                {
                    cg::util::push_unique(hits, arg);
                    break;
                }
            }
        }
        std::ranges::sort(hits);
        return hits;
    }

    /**
     * @brief 自建流水线（见模块级说明）。
     *
     * module_units 是根下发现的模块味文件；classic 携带非模块 TU 的兜底
     * 编译命令，并为 import 的消费者就地附着模块投喂。降级单元保留其命
     * 令（preordained_failure 已设、无投喂）：索引器上报点名类、其 import
     * 由 scan_imports 在 tier-1 落地。
     *
     * @internal
     * 前置校验：无驱动器时整批降级；生产者锁——产出每个 pcm 的驱动
     * 器必须与解析它们的引擎版本同一（否则整批降级并提示
     * CCG_LLVM_ROOT/--llvm-root）；找不到 clang-scan-deps 时整批降级
     * （P1689 扫描无法运行）。
     * 扫描段：cl_mode 把整条链改道经 clang-cl 配方，钉住的 include 根
     * 来自记录 cl 命令（构建实际用的工具集），否则驱动器自身自动探测
     * 的报告。cl 路由拥有 std 选择：TU 对着 MSVC STL 编译（记录 /I 根），
     * std 模块就是工具集自身的 std.ixx——libc++/libstdc++ pcm 会是静默
     * 错误的异风味。构建编排自身的工具集解析（如 VCToolsVersion 钉子）
     * 胜过一切扫描。记录 cl 目录与驱动器报告都不指名工具集时回落文件
     * 系统扫描（最新携带 modules/std.ixx 的工具集，下限在后施加）。解
     * 析到工具集后钉住其自身在布线中呈现的根：闸门绝不能是"布线为空"
     * ——真实录制带项目 /I 目录（msbuild 适配器剥工具集根，但 /I 加入的
     * SDK 目录在 MSVC 切片中存活），没有工具集 include 钉住时 clang-cl
     * 自动探测贡献最新安装工具集的头——14.44 目标构建的 std.pcm 静默对
     * 14.51 编译（ts1444 之险）。SDK 目录同理随行：缺了它们 SDK 轴掉出
     * 配置键、工具集升级复用陈旧 pcm，且混合 TU 探测器（键控于布线的
     * STL 切片）会丢 STL 根。记录的项目目录继续骑在工具集事实之后、去
     * 重。无 cl 路由发现（Windows 原生引擎自动探测工具集）时钉驱动器自
     * 身报告的根，使 std.pcm 生产确定。db 路线（clang/gcc 方言）：记录
     * argv 是 gnu 模式 flag，而 msvc-stl std 供给只在 clang-cl 方言可组
     * 合，此类引擎诚实降级 std 而非混合方言（libc++/libstdc++ 标准库
     * 生组合、继续）。本趟组的 PCM 语言选项轴（路由层从记录 argv 算出
     * 的组向量）必须骑 std pcm 生产与每个合成基线 argv，否则 clang 在
     * import 处按 LangOpts 失配拒绝 std pcm。生产/解析方言 = cl 路由或
     * msvc-stl 标准库（std.ixx pcm 链只在 clang-cl 方言组合）。
     * argv 基座：base_argv 是扫描命令与合成解析共享的（兜底路径解析所
     * 用的同一 include 布线）。cl 孪生 cl_base_argv 携带钉住的 MSVC 根；
     * /imsvc 只以 "-imsvc DIR" 空格分写形式发出、绝不用 "/imsvc:DIR" 冒
     * 号连写——冒号拼写粘上盘符冒号、cc1 收到指向不存在路径的
     * internal-isystem 条目（被静默忽略），头查找落到自动探测的最新工
     * 具集（14.44 目标构建静默对 14.51 头编译）；单元测试断言此点。逐单
     * 元扫描 flag 权威：db 模式取该文件编译数据库条目的记录 argv（与构
     * 建扫描时的同一 flag）——无记录条目的额外项回落链基线（合成基线
     * 同样骑记录轴：扫描的 LangOpts 必须与单元将被打生产与解析的相同）；
     * cl 与兜底模式保持链向量。scan_batch 一次 clang-scan-deps 调用跑合
     * 成编译数据库（"arguments" 数组形式——全程无 shell 引用）；cl 模式
     * 跑 cl 拼写条目：扫描器从 argv[0] 或命令内 --driver-mode=cl flag 取
     * 驱动模式（23.1.3 上实证：命令内形式产出完整 P1689 规则）；cl 条目
     * 里的 gnu -std 拼写被忽略、扫描落 c++14 默认（模块语法不可见），
     * 故 /std: 在此强制。scan_argv0 非空时重拼 db 的编译器 token
     * （clang-cl.exe 垫片兜底）。flags（设置时）覆盖逐单元权威（经典消
     * 费者扫描传消费者自己的记录 argv）。规则归属经 provides 的
     * source-path；未归属单元（无 provides 的规则或根本无规则——扫描器
     * 静默丢弃坏 TU 而 rc 保持 0，故规则存在即裁决）回落逐文件扫描。扫
     * 描集是项目模块单元（模块路由与自建管道的输入）。U-A 兜底：忽略命令内模式
     * flag 的扫描器会对每个模块单元返 回零规则——在 .ccg 目录物化 clang-cl
     * 垫片（argv[0] 携带模式）重
     * 扫。经典消费者：逐文件扫描（罕见——预过滤器跳过每个无书写 import 行的
     * TU）；db 模式用消费者自己的记录 argv 扫描（其 include/define 事实）。
     * 缓存键段：平台三元组经 -print-target-triple 取得并消毒。体系轴
     * （self/db）与 db 路线的构建配置标签骑配置键：键命名本趟 pcm 子目
     * 录，兜底/cl 链（单一规范生产向量）与每个 db 配置（各自的记录
     * argv 变体）各拥独立的 `<config_key>`/ 目录、内放纯模块名 pcm 文件。
     * 体系或配置变更落入不同目录、旧目录由孤儿目录清理回收；只有引
     * 擎变更抹掉整个引擎目录。记录的 PCM 语言选项轴同理
     * 骑键：db 停止（或开始）记录的轴改变 std pcm 的 LangOpts 身份，
     * 必须落入新目录而非覆盖活目录（生产命令摘要反正会重建它；键使两
     * 变体不致互为载体并存）。建目录时恰好一次请求压缩（之后的文件继
     * 承 flag）——压缩从不门控正确性。先前运行的清单：同一引擎键下的
     * 唯一台账（条目携带各自 `<config_key>`/ 子目录下的 pcm 路径），引擎
     * 匹配即接受；陈旧属主清单（另一引擎写过此目录）与引擎层扁平 pcm 的
     * 目录各自抹掉整个目录（另一引擎写的 pcms 是异格式、绝不与新者
     * 混合——崩溃类教训；清单旁出现任何常规文件即按扁平 pcm 目录处理，
     * 本布局绝不写入）。先前条目按
     * 名+命令摘要查取：db 路线每（模块， 生产命令）一条目——多配置
     * 数据库产出同模块名的多个变体（记录 argv 不同），命令变更必须完
     * 全错过先前条目（重建 + 级联）而非部分匹配。
     * DAG 段：来自每条规则的提供者（接口 + 分区）；两文件提供同名模块
     * 时该模块的一切消费者降级。实现单元只消费不提供。额外项修剪：只
     * 构建项目单元 import 的传递 requires 闭包（环境模块根可能带树从不
     * import 的连接器模块——构建它们耗时且会在缺失依赖上失败）。拓扑
     * 序落选者处于（或依赖）import 环：接口 BMI 无法产出。
     * std pcm 段：std pcm 落在本趟 `<config_key>`/ 目录、用纯名（配置维度
     * 是子目录、绝不是文件名指纹）。地板/工具集缺席降级（设计定案：
     * 14.44 之下零兼容投入——那些 std.ixx 世代的 STL 期导出错误无 flag
     * 向量可修）。姊妹引擎目录在本引擎下已死（绝不混喂新旧引擎
     * pcm），但只由显式 --force/--full 且超过 1 小时宽限期回收；增量/
     * update 保留它们（并发配置可能正靠其工作）。复用判定：同源内容
     * （stat 快路径 + 哈希确认）、同生产命令、pcm 在盘；db 路
     * 线每配置一趟、每配置自有 `<config_key>`/std.pcm——自身副本缺席的
     * 一趟从另一配置目录搬迁同一生产者产出（同命令摘要 = 同 pcm 字节）
     * 而非重编 30+ MB 接口。clang-cl 生产向量（配方，在 14.44 与 14.51
     * 工具集上证验）：cl 拼写、STL 源需要的 purview 内尖括号豁免、钉住
     * 的工具集/SDK 根、与 -clang:-o 的 BMI 输出命名（cl 模式
     * --precompile 下普通 -o 被静默忽略——输出默认 `<stem>`.pcm、覆盖危
     * 险——flag 与值各自包进 -clang:）。缩减 BMI：与 CMake
     * 在 -c 期间 -fmodule-output= 写出的工件字节相同，且关键的 parity
     * ——完整 pcm 序列化被 import 的定义、随后泄漏进同索引后续消费者
     * 解析成为带坏命名空间上下文的反序列化声明（自索引上实测：重复的
     * 空命名空间符号行）。记录语言选项轴同样骑 std 生产（std pcm 的
     * LangOpts 必须匹配将 import 它的记录 argv）。std 伪节点让闭包与消
     * 费者投喂看见它们。
     * 接口 BMI 段：闭包中不可用的依赖（环、构建失败、std 缺席）阻塞本
     * 节点与其全部下游（级联的降级方向——过度重建便宜、投喂坏集合是崩
     * 溃）。db 权威：接口单元的记录 argv（编译数据库自己对此文件
     * 的命令——构建用过的每个 -D/-I/-std/-target）加发接口增补；无记
     * 录条目的单元（cl 路由或无记录条目者）无权威可守：链基线适用、
     * 与其他生产向量一样折进摘要。防御性 std 钉：无任何 -std 的模块单
     * 元会骑驱动器默认（低于模块语法）——模块需要 >= c++20；记录遗漏
     * 以链的语言轴填补。无权威基线同样骑记录轴（此 pcm 与同轴下产出
     * 的 std pcm 及兄弟互喂互食）。兜底 include/define flag（
     * CCG_FALLBACK_FLAGS）随行：接口的全局模块片段必须在生产中解析其
     * 文本 include、恰如解析时。缩减 BMI 与上方 std 构建相同（与
     * CMake 消费者侧 pcm 对齐；完整 pcm 把定义泄漏进后续同索引解析）。
     * 记录的编译器身份折进摘要（以不同生产者编译器重录的数据库即命令
     * 变更）：argv 在加载时丢 argv[0]。布局重整：pcm 文件名是纯模块拼
     * 写（分区 mod:part 映射规则不变）；配置维度是本趟构建进的
     * `<config_key>`/ 子目录。一个配置内一个模块名恰有一条生产命令（db
     * 重复在路由层丢弃；兜底单向量），名字不可能相撞——命令变更重建同
     * 一文件、陈旧清单条目被下方活路径感知的清除丢弃。有效性：自身源
     * 未变（stat 快路径 + 哈希确认）、生产命令未变、每个依赖仍有效
     * （级联）、pcm 在盘。
     * 合成命令段：消费者解析向量（dialect_cl）配方 A = 带引擎自述 target
     * 与 3 轴的 cl 模式（-Xclang -fexternc-nounwind、-fstack-protector-
     * strong 与 -fms-compatibility 钉住，PATH 依赖的自动探测无从歪曲
     * /EHsc 会烤进的东西；target 取引擎驱动器 -print-target-triple 的
     * 自述、随主机体系走，探询失败回落写死的 x64 msvc 三元组并注记——
     * 写死值在 arm64 主机会把全部 TU 按 x64 ABI 烤进 BMI 身份）；配方 B
     * （CCG_MSVC_GNU_VECTOR，
     * 有文档的单元兜底、不是第二条流水线）：同一批 cl 构建 pcm 的 gnu
     * 模式解析——4 flag 向量作为一个单元施加。覆盖降级规则（模块单元
     * 与经典消费者共享）：头单元完全离开投喂解析（其 import 行改写为
     * `#include`）；会传递投喂 std 的 MSVC 地 TU——直接 import
     * std/std.compat、或 import 闭包绑 std 的任何模块（再导出链）——
     * 同时文本包含 STL 头的，绝不见 std.pcm：伞头覆盖替换 std import
     * 行、闭包含 std 的投喂全弃、被弃直接 import 的书写行消隐（无映射
     * 的 import 会让降级存活的解析硬失败）。冲突按规则避免、绝不事后
     * 诊断。扫描不报告的头单元 import 行（实测）从书写文本检测；无论
     * 方言都降级为 include（初版策略）。模块单元：接口 TU 喂其依赖
     * 闭包解析（绝不喂自己的 pcm）；降级单元携带点名类。db 模式消费
     * 者解析 argv 是记录 argv（权威）——兜底/cl 链向量绝不顶替记录命
     * 令；无权威额外项同样骑记录轴解析（其投喂——首当其冲 std pcm—
     * —带着轴产出）。接口单元需要显式语言：.ixx 对 clang 是"linker
     * input"；实现单元按普通 C++ 解析。例外是仅有头单元的降级：
     * 唯一模块级依赖是头单元（未书写全局模块片段）的接口降级为经典解
     * 析——模块声明同宽消隐、头单元 import 变文本 include（purview 内
     * include 系统味头是 ODR 错误，初版降级）。无显式 -x 时 .ixx 在
     * 两方言都是 "linker input"——去模块的解析仍需强制语言、只
     * 是非模块模式。db 路由单元的记录 argv 可能已强制模块语言；
     * 仅缺席时追加 -x c++-module，重放绝不携带双 token。本单元构建产出
     * 的 pcm 是失败分类学里该模块的生产者身份。经典消费者：同样投喂
     * 就地追加。
     * 清除段：db 路线上其他配置的变体（本次每配置一趟；每趟重写此清
     * 单）只要 pcm 存在且源仍在数据库模块单元之列就在趟内存活——同配
     * 置陈旧摘要（记录命令变了；其文件名此刻也是活条目的）、离开数据
     * 库的模块、或配置前时代的旧条目（config ""）不结转、被清除。项目
     * 模块需要其源在数据库单元中；std/std.compat 条目的源按设计在数
     * 据库之外（引擎自身安装）——其存活性随配置。模块离开 DAG（或其
     * 记录命令变更、文件已在同一纯名下重建）时：仅当无活/结转条目仍
     * 需要该文件才删除。零条目的探针趟（如 gnu 引擎下的 msvc-stl 标准库
     * 探测）不得持久化空台账——姊妹平台族目录保持干净（首条目时才建
     * 文件）。孤儿产物：只有显式 --force/--full 且 mtime 超过 1 小时宽限
     * 期、且不被重新读取的完整清单（含兄弟配置条目）引用时才删除；增量/
     * update 从不删除其它 `<config_key>`/ 目录（"作废一个配置" 不再由一次
     * 普通索引触发）。
     */
    [[nodiscard]]
    auto self_build_modules(
        const SelfBuildOptions&         opt,
        const std::vector<std::string>& module_units,
        std::vector<CompileCommand>&    classic) -> SelfBuildResult
    {
        namespace fs = std::filesystem;
        namespace json = cg::util::json;
        SelfBuildResult out;
        if (module_units.empty())
        {
            // 纯头单元语料（整库零命名模块单元）：无 BMI DAG 可建、无 std
            // 可投喂；classic 条目文本携带 import 时仍走 classic 降级半边
            // （import 行改写 #include），否则头单元消费者对裸 argv 解析
            // 必败（"not known to be a header unit"，TU 基线即
            // parse-failed）。无任何文本 import 的 classic 语料维持既有快
            // 路径原样返回；扫描器不可用（无驱动器/无 clang-scan-deps）
            // 时 classic_rules 建不起来，同样维持既有行为。
            const bool classic_import_face = std::ranges::any_of(
                classic,
                [](const CompileCommand& cc)
                { return text_has_import(cc.file); });
            if (classic_import_face)
            {
                const DriverInfo& info = driver_info();
                const std::string scan_deps =
                    detail::find_scan_deps(info.driver);
                if (!info.driver.empty() && !scan_deps.empty())
                {
                    const std::size_t touched =
                        detail::degrade_classic_header_unit_imports(
                            opt,
                            info.driver,
                            scan_deps,
                            classic);
                    if (touched > 0)
                    {
                        out.notes.push_back(
                            std::format(
                                "no named module units: {} classic TU(s) had "
                                "header-unit imports degraded to text includes",
                                touched));
                    }
                }
            }
            return out;
        }

        const DriverInfo& info = driver_info();
        if (info.driver.empty())
        {
            for (const std::string& unit : module_units)
            {
                CompileCommand cc;
                cc.directory = opt.root;
                cc.file = cg::util::norm_path(unit);
                cc.preordained_failure =
                    "self-build-unavailable: no clang driver resolved for the "
                    "loaded engine - module TU degrades to tier-1";
                out.commands.emplace_back(std::move(cc));
            }
            out.degraded = module_units.size();
            out.notes.emplace_back(
                "module BMIs not built: no clang driver resolved for the "
                "loaded engine");
            return out;
        }

        const std::string engine_version = []
        {
            try
            {
                return libclang_version();
            }
            catch (const std::exception&)
            {
                return std::string{};
            }
        }();
        const std::string engine_token = clang_version_token(engine_version);
        const std::string producer_token = compiler_version(
            info.driver,
            fs::path{info.driver}.parent_path().generic_string());

        if (engine_version.empty() || producer_token.empty()
            || producer_token != engine_token)
        {
            const std::string text = std::format(
                "bmi-config-mismatch [axis: pcm producer version (format "
                "lock)]: the discovered driver '{}' reports clang version "
                "'{}' but the loaded parse engine is '{}' - the self-build "
                "producer must be the engine's own clang; module TUs degrade "
                "to tier-1 (set CCG_LLVM_ROOT/--llvm-root to the engine "
                "installation)",
                info.driver,
                producer_token.empty() ? "<none>" : producer_token,
                engine_token.empty() ? "<none>" : engine_token);
            for (const std::string& unit : module_units)
            {
                CompileCommand cc;
                cc.directory = opt.root;
                cc.file = cg::util::norm_path(unit);
                cc.preordained_failure = text;
                out.commands.emplace_back(std::move(cc));
            }
            out.degraded = module_units.size();
            out.notes.push_back(text);
            return out;
        }

        const std::string scan_deps = detail::find_scan_deps(info.driver);
        if (scan_deps.empty())
        {
            const std::string text = std::format(
                "self-build-unavailable: clang-scan-deps not found beside the "
                "driver '{}' or on PATH - the P1689 scan cannot run, module "
                "TUs degrade to tier-1",
                info.driver);
            for (const std::string& unit : module_units)
            {
                CompileCommand cc;
                cc.directory = opt.root;
                cc.file = cg::util::norm_path(unit);
                cc.preordained_failure = text;
                out.commands.emplace_back(std::move(cc));
            }
            out.degraded = module_units.size();
            out.notes.push_back(text);
            return out;
        }

        const bool                     cl = opt.cl_mode;
        const std::vector<std::string> cl_dirs = [&]
        {
            if (!cl)
            {
                return std::vector<std::string>{};
            }
            return detail::msvc_include_slice(
                opt.cl_include_dirs.empty() ? driver_info().cxx_include_dirs
                                            : opt.cl_include_dirs);
        }();
        const auto [cl_std, gnu_twin_std] = detail::cl_std_of(opt.std_value);
        StdModuleSource std_source_resolved = [&]
        {
            StdModuleSource discovered = discover_std_source();
            if (cl)
            {
                auto [toolset, version] =
                    detail::msvc_toolset_from_report(cl_dirs);
                std::vector<std::string> wiring_dirs = cl_dirs;
                if (toolset.empty() && !opt.msvc_toolset_hint.empty())
                {
                    std::error_code hec;
                    if (fs::is_regular_file(
                            fs::path{opt.msvc_toolset_hint} / "modules"
                                / "std.ixx",
                            hec))
                    {
                        toolset = cg::util::norm_path(opt.msvc_toolset_hint);
                        version = fs::path{toolset}.filename().generic_string();
                    }
                }
                if (toolset.empty())
                {
                    for (const std::string& candidate :
                         detail::discover_msvc_toolsets())
                    {
                        const std::string v =
                            fs::path{candidate}.filename().generic_string();
                        if (!detail::toolset_at_floor(v))
                        {
                            continue;
                        }
                        toolset = candidate;
                        version = v;
                        break;
                    }
                }
                if (!toolset.empty())
                {
                    std::vector<std::string> pinned;
                    pinned.push_back(
                        cg::util::norm_path(
                            (fs::path{toolset} / "include").generic_string()));
                    std::error_code aec;
                    const fs::path  atlmfc =
                        fs::path{toolset} / "atlmfc" / "include";
                    if (fs::is_directory(atlmfc, aec))
                    {
                        pinned.push_back(
                            cg::util::norm_path(
                                atlmfc.lexically_normal().generic_string()));
                    }
                    for (const std::string& d :
                         detail::discover_windows_sdk_dirs())
                    {
                        pinned.push_back(d);
                    }
                    std::vector<std::string> merged;
                    for (const std::string& d : pinned)
                    {
                        cg::util::push_unique(merged, d);
                    }
                    for (const std::string& d : wiring_dirs)
                    {
                        cg::util::push_unique(merged, d);
                    }
                    wiring_dirs = std::move(merged);
                }
                if (toolset.empty())
                {
                    discovered = StdModuleSource{};
                    discovered.note =
                        "cl-mode chain found no MSVC toolset in the recorded or "
                        "auto-detected include roots - std.pcm cannot be produced";
                }
                else if (
                    auto src = detail::msvc_stl_source(
                        toolset,
                        version,
                        wiring_dirs,
                        wiring_dirs))
                {
                    discovered = *src;
                }
                else
                {
                    discovered = StdModuleSource{};
                    discovered.note = std::format(
                        "cl-mode chain: toolset {} carries no modules/std.ixx",
                        toolset);
                }
            }
            else if (discovered.stdlib_impl == "msvc-stl")
            {
                discovered.wiring =
                    detail::msvc_include_slice(driver_info().cxx_include_dirs);
            }
            return discovered;
        }();
        const bool                      db = opt.db_mode;
        const std::vector<std::string>& langopt_axes = opt.langopt_axes;
        // gnu 方言 db 路线的记录命令是 gnu 拼写，绝不与 clang-cl 方言的
        // msvc-stl std 生产混轴；cl 方言 db 路线（cl cdb 的记录 argv 权威）
        // 的记录命令本身就是 cl 拼写——方言一致，msvc-stl 供应链照常保留。
        if (db && !cl && std_source_resolved.stdlib_impl == "msvc-stl")
        {
            std_source_resolved = StdModuleSource{};
            std_source_resolved.note =
                "the engine's std implementation (msvc-stl) produces std.pcm "
                "only in the "
                "clang-cl dialect, but the db route replays gnu-dialect "
                "commands - dialects are never mixed";
        }
        const bool dialect_cl =
            cl || std_source_resolved.stdlib_impl == "msvc-stl";

        const auto base_argv = [&]
        {
            std::vector<std::string> argv = fallback_argv();
            if (!opt.root.empty())
            {
                argv.emplace_back("-I");
                argv.push_back(opt.root);
            }
            return argv;
        };
        const auto cl_base_argv = [&]
        {
            std::vector<std::string> argv{"--driver-mode=cl", "/EHsc", cl_std};
            const std::vector<std::string>& pin_dirs =
                std_source_resolved.stdlib_impl == "msvc-stl"
                        && std_source_resolved.wiring.empty()
                    ? cl_dirs
                    : std_source_resolved.wiring;
            for (const std::string& dir : pin_dirs)
            {
                argv.emplace_back("-imsvc");
                argv.push_back(dir);
            }
            for (const std::string& define : opt.cl_defines)
            {
                argv.emplace_back("/D");
                argv.push_back(define);
            }
            if (!opt.root.empty())
            {
                argv.emplace_back("-I");
                argv.push_back(opt.root);
            }
            return argv;
        };

        // 剥离层 + /reference 名单：记录 argv 的 MSVC 模块开关在每个消费
        // 点（扫描/BMI 生产/重放）剥除——clang 家工具一律不再见到它们；
        // /reference 的名字是构建系统声明的消费名单，喂给注入与扫描器结
        // 果取并集（扫描器漏检由它兜底）。gnu 方言命令无这些开关，剥离是
        // 逐字节恒等操作。
        std::unordered_map<std::string, detail::MsvcModuleStrip>
            stripped_recorded;
        for (const auto& [file, rec] : opt.recorded)
        {
            detail::MsvcModuleStrip strip =
                detail::strip_msvc_module_switches(rec.argv);
            stripped_recorded.emplace(file, std::move(strip));
        }
        std::unordered_map<std::string, std::vector<std::string>>
            reference_modules;
        for (const auto& [file, strip] : stripped_recorded)
        {
            if (!strip.references.empty())
            {
                reference_modules.emplace(file, strip.references);
            }
        }

        const auto unit_scan_flags =
            [&](const std::string& file) -> std::vector<std::string>
        {
            if (db)
            {
                if (const auto it = stripped_recorded.find(file);
                    it != stripped_recorded.end())
                {
                    return it->second.argv;
                }
                if (cl)
                {
                    return cl_base_argv(); // 无记录权威的 cl 单元：合成兜底
                }
                std::vector<std::string> flags;
                flags.emplace_back(std::format("-std={}", opt.std_value));
                for (const std::string& axis : langopt_axes)
                {
                    flags.push_back(axis);
                }
                for (const std::string& a : base_argv())
                {
                    if (!a.starts_with("-std="))
                    {
                        flags.push_back(a);
                    }
                }
                return flags;
            }
            if (cl)
            {
                return cl_base_argv();
            }
            return base_argv();
        };

        std::string scan_argv0;
        const auto  scan_batch =
            [&](const std::vector<std::string>& files,
                bool                            module_units_batch,
                const std::vector<std::string>* flags = nullptr,
                std::string* scan_err = nullptr) -> std::vector<ScanRule>
        {
            // 逐文件旗标展开（flags 缺席时走 unit_scan_flags 的兜底组装）
            // 后共享 detail::run_p1689_scan 的发射体（json 落盘、run_tool、
            // 解析）——与纯头单元语料的 classic 降级通道逐字节同源。
            std::vector<std::vector<std::string>> per_file;
            per_file.reserve(files.size());
            for (const std::string& file : files)
            {
                per_file.push_back(
                    flags != nullptr ? *flags : unit_scan_flags(file));
            }
            return detail::run_p1689_scan(
                opt.root,
                scan_argv0.empty() ? std::string{info.driver} : scan_argv0,
                scan_deps,
                files,
                module_units_batch,
                per_file,
                scan_err);
        };

        std::vector<std::string> scan_units;
        {
            std::unordered_set<std::string> seen;
            for (const std::string& unit : module_units)
            {
                const std::string norm = cg::util::norm_path(unit);
                if (seen.insert(norm).second)
                {
                    scan_units.push_back(norm);
                }
            }
        }
        std::unordered_map<std::string, ScanRule> rule_of_unit;
        std::string batch_scan_err; // 初始整批扫描的 stderr 首行（归属模糊）
        const auto  populate_rules = [&]
        {
            rule_of_unit.clear();
            std::unordered_map<std::string, ScanRule> by_source;
            for (ScanRule& rule :
                 scan_batch(scan_units, true, nullptr, &batch_scan_err))
            {
                for (const ScanProvide& p : rule.provides)
                {
                    if (!p.source.empty())
                    {
                        by_source.emplace(p.source, rule);
                    }
                }
            }
            for (const std::string& unit : scan_units)
            {
                if (const auto it = by_source.find(unit); it != by_source.end())
                {
                    rule_of_unit.emplace(unit, it->second);
                }
            }
        };
        populate_rules();
        // 布局锚（裁量 a/b）：BMI 缓存子树整体挂配置目录；垫片等引擎无关
        // 共享物留配置目录的父（布局文件夹根）。垫片物化在扫描兜底之前
        // 就需要它，故在此先行定锚。
        const fs::path    ccg_cfg = !opt.ccg_config_dir.empty()
                                        ? fs::path{opt.ccg_config_dir}
                                        : fs::path{opt.root} / ".ccg" / "self";
        const std::string shared_ccg_root = cg::util::norm_path(
            ccg_cfg.parent_path().lexically_normal().generic_string());

        if (cl && rule_of_unit.empty() && !scan_units.empty())
        {
            std::error_code ec;
            const fs::path  shim = fs::path{shared_ccg_root} / "clang-cl.exe";
            if (opt.dry_run)
            {
                // 干跑保真：垫片在扫描里只作 argv[0] 的身份供扫描器判方言
                // （从不被执行），无需物化文件即可指向未来路径重扫——干跑
                // 的规则集与降级计数和真实运行完全一致，不再更悲观。
                scan_argv0 = shim.lexically_normal().generic_string();
                populate_rules();
            }
            else
            {
                fs::create_directories(shim.parent_path(), ec);
                std::error_code copy_ec;
                fs::copy_file(
                    fs::path{info.driver},
                    shim,
                    fs::copy_options::overwrite_existing,
                    copy_ec);
                if (copy_ec)
                {
                    out.notes.push_back(
                        std::format(
                            "module BMIs: cl-mode scan produced no rules and the "
                            "clang-cl shim could not be materialized ({}) - module "
                            "TUs degrade to tier-1",
                            copy_ec.message()));
                }
                else
                {
                    scan_argv0 = shim.lexically_normal().generic_string();
                    populate_rules();
                }
            }
        }
        std::unordered_map<std::string, std::string> scan_err_of_unit;
        for (const std::string& unit : scan_units)
        {
            if (rule_of_unit.contains(unit))
            {
                continue;
            }
            std::string                 unit_err;
            const std::vector<ScanRule> rules =
                scan_batch({unit}, true, nullptr, &unit_err);
            if (rules.size() == 1)
            {
                rule_of_unit.emplace(unit, rules.front());
            }
            else
            {
                scan_err_of_unit.emplace(unit, std::move(unit_err));
            }
        }

        // classic 重放 argv 的剥离（第三个消费点）：模块消费 TU 记录命令里
        // 的 /reference /ifcSearchDir 等就地剥除（/reference 名单记账），
        // 扫描与重放共用同一份干净 argv。
        for (CompileCommand& cc : classic)
        {
            if (!db)
            {
                break; // gnu 记录命令不含 MSVC 开关——argv 逐字节不动
            }
            detail::MsvcModuleStrip strip =
                detail::strip_msvc_module_switches(cc.argv);
            if (strip.stripped)
            {
                cc.argv = std::move(strip.argv);
            }
            if (!strip.references.empty())
            {
                reference_modules.emplace(
                    cg::util::norm_path(cc.file),
                    std::move(strip.references));
            }
        }
        std::unordered_map<std::string, ScanRule> classic_rules;
        for (const CompileCommand& cc : classic)
        {
            if (!text_has_import(cc.file))
            {
                continue;
            }
            const std::vector<std::string>* flags = nullptr;
            const std::vector<std::string>  db_flags =
                db ? cc.argv : std::vector<std::string>{};
            if (db && !db_flags.empty())
            {
                flags = &db_flags;
            }
            const std::vector<ScanRule> rules =
                scan_batch({cg::util::norm_path(cc.file)}, false, flags);
            if (rules.size() == 1)
            {
                classic_rules.emplace(
                    cg::util::norm_path(cc.file),
                    rules.front());
            }
        }

        // 诊断面（--dry-run 扩展）：逐 TU 打印扫描规则（provides/requires）
        // 与实际发往扫描器的 argv——计划面与执行面用同一份 argv 组装逻辑，
        // 打印即取证。
        if (opt.dry_run)
        {
            const auto describe_flags =
                [&](const std::vector<std::string>& flags,
                    bool                            module_units_batch,
                    const std::string&              file)
            {
                std::string line =
                    scan_argv0.empty() ? std::string{info.driver} : scan_argv0;
                for (const std::string& a : flags)
                {
                    line += " " + a;
                }
                if (module_units_batch)
                {
                    line += " -x c++-module";
                }
                line += " " + file;
                return line;
            };
            for (const std::string& unit : scan_units)
            {
                const auto  rit = rule_of_unit.find(unit);
                std::string rule_text;
                if (rit != rule_of_unit.end())
                {
                    rule_text = " provides [";
                    for (const ScanProvide& p : rit->second.provides)
                    {
                        rule_text +=
                            p.name
                            + (p.is_interface ? std::string{"(interface)"}
                                              : std::string{"(partition)"})
                            + " ";
                    }
                    if (rule_text.back() == ' ')
                    {
                        rule_text.pop_back();
                    }
                    rule_text += "] requires [";
                    for (const ScanRequire& rq : rit->second.requires_names)
                    {
                        rule_text += rq.name + " ";
                    }
                    if (rule_text.back() == ' ')
                    {
                        rule_text.pop_back();
                    }
                    rule_text += "]";
                }
                else
                {
                    rule_text = " <no rule - degrades to tier-1>";
                }
                out.notes.push_back(
                    std::format(
                        "scan rule: {}{} | scan argv: {}",
                        unit,
                        rule_text,
                        describe_flags(unit_scan_flags(unit), true, unit)));
            }
            for (const CompileCommand& cc : classic)
            {
                const std::string norm = cg::util::norm_path(cc.file);
                const auto        rit = classic_rules.find(norm);
                if (rit == classic_rules.end())
                {
                    continue;
                }
                std::string rule_text = " requires [";
                for (const ScanRequire& rq : rit->second.requires_names)
                {
                    rule_text += rq.name + " ";
                }
                if (rule_text.back() == ' ')
                {
                    rule_text.pop_back();
                }
                rule_text += "]";
                out.notes.push_back(
                    std::format(
                        "scan rule: {}{} | scan argv: {}",
                        norm,
                        rule_text,
                        describe_flags(
                            db ? cc.argv : unit_scan_flags(norm),
                            false,
                            norm)));
            }
        }

        const StdModuleSource& std_source = std_source_resolved;
        /**
         * @brief 引擎驱动器自述三元组（-print-target-triple 首行；探询
         * 失败或空输出收敛为空串）。
         *
         * @internal 身份键的平台段与解析向量的 --target 同源于这一次探
         * 询：分开各探一次既多派生一个进程，也允许身份轴声称的体系与
         * 烤进 pcm 的 ABI 互漂。驱动器可随引擎在静止期切换，故按趟求
         * 值（与下方消费视图的缓存形态一致），不做进程级静态缓存。
         */
        const std::string driver_triple = [&]
        {
            const detail::ProcResult r =
                detail::run_tool(info.driver, {"-print-target-triple"});
            std::string triple =
                r.started && r.exit_code == 0
                    ? std::string{std::string_view{r.out}.substr(
                          0,
                          r.out.find('\n'))}
                    : std::string{};
            while (!triple.empty()
                   && (triple.back() == '\r' || triple.back() == ' '))
            {
                triple.pop_back();
            }
            return triple;
        }();
        // 身份键的平台段：原始三元组的消毒视图（目录名语境不允许其它
        // 字符，空探询收敛为 "unknown"）。
        const std::string platform = [&]
        {
            std::string safe;
            for (const char c : driver_triple)
            {
                const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                                || (c >= '0' && c <= '9') || c == '.'
                                || c == '_' || c == '-';
                safe += ok ? c : '_';
            }
            return safe.empty() ? std::string{"unknown"} : safe;
        }();
        const std::string std_hash =
            std_source.std_path.empty()
                ? std::string{"-"}
                : detail::hash_source(std_source.std_path);
        const std::string config_key = detail::hash_text(
            std::format(
                "mode={}\nconfig={}\nstd={}\ntriple={}\ntoolset={}\nstdlib={}:{}:{}\n"
                "wiring={}\ncompat={}\nlangopts={}",
                db ? "db" : "self",
                opt.db_config_label,
                opt.std_value,
                platform,
                std_source.toolset_id,
                std_source.stdlib_impl,
                std_source.std_path,
                std_hash,
                [&]
                {
                    std::string joined;
                    for (const std::string& w : std_source.wiring)
                    {
                        joined += w + ",";
                    }
                    return joined;
                }(),
                std_source.compat_path.empty()
                    ? std::string{"-"}
                    : std::format(
                          "{}:{}",
                          std_source.compat_path,
                          detail::hash_source(std_source.compat_path)),
                [&]
                {
                    if (langopt_axes.empty())
                    {
                        return std::string{"-"};
                    }
                    std::string joined;
                    for (const std::string& axis : langopt_axes)
                    {
                        joined += axis + ",";
                    }
                    return joined;
                }()));
        const std::string engine_key =
            detail::hash_text(engine_version).substr(0, 16);

        const std::string bmi_root =
            cg::util::norm_path((ccg_cfg / std::format("bmi-self-{}", platform))
                                    .lexically_normal()
                                    .generic_string());
        const std::string engine_dir =
            cg::util::norm_path((fs::path{bmi_root} / engine_key)
                                    .lexically_normal()
                                    .generic_string());
        const std::string config_dir =
            cg::util::norm_path((fs::path{engine_dir} / config_key)
                                    .lexically_normal()
                                    .generic_string());
        out.cache_dir = engine_dir;

        const auto create_dir = [](const std::string& dir)
        {
            std::error_code ec;
            if (fs::is_directory(fs::path{dir}, ec))
            {
                return;
            }
            fs::create_directories(fs::path{dir}, ec);
            cg::util::platform::enable_dir_compression(dir);
        };

        BmiManifest manifest;
        const auto  wipe_engine_dir = [&](const char* why)
        {
            std::error_code rm;
            for (const fs::directory_entry& e :
                 fs::directory_iterator(engine_dir, rm))
            {
                fs::remove_all(e.path(), rm);
            }
            out.purged += manifest.entries.size();
            out.notes.push_back(
                std::format("BMI cache wiped ({}) - full rebuild", why));
            manifest = BmiManifest{};
        };
        {
            std::optional<BmiManifest> previous;
            std::error_code            ec;
            const fs::path p = fs::path{engine_dir} / "manifest.json";
            if (fs::is_regular_file(p, ec))
            {
                if (const auto manifest_text =
                        cg::util::read_text_file(p.string()))
                {
                    previous = detail::manifest_from_json(*manifest_text);
                }
            }
            manifest = previous.value_or(BmiManifest{});
            if (previous.has_value() && previous->engine != engine_version
                && !opt.dry_run)
            {
                const std::string was = previous->engine;
                wipe_engine_dir("was built by a different engine");
                out.notes.back() += std::format(" (was '{}')", was);
                previous.reset();
            }
            if (!opt.dry_run)
            {
                std::error_code dex;
                if (fs::is_directory(fs::path{engine_dir}, dex))
                {
                    std::error_code sc;
                    for (const fs::directory_entry& e :
                         fs::directory_iterator(engine_dir, sc))
                    {
                        std::error_code   isc;
                        const std::string name =
                            e.path().filename().generic_string();
                        // 原子替换的唯一 tmp 也是常规文件：它不是旧扁平
                        // 布局的证据，并发写者更不能因此触发整目录清空。
                        if (e.is_regular_file(isc) && name != "manifest.json"
                            && !name.ends_with(".tmp"))
                        {
                            wipe_engine_dir(
                                "pre-rework flat-pcm cache layout detected");
                            break;
                        }
                    }
                }
            }
        }
        const auto entry_key =
            [](std::string_view name, std::string_view command)
        { return detail::bmi_entry_key(name, command); };
        std::unordered_map<std::string, BmiEntry> prior;
        for (BmiEntry& e : manifest.entries)
        {
            prior.emplace(entry_key(e.name, e.command), std::move(e));
        }
        manifest.entries.clear();
        manifest.engine = engine_version;
        manifest.engine_key = engine_key;
        manifest.config_key = config_key;
        manifest.toolset_id = std_source.toolset_id;

        /**
         * @internal 跨配置 std 捐赠（裁量 a）：布局改版后每个配置目录各有
         * 自己的引擎目录与台账，本清单 miss 的 std pcm 从兄弟配置目录扫描
         * 同一生产者产出（同源路径 + 同内容哈希 + 同命令摘要 = 同 pcm 字节）
         * 拷贝重定位，绝不共享句柄——33MB 级 std 不因布局改版退化为每配置
         * 重编。命中条件与清单内捐赠同等保守。
         */
        struct DonorPcm
        {
            std::string  path;
            std::int64_t size = 0;
        };
        const auto cross_config_donor =
            [&](std::string_view   module,
                const std::string& command,
                const std::string& source,
                const std::string& source_hash) -> DonorPcm
        {
            if (opt.ccg_config_dir.empty())
            {
                return {};
            }
            std::error_code lec;
            for (const fs::directory_entry& sibling :
                 fs::directory_iterator(fs::path{shared_ccg_root}, lec))
            {
                std::error_code sec;
                if (!sibling.is_directory(sec))
                {
                    continue;
                }
                std::error_code bec;
                for (const fs::directory_entry& bmi_dir :
                     fs::directory_iterator(sibling.path(), bec))
                {
                    std::error_code dec;
                    if (!bmi_dir.is_directory(dec))
                    {
                        continue;
                    }
                    if (!bmi_dir.path().filename().generic_string().starts_with(
                            "bmi-self-"))
                    {
                        continue;
                    }
                    const fs::path donor_engine = bmi_dir.path() / engine_key;
                    const fs::path donor_manifest =
                        donor_engine / "manifest.json";
                    std::error_code mec;
                    if (!fs::is_regular_file(donor_manifest, mec))
                    {
                        continue;
                    }
                    const auto donor_text =
                        cg::util::read_text_file(donor_manifest.string());
                    if (!donor_text.has_value())
                    {
                        continue;
                    }
                    const auto other = detail::manifest_from_json(*donor_text);
                    if (!other.has_value() || other->engine != engine_version)
                    {
                        continue;
                    }
                    for (const BmiEntry& e : other->entries)
                    {
                        if (e.name != module || !e.std_module
                            || e.command != command || e.source != source
                            || e.hash != source_hash || e.pcm.empty()
                            || e.pcm_size <= 0)
                        {
                            continue;
                        }
                        const fs::path  donor = donor_engine / fs::path{e.pcm};
                        std::error_code cec;
                        if (fs::is_regular_file(donor, cec)
                            && detail::artifact_size(
                                   donor.lexically_normal().generic_string())
                                   == e.pcm_size)
                        {
                            return DonorPcm{
                                donor.lexically_normal().generic_string(),
                                e.pcm_size};
                        }
                    }
                }
            }
            return {};
        };

        std::unordered_map<std::string, detail::BmiNode> nodes;
        std::unordered_map<std::string, std::string>     source_of_name;
        std::unordered_map<std::string, std::string>     ambiguous_of_name;

        for (const auto& [unit, rule] : rule_of_unit)
        {
            for (const ScanProvide& p : rule.provides)
            {
                if (!p.is_interface)
                {
                    continue; // 实现单元只消费、不提供
                }
                const std::string source =
                    p.source.empty() ? unit : cg::util::norm_path(p.source);
                const auto it = source_of_name.find(p.name);
                if (it != source_of_name.end() && it->second != source)
                {
                    ambiguous_of_name.emplace(
                        p.name,
                        std::format(
                            "self-build-unavailable: module '{}' is "
                            "provided by two files ('{}' and '{}') - "
                            "the module DAG is ambiguous",
                            p.name,
                            it->second,
                            source));
                    continue;
                }
                if (it != source_of_name.end())
                {
                    continue;
                }
                source_of_name.emplace(p.name, source);
                detail::BmiNode node;
                node.name = p.name;
                node.source = source;
                node.interface = true;
                for (const ScanRequire& rq : rule.requires_names)
                {
                    node.direct.push_back(rq.name);
                }
                std::ranges::sort(node.direct);
                auto [keep, last] = std::ranges::unique(node.direct);
                node.direct.erase(keep, last);
                nodes.emplace(p.name, std::move(node));
            }
        }

        std::vector<std::string> interface_names;
        interface_names.reserve(nodes.size());
        for (const auto& [name, node] : nodes)
        {
            interface_names.push_back(name);
        }
        std::ranges::sort(interface_names);
        const std::vector<std::vector<std::string>> layers =
            detail::topo_layers(nodes, interface_names);
        std::vector<std::string> order; // 展平层序（层间依赖序，层内名字序）
        for (const std::vector<std::string>& layer : layers)
        {
            order.insert(order.end(), layer.begin(), layer.end());
        }
        {
            const std::unordered_set<std::string> ordered{
                order.begin(),
                order.end()};
            for (const std::string& name : interface_names)
            {
                if (ordered.contains(name))
                {
                    continue;
                }
                const auto it = nodes.find(name);
                if (it != nodes.end() && it->second.failure.empty())
                {
                    it->second.failure = std::format(
                        "module-dag-cycle: module '{}' participates in (or "
                        "depends on) an import cycle - the interface BMI "
                        "cannot be produced; the TU degrades to tier-1",
                        name);
                }
            }
        }

        const auto std_pcm_path = [&](std::string_view module)
        {
            return cg::util::norm_path(
                (fs::path{config_dir} / std::format("{}.pcm", module))
                    .lexically_normal()
                    .generic_string());
        };
        bool        std_available = false;
        std::string std_failure;
        if (std_source.stdlib_impl == "msvc-stl" && !std_source.note.empty())
        {
            std_failure = std::format(
                "std-bmi-build-failed: {} - TUs importing std degrade to "
                "tier-1 (imports still recorded from the P1689 scan)",
                std_source.note);
            out.notes.push_back(std_failure);
        }
        else if (std_source.stdlib_impl.empty())
        {
            std_failure = std::format(
                "std-bmi-build-failed: {} - TUs importing std degrade to "
                "tier-1",
                std_source.note);
            out.notes.push_back(std_failure);
        }
        else if (opt.dry_run)
        {
            std_available = true;
        }
        else
        {
            create_dir(bmi_root);
            create_dir(engine_dir);
            create_dir(config_dir);
            // 姊妹引擎目录只在显式 --force/--full 且超过宽限期时回收：
            // 增量/update 绝不删除其它引擎（及其下的配置目录）的缓存。
            // 别的引擎的 pcm 本引擎从不读取，留下的代价只是磁盘；而并发
            // 的另一配置可能正靠它工作。
            if (opt.force)
            {
                std::error_code ec;
                for (const fs::directory_entry& e :
                     fs::directory_iterator(bmi_root, ec))
                {
                    std::error_code lec;
                    if (!e.is_directory(lec)
                        || e.path().filename().generic_string() == engine_key)
                    {
                        continue;
                    }
                    std::error_code tec;
                    const auto      mtime = fs::last_write_time(e.path(), tec);
                    if (tec
                        || fs::file_time_type::clock::now() - mtime
                               <= std::chrono::hours{1})
                    {
                        continue;
                    }
                    fs::remove_all(e.path(), ec);
                }
            }
            std::string std_error; // 首个失败 std 构建的细节
            const auto  build_std = [&](std::string_view   module,
                                        const std::string& source,
                                        const std::string& feed_std_pcm)
            {
                std::string pcm = std_pcm_path(module);
                // 参数向量不含引擎可执行路径：run_tool 的首参（进程可执
                // 行栏）已携带它，run_process 又把它拼成命令行首 token
                // （"exe" + 参数）——参数向量里再带一份，引擎 .exe 路径就
                // 会在命令行出现两遍，第二份是真实存在的文件，被 clang
                // 当作额外输入。std 路径的合成 argv 无 /TP，clang-cl 把
                // .exe 归为 linker input 只出警告，此前侥幸成功；这与接
                // 口路径（/TP 把它强制成 C++ 输入，双输入 + -clang:-o 即
                // cannot specify -o when generating multiple output
                // files，参数逃逸真凶）是同源的结构问题，一并根除。
                std::vector<std::string> argv;
                if (dialect_cl)
                {
                    argv.emplace_back("--driver-mode=cl");
                    argv.emplace_back("/EHsc");
                    argv.emplace_back(cl_std);
                    argv.emplace_back("-Wno-include-angled-in-module-purview");
                }
                else
                {
                    argv.emplace_back(std::format("-std={}", opt.std_value));
                    for (const std::string& axis : langopt_axes)
                    {
                        argv.push_back(axis);
                    }
                }
                argv.emplace_back("-Wno-reserved-module-identifier");
                if (!feed_std_pcm.empty())
                {
                    argv.emplace_back(
                        std::format("-fmodule-file=std={}", feed_std_pcm));
                }
                argv.emplace_back("-x");
                argv.emplace_back("c++-module");
                argv.emplace_back("--precompile-reduced-bmi");
                if (dialect_cl)
                {
                    for (const std::string& dir : std_source.wiring)
                    {
                        argv.emplace_back("-imsvc");
                        argv.push_back(dir);
                    }
                }
                else
                {
                    for (const std::string& w : std_source.wiring)
                    {
                        argv.push_back(w);
                    }
                }
                argv.push_back(source);
                std::string canonical;
                for (const std::string& a : argv)
                {
                    canonical += a + "\n";
                }
                const std::string command = detail::hash_text(canonical);
                const BmiEntry*   entry_prior = [&]
                {
                    const auto it = prior.find(
                        entry_key(module, std::string_view{command}));
                    return it == prior.end() ? nullptr : &it->second;
                }();
                const detail::SourceCheck check =
                    detail::check_source(source, entry_prior);
                if (!opt.force && check.matches_prior && entry_prior != nullptr
                    && entry_prior->command == command)
                {
                    const std::int64_t cached =
                        detail::cached_pcm_size(pcm, entry_prior->pcm_size);
                    if (cached > 0)
                    {
                        BmiEntry entry = *entry_prior;
                        entry.mtime = check.stat.mtime;
                        entry.size = check.stat.size;
                        entry.pcm = detail::pcm_rel(engine_dir, pcm);
                        entry.pcm_size = cached;
                        entry.config = opt.db_config_label;
                        manifest.entries.push_back(std::move(entry));
                        ++out.reused;
                        return pcm;
                    }
                    // 0 字节/半截/尺寸不符：同轮失效（丢缓存），先试同
                    // 清单的 donor，仍不可用则落回本地重建。
                    if (entry_prior != nullptr && !entry_prior->pcm.empty())
                    {
                        const fs::path donor =
                            fs::path{engine_dir} / fs::path{entry_prior->pcm};
                        if (detail::copy_donor_verified(
                                donor.lexically_normal().generic_string(),
                                pcm,
                                entry_prior->pcm_size))
                        {
                            BmiEntry entry = *entry_prior;
                            entry.mtime = check.stat.mtime;
                            entry.size = check.stat.size;
                            entry.pcm = detail::pcm_rel(engine_dir, pcm);
                            entry.pcm_size = entry_prior->pcm_size;
                            entry.config = opt.db_config_label;
                            manifest.entries.push_back(std::move(entry));
                            ++out.reused;
                            return pcm;
                        }
                    }
                }
                // 跨配置 std 捐赠：本配置清单 miss（首跑）时从兄弟配置目录
                // 拷贝同一生产者产出，30+ MB 的 std 接口不重编。donor 记录
                // 尺寸必须 >0，复制后目标尺寸必须与之一致。
                if (!opt.force)
                {
                    const DonorPcm donor =
                        cross_config_donor(module, command, source, check.hash);
                    if (!donor.path.empty()
                        && detail::copy_donor_verified(
                            donor.path,
                            pcm,
                            donor.size))
                    {
                        BmiEntry entry;
                        entry.name = std::string{module};
                        entry.source = source;
                        entry.hash = check.hash;
                        entry.mtime = check.stat.mtime;
                        entry.size = check.stat.size;
                        entry.command = command;
                        entry.pcm = detail::pcm_rel(engine_dir, pcm);
                        entry.pcm_size = donor.size;
                        entry.std_module = true;
                        entry.config = opt.db_config_label;
                        manifest.entries.push_back(std::move(entry));
                        ++out.reused;
                        return pcm;
                    }
                }
                // 产出先落唯一 tmp：exit 0 且 size>0 才原子替换正式名；
                // 失败删 tmp、旧 pcm 原样保留。
                const fs::path pcm_tmp_path =
                    cg::util::platform::unique_temp_path(fs::path{pcm});
                const std::string pcm_tmp = pcm_tmp_path.generic_string();
                if (dialect_cl)
                {
                    argv.emplace_back("-clang:-o");
                    argv.push_back(std::format("-clang:{}", pcm_tmp));
                }
                else
                {
                    argv.emplace_back("-o");
                    argv.push_back(pcm_tmp);
                }
                const detail::ProcResult r =
                    detail::run_tool(info.driver, argv);
                const std::int64_t produced =
                    r.started && r.exit_code == 0
                        ? detail::artifact_size(pcm_tmp)
                        : 0;
                if (produced > 0
                    && cg::util::platform::atomic_replace(
                        pcm_tmp_path,
                        fs::path{pcm}))
                {
                    BmiEntry entry;
                    entry.name = std::string{module};
                    entry.source = source;
                    entry.hash = check.hash;
                    entry.mtime = check.stat.mtime;
                    entry.size = check.stat.size;
                    entry.command = command;
                    entry.pcm = detail::pcm_rel(engine_dir, pcm);
                    entry.pcm_size = produced;
                    entry.std_module = true;
                    entry.config = opt.db_config_label;
                    manifest.entries.push_back(std::move(entry));
                    ++out.built;
                    return pcm;
                }
                cg::util::platform::remove_file(pcm_tmp_path);
                std_error =
                    produced == 0 && r.started && r.exit_code == 0
                        ? std::string{"output missing or empty"}
                        : std::format(
                              "exit {}",
                              r.exit_code == -1 && !r.started ? -1
                                                              : r.exit_code);
                if (!r.err.empty())
                {
                    std_error += "; " + detail::first_line_of(r.err);
                }
                else if (!r.out.empty())
                {
                    std_error += "; " + detail::first_line_of(r.out);
                }
                return std::string{};
            };
            const auto        t0 = std::chrono::steady_clock::now();
            const std::string std_pcm =
                build_std("std", std_source.std_path, "");
            std_available = !std_pcm.empty();
            if (std_available)
            {
                const double seconds =
                    std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
                const std::int64_t bytes = detail::artifact_size(std_pcm);
                out.notes.push_back(
                    std::format(
                        "std BMI {} in {:.1f}s ({} MB, stdlib {}, cache {})",
                        out.reused > 0 ? "reused" : "built",
                        seconds,
                        bytes / (1024 * 1024),
                        std_source.stdlib_impl,
                        engine_dir));
                if (!std_source.compat_path.empty())
                {
                    build_std("std.compat", std_source.compat_path, std_pcm);
                }
            }
            else
            {
                std_failure = std::format(
                    "std-bmi-build-failed: building the std module BMI from "
                    "'{}' failed ({}) - TUs importing std degrade to tier-1 "
                    "(imports still recorded from the P1689 scan)",
                    std_source.std_path,
                    std_error.empty() ? "no output" : std_error);
                out.notes.push_back(std_failure);
            }
        }

        std::unordered_map<std::string, std::string> std_node_failure;
        if (std_available)
        {
            detail::BmiNode node;
            node.name = "std";
            node.interface = false;
            node.pcm = std_pcm_path("std");
            node.available = true;
            nodes.emplace("std", node);
            detail::BmiNode compat;
            compat.name = "std.compat";
            compat.interface = false;
            compat.direct = {"std"};
            compat.pcm = std_pcm_path("std.compat");
            // 存在还不够：0 字节/半截文件是失败构建的残骸，投喂它只会让
            // 消费者以同样的错误失败。非空才算可用。
            compat.available =
                opt.dry_run || detail::artifact_size(compat.pcm) > 0;
            if (compat.available)
            {
                nodes.emplace("std.compat", compat);
            }
            else
            {
                std_node_failure.emplace(
                    "std.compat",
                    "std-bmi-build-failed: the std.compat BMI is absent - TUs "
                    "importing it degrade to tier-1");
            }
        }
        else
        {
            std_node_failure.emplace("std", std_failure);
            std_node_failure.emplace("std.compat", std_failure);
        }

        if (!opt.dry_run)
        {
            create_dir(config_dir);
        }
        std::unordered_map<std::string, std::vector<std::string>> closure_memo;
        // 闭包预算：closure_of 只读构建期不变的 direct 字段（扫描期定案），
        // 循环前按展平序一次算齐——此后含层内并行任务全程只读零锁。
        for (const std::string& name : order)
        {
            (void)detail::closure_of(name, nodes, closure_memo);
        }

        // 层前预备（主线程纯读组装，构建任务的输入）与结果槽（任务按下标
        // 独占写，屏障后主线程按名字序读——节点表与清单条目绝不被工人
        // 线程触碰，无锁协议）。
        struct LayerPrep
        {
            std::string              name;
            std::vector<std::string> argv;    // 组装产物（未含源与输出参数）
            std::string              command; // 规范生产命令哈希
            const BmiEntry*          prior_entry = nullptr; // 清单命中（可空）
            std::string              pcm;                   // 绝对产物路径
        };
        struct LayerOutcome
        {
            std::optional<BmiEntry> entry;   // 构建成功（产物已原子替换落盘）
            std::string             failure; // 失败文本（含异常收编形态）
        };
        // 串行对照门（测试与排障的逃生门，非产品面）：置 CCG_BMI_SERIAL 时
        // 层内任务改在主线程逐个执行——同一任务体、同一收尾顺序，产物与
        // 并行形态逐字节一致，供逐字节对照与并行无关缺陷的二分。
        const bool serial_bmi =
            cg::util::cstd::get_env("CCG_BMI_SERIAL").has_value();
        // 峰值工人数观测（汇总 note 披露）：各层开工数 = resolve_worker_count
        // (opt.jobs, 层宽)，全程峰值即统一并发上限在本段的实际生效面（窄层
        // 天然封顶：层宽 3 时开不出 8 路）。全复用/干跑/串行门下无池运行，
        // 恒为 1。
        int peak_workers = 1;

        for (const std::vector<std::string>& layer : layers)
        {
            // —— 层前主线程预检（纯读；屏障保证上游 failure/reusable 已定案，
            // 同层节点互不在彼此闭包内，批量预检与逐节点串行判定等价）——
            std::vector<LayerPrep> preps;
            for (const std::string& name : layer)
            {
                detail::BmiNode& node = nodes.at(name);
                if (!node.failure.empty())
                {
                    continue;
                }
                const std::vector<std::string>& closure =
                    detail::closure_of(name, nodes, closure_memo);
                bool        blocked = false;
                std::string blocker;
                for (const std::string& dep : closure)
                {
                    if (const auto a = ambiguous_of_name.find(dep);
                        a != ambiguous_of_name.end())
                    {
                        blocked = true;
                        blocker = a->second;
                        break;
                    }
                    if (const auto f = std_node_failure.find(dep);
                        f != std_node_failure.end())
                    {
                        blocked = true;
                        blocker = f->second;
                        break;
                    }
                    const auto dit = nodes.find(dep);
                    if (dit != nodes.end() && !dit->second.failure.empty())
                    {
                        blocked = true;
                        blocker = dit->second.failure;
                        break;
                    }
                }
                if (blocked)
                {
                    node.failure = blocker;
                    continue;
                }

                // 参数向量经 detail::bmi_replay_argv 组装：不含引擎可执行路径
                // （run_tool 首参已带；参数向量里再带一份就是参数逃逸真凶——
                // 命令行双份 .exe，/TP 把第二份强制成 C++ 输入，双输入 +
                // -clang:-o 报 cannot specify -o when generating multiple
                // output files）。argv 结构随本修复变化，canonical 命令哈希随
                // 之改变：旧清单条目一律 miss、BMI 一次性全量重建（预期行为）。
                const CompileCommand*          recorded = nullptr;
                const detail::MsvcModuleStrip* recorded_strip = nullptr;
                if (db)
                {
                    const auto rit = stripped_recorded.find(node.source);
                    if (rit != stripped_recorded.end())
                    {
                        recorded = &opt.recorded.find(node.source)->second;
                        recorded_strip = &rit->second;
                    }
                }
                std::vector<std::pair<std::string, std::string>> dep_feeds;
                for (const std::string& dep : closure)
                {
                    const auto dit = nodes.find(dep);
                    if (dit != nodes.end())
                    {
                        dep_feeds.emplace_back(dep, dit->second.pcm);
                    }
                }
                std::vector<std::string> argv = detail::bmi_replay_argv(
                    recorded_strip,
                    dialect_cl,
                    cl_std,
                    opt.std_value,
                    langopt_axes,
                    cl_base_argv(),
                    base_argv(),
                    dep_feeds);
                std::string canonical;
                for (const std::string& a : argv)
                {
                    canonical += a + "\n";
                }
                canonical += node.source + "\n";
                if (recorded != nullptr && !recorded->compiler.empty())
                {
                    canonical += recorded->compiler + "\n";
                }
                const std::string command = detail::hash_text(canonical);
                const std::string pcm = cg::util::norm_path(
                    (fs::path{config_dir}
                     / std::format("{}.pcm", detail::sanitize_pcm_name(name)))
                        .lexically_normal()
                        .generic_string());
                const BmiEntry* entry_prior = [&]
                {
                    const auto it =
                        prior.find(entry_key(name, std::string_view{command}));
                    return it == prior.end() ? nullptr : &it->second;
                }();
                const detail::SourceCheck check =
                    detail::check_source(node.source, entry_prior);
                if (!opt.force && check.matches_prior && entry_prior != nullptr
                    && entry_prior->command == command)
                {
                    bool deps_valid = true;
                    for (const std::string& dep : closure)
                    {
                        const auto dit = nodes.find(dep);
                        if (dit != nodes.end()
                            && dit->second.interface && !dit->second.reusable)
                        {
                            deps_valid = false;
                            break;
                        }
                    }
                    const std::int64_t cached =
                        detail::cached_pcm_size(pcm, entry_prior->pcm_size);
                    if (deps_valid && cached > 0)
                    {
                        node.reusable = true;
                        node.available = true;
                        node.pcm = pcm;
                        BmiEntry entry = *entry_prior;
                        entry.mtime = check.stat.mtime;
                        entry.size = check.stat.size;
                        entry.pcm_size = cached;
                        entry.requires_names = node.direct;
                        manifest.entries.push_back(std::move(entry));
                        ++out.reused;
                        continue;
                    }
                }
                if (opt.dry_run)
                {
                    node.available = true;
                    node.pcm = pcm;
                    continue;
                }
                LayerPrep prep;
                prep.name = name;
                prep.argv = std::move(argv);
                prep.command = command;
                prep.prior_entry = entry_prior;
                prep.pcm = pcm;
                preps.push_back(std::move(prep));
            }
            if (preps.empty())
            {
                continue;
            }

            // —— 层内构建：每任务一个 clang 子进程（run_tool 每调用独立
            // CreateProcess，进程级隔离）；源身份在任务体内、构建动作前一
            // 刻取——排队与调度延迟不拉长 stat 与编译器读源之间的观测窗口
            // （update 期间 ixx 被改的边缘处理锚点：身份与内容至多在毫秒
            // 级窗口内错位，下一轮 stat/hash 必不匹配而重建自愈）。——
            std::vector<LayerOutcome> outcomes(preps.size());
            const auto                run_layer_task = [&](std::size_t idx)
            {
                const LayerPrep& prep = preps[idx];
                LayerOutcome&    slot = outcomes[idx];
                try
                {
                    const detail::BmiNode&    node = nodes.at(prep.name);
                    const detail::SourceCheck check =
                        detail::check_source(node.source, prep.prior_entry);
                    std::vector<std::string> argv = prep.argv;
                    argv.push_back(node.source);
                    // 唯一 tmp → 校验 size>0 → 原子替换；失败保留旧 pcm。
                    const fs::path pcm_tmp_path =
                        cg::util::platform::unique_temp_path(
                            fs::path{prep.pcm});
                    const std::string pcm_tmp = pcm_tmp_path.generic_string();
                    if (dialect_cl)
                    {
                        argv.emplace_back("-clang:-o");
                        argv.push_back(std::format("-clang:{}", pcm_tmp));
                    }
                    else
                    {
                        argv.emplace_back("-o");
                        argv.push_back(pcm_tmp);
                    }
                    const detail::ProcResult r =
                        detail::run_tool(info.driver, argv);
                    const std::int64_t produced =
                        r.started && r.exit_code == 0
                            ? detail::artifact_size(pcm_tmp)
                            : 0;
                    if (produced > 0
                        && cg::util::platform::atomic_replace(
                            pcm_tmp_path,
                            fs::path{prep.pcm}))
                    {
                        BmiEntry entry;
                        entry.name = prep.name;
                        entry.source = node.source;
                        entry.hash = check.hash;
                        entry.mtime = check.stat.mtime;
                        entry.size = check.stat.size;
                        entry.command = prep.command;
                        entry.requires_names = node.direct;
                        entry.pcm = detail::pcm_rel(engine_dir, prep.pcm);
                        entry.pcm_size = produced;
                        entry.config = opt.db_config_label;
                        slot.entry = std::move(entry);
                        return;
                    }
                    cg::util::platform::remove_file(pcm_tmp_path);
                    const std::string why =
                        r.started && r.exit_code == 0
                            ? (produced == 0
                                   ? std::string{"output missing or empty"}
                                   : std::string{"atomic replace failed"})
                            : (r.started ? std::format("exit {}", r.exit_code)
                                         : std::string{"spawn failed"});
                    // 归属冲突（MSVC 宽容、clang 拒绝的生成头两路包含形状）
                    // 升格为专属类并携带工程侧修法；判定在全文 stderr 上做
                    // （该诊断按终端列宽折行，首行未必带齐锚），其余保持
                    // module-build-failed 口径。
                    slot.failure =
                        is_module_attachment_conflict(r.err)
                            ? module_attachment_conflict_failure(
                                  detail::first_line_of(r.err),
                                  prep.name)
                            : std::format(
                                  "module-build-failed: building the interface "
                                  "BMI for '{}' from '{}' failed ({}{}) - the "
                                  "TU and its dependents degrade to tier-1",
                                  prep.name,
                                  node.source,
                                  why,
                                  r.err.empty()
                                      ? std::string{}
                                      : "; " + detail::first_line_of(r.err));
                }
                catch (const std::exception& e)
                {
                    // 工人线程里未捕获的 C++ 异常会终止整个进程：收编为该
                    // 节点的失败记名（同层其余任务照常完成），与解析池任务
                    // 体的异常纪律同款。
                    slot.failure = std::format(
                        "module-build-failed: building the interface BMI for "
                        "'{}' failed (internal error: {}) - the TU and its "
                        "dependents degrade to tier-1",
                        prep.name,
                        e.what());
                }
                catch (...)
                {
                    slot.failure = std::format(
                        "module-build-failed: building the interface BMI for "
                        "'{}' failed (internal error: unknown) - the TU and "
                        "its dependents degrade to tier-1",
                        prep.name);
                }
            };
            if (serial_bmi)
            {
                for (std::size_t i = 0; i < preps.size(); ++i)
                {
                    run_layer_task(i);
                }
            }
            else
            {
                cg::util::pool::PoolRun pool_spec;
                pool_spec.count = preps.size();
                pool_spec.requested = cg::util::pool::resolve_worker_count(
                    opt.jobs,
                    preps.size());
                peak_workers = std::max(peak_workers, pool_spec.requested);
                pool_spec.task = [&](std::size_t i, int /*worker_id*/)
                { run_layer_task(i); };
                cg::util::pool::run(std::move(pool_spec));
            } // 池 join 点即层屏障：本层产物全部落盘、结果槽全部写满

            // —— 层收尾（主线程，按层内名字序写回）：下一层此刻才开 ——
            for (std::size_t i = 0; i < preps.size(); ++i)
            {
                detail::BmiNode& node = nodes.at(preps[i].name);
                LayerOutcome&    outcome = outcomes[i];
                if (outcome.entry.has_value())
                {
                    node.available = true;
                    node.reusable = false;
                    node.pcm = preps[i].pcm;
                    manifest.entries.push_back(std::move(*outcome.entry));
                    ++out.built;
                }
                else
                {
                    node.failure = std::move(outcome.failure);
                }
            }
        }
        for (const std::string& name : order)
        {
            detail::BmiNode& node = nodes.at(name);
            if (node.failure.empty() && !node.available)
            {
                node.failure = std::format(
                    "module-build-failed: a dependency of module '{}' did not "
                    "build",
                    name);
            }
        }

        const bool recipe_b =
            dialect_cl
            && cg::util::cstd::get_env("CCG_MSVC_GNU_VECTOR").has_value();
        /**
         * @brief 配方 A/B 的 --target 三元组：引擎驱动器自述；探询失败
         * 回落写死的 x64 msvc 值并注记（绝不静默）。
         *
         * @internal 原为写死 x86_64-pc-windows-msvc——arm64 主机上它会把
         * 全部 TU 按 x64 ABI 烤进 BMI 身份；自述随主机体系走。x64 主机
         * 上探询输出与写死值逐字节相同（零行为变化）；体系不同处
         * --target 变化使生产命令摘要失配、pcm 一次性重建属预期。
         */
        const std::string cl_target = [&]
        {
            if (!driver_triple.empty())
            {
                return driver_triple;
            }
            out.notes.push_back(
                "target-triple probe failed - the cl parse vector target "
                "falls back to the pinned x86_64-pc-windows-msvc triple");
            return std::string{"x86_64-pc-windows-msvc"};
        }();
        const auto cl_parse_argv = [&]
        {
            std::vector<std::string> argv;
            if (!recipe_b)
            {
                argv = {
                    "--driver-mode=cl",
                    "/EHsc",
                    cl_std,
                    "-Xclang",
                    "-fexternc-nounwind",
                    "-fstack-protector-strong",
                    "-fms-compatibility",
                    std::format("--target={}", cl_target)};
                for (const std::string& dir : std_source.wiring)
                {
                    argv.emplace_back("-imsvc");
                    argv.push_back(dir);
                }
                for (const std::string& define : opt.cl_defines)
                {
                    argv.emplace_back("/D");
                    argv.push_back(define);
                }
            }
            else
            {
                argv = {
                    std::format("-std={}", gnu_twin_std),
                    std::format("--target={}", cl_target),
                    "-fms-compatibility",
                    "-Xclang",
                    "-fexternc-nounwind",
                    "-fstack-protector-strong"};
                for (const std::string& dir : std_source.wiring)
                {
                    argv.emplace_back("-isystem");
                    argv.push_back(dir);
                }
                for (const std::string& define : opt.cl_defines)
                {
                    argv.emplace_back("-D");
                    argv.push_back(define);
                }
            }
            if (!opt.root.empty())
            {
                argv.emplace_back("-I");
                argv.push_back(opt.root);
            }
            return argv;
        };
        const std::vector<std::string> toolset_roots =
            dialect_cl ? detail::stl_include_slice(std_source.wiring)
                       : std::vector<std::string>{};
        const auto apply_degrade_rules =
            [&](CompileCommand&                 cc,
                const std::vector<ScanRequire>& reqs,
                std::vector<ModuleFeed>&        feeds)
        {
            const auto add_unique =
                [](std::vector<std::string>& v, const std::string& s)
            { cg::util::push_unique(v, s); };
            for (const ScanRequire& rq : reqs)
            {
                if (detail::looks_like_header_unit(rq.name))
                {
                    cc.overlay.header_unit_includes = true;
                    std::string name = rq.name;
                    if (name.size() >= 2
                        && ((name.front() == '"' && name.back() == '"')
                            || (name.front() == '<' && name.back() == '>')))
                    {
                        name = name.substr(1, name.size() - 2);
                    }
                    cc.overlay_imports.push_back(std::move(name));
                }
            }
            for (const std::string& name :
                 detail::text_header_unit_imports(cc.file))
            {
                cc.overlay.header_unit_includes = true;
                add_unique(cc.overlay_imports, name);
            }
            // 伞头前提门（requires_std）：P1689 规则确实牵涉 std 才允许
            // overlay；文本 STL 包含（stdafx 形态）单独绝不触发。
            const bool needs_std =
                detail::requires_std(reqs, nodes, closure_memo);
            if (dialect_cl && needs_std
                && detail::text_has_stl_include(cc.file, toolset_roots))
            {
                cc.overlay.std_umbrella = true;
                for (const ScanRequire& rq : reqs)
                {
                    if (rq.name == "std" || rq.name == "std.compat")
                    {
                        add_unique(cc.overlay_imports, rq.name);
                    }
                }
                std::erase_if(
                    feeds,
                    [&](const ModuleFeed& feed)
                    {
                        const bool is_std =
                            feed.name == "std" || feed.name == "std.compat";
                        const std::vector<std::string> closure =
                            detail::closure_of(feed.name, nodes, closure_memo);
                        const bool std_bound =
                            is_std
                            || std::ranges::find(closure, "std")
                                   != closure.end()
                            || std::ranges::find(closure, "std.compat")
                                   != closure.end();
                        if (std_bound
                            && !detail::looks_like_header_unit(feed.name)
                            && std::ranges::any_of(
                                reqs,
                                [&](const ScanRequire& rq)
                                { return rq.name == feed.name; }))
                        {
                            add_unique(cc.overlay.dropped_modules, feed.name);
                            add_unique(cc.overlay_imports, feed.name);
                        }
                        return std_bound;
                    });
            }
        };

        const auto feed_flags_for =
            [&](const std::vector<std::string>& names,
                std::vector<ModuleFeed>&        feeds,
                const std::vector<std::string>* reference_names =
                    nullptr) -> std::optional<std::string>
        {
            std::map<std::string, std::string> resolved; // 有序、去重
            for (const std::string& direct : names)
            {
                if (const auto a = ambiguous_of_name.find(direct);
                    a != ambiguous_of_name.end())
                {
                    return a->second;
                }
                std::vector<std::string> closure =
                    detail::closure_of(direct, nodes, closure_memo);
                closure.push_back(direct);
                for (const std::string& dep : closure)
                {
                    const auto dit = nodes.find(dep);
                    if (dit != nodes.end() && dit->second.available
                        && !dit->second.pcm.empty())
                    {
                        resolved.emplace(dep, dit->second.pcm);
                        continue;
                    }
                    if (const auto f = std_node_failure.find(dep);
                        f != std_node_failure.end())
                    {
                        return f->second;
                    }
                    if (dit != nodes.end() && !dit->second.failure.empty())
                    {
                        return dit->second.failure;
                    }
                    return std::format(
                        "module-not-found: no provider under the roots for "
                        "module '{}' ({}) - the TU degrades to tier-1",
                        dep,
                        reference_names != nullptr
                                && std::ranges::find(*reference_names, dep)
                                       != reference_names->end()
                            ? "the build's /reference names it, but no module "
                              "source under the roots provides it"
                            : "self-build scan");
                }
            }
            for (const auto& [dep, pcm] : resolved)
            {
                feeds.push_back(ModuleFeed{.name = dep, .pcm = pcm});
            }
            return std::nullopt;
        };

        for (const std::string& unit : module_units)
        {
            const std::string norm = cg::util::norm_path(unit);
            CompileCommand    cc;
            cc.compiler = info.driver;
            cc.directory = opt.root;
            cc.file = norm;
            if (db)
            {
                const auto rec = opt.recorded.find(norm);
                const auto strip = stripped_recorded.find(norm);
                if (rec != opt.recorded.end()
                    && strip != stripped_recorded.end())
                {
                    cc.argv = strip->second.argv;
                    cc.directory = rec->second.directory;
                    /**
                     * @brief PCH 语义保真搬运：记录条目的消费者字段随记录
                     * 权威进入合成命令。
                     *
                     * @internal 装载层把 /Yu /Fp 与 -include-pch 全部落进
                     * 结构化字段（argv 不留旗标），路由前的记录条目与普通
                     * TU 完全同权；合成若只搬 argv，这些字段就静默死亡，
                     * 模块单元的解析世界比真实构建少整个预编译头（宏与
                     * 声明）。搬运后该条目进入 PCH 规划的消费者循环：生产
                     * 者在场则影子注入（注入的三元组与 -x c++-module/
                     * -fmodule-file 末尾追加形态可共存），生产者缺失或影子
                     * 失败则文本回退——合成 argv 本无 PCH 旗标，剥离是恒
                     * 等操作，等价于搬运前的现状，无新增失败面。刻意不搬
                     * pch_create：模块单元充当 PCH 生产者是构建侧编不过的
                     * 病态语料，搬运会把合成命令错送生产者分支。
                     */
                    cc.pch_use = rec->second.pch_use;
                    cc.pch_path = rec->second.pch_path;
                    cc.pch_header = rec->second.pch_header;
                    cc.pch_involved = rec->second.pch_involved;
                    if (std::ranges::find_if(
                            cc.argv,
                            [](const std::string& a)
                            {
                                return a.starts_with("-std=")
                                       || a.starts_with("/std:");
                            })
                        == cc.argv.end())
                    {
                        cc.argv.emplace_back(
                            dialect_cl ? cl_std
                                       : std::format("-std={}", opt.std_value));
                    }
                }
                else if (dialect_cl)
                {
                    cc.argv = cl_parse_argv(); // 无记录权威的 cl 单元：合成兜底
                }
                else
                {
                    cc.argv = base_argv();
                    for (const std::string& axis : langopt_axes)
                    {
                        cc.argv.push_back(axis);
                    }
                }
            }
            else
            {
                cc.argv = dialect_cl ? cl_parse_argv() : base_argv();
            }
            const auto rit = rule_of_unit.find(norm);
            if (rit == rule_of_unit.end())
            {
                // 诊断面：降级归因附带扫描器 stderr 首行——单 TU 重扫的
                // 报错优先，缺席时退回初始整批的首行（err 已在 ProcResult
                // 捕获，此前被整体丢弃）。
                std::string scanner_note;
                if (const auto eit = scan_err_of_unit.find(norm);
                    eit != scan_err_of_unit.end() && !eit->second.empty())
                {
                    scanner_note = std::format(" (scanner: {})", eit->second);
                }
                else if (!batch_scan_err.empty())
                {
                    scanner_note =
                        std::format(" (scanner: {})", batch_scan_err);
                }
                cc.preordained_failure = std::format(
                    "self-build-unavailable: the P1689 scan produced no rule "
                    "for '{}' (syntax error, unreadable, or not a module "
                    "unit){} - the TU degrades to tier-1",
                    norm,
                    scanner_note);
                ++out.degraded;
                out.commands.emplace_back(std::move(cc));
                continue;
            }
            for (const ScanRequire& rq : rit->second.requires_names)
            {
                cc.scan_imports.push_back(rq.name);
            }
            // BMI 级失败的重试观察集：直接与传递 requires 里每个接口依赖的
            // 源路径（std 伪节点 source 为空、自然跳过）。P1689 伪预处理看
            // 不见文本 include，头文件链由解析侧的诊断定位另行补充；此处
            // 收集的是模块级依赖源，失败记录把它们压进 deps_json。
            std::vector<std::string> failure_dep_paths;
            {
                std::unordered_set<std::string> dep_names;
                for (const ScanRequire& rq : rit->second.requires_names)
                {
                    dep_names.insert(rq.name);
                    for (const std::string& deeper :
                         detail::closure_of(rq.name, nodes, closure_memo))
                    {
                        dep_names.insert(deeper);
                    }
                }
                for (const std::string& dep : dep_names)
                {
                    const auto dit = nodes.find(dep);
                    if (dit != nodes.end() && !dit->second.source.empty())
                    {
                        failure_dep_paths.push_back(dit->second.source);
                    }
                }
                std::ranges::sort(failure_dep_paths);
                auto [keep, last] = std::ranges::unique(failure_dep_paths);
                failure_dep_paths.erase(keep, last);
            }
            std::vector<std::string> direct_names;
            for (const ScanRequire& rq : rit->second.requires_names)
            {
                if (!detail::looks_like_header_unit(rq.name))
                {
                    direct_names.push_back(rq.name);
                }
            }
            // /reference 名单 ∪ 扫描器结果：构建系统声明的消费模块兜底扫
            // 描漏检（宏展开形态的 import 对 P1689 伪预处理不可见）——注入
            // 路径永远用影子 pcm，绝不用 .ifc 路径。
            const std::vector<std::string>* unit_references = nullptr;
            if (const auto lit = reference_modules.find(norm);
                lit != reference_modules.end())
            {
                unit_references = &lit->second;
                for (const std::string& name : lit->second)
                {
                    if (!detail::looks_like_header_unit(name)
                        && std::ranges::find(direct_names, name)
                               == direct_names.end())
                    {
                        direct_names.push_back(name);
                    }
                }
            }
            std::vector<ModuleFeed> feeds;
            if (auto blocked =
                    feed_flags_for(direct_names, feeds, unit_references))
            {
                cc.preordained_failure = *blocked;
                cc.bmi_failure_deps = std::move(failure_dep_paths);
                ++out.degraded;
                out.commands.emplace_back(std::move(cc));
                continue;
            }
            apply_degrade_rules(cc, rit->second.requires_names, feeds);
            const bool interface_unit = std::ranges::any_of(
                rit->second.provides,
                [](const ScanProvide& p) { return p.is_interface; });
            const bool header_unit_only =
                cc.overlay.header_unit_includes && direct_names.empty();
            if (header_unit_only)
            {
                cc.overlay.demodule = true;
            }
            if (header_unit_only)
            {
                cc.argv.emplace_back("-x");
                cc.argv.emplace_back("c++");
            }
            else if (interface_unit)
            {
                bool has_module_lang = false;
                for (std::size_t i = 0; i + 1 < cc.argv.size(); ++i)
                {
                    if (cc.argv[i] == "-x" && cc.argv[i + 1] == "c++-module")
                    {
                        has_module_lang = true;
                        break;
                    }
                }
                if (!has_module_lang)
                {
                    cc.argv.emplace_back("-x");
                    cc.argv.emplace_back("c++-module");
                }
                for (const ScanProvide& p : rit->second.provides)
                {
                    if (p.is_interface)
                    {
                        const auto nit = nodes.find(p.name);
                        if (nit != nodes.end() && !nit->second.pcm.empty())
                        {
                            cc.module_output = nit->second.pcm;
                        }
                        break;
                    }
                }
            }
            // 自身接口的 BMI 生产失败（构建失败/环/被依赖阻塞）：本命令仍会
            // 进入解析，但其失败记录必须携带依赖闭包——自身源已有 stat 与
            // 内容哈希盯守，这里补给模块级依赖源，头文件链由解析侧的诊断
            // 定位补充。
            for (const ScanProvide& p : rit->second.provides)
            {
                const auto nit = nodes.find(p.name);
                if (nit != nodes.end() && !nit->second.failure.empty())
                {
                    cc.bmi_failure_deps = std::move(failure_dep_paths);
                    break;
                }
            }
            if (!header_unit_only)
            {
                // 相邻发现配套项：文本强制包含先于 module 声明注入头内容，
                // 对按模块语义解析的单元是硬错误——显式降级（剥除）并逐文
                // 件记名，绝不静默。头单元形态（demodule，按普通 C++ 解析）
                // 的强制包含合法，不剥。PCH 介入的条目经生产者闭包仍以影
                // 子形态拿到强制包含内容。
                detail::ForcedIncludeStrip fis =
                    detail::strip_module_text_forced_includes(cc.argv);
                if (!fis.removed.empty())
                {
                    cc.argv = std::move(fis.argv);
                    out.notes.emplace_back(
                        std::format(
                            "{}: {} forced include{} stripped from the module "
                            "unit's replay argv - a text include cannot precede "
                            "the module declaration (a hard error under the "
                            "module rules; in the replay the tokens carry no "
                            "usable semantics either); the PCH channel still "
                            "delivers such content through the shadow closure "
                            "when a producer is recorded",
                            norm,
                            fis.removed.size(),
                            fis.removed.size() == 1 ? "" : "s"));
                }
            }
            for (const ModuleFeed& feed : feeds)
            {
                cc.argv.emplace_back(
                    std::format("-fmodule-file={}={}", feed.name, feed.pcm));
                cc.module_feeds.push_back(feed);
            }
            out.commands.emplace_back(std::move(cc));
        }

        for (CompileCommand& cc : classic)
        {
            const std::string classic_norm = cg::util::norm_path(cc.file);
            const auto        rit = classic_rules.find(classic_norm);
            const auto        lit = reference_modules.find(classic_norm);
            if (rit == classic_rules.end() && lit == reference_modules.end())
            {
                continue; // 无扫描规则也无 /reference 名单：不是模块消费者
            }
            const std::vector<ScanRequire>  no_requires;
            const std::vector<ScanRequire>& requires_names =
                rit != classic_rules.end() ? rit->second.requires_names
                                           : no_requires;
            for (const ScanRequire& rq : requires_names)
            {
                cc.scan_imports.push_back(rq.name);
            }
            std::vector<std::string> direct_names;
            for (const ScanRequire& rq : requires_names)
            {
                if (!detail::looks_like_header_unit(rq.name))
                {
                    direct_names.push_back(rq.name);
                }
            }
            // /reference 名单 ∪ 扫描器结果：扫描器完全漏检（宏展开形态的
            // import）时经典条目仍按构建声明获得投喂。
            const std::vector<std::string>* classic_references = nullptr;
            if (lit != reference_modules.end())
            {
                classic_references = &lit->second;
                for (const std::string& name : lit->second)
                {
                    if (!detail::looks_like_header_unit(name)
                        && std::ranges::find(direct_names, name)
                               == direct_names.end())
                    {
                        direct_names.push_back(name);
                    }
                }
            }
            std::vector<ModuleFeed> feeds;
            if (auto blocked =
                    feed_flags_for(direct_names, feeds, classic_references))
            {
                cc.preordained_failure = *blocked;
                ++out.degraded;
                continue;
            }
            apply_degrade_rules(cc, requires_names, feeds);
            for (const ModuleFeed& feed : feeds)
            {
                cc.argv.emplace_back(
                    std::format("-fmodule-file={}={}", feed.name, feed.pcm));
                cc.module_feeds.push_back(feed);
            }
        }

        if (!opt.dry_run)
        {
            const fs::path manifest_path{
                fs::path{engine_dir} / "manifest.json"};
            // 宽限期：只有显式 --force/--full 且产物 mtime 超过 1 小时才
            // 允许删除。增量/update 从不删除任何缓存产物。
            constexpr auto grace = std::chrono::hours{1};
            const auto     older_than_grace = [&](const fs::path& p)
            {
                std::error_code ec;
                const auto      mtime = fs::last_write_time(p, ec);
                if (ec)
                {
                    return false; // 年龄未知：保守保留
                }
                return fs::file_time_type::clock::now() - mtime > grace;
            };
            const auto artifact_of = [&](const BmiEntry& e)
            { return fs::path{engine_dir} / fs::path{e.pcm}; };
            // pcm 拼写为 "`<config_key>`/`<module>`.pcm"；首段不是本趟配置键
            // 即兄弟配置的产物——即便 --force/--full 也不回收。
            const auto other_config = [&](const BmiEntry& e)
            {
                const std::size_t slash = e.pcm.find('/');
                return slash != std::string::npos
                       && e.pcm.substr(0, slash) != config_key;
            };

            std::unordered_set<std::string> live;
            for (const BmiEntry& e : manifest.entries)
            {
                live.insert(entry_key(e.name, e.command));
            }
            std::unordered_set<std::string> dropped;
            for (auto& [key, entry] : prior)
            {
                if (live.contains(key))
                {
                    continue; // 本趟已写出的版本
                }
                // 默认结转所有未参与本趟的条目（含兄弟配置与历史条目）：
                // 增量运行绝不删除其它配置目录，且旧条目仍是引用集的一
                // 部分，不会被清扫当作孤儿。
                if (opt.force && !other_config(entry)
                    && older_than_grace(artifact_of(entry)))
                {
                    dropped.insert(key);
                    ++out.purged;
                    continue;
                }
                manifest.entries.push_back(entry);
                live.insert(key);
            }
            // 乐观合并：构建期间另一个进程可能已原子替换了清单。重读并
            // 并入本进程不知道的条目（绝不复活本趟显式丢弃的键），使
            // 2/3 并发写者不丢彼此的配置条目。最终一次原子替换保证读者
            // 永远看到合法 JSON。
            {
                std::error_code sec;
                if (fs::is_regular_file(manifest_path, sec))
                {
                    if (const auto disk_text =
                            cg::util::read_text_file(manifest_path.string()))
                    {
                        if (const auto disk =
                                detail::manifest_from_json(*disk_text);
                            disk.has_value() && disk->engine == manifest.engine)
                        {
                            // 本趟已写出的条目不在 skip 集里——并集按身份
                            // 键去重；只有显式丢弃的键被排除。
                            std::unordered_set<std::string> skip;
                            for (const std::string& key : dropped)
                            {
                                skip.insert(key);
                            }
                            (void)detail::merge_manifest_entries(
                                manifest,
                                *disk,
                                skip);
                        }
                    }
                }
            }
            // 清单条目按 (name, command) 全序排序后落盘：层内并行只改变任
            // 务完成序，不改变条目集合——排序使清单跨并行度、跨运行形态
            // 逐字节稳定（条目查找全部走身份键，本无顺序语义；结转与乐观
            // 合并段同样参与排序，多次运行幂等）。
            std::ranges::sort(
                manifest.entries,
                [](const BmiEntry& a, const BmiEntry& b)
                {
                    if (a.name != b.name)
                    {
                        return a.name < b.name;
                    }
                    return a.command < b.command;
                });
            if (!manifest.entries.empty() || fs::exists(manifest_path))
            {
                const std::string text = detail::manifest_to_json(manifest);
                if (!cg::util::platform::atomic_write_file(manifest_path, text))
                {
                    out.notes.push_back(
                        "BMI manifest write failed (the previous manifest was "
                        "kept; this run's cache entries are not recorded)");
                }
            }
            // 孤儿清扫：只允许显式 --force/--full；目标必须超过 1 小时
            // 宽限，且不在重新读取的完整清单（含兄弟配置）引用集中。
            // 清单读不回时整段跳过——没有完整引用集就无法证明任何文件
            // 是孤儿，宁可留盘也不误删别的配置。
            std::unordered_set<std::string> referenced;
            bool                            have_disk_manifest = false;
            {
                const auto reference = [&](const BmiEntry& e)
                {
                    referenced.insert(
                        cg::util::norm_path(artifact_of(e)
                                                .lexically_normal()
                                                .generic_string()));
                };
                for (const BmiEntry& e : manifest.entries)
                {
                    reference(e);
                }
                // 重新读取落盘的完整清单（并发写者可能刚并入新条目）。
                std::error_code sec;
                if (fs::is_regular_file(manifest_path, sec))
                {
                    if (const auto disk_text =
                            cg::util::read_text_file(manifest_path.string()))
                    {
                        if (const auto disk =
                                detail::manifest_from_json(*disk_text);
                            disk.has_value() && disk->engine == manifest.engine)
                        {
                            for (const BmiEntry& e : disk->entries)
                            {
                                reference(e);
                            }
                            have_disk_manifest = true;
                        }
                    }
                }
            }
            if (opt.force && have_disk_manifest)
            {
                std::vector<fs::path> dirs;
                std::error_code       rec;
                for (const fs::directory_entry& e :
                     fs::recursive_directory_iterator(engine_dir, rec))
                {
                    std::error_code fec;
                    if (e.is_regular_file(fec))
                    {
                        const std::string name =
                            e.path().filename().generic_string();
                        if (name == "manifest.json" || name.ends_with(".tmp"))
                        {
                            continue;
                        }
                        const std::string abs = cg::util::norm_path(
                            e.path().lexically_normal().generic_string());
                        if (referenced.contains(abs)
                            || !older_than_grace(e.path()))
                        {
                            continue;
                        }
                        std::error_code rm;
                        fs::remove(e.path(), rm);
                    }
                    else if (e.is_directory(fec))
                    {
                        dirs.push_back(e.path());
                    }
                }
                std::ranges::sort(
                    dirs,
                    [](const fs::path& a, const fs::path& b)
                    { return a.native().size() > b.native().size(); });
                for (const fs::path& dir : dirs)
                {
                    std::error_code dec;
                    if (!fs::is_empty(dir, dec) || dec
                        || !older_than_grace(dir))
                    {
                        continue;
                    }
                    std::error_code rm;
                    fs::remove(dir, rm);
                }
            }
        }

        out.notes.push_back(
            std::format(
                "module BMIs{}: {} built, {} reused, {} purged, {} TU(s) degraded "
                "to tier-1 (stdlib {}, cache {}, {} layer{} on up to {} worker{}){}",
                db ? std::format(
                         " (compile-db route{}, recorded argv authority)",
                         opt.db_config_label.empty() ? std::string{}
                                                     : std::format(
                                                           ", config '{}'",
                                                           opt.db_config_label))
                   : std::string{},
                out.built,
                out.reused,
                out.purged,
                out.degraded,
                std_source.stdlib_impl.empty() ? "<none>"
                                               : std_source.stdlib_impl,
                bmi_root,
                layers.size(),
                layers.size() == 1 ? "" : "s",
                peak_workers,
                peak_workers == 1 ? "" : "s",
                // 干跑不评估 prior manifest 的复用（纯读盘+哈希也不做），
                // reused 恒为计划值 0——如实注明，防止把干跑计数当真实吞吐。
                opt.dry_run
                    ? std::
                          string{" [dry run: built/reused are planning figures; "
                                 "prior-manifest reuse is not evaluated]"}
                    : std::string{}));
        return out;
    }

} // namespace cg::clang
