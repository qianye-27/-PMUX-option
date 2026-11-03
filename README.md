# 多路选择器（PMUX）面积最优化设计 —— 代码与工具包

本包为「赛题二：Yosys PMUX 识别与优化」的作品交付物，解压后可直接运行，**不含任何绝对路径**。

> 说明：本包为**代码与工具包**；**技术文档单独提交**（`技术文档.pdf`），不在本包内。

## 合规要点（按 11-03 版答疑核对）

- **允许**：修改 abc 脚本、新增优化 pass（第 162 问）；我们只改了 `synth_intel.cc` 里 abc9 的 `-script` 与新增 4 个 pmux pass。
- **未做**：改其它 pass 源码 / 调换 pass 顺序 / 多次调用 abc（第 166 问 3）——`abc9` 仍是单次调用。
- **验证环境**：不随包提供 OSS CAD Suite（第 163 问），只提供 eqy 脚本与状态模型（第 166 问 7）。
- **资源**：DFF 增量 0、IO 不变（第 166 问 6 的 20% 阈值）。
- **热身/复核口径**（第 166 问 4）：预热用原版跑 `pmux_case05_nested` 10 次；超 5% 时两版各跑 5 次求平均后再算比例。

## 解压说明（重要）

压缩包**不含顶层文件夹**，请这样解压：

- 方式一：右键 `code.rar` →「全部解压缩」，系统会生成名为 `code` 的文件夹，内容即为包根（推荐）；
- 方式二：解压到任意空目录，**该目录就是包根**。

若解压后看到 `code\code\`（双层同名目录），说明是解压到了已存在的 `code` 文件夹里，
把内层的内容上移一层即可；此时在**外层目录**运行 `bench\show_lc.ps1` 会提示"找不到脚本"。

## 目录结构

```
code/
├─ README.md                     本文件
├─ 提交清单与合规说明.md         对照最新答疑的逐条合规自查
├─ 实验结果命令.md                   演示/录屏用的命令清单（含预期输出）
├─ MANIFEST.txt                  SHA256 清单
├─ src/                          优化源码（相对 yosys 0.69 源码树的位置见下）
│  ├─ pmux_recognize.cc          Phase 1：PMUX 识别与分类
│  ├─ pmux_decompose.cc          Phase 2：按分类展开（本次未启用，保留）
│  ├─ pmux_optimize.cc           Phase 3：冗余 mux 消除 / $reduce_or CSE / $eq 化简
│  ├─ pmux_recode.cc             新增：常量解码 $pmux → 总线索引 $mux 树
│  ├─ synth_intel.cc             唯一接入点（pmux 注入 + 统一 abc9 脚本）
│  └─ passes_pmux_CMakeLists.txt  passes/pmux 的构建注册文件
├─ bin/                          编译产物与运行依赖（自带 DLL 与 share/，解压即用）
│  ├─ yosys-opt.exe              优化版 yosys（Windows x64）
│  ├─ yosys-abc.exe              随 yosys 0.69 的 ABC（未修改）
│  ├─ *.dll                      tcl86 / readline / ffi / zlib / termcap / winpthread 等
│  └─ share/                     运行期数据（intel 器件库、abc9 模板等）
├─ bench/
│  ├─ run_cases.ps1              跑 case/ 下全部用例，输出 LUT/NOT/LC/DFF
│  ├─ measure.ps1                面积 + 墙钟 + 峰值内存一键对比（按官方口径）
│  └─ show_lc.ps1                单个用例一行输出 LUT / NOT / LC / DFF
├─ case/                         9 个官方用例（4 旧 + 5 新），可直接跑
└─ verify/                       等价性验证材料
   ├─ run_equiv.ps1              一键等价性检查（相对路径，自动遍历 cases/）
   ├─ dffeas_sim.v               dffeas 最小行为模型（equiv -map 用）
   ├─ cases/                     把待验 .v 放进来即可
   ├─ eqy/                       eqy 官方口径验证
   │  ├─ run_eqy.bat             一键入口（自动 call OSS CAD Suite 环境）
   │  ├─ run_eqy.ps1             生成 $lut 网表 → 写 eqy 配置 → 调 eqy
   │  ├─ miter_test3.ys          test3 的整设计 miter 证明脚本
   │  └─ results/                9 例 eqy 日志（8 PASS + test3 miter PASS）
```

## 快速开始（含用例，可直接复制运行）

> 优化率一列需要原版 yosys：`check.ps1` 会自动找 `..\yosys-baseline\yosys-baseline.exe`；
> 不想依赖外部目录时，把原版复制成 `bin\yosys-orig.exe` 即可（没有原版时只打印 LC，其它检查不受影响）。


> 录屏/演示时用的那一套命令单独整理在 `实验结果命令.md`，可直接照敲。

包内 `case\` 已放好 9 个官方案例（`test1~4` + `pmux_case01~05`），下面命令在**包根目录**执行：

```powershell
# 一键自检（面积表 + 等价性；加 -Eqy 连 eqy 一起跑）
.\check.ps1

# 0) 看版本
.\bin\yosys-opt.exe -V

# 1) 跑全部 9 个用例（只用优化版，逐例打印 LUT / NOT / LC / DFF）
.\bench\run_cases.ps1 -Cases .\case

# 2) 单个用例（官方脚本形式：read_verilog + 空行 + synth_intel）
.\bin\yosys-opt.exe -p "read_verilog case\test1.v; synth_intel -family cycloneiv -top test1; stat"

# 3) 与原版对比（面积 + 墙钟 + 峰值内存，需先准备官方原版 yosys）
.\bench\measure.ps1 -Baseline <原版 yosys.exe> -Optimized .\bin\yosys-opt.exe -Cases .\case -Reps 5 -Warmup 3

# 4) 等价性（RTL vs abc 之后的 $lut 网表）
.\verify\run_equiv.ps1 -Cases .\case

# 5) eqy 官方口径（自动查找 OSS CAD Suite）
cd .\verify\eqy
.\run_eqy.bat -Cases ..\..\case
cd ..\..
```

预期结果（`run_cases.ps1` 输出）：

```
case                        LUT  NOT     LC   DFF
pmux_case01_balanced        321    0    321    12
pmux_case02_pipeline        344    0    344    24
pmux_case03_crossbar        318    0    318    10
pmux_case04_feedback        308    1    309    16
pmux_case05_nested          327    0    327    12
test1                       246    5    251   222
test2                        15    0     15     0
test3                        10    1     11    17
test4                        32    1     33    10
```

## 运行方式（相对路径）

> 说明：`bench\` 与 `verify\` 下的脚本会在包内自动创建 `_tmp\` 作为临时目录
> （yosys 的 `-l` 日志与 abc9 临时文件），这样即使包被解压到受限目录也能运行；
> 该目录可以随时删除，不属于提交内容。



```powershell
cd code\bin
.\yosys-opt.exe -p "read_verilog ..\..\test\test1.v; hierarchy -auto-top; synth_intel -family cycloneiv; stat"
```

`bin\` 内已带 `share\`，yosys 会按可执行文件所在目录自动查找，无需设置任何环境变量。
唯一要求：测试用例（官方 `test/*.v`）由评测方提供，路径按实际位置传入。

## 重新构建（可选）

在 yosys 0.69（commit `143eb14f`）源码树上：

1. 用 `src/pmux_*.cc` 覆盖 `passes/pmux/`（`pmux_recode.cc` 为新增文件）；
2. 用 `src/passes_pmux_CMakeLists.txt` 覆盖 `passes/pmux/CMakeLists.txt`；
3. 用 `src/synth_intel.cc` 覆盖 `techlibs/intel/synth_intel.cc`；
4. `cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXE_LINKER_FLAGS="-static"` 后 `cmake --build build --target yosys -j`。

## 技术要点

1. **识别（`pmux_recognize`）**：遍历网表，把 `$pmux` 的选择判据按「常量比较 / 位宽不等 / 含变量位」分类并打 `pmux_*` 属性。
2. **重构（`pmux_recode`，新增，核心）**：把「选择端全部来自同一总线常量比较」的 `$pmux` 改写为**以总线位为选择端的 `$mux` 平衡树**：整组比较器消失、解码共享。
   - 支持 `$eq(bus,K)`、`$logic_not(bus)`、`$reduce_and(bus)` 三种判据形态；
   - 支持**部分常量**判据（如 `{3'000, w}`），按 `$pmux` 的下标优先语义逐取值折叠；
   - 两条通用门限：覆盖率 `2^W ≥ 50%·N`（稀疏选择不改写，避免树比比较器更贵）、依赖链深 `< 2`（避免把 mux 链变成深树）；
   - 构造性等价，无启发式猜测。
3. **收敛（`pmux_optimize`）**：冗余 `$mux(A==B)` 消除、跨单元 `$reduce_or` 公共子式合并、位宽安全的 `$eq` 恒等式化简（`eq(a,0)→~|a`、同位宽或双符号 `eq(a,全1)→&a`）。
4. **映射（`synth_intel.cc`）**：按答疑 C130「统一脚本」要求，所有设计走同一条 abc9 命令：

   ```
   abc9 -lut 4 -W 300 -D 3000 -script "+&scorr; &sweep; &if {W} {D}; &save; &st; &syn2;
     &if {W} {D} {R} -a -v; &save; &load; &st; &dch -f; &if {W} {D} {R} -a -v;
     &save; &load; &st; &syn3; &if {W} {D} {R} -a -v;"
   ```

## 指标（本地 9 个官方用例，LC = LUT + `$not`）

| 用例 | 原版 | 优化版 | 削减 |
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

- 9 例算术平均 **R = 20.41%**；逐例均无资源增加；DFF 全部不变。
- 墙钟（交错 7 次中位数）：−5.9% ~ **+2.4%**，全部 ≤5%；峰值内存全部下降（−1.4% ~ −8.5%）。
- 等价性：9/9 通过（RTL ↔ ABC 后 `$lut` 网表，时间归纳）。

## 等价性验证

```powershell
cd code\verify
.\run_equiv.ps1 -Cases "..\..\test\test1.v" ...
```

脚本用相对路径调用 `..\bin\yosys-opt.exe`，内部流程与官方一致：`read_verilog → prep → equiv_opt -assert -async2sync -map dffeas_sim.v synth_intel -run begin:map_cells`。
`dffeas_sim.v` 是为 `dffeas` 写的最小行为模型（异步低电平清零 + 时钟使能 + 按 `power_up` 置初值），用官方 `cells_sim.v` 的模型会推出 latch，SAT 无法处理。

**eqy 原生验证（官方口径 9月24-28日答疑·赛题二第54问第3点）**：verify/eqy/ 内含 eqy 配置生成器与全部日志，9 个用例全部通过——
8 个在 eqy 默认分区模式下 PASS；test3 因分区切断跨触发器不变式，按9月24-28日答疑·赛题二EQY第5问 用整设计 miter 证明通过。
详见 verify/eqy/README.md 与 verify/eqy/results/SUMMARY.txt。
