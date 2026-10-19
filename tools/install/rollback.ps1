<#
.SYNOPSIS
  Restore a StreamMulticast backup made by install.ps1.

.DESCRIPTION
  Restores the newest backup (or -From <dir>) of the plugin dll, pdb, data dir and plugin config.
  Refuses while OBS runs (exit 2); needs write access to the OBS install dir (exit 3 with the
  elevated command otherwise). Before restoring, the CURRENT state is snapshotted to
  <BackupRoot>\streammulticast-prerollback-<ts>\ so a rollback is itself undoable with -From.
  Stream keys are never printed.

  Exit codes: 0 ok, 1 error, 2 OBS running, 3 not writable (elevation needed).
#>
[CmdletBinding()]
param(
    [string]$From,
    [switch]$DryRun,
    [string]$InstallDir,
    [string]$BackupRoot = 'E:\Backups',
    [string]$ConfigDir,
    [string]$ProcessName = 'obs64'
)

$ErrorActionPreference = 'Stop'
$BoundParams = $PSBoundParameters

function Fail([int]$Code, [string]$Message) {
    [Console]::Error.WriteLine($Message)
    Write-Host $Message
    exit $Code
}

function Test-DirWritable([string]$Dir) {
    $d = $Dir
    while ($d -and -not (Test-Path -LiteralPath $d -PathType Container)) {
        if (Test-Path -LiteralPath $d) { return $false }
        $parent = Split-Path -Parent $d
        if ($parent -eq $d) { return $false }
        $d = $parent
    }
    if (-not $d) { return $false }
    $probe = Join-Path $d ('.smulti-write-probe-' + [guid]::NewGuid().ToString('N'))
    try {
        [IO.File]::WriteAllText($probe, 'x')
        Remove-Item -LiteralPath $probe -Force
        return $true
    } catch {
        return $false
    }
}

function Test-IsAdmin {
    return ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# Read-only access check for -DryRun: never creates, writes or deletes anything.
function Test-WritableNoWrite([string]$Dir) {
    if (Test-IsAdmin) { return $true }
    $candidate = $null
    if (Test-Path -LiteralPath $Dir -PathType Container) {
        $candidate = Get-ChildItem -LiteralPath $Dir -File -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
    }
    if ($candidate) {
        try {
            $fs = [IO.File]::Open($candidate.FullName, 'Open', 'ReadWrite', 'ReadWrite')
            $fs.Close()
            return $true
        } catch { return $false }
    }
    foreach ($pf in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) {
        if ($pf -and $Dir.StartsWith($pf, [StringComparison]::OrdinalIgnoreCase)) { return $false }
    }
    return $true
}

function Get-FullPathTrim([string]$p) {
    $f = [IO.Path]::GetFullPath($p)
    if ($f.Length -gt 3) { $f = $f.TrimEnd([char]92) }
    return $f
}

function Get-ElevatedCommand {
    $pathParams = @('Source', 'From', 'InstallDir', 'BackupRoot', 'ConfigDir', 'ServiceFile', 'AitumFile')
    $parts = @('-NoExit', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"' + $PSCommandPath + '"'))
    foreach ($k in $BoundParams.Keys) {
        if ($k -eq 'DryRun') { continue }   # the elevated command is the real run
        $v = $BoundParams[$k]
        if ($v -is [System.Management.Automation.SwitchParameter]) { if ($v.IsPresent) { $parts += "-$k" }; continue }
        if ($pathParams -contains $k) { $v = Get-FullPathTrim ([string]$v) }
        $parts += "-$k"; $parts += ('"' + $v + '"')
    }
    $arg = ($parts -join ' ').Replace("'", "''")
    return "Start-Process powershell -Verb RunAs -ArgumentList '$arg'"
}

try {
    # ---- pick the backup
    if ($From) {
        if (-not (Test-Path -LiteralPath $From -PathType Container)) { Fail 1 "Backup folder not found: $From" }
        $backup = (Resolve-Path -LiteralPath $From).Path
    } else {
        if (-not (Test-Path -LiteralPath $BackupRoot -PathType Container)) { Fail 1 "Backup root not found: $BackupRoot" }
        $newest = Get-ChildItem -LiteralPath $BackupRoot -Directory |
            Where-Object { $_.Name -match '^streammulticast-\d{8}-\d{6}$' } |
            Sort-Object Name -Descending | Select-Object -First 1
        if (-not $newest) { Fail 1 "No streammulticast-<yyyyMMdd-HHmmss> backup in $BackupRoot" }
        $backup = $newest.FullName
    }
    $manifestFile = Join-Path $backup 'MANIFEST.json'
    if (-not (Test-Path -LiteralPath $manifestFile)) { Fail 1 "Not an install.ps1 backup (no MANIFEST.json): $backup" }
    $m = Get-Content -LiteralPath $manifestFile -Raw | ConvertFrom-Json
    if (-not $InstallDir) { $InstallDir = $m.installDir }
    if (-not $ConfigDir) { $ConfigDir = $m.configDir }
    if (-not $InstallDir -or -not $ConfigDir) { Fail 1 'Backup manifest lacks install/config paths; pass -InstallDir and -ConfigDir' }

    $plugDir = Join-Path $InstallDir 'obs-plugins\64bit'
    $dataDir = Join-Path $InstallDir 'data\obs-plugins\streammulticast'
    $cfgFile = Join-Path $ConfigDir 'config.json'
    $bkDll = Join-Path $backup 'streammulticast.dll'
    $bkPdb = Join-Path $backup 'streammulticast.pdb'
    $bkData = Join-Path $backup 'data'
    $bkCfg = Join-Path $backup 'plugin_config\streammulticast\config.json'
    $curDll = Join-Path $plugDir 'streammulticast.dll'
    $curPdb = Join-Path $plugDir 'streammulticast.pdb'

    $actions = @(
        @{ name = 'dll';    has = [bool]$m.items.dll;    target = $curDll },
        @{ name = 'pdb';    has = [bool]$m.items.pdb;    target = $curPdb },
        @{ name = 'data';   has = [bool]$m.items.data;   target = $dataDir },
        @{ name = 'config'; has = [bool]$m.items.config; target = $cfgFile }
    )
    foreach ($a in $actions) {
        $src = @{ dll = $bkDll; pdb = $bkPdb; data = $bkData; config = $bkCfg }[$a.name]
        if ($a.has -and -not (Test-Path -LiteralPath $src)) { Fail 1 "Backup is incomplete, missing: $src" }
    }

    $obsRunning = [bool](Get-Process -Name $ProcessName -ErrorAction SilentlyContinue)

    if ($DryRun) {
        Write-Host 'DRY RUN - nothing will be changed'
        Write-Host "Backup to restore: $backup"
        if ($obsRunning) { Write-Host 'NOTE: OBS is running; a real rollback would exit 2' }
        foreach ($a in $actions) {
            if ($a.has) { Write-Host ("  restore {0,-6} -> {1}" -f $a.name, $a.target) }
            elseif ($a.name -ne 'config') { Write-Host ("  remove  {0,-6} -> {1} (was not installed before)" -f $a.name, $a.target) }
            else { Write-Host '  config: no config in the backup, current config is left alone' }
        }
        if (-not (Test-WritableNoWrite $plugDir) -or -not (Test-WritableNoWrite $dataDir)) {
            Write-Host 'NOTE: install dir is not writable from this session (needs elevation). Run:'
            Write-Host ('  ' + (Get-ElevatedCommand))
        }
        exit 0
    }

    if ($obsRunning) { Fail 2 'OBS is running - close OBS first' }
    if (-not (Test-DirWritable $plugDir) -or -not (Test-DirWritable $dataDir)) {
        Write-Host "The OBS install dir is not writable: $InstallDir"
        Write-Host 'Run this from the current (non-elevated) shell to retry elevated:'
        Write-Host ('  ' + (Get-ElevatedCommand))
        exit 3
    }

    $snap = $null
    # ---- snapshot the current state first
    $snap = Join-Path $BackupRoot ('streammulticast-prerollback-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
    New-Item -ItemType Directory -Path $snap -ErrorAction Stop | Out-Null
    $snapItems = [ordered]@{ dll = $false; pdb = $false; data = $false; config = $false }
    if (Test-Path -LiteralPath $curDll) { Copy-Item -LiteralPath $curDll -Destination (Join-Path $snap 'streammulticast.dll'); $snapItems.dll = $true }
    if (Test-Path -LiteralPath $curPdb) { Copy-Item -LiteralPath $curPdb -Destination (Join-Path $snap 'streammulticast.pdb'); $snapItems.pdb = $true }
    if (Test-Path -LiteralPath $dataDir) { Copy-Item -LiteralPath $dataDir -Destination (Join-Path $snap 'data') -Recurse; $snapItems.data = $true }
    if (Test-Path -LiteralPath $cfgFile) {
        $d = Join-Path $snap 'plugin_config\streammulticast'
        New-Item -ItemType Directory -Path $d -Force | Out-Null
        Copy-Item -LiteralPath $cfgFile -Destination (Join-Path $d 'config.json')
        $snapItems.config = $true
    }
    $snapManifest = [ordered]@{ version = 1; created = (Get-Date).ToString('s'); installDir = $InstallDir; configDir = $ConfigDir; source = "pre-rollback snapshot (restoring $backup)"; items = $snapItems }
    [IO.File]::WriteAllText((Join-Path $snap 'MANIFEST.json'), ($snapManifest | ConvertTo-Json -Depth 4), (New-Object Text.UTF8Encoding($false)))
    Write-Host "Pre-rollback snapshot: $snap"

    # ---- restore
    New-Item -ItemType Directory -Path $plugDir -Force | Out-Null
    foreach ($pair in @(@($bkDll, $curDll, [bool]$m.items.dll), @($bkPdb, $curPdb, [bool]$m.items.pdb))) {
        if ($pair[2]) {
            $tmp = $pair[1] + '.new'
            Copy-Item -LiteralPath $pair[0] -Destination $tmp -Force
            Move-Item -LiteralPath $tmp -Destination $pair[1] -Force
            if ((Get-FileHash -LiteralPath $pair[0] -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $pair[1] -Algorithm SHA256).Hash) {
                throw "post-restore hash mismatch for $($pair[1])"
            }
        } elseif (Test-Path -LiteralPath $pair[1]) {
            Remove-Item -LiteralPath $pair[1] -Force
        }
    }
    if (Test-Path -LiteralPath $dataDir) { Remove-Item -LiteralPath $dataDir -Recurse -Force }
    if ($m.items.data) {
        New-Item -ItemType Directory -Path (Split-Path -Parent $dataDir) -Force | Out-Null
        Copy-Item -LiteralPath $bkData -Destination $dataDir -Recurse
    }
    if ($m.items.config) {
        New-Item -ItemType Directory -Path $ConfigDir -Force | Out-Null
        Copy-Item -LiteralPath $bkCfg -Destination $cfgFile -Force
        if ((Get-FileHash -LiteralPath $bkCfg -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $cfgFile -Algorithm SHA256).Hash) {
            throw 'post-restore hash mismatch for config.json'
        }
    } else {
        Write-Host 'Note: the backup holds no config; the current config was left alone.'
    }
    Write-Host "Restored from: $backup"
    exit 0
} catch {
    $msg = "rollback.ps1 failed: " + $_.Exception.Message
    if ($curDll) { Remove-Item -LiteralPath ($curDll + '.new'), ($curPdb + '.new') -Force -ErrorAction SilentlyContinue }
    if ($snap -and (Test-Path -LiteralPath $snap)) {
        $msg += [Environment]::NewLine + "The restore may be partial. Undo with: powershell -NoProfile -File `"$PSCommandPath`" -From `"$snap`""
        $msg += [Environment]::NewLine + "Original backup: $backup"
    }
    Fail 1 $msg
}
