
@echo off
setlocal EnableExtensions DisableDelayedExpansion
title Windows FilterKeys Configurator

:MENU
cls
echo.
echo ==================================================
echo             WINDOWS FILTERKEYS
echo ==================================================
echo.
echo   [1] WINDOWS STANDARD - FilterKeys OFF
echo   [2] REPEATKEYS - Custom configuration
echo   [3] BOUNCEKEYS - Custom configuration
echo   [4] READ CURRENT CONFIGURATION
echo   [5] EXIT
echo.
choice /c 12345 /n /m "Select option: "

if errorlevel 5 goto END
if errorlevel 4 goto READ
if errorlevel 3 goto BOUNCE
if errorlevel 2 goto REPEAT
if errorlevel 1 goto WINDOWS

:WINDOWS
call :RUN OFF 0 0 0 0
goto MENU

:REPEAT
cls
echo.
echo ==================================================
echo                  REPEATKEYS
echo ==================================================
echo.
echo ENTER keeps the proposed default.
echo Zero is allowed for iWaitMSec.
echo.

set "WAIT_MS=0"
set "DELAY_MS=250"
set "REPEAT_MS=50"

set /p "WAIT_MS=iWaitMSec [0]: "
if not defined WAIT_MS set "WAIT_MS=0"

set /p "DELAY_MS=iDelayMSec [250]: "
if not defined DELAY_MS set "DELAY_MS=250"

set /p "REPEAT_MS=iRepeatMSec [50]: "
if not defined REPEAT_MS set "REPEAT_MS=50"

call :RUN REPEAT "%WAIT_MS%" "%DELAY_MS%" "%REPEAT_MS%" 0
goto MENU

:BOUNCE
cls
echo.
echo ==================================================
echo                   BOUNCEKEYS
echo ==================================================
echo.
echo RepeatKeys and SlowKeys will be disabled.
echo.

set "BOUNCE_MS=32"

set /p "BOUNCE_MS=iBounceMSec [32]: "
if not defined BOUNCE_MS set "BOUNCE_MS=32"

call :RUN BOUNCE 0 0 0 "%BOUNCE_MS%"
goto MENU

:READ
call :RUN READ 0 0 0 0
goto MENU

:RUN
echo.
powershell.exe -NoProfile -ExecutionPolicy Bypass ^
    -File "%~dp0Set-FilterKeys.ps1" ^
    -Mode "%~1" ^
    -WaitMs "%~2" ^
    -DelayMs "%~3" ^
    -RepeatMs "%~4" ^
    -BounceMs "%~5"

set "RC=%ERRORLEVEL%"

echo.
if "%RC%"=="0" (
    echo [SUCCESS] Operation completed.
) else (
    echo [ERROR] Operation failed. Exit code: %RC%
)

echo.
pause
goto :eof

:END
endlocal
exit /b 0
