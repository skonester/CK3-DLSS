<#
.SYNOPSIS
    Frontier 3 live-test helper for the local CK3 install: status, deliberate feeder deployment,
    config switches, log collection and restore. Never runs while CK3 is running.

.DESCRIPTION
    Status     Profile, feeder version/hash (active and payload), relevant config keys,
               NeuralUplift, and DLSS 5 evaluate success/failure counts in ReShade.log.
    Deploy     Backs up the active/payload add-ons, receipt and config (hash-verified), copies
               -Addon to both locations, verifies SHA-256 and records it in the receipt.
               Refuses unless the installed profile is DLSS5Extended (-AnyProfile overrides).
    Configure  Sets dlss5-feed.cfg keys (-Set render_dump=1,frame_stats=0), backing the file up.
    Collect    Copies dlss5-feed.log, ReShade.log, the config, dlss5-feed-frames.csv and the
               dlss5-feed-dump folder into build\frontier3-<label>-<UTC stamp>\ in this checkout.
    Restore    Puts a backup made by Deploy/Configure back and verifies the hashes.

.EXAMPLE
    .\tools\Frontier3-Live.ps1 -Action Status
    .\tools\Frontier3-Live.ps1 -Action Deploy -Addon .\build\dlss5-feed.addon64
    .\tools\Frontier3-Live.ps1 -Action Configure -Set render_dump=1
    .\tools\Frontier3-Live.ps1 -Action Collect -Label dump
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidateSet('Status', 'Deploy', 'Configure', 'Collect', 'Restore')][string]$Action,
    [string]$GameRoot = 'C:\Program Files (x86)\Steam\steamapps\common\Crusader Kings III',
    [string]$Addon,
    [string[]]$Set,
    [string]$Label = 'session',
    [string]$Backup,
    [switch]$AnyProfile,
    [string]$ProcessName = 'ck3',
    [string]$OutputRoot
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
if (-not $OutputRoot) { $OutputRoot = Join-Path $repo 'build' }
$bin = Join-Path $GameRoot 'binaries'
$active = Join-Path $bin 'dlss-active'
$payload = Join-Path $bin 'dlss-payload'
$backups = Join-Path $bin 'dlss-backups'
$receiptPath = Join-Path $active 'CK3-DLSS-RUNTIME.json'
$cfgPath = Join-Path $active 'dlss5-feed.cfg'
$stamp = (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ')

if (-not (Test-Path -LiteralPath (Join-Path $bin 'ck3.exe') -PathType Leaf)) { throw "CK3 not found under '$GameRoot'" }

function Get-Sha([string]$path) { if (Test-Path -LiteralPath $path -PathType Leaf) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() } else { $null } }

function Assert-GameClosed {
    $running = @(Get-Process -Name $ProcessName -ErrorAction SilentlyContinue)
    if ($running.Count -gt 0) { throw "$ProcessName is running (pid $($running[0].Id)); close CK3 first" }
}

function Get-FeederVersion([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return $null }
    $bytes = [IO.File]::ReadAllBytes($path)
    $text = [Text.Encoding]::ASCII.GetString($bytes)
    $m = [regex]::Match($text, 'ck3-[a-z0-9]+(-[a-z0-9]+)*\.[0-9]+')
    if ($m.Success) { $m.Value } else { 'unknown' }
}

function Get-Profile {
    if (-not (Test-Path -LiteralPath $receiptPath)) { return $null }
    (Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json).Profile
}

function Read-Cfg {
    $map = [ordered]@{}
    if (Test-Path -LiteralPath $cfgPath) {
        foreach ($line in Get-Content -LiteralPath $cfgPath) {
            if ($line -match '^\s*([A-Za-z0-9_]+)\s*=\s*(.*)$') { $map[$Matches[1]] = $Matches[2].Trim() }
        }
    }
    $map
}

# Copies files (paths relative to binaries\) into a new backup folder and verifies each copy.
function New-Backup([string]$kind, [string[]]$relative) {
    $prev = Get-Sha (Join-Path $active 'dlss5-feed.addon64')
    $dir = Join-Path $backups ('{0}-{1}-{2}' -f $kind, $stamp, $(if ($prev) { $prev.Substring(0, 8) } else { 'none' }))
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    $files = @()
    foreach ($rel in $relative) {
        $src = Join-Path $bin $rel
        if (-not (Test-Path -LiteralPath $src -PathType Leaf)) { continue }
        $dst = Join-Path $dir $rel
        New-Item -ItemType Directory -Path (Split-Path -Parent $dst) -Force | Out-Null
        Copy-Item -LiteralPath $src -Destination $dst -Force
        $h = Get-Sha $src
        if ((Get-Sha $dst) -ne $h) { throw "backup copy of $rel does not match" }
        $files += [ordered]@{ Path = $rel; Sha256 = $h }
    }
    @{ Dir = $dir; Files = $files; PreviousSha256 = $prev }
}

switch ($Action) {
    'Status' {
        $running = @(Get-Process -Name $ProcessName -ErrorAction SilentlyContinue).Count -gt 0
        $cfg = Read-Cfg
        $ini = Join-Path $bin 'ReShade.ini'
        $uplift = if (Test-Path -LiteralPath $ini) { (Select-String -LiteralPath $ini -Pattern '^NeuralUplift=(.*)$' | Select-Object -First 1 | ForEach-Object { $_.Matches[0].Groups[1].Value }) } else { $null }
        $log = Join-Path $bin 'ReShade.log'
        $ok = 0; $fail = 0
        if (Test-Path -LiteralPath $log) {
            $ok = @(Select-String -LiteralPath $log -Pattern 'evaluation succeeded').Count
            $fail = @(Select-String -LiteralPath $log -Pattern 'evaluate failed|create failed').Count
        }
        $feedLog = Join-Path $active 'dlss5-feed.log'
        $attached = if (Test-Path -LiteralPath $feedLog) { (Select-String -LiteralPath $feedLog -Pattern 'attached\.' | Select-Object -First 1).Line } else { $null }
        [pscustomobject][ordered]@{
            CK3Running        = $running
            Profile           = Get-Profile
            ActiveFeeder      = '{0} {1}' -f (Get-FeederVersion (Join-Path $active 'dlss5-feed.addon64')), (Get-Sha (Join-Path $active 'dlss5-feed.addon64'))
            PayloadFeeder     = '{0} {1}' -f (Get-FeederVersion (Join-Path $payload 'dlss5-feed.addon64')), (Get-Sha (Join-Path $payload 'dlss5-feed.addon64'))
            NativeStreamline  = Test-Path -LiteralPath (Join-Path $active 'streamline-native.enabled')
            PortraitSettings  = ($cfg.Keys | Where-Object { $_ -match '^(enabled|mode|preset|portrait_|render_dump|frame_stats)' } | ForEach-Object { '{0}={1}' -f $_, $cfg[$_] }) -join ' '
            NeuralUplift      = $uplift
            LastFeederStart   = $attached
            ReShadeEvalOk     = $ok
            ReShadeEvalFailed = $fail
        } | Format-List
    }

    'Deploy' {
        Assert-GameClosed
        if (-not $Addon) { throw '-Addon is required' }
        $source = (Resolve-Path -LiteralPath $Addon).Path
        $installedProfile = Get-Profile
        if ($installedProfile -ne 'DLSS5Extended' -and -not $AnyProfile) {
            throw "installed profile is '$installedProfile'; Frontier 3 portrait testing expects DLSS5Extended (reinstall it, or pass -AnyProfile deliberately)"
        }
        $hash = Get-Sha $source
        $version = Get-FeederVersion $source
        $b = New-Backup 'frontier3' @('dlss-active\dlss5-feed.addon64', 'dlss-payload\dlss5-feed.addon64', 'dlss-active\CK3-DLSS-RUNTIME.json', 'dlss-active\dlss5-feed.cfg')
        foreach ($dir in $active, $payload) {
            Copy-Item -LiteralPath $source -Destination (Join-Path $dir 'dlss5-feed.addon64') -Force
            if ((Get-Sha (Join-Path $dir 'dlss5-feed.addon64')) -ne $hash) { throw "copy into $dir does not match the build" }
        }
        $fileVersion = (Get-Item -LiteralPath $source).VersionInfo.FileVersion
        if (Test-Path -LiteralPath $receiptPath) {
            $receipt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
            $feeder = $receipt.Components.Feeder
            $feeder.Source = "Local Frontier 3 build $version"
            $feeder.Sha256 = $hash
            $feeder.FileVersion = $fileVersion
            if ($feeder.PSObject.Properties.Name -contains 'DeployedUtc') { $feeder.DeployedUtc = (Get-Date).ToUniversalTime().ToString('o') }
            else { $feeder | Add-Member -NotePropertyName DeployedUtc -NotePropertyValue (Get-Date).ToUniversalTime().ToString('o') }
            $receipt | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $receiptPath -Encoding UTF8
        }
        [ordered]@{
            DeployedUtc = (Get-Date).ToUniversalTime().ToString('o'); GameRoot = $GameRoot; Profile = $installedProfile
            Source = $source; Version = $version; Sha256 = $hash; PreviousSha256 = $b.PreviousSha256; BackupFiles = $b.Files
        } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $b.Dir 'deployment.json') -Encoding UTF8
        "Deployed $version ($hash) to dlss-active and dlss-payload."
        "Backup: $($b.Dir)"
        "Restore with: .\tools\Frontier3-Live.ps1 -Action Restore -Backup '$($b.Dir)'"
    }

    'Configure' {
        Assert-GameClosed
        if (-not $Set) { throw '-Set key=value[,key=value] is required' }
        if (-not (Test-Path -LiteralPath $cfgPath)) { throw "no config at $cfgPath" }
        $allowed = 'render_dump', 'frame_stats', 'portrait_mode', 'portrait_feather', 'portrait_budget', 'portrait_min'
        $b = New-Backup 'frontier3-cfg' @('dlss-active\dlss5-feed.cfg')
        $lines = [Collections.Generic.List[string]]::new([string[]](Get-Content -LiteralPath $cfgPath))
        foreach ($pair in $Set) {
            if ($pair -notmatch '^([a-z_]+)=(-?[0-9]+)$') { throw "bad setting '$pair' (expected key=integer)" }
            $key = $Matches[1]; $value = $Matches[2]
            if ($allowed -notcontains $key) { throw "'$key' is not a Frontier 3 test setting ($($allowed -join ', '))" }
            $found = $false
            for ($i = 0; $i -lt $lines.Count; ++$i) {
                if ($lines[$i] -match "^\s*$key\s*=") { $lines[$i] = "$key=$value"; $found = $true }
            }
            if (-not $found) { $lines.Add("$key=$value") }
        }
        [IO.File]::WriteAllLines($cfgPath, $lines)
        "Updated $cfgPath ($($Set -join ', ')). Previous config backed up in $($b.Dir)"
    }

    'Collect' {
        $dest = Join-Path $OutputRoot ('frontier3-{0}-{1}' -f $Label, $stamp)
        New-Item -ItemType Directory -Path $dest -Force | Out-Null
        $items = @(
            (Join-Path $active 'dlss5-feed.log'), (Join-Path $active 'dlss5-feed.cfg'), (Join-Path $active 'dlss5-feed-frames.csv'),
            (Join-Path $active 'CK3-DLSS-RUNTIME.json'), (Join-Path $bin 'ReShade.log'), (Join-Path $bin 'ReShade.ini')
        )
        foreach ($item in $items) { if (Test-Path -LiteralPath $item) { Copy-Item -LiteralPath $item -Destination $dest -Force } }
        $dump = Join-Path $active 'dlss5-feed-dump'
        if (Test-Path -LiteralPath $dump) { Copy-Item -LiteralPath $dump -Destination $dest -Recurse -Force }
        $log = Join-Path $dest 'ReShade.log'
        $ok = if (Test-Path $log) { @(Select-String -LiteralPath $log -Pattern 'evaluation succeeded').Count } else { 0 }
        $fail = if (Test-Path $log) { @(Select-String -LiteralPath $log -Pattern 'evaluate failed|create failed').Count } else { 0 }
        $feedLog = Join-Path $dest 'dlss5-feed.log'
        $probes = if (Test-Path $feedLog) { @(Select-String -LiteralPath $feedLog -Pattern '\[dump\] probe frame').Count } else { 0 }
        "Collected into $dest"
        "ReShade.log: 'evaluation succeeded' lines $ok, failure lines $fail; feeder log probe lines $probes"
        if ($ok -eq 0 -or $fail -gt 0) { "WARNING: DLSS 5 did not evaluate cleanly in this session; do not treat it as a working build." }
    }

    'Restore' {
        Assert-GameClosed
        if (-not $Backup) { throw '-Backup <folder> is required' }
        $files = Get-ChildItem -LiteralPath $Backup -Recurse -File | Where-Object { $_.Name -ne 'deployment.json' }
        foreach ($f in $files) {
            $rel = $f.FullName.Substring((Resolve-Path -LiteralPath $Backup).Path.TrimEnd('\').Length + 1)
            $dst = Join-Path $bin $rel
            Copy-Item -LiteralPath $f.FullName -Destination $dst -Force
            if ((Get-Sha $dst) -ne (Get-Sha $f.FullName)) { throw "restore of $rel does not match its backup" }
            "restored $rel"
        }
    }
}
