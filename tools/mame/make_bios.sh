#!/bin/sh
# Build a working MAME "cdimono1" BIOS set in a scratch folder from the user's own zips.
# MAME 0.289's cdimono1 wants cdi200/cdi220/cdi220b plus the two 68HC05 MCU dumps
# (servo zx405037p, slave zx405042p), which come in cdi910.zip. Nothing here is committed.
#   tools/mame/make_bios.sh "CDi Bios" WORKDIR      -> WORKDIR/roms/cdimono1.zip
set -e
[ -d "$1" ] && [ -n "$2" ] || { echo "usage: $0 BIOS_DIR WORKDIR" >&2; exit 2; }
mkdir -p "$2"
bios=$(cd "$1" && pwd); work=$(cd "$2" && pwd)
mkdir -p "$work/roms" "$work/tmp" "$work/cfg" "$work/nvram"
cp "$bios/cdimono1.zip" "$work/roms/cdimono1.zip"
( cd "$work/tmp" && unzip -o -q "$bios/cdi910.zip" 'zx405037p*' 'zx405042p*' && zip -q ../roms/cdimono1.zip zx405037p* zx405042p* )
mame -rompath "$work/roms" -verifyroms cdimono1 | tail -1
