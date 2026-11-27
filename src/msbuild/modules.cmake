# MSBuild 层（cg.msbuild）的模块单元
ccg_add_dir_modules(cg_core "${CMAKE_CURRENT_LIST_DIR}"
    msbuild.ixx
    translate.ixx
    msbuild.tables.ixx
    wrap.ixx
    pipeline.configs.ixx
    pipeline.registry.ixx
    pipeline.wraplog.ixx
    pipeline.extractargs.ixx
    pipeline.hostfind.ixx
    pipeline.treestate.ixx
    pipeline.outputs.ixx
    pipeline.extract.ixx
    pipeline.ixx
    eval.ixx
    compose.ixx)
