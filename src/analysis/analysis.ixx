/**
 * @module cg.analysis
 *
 * @brief 动态派发分析的伞形模块：model/collect/points_to/propagate/
 *        dispatch/external 的重导出。
 *
 * @internal 依赖围栏：analysis → db/clang；graph/cli →
 * analysis。extract/db/clang 绝不导入 analysis。未来全程序指针/别名
 * 分析只在 src/analysis 内演进。
 */
export module cg.analysis;

export import cg.analysis.model;
export import cg.analysis.collect;
export import cg.analysis.points_to;
export import cg.analysis.snapshot;
export import cg.analysis.propagate;
export import cg.analysis.dispatch;
export import cg.analysis.external;
