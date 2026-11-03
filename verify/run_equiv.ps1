param(
    [string[]]$Cases = @(),
    [string]$Yosys = "",
    [string]$Dffeas = ""
)

# Equivalence check: RTL (gold) vs post-abc9 $lut netlist (gate), temporal induction.
# Usage (relative paths are resolved against the directory you call it from):
#   .\verify\run_equiv.ps1 -Cases .\case
#   .\verify\run_equiv.ps1 -Cases .\case\test1.v
#   .\verify\run_equiv.ps1                     # auto: verify\cases\*.v
# ASCII-only on purpose: PowerShell 5.1 mis-parses UTF-8 without BOM.

$ErrorActionPreference = 'Continue'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $here
if ($Yosys -eq "") { $Yosys = Join-Path $root "bin\yosys-opt.exe" }
if ($Dffeas -eq "") { $Dffeas = Join-Path $here "dffeas_sim.v" }

. (Join-Path $here '_tmpfix.ps1')
$env:TEMP = Get-WorkTemp -Root $root
$env:TMP = $env:TEMP

$Yosys = (Resolve-Path $Yosys).Path
$model = (Resolve-Path $Dffeas).Path

$Cases = @($Cases | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' })
$resolved = @()
foreach ($c in $Cases) {
    if (-not [IO.Path]::IsPathRooted($c)) { $c = Join-Path (Get-Location).Path $c }
    if (Test-Path $c -PathType Container) {
        $resolved += @(Get-ChildItem -Path $c -Filter *.v | ForEach-Object { $_.FullName })
    } else {
        $resolved += $c
    }
}
$Cases = $resolved

if ($Cases.Count -eq 0) {
    $dir = Join-Path $here 'cases'
    if (Test-Path $dir) { $Cases = @(Get-ChildItem $dir -Filter *.v | ForEach-Object { $_.FullName }) }
    if ($Cases.Count -eq 0) { Write-Host "No cases: pass -Cases <file-or-dir> or put .v files in verify\cases"; exit 1 }
}

$ok = 0
$bad = 0
foreach ($v in $Cases) {
    if (-not (Test-Path $v)) { Write-Host ("MISSING  " + $v) -ForegroundColor Yellow; continue }
    $top = [IO.Path]::GetFileNameWithoutExtension($v)
    $ys = Join-Path $env:TEMP ("eq_" + $top + ".ys")
    $log = Join-Path $here ("eq_" + $top + ".log")

    $lines = @()
    $lines += "read_verilog " + $v
    $lines += "prep -top " + $top
    $lines += "memory -nomap"
    $lines += "memory_map"
    $lines += "opt -fast"
    $lines += "equiv_opt -assert -async2sync -map " + $model + " synth_intel -family cycloneiv -top " + $top + " -run begin:map_cells"
    Set-Content -Path $ys -Value $lines -Encoding ASCII

    $p = Start-Process -FilePath $Yosys -ArgumentList @('-q','-s',$ys,'-l',$log) -PassThru -NoNewWindow `
        -RedirectStandardOutput ($log + ".stdout") -RedirectStandardError ($log + ".stderr") -Wait
    $rc = $p.ExitCode
    $proved = (Select-String -Path $log -Pattern 'successfully proven' -Quiet)
    if ($rc -eq 0 -and $proved) {
        Write-Host ("PASS  " + $top) -ForegroundColor Green
        $ok++
    } else {
        Write-Host ("FAIL  " + $top + "   (see " + $log + ")") -ForegroundColor Red
        $bad++
    }
}

Write-Host ""
Write-Host ("Result: PASS " + $ok + " / FAIL " + $bad)
if ($bad -gt 0) { exit 1 }
