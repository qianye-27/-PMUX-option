param(
    [Parameter(Mandatory=$true)][string]$Yosys,
    [Parameter(Mandatory=$true)][string]$Case,
    [string]$Top = "",
    [string]$Tag = ""
)

# Print one line:  <tag>  LUT=x  NOT=y  LC=z  DFF=d
# Usage: .\show_lc.ps1 -Yosys ..\yosys-baseline\yosys-baseline.exe -Case demo\test1.v -Tag "original"
# ASCII-only on purpose (PowerShell 5.1 mis-parses UTF-8 without BOM).

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '_tmpfix.ps1')
$tmp = Get-WorkTemp -Root (Split-Path -Parent $PSScriptRoot)
$env:TEMP = $tmp
$env:TMP = $tmp
if ($Top -eq "") { $Top = [IO.Path]::GetFileNameWithoutExtension($Case) }

$ys = Join-Path $tmp ("showlc_" + $Top + ".ys")
$lines = @()
$lines += "read_verilog " + $Case
$lines += "synth_intel -family cycloneiv -top " + $Top
$lines += "stat"
Set-Content -Path $ys -Value $lines -Encoding ASCII

$log = Join-Path $tmp ("showlc_" + $Top + ".log")
$errFile = $log + ".stderr"
$p = Start-Process -FilePath $Yosys -ArgumentList @('-q','-s',$ys,'-l',$log) -PassThru -NoNewWindow `
    -RedirectStandardOutput ($log + ".stdout") -RedirectStandardError $errFile -Wait

$lut = 0; $inv = 0; $dff = 0
foreach ($line in Get-Content $log) {
    if ($line -match '^\s+(\d+)\s+cycloneiv_lcell_comb\s*$') { $lut = [int]$Matches[1] }
    if ($line -match '^\s+(\d+)\s+\$not\s*$') { $inv = [int]$Matches[1] }
    if ($line -match '^\s+(\d+)\s+dffeas\s*$') { $dff = [int]$Matches[1] }
}
$lc = $lut + $inv
if ($Tag -ne "") { Write-Host ("{0,-10} LUT={1,5}  NOT={2,3}  LC={3,5}  DFF={4,4}" -f $Tag, $lut, $inv, $lc, $dff) }
else { Write-Host ("LUT={0}  NOT={1}  LC={2}  DFF={3}" -f $lut, $inv, $lc, $dff) }
