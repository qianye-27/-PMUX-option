param(
    [Parameter(Mandatory=$true)][string]$Cases,
    [string]$Yosys = "..\..\bin\yosys-opt.exe",
    [string]$Dffeas = "dffeas_sim.v",
    [int]$Depth = 10
)

# Official-style equivalence check.
#   gold = RTL, gate = post-abc9 $lut netlist (cycloneiv cell mapping excluded).
# eqy is used first (partition mode, the default). If a partition cannot be proven,
# the case is re-checked with a whole-design miter + SAT temporal induction, which the
# organisers explicitly allow (9月24-28日答疑·赛题二EQY第5问: switching verification mode counts as pass).
# Only when both methods fail is the case reported as FAIL.
# ASCII-only on purpose: PowerShell 5.1 mis-parses UTF-8 without BOM.

$ErrorActionPreference = 'Continue'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$caller = (Get-Location).Path
Set-Location $here
. (Join-Path $here '_tmpfix.ps1')
$env:TEMP = Get-WorkTemp -Root (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$env:TMP = $env:TEMP
if (-not [IO.Path]::IsPathRooted($Cases)) { $Cases = Join-Path $caller $Cases }

$work = Join-Path $here 'work'
New-Item -ItemType Directory -Force -Path $work | Out-Null
$Yosys = (Resolve-Path $Yosys).Path
$Dffeas = (Resolve-Path $Dffeas).Path
Copy-Item $Dffeas (Join-Path $work 'dffeas_sim.v') -Force

$files = @(Get-ChildItem -Path $Cases -Filter *.v)
if ($files.Count -eq 0) { Write-Host "no .v files under $Cases"; exit 1 }

function Run-Yosys {
    param([string]$Script, [string]$Log)
    $p = Start-Process -FilePath $Yosys -ArgumentList @('-q','-s',$Script,'-l',$Log) -PassThru -NoNewWindow `
        -RedirectStandardOutput ($Log + ".stdout") -RedirectStandardError ($Log + ".stderr") -Wait
    return $p.ExitCode
}

function Try-Miter {
    param([string]$Top)
    $ys = @()
    $ys += "read_verilog " + $Top + ".v"
    $ys += "prep -top " + $Top
    $ys += "memory -nomap"
    $ys += "memory_map"
    $ys += "opt -fast"
    $ys += "design -stash gold"
    $ys += "read_rtlil gate_" + $Top + ".il"
    $ys += "techmap -autoproc -map dffeas_sim.v"
    $ys += "opt -fast"
    $ys += "rename " + $Top + " gate"
    $ys += "design -stash gate"
    $ys += "design -copy-from gold -as gold " + $Top
    $ys += "design -copy-from gate -as gate gate"
    $ys += "miter -equiv -flatten -make_outputs gold gate miter"
    $ys += "sat -verify -prove-asserts -tempinduct -show-ports miter"
    $script = Join-Path $work ("miter_" + $Top + ".ys")
    $log = Join-Path $here ("results_" + $Top + "_miter.log")
    Set-Content -Path $script -Value $ys -Encoding ASCII
    Push-Location $work
    $rc = Run-Yosys -Script ("miter_" + $Top + ".ys") -Log $log
    Pop-Location
    $base = Select-String -Path $log -Pattern 'Base case for induction .* proven' -Quiet
    $step = Select-String -Path $log -Pattern 'Induction step proven: SUCCESS' -Quiet
    if ($rc -eq 0 -and $base -and $step) { return $true }
    return $false
}

$pass = 0
$fail = 0
$miterUsed = 0
foreach ($case in $files) {
    $top = $case.BaseName
    Copy-Item $case.FullName (Join-Path $work ($top + '.v')) -Force

    # 1) gate netlist: stop right after abc9 ($lut cells)
    $gen = @()
    $gen += "read_verilog " + $top + ".v"
    $gen += "synth_intel -family cycloneiv -top " + $top + " -run begin:map_cells"
    $gen += "select " + $top
    $gen += "write_rtlil -selected gate_" + $top + ".il"
    Set-Content -Path (Join-Path $work ("gen_" + $top + ".ys")) -Value $gen -Encoding ASCII
    Push-Location $work
    $null = Run-Yosys -Script ("gen_" + $top + ".ys") -Log ("gen_" + $top + ".log")
    Pop-Location

    # 2) eqy config: gold = RTL, gate = $lut netlist with dffeas mapped back to FFs
    $eqy = @()
    $eqy += "[gold]"
    $eqy += "read_verilog " + $top + ".v"
    $eqy += "prep -top " + $top
    $eqy += "memory -nomap"
    $eqy += "memory_map"
    $eqy += "opt -fast"
    $eqy += ""
    $eqy += "[gate]"
    $eqy += "read_rtlil gate_" + $top + ".il"
    $eqy += "techmap -autoproc -map dffeas_sim.v"
    $eqy += "opt -fast"
    $eqy += ""
    $eqy += "[strategy simple]"
    $eqy += "use sat"
    $eqy += "depth " + $Depth
    Set-Content -Path (Join-Path $work ($top + ".eqy")) -Value $eqy -Encoding ASCII

    # 3) run eqy (partition mode)
    $eqyLog = Join-Path $here ("results_" + $top + ".log")
    Push-Location $work
    & eqy -f ($top + ".eqy") 2>&1 | Tee-Object -FilePath $eqyLog | Out-Null
    $rc = $LASTEXITCODE
    Pop-Location

    if ($rc -eq 0) {
        Write-Host ("PASS  " + $top) -ForegroundColor Green
        $pass++
        continue
    }

    # 4) eqy partition inconclusive -> whole-design miter (allowed by 9月24-28日答疑·赛题二EQY第5问)
    if (Try-Miter -Top $top) {
        Write-Host ("PASS  " + $top + "   (eqy partition inconclusive; whole-design miter proved equivalence)") -ForegroundColor Green
        $pass++
        $miterUsed++
    }
    else {
        Write-Host ("FAIL  " + $top + "   (see results_" + $top + ".log and results_" + $top + "_miter.log)") -ForegroundColor Red
        $fail++
    }
}

Write-Host ""
if ($miterUsed -gt 0) {
    Write-Host ("note: " + $miterUsed + " case(s) were proven with the whole-design miter because eqy partitioning cut a cross-flip-flop invariant.")
    Write-Host ("      Switching verification mode is explicitly allowed (9月24-28日答疑·赛题二EQY第5问); details in results_<case>_miter.log.")
}
Write-Host ("eqy result: PASS " + $pass + " / FAIL " + $fail)
if ($fail -gt 0) { exit 1 }
