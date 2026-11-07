# C2801 规避接线（机制与验证见 cmake/C2801ApplyPatch.cmake 的头注释）。
#
# MSVC cl 的模块路径在合并 IFC 时错置 <filesystem> 的类内隐藏友元
# _Path_iterator::operator==，发出假 C2801；决定性宿主只有 std
# BMI，所以受认可的施加点就是 std.ixx 编译（cg_std_modules OBJECT
# 库）。本模块在 configure 期跑一次独立补丁器
#（cmake/C2801ApplyPatch.cmake），且仅在补丁经校验时把影子 include
# 目录前插到那一个目标。其余一切（VS 系统文件、ccg 源码、其它
# 每个 TU）零触碰，clang64 路径从不加载本文件。

option(CCG_MsvcC2801Workaround
    "Shadow-patch the toolset <filesystem> header for the std.ixx compile \
(C2801 module-merge workaround; MSVC-only, falls back to the unpatched \
header whenever the patch cannot be produced and verified)" ON)

set(_CCG_C2801_MODULE_DIR "${CMAKE_CURRENT_LIST_DIR}")

# 把影子 include 目录接到 <target> 上（预期：cg_std_modules）。
# 每种结局都打印一条 STATUS/WARNING 行；只有经校验的补丁才会
# 改目标的编译属性。
function(ccg_c2801_wire_std_module_shadow target)
    if(NOT CCG_MsvcC2801Workaround)
        message(STATUS
            "ccg c2801-wa: disabled (CCG_MsvcC2801Workaround=OFF) - std.ixx "
            "compiles against the unpatched toolset header")
        return()
    endif()

    set(_wa_root "${CMAKE_BINARY_DIR}/c2801-wa")
    set(_wa_inc "${_wa_root}/include")
    set(_status_file "${_wa_root}/status.txt")

    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DC2801_SRC=${CCG_MSVC_TOOLSET_ROOT}/include/filesystem"
            "-DC2801_OUT_DIR=${_wa_inc}"
            "-DC2801_WORK_DIR=${_wa_root}"
            "-DC2801_CL=${CMAKE_CXX_COMPILER}"
            -P "${_CCG_C2801_MODULE_DIR}/C2801ApplyPatch.cmake"
        RESULT_VARIABLE _rc)

    # 工具集更新必须重跑本决策，无论上次结局如何：一次回退
    # （如 pattern-absent）也必须察觉后续把 pattern 带回来的
    # 工具集更新。因此把源头文件登记为 configure 依赖，
    # 覆盖每条路径，而不只是补丁已施加时。
    set_property(DIRECTORY APPEND PROPERTY
        CMAKE_CONFIGURE_DEPENDS "${CCG_MSVC_TOOLSET_ROOT}/include/filesystem")

    # 补丁器非零返回码（进程中断、IO 错误……）绝不能继承陈旧的
    # ok 令牌：状态文件只为干净的补丁器退出作证。其余一切落入
    # 下方 script-error 分支，使旧工具集的遗留产物永远不会被接上。
    set(_token "script-error:patcher-rc-${_rc}")
    if(_rc EQUAL 0 AND EXISTS "${_status_file}")
        file(READ "${_status_file}" _raw)
        string(STRIP "${_raw}" _token)
    endif()

    if(_token STREQUAL "patched-ok" OR _token STREQUAL "reused-patched-ok")
        if(NOT EXISTS "${_wa_inc}/filesystem")
            message(WARNING
                "ccg c2801-wa: status '${_token}' but the shadow header is "
                "missing ('${_wa_inc}/filesystem') - not applied. std.ixx "
                "compiles against the unpatched toolset header.")
            return()
        endif()
        target_include_directories(${target} BEFORE PRIVATE "${_wa_inc}")
        message(STATUS
            "ccg c2801-wa: ${_token} - std.ixx compiles against the patched "
            "<filesystem> shadow (${_wa_inc})")
    elseif(_token STREQUAL "fell-back:pattern-absent")
        # 按设计中立：工具集更新后该块消失，正是上游修复到位时的
        # 正常退役路径（回退=直接用未打补丁的头文件编译 std.ixx）。
        message(STATUS
            "ccg c2801-wa: fell-back:pattern-absent - the toolset "
            "<filesystem> no longer carries the C2801 hidden-friend block "
            "(possibly fixed upstream after a toolset update, which is the "
            "normal retirement); std.ixx compiles against the unpatched "
            "header")
    elseif(_token MATCHES "^fell-back:")
        message(WARNING
            "ccg c2801-wa: ${_token} - workaround not applied, std.ixx "
            "compiles against the unpatched toolset header (details above; "
            "logs under ${_wa_root}/logs/). The C2801 failure may resurface.")
    else()
        message(WARNING
            "ccg c2801-wa: unexpected patcher state '${_token}' (rc ${_rc}) "
            "- workaround not applied, std.ixx compiles against the "
            "unpatched toolset header.")
    endif()
endfunction()
