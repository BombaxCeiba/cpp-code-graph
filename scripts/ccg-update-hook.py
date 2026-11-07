#!/usr/bin/env python3
"""`ccg update` 的 Claude Code PostToolUse hook 示例。

从 stdin 读取 PostToolUse 事件 JSON，只保留对语料（corpus）文件的编辑，
然后以分离（DETACHED）方式拉起 `ccg update <file>`，使 hook 在毫秒级
返回、刷新在后台运行。若已有一轮刷新在途，被拉起的 ccg 会把本次文件
排进数据库的 pending_updates 表后退出——hook 绝不阻塞编辑器。

本文件是带文档的示例（SAMPLE）。是否把它接进任何 settings.json 刻意
留给用户决定；ccg 从不改动设置文件。

环境变量：
  CCG_HOOK_CCG   要拉起的 ccg 可执行文件                （默认："ccg"）
  CCG_DB         让 ccg 更新其数据库的 ccg 输出目录（FOLDER，
                 或其中的一个配置目录）；被拉起的进程继承它，否则适用
                 ccg 自身的发现规则（被拉起进程工作目录下的
                 ./.ccg；恰好只含一个配置的目录会自动选中该配置）
  CCG_HOOK_LOG   追加写入被拉起进程的 stdout/stderr      （默认：丢弃）

退出码恒为 0：hook 绝不能让它所观察的那次工具调用失败。
"""

from __future__ import annotations

import json
import os
import subprocess
import sys

# 监听面：Claude Code PostToolUse 事件里会改写文件的四个工具名。
# Update 载荷同为 "file_path"。
EDIT_TOOLS = {"Edit", "Write", "MultiEdit", "Update"}

# ccg 会解析的扩展名（主源文件加上头文件与模块单元）；其余一切
# （配置、文档、脚本、锁文件……）都不属于语料（corpus）。
CORPUS_EXTS = {
    ".c", ".cc", ".cpp", ".cxx", ".c++",
    ".h", ".hh", ".hpp", ".hxx", ".inc", ".inl",
    ".ixx", ".cppm", ".mpp", ".mxx", ".cxxm", ".c++m",
}


def spawn_detached(argv: list[str]) -> None:
    """在两个平台上都以与当前进程解耦的方式启动 argv。"""
    log_path = os.environ.get("CCG_HOOK_LOG")
    # 日志打开失败（坏路径/权限）绝不拖垮 hook：退出码恒 0 是文档契约，
    # 退回丢弃输出（DEVNULL），与其余失败形态的吞掉语义一致。
    try:
        log = open(log_path, "a") if log_path else subprocess.DEVNULL
    except OSError:
        log = subprocess.DEVNULL
    kwargs: dict[str, object] = {}
    if os.name == "nt":
        # DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP：不弹控制台窗口、
        # 不共享 Ctrl+C 组，且在本 hook 退出后仍存活。
        kwargs["creationflags"] = 0x00000008 | 0x00000200
    else:
        # 新会话（setsid 语义）：脱离本进程组、重新挂靠（reparent）。
        kwargs["start_new_session"] = True
    try:
        subprocess.Popen(
            argv,
            stdin=subprocess.DEVNULL,
            stdout=log,
            stderr=log,
            close_fds=True,
            **kwargs,  # type: ignore[arg-type]
        )
    except OSError as exc:
        # 拉起失败可被观察到，但对 hook 永不致命。
        print(f"ccg-update-hook: spawn failed: {exc}", file=sys.stderr)
    finally:
        if log is not subprocess.DEVNULL:
            log.close()


def main() -> int:
    try:
        event = json.loads(sys.stdin.read())
    except Exception:
        return 0  # 不是我们能理解的 hook 负载；保持静默

    if event.get("tool_name") not in EDIT_TOOLS:
        return 0
    path = (event.get("tool_input") or {}).get("file_path") or ""
    if not path:
        return 0
    if os.path.splitext(path)[1].lower() not in CORPUS_EXTS:
        return 0

    argv = [os.environ.get("CCG_HOOK_CCG", "ccg"), "update", path]
    db = os.environ.get("CCG_DB")
    if db:
        argv += ["-o", db]
    spawn_detached(argv)
    return 0


if __name__ == "__main__":
    sys.exit(main())
