# Doxygen HTML API 文档接线（由根 CMakeLists.txt 在 CCG_BUILD_DOCS=ON 时 include）。
#
# 发现链：find_package(Doxygen) 尊重 DOXYGEN_EXECUTABLE 缓存变量（命令行
# -DDOXYGEN_EXECUTABLE=... 注入），其次 PATH——本文件不硬编码任何机器路径。
# 版本门槛：>= 1.16。C++20 命名模块自 1.10 起支持，但 1.16.0 才修复「module 内
# namespace 之后 class 成员被记到全局作用域」（上游 issue #11685）；本仓全部
# 导出都在 export namespace cg::… 里，低版本会击穿成员归属（详见
# docs/doxygen/README.md）。
#
# 配置机制：当前 CMake（4.4）的 doxygen_add_docs 不支持模板文件参数（历史上的
# DOXYFILE_IN 关键字已不存在，会被当作输入文件吞进 INPUT），配置一律走
# DOXYGEN_* 变量。为避免两处维护，下方 DOXYGEN_* 变量块是唯一事实源：
# 它同时喂给 doxygen_add_docs（生成 build 树内 Doxyfile.docs）与
# docs/doxygen/Doxyfile.in 模板（configure_file 产出可手动直跑的独立 Doxyfile）。
# docs 为 custom target（无 per-config 变体，双工具链树行为一致）且不进 ALL，
# 须显式 --target docs。
find_package(Doxygen REQUIRED)
if(Doxygen_VERSION VERSION_LESS 1.16)
    message(FATAL_ERROR
        "Doxygen >= 1.16 required (C++20 modules + export-namespace member "
        "scoping, upstream issue #11685); found ${Doxygen_VERSION}. "
        "Override via -DDOXYGEN_EXECUTABLE=... or PATH.")
endif()

set(DOXYGEN_PROJECT_NAME    "cpp-code-graph (ccg)")
# Doxygen 的 Windows 版经进程 ANSI 代码页读 Doxyfile 自身（DOXYFILE_ENCODING
# 管的是 INPUT 文件、不治自身）：GBK 语境下 UTF-8 中文值被误读成乱码（调研
# 期实测；本机 Git Bash 语境则字节透传）——中文值是否乱码取决于运行环境。
# 故 Doxyfile 内的值一律 ASCII（对任意代码页免疫），中文简介挂在
# docs/pages/mainpage.md 的 @brief（经 INPUT_ENCODING=UTF-8 正常）。
set(DOXYGEN_PROJECT_BRIEF   "C++23 code knowledge-graph engine (named modules)")
set(DOXYGEN_RECURSIVE       YES)
# *.md 使 docs/pages 的中文页（mainpage/callgraphs）真被目录扫描拾起——
# FILE_PATTERNS 门住目录扫描，而 .md 语言由 Doxygen 内建识别（勿写进
# EXTENSION_MAPPING，"Markdown" 非法映射值会报错）。
set(DOXYGEN_FILE_PATTERNS   *.ixx *.cpp *.h *.md)
# 防御性钉死扩展名语言（.ixx 的默认映射历史上随版本变动过）
set(DOXYGEN_EXTENSION_MAPPING ".ixx=C++ .cppm=C++ .cpp=C++ .h=C++")
# 文档注释为中文，显式钉死防本地编码漂移
set(DOXYGEN_INPUT_ENCODING  UTF-8)
# 排除构建产物与发行物（布局裁定：.ccg/<config>/ccg.db + publish/）
set(DOXYGEN_EXCLUDE_PATTERNS */.ccg/* */build/* */publish/* */3rdParty/* */.wf/*
                              */Testing/* */.cpm-cache/*)
# @internal 段（自体内上移的实现内幕）不进公开文档站，仅存源码
set(DOXYGEN_INTERNAL_DOCS   NO)
set(DOXYGEN_EXTRACT_ALL     YES)   # 迁移期兜底；全量迁移完成后收窄
set(DOXYGEN_EXTRACT_PRIVATE NO)
set(DOXYGEN_ENABLE_PREPROCESSING YES)  # GMF 里 #include 的条件块需要
set(DOXYGEN_MACRO_EXPANSION NO)        # 不展开 sqlpp23/boost 宏，词法级足够
set(DOXYGEN_EXCLUDE_SYMBOLS      detail::*)
set(DOXYGEN_SHOW_FILES      YES)
set(DOXYGEN_GENERATE_LATEX  NO)
set(DOXYGEN_WARN_NO_PARAMDOC NO)      # 初期不强制参数文档
# WARN_IF_UNDOCUMENTED 在 EXTRACT_ALL=YES 期间自动失效，迁移完成后由其接管
set(DOXYGEN_WARN_IF_UNDOCUMENTED YES)

# graphviz 发现链（DOT_EXECUTABLE 缓存变量或 PATH，不硬编码机器路径）：
# 发现 -> HAVE_DOT=YES + DOT_PATH + docs 目标前置 ccg graph 生成 .dot；
# 未发现 -> HAVE_DOT=NO，跳过调用图生成并 STATUS 提示（优雅降级不失败，
# 站内 callgraphs 页退化为纯文字，@dotfile 仅告警不报错）。
find_program(CCG_DOT_EXECUTABLE NAMES dot)
if(CCG_DOT_EXECUTABLE)
    set(DOXYGEN_HAVE_DOT YES)
    get_filename_component(_ccg_dot_dir "${CCG_DOT_EXECUTABLE}" DIRECTORY)
    set(DOXYGEN_DOT_PATH "${_ccg_dot_dir}")
else()
    set(DOXYGEN_HAVE_DOT NO)
    set(DOXYGEN_DOT_PATH "")
endif()
# 调用图注入（docs/pages/callgraphs.md 的 @dotfile 数据源）：svg 输出 +
# 缩放交互；@dotfile 图不走 DOT_GRAPH_MAX_NODES 的每函数截断，调大以容
# 模块总图级别的大图。
set(DOXYGEN_DOT_IMAGE_FORMAT   svg)
set(DOXYGEN_INTERACTIVE_SVG    YES)
set(DOXYGEN_DOT_GRAPH_MAX_NODES 200)
# @dotfile 的搜索路径（须在 doxygen_add_docs 之前定好）
set(CCG_DOXYGEN_DOT_DIR "${CMAKE_BINARY_DIR}/docs/doxygen/dot")
set(DOXYGEN_DOTFILE_DIRS       "${CCG_DOXYGEN_DOT_DIR}")

set(DOXYGEN_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/docs/doxygen")
# INPUT 收录 src、站内中文页与随包手册两个语言目录（docs/en/ 与
# docs/zh-cn/ 同名七件；译文目录在翻译阶段产出前缺席时 Doxygen
# 仅告警跳过，docs 目标不失败）。
doxygen_add_docs(docs "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/docs/pages"
    "${CMAKE_SOURCE_DIR}/docs/en" "${CMAKE_SOURCE_DIR}/docs/zh-cn"
    COMMENT "生成 ccg API 文档（Doxygen HTML）")

# 调用图 .dot 前置生成（docs/pages/callgraphs.md 的 @dotfile 数据源）：
# 模块总图 + 固定入口清单（命令分派扇出与索引运行器）的执行流图，数据
# 读源码树自索引库的 main 配置（.ccg/main/ccg.db；自索引配置名=main）。
# 入口必须唯一解析（run_main 有两个重载、
# 属同文件同 kind 的死角，故选 dispatch）。graphviz 或自索引库缺失时优
# 雅跳过——callgraphs 页退化为纯文字、@dotfile 仅告警，docs 不失败。
set(CCG_DOCS_LAYOUT "${CMAKE_SOURCE_DIR}/.ccg")
set(CCG_DOCS_DB "${CCG_DOCS_LAYOUT}/main/ccg.db")
if(DOXYGEN_HAVE_DOT AND EXISTS "${CCG_DOCS_DB}")
    add_custom_target(ccg_docs_dots
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CCG_DOXYGEN_DOT_DIR}"
        COMMAND $<TARGET_FILE:ccg> graph --modules --min-weight 3
                --file src/ --format dot
                --output "${CCG_DOXYGEN_DOT_DIR}/modules.dot"
                -o "${CCG_DOCS_LAYOUT}"
        COMMAND $<TARGET_FILE:ccg> graph --symbol cg::cli::dispatch
                --depth 2 --format dot
                --output "${CCG_DOXYGEN_DOT_DIR}/dispatch.dot"
                -o "${CCG_DOCS_LAYOUT}"
        COMMAND $<TARGET_FILE:ccg> graph --symbol cg::cli::detail::run_index
                --depth 2 --format dot
                --output "${CCG_DOXYGEN_DOT_DIR}/run-index.dot"
                -o "${CCG_DOCS_LAYOUT}"
        DEPENDS ccg
        COMMENT "生成 ccg 调用图 .dot（ccg graph，数据源自索引库）"
        VERBATIM)
    add_dependencies(docs ccg_docs_dots)
elseif(DOXYGEN_HAVE_DOT)
    message(STATUS
        "docs: 自索引库 .ccg/main/ccg.db 不存在 - 跳过调用图生成（先在"
        "仓库根跑 ccg index --config main；callgraphs 页暂为纯文字）")
endif()

# 独立运行入口：模板与上方 DOXYGEN_* 同源，供脱离 CMake 手动重跑/调试
# （产物落 build/<树>/docs/doxygen/html）。INPUT 在此显式写死（等价于上方
# doxygen_add_docs 的实参）——doxygen_add_docs 会用自己的实参覆盖 DOXYGEN_INPUT，
# 两边无法共享同一个变量。
# configure_file 对列表变量按分号展开，而 Doxyfile 以空格分隔——先转写。
string(REPLACE ";" " " _DOXYGEN_FILE_PATTERNS_STR "${DOXYGEN_FILE_PATTERNS}")
string(REPLACE ";" " " _DOXYGEN_EXCLUDE_PATTERNS_STR "${DOXYGEN_EXCLUDE_PATTERNS}")
configure_file("${CMAKE_SOURCE_DIR}/docs/doxygen/Doxyfile.in"
    "${DOXYGEN_OUTPUT_DIRECTORY}/Doxyfile.standalone" @ONLY)
