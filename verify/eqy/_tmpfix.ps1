# Shared helper: pick a writable scratch directory for yosys logs and abc9 temp files.
# A package extracted into a low-integrity folder makes the system %TEMP% unwritable for
# the shipped exe, so prefer "<package>\_tmp" and fall back only if that fails as well.
function Get-WorkTemp {
    param([Parameter(Mandatory=$true)][string]$Root)
    $cands = @((Join-Path $Root '_tmp'), $env:TEMP, (Get-Location).Path)
    foreach ($c in $cands) {
        if (-not $c) { continue }
        try {
            if (-not (Test-Path $c)) { New-Item -ItemType Directory -Force -Path $c | Out-Null }
            $probe = Join-Path $c ("probe_" + [guid]::NewGuid().ToString('N') + ".tmp")
            Set-Content -Path $probe -Value 'ok' -ErrorAction Stop
            $tmp = Join-Path $c ("run_" + [guid]::NewGuid().ToString('N') + ".tmp")
            Set-Content -Path $tmp -Value 'ok' -ErrorAction Stop
            Remove-Item $probe, $tmp -Force -ErrorAction SilentlyContinue
            return (Resolve-Path $c).Path
        } catch { }
    }
    return (Get-Location).Path
}
