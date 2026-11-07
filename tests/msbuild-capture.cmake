# ccg_msbuild_capture（审计修复）：发现辅助器
# run_capture 必须并发排空两条管道。用 `--cl <洪水桩>`
# 驱动真实 ccg.exe 让第一次 run_capture（覆盖的
# cl 横幅探针）面对先灌 stderr 后 stdout 的子进程
#（运行显式 --record——默认通道已是工程求值、不再拉起
# MSBuild 子进程，本闸门测的就是子进程管道排空）：
# 先 stdout 后 stderr 的顺序排空锁死两个进程、运行
# 永不返回；并发排空完成且运行继续（被
# 包装的 Rebuild 以桩为"真 cl"什么都不编译、MSBuild
# 失败、捕获的调用仍翻译——文档记载的诚实
# 契约——且 --export-only 以 0 退出并带 compile_commands.json）。
#
# 一个文件的两副面孔：
#   从 tests/CMakeLists.txt 注册 -> ccg_msbuild_capture ctest。
#   经 `cmake -P msbuild-capture.cmake <ccg.exe> <floodstub.exe>
#   <fixture-dir> <work-dir>` 运行。
#
# 发现不了 Visual Studio / MSBuild 时 SKIPPED
#（ctest SKIP_REGULAR_EXPRESSION 让无 VS 的 CI 保持绿）。

if(NOT DEFINED CMAKE_ARGV6)
    message(FATAL_ERROR
        "msbuild-capture.cmake: expected <ccg.exe> <floodstub.exe> "
        "<fixture-dir> <work-dir>")
endif()
set(exe      "${CMAKE_ARGV3}")
set(stub     "${CMAKE_ARGV4}")
set(fixture  "${CMAKE_ARGV5}")
set(work     "${CMAKE_ARGV6}")

# VS 存在性（跳过闸门），解析方式同 msbuild-e2e.cmake：
# 经 `cmake -E environment` 拿 ProgramFiles(x86)、vswhere -find MSBuild。
set(_pf86 "")
execute_process(COMMAND "${CMAKE_COMMAND}" -E environment
    OUTPUT_VARIABLE _env_block RESULT_VARIABLE _env_rc)
if(_env_rc EQUAL 0)
    string(PREPEND _env_block "\n")
    string(FIND "${_env_block}" "\nProgramFiles(x86)=" _at)
    if(_at GREATER -1)
        math(EXPR _val_at "${_at} + 19")
        string(SUBSTRING "${_env_block}" ${_val_at} -1 _tail)
        string(REGEX REPLACE "^([^\r\n]*).*" "\\1" _pf86 "${_tail}")
    endif()
endif()
set(_vswhere "")
if(NOT _pf86 STREQUAL "")
    set(_vswhere "${_pf86}/Microsoft Visual Studio/Installer/vswhere.exe")
endif()
set(_msbuild "")
if(NOT _vswhere STREQUAL "" AND EXISTS "${_vswhere}")
    execute_process(COMMAND "${_vswhere}" -latest -products *
                    -requires Microsoft.Component.MSBuild
                    -find MSBuild/**/Bin/MSBuild.exe
        RESULT_VARIABLE _vw_rc
        OUTPUT_VARIABLE _vw_out
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_vw_rc EQUAL 0)
        set(_msbuild "${_vw_out}")
    endif()
endif()
if(_msbuild STREQUAL "")
    message(STATUS
        "*** SKIPPED: no Visual Studio / MSBuild found - "
        "ccg_msbuild_capture needs it")
    return()
endif()

file(REMOVE_RECURSE "${work}")
file(COPY "${fixture}/incpath" DESTINATION "${work}")

# TIMEOUT 是死锁绊网：回归的顺序排空在此挂起
#（桩阻塞在写 stderr、ccg 阻塞在 ReadFile 读 stdout）。
execute_process(COMMAND "${exe}" msbuild
                        "${work}/incpath/incpath.sln"
                        --config Debug --platform x64
                        --msbuild "${_msbuild}"
                        --cl "${stub}"
                        --export-only --record
    TIMEOUT 120
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR
        "msbuild-capture [drain]: exit code ${rc} (deadlock or run "
        "failure)\n--stdout--\n${out}\n--stderr--\n${err}")
endif()
string(FIND "${out}" "compile commands" _pos)
if(_pos EQUAL -1)
    message(FATAL_ERROR
        "msbuild-capture [export]: stdout missing 'compile commands'\n"
        "--stdout--\n${out}\n--stderr--\n${err}")
endif()
if(NOT EXISTS "${work}/incpath/.ccg/Debug-x64/compile_commands.json")
    message(FATAL_ERROR "msbuild-capture [export]: compile_commands.json "
        "was not written")
endif()
message(STATUS "ccg_msbuild_capture: concurrent drain gate passed")
