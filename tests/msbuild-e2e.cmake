# MSBuild E2E 闸门（独立答案源形态——期望值不再来自提交金标：
# 捕获保真度的答案来自桩编译器这一独立信源，索引行为的答案来自
# 构建目录自参照：首轮副本 + 确定性 + 三环境一致）：
#   解析矩阵：缺操作数 / 未知选项 / 坏 --platform /
#       互斥组合 -> 退出码 2 带用法；--help 显示
#       命令；缺工程路径 -> 退出码 1。无任何 VS 也能跑。
#   记录模式主跑：Debug 捕获 + 索引的输出面
#       （TU 计数 / 失败语义 / 符号数）；捕获的逐 argv
#       保真由捕获保真闸的桩记录对照独立钉住，本门不比对
#       任何入库期望文件。
#   库自参照：首轮 Debug 库拍快照为基准副本，每轮
#       产出的库与基准携带相同的符号 unique_key /
#       edge / include / file 集（增量轮、三环境一致
#       同理；Release 侧两遍互比做确定性）。
#   配置隔离：debug_only_probe 只在 Debug、镜像在
#       Release（夹具入库的探针 TU）。
#   增量：紧接着的第二次运行跳过每个健康 TU
#       且库集合不变（失败的 unicode TU 重试）。
#   诚实：一个 #error TU -> 退出码 0、failed: 1、其余 TU
#       被索引、MSBuild 非零退出浮现在输出。
#   --all + --list：一次调用 -> 双库 + 注册表；--list
#       两者都打印。
#   IncludePath 投递：incpath 夹具工程的 include 目录
#       只经 INCLUDE 环境抵达 cl（VC++ Include Directories）；
#       适配器按捕获顺序完整物化 INCLUDE（工程、
#       工具集、SDK）成 "-imsvc DIR" 并插入 /X，
#       argv 因此自含、不依赖外部 INCLUDE 环境。
#   手工 query/add + 可运行 exe 证明：无法在 CI 中
#   脚本化（人工验证）。
#   工程文件联动：update <vcxproj>
#       经既有增量尾巴重捕获该配置
#       （快乐路径 + 发现缓存复用）、捕获失败时保住
#       既有语料（失败保旧）、修复后哈希门吸收等价事件
#       （修复收敛：内容复原 = 与快照全等 -> skipped-unchanged，
#       不派生采集）、根外工程文件视为
#       未跟踪（根外文件）、遇活捕获锁则推迟（捕获互斥）。
#   哈希门闸：联动重采前先比对刷新快照
#       （.ccg/msbuild/<stem>/snapshot.json：sln + 成员
#       vcxproj + import 闭包的文件哈希 + INCLUDE/工具链
#       身份 + ccg 版本）——全等即吸收；快照只由成功
#       求值采集尾部写回（失败保旧同步保快照）。
#   模块接口工程全链：.ixx 记录 cdb 双条形态
#       （双条、/ifcOutput 成对带值、/TP 在场、扫描/依赖直出两趟旗标）
#       + 紧接 index 全链 failed: 0（BMI 自建 + 消费 TU 符号入库）。
#   捕获保真闸（独立信源）：--full-build --cl 指向
#       clstub.py（wrap 对 .py 前插 python 拉起），桩记录
#       实收的每次 CL 调用（argv + cwd）——桩不经过我们的
#       翻译链路，是捕获面的独立答案源；对照桩记录与
#       compile_commands.json：条目数相等、逐条 argv/cwd
#       互推（对称过滤清单见闸门内注释）。
#
# 通道三态与门禁分工（默认=求值；--record 显式记录臂）：
#   上述各记录面闸门全部显式 --record 钉住 wrap 层回归
#   （记录机制退居测试/验证基础设施：wrap 测试链自体 +
#   双通道对账的对照真值臂）。求值通道（默认）验收段：
#     求值基本闸：默认通道对 msbdemo 求值——退出 0、TU
#       计数、索引全绿、注册表 mode="evaluate"、无沙箱/
#       桩/捕获日志产物（求值不跑构建、不桩化工具）。
#     双通道对账闸：同夹具 record 臂与求值臂的 cdb 按
#       (cwd, file) 对账，六维语义桶全等（通道差异面滤除
#       清单与设计 2.7 表逐项对应）；求值臂暖机墙钟锚
#       （4.5 秒 / 59 工程口径的放宽版）。
#     模块求值闸：modmsb 走求值——.ixx 单条（无扫描趟）、
#       /TP 与 /interface、/ifcOutput 真实 IntDir 值；BMI
#       自建、索引全绿。
#     求值失败保旧闸：E4 部分失败继续（好工程产物照常
#       组装 + 披露行 + failed_projects 计数）、E5 缺
#       import 降级披露、E2 看门狗超时失败保旧（cdb/
#       注册表/快照三不动——快照由此前成功轮写就，失败
#       轮字节级不重写）。
#   求值通道直读本进程 INCLUDE 组装 -imsvc（记录臂的
#   wrap 捕获拿到的是 MSBuild 子进程内 VS 初始化过的
#   INCLUDE——冷 shell 下两臂并不同源），索引断言因此经
#   cmake -E env 注入记录臂捕获的 INCLUDE（纯系统目录、
#   与夹具树无关），闸门在任何宿主 shell 下确定性成立。
#
# 一个文件的两副面孔：
#   从 tests/CMakeLists.txt 注册 -> ccg_msbuild_e2e ctest。
#   经 `cmake -P msbuild-e2e.cmake <ccg.exe> <fixture-dir> <work-dir>` 运行。
#
# 解析矩阵行在所有平台运行；提取闸门打印
# "*** SKIPPED" 标记并在无 Visual Studio / MSBuild 可发现时
# 返回（ctest SKIP_REGULAR_EXPRESSION 让无 VS 的 CI 保持绿）。

if(NOT DEFINED CMAKE_ARGV5)
    message(FATAL_ERROR
        "msbuild-e2e.cmake: expected <ccg.exe> <fixture-dir> <work-dir>")
endif()

# 内嵌 Python 载荷经宏实参传递；CMP0219
#（CMake 4.x）决定其中的反斜杠是否转义。正则
# 规范化器需要字面 `\d`，所以钉住保留行为。
if(POLICY CMP0219)
    cmake_policy(SET CMP0219 NEW)
endif()
set(exe     "${CMAKE_ARGV3}")
set(fixture "${CMAKE_ARGV4}")
set(work    "${CMAKE_ARGV5}")

# 运行/断言宏组（ccg_run/expect_rc/expect_out/expect_err/
# fail/py_run）收纳在同目录 ccg-harness.cmake，供各闸门脚本
# 共享；内嵌 Python 载荷的宏实参转义由调用点的 CMP0219 状态
# 决定（上面的脚本级钉定继续生效）。
include("${CMAKE_CURRENT_LIST_DIR}/ccg-harness.cmake")

# ----------------------------------------------------------------- 解析矩阵 --
ccg_run(msbuild)
expect_rc(2 "解析矩阵 missing operand")
ccg_run(msbuild "${fixture}/DemoSln.sln" --platform "x6|4")
expect_rc(2 "解析矩阵 bad platform")
ccg_run(msbuild "${fixture}/DemoSln.sln" --bogus)
expect_rc(2 "解析矩阵 unknown option")
ccg_run(msbuild "${fixture}/DemoSln.sln" --all --config Debug)
expect_rc(2 "解析矩阵 all+config exclusive")
ccg_run(msbuild --list "${fixture}/DemoSln.sln" --export-only)
expect_rc(2 "解析矩阵 list+export exclusive")
ccg_run(help msbuild)
expect_rc(0 "解析矩阵 help topic")
expect_out("usage: ccg msbuild" "解析矩阵 help topic")
ccg_run(--help)
expect_rc(0 "解析矩阵 top help")
expect_out("msbuild" "解析矩阵 top help")
ccg_run(msbuild --list "${fixture}/definitely-missing")
expect_rc(1 "解析矩阵 list missing path")
expect_err("project path not found" "解析矩阵 list missing path")

# ------------------------------------------------- VS 存在性（跳过闸门）--
# $ENV{...} 拼不出带括号的 "ProgramFiles(x86)" 名，所以
# 经 `cmake -E environment` 读环境块并解析值。
set(_pf86 "")
execute_process(COMMAND "${CMAKE_COMMAND}" -E environment
    OUTPUT_VARIABLE _env_block RESULT_VARIABLE _env_rc)
if(_env_rc EQUAL 0)
    # 锚定行首：否则 "CommonProgramFiles(x86)=" 会先
    # 命中（Common 前缀内的子串命中）。
    string(PREPEND _env_block "\n")
    string(FIND "${_env_block}" "\nProgramFiles(x86)=" _at)
    if(_at GREATER -1)
        math(EXPR _val_at "${_at} + 19")  # \n + len("ProgramFiles(x86)=") = 19
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
        "extraction gates need it (parse rows already passed)")
    return()
endif()

# VS 安装根（供真 vcvars shell 闸门与混合工具集
# 夹具检查用）。缺席条目即跳过那些环境变体。
set(_vs_root "")
if(NOT _vswhere STREQUAL "" AND EXISTS "${_vswhere}")
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
endif()

file(REMOVE_RECURSE "${work}")

# ------------------------------------------------------------- 记录模式主跑 --
# 复制夹具工程（夹具是只读输入；MSBuild 把
# build/ 输出写进工作副本）。--record 显式钉住记录臂
# （默认通道已是求值；本闸门链测的是 wrap 捕获面）。
foreach(_d DemoSln.sln msbdemo mathlib)
    file(COPY "${fixture}/${_d}" DESTINATION "${work}/proj")
endforeach()
ccg_run(msbuild "${work}/proj/DemoSln.sln" --config Debug --platform x64
        --record)
expect_rc(0 "记录模式主跑 debug run")
expect_out("indexing 5 TUs" "记录模式主跑 debug run")
# 夹具的非 ASCII TU（mathlib src-ünikode/scale.cpp）是
# ccg 的已知摄取限制：提取对 unicode 干净、加载器
# 不是——确定性失败恰一个 TU、退出码 0。
expect_out("failed: 1" "记录模式主跑 debug run")
expect_out("symbols: 9" "记录模式主跑 debug run")
if(NOT EXISTS "${work}/proj/.ccg/Debug-x64/compile_commands.json")
    fail("记录模式主跑" "compile_commands.json was not written")
endif()

# 首轮基准快照（构建目录自参照的锚点）：记录模式主跑的 Debug 库此刻刚落盘，
# 备份一份只读副本，供库自参照/增量/三环境各轮对照。经 sqlite backup API 拷贝
#（读连接看到的是已提交数据，WAL 边车若在场也一并收入副本），
# 索引行为的答案来自本轮构建自身，没有任何入库期望库。
file(MAKE_DIRECTORY "${work}/baseline")
set(py_args "${work}/proj/.ccg/Debug-x64/ccg.db" "${work}/baseline/ccg-Debug.db")
py_run("记录模式主跑 first-run baseline snapshot" [==[
import sqlite3, sys
src = sqlite3.connect(sys.argv[1])
dst = sqlite3.connect(sys.argv[2])
src.backup(dst)
dst.close()
src.close()
print("baseline-ok")
]==])

# 共享库比较器：文件路径、符号 unique_key 集、边
#（src_key, dst_key, type, site_file, site_line）集与 include
#（file, target, line, angled）集，全部根规范化（库自参照/增量对照共用）。
function(db_compare label run_db gold_db)
    set(py_args "${run_db}" "${gold_db}")
    py_run("${label}" [==[
import sqlite3, sys
def sets(path):
    c = sqlite3.connect(path)
    q = lambda s: list(c.execute(s))
    files = {r[0] for r in q(
        "select s.s from files f join str s on s.id = f.path_id")}
    keys = {r[0] for r in q(
        "select s.s from symbols y join str s on s.id = y.unique_key_id")}
    edges = set(q(
        "select a.s, b.s, t.name, pf.s, e.site_line from edges e "
        "join symbols sa on sa.sym_id = e.src_id "
        "join str a on a.id = sa.unique_key_id "
        "join symbols sb on sb.sym_id = e.dst_id "
        "join str b on b.id = sb.unique_key_id "
        "join edge_types t on t.type_id = e.type_id "
        "join files f on f.file_id = e.site_file_id "
        "join str pf on pf.id = f.path_id"))
    incs = set(q(
        "select pf.s, ts.s, i.line, i.angled from includes i "
        "join files f on f.file_id = i.file_id "
        "join str pf on pf.id = f.path_id "
        "join str ts on ts.id = i.target_id"))
    c.close()
    return files, keys, edges, incs
def root_of(files):
    for p in files:
        f = p.replace(chr(92), "/")
        for m in ("/msbdemo/", "/mathlib/"):
            i = f.lower().find(m)
            if i >= 0:
                return f[:i]
    raise AssertionError("no project marker path in the files table")
def rep(s, root):
    f = s.replace(chr(92), "/")
    r = root.lower()
    i = f.lower().find(r)
    while i >= 0:
        f = f[:i] + "<R>" + f[i + len(r):]
        i = f.lower().find(r)
    return f
def norm(v, root):
    if isinstance(v, str):
        return rep(v, root)
    if isinstance(v, tuple):
        return tuple(norm(x, root) for x in v)
    return v
rf, rk, re_, ri = sets(sys.argv[1])
gf, gk, ge, gi = sets(sys.argv[2])
rroot, groot = root_of(rf), root_of(gf)
for name, got, want in (("files", rf, gf), ("symbol keys", rk, gk),
                        ("edges", re_, ge), ("includes", ri, gi)):
    a = {norm(x, rroot) for x in got}
    b = {norm(x, groot) for x in want}
    if a != b:
        only_run = sorted(map(str, a - b))[:5]
        only_gold = sorted(map(str, b - a))[:5]
        raise AssertionError((name, only_run, only_gold))
print("db-ok")
]==])
endfunction()

# ----------------------------------------------------------------- 库自参照 --
db_compare("库自参照 debug db vs first-run baseline"
           "${work}/proj/.ccg/Debug-x64/ccg.db" "${work}/baseline/ccg-Debug.db")

# ---------------------------------------------------------------- 三环境一致 --
# 三环境一致性：ccg 产出的 cdb 自含
#（/X + 完整 -imsvc），所以在纯 shell、在捕获的
# INCLUDE 与被投毒的 INCLUDE 下索引它必须产出
# 相同的 files/symbols/edges/includes。
set(_env_cdb "${work}/proj/.ccg/Debug-x64/compile_commands.json")
set(py_args "${_env_cdb}")
py_run("三环境一致 captured INCLUDE" [==[
import json, sys
entries = json.load(open(sys.argv[1], encoding="utf-8"))
argv = entries[0]["arguments"]
dirs = [argv[i] for i in range(1, len(argv)) if argv[i - 1] == "-imsvc"]
assert dirs, argv
print(";".join(dirs))
]==])
set(_captured_include "${py_out}")
set(_env_root "${work}/env-consistency")
# 硬闸门要求纯 shell / vcvars shell / 捕获 INCLUDE
# 三者一致。vcvars 案例跑真实 `vcvars64.bat` shell（不只是
# 一个 INCLUDE 值），当 VS 安装提供时。
set(_env_cases pure captured bogus)
set(_vcvars_bat "${_vs_root}/VC/Auxiliary/Build/vcvars64.bat")
if(EXISTS "${_vcvars_bat}")
    list(APPEND _env_cases vcvars)
else()
    message(STATUS
        "msbuild-e2e: vcvars shell case skipped (${_vcvars_bat} absent)")
endif()
foreach(_case ${_env_cases})
    set(_out "${_env_root}/${_case}/EnvCheck/ccg.db")
    if(_case STREQUAL "pure")
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env --unset=INCLUDE
                    "${exe}" index "${work}/proj" --config EnvCheck
                    --compile-commands "${_env_cdb}" -o "${_env_root}/${_case}"
            RESULT_VARIABLE _env_rc OUTPUT_VARIABLE _env_out
            ERROR_VARIABLE _env_err)
    elseif(_case STREQUAL "captured")
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env "INCLUDE=${_captured_include}"
                    "${exe}" index "${work}/proj" --config EnvCheck
                    --compile-commands "${_env_cdb}" -o "${_env_root}/${_case}"
            RESULT_VARIABLE _env_rc OUTPUT_VARIABLE _env_out
            ERROR_VARIABLE _env_err)
    elseif(_case STREQUAL "vcvars")
        # 单个 cmd.exe 脚本：调 vcvars64.bat 再跑同一条 index
        # 命令。此处不用 10 秒——闸门练的是真实
        # 环境，ctest 自己的 600 秒超时兜底。
        file(TO_NATIVE_PATH "${_vcvars_bat}" _vcvars_native)
        file(TO_NATIVE_PATH "${exe}" _exe_native)
        file(TO_NATIVE_PATH "${work}/proj" _proj_native)
        file(TO_NATIVE_PATH "${_env_cdb}" _cdb_native)
        file(TO_NATIVE_PATH "${_env_root}/vcvars" _out_native)
        set(_vc_script "${_env_root}/run-vcvars-index.cmd")
        file(WRITE "${_vc_script}"
            "@echo off\r\n"
            "call \"${_vcvars_native}\" >nul 2>&1\r\n"
            "if errorlevel 1 exit /b 2\r\n"
            "\"${_exe_native}\" index \"${_proj_native}\" --config EnvCheck "
            "--compile-commands \"${_cdb_native}\" -o \"${_out_native}\"\r\n")
        execute_process(
            COMMAND cmd.exe /d /s /c "${_vc_script}"
            RESULT_VARIABLE _env_rc OUTPUT_VARIABLE _env_out
            ERROR_VARIABLE _env_err)
    else()
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env
                    "INCLUDE=${work}/bogus-include-dir"
                    "${exe}" index "${work}/proj" --config EnvCheck
                    --compile-commands "${_env_cdb}" -o "${_env_root}/${_case}"
            RESULT_VARIABLE _env_rc OUTPUT_VARIABLE _env_out
            ERROR_VARIABLE _env_err)
    endif()
    if(NOT _env_rc EQUAL 0)
        fail("三环境一致 ${_case}" "index exited ${_env_rc}:\n${_env_out}\n${_env_err}")
    endif()
endforeach()
db_compare("三环境一致 pure shell vs captured INCLUDE"
           "${_env_root}/pure/EnvCheck/ccg.db"
           "${_env_root}/captured/EnvCheck/ccg.db")
db_compare("三环境一致 pure shell vs poisoned INCLUDE"
           "${_env_root}/pure/EnvCheck/ccg.db"
           "${_env_root}/bogus/EnvCheck/ccg.db")
if(EXISTS "${_env_root}/vcvars/EnvCheck/ccg.db")
    db_compare("三环境一致 pure shell vs vcvars shell"
               "${_env_root}/pure/EnvCheck/ccg.db"
               "${_env_root}/vcvars/EnvCheck/ccg.db")
    db_compare("三环境一致 vcvars shell vs first-run baseline"
               "${_env_root}/vcvars/EnvCheck/ccg.db"
               "${work}/baseline/ccg-Debug.db")
endif()
db_compare("三环境一致 pure shell vs first-run baseline"
           "${_env_root}/pure/EnvCheck/ccg.db" "${work}/baseline/ccg-Debug.db")

# ----------------------------------------------------------------- 配置隔离 --
ccg_run(query debug_only_probe -o "${work}/proj/.ccg" --config Debug-x64)
expect_rc(0 "配置隔离 debug probe hit")
expect_out("demo::debug_only_probe" "配置隔离 debug probe hit")
ccg_run(query release_only_probe -o "${work}/proj/.ccg" --config Debug-x64)
expect_rc(1 "配置隔离 debug probe mirror miss")

# --------------------------------------------------------------------- 增量 --
ccg_run(msbuild "${work}/proj/DemoSln.sln" --config Debug --platform x64
        --record)
expect_rc(0 "增量 incremental run")
expect_out("mode: incremental" "增量 incremental run")
expect_out("skipped: 4" "增量 incremental run")
expect_out("failed: 1" "增量 incremental run")
db_compare("增量 db sets unchanged"
           "${work}/proj/.ccg/Debug-x64/ccg.db" "${work}/baseline/ccg-Debug.db")

# ------------------------------------- Release 配置（主跑/自参照/隔离的 Release 侧）
ccg_run(msbuild "${work}/proj/DemoSln.sln" --config Release --platform x64
        --record)
expect_rc(0 "配置隔离·Release release run")
expect_out("failed: 1" "配置隔离·Release release run")
set(py_args "${work}/proj/.ccg/Release-x64/compile_commands.json")
py_run("记录模式主跑·Release release cdb shape" [==[
import json, sys
def load(p):
    with open(p, encoding="utf-8") as f:
        return json.load(f)
run = load(sys.argv[1])
assert any("NDEBUG" in a for e in run for a in e["arguments"]), "NDEBUG missing"
assert len(run) == 5, len(run)
print("release-cdb-ok")
]==])
# Release 首轮基准快照 + 第二遍（增量）互比：同配置两轮产出
# 携带相同集合即确定性证明（两遍互比，无入库期望库）。
set(py_args "${work}/proj/.ccg/Release-x64/ccg.db"
            "${work}/baseline/ccg-Release.db")
py_run("库自参照·Release 互比 release first-run baseline snapshot" [==[
import sqlite3, sys
src = sqlite3.connect(sys.argv[1])
dst = sqlite3.connect(sys.argv[2])
src.backup(dst)
dst.close()
src.close()
print("release-baseline-ok")
]==])
ccg_run(msbuild "${work}/proj/DemoSln.sln" --config Release --platform x64
        --record)
expect_rc(0 "库自参照·Release 互比 release second run")
expect_out("mode: incremental" "库自参照·Release 互比 release second run")
db_compare("库自参照·Release 互比 release determinism (two runs)"
           "${work}/proj/.ccg/Release-x64/ccg.db"
           "${work}/baseline/ccg-Release.db")
ccg_run(query release_only_probe -o "${work}/proj/.ccg" --config Release-x64)
expect_rc(0 "配置隔离·Release release probe hit")
expect_out("demo::release_only_probe" "配置隔离·Release release probe hit")
ccg_run(query debug_only_probe -o "${work}/proj/.ccg" --config Release-x64)
expect_rc(1 "配置隔离·Release release probe mirror miss")

# --------------------------------------------------------------------- 诚实 --
file(COPY "${fixture}/msbdemo" DESTINATION "${work}/broken")
file(WRITE "${work}/broken/msbdemo/src/broken.cpp" [===[
// Honesty gate TU: always fails to compile; the remaining batch TUs must
// still be captured (the wrapper logs the invocation before forwarding),
// translated, and indexed - with the run exiting 0 and failed: 1.
#error "intentional compile failure for the ccg msbuild honesty gate"
]===])
file(READ "${work}/broken/msbdemo/msbdemo.vcxproj" _vcx)
string(REPLACE
    [==[<ClCompile Include="src\probe.cpp" />]==]
    [==[<ClCompile Include="src\probe.cpp" />
    <ClCompile Include="src\broken.cpp" />]==]
    _vcx_patched "${_vcx}")
if(_vcx_patched STREQUAL _vcx)
    fail("诚实" "could not patch the copied vcxproj (probe line not found)")
endif()
file(WRITE "${work}/broken/msbdemo/msbdemo.vcxproj" "${_vcx_patched}")
# 记录臂（显式 --record）从不真实编译，所以 #error TU 仍
# 被捕获、只在加载器处失败——MSBuild 自身退出 0。
ccg_run(msbuild "${work}/broken/msbdemo/msbdemo.sln"
        --config Debug --platform x64 --record)
expect_rc(0 "诚实 honesty exit code")
expect_out("indexing 4 TUs" "诚实 honesty summary")
expect_out("fresh: 4" "诚实 honesty summary")
expect_out("failed: 1" "诚实 honesty summary")
# --full-build 保住真实编译的诚实契约：cl 失败、
# MSBuild 非零退出被浮现（绝不吞掉）、其余 TU 仍
# 被捕获 + 索引（诚实契约，逐字节）。
ccg_run(msbuild "${work}/broken/msbdemo/msbdemo.sln"
        --config Debug --platform x64 --full-build -o "${work}/broken-full")
expect_rc(0 "诚实 full-build honesty exit code")
expect_out("fresh: 4" "诚实 full-build honesty summary")
expect_out("failed: 1" "诚实 full-build honesty summary")
expect_err("MSBuild exited" "诚实 full-build msbuild exit surfaced")
ccg_run(query demo::add -o "${work}/broken/msbdemo/.ccg" --config Debug-x64)
expect_rc(0 "诚实 remaining TUs indexed")
expect_out("demo::add" "诚实 remaining TUs indexed")

# --------------------------------------------------------------- all 与 list --
foreach(_d DemoSln.sln msbdemo mathlib)
    file(COPY "${fixture}/${_d}" DESTINATION "${work}/all")
endforeach()
ccg_run(msbuild "${work}/all/DemoSln.sln" --all --record)
expect_rc(0 "--all 与 --list all run")
foreach(_stem Debug-x64 Release-x64)
    if(NOT EXISTS "${work}/all/.ccg/${_stem}/ccg.db")
        fail("--all 与 --list" "${_stem}.db was not produced by --all")
    endif()
endforeach()
ccg_run(msbuild --list "${work}/all/DemoSln.sln")
expect_rc(0 "--all 与 --list list")
expect_out("Debug-x64" "--all 与 --list list")
expect_out("Release-x64" "--all 与 --list list")
set(py_args "${work}/all/.ccg/msbuild.json")
py_run("--all 与 --list registry" [==[
import json, sys
with open(sys.argv[1], encoding="utf-8") as f:
    reg = json.load(f)
names = [c["name"] for c in reg["configs"]]
assert names == ["Debug-x64", "Release-x64"], names
assert reg["default"] == "Debug-x64", reg["default"]
for c in reg["configs"]:
    assert c["db"] == ".ccg/%s/ccg.db" % c["name"], c
    assert c["compile_commands"] == ".ccg/%s/compile_commands.json" % c["name"], c
    assert c["tus"] == 5 and c["failed"] == 1, c
print("registry-ok")
]==])

# --------------------------------------------------------- IncludePath 投递 --
# IncludePath 投递（INCLUDE 环境分类）：incpath 夹具的
# 两个 include 目录只乘捕获的 INCLUDE 环境。分类
# 修复前本闸门以两个 TU 都不可解析失败
#（"failed: 2"、0 查询命中、cdb 无 -imsvc）。
file(COPY "${fixture}/incpath" DESTINATION "${work}/incpath-run")
ccg_run(msbuild "${work}/incpath-run/incpath/incpath.sln"
        --config Debug --platform x64 --record)
expect_rc(0 "IncludePath 投递 includepath run")
expect_out("indexing 2 TUs" "IncludePath 投递 includepath run")
expect_out("failed: 0" "IncludePath 投递 includepath clean")
ccg_run(query envmath::env_scale -o "${work}/incpath-run/incpath/.ccg" --config Debug-x64)
expect_rc(0 "IncludePath 投递 inc-dir header resolved (env_scale)")
expect_out("envmath::env_scale" "IncludePath 投递 inc-dir header resolved (env_scale)")
ccg_run(query envmath::env_deep -o "${work}/incpath-run/incpath/.ccg" --config Debug-x64)
expect_rc(0 "IncludePath 投递 inc2-dir header resolved (env_deep)")
expect_out("envmath::env_deep" "IncludePath 投递 inc2-dir header resolved (env_deep)")
set(py_args "${work}/incpath-run/incpath/.ccg/Debug-x64/compile_commands.json")
py_run("IncludePath 投递 cdb INCLUDE materialization" [==[
import json, re, sys
entries = json.load(open(sys.argv[1], encoding="utf-8"))
assert len(entries) == 2, len(entries)
for e in entries:
    argv = e["arguments"]
    fed = [v.replace(chr(92), "/") for i, v in enumerate(argv)
           if i > 0 and argv[i - 1] == "-imsvc"]
    # 完整物化：工具集、VS Aux、Windows SDK 与 NETFXSDK
    # 不再被丢，/X 让 argv 成为唯一来源。
    assert "/X" in argv, argv
    lows = [v.lower() for v in fed]
    assert any("/vc/tools/msvc/" in v for v in lows), fed
    assert any("/windows kits/" in v for v in lows), fed
    # 工程目录排在系统根之后：cl 先搜 /I
    #（命令行）再搜 INCLUDE 环境，且捕获顺序保持
    # inc 在 inc2 前。
    incs = [v for v in fed if v.endswith("/inc")]
    inc2s = [v for v in fed if v.endswith("/inc2")]
    assert len(incs) == 1 and len(inc2s) == 1, fed
    assert fed.index(incs[0]) < fed.index(inc2s[0]), fed
    compat = [v for v in argv
              if v.startswith("-fms-compatibility-version=")]
    assert len(compat) == 1, argv
    assert re.fullmatch(r"-fms-compatibility-version=19\.\d+\.\d+",
                        compat[0]), compat
    slash = [i for i, t in enumerate(argv) if t.startswith("/")]
    assert argv.index("-imsvc") > (max(slash) if slash else 1), argv
print("includepath-ok")
]==])

# ----------------------------------------------------------------- PCH 工程 --
# MSBuild /Yc PCH 链：--export-only 捕获在 cdb 旁写 pch.json
#（cdb_sha256 命中、producers/consumers 非空）；随后
# ccg index 从边车构建影子且消费者经它解析
#（0 failed、无文本回退）。
file(COPY "${fixture}/pchp" DESTINATION "${work}/pchp-run")
ccg_run(msbuild "${work}/pchp-run/pchp/pchp.sln"
        --config Debug --platform x64 --export-only --record)
expect_rc(0 "PCH 工程 PCH export-only")
expect_out("compile commands (2 TUs, record)" "PCH 工程 PCH export-only")
set(_pch_cdb "${work}/pchp-run/pchp/.ccg/Debug-x64/compile_commands.json")
set(_pch_json "${work}/pchp-run/pchp/.ccg/Debug-x64/pch.json")
if(NOT EXISTS "${_pch_json}")
    fail("PCH 工程" "pch.json was not written beside the cdb")
endif()
set(py_args "${_pch_json}" "${_pch_cdb}")
py_run("PCH 工程 sidecar contents" [==[
import hashlib, json, sys
side = json.load(open(sys.argv[1], encoding="utf-8"))
cdb_bytes = open(sys.argv[2], "rb").read()
assert side["version"] == 1, side.get("version")
assert side["cdb_sha256"] == hashlib.sha256(cdb_bytes).hexdigest(), "cdb_sha256 mismatch"
prod = side.get("producers") or []
cons = side.get("consumers") or []
assert len(prod) == 1, prod
assert len(cons) == 1, cons
argv = prod[0]["argv"]
assert any(a.startswith("/Yc") for a in argv), argv
assert any(a.startswith("/Fp") for a in argv), argv
assert prod[0]["pch_key"] == cons[0]["pch_key"], (prod[0]["pch_key"], cons[0]["pch_key"])
print("pch-sidecar-ok")
]==])
ccg_run(index "${work}/pchp-run/pchp" --config PchTest
        --compile-commands "${_pch_cdb}" -o "${work}/pchp-run/idx")
expect_rc(0 "PCH 工程 PCH index")
expect_out("pch: 1 built" "PCH 工程 PCH shadow built")
expect_out("0 text fallbacks" "PCH 工程 no text fallback")
expect_out("failed: 0" "PCH 工程 PCH index clean")
file(GLOB _shadows "${work}/pchp-run/idx/PchTest/pch/*/*/shadow.pch")
if(NOT _shadows)
    fail("PCH 工程" "no shadow.pch under the PCH cache")
endif()

# --------------------------------------------------------------- 混合工具集 --
# 混合工具集 .sln：projA 钉 14.51、projB 钉 14.44（入库夹具只钉
# 大版本）。MSBuild 对部分版本号的 VCToolsVersion 直接拒绝
#（MSB8070），所以构建前把该大版本下已安装的完整工具集替换进
# 副本的 vcxproj。被包装的 real_cl 对两者都是 .sln 级最高工具集，
# 所以只有每 TU INCLUDE 携带 projB 真实工具集。每个 main.cpp 都是
# 字面 _MSC_VER 探针：除非重放兼容版本匹配该工程工具集否则 #error。
set(_vs_cmake "${_vs_root}")
if(NOT _vs_cmake STREQUAL "")
    file(TO_CMAKE_PATH "${_vs_cmake}" _vs_cmake)
endif()
# 从 cl.exe 的 GLOB 命中路径解析 MSVC 完整版本号（MSVC
# 目录名的下一段）；同一大版本下命中多个时取字典序最高
#（完整版本号末段等长，字典序即版本序）。
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
file(GLOB _cl_1451
     "${_vs_cmake}/VC/Tools/MSVC/14.51.*/bin/Host*/x64/cl.exe")
msvc_toolset_version("${_cl_1444}" _ver_1444)
msvc_toolset_version("${_cl_1451}" _ver_1451)
if(_ver_1444 STREQUAL "" OR _ver_1451 STREQUAL "")
    message(STATUS
        "msbuild-e2e: mixed-toolset gate skipped (needs both MSVC "
        "14.44 and 14.51 toolsets)")
else()
    file(COPY "${fixture}/mixed" DESTINATION "${work}/mixed-run")
    # 钉版大版本 -> 已安装完整版本，写进两个构建副本。
    foreach(_spec "projA;14\\.51;${_ver_1451}" "projB;14\\.44;${_ver_1444}")
        list(GET _spec 0 _proj)
        list(GET _spec 1 _major)
        list(GET _spec 2 _full)
        set(_vcx "${work}/mixed-run/mixed/${_proj}/${_proj}.vcxproj")
        file(READ "${_vcx}" _vcx_text)
        string(REGEX REPLACE
            "(<VCToolsVersion>)${_major}[^<]*(</VCToolsVersion>)"
            "\\1${_full}\\2" _vcx_new "${_vcx_text}")
        if(_vcx_new STREQUAL _vcx_text)
            fail("混合工具集" "could not pin the installed toolset into ${_proj}.vcxproj")
        endif()
        file(WRITE "${_vcx}" "${_vcx_new}")
    endforeach()
    ccg_run(msbuild "${work}/mixed-run/mixed/mixed.sln"
            --config Debug --platform x64 --record)
    expect_rc(0 "混合工具集 mixed-toolset run")
    expect_out("indexing 2 TUs" "混合工具集 mixed-toolset run")
    expect_out("failed: 0" "混合工具集 mixed-toolset _MSC_VER probes")
    ccg_run(query projA_probe -o "${work}/mixed-run/mixed/.ccg"
            --config Debug-x64)
    expect_rc(0 "混合工具集 projA probe symbol")
    ccg_run(query projB_probe -o "${work}/mixed-run/mixed/.ccg"
            --config Debug-x64)
    expect_rc(0 "混合工具集 projB probe symbol")
    set(py_args
        "${work}/mixed-run/mixed/.ccg/Debug-x64/compile_commands.json"
        "${_ver_1451}" "${_ver_1444}")
    py_run("混合工具集 per-TU compatibility versions" [==[
import json, sys
entries = json.load(open(sys.argv[1], encoding="utf-8"))
assert len(entries) == 2, len(entries)
by_proj = {}
for e in entries:
    f = e["file"].replace(chr(92), "/").lower()
    if "/proja/" in f:
        by_proj["projA"] = e
    elif "/projb/" in f:
        by_proj["projB"] = e
assert set(by_proj) == {"projA", "projB"}, by_proj
# 兼容版本由工具集完整版本推导：14.51.36231 -> 19.51.36231。
def compat_of(ver):
    assert ver.startswith("14."), ver
    return "19" + ver[2:]
want = {"projA": (compat_of(sys.argv[2]), sys.argv[2]),
        "projB": (compat_of(sys.argv[3]), sys.argv[3])}
for name, e in by_proj.items():
    argv = e["arguments"]
    compat = [a for a in argv if a.startswith("-fms-compatibility-version=")]
    assert compat == ["-fms-compatibility-version=" + want[name][0]], (name, compat)
    fed = [v.replace(chr(92), "/").lower() for i, v in enumerate(argv)
           if i > 0 and argv[i - 1] == "-imsvc"]
    assert any(("tools/msvc/" + want[name][1] + "/include") in v for v in fed), (name, fed)
    other = sys.argv[3] if name == "projA" else sys.argv[2]
    assert not any(("tools/msvc/" + other + "/include") in v for v in fed), (name, fed)
    assert "/X" in argv, (name, argv)
print("mixed-toolset-ok")
]==])
endif()

# ------------------------------------------------------------- 工程文件联动 --
# 工程文件联动：编辑工程文件必须经
# `ccg update <file>` 流入对该配置的
# 重捕获（引擎不动）、经去抖与
# 互斥守卫，捕获失败时保住既有语料完整。
#   快乐路径：vcxproj 注册新 TU -> update <vcxproj>
#         重捕获、cdb 得到该 TU、联动索引它
#         （查询命中）且旧语料保留（旧符号命中）；
#         工具链发现从注册表复用（verbose 行）。
#   失败保旧：坏 vcxproj 中止刷新——
#         cdb 字节不变、库仍可查、退出码 0、state "failed"。
#   修复收敛：修好 vcxproj 并再保存收敛
#         保留队列行（state "captured"）。
#   根外工程文件：无注册表匹配 -> state
#         "untracked"、不捕获。
#   捕获互斥：活锁推迟刷新（state "busy"）、
#         排队行留下；移除锁后下一事件
#         收敛它。
foreach(_d DemoSln.sln msbdemo mathlib)
    file(COPY "${fixture}/${_d}" DESTINATION "${work}/link")
endforeach()
ccg_run(msbuild "${work}/link/DemoSln.sln" --config Debug --platform x64
        --record)
expect_rc(0 "工程文件联动 base capture")
expect_out("indexing 5 TUs" "工程文件联动 base capture")
# 短去抖，闸门不必每次刷新付默认 3 秒。
ccg_run(config set msbuild-refresh-debounce-ms 150 -o "${work}/link/.ccg")
expect_rc(0 "工程文件联动 debounce knob")
set(_link_cdb "${work}/link/.ccg/Debug-x64/compile_commands.json")

# --- 快乐路径：经 vcxproj 编辑加新 TU -> update <vcxproj> 重捕获 ---
# 派生刷新走生产求值道（ccg update 派生的 ccg msbuild 不带 --record）：
# 求值直读本进程 INCLUDE 组装 -imsvc，索引断言因此给 update 注入
# 记录臂捕获的 INCLUDE（系统目录集），派生子进程原样继承。
file(WRITE "${work}/link/msbdemo/src/extra.cpp" [===[
#include "util.hpp"

namespace demo {

int extra_value() { return add(19, 23); }

}  // namespace demo
]===])
file(READ "${work}/link/msbdemo/msbdemo.vcxproj" _link_vcx)
string(REPLACE
    [==[<ClCompile Include="src\probe.cpp" />]==]
    [==[<ClCompile Include="src\probe.cpp" />
    <ClCompile Include="src\extra.cpp" />]==]
    _link_vcx_patched "${_link_vcx}")
if(_link_vcx_patched STREQUAL _link_vcx)
    fail("工程文件联动·快乐路径" "could not patch the copied vcxproj (probe line not found)")
endif()
file(WRITE "${work}/link/msbdemo/msbdemo.vcxproj" "${_link_vcx_patched}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "INCLUDE=${_captured_include}"
            -- "${exe}" update "${work}/link/msbdemo/msbdemo.vcxproj"
            -o "${work}/link/.ccg" --config Debug-x64 --json
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "工程文件联动·快乐路径 linkage update exit")
expect_out("captured" "工程文件联动·快乐路径 linkage state captured")
expect_err("project files re-captured" "工程文件联动·快乐路径 capture notice")
# 重捕获的 cdb 携带新 TU。
set(py_args "${_link_cdb}")
py_run("工程文件联动·快乐路径 cdb gained the new TU" [==[
import json, sys
entries = json.load(open(sys.argv[1], encoding="utf-8"))
files = [e["file"].replace(chr(92), "/").lower() for e in entries]
assert any(f.endswith("/extra.cpp") for f in files), files
print("cdb-extra-ok")
]==])
# 增量尾巴只索引新 TU：它可查询、旧
# 语料（既有符号）不动。
ccg_run(query extra_value -o "${work}/link/.ccg" --config Debug-x64)
expect_rc(0 "工程文件联动·快乐路径 new TU symbol indexed")
expect_out("demo::extra_value" "工程文件联动·快乐路径 new TU symbol indexed")
ccg_run(query demo::add -o "${work}/link/.ccg" --config Debug-x64)
expect_rc(0 "工程文件联动·快乐路径 old corpus intact")
# 工具链发现缓存：无显式覆盖的第二次捕获
# 复用注册表记录的 msbuild/cl 路径（verbose 行）。
ccg_run(msbuild "${work}/link/DemoSln.sln" --config Debug --platform x64
        --record -v)
expect_rc(0 "工程文件联动·快乐路径 discovery-cache run")
expect_err("toolchain discovery reused" "工程文件联动·快乐路径 discovery cache reused")

# --- 失败保旧：坏 vcxproj -> 刷新中止、既有语料保留 ------------------------
# 求值道的部分失败语义（E4）会让"单坏工程"的刷新继续组装好工程产物
# （退出 0、cdb 重写）——失败保旧腿要的联动语义（失败 -> 队列行保留、
# 语料不动）要求全工程失败（E3 通道级失败保旧），因此两个成员工程
# 都弄坏（与记录臂"编排层坏即整轮中止"同形的断言面）。
file(SHA256 "${_link_cdb}" _cdb_before)
# 快照已由快乐路径派生的求值采集写就；失败轮必须字节级不动它
# （失败保旧同步保快照——快照只在成功后写）。
set(_link_snapshot "${work}/link/.ccg/msbuild/Debug-x64/snapshot.json")
file(SHA256 "${_link_snapshot}" _snap_before)
file(READ "${work}/link/mathlib/mathlib/mathlib.vcxproj" _mathlib_vcx)
file(WRITE "${work}/link/msbdemo/msbdemo.vcxproj" "<Project>\n<Broken")
file(WRITE "${work}/link/mathlib/mathlib/mathlib.vcxproj" "<Project>\n<Broken")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "INCLUDE=${_captured_include}"
            -- "${exe}" update "${work}/link/msbdemo/msbdemo.vcxproj"
            -o "${work}/link/.ccg" --config Debug-x64 --json
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "工程文件联动·失败保旧 failed refresh keeps update exit 0")
expect_out("failed" "工程文件联动·失败保旧 linkage state failed")
expect_err("project refresh failed" "工程文件联动·失败保旧 failure notice")
file(SHA256 "${_link_cdb}" _cdb_after)
if(NOT _cdb_after STREQUAL _cdb_before)
    fail("工程文件联动·失败保旧" "the cdb was rewritten despite the aborted refresh")
endif()
file(SHA256 "${_link_snapshot}" _snap_after)
if(NOT _snap_after STREQUAL _snap_before)
    fail("工程文件联动·失败保旧" "the refresh snapshot was rewritten despite the aborted refresh")
endif()
ccg_run(query extra_value -o "${work}/link/.ccg" --config Debug-x64)
expect_rc(0 "工程文件联动·失败保旧 database keeps the previous corpus")

# --- 修复收敛：修复 -> 哈希门吸收等价事件（不派生采集）---------------------
# 两个 vcxproj 都复原到快照时内容（快乐路径采集所见），快照全等 ->
# 事件被 skipped-unchanged 吸收：行出队、无派生子进程、语料不动
# （语料本就与复原后的输入一致——失败轮没动过它）。
file(WRITE "${work}/link/msbdemo/msbdemo.vcxproj" "${_link_vcx_patched}")
file(WRITE "${work}/link/mathlib/mathlib/mathlib.vcxproj" "${_mathlib_vcx}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "INCLUDE=${_captured_include}"
            -- "${exe}" update "${work}/link/msbdemo/msbdemo.vcxproj"
            -o "${work}/link/.ccg" --config Debug-x64 --json
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "工程文件联动·修复收敛 repair converges")
expect_out("skipped-unchanged" "工程文件联动·修复收敛 linkage state skipped-unchanged")
if(out MATCHES "captured")
    fail("工程文件联动·修复收敛" "the unchanged event must not spawn a capture")
endif()
ccg_run(query extra_value -o "${work}/link/.ccg" --config Debug-x64)
expect_rc(0 "工程文件联动·修复收敛 corpus restored")

# --- 根外文件：根外工程文件 -> 未跟踪、不捕获 ------------------------------
file(WRITE "${work}/outside.props" "<Project />")
ccg_run(update "${work}/outside.props"
        -o "${work}/link/.ccg" --config Debug-x64 --json)
expect_rc(0 "工程文件联动·根外文件 outside props exit")
expect_out("untracked" "工程文件联动·根外文件 linkage state untracked")
expect_err("outside the registered solution tree" "工程文件联动·根外文件 outside note")

# --- 捕获互斥：活捕获锁推迟刷新（行留下）---------------------------------
# 哈希门在锁探测之前——输入与快照全等的事件根本到不了锁探测。本腿
# 要练锁路径，先弄脏输入（尾部追加换行：内容变、求值语义不变）。
file(APPEND "${work}/link/msbdemo/msbdemo.vcxproj" "\n")
set(_cap_lock "${work}/link/.ccg/msbuild/capture-Debug-x64.lock")
file(MAKE_DIRECTORY "${work}/link/.ccg/msbuild")
# 锁载荷：活 pid（本 cmake 进程经 python 子进程的
# ppid）加远期时间戳，两条活性规则一致判"持有"。
execute_process(COMMAND python -I -c
    "import os; open(r'${_cap_lock}', 'w').write('%d\\n99999999999999\\ncapture-defer-probe' % os.getppid())"
    RESULT_VARIABLE _lock_rc)
if(NOT _lock_rc EQUAL 0)
    fail("工程文件联动·捕获互斥" "could not write the live capture lock (${_lock_rc})")
endif()
ccg_run(update "${work}/link/msbdemo/msbdemo.vcxproj"
        -o "${work}/link/.ccg" --config Debug-x64 --json)
expect_rc(0 "工程文件联动·捕获互斥 deferred refresh exit")
expect_out("busy" "工程文件联动·捕获互斥 linkage state busy")
expect_err("another capture is running" "工程文件联动·捕获互斥 deferral notice")
# 锁没了 -> 下一事件收敛保留行。
file(REMOVE "${_cap_lock}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "INCLUDE=${_captured_include}"
            -- "${exe}" update "${work}/link/msbdemo/msbdemo.vcxproj"
            -o "${work}/link/.ccg" --config Debug-x64 --json
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "工程文件联动·捕获互斥 post-lock convergence")
expect_out("captured" "工程文件联动·捕获互斥 linkage state captured after lock release")

# --- 哈希门闸：快照写回形态 + 等价事件吸收 ---------------------------------
# 上一腿的脏输入触发过一次真实重采（captured），其求值腿尾部应已把新
# 快照写回 .ccg/msbuild/Debug-x64/。形态断言：文件集含 sln + 两成员
# vcxproj + VS targets 的 import 闭包项；逐条 sha256 与当前盘上文件
# 一致（写回反映当前输入身份）；环境四元组与 ccg 版本在场。
if(NOT EXISTS "${_link_snapshot}")
    fail("哈希门闸" "the spawned evaluate capture did not write the refresh snapshot")
endif()
set(py_args "${_link_snapshot}" "${work}/link/DemoSln.sln"
            "${work}/link/msbdemo/msbdemo.vcxproj"
            "${work}/link/mathlib/mathlib/mathlib.vcxproj")
py_run("哈希门闸 snapshot shape" [==[
import hashlib, json, os, sys
snap = json.load(open(sys.argv[1], encoding="utf-8"))
paths = [f["path"].replace(chr(92), "/").lower() for f in snap["files"]]
def find(needle):
    return next((f for f in snap["files"]
                 if f["path"].replace(chr(92), "/").lower().endswith(needle)),
                None)
for needle in (sys.argv[2], sys.argv[3], sys.argv[4]):
    entry = find(needle.lower())
    assert entry is not None, (needle, paths[:8])
    digest = hashlib.sha256(open(needle, "rb").read()).hexdigest()
    assert entry["sha256"] == digest, (entry["path"], entry["sha256"], digest)
assert any("/microsoft.cpp." in p for p in paths), "import closure of VC targets"
for f in snap["files"]:
    assert isinstance(f["sha256"], str) and len(f["sha256"]) in (0, 64), f
env = snap["env"]
for key in ("include", "msbuild", "cl", "vc_targets"):
    assert isinstance(env.get(key), str) and env[key], key
assert len(env["include"]) == 64, env["include"]
assert snap["ccg_version"], snap
print("hashgate-snapshot-ok")
]==])
# 等价事件吸收：同内容重写（mtime 变、哈希不变）-> skipped-unchanged、
# 无派生采集。
file(READ "${work}/link/msbdemo/msbdemo.vcxproj" _gate_same_bytes)
file(WRITE "${work}/link/msbdemo/msbdemo.vcxproj" "${_gate_same_bytes}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "INCLUDE=${_captured_include}"
            -- "${exe}" update "${work}/link/msbdemo/msbdemo.vcxproj"
            -o "${work}/link/.ccg" --config Debug-x64 --json
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "哈希门闸 equivalent event absorbed")
expect_out("skipped-unchanged" "哈希门闸 linkage state skipped-unchanged")
if(out MATCHES "captured")
    fail("哈希门闸" "an unchanged event must not reach the capture")
endif()

# --------------------------------------------------------- 模块接口工程全链 --
# 模块接口工程的全链闸门（语料形态与 MSBuild 实录形态一致）：
# MSBuild 给 .ixx 发扫描/编译两趟调用 -> 记录 cdb 每文件双条、
# 带值开关成对保留（/ifcOutput 后随 .ifc 产物路径值而非裸旗标，
# 名单单点见 cg.util.msvcswitch）、/TP 在场。断言：
#   记录面：mod.ixx 恰好两条、两条都带 /TP、/ifcOutput 成对带
#         .ifc 值、趟旗标一条 /scanDependencies（目录值）一条
#         /sourceDependencies（依赖直出 json 值）
#   索引面：紧接的 index 全链 failed: 0（BMI 自建成功、消费 TU
#         的 import 落库）——拼装层把引擎路径塞进参数向量首元素 ×
#         记录 argv 的 /TP 双输入 + 追加的 -clang:-o 的组合恰会在此
#         形态下触发 cannot specify -o when generating
#         multiple output files。msvc 树与 clang 树都跑全链：记录面走
#         MSBuild/clwrap，BMI 自建走外部配置的引擎 clang++，与构建树
#         编译器无关（msvc 树同样全链通过，无需按树降级）。
file(COPY "${fixture}/modmsb" DESTINATION "${work}")
ccg_run(msbuild "${work}/modmsb/modmsb.sln" --config Debug --platform x64
        --record)
expect_rc(0 "模块接口工程全链 module project capture exit")
expect_out("indexing 2 TUs" "模块接口工程全链 module project indexes both TUs")
expect_out("failed: 0" "模块接口工程全链 the module project indexes clean end to end")
expect_err("0 TU(s) degraded to tier-1"
           "模块接口工程全链 the interface BMI builds, nothing degrades")
if(NOT EXISTS "${work}/modmsb/.ccg/Debug-x64/compile_commands.json")
    fail("模块接口工程全链" "compile_commands.json was not written")
endif()
set(py_args "${work}/modmsb/.ccg/Debug-x64/compile_commands.json")
py_run("模块接口工程·记录面 dual-entry cdb shape" [==[
import json, sys
entries = json.load(open(sys.argv[1], encoding="utf-8"))
mod = [e for e in entries
       if e["file"].replace(chr(92), "/").lower().endswith("/mod.ixx")]
assert len(entries) == 3, len(entries)  # mod.ixx x2 + use.cpp x1
assert len(mod) == 2, len(mod)
for e in mod:
    argv = e["arguments"]
    assert "/TP" in argv, argv  # MSBuild 记录 argv 必带 /TP
    i = argv.index("/ifcOutput")
    nxt = argv[i + 1]
    assert not nxt.startswith("/") and nxt.lower().endswith(".ifc"), (
        "paired /ifcOutput must carry its .ifc artifact value, got: %r" % nxt)
pass_values = {}
for e in mod:
    argv = e["arguments"]
    for f in ("/scanDependencies", "/sourceDependencies"):
        if f in argv:
            v = argv[argv.index(f) + 1]
            assert not v.startswith("/"), (
                "%s must carry its value token, got: %r" % (f, v))
            pass_values[f] = v.replace(chr(92), "/")
assert set(pass_values) == {"/scanDependencies", "/sourceDependencies"}, pass_values
# 扫描趟 /scanDependencies 带沙箱 IntDir 目录值、编译趟
# /sourceDependencies 带依赖直出 json 值（语料实录形态）。
assert pass_values["/scanDependencies"].endswith("/"), pass_values
assert pass_values["/sourceDependencies"].endswith(".json"), pass_values
print("mod-cdb-ok")
]==])
ccg_run(query g16_take -o "${work}/modmsb/.ccg" --config Debug-x64)
expect_rc(0 "模块接口工程全链 consumer symbol queryable through the BMI")

# ----------------------------------------------------------------- 捕获保真 --
# 捕获保真闸（独立信源）：--full-build --cl 指向 tests/clstub.py（wrap
# 对 .py 后缀的 real_cl 前插 PATH python 拉起），桩逐次记录实收的 CL
# 调用（argv + cwd，一调用一文件）并以 0 退出 + 按 /Fo touch 空 obj。
# MSBuild 的 CL 任务在工具退出 0 时不校验产物在盘，全批 TU 都被调用；
# 真 Lib/Link（--full-build 只包装 CL）无法消费空 obj 而如实失败，MSBuild
# 非零退出被 ccg 浮现（自身仍 exit 0、捕获面完整翻译）——桩从不经过我们
# 的翻译链路，它的记录就是捕获面的独立答案源。覆盖 5 TU：DemoSln 只收
# mathlib 两 TU（msbdemo 被 mathlib 的 lib 失败沿 sln 依赖链截断），单工程
# msbdemo.sln 补齐三 TU；两次跑共享同一桩记录目录（天然累计追加）。
set(_fid "${work}/fidelity")
file(MAKE_DIRECTORY "${_fid}/stub-records")
foreach(_d DemoSln.sln msbdemo mathlib)
    file(COPY "${fixture}/${_d}" DESTINATION "${_fid}/proj")
endforeach()
set(_stub "${CMAKE_CURRENT_LIST_DIR}/clstub.py")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env
        "CCG_CLSTUB_OUT=${_fid}/stub-records"
        "CCG_CLSTUB_RC=0"
        -- "${exe}" msbuild "${_fid}/proj/DemoSln.sln"
        --config Debug --platform x64 --full-build --export-only
        --cl "${_stub}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "捕获保真 stub capture exit (DemoSln)")
expect_out("compile commands (2 TUs, full-build)" "捕获保真 DemoSln TU count")
expect_err("MSBuild exited" "捕获保真 mathlib real-lib failure surfaced")
expect_err("still translated and indexed" "捕获保真 DemoSln capture survives")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env
        "CCG_CLSTUB_OUT=${_fid}/stub-records"
        "CCG_CLSTUB_RC=0"
        -- "${exe}" msbuild "${_fid}/proj/msbdemo/msbdemo.sln"
        --config Debug --platform x64 --full-build --export-only
        --cl "${_stub}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "捕获保真 stub capture exit (msbdemo.sln)")
expect_out("compile commands (3 TUs, full-build)" "捕获保真 msbdemo TU count")
expect_err("MSBuild exited" "捕获保真 msbdemo real-link failure surfaced")
# 对照（双向互推）：桩记录侧把原始 cl argv 投影到与翻译产物可比较的
# 公共形态（复刻翻译层的分支语义），cdb 侧滤掉翻译层合成/追加的旗标；
# 两侧过滤严格对称——每一项过滤都因"只在一侧存在"或"两侧同源必须
# 抹平"而立项，清单如下。
#   桩侧滤除（翻译层丢弃面，cdb 必无）：
#     * /Fo /Fd /Fp /Yc /Yu——分写（含值，带翻译层同款守卫：下一 token
#       以 '/'-'-' 开头或是源文件时值不吞）与连写（/Fo<路径>）两种形态；
#     * /Gf /u-——翻译层黑名单。
#     * /X 族（/X /x -X -x）——原生 cl 的 IgnoreStandardIncludePath
#       全部合法拼写，与翻译层 is_native_cl_ignore_include 同口径。
#       翻译层把原始拼写规范化为至多一个 /X（原始未带则追加），
#       两种来路在 cdb 侧都被滤除，桩侧必须对称滤掉原始拼写，
#       否则 IgnoreStandardIncludePath 属性置真的工程必假红。
#   桩侧改形（翻译层同款改形，两侧归到同一形态）：
#     * "/D" 分写值拼单 token（/DMACRO=VALUE；clang 响应文件按空白切词）；
#     * /I 族（/I /external:I /FI）的值经翻译层 to_abs 绝对化——连写
#       保持单 token（"/I" + abs），分写保持双 token（"/I"、abs）。
#   桩侧成对透传（翻译层把带值开关与其值成对保留，两侧同形）：
#     * /ifcOutput /scanDependencies /sourceDependencies /ifcSearchDir
#       /reference /exportHeader——MSVC 模块带值开关六项（翻译层
#       开关与值成对保留进 cdb，值逐字绝不绝对化），桩侧对称把开关与
#       后随值 token 都保进 flags（末 token 裸开关只保开关、不吞值，
#       与翻译层 i+1<n 守卫一致）；名单与 cg.util.msvcswitch 单点清单
#       对表——本脚本的 Python 复刻物理上无法 import C++ 模块，是
#       登记在案的唯一同步点。
#   cdb 侧滤除（翻译层合成/追加，桩侧 argv 永不可见）：
#     * arguments[0:2]（"clang-cl"、"--driver-mode=cl"——驱动器前缀合成）；
#     * "-finput-charset=..."（宿主 ANSI 代码页探测，本机相关；单元闸门
#       msbuild.adapt_input_charset 钉住计数语义）；
#     * "-fms-compatibility-version=..."（INCLUDE/real_cl 推导的逐 TU 追加）；
#     * "/Zc:twoPhase-"（模板模式透明补齐，未显式声明时追加）；
#     * "/X"（原始未带时翻译层追加、带时由四种 cl 拼写规范化保留；
#       与桩侧的原始拼写滤除对称）；
#     * "-imsvc" 及其后随值（捕获 INCLUDE 的全量物化对）。
#   断言三层：条目数相等（桩各调用的源数总和 == cdb 条目数）；每条
#   cdb 条目恰被一条桩调用解释（cwd 相等 + file 在剩余源集中 + 投影
#   argv 全等）；桩记录的每个源都被消费（反向无遗漏）。
set(py_args
    "${_fid}/stub-records"
    "${_fid}/proj/.ccg/Debug-x64/compile_commands.json"
    "${_fid}/proj/msbdemo/.ccg/Debug-x64/compile_commands.json")
py_run("捕获保真 stub-vs-capture fidelity" [==[
import glob, json, posixpath, sys
stub_dir, cdb_a, cdb_b = sys.argv[1], sys.argv[2], sys.argv[3]
SEP_DROP = {"/Fo", "/Fd", "/Fp", "/Yc", "/Yu"}
JOINED_DROP = ("/Fo", "/Fd", "/Fp", "/Yc", "/Yu")
JOINED_INCLUDE = ("/external:I", "/FI", "/I")
BLACKLIST = {"/Gf", "/u-"}
IGNORE_INCLUDE = {"/X", "/x", "-X", "-x"}
SEP_INCLUDE = {"/I", "/external:I", "/FI"}
# MSVC 模块带值开关六项（与 cg.util.msvcswitch 单点清单对表——
# 本脚本的 Python 复刻是无法 import C++ 模块的登记同步点）：
# 桩侧成对透传，对称翻译层的分值保留投影。
MODULE_VALUED = {"/ifcOutput", "/scanDependencies", "/sourceDependencies",
                 "/ifcSearchDir", "/reference", "/exportHeader"}
# 权威集 = util.ixx 的 is_tu_input_file（classify_source_path 表
# CSource/CppSource/ModuleUnit 三类的并集）。
SOURCE_EXTS = {".cpp", ".cxx", ".cc", ".c++", ".c", ".cppm", ".cxxm",
               ".ixx", ".mpp", ".mxx", ".c++m"}
def to_abs(value, cwd):
    # 翻译层 detail::to_abs 的同语义复刻：反斜杠/引号清理、相对
    # 路径拼 cwd、normpath（lexically_normal 的纯文本等价）、盘符小
    # 写（util::norm_path 存储规范化的同款——翻译层输出一律小写盘
    # 符；桩侧原始 argv 的盘符大小写随 wrap 层把工程路径交给
    # MSBuild 时的解析形态而定，不同工具链构建的可执行可不同，投影
    # 时必须抹平，否则跨树对照假红）。
    cleaned = value.replace(chr(92), "/").replace('"', "")
    absolute = (len(cleaned) >= 3 and cleaned[0].isalpha()
                and cleaned[1] == ":" and cleaned[2] == "/")
    if not absolute and not cleaned.startswith("/"):
        cleaned = cwd.rstrip("/") + "/" + cleaned
    out = posixpath.normpath(cleaned)
    if len(out) >= 2 and out[1] == ":" and "A" <= out[0] <= "Z":
        out = out[0].lower() + out[1:]
    return out
def norm_cwd(value):
    # 桩记录 cwd 的投影：与 cdb directory 同为 ccg 存储规范化形态
    # （反斜杠转斜杠 + 盘符小写），理由同上。
    out = value.replace(chr(92), "/")
    if len(out) >= 2 and out[1] == ":" and "A" <= out[0] <= "Z":
        out = out[0].lower() + out[1:]
    return out
calls = []
for path in sorted(glob.glob(stub_dir + "/*.jsonl")):
    rec = json.load(open(path, encoding="utf-8"))
    argv, cwd = rec["argv"], norm_cwd(rec["cwd"])
    flags, sources = [], []
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == "/D" and i + 1 < len(argv):
            flags.append(a + argv[i + 1])
            i += 2
            continue
        if a in SEP_INCLUDE and i + 1 < len(argv):
            flags.append(a)
            flags.append(to_abs(argv[i + 1], cwd))
            i += 2
            continue
        joined = next((p for p in JOINED_INCLUDE
                       if a.startswith(p) and len(a) > len(p)), None)
        if joined:
            flags.append(joined + to_abs(a[len(joined):], cwd))
            i += 1
            continue
        if a in MODULE_VALUED:
            # 六项带值开关成对透传（对称翻译层 sep_value_kept 投影）：
            # 开关与后随值 token 都进 flags、值逐字不绝对化；末 token
            # 裸开关只保开关（i+1 越界不吞），与翻译层 i+1<n 守卫一致。
            flags.append(a)
            if i + 1 < len(argv):
                flags.append(argv[i + 1])
                i += 1
            i += 1
            continue
        if a in SEP_DROP:
            if (i + 1 < len(argv) and not argv[i + 1].startswith(("/", "-"))
                    and posixpath.splitext(argv[i + 1])[1].lower()
                    not in SOURCE_EXTS):
                i += 1
            i += 1
            continue
        if a in BLACKLIST:
            i += 1
            continue
        if a in IGNORE_INCLUDE:
            i += 1
            continue
        if a.startswith(JOINED_DROP):
            i += 1
            continue
        if a and a[0] not in "/-":
            if posixpath.splitext(a)[1].lower() in SOURCE_EXTS:
                sources.append(to_abs(a, cwd))
            i += 1
            continue
        flags.append(a)
        i += 1
    calls.append({"cwd": cwd, "flags": flags, "sources": sources})
TRANSLATED = ("-finput-charset=", "-fms-compatibility-version=")
MATERIALIZED = {"-imsvc", "/X", "/Zc:twoPhase-"}
entries = []
for p in (cdb_a, cdb_b):
    for e in json.load(open(p, encoding="utf-8")):
        argv = e["arguments"]
        assert argv[0] == "clang-cl" and argv[1] == "--driver-mode=cl", argv[:2]
        flags, skip_value = [], False
        for tok in argv[2:]:
            if skip_value:
                skip_value = False
                continue
            if tok == "-imsvc":
                skip_value = True
                continue
            if tok.startswith(TRANSLATED) or tok in MATERIALIZED:
                continue
            flags.append(tok)
        entries.append({"cwd": e["directory"],
                        "file": e["file"].replace(chr(92), "/"),
                        "flags": flags})
total = sum(len(c["sources"]) for c in calls)
assert total == len(entries), "entry count %d != stub sources %d" % (
    len(entries), total)
for e in entries:
    for c in calls:
        if (c["cwd"] == e["cwd"] and e["file"] in c["sources"]
                and c["flags"] == e["flags"]):
            c["sources"].remove(e["file"])
            break
    else:
        raise AssertionError(("cdb entry has no stub call",
                              e["cwd"], e["file"], e["flags"]))
for c in calls:
    assert not c["sources"], ("stub source missing from the capture",
                              c["cwd"], c["sources"])
print("fidelity-ok")
]==])

# ------------------------------------------------------------ 求值通道默认 --
# 求值通道验收段（默认通道=evaluate；上文各记录面闸门已显式 --record）。
# 索引断言的确定性：求值直读本进程 INCLUDE 组装 -imsvc（记录臂捕获的
# 是 MSBuild 子进程内 VS 初始化过的 INCLUDE，冷 shell 下两臂不同源），
# 各求值腿经 cmake -E env 注入 _captured_include（记录臂捕获的纯系统
# 目录集，与夹具树无关）——任何宿主 shell 下闸门同一答案。
# 哈希门闸（update 联动的快照吸收/写回）在工程文件联动段断言（本树
# 的失败保旧腿顺带钉死"E2 超时不重写快照"）。

# —— 求值基本闸：默认通道对 msbdemo 求值 ——
file(COPY "${fixture}/msbdemo" DESTINATION "${work}/eval")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "INCLUDE=${_captured_include}"
            -- "${exe}" msbuild "${work}/eval/msbdemo/msbdemo.sln"
            --config Debug --platform x64
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "求值基本闸 evaluate run")
expect_out("indexing 3 TUs" "求值基本闸 tu count")
expect_out("failed: 0" "求值基本闸 index clean")
# 求值不跑构建、不桩化工具：沙箱/桩目录/捕获日志/wrap 捕获库全部
# 不得产生（通道混淆——求值腿意外落进构建编排——在这里确定性现形）。
if(EXISTS "${work}/eval/msbdemo/.ccg/Debug-x64/msbuild-sandbox")
    fail("求值基本闸" "msbuild-sandbox must not exist for the evaluate channel")
endif()
if(EXISTS "${work}/eval/msbdemo/.ccg/Debug-x64/capture.db")
    fail("求值基本闸" "capture.db must not exist for the evaluate channel")
endif()
if(EXISTS "${work}/eval/msbdemo/.ccg/msbuild/Debug-x64/stub-tools")
    fail("求值基本闸" "stub-tools must not exist for the evaluate channel")
endif()
file(GLOB _eval_capture_logs
     "${work}/eval/msbdemo/.ccg/msbuild/Debug-x64/clwrap-*.jsonl")
if(_eval_capture_logs)
    fail("求值基本闸" "capture logs must not exist for the evaluate channel")
endif()
if(NOT EXISTS "${work}/eval/msbdemo/.ccg/Debug-x64/compile_commands.json")
    fail("求值基本闸" "compile_commands.json was not written")
endif()
set(py_args "${work}/eval/msbdemo/.ccg/msbuild.json")
py_run("求值基本闸 registry mode" [==[
import json, sys
with open(sys.argv[1], encoding="utf-8") as f:
    reg = json.load(f)
assert len(reg["configs"]) == 1, reg
c = reg["configs"][0]
assert c["name"] == "Debug-x64", c
assert c["mode"] == "evaluate", c
assert c["tus"] == 3 and c["failed"] == 0, c
print("eval-registry-ok")
]==])
ccg_run(msbuild --list "${work}/eval/msbdemo/msbdemo.sln")
expect_rc(0 "求值基本闸 list")
expect_out("evaluate" "求值基本闸 list mode column")
ccg_run(query demo::add -o "${work}/eval/msbdemo/.ccg" --config Debug-x64)
expect_rc(0 "求值基本闸 symbol queryable")
expect_out("demo::add" "求值基本闸 symbol queryable")
# 冷 shell 披露面（INCLUDE 同源论证的可观测面）：进程 INCLUDE 为空时
# 求值直读拿不到系统目录——note 行如实披露修复路径、不拦截（cdb 照常
# 产出；索引侧 per-TU 失败是权威判定，计数随引擎自探测能力而变、不做
# 环境相关断言）。unset 注入让本腿在任何宿主 shell 下同一答案。
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env --unset=INCLUDE
            -- "${exe}" msbuild "${work}/eval/msbdemo/msbdemo.sln"
            --config Debug --platform x64 --export-only
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "求值基本闸 cold-shell disclosure exit")
expect_out("3 TUs, evaluate" "求值基本闸 cold-shell cdb still composed")
expect_err("INCLUDE is empty in this process"
           "求值基本闸 cold-shell disclosure note")

# —— 双通道对账闸 + 性能锚：record 臂（对照真值）vs 求值臂 ——
# 同一夹具两臂 cdb 按 (cwd, file) 对账：六维语义桶全等（TU 覆盖 /
# defines 序 / include 序 / 警告族 / 运行库与异常模型 / 语言标准 /
# 一致性 / 调用约定 / TP-TC / 模块开关 / PCH / 工具集兼容版本）。
# 通道差异面（设计 2.7 表逐项，滤除清单与之一一对应）：
#   * 扫描趟旗标 /scanDependencies /sourceDependencies（含值）——
#     求值无扫描趟，record 侧整对滤除、求值侧出现即红；
#   * /ifcOutput 后随值——record=沙箱 IntDir、求值=真实 IntDir，
#     两侧同映射占位符（本夹具无 .ixx 不触发，规则登记在案）；
#   * -imsvc 面——INCLUDE 物化来源不同（wrap 捕获 vs 进程直读），
#     两侧同滤不比对；
#   * 条目数——.ixx 双趟只属 record（本夹具全 .cpp，两侧 1:1；
#     record 侧同 file 双条取编译趟的规则登记在案）；
#   * /Fo /Fd——sep_drop 剥离，两侧 cdb 都无。
# 语义桶外的 MSBuild CL 任务默认值开关（/c /JMC /nologo /Od /RTC1
# /GS /fp:precise /Zc:forScope /Zc:inline /FC /diagnostics:column
# /errorReport:queue 等）不在求值组装器的 37 字段映射面内——那是
# 六维判据（等价性以维度计，不以整 argv 计）的既定边界，登记在案。
foreach(_d DemoSln.sln msbdemo mathlib)
    file(COPY "${fixture}/${_d}" DESTINATION "${work}/dual")
endforeach()
ccg_run(msbuild "${work}/dual/DemoSln.sln" --config Debug --platform x64
        --record --export-only)
expect_rc(0 "双通道对账 record arm")
expect_out("5 TUs, record" "双通道对账 record arm summary")
file(RENAME "${work}/dual/.ccg/Debug-x64/compile_commands.json"
     "${work}/dual/record-cdb.json")
# 求值臂（默认通道）暖机墙钟锚：记录臂已填充工具链发现缓存。锚口径
# = 4.5 秒 / 59 工程（76ms/工程）的本夹具放宽版（2 工程 + 固定开销
# 实测约 1.3 秒；5 秒界 ≈ 3.8 倍余量，防多秒级/工程类回退——如逐
# 工程 -getProperty 风暴。通道混淆由上文产物断言确定性抓，不靠时序）。
string(TIMESTAMP _dual_t0 "%s%f")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "INCLUDE=${_captured_include}"
            -- "${exe}" msbuild "${work}/dual/DemoSln.sln"
            --config Debug --platform x64 --export-only
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
string(TIMESTAMP _dual_t1 "%s%f")
math(EXPR _dual_ms "(${_dual_t1} - ${_dual_t0}) / 1000")
expect_rc(0 "双通道对账 evaluate arm")
expect_out("5 TUs, evaluate" "双通道对账 evaluate arm summary")
if(_dual_ms GREATER 5000)
    fail("双通道对账" "evaluate export-only took ${_dual_ms} ms "
         "(relaxed performance anchor 5000 ms; scale source: "
         "4.5 s / 59 projects)")
endif()
message(STATUS "ccg_msbuild_e2e: evaluate arm warm wall clock ${_dual_ms} ms")
set(py_args "${work}/dual/record-cdb.json"
            "${work}/dual/.ccg/Debug-x64/compile_commands.json")
py_run("双通道对账 six-dimension projection" [==[
import json, re, sys
def buckets(args):
    b = {"defines": [], "includes": [], "wd": [], "we": [], "std": None,
         "warn": [], "eh": [], "runtime": [], "call": [], "conformance": [],
         "tp": False, "tc": False, "interface": False, "ifc": None,
         "scan": False, "pch_yc": False, "pch_yu": False, "pch_fp": None,
         "mscompat": None}
    i = 2
    while i < len(args):
        a = args[i]
        nxt = args[i + 1] if i + 1 < len(args) else None
        val_follows = nxt is not None and not nxt.startswith(("/", "-"))
        if a == "-imsvc":
            i += 2
            continue
        if a.startswith("-fms-compatibility-version="):
            b["mscompat"] = a.split("=", 1)[1]
            i += 1
            continue
        if a.startswith("/D"):
            b["defines"].append(a[2:])
        elif a.startswith("/I"):
            b["includes"].append(a[2:].replace(chr(92), "/").lower())
        elif a.startswith("/wd"):
            b["wd"].append(a[3:])
        elif a.startswith("/we"):
            b["we"].append(a[3:])
        elif a.startswith("/std:"):
            b["std"] = a[5:]
        elif re.match(r"^/W", a):
            b["warn"].append(a)
        elif re.match(r"^/EH", a):
            b["eh"].append(a)
        elif re.match(r"^/M[DT]", a):
            b["runtime"].append(a)
        elif a in ("/Gd", "/Gr", "/Gz", "/Gv"):
            b["call"].append(a)
        elif a in ("/permissive-", "/permissive"):
            b["conformance"].append(a)
        elif a == "/TP":
            b["tp"] = True
        elif a == "/TC":
            b["tc"] = True
        elif a == "/interface":
            b["interface"] = True
        elif a == "/ifcOutput" and val_follows:
            b["ifc"] = "<ifc-value>"
            i += 1
        elif a == "/scanDependencies":
            b["scan"] = True
        elif a.startswith("/Yc"):
            b["pch_yc"] = True
        elif a.startswith("/Yu"):
            b["pch_yu"] = True
        elif a.startswith("/Fp"):
            b["pch_fp"] = a[3:]
        i += 1
    return b
def load(path):
    out = {}
    for e in json.load(open(path, encoding="utf-8")):
        key = (e["directory"].replace(chr(92), "/").lower(),
               e["file"].replace(chr(92), "/").lower())
        out.setdefault(key, []).append(buckets(e["arguments"]))
    return out
rec, ev = load(sys.argv[1]), load(sys.argv[2])
assert set(rec) == set(ev), (
    "entry (cwd,file) sets differ",
    sorted(str(k) for k in set(rec) ^ set(ev))[:4])
DIMS = ("defines", "includes", "wd", "we", "std", "warn", "eh", "runtime",
        "call", "conformance", "tp", "tc", "interface", "ifc", "pch_yc",
        "pch_yu", "pch_fp", "mscompat")
for key in sorted(rec):
    name = key[1].rsplit("/", 1)[-1]
    assert len(ev[key]) == 1, ("evaluate must be single-pass per TU",
                               name, len(ev[key]))
    # record 侧同 file 双条（.ixx 扫描趟）取编译趟：无 /scanDependencies
    # 者；单条（全 .cpp 夹具常态）直取。
    entries = rec[key]
    pick = entries[0] if len(entries) == 1 else next(
        (x for x in entries if not x["scan"]), entries[0])
    diff = [(d, pick[d], ev[key][0][d]) for d in DIMS
            if pick[d] != ev[key][0][d]]
    assert not diff, ("channel dimension mismatch", name, diff)
    assert not ev[key][0]["scan"], ("evaluate emitted scan-pass flags",
                                    name)
print("dual-channel-ok")
]==])

# —— 模块求值闸：modmsb 走求值（单趟、模块单元判定、真实 IntDir）——
file(COPY "${fixture}/modmsb" DESTINATION "${work}/evalmod")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "INCLUDE=${_captured_include}"
            -- "${exe}" msbuild "${work}/evalmod/modmsb/modmsb.sln"
            --config Debug --platform x64
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "模块求值闸 evaluate run")
expect_out("indexing 2 TUs" "模块求值闸 tu count")
expect_out("failed: 0" "模块求值闸 index clean end to end")
expect_err("0 TU(s) degraded to tier-1"
           "模块求值闸 the interface BMI builds, nothing degrades")
set(py_args "${work}/evalmod/modmsb/.ccg/Debug-x64/compile_commands.json")
py_run("模块求值闸 single-pass module cdb shape" [==[
import json, sys
entries = json.load(open(sys.argv[1], encoding="utf-8"))
assert len(entries) == 2, len(entries)
mod = [e for e in entries
       if e["file"].replace(chr(92), "/").lower().endswith("/mod.ixx")]
use = [e for e in entries
       if e["file"].replace(chr(92), "/").lower().endswith("/use.cpp")]
assert len(mod) == 1 and len(use) == 1, (len(mod), len(use))
# 求值每 TU 单趟：.ixx 恰一条（记录臂是扫描+编译双条）；模块单元
# 判定规则的 e2e 实证：接口单元 /interface + 垫尾 /TP，消费单元 /TP
# 无 /interface。
margv = mod[0]["arguments"]
assert "/TP" in margv and "/interface" in margv, margv
uargv = use[0]["arguments"]
assert "/TP" in uargv and "/interface" not in uargv, uargv
# /ifcOutput 带值：真实 IntDir 形态（非沙箱路径——求值不建沙箱）。
i = margv.index("/ifcOutput")
val = margv[i + 1]
assert not val.startswith("/") and val.lower().endswith(".ifc"), val
assert "msbuild-sandbox" not in val.replace(chr(92), "/").lower(), val
assert "/scanDependencies" not in margv and "/sourceDependencies" not in margv, margv
print("eval-module-ok")
]==])
ccg_run(query g16_take -o "${work}/evalmod/modmsb/.ccg" --config Debug-x64)
expect_rc(0 "模块求值闸 consumer symbol queryable through the BMI")

# —— 求值失败保旧闸：E4 部分失败继续 / E5 缺 import 降级 / E2 看门狗 ——
# E4：坏 msbdemo + 好数学库 -> 部分成功语义（退出 0、披露行、好工程
# 产物照常组装、failed_projects 如实计数）。
foreach(_d DemoSln.sln msbdemo mathlib)
    file(COPY "${fixture}/${_d}" DESTINATION "${work}/evalfail")
endforeach()
file(WRITE "${work}/evalfail/msbdemo/msbdemo.vcxproj" "<Project>\n<Broken")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "INCLUDE=${_captured_include}"
            -- "${exe}" msbuild "${work}/evalfail/DemoSln.sln"
            --config Debug --platform x64 --export-only --json
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "求值失败保旧 E4 partial-failure exit")
expect_err("eval project failure: msbdemo" "求值失败保旧 E4 disclosure line")
file(WRITE "${work}/evalfail/e4-stdout.json" "${out}")
set(py_args "${work}/evalfail/e4-stdout.json"
            "${work}/evalfail/.ccg/Debug-x64/compile_commands.json")
py_run("求值失败保旧 E4 json counting + composed corpus" [==[
import json, sys
# --json 的机器行是 stdout 末行；求值无 MSBuild 子进程噪声，整段输出
# 通常只此一行，倒序解析兼容任何前缀噪声。
obj = None
for line in reversed(open(sys.argv[1], encoding="utf-8").read().splitlines()):
    line = line.strip()
    if not line:
        continue
    try:
        obj = json.loads(line)
        break
    except ValueError:
        continue
assert obj is not None, "no json machine line on stdout"
assert obj["mode"] == "evaluate", obj
assert obj["tus"] == 2, obj
assert obj["failed_projects"] == 1, obj
cdb = json.load(open(sys.argv[2], encoding="utf-8"))
files = [e["file"].replace(chr(92), "/").lower() for e in cdb]
assert len(cdb) == 2, files
assert all("/mathlib/" in f for f in files), files
print("e4-ok")
]==])
# E5：缺 import 的降级求值（MSB4019 -> IgnoreMissingImports 腿）继续
# 且披露 note 行，全量 TU 照常组装。先复原 E4 弄坏的 msbdemo（本腿
# 只降级 mathlib，两条披露面互不掺杂）。
file(COPY "${fixture}/msbdemo/msbdemo.vcxproj"
     DESTINATION "${work}/evalfail/msbdemo")
file(READ "${fixture}/mathlib/mathlib/mathlib.vcxproj" _e5_vcx)
string(REPLACE
    [==[<Import Project="$(VCTargetsPath)\Microsoft.Cpp.targets" />]==]
    [==[<Import Project="$(VCTargetsPath)\Microsoft.Cpp.targets" />
  <Import Project="ccg-gate-missing.props" />]==]
    _e5_vcx_patched "${_e5_vcx}")
if(_e5_vcx_patched STREQUAL _e5_vcx)
    fail("求值失败保旧 E5" "could not patch the copied mathlib.vcxproj (targets import line not found)")
endif()
file(WRITE "${work}/evalfail/mathlib/mathlib/mathlib.vcxproj" "${_e5_vcx_patched}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "INCLUDE=${_captured_include}"
            -- "${exe}" msbuild "${work}/evalfail/DemoSln.sln"
            --config Debug --platform x64
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "求值失败保旧 E5 degraded exit")
expect_err("evaluated with missing imports ignored: mathlib"
           "求值失败保旧 E5 degraded note line")
expect_out("indexing 5 TUs" "求值失败保旧 E5 full corpus composed")
expect_out("failed: 1" "求值失败保旧 E5 degraded corpus indexes (unicode TU only)")
# E2：看门狗超时（CCG_EVAL_TIMEOUT_MS=1）失败保旧——本配置 cdb 不写、
# 注册表不动、快照不重写（快照只由成功轮写就，失败轮必须字节级不动
# ——失败保旧同步保快照）。求值基本闸的树已有一轮成功产物（含快照），
# 是失败保旧的字节级基准。
file(SHA256 "${work}/eval/msbdemo/.ccg/Debug-x64/compile_commands.json"
     _e2_cdb_before)
file(SHA256 "${work}/eval/msbdemo/.ccg/msbuild.json" _e2_reg_before)
file(SHA256 "${work}/eval/msbdemo/.ccg/msbuild/Debug-x64/snapshot.json"
     _e2_snap_before)
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "CCG_EVAL_TIMEOUT_MS=1"
            -- "${exe}" msbuild "${work}/eval/msbdemo/msbdemo.sln"
            --config Debug --platform x64
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(1 "求值失败保旧 E2 watchdog exit")
expect_err("project refresh aborted" "求值失败保旧 E2 failure-keep notice")
expect_err("timed out" "求值失败保旧 E2 watchdog diagnosis")
file(SHA256 "${work}/eval/msbdemo/.ccg/Debug-x64/compile_commands.json"
     _e2_cdb_after)
file(SHA256 "${work}/eval/msbdemo/.ccg/msbuild.json" _e2_reg_after)
file(SHA256 "${work}/eval/msbdemo/.ccg/msbuild/Debug-x64/snapshot.json"
     _e2_snap_after)
if(NOT _e2_cdb_after STREQUAL _e2_cdb_before)
    fail("求值失败保旧 E2" "the cdb was rewritten despite the watchdog abort")
endif()
if(NOT _e2_reg_after STREQUAL _e2_reg_before)
    fail("求值失败保旧 E2" "the registry was rewritten despite the watchdog abort")
endif()
if(NOT _e2_snap_after STREQUAL _e2_snap_before)
    fail("求值失败保旧 E2" "the refresh snapshot was rewritten despite the watchdog abort")
endif()

message(STATUS "ccg_msbuild_e2e: all gates passed")
