#!/bin/sh
# Assemble release files from the built packages into dist/release/ (run the build-*.sh
# scripts first). Every download carries READ ME FIRST, the licence and the third-party
# notices; the Switch zip mirrors the SD card layout. Writes SHA256SUMS.txt.
set -e
cd "$(dirname "$0")/.."
V=$(sed -n 's/^project(laserlords VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
OUT=dist/release
rm -rf "$OUT"; mkdir -p "$OUT"
docs() { cp packaging/READ_ME_FIRST.txt "$1/READ ME FIRST.txt"; cp LICENSE "$1/LICENSE.txt"; cp THIRD_PARTY_NOTICES.md "$1/"; }
tmp=$(mktemp -d)

cp dist/LaserLords-macOS.dmg "$OUT/LaserLords-$V-macOS.dmg"

mkdir -p "$tmp/win/LaserLords"
cp dist/windows/LaserLords.exe dist/windows/laserlords-cli.exe "$tmp/win/LaserLords/"
docs "$tmp/win/LaserLords"
(cd "$tmp/win" && zip -qr - LaserLords) > "$OUT/LaserLords-$V-windows-x64.zip"

mkdir -p "$tmp/linux/LaserLords"
cp dist/linux/laserlords dist/linux/laserlords.desktop dist/linux/icon_1024.png "$tmp/linux/LaserLords/"
chmod +x "$tmp/linux/LaserLords/laserlords"
docs "$tmp/linux/LaserLords"
tar -C "$tmp/linux" -czf "$OUT/LaserLords-$V-linux-x86_64.tar.gz" LaserLords

mkdir -p "$tmp/switch/switch/laserlords"
cp dist/switch/laserlords.nro "$tmp/switch/switch/laserlords/"
docs "$tmp/switch/switch/laserlords"
(cd "$tmp/switch" && zip -qr - switch) > "$OUT/LaserLords-$V-switch.zip"

rm -rf "$tmp"
(cd "$OUT" && shasum -a 256 LaserLords-* > SHA256SUMS.txt)

# point the README's download links at this version (commit the README with the release)
sed -i '' -E "s#(releases/download/)v[0-9.]+/LaserLords-[0-9.]+-#\1v$V/LaserLords-$V-#g; s#LaserLords-[0-9.]+-(macOS|windows|linux|switch)#LaserLords-$V-\1#g; s#Download \(v[0-9.]+#Download (v$V#" README.md
ls -l "$OUT"
