

/**
 * @module cg.clang.compile_db.model
 *
 * @brief 编译命令域的导出类型：模块喂给/覆盖、PCH 使用、CompileCommand 与模块
 * TU 判定。
 */
export module cg.clang.compile_db.model;

import std;
import cg.util;
import cg.util.cstd;
import cg.clang.driver;

export namespace cg::clang
{

    /**
     * @brief 条目重放携带的一条命名模块 BMI 投喂。
     *
     * 在 `modmap` 响应文件（或裸命令行）展开时以结构化形式捕获：模块名
     * 加绝对 pcm 路径。
     */
    struct ModuleFeed
    {
        std::string name; // 被导入的模块名（"std"、"cg.util.json"）
        std::string pcm;  // 绝对、规范化 BMI 路径
    };

    /**
     * @brief 解析期模块覆盖旋钮。
     *
     * @internal
     * MSVC 地混合 TU（import std + 文本 STL include）绝不可投喂 std.pcm：
     * pcm 导出的 std:: 模板与文本包含的 STL 定义同时物化并冲突
     * （'is_same_v' 重定义之类的 22 条诊断风暴、rc=0）——此类 TU 按规则
     * 以工具链伞头文件替换 `import std;` 行解析，并丢弃传递闭包含 std 的
     * 一切投喂。头单元降级为文本 include（头单元的
     * name=key 投喂形式总是失败"not known to be a header unit"，唯一可行
     * 的 path-only 急切形式仍属实验性——v1 策略降级）。
     */
    struct ModuleOverlay
    {
        /**
         * @brief 把 `import std;` / `import std.compat;` 行替换为
         * `#include` `<__msvc_all_public_headers.hpp>`（单行——保留书写行号；
         * 工具链 include 目录经 -imsvc 已在解析 include 路径上）。
         */
        bool std_umbrella = false;
        /** @brief 把 `import "hdr";` / `import <hdr>;` 改写为对应的
         *  `#include`。 */
        bool header_unit_includes = false;
        /**
         * @brief 把头单元 import 无法活在 purview 内（未书写全局模块
         *  片段）的接口单元降级为经典 TU：模块声明同宽消隐、解析去掉
         *  -x c++-module。行列保持精确。
         */
        bool demodule = false;
        /**
         * @brief 伞头降级下被消隐 import 行的命名模块。
         *
         * 其投喂已被抹除（投喂会传递加载 std.pcm、撞上文本 STL 头），
         * 留下无映射的书写 import 会让本该存活的解析硬失败。import 关系
         * 经 overlay_imports 的行 0 行存活。
         */
        std::vector<std::string> dropped_modules;
    };

    /**
     * @brief 一条条目的 PCH 消费/回退判定（由 cg.clang.pch 的规划器填写）。
     *
     * @internal 无 PCH 标记的条目保持 `none` 且 identity 为空——parse_digest
     * 不追加任何后缀，保证不用 PCH 的工程与 PCH 支持落地前逐字节一致。
     */
    struct PchUse
    {
        enum class Status : std::uint8_t
        {
            none,
            shadow,          // 消费本引擎构建的 DPR 影子 PCH
            text_fallback,   // PCH 旗标已剥除，走纯文本解析
            replay_degraded, // 显式 opt-in：消费记录 PCH、关闭 DPR（图不完整）
            unusable,        // 文本回退也不可行（仅诊断分类）
        };
        Status status = Status::none;
        /** @brief 折入 parse_digest 的身份；none 时为空。 */
        std::string identity;
        /** @brief 人类可读的原因/失败类（摘要与逐 TU note）。 */
        std::string note;
        /**
         * @brief 实际注入的缓存影子路径（仅 status==shadow）。
         *
         * @internal 下游 pch-unusable 失效用它定位 slot；文本回退/重放
         * 路径为空。
         */
        std::string shadow_path;
    };

    /**
     * @brief 一条数据库条目，重整形供重放。
     */
    struct CompileCommand
    {
        std::string directory; // 绝对、规范化（加载时解析所得）
        std::string file;      // 绝对、规范化
        /**
         * @brief 仅编译器开关：argv[0] 丢弃、"-c" 丢弃、"-o `<path>`"
         * （分写或连写）丢弃。
         *
         * @internal libclang 反正会忽略它们；丢弃使重放输入统一。
         */
        std::vector<std::string> argv;
        /**
         * @brief 数据库记录的编译器可执行文件原样拼写（argv[0]）。
         *
         * @internal 它是本条目投喂或写出的任何 BMI 的生产者身份；保持
         * 原样（可能相对；运行时按 directory 解析）。
         */
        std::string compiler;
        /**
         * @brief 本条目构建产出的 BMI（"-fmodule-output="，解析为绝对；
         * 条目不是模块接口单元时为空串）。
         *
         * @internal ccg 自身 BMI 配置键的生产者一半。
         */
        std::string module_output;
        /**
         * @brief 本条目投喂的命名模块 BMI（"-fmodule-file=NAME=path"，
         * 解析为绝对；经典 TU 为空）。
         *
         * @internal 投喂同时保留在 argv 中——本字段是供校验与分类的
         * 结构化孪生。
         */
        std::vector<ModuleFeed> module_feeds;
        /**
         * @brief 首个模块依赖映射响应文件（"@…modmap"，解析为绝对；无则
         * 空串）：姊妹 CXXModules.json 的锚点。
         */
        std::string module_map;
        /**
         * @brief 一级 P1689 扫描所得 TU 的直接模块 requires。
         *
         * @internal 无二级解析而降级的 TU 仍由此列表落其书写 import
         * （行 0 的 import 行）；扫描从未覆盖的条目为空。
         */
        std::vector<std::string> scan_imports;
        /**
         * @brief 命令合成阶段已预知的失败（自建降级：std BMI 不可构建、
         * 模块 DAG 环、扫描洞）：索引器逐字上报此文本而不解析该 TU。
         */
        std::string preordained_failure;
        /**
         * @brief BMI 级失败的重试观察路径（绝对、规范化、排序去重）。
         *
         * @internal 自建链对模块单元的 BMI 生产失败（自身接口不可用，或
         * 依赖模块失败而投喂被拒）时，把该单元 P1689 requires 闭包里每个
         * 接口依赖的源路径附着在命令上。索引器的失败记录把它们连同 stat
         * 身份冻结进 deps_json——失败 TU 自己的 includes 行无从产生（解析
         * 未发生或未成功），世界快照必须由命令侧补给，依赖源被修复后
         * deps_changed 才拉得回重试。空 = 无 BMI 级失败语境；本字段不参与
         * 任何摘要（cmd/feed）的计算。
         */
        std::vector<std::string> bmi_failure_deps;
        /** @brief 本 TU 解析的覆盖旋钮（见 ModuleOverlay）。 */
        ModuleOverlay overlay;
        /**
         * @brief 覆盖改写了其书写行的 import 名（伞头下的 std/std.compat、
         * include 改写下的头单元）。
         *
         * @internal 即使解析成功，索引器也把它们落成扫描通道的行 0
         * import 行（改写抹掉了它们的 import 游标）。
         */
        std::vector<std::string> overlay_imports;
        /**
         * @brief 条目是否携带 PCH 标记（消费或生产）。
         *
         * @internal 条目级门控：只有它为 true 的条目才走 PCH 归一化
         * （-Xclang 原子对、路径按 directory 绝对化、cl /Yu /Yc /Fp 守卫、
         * -include 转发化）。其余条目的 argv 逐字节保持历史形状。
         */
        bool pch_involved = false;
        /**
         * @brief loader 推断出的 cl 方言（`/Yc` `/Yu` `/Fp` 或
         * `--driver-mode=cl`）。
         *
         * @internal 重放与 producer 跳过判定共用的唯一信号：外来 MSVC cdb
         * 的 argv 是 `cl.exe /c /I ... /Ycpch.h /Fp...`，没有
         * `--driver-mode=cl`，只看该旗标会把记录误判成 GNU。非 PCH 条目
         * 保持 false（不影响历史 argv/摘要）。
         */
        bool cl_mode = false;
        /**
         * @brief 系统头来源已降级为 clang-cl 自动探测。
         *
         * @internal 外来原生 MSVC cdb 的 cl 路径没有工具集线索（如裸
         * `cl.exe`），或带线索但 vcvars 重建 INCLUDE 失败时为 true（原始
         * 命令已带 `/X` 时自动探测被关掉，故两种情况下都不标注）。摘要与
         * JSON 据此如实输出 `system-includes: auto-detected`，绝不静默
         * 假装精确。无 cl 条目的工程恒为 false（零变化门禁）。
         */
        bool system_includes_auto = false;
        /** @brief 记录的生产者输出（-o 或 /Fp），绝对。 */
        std::string pch_output;
        /** @brief 消费者引用的 PCH（-include-pch 或 /Fp），绝对。 */
        std::string pch_path;
        /** @brief /Yc 或 /Yu 的头拼写（原样；裸 /Yu 为空）。 */
        std::string pch_header;
        /** @brief 本条目是 PCH 生产者（-x c++-header+emit-pch 或 /Yc）。 */
        bool pch_create = false;
        /** @brief 本条目消费 PCH（-include-pch 或 /Yu）。 */
        bool pch_use = false;
        /** @brief 强制包含（-include / -FI），绝对。 */
        std::vector<std::string> forced_includes;
        /** @brief 规划器判定（见 PchUse）。 */
        PchUse pch;
        /**
         * @brief 文本回退 argv（影子 PCH 加载失败时解析器就地重试）。
         *
         * @internal 只有 pch_use 条目可能非空；不在 parse_digest 中。
         */
        std::vector<std::string> pch_fallback_argv;
    };

    /**
     * @brief 条目是否描述命名模块单元：模块味文件扩展名或重放 argv 中
     * 显式的 "-x c++-module"。
     *
     * @internal 扩展判定走 util 的分类单点（ASCII 折叠后缀——大写 .IXX
     * 同样是模块单元）。
     */
    [[nodiscard]]
    auto is_module_tu(const CompileCommand& cc) -> bool
    {
        if (cg::util::is_module_unit_file(cc.file))
        {
            return true;
        }
        for (std::size_t i = 0; i + 1 < cc.argv.size(); ++i)
        {
            if (cc.argv[i] == "-x"
                && (cc.argv[i + 1] == "c++-module"
                    || cc.argv[i + 1] == "cpp-module"))
            {
                return true;
            }
        }
        return false;
    }
} // namespace cg::clang
