@echo off
cd /d "%~dp0"
g++ -O2 -std=c++17 rdp_server.cpp -o rdp_server.exe -static -lgdiplus -lgdi32 -lws2_32 -lole32 -luser32
if errorlevel 1 (echo Server build FAILED & pause & exit /b 1)
g++ -O2 -std=c++17 rdp_client.cpp -o rdp_client.exe -static -lgdiplus -lgdi32 -lws2_32 -lole32 -luser32
if errorlevel 1 (echo Client build FAILED & pause & exit /b 1)
echo Build OK: rdp_server.exe rdp_client.exe
pause
