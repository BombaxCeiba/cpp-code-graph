

/**
 * @module cg.clang.tu.parse
 *
 * @brief 翻译单元解析域：parse
 * 主链（命令装配、叠加清洗、单次解析与诊断收集）与主文件判定。
 */
export module cg.clang.tu.parse;

import std;
import cg.util;
import cg.util.cstd;
import cg.clang.capi;
import cg.clang.driver;
import cg.clang.compile_db;
import cg.clang.tu.lex;
import cg.clang.tu.imports;
import cg.clang.tu.argv;
import cg.clang.tu.scrub;

export namespace cg::clang
{

    /**
     * @brief 以 argv 编译器开关（无 argv[0]）解析 file（绝对或相对进程
     * 工作目录）。
     *
     * 可恢复诊断绝不抛出；仅硬索引/解析失败抛 std::runtime_error。追加
     * driver_supplement，使真实编译数据库条目的解析与其记录的编译器所
     * 见完全一致。
     *
     * @internal
     * `module_tu`（编译数据库条目的 cg::clang::is_module_tu）把上述 export
     * 消隐覆盖作为 unsaved-file 覆盖应用：libclang 把 `export namespace`/
     * `export {}`/逐声明 `export` 下的一切藏进不可访问的 UnexposedDecl，
     * 没有覆盖时这些文件产出零符号。经典 TU 走默认值、按字节精确解析。
     * export 消隐只骑模块模式解析；import 改写（混合 TU 伞头、头单元
     * 降级）也可以命中经典消费者 TU，故不被 module_tu 门控。诊断收集
     * 上限 8 条——只有第一条错误拼写会进失败报告；CCG_DEBUG_DIAGS 选择
     * 回完整列表。首轮诊断命中 "illegal character encoding"（宽/Unicode
     * 字面量内的系统 ANSI 码页坏字节）时，经 detail::rewrite_illegal_
     * encoding 把字面量内不可解码字节转八进制转义后重解析一次（不落
     * 盘、一次上限、仅 cl 方言）；首轮诊断命中 cl 方言宽松特化家族且带
     * FixIt 时，经 detail::rewrite_lenient_specializations 做一次诊断
     * 驱动的内存源码改写并重解析（不落盘、一次上限），其基座为消毒后
     * 的覆盖表（两机制链式叠加、各自一次重解析上限）。
     */
    [[nodiscard]]
    auto parse(
        const Index&                    index,
        const std::string&              file,
        const std::vector<std::string>& argv,
        bool                            module_tu = false,
        const ModuleOverlay*            module_overlay = nullptr,
        bool                            degraded = false) -> ParsedTu
    {
        ParsedTu                 out;
        std::vector<UnsavedFile> overlay;
        const bool rewrite = module_overlay != nullptr
                             && (module_overlay->std_umbrella
                                 || module_overlay->header_unit_includes
                                 || module_overlay->demodule
                                 || !module_overlay->dropped_modules.empty());
        if (module_tu || rewrite)
        {
            const std::string src = read_source(file);
            if (!src.empty())
            {
                std::string blanked = module_tu ? export_overlay(src) : src;
                if (rewrite)
                {
                    blanked = import_rewrite_overlay(
                                  blanked,
                                  module_overlay->std_umbrella,
                                  module_overlay->header_unit_includes,
                                  module_overlay->demodule,
                                  module_overlay->dropped_modules)
                                  .text;
                }
                if (blanked != src)
                {
                    overlay.push_back(
                        UnsavedFile{
                            .filename = file,
                            .contents = std::move(blanked)});
                }
            }
        }
        std::vector<std::string> extra = driver_supplement(file, argv);
        // 记录 argv 之后的告警策略（绝不写回 cc.argv / parse_digest 喂进的
        // 摘要，否则每个指纹都会因引擎侧策略而失配）：
        // -Wno-nonportable-include-path / -Wno-nonportable-system-include-path
        // 压掉 include 路径大小写告警；在 -Werror 下这些告警会升级成错误，
        // 让本可恢复的 TU 整只失败（参考语料 11 个 TU 的实测根因之一）。两个
        // 开关在 GNU 与 --driver-mode=cl 两种方言下都被 libclang 的选项
        // 解析器接受，故不需要方言分支。-Wno-unknown-warning-option 放在
        // 二者之后：旧 clang 不认识 nonportable 开关时会发 unknown warning
        // option，-Werror 下那本身又是一条错误；该开关让未知 -Wno-* 静默。
        extra.emplace_back("-Wno-nonportable-include-path");
        extra.emplace_back("-Wno-nonportable-system-include-path");
        extra.emplace_back("-Wno-unknown-warning-option");
        // non-pod-varargs 是 clang 的 DefaultError（与 -Werror 无关）：非平凡
        // 类型传 varargs 的日志宏在 MSVC 下完全合法（ATL/日志上抛形态），
        // 索引语境只需把错误升级降回警告——警告本体保留（诊断只收集
        // Error 级，符号提取不受影响），两个方言都接受该开关。
        extra.emplace_back("-Wno-error=non-pod-varargs");
        // cl 方言在末尾追加 /WX-（后写胜出）：/WX 会把 libclang 解析入口
        // 固定追加的 -fno-spell-checking 触发的 unknown-argument 驱动警告
        // 提升成错误，压掉一层后下一层（argument unused during
        // compilation: '/c'）还会冒——逐警告 -Wno-* 打地鼠不可取，一次性
        // 解除全部升级。无 /WX 时 /WX- 是 no-op；GNU 方言不接受 /WX-，故
        // 严格门控在 cl 方言（loader/翻译层已保证 cl 条目带
        // --driver-mode=cl）。
        if (std::ranges::find(argv, "--driver-mode=cl") != argv.end())
        {
            extra.emplace_back("/WX-");
        }
        std::vector<std::string> replay = argv;
        replay.insert(replay.end(), extra.begin(), extra.end());
        // 引擎解析 + 编码感知的硬失败重试：宽/Unicode 字面量内的系统
        // ANSI 坏字节让 libclang 直接拒绝建 TU（ParseError，无诊断可看），
        // 此时盲扫主文件——字面量内确有坏点才消毒重试一次，否则原样
        // 重抛（其余硬失败口径不变）。诊断驱动的残余消毒（TU 建立、诊断
        // 点名被含头）见 detail::rewrite_illegal_encoding。
        // 实例化跳过形态按本 replay 判定一次：首轮与编码消毒重试共用；
        // 下方两个 detail 改写器的重试在同一 replay 上独立求值同一
        // 纯函数，形态必然一致。
        const bool skip_instantiations = detail::instantiation_skip_allowed(
            cg::util::cstd::get_env("CCG_SKIP_INSTANTIATIONS"),
            replay);
        std::exception_ptr first_failure;
        bool               parsed_ok = detail::parse_once(
            out,
            index,
            file,
            replay,
            overlay,
            !degraded,
            skip_instantiations,
            first_failure);
        if (!parsed_ok
            && std::ranges::find(argv, "--driver-mode=cl") != argv.end()
            && !cg::util::platform::ansi_input_charset().empty())
        {
            std::string buf;
            for (const UnsavedFile& u : overlay)
            {
                if (u.filename == file)
                {
                    buf = u.contents;
                    break;
                }
            }
            if (buf.empty())
            {
                buf = read_source(file);
            }
            detail::ScrubOutcome scrubbed = detail::scrub_source_encoding(buf);
            if (scrubbed.edits > 0)
            {
                std::erase_if(
                    overlay,
                    [&](const UnsavedFile& u) { return u.filename == file; });
                overlay.push_back(
                    UnsavedFile{
                        .filename = file,
                        .contents = std::move(scrubbed.text)});
                parsed_ok = detail::parse_once(
                    out,
                    index,
                    file,
                    replay,
                    overlay,
                    !degraded,
                    skip_instantiations,
                    first_failure);
                if (parsed_ok)
                {
                    out.encoding_scrub_applied = true;
                    out.encoding_scrub_edits =
                        static_cast<std::int64_t>(scrubbed.edits);
                    out.encoding_scrub_files = {file};
                }
            }
        }
        if (!parsed_ok)
        {
            std::rethrow_exception(first_failure); // 原始异常原样上抛
        }
        // 诊断上限开关进程内求值一次（每次 parse 都经过这里）；
        // 改环境变量需重启进程才生效。
        static const std::size_t diag_cap =
            cg::util::cstd::get_env("CCG_DEBUG_DIAGS").has_value() ? 0 : 8;
        out.errors = out.tu.diagnostics(DiagSeverity::Error, diag_cap, true);
        // 活基座：编码消毒（如有）就地更新覆盖表，随后的 FixIt 改写以
        // 最新基座链式叠加——编辑偏移始终对齐 clang 眼中的缓冲。
        std::vector<UnsavedFile> base = std::move(overlay);
        detail::rewrite_illegal_encoding(
            out,
            index,
            file,
            argv,
            replay,
            base,
            !degraded,
            diag_cap);
        detail::rewrite_lenient_specializations(
            out,
            index,
            file,
            argv,
            replay,
            base,
            !degraded,
            diag_cap);
        return out;
    }

    /**
     * @brief cursor 是否位于 tu 的主文件（而非被包含的头）内。
     *
     * @internal 比较经 norm_path：clang 按传入 parse() 的原样报告主文件
     * 路径，分隔符/大小写可能不同。
     */
    [[nodiscard]]
    auto in_main_file(const Tu& tu, const CXCursor& cursor) -> bool
    {
        const SourceLocation loc = location(cursor);
        if (loc.file.empty())
        {
            return false;
        }
        return cg::util::norm_path(loc.file) == cg::util::norm_path(tu.file());
    }
} // namespace cg::clang
