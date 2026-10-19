# =========================================================================
#  MSA EcoSync - fetch the root CA certificate for the ArcGIS feature service
#  and write it as ca.pem for the unit's SD card.
#
#  The ESP32 loads /ca.pem at boot and uses it to VERIFY that the server it
#  is uploading to is really Esri. With no ca.pem the firmware falls back to
#  client.setInsecure() - still encrypted, but nothing checks who is on the
#  other end, and the ArcGIS API token is in that request.
#
#  Run it any of these ways:
#
#      powershell -ExecutionPolicy Bypass -File get_ca_pem.ps1
#      pwsh -File get_ca_pem.ps1
#      right-click the file -> Run with PowerShell
#
#  It prints the certificate chain it found and writes ca.pem into the same
#  folder. If anything fails it prints the exception type and message - send
#  that text rather than "it fell over" and it can be fixed in one go.
# =========================================================================

$ErrorActionPreference = 'Stop'

$hostName = 'services2.arcgis.com'

# $PSScriptRoot is empty when the body is pasted into a console rather than
# run as a file, which made Join-Path throw before anything useful happened.
$scriptDir = $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($scriptDir)) { $scriptDir = (Get-Location).Path }
$outFile = Join-Path $scriptDir 'ca.pem'
$logFile = Join-Path $scriptDir 'get_ca_pem_log.txt'

# Everything to a log as well as the screen. Launched by double-click the
# window closes the instant this returns, and the error goes with it.
$transcribing = $false
try {
    Start-Transcript -Path $logFile -Force | Out-Null
    $transcribing = $true
} catch {
    Write-Host "(could not start a log file - screen output only)"
}

Write-Host "PowerShell     : $($PSVersionTable.PSVersion)  ($($PSVersionTable.PSEdition))"
Write-Host "Writing to     : $outFile"
Write-Host ""

try {
    Write-Host "Connecting to $hostName ..."

    $tcp = New-Object System.Net.Sockets.TcpClient
    $tcp.Connect($hostName, 443)
    $ssl = New-Object System.Net.Security.SslStream($tcp.GetStream(), $false)

    try {
        # Explicit TLS 1.2 + 1.3 rather than "system default", which on an
        # older Windows PowerShell can still negotiate down and fail.
        $protocols = [System.Security.Authentication.SslProtocols]::Tls12
        try {
            $protocols = $protocols -bor [System.Security.Authentication.SslProtocols]::Tls13
        } catch {
            # Tls13 is not defined on older .NET Framework. 1.2 alone is fine.
        }

        # No certificate-override callback on purpose: this validates against
        # the Windows trust store. If it throws, something is intercepting the
        # connection, and whatever it hands back is not worth pinning.
        $ssl.AuthenticateAsClient($hostName, $null, $protocols, $false)

        $raw  = $ssl.RemoteCertificate
        if ($null -eq $raw) { throw "The server did not present a certificate." }
        $leaf = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 (,$raw.GetRawCertData())
    }
    finally {
        if ($ssl) { $ssl.Dispose() }
        if ($tcp) { $tcp.Close() }
    }

    $chain = New-Object System.Security.Cryptography.X509Certificates.X509Chain
    $chain.ChainPolicy.RevocationMode = [System.Security.Cryptography.X509Certificates.X509RevocationMode]::NoCheck
    $built = $chain.Build($leaf)

    if (-not $built) {
        Write-Host "WARNING: the chain did not build cleanly:" -ForegroundColor Yellow
        foreach ($s in $chain.ChainStatus) {
            Write-Host ("  " + $s.Status + " - " + $s.StatusInformation.Trim())
        }
        Write-Host ""
    }

    $count = $chain.ChainElements.Count
    if ($count -lt 1) { throw "The certificate chain came back empty." }

    Write-Host "Certificate chain:"
    for ($i = 0; $i -lt $count; $i++) {
        $c = $chain.ChainElements[$i].Certificate
        if ($i -eq 0) { $tag = 'leaf        ' }
        elseif ($i -eq ($count - 1)) { $tag = 'ROOT        ' }
        else { $tag = 'intermediate' }
        Write-Host ("  [$i] $tag " + $c.Subject)
    }

    $root = $chain.ChainElements[$count - 1].Certificate

    if ($root.Subject -ne $root.Issuer) {
        Write-Host ""
        Write-Host "WARNING: the top of the chain is not self-signed, so it is not a root." -ForegroundColor Yellow
        Write-Host "         Pinning it may still work. Check before relying on it."
    }

    # PEM: base64 of the DER bytes, wrapped at 76 characters, ASCII, no BOM.
    # The firmware searches the file for the literal text BEGIN CERTIFICATE,
    # which a UTF-16 or BOM-prefixed file fails - it warns and falls back to
    # insecure, so the encoding here is load-bearing.
    $opt  = [System.Base64FormattingOptions]::InsertLineBreaks
    $body = [System.Convert]::ToBase64String($root.RawData, $opt)
    $pem  = "-----BEGIN CERTIFICATE-----`r`n" + $body + "`r`n-----END CERTIFICATE-----`r`n"

    [System.IO.File]::WriteAllText($outFile, $pem, (New-Object System.Text.ASCIIEncoding))

    $size = (Get-Item $outFile).Length
    Write-Host ""
    Write-Host "Root written   : $outFile"
    Write-Host ("Subject        : " + $root.Subject)
    Write-Host ("Valid until    : {0:yyyy-MM-dd}" -f $root.NotAfter)
    Write-Host ("Size           : $size bytes")
    Write-Host ""
    Write-Host "Copy ca.pem to the ROOT of each unit's SD card."
    Write-Host "At boot the unit should say: TLS root certificate loaded from SD ($size bytes)."
}
catch {
    Write-Host ""
    Write-Host "FAILED" -ForegroundColor Red
    Write-Host ("  type    : " + $_.Exception.GetType().FullName)
    Write-Host ("  message : " + $_.Exception.Message)
    if ($_.Exception.InnerException) {
        Write-Host ("  inner   : " + $_.Exception.InnerException.Message)
    }
    Write-Host ("  at line : " + $_.InvocationInfo.ScriptLineNumber + "  " + $_.InvocationInfo.Line.Trim())
    Write-Host ""
    Write-Host "Send that block and this can be fixed properly."
    Write-Host "It is also saved in get_ca_pem_log.txt beside this script."
    if ($transcribing) { try { Stop-Transcript | Out-Null } catch { } }
    exit 1
}

if ($transcribing) { try { Stop-Transcript | Out-Null } catch { } }
