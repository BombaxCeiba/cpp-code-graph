# cg_core：持有全部 src 模块单元的单一 OBJECT 库——自
# 废除模块实现单元拆分以来，它们全部是
# .ixx 接口（该拆分当年用来躲避的 C2801 修复已由
# cmake/CcgC2801Workaround.cmake 接管）。
# PUBLIC 的 CXX_MODULES 文件集：消费者（ccg、测试可执行文件）继承 BMI——
# 与消费 sqlpp23 自家模块目标的形态一致（探针验证过）。
# 唯一的经典源文件是下面两个 TU：yyjson 类型在 MSVC 上绝不能
# 进入模块单元（util.json.bridge.cpp），
# db.sqlpp.anchor.cpp 则锚定 sqlpp23 的内联静态量：非模板类的
# 静态数据成员与命名空间级内联变量。
# SEH 崩溃守卫重新以模块单元编译（util.guard.ixx）——它在
# gnu-ABI 的 clang 工具链上书写 __try 所需的 -fms-extensions
# 语言选项由 cmake/CcgUtils.cmake 全构建范围注入，因此整棵树的
# 每个 pcm（含 CMake 合成的 std 模块）都携带同一种
# LangOpts 形态。
add_library(cg_core OBJECT
    util/util.json.bridge.cpp
    # 把 sqlpp23 的内联静态量（非模板类的静态数据成员与
    # 命名空间级内联变量）锚进 cg_core 的经典 TU：MSVC 对模块
    # 单元中仅供导入的内联变量只发出 UNDEF 引用，而 sqlpp23 的
    # 模块目标文件永远到不了 ccg 的链接行
    # （见 db.sqlpp.anchor.cpp 的头部注释）。
    db/db.sqlpp.anchor.cpp
    # SymBits 批量位运算内核，每档一个经典 TU：MSVC 没有
    # 按函数的 target 属性，因此向量档只能
    # 整个 TU 一起启用（下方按源文件施加架构旗标）。标量
    # 档保持全构建基线，作为两个向量档的等价判定基准；
    # 签名在 symbits.kernels.h。
    analysis/symbits.kernels.cpp
    analysis/symbits.kernels.avx2.cpp
    analysis/symbits.kernels.avx512.cpp)
if(MSVC)
    set_source_files_properties(
        analysis/symbits.kernels.avx2.cpp PROPERTIES COMPILE_OPTIONS /arch:AVX2)
    set_source_files_properties(
        analysis/symbits.kernels.avx512.cpp PROPERTIES COMPILE_OPTIONS /arch:AVX512)
else()
    set_source_files_properties(
        analysis/symbits.kernels.avx2.cpp PROPERTIES COMPILE_OPTIONS -mavx2)
    # 探针在 F/VL/BW/DQ 同时具备时才开 AVX-512；此处精确镜像同一
    # 子集，确保编译器在本 TU 中可能生成的任何指令都不超出探测范围。
    set_source_files_properties(
        analysis/symbits.kernels.avx512.cpp PROPERTIES
        COMPILE_OPTIONS "-mavx512f;-mavx512vl;-mavx512bw;-mavx512dq")
endif()
ccg_module_target(cg_core)
# 模块单元的清单分散在各层自己的子目录里：每个子目录的 modules.cmake
# 把本层的单元追加进 cg_core，文件名相对该子目录，文件集的基准目录
# 是 src。本文件只负责把清单包含进来；包含次序决定单元进入目标的
# 次序。
foreach(_ccg_layer IN ITEMS util db clang extract analysis graph msbuild cli)
    include("${CMAKE_CURRENT_SOURCE_DIR}/${_ccg_layer}/modules.cmake")
endforeach()
target_link_libraries(cg_core PUBLIC
    cg::sqlite3
    cg::libclang
    # tree-sitter 运行时与官方 Bash 语法成对消费（cg.util.treesit
    # 的 bash_language 链接期解析语法库符号 tree_sitter_bash）。
    cg::treesitter
    cg::treesitter_bash
    # 钩子受限正则路径的匹配引擎：再导出单元在全局模块片段包含
    # 引擎头文件，头文件路径与静态链接宏经目标公开。
    cg::pcre2
    sqlpp23::sqlite3_module
    cpp_yyjson::cpp_yyjson)
if(MSVC)
    # db.store.ixx 的模板实例化（sqlpp23 表模型 + 全部 upsert 体型）在
    # 无优化的 Debug 配置下节区数超 COFF 上限（C1128，实测复现；
    # Release 因节区合并不触发）。/bigobj 是标准解，无语义影响。
    target_compile_options(cg_core PRIVATE /bigobj)
endif()
if(WIN32)
    # Wrap 模式与发现功能使用 CommandLineToArgvW（shell32）；tests/ 里的
    # 纯 C 闸门桩以同样方式链接它。Version 供 PowerShell 进程内引擎宿主
    # 读 hostfxr.dll 的产品版本（商店版复制布局的版本目录名）。User32
    # 供 cg.util.crashdump 的崩溃态路径拼接（wsprintfW——比 CRT 的
    # 格式化例程更可依赖）。
    target_link_libraries(cg_core PUBLIC Shell32 Version User32)
endif()

# C# 对象宿主垫片程序集（cshost：Select-String 引擎面与 MSBuild 求值
# 面共宿主的 C# 侧）：构建期由 cmake/CsShimGen.cmake 编译两份程序集
#（51 线用 Windows 自带的 Framework csc、7 线用可选的 dotnet SDK），
# dll 本体留在生成目录；运行期唯一装载路线是可执行文件同目录直载
#（cg.util.cshost 的 bundled_shim），因此下方在 ccg 定义后以
# POST_BUILD 拷贝把开发树摆齐，发布面（cmake/Install.cmake）从同一
# 生成目录随包分发。工具缺失的线产物缺席，运行期按路线不可用降级
#（缺席诊断进 last_detail 链），构建永不因此变红。
if(WIN32)
    # CACHE INTERNAL 而非子目录局部 set：安装面（cmake/Install.cmake，
    # 顶层作用域后段 include）与测试面的 POST_BUILD 拷贝需要从这里取
    # 产物路径与产线就绪标记。记账变量族 CCG_CSHOST_*（纯构建内部名，
    # 跨三文件共享；外部契约名 CCG_PSHOST_FORBID/CCG_PSHOME 不属此族，
    # 见 cg.util.cshost）。
    set(CCG_CSHOST_GEN "${CMAKE_BINARY_DIR}/gen/cshost" CACHE INTERNAL
        "C# object-host shim dll generation dir (install surface source)")
    set(CCG_CSHOST_SRC "${CMAKE_CURRENT_SOURCE_DIR}/util/cshost-shim")
    find_program(CCG_CSC_EXE
        NAMES csc.exe
        HINTS "$ENV{SystemRoot}/Microsoft.NET/Framework64/v4.0.30319"
              "$ENV{SystemRoot}/Microsoft.NET/Framework/v4.0.30319")
    set(CCG_CSHOST_CSC "")
    if(CCG_CSC_EXE)
        set(CCG_CSHOST_CSC "${CCG_CSC_EXE}")
    endif()
    file(GLOB _ccg_sma_refs
         "$ENV{SystemRoot}/Microsoft.NET/assembly/GAC_MSIL/System.Management.Automation/*/System.Management.Automation.dll")
    list(SORT _ccg_sma_refs)
    set(CCG_CSHOST_SMA_REF "")
    if(_ccg_sma_refs)
        list(GET _ccg_sma_refs -1 CCG_CSHOST_SMA_REF)
    endif()
    find_program(CCG_DOTNET_EXE NAMES dotnet)
    set(CCG_CSHOST_DOTNET "")
    if(CCG_DOTNET_EXE)
        set(CCG_CSHOST_DOTNET "${CCG_DOTNET_EXE}")
    endif()
    # ccg-cshost7 的 SMA 编译引用：MSI pwsh 目录优先，其次 NuGet 全局包的
    # microsoft.powershell.sdk ref 程序集；都缺时该产物线缺席
    #（运行期 7 路线按不可用降级，构建不红）。系统位置一律从环境
    # 变量推导（可移植性闸门禁机器绝对路径）。
    set(CCG_CSHOST_SMA7 "")
    if(EXISTS "$ENV{ProgramFiles}/PowerShell/7/System.Management.Automation.dll")
        set(CCG_CSHOST_SMA7 "$ENV{ProgramFiles}/PowerShell/7/System.Management.Automation.dll")
    else()
        file(GLOB _ccg_sdk_sma
             "$ENV{USERPROFILE}/.nuget/packages/microsoft.powershell.sdk/*/ref/net10.0/System.Management.Automation.dll")
        list(SORT _ccg_sdk_sma)
        if(_ccg_sdk_sma)
            list(GET _ccg_sdk_sma -1 CCG_CSHOST_SMA7)
        endif()
    endif()
    # 产线就绪标记（CACHE INTERNAL 供 ccg 与 tests 的 POST_BUILD 摆齐
    # 共用）：某线工具齐备则构建期产物必然生成，该线的拷贝步骤才挂
    # 上——与 CsShimGen 内部的产线守卫同一判据；工具缺失的线产物与
    # 拷贝一并缺席（构建不红）。
    set(CCG_CSHOST_LINE51 "")
    if(CCG_CSC_EXE AND CCG_CSHOST_SMA_REF)
        set(CCG_CSHOST_LINE51 ON)
    endif()
    set(CCG_CSHOST_LINE51 "${CCG_CSHOST_LINE51}" CACHE INTERNAL
        "ccg-cshost51 toolchain line ready (build-time copy gate)")
    set(CCG_CSHOST_LINE7 "")
    if(CCG_DOTNET_EXE AND CCG_CSHOST_SMA7)
        set(CCG_CSHOST_LINE7 ON)
    endif()
    set(CCG_CSHOST_LINE7 "${CCG_CSHOST_LINE7}" CACHE INTERNAL
        "ccg-cshost7 toolchain line ready (build-time copy gate)")
    add_custom_command(
        OUTPUT "${CCG_CSHOST_GEN}/ccg-cshost51.dll"
               "${CCG_CSHOST_GEN}/ccg-cshost7.dll"
        COMMAND ${CMAKE_COMMAND}
                -D SRC_DIR=${CCG_CSHOST_SRC}
                -D GEN_DIR=${CCG_CSHOST_GEN}
                -D CSC_EXE=${CCG_CSHOST_CSC}
                -D SMA_REF=${CCG_CSHOST_SMA_REF}
                -D DOTNET_EXE=${CCG_CSHOST_DOTNET}
                -D SMA7_HINT=${CCG_CSHOST_SMA7}
                -P "${CMAKE_SOURCE_DIR}/cmake/CsShimGen.cmake"
        DEPENDS "${CCG_CSHOST_SRC}/CshostTable.cs"
                "${CCG_CSHOST_SRC}/SelectStringCore.cs"
                "${CCG_CSHOST_SRC}/MsbuildEval.cs"
                "${CCG_CSHOST_SRC}/Cshost51Entry.cs"
                "${CCG_CSHOST_SRC}/Cshost7Entry.cs"
                "${CCG_CSHOST_SRC}/Cshost7.csproj"
                "${CMAKE_SOURCE_DIR}/cmake/CsShimGen.cmake"
        COMMENT "ccg: cs-shim compile"
        VERBATIM)
    add_custom_target(cg_cshost_shims
        DEPENDS "${CCG_CSHOST_GEN}/ccg-cshost51.dll"
                "${CCG_CSHOST_GEN}/ccg-cshost7.dll")
endif()

# CLI 可执行文件。cg.cli（在 cg_core 内）承担全部命令
# 派发；app.main 只是跳板进入它。
add_executable(ccg)
ccg_module_target(ccg)
include("${CMAKE_CURRENT_SOURCE_DIR}/app/modules.cmake")
target_link_libraries(ccg PRIVATE cg_core)

# 可选启用的打包辅助；开发构建依赖工具集 bin 目录已在
# PATH 上。闭包从解析出的 LLVM 前缀推导（缺失
# 条目以一行 STATUS 跳过——闭包因工具集/版本
# 而异）。
ccg_toolchain_dlls(_ccg_dlls)
ccg_copy_dlls_after_build(ccg ${_ccg_dlls})

# 开发树摆齐：把两份 cshost 垫片 dll 拷到 ccg.exe 旁，与发布
# 布局同形态——cg.util.cshost 的同目录直载因此对开发构建树同样
# 生效（发布面走 cmake/Install.cmake 的安装规则，同一生成目录）。
# 守卫取产线就绪标记而非产物文件的配置期 EXISTS：全新构建树在
# 配置时产物尚未生成，按工具在场判定才能保证首次构建即摆齐。
# dll 不参与链接，add_dependencies 只为 POST_BUILD 拷贝排序。
if(WIN32)
    add_dependencies(ccg cg_cshost_shims)
    if(CCG_CSHOST_LINE51)
        add_custom_command(TARGET ccg POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CCG_CSHOST_GEN}/ccg-cshost51.dll"
                    "$<TARGET_FILE_DIR:ccg>/ccg-cshost51.dll"
            VERBATIM)
    endif()
    if(CCG_CSHOST_LINE7)
        add_custom_command(TARGET ccg POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CCG_CSHOST_GEN}/ccg-cshost7.dll"
                    "$<TARGET_FILE_DIR:ccg>/ccg-cshost7.dll"
            VERBATIM)
    endif()
endif()

# 发布布局的集成材料：publish/（hook 跳板脚本
# 加 .claude 技能/插件家族）以幂等方式拷到
# 可执行文件旁成为 .ccg/，对齐交付布局 ccg(.exe) +
# .ccg/{hooks,.claude}。copy_directory 与已存在于
# 那里的内容合并，因此重复构建收敛而不删除任何手工放进
# .ccg/ 的东西。插件目录刻意不带 skills
# 副本发布：技能家族的唯一源（publish/.claude/skills）在
# 拷贝时折进插件，因此插件渠道与
# 拷到 .claude 的渠道提供完全相同的技能集。
if(EXISTS "${CMAKE_SOURCE_DIR}/publish")
    add_custom_command(TARGET ccg POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_SOURCE_DIR}/publish"
                "$<TARGET_FILE_DIR:ccg>/.ccg"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_SOURCE_DIR}/publish/.claude/skills"
                "$<TARGET_FILE_DIR:ccg>/.ccg/.claude/plugin/skills"
        VERBATIM)
endif()

# 可执行文件旁放随包手册：--help 与报错文本的关键事实已内联
# 进输出本身，完整手册从 docs/en/（英文）与 docs/zh-cn/
# （中文译文）伴随二进制——只拿到开发树二进制的人也能读到
# 全文，且与 cmake/Install.cmake 安装的发行布局（docs/ 根放
# 英文、docs/zh-cn/ 放中文）完全一致。用 copy_if_different
# 而非上面整目录的 copy_directory：文档没有
# 手工放置的内容需要收敛，只在内容变化时重拷
# 让增量构建不做多余的拷贝。
foreach(_ccg_doc IN ITEMS CLI.md REALTIME.md MSBUILD.md SCHEMA.md
                           DESIGN.md INSTALL.md)
    if(EXISTS "${CMAKE_SOURCE_DIR}/docs/en/${_ccg_doc}")
        add_custom_command(TARGET ccg POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_SOURCE_DIR}/docs/en/${_ccg_doc}"
                    "$<TARGET_FILE_DIR:ccg>/docs/${_ccg_doc}"
            VERBATIM)
    endif()
    if(EXISTS "${CMAKE_SOURCE_DIR}/docs/zh-cn/${_ccg_doc}")
        add_custom_command(TARGET ccg POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_SOURCE_DIR}/docs/zh-cn/${_ccg_doc}"
                    "$<TARGET_FILE_DIR:ccg>/docs/zh-cn/${_ccg_doc}"
            VERBATIM)
    endif()
endforeach()
