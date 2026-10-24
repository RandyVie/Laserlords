#!/bin/sh
# Nintendo Switch homebrew build in devkitPro's Docker image -> dist/switch/laserlords.nro
# Copy it to sdmc:/switch/laserlords/ on the SD card with your own disc image, or send it with
# the Homebrew Menu's netloader:  nxlink -s dist/switch/laserlords.nro
set -e
cd "$(dirname "$0")/.."
docker run --rm --platform linux/amd64 -v "$PWD:/src" -w /src devkitpro/devkita64 sh -c '
    set -e
    cmake -S . -B build-switch -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake -DCMAKE_BUILD_TYPE=Release
    cmake --build build-switch -j"$(nproc)" --target laserlords_switch_nro
'
mkdir -p dist/switch
cp build-switch/laserlords.nro dist/switch/
ls -l dist/switch/laserlords.nro
