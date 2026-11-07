# 全模块源码树的构建辅助：每个工程源文件都是
# 模块单元（.ixx），经 CXX_MODULES 文件集编译。

# ---------------------------------------------------------------------------
# 工具集发现（可移植契约）。任何
# 构建输入都不许出现机器绝对路径。政策第一步：
# 从实际使用的编译器推导工具集布局——其 bin/ 目录的
# 父目录对任何 MSYS2 式前缀（clang64、ucrt64、
# mingw64）与标准 LLVM 布局都成立。（CMake 在 project() 期间
# 会把按 PATH 命名的编译器解析成完整路径，因此 preset 保持可移植。）
# 覆盖值（CCG_LLVM_ROOT / CCG_SQLITE3_ROOT，默认为空）在
# 3rdParty/CMakeLists.txt 中生效；设置时胜过此推导。
get_filename_component(CCG_TOOLCHAIN_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
get_filename_component(CCG_TOOLCHAIN_ROOT "${CCG_TOOLCHAIN_BIN}" DIRECTORY)
message(STATUS "ccg toolchain: compiler ${CMAKE_CXX_COMPILER}")
message(STATUS "ccg toolchain: derived root ${CCG_TOOLCHAIN_ROOT}")

# ---------------------------------------------------------------------------
# SEH 守卫语言选项（非 MSVC 的 Windows 工具集）：崩溃守卫的
# __try 写法（src/util/util.guard.ixx，一个模块单元）需要
# -fms-extensions，而该语言选项属于 C++20 模块 pcm 兼容身份的
# 一部分。因此它必须搭上构建的每个 TU——
# 绝不能只作用于按源文件或按目标的子集：若模块单元带旗标而
# CMake 合成的 @cmake_cxx_std std.cppm 不带，clang 会
# 直接拒收 std pcm（"Microsoft C++ extensions was disabled in
# precompiled file ... but is currently enabled"——合成的 std
# 目标不继承消费目标的任何按源/按目标旗标）。
# 从根目录来的目录作用域（本文件在任何
# add_subdirectory 之前被 include）覆盖每个工程、3rdParty 与测试目标以及
# 合成的 std.cppm 命令，因此整棵树在同一种
# LangOpts 形态下编译。MSVC 无需任何东西（cl 原生接受 __try）；其他
# 平台编译守卫的 POSIX 分支。被 ccg 索引的树把
# 该旗标记进每个模块 argv，ccg 的 compile-db 路线把自己构建的
# std pcm 变体精确对齐到那些记录下来的向量。
if(WIN32 AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    add_compile_options(-fms-extensions)
    message(STATUS "ccg toolchain: -fms-extensions injected build-wide "
                   "(SEH guard module unit, non-MSVC Windows)")
endif()

# ---------------------------------------------------------------------------
# MSVC 的 `import std` 供给（探针实测）：
# CMake 实验性的合成 @cmake_cxx_std 静态库能链接但
# 丢掉 STL charconv 的内联静态量（每个 std::format 消费者上
# std::_General_precision_tables 报 LNK2001/LNK2019——库符号
# 索引从未登记过它们）。可行的配方是把
# 工具集自带的 modules/std.ixx 构建成带 PUBLIC CXX_MODULES
# 文件集的 OBJECT 库：目标文件直接上链接行，没有库，没有缺口。
# 探针钉死的要求：文件集必须 PUBLIC（PRIVATE 使
# 消费者拿不到模块），BASE_DIRS 必须覆盖工具集
# 路径（默认基目录拒绝源码树外的文件），必须有一个
# 普通锚点 TU，STL 模块本身要 /utf-8，且 MSVC 路径上任何地方都不得出现
# CXX_MODULE_STD / CMAKE_EXPERIMENTAL_CXX_IMPORT_STD
# （cl 下没有任何东西设置 CXX_MODULE_STD，因此根
# CMakeLists 的 UUID 闸在这里保持惰性；clang64 路径保持不变）。
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    get_filename_component(_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(CCG_MSVC_TOOLSET_ROOT "${_cl_dir}/../../.." ABSOLUTE)
    if(NOT EXISTS "${CCG_MSVC_TOOLSET_ROOT}/modules/std.ixx")
        message(FATAL_ERROR
            "ccg: the MSVC toolset at '${CCG_MSVC_TOOLSET_ROOT}' ships no "
            "modules/std.ixx - the C++23 modules component is missing. "
            "Install the MSVC 'C++ core features' (module) component or "
            "point the build at a complete toolset.")
    endif()
    add_library(cg_std_modules OBJECT
        "${CMAKE_CURRENT_LIST_DIR}/std_modules_anchor.cpp")
    target_sources(cg_std_modules PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CCG_MSVC_TOOLSET_ROOT}/modules"
        FILES "${CCG_MSVC_TOOLSET_ROOT}/modules/std.ixx")
    target_compile_options(cg_std_modules PRIVATE /utf-8)
    message(STATUS "ccg toolchain: MSVC std module "
                   "${CCG_MSVC_TOOLSET_ROOT}/modules/std.ixx")

    # C2801 规避（机制与验证见
    # cmake/C2801ApplyPatch.cmake 的头注释）：对工具集 <filesystem> 做配置期影子补丁，
    # 只施加到本目标——它是 std BMI 的唯一生产者、
    # 该缺陷的决定性宿主。补丁无法匹配并验证时
    # 一律回落到未打补丁的头文件。
    include("${CMAKE_CURRENT_LIST_DIR}/CcgC2801Workaround.cmake")
    ccg_c2801_wire_std_module_shadow(cg_std_modules)
endif()

# 每个调用 ccg 的测试都前插进 PATH 的目录：解析出的
# libclang 的 bin 目录（由 3rdParty 设为 CCG_LIBCLANG_BIN）加上
# 编译器的 bin 目录（运行期 libc++ 等）。所有受支持的
# 布局里二者相同，因此列表通常折叠成一个条目。
function(ccg_test_path_mods out_var)
    set(_dirs "${CCG_TOOLCHAIN_BIN}")
    if(CCG_LIBCLANG_BIN)
        list(INSERT _dirs 0 "${CCG_LIBCLANG_BIN}")
    endif()
    list(REMOVE_DUPLICATES _dirs)
    set(_mods "")
    foreach(_d IN LISTS _dirs)
        list(APPEND _mods "PATH=path_list_prepend:${_d}")
    endforeach()
    # ASAN 插桩的运行：libclang 的 SEH 崩溃恢复会拦截
    # ASAN 运行期良性的首次机会访问违例（惰性影子页提交），
    # 在解析期间把它们误报成 CXError_Crashed（MSYS2
    # clang 23.1.3 + 动态运行期实测；探针证明：关掉恢复后
    # 全套件转绿，ASAN 自己的处理器仍会处理
    # 这些访问违例）。
    if(CCG_ASAN_ACTIVE)
        list(APPEND _mods "LIBCLANG_DISABLE_CRASH_RECOVERY=set:1")
    endif()
    set(${out_var} "${_mods}" PARENT_SCOPE)
endfunction()

# 供可选拷贝步骤用的工具集运行期 DLL 集：只有 ccg.exe 自身的
# 运行期依赖（libc++.dll 按实名找，带编号/后缀的
# zlib/xml/ffi/iconv 依赖按 GLOB 找——集合因工具集与
# 版本而异）。libclang 自每库一引擎政策起改为运行期加载
# （cg.clang.loader 按数据库解析引擎），因此故意不再拷贝
# libclang.dll + libLLVM-* 闭包：引擎经
# llvm-path 配置 / CCG_LLVM_ROOT / PATH 发现来解析，
# 绝不经可执行文件旁的文件。缺失条目以
# 一行 STATUS 跳过而不让构建失败。
function(ccg_toolchain_dlls out_var)
    set(_bin "${CCG_LIBCLANG_BIN}")
    if(NOT _bin)
        set(_bin "${CCG_TOOLCHAIN_BIN}")
    endif()
    set(_found "")
    foreach(_name libc++.dll zlib1.dll libzstd.dll)
        if(EXISTS "${_bin}/${_name}")
            list(APPEND _found "${_bin}/${_name}")
        else()
            message(STATUS "ccg dll copy: ${_name} not in ${_bin} (skipped)")
        endif()
    endforeach()
    foreach(_pat libxml2-*.dll libffi-*.dll libiconv-*.dll)
        file(GLOB _hits "${_bin}/${_pat}")
        if(NOT _hits)
            message(STATUS "ccg dll copy: no ${_pat} in ${_bin} (skipped)")
        endif()
        list(APPEND _found ${_hits})
    endforeach()
    set(${out_var} "${_found}" PARENT_SCOPE)
endfunction()

# 把 .ixx 模块单元作为一个 PUBLIC 的 CXX_MODULES 文件集追加到目标。
# 相对文件路径按调用方（CALLER）的当前源码目录解析，
# 因此同一个辅助从 src/ 和 tests/ 调用都不用改。
function(ccg_add_modules target base_dir)
    set(_abs_files "")
    foreach(f IN LISTS ARGN)
        if(IS_ABSOLUTE "${f}")
            list(APPEND _abs_files "${f}")
        else()
            list(APPEND _abs_files "${CMAKE_CURRENT_SOURCE_DIR}/${f}")
        endif()
    endforeach()
    target_sources(${target} PUBLIC
        FILE_SET CXX_MODULES BASE_DIRS "${base_dir}" FILES ${_abs_files})
endfunction()

# C++23 + `import std;` + 警告，对每个持有模块的目标做一次。
function(ccg_module_target target)
    # 做登记使 tidy 闸（cmake/CcgTidy.cmake）能依赖每个
    # 持有模块的目标：clang-tidy 读取这些目标构建出的 BMI。
    set_property(GLOBAL APPEND PROPERTY CCG_MODULE_TARGETS ${target})
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        # MSVC：`import std` 来自上方搭好的工具集 std.ixx OBJECT 库，
        # 不来自 CMake 的合成目标（探针结论；
        # 库形态会丢 charconv 内联静态量）。
        set_target_properties(${target} PROPERTIES CXX_MODULE_STD OFF)
        target_compile_features(${target} PUBLIC cxx_std_23)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
        target_link_libraries(${target} PRIVATE cg_std_modules)
    else()
        set_target_properties(${target} PROPERTIES CXX_MODULE_STD ON)
        target_compile_features(${target} PUBLIC cxx_std_23)
        target_compile_options(${target} PRIVATE -Wall -Wextra)
        # 此处不加 -fms-extensions：在守卫 __try 写法需要它的
        # 非 MSVC Windows 工具集上，该旗标已在
        # 目录作用域全构建注入（见本文件顶部），按目标
        # 重复是冗余——而在其他平台上
        # 它根本不许出现。
    endif()
    # 消毒器（按配置的生成器表达式；CCG_USE_ASAN 未激活时为空操作——
    # cmake/CcgAsan.cmake）。
    ccg_asan_target(${target})
endfunction()

# 登记一个纯模块的测试可执行文件：链接 cg_core、编译
# 测试骨架接口，并在测试期把工具集 bin 目录前插进 PATH，
# 使运行期 DLL（libc++.dll、libclang.dll 闭包）在 ctest 下可解析。
function(ccg_add_test name)
    add_executable(${name})
    ccg_module_target(${name})
    ccg_add_modules(${name} "${CMAKE_CURRENT_SOURCE_DIR}"
        ${ARGN} test_harness.ixx)
    target_link_libraries(${name} PRIVATE cg_core)
    if(MSVC)
        # 测试 TU 与 cg_core 同为巨型单文件模块：无优化的 Debug 配置
        # 不合并节区，模板/lambda 实例化可推过 COFF 节区上限
        # （C1128，与 cg_core 的 /bigobj 同因）；/bigobj 无语义影响。
        target_compile_options(${name} PRIVATE /bigobj)
    endif()
    add_test(NAME ${name} COMMAND ${name})
    ccg_test_path_mods(_path_mods)
    set_tests_properties(${name} PROPERTIES ENVIRONMENT_MODIFICATION
        "${_path_mods}")
endfunction()

option(CCG_COPY_RUNTIME_DLLS
    "Copy toolchain runtime DLLs beside built executables" OFF)

# 在 POST_BUILD 用 copy_if_different 把给定 DLL 拷到目标输出旁。
# 仅为可选启用的打包辅助：libclang 闭包约 194 MiB，开发构建
# 改为依赖 PATH。
function(ccg_copy_dlls_after_build)
    if(NOT CCG_COPY_RUNTIME_DLLS)
        return()
    endif()
    list(GET ARGV 0 target)
    list(SUBLIST ARGV 1 -1 dlls)   # 不要用 ${ARGV1}：ARGV1 只是单个参数
    foreach(dll IN LISTS dlls)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${dll}" "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    endforeach()
endfunction()
