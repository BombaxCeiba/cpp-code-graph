# cpp-code-graph 的 clang-tidy 接线（cmake/CcgAsan.cmake 的
# 质量开关兄弟件；option 惯用法参考同类工程的 use-asan 惯用法改写）。
# 检查集放在源码根的 .clang-tidy（参考同类工程配置改写；仅
# HeaderFilterRegex 适配了本源码树）。
#
# 一个始终运行的自定义目标：
#   cmake --build --preset clang-dev --target tidy
# 经构建树的 compile_commands.json 检查每个工程模块单元
# （src/**/*.ixx、tests/*.ixx）。模块单元 import std 且互相
# import，因此 clang-tidy 必须读构建产出的 BMI；该目标因而
# 依赖每个持有模块的目标（由 ccg_module_target 以全局属性
# CCG_MODULE_TARGETS 登记）并先重建它们。

# 二进制发现（工具链政策，取
# cmake/CcgAsan.cmake 的 ASAN 运行库变体同款手法）：编译器自己的
# bin 目录优先，然后默认搜索，最后把 CCG_LLVM_ROOT 覆盖值的
# bin 目录作附加提示。编译器前缀永远胜过覆盖值，因为 BMI 有
# 版本锁：来自另一个 LLVM 构建的 clang-tidy 读不了本编译器的
# .pcm 文件。在缓存里预置 CCG_CLANG_TIDY_EXECUTABLE 可整体
# 跳过搜索。
#
# MSVC preset：编译器前缀里没有 clang-tidy，且闸门反正读不了
# .ifc BMI，所以发现过程从不让 configure 失败——回退顺序为
# CCG_LLVM_ROOT/bin -> VS 自带 clang 工具（由 vswhere 推导，
# 绝非机器字面量）-> 带提示跳过（tidy 目标退化为一条 no-op
# 消息）。
set(_ccg_tidy_hints "${CCG_TOOLCHAIN_BIN}")
if(CCG_LLVM_ROOT)
    list(APPEND _ccg_tidy_hints "${CCG_LLVM_ROOT}/bin")
endif()
find_program(CCG_CLANG_TIDY_EXECUTABLE NAMES clang-tidy
    HINTS ${_ccg_tidy_hints})
set(_ccg_tidy_stub OFF)
if(NOT CCG_CLANG_TIDY_EXECUTABLE AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    # 经 vswhere 推导的 VS 自带 clang-tidy（clang-cl 组件工具）。
    set(_pf_x86_name "ProgramFiles(x86)")
    find_program(CCG_VSWHERE_EXECUTABLE NAMES vswhere
        HINTS "$ENV{${_pf_x86_name}}/Microsoft Visual Studio/Installer")
    if(CCG_VSWHERE_EXECUTABLE)
        execute_process(
            COMMAND "${CCG_VSWHERE_EXECUTABLE}" -latest -products *
                    -property installationPath
            OUTPUT_VARIABLE _vs_root
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(_vs_root)
            file(GLOB _vs_tidy "${_vs_root}/VC/Tools/Llvm/*/bin/clang-tidy.exe")
            list(GET _vs_tidy 0 CCG_CLANG_TIDY_EXECUTABLE)
        endif()
    endif()
    if(NOT CCG_CLANG_TIDY_EXECUTABLE)
        # 优雅退化（MSVC 下绝不致命）：见下方 stub。
        set(CCG_CLANG_TIDY_EXECUTABLE "" CACHE FILEPATH
            "clang-tidy executable (empty = tidy gate skipped)")
        set(_ccg_tidy_stub ON)
    endif()
endif()
if(NOT CCG_CLANG_TIDY_EXECUTABLE AND NOT _ccg_tidy_stub)
    message(FATAL_ERROR
        "ccg tidy: clang-tidy not found beside the compiler "
        "('${CCG_TOOLCHAIN_BIN}'), through the default search, or in the "
        "CCG_LLVM_ROOT override's bin dir. Set CCG_CLANG_TIDY_EXECUTABLE to "
        "the clang-tidy matching the compiler '${CMAKE_CXX_COMPILER}'.")
endif()
if(_ccg_tidy_stub)
    message(STATUS "ccg tidy: no clang-tidy found for the MSVC preset "
                   "(looked in CCG_LLVM_ROOT/bin and the VS-bundled clang "
                   "tools) - the tidy target is a documented no-op here")
else()
    message(STATUS "ccg tidy: ${CCG_CLANG_TIDY_EXECUTABLE}")
endif()

# 运行按设计是串行的：本工具链的 clang-tidy（23.1.3）没有原生
# -j 工作进程选项（探针实测：`-j 4` -> "Did you mean '-h'?"），
# 并行化在此并不平凡；随附的 run-clang-tidy 包装器会给闸门
# 加一个 python 依赖。待工具链的 clang-tidy 长出 worker 支持
# 时再重审。

# 登记 tidy 闸门。只调用一次，且在每个持有模块的目标都存在
# 之后（顶层 CMakeLists 底部），使依赖清单完整。
function(ccg_add_tidy_target)
    get_property(_deps GLOBAL PROPERTY CCG_MODULE_TARGETS)
    if(NOT CCG_CLANG_TIDY_EXECUTABLE)
        add_custom_target(tidy
            COMMAND "${CMAKE_COMMAND}" -E echo
                    "ccg tidy: skipped - no clang-tidy for this MSVC preset (see configure log)"
            VERBATIM
            COMMENT "ccg tidy: skipped (no clang-tidy)")
        return()
    endif()
    add_custom_target(tidy
        COMMAND "${CMAKE_COMMAND}"
                -DTIDY_EXE=${CCG_CLANG_TIDY_EXECUTABLE}
                -DBINARY_DIR=${CMAKE_BINARY_DIR}
                -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
                -P "${CMAKE_SOURCE_DIR}/cmake/ccg-tidy.cmake"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        DEPENDS ${_deps}
        VERBATIM
        COMMENT "ccg tidy: clang-tidy over project module units")
endfunction()
