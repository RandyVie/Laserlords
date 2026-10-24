#!/bin/sh
# macOS: builds LaserLords.app (SDL3 inside) and a .dmg in dist/.
#   UNIVERSAL=1 scripts/build-macos.sh   -> Apple Silicon + Intel (builds SDL3 from source)
set -e
cd "$(dirname "$0")/.."
EXTRA=""
if [ "${UNIVERSAL:-0}" = 1 ]; then
    EXTRA="-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64 -DLL_BUNDLED_SDL=ON"
    BUILD=build-macos-universal
else
    BUILD=build-macos
fi
cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 $EXTRA
cmake --build "$BUILD" -j --target LaserLordsApp
mkdir -p dist
rm -rf dist/LaserLords.app dist/LaserLords-macOS.dmg
cp -R "$BUILD/LaserLords.app" dist/
# disk image: the app, an Applications shortcut to drag it onto, the read-me and the licences
stage=$(mktemp -d)
cp -R dist/LaserLords.app "$stage/"
ln -s /Applications "$stage/Applications"
cp packaging/READ_ME_FIRST.txt "$stage/READ ME FIRST.txt"
cp LICENSE "$stage/LICENSE.txt"
cp THIRD_PARTY_NOTICES.md "$stage/THIRD_PARTY_NOTICES.md"
hdiutil create -quiet -volname "Laser Lords" -srcfolder "$stage" -ov -format UDZO dist/LaserLords-macOS.dmg
rm -rf "$stage"
echo "dist/LaserLords.app and dist/LaserLords-macOS.dmg"
