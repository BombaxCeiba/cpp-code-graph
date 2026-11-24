module;
#include <sqlpp23/core/name/create_name_tag.h>

/**
 * @module cg.db.tables.sites
 *
 * @brief 图数据库表模型四组当中的站点组（命名空间 cg::db::tab），经入口模块
 *        cg.db.tables 再导出：类型线索（TypeHints）与调用点（CallSites）两张
 *        表。
 *
 * 表模型的写法、名字标签为什么上提、全局模块片段为什么只有名字标签宏头，见入口
 * 模块 cg.db.tables 的说明。
 */
export module cg.db.tables.sites;

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

    export struct TypeHints_HintId_tag
    {
        SQLPP_NAME_TAG_GUTS(hint_id, hintId);
    };
    export struct TypeHints_OwnerId_tag
    {
        SQLPP_NAME_TAG_GUTS(owner_id, ownerId);
    };
    export struct TypeHints_VarName_tag
    {
        SQLPP_NAME_TAG_GUTS(var_name, varName);
    };
    export struct TypeHints_FileId_tag
    {
        SQLPP_NAME_TAG_GUTS(file_id, fileId);
    };
    export struct TypeHints_Line_tag
    {
        SQLPP_NAME_TAG_GUTS(line, line);
    };
    export struct TypeHints_Col_tag
    {
        SQLPP_NAME_TAG_GUTS(col, col);
    };
    export struct TypeHints_TypeSet_tag
    {
        SQLPP_NAME_TAG_GUTS(type_set, typeSet);
    };
    export struct TypeHints_Source_tag
    {
        SQLPP_NAME_TAG_GUTS(source, source);
    };
    export struct TypeHints_Confidence_tag
    {
        SQLPP_NAME_TAG_GUTS(confidence, confidence);
    };
    export struct TypeHints_FallbackReason_tag
    {
        SQLPP_NAME_TAG_GUTS(fallback_reason, fallbackReason);
    };
    export struct TypeHints_ParamIndex_tag
    {
        SQLPP_NAME_TAG_GUTS(param_index, paramIndex);
    };
    export struct TypeHints_RelatedVar_tag
    {
        SQLPP_NAME_TAG_GUTS(related_var, relatedVar);
    };
    export struct TypeHints_RelatedKey_tag
    {
        SQLPP_NAME_TAG_GUTS(related_key, relatedKey);
    };

    /**
     * @brief 类型线索（schema v9）：owner 符号内某变量/形参/返回槽的动态
     *        类型候选集（JSON 数组）。
     */
    export struct TypeHints_
    {
        struct HintId
        {
            using _sqlpp_name_tag = TypeHints_HintId_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::true_type;
        };
        struct OwnerId
        {
            using _sqlpp_name_tag = TypeHints_OwnerId_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct VarName
        {
            using _sqlpp_name_tag = TypeHints_VarName_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::true_type;
        };
        struct FileId
        {
            using _sqlpp_name_tag = TypeHints_FileId_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct Line
        {
            using _sqlpp_name_tag = TypeHints_Line_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct Col
        {
            using _sqlpp_name_tag = TypeHints_Col_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct TypeSet
        {
            using _sqlpp_name_tag = TypeHints_TypeSet_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::true_type;
        };
        struct Source
        {
            using _sqlpp_name_tag = TypeHints_Source_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct Confidence
        {
            using _sqlpp_name_tag = TypeHints_Confidence_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct FallbackReason
        {
            using _sqlpp_name_tag = TypeHints_FallbackReason_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::true_type;
        };
        struct ParamIndex
        {
            using _sqlpp_name_tag = TypeHints_ParamIndex_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::true_type;
        };
        struct RelatedVar
        {
            using _sqlpp_name_tag = TypeHints_RelatedVar_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::true_type;
        };
        struct RelatedKey
        {
            using _sqlpp_name_tag = TypeHints_RelatedKey_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::true_type;
        };
        SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(type_hints, typeHints);
        template <typename T>
        using _table_columns = ::sqlpp::table_columns<
            T,
            HintId,
            OwnerId,
            VarName,
            FileId,
            Line,
            Col,
            TypeSet,
            Source,
            Confidence,
            FallbackReason,
            ParamIndex,
            RelatedVar,
            RelatedKey>;
        using _required_insert_columns = ::sqlpp::detail::type_set<
            ::sqlpp::column_t<::sqlpp::table_t<TypeHints_>, OwnerId>,
            ::sqlpp::column_t<::sqlpp::table_t<TypeHints_>, VarName>,
            ::sqlpp::column_t<::sqlpp::table_t<TypeHints_>, FileId>,
            ::sqlpp::column_t<::sqlpp::table_t<TypeHints_>, Line>,
            ::sqlpp::column_t<::sqlpp::table_t<TypeHints_>, Col>,
            ::sqlpp::column_t<::sqlpp::table_t<TypeHints_>, TypeSet>,
            ::sqlpp::column_t<::sqlpp::table_t<TypeHints_>, Source>,
            ::sqlpp::column_t<::sqlpp::table_t<TypeHints_>, Confidence>>;
    };
    export using TypeHints = ::sqlpp::table_t<TypeHints_>;

    export struct CallSites_SiteId_tag
    {
        SQLPP_NAME_TAG_GUTS(site_id, siteId);
    };
    export struct CallSites_CallerId_tag
    {
        SQLPP_NAME_TAG_GUTS(caller_id, callerId);
    };
    export struct CallSites_CalleeId_tag
    {
        SQLPP_NAME_TAG_GUTS(callee_id, calleeId);
    };
    export struct CallSites_CalleeKey_tag
    {
        SQLPP_NAME_TAG_GUTS(callee_key, calleeKey);
    };
    export struct CallSites_CalleeName_tag
    {
        SQLPP_NAME_TAG_GUTS(callee_name, calleeName);
    };
    export struct CallSites_CalleeNs_tag
    {
        SQLPP_NAME_TAG_GUTS(callee_ns, calleeNs);
    };
    export struct CallSites_CalleeParent_tag
    {
        SQLPP_NAME_TAG_GUTS(callee_parent, calleeParent);
    };
    export struct CallSites_CalleeSig_tag
    {
        SQLPP_NAME_TAG_GUTS(callee_sig, calleeSig);
    };
    export struct CallSites_CalleeConst_tag
    {
        SQLPP_NAME_TAG_GUTS(callee_const, calleeConst);
    };
    export struct CallSites_MethodId_tag
    {
        SQLPP_NAME_TAG_GUTS(method_id, methodId);
    };
    export struct CallSites_MethodKey_tag
    {
        SQLPP_NAME_TAG_GUTS(method_key, methodKey);
    };
    export struct CallSites_IsVirtual_tag
    {
        SQLPP_NAME_TAG_GUTS(is_virtual, isVirtual);
    };
    export struct CallSites_Qualified_tag
    {
        SQLPP_NAME_TAG_GUTS(qualified, qualified);
    };
    export struct CallSites_Indirect_tag
    {
        SQLPP_NAME_TAG_GUTS(indirect, indirect);
    };
    export struct CallSites_ReceiverName_tag
    {
        SQLPP_NAME_TAG_GUTS(receiver_name, receiverName);
    };
    export struct CallSites_ReceiverIsThis_tag
    {
        SQLPP_NAME_TAG_GUTS(receiver_is_this, receiverIsThis);
    };
    export struct CallSites_ReceiverOwnerKey_tag
    {
        SQLPP_NAME_TAG_GUTS(receiver_owner_key, receiverOwnerKey);
    };
    export struct CallSites_StaticTypeId_tag
    {
        SQLPP_NAME_TAG_GUTS(static_type_id, staticTypeId);
    };
    export struct CallSites_FileId_tag
    {
        SQLPP_NAME_TAG_GUTS(file_id, fileId);
    };
    export struct CallSites_Line_tag
    {
        SQLPP_NAME_TAG_GUTS(line, line);
    };
    export struct CallSites_Col_tag
    {
        SQLPP_NAME_TAG_GUTS(col, col);
    };
    export struct CallSites_ArgTypes_tag
    {
        SQLPP_NAME_TAG_GUTS(arg_types, argTypes);
    };
    export struct CallSites_ArgVars_tag
    {
        SQLPP_NAME_TAG_GUTS(arg_vars, argVars);
    };
    export struct CallSites_ArgCallKeys_tag
    {
        SQLPP_NAME_TAG_GUTS(arg_call_keys, argCallKeys);
    };
    export struct CallSites_ReceiverTypes_tag
    {
        SQLPP_NAME_TAG_GUTS(receiver_types, receiverTypes);
    };
    export struct CallSites_ReceiverStatic_tag
    {
        SQLPP_NAME_TAG_GUTS(receiver_static, receiverStatic);
    };
    export struct CallSites_CalleeResolved_tag
    {
        SQLPP_NAME_TAG_GUTS(callee_resolved, calleeResolved);
    };
    export struct CallSites_Resolved_tag
    {
        SQLPP_NAME_TAG_GUTS(resolved, resolved);
    };
    export struct CallSites_Confidence_tag
    {
        SQLPP_NAME_TAG_GUTS(confidence, confidence);
    };
    export struct CallSites_Candidates_tag
    {
        SQLPP_NAME_TAG_GUTS(candidates, candidates);
    };
    export struct CallSites_FallbackReason_tag
    {
        SQLPP_NAME_TAG_GUTS(fallback_reason, fallbackReason);
    };
    export struct CallSites_ArgTargets_tag
    {
        SQLPP_NAME_TAG_GUTS(arg_targets, argTargets);
    };
    export struct CallSites_CalleeUsr_tag
    {
        SQLPP_NAME_TAG_GUTS(callee_usr, calleeUsr);
    };
    export struct CallSites_CalleeExpr_tag
    {
        SQLPP_NAME_TAG_GUTS(callee_expr, calleeExpr);
    };
    export struct CallSites_CalleeParam_tag
    {
        SQLPP_NAME_TAG_GUTS(callee_param, calleeParam);
    };

    /** @brief 调用点（schema v9）：项目内每个可解析调用一行。 */
    export struct CallSites_
    {
        struct SiteId
        {
            using _sqlpp_name_tag = CallSites_SiteId_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::true_type;
        };
        struct CallerId
        {
            using _sqlpp_name_tag = CallSites_CallerId_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct CalleeId
        {
            using _sqlpp_name_tag = CallSites_CalleeId_tag;
            using data_type = std::optional<::sqlpp::integral>;
            using has_default = std::true_type;
        };
        struct CalleeKey
        {
            using _sqlpp_name_tag = CallSites_CalleeKey_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::true_type;
        };
        struct CalleeName
        {
            using _sqlpp_name_tag = CallSites_CalleeName_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::true_type;
        };
        struct CalleeNs
        {
            using _sqlpp_name_tag = CallSites_CalleeNs_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::true_type;
        };
        struct CalleeParent
        {
            using _sqlpp_name_tag = CallSites_CalleeParent_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::true_type;
        };
        struct CalleeSig
        {
            using _sqlpp_name_tag = CallSites_CalleeSig_tag;
            using data_type = std::optional<::sqlpp::integral>;
            using has_default = std::true_type;
        };
        struct CalleeConst
        {
            using _sqlpp_name_tag = CallSites_CalleeConst_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::true_type;
        };
        struct MethodId
        {
            using _sqlpp_name_tag = CallSites_MethodId_tag;
            using data_type = std::optional<::sqlpp::integral>;
            using has_default = std::true_type;
        };
        struct MethodKey
        {
            using _sqlpp_name_tag = CallSites_MethodKey_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::true_type;
        };
        struct IsVirtual
        {
            using _sqlpp_name_tag = CallSites_IsVirtual_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::true_type;
        };
        struct Qualified
        {
            using _sqlpp_name_tag = CallSites_Qualified_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::true_type;
        };
        struct Indirect
        {
            using _sqlpp_name_tag = CallSites_Indirect_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::true_type;
        };
        struct ReceiverName
        {
            using _sqlpp_name_tag = CallSites_ReceiverName_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::true_type;
        };
        struct ReceiverIsThis
        {
            using _sqlpp_name_tag = CallSites_ReceiverIsThis_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::true_type;
        };
        struct ReceiverOwnerKey
        {
            using _sqlpp_name_tag = CallSites_ReceiverOwnerKey_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::true_type;
        };
        struct StaticTypeId
        {
            using _sqlpp_name_tag = CallSites_StaticTypeId_tag;
            using data_type = std::optional<::sqlpp::integral>;
            using has_default = std::true_type;
        };
        struct FileId
        {
            using _sqlpp_name_tag = CallSites_FileId_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct Line
        {
            using _sqlpp_name_tag = CallSites_Line_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct Col
        {
            using _sqlpp_name_tag = CallSites_Col_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct ArgTypes
        {
            using _sqlpp_name_tag = CallSites_ArgTypes_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::true_type;
        };
        struct ArgVars
        {
            using _sqlpp_name_tag = CallSites_ArgVars_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::true_type;
        };
        struct ArgCallKeys
        {
            using _sqlpp_name_tag = CallSites_ArgCallKeys_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::true_type;
        };
        struct ReceiverTypes
        {
            using _sqlpp_name_tag = CallSites_ReceiverTypes_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::true_type;
        };
        struct ReceiverStatic
        {
            using _sqlpp_name_tag = CallSites_ReceiverStatic_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::true_type;
        };
        struct CalleeResolved
        {
            using _sqlpp_name_tag = CallSites_CalleeResolved_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::true_type;
        };
        struct Resolved
        {
            using _sqlpp_name_tag = CallSites_Resolved_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::true_type;
        };
        struct Confidence
        {
            using _sqlpp_name_tag = CallSites_Confidence_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::true_type;
        };
        struct Candidates
        {
            using _sqlpp_name_tag = CallSites_Candidates_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::true_type;
        };
        struct FallbackReason
        {
            using _sqlpp_name_tag = CallSites_FallbackReason_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::true_type;
        };
        struct ArgTargets
        {
            using _sqlpp_name_tag = CallSites_ArgTargets_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::true_type;
        };
        struct CalleeUsr
        {
            using _sqlpp_name_tag = CallSites_CalleeUsr_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::true_type;
        };
        struct CalleeExpr
        {
            using _sqlpp_name_tag = CallSites_CalleeExpr_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::true_type;
        };
        struct CalleeParam
        {
            using _sqlpp_name_tag = CallSites_CalleeParam_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::true_type;
        };
        SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(call_sites, callSites);
        template <typename T>
        using _table_columns = ::sqlpp::table_columns<
            T,
            SiteId,
            CallerId,
            CalleeId,
            CalleeKey,
            CalleeName,
            CalleeNs,
            CalleeParent,
            CalleeSig,
            CalleeConst,
            MethodId,
            MethodKey,
            IsVirtual,
            Qualified,
            Indirect,
            ReceiverName,
            ReceiverIsThis,
            ReceiverOwnerKey,
            StaticTypeId,
            FileId,
            Line,
            Col,
            ArgTypes,
            ArgVars,
            ArgCallKeys,
            ArgTargets,
            ReceiverTypes,
            ReceiverStatic,
            CalleeResolved,
            Resolved,
            Confidence,
            Candidates,
            FallbackReason,
            CalleeUsr,
            CalleeExpr,
            CalleeParam>;
        using _required_insert_columns = ::sqlpp::detail::type_set<
            ::sqlpp::column_t<::sqlpp::table_t<CallSites_>, CallerId>,
            ::sqlpp::column_t<::sqlpp::table_t<CallSites_>, CalleeKey>,
            ::sqlpp::column_t<::sqlpp::table_t<CallSites_>, MethodKey>,
            ::sqlpp::column_t<::sqlpp::table_t<CallSites_>, IsVirtual>,
            ::sqlpp::column_t<::sqlpp::table_t<CallSites_>, ReceiverName>,
            ::sqlpp::column_t<::sqlpp::table_t<CallSites_>, FileId>,
            ::sqlpp::column_t<::sqlpp::table_t<CallSites_>, Line>,
            ::sqlpp::column_t<::sqlpp::table_t<CallSites_>, Col>>;
    };
    export using CallSites = ::sqlpp::table_t<CallSites_>;

} // namespace cg::db::tab
// NOLINTEND(readability-identifier-naming)
