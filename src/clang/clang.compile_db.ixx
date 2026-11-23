

/**
 * @module cg.clang.compile_db
 *
 * @brief 面向 TU 重放的 compile_commands.json 加载：经 cg.util.json
 * 门面解析 JSON、按 MSVC CRT 规则切分 Windows "command" 字符串、产出
 * 逐文件的就绪 argv 向量供 Index::parse 使用。
 *
 * 索引主策略是记录命令行的逐字重放。
 *
 * @internal
 * 刻意不用 C API 的 CXCompilationDatabase——根过滤与条目整形保持在
 * 项目代码中。
 */
export module cg.clang.compile_db;

import std;
import cg.util;
import cg.util.cstd;
import cg.clang.driver;

export import cg.clang.compile_db.model;
export import cg.clang.compile_db.tokens;
export import cg.clang.compile_db.expand;
export import cg.clang.compile_db.bmi;
export import cg.clang.compile_db.load;
