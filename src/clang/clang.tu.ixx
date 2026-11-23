

/**
 * @module cg.clang.tu
 *
 * @brief 翻译单元级 API：单次调用解析、返回 RAII 句柄加严重度
 * >=error 的诊断，以及一切走查器都需要的主文件游标过滤器（包含头
 * 内的游标位置绝不能成为项目符号——探针验证过的过滤模式）。
 */
export module cg.clang.tu;

import std;
import cg.util;
import cg.util.cstd;
import cg.clang.capi;
import cg.clang.driver;
import cg.clang.compile_db;

export import cg.clang.tu.lex;
export import cg.clang.tu.imports;
export import cg.clang.tu.argv;
export import cg.clang.tu.scrub;
export import cg.clang.tu.parse;
