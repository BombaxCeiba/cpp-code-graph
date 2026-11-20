module;
#include <clang-c/Index.h> // 第三方头文件，仅限全局模块片段

/**
 * @module cg.clang.capi
 *
 * @brief libclang C API 之上的 RAII 句柄与薄类型化门面。
 *
 * 本单元是全项目唯一可接触 clang-c 头的单元（单一所有者规则：每个
 * 第三方头恰好归一个模块的全局模块片段所有）；其余一切 cg.clang
 * 子模块与导入方只经下方导出表面工作。导入方看得见
 * CXCursor/CXCursorKind（回传给门面调用的 不透明值类型），但绝见不到裸 libclang
 * 函数。
 *
 * @internal
 * clang-c/Index.h 是纯 extern "C" 声明、没有 static-inline 助手，故
 * 无需 TU 局部实体路由（extern "C" API 的直包模式）；导入方要用的
 * 类型名经 export using 重导出（GMF include 的名字仅"可达"、不能被
 * 导入方按名查找），RAII 属主因用户声明的析构抑制隐式移动而逐字
 * 写出移动构造。库本身是运行时加载（按数据库解析的世界观，既有
 * 口径）：项目任何地方都不存在对 libclang 的链接期依赖。
 * 下方每个 clang-c 调用都走模块内部分发表（detail::Api），在
 * detail::load_locked() 时由 cg.clang.loader 句柄按名填充——项目中
 * 已零直接 clang-c 调用。生命周期显式：load_clang_library()（或首个
 * Index 获取的默认链）加载、最后一个 Index 析构卸载、Index 对象
 * 存活期间拒绝切换引擎（一个解析会话 = 一个世界观）。
 */
export module cg.clang.capi;

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
         * 生命周期：一经加载，引擎库保持映射到进程退出——刻意不调用
         * dylib::close。libclang 的静态初始化器安装进程级崩溃恢复机制
         * （Windows 上是向量化 SEH handler），FreeLibrary/dlclose 不收回它；
         * 陈旧的 handler 表项在卸载后存活，重载后的下一次解析会经它派发进
         * 未映射内存。在本工具链实测（MSYS2 clang 23.1.3）：加载/卸载/重载
         * 循环在开启崩溃恢复（默认）时使 cg_test_incr 崩溃，仅
         * LIBCLANG_DISABLE_CRASH_RECOVERY=1 时通过；从不关闭句柄则两种情形
         * 皆绿。因此引擎切换保留旧句柄映射（其 handler 表项仍指向已映射
         * 代码，多份无害）；短命 CLI 进程实际至多切换一次，保留映射不耗
         * 任何真实成本。
         */
        struct EngineState
        {
            std::mutex  mutex;
            int         refs = 0;
            void*       library = nullptr; // cg.clang::dylib 句柄
            std::string path;              // 已加载库路径（未加载时为 ""）
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
        auto symbol_as(void* library, const char* name) -> Fn
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
         * @internal 失败时保持调用方状态原样未动（调用方关闭一个从未运行过
         * 任何 clang 调用的库——见 EngineState 的生命周期注记，解释为何该
         * 关闭安全而退役引擎的关闭不安全）。
         */
        void resolve_api_into(Api& a, void* library)
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
         * @brief 把 path 加载进 e（互斥锁由调用方持有）。
         *
         * @internal
         * 任何失败时状态与调用前完全一致。解析失败的库可以再关闭：还没有
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
            void* library = dylib::open(path);
            Api   api{};
            try
            {
                resolve_api_into(api, library);
            }
            catch (...)
            {
                dylib::close(library);
                throw;
            }
            e.library = library;
            e.api = api;
            e.path = path;
            const CXString raw = e.api.clang_getClangVersion();
            const char*    text = e.api.clang_getCString(raw);
            e.version = text != nullptr ? text : "";
            e.api.clang_disposeString(raw);
            e.api.clang_toggleCrashRecovery(1);
            e.table.store(&e.api, std::memory_order_release);
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
                    "ccg internal error: the libclang engine is not loaded (every "
                    "clang call needs a live Index - file a bug if you see this)");
            }
            return *t;
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

    /**
     * @brief 拥有一个 CXString；转换为 C 串 / 视图 / std::string。
     *
     * @internal 探针验证过的形状；因用户声明的析构抑制隐式移动构造而
     * 逐字写出（删除拷贝与移动）。
     */
    class Str
    {
    public:
        explicit Str(CXString s) noexcept : s_{s} {}
        ~Str() { detail::api().clang_disposeString(s_); }
        Str(const Str&) = delete;
        Str& operator=(const Str&) = delete;
        Str(Str&&) = delete;
        Str& operator=(Str&&) = delete;

        [[nodiscard]]
        const char* c_str() const noexcept
        {
            const char* p = detail::api().clang_getCString(s_);
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
        CXString s_;
    };

    namespace detail
    {

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
     */
    class Tu
    {
    public:
        Tu() = default;
        ~Tu()
        {
            if (tu_ != nullptr)
            {
                detail::api().clang_disposeTranslationUnit(tu_);
            }
        }
        Tu(const Tu&) = delete;
        Tu& operator=(const Tu&) = delete;
        Tu(Tu&& other) noexcept : tu_{other.tu_}, file_{std::move(other.file_)}
        {
            other.tu_ = nullptr;
        }
        Tu& operator=(Tu&& other) noexcept
        {
            if (this != &other)
            {
                if (tu_ != nullptr)
                {
                    detail::api().clang_disposeTranslationUnit(tu_);
                }
                tu_ = other.tu_;
                file_ = std::move(other.file_);
                other.tu_ = nullptr;
            }
            return *this;
        }

        [[nodiscard]]
        bool valid() const noexcept
        {
            return tu_ != nullptr;
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
            return detail::api().clang_getTranslationUnitCursor(tu_);
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
            const unsigned count = detail::api().clang_getNumDiagnostics(tu_);
            for (unsigned i = 0; i < count; ++i)
            {
                const CXDiagnostic d =
                    detail::api().clang_getDiagnostic(tu_, i);
                const auto severity = static_cast<DiagSeverity>(
                    detail::api().clang_getDiagnosticSeverity(d));
                if (severity >= min_severity)
                {
                    out.push_back(
                        detail::make_diagnostic(d, severity, with_fixits));
                    if (limit != 0 && out.size() >= limit)
                    {
                        detail::api().clang_disposeDiagnostic(d);
                        break;
                    }
                }
                detail::api().clang_disposeDiagnostic(d);
            }
            return out;
        }

    private:
        friend class Index;
        Tu(CXTranslationUnit tu, std::string file) noexcept
            : tu_{tu}, file_{std::move(file)}
        {
        }

        CXTranslationUnit tu_ = nullptr;
        std::string       file_;
    };

    /**
     * @brief 拥有一个 CXIndex。必须活过由它创建的每个 Tu。不可移动：
     * 一个索引对应一个解析会话即预期用法。
     *
     * @internal 构造获取进程引擎（尚无显式加载时经默认解析链加载）；
     * 最后一个存活 Index 的析构再次卸载引擎库。构造参数 (0, 0) = 保留
     * PCH 声明、不打印 libclang 侧诊断（诊断经 Tu::diagnostics 收集）。
     * 崩溃恢复不是 createIndex 参数——它每次引擎加载武装一次
     * （detail::load_locked 调 clang_toggleCrashRecovery）。
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
            CXTranslationUnit raw = nullptr;
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
                &raw);
            if (rc != CXError_Success || raw == nullptr)
            {
                const auto code = static_cast<ParseErrc>(
                    raw == nullptr && rc == CXError_Success ? CXError_Failure
                                                            : rc);
                throw ParseError(
                    std::format(
                        "libclang failed to parse {}: error code {}",
                        path,
                        static_cast<int>(code)),
                    code);
            }
            return Tu{raw, path};
        }

    private:
        CXIndex idx_;
    };

    /**
     * @name 游标门面
     * 基于 const CXCursor& 的自由函数；cg.extract 绝不直接调 clang_*
     * 函数。
     */
    /** @{ */
    [[nodiscard]]
    auto kind(const CXCursor& c) noexcept -> CXCursorKind
    {
        return detail::api().clang_getCursorKind(c);
    }

    [[nodiscard]]
    auto kind_spelling(CXCursorKind k) -> std::string
    {
        return Str{detail::api().clang_getCursorKindSpelling(k)}.std_str();
    }

    [[nodiscard]]
    auto spelling(const CXCursor& c) -> std::string
    {
        return Str{detail::api().clang_getCursorSpelling(c)}.std_str();
    }

    [[nodiscard]]
    auto display_name(const CXCursor& c) -> std::string
    {
        return Str{detail::api().clang_getCursorDisplayName(c)}.std_str();
    }

    [[nodiscard]]
    auto is_definition(const CXCursor& c) noexcept -> bool
    {
        return detail::api().clang_isCursorDefinition(c) != 0;
    }

    [[nodiscard]]
    auto is_const_method(const CXCursor& c) noexcept -> bool
    {
        return detail::api().clang_CXXMethod_isConst(c) != 0;
    }

    [[nodiscard]]
    auto is_virtual_method(const CXCursor& c) noexcept -> bool
    {
        return detail::api().clang_CXXMethod_isVirtual(c) != 0;
    }

    [[nodiscard]]
    auto is_pure_virtual(const CXCursor& c) noexcept -> bool
    {
        return detail::api().clang_CXXMethod_isPureVirtual(c) != 0;
    }

    [[nodiscard]]
    auto is_static_method(const CXCursor& c) noexcept -> bool
    {
        return detail::api().clang_CXXMethod_isStatic(c) != 0;
    }

    /**
     * @brief 显式默认（"= default"）成员。
     *
     * @internal clang 逐 TU 惰性补全类内默认定义，故仅
     * clang_isCursorDefinition 会漏掉只包含头文件的 TU 中的它们。
     */
    [[nodiscard]]
    auto is_defaulted_method(const CXCursor& c) noexcept -> bool
    {
        return detail::api().clang_CXXMethod_isDefaulted(c) != 0;
    }

    [[nodiscard]]
    auto is_destructor(const CXCursor& c) noexcept -> bool
    {
        return detail::api().clang_getCursorKind(c) == CXCursor_Destructor;
    }

    /** @brief 成员/基说明符游标的访问性（其他游标为 Invalid）。 */
    [[nodiscard]]
    auto access_of(const CXCursor& c) noexcept -> Access
    {
        return static_cast<Access>(
            detail::api().clang_getCXXAccessSpecifier(c));
    }

    /**
     * @brief CXXBaseSpecifier 游标的虚基位。
     *
     * @internal C API 仅部分绑定提供；此处一次调用即得——非基游标静默
     * 降级返回 false。
     */
    [[nodiscard]]
    auto is_virtual_base(const CXCursor& c) noexcept -> bool
    {
        return detail::api().clang_isVirtualBase(c) != 0;
    }

    /**
     * @brief c 所指（或所声明）实体的定义游标；本翻译单元内不可达任何
     * 定义时为空游标。
     */
    [[nodiscard]]
    auto definition_of(const CXCursor& c) noexcept -> CXCursor
    {
        return detail::api().clang_getCursorDefinition(c);
    }

    /**
     * @brief 两游标是否同一实体（逐字段等价比较）。
     *
     * @internal clang_equalCursors 是 dllimport 符号（本模块不链接导入库，
     * 动态加载其余 API），等价的字段比较：kind/xdata/data 三元组相等即同
     * 一游标（clang-c/Index.h 的官方实现语义）。
     */
    [[nodiscard]]
    inline auto equal_cursors(const CXCursor& a, const CXCursor& b) noexcept
        -> bool
    {
        if (a.kind != b.kind || a.xdata != b.xdata)
        {
            return false;
        }
        for (int i = 0; i < 3; ++i)
        {
            if (a.data[i] != b.data[i])
            {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief 函数型游标的参数类型拼写（按序）。
     *
     * 非函数游标与无参函数都返回空；调用方无法仅凭返回值区分这两种
     * 情形（用游标 kind 判断）。
     *
     * @internal clang_getNumArgTypes 对一切非函数类型（记录、依赖/未解析
     * 类型、null 类型）回答 -1，而把 size_t 重解释的 -1 拿去 reserve 会
     * 抛 std::length_error——reserve 前先防护。
     */
    [[nodiscard]]
    auto arg_type_spellings(const CXCursor& c) -> std::vector<std::string>
    {
        const CXType             t = detail::api().clang_getCursorType(c);
        const int                n = detail::api().clang_getNumArgTypes(t);
        std::vector<std::string> out;
        if (n > 0)
        {
            out.reserve(static_cast<std::size_t>(n));
        }
        for (int i = 0; i < n; ++i)
        {
            out.push_back(
                Str{detail::api().clang_getTypeSpelling(
                        detail::api().clang_getArgType(t, i))}
                    .std_str());
        }
        return out;
    }

    /**
     * @brief 此（虚）成员函数游标覆写的方法，直接基优先。
     *
     * 非虚函数或无基声明该签名时为空。
     *
     * @internal libclang 解析整条继承链，含 ~D -> ~B 映射。
     */
    [[nodiscard]]
    auto overridden(const CXCursor& c) -> std::vector<CXCursor>
    {
        CXCursor* cursors = nullptr;
        unsigned  count = 0;
        detail::api().clang_getOverriddenCursors(c, &cursors, &count);
        std::vector<CXCursor> out;
        if (static_cast<std::int64_t>(count) > 0)
        {
            out.reserve(count);
        }
        for (unsigned i = 0; i < count; ++i)
        {
            out.push_back(cursors[i]);
        }
        detail::api().clang_disposeOverriddenCursors(cursors);
        return out;
    }

    /** @brief 特化化游标的模板实参数（非特化游标为 -1）。 */
    [[nodiscard]]
    auto num_template_arguments(const CXCursor& c) noexcept -> int
    {
        return detail::api().clang_Cursor_getNumTemplateArguments(c);
    }

    /** @brief 特化游标第 i 个模板实参类型的拼写（非类型/值实参与坏下标
     *  为 ""）。 */
    [[nodiscard]]
    auto template_argument_type_spelling(const CXCursor& c, int i)
        -> std::string
    {
        if (i < 0 || i >= detail::api().clang_Cursor_getNumTemplateArguments(c))
        {
            return {};
        }
        return Str{
            detail::api().clang_getTypeSpelling(
                detail::api().clang_Cursor_getTemplateArgumentType(c, i))}
            .std_str();
    }

    /**
     * @brief 二元运算游标的运算符码（非二元运算游标为 CX_BO_Invalid）。
     *
     * @internal opcode 是 Sema 建树时定在 AST 节点上的权威判定，不经
     * 源码词法化，宏展开造成的 extent 错位碰不到它——赋值判定（原
     * "'=' 出现在圆括号深度零"的 token 判据判的就是它）一律走这里。
     * 绑定旧名 clang_Cursor_getBinaryOpcode：新名
     * clang_getCursorBinaryOperatorKind 需 LLVM 21 起，旧名自 LLVM 3.6
     * 起全程在列且引擎实现即新名的转发。
     */
    [[nodiscard]]
    auto binary_opcode(const CXCursor& c) noexcept -> CX_BinaryOperatorKind
    {
        return detail::api().clang_Cursor_getBinaryOpcode(c);
    }

    namespace detail
    {
        /**
         * @brief 单语句级 token 扫描的 spelling 区间上限（字节）。
         *
         * @internal token_spellings 的全部消费点都是单表达式/单语句/单
         * 声明内的首符与形态判定，正常形态几十到几百字节；4KB 比实测
         * 最长的正常语句高一个数量级、比宏错位形态（数万字节，见
         * token_range_sane）低一个数量级，两侧都有安全边距。与
         * writes_through_deref 的 4096 节点上限同数量级（同为"防病态
         * 生成物"的界）。
         */
        constexpr unsigned max_statement_token_span = 4096;

        /** @brief 一处源位置的 spelling 落点（所属文件 + 文件内字节偏移）。 */
        struct SpellingEnd
        {
            CXFile   file = nullptr;
            unsigned off = 0;
        };

        [[nodiscard]]
        auto spelling_end(const CXSourceLocation loc) -> SpellingEnd
        {
            SpellingEnd out;
            api().clang_getSpellingLocation(
                loc,
                &out.file,
                nullptr,
                nullptr,
                &out.off);
            return out;
        }

        /**
         * @brief extent 两端的 spelling 位置是否构成同文件内的有效正向
         *        区间。
         *
         * @internal 跨文件区间（宏边界两端各在一处）clang_tokenize 本就
         * 产不出 token；同文件倒置区间只能词法出一个 token 且必属错位
         * 垃圾。两者对任何合法扫描都拿不到可用 token，拒绝是零代价的
         * 卫生边界。
         */
        [[nodiscard]]
        auto token_range_ordered(const CXSourceRange& range) -> bool
        {
            const SpellingEnd begin =
                spelling_end(api().clang_getRangeStart(range));
            const SpellingEnd end =
                spelling_end(api().clang_getRangeEnd(range));
            return begin.file != nullptr && end.file == begin.file
                   && begin.off <= end.off;
        }

        /**
         * @brief token_range_ordered 再加单语句级跨度上限。
         *
         * @internal clang_tokenize 按 range 两端的 spelling 偏移决定词法
         * 化区间：宏展开生成的表达式（SWIG PYTHON 后端把桥接 API 生成
         * #define，调用点全部经宏展开）其 extent 两端各落在相距数万字
         * 节的不同宏定义处，被词法化的是两个定义点之间的无关源码——
         * 单次调用产出数千至上万 token、数十毫秒，且 token 内容与本游
         * 标无关，任何判定消费它都是误报。跨度越界即拒绝 tokenize（调
         * 用方拿空 token 序列，与"宁漏勿误"的判定纪律一致）。
         */
        [[nodiscard]]
        auto token_range_sane(const CXSourceRange& range) -> bool
        {
            const SpellingEnd begin =
                spelling_end(api().clang_getRangeStart(range));
            const SpellingEnd end =
                spelling_end(api().clang_getRangeEnd(range));
            return begin.file != nullptr && end.file == begin.file
                   && begin.off <= end.off
                   && end.off - begin.off <= max_statement_token_span;
        }
    }

    /**
     * @brief 覆盖游标 extent 的原始 token 拼写（语句级卫生闸内）。
     *
     * @internal 用于对预处理指令与单表达式/单语句做 token 扫描——部分
     * libclang 构建上 INCLUSION_DIRECTIVE 元数据不可靠。extent 两端
     * spelling 位置错位（宏展开形态）或跨度超过单语句语义上限时返回
     * 空：错位区间的 token 是别处宏定义的源码，消费它只会误报（闸的
     * 语义论证见 detail::token_range_sane）。
     */
    [[nodiscard]]
    auto token_spellings(const CXCursor& c) -> std::vector<std::string>
    {
        std::vector<std::string> out;
        const CXTranslationUnit  tu =
            detail::api().clang_Cursor_getTranslationUnit(c);
        if (tu == nullptr)
        {
            return out;
        }
        const CXSourceRange range = detail::api().clang_getCursorExtent(c);
        if (!detail::token_range_sane(range))
        {
            return out;
        }
        CXToken* tokens = nullptr;
        unsigned count = 0;
        detail::api().clang_tokenize(tu, range, &tokens, &count);
        for (unsigned i = 0; i < count; ++i)
        {
            out.push_back(
                Str{detail::api().clang_getTokenSpelling(tu, tokens[i])}
                    .std_str());
        }
        detail::api().clang_disposeTokens(tu, tokens, count);
        return out;
    }

    /**
     * @brief 被引用名在源码里是否显式写了嵌套名限定符
     *        （ns:: / Base:: / ::）。
     *
     * 判据是纯 AST 位置信息：带 WantQualifier 标志的引用名范围会把
     * nested-name-specifier 并入首段，与裸名范围不等即限定符在场。
     *
     * @internal 与 token 判据（extent 词法化找 "::"）语义同面且互补：
     * 本判据不经词法化，不受宏展开的 spelling 错位影响（token 侧在错位
     * 形态下被卫生闸拒绝而漏报），实参限定天然排除（只看被引用名自身）。
     * 非引用形态（指针间接调用等）两范围同为兜底值或 null，返回 false
     * 与"未写限定"同面。已知缺口：依赖重载集（UnresolvedLookupExpr）
     * 取不到限定符位置，两范围相同恒返回 false——由调用方在被调子式的
     * 限定符子游标（NamespaceRef）判据互补。
     */
    [[nodiscard]]
    auto reference_name_includes_qualifier(const CXCursor& c) -> bool
    {
        const CXSourceRange bare =
            detail::api().clang_getCursorReferenceNameRange(c, 0, 0);
        const CXSourceRange with_qualifier =
            detail::api().clang_getCursorReferenceNameRange(
                c,
                CXNameRange_WantQualifier | CXNameRange_WantSinglePiece,
                0);
        if (detail::api().clang_Range_isNull(bare)
            || detail::api().clang_Range_isNull(with_qualifier))
        {
            return false;
        }
        return !detail::api().clang_equalRanges(bare, with_qualifier);
    }

    /**
     * @brief 声明游标的 AST 重打印文本（编译器按语义重建，宏已展开）。
     *
     * 非声明形态返回空串。释放义务（CXString）由 Str 封装履行。
     *
     * @internal 与源文本的关键差别：打印走的是语义 AST，说明符以展开后
     * 的形态输出——`#define CXP constexpr` 生成的说明符在打印文本里就是
     * constexpr 本词，而源文件前缀里只有宏别名。函数**定义**的打印含整
     * 个函数体，只应消费声明头的判据必须自行截断（见 extract 侧
     * is_constexpr_decl 的截体与剥字符串两道精化）。打印策略传空指针：
     * 接口文档明确此时用默认策略（与 clang_getCursorPrintingPolicy 同源
     * 构造，均取自 TU 上下文的默认 PrintingPolicy），可免一次策略堆分配
     * 与释放。
     */
    [[nodiscard]]
    auto pretty_printed_decl(const CXCursor& c) -> std::string
    {
        return Str{detail::api().clang_getCursorPrettyPrinted(c, nullptr)}
            .std_str();
    }

    /**
     * @brief 声明前导说明符段的原始源文本（extent 起点 到 游标名字
     * 位置 的文件字节直读）。
     *
     * 范围不能当作同文件纯文本信任时（extent 与名字在不同文件、缓冲
     * 不可用、或名字拼写不在其文件位置——即宏生成的声明）返回
     * nullopt。
     *
     * @internal
     * 消费者是显式 defaulted 成员的说明符检测：打印器对 "= default"
     * 分支结构性丢弃 constexpr 说明符（实测各 PrintingPolicy 开关下
     * 均如此；"= delete" 与常规函数保留），此路只对 defaulted 形态
     * 直读源文本兜底。直读是文件缓冲上的字节扫描，零 tokenize——
     * clang_tokenize 每次调用都从文件头重新词法化，对多定义文件是
     * 平方级（SWIG 生成的翻译单元曾把 68 秒访问时间的 99% 花在
     * 6.6 千次这样的扫描上），绝不做它的回退。名字必须恰好写在
     * 它的文件位置所指处：拼写在别处（#define 内）意味着声明由宏
     * 生成，原始字节范围证明不了任何事。
     */
    [[nodiscard]]
    auto specifier_prefix_text(const CXCursor& c) -> std::optional<std::string>
    {
        const CXTranslationUnit tu =
            detail::api().clang_Cursor_getTranslationUnit(c);
        if (tu == nullptr)
        {
            return std::nullopt;
        }
        const CXSourceRange extent = detail::api().clang_getCursorExtent(c);
        CXFile              begin_file = nullptr, name_file = nullptr;
        unsigned            begin_off = 0, name_off = 0;
        detail::api().clang_getFileLocation(
            detail::api().clang_getRangeStart(extent),
            &begin_file,
            nullptr,
            nullptr,
            &begin_off);
        detail::api().clang_getFileLocation(
            detail::api().clang_getCursorLocation(c),
            &name_file,
            nullptr,
            nullptr,
            &name_off);
        if (begin_file == nullptr || begin_file != name_file
            || name_off < begin_off)
        {
            return std::nullopt;
        }
        CXFile   spell_file = nullptr;
        unsigned spell_off = 0;
        detail::api().clang_getSpellingLocation(
            detail::api().clang_getCursorLocation(c),
            &spell_file,
            nullptr,
            nullptr,
            &spell_off);
        if (spell_file != begin_file || spell_off != name_off)
        {
            return std::nullopt;
        }
        std::size_t size = 0;
        const char* data =
            detail::api().clang_getFileContents(tu, begin_file, &size);
        if (data == nullptr || begin_off >= size)
        {
            return std::nullopt;
        }
        const std::size_t end = std::min<std::size_t>(name_off, size);
        return std::string{data + begin_off, end - begin_off};
    }

    [[nodiscard]]
    auto semantic_parent(const CXCursor& c) noexcept -> CXCursor
    {
        return detail::api().clang_getCursorSemanticParent(c);
    }

    [[nodiscard]]
    auto lexical_parent(const CXCursor& c) noexcept -> CXCursor
    {
        return detail::api().clang_getCursorLexicalParent(c);
    }

    [[nodiscard]]
    auto referenced(const CXCursor& c) noexcept -> CXCursor
    {
        return detail::api().clang_getCursorReferenced(c);
    }

    /**
     * @brief 此游标特化自的主模板（若有）；否则空游标（见 is_null）。
     *
     * @internal c 是模板实例化（非模板本尊）时返回来源模板声明——隐式/
     * 显式实例化的函数与记录体随使用方 TU 展开在 AST 上，referenced 游标
     * 即实例 decl；本尊（模板定义处）返回空。是"界外但可见"与"界外且
     * 不可见"的分野点。
     */
    [[nodiscard]]
    auto specialized_template(const CXCursor& c) noexcept -> CXCursor
    {
        return detail::api().clang_getSpecializedCursorTemplate(c);
    }

    /**
     * @brief 游标的 USR（统一符号解析名）；无 USR 或垃圾游标为空串。
     *
     * @internal 同一声明的 USR 在同一 TU 内稳定，是跨访问形态（表达式引用
     * 与声明游标）比对字段/变量身份的依据。
     */
    [[nodiscard]]
    auto usr(const CXCursor& c) -> std::string
    {
        return Str{detail::api().clang_getCursorUSR(c)}.std_str();
    }

    /**
     * @brief 空游标判定：C API 的 null 与值初始化的空游标都视为空。
     *
     * @internal `clang_Cursor_isNull` 只把 `CXCursor_NoDeclFound(71)` 当作
     * null；值初始化的 `CXCursor{}` 其 `kind == 0`（不是任何有效游标
     * 种类），此前会被误判为非空，导致 analysis.collect 的
     * `referenced_callable` 对 VarDecl/FieldDecl 返回的空游标被当成真实
     * 声明。kind==0 在 libclang 中永远不是有效游标，两个约定一起接受。
     */
    [[nodiscard]]
    auto is_null(const CXCursor& c) noexcept -> bool
    {
        return c.kind == 0 || detail::api().clang_Cursor_isNull(c) != 0;
    }

    [[nodiscard]]
    auto location(const CXCursor& c) -> SourceLocation
    {
        return detail::file_location(detail::api().clang_getCursorLocation(c));
    }

    [[nodiscard]]
    auto extent(const CXCursor& c) -> SourceExtent
    {
        const CXSourceRange range = detail::api().clang_getCursorExtent(c);
        return SourceExtent{
            .begin =
                detail::file_location(detail::api().clang_getRangeStart(range)),
            .end =
                detail::file_location(detail::api().clang_getRangeEnd(range))};
    }

    /**
     * @brief 游标类型的拼写（函数游标得到的是完整函数类型，如
     * "int (ns::Widget &)"）。
     */
    [[nodiscard]]
    auto type_spelling(const CXCursor& c) -> std::string
    {
        return Str{detail::api().clang_getTypeSpelling(
                       detail::api().clang_getCursorType(c))}
            .std_str();
    }

    /** @brief 函数型游标的返回类型拼写。 */
    [[nodiscard]]
    auto result_type_spelling(const CXCursor& c) -> std::string
    {
        return Str{detail::api().clang_getTypeSpelling(
                       detail::api().clang_getCursorResultType(c))}
            .std_str();
    }

    /** @brief typedef 底层类型的拼写（非 typedef 游标为 ""）。 */
    [[nodiscard]]
    auto underlying_type_spelling(const CXCursor& c) -> std::string
    {
        return Str{detail::api().clang_getTypeSpelling(
                       detail::api().clang_getTypedefDeclUnderlyingType(c))}
            .std_str();
    }
    /** @} */

    namespace detail
    {

        /**
         * @brief 声明类型记录走查的一个递归步。
         *
         * 下穿 cv/引用/指针/数组/typedef 糖，收集记录声明游标，并跟随模板
         * 实参类型，使模板特化内嵌套的项目类型（std::vector`<Widget>` ->
         * Widget）也浮现。依赖类型（模板参数）与非记录类型终止该分支。
         *
         * @internal
         * 有界：深度上限防病态糖链，记录上限防深嵌套实参表。模板特化类型
         * 在此 libclang 构建上以 Unexposed"按书写"形态浮现
         * （std::vector`<Widget>`、Box`<int>`）：书写的模板实参保持可查、规范
         * 类型是 Record 节点（声明 + 完整实参表、含默认实参）——两者都走，
         * USR 去重吸收重叠；但绝不递归进另一个 Unexposed 的规范类型（依赖
         * 名规范化为自身，那不会终止）。'int Widget::*' /
         * 'void (Widget::*)(Widget&)'：类类型命名拥有式记录；指针所指（数据
         * 或函数类型）携带自身引用、按任何声明类型一样走查。
         * clang_Type_getClassType 是抵达该类类型的唯一 C API 通路，但
         * libclang（23.x 上观察到）在成员指针为依赖时于其内部解引用
         * NULL——即未实例化模板模式中的 'void (U::*)(void)' 参数形状，其
         * 类半边是模板参数上的嵌名说明符。C API 无法在调用前检测该依赖性
         * （彼处指针所指是普通非依赖类型，也没有类型级查询暴露类半边的
         * 形状），故该调用在硬件错误防护下运行而非被丢弃：一个依赖模式
         * 损失的恰是这一条拥有式记录边（记录在那里反正未解析），而每个
         * 非依赖成员指针保住它的 USES 边——此走查为之存在的铸边。引擎在
         * 被防护的错误后继续服务（探针验证：同一 TU 上稍后的非依赖类
         * 类型查询正常回答），走查与访问的其余部分不受扰。回调形状：
         * std::function<Gadget()> 与 std::function<void(Widget&)> 把函数类型
         * 暴露为模板实参，函数指针/typedef 类型直接内嵌它；记录在结果类型
         * 与每个参数类型中浮现。
         */
        void record_decls_of_type(
            const CXType&                    t,
            int                              depth,
            std::vector<CXCursor>&           out,
            std::unordered_set<std::string>& seen)
        {
            constexpr int         MAX_DEPTH = 8;
            constexpr std::size_t MAX_RECORDS = 32;
            if (depth > MAX_DEPTH || out.size() >= MAX_RECORDS)
            {
                return;
            }
            switch (t.kind)
            {
            case CXType_Pointer:
            case CXType_LValueReference:
            case CXType_RValueReference:
                record_decls_of_type(
                    api().clang_getPointeeType(t),
                    depth + 1,
                    out,
                    seen);
                return;
            case CXType_ConstantArray:
            case CXType_IncompleteArray:
            case CXType_VariableArray:
            case CXType_DependentSizedArray:
                record_decls_of_type(
                    api().clang_getElementType(t),
                    depth + 1,
                    out,
                    seen);
                return;
            case CXType_Elaborated:
                record_decls_of_type(
                    api().clang_Type_getNamedType(t),
                    depth + 1,
                    out,
                    seen);
                return;
            case CXType_Typedef:
            case CXType_Auto:
            {
                const CXType canon = api().clang_getCanonicalType(t);
                if (canon.kind != t.kind)
                { // 未推导的 auto 规范化为自身
                    record_decls_of_type(canon, depth + 1, out, seen);
                }
                return;
            }
            case CXType_Attributed:
                record_decls_of_type(
                    api().clang_Type_getModifiedType(t),
                    depth + 1,
                    out,
                    seen);
                return;
            case CXType_Record:
            {
                const CXCursor decl = api().clang_getTypeDeclaration(t);
                if (!api().clang_Cursor_isNull(decl))
                {
                    const std::string usr =
                        Str{api().clang_getCursorUSR(decl)}.std_str();
                    if (!usr.empty() && seen.insert(usr).second)
                    {
                        out.push_back(decl);
                    }
                }
                const int argc = api().clang_Type_getNumTemplateArguments(t);
                for (int i = 0; i < argc && out.size() < MAX_RECORDS; ++i)
                {
                    record_decls_of_type(
                        api().clang_Type_getTemplateArgumentAsType(t, i),
                        depth + 1,
                        out,
                        seen);
                }
                return;
            }
            case CXType_Unexposed:
            {
                const int argc = api().clang_Type_getNumTemplateArguments(t);
                for (int i = 0; i < argc && out.size() < MAX_RECORDS; ++i)
                {
                    record_decls_of_type(
                        api().clang_Type_getTemplateArgumentAsType(t, i),
                        depth + 1,
                        out,
                        seen);
                }
                const CXType canon = api().clang_getCanonicalType(t);
                if (canon.kind == CXType_Record)
                {
                    record_decls_of_type(canon, depth + 1, out, seen);
                }
                return;
            }
            case CXType_MemberPointer:
            {
                CXType cls{};
                if (cg::util::guard::run_guarded(
                        [&] { cls = api().clang_Type_getClassType(t); }))
                {
                    record_decls_of_type(cls, depth + 1, out, seen);
                }
                record_decls_of_type(
                    api().clang_getPointeeType(t),
                    depth + 1,
                    out,
                    seen);
                return;
            }
            case CXType_FunctionProto:
            case CXType_FunctionNoProto:
            {
                record_decls_of_type(
                    api().clang_getResultType(t),
                    depth + 1,
                    out,
                    seen);
                const int argc = api().clang_getNumArgTypes(t);
                for (int i = 0; i < argc && out.size() < MAX_RECORDS; ++i)
                {
                    record_decls_of_type(
                        api().clang_getArgType(t, static_cast<unsigned>(i)),
                        depth + 1,
                        out,
                        seen);
                }
                return;
            }
            default:
                return; // 内建、枚举、依赖名等
            }
        }

    } // namespace detail

    /**
     * @brief c 的声明类型引用的每个记录的声明游标（USES 边目标）。
     *
     * 函数型游标取结果类型，字段/变量/参数取声明类型。下穿途中剥掉
     * cv/引用/指针/数组包装（Widget*、const Widget& 与 Widget[4] 都引用
     * Widget），typedef 经规范类型解析，模板实参类型递归走查（Box<
     * Widget> 字段同时引用 Box 与 Widget），成员指针引用其类类型
     * （int Widget::* 引用 Widget），函数类型经结果 + 参数走查
     * （std::function<Gadget()> 与接受 Widget& 的回调引用 Gadget/Widget）。
     * 特化游标原样返回；把它们折叠到主模板是调用方的策略（它持有项目
     * 允许清单）。顺序为类型走查序、按 USR 去重。
     *
     * @internal 函数型游标取结果类型（构造/析构与非函数上为 Invalid，
     * 它们改取自身声明类型——构造函数的游标类型是其函数类型，走查无
     * 所获）。
     */
    [[nodiscard]]
    auto declared_type_record_decls(const CXCursor& c) -> std::vector<CXCursor>
    {
        std::vector<CXCursor> out;
        CXType                t = detail::api().clang_getCursorResultType(c);
        if (t.kind == CXType_Invalid)
        {
            t = detail::api().clang_getCursorType(c);
        }
        std::unordered_set<std::string> seen;
        detail::record_decls_of_type(t, 0, out, seen);
        return out;
    }

    /**
     * @brief 类型形状的粗分类（cg.analysis 的类型线索门面）。
     *
     * @internal 不导出裸 CXType：调用方只需要形状判别，具体类型查询走下方
     * 返回游标的包装。值刻意按 clang-c 的常用族分组，非逐 kind 镜像。
     */
    enum class TypeClass : std::uint8_t
    {
        Invalid = 0,
        Pointer,
        LValueReference,
        RValueReference,
        Record,
        Enumeration,
        Typedef,
        Auto,
        Elaborated,
        Unexposed,
        Dependent,
        MemberPointer,
        Array,
        Function,
        Builtin,
        Other,
    };

    namespace detail
    {

        auto classify_type(CXType t) -> TypeClass
        {
            switch (t.kind)
            {
            case CXType_Invalid:
                return TypeClass::Invalid;
            case CXType_Pointer:
                return TypeClass::Pointer;
            case CXType_LValueReference:
                return TypeClass::LValueReference;
            case CXType_RValueReference:
                return TypeClass::RValueReference;
            case CXType_Record:
                return TypeClass::Record;
            case CXType_Enum:
                return TypeClass::Enumeration;
            case CXType_Typedef:
                return TypeClass::Typedef;
            case CXType_Auto:
                return TypeClass::Auto;
            case CXType_Elaborated:
                return TypeClass::Elaborated;
            case CXType_Unexposed:
                return TypeClass::Unexposed;
            case CXType_Dependent:
                return TypeClass::Dependent;
            case CXType_MemberPointer:
                return TypeClass::MemberPointer;
            case CXType_ConstantArray:
            case CXType_IncompleteArray:
            case CXType_VariableArray:
            case CXType_DependentSizedArray:
                return TypeClass::Array;
            case CXType_FunctionProto:
            case CXType_FunctionNoProto:
                return TypeClass::Function;
            default:
                return CXType_FirstBuiltin <= t.kind
                               && t.kind <= CXType_LastBuiltin
                           ? TypeClass::Builtin
                           : TypeClass::Other;
            }
        }

        /**
         * @brief 剥掉指针/引用/elaborated/typedef/attributed 糖，停在记录/枚举
         *        或第一个不可解的 kind 上。
         *
         * @internal 绝不调用 clang_Type_getClassType（依赖成员指针上的已知崩溃
         * 面）；这里只需要记录声明。
         */
        auto peel_type(CXType t) -> CXType
        {
            for (int depth = 0; depth < 8; ++depth)
            {
                switch (t.kind)
                {
                case CXType_Pointer:
                case CXType_LValueReference:
                case CXType_RValueReference:
                    t = api().clang_getPointeeType(t);
                    break;
                case CXType_Elaborated:
                    t = api().clang_Type_getNamedType(t);
                    break;
                case CXType_Attributed:
                    t = api().clang_Type_getModifiedType(t);
                    break;
                case CXType_Typedef:
                case CXType_Auto:
                {
                    const CXType canon = api().clang_getCanonicalType(t);
                    if (canon.kind == t.kind || canon.kind == CXType_Invalid)
                    {
                        return t;
                    }
                    t = canon;
                    break;
                }
                case CXType_Unexposed:
                {
                    const CXType canon = api().clang_getCanonicalType(t);
                    if (canon.kind == CXType_Unexposed
                        || canon.kind == CXType_Invalid)
                    {
                        return t;
                    }
                    t = canon;
                    break;
                }
                default:
                    return t;
                }
            }
            return t;
        }

        auto declaration_of_type(CXType t) -> CXCursor
        {
            t = peel_type(t);
            if (t.kind != CXType_Record && t.kind != CXType_Enum)
            {
                return CXCursor{};
            }
            return api().clang_getTypeDeclaration(t);
        }

        /**
         * @brief 类型先规范再剥 layers 层指针后的拼写。
         *
         * @internal 只经 CXType_Pointer 下探（引用/数组/Invalid 中途终止）；
         * pointee 的 canonical 仍是 canonical，故无需逐层再规范化。模板模式体
         * 内的模板形参在此打印为
         * "type-parameter-`<depth>`-`<index>`"（canonical 无 identifier
         * 的结构性输出，C++ 标识符不含 '-' 不可伪造），调用方以此做
         * 零名字参与的形状判别。
         */
        auto canonical_peeled_spelling(CXType t, int layers)
            -> std::optional<std::string>
        {
            if (layers < 0 || t.kind == CXType_Invalid)
            {
                return std::nullopt;
            }
            CXType cur = api().clang_getCanonicalType(t);
            if (cur.kind == CXType_Invalid)
            {
                return std::nullopt;
            }
            for (int i = 0; i < layers; ++i)
            {
                if (cur.kind != CXType_Pointer)
                {
                    return std::nullopt;
                }
                cur = api().clang_getPointeeType(cur);
                if (cur.kind == CXType_Invalid)
                {
                    return std::nullopt;
                }
            }
            return Str{api().clang_getTypeSpelling(cur)}.std_str();
        }

    } // namespace detail

    /**
     * @brief 表达式静态类型的 canonical 拼写（不剥层）。
     *
     * @internal 重载归属裁决：模板上下文的调用点实参（T/T&/T* 等）
     * canonical 含 "type-parameter-"（依赖类型，C++ 标识符不含 '-' 不可
     * 伪造——与既有判据族同源）；无效类型返回空串。
     */
    [[nodiscard]]
    auto canonical_type_spelling(const CXCursor& c) -> std::string
    {
        CXType t = detail::api().clang_getCursorType(c);
        if (t.kind == CXType_Invalid)
        {
            return {};
        }
        t = detail::api().clang_getCanonicalType(t);
        if (t.kind == CXType_Invalid)
        {
            return {};
        }
        return Str{detail::api().clang_getTypeSpelling(t)}.std_str();
    }

    /** @brief 游标声明类型的规范形状（剥 typedef/auto）。 */
    [[nodiscard]]
    auto canonical_type_class(const CXCursor& c) -> TypeClass
    {
        const CXType t = detail::api().clang_getCursorType(c);
        return detail::classify_type(detail::api().clang_getCanonicalType(t));
    }

    /**
     * @brief 函数型游标结果类型的形状（不剥 typedef——依赖名的引用在
     *        canonical 化时会折叠成 Unexposed，raw kind 才保得住引用位）。
     *
     * @internal 非函数游标为 Invalid。消费方：透明转发函数判据的引用
     *           返回证据（模板转发器的 `T&&` raw kind 是 RValueReference，
     *           表达式侧的 cast 类型则已被依赖糖折叠，不可用）。
     */
    [[nodiscard]]
    auto result_type_class(const CXCursor& c) -> TypeClass
    {
        return detail::classify_type(
            detail::api().clang_getCursorResultType(c));
    }

    /**
     * @brief 游标声明类型的规范拼写，先剥 layers 层指针。
     *
     * 层数不足（中途遇到非指针/Invalid）为 nullopt；返回的拼写来自规范
     * 类型，模板形参呈现为 "type-parameter-`<depth>`-`<index>`" 标记
     * （detail::canonical_peeled_spelling 的判别语义）。
     */
    [[nodiscard]]
    auto canonical_pointee_spelling(const CXCursor& c, int layers)
        -> std::optional<std::string>
    {
        return detail::canonical_peeled_spelling(
            detail::api().clang_getCursorType(c),
            layers);
    }

    /** @brief 函数型游标结果类型的同款查询（非函数为 nullopt）。 */
    [[nodiscard]]
    auto canonical_result_pointee_spelling(const CXCursor& c, int layers)
        -> std::optional<std::string>
    {
        return detail::canonical_peeled_spelling(
            detail::api().clang_getCursorResultType(c),
            layers);
    }

    /** @brief 游标类型是否为指针或引用。 */
    [[nodiscard]]
    auto is_pointer_or_reference(const CXCursor& c) -> bool
    {
        const CXType t = detail::api().clang_getCursorType(c);
        return t.kind == CXType_Pointer || t.kind == CXType_LValueReference
               || t.kind == CXType_RValueReference;
    }

    /**
     * @brief 函数游标的结果类型是否为指针/引用。
     *
     * @internal 表达式游标（尤其 CXXOperatorCallExpr）的 `clang_getCursorType`
     * 在 libclang 里可能报成绑定成员函数类型而不是调用结果；判定“调用
     * 返回指针/引用”必须看被调用函数的 result type，否则容器解引用
     * （`operator*`）的静态元素类型会被误当成动态证据。
     */
    [[nodiscard]]
    auto result_is_pointer_or_reference(const CXCursor& c) -> bool
    {
        const CXType t = detail::api().clang_getCursorResultType(c);
        return t.kind == CXType_Pointer || t.kind == CXType_LValueReference
               || t.kind == CXType_RValueReference;
    }

    /** @brief 游标声明类型（剥糖后）的记录/枚举声明游标；否则空游标。 */
    [[nodiscard]]
    auto type_declaration(const CXCursor& c) -> CXCursor
    {
        return detail::declaration_of_type(
            detail::api().clang_getCursorType(c));
    }

    /** @brief 游标类型所指记录/枚举的声明游标；非指针/引用为空游标。 */
    [[nodiscard]]
    auto pointee_declaration(const CXCursor& c) -> CXCursor
    {
        const CXType t = detail::api().clang_getCursorType(c);
        if (t.kind != CXType_Pointer && t.kind != CXType_LValueReference
            && t.kind != CXType_RValueReference)
        {
            return CXCursor{};
        }
        return detail::declaration_of_type(
            detail::api().clang_getPointeeType(t));
    }

    /** @brief 函数型游标返回类型（剥糖后）的记录/枚举声明；否则空游标。 */
    [[nodiscard]]
    auto result_type_declaration(const CXCursor& c) -> CXCursor
    {
        return detail::declaration_of_type(
            detail::api().clang_getCursorResultType(c));
    }

    /**
     * @brief 游标类型的第 i 个模板实参（剥糖后）的记录/枚举声明；
     *        越界或无记录实参时为空游标。
     */
    [[nodiscard]]
    auto type_template_arg_declaration(const CXCursor& c, int i) -> CXCursor
    {
        if (i < 0)
        {
            return CXCursor{};
        }
        CXType t = detail::api().clang_getCursorType(c);
        int    argc = detail::api().clang_Type_getNumTemplateArguments(t);
        if (argc <= i)
        {
            t = detail::api().clang_getCanonicalType(t);
            argc = detail::api().clang_Type_getNumTemplateArguments(t);
        }
        if (argc <= i)
        {
            return CXCursor{};
        }
        return detail::declaration_of_type(
            detail::api().clang_Type_getTemplateArgumentAsType(
                t,
                static_cast<unsigned>(i)));
    }

    /**
     * @brief 访问 parent 的每个直接子（fn 返回 true 时含其整个子树）。
     *
     * 这是 cg.extract 赖以构建的递归走查原语。
     */
    void children(
        const CXCursor&                             parent,
        const std::function<bool(const CXCursor&)>& fn)
    {
        detail::VisitCtx ctx{&fn};
        detail::api().clang_visitChildren(parent, detail::visitor, &ctx);
    }

} // namespace cg::clang
