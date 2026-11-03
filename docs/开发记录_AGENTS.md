# AGENTS.md — Yosys PMUX 识别与优化：任务说明与工作规范

> 本文件面向在本仓库中工作的编码代理（人或 AI）。任何改动都必须先读本文件。
> 目标只有一个：**在不牺牲面积、DFF 数量、其它单元和 CPU/内存开销的前提下，把 PMUX 的识别与优化做到平均 20% 以上的逻辑单元削减。**

---

## 1. 任务

改进 Yosys 对 PMUX（Priority MUX / `$pmux` / 优先级 `$mux` 链）的**识别**与**优化**：

- 识别：更准地判定 `case` / 优先级链 / one-hot 选择结构，正确分类（exclusive / priority / one_hot / sparse / wide / nested …）。
- 优化：在映射到 LUT 之前，把可合并、可共享、可消除的 mux 结构化简掉，减少最终 LUT 数量。
- 所有优化必须**语义等价**（优先用形式化等价检查兜底，见 §5）。

实现入口与现状：

| 文件 | 作用 |
|---|---|
| `yosys-src/passes/pmux/pmux_recognize.cc` | Phase 1：识别、分类、标注属性（`-annotate`） |
| `yosys-src/passes/pmux/pmux_decompose.cc` | Phase 2：按分类展开为 `$mux` 链 / 平衡树（**当前被注释，未启用**） |
| `yosys-src/passes/pmux/pmux_optimize.cc` | Phase 3：冗余 mux 消除、`$reduce_or` CSE、`$eq` 恒等式 |
| `yosys-src/techlibs/intel/synth_intel.cc:216` | 唯一的接入点（`pmux_opt` 标签） |
| `pmux_work/` | 上述三个 pass 的**已跟踪副本**，仓库只提交这里 |

---

## 2. 评分规则

### 2.1 逻辑单元与面积得分

**逻辑单元（Logic Cell, LC） = LUT 数 + 取反单元（`$not`）数。取反也算一个逻辑单元。**

对第 i 个用例：

```
LC_before,i  = 改动前综合结果的 LUT + $not 数量
LC_after,i   = 改动后综合结果的 LUT + $not 数量
r_i          = (LC_before,i − LC_after,i) / LC_before,i        # 单用例优化率
面积得分率 R  = (1/N) · Σ r_i                                   # N = 用例数，算术平均
```

- **按用例求平均，不是总量相除**。大用例不会因为体量大而主导分数。
- 平均优化率 **R = 20% 即为满分**（0% → 0 分，中间按线性给分）。
- 参考目标（当前 4 个用例、`$not` 暂按 0 估算，需按 §3 口径复测）：

| 用例 | LC 现状 | 20% 目标 |
|---|---|---|
| test1 | 286 | ≤ 229 |
| test2 | 25 | ≤ 20 |
| test3 | 12 | ≤ 10 |
| test4 | 35 | ≤ 28 |

### 2.2 效率得分（相对原版 Yosys）

对**每个用例**单独计算相对原版 Yosys（`yosys-baseline`，即上游 `143eb14f` 未经修改的构建）的面积偏差：

```
o_i = (LC_optimized,i − LC_upstream,i) / LC_upstream,i
```

| 条件 | 得分 |
|---|---|
| `o_i ≤ 5%` | 效率满分 |
| `5% < o_i ≤ 15%` | 按线性递减（区间内插值；**此段规则待评测方确认**） |
| `o_i > 15%` | 效率不得分 |

- **逐用例判定**，取最差用例作为该维度得分；不允许"平均下来还好"。
- CPU 时间与峰值内存相对原版的开销同样 **≤ 5%**（见 §3.2），否则效率维度按不合格处理。

### 2.3 硬性约束（任一不满足即视为失败）

1. **DFF 不得增加**：`DFF_after,i ≤ DFF_before,i`（逐用例；建议同时不高于原版）。
2. **其它单元不得增加**：IO、`$dff`/LUT/`$not` 之外的单元，`Other_after,i ≤ Other_before,i`；确有必要增加时必须单独说明理由并取得确认。
3. **不得用 `$mux`/`$pmux` 的"消失"来伪造收益**——评分只认最终映射后的 LUT + `$not`。
4. 语义等价：综合前后功能不变（§5 的等价检查必须通过）。

---

## 3. 度量方法（必须用同一套脚本，避免口径漂移）

### 3.1 面积口径

```sh
# 统一综合命令（不要用不同 -family / -top / 选项做对比）
yosys-opt/yosys-opt.exe -p "read_verilog test/test1.v; \
    hierarchy -top test1; synth_intel -family cycloneiv -top test1; \
    select -count t:\$lut; select -count t:\$not; select -count t:*DFF*; stat"
```

- `LC = ($lut 数) + ($not 数)`，在**完整综合结束后**统计（不要在中途 pass 之后统计，`opt_clean` 可能把反相器折进 LUT）。
- 统计脚本输出 CSV：`case,lc_before,lc_after,dff,other,cell_total`，放入 `bench/results/` 并按提交累加，便于回看趋势。
- 每个数据点至少跑 2 次取一致值（Yosys 综合是确定性的，结果应完全一致；不一致说明有随机化或并发问题）。

### 3.2 CPU / 内存口径

- 冷启动、同机、同一命令；先跑 1 次 warmup 丢弃，再跑 **≥5 次取中位数**。
- 墙钟时间用 PowerShell：

  ```powershell
  $t = Measure-Command { & .\yosys-opt\yosys-opt.exe -p "..." }
  $t.TotalMilliseconds
  ```

- 峰值内存用采样（Windows 没有 `getrusage`）：

  ```powershell
  $p = Start-Process -FilePath .\yosys-opt\yosys-opt.exe -ArgumentList '-p','...' -PassThru
  $peak = 0
  while (-not $p.HasExited) { $p.Refresh(); $peak = [Math]::Max($peak, $p.PeakWorkingSet64); Start-Sleep -Milliseconds 20 }
  $peak / 1MB
  ```

- 报告 `time_after/time_ref − 1` 与 `mem_after/mem_ref − 1`，任一超过 5% 即需优化掉。

---

## 4. 代码与仓库约定

### 4.1 目录

- 源码：`yosys-src/`（**本地存在但被 `.gitignore` 白名单排除**，见 4.3）。
- 已跟踪：`yosys-baseline/`、`yosys-opt/`、`pmux_work/`、`README.md`、`.gitignore`。
- 构建产物：`yosys-src/build/`，永不入库。
- 日志/基准：`yosys-logs/`（历史）、新建 `bench/` 放脚本与结果 CSV。

### 4.2 Pass 编写规范

- 新 pass 必须注册到 `yosys-src/CMakeLists.txt`（参照 `pmux` 三个文件的写法），否则不会编译。
- 会修改网表的 pass 必须提供 `-mod` 开关，**默认只读**（只识别/报告），避免污染综合流程。
- 属性统一 `pmux_*` 前缀，写入用 `setattr -mod`；下游 pass 只认这些属性。
- 日志用 `log()`，格式参照现有 pass；错误用 `log_error()`；不要 `printf`。
- 每个 pass 的选项都要在 `help` 里可查（`Pass` 构造里的 `help` 文本）。
- `pmux_fsm_recode.cc` 目前未编译：要么接进 CMake 并测试，要么删除，不要留着悬空。

### 4.3 源码与 `pmux_work/` 的同步（重要）

仓库白名单只放行 `pmux_work/`、两套二进制目录。**修改 `yosys-src/passes/pmux/*.cc` 或接入点 `yosys-src/techlibs/intel/synth_intel.cc` 后，必须同步复制到 `pmux_work/` 并一起提交**；改了 CMake 时，需先调整 `.gitignore` 放行相应文件，再提交。同步命令：

```sh
cp yosys-src/passes/pmux/*.cc pmux_work/
cp yosys-src/techlibs/intel/synth_intel.cc pmux_work/
```

`pmux_work/synth_intel.cc` 是**接入点快照**，不是构建输入（构建仍用 `yosys-src/techlibs/intel/synth_intel.cc`）。它存在的意义是：`yosys-src/` 整体不入库，而 `synth_intel.cc:216` 附近的 pmux 注入和 `:276` 附近的 abc9 改动恰好是最容易被忘掉的两处差异，入库一份快照才能做归因和回滚。

---

## 5. 测试要求（每次改动都必须做）

**每次改动后，先写/更新测试，再交付；所有测试与验证必须在交付前全部通过。**

1. **单元/回归测试**：在 `yosys-src/tests/pmux/` 下新增或更新 `.ys` 脚本 + 期望输出（识别数量、分类、日志关键行）。可用 `yosys -s script.ys` 跑，输出存 `golden/`，用 `diff` 比对。
2. **四个基准用例**：`test1~4.v` 必须全部重跑，记录 LC/DFF/其它单元，逐项对照 §2。
3. **等价性检查**（推荐，至少对改动涉及的结构做）：

   ```sh
   yosys -p "read_verilog test/test1.v -formal; prep -top test1; \
       miter -equiv -flatten -make_outputs gold gate miter; \
       sat -verify -prove-asserts -show-ports miter"
   ```

   或对改动前后的网表做 `equiv_make/equiv_simple/equiv_induct/equiv_status`。
4. **既有测试**：改动 pass 后至少重跑 `yosys-src/tests/` 下与该 pass 相关的子目录（如 `various`、`techmap`），确认无回归。
5. 任何测试失败 → 修到全绿再提交；不允许 `--force`、跳过、或注释掉断言。

---

## 6. Git 提交规范

- **每次逻辑校验通过的改动，立即创建一个 commit**，保证可回滚、可 bisect。不要积攒多个不相关改动。
- 提交信息格式：

  ```
  type(scope): 摘要（≤50 字）

  - 改了什么、为什么
  - 面积：test1 286→258、test2 25→25 …（LC 口径）
  - 平均优化率：+x.x%（逐用例：a/b/c/d）
  - 测试：tests/pmux 12/12、test1~4 等价检查通过
  ```

  `type` ∈ `feat | fix | perf | test | docs | chore`；`scope` ∈ `pmux-recognize | pmux-decompose | pmux-optimize | bench | repo`。
- 一次提交一个主题；**不要把 `*.exe` / `*.dll` 的更新和源码改动混在一起**。
- 二进制目录已入库，每重建一次 `yosys-opt/yosys-opt.exe` 就会多一个 ~45 MB 的 blob。**只在里程碑（确认收益、要对外发布）时单独提交二进制**；日常迭代只提交 `pmux_work/` 与测试/脚本。
- 提交前 `git status` 必须干净可解释；不要提交 `build/`、临时日志、`.remote_probe/` 之类。

---

## 7. 交付前检查清单

- [ ] 每个用例 `LC_after` 已下降，且逐用例计算了 `r_i` 与平均 `R`
- [ ] `R ≥ 20%`（否则继续优化）
- [ ] 每个用例 `o_i ≤ 5%`（相对原版），且无用例 > 15%
- [ ] `DFF_after ≤ DFF_before`，其它单元未增加
- [ ] CPU/内存相对原版 ≤ 5%
- [ ] `tests/pmux` 全绿，`test1~4` 全部重跑，等价性检查通过
- [ ] `pmux_work/` 与 `yosys-src/passes/pmux/` 内容一致
- [ ] 已创建语义清晰的 commit，提交信息含基准数据

---

## 8. 已知陷阱（踩过的坑）

1. **abc9 脚本被改过，收益无法归因**：`yosys-src/techlibs/intel/synth_intel.cc:276` 已把上游的 `abc9 -lut 4 -W 300` 换成自定义 `-D 3000 -script "…&scorr; &sweep; &st; &syn2; &dch; &mfs; …"`。test2/test4 在 PMUX 完全没生效（`No optimizations applied`、0 个 PMUX）的情况下 LUT 仍然 25→15、35→26，说明**现有收益主要来自 abc9 改动**。做 PMUX 归因实验时，必须构造对照：仅 abc9 改动版 / 仅 PMUX 版 / 两者都有。

   **2026-10-24 补充：自定义脚本只对小设计有正收益。** 把 `-D 3000 -script "…"` 与 ABC 默认脚本（只给 `-D 3000`）在额外设计上对照，默认脚本全面更好：`case_large` 621 vs 757 LC（−18%）/ 1972 vs 2231 ms，`cam` 623 vs 639（−2.5%）/ 2537 vs 3111 ms，`multiplier` 26 vs 33（−21%）/ 276 vs 349 ms，`aes_kexp128` 16 vs 16 持平，`arbiter` 27 vs 28。于是在 `synth_intel.cc` 里按规模分流：**进入 map_luts 时单元数 ≥ 500 用 ABC 默认脚本，< 500 用自定义脚本**。test1 在该点有 1059 个单元（走默认脚本，预计 LC 260→261、墙钟 −50 ms 左右），test2/3/4 分别是 20/73/83，仍走自定义脚本，收益不变。

   **2026-10-24 补充二：abc 完全一致时 PMUX 只有 1 个百分点。** 把两臂 abc9 参数都强制成上游 `abc9 -lut 4 -W 300`，用同一流程（`synth_intel -run begin:map_luts` → abc9 → `-run map_cells:`，即 `out/ys/eq1..4.ys` + `out/eq.ps1`）对照，结果是 test1 288→285 LC（LUT 286→283，−1.04%）、test2 25→25、test3 13→13、test4 36→36；**R 仅 0.26%**。墙钟同时全部落在 ±1.7% 内。所以「22.5%」里 PMUX 两个 pass 自己只占约 1 个百分点，其余来自 abc 脚本，对外表述必须把这两部分分开说。
2. **`pmux_decompose` 从未跑过**：`synth_intel.cc` 里是注释状态，历史日志中没有任何 `PMUX_DECOMPOSE` 输出。启用它等于改变综合流程，必须重新建立基线。
3. **`$not` 会被 `opt_clean` 吸收**：中途统计 `$not` 会低估。统一在 `synth_intel` 结束、`stat` 之后统计。
4. **别把 `$mux` 当成绩效**：评分只认 LUT + `$not`。把 `$pmux` 拆成 `$mux` 链如果最后 LUT 没降，就是负收益（还可能增加 DFF/其它单元）。
5. **Windows 日志是 UTF-16LE**：历史 `warmup_*`/`base_time_*` 文件需 `iconv -f UTF-16LE -t UTF-8` 才能 grep；新脚本统一写 UTF-8。
6. **计时不要用 Yosys 自带的 `Time spent`**：历史日志里被截断成 `0 sec`，必须外部计时。
7. **git 网络操作在 DSH 沙箱里会被拒**（管道创建失败）：需要 `danger-full-access` 审批；`git gc` 同理。本地 `git add/commit/status` 不需要。
8. **`$eq` 化简必须检查位宽扩展（已修复）**：`pmux_optimize` 的 [3.3] 原来把 `eq(a, 全1常量)` 一律改成 `&a`，但 `$eq` 允许两侧位宽不同（实测 `A_WIDTH=4`、`B=2'b11`，B 会零扩展成 `4'b0011`）。窄的全 1 常量并不等于“全 1”，这个误判让 test1 少了 28 个 LUT、等价性直接挂掉。修法：只有 `cval.size() == a_sig.size()`（零扩展不补位）或两侧都声明为有符号（符号扩展补 1）时才允许 `&a`；全 0 常量可无条件化简（零扩展、符号扩展都还是 0）。**教训：任何面积数据必须先过等价性检查再上报。**
9. **`eq(a,c) -> ~|(a^c)` 一般形式没有收益、且会伤大设计（已实测后放弃）**：把常量比较统一改写成 `$xor + $reduce_or + $not`，四个基准用例 LC 一个都没降（288/15/11/27），但 `case_large` 从 **625 LUT 暴涨到 1801 LUT**（等价性是通过的，纯粹是 ABC 无法再还原比较器/共享结构）。结论：只保留 `eq(a,0) -> ~|a` 与位宽安全的 `eq(a,all1) -> &a` 这两个特例，不要做一般形式替换。
10. **abc 脚本“按规模分三档”试过，因 arbiter 面积回归而放弃**：把 40~499 单元的设计从完整自定义脚本换成去掉收尾 synch2 段的裁剪版，test3/test4 的墙钟确实降了（test3 从 +4.28% 到 +0.81%），但 `arbiter` 从 26 LUT 涨到 29 LUT（+11.5%），违反“任何用例不得增加资源”。结论：<500 单元统一用完整自定义脚本；时间余量靠 abc 参数（-D 2500）而不是裁剪脚本。
11. **`$mux` 的 A/B 极性极易写反（`pmux_recode` 踩过）**：RTLIL 里 `$mux` 是 **Y = S ? B : A**（A 是 S=0 分支），而 `Module::addMux(name, sig_a, sig_b, sig_s, sig_y)` 的形参顺序是 a,b。第一版写成 `addMux(hi, lo, S)`，结果 value 1 和 2 的叶子互换——`equiv_opt` 能抓出来，但单看 LUT 数只差一点点，很容易被当成噪声放过。教训：任何改写 mux 结构的 pass，都必须先用**逐输入向量比对**（`sat -set ... -show`）在最小用例上确认语义，再上大设计。

---

## 9. 当前基线（2026-10-27 实测，LC = LUT + `$not`）

口径：`yosys-{baseline,opt}/yosys-{baseline,opt}.exe -p "read_verilog …; hierarchy -auto-top; synth_intel -family cycloneiv; stat"`。`-family cycloneiv` 下 LUT 单元名是 `cycloneiv_lcell_comb`。

**abc9 统一脚本（10-26 答疑 C130 要求"统一脚本、不得对其他用例起副作用"）**：

```
abc9 -lut 4 -W 300 -D 3000 -script "+&scorr; &sweep; &if {W} {D}; &save; &st; &syn2;
  &if {W} {D} {R} -a -v; &save; &load; &st; &dch -f; &if {W} {D} {R} -a -v;
  &save; &load; &st; &syn3; &if {W} {D} {R} -a -v;"
```

所有设计走同一条命令，脚本随源码提交在 `synth_intel.cc`。

### 9.1 官方 9 个用例（2026-10-27 晚，含 `&syn3`）

| 用例 | 结构 | 原版 LC | opt LC | r_i | DFF |
|---|---|---|---|---|---|
| test1 | 16 状态 FSM + 16×12bit 数据通路 | 288 | 251 | 12.85% | 222 |
| test2 | `case(1'b1)` 4 路优先级 | 25 | 15 | 40.00% | 0 |
| test3 | 74HC595 类移位 + `case(sr)` | 13 | 11 | 15.38% | 17 |
| test4 | 5→6bit 译码 | 36 | 33 | 8.33% | 10 |
| pmux_case01_balanced | 4 路 mux 平衡树 | 426 | 321 | 24.65% | 12 |
| pmux_case02_pipeline | 多级流水 mux | 450 | 344 | 23.56% | 24 |
| pmux_case03_crossbar | 10 个 one_hot PMUX 交叉开关 | 408 | 318 | 22.06% | 10 |
| pmux_case04_feedback | 带反馈的 4 级 mux 链 | 368 | 309 | 16.03% | 16 |
| pmux_case05_nested | 嵌套 mux | 413 | 327 | 20.82% | 12 |

- **9 例算术平均 R = 20.41%**（首破 20%）；逐例不增加；DFF 全部不变。
- **等价性**：9/9 通过。
- **墙钟**（7 次交错中位数）：−4.4% / −0.8% / −5.9% / −0.7% / **+2.4%** / −6.6% / +1.8% / −0.2% / +1.2%，全部 ≤5%。
- **峰值内存**（5 次中位数）：−5.03% / −6.81% / −6.09% / −1.44% / −8.52% / −4.61% / −3.37% / −1.78% / −5.56%，全部下降。
- **额外设计**：case_large 625→**477（−23.7%）**、cam 623→511（−18.0%）、arbiter 28→26（−7.1%）、aes 16→16；**multiplier 33→36（+9.1%，唯一回归）**。

**`&syn3` 是怎么找到的**：先列出 ABC `&` 组 130+ 条命令，挑出 `&muxdec/&bidec/&dsd/&dc2/&syn3/&fraig/&resyn3` 七个候选，逐个追加到现有脚本后测 9 例面积；只有 `&syn3` 全面改善（R 19.47%→20.41%），且 `-h` 显示它无可调 effort 参数。时序上 `&syn3` 比 `&dch` 便宜得多（+2.4% vs +11.7%），因为它不带 choice 计算。

**已知代价**：`&syn3` 对小乘法器（multiplier，33→36）有 +9% 回归。官方 9 例无此问题，但若新用例含小算术结构需复测。

### 9.2 历史数据（4 例口径，2026-10-24/26）

早先按规模分档脚本（≥500 单元用 `-D 3000` 默认脚本，<500 用自定义脚本）时：test1 204、test2 15、test3 11、test4 27，4 例 R = 20.10%，9 例 R = 18.34%。**该方案已废止**：10-26 答疑 C130 明确要求统一脚本、且不得对其他用例起副作用，按规模切换脚本不再合规；改用统一脚本后 R 变为 19.00%（仍高于 18.34%，且逐例不增）。

### 9.3 收益归因（2×2 对照，2026-10-27 实测）

四个格子：`pmux_recode` 开/关 × abc 脚本（上游参数 `abc9 -lut 4 -W 300` / 统一脚本）。

| 用例 | baseline | 仅脚本 | 仅改写 | 两者（当前） | 脚本贡献 | 改写贡献 |
|---|---|---|---|---|---|---|
| test1 | 288 | 288 | 228 | 227 | 0.00% | 20.83% |
| test2 | 25 | 15 | 25 | 15 | 40.00% | 0.00% |
| test3 | 13 | 12 | 13 | 12 | 7.69% | 0.00% |
| test4 | 36 | 27 | 36 | 27 | 25.00% | 0.00% |
| case01 | 426 | 398 | 390 | 355 | 6.57% | 8.45% |
| case02 | 450 | 407 | 437 | 340 | 9.56% | 2.89% |
| case03 | 408 | 375 | 367 | 339 | 8.09% | 10.05% |
| case04 | 368 | 350 | 356 | 335 | 4.89% | 3.26% |
| case05 | 413 | 398 | 409 | 371 | 3.63% | 0.97% |
| **R（9 例平均）** | **0%** | **11.71%** | **5.16%** | **19.00%** | — | — |

- 「仅脚本」= 关闭 `pmux_recode` 后重新编译实测；「仅改写」= 保留 `pmux_recode` 但把 abc 换回上游参数（`-run begin:map_luts` + `abc9 -lut 4 -W 300` + `-run map_cells:`）。
- 两者之和 16.87% < 实测 19.00%，**交互项 +2.13%**：改写后的网表（一热选通→总线索引树）让 abc 的 `&syn2`/`&dch` 更容易继续收敛。交互在 case02（+12.0）、case05（+5.6）最明显。
- 结论：收益不是单点技巧，而是「结构性改写 + 映射脚本」两段互补；只做任何一段都拿不到 19%。

### 9.4 深链（case04/05）级联实验记录（2026-10-27 晚第二/三轮）

**策略对比（9 例，LC）**：

| 策略 | test1 | case01 | case02 | case03 | case04 | case05 | R |
|---|---|---|---|---|---|---|---|
| 链深<2（当前采用） | 227 | 357 | 339 | 330 | 333 | 364 | **19.47%** |
| 偶数层（0,2,…） | 227 | 355 | 340 | 339 | 365 | 361 | 18.37% |
| 奇数层 | 288 | 403 | 386 | 362 | 365 | 397 | 15.9% |
| 只改深≥2 | 288 | 398 | 407 | 375 | 363 | 390 | 14.6% |

结论：奇偶交替、只改深层都不如「链深<2」。case04 的 4 级链在 depth<2 时已是最优（333），再往前改反而变差。

**统一脚本再搜索（recorde 改动后重扫，LC）**：

| 脚本 | test1 | case01 | case02 | case03 | case04 | case05 | R |
|---|---|---|---|---|---|---|---|
| A：syn2 + dch -f（当前） | 227 | 357 | 339 | 330 | 333 | 364 | 19.47% |
| G：syn2 + dch -f + dch（无 -f） | **218** | **349** | 344 | 329 | **318** | **358** | **20.54%** |
| C：A + mfs | 227 | 355 | 339 | 330 | 367 | 373 | 19.11% |
| B：syn2 + dch -f + dch -f | 219 | 356 | 344 | 330 | 319 | 353 | 18.69%（test3 14>13 违规） |
| E：A + synch2 尾 | 251 | 379 | 360 | 357 | 377 | 379 | 13.1% |

**G 脚本（20.54%）被放弃的原因：时间超标**。实测（11~15 次，min/median）：

| 用例 | baseline | G 脚本 | 偏差 |
|---|---|---|---|
| case04 | 437 ms | 488 ms | **+11.7%**（median +15.9%） |
| case05 | 447 ms | 516 ms | **+15.4%**（median +6.8%） |

换成 `-D 2000` 后面积仍 20.5%（case04 312），但时间依旧 +11.7%/+15.4%——多出来的 `&dch` pass 本身就这样，与 delay target 无关。5% 是硬线，所以回退到 A 脚本（19.47%）。

### 9.5 ABC `&dch` 语义与"压缩时间"尝试（2026-10-27 晚第三轮）

`yosys-abc -c "&dch -h"` 的真实参数（版本为随 yosys 0.69 构建的 abc）：

```
usage: &dch [-WCS num] [-sptfremngcxyvh]
 -W num : 最大模拟字数 [8]        -C num : 每节点最大冲突 [1000]
 -S num : 最大 SAT 变量 [5000]    -s : 合成三个快照（开）
 -t : 对 TFO 类做模拟（开）        -f : 用更轻的逻辑综合（关）
 -e : 合并等价（关） -m : 合并后最小化逻辑级数（关） -r/-n/-g/-c/-x/-y/-v/-h
```

实测（9 例面积全等 20.54%，只在 case04/case05 上量时间，7 次中位数）：

| 变体 | case04 | case05 |
|---|---|---|
| G0（`&dch` 原样） | +8.8% | +8.5% |
| G1（`&dch -s`） | +10.8% | +14.0% |
| G2（`&dch -t`） | +6.9% | +18.5% |
| G3（`&dch -W 2 -C 200 -S 1000`） | +6.2% | +16.1% |
| H2（把第一个 `&dch -f` 也限流） | +7.5% | +14.0% |
| H3（`&dch -x`）/ H4（`&dch -y`） | +14.6% / +9.6% | +21.2% / +12.5% |

结论：**`&dch` 的时间主要由其结构/等价合并逻辑主导，限流参数压不下来**（最好也只到 +6.2%），所以 G 系列统一脚本（20.54%）依旧被 5% 硬线否掉。

另外一个语法约束（踩坑）：`&save` 之前必须有 `&if`（把 GIA 映射成网络），否则 ABC 直接 `Error: GIA has no mapping` 并中止；去掉 `&save/&load` 硬串起来虽然能跑，但 test1 从 218 退化到 240。所以脚本就是 `…&if; &save; &st; <pass>; &if…; &save; &load; &st; …` 这个骨架，压不动。

## 10. 官方最新口径（10.19–10.21 补充答疑，与上文冲突时以本节为准）


来源：`FPGA创新大赛问题汇总` 表格 B114–C117 行（全表最后四条相关问答）。答疑原话要点：

1. **abc 脚本允许修改**（Q114：是改 `abc9.cc` 内置脚本，还是在 `synth_intel.cc` 里显式调用自定义脚本？）答：「推荐直接修改 abc9.cc 的内置脚本命令，显式给出脚本供 abc 使用也可以。」⇒ 本项目当前在 `synth_intel.cc` 里显式传 `-script` 属于官方认可的第二种形式；同时保留「≥500 单元回落到 ABC 默认脚本」的规模分流也仍在"abc 脚本"范畴内。此前 C23「可以调整 abc 的脚本」、C101-1、C104-1 亦一致。
2. **不许拿别处的提速抵消 PMUX 的时间开销**（C114-2）：「不允许通过优化其他 pass 或者依赖程序的方法抵消 pmux 优化 pass 增加的时间」。⇒ 追求 §2.2 的 ≤5% 必须来自 pmux 自身或 abc 脚本本身，不能靠关掉/弱化别的 pass 来"省"时间。
3. **会追加用例**（C115-1）：「10 月底之前提供 3~5 个用例」，含大规模、PMUX 嵌套、带寄存器结构。评分改为**全部用例**（含新增）算术平均（C115-2），且**任何单个用例不得出现资源增加**；cell 数以**输出网表**为准（C115-3）。⇒ §9 的 4 例 22.5% 只是过渡数据，必须补跑新用例。
4. **构建与计时**（C116-1、C117）：「推荐编译成 windows exe」，wsl 下必须能现场编译；计时工具自备。⇒ 交付物必须是可交付的 exe + 依赖库，`cmake` 工程要能在干净环境 configure。
5. **打包清单**（C55）不变：优化源码（含新增 `passes/pmux/*.cc`，若改了 abc 脚本则一并提交）+ 编译后的 yosys 可执行程序及依赖库。


### 10.1 2026-10-22~10-26 补充答疑（C130–C134，优先级高于上文）

来源：`FPGA创新大赛问题汇总 (1).xlsx`（10-27 版，共 134 行），比 10-23 版多 17 行。

1. **C130-1（最关键）**：「ABC 优化脚本是否必须对所有用例使用单一固定脚本？是否允许根据综合前网表的通用结构特征选择不同优化脚本？」答：「**需要用统一脚本**，脚本在优化某个用例的基础上不能对其他用例起副作用。」
   ⇒ 我们原先「≥500 单元用默认 `-D 3000`、<500 单元用自定义脚本」的**按规模分档已不合规**，2026-10-27 改为**单一脚本**（见 §9 顶部），并重新搜索了脚本形态，最终 R 从 18.34%（分档，9 例）变为 **19.00%（统一脚本）**，逐例仍不增加。
2. **C130-2**：「①判定的资源口径是 LUT、LUT+$not，还是含 DFF 的总 cell 数？DFF 单增 1 个算不算增加？②个别用例资源增加是稀释还是判不合格？」答：「**主要看逻辑单元，dff 增加不做关注；个别用例也不可以增加，是通用优化而不是针对个别用例的优化**。」
   ⇒ 口径确认：只看逻辑单元；任何单个用例都不许增；且必须是通用优化（不能按用例打补丁）。
3. **C131-1**：交付要「解包之后应能直接启动，不要用绝对路径」。⇒ 提交的二进制目录与 `share/` 必须相对路径可运行（当前 `yosys-opt/` 自带 DLL 与 share，满足）。
4. **C132（计时口径）**：「运行前先预热跑原版 + case 10 次，然后开始先跑原版再跑优化版，后台程序保持统一，杀软关闭；若某次结果超过 5%，小 case（总时间不足 1 s）可复核 5 次取平均，大 case（10 s 以上）只有一次机会。」
   ⇒ 我们按此实现了 `out/timing_official.sh`（10 次预热 + 先原版后优化 + 5 次平均）。
5. **C133/C134**：「一次运行同时测耗时、内存、逻辑单元」「会进行预热，原版也会需要等待」。⇒ 内存与时间同一次运行采样即可。

---

## 11. 本机构建（2026-10-24 实测可复现）

MSYS2 的 cygwin 程序（flex/bison/msys bash）在受限环境里可能直接起不来（`STATUS_DLL_INIT_FAILED`，或 `NtCreateDirectoryObject ... 0xC0000022`），所以改用**原生 winflexbison**：

1. 工具：`out/wf/win_flex.exe`（2.6.4）、`out/wf/win_bison.exe`（3.8.2），来源 lexxmark/winflexbison v2.5.25；`PATH` 只加 `D:\msys64\mingw64\bin`（g++ 16.2 / cmake 4.4 / mingw32-make）。
2. slang 版本：仓库里若带非语义 tag，`libs/slang` 的 `git describe` 会拼出非法版本串。已在 `yosys-src/libs/slang/CMakeLists.txt` 里把 `SLANG_VERSION_PATCH/PRERELEASE/HASH` 固定为 `0/""/0`（本地改动，不入库）。
3. configure：

   ```powershell
   cmake -S yosys-src -B build-new -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release `
     -DGIT_EXECUTABLE=D:/Git/cmd/git.exe `
     -DFLEX_EXECUTABLE=C:/yosys/out/wf/win_flex.exe `
     -DBISON_EXECUTABLE=C:/yosys/out/wf/win_bison.exe `
     -DCMAKE_EXE_LINKER_FLAGS="-static"
   ```

   `GIT_EXECUTABLE` 必须是 Git for Windows 的 git；用 MSYS2 的 git 会返回 POSIX 路径 `/c/yosys/.git`，configure 会死在 `cmake/YosysVersion.cmake:54`。
4. 编译：`cmake --build build-new --target yosys -j 16`。首次全量约 2 小时；只改 `synth_intel.cc` 的增量约 3 分钟。
5. ABC 不需要单独编：把 `yosys-opt\yosys-abc.exe` 复制成 `build-new\yosys-abc.exe` 即可。
6. 安装：`Copy-Item build-new\yosys.exe yosys-opt\yosys-opt.exe -Force`。静态链接后只依赖 KERNEL32/msvcrt/libffi-8/libreadline8/tcl86/zlib1，`yosys-opt\` 目录里已备齐。
7. 若 `yosys-opt\share` 缺失：`git restore --source=HEAD --worktree -- yosys-opt/share`。

---

## 12. 等价性检查（复现官方 eqy 口径）

官方用 eqy 验 **ABC 之后**的 `$lut` 网表（C54-3）。本机没有 eqy，用 Yosys 自带的 `equiv_opt`（同一套 `equiv_make`/`equiv_induct` 引擎，时间归纳）复现，四个用例全部通过。

单例脚本（`out/eqy/final_testN.ys`）：

```
read_verilog test/testN.v
prep -top testN
equiv_opt -assert -async2sync -map C:/yosys/out/eqy/dffeas_sim.v synth_intel -family cycloneiv -top testN -run begin:map_cells
```

要点：

- `-run begin:map_cells` 让门级停在 ABC 之后（`$lut` + `dffeas`），before/after 只差 `map_cells` 那一步不做，正好对上官方“abc 之后输出 ilang”。
- `-map dffeas_sim.v`：给 `dffeas` 写的最小行为模型（异步低电平清零 + 时钟使能，`initial` 按 `power_up` 置初值）。用 `share/intel/cycloneiv/cells_sim.v` 里的原模型会推出 latch，SAT 无法处理。
- `-async2sync`：把 `$adff`/`$dlatch` 转同步后再做归纳（test1/test3/test4 有异步复位）。
- test4 的 `case` 表在 gold 里会变成 `$mem_v2`，脚本里先 `memory -nomap; memory_map; opt -fast` 再比。
- 一键：`out/eqy/run_all.ps1`，输出 `test1 PASS 222 cells / test2 PASS 5 / test3 PASS 18 / test4 PASS 10`。

> 如果评测方真的用 eqy：把 `dffeas_sim.v` 作为 `read -lib` 或 `-map`，同样的 `-run begin:map_cells` 产出 gate，`post_abc.il` 就是官方要的输入。
