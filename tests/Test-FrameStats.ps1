# Checks tools\frame-stats.ps1 against a synthetic capture with hand-computed answers.
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$tool = Join-Path $root 'tools\frame-stats.ps1'
$out = Join-Path $root 'build\compat-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$csv = Join-Path $out 'frame-stats-fixture.csv'
$json = Join-Path $out 'frame-stats-fixture.json'

function Require($ok, $message) { if (-not $ok) { throw "FAIL: $message" } }

# Rows 1-120 warm-up (100 ms), 121-620 steady, 621 feature build, 622-740 post-build (40 ms,
# excluded together with the build row), 741-1240 steady. Each steady half has five 30 ms and
# one 60 ms frame. Included: 1000 intervals -> 988 x 10, 10 x 30, 2 x 60 ms.
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine('present,t_ms,dt_ms,api,ran,evaluated,skip,atlas_w,atlas_h,blocks,portraits,rect_age,reset,reset_why,build,feed_cpu_ms')
$t = 0.0
for ($row = 1; $row -le 1240; ++$row) {
    $dt = 10.0
    if ($row -le 120) { $dt = 100.0 }
    elseif ($row -ge 622 -and $row -le 740) { $dt = 40.0 }
    elseif (@(150, 250, 350, 450, 550, 800, 900, 1000, 1100, 1150) -contains $row) { $dt = 30.0 }
    elseif ($row -eq 600 -or $row -eq 1200) { $dt = 60.0 }
    $t += $dt
    $dtText = if ($row -eq 1) { '' } else { $dt.ToString('0.000', [Globalization.CultureInfo]::InvariantCulture) }
    $even = ($row % 2) -eq 0
    $eval = if ($even) { 1 } else { 0 }
    $skip = if ($even) { '' } else { 'no_portraits' }
    $reset = 0; $why = ''
    if (($row % 100) -eq 0) { $reset = 1; $why = 'resized|canvas' }
    if ($row -eq 1001) { $reset = 1; $why = '' }
    $build = if ($row -eq 621) { 1 } else { 0 }
    [void]$sb.AppendLine(('{0},{1},{2},vk,1,{3},{4},{5},{6},2,3,3,{7},{8},{9},0.500' -f
        $row, $t.ToString('0.000', [Globalization.CultureInfo]::InvariantCulture), $dtText, $eval, $skip,
        $(if ($even) { 512 } else { 0 }), $(if ($even) { 768 } else { 0 }), $reset, $why, $build))
}
[IO.File]::WriteAllText($csv, $sb.ToString())

& $tool -Csv $csv -Json $json | Out-Null
$r = (Get-Content -LiteralPath $json -Raw | ConvertFrom-Json).Baseline
Require ($r.Rows -eq 1000) "1000 steady rows (got $($r.Rows))"
Require ($r.ExcludedWarmup -eq 240) "start-up and post-build warm-up excluded (got $($r.ExcludedWarmup))"
Require ($r.MedianMs -eq 10 -and $r.P95Ms -eq 10 -and $r.P99Ms -eq 30 -and $r.MaxMs -eq 60) "percentiles ($($r.MedianMs)/$($r.P95Ms)/$($r.P99Ms)/$($r.MaxMs))"
Require ($r.Hitches -eq 12 -and $r.SevereHitches -eq 2) "hitch counts ($($r.Hitches)/$($r.SevereHitches))"
Require ($r.Evaluated -eq 500 -and $r.Skipped.no_portraits -eq 500) 'evaluated and skipped frames'
Require ($r.Resets -eq 11 -and $r.ResetReasons.resized -eq 10 -and $r.ResetReasons.canvas -eq 10 -and $r.ResetReasons.unattributed -eq 1) 'reset reasons'
Require ($r.Builds -eq 1) 'feature builds'
Require ($r.AtlasSizes.'512x768' -eq 500) 'atlas size histogram'
Require ($r.FeedCpuMedianMs -eq 0.5) 'feeder CPU time'

$window = Join-Path $out 'frame-stats-window.json'
& $tool -Csv $csv -FromSeconds 13 -ToSeconds 17 -Json $window | Out-Null
$w = (Get-Content -LiteralPath $window -Raw | ConvertFrom-Json).Baseline
Require ($w.Rows -gt 0 -and $w.Rows -lt 1000 -and $w.ExcludedWindow -gt 0) 'time window selects a subset'

$cmp = & $tool -Csv $csv -Compare $csv
Require (($cmp -join "`n") -match 'MedianMs\s+10\s+->\s+10\s+\(0\)') 'comparison of identical captures has zero delta'

$bad = Join-Path $out 'frame-stats-bad.csv'
[IO.File]::WriteAllText($bad, "present,t_ms`n1,0`n")
$threw = $false
try { & $tool -Csv $bad | Out-Null } catch { $threw = $true }
Require $threw 'a capture with another column layout is refused'

$header = 'present,t_ms,dt_ms,api,ran,evaluated,skip,atlas_w,atlas_h,blocks,portraits,rect_age,reset,reset_why,build,feed_cpu_ms'
$writer = Get-Content -LiteralPath (Join-Path $root 'src\feed_frame_stats.h') -Raw
Require ($writer.Contains('"' + $header + '\n"')) 'the add-on writes the header this tool expects'
Require ((Get-Content -LiteralPath $tool -Raw).Contains("'" + $header + "'")) 'the tool expects the same header'

'PASS frame-stats analyser: warm-up exclusion, percentiles, hitches, skips, reset reasons, builds, window, comparison, header check'
