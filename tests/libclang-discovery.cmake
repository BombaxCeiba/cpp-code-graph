# cmake/CcgDeps.cmake 的 clang-c 头文件查找发现顺序闸门
#（MSVC 移植期）：非法 CCG_LLVM_ROOT
# 覆盖必须点名覆盖变量大声失败（绝不静默
# 滑向另一个引擎或前缀），合法覆盖必须
# 对主构建所用的同一发现代码干净配置。
# 自含：配置两个包含真实 cmake/CcgDeps.cmake 的
# 微工程；无网络、无夹具。
#
# 一个文件的两副面孔（llvm-e2e.cmake 模式）：
#   从 tests/CMakeLists.txt INCLUDE 进来 -> 注册 ctest
#   经 cmake -P libclang-discovery.cmake <source-root> <work-dir>
#     <valid-llvm-root-or-dash> <cxx-compiler> 运行

if(NOT CMAKE_SCRIPT_MODE_FILE)
    add_test(NAME ccg_libclang_discovery
             COMMAND ${CMAKE_COMMAND} -P
                     "${CMAKE_CURRENT_SOURCE_DIR}/libclang-discovery.cmake"
                     "${CMAKE_SOURCE_DIR}"
                     "${CMAKE_BINARY_DIR}/test-libclang-discovery"
                     "$<IF:$<BOOL:${CCG_LLVM_ROOT}>,${CCG_LLVM_ROOT},->"
                     "${CMAKE_CXX_COMPILER}")
    return()
endif()

if(NOT DEFINED CMAKE_ARGV5)
    message(FATAL_ERROR "libclang-discovery.cmake: expected <source-root> <work-dir> <valid-root> <cxx>")
endif()
set(src_root "${CMAKE_ARGV3}")
set(work_dir "${CMAKE_ARGV4}")
set(valid_root "${CMAKE_ARGV5}")
set(cxx_compiler "${CMAKE_ARGV6}")

file(REMOVE_RECURSE "${work_dir}")
file(MAKE_DIRECTORY "${work_dir}/bogus" "${work_dir}/valid")

# 微工程用缓存里落下的任何
# CCG_LLVM_ROOT 练真实发现函数。
file(WRITE "${work_dir}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.28)
project(libclang_discovery_probe LANGUAGES CXX)
# 编译器推导根对本探针无意义（模拟 cl）：
# 把它强指到一个没有 clang-c 头的地方。
set(CCG_TOOLCHAIN_ROOT "${CMAKE_BINARY_DIR}")
include("@src_root@/cmake/CcgDeps.cmake")
_config_libclang()
message(STATUS "probe resolved headers: ${LIBCLANG_INCLUDE_DIR}")
]=])
configure_file("${work_dir}/CMakeLists.txt" "${work_dir}/CMakeLists.txt" @ONLY)

# 案例 1：非法覆盖 -> 点名 CCG_LLVM_ROOT 的 FATAL。
execute_process(
    COMMAND "${CMAKE_COMMAND}" -G "Ninja" -S "${work_dir}" -B "${work_dir}/bogus"
            -DCMAKE_CXX_COMPILER=${cxx_compiler}
            -DCCG_LLVM_ROOT=${work_dir}/no-such-llvm
    OUTPUT_QUIET
    ERROR_VARIABLE bogus_err
    RESULT_VARIABLE bogus_rc)
if(NOT bogus_rc EQUAL 0 AND bogus_err MATCHES "CCG_LLVM_ROOT")
    message(STATUS "bogus override: refused naming CCG_LLVM_ROOT (rc=${bogus_rc})")
else()
    message(FATAL_ERROR
        "ccg_libclang_discovery: a bogus CCG_LLVM_ROOT did not produce the "
        "actionable FATAL_ERROR naming CCG_LLVM_ROOT (rc=${bogus_rc}, "
        "stderr tail: ${bogus_err})")
endif()

# 案例 2：合法覆盖（主构建自己解析出的根——当它来自
# 一个时；clang64 开发树改从编译器推导且
# 推导路径被其余每个引擎测试覆盖）-> 干净解析。
if(valid_root STREQUAL "" OR valid_root STREQUAL "-")
    message(STATUS "valid-override case skipped: main build derived its root from the compiler")
    message(STATUS "CCG_LIBCLANG_DISCOVERY OK")
    return()
endif()
# cl TryCompile 热身：满编 ctest 群里的冷 mspdbsrv 曾在
# 本探针抵达发现之前弄败配置期编译器测试
#（D8050）——先编一个小 TU，失败若
# 仍发生则归环境而非闸门。尽力而为。
get_filename_component(cxx_base "${cxx_compiler}" NAME_WE)
string(TOLOWER "${cxx_base}" cxx_base)
if(cxx_base MATCHES "^(cl|clang-cl)$")
    file(WRITE "${work_dir}/warmup.cpp" "int main(){return 0;}\n")
    execute_process(
        COMMAND "${cxx_compiler}" /nologo /c "${work_dir}/warmup.cpp"
                /Fo"${work_dir}/warmup.obj" /Fd"${work_dir}/warmup.pdb"
        OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE warmup_rc)
    message(STATUS "cl warm-up rc=${warmup_rc} (best effort)")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" -G "Ninja" -S "${work_dir}" -B "${work_dir}/valid"
            -DCMAKE_CXX_COMPILER=${cxx_compiler}
            -DCCG_LLVM_ROOT=${valid_root}
    OUTPUT_VARIABLE valid_out
    ERROR_VARIABLE valid_err
    RESULT_VARIABLE valid_rc)
if(NOT valid_rc EQUAL 0 OR NOT valid_out MATCHES "probe resolved headers:")
    if(valid_err MATCHES "CMakeTestCXXCompiler|D8050|mspdbsrv")
        message(STATUS
            "NOTE: the compiler TryCompile itself failed before discovery "
            "ran (cold mspdbsrv under a parallel ctest swarm is the known "
            "shape) - rerun this test alone before treating it as a "
            "discovery regression")
    endif()
    message(FATAL_ERROR
        "ccg_libclang_discovery: the valid CCG_LLVM_ROOT '${valid_root}' did "
        "not resolve cleanly (rc=${valid_rc}, out=${valid_out}, err=${valid_err})")
endif()
message(STATUS "valid override: headers resolved through CCG_LLVM_ROOT")
message(STATUS "CCG_LIBCLANG_DISCOVERY OK")
