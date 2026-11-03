param(
    [Parameter(Mandatory=$true)][string]$Baseline,
    [Parameter(Mandatory=$true)][string]$Optimized,
    [Parameter(Mandatory=$true)][string]$Cases,
    [int]$Reps = 5,
    [int]$Warmup = 10
)

# Wall-clock / peak-memory / logic-cell comparison following the official protocol:
# warm up both binaries, then alternate baseline -> optimized, report medians.
# LC = cycloneiv_lcell_comb + $not (from the stat block).
# ASCII-only on purpose: PowerShell 5.1 mis-parses UTF-8 without BOM.

$ErrorActionPreference = 'Continue'

# A missing baseline must not hang: fall back to a logic-cell-only table.
if (-not (Test-Path $Baseline)) {
    Write-Host ("[warn] baseline not found: " + $Baseline)
    Write-Host "[warn] falling back to the logic-cell-only table"
    & (Join-Path $PSScriptRoot 'run_cases.ps1') -Yosys $Optimized -Cases $Cases
    exit $LASTEXITCODE
}
if (-not (Test-Path $Optimized)) {
    Write-Host ("[error] optimized yosys not found: " + $Optimized)
    exit 1
}

. (Join-Path $PSScriptRoot '_tmpfix.ps1')
$tmp = Get-WorkTemp -Root (Split-Path -Parent $PSScriptRoot)
$env:TEMP = $tmp
$env:TMP = $tmp

function Run-One {
    param([string]$Exe, [string]$Ys, [string]$Log)
    # yosys prints "Warning: Feature 'synth_intel' is experimental." on stderr;
    # swallow it so the console only shows the result table.
    $errFile = $Log + ".stderr"
    $p = Start-Process -FilePath $Exe -ArgumentList @('-q','-s',$Ys,'-l',$Log) -PassThru -NoNewWindow `
        -RedirectStandardOutput ($Log + ".stdout") -RedirectStandardError $errFile
    if (-not $p) { return 0 }
    $peak = 0
    while ($true) {
        if ($p.HasExited) { break }
        try {
            $p.Refresh()
            if ($p.PeakWorkingSet64 -gt $peak) { $peak = $p.PeakWorkingSet64 }
        } catch { }
        Start-Sleep -Milliseconds 10
    }
    return $peak
}

function Get-LC {
    param([string]$Log)
    $lut = 0
    $inv = 0
    foreach ($line in Get-Content $Log) {
        if ($line -match '^\s+(\d+)\s+cycloneiv_lcell_comb\s*$') { $lut = [int]$Matches[1] }
        if ($line -match '^\s+(\d+)\s+\$not\s*$') { $inv = [int]$Matches[1] }
    }
    return ($lut + $inv)
}

function Median {
    param($Values)
    $s = @($Values | Sort-Object)
    return $s[[int]([math]::Floor($s.Count / 2))]
}

$files = @(Get-ChildItem -Path $Cases -Filter *.v)
if ($files.Count -eq 0) { Write-Host "no .v files under $Cases"; exit 1 }
$sum = 0.0

foreach ($case in $files) {
    $top = $case.BaseName
    $ys = Join-Path $tmp ("bench_" + $top + ".ys")
    $body = @()
    $body += "read_verilog " + $case.FullName
    $body += ""
    $body += "synth_intel -family cycloneiv -top " + $top
    $body += "stat"
    Set-Content -Path $ys -Value $body -Encoding ASCII

    $warmLogB = Join-Path $tmp "warm_b.log"
    $warmLogO = Join-Path $tmp "warm_o.log"
    for ($i = 0; $i -lt $Warmup; $i++) {
        Run-One -Exe $Baseline -Ys $ys -Log $warmLogB | Out-Null
        Run-One -Exe $Optimized -Ys $ys -Log $warmLogO | Out-Null
    }

    $timesB = @(); $timesO = @(); $memsB = @(); $memsO = @()
    for ($i = 0; $i -lt $Reps; $i++) {
        $logB = Join-Path $tmp "rep_b.log"
        $logO = Join-Path $tmp "rep_o.log"

        $sw = [Diagnostics.Stopwatch]::StartNew()
        $peekB = Run-One -Exe $Baseline -Ys $ys -Log $logB
        $sw.Stop()
        $timesB += $sw.Elapsed.TotalMilliseconds
        $memsB += ($peekB / 1MB)

        $sw = [Diagnostics.Stopwatch]::StartNew()
        $peekO = Run-One -Exe $Optimized -Ys $ys -Log $logO
        $sw.Stop()
        $timesO += $sw.Elapsed.TotalMilliseconds
        $memsO += ($peekO / 1MB)
    }

    $lcB = Get-LC -Log (Join-Path $tmp "rep_b.log")
    $lcO = Get-LC -Log (Join-Path $tmp "rep_o.log")
    $tB = Median $timesB; $tO = Median $timesO
    $mB = Median $memsB; $mO = Median $memsO
    $red = 100.0 * ($lcB - $lcO) / $lcB
    $td = 100.0 * ($tO - $tB) / $tB
    $md = 100.0 * ($mO - $mB) / $mB

    Write-Host ("{0,-24} LC {1,5} -> {2,5} ({3,6:F2}%)  time {4,7:F1} -> {5,7:F1} ms ({6,6:F2}%)  mem {7,6:F1} -> {8,6:F1} MB ({9,6:F2}%)" -f `
        $top, $lcB, $lcO, $red, $tB, $tO, $td, $mB, $mO, $md)
    $sum = $sum + $red
    Remove-Item $ys -Force -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host ("average logic-cell reduction over {0} cases: {1:F2}%" -f $files.Count, ($sum / $files.Count))
