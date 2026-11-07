# LLVM 按库引擎 e2e：
#   S1  ccg config 往返 + 重建提示 + 退出码
#   S2  显式配置路径加载同一个 msys2 libclang.dll（闭包
#       拷进临时目录）——解析结果与默认引擎一致、
#       版本串被记录
#   S3  另一个真实引擎（VS Clang 组件，在场时）强制
#       全量重索引并更新记录的 meta
#   S4  同路径不同内容（临时 dll 换成 VS 的）
#       仍强制全量——版本才是身份、不只是路径
#   S5  引擎未变 -> 普通增量、不强制
#   S6  不可加载/缺失引擎路径 -> 可操作错误、退出码 1
#
# 一个文件的两副面孔：
#   从 tests/CMakeLists.txt INCLUDE 进来 -> 注册 ccg_llvm_e2e ctest
#   （经 $ENV{ProgramFiles} 探测第二份真实 libclang、绝不
#   用机器绝对字面量——可移植性扫描会读本文件）。
#   经 cmake -P llvm-e2e.cmake <ccg.exe> <fixture-dir> <work-dir>
#   <msys-bin> <vs-libclang-or-dash> 运行。

if(NOT CMAKE_SCRIPT_MODE_FILE)
    set(_vs_libclang "")
    # 'ProgramFiles(x86)' 的名字带括号：经变量
    # 构造环境引用，foreach 实参才可解析。
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
    # 原生 GLOB 结果可能带反斜杠；脚本模式宏
    # 实参传递扛不住（非法 '\P' 转义）。
    if(NOT _vs_libclang STREQUAL "")
        file(TO_CMAKE_PATH "${_vs_libclang}" _vs_libclang)
    endif()
    if(_vs_libclang STREQUAL "")
        message(STATUS "llvm-e2e: no VS Clang libclang found - the "
                       "second-engine scenarios run against the copy "
                       "engine only")
    else()
        message(STATUS "llvm-e2e: second engine ${_vs_libclang}")
    endif()
    add_test(NAME ccg_llvm_e2e
             COMMAND ${CMAKE_COMMAND} -P
                     "${CMAKE_CURRENT_SOURCE_DIR}/llvm-e2e.cmake"
                     $<TARGET_FILE:ccg>
                     "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sample1"
                     "${CMAKE_BINARY_DIR}/test-llvm-e2e"
                     "${CCG_LIBCLANG_BIN}"
                     "${_vs_libclang}")
    ccg_test_path_mods(_llvm_mods)
    set_tests_properties(ccg_llvm_e2e PROPERTIES
        ENVIRONMENT_MODIFICATION "${_llvm_mods}"
        TIMEOUT 900)
    return()
endif()

if(NOT DEFINED CMAKE_ARGV7)
    message(FATAL_ERROR
        "llvm-e2e.cmake: expected <ccg.exe> <fixture-dir> <work-dir> "
        "<msys-bin> <vs-libclang-or-dash>")
endif()
set(exe      "${CMAKE_ARGV3}")
set(fixture  "${CMAKE_ARGV4}")
# ccg 把根规范化成绝对形态；比较前同样处理。
cmake_path(ABSOLUTE_PATH fixture NORMALIZE)
set(work     "${CMAKE_ARGV5}")
set(msysbin  "${CMAKE_ARGV6}")
set(vsdll    "${CMAKE_ARGV7}")
set(engA     "${work}/engA")

file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}")

# 库存的是规范化根（正斜杠、小写盘符）；
# 从夹具实参推导而非拼机器绝对
# 路径（可移植性闸门）。
string(SUBSTRING "${fixture}" 1 -1 _fixture_tail)
string(SUBSTRING "${fixture}" 0 1 _fixture_drive)
string(TOLOWER "${_fixture_drive}" _fixture_drive)
set(fixture_norm "${_fixture_drive}${_fixture_tail}")

# 运行/断言宏组（ccg_run/expect_rc/expect_out/expect_err/
# expect_no）收纳在同目录 ccg-harness.cmake，供各闸门脚本共享。
include("${CMAKE_CURRENT_LIST_DIR}/ccg-harness.cmake")

# 仅本次调用把 CCG_LLVM_ROOT 设为 `root` 运行 ccg
#（旧值事后还原）；环境其余部分——
# 尤其是 ccg 自身运行期 DLL 解析所在的 PATH——原样透传。
macro(ccg_run_env_root root)
    set(_saved_ccg_root "$ENV{CCG_LLVM_ROOT}")
    set(ENV{CCG_LLVM_ROOT} "${root}")
    ccg_run(${ARGN})
    if(_saved_ccg_root STREQUAL "")
        unset(ENV{CCG_LLVM_ROOT})
    else()
        set(ENV{CCG_LLVM_ROOT} "${_saved_ccg_root}")
    endif()
endmacro()

# 从 ccg --json 载荷提取 "字段":"值" 字符串对。
macro(extract_version field json label)
    string(REGEX MATCH "\"${field}\": *\"[^\"]*\"" _ver_match "${json}")
    if(NOT _ver_match)
        message(FATAL_ERROR "llvm-e2e [${label}]: no '${field}' in ${json}")
    endif()
    set(_extracted "${_ver_match}")
endmacro()

# 提取数值对象字段的 "total"（"symbols": {"total": N}）。
macro(extract_total field json label)
    string(REGEX MATCH "\"${field}\": *\{[^}]*\"total\": *([0-9]+)" _tot_match "${json}")
    if(NOT _tot_match)
        message(FATAL_ERROR "llvm-e2e [${label}]: no '${field}' total in ${json}")
    endif()
    set(_extracted "${CMAKE_MATCH_1}")
endmacro()

message(STATUS "llvm-e2e: S0 baseline index (default engine)")
ccg_run(index "${fixture}" -o "${work}" --config main --jobs 2)
expect_rc(0 "S0 index")
expect_out("failed: 0" "S0 index")
expect_no("FULL re-index" "S0 fresh db is not an engine-change force")

message(STATUS "llvm-e2e: S1 config round-trip + advisories")
ccg_run(config get -o "${work}" --config main)
expect_rc(0 "S1 get")
expect_out("llvm-path: (unset - default resolution)" "S1 get unset")
expect_out("engine:" "S1 get engine")
expect_out("version:" "S1 get version")
ccg_run(config get --json -o "${work}" --config main)
expect_rc(0 "S1 get json")
expect_out("\"engine\":" "S1 get json engine")
ccg_run(config get -o "${work}" --config missing)
expect_rc(1 "S1 get missing db")
ccg_run(config frobnicate -o "${work}" --config main)
expect_rc(2 "S1 unknown action")
ccg_run(config set no-such-key x -o "${work}" --config main)
expect_rc(2 "S1 unknown key")
ccg_run(config unset -o "${work}" --config main)
expect_rc(2 "S1 unset without key")
ccg_run(config set llvm-path "${work}/definitely-not-here")
expect_rc(1 "S1 set nonexistent path")
ccg_run(config set llvm-path "${fixture}" -o "${work}" --config main)
expect_rc(0 "S1 set")
expect_err("ccg index --full" "S1 set advisory names the rebuild")
# 提示携带规范化路径（小写盘符），
# 所以匹配夹具路径中大小写稳定的内部片段。
expect_err("tests/fixtures/sample1" "S1 set advisory names the path")
ccg_run(config get -o "${work}" --config main)
expect_out("llvm-path: ${fixture_norm}" "S1 get round-trips the normalized path")
ccg_run(config set llvm-path "${fixture}" -o "${work}" --config main)
expect_rc(0 "S1 set same again")
expect_err("no change" "S1 same value reports no change")
expect_no("ccg index --full" "S1 same value prints no advisory")
ccg_run(config unset llvm-path -o "${work}" --config main)
expect_rc(0 "S1 unset")
expect_err("ccg index --full" "S1 unset advisory names the rebuild")
ccg_run(config get -o "${work}" --config main)
expect_out("llvm-path: (unset - default resolution)" "S1 unset restores default")

message(STATUS "llvm-e2e: S2 explicit path, same engine, identical results")
file(MAKE_DIRECTORY "${engA}")
foreach(_name libclang.dll libc++.dll zlib1.dll libzstd.dll)
    if(EXISTS "${msysbin}/${_name}")
        file(COPY_FILE "${msysbin}/${_name}" "${engA}/${_name}")
    endif()
endforeach()
foreach(_pat libLLVM-*.dll libxml2-*.dll libffi-*.dll libiconv-*.dll)
    file(GLOB _hits "${msysbin}/${_pat}")
    foreach(_h IN LISTS _hits)
        cmake_path(GET _h FILENAME _fn)
        file(COPY_FILE "${_h}" "${engA}/${_fn}")
    endforeach()
endforeach()
ccg_run(index "${fixture}" -o "${work}" --config ref --jobs 2)
expect_rc(0 "S2 reference index")
ccg_run(config set llvm-path "${engA}/libclang.dll" -o "${work}" --config main)
expect_rc(0 "S2 set explicit file path")
ccg_run(index "${fixture}" -o "${work}" --config main --jobs 2)
expect_rc(0 "S2 index via explicit path")
expect_err("forcing a FULL re-index" "S2 path delta forces full")
expect_out("failed: 0" "S2 explicit engine parses clean")
# 解析结果一致：symbols/edges/files 总数等于
# 默认引擎参照库。
ccg_run(stats --json -o "${work}" --config main)
expect_rc(0 "S2 stats")
extract_total(symbols "${out}" "S2 stats")
set(_a_syms "${_extracted}")
ccg_run(stats --json -o "${work}" --config ref)
expect_rc(0 "S2 ref stats")
extract_total(symbols "${out}" "S2 ref stats")
if(NOT _a_syms STREQUAL "${_extracted}")
    message(FATAL_ERROR
        "llvm-e2e [S2 identical results]: symbols differ - explicit ${_a_syms} vs default ${_extracted}")
endif()
ccg_run(config get --json -o "${work}" --config main)
expect_rc(0 "S2 get json")
extract_version(version "${out}" "S2 get json")
set(_a_ver "${_extracted}")
ccg_run(config get --json -o "${work}" --config ref)
expect_rc(0 "S2 ref get json")
extract_version(version "${out}" "S2 ref get json")
if(NOT _a_ver STREQUAL "${_extracted}")
    message(FATAL_ERROR
        "llvm-e2e [S2 same engine]: version differs - ${_a_ver} vs ${_extracted}")
endif()
message(STATUS "llvm-e2e: S2 recorded version ${_a_ver}")

message(STATUS "llvm-e2e: S5a engine unchanged -> incremental")
ccg_run(index "${fixture}" -o "${work}" --config main --jobs 2)
expect_rc(0 "S5a index")
expect_out("skipped: all (3)" "S5a incremental skips")
expect_no("forcing a FULL re-index" "S5a no engine-change force")

if(vsdll STREQUAL "-" OR NOT EXISTS "${vsdll}")
    message(STATUS
        "llvm-e2e: S3/S4 SKIPPED - no second real libclang on this machine "
        "(documented; S2 already proves explicit-path loading)")
else()
    message(STATUS "llvm-e2e: S3 different engine forces full")
    cmake_path(GET vsdll PARENT_PATH _vs_dir)
    ccg_run(config set llvm-path "${_vs_dir}" -o "${work}" --config main)
    expect_rc(0 "S3 set vs engine")
    ccg_run(index "${fixture}" -o "${work}" --config main --jobs 2)
    expect_rc(0 "S3 index")
    expect_err("forcing a FULL re-index" "S3 engine delta forces full")
    # 另一个引擎是另一个世界观：sample1 cdb 带
    # 绝对 msys2 libc++ -I 目录，MSVC 目标的 VS 引擎解析
    # 那些 TU 的方式不同——所以本场景断言切换与
    #    记录的身份、而非干净解析。
    expect_out("symbols:" "S3 vs engine indexes the fixture")
    ccg_run(config get --json -o "${work}" --config main)
    expect_rc(0 "S3 get json")
    extract_version(version "${out}" "S3 get json")
    if(_extracted STREQUAL _a_ver)
        message(FATAL_ERROR
            "llvm-e2e [S3 different engine]: version unchanged (${_extracted})")
    endif()
    message(STATUS "llvm-e2e: S3 second engine ${_extracted}")

    message(STATUS "llvm-e2e: S4a back to the scratch engine (path delta)")
    ccg_run(config set llvm-path "${engA}/libclang.dll" -o "${work}" --config main)
    expect_rc(0 "S4a set scratch path")
    ccg_run(index "${fixture}" -o "${work}" --config main --jobs 2)
    expect_rc(0 "S4a index")
    expect_err("forcing a FULL re-index" "S4a path delta forces full")

    message(STATUS "llvm-e2e: S4b same path, different content forces full")
    file(COPY_FILE "${vsdll}" "${engA}/libclang.dll")
    ccg_run(index "${fixture}" -o "${work}" --config main --jobs 2)
    expect_rc(0 "S4b index")
    expect_err("forcing a FULL re-index" "S4b content delta forces full")
    expect_out("symbols:" "S4b replaced engine indexes the fixture")

    message(STATUS "llvm-e2e: S5b engine unchanged again -> incremental")
    ccg_run(index "${fixture}" -o "${work}" --config main --jobs 2)
    expect_rc(0 "S5b index")
    expect_out("mode: incremental" "S5b stays incremental")
    expect_no("forcing a FULL re-index" "S5b no engine-change force")

    message(STATUS "llvm-e2e: S5c back to default resolution forces full once")
    ccg_run(config unset llvm-path -o "${work}" --config main)
    expect_rc(0 "S5c unset")
    ccg_run(index "${fixture}" -o "${work}" --config main --jobs 2)
    expect_rc(0 "S5c index")
    expect_err("forcing a FULL re-index" "S5c default engine differs")
    ccg_run(index "${fixture}" -o "${work}" --config main --jobs 2)
    expect_rc(0 "S5c index 2")
    expect_out("skipped: all (3)" "S5c incremental after re-record")
endif()

message(STATUS "llvm-e2e: S6 unloadable/missing engine paths")
file(MAKE_DIRECTORY "${work}/empty-root")
ccg_run(config set llvm-path "${work}/empty-root" -o "${work}" --config main)
expect_rc(0 "S6 set empty dir")
ccg_run(index "${fixture}" -o "${work}" --config main --jobs 2)
expect_rc(1 "S6 index with no library")
expect_err("libclang" "S6 error names the library")
expect_err("llvm-path" "S6 error names the config knob")
file(MAKE_DIRECTORY "${work}/fake-root")
file(WRITE "${work}/fake-root/fake.dll" "not a real dll")
ccg_run(config set llvm-path "${work}/fake-root/fake.dll" -o "${work}" --config main)
expect_rc(0 "S6 set fake dll file")
ccg_run(index "${fixture}" -o "${work}" --config main --jobs 2)
expect_rc(1 "S6 index with fake dll")
expect_err("cannot load LLVM engine" "S6 load failure is actionable")
expect_err("llvm-path" "S6 load error names the config knob")
ccg_run(config unset llvm-path -o "${work}" --config main)
expect_rc(0 "S6 restore default")

# S7：仅环境变量的标记根在常规编译数据库路径的引擎解析
# 中仍大声失败（标记工具链是异根下
# 驱动形状的非可执行文件）。
message(STATUS "llvm-e2e: S7 env-only marker root fails at engine resolution")
set(marker "${work}/marker-root")
file(MAKE_DIRECTORY "${marker}/bin")
file(WRITE "${marker}/bin/clang++.exe" "marker")
ccg_run_env_root("${marker}" index "${fixture}" -o "${work}" --config marker
                 --jobs 2)
expect_rc(1 "S7 env-only marker root fails loudly")
expect_err("LLVM engine resolution failed"
           "S7 fails at engine resolution")
expect_err("CCG_LLVM_ROOT" "S7 error names the env knob")

message(STATUS "llvm-e2e: all scenarios green")
