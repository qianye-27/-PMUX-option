param(
    [string]$Baseline = "",
    [switch]$NoEqy
)

# One-shot reproduction of every result reported in the technical document:
#   1) logic cells + reduction rate + wall clock + peak memory (all 9 cases)
#   2) equivalence: RTL vs post-abc9 $lut netlist
#   3) eqy (official flow) -- skipped automatically when OSS CAD Suite is absent
#
# Usage (run from anywhere):
#     .\check.ps1                 # 1 + 2 + 3
#     .\check.ps1 -NoEqy          # 1 + 2
#     .\check.ps1 -Baseline <original yosys.exe>
# ASCII-only on purpose: PowerShell 5.1 mis-parses UTF-8 without BOM.

$ErrorActionPreference = 'Continue'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $here
Set-Location $here
$LASTEXITCODE = 0

# ---- locate the original yosys (for base LC / reduction rate) --------------
if ($Baseline -eq "") {
    $cands = @(
        (Join-Path $here 'bin\yosys-orig.exe'),
        (Join-Path $here 'bin\yosys-baseline.exe'),
        (Join-Path $root 'yosys-baseline\yosys-baseline.exe'),
        (Join-Path $root '..\yosys-baseline\yosys-baseline.exe')
    )
    if ($env:YOSYS_BASELINE) { $cands = @($env:YOSYS_BASELINE) + $cands }
    foreach ($c in $cands) { if (Test-Path $c) { $Baseline = $c; break } }
}

# ---- locate OSS CAD Suite (for eqy) ---------------------------------------
$suite = $env:OSS_CAD
if ($suite -and -not (Test-Path (Join-Path $suite 'environment.bat'))) { $suite = "" }
if (-not $suite) {
    $cands = @(
        'C:\oss-cad-suite', 'C:\oss-cad-suite\oss-cad-suite',
        'C:\tools\oss-cad-suite', 'C:\tools\oss-cad-suite\oss-cad-suite',
        (Join-Path $root 'tools\oss-cad-suite'), (Join-Path $root 'tools\oss-cad-suite\oss-cad-suite'),
        (Join-Path $root '..\tools\oss-cad-suite'), (Join-Path $root '..\tools\oss-cad-suite\oss-cad-suite'),
        (Join-Path $root '..\..\tools\oss-cad-suite'), (Join-Path $root '..\..\tools\oss-cad-suite\oss-cad-suite')
    )
    foreach ($c in $cands) {
        if (Test-Path (Join-Path $c 'environment.bat')) { $suite = $c; break }
    }
}

# ---- 1/3 area + rate + time + memory --------------------------------------
Write-Host "== 1/3  logic cells / reduction rate / wall clock / peak memory ============="
if ($Baseline -ne "") {
    Write-Host ("(original yosys: " + $Baseline + ")")
    & (Join-Path $here 'bench\measure.ps1') -Baseline $Baseline `
        -Optimized (Join-Path $here 'bin\yosys-opt.exe') -Cases (Join-Path $here 'case') -Reps 5 -Warmup 3
} else {
    Write-Host "(no original yosys found; printing logic cells only -- put it at bin\yosys-orig.exe to get the rate)"
    & (Join-Path $here 'bench\run_cases.ps1') -Cases (Join-Path $here 'case')
}
$rc1 = $LASTEXITCODE

# ---- 2/3 equivalence -------------------------------------------------------
Write-Host ""
Write-Host "== 2/3  equivalence: RTL vs post-abc9 LUT netlist ============================"
& (Join-Path $here 'verify\run_equiv.ps1') -Cases (Join-Path $here 'case')
$rc2 = $LASTEXITCODE

# ---- 3/3 eqy ---------------------------------------------------------------
$rc3 = 0
Write-Host ""
if ($NoEqy) {
    Write-Host "== 3/3  eqy skipped (-NoEqy) ================================================="
} elseif ($suite -eq "") {
    Write-Host "== 3/3  eqy skipped: OSS CAD Suite not found ================================="
    Write-Host "        install it, or set  `$env:OSS_CAD = <path-to-oss-cad-suite>, then rerun."
} else {
    Write-Host ("== 3/3  eqy (official flow, suite: " + $suite + ") ==================")
    Push-Location (Join-Path $here 'verify\eqy')
    & (Join-Path $here 'verify\eqy\run_eqy.bat') -Cases (Join-Path $here 'case')
    $rc3 = $LASTEXITCODE
    Pop-Location
}

Write-Host ""
if ($rc1 -eq 0 -and $rc2 -eq 0 -and $rc3 -eq 0) {
    Write-Host "ALL CHECKS PASSED" -ForegroundColor Green
    exit 0
}
Write-Host ("SOME CHECKS FAILED (rc: " + $rc1 + "/" + $rc2 + "/" + $rc3 + ")") -ForegroundColor Red
exit 1
