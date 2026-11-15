@echo off
setlocal DisableDelayedExpansion
go run "%~dp0Tools\sdktool.go" run %*
exit /b %ERRORLEVEL%
