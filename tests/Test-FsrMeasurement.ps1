$ErrorActionPreference='Stop'
. "$PSScriptRoot\..\ck3-package\CK3-FSR-Measurement.ps1" -Action Library
function Assert($Condition,[string]$Message){ if(-not $Condition){ throw $Message } }
$source=[IO.File]::ReadAllText((Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Paradox Interactive\Crusader Kings III\pdx_settings.txt'))
$original=Get-PdxLiteral $source 'render_scale'
$changed=Set-PdxLiteral $source 'render_scale' '0.666667'
Assert ((Get-PdxLiteral $changed 'render_scale') -eq '0.666667') 'Render scale was not changed.'
Assert ((Set-PdxLiteral $changed 'render_scale' $original) -ceq $source) 'Settings did not round-trip exactly.'
$changed=Set-PdxLiteral $changed 'windowed_resolution' '"1920x1080"'
Assert ((Get-PdxLiteral $changed 'windowed_resolution') -eq '"1920x1080"') 'Resolution change failed.'
$outside=$source.Substring(0,(Get-GraphicsRegion $source).Start)
Assert ($changed.StartsWith($outside)) 'Non-graphics settings changed.'
$cfg="portrait_mode=0`r`nother=hello`r`n"
Assert ((Get-ConfigValue $cfg 'missing') -eq $null) 'Missing config key should return null.'
$next=Set-ConfigValue $cfg 'frame_stats' '1'
$next=Set-ConfigValue $next 'portrait_mode' '1'
$next=Set-ConfigValue $next 'portrait_mode' '0'
$next=Set-ConfigValue $next 'frame_stats' $null
Assert ($next -ceq $cfg) 'Config did not restore missing and existing keys exactly.'
$rejected=$false
try { Get-PdxLiteral ($source+$source) 'render_scale' } catch { $rejected=$true }
Assert $rejected 'Ambiguous settings were accepted.'
Assert ((Quantile ([double[]]@(4,1,3,2)) .5) -eq 2.5) 'Median interpolation is wrong.'
Write-Output 'FSR measurement settings/restore checks passed.'
