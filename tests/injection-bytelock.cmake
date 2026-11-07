# V7.1 PreToolUse 注入字节锁闸门。
#
# 从 tests/CMakeLists.txt INCLUDE 进来 -> 注册 ccg_injection_bytelock
# ctest。经 `cmake -P injection-bytelock.cmake <ccg.exe> <source-dir>
# <work-dir>` 运行。
#
# 本闸门把伪造的 Grep hook JSON 喂给真实
# `ccg hook pretooluse` 处理器，并断言 additionalContext
# 与已锁定的注入规格逐字节一致。合成语料源
# （tests/fixtures/injection/corpus/）与锁清单
# （tests/fixtures/injection/byte-lock.json）随仓库分发；
# 参考库不落库——测试期把语料拷进构建树工作目录、
# 用被测 ccg.exe 现场索引（引擎发现与 tests/e2e.cmake 的
# 索引惯例一致：无旗标，由 ccg 自行发现），hook 渲染以
# 工作目录为 cwd，display_path 因此相对化，换机零漂移。
# 渲染器或语料有意变化后按清单旁 README 的步骤再生锁
#（环境变量 CCG_BYTELOCK_REGEN=1 重跑一次 ctest 即重锁）。
# 索引失败是一等仓库测试的失败（FATAL_ERROR，不做缺席
# SKIP）；唯一 SKIP 是无 Python 解释器。

if(NOT CMAKE_SCRIPT_MODE_FILE)
    add_test(NAME ccg_injection_bytelock
             COMMAND ${CMAKE_COMMAND} -P
                     "${CMAKE_CURRENT_SOURCE_DIR}/injection-bytelock.cmake"
                     $<TARGET_FILE:ccg>
                     "${CMAKE_SOURCE_DIR}"
                     "${CMAKE_CURRENT_BINARY_DIR}/bytelock-work")
    set_tests_properties(ccg_injection_bytelock PROPERTIES
        TIMEOUT 240)
    return()
endif()

if(NOT DEFINED CMAKE_ARGV5)
    message(FATAL_ERROR
        "injection-bytelock.cmake: expected <ccg.exe> <source-dir> <work-dir>")
endif()
set(exe  "${CMAKE_ARGV3}")
set(src  "${CMAKE_ARGV4}")
set(work "${CMAKE_ARGV5}")

# 锁清单与语料源都是仓库内相对路径：缺任一即仓库损坏，
# 响亮失败而非跳过。
set(manifest "${src}/tests/fixtures/injection/byte-lock.json")
set(corpus   "${src}/tests/fixtures/injection/corpus")
if(NOT EXISTS "${manifest}")
    message(FATAL_ERROR
        "injection-bytelock.cmake: byte-lock manifest missing: ${manifest}")
endif()
if(NOT EXISTS "${corpus}/compile_commands.json")
    message(FATAL_ERROR
        "injection-bytelock.cmake: corpus sources missing: ${corpus}")
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_EXECUTABLE)
    message(STATUS
        "SKIP ccg_injection_bytelock: no Python3 interpreter found")
    return()
endif()

# 工作目录每次从头构建（构建树产物，绝不入库）：语料源
# 原样拷入，被测二进制现场索引。索引失败即闸门失败。
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}")
file(COPY "${corpus}/" DESTINATION "${work}")

execute_process(
    COMMAND "${exe}" index "${work}" -o "${work}/.ccg" --config v16
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
if(NOT rc EQUAL 0 OR NOT out MATCHES "failed: 0")
    message(FATAL_ERROR
        "injection-bytelock: fixture index failed (rc=${rc}):\n${out}\n${err}")
endif()
if(NOT EXISTS "${work}/.ccg/v16/ccg.db")
    message(FATAL_ERROR
        "injection-bytelock: index reported success but no database "
        "appeared at ${work}/.ccg/v16/ccg.db")
endif()

execute_process(
    COMMAND "${Python3_EXECUTABLE}" -I
            "${src}/tests/injection_bytelock.py"
            "${exe}" "${src}" "${work}"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR
        "injection-bytelock failed (rc=${rc}):\n${out}\n${err}")
endif()
message(STATUS "${out}")
