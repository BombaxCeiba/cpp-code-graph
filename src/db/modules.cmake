# 存储层（cg.db）的模块单元
ccg_add_dir_modules(cg_core "${CMAKE_CURRENT_LIST_DIR}"
    db.tables.core.ixx
    db.tables.graph.ixx
    db.tables.sites.ixx
    db.tables.analysis.ixx
    db.tables.ixx
    db.schema.ixx
    db.store.ixx
    db.query.lookup.ixx
    db.query.batch.ixx
    db.query.members.ixx
    db.query.stats.ixx
    db.query.dumpkeys.ixx
    db.query.ixx
    db.ixx)
