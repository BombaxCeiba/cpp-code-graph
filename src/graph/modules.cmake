# 图层（cg.graph）的模块单元
ccg_add_dir_modules(cg_core "${CMAKE_CURRENT_LIST_DIR}"
    graph.headcache.ixx
    graph.waveflush.ixx
    graph.symledger.ixx
    graph.headdomain.ixx
    graph.incrcapture.ixx
    graph.timing.ixx
    graph.build.ixx
    graph.traverse.model.ixx
    graph.traverse.dispatchidx.ixx
    graph.traverse.include.ixx
    graph.traverse.walk.ixx
    graph.traverse.queries.ixx
    graph.traverse.context.ixx
    graph.traverse.changes.ixx
    graph.traverse.ixx
    graph.ixx)
