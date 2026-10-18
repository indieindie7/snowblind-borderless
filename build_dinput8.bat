@echo off
rem Builds dinput8.dll (32-bit) for Project: Snowblind. Needs VS 2026 (vcvarsall x86).
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
cd /d "%~dp0"
cl /nologo /O2 /W3 /EHsc /LD /D_CRT_SECURE_NO_WARNINGS dinput8.cpp /Fe:dinput8.dll /link /DEF:dinput8.def dinput8.lib dxguid.lib /SUBSYSTEM:WINDOWS
del /q dinput8.obj dinput8.exp dinput8.lib 2>nul
if exist dinput8.dll echo built dinput8.dll
