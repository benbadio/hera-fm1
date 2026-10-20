#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Run HERA on the host simulator: build/hera/hera.wav and a PNG of every page (tests/hera_scenario.txt)
set -e
cd "$(dirname "$0")/.."
mkdir -p build/hera
rm -f build/hera/hera-*.ppm build/hera/hera-*.png
cc -O2 -Wall -Wno-unused-function -DDINGHY_HOST -DAPP_VER="\"dev\"" "-D__attribute__(x)=" \
   -Ifirmware/src -Ifirmware/src/dsp -Ifirmware/gen -o build/hera/hera_sim firmware/src/hera_app.c
build/hera/hera_sim build/hera/hera tests/hera_scenario.txt | grep -v "MIDI out" || true
python3 -I tools/hera_sheet.py build/hera/sheet.ppm $(ls build/hera/hera-*.ppm | sort -t- -k2 -n)
for p in build/hera/hera-*.ppm build/hera/sheet.ppm; do
    [ -f "$p" ] || continue
    png="${p%.ppm}.png"
    if command -v sips >/dev/null 2>&1; then sips -s format png "$p" --out "$png" >/dev/null 2>&1
    else python3 -c "from PIL import Image; Image.open('$p').save('$png')"; fi
    rm -f "$p"
done
ls build/hera/*.png build/hera/hera.wav
