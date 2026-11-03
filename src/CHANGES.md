# 相对 Yosys 0.69 的改动清单

本目录只包含**被修改或新增**的文件，其余源码与官方 Yosys 0.69 完全一致。
编译时把这 6 个文件放回源码树对应位置即可（`passes/pmux/` 与 `techlibs/intel/`）。

## 新增文件（4 个 pass + 1 个构建注册）

| 文件 | 目标位置 | 作用 |
|---|---|---|
| `pmux_recognize.cc` | `passes/pmux/pmux_recognize.cc` | Phase 1：识别 `$pmux` 结构，判定选择端来源与编码方式，写 `pmux_*` 属性（只读） |
| `pmux_decompose.cc` | `passes/pmux/pmux_decompose.cc` | Phase 2：按分类展开（保留，当前未在流程中启用） |
| `pmux_optimize.cc` | `passes/pmux/pmux_optimize.cc` | Phase 3：删除 A==B 的 `$mux`、合并相同 `$reduce_or`、位宽安全的 `$eq` 化简 |
| `pmux_recode.cc` | `passes/pmux/pmux_recode.cc` | 核心：把「常量解码型」`$pmux` 改写为以总线位为选择端的 `$mux` 平衡树 |
| `passes_pmux_CMakeLists.txt` | `passes/pmux/CMakeLists.txt` | 注册上述 4 个 pass |

## 修改文件（1 个）

| 文件 | 目标位置 | 改动 |
|---|---|---|
| `synth_intel.cc` | `techlibs/intel/synth_intel.cc` | ① 在 `pmux_opt` 标签处依次调用 `pmux_recognize -annotate` → `pmux_optimize` → `pmux_recode` → `opt_clean`；② 把 `map_luts` 里的 abc9 命令替换为下面的**统一脚本**（仍是单次 `abc9` 调用） |

### 统一 abc9 脚本（对所有设计完全相同）

```
abc9 -lut 4 -W 300 -D 3000 -script "+&scorr; &sweep; &if {W} {D}; &save; &st; &syn2; &if {W} {D} {R} -a -v; &save; &load; &st; &dch -f; &if {W} {D} {R} -a -v; &save; &load; &st; &syn3; &if {W} {D} {R} -a -v;"
```

## 没有做的事（按答疑约束）

- 未修改除 `synth_intel.cc` 之外任何既有 pass 的源码；
- 未调整既有 pass 的调用顺序；
- 未额外多次调用 `abc`（脚本内部的多个 `&if` 都在同一次 abc 进程内）；
- 未在 abc9 之后对 `$lut` 网表做任何改写。

## 判据（`pmux_recode` 默认参数）

- `-max-width 6`：总线位宽上限（控制树规模）；
- `-min-coverage 50`：`2^W ≥ 50%·N` 才改写，避免稀疏选择反而变贵；
- `-max-chain 2`：候选依赖链深 < 2 才改写，避免把 mux 链变成深树；
- `-mode depth`：按链深选择策略。
