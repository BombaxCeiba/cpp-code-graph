# 端到端闸门脚本的共享宏组（cmake -P 脚本模式）：闸门
# 脚本头部 include 本文件，获得同一套运行与断言宏——
# ccg_run/expect_rc/expect_out/expect_err 四件套、fail/py_run/
# grab_out_count 附属件、expect_absent 负断言（stderr 上不得
# 出现探针）与 expect_no 双流负断言（stdout 与 stderr 都不得
# 出现探针）。报错前缀取自主脚本文件名
#（CMAKE_SCRIPT_MODE_FILE 去扩展名），与各闸门此前逐字复制
# 宏体时硬编码的前缀一致；宏按 CMake 文本展开语义直达调用方
# 作用域变量（exe/out/err/rc/py_args），include 不引入新作用域，
# 行为与宏就地定义无异。仅做前缀推导与宏定义，配置期被
# include 属于接线错误，立即报错。

if(NOT CMAKE_SCRIPT_MODE_FILE)
    message(FATAL_ERROR
        "ccg-harness.cmake: gate macros must be included from a cmake -P "
        "script, not at configure time (the error prefix would be empty)")
endif()
get_filename_component(_ccg_gate "${CMAKE_SCRIPT_MODE_FILE}" NAME_WE)

macro(fail label)
    message(FATAL_ERROR "${_ccg_gate} [${label}]: ${ARGN}")
endmacro()

# 以 ${ARGN} 运行 ccg；捕获 stdout/stderr 与退出码。
macro(ccg_run)
    set(_cmd "${exe}")
    foreach(_a ${ARGN})
        list(APPEND _cmd "${_a}")
    endforeach()
    execute_process(COMMAND ${_cmd}
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE)
endmacro()

macro(expect_rc want label)
    if(NOT rc EQUAL ${want})
        message(FATAL_ERROR
            "${_ccg_gate} [${label}]: exit code ${rc}, wanted ${want}\n--stdout--\n${out}\n--stderr--\n${err}")
    endif()
endmacro()

macro(expect_out needle label)
    string(FIND "${out}" "${needle}" _pos)
    if(_pos EQUAL -1)
        message(FATAL_ERROR
            "${_ccg_gate} [${label}]: stdout missing '${needle}'\n--stdout--\n${out}\n--stderr--\n${err}")
    endif()
endmacro()

macro(expect_err needle label)
    string(FIND "${err}" "${needle}" _pos)
    if(_pos EQUAL -1)
        message(FATAL_ERROR
            "${_ccg_gate} [${label}]: stderr missing '${needle}'\n--stdout--\n${out}\n--stderr--\n${err}")
    endif()
endmacro()

# expect_err 的负孪生：stderr 上不得出现探针。
macro(expect_absent needle label)
    string(FIND "${err}" "${needle}" _pos)
    if(NOT _pos EQUAL -1)
        message(FATAL_ERROR
            "${_ccg_gate} [${label}]: stderr unexpectedly contains '${needle}'\n--stdout--\n${out}\n--stderr--\n${err}")
    endif()
endmacro()

# 双流负断言：stdout 与 stderr 上都不得出现探针。
macro(expect_no needle label)
    string(FIND "${out}" "${needle}" _out_pos)
    string(FIND "${err}" "${needle}" _err_pos)
    if(NOT _out_pos EQUAL -1 OR NOT _err_pos EQUAL -1)
        message(FATAL_ERROR
            "${_ccg_gate} [${label}]: '${needle}' must not appear\n--stdout--\n${out}\n--stderr--\n${err}")
    endif()
endmacro()

# 把 stdout 中 `prefix` 后的第一个整数取进 `value`。
macro(grab_out_count prefix label)
    string(REGEX MATCH "${prefix}([0-9]+)" _m "${out}")
    if(NOT CMAKE_MATCH_1 STREQUAL "")
        set(value "${CMAKE_MATCH_1}")
    else()
        message(FATAL_ERROR
            "${_ccg_gate} [${label}]: stdout missing '${prefix}<n>'\n--stdout--\n${out}\n--stderr--\n${err}")
    endif()
endmacro()

# 运行内嵌 python -I 载荷（sqlite + JSON 比对的特批
# 读取器，沿用 e2e 约定）；${ARGN} 是 argv 尾参。
macro(py_run label)
    execute_process(COMMAND python -I -c "${ARGN}" ${py_args}
        RESULT_VARIABLE py_rc
        OUTPUT_VARIABLE py_out
        ERROR_VARIABLE py_err
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE)
    if(NOT py_rc EQUAL 0)
        message(FATAL_ERROR
            "${_ccg_gate} [${label}]: python check failed (${py_rc}):\n${py_out}\n${py_err}")
    endif()
endmacro()
