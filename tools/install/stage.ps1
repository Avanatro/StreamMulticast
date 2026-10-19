<#
.SYNOPSIS
  Stage a built StreamMulticast plugin for installation (nothing is installed).

.DESCRIPTION
  Copies streammulticast.dll/.pdb from -Build, data/locale from the repo, and the install scripts
  into <OutRoot>\<sha>\ and writes VERSION (git rev-parse HEAD, or -Sha) and SHA256SUMS
  (sha256sum format: "<hash>  <relative/path>", LF endings, VERSION included, SHA256SUMS itself excluded).
  Refuses to overwrite an existing staging folder.

.EXAMPLE
  powershell -NoProfile -File stage.ps1 -Build D:\OpenWorld\StreamMulticast\build_tests\Release
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Build,
    [string]$OutRoot = 'E:\Stream\StreamMulticast-staging',
    [string]$RepoRoot,
    [string]$Sha
)

$ErrorActionPreference = 'Stop'

try {
    if (-not $RepoRoot) { $RepoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot) }
    $dll = Join-Path $Build 'streammulticast.dll'
    $pdb = Join-Path $Build 'streammulticast.pdb'
    if (-not (Test-Path -LiteralPath $dll -PathType Leaf)) { throw "Build dir has no streammulticast.dll: $Build" }
    if (-not (Test-Path -LiteralPath $pdb -PathType Leaf)) { throw "Build dir has no streammulticast.pdb: $Build" }

    if (-not $Sha) {
        $Sha = (& git -C $RepoRoot rev-parse HEAD).Trim()
        if ($LASTEXITCODE -ne 0 -or $Sha -notmatch '^[0-9a-f]{40}$') { throw 'git rev-parse HEAD failed' }
        $dirty = & git -C $RepoRoot status --porcelain -- src CMakeLists.txt data
        if ($dirty) { Write-Host 'WARNING: working tree has uncommitted changes under src/, data/ or CMakeLists.txt; VERSION names HEAD only' }
    }
    if ($Sha -notmatch '^[0-9A-Za-z._-]+$') { throw 'Sha contains unsafe characters' }

    $locale = Join-Path $RepoRoot 'data\locale'
    if (-not (Test-Path -LiteralPath $locale -PathType Container)) { throw "Missing data\locale in $RepoRoot" }
    $scripts = @('install.ps1', 'rollback.ps1', 'migrate-config.mjs')
    foreach ($s in $scripts) {
        if (-not (Test-Path -LiteralPath (Join-Path $PSScriptRoot $s))) { throw "Missing script $s next to stage.ps1" }
    }

    $out = Join-Path $OutRoot $Sha
    if (Test-Path -LiteralPath $out) { throw "Staging folder already exists (not overwriting): $out" }
    New-Item -ItemType Directory -Path $out -ErrorAction Stop | Out-Null

    Copy-Item -LiteralPath $dll -Destination $out
    Copy-Item -LiteralPath $pdb -Destination $out
    New-Item -ItemType Directory -Path (Join-Path $out 'data') | Out-Null
    Copy-Item -LiteralPath $locale -Destination (Join-Path $out 'data\locale') -Recurse
    foreach ($s in $scripts) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot $s) -Destination $out }

    $utf8 = New-Object Text.UTF8Encoding($false)
    [IO.File]::WriteAllText((Join-Path $out 'VERSION'), ($Sha + "`n"), $utf8)

    $lines = New-Object System.Collections.Generic.List[string]
    $root = (Resolve-Path -LiteralPath $out).Path.TrimEnd('\')
    foreach ($f in (Get-ChildItem -LiteralPath $out -Recurse -File | Sort-Object FullName)) {
        $rel = $f.FullName.Substring($root.Length + 1).Replace('\', '/')
        if ($rel -eq 'SHA256SUMS') { continue }
        $h = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        $lines.Add("$h  $rel")
    }
    [IO.File]::WriteAllText((Join-Path $out 'SHA256SUMS'), (($lines -join "`n") + "`n"), $utf8)

    Write-Host "Staged: $out"
    Write-Host "VERSION: $Sha"
    foreach ($l in $lines) { Write-Host ('  ' + $l.Substring(66)) }
    exit 0
} catch {
    [Console]::Error.WriteLine("stage.ps1 failed: " + $_.Exception.Message)
    Write-Host ("stage.ps1 failed: " + $_.Exception.Message)
    exit 1
}
