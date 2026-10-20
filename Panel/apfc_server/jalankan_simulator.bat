@echo off
cd /d "%~dp0"
python tools\simulator.py %*
pause
