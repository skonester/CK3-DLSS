[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
$probe=Join-Path $PSScriptRoot 'tools\vega-fsr-probe'
$dest=Join-Path $PSScriptRoot 'ck3-package\fsr-measurement'
$python=Join-Path $probe '.venv\Scripts\python.exe'
foreach($path in @($python,(Join-Path $PSScriptRoot 'build\ck3-fsr-measure.addon64'),(Join-Path $PSScriptRoot 'build\ck3-fsr-convert.dll'),(Join-Path $probe 'runtime\fsr31\amd_fidelityfx_vk.dll'))){ if(-not(Test-Path -LiteralPath $path)){ throw "Required local measurement artifact missing: $path" } }
New-Item -ItemType Directory -Path (Join-Path $dest 'runtime\fsr31') -Force | Out-Null
foreach($file in @('live_worker.py','fsr_benchmark.py','fsr_types.py','vk_native.py','vk_types.py','probe.py','assessment.py','AMD-FSR-LICENSE.txt')){ Copy-Item -LiteralPath (Join-Path $probe $file) -Destination (Join-Path $dest $file) -Force }
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'build\ck3-fsr-measure.addon64') -Destination $dest -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'build\ck3-fsr-convert.dll') -Destination $dest -Force
foreach($file in @('amd_fidelityfx_vk.dll','LICENSE.txt')){ Copy-Item -LiteralPath (Join-Path $probe ('runtime\fsr31\'+$file)) -Destination (Join-Path $dest ('runtime\fsr31\'+$file)) -Force }
New-Item -ItemType Directory -Path (Join-Path $dest 'layer') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'build\VkLayer_ck3_fsr.dll') -Destination (Join-Path $dest 'layer') -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'layer\VkLayer_ck3_fsr.json') -Destination (Join-Path $dest 'layer') -Force
[IO.File]::WriteAllText((Join-Path $dest 'python-path.txt'),$python,[Text.UTF8Encoding]::new($false))
Write-Output "Staged local-only FSR measurement tools: $dest"
