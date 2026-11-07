# tidy 目标（cmake/CcgTidy.cmake）背后的执行器。读构建树的
# compile_commands.json，选出工程模块单元（源码根 src/ 与 tests/ 下的
# .ixx——第三方 CPM 源码住在别处、永不匹配），对它们跑一次串行
# clang-tidy 调用。
#
# 工作目录必须是构建目录（tidy 目标已设置）：CMake 经
# @<unit>.modmap 响应文件编译模块单元，其中是 BMI 的相对
# -fmodule-file= 路径，编译器按进程工作目录解析响应文件与
# BMI 路径两者。
#
# 闸门契约：clang-tidy 自身失败，或存在任何工程代码 finding，
# 都以非零退出。clang-tidy 对纯警告退出 0，因此 finding 闸门
# 在这里从捕获的输出计算；相同 finding 先去重（clang-tidy 会按
# 编译数据库展开出的每个导入单元重复分析同一模块单元一次）。

if(NOT DEFINED TIDY_EXE OR NOT DEFINED BINARY_DIR OR NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR
        "ccg-tidy.cmake: expected -DTIDY_EXE=<path> -DBINARY_DIR=<path> "
        "-DSOURCE_DIR=<path>")
endif()

set(_db "${BINARY_DIR}/compile_commands.json")
if(NOT EXISTS "${_db}")
    message(FATAL_ERROR
        "ccg tidy: '${_db}' missing - reconfigure a preset with "
        "CMAKE_EXPORT_COMPILE_COMMANDS=ON (the shipped presets set it).")
endif()

# ---------------------------------------------------------------- 文件集 --
file(READ "${_db}" _json)
string(REGEX MATCHALL "\"file\": *\"[^\"]+\"" _file_fields "${_json}")
set(_files "")
foreach(_field IN LISTS _file_fields)
    string(REGEX REPLACE "\"file\": *\"([^\"]+)\"" "\\1" _f "${_field}")
    # JSON 转义的反斜杠在匹配前换成普通分隔符。
    string(REPLACE "\\\\" "/" _f "${_f}")
    # 前缀判定用 string(FIND)：源码根可能含正则元字符，
    # 绝不能让它漏进 MATCHES 模式。
    string(FIND "${_f}" "${SOURCE_DIR}/src/" _in_src)
    string(FIND "${_f}" "${SOURCE_DIR}/tests/" _in_tests)
    if((_in_src EQUAL 0 OR _in_tests EQUAL 0) AND _f MATCHES "\\.ixx$")
        list(APPEND _files "${_f}")
    endif()
endforeach()
list(REMOVE_DUPLICATES _files)
list(SORT _files)
list(LENGTH _files _file_count)
if(_file_count EQUAL 0)
    message(FATAL_ERROR
        "ccg tidy: no .ixx entries under '${SOURCE_DIR}/src' or "
        "'${SOURCE_DIR}/tests' in '${_db}'.")
endif()

# ------------------------------------------------------------------- 调用 --
# 按设计串行：本 clang-tidy（23.1.3）没有 -j 工作进程选项。
set(_cmd "${TIDY_EXE}" -p "${BINARY_DIR}"
    -extra-arg=-Wno-unused-command-line-argument)
list(APPEND _cmd ${_files})

execute_process(COMMAND ${_cmd}
    WORKING_DIRECTORY "${BINARY_DIR}"
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err
    RESULT_VARIABLE _rc)

# ------------------------------------------------------------------- 闸门 --
# 工程 finding 行的形态：<源码根下的路径>:行:列: warning:
# …… [check-name]。路径分隔符先归一化，源码根先转义，
# 使其盘符/点号不能充当正则运算符。
set(_all "${_out}\n${_err}")
string(REPLACE "\\" "/" _norm "${_all}")
# 源码根做正则转义（分隔符先归一化），使用户路径的任何字符
# 都无法在下方 finding 模式里充当运算符。
set(_src_re "${SOURCE_DIR}")
string(REPLACE "\\" "/" _src_re "${_src_re}")
string(REGEX REPLACE "([][.+*?()^$|-])" "\\\\\\1" _src_re "${_src_re}")
string(REGEX MATCHALL
    "(${_src_re}/(src|tests)/[^ \r\n:]*):[0-9]+:[0-9]+: warning: [^\r\n]*"
    _hits "${_norm}")
list(REMOVE_DUPLICATES _hits)   # 依赖重分析会重复同一单元

if(NOT _rc EQUAL 0)
    message("${_all}")
    message(FATAL_ERROR "ccg tidy: clang-tidy failed with exit ${_rc}.")
endif()

list(LENGTH _hits _finding_count)
if(_finding_count GREATER 0)
    foreach(_line IN LISTS _hits)
        # 检查名形态为 [check-name]；裸 clang 诊断在 clang-tidy 输出里
        # 打成 [-Wwarning-name]——两种都计入。
        string(REGEX MATCH "\\[-?[A-Za-z0-9.\-]+\\]$" _bracketed "${_line}")
        if(NOT _bracketed STREQUAL "")
            # "[check-name]" -> "check-name"：作变量名后缀是安全的
            string(REGEX REPLACE "\\[|\\]" "" _check "${_bracketed}")
            if(NOT DEFINED _cnt_${_check})
                set(_cnt_${_check} 0)
                list(APPEND _checks_seen "${_check}")
            endif()
            math(EXPR _cnt_${_check} "${_cnt_${_check}} + 1")
        endif()
    endforeach()
    list(SORT _checks_seen)
    message("${_all}")
    set(_summary "")
    foreach(_check IN LISTS _checks_seen)
        string(APPEND _summary "\n  [${_check}] ${_cnt_${_check}}")
    endforeach()
    message(FATAL_ERROR
        "ccg tidy: FAIL - ${_finding_count} project finding(s) in "
        "${_file_count} module unit(s), by check:${_summary}")
endif()
message(STATUS
    "ccg tidy: PASS - 0 project findings across ${_file_count} module units")
