/**
 * @module cg.cli.commands
 *
 * @brief 查询侧命令运行器：解析后的 Invocation、cg.db / cg.graph 与渲染器
 *        之间的胶合层，聚合再导出族内三个子模块。
 *
 * 持有退出码契约（0 成功、1 未找到/歧义/运行时错误、2 用法）
 * 与符号名解析层级（含第 4 层"你是不是想找"建议）。
 *
 * @internal
 * 族内结构：matchers 是底层共享模块，runs 只导入 matchers，entry 只导入
 * runs，任一子模块都不导入 cg.clang。index/update/msbuild/config/hook
 * 运行器住在各自的接口模块（cg.cli.index/update/msbuild/config/hook），
 * 共享脊柱在 cg.cli.runners，二者都由 entry 导入。本单元不持有声明、不
 * 导入任何模块，只做再导出；它不提及表模型，所以没有全局模块片段。
 */
export module cg.cli.commands;

export import cg.cli.commands.matchers;
export import cg.cli.commands.runs;
export import cg.cli.commands.entry;
