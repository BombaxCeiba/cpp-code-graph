

/**
 * @module cg.clang.compile_db.tokens
 *
 * @brief 命令行 token 判定域：路径归属、PCH 旗标/源扩展/cl 与 GNU
 * 旗标判定、编译器家族识别、隐式 PCH 与 rsp 内 PCH 探测。
 */
export module cg.clang.compile_db.tokens;

import std;
import cg.util;
import cg.util.cstd;
import cg.clang.driver;
import cg.clang.compile_db.model;

export namespace cg::clang
{

    namespace detail
    {

        /**
         * @brief 带路径段边界的绝对路径前缀测试。
         *
         * @internal 根不得匹配仅共享前缀的更长姊妹路径。模块内部使用，不导出。
         */
        [[nodiscard]]
        auto path_under(std::string_view path, std::string_view root) -> bool
        {
            if (root.empty())
            {
                return path.empty();
            }
            if (path == root)
            {
                return true;
            }
            if (!path.starts_with(root))
            {
                return false;
            }
            return root.back() == '/' || path[root.size()] == '/';
        }

        /**
         * @brief PCH 标记 token 测试（消费或生产）。
         *
         * @internal 只认旗标本身，不认路径值——`-include-pch` 的直接与
         * `-Xclang` 转发拼写都表现为独立 token，故词法层即可发现。`/Yc` /
         * `/Yu` / `/Fp` 是 cl 的 PCH 旗标；`-emit-pch` 是 GNU 生产旗标。
         */
        [[nodiscard]]
        auto pch_marker_token(std::string_view t) -> bool
        {
            return t == "-include-pch" || t.starts_with("-include-pch=")
                   || t == "-emit-pch" || t.starts_with("/Yc")
                   || t.starts_with("/Yu") || t.starts_with("/Fp");
        }

        /**
         * @brief C/C++ 翻译单元扩展名测试（大小写不敏感）。
         *
         * @internal 判定收编至 util 的扩展分类单点（is_tu_input_file——集合
         * 与折叠规则只此一处）：cl 的分写 `/Yu`/`/Yc` 值只有在不是源文件时
         * 才是头名，否则会吞掉下一条 TU。
         */
        [[nodiscard]]
        inline auto has_source_ext_token(std::string_view token) -> bool
        {
            return cg::util::is_tu_input_file(token);
        }

        /**
         * @brief "下一 token 是值"守卫族（分写旗标的吞值判定，clang 层单点）。
         *
         * 适配层/装载层/PCH/BMI 四处剥离与重放循环曾各自内联这套判定，口径
         * 漂移即吞值或漏吞。判据是三型（刻意不成单函数——各型的旗标世界与
         * 排除面不同）：
         * 1. cl 型——cl 世界旗标以 '/' 或 '-' 开头（cl 的路径值带盘符，
         *    不以 '/' 开头），下一 token 不是旗标即为值（输出路径类）；
         * 2. cl 头值型——在 cl 型之上再排除源文件扩展（PCH/模块类旗标：值
         *    是头名或产物路径，源文件是解析/重放的输入，绝不吞）；
         * 3. gnu 型——gnu 世界旗标只以 '-' 开头，下一 token 不以 '-' 开头
         *    即为值（-include 类；'/' 开头的路径在 gnu 世界是值）。
         */
        ///@{
        /** @brief token 是否 cl 世界旗标（'/' 或 '-' 开头）。 */
        [[nodiscard]]
        inline auto is_cl_flag_token(std::string_view token) -> bool
        {
            return token.starts_with('/') || token.starts_with('-');
        }

        /** @brief token 是否 gnu 世界旗标（'-' 开头）。 */
        [[nodiscard]]
        inline auto is_gnu_flag_token(std::string_view token) -> bool
        {
            return token.starts_with('-');
        }

        /** @brief cl 型：位置 i 的下一 token
         * 存在且不是旗标（输出路径类分写值）。 */
        [[nodiscard]]
        inline auto cl_value_follows(
            std::span<const std::string> argv,
            std::size_t                  i) -> bool
        {
            return i + 1 < argv.size() && !is_cl_flag_token(argv[i + 1]);
        }

        /** @brief cl 头值型：cl 型守卫且下一 token
         * 不是源文件（PCH/模块类分写值）。 */
        [[nodiscard]]
        inline auto cl_header_value_follows(
            std::span<const std::string> argv,
            std::size_t                  i) -> bool
        {
            return cl_value_follows(argv, i)
                   && !has_source_ext_token(argv[i + 1]);
        }

        /** @brief gnu 型：位置 i 的下一 token 存在且不以 '-' 开头（-include
         * 类分写值）。 */
        [[nodiscard]]
        inline auto gnu_value_follows(
            std::span<const std::string> argv,
            std::size_t                  i) -> bool
        {
            return i + 1 < argv.size() && !is_gnu_flag_token(argv[i + 1]);
        }
        ///@}

        /** @brief 编译器 token 的基名（去目录、小写、去 .exe）。 */
        [[nodiscard]]
        inline auto compiler_basename(std::string_view compiler) -> std::string
        {
            std::string base =
                cg::util::ascii_lower(cg::util::path_basename(compiler));
            if (base.ends_with(".exe"))
            {
                base.resize(base.size() - 4);
            }
            return base;
        }

        /**
         * @brief 编译器是否 cl 家族（cl/clang-cl）。
         *
         * @internal 只有这两个名字才注入 cl 方言；clang/clang++ 条目逐字节
         * 不变（硬门禁）。
         */
        [[nodiscard]]
        inline auto compiler_is_cl_family(std::string_view compiler) -> bool
        {
            const std::string base = compiler_basename(compiler);
            return base == "cl" || base == "clang-cl";
        }

        /** @brief 编译器是否原生 MSVC cl（而非 clang-cl）。 */
        [[nodiscard]]
        inline auto compiler_is_native_cl(std::string_view compiler) -> bool
        {
            return compiler_basename(compiler) == "cl";
        }

        /**
         * @brief token 是否为 cl 的 IgnoreStandardIncludePath（`/X`）。
         *
         * @internal cl 接受 '/' 与 '-' 前缀且该选项大小写不敏感。clang-cl 只
         * 认规范 `/X` 与 `-X`（实测 `/x` 被当作输入文件），因此共享判定接受
         * `/X`、`/x`、`-X`；`-x` 只在确知是原生 cl 时才按 /X 对待（在
         * clang-cl 里它是语言选项，不能改写）。
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

        [[nodiscard]]
        auto has_pch_marker(std::span<const std::string> tokens) -> bool
        {
            for (const std::string& t : tokens)
            {
                if (pch_marker_token(t))
                {
                    return true;
                }
            }
            return false;
        }

        /** @brief 头文件路径旁是否存在同名 .pch/.gch（相对值按 dir 解析）。 */
        [[nodiscard]]
        inline auto sidecar_pch_exists(
            const std::string& dir,
            std::string_view   raw) -> bool
        {
            namespace fs = std::filesystem;
            if (raw.empty())
            {
                return false;
            }
            std::string path{raw};
            if (fs::path{path}.is_relative())
            {
                path =
                    (fs::path{dir} / path).lexically_normal().generic_string();
            }
            std::error_code ec;
            return fs::is_regular_file(fs::path{path + ".pch"}, ec)
                   || fs::is_regular_file(fs::path{path + ".gch"}, ec);
        }

        /**
         * @brief 隐式 PCH 探测：`-include`（含 `-Xclang -include` 转发）指向的
         *        头文件旁存在 `<header>.pch` / `<header>.gch`。
         *
         * @internal clang 驱动器只为顶层 `-include` 注入隐式 PCH（`-###` 实测：
         * `-Xclang -include` 不会），故命中后 PCH 感知解析把该旗标改写为
         * `-Xclang -include -Xclang <abs>` 即可绕开自动加载。相对值按条目目录
         * （记录的 CWD）解析。源码首个 `#include` 不触发 clang 的隐式加载
         * （实测 `-###` 无 `-include-pch`），无需探测源文件。
         */
        [[nodiscard]]
        auto implicit_pch_via_include(
            std::span<const std::string> tokens,
            const std::string&           dir) -> bool
        {
            for (std::size_t i = 0; i < tokens.size(); ++i)
            {
                const std::string& t = tokens[i];
                if (t == "-Xclang" && i + 3 < tokens.size()
                    && tokens[i + 1] == "-include"
                    && tokens[i + 2] == "-Xclang")
                {
                    if (sidecar_pch_exists(dir, tokens[i + 3]))
                    {
                        return true;
                    }
                    i += 3;
                    continue;
                }
                if (t == "-include" && gnu_value_follows(tokens, i))
                {
                    if (sidecar_pch_exists(dir, tokens[i + 1]))
                    {
                        return true;
                    }
                    ++i;
                }
            }
            return false;
        }

        /**
         * @brief 响应文件里是否出现 PCH 标记（只读探测；不改变展开语义）。
         *
         * @internal 必须在展开前决定条目是否走 PCH 归一化，而 cl 的
         * `AdditionalOptions` 可能整体记录在 @file 中。探测只读文件、按同一
         * tokenizer 切分并递归一层；读不到的文件忽略（展开阶段会按原语义
         * 丢弃）。
         */
        [[nodiscard]]
        auto rsp_has_pch_marker(
            std::span<const std::string> tokens,
            const std::string&           dir,
            std::size_t                  depth = 0) -> bool
        {
            namespace fs = std::filesystem;
            if (depth > 16)
            {
                return false;
            }
            for (const std::string& t : tokens)
            {
                if (!t.starts_with('@') || t.size() < 2)
                {
                    continue;
                }
                std::string rsp = t.substr(1);
                if (fs::path{rsp}.is_relative())
                {
                    rsp = (fs::path{dir} / rsp)
                              .lexically_normal()
                              .generic_string();
                }
                const auto rsp_text = cg::util::read_text_file(rsp);
                if (!rsp_text.has_value())
                {
                    continue;
                }
                const std::vector<std::string> inner =
                    cg::util::tokenize_command(*rsp_text);
                if (has_pch_marker(inner)
                    || rsp_has_pch_marker(inner, dir, depth + 1))
                {
                    return true;
                }
            }
            return false;
        }

        /** @brief 路径值解析回调：绝对值规范化，相对值按条目目录解析后
         *  规范化。 */
        using ArgResolver = std::function<std::string(const std::string&)>;

        /** @brief 模块投喂登记回调：消化 "NAME=path" 形式的联合值追加进
         *  投喂表；缺 '=' 或名/路径段为空时不登记。 */
        using FeedNotifier = std::function<void(std::string_view)>;

    } // namespace detail
} // namespace cg::clang
