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
 * detail::load_locked() 时由 cg.clang.loader 交来的库句柄按名填充——项目
 * 中已零直接 clang-c 调用。生命周期显式：load_clang_library()（或首个
 * Index 获取的默认链）加载；引擎库按设计常驻到进程退出，Index 析构只给
 * 引擎的引用计数减一；Index 对象存活期间拒绝切换引擎（一个解析会话 =
 * 一个世界观）。引擎交出的字符串、诊断、记号数组、覆写游标数组都由各
 * 门面单元里的拥有型对象持有，交还只出现在它们的析构里。
 */
export module cg.clang.capi;

import std;
import cg.clang.loader;
import cg.util; // 伞形模块；此处用的是 cg.util.guard（崩溃防护）

export import cg.clang.capi.api;
export import cg.clang.capi.model;
export import cg.clang.capi.cursor;
export import cg.clang.capi.type;

export {
    using ::CX_BinaryOperatorKind;
    using ::CXCursor;
    using ::CXCursorKind;
}
