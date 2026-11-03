# 等价性验证（eqy，官方口径）

> **结论：9/9 全部通过，脚本输出里不会出现 FAIL。**
> 8 个用例在 eqy 默认分区模式下直接 PASS；test3 的分区模式推不出来，
> `run_eqy.ps1` 会**自动回退到整设计 miter**（不分区）再证一次，成功即判 PASS。
> 依据：9月24-28日答疑·赛题二EQY第5问「切换设计模式验证是允许的，算作通过」。

官方口径（9月24-28日答疑·赛题二第54问第3点）：eqy 验证 **abc 之后的 `$lut` 网表**（完全映射成 cycloneiv 单元后 eqy 认不出功能）。

---

## 1. 结果一览

| 用例 | eqy 分区模式 | 整设计 miter | 判定 |
|---|---|---|---|
| test1 | PASS（4 分区） | — | **通过** |
| test2 | PASS | — | **通过** |
| test3 | 4/6 分区 PASS，2 分区（sr、rw）停住 | **PASS** | **通过**（见 §3） |
| test4 | PASS | — | **通过** |
| pmux_case01_balanced | PASS | — | **通过** |
| pmux_case02_pipeline | PASS | — | **通过** |
| pmux_case03_crossbar | PASS | — | **通过** |
| pmux_case04_feedback | PASS | — | **通过** |
| pmux_case05_nested | PASS | — | **通过** |

原始日志见 `results/`：`<case>.log` 为 eqy 输出；`test3_miter.log` 为整设计 miter 证明日志
（含 `Base case for induction length 1 proven` 与 `Induction step proven: SUCCESS!`）。

---

## 2. 复现方法

```bat
rem 在 perfect\verify\eqy 目录下执行；脚本会自动 call OSS CAD Suite 的 environment.bat
run_eqy.bat -Cases ..\..\case
rem 套件会自动查找（C:\oss-cad-suite、C:\tools\oss-cad-suite、<包>\..\tools\oss-cad-suite 等）；
rem 找不到时在 PowerShell 里设一次环境变量：  $env:OSS_CAD = "<解压目录>"
rem 若无 make（本机用 D:\msys64\usr\bin\make.exe），脚本会自动加入 PATH
```

`run_eqy.ps1` 对每个用例：用 `bin\yosys-opt.exe` 生成 abc 之后的 `$lut` 网表 → 写 `.eqy` 配置 →
先跑 eqy（分区模式）；若某个分区推不出来，**自动改用整设计 miter + SAT 时间归纳**再证一次；
两种方式都失败才打印 FAIL。

---

## 3. 关于 test3 的两处 "Failed to prove"（重要）

**现象**：eqy 默认按输出分区，test3 的 `sr`、`rw` 两个分区停在抽象层，日志写
`Could not prove equivalence of partition 'test3.rw' ... partitions not equivalent`。

**原因**：test3 是"移位寄存器 + 组合逻辑"混合结构，输出分区会把**跨触发器的不变式**切断；
每个分区单独看都缺一个只在整个设计里成立的不变量，于是任何策略都推不出来。
**这与是否做优化无关**——同一赛题的其他参赛队也报告了完全相同的现象（答疑 B53 问题 5）。

**依据**：答疑 **9月24-28日答疑·赛题二EQY第5问** 原文："**切换设计模式验证是允许的，算作通过**"。

**做法**：改用**整设计 miter + SAT 时间归纳**（不分区的等价证明）：

```
read_verilog test3.v
prep -top test3
memory -nomap ; memory_map ; opt -fast
design -stash gold
read_rtlil gate_test3.il              ; run_eqy.ps1 生成在 work\ 下
techmap -autoproc -map dffeas_sim.v
opt -fast
rename test3 gate
design -stash gate
design -copy-from gold -as gold test3
design -copy-from gate -as gate gate
miter -equiv -flatten -make_outputs gold gate miter
sat -verify -prove-asserts -tempinduct -show-ports miter
```

日志 `results/test3_miter.log` 显示：miter 由 `gold` 与 `gate` 构成，导入 51 个单元，
比较 `gate_sr_out`/`gold_sr_out`、`gate_gpioout`/`gold_gpioout` 等输出，
**base case 与 induction step 均证明成功**，即整设计等价。

复现：

```bat
cd work
..\..\..\bin\yosys-opt.exe -q -s ..\miter_test3.ys -l ..\results_test3_miter.log
```

---

## 4. 配置要点（踩过的坑）

- gate 必须用 `write_rtlil -selected` **只写顶层模块**，否则技术库模块会被 eqy 判为 "unmatched module"；
- gate 需要 `techmap -autoproc -map dffeas_sim.v` 把 `dffeas` 还原成通用触发器，eqy 才能比较；
- gold 侧先 `memory -nomap; memory_map; opt -fast`，否则 test4 的 case 表以 `$mem_v2` 形式存在，与 gate 不匹配；
- 运行 eqy 的环境必须 `call environment.bat`（eqy.exe 依赖套件内的 python/lib），并保证 `make` 在 PATH 上。
