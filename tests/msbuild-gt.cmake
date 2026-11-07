# MSBuild 金标六维对账门禁（tests/msbuild-gt/ 的 ctest 壳）。
#
# 从 tests/CMakeLists.txt INCLUDE 进来 -> 注册 ccg_msbuild_gt
# ctest。经 `cmake -P msbuild-gt.cmake <gate-dir> <work-dir>` 运行：
# 对入库语料（仓内中性夹具的本体产物，见 tests/msbuild-gt/corpus/
# 与 README.md 的语料来源说明）执行金标（工程求值产物）与 ccg
# 记录通道产物（compile_commands.json + pch.json）的六维对账
#（TU 覆盖 / defines / include / PCH / 工具集 / 杂项），
# 实测数字与入库基线（baseline-six-dims.json）逐值锁死，
# 语料与引擎内容指纹（fingerprints.json）负责漂移分罪：
#   退出码 3 = 语料指纹全等但数字漂移（ccg 回归嫌疑，变好也红）；
#   退出码 4 = 语料/引擎指纹漂移（语料重采待重审，不算回归）。
# 基线与指纹文件成对双写：改基线而不重锁指纹会被内容指纹
# 当场抓获（防基线静默篡改）。
#
# 模块开关维度（/ifcOutput 等六项带值开关）已按带值开关成对保留修复后
# 的成对带值形态锁定（翻译层成对保留、消费端成对剥除、引擎
# argv 逐字节不变），裸拼写回潮即红；脚本内 XFAIL 判读机构
# 保留，供其他维度将来按已知缺陷/已修复两种形态登记。
#
# 门禁本体是纯数据对账（语料三件以文本入库，全部路径洗涤为
# 占位假根），不运行 MSBuild、无需工具链在席；
# 无 Python 解释器时打印 *** SKIPPED 标记（CTest 的
# SKIP_REGULAR_EXPRESSION 保持无解释器机器绿，
# 与 ccg_msbuild_e2e 的跳过语义一致）。
if(NOT CMAKE_SCRIPT_MODE_FILE)
    add_test(NAME ccg_msbuild_gt
             COMMAND ${CMAKE_COMMAND} -P
                     "${CMAKE_CURRENT_SOURCE_DIR}/msbuild-gt.cmake"
                     "${CMAKE_CURRENT_SOURCE_DIR}/msbuild-gt"
                     "${CMAKE_CURRENT_BINARY_DIR}/msbuild-gt-work")
    set_tests_properties(ccg_msbuild_gt PROPERTIES
        SKIP_REGULAR_EXPRESSION "\\*\\*\\* SKIPPED"
        TIMEOUT 300)
    return()
endif()

if(NOT DEFINED CMAKE_ARGV3 OR NOT DEFINED CMAKE_ARGV4)
    message(FATAL_ERROR
        "msbuild-gt.cmake: expected <gate-dir> <work-dir>")
endif()
set(gate_dir "${CMAKE_ARGV3}")
set(work_dir "${CMAKE_ARGV4}")

foreach(piece IN ITEMS gt_gate.py corpus/groundtruth.json
                      corpus/compile_commands.json corpus/pch.json
                      baseline-six-dims.json fingerprints.json)
    if(NOT EXISTS "${gate_dir}/${piece}")
        message(FATAL_ERROR
            "msbuild-gt.cmake: gate piece missing: ${gate_dir}/${piece}")
    endif()
endforeach()

find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_EXECUTABLE)
    message(STATUS "*** SKIPPED ccg_msbuild_gt: no Python3 interpreter found")
    return()
endif()

# 报告落盘进构建树（实测指标的完整快照，供失败排查）。
file(MAKE_DIRECTORY "${work_dir}")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" -I
            "${gate_dir}/gt_gate.py"
            --corpus "${gate_dir}/corpus"
            --baseline "${gate_dir}/baseline-six-dims.json"
            --fingerprints "${gate_dir}/fingerprints.json"
            --report "${work_dir}/gt-report.json"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR
        "msbuild-gt gate failed (rc=${rc}; 3=six-dim drift=regression "
        "suspect, 4=corpus/engine fingerprint drift=review needed):\n"
        "${out}\n${err}")
endif()
message(STATUS "${out}")
