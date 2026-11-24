/**
 * @module cg.msbuild.pipeline
 *
 * @brief `ccg msbuild` 的发现、编排与注册表：伞形重导出本域全部子模块。
 *
 * 定位 MSBuild（vswhere、环境/选项覆盖），定位真实 cl（MSBuild
 * -getProperty:VCToolsInstallDir 加"末条路径行"规则、vswhere 兜底、
 * --cl 覆盖；横幅版本供 -fms-compatibility-version），运行喂 wrap
 * 模式的被包装 -t:Rebuild（src/msbuild/wrap.ixx），从 .sln/.vcxproj
 * 输入枚举 Configuration|Platform 对，并把逐配置注册表持久化到
 * `<root>`/.ccg/msbuild.json（ASCII 安全相对路径、原子替换）。
 *
 * 子模块分工——cg.msbuild.pipeline.configs（配置枚举、cl 目录路径解析）、
 * cg.msbuild.pipeline.registry（CL 条目与注册表结构的读写、原子落盘）、
 * cg.msbuild.pipeline.wraplog（wrap 采集日志的装载与产物清单写盘）、
 * cg.msbuild.pipeline.extractargs（被包装 Rebuild 的 MSBuild 命令行构建）、
 * cg.msbuild.pipeline.hostfind（MSBuild 与 cl 的定位、解决方案项目枚举）、
 * cg.msbuild.pipeline.treestate（沙箱差异、树快照与文件状态探针）、
 * cg.msbuild.pipeline.outputs（声明产物提取与树外泄漏扫描）、
 * cg.msbuild.pipeline.extract（被包装 Rebuild 的运行器与采集日志清理）。
 *
 * @internal
 * 全部是文本 + 进程管道：图引擎零改动（既定决策——一个配置一个 db
 * 文件，互不污染）。子进程派生（vswhere/MSBuild/cl 讯问、被包装
 * Rebuild）已收敛到 cg.util.platform 的捕获原语 run_child；纯文本半边
 * （配置枚举、注册表、路径拼接、输出解析）到处可编译且有单元测试。
 */
export module cg.msbuild.pipeline;

export import cg.msbuild.pipeline.configs;
export import cg.msbuild.pipeline.registry;
export import cg.msbuild.pipeline.wraplog;
export import cg.msbuild.pipeline.extractargs;
export import cg.msbuild.pipeline.hostfind;
export import cg.msbuild.pipeline.treestate;
export import cg.msbuild.pipeline.outputs;
export import cg.msbuild.pipeline.extract;
