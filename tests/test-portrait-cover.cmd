@echo off
setlocal
cd /d "%~dp0.."
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b %ERRORLEVEL%
if not exist build\compat-tests mkdir build\compat-tests
rem The transport header also defines unrelated import helpers; C4505 is expected here.
cl /nologo /EHsc /O2 /MD /W4 /WX /wd4505 /std:c++20 /Iexternal\vulkan /Fobuild\compat-tests\ /Febuild\compat-tests\portrait-cover.exe tests\portrait-cover.cpp
if errorlevel 1 exit /b %ERRORLEVEL%
build\compat-tests\portrait-cover.exe
exit /b %ERRORLEVEL%
