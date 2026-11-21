# BMI 分层并行 e2e（设计=设计文档测试清单 (3)-(7)
# 与附录二 T2-T7，另含追加的 update 边缘三形态）。
#
# 夹具全部是手写 clang 方言 compile db（无喂给条目 → self_build 路由，
# .ixx 后缀即模块单元），不依赖构建树形态，MSVC/clang 两树皆真跑；
# 引擎不可发现的机器响亮 SKIP 而非失败（首段冒烟判定）。
#   段 0  引擎冒烟（不可用 → SKIP）
#   M8b  层间屏障/闭包完整：--jobs 4 三层夹具全绿、每层消费者符号
#        可 query（下游构建喂到的上游 pcm 必已落盘——屏障的端到端
#        证据；喂给前在场校验即既有的 module-bmi-missing 拒绝面）
#   M8d  失败三形态：接口语法错（同层不株连 + 失败保旧）、下游
#        级联点名（blocker 文本传递）、环降级（module-dag-cycle）
#   M8g  增量闭包：二跑全复用 + 改一个接口恰重建其下游闭包
#   M8c  并行 vs 串行逐字节：同一缓存目录 CCG_BMI_SERIAL 轮与并行轮
#        （各 --full）全部 pcm SHA256 相等 + manifest 逐字节相等 +
#        并行轮自产缓存三跑全复用
#   M8h  并发上限贯通：--jobs 2 / config index-jobs 3 / 旗标 5 压
#        配置 3 / 窄层天然封顶（第一段 note 的 W 与第二段 -v 行的 M）
#   M8i  提速夹具（Windows 计时）：宽图 12 独立接口 BMI 段 ≥3x、
#        深链 24 层 BMI 段 ≤1.1x（口径=接口 pcm 的 mtime 跨度，
#        与预研/棒二的量法一致，避开提交段噪声）
#   M8u  update 边缘三形态（追加指令，独立验收判据）：
#        u1 排队期间改（持有者跑批 A 时改源入队批 B → 批 A 完成后
#           批 B 身份失配自动重建，收敛断言）
#        u2 构建期间改（index 构建中改源 → 收敛轮 + --full 全量
#           对照轮逐字节相等 = 不错配固化断言）
#        u3 层间窗口改（层 K 构建中改层 K+1 的接口 → 该层读到新
#           内容、身份记录正确、无失配重建——此形态直接正确）
#
# 一个文件的两副面孔（llvm-e2e.cmake 模式）：
#   从 tests/CMakeLists.txt INCLUDE 进来 -> 注册 ccg_bmi_e2e ctest
#   经 cmake -P bmi-e2e.cmake <ccg.exe> <work-dir> 运行。

if(NOT CMAKE_SCRIPT_MODE_FILE)
    add_test(NAME ccg_bmi_e2e
             COMMAND ${CMAKE_COMMAND} -P
                     "${CMAKE_CURRENT_SOURCE_DIR}/bmi-e2e.cmake"
                     $<TARGET_FILE:ccg>
                     "${CMAKE_BINARY_DIR}/test-bmi-e2e")
    ccg_test_path_mods(_bmi_mods)
    set_tests_properties(ccg_bmi_e2e PROPERTIES
        ENVIRONMENT_MODIFICATION "${_bmi_mods}"
        SKIP_REGULAR_EXPRESSION "\\*\\*\\* SKIPPED"
        TIMEOUT 900)
    return()
endif()

if(NOT DEFINED CMAKE_ARGV4)
    message(FATAL_ERROR
        "bmi-e2e.cmake: expected <ccg.exe> <work-dir>")
endif()
set(exe  "${CMAKE_ARGV3}")
set(work "${CMAKE_ARGV4}")

file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}")

# ---------------------------------------------------------------- 宏 --
# 运行/断言宏组（ccg_run/expect_rc/expect_out/expect_err/
# grab_out_count）收纳在同目录 ccg-harness.cmake，供各闸门脚本
# 共享；本闸门特有的轮询宏留在下面。
include("${CMAKE_CURRENT_LIST_DIR}/ccg-harness.cmake")

# 轮询等待文件出现（每秒一次；超时即失败点名）。
macro(wait_for_file path label tries)
    set(_wf_found FALSE)
    foreach(_i RANGE 1 ${tries})
        if(EXISTS "${path}")
            set(_wf_found TRUE)
            break()
        endif()
        execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 1)
    endforeach()
    if(NOT _wf_found)
        message(FATAL_ERROR
            "bmi-e2e [${label}]: timed out waiting for ${path}")
    endif()
endmacro()

# ------------------------------------------------------- 夹具生成器 --
# 三层混合 DAG（无 std）：base / solo <- mid1, mid2 <- top + use.cpp。
# 层切分恰为 [base, solo] [mid1, mid2] [top]，5 个接口 + 1 个消费 TU。
function(gen_mixed srcdir)
    file(MAKE_DIRECTORY "${srcdir}")
    file(WRITE "${srcdir}/base.ixx" [[
export module base;
export inline int base_v1() { return 1; }
]])
    file(WRITE "${srcdir}/mid1.ixx" [[
export module mid1;
import base;
export inline int mid1_v() { return base_v1() + 10; }
]])
    file(WRITE "${srcdir}/mid2.ixx" [[
export module mid2;
import base;
export inline int mid2_v() { return base_v1() + 20; }
]])
    file(WRITE "${srcdir}/top.ixx" [[
export module top;
import mid1;
import mid2;
export inline int top_v() { return mid1_v() + mid2_v(); }
]])
    file(WRITE "${srcdir}/solo.ixx" [[
export module solo;
export inline int solo_v() { return 5; }
]])
    file(WRITE "${srcdir}/use.cpp" [[
import top;
int use_top() { return top_v(); }
]])
    set(_entries "")
    foreach(_f base.ixx mid1.ixx mid2.ixx top.ixx solo.ixx use.cpp)
        string(APPEND _entries
            " {\"directory\": \"${srcdir}\", \"arguments\": [\"clang++\", \"-std=c++20\", \"-c\", \"${srcdir}/${_f}\"], \"file\": \"${srcdir}/${_f}\"},\n")
    endforeach()
    string(REGEX REPLACE ",\n?$" "" _entries "${_entries}")
    file(WRITE "${srcdir}/compile_commands.json" "[\n${_entries}]\n")
endfunction()

# 宽图：count 个互不依赖的接口（repeat 控制单模块体量），无 std。
function(gen_wide srcdir count repeat)
    file(MAKE_DIRECTORY "${srcdir}")
    string(REPEAT "s += 1; s ^= 3; s <<= 1; s >>= 1; " ${repeat} _body)
    set(_entries "")
    foreach(_i RANGE 0 ${count})
        if(_i GREATER_EQUAL ${count})
            break()
        endif()
        if(_i LESS 10)
            set(_nn "0${_i}")
        else()
            set(_nn "${_i}")
        endif()
        file(WRITE "${srcdir}/w${_nn}.ixx"
"export module w${_nn};
export inline unsigned long long w${_nn}_work() {
    unsigned long long s = 0;
    ${_body}
    return s;
}
")
        string(APPEND _entries
            " {\"directory\": \"${srcdir}\", \"arguments\": [\"clang++\", \"-std=c++20\", \"-c\", \"${srcdir}/w${_nn}.ixx\"], \"file\": \"${srcdir}/w${_nn}.ixx\"},\n")
    endforeach()
    string(REGEX REPLACE ",\n?$" "" _entries "${_entries}")
    file(WRITE "${srcdir}/compile_commands.json" "[\n${_entries}]\n")
endfunction()

# 深链：count 层每层 1 接口（d00 <- d01 <- ...），可选 use.cpp。
function(gen_deep srcdir count with_use)
    file(MAKE_DIRECTORY "${srcdir}")
    file(WRITE "${srcdir}/d00.ixx" [[
export module d00;
export inline int d00_v() { return 0; }
]])
    foreach(_i RANGE 1 ${count})
        if(_i GREATER_EQUAL ${count})
            break()
        endif()
        if(_i LESS 10)
            set(_nn "0${_i}")
        else()
            set(_nn "${_i}")
        endif()
        set(_prev ${_nn})
        math(EXPR _p "${_i} - 1")
        if(_p LESS 10)
            set(_pn "0${_p}")
        else()
            set(_pn "${_p}")
        endif()
        file(WRITE "${srcdir}/d${_nn}.ixx"
"export module d${_nn};
import d${_pn};
export inline int d${_nn}_v() { return d${_pn}_v() + 1; }
")
    endforeach()
    set(_entries "")
    foreach(_i RANGE 0 ${count})
        if(_i GREATER_EQUAL ${count})
            break()
        endif()
        if(_i LESS 10)
            set(_nn "0${_i}")
        else()
            set(_nn "${_i}")
        endif()
        string(APPEND _entries
            " {\"directory\": \"${srcdir}\", \"arguments\": [\"clang++\", \"-std=c++20\", \"-c\", \"${srcdir}/d${_nn}.ixx\"], \"file\": \"${srcdir}/d${_nn}.ixx\"},\n")
    endforeach()
    if(with_use)
        math(EXPR _last "${count} - 1")
        if(_last LESS 10)
            set(_ln "0${_last}")
        else()
            set(_ln "${_last}")
        endif()
        file(WRITE "${srcdir}/use.cpp"
"import d${_ln};
int use_deep() { return d${_ln}_v(); }
")
        string(APPEND _entries
            " {\"directory\": \"${srcdir}\", \"arguments\": [\"clang++\", \"-std=c++20\", \"-c\", \"${srcdir}/use.cpp\"], \"file\": \"${srcdir}/use.cpp\"},\n")
    endif()
    string(REGEX REPLACE ",\n?$" "" _entries "${_entries}")
    file(WRITE "${srcdir}/compile_commands.json" "[\n${_entries}]\n")
endfunction()

# 厚 base 三层 DAG（u2/u3 用，不 import std——std 段的存在与否依赖
# 引擎方言配置，测试聚焦分层窗口语义，不该把窗口押在 std 上）：厚
# base（层 0）单独拉长"层 1 之前"的改动安全窗口。
function(gen_bigmixed srcdir repeat)
    file(MAKE_DIRECTORY "${srcdir}")
    string(REPEAT "s += 1; s ^= 3; s <<= 1; s >>= 1; " ${repeat} _body)
    file(WRITE "${srcdir}/bb.ixx"
"export module bb;
export inline unsigned long long bb_work() {
    unsigned long long s = 0;
    ${_body}
    return s;
}
")
    file(WRITE "${srcdir}/bm1.ixx" [[
export module bm1;
import bb;
export inline int bm1_v() { return static_cast<int>(bb_work() & 1) + 1; }
]])
    file(WRITE "${srcdir}/bm2.ixx" [[
export module bm2;
import bb;
export inline int bm2_v() { return 2; }
]])
    file(WRITE "${srcdir}/bt.ixx" [[
export module bt;
import bm1;
import bm2;
export inline int bt_v() { return bm1_v() + bm2_v(); }
]])
    file(WRITE "${srcdir}/bu.cpp" [[
import bt;
int bu_v() { return bt_v(); }
]])
    set(_entries "")
    foreach(_f bb.ixx bm1.ixx bm2.ixx bt.ixx bu.cpp)
        string(APPEND _entries
            " {\"directory\": \"${srcdir}\", \"arguments\": [\"clang++\", \"-std=c++20\", \"-c\", \"${srcdir}/${_f}\"], \"file\": \"${srcdir}/${_f}\"},\n")
    endforeach()
    string(REGEX REPLACE ",\n?$" "" _entries "${_entries}")
    file(WRITE "${srcdir}/compile_commands.json" "[\n${_entries}]\n")
endfunction()

# ------------------------------------------------------------- 段 0 --
message(STATUS "bmi-e2e: engine smoke (2-module mini index)")
set(smoke "${work}/smoke")
gen_deep("${smoke}" 2 FALSE)
ccg_run(index "${smoke}" --compile-commands "${smoke}/compile_commands.json"
        -o "${work}" --config smoke)
if(NOT rc EQUAL 0)
    # 引擎不可发现/不可加载（libclang、engine、LLVM 字样）是环境面：
    # 响亮 SKIP，绝不把环境缺失记成失败；其余退出码照样红。
    #（cmake 的 MATCHES 是自有正则方言，无内联 (?i) 旗标。）
    if("${err};${out}" MATCHES "[Ll]ibclang|[Ee]ngine|[Ll][Ll][Vv][Mm]")
        message("*** SKIPPED: no usable clang engine for bmi-e2e "
                "(first index failed: ${err})")
        return()
    endif()
    message(FATAL_ERROR
        "bmi-e2e [smoke]: index failed\n--stdout--\n${out}\n--stderr--\n${err}")
endif()
expect_out("failed: 0" "smoke every TU parses")

# ------------------------------------------------------------- M8b --
message(STATUS "bmi-e2e: M8b layer barrier / closure completeness")
set(m8b "${work}/m8b")
gen_mixed("${m8b}")
ccg_run(index "${m8b}" --compile-commands "${m8b}/compile_commands.json"
        -o "${work}" --config m8b -v --jobs 4)
expect_rc(0 "M8b index exit")
expect_out("failed: 0" "M8b every TU parses through fed BMIs")
expect_out("imports: " "M8b imports reported")
grab_out_count("imports: " "M8b imports count")
if(value LESS 3)
    message(FATAL_ERROR
        "bmi-e2e [M8b]: expected at least 3 import relations, got ${value}")
endif()
# 三层夹具、最宽层 2：层宽天然封顶 --jobs 4。
expect_err("3 layers on up to 2 workers" "M8b the layered note names shape and cap")
# 屏障与闭包完整的端到端证据：每一层的消费者符号都可查询（下游
# 构建与解析喂到的上游 pcm 必已落盘，否则 module-bmi-missing /
# module-build-failed 早已把 failed 抬起来）。
ccg_run(query base_v1 -o "${work}" --config m8b)
expect_rc(0 "M8b layer-0 symbol queryable")
ccg_run(query mid1_v -o "${work}" --config m8b)
expect_rc(0 "M8b layer-1 consumer symbol queryable (closure fed)")
ccg_run(query mid2_v -o "${work}" --config m8b)
expect_rc(0 "M8b layer-1 second consumer symbol queryable")
ccg_run(query top_v -o "${work}" --config m8b)
expect_rc(0 "M8b layer-2 consumer symbol queryable (full closure)")
ccg_run(query use_top -o "${work}" --config m8b)
expect_rc(0 "M8b classic importing TU symbol queryable")

# ------------------------------------------------------------- M8d --
message(STATUS "bmi-e2e: M8d failure shapes (same layer / cascade / cycle)")
# 好轮先行：拿到 mid1 的好产物并记哈希（失败保旧的对照物）。
set(m8d "${work}/m8d")
gen_mixed("${m8d}")
ccg_run(index "${m8d}" --compile-commands "${m8d}/compile_commands.json"
        -o "${work}" --config m8d)
expect_rc(0 "M8d healthy baseline index")
expect_out("failed: 0" "M8d baseline clean")
file(GLOB _m8d_pcm "${work}/m8d/bmi-self-*/*/*/mid1.pcm")
list(LENGTH _m8d_pcm _m8d_n)
if(NOT _m8d_n EQUAL 1)
    message(FATAL_ERROR
        "bmi-e2e [M8d]: expected exactly one mid1.pcm, found ${_m8d_n}")
endif()
file(SHA256 "${_m8d_pcm}" _m8d_pcm_hash)

# d1+d2：把 mid1 注入语法错误（M7c 同款坏形态）。
file(WRITE "${m8d}/mid1.ixx" [[
export module mid1;
import base;
export int mid1_broken( {
]])
ccg_run(index "${m8d}" --compile-commands "${m8d}/compile_commands.json"
        -o "${work}" --config m8d -v --jobs 4)
expect_rc(0 "M8d broken-mid1 run exit (per-TU failure is not a run failure)")
# mid1 自身 TU 真解析撞坏语法；top（blocked）与 use（级联）带
# module-build-failed——同层 mid2 与上/无关层照建。
expect_out("failed: 3" "M8d exactly the broken interface plus its closure fail")
expect_out("failure classes: module-build-failed x2, parse-failed x1"
           "M8d failure classes summarized")
expect_err("for 'mid1'" "M8d the failing interface is named in the diagnostics")
# 同层不株连：mid2（与 mid1 同层）照建、符号可查询。
ccg_run(query mid2_v -o "${work}" --config m8d)
expect_rc(0 "M8d same-layer sibling built and queryable")
# 失败保旧：mid1.pcm 原样保留（上一轮好产物的字节一字不动）。
file(SHA256 "${_m8d_pcm}" _m8d_pcm_hash2)
if(NOT _m8d_pcm_hash2 STREQUAL "${_m8d_pcm_hash}")
    message(FATAL_ERROR
        "bmi-e2e [M8d]: the old mid1.pcm was not preserved across the failure")
endif()

# d3：环降级。ca <-> cb 互相 import，独立好模块 good 不受株连。
set(m8cyc "${work}/m8cyc")
file(MAKE_DIRECTORY "${m8cyc}")
file(WRITE "${m8cyc}/ca.ixx" [[
export module ca;
import cb;
export inline int ca_v() { return 3; }
]])
file(WRITE "${m8cyc}/cb.ixx" [[
export module cb;
import ca;
export inline int cb_v() { return 4; }
]])
file(WRITE "${m8cyc}/good.ixx" [[
export module good;
export inline int cyc_good_v() { return 9; }
]])
file(WRITE "${m8cyc}/usegood.cpp" [[
import good;
int cyc_use() { return cyc_good_v(); }
]])
file(WRITE "${m8cyc}/compile_commands.json" "[
 {\"directory\": \"${m8cyc}\", \"arguments\": [\"clang++\", \"-std=c++20\", \"-c\", \"${m8cyc}/ca.ixx\"], \"file\": \"${m8cyc}/ca.ixx\"},
 {\"directory\": \"${m8cyc}\", \"arguments\": [\"clang++\", \"-std=c++20\", \"-c\", \"${m8cyc}/cb.ixx\"], \"file\": \"${m8cyc}/cb.ixx\"},
 {\"directory\": \"${m8cyc}\", \"arguments\": [\"clang++\", \"-std=c++20\", \"-c\", \"${m8cyc}/good.ixx\"], \"file\": \"${m8cyc}/good.ixx\"},
 {\"directory\": \"${m8cyc}\", \"arguments\": [\"clang++\", \"-std=c++20\", \"-c\", \"${m8cyc}/usegood.cpp\"], \"file\": \"${m8cyc}/usegood.cpp\"}
]")
ccg_run(index "${m8cyc}" --compile-commands "${m8cyc}/compile_commands.json"
        -o "${work}" --config m8cyc -v --jobs 4)
expect_rc(0 "M8d cycle run exit")
expect_out("failed: 2" "M8d both cycle members degrade")
expect_out("module-dag-cycle x2" "M8d the cycle class is booked per member")
expect_err("module-dag-cycle" "M8d the cycle is named on stderr")
ccg_run(query cyc_good_v -o "${work}" --config m8cyc)
expect_rc(0 "M8d the off-cycle module is unaffected")

# ------------------------------------------------------------- M8g --
message(STATUS "bmi-e2e: M8g incremental closure (reuse all / rebuild downstream)")
# 注：BMI note 的 built/reused 计数含 std/std.compat（引擎方言决定
# 其可建性：libc++ 引擎建、msvc-stl 方言拒绝），跨引擎树的精确数字
# 不同——闭包语义的精确断言用产物 mtime（哪个接口重建了、哪个纹丝
# 未动），note 只作零/非零的前缀断言。
set(m8g "${work}/m8g")
gen_mixed("${m8g}")
ccg_run(index "${m8g}" --compile-commands "${m8g}/compile_commands.json"
        -o "${work}" --config m8g --jobs 4)
expect_rc(0 "M8g first index exit")
expect_out("failed: 0" "M8g first pass is clean")
# 二跑：无操作全复用（并行版自产缓存被并行版全量复用）。
ccg_run(index "${m8g}" --compile-commands "${m8g}/compile_commands.json"
        -o "${work}" --config m8g --jobs 4)
expect_rc(0 "M8g no-op rerun exit")
expect_out("skipped: all" "M8g fingerprint-stable no-op rerun")
expect_err("0 built," "M8g BMI cache fully reused (nothing rebuilt)")
# 改 base：恰重建 base 与其下游闭包（mid1/mid2/top），solo 复用。
# mtime 证据（秒级时间戳 + 跨秒间隔保证可分辨）：
execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 2)
file(GLOB _m8g_pcm_base "${work}/m8g/bmi-self-*/*/*/base.pcm")
file(GLOB _m8g_pcm_mid1 "${work}/m8g/bmi-self-*/*/*/mid1.pcm")
file(GLOB _m8g_pcm_mid2 "${work}/m8g/bmi-self-*/*/*/mid2.pcm")
file(GLOB _m8g_pcm_top "${work}/m8g/bmi-self-*/*/*/top.pcm")
file(GLOB _m8g_pcm_solo "${work}/m8g/bmi-self-*/*/*/solo.pcm")
foreach(_pair _m8g_pcm_base _m8g_pcm_mid1 _m8g_pcm_mid2 _m8g_pcm_top _m8g_pcm_solo)
    list(LENGTH ${_pair} _m8g_n)
    if(NOT _m8g_n EQUAL 1)
        message(FATAL_ERROR
            "bmi-e2e [M8g]: expected one ${_pair}, found ${_m8g_n}")
    endif()
endforeach()
file(TIMESTAMP "${_m8g_pcm_base}" _m8g_t_base "%Y%m%d%H%M%S" UTC)
file(TIMESTAMP "${_m8g_pcm_solo}" _m8g_t_solo "%Y%m%d%H%M%S" UTC)
file(APPEND "${m8g}/base.ixx" "\nexport inline int base_v2() { return 2; }\n")
ccg_run(index "${m8g}" --compile-commands "${m8g}/compile_commands.json"
        -o "${work}" --config m8g -v --jobs 4)
expect_rc(0 "M8g edit rerun exit")
expect_out("failed: 0" "M8g edit cascade stays clean")
# 恰闭包重建：base 与全部下游的产物被重写、solo 纹丝未动。
file(TIMESTAMP "${_m8g_pcm_base}" _m8g_t_base2 "%Y%m%d%H%M%S" UTC)
file(TIMESTAMP "${_m8g_pcm_mid1}" _m8g_t_mid1 "%Y%m%d%H%M%S" UTC)
file(TIMESTAMP "${_m8g_pcm_mid2}" _m8g_t_mid2 "%Y%m%d%H%M%S" UTC)
file(TIMESTAMP "${_m8g_pcm_top}" _m8g_t_top "%Y%m%d%H%M%S" UTC)
file(TIMESTAMP "${_m8g_pcm_solo}" _m8g_t_solo2 "%Y%m%d%H%M%S" UTC)
if(NOT (_m8g_t_base2 GREATER _m8g_t_base AND
        _m8g_t_mid1 GREATER _m8g_t_base AND
        _m8g_t_mid2 GREATER _m8g_t_base AND
        _m8g_t_top GREATER _m8g_t_base))
    message(FATAL_ERROR
        "bmi-e2e [M8g]: the downstream closure was not rebuilt "
        "(base ${_m8g_t_base}->${_m8g_t_base2}, mid1 ${_m8g_t_mid1}, "
        "mid2 ${_m8g_t_mid2}, top ${_m8g_t_top})")
endif()
if(NOT _m8g_t_solo2 EQUAL _m8g_t_solo)
    message(FATAL_ERROR
        "bmi-e2e [M8g]: the untouched interface was rebuilt "
        "(solo ${_m8g_t_solo} -> ${_m8g_t_solo2})")
endif()
ccg_run(query base_v2 -o "${work}" --config m8g)
expect_rc(0 "M8g the appended symbol is queryable")
ccg_run(query solo_v -o "${work}" --config m8g)
expect_rc(0 "M8g the untouched interface keeps its symbols")

# ------------------------------------------------------------- M8c --
message(STATUS "bmi-e2e: M8c parallel vs serial byte-identical artifacts")
set(m8c "${work}/m8c")
gen_mixed("${m8c}")
set(m8cdb "${m8c}/compile_commands.json")
# 串行门控轮（CCG_BMI_SERIAL=1）：同一任务体、主线程逐个执行。
execute_process(COMMAND ${CMAKE_COMMAND} -E env CCG_BMI_SERIAL=1
                        "${exe}" index "${m8c}" --compile-commands "${m8cdb}"
                        -o "${work}" --config m8c --full
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "M8c serial-gated index exit")
string(REGEX MATCH ": ([0-9]+) built, [0-9]+ reused" _m8c_serial_note "${err}")
if(CMAKE_MATCH_1 STREQUAL "" OR CMAKE_MATCH_1 EQUAL 0)
    message(FATAL_ERROR
        "bmi-e2e [M8c]: no BMI builds in the serial pass\n${err}")
endif()
set(_m8c_serial_built "${CMAKE_MATCH_1}")
file(GLOB _m8c_manifest "${work}/m8c/bmi-self-*/*/manifest.json")
list(LENGTH _m8c_manifest _m8c_mn)
if(NOT _m8c_mn EQUAL 1)
    message(FATAL_ERROR "bmi-e2e [M8c]: manifest not found under the cache")
endif()
file(READ "${_m8c_manifest}" _m8c_manifest_serial)
file(GLOB _m8c_pcms "${work}/m8c/bmi-self-*/*/*/*.pcm")
list(SORT _m8c_pcms)
set(_m8c_serial_blobs "")
foreach(_p ${_m8c_pcms})
    file(SHA256 "${_p}" _h)
    string(APPEND _m8c_serial_blobs "${_p}|${_h};")
endforeach()
# 并行轮：同一缓存目录再全量（唯一差异=层内并行；两轮之间源未动，
# 命令同构，manifest 键与源身份完全一致）。
ccg_run(index "${m8c}" --compile-commands "${m8cdb}"
        -o "${work}" --config m8c --full --jobs 4)
expect_rc(0 "M8c parallel index exit")
string(REGEX MATCH ": ([0-9]+) built, [0-9]+ reused" _m8c_par_note "${err}")
if(CMAKE_MATCH_1 STREQUAL "")
    message(FATAL_ERROR "bmi-e2e [M8c]: no BMI note in the parallel pass\n${err}")
endif()
if(NOT CMAKE_MATCH_1 EQUAL _m8c_serial_built)
    # 计数含 std/std.compat（引擎方言决定），但同一棵树上两轮的
    # 计数口径一致——数字必须逐字相等。
    message(FATAL_ERROR
        "bmi-e2e [M8c]: built count differs between serial-gated "
        "(${_m8c_serial_built}) and parallel (${CMAKE_MATCH_1}) passes")
endif()
file(READ "${_m8c_manifest}" _m8c_manifest_parallel)
if(NOT _m8c_manifest_parallel STREQUAL "${_m8c_manifest_serial}")
    message(FATAL_ERROR
        "bmi-e2e [M8c]: manifest.json differs between the serial-gated and "
        "parallel passes over the same cache directory")
endif()
set(_m8c_parallel_blobs "")
foreach(_p ${_m8c_pcms})
    file(SHA256 "${_p}" _h)
    string(APPEND _m8c_parallel_blobs "${_p}|${_h};")
endforeach()
if(NOT _m8c_parallel_blobs STREQUAL "${_m8c_serial_blobs}")
    message(FATAL_ERROR
        "bmi-e2e [M8c]: pcm artifacts differ byte-wise between the "
        "serial-gated and parallel passes")
endif()
# 并行轮自产缓存全量复用（并行产物自洽）。
ccg_run(index "${m8c}" --compile-commands "${m8cdb}"
        -o "${work}" --config m8c --jobs 4)
expect_rc(0 "M8c third no-flag run exit")
expect_out("skipped: all" "M8c parallel-produced cache is fully reused")
expect_err("0 built," "M8c BMI note confirms full reuse")

# ------------------------------------------------------------- M8h --
message(STATUS "bmi-e2e: M8h unified concurrency limit end to end")
# 臂 1：--jobs 2，6 宽一层（两段都吃到 2）。
set(m8h1 "${work}/m8h1")
gen_wide("${m8h1}" 6 20)
ccg_run(index "${m8h1}" --compile-commands "${m8h1}/compile_commands.json"
        -o "${work}" --config m8h1 -v --jobs 2)
expect_rc(0 "M8h flag-2 index exit")
expect_err("1 layer on up to 2 workers" "M8h flag 2 reaches the BMI segment")
expect_err("parsing 6 stale TUs on 2 workers" "M8h flag 2 reaches the parse pool")
# 臂 2：config index-jobs 3，裸跑（无旗标）。
set(m8h2 "${work}/m8h2")
gen_wide("${m8h2}" 6 20)
ccg_run(config set index-jobs 3 -o "${work}" --config m8h2)
expect_rc(0 "M8h config set 3")
ccg_run(index "${m8h2}" --compile-commands "${m8h2}/compile_commands.json"
        -o "${work}" --config m8h2 -v)
expect_rc(0 "M8h config-3 index exit")
expect_err("1 layer on up to 3 workers" "M8h config 3 reaches the BMI segment")
expect_err("parsing 6 stale TUs on 3 workers" "M8h config 3 reaches the parse pool")
# 臂 3：config 3 + --jobs 5（旗标压配置）。
set(m8h3 "${work}/m8h3")
gen_wide("${m8h3}" 6 20)
ccg_run(config set index-jobs 3 -o "${work}" --config m8h3)
expect_rc(0 "M8h config set 3 (override arm)")
ccg_run(index "${m8h3}" --compile-commands "${m8h3}/compile_commands.json"
        -o "${work}" --config m8h3 -v --jobs 5)
expect_rc(0 "M8h flag-over-config index exit")
expect_err("1 layer on up to 5 workers" "M8h the flag overrides the config (BMI)")
expect_err("parsing 6 stale TUs on 5 workers"
           "M8h the flag overrides the config (parse pool)")
# 臂 4：窄层天然封顶（6 层链 + use = 7 TU；--jobs 8 下 BMI 恒 1、解析 7）。
set(m8h4 "${work}/m8h4")
gen_deep("${m8h4}" 6 TRUE)
ccg_run(index "${m8h4}" --compile-commands "${m8h4}/compile_commands.json"
        -o "${work}" --config m8h4 -v --jobs 8)
expect_rc(0 "M8h narrow-layer index exit")
expect_err("6 layers on up to 1 worker"
           "M8h a width-1 chain caps the BMI segment at one worker")
expect_err("parsing 7 stale TUs on 7 workers"
           "M8h the parse pool takes min(8, 7)")

# ------------------------------------------------------------- M8i --
# 提速夹具（Windows 计时：pcm 的 mtime 跨度=接口 BMI 段墙钟，与预研/
# 棒二同口径，避开提交段噪声）。宽形态验收线 ≥3x、深形态 ≤1.1x
#（理论上限 8 路；深链层宽 1 收益封顶，断言的是"并行不引入可感知
# 退化"——层间屏障开销记账）。
if(NOT WIN32)
    message(STATUS "bmi-e2e: M8i skipped (pcm-mtime timing needs Windows "
                   "PowerShell; other gates above are complete)")
else()
    # 接口 pcm 的 mtime 跨度（毫秒）：bmi 缓存树下的接口产物——按名
    # 排除 std/std.compat（libc++ 方言引擎会自动建它们且发生在分层
    # 循环之前，掺进来会污染接口段的口径；msvc-stl 方言引擎不建）。
    function(pcm_span_ms out_var cache_dir)
        set(_span -1)
        execute_process(COMMAND powershell -NoProfile -Command
                "(Get-ChildItem -LiteralPath '${cache_dir}' -Recurse -Filter *.pcm | Where-Object { \$_.Name -ne 'std.pcm' -and \$_.Name -ne 'std.compat.pcm' } | Sort-Object LastWriteTimeUtc | Select-Object -First 1).LastWriteTimeUtc.Ticks"
            OUTPUT_VARIABLE _tmin RESULT_VARIABLE _rc1
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        execute_process(COMMAND powershell -NoProfile -Command
                "(Get-ChildItem -LiteralPath '${cache_dir}' -Recurse -Filter *.pcm | Where-Object { \$_.Name -ne 'std.pcm' -and \$_.Name -ne 'std.compat.pcm' } | Sort-Object LastWriteTimeUtc | Select-Object -Last 1).LastWriteTimeUtc.Ticks"
            OUTPUT_VARIABLE _tmax RESULT_VARIABLE _rc2
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(NOT _rc1 EQUAL 0 OR NOT _rc2 EQUAL 0 OR _tmin STREQUAL "" OR
                _tmax STREQUAL "")
            message(FATAL_ERROR
                "bmi-e2e [M8i]: pcm mtime probe failed under ${cache_dir}")
        endif()
        math(EXPR _span "(${_tmax} - ${_tmin}) / 10000")
        set(${out_var} "${_span}" PARENT_SCOPE)
    endfunction()

    # 宽形态：12 个互不依赖的接口，串行（--jobs 1）vs 并行（--jobs 8）。
    message(STATUS "bmi-e2e: M8i wide-shape speedup (12 independent interfaces)")
    set(m8w_ser "${work}/m8w-ser")
    set(m8w_par "${work}/m8w-par")
    gen_wide("${m8w_ser}" 12 3000)
    gen_wide("${m8w_par}" 12 3000)
    ccg_run(index "${m8w_ser}" --compile-commands
            "${m8w_ser}/compile_commands.json"
            -o "${work}" --config m8wser --jobs 1)
    expect_rc(0 "M8i wide serial index exit")
    pcm_span_ms(_wide_ser_ms "${work}/m8wser")
    ccg_run(index "${m8w_par}" --compile-commands
            "${m8w_par}/compile_commands.json"
            -o "${work}" --config m8wpar --jobs 8)
    expect_rc(0 "M8i wide parallel index exit")
    pcm_span_ms(_wide_par_ms "${work}/m8wpar")
    message(STATUS "bmi-e2e: M8i wide BMI span serial=${_wide_ser_ms}ms "
                   "parallel=${_wide_par_ms}ms")
    if(_wide_par_ms LESS 1)
        set(_wide_par_ms 1)
    endif()
    math(EXPR _wide_ratio_pct "${_wide_ser_ms} * 100 / ${_wide_par_ms}")
    if(_wide_ratio_pct LESS 300)
        message(FATAL_ERROR
            "bmi-e2e [M8i]: wide-shape speedup below the 3x acceptance line "
            "(serial ${_wide_ser_ms}ms vs parallel ${_wide_par_ms}ms = "
            "${_wide_ratio_pct}% of serial)")
    endif()

    # 深形态：24 层链，两臂 BMI 段墙钟差不得超过 10%（屏障开销记账）。
    message(STATUS "bmi-e2e: M8i deep-shape no-regression (24-layer chain)")
    set(m8d_ser "${work}/m8d-ser")
    set(m8d_par "${work}/m8d-par")
    gen_deep("${m8d_ser}" 24 FALSE)
    gen_deep("${m8d_par}" 24 FALSE)
    ccg_run(index "${m8d_ser}" --compile-commands
            "${m8d_ser}/compile_commands.json"
            -o "${work}" --config m8dser --jobs 1)
    expect_rc(0 "M8i deep serial index exit")
    pcm_span_ms(_deep_ser_ms "${work}/m8dser")
    ccg_run(index "${m8d_par}" --compile-commands
            "${m8d_par}/compile_commands.json"
            -o "${work}" --config m8dpar --jobs 8)
    expect_rc(0 "M8i deep parallel index exit")
    pcm_span_ms(_deep_par_ms "${work}/m8dpar")
    message(STATUS "bmi-e2e: M8i deep BMI span serial=${_deep_ser_ms}ms "
                   "parallel=${_deep_par_ms}ms")
    math(EXPR _deep_budget "${_deep_ser_ms} * 11 / 10 + 50")
    if(_deep_par_ms GREATER _deep_budget)
        message(FATAL_ERROR
            "bmi-e2e [M8i]: deep-shape parallel arm regressed beyond the "
            "1.1x line (serial ${_deep_ser_ms}ms vs parallel ${_deep_par_ms}ms)")
    endif()
endif()

# ------------------------------------------------------------- M8u --
# 追加指令：update 场景下排队/编译期间 .ixx 改变的三
# 形态。处理原则=依赖既有身份失配自愈语义（源身份在任务体内、构建
# 动作前一刻取；下一轮 stat/hash 必不匹配而重建），这里证明它够用。
if(NOT WIN32)
    message(STATUS "bmi-e2e: M8u background-holder orchestration is "
                   "Windows-shaped; POSIX machines skip the three probes")
else()
    # -- u1：排队期间改 ------------------------------------------------
    # 持有者正跑批 A（改 v2 触发）时，同文件再改 v3 并入队批 B。真实
    # 竞态决定批 A 走哪种正确形态：v3 落在批 A 的 TU 快照之前=当轮
    # 吸收（一批收敛，BMI 层错位留给下一进程的 load_corpus）；落在
    # 之后=队列行保留、持有者第二批以身份失配重建接口及下游闭包。
    # 两种形态的共同终点一致，断言链对时序鲁棒：排队路径生效、收敛
    # 轮修平 BMI 错位、终态全复用且产物与最终源的全量构建逐字节相等
    #（不错配固化）。厚 base 让批 A 跑进秒级，claim 窗口稳定可达。
    message(STATUS "bmi-e2e: M8u1 ixx changed while queued behind the holder")
    set(m8u1 "${work}/m8u1")
    gen_bigmixed("${m8u1}" 100000)
    ccg_run(index "${m8u1}" --compile-commands "${m8u1}/compile_commands.json"
            -o "${work}" --config m8u1)
    expect_rc(0 "M8u1 baseline index")
    set(_u1_log "${work}/m8u1-holder.log")
    set(_u1_end "${work}/m8u1-holder.end")
    set(_u1_marker "${work}/m8u1-claim.marker")
    file(REMOVE "${_u1_log}" "${_u1_end}" "${_u1_marker}")
    file(APPEND "${m8u1}/bb.ixx"
        "\nexport inline int bb_v2() { return 2; }\n")
    file(WRITE "${work}/m8u1-holder.cmd"
"@echo off
set CCG_TEST_CLAIM_MARKER=${_u1_marker}
\"${exe}\" update \"${m8u1}/bb.ixx\" --drain -v -o \"${work}\" --config m8u1 > \"${_u1_log}\" 2>&1
echo done > \"${_u1_end}\"
")
    execute_process(COMMAND powershell -NoProfile -Command
        "Start-Process -FilePath 'cmd.exe' -ArgumentList '/c','${work}/m8u1-holder.cmd' -WindowStyle Hidden")
    set(_u1_claimed FALSE)
    foreach(_i RANGE 1 100)
        if(EXISTS "${_u1_marker}")
            set(_u1_claimed TRUE)
            break()
        endif()
        execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 0.2)
    endforeach()
    if(NOT _u1_claimed)
        message(FATAL_ERROR "bmi-e2e [M8u1]: the holder never claimed")
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 1)
    # 持有者正跑批 A（厚 bb 的构建持续秒级）：同文件再改 v3，第二个
    # update 只能排队。
    file(APPEND "${m8u1}/bb.ixx"
        "\nexport inline int bb_v3() { return 3; }\n")
    ccg_run(update "${m8u1}/bb.ixx" --drain -o "${work}" --config m8u1)
    expect_rc(0 "M8u1 the queued update exits 0")
    expect_err("queued (the holder drains the queue)"
               "M8u1 the live claim sends the edit to the pending queue")
    wait_for_file("${_u1_end}" "M8u1 holder finished" 300)
    file(READ "${_u1_log}" _u1_hold)
    string(REGEX MATCHALL "refreshing 1 file" _u1_runs "${_u1_hold}")
    list(LENGTH _u1_runs _u1_run_count)
    message(STATUS "bmi-e2e: M8u1 the holder drained ${_u1_run_count} "
                   "in-batch batch(es) (either racing shape is correct)")
    # 收敛轮：吸收形态在此修平 BMI 错位（load_corpus 的身份失配重建）；
    # 失配形态已在持有者第二批重建。两种形态的收敛轮都无事可做。
    ccg_run(update --drain -o "${work}" --config m8u1)
    expect_rc(0 "M8u1 convergence update")
    expect_err("nothing to do (queue empty)" "M8u1 the queue converges empty")
    # 终态断言：全复用（错配已修平、无固化）+ v3 在库。
    ccg_run(update --drain -o "${work}" --config m8u1)
    expect_rc(0 "M8u1 settled update")
    expect_err("nothing to do (queue empty)" "M8u1 the settled queue is empty")
    expect_err("0 built," "M8u1 the settled BMI cache fully reuses")
    ccg_run(query bb_v3 -o "${work}" --config m8u1)
    expect_rc(0 "M8u1 the newest content is in the db")
    # 产物硬断言：终态 manifest 与全部 pcm == 直接用最终源内容的
    # 全量构建（同缓存目录 --full 对照，逐字节相等）。
    file(GLOB _u1_manifest "${work}/m8u1/bmi-self-*/*/manifest.json")
    file(READ "${_u1_manifest}" _u1_manifest_settled)
    file(GLOB _u1_pcms "${work}/m8u1/bmi-self-*/*/*/*.pcm")
    list(SORT _u1_pcms)
    set(_u1_blobs "")
    foreach(_p ${_u1_pcms})
        file(SHA256 "${_p}" _h)
        string(APPEND _u1_blobs "${_p}|${_h};")
    endforeach()
    ccg_run(index "${m8u1}" --compile-commands "${m8u1}/compile_commands.json"
            -o "${work}" --config m8u1 --full --jobs 4)
    expect_rc(0 "M8u1 fresh-full comparison run exit")
    file(READ "${_u1_manifest}" _u1_manifest_fresh)
    if(NOT _u1_manifest_fresh STREQUAL "${_u1_manifest_settled}")
        message(FATAL_ERROR
            "bmi-e2e [M8u1]: settled manifest differs from the fresh-full "
            "rebuild (a stale identity solidified)")
    endif()
    foreach(_p ${_u1_pcms})
        file(SHA256 "${_p}" _h)
        string(FIND "${_u1_blobs}" "${_p}|${_h};" _pos)
        if(_pos EQUAL -1)
            message(FATAL_ERROR
                "bmi-e2e [M8u1]: pcm ${_p} differs from the fresh-full rebuild")
        endif()
    endforeach()

    # -- u2：构建期间改 ------------------------------------------------
    # index 的并行 BMI 构建进行中改源：产物身份绑构建前读的源内容，
    # 撕裂读取走失败语义保旧；之后的无改动轮把失配重建掉，收敛轮与
    # --full 全量对照轮逐字节相等 = 不错配固化断言（连续两轮后库与
    # 产物等价于直接用最终源内容的全量构建）。
    message(STATUS "bmi-e2e: M8u2 ixx changed while the index is building")
    set(m8u2 "${work}/m8u2")
    gen_bigmixed("${m8u2}" 75000)
    set(_u2_log "${work}/m8u2-run.log")
    set(_u2_end "${work}/m8u2-run.end")
    set(_u2_rc "${work}/m8u2-run.rc")
    file(REMOVE "${_u2_log}" "${_u2_end}" "${_u2_rc}")
    file(WRITE "${work}/m8u2-run.cmd"
"@echo off
\"${exe}\" index \"${m8u2}\" --compile-commands \"${m8u2}/compile_commands.json\" -o \"${work}\" --config m8u2 -v --jobs 4 > \"${_u2_log}\" 2>&1
(echo rc=%errorlevel%)> \"${_u2_rc}\"
echo done > \"${_u2_end}\"
")
    execute_process(COMMAND powershell -NoProfile -Command
        "Start-Process -FilePath 'cmd.exe' -ArgumentList '/c','${work}/m8u2-run.cmd' -WindowStyle Hidden")
    # 厚 base（层 0）构建中改层 1 的 bm1：落在其 stat 之前
    # 还是之后由真实竞态决定，两种形态都必须收敛。
    execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 1)
    file(APPEND "${m8u2}/bm1.ixx"
        "\nexport inline int bm1_late() { return 7; }\n")
    wait_for_file("${_u2_end}" "M8u2 background index finished" 300)
    file(READ "${_u2_rc}" _u2_rc_text)
    if(NOT _u2_rc_text MATCHES "rc=0")
        message(FATAL_ERROR
            "bmi-e2e [M8u2]: the in-flight-edit index run failed "
            "(${_u2_rc_text}); see ${_u2_log}")
    endif()
    file(READ "${_u2_log}" _u2_round1)
    if(_u2_round1 MATCHES "failed: ([0-9]+)")
        if(NOT CMAKE_MATCH_1 EQUAL 0)
            # 撕裂读取按失败语义保旧（极窄窗口）；非零失败必须集中
            # 在被改文件自身的 TU，否则是缺陷。
            message(FATAL_ERROR
                "bmi-e2e [M8u2]: round 1 reported failed: ${CMAKE_MATCH_1} "
                "(torn reads must stay on the edited TU at most); "
                "see ${_u2_log}")
        endif()
    else()
        message(FATAL_ERROR "bmi-e2e [M8u2]: no failed count in round 1 log")
    endif()
    # 收敛轮：无改动重跑把一切失配重建掉。
    ccg_run(index "${m8u2}" --compile-commands "${m8u2}/compile_commands.json"
            -o "${work}" --config m8u2 --jobs 4)
    expect_rc(0 "M8u2 convergence rerun exit")
    expect_out("failed: 0" "M8u2 the convergence rerun is clean")
    ccg_run(query bm1_late -o "${work}" --config m8u2)
    expect_rc(0 "M8u2 the final content is in the db")
    # 无错配固化：再跑一次必须全复用。
    ccg_run(index "${m8u2}" --compile-commands "${m8u2}/compile_commands.json"
            -o "${work}" --config m8u2 --jobs 4)
    expect_rc(0 "M8u2 settled rerun exit")
    expect_out("skipped: all" "M8u2 the settled state fully reuses")
    # 全量对照轮：同缓存目录 --full = 直接用最终源内容的全量构建；
    # 与收敛态逐字节相等（manifest + 全部 pcm）。
    file(GLOB _u2_manifest "${work}/m8u2/bmi-self-*/*/manifest.json")
    file(READ "${_u2_manifest}" _u2_manifest_settled)
    file(GLOB _u2_pcms "${work}/m8u2/bmi-self-*/*/*/*.pcm")
    list(SORT _u2_pcms)
    set(_u2_blobs "")
    foreach(_p ${_u2_pcms})
        file(SHA256 "${_p}" _h)
        string(APPEND _u2_blobs "${_p}|${_h};")
    endforeach()
    ccg_run(index "${m8u2}" --compile-commands "${m8u2}/compile_commands.json"
            -o "${work}" --config m8u2 --full --jobs 4)
    expect_rc(0 "M8u2 fresh-full comparison run exit")
    file(READ "${_u2_manifest}" _u2_manifest_fresh)
    if(NOT _u2_manifest_fresh STREQUAL "${_u2_manifest_settled}")
        message(FATAL_ERROR
            "bmi-e2e [M8u2]: settled manifest differs from the fresh-full "
            "rebuild (mismatch would mean a stale identity solidified)")
    endif()
    foreach(_p ${_u2_pcms})
        file(SHA256 "${_p}" _h)
        string(FIND "${_u2_blobs}" "${_p}|${_h};" _pos)
        if(_pos EQUAL -1)
            message(FATAL_ERROR
                "bmi-e2e [M8u2]: pcm ${_p} differs between the settled "
                "state and the fresh-full rebuild")
        endif()
    endforeach()

    # -- u3：层间窗口改 ------------------------------------------------
    # 第 K 层（厚 base）构建中改第 K+1 层的接口（bm1）：bm1 构建时读
    # 到的已是新内容、身份记录正确——无需任何特殊处理，断言此形态
    # 直接正确：首跑全绿、新符号在库、无改动重跑全复用（零失配重建）。
    message(STATUS "bmi-e2e: M8u3 ixx changed across the layer window")
    set(m8u3 "${work}/m8u3")
    gen_bigmixed("${m8u3}" 300000)
    set(_u3_log "${work}/m8u3-run.log")
    set(_u3_end "${work}/m8u3-run.end")
    set(_u3_rc "${work}/m8u3-run.rc")
    file(REMOVE "${_u3_log}" "${_u3_end}" "${_u3_rc}")
    file(WRITE "${work}/m8u3-run.cmd"
"@echo off
\"${exe}\" index \"${m8u3}\" --compile-commands \"${m8u3}/compile_commands.json\" -o \"${work}\" --config m8u3 -v --jobs 4 > \"${_u3_log}\" 2>&1
(echo rc=%errorlevel%)> \"${_u3_rc}\"
echo done > \"${_u3_end}\"
")
    execute_process(COMMAND powershell -NoProfile -Command
        "Start-Process -FilePath 'cmd.exe' -ArgumentList '/c','${work}/m8u3-run.cmd' -WindowStyle Hidden")
    # 1 秒：索引仍在启动/引擎准备，厚 base（层 0）远未建完——
    # 层 1 的 bm1 离 stat 还远，改动必然先于它落盘。bb 的 repeat
    # 体量按"层 0 构建稳定跨过 1 秒编辑点"选定：75000 时层 1/层 2
    # 的 pcm 落盘时刻与编辑点仅差几毫秒到百余毫秒，编辑落点会在
    # bm1.pcm 前后两态间随机摆动（两态最终库内容都正确），而本段
    # 断言只钉死"编辑早于全部 pcm"这一形态；加厚到 300000 后余量
    # 实测拉到 2-3.8 秒，三连稳定停在单形态上。
    execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 1)
    file(APPEND "${m8u3}/bm1.ixx"
        "\nexport inline int bm1_win() { return 11; }\n")
    wait_for_file("${_u3_end}" "M8u3 background index finished" 300)
    file(READ "${_u3_rc}" _u3_rc_text)
    if(NOT _u3_rc_text MATCHES "rc=0")
        message(FATAL_ERROR
            "bmi-e2e [M8u3]: the cross-layer-edit index run failed "
            "(${_u3_rc_text}); see ${_u3_log}")
    endif()
    file(READ "${_u3_log}" _u3_round1)
    string(FIND "${_u3_round1}" "failed: 0" _u3_clean)
    if(_u3_clean EQUAL -1)
        message(FATAL_ERROR
            "bmi-e2e [M8u3]: round 1 was not clean; see ${_u3_log}")
    endif()
    ccg_run(query bm1_win -o "${work}" --config m8u3)
    expect_rc(0 "M8u3 the newer layer-1 content landed in round 1")
    # 直接正确：身份与新内容自洽，无改动重跑零重建。
    ccg_run(index "${m8u3}" --compile-commands "${m8u3}/compile_commands.json"
            -o "${work}" --config m8u3 --jobs 4)
    expect_rc(0 "M8u3 no-op rerun exit")
    expect_out("skipped: all" "M8u3 the recorded identity matches (no rebuild)")
    expect_err("0 built," "M8u3 BMI note confirms zero rebuilds")
endif()

message(STATUS "bmi-e2e: all gates passed")
