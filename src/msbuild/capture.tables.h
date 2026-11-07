#pragma once
/**
 * @file capture.tables.h
 * @brief capture.db 的 capture 表 sqlpp23 表模型（形状对应 wrap.ixx
 *        bootstrap_capture_db 的建表 DDL，列名逐字一致）。
 *
 * 手写形状，镜像 sqlpp23-ddl2cpp 生成头的形态（尾下划线表结构体、
 * camelCase 的 cpp 访问器；SQL 列名/表名取
 * SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP 的第一个宏实参，保持不变）。
 * 可空列（include_env、message）用 std::optional<...> 数据类型；id 是
 * INTEGER PRIMARY KEY AUTOINCREMENT，置 has_default = true（插入侧永远
 * 不提供该列）。
 *
 * @internal
 * 【必须是经典头，不是风格选择】表结构体参数化 sqlpp23 的表模板，而
 * MSVC 要求表结构体与名字标签宏在每个实例化翻译单元的全局模块片段中
 * 都可见——定义进命名模块再跨模块导入会让 IFC 读取器崩溃（与
 * src/db/db.tables.h 同一探针矩阵结论）。使用本表模型的导出内联函数
 * （capture_db_insert / read_wrap_log 等）会把表模板实例化带进每个
 * 调用方单元，因此**每个 odr-use 这些函数的单元都必须在自己的全局
 * 模块片段 include 本头**（现：wrap.ixx、pipeline.ixx、cli.msbuild
 * .ixx、tests/msbuild.test.ixx；新增调用方时同样必须）。查询构建器
 * 机制经 import sqlpp23.core / sqlpp23.sqlite3 提供，表模型本身保持
 * 经典 include 形态。include 闭包刻意只覆盖表模型所需（与
 * db.tables.h 同法，不触 sqlpp23.h 伞头，原因见彼处注释）。
 */
#include <sqlpp23/core/basic/table.h>
#include <sqlpp23/core/name/create_name_tag.h>

// NOLINTBEGIN(readability-identifier-naming) 手写 sqlpp23 表模型刻意镜像
// sqlpp23-ddl2cpp 生成头形态：尾下划线表结构体、camelCase 的 cpp 访问器、SQL
// 列名/表名取宏第一个实参保持不变（与 db.tables.h 同法）
namespace cg::msbuild::cap
{

    /**
     * @brief capture 表模型集（wrap 写侧插入、pipeline 读侧查询共用）。
     *
     * @internal 手写 sqlpp23 模式（正常应由 sqlpp23-ddl2cpp 生成），形状
     * 遵循 sqlpp23 自家生成头的形态：尾下划线表结构体、camelCase 的 cpp
     * 访问器；SQL 列名/表名取宏第一个实参保持不变。
     */
    struct Capture_
    {
        struct Id
        {
            SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(id, id);
            using data_type = ::sqlpp::integral;
            using has_default = std::true_type;
        };
        struct Token
        {
            SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(token, token);
            using data_type = ::sqlpp::text;
            using has_default = std::false_type;
        };
        struct Timestamp
        {
            SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(timestamp, timestamp);
            using data_type = ::sqlpp::text;
            using has_default = std::false_type;
        };
        struct Kind
        {
            SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(kind, kind);
            using data_type = ::sqlpp::text;
            using has_default = std::false_type;
        };
        struct ArgvJson
        {
            SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(argv_json, argvJson);
            using data_type = ::sqlpp::text;
            using has_default = std::false_type;
        };
        struct Cwd
        {
            SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(cwd, cwd);
            using data_type = ::sqlpp::text;
            using has_default = std::false_type;
        };
        struct IncludeEnv
        {
            SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(include_env, includeEnv);
            using data_type = std::optional<::sqlpp::text>;
            using has_default = std::false_type;
        };
        struct RealCl
        {
            SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(real_cl, realCl);
            using data_type = ::sqlpp::text;
            using has_default = std::false_type;
        };
        struct Message
        {
            SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(message, message);
            using data_type = std::optional<::sqlpp::text>;
            using has_default = std::false_type;
        };
        SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP(capture, capture);
        template <typename T>
        using _table_columns = ::sqlpp::table_columns<
            T,
            Id,
            Token,
            Timestamp,
            Kind,
            ArgvJson,
            Cwd,
            IncludeEnv,
            RealCl,
            Message>;
        using _required_insert_columns = ::sqlpp::detail::type_set<
            ::sqlpp::column_t<::sqlpp::table_t<Capture_>, Token>,
            ::sqlpp::column_t<::sqlpp::table_t<Capture_>, Timestamp>,
            ::sqlpp::column_t<::sqlpp::table_t<Capture_>, Kind>,
            ::sqlpp::column_t<::sqlpp::table_t<Capture_>, ArgvJson>,
            ::sqlpp::column_t<::sqlpp::table_t<Capture_>, Cwd>,
            ::sqlpp::column_t<::sqlpp::table_t<Capture_>, RealCl>>;
    };
    using Capture = ::sqlpp::table_t<Capture_>;

} // namespace cg::msbuild::cap
// NOLINTEND(readability-identifier-naming)
