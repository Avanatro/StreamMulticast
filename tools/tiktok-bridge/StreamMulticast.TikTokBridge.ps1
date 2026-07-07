<#
.SYNOPSIS
Writes local TikTok RTMP handoff data for StreamMulticast.

.DESCRIPTION
This companion does not generate TikTok stream keys and does not automate
TikTok or third-party login flows. It only writes RTMP server/key data supplied
by the user, clipboard, or another local helper into StreamMulticast's bridge
JSON handoff file.
#>

[CmdletBinding()]
param(
    [string]$Name = "TikTok Bridge",
    [string]$ServerUrl,
    [string]$StreamKey,
    [string]$ExpiresAt,
    [string]$OutputPath,
    [switch]$FromClipboard,
    [string]$HelperPath,
    [string]$HelperArgs,
    [switch]$WaitForHelper
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-DefaultBridgePath {
    if (-not $env:APPDATA) {
        throw "APPDATA is not set; pass -OutputPath explicitly."
    }
    Join-Path $env:APPDATA "obs-studio\plugin_config\streammulticast\tiktok_bridge.json"
}

function Read-SecretText([string]$Prompt) {
    $secure = Read-Host -Prompt $Prompt -AsSecureString
    $bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
    try {
        [Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr)
    } finally {
        [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr)
    }
}

function Import-ClipboardBridgeData {
    $raw = Get-Clipboard -Raw
    if (-not $raw) {
        throw "Clipboard is empty."
    }

    try {
        $json = $raw | ConvertFrom-Json
        return @{
            Name = if ($json.name) { [string]$json.name } else { $null }
            ServerUrl = if ($json.server_url) { [string]$json.server_url } elseif ($json.server) { [string]$json.server } else { $null }
            StreamKey = if ($json.stream_key) { [string]$json.stream_key } elseif ($json.key) { [string]$json.key } else { $null }
            ExpiresAt = if ($json.expires_at) { [string]$json.expires_at } else { $null }
        }
    } catch {
        $lines = $raw -split "`r?`n" | ForEach-Object { $_.Trim() } | Where-Object { $_ }
        $serverLine = $lines | Where-Object { $_ -match "^(rtmp|rtmps)://" } | Select-Object -First 1
        $keyLine = $lines | Where-Object { $_ -notmatch "^(rtmp|rtmps)://" -and $_ -notmatch "^(server|server_url|key|stream_key)\s*=" } | Select-Object -First 1

        foreach ($line in $lines) {
            if ($line -match "^(server|server_url)\s*=\s*(.+)$") { $serverLine = $Matches[2].Trim() }
            if ($line -match "^(key|stream_key)\s*=\s*(.+)$") { $keyLine = $Matches[2].Trim() }
        }

        return @{
            Name = $null
            ServerUrl = $serverLine
            StreamKey = $keyLine
            ExpiresAt = $null
        }
    }
}

if ($HelperPath) {
    if (-not (Test-Path -LiteralPath $HelperPath)) {
        throw "HelperPath does not exist: $HelperPath"
    }

    $startInfo = @{
        FilePath = $HelperPath
        WindowStyle = "Minimized"
        PassThru = $true
    }
    if ($HelperArgs) {
        $startInfo.ArgumentList = $HelperArgs
    }

    $helper = Start-Process @startInfo
    Write-Host "Started helper process PID $($helper.Id)."

    if ($WaitForHelper) {
        Write-Host "Waiting for helper to exit..."
        Wait-Process -Id $helper.Id
    }
}

if (-not $OutputPath) {
    $OutputPath = Get-DefaultBridgePath
}

if ($FromClipboard) {
    $clip = Import-ClipboardBridgeData
    if (-not $ServerUrl -and $clip.ServerUrl) { $ServerUrl = $clip.ServerUrl }
    if (-not $StreamKey -and $clip.StreamKey) { $StreamKey = $clip.StreamKey }
    if ($Name -eq "TikTok Bridge" -and $clip.Name) { $Name = $clip.Name }
    if (-not $ExpiresAt -and $clip.ExpiresAt) { $ExpiresAt = $clip.ExpiresAt }
}

if (-not $ServerUrl) {
    $ServerUrl = Read-Host -Prompt "TikTok RTMP server URL"
}
if (-not $StreamKey) {
    $StreamKey = Read-SecretText "TikTok stream key"
}

if (-not $ServerUrl -or -not $StreamKey) {
    throw "ServerUrl and StreamKey are required."
}

$payload = [ordered]@{
    name = $Name
    server_url = $ServerUrl
    stream_key = $StreamKey
}
if ($ExpiresAt) {
    $payload.expires_at = $ExpiresAt
}

$parent = Split-Path -Parent $OutputPath
if ($parent) {
    New-Item -ItemType Directory -Path $parent -Force | Out-Null
}

$payload | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $OutputPath -Encoding UTF8
Write-Host "Wrote TikTok Bridge handoff file: $OutputPath"
Write-Host "Open StreamMulticast endpoint settings and click 'Import TikTok Bridge'."
