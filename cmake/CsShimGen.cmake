# C# 对象宿主垫片程序集的构建期编译（cshost：Select-String 引擎面与
# MSBuild 求值面共宿主，对象协议 ObjCreate/ObjCall/ObjDestroy）。
#
# 产物：${GEN_DIR}/ccg-cshost51.dll 与 ${GEN_DIR}/ccg-cshost7.dll——
# 运行期唯一装载路线是可执行文件同目录直载（cg.util.cshost 的
# bundled_shim）：发布面（cmake/Install.cmake）从这里随包分发，
# 开发树由 src/CMakeLists.txt 的 POST_BUILD 拷贝摆到可执行文件旁。
# 工具缺失/编译失败的线产物缺席，运行期按路线不可用降级（缺席
# 诊断进 last_detail 链），构建永不因此变红。
#
# 两线产物面（MsbuildEval 仅 51 线——VS 自带 Microsoft.Build.dll 是
# net472 程序集，CoreCLR 装不下 .NET Framework 家族）：
#   51 线：编译 CshostTable.cs + SelectStringCore.cs + MsbuildEval.cs +
#          Cshost51Entry.cs；/reference 在 SMA 之外追加
#          System.Web.Extensions.dll（GAC 简单名引用——求值协议 JSON 的
#          JavaScriptSerializer 所在程序集，net48 在席）；
#   7 线：build Cshost7.csproj（net10 目标），编译 CshostTable.cs +
#         SelectStringCore.cs + Cshost7Entry.cs，不引用 MsbuildEval.cs。
#
# 用法（cmake -P）：
#   cmake -P cmake/CsShimGen.cmake
#        -D SRC_DIR=<cshost-shim 源码目录>
#        -D GEN_DIR=<生成目录>
#        -D CSC_EXE=<csc 路径或空>
#        -D SMA_REF=<GAC SMA 引用路径或空>
#        -D DOTNET_EXE=<dotnet 路径或空>
#        -D SMA7_HINT=<ccg-cshost7 编译引用的 SMA 路径或空>
#
# 注：本脚本自身不写源树（dotnet 的 obj/bin 经属性重定向进 GEN_DIR）。

if(NOT DEFINED SRC_DIR OR NOT DEFINED GEN_DIR)
    message(FATAL_ERROR "CsShimGen: SRC_DIR/GEN_DIR required")
endif()

file(MAKE_DIRECTORY "${GEN_DIR}")

# ---- ccg-cshost51.dll：.NET Framework 自带 csc 编译（C# 5 语法源） ----
# 经响应文件（@rsp）传参且路径一律反斜杠原生形态：实测该 csc 对
# 正斜杠盘符路径（<盘符>:/...）在 rsp 与命令行两种形态下都截断成
# cwd+basename（CS1504），反斜杠路径是它几十年的惯用形态。
# System.Web.Extensions.dll 按 GAC 简单名引用（csc 自带目录与 GAC 均有
# 该程序集；不落机器绝对路径，可移植性闸门零负担）。
set(_shim51 "${GEN_DIR}/ccg-cshost51.dll")
if(CSC_EXE AND SMA_REF)
    foreach(_p SMA_REF SRC_DIR GEN_DIR)
        file(TO_NATIVE_PATH "${${_p}}" _n_${_p})
    endforeach()
    file(WRITE "${GEN_DIR}/shim51.rsp"
" /nologo
 /target:library
 /optimize+
 /warn:0
 /codepage:65001
 /reference:\"${_n_SMA_REF}\"
 /reference:System.Web.Extensions.dll
 /out:\"${_n_GEN_DIR}\\ccg-cshost51.dll\"
 \"${_n_SRC_DIR}\\CshostTable.cs\"
 \"${_n_SRC_DIR}\\SelectStringCore.cs\"
 \"${_n_SRC_DIR}\\MsbuildEval.cs\"
 \"${_n_SRC_DIR}\\Cshost51Entry.cs\"
")
    execute_process(
        COMMAND "${CSC_EXE}" @${GEN_DIR}/shim51.rsp
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err)
    if(NOT (_rc EQUAL 0 AND EXISTS "${_shim51}"))
        # 编译失败：删除半成品，让运行期按不可用降级。
        file(REMOVE "${_shim51}")
        message(STATUS "ccg: ccg-cshost51 csc failed rc=${_rc} ${_out} ${_err}")
    endif()
endif()

# ---- ccg-cshost7.dll：dotnet SDK 构建（net10.0 目标，同一份核心源码） ----
set(_shim7 "${GEN_DIR}/ccg-cshost7.dll")
if(DOTNET_EXE AND SMA7_HINT)
    file(REMOVE_RECURSE "${GEN_DIR}/shim7-obj" "${GEN_DIR}/shim7-bin")
    execute_process(
        COMMAND "${DOTNET_EXE}" build ${SRC_DIR}/Cshost7.csproj
                -c Release --nologo -v quiet
                -p:BaseIntermediateOutputPath=${GEN_DIR}/shim7-obj/
                -p:BaseOutputPath=${GEN_DIR}/shim7-bin/
                -p:CCG_SMA_HINT=${SMA7_HINT}
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err)
    if(_rc EQUAL 0)
        file(GLOB_RECURSE _shim7_found "${GEN_DIR}/shim7-bin/ccg-cshost7.dll")
        if(_shim7_found)
            list(GET _shim7_found 0 _shim7_path)
            if(EXISTS "${_shim7_path}")
                file(COPY "${_shim7_path}" DESTINATION "${GEN_DIR}")
            endif()
        endif()
    endif()
    if(NOT EXISTS "${_shim7}")
        message(STATUS "ccg: ccg-cshost7 dotnet build failed rc=${_rc} ${_out} ${_err}")
    endif()
endif()
