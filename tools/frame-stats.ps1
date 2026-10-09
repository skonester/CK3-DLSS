<#
.SYNOPSIS
    Summarises dlss5-feed-frames.csv (frame_stats=1) for Frontier 3 baselines and comparisons.

.DESCRIPTION
    Every row is one present of the game's swapchain, evaluated or not. dt_ms is the CPU-side
    present-to-present interval; feed_cpu_ms is the feeder's own CPU wall time. Neither is GPU
    time. Warm-up is excluded: the first -WarmupPresents rows and the same number of rows after
    every feature build (build=1). -FromSeconds/-ToSeconds select a steady-state window by t_ms.

    With -Compare, the second capture is summarised with identical settings and the deltas are
    printed. Only compare captures taken with the same save, resolution, settings and scene.

.EXAMPLE
    .\tools\frame-stats.ps1 -Csv .\build\frontier3-baseline\dlss5-feed-frames.csv -FromSeconds 30 -ToSeconds 90
.EXAMPLE
    .\tools\frame-stats.ps1 -Csv base.csv -Compare candidate.csv -Json result.json
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Csv,
    [string]$Compare,
    [int]$WarmupPresents = 120,
    [double]$FromSeconds = 0,
    [double]$ToSeconds = [double]::PositiveInfinity,
    [double]$HitchFactor = 2.0,
    [double]$SevereMs = 50.0,
    [string]$Json
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$expected = 'present,t_ms,dt_ms,api,ran,evaluated,skip,atlas_w,atlas_h,blocks,portraits,rect_age,reset,reset_why,build,feed_cpu_ms'

function Get-Percentile([double[]]$sorted, [double]$p) {
    if ($sorted.Count -eq 0) { return [double]::NaN }
    # Same nearest-rank rule as the add-on's log summary.
    $k = [int][math]::Floor($p * ($sorted.Count - 1) + 0.5)
    return $sorted[$k]
}

function Get-Summary([string]$path) {
    $resolved = (Resolve-Path -LiteralPath $path).Path
    $lines = [IO.File]::ReadAllLines($resolved)
    if ($lines.Count -lt 2) { throw "$resolved has no data rows" }
    if ($lines[0] -ne $expected) { throw "$resolved header does not match this tool (got '$($lines[0])')" }

    $cols = $expected.Split(',')
    $index = @{}
    for ($i = 0; $i -lt $cols.Count; ++$i) { $index[$cols[$i]] = $i }

    $dts = New-Object System.Collections.Generic.List[double]
    $cpu = New-Object System.Collections.Generic.List[double]
    $skips = @{}; $reasons = @{}; $atlas = @{}; $ages = @{}
    $evaluated = 0; $resets = 0; $builds = 0; $rows = 0; $excludedWarmup = 0; $excludedWindow = 0; $blankDt = 0
    $cooldown = $WarmupPresents
    $first = $null; $last = $null

    for ($n = 1; $n -lt $lines.Count; ++$n) {
        $line = $lines[$n]
        if ($line.Length -eq 0) { continue }
        $f = $line.Split(',')
        if ($f.Count -ne $cols.Count) { throw "$resolved line $($n + 1): expected $($cols.Count) fields, got $($f.Count)" }
        $inv = [Globalization.CultureInfo]::InvariantCulture
        $t = [double]::Parse($f[$index['t_ms']], $inv)
        $build = $f[$index['build']] -eq '1'
        if ($build) { ++$builds; $cooldown = [math]::Max($cooldown, $WarmupPresents) }
        if ($cooldown -gt 0) { --$cooldown; ++$excludedWarmup; continue }
        if ($t / 1000.0 -lt $FromSeconds -or $t / 1000.0 -gt $ToSeconds) { ++$excludedWindow; continue }
        ++$rows
        if ($null -eq $first) { $first = $t }
        $last = $t
        if ($f[$index['dt_ms']].Length -eq 0) { ++$blankDt } else { $dts.Add([double]::Parse($f[$index['dt_ms']], $inv)) }
        $cpu.Add([double]::Parse($f[$index['feed_cpu_ms']], $inv))
        if ($f[$index['evaluated']] -eq '1') {
            ++$evaluated
            $key = '{0}x{1}' -f $f[$index['atlas_w']], $f[$index['atlas_h']]
            $atlas[$key] = 1 + $(if ($atlas.ContainsKey($key)) { $atlas[$key] } else { 0 })
        } else {
            $s = $f[$index['skip']]; if ($s.Length -eq 0) { $s = 'none' }
            $skips[$s] = 1 + $(if ($skips.ContainsKey($s)) { $skips[$s] } else { 0 })
        }
        $age = $f[$index['rect_age']]
        if ($age -ne '-1') { $ages[$age] = 1 + $(if ($ages.ContainsKey($age)) { $ages[$age] } else { 0 }) }
        if ($f[$index['reset']] -eq '1') {
            ++$resets
            foreach ($r in $f[$index['reset_why']].Split('|')) {
                if ($r.Length -eq 0) { $r = 'unattributed' }
                $reasons[$r] = 1 + $(if ($reasons.ContainsKey($r)) { $reasons[$r] } else { 0 })
            }
        }
    }

    $sorted = $dts.ToArray(); [Array]::Sort($sorted)
    $cpuSorted = $cpu.ToArray(); [Array]::Sort($cpuSorted)
    $median = Get-Percentile $sorted 0.5
    $hitches = @($sorted | Where-Object { $_ -gt $HitchFactor * $median }).Count
    $severe = @($sorted | Where-Object { $_ -gt $SevereMs }).Count
    [pscustomobject][ordered]@{
        File            = $resolved
        Rows            = $rows
        SpanSeconds     = if ($null -ne $first) { [math]::Round(($last - $first) / 1000.0, 2) } else { 0 }
        ExcludedWarmup  = $excludedWarmup
        ExcludedWindow  = $excludedWindow
        MedianMs        = [math]::Round($median, 3)
        MedianFps       = if ($median -gt 0) { [math]::Round(1000.0 / $median, 1) } else { 0 }
        P95Ms           = [math]::Round((Get-Percentile $sorted 0.95), 3)
        P99Ms           = [math]::Round((Get-Percentile $sorted 0.99), 3)
        MaxMs           = if ($sorted.Count) { [math]::Round($sorted[$sorted.Count - 1], 3) } else { [double]::NaN }
        Hitches         = $hitches
        SevereHitches   = $severe
        Evaluated       = $evaluated
        Skipped         = $skips
        Resets          = $resets
        ResetReasons    = $reasons
        Builds          = $builds
        AtlasSizes      = $atlas
        RectAgeFrames   = $ages
        FeedCpuMedianMs = [math]::Round((Get-Percentile $cpuSorted 0.5), 3)
        FeedCpuP95Ms    = [math]::Round((Get-Percentile $cpuSorted 0.95), 3)
        Note            = 'dt is CPU present-to-present time; feed_cpu is feeder CPU wall time; GPU time is not measured'
    }
}

function Format-Table1($summary) {
    $map = { param($h) if ($h.Count -eq 0) { 'none' } else { ($h.GetEnumerator() | Sort-Object Name | ForEach-Object { '{0}={1}' -f $_.Name, $_.Value }) -join ' ' } }
    "File:         $($summary.File)"
    "Rows:         $($summary.Rows) over $($summary.SpanSeconds) s (excluded: warm-up $($summary.ExcludedWarmup), outside window $($summary.ExcludedWindow))"
    "Frame time:   median $($summary.MedianMs) ms ($($summary.MedianFps) fps), p95 $($summary.P95Ms), p99 $($summary.P99Ms), max $($summary.MaxMs)"
    "Hitches:      >$HitchFactor x median: $($summary.Hitches); >$SevereMs ms: $($summary.SevereHitches)"
    "Evaluated:    $($summary.Evaluated); skipped: $(& $map $summary.Skipped)"
    "Resets:       $($summary.Resets); reasons: $(& $map $summary.ResetReasons)"
    "Builds:       $($summary.Builds)"
    "Atlas sizes:  $(& $map $summary.AtlasSizes)"
    "Rect age:     $(& $map $summary.RectAgeFrames)"
    "Feed CPU:     median $($summary.FeedCpuMedianMs) ms, p95 $($summary.FeedCpuP95Ms) ms"
    "Note:         $($summary.Note)"
}

$base = Get-Summary $Csv
Format-Table1 $base
$result = [ordered]@{ Baseline = $base }
if ($Compare) {
    $cand = Get-Summary $Compare
    ''
    Format-Table1 $cand
    ''
    'Candidate minus baseline:'
    foreach ($k in 'MedianMs', 'P95Ms', 'P99Ms', 'MaxMs', 'Hitches', 'SevereHitches', 'Resets', 'Builds', 'FeedCpuMedianMs') {
        '  {0,-16} {1,10} -> {2,10}  ({3:+0.###;-0.###;0})' -f $k, $base.$k, $cand.$k, ($cand.$k - $base.$k)
    }
    $result.Candidate = $cand
}
if ($Json) { $result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $Json -Encoding UTF8 }
