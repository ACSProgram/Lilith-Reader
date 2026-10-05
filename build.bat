@echo off
rem Build LilithReader (Release x64) using VS 18 (2026) MSBuild.
rem Usage: build.bat
setlocal
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
  echo Visual Studio not found.
  exit /b 1
)
"%VSPATH%\MSBuild\Current\Bin\MSBuild.exe" "%~dp0src\LilithReader.vcxproj" -p:Configuration=Release -p:Platform=x64 -m -nologo
endlocal
