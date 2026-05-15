param(
  [Parameter(Mandatory = $true)]
  [string]$CloudflaredPath
)

$ErrorActionPreference = "Stop"

$proxyPort = if ($env:PROXY_PORT) { $env:PROXY_PORT } else { "8080" }
$localUrl = "http://localhost:$proxyPort"
$token = if ($env:PROXY_TOKEN) { $env:PROXY_TOKEN } else { "" }
$logPath = Join-Path $PSScriptRoot "cloudflared-last.log"

Write-Host "Starting local PC proxy on $localUrl ..."
$node = Start-Process -FilePath "node" `
  -ArgumentList "server.js" `
  -WorkingDirectory $PSScriptRoot `
  -WindowStyle Minimized `
  -PassThru

try {
  Start-Sleep -Seconds 2
  Write-Host ""
  Write-Host "Starting Cloudflare Tunnel..."
  Write-Host "Waiting for the real public URL..."
  Write-Host ""

  if (Test-Path -LiteralPath $logPath) {
    Remove-Item -LiteralPath $logPath -Force
  }

  $cloudflared = Start-Process -FilePath $CloudflaredPath `
    -ArgumentList @("tunnel", "--no-autoupdate", "--url", $localUrl, "--logfile", $logPath, "--loglevel", "info") `
    -WorkingDirectory $PSScriptRoot `
    -WindowStyle Minimized `
    -PassThru

  $publicUrl = $null
  for ($i = 0; $i -lt 60 -and -not $publicUrl; $i++) {
    Start-Sleep -Seconds 1
    if ($cloudflared.HasExited) {
      throw "cloudflared stopped before creating a public URL. Check $logPath"
    }
    if (Test-Path -LiteralPath $logPath) {
      $log = Get-Content -LiteralPath $logPath -Raw -ErrorAction SilentlyContinue
      $match = [regex]::Match($log, "https://[-a-zA-Z0-9]+\.trycloudflare\.com")
      if ($match.Success) {
        $publicUrl = $match.Value
      }
    }
  }

  if (-not $publicUrl) {
    throw "Could not find a trycloudflare.com URL in $logPath"
  }

  $openUrl = if ($token) { "$publicUrl/?token=$token" } else { "$publicUrl/" }

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
  if ($cloudflared -and -not $cloudflared.HasExited) {
    Stop-Process -Id $cloudflared.Id -Force -ErrorAction SilentlyContinue
  }
  if ($node -and -not $node.HasExited) {
    Stop-Process -Id $node.Id -Force -ErrorAction SilentlyContinue
  }
}
