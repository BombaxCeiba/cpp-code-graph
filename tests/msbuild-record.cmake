# 记录模式 e2e 闸门（各运行显式 --record——默认通道已是工程求值，
# 记录管线是显式调用的对照真值臂/测试设施）：
#   捕获等价闸：记录模式 cdb == --full-build cdb（同一
#       工程副本、解析后 JSON 相等；/Fo /Fp /Yc /Yu /Fd
#       被适配器丢弃，所以沙盒 IntDir 差异不可见）。
#   路径带空格闸："path spaces/my demo" 目录下的工程在
#       默认记录模式下捕获 3 个 TU（注入的 /p:
#       值继承空格；内嵌引号令牌会以
#       MSB4184 失败并得到 0 TU）。
#   完整桩面闸 + 生成器 TU 完整性 + 零泄漏：
#       面夹具驱动整个被桩化的家族（PCH、rc、真
#       midl、MASM、CustomBuild gen.cpp、静态库 lib.exe、link）——
#       构建图跑起来零真实 CL 编译、生成的
#       gen.cpp 是被捕获 TU、桩产物消失（零磁盘
#       契约：被桩编译类输出不得存在于
#       任何地方）、真 midl/masm 输出仍落沙盒、
#       且无沙盒泄漏警告（gen.cpp 是声明过的
#       生成源、白名单内）。
#   沙盒泄漏对抗闸：CustomBuild 逃逸（leaked.txt、真实
#       生成器写入）以具名沙盒泄漏警告浮出；
#       硬编码 cl /Fo 与 Link OutputFile 暗门在零磁盘
#       契约下什么都不产（无泄漏行、金丝雀沉默）；
#       --json 携带 "mode":"record" 与 sandbox_leaks 清单；退出
#       码保持 0。
#   工具集版本感知闸：按配置的 VCToolsVersion 钉定经配置感知
#       的 getProperty 解析（Release 钉 14.44，仅带
#       -p:Configuration 时钉定可见），且
#       混合 .sln 在注册表携带每工程 cl 解析。
#       两者在无钉定工具集时跳过（本机夹具）。
#
# 一个文件的两副面孔：
#   从 tests/CMakeLists.txt 注册 -> ccg_msbuild_record ctest。
#   经 `cmake -P msbuild-record.cmake <ccg.exe> <fixture-dir> <work-dir>` 运行。
# 无 VS 时 SKIPPED（ctest SKIP_REGULAR_EXPRESSION 让无 VS 的 CI
# 保持绿），同 ccg_msbuild_e2e。

if(NOT DEFINED CMAKE_ARGV5)
    message(FATAL_ERROR
        "msbuild-record.cmake: expected <ccg.exe> <fixture-dir> <work-dir>")
endif()

set(exe     "${CMAKE_ARGV3}")
set(fixture "${CMAKE_ARGV4}")
set(work    "${CMAKE_ARGV5}")

# 运行/断言宏组（ccg_run/expect_rc/expect_out/expect_err/
# expect_absent/fail/py_run）收纳在同目录 ccg-harness.cmake，
# 供各闸门脚本共享；expect_absent 断言 stderr 上不得出现探针。
include("${CMAKE_CURRENT_LIST_DIR}/ccg-harness.cmake")

# ------------------------------------------------- VS 存在性（跳过闸门）--
set(_pf86 "")
execute_process(COMMAND "${CMAKE_COMMAND}" -E environment
    OUTPUT_VARIABLE _env_block RESULT_VARIABLE _env_rc)
if(_env_rc EQUAL 0)
    string(PREPEND _env_block "\n")
    string(FIND "${_env_block}" "\nProgramFiles(x86)=" _at)
    if(_at GREATER -1)
        math(EXPR _val_at "${_at} + 19")
        string(SUBSTRING "${_env_block}" ${_val_at} -1 _tail)
        string(REGEX REPLACE "^([^\r\n]*).*" "\\1" _pf86 "${_tail}")
    endif()
endif()
set(_vswhere "")
if(NOT _pf86 STREQUAL "")
    set(_vswhere "${_pf86}/Microsoft Visual Studio/Installer/vswhere.exe")
endif()
set(_msbuild "")
if(NOT _vswhere STREQUAL "" AND EXISTS "${_vswhere}")
    execute_process(COMMAND "${_vswhere}" -latest -products *
                    -requires Microsoft.Component.MSBuild
                    -find MSBuild/**/Bin/MSBuild.exe
        RESULT_VARIABLE _vw_rc
        OUTPUT_VARIABLE _vw_out
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_vw_rc EQUAL 0)
        set(_msbuild "${_vw_out}")
    endif()
endif()
if(_msbuild STREQUAL "")
    message(STATUS
        "*** SKIPPED: no Visual Studio / MSBuild found - the ccg msbuild "
        "the ccg msbuild record-mode gates need it")
    return()
endif()
set(_vs_root "")
execute_process(COMMAND "${_vswhere}" -latest -products *
                -property installationPath
    RESULT_VARIABLE _vs_rc
    OUTPUT_VARIABLE _vs_out
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(_vs_rc EQUAL 0 AND NOT _vs_out STREQUAL "")
    string(REPLACE "\r" "" _vs_out "${_vs_out}")
    string(REPLACE "\n" ";" _vs_lines "${_vs_out}")
    list(GET _vs_lines 0 _vs_root)
endif()

file(REMOVE_RECURSE "${work}")

# ---------------------------------------------------- 捕获等价闸 -------
# 同一工程副本上记录 cdb 对 --full-build cdb：解析后
# JSON 相等（条目序、文件、目录、完整 argv）。记录运行
# 用默认布局（.ccg）；全构建运行导出到别处。
file(COPY "${fixture}/msbdemo" DESTINATION "${work}/equiv")
ccg_run(msbuild "${work}/equiv/msbdemo/msbdemo.sln"
        --config Debug --platform x64 --export-only --record)
expect_rc(0 "capture-equivalence record run")
expect_out("3 TUs, record" "capture-equivalence record summary")
ccg_run(msbuild "${work}/equiv/msbdemo/msbdemo.sln"
        --config Debug --platform x64 --full-build --export-only
        -o "${work}/equiv-full")
expect_rc(0 "capture-equivalence full-build run")
expect_out("3 TUs, full-build" "capture-equivalence full-build summary")
set(py_args "${work}/equiv/msbdemo/.ccg/Debug-x64/compile_commands.json"
            "${work}/equiv-full/Debug-x64/compile_commands.json")
py_run("capture-equivalence cdb equality" [==[
import json, sys
rec = json.load(open(sys.argv[1], encoding="utf-8"))
full = json.load(open(sys.argv[2], encoding="utf-8"))
assert len(rec) == 3 and len(full) == 3, (len(rec), len(full))
assert rec == full, "record-mode cdb differs from the --full-build cdb"
print("equiv-ok")
]==])

# --------------------------------------------------- 路径带空格闸 -------
# 工程路径带空格的端到端：记录模式注入 /p:
# IntDir/OutDir 与值继承空格的桩 ToolPaths；
# 令牌必须无引号抵达 MSBuild（内嵌引号
# 拼写会把引号漏进属性值 -> MSB4184 "路径含
# 非法字符" -> 0 TU -> rc=1；此处其它夹具都
# 无空格，这正是本闸门存在的原因）。
file(COPY "${fixture}/msbdemo" DESTINATION "${work}/path spaces")
file(RENAME "${work}/path spaces/msbdemo" "${work}/path spaces/my demo")
ccg_run(msbuild "${work}/path spaces/my demo/msbdemo.sln"
        --config Debug --platform x64 --export-only --record)
expect_rc(0 "spaces-path record run")
expect_out("3 TUs, record" "spaces-path summary")
set(py_args "${work}/path spaces/my demo/.ccg/Debug-x64/compile_commands.json")
py_run("spaces-path cdb" [==[
import json, sys
entries = json.load(open(sys.argv[1], encoding="utf-8"))
assert len(entries) == 3, len(entries)
for e in entries:
    f = e["file"].replace(chr(92), "/").lower()
    assert "my demo" in f, f
print("spaces-cdb-ok")
]==])

# ----------------------------------------------- 完整桩面与零泄漏闸 -------
# 整个桩家族 + 生成器 TU 完整性 + 零泄漏。
file(COPY "${fixture}/surface" DESTINATION "${work}/surface-run")
ccg_run(msbuild "${work}/surface-run/surface/bsurface.sln"
        --config Debug --platform x64 --record)
expect_rc(0 "stub-family surface run")
expect_out("indexing 4 TUs" "stub-family tu count")
expect_out("failed: 0" "stub-family probes clean")
expect_absent("sandbox-leak" "stub-family zero leak")
set(py_args "${work}/surface-run/surface/.ccg/Debug-x64/compile_commands.json")
py_run("generator TU captured" [==[
import json, sys
entries = json.load(open(sys.argv[1], encoding="utf-8"))
files = [e["file"].replace(chr(92), "/").lower() for e in entries]
for want in ("/gen.cpp", "/pch.cpp", "/main.cpp", "/libsub.cpp"):
    assert any(f.endswith(want) for f in files), (want, files)
print("gen-tu-ok")
]==])
# 零磁盘契约：被桩编译类产物（PCH、res、obj、
# exe、lib）不得存在于任何地方。仍落沙盒的
# 例外：真 midl 输出（obj.h）与 mul.obj——当前 MSVC 的
# MASM 目标按绝对工具集路径解析 ml64.exe，PATH
# 前置拦不住它、ml64 真跑（既有现象；其
# .obj 是 MSBuild 管理的产物、重定向进沙盒）。
set(_sb "${work}/surface-run/surface/.ccg/Debug-x64/msbuild-sandbox")
foreach(_f "int/bsurface.pch" "int/bsurface.res" "int/gen.obj"
           "out/bsurface.exe" "out/libsub.lib")
    if(EXISTS "${_sb}/${_f}")
        fail("zero-disk stubs" "${_f} must not exist (stubs write nothing)")
    endif()
endforeach()
foreach(_f "int/obj.h" "int/mul.obj")
    if(NOT EXISTS "${_sb}/${_f}")
        fail("sandbox products" "${_f} missing from the sandbox")
    endif()
endforeach()
ccg_run(query gen -o "${work}/surface-run/surface/.ccg" --config Debug-x64)
expect_rc(0 "generated-TU symbol indexed")

# ------------------------------------------------- 沙盒泄漏对抗闸 -------
# 对抗性泄漏逃逸：具名警告、JSON 字段、退出码 0。零磁盘
# 契约下硬编码 cl /Fo 与 Link OutputFile 暗门
# 什么都不产（无新文件、金丝雀沉默——只有真实写入算数）；
# CustomBuild 步骤真跑，所以 leaked.txt 保持具名泄漏。
# --json 机器行与 MSBuild 噪声共享 stdout（继承
# 句柄）；它是最后一行，捕获进文件供 python 检查。
file(COPY "${fixture}/adversarial" DESTINATION "${work}/adv-run")
execute_process(COMMAND "${exe}" msbuild
                        "${work}/adv-run/adversarial/adv.sln"
                        --config Debug --platform x64 --export-only --json
                        --record -o "${work}/adv-out"
    RESULT_VARIABLE rc
    OUTPUT_FILE "${work}/adv-run/stdout.txt"
    ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "adversarial-leak exit")
expect_err("sandbox-leak: leak/leaked.txt" "adversarial-leak leaked.txt")
expect_absent("sandbox-leak: hard" "adversarial-leak no hardcoded-output (zero-disk)")
expect_absent("sandbox-leak: hardobj" "adversarial-leak no hardcoded-obj (zero-disk)")
if(EXISTS "${work}/adv-run/adversarial/hard.exe")
    fail("adversarial zero-disk stubs" "hard.exe must not be written (stubs write nothing)")
endif()
if(EXISTS "${work}/adv-run/adversarial/hardobj/adv.obj")
    fail("adversarial zero-disk stubs" "hardobj/adv.obj must not be written (stubs write nothing)")
endif()
set(py_args "${work}/adv-run/stdout.txt")
py_run("adversarial-leak json field" [==[
import json, sys
last = None
for line in open(sys.argv[1], encoding="utf-8", errors="replace"):
    if line.strip():
        last = line.strip()
assert last and last.startswith("{"), repr(last)
obj = json.loads(last)
assert obj.get("mode") == "record", obj
leaks = obj.get("sandbox_leaks")
assert isinstance(leaks, list) and len(leaks) == 1, obj
assert "leak/leaked.txt" in leaks, leaks
print("adv-json-ok")
]==])

# ------------------------------------------------ 工具集版本感知闸 -------
# 版本感知。工具集可用性跳过模式同 e2e 的
# 混合工具集闸门：钉定工具集是本机夹具。
set(_vs_cmake "${_vs_root}")
if(NOT _vs_cmake STREQUAL "")
    file(TO_CMAKE_PATH "${_vs_cmake}" _vs_cmake)
endif()
# 从 cl.exe 的 GLOB 命中路径解析 MSVC 完整版本号（MSVC
# 目录名的下一段）；同一大版本下命中多个时取字典序最高
#（完整版本号末段等长，字典序即版本序），同 e2e 闸门。
function(msvc_toolset_version hits out_var)
    set("${out_var}" "" PARENT_SCOPE)
    if("${hits}" STREQUAL "")
        return()
    endif()
    list(SORT hits)
    list(GET hits -1 _hit)
    string(REPLACE "/" ";" _parts "${_hit}")
    list(FIND _parts "MSVC" _at)
    if(_at LESS 0)
        return()
    endif()
    math(EXPR _vat "${_at} + 1")
    list(GET _parts "${_vat}" _ver)
    if(NOT _ver MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
        return()
    endif()
    set("${out_var}" "${_ver}" PARENT_SCOPE)
endfunction()
file(GLOB _cl_1444
     "${_vs_cmake}/VC/Tools/MSVC/14.44.*/bin/Host*/x64/cl.exe")
msvc_toolset_version("${_cl_1444}" _ver_1444)
if(_ver_1444 STREQUAL "")
    message(STATUS
        "msbuild-record: pinned-toolset gate skipped (needs MSVC "
        "14.44)")
else()
    file(COPY "${fixture}/vcfgpin" DESTINATION "${work}/vcfg-run")
    # MSBuild 不接受部分版本号的 VCToolsVersion（MSB8070），
    # 把该大版本下已安装的完整工具集替换进构建副本。
    set(_vcfg "${work}/vcfg-run/vcfgpin/vcfgpin.vcxproj")
    file(READ "${_vcfg}" _vcfg_text)
    string(REGEX REPLACE
        "(<VCToolsVersion>)14\\.44[^<]*(</VCToolsVersion>)"
        "\\1${_ver_1444}\\2" _vcfg_new "${_vcfg_text}")
    if(_vcfg_new STREQUAL _vcfg_text)
        fail("version-pin" "could not pin the installed toolset into vcfgpin.vcxproj")
    endif()
    file(WRITE "${_vcfg}" "${_vcfg_new}")
    ccg_run(msbuild "${work}/vcfg-run/vcfgpin/vcfgpin.sln"
            --config Release --platform x64 --export-only --record)
    expect_rc(0 "pinned-toolset release run")
    set(py_args "${work}/vcfg-run/vcfgpin/.ccg/msbuild.json"
                "${work}/vcfg-run/vcfgpin/.ccg/Release-x64/compile_commands.json"
                "${_ver_1444}")
    py_run("pinned-toolset cl + compat" [==[
import json, sys
reg = json.load(open(sys.argv[1], encoding="utf-8"))
rel = [c for c in reg["configs"] if c["name"] == "Release-x64"]
assert len(rel) == 1, reg
rel = rel[0]
assert sys.argv[3] in rel["cl"], rel
assert "14.51" not in rel["cl"], rel
assert rel.get("cl_resolution") == "project-property", rel
assert rel.get("mode") == "record", rel
entries = json.load(open(sys.argv[2], encoding="utf-8"))
compat = [a for e in entries for a in e["arguments"]
          if a.startswith("-fms-compatibility-version=")]
assert compat == ["-fms-compatibility-version=19" + sys.argv[3][2:]], compat
print("pin-ok")
]==])
endif()

# 混合工具集 sln：注册表内每工程 cl 解析（sln
# 自身按 vswhere 最高工具集解析每个工程）。
file(GLOB _cl_1451
     "${_vs_cmake}/VC/Tools/MSVC/14.51.*/bin/Host*/x64/cl.exe")
msvc_toolset_version("${_cl_1451}" _ver_1451)
if(_ver_1444 STREQUAL "" OR _ver_1451 STREQUAL "")
    message(STATUS
        "msbuild-record: mixed-toolset registry gate skipped (needs "
        "MSVC 14.44 and 14.51)")
else()
    file(COPY "${fixture}/mixed" DESTINATION "${work}/mixed-reg")
    # 钉版大版本 -> 已安装完整版本，写进两个构建副本
    #（同 e2e 的混合工具集闸门）。
    foreach(_spec "projA;14\\.51;${_ver_1451}" "projB;14\\.44;${_ver_1444}")
        list(GET _spec 0 _proj)
        list(GET _spec 1 _major)
        list(GET _spec 2 _full)
        set(_vcx "${work}/mixed-reg/mixed/${_proj}/${_proj}.vcxproj")
        file(READ "${_vcx}" _vcx_text)
        string(REGEX REPLACE
            "(<VCToolsVersion>)${_major}[^<]*(</VCToolsVersion>)"
            "\\1${_full}\\2" _vcx_new "${_vcx_text}")
        if(_vcx_new STREQUAL _vcx_text)
            fail("version-pin" "could not pin the installed toolset into ${_proj}.vcxproj")
        endif()
        file(WRITE "${_vcx}" "${_vcx_new}")
    endforeach()
    ccg_run(msbuild "${work}/mixed-reg/mixed/mixed.sln"
            --config Debug --platform x64 --export-only --record)
    expect_rc(0 "mixed-toolset registry run")
    set(py_args "${work}/mixed-reg/mixed/.ccg/msbuild.json"
                "${_ver_1451}" "${_ver_1444}")
    py_run("mixed-toolset per-project cl" [==[
import json, sys
reg = json.load(open(sys.argv[1], encoding="utf-8"))
cfg = [c for c in reg["configs"] if c["name"] == "Debug-x64"]
assert len(cfg) == 1, reg
by_proj = {p["project"]: p["cl"] for p in cfg[0].get("cl_projects", [])}
assert sys.argv[2] in by_proj.get("projA", ""), by_proj
assert sys.argv[3] in by_proj.get("projB", ""), by_proj
print("mixed-reg-ok")
]==])
endif()

message(STATUS "ccg_msbuild_record: all gates passed")
