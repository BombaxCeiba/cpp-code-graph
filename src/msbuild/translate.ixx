/**
 * @module cg.msbuild.translate
 *
 * @brief cl 方言编译命令适配器：把一次被包装的 MSBuild cl 调用转换为
 * 一条或多条 compile_commands.json 条目（cl 模式为主路线，机械翻译是
 * MSBuild 管线规格 中记载的文档化兜底）。
 *
 * 条目的 flags 是记录到的 cl flags 逐字保留——clang-cl 原生解析
 * /I /D /std: /EHsc /openmp /arch: …，未知 flag 至多告警，因此适配器
 * 从不静默改写语义。
 *
 * @internal
 * 仅有的三处编辑（每处都经实证必要）：
 * 1. 在编译器名后注入 "--driver-mode=cl" 作首 token（序列化为
 *    "clang-cl"；ccg 加载器会丢弃 arguments[0]）。
 * 2. 丢弃位置输入文件（MSBuild 会把多个 TU 批进一次 cl 调用，每个
 *    恰好成为一条目）以及 /Fo /Fd /Fp /Yc /Yu（产物/PCH 路径；/Yc
 *    /Yu /Fp 在 cl 模式实证为 rc=4；/Yu 的 TU 仍能解析，因为 pch 的
 *    include 就在源码里）与硬失败黑名单 /Gf /u-（实证 rc=4；其有效
 *    兄弟拼写 /GF /u 保留）。
 * 3. 丢弃非 C/C++ 编译的调用（rc.exe/midl.exe 等工具基名、无 C/C++
 *    位置参数、位置参数非 C/C++ 源）。
 * 另有两处超出字面配方的机械必要（均保语义）：
 * - "@file" 响应 token 防御性展开（递归、BOM 自动探测、MSVC CRT 引用
 *   规则）。到达 ccg 加载器的 '@' 会按其面向模块的规则展开——该规则
 *   对每个非 '-' token 做路径解析，会破坏 cl flag；未展开的 '@' 到达
 *   libclang 则是孤儿输入（rc=4）。
 * - 带路径值的 include flag /I /external:I /FI（连写或分写形式）按
 *   记录的 cwd 绝对化。ccg 加载器对 GNU 拼写的相对 include 值按条目
 *   目录解析、但对 cl 拼写原样透传，而 libclang 按进程 cwd 解析相对
 *   include 路径——进程 cwd 并非记录的编译目录。
 * flag 匹配是规范大小写的精确匹配：cl flag 大小写有意义（/Gf 与 /GF、
 * /fp: 与 /Fp、/Zi 与 /ZI），非规范拼写逐字保留、留给下游告警或失败，
 * 绝不静默重解释。分值 flag（/U /FU /AI /arch /source-charset
 * /execution-charset /validate-charset，以及 MSVC 模块带值开关六项
 * /ifcOutput /scanDependencies /sourceDependencies /ifcSearchDir
 * /reference /exportHeader——名单出自 cg.util.msvcswitch 单点清单）
 * 逐字保留并吞掉其随后的 token：开关与值成对进 cdb（cdb 是采集产物，
 * 忠实记录 MSBuild 实发命令行），防值 token 被误读为位置源文件或孤儿
 * 输入丢弃；喂引擎前的成对剥除由剥离层单点负责（cdb 里写什么、剥什么，
 * 绝不剥一半留一半）。/D 是唯一例外——分写值拼为 /DMACRO=VALUE
 * 单 token（clang 响应文件按空白切词，分离两 token 会破坏兼容）。
 * 另有两处与模板模式旗标同层的透明追加（写进 cdb 可见，显式声明过的
 * 条目原样不加）：无 /utf-8 且无 /source-charset 声明时补
 * -finput-charset=<运行时系统 ANSI 代码页>（对齐 cl 的默认源编码；
 * 带 BOM 的文件不受影响）；无模板模式声明时补 /Zc:twoPhase-。
 * 捕获的 INCLUDE 环境变量全量物化：按捕获顺序把每个非空条目的绝对
 * 路径追加为 "-imsvc DIR" 对（工具集、VS Aux、Windows SDK、NETFXSDK
 * 一个都不丢），并在 argv 中插入 cl 的 `/X`，令重放不再依赖 ccg 进程
 * 环境或 clang-cl 自动探测。与该条目 /I（含连写形态）重复的目录除外：
 * 同目录 user/system 双身份会被 clang 去重保留 system 位置，/I 的前置
 * 搜索优先级因此丢失（同名 stdafx.h 解析到错误文件）。若原始 cl 命令
 * 已带 `/X`（cl 本就会忽略 INCLUDE；`/x` 与 `-X`/`-x` 是同一选项的
 * 合法拼写），则绝不追加 INCLUDE 目录。兼容版本优先按该次调用 INCLUDE 里的
 * `.../VC/Tools/MSVC/`<ver>`/include` 解析（逐 TU 权威），回退到
 * real_cl 路径：混合工具集的 .sln 里 real_cl 对整个构建是统一的最高
 * 工具集，只有 INCLUDE 反映每个 TU 的真实头文件集合。适配器是纯
 * 函数：同样输入同样输出、调用序与位置参数序保持。
 */
export module cg.msbuild.translate;

import std;
import cg.util;

export namespace cg::msbuild
{

    /**
     * @brief wrap 模式日志记录的一次编译器调用（argv 为 UTF-8，含编译器/
     * 包装器 token）。
     */
    struct ClInvocation
    {
        std::vector<std::string> argv_utf8;
        std::string              cwd; // 构建的绝对工作目录
        std::string include_env;      // 捕获的 INCLUDE ';' 列表（全量物化用）
        /**
         * @brief 本次调用实际转发的 cl.exe 绝对路径（wrap 记录的 real_cl）。
         *
         * @internal 逐次调用记录而不是整个构建一份：混合工具集的 solution
         * 同一构建里不同 TU 的工具集可以不同，兼容版本必须逐 TU 解析。
         */
        std::string real_cl;
    };

    /**
     * @brief 一条 compile_commands.json 条目（"arguments" 形状，含编译器
     * token，与序列化结果完全一致）。
     */
    struct CdbEntry
    {
        std::string              directory;
        std::string              file;
        std::vector<std::string> arguments;
    };

    /**
     * @brief 从 MSVC 工具集路径解析版本号（正斜杠/反斜杠、任意大小写均可）。
     *
     * 形如 `.../VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64/cl.exe` →
     * `{14, 51, 36231}`。这是本模块适配器与 `ccg msbuild` 的 vswhere 兜底
     * 挑选共用的单一实现。
     *
     * @internal 查找算法：斜杠规范化 → 整串折叠小写 → 找三连段标记
     * `/vc/tools/msvc/` → 取标记后的单段作版本。折叠小写让真实磁盘上
     * 任意混合大小写拼写（含全小写形态）都能命中；三连段标记防"路径里
     * 恰有 MSVC 字样的目录"（如 `…/MSVC/VS18/…`）被误当工具集根。
     * 版本段交给 cg::util::parse_dotted_version 严格校验——每段必须纯
     * 数字，`latest`、`14.29.x`、尾空段（`14.`）等畸形一律返回空，绝不
     * 产出 {0} 假版本混入"最高版本胜出"的比较。
     */
    [[nodiscard]]
    inline auto msvc_toolset_version_of(std::string_view cl_path)
        -> std::vector<int>
    {
        const std::string forward = cg::util::to_forward_slashes(cl_path);
        const std::string lower = cg::util::ascii_lower(forward);
        constexpr std::string_view marker{"/vc/tools/msvc/"};
        const std::size_t          at = lower.find(marker);
        if (at == std::string::npos)
        {
            return {};
        }
        const std::size_t      begin = at + marker.size();
        const std::size_t      end = forward.find('/', begin);
        const std::string_view version = std::string_view{forward}.substr(
            begin,
            end == std::string::npos ? std::string::npos : end - begin);
        return cg::util::parse_dotted_version(version);
    }

    namespace detail
    {

        /**
         * @brief 大小写敏感的精确匹配 / 连缀前缀匹配表。
         *
         * @internal 规范 cl 拼写以 MSBuild 实际发出的为准；大小写规则的理由
         * 见本模块的模块级说明。
         */
        constexpr std::string_view sep_include[] = {"/I", "/external:I", "/FI"};
        constexpr std::string_view sep_drop[] =
            {"/Fo", "/Fd", "/Fp", "/Yc", "/Yu"};
        constexpr std::string_view blacklist[] = {"/Gf", "/u-"};
        // /D 不在此表：分写值改拼为单 token（/DMACRO=VALUE，见适配循环内的
        // 专属分支）——clang 响应文件按空白切 token，分离的 "/D" "MACRO" 两个
        // token 会被当成两个独立参数，破坏响应文件兼容。
        // MSVC 模块带值开关六项（/ifcOutput /scanDependencies
        // /sourceDependencies /ifcSearchDir /reference /exportHeader）不在
        // 此表而在 cg.util.msvcswitch 单点清单：名单与剥离层共用一份，
        // '/' 前缀的适配由 is_module_valued_switch 在本侧一处完成。
        constexpr std::string_view sep_value_kept[] = {
            "/U",
            "/FU",
            "/AI",
            "/arch",
            "/source-charset",
            "/execution-charset",
            "/validate-charset"};
        constexpr std::string_view joined_include[] = {
            "/external:I",
            "/FI",
            "/I"};
        constexpr std::string_view joined_drop[] =
            {"/Fo", "/Fd", "/Fp", "/Yc", "/Yu"};
        /**
         * @brief 绝不可到达 libclang 的非 C/C++ 工具基名（rc=4 失败类）。
         *
         * @internal CLToolExe 包装器只会见到 cl，此表是纵深防御。
         */
        constexpr std::string_view tool_skip[] = {
            "rc.exe",
            "midl.exe",
            "mt.exe",
            "mc.exe",
            "xaml.exe",
            "manifest.exe"};

        [[nodiscard]]
        inline auto in_table(
            std::span<const std::string_view> table,
            std::string_view                  token) -> bool
        {
            return std::ranges::find(table, token) != table.end();
        }

        /**
         * @brief token 是否 MSVC 模块带值开关的规范 '/' 拼写（精确大小写）。
         *
         * @internal 名单出自 cg::util::msvcswitch::valued 单点清单（与剥离层
         * 共用一份）；'/' 前缀在这里一处适配——清单存无前缀规范拼写，对齐
         * 剥离层判定的既有形态。冒号连写形态（"/ifcOutput:值"）不在此命中
         * ——它作为单个 '/' 旗标 token 逐字进 cdb，开关与值本就同 token。
         * 精确大小写与本模块的旗标匹配纪律一致：MSBuild 只发规范拼写，非
         * 规范拼写逐字保留、绝不静默重解释。
         */
        [[nodiscard]]
        inline auto is_module_valued_switch(std::string_view token) -> bool
        {
            if (token.size() < 2 || token.front() != '/')
            {
                return false;
            }
            const std::string_view spelling = token.substr(1);
            return std::ranges::find(cg::util::msvcswitch::valued, spelling)
                != std::end(cg::util::msvcswitch::valued);
        }

        /**
         * @brief cl 方言条目的源字符集是否已显式声明。
         *
         * @internal /utf-8 与 /source-charset（连写 ':' 或分写值）任一在场即
         * 已声明——工程自己选择了源编码世界观。该族选项 cl 前缀 '/' 与 '-'
         * 皆合法、大小写不敏感，故按小写后的全量拼写比对。
         */
        [[nodiscard]]
        inline auto input_charset_declared(
            const std::vector<std::string>& flags) -> bool
        {
            for (const std::string& f : flags)
            {
                const std::string low = cg::util::ascii_lower(f);
                if (low == "/utf-8" || low == "-utf-8"
                    || low == "/source-charset" || low == "-source-charset"
                    || low.starts_with("/source-charset:")
                    || low.starts_with("-source-charset:"))
                {
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief 模板模式旗标是否已在命令行里显式声明。
         *
         * @internal /permissive-（一致模式）与 /Zc:twoPhase（显式两阶段查找）
         * 都表示工程自己选择了严格模板语义；/Zc:twoPhase- 则是延迟解析的
         * 显式拼写。cl 前缀 '/' 与 '-' 皆合法、该族选项大小写不敏感，故按
         * 小写后的全量拼写比对；任一在场即视为已声明，等效旗标不再补。
         */
        [[nodiscard]]
        inline auto template_mode_declared(
            const std::vector<std::string>& flags) -> bool
        {
            for (const std::string& f : flags)
            {
                const std::string low = cg::util::ascii_lower(f);
                if (low == "/permissive-" || low == "-permissive-"
                    || low == "/zc:twophase" || low == "-zc:twophase"
                    || low == "/zc:twophase-" || low == "-zc:twophase-")
                {
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief token 是否为 cl 的 IgnoreStandardIncludePath（`/X`）。
         *
         * @internal cl 接受 '/' 与 '-' 两种前缀且该选项大小写不敏感。clang-cl
         * 只认规范 `/X` 与 `-X`（实测 `/x` 被当作输入文件）；因此共享判定
         * 接受 `/X`、`/x`、`-X`，而 `-x` 只在确知是原生 cl 时接受（在那里
         * 它是 /X；在 clang-cl 里它是语言选项）。适配器/加载器/PCH 闭包三处
         * 必须共用同一判定，否则原命令用 `-X`/`/x` 时会把捕获的 INCLUDE 重新
         * 物化回来（cl 本应忽略它们）。
         */
        [[nodiscard]]
        inline auto is_ignore_standard_include(std::string_view token) -> bool
        {
            if (token.size() != 2)
            {
                return false;
            }
            if (token[0] == '/')
            {
                return token[1] == 'X' || token[1] == 'x';
            }
            return token[0] == '-' && token[1] == 'X';
        }

        /** @brief 原生 cl 的 IgnoreStandardIncludePath（额外接受 `-x`）。 */
        [[nodiscard]]
        inline auto is_native_cl_ignore_include(std::string_view token) -> bool
        {
            return is_ignore_standard_include(token) || token == "-x";
        }

        /**
         * @brief C/C++ 翻译单元扩展名的大小写不敏感后缀测试。
         *
         * @internal 判定收编至 util 的扩展分类单点（is_tu_input_file）。既往
         * 本地表比 compile_db 侧少收 .mpp/.mxx/.c++m 三拼（对方注释自认"与
         * msbuild 适配器同语义"，集合却漂移）——统一到全集合。
         */
        [[nodiscard]]
        inline auto has_source_ext(std::string_view token) -> bool
        {
            return cg::util::is_tu_input_file(token);
        }

        /** @brief 取路径最后一段并小写化（路径可能是 Windows 分隔符拼写）。 */
        [[nodiscard]]
        inline auto path_basename_lower(std::string_view path) -> std::string
        {
            return cg::util::ascii_lower(cg::util::path_basename(path));
        }

        /**
         * @brief 相对 cwd 绝对化：正斜杠、大小写与盘符保留、点段折叠。
         *
         * @internal
         * 刻意不用 cg::util::norm_path：它会把盘符小写化，破坏字节稳定的
         * 黄金样张。刻意做纯文本处理（项目跨平台规则）：拼接前先把反斜杠
         * 换为正斜杠、绝对性按文本判定（盘符根、'/' 或 UNC '//'），使适配器
         * 对同一记录调用在任何平台产出完全相同的输出。lexically_normal 会
         * 保留尾部分隔符，而黄金形状不带（裸盘符根保留其单一尾斜杠）。
         */
        [[nodiscard]]
        inline auto to_abs(std::string_view value, std::string_view cwd)
            -> std::string
        {
            std::string cleaned;
            cleaned.reserve(value.size());
            for (const char c : value)
            {
                if (c == '\\')
                {
                    cleaned.push_back('/');
                }
                else if (c != '"')
                {
                    cleaned.push_back(c);
                }
            }
            const bool absolute =
                cleaned.size() >= 3
                && ((cleaned[0] >= 'A' && cleaned[0] <= 'Z')
                    || (cleaned[0] >= 'a' && cleaned[0] <= 'z'))
                && cleaned[1] == ':' && cleaned[2] == '/';
            std::filesystem::path p{cleaned};
            if (!absolute && !cleaned.starts_with('/'))
            {
                p = std::filesystem::path{std::string{cwd}} / p;
            }
            std::string out = p.lexically_normal().generic_string();
            while (out.size() > 3 && out.back() == '/')
            {
                out.pop_back();
            }
            return out;
        }

        /**
         * @brief UTF-16LE（带 BOM）响应文件字节转 UTF-8。
         *
         * @internal 代理项配对合并；孤立代理项转为 U+FFFD——响应文件的
         * UTF-16 损坏属数据损坏，不应令整个转换失败。仓内唯一实现（std 现代
         * 设施缺位且 wstring_convert 已弃用）；第二处需求出现时先上提 util
         * 立封装，不在调用点再写一份。
         */
        [[nodiscard]]
        inline auto utf16le_to_utf8(std::string_view raw) -> std::string
        {
            std::string out;
            out.reserve(raw.size() / 2);
            for (std::size_t i = 0; i + 1 < raw.size(); i += 2)
            {
                const char32_t unit =
                    static_cast<unsigned char>(raw[i])
                    | (static_cast<char32_t>(
                           static_cast<unsigned char>(raw[i + 1]))
                       << 8);
                char32_t cp = unit;
                if (unit >= 0xD800 && unit <= 0xDBFF && i + 3 < raw.size())
                {
                    const char32_t low =
                        static_cast<unsigned char>(raw[i + 2])
                        | (static_cast<char32_t>(
                               static_cast<unsigned char>(raw[i + 3]))
                           << 8);
                    if (low >= 0xDC00 && low <= 0xDFFF)
                    {
                        cp = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                        i += 2;
                    }
                }
                else if (unit >= 0xD800 && unit <= 0xDFFF)
                {
                    cp = 0xFFFD;
                }
                if (cp < 0x80)
                {
                    out.push_back(static_cast<char>(cp));
                }
                else if (cp < 0x800)
                {
                    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                }
                else if (cp < 0x10000)
                {
                    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                }
                else
                {
                    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                    out.push_back(
                        static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                }
            }
            return out;
        }

        /**
         * @brief 读一个响应文件为 flag 文本。
         *
         * @internal UTF-8 BOM 剥离、UTF-16LE BOM 自动探测（MSBuild 写的是
         * 无 BOM 的 UTF-8）。
         */
        [[nodiscard]]
        inline auto read_rsp_text(const std::string& path) -> std::string
        {
            const auto bytes = cg::util::read_text_file(path);
            if (!bytes.has_value())
            {
                throw std::runtime_error(
                    "ccg msbuild: cannot read response file: " + path);
            }
            std::string raw = std::move(*bytes);
            if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF
                && static_cast<unsigned char>(raw[1]) == 0xBB
                && static_cast<unsigned char>(raw[2]) == 0xBF)
            {
                return raw.substr(3);
            }
            if (raw.size() >= 2 && static_cast<unsigned char>(raw[0]) == 0xFF
                && static_cast<unsigned char>(raw[1]) == 0xFE)
            {
                return utf16le_to_utf8(raw.substr(2));
            }
            return raw;
        }

        /**
         * @brief 展开每个 "@file" token（递归；嵌套响应文件是合法 cl 输入）。
         *
         * 相对名按调用 cwd 解析；裸 "@" 不带路径、逐字保留。
         *
         * @internal
         * 深度上限 16 作循环防护：超出即数据错误、响亮失败，绝不挂起。读不到
         * 的响应文件抛异常——静默保留或丢弃 token 都会在下游产生不诚实的失败。
         */
        [[nodiscard]]
        inline auto expand_rsp(
            std::span<const std::string> argv,
            const std::string&           cwd,
            std::size_t                  depth = 0) -> std::vector<std::string>
        {
            constexpr std::size_t max_depth = 16;
            if (depth > max_depth)
            {
                throw std::runtime_error(
                    std::format(
                        "ccg msbuild: response files nested deeper than {}",
                        max_depth));
            }
            namespace fs = std::filesystem;
            std::vector<std::string> out;
            out.reserve(argv.size());
            for (const std::string& token : argv)
            {
                if (token.size() < 2 || token[0] != '@')
                {
                    out.push_back(token);
                    continue;
                }
                std::string path = token.substr(1);
                // 记录到的 rsp 文件名可能用 '\' 作分隔符。
                std::ranges::replace(path, '\\', '/');
                if (fs::path{path}.is_relative())
                {
                    path = (fs::path{cwd} / path).lexically_normal().string();
                }
                const std::string              text = read_rsp_text(path);
                const std::vector<std::string> split =
                    cg::util::tokenize_command(text);
                const std::vector<std::string> nested =
                    expand_rsp(split, cwd, depth + 1);
                out.insert(out.end(), nested.begin(), nested.end());
            }
            return out;
        }

        /**
         * @brief 从捕获的 INCLUDE 环境变量提取全部目录（按捕获顺序）。
         *
         * 每个 ';' 分隔条目按捕获顺序、相对记录的 cwd 绝对化后返回（相对
         * `<IncludePath>` 条目按 cl 工作目录解析；$(ProjectDir) 拼接产生的双
         * 反斜杠折叠）。工具集、VS Aux、Windows SDK、NETFXSDK 与项目目录
         * 一视同仁——全量物化才让 argv 自包含。
         *
         * @internal
         * 空条目跳过；INCLUDE 缺失或为空时返回空。取条目必须用
         * string_view::substr 而非 std::string::substr：后者的临时对象会
         * 悬垂进循环体——此修复的第一次单元运行把已释放的 SSO 缓冲读成 NUL
         * 段并投喂了它们。
         */
        [[nodiscard]]
        inline auto env_include_dirs(
            const std::string& include_env,
            std::string_view   cwd) -> std::vector<std::string>
        {
            std::vector<std::string> out;
            if (include_env.empty())
            {
                return out;
            }
            for (const std::string_view entry :
                 cg::util::split(include_env, ';'))
            {
                if (!entry.empty())
                {
                    out.push_back(to_abs(entry, cwd));
                }
            }
            return out;
        }

        /**
         * @brief 把 MSVC 工具集版本号数组（14.minor.build[.rev]）映射成
         * clang 的 MSVC 兼容版本 `19.minor.build`；段不足时返回空。
         *
         * @internal 实测 `-fms-compatibility-version=19.44.35207` 精确复现
         * `_MSC_FULL_VER=194435207`；只给 major.minor 会丢掉 build 段。
         */
        [[nodiscard]]
        inline auto compat_version_of_parts(const std::vector<int>& v)
            -> std::string
        {
            if (v.size() < 3)
            {
                return {};
            }
            return std::format("19.{}.{}", v[1], v[2]);
        }

        /** @brief cl 路径到兼容版本的同一映射（工具集版本号解析后取段）。 */
        [[nodiscard]]
        inline auto compat_version_of(std::string_view cl_path) -> std::string
        {
            return compat_version_of_parts(msvc_toolset_version_of(cl_path));
        }

        /**
         * @brief 捕获的 INCLUDE 里 MSVC 工具集 include 的版本号数组。
         *
         * @internal INCLUDE 的分号列表按捕获顺序扫描，首个形如
         * `.../VC/Tools/MSVC/`<ver>`/...` 的条目胜出，msvc_toolset_version_of
         * 的解析结果原样返回（不拼回字符串——兼容版本映射直接吃号数组，
         * 无序列化再反序列化的往返）。工具集 include 与 atlmfc include
         * 同版本，任一命中即可。没有工具集目录时返回空。
         */
        [[nodiscard]]
        inline auto include_env_toolset_version(std::string_view include_env)
            -> std::vector<int>
        {
            for (const std::string_view entry :
                 cg::util::split(include_env, ';'))
            {
                if (const std::vector<int> parts =
                        msvc_toolset_version_of(entry);
                    parts.size() >= 3)
                {
                    return parts;
                }
            }
            return {};
        }

        /**
         * @brief 本次调用的兼容版本来源。
         *
         * @internal 顺序：捕获 INCLUDE 的工具集目录 > 记录的 real_cl > argv[0]
         * 的绝对工具集路径（旧日志/手写调用）。INCLUDE 在最前是因为混合
         * 工具集的 .sln 里 wrap 记录的 real_cl 对整个构建统一（最高工具集），
         * 只有每次 cl 调用的 INCLUDE 才携带该 TU 的真实头文件工具集；不一致
         * 时以 INCLUDE 为准，否则 projB 会带着 19.51 的兼容版本去解析 14.44
         * 的头文件。INCLUDE 缺席时行为与历史一致。
         */
        [[nodiscard]]
        inline auto invocation_compat_version(
            const ClInvocation& inv,
            std::string_view    argv0) -> std::string
        {
            if (const std::vector<int> from_include =
                    include_env_toolset_version(inv.include_env);
                from_include.size() >= 3)
            {
                return compat_version_of_parts(from_include);
            }
            if (!inv.real_cl.empty())
            {
                return compat_version_of(inv.real_cl);
            }
            return compat_version_of(argv0);
        }

    } // namespace detail

    /**
     * @brief 把一次被包装的 cl 调用适配为 cl 模式编译数据库条目。
     *
     * 可能产出多条（MSBuild 批量打包 TU）；调用不是 C/C++ 编译时产出零条。
     * 纯函数且确定。位置输入文件中只有 C/C++ TU 成为条目，杂散非源路径按
     * 孤儿输入防御性丢弃。
     *
     * @internal
     * 记录 cwd 的一个正斜杠拼写供响应文件解析、条目目录与每次 to_abs
     * 拼接使用（记录的日志用 '\'）。捕获的 INCLUDE 环境按原顺序全量
     * 物化为 "-imsvc DIR" 空格形式（/imsvc: 冒号拼写会把 ':' 与盘符
     * 粘连、目录随后被静默丢弃，本工具链上用遮蔽头文件复验过）：
     * - 原始命令没有 `/X` 时，先追加 `/X` 再追加全部 INCLUDE 目录。`/X`
     *   同时关掉 clang-cl 自动探测与进程 INCLUDE，argv 因此成为唯一来源
     *   （实测：`/X -imsvc D` + 外部 INCLUDE=X 只搜 D）；
     * - 原始命令已带 `/X` 时（cl 的 IgnoreStandardIncludePath），cl 本就
     *   忽略 INCLUDE——绝不能再把它们追加回来，否则是独立的保真缺陷。
     *   判定接受 cl 的合法拼写：`/X`、`/x`、`-X`、`-x`；
     * - 兼容版本按本次调用 INCLUDE 的工具集目录（逐 TU）追加，回退
     *   real_cl，混合工具集因此逐 TU 正确。
     */
    [[nodiscard]]
    inline auto adapt_cl(const ClInvocation& inv) -> std::vector<CdbEntry>
    {
        if (inv.argv_utf8.empty())
        {
            return {};
        }
        if (detail::in_table(
                std::span{detail::tool_skip},
                detail::path_basename_lower(inv.argv_utf8[0])))
        {
            return {};
        }
        std::string        cwd = cg::util::to_forward_slashes(inv.cwd);
        const std::string& directory = cwd;

        const std::vector<std::string> argv =
            detail::expand_rsp(inv.argv_utf8, cwd);

        std::vector<std::string> flags;
        flags.reserve(argv.size());
        std::vector<std::string> sources;
        // 记录 /I 声明的目录（已绝对化，分写与连写两种形态都收）：INCLUDE
        // 物化时与之重复的条目不再追加 -imsvc（见物化处的说明）。
        std::vector<std::string> i_dirs;
        bool                     original_ignores_include = false;
        const std::size_t        n = argv.size();
        for (std::size_t i = 1; i < n; ++i)
        {
            const std::string& a = argv[i];
            if (detail::is_native_cl_ignore_include(a))
            {
                // cl 对该选项大小写/'/'/'-' 前缀不敏感，而 clang-cl 只认
                // 规范拼写 `/X`（实测 `/x` 与 `-x` 分别被当作输入文件与
                // 语言选项）。语义等价地规范化，且最多保留一个。
                original_ignores_include = true;
                if (std::ranges::find(flags, "/X") == flags.end())
                {
                    flags.emplace_back("/X");
                }
                continue;
            }
            if (detail::in_table(std::span{detail::sep_include}, a)
                && i + 1 < n)
            {
                flags.push_back(a);
                flags.push_back(detail::to_abs(argv[i + 1], cwd));
                if (a == "/I")
                {
                    i_dirs.push_back(flags.back());
                }
                ++i;
                continue;
            }
            if (detail::in_table(std::span{detail::sep_drop}, a))
            {
                // 分写值守卫：裸 `/Yu` 绝不能把下一条源文件当值吞掉
                // （AdditionalOptions=/Yu 曾使 cdb 少一条 TU 且 rc=0 无告警）。
                if (i + 1 < n && !argv[i + 1].starts_with('/')
                    && !argv[i + 1].starts_with('-')
                    && !detail::has_source_ext(argv[i + 1]))
                {
                    ++i;
                }
                continue;
            }
            if (detail::in_table(std::span{detail::blacklist}, a))
            {
                continue;
            }
            // /D 分写值拼为单 token（/DMACRO=VALUE）：与 /I 连写同款拼装
            // 风格；分离两 token 会破坏 clang 响应文件的按空白切词。末
            // token 的裸 "/D" 无值可拼，原样保留（与 /U 等分值族一致）。
            if (a == "/D" && i + 1 < n)
            {
                flags.push_back("/D" + argv[i + 1]);
                ++i;
                continue;
            }
            if ((detail::in_table(std::span{detail::sep_value_kept}, a)
                 || detail::is_module_valued_switch(a))
                && i + 1 < n)
            {
                // 分值族与模块带值开关同款行为：开关与值成对保留（值 token
                // 逐字、绝不绝对化——capture 记录的已是绝对路径）。末 token
                // 裸开关因 i+1<n 条件不误吞，与 /U 族一致；消费端剥离层的
                // 分写值守卫（下一 token 以 '/' 或 '-' 开头即旗标、不当值吞）
                // 保证剥除面与本面的吞值面互为镜像，引擎 argv 不变。
                flags.push_back(a);
                flags.push_back(argv[i + 1]);
                ++i;
                continue;
            }
            bool matched = false;
            for (const std::string_view p : detail::joined_include)
            {
                if (a.starts_with(p) && a.size() > p.size())
                {
                    const std::string abs_val =
                        detail::to_abs(a.substr(p.size()), cwd);
                    flags.push_back(std::string{p} + abs_val);
                    if (p == "/I")
                    {
                        i_dirs.push_back(abs_val);
                    }
                    matched = true;
                    break;
                }
            }
            if (matched)
            {
                continue;
            }
            for (const std::string_view p : detail::joined_drop)
            {
                if (a.starts_with(p) && a.size() > p.size())
                {
                    matched = true;
                    break;
                }
            }
            if (matched)
            {
                continue;
            }
            if (a[0] != '/' && a[0] != '-')
            {
                if (detail::has_source_ext(a))
                {
                    sources.push_back(a);
                }
                continue;
            }
            flags.push_back(a);
        }

        // cl 的 `/X`（IgnoreStandardIncludePath）：原始命令自己就忽略了
        // 标准 include 路径。clang-cl 在 /X 下同样忽略进程 INCLUDE，故这
        // 时把捕获值追加回来会重新引入 cl 本应忽略的目录。`/x`、`-X`、
        // `-x` 都是该选项的合法拼写（扫描时已规范化为 `/X`）。
        if (!original_ignores_include)
        {
            flags.emplace_back("/X");
            for (const std::string& dir :
                 detail::env_include_dirs(inv.include_env, cwd))
            {
                // 与记录 /I 同目录的 INCLUDE 条目不再物化为 -imsvc：同一
                // 目录以 user（/I）与 system（-imsvc）两身份出现时 clang
                // 去重保留的是 system 位置，/I 本应靠前的搜索优先级丢失
                // （同名 stdafx.h 被后面目录里的抢先解析）。比对大小写不
                // 敏感——Windows 文件系统不区分大小写，两侧都经 to_abs
                // 规范化过，仅剩大小写差异。
                const std::string low = cg::util::ascii_lower(dir);
                const bool        covered = std::ranges::any_of(
                    i_dirs,
                    [&](const std::string& d)
                    { return cg::util::ascii_lower(d) == low; });
                if (covered)
                {
                    continue;
                }
                flags.emplace_back("-imsvc");
                flags.push_back(dir);
            }
        }
        const std::string compat = detail::invocation_compat_version(
            inv,
            argv.empty() ? std::string_view{} : std::string_view{argv[0]});
        if (!compat.empty())
        {
            flags.push_back("-fms-compatibility-version=" + compat);
        }
        // 源编码对齐 MSVC（翻译层透明追加）：cl 默认按系统 ANSI 代码页读
        // 源文件，clang 默认按 UTF-8 读——无 /utf-8 也无 /source-charset
        // 声明的捕获命令补 -finput-charset=<运行时探测的系统 ANSI 字符集>
        // （Windows GetACP 映射，不硬编码；探测不到则不加）。带 BOM 的文件
        // 不受影响：clang 见 BOM 即按 BOM 解码，此旗标只管无 BOM 的字节流
        // （四象限夹具实测钉住）。显式声明过编码的条目原样不加，绝不代言。
        // 旗标写进 compile_commands.json 看得见（数据库里写什么引擎就跑
        // 什么）。
        if (!detail::input_charset_declared(argv))
        {
            if (const std::string cs = cg::util::platform::ansi_input_charset();
                !cs.empty())
            {
                flags.push_back("-finput-charset=" + cs);
            }
        }
        // 模板模式等效旗标（末尾、透明）：MSVC 默认（无 /permissive- 也无
        // /Zc:twoPhase*）是延迟解析（两阶段查找关闭），/Zc:twoPhase- 是
        // clang-cl 选项表里该语义的官方拼写（-fdelayed-template-parsing
        // 的正式别名）。命令行未显式表态时补齐——补在翻译产物末尾、写进
        // compile_commands.json 看得见（数据库里写什么引擎就跑什么）；
        // 显式表态过的条目原样保留，绝不代言。
        if (!detail::template_mode_declared(flags))
        {
            flags.emplace_back("/Zc:twoPhase-");
        }

        std::vector<CdbEntry> out;
        out.reserve(sources.size());
        for (const std::string& src : sources)
        {
            CdbEntry entry;
            entry.directory = directory;
            entry.file = detail::to_abs(src, cwd);
            entry.arguments.reserve(flags.size() + 2);
            entry.arguments.emplace_back("clang-cl");
            entry.arguments.emplace_back("--driver-mode=cl");
            entry.arguments.insert(
                entry.arguments.end(),
                flags.begin(),
                flags.end());
            out.push_back(std::move(entry));
        }
        return out;
    }

    /**
     * @brief 一次 cl 调用里的 PCH 分类（只读；cdb 适配保持剥离语义）。
     *
     * @internal `/Yc` 是 producer TU 普通编译命令上的旗标；`/Fp` 是
     * producer→consumer 的关联键。取“最后一个 `/Yc*` 与最后一个 `/Fp*`”
     * ——镜像 cl 后者覆盖前者的行为。`/Yu` 无 `/Fp` 时不可关联，调用方
     * 按 producer_missing 文本回退。
     */
    struct PchClassification
    {
        bool                     is_producer = false;
        bool                     pch_use = false;
        std::string              pch_key;    // 绝对 /Fp；无则 ""
        std::string              pch_header; // /Yc 或 /Yu 的头（原拼写）
        std::vector<std::string> argv;       // 展开后的逐字 argv（含 argv[0]）
        std::vector<std::string> sources;    // C/C++ 位置输入
    };

    /**
     * @brief 对一次捕获的 cl 调用做 PCH 分类。
     *
     * 纯函数；响应文件展开失败时返回空分类（不抛出——捕获管线已在别处
     * 报错，分类不是闸门）。
     */
    [[nodiscard]]
    inline auto classify_cl_pch(const ClInvocation& inv) -> PchClassification
    {
        PchClassification out;
        if (inv.argv_utf8.empty())
        {
            return out;
        }
        std::string cwd = cg::util::to_forward_slashes(inv.cwd);
        try
        {
            out.argv = detail::expand_rsp(inv.argv_utf8, cwd);
        }
        catch (const std::exception&)
        {
            return PchClassification{};
        }
        bool              yc_set = false;
        bool              yu_set = false;
        std::string       yc_header;
        std::string       yu_header;
        std::string       fp;
        const std::size_t n = out.argv.size();
        for (std::size_t i = 1; i < n; ++i)
        {
            const std::string& a = out.argv[i];
            if (a == "/Fp" || a == "/Yc" || a == "/Yu")
            {
                std::string value;
                bool        have = false;
                if (i + 1 < n && !out.argv[i + 1].starts_with('/')
                    && !out.argv[i + 1].starts_with('-')
                    && !detail::has_source_ext(out.argv[i + 1]))
                {
                    value = out.argv[i + 1];
                    have = true;
                    ++i;
                }
                if (a == "/Fp")
                {
                    if (have)
                    {
                        fp = detail::to_abs(value, cwd);
                    }
                }
                else if (a == "/Yc")
                {
                    yc_set = true;
                    yc_header = value;
                }
                else
                {
                    yu_set = true;
                    yu_header = value;
                }
                continue;
            }
            if (a.starts_with("/Fp") && a.size() > 3)
            {
                fp = detail::to_abs(a.substr(3), cwd);
                continue;
            }
            if (a.starts_with("/Yc") && a.size() > 3)
            {
                yc_set = true;
                yc_header = a.substr(3);
                continue;
            }
            if (a.starts_with("/Yu") && a.size() > 3)
            {
                yu_set = true;
                yu_header = a.substr(3);
                continue;
            }
            if (!a.empty() && a[0] != '/' && a[0] != '-'
                && detail::has_source_ext(a))
            {
                out.sources.push_back(a);
            }
        }
        out.is_producer = yc_set;
        // 以命令行为准：仅显式 /Yu 才算 PCH 消费者。
        // /Fp 只是产物路径声明，不等于"要使用 PCH"——工程配了 /Fp 但
        // NotUsing 的场景不应被卷进影子 PCH 通道。
        out.pch_use = !yc_set && yu_set;
        out.pch_key = fp;
        out.pch_header = yc_set ? yc_header : yu_header;
        return out;
    }

} // namespace cg::msbuild
