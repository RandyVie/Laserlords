#!/bin/sh
# Decompile every program module in ext/ to out/c/<module>.c with Ghidra headless.
set -e
cd "$(dirname "$0")/.."
GH=${GHIDRA_HEADLESS:-/opt/homebrew/opt/ghidra/libexec/support/analyzeHeadless}
PY=${PY:-.venv/bin/python}
mkdir -p out/ghidra out/c
for m in ${@:-cdi_launcher cdi_tribes cdi_space cdi_nvrui cdi_bumperanim}; do
  $PY -I tools/ghidra_entries.py ext/$m out/ghidra/$m.entries
  "$GH" out/ghidra LaserLords -import ext/$m -overwrite -noanalysis \
    -processor 68000:BE:32:default -loader BinaryLoader -loader-baseAddr 0 \
    -scriptPath tools/ghidra -postScript DecompileOS9.java "$PWD/out/ghidra/$m.entries" "$PWD/out/c/$m.c" \
    2>&1 | grep -E "decompiled|ERROR|Exception" || true
done
