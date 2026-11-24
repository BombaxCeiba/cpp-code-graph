module;
#include <sqlpp23/core/name/create_name_tag.h>

/**
 * @module cg.db.tables.analysis
 *
 * @brief 图数据库表模型四组当中的分析产物组（命名空间 cg::db::tab），经入口模块
 *        cg.db.tables 再导出：传播槽终态（AnalysisSlots）、透传包装
 *        （PassthroughWrappers）、实例桥传入侧（ExternalParamArgs）、实例桥体内
 *        调用侧（ExternalInvokes）与符号贡献登记（SymbolContributions）五张表。
 *
 * 表模型的写法、名字标签为什么上提、全局模块片段为什么只有名字标签宏头，见入口
 * 模块 cg.db.tables 的说明。
 */
export module cg.db.tables.analysis;

import std;
import sqlpp23.core;

/**
 * @brief 本文件的 sqlpp23 表模型。
 *
 * @internal
 * 手写 sqlpp23 模式（正常应由 sqlpp23-ddl2cpp 生成），形状遵循 sqlpp23 自家生成
 * 头的形态。标识符刻意镜像生成头形状：尾下划线表结构体、camelCase 的 cpp 访问
 * 器；SQL 列名/表名取宏的第一个实参保持不变——改成项目 snake_case 风格会偏离这里
 * 手工镜像的生成形态。
 */
// NOLINTBEGIN(readability-identifier-naming)
namespace cg::db::tab
{

    export struct AnalysisSlots_OwnerSym_tag
    {
        SQLPP_NAME_TAG_GUTS(owner_sym, ownerSym);
    };
    export struct AnalysisSlots_VarName_tag
    {
        SQLPP_NAME_TAG_GUTS(var_name, varName);
    };
    export struct AnalysisSlots_TypeSet_tag
    {
        SQLPP_NAME_TAG_GUTS(type_set, typeSet);
    };
    export struct AnalysisSlots_Confidence_tag
    {
        SQLPP_NAME_TAG_GUTS(confidence, confidence);
    };
    export struct AnalysisSlots_Source_tag
    {
        SQLPP_NAME_TAG_GUTS(source, source);
    };
    export struct AnalysisSlots_Reason_tag
    {
        SQLPP_NAME_TAG_GUTS(reason, reason);
    };
    export struct AnalysisSlots_Truncated_tag
    {
        SQLPP_NAME_TAG_GUTS(truncated, truncated);
    };
    export struct AnalysisSlots_Ver_tag
    {
        SQLPP_NAME_TAG_GUTS(ver, ver);
    };
    export struct AnalysisSlots_AliasSrc_tag
    {
        SQLPP_NAME_TAG_GUTS(alias_src, aliasSrc);
    };
    export struct AnalysisSlots_RetSrc_tag
    {
        SQLPP_NAME_TAG_GUTS(ret_src, retSrc);
    };

    /**
     * @brief 传播槽终态持久化（schema v12）：每个 (owner_sym,
     * var_name) 传播槽一行，记录最近一次提交段的固定点输出。增量批装载它做
     *        warm start，非脏闭包槽直接沿用、闭包槽从初始值重算。
     *
     * @internal 除 type_set/truncated/ver 外，还持久化
     * confidence/source/reason（写回列的 byte 级等价需要槽级终态置信度，
     * 它们无法从 type_set 复原）与 alias_src/ret_src（上次运行解析出的
     * 函数内别名/返回值传播源槽键——删除/漂移检测的旧边见证）。
     * owner_sym 存符号 unique_key 文本而非 id：符号行被 purge 重建后 id
     * 漂移，键文本才是稳定身份。
     */
    export struct AnalysisSlots_
    {
        struct OwnerSym
        {
            using _sqlpp_name_tag = AnalysisSlots_OwnerSym_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::false_type;
        };
        struct VarName
        {
            using _sqlpp_name_tag = AnalysisSlots_VarName_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::false_type;
        };
        struct TypeSet
        {
            using _sqlpp_name_tag = AnalysisSlots_TypeSet_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::true_type;
        };
        struct Confidence
        {
            using _sqlpp_name_tag = AnalysisSlots_Confidence_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct Source
        {
            using _sqlpp_name_tag = AnalysisSlots_Source_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct Reason
        {
            using _sqlpp_name_tag = AnalysisSlots_Reason_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::true_type;
        };
        struct Truncated
        {
            using _sqlpp_name_tag = AnalysisSlots_Truncated_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::true_type;
        };
        struct Ver
        {
            using _sqlpp_name_tag = AnalysisSlots_Ver_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct AliasSrc
        {
            using _sqlpp_name_tag = AnalysisSlots_AliasSrc_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::true_type;
        };
        struct RetSrc
        {
            using _sqlpp_name_tag = AnalysisSlots_RetSrc_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::true_type;
        };
        SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(analysis_slots, analysisSlots);
        template <typename T>
        using _table_columns = ::sqlpp::table_columns<
            T,
            OwnerSym,
            VarName,
            TypeSet,
            Confidence,
            Source,
            Reason,
            Truncated,
            Ver,
            AliasSrc,
            RetSrc>;
        using _required_insert_columns = ::sqlpp::detail::type_set<
            ::sqlpp::column_t<::sqlpp::table_t<AnalysisSlots_>, OwnerSym>,
            ::sqlpp::column_t<::sqlpp::table_t<AnalysisSlots_>, VarName>,
            ::sqlpp::column_t<::sqlpp::table_t<AnalysisSlots_>, Confidence>,
            ::sqlpp::column_t<::sqlpp::table_t<AnalysisSlots_>, Source>,
            ::sqlpp::column_t<::sqlpp::table_t<AnalysisSlots_>, Ver>>;
    };
    export using AnalysisSlots = ::sqlpp::table_t<AnalysisSlots_>;

    export struct PassthroughWrappers_WrapperKey_tag
    {
        SQLPP_NAME_TAG_GUTS(wrapper_key, wrapperKey);
    };
    export struct PassthroughWrappers_GetterKey_tag
    {
        SQLPP_NAME_TAG_GUTS(getter_key, getterKey);
    };
    export struct PassthroughWrappers_GetterName_tag
    {
        SQLPP_NAME_TAG_GUTS(getter_name, getterName);
    };
    export struct PassthroughWrappers_FileId_tag
    {
        SQLPP_NAME_TAG_GUTS(file_id, fileId);
    };
    export struct PassthroughWrappers_Line_tag
    {
        SQLPP_NAME_TAG_GUTS(line, line);
    };
    export struct PassthroughWrappers_ArgIndex_tag
    {
        SQLPP_NAME_TAG_GUTS(arg_index, argIndex);
    };

    /** @brief passthrough_wrappers 表列（schema v13，槽位基础设施）。 */
    export struct PassthroughWrappers_
    {
        struct WrapperKey
        {
            using _sqlpp_name_tag = PassthroughWrappers_WrapperKey_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::false_type;
        };
        struct GetterKey
        {
            using _sqlpp_name_tag = PassthroughWrappers_GetterKey_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::false_type;
        };
        struct GetterName
        {
            using _sqlpp_name_tag = PassthroughWrappers_GetterName_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::false_type;
        };
        struct FileId
        {
            using _sqlpp_name_tag = PassthroughWrappers_FileId_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct Line
        {
            using _sqlpp_name_tag = PassthroughWrappers_Line_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct ArgIndex
        {
            using _sqlpp_name_tag = PassthroughWrappers_ArgIndex_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(
            passthrough_wrappers,
            passthroughWrappers);
        template <typename T>
        using _table_columns = ::sqlpp::table_columns<
            T,
            WrapperKey,
            GetterKey,
            GetterName,
            FileId,
            Line,
            ArgIndex>;
        using _required_insert_columns = ::sqlpp::detail::type_set<
            ::sqlpp::
                column_t<::sqlpp::table_t<PassthroughWrappers_>, WrapperKey>,
            ::sqlpp::
                column_t<::sqlpp::table_t<PassthroughWrappers_>, GetterKey>,
            ::sqlpp::
                column_t<::sqlpp::table_t<PassthroughWrappers_>, GetterName>,
            ::sqlpp::column_t<::sqlpp::table_t<PassthroughWrappers_>, FileId>,
            ::sqlpp::column_t<::sqlpp::table_t<PassthroughWrappers_>, Line>,
            ::sqlpp::
                column_t<::sqlpp::table_t<PassthroughWrappers_>, ArgIndex>>;
    };
    export using PassthroughWrappers = ::sqlpp::table_t<PassthroughWrappers_>;

    export struct ExternalParamArgs_HostUsr_tag
    {
        SQLPP_NAME_TAG_GUTS(host_usr, hostUsr);
    };
    export struct ExternalParamArgs_ParamIndex_tag
    {
        SQLPP_NAME_TAG_GUTS(param_index, paramIndex);
    };
    export struct ExternalParamArgs_CallerKey_tag
    {
        SQLPP_NAME_TAG_GUTS(caller_key, callerKey);
    };
    export struct ExternalParamArgs_TargetKey_tag
    {
        SQLPP_NAME_TAG_GUTS(target_key, targetKey);
    };
    export struct ExternalParamArgs_FileId_tag
    {
        SQLPP_NAME_TAG_GUTS(file_id, fileId);
    };
    export struct ExternalParamArgs_Line_tag
    {
        SQLPP_NAME_TAG_GUTS(line, line);
    };
    export struct ExternalParamArgs_Col_tag
    {
        SQLPP_NAME_TAG_GUTS(col, col);
    };

    /** @brief external_param_args 表列（schema v15，实例桥传入侧）。 */
    export struct ExternalParamArgs_
    {
        struct HostUsr
        {
            using _sqlpp_name_tag = ExternalParamArgs_HostUsr_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::false_type;
        };
        struct ParamIndex
        {
            using _sqlpp_name_tag = ExternalParamArgs_ParamIndex_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct CallerKey
        {
            using _sqlpp_name_tag = ExternalParamArgs_CallerKey_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::false_type;
        };
        struct TargetKey
        {
            using _sqlpp_name_tag = ExternalParamArgs_TargetKey_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::false_type;
        };
        struct FileId
        {
            using _sqlpp_name_tag = ExternalParamArgs_FileId_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct Line
        {
            using _sqlpp_name_tag = ExternalParamArgs_Line_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct Col
        {
            using _sqlpp_name_tag = ExternalParamArgs_Col_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(
            external_param_args,
            externalParamArgs);
        template <typename T>
        using _table_columns = ::sqlpp::table_columns<
            T,
            HostUsr,
            ParamIndex,
            CallerKey,
            TargetKey,
            FileId,
            Line,
            Col>;
        using _required_insert_columns = ::sqlpp::detail::type_set<
            ::sqlpp::column_t<::sqlpp::table_t<ExternalParamArgs_>, HostUsr>,
            ::sqlpp::column_t<::sqlpp::table_t<ExternalParamArgs_>, ParamIndex>,
            ::sqlpp::column_t<::sqlpp::table_t<ExternalParamArgs_>, CallerKey>,
            ::sqlpp::column_t<::sqlpp::table_t<ExternalParamArgs_>, TargetKey>,
            ::sqlpp::column_t<::sqlpp::table_t<ExternalParamArgs_>, FileId>,
            ::sqlpp::column_t<::sqlpp::table_t<ExternalParamArgs_>, Line>,
            ::sqlpp::column_t<::sqlpp::table_t<ExternalParamArgs_>, Col>>;
    };
    export using ExternalParamArgs = ::sqlpp::table_t<ExternalParamArgs_>;

    export struct ExternalInvokes_HostUsr_tag
    {
        SQLPP_NAME_TAG_GUTS(host_usr, hostUsr);
    };
    export struct ExternalInvokes_ParamIndex_tag
    {
        SQLPP_NAME_TAG_GUTS(param_index, paramIndex);
    };
    export struct ExternalInvokes_FileId_tag
    {
        SQLPP_NAME_TAG_GUTS(file_id, fileId);
    };
    export struct ExternalInvokes_Line_tag
    {
        SQLPP_NAME_TAG_GUTS(line, line);
    };
    export struct ExternalInvokes_Col_tag
    {
        SQLPP_NAME_TAG_GUTS(col, col);
    };

    /** @brief external_invokes 表列（schema v15，实例桥体内调用侧）。 */
    export struct ExternalInvokes_
    {
        struct HostUsr
        {
            using _sqlpp_name_tag = ExternalInvokes_HostUsr_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::false_type;
        };
        struct ParamIndex
        {
            using _sqlpp_name_tag = ExternalInvokes_ParamIndex_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct FileId
        {
            using _sqlpp_name_tag = ExternalInvokes_FileId_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct Line
        {
            using _sqlpp_name_tag = ExternalInvokes_Line_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct Col
        {
            using _sqlpp_name_tag = ExternalInvokes_Col_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(
            external_invokes,
            externalInvokes);
        template <typename T>
        using _table_columns =
            ::sqlpp::table_columns<T, HostUsr, ParamIndex, FileId, Line, Col>;
        using _required_insert_columns = ::sqlpp::detail::type_set<
            ::sqlpp::column_t<::sqlpp::table_t<ExternalInvokes_>, HostUsr>,
            ::sqlpp::column_t<::sqlpp::table_t<ExternalInvokes_>, ParamIndex>,
            ::sqlpp::column_t<::sqlpp::table_t<ExternalInvokes_>, FileId>,
            ::sqlpp::column_t<::sqlpp::table_t<ExternalInvokes_>, Line>,
            ::sqlpp::column_t<::sqlpp::table_t<ExternalInvokes_>, Col>>;
    };
    export using ExternalInvokes = ::sqlpp::table_t<ExternalInvokes_>;

    export struct SymbolContributions_SymId_tag
    {
        SQLPP_NAME_TAG_GUTS(sym_id, symId);
    };
    export struct SymbolContributions_FileId_tag
    {
        SQLPP_NAME_TAG_GUTS(file_id, fileId);
    };
    export struct SymbolContributions_CmdDigest_tag
    {
        SQLPP_NAME_TAG_GUTS(cmd_digest, cmdDigest);
    };

    /**
     * @brief 贡献登记表（schema v18，行生老病死记账制）：符号行 ×
     *        编译条目的一票一行。
     *
     * @internal 条目身份与 fingerprints/failures 同款 (file_id,
     *           cmd_digest) 对——双编译条目天然两票（同文件两条目各
     *           一票），条目重解析后按差集撤票/续票，票数归零的符号行
     *           在 dispatch 装载前删除（FK 级联清边/线索/站点）。PK 以
     *           sym_id 开头服务归零仲裁的 EXISTS 探测；entry 辅助索引
     *           服务逐条目的旧票读取（差集撤票的输入）。
     */
    export struct SymbolContributions_
    {
        struct SymId
        {
            using _sqlpp_name_tag = SymbolContributions_SymId_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct FileId
        {
            using _sqlpp_name_tag = SymbolContributions_FileId_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct CmdDigest
        {
            using _sqlpp_name_tag = SymbolContributions_CmdDigest_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::false_type;
        };
        SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(
            symbol_contributions,
            symbolContributions);
        template <typename T>
        using _table_columns =
            ::sqlpp::table_columns<T, SymId, FileId, CmdDigest>;
        using _required_insert_columns = ::sqlpp::detail::type_set<
            ::sqlpp::column_t<::sqlpp::table_t<SymbolContributions_>, SymId>,
            ::sqlpp::column_t<::sqlpp::table_t<SymbolContributions_>, FileId>,
            ::sqlpp::
                column_t<::sqlpp::table_t<SymbolContributions_>, CmdDigest>>;
    };
    export using SymbolContributions = ::sqlpp::table_t<SymbolContributions_>;

} // namespace cg::db::tab
// NOLINTEND(readability-identifier-naming)
