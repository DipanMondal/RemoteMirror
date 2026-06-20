:: This file is for running the entire build process freshly
:: If you don't want to run the entire build process repeatedly,
:: you can ignore it. 

@echo off

rmdir /s /q build
echo ============================
echo Removed old build folder.
echo ============================

cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release

echo ===========================
echo New build folder ready.
echo ===========================