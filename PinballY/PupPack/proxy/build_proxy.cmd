@echo off
rem Build the VPinMAME controller tee proxy for both bitnesses.
rem Run from a plain shell; locates VsDevCmd itself.
setlocal
set VSDEV="C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat"
set SRC=%~dp0PupPinMameProxy.cpp
set DEF=%~dp0PupPinMameProxy.def
set OUT=%~dp0..\..\..\Release

cmd /c "%VSDEV% -no_logo -arch=x86 && cl /nologo /LD /O2 /W4 /EHsc "%SRC%" /Fe:"%OUT%\PupPinMameProxy32.dll" /link /DEF:"%DEF%" ole32.lib oleaut32.lib advapi32.lib user32.lib"
if errorlevel 1 exit /b 1
cmd /c "%VSDEV% -no_logo -arch=amd64 && cl /nologo /LD /O2 /W4 /EHsc "%SRC%" /Fe:"%OUT%\PupPinMameProxy64.dll" /link /DEF:"%DEF%" ole32.lib oleaut32.lib advapi32.lib user32.lib"
if errorlevel 1 exit /b 2
echo proxy build OK
