# 分析层（cg.analysis）的模块单元
ccg_add_dir_modules(cg_core "${CMAKE_CURRENT_LIST_DIR}"
    # 动态派发分析（自 schema v8 起）。依赖栅栏：analysis 可以
    # import db/clang；graph/cli 可以 import analysis；反向绝不。
    analysis.model.ixx
    analysis.points_to.ixx
    analysis.collect.ixx
    analysis.snapshot.ixx
    analysis.propagate.ixx
    analysis.dispatch.graph.ixx
    analysis.dispatch.context.ixx
    analysis.dispatch.statics.ixx
    analysis.dispatch.dynamics.ixx
    analysis.dispatch.ixx
    analysis.external.ixx
    analysis.ixx)
