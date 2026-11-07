---
name: ccg-sync
description: "Use when agent 刚做完 git checkout/switch/pull/merge/stash 等批量代码同步操作，需要让 ccg 索引跟上新的代码状态。Examples: \"git pull 完了\", \"切了个分支\", \"checkout 到旧提交了\", \"rebase 之后索引要刷新\""
---

# git 同步后的索引刷新

单文件编辑由 PostToolUse 钩子自动刷新索引，无需本技能。但 `git checkout / switch / pull / merge / rebase / stash pop` 会一次性改掉成片的文件，钩子不感知 git 操作——这类批量同步后必须主动刷新，否则查询结果是旧代码的。

## 何时使用

- `git pull` / `git fetch + merge` 完成
- `git checkout` / `git switch` 切换分支或提交
- `git stash pop` / `git merge` / `git rebase` / `git reset --hard` 恢复或改写工作区
- 用户问"同步代码后索引要不要更新"

## 判定流程

1. **确认 ccg 可用**：`ccg --version`（PATH 发现；`CCG_BIN` 可覆盖）。不可用则停止，如实告知。

2. **判定索引模式**：项目根存在 `.ccg/msbuild.json`（或 `ccg msbuild --list` 有输出）→ **MSBuild 模式**；否则为普通编译数据库模式。

3. **拿本次变更文件清单**（决定走哪条路）：
   ```bash
   # pull/merge/rebase 之后：
   git diff --name-only ORIG_HEAD..HEAD
   # checkout/switch 前后两个 ref：
   git diff --name-only <旧ref>..<新ref>
   # 拿不到基线时的兜底：
   git status --porcelain
   ```

4. **分支判定**：
   - **MSBuild 模式且清单含 `.sln` / `.vcxproj` / `.props` / `.targets`** → 工程结构或编译口径变了，`ccg update` 覆盖不到（它只重放库内已记录的命令行，不跑 MSBuild）。先重建再收敛：
     ```bash
     ccg msbuild <solution.sln> [--config C --platform P | --all] --force
     ccg update --drain
     ```
     （新增/删除源文件、改编译选项/工具集同理，都走这条路。）
   - **其余情况（只有 .cpp/.h/.hpp 等源码变更，或非 MSBuild 模式）** → 直接喂清单：
     ```bash
     ccg update <变更的源文件...>
     ccg update --drain   # 需要立即收敛类型传播/派发分析时
     ```
     文件很多时可以省略参数，update 会按库内记录的修改时间闭包自行收敛。

## 注意事项

- **并发安全**：update 有租约排队机制（编辑钩子触发的批次会自动合并/排队），与正在跑的后台批次并发是安全的；msbuild 重建遇到"另一个 update 会话持有租约"时会自动排队等待（默认上限 30 分钟）。
- **schema 升级**：报 `schema version mismatch ... recreate with --force` 时，重建类命令（`ccg msbuild` / `ccg index`）带上 `--force`。
- **非 git 项目**（svn/手工拷贝等）：跳过第 3 步，直接把动过的源文件清单喂给 `ccg update`。
- MSBuild 模式下 `ccg msbuild` 默认 record 模式（不真编译，只走一遍构建图记录命令行），大工程也比真编译便宜；需要真编译判定时才用 `--full-build`。
- 重建不会丢 `ccg config set clang-compile-flags ...` 的旗标配置（注入在重放层，见 ccg-index 技能的「解析旗标注入」节）。

## 完整示例（pull 之后，MSBuild 工程）

```bash
git pull
changed=$(git diff --name-only ORIG_HEAD..HEAD)
echo "$changed" | grep -qiE '\.(sln|vcxproj|props|targets)$'
if [ $? -eq 0 ]; then
  ccg msbuild MySolution.sln --all --force && ccg update --drain
else
  ccg update $changed && ccg update --drain
fi
```
