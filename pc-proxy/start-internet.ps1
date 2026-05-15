param(
  [Parameter(Mandatory = $true)]
  [string]$CloudflaredPath
)

$ErrorActionPreference = "Stop"

$proxyPort = if ($env:PROXY_PORT) { $env:PROXY_PORT } else { "8080" }
$localUrl = "http://localhost:$proxyPort"
$token = if ($env:PROXY_TOKEN) { $env:PROXY_TOKEN } else { "" }
$logPath = Join-Path $PSScriptRoot "internet-tunnel-last.log"
$toolsPath = Join-Path $PSScriptRoot "tools"

function Ensure-Ngrok {
  if (-not (Test-Path -LiteralPath $toolsPath)) {
    New-Item -Path $toolsPath -ItemType Directory -Force | Out-Null
  }

  $ngrokExe = Join-Path $toolsPath "ngrok.exe"
  if (Test-Path -LiteralPath $ngrokExe) {
    return $ngrokExe
  }

  $zipPath = Join-Path $toolsPath "ngrok.zip"
  Write-Host "Downloading ngrok..."
  Invoke-WebRequest `
    -Uri "https://bin.equinox.io/c/bNyj1mQVY4c/ngrok-v3-stable-windows-amd64.zip" `
    -OutFile $zipPath
  Expand-Archive -LiteralPath $zipPath -DestinationPath $toolsPath -Force
  Remove-Item -LiteralPath $zipPath -Force -ErrorAction SilentlyContinue

  if (-not (Test-Path -LiteralPath $ngrokExe)) {
    throw "ngrok.exe was not found after extracting $zipPath"
  }
  return $ngrokExe
}

function Stop-ExistingNgrokProcesses {
  $existing = Get-Process -Name "ngrok" -ErrorAction SilentlyContinue
  if (-not $existing) {
    return
  }

  Write-Host "Stopping previous ngrok processes..."
  foreach ($process in $existing) {
    try {
      Stop-Process -Id $process.Id -Force -ErrorAction Stop
    } catch {
      Write-Host "Could not stop ngrok PID $($process.Id): $($_.Exception.Message)"
    }
  }
  Start-Sleep -Seconds 3
}

function Stop-ExistingProxyProcesses {
  $projectPath = [System.IO.Path]::GetFullPath($PSScriptRoot).TrimEnd("\")
  $processes = Get-CimInstance Win32_Process -Filter "name = 'node.exe'" -ErrorAction SilentlyContinue
  if (-not $processes) {
    return
  }

  $matched = @()
  foreach ($process in $processes) {
    $commandLine = [string]$process.CommandLine
    if ($commandLine -match "(^|[\\/\s])server\.js(\s|$)" -or $commandLine.Contains($projectPath)) {
      $matched += $process
    }
  }

  if (-not $matched) {
    return
  }

  Write-Host "Stopping previous local proxy processes..."
  foreach ($process in $matched) {
    try {
      Stop-Process -Id $process.ProcessId -Force -ErrorAction Stop
    } catch {
      Write-Host "Could not stop node PID $($process.ProcessId): $($_.Exception.Message)"
    }
  }
  Start-Sleep -Seconds 2
}

function Write-LogLine {
  param([string]$Line)
  if ([string]::IsNullOrWhiteSpace($Line)) {
    return
  }
  Add-Content -LiteralPath $logPath -Value $Line
  Write-Host $Line
}

function Stop-TunnelProcess {
  param($Tunnel)
  if ($Tunnel -and $Tunnel.Process -and -not $Tunnel.Process.HasExited) {
    Stop-Process -Id $Tunnel.Process.Id -Force -ErrorAction SilentlyContinue
  }
  if ($Tunnel -and $Tunnel.OutputEvent) {
    Unregister-Event -SubscriptionId $Tunnel.OutputEvent.Id -ErrorAction SilentlyContinue
  }
  if ($Tunnel -and $Tunnel.ErrorEvent) {
    Unregister-Event -SubscriptionId $Tunnel.ErrorEvent.Id -ErrorAction SilentlyContinue
  }
}

function Start-TunnelProcess {
  param(
    [string]$Name,
    [string]$FileName,
    [string]$Arguments,
    [string]$UrlRegex,
    [int]$TimeoutSeconds
  )

  Write-Host ""
  Write-Host "Starting $Name..."
  Write-Host ""

  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = $FileName
  $psi.Arguments = $Arguments
  $psi.WorkingDirectory = $PSScriptRoot
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $psi.CreateNoWindow = $true

  $process = New-Object System.Diagnostics.Process
  $process.StartInfo = $psi

  $handler = {
    if ($EventArgs.Data) {
      Add-Content -LiteralPath $Event.MessageData.LogPath -Value $EventArgs.Data
      Write-Host $EventArgs.Data
    }
  }

  $outEvent = Register-ObjectEvent -InputObject $process -EventName OutputDataReceived -MessageData @{ LogPath = $logPath } -Action $handler
  $errEvent = Register-ObjectEvent -InputObject $process -EventName ErrorDataReceived -MessageData @{ LogPath = $logPath } -Action $handler

  if (-not $process.Start()) {
    throw "Could not start $Name."
  }
  $process.BeginOutputReadLine()
  $process.BeginErrorReadLine()

  $publicUrl = $null
  for ($i = 0; $i -lt $TimeoutSeconds -and -not $publicUrl; $i++) {
    Start-Sleep -Seconds 1
    if (Test-Path -LiteralPath $logPath) {
      $log = Get-Content -LiteralPath $logPath -Raw -ErrorAction SilentlyContinue
      $match = [regex]::Match($log, $UrlRegex)
      if ($match.Success) {
        $publicUrl = $match.Value
      }
    }
    if ($process.HasExited -and -not $publicUrl) {
      break
    }
  }

  [pscustomobject]@{
    Name = $Name
    Process = $process
    OutputEvent = $outEvent
    ErrorEvent = $errEvent
    PublicUrl = $publicUrl
    ExitCode = if ($process.HasExited) { $process.ExitCode } else { $null }
  }
}

function Show-PublicUrlAndWait {
  param($Tunnel)

  $openUrl = if ($token) { "$($Tunnel.PublicUrl)/?token=$([uri]::EscapeDataString($token))" } else { "$($Tunnel.PublicUrl)/" }

  Write-Host ""
  Write-Host "INTERNET URL:"
  Write-Host $openUrl
  Write-Host ""
  Write-Host "Open this exact URL from another network, for example mobile internet."
  Write-Host "Do not close this window while streaming."
  Write-Host ""

  Start-Process $openUrl

  while (-not $Tunnel.Process.HasExited) {
    Start-Sleep -Seconds 2
  }
}

Stop-ExistingProxyProcesses

Write-Host "Starting local PC proxy on $localUrl ..."
$node = Start-Process -FilePath "node" `
  -ArgumentList "server.js" `
  -WorkingDirectory $PSScriptRoot `
  -WindowStyle Minimized `
  -PassThru

$activeTunnel = $null

try {
  Start-Sleep -Seconds 2

  if (Test-Path -LiteralPath $logPath) {
    Remove-Item -LiteralPath $logPath -Force
  }
  New-Item -Path $logPath -ItemType File -Force | Out-Null

  Write-Host ""
  Write-Host "Trying ngrok first..."
  $ngrokExe = Ensure-Ngrok
  Stop-ExistingNgrokProcesses
  if ($env:NGROK_AUTHTOKEN) {
    & $ngrokExe config add-authtoken $env:NGROK_AUTHTOKEN | ForEach-Object { Write-LogLine $_ }
  } else {
    Write-Host "No NGROK_AUTHTOKEN environment variable was provided."
    Write-Host "Using saved ngrok config if it exists."
  }

  $activeTunnel = Start-TunnelProcess `
    -Name "ngrok" `
    -FileName $ngrokExe `
    -Arguments "http $proxyPort --log stdout" `
    -UrlRegex "https://[-a-zA-Z0-9.]+\.ngrok[-a-zA-Z0-9.]*\.(app|io|dev)" `
    -TimeoutSeconds 45

  if ($activeTunnel.PublicUrl) {
    Show-PublicUrlAndWait $activeTunnel
    return
  }

  Write-Host ""
  Write-Host "ngrok did not create a public URL."
  Stop-TunnelProcess $activeTunnel
  $activeTunnel = $null

  Write-Host ""
  Write-Host "Trying Cloudflare Tunnel..."
  Write-Host "If Cloudflare is blocked or times out, localtunnel will be tried automatically."

  $cloudflareArgs = "tunnel --no-autoupdate --protocol http2 --url $localUrl --loglevel info"
  $activeTunnel = Start-TunnelProcess `
    -Name "Cloudflare Tunnel" `
    -FileName $CloudflaredPath `
    -Arguments $cloudflareArgs `
    -UrlRegex "https://(?!api\.)[-a-zA-Z0-9]+\.trycloudflare\.com" `
    -TimeoutSeconds 35

  if ($activeTunnel.PublicUrl) {
    Show-PublicUrlAndWait $activeTunnel
    return
  }

  Write-Host ""
  Write-Host "Cloudflare did not create a public URL."
  Write-Host "Trying localtunnel fallback through npx..."
  Stop-TunnelProcess $activeTunnel
  $activeTunnel = $null

  $npx = Get-Command "npx.cmd" -ErrorAction SilentlyContinue
  if (-not $npx) {
    $npx = Get-Command "npx" -ErrorAction SilentlyContinue
  }
  if (-not $npx) {
    throw "npx was not found. Reinstall Node.js LTS with npm included."
  }

  $localtunnelArgs = "--yes localtunnel --port $proxyPort --local-host localhost"
  $activeTunnel = Start-TunnelProcess `
    -Name "localtunnel" `
    -FileName $npx.Source `
    -Arguments $localtunnelArgs `
    -UrlRegex "https://[-a-zA-Z0-9]+\.loca\.lt" `
    -TimeoutSeconds 60

  if ($activeTunnel.PublicUrl) {
    Write-Host ""
    Write-Host "Note: localtunnel may show a visitor-password page on first open."
    Write-Host "If it asks for a password, follow the instruction shown on that page."
    Show-PublicUrlAndWait $activeTunnel
    return
  }

  throw "No internet tunnel could be created. Check $logPath"
} finally {
  Stop-TunnelProcess $activeTunnel
  if ($node -and -not $node.HasExited) {
    Stop-Process -Id $node.Id -Force -ErrorAction SilentlyContinue
  }
}
