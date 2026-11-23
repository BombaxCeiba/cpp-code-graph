

/**
 * @module cg.clang.pch
 *
 * @brief 构建系统记录的 PCH 的“影子 PCH”规划：发现 producer/consumer、
 * 复刻 producer argv（同源驱动器 + DetailedPreprocessingRecord）构建
 * 影子缓存、按闭包哈希失效、把消费者改写为消费影子。
 *
 * @internal
 * 只有构建系统自己已经在用 PCH 的工程才进入本模块；无 PCH 标记的
 * 条目逐字节原样返回（零变化门禁）。生产命令只认构建系统记录：
 * CMake/GNU 的 `-x c++-header` + `-emit-pch`/`.pch` 条目，或 MSBuild
 * sidecar `pch.json` 的 `/Yc` 调用。没有记录就文本回退——绝不拿消费者
 * 伞头合成 recipe。
 */
export module cg.clang.pch;

import std;
import cg.util;
import cg.util.cstd;
import cg.clang.capi;
import cg.clang.driver;
import cg.clang.compile_db;
import cg.clang.bmi;

export import cg.clang.pch.model;
export import cg.clang.pch.paths;
export import cg.clang.pch.sidecar;
export import cg.clang.pch.incdirs;
export import cg.clang.pch.depfile;
export import cg.clang.pch.inject;
export import cg.clang.pch.manifest;
export import cg.clang.pch.plan;
