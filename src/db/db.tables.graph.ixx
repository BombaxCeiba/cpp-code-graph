module;
#include <sqlpp23/core/name/create_name_tag.h>

/**
 * @module cg.db.tables.graph
 *
 * @brief 图数据库表模型四组当中的图数据组（命名空间 cg::db::tab），经入口模块
 *        cg.db.tables 再导出：符号（Symbols）、边（Edges）、包含（Includes）与
 *        导入（Imports）四张表。
 *
 * 表模型的写法、名字标签为什么上提、全局模块片段为什么只有名字标签宏头，见入口
 * 模块 cg.db.tables 的说明。
 */
export module cg.db.tables.graph;

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

    export struct Symbols_SymId_tag
    {
        SQLPP_NAME_TAG_GUTS(sym_id, symId);
    };
    export struct Symbols_UniqueKeyId_tag
    {
        SQLPP_NAME_TAG_GUTS(unique_key_id, uniqueKeyId);
    };
    export struct Symbols_KindId_tag
    {
        SQLPP_NAME_TAG_GUTS(kind_id, kindId);
    };
    export struct Symbols_NameId_tag
    {
        SQLPP_NAME_TAG_GUTS(name_id, nameId);
    };
    export struct Symbols_NsId_tag
    {
        SQLPP_NAME_TAG_GUTS(ns_id, nsId);
    };
    export struct Symbols_SigHash_tag
    {
        SQLPP_NAME_TAG_GUTS(sig_hash, sigHash);
    };
    export struct Symbols_FileId_tag
    {
        SQLPP_NAME_TAG_GUTS(file_id, fileId);
    };
    export struct Symbols_Line_tag
    {
        SQLPP_NAME_TAG_GUTS(line, line);
    };
    export struct Symbols_Col_tag
    {
        SQLPP_NAME_TAG_GUTS(col, col);
    };
    export struct Symbols_ParentId_tag
    {
        SQLPP_NAME_TAG_GUTS(parent_id, parentId);
    };
    export struct Symbols_Flags_tag
    {
        SQLPP_NAME_TAG_GUTS(flags, flags);
    };
    export struct Symbols_ParamTypes_tag
    {
        SQLPP_NAME_TAG_GUTS(param_types, paramTypes);
    };
    export struct Symbols_TemplateParams_tag
    {
        SQLPP_NAME_TAG_GUTS(template_params, templateParams);
    };

    export struct Symbols_
    {
        struct SymId
        {
            using _sqlpp_name_tag = Symbols_SymId_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::true_type;
        };
        struct UniqueKeyId
        {
            using _sqlpp_name_tag = Symbols_UniqueKeyId_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct KindId
        {
            using _sqlpp_name_tag = Symbols_KindId_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct NameId
        {
            using _sqlpp_name_tag = Symbols_NameId_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct NsId
        {
            using _sqlpp_name_tag = Symbols_NsId_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct SigHash
        {
            using _sqlpp_name_tag = Symbols_SigHash_tag;
            using data_type = std::optional<::sqlpp::integral>;
            using has_default = std::true_type;
        };
        struct FileId
        {
            using _sqlpp_name_tag = Symbols_FileId_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct Line
        {
            using _sqlpp_name_tag = Symbols_Line_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct Col
        {
            using _sqlpp_name_tag = Symbols_Col_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct ParentId
        {
            using _sqlpp_name_tag = Symbols_ParentId_tag;
            using data_type = std::optional<::sqlpp::integral>;
            using has_default = std::true_type;
        };
        struct Flags
        {
            using _sqlpp_name_tag = Symbols_Flags_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::true_type;
        };
        struct ParamTypes
        {
            using _sqlpp_name_tag = Symbols_ParamTypes_tag;
            using data_type = std::optional<::sqlpp::text>;
            using has_default = std::true_type;
        };
        struct TemplateParams
        {
            using _sqlpp_name_tag = Symbols_TemplateParams_tag;
            using data_type = std::optional<::sqlpp::text>;
            using has_default = std::true_type;
        };
        SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(symbols, symbols);
        template <typename T>
        using _table_columns = ::sqlpp::table_columns<
            T,
            SymId,
            UniqueKeyId,
            KindId,
            NameId,
            NsId,
            SigHash,
            FileId,
            Line,
            Col,
            ParentId,
            Flags,
            ParamTypes,
            TemplateParams>;
        using _required_insert_columns = ::sqlpp::detail::type_set<
            ::sqlpp::column_t<::sqlpp::table_t<Symbols_>, UniqueKeyId>,
            ::sqlpp::column_t<::sqlpp::table_t<Symbols_>, KindId>,
            ::sqlpp::column_t<::sqlpp::table_t<Symbols_>, NameId>,
            ::sqlpp::column_t<::sqlpp::table_t<Symbols_>, NsId>,
            ::sqlpp::column_t<::sqlpp::table_t<Symbols_>, FileId>,
            ::sqlpp::column_t<::sqlpp::table_t<Symbols_>, Line>,
            ::sqlpp::column_t<::sqlpp::table_t<Symbols_>, Col>>;
    };
    export using Symbols = ::sqlpp::table_t<Symbols_>;

    export struct Edges_EdgeId_tag
    {
        SQLPP_NAME_TAG_GUTS(edge_id, edgeId);
    };
    export struct Edges_SrcId_tag
    {
        SQLPP_NAME_TAG_GUTS(src_id, srcId);
    };
    export struct Edges_DstId_tag
    {
        SQLPP_NAME_TAG_GUTS(dst_id, dstId);
    };
    export struct Edges_TypeId_tag
    {
        SQLPP_NAME_TAG_GUTS(type_id, typeId);
    };
    export struct Edges_SiteFileId_tag
    {
        SQLPP_NAME_TAG_GUTS(site_file_id, siteFileId);
    };
    export struct Edges_SiteLine_tag
    {
        SQLPP_NAME_TAG_GUTS(site_line, siteLine);
    };
    export struct Edges_Attrs_tag
    {
        SQLPP_NAME_TAG_GUTS(attrs, attrs);
    };
    export struct Edges_Confidence_tag
    {
        SQLPP_NAME_TAG_GUTS(confidence, confidence);
    };
    export struct Edges_Occurrences_tag
    {
        SQLPP_NAME_TAG_GUTS(occurrences, occurrences);
    };

    export struct Edges_
    {
        struct EdgeId
        {
            using _sqlpp_name_tag = Edges_EdgeId_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::true_type;
        };
        struct SrcId
        {
            using _sqlpp_name_tag = Edges_SrcId_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct DstId
        {
            using _sqlpp_name_tag = Edges_DstId_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct TypeId
        {
            using _sqlpp_name_tag = Edges_TypeId_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct SiteFileId
        {
            using _sqlpp_name_tag = Edges_SiteFileId_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct SiteLine
        {
            using _sqlpp_name_tag = Edges_SiteLine_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct Attrs
        {
            using _sqlpp_name_tag = Edges_Attrs_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::true_type;
        };
        struct Confidence
        {
            using _sqlpp_name_tag = Edges_Confidence_tag;
            using data_type = ::sqlpp::floating_point;
            using has_default = std::true_type;
        };
        struct Occurrences
        {
            using _sqlpp_name_tag = Edges_Occurrences_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::true_type;
        };
        SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(edges, edges);
        template <typename T>
        using _table_columns = ::sqlpp::table_columns<
            T,
            EdgeId,
            SrcId,
            DstId,
            TypeId,
            SiteFileId,
            SiteLine,
            Attrs,
            Confidence,
            Occurrences>;
        using _required_insert_columns = ::sqlpp::detail::type_set<
            ::sqlpp::column_t<::sqlpp::table_t<Edges_>, SrcId>,
            ::sqlpp::column_t<::sqlpp::table_t<Edges_>, DstId>,
            ::sqlpp::column_t<::sqlpp::table_t<Edges_>, TypeId>,
            ::sqlpp::column_t<::sqlpp::table_t<Edges_>, SiteFileId>,
            ::sqlpp::column_t<::sqlpp::table_t<Edges_>, SiteLine>>;
    };
    export using Edges = ::sqlpp::table_t<Edges_>;

    export struct Includes_FileId_tag
    {
        SQLPP_NAME_TAG_GUTS(file_id, fileId);
    };
    export struct Includes_TargetId_tag
    {
        SQLPP_NAME_TAG_GUTS(target_id, targetId);
    };
    export struct Includes_Line_tag
    {
        SQLPP_NAME_TAG_GUTS(line, line);
    };
    export struct Includes_Angled_tag
    {
        SQLPP_NAME_TAG_GUTS(angled, angled);
    };

    export struct Includes_
    {
        struct FileId
        {
            using _sqlpp_name_tag = Includes_FileId_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct TargetId
        {
            using _sqlpp_name_tag = Includes_TargetId_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct Line
        {
            using _sqlpp_name_tag = Includes_Line_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct Angled
        {
            using _sqlpp_name_tag = Includes_Angled_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(includes, includes);
        template <typename T>
        using _table_columns =
            ::sqlpp::table_columns<T, FileId, TargetId, Line, Angled>;
        using _required_insert_columns = ::sqlpp::detail::type_set<
            ::sqlpp::column_t<::sqlpp::table_t<Includes_>, FileId>,
            ::sqlpp::column_t<::sqlpp::table_t<Includes_>, TargetId>,
            ::sqlpp::column_t<::sqlpp::table_t<Includes_>, Line>,
            ::sqlpp::column_t<::sqlpp::table_t<Includes_>, Angled>>;
    };
    export using Includes = ::sqlpp::table_t<Includes_>;

    export struct Imports_FileId_tag
    {
        SQLPP_NAME_TAG_GUTS(file_id, fileId);
    };
    export struct Imports_ModuleId_tag
    {
        SQLPP_NAME_TAG_GUTS(module_id, moduleId);
    };
    export struct Imports_Line_tag
    {
        SQLPP_NAME_TAG_GUTS(line, line);
    };

    export struct Imports_
    {
        struct FileId
        {
            using _sqlpp_name_tag = Imports_FileId_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct ModuleId
        {
            using _sqlpp_name_tag = Imports_ModuleId_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct Line
        {
            using _sqlpp_name_tag = Imports_Line_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(imports, imports);
        template <typename T>
        using _table_columns =
            ::sqlpp::table_columns<T, FileId, ModuleId, Line>;
        using _required_insert_columns = ::sqlpp::detail::type_set<
            ::sqlpp::column_t<::sqlpp::table_t<Imports_>, FileId>,
            ::sqlpp::column_t<::sqlpp::table_t<Imports_>, ModuleId>,
            ::sqlpp::column_t<::sqlpp::table_t<Imports_>, Line>>;
    };
    export using Imports = ::sqlpp::table_t<Imports_>;

} // namespace cg::db::tab
// NOLINTEND(readability-identifier-naming)
