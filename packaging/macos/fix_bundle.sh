#!/bin/sh
# Point the app at the SDL3 copy inside the bundle and ad-hoc sign it.
# Usage: fix_bundle.sh path/to/LaserLords.app
set -e
APP="$1"
EXE="$APP/Contents/MacOS/LaserLords"
FW="$APP/Contents/Frameworks"
for lib in "$FW"/libSDL3*.dylib; do
    [ -e "$lib" ] || continue   # static SDL: nothing to relink
    name=$(basename "$lib")
    install_name_tool -id "@rpath/$name" "$lib" 2>/dev/null
    old=$(otool -L "$EXE" | awk '/libSDL3/ {print $1; exit}')
    [ -n "$old" ] && install_name_tool -change "$old" "@rpath/$name" "$EXE" 2>/dev/null
done
codesign --force --deep --sign - "$APP" >/dev/null 2>&1 || echo "warning: codesign failed"
