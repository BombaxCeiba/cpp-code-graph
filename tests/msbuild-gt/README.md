# MSBuild 金标六维对账门禁

对入库语料（仓内中性夹具的本体产物）执行「工程求值金标 ↔ ccg
记录通道产物」的六维对账（TU 覆盖 / defines / include / PCH /
工具集 / 杂项），实测数字与入库基线逐值锁死，是 MSBuild 采集链
（含后续求值快车道改造）后续任何改动的回归验收仪器。门禁本体
是纯数据对账（Python 脚本，不运行 MSBuild、不依赖工具链在席）。

## 目录内容

| 文件 | 职责 |
|---|---|
| `gt_gate.py` | 门禁脚本：金标普查 + 六维对账 + 基线比对 + 指纹分罪 |
| `corpus/groundtruth.json` | 金标语料（四解决方案的工程求值产物合并，全部路径洗涤为 `X:/ccg-fake` 占位根） |
| `corpus/compile_commands.json` | ccg 记录通道产物语料（cdb，同洗涤） |
| `corpus/pch.json` | PCH 旁车语料（内嵌 `cdb_sha256` 对合并 cdb 重算后参与交叉验证） |
| `baseline-six-dims.json` | 六维基线（锚定数字 + 模块开关形态锁定） |
| `fingerprints.json` | 双指纹：基线内容指纹 + 语料三件内容指纹 + 金标引擎指纹 |
| `../msbuild-gt.cmake` | ctest 壳（注册名 `ccg_msbuild_gt`） |

## 语料来源与再生成（夹具只收文本政策）

语料本体 = 仓库自有中性夹具 `tests/fixtures/msbuild/` 的四个解决
方案，全部取 Debug|x64 配置：

| 解决方案 | 工程 | 六维覆盖面 |
|---|---|---|
| `DemoSln.sln` | mathlib（含非 ASCII 目录 TU）+ msbdemo | TU 覆盖 / defines / include 主力 |
| `mixed/mixed.sln` | projA（v145，钉 14.51）+ projB（v143，钉 14.44） | 混合工具集换算（toolset 维度） |
| `pchp/pchp.sln` | pchp（pch.cpp /Yc + main.cpp /Yu） | PCH 生产消费对账（pch 维度） |
| `modmsb/modmsb.sln` | g16mod（mod.ixx + use.cpp） | 模块双趟形态 / 开关成对带值（misc_invariants、module_switches 维度） |

再生成流程（语料漂移须人工重审基线后重锁，见下）：

1. 把四组夹具复制到工作目录；mixed 的两工程把钉死的大版本号
   （14.51/14.44）替换为该机已安装的完整工具集版本（大版本
   GLOB 动态探测——与 `msbuild-e2e.cmake` 混合工具集闸门同款）；
2. 记录臂：`ccg msbuild <sln> --config Debug --platform x64
   --export-only`（VsDevCmd 环境下），取各 `.ccg/Debug-x64/` 的
   `compile_commands.json` 与 `pch.json`；
3. 金标臂：对同一工作副本逐解决方案做工程求值（Rider 语义的
   全局属性注入 + 官方 Microsoft.Build.Evaluation 只读求值，
   逐工程枚举 ClCompile 条目与最终 metadata；VCTargetsPath
   按「含 Default/Common props」降序存在性探测、尾反斜杠硬约束，
   VS 根经 vswhere 运行期探测），四份合并；
4. 洗涤：全部绝对路径按前缀映射成 `X:/ccg-fake` 占位根（夹具
   工作根 / VS 安装根 / Windows Kits / 求值引擎目录），路径层级
   形状不变；合并 cdb 落盘后重算 `cdb_sha256` 回填 pch.json；
5. 跑 `gt_gate.py --report` 取实测数字，更新基线并同批重锁指纹。

## 退出码协议（漂移分罪）

| 码 | 含义 | 处置 |
|---|---|---|
| 0 | 全绿 | — |
| 1 | 输入/协议错误（含基线与指纹文件失锁） | 修输入 |
| 3 | 语料指纹全等但六维数字漂移 | ccg 回归嫌疑（数字变好也红）：人工重审后重锁 |
| 4 | 语料或引擎指纹漂移 | 语料已重采/金标已重生：重审基线后重锁，不算 ccg 回归 |

## 模块开关形态（module_switches 维度）的锁定与 XFAIL 机构

`/ifcOutput` 等六项带值模块开关（ifcOutput/scanDependencies/
sourceDependencies/ifcSearchDir/reference/exportHeader）在翻译层
成对保留进 cdb（带值开关成对保留修复；名单单点 `cg.util.msvcswitch`，消费端
喂引擎前成对剥除，引擎 argv 逐字节不变——守卫单测钉死）。语料
重录后 2/2 条 .ixx 条目均为两段式带值，基线 `module_switches`
节按实测值直接锁死（`xfail: false`），裸拼写回潮即门禁失败。
脚本内的 XFAIL 判读机构（缺陷形态在=绿、修复形态出现=故意翻红
提示升级基线、偏离两种形态=失败）保留：基线其他维度将来按已知
缺陷登记时复用同一机构（登记处即本文件、ctest 壳注释与基线节）。

## 求值通道的用法（快车道验收）

A1 求值快车道落地后，本门禁即其验收仪器：把 `--corpus` 指向
「同一金标 + 求值通道产出的新 cdb/pch」目录跑同样六维（记录通
道基线数字对求值形态的口径差异——.ixx 无扫描趟、条目数少一趟
——须按设计稿通道差异清单显式投影后对账）。

## 重锁步骤（任何基线/语料变更后）

以 git 可审计为原则，基线数字与指纹必须同批变更：

1. 改 `baseline-six-dims.json`（如 XFAIL 翻 ASSERT、语料重采后的
   新锚定数字）；
2. 重算其 SHA-256 写回 `fingerprints.json` 的 `baseline_sha256`；
   语料重采则同时更新三件语料指纹与金标引擎指纹（引擎版本
   取自新金标 `meta`；金标不携带生成时间戳，生成时点以 git
   历史为准）；
3. 提交说明中写明变更理由（六维哪几维为何重锚）。
