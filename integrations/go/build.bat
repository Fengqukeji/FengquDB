@echo off
setlocal DisableDelayedExpansion
go run "%~dp0Tools\sdktool.go" build %*
exit /b %ERRORLEVEL%
