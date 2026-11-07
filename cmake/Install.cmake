# Release 安装规则（cmake --install）；用法契约与
# PATH shim 机制记录在本文件与 scripts/ccg-install.cmd/.sh 的头注释。
#
# 定义：唯一一个前缀目录装下发行二进制在其自身位置解析到的
# 全部东西——ccg(.exe)、.ccg/{hooks,.claude}（hook 注册在
# <exe-dir>/.ccg/hooks/ 解析跳板，见 src/cli/cli.hook.ixx 的
# script_path_for）、docs/（随包手册：英文在根、中文在
# docs/zh-cn/ 子目录）、以及 PATH shim 安装器。扁平
# 形态就是发行布局的契约；拆散它会破坏 hook 注册与随包手册的就近布局。
#
# 可靠性边界：前缀目录之外零触碰，且不安装任何工具链运行期 DLL
# 闭包——ccg 的 libclang 引擎在运行期经 llvm-path 配置 /
# CCG_LLVM_ROOT / PATH 解析（见 cmake/CcgUtils.cmake 的
# ccg_toolchain_dlls 注释），工具链运行库则搭 PATH 或可选的
# CCG_COPY_RUNTIME_DLLS 复制步骤。唯一例外是下方随包携带的两份
# PowerShell 垫片程序集：它们是 ccg 自家产物而非工具链闭包。
# 多配置树的 install 命令行需要显式 --config（Release 是发行形态）。
#
# 回退路径，全部原样可用：指向二进制的 CCG_BIN（在任何 PATH
# 查找之前被 hook 跳板尊重）、手工加入 PATH 的 release 目录
# （shim 是便利品，不是必需品）、以及 dev 形态本身——构建树
# 二进制加 src/CMakeLists.txt 的 POST_BUILD 复制。

# 前缀根上的二进制：与 .ccg/ 和 docs/ 并排，正是 hook 注册
# 与文档引用所期待的布局。
install(TARGETS ccg RUNTIME DESTINATION .)

# C# 对象宿主垫片的两份程序集（cmake/CsShimGen.cmake 的构建期产物，
# ${CCG_CSHOST_GEN} 由 src/CMakeLists.txt 以 CACHE INTERNAL 供给）：
# 装在二进制旁，cg.util.cshost 的垫片定位唯一路线就是同目录直载的
# ccg-cshost51/7.dll（开发树由 src/CMakeLists.txt 的 POST_BUILD 拷贝
# 摆成同一布局），用户侧零运行时投放。OPTIONAL：构建机缺 csc/dotnet
# 时产物缺席，安装静默跳过，与"构建永不因此变红"的降级纪律一致。
if(WIN32)
    install(FILES
        "${CCG_CSHOST_GEN}/ccg-cshost51.dll"
        "${CCG_CSHOST_GEN}/ccg-cshost7.dll"
        DESTINATION .
        OPTIONAL)
endif()

# publish/ -> .ccg/：与 src/CMakeLists.txt 的 POST_BUILD 步骤同样的
# 合并语义（hook 跳板加 .claude skill/plugin 家族），包括 skills
# 折叠进 plugin 通道（publish/ 发行 plugin 时不带 skills 副本；
# 唯一源 publish/.claude/skills 只在复制/安装时落到那里，因此
# 两个通道供同一套 skill 集）。
install(DIRECTORY publish/ DESTINATION .ccg)
install(DIRECTORY publish/.claude/skills/
        DESTINATION .ccg/.claude/plugin/skills)

# 随包手册集（docs/en/ 的英文六件装进 docs/ 根，docs/zh-cn/
# 的中文译文六件装进 docs/zh-cn/——具体清单即下方两个
# install(FILES)；源树另有 MSVC 工具链构建一篇不随包分发）：
# --help 与报错文本的关键事实已内联进输出本身，完整手册装在
# 二进制旁——只拿到二进制的人也能读到全文。该集合必须与
# src/CMakeLists.txt 的 POST_BUILD 复制保持同步。
install(FILES
    docs/en/CLI.md
    docs/en/REALTIME.md
    docs/en/MSBUILD.md
    docs/en/SCHEMA.md
    docs/en/DESIGN.md
    docs/en/INSTALL.md
    DESTINATION docs)
install(FILES
    docs/zh-cn/CLI.md
    docs/zh-cn/REALTIME.md
    docs/zh-cn/MSBUILD.md
    docs/zh-cn/SCHEMA.md
    docs/zh-cn/DESIGN.md
    docs/zh-cn/INSTALL.md
    DESTINATION docs/zh-cn)

# 二进制旁的 PATH shim 安装器：运行其中一个即把裸 `ccg` 命令
# 登记上 PATH，而不挪动 release 目录。
install(PROGRAMS
    scripts/ccg-install.cmd
    scripts/ccg-install.sh
    DESTINATION .)

# 随包法律文件（Apache-2.0 分发义务）：许可证全文、NOTICE 与第三方
# 声明装进前缀根。cmake --install（便携 zip 的 staging 源）与 CPack
# 安装包（自动复用本文件规则，见 cmake/CPackSetup.cmake）两条发行
# 形态一次声明全覆盖；许可页另由 CPACK_RESOURCE_FILE_LICENSE 单独
# 指向 LICENSE 全文。
install(FILES
    LICENSE
    NOTICE
    THIRD-PARTY-NOTICES.md
    DESTINATION .)
