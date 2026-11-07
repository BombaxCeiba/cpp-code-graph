# cpp-code-graph 的系统工具集依赖发现（工具链
# 可移植发现政策的实现），由 3rdParty/CMakeLists.txt include。从
# 3rdParty 清单拆出，使发现契约（及其回退顺序测试
# tests/libclang-discovery.cmake）能在微型项目里对着真实
# 函数演练，而不拖入 CPM 依赖图。

# 每个系统工具集依赖的发现政策（任何机器绝对
# 路径都不许被固化进来）：
#   sqlite3——默认自行拉取：固定的 sqlite.org 合并版在
#      每个工具集上都以静态库形式在树内构建（按上游工程惯例
#      的自包含配方；sqlite 移植探针实测）。
#      设置 CCG_SQLITE3_ROOT 缓存覆盖仍可选一个系统安装
#      （include/ + lib/）——那时只搜该前缀；
#   libclang 头文件——1. CCG_LLVM_ROOT 覆盖（只搜它），
#      2. 从当前编译器推导的前缀（CCG_TOOLCHAIN_ROOT，
#         在 cmake/CcgUtils.cmake 中计算——对 MSYS2 clang
#         前缀与标准 LLVM 布局有意义，在 cl.exe 下无意义），
#      3. 默认 find_* 搜索（能兜住常规 LLVM 安装），
#      4. 仍一无所获 -> FATAL_ERROR 并点名覆盖变量。
# CG_SQLITE3_ROOT / CG_LIBCLANG_ROOT 作为弃用别名继续可用，使
# 可移植化之前的构建缓存活过重新生成。
#
# 运行期 libclang.dll 本身绝不是链接依赖（经
# cg.clang.loader 在运行期加载）；测试通过
# CCG_LIBCLANG_BIN 缓存条目得知它的 bin 目录。

function(_ccg_override_root out_var new_cache deprecated_cache)
    # ${out_var} 里的生效覆盖前缀：新缓存变量已设置
    # 用它，否则用弃用别名，否则为 ""。
    set(_root "${${new_cache}}")
    if(_root STREQUAL "" AND NOT "${${deprecated_cache}}" STREQUAL "")
        set(_root "${${deprecated_cache}}")
        message(DEPRECATION
            "${deprecated_cache} is deprecated; use ${new_cache}")
    endif()
    set(${out_var} "${_root}" PARENT_SCOPE)
endfunction()

function(_config_sqlite3)
    # sqlite3，静态链接（用户决定）。两个来源，由覆盖值分流：
    #   CCG_SQLITE3_ROOT 未设置（默认）：自行拉取固定的 sqlite.org
    #     合并版（MSVC cl 与 MSYS2 clang64 均探针验证）并在
    #     树内构建 sqlite3.c——每个工具集同一份源码，任何地方
    #     都不与机器耦合。合并版 zip 自带可直接编译的 sqlite3.c +
    #     sqlite3.h（GitHub 源码快照则需要
    #     tclsh + msys-make 链条，在 MSVC 上无法用 CMake 构建）。
    #   CCG_SQLITE3_ROOT 已设置：系统安装发现，只搜该前缀。
    # 无论哪条路，SQLite3_* 缓存条目都会被预先播种，使 sqlpp23 自己的
    # find_package(SQLite3) 解析到同一个 sqlite（直接点名
    # 原始库文件胜过 find 模块在 MinGW 式
    # 工具集上的 DLL 导入偏好；探针实测）。
    set(CCG_SQLITE3_ROOT "" CACHE PATH
        "system sqlite3 prefix override (holds include/ and lib/); empty (default) = self-fetched pinned amalgamation")
    set(CG_SQLITE3_ROOT "" CACHE PATH "deprecated alias of CCG_SQLITE3_ROOT")
    _ccg_override_root(_root CCG_SQLITE3_ROOT CG_SQLITE3_ROOT)

    if(NOT _root STREQUAL "")
        find_path(SQLITE3_INCLUDE_DIR NAMES sqlite3.h
                  PATHS "${_root}/include" NO_DEFAULT_PATH)
        # 两种库存档拼法都认：MinGW 式 libsqlite3.a 与 MSVC 的
        # sqlite3.lib。
        find_library(SQLITE3_STATIC_LIBRARY NAMES libsqlite3.a sqlite3
                     PATHS "${_root}/lib" NO_DEFAULT_PATH)
        if(NOT SQLITE3_INCLUDE_DIR OR NOT SQLITE3_STATIC_LIBRARY)
            message(FATAL_ERROR
                "ccg: sqlite3 not found under the CCG_SQLITE3_ROOT override "
                "'${_root}' (looked for include/sqlite3.h and "
                "lib/libsqlite3.a or lib/sqlite3.lib). Fix the override or "
                "unset it to build the pinned amalgamation in-tree.")
        endif()
        add_library(cg_sqlite3 INTERFACE)
        add_library(cg::sqlite3 ALIAS cg_sqlite3)
        target_include_directories(cg_sqlite3 INTERFACE "${SQLITE3_INCLUDE_DIR}")
        target_link_libraries(cg_sqlite3 INTERFACE "${SQLITE3_STATIC_LIBRARY}")
        set(SQLite3_INCLUDE_DIR "${SQLITE3_INCLUDE_DIR}" CACHE PATH
            "sqlite3 headers" FORCE)
        set(SQLite3_LIBRARY "${SQLITE3_STATIC_LIBRARY}" CACHE FILEPATH
            "sqlite3 static lib" FORCE)
        message(STATUS "sqlite3: system ${SQLITE3_STATIC_LIBRARY} "
                       "(static, CCG_SQLITE3_ROOT override)")
        return()
    endif()

    # 自行拉取。URL_HASH 固定的上游压缩包（按上游工程惯例钉版；年份段
    # 2026 是 sqlite.org 对 3.54.0 的发布布局的一部分）。
    CPMAddPackage(NAME sqlite3
        URL "https://www.sqlite.org/2026/sqlite-amalgamation-3540000.zip"
        URL_HASH SHA256=68e913b0fe8ec6b4e5f391c594a28872884e80d6cd042a1e50c957be46b5aa4d
        DOWNLOAD_ONLY YES)
    if(NOT EXISTS "${sqlite3_SOURCE_DIR}/sqlite3.c")
        message(FATAL_ERROR
            "ccg: sqlite3 amalgamation fetch incomplete - "
            "${sqlite3_SOURCE_DIR}/sqlite3.c missing")
    endif()
    add_library(cg_sqlite3 STATIC "${sqlite3_SOURCE_DIR}/sqlite3.c")
    add_library(cg::sqlite3 ALIAS cg_sqlite3)
    target_include_directories(cg_sqlite3 PUBLIC "${sqlite3_SOURCE_DIR}")
    target_compile_definitions(cg_sqlite3 PRIVATE
        SQLITE_ENABLE_COLUMN_METADATA SQLITE_THREADSAFE=1)
    # 为 sqlpp23 的 find_package 播种 CMake 的 FindSQLite3。库条目
    # 必须是普通路径（生成器表达式在 Ninja 链接行上不会被求值
    # ——实测过；把 SQLite3::SQLite3 别名到真实
    # 目标也不可行，因为 sqlpp23 会 install(EXPORT) 它的
    # 目标）。因此库文件进一个确定性的目录：
    # 属性用了（两臂相等的）生成器表达式，因为
    # 多配置生成器（Ninja Multi-Config）会往普通输出目录追加按配置的
    # 子目录——任何生成器表达式都会抑制该
    # 追加——于是 Debug 与 Release 的库存档落在缓存条目
    # 点名的同一路径上，且 CMake 会把导入位置变成构建
    # 排序边，因为该路径是已知的目标输出。
    set_target_properties(cg_sqlite3 PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "$<IF:$<CONFIG:Debug>,${CMAKE_BINARY_DIR}/3rdParty/sqlite3,${CMAKE_BINARY_DIR}/3rdParty/sqlite3>")
    set(SQLite3_INCLUDE_DIR "${sqlite3_SOURCE_DIR}" CACHE PATH
        "sqlite3 headers" FORCE)
    set(SQLite3_LIBRARY
        "${CMAKE_BINARY_DIR}/3rdParty/sqlite3/${CMAKE_STATIC_LIBRARY_PREFIX}cg_sqlite3${CMAKE_STATIC_LIBRARY_SUFFIX}"
        CACHE FILEPATH "sqlite3 static lib" FORCE)
    message(STATUS "sqlite3: self-fetched amalgamation 3.54.0 "
                   "(in-tree static build; CCG_SQLITE3_ROOT selects a "
                   "system install instead)")
endfunction()

function(_config_libclang)
    # 系统 libclang，自每库一引擎政策起改为运行期加载
    # （设计定案）：ccg 不再链接依赖 libclang 的
    # 任何东西——引擎（libclang.dll / libclang.so）经
    # cg.clang.loader 在运行期打开。构建仍需要 clang-c
    # 头文件（cg.clang.capi 的派发表按它们定型）以及
    # 运行期库的前缀位置（测试把它前插到
    # PATH，让默认引擎发现能解析）。导入库仅为推导
    # 该前缀而探测；它缺失已不再致命。
    set(CCG_LLVM_ROOT "" CACHE PATH
        "LLVM/libclang prefix override (holds include/ and bin/ or lib/); wins over the compiler-derived root and the default find search")
    set(CG_LIBCLANG_ROOT "" CACHE PATH "deprecated alias of CCG_LLVM_ROOT")
    _ccg_override_root(_root CCG_LLVM_ROOT CG_LIBCLANG_ROOT)

    if(NOT _root STREQUAL "")
        # 覆盖值胜出且是唯一被搜索的对象（MSVC preset 上
        # 也不例外，那里编译器推导的根无意义）。
        find_path(LIBCLANG_INCLUDE_DIR clang-c/Index.h
                  PATHS "${_root}/include" NO_DEFAULT_PATH)
        find_file(LIBCLANG_IMPLIB NAMES libclang.dll.a libclang.lib
                  PATHS "${_root}/lib" NO_DEFAULT_PATH)
    else()
        find_path(LIBCLANG_INCLUDE_DIR clang-c/Index.h
                  PATHS "${CCG_TOOLCHAIN_ROOT}/include" NO_DEFAULT_PATH)
        find_file(LIBCLANG_IMPLIB NAMES libclang.dll.a libclang.lib
                  PATHS "${CCG_TOOLCHAIN_ROOT}/lib" NO_DEFAULT_PATH)
        if(NOT LIBCLANG_INCLUDE_DIR)
            find_path(LIBCLANG_INCLUDE_DIR clang-c/Index.h)
            find_file(LIBCLANG_IMPLIB NAMES libclang.dll.a libclang.lib)
        endif()
    endif()
    if(NOT LIBCLANG_INCLUDE_DIR)
        if(NOT _root STREQUAL "")
            set(_where "override CCG_LLVM_ROOT '${_root}'")
        else()
            set(_where "the toolchain root derived from CMAKE_CXX_COMPILER "
                       "'${CCG_TOOLCHAIN_ROOT}' and the default search paths")
        endif()
        message(FATAL_ERROR
            "ccg: libclang headers not found (looked for clang-c/Index.h "
            "under ${_where}). Set CCG_LLVM_ROOT to the LLVM prefix "
            "containing include/clang-c/Index.h - for the MSVC preset this "
            "is any regular LLVM release install (the VS-bundled clang-cl "
            "component ships NO clang-c headers).")
    endif()
    # 运行期引擎库的所在：所有受支持的布局都把它放在
    # <prefix>/bin 里与驱动器相邻。优先从导入库的
    # 位置推导（这是前缀的扎实证据），否则取 include
    # 目录的父目录。测试把它前插到 PATH，让默认引擎
    # 发现（PATH -> 工具集根 -> bin）解析到它。
    if(LIBCLANG_IMPLIB)
        get_filename_component(_implib_lib "${LIBCLANG_IMPLIB}" DIRECTORY)
        get_filename_component(_llvm_prefix "${_implib_lib}" DIRECTORY)
    else()
        get_filename_component(_llvm_prefix "${LIBCLANG_INCLUDE_DIR}" DIRECTORY)
    endif()
    set(CCG_LIBCLANG_BIN "${_llvm_prefix}/bin" CACHE INTERNAL
        "bin dir of the resolved LLVM prefix (runtime libclang location)")
    add_library(cg_libclang INTERFACE)
    add_library(cg::libclang ALIAS cg_libclang)
    target_include_directories(cg_libclang INTERFACE "${LIBCLANG_INCLUDE_DIR}")
    if(LIBCLANG_IMPLIB)
        message(STATUS "libclang (runtime-loaded): headers ${LIBCLANG_INCLUDE_DIR}, import lib located but NOT linked: ${LIBCLANG_IMPLIB}")
    else()
        message(STATUS "libclang (runtime-loaded): headers ${LIBCLANG_INCLUDE_DIR}, no import lib (not needed - engine opens at runtime)")
    endif()
    message(STATUS "libclang runtime bin: ${CCG_LIBCLANG_BIN}")
endfunction()
