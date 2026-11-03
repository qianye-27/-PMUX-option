@echo off
setlocal
rem Run the eqy equivalence check inside an OSS CAD Suite environment.
rem Usage:  run_eqy.bat -Cases ..\..\demo   [-Depth 10]
rem The suite is auto-detected; override with:  $env:OSS_CAD="D:\path\to\oss-cad-suite"
set "SUITE="
if not "%OSS_CAD%"=="" if exist "%OSS_CAD%\environment.bat" set "SUITE=%OSS_CAD%"

rem 1) well-known install locations
if not defined SUITE if exist "C:\oss-cad-suite\environment.bat" set "SUITE=C:\oss-cad-suite"
if not defined SUITE if exist "C:\oss-cad-suite\oss-cad-suite\environment.bat" set "SUITE=C:\oss-cad-suite\oss-cad-suite"
if not defined SUITE if exist "C:\tools\oss-cad-suite\environment.bat" set "SUITE=C:\tools\oss-cad-suite"
if not defined SUITE if exist "C:\tools\oss-cad-suite\oss-cad-suite\environment.bat" set "SUITE=C:\tools\oss-cad-suite\oss-cad-suite"
if not defined SUITE if exist "%USERPROFILE%\oss-cad-suite\environment.bat" set "SUITE=%USERPROFILE%\oss-cad-suite"
if not defined SUITE if exist "%USERPROFILE%\oss-cad-suite\oss-cad-suite\environment.bat" set "SUITE=%USERPROFILE%\oss-cad-suite\oss-cad-suite"

rem 2) walk up from this script looking for <...>\tools\oss-cad-suite
if not defined SUITE if exist "%~dp0..\..\..\tools\oss-cad-suite\oss-cad-suite\environment.bat" set "SUITE=%~dp0..\..\..\tools\oss-cad-suite\oss-cad-suite"
if not defined SUITE if exist "%~dp0..\..\..\tools\oss-cad-suite\environment.bat" set "SUITE=%~dp0..\..\..\tools\oss-cad-suite"
if not defined SUITE if exist "%~dp0..\..\..\oss-cad-suite\oss-cad-suite\environment.bat" set "SUITE=%~dp0..\..\..\oss-cad-suite\oss-cad-suite"
if not defined SUITE if exist "%~dp0..\..\..\oss-cad-suite\environment.bat" set "SUITE=%~dp0..\..\..\oss-cad-suite"
if not defined SUITE if exist "%~dp0..\..\..\..\tools\oss-cad-suite\oss-cad-suite\environment.bat" set "SUITE=%~dp0..\..\..\..\tools\oss-cad-suite\oss-cad-suite"
if not defined SUITE if exist "%~dp0..\..\..\..\tools\oss-cad-suite\environment.bat" set "SUITE=%~dp0..\..\..\..\tools\oss-cad-suite"
if not defined SUITE if exist "%~dp0..\..\..\..\oss-cad-suite\oss-cad-suite\environment.bat" set "SUITE=%~dp0..\..\..\..\oss-cad-suite\oss-cad-suite"
if not defined SUITE if exist "%~dp0..\..\..\..\oss-cad-suite\environment.bat" set "SUITE=%~dp0..\..\..\..\oss-cad-suite"
if not defined SUITE if exist "%~dp0..\..\..\..\..\tools\oss-cad-suite\oss-cad-suite\environment.bat" set "SUITE=%~dp0..\..\..\..\..\tools\oss-cad-suite\oss-cad-suite"
if not defined SUITE if exist "%~dp0..\..\..\..\..\tools\oss-cad-suite\environment.bat" set "SUITE=%~dp0..\..\..\..\..\tools\oss-cad-suite"
if not defined SUITE if exist "%~dp0..\..\..\..\..\oss-cad-suite\oss-cad-suite\environment.bat" set "SUITE=%~dp0..\..\..\..\..\oss-cad-suite\oss-cad-suite"
if not defined SUITE if exist "%~dp0..\..\..\..\..\oss-cad-suite\environment.bat" set "SUITE=%~dp0..\..\..\..\..\oss-cad-suite"
if not defined SUITE if exist "%~dp0..\..\..\..\..\..\tools\oss-cad-suite\oss-cad-suite\environment.bat" set "SUITE=%~dp0..\..\..\..\..\..\tools\oss-cad-suite\oss-cad-suite"
if not defined SUITE if exist "%~dp0..\..\..\..\..\..\tools\oss-cad-suite\environment.bat" set "SUITE=%~dp0..\..\..\..\..\..\tools\oss-cad-suite"
if not defined SUITE if exist "%~dp0..\..\..\..\..\..\oss-cad-suite\oss-cad-suite\environment.bat" set "SUITE=%~dp0..\..\..\..\..\..\oss-cad-suite\oss-cad-suite"
if not defined SUITE if exist "%~dp0..\..\..\..\..\..\oss-cad-suite\environment.bat" set "SUITE=%~dp0..\..\..\..\..\..\oss-cad-suite"

if not defined SUITE (
  echo [ERROR] OSS CAD Suite was not found automatically.
  echo         Set it once and retry, e.g. in PowerShell:
  echo             $env:OSS_CAD = "C:\path\to\oss-cad-suite"
  echo         or in cmd:
  echo             set OSS_CAD=C:\path\to\oss-cad-suite
  exit /b 1
)
echo [info] OSS CAD Suite: %SUITE%
call "%SUITE%\environment.bat"
rem eqy needs make; the suite does not ship one, so reuse any make on PATH
where make >nul 2>&1
if errorlevel 1 if exist "D:\msys64\usr\bin\make.exe" set "PATH=D:\msys64\usr\bin;%PATH%"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_eqy.ps1" %*
endlocal
