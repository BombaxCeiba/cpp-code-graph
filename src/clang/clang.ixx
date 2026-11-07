/**
 * @module cg.clang
 *
 * @brief libclang 包装层的主接口：伞形重导出本层全部子模块。
 *
 * 各子模块分工——cg.clang.loader（跨平台运行时加载 LLVM 引擎库与引擎
 * PATH 解析链）、cg.clang.capi（clang-c 符号分发表、RAII 句柄、游标
 * 门面、诊断——clang-c 头文件的唯一持有者）、cg.clang.driver（运行时
 * clang 驱动器发现与讯问：工具链 include 路径的事实来源）、cg.clang.tu
 * （单次调用解析 + 主文件过滤）、cg.clang.compile_db（compile_commands.json
 * 加载、按文件查找与兜底 argv）、cg.clang.bmi（按目标供应 std 模块源 +
 * 引擎锁定的 BMI 自建缓存）、cg.clang.pch（构建系统记录的 PCH 的
 * 影子缓存规划与消费者注入）。
 *
 * @internal
 * 表面刻意保持精小：cg.extract 恰好建立在这些名字之上，除 CXCursor/
 * CXCursorKind 外不接触任何裸 CX 类型。历史上的模块分区已在拆分为
 * 点分命名模块的重构中解散，本伞形模块只做重导出。
 */
export module cg.clang;

export import cg.clang.loader;
export import cg.clang.capi;
export import cg.clang.driver;
export import cg.clang.tu;
export import cg.clang.compile_db;
export import cg.clang.bmi;
export import cg.clang.pch;
