@echo off
rem Build the Lilith Reader installer with Inno Setup 6.
rem Usage: build_installer.bat   (requires: winget install JRSoftware.InnoSetup)
setlocal
set "ISCC=%LocalAppData%\Programs\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" set "ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" (
  echo Inno Setup 6 not found. Install it with:
  echo   winget install JRSoftware.InnoSetup
  exit /b 1
)
if not exist "%~dp0..\bin\Release\LilithReader.exe" (
  echo LilithReader.exe not found. Run build.bat at repo root first.
  exit /b 1
)
"%ISCC%" "%~dp0LilithReader.iss"
endlocal
