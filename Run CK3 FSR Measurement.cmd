@echo off
setlocal
set "CK3_FSR_APP=%~dp0ck3-package\tools\CK3-DLSS-Installer\CK3 DLSS Installer.exe"
if not exist "%CK3_FSR_APP%" (
  echo Build the Kotlin installer with Build-Installer-GUI.ps1 first.
  pause
  exit /b 1
)
start "" "%CK3_FSR_APP%" --package-root "%~dp0ck3-package"
