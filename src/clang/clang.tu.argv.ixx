

/**
 * @module cg.clang.tu.argv
 *
 * @brief argv 判定与驱动补供域：C++ TU 判定、include
 * 目录旗标、驱动补供参数、ParsedTu 载荷、目标三元组与实例化跳过许可。
 */
export module cg.clang.tu.argv;

import std;
import cg.util;
import cg.util.cstd;
import cg.clang.capi;
import cg.clang.driver;
import cg.clang.compile_db;
import cg.clang.tu.lex;

export namespace cg::clang
{

    namespace detail
    {

        [[nodiscard]]
        auto is_cxx_tu(std::string_view file, std::span<const std::string> argv)
            -> bool
        {
            for (const std::string& a : argv)
            {
                if (a.starts_with("-std=c++") || a.starts_with("-std=gnu++"))
                {
                    return true;
                }
            }
            constexpr std::string_view cxx_ext[] =
                {".cpp", ".cc", ".cxx", ".c++", ".hpp", ".hh", ".hxx", ".ipp"};
            for (const std::string_view ext : cxx_ext)
            {
                if (file.ends_with(ext))
                {
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief 某个 -I/-isystem/-iquote/-idirafter 条目（分写或连写 -I 形式）
         * 是否精确解析到 dir。
         *
         * @internal 此处子串测试是错的：记录的 "-I `<toolchain>`/include/
         * `<vendor>/<product>`"（恰好住在该前缀下的无关产品树）不得满足系统
         * "`<toolchain>`/include" 根而压制补充项（参考语料实测：每个这样的 TU
         * 随后都在 Windows.h / memcmp 上失败）。
         */
        [[nodiscard]]
        auto argv_has_include_dir(
            std::span<const std::string> argv,
            std::string_view             dir) -> bool
        {
            namespace fs = std::filesystem;
            const std::string want = [&]
            {
                std::error_code ec;
                return fs::path{std::string{dir}}
                    .lexically_normal()
                    .generic_string();
            }();
            for (std::size_t i = 0; i < argv.size(); ++i)
            {
                std::optional<std::string> value;
                if ((argv[i] == "-I" || argv[i] == "-isystem"
                     || argv[i] == "-iquote" || argv[i] == "-idirafter")
                    && i + 1 < argv.size())
                {
                    value = argv[++i];
                }
                else if (argv[i].starts_with("-I") && argv[i].size() > 2)
                {
                    value = argv[i].substr(2);
                }
                if (!value.has_value())
                {
                    continue;
                }
                std::error_code ec;
                if (cg::util::ascii_iequals(
                        fs::path{*value}.lexically_normal().generic_string(),
                        want))
                {
                    return true;
                }
            }
            return false;
        }

    } // namespace detail

    /**
     * @brief 记录的编译器驱动器隐式添加、而 libclang 不会添加的编译器
     * 开关：资源目录加驱动器自身的系统 include 根，按驱动器报告的顺序
     * （cg.clang.driver 讯问；本工具链上 libc++ 在前、其后是 mingw-w64
     * 的 C 集）。
     *
     * 资源目录的 INCLUDE 条目被跳过——-resource-dir flag 已覆盖；argv
     * 已有的 flag 绝不重复。
     *
     * @internal
     * 经参考语料重放冒烟运行发现。cl 模式门控：clang-cl 模式的重放绝不
     * 可被追加驱动器讯问来的 GNU 系统 include 根——clang-cl 把 -isystem
     * 当 GNU 选项忽略、其值变成孤儿位置输入、解析以
     * CXError_ASTReadError 失败（实证 v3/v4/v5）；改为把这些目录预置成
     * -I 又会让 libc++ 遮蔽 MSVC STL 而解析失败（v6）。clang-cl 自行
     * 找到 MSVC 工具链（实证 s7），故什么都不需要补；上面的
     * -resource-dir 不设门控——cl 模式也接受它（实证 v2_res）。
     * 自带 stdlib 布线的 TU 携带 -nostdinc++ 加自己的 -isystem stdlib 集
     * （libstdc++ 布线：3×-isystem + -nostdinc++ 是唯一无泄漏的接线）：
     * 它们自供 C++ 标准库，引擎自己的 stdlib 根（对 C++ 报告但对 C 不
     * 报告的那些目录）绝不可追加其后——libc++ 也带 stddef.h/__config，
     * libc++ 接线的引擎上 libstdc++ 布线的解析会经乘积撞上两个 stdlib
     * （实测 10 错误冲突；原生 gnu 布线引擎不受影响：其根已在 argv 上、
     * 在下方去重）。C 根保留：它们不是 stdlib、布线依赖它们。仅当 C
     * 报告确切给出 stdlib 专属集时跳过；空 C 报告保持历史追加行为。
     */
    [[nodiscard]]
    auto driver_supplement(
        const std::string&              file,
        const std::vector<std::string>& argv) -> std::vector<std::string>
    {
        const DriverInfo& info = driver_info();
        if (info.driver.empty())
        {
            throw std::runtime_error(
                "ccg: no clang driver executable found (looked beside the "
                "loaded LLVM engine library and under its installation prefix, "
                "then via CCG_LLVM_ROOT, then on PATH). Set CCG_LLVM_ROOT, pass "
                "`ccg index --llvm-root <prefix>`, or configure `ccg config set "
                "llvm-path <prefix>` to name the LLVM installation containing "
                "clang++/clang.");
        }
        std::vector<std::string> extra;
        if (!builtin_resource_dir().empty()
            && std::ranges::find(argv, "-resource-dir") == argv.end())
        {
            extra.emplace_back("-resource-dir");
            extra.push_back(builtin_resource_dir());
        }
        const bool cl_mode =
            std::ranges::find(argv, "--driver-mode=cl") != argv.end();
        if (!cl_mode)
        {
            const std::string resource_include =
                info.resource_dir.empty()
                    ? std::string{}
                    : cg::util::norm_path(
                          (std::filesystem::path{info.resource_dir} / "include")
                              .generic_string());
            const bool cxx_tu = detail::is_cxx_tu(file, argv);
            const std::vector<std::string>& reported =
                cxx_tu ? info.cxx_include_dirs : info.c_include_dirs;
            const bool rewired_stdlib =
                cxx_tu && !info.c_include_dirs.empty()
                && std::ranges::find(argv, "-nostdinc++") != argv.end();
            for (const std::string& dir : reported)
            {
                if (!resource_include.empty() && dir == resource_include)
                {
                    continue; // 由上面的 -resource-dir flag 承载
                }
                if (rewired_stdlib
                    && std::ranges::find(info.c_include_dirs, dir)
                           == info.c_include_dirs.end())
                {
                    continue; // 引擎的 stdlib 根，被 TU 的 -nostdinc++ 布线排除
                }
                if (!detail::argv_has_include_dir(argv, dir))
                {
                    extra.emplace_back("-isystem");
                    extra.push_back(dir);
                }
            }
        }
        return extra;
    }

    /**
     * @brief 解析一个翻译单元的结果。
     *
     * @internal parse() 返回时 `tu` 总是有效：可恢复诊断在 `errors` 中，
     * 部分恢复的游标树仍可走查。dialect_fixit_* 三字段记录诊断驱动的
     * 内存源码改写（见 detail::rewrite_lenient_specializations）是否
     * 发生：applied 表示确有一次重解析；改写后 errors 即重解析的诊断
     * （一次重解析上限，绝不二轮）。encoding_scrub_* 三字段同构，记录
     * ANSI 编码消毒（见 detail::rewrite_illegal_encoding；字符串/字符
     * 字面量内系统 ANSI 码页不可解码的字节转八进制转义后重解析一次，
     * 消毒先于 FixIt 改写执行，两者的内存基座链式叠加）。
     */
    struct ParsedTu
    {
        Tu                      tu;
        std::vector<Diagnostic> errors;
        bool                    dialect_fixit_applied = false;
        std::int64_t dialect_fixit_edits = 0;         // 实际应用的 FixIt 条数
        std::vector<std::string> dialect_fixit_files; // 被改写的文件（排序）
        bool                     encoding_scrub_applied = false;
        std::int64_t             encoding_scrub_edits = 0; // 消毒的编码单元数
        std::vector<std::string> encoding_scrub_files; // 被消毒的文件（排序）
    };

    namespace detail
    {

        /**
         * @brief argv 里的目标三元组（--target=<t> 连写与 -target/--target
         * 分写形态，与装载层 argv 值解析同款逐 token 扫描）。
         *
         * @internal 后写覆盖（驱动器语义：靠后的 -target 生效）。分写
         * 守卫：下一 token 以 '-' 开头视为旗标而非值（三元组永不以 '-'
         * 开头），裸开关绝不吞它。精确 token 匹配使 -target-cpu 之类的
         * 同前缀旗标不误配。
         */
        [[nodiscard]]
        auto argv_target_triple(const std::vector<std::string>& argv)
            -> std::string
        {
            std::string found;
            for (std::size_t i = 0; i < argv.size(); ++i)
            {
                const std::string& t = argv[i];
                if ((t == "-target" || t == "--target") && i + 1 < argv.size()
                    && !argv[i + 1].starts_with('-'))
                {
                    found = argv[++i];
                }
                else if (
                    t.starts_with("--target=") || t.starts_with("-target="))
                {
                    found = t.substr(t.find('=') + 1);
                }
            }
            return found;
        }

        /**
         * @brief 实例化跳过（TU_Prefix 形态）对该 argv 是否生效。
         *
         * @internal 环境变量值与 argv 都由调用方递入（纯函数，可单测）：
         * - CCG_SKIP_INSTANTIATIONS 精确值 "1" 才开（默认关=解析行为
         *   逐字节不变；与 CCG_HEADCACHE 同一显式开形态）。
         * - cl 方言条目（argv 含 --driver-mode=cl，装载/翻译层保证 cl
         *   家族条目带该旗标）永久禁用：原生 cl 条目装载期补
         *   /Zc:twoPhase-、clang-cl 在 MSVC target 低于 C++20 时驱动
         *   默认开延迟模板解析——延迟语料下 TU_Prefix 形态不 parse
         *   模板 pattern 体（LateParsed 实例化被转移且不再解析），模板
         *   体内符号与依赖调用边全丢，开关对该方言无效。
         * - 显式 -fdelayed-template-parsing（GNU 方言拼写）机制同上，
         *   一并禁用。
         * - 目标三元组为 *-windows-msvc 的 GNU 拼写条目一并禁用：驱动器
         *   对该目标在低于 C++20 时自动注入 -fdelayed-template-parsing
         *   （注入发生在 cc1 命令行装配期，argv 层不可见）。判据与标准
         *   版本无关——凡 msvc 三元组即禁用（免解析 -std 版本序，误禁
         *   方向安全：保真优先）；mingw 的 *-windows-gnu 三元组无此注入，
         *   不受影响。
         */
        [[nodiscard]]
        auto instantiation_skip_allowed(
            const std::optional<std::string>& env_value,
            const std::vector<std::string>&   argv) -> bool
        {
            if (env_value != std::optional<std::string>{"1"})
            {
                return false;
            }
            const std::string          triple = argv_target_triple(argv);
            constexpr std::string_view msvc_suffix{"-windows-msvc"};
            const bool                 msvc_target =
                triple.size() >= msvc_suffix.size()
                && cg::util::ascii_iequals(
                    std::string_view{triple}.substr(
                        triple.size() - msvc_suffix.size()),
                    msvc_suffix);
            return !msvc_target
                   && std::ranges::none_of(
                       argv,
                       [](const std::string& t)
                       {
                           return t == "--driver-mode=cl"
                                  || t == "-fdelayed-template-parsing";
                       });
        }

        /**
         * @brief 诊断驱动的内存源码改写：cl 方言的宽松特化家族。
         *
         * 形态：MSVC 接受、clang 报 "template specialization requires
         * 'template<>'" 的类外成员特化定义（如 `__declspec(selectany)
         * Widget Singleton<T>::mutex_;`）。clang 该诊断自带
         * FixItHint（在声明头插入 "template<> "，插入点与文本全由编译器给出）。
         *
         * 触发条件三合一，缺一零动作：仅 cl 方言（argv 含 --driver-mode=cl，
         * GNU 方言零改动）；首轮解析的诊断里含该拼写且该诊断带 FixIt
         * （绝不自己发明插入点）；FixIt 缺失时零动作。改写在内存源码缓冲按
         * FixIt 定点插入（unsaved buffer 管线，磁盘文件绝不修改），重解析
         * 一次；一次重解析后仍失败则如实保留新诊断（失败口径由调用方分类，
         * 不做二轮）。重解析硬失败（ParseError）时保留首轮 TU 与诊断。
         *
         * @internal 缓冲基座是覆盖感知的：FixIt 所在文件若已有内存覆盖
         * （export 消隐等），偏移以覆盖后的文本为准，改写叠在其上；否则
         * 从磁盘整读。同一文件的多条 FixIt 按偏移降序应用（先改后段、先段
         * 偏移保持有效），与已应用编辑重叠或越界的条目被丢弃。被改写文件
         * 的原覆盖条目被替换，未触及文件的覆盖原样保留。
         */
        /** @brief 两条 FixIt 是否同一位置同一替换（跨诊断去重判据）。 */
        [[nodiscard]]
        inline auto same_fixit(const FixIt& a, const FixIt& b) -> bool
        {
            return a.file == b.file && a.begin == b.begin && a.end == b.end
                   && a.replacement == b.replacement;
        }

    } // namespace detail
} // namespace cg::clang
