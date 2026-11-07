# 模块 e2e（模块索引功能的第一轮端到端验收）：ccg
# 自索引就是验收语料——构建树自身的
# compile_commands.json + 构建刚产出的 BMI。
#   self-index 喂入构建 BMI 的自索引：每个 TU 解析（failed: 0）且
#       模块 import 关系落库（全树 imports >= 100）
#   no-op-rerun 无操作重跑全跳过（未覆盖单元提示已随特性退役：
#       构建从未提及的文件不是任何人的行动项）
#   std-mismatch-probe 故意配置失配探针：一个消费者以
#       -std=c++20 对 c++23 构建的 BMI 重放 -> rc=4 被归类为
#       bmi-config-mismatch，并从 ccg 自己的 key 点名 -std 轴
#   bmi-missing-probe module-bmi-missing 探针：modmap 喂入指向不在
#       磁盘上的 pcm -> 喂入前拒绝并点名模块 + pcm 路径
#   producer-version-probe 故意生产者版本探针（仅当存在第二份真实
#       libclang 时）：钉住另一引擎会拒绝喂入
#       BMI 并点名生产者版本轴
#   export-overlay  export 覆盖层闸门：符号产出越过覆盖层阈值
#       且 export 命名空间符号可查询
#
# 一个文件的两副面孔（llvm-e2e.cmake 模式）：
#   从 tests/CMakeLists.txt INCLUDE 进来 -> 注册 ccg_module_e2e
#   ctest，经 $ENV{ProgramFiles} 探测第二份 libclang。
#   经 cmake -P module-e2e.cmake <ccg.exe> <source-root> <build-dir>
#   <work-dir> <vs-libclang-or-dash> 运行。

if(NOT CMAKE_SCRIPT_MODE_FILE)
    if(MSVC)
        # 自索引把本构建树的 BMI 喂给 libclang
        # 引擎：cl 下它们是 IFC 文件，任何 clang 发行版都读不了
        #（工具链格式事实，非 ccg 缺陷）。带标记跳过，
        # 而非失败；clang 工具链树保留完整闸门。
        message(STATUS "module-e2e: MSVC build tree produces IFC BMIs - "
                       "self-index e2e runs on the clang toolchain only")
        add_test(NAME ccg_module_e2e
                 COMMAND ${CMAKE_COMMAND} -E echo
                         "*** SKIPPED: MSVC tree produces IFC BMIs the clang engine cannot consume")
        set_tests_properties(ccg_module_e2e PROPERTIES
            SKIP_REGULAR_EXPRESSION "\\*\\*\\* SKIPPED")
        return()
    endif()
    set(_vs_libclang "")
    set(_pf_x86_name "ProgramFiles(x86)")
    foreach(_pf "$ENV{ProgramFiles}" "$ENV{${_pf_x86_name}}")
        file(GLOB _hits
             "${_pf}/Microsoft Visual Studio/*/*/VC/Tools/Llvm/*/bin/libclang.dll")
        foreach(_h IN LISTS _hits)
            if(_h MATCHES "[/\\\\]x64[/\\\\]")
                set(_vs_libclang "${_h}")
            endif()
        endforeach()
    endforeach()
    if(NOT _vs_libclang STREQUAL "")
        file(TO_CMAKE_PATH "${_vs_libclang}" _vs_libclang)
    endif()
    add_test(NAME ccg_module_e2e
             COMMAND ${CMAKE_COMMAND} -P
                     "${CMAKE_CURRENT_SOURCE_DIR}/module-e2e.cmake"
                     $<TARGET_FILE:ccg>
                     "${CMAKE_SOURCE_DIR}"
                     "${CMAKE_BINARY_DIR}"
                     "${CMAKE_BINARY_DIR}/test-module-e2e"
                     "${_vs_libclang}"
                     "$<CONFIG>")
    # AddressSanitizer 构建让这个多进程 e2e 的每个阶段都变慢
    #（全模块树自索引加串行 BMI 依赖
    # 链）以复利系数放大；普通 Debug 树在 2400 秒内轻松
    # 通过，所以只在消毒器开启时放宽预算。
    set(_module_e2e_timeout 2400)
    if(CMAKE_CXX_FLAGS MATCHES "sanitize")
        set(_module_e2e_timeout 5400)
    endif()
    ccg_test_path_mods(_module_mods)
    set_tests_properties(ccg_module_e2e PROPERTIES
        ENVIRONMENT_MODIFICATION "${_module_mods}"
        TIMEOUT "${_module_e2e_timeout}")
    return()
endif()

if(NOT DEFINED CMAKE_ARGV7)
    message(FATAL_ERROR
        "module-e2e.cmake: expected <ccg.exe> <source-root> <build-dir> "
        "<work-dir> <vs-libclang-or-dash>")
endif()
set(exe      "${CMAKE_ARGV3}")
set(src      "${CMAKE_ARGV4}")
set(build    "${CMAKE_ARGV5}")
set(work     "${CMAKE_ARGV6}")
set(vsdll    "${CMAKE_ARGV7}")
set(cfg      "${CMAKE_ARGV8}")   # $<CONFIG> at test time ("" if unknown)
set(cdb      "${build}/compile_commands.json")

if(NOT EXISTS "${cdb}")
    message(FATAL_ERROR "module-e2e: no compile_commands.json at ${cdb}")
endif()

file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}")

# 多配置生成器（Ninja Multi-Config）对每个文件每配置
# 产出一条 compile_commands.json 条目；只有被测配置的
# BMI 被构建，兄弟条目的模块喂入因此挨饿。点名且匹配的
# 配置存在时过滤到它；单配置树
#（命令里无 CMAKE_INTDIR）保持文件不动。
if(NOT cfg STREQUAL "")
    file(READ "${cdb}" _cdb_all)
    string(JSON _n LENGTH "${_cdb_all}")
    set(_kept "[")
    set(_first ON)
    set(_i 0)
    while(_i LESS _n)
        string(JSON _e GET "${_cdb_all}" ${_i})
        string(JSON _c GET "${_e}" "command")
        if(_c MATCHES "[/\\\\]${cfg}[/\\\\]")
            if(NOT _first)
                set(_kept "${_kept},")
            endif()
            set(_kept "${_kept}${_e}")
            set(_first OFF)
        endif()
        math(EXPR _i "${_i} + 1")
    endwhile()
    set(_kept "${_kept}]")
    string(JSON _kept_n LENGTH "${_kept}")
    if(_kept_n GREATER 0)
        set(cdb "${work}/cdb_${cfg}.json")
        file(WRITE "${cdb}" "${_kept}")
        message(STATUS "module-e2e: filtered compile db to the '${cfg}' "
                       "configuration (${_kept_n} entries)")
    endif()
endif()

# 运行/断言宏组（ccg_run/expect_rc/expect_out/expect_err/
# expect_absent/grab_out_count）收纳在同目录 ccg-harness.cmake，
# 供各闸门脚本共享；expect_absent 守卫无人能开的幽灵分组
# 标签缺席（stderr 上不得出现探针）。
include("${CMAKE_CURRENT_LIST_DIR}/ccg-harness.cmake")

message(STATUS "module-e2e: self-index with fed build BMIs")
ccg_run(index "${src}" --compile-commands "${cdb}" -o "${work}" --config self)
expect_rc(0 "self-index run")
expect_out("failed: 0" "self-index every TU parses (module TUs included)")
# 单配置噪声签名（".dir/<src-subpath>/"）不是配置
# 名——分歧签名提示必须对其沉默。
expect_absent("share no common file"
    "self-index noise signatures do not raise the advisory")
grab_out_count("imports: " "self-index imports reported")
if(value LESS 100)
    message(FATAL_ERROR
        "module-e2e [self-index]: expected at least 100 import relations, got ${value}\n--stdout--\n${out}")
endif()

# 未覆盖模块单元提示已随特性退役：构建
# 从未提及的文件（孤儿、条件排除、其它配置文件）
# 不是任何人的行动项，所以运行对它们只字不提。
message(STATUS "module-e2e: no-op-rerun silent about units the build never mentions")
ccg_run(index "${src}" --compile-commands "${cdb}" -o "${work}" --config self)
expect_rc(0 "no-op-rerun run")
expect_out("skipped: all" "no-op-rerun fingerprint-stable no-op rerun")
expect_out("failed: 0" "no-op-rerun rerun still clean")

# export-overlay export 抹除覆盖层：同宽
# 抹除模块 TU 的 `export` 解锁 export 命名空间文件——
# 模块 TU 符号产出覆盖层前后实测 385 -> 2207（每个
# 模块单元都有产出、零空）。总量闸门还驮着下方
# 稳定的约 5300 个头文件回退符号，所以阈值稳稳
# 低于实测总量 7493；两个查询是纯覆盖层
# 战果（`export namespace` 块内的符号，此前不可查询）。
message(STATUS "module-e2e: export-overlay yield + tripwires")
grab_out_count("symbols: " "export-overlay symbol yield reported")
if(value LESS 7000)
    message(FATAL_ERROR
        "module-e2e [export-overlay]: expected at least 7000 symbols with the export overlay, got ${value}")
endif()
ccg_run(query builtin_resource_dir -o "${work}" --config self)
expect_rc(0 "export-overlay export-namespace symbol queryable (clang.tu.ixx)")
ccg_run(query tokenize_command -o "${work}" --config self)
expect_rc(0 "export-overlay export-namespace symbol queryable (compile_db)")

message(STATUS "module-e2e: std-mismatch-probe deliberate config-mismatch probe (-std)")
file(READ "${cdb}" _cdb_text)
string(REGEX REPLACE
    "(-std=)c\\+\\+23([^\"]*clang\\.tu\\.ixx\\.obj\\.modmap)"
    "\\1c++20\\2" _cdb_d2 "${_cdb_text}")
if(_cdb_d2 STREQUAL "${_cdb_text}")
    message(FATAL_ERROR "module-e2e [std-mismatch-probe]: probe patch did not apply")
endif()
file(WRITE "${work}/probe_d2.json" "${_cdb_d2}")
ccg_run(index "${src}" --compile-commands "${work}/probe_d2.json"
        -o "${work}" --config self -v)
expect_rc(0 "std-mismatch-probe index exit (per-TU failure is not a run failure)")
expect_out("failed: 1" "std-mismatch-probe exactly the mis-configured TU fails")
expect_out("failure classes: bmi-config-mismatch x1" "std-mismatch-probe class summarized")
expect_err("bmi-config-mismatch" "std-mismatch-probe class named")
expect_err("axis: -std" "std-mismatch-probe axis named from ccg's own key")
expect_err("producer -std=c++23" "std-mismatch-probe names the recorded producer std")

message(STATUS "module-e2e: bmi-missing-probe")
# modmap 路径依赖生成器布局：单配置 Ninja 把它直接放
# 目标目录下，Ninja Multi-Config 每配置深一层嵌套——
# 取两种形态的首个命中。
set(_mm "${build}/src/CMakeFiles/cg_core.dir/clang/clang.tu.ixx.obj.modmap")
if(NOT EXISTS "${_mm}")
    file(GLOB _mms "${build}/src/CMakeFiles/cg_core.dir/*/clang/clang.tu.ixx.obj.modmap")
    list(GET _mms 0 _mm)
endif()
file(READ "${_mm}" _mm)
string(REPLACE "cg.util.pcm" "cg.util.GONE.pcm" _mm_gone "${_mm}")
file(WRITE "${work}/tu_gone.modmap" "${_mm_gone}")
string(REGEX REPLACE
    "@[^\"]*clang\\.tu\\.ixx\\.obj\\.modmap"
    "@${work}/tu_gone.modmap" _cdb_mm "${_cdb_text}")
if(_cdb_mm STREQUAL "${_cdb_text}")
    message(FATAL_ERROR "module-e2e [bmi-missing-probe]: probe patch did not apply")
endif()
file(WRITE "${work}/probe_mm.json" "${_cdb_mm}")
ccg_run(index "${src}" --compile-commands "${work}/probe_mm.json"
        -o "${work}" --config self -v)
expect_rc(0 "bmi-missing-probe index exit")
expect_out("failed: 1" "bmi-missing-probe exactly the starved TU fails")
expect_out("failure classes: module-bmi-missing x1" "bmi-missing-probe class summarized")
expect_err("module-bmi-missing" "bmi-missing-probe class named")
expect_err("cg.util" "bmi-missing-probe module named")
expect_err("cg.util.GONE.pcm" "bmi-missing-probe pcm path named")

if(NOT vsdll STREQUAL "-" AND NOT vsdll STREQUAL "")
    message(STATUS "module-e2e: producer-version-probe (engine ${vsdll})")
    ccg_run(config set llvm-path "${vsdll}" -o "${work}" --config probe-d1)
    expect_rc(0 "producer-version-probe pin engine")
    ccg_run(index "${src}" --compile-commands "${cdb}"
            -o "${work}" --config probe-d1 -v)
    expect_rc(0 "producer-version-probe index exit")
    grab_out_count("failed: " "producer-version-probe failures reported")
    if(value LESS 1)
        message(FATAL_ERROR
            "module-e2e [producer-version-probe]: expected refused module TUs, got failed: ${value}")
    endif()
    expect_out("bmi-config-mismatch" "producer-version-probe class summarized")
    expect_err("axis: pcm producer version" "producer-version-probe producer-version axis named")
else()
    message(STATUS "module-e2e: producer-version-probe skipped (no second libclang found)")
endif()

# ----------------------------------------------------------------------------
# no-db-refusal（设计定案）：没有编译数据库的模块树
# 被拒绝。没有自构建回退扫描：错误的图比缺图
# 更危险，所以运行以非零退出码加
# 可操作错误退出且不建库。
message(STATUS "module-e2e: no-db-refusal no-compile-db module tree is refused")
set(m3 "${work}/m3tree")
file(MAKE_DIRECTORY "${m3}")
file(WRITE "${m3}/demo.ixx" "export module demo;
export int demo_value();
")
ccg_run(index "${m3}" -o "${work}" --config m3)
expect_rc(2 "no-db-refusal: index without compile db exits 2")
expect_err("no compile_commands.json found under"
           "no-db-refusal error names the missing compile db")
expect_err("CMAKE_EXPORT_COMPILE_COMMANDS=ON"
           "no-db-refusal error carries the CMake hint")
expect_err("ccg msbuild" "no-db-refusal error names the msbuild route")
expect_err("--compile-commands <file>" "no-db-refusal error names the explicit flag")
if(EXISTS "${work}/m3/ccg.db")
    message(FATAL_ERROR "module-e2e [no-db-refusal]: the refused run created a database")
endif()

# ----------------------------------------------------------------------------
# db-route（数据库路由）：clang 方言编译数据库、模块命令不带
# 喂入。多配置生成器把每个配置的命令都记进
# compile_commands.json，但只有已构建配置的 .modmap
# rsp 文件与 BMI 存在——未构建条目会硬失败 rc=4。
# 数据库路由经自构建喂它们，以记录的 argv
# 为生产/解析权威（每配置一趟）。
# 多配置编译数据库在索引期
# 按每个内部配置拆成一个配置目录、
# 各一库——恰是本夹具形态的东西就是验收语料。
#   db-route-split 迷你 CMake 模块工程，配置 + 构建一个配置，索引
#       完整库：拆成 Debug/Release/RelWithDebInfo 配置
#       目录，每个 TU 解析，按配置的路由注记点名
#       各趟，第二个未构建趟经跨配置捐供者
#       复用第一趟的 std pcm
#   db-route-discovery 发现体验：对 3 个配置的裸查询被
#       拒绝并点名候选；--config 选择
#   db-route-cache-rerun 缓存命中重跑：无重建、全跳过（全部 3 库）
#   db-route-edit-cascade 源编辑：每个未构建配置只有被编辑模块 +
#       下游重建，failed 保持 0
#   db-route-digest-change 记录命令变化（给某配置的命令加 -D）：
#       恰好该配置的 pcm 重建（摘要权威），
#       兄弟配置复用
#   db-route-config-retire 配置退役：把某配置的条目从库中删掉
#       即停止刷新其目录（stale 隔离；运行
#       提及余下配置）
#   db-route-disjoint 分歧签名提示：各配置索引不相交
#       源的多配置库通过重复文件判别——
#       运行保持单配置且在 stderr 点名歧义
#       （拆分盲区提示）
message(STATUS "module-e2e: compile-db route (feed-less clang commands)")
find_program(M5_NINJA ninja)
if(M5_NINJA STREQUAL "M5_NINJA-NOTFOUND")
    message(STATUS "module-e2e: db-route skipped (no ninja on PATH for the "
                   "multi-config fixture)")
elseif(CMAKE_VERSION VERSION_LESS 4.3)
    message(STATUS "module-e2e: db-route skipped (CMake ${CMAKE_VERSION} lacks "
                   "the import-std gate)")
else()
    set(m5 "${work}/m5proj")
    set(m5b "${work}/m5build")
    file(MAKE_DIRECTORY "${m5}")
    file(WRITE "${m5}/CMakeLists.txt" [[
cmake_minimum_required(VERSION 3.28)
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "f35a9ac6-8463-4d38-8eec-5d6008153e7d")
project(m5probe LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_library(m5core)
target_sources(m5core PUBLIC
    FILE_SET cxx_modules TYPE CXX_MODULES FILES base.ixx mid.ixx)
set_target_properties(m5core PROPERTIES CXX_MODULE_STD ON)
target_compile_features(m5core PUBLIC cxx_std_23)
add_executable(m5app main.cpp)
target_link_libraries(m5app PRIVATE m5core)
]])
    file(WRITE "${m5}/base.ixx" [[
export module m5.base;
import std;
export namespace m5 {
inline int base_value() { return 21; }
}
]])
    file(WRITE "${m5}/mid.ixx" [[
export module m5.mid;
import std;
export import m5.base;
export namespace m5 {
inline int mid_double(int v) { return v * 2; }
}
]])
    file(WRITE "${m5}/main.cpp" [[
import std;
import m5.mid;
int main() { return m5::mid_double(m5::base_value()) == 42 ? 0 : 1; }
]])
    execute_process(COMMAND "${CMAKE_COMMAND}" -G "Ninja Multi-Config"
                            -DCMAKE_CXX_COMPILER=clang++
                            -DCMAKE_C_COMPILER=clang
                            -S "${m5}" -B "${m5b}"
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR
            "module-e2e [db-route-split]: fixture configure failed (${rc})\n${out}\n${err}")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" --build "${m5b}"
                            --config Debug
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR
            "module-e2e [db-route-split]: fixture build failed (${rc})\n${out}\n${err}")
    endif()
    # 库记录每个配置；只有 Debug 的 rsp/pcm 存在。
    # 拆分规则：-o 目录下每个内部配置一个配置
    #    目录，各含一个 ccg.db。
    set(m5out "${work}/m5")
    ccg_run(index "${m5}" --compile-commands "${m5b}/compile_commands.json"
            -o "${m5out}" --config m5 -v)
    expect_rc(0 "db-route-split index exit")
    expect_out("failed: 0" "db-route-split every TU parses across all configs")
    expect_err("splitting into one configuration directory per config"
               "db-route-split the split names itself")
    expect_err("compile-db route" "db-route-split the db route names itself")
    expect_err("config 'Release'" "db-route-split one pass per unbuilt configuration")
    expect_err("config 'RelWithDebInfo'" "db-route-split the second unbuilt pass")
    # 跨配置 std 捐供者：字母序更晚的
    # 未构建配置复用更早者的 std pcm（拷贝
    #    搬迁，绝不重建）。
    expect_err("std BMI reused" "db-route-split the second pass relocates the std pcm")
    foreach(_c Debug Release RelWithDebInfo)
        if(NOT EXISTS "${m5out}/${_c}/ccg.db")
            message(FATAL_ERROR
                "module-e2e [db-route-split]: split config db missing at ${m5out}/${_c}/ccg.db")
        endif()
        if(NOT EXISTS "${m5out}/${_c}/profile.json")
            message(FATAL_ERROR
                "module-e2e [db-route-split]: split config profile missing at ${m5out}/${_c}/profile.json")
        endif()
    endforeach()
    # 按配置 BMI 缓存：每个未构建配置的树持有引擎
    #    清单 + 纯模块名 pcm 于自己的 <config_key>/ 目录。
    file(GLOB _m5_rel_manifest "${m5out}/Release/bmi-self-*/*/manifest.json")
    if(NOT _m5_rel_manifest)
        message(FATAL_ERROR
            "module-e2e [db-route-split]: Release engine manifest.json missing under ${m5out}/Release")
    endif()
    file(GLOB _m5_pcms "${m5out}/Release/bmi-self-*/*/*/m5.base.pcm")
    if(NOT _m5_pcms)
        message(FATAL_ERROR
            "module-e2e [db-route-split]: pure module-name pcm (m5.base.pcm) missing "
            "under the Release config directory")
    endif()

    message(STATUS "module-e2e: db-route discovery UX (single vs multiple configs)")
    # 多配置：裸查询被拒绝并列出
    #    候选（规格 5）；--config 选择。
    ccg_run(query base_value -o "${m5out}")
    expect_rc(1 "db-route-discovery bare query over multiple configs rejected")
    expect_err("multiple configurations" "db-route-discovery the error names the shape")
    expect_err("Release" "db-route-discovery candidates listed")
    ccg_run(query base_value -o "${m5out}" --config Release)
    expect_rc(0 "db-route-discovery --config selects the Release db")
    ccg_run(query mid_double -o "${m5out}" --config Release)
    expect_rc(0 "db-route-discovery consumer module symbol queryable")
    # 单配置目录：--config 自动选择（规格 5）。
    ccg_run(query base_value -o "${m5out}/Release")
    expect_rc(0 "db-route-discovery a folder holding one config auto-selects it")

    message(STATUS "module-e2e: db-route-cache-rerun cache-hit rerun")
    ccg_run(index "${m5}" --compile-commands "${m5b}/compile_commands.json"
            -o "${m5out}" --config m5 -v)
    expect_rc(0 "db-route-cache-rerun rerun exit")
    expect_out("failed: 0" "db-route-cache-rerun rerun stays clean")
    expect_out("skipped: all" "db-route-cache-rerun fingerprint-stable no-op rerun")

    message(STATUS "module-e2e: db-route-edit-cascade source-edit cascade")
    file(APPEND "${m5}/base.ixx" "\n// probe edit\n")
    ccg_run(index "${m5}" --compile-commands "${m5b}/compile_commands.json"
            -o "${m5out}" --config m5 -v)
    expect_rc(0 "db-route-edit-cascade edit rerun exit")
    expect_out("failed: 0" "db-route-edit-cascade cascade stays clean")
    # 每个未构建配置重构建 base + 依赖它的 mid；已构建
    #    （Debug）配置逐字重放、从未进趟。
    expect_err("config 'Release', recorded argv authority): 2 built"
               "db-route-edit-cascade Release rebuilt the edit + downstream only")
    expect_err("config 'RelWithDebInfo', recorded argv authority): 2 built"
               "db-route-edit-cascade RelWithDebInfo rebuilt the edit + downstream only")

    message(STATUS "module-e2e: db-route-digest-change recorded-command digest probe")
    file(READ "${m5b}/compile_commands.json" _m5_cdb)
    # 只给 mid.ixx 的 Release 命令加 -DM5_PROBE_X=1：锚点是
    #    Release 自己的 "-O3 -DNDEBUG" 前缀，且运行留在单个
    #    JSON 字符串内（[^\"]* 穿不过条目的闭引号——裸
    #    .* 在此会跨条目匹配，已实测）。
    string(REGEX REPLACE
        "(-O3 -DNDEBUG)( [^\"]*mid\\.ixx\\.obj\\.modmap)"
        "\\1 -DM5_PROBE_X=1\\2" _m5_cdb_rel "${_m5_cdb}")
    string(REGEX MATCHALL "\-DM5_PROBE_X=1" _m5_hits "${_m5_cdb_rel}")
    list(LENGTH _m5_hits _m5_n)
    if(NOT _m5_n EQUAL 1)
        message(FATAL_ERROR
            "module-e2e [db-route-digest-change]: probe patch matched ${_m5_n} commands, wanted 1")
    endif()
    file(WRITE "${work}/m5_probe.json" "${_m5_cdb_rel}")
    ccg_run(index "${m5}" --compile-commands "${work}/m5_probe.json"
            -o "${m5out}" --config m5 -v)
    expect_rc(0 "db-route-digest-change digest rerun exit")
    expect_out("failed: 0" "db-route-digest-change digest rebuild stays clean")
    expect_err("config 'Release', recorded argv authority): 1 built"
               "db-route-digest-change the changed command rebuilt exactly its module")
    expect_err("config 'RelWithDebInfo', recorded argv authority): 0 built"
               "db-route-digest-change the sibling config reused everything")

    message(STATUS "module-e2e: db-route-config-retire configuration retirement (split layout)")
    # 把 RelWithDebInfo 条目从库中删掉：拆分不再
    #    创建/刷新该配置——其目录留在磁盘上
    #    作 stale 隔离（库绝不背着用户删除），
    #    且运行本身只提及余下配置。
    string(JSON _m5_n LENGTH "${_m5_cdb}")
    set(_m5_kept "[")
    set(_m5_first ON)
    set(_m5_i 0)
    while(_m5_i LESS _m5_n)
        string(JSON _m5_e GET "${_m5_cdb}" ${_m5_i})
        string(JSON _m5_c GET "${_m5_e}" "command")
        if(NOT _m5_c MATCHES "RelWithDebInfo")
            if(NOT _m5_first)
                set(_m5_kept "${_m5_kept},")
            endif()
            set(_m5_kept "${_m5_kept}${_m5_e}")
            set(_m5_first OFF)
        endif()
        math(EXPR _m5_i "${_m5_i} + 1")
    endwhile()
    set(_m5_kept "${_m5_kept}]")
    string(JSON _m5_kept_n LENGTH "${_m5_kept}")
    if(_m5_kept_n GREATER_EQUAL _m5_n OR _m5_kept_n EQUAL 0)
        message(FATAL_ERROR
            "module-e2e [db-route-config-retire]: filtering dropped nothing (kept "
            "${_m5_kept_n} of ${_m5_n} entries)")
    endif()
    file(WRITE "${work}/m5_norwdi.json" "${_m5_kept}")
    ccg_run(index "${m5}" --compile-commands "${work}/m5_norwdi.json"
            -o "${m5out}" --config m5 -v)
    expect_rc(0 "db-route-config-retire retirement rerun exit")
    expect_out("failed: 0" "db-route-config-retire retirement stays clean")
    expect_err("2 build configurations" "db-route-config-retire the split names the remainder")
    expect_absent("config 'RelWithDebInfo'"
        "db-route-config-retire the retired configuration runs no pass")
    if(NOT EXISTS "${m5out}/RelWithDebInfo/ccg.db")
        message(FATAL_ERROR
            "module-e2e [db-route-config-retire]: the retired configuration's db was deleted "
            "(stale quarantine keeps it until the user removes it)")
    endif()

    message(STATUS "module-e2e: db-route-disjoint disjoint-signature advisory (blind spot)")
    # 拆分判别子是跨签名重复的文件；各配置
    #    索引不相交源的多配置库拿不出
    #    这种重复。留一条 Debug 条目（main.cpp）与一条
    #    Release 条目（base.ixx）——无共享文件、两签名皆配置形：
    #    运行必须保持单配置且在 stderr 说出来，而非
    #    静默吞掉歧义。
    string(JSON _m5_n LENGTH "${_m5_cdb}")
    set(_m5_pair "[")
    set(_m5_first ON)
    set(_m5_i 0)
    while(_m5_i LESS _m5_n)
        string(JSON _m5_e GET "${_m5_cdb}" ${_m5_i})
        string(JSON _m5_c GET "${_m5_e}" "command")
        string(JSON _m5_f GET "${_m5_e}" "file")
        if((_m5_c MATCHES "Debug" AND _m5_f MATCHES "main\\.cpp$")
                OR (_m5_c MATCHES "Release" AND _m5_f MATCHES "base\\.ixx$"))
            if(NOT _m5_first)
                set(_m5_pair "${_m5_pair},")
            endif()
            set(_m5_pair "${_m5_pair}${_m5_e}")
            set(_m5_first OFF)
        endif()
        math(EXPR _m5_i "${_m5_i} + 1")
    endwhile()
    if(_m5_first)
        message(FATAL_ERROR
            "module-e2e [db-route-disjoint]: the disjoint filter kept no entry "
            "(fixture db shape changed?)")
    endif()
    file(WRITE "${work}/m5_disjoint.json" "${_m5_pair}]")
    set(m5fout "${work}/m5f")
    ccg_run(index "${m5}" --compile-commands "${work}/m5_disjoint.json"
            -o "${m5fout}" --config m5 -v)
    expect_rc(0 "db-route-disjoint disjoint index exit")
    expect_out("failed: 0" "db-route-disjoint both disjoint entries parse clean")
    expect_err("share no common file" "db-route-disjoint the advisory names the blind spot")
    expect_err("single-configuration" "db-route-disjoint the advisory names the verdict")
    expect_err("Debug, Release" "db-route-disjoint the advisory lists the signatures")
    if(NOT EXISTS "${m5fout}/m5/ccg.db")
        message(FATAL_ERROR
            "module-e2e [db-route-disjoint]: no db under the --config directory")
    endif()
    foreach(_c Debug Release)
        if(EXISTS "${m5fout}/${_c}")
            message(FATAL_ERROR
                "module-e2e [db-route-disjoint]: the disjoint db must not split "
                "(${_c} configuration directory appeared)")
        endif()
    endforeach()

    # langopt-axes（PCM 语言选项轴）：语言选项如
    #    -fms-extensions 是 pcm 导入兼容身份的一部分——
    #    不带它构建的 std pcm 不能被带它记录的 TU
    #    导入（一个不透明的模块失败）。数据库路由因此必须
    #    把记录的轴带进它合成的每个 argv（std 生产
    #    也在内），且当记录的 argv 们意见不一时
    #    把配置按每个语言选项向量拆成趟——
    #    每组构建并消费自己的 std pcm 变体，跨组
    #    导入以具名类降级而非静默
    #    混合 LangOpts 形态。
    message(STATUS "module-e2e: langopt-consistent consistent PCM language-option axes")
    # 每条 Release 命令都加上该轴（崩溃守卫模块单元在
    #    gnu-ABI Windows clang 上全构建范围编译所用的
    #    旗标）：配置内一致，所以 std pcm 带着
    #    它构建且每个导入保持兼容。只有 Release 带
    #    "-O3 -NDEBUG" 风格
    #    优化前缀，锚点漏不到其它
    #    配置。
    string(REGEX REPLACE
        "(-O3 -DNDEBUG)" "\\1 -fms-extensions" _m6_cdb_a "${_m5_cdb}")
    if(_m6_cdb_a STREQUAL "${_m5_cdb}")
        message(FATAL_ERROR
            "module-e2e [langopt-consistent]: probe patch matched no Release command")
    endif()
    file(WRITE "${work}/m6_consistent.json" "${_m6_cdb_a}")
    ccg_run(index "${m5}" --compile-commands "${work}/m6_consistent.json"
            -o "${work}/m6" --config m6 -v)
    expect_rc(0 "langopt-consistent consistent-axis index exit")
    expect_out("failed: 0" "langopt-consistent the axis-carrying chain parses clean")
    # 轴随配置键走：别的配置的 std pcm 不能
    #    被本趟复用（其命令摘要不同），所以轴形
    #    std pcm 是构建出来的，不是从兄弟搬迁的。
    expect_err("std BMI built" "langopt-consistent the std pcm is produced under the axis")

    message(STATUS "module-e2e: langopt-mixed mixed PCM language-option axes")
    # 只有 base.ixx 的 Release 命令加轴（锚点纪律
    #    同 db-route-digest-change：[^\"]* 穿不过条目闭引号）：
    #    配置的记录 argv 们意见不一。数据库路由
    #    把配置拆成两个语言选项组、各带
    #    自己的 std pcm 变体：base.ixx 在带轴组
    #    （'Release~langopt(-fms-extensions)'）下解析，而 mid.ixx/main.cpp
    #    （无旗标）再也导入不了 base 的轴形 pcm——其
    #    导入以具名 module-not-found 类降级（绝不
    #    静默混合 LangOpts），且没有别的组被拖下水。
    string(REGEX REPLACE
        "(-O3 -DNDEBUG)( [^\"]*base\\.ixx\\.obj\\.modmap)"
        "\\1 -fms-extensions\\2" _m6_cdb_b "${_m5_cdb}")
    string(REGEX MATCHALL "\-fms\-extensions" _m6_hits "${_m6_cdb_b}")
    list(LENGTH _m6_hits _m6_n)
    if(NOT _m6_n EQUAL 1)
        message(FATAL_ERROR
            "module-e2e [langopt-mixed]: probe patch matched ${_m6_n} commands, wanted 1")
    endif()
    file(WRITE "${work}/m6_mixed.json" "${_m6_cdb_b}")
    ccg_run(index "${m5}" --compile-commands "${work}/m6_mixed.json"
            -o "${work}/m6" --config m6 -v)
    expect_rc(0 "langopt-mixed mixed-axis index exit")
    expect_err("config 'Release~langopt(-fms-extensions)'"
               "langopt-mixed the axis group runs as its own pass")
    expect_err("2 TU(s) degraded to tier-1"
               "langopt-mixed the cross-shape imports degrade, named")
    expect_out("failure classes: module-not-found x2"
               "langopt-mixed cross-group imports fail as the named class")

    message(STATUS "module-e2e: langopt-classic classic TU with foreign axes")
    # 参与范围（幽灵分组守卫）：拆分信号只
    #    来自模块 TU。什么都不 import 的经典 TU 从不消费
    #    std pcm，所以其记录 argv 里的白名单轴不得开
    #    组——下方向量（-fms-extensions -fshort-wchar）不匹配
    #    本库任何模块组，为它开组就是幽灵
    #    组加一个白费 std pcm 变体。
    file(WRITE "${m5}/plain_flags.cpp" [[
int plain_flags_value() { return 5; }
]])
    string(STRIP "${_m5_cdb}" _m6c_stripped)
    string(REGEX REPLACE "\\]$"
        ",{\"directory\": \"${m5}\", \"arguments\": [\"clang++\", \"-g\", \"-std=c++23\", \"-fms-extensions\", \"-fshort-wchar\", \"-c\", \"${m5}/plain_flags.cpp\"], \"file\": \"${m5}/plain_flags.cpp\"}]"
        _m6c_cdb "${_m6c_stripped}")
    if(_m6c_cdb STREQUAL "${_m6c_stripped}")
        message(FATAL_ERROR
            "module-e2e [langopt-classic]: array-close patch matched no entry")
    endif()
    file(WRITE "${work}/m6c_classic.json" "${_m6c_cdb}")
    ccg_run(index "${m5}" --compile-commands "${work}/m6c_classic.json"
            -o "${work}/m6c" --config m6c -v)
    expect_rc(0 "langopt-classic classic-axis index exit")
    expect_out("failed: 0" "langopt-classic the foreign-axis classic TU parses clean")
    expect_absent("langopt(-fms-extensions -fshort-wchar)"
        "langopt-classic no ghost group opens for the classic TU's axes")
    ccg_run(query plain_flags_value -o "${work}/m6c" --config Release)
    expect_rc(0 "langopt-classic classic TU symbol queryable")
endif()

# cl-route (cl-dialect recorded-argv route): cl 方言 cdb 的模块单元以记录编译
# 命令为权威（route_cl_module_tus 填 recorded + db_mode），扫描/BMI/解
# 析三处消费记录 argv；其中的 MSVC 模块开关（/interface /ifcOutput
# /scanDependencies /ifcSearchDir /reference）由剥离层剥除，/reference
# 名单与扫描器结果取并集注入影子 pcm。cl 方言夹具仅 Windows 运行。
if(NOT WIN32)
    message(STATUS "module-e2e: cl-route skipped (cl-dialect fixtures are "
                   "Windows-only)")
else()
    message(STATUS "module-e2e: cl-dialect recorded-argv route")
    # cl-route-gmf-header GMF 项目头夹具：m3.ixx 的全局模块片段 #include "extra.h" 只
    # 有它自己记录的 /I 能找到——合成 argv 路线丢弃模块条目的 /I（项
    # 目 include 丢失根因），记录 argv 路线自动在场。
    set(m7a "${work}/m7a")
    file(MAKE_DIRECTORY "${m7a}" "${m7a}/sub")
    file(WRITE "${m7a}/m1.ixx"
"export module m7a.one;
export int one() { return 1; }
")
    file(WRITE "${m7a}/m3.ixx"
"module;
#include \"extra.h\"
export module m7a.gmf;
export int gmf() { return EXTRA_VAL; }
")
    file(WRITE "${m7a}/sub/extra.h" "#define EXTRA_VAL 7\n")
    file(WRITE "${m7a}/use.cpp"
"import m7a.gmf;
int use_gmf() { return gmf(); }
")
    file(WRITE "${work}/m7a.json" "[
 {\"directory\": \"${m7a}\", \"file\": \"m1.ixx\", \"arguments\": [\"clang-cl\", \"--driver-mode=cl\", \"/c\", \"/EHsc\", \"/std:c++20\", \"/interface\", \"/ifcOutput\", \"${m7a}/b/m1.ifc\", \"/scanDependencies\", \"${m7a}/b/\", \"m1.ixx\"]},
 {\"directory\": \"${m7a}\", \"file\": \"m3.ixx\", \"arguments\": [\"clang-cl\", \"--driver-mode=cl\", \"/c\", \"/EHsc\", \"/std:c++20\", \"/I\", \"${m7a}/sub\", \"/interface\", \"/ifcOutput\", \"${m7a}/b/m3.ifc\", \"/scanDependencies\", \"${m7a}/b/\", \"m3.ixx\"]},
 {\"directory\": \"${m7a}\", \"file\": \"use.cpp\", \"arguments\": [\"clang-cl\", \"--driver-mode=cl\", \"/c\", \"/EHsc\", \"/std:c++20\", \"/reference\", \"m7a.one=${m7a}/b/m1.ifc\", \"/reference\", \"m7a.gmf=${m7a}/b/m3.ifc\", \"use.cpp\"]}
]")
    ccg_run(index "${m7a}" --compile-commands "${work}/m7a.json"
            -o "${work}/m7out" --config m7 -v)
    expect_rc(0 "cl-route-gmf-header cl cdb index exit")
    expect_err("recorded argv authority"
               "cl-route-gmf-header the cl route replays recorded argv")
    expect_err("0 TU(s) degraded"
               "cl-route-gmf-header every module TU survives (GMF header via the recorded /I)")
    expect_out("failed: 0" "cl-route-gmf-header the whole fixture parses clean")
    if(NOT EXISTS "${work}/m7out/m7/ccg.db")
        message(FATAL_ERROR "module-e2e [cl-route-gmf-header]: no db under ${work}/m7out/m7")
    endif()
    ccg_run(query use_gmf -o "${work}/m7out" --config m7)
    expect_rc(0 "cl-route-gmf-header GMF module consumer symbol queryable")

    # cl-route-macro-import /reference 兜底极端夹具：import 完全由宏拼出（P1689 伪预处
    # 理看不见、行首也没有 import 字样），消费只由 /reference 名单声
    # 明——名单与扫描器结果取并集，注入的路径永远是影子 pcm。
    set(m7b "${work}/m7b")
    file(MAKE_DIRECTORY "${m7b}")
    file(WRITE "${m7b}/mod.ixx"
"export module m7b.mod;
export int m7b_val() { return 9; }
")
    file(WRITE "${m7b}/use.cpp"
"#define M7B_IMP import
M7B_IMP m7b.mod;
int m7b_take() { return m7b_val(); }
")
    file(WRITE "${work}/m7b.json" "[
 {\"directory\": \"${m7b}\", \"file\": \"mod.ixx\", \"arguments\": [\"clang-cl\", \"--driver-mode=cl\", \"/c\", \"/EHsc\", \"/std:c++20\", \"/interface\", \"/ifcOutput\", \"${m7b}/b/mod.ifc\", \"/scanDependencies\", \"${m7b}/b/\", \"mod.ixx\"]},
 {\"directory\": \"${m7b}\", \"file\": \"use.cpp\", \"arguments\": [\"clang-cl\", \"--driver-mode=cl\", \"/c\", \"/EHsc\", \"/std:c++20\", \"/reference\", \"m7b.mod=${m7b}/b/mod.ifc\", \"use.cpp\"]}
]")
    # 宏拼出的 import（#define M7B_IMP import）对 text_has_import 与
    # P1689 伪预处理双盲，但 clang 前端同样不认（实测：
    # 'unknown type name 'import''——import 拼写不能来自宏展开，标准
    # 灰区实测定论）。/reference 名单兜底救不了编译器拒绝解析的
    # import；该形态的正确行为=消费 TU 诚实 parse-failed、提供者侧
    # 完好（名单驱动注入的可观测行为由 cl-route-gmf-header 覆盖：扫描器 ∪ 名单并集）。
    ccg_run(index "${m7b}" --compile-commands "${work}/m7b.json"
            -o "${work}/m7bout" --config m7 -v)
    expect_rc(0 "cl-route-macro-import macro-import cdb index exit")
    expect_out("failed: 1"
               "cl-route-macro-import the macro-hidden import fails honestly (frontend rejects it)")
    expect_out("failure classes: parse-failed x1"
               "cl-route-macro-import the failure books as parse-failed (not silent loss)")
    expect_err("unknown type name 'import'"
               "cl-route-macro-import the raw frontend diagnostic surfaces")
    ccg_run(query m7b_val -o "${work}/m7bout" --config m7)
    expect_rc(0 "cl-route-macro-import provider-side module symbol queryable")

    # cl-route-dry-run 干跑计划 + 诊断面：好/坏（语法错误）单元混合。--dry-run 如今
    # 在一切引擎触点之前短路（毫秒级计划，不跑 clang-cl 扫描垫片——
    # 垫片保真断言随该语义退役）；坏单元的真实诊断在真跑侧原样透传。
    # 注：扫描器 rc!=0 触发 "(scanner:" 首行透传的形态在 e2e 未能稳定
    # 构造（clang-scan-deps 对 flag 毒药/非法 std 值均宽容），该文案
    # 透传已实现、留待真实世界的扫描器失败样本；诚实记账。
    set(m7c "${work}/m7c")
    file(MAKE_DIRECTORY "${m7c}")
    file(WRITE "${m7c}/good.ixx"
"export module m7c.good;
export int good() { return 3; }
")
    file(WRITE "${m7c}/bad.ixx"
"export module m7c.bad;
export int broken( {
")
    file(WRITE "${work}/m7c.json" "[
 {\"directory\": \"${m7c}\", \"file\": \"good.ixx\", \"arguments\": [\"clang-cl\", \"--driver-mode=cl\", \"/c\", \"/EHsc\", \"/std:c++20\", \"/interface\", \"/ifcOutput\", \"${m7c}/b/good.ifc\", \"/scanDependencies\", \"${m7c}/b/\", \"good.ixx\"]},
 {\"directory\": \"${m7c}\", \"file\": \"bad.ixx\", \"arguments\": [\"clang-cl\", \"--driver-mode=cl\", \"/c\", \"/EHsc\", \"/std:c++20\", \"/interface\", \"/ifcOutput\", \"${m7c}/b/bad.ifc\", \"/scanDependencies\", \"${m7c}/b/\", \"bad.ixx\"]}
]")
    ccg_run(index "${m7c}" --compile-commands "${work}/m7c.json"
            -o "${work}/m7cdry" --config m7 -v --dry-run)
    expect_rc(0 "cl-route-dry-run dry-run exit")
    expect_out("plan: 2 of 2 TUs" "cl-route-dry-run dry-run plans both module TUs")
    expect_out("dry run" "cl-route-dry-run dry-run prints the plan line")
    if(EXISTS "${work}/m7cdry/m7/ccg.db")
        message(FATAL_ERROR
            "module-e2e [cl-route-dry-run]: --dry-run created a database file")
    endif()
    ccg_run(index "${m7c}" --compile-commands "${work}/m7c.json"
            -o "${work}/m7creal" --config m7 -v)
    expect_rc(0 "cl-route-dry-run real run exit")
    expect_err("0 TU(s) degraded"
               "cl-route-dry-run the real run degrades nothing (bad unit keeps its rule)")
    expect_out("failed: 1" "cl-route-dry-run the syntax-broken unit fails honestly")
    expect_out("failure classes: parse-failed x1"
               "cl-route-dry-run the failure books as parse-failed")
    expect_err("expected expression"
               "cl-route-dry-run the raw frontend diagnostic surfaces verbatim")

    # cl-route-scan-first/cl-route-compile-first 真实记录形态变体（真实 MSVC 工程语料实录，MSVC 14.51，与
    # 真实工程实录文物同形）：MSBuild 给 .ixx 发扫描/编译两趟调用——cdb 每文件
    # 双条，值全部裸露（/ifcOutput 后随旗标而非路径）、/TP 在场（把一
    # 切输入当 C++ 源码）；消费 .cpp 的记录条目无任何模块旗标（MSBuild
    # 靠隐式 ifc 搜索，不经命令行）。双条下路由取每文件首条为权威：
    # 两种条目顺序（cl-route-scan-first 扫描趟在前=MSBuild 实录序、cl-route-compile-first 编译趟在前）
    # 都必须 failed: 0——剥离层把两趟剥成同一重放命令，选条语义对顺
    # 序不敏感。拼装层双 -o 真凶形态（引擎路径双份 × /TP 双输入 +
    # -clang:-o）在此形态下一字不差触发过 cannot specify -o when
    # generating multiple output files，本变体是它的全链回归闸门。
    set(m7d "${work}/m7d")
    file(MAKE_DIRECTORY "${m7d}")
    file(WRITE "${m7d}/mod.ixx"
"export module m7d.mod;
export int m7d_val() { return 11; }
")
    file(WRITE "${m7d}/use.cpp"
"import m7d.mod;
int m7d_take() { return m7d_val(); }
")
    file(WRITE "${work}/m7d.json" "[
 {\"directory\": \"${m7d}\", \"file\": \"mod.ixx\", \"arguments\": [\"clang-cl\", \"--driver-mode=cl\", \"/c\", \"/EHsc\", \"/std:c++20\", \"/interface\", \"/ifcOutput\", \"/scanDependencies\", \"/TP\", \"mod.ixx\"]},
 {\"directory\": \"${m7d}\", \"file\": \"mod.ixx\", \"arguments\": [\"clang-cl\", \"--driver-mode=cl\", \"/c\", \"/EHsc\", \"/std:c++20\", \"/interface\", \"/ifcOutput\", \"/sourceDependencies\", \"/TP\", \"mod.ixx\"]},
 {\"directory\": \"${m7d}\", \"file\": \"use.cpp\", \"arguments\": [\"clang-cl\", \"--driver-mode=cl\", \"/c\", \"/EHsc\", \"/std:c++20\", \"/TP\", \"use.cpp\"]}
]")
    ccg_run(index "${m7d}" --compile-commands "${work}/m7d.json"
            -o "${work}/m7dout" --config m7 -v)
    expect_rc(0 "cl-route-scan-first scan-pass-first index exit")
    expect_err("recorded argv authority"
               "cl-route-scan-first the cl route replays recorded argv")
    expect_err("0 TU(s) degraded"
               "cl-route-scan-first the dual-entry recording degrades nothing")
    expect_out("failed: 0" "cl-route-scan-first scan-pass-first dual-entry BMI parses clean")
    expect_out("imports: 1" "cl-route-scan-first the consumer's import lands in the db")
    ccg_run(query m7d_take -o "${work}/m7dout" --config m7)
    expect_rc(0 "cl-route-scan-first consumer symbol queryable through the BMI")
    ccg_run(query m7d_val -o "${work}/m7dout" --config m7)
    expect_rc(0 "cl-route-scan-first interface symbol queryable through the BMI")

    # cl-route-compile-first 同语料、条目顺序对调（编译趟在前）：选条语义钉死——首条权威
    # 无论选中哪一趟，剥离后命令等价，两种顺序同绿。
    file(WRITE "${work}/m7e.json" "[
 {\"directory\": \"${m7d}\", \"file\": \"mod.ixx\", \"arguments\": [\"clang-cl\", \"--driver-mode=cl\", \"/c\", \"/EHsc\", \"/std:c++20\", \"/interface\", \"/ifcOutput\", \"/sourceDependencies\", \"/TP\", \"mod.ixx\"]},
 {\"directory\": \"${m7d}\", \"file\": \"mod.ixx\", \"arguments\": [\"clang-cl\", \"--driver-mode=cl\", \"/c\", \"/EHsc\", \"/std:c++20\", \"/interface\", \"/ifcOutput\", \"/scanDependencies\", \"/TP\", \"mod.ixx\"]},
 {\"directory\": \"${m7d}\", \"file\": \"use.cpp\", \"arguments\": [\"clang-cl\", \"--driver-mode=cl\", \"/c\", \"/EHsc\", \"/std:c++20\", \"/TP\", \"use.cpp\"]}
]")
    ccg_run(index "${m7d}" --compile-commands "${work}/m7e.json"
            -o "${work}/m7eout" --config m7 -v)
    expect_rc(0 "cl-route-compile-first compile-pass-first index exit")
    expect_err("0 TU(s) degraded"
               "cl-route-compile-first the flipped dual-entry recording degrades nothing")
    expect_out("failed: 0" "cl-route-compile-first compile-pass-first dual-entry BMI parses clean")
    ccg_run(query m7d_take -o "${work}/m7eout" --config m7)
    expect_rc(0 "cl-route-compile-first consumer symbol queryable through the BMI")
endif()

message(STATUS "module-e2e: all gates passed")
