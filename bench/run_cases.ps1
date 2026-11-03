param(
    [string]$Yosys = "",
    [string]$Baseline = "",
    [string]$Cases = ""
)

# Print per-case logic cells; when -Baseline is given also print the reduction rate.
# Usage (from the package root):
#     .\bench\run_cases.ps1
#     .\bench\run_cases.ps1 -Baseline ..\yosys-baseline\yosys-baseline.exe
# ASCII-only on purpose: PowerShell 5.1 mis-parses UTF-8 without BOM.

$ErrorActionPreference = 'Continue'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $here
$caller = (Get-Location).Path
if ($Yosys -eq "") { $Yosys = Join-Path $root "bin\yosys-opt.exe" }
if ($Cases -eq "") { $Cases = Join-Path $root "case" }

. (Join-Path $here '_tmpfix.ps1')
$tmp = Get-WorkTemp -Root $root
$env:TEMP = $tmp
$env:TMP = $tmp
if (-not [IO.Path]::IsPathRooted($Cases)) { $Cases = Join-Path $caller $Cases }

$Yosys = (Resolve-Path $Yosys).Path
$hasBase = $false
if ($Baseline -ne "" -and (Test-Path $Baseline)) { $Baseline = (Resolve-Path $Baseline).Path; $hasBase = $true }

$files = @(Get-ChildItem -Path $Cases -Filter *.v | Sort-Object Name)
if ($files.Count -eq 0) { Write-Host "no .v files under $Cases"; exit 1 }

function Get-LC {
    param([string]$Exe, [string]$Top, [string]$Dir)
    $ys = Join-Path $Dir ("lc_" + $Top + ".ys")
    $log = Join-Path $Dir ("lc_" + $Top + "_" + [IO.Path]::GetFileNameWithoutExtension($Exe) + ".log")
    $body = @()
    $body += "read_verilog " + (Join-Path $Dir ($Top + ".v"))
    $body += ""
    $body += "synth_intel -family cycloneiv -top " + $Top
    $body += "stat"
    Set-Content -Path $ys -Value $body -Encoding ASCII
    Start-Process -FilePath $Exe -ArgumentList @('-q','-s',$ys,'-l',$log) -Wait -NoNewWindow `
        -RedirectStandardOutput ($log + ".stdout") -RedirectStandardError ($log + ".stderr") | Out-Null
    $lut = 0; $inv = 0; $dff = 0
    if (Test-Path $log) {
        foreach ($line in Get-Content $log) {
            if ($line -match '^\s+(\d+)\s+cycloneiv_lcell_comb\s*$') { $lut = [int]$Matches[1] }
            if ($line -match '^\s+(\d+)\s+\$not\s*$') { $inv = [int]$Matches[1] }
            if ($line -match '^\s+(\d+)\s+dffeas\s*$') { $dff = [int]$Matches[1] }
        }
    }
    return @(($lut + $inv), $lut, $inv, $dff)
}

if ($hasBase) {
    Write-Host ("{0,-24} {1,8} {2,8} {3,8} {4,5}" -f "case", "base LC", "opt LC", "reduction", "DFF")
} else {
    Write-Host ("{0,-24} {1,6} {2,4} {3,6} {4,5}" -f "case", "LUT", "NOT", "LC", "DFF")
}

$sum = 0.0
foreach ($case in $files) {
    $top = $case.BaseName
    Copy-Item $case.FullName (Join-Path $tmp ($top + ".v")) -Force
    $opt = Get-LC -Exe $Yosys -Top $top -Dir $tmp
    if ($hasBase) {
        $bas = Get-LC -Exe $Baseline -Top $top -Dir $tmp
        $rate = 100.0 * ($bas[0] - $opt[0]) / $bas[0]
        $sum += $rate
        Write-Host ("{0,-24} {1,8} {2,8} {3,7:F2}% {4,5}" -f $top, $bas[0], $opt[0], $rate, $opt[3])
    } else {
        Write-Host ("{0,-24} {1,6} {2,4} {3,6} {4,5}" -f $top, $opt[1], $opt[2], $opt[0], $opt[3])
    }
}

Write-Host ""
if ($hasBase) {
    Write-Host ("average reduction over {0} cases: {1:F2}%" -f $files.Count, ($sum / $files.Count))
} else {
    Write-Host "note: pass -Baseline <original yosys.exe> to also print the reduction rate"
}
