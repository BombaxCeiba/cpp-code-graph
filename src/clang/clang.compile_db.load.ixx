

/**
 * @module cg.clang.compile_db.load
 *
 * @brief compile_commands.json 的装载与规范化主链 load，及按文件查命令的
 * find_command。
 */
export module cg.clang.compile_db.load;

import std;
import cg.util;
import cg.util.cstd;
import cg.clang.driver;
import cg.clang.compile_db.model;
import cg.clang.compile_db.tokens;
import cg.clang.compile_db.expand;
import cg.clang.compile_db.bmi;

export namespace cg::clang
{

    /**
     * @brief 加载一份 compile_commands.json。
     *
     * 解析出的绝对 "file" 不在任一 root_filters 之下的条目被丢弃；空 span
     * 保留全部。相对 "directory" 字段按数据库文件所在目录解析；相对
     * "file" 字段按其条目的 directory 解析。畸形条目（无 "file"、无
     * "command"/"arguments"）跳过而非致命。文件不可读或不是 JSON 数组时
     * 抛 std::runtime_error。
     *
     * @internal
     * 条目整形要点：两种形状（"command" 单条 Windows 命令串 / "arguments"
     * 已切分 argv）都保留 argv[1:]，编译器 token（argv[0]）单独留作 BMI
     * 生产者身份。"@file" 响应 token 必须展开（CMake 4.4 以此写模块单元的
     * 模块依赖映射）：驱动器把它们拼进命令行、libclang 不能——且它们携带
     * 的 flag（-fmodule-file=NAME=path.pcm）恰是命名模块 TU 能解析的命脉。
     * 展开一层即可（嵌套 @ 文件不存在）。"-x `<language>`" 的值不是路径、
     * 原样保留；"-fmodule-output=" 只命名构建会写的 BMI——从重放 argv
     * 丢弃但记录进 module_output。非解析参数剥离："-c" 及其后的输入文件
     * （杂散位置路径会令 libclang 以 CXError_ASTReadError 失败）、"-o"
     * 及其路径、连写 "-o`<path>`"、"@file"（未展开的 @token 会被当作第二个
     * 输入文件）。带路径值的 flag（"-I"、"-isystem"…）按条目目录解析：
     * libclang 按进程工作目录解释相对 include 路径，那不是记录的编译目录。
     * 裸命令行上的模块 flag 与 rsp 孪生同法捕获进结构化字段。CMake 的
     * 合成重扫条目（@synth_N）携带 "--precompile"：它命令驱动器产出 BMI，
     * libclang 做不到（rc=4）——索引的是接口单元的解析，该 flag 是构建
     * 模式而非解析输入。连写 "-fmodule-file=NAME=path" 是 CMake/Ninja 记录
     * 的形状；从展开 @modmap 拼来的 token 已解析过、须去重。条目自身的
     * 主文件规范化一次（合成重扫条目以裸位置参数命名输入、无 -c，杂散
     * 位置输入即两个输入文件的失败——恰好丢弃该 token、他者不动）。
     * note_feed 消化的 joined_value 形如 "NAME=path"，调用方随后解析路径
     * 并把 .pcm 修正为解析后的拼写。
     * file 先对 directory 解析、再二者规范化入库：norm_path 的输出绝不可
     * 再喂给文件系统拼接（且其自身必须幂等）。
     */
    [[nodiscard]]
    auto load(
        std::string_view             db_path,
        std::span<const std::string> root_filters)
        -> std::vector<CompileCommand>
    {
        namespace fs = std::filesystem;
        namespace json = cg::util::json;

        const std::optional<std::string> db_text =
            cg::util::read_text_file(std::string{db_path});
        if (!db_text.has_value())
        {
            throw std::runtime_error(
                std::format("cannot open compile database: {}", db_path));
        }

        const fs::path db_dir =
            fs::path{std::string_view{db_path}}.parent_path();
        const std::string db_dir_str = db_dir.generic_string();

        const json::Document  doc = json::Document::parse(*db_text);
        const json::ValueView root = doc.root();
        if (!root.is_array())
        {
            throw std::runtime_error(
                std::format(
                    "compile database is not a JSON array: {}",
                    db_path));
        }

        std::vector<CompileCommand> out;
        const std::size_t           count = root.size();
        for (std::size_t i = 0; i < count; ++i)
        {
            const auto entry_view = root.at(i);
            if (!entry_view || !entry_view->is_object())
            {
                continue;
            }
            const json::ValueView entry = *entry_view;

            const std::optional<std::string> file_field =
                detail::json_string_field(entry, "file");
            if (!file_field)
            {
                continue;
            }

            std::string              compiler;
            std::vector<std::string> tokens;
            if (const auto args = entry.get("arguments");
                args && args->is_array())
            {
                if (const auto v = args->at(0); v && v->is_string())
                {
                    compiler = std::string{*v->as_string()};
                }
                const std::size_t n = args->size();
                for (std::size_t j = 1; j < n; ++j)
                {
                    if (const auto v = args->at(j); v && v->is_string())
                    {
                        tokens.emplace_back(*v->as_string());
                    }
                }
            }
            else if (
                const auto cmd = detail::json_string_field(entry, "command"))
            {
                tokens = cg::util::tokenize_command(*cmd);
                if (!tokens.empty())
                {
                    compiler = std::move(tokens.front());
                    tokens.erase(tokens.begin());
                }
            }
            else
            {
                continue;
            }

            std::string dir =
                detail::json_string_field(entry, "directory").value_or(".");
            if (fs::path{dir}.is_relative())
            {
                dir = (fs::path{db_dir_str} / dir)
                          .lexically_normal()
                          .generic_string();
            }
            while (dir.size() > 3 && dir.back() == '/')
            {
                dir.pop_back(); // lexically_normal 保留尾部分隔符
            }

            const auto resolve_arg = [&](const std::string& value)
            {
                if (fs::path{value}.is_absolute())
                {
                    return cg::util::norm_path(value);
                }
                return cg::util::norm_path((fs::path{dir} / value)
                                               .lexically_normal()
                                               .generic_string());
            };
            const std::optional<std::string> output_field =
                detail::json_string_field(entry, "output");
            // 条目级 PCH 门控：先看命令行，再看响应文件内容。只有涉及 PCH
            // 的条目才允许后续归一化触碰 argv（零变化门禁）。
            const bool pch_hint = detail::has_pch_marker(tokens)
                                  || detail::rsp_has_pch_marker(tokens, dir);
            CompileCommand cc;
            cc.compiler = std::move(compiler);
            const auto note_feed = [&](std::string_view joined_value)
            {
                const std::size_t eq = joined_value.find('=');
                if (eq == std::string_view::npos || eq == 0
                    || eq + 1 >= joined_value.size())
                {
                    return;
                }
                cc.module_feeds.push_back(
                    ModuleFeed{
                        .name = std::string{joined_value.substr(0, eq)},
                        .pcm = std::string{joined_value.substr(eq + 1)}});
            };
            tokens = detail::expand_response_files(
                std::move(tokens),
                dir,
                pch_hint,
                cc,
                resolve_arg,
                note_feed);

            // ---- 外来 MSVC 条目的方言与系统头补齐 ----
            // 原生 cdb（cl.exe /c ...）没有 --driver-mode=cl 时，重放会走
            // GNU 方言、静默用 libc++ 建图。按编译器基名注入（放在编译器
            // token 之后，即 argv[1] 位置）；clang/clang++ 条目绝不触碰。
            const bool cl_family = detail::compiler_is_cl_family(cc.compiler);
            if (cl_family
                && std::ranges::find(tokens, "--driver-mode=cl")
                       == tokens.end())
            {
                tokens.insert(tokens.begin(), "--driver-mode=cl");
            }
            if (cl_family)
            {
                // clang-cl 只认规范 `/X` 与 `-X`：把 cl 的 `/x` 规范化，
                // 并让重复拼写只留一个。`-x` 不在此处理——在 clang-cl 里
                // 它是语言选项，只有原生 cl 才按 /X 对待（见下）。
                bool saw_ignore = false;
                if (std::ranges::any_of(
                        tokens,
                        detail::is_ignore_standard_include))
                {
                    std::vector<std::string> canonical;
                    canonical.reserve(tokens.size());
                    for (const std::string& t : tokens)
                    {
                        if (detail::is_ignore_standard_include(t))
                        {
                            if (!saw_ignore)
                            {
                                canonical.emplace_back("/X");
                            }
                            saw_ignore = true;
                            continue;
                        }
                        canonical.push_back(t);
                    }
                    tokens = std::move(canonical);
                }
            }
            // 外来原生 cl 条目的系统头补齐（第 3 层）：按 cl.exe 路径反推
            // 工具集版本 → vcvars 捕获完整有序 INCLUDE → 与 ccg msbuild
            // 第 1 层同形物化（/X + -imsvc + 兼容版本）。两件事必须解耦：
            //  - 兼容版本只依赖编译器路径，原始命令带不带 /X 都要追加，
            //    否则 /X（IgnoreStandardIncludePath）的 TU 会静默走错
            //    `#if _MSC_VER` 分支；
            //  - INCLUDE 物化才是 /X 条件性的：原始命令已带 /X（任意合法
            //    拼写）时 cl 本会忽略 INCLUDE，不得追加。
            if (cl_family && detail::compiler_is_native_cl(cc.compiler))
            {
                // 原生 cl 的 `-x` 就是 /X（cl 选项前缀不敏感）；clang-cl
                // 会把它当语言选项，必须规范化为 `/X`。共享拼写已在上面
                // 处理，这里只剩 `-x` 与去重。
                bool ignores_include =
                    std::ranges::find(tokens, "/X") != tokens.end();
                if (std::ranges::find(tokens, "-x") != tokens.end())
                {
                    ignores_include = true;
                    bool                     have_x = false;
                    std::vector<std::string> canonical;
                    canonical.reserve(tokens.size());
                    for (const std::string& t : tokens)
                    {
                        if (t == "-x")
                        {
                            if (!have_x)
                            {
                                canonical.emplace_back("/X");
                            }
                            have_x = true;
                            continue;
                        }
                        if (t == "/X")
                        {
                            if (have_x)
                            {
                                continue;
                            }
                            have_x = true;
                        }
                        canonical.push_back(t);
                    }
                    tokens = std::move(canonical);
                }
                const MsvcToolsetPath toolset =
                    parse_msvc_toolset_path(cc.compiler);
                if (toolset.version.empty())
                {
                    // 连工具集线索都没有：只做方言修正，并让自动探测状态
                    // 可见（/X 时自动探测已被关掉，无需标注）。
                    if (!ignores_include)
                    {
                        cc.system_includes_auto = true;
                    }
                }
                else
                {
                    if (const std::string compat =
                            msvc_compat_version(toolset.version);
                        !compat.empty())
                    {
                        tokens.push_back(
                            std::format(
                                "-fms-compatibility-version={}",
                                compat));
                    }
                    if (!ignores_include)
                    {
                        const MsvcIncludeCapture captured =
                            capture_msvc_include_env(cc.compiler, dir);
                        if (captured.ok)
                        {
                            tokens.emplace_back("/X");
                            // INCLUDE 是 ';' 分隔的完整有序集合；空段跳过。
                            for (const std::string_view item :
                                 cg::util::split(captured.include_env, ';'))
                            {
                                if (!item.empty())
                                {
                                    tokens.emplace_back("-imsvc");
                                    tokens.emplace_back(
                                        cg::util::to_forward_slashes(item));
                                }
                            }
                        }
                        else
                        {
                            cc.system_includes_auto = true;
                        }
                    }
                }
            }
            // 模板模式等效旗标：仅原生 cl.exe 著条目——MSVC 默认（未显式
            // 声明 /permissive- 或 /Zc:twoPhase*）是延迟解析，/Zc:twoPhase-
            // 是该语义在 clang-cl 里的官方拼写（-fdelayed-template-parsing
            // 的别名）。clang-cl 自著条目忠实保留其自身语义不加（ccg
            // msbuild 翻译产物已在生成 cdb 时落旗标，同样不再补）。补在
            // 装载期的内存副本（外来 cdb 文件绝不改写），db 记录的 argv
            // 可见本旗标。
            if (cl_family && detail::compiler_is_native_cl(cc.compiler)
                && !detail::template_mode_declared(tokens))
            {
                tokens.emplace_back("/Zc:twoPhase-");
            }

            // 宏定义值的空白兜底（语义见 detail::normalize_macro_values）：
            // 重放 argv 与 digest 因此得到确定性的干净形态，任何来源的
            // 编译数据库（msbuild 采集/手写/外部生成）都受益。
            detail::normalize_macro_values(tokens);

            const std::string main_norm = cg::util::norm_path(
                [&]
                {
                    fs::path p{*file_field};
                    if (p.is_relative())
                    {
                        p = fs::path{dir} / p;
                    }
                    return p.lexically_normal().generic_string();
                }());
            const bool pch_involved =
                pch_hint || detail::has_pch_marker(tokens)
                || detail::implicit_pch_via_include(tokens, dir);
            cc.pch_involved = pch_involved;
            if (pch_involved)
            {
                // ---- PCH 感知解析 ----
                // 唯一目标：把构建系统记录的 PCH 语义完整搬进结构化字段，并
                // 让消费者 argv 对 libclang 可重放。`-Xclang <arg>` 是原子
                // 转发对；`-Xclang -include -Xclang V` 是两级转发，V 是路径
                // 值、按条目目录绝对化。绝不把转发 token 当顶层旗标消费。
                const bool cl_mode =
                    std::ranges::find(tokens, "--driver-mode=cl")
                        != tokens.end()
                    || std::ranges::any_of(
                        tokens,
                        [](const std::string& a)
                        {
                            return a.starts_with("/Yc") || a.starts_with("/Yu")
                                   || a.starts_with("/Fp");
                        })
                    // cc.compiler 是 argv[0] 的独立副本（原 token 已
                    // 移入此处）。
                    || detail::compiler_is_cl_family(cc.compiler);
                cc.cl_mode = cl_mode;
                bool        emit_pch = false;
                bool        header_mode = false;
                bool        yc_set = false;
                std::string out_flag;
                std::string fp_path;
                std::size_t j = 0;
                while (j < tokens.size())
                {
                    const std::string t = tokens[j];
                    if (t == "-Xclang" && j + 1 < tokens.size())
                    {
                        const std::string v = tokens[j + 1];
                        if (v == "-include" || v == "-include-pch")
                        {
                            cc.argv.push_back("-Xclang");
                            cc.argv.push_back(v);
                            j += 2;
                            std::string raw;
                            if (j < tokens.size() && tokens[j] == "-Xclang"
                                && j + 1 < tokens.size())
                            {
                                raw = tokens[j + 1];
                                j += 2;
                            }
                            else if (
                                j < tokens.size()
                                && !detail::is_gnu_flag_token(tokens[j]))
                            {
                                raw = tokens[j];
                                ++j;
                            }
                            const std::string abs =
                                raw.empty() ? std::string{} : resolve_arg(raw);
                            if (!abs.empty())
                            {
                                cc.argv.push_back("-Xclang");
                                cc.argv.push_back(abs);
                            }
                            if (v == "-include-pch")
                            {
                                cc.pch_use = true;
                                cc.pch_path = abs;
                            }
                            else
                            {
                                cc.forced_includes.push_back(abs);
                            }
                            continue;
                        }
                        if (v == "-emit-pch")
                        {
                            emit_pch = true;
                        }
                        cc.argv.push_back("-Xclang");
                        cc.argv.push_back(v);
                        j += 2;
                        continue;
                    }
                    if (t == "-x" && j + 1 < tokens.size())
                    {
                        if (tokens[j + 1] == "c++-header"
                            || tokens[j + 1] == "c-header")
                        {
                            header_mode = true;
                        }
                        cc.argv.push_back(t);
                        cc.argv.push_back(tokens[j + 1]);
                        j += 2;
                        continue;
                    }
                    if (t == "-emit-pch")
                    {
                        emit_pch = true;
                        cc.argv.push_back(t);
                        ++j;
                        continue;
                    }
                    if (t == "-include")
                    {
                        std::string raw;
                        if (const auto v = detail::next_token(tokens, j))
                        {
                            raw = *v;
                        }
                        if (!raw.empty())
                        {
                            const std::string abs = resolve_arg(raw);
                            cc.forced_includes.push_back(abs);
                            // 转发形式绕过 clang 对同名 .pch 的隐式加载
                            // （隐式 PCH 与 DPR 硬冲突，实测 rc=4）。
                            cc.argv.push_back("-Xclang");
                            cc.argv.push_back("-include");
                            cc.argv.push_back("-Xclang");
                            cc.argv.push_back(abs);
                        }
                        ++j;
                        continue;
                    }
                    if (t == "-include-pch" || t.starts_with("-include-pch="))
                    {
                        std::string raw;
                        if (t == "-include-pch")
                        {
                            if (const auto v = detail::next_token(tokens, j))
                            {
                                raw = *v;
                            }
                        }
                        else
                        {
                            raw = t.substr(
                                std::string_view{"-include-pch="}.size());
                        }
                        cc.pch_use = true;
                        cc.pch_path =
                            raw.empty() ? std::string{} : resolve_arg(raw);
                        cc.argv.push_back("-Xclang");
                        cc.argv.push_back("-include-pch");
                        if (!cc.pch_path.empty())
                        {
                            cc.argv.push_back("-Xclang");
                            cc.argv.push_back(cc.pch_path);
                        }
                        ++j;
                        continue;
                    }
                    if (t == "-o")
                    {
                        std::string raw;
                        if (const auto v = detail::next_token(tokens, j))
                        {
                            raw = *v;
                        }
                        if (!raw.empty())
                        {
                            out_flag = resolve_arg(raw);
                        }
                        ++j;
                        continue;
                    }
                    if (t.starts_with("-o") && t.size() > 2)
                    {
                        out_flag = resolve_arg(t.substr(2));
                        ++j;
                        continue;
                    }
                    if (t == "-c")
                    {
                        if (detail::gnu_value_follows(tokens, j))
                        {
                            ++j;
                        }
                        ++j;
                        continue;
                    }
                    if (t == "--precompile")
                    {
                        ++j;
                        continue;
                    }
                    if (t.starts_with('@'))
                    {
                        detail::note_rsp_module_map(cc, dir, t);
                        ++j;
                        continue;
                    }
                    if (t.starts_with("-fmodule-output="))
                    {
                        detail::note_module_output(cc, resolve_arg, t);
                        ++j;
                        continue;
                    }
                    if (detail::try_module_file_flag(cc, resolve_arg, t))
                    {
                        ++j;
                        continue;
                    }
                    if (cl_mode)
                    {
                        if (t == "/c")
                        {
                            ++j;
                            continue;
                        }
                        if (!detail::is_cl_flag_token(t)
                            && resolve_arg(t) == main_norm)
                        {
                            ++j; // 条目自身的输入：解析器另行传入
                            continue;
                        }
                        if (detail::is_cl_separate_value_flag(t))
                        {
                            std::string value;
                            bool        have = false;
                            if (detail::cl_value_follows(tokens, j))
                            {
                                value = tokens[j + 1];
                                have = true;
                                ++j;
                            }
                            if (have)
                            {
                                if (t == "/Fp")
                                {
                                    fp_path = resolve_arg(value);
                                }
                                else if (t == "/Fo" || t == "/Fd")
                                {
                                    // 构建产物：重定向由影子构建负责。
                                }
                                else if (t == "/FI")
                                {
                                    const std::string abs = resolve_arg(value);
                                    cc.forced_includes.push_back(abs);
                                    cc.argv.push_back(t);
                                    cc.argv.push_back(abs);
                                }
                                else if (t == "/I" || t == "/external:I")
                                {
                                    cc.argv.push_back(t);
                                    cc.argv.push_back(resolve_arg(value));
                                }
                                else
                                {
                                    cc.argv.push_back(t);
                                    cc.argv.push_back(value);
                                }
                            }
                            ++j;
                            continue;
                        }
                        std::optional<std::string> fi_value =
                            detail::cl_joined_value(t, "/external:I");
                        if (!fi_value.has_value())
                        {
                            fi_value = detail::cl_joined_value(t, "/FI");
                        }
                        if (fi_value.has_value())
                        {
                            const std::string abs = resolve_arg(*fi_value);
                            if (t.starts_with("/FI"))
                            {
                                cc.forced_includes.push_back(abs);
                            }
                            cc.argv.push_back(
                                std::format(
                                    "{}{}",
                                    t.starts_with("/FI") ? "/FI"
                                                         : "/external:I",
                                    abs));
                            ++j;
                            continue;
                        }
                        if (const auto v = detail::cl_joined_value(t, "/I"))
                        {
                            cc.argv.push_back(
                                std::format("/I{}", resolve_arg(*v)));
                            ++j;
                            continue;
                        }
                        if (const auto v = detail::cl_joined_value(t, "/Fp"))
                        {
                            fp_path = resolve_arg(*v);
                            ++j;
                            continue;
                        }
                        if (detail::cl_joined_value(t, "/Fo").has_value()
                            || detail::cl_joined_value(t, "/Fd").has_value())
                        {
                            ++j;
                            continue;
                        }
                        if (const auto v = detail::cl_joined_value(t, "/Yc"))
                        {
                            yc_set = true;
                            cc.pch_header = *v;
                            cc.argv.push_back(t);
                            ++j;
                            continue;
                        }
                        if (const auto v = detail::cl_joined_value(t, "/Yu"))
                        {
                            cc.pch_use = true;
                            cc.pch_header = *v;
                            ++j;
                            continue;
                        }
                        if (t == "/Yc" || t == "/Yu")
                        {
                            // 分写值守卫（cl 头值型）：源文件扩展名绝不能被
                            // 吞（v1 的 AdditionalOptions=/Yu 把下一条 TU 吃
                            // 掉的 bug）。
                            std::string value;
                            if (detail::cl_header_value_follows(tokens, j))
                            {
                                value = tokens[j + 1];
                                ++j;
                            }
                            if (t == "/Yc")
                            {
                                yc_set = true;
                                cc.pch_header = value;
                                cc.argv.push_back(t);
                            }
                            else
                            {
                                cc.pch_use = true;
                                cc.pch_header = value;
                            }
                            ++j;
                            continue;
                        }
                        if (t == "/showIncludes" || t == "/MP" || t == "/Gm"
                            || t == "/GL" || t == "/analyze" || t == "/WX"
                            || t == "/FS")
                        {
                            ++j;
                            continue;
                        }
                        cc.argv.push_back(t);
                        ++j;
                        continue;
                    }
                    if (detail::is_gnu_include_flag(t))
                    {
                        detail::push_include_token(
                            cc,
                            tokens,
                            resolve_arg,
                            t,
                            j);
                        ++j;
                        continue;
                    }
                    if (detail::is_joined_include_flag(t))
                    {
                        detail::push_joined_include(cc, resolve_arg, t);
                        ++j;
                        continue;
                    }
                    if (!detail::is_gnu_flag_token(t)
                        && resolve_arg(t) == main_norm)
                    {
                        ++j;
                        continue;
                    }
                    if (t.starts_with("-Werror"))
                    {
                        ++j; // 告警不影响 AST 形状，影子构建不应假失败
                        continue;
                    }
                    cc.argv.push_back(t);
                    ++j;
                }
                if (cl_mode)
                {
                    if (yc_set)
                    {
                        cc.pch_create = true;
                        cc.pch_output = fp_path;
                    }
                    else if (!fp_path.empty())
                    {
                        cc.pch_path = fp_path;
                        cc.pch_use = true;
                    }
                }
                else
                {
                    cc.pch_output = out_flag;
                    cc.pch_create =
                        header_mode
                        && (emit_pch || detail::is_pch_suffix(out_flag));
                }
                if (cc.pch_output.empty() && output_field.has_value())
                {
                    cc.pch_output = resolve_arg(*output_field);
                }
            }
            else
                for (std::size_t j = 0; j < tokens.size(); ++j)
                {
                    const std::string& t = tokens[j];
                    if (t == "-c")
                    {
                        if (detail::gnu_value_follows(tokens, j))
                        {
                            ++j;
                        }
                        continue;
                    }
                    if (t == "-o")
                    {
                        ++j;
                        continue;
                    }
                    if (t.starts_with("-o") && t.size() > 2)
                    {
                        continue; // 连写形式 "-opath"
                    }
                    if (t == "--precompile")
                    {
                        continue;
                    }
                    if (!detail::is_gnu_flag_token(t)
                        && resolve_arg(t) == main_norm)
                    {
                        continue; // 条目自身的输入，位置拼写
                    }
                    if (t.starts_with('@'))
                    {
                        detail::note_rsp_module_map(cc, dir, t);
                        continue; // 驱动器响应文件（如 @…modmap）
                    }
                    if (t.starts_with("-fmodule-output="))
                    {
                        detail::note_module_output(cc, resolve_arg, t);
                        continue; // 构建产物，非解析输入
                    }
                    if (detail::try_module_file_flag(cc, resolve_arg, t))
                    {
                        continue;
                    }
                    if (detail::is_gnu_include_flag(t))
                    {
                        detail::push_include_token(
                            cc,
                            tokens,
                            resolve_arg,
                            t,
                            j);
                        continue;
                    }
                    if (detail::is_joined_include_flag(t))
                    {
                        detail::push_joined_include(cc, resolve_arg, t);
                        continue;
                    }
                    cc.argv.push_back(t);
                }

            fs::path file_path{*file_field};
            if (file_path.is_relative())
            {
                file_path = fs::path{dir} / file_path;
            }

            cc.directory = cg::util::norm_path(dir);
            cc.file = cg::util::norm_path(
                file_path.lexically_normal().generic_string());

            bool keep = root_filters.empty();
            for (const std::string& filter : root_filters)
            {
                if (detail::path_under(cc.file, cg::util::norm_path(filter)))
                {
                    keep = true;
                    break;
                }
            }
            if (keep)
            {
                out.push_back(std::move(cc));
            }
        }
        return out;
    }

    /**
     * @brief 按文件查找命令：先精确规范化路径匹配，再带路径段边界的后缀
     * 匹配（以不同绝对前缀记录的数据库仍能命中）。
     *
     * @return 无匹配时为 nullptr。
     */
    [[nodiscard]]
    auto find_command(
        const std::vector<CompileCommand>& commands,
        std::string_view                   file) -> const CompileCommand*
    {
        const std::string needle = cg::util::norm_path(file);
        for (const CompileCommand& cc : commands)
        {
            if (cc.file == needle)
            {
                return &cc;
            }
        }
        for (const CompileCommand& cc : commands)
        {
            if (cc.file.size() > needle.size() && cc.file.ends_with(needle)
                && cc.file[cc.file.size() - needle.size() - 1] == '/')
            {
                return &cc;
            }
        }
        return nullptr;
    }
} // namespace cg::clang
