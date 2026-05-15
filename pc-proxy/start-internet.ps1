param(
  [Parameter(Mandatory = $true)]
  [string]$CloudflaredPath
)

$ErrorActionPreference = "Stop"

$proxyPort = if ($env:PROXY_PORT) { $env:PROXY_PORT } else { "8080" }
$localUrl = "http://localhost:$proxyPort"
$token = if ($env:PROXY_TOKEN) { $env:PROXY_TOKEN } else { "" }
$logPath = Join-Path $PSScriptRoot "cloudflared-last.log"

function Append-CloudflaredLog {
  param([string]$Line)
  if ([string]::IsNullOrWhiteSpace($Line)) {
    return
  }
  Add-Content -LiteralPath $logPath -Value $Line
  Write-Host $Line
  $match = [regex]::Match($Line, "https://[-a-zA-Z0-9]+\.trycloudflare\.com")
  if ($match.Success -and -not $script:PublicUrl) {
    $script:PublicUrl = $match.Value
  }
}

$script:PublicUrl = $null

Write-Host "Starting local PC proxy on $localUrl ..."
$node = Start-Process -FilePath "node" `
  -ArgumentList "server.js" `
  -WorkingDirectory $PSScriptRoot `
  -WindowStyle Minimized `
  -PassThru

try {
  Start-Sleep -Seconds 2

  if (Test-Path -LiteralPath $logPath) {
    Remove-Item -LiteralPath $logPath -Force
  }
  New-Item -Path $logPath -ItemType File -Force | Out-Null

  Write-Host ""
  Write-Host "Starting Cloudflare Tunnel..."
  Write-Host "Using HTTP/2 protocol because it works on more networks than QUIC."
  Write-Host "Waiting for the real public URL..."
  Write-Host ""

  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = $CloudflaredPath
  $psi.WorkingDirectory = $PSScriptRoot
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $psi.CreateNoWindow = $true
  $psi.Arguments = "tunnel --no-autoupdate --protocol http2 --url $localUrl --loglevel info"

  $cloudflared = New-Object System.Diagnostics.Process
  $cloudflared.StartInfo = $psi

  $outputHandler = {
    if ($EventArgs.Data) {
      Append-CloudflaredLog $EventArgs.Data
    }
  }
  $errorHandler = {
    if ($EventArgs.Data) {
      Append-CloudflaredLog $EventArgs.Data
    }
  }

  $outEvent = Register-ObjectEvent -InputObject $cloudflared -EventName OutputDataReceived -Action $outputHandler
  $errEvent = Register-ObjectEvent -InputObject $cloudflared -EventName ErrorDataReceived -Action $errorHandler

  if (-not $cloudflared.Start()) {
    throw "Could not start cloudflared."
  }
  $cloudflared.BeginOutputReadLine()
  $cloudflared.BeginErrorReadLine()

  for ($i = 0; $i -lt 90 -and -not $script:PublicUrl; $i++) {
    Start-Sleep -Seconds 1
    if ($cloudflared.HasExited) {
      throw "cloudflared stopped before creating a public URL. Check $logPath"
    }
  }

  if (-not $script:PublicUrl) {
    throw "Could not find a trycloudflare.com URL in $logPath"
  }

  $openUrl = if ($token) { "$($script:PublicUrl)/?token=$token" } else { "$($script:PublicUrl)/" }

  Write-Host ""
  Write-Host "INTERNET URL:"
  Write-Host $openUrl
  Write-Host ""
  Write-Host "Open this exact URL from another network, for example mobile internet."
  Write-Host "Do not close this window while streaming."
  Write-Host ""

  Start-Process $openUrl

  while (-not $cloudflared.HasExited) {
    Start-Sleep -Seconds 2
  }
} finally {
  if ($outEvent) {
    Unregister-Event -SubscriptionId $outEvent.Id -ErrorAction SilentlyContinue
  }
  if ($errEvent) {
    Unregister-Event -SubscriptionId $errEvent.Id -ErrorAction SilentlyContinue
  }
  if ($cloudflared -and -not $cloudflared.HasExited) {
    Stop-Process -Id $cloudflared.Id -Force -ErrorAction SilentlyContinue
  }
  if ($node -and -not $node.HasExited) {
    Stop-Process -Id $node.Id -Force -ErrorAction SilentlyContinue
  }
}
