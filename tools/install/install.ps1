<#
.SYNOPSIS
  Install a staged StreamMulticast build into OBS, with backup and config migration.

.DESCRIPTION
  Refuses while OBS runs (exit 2). Needs write access to the OBS install dir
  (exit 3 with the exact elevated command otherwise). Verifies SHA256SUMS when the
  source folder has one (exit 4 on mismatch). Backs up the installed dll, pdb,
  plugin data dir and plugin config to <BackupRoot>\streammulticast-<yyyyMMdd-HHmmss>\,
  runs migrate-config.mjs --apply, then copies the new files.
  Stream keys are never printed.

  Exit codes: 0 ok, 1 error, 2 OBS running, 3 not writable (elevation needed), 4 checksum mismatch.

.EXAMPLE
  powershell -NoProfile -File install.ps1 -Source E:\Stream\StreamMulticast-staging\<sha> -DryRun
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Source,
    [switch]$DryRun,
    [string]$InstallDir = 'C:\Program Files\obs-studio',
    [string]$BackupRoot = 'E:\Backups',
    [string]$ConfigDir = (Join-Path $env:APPDATA 'obs-studio\plugin_config\streammulticast'),
    [string]$ServiceFile,
    [string]$AitumFile,
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
    # Probe the nearest existing ancestor (the dir itself when it exists).
    $d = $Dir
    while ($d -and -not (Test-Path -LiteralPath $d -PathType Container)) {
        if (Test-Path -LiteralPath $d) { return $false }   # a file is in the way
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

function Get-ElevatedCommand {
    $parts = @('-NoExit', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"' + $PSCommandPath + '"'))
    foreach ($k in $BoundParams.Keys) {
        if ($k -eq 'DryRun') { continue }   # the elevated command is the real run
        $v = $BoundParams[$k]
        if ($k -eq 'Source') { $v = $Source }
        if ($k -eq 'From') { $v = $From }
        if ($v -is [System.Management.Automation.SwitchParameter]) { if ($v.IsPresent) { $parts += "-$k" } }
        else { $parts += "-$k"; $parts += ('"' + $v + '"') }
    }
    if (-not $BoundParams.ContainsKey('ConfigDir')) { $parts += '-ConfigDir'; $parts += ('"' + $ConfigDir + '"') }   # an elevated different account has another APPDATA
    $arg = ($parts -join ' ').Replace("'", "''")
    return "Start-Process powershell -Verb RunAs -ArgumentList '$arg'"
}

$plugDir = Join-Path $InstallDir 'obs-plugins\64bit'
$dataDir = Join-Path $InstallDir 'data\obs-plugins\streammulticast'
$cfgFile = Join-Path $ConfigDir 'config.json'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$backupDir = Join-Path $BackupRoot ('streammulticast-' + $stamp)

try {
    # ---- source validation
    if (-not (Test-Path -LiteralPath $Source -PathType Container)) { Fail 1 "Source folder not found: $Source" }
    $Source = (Resolve-Path -LiteralPath $Source).Path
    $srcDll = Join-Path $Source 'streammulticast.dll'
    $srcPdb = Join-Path $Source 'streammulticast.pdb'
    $srcData = Join-Path $Source 'data'
    if (-not (Test-Path -LiteralPath $srcDll -PathType Leaf)) { Fail 1 "Source has no streammulticast.dll: $Source" }

    $sums = Join-Path $Source 'SHA256SUMS'
    if (Test-Path -LiteralPath $sums -PathType Leaf) {
        $n = 0
        foreach ($line in (Get-Content -LiteralPath $sums)) {
            if ([string]::IsNullOrWhiteSpace($line)) { continue }
            if ($line -notmatch '^([0-9a-fA-F]{64}) [ \*](.+)$') { Fail 4 "Malformed SHA256SUMS line in $sums" }
            $want = $Matches[1].ToLowerInvariant()
            $file = Join-Path $Source ($Matches[2] -replace '/', '\')
            if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { Fail 4 "SHA256SUMS lists a missing file: $($Matches[2])" }
            $have = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($have -ne $want) { Fail 4 "Checksum mismatch: $($Matches[2])" }
            $n++
        }
        Write-Host "SHA256SUMS verified ($n files)"
    } else {
        Write-Host 'WARNING: no SHA256SUMS in the source folder, checksums not verified'
    }
    $versionFile = Join-Path $Source 'VERSION'
    if (Test-Path -LiteralPath $versionFile) { Write-Host ('Version: ' + (Get-Content -LiteralPath $versionFile -TotalCount 1)) }

    $migrate = Join-Path $Source 'migrate-config.mjs'
    if (-not (Test-Path -LiteralPath $migrate)) { $migrate = Join-Path $PSScriptRoot 'migrate-config.mjs' }
    $haveConfig = Test-Path -LiteralPath $cfgFile -PathType Leaf
    $node = Get-Command node -ErrorAction SilentlyContinue
    if ($haveConfig -and (-not $node -or -not (Test-Path -LiteralPath $migrate))) {
        Fail 1 'node or migrate-config.mjs not found; refusing to install without the config migration'
    }
    $migrateArgs = @()
    if ($ServiceFile) { $migrateArgs += @('--service', $ServiceFile) }
    if ($AitumFile) { $migrateArgs += @('--aitum', $AitumFile) }

    $obsRunning = [bool](Get-Process -Name $ProcessName -ErrorAction SilentlyContinue)

    # ---- dry run
    if ($DryRun) {
        Write-Host 'DRY RUN - nothing will be changed'
        if ($obsRunning) { Write-Host 'NOTE: OBS is running; a real install would exit 2' }
        Write-Host "Backup target : $backupDir"
        Write-Host 'Files that would be backed up (if present):'
        foreach ($p in @((Join-Path $plugDir 'streammulticast.dll'), (Join-Path $plugDir 'streammulticast.pdb'), $dataDir, $cfgFile)) {
            Write-Host ('  {0}  [{1}]' -f $p, $(if (Test-Path -LiteralPath $p) { 'exists' } else { 'absent' }))
        }
        Write-Host 'Files that would be installed:'
        Write-Host "  streammulticast.dll -> $plugDir"
        if (Test-Path -LiteralPath $srcPdb) { Write-Host "  streammulticast.pdb -> $plugDir" }
        if (Test-Path -LiteralPath $srcData) { Write-Host "  data\ -> $dataDir" }
        if (-not (Test-DirWritable $plugDir) -or -not (Test-DirWritable $dataDir)) {
            Write-Host 'NOTE: install dir is not writable from this session (needs elevation). Run:'
            Write-Host ('  ' + (Get-ElevatedCommand))
        }
        if ($haveConfig) {
            Write-Host 'Config migration preview:'
            & $node.Source $migrate --dry-run --config $cfgFile @migrateArgs
            if ($LASTEXITCODE -ne 0) { Fail 1 "migrate-config dry run failed (exit $LASTEXITCODE)" }
        } else {
            Write-Host "No plugin config at $cfgFile (nothing to migrate)"
        }
        exit 0
    }

    # ---- real install
    if ($obsRunning) { Fail 2 'OBS is running - close OBS first' }

    if (-not (Test-DirWritable $plugDir) -or -not (Test-DirWritable $dataDir)) {
        Write-Host "The OBS install dir is not writable: $InstallDir"
        Write-Host 'Run this from the current (non-elevated) shell to retry elevated:'
        Write-Host ('  ' + (Get-ElevatedCommand))
        exit 3
    }
    if (Test-Path -LiteralPath $backupDir) { Fail 1 "Backup dir already exists: $backupDir" }

    # ---- backup
    $bkDll = Join-Path $backupDir 'streammulticast.dll'
    $bkPdb = Join-Path $backupDir 'streammulticast.pdb'
    $bkData = Join-Path $backupDir 'data'
    $bkCfgDir = Join-Path $backupDir 'plugin_config\streammulticast'
    New-Item -ItemType Directory -Path $backupDir -ErrorAction Stop | Out-Null
    $items = [ordered]@{ dll = $false; pdb = $false; data = $false; config = $false }
    $curDll = Join-Path $plugDir 'streammulticast.dll'
    $curPdb = Join-Path $plugDir 'streammulticast.pdb'
    if (Test-Path -LiteralPath $curDll) { Copy-Item -LiteralPath $curDll -Destination $bkDll; $items.dll = $true }
    if (Test-Path -LiteralPath $curPdb) { Copy-Item -LiteralPath $curPdb -Destination $bkPdb; $items.pdb = $true }
    if (Test-Path -LiteralPath $dataDir) { Copy-Item -LiteralPath $dataDir -Destination $bkData -Recurse; $items.data = $true }
    if ($haveConfig) {
        New-Item -ItemType Directory -Path $bkCfgDir -Force | Out-Null
        Copy-Item -LiteralPath $cfgFile -Destination (Join-Path $bkCfgDir 'config.json')
        $items.config = $true
    }
    foreach ($pair in @(@($curDll, $bkDll, $items.dll), @($curPdb, $bkPdb, $items.pdb), @($cfgFile, (Join-Path $bkCfgDir 'config.json'), $items.config))) {
        if ($pair[2]) {
            $a = (Get-FileHash -LiteralPath $pair[0] -Algorithm SHA256).Hash
            $b = (Get-FileHash -LiteralPath $pair[1] -Algorithm SHA256).Hash
            if ($a -ne $b) { Fail 1 "Backup verification failed for $($pair[0]); nothing was changed" }
        }
    }
    $manifest = [ordered]@{
        version    = 1
        created    = (Get-Date).ToString('s')
        installDir = $InstallDir
        configDir  = $ConfigDir
        source     = $Source
        items      = $items
    }
    [IO.File]::WriteAllText((Join-Path $backupDir 'MANIFEST.json'), ($manifest | ConvertTo-Json -Depth 4), (New-Object Text.UTF8Encoding($false)))
    Write-Host "Backup written: $backupDir"

    # ---- config migration
    if ($haveConfig) {
        & $node.Source $migrate --apply --config $cfgFile --backup-dir $backupDir @migrateArgs --process-name $ProcessName
        if ($LASTEXITCODE -ne 0) { Fail 1 "migrate-config failed (exit $LASTEXITCODE); DLL not installed. Backup: $backupDir" }
    }

    # ---- copy files (temp name + move, so a failed copy never leaves a half-written dll)
    try {
        New-Item -ItemType Directory -Path $plugDir -Force | Out-Null
        $pairs = @(,@($srcDll, $curDll))
        if (Test-Path -LiteralPath $srcPdb) { $pairs += ,@($srcPdb, $curPdb) }
        foreach ($p in $pairs) {
            $tmp = $p[1] + '.new'
            Copy-Item -LiteralPath $p[0] -Destination $tmp -Force
            Move-Item -LiteralPath $tmp -Destination $p[1] -Force
            if ((Get-FileHash -LiteralPath $p[0] -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $p[1] -Algorithm SHA256).Hash) {
                throw "post-copy hash mismatch for $($p[1])"
            }
        }
        if (Test-Path -LiteralPath $srcData -PathType Container) {
            New-Item -ItemType Directory -Path $dataDir -Force | Out-Null
            Copy-Item -Path (Join-Path $srcData '*') -Destination $dataDir -Recurse -Force
        }
    } catch {
        Write-Host "Install failed while copying: $($_.Exception.Message)"
        Write-Host "Restore with: powershell -NoProfile -File `"$(Join-Path $PSScriptRoot 'rollback.ps1')`" -From `"$backupDir`""
        exit 1
    }

    Write-Host 'Installed:'
    Write-Host "  $curDll"
    if (Test-Path -LiteralPath $srcPdb) { Write-Host "  $curPdb" }
    if (Test-Path -LiteralPath $srcData) { Write-Host "  $dataDir" }
    Write-Host "Rollback: powershell -NoProfile -File `"$(Join-Path $PSScriptRoot 'rollback.ps1')`" -From `"$backupDir`""
    exit 0
} catch {
    Fail 1 ("install.ps1 failed: " + $_.Exception.Message)
}
