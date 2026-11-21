# CPack 出包配置（Inno Setup 注入式安装包）。
#
# 用法契约：cmake --preset msvc-dev 之后
#   cpack --config build/msvc-dev/CPackConfig.cmake -C Release -G INNOSETUP -B dist
# 出 dist/ccg-setup-<版本>.exe。-C Release 不可省：Ninja Multi-Config 树
# 的 cpack 配置取自 -C，缺省会聚不到任何配置（经典 Debug 空包事故）。
# cpack 自动先跑仓内 install() 规则聚安装树再交 ISCC 编译——装什么
# 只有 install() 一份清单（cmake/Install.cmake），安装包零双清单维护。
#
# 版本号纯 tag 驱动（无版本文件、无源内手写）：优先级 = 显式
# -DCCG_RELEASE_VERSION 缓存值 > 环境变量 CCG_RELEASE_VERSION >
# 0.0.0-dev（本地无注入的默认值）。CI 在 tag 上剥 v 前缀后经环境变量
# 注入，安装器文件名与包版本随之生成。
#
# 安装形态：仅 Windows、默认 user 级
# （PrivilegesRequired=lowest 永不请求管理员，安装根随 {autopf} 落
# 用户本地 Programs 目录）、per-machine 档与桌面快捷方式不做；唯一
# 勾选项 addtopath（默认勾选）走垫片方案——在 %LOCALAPPDATA%\ccg\bin
# 写两个转发垫片并把该目录幂等追加进用户 Path，与仓内
# scripts/ccg-install.cmd 的手动安装形态同构；卸载删垫片目录并按分号
# 拆数组过滤 Path 条目写回（保 REG_EXPAND_SZ 类型），细节在
# packaging/tasks.iss 与 packaging/shim-code.pas。

# ---------------------------------------------------------------- 版本 ---
if(NOT DEFINED CCG_RELEASE_VERSION)
    if(DEFINED ENV{CCG_RELEASE_VERSION})
        set(CCG_RELEASE_VERSION "$ENV{CCG_RELEASE_VERSION}")
    else()
        set(CCG_RELEASE_VERSION "0.0.0-dev")
    endif()
endif()

# ------------------------------------------------------------ 基础族 ---
set(CPACK_PACKAGE_NAME "ccg")
set(CPACK_PACKAGE_VENDOR "Dusk_NM02")
set(CPACK_PACKAGE_VERSION "${CCG_RELEASE_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "ccg-setup-${CCG_RELEASE_VERSION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "ccg")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "ccg - C++ code graph indexer and query CLI")
# 桌面快捷方式不做（既定决策；CLI 工具无意义），addtopath 是唯一
# 勾选项。CPack Inno 生成器默认在 [Tasks] 段无条件写一个 desktopicon
# 任务（默认不勾且 [Icons] 无引用、勾了也无效果）——只有把
# CPACK_CREATE_DESKTOP_LINKS 显式设为空串才会跳过它（生成器源码
# IsSetToEmpty 分支）。
set(CPACK_CREATE_DESKTOP_LINKS "")
# 许可页显示 Apache-2.0 全文；NOTICE 与第三方声明作为普通文件随
# install() 规则进包（见 cmake/Install.cmake 尾部）。
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")

# ------------------------------------------------------- Inno 注入族 ---
# 目标架构：安装根 {autopf} 在 lowest 权限下解析为 userpf
# （用户本地 Programs 目录），不写系统级目录、不请求管理员。
set(CPACK_INNOSETUP_ARCHITECTURE "x64")
# 简体中文是 Inno Setup 7.0.2+ 的官方语言（编译机须装 Inno 7+；
# CI 在 cpack 前显式安装 Inno Setup 7.1.1，本机同样在席）。
set(CPACK_INNOSETUP_LANGUAGES "english;chinesesimplified")
# [Setup] 段透传：user 级 + 环境（PATH）变更广播（装/卸载完成后
# WM_SETTINGCHANGE("Environment") 即时通知新开终端可见）。
set(CPACK_INNOSETUP_SETUP_PrivilegesRequired "lowest")
set(CPACK_INNOSETUP_SETUP_ChangesEnvironment "yes")
# 砍掉向导的「选择开始菜单文件夹」页：CPack Inno 生成器默认
# [Icons] 段为空（本包不建任何快捷方式），该页对产物无作用，
# 页序规格（语言→许可→目录→勾选→完成）不含它。
set(CPACK_INNOSETUP_SETUP_DisableProgramGroupPage "yes")
# 产物名 ccg-setup-<版本>.exe（显式钉住 OutputBaseFilename，优先级
# 高于 CPACK_PACKAGE_FILE_NAME 的派生路径，双保险）。
set(CPACK_INNOSETUP_SETUP_OutputBaseFilename
    "ccg-setup-${CCG_RELEASE_VERSION}")

# 注入资产（相对 .iss 的 Source 路径经 public 宏 CCGSourceRoot 传递，
# 宏在 extra script 内随处可用——官方 CPACK_INNOSETUP_DEFINE_ 机制）：
#   tasks.iss      [Tasks] 勾选项 + [Registry] 条件写 HKCU Path
#                  + 双语完成页提示（[CustomMessages]/[Messages]）
#   shim-code.pas  垫片写入（幂等）与卸载过滤的 Pascal 实现
set(CPACK_INNOSETUP_EXTRA_SCRIPTS
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/tasks.iss")
set(CPACK_INNOSETUP_CODE_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/shim-code.pas")
set(CPACK_INNOSETUP_DEFINE_CCGSourceRoot "${CMAKE_CURRENT_SOURCE_DIR}")

# ISCC 发现：默认按名找（PATH 上有即用），系统级安装目录经
# 环境变量构造兜底（CI 把 Inno Setup 7 装进系统级目录，本机同
# 形态目录在席）。找不到时不在此报错：cpack -G INNOSETUP 自会
# 给出可读的失败信息。
find_program(CCG_ISCC_EXECUTABLE NAMES ISCC
    HINTS
        "$ENV{ProgramFiles}/Inno Setup 7"
    DOC "Inno Setup compiler used by the CPack INNOSETUP generator")
if(CCG_ISCC_EXECUTABLE)
    set(CPACK_INNOSETUP_EXECUTABLE "${CCG_ISCC_EXECUTABLE}")
endif()

include(CPack)
