# 每层各一个 module-only 测试可执行文件；harness 编译进每个之中。

ccg_add_test(cg_test_util util.test.ixx)
# shell 查找意图提取层（PreToolUse 四工具监听）：Bash/PowerShell
# 矩阵（生成表 shell-matrix.inc）、Search 三候选字段直读、预筛与降级链
# 注入；PowerShell 组经进程内引擎宿主，无引擎环境整组跳过。
ccg_add_test(cg_test_shellscan shellscan.test.ixx)
# shellscan 的 PowerShell 矩阵经进程内引擎宿主走真实 shim，装载是
# 可执行文件同目录直载，而测试可执行文件与 ccg.exe 分处不同输出
# 目录：这里镜像 src/ 的 POST_BUILD 摆齐（同一生成目录、同一产线
# 就绪标记，均由 src/CMakeLists.txt 以 CACHE INTERNAL 供给）。
if(WIN32 AND TARGET cg_cshost_shims)
    add_dependencies(cg_test_shellscan cg_cshost_shims)
    if(CCG_CSHOST_LINE51)
        add_custom_command(TARGET cg_test_shellscan POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CCG_CSHOST_GEN}/ccg-cshost51.dll"
                    "$<TARGET_FILE_DIR:cg_test_shellscan>/ccg-cshost51.dll"
            VERBATIM)
    endif()
    if(CCG_CSHOST_LINE7)
        add_custom_command(TARGET cg_test_shellscan POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CCG_CSHOST_GEN}/ccg-cshost7.dll"
                    "$<TARGET_FILE_DIR:cg_test_shellscan>/ccg-cshost7.dll"
            VERBATIM)
    endif()
endif()
# 崩溃守卫单元测试故意制造非法写入（NULL store）并期望
# 平台守卫捕获它；ASAN 运行会先拦截 SEGV
# （默认 handle_segv=1）并在守卫介入前就终止，所以唯独
# 这个可执行文件选择退出 SEGV 拦截。ASAN 的内存错误
# 报告不受影响，非 ASAN 构建则完全无视 ASAN_OPTIONS。
set_tests_properties(cg_test_util PROPERTIES ENVIRONMENT
    "ASAN_OPTIONS=handle_segv=0")
ccg_add_test(cg_test_db db.test.ixx)
ccg_add_test(cg_test_clang clang.test.ixx)
ccg_add_test(cg_test_extract extract.test.ixx)
ccg_add_test(cg_test_graph graph.test.ixx)
ccg_add_test(cg_test_cli cli.test.ixx)
# PreToolUse 正则开关的"回落默认"断言要求进程环境里 CCG_PRETOOLUSE_REGEX
# 处于未设置或空值态：ctest 显式置空值压过宿主残留——Windows 环境名查找
# 大小写不敏感，宿主 export 过的大小写变体（含小写）一并被覆盖，单测
# 才不会被先前评测实验留下的环境取值翻转成假红/假绿。
set_tests_properties(cg_test_cli PROPERTIES ENVIRONMENT
    "CCG_PRETOOLUSE_REGEX=")
ccg_add_test(cg_test_incr incr.test.ixx)
# 动态派发分析（schema v8）：三档传播用例（类型线索/函数内
# 传播/全局不动点）+ 三个具名 demo + 不动点确定性。
ccg_add_test(cg_test_analysis analysis.test.ixx)
ccg_add_test(cg_test_msbuild msbuild.test.ixx)
# 求值通道定向验证（MsbuildEval 过桥拿结构化元数据）经 51 线垫片走
# 真实 VS 求值引擎：与 shellscan 同款把 ccg-cshost51.dll 摆到测试可
# 执行文件旁（同目录直载）；求值面钉 51 线，7 线产物无需拷贝。无 VS
# 或垫片缺席的环境由用例内部按发现链缺席打说明行跳过（不红）。
if(WIN32 AND TARGET cg_cshost_shims)
    add_dependencies(cg_test_msbuild cg_cshost_shims)
    if(CCG_CSHOST_LINE51)
        add_custom_command(TARGET cg_test_msbuild POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CCG_CSHOST_GEN}/ccg-cshost51.dll"
                    "$<TARGET_FILE_DIR:cg_test_msbuild>/ccg-cshost51.dll"
            VERBATIM)
    endif()
endif()
# 镜像对账：DDL/表模型/围栏两侧常量镜像的逐值断言，
# 任何一侧漂移此套件先红。
ccg_add_test(cg_test_schema_mirror schema-mirror.test.ixx)
# libclang 包装层测试的夹具位置（夹具是解析输入
# 数据，从不编译）。
target_compile_definitions(cg_test_clang PRIVATE
    CCG_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/clang")
# 供提取器 + 图构建器测试使用的迷你工程夹具（同样是数据）。
target_compile_definitions(cg_test_extract PRIVATE
    CCG_SAMPLE1_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sample1"
    CCG_SAMPLE3_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sample3"
    CCG_USES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/uses")
target_compile_definitions(cg_test_graph PRIVATE
    CCG_SAMPLE1_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sample1"
    CCG_SAMPLE2_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sample2"
    CCG_USES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/uses"
    CCG_ILINK_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/ilinkage")
target_compile_definitions(cg_test_cli PRIVATE
    CCG_SAMPLE1_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sample1")
# sample2 同时供增量等价性测试套件使用。
target_compile_definitions(cg_test_incr PRIVATE
    CCG_SAMPLE2_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sample2")
target_compile_definitions(cg_test_analysis PRIVATE
    CCG_DISPATCH_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/dispatch")
# cl 模式适配器的研究日志 golden 与 rsp 夹具（仅数据）。
target_compile_definitions(cg_test_msbuild PRIVATE
    CCG_MSBUILD_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/msbuild")

# 真实二进制的冒烟测试：能运行、能找到运行期 DLL、能打印
# 版本行（完整 e2e 覆盖在 e2e.cmake 里）。
add_test(NAME ccg_smoke_version
         COMMAND $<TARGET_FILE:ccg> --version)
ccg_test_path_mods(_path_mods)
set_tests_properties(ccg_smoke_version PROPERTIES
    PASS_REGULAR_EXPRESSION "ccg [0-9]+\\.[0-9]+\\.[0-9]+"
    ENVIRONMENT_MODIFICATION "${_path_mods}")

# 可移植性闸门：构建输入（CMakeLists/presets/cmake/src 树）里
# 不允许出现机器绝对路径；一旦出现即以文件名+行号报失败。
add_test(NAME ccg_portability_scan
         COMMAND ${CMAKE_COMMAND} -P
                 "${CMAKE_CURRENT_SOURCE_DIR}/portability_scan.cmake"
                 "${CMAKE_SOURCE_DIR}")

# 自取件冒烟：presets 在树内构建的 sqlite 合并源
# （cmake/CcgDeps.cmake）在每条工具链上自证（打开、DDL、绑定
# 插入、回读往返、版本一致性）。按测试数据豁免用纯 C 编写。
#（cg::sqlite3 由 3rdParty 两条配置臂之一无条件创建，轮到
# tests 子目录时必然在场，无需再探目标存在。）
add_executable(ccg_sqlite_smoke sqlite_smoke.c)
# 源内注释是中文，cl 默认按宿主代码页读源文件会双字节
# 吞并破坏注释结构（C4819/C2059），必须显式按 UTF-8 读。
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(ccg_sqlite_smoke PRIVATE /utf-8)
endif()
target_link_libraries(ccg_sqlite_smoke PRIVATE cg::sqlite3)
add_test(NAME ccg_sqlite_smoke COMMAND ccg_sqlite_smoke)

# clang-c 头文件查找的发现顺序闸门（非法覆盖必须
# 报失败并点名 CCG_LLVM_ROOT；合法覆盖必须能解析）。
include(libclang-discovery.cmake)

# LLVM 按库引擎 e2e：配置回读 +
# 提示信息、显式路径引擎加载、引擎身份变化（路径与内容
# 双条件同时变化）触发 force-full、不可加载路径报错、增量稳定性。
# 第二引擎场景需要 VS 的 Clang 组件（配置期探测；
# 缺失时优雅跳过）。
include(llvm-e2e.cmake)

# 模块 e2e：ccg 自索引对本构建树自身的
# compile db + BMI（所有 TU 在喂入 BMI 下解析、导入关系落地、
# 诚实计数器触发），外加故意设置的失败类探针
# （config-mismatch 的轴命名、module-bmi-missing、存在第二份
# libclang 时的生产者版本——缺失时优雅跳过）。
include(module-e2e.cmake)

# BMI 分层并行 e2e：手写 clang 方言 cdb 的
# 自建夹具（不依赖构建树形态）——分层屏障/闭包完整、失败三形态、
# 增量闭包、并行 vs 串行逐字节、并发上限贯通（旗标/config/auto/层宽
# 封顶）、宽深两形态提速、以及 update 场景排队/构建/层间窗口期间
# ixx 改动的三形态（追加指令）。引擎不可发现的机器响亮 SKIP。
include(bmi-e2e.cmake)

# 端到端：真实 ccg.exe 对 sample1 夹具（索引、每条
# 查询命令、退出码、增量重索引）。
include(e2e.cmake)

# PreToolUse 注入字节锁：真实 hook 处理器与已锁定的
# 注入规格逐字节比对。语料源与锁定字节入库，参考库测试期
# 现场构建；唯一 SKIP 是无 Python 解释器。
include(injection-bytelock.cmake)

# 调用图导出 e2e：dot/JSON 形态、--output、
# 用法路径；本机存在 dot 可执行文件时做 graphviz 渲染冒烟。
include(graph-e2e.cmake)

# MSBuild e2e 多道闸门：解析行在所有平台运行；
# 提取闸门打印 *** SKIPPED 标记（CTest 的
# SKIP_REGULAR_EXPRESSION 让无 VS 的 CI 保持绿，而非失败）。
# 真实工具链运行很吃 Rebuild，因此给了 TIMEOUT 600。
add_test(NAME ccg_msbuild_e2e
         COMMAND ${CMAKE_COMMAND} -P
                 "${CMAKE_CURRENT_SOURCE_DIR}/msbuild-e2e.cmake"
                 $<TARGET_FILE:ccg>
                 "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/msbuild"
                 "${CMAKE_BINARY_DIR}/test-msbuild-e2e")
ccg_test_path_mods(_msbuild_mods)
set_tests_properties(ccg_msbuild_e2e PROPERTIES
    ENVIRONMENT_MODIFICATION "${_msbuild_mods}"
    SKIP_REGULAR_EXPRESSION "\\*\\*\\* SKIPPED"
    TIMEOUT 600)

if(WIN32)
    # 记录模式闸门：record/--full-build
    # 的 cdb 等价、完整 stub 面 + 生成器 TU 完整性 +
    # 零泄漏、对抗性沙盒泄漏逃逸、固定工具链
    # 版本感知。无 VS 时 SKIPPED（与 ccg_msbuild_e2e 对齐）；
    # 等价性闸按设计跑一次真实编译（等价性孪生）。
    add_test(NAME ccg_msbuild_record
             COMMAND ${CMAKE_COMMAND} -P
                     "${CMAKE_CURRENT_SOURCE_DIR}/msbuild-record.cmake"
                     $<TARGET_FILE:ccg>
                     "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/msbuild"
                     "${CMAKE_BINARY_DIR}/test-msbuild-record")
    ccg_test_path_mods(_record_mods)
    set_tests_properties(ccg_msbuild_record PROPERTIES
        ENVIRONMENT_MODIFICATION "${_record_mods}"
        SKIP_REGULAR_EXPRESSION "\\*\\*\\* SKIPPED"
        TIMEOUT 600)

    # 包装模式闸门：真实 ccg.exe 作为 MSBuild CL 包装器
    # 驱动 clstub——一个充当真实编译器的 Python 脚本替身（git 零二进制
    # 零 C 编译物）。拉起由 wrap 侧对 .py 后缀的 real_cl 自动前插 PATH
    # 上的 python 解释器；PATH 依赖与本脚本内的行断言读取器
    #（python -I）同源。非 Windows 平台整个 wrap 特性编译成空操作，
    # 闸门不存在。
    add_test(NAME ccg_msbuild_wrap
             COMMAND ${CMAKE_COMMAND} -P
                     "${CMAKE_CURRENT_SOURCE_DIR}/msbuild-wrap.cmake"
                     $<TARGET_FILE:ccg>
                     "${CMAKE_CURRENT_SOURCE_DIR}/clstub.py"
                     "${CMAKE_SOURCE_DIR}"
                     "${CMAKE_BINARY_DIR}/test-msbuild-wrap")
    ccg_test_path_mods(_wrap_mods)
    set_tests_properties(ccg_msbuild_wrap PROPERTIES
        ENVIRONMENT_MODIFICATION "${_wrap_mods}"
        TIMEOUT 300)

    # run_capture 并发闸门：洪水 stub
    # 在它仅有的一行 stdout 之前先向 stderr 写 128 KiB，
    # 顺序先 stdout 后 stderr 的排空会死锁；`--cl <stub>` 通过真实
    # ccg.exe 把横幅探测（第一次 run_capture）引向它。
    # 无 VS 时 SKIPPED（与 ccg_msbuild_e2e 对齐）。
    add_executable(ccg_floodstub floodstub.c)
    # 同 ccg_sqlite_smoke：中文注释在 MSVC 下必须显式 /utf-8。
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(ccg_floodstub PRIVATE /utf-8)
    endif()
    # 驱动分区的 run_process 排空闸门放在 clang 单元套件里
    # （compiler_version 经由同一 runner 走 --version）。
    target_compile_definitions(cg_test_clang PRIVATE
        CCG_FLOODSTUB_EXE="$<TARGET_FILE:ccg_floodstub>")
    add_test(NAME ccg_msbuild_capture
             COMMAND ${CMAKE_COMMAND} -P
                     "${CMAKE_CURRENT_SOURCE_DIR}/msbuild-capture.cmake"
                     $<TARGET_FILE:ccg>
                     $<TARGET_FILE:ccg_floodstub>
                     "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/msbuild"
                     "${CMAKE_BINARY_DIR}/test-msbuild-capture")
    ccg_test_path_mods(_capture_mods)
    set_tests_properties(ccg_msbuild_capture PROPERTIES
        ENVIRONMENT_MODIFICATION "${_capture_mods}"
        SKIP_REGULAR_EXPRESSION "\\*\\*\\* SKIPPED"
        TIMEOUT 300)
endif()

# MSBuild 金标六维对账门禁：工程求值金标与 ccg 记录产物
# （cdb + pch 旁车）在入库夹具语料（tests/msbuild-gt/corpus/，
# 仓内四个中性夹具解决方案的本体产物，路径全部为占位根）上
# 的六维对账（TU 覆盖/defines/include/PCH/工具集/杂项），
# 实测数字与入库基线锁死，语料与引擎双指纹负责漂移分罪
#（退出码 3=回归嫌疑 / 4=语料漂移待重审）。模块开关维度
#（/ifcOutput 等六项带值开关）已按成对带值形态
# 锁定（裸拼写回潮即红）；脚本内 XFAIL 判读机构保留，供其他
# 维度将来登记已知缺陷用。纯数据对账（无工具链
# 依赖、跨平台可跑），唯一 SKIP 是无 Python 解释器。
include(msbuild-gt.cmake)

# 崩溃转储矩阵闸门：ccg_crashprobe 启动第一行挂载 cg_core 里真实的
# cg.util.crashdump（与 ccg 的 main 同形），驱动脚本以子进程逐形态
# 触发致命故障并断言：可拦形态（av/so/terminate/heapcor/abort/
# purecall、multiav 三连、MSVC 的 invarg）各留恰一份 >50KB 的完整
# 转储——0 字节是崩溃线程原地写盘的实测失败形态，必须防假阳性；
# 快速失败形态（fastfail）按定界不得产生转储（守护"不夸大承诺"）；
# 环境变量未设置时崩溃退出码逐位如旧（零行为承诺）；目录不可写时
# 报错进 stderr 且退出码保持。仅 Windows（POSIX 分支为空实现，本
# 闸门不存在）。
if(WIN32)
    add_executable(ccg_crashprobe)
    ccg_module_target(ccg_crashprobe)
    ccg_add_modules(ccg_crashprobe "${CMAKE_CURRENT_SOURCE_DIR}"
        crashdump.probe.ixx)
    target_link_libraries(ccg_crashprobe PRIVATE cg_core)
    if(MSVC)
        # 与其他模块 TU 同因：Debug 无优化时节区数可越 COFF 上限
        # （C1128），/bigobj 无语义影响。
        target_compile_options(ccg_crashprobe PRIVATE /bigobj)
    endif()
    if(MSVC)
        set(_crashdump_msvc "1")
    else()
        set(_crashdump_msvc "0")
    endif()
    add_test(NAME ccg_crashdump_matrix
             COMMAND ${CMAKE_COMMAND} -P
                     "${CMAKE_CURRENT_SOURCE_DIR}/crashdump-matrix.cmake"
                     $<TARGET_FILE:ccg_crashprobe>
                     "${CMAKE_BINARY_DIR}/test-crashdump"
                     "${_crashdump_msvc}")
    ccg_test_path_mods(_crashdump_mods)
    set_tests_properties(ccg_crashdump_matrix PROPERTIES
        ENVIRONMENT_MODIFICATION "${_crashdump_mods}"
        # ASAN 插桩会先于进程级过滤器拦截访问违例（同 cg_test_util
        # 的注记），关掉其 SEGV 拦截让矩阵在 ASAN 树也可跑。
        ENVIRONMENT "ASAN_OPTIONS=handle_segv=0"
        TIMEOUT 240)
endif()
