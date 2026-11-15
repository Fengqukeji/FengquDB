@echo off
setlocal DisableDelayedExpansion
chcp 65001 >nul
set "COMPILER=%~dp0Tools\fqdb-codegen.exe"
if not exist "%COMPILER%" set "COMPILER=%~dp0Editor\Tools\fqdb-codegen.exe"
if not exist "%COMPILER%" (
  echo Missing bundled generator. Extract the complete language SDK folder.
  if not defined CI pause
  exit /b 1
)
if "%~1"=="" (
  "%COMPILER%" project --sdk "%~dp0."
) else (
  "%COMPILER%" project --sdk "%~dp0." --root "%~f1\."
)
set "RESULT=%ERRORLEVEL%"
if "%RESULT%"=="0" (echo Entity codecs are up to date.) else (echo Generation failed. Original source files were not modified.)
if not defined CI pause
endlocal & exit /b %RESULT%
