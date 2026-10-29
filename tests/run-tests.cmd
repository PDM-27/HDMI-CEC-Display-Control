@echo off
rem Builds and runs every logic test. Run from any prompt; it finds Visual
rem Studio's compiler itself when cl is not already on the path.
setlocal
where cl >nul 2>nul
if errorlevel 1 (
  for /f "usebackq delims=" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%I
)
if defined VSDIR call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
where cl >nul 2>nul
if errorlevel 1 (
  echo Could not find the Visual Studio C++ compiler.
  exit /b 1
)
cd /d "%~dp0"
set OUT=%~dp0build
if not exist "%OUT%" mkdir "%OUT%"
set FAIL=0
for %%T in (alloc_test addressmove_test port_test devices_test recovery_test standby_test) do (
  echo ===== %%T
  cl /nologo /EHsc /std:c++17 /W4 %%T.cpp /Fo:"%OUT%\\" /Fe:"%OUT%\%%T.exe" >"%OUT%\%%T.build.txt" 2>&1 || (type "%OUT%\%%T.build.txt" & set FAIL=1)
  "%OUT%\%%T.exe" || set FAIL=1
)
echo ===== pipe_test
cl /nologo /EHsc /std:c++17 /W3 pipe_test.cpp ..\CecService\PipeServer.cpp /I ..\CecService /Fo:"%OUT%\\" /Fe:"%OUT%\pipe_test.exe" >"%OUT%\pipe_test.build.txt" 2>&1 || (type "%OUT%\pipe_test.build.txt" & set FAIL=1)
"%OUT%\pipe_test.exe" || set FAIL=1
echo ===== overall FAIL=%FAIL%
exit /b %FAIL%
