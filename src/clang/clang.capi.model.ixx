module;
#include <clang-c/Index.h> // 第三方头文件，仅限全局模块片段

/**
 * @module cg.clang.capi.model
 *
 * @brief 门面类型域：位置/范围/诊断/未保存文件/Tu 翻译单元/Index
 * 索引，及诊断构造与子游标访问器。引擎给出的字符串由 cg.clang.capi.api
 * 的 Str 持有（装载路径要用它，所以它住在分发表所在的单元）。
 */
export module cg.clang.capi.model;

import std;
import cg.clang.loader;
import cg.util; // 伞形模块；此处用的是 cg.util.guard（崩溃防护）
import cg.clang.capi.api;

export {
    using ::CX_BinaryOperatorKind;
    using ::CXCursor;
    using ::CXCursorKind;
}

export namespace cg::clang
{

    struct SourceLocation
    {
        std::string file;       // 无效位置时为空
        unsigned    line = 0;   // 1 起；未知时 0
        unsigned    column = 0; // 1 起；未知时 0
    };

    struct SourceExtent
    {
        SourceLocation begin;
        SourceLocation end; // 区间最后一个 token 之后的下一位置
    };

    /** @brief 与 CXDiagnosticSeverity 数值相同（0..4）。 */
    enum class DiagSeverity : std::uint8_t
    {
        Ignored = 0,
        Note = 1,
        Warning = 2,
        Error = 3,
        Fatal = 4,
    };

    /** @brief 镜像 CX_CXXAccessSpecifier（裸 C 枚举子不导出）。 */
    enum class Access : std::uint8_t
    {
        Invalid = 0,
        Public = 1,
        Protected = 2,
        Private = 3,
    };

    /**
     * @brief 镜像 CXErrorCode（裸 C 枚举子不导出）。
     *
     * @internal 失败分类以此键控：AstReadError 是静默的 BMI 拒绝类
     * （bmi-config-mismatch），Crashed 是被恢复的陈旧级联类。
     */
    enum class ParseErrc : std::uint8_t
    {
        Success = 0,
        Failure = 1,
        Crashed = 2,
        InvalidArguments = 3,
        AstReadError = 4,
    };

    /**
     * @brief 硬解析失败（clang_parseTranslationUnit2 返回非零，故无 TU
     * 存在、诊断无法收集）。
     *
     * @internal 携带原始错误码，调用方按码分类而非串匹配消息。
     */
    class ParseError : public std::runtime_error
    {
    public:
        ParseError(const std::string& message, ParseErrc code)
            : std::runtime_error(message), code_(code)
        {
        }
        [[nodiscard]]
        ParseErrc code() const noexcept
        {
            return code_;
        }

    private:
        ParseErrc code_;
    };

    /**
     * @brief 诊断自带的一条 FixItHint：把 [begin, end) 字节跨度替换为
     * replacement（begin == end 是纯插入）。
     *
     * @internal 偏移取自 clang_getSpellingLocation——与 clang 眼中的文件
     * 缓冲逐字节对齐（本仓库按二进制整读同一文件，两边一致）。跨文件的
     * 跨度（宏边界）无法应用到任何单个缓冲，收集时即被丢弃。
     */
    struct FixIt
    {
        std::string file;      // 重写目标（clang 报告的拼写）；无效为空
        std::size_t begin = 0; // 文件内字节偏移，区间起点（含）
        std::size_t end = 0;   // 区间终点（不含）；插入形 fixit 两端相等
        std::string replacement;
    };

    struct Diagnostic
    {
        DiagSeverity       severity = DiagSeverity::Ignored;
        std::string        spelling;
        SourceLocation     location;
        std::vector<FixIt> fixits;
    };

    /**
     * @brief 递交给解析的一个内存源覆盖（CXUnsavedFile 孪生）。
     *
     * filename 与被解析文件拼写精确匹配时，clang 仅本次解析读 contents
     * 而非磁盘文件。
     *
     * @internal
     * 这是 export 同宽消隐覆盖（把模块单元的 export 关键字按同宽空白
     * 替换、绕开 libclang 对 export 声明零符号产出的坑）的机制基础：
     * 磁盘文件绝不修改，缓冲只需活过解析调用本身（TU 不保留其引用）。
     */
    struct UnsavedFile
    {
        std::string filename;
        std::string contents;
    };

    namespace detail
    {

        /**
         * @brief 拥有一条引擎给出的诊断：析构时交还给引擎。
         *
         * @internal
         * 引擎按条交出诊断，每条都要交还。诊断只在收集它的那一次循环里用
         * 得到，所以不移动也不拷贝，交还调用只出现在析构一处。引擎没有给
         * 出诊断（下标越界）时对象为空，析构不做任何事。
         */
        class OwnedDiagnostic
        {
        public:
            OwnedDiagnostic(const OwnedDiagnostic&) = delete;
            auto operator=(const OwnedDiagnostic&) -> OwnedDiagnostic& = delete;
            OwnedDiagnostic(OwnedDiagnostic&&) = delete;
            auto operator=(OwnedDiagnostic&&) -> OwnedDiagnostic& = delete;

            ~OwnedDiagnostic()
            {
                if (diag_ != nullptr)
                {
                    api().clang_disposeDiagnostic(diag_);
                }
            }

            /** @brief 取翻译单元的第 index 条诊断并持有；越界时对象为空。 */
            [[nodiscard]]
            static auto at(const CXTranslationUnit& tu, unsigned index)
                -> OwnedDiagnostic
            {
                return OwnedDiagnostic{api().clang_getDiagnostic(tu, index)};
            }

            /** @brief 诊断的原生值，只供传给门面调用；不持有时是空值。 */
            [[nodiscard]]
            auto get() const noexcept -> CXDiagnostic
            {
                return diag_;
            }

        private:
            /** @brief 接管引擎给出的一条诊断。 */
            explicit OwnedDiagnostic(CXDiagnostic diag) noexcept : diag_{diag}
            {
            }

            CXDiagnostic diag_ = nullptr;
        };

        auto file_location(CXSourceLocation loc) -> SourceLocation
        {
            SourceLocation out;
            CXFile         file = nullptr;
            api().clang_getFileLocation(
                loc,
                &file,
                &out.line,
                &out.column,
                nullptr);
            if (file != nullptr)
            {
                out.file = Str{api().clang_getFileName(file)}.std_str();
            }
            return out;
        }

        auto make_diagnostic(
            CXDiagnostic d,
            DiagSeverity severity,
            bool         with_fixits) -> Diagnostic
        {
            Diagnostic diag;
            diag.severity = severity;
            diag.spelling = Str{api().clang_getDiagnosticSpelling(d)}.std_str();
            diag.location = file_location(api().clang_getDiagnosticLocation(d));
            if (with_fixits)
            {
                const unsigned count = api().clang_getDiagnosticNumFixIts(d);
                for (unsigned k = 0; k < count; ++k)
                {
                    CXSourceRange  range;
                    const CXString raw =
                        api().clang_getDiagnosticFixIt(d, k, &range);
                    FixIt fix;
                    fix.replacement = Str{raw}.std_str();
                    CXFile   begin_file = nullptr;
                    CXFile   end_file = nullptr;
                    unsigned begin_off = 0;
                    unsigned end_off = 0;
                    api().clang_getSpellingLocation(
                        api().clang_getRangeStart(range),
                        &begin_file,
                        nullptr,
                        nullptr,
                        &begin_off);
                    api().clang_getSpellingLocation(
                        api().clang_getRangeEnd(range),
                        &end_file,
                        nullptr,
                        nullptr,
                        &end_off);
                    if (begin_file == nullptr || begin_file != end_file)
                    {
                        continue; // 无文件或跨文件跨度：无处可应用
                    }
                    fix.file =
                        Str{api().clang_getFileName(begin_file)}.std_str();
                    fix.begin = begin_off;
                    fix.end = end_off >= begin_off ? end_off : begin_off;
                    if (fix.file.empty())
                    {
                        continue;
                    }
                    diag.fixits.push_back(std::move(fix));
                }
            }
            return diag;
        }

        struct VisitCtx
        {
            const std::function<bool(const CXCursor&)>* fn = nullptr;
        };

        /**
         * @brief clang_visitChildren 的回调蹦床。
         *
         * @internal fn 返回 true：继续下探；false：修剪该子树、保留兄弟。
         */
        auto visitor(CXCursor cursor, CXCursor /*parent*/, CXClientData data)
            -> CXChildVisitResult
        {
            const auto& fn = *static_cast<VisitCtx*>(data)->fn;
            return fn(cursor) ? CXChildVisit_Recurse : CXChildVisit_Continue;
        }

    } // namespace detail

    /**
     * @brief 拥有一个 CXTranslationUnit，并记住解析它的主文件路径（主
     * 文件游标过滤的锚点）。
     *
     * @internal
     * 翻译单元指针由带删除器的拥有型指针持有：销毁调用只在删除器里，
     * 析构、移动赋值与交出地址都经它，所以析构与移动都不抛。只能移动，
     * 被移动走的对象不持有翻译单元。
     */
    class Tu
    {
        /** @brief 翻译单元的删除器；销毁调用只在这一处出现。 */
        struct Deleter
        {
            auto operator()(CXTranslationUnit tu) const noexcept -> void
            {
                detail::api().clang_disposeTranslationUnit(tu);
            }
        };

        /**
         * @brief 翻译单元指针的拥有者。
         *
         * @internal
         * CXTranslationUnit 本身是指针类型（指向引擎内部的结构体），模板
         * 参数写它指向的那个结构体。
         */
        using Owner =
            cg::util::handle::OwnedPtr<CXTranslationUnitImpl, Deleter>;

    public:
        Tu() = default;
        Tu(const Tu&) = delete;
        Tu& operator=(const Tu&) = delete;
        Tu(Tu&& other) noexcept = default;
        Tu& operator=(Tu&& other) noexcept = default;

        [[nodiscard]]
        bool valid() const noexcept
        {
            return tu_.valid();
        }
        /** @brief 传入 Index::parse 时的原样路径；比较须经
         *  cg::util::norm_path。 */
        [[nodiscard]]
        const std::string& file() const noexcept
        {
            return file_;
        }

        /** @brief 整个翻译单元（含头文件）的根游标。 */
        [[nodiscard]]
        CXCursor cursor() const noexcept
        {
            return detail::api().clang_getTranslationUnitCursor(tu_.get());
        }

        /**
         * @brief 按报告顺序取 min_severity 及以上的诊断，至多 limit 条
         * （0 = 不限）；with_fixits 时每条诊断还携带其 FixItHint。
         *
         * @internal 只上报首条拼写的调用方传小限额——坏 include 的错误
         * 风暴不能令诊断收集成为 TU 的主导成本。FixIt 收集按需开启
         * （诊断驱动的内存源码改写是唯一消费者），其余调用方零开销。
         */
        [[nodiscard]]
        std::vector<Diagnostic> diagnostics(
            DiagSeverity min_severity,
            std::size_t  limit = 0,
            bool         with_fixits = false) const
        {
            std::vector<Diagnostic> out;
            const unsigned          count =
                detail::api().clang_getNumDiagnostics(tu_.get());
            for (unsigned i = 0; i < count; ++i)
            {
                // 每条诊断一取到就交给拥有者：达到条数上限提前离开，或者
                // 拼接文本时抛出，它都随本次循环交还。
                const detail::OwnedDiagnostic owned =
                    detail::OwnedDiagnostic::at(tu_.get(), i);
                const CXDiagnostic d = owned.get();
                const auto         severity = static_cast<DiagSeverity>(
                    detail::api().clang_getDiagnosticSeverity(d));
                if (severity >= min_severity)
                {
                    out.push_back(
                        detail::make_diagnostic(d, severity, with_fixits));
                    if (limit != 0 && out.size() >= limit)
                    {
                        break;
                    }
                }
            }
            return out;
        }

    private:
        friend class Index;
        Tu(Owner tu, std::string file) noexcept
            : tu_{std::move(tu)}, file_{std::move(file)}
        {
        }

        Owner       tu_;
        std::string file_;
    };

    /**
     * @brief 拥有一个 CXIndex。必须活过由它创建的每个 Tu。不可移动：
     * 一个索引对应一个解析会话即预期用法。
     *
     * @internal 构造获取进程引擎（尚无显式加载时经默认解析链加载）；析构
     * 给引擎的引用计数减一。引擎库本身按设计常驻到进程退出，最后一个
     * Index 析构也不卸载它（理由见 detail::EngineState 的生命周期注记）。
     * 构造参数 (0, 0) = 保留 PCH 声明、不打印 libclang 侧诊断（诊断经
     * Tu::diagnostics 收集）。崩溃恢复不是 createIndex 参数——它每次引擎加载武
     * 装一次（detail::load_locked 调 clang_toggleCrashRecovery）。
     */
    class Index
    {
    public:
        Index()
        {
            detail::acquire_engine();
            idx_ = detail::api().clang_createIndex(0, 0);
        }
        ~Index()
        {
            if (idx_ != nullptr)
            {
                detail::api().clang_disposeIndex(idx_);
            }
            detail::release_engine();
        }
        Index(const Index&) = delete;
        Index& operator=(const Index&) = delete;
        Index(Index&&) = delete;
        Index& operator=(Index&&) = delete;

        [[nodiscard]]
        bool valid() const noexcept
        {
            return idx_ != nullptr;
        }

        /**
         * @brief 以 args（仅编译器开关，无 argv[0]）解析 file，可带内存
         * 源覆盖。
         *
         * 选项：详细预处理记录（include 指令成为游标）加 KeepGoing（尽
         * 可能恢复树）。skip_instantiations 为 true 时追加 Incomplete
         * （TUKind=TU_Prefix：Sema 跳过 TU 末尾的延迟模板实例化），默认
         * false 时选项位集与不加该参数逐字节相同。仅硬失败抛
         * std::runtime_error；可恢复问题经 Tu::diagnostics() 上报。
         *
         * unsaved 中每个 filename 与解析会读的文件（主文件拼写精确匹配，
         * 或某个 include）匹配时，clang 仅本次解析用该条目的 contents
         * 代替磁盘字节。覆盖串须存活到本调用返回；产出的 Tu 此后自包含。
         *
         * @internal export 同宽消隐覆盖的机制基础（磁盘文件绝不修改）。
         */
        [[nodiscard]]
        Tu parse(
            std::string_view                file,
            const std::vector<std::string>& args,
            std::span<const UnsavedFile>    unsaved,
            bool                            detailed = true,
            bool                            skip_instantiations = false) const
        {
            std::vector<const char*> cargs;
            cargs.reserve(args.size());
            for (const std::string& a : args)
            {
                cargs.push_back(a.c_str());
            }
            std::vector<CXUnsavedFile> covers;
            covers.reserve(unsaved.size());
            for (const UnsavedFile& u : unsaved)
            {
                covers.push_back(
                    CXUnsavedFile{
                        .Filename = u.filename.c_str(),
                        .Contents = u.contents.c_str(),
                        .Length =
                            static_cast<unsigned long>(u.contents.size())});
            }
            const std::string path{file};
            // 翻译单元直接由拥有者接住：解析失败而引擎在出参里留下了东
            // 西时，它随抛出的栈展开销毁。
            Tu::Owner parsed;
            // replay-degraded PCH 消费显式关闭预处理记录（否则非 DPR 用户
            // PCH 硬失败）；该模式下 include 边不完整，索引侧持续标记。
            const unsigned options = static_cast<unsigned>(
                (detailed ? CXTranslationUnit_DetailedPreprocessingRecord : 0u)
                | CXTranslationUnit_KeepGoing
                | (skip_instantiations ? CXTranslationUnit_Incomplete : 0u));
            const CXErrorCode rc = detail::api().clang_parseTranslationUnit2(
                idx_,
                path.c_str(),
                cargs.data(),
                static_cast<int>(cargs.size()),
                covers.data(),
                static_cast<unsigned>(covers.size()),
                options,
                &parsed);
            if (rc != CXError_Success || !parsed)
            {
                const auto code = static_cast<ParseErrc>(
                    !parsed && rc == CXError_Success ? CXError_Failure : rc);
                throw ParseError(
                    std::format(
                        "libclang failed to parse {}: error code {}",
                        path,
                        static_cast<int>(code)),
                    code);
            }
            return Tu{std::move(parsed), path};
        }

    private:
        CXIndex idx_;
    };
} // namespace cg::clang
