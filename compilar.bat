@echo off
cmake -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
echo.
echo Listo: build\AQDAW_artefacts\Release\AQDAW.exe
pause
