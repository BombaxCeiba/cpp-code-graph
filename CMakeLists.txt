cmake_minimum_required(VERSION 3.28)

# import std 闸门：实验性 UUID 随 CMake 版本条件化，且必须设在
# project() 之前（本机探针实测）。
# MSVC 注记：project() 之前编译器未知，
# UUID 无法在此按编译器分支——它在 MSVC preset 上保持惰性，
# 因为 cl 下没有任何东西设置 CXX_MODULE_STD（cmake/CcgUtils.cmake
# 改为供给工具集自带的 std.ixx，且 sqlpp23 的模块目标从不用
# import std——两者都经目标 MSVC 工具链上的探针验证）。
if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.3)
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "f35a9ac6-8463-4d38-8eec-5d6008153e7d")
else()
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")
endif()

project(cpp-code-graph LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Release 全程序优化：CMake 按工具链自动映射（MSVC=/GL+/LTCG，
# gcc/clang=-flto），一份声明覆盖三份工具链；仅 Release 生效，dev/asan
# 等开发构建不受影响。
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)

# MSVC 工具链的 CRT 选择：Debug 用 /MTd、其余配置（含 Release）用
# /MT——Release 产物把 VC 运行库静态链进 ccg.exe，净机器免装 redist
# 即可运行。仅 MSVC 家族工具链消费该变量（Platform/Windows-MSVC.cmake）；
# clang64（GNU triple）走自身工具链的静态链接语义，不读它，clang-dev
# 预设不受影响。发布形态的完整论证见安装包预研的 /MT 无风险预检。
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY
        "$<$<CONFIG:Debug>:MultiThreadedDebug>$<$<NOT:$<CONFIG:Debug>>:MultiThreaded>")
endif()

# CPM 引导；仓内缓存已暖，configure 不得克隆。
set(CPM_SOURCE_CACHE "${CMAKE_SOURCE_DIR}/.cpm-cache"
    CACHE PATH "Shared CPM source cache for cpp-code-graph")
include("${CMAKE_SOURCE_DIR}/cmake/CPM.cmake")

include("${CMAKE_SOURCE_DIR}/cmake/CcgUtils.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/CcgAsan.cmake")

add_subdirectory(3rdParty)
add_subdirectory(src)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# clang-tidy 闸门（tidy 目标）：clang-tidy 经构建树的
# compile_commands.json 解析命名模块单元时必须读到各持有
# 模块的目标产出的 BMI，故 tidy 目标依赖它们，须在每个持有
# 模块的目标都存在之后登记。
include("${CMAKE_SOURCE_DIR}/cmake/CcgTidy.cmake")
ccg_add_tidy_target()

# Doxygen HTML API 文档（可选，默认零感知）：cmake/Docs.cmake 负责发现链与
# 版本门槛，配置模板在 docs/doxygen/Doxyfile.in。docs 目标不进 ALL，
# 须显式 cmake --build <树> --target docs。
option(CCG_BUILD_DOCS "生成 Doxygen HTML 文档" OFF)
if(CCG_BUILD_DOCS)
    include("${CMAKE_SOURCE_DIR}/cmake/Docs.cmake")
endif()

# Release 安装规则（cmake --install）：扁平布局 ccg(.exe) +
# .ccg/ + docs/ + 二进制旁的 PATH shim 安装器；用法契约在
# cmake/Install.cmake 头注释。放在 add_subdirectory(src) 之后 include，
# 使解析 install(TARGETS) 行时 ccg 目标已存在。
include("${CMAKE_SOURCE_DIR}/cmake/Install.cmake")

# CPack 出包配置（Inno Setup 注入式安装包）：放在 Install.cmake 之后
# include，使 CPACK_RESOURCE_FILE_LICENSE 等引用的文件与 install()
# 载荷处于同一语义层（出包=自动复用 install 规则，见文件头注释）。
include("${CMAKE_SOURCE_DIR}/cmake/CPackSetup.cmake")
