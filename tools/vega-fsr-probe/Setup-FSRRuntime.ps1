$ErrorActionPreference = 'Stop'
$taskRuntimeDir = Join-Path $PSScriptRoot 'runtime\fsr31'
$taskSdkCommit = '54fbaafdc34716811751bea5032700e78f5a0f33'
$taskExpectedHash = '04A455BB7C01AF1ADC630F3BE41BD85689B6A8565199E25B09696240750F6D1E'
New-Item -ItemType Directory -Path $taskRuntimeDir -Force | Out-Null
$taskRuntimeDll = Join-Path $taskRuntimeDir 'amd_fidelityfx_vk.dll'
$taskBaseUrl = 'https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/' + $taskSdkCommit
if (-not (Test-Path -LiteralPath $taskRuntimeDll) -or (Get-FileHash -LiteralPath $taskRuntimeDll -Algorithm SHA256).Hash -ne $taskExpectedHash) {
    Invoke-WebRequest -Uri ($taskBaseUrl + '/PrebuiltSignedDLL/amd_fidelityfx_vk.dll') -OutFile $taskRuntimeDll
}
if ((Get-FileHash -LiteralPath $taskRuntimeDll -Algorithm SHA256).Hash -ne $taskExpectedHash) {
    throw 'FSR Vulkan runtime hash does not match the pinned official build.'
}
Invoke-WebRequest -Uri ($taskBaseUrl + '/LICENSE.txt') -OutFile (Join-Path $taskRuntimeDir 'LICENSE.txt')
Write-Output 'Official AMD FSR Vulkan runtime installed and SHA-256 verified. Restart the probe app.'
