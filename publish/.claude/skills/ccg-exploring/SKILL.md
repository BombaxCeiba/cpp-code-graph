---
name: ccg-exploring
description: "Use when 用户想理解代码——找符号、看某个函数/类的上下游关系、顺调用链走读陌生代码。Examples: \"这个函数在哪定义\", \"谁调用了 draw\", \"这个类的结构是什么样的\", \"帮我走读这段代码的调用关系\""
---

# ccg 代码走读剧本

用 ccg 把"这段代码怎么工作"变成一条可重复的查询链：query 定位 → context 全景 → callers/callees 展开 → trace 连线。

## 何时使用

- "X 在哪定义 / X 是什么"
- "谁调用了 X / X 调用了谁"
- "这个功能的执行流经过哪些函数"
- 接手陌生代码，需要快速建立结构认知

## 前置检查

1. `ccg stats` 确认 db 存在（退出码 1 = 还没索引，先走 `ccg-index` 技能建库）。
2. 跨目录时 `-o DIR`（产出文件夹）或 `$CCG_DB` 指向目标布局；多配置时加 `--config <名>`（唯一配置自动选中）。
3. 装了 hook 的项目 db 通常已是最新；否则对刚改过的文件先 `ccg update <file>`。

## 剧本

### 第 1 步：query 定位坐标

```
ccg query <name> [--kind K] [--file SUBSTR] [--implements] [--json]
```

- `--implements` 是反向问法："谁派生自这个接口/基类"（类型继承树，含间接层，depth 列标层级）——找插件实现类/mock/生成包装时用。

- 四层匹配 tier：exact（含全限定名）→ qualified suffix（`Canvas::draw`）→ substring（大小写敏感）→ fuzzy（不区分大小写/分隔符，编辑距离排序）。
- **query 多命中是罗列（退出码 0），永不歧义报错**——它就是用来看候选的。零命中时 fuzzy 会给 "did you mean" 建议（尽力而为，不保证）。
- 先 query 看清候选的 kind 和 file 列，再带着消歧器进单目标命令。

### 第 2 步：context 看全景

```
ccg context <sym> [--kind K] [--file SUBSTR] [--no-merge] [--json]
```

输出三段：符号块（kind/限定名/定义位置/flags 如 `virtual, pure-virtual, const`/参数/模板/成员计数）→ incoming（calls 进来的、uses 它的、extends 它的、overrides 它的）→ outgoing（它 calls/extends/uses/overrides 的）。每条边带一个代表调用点。

### 第 3 步：callers / callees 展开调用链

```
ccg callers <sym> [--depth N]     # 谁调用它（N: 1-10，默认 1）
ccg callees <sym> [--depth N]     # 它调用谁
```

- depth > 1 时输出按深度分层，行尾 `(via <上一层符号>)` 溯源；每个符号只出现在最短深度；环由 visited 集终止。
- `truncated: yes` 表示 **--depth 用尽**（扩展被截断），不是列表被截断，退出码仍是 0、计数完整。
- **零调用者不是错误而是风险信号**：消息 `no callers found (risk-relevant: this may mean the symbol is unused, or only called via unresolved targets)` 的含义是"未使用，或仅经未解析目标（函数指针/虚分发遗漏）调用"。

### 第 4 步：trace 连两点

```
ccg trace <a> <b>
```

calls 边 BFS 最短路（继承链经 extends 连通）。找到了打印逐跳 `--calls-->` 路径；不通打印 `no path ... within 10 hops`（退出码 1）——不通的含义是"调用图上没有这条链"（可能中间隔了未解析边），不是"代码有 bug"。

### 第 5 步（可选）：graph 出图

```
ccg graph --symbol <入口> --depth 2 --format dot    # 入口执行流（dot 文本；--depth 0 = 不限深度全闭包）
ccg graph --symbol <入口> --format json             # agent 消费（--json 同义）
ccg graph --modules --min-weight 3                  # 模块级总图（按定义文件聚合）
```

走读结论要可视化、或要一张"这个入口往下怎么流"的图时用；输出是 Graphviz dot（默认）或 JSON，`--output FILE` 落盘。深度 2 是可读上限，3 在仓库尺度上过大。

## 消歧技巧（单目标命令）

`context/callers/callees/impact/trace` 在 tier-1 多命中时打印候选表并退出 1（JSON 得 `candidates` 数组）。收窄三通道：

1. `--kind K`：11 种（record function method constructor destructor field variable enum enum_constant type_alias）。候选 kind 不同时最有效。
2. `--file SUBSTR`：定义路径子串。
3. 更长限定名：`shape::Circle::area` 优于 `area`。

**已知死角**：同 kind 同文件的重载（如同名函数一个头文件里两个重载）在当前版本无法用 --kind/--file 区分——改用 query 定位坐标（file:line）、换分析目标，或结合源码阅读确认。

## override 家族合并

查虚方法默认合并整个 override 家族的边（合并行带 `[family: ns::Class::method]` 标记，尾部列家族成员数）；`--no-merge` 只查单符号。非虚符号不受影响。判断"这个接口的所有实现"时用默认合并；判断"这个具体实现"时加 `--no-merge`。

## 动态派发证据（默认开启）

调用点上有 confidence ≥ 0.7 的 virtual_dispatch 证据（1.0/0.7）时，归属改用证据目标，不再按整个 override 家族过近似；没有证明目标的家族合并结果标 `[dispatch unknown · family estimate]`（含义：动态类型无法证明，按该虚方法的整个 override 家族估算，可能包含并非实际目标的实现）。有证据时标 `[dispatch 0.70]` 这类数值。0.4 只是弱推断（项目内裸名/多候选），低于默认证据阈值，不当作具体动态目标，仍按家族估算。

- `--no-dispatch`：纯静态视图（保守上界，与引入派发前的默认输出逐字节一致）。怀疑漏调用者时用它对照。
- `--dispatch-fallback`：额外显示低于阈值的 0.4/0.0 家族标记（只影响展示与状态标注，不改变归属集合）。

## 常见坑

- **表格长路径/长限定名列溢出是显示 nit**：数据完整，`--json` 无此问题，不要误报数据损坏。
- **fuzzy 建议不保证出现**：拼写差太远时零建议零命中，换更准的名字或 query 子串。
- **trace 的消歧器同时过滤两个端点**：trace 也接受 `--kind/--file`（help 是权威），但过滤器同时套在两个端点上——某个端点自身的 kind/定义文件不匹配时就报 `no symbol matching`（哪怕该名字本来唯一）。两端点类型/文件不一致时应改用更长限定名（`ns::Circle::area`）消歧，不用 `--kind/--file`。
- 探索大库时善用 `--depth 2` 逐层展开，避免一次 `--depth 10` 产生海量输出。

## Checklist

- [ ] `ccg stats` 通过
- [ ] query 先行，看清候选再消歧
- [ ] 需要给用户看图时 `ccg graph --symbol <入口>` 导出 dot
- [ ] context 全景 → callers/callees 分层展开 → trace 连线
- [ ] 零调用者解读为风险信号并如实转述
- [ ] 结论附符号的 file:line 坐标

## 边界

- 图只覆盖被索引的 TU 及其可解析目标；函数指针/动态分发可能表现为"未解析边"。
- ccg 不读注释/文档语义，只看结构；语义推断要回到源码。
- 不发明命令或旗标；用法疑问查 `ccg help <命令>`。
