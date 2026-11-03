# PMUX 面积最优化设计（FPGA 创新大赛 · 选题二）

在 Yosys `synth_intel`（Cyclone IV）流程内实现「识别—重构—收敛」三段式 PMUX 优化：
把「一热选通 + 整组常量比较器」的 `$pmux` 改写成以总线位为选择端的 `$mux` 平衡树，
再配合**单次调用**的统一 abc9 脚本，在 9 个公开用例上平均削减 **20.41%** 逻辑单元（LUT + `$not`），
逐例不增资源，等价性 `run_equiv` 9/9、`eqy` 9/9 通过。

## 结果一览（LC = LUT + `$not`）

| 用例 | 原版 | 优化后 | 削减率 |
|---|---|---|---|
| test1 | 288 | 251 | 12.85% |
| test2 | 25 | 15 | 40.00% |
| test3 | 13 | 11 | 15.38% |
| test4 | 36 | 33 | 8.33% |
| pmux_case01_balanced | 426 | 321 | 24.65% |
| pmux_case02_pipeline | 450 | 344 | 23.56% |
| pmux_case03_crossbar | 408 | 318 | 22.06% |
| pmux_case04_feedback | 368 | 309 | 16.03% |
| pmux_case05_nested | 413 | 327 | 20.82% |
| **平均** | — | — | **20.41%** |

- 墙钟：最差 **+2.4%**（≤5%）；峰值内存全部下降；
- DFF 增量 **0**（9 例全部不变），IO 不变；
- 等价性：`verify/run_equiv.ps1` 9/9 PASS；`verify/eqy` 9/9 PASS（test3 分区抽象不收敛，按答疑改用整设计 miter）。

## 目录

```
├─ src/                       优化源码
│  ├─ pmux_recognize.cc       Phase 1 识别与分类
│  ├─ pmux_decompose.cc       Phase 2（保留，当前未启用）
│  ├─ pmux_optimize.cc        Phase 3 冗余 mux / $reduce_or CSE / $eq 化简
│  ├─ pmux_recode.cc          新增：常量解码 $pmux → 总线索引 $mux 树
│  ├─ synth_intel.cc          接入点（pmux 注入 + 统一 abc9 脚本）
│  └─ passes_pmux_CMakeLists.txt
├─ bin/                       编译产物与依赖（yosys-opt.exe、yosys-abc.exe、DLL、share/）
├─ case/                      9 个官方用例（test1~4 + pmux_case01~05）
├─ bench/                     run_cases.ps1（面积+优化率）、measure.ps1（+时间/内存）、show_lc.ps1
├─ verify/                    run_equiv.ps1 + dffeas_sim.v；eqy/ 为官方口径配置与 9 例日志
├─ docs/
│  ├─ 技术文档.pdf / .docx    技术文档（9 页，含图、表）
│  ├─ 演示视频脚本_3分钟.md   演示分镜、台词与命令
│  ├─ 开发记录_AGENTS.md      设计/实验/踩坑记录（含脚本搜索、级联策略等）
│  └─ fig1_flow.png / fig2_recode.png
├─ check.ps1 / check.bat      一键自检（面积+优化率+等价性，-Eqy 加 eqy）
├─ README.md（本文件）
├─ 提交清单与合规说明.md      对照答疑逐条自查（含 11-03 版第 155–168 行）
└─ 演示命令.md                演示/录屏命令清单（含预期输出）
```

## 一键运行

```powershell
cd <repo>
.\check.ps1            # 面积表 + 优化率 + 等价性
.\check.ps1 -Eqy       # 再加 eqy（需 OSS CAD Suite）
```

单用例（与评测脚本一致：`read_verilog` + 空行 + `synth_intel`）：

```powershell
.\bin\yosys-opt.exe -p "read_verilog case\test1.v; synth_intel -family cycloneiv -top test1; stat"
```

## 合规要点（按 11-03 版答疑核对）

| 答疑 | 要点 | 我们的做法 |
|---|---|---|
| 第 161 问 | 0.69 大版本即可 | 0.69（`16139ea24`），原版/优化版同源构建 |
| 第 162 问 | 允许改 abc 脚本 + 新增 pass，其它修改不允许 | 只改 `synth_intel.cc` 的 abc9 `-script` + 新增 4 个 pmux pass |
| 第 163 / 168 问 | 验证环境赛事方准备 | 包内不含 OSS CAD Suite，仅提供脚本 |
| 第 166 问 3 | 不得多次调用 abc、不得调顺序 | `abc9` 单次调用；流程顺序未动 |
| 第 166 问 4 | 预热：原版跑 `pmux_case05_nested` 10 次；超 5% 两版都复核 | 见 `演示命令.md` |
| 第 166 问 6 | DFF/IO 超原版 20% 才影响评分 | DFF 增量 0 |
| 第 166 问 7 | eqy 验证脚本需提供 | `verify/eqy/` 全套（含日志与说明） |

## 许可

本仓库内容用于参赛评审；Yosys/ABC 遵循其原始 ISC 许可。
