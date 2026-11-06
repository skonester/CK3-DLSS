[CmdletBinding()]
param(
    [ValidateSet('Start','Capture','Compare','Restore','Status','Library')][string]$Action = 'Status',
    [ValidateSet('Native','Off','Dedicated','Integrated')][string]$Mode = 'Off',
    [string]$GameRoot = $PSScriptRoot,
    [string]$SettingsPath,
    [string]$ToolRoot,
    [ValidateRange(5,180)][int]$CaptureSeconds = 30,
    [string]$Label = 'paused-scene',
    [ValidateRange(0.25,1.0)][double]$RenderScale = 0.666667,
    [ValidateSet('','fsr2','dlss','xess')][string]$EngineUpscaler = '',
    [switch]$AutoLoad,
    [switch]$Census,
    [switch]$FreezeInput,
    [switch]$FeederOff,
    [switch]$NoMotion,
    [switch]$NoSpatial,
    [switch]$PanShot,
    [ValidateRange(0,5000)][int]$PortraitBudget = 0,
    [ValidateRange(0,255)][int]$OpenKey = 0,
    [ValidateRange(1.0,2.0)][double]$PortraitScale = 1.5,
    [ValidateSet('Native','Off','Dedicated','Integrated')][string[]]$Modes = @('Native','Off','Dedicated'),
    [ValidateRange(5,300)][int]$SettleSeconds = 30
)
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Write-Step([string]$Text) { Write-Output "[CK3 FSR] $Text" }
function Write-Json([string]$Path,$Value) {
    [IO.File]::WriteAllText($Path,($Value | ConvertTo-Json -Depth 12),[Text.UTF8Encoding]::new($false))
}
function Get-GraphicsRegion([string]$Text) {
    $matches = [regex]::Matches($Text,'(?m)^\s*"Graphics"\s*=\s*\{')
    if ($matches.Count -ne 1) { throw 'Expected exactly one Graphics settings block.' }
    $open = $Text.IndexOf('{',$matches[0].Index); $depth=0; $quoted=$false; $escaped=$false
    for ($i=$open; $i -lt $Text.Length; ++$i) {
        $c=$Text[$i]
        if ($escaped) { $escaped=$false; continue }
        if ($quoted -and $c -eq '\') { $escaped=$true; continue }
        if ($c -eq '"') { $quoted=-not $quoted; continue }
        if ($quoted) { continue }
        if ($c -eq '{') { ++$depth }
        if ($c -eq '}') { --$depth; if ($depth -eq 0) { return [pscustomobject]@{ Start=$open; Length=$i-$open+1 } } }
    }
    throw 'Unbalanced Graphics settings block.'
}
function Get-PdxLiteral([string]$Text,[string]$Key) {
    $region=Get-GraphicsRegion $Text; $block=$Text.Substring($region.Start,$region.Length)
    $pattern='(?ms)("'+[regex]::Escape($Key)+'"\s*=\s*\{[^{}]*?\b(?:value|enabled)\s*=\s*)("[^"]*"|[^\s}\r\n]+)'
    $matches=[regex]::Matches($block,$pattern)
    if($matches.Count -ne 1){ throw "Expected one Graphics.$Key value; found $($matches.Count)." }
    $matches[0].Groups[2].Value
}
function Set-PdxLiteral([string]$Text,[string]$Key,[string]$Literal) {
    [void](Get-PdxLiteral $Text $Key)
    $region=Get-GraphicsRegion $Text; $block=$Text.Substring($region.Start,$region.Length)
    $pattern='(?ms)("'+[regex]::Escape($Key)+'"\s*=\s*\{[^{}]*?\b(?:value|enabled)\s*=\s*)("[^"]*"|[^\s}\r\n]+)'
    $replacement=[regex]::Replace($block,$pattern,{param($m) $m.Groups[1].Value+$Literal},1)
    $Text.Substring(0,$region.Start)+$replacement+$Text.Substring($region.Start+$region.Length)
}
function Get-ConfigValue([string]$Text,[string]$Key) {
    $m=[regex]::Matches($Text,'(?m)^\s*'+[regex]::Escape($Key)+'\s*=\s*([^\r\n]*)')
    if($m.Count -gt 1){ throw "Duplicate configuration key $Key." }
    if($m.Count){ $m[0].Groups[1].Value.Trim() } else { $null }
}
function Set-ConfigValue([string]$Text,[string]$Key,$Value) {
    $pattern='(?m)^\s*'+[regex]::Escape($Key)+'\s*=[^\r\n]*(?:\r?\n)?'
    if([regex]::Matches($Text,$pattern).Count -gt 1){ throw "Duplicate configuration key $Key." }
    $line=if($null -eq $Value){ '' } else { "$Key=$Value`r`n" }
    if([regex]::IsMatch($Text,$pattern)){ [regex]::Replace($Text,$pattern,{param($m) $line},1) }
    elseif($null -ne $Value){ $Text.TrimEnd()+"`r`n"+$line } else { $Text }
}
$autosavePattern='(?ms)("autosave"\s*=\s*\{[^{}]*?\bvalue\s*=\s*)("[^"]*")'
function Get-GameAutosave([string]$Text) {
    $found=[regex]::Matches($Text,$autosavePattern)
    if($found.Count -ne 1){ throw "Expected one autosave setting; found $($found.Count)." }
    $found[0].Groups[2].Value
}
function Set-GameAutosave([string]$Text,[string]$Literal) {
    [void](Get-GameAutosave $Text)
    [regex]::Replace($Text,$autosavePattern,{param($m) $m.Groups[1].Value+$Literal},1)
}
# The layer copies the game's own swapchain image on request, so nothing else on the desktop can be captured.
function Save-GameFrame([string]$Directory) {
    $done=Join-Path $Directory 'screen.done'; $raw=Join-Path $Directory 'screen.raw'
    [IO.File]::WriteAllText((Join-Path $Directory 'screen.request'),'')
    $deadline=(Get-Date).AddSeconds(20)
    while(-not(Test-Path -LiteralPath $done)){
        if((Get-Date) -gt $deadline){ Write-Step 'The layer did not deliver a screenshot.'; return }
        Start-Sleep -Milliseconds 200
    }
    Add-Type -AssemblyName System.Drawing
    $bytes=[IO.File]::ReadAllBytes($raw)
    $w=[BitConverter]::ToInt32($bytes,0); $h=[BitConverter]::ToInt32($bytes,4)
    $bitmap=New-Object Drawing.Bitmap $w,$h,([Drawing.Imaging.PixelFormat]::Format32bppRgb)
    try {
        $lock=$bitmap.LockBits((New-Object Drawing.Rectangle 0,0,$w,$h),[Drawing.Imaging.ImageLockMode]::WriteOnly,$bitmap.PixelFormat)
        for($y=0;$y -lt $h;++$y){ [Runtime.InteropServices.Marshal]::Copy($bytes,8+$y*$w*4,[IntPtr]::Add($lock.Scan0,$y*$lock.Stride),$w*4) }
        $bitmap.UnlockBits($lock)
        $bitmap.Save((Join-Path $Directory 'screen.png'),[Drawing.Imaging.ImageFormat]::Png)
    } finally { $bitmap.Dispose() }
    Remove-Item -LiteralPath $raw,$done -Force
}
# During an unattended run the player keeps using the PC: the game takes no input and stays behind other windows.
function Set-GameWindowAside([int]$GamePid) {
    if(-not('Ck3Fsr.Window' -as [type])){
        Add-Type -Namespace Ck3Fsr -Name Window -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool EnableWindow(IntPtr h, bool enable);
[DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
[DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
'@
    }
    $handle=(Get-Process -Id $GamePid -ErrorAction SilentlyContinue).MainWindowHandle
    if(-not $handle -or $handle -eq [IntPtr]::Zero){ return }
    if([Ck3Fsr.Window]::IsWindowEnabled($handle)){
        [void][Ck3Fsr.Window]::EnableWindow($handle,$false)
        # HWND_BOTTOM; SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE
        [void][Ck3Fsr.Window]::SetWindowPos($handle,[IntPtr]1,0,0,0,0,0x13)
    }
}
function Get-LastFrameRows([string]$Csv,[int]$Bytes=65536) {
    # frames.csv grows while the game runs; read only its tail, sharing the file with the writer.
    $stream=[IO.File]::Open($Csv,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
    try {
        $count=[int][math]::Min($Bytes,$stream.Length)
        [void]$stream.Seek(-$count,[IO.SeekOrigin]::End)
        $buffer=New-Object byte[] $count
        $read=$stream.Read($buffer,0,$count)
        $lines=[Text.Encoding]::ASCII.GetString($buffer,0,$read) -split "`n"
        if($lines.Count -lt 3){ return @() }
        @($lines[1..($lines.Count-2)])
    } finally { $stream.Dispose() }
}
function Write-Utf8([string]$Path,[string]$Text) { [IO.File]::WriteAllText($Path,$Text,[Text.UTF8Encoding]::new($false)) }
function Assert-Closed { if(@(Get-Process ck3 -ErrorAction SilentlyContinue).Count){ throw 'Close Crusader Kings III before changing measurement settings.' } }
function Get-Session {
    if(-not(Test-Path -LiteralPath $latest -PathType Leaf)){ throw 'No FSR measurement session exists.' }
    Get-Content -LiteralPath $latest -Raw | ConvertFrom-Json
}
function Restore-Session($Receipt) {
    Assert-Closed
    if($Receipt.Status -eq 'restored'){ Write-Step 'Nothing to restore.'; return }
    $text=[IO.File]::ReadAllText($Receipt.SettingsPath)
    foreach($p in $Receipt.OriginalGraphics.PSObject.Properties){ $text=Set-PdxLiteral $text $p.Name ([string]$p.Value) }
    if($Receipt.PSObject.Properties['OriginalAutosave'] -and $Receipt.OriginalAutosave){ $text=Set-GameAutosave $text $Receipt.OriginalAutosave }
    Write-Utf8 $Receipt.SettingsPath $text
    if($Receipt.PSObject.Properties['ContinuePath'] -and $Receipt.ContinuePath){
        Copy-Item -LiteralPath (Join-Path $Receipt.Directory 'original-continue_game.json') -Destination $Receipt.ContinuePath -Force
        Copy-Item -LiteralPath (Join-Path $Receipt.Directory 'original-last_save.ck3') -Destination $Receipt.LastSavePath -Force
        Remove-Item -LiteralPath (Join-Path $Receipt.Directory 'original-last_save.ck3') -Force
        if(Test-Path -LiteralPath $Receipt.MeasureSavePath){ Remove-Item -LiteralPath $Receipt.MeasureSavePath -Force }
    }
    $cfg=[IO.File]::ReadAllText($Receipt.ConfigPath)
    foreach($p in $Receipt.OriginalConfig.PSObject.Properties){ $cfg=Set-ConfigValue $cfg $p.Name $p.Value }
    Write-Utf8 $Receipt.ConfigPath $cfg
    $ini=[IO.File]::ReadAllText($Receipt.ReShadePath)
    $ini=Set-ConfigValue $ini 'NeuralUplift' $Receipt.OriginalUplift
    Write-Utf8 $Receipt.ReShadePath $ini
    if($Receipt.HadAddon){ Copy-Item -LiteralPath (Join-Path $Receipt.Directory 'original-addon.bin') -Destination $Receipt.AddonPath -Force }
    elseif(Test-Path -LiteralPath $Receipt.AddonPath){ Remove-Item -LiteralPath $Receipt.AddonPath -Force }
    $Receipt.Status='restored'; Write-Json $latest $Receipt
    Write-Step 'Restored the saved graphics/portrait settings and the measurement add-on state.'
}
function Quantile([double[]]$Values,[double]$Fraction) {
    if(-not $Values.Count){ throw 'No frame timings to summarize.' }
    $sorted=@($Values | Sort-Object); $at=($sorted.Count-1)*$Fraction
    $lo=[int][math]::Floor($at); $hi=[int][math]::Ceiling($at)
    [double]$sorted[$lo]+([double]$sorted[$hi]-[double]$sorted[$lo])*($at-$lo)
}
if($Action -eq 'Library'){ return }

$GameRoot=[IO.Path]::GetFullPath($GameRoot.TrimEnd('\','/'))
$bin=Join-Path $GameRoot 'binaries'; $active=Join-Path $bin 'dlss-active'
if(-not(Test-Path -LiteralPath (Join-Path $bin 'ck3.exe') -PathType Leaf)){ throw 'CK3 binaries\ck3.exe was not found.' }
$measureRoot=Join-Path $bin 'fsr-measurements'; $latest=Join-Path $measureRoot 'latest.json'; $sceneSave=Join-Path $measureRoot 'scene.ck3'
if(-not $SettingsPath){ $SettingsPath=Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Paradox Interactive\Crusader Kings III\pdx_settings.txt' }
if(-not $ToolRoot){ $ToolRoot=Join-Path $PSScriptRoot 'fsr-measurement' }

switch($Action){
    'Status' {
        if(Test-Path -LiteralPath $latest){ Get-Content -LiteralPath $latest -Raw }
        else { Write-Step 'No measurement session has been started.' }
    }
    'Restore' { Restore-Session (Get-Session) }
    'Start' {
        Assert-Closed
        $profile=Get-Content -LiteralPath (Join-Path $active 'CK3-DLSS-RUNTIME.json') -Raw | ConvertFrom-Json
        if($profile.Profile -ne 'DLSS5Extended'){ throw 'Install DLSS 5 Extended before starting a portrait-mode measurement.' }
        $addon=Join-Path $ToolRoot 'ck3-fsr-measure.addon64'; $pythonFile=Join-Path $ToolRoot 'python-path.txt'
        $layerDir=Join-Path $ToolRoot 'layer'; $fsrRuntime=Join-Path $ToolRoot 'runtime\fsr31\amd_fidelityfx_vk.dll'
        if(-not(Test-Path -LiteralPath (Join-Path $layerDir 'VkLayer_ck3_fsr.json') -PathType Leaf)){ throw 'Measurement layer is missing. Run Stage-CK3-FsrMeasurement.ps1 first.' }
        if($Mode -eq 'Integrated' -and -not(Test-Path -LiteralPath $addon -PathType Leaf)){ throw 'Measurement add-on is missing. Run Stage-CK3-FsrMeasurement.ps1 first.' }
        if($Mode -eq 'Dedicated' -and -not(Test-Path -LiteralPath $fsrRuntime -PathType Leaf)){ throw 'The pinned FSR runtime is missing. Run Stage-CK3-FsrMeasurement.ps1 first.' }
        if($Mode -eq 'Native'){ $RenderScale=1.0 }
        elseif($Mode -eq 'Off' -and -not $PSBoundParameters.ContainsKey('RenderScale')){ $RenderScale=0.7 }
        $python=if($env:CK3_FSR_PYTHON){ $env:CK3_FSR_PYTHON }elseif(Test-Path -LiteralPath $pythonFile){ ([IO.File]::ReadAllText($pythonFile)).Trim() }else{ '' }
        if(-not(Test-Path -LiteralPath $python -PathType Leaf)){ throw 'The local FSR measurement Python environment is unavailable.' }
        $scanLines=& $python (Join-Path $ToolRoot 'probe.py') --mode scan
        if($LASTEXITCODE -ne 0){ throw 'GPU preflight failed before changing game settings.' }
        $scan=($scanLines -join "`n") | ConvertFrom-Json
        $renderGpu=@($scan.devices | Where-Object { $_.vendor_id -eq 4318 -and $_.device_type -eq 'discrete' })
        if($renderGpu.Count -ne 1){ throw 'Expected one NVIDIA rendering GPU. No fallback selected.' }
        if($Mode -eq 'Integrated' -and @($scan.devices | Where-Object { $_.vendor_id -eq 4098 -and $_.device_type -eq 'integrated' }).Count -ne 1){ throw 'Expected one AMD integrated processing GPU. No fallback selected.' }
        if($Mode -eq 'Integrated'){
            & $python -c "import sys; sys.path.insert(0,sys.argv[1]); from fsr_benchmark import Runtime; Runtime(); from live_worker import NativeConverter; NativeConverter()" $ToolRoot 2>&1 | ForEach-Object { Write-Output $_ }
            if($LASTEXITCODE -ne 0){ throw 'The pinned FSR runtime could not initialize. No game settings changed.' }
        }
        if(Test-Path -LiteralPath $latest){ $old=Get-Session; if($old.Status -ne 'restored'){ Restore-Session $old } }
        $token=[guid]::NewGuid().ToString('N'); $dir=Join-Path $measureRoot $token
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
        $cfgPath=Join-Path $active 'dlss5-feed.cfg'; $iniPath=Join-Path $bin 'ReShade.ini'
        $text=[IO.File]::ReadAllText($SettingsPath); $cfg=[IO.File]::ReadAllText($cfgPath); $ini=[IO.File]::ReadAllText($iniPath)
        $settings=[ordered]@{ renderer='"Vulkan"'; display_mode='"windowed"'; fullscreen_resolution='"1920x1080"'; windowed_resolution='"1920x1080"'; render_scale=$RenderScale.ToString('0.000000',[Globalization.CultureInfo]::InvariantCulture); vsync='no'; adaptive_framerate='no'; device=('"'+$renderGpu[0].name+'"') }
        if($EngineUpscaler){ $settings['upscale']='"'+$EngineUpscaler+'"'; $settings['upscale_quality']='"quality"' }
        $original=[ordered]@{}; foreach($key in $settings.Keys){ $original[$key]=Get-PdxLiteral $text $key; $text=Set-PdxLiteral $text $key $settings[$key] }
        $originalCfg=[ordered]@{}; foreach($key in @('portrait_mode','portrait_texture','frame_stats')){ $originalCfg[$key]=Get-ConfigValue $cfg $key; $cfg=Set-ConfigValue $cfg $key '1' }
        if($FeederOff){ $originalCfg['enabled']=Get-ConfigValue $cfg 'enabled'; $cfg=Set-ConfigValue $cfg 'enabled' '0' }
        # DLSS 5's budget counts atlas pixels, which grow with the square of the portrait scale: keep the same portraits covered.
        $originalCfg['portrait_budget']=Get-ConfigValue $cfg 'portrait_budget'
        $budget=if($PortraitBudget -gt 0){ $PortraitBudget }elseif($Mode -eq 'Dedicated'){ [int][math]::Ceiling(400*$PortraitScale*$PortraitScale) }elseif($Mode -eq 'Off'){ 700 }else{ 400 }
        $cfg=Set-ConfigValue $cfg 'portrait_budget' $budget
        $uplift=Get-ConfigValue $ini 'NeuralUplift'
        if($null -eq $uplift){ throw 'DLSS 5 NeuralUplift configuration is missing; install the Extended profile first.' }
        $newIni=Set-ConfigValue $ini 'NeuralUplift' '1'
        $targetAddon=Join-Path $active 'ck3-fsr-measure.addon64'; $hadAddon=Test-Path -LiteralPath $targetAddon
        if($hadAddon){ Copy-Item -LiteralPath $targetAddon -Destination (Join-Path $dir 'original-addon.bin') }
        # A session never writes to the player's saves: autosave is off and an automatic load uses a private copy of the scene.
        $originalAutosave=Get-GameAutosave $text; $text=Set-GameAutosave $text '"NO_AUTOSAVE"'
        $documents=Split-Path -Parent $SettingsPath; $continuePath=''; $lastSavePath=''; $measureSavePath=''
        if($AutoLoad){
            $continuePath=Join-Path $documents 'continue_game.json'; $lastSavePath=Join-Path $documents 'last_save.ck3'
            $measureSavePath=Join-Path $documents 'save games\CK3_FSR_measure.ck3'
            if(-not(Test-Path -LiteralPath $sceneSave -PathType Leaf)){ Copy-Item -LiteralPath $lastSavePath -Destination $sceneSave }
            Copy-Item -LiteralPath $continuePath -Destination (Join-Path $dir 'original-continue_game.json')
            Copy-Item -LiteralPath $lastSavePath -Destination (Join-Path $dir 'original-last_save.ck3')
        }
        $receipt=[pscustomobject][ordered]@{ SchemaVersion=1; Session=$token; Mode=$Mode; Status='prepared'; Directory=$dir;
            SettingsPath=$SettingsPath; OriginalGraphics=$original; ConfigPath=$cfgPath; OriginalConfig=$originalCfg;
            ReShadePath=$iniPath; OriginalUplift=$uplift; AddonPath=$targetAddon; HadAddon=$hadAddon;
            OriginalAutosave=$originalAutosave; ContinuePath=$continuePath; LastSavePath=$lastSavePath; MeasureSavePath=$measureSavePath;
            GamePid=0; WorkerPid=0; RenderGpu=$renderGpu[0].name; ConfiguredRender=$(if($Mode -eq 'Native'){ [int[]]@(1920,1080) }elseif($Mode -eq 'Off'){ [int[]]@([int](1920*$RenderScale),[int](1080*$RenderScale)) }else{ [int[]]@(1280,720) }); Output=[int[]]@(1920,1080);
            Notes=$(switch($Mode){
                'Native' { 'Native 1080p scene, no upscaler: the baseline every other mode is judged against.' }
                'Off' { 'Scene at reduced render scale, FSR 1 EASU + RCAS instead of the game''s bilinear upscale, RCAS on portraits; GUI at native resolution.' }
                'Dedicated' { 'FSR 3.1.3 in-process on the game''s device via VK_LAYER_ck3_fsr: tonemapped 720p scene, scene depth, viewport jitter, camera motion vectors, GUI at native resolution, portraits rendered at native size.' }
                'Integrated' { 'Whole-frame host-staged bridge to the AMD integrated GPU, including the GUI; estimated motion, no jitter.' }
            }) }
        Write-Json $latest $receipt
        try {
            Write-Utf8 $SettingsPath $text; Write-Utf8 $cfgPath $cfg; Write-Utf8 $iniPath $newIni
            if($AutoLoad){
                Copy-Item -LiteralPath $sceneSave -Destination $measureSavePath -Force
                Copy-Item -LiteralPath $sceneSave -Destination $lastSavePath -Force
                $continue=[IO.File]::ReadAllText($continuePath)
                Write-Utf8 $continuePath ([regex]::Replace($continue,'("title"\s*:\s*)"[^"]*"','$1"CK3_FSR_measure"',1))
            }
            if($Mode -eq 'Integrated'){ Copy-Item -LiteralPath $addon -Destination $targetAddon -Force }
            Add-Type -Namespace Ck3Fsr -Name Native -MemberDefinition '[DllImport("kernel32.dll", CharSet=CharSet.Unicode)] public static extern bool SetDllDirectory(string path);'
            [void][Ck3Fsr.Native]::SetDllDirectory($null)
            $psi=New-Object Diagnostics.ProcessStartInfo (Join-Path $bin 'ck3.exe')
            $psi.UseShellExecute=$false; $psi.WorkingDirectory=$bin; $psi.Arguments=if($AutoLoad){ '-gdpr-compliant -continuelastsave' }else{ '-gdpr-compliant' }
            $environment=@{ VK_LAYER_PATH=(Join-Path $bin 'dlss5-vulkan'); VK_INSTANCE_LAYERS='VK_LAYER_feed_vk;VK_LAYER_reshade'; DISABLE_VK_LAYER_reshade_1='1'; RESHADE_BASE_PATH_OVERRIDE=$bin; CK3_FSR_SESSION=$token; CK3_FSR_MODE=$Mode; CK3_FSR_OUTPUT=$dir }
            $environment.VK_LAYER_PATH=$layerDir+';'+$environment.VK_LAYER_PATH
            $environment.VK_INSTANCE_LAYERS='VK_LAYER_ck3_fsr;'+$environment.VK_INSTANCE_LAYERS
            $environment.CK3_FSR_RUNTIME=$fsrRuntime
            if($Census){ $environment.CK3_FSR_CENSUS='1' }
            if($FreezeInput){ $environment.CK3_FSR_FREEZE_INPUT='1' }
            if($NoMotion){ $environment.CK3_FSR_NO_MOTION='1' }
            if($NoSpatial){ $environment.CK3_FSR_SPATIAL='0' }
            $environment.CK3_FSR_PORTRAIT_SCALE=$PortraitScale.ToString('0.###',[Globalization.CultureInfo]::InvariantCulture)
            foreach($key in $environment.Keys){ $psi.EnvironmentVariables[$key]=$environment[$key] }
            $game=[Diagnostics.Process]::Start($psi); $receipt.GamePid=$game.Id; $receipt.Status='running'
            if($Mode -eq 'Integrated'){
                $worker=New-Object Diagnostics.ProcessStartInfo $python
                $worker.UseShellExecute=$false; $worker.CreateNoWindow=$true; $worker.WorkingDirectory=$ToolRoot
                $worker.Arguments='"'+(Join-Path $ToolRoot 'live_worker.py')+'" --session '+$token+' --gpu '+$Mode+' --game-pid '+$game.Id+' --output "'+$dir+'"'
                $receipt.WorkerPid=([Diagnostics.Process]::Start($worker)).Id
            }
            Write-Json $latest $receipt
            Write-Step "Started CK3 with DLSS5 Extended portrait mode; mode=$Mode; configured scene=$($receipt.ConfiguredRender -join 'x'); output=1080p."
            Write-Step 'Load the same save, pause, use the same camera and portraits, then press Record 30 seconds in the installer.'
            Write-Step "Session: $dir"
        } catch {
            if(-not $receipt.GamePid){ Restore-Session $receipt }
            throw
        }
    }
    'Capture' {
        $receipt=Get-Session
        if($receipt.Status -ne 'running' -or -not(Get-Process -Id $receipt.GamePid -ErrorAction SilentlyContinue)){ throw 'The measurement session game is not running.' }
        $csv=Join-Path $receipt.Directory 'frames.csv'
        if(-not(Test-Path -LiteralPath $csv)){ throw 'Measurement add-on has not produced frames.csv. Check ReShade.log before recording.' }
        $before=@(Import-Csv -LiteralPath $csv); if($before.Count -lt 2){ throw 'Wait for CK3 to finish loading before recording.' }
        $start=[double]::Parse($before[-1].t_ms,[Globalization.CultureInfo]::InvariantCulture)
        Write-Step "Recording $CaptureSeconds seconds. Keep the game in the same paused scene."
        for($second=0;$second -lt $CaptureSeconds;++$second){
            Start-Sleep -Seconds 1
            if(-not(Get-Process -Id $receipt.GamePid -ErrorAction SilentlyContinue)){ throw 'CK3 exited during capture; no valid measurement report generated.' }
            if(($second+1)%5 -eq 0){ Write-Step "Recorded $($second+1)/$CaptureSeconds seconds" }
        }
        $rows=@(Import-Csv -LiteralPath $csv | Where-Object { [double]::Parse($_.t_ms,[Globalization.CultureInfo]::InvariantCulture) -gt $start })
        if($rows.Count -lt 30){ throw 'Fewer than 30 game presents were recorded; no valid measurement report generated.' }
        if(([double]::Parse($rows[-1].t_ms,[Globalization.CultureInfo]::InvariantCulture)-$start) -lt ($CaptureSeconds-1)*1000){ throw 'The frame log stalled during capture; no valid measurement report generated.' }
        $enabled=$receipt.Mode -in @('Dedicated','Integrated')
        $bad=@($rows | Where-Object { $_.output_w -ne '1920' -or $_.output_h -ne '1080' -or ($enabled -and ($_.fsr_success -ne '1' -or $_.input_w -ne '1280' -or $_.input_h -ne '720')) })
        if($bad.Count){ throw "$($bad.Count) captured frames lack verified 720-to-1080 FSR/output. Inspect bridge.log and worker.json; this run is invalid." }
        $inv=[Globalization.CultureInfo]::InvariantCulture
        $dt=[double[]]@($rows | ForEach-Object { [double]::Parse($_.dt_ms,$inv) })
        $cpu=[double[]]@($rows | ForEach-Object { [double]::Parse($_.fsr_cpu_ms,$inv) })
        $gpu=[double[]]@($rows | ForEach-Object { [double]::Parse($_.fsr_gpu_ms,$inv) })
        $report=[ordered]@{ SchemaVersion=1; Session=$receipt.Session; Mode=$receipt.Mode; Label=$Label; Frames=$rows.Count;
            CapturedUtc=[DateTime]::UtcNow.ToString('o'); CaptureSeconds=$CaptureSeconds; MedianFrameMs=(Quantile $dt .5); P95FrameMs=(Quantile $dt .95); P99FrameMs=(Quantile $dt .99);
            MedianFps=(1000/(Quantile $dt .5)); MedianFsrStageCpuMs=(Quantile $cpu .5); MedianFsrGpuMs=(Quantile $gpu .5);
            MedianWorkerMs=(Quantile ([double[]]@($rows | ForEach-Object { [double]::Parse($_.worker_ms,$inv) })) .5);
            VerifiedInput=$(if($enabled){ [int[]]@(1280,720) }else{ $null }); ConfiguredSceneRender=[int[]]$receipt.ConfiguredRender; VerifiedOutput=[int[]]@(1920,1080); FsrEnabled=$enabled; ConfiguredProfile='DLSS5Extended'; ConfiguredPortraitTexture=$true;
            RenderGpu=$receipt.RenderGpu; ProcessingGpu=$rows[-1].gpu;
            Limitations=$receipt.Notes+' Frame interval is CPU present-to-present, not GPU frametime or display latency.' }
        if($receipt.Mode -eq 'Integrated'){ $workerReport=Get-Content -LiteralPath (Join-Path $receipt.Directory 'worker.json') -Raw | ConvertFrom-Json; if($workerReport.status -eq 'failed'){ throw 'The actual FSR worker failed; run is invalid.' }; $report.Worker=$workerReport }
        if($rows[0].PSObject.Properties.Name -contains 'readback_wait_ms'){
            $stages=[ordered]@{}
            foreach($name in @('readback_wait_ms','input_copy_ms','worker_wait_ms','output_copy_ms','upload_wait_ms')){
                $stages[$name]=Quantile ([double[]]@($rows | ForEach-Object { [double]::Parse($_.$name,$inv) })) .5
            }
            $report.BridgeMedianMs=$stages
        }
        $tag=(Get-Date).ToString('yyyyMMdd-HHmmss'); $reportPath=Join-Path $receipt.Directory ("measurement-$tag.json")
        Write-Json $reportPath $report
        $rows | Export-Csv -LiteralPath (Join-Path $receipt.Directory ("measurement-$tag.csv")) -NoTypeInformation -Encoding UTF8
        foreach($file in @('dlss5-feed.log','dlss5-feed-frames.csv')){
            $src=Join-Path $active $file
            if(Test-Path -LiteralPath $src){
                try { Copy-Item -LiteralPath $src -Destination (Join-Path $receipt.Directory $file) -Force }
                catch { Write-Step "Optional $file snapshot unavailable while CK3 owns the file. Core measurement was saved." }
            }
        }
        Write-Step ("Frame median {0:N2} ms; p95 {1:N2}; p99 {2:N2}; median FPS {3:N1}" -f $report.MedianFrameMs,$report.P95FrameMs,$report.P99FrameMs,$report.MedianFps)
        Write-Step "Report: $reportPath"
    }
    'Compare' {
        # Unattended: every mode loads the same private copy of the scene, settles, records and closes.
        Assert-Closed
        if(Test-Path -LiteralPath $latest){ $old=Get-Session; if($old.Status -ne 'restored'){ Restore-Session $old } }
        $common=@{ GameRoot=$GameRoot; SettingsPath=$SettingsPath; ToolRoot=$ToolRoot }
        $rows=@()
        try {
            foreach($each in $Modes){
                Write-Step "--- $each ---"
                & $PSCommandPath -Action Start -Mode $each -AutoLoad -FreezeInput -FeederOff:$FeederOff -NoMotion:$NoMotion -NoSpatial:$NoSpatial -PortraitScale $PortraitScale -PortraitBudget $PortraitBudget @common
                $receipt=Get-Session; $csv=Join-Path $receipt.Directory 'frames.csv'
                $deadline=(Get-Date).AddSeconds(300); $ready=$false
                while((Get-Date) -lt $deadline){
                    Start-Sleep -Seconds 2
                    if(-not(Get-Process -Id $receipt.GamePid -ErrorAction SilentlyContinue)){ throw "CK3 exited while loading in $each mode." }
                    Set-GameWindowAside $receipt.GamePid
                    if(-not(Test-Path -LiteralPath $csv)){ continue }
                    $tail=@(Get-LastFrameRows $csv)
                    if($tail.Count -lt 120){ continue }
                    if($each -eq 'Integrated'){ $ready=@($tail[-120..-1] | Where-Object { ($_ -split ',')[4] -ne '1' }).Count -eq 0 }
                    else { $ready=@($tail[-120..-1] | Where-Object { [int](($_ -split ',')[18]) -lt 50 }).Count -eq 0 }
                    if($ready){ break }
                }
                if(-not $ready){ throw "The map did not appear within five minutes in $each mode." }
                if($OpenKey -gt 0){ [IO.File]::WriteAllText((Join-Path $receipt.Directory 'key.request'),"$OpenKey"); Start-Sleep -Seconds 2 }
                Write-Step "Map is up; settling for $SettleSeconds seconds."
                Start-Sleep -Seconds $SettleSeconds
                $screen=''
                if($each -ne 'Integrated'){ Save-GameFrame $receipt.Directory; $screen=Join-Path $receipt.Directory 'screen.png'; Start-Sleep -Seconds 2 }
                & $PSCommandPath -Action Capture -Label "compare-$each" -CaptureSeconds $CaptureSeconds @common
                if($PanShot -and $each -ne 'Integrated'){
                    # A held camera key, with a frame taken mid-pan; the game keeps running for the moving-camera comparison.
                    Move-Item -LiteralPath $screen -Destination (Join-Path $receipt.Directory 'screen-still.png') -Force
                    [IO.File]::WriteAllText((Join-Path $receipt.Directory 'pan.request'),'')
                    Start-Sleep -Seconds 3
                    Save-GameFrame $receipt.Directory
                    Move-Item -LiteralPath $screen -Destination (Join-Path $receipt.Directory 'screen-pan.png') -Force
                    $screen=Join-Path $receipt.Directory 'screen-still.png'
                }
                $report=Get-ChildItem -LiteralPath $receipt.Directory -Filter 'measurement-*.json' | Sort-Object LastWriteTime | Select-Object -Last 1
                $data=Get-Content -LiteralPath $report.FullName -Raw | ConvertFrom-Json
                # The same draw count throughout the settle and the recording means nobody moved the view.
                $viewDraws=0; $viewChanged=$false
                $recorded=Join-Path $receipt.Directory ($report.BaseName+'.csv')
                $captured=@(Import-Csv -LiteralPath $recorded)
                if($captured.Count -and $captured[0].PSObject.Properties.Name -contains 'scene_draws'){
                    $draws=@($captured | ForEach-Object { [int]$_.scene_draws } | Sort-Object)
                    $viewDraws=$draws[[int]($draws.Count/2)]
                    $viewChanged=($draws[-1]-$draws[0]) -gt [math]::Max(8,$viewDraws*0.05)
                    if($viewChanged){ Write-Step "WARNING: the view changed during the $each recording ($($draws[0]) to $($draws[-1]) scene draws); do not compare this row." }
                }
                $rows+=[pscustomobject][ordered]@{ Mode=$each; MedianFps=[math]::Round($data.MedianFps,1); MedianMs=[math]::Round($data.MedianFrameMs,2); P95Ms=[math]::Round($data.P95FrameMs,2); P99Ms=[math]::Round($data.P99FrameMs,2); FsrGpuMs=[math]::Round($data.MedianFsrGpuMs,2); FsrStageCpuMs=[math]::Round($data.MedianFsrStageCpuMs,2); SceneDraws=$viewDraws; ViewChanged=$viewChanged; Frames=$data.Frames; Report=$report.FullName; Screen=$screen }
                Stop-Process -Id $receipt.GamePid -Force
                Wait-Process -Id $receipt.GamePid -Timeout 30 -ErrorAction SilentlyContinue
                Start-Sleep -Seconds 2
                Restore-Session (Get-Session)
            }
        } finally {
            Get-Process ck3 -ErrorAction SilentlyContinue | Stop-Process -Force
            Start-Sleep -Seconds 2
            $last=Get-Session; if($last.Status -ne 'restored'){ Restore-Session $last }
        }
        $tag=(Get-Date).ToString('yyyyMMdd-HHmmss'); $path=Join-Path $measureRoot "comparison-$tag.json"
        Write-Json $path ([ordered]@{ CapturedUtc=[DateTime]::UtcNow.ToString('o'); SceneSave=$sceneSave; CaptureSeconds=$CaptureSeconds; Rows=$rows })
        $rows | Format-Table Mode,MedianFps,MedianMs,P95Ms,P99Ms,FsrGpuMs,FsrStageCpuMs,SceneDraws,ViewChanged,Frames -AutoSize | Out-String | Write-Output
        Write-Step "Comparison: $path"
    }
}
