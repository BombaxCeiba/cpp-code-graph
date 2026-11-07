# 崩溃转储矩阵闸门的驱动脚本：以子进程逐形态触发 ccg_crashprobe
#（挂载 cg_core 里真实的 cg.util.crashdump），断言——
#   正向形态（av/so/terminate/heapcor/abort/purecall、multiav 三连、
#     MSVC 的 invarg）：恰一份转储且 >50KB。0 字节是崩溃线程原地
#     写盘的实测失败形态（栈溢出场景五连全零），必须防假阳性。
#   负向形态（fastfail）：快速失败按 Windows 设计绕过一切用户态
#     回调，不得产生转储——守护"不夸大承诺"的定界。
#   零行为承诺：环境变量未设置时崩溃退出码与不挂载逐位一致
#     （访问违例——CMake 把崩溃退出码翻译成可读串，等价断言），
#     且无任何转储。
#   不可写目录：报错进 stderr、退出码保持原异常码。
# 单形态 40 秒护栏（写盘卡死不留僵尸；护栏触发时进程被杀、转储
# 缺席会让恰一断言变红）；整个闸门 ctest 层 240 秒。
#
# 用法（由 tests/CMakeLists.txt 注册）：
#   cmake -P crashdump-matrix.cmake <probe-exe> <work-dir> <msvc:0|1>

set(probe "${CMAKE_ARGV3}")
set(work "${CMAKE_ARGV4}")
set(is_msvc "${CMAKE_ARGV5}")

if(NOT probe OR NOT work)
    message(FATAL_ERROR
        "usage: crashdump-matrix.cmake <probe-exe> <work-dir> <msvc:0|1>")
endif()

file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}")

# 跑一个形态；rc_stderr/rc_raw 供后续断言读取。
macro(run_form form)
    set(form_dir "${work}/${form}")
    file(REMOVE_RECURSE "${form_dir}")
    set(ENV{CCG_CRASH_DUMP_DIR} "${form_dir}")
    execute_process(
        COMMAND "${probe}" "${form}"
        RESULT_VARIABLE rc
        TIMEOUT 40
        OUTPUT_VARIABLE rc_stdout
        ERROR_VARIABLE rc_stderr)
endmacro()

# 正向断言：恰一份转储 + 非平凡体积。
macro(expect_one_dump form)
    if(rc EQUAL 0)
        message(FATAL_ERROR
            "ccg_crashdump_matrix: form ${form}: probe returned "
            "without crashing (rc=0)")
    endif()
    file(GLOB found "${form_dir}/*.dmp")
    list(LENGTH found count)
    if(NOT count EQUAL 1)
        message(FATAL_ERROR
            "ccg_crashdump_matrix: form ${form}: expected exactly 1 "
            "dump, got ${count} (rc=${rc}) stderr: ${rc_stderr}")
    endif()
    list(GET found 0 dump_file)
    file(SIZE "${dump_file}" dump_bytes)
    if(dump_bytes LESS 51200)
        message(FATAL_ERROR
            "ccg_crashdump_matrix: form ${form}: dump is only "
            "${dump_bytes} bytes (<50KB, the zero/short-dump failure "
            "shape)")
    endif()
    message(STATUS "crashdump ${form}: rc=${rc} dump=${dump_bytes} bytes")
endmacro()

# ---- 正向形态 ----
set(positive_forms av so terminate heapcor abort purecall)
if(is_msvc)
    list(APPEND positive_forms invarg)
endif()
foreach(form IN LISTS positive_forms)
    run_form(${form})
    expect_one_dump(${form})
endforeach()

# ---- 并发崩溃：三连，每连恰一份（单写者闸）----
foreach(run RANGE 1 3)
    run_form(multiav)
    if(rc EQUAL 0)
        message(FATAL_ERROR
            "ccg_crashdump_matrix: multiav run ${run}: probe returned "
            "without crashing (rc=0)")
    endif()
    file(GLOB found "${form_dir}/*.dmp")
    list(LENGTH found count)
    if(NOT count EQUAL 1)
        message(FATAL_ERROR
            "ccg_crashdump_matrix: multiav run ${run}: expected "
            "exactly 1 dump (single-writer gate), got ${count}")
    endif()
    list(GET found 0 dump_file)
    file(SIZE "${dump_file}" dump_bytes)
    if(dump_bytes LESS 51200)
        message(FATAL_ERROR
            "ccg_crashdump_matrix: multiav run ${run}: dump is only "
            "${dump_bytes} bytes")
    endif()
    message(STATUS
        "crashdump multiav run ${run}: exactly 1 dump, ${dump_bytes} bytes")
endforeach()

# ---- 负向形态：快速失败不可拦（守护定界，不夸大承诺）----
run_form(fastfail)
if(rc EQUAL 0)
    message(FATAL_ERROR
        "ccg_crashdump_matrix: form fastfail: probe returned without "
        "crashing (rc=0)")
endif()
file(GLOB found "${form_dir}/*.dmp")
list(LENGTH found count)
if(NOT count EQUAL 0)
    message(FATAL_ERROR
        "ccg_crashdump_matrix: form fastfail: fail-fast must NOT be "
        "caught by user mode (over-promise), found ${count} dump(s)")
endif()
message(STATUS "crashdump fastfail: no dump (as documented), rc=${rc}")

# ---- 零行为承诺：环境变量未设置时崩溃逐位如旧 ----
# CMake 把 0xC0000005 崩溃退出码翻译成 "Access violation" 可读串，
# 与原异常码一一对应——断言串等价于断言退出码未被吞改。
set(ENV{CCG_CRASH_DUMP_DIR} "")
set(noenv_dir "${work}/noenv")
file(REMOVE_RECURSE "${noenv_dir}")
execute_process(
    COMMAND "${probe}" av
    RESULT_VARIABLE rc
    TIMEOUT 40
    OUTPUT_VARIABLE rc_stdout
    ERROR_VARIABLE rc_stderr)
if(NOT rc STREQUAL "Access violation")
    message(FATAL_ERROR
        "ccg_crashdump_matrix: noenv-av: exit must stay the untouched "
        "access-violation code when the variable is unset, got: ${rc}")
endif()
file(GLOB found "${noenv_dir}/*.dmp")
list(LENGTH found count)
if(NOT count EQUAL 0)
    message(FATAL_ERROR
        "ccg_crashdump_matrix: noenv-av: no dump may be written when "
        "the variable is unset, found ${count}")
endif()
message(STATUS "crashdump noenv-av: untouched access-violation exit, no dump")

# ---- 不可写目录：报错进 stderr、退出码保持原异常码 ----
file(WRITE "${work}/not-a-dir" "this is a regular file, not a directory")
set(ENV{CCG_CRASH_DUMP_DIR} "${work}/not-a-dir/sub")
execute_process(
    COMMAND "${probe}" av
    RESULT_VARIABLE rc
    TIMEOUT 40
    OUTPUT_VARIABLE rc_stdout
    ERROR_VARIABLE rc_stderr)
if(NOT rc STREQUAL "Access violation")
    message(FATAL_ERROR
        "ccg_crashdump_matrix: bad-dir: exit must keep the original "
        "exception code, got: ${rc}")
endif()
if(NOT rc_stderr MATCHES "crash dump CreateFile failed")
    message(FATAL_ERROR
        "ccg_crashdump_matrix: bad-dir: expected a gle diagnostic on "
        "stderr, got: ${rc_stderr}")
endif()
message(STATUS "crashdump bad-dir: original exit kept, diagnostic on stderr")
