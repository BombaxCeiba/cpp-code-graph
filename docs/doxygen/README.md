# Doxygen 文档构建

## 用法

```bash
cmake --preset clang-dev -DCCG_BUILD_DOCS=ON   # 或对已有树：cmake -B build/clang-dev -DCCG_BUILD_DOCS=ON
cmake --build build/clang-dev --target docs    # 产物：build/clang-dev/docs/doxygen/html/index.html
```

`CCG_BUILD_DOCS` 默认 OFF（零感知：不 find_package、不加目标）。docs 目标不进 ALL，
须显式 `--target docs`；custom target 无 per-config 变体，clang-dev / msvc-dev 双树行为一致。

## 发现链（无硬编码）

1. `-DDOXYGEN_EXECUTABLE=<路径>`（CMake 缓存变量，优先级最高）；
2. PATH 上的 doxygen。

## 版本门槛

Doxygen ≥ 1.16 为硬要求：C++20 命名模块自 1.10 起支持，但 1.16.0 才修复
「module 内 namespace 之后 class 成员被记到全局作用域」（上游 issue #11685）——
本仓全部导出都在 `export namespace cg::…` 里，低版本会击穿成员归属。
版本不足时 configure 阶段直接 FATAL_ERROR。

## 配置要点（cmake/Docs.cmake 的 DOXYGEN_* 变量块）

- 配置机制：当前 CMake（4.4）的 `doxygen_add_docs` 不支持模板文件参数（历史上的
  `DOXYFILE_IN` 关键字已不存在，传了会被当作输入文件吞进 INPUT）——配置一律走
  `DOXYGEN_*` 变量，由 doxygen_add_docs 生成 build 树内的 Doxyfile.docs。
  `docs/doxygen/Doxyfile.in` 是与该变量块同源的模板，configure_file 产出
  `build/<树>/docs/doxygen/Doxyfile.standalone` 供脱离 CMake 手动重跑。
  **修改配置只改 cmake/Docs.cmake 一处。**
- `EXTENSION_MAPPING` 显式钉死 `.ixx=C++`（默认映射历史上有过变动）。
- `FILE_PATTERNS` 含 `*.md`：目录扫描才拾得起 docs/pages 的中文页（.md 语言由
  Doxygen 内建识别，勿写进 EXTENSION_MAPPING——"Markdown" 是非法映射值）。
- `INPUT_ENCODING=UTF-8`：文档注释为中文，防 Windows 本地编码漂移。
- `PROJECT_BRIEF` 等 Doxyfile 值一律 ASCII：Doxygen 的 Windows 版经进程 ANSI
  代码页读 Doxyfile 自身（DOXYFILE_ENCODING 不治），GBK 语境下 UTF-8 中文值
  误读成乱码；中文简介挂 docs/pages/mainpage.md 的 @brief。
- `INTERNAL_DOCS=NO`：源码中 `@internal` 段（函数体/GMF 上移的实现内幕）不渲染进公开文档站。
- `EXCLUDE_PATTERNS` 排除 `.ccg/`（数据库域）、`build/`、`publish/`、`3rdParty/`、`Testing/` 与缓存目录。

## 调用图（ccg graph 注入）

- graphviz 发现链同 doxygen：`-DCCG_DOT_EXECUTABLE=<路径>` 缓存变量或 PATH 上的
  `dot`。发现 -> `HAVE_DOT=YES` + `DOT_PATH`（bin 目录；Doxyfile 无
  `DOT_EXECUTABLE` 标签，写错只会得到 warning）；未发现 -> `HAVE_DOT=NO`，docs
  目标跳过调用图生成并 STATUS 提示（优雅降级不失败，callgraphs 页退化为纯文字）。
- docs 目标前置 `ccg_docs_dots`：跑构建出的 `ccg graph` 从源码树自索引库
  `.ccg/main/ccg.db`（自索引配置 main）导出模块总图与固定入口图到 `build/<树>/docs/doxygen/dot/`（即
  `DOTFILE_DIRS`）；库缺失时同样优雅跳过。
- `docs/pages/callgraphs.md` 以 `@dotfile` 挂图（中文 caption + INTERACTIVE_SVG
  缩放 + md5 内容指纹缓存）。注意：markdown 标题里不能出现字面 `@dotfile`
  字样（Doxygen 会当命令解析）。

## 已知噪声

`import std;` 在词法级解析下会产生指向合成 std 模块的未解析边/少量警告——
不阻塞生成，属预期噪声。
