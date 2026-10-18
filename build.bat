@echo off
rem Builds d3d9.dll (32-bit) for Project: Snowblind. Needs VS 2026 (vcvarsall x86).
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
cd /d "%~dp0"
cl /nologo /O2 /W3 /EHsc /LD /D_CRT_SECURE_NO_WARNINGS d3d9.cpp /Fe:d3d9.dll /link /DEF:d3d9.def user32.lib /SUBSYSTEM:WINDOWS
del /q d3d9.obj d3d9.exp d3d9.lib 2>nul
if exist d3d9.dll echo built d3d9.dll
