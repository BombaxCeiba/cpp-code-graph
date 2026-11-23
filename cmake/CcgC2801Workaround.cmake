# C2801 规避接线（机制与验证见 cmake/C2801ApplyPatch.cmake 的头注释）。
#
# MSVC cl 的模块路径在合并 IFC 时错置 <filesystem> 的类内隐藏友元
# _Path_iterator::operator==，发出假 C2801；决定性宿主只有 std
# BMI，所以受认可的施加点就是 std.ixx 编译。std.ixx 的生产者按
# CMake 版本分两条路径（cmake/CcgUtils.cmake 的版本分支）：
#   - CMake >= 4.5：import std 非实验化，CMake 给每个有模块扫描
#     源的目标无条件挂 @cmake_cxx_std 合成目标（ApplyCXXStdTarget
#     不再读 CXX_MODULE_STD/CMAKE_EXPERIMENTAL_CXX_IMPORT_STD，
#     项目侧无任何开关可拒绝；项目若再自建 std.ixx 供给即双供应
#     Disagreement 硬错误）。本模块生成一份 conforming 格式的
#     影子 modules.json（CMAKE_CXX_STDLIB_MODULES_JSON 覆盖点），
#     经 local-arguments.include-directories 把影子 include 目录
#     注入合成目标里 std.ixx/std.compat.ixx 的编译命令（4.5 实测
#     该通道生效；vendor.compile-options 通道不生效，synth 命令的
#     运行库旗标由 CMake 内部拼装、项目侧不可及）。
#   - CMake < 4.5：CMake 实验性合成库丢 STL charconv 内联静态量
#     （LNK2001 探针），项目自建 cg_std_modules OBJECT 库为
#     std 生产者，影子 include 直接挂到那一个目标上。
# 其余一切（VS 系统文件、ccg 源码、其它每个 TU）零触碰，clang64
# 路径从不加载本文件。

option(CCG_MsvcC2801Workaround
    "Shadow-patch the toolset <filesystem> header for the std.ixx compile \
(C2801 module-merge workaround; MSVC-only, falls back to the unpatched \
header whenever the patch cannot be produced and verified)" ON)

set(_CCG_C2801_MODULE_DIR "${CMAKE_CURRENT_LIST_DIR}")

# 跑一次独立补丁器并把结局规约成状态令牌。补丁产物与状态文件
# 固定落在 <build>/c2801-wa 下；调用方经 <out_var> 拿令牌，经
# CCG_C2801_WA_ROOT / CCG_C2801_WA_INC 拿路径。
function(_ccg_c2801_run_patcher out_var)
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
    # script-error 分支，使旧工具集的遗留产物永远不会被接上。
    set(_token "script-error:patcher-rc-${_rc}")
    if(_rc EQUAL 0 AND EXISTS "${_status_file}")
        file(READ "${_status_file}" _raw)
        string(STRIP "${_raw}" _token)
    endif()

    set(CCG_C2801_WA_ROOT "${_wa_root}" PARENT_SCOPE)
    set(CCG_C2801_WA_INC "${_wa_inc}" PARENT_SCOPE)
    set(${out_var} "${_token}" PARENT_SCOPE)
endfunction()

# 两条接线路径共用的判定半边：守卫检查、补丁器调用、失败结局的
# 分类打印与影子头存在性校验。只在状态令牌为经校验的补丁且影子头
# 就位时才把令牌经 <out_token> 传出；失败路径不设置它（未定义即
# 假）。补丁器以 PARENT_SCOPE 写出的 CCG_C2801_WA_ROOT /
# CCG_C2801_WA_INC 只到本函数一层，必须在这里再向上一层传出，
# 否则接线动作拿不到补丁产物路径。<std_desc> 是消息里对 std
# 生产者的称谓：合成目标路径传 "synth std.ixx"，OBJECT 库路径传
# "std.ixx"。
function(_ccg_c2801_wire_common out_token std_desc)
    if(NOT CCG_MsvcC2801Workaround)
        message(STATUS
            "ccg c2801-wa: disabled (CCG_MsvcC2801Workaround=OFF) - ${std_desc} "
            "compiles against the unpatched toolset header")
        return()
    endif()

    _ccg_c2801_run_patcher(_token)

    if(NOT (_token STREQUAL "patched-ok" OR _token STREQUAL "reused-patched-ok"))
        if(_token STREQUAL "fell-back:pattern-absent")
            # 按设计中立：工具集更新后该块消失，正是上游修复到位时的
            # 正常退役路径（回退=直接用未打补丁的头文件编译）。
            message(STATUS
                "ccg c2801-wa: fell-back:pattern-absent - the toolset "
                "<filesystem> no longer carries the C2801 hidden-friend "
                "block (possibly fixed upstream after a toolset update, "
                "which is the normal retirement); ${std_desc} compiles "
                "against the unpatched header")
        elseif(_token MATCHES "^fell-back:")
            message(WARNING
                "ccg c2801-wa: ${_token} - workaround not applied, "
                "${std_desc} compiles against the unpatched toolset header "
                "(details above; logs under ${CCG_C2801_WA_ROOT}/logs/). "
                "The C2801 failure may resurface.")
        else()
            message(WARNING
                "ccg c2801-wa: unexpected patcher state '${_token}' "
                "- workaround not applied, ${std_desc} compiles against "
                "the unpatched toolset header (details above; logs under "
                "${CCG_C2801_WA_ROOT}/logs/). The C2801 failure may "
                "resurface.")
        endif()
        return()
    endif()
    if(NOT EXISTS "${CCG_C2801_WA_INC}/filesystem")
        message(WARNING
            "ccg c2801-wa: status '${_token}' but the shadow header is "
            "missing ('${CCG_C2801_WA_INC}/filesystem') - not applied; "
            "${std_desc} compiles against the unpatched toolset header.")
        return()
    endif()

    set(CCG_C2801_WA_ROOT "${CCG_C2801_WA_ROOT}" PARENT_SCOPE)
    set(CCG_C2801_WA_INC "${CCG_C2801_WA_INC}" PARENT_SCOPE)
    set(${out_token} "${_token}" PARENT_SCOPE)
endfunction()

# CMake >= 4.5 路径：影子补丁经 conforming modules.json 进入
# @cmake_cxx_std 合成目标的 std.ixx/std.compat.ixx 编译。必须在
# 顶层目录作用域调用（set 的 CMAKE_CXX_STDLIB_MODULES_JSON 要被
# 生成期的合成目标读取）。守卫与全部失败结局的处理在
# _ccg_c2801_wire_common；这里只做施加动作与成功播报。
function(ccg_c2801_wire_synth_std)
    _ccg_c2801_wire_common(_token "synth std.ixx")
    if(NOT _token)
        return()
    endif()

    # JSON 字符串里反斜杠必须转义，统一转成正斜杠路径（cl 与
    # CMake 的路径规约化都接受）。
    string(REPLACE "\\" "/" _wa_inc_fwd "${CCG_C2801_WA_INC}")
    string(REPLACE "\\" "/" _std_ixx_fwd
        "${CCG_MSVC_TOOLSET_ROOT}/modules/std.ixx")
    string(REPLACE "\\" "/" _compat_ixx_fwd
        "${CCG_MSVC_TOOLSET_ROOT}/modules/std.compat.ixx")
    set(_manifest "${CCG_C2801_WA_ROOT}/msvcstl-shadow.modules.json")
    file(WRITE "${_manifest}" "{
  \"version\": 1,
  \"revision\": 1,
  \"modules\": [
    {
      \"logical-name\": \"std\",
      \"source-path\": \"${_std_ixx_fwd}\",
      \"is-std-library\": true,
      \"local-arguments\": { \"include-directories\": [\"${_wa_inc_fwd}\"] }
    },
    {
      \"logical-name\": \"std.compat\",
      \"source-path\": \"${_compat_ixx_fwd}\",
      \"is-std-library\": true,
      \"local-arguments\": { \"include-directories\": [\"${_wa_inc_fwd}\"] }
    }
  ]
}
")
    set(CMAKE_CXX_STDLIB_MODULES_JSON "${_manifest}" PARENT_SCOPE)
    message(STATUS
        "ccg c2801-wa: ${_token} - the CMake-synthesized std module "
        "compiles against the patched <filesystem> shadow "
        "(${CCG_C2801_WA_INC}) via ${_manifest}")
endfunction()

# CMake < 4.5 路径：把影子 include 目录接到 <target> 上
# （预期：cg_std_modules——项目自建的 std.ixx 生产者）。
# 守卫与每种结局的打印在 _ccg_c2801_wire_common；只有经校验的
# 补丁才会改目标的编译属性。
function(ccg_c2801_wire_std_module_shadow target)
    _ccg_c2801_wire_common(_token "std.ixx")
    if(NOT _token)
        return()
    endif()

    target_include_directories(${target} BEFORE PRIVATE "${CCG_C2801_WA_INC}")
    message(STATUS
        "ccg c2801-wa: ${_token} - std.ixx compiles against the patched "
        "<filesystem> shadow (${CCG_C2801_WA_INC})")
endfunction()
