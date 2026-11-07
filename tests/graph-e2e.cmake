# ccg graph e2e（doxygen-flow 波）：真实二进制把调用
# 图从 sample1 夹具库导出为 Graphviz dot 与 JSON。结构
# 断言总是跑；能发现 dot 可执行文件时导出
# 还加渲染冒烟（否则只做语法级断言——不硬依赖
# graphviz，沿用既定的可选环境模式）。
#
# 一个文件的两副面孔：
#   从 tests/CMakeLists.txt INCLUDE 进来 -> 注册 ccg_graph_e2e 这个 ctest。
#   经 `cmake -P graph-e2e.cmake <ccg.exe> <fixture-dir> <work-dir>` 运行。

if(NOT CMAKE_SCRIPT_MODE_FILE)
    add_test(NAME ccg_graph_e2e
             COMMAND ${CMAKE_COMMAND} -P
                     "${CMAKE_CURRENT_SOURCE_DIR}/graph-e2e.cmake"
                     $<TARGET_FILE:ccg>
                     "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sample1"
                     "${CMAKE_BINARY_DIR}/test-graph-e2e")
    ccg_test_path_mods(_graph_mods)
    set_tests_properties(ccg_graph_e2e PROPERTIES
        ENVIRONMENT_MODIFICATION "${_graph_mods}"
        TIMEOUT 120)
    return()
endif()

if(NOT DEFINED CMAKE_ARGV5)
    message(FATAL_ERROR "graph-e2e.cmake: expected <ccg.exe> <fixture-dir> <work-dir>")
endif()
set(exe     "${CMAKE_ARGV3}")
set(fixture "${CMAKE_ARGV4}")
set(work    "${CMAKE_ARGV5}")

# 运行/断言宏组（ccg_run/expect_rc/expect_out/expect_err）
# 收纳在同目录 ccg-harness.cmake，供各闸门脚本共享。
include("${CMAKE_CURRENT_LIST_DIR}/ccg-harness.cmake")

file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}")

ccg_run(index "${fixture}" -o "${work}" --config main)
expect_rc(0 "graph e2e index")
expect_out("failed: 0" "graph e2e index")

# --------------------------------------------------------- 符号模式 ---------
ccg_run(graph --symbol main --format dot -o "${work}" --config main)
expect_rc(0 "graph symbol dot")
expect_out("digraph ccg_call_graph {" "graph symbol dot")
expect_out("\"main\" [fillcolor=\"#ffd166\"];" "graph symbol dot")
expect_out("\"app::render_all\";" "graph symbol dot")
expect_out("\"main\" -> \"app::render_all\";" "graph symbol dot")

# 默认深度是 2；--depth 1 只保留直接一层（无 area_all）。
ccg_run(graph --symbol main --depth 1 --format dot -o "${work}" --config main)
expect_rc(0 "graph symbol depth 1")
expect_out("\"app::render_all\";" "graph symbol depth 1")
if(out MATCHES "app::area_all")
    message(FATAL_ERROR
        "graph-e2e [graph symbol depth 1]: transitive tier leaked:\n${out}")
endif()

# 同一导出的 JSON 面。
ccg_run(graph --symbol main --format json -o "${work}" --config main)
expect_rc(0 "graph symbol json")
string(SUBSTRING "${out}" 0 1 _first)
if(NOT _first STREQUAL "{")
    message(FATAL_ERROR "graph-e2e [graph symbol json]: stdout is not JSON:\n${out}")
endif()
expect_out("\"target\"" "graph symbol json")
expect_out("\"nodes\"" "graph symbol json")
expect_out("\"edges\"" "graph symbol json")
expect_out("\"truncated\":false" "graph symbol json")
expect_out("\"deeperEdgesExist\":true" "graph symbol json")

# -------------------------------------------------------- 模块模式 ---------
ccg_run(graph --modules --format dot -o "${work}" --config main)
expect_rc(0 "graph modules dot")
expect_out("digraph ccg_module_graph {" "graph modules dot")
expect_out("\"main\";" "graph modules dot")
expect_out("\"canvas\" -> \"circle\"" "graph modules dot")

# --json 隐含 json 格式（与 query 家族对齐）。
ccg_run(graph --modules --json -o "${work}" --config main)
expect_rc(0 "graph modules json")
expect_out("\"minWeight\":1" "graph modules json")
expect_out("\"edgesTotal\":" "graph modules json")

# --min-weight 修剪边/节点，但从不修剪总计。
ccg_run(graph --modules --min-weight 100 --format json -o "${work}" --config main)
expect_rc(0 "graph modules min-weight")
expect_out("\"edgeCount\":0" "graph modules min-weight")
string(REGEX MATCH "\"edgesTotal\":[0-9]+" _total "${out}")
if(_total STREQUAL "\"edgesTotal\":0")
    message(FATAL_ERROR
        "graph-e2e [graph modules min-weight]: totals were capped by the filter:\n${out}")
endif()

# ------------------------------------------------------------ --output 选项 -----
ccg_run(graph --symbol main --format dot --output "${work}/flow.dot" -o "${work}" --config main)
expect_rc(0 "graph output file")
if(NOT out STREQUAL "")
    message(FATAL_ERROR "graph-e2e [graph output file]: stdout not clean:\n${out}")
endif()
expect_err("ccg: wrote" "graph output file")
if(NOT EXISTS "${work}/flow.dot")
    message(FATAL_ERROR "graph-e2e [graph output file]: payload file missing")
endif()
file(READ "${work}/flow.dot" _dot)
string(FIND "${_dot}" "digraph ccg_call_graph {" _pos)
if(_pos EQUAL -1)
    message(FATAL_ERROR "graph-e2e [graph output file]: payload is not a dot graph:\n${_dot}")
endif()

ccg_run(graph --modules --format dot --output "${work}/modules.dot" -o "${work}" --config main)
expect_rc(0 "graph output modules file")

# ------------------------------------------------- 未命中与用法 -----------
ccg_run(graph --symbol nosuchsym -o "${work}" --config main)
expect_rc(1 "graph symbol miss")
expect_err("no symbol matching" "graph symbol miss")

ccg_run(graph -o "${work}" --config main)
expect_rc(2 "graph no mode")
ccg_run(graph --symbol a --modules -o "${work}" --config main)
expect_rc(2 "graph both modes")
ccg_run(graph --symbol a --format svg -o "${work}" --config main)
expect_rc(2 "graph bad format")
ccg_run(graph --modules --depth 2 -o "${work}" --config main)
expect_rc(2 "graph depth in modules mode")
ccg_run(help graph)
expect_rc(0 "graph help topic")
expect_out("usage: ccg graph" "graph help topic")

ccg_run(graph --symbol main -o "${work}" --config no-such)
expect_rc(1 "graph missing db")
expect_err("file not found" "graph missing db")

# ------------------------------------------------------ 渲染冒烟 -----------
# 上面的语法级断言是常开底线；能发现
# graphviz dot 时升级为渲染冒烟（可选环境，优雅跳过）。
find_program(CCG_GRAPH_DOT_EXE NAMES dot)
if(CCG_GRAPH_DOT_EXE)
    foreach(_f flow modules)
        execute_process(
            COMMAND "${CCG_GRAPH_DOT_EXE}" -Tsvg "${work}/${_f}.dot"
                    -o "${work}/${_f}.svg"
            RESULT_VARIABLE rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR
                "graph-e2e [render ${_f}]: dot rc ${rc}\n${_err}")
        endif()
        file(READ "${work}/${_f}.svg" _svg)
        string(FIND "${_svg}" "<svg" _pos)
        if(_pos EQUAL -1)
            message(FATAL_ERROR
                "graph-e2e [render ${_f}]: svg output has no svg root:\n${_svg}")
        endif()
    endforeach()
    message(STATUS "ccg_graph_e2e: graphviz render smoke passed")
else()
    message(STATUS "ccg_graph_e2e: no dot executable found - render smoke skipped")
endif()

message(STATUS "ccg_graph_e2e: all checks passed")
