#pragma once
/**
 * @file db.tables.h
 * @brief 图数据库的 sqlpp23 表模型（形状对应 SCHEMA 规格文档 第 2-3 节）。
 *
 * 手写形状，镜像 sqlpp23-ddl2cpp 生成头的形态（尾下划线表结构体、
 * camelCase 的 cpp 访问器；SQL 列名/表名取
 * SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP 的第一个宏实参，保持不变）。
 * 可空列用 std::optional<...> 数据类型；带 DDL 默认值（或 rowid 别名
 * 主键）的列置 has_default = true。
 *
 * @internal
 * 【必须是经典头，不是风格选择】这些结构体参数化 sqlpp23 的表模板，
 * 而 MSVC 14.38-14.51 要求表结构体与名字标签宏在每个实例化翻译单元的
 * 全局模块片段中都可见——把它们定义进一个命名模块、再从兄弟单元导入
 * 会让模块 IFC 读取器崩溃（C1001，reading-common.h；探针矩阵结论：
 * 定义进模块再跨模块导入的两种形态（双模块/本模块）失败，各消费者经
 * 全局模块片段包含经典头的形态可行）。因此每个 cg.db 单元都在自己的全局
 * 模块片段里 include 本头，并使用经典 sqlpp23 头而非
 * `import sqlpp23.core`。
 *
 * include 闭包刻意只覆盖表模型所需：table.h 拉入 table_t/table_columns/
 * column_t/type_set 与数据类型 trait；名字标签宏来自仅含宏的头。刻意
 * 不用 sqlpp23.h 伞头——它会触达携带 <format> 的头（debug_logger/
 * fixed_string/to_sql_string），而经典 <format> 进入全局模块片段会把
 * filesystem ABI 拖进同时 `import std;` 的翻译单元，MSVC 的跨 IFC 合并
 * 因此在 <filesystem> 上命中 C2801（二分系列日志 b7 证实）。查询构建
 * 器机制改由 `import sqlpp23.core;` 提供。
 */
#include <sqlpp23/core/basic/table.h>
#include <sqlpp23/core/name/create_name_tag.h>

namespace cg::db
{

    /**
     * @brief sqlpp23 表模型集。
     *
     * @internal
     * 手写 sqlpp23 模式（正常应由 sqlpp23-ddl2cpp 生成），形状遵循 sqlpp23
     * 自家生成头的形态。标识符刻意镜像生成头形状：尾下划线表结构体、
     * camelCase 的 cpp 访问器；SQL 列名/表名取宏的第一个实参保持不变——
     * 改成项目 snake_case 风格会偏离本节手工镜像的生成形态。
     */
    // NOLINTBEGIN(readability-identifier-naming)
    namespace tab
    {

        struct Meta_
        {
            struct Key
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(key, key);
                using data_type = ::sqlpp::text;
                using has_default = std::false_type;
            };
            struct Value
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(value, value);
                using data_type = ::sqlpp::text;
                using has_default = std::false_type;
            };
            SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(meta, meta);
            template <typename T>
            using _table_columns = ::sqlpp::table_columns<T, Key, Value>;
            using _required_insert_columns = ::sqlpp::detail::type_set<>;
        };
        using Meta = ::sqlpp::table_t<Meta_>;

        struct Str_
        {
            struct Id
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(id, id);
                using data_type = ::sqlpp::integral;
                using has_default = std::true_type;
            };
            struct S
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(s, s);
                using data_type = ::sqlpp::text;
                using has_default = std::false_type;
            };
            SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(str, str);
            template <typename T>
            using _table_columns = ::sqlpp::table_columns<T, Id, S>;
            using _required_insert_columns = ::sqlpp::detail::type_set<
                ::sqlpp::column_t<::sqlpp::table_t<Str_>, S>>;
        };
        using Str = ::sqlpp::table_t<Str_>;

        /**
         * @brief 符号 kind 字典表（种子由 store 侧引导经类型化 insert 写入，
         *        名单唯一来源是 schema 模块的 name_of(SymbolKind)）。
         */
        struct SymbolKinds_
        {
            struct KindId
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(kind_id, kindId);
                using data_type = ::sqlpp::integral;
                using has_default = std::true_type;
            };
            struct Name
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(name, name);
                using data_type = ::sqlpp::text;
                using has_default = std::false_type;
            };
            SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(symbol_kinds, symbolKinds);
            template <typename T>
            using _table_columns = ::sqlpp::table_columns<T, KindId, Name>;
            using _required_insert_columns = ::sqlpp::detail::type_set<
                ::sqlpp::column_t<::sqlpp::table_t<SymbolKinds_>, Name>>;
        };
        using SymbolKinds = ::sqlpp::table_t<SymbolKinds_>;

        /** @brief 边类型字典表（种子同上，名单唯一来源 name_of(EdgeType)）。 */
        struct EdgeTypes_
        {
            struct TypeId
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(type_id, typeId);
                using data_type = ::sqlpp::integral;
                using has_default = std::true_type;
            };
            struct Name
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(name, name);
                using data_type = ::sqlpp::text;
                using has_default = std::false_type;
            };
            SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(edge_types, edgeTypes);
            template <typename T>
            using _table_columns = ::sqlpp::table_columns<T, TypeId, Name>;
            using _required_insert_columns = ::sqlpp::detail::type_set<
                ::sqlpp::column_t<::sqlpp::table_t<EdgeTypes_>, Name>>;
        };
        using EdgeTypes = ::sqlpp::table_t<EdgeTypes_>;

        /**
         * @brief sqlite 目录表，建为只读模型，供打开时的 schema 存在性探测使用
         *        （保持引导期免于裸 SQL）。
         */
        struct SqliteMaster_
        {
            struct Type
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(type, type);
                using data_type = ::sqlpp::text;
                using has_default = std::false_type;
            };
            struct Name
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(name, name);
                using data_type = ::sqlpp::text;
                using has_default = std::false_type;
            };
            SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(sqlite_master, sqliteMaster);
            template <typename T>
            using _table_columns = ::sqlpp::table_columns<T, Type, Name>;
            using _required_insert_columns = ::sqlpp::detail::type_set<>;
        };
        using SqliteMaster = ::sqlpp::table_t<SqliteMaster_>;

        struct Files_
        {
            struct FileId
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(file_id, fileId);
                using data_type = ::sqlpp::integral;
                using has_default = std::true_type;
            };
            struct PathId
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(path_id, pathId);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct ContentHash
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(
                    content_hash,
                    contentHash);
                using data_type = std::optional<::sqlpp::text>;
                using has_default = std::true_type;
            };
            struct ParseStatus
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(
                    parse_status,
                    parseStatus);
                using data_type = ::sqlpp::integral;
                using has_default = std::true_type;
            };
            struct ErrorCount
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(error_count, errorCount);
                using data_type = ::sqlpp::integral;
                using has_default = std::true_type;
            };
            struct LastError
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(last_error, lastError);
                using data_type = std::optional<::sqlpp::text>;
                using has_default = std::true_type;
            };
            struct IndexedAt
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(indexed_at, indexedAt);
                using data_type = std::optional<::sqlpp::text>;
                using has_default = std::true_type;
            };
            SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(files, files);
            template <typename T>
            using _table_columns = ::sqlpp::table_columns<
                T,
                FileId,
                PathId,
                ContentHash,
                ParseStatus,
                ErrorCount,
                LastError,
                IndexedAt>;
            using _required_insert_columns = ::sqlpp::detail::type_set<
                ::sqlpp::column_t<::sqlpp::table_t<Files_>, PathId>>;
        };
        using Files = ::sqlpp::table_t<Files_>;

        struct Symbols_
        {
            struct SymId
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(sym_id, symId);
                using data_type = ::sqlpp::integral;
                using has_default = std::true_type;
            };
            struct UniqueKeyId
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(
                    unique_key_id,
                    uniqueKeyId);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct KindId
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(kind_id, kindId);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct NameId
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(name_id, nameId);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct NsId
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(ns_id, nsId);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct SigHash
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(sig_hash, sigHash);
                using data_type = std::optional<::sqlpp::integral>;
                using has_default = std::true_type;
            };
            struct FileId
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(file_id, fileId);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct Line
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(line, line);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct Col
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(col, col);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct ParentId
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(parent_id, parentId);
                using data_type = std::optional<::sqlpp::integral>;
                using has_default = std::true_type;
            };
            struct Flags
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(flags, flags);
                using data_type = ::sqlpp::integral;
                using has_default = std::true_type;
            };
            struct ParamTypes
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(param_types, paramTypes);
                using data_type = std::optional<::sqlpp::text>;
                using has_default = std::true_type;
            };
            struct TemplateParams
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(
                    template_params,
                    templateParams);
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
        using Symbols = ::sqlpp::table_t<Symbols_>;

        struct Edges_
        {
            struct EdgeId
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(edge_id, edgeId);
                using data_type = ::sqlpp::integral;
                using has_default = std::true_type;
            };
            struct SrcId
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(src_id, srcId);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct DstId
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(dst_id, dstId);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct TypeId
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(type_id, typeId);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct SiteFileId
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(site_file_id, siteFileId);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct SiteLine
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(site_line, siteLine);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct Attrs
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(attrs, attrs);
                using data_type = ::sqlpp::integral;
                using has_default = std::true_type;
            };
            struct Confidence
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(confidence, confidence);
                using data_type = ::sqlpp::floating_point;
                using has_default = std::true_type;
            };
            struct Occurrences
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(occurrences, occurrences);
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
        using Edges = ::sqlpp::table_t<Edges_>;

        struct Includes_
        {
            struct FileId
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(file_id, fileId);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct TargetId
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(target_id, targetId);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct Line
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(line, line);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct Angled
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(angled, angled);
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
        using Includes = ::sqlpp::table_t<Includes_>;

        struct Imports_
        {
            struct FileId
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(file_id, fileId);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct ModuleId
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(module_id, moduleId);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct Line
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(line, line);
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
        using Imports = ::sqlpp::table_t<Imports_>;

        struct Fingerprints_
        {
            struct FileId
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(file_id, fileId);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct MtimeNs
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(mtime_ns, mtimeNs);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct Size
            {
                /**
                 * @internal cpp 访问器名刻意避开 size：sqlpp23
                 * 的参数列表暴露一个 静态成员
                 * size，同名访问器会在每个绑定点都把它遮蔽得很难看。
                 */
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(size, sizeBytes);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct CmdDigest
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(cmd_digest, cmdDigest);
                using data_type = ::sqlpp::text;
                using has_default = std::true_type;
            };
            SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(fingerprints, fingerprints);
            template <typename T>
            using _table_columns =
                ::sqlpp::table_columns<T, FileId, MtimeNs, Size, CmdDigest>;
            using _required_insert_columns = ::sqlpp::detail::type_set<
                ::sqlpp::column_t<::sqlpp::table_t<Fingerprints_>, FileId>,
                ::sqlpp::column_t<::sqlpp::table_t<Fingerprints_>, MtimeNs>,
                ::sqlpp::column_t<::sqlpp::table_t<Fingerprints_>, Size>>;
        };
        using Fingerprints = ::sqlpp::table_t<Fingerprints_>;

        /**
         * @brief 失败条目（schema v7，SCHEMA 规格文档 第 7 节）：每个
         *        (file_id, cmd_digest) 一行，记录一次解析失败的世界状态。
         *        失败跳过的判据全部来自本表；无行 = 该条目从未失败。
         */
        struct Failures_
        {
            struct FileId
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(file_id, fileId);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct CmdDigest
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(cmd_digest, cmdDigest);
                using data_type = ::sqlpp::text;
                using has_default = std::false_type;
            };
            struct ContentHash
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(
                    content_hash,
                    contentHash);
                using data_type = ::sqlpp::text;
                using has_default = std::true_type;
            };
            struct MtimeNs
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(mtime_ns, mtimeNs);
                using data_type = ::sqlpp::integral;
                using has_default = std::true_type;
            };
            struct Size
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(size, sizeBytes);
                using data_type = ::sqlpp::integral;
                using has_default = std::true_type;
            };
            struct EngineToken
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(
                    engine_token,
                    engineToken);
                using data_type = ::sqlpp::text;
                using has_default = std::true_type;
            };
            struct FeedDigest
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(feed_digest, feedDigest);
                using data_type = ::sqlpp::text;
                using has_default = std::true_type;
            };
            struct RootsDigest
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(
                    roots_digest,
                    rootsDigest);
                using data_type = ::sqlpp::text;
                using has_default = std::true_type;
            };
            struct FailureClass
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(
                    failure_class,
                    failureClass);
                using data_type = ::sqlpp::text;
                using has_default = std::true_type;
            };
            struct FailureText
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(
                    failure_text,
                    failureText);
                using data_type = ::sqlpp::text;
                using has_default = std::true_type;
            };
            struct DepsJson
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(deps_json, depsJson);
                using data_type = ::sqlpp::text;
                using has_default = std::true_type;
            };
            struct Attempts
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(attempts, attempts);
                using data_type = ::sqlpp::integral;
                using has_default = std::true_type;
            };
            struct FirstFailedAt
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(
                    first_failed_at,
                    firstFailedAt);
                using data_type = ::sqlpp::text;
                using has_default = std::true_type;
            };
            struct LastFailedAt
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(
                    last_failed_at,
                    lastFailedAt);
                using data_type = ::sqlpp::text;
                using has_default = std::true_type;
            };
            struct RetryAfter
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(retry_after, retryAfter);
                using data_type = ::sqlpp::integral;
                using has_default = std::true_type;
            };
            SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(failures, failures);
            template <typename T>
            using _table_columns = ::sqlpp::table_columns<
                T,
                FileId,
                CmdDigest,
                ContentHash,
                MtimeNs,
                Size,
                EngineToken,
                FeedDigest,
                RootsDigest,
                FailureClass,
                FailureText,
                DepsJson,
                Attempts,
                FirstFailedAt,
                LastFailedAt,
                RetryAfter>;
            using _required_insert_columns = ::sqlpp::detail::type_set<
                ::sqlpp::column_t<::sqlpp::table_t<Failures_>, FileId>,
                ::sqlpp::column_t<::sqlpp::table_t<Failures_>, CmdDigest>,
                ::sqlpp::column_t<::sqlpp::table_t<Failures_>, ContentHash>,
                ::sqlpp::column_t<::sqlpp::table_t<Failures_>, MtimeNs>,
                ::sqlpp::column_t<::sqlpp::table_t<Failures_>, Size>,
                ::sqlpp::column_t<::sqlpp::table_t<Failures_>, EngineToken>,
                ::sqlpp::column_t<::sqlpp::table_t<Failures_>, FeedDigest>,
                ::sqlpp::column_t<::sqlpp::table_t<Failures_>, RootsDigest>,
                ::sqlpp::column_t<::sqlpp::table_t<Failures_>, FailureClass>,
                ::sqlpp::column_t<::sqlpp::table_t<Failures_>, FailureText>,
                ::sqlpp::column_t<::sqlpp::table_t<Failures_>, DepsJson>,
                ::sqlpp::column_t<::sqlpp::table_t<Failures_>, Attempts>,
                ::sqlpp::column_t<::sqlpp::table_t<Failures_>, FirstFailedAt>,
                ::sqlpp::column_t<::sqlpp::table_t<Failures_>, LastFailedAt>,
                ::sqlpp::column_t<::sqlpp::table_t<Failures_>, RetryAfter>>;
        };
        using Failures = ::sqlpp::table_t<Failures_>;

        /**
         * @brief 实时变更队列（schema v6，多进程实时协调协议）：每条规范化路径一行；
         *        TEXT 主键对并发的重复入队去重。
         */
        struct PendingUpdates_
        {
            struct Path
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(path, path);
                using data_type = ::sqlpp::text;
                using has_default = std::false_type;
            };
            SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(
                pending_updates,
                pendingUpdates);
            template <typename T>
            using _table_columns = ::sqlpp::table_columns<T, Path>;
            using _required_insert_columns = ::sqlpp::detail::type_set<
                ::sqlpp::column_t<::sqlpp::table_t<PendingUpdates_>, Path>>;
        };
        using PendingUpdates = ::sqlpp::table_t<PendingUpdates_>;

        /**
         * @brief 实时声明（claim）单例（schema v6）：恰好一行（id = 1）记录当前
         *        持有者及其租约到期的 unix 纪元毫秒时刻。
         *
         * @internal 过期租约可被下一个更新者窃取（崩溃自愈——无锁文件、无 PID
         * 猜测）。
         */
        struct UpdateClaim_
        {
            struct Id
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(id, id);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct Holder
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(holder, holder);
                using data_type = ::sqlpp::text;
                using has_default = std::false_type;
            };
            struct LeaseUntil
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(lease_until, leaseUntil);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(update_claim, updateClaim);
            template <typename T>
            using _table_columns =
                ::sqlpp::table_columns<T, Id, Holder, LeaseUntil>;
            using _required_insert_columns = ::sqlpp::detail::type_set<
                ::sqlpp::column_t<::sqlpp::table_t<UpdateClaim_>, Id>,
                ::sqlpp::column_t<::sqlpp::table_t<UpdateClaim_>, Holder>,
                ::sqlpp::column_t<::sqlpp::table_t<UpdateClaim_>, LeaseUntil>>;
        };
        using UpdateClaim = ::sqlpp::table_t<UpdateClaim_>;

        /**
         * @brief 类型线索（schema v9）：owner 符号内某变量/形参/返回槽的动态
         *        类型候选集（JSON 数组）。
         */
        struct TypeHints_
        {
            struct HintId
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(hint_id, hintId);
                using data_type = ::sqlpp::integral;
                using has_default = std::true_type;
            };
            struct OwnerId
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(owner_id, ownerId);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct VarName
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(var_name, varName);
                using data_type = ::sqlpp::text;
                using has_default = std::true_type;
            };
            struct FileId
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(file_id, fileId);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct Line
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(line, line);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct Col
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(col, col);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct TypeSet
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(type_set, typeSet);
                using data_type = ::sqlpp::text;
                using has_default = std::true_type;
            };
            struct Source
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(source, source);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct Confidence
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(confidence, confidence);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct FallbackReason
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(
                    fallback_reason,
                    fallbackReason);
                using data_type = ::sqlpp::text;
                using has_default = std::true_type;
            };
            struct ParamIndex
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(param_index, paramIndex);
                using data_type = ::sqlpp::integral;
                using has_default = std::true_type;
            };
            struct RelatedVar
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(related_var, relatedVar);
                using data_type = ::sqlpp::text;
                using has_default = std::true_type;
            };
            struct RelatedKey
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(related_key, relatedKey);
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
        using TypeHints = ::sqlpp::table_t<TypeHints_>;

        /** @brief 调用点（schema v9）：项目内每个可解析调用一行。 */
        struct CallSites_
        {
            struct SiteId
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(site_id, siteId);
                using data_type = ::sqlpp::integral;
                using has_default = std::true_type;
            };
            struct CallerId
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(caller_id, callerId);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct CalleeId
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(callee_id, calleeId);
                using data_type = std::optional<::sqlpp::integral>;
                using has_default = std::true_type;
            };
            struct CalleeKey
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(callee_key, calleeKey);
                using data_type = ::sqlpp::text;
                using has_default = std::true_type;
            };
            struct CalleeName
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(callee_name, calleeName);
                using data_type = ::sqlpp::text;
                using has_default = std::true_type;
            };
            struct CalleeNs
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(callee_ns, calleeNs);
                using data_type = ::sqlpp::text;
                using has_default = std::true_type;
            };
            struct CalleeParent
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(
                    callee_parent,
                    calleeParent);
                using data_type = ::sqlpp::text;
                using has_default = std::true_type;
            };
            struct CalleeSig
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(callee_sig, calleeSig);
                using data_type = std::optional<::sqlpp::integral>;
                using has_default = std::true_type;
            };
            struct CalleeConst
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(
                    callee_const,
                    calleeConst);
                using data_type = ::sqlpp::integral;
                using has_default = std::true_type;
            };
            struct MethodId
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(method_id, methodId);
                using data_type = std::optional<::sqlpp::integral>;
                using has_default = std::true_type;
            };
            struct MethodKey
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(method_key, methodKey);
                using data_type = ::sqlpp::text;
                using has_default = std::true_type;
            };
            struct IsVirtual
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(is_virtual, isVirtual);
                using data_type = ::sqlpp::integral;
                using has_default = std::true_type;
            };
            struct Qualified
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(qualified, qualified);
                using data_type = ::sqlpp::integral;
                using has_default = std::true_type;
            };
            struct Indirect
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(indirect, indirect);
                using data_type = ::sqlpp::integral;
                using has_default = std::true_type;
            };
            struct ReceiverName
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(
                    receiver_name,
                    receiverName);
                using data_type = ::sqlpp::text;
                using has_default = std::true_type;
            };
            struct ReceiverIsThis
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(
                    receiver_is_this,
                    receiverIsThis);
                using data_type = ::sqlpp::integral;
                using has_default = std::true_type;
            };
            struct ReceiverOwnerKey
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(
                    receiver_owner_key,
                    receiverOwnerKey);
                using data_type = ::sqlpp::text;
                using has_default = std::true_type;
            };
            struct StaticTypeId
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(
                    static_type_id,
                    staticTypeId);
                using data_type = std::optional<::sqlpp::integral>;
                using has_default = std::true_type;
            };
            struct FileId
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(file_id, fileId);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct Line
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(line, line);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct Col
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(col, col);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct ArgTypes
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(arg_types, argTypes);
                using data_type = ::sqlpp::text;
                using has_default = std::true_type;
            };
            struct ArgVars
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(arg_vars, argVars);
                using data_type = ::sqlpp::text;
                using has_default = std::true_type;
            };
            struct ArgCallKeys
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(
                    arg_call_keys,
                    argCallKeys);
                using data_type = ::sqlpp::text;
                using has_default = std::true_type;
            };
            struct ReceiverTypes
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(
                    receiver_types,
                    receiverTypes);
                using data_type = ::sqlpp::text;
                using has_default = std::true_type;
            };
            struct CalleeResolved
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(
                    callee_resolved,
                    calleeResolved);
                using data_type = ::sqlpp::integral;
                using has_default = std::true_type;
            };
            struct Resolved
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(resolved, resolved);
                using data_type = ::sqlpp::integral;
                using has_default = std::true_type;
            };
            struct Confidence
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(confidence, confidence);
                using data_type = ::sqlpp::integral;
                using has_default = std::true_type;
            };
            struct Candidates
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(candidates, candidates);
                using data_type = ::sqlpp::text;
                using has_default = std::true_type;
            };
            struct FallbackReason
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(
                    fallback_reason,
                    fallbackReason);
                using data_type = ::sqlpp::text;
                using has_default = std::true_type;
            };
            struct ArgTargets
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(arg_targets, argTargets);
                using data_type = ::sqlpp::text;
                using has_default = std::true_type;
            };
            struct CalleeUsr
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(callee_usr, calleeUsr);
                using data_type = ::sqlpp::text;
                using has_default = std::true_type;
            };
            struct CalleeExpr
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(callee_expr, calleeExpr);
                using data_type = ::sqlpp::text;
                using has_default = std::true_type;
            };
            struct CalleeParam
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(
                    callee_param,
                    calleeParam);
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
        using CallSites = ::sqlpp::table_t<CallSites_>;

        /**
         * @brief 传播槽终态持久化（schema v12）：每个 (owner_sym,
         * var_name) 传播槽一行，记录最近一次提交段的固定点输出。增量批装载它做
         *        warm start，非脏闭包槽直接沿用、闭包槽从初始值重算。
         *
         * @internal 除任务书点名的 type_set/truncated/ver 外，还持久化
         * confidence/source/reason（写回列的 byte 级等价需要槽级终态置信度，
         * 它们无法从 type_set 复原）与 alias_src/ret_src（上次运行解析出的
         * 函数内别名/返回值传播源槽键——删除/漂移检测的旧边见证）。
         * owner_sym 存符号 unique_key 文本而非 id：符号行被 purge 重建后 id
         * 漂移，键文本才是稳定身份。
         */
        struct AnalysisSlots_
        {
            struct OwnerSym
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(owner_sym, ownerSym);
                using data_type = ::sqlpp::text;
                using has_default = std::false_type;
            };
            struct VarName
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(var_name, varName);
                using data_type = ::sqlpp::text;
                using has_default = std::false_type;
            };
            struct TypeSet
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(type_set, typeSet);
                using data_type = ::sqlpp::text;
                using has_default = std::true_type;
            };
            struct Confidence
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(confidence, confidence);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct Source
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(source, source);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct Reason
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(reason, reason);
                using data_type = ::sqlpp::text;
                using has_default = std::true_type;
            };
            struct Truncated
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(truncated, truncated);
                using data_type = ::sqlpp::integral;
                using has_default = std::true_type;
            };
            struct Ver
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(ver, ver);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct AliasSrc
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(alias_src, aliasSrc);
                using data_type = ::sqlpp::text;
                using has_default = std::true_type;
            };
            struct RetSrc
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(ret_src, retSrc);
                using data_type = ::sqlpp::text;
                using has_default = std::true_type;
            };
            SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(
                analysis_slots,
                analysisSlots);
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
        using AnalysisSlots = ::sqlpp::table_t<AnalysisSlots_>;

        /** @brief passthrough_wrappers 表列（schema v13，槽位基础设施）。 */
        struct PassthroughWrappers_
        {
            struct WrapperKey
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(wrapper_key, wrapperKey);
                using data_type = ::sqlpp::text;
                using has_default = std::false_type;
            };
            struct GetterKey
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(getter_key, getterKey);
                using data_type = ::sqlpp::text;
                using has_default = std::false_type;
            };
            struct GetterName
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(getter_name, getterName);
                using data_type = ::sqlpp::text;
                using has_default = std::false_type;
            };
            struct FileId
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(file_id, fileId);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct Line
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(line, line);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct ArgIndex
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(arg_index, argIndex);
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
                ::sqlpp::column_t<
                    ::sqlpp::table_t<PassthroughWrappers_>,
                    WrapperKey>,
                ::sqlpp::
                    column_t<::sqlpp::table_t<PassthroughWrappers_>, GetterKey>,
                ::sqlpp::column_t<
                    ::sqlpp::table_t<PassthroughWrappers_>,
                    GetterName>,
                ::sqlpp::
                    column_t<::sqlpp::table_t<PassthroughWrappers_>, FileId>,
                ::sqlpp::column_t<::sqlpp::table_t<PassthroughWrappers_>, Line>,
                ::sqlpp::
                    column_t<::sqlpp::table_t<PassthroughWrappers_>, ArgIndex>>;
        };
        using PassthroughWrappers = ::sqlpp::table_t<PassthroughWrappers_>;

        /** @brief external_param_args 表列（schema v15，实例桥传入侧）。 */
        struct ExternalParamArgs_
        {
            struct HostUsr
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(host_usr, hostUsr);
                using data_type = ::sqlpp::text;
                using has_default = std::false_type;
            };
            struct ParamIndex
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(param_index, paramIndex);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct CallerKey
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(caller_key, callerKey);
                using data_type = ::sqlpp::text;
                using has_default = std::false_type;
            };
            struct TargetKey
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(target_key, targetKey);
                using data_type = ::sqlpp::text;
                using has_default = std::false_type;
            };
            struct FileId
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(file_id, fileId);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct Line
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(line, line);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct Col
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(col, col);
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
                ::sqlpp::
                    column_t<::sqlpp::table_t<ExternalParamArgs_>, HostUsr>,
                ::sqlpp::
                    column_t<::sqlpp::table_t<ExternalParamArgs_>, ParamIndex>,
                ::sqlpp::
                    column_t<::sqlpp::table_t<ExternalParamArgs_>, CallerKey>,
                ::sqlpp::
                    column_t<::sqlpp::table_t<ExternalParamArgs_>, TargetKey>,
                ::sqlpp::column_t<::sqlpp::table_t<ExternalParamArgs_>, FileId>,
                ::sqlpp::column_t<::sqlpp::table_t<ExternalParamArgs_>, Line>,
                ::sqlpp::column_t<::sqlpp::table_t<ExternalParamArgs_>, Col>>;
        };
        using ExternalParamArgs = ::sqlpp::table_t<ExternalParamArgs_>;

        /** @brief external_invokes 表列（schema v15，实例桥体内调用侧）。 */
        struct ExternalInvokes_
        {
            struct HostUsr
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(host_usr, hostUsr);
                using data_type = ::sqlpp::text;
                using has_default = std::false_type;
            };
            struct ParamIndex
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(param_index, paramIndex);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct FileId
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(file_id, fileId);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct Line
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(line, line);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            struct Col
            {
                SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(col, col);
                using data_type = ::sqlpp::integral;
                using has_default = std::false_type;
            };
            SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(
                external_invokes,
                externalInvokes);
            template <typename T>
            using _table_columns = ::sqlpp::
                table_columns<T, HostUsr, ParamIndex, FileId, Line, Col>;
            using _required_insert_columns = ::sqlpp::detail::type_set<
                ::sqlpp::column_t<::sqlpp::table_t<ExternalInvokes_>, HostUsr>,
                ::sqlpp::
                    column_t<::sqlpp::table_t<ExternalInvokes_>, ParamIndex>,
                ::sqlpp::column_t<::sqlpp::table_t<ExternalInvokes_>, FileId>,
                ::sqlpp::column_t<::sqlpp::table_t<ExternalInvokes_>, Line>,
                ::sqlpp::column_t<::sqlpp::table_t<ExternalInvokes_>, Col>>;
        };
        using ExternalInvokes = ::sqlpp::table_t<ExternalInvokes_>;

        /** @brief 聚合结果列的别名标签组（SELECT expr AS `<tag>`）。 */
        SQLPP_CREATE_NAME_TAG(cnt);
        SQLPP_CREATE_NAME_TAG(occ_sum);
        SQLPP_CREATE_NAME_TAG(site_min);
        SQLPP_CREATE_NAME_TAG(max_conf);
        SQLPP_CREATE_NAME_TAG(max_attrs);

        /**
         * @brief 轻量投影的别名标签组：str 表在一批查询里连接多次
         *        （名字/唯一键/路径），表与结果列都要显式别名以免列名相撞。
         */
        SQLPP_CREATE_NAME_TAG(brief_str_name);
        SQLPP_CREATE_NAME_TAG(brief_str_key);
        SQLPP_CREATE_NAME_TAG(brief_str_path);
        SQLPP_CREATE_NAME_TAG(brief_name);
        SQLPP_CREATE_NAME_TAG(brief_key);
        SQLPP_CREATE_NAME_TAG(brief_path);

    } // namespace tab
    // NOLINTEND(readability-identifier-naming)
} // namespace cg::db
