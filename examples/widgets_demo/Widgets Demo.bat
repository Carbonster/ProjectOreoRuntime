@echo off
cd /d "%~dp0"
pymhf run widgets_demo.py
set "exit_code=%errorlevel%"
if not "%exit_code%"=="0" (
    echo.
    echo Widgets demo failed to start or exited with an error. Exit code: %exit_code%
    echo Check the logs folder for details.
    pause
)
exit /b %exit_code%
