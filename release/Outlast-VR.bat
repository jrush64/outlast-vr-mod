@echo off
setlocal enableextensions enabledelayedexpansion
title Outlast VR

echo(
echo   OUTLAST VR
echo(

REM --- must be run from the game folder, next to OLGame.exe ---
set "G=%~dp0"
if "!G:~-1!"=="\" set "G=!G:~0,-1!"

if not exist "!G!\OLGame.exe" (
  echo   Put all the mod files in your Outlast\Binaries\Win64 folder
  echo   and run this from there.
  pause & exit /b 1
)
if not exist "!G!\d3d9.dll" (
  echo   d3d9.dll is missing - copy all the files across, not just this one.
  pause & exit /b 1
)
tasklist /fi "imagename eq OLGame.exe" | find /i "OLGame.exe" >nul
if not errorlevel 1 ( echo   Close Outlast first. & pause & exit /b 1 )

REM --- Documents can be redirected by OneDrive, so ask Windows where it is ---
for /f "usebackq delims=" %%D in (`powershell -NoProfile -Command "[Environment]::GetFolderPath('MyDocuments')"`) do set "C=%%D\My Games\Outlast\OLGame\Config"

if not exist "!C!\OLGame.ini" (
  echo   Run Outlast once first, then try again.
  pause & exit /b 1
)

set "DONE=0"
if exist "!C!\OLGame.ini.bak-outlastvr" set "DONE=1"

echo     [1] Install
echo     [2] Uninstall
echo(
set /p "A=Enter 1 or 2 [1]: "
if "!A!"=="2" goto REMOVE


:INSTALL
if not exist "!C!\OLGame.ini.bak-outlastvr" copy /y "!C!\OLGame.ini" "!C!\OLGame.ini.bak-outlastvr" >nul
if not exist "!C!\OLSystemSettings.ini.bak-outlastvr" copy /y "!C!\OLSystemSettings.ini" "!C!\OLSystemSettings.ini.bak-outlastvr" >nul

REM --- wide FOV: the game renders about half the camera FOV per eye ---
powershell -NoProfile -Command "$p='!C!\OLGame.ini'; (Get-Content $p) -replace '^DefaultFOV=.*','DefaultFOV=120.0' -replace '^RunningFOV=.*','RunningFOV=126.0' -replace '^CamcorderMaxFOV=.*','CamcorderMaxFOV=120.0' -replace '^CamcorderNVMaxFOV=.*','CamcorderNVMaxFOV=120.0' | Set-Content $p"

REM --- depth of field ON (it carries the colour grading), motion blur OFF ---
REM --- only the [SystemSettings] block, so quality presets keep their values ---
powershell -NoProfile -Command "$p='!C!\OLSystemSettings.ini'; $s=''; (Get-Content $p | %%{ $l=$_; if($l -match '^\[(.+)\]'){$s=$Matches[1]}; if($s -eq 'SystemSettings'){ if($l -match '^DepthOfField='){$l='DepthOfField=True'} elseif($l -match '^MotionBlur='){$l='MotionBlur=False'} elseif($l -match '^MotionBlurPause='){$l='MotionBlurPause=False'} }; $l }) | Set-Content $p"

echo(
echo   INSERT opens settings. G or double-tap L1 recenters.
echo(
pause & exit /b 0


:REMOVE
if "!DONE!"=="0" ( echo   Not installed. & pause & exit /b 0 )

REM --- restore first: leaving FOV 120 behind would break the normal game ---
if exist "!C!\OLGame.ini.bak-outlastvr" move /y "!C!\OLGame.ini.bak-outlastvr" "!C!\OLGame.ini" >nul
if exist "!C!\OLSystemSettings.ini.bak-outlastvr" move /y "!C!\OLSystemSettings.ini.bak-outlastvr" "!C!\OLSystemSettings.ini" >nul

del /f /q "!G!\d3d9.dll" >nul 2>&1
del /f /q "!G!\openxr_loader.dll" >nul 2>&1

echo(
echo   Uninstalled.
echo(
pause & exit /b 0
