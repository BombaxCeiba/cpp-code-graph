# 配置期的 C2801 规避补丁器（脚本模式：`cmake -P`）。
#
# 用途：MSVC 工具集的 <filesystem> 把 _Path_iterator 的
# operator== 定义为类内隐藏友元；该形态被错误序列化进
# std BMI，随后又被 cl 的模块路径错误归位成成员声明，
# 发出虚假的 C2801。经验证的修复方案保留类内模板友元
# 声明，并把定义移到类外——查找、ADL 与重载决议均不变。
# 本脚本在不改动工具集文件的前提下
# 产出该补丁副本，受以下硬性安全规则约束：
#   a) 每个文本锚点必须在源中恰好匹配一次
#      （出现 0 次或超过 1 次则拒绝打补丁）；
#   b) 补丁后的头文件必须通过 cl /Zs 语法自检，且补丁
#      include 目录排在最前（原目录作为对照腿）；任何失败都
#      撤回补丁（构建回落到未打补丁的头文件）；
#   c) 一枚 sha256 戳记录源与输出的哈希；源相同且输出未被改动
#      则复用，其余情况一律重新拷贝并重新打补丁；
#   d) 每条分支都打印一行 STATUS/WARNING 并写 status.txt——
#      不允许任何静默失败。
#
# 参数（在 -P 之前用 -D 传入）：
#   C2801_SRC      要读取的工具集 <filesystem> 头文件（从不修改）
#   C2801_OUT_DIR  要填充的影子 include 目录（<dir>/filesystem）
#   C2801_WORK_DIR 戳、状态与日志所在目录（默认：OUT_DIR 的父目录）
#   C2801_CL       用于语法自检的 cl.exe
#
# 输出：
#   ${C2801_WORK_DIR}/status.txt  单一枚举值：patched-ok |
#                                 reused-patched-ok | fell-back:<原因> |
#                                 script-error:<原因>
#   ${C2801_OUT_DIR}/filesystem   打过补丁的影子头文件（仅当最后一次
#                                 写入的状态是 ok 枚举值时才存在）
#   ${C2801_WORK_DIR}/logs/       cl 自检记录（两条腿）
#   ${C2801_WORK_DIR}/source.sha256 戳（source_sha256=/output_sha256=）

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED C2801_SRC OR NOT DEFINED C2801_OUT_DIR)
    message(FATAL_ERROR "ccg c2801-wa: C2801_SRC and C2801_OUT_DIR are required")
endif()
if(NOT DEFINED C2801_WORK_DIR)
    get_filename_component(C2801_WORK_DIR "${C2801_OUT_DIR}" DIRECTORY)
endif()

set(_status_file "${C2801_WORK_DIR}/status.txt")
set(_stamp_file "${C2801_WORK_DIR}/source.sha256")
set(_log_dir "${C2801_WORK_DIR}/logs")
set(_cand_dir "${C2801_WORK_DIR}/candidate")
set(_out_hdr "${C2801_OUT_DIR}/filesystem")
set(_ok_tokens "patched-ok" "reused-patched-ok")

# 统计 <needle> 在 <haystack> 中不重叠的出现次数。原生
# string(FIND) 没有起始偏移参数，因此每次命中后把 haystack 切掉前段再找。
function(_c2801_count haystack needle out_var)
    string(LENGTH "${needle}" _nlen)
    set(_count 0)
    set(_rest "${haystack}")
    while(TRUE)
        string(FIND "${_rest}" "${needle}" _at)
        if(_at LESS 0)
            break()
        endif()
        math(EXPR _count "${_count} + 1")
        math(EXPR _skip "${_at} + ${_nlen}")
        string(SUBSTRING "${_rest}" ${_skip} -1 _rest)
    endwhile()
    set(${out_var} "${_count}" PARENT_SCOPE)
endfunction()

# 语法自检：对一个包含 <filesystem> 且 odr-使用补丁后
# operator== 的微型 TU 跑 cl /Zs。<inc_dir> 排在 INCLUDE 之前搜索。
function(_c2801_cl_check cl inc_dir tu work_dir log_file out_rc)
    execute_process(
        COMMAND "${cl}" /nologo /Zs /std:c++latest /utf-8 /I "${inc_dir}" "${tu}"
        WORKING_DIRECTORY "${work_dir}"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _so
        ERROR_VARIABLE _se)
    file(WRITE "${log_file}"
        "cl: ${cl}\ninclude dir (first): ${inc_dir}\ntu: ${tu}\nrc: ${_rc}\n--- stdout ---\n${_so}--- stderr ---\n${_se}")
    set(${out_rc} "${_rc}" PARENT_SCOPE)
endfunction()

file(MAKE_DIRECTORY "${C2801_WORK_DIR}")

# 撤回影子头文件（每个非 ok 的退出路径都会调用；不变式是
# “OUT_DIR 持有头文件，当且仅当最后一次写入的状态是 ok 枚举值”）。
function(_c2801_withdraw)
    if(EXISTS "${_out_hdr}")
        file(REMOVE "${_out_hdr}")
    endif()
endfunction()

if(NOT EXISTS "${C2801_SRC}")
    _c2801_withdraw()
    file(WRITE "${_status_file}" "fell-back:source-missing\n")
    message(WARNING
        "ccg c2801-wa: toolset header not found ('${C2801_SRC}') - no patch "
        "applied, std.ixx will compile against the regular toolset header.")
    return()
endif()

# 防止把影子目录指向真实的工具集 include 目录。
get_filename_component(_src_real "${C2801_SRC}" REALPATH)
get_filename_component(_out_real "${C2801_OUT_DIR}" REALPATH)
get_filename_component(_src_real_dir "${_src_real}" DIRECTORY)
if(_out_real STREQUAL _src_real_dir)
    _c2801_withdraw()
    file(WRITE "${_status_file}" "fell-back:bad-args\n")
    message(WARNING
        "ccg c2801-wa: C2801_OUT_DIR ('${C2801_OUT_DIR}') is the toolset "
        "include dir itself - refusing to patch in place.")
    return()
endif()

file(SHA256 "${_src_real}" _src_sha)

# 戳复用：源哈希相同、补丁输出完好、上一次运行状态为 ok ->
# 无事可做（该输出在产出当时已经过 cl 验证）。
set(_reuse FALSE)
if(EXISTS "${_stamp_file}" AND EXISTS "${_status_file}" AND EXISTS "${_out_hdr}")
    file(READ "${_stamp_file}" _stamp)
    file(READ "${_status_file}" _prev_status)
    string(STRIP "${_prev_status}" _prev_status)
    file(SHA256 "${_out_hdr}" _cur_out_sha)
    string(REGEX MATCH "source_sha256=([0-9a-fA-F]+)" _m "${_stamp}")
    set(_stamp_src "${CMAKE_MATCH_1}")
    string(REGEX MATCH "output_sha256=([0-9a-fA-F]+)" _m "${_stamp}")
    set(_stamp_out "${CMAKE_MATCH_1}")
    if("${_stamp_src}" STREQUAL "${_src_sha}"
            AND "${_stamp_out}" STREQUAL "${_cur_out_sha}"
            AND _prev_status IN_LIST _ok_tokens)
        set(_reuse TRUE)
    endif()
endif()

if(_reuse)
    file(WRITE "${_status_file}" "reused-patched-ok\n")
    message(STATUS
        "ccg c2801-wa: reusing patched <filesystem> shadow (stamp matches, "
        "source sha256 ${_src_sha}) - '${_out_hdr}'")
    return()
endif()

# ---- 完整流水线：拷贝 -> 打补丁 -> 自检 -> 发布 ----------------
file(READ "${_src_real}" _content)

# 逐字节对齐源文件的行尾风格（当前是 CRLF，LF 也保持合法）。
string(FIND "${_content}" "\r\n" _crlf_at)
if(NOT _crlf_at LESS 0)
    set(_nl "\r\n")
else()
    set(_nl "\n")
endif()

# 锚点 A：类内隐藏友元定义（触发 C2801 的形态）。
set(_anchor_a "")
string(APPEND _anchor_a "        _NODISCARD friend bool operator==(const _Path_iterator& _Lhs, const _Path_iterator& _Rhs) {${_nl}")
string(APPEND _anchor_a "            return _Lhs._Position == _Rhs._Position;${_nl}")
string(APPEND _anchor_a "        }")

# 替换 A：类内模板友元声明，与下方
# 类外定义配对（经验证的形态，语义零变化）。
set(_repl_a "")
string(APPEND _repl_a "        template <class _Other>${_nl}")
string(APPEND _repl_a "        friend bool operator==(const _Path_iterator<_Other>& _Lhs, const _Path_iterator<_Other>& _Rhs);")

# 锚点 B：_Path_iterator 类的尾部；类外定义
# 就插在类的收尾花括号之后。
set(_anchor_b "")
string(APPEND _anchor_b "        _Base_iter _Position{};${_nl}")
string(APPEND _anchor_b "        path _Element{};${_nl}")
string(APPEND _anchor_b "        const path* _Mypath{};${_nl}")
string(APPEND _anchor_b "    };")

# 插入 B：空行 + 类外定义（不带尾随换行；
# 源文件自身随后的空行原位保留）。
set(_insert_b "")
string(APPEND _insert_b "${_nl}${_nl}")
string(APPEND _insert_b "    template <class _Base_iter>${_nl}")
string(APPEND _insert_b "    _NODISCARD bool operator==(const _Path_iterator<_Base_iter>& _Lhs, const _Path_iterator<_Base_iter>& _Rhs) {${_nl}")
string(APPEND _insert_b "        return _Lhs._Position == _Rhs._Position;${_nl}")
string(APPEND _insert_b "    }")

_c2801_count("${_content}" "${_anchor_a}" _n_a)
if(_n_a EQUAL 0)
    _c2801_withdraw()
    file(WRITE "${_status_file}" "fell-back:pattern-absent\n")
    message(STATUS
        "ccg c2801-wa: the hidden-friend operator== block was not found in "
        "the toolset <filesystem> - no patch applied. If the toolset was "
        "updated this is the expected retirement once the upstream fix "
        "arrives; std.ixx compiles against the unpatched "
        "header either way.")
    return()
endif()
if(_n_a GREATER 1)
    _c2801_withdraw()
    file(WRITE "${_status_file}" "fell-back:pattern-not-unique\n")
    message(WARNING
        "ccg c2801-wa: the hidden-friend operator== block occurs ${_n_a}x "
        "in the toolset <filesystem> (expected exactly 1) - refusing to "
        "patch. std.ixx compiles against the unpatched header.")
    return()
endif()

string(REPLACE "${_anchor_a}" "${_repl_a}" _patched "${_content}")

# 在打过补丁的文本上重新数锚点 B，使插入点恰好落在
# 即将交付的那些字节上得到验证。
_c2801_count("${_patched}" "${_anchor_b}" _n_b)
if(NOT _n_b EQUAL 1)
    _c2801_withdraw()
    file(WRITE "${_status_file}" "fell-back:anchor-class-tail-${_n_b}\n")
    message(WARNING
        "ccg c2801-wa: the _Path_iterator class tail anchor occurs ${_n_b}x "
        "in the toolset <filesystem> (expected exactly 1) - refusing to "
        "patch. std.ixx compiles against the unpatched header.")
    return()
endif()

string(FIND "${_patched}" "${_anchor_b}" _b_at)
string(LENGTH "${_anchor_b}" _b_len)
math(EXPR _ins "${_b_at} + ${_b_len}")
string(SUBSTRING "${_patched}" 0 ${_ins} _head)
string(SUBSTRING "${_patched}" ${_ins} -1 _tail)
set(_final "${_head}${_insert_b}${_tail}")

# 先发布进候选目录：OUT_DIR 只接收
# 已通过 cl 自检的头文件。
file(REMOVE_RECURSE "${_cand_dir}")
file(MAKE_DIRECTORY "${_cand_dir}")
file(WRITE "${_cand_dir}/filesystem" "${_final}")
file(SHA256 "${_cand_dir}/filesystem" _cand_sha)

if(NOT DEFINED C2801_CL OR NOT EXISTS "${C2801_CL}")
    _c2801_withdraw()
    file(WRITE "${_status_file}" "fell-back:cl-unavailable\n")
    message(WARNING
        "ccg c2801-wa: cl.exe ('${C2801_CL}') not available for the syntax "
        "self-check - refusing to publish an unverified header. std.ixx "
        "compiles against the unpatched header.")
    return()
endif()

file(MAKE_DIRECTORY "${_log_dir}" "${C2801_WORK_DIR}/selfcheck")
set(_tu "${C2801_WORK_DIR}/selfcheck/c2801_selfcheck_tu.cpp")
file(WRITE "${_tu}" "// C2801 workaround self-check TU: the patched <filesystem> must
// pass a plain syntax check, including an odr-use of _Path_iterator's
// operator== (the exact entity the patch touches).
#include <filesystem>
bool c2801_wa_eq(const std::filesystem::path::iterator& _Lhs,
    const std::filesystem::path::iterator& _Rhs) {
    return _Lhs == _Rhs;
}
int main() {
    return 0;
}
")

_c2801_cl_check("${C2801_CL}" "${_cand_dir}" "${_tu}" "${C2801_WORK_DIR}/selfcheck"
    "${_log_dir}/selfcheck-patched.log" _rc_patched)

if(NOT _rc_patched EQUAL 0)
    # 对照腿：原始头文件必须在本环境中能编译通过，
    # 否则那次失败说明不了补丁的任何问题。
    _c2801_cl_check("${C2801_CL}" "${_src_real_dir}" "${_tu}" "${C2801_WORK_DIR}/selfcheck"
        "${_log_dir}/selfcheck-control.log" _rc_control)
    if(_rc_control EQUAL 0)
        file(WRITE "${_status_file}" "fell-back:self-check-failed\n")
        message(WARNING
            "ccg c2801-wa: cl syntax self-check FAILED on the patched "
            "<filesystem> (the unpatched header passes) - patch withdrawn, "
            "std.ixx compiles against the unpatched header. Transcript: "
            "${_log_dir}/selfcheck-patched.log")
    else()
        file(WRITE "${_status_file}" "fell-back:cl-env-broken\n")
        message(WARNING
            "ccg c2801-wa: cl cannot syntax-check even the ORIGINAL "
            "<filesystem> here (environment problem, e.g. no vcvars INCLUDE) "
            "- patch withdrawn. Transcripts: ${_log_dir}/selfcheck-{patched,control}.log")
    endif()
    _c2801_withdraw()
    file(REMOVE_RECURSE "${_cand_dir}")
    return()
endif()

file(MAKE_DIRECTORY "${C2801_OUT_DIR}")
if(EXISTS "${_out_hdr}")
    file(REMOVE "${_out_hdr}")
endif()
file(RENAME "${_cand_dir}/filesystem" "${_out_hdr}")
file(REMOVE_RECURSE "${_cand_dir}")
file(WRITE "${_stamp_file}"
    "source_sha256=${_src_sha}\noutput_sha256=${_cand_sha}\n")
file(WRITE "${_status_file}" "patched-ok\n")
message(STATUS
    "ccg c2801-wa: patched <filesystem> shadow published and cl-verified "
    "(source sha256 ${_src_sha}) - '${_out_hdr}'")
