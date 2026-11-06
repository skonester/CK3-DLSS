@echo off
setlocal
cd /d "%~dp0"
if not exist ".venv\Scripts\python.exe" (
    python -m venv .venv
    if errorlevel 1 goto failed
)
".venv\Scripts\python.exe" -c "import flet, flet_desktop, numpy; from importlib.metadata import version; assert version('flet') == '1.1.0'; assert version('numpy') == '2.4.6'" >nul 2>&1
if errorlevel 1 (
    ".venv\Scripts\python.exe" -m pip install -r requirements.txt
    if errorlevel 1 goto failed
)
".venv\Scripts\python.exe" app.py
if errorlevel 1 goto failed
exit /b 0
:failed
echo.
echo Vega FSR Preflight could not start. See the error above.
pause
exit /b 1
