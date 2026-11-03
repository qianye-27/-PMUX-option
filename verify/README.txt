等价性验证（对应赛题"abc 之后输出 ilang 再验"的口径）

用法（PowerShell，相对路径）：
  在 perfect\verify 下：
    .\run_equiv.ps1                                   # 自动跑 cases\*.v
    .\run_equiv.ps1 -Cases ..\..\test\test1.v          # 单个用例
    .\run_equiv.ps1 -Cases '..\..\test\test1.v,..\..\test\test4.v'   # 逗号分隔

内部流程（与 run_equiv.ps1 生成的一致）：
  read_verilog <case>.v
  prep -top <case>
  memory -nomap ; memory_map ; opt -fast     # 仅当用例含 case 表常量（如 test4）时必需，其余为无害 no-op
  equiv_opt -assert -async2sync -map dffeas_sim.v synth_intel -family cycloneiv -top <case> -run begin:map_cells

要点：
  - -run begin:map_cells 让"门级"停在 ABC 之后（$lut + dffeas），与官方的"abc 之后输出 ilang"一致
  - dffeas_sim.v 是 dffeas 的最小行为模型（异步低电平清零 + 时钟使能 + 按 power_up 置初值）；
    用官方 cells_sim.v 的模型会推出 latch，SAT 无法处理
  - -async2sync 把 $adff/$dlatch 转同步后再做时间归纳

本包实测：9 个官方用例全部 PASS。
