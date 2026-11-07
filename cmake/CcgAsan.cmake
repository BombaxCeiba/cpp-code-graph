# cpp-code-graph 的 AddressSanitizer 接线（选项范式参考
# 同类工程的 use-asan 惯用法改写）。消毒器从不硬编码进 preset、
# CMAKE_*_FLAGS 变量或工具链文件：一切都来自此处一次性
# 解析的三态缓存选项，再通过按配置的生成器表达式
# 施加到每个项目目标，因此
# 多配置树只对列出的配置做插桩，
# 别的一概不碰。

# 三种状态，刻意用 STRING（而非 BOOL），使空的 AUTO 默认值
# 能与显式选择区分开：
#   ""  AUTO——当 CCG_ASAN_CONFIGS 之一正在被构建时启用
#              （单配置：比较 CMAKE_BUILD_TYPE，大小写不敏感；
#               多配置：生成器的任一配置命中即可）；
#   ON  强制启用（仍只作用于 CCG_ASAN_CONFIGS 的配置）；
#   OFF 强制停用。
set(CCG_USE_ASAN "" CACHE STRING
    "AddressSanitizer switch: '' = AUTO (on iff one of CCG_ASAN_CONFIGS is being built; single-config compares CMAKE_BUILD_TYPE case-insensitively, multi-config checks the generator's configuration set), ON = force on, OFF = force off. When on, only the configurations in CCG_ASAN_CONFIGS are instrumented (per-config generator expressions).")
set(CCG_ASAN_CONFIGS "Debug" CACHE STRING
    "Configurations instrumented while ASAN is active (semicolon-separated list, default 'Debug'). Escape hatch for wider multi-config coverage, e.g. 'Debug;Release' - a forced CCG_USE_ASAN=ON alone never widens the instrumented set.")

# ------------------------------------------------------------------ 解析 --
set(_ccg_asan_how "")
string(TOUPPER "${CCG_USE_ASAN}" _ccg_asan_choice)
set(_ccg_asan_active FALSE)
if(_ccg_asan_choice STREQUAL "")
    # AUTO：当列出的配置之一属于正在构建的配置时启用。
    set(_ccg_built "")
    if(CMAKE_CONFIGURATION_TYPES)
        set(_ccg_built "${CMAKE_CONFIGURATION_TYPES}")
    elseif(CMAKE_BUILD_TYPE)
        set(_ccg_built "${CMAKE_BUILD_TYPE}")
    endif()
    foreach(_c IN LISTS CCG_ASAN_CONFIGS)
        string(TOUPPER "${_c}" _cu)
        foreach(_b IN LISTS _ccg_built)
            string(TOUPPER "${_b}" _bu)
            if(_cu STREQUAL _bu)
                set(_ccg_asan_active TRUE)
                set(_ccg_asan_how "AUTO (configuration '${_b}' is being built)")
            endif()
        endforeach()
    endforeach()
    if(NOT _ccg_asan_active)
        set(_ccg_asan_how "AUTO (no configuration in '${CCG_ASAN_CONFIGS}' is being built)")
    endif()
elseif(_ccg_asan_choice MATCHES "^(ON|TRUE|1|YES)$")
    set(_ccg_asan_active TRUE)
    set(_ccg_asan_how "forced ON")
elseif(_ccg_asan_choice MATCHES "^(OFF|FALSE|0|NO)$")
    set(_ccg_asan_active FALSE)
    set(_ccg_asan_how "forced OFF")
else()
    message(FATAL_ERROR
        "ccg: CCG_USE_ASAN must be '', ON or OFF (got '${CCG_USE_ASAN}').")
endif()

# 单配置 + 开关激活 + 正在构建的配置不在列表内：下方的
# 生成器表达式会展开为空，因此大声报出来，而不是交付一个
# 悄悄没插桩的“已消毒”构建。
if(_ccg_asan_active AND NOT CMAKE_CONFIGURATION_TYPES AND CMAKE_BUILD_TYPE)
    set(_ccg_overlap FALSE)
    string(TOUPPER "${CMAKE_BUILD_TYPE}" _bu)
    foreach(_c IN LISTS CCG_ASAN_CONFIGS)
        string(TOUPPER "${_c}" _cu)
        if(_cu STREQUAL _bu)
            set(_ccg_overlap TRUE)
        endif()
    endforeach()
    if(NOT _ccg_overlap)
        message(WARNING
            "ccg: CCG_USE_ASAN is ${_ccg_asan_how}, but the single built "
            "configuration '${CMAKE_BUILD_TYPE}' is not in CCG_ASAN_CONFIGS "
            "'${CCG_ASAN_CONFIGS}' - nothing gets instrumented. Build one of "
            "the listed configurations or extend CCG_ASAN_CONFIGS.")
    endif()
endif()

set(CCG_ASAN_ACTIVE "${_ccg_asan_active}" CACHE INTERNAL
    "Resolved state of the CCG_USE_ASAN switch")
message(STATUS "ccg asan: ${_ccg_asan_how} -> instrumentation "
               "${CCG_ASAN_ACTIVE} for configuration(s) '${CCG_ASAN_CONFIGS}'")

# 下方的 ccg_asan_target() 在开关的每种状态下都必须有定义
# （ccg_module_target() 无条件调用它；未激活时
# 它立即返回）。

# -------------------------------------------------------------------- 旗标 --
# 编译旗标的载体：按配置的 CMAKE_CXX_FLAGS_<CONFIG>
# 变量，只在开关激活时为 CCG_ASAN_CONFIGS 的条目
# 追加——不用 target_compile_options 生成器表达式。
# 原因（CMake 4.4.4 实测）：CMake 为每个
# 消费者合成的模块 BMI 目标（<provider>@synth_N，为
# std.ixx 与跨目录消费的 sqlpp23 模块接口
# 创建）会在无配置上下文里捕获消费目标的 COMPILE_OPTIONS，此时
# $<$<CONFIG:Debug>:...> 生成器表达式被误判为真——消毒器
# 旗标曾泄漏到 Release/RelWithDebInfo ninja 文件的 6 条
# 合成 std/sqlpp23 BMI 规则上。而旗标变量即使在
# 合成规则内部也会被生成器按配置展开，这就恢复了下方的契约。
#
# 契约（不变）：多配置树（Ninja Multi-Config）
# 只对列出的配置插桩——Release 产物保持
# 干净——单配置树则在其唯一配置
# 位于列表时才插桩。范围说明：这些变量触及树上每个 C++ 目标，
# 含第三方 C++（C 目标不受影响）；这属于
# 更深的覆盖面，且那些模块 BMI 早已通过
# 合成规则泄漏被插桩过。链接旗标与运行期 DLL 拷贝
# 仍按可执行文件经 ccg_asan_target() 处理。旗标拼法
# 按编译器区分：MSVC 下 /fsanitize=address 无需链接旗标、无需
# 随附运行期 DLL（运行期在 vcvars64 下自动解析；
# ASAN 探针实测）；clang 拼法则另加保留帧
# 指针并在链接时传入消毒器。
set(CCG_ASAN_LINK_FLAGS "")
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(_ccg_asan_compile_str "/fsanitize=address")
else()
    set(_ccg_asan_compile_str "-fsanitize=address -fno-omit-frame-pointer")
    foreach(_c IN LISTS CCG_ASAN_CONFIGS)
        list(APPEND CCG_ASAN_LINK_FLAGS
             "$<$<CONFIG:${_c}>:-fsanitize=address>")
    endforeach()
endif()
if(CCG_ASAN_ACTIVE)
    foreach(_c IN LISTS CCG_ASAN_CONFIGS)
        string(TOUPPER "${_c}" _cu)
        set(CMAKE_CXX_FLAGS_${_cu}
            "${CMAKE_CXX_FLAGS_${_cu}} ${_ccg_asan_compile_str}")
    endforeach()
endif()

# ----------------------------------------------------------- 运行期 DLL ----
# 插桩后的可执行文件动态链接 ASAN 运行期
# （本 ABI 上是 libclang_rt.asan_dynamic-x86_64.dll；探针结论：可执行
# 文件旁与 PATH 上都没有它时，加载器启动即杀进程）。运行期
# 必须与给代码插桩的编译器匹配，因此发现流程遵循
# 工具集根目录政策，编译器推导的根排在最前；
# CCG_LLVM_ROOT 覆盖只作为附加根参与查找，
# 永远不会遮蔽编译器自带的运行期：
#   1. 编译器的 bin 目录（MSYS2 式前缀把该 DLL 放在这里）；
#   2. 编译器前缀下的标准运行期目录
#      （lib/clang/<version>/lib/windows 与 .../bin——标准 LLVM 布局）；
#   3. CCG_LLVM_ROOT 覆盖前缀，同样两种形态。
# 找不到 -> WARNING，绝不致命（PATH 仍是开发者的兜底；拷贝
# 步骤跳过）。惰性解析一次：ccg_asan_target() 只会在
# 3rdParty/ 之后运行，届时 CCG_LLVM_ROOT 缓存条目必然已存在。
function(_ccg_asan_find_dll)
    if(DEFINED CCG_ASAN_DLL)
        return()  # 已解析过（命中，或 '' = 未找到）
    endif()
    set(_candidates
        "${CCG_TOOLCHAIN_BIN}/libclang_rt.asan_dynamic-x86_64.dll")
    file(GLOB _rt_dirs "${CCG_TOOLCHAIN_ROOT}/lib/clang/*")
    foreach(_d IN LISTS _rt_dirs)
        list(APPEND _candidates
            "${_d}/lib/windows/libclang_rt.asan_dynamic-x86_64.dll"
            "${_d}/bin/libclang_rt.asan_dynamic-x86_64.dll")
    endforeach()
    if(CCG_LLVM_ROOT)
        list(APPEND _candidates
            "${CCG_LLVM_ROOT}/bin/libclang_rt.asan_dynamic-x86_64.dll")
        file(GLOB _ov_dirs "${CCG_LLVM_ROOT}/lib/clang/*")
        foreach(_d IN LISTS _ov_dirs)
            list(APPEND _candidates
                "${_d}/lib/windows/libclang_rt.asan_dynamic-x86_64.dll")
        endforeach()
    endif()
    set(_found "")
    foreach(_c IN LISTS _candidates)
        if(_found STREQUAL "" AND EXISTS "${_c}")
            set(_found "${_c}")
        endif()
    endforeach()
    if(_found STREQUAL "")
        message(WARNING
            "ccg asan: libclang_rt.asan_dynamic-x86_64.dll not found beside "
            "the compiler ('${CCG_TOOLCHAIN_BIN}'), under its runtime dirs, "
            "or under the CCG_LLVM_ROOT override. Instrumented executables "
            "will need the runtime DLL on PATH at run time; the beside-the-"
            "exe copy step is skipped.")
        set(CCG_ASAN_DLL "" CACHE INTERNAL
            "ASAN runtime DLL ('' = not found, copy step skipped)")
    else()
        message(STATUS "ccg asan: runtime DLL ${_found}")
        set(CCG_ASAN_DLL "${_found}" CACHE INTERNAL
            "ASAN runtime DLL (copied beside instrumented executables)")
    endif()
endfunction()

# 把消毒器的链接侧部件施加到一个项目目标（编译
# 插桩已通过上方的 CMAKE_CXX_FLAGS_<CONFIG>
# 变量覆盖全树）。MSVC 每目标无事可做：/fsanitize=address
# 自动解析运行期（ASAN 探针实测）。clang 的可执行文件
# 则拿到链接旗标与可执行文件旁的运行期 DLL 拷贝。由
# ccg_module_target() 调用（cmake/CcgUtils.cmake）。
function(ccg_asan_target target)
    if(NOT CCG_ASAN_ACTIVE)
        return()
    endif()
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        # /fsanitize=address 搭乘 CMAKE_CXX_FLAGS_<CONFIG>（见旗标
        # 一节）并自动解析自身运行期；无链接旗标、
        # 无可执行文件旁的 DLL 拷贝。
        return()
    endif()
    get_target_property(_type ${target} TYPE)
    if(NOT _type STREQUAL "EXECUTABLE")
        return()
    endif()
    target_link_options(${target} PRIVATE ${CCG_ASAN_LINK_FLAGS})
    _ccg_asan_find_dll()
    if(NOT CCG_ASAN_DLL)
        return()
    endif()
    # 与可选启用的工具链 DLL 闭包（ccg_copy_dlls_after_build）相同的
    # POST_BUILD copy_if_different 机制，但带配置守卫：命令
    # 本身携带 $<CONFIG:...> 生成器表达式，因此在多配置树上构建
    # 未插桩的配置时命令展开为空，
    # 不会往干净产物旁拷贝任何运行期。
    foreach(_c IN LISTS CCG_ASAN_CONFIGS)
        # COMMAND_EXPAND_LISTS：整命令的生成器表达式在配置属于
        # 列表时展开为分号列表（每个 token 一个 argv），否则
        # 展开为空列表——空命令会被构建系统
        # 跳过，因此干净配置什么也不拷贝。
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "$<$<CONFIG:${_c}>:${CMAKE_COMMAND};-E;copy_if_different;${CCG_ASAN_DLL};$<TARGET_FILE_DIR:${target}>>"
            COMMAND_EXPAND_LISTS
            VERBATIM)
    endforeach()
endfunction()
