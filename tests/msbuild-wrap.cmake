# 包装模式闸门：驱动真实 ccg.exe
# 充当 MSBuild CL 包装器，对着桩"真实编译器"（tests/clstub.py），
# 桩倾倒其宽字符 argv 并以 7 退出。
#
# 一个文件的两副面孔：
#   从 tests/CMakeLists.txt 注册 -> ccg_msbuild_wrap ctest。
#   经 `cmake -P msbuild-wrap.cmake <ccg.exe> <clstub.py>
#   <source-root> <work-dir>` 运行。
#
# 捕获存储：包装器把 SQLite 行写进由 CCG_CLWRAP_LOG
# 值推导的 capture.db（两侧同一真值函数）；
# 纯 ${work}/log.jsonl 形态回退到 ${work}/capture.db。
# 下方所有行断言读该库（python sqlite3——闸门
# 特批的读取器）。
#
# 闸门行：
#   快乐路径：退出码 7 传播；捕获表恰持一行
#       调用行，带 rsp 展开后的 argv（@rsp 递归
#       展开、带引号 define 单令牌、unicode 按位精确）、
#       工作 cwd 与转发的 real_cl；桩收到的
#       展开后 argv 相同。
#   缺 CCG_CLWRAP_REAL_CL -> 一行错误行、退出码 1。
#   读不了的响应文件 -> 一行错误行、退出码 1。
#   未设环境 -> ccg --version 不变（缺席时零影响）。
#   桩模式记录同一调用行（real_cl 空、INCLUDE
#       被捕获）、写桩产物、退出 0、绝不拉起编译器。
#   CCG_CLWRAP_CL 前缀映射：被选 cl 记进 real_cl。

if(NOT DEFINED CMAKE_ARGV6)
    message(FATAL_ERROR
        "msbuild-wrap.cmake: expected <ccg.exe> <clstub.py> <source-root> <work-dir>")
endif()
set(exe     "${CMAKE_ARGV3}")
set(stub    "${CMAKE_ARGV4}")
set(root    "${CMAKE_ARGV5}")
set(work    "${CMAKE_ARGV6}")

file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}")

# 测试向量：顶层 rsp 引一个带空格 define、嵌套
# 第二个 rsp，外加命令行上一个 unicode 位置令牌。
file(WRITE "${work}/inner.rsp" [[/DINNER=1]])
file(WRITE "${work}/test.rsp"
    [[/c /DSPACE=1 "/DQUOTED=a b" @inner.rsp src\main.cpp
]])

# 每轮次之间的清理：capture 表是追加式（INSERT），门禁断言"恰一行"
# 必须先移走上一轮的 db（连同 WAL 边车）。
set(capture_db "${work}/capture.db")
macro(clear_capture)
    file(REMOVE "${capture_db}" "${capture_db}-wal" "${capture_db}-shm")
endmacro()

# 运行/断言宏组（fail 等）收纳在同目录 ccg-harness.cmake，
# 供各闸门脚本共享。
include("${CMAKE_CURRENT_LIST_DIR}/ccg-harness.cmake")

# ------------------------------------------- 快乐路径（转发 + 捕获行） -------
execute_process(COMMAND "${CMAKE_COMMAND}" -E env
    "CCG_CLWRAP_LOG=${work}/log.jsonl"
    "CCG_CLWRAP_REAL_CL=${stub}"
    "CCG_CLSTUB_OUT=${work}/stub.json"
    -- "${exe}" /nologo @test.rsp ünïcode-Ω.cpp
    WORKING_DIRECTORY "${work}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
if(NOT rc EQUAL 7)
    fail("forward" "exit code ${rc}, wanted 7 (stub exit must propagate)\n--stdout--\n${out}\n--stderr--\n${err}")
endif()
if(NOT EXISTS "${capture_db}")
    fail("forward" "wrap capture db was not created")
endif()
if(NOT EXISTS "${work}/stub.json")
    fail("forward" "stub did not run (CCG_CLSTUB_OUT missing)")
endif()
# 单条调用行、精确展开 argv、cwd、real_cl；桩倾倒
# 证明转发的 argv 与记录的逐令牌相等。
execute_process(COMMAND python -I -c [[
import json, sqlite3, sys
db, stub_path, want_cwd, want_cl = sys.argv[1:5]
con = sqlite3.connect(db)
rows = con.execute(
    "SELECT kind, argv_json, cwd, include_env, real_cl, message "
    "FROM capture ORDER BY id").fetchall()
con.close()
assert len(rows) == 1, "expected one capture row, got %d" % len(rows)
kind, argv_json, cwd, inc, real_cl, msg = rows[0]
assert kind == "invocation", (kind, msg)
argv = json.loads(argv_json)
assert isinstance(argv, list) and argv, argv_json
assert all(isinstance(x, str) for x in argv), argv_json
tail = ["/nologo", "/c", "/DSPACE=1", "/DQUOTED=a b", "/DINNER=1",
        "src\\main.cpp", "ünïcode-Ω.cpp"]
assert argv[1:] == tail, argv
assert cwd.replace("\\", "/") == want_cwd.replace("\\", "/"), (cwd, want_cwd)
assert real_cl == want_cl, (real_cl, want_cl)
stub = [json.loads(x) for x in open(stub_path, encoding="utf-8") if x.strip()]
assert len(stub) == 1, stub
assert stub[0]["argv"][1:] == tail, stub[0]["argv"]
print("wrap-capture-ok")
]] "${capture_db}" "${work}/stub.json" "${work}" "${stub}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
if(NOT rc EQUAL 0)
    fail("capture" "python check failed (${rc}):\n${out}\n${err}")
endif()

# ------------------------------------------------ 缺 CCG_CLWRAP_REAL_CL -------
clear_capture()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env
    "CCG_CLWRAP_LOG=${work}/log.jsonl"
    -- "${exe}" /c missing.cpp
    WORKING_DIRECTORY "${work}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
if(NOT rc EQUAL 1)
    fail("no-real-cl" "exit code ${rc}, wanted 1\n--stdout--\n${out}\n--stderr--\n${err}")
endif()
execute_process(COMMAND python -I -c [[
import sqlite3, sys
con = sqlite3.connect(sys.argv[1])
rows = con.execute(
    "SELECT kind, message FROM capture ORDER BY id").fetchall()
con.close()
errors = [r for r in rows if r[0] == "error"]
assert len(errors) == 1, rows
assert "CCG_CLWRAP_REAL_CL is not set" in errors[0][1], errors[0][1]
print("wrap-realcl-error-ok")
]] "${capture_db}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
if(NOT rc EQUAL 0)
    fail("no-real-cl" "python check failed (${rc}):\n${out}\n${err}")
endif()

# ------------------------------------------------------ 响应文件读失败 -------
clear_capture()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env
    "CCG_CLWRAP_LOG=${work}/log.jsonl"
    "CCG_CLWRAP_REAL_CL=${stub}"
    -- "${exe}" /c @does-not-exist.rsp
    WORKING_DIRECTORY "${work}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
if(NOT rc EQUAL 1)
    fail("bad-rsp" "exit code ${rc}, wanted 1\n--stdout--\n${out}\n--stderr--\n${err}")
endif()
execute_process(COMMAND python -I -c [[
import sqlite3, sys
con = sqlite3.connect(sys.argv[1])
rows = con.execute(
    "SELECT kind, message FROM capture ORDER BY id").fetchall()
con.close()
assert len(rows) == 1 and rows[0][0] == "error", rows
assert "response-file expansion failed" in rows[0][1], rows[0][1]
print("wrap-badrsp-error-ok")
]] "${capture_db}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
if(NOT rc EQUAL 0)
    fail("bad-rsp" "python check failed (${rc}):\n${out}\n${err}")
endif()

# ------------------------------------------- 未设环境（缺席时零影响） -------
# 未设环境：hook 是一次探测、CLI 不动。
execute_process(COMMAND "${exe}" --version
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
if(NOT rc EQUAL 0)
    fail("version" "ccg --version exited ${rc}:\n${out}\n${err}")
endif()
if(NOT out MATCHES "^ccg [0-9]+\\.[0-9]+\\.[0-9]+")
    fail("version" "unexpected version line: ${out}")
endif()

# --------------------------------------------- 桩模式（零磁盘空操作） -------
# 记录模式（CCG_CLWRAP_STUB=1）：包装器记录同一调用
# 行（real_cl 空——什么都没设、INCLUDE 逐字捕获）、
# 什么都不写（零磁盘空操作契约：桩产物无消费者
# 且会砸掉硬编码路径上的真实 PDB/obj）、退出 0，
# 且绝不拉起任何真实编译器（连 REAL_CL 都不需要）。
clear_capture()
file(MAKE_DIRECTORY "${work}/intdir")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env
    "CCG_CLWRAP_LOG=${work}/log.jsonl"
    "CCG_CLWRAP_STUB=1"
    "INCLUDE=VS/VC/Tools/MSVC/14.99.99999/include;Kits/include"
    -- "${exe}" /nologo /c /Fo"${work}/intdir/" /Fd"${work}/intdir/" src/main.cpp
    WORKING_DIRECTORY "${work}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
if(NOT rc EQUAL 0)
    fail("stub-mode" "exit code ${rc}, wanted 0 (zero-disk stub, never compiles)
--stdout--
${out}
--stderr--
${err}")
endif()
# 零磁盘契约：声明的输出目录下任何地方都不得
# 出现桩产物（旧的 /Fo 按源 .obj 与 /Fd vc<ver>.pdb
# 推导已移除；那些路径上的既有文件保持不动）。
file(GLOB _stub_left "${work}/intdir/*")
if(NOT _stub_left STREQUAL "")
    fail("stub-mode" "intdir must stay empty under the zero-disk contract:
${_stub_left}")
endif()
execute_process(COMMAND python -I -c [[
import sqlite3, sys
con = sqlite3.connect(sys.argv[1])
rows = con.execute(
    "SELECT kind, include_env, real_cl, message FROM capture ORDER BY id"
).fetchall()
con.close()
assert len(rows) == 1 and rows[0][0] == "invocation", rows
assert rows[0][1] == "VS/VC/Tools/MSVC/14.99.99999/include;Kits/include", rows[0][1]
assert rows[0][2] == "", rows[0][2]
print("wrap-stub-mode-ok")
]] "${capture_db}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
if(NOT rc EQUAL 0)
    fail("stub-mode" "python check failed (${rc}):\n${out}\n${err}")
endif()

# ------------------------------------------------- cl 前缀映射（命中/落空） -------
# CCG_CLWRAP_CL 前缀映射：cwd 选择每工程 cl（最长
# 前缀）、落空回退 CCG_CLWRAP_REAL_CL。捕获行
# 把被选 cl 记进其 real_cl 字段，闸门由此证明
# 包装器选了哪个 cl（两个替身是同一份 clstub 脚本的
# 两个副本——记录的 real_cl 就是判别子）。
file(REMOVE "${work}/stub2.json")
file(COPY_FILE "${stub}" "${work}/clstub2.py")
# 命中：映射恰好为本 cwd 点名 clstub2。
clear_capture()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env
    "CCG_CLWRAP_LOG=${work}/log.jsonl"
    "CCG_CLWRAP_REAL_CL=${stub}"
    "CCG_CLSTUB_OUT=${work}/stub2.json"
    "CCG_CLWRAP_CL=${work}	${work}/clstub2.py"
    -- "${exe}" /nologo /c src/main.cpp
    WORKING_DIRECTORY "${work}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
if(NOT rc EQUAL 7)
    fail("cl-map-hit" "exit code ${rc}, wanted 7
--stdout--
${out}
--stderr--
${err}")
endif()
if(NOT EXISTS "${work}/stub2.json")
    fail("cl-map-hit" "no compiler ran at all (marker file absent)")
endif()
execute_process(COMMAND python -I -c [[
import sqlite3, sys
con = sqlite3.connect(sys.argv[1])
rows = con.execute(
    "SELECT kind, real_cl FROM capture ORDER BY id").fetchall()
con.close()
assert len(rows) == 1 and rows[0][0] == "invocation", rows
assert rows[0][1].replace("\\", "/").endswith("/clstub2.py"), rows[0][1]
print("wrap-clmap-hit-ok")
]] "${capture_db}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
if(NOT rc EQUAL 0)
    fail("cl-map-hit" "python check failed (${rc}):\n${out}\n${err}")
endif()
# 落空：映射点名另一棵树；REAL_CL 胜出且行如是记录。
file(REMOVE "${work}/stub2.json")
clear_capture()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env
    "CCG_CLWRAP_LOG=${work}/log.jsonl"
    "CCG_CLWRAP_REAL_CL=${stub}"
    "CCG_CLSTUB_OUT=${work}/stub2.json"
    "CCG_CLWRAP_CL=definitely/not/this/tree	${work}/clstub2.py"
    -- "${exe}" /nologo /c src/main.cpp
    WORKING_DIRECTORY "${work}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
if(NOT rc EQUAL 7)
    fail("cl-map-miss" "exit code ${rc}, wanted 7
--stdout--
${out}
--stderr--
${err}")
endif()
if(NOT EXISTS "${work}/stub2.json")
    fail("cl-map-miss" "the fallback REAL_CL did not run")
endif()
execute_process(COMMAND python -I -c [[
import sqlite3, sys
con = sqlite3.connect(sys.argv[1])
rows = con.execute(
    "SELECT kind, real_cl FROM capture ORDER BY id").fetchall()
con.close()
assert len(rows) == 1 and rows[0][0] == "invocation", rows
assert not rows[0][1].replace("\\", "/").endswith("/clstub2.py"), rows[0][1]
print("wrap-clmap-miss-ok")
]] "${capture_db}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
if(NOT rc EQUAL 0)
    fail("cl-map-miss" "python check failed (${rc}):\n${out}\n${err}")
endif()

message(STATUS "ccg_msbuild_wrap: all checks passed")
