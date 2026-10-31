@echo off
rem Builds bin\dinput8.dll (Styx Engine Fix, x64) with the VS 2022 Build Tools.
setlocal
set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
call "%VCVARS%" >nul || exit /b 1
cd /d "%~dp0"
if not exist bin mkdir bin
if not exist obj mkdir obj
cl /nologo /O2 /MT /EHsc /std:c++17 /W3 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /Foobj\ src\dllmain.cpp /LD /Febin\dinput8.dll ^
   /link /DEF:src\dinput8.def /IMPLIB:obj\dinput8.lib advapi32.lib || exit /b 1
echo Build OK: %~dp0bin\dinput8.dll
