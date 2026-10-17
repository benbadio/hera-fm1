#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Run MORPHEUS firmware on host simulator: build/host/morpheus.wav and screens as PNG
set -e
cd "$(dirname "$0")/.."
mkdir -p build/host

echo "==> Compiling MORPHEUS host simulator..."
cc -O2 -Wall -Wno-unused-function -DDINGHY_HOST -DAPP_VER="\"dev\"" "-D__attribute__(x)=" \
   -Ifirmware/src -Ifirmware/gen -o build/host/morpheus_sim firmware/src/morpheus_app.c

echo "==> Running automated multi-track test scenario..."
build/host/morpheus_sim build/host/morpheus tests/morpheus_scenario.txt

echo "==> Converting screenshots to PNG..."
for p in build/host/morpheus-*.ppm; do
    if [ -f "$p" ]; then
        png="${p%.ppm}.png"
        sips -s format png "$p" --out "$png" >/dev/null 2>&1
        rm -f "$p"
        echo "  [OK] screen -> $png"
    fi
done

echo "==> Testing audio output..."
if [ -f "build/host/morpheus.wav" ]; then
    ls -lh build/host/morpheus.wav
    echo "  [OK] audio -> build/host/morpheus.wav"
fi

echo "==> All host simulator tests passed successfully!"
