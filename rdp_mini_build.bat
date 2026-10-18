@echo off
cd /d "%~dp0"
g++ -O2 -std=c++17 rdp_mini.cpp -o rdp_mini.exe -static-libgcc -static-libstdc++ -lgdiplus -lgdi32 -lws2_32 -lole32 -luser32
if errorlevel 1 (echo Build FAILED) else (echo Build OK: rdp_mini.exe)
pause
