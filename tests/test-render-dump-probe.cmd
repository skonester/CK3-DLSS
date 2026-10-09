@echo off
setlocal
cd /d "%~dp0.."
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b %ERRORLEVEL%
if not exist build\compat-tests mkdir build\compat-tests
cl /nologo /EHsc /O2 /MD /W4 /WX /std:c++20 /Fobuild\compat-tests\ /Febuild\compat-tests\render-dump-probe.exe tests\render-dump-probe.cpp
if errorlevel 1 exit /b %ERRORLEVEL%
build\compat-tests\render-dump-probe.exe
exit /b %ERRORLEVEL%
