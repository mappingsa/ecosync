# =========================================================================
#  MSA EcoSync - make a shareable copy of a unit's config.txt
#
#  config.txt holds the Wi-Fi password, the ArcGIS token and the device login.
#  This writes config_redacted.txt with those values replaced by a note saying
#  only whether they are set and how long they are - which is all anyone needs
#  to debug a config, and nothing anyone needs to steal one.
#
#  Drag config.txt onto redact_config.bat, or:
#      powershell -ExecutionPolicy Bypass -File redact_config.ps1 E:\config.txt
# =========================================================================

param([string]$Path)

$ErrorActionPreference = 'Stop'

$scriptDir = $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($scriptDir)) { $scriptDir = (Get-Location).Path }

if ([string]::IsNullOrWhiteSpace($Path)) {
    $Path = Join-Path $scriptDir 'config.txt'
}

if (-not (Test-Path -LiteralPath $Path)) {
    Write-Host "Cannot find: $Path" -ForegroundColor Red
    Write-Host ""
    Write-Host "Drag the SD card's config.txt onto redact_config.bat,"
    Write-Host "or pass its path:  redact_config.bat E:\config.txt"
    exit 1
}

# Anything whose VALUE is a secret. Matched on the key, case-insensitively.
$secretKeys = @('wifi_pass', 'ap_pass', 'esri_token', 'admin_pass')

$out     = New-Object System.Collections.Generic.List[string]
$masked  = 0
$lineNo  = 0

foreach ($line in (Get-Content -LiteralPath $Path)) {
    $lineNo++
    $trimmed = $line.Trim()

    # Comments and blanks pass through untouched - the comments are half of
    # what makes a config readable.
    if ($trimmed -eq '' -or $trimmed.StartsWith('#')) {
        $out.Add($line)
        continue
    }

    $eq = $line.IndexOf('=')
    if ($eq -lt 1) { $out.Add($line); continue }

    $key = $line.Substring(0, $eq).Trim()
    $val = $line.Substring($eq + 1)

    if ($secretKeys -contains $key.ToLower()) {
        if ($val.Trim().Length -eq 0) {
            $out.Add("$key=<BLANK - not set>")
        } else {
            $out.Add("$key=<REDACTED, $($val.Trim().Length) characters>")
        }
        $masked++
    } else {
        $out.Add($line)
    }
}

$outFile = Join-Path $scriptDir 'config_redacted.txt'
$header = @(
    "# Redacted copy of: $Path"
    "# Generated: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
    "# $masked secret value(s) replaced. Everything else is verbatim."
    "#"
)
Set-Content -LiteralPath $outFile -Value ($header + $out) -Encoding ascii

Write-Host ""
Write-Host "Read   : $Path"
Write-Host "Lines  : $lineNo"
Write-Host "Masked : $masked"
Write-Host "Written: $outFile"
Write-Host ""
Write-Host "Open it and check nothing sensitive survived before sending it."
