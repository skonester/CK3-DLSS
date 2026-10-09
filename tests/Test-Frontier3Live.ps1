# Exercises tools\Frontier3-Live.ps1 against a fake CK3 folder; never touches the real install.
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$tool = Join-Path $root 'tools\Frontier3-Live.ps1'
$sandbox = Join-Path $root 'build\compat-tests\fake-ck3'
if (Test-Path -LiteralPath $sandbox) { Remove-Item -LiteralPath $sandbox -Recurse -Force }
$bin = Join-Path $sandbox 'binaries'
$active = Join-Path $bin 'dlss-active'
$payload = Join-Path $bin 'dlss-payload'
New-Item -ItemType Directory -Path $active, $payload -Force | Out-Null
$out = Join-Path $root 'build\compat-tests\fake-ck3-out'
if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Recurse -Force }

function Require($ok, $message) { if (-not $ok) { throw "FAIL: $message" } }
function Sha($p) { (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLowerInvariant() }
function Expect-Throw([scriptblock]$block, [string]$pattern, [string]$message) {
    $threw = $false
    try { & $block | Out-Null } catch { $threw = $_.Exception.Message -match $pattern; if (-not $threw) { throw "FAIL: $message (wrong error: $($_.Exception.Message))" } }
    Require $threw $message
}

[IO.File]::WriteAllText((Join-Path $bin 'ck3.exe'), 'fake')
[IO.File]::WriteAllText((Join-Path $active 'dlss5-feed.addon64'), 'old feeder ck3-frontier2-atlas.1')
[IO.File]::WriteAllText((Join-Path $payload 'dlss5-feed.addon64'), 'old feeder ck3-frontier2-atlas.1')
[IO.File]::WriteAllLines((Join-Path $active 'dlss5-feed.cfg'), [string[]]@('enabled=1', 'mode=2', 'portrait_mode=1', 'render_dump=0', 'preset=13'))
@{ Profile = 'NativeStreamline'; Components = @{ Feeder = @{ Source = 'Package payload'; Sha256 = 'x'; FileVersion = '0.6.0.0' } } } |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $active 'CK3-DLSS-RUNTIME.json') -Encoding UTF8
[IO.File]::WriteAllLines((Join-Path $bin 'ReShade.ini'), [string[]]@('[DLSS 5]', 'NeuralUplift=1'))
[IO.File]::WriteAllLines((Join-Path $bin 'ReShade.log'), [string[]]@('x inline feature 18 evaluation succeeded', 'y inline feature 18 evaluation succeeded'))
[IO.File]::WriteAllLines((Join-Path $active 'dlss5-feed.log'), [string[]]@('00:00:00.000  dlss5-feed ck3-frontier2-atlas.1 (built x) attached.', '[dump] probe frame 5 T#1 Portrait'))
$newAddon = Join-Path $root 'build\compat-tests\fake-new.addon64'
[IO.File]::WriteAllText($newAddon, 'new feeder ck3-frontier3-diag.1')
$common = @{ GameRoot = $sandbox; ProcessName = 'no-such-process-frontier3' }

$status = & $tool -Action Status @common | Out-String
Require ($status -match 'NativeStreamline' -and $status -match 'ck3-frontier2-atlas\.1' -and $status -match 'ReShadeEvalOk\s*:\s*2') 'status reports profile, feeder and evaluate counts'

Expect-Throw { & $tool -Action Deploy -Addon $newAddon @common } 'expects DLSS5Extended' 'deploy refuses the wrong profile'
Require ((Get-Content (Join-Path $active 'dlss5-feed.addon64') -Raw) -match 'frontier2') 'refused deploy changed nothing'

Expect-Throw { & $tool -Action Deploy -Addon $newAddon -AnyProfile -GameRoot $sandbox -ProcessName 'powershell' } 'is running' 'deploy refuses while the game process runs'

$receiptPath = Join-Path $active 'CK3-DLSS-RUNTIME.json'
$r = Get-Content $receiptPath -Raw | ConvertFrom-Json; $r.Profile = 'DLSS5Extended'; $r | ConvertTo-Json -Depth 5 | Set-Content $receiptPath -Encoding UTF8
$oldHash = Sha (Join-Path $active 'dlss5-feed.addon64')
$deploy = & $tool -Action Deploy -Addon $newAddon @common | Out-String
$newHash = Sha $newAddon
Require ((Sha (Join-Path $active 'dlss5-feed.addon64')) -eq $newHash -and (Sha (Join-Path $payload 'dlss5-feed.addon64')) -eq $newHash) 'both locations receive the build'
$receipt = Get-Content $receiptPath -Raw | ConvertFrom-Json
Require ($receipt.Components.Feeder.Sha256 -eq $newHash -and $receipt.Components.Feeder.Source -match 'ck3-frontier3-diag\.1' -and $receipt.Components.Feeder.DeployedUtc) 'receipt records the deployed feeder'
$backup = @(Get-ChildItem (Join-Path $bin 'dlss-backups') -Directory | Where-Object Name -like 'frontier3-2*')[0].FullName
$manifest = Get-Content (Join-Path $backup 'deployment.json') -Raw | ConvertFrom-Json
Require ($manifest.PreviousSha256 -eq $oldHash -and @($manifest.BackupFiles).Count -eq 4 -and $manifest.Version -eq 'ck3-frontier3-diag.1') 'deployment manifest and four backed-up files'
Require ((Sha (Join-Path $backup 'dlss-active\dlss5-feed.addon64')) -eq $oldHash) 'backup holds the previous add-on'

& $tool -Action Configure -Set 'render_dump=1', 'frame_stats=1' @common | Out-Null
$cfg = Get-Content (Join-Path $active 'dlss5-feed.cfg')
Require (($cfg -contains 'render_dump=1') -and ($cfg -contains 'frame_stats=1') -and ($cfg -contains 'preset=13') -and @($cfg | Where-Object { $_ -match '^render_dump=' }).Count -eq 1) 'configure edits, appends and preserves keys'
Expect-Throw { & $tool -Action Configure -Set 'mode=0' @common } 'not a Frontier 3 test setting' 'configure refuses unrelated keys'

New-Item -ItemType Directory -Path (Join-Path $active 'dlss5-feed-dump') -Force | Out-Null
[IO.File]::WriteAllText((Join-Path $active 'dlss5-feed-dump\f5-T1-Portrait.png'), 'png')
[IO.File]::WriteAllText((Join-Path $active 'dlss5-feed-frames.csv'), 'present,t_ms')
$collect = & $tool -Action Collect -Label test -OutputRoot $out @common | Out-String
$dest = @(Get-ChildItem $out -Directory)[0].FullName
Require ((Test-Path (Join-Path $dest 'dlss5-feed-dump\f5-T1-Portrait.png')) -and (Test-Path (Join-Path $dest 'dlss5-feed-frames.csv')) -and (Test-Path (Join-Path $dest 'ReShade.log'))) 'collect copies logs, frames and dump images'
Require ($collect -match "evaluation succeeded' lines 2, failure lines 0; feeder log probe lines 1") 'collect summarises the session'

& $tool -Action Restore -Backup $backup @common | Out-Null
Require ((Sha (Join-Path $active 'dlss5-feed.addon64')) -eq $oldHash -and (Sha (Join-Path $payload 'dlss5-feed.addon64')) -eq $oldHash) 'restore puts the previous add-ons back'
Require ((Get-Content (Join-Path $active 'dlss5-feed.cfg')) -contains 'render_dump=0') 'restore puts the previous config back'
Require ((Get-Content $receiptPath -Raw | ConvertFrom-Json).Components.Feeder.Sha256 -eq 'x') 'restore puts the previous receipt back'

'PASS Frontier 3 live helper: status, profile and process guards, deploy with verified backup and receipt, configure, collect, restore'
