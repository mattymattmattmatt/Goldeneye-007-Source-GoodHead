@echo off
echo GoldenEye: Source VR — install helper
echo.
echo 1. Build GESVR.sln  (Release | Win32)   OR use a prebuilt dist\d3d9.dll
echo 2. Install Source SDK Base 2007 (Steam app 218)
echo 3. Install GoldenEye: Source into steamapps\sourcemods\gesource
echo 4. Start SteamVR, then run Launch-GESVR.bat
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Launch-GESVR.ps1"
if errorlevel 1 pause
