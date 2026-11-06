@echo off
setlocal
cd /d "%~dp0"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b %ERRORLEVEL%
if not exist build mkdir build
cl /nologo /LD /EHsc /O2 /MD /std:c++20 /Iexternal\reshade\include /Fobuild\ck3-fsr-measure.obj /Fdbuild\ck3-fsr-measure.pdb src\ck3-fsr-measure.cpp /link /OUT:build\ck3-fsr-measure.addon64 /IMPLIB:build\ck3-fsr-measure.lib user32.lib
if errorlevel 1 exit /b %ERRORLEVEL%
cl /nologo /LD /EHsc /O2 /MD /std:c++20 /Fobuild\ck3-fsr-convert.obj /Fdbuild\ck3-fsr-convert.pdb src\ck3-fsr-convert.cpp /link /OUT:build\ck3-fsr-convert.dll /IMPLIB:build\ck3-fsr-convert.lib
if errorlevel 1 exit /b %ERRORLEVEL%
cl /nologo /LD /EHsc /O2 /MD /W3 /std:c++20 /Iexternal\vulkan /Fobuild\ck3-fsr-layer.obj /Fdbuild\ck3-fsr-layer.pdb layer\ck3_fsr_layer.cpp /link /OUT:build\VkLayer_ck3_fsr.dll /IMPLIB:build\VkLayer_ck3_fsr.lib /EXPORT:vkNegotiateLoaderLayerInterfaceVersion kernel32.lib user32.lib
exit /b %ERRORLEVEL%
