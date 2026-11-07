---
name: ccg-impact
description: "Use when 用户想知道改动的影响面、改动是否安全、某个符号被哪些代码依赖，或改了几个文件后谁会被波及。Examples: \"改这个函数安全吗\", \"X 被谁依赖\", \"我改了这几个文件会影响什么\", \"这个类的影响面有多大\""
---

# ccg 改动安全分析

改动前评估（符号级 blast radius）与改动后盘点（文件级波及面），配统一的风险分级解读。

## 何时使用

- "改 X 安全吗 / X 被谁依赖" → `ccg impact`
- "我改了这几个文件，影响什么" → `ccg changes`
- 需要风险分级（LOW/MEDIUM/HIGH/CRITICAL）支撑决策

## 前置检查

1. `ccg stats` 确认 db 存在（没有则先走 `ccg-index` 建库）。
2. 改动后的文件先刷新：`ccg update <file>...`（装了 hook 的项目通常已自动折入）。
3. 涉及文件级分析时准备好改动文件列表（ccg 与 VCS 无关，列表由你提供）。

## ccg impact：符号级影响面

```
ccg impact <sym> [--direction up|down] [--depth N] [--no-merge] [--no-dispatch|--dispatch-fallback] [--json]
```

- **up（默认）= 谁依赖它**：传递 callers + extends 它的类型 + uses 它声明类型的符号。
- **down = 它依赖谁**：传递 callees + 基类 + 使用的记录；记录的 down-walk 含字段 uses（布局依赖）——改一个类定义前跑 down 看它依赖什么同样有价值。
- `--depth` 默认 3（1-10）；虚方法默认合并 override 家族（整个家族的 blast radius），`--no-merge` 排除。
- 动态派发证据默认开启：有 confidence ≥ 0.7（1.0/0.7）证据的调用点按证据目标归属，不再按整个 override 家族过近似；无证明目标的家族结果标 `[dispatch unknown · family estimate]`。0.4 弱推断低于阈值，按家族估算处理。风险分按精确结果计算，家族估算不抬风险级。
- `--no-dispatch` 是纯静态保守上界（与引入派发前逐字节一致）；`--dispatch-fallback` 额外显示低于阈值的 0.4/0.0 家族标记（不改变归属集合）。

### 风险模型（impact 与 changes 共用）

```
score = 3*direct + transitive + fan_in
LOW ≤ 8 < MEDIUM ≤ 30 < HIGH ≤ 120 < CRITICAL
```

- **up 方向零直接影响 = UNKNOWN**：这是"无边不是安全证据"的诚实表达（符号可能未被使用，也可能只经未解析目标被调用），必须原样传达，不得粉饰成 LOW。
- 同一 db 状态下分数与档位确定（可复现）。

## ccg changes：文件级波及

```
ccg changes <file>... [--from-file LIST] [--depth N] [--json]
git diff --name-only | ccg changes        # stdin 管道（ccg 自己绝不调 git）
```

- **VCS 无关铁律**：喂文件列表（argv / `--from-file` / stdin 管道；真控制台裸跑是用法错误，防挂起）。相对路径以工作目录绝对化。
- 每个文件的状态三态，绝不静默丢：`present`（含符号数）/ `deleted`（文件已删但 db 行还在——**删除要在下次 index 前查 changes 才可见**）/ `unknown`（不在库中）。
- 消费者经 calls/extends/uses/overrides 双向传播，最短深度去重；每消费者带风险（`(depth1?3:1)+fan_in`），汇总用 impact 公式。
- 空结果（无已知文件或无消费者）退出码 1 但 payload 照打印。
- `--depth` 默认 3。

## 剧本：一次典型的改动评估

```
1. ccg query <name>              # 定位候选，选定消歧器
2. ccg impact <sym> --direction up    # 谁会被波及 + 风险档
3. ccg impact <sym> --direction down  # 它依赖什么（改动的地基）
4. （动手改）
5. ccg update <改动的文件>...    # 折入 db
6. ccg changes <改动的文件>...   # 文件级波及面盘点
7. 对高风险消费者逐个 ccg context 复核
```

## hook 注入的摘要怎么读

装了 hook 的项目，每次编辑后自动注入一行类似：

```
[ccg] 已入库 canvas.hpp（重析 3 TU）；受影响符号 4（直接 3），整体风险 MEDIUM；最相关: shape::Circle (circle.hpp:12)、...
```

这就是 changes 的浓缩（受影响符号数 + 风险级 + top 消费者，有界截断）。看到 HIGH/CRITICAL 时应停下来向用户复述影响面再继续；注入行只是摘要，正式评估仍以 `ccg changes --json` 全量输出为准。

## USES 边语义（record 级 impact 的根基）

字段/参数/返回值/局部变量/命名空间变量的声明类型引用项目内记录时产生 uses 边：cv/ref/指针/数组修饰剥除、typedef 追根、模板特化折主模板、成员指针取类类型。**标准库/系统头里的类型不产边**——所以"改我的类会影响谁"的答案只覆盖项目内依赖，这是设计而非遗漏。

## 常见坑

- **UNKNOWN ≠ LOW**：up 零直接影响时如实报 UNKNOWN。
- **deleted 文件的窗口期**：文件删了 db 行还在，下次 index 才清；想看"删除影响"就在 index 前跑 changes。
- **truncated: yes 仍 exit 0**：计数完整，只是深度扩展被截。
- 表格长路径列溢出是显示 nit；机器分析用 `--json`。
- `--json` 字段速查：impact 得 `target/direction/depth/impactedCount/direct/transitive/fanIn/riskScore/byDepthCounts/byDepth/risk/truncated`；changes 得 `depth/files/seeds/consumers/direct/transitive/fanIn/riskScore/risk/truncated`。

## Checklist

- [ ] db 存在且已折入最新改动
- [ ] up + down 双向都看过
- [ ] 风险档位与 score 原样转述（尤其 UNKNOWN）
- [ ] 文件级盘点用了 changes 而非只看 impact
- [ ] 高风险消费者逐个复核

## 边界

- 风险分数是结构指标（依赖计数加权），不是语义正确性判断；最终仍需人读关键路径。
- 图未覆盖未解析目标（函数指针/动态分发）与库外类型。
- 不发明命令或旗标；用法疑问查 `ccg help impact` / `ccg help changes`。
