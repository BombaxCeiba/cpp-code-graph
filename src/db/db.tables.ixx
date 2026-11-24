/**
 * @module cg.db.tables
 *
 * @brief 图数据库 sqlpp23 表模型的入口模块（命名空间 cg::db::tab）：自身不声明
 *        任何内容，只再导出四个子模块。用到表模型名字的单元
 *        `import cg.db.tables;` 即得全部表与别名标签；表模型不经总接口模块
 *        cg.db 再导出。
 *
 * 四个子模块各管一组互不引用的表：
 *
 * - cg.db.tables.core：库自身的元数据与字符串表、kind 与边类型两张字典表、
 *   sqlite 目录表的只读模型、文件登记与指纹、失败条目、实时变更协调，以及聚合与
 *   连接用的全部别名标签；
 * - cg.db.tables.graph：符号、边、包含、导入四张表；
 * - cg.db.tables.sites：类型线索与调用点两张表；
 * - cg.db.tables.analysis：传播槽、透传包装、实例桥传入侧、实例桥体内调用侧、符
 *   号贡献登记五张表。
 *
 * 表模型的形状手写而成，镜像 sqlpp23-ddl2cpp 生成头的形态（尾下划线表结构体、
 * camelCase 的 cpp 访问器；SQL 列名/表名取
 * SQLPP_CREATE_NAME_TAG_FOR_SQL_AND_CPP 的第一个宏实参，保持不变）。可空列用
 * std::optional<...> 数据类型；带 DDL 默认值（或 rowid 别名主键）的列置
 * has_default = true。除 SqliteMaster 是 SQLite 系统目录表的只读模型外，表与列
 * 的形状对应 cg.db.schema 里的建表 DDL。
 *
 * @internal
 * 为什么做成模块：做成经典头的话，每个用到它的单元都要在自己的全局模块片段里文
 * 本包含它，表模型所依赖的 sqlpp23 机制头连带的标准库声明随之进入几十个单元的全
 * 局模块片段，这些单元的模块接口文件因此一直带着这批声明。做成模块则每张表的定
 * 义只在它的子模块单元里有一份，使用方导入即得。
 *
 * @internal
 * 为什么名字标签上提而列说明不动：MSVC 14.44 与 14.51 的 IFC 读取器有缺陷，导出
 * 类里嵌套两层的类带有静态数据成员时，导入方读到它就崩（C1001）。列说明嵌套在表
 * 结构体里，列说明的名字标签 `_sqlpp_name_tag` 正是这样的第二层嵌套类，带
 * require_quotes 与 name 两个静态数据成员，所以列说明不能原样导出。处理办法：列
 * 说明仍嵌套在表结构体里，表结构体名、列说明名、成品别名与访问器名全部不变；只
 * 把每个列说明的名字标签本体提到 cg::db::tab 下并导出，命名为
 * `表名_列说明名_tag`（表名即表结构体名去掉尾下划线），列说明里用
 * `using _sqlpp_name_tag = ...;` 指回。增删列时，上提的结构体与列说明里的别名成
 * 对增删。表结构体自己的名字标签只嵌套一层，不触发该缺陷，原样保留。
 *
 * @internal
 * 为什么名字标签宏头必须在全局片段：模块不导出宏，名字标签宏的定义只能在全局模
 * 块片段里文本取得，所以四个子模块的全局模块片段都只有 create_name_tag.h，它只
 * 有宏定义、不带任何标准头。表模板、列模板、type_set 与数据类型 trait 等机制由
 * `import sqlpp23.core;` 提供。机制头不进全局片段：进来之后该子模块单元就同时拥
 * 有标准库声明的文本副本与 `import std;` 给出的模块副本，模块接口文件重新带上这
 * 批声明。
 *
 * @internal
 * 同一个翻译单元里不能既文本包含表模型定义、又导入表模型模块：两份同名声明并
 * 存，使用处报歧义。
 */
export module cg.db.tables;

export import cg.db.tables.core;
export import cg.db.tables.graph;
export import cg.db.tables.sites;
export import cg.db.tables.analysis;
