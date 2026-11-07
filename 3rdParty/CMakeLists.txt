# cpp-code-graph 的第三方接线。
# 系统工具链依赖（sqlite3 自取 / libclang 头）走自适应发现策略
# （缓存覆盖 CCG_LLVM_ROOT / CCG_SQLITE3_ROOT 设置后只搜它们，否则
# 从当前编译器推导工具链根，未命中再标准搜索，仍一无所获就报错
# 点名覆盖变量，绝不烤死绝对路径；实现在 cmake/CcgDeps.cmake）
# 并以 cg:: 目标暴露；
# 其余一律经 CPM 拉取，走可覆盖的 GITHUB_MIRROR_URL。下列配方全部
# 已于 2026-10 在 MSVC 与 MinGW 双工具链本机 configure
# 实测通过。

set(GITHUB_MIRROR_URL "https://github.com" CACHE STRING "GitHub prefix override")

# 发现与自取函数（sqlite3 合并源、clang-c 头）。
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/CcgDeps.cmake")

# 毒缓存守卫：CPM 取包在克隆中途被网络中断（掉线、超时）会留下一个
# 空源目录；CPM 下次运行时信任该目录加其哈希标记，于是在
# FetchContent 内部死在一个莫名其妙的 git 错误上（实测：sqlpp23 的
# 取包曾被网络中断打断——sqlite 合并源的 sqlite3.c 检查只兜得住它自家
# 那种变体）。此处改为带着恢复步骤快速失败。
file(GLOB _ccg_cpm_entries LIST_DIRECTORIES true "${CPM_SOURCE_CACHE}/*/*")
foreach(_e IN LISTS _ccg_cpm_entries)
    if(IS_DIRECTORY "${_e}")
        file(GLOB _inner LIST_DIRECTORIES true "${_e}/*")
        list(LENGTH _inner _n)
        if(_n EQUAL 0)
            message(FATAL_ERROR
                "ccg: CPM cache entry '${_e}' is EMPTY - a previous fetch "
                "was interrupted. Remove that one directory (keep its "
                "siblings) and re-run configure; the package is fetched "
                "fresh from its pin.")
        endif()
    endif()
endforeach()

# -------------------------------------------------------------------- CPM ---

function(_cpm_add_pfr)
    # 第一个：它的 Boost::pfr 目标必须在 cpp_yyjson 校验
    # CPPYYJSON_PFR_TARGET 之前存在。版本钉：boost-1.92.0 =
    # boostorg/pfr 的最新稳定 tag（2026-10-11 经 GitHub tags API 核对）。
    # BUILD_TESTING OFF：pfr 的测试链接 Boost::core/Boost::container_hash
    # （只在 Boost 超级工程内才有定义）。首次全新 configure 碰巧跳过
    # 它们（3rdParty 运行时 BUILD_TESTING 还没进缓存）；之后的重新生成
    # 会拿到缓存值 ON 而失败。钉死 OFF，让每次重新生成都一致。
    CPMAddPackage(NAME pfr
        GIT_REPOSITORY "${GITHUB_MIRROR_URL}/boostorg/pfr.git"
        GIT_TAG boost-1.92.0
        GIT_SHALLOW TRUE
        SYSTEM YES
        EXCLUDE_FROM_ALL YES
        OPTIONS
            "BUILD_TESTING OFF")
endfunction()

function(_cpm_add_yyjson)
    # 第二个：`yyjson` 目标已存在时，cpp_yyjson 的
    # find_and_link() 直接链它，cpp_yyjson 内部的 FetchContent
    # 兜底（从 raw github.com 钉 0.12.0）变成空操作——同名
    # FetchContent NAME 已填充，我们钉的 1f934d2 获胜。
    # 静态 C 库，无需随包发 DLL。
    CPMAddPackage(NAME yyjson
        GIT_REPOSITORY "${GITHUB_MIRROR_URL}/ibireme/yyjson.git"
        GIT_TAG 1f934d2aaee3e2d016fc1f271b6a01388581dcbf
        GIT_SHALLOW TRUE
        SYSTEM YES
        EXCLUDE_FROM_ALL YES
        OPTIONS
            "YYJSON_BUILD_TESTS OFF"
            "YYJSON_BUILD_FUZZER OFF"
            "YYJSON_BUILD_MISC OFF"
            "YYJSON_BUILD_DOC OFF"
            "BUILD_SHARED_LIBS OFF")
endfunction()

function(_cpm_add_yyjson_cpp)
    # 第三个：纯头 INTERFACE 库，C++20；std::format 后端（clang 23 验证可用）。
    CPMAddPackage(NAME cpp_yyjson
        GIT_REPOSITORY "${GITHUB_MIRROR_URL}/BombaxCeiba/yyjson-cpp.git"
        GIT_TAG 2ed9d20e485a5040e64165b8990ed0682dce458c
        GIT_SHALLOW TRUE
        GIT_SUBMODULES ""              # 跳过 benchmark 第三方子模块
        SYSTEM YES
        EXCLUDE_FROM_ALL YES
        OPTIONS
            "CPPYYJSON_BUILD_TEST OFF"
            "CPPYYJSON_BUILD_BENCH OFF"
            "CPPYYJSON_USE_STD_FORMAT ON"
            "CPPYYJSON_PFR_TARGET Boost::pfr")
endfunction()

function(_cpm_add_sqlpp23)
    # sqlpp23 自家的 find_package(SQLite3 REQUIRED) 消费 _config_sqlite3()
    # 预置的 SQLite3_* 缓存条目（每条工具链同一份 sqlite：自取的合并源，
    # 或被 CCG_SQLITE3_ROOT 覆盖时的系统安装——绝不是烤死的路径）。
    CPMAddPackage(NAME sqlpp23
        GIT_REPOSITORY "${GITHUB_MIRROR_URL}/rbock/sqlpp23.git"
        GIT_TAG 7e8c9a7fde5a7bcc6a1cf694c29a4e9a886ffeb6
        GIT_SHALLOW TRUE
        SYSTEM YES
        EXCLUDE_FROM_ALL YES
        OPTIONS
            "BUILD_SQLITE3_CONNECTOR ON"
            "BUILD_WITH_MODULES ON")   # 生成 sqlpp23.core / sqlpp23.sqlite3 的 BMI
endfunction()

function(_cpm_add_treesitter)
    # tree-sitter 运行时 v0.27.1（PreToolUse shell 监听的设计定案
    # ）。AMALGAMATED 单 TU（官方 CMake 的 AMALGAMATED 选项形态：
    # lib/src/lib.c 就地 include 全部实现 .c）——一个翻译单元拿到完整
    # 核心库，wasm 特性默认关闭（wasm_store.c 在无 TREE_SITTER_FEATURE_WASM
    # 时编译成空壳）。静态库、零传递依赖。MSVC 的五个 /wd 与上游自己的
    # CMakeLists 相同（上游在 /W4 下测得；此处按 /W3 基线预防性压制，
    # 保证第三方目标在两条工具链上都安静）。
    CPMAddPackage(NAME tree-sitter
        GIT_REPOSITORY "${GITHUB_MIRROR_URL}/tree-sitter/tree-sitter.git"
        GIT_TAG v0.27.1
        GIT_SHALLOW TRUE
        SYSTEM YES
        EXCLUDE_FROM_ALL YES
        DOWNLOAD_ONLY YES)
    if(NOT EXISTS "${tree-sitter_SOURCE_DIR}/lib/src/lib.c")
        message(FATAL_ERROR
            "ccg: tree-sitter fetch incomplete - "
            "${tree-sitter_SOURCE_DIR}/lib/src/lib.c missing")
    endif()
    add_library(cg_treesitter STATIC
        "${tree-sitter_SOURCE_DIR}/lib/src/lib.c")
    add_library(cg::treesitter ALIAS cg_treesitter)
    # 运行时源码使用 C11（_Static_assert 等）：MSVC 编 .c 的默认
    # 方言不带它，必须显式立标准（clang 默认 gnu11 本就满足）。
    set_target_properties(cg_treesitter PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON)
    target_include_directories(cg_treesitter
        PUBLIC "${tree-sitter_SOURCE_DIR}/lib/include")
    target_include_directories(cg_treesitter
        PRIVATE "${tree-sitter_SOURCE_DIR}/lib/src")
    if(MSVC)
        target_compile_options(cg_treesitter PRIVATE
            /utf-8  # api.h 文档注释含非 ASCII，按代码页读会告 C4819
            /W3
            /wd4018  # 有符号/无符号比较（上游同款压制）
            /wd4232  # 非标准扩展（上游同款压制）
            /wd4244  # 可能的数据丢失（上游同款压制）
            /wd4267  # size_t 转换丢失（上游同款压制）
            /wd4701) # 可能未初始化的局部变量（上游同款压制）
    endif()
endfunction()

function(_cpm_add_treesitter_bash)
    # 官方 Bash 语法 v0.25.1（tree-sitter-bash；与运行时 v0.27.1 同为
    # ABI 14 配套代际）。生成的 parser.c 自带解析框架（不引用运行时
    # 符号），scanner.c 是外置词法器；两者已在沙盒用
    # cl /W3 与 clang 双编译器验证零警告（预研实测
    # ）。语法的 CMake 会尝试调 tree-sitter CLI
    # 重新生成 parser.c，故不用它的构建系统、DOWNLOAD_ONLY 自编。
    CPMAddPackage(NAME tree-sitter-bash
        GIT_REPOSITORY "${GITHUB_MIRROR_URL}/tree-sitter/tree-sitter-bash.git"
        GIT_TAG v0.25.1
        GIT_SHALLOW TRUE
        SYSTEM YES
        EXCLUDE_FROM_ALL YES
        DOWNLOAD_ONLY YES)
    if(NOT EXISTS "${tree-sitter-bash_SOURCE_DIR}/src/parser.c")
        message(FATAL_ERROR
            "ccg: tree-sitter-bash fetch incomplete - "
            "${tree-sitter-bash_SOURCE_DIR}/src/parser.c missing")
    endif()
    add_library(cg_treesitter_bash STATIC
        "${tree-sitter-bash_SOURCE_DIR}/src/parser.c"
        "${tree-sitter-bash_SOURCE_DIR}/src/scanner.c")
    add_library(cg::treesitter_bash ALIAS cg_treesitter_bash)
    set_target_properties(cg_treesitter_bash PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON)
    if(MSVC)
        target_compile_options(cg_treesitter_bash PRIVATE /utf-8 /W3)
    endif()
    target_include_directories(cg_treesitter_bash
        PRIVATE "${tree-sitter-bash_SOURCE_DIR}/src")
    # 语法库与运行时永远成对使用（门面 cg.util.treesit 一并消费），
    # 消费方只链 cg::treesitter 即可拿到语言符号。
    target_link_libraries(cg_treesitter_bash PUBLIC cg::treesitter)
endfunction()

# 调用次序有讲究：pfr 必须先于 cpp_yyjson（目标校验所需）。
_config_sqlite3()
_config_libclang()
_cpm_add_pfr()
_cpm_add_yyjson()
_cpm_add_yyjson_cpp()
_cpm_add_sqlpp23()
_cpm_add_treesitter()
_cpm_add_treesitter_bash()
