param(
    [string]$Baseline = "",
    [switch]$Eqy
)

# One-shot self check (run from anywhere):
#     .\check.ps1                       -> logic-cell table + reduction rate + equivalence
#     .\check.ps1 -Baseline <yosys.exe>  -> explicit original yosys
#     .\check.ps1 -Eqy                   -> ... plus the eqy run (needs OSS CAD Suite)
# The original yosys is auto-detected at ..\yosys-baseline\yosys-baseline.exe.
# ASCII-only on purpose: PowerShell 5.1 mis-parses UTF-8 without BOM.

$ErrorActionPreference = 'Continue'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $here
Set-Location $here
$LASTEXITCODE = 0

if ($Baseline -eq "") {
    $cands = @(
        (Join-Path $here 'bin\yosys-orig.exe'),
        (Join-Path $here 'bin\yosys-baseline.exe'),
        (Join-Path $root 'yosys-baseline\yosys-baseline.exe'),
        (Join-Path $root '..\yosys-baseline\yosys-baseline.exe'),
        (Join-Path $here '..\yosys-baseline\yosys-baseline.exe')
    )
    if ($env:YOSYS_BASELINE) { $cands = @($env:YOSYS_BASELINE) + $cands }
    foreach ($c in $cands) { if (Test-Path $c) { $Baseline = $c; break } }
}

Write-Host "== 1/3  logic cells and reduction rate ======================================="
if ($Baseline -ne "") {
    & (Join-Path $here 'bench\run_cases.ps1') -Cases (Join-Path $here 'case') -Baseline $Baseline
} else {
    Write-Host "(no original yosys found next to the package; printing logic cells only)"
    & (Join-Path $here 'bench\run_cases.ps1') -Cases (Join-Path $here 'case')
}
$rc1 = $LASTEXITCODE

Write-Host ""
Write-Host "== 2/3  equivalence: RTL vs post-abc9 LUT netlist ============================"
& (Join-Path $here 'verify\run_equiv.ps1') -Cases (Join-Path $here 'case')
$rc2 = $LASTEXITCODE

$rc3 = 0
if ($Eqy) {
    Write-Host ""
    Write-Host "== 3/3  eqy (official flow) =================================================="
    Push-Location (Join-Path $here 'verify\eqy')
    & (Join-Path $here 'verify\eqy\run_eqy.bat') -Cases (Join-Path $here 'case')
    $rc3 = $LASTEXITCODE
    Pop-Location
} else {
    Write-Host ""
    Write-Host "== 3/3  eqy skipped (add -Eqy to run it) ====================================="
}

Write-Host ""
if ($rc1 -eq 0 -and $rc2 -eq 0 -and $rc3 -eq 0) {
    Write-Host "ALL CHECKS PASSED" -ForegroundColor Green
    exit 0
}
Write-Host ("SOME CHECKS FAILED (rc: " + $rc1 + "/" + $rc2 + "/" + $rc3 + ")") -ForegroundColor Red
exit 1
