#!/bin/sh
# Windows x86_64 cross build with MinGW-w64: dist/windows/LaserLords.exe (single file).
# (On Windows itself: cmake -S . -B build -DLL_BUNDLED_SDL=ON && cmake --build build --config Release)
set -e
cd "$(dirname "$0")/.."
cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=packaging/windows/mingw-toolchain.cmake -DLL_BUNDLED_SDL=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-win -j --target LaserLordsGui laserlords
mkdir -p dist/windows
cp build-win/LaserLords.exe build-win/laserlords-cli.exe dist/windows/
x86_64-w64-mingw32-strip dist/windows/*.exe
echo "dist/windows/LaserLords.exe"
