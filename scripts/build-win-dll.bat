@echo off
rem Build renpho_ble_win.dll with MSVC (run from a "x64 Native Tools" prompt).
rem Usage: scripts\build-win-dll.bat [output-dir]
set OUT=%~1
if "%OUT%"=="" set OUT=.lake\build\bin
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++17 /EHsc /O2 /MD /LD /W3 ffi\win\renpho_ble_win.cpp /Fo"%OUT%\\" /Fe"%OUT%\renpho_ble_win.dll" windowsapp.lib
exit /b %ERRORLEVEL%
