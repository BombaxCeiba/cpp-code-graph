module;
// capture 表模型置于全局模块片段：本单元重导出 wrap/pipeline，二者
// 的导出内联函数（capture 读写）携带 capture 表模板的实例化记录；
// MSVC 合并这些记录时要求表模型定义在本单元的全局片段可见（与
// db.tables.h 同一探针矩阵结论，机理详见 capture.tables.h）。
#include "capture.tables.h"

/**
 * @module cg.msbuild
 *
 * @brief MSBuild 层的主接口：伞形重导出本层全部子模块。
 *
 * 本层是设计上的 Windows 专属能力；代码本身是纯文本处理，平台洁净。
 * 子模块分工——cg.msbuild.translate（cl 方言编译命令适配器：cl 风格
 * flags 原样保留、供 --driver-mode=cl 重放）、cg.msbuild.wrap（ccg.exe
 * 内部的 CLToolExe 包装模式）、cg.msbuild.pipeline（发现、编排、注册表）、
 * cg.msbuild.eval（求值通道客户端：发现链推导 + cshost 桥上的
 * MsbuildEval 会话驱动与事件流消费）。
 *
 * @internal
 * 适配器的契约：cl 风格 flags 逐字保留进入编译数据库，仅在重放侧注入
 * --driver-mode=cl 并剔除位置输入文件、产物路径类 flag 与非 C/C++ 工具
 * 条目——cl 模式原生解析 /I /D /std: 等拼写并自配置 MSVC 环境。包装
 * 模式经 MSBuild 的 CLToolExe/CLToolPath 全局属性把每次 cl 调用改道进
 * ccg.exe 的 wrap 分支（环境变量触发），逐条记录 argv/cwd/INCLUDE 后
 * 转发给真实 cl，或记录模式下零写盘空跑（只记 argv）。历史上的模块分
 * 区已拆分为点分命名模块。cg.msbuild.compose（求值通道的 argv 组装器：
 * 求值元数据 → ClInvocation，记录通道 wrap 捕获的对照生产侧）。
 */
export module cg.msbuild;

export import cg.msbuild.translate;
export import cg.msbuild.wrap;
export import cg.msbuild.pipeline;
export import cg.msbuild.eval;
export import cg.msbuild.compose;
