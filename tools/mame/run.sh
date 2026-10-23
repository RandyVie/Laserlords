#!/bin/sh
# Run the real CD-i (MAME cdimono1) headless with a Lua script, as fast as possible.
#   tools/mame/run.sh WORKDIR DISC.chd SCRIPT.lua SECONDS [extra mame args...]
# Snapshots go to WORKDIR/snap/cdimono1/NNNN.png; script settings come from environment
# variables (see each .lua). Example:
#   ROUTE="80:r:3" STOP=95 SNAP=0.5 tools/mame/run.sh /tmp/mw disc.chd tools/mame/route.lua 96
# Add "-wavwrite out.wav" (and drop -sound none by setting MAME_SOUND=1) to record audio.
work=$1; disc=$2; script=$3; secs=$4; shift 4
sound="-sound none"; [ -n "$MAME_SOUND" ] && sound=""
exec mame cdimono1 -rompath "$work/roms" -cdrom "$disc" -video none -seconds_to_run "$secs" $sound \
    -nothrottle -skip_gameinfo -cfg_directory "$work/cfg" -nvram_directory "$work/nvram" \
    -snapshot_directory "$work/snap" -autoboot_script "$script" "$@"
