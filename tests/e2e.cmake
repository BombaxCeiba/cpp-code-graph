# 通过真实 ccg.exe 的端到端覆盖：把 sample1 夹具
# 索引进临时数据库，重跑验证增量契约，
# 触碰一个文件，然后逐个跑所有查询命令，按 CLI 契约
# （0 成功、1 未找到/歧义/运行时错误、2 用法）
# 断言期望行与退出码。
#
# 一个文件的两副面孔：
#   从 tests/CMakeLists.txt INCLUDE 进来 -> 注册 ccg_e2e 这个 ctest。
#   经 `cmake -P e2e.cmake <ccg.exe> <fixture-dir> <work-dir>` 运行。

if(NOT CMAKE_SCRIPT_MODE_FILE)
    add_test(NAME ccg_e2e
             COMMAND ${CMAKE_COMMAND} -P
                     "${CMAKE_CURRENT_SOURCE_DIR}/e2e.cmake"
                     $<TARGET_FILE:ccg>
                     "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sample1"
                     "${CMAKE_BINARY_DIR}/test-fixture")
    ccg_test_path_mods(_e2e_mods)
    set_tests_properties(ccg_e2e PROPERTIES
        ENVIRONMENT_MODIFICATION "${_e2e_mods}"
        TIMEOUT 480)
    return()
endif()

if(NOT DEFINED CMAKE_ARGV5)
    message(FATAL_ERROR "e2e.cmake: expected <ccg.exe> <fixture-dir> <work-dir>")
endif()
set(exe     "${CMAKE_ARGV3}")
set(fixture "${CMAKE_ARGV4}")
set(work    "${CMAKE_ARGV5}")

# 运行/断言宏组（ccg_run/expect_rc/expect_out/expect_err）
# 收纳在同目录 ccg-harness.cmake，供各闸门脚本共享。
include("${CMAKE_CURRENT_LIST_DIR}/ccg-harness.cmake")

# ---------------------------------------------------------------- 索引 ------
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}")

ccg_run(index "${fixture}" -o "${work}" --config main)
expect_rc(0 "index fresh")
expect_out("indexing 3 TUs" "index fresh")
expect_out("fresh: 3" "index fresh")
expect_out("failed: 0" "index fresh")
expect_out("symbols: 35" "index fresh")
expect_out("edges: 28" "index fresh")
expect_out("unresolved edges dropped: 0" "index fresh")

ccg_run(index "${fixture}" -o "${work}" --config main)
expect_rc(0 "index incremental")
expect_out("skipped: all" "index incremental")
expect_out("stale: 0" "index incremental")
expect_out("mode: incremental" "index incremental")

# 干跑：计划只打印，不碰数据库。
ccg_run(index "${fixture}" -o "${work}" --config main --dry-run)
expect_rc(0 "index dry-run")
expect_out("dry run" "index dry-run")
expect_out("plan: 0 of 3 TUs" "index dry-run")
ccg_run(index "${fixture}" -o "${work}" --config never --dry-run)
expect_rc(0 "index dry-run missing db")
expect_out("mode: full" "index dry-run missing db")
if(EXISTS "${work}/never/ccg.db")
    message(FATAL_ERROR "e2e [dry-run]: --dry-run created a database file")
endif()

# 编译数据库是前置条件：没有它的树（哪怕有 C++
# 源码）必须以可操作的错误失败，而不是用猜测的
# 旗标去扫描。
file(MAKE_DIRECTORY "${work}/nocdb")
file(WRITE "${work}/nocdb/main.cpp" "int main() { return 0; }\n")
ccg_run(index "${work}/nocdb" -o "${work}" --config nocdb)
expect_rc(2 "index without compile db")
expect_err("no compile_commands.json found under" "index without compile db")
expect_err("CMAKE_EXPORT_COMPILE_COMMANDS=ON" "index without compile db cmake hint")
expect_err("ccg msbuild" "index without compile db msbuild hint")
expect_err("--compile-commands <file>" "index without compile db flag hint")
if(EXISTS "${work}/nocdb/ccg.db")
    message(FATAL_ERROR "e2e [nocdb]: the failed run created a database")
endif()

# 复制夹具树（夹具是只读输入）并只触碰一个文件：
# 恰好只有那个 TU 重索引。
file(COPY "${fixture}/" DESTINATION "${work}/copy")
ccg_run(index "${work}/copy" -o "${work}" --config copy)
expect_rc(0 "index copy")
expect_out("fresh: 3" "index copy")
# 同一夹具走并行池必须落相同的行。
ccg_run(index "${work}/copy" -o "${work}" --config copy-jobs --jobs 2)
expect_rc(0 "index jobs pool")
expect_out("fresh: 3" "index jobs pool")
expect_out("symbols: 35" "index jobs pool")
expect_out("edges: 28" "index jobs pool")
file(APPEND "${work}/copy/circle.cpp" "\n// e2e touch comment\n")
ccg_run(index "${work}/copy" -o "${work}" --config copy)
expect_rc(0 "index touched")
expect_out("stale: 1" "index touched")
expect_out("skipped: 2" "index touched")

# ---------------------------------------------------------------- 更新 ----
# 经真实二进制的实时一次性更新：数据库是
# 协调者（pending_updates + update_claim，schema v6）——入队 +
# 认领 + take 排空走普通增量引擎，崩溃尾巴由
# --drain 收敛，协调旋钮走 ccg config，
# 还有对同一树状态的普通增量索引的等价性
# 绊网对照。
file(COPY "${fixture}/" DESTINATION "${work}/rt")
ccg_run(index "${work}/rt" -o "${work}" --config rt)
expect_rc(0 "index rt fixture")
expect_out("fresh: 3" "index rt fixture")
file(APPEND "${work}/rt/circle.cpp" "\nint rt_added_fn() { return 42; }\n")

# 直连路径：单进程内入队 + 认领 + 排空；新符号
# 经播种的增量引擎落库。
ccg_run(update "${work}/rt/circle.cpp" -o "${work}" --config rt)
expect_rc(0 "update direct")
expect_out("stale: 1" "update direct")
ccg_run(query rt_added_fn -o "${work}" --config rt)
expect_rc(0 "query direct update symbol")
expect_out("rt_added_fn" "query direct update symbol")

# 安静排空退出码 0；被推迟的派发分析（C 方案）在这里
# 收敛，因为上面的直连更新留下了 dispatch_stale。
ccg_run(update --drain -o "${work}" --config rt)
expect_rc(0 "update drain quiet")
expect_out("dispatch-converge" "update drain quiet")

# 崩溃尾巴收敛：入队之后失败的更新（这里：
# 记录的编译数据库消失）会把队列行留下——
# 入队本身就是一次已提交的事务。世界恢复理智后下一次 --drain
# 会把队列收敛（无守护进程的尾巴故事）。
file(APPEND "${work}/rt/canvas.cpp" "\nint rt_tail_fn() { return 3; }\n")
file(RENAME "${work}/rt/compile_commands.json" "${work}/rt/cc.json.hold")
ccg_run(update "${work}/rt/canvas.cpp" -o "${work}" --config rt)
expect_rc(1 "update failing run still enqueues")
expect_err("missing" "update failing run still enqueues")
file(RENAME "${work}/rt/cc.json.hold" "${work}/rt/compile_commands.json")
ccg_run(update --drain -o "${work}" --config rt)
expect_rc(0 "update drain converges the crashed tail")
expect_out("stale: 1" "update drain converges the crashed tail")
ccg_run(query rt_tail_fn -o "${work}" --config rt)
expect_rc(0 "query tail-converged symbol")
expect_out("rt_tail_fn" "query tail-converged symbol")

# 协调旋钮在 ccg config 里（默认值来自 schema）。
ccg_run(config set update-lease-ms 120000 -o "${work}" --config rt)
expect_rc(0 "config set update-lease-ms")
ccg_run(config get update-lease-ms -o "${work}" --config rt)
expect_rc(0 "config get update-lease-ms")
expect_out("update-lease-ms: 120000" "config get update-lease-ms")
ccg_run(config unset update-lease-ms -o "${work}" --config rt)
expect_rc(0 "config unset update-lease-ms")
ccg_run(config get update-lease-ms -o "${work}" --config rt)
expect_rc(0 "config get shows the default")
expect_out("(default)" "config get shows the default")
ccg_run(config set update-lease-ms 0 -o "${work}" --config rt)
expect_rc(1 "config set update-lease-ms 0 rejected")

# 等价性绊网（硬规则）：对同一个最终树状态，
# 经 update 刷新的库与普通增量 `ccg index` 的库得出
# 相同的行——经由产品表面比较（运行摘要的
# 计数与 stats 行）。
file(COPY "${fixture}/" DESTINATION "${work}/eq-a")
file(COPY "${fixture}/" DESTINATION "${work}/eq-b")
ccg_run(index "${work}/eq-a" -o "${work}" --config eq-a)
expect_rc(0 "equivalence baseline index a")
ccg_run(index "${work}/eq-b" -o "${work}" --config eq-b)
expect_rc(0 "equivalence baseline index b")
file(APPEND "${work}/eq-a/circle.cpp" "\nint eq_fn() { return 7; }\n")
file(APPEND "${work}/eq-b/circle.cpp" "\nint eq_fn() { return 7; }\n")
ccg_run(index "${work}/eq-a" -o "${work}" --config eq-a --json)
expect_rc(0 "equivalence incremental index")
set(a_sum "${out}")
ccg_run(update "${work}/eq-b/circle.cpp" -o "${work}" --config eq-b --json)
expect_rc(0 "equivalence update")
set(b_sum "${out}")
foreach(_f symbols edges includes imports)
    string(REGEX MATCH "\"${_f}\":[0-9]+" _a "${a_sum}")
    string(REGEX MATCH "\"${_f}\":[0-9]+" _b "${b_sum}")
    if(_a STREQUAL "" OR NOT _a STREQUAL _b)
        message(FATAL_ERROR
            "e2e [equivalence]: ${_f} differs between index and update:\nA: ${_a}\nB: ${_b}\n--index--\n${a_sum}\n--update--\n${b_sum}")
    endif()
endforeach()
ccg_run(stats -o "${work}" --config eq-a)
set(a_stats "${out}")
ccg_run(stats -o "${work}" --config eq-b)
foreach(_row symbols edges includes imports)
    string(REGEX MATCH "${_row}[ ]+[0-9]+" _a "${a_stats}")
    string(REGEX MATCH "${_row}[ ]+[0-9]+" _b "${out}")
    if(_a STREQUAL "" OR NOT _a STREQUAL _b)
        message(FATAL_ERROR
            "e2e [equivalence]: stats row ${_row} differs:\nA: ${_a}\nB: ${_b}\n--index--\n${a_stats}\n--update--\n${out}")
    endif()
endforeach()
ccg_run(query eq_fn -o "${work}" --config eq-b)
expect_rc(0 "query equivalence symbol")
expect_out("eq_fn" "query equivalence symbol")

# 错误路径 + 用法。
ccg_run(update x.cpp -o "${work}" --config no-such-rt)
expect_rc(1 "update missing db")
ccg_run(update)
expect_rc(2 "update bare usage")
ccg_run(help update)
expect_rc(0 "help update")
expect_out("usage: ccg update" "help update")
expect_out("DATABASE is the" "help update names db coordination")

# ----------------------------------------------------------------- 查询 -----
ccg_run(query area -o "${work}" --config main)
expect_rc(0 "query hit")
expect_out("shape::Circle::area" "query hit")
expect_out("shape::Shape::area" "query hit")
expect_out("(tier: exact + substring)" "query hit")

ccg_run(query area --json -o "${work}" --config main)
expect_rc(0 "query json")
string(SUBSTRING "${out}" 0 1 _first)
if(NOT _first STREQUAL "{")
    message(FATAL_ERROR "e2e [query json]: stdout is not JSON:\n${out}")
endif()
expect_out("\"tier\":\"exact\"" "query json")
expect_out("\"qualified_name\":\"shape::Circle::area\"" "query json")

ccg_run(query areaa -o "${work}" --config main)
expect_rc(1 "query miss")
expect_err("no symbol matching 'areaa'" "query miss")
expect_err("did you mean" "query miss")

# -------------------------------------------------------------- 上下文 -----
ccg_run(context shape::Circle::area -o "${work}" --config main)
expect_rc(0 "context hit")
expect_out("symbol  method shape::Circle::area" "context hit")
expect_out("incoming" "context hit")
expect_out("render::Canvas::total_area" "context hit")
expect_out("overrides" "context hit")
expect_out("shape::pi" "context hit")

ccg_run(context shape::Circle::area --json -o "${work}" --config main)
expect_rc(0 "context json")
expect_out("\"incoming\"" "context json")
expect_out("\"outgoing\"" "context json")

ccg_run(context area -o "${work}" --config main)
expect_rc(1 "context ambiguous")
expect_err("ambiguous" "context ambiguous")
expect_err("shape::Circle::area" "context ambiguous")

# ---------------------------------------------------- 调用者 / 被调者 -----
ccg_run(callers shape::Circle::area --depth 2 -o "${work}" --config main)
expect_rc(0 "callers")
expect_out("depth 1" "callers")
expect_out("render::Canvas::total_area" "callers")
expect_out("depth 2" "callers")
expect_out("app::area_all" "callers")
expect_out("(via render::Canvas::total_area)" "callers")

ccg_run(callees shape::Circle::area -o "${work}" --config main)
expect_rc(0 "callees")
expect_out("shape::pi" "callees")

ccg_run(callees shape::unused_helper -o "${work}" --config main)
expect_rc(1 "callees empty")
expect_err("no callees found" "callees empty")

# --------------------------------------------------------------- 影响面 -----
ccg_run(impact shape::Circle::area --direction up --depth 3 -o "${work}" --config main)
expect_rc(0 "impact")
expect_out("risk     LOW" "impact")
expect_out("impacted 3 (direct: 1" "impact")
expect_out("truncated: no" "impact")
expect_out("deeper edges exist: yes" "impact")

# ---------------------------------------------------------------- 路径 -----
ccg_run(trace main shape::pi -o "${work}" --config main)
expect_rc(0 "trace found")
expect_out("found: main -> shape::pi  (5 hops)" "trace found")
expect_out("--calls--> shape::pi" "trace found")

ccg_run(trace shape::pi main -o "${work}" --config main)
expect_rc(1 "trace unreachable")
expect_err("no path from shape::pi to main" "trace unreachable")

ccg_run(trace shape::Circle::area main -o "${work}" --config main)
expect_rc(1 "trace unreachable reverse")

ccg_run(trace --json main shape::nosuch -o "${work}" --config main)
expect_rc(1 "trace json unresolved endpoint")
expect_out("\"error\"" "trace json unresolved endpoint")

# ---------------------------------------------------------------- 统计 -----
ccg_run(stats -o "${work}" --config main)
expect_rc(0 "stats")
expect_out("schema 18" "stats")
expect_out("symbols" "stats")

ccg_run(stats --json -o "${work}" --config main)
expect_rc(0 "stats json")
expect_out("\"symbols\"" "stats json")

ccg_run(stats -o "${work}" --config no-such)
expect_rc(1 "stats missing db")
expect_err("file not found" "stats missing db")

# --------------------------------------------------------------- 变更 ----
# argv 模式：一个已知头文件 -> 图中所有消费者，退出码 0。
ccg_run(changes "${fixture}/circle.hpp" -o "${work}" --config main)
expect_rc(0 "changes known file")
expect_out("known 1, deleted 0, unknown 0" "changes known file")
expect_out("render::Canvas::circles_" "changes known file")
expect_out("depth 2" "changes known file")
expect_out("app::area_all" "changes known file")

# 只有一个未知路径 -> 诚实报告，退出码 1（什么都没找到）。
ccg_run(changes "${fixture}/definitely-missing.cpp" -o "${work}" --config main)
expect_rc(1 "changes unknown only")
expect_out("unknown" "changes unknown only")
expect_out("not in the database" "changes unknown only")

# --from-file：带引号行、去注释去重、已知与未知混合。
file(WRITE "${work}/changed.txt"
     "  \"${fixture}/circle.hpp\"\n${fixture}/circle.hpp\n${fixture}/nope.cpp\n")
ccg_run(changes --from-file "${work}/changed.txt" -o "${work}" --config main)
expect_rc(0 "changes from-file")
expect_out("2 files fed (known 1, deleted 0, unknown 1)" "changes from-file")
expect_out("not in the database" "changes from-file")

# 已删除文件：库在下一次索引前仍记得它的符号。
file(REMOVE "${work}/copy/canvas.cpp")
ccg_run(changes "${work}/copy/canvas.cpp" -o "${work}" --config copy)
expect_rc(0 "changes deleted file")
expect_out("deleted" "changes deleted file")
expect_out("app::area_all" "changes deleted file")

# --depth 1 只保留直接一层。
ccg_run(changes "${fixture}/circle.hpp" -o "${work}" --config main --depth 1)
expect_rc(0 "changes depth 1")
expect_out("transitive 0" "changes depth 1")

# --json 形态。
ccg_run(changes "${fixture}/circle.hpp" -o "${work}" --config main --json)
expect_rc(0 "changes json")
expect_out("\"status\":\"present\"" "changes json")
expect_out("\"viaSeed\":\"shape::Circle\"" "changes json")
expect_out("\"consumers\"" "changes json")

# stdin 管道模式（显式 '-' 操作数与无操作数的管道形态）。
file(WRITE "${work}/pipe.txt" "${fixture}/shape.hpp\n")
execute_process(COMMAND "${exe}" changes - -o "${work}" --config main
    INPUT_FILE "${work}/pipe.txt"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "e2e [changes stdin dash]: exit ${rc}\n--stdout--\n${out}\n--stderr--\n${err}")
endif()
if(NOT out MATCHES "shape::Circle::area")
    message(FATAL_ERROR "e2e [changes stdin dash]: override family missing:\n${out}")
endif()
execute_process(COMMAND "${exe}" changes -o "${work}" --config main
    INPUT_FILE "${work}/pipe.txt"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "e2e [changes stdin piped]: exit ${rc}\n--stdout--\n${out}\n--stderr--\n${err}")
endif()
if(NOT out MATCHES "known 1")
    message(FATAL_ERROR "e2e [changes stdin piped]: file not mapped:\n${out}")
endif()

# 缺库 -> 运行时错误，退出码 1。
ccg_run(changes "${fixture}/circle.hpp" -o "${work}" --config no-such)
expect_rc(1 "changes missing db")
expect_err("file not found" "changes missing db")

# ----------------------------------------------------------------- 检查 ----
# 脚本模式（-P）下 CMAKE_CURRENT_SOURCE_DIR 相对的是测试
# 工作目录，所以兄弟夹具目录改从
# sample1 夹具路径（CMAKE_ARGV4）推导。
get_filename_component(fixtures_dir "${fixture}" DIRECTORY)
set(cycle_dir "${fixtures_dir}/cycle")
set(diamond_dir "${fixtures_dir}/diamond")
set(selfinc_dir "${fixtures_dir}/selfinc")

ccg_run(index "${cycle_dir}" -o "${work}" --config cycle)
expect_rc(0 "index cycle fixture")
expect_out("failed: 0" "index cycle fixture")
ccg_run(check --cycles -o "${work}" --config cycle)
expect_rc(3 "check cycles found")
expect_out("include cycles   1 group (2 files, 2 edges)" "check cycles found")
expect_out("cycle/a.hpp -> " "check cycles found")
expect_out("cycle/b.hpp -> " "check cycles found")
expect_out("truncated: no" "check cycles found")

ccg_run(check --cycles --json -o "${work}" --config cycle)
expect_rc(3 "check cycles json")
expect_out("\"cycles\":true" "check cycles json")
expect_out("\"groupsTotal\":1" "check cycles json")

# 菱形夹具：共享底层，无环。
ccg_run(index "${diamond_dir}" -o "${work}" --config diamond)
expect_rc(0 "index diamond fixture")
ccg_run(check --cycles -o "${work}" --config diamond)
expect_rc(0 "check diamond clean")
expect_out("none" "check diamond clean")

# 自包含头文件：单文件分组带一条自环边。
ccg_run(index "${selfinc_dir}" -o "${work}" --config selfinc)
expect_rc(0 "index selfinc fixture")
ccg_run(check --cycles -o "${work}" --config selfinc)
expect_rc(3 "check self include")
expect_out("cycle 1 (1 file)" "check self include")
expect_out("selfinc/s.hpp -> " "check self include")

# 多环夹具（一个编译数据库、两个 TU）：一个库里 2 个分组；
# --max 只封顶列表条数，从不封顶总计。
ccg_run(index "${fixtures_dir}/cycmulti" -o "${work}" --config multi)
expect_rc(0 "index multi-cycle fixture")
expect_out("fresh: 2" "index multi-cycle fixture")
ccg_run(check --cycles -o "${work}" --config multi)
expect_rc(3 "check two groups")
expect_out("2 groups" "check two groups")
expect_out("(2 of 2 groups shown)" "check two groups")
ccg_run(check --cycles --max 1 -o "${work}" --config multi)
expect_rc(3 "check max cap")
expect_out("truncated: yes (1 of 2 groups shown)" "check max cap")

# 错误路径：无子命令的 check 是用法错误（2）；缺库是运行时错误（1）。
ccg_run(check -o "${work}" --config main)
expect_rc(2 "check without mode")
ccg_run(check --cycles -o "${work}" --config no-such-2)
expect_rc(1 "check missing db")

# ---------------------------------------------------------------- 用法 -----
ccg_run(bogus)
expect_rc(2 "unknown command")
ccg_run(query)
expect_rc(2 "missing operand")
ccg_run(query --kind wat x -o "${work}" --config main)
expect_rc(2 "bad kind")
ccg_run(--help)
expect_rc(0 "top help")
expect_out("usage: ccg" "top help")
ccg_run(callers --help)
expect_rc(0 "subcommand help")
expect_out("usage: ccg callers" "subcommand help")
ccg_run(help query)
expect_rc(0 "help topic")
expect_out("usage: ccg query" "help topic")
ccg_run(help changes)
expect_rc(0 "changes help topic")
expect_out("usage: ccg changes" "changes help topic")

# ------------------------------------------------------------------ 钩子 ------
# 经真实二进制的 Claude Code 集成闸门：临时工程目录里的
# settings 安装/卸载往返，以及两个 stdin-JSON
# 处理器入口。注册指向本可执行文件旁边的
# 弹跳脚本（publish/ 的 POST_BUILD 拷贝），所以这些闸门
# 同时证明发布布局已就位。

# 带显式工作目录的 ccg_run（hook --project 编辑
# <cwd>/.claude/settings.json 该文件）。
macro(ccg_run_in _dir)
    set(_cmd "${exe}")
    foreach(_a ${ARGN})
        list(APPEND _cmd "${_a}")
    endforeach()
    execute_process(COMMAND ${_cmd}
        WORKING_DIRECTORY "${_dir}"
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE)
endmacro()

set(hook_proj "${work}/hookproj")
file(REMOVE_RECURSE "${hook_proj}")
file(MAKE_DIRECTORY "${hook_proj}")

# 全新目录上的 status：文件缺失，零条目。
ccg_run_in("${hook_proj}" hook status --project)
expect_rc(0 "hook status virgin")
expect_out("(missing)" "hook status virgin")

# 安装：全新的 settings.json 带两个事件；可执行文件旁边的
# 弹跳脚本必须存在（fail-closed 注册绝不写死路径）。
ccg_run_in("${hook_proj}" hook install --project)
expect_rc(0 "hook install")
expect_err("installed into" "hook install")
ccg_run_in("${hook_proj}" hook status --project --json)
expect_rc(0 "hook status after install")
expect_out("\"PostToolUse\"" "hook status after install")
expect_out("\"SessionStart\"" "hook status after install")
expect_out("\"UserPromptSubmit\"" "hook status after install")
expect_out("\"PreToolUse\"" "hook status after install")
expect_out("\"entries\":1" "hook status after install")
if(NOT EXISTS "${hook_proj}/.claude/settings.json")
    message(FATAL_ERROR "e2e [hook install]: settings.json not created")
endif()
file(READ "${hook_proj}/.claude/settings.json" _installed)
string(FIND "${_installed}" "ccg-posttooluse" _ptu)
string(FIND "${_installed}" "ccg-sessionstart" _sst)
string(FIND "${_installed}" "ccg-userpromptsubmit" _ups)
string(FIND "${_installed}" "ccg-pretooluse" _prtu)
if(_ptu EQUAL -1 OR _sst EQUAL -1 OR _ups EQUAL -1 OR _prtu EQUAL -1)
    message(FATAL_ERROR "e2e [hook install]: trampoline names missing from:\n${_installed}")
endif()
# 发布布局必须真的携带全部四个弹跳脚本（注册
# 在脚本缺失时 fail-closed；本闸门验证发布拷贝）。
get_filename_component(_exe_dir "${exe}" DIRECTORY)
foreach(_hook_name ccg-posttooluse ccg-sessionstart ccg-userpromptsubmit ccg-pretooluse)
    if(NOT EXISTS "${_exe_dir}/.ccg/hooks/${_hook_name}.cmd"
       AND NOT EXISTS "${_exe_dir}/.ccg/hooks/${_hook_name}.sh")
        message(FATAL_ERROR
            "e2e [hook install]: trampoline ${_hook_name} missing beside ${exe}")
    endif()
endforeach()

# 重装收敛：refresh 动词是替换而非累积。
ccg_run_in("${hook_proj}" hook install --project)
expect_rc(0 "hook reinstall")
expect_err("refreshed in" "hook reinstall")
ccg_run_in("${hook_proj}" hook status --project --json)
string(REGEX MATCHALL "\"entries\":[0-9]+" _counts "${out}")
list(LENGTH _counts _ncounts)
set(_own 0)
foreach(_c ${_counts})
    string(REGEX REPLACE "\"entries\":" "" _n "${_c}")
    math(EXPR _own "${_own} + ${_n}")
endforeach()
if(NOT _own EQUAL 4)
    message(FATAL_ERROR "e2e [hook reinstall]: expected 4 total own entries, saw ${_own}:\n${out}")
endif()

# 保留性：注释、用户设置与邻居用户钩子在安装
# 与卸载后都幸存；不带 --force 的卸载是干跑。
file(WRITE "${hook_proj}/.claude/settings.json" "{
  // user settings with comments
  \"model\": \"sonnet\",
  \"hooks\": {
    \"PostToolUse\": [
      {
        // my own hook, keep it
        \"matcher\": \"Bash\",
        \"hooks\": [
          {\"type\": \"command\", \"command\": \"echo user-hook\", \"timeout\": 5}
        ]
      }
    ],
    \"UserPromptSubmit\": [
      {
        // my own prompt hook, keep it too
        \"hooks\": [
          {\"type\": \"command\", \"command\": \"echo my-prompt-hook\", \"timeout\": 5}
        ]
      }
    ]
  }
}
")
ccg_run_in("${hook_proj}" hook install --project)
expect_rc(0 "hook install over user file")
file(READ "${hook_proj}/.claude/settings.json" _merged)
foreach(_needle "// user settings with comments" "// my own hook, keep it" "echo user-hook" "echo my-prompt-hook" "ccg-posttooluse" "ccg-userpromptsubmit")
    string(FIND "${_merged}" "${_needle}" _pos)
    if(_pos EQUAL -1)
        message(FATAL_ERROR "e2e [hook install over user file]: '${_needle}' missing from:\n${_merged}")
    endif()
endforeach()

ccg_run_in("${hook_proj}" hook uninstall --project)
expect_rc(0 "hook uninstall dry run")
expect_err("dry run" "hook uninstall dry run")
file(READ "${hook_proj}/.claude/settings.json" _after_dry)
if(NOT _after_dry STREQUAL _merged)
    message(FATAL_ERROR "e2e [hook uninstall dry run]: dry run modified the file")
endif()

ccg_run_in("${hook_proj}" hook uninstall --project --force)
expect_rc(0 "hook uninstall force")
expect_err("removed 4 hook commands" "hook uninstall force")
file(READ "${hook_proj}/.claude/settings.json" _after_rm)
foreach(_residue "ccg-posttooluse" "ccg-sessionstart" "ccg-userpromptsubmit" "ccg-pretooluse")
    string(FIND "${_after_rm}" "${_residue}" _pos)
    if(NOT _pos EQUAL -1)
        message(FATAL_ERROR "e2e [hook uninstall force]: ccg residue '${_residue}' left behind:\n${_after_rm}")
    endif()
endforeach()
foreach(_needle "// user settings with comments" "// my own hook, keep it" "echo user-hook" "echo my-prompt-hook")
    string(FIND "${_after_rm}" "${_needle}" _pos)
    if(_pos EQUAL -1)
        message(FATAL_ERROR "e2e [hook uninstall force]: '${_needle}' dropped:\n${_after_rm}")
    endif()
endforeach()

# 只含 ccg 条目的文件卸载后钩子面为空且无残留
#（临时工程自己的 settings 来自第一次安装）。
ccg_run_in("${hook_proj}" hook install --project)
expect_rc(0 "hook reinstall clean-file")
ccg_run_in("${hook_proj}" hook uninstall --project --force)
expect_rc(0 "hook uninstall clean-file")
ccg_run_in("${hook_proj}" hook status --project --json)
expect_rc(0 "hook status after clean uninstall")
string(REGEX REPLACE "\n" "" _flat "${out}")
string(FIND "${_flat}" "\"entries\":1" _pos)
if(NOT _pos EQUAL -1)
    message(FATAL_ERROR "e2e [hook uninstall clean-file]: residue after removing everything:\n${out}")
endif()
if(NOT EXISTS "${hook_proj}/.claude/settings.json")
    message(FATAL_ERROR "e2e [hook uninstall clean-file]: user's settings file was deleted")
endif()

# 处理器：stdin 上一条伪造的 PostToolUse 事件刷新文件并
# 以文档规定的信封作答；非编辑工具保持沉默。
file(WRITE "${work}/hook-ptu.json"
     "{\"hook_event_name\":\"PostToolUse\",\"tool_name\":\"Write\",\"tool_input\":{\"file_path\":\"${work}/rt/circle.cpp\"}}")
execute_process(COMMAND "${exe}" hook posttooluse -o "${work}" --config rt
    INPUT_FILE "${work}/hook-ptu.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "hook posttooluse handler")
expect_out("\"hookSpecificOutput\"" "hook posttooluse handler")
expect_out("\"additionalContext\"" "hook posttooluse handler")
expect_out("PostToolUse" "hook posttooluse handler")
file(WRITE "${work}/hook-skip.json"
     "{\"hook_event_name\":\"PostToolUse\",\"tool_name\":\"Bash\",\"tool_input\":{\"command\":\"ls\"}}")
execute_process(COMMAND "${exe}" hook posttooluse -o "${work}" --config rt
    INPUT_FILE "${work}/hook-skip.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "hook posttooluse skip path")
if(NOT out STREQUAL "")
    message(FATAL_ERROR "e2e [hook posttooluse skip path]: expected zero output, saw:\n${out}")
endif()

# 文件级门（两段式）之拒收态：被编辑文件是源码扩展但不在目标库语料面
#（nocdb/main.cpp 从未被 rt 配置索引）——干活之前立即拒收：零 stdout、
# 一行 stderr、enqueue-reject 审计行落库（hook status --json 可见）。
# 跨仓库会话的编辑事件从此不再被按会话目录发现的无关库吸进队列。
file(WRITE "${work}/hook-reject.json"
     "{\"hook_event_name\":\"PostToolUse\",\"tool_name\":\"Write\",\"tool_input\":{\"file_path\":\"${work}/nocdb/main.cpp\"}}")
execute_process(COMMAND "${exe}" hook posttooluse -o "${work}" --config rt
    INPUT_FILE "${work}/hook-reject.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "hook posttooluse corpus gate reject")
if(NOT out STREQUAL "")
    message(FATAL_ERROR "e2e [hook posttooluse corpus gate reject]: expected zero output, saw:\n${out}")
endif()
expect_err("not in corpus of rt" "hook posttooluse corpus gate reject")
ccg_run(hook status --project -o "${work}" --config rt --json)
expect_rc(0 "hook posttooluse corpus gate audit")
expect_out("enqueue-reject" "hook posttooluse corpus gate audit")
# run-start/run-finish 生命体征：一次 update 运行恰好一对，且本会话的
# coordEvents 数组（按 holder 过滤）包含它们。
ccg_run(update "${work}/rt/circle.cpp" -o "${work}" --config rt --json)
expect_rc(0 "hook posttooluse run audit pair")
expect_out("run-start" "hook posttooluse run audit pair")
expect_out("run-finish" "hook posttooluse run audit pair")

# 文件级门之工程文件态：.vcxproj 编辑走重采集联动通道（不做语料面点
# 查）；CMake 库无 msbuild 注册表 → 联动按未跟踪出队，注入如实报联动
# 结论（不再拒收、也不入 TU 解析批）。
file(WRITE "${work}/rt/gate-probe.vcxproj" "<Project/>\n")
file(WRITE "${work}/hook-projfile.json"
     "{\"hook_event_name\":\"PostToolUse\",\"tool_name\":\"Write\",\"tool_input\":{\"file_path\":\"${work}/rt/gate-probe.vcxproj\"}}")
execute_process(COMMAND "${exe}" hook posttooluse -o "${work}" --config rt
    INPUT_FILE "${work}/hook-projfile.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "hook posttooluse corpus gate project file")
# 注入文案的 ASCII 锚（跨进程的中文断言受控制台编码影响，不测）：
# 联动结论行必然带 msbuild 指引；update 转发的 stderr 证实走了联动
# 通道（no msbuild registry → untracked 出队）而非 TU 解析批。
expect_out("msbuild" "hook posttooluse corpus gate project file")
expect_out("gate-probe.vcxproj" "hook posttooluse corpus gate project file")
expect_err("no msbuild registry" "hook posttooluse corpus gate project file")

# SessionStart：对着活库的一条有界状态行。
ccg_run(hook sessionstart -o "${work}" --config rt)
expect_rc(0 "hook sessionstart handler")
expect_out("\"hookSpecificOutput\"" "hook sessionstart handler")
expect_out("SessionStart" "hook sessionstart handler")

# 缺库保持沉默（技能文档教的是 `ccg index`）。
ccg_run(hook sessionstart -o "${work}" --config never)
expect_rc(0 "hook sessionstart no db")
if(NOT out STREQUAL "")
    message(FATAL_ERROR "e2e [hook sessionstart no db]: expected zero output, saw:\n${out}")
endif()

# PreToolUse：一条伪造的 Grep 事件，其模式点名一个已索引符号时
# 以文档规定的信封与 V7.1 事实行作答；模式按顶层交替拆分逐分支
# 注入（多词清单）、含元字符形态在开关开启且过护栏时走受限正则
# 路径；其余非符号查找、未知符号保持沉默（仅在有价值时注入）。
file(WRITE "${work}/hook-prtu-hit.json"
     "{\"hook_event_name\":\"PreToolUse\",\"tool_name\":\"Grep\",\"tool_input\":{\"pattern\":\"pi\",\"output_mode\":\"content\"}}")
execute_process(COMMAND "${exe}" hook pretooluse -o "${work}" --config rt
    INPUT_FILE "${work}/hook-prtu-hit.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "hook pretooluse hit")
expect_out("\"hookSpecificOutput\"" "hook pretooluse hit")
expect_out("\"PreToolUse\"" "hook pretooluse hit")
expect_out("\"additionalContext\"" "hook pretooluse hit")
expect_out("[ccg] shape::pi(" "hook pretooluse hit")
# def 行的路径在位于调用 cwd 之下时相对化，其余情况
# 用裸文件名，所以此处只有文件名本身是稳定的。
expect_out("circle.cpp:" "hook pretooluse hit")
file(WRITE "${work}/hook-prtu-bash-nosearch.json"
     "{\"hook_event_name\":\"PreToolUse\",\"tool_name\":\"Bash\",\"tool_input\":{\"command\":\"ls\",\"description\":\"List files\"}}")
execute_process(COMMAND "${exe}" hook pretooluse -o "${work}" --config rt
    INPUT_FILE "${work}/hook-prtu-bash-nosearch.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "hook pretooluse bash without a search action")
if(NOT out STREQUAL "")
    message(FATAL_ERROR "e2e [hook pretooluse bash without a search action]: expected zero output, saw:\n${out}")
endif()
file(WRITE "${work}/hook-prtu-miss.json"
     "{\"hook_event_name\":\"PreToolUse\",\"tool_name\":\"Grep\",\"tool_input\":{\"pattern\":\"no_such_symbol_zz9\"}}")
execute_process(COMMAND "${exe}" hook pretooluse -o "${work}" --config rt
    INPUT_FILE "${work}/hook-prtu-miss.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "hook pretooluse no hit")
if(NOT out STREQUAL "")
    message(FATAL_ERROR "e2e [hook pretooluse no hit]: expected zero output, saw:\n${out}")
endif()
# 交替拆分：pi|area 按顶层 | 拆分逐分支查询，两分支各命中一个唯一
# 符号 → 多词候选清单（首行披露模式原文与合计命中、条目按分支序、
# 末行教单分支收敛）。
file(WRITE "${work}/hook-prtu-alternation.json"
     "{\"hook_event_name\":\"PreToolUse\",\"tool_name\":\"Grep\",\"tool_input\":{\"pattern\":\"pi|area\"}}")
execute_process(COMMAND "${exe}" hook pretooluse -o "${work}" --config rt
    INPUT_FILE "${work}/hook-prtu-alternation.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "hook pretooluse alternation multi-lookup")
expect_out("\"hookSpecificOutput\"" "hook pretooluse alternation multi-lookup")
expect_out("matched 3 symbols across 2 terms" "hook pretooluse alternation multi-lookup")
expect_out("[ccg] - shape::pi" "hook pretooluse alternation multi-lookup")
expect_out("[ccg] - shape::Circle::area" "hook pretooluse alternation multi-lookup")
expect_out("re-grep with one term to pin its facts" "hook pretooluse alternation multi-lookup")

# 选择器语义：调用形态模式仍能钉住唯一符号
#（五行答案）；有歧义的裸名以候选清单作答
#（shape::Circle::scale 在本夹具里是双重重载族）。
file(WRITE "${work}/hook-prtu-call.json"
     "{\"hook_event_name\":\"PreToolUse\",\"tool_name\":\"Grep\",\"tool_input\":{\"pattern\":\"pi\\\\(\"}}")
execute_process(COMMAND "${exe}" hook pretooluse -o "${work}" --config rt
    INPUT_FILE "${work}/hook-prtu-call.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "hook pretooluse call-shape pin")
expect_out("[ccg] shape::pi(" "hook pretooluse call-shape pin")
file(WRITE "${work}/hook-prtu-list.json"
     "{\"hook_event_name\":\"PreToolUse\",\"tool_name\":\"Grep\",\"tool_input\":{\"pattern\":\"scale\"}}")
execute_process(COMMAND "${exe}" hook pretooluse -o "${work}" --config rt
    INPUT_FILE "${work}/hook-prtu-list.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "hook pretooluse candidate list")
expect_out("matches 2 same-name symbols (no dominant one)" "hook pretooluse candidate list")
expect_out("[ccg] - shape::Circle::scale" "hook pretooluse candidate list")
expect_out("method, 1 refs @circle.cpp:20" "hook pretooluse candidate list")
expect_out("re-grep with a qualified name (e.g. shape::Circle::scale)" "hook pretooluse candidate list")
# 路径预过滤：命中唯一符号的路径保留
# 五行答案；排除所有同名候选的路径保持沉默。
file(WRITE "${work}/hook-prtu-path.json"
     "{\"hook_event_name\":\"PreToolUse\",\"tool_name\":\"Grep\",\"tool_input\":{\"pattern\":\"pi\",\"path\":\"circle.cpp\"}}")
execute_process(COMMAND "${exe}" hook pretooluse -o "${work}" --config rt
    INPUT_FILE "${work}/hook-prtu-path.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "hook pretooluse path keep")
expect_out("[ccg] shape::pi(" "hook pretooluse path keep")
file(WRITE "${work}/hook-prtu-pathmiss.json"
     "{\"hook_event_name\":\"PreToolUse\",\"tool_name\":\"Grep\",\"tool_input\":{\"pattern\":\"pi\",\"path\":\"nowhere.hpp\"}}")
execute_process(COMMAND "${exe}" hook pretooluse -o "${work}" --config rt
    INPUT_FILE "${work}/hook-prtu-pathmiss.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "hook pretooluse path miss")
if(NOT out STREQUAL "")
    message(FATAL_ERROR "e2e [hook pretooluse path miss]: expected zero output, saw:\n${out}")
endif()

# 四工具监听（matcher Grep|Bash|PowerShell|Search）：Bash 里的 rg 命令
# 与 Search 载荷直读经前置提取层汇入同一查询接口层；非查找命令、
# 多查找单元/多路径等真多意图形态保持沉默（宁漏勿猜），而单请求的
# 多 pattern（rg -e a -e b，OR 语义）与顶层交替同构、按多词并查救活。
file(WRITE "${work}/hook-prtu-bash-hit.json"
     "{\"hook_event_name\":\"PreToolUse\",\"tool_name\":\"Bash\",\"tool_input\":{\"command\":\"rg -n pi\",\"description\":\"Find pi uses\"}}")
execute_process(COMMAND "${exe}" hook pretooluse -o "${work}" --config rt
    INPUT_FILE "${work}/hook-prtu-bash-hit.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "hook pretooluse bash rg hit")
expect_out("\"hookSpecificOutput\"" "hook pretooluse bash rg hit")
expect_out("[ccg] shape::pi(" "hook pretooluse bash rg hit")
expect_out("circle.cpp:" "hook pretooluse bash rg hit")
# 多 pattern 并查翻转：rg 的多 -e 是 OR 语义，与顶层交替同构——
# 与 Grep 交替用例同款多词清单（四臂同闸门同语义的证据）。
file(WRITE "${work}/hook-prtu-bash-multipat.json"
     "{\"hook_event_name\":\"PreToolUse\",\"tool_name\":\"Bash\",\"tool_input\":{\"command\":\"rg -e pi -e area\"}}")
execute_process(COMMAND "${exe}" hook pretooluse -o "${work}" --config rt
    INPUT_FILE "${work}/hook-prtu-bash-multipat.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "hook pretooluse bash multi-pattern multi-lookup")
expect_out("matched 3 symbols across 2 terms" "hook pretooluse bash multi-pattern multi-lookup")
expect_out("[ccg] - shape::pi" "hook pretooluse bash multi-pattern multi-lookup")
expect_out("[ccg] - shape::Circle::area" "hook pretooluse bash multi-pattern multi-lookup")
file(WRITE "${work}/hook-prtu-bash-fixed.json"
     "{\"hook_event_name\":\"PreToolUse\",\"tool_name\":\"Bash\",\"tool_input\":{\"command\":\"rg -F 'IsOk()'\"}}")
execute_process(COMMAND "${exe}" hook pretooluse -o "${work}" --config rt
    INPUT_FILE "${work}/hook-prtu-bash-fixed.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "hook pretooluse bash fixed non-chain literal stays silent")
if(NOT out STREQUAL "")
    message(FATAL_ERROR "e2e [hook pretooluse bash fixed non-chain literal stays silent]: expected zero output, saw:\n${out}")
endif()
file(WRITE "${work}/hook-prtu-pwsh-nosearch.json"
     "{\"hook_event_name\":\"PreToolUse\",\"tool_name\":\"PowerShell\",\"tool_input\":{\"command\":\"Get-ChildItem -Recurse\"}}")
execute_process(COMMAND "${exe}" hook pretooluse -o "${work}" --config rt
    INPUT_FILE "${work}/hook-prtu-pwsh-nosearch.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "hook pretooluse powershell without a search action")
if(NOT out STREQUAL "")
    message(FATAL_ERROR "e2e [hook pretooluse powershell without a search action]: expected zero output, saw:\n${out}")
endif()
# PowerShell 查找命令：引擎不可用的环境（无 5.1 与 7 双缺）按设计
# 静默，可用环境必须给出与 Grep/Bash 同源的注入——先探测一次，
# 可用才上强断言（响亮跳过，形态照 injection-bytelock 的沙盒缺失
# 先例）。
file(WRITE "${work}/hook-prtu-pwsh-probe.json"
     "{\"hook_event_name\":\"PreToolUse\",\"tool_name\":\"PowerShell\",\"tool_input\":{\"command\":\"Select-String -Pattern 'pi'\"}}")
execute_process(COMMAND "${exe}" hook pretooluse -o "${work}" --config rt
    INPUT_FILE "${work}/hook-prtu-pwsh-probe.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "hook pretooluse powershell select-string probe")
if(out STREQUAL "")
    message(STATUS "e2e [hook pretooluse powershell]: engine unavailable on this machine, positive case skipped")
else()
    expect_out("[ccg] shape::pi(" "hook pretooluse powershell select-string hit")
    expect_out("circle.cpp:" "hook pretooluse powershell select-string hit")
endif()
# 引擎被测试钩子强制全禁时：确定性静默（降级链快速否决，不崩）。
execute_process(COMMAND ${CMAKE_COMMAND} -E env "CCG_PSHOST_FORBID=all"
    "${exe}" hook pretooluse -o "${work}" --config rt
    INPUT_FILE "${work}/hook-prtu-pwsh-probe.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "hook pretooluse powershell engine forbidden stays silent")
if(NOT out STREQUAL "")
    message(FATAL_ERROR "e2e [hook pretooluse powershell engine forbidden stays silent]: expected zero output, saw:\n${out}")
endif()
file(WRITE "${work}/hook-prtu-search-hit.json"
     "{\"hook_event_name\":\"PreToolUse\",\"tool_name\":\"Search\",\"tool_input\":{\"query\":\"pi\"}}")
execute_process(COMMAND "${exe}" hook pretooluse -o "${work}" --config rt
    INPUT_FILE "${work}/hook-prtu-search-hit.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "hook pretooluse search field hit")
expect_out("[ccg] shape::pi(" "hook pretooluse search field hit")
expect_out("circle.cpp:" "hook pretooluse search field hit")
# Search 臂交替（载荷字节串无反斜杠，字面竖线）：与 Grep/Bash 同闸门
# 同语义——多词清单（四臂一致的第 4 号证据）。
file(WRITE "${work}/hook-prtu-search-neg.json"
     "{\"hook_event_name\":\"PreToolUse\",\"tool_name\":\"Search\",\"tool_input\":{\"query\":\"pi|area\"}}")
execute_process(COMMAND "${exe}" hook pretooluse -o "${work}" --config rt
    INPUT_FILE "${work}/hook-prtu-search-neg.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "hook pretooluse search alternation multi-lookup")
expect_out("matched 3 symbols across 2 terms" "hook pretooluse search alternation multi-lookup")
expect_out("[ccg] - shape::pi" "hook pretooluse search alternation multi-lookup")
expect_out("[ccg] - shape::Circle::area" "hook pretooluse search alternation multi-lookup")
file(WRITE "${work}/hook-prtu-search-wild.json"
     "{\"hook_event_name\":\"PreToolUse\",\"tool_name\":\"Search\",\"tool_input\":{\"query\":\"pi\",\"path\":\"*.cpp\"}}")
execute_process(COMMAND "${exe}" hook pretooluse -o "${work}" --config rt
    INPUT_FILE "${work}/hook-prtu-search-wild.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "hook pretooluse search wildcard path stays silent")
if(NOT out STREQUAL "")
    message(FATAL_ERROR "e2e [hook pretooluse search wildcard path stays silent]: expected zero output, saw:\n${out}")
endif()

# ------------------------ PreToolUse 多查找形态（rtml 独立 config）---------
# 交替拆分与受限正则路径的形态学用例群：独立夹具库（拷 rt 先例）防
# digest.preToolUseFlushes 计数互染与 rt 状态污染；环境注入写法照
# CCG_PSHOST_FORBID 先例（cmake -E env）。载荷字节串澄清：下文 JSON
# 里的竖线一律是字面竖线（无反斜杠），正则元字符形如 \w 在 JSON 里
# 写作 \\w。
file(COPY "${fixture}/" DESTINATION "${work}/rtml")
ccg_run(index "${work}/rtml" -o "${work}" --config rtml)
expect_rc(0 "pretooluse multilookup index fixture")

# n1 噪声分支：pi|class 里 class 被噪声词表丢弃，单有效分支退化为
# 现行单链出口——与 #1（pi 五行）字节同构。
file(WRITE "${work}/hook-prtu-ml-noise.json"
     "{\"hook_event_name\":\"PreToolUse\",\"tool_name\":\"Grep\",\"tool_input\":{\"pattern\":\"pi|class\"}}")
execute_process(COMMAND "${exe}" hook pretooluse -o "${work}" --config rtml
    INPUT_FILE "${work}/hook-prtu-ml-noise.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "pretooluse noise branch degrades to single chain")
expect_out("[ccg] shape::pi(" "pretooluse noise branch degrades to single chain")

# n2 零命中分支：pi|no_such_symbol_zz9 的零命中分支不计入不披露，
# 仍是单有效分支的五行。
file(WRITE "${work}/hook-prtu-ml-miss.json"
     "{\"hook_event_name\":\"PreToolUse\",\"tool_name\":\"Grep\",\"tool_input\":{\"pattern\":\"pi|no_such_symbol_zz9\"}}")
execute_process(COMMAND "${exe}" hook pretooluse -o "${work}" --config rtml
    INPUT_FILE "${work}/hook-prtu-ml-miss.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "pretooluse zero-hit branch stays uncounted")
expect_out("[ccg] shape::pi(" "pretooluse zero-hit branch stays uncounted")

# n3 分支超限（9 分支）：整体静默。
file(WRITE "${work}/hook-prtu-ml-overflow.json"
     "{\"hook_event_name\":\"PreToolUse\",\"tool_name\":\"Grep\",\"tool_input\":{\"pattern\":\"pi|a|b|c|d|e|f|g|h\"}}")
execute_process(COMMAND "${exe}" hook pretooluse -o "${work}" --config rtml
    INPUT_FILE "${work}/hook-prtu-ml-overflow.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "pretooluse branch overflow stays silent")
if(NOT out STREQUAL "")
    message(FATAL_ERROR "e2e [pretooluse branch overflow stays silent]: expected zero output, saw:\n${out}")
endif()

# n4 正则开启：rad\w\w\w 全匹配唯一命中 radius 名（声明/定义合并后的
# 唯一符号；radius_ 字段名 7 字节不匹配）→ 五行。
file(WRITE "${work}/hook-prtu-ml-regex.json"
     "{\"hook_event_name\":\"PreToolUse\",\"tool_name\":\"Grep\",\"tool_input\":{\"pattern\":\"rad\\\\w\\\\w\\\\w\"}}")
execute_process(COMMAND ${CMAKE_COMMAND} -E env "CCG_PRETOOLUSE_REGEX=1"
    "${exe}" hook pretooluse -o "${work}" --config rtml
    INPUT_FILE "${work}/hook-prtu-ml-regex.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "pretooluse regex enabled injects")
expect_out("[ccg] shape::Circle::radius(" "pretooluse regex enabled injects")

# n4b 正则多名（2..5 名）：ar\w+ 命中 area（Shape 纯虚 + Circle 覆写两个
# 符号）与 area_all 两名 → 多词清单（names 口径）。
file(WRITE "${work}/hook-prtu-ml-regex2.json"
     "{\"hook_event_name\":\"PreToolUse\",\"tool_name\":\"Grep\",\"tool_input\":{\"pattern\":\"ar\\\\w+\"}}")
execute_process(COMMAND ${CMAKE_COMMAND} -E env "CCG_PRETOOLUSE_REGEX=1"
    "${exe}" hook pretooluse -o "${work}" --config rtml
    INPUT_FILE "${work}/hook-prtu-ml-regex2.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "pretooluse regex multi-name list injects")
expect_out("matched 3 symbols across 2 names" "pretooluse regex multi-name list injects")
expect_out("[ccg] - shape::Circle::area" "pretooluse regex multi-name list injects")
expect_out("[ccg] - app::area_all" "pretooluse regex multi-name list injects")

# n5 正则关（同载荷对照）：零输出。
execute_process(COMMAND ${CMAKE_COMMAND} -E env "CCG_PRETOOLUSE_REGEX=0"
    "${exe}" hook pretooluse -o "${work}" --config rtml
    INPUT_FILE "${work}/hook-prtu-ml-regex.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "pretooluse regex disabled stays silent")
if(NOT out STREQUAL "")
    message(FATAL_ERROR "e2e [pretooluse regex disabled stays silent]: expected zero output, saw:\n${out}")
endif()

# n6 垃圾值回落默认：内置默认已定版为启用，垃圾值与未设置同待遇——
# 走默认开启的正则路径，注入与 n4 同款五行。
execute_process(COMMAND ${CMAKE_COMMAND} -E env "CCG_PRETOOLUSE_REGEX=yes"
    "${exe}" hook pretooluse -o "${work}" --config rtml
    INPUT_FILE "${work}/hook-prtu-ml-regex.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "pretooluse regex garbage value falls back to enabled default")
expect_out("[ccg] shape::Circle::radius(" "pretooluse regex garbage value falls back to enabled default")

# n7 开关只闸正则路径：交替拆分不经开关——=0 下 pi|area 仍注入清单。
file(WRITE "${work}/hook-prtu-ml-alt0.json"
     "{\"hook_event_name\":\"PreToolUse\",\"tool_name\":\"Grep\",\"tool_input\":{\"pattern\":\"pi|area\"}}")
execute_process(COMMAND ${CMAKE_COMMAND} -E env "CCG_PRETOOLUSE_REGEX=0"
    "${exe}" hook pretooluse -o "${work}" --config rtml
    INPUT_FILE "${work}/hook-prtu-ml-alt0.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "pretooluse alternation ignores the regex switch")
expect_out("matched 3 symbols across 2 terms" "pretooluse alternation ignores the regex switch")

# n8 全名命中超限：\w+ 命中名 >5 → 计数披露行（names 口径），无清单条目。
file(WRITE "${work}/hook-prtu-ml-wide.json"
     "{\"hook_event_name\":\"PreToolUse\",\"tool_name\":\"Grep\",\"tool_input\":{\"pattern\":\"\\\\w+\"}}")
execute_process(COMMAND ${CMAKE_COMMAND} -E env "CCG_PRETOOLUSE_REGEX=1"
    "${exe}" hook pretooluse -o "${work}" --config rtml
    INPUT_FILE "${work}/hook-prtu-ml-wide.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "pretooluse regex overmatch discloses count")
expect_out("too many to list" "pretooluse regex overmatch discloses count")
string(FIND "${out}" "names" _ml_names_pos)
string(FIND "${out}" "[ccg] - " _ml_entry_pos)
if(_ml_names_pos EQUAL -1 OR NOT _ml_entry_pos EQUAL -1)
    message(FATAL_ERROR "e2e [pretooluse regex overmatch discloses count]: expected a names count line without list entries, saw:\n${out}")
endif()

# n9 fixed 字面含竖线：字面语义绝不拆交替。
file(WRITE "${work}/hook-prtu-ml-fixed.json"
     "{\"hook_event_name\":\"PreToolUse\",\"tool_name\":\"Bash\",\"tool_input\":{\"command\":\"rg -F 'pi|area'\"}}")
execute_process(COMMAND "${exe}" hook pretooluse -o "${work}" --config rtml
    INPUT_FILE "${work}/hook-prtu-ml-fixed.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "pretooluse fixed pipe literal stays silent")
if(NOT out STREQUAL "")
    message(FATAL_ERROR "e2e [pretooluse fixed pipe literal stays silent]: expected zero output, saw:\n${out}")
endif()

# n10 PowerShell 同款 fixed 字面（引擎探针可用才强断言，照 pwsh 先例）。
file(WRITE "${work}/hook-prtu-ml-psfixed.json"
     "{\"hook_event_name\":\"PreToolUse\",\"tool_name\":\"PowerShell\",\"tool_input\":{\"command\":\"Select-String -SimpleMatch 'pi|area'\"}}")
execute_process(COMMAND "${exe}" hook pretooluse -o "${work}" --config rtml
    INPUT_FILE "${work}/hook-prtu-ml-psfixed.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "pretooluse powershell fixed pipe stays silent")
if(NOT out STREQUAL "")
    message(FATAL_ERROR "e2e [pretooluse powershell fixed pipe stays silent]: expected zero output, saw:\n${out}")
endif()

# n11 真病态形态（嵌套量词，第三道机械护栏的靶）：黑盒下"护栏拒"与
# "零命中静默"同观感，用例价值=真病态载荷走全管线不崩不挂；形态鉴
# 别由单测承担。
file(WRITE "${work}/hook-prtu-ml-patho.json"
     "{\"hook_event_name\":\"PreToolUse\",\"tool_name\":\"Grep\",\"tool_input\":{\"pattern\":\"(a*)*\"}}")
execute_process(COMMAND ${CMAKE_COMMAND} -E env "CCG_PRETOOLUSE_REGEX=1"
    "${exe}" hook pretooluse -o "${work}" --config rtml
    INPUT_FILE "${work}/hook-prtu-ml-patho.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "pretooluse pathological regex stays silent")
if(NOT out STREQUAL "")
    message(FATAL_ERROR "e2e [pretooluse pathological regex stays silent]: expected zero output, saw:\n${out}")
endif()

# n12 长度护栏：65 字节正则零输出。
file(WRITE "${work}/hook-prtu-ml-long.json"
     "{\"hook_event_name\":\"PreToolUse\",\"tool_name\":\"Grep\",\"tool_input\":{\"pattern\":\"GGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGG\\\\w+\"}}")
execute_process(COMMAND ${CMAKE_COMMAND} -E env "CCG_PRETOOLUSE_REGEX=1"
    "${exe}" hook pretooluse -o "${work}" --config rtml
    INPUT_FILE "${work}/hook-prtu-ml-long.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "pretooluse over-long regex stays silent")
if(NOT out STREQUAL "")
    message(FATAL_ERROR "e2e [pretooluse over-long regex stays silent]: expected zero output, saw:\n${out}")
endif()

# n13 Search 臂正则（四臂覆盖矩阵补钉）：query 字段直读、single=true、
# fixed=false 的形态组合经 search_tool_queries + normalize_shell_lookup
# 抵达与 Grep 臂同一条受限正则路径——Search 侧端到端证据（与 n4 同款
# 五行断言）。
file(WRITE "${work}/hook-prtu-ml-regex-srch.json"
     "{\"hook_event_name\":\"PreToolUse\",\"tool_name\":\"Search\",\"tool_input\":{\"query\":\"rad\\\\w\\\\w\\\\w\"}}")
execute_process(COMMAND ${CMAKE_COMMAND} -E env "CCG_PRETOOLUSE_REGEX=1"
    "${exe}" hook pretooluse -o "${work}" --config rtml
    INPUT_FILE "${work}/hook-prtu-ml-regex-srch.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "pretooluse search regex injects")
expect_out("[ccg] shape::Circle::radius(" "pretooluse search regex injects")

# n14 Bash 臂 rg 正则（无 -F，旗标表提取后 fixed=false）：与 Grep/Search
# 同一受限正则路径——Bash 侧端到端证据；与 n9（fixed 含竖线静默）合成
# Bash 臂正则语义的双向钉子。
file(WRITE "${work}/hook-prtu-ml-regex-bash.json"
     "{\"hook_event_name\":\"PreToolUse\",\"tool_name\":\"Bash\",\"tool_input\":{\"command\":\"rg 'rad\\\\w\\\\w\\\\w'\"}}")
execute_process(COMMAND ${CMAKE_COMMAND} -E env "CCG_PRETOOLUSE_REGEX=1"
    "${exe}" hook pretooluse -o "${work}" --config rtml
    INPUT_FILE "${work}/hook-prtu-ml-regex-bash.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "pretooluse bash regex injects")
expect_out("[ccg] shape::Circle::radius(" "pretooluse bash regex injects")

# 超时路径：1 毫秒内部预算强制走分离分支。
# 更新子进程继续在后台跑（不会被杀）；
# hook 仍必须立即以文档规定的后台行作答，
# 影响面取自既有索引。
file(COPY "${fixture}/" DESTINATION "${work}/rtbg")
ccg_run(index "${work}/rtbg" -o "${work}" --config rtbg)
expect_rc(0 "index rtbg fixture")
ccg_run(stats --json -o "${work}" --config rtbg)
expect_rc(0 "index rtbg stats")
# indexedAt 只是辅助信号；绝不能替代
# 下面的真实结果。
string(REGEX MATCH "\"indexedAt\":\"[^\"]*\"" _bg_indexed_at "${out}")
file(APPEND "${work}/rtbg/circle.cpp" "\nint rt_bg_fn() { return 9; }\n")
file(WRITE "${work}/hook-bg.json"
     "{\"hook_event_name\":\"PostToolUse\",\"tool_name\":\"Write\",\"tool_input\":{\"file_path\":\"${work}/rtbg/circle.cpp\"}}")
execute_process(COMMAND ${CMAKE_COMMAND} -E env "CCG_HOOK_UPDATE_TIMEOUT_MS=1"
    "${exe}" hook posttooluse -o "${work}" --config rtbg
    INPUT_FILE "${work}/hook-bg.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "hook posttooluse timeout path")
# CMake 的 OUTPUT_VARIABLE 解码依赖控制台代码页，所以
# 断言用处理器的 ASCII stderr 确认（注入的
# 文本本身是中文，可能被整体弄乱）。
expect_err("refresh continues in the background" "hook posttooluse timeout path")
expect_out("hookSpecificOutput" "hook posttooluse timeout path")
expect_out("PostToolUse" "hook posttooluse timeout path")

# 分离的子进程必须真的完成刷新（而不只是被
# 拉起）：新符号必须可查询且队列必须
# 已排空（无待解析工作）。indexedAt 单独不被
# 接受为证据——它可能在符号未落库时前进。
set(_bg_refreshed FALSE)
foreach(_attempt RANGE 1 60)
    ccg_run(query rt_bg_fn -o "${work}" --config rtbg)
    if(rc EQUAL 0 AND out MATCHES "rt_bg_fn")
        ccg_run(stats --json -o "${work}" --config rtbg)
        expect_rc(0 "hook posttooluse timeout path stats")
        expect_out("\"pending\":0"
                   "hook posttooluse timeout path queue drained")
        set(_bg_refreshed TRUE)
        break()
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 1)
endforeach()
if(NOT _bg_refreshed)
    message(FATAL_ERROR
        "e2e [hook posttooluse timeout path]: the detached background update "
        "did not finish within 60s (rt_bg_fn never became queryable)")
endif()

# 绝对截止守卫：永不完成的子进程必须在等待预算处分离，
# 等待预算 = 总预算减去预留。停顿注入只睡
# 算出来的等待预算，所以报告的等待时长与
# 墙钟时间都证明预留被扣掉了。
# 此处的宿主超时对应物是 CCG_HOOK_TOTAL_BUDGET_MS（1200 毫秒）。
file(WRITE "${work}/hook-stall.json"
     "{\"hook_event_name\":\"PostToolUse\",\"tool_name\":\"Write\",\"tool_input\":{\"file_path\":\"${work}/rtbg/circle.cpp\"}}")
string(TIMESTAMP _t0 "%s%f")
execute_process(COMMAND ${CMAKE_COMMAND} -E env
    "CCG_HOOK_TEST_STALL_MS=60000"
    "CCG_HOOK_TOTAL_BUDGET_MS=1200"
    "CCG_HOOK_POST_RESERVE_MS=500"
    "${exe}" hook posttooluse -o "${work}" --config rtbg
    INPUT_FILE "${work}/hook-stall.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
string(TIMESTAMP _t1 "%s%f")
expect_rc(0 "hook posttooluse deadline path")
expect_err("refresh continues in the background"
           "hook posttooluse deadline path")
string(REGEX MATCH "after ([0-9]+) ms" _stall_match "${err}")
if(NOT _stall_match)
    message(FATAL_ERROR
        "e2e [hook posttooluse deadline path]: missing wait-budget "
        "diagnostic in stderr:\n${err}")
endif()
set(_wait_ms "${CMAKE_MATCH_1}")
math(EXPR _budget_ceiling "1200 - 500")
if(_wait_ms GREATER _budget_ceiling)
    message(FATAL_ERROR
        "e2e [hook posttooluse deadline path]: waited ${_wait_ms} ms, "
        "above total budget minus reserve (${_budget_ceiling} ms)")
endif()
math(EXPR _elapsed_ms "(${_t1} - ${_t0}) / 1000")
if(_elapsed_ms GREATER 1700)
    message(FATAL_ERROR
        "e2e [hook posttooluse deadline path]: hook took ${_elapsed_ms} ms, "
        "above the 1200 ms budget plus 500 ms startup allowance")
endif()

# ------------------------------------------------ hook 摘要积压 ------
# 批持有 SQLite 写锁期间，编辑必须走快路径：
# 文件入队、写一条日志、一秒内返回且 stdout 为零
#（不拉起更新、不算影响面）。一旦批
# 收敛，UserPromptSubmit hook 基于刷新后的库报告一条合并结论——
# 且绝不重复报告。
file(COPY "${fixture}/" DESTINATION "${work}/rtdg")
ccg_run(index "${work}/rtdg" -o "${work}" --config rtdg)
expect_rc(0 "digest index fixture")

set(_dg_dir "${work}/rtdg/hook-digest")
set(_dg_marker "${work}/digest-claim.marker")
set(_dg_ups "${work}/hook-ups.json")
file(WRITE "${_dg_ups}" "{\"hook_event_name\":\"UserPromptSubmit\",\"prompt\":\"go\"}")
file(REMOVE "${_dg_marker}")
file(REMOVE_RECURSE "${_dg_dir}")
file(WRITE "${work}/digest-hold.sh"
"#!/bin/sh
export CCG_TEST_HOLD_WRITE_LOCK_MS=6000
export CCG_TEST_CLAIM_MARKER=\"${_dg_marker}\"
\"${exe}\" update --drain -o \"${work}\" --config rtdg >/dev/null 2>&1
")
if(WIN32)
    # 经 PowerShell 的 Start-Process 分离：`cmd /c start /b` 会让
    # 后台批继承 CMake 的 stdout/stderr 管道，
    # execute_process 会阻塞到持有结束（而编辑运行时
    # claim 已释放）。Start-Process 不
    # 继承那些句柄。
    file(WRITE "${work}/digest-hold.cmd"
"@echo off
set CCG_TEST_HOLD_WRITE_LOCK_MS=6000
set CCG_TEST_CLAIM_MARKER=${_dg_marker}
\"${exe}\" update --drain -o \"${work}\" --config rtdg >nul 2>&1
")
    execute_process(COMMAND powershell -NoProfile -Command
        "Start-Process -FilePath 'cmd.exe' -ArgumentList '/c','${work}/digest-hold.cmd' -WindowStyle Hidden")
else()
    execute_process(COMMAND sh -c "\"${work}/digest-hold.sh\" >/dev/null 2>&1 &")
endif()
set(_dg_held FALSE)
foreach(_attempt RANGE 1 20)
    if(EXISTS "${_dg_marker}")
        set(_dg_held TRUE)
        break()
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 1)
endforeach()
if(NOT _dg_held)
    message(FATAL_ERROR "e2e [digest]: the held-write-lock update never claimed")
endif()

file(APPEND "${work}/rtdg/circle.cpp" "\nint dg_alpha() { return 1; }\n")
file(APPEND "${work}/rtdg/canvas.cpp" "\nint dg_beta() { return 2; }\n")
file(WRITE "${work}/hook-dg-a.json"
     "{\"hook_event_name\":\"PostToolUse\",\"tool_name\":\"Edit\",\"tool_input\":{\"file_path\":\"${work}/rtdg/circle.cpp\",\"old_string\":\"int dg_alpha_old() { return 1; }\",\"new_string\":\"int dg_alpha() { return 1; }\"}}")
file(WRITE "${work}/hook-dg-b.json"
     "{\"hook_event_name\":\"PostToolUse\",\"tool_name\":\"Edit\",\"tool_input\":{\"file_path\":\"${work}/rtdg/canvas.cpp\",\"old_string\":\"int dg_beta_old() { return 2; }\",\"new_string\":\"int dg_beta() { return 2; }\"}}")

string(TIMESTAMP _t0 "%s%f")
execute_process(COMMAND "${exe}" hook posttooluse -o "${work}" --config rtdg
    INPUT_FILE "${work}/hook-dg-a.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
string(TIMESTAMP _t1 "%s%f")
expect_rc(0 "digest fast path")
if(NOT out STREQUAL "")
    message(FATAL_ERROR "e2e [digest fast path]: expected zero stdout, saw:\n${out}")
endif()
math(EXPR _dg_elapsed "(${_t1} - ${_t0}) / 1000")
if(_dg_elapsed GREATER 1500)
    message(FATAL_ERROR
        "e2e [digest fast path]: hook took ${_dg_elapsed} ms (must stay well under a second plus startup allowance)")
endif()
# 持锁期间同一文件再编辑两次（同文件与第二个文件）。
execute_process(COMMAND "${exe}" hook posttooluse -o "${work}" --config rtdg
    INPUT_FILE "${work}/hook-dg-a.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
if(NOT out STREQUAL "")
    message(FATAL_ERROR "e2e [digest fast path]: second edit produced output:\n${out}")
endif()
execute_process(COMMAND "${exe}" hook posttooluse -o "${work}" --config rtdg
    INPUT_FILE "${work}/hook-dg-b.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
if(NOT out STREQUAL "")
    message(FATAL_ERROR "e2e [digest fast path]: third edit produced output:\n${out}")
endif()
file(GLOB _dg_pending "${_dg_dir}/pending-*.json")
list(LENGTH _dg_pending _dg_count)
if(NOT _dg_count EQUAL 3)
    message(FATAL_ERROR "e2e [digest fast path]: expected 3 journal files, saw ${_dg_count}")
endif()
list(GET _dg_pending 0 _dg_one)
file(READ "${_dg_one}" _dg_entry)
string(FIND "${_dg_entry}" "\"need_drain\":true" _dg_pos)
if(_dg_pos EQUAL -1)
    message(FATAL_ERROR
        "e2e [digest fast path]: the held write lock must force the need_drain bypass:\n${_dg_entry}")
endif()

# 需求 5：仍在跑的批让 flush 沉默而迅速。
string(TIMESTAMP _t0 "%s%f")
execute_process(COMMAND "${exe}" hook userpromptsubmit -o "${work}" --config rtdg
    INPUT_FILE "${_dg_ups}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
string(TIMESTAMP _t1 "%s%f")
expect_rc(0 "digest flush while running")
if(NOT out STREQUAL "")
    message(FATAL_ERROR "e2e [digest flush while running]: expected zero stdout, saw:\n${out}")
endif()
math(EXPR _dg_elapsed "(${_t1} - ${_t0}) / 1000")
if(_dg_elapsed GREATER 1500)
    message(FATAL_ERROR
        "e2e [digest flush while running]: took ${_dg_elapsed} ms; a live batch must return immediately")
endif()

# 同一活租约窗口、同一积压在场：PreToolUse 静默出口的捎带也必须
# 零输出、迅速返回（Read 载体 + 有积压 + 活租约 = 投递被放弃）。
file(WRITE "${work}/hook-prtu-flush-live.json"
     "{\"hook_event_name\":\"PreToolUse\",\"tool_name\":\"Read\",\"tool_input\":{\"file_path\":\"${work}/rtdg/circle.cpp\"}}")
string(TIMESTAMP _t0 "%s%f")
execute_process(COMMAND "${exe}" hook pretooluse -o "${work}" --config rtdg
    INPUT_FILE "${work}/hook-prtu-flush-live.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
string(TIMESTAMP _t1 "%s%f")
expect_rc(0 "pretooluse piggyback live lease stays silent")
if(NOT out STREQUAL "")
    message(FATAL_ERROR "e2e [pretooluse piggyback live lease stays silent]: expected zero stdout, saw:\n${out}")
endif()
math(EXPR _dg_elapsed "(${_t1} - ${_t0}) / 1000")
if(_dg_elapsed GREATER 1500)
    message(FATAL_ERROR
        "e2e [pretooluse piggyback live lease stays silent]: took ${_dg_elapsed} ms; a live batch must return immediately")
endif()
file(GLOB _dg_live_pending "${_dg_dir}/pending-*.json")
list(LENGTH _dg_live_pending _dg_live_count)
if(NOT _dg_live_count EQUAL 3)
    message(FATAL_ERROR "e2e [pretooluse piggyback live lease stays silent]: live lease must not claim the backlog (saw ${_dg_live_count} journal files)")
endif()

# 等持有者释放；第一次 flush 合并 need_drain 积压并
# 分离一次排空（仍然沉默）。轮询直到某次 flush 有报告。
execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 7)
set(_dg_reported FALSE)
set(_dg_report_out "")
foreach(_attempt RANGE 1 60)
    execute_process(COMMAND "${exe}" hook userpromptsubmit -o "${work}" --config rtdg
        INPUT_FILE "${_dg_ups}"
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
    if(out MATCHES "hookSpecificOutput")
        set(_dg_reported TRUE)
        set(_dg_report_out "${out}")
        break()
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 1)
endforeach()
if(NOT _dg_reported)
    message(FATAL_ERROR
        "e2e [digest report]: no merged conclusion after the batch converged (last stderr:\n${err})")
endif()
# 只用 ASCII 探针：CMake 的控制台解码在某些机器上会弄坏中文
# 载荷，而机器断言只关心信封。
foreach(_needle "\"hookSpecificOutput\"" "\"additionalContext\"" "UserPromptSubmit" "dg_alpha" "dg_beta" "circle.cpp" "canvas.cpp")
    string(FIND "${_dg_report_out}" "${_needle}" _dg_pos)
    if(_dg_pos EQUAL -1)
        message(FATAL_ERROR
            "e2e [digest report]: '${_needle}' missing from:\n${_dg_report_out}")
    endif()
endforeach()
string(REGEX MATCHALL "hookSpecificOutput" _dg_headers "${_dg_report_out}")
list(LENGTH _dg_headers _dg_header_count)
if(NOT _dg_header_count EQUAL 1)
    message(FATAL_ERROR
        "e2e [digest report]: expected exactly one merged envelope, saw ${_dg_header_count}")
endif()

# 需求 3：注入的积压已消失；第二次提示保持沉默。
execute_process(COMMAND "${exe}" hook userpromptsubmit -o "${work}" --config rtdg
    INPUT_FILE "${_dg_ups}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "digest no repeat")
if(NOT out STREQUAL "")
    message(FATAL_ERROR "e2e [digest no repeat]: a second prompt re-injected:\n${out}")
endif()
file(GLOB _dg_left "${_dg_dir}/pending-*.json" "${_dg_dir}/claimed-*")
list(LENGTH _dg_left _dg_left_count)
if(NOT _dg_left_count EQUAL 0)
    message(FATAL_ERROR
        "e2e [digest no repeat]: backlog residue after injection: ${_dg_left}")
endif()

# 两个库里都没有的不同文件必须各自保留自己的
# 结论。摘要级去重过去在显示名被赋值之前
# 运行，所以两者渲染成同一段空前缀"未索引"文本，
# 一条结论被静默丢弃。
file(WRITE "${_dg_dir}/pending-ghost-a.json"
     "{\"v\":1,\"path\":\"${work}/rtdg/ghost_one.cpp\",\"norm\":\"${work}/rtdg/ghost_one.cpp\",\"have_line\":true,\"line\":1,\"lines\":1,\"comment_only\":false,\"need_drain\":false,\"ts\":1}")
file(WRITE "${_dg_dir}/pending-ghost-b.json"
     "{\"v\":1,\"path\":\"${work}/rtdg/ghost_two.cpp\",\"norm\":\"${work}/rtdg/ghost_two.cpp\",\"have_line\":true,\"line\":1,\"lines\":1,\"comment_only\":false,\"need_drain\":false,\"ts\":2}")
execute_process(COMMAND "${exe}" hook userpromptsubmit -o "${work}" --config rtdg
    INPUT_FILE "${_dg_ups}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "digest distinct unindexed files")
if(NOT out MATCHES "hookSpecificOutput")
    message(FATAL_ERROR
        "e2e [digest distinct unindexed files]: no conclusion injected:\n${out}\n${err}")
endif()
string(REGEX MATCHALL "hookSpecificOutput" _dg_headers "${out}")
list(LENGTH _dg_headers _dg_header_count)
if(NOT _dg_header_count EQUAL 1)
    message(FATAL_ERROR
        "e2e [digest distinct unindexed files]: expected one merged envelope, saw ${_dg_header_count}:\n${out}")
endif()
foreach(_needle "ghost_one.cpp" "ghost_two.cpp")
    string(FIND "${out}" "${_needle}" _dg_pos)
    if(_dg_pos EQUAL -1)
        message(FATAL_ERROR
            "e2e [digest distinct unindexed files]: '${_needle}' missing (a file conclusion was deduped away):\n${out}")
    endif()
endforeach()

# CREATE_NEW 与第一次 WriteFile 之间崩溃会留下空锁
# 文件。flush 必须回收它（Windows：可读的空锁证明
# 创建者已死；POSIX：mtime 超过陈旧窗口）并仍报告
# 待处理积压，而不是永远沉默。
file(WRITE "${_dg_dir}/flush.lock" "")
file(WRITE "${_dg_dir}/pending-halflock.json"
     "{\"v\":1,\"path\":\"${work}/rtdg/ghost_half.cpp\",\"norm\":\"${work}/rtdg/ghost_half.cpp\",\"have_line\":true,\"line\":1,\"lines\":1,\"comment_only\":false,\"need_drain\":false,\"ts\":3}")
set(_dg_halflock_ready TRUE)
if(NOT WIN32)
    execute_process(COMMAND touch -d "5 minutes ago" "${_dg_dir}/flush.lock"
                    RESULT_VARIABLE _dg_touch)
    if(NOT _dg_touch EQUAL 0)
        set(_dg_halflock_ready FALSE)
    endif()
endif()
if(_dg_halflock_ready)
    execute_process(COMMAND "${exe}" hook userpromptsubmit -o "${work}" --config rtdg
        INPUT_FILE "${_dg_ups}"
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
    expect_rc(0 "digest half-written lock")
    string(FIND "${out}" "ghost_half.cpp" _dg_pos)
    if(_dg_pos EQUAL -1)
        message(FATAL_ERROR
            "e2e [digest half-written lock]: the empty flush.lock wedged the backlog ('ghost_half.cpp' missing):\n${out}\n${err}")
    endif()
    file(GLOB _dg_left "${_dg_dir}/pending-*.json")
    list(LENGTH _dg_left _dg_left_count)
    if(NOT _dg_left_count EQUAL 0)
        message(FATAL_ERROR
            "e2e [digest half-written lock]: backlog residue after injection: ${_dg_left}")
    endif()
else()
    message(STATUS
        "ccg_e2e: half-written lock e2e skipped (cannot backdate mtime)")
    file(REMOVE "${_dg_dir}/flush.lock" "${_dg_dir}/pending-halflock.json")
endif()

# ---------------------------------------- PreToolUse 捎带投递 ------
# 无人值守缺口：agent 做完末轮编辑转纯查询/读文件后，投递时机
# 全集（下一次 PostToolUse / UserPromptSubmit / SessionStart）一个
# 都不再触发——末批 digest 永不投递。PreToolUse 的静默出口捎带
# 同款 Opportunistic flush 补上这个缺口；Read/Glob 是纯投递载体
# （不查图）；命中注入的出口不捎带（1400B 信封不与 digest 混排）；
# 投递计数落 flushed-pretooluse-* 标记，ccg hook status --json 的
# digest.preToolUseFlushes 可见。活租约场景已在上面 digest 段的
# live-lease 探针覆盖。
file(COPY "${fixture}/" DESTINATION "${work}/rtpf")
ccg_run(index "${work}/rtpf" -o "${work}" --config rtpf)
expect_rc(0 "pretooluse piggyback index fixture")

set(_pf_dir "${work}/rtpf/hook-digest")
file(MAKE_DIRECTORY "${_pf_dir}")
file(WRITE "${_pf_dir}/pending-ptu-a.json"
     "{\"v\":1,\"path\":\"${work}/rtpf/ghost_flush_a.cpp\",\"norm\":\"${work}/rtpf/ghost_flush_a.cpp\",\"have_line\":true,\"line\":1,\"lines\":1,\"comment_only\":false,\"need_drain\":false,\"ts\":1}")
file(WRITE "${_pf_dir}/pending-ptu-b.json"
     "{\"v\":1,\"path\":\"${work}/rtpf/ghost_flush_b.cpp\",\"norm\":\"${work}/rtpf/ghost_flush_b.cpp\",\"have_line\":true,\"line\":1,\"lines\":1,\"comment_only\":false,\"need_drain\":false,\"ts\":2}")

# Read 载体：只投递不查图——一个信封、积压结论在场、
# 符号注入（夹具已索引的 shape::pi）绝不能出现。
file(WRITE "${work}/hook-pf-read.json"
     "{\"hook_event_name\":\"PreToolUse\",\"tool_name\":\"Read\",\"tool_input\":{\"file_path\":\"${work}/rtpf/circle.cpp\"}}")
execute_process(COMMAND "${exe}" hook pretooluse -o "${work}" --config rtpf
    INPUT_FILE "${work}/hook-pf-read.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "pretooluse piggyback read delivers")
expect_out("\"hookSpecificOutput\"" "pretooluse piggyback read delivers")
expect_out("\"PreToolUse\"" "pretooluse piggyback read delivers")
expect_out("ghost_flush_a.cpp" "pretooluse piggyback read delivers")
expect_out("ghost_flush_b.cpp" "pretooluse piggyback read delivers")
string(FIND "${out}" "shape::pi(" _pf_pos)
if(NOT _pf_pos EQUAL -1)
    message(FATAL_ERROR "e2e [pretooluse piggyback read delivers]: Read must not run the symbol query, saw injection:\n${out}")
endif()
string(REGEX MATCHALL "hookSpecificOutput" _pf_headers "${out}")
list(LENGTH _pf_headers _pf_header_count)
if(NOT _pf_header_count EQUAL 1)
    message(FATAL_ERROR "e2e [pretooluse piggyback read delivers]: expected exactly one envelope, saw ${_pf_header_count}:\n${out}")
endif()
# 投递后积压清空、标记落下。
file(GLOB _pf_left "${_pf_dir}/pending-*.json" "${_pf_dir}/claimed-*")
list(LENGTH _pf_left _pf_left_count)
if(NOT _pf_left_count EQUAL 0)
    message(FATAL_ERROR "e2e [pretooluse piggyback read delivers]: backlog residue after delivery: ${_pf_left}")
endif()
file(GLOB _pf_markers "${_pf_dir}/flushed-pretooluse-*")
list(LENGTH _pf_markers _pf_marker_count)
if(NOT _pf_marker_count EQUAL 1)
    message(FATAL_ERROR "e2e [pretooluse piggyback read delivers]: expected 1 delivery marker, saw ${_pf_marker_count}")
endif()

# 无积压时零成本：同一条 Read 事件再跑一次必须零输出。
execute_process(COMMAND "${exe}" hook pretooluse -o "${work}" --config rtpf
    INPUT_FILE "${work}/hook-pf-read.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "pretooluse piggyback no backlog stays silent")
if(NOT out STREQUAL "")
    message(FATAL_ERROR "e2e [pretooluse piggyback no backlog stays silent]: expected zero output, saw:\n${out}")
endif()

# 零命中的 Grep 也捎带（查询接口层的静默出口）。
file(WRITE "${_pf_dir}/pending-ptu-c.json"
     "{\"v\":1,\"path\":\"${work}/rtpf/ghost_flush_c.cpp\",\"norm\":\"${work}/rtpf/ghost_flush_c.cpp\",\"have_line\":true,\"line\":1,\"lines\":1,\"comment_only\":false,\"need_drain\":false,\"ts\":3}")
file(WRITE "${work}/hook-pf-miss.json"
     "{\"hook_event_name\":\"PreToolUse\",\"tool_name\":\"Grep\",\"tool_input\":{\"pattern\":\"no_such_symbol_zz9\"}}")
execute_process(COMMAND "${exe}" hook pretooluse -o "${work}" --config rtpf
    INPUT_FILE "${work}/hook-pf-miss.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "pretooluse piggyback grep miss delivers")
expect_out("ghost_flush_c.cpp" "pretooluse piggyback grep miss delivers")

# 非查找的 Bash 命令（预筛出口）与 Glob 载体同样投递。
file(WRITE "${_pf_dir}/pending-ptu-e.json"
     "{\"v\":1,\"path\":\"${work}/rtpf/ghost_flush_e.cpp\",\"norm\":\"${work}/rtpf/ghost_flush_e.cpp\",\"have_line\":true,\"line\":1,\"lines\":1,\"comment_only\":false,\"need_drain\":false,\"ts\":4}")
file(WRITE "${work}/hook-pf-bash.json"
     "{\"hook_event_name\":\"PreToolUse\",\"tool_name\":\"Bash\",\"tool_input\":{\"command\":\"ls\",\"description\":\"List files\"}}")
execute_process(COMMAND "${exe}" hook pretooluse -o "${work}" --config rtpf
    INPUT_FILE "${work}/hook-pf-bash.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "pretooluse piggyback bash non-search delivers")
expect_out("ghost_flush_e.cpp" "pretooluse piggyback bash non-search delivers")
file(WRITE "${_pf_dir}/pending-ptu-f.json"
     "{\"v\":1,\"path\":\"${work}/rtpf/ghost_flush_f.cpp\",\"norm\":\"${work}/rtpf/ghost_flush_f.cpp\",\"have_line\":true,\"line\":1,\"lines\":1,\"comment_only\":false,\"need_drain\":false,\"ts\":5}")
file(WRITE "${work}/hook-pf-glob.json"
     "{\"hook_event_name\":\"PreToolUse\",\"tool_name\":\"Glob\",\"tool_input\":{\"pattern\":\"src/**/*.cpp\"}}")
execute_process(COMMAND "${exe}" hook pretooluse -o "${work}" --config rtpf
    INPUT_FILE "${work}/hook-pf-glob.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "pretooluse piggyback glob delivers")
expect_out("ghost_flush_f.cpp" "pretooluse piggyback glob delivers")

# 命中注入的出口不捎带：注入五行在场、digest 的文件结论不在场、
# 积压原样保留（留给下一个静默出口）。
file(WRITE "${_pf_dir}/pending-ptu-d.json"
     "{\"v\":1,\"path\":\"${work}/rtpf/ghost_flush_d.cpp\",\"norm\":\"${work}/rtpf/ghost_flush_d.cpp\",\"have_line\":true,\"line\":1,\"lines\":1,\"comment_only\":false,\"need_drain\":false,\"ts\":6}")
file(WRITE "${work}/hook-pf-hit.json"
     "{\"hook_event_name\":\"PreToolUse\",\"tool_name\":\"Grep\",\"tool_input\":{\"pattern\":\"pi\"}}")
execute_process(COMMAND "${exe}" hook pretooluse -o "${work}" --config rtpf
    INPUT_FILE "${work}/hook-pf-hit.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "pretooluse hit does not piggyback")
expect_out("[ccg] shape::pi(" "pretooluse hit does not piggyback")
string(FIND "${out}" "ghost_flush_d.cpp" _pf_pos)
if(NOT _pf_pos EQUAL -1)
    message(FATAL_ERROR "e2e [pretooluse hit does not piggyback]: a symbol-injection hit must not carry the digest, saw:\n${out}")
endif()
file(GLOB _pf_pending_hit "${_pf_dir}/pending-*.json")
list(LENGTH _pf_pending_hit _pf_pending_hit_count)
if(NOT _pf_pending_hit_count EQUAL 1)
    message(FATAL_ERROR "e2e [pretooluse hit does not piggyback]: the backlog must survive a hit, saw ${_pf_pending_hit_count} journal files")
endif()

# 计数观测：status --json 的 digest 节点报 preToolUseFlushes
#（read=1、grep miss=1、bash=1、glob=1；命中注入不计）。
ccg_run(hook status --project -o "${work}" --config rtpf --json)
expect_rc(0 "pretooluse piggyback status count")
expect_out("\"preToolUseFlushes\":4" "pretooluse piggyback status count")

# ------------------------------------------------ 索引租约协调 ----
# 方案 D（索引提交阶段取
# 更新 claim）。四个钉子：(1) 排队在活持有者后面、接管后
# 两个写者的结果都落库；(2) 长批持有写锁期间
# hook 走 150 毫秒快路径（日志 need_drain，stdout 为零）——
# 这为两类持有者钉住长批行为：更新持有者
# 靠 pid 探测保持 claim 活着（hooks 避让并不需要
# >300 秒的 lease），索引提交阶段以同样方式持有
# claim；(3) 提交中途 claim 被抢则整个阶段干净回滚；
# (4) --wait 封顶排队、--no-wait 快速失败。

# --- (1) 排队接管 + 双方结果落库 + hook 快路径被挡在外面 ----------
file(COPY "${fixture}/" DESTINATION "${work}/lease")
ccg_run(index "${work}/lease" -o "${work}" --config lease)
expect_rc(0 "lease index fixture")

# 更新侧先落结果（钉子里的"另一个写者"）。
file(APPEND "${work}/lease/circle.cpp" "\nint dg_lease_a() { return 1; }\n")
ccg_run(update "${work}/lease/circle.cpp" -o "${work}" --config lease)
expect_rc(0 "lease update enqueue")
ccg_run(update --drain -o "${work}" --config lease)
expect_rc(0 "lease update converge")
ccg_run(query dg_lease_a -o "${work}" --config lease)
expect_rc(0 "lease query update result")
expect_out("dg_lease_a" "lease query update result")

# 一个同时持有 claim 与写锁 9 秒的持有者。
set(_lc_dir "${work}/lease/hook-digest")
set(_lc_marker "${work}/lease-holder.marker")
file(REMOVE "${_lc_marker}")
file(REMOVE_RECURSE "${_lc_dir}")
file(WRITE "${work}/lease-hold.sh"
"#!/bin/sh
export CCG_TEST_HOLD_WRITE_LOCK_MS=9000
export CCG_TEST_CLAIM_MARKER=\"${_lc_marker}\"
\"${exe}\" update --drain -o \"${work}\" --config lease >/dev/null 2>&1
")
if(WIN32)
    file(WRITE "${work}/lease-hold.cmd"
"@echo off
set CCG_TEST_HOLD_WRITE_LOCK_MS=9000
set CCG_TEST_CLAIM_MARKER=${_lc_marker}
\"${exe}\" update --drain -o \"${work}\" --config lease >nul 2>&1
")
    execute_process(COMMAND powershell -NoProfile -Command
        "Start-Process -FilePath 'cmd.exe' -ArgumentList '/c','${work}/lease-hold.cmd' -WindowStyle Hidden")
else()
    execute_process(COMMAND sh -c "\"${work}/lease-hold.sh\" >/dev/null 2>&1 &")
endif()
set(_lc_held FALSE)
foreach(_attempt RANGE 1 20)
    if(EXISTS "${_lc_marker}")
        set(_lc_held TRUE)
        break()
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 1)
endforeach()
if(NOT _lc_held)
    message(FATAL_ERROR "e2e [lease]: the held-write-lock update never claimed")
endif()

# 长批运行期间，编辑必须走 hook 快路径：活的
# claim 快照（pid 探测过）把它送去排队 + 记日志，亚秒返回、
# stdout 为零——编辑者绝不被分钟级长批阻塞。
file(WRITE "${work}/lease-hook.json"
     "{\"hook_event_name\":\"PostToolUse\",\"tool_name\":\"Edit\",\"tool_input\":{\"file_path\":\"${work}/lease/canvas.cpp\",\"old_string\":\"int dg_lease_old() { return 2; }\",\"new_string\":\"int dg_lease_b() { return 2; }\"}}")
string(TIMESTAMP _t0 "%s%f")
execute_process(COMMAND "${exe}" hook posttooluse -o "${work}" --config lease
    INPUT_FILE "${work}/lease-hook.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
string(TIMESTAMP _t1 "%s%f")
expect_rc(0 "lease hook fast path")
if(NOT out STREQUAL "")
    message(FATAL_ERROR "e2e [lease hook fast path]: expected zero stdout, saw:\n${out}")
endif()
math(EXPR _lc_hook_ms "(${_t1} - ${_t0}) / 1000")
if(_lc_hook_ms GREATER 1500)
    message(FATAL_ERROR
        "e2e [lease hook fast path]: hook took ${_lc_hook_ms} ms while the batch held the lock")
endif()
file(GLOB _lc_pending "${_lc_dir}/pending-*.json")
list(LENGTH _lc_pending _lc_journal)
if(NOT _lc_journal EQUAL 1)
    message(FATAL_ERROR "e2e [lease hook fast path]: expected 1 journal entry, saw ${_lc_journal}")
endif()
list(GET _lc_pending 0 _lc_one)
file(READ "${_lc_one}" _lc_entry)
string(FIND "${_lc_entry}" "\"need_drain\":true" _lc_pos)
if(_lc_pos EQUAL -1)
    message(FATAL_ERROR
        "e2e [lease hook fast path]: the held write lock must force the need_drain bypass:\n${_lc_entry}")
endif()

# 索引启动即接管（主动偷取）：持有者还握着写锁时，偷取事务在
# 探测退避循环里排队（stderr 有接管等待行），锁一放就偷到手；
# 两个写者的结果都落库（rc 0），钩子期间入队的 canvas.cpp 由
# 收尾清队就地消化（不再依赖后台 drain 的时序）。
file(APPEND "${work}/lease/canvas.cpp" "\nint dg_lease_b() { return 2; }\n")
ccg_run(index "${work}/lease" -o "${work}" --config lease)
expect_rc(0 "lease index takeover")
expect_err("waiting to take over the update claim" "lease index takeover")
# 注：钩子的入队被持有者的写锁挡成 journal（need_drain），队列在接管
# 时是空的——canvas.cpp 的重解析由主批的新鲜度扫描承担（下方查询
# 断言），收尾清队的"队列有事件"形态由 (7a) 的无锁持有者钉住。
ccg_run(query dg_lease_b -o "${work}" --config lease)
expect_rc(0 "lease query index result")
expect_out("dg_lease_b" "lease query index result")
ccg_run(query dg_lease_a -o "${work}" --config lease)
expect_rc(0 "lease query both writers")
expect_out("dg_lease_a" "lease query both writers")

# --- (2) 索引提交阶段持锁：hook 再次走快路径 ------------------------
file(COPY "${fixture}/" DESTINATION "${work}/lease2")
ccg_run(index "${work}/lease2" -o "${work}" --config lease2)
expect_rc(0 "lease2 index fixture")
set(_l2_dir "${work}/lease2/hook-digest")
set(_l2_marker "${work}/lease2-claim.marker")
file(REMOVE "${_l2_marker}")
file(REMOVE_RECURSE "${_l2_dir}")
# 后台全量重索引，其提交阶段在写事务内部睡 6 秒
#（CCG_TEST_INDEX_HOLD_COMMIT_MS），心跳节奏 1 秒。
file(WRITE "${work}/lease2-hold.sh"
"#!/bin/sh
export CCG_TEST_INDEX_CLAIM_MARKER=\"${_l2_marker}\"
export CCG_TEST_INDEX_HOLD_COMMIT_MS=6000
export CCG_TEST_COMMIT_HEARTBEAT_MS=1000
\"${exe}\" index \"${work}/lease2\" -o \"${work}\" --config lease2 --full >/dev/null 2>&1
")
if(WIN32)
    file(WRITE "${work}/lease2-hold.cmd"
"@echo off
set CCG_TEST_INDEX_CLAIM_MARKER=${_l2_marker}
set CCG_TEST_INDEX_HOLD_COMMIT_MS=6000
set CCG_TEST_COMMIT_HEARTBEAT_MS=1000
\"${exe}\" index \"${work}/lease2\" -o \"${work}\" --config lease2 --full >nul 2>&1
")
    execute_process(COMMAND powershell -NoProfile -Command
        "Start-Process -FilePath 'cmd.exe' -ArgumentList '/c','${work}/lease2-hold.cmd' -WindowStyle Hidden")
else()
    execute_process(COMMAND sh -c "\"${work}/lease2-hold.sh\" >/dev/null 2>&1 &")
endif()
set(_l2_held FALSE)
foreach(_attempt RANGE 1 30)
    if(EXISTS "${_l2_marker}")
        set(_l2_held TRUE)
        break()
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 1)
endforeach()
if(NOT _l2_held)
    message(FATAL_ERROR "e2e [lease2]: the background index never claimed")
endif()
file(WRITE "${work}/lease2-hook.json"
     "{\"hook_event_name\":\"PostToolUse\",\"tool_name\":\"Edit\",\"tool_input\":{\"file_path\":\"${work}/lease2/circle.cpp\",\"old_string\":\"int dg_lease2_old() { return 1; }\",\"new_string\":\"int dg_lease2() { return 1; }\"}}")
string(TIMESTAMP _t0 "%s%f")
execute_process(COMMAND "${exe}" hook posttooluse -o "${work}" --config lease2
    INPUT_FILE "${work}/lease2-hook.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
string(TIMESTAMP _t1 "%s%f")
expect_rc(0 "lease2 hook fast path")
if(NOT out STREQUAL "")
    message(FATAL_ERROR "e2e [lease2 hook fast path]: expected zero stdout, saw:\n${out}")
endif()
math(EXPR _l2_hook_ms "(${_t1} - ${_t0}) / 1000")
if(_l2_hook_ms GREATER 1500)
    message(FATAL_ERROR
        "e2e [lease2 hook fast path]: hook took ${_l2_hook_ms} ms while the index commit stage held the lock")
endif()
file(GLOB _l2_pending "${_l2_dir}/pending-*.json")
list(LENGTH _l2_pending _l2_journal)
if(NOT _l2_journal EQUAL 1)
    message(FATAL_ERROR "e2e [lease2 hook fast path]: expected 1 journal entry, saw ${_l2_journal}")
endif()
# 等后台全量跑完，随后的运行必须是干净的。
execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 12)
ccg_run(index "${work}/lease2" -o "${work}" --config lease2)
expect_rc(0 "lease2 follow-up index")

# --- (3) 提交中途被抢：整个阶段回滚，什么都不落库 -------------------
file(COPY "${fixture}/" DESTINATION "${work}/lease3")
ccg_run(index "${work}/lease3" -o "${work}" --config lease3)
expect_rc(0 "lease3 index fixture")
ccg_run(stats -o "${work}" --config lease3)
expect_rc(0 "lease3 stats before")
string(REGEX MATCH "symbols[ \t]+35" _l3_hit "${out}")
if(NOT _l3_hit)
    message(FATAL_ERROR "e2e [lease3 stats before]: unexpected baseline:\n${out}")
endif()
# 注入在提交事务内部把 claim 行改写成假抢夺者；
# 节流的事务内心跳必须看到零行、抛出并
# 回滚整个阶段（基线行数不变）。
if(WIN32)
    file(WRITE "${work}/lease3-steal.cmd"
"@echo off
set CCG_TEST_INDEX_HOLD_COMMIT_MS=4000
set CCG_TEST_COMMIT_HEARTBEAT_MS=500
set CCG_TEST_INDEX_STEAL_CLAIM=1
\"${exe}\" index \"${work}/lease3\" -o \"${work}\" --config lease3 --full
")
    execute_process(COMMAND cmd /c "${work}/lease3-steal.cmd"
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
else()
    file(WRITE "${work}/lease3-steal.sh"
"#!/bin/sh
export CCG_TEST_INDEX_HOLD_COMMIT_MS=4000
export CCG_TEST_COMMIT_HEARTBEAT_MS=500
export CCG_TEST_INDEX_STEAL_CLAIM=1
exec \"${exe}\" index \"${work}/lease3\" -o \"${work}\" --config lease3 --full
")
    execute_process(COMMAND sh "${work}/lease3-steal.sh"
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
endif()
if(rc EQUAL 0)
    message(FATAL_ERROR
        "e2e [lease3 steal]: expected a failed exit, got 0 (the theft went unnoticed):\n${out}\n${err}")
endif()
string(FIND "${err}" "claim stolen mid-commit" _l3_pos)
if(_l3_pos EQUAL -1)
    message(FATAL_ERROR "e2e [lease3 steal]: missing claim-stolen diagnostic:\n${err}")
endif()
ccg_run(stats -o "${work}" --config lease3)
expect_rc(0 "lease3 stats after")
string(REGEX MATCH "symbols[ \t]+35" _l3_hit "${out}")
if(NOT _l3_hit)
    message(FATAL_ERROR
        "e2e [lease3 steal]: the rolled-back stage changed the database:\n${out}")
endif()

# --- (4) --wait 封顶排队；--no-wait 快速失败 ---------------------------
file(COPY "${fixture}/" DESTINATION "${work}/lease4")
ccg_run(index "${work}/lease4" -o "${work}" --config lease4)
expect_rc(0 "lease4 index fixture")
set(_l4_marker "${work}/lease4-holder.marker")
file(REMOVE "${_l4_marker}")
file(WRITE "${work}/lease4-hold.sh"
"#!/bin/sh
export CCG_TEST_HOLD_WRITE_LOCK_MS=8000
export CCG_TEST_CLAIM_MARKER=\"${_l4_marker}\"
\"${exe}\" update --drain -o \"${work}\" --config lease4 >/dev/null 2>&1
")
if(WIN32)
    file(WRITE "${work}/lease4-hold.cmd"
"@echo off
set CCG_TEST_HOLD_WRITE_LOCK_MS=8000
set CCG_TEST_CLAIM_MARKER=${_l4_marker}
\"${exe}\" update --drain -o \"${work}\" --config lease4 >nul 2>&1
")
    execute_process(COMMAND powershell -NoProfile -Command
        "Start-Process -FilePath 'cmd.exe' -ArgumentList '/c','${work}/lease4-hold.cmd' -WindowStyle Hidden")
else()
    execute_process(COMMAND sh -c "\"${work}/lease4-hold.sh\" >/dev/null 2>&1 &")
endif()
set(_l4_held FALSE)
foreach(_attempt RANGE 1 20)
    if(EXISTS "${_l4_marker}")
        set(_l4_held TRUE)
        break()
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 1)
endforeach()
if(NOT _l4_held)
    message(FATAL_ERROR "e2e [lease4]: the held-write-lock update never claimed")
endif()
# --wait=500：排队循环到上限即放弃，并点名持有者。
ccg_run(index "${work}/lease4" -o "${work}" --config lease4 --full --wait=500)
if(rc EQUAL 0)
    message(FATAL_ERROR "e2e [lease4 wait cap]: expected failure at the cap, got 0")
endif()
string(FIND "${err}" "--wait limit" _l4_pos)
if(_l4_pos EQUAL -1)
    message(FATAL_ERROR "e2e [lease4 wait cap]: missing cap diagnostic:\n${err}")
endif()
# --no-wait：带着可操作的错误信息快速失败。
ccg_run(index "${work}/lease4" -o "${work}" --config lease4 --full --no-wait)
if(rc EQUAL 0)
    message(FATAL_ERROR "e2e [lease4 no-wait]: expected failure, got 0")
endif()
string(FIND "${err}" "live update session" _l4_pos)
if(_l4_pos EQUAL -1)
    message(FATAL_ERROR "e2e [lease4 no-wait]: missing fail-fast diagnostic:\n${err}")
endif()
# 持有者释放后，普通运行恢复正常（默认排队）。
execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 9)
ccg_run(index "${work}/lease4" -o "${work}" --config lease4 --full)
expect_rc(0 "lease4 recovery index")

# --- (5) 解析期快照的行被并发删除：复活兜底，绝不外键失败 ----------
# 受控复现"删-插穿插"：提交开锁前删掉 circle.cpp 的 files 行（等价
# 于并发清扫进程在解析期提交的短删除事务），解析期快照原样持旧 id。
# 旧行为=symbols 的 file_id 外键失败整批回滚（exit 1）；修复后提交段
# 的陈旧快照核对照样复活该行，符号重建、行数回到基线。
file(COPY "${fixture}/" DESTINATION "${work}/lease5")
ccg_run(index "${work}/lease5" -o "${work}" --config lease5)
expect_rc(0 "lease5 index fixture")
if(WIN32)
    file(WRITE "${work}/lease5-race.cmd"
"@echo off
set CCG_TEST_INDEX_DELETE_BEFORE_COMMIT=${work}/lease5/circle.cpp
\"${exe}\" index \"${work}/lease5\" -o \"${work}\" --config lease5 --full
")
    execute_process(COMMAND cmd /c "${work}/lease5-race.cmd"
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
else()
    file(WRITE "${work}/lease5-race.sh"
"#!/bin/sh
export CCG_TEST_INDEX_DELETE_BEFORE_COMMIT=\"${work}/lease5/circle.cpp\"
exec \"${exe}\" index \"${work}/lease5\" -o \"${work}\" --config lease5 --full
")
    execute_process(COMMAND sh "${work}/lease5-race.sh"
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
endif()
expect_rc(0 "lease5 stale-snapshot resurrection (no FK abort)")
string(FIND "${err}" "FOREIGN KEY" _l5_fk)
if(NOT _l5_fk EQUAL -1)
    message(FATAL_ERROR "e2e [lease5]: a foreign key abort leaked through:\n${err}")
endif()
string(FIND "${err}" "vanished while parsing" _l5_note)
if(_l5_note EQUAL -1)
    message(FATAL_ERROR "e2e [lease5]: missing the resurrection note:\n${err}")
endif()
ccg_run(stats -o "${work}" --config lease5)
expect_rc(0 "lease5 stats after resurrection")
string(REGEX MATCH "symbols[ \t]+35" _l5_hit "${out}")
if(NOT _l5_hit)
    message(FATAL_ERROR "e2e [lease5]: symbol count drifted after resurrection:\n${out}")
endif()

# --- (6) 增量持有者批内被偷：优雅让位 + 队列保留 --------------------
# update 的引擎批提交段睡 6 秒，期间声明行被注入改写为伪造偷取者
#（死 pid 记号）；事务内心跳（1 秒节奏）匹配零行 → ClaimStolenError
# → run_update 捕获优雅停轮：rc 0、批已干净回滚、队列行保留（下一次
# update --drain 能收敛它——队列没丢）。
file(COPY "${fixture}/" DESTINATION "${work}/lease6")
ccg_run(index "${work}/lease6" -o "${work}" --config lease6)
expect_rc(0 "lease6 index fixture")
file(APPEND "${work}/lease6/circle.cpp" "\nint dg_yield_a() { return 1; }\n")
if(WIN32)
    file(WRITE "${work}/lease6-yield.cmd"
"@echo off
set CCG_TEST_INDEX_HOLD_COMMIT_MS=6000
set CCG_TEST_COMMIT_HEARTBEAT_MS=1000
set CCG_TEST_INDEX_STEAL_CLAIM=1
\"${exe}\" update \"${work}/lease6/circle.cpp\" -o \"${work}\" --config lease6
")
    execute_process(COMMAND cmd /c "${work}/lease6-yield.cmd"
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
else()
    file(WRITE "${work}/lease6-yield.sh"
"#!/bin/sh
export CCG_TEST_INDEX_HOLD_COMMIT_MS=6000
export CCG_TEST_COMMIT_HEARTBEAT_MS=1000
export CCG_TEST_INDEX_STEAL_CLAIM=1
exec \"${exe}\" update \"${work}/lease6/circle.cpp\" -o \"${work}\" --config lease6
")
    execute_process(COMMAND sh "${work}/lease6-yield.sh"
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
endif()
expect_rc(0 "lease6 graceful yield exit")
string(FIND "${err}" "claim stolen mid-batch" _l6_pos)
if(_l6_pos EQUAL -1)
    message(FATAL_ERROR "e2e [lease6]: missing the graceful yield message:\n${err}")
endif()
# 回滚不丢审计：引擎批已干净回滚（stderr 的让位消息为证），rollback
# 协调事件在回滚**之后**的独立短事务里落库——detach 子进程 stderr 蒸发
# 的现场，库内证据仍然幸存。
ccg_run(hook status --project -o "${work}" --config lease6 --json)
expect_rc(0 "lease6 rollback audit readable")
expect_out("rollback" "lease6 rollback audit readable")
# 伪造偷取者是死 pid 记号：pid 探活判死，下一个更新者直接偷回并收敛
# 保留下来的队列行。（注：CCG_TEST_INDEX_STEAL_CLAIM 的改写在引擎批
# 事务内、随 ROLLBACK 一同消失——drain 进来是空声明直接落笔，本场景
# 不产生接管；orphan-recover 的 pid-dead/lease-expired 两分支由
# db.coord_events_audit 单元覆盖。）
ccg_run(update --drain -o "${work}" --config lease6)
expect_rc(0 "lease6 converge after yield")
ccg_run(query dg_yield_a -o "${work}" --config lease6)
expect_rc(0 "lease6 query converged symbol")
expect_out("dg_yield_a" "lease6 query converged symbol")

# --- (7) 收尾清队：三段式输出 / 追轮收敛 / 超限诚实退出 ------------
# (7a) 活租约期间（无在飞批）第二个 update 只入队；index 启动接管后
# 把队列就地消化：接管注记 + 检测提示 + 全量文件列表 + 收尾结果行。
file(COPY "${fixture}/" DESTINATION "${work}/lease7")
ccg_run(index "${work}/lease7" -o "${work}" --config lease7)
expect_rc(0 "lease7 index fixture")
file(APPEND "${work}/lease7/canvas.cpp" "\nint dg_drain_a() { return 1; }\n")
set(_l7_marker "${work}/lease7-holder.marker")
file(REMOVE "${_l7_marker}")
file(WRITE "${work}/lease7-hold.sh"
"#!/bin/sh
export CCG_TEST_CLAIM_HOLD_MS=15000
export CCG_TEST_CLAIM_MARKER=\"${_l7_marker}\"
\"${exe}\" update --drain -o \"${work}\" --config lease7 >/dev/null 2>&1
")
if(WIN32)
    file(WRITE "${work}/lease7-hold.cmd"
"@echo off
set CCG_TEST_CLAIM_HOLD_MS=15000
set CCG_TEST_CLAIM_MARKER=${_l7_marker}
\"${exe}\" update --drain -o \"${work}\" --config lease7 >nul 2>&1
")
    execute_process(COMMAND powershell -NoProfile -Command
        "Start-Process -FilePath 'cmd.exe' -ArgumentList '/c','${work}/lease7-hold.cmd' -WindowStyle Hidden")
else()
    execute_process(COMMAND sh -c "\"${work}/lease7-hold.sh\" >/dev/null 2>&1 &")
endif()
set(_l7_held FALSE)
foreach(_attempt RANGE 1 20)
    if(EXISTS "${_l7_marker}")
        set(_l7_held TRUE)
        break()
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 1)
endforeach()
if(NOT _l7_held)
    message(FATAL_ERROR "e2e [lease7]: the claim-only holder never claimed")
endif()
# 单写者不变性：活租约在场，第二个 update 只入队不处理（rc 0 + 排队
# 注记），绝不与持有者并发写库。
ccg_run(update "${work}/lease7/canvas.cpp" -o "${work}" --config lease7)
expect_rc(0 "lease7 second writer queues only")
expect_err("queued" "lease7 second writer queues only")
# index 启动接管（持有者只是睡着， displaced 的是活声明）。
ccg_run(index "${work}/lease7" -o "${work}" --config lease7)
expect_rc(0 "lease7 index takeover")
expect_err("took over the update lease" "lease7 index takeover")
expect_err("changed while indexing" "lease7 drain notice")
expect_err("canvas.cpp" "lease7 drain file list")
expect_err("drain finished: 1 round" "lease7 drain result line")
# 顶替活声明的接管留 steal 审计行（与偷取同事务提交）。放在 index 的
# stderr 断言之后：ccg_run 会覆写 out/err，先取用先断言。
ccg_run(hook status --project -o "${work}" --config lease7 --json)
expect_rc(0 "lease7 steal audited")
expect_out("steal" "lease7 steal audited")
expect_out("lastRun" "lease7 steal audited")
expect_out("run-finish" "lease7 steal audited")
ccg_run(query dg_drain_a -o "${work}" --config lease7)
expect_rc(0 "lease7 query drained symbol")
expect_out("dg_drain_a" "lease7 query drained symbol")
# 队列已空（"drain finished: 1 round" 钉住）；--drain 此时会顺带跑
# 延迟派发的收敛批（追轮批 defer_dispatch 的既有语义），输出是收敛
# 摘要而非 "nothing to do"，故只断言 rc 0。
ccg_run(update --drain -o "${work}" --config lease7)
expect_rc(0 "lease7 post-drain convergence run")

# (7b) 超限诚实退出：轮数上限压到 0，入队事件原样留给常规机制（后台
# --drain 收敛），收尾结果行如实报剩余。
file(COPY "${fixture}/" DESTINATION "${work}/lease7b")
ccg_run(index "${work}/lease7b" -o "${work}" --config lease7b)
expect_rc(0 "lease7b index fixture")
file(APPEND "${work}/lease7b/canvas.cpp" "\nint dg_drain_b() { return 2; }\n")
set(_l7b_marker "${work}/lease7b-holder.marker")
file(REMOVE "${_l7b_marker}")
file(WRITE "${work}/lease7b-hold.sh"
"#!/bin/sh
export CCG_TEST_CLAIM_HOLD_MS=15000
export CCG_TEST_CLAIM_MARKER=\"${_l7b_marker}\"
\"${exe}\" update --drain -o \"${work}\" --config lease7b >/dev/null 2>&1
")
if(WIN32)
    file(WRITE "${work}/lease7b-hold.cmd"
"@echo off
set CCG_TEST_CLAIM_HOLD_MS=15000
set CCG_TEST_CLAIM_MARKER=${_l7b_marker}
\"${exe}\" update --drain -o \"${work}\" --config lease7b >nul 2>&1
")
    execute_process(COMMAND powershell -NoProfile -Command
        "Start-Process -FilePath 'cmd.exe' -ArgumentList '/c','${work}/lease7b-hold.cmd' -WindowStyle Hidden")
else()
    execute_process(COMMAND sh -c "\"${work}/lease7b-hold.sh\" >/dev/null 2>&1 &")
endif()
set(_l7b_held FALSE)
foreach(_attempt RANGE 1 20)
    if(EXISTS "${_l7b_marker}")
        set(_l7b_held TRUE)
        break()
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 1)
endforeach()
if(NOT _l7b_held)
    message(FATAL_ERROR "e2e [lease7b]: the claim-only holder never claimed")
endif()
ccg_run(update "${work}/lease7b/canvas.cpp" -o "${work}" --config lease7b)
expect_rc(0 "lease7b second writer queues only")
if(WIN32)
    file(WRITE "${work}/lease7b-cap.cmd"
"@echo off
set CCG_TEST_INDEX_DRAIN_ROUNDS=0
\"${exe}\" index \"${work}/lease7b\" -o \"${work}\" --config lease7b
")
    execute_process(COMMAND cmd /c "${work}/lease7b-cap.cmd"
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
else()
    file(WRITE "${work}/lease7b-cap.sh"
"#!/bin/sh
export CCG_TEST_INDEX_DRAIN_ROUNDS=0
exec \"${exe}\" index \"${work}/lease7b\" -o \"${work}\" --config lease7b
")
    execute_process(COMMAND sh "${work}/lease7b-cap.sh"
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
endif()
expect_rc(0 "lease7b capped drain exit")
string(FIND "${err}" "still queued" _l7b_pos)
if(_l7b_pos EQUAL -1)
    message(FATAL_ERROR "e2e [lease7b]: missing the honest leftover line:\n${err}")
endif()
# 常规机制兜底：detach 的 --drain 后台收敛，轮询到符号出现为止。
set(_l7b_done FALSE)
foreach(_attempt RANGE 1 40)
    ccg_run(query dg_drain_b -o "${work}" --config lease7b)
    if(out MATCHES "dg_drain_b")
        set(_l7b_done TRUE)
        break()
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 1)
endforeach()
if(NOT _l7b_done)
    message(FATAL_ERROR "e2e [lease7b]: the background drain never converged the leftover (last: ${out})")
endif()

# (7c) 连续编辑追轮：主提交与每个追轮批的提交段都睡 6 秒，追轮 1 的
# 批运行期间再次编辑同一文件（内容哈希与批前快照不同）→ 队列行保留
# → 追轮 2 重处理 → 收尾行报 2 轮。
file(COPY "${fixture}/" DESTINATION "${work}/lease7c")
ccg_run(index "${work}/lease7c" -o "${work}" --config lease7c)
expect_rc(0 "lease7c index fixture")
file(APPEND "${work}/lease7c/canvas.cpp" "\nint dg_drain_c1() { return 3; }\n")
set(_l7c_marker "${work}/lease7c-commit.marker")
file(REMOVE "${_l7c_marker}")
file(WRITE "${work}/lease7c-hold.sh"
"#!/bin/sh
export CCG_TEST_CLAIM_HOLD_MS=15000
export CCG_TEST_CLAIM_MARKER=\"${_l7c_marker}\"
\"${exe}\" update --drain -o \"${work}\" --config lease7c >/dev/null 2>&1
")
if(WIN32)
    file(WRITE "${work}/lease7c-hold.cmd"
"@echo off
set CCG_TEST_CLAIM_HOLD_MS=15000
set CCG_TEST_CLAIM_MARKER=${_l7c_marker}
\"${exe}\" update --drain -o \"${work}\" --config lease7c >nul 2>&1
")
    execute_process(COMMAND powershell -NoProfile -Command
        "Start-Process -FilePath 'cmd.exe' -ArgumentList '/c','${work}/lease7c-hold.cmd' -WindowStyle Hidden")
else()
    execute_process(COMMAND sh -c "\"${work}/lease7c-hold.sh\" >/dev/null 2>&1 &")
endif()
set(_l7c_held FALSE)
foreach(_attempt RANGE 1 20)
    if(EXISTS "${_l7c_marker}")
        set(_l7c_held TRUE)
        break()
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 1)
endforeach()
if(NOT _l7c_held)
    message(FATAL_ERROR "e2e [lease7c]: the claim-only holder never claimed")
endif()
ccg_run(update "${work}/lease7c/canvas.cpp" -o "${work}" --config lease7c)
expect_rc(0 "lease7c second writer queues only")
file(WRITE "${work}/lease7c-index.err" "")
file(WRITE "${work}/lease7c-index.out" "")
if(WIN32)
    file(WRITE "${work}/lease7c-run.cmd"
"@echo off
set CCG_TEST_INDEX_HOLD_COMMIT_MS=6000
set CCG_TEST_COMMIT_HEARTBEAT_MS=1000
set CCG_TEST_INDEX_CLAIM_MARKER=${_l7c_marker}
\"${exe}\" index \"${work}/lease7c\" -o \"${work}\" --config lease7c >\"${work}/lease7c-index.out\" 2>\"${work}/lease7c-index.err\"
")
    execute_process(COMMAND powershell -NoProfile -Command
        "Start-Process -FilePath 'cmd.exe' -ArgumentList '/c','${work}/lease7c-run.cmd' -WindowStyle Hidden")
else()
    file(WRITE "${work}/lease7c-run.sh"
"#!/bin/sh
export CCG_TEST_INDEX_HOLD_COMMIT_MS=6000
export CCG_TEST_COMMIT_HEARTBEAT_MS=1000
export CCG_TEST_INDEX_CLAIM_MARKER=\"${_l7c_marker}\"
\"${exe}\" index \"${work}/lease7c\" -o \"${work}\" --config lease7c >\"${work}/lease7c-index.out\" 2>\"${work}/lease7c-index.err\" &
")
    execute_process(COMMAND sh -c "\"${work}/lease7c-run.sh\" >/dev/null 2>&1 &")
endif()
# 等 index 的主提交段开始（marker 被 index 重写为记号行）。
set(_l7c_commit FALSE)
foreach(_attempt RANGE 1 60)
    file(READ "${_l7c_marker}" _l7c_mk)
    if(_l7c_mk MATCHES "ccg-update:")
        set(_l7c_commit TRUE)
        break()
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 1)
endforeach()
if(NOT _l7c_commit)
    message(FATAL_ERROR "e2e [lease7c]: the background index never reached its commit")
endif()
file(TIMESTAMP "${_l7c_marker}" _l7c_mt1 "%s")
# 等追轮 1 的提交段开始（同一 marker 再次重写，mtime 变新）。
set(_l7c_round1 FALSE)
foreach(_attempt RANGE 1 60)
    file(TIMESTAMP "${_l7c_marker}" _l7c_mt2 "%s")
    if(NOT "${_l7c_mt2}" STREQUAL "${_l7c_mt1}")
        set(_l7c_round1 TRUE)
        break()
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 1)
endforeach()
if(NOT _l7c_round1)
    file(READ "${work}/lease7c-index.err" _l7c_err)
    message(FATAL_ERROR "e2e [lease7c]: the first drain round never committed:\n${_l7c_err}")
endif()
# 追轮 1 批内（提交段睡 6 秒的中段）再次编辑同一文件：批前快照哈希
# 与批后当前哈希不同 → clear_covered 保留队列行 → 追轮 2 重处理。
execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 2)
file(APPEND "${work}/lease7c/canvas.cpp" "\nint dg_drain_c2() { return 4; }\n")
set(_l7c_done FALSE)
foreach(_attempt RANGE 1 90)
    file(READ "${work}/lease7c-index.err" _l7c_err)
    if(_l7c_err MATCHES "index drain finished: 2 round")
        set(_l7c_done TRUE)
        break()
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 1)
endforeach()
if(NOT _l7c_done)
    message(FATAL_ERROR "e2e [lease7c]: expected a 2-round drain result line within 90 seconds:\n${_l7c_err}")
endif()
ccg_run(query dg_drain_c1 -o "${work}" --config lease7c)
expect_rc(0 "lease7c query first-edit symbol")
expect_out("dg_drain_c1" "lease7c query first-edit symbol")
ccg_run(query dg_drain_c2 -o "${work}" --config lease7c)
expect_rc(0 "lease7c query second-edit symbol")
expect_out("dg_drain_c2" "lease7c query second-edit symbol")
ccg_run(update --drain -o "${work}" --config lease7c)
expect_rc(0 "lease7c post-drain convergence run")

# --- (8) 解析期租约心跳（真库终验 F-1 的修复）------------------------
# 解析段远超租约时长（短租约 3 秒 + 解析拉长 15 秒）的现场：
# (8a) 伴随心跳线程把声明持续续租——钩子看到活声明只入队不派生，
#      index 收尾清队就地消化，rc 0 + json 的 parseLeaseBeats 非零；
# (8b) 禁用心跳的对照臂里租约如期过期，钩子按"过期可偷"派生新写者
#      反偷，index 按 ClaimStolenError 语义干净让位（rc 1、零悬空）。
file(COPY "${fixture}/" DESTINATION "${work}/lease8")
ccg_run(index "${work}/lease8" -o "${work}" --config lease8)
expect_rc(0 "lease8 index fixture")
ccg_run(config set update-lease-ms 3000 -o "${work}" --config lease8)
expect_rc(0 "lease8 short lease knob")
set(_l8_marker "${work}/lease8-parse.marker")
file(REMOVE "${_l8_marker}")
file(WRITE "${work}/lease8-run.sh"
"#!/bin/sh
export CCG_TEST_TAKEOVER_LEASE_MS=3000
export CCG_TEST_PARSE_HOLD_MS=15000
export CCG_TEST_PARSE_CLAIM_MARKER=\"${_l8_marker}\"
\"${exe}\" index \"${work}/lease8\" -o \"${work}\" --config lease8 --full --json >\"${work}/lease8.out\" 2>\"${work}/lease8.err\"
echo $? >\"${work}/lease8.rc\"
")
if(WIN32)
    file(WRITE "${work}/lease8-run.cmd"
"@echo off
set CCG_TEST_TAKEOVER_LEASE_MS=3000
set CCG_TEST_PARSE_HOLD_MS=15000
set CCG_TEST_PARSE_CLAIM_MARKER=${_l8_marker}
\"${exe}\" index \"${work}/lease8\" -o \"${work}\" --config lease8 --full --json >\"${work}/lease8.out\" 2>\"${work}/lease8.err\"
echo %ERRORLEVEL% >\"${work}/lease8.rc\"
")
    execute_process(COMMAND powershell -NoProfile -Command
        "Start-Process -FilePath 'cmd.exe' -ArgumentList '/c','${work}/lease8-run.cmd' -WindowStyle Hidden")
else()
    execute_process(COMMAND sh -c "\"${work}/lease8-run.sh\" >/dev/null 2>&1 &")
endif()
set(_l8_parse FALSE)
foreach(_attempt RANGE 1 30)
    if(EXISTS "${_l8_marker}")
        set(_l8_parse TRUE)
        break()
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 1)
endforeach()
if(NOT _l8_parse)
    message(FATAL_ERROR "e2e [lease8]: the background index never reached its parse phase")
endif()
# 解析中段（租约已被心跳续过数轮）编辑文件：活声明把钩子挡在入队
# 路径（stdout 零、绝不派生第二个写者）。
execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 2)
file(APPEND "${work}/lease8/circle.cpp" "\nint dg_drain8a() { return 1; }\n")
file(WRITE "${work}/lease8-hook.json"
     "{\"hook_event_name\":\"PostToolUse\",\"tool_name\":\"Edit\",\"tool_input\":{\"file_path\":\"${work}/lease8/circle.cpp\",\"old_string\":\"int dg_lease8_old() { return 1; }\",\"new_string\":\"int dg_drain8a() { return 1; }\"}}")
execute_process(COMMAND "${exe}" hook posttooluse -o "${work}" --config lease8
    INPUT_FILE "${work}/lease8-hook.json"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
expect_rc(0 "lease8 hook enqueues only")
if(NOT out STREQUAL "")
    message(FATAL_ERROR "e2e [lease8 hook]: expected zero stdout, saw:\n${out}")
endif()
set(_l8_done FALSE)
foreach(_attempt RANGE 1 90)
    if(EXISTS "${work}/lease8.rc")
        set(_l8_done TRUE)
        break()
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 1)
endforeach()
if(NOT _l8_done)
    file(READ "${work}/lease8.err" _l8_err)
    message(FATAL_ERROR "e2e [lease8]: the background index never finished:\n${_l8_err}")
endif()
file(READ "${work}/lease8.rc" _l8_rc)
string(STRIP "${_l8_rc}" _l8_rc)
if(NOT _l8_rc STREQUAL "0")
    file(READ "${work}/lease8.err" _l8_err)
    message(FATAL_ERROR "e2e [lease8]: expected rc 0 with the parse keepalive, got ${_l8_rc}:\n${_l8_err}")
endif()
file(READ "${work}/lease8.err" _l8_err)
if(NOT _l8_err MATCHES "index drain finished: 1 round")
    message(FATAL_ERROR "e2e [lease8]: missing the drain result line:\n${_l8_err}")
endif()
file(READ "${work}/lease8.out" _l8_out)
if(NOT _l8_out MATCHES "parseLeaseBeats")
    message(FATAL_ERROR "e2e [lease8]: expected a nonzero parseLeaseBeats in the json payload:\n${_l8_out}")
endif()
ccg_run(query dg_drain8a -o "${work}" --config lease8)
expect_rc(0 "lease8 query symbol")
expect_out("dg_drain8a" "lease8 query symbol")

# (8b) 对照臂：禁用解析期心跳（CCG_TEST_PARSE_KEEPALIVE=0），同款短
# 租约 + 长解析。租约过期后一个反偷写者（第二个 index——真实现场里
# 钩子按"过期可偷"派生的更新者与它对原持有者等价：声明易主）在解析
# 中段接管，长解析的那个按 ClaimStolenError 语义干净让位。
file(COPY "${fixture}/" DESTINATION "${work}/lease8b")
ccg_run(index "${work}/lease8b" -o "${work}" --config lease8b)
expect_rc(0 "lease8b index fixture")
ccg_run(config set update-lease-ms 3000 -o "${work}" --config lease8b)
expect_rc(0 "lease8b short lease knob")
set(_l8b_marker "${work}/lease8b-parse.marker")
file(REMOVE "${_l8b_marker}")
file(WRITE "${work}/lease8b-run.sh"
"#!/bin/sh
export CCG_TEST_TAKEOVER_LEASE_MS=3000
export CCG_TEST_PARSE_HOLD_MS=30000
export CCG_TEST_PARSE_KEEPALIVE=0
export CCG_TEST_PARSE_CLAIM_MARKER=\"${_l8b_marker}\"
\"${exe}\" index \"${work}/lease8b\" -o \"${work}\" --config lease8b --full >\"${work}/lease8b.out\" 2>\"${work}/lease8b.err\"
echo $? >\"${work}/lease8b.rc\"
")
if(WIN32)
    file(WRITE "${work}/lease8b-run.cmd"
"@echo off
set CCG_TEST_TAKEOVER_LEASE_MS=3000
set CCG_TEST_PARSE_HOLD_MS=30000
set CCG_TEST_PARSE_KEEPALIVE=0
set CCG_TEST_PARSE_CLAIM_MARKER=${_l8b_marker}
\"${exe}\" index \"${work}/lease8b\" -o \"${work}\" --config lease8b --full >\"${work}/lease8b.out\" 2>\"${work}/lease8b.err\"
echo %ERRORLEVEL% >\"${work}/lease8b.rc\"
")
    execute_process(COMMAND powershell -NoProfile -Command
        "Start-Process -FilePath 'cmd.exe' -ArgumentList '/c','${work}/lease8b-run.cmd' -WindowStyle Hidden")
else()
    execute_process(COMMAND sh -c "\"${work}/lease8b-run.sh\" >/dev/null 2>&1 &")
endif()
set(_l8b_parse FALSE)
foreach(_attempt RANGE 1 30)
    if(EXISTS "${_l8b_marker}")
        set(_l8b_parse TRUE)
        break()
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 1)
endforeach()
if(NOT _l8b_parse)
    message(FATAL_ERROR "e2e [lease8b]: the background index never reached its parse phase")
endif()
# 等租约（3 秒）过期，再让第二个写者进场反偷（快跑无注入，秒级完
# 成接管与释放）——真库终验 F-1 的原发病根形态：解析期无人续租，
# 声明被判死易主。
execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 5)
file(APPEND "${work}/lease8b/circle.cpp" "\nint dg_drain8b() { return 2; }\n")
ccg_run_in("${work}" index "${work}/lease8b" -o "${work}" --config lease8b --full)
expect_rc(0 "lease8b second writer takes over")
set(_l8b_done FALSE)
foreach(_attempt RANGE 1 90)
    if(EXISTS "${work}/lease8b.rc")
        set(_l8b_done TRUE)
        break()
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 1)
endforeach()
if(NOT _l8b_done)
    file(READ "${work}/lease8b.err" _l8b_err)
    message(FATAL_ERROR "e2e [lease8b]: the background index never finished:\n${_l8b_err}")
endif()
file(READ "${work}/lease8b.rc" _l8b_rc)
string(STRIP "${_l8b_rc}" _l8b_rc)
if(NOT _l8b_rc STREQUAL "1")
    file(READ "${work}/lease8b.err" _l8b_err)
    message(FATAL_ERROR "e2e [lease8b]: expected rc 1 (claim stolen without the keepalive), got ${_l8b_rc}:\n${_l8b_err}")
endif()
file(READ "${work}/lease8b.err" _l8b_err)
if(NOT _l8b_err MATCHES "claim stolen")
    message(FATAL_ERROR "e2e [lease8b]: missing the honest claim-stolen line:\n${_l8b_err}")
endif()
# 让位后库必须完好（零悬空）：stats 直接可查，第二写者的成果在场。
ccg_run(stats -o "${work}" --config lease8b)
expect_rc(0 "lease8b stats after yielding")
ccg_run(query dg_drain8b -o "${work}" --config lease8b)
expect_rc(0 "lease8b query second-writer symbol")
expect_out("dg_drain8b" "lease8b query second-writer symbol")

# 用法错误。
ccg_run_in("${hook_proj}" hook bogus)
expect_rc(2 "hook bogus action")
ccg_run_in("${hook_proj}" hook install)
expect_rc(2 "hook install needs a scope")

message(STATUS "ccg_e2e: all checks passed")
