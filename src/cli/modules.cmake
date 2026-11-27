# 命令行层（cg.cli）的模块单元
ccg_add_dir_modules(cg_core "${CMAKE_CURRENT_LIST_DIR}"
    cli.parse.model.ixx
    cli.parse.usage.ixx
    cli.parse.opts.ixx
    cli.parse.parse.ixx
    cli.parse.ixx
    cli.render.base.ixx
    cli.render.symbol.ixx
    cli.render.context.ixx
    cli.render.impact.ixx
    cli.render.stats.ixx
    cli.render.changes.ixx
    cli.render.ixx
    cli.lookup.name.ixx
    cli.render.disclose.ixx
    cli.scan.names.ixx
    cli.layout.ixx
    cli.commands.matchers.ixx
    cli.commands.runs.ixx
    cli.commands.entry.ixx
    cli.commands.ixx
    # 共享的索引主干（原先是内部分区，后来是
    # 纯声明接口；自模块布局定稿起
    # 定义就地内联）与各 CLI 运行器模块。
    cli.runners.engine.ixx
    cli.runners.resolve.ixx
    cli.runners.tail.ixx
    cli.runners.ixx
    cli.index.ixx
    cli.update.ixx
    cli.msbuild.ixx
    cli.config.ixx
    cli.hook.ixx
    cli.ixx)
