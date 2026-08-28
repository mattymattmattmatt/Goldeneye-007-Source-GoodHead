@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Launch-GESVR.ps1"
if errorlevel 1 pause
