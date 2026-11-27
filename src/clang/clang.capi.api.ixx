module;
#include <clang-c/Index.h> // 第三方头文件，仅限全局模块片段

/**
 * @module cg.clang.capi.api
 *
 * @brief libclang 分发表与引擎装载域：Api
 * 函数指针表、符号解析、进程生命期的引擎装载/获取/引用计数，以及
 * CXString 的拥有者 Str。
 */
export module cg.clang.capi.api;

import std;
import cg.clang.loader;
import cg.util; // 伞形模块；此处用的是 cg.util.guard（崩溃防护）
export {
    using ::CX_BinaryOperatorKind;
    using ::CXCursor;
    using ::CXCursorKind;
}

export namespace cg::clang
{

    namespace detail
    {

        /**
         * @brief 分发表：门面用到的每个 clang-c 符号一个函数指针。
         *
         * @internal 成员列表自下方代码构建（本单元持有项目中全部裸 CX 调用）；
         * detail::resolve_api_into 在运行时加载的库上按名填充每个槽位。
         */
        // NOLINTBEGIN(readability-identifier-naming) 成员刻意与 clang-c
        // 符号名逐一相同——该结构体本身就是可审计的符号清单，每个条目都必须与其在
        // resolve_api_into 中的字符串一致
        struct Api
        {
            decltype(&::clang_createIndex)  clang_createIndex;
            decltype(&::clang_disposeIndex) clang_disposeIndex;
            decltype(&::clang_parseTranslationUnit2)
                clang_parseTranslationUnit2;
            decltype(&::clang_disposeTranslationUnit)
                clang_disposeTranslationUnit;
            decltype(&::clang_getTranslationUnitCursor)
                                                 clang_getTranslationUnitCursor;
            decltype(&::clang_getNumDiagnostics) clang_getNumDiagnostics;
            decltype(&::clang_getDiagnostic)     clang_getDiagnostic;
            decltype(&::clang_disposeDiagnostic) clang_disposeDiagnostic;
            decltype(&::clang_getDiagnosticSeverity)
                clang_getDiagnosticSeverity;
            decltype(&::clang_getDiagnosticSpelling)
                clang_getDiagnosticSpelling;
            decltype(&::clang_getDiagnosticLocation)
                clang_getDiagnosticLocation;
            decltype(&::clang_getDiagnosticNumFixIts)
                                                  clang_getDiagnosticNumFixIts;
            decltype(&::clang_getDiagnosticFixIt) clang_getDiagnosticFixIt;
            decltype(&::clang_getCString)         clang_getCString;
            decltype(&::clang_disposeString)      clang_disposeString;
            decltype(&::clang_visitChildren)      clang_visitChildren;
            decltype(&::clang_getCursorKind)      clang_getCursorKind;
            decltype(&::clang_getCursorKindSpelling)
                                                    clang_getCursorKindSpelling;
            decltype(&::clang_getCursorSpelling)    clang_getCursorSpelling;
            decltype(&::clang_getCursorDisplayName) clang_getCursorDisplayName;
            decltype(&::clang_isCursorDefinition)   clang_isCursorDefinition;
            decltype(&::clang_CXXMethod_isConst)    clang_CXXMethod_isConst;
            decltype(&::clang_CXXMethod_isVirtual)  clang_CXXMethod_isVirtual;
            decltype(&::clang_CXXMethod_isPureVirtual)
                                                  clang_CXXMethod_isPureVirtual;
            decltype(&::clang_CXXMethod_isStatic) clang_CXXMethod_isStatic;
            decltype(&::clang_CXXMethod_isDefaulted)
                clang_CXXMethod_isDefaulted;
            decltype(&::clang_getCXXAccessSpecifier)
                                                   clang_getCXXAccessSpecifier;
            decltype(&::clang_isVirtualBase)       clang_isVirtualBase;
            decltype(&::clang_getCursorDefinition) clang_getCursorDefinition;
            decltype(&::clang_getCursorType)       clang_getCursorType;
            decltype(&::clang_getCursorResultType) clang_getCursorResultType;
            decltype(&::clang_getNumArgTypes)      clang_getNumArgTypes;
            decltype(&::clang_getArgType)          clang_getArgType;
            decltype(&::clang_getTypeSpelling)     clang_getTypeSpelling;
            decltype(&::clang_getTypedefDeclUnderlyingType)
                clang_getTypedefDeclUnderlyingType;
            decltype(&::clang_getOverriddenCursors) clang_getOverriddenCursors;
            decltype(&::clang_disposeOverriddenCursors)
                clang_disposeOverriddenCursors;
            decltype(&::clang_Cursor_getNumTemplateArguments)
                clang_Cursor_getNumTemplateArguments;
            decltype(&::clang_Cursor_getTemplateArgumentType)
                clang_Cursor_getTemplateArgumentType;
            decltype(&::clang_Cursor_getTranslationUnit)
                                                clang_Cursor_getTranslationUnit;
            decltype(&::clang_tokenize)         clang_tokenize;
            decltype(&::clang_getTokenSpelling) clang_getTokenSpelling;
            decltype(&::clang_disposeTokens)    clang_disposeTokens;
            decltype(&::clang_getCursorExtent)  clang_getCursorExtent;
            decltype(&::clang_Cursor_getBinaryOpcode)
                                             clang_Cursor_getBinaryOpcode;
            decltype(&::clang_getRangeStart) clang_getRangeStart;
            decltype(&::clang_getRangeEnd)   clang_getRangeEnd;
            decltype(&::clang_getCursorReferenceNameRange)
                                            clang_getCursorReferenceNameRange;
            decltype(&::clang_equalRanges)  clang_equalRanges;
            decltype(&::clang_Range_isNull) clang_Range_isNull;
            decltype(&::clang_getCursorPrettyPrinted)
                                                   clang_getCursorPrettyPrinted;
            decltype(&::clang_getCursorLocation)   clang_getCursorLocation;
            decltype(&::clang_getSpellingLocation) clang_getSpellingLocation;
            decltype(&::clang_getFileLocation)     clang_getFileLocation;
            decltype(&::clang_getFileName)         clang_getFileName;
            decltype(&::clang_getFileContents)     clang_getFileContents;
            decltype(&::clang_getCursorSemanticParent)
                clang_getCursorSemanticParent;
            decltype(&::clang_getCursorLexicalParent)
                                                   clang_getCursorLexicalParent;
            decltype(&::clang_getCursorReferenced) clang_getCursorReferenced;
            decltype(&::clang_getSpecializedCursorTemplate)
                                             clang_getSpecializedCursorTemplate;
            decltype(&::clang_Cursor_isNull) clang_Cursor_isNull;
            decltype(&::clang_getPointeeType)       clang_getPointeeType;
            decltype(&::clang_getElementType)       clang_getElementType;
            decltype(&::clang_Type_getNamedType)    clang_Type_getNamedType;
            decltype(&::clang_getCanonicalType)     clang_getCanonicalType;
            decltype(&::clang_Type_getModifiedType) clang_Type_getModifiedType;
            decltype(&::clang_getTypeDeclaration)   clang_getTypeDeclaration;
            decltype(&::clang_getCursorUSR)         clang_getCursorUSR;
            decltype(&::clang_Type_getNumTemplateArguments)
                clang_Type_getNumTemplateArguments;
            decltype(&::clang_Type_getTemplateArgumentAsType)
                clang_Type_getTemplateArgumentAsType;
            decltype(&::clang_Type_getClassType)   clang_Type_getClassType;
            decltype(&::clang_getResultType)       clang_getResultType;
            decltype(&::clang_getClangVersion)     clang_getClangVersion;
            decltype(&::clang_toggleCrashRecovery) clang_toggleCrashRecovery;
        };
        // NOLINTEND(readability-identifier-naming)

        /**
         * @brief 进程级引擎状态。
         *
         * refs 计数存活的 Index 对象；非零计数拒绝引擎切换（一个解析会话 =
         * 一个世界观；既定的 Index 活过 Tu 契约使 Index 计数足以界定每个
         * CXIndex/CXTranslationUnit）。table 是每个门面调用解引用的无锁读侧。
         *
         * @internal
         * 生命周期：一经加载，引擎库保持映射到进程退出——装载函数在全部
         * 步骤成功之后把库句柄的所有权显式交给本状态（见 load_locked），
         * 此后从不卸载；中途失败的库由装载路径上的拥有者析构卸载。
         * libclang 的静态初始化器安装进程级崩溃恢复机制
         * （Windows 上是向量化 SEH handler），FreeLibrary/dlclose 不收回它；
         * 陈旧的 handler 表项在卸载后存活，重载后的下一次解析会经它派发进
         * 未映射内存。在本工具链实测（MSYS2 clang 23.1.3）：加载/卸载/重载
         * 循环在开启崩溃恢复（默认）时使 cg_test_incr 崩溃，仅
         * LIBCLANG_DISABLE_CRASH_RECOVERY=1 时通过；从不关闭句柄则两种情形
         * 皆绿。因此引擎切换保留旧句柄映射（其 handler 表项仍指向已映射
         * 代码，多份无害）；短命 CLI 进程实际至多切换一次，保留映射不耗
         * 任何真实成本。
         *
         * 按设计常驻、直到进程退出都不释放的资源在此集中登记：
         * - library：当前引擎库的句柄，以及被它替换下来的旧引擎库的映射
         *   （切换时不卸载，理由同上）。进程退出时由系统一并回收。
         */
        struct EngineState
        {
            std::mutex mutex;
            int        refs = 0;
            // 按设计常驻的引擎库句柄：装载函数交出所有权后登记在此。
            void*       library = nullptr;
            std::string path;    // 已加载库路径（未加载时为 ""）
            std::string version; // 加载时捕获的 clang_getClangVersion()
            Api         api{};
            std::atomic<const Api*> table{nullptr};
        };

        auto engine_state() -> EngineState&
        {
            static EngineState state;
            return state;
        }

        /**
         * @brief 类型化符号查找。
         *
         * @internal nullptr 变为点名缺失导出的可行动不兼容引擎错误。
         */
        template <typename Fn>
        [[nodiscard]]
        auto symbol_as(const dylib::Library& library, const char* name) -> Fn
        {
            void* raw = dylib::symbol(library, name);
            if (raw == nullptr)
            {
                throw std::runtime_error(
                    std::string{"the loaded LLVM engine exports no '"} + name
                    + "' (incompatible libclang build?)");
            }
            return reinterpret_cast<Fn>(raw);
        }

        /**
         * @brief 从 library 填充 a。
         *
         * @internal 失败时保持调用方状态原样未动（调用方卸载一个从未运行
         * 过任何 clang 调用的库——见 EngineState 的生命周期注记，解释为何
         * 这次卸载安全而退役引擎的卸载不安全）。
         */
        void resolve_api_into(Api& a, const dylib::Library& library)
        {
            a.clang_createIndex = symbol_as<decltype(a.clang_createIndex)>(
                library,
                "clang_createIndex");
            a.clang_disposeIndex = symbol_as<decltype(a.clang_disposeIndex)>(
                library,
                "clang_disposeIndex");
            a.clang_parseTranslationUnit2 =
                symbol_as<decltype(a.clang_parseTranslationUnit2)>(
                    library,
                    "clang_parseTranslationUnit2");
            a.clang_disposeTranslationUnit =
                symbol_as<decltype(a.clang_disposeTranslationUnit)>(
                    library,
                    "clang_disposeTranslationUnit");
            a.clang_getTranslationUnitCursor =
                symbol_as<decltype(a.clang_getTranslationUnitCursor)>(
                    library,
                    "clang_getTranslationUnitCursor");
            a.clang_getNumDiagnostics =
                symbol_as<decltype(a.clang_getNumDiagnostics)>(
                    library,
                    "clang_getNumDiagnostics");
            a.clang_getDiagnostic = symbol_as<decltype(a.clang_getDiagnostic)>(
                library,
                "clang_getDiagnostic");
            a.clang_disposeDiagnostic =
                symbol_as<decltype(a.clang_disposeDiagnostic)>(
                    library,
                    "clang_disposeDiagnostic");
            a.clang_getDiagnosticSeverity =
                symbol_as<decltype(a.clang_getDiagnosticSeverity)>(
                    library,
                    "clang_getDiagnosticSeverity");
            a.clang_getDiagnosticSpelling =
                symbol_as<decltype(a.clang_getDiagnosticSpelling)>(
                    library,
                    "clang_getDiagnosticSpelling");
            a.clang_getDiagnosticLocation =
                symbol_as<decltype(a.clang_getDiagnosticLocation)>(
                    library,
                    "clang_getDiagnosticLocation");
            a.clang_getDiagnosticNumFixIts =
                symbol_as<decltype(a.clang_getDiagnosticNumFixIts)>(
                    library,
                    "clang_getDiagnosticNumFixIts");
            a.clang_getDiagnosticFixIt =
                symbol_as<decltype(a.clang_getDiagnosticFixIt)>(
                    library,
                    "clang_getDiagnosticFixIt");
            a.clang_getCString = symbol_as<decltype(a.clang_getCString)>(
                library,
                "clang_getCString");
            a.clang_disposeString = symbol_as<decltype(a.clang_disposeString)>(
                library,
                "clang_disposeString");
            a.clang_visitChildren = symbol_as<decltype(a.clang_visitChildren)>(
                library,
                "clang_visitChildren");
            a.clang_getCursorKind = symbol_as<decltype(a.clang_getCursorKind)>(
                library,
                "clang_getCursorKind");
            a.clang_getCursorKindSpelling =
                symbol_as<decltype(a.clang_getCursorKindSpelling)>(
                    library,
                    "clang_getCursorKindSpelling");
            a.clang_getCursorSpelling =
                symbol_as<decltype(a.clang_getCursorSpelling)>(
                    library,
                    "clang_getCursorSpelling");
            a.clang_getCursorDisplayName =
                symbol_as<decltype(a.clang_getCursorDisplayName)>(
                    library,
                    "clang_getCursorDisplayName");
            a.clang_isCursorDefinition =
                symbol_as<decltype(a.clang_isCursorDefinition)>(
                    library,
                    "clang_isCursorDefinition");
            a.clang_CXXMethod_isConst =
                symbol_as<decltype(a.clang_CXXMethod_isConst)>(
                    library,
                    "clang_CXXMethod_isConst");
            a.clang_CXXMethod_isVirtual =
                symbol_as<decltype(a.clang_CXXMethod_isVirtual)>(
                    library,
                    "clang_CXXMethod_isVirtual");
            a.clang_CXXMethod_isPureVirtual =
                symbol_as<decltype(a.clang_CXXMethod_isPureVirtual)>(
                    library,
                    "clang_CXXMethod_isPureVirtual");
            a.clang_CXXMethod_isStatic =
                symbol_as<decltype(a.clang_CXXMethod_isStatic)>(
                    library,
                    "clang_CXXMethod_isStatic");
            a.clang_CXXMethod_isDefaulted =
                symbol_as<decltype(a.clang_CXXMethod_isDefaulted)>(
                    library,
                    "clang_CXXMethod_isDefaulted");
            a.clang_getCXXAccessSpecifier =
                symbol_as<decltype(a.clang_getCXXAccessSpecifier)>(
                    library,
                    "clang_getCXXAccessSpecifier");
            a.clang_isVirtualBase = symbol_as<decltype(a.clang_isVirtualBase)>(
                library,
                "clang_isVirtualBase");
            a.clang_getCursorDefinition =
                symbol_as<decltype(a.clang_getCursorDefinition)>(
                    library,
                    "clang_getCursorDefinition");
            a.clang_getCursorType = symbol_as<decltype(a.clang_getCursorType)>(
                library,
                "clang_getCursorType");
            a.clang_getCursorResultType =
                symbol_as<decltype(a.clang_getCursorResultType)>(
                    library,
                    "clang_getCursorResultType");
            a.clang_getNumArgTypes =
                symbol_as<decltype(a.clang_getNumArgTypes)>(
                    library,
                    "clang_getNumArgTypes");
            a.clang_getArgType = symbol_as<decltype(a.clang_getArgType)>(
                library,
                "clang_getArgType");
            a.clang_getTypeSpelling =
                symbol_as<decltype(a.clang_getTypeSpelling)>(
                    library,
                    "clang_getTypeSpelling");
            a.clang_getTypedefDeclUnderlyingType =
                symbol_as<decltype(a.clang_getTypedefDeclUnderlyingType)>(
                    library,
                    "clang_getTypedefDeclUnderlyingType");
            a.clang_getOverriddenCursors =
                symbol_as<decltype(a.clang_getOverriddenCursors)>(
                    library,
                    "clang_getOverriddenCursors");
            a.clang_disposeOverriddenCursors =
                symbol_as<decltype(a.clang_disposeOverriddenCursors)>(
                    library,
                    "clang_disposeOverriddenCursors");
            a.clang_Cursor_getNumTemplateArguments =
                symbol_as<decltype(a.clang_Cursor_getNumTemplateArguments)>(
                    library,
                    "clang_Cursor_getNumTemplateArguments");
            a.clang_Cursor_getTemplateArgumentType =
                symbol_as<decltype(a.clang_Cursor_getTemplateArgumentType)>(
                    library,
                    "clang_Cursor_getTemplateArgumentType");
            a.clang_Cursor_getTranslationUnit =
                symbol_as<decltype(a.clang_Cursor_getTranslationUnit)>(
                    library,
                    "clang_Cursor_getTranslationUnit");
            a.clang_tokenize = symbol_as<decltype(a.clang_tokenize)>(
                library,
                "clang_tokenize");
            a.clang_getTokenSpelling =
                symbol_as<decltype(a.clang_getTokenSpelling)>(
                    library,
                    "clang_getTokenSpelling");
            a.clang_disposeTokens = symbol_as<decltype(a.clang_disposeTokens)>(
                library,
                "clang_disposeTokens");
            a.clang_getCursorExtent =
                symbol_as<decltype(a.clang_getCursorExtent)>(
                    library,
                    "clang_getCursorExtent");
            a.clang_Cursor_getBinaryOpcode =
                symbol_as<decltype(a.clang_Cursor_getBinaryOpcode)>(
                    library,
                    "clang_Cursor_getBinaryOpcode");
            a.clang_getRangeStart = symbol_as<decltype(a.clang_getRangeStart)>(
                library,
                "clang_getRangeStart");
            a.clang_getRangeEnd = symbol_as<decltype(a.clang_getRangeEnd)>(
                library,
                "clang_getRangeEnd");
            a.clang_getCursorReferenceNameRange =
                symbol_as<decltype(a.clang_getCursorReferenceNameRange)>(
                    library,
                    "clang_getCursorReferenceNameRange");
            a.clang_equalRanges = symbol_as<decltype(a.clang_equalRanges)>(
                library,
                "clang_equalRanges");
            a.clang_Range_isNull = symbol_as<decltype(a.clang_Range_isNull)>(
                library,
                "clang_Range_isNull");
            a.clang_getCursorPrettyPrinted =
                symbol_as<decltype(a.clang_getCursorPrettyPrinted)>(
                    library,
                    "clang_getCursorPrettyPrinted");
            a.clang_getCursorLocation =
                symbol_as<decltype(a.clang_getCursorLocation)>(
                    library,
                    "clang_getCursorLocation");
            a.clang_getSpellingLocation =
                symbol_as<decltype(a.clang_getSpellingLocation)>(
                    library,
                    "clang_getSpellingLocation");
            a.clang_getFileLocation =
                symbol_as<decltype(a.clang_getFileLocation)>(
                    library,
                    "clang_getFileLocation");
            a.clang_getFileName = symbol_as<decltype(a.clang_getFileName)>(
                library,
                "clang_getFileName");
            a.clang_getFileContents =
                symbol_as<decltype(a.clang_getFileContents)>(
                    library,
                    "clang_getFileContents");
            a.clang_getCursorSemanticParent =
                symbol_as<decltype(a.clang_getCursorSemanticParent)>(
                    library,
                    "clang_getCursorSemanticParent");
            a.clang_getCursorLexicalParent =
                symbol_as<decltype(a.clang_getCursorLexicalParent)>(
                    library,
                    "clang_getCursorLexicalParent");
            a.clang_getCursorReferenced =
                symbol_as<decltype(a.clang_getCursorReferenced)>(
                    library,
                    "clang_getCursorReferenced");
            a.clang_getSpecializedCursorTemplate =
                symbol_as<decltype(a.clang_getSpecializedCursorTemplate)>(
                    library,
                    "clang_getSpecializedCursorTemplate");
            a.clang_Cursor_isNull = symbol_as<decltype(a.clang_Cursor_isNull)>(
                library,
                "clang_Cursor_isNull");
            a.clang_getPointeeType =
                symbol_as<decltype(a.clang_getPointeeType)>(
                    library,
                    "clang_getPointeeType");
            a.clang_getElementType =
                symbol_as<decltype(a.clang_getElementType)>(
                    library,
                    "clang_getElementType");
            a.clang_Type_getNamedType =
                symbol_as<decltype(a.clang_Type_getNamedType)>(
                    library,
                    "clang_Type_getNamedType");
            a.clang_getCanonicalType =
                symbol_as<decltype(a.clang_getCanonicalType)>(
                    library,
                    "clang_getCanonicalType");
            a.clang_Type_getModifiedType =
                symbol_as<decltype(a.clang_Type_getModifiedType)>(
                    library,
                    "clang_Type_getModifiedType");
            a.clang_getTypeDeclaration =
                symbol_as<decltype(a.clang_getTypeDeclaration)>(
                    library,
                    "clang_getTypeDeclaration");
            a.clang_getCursorUSR = symbol_as<decltype(a.clang_getCursorUSR)>(
                library,
                "clang_getCursorUSR");
            a.clang_Type_getNumTemplateArguments =
                symbol_as<decltype(a.clang_Type_getNumTemplateArguments)>(
                    library,
                    "clang_Type_getNumTemplateArguments");
            a.clang_Type_getTemplateArgumentAsType =
                symbol_as<decltype(a.clang_Type_getTemplateArgumentAsType)>(
                    library,
                    "clang_Type_getTemplateArgumentAsType");
            a.clang_Type_getClassType =
                symbol_as<decltype(a.clang_Type_getClassType)>(
                    library,
                    "clang_Type_getClassType");
            a.clang_getResultType = symbol_as<decltype(a.clang_getResultType)>(
                library,
                "clang_getResultType");
            a.clang_getClangVersion =
                symbol_as<decltype(a.clang_getClangVersion)>(
                    library,
                    "clang_getClangVersion");
            a.clang_toggleCrashRecovery =
                symbol_as<decltype(a.clang_toggleCrashRecovery)>(
                    library,
                    "clang_toggleCrashRecovery");
        }

        /**
         * @brief 每个门面调用解引用的唯一访问器。
         *
         * @internal 无锁；原子指针仅在状态互斥锁内的加载中从 null 摆向已
         * 填充的表。
         */
        [[nodiscard]]
        auto api() -> const Api&
        {
            const Api* t = engine_state().table.load(std::memory_order_acquire);
            if (t == nullptr)
            {
                throw std::runtime_error(
                    "ccg internal error: the libclang engine is not loaded "
                    "(every clang call needs a live Index - file a bug if you "
                    "see this)");
            }
            return *t;
        }

    } // namespace detail

    /**
     * @brief 拥有一个 CXString；转换为 C 串 / 视图 / std::string。
     *
     * @internal
     * 释放与取串都经构造时记下的那张分发表，析构不再去取已发布的表，所
     * 以析构不抛。常用的那个构造取已发布的表：引擎没有装载时它在构造处
     * 抛出，而不是等到析构。装载路径上分发表还没有发布（它要到装载函数
     * 末尾才发布），那里用带分发表引用的构造，把刚填好的表直接交给它。
     * 这个类住在分发表所在的单元，装载函数才用得到它。因用户声明的析构
     * 抑制隐式移动构造而逐字写出（删除拷贝与移动）。
     */
    class Str
    {
    public:
        /** @brief 按已发布的分发表持有 s；引擎没有装载时抛出。 */
        explicit Str(CXString s) : Str{s, detail::api()} {}

        /** @brief 按给定的分发表持有 s；分发表必须在本次构造之后一直有效。 */
        explicit Str(CXString s, const detail::Api& api) noexcept
            : s_{s}, api_{&api}
        {
        }

        ~Str() { api_->clang_disposeString(s_); }
        Str(const Str&) = delete;
        Str& operator=(const Str&) = delete;
        Str(Str&&) = delete;
        Str& operator=(Str&&) = delete;

        [[nodiscard]]
        const char* c_str() const noexcept
        {
            const char* p = api_->clang_getCString(s_);
            return p != nullptr ? p : "";
        }
        [[nodiscard]]
        std::string_view view() const noexcept
        {
            return c_str();
        }
        [[nodiscard]]
        std::string std_str() const
        {
            return c_str();
        }

    private:
        CXString           s_;
        const detail::Api* api_;
    };

    namespace detail
    {

        /**
         * @brief 把 path 加载进 e（互斥锁由调用方持有）。
         *
         * @internal
         * 任何失败时状态与调用前完全一致。解析失败的库可以再卸载：还没有
         * 任何 clang 调用运行过、未安装 handler 状态（见 EngineState 的
         * 生命周期注记）。在设置分发表的同时捕获版本串——它是新鲜度校验
         * （数据库 meta 的 llvm_version）的引擎身份一半。崩溃恢复刻意开启：
         * 陈旧 BMI 变异类（下游 pcm 引用了被删除的声明）会直接杀死宿主
         * 进程，恢复机制把它变成 ccg 可分类的 CXError_Crashed 解析结果而非
         * 静默死亡。它在 libclang 内是进程级全局
         * （clang_toggleCrashRecovery 翻转
         * llvm::CrashRecoveryContext::Enable），
         * 故每次引擎加载一次即覆盖会话的每个 Index；引擎此后永不卸载
         * （EngineState 生命周期注记），被恢复的 handler 状态保持武装才是
         * 安全的。
         */
        void load_locked(EngineState& e, const std::string& path)
        {
            // 库一取到就交给拥有者：后面任何一步抛出，都由它的析构卸载。
            dylib::Library library = dylib::open(path);
            Api            api{};
            resolve_api_into(api, library);
            // 分发表要到本函数末尾才发布，版本串因此按刚填好的这张表持有。
            const Str version{api.clang_getClangVersion(), api};
            // 两份文本先拷好再落位：拷贝可能抛出，抛出时引擎状态还没有
            // 被动过；之后的赋值都是不抛的移动或逐值复制。
            std::string version_text = version.std_str();
            std::string path_text{path};
            e.version = std::move(version_text);
            e.path = std::move(path_text);
            e.api = api;
            e.api.clang_toggleCrashRecovery(1);
            e.table.store(&e.api, std::memory_order_release);
            // 显式交出所有权：引擎库按设计常驻到进程退出，理由见 EngineState
            // 的生命周期注记。此后它由引擎状态以裸句柄登记持有，本函数的
            // 拥有者析构时不再卸载它。
            e.library = library.release();
        }

        /**
         * @brief 无显式引擎的 Index 的首次获取路径：默认解析链
         * （$CCG_LLVM_ROOT，再 PATH 发现）。
         */
        void acquire_engine()
        {
            auto&                  e = engine_state();
            const std::scoped_lock lock{e.mutex};
            if (e.library == nullptr)
            {
                load_locked(e, engine::resolve_library(""));
            }
            ++e.refs;
        }

        void release_engine()
        {
            auto&                  e = engine_state();
            const std::scoped_lock lock{e.mutex};
            if (e.library != nullptr)
            {
                --e.refs;
            }
        }

    } // namespace detail

    /**
     * @brief 为进程生命期加载 path 处的引擎（由 cg.clang::engine 解析）。
     *
     * 对已加载路径幂等；仅在静止（refs == 0）时允许切换到不同路径——
     * 活跃的 Index 对象会在会话中途经被交换的表解析。切换刻意保留旧
     * 句柄映射（见 detail::EngineState 的生命周期注记）。失败时抛出带
     * 可行动消息的 std::runtime_error。
     */
    void load_clang_library(const std::string& path)
    {
        auto&                  e = detail::engine_state();
        const std::scoped_lock lock{e.mutex};
        if (e.library != nullptr)
        {
            if (e.path == path)
            {
                return;
            }
            if (e.refs > 0)
            {
                throw std::runtime_error(
                    std::format(
                        "cannot switch the LLVM engine to '{}' while {} Index "
                        "object(s) are alive (one parse session = one worldview)",
                        path,
                        e.refs));
            }
        }
        try
        {
            detail::load_locked(e, path);
        }
        catch (const std::runtime_error& err)
        {
            throw std::runtime_error(
                std::format("{}\n  {}", err.what(), engine::resolution_hint()));
        }
    }

    /**
     * @brief 已加载引擎的路径（未加载时为 ""）。
     *
     * @internal 运行时取代旧的"链接器绑定了哪个 libclang"问题。
     */
    [[nodiscard]]
    auto loaded_clang_library_path() -> std::string
    {
        auto&                  e = detail::engine_state();
        const std::scoped_lock lock{e.mutex};
        return e.library != nullptr ? e.path : std::string{};
    }

    /**
     * @brief 已加载引擎报告的版本串（clang_getClangVersion，加载时捕获）。
     *
     * @internal 从未加载过引擎时抛出。
     */
    [[nodiscard]]
    auto libclang_version() -> std::string
    {
        auto&                  e = detail::engine_state();
        const std::scoped_lock lock{e.mutex};
        if (e.version.empty())
        {
            throw std::runtime_error(
                "ccg internal error: no LLVM engine has been loaded in this "
                "process");
        }
        return e.version;
    }
} // namespace cg::clang
