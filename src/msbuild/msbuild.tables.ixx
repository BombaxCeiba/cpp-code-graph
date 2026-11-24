module;
#include <sqlpp23/core/name/create_name_tag.h>

/**
 * @module cg.msbuild.tables
 *
 * @brief capture.db 的 capture 表 sqlpp23 表模型（命名空间 cg::msbuild::cap），
 *        以命名模块供使用方导入；列的形状对应 cg.msbuild.wrap 里
 *        bootstrap_capture_db 的建表 DDL，列名逐字一致。
 *
 * 手写形状，镜像 sqlpp23-ddl2cpp 生成头的形态（尾下划线表结构体、camelCase 的
 * cpp 访问器；SQL 列名/表名取 SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP 的第一个宏
 * 实参，保持不变）。可空列（include_env、message）用 std::optional<...> 数据类
 * 型；id 是 INTEGER PRIMARY KEY AUTOINCREMENT，置 has_default = true（插入侧永
 * 远不提供该列）。
 *
 * @internal
 * wrap 写侧插入与 pipeline 读侧查询的单元以 `import cg.msbuild.tables;` 取得表
 * 模型；只调用它们导出的内联函数（capture_db_insert、read_wrap_log 等）的单元不
 * 必为本表模型再导入。
 *
 * @internal
 * 为什么全局片段只有名字标签宏头：模块不导出宏，名字标签宏的定义只能在全局模块
 * 片段里文本取得，而 create_name_tag.h 只有宏定义、不带任何标准头。机制由
 * `import sqlpp23.core;` 提供，机制头不进全局片段：进来之后本单元就同时拥有标准
 * 库声明的文本副本与 `import std;` 给出的模块副本，模块接口文件重新带上这批声
 * 明。
 *
 * @internal
 * 为什么名字标签上提而列说明不动：MSVC 14.44 与 14.51 的 IFC 读取器有缺陷，导出
 * 类里嵌套两层的类带有静态数据成员时，导入方读到它就崩（C1001）；列说明里的名字
 * 标签 `_sqlpp_name_tag` 正是这样的第二层嵌套类。因此列说明仍嵌套在表结构体里，
 * 表结构体名、列说明名、成品别名与访问器名全部不变，只把每个列说明的名字标签本
 * 体提到 cg::msbuild::cap 下并导出，命名为"表名_列说明名_tag"（表名即表结构体名
 * 去掉尾下划线），列说明里用 `using _sqlpp_name_tag = ...;` 指回。增删列时，上
 * 提的结构体与列说明里的别名成对增删。表结构体自己的名字标签只嵌套一层，不触发
 * 该缺陷，原样保留。
 *
 * @internal
 * 同一个翻译单元里不能既文本包含表模型定义、又导入本模块：两份同名声明并存，使
 * 用处报歧义。
 */
export module cg.msbuild.tables;

import std;
import sqlpp23.core;

// NOLINTBEGIN(readability-identifier-naming) 手写 sqlpp23 表模型刻意镜像
// sqlpp23-ddl2cpp 生成头形态：尾下划线表结构体、camelCase 的 cpp 访问器、SQL
// 列名/表名取宏第一个实参保持不变（写法说明见 cg.db.tables 入口模块的文件头）
namespace cg::msbuild::cap
{

    export struct Capture_Id_tag
    {
        SQLPP_NAME_TAG_GUTS(id, id);
    };
    export struct Capture_Token_tag
    {
        SQLPP_NAME_TAG_GUTS(token, token);
    };
    export struct Capture_Timestamp_tag
    {
        SQLPP_NAME_TAG_GUTS(timestamp, timestamp);
    };
    export struct Capture_Kind_tag
    {
        SQLPP_NAME_TAG_GUTS(kind, kind);
    };
    export struct Capture_ArgvJson_tag
    {
        SQLPP_NAME_TAG_GUTS(argv_json, argvJson);
    };
    export struct Capture_Cwd_tag
    {
        SQLPP_NAME_TAG_GUTS(cwd, cwd);
    };
    export struct Capture_IncludeEnv_tag
    {
        SQLPP_NAME_TAG_GUTS(include_env, includeEnv);
    };
    export struct Capture_RealCl_tag
    {
        SQLPP_NAME_TAG_GUTS(real_cl, realCl);
    };
    export struct Capture_Message_tag
    {
        SQLPP_NAME_TAG_GUTS(message, message);
    };

    /**
     * @brief capture 表模型集（wrap 写侧插入、pipeline 读侧查询共用）。
     *
     * @internal 手写 sqlpp23 模式（正常应由 sqlpp23-ddl2cpp 生成），形状
     * 遵循 sqlpp23 自家生成头的形态：尾下划线表结构体、camelCase 的 cpp
     * 访问器；SQL 列名/表名取宏第一个实参保持不变。
     */
    export struct Capture_
    {
        struct Id
        {
            using _sqlpp_name_tag = Capture_Id_tag;
            using data_type = ::sqlpp::integral;
            using has_default = std::true_type;
        };
        struct Token
        {
            using _sqlpp_name_tag = Capture_Token_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::false_type;
        };
        struct Timestamp
        {
            using _sqlpp_name_tag = Capture_Timestamp_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::false_type;
        };
        struct Kind
        {
            using _sqlpp_name_tag = Capture_Kind_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::false_type;
        };
        struct ArgvJson
        {
            using _sqlpp_name_tag = Capture_ArgvJson_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::false_type;
        };
        struct Cwd
        {
            using _sqlpp_name_tag = Capture_Cwd_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::false_type;
        };
        struct IncludeEnv
        {
            using _sqlpp_name_tag = Capture_IncludeEnv_tag;
            using data_type = std::optional<::sqlpp::text>;
            using has_default = std::false_type;
        };
        struct RealCl
        {
            using _sqlpp_name_tag = Capture_RealCl_tag;
            using data_type = ::sqlpp::text;
            using has_default = std::false_type;
        };
        struct Message
        {
            using _sqlpp_name_tag = Capture_Message_tag;
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
    export using Capture = ::sqlpp::table_t<Capture_>;

} // namespace cg::msbuild::cap
// NOLINTEND(readability-identifier-naming)
