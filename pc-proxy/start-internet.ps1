param(
  [Parameter(Mandatory = $true)]
  [string]$CloudflaredPath
)

$ErrorActionPreference = "Stop"

$proxyPort = if ($env:PROXY_PORT) { $env:PROXY_PORT } else { "8080" }
$localUrl = "http://localhost:$proxyPort"

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
  Write-Host "When the public URL appears, append: ?token=$env:PROXY_TOKEN"
  Write-Host ""
  & $CloudflaredPath tunnel --no-autoupdate --url $localUrl
} finally {
  if ($node -and -not $node.HasExited) {
    Stop-Process -Id $node.Id -Force -ErrorAction SilentlyContinue
  }
}
