module;
#include <sqlpp23/core/name/create_name_tag.h>

/**
 * @module cg.db.tables.core
 *
 * @brief 图数据库表模型四组当中的基础组（命名空间 cg::db::tab），经入口模块
 *        cg.db.tables 再导出：库自身的元数据与字符串表（Meta、Str）、kind 与边
 *        类型两张字典表（SymbolKinds、EdgeTypes）、sqlite 目录表的只读模型
 *        （SqliteMaster）、文件登记与指纹（Files、Fingerprints）、失败条目
 *        （Failures）、实时变更协调（PendingUpdates、UpdateClaim），以及聚合与
 *        连接用的全部别名标签（tab::cnt 等）。
 *
 * @internal
 * 别名标签（tab::cnt 等）来自宏 SQLPP_CREATE_NAME_TAG：宏本身不能导出，这里写开
 * 它的外层，结构体与变量两条声明各自导出，结构体内部仍由
 * SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP 生成。
 *
 * 表模型的写法、名字标签为什么上提、全局模块片段为什么只有名字标签宏头，见入口
 * 模块 cg.db.tables 的说明。
 */
export module cg.db.tables.core;

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

    export struct Meta_Key_tag
    {
        SQLPP_NAME_TAG_GUTS(key, key);
    };
    export struct Meta_Value_tag
    {
        SQLPP_NAME_TAG_GUTS(value, value);
    };

    export struct Meta_
    {
        struct Key
        {
            using _sqlpp_name_tag = Meta_Key_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::false_type;
        };
        struct Value
        {
            using _sqlpp_name_tag = Meta_Value_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::false_type;
        };
        SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(meta, meta);
        template <typename T>
        using _table_columns = ::sqlpp::table_columns<T, Key, Value>;
        using _required_insert_columns = ::sqlpp::detail::type_set<>;
    };
    export using Meta = ::sqlpp::table_t<Meta_>;

    export struct Str_Id_tag
    {
        SQLPP_NAME_TAG_GUTS(id, id);
    };
    export struct Str_S_tag
    {
        SQLPP_NAME_TAG_GUTS(s, s);
    };

    export struct Str_
    {
        struct Id
        {
            using _sqlpp_name_tag = Str_Id_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::true_type;
        };
        struct S
        {
            using _sqlpp_name_tag = Str_S_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::false_type;
        };
        SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(str, str);
        template <typename T>
        using _table_columns = ::sqlpp::table_columns<T, Id, S>;
        using _required_insert_columns = ::sqlpp::detail::type_set<
            ::sqlpp::column_t<::sqlpp::table_t<Str_>, S>>;
    };
    export using Str = ::sqlpp::table_t<Str_>;

    export struct SymbolKinds_KindId_tag
    {
        SQLPP_NAME_TAG_GUTS(kind_id, kindId);
    };
    export struct SymbolKinds_Name_tag
    {
        SQLPP_NAME_TAG_GUTS(name, name);
    };

    /**
     * @brief 符号 kind 字典表（种子由 store 侧引导经类型化 insert 写入，
     *        名单唯一来源是 schema 模块的 name_of(SymbolKind)）。
     */
    export struct SymbolKinds_
    {
        struct KindId
        {
            using _sqlpp_name_tag = SymbolKinds_KindId_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::true_type;
        };
        struct Name
        {
            using _sqlpp_name_tag = SymbolKinds_Name_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::false_type;
        };
        SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(symbol_kinds, symbolKinds);
        template <typename T>
        using _table_columns = ::sqlpp::table_columns<T, KindId, Name>;
        using _required_insert_columns = ::sqlpp::detail::type_set<
            ::sqlpp::column_t<::sqlpp::table_t<SymbolKinds_>, Name>>;
    };
    export using SymbolKinds = ::sqlpp::table_t<SymbolKinds_>;

    export struct EdgeTypes_TypeId_tag
    {
        SQLPP_NAME_TAG_GUTS(type_id, typeId);
    };
    export struct EdgeTypes_Name_tag
    {
        SQLPP_NAME_TAG_GUTS(name, name);
    };

    /** @brief 边类型字典表（种子同上，名单唯一来源 name_of(EdgeType)）。 */
    export struct EdgeTypes_
    {
        struct TypeId
        {
            using _sqlpp_name_tag = EdgeTypes_TypeId_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::true_type;
        };
        struct Name
        {
            using _sqlpp_name_tag = EdgeTypes_Name_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::false_type;
        };
        SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(edge_types, edgeTypes);
        template <typename T>
        using _table_columns = ::sqlpp::table_columns<T, TypeId, Name>;
        using _required_insert_columns = ::sqlpp::detail::type_set<
            ::sqlpp::column_t<::sqlpp::table_t<EdgeTypes_>, Name>>;
    };
    export using EdgeTypes = ::sqlpp::table_t<EdgeTypes_>;

    export struct SqliteMaster_Type_tag
    {
        SQLPP_NAME_TAG_GUTS(type, type);
    };
    export struct SqliteMaster_Name_tag
    {
        SQLPP_NAME_TAG_GUTS(name, name);
    };

    /**
     * @brief sqlite 目录表，建为只读模型，供打开时的 schema 存在性探测使用
     *        （保持引导期免于裸 SQL）。
     */
    export struct SqliteMaster_
    {
        struct Type
        {
            using _sqlpp_name_tag = SqliteMaster_Type_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::false_type;
        };
        struct Name
        {
            using _sqlpp_name_tag = SqliteMaster_Name_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::false_type;
        };
        SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(sqlite_master, sqliteMaster);
        template <typename T>
        using _table_columns = ::sqlpp::table_columns<T, Type, Name>;
        using _required_insert_columns = ::sqlpp::detail::type_set<>;
    };
    export using SqliteMaster = ::sqlpp::table_t<SqliteMaster_>;

    export struct Files_FileId_tag
    {
        SQLPP_NAME_TAG_GUTS(file_id, fileId);
    };
    export struct Files_PathId_tag
    {
        SQLPP_NAME_TAG_GUTS(path_id, pathId);
    };
    export struct Files_ContentHash_tag
    {
        SQLPP_NAME_TAG_GUTS(content_hash, contentHash);
    };
    export struct Files_ParseStatus_tag
    {
        SQLPP_NAME_TAG_GUTS(parse_status, parseStatus);
    };
    export struct Files_ErrorCount_tag
    {
        SQLPP_NAME_TAG_GUTS(error_count, errorCount);
    };
    export struct Files_LastError_tag
    {
        SQLPP_NAME_TAG_GUTS(last_error, lastError);
    };
    export struct Files_IndexedAt_tag
    {
        SQLPP_NAME_TAG_GUTS(indexed_at, indexedAt);
    };

    export struct Files_
    {
        struct FileId
        {
            using _sqlpp_name_tag = Files_FileId_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::true_type;
        };
        struct PathId
        {
            using _sqlpp_name_tag = Files_PathId_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct ContentHash
        {
            using _sqlpp_name_tag = Files_ContentHash_tag;
            using data_type = std::optional<::sqlpp::text>;
            using has_default = std::true_type;
        };
        struct ParseStatus
        {
            using _sqlpp_name_tag = Files_ParseStatus_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::true_type;
        };
        struct ErrorCount
        {
            using _sqlpp_name_tag = Files_ErrorCount_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::true_type;
        };
        struct LastError
        {
            using _sqlpp_name_tag = Files_LastError_tag;
            using data_type = std::optional<::sqlpp::text>;
            using has_default = std::true_type;
        };
        struct IndexedAt
        {
            using _sqlpp_name_tag = Files_IndexedAt_tag;
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
    export using Files = ::sqlpp::table_t<Files_>;

    export struct Fingerprints_FileId_tag
    {
        SQLPP_NAME_TAG_GUTS(file_id, fileId);
    };
    export struct Fingerprints_MtimeNs_tag
    {
        SQLPP_NAME_TAG_GUTS(mtime_ns, mtimeNs);
    };
    /**
     * @internal cpp 访问器名刻意避开 size：sqlpp23 的参数列表暴露一个静态
     * 成员 size，同名访问器会在每个绑定点都把它遮蔽得很难看。
     */
    export struct Fingerprints_Size_tag
    {
        SQLPP_NAME_TAG_GUTS(size, sizeBytes);
    };
    export struct Fingerprints_CmdDigest_tag
    {
        SQLPP_NAME_TAG_GUTS(cmd_digest, cmdDigest);
    };

    export struct Fingerprints_
    {
        struct FileId
        {
            using _sqlpp_name_tag = Fingerprints_FileId_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct MtimeNs
        {
            using _sqlpp_name_tag = Fingerprints_MtimeNs_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct Size
        {
            using _sqlpp_name_tag = Fingerprints_Size_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct CmdDigest
        {
            using _sqlpp_name_tag = Fingerprints_CmdDigest_tag;
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
    export using Fingerprints = ::sqlpp::table_t<Fingerprints_>;

    export struct Failures_FileId_tag
    {
        SQLPP_NAME_TAG_GUTS(file_id, fileId);
    };
    export struct Failures_CmdDigest_tag
    {
        SQLPP_NAME_TAG_GUTS(cmd_digest, cmdDigest);
    };
    export struct Failures_ContentHash_tag
    {
        SQLPP_NAME_TAG_GUTS(content_hash, contentHash);
    };
    export struct Failures_MtimeNs_tag
    {
        SQLPP_NAME_TAG_GUTS(mtime_ns, mtimeNs);
    };
    export struct Failures_Size_tag
    {
        SQLPP_NAME_TAG_GUTS(size, sizeBytes);
    };
    export struct Failures_EngineToken_tag
    {
        SQLPP_NAME_TAG_GUTS(engine_token, engineToken);
    };
    export struct Failures_FeedDigest_tag
    {
        SQLPP_NAME_TAG_GUTS(feed_digest, feedDigest);
    };
    export struct Failures_RootsDigest_tag
    {
        SQLPP_NAME_TAG_GUTS(roots_digest, rootsDigest);
    };
    export struct Failures_FailureClass_tag
    {
        SQLPP_NAME_TAG_GUTS(failure_class, failureClass);
    };
    export struct Failures_FailureText_tag
    {
        SQLPP_NAME_TAG_GUTS(failure_text, failureText);
    };
    export struct Failures_DepsJson_tag
    {
        SQLPP_NAME_TAG_GUTS(deps_json, depsJson);
    };
    export struct Failures_Attempts_tag
    {
        SQLPP_NAME_TAG_GUTS(attempts, attempts);
    };
    export struct Failures_FirstFailedAt_tag
    {
        SQLPP_NAME_TAG_GUTS(first_failed_at, firstFailedAt);
    };
    export struct Failures_LastFailedAt_tag
    {
        SQLPP_NAME_TAG_GUTS(last_failed_at, lastFailedAt);
    };
    export struct Failures_RetryAfter_tag
    {
        SQLPP_NAME_TAG_GUTS(retry_after, retryAfter);
    };

    /**
     * @brief 失败条目（schema v7，建表语句在 cg.db.schema 里）：每个
     *        (file_id, cmd_digest) 一行，记录一次解析失败的世界状态。
     *        失败跳过的判据全部来自本表；无行 = 该条目从未失败。
     */
    export struct Failures_
    {
        struct FileId
        {
            using _sqlpp_name_tag = Failures_FileId_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct CmdDigest
        {
            using _sqlpp_name_tag = Failures_CmdDigest_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::false_type;
        };
        struct ContentHash
        {
            using _sqlpp_name_tag = Failures_ContentHash_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::true_type;
        };
        struct MtimeNs
        {
            using _sqlpp_name_tag = Failures_MtimeNs_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::true_type;
        };
        struct Size
        {
            using _sqlpp_name_tag = Failures_Size_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::true_type;
        };
        struct EngineToken
        {
            using _sqlpp_name_tag = Failures_EngineToken_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::true_type;
        };
        struct FeedDigest
        {
            using _sqlpp_name_tag = Failures_FeedDigest_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::true_type;
        };
        struct RootsDigest
        {
            using _sqlpp_name_tag = Failures_RootsDigest_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::true_type;
        };
        struct FailureClass
        {
            using _sqlpp_name_tag = Failures_FailureClass_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::true_type;
        };
        struct FailureText
        {
            using _sqlpp_name_tag = Failures_FailureText_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::true_type;
        };
        struct DepsJson
        {
            using _sqlpp_name_tag = Failures_DepsJson_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::true_type;
        };
        struct Attempts
        {
            using _sqlpp_name_tag = Failures_Attempts_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::true_type;
        };
        struct FirstFailedAt
        {
            using _sqlpp_name_tag = Failures_FirstFailedAt_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::true_type;
        };
        struct LastFailedAt
        {
            using _sqlpp_name_tag = Failures_LastFailedAt_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::true_type;
        };
        struct RetryAfter
        {
            using _sqlpp_name_tag = Failures_RetryAfter_tag;
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
    export using Failures = ::sqlpp::table_t<Failures_>;

    export struct PendingUpdates_Path_tag
    {
        SQLPP_NAME_TAG_GUTS(path, path);
    };

    /**
     * @brief 实时变更队列（schema
     * v6，多进程实时协调协议）：每条规范化路径一行； TEXT
     * 主键对并发的重复入队去重。
     */
    export struct PendingUpdates_
    {
        struct Path
        {
            using _sqlpp_name_tag = PendingUpdates_Path_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::false_type;
        };
        SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(pending_updates, pendingUpdates);
        template <typename T>
        using _table_columns = ::sqlpp::table_columns<T, Path>;
        using _required_insert_columns = ::sqlpp::detail::type_set<
            ::sqlpp::column_t<::sqlpp::table_t<PendingUpdates_>, Path>>;
    };
    export using PendingUpdates = ::sqlpp::table_t<PendingUpdates_>;

    export struct UpdateClaim_Id_tag
    {
        SQLPP_NAME_TAG_GUTS(id, id);
    };
    export struct UpdateClaim_Holder_tag
    {
        SQLPP_NAME_TAG_GUTS(holder, holder);
    };
    export struct UpdateClaim_LeaseUntil_tag
    {
        SQLPP_NAME_TAG_GUTS(lease_until, leaseUntil);
    };

    /**
     * @brief 实时声明（claim）单例（schema v6）：恰好一行（id = 1）记录当前
     *        持有者及其租约到期的 unix 纪元毫秒时刻。
     *
     * @internal 过期租约可被下一个更新者窃取（崩溃自愈——无锁文件、无 PID
     * 猜测）。
     */
    export struct UpdateClaim_
    {
        struct Id
        {
            using _sqlpp_name_tag = UpdateClaim_Id_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::false_type;
        };
        struct Holder
        {
            using _sqlpp_name_tag = UpdateClaim_Holder_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::false_type;
        };
        struct LeaseUntil
        {
            using _sqlpp_name_tag = UpdateClaim_LeaseUntil_tag;
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
    export using UpdateClaim = ::sqlpp::table_t<UpdateClaim_>;

    /** @brief 聚合结果列的别名标签组（SELECT expr AS `<tag>`）。 */
    export struct cnt_t
    {
        SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(cnt, cnt);
    };
    export inline constexpr auto cnt = cnt_t{};
    export struct occ_sum_t
    {
        SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(occ_sum, occ_sum);
    };
    export inline constexpr auto occ_sum = occ_sum_t{};
    export struct site_min_t
    {
        SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(site_min, site_min);
    };
    export inline constexpr auto site_min = site_min_t{};
    export struct max_conf_t
    {
        SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(max_conf, max_conf);
    };
    export inline constexpr auto max_conf = max_conf_t{};
    export struct max_attrs_t
    {
        SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(max_attrs, max_attrs);
    };
    export inline constexpr auto max_attrs = max_attrs_t{};

    /**
     * @brief 轻量投影的别名标签组：str 表在一批查询里连接多次
     *        （名字/唯一键/路径），表与结果列都要显式别名以免列名相撞。
     */
    export struct brief_str_name_t
    {
        SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(brief_str_name, brief_str_name);
    };
    export inline constexpr auto brief_str_name = brief_str_name_t{};
    export struct brief_str_key_t
    {
        SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(brief_str_key, brief_str_key);
    };
    export inline constexpr auto brief_str_key = brief_str_key_t{};
    export struct brief_str_path_t
    {
        SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(brief_str_path, brief_str_path);
    };
    export inline constexpr auto brief_str_path = brief_str_path_t{};
    export struct brief_name_t
    {
        SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(brief_name, brief_name);
    };
    export inline constexpr auto brief_name = brief_name_t{};
    export struct brief_key_t
    {
        SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(brief_key, brief_key);
    };
    export inline constexpr auto brief_key = brief_key_t{};
    export struct brief_path_t
    {
        SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(brief_path, brief_path);
    };
    export inline constexpr auto brief_path = brief_path_t{};

} // namespace cg::db::tab
// NOLINTEND(readability-identifier-naming)
