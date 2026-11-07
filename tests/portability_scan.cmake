# 可移植性闸门（配置期契约）：
# 任何构建输入里都不得出现机器绝对路径。扫描
# CMakeLists 文件、presets、cmake/ 辅助与每个 src/ 模块单元，
# 查盘符绝对路径与 msys2 前缀标记。契约豁免：
# tests 模块单元（移植档案 INVENTORY.md 记载的合成路径数据）、
# tests/fixtures
#（数据）、docs（叙事史）、build/（生成）、.cpm-cache/
#（vendored）、运行史目录（未跟踪）。退出为致命（非零）并点名文件与
# 行号。
#
# 运行：cmake -P portability_scan.cmake <source-root>

if(NOT DEFINED CMAKE_ARGV3)
    message(FATAL_ERROR "portability_scan.cmake: expected <source-root>")
endif()
set(root "${CMAKE_ARGV3}")

# 不在本脚本内拼盘符路径来构建模式：盘符是
# 前面是非字母的单个字母（所以 https:// 这类 URL
# 与 word:\n 这类转义对不匹配）、后面跟
# 正斜杠或反斜杠。
set(_letters "A-Za-z")
string(CONCAT _drive_pattern
       "(^|[^" "${_letters}" "])" "[" "${_letters}" "]" ":" "[/\\\\]")

# 本脚本自身不被扫描：它点名自己要找的模式。
# 本地扩展令牌（可选）：同目录 portability_scan.local.cmake 设定
# _extra_tokens（正则或字面量，"|" 连接），该文件不入库（忽略规则
# 随本文件注释记载），供开发机登记内部代号/端口等机器专属标记。
set(_extra_tokens "__ccg_never_matches__")
include("${CMAKE_CURRENT_LIST_DIR}/portability_scan.local.cmake" OPTIONAL)

set(_inputs
    "CMakeLists.txt"
    "3rdParty/CMakeLists.txt"
    "src/CMakeLists.txt"
    "tests/CMakeLists.txt"
    "tests/e2e.cmake"
    "tests/llvm-e2e.cmake"
    "tests/module-e2e.cmake"
    "tests/libclang-discovery.cmake"
    "tests/msbuild-e2e.cmake"
    "tests/msbuild-record.cmake"
    "tests/msbuild-wrap.cmake"
    "tests/msbuild-capture.cmake"
    "tests/msbuild-gt.cmake"
    "tests/msbuild-gt/gt_gate.py"
    "tests/bmi-e2e.cmake"
    "tests/graph-e2e.cmake"
    "tests/ccg-harness.cmake"
    "tests/injection-bytelock.cmake"
    "CMakePresets.json"
    "cmake/CcgUtils.cmake"
    "cmake/CcgDeps.cmake"
    "cmake/CcgAsan.cmake"
    "cmake/CcgTidy.cmake"
    "cmake/ccg-tidy.cmake"
    "cmake/CPM.cmake"
    ".clang-tidy")
file(GLOB _flat_units
    "${root}/src/*.ixx" "${root}/src/*.cpp" "${root}/src/*.h"
    "${root}/src/*/*.ixx" "${root}/src/*/*.cpp" "${root}/src/*/*.h"
    "${root}/tests/*.c" "${root}/tests/*.cpp" "${root}/tests/*.py"
    "${root}/cmake/*.cmake"
    "${root}/scripts/*.py" "${root}/scripts/*.cmake")
foreach(_f IN LISTS _flat_units)
    cmake_path(RELATIVE_PATH _f BASE_DIRECTORY "${root}" OUTPUT_VARIABLE _rel)
    # 已在硬清单里的文件跳过，避免同一文件双扫双报。
    if(NOT _rel STREQUAL "" AND NOT _rel IN_LIST _inputs)
        list(APPEND _inputs "${_rel}")
    endif()
endforeach()

set(_violations "")
foreach(_rel IN LISTS _inputs)
    set(_abs "${root}/${_rel}")
    if(NOT EXISTS "${_abs}")
        continue()
    endif()
    file(READ "${_abs}" _content)
    # 按行拆分，报告才能点名违规行号。
    string(REPLACE "\r" "" _content "${_content}")
    string(REPLACE "\n" ";" _lines "${_content}")
    set(_line_no 0)
    foreach(_line IN LISTS _lines)
        math(EXPR _line_no "${_line_no} + 1")
        # 标记集：盘符、msys2 前缀（裸）、MSYS2
        #        ABI 风格目录 + Program Files（带尾斜杠的路径形态，
        #        所以注释里像 "clang64, ucrt64" 这样的行文提及保持合法）。
        #        机器专属令牌（内部工程代号、本地端口等）不入库——经
        #        同目录 portability_scan.local.cmake（被忽略、按需自建）
        #        以 _extra_tokens 列表注入，本机防护与仓库洁净两全。
        if(_line MATCHES "${_drive_pattern}" OR _line MATCHES "msys64"
                OR _line MATCHES "(clang64|ucrt64|mingw64)[/\\]"
                OR _line MATCHES "Program Files"
                OR _line MATCHES "llvm-project"
                OR _line MATCHES "${_extra_tokens}")
            list(APPEND _violations
                 "${_rel}:${_line_no}: ${_line}")
        endif()
    endforeach()
endforeach()

if(_violations)
    list(JOIN _violations "\n  " _report)
    message(FATAL_ERROR
        "ccg_portability_scan: machine-absolute paths in build inputs:\n"
        "  ${_report}\n"
        "Build inputs must stay machine-independent: derive toolchain "
        "locations from CMAKE_CXX_COMPILER or the CCG_LLVM_ROOT / "
        "CCG_SQLITE3_ROOT cache overrides.")
endif()

# 分析层依赖栅栏（设计定案）：只有 graph/cli 可以
# import cg.analysis。extract/db/clang 引它会倒置
# 分层方向（analysis -> db/clang），是硬失败。
file(GLOB_RECURSE _src_units "${root}/src/*.ixx")
set(_fence_violations "")
foreach(_f IN LISTS _src_units)
    cmake_path(RELATIVE_PATH _f BASE_DIRECTORY "${root}" OUTPUT_VARIABLE _rel)
    # graph/cli 可消费 analysis；analysis 单元可 import 自己的
    #    子模块。其余一律不得进。
    if(_rel MATCHES "^src/(graph|cli|analysis)/")
        continue()
    endif()
    file(READ "${_f}" _content)
    string(REPLACE "\r" "" _content "${_content}")
    string(REPLACE "\n" ";" _lines "${_content}")
    set(_line_no 0)
    foreach(_line IN LISTS _lines)
        math(EXPR _line_no "${_line_no} + 1")
        if(_line MATCHES "^[ \t]*import[ \t]+cg\\.analysis")
            list(APPEND _fence_violations "${_rel}:${_line_no}: ${_line}")
        endif()
    endforeach()
endforeach()
if(_fence_violations)
    list(JOIN _fence_violations "\n  " _fence_report)
    message(FATAL_ERROR
        "ccg_portability_scan: cg.analysis imported outside src/graph or "
        "src/cli (analysis -> db/clang is the only allowed direction):\n"
        "  ${_fence_report}")
endif()
message(STATUS "ccg_portability_scan: ${_inputs} clean")
