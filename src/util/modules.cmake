# 工具层（cg.util）的模块单元
ccg_add_dir_modules(cg_core "${CMAKE_CURRENT_LIST_DIR}"
    util.ixx
    util.json.ixx
    util.platform.ixx
    util.guard.ixx
    util.crashdump.ixx
    util.cstd.ixx
    util.msvcswitch.ixx
    util.pool.ixx
    util.instrdigest.ixx
    util.treesit.ixx
    util.cshost.ixx
    util.shellscan.ixx
    util.pcre2.ixx
    util.regex.ixx)
