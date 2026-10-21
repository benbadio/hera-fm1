#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Build HERA for the browser: web/hera.wasm (the real firmware, freestanding WebAssembly).
# Needs LLVM with the wasm32 target and wasm-ld (macOS: brew install llvm lld).
set -e
cd "$(dirname "$0")/.."
VER=$(cat VERSION)
PRODUCT=$(echo "${VER%%-*}" | awk -F. '{ printf "FM-1_43%d%02d%02d", $1, $2, $3 }')   # as identity() in tools/build.py
CLANG=${WASM_CC:-$(command -v /opt/homebrew/opt/llvm/bin/clang || command -v clang)}
EXPORTS="web_init web_step web_audio web_audio_buf web_button web_keys web_enc web_midi_in web_midi_out web_fb \
web_leds web_store web_store_len web_reload web_status web_status_size web_style_name web_var_name web_sound_name \
web_param_label web_param_value web_page web_scratch j6_engine_get j6_engine_set j6_engine_cmd j6_engine_pattern \
j6_engine_write j6_chord_name j6_chord_genre j6_chord_raw j6_param_info"
EX=""
for e in $EXPORTS; do EX="$EX -Wl,--export=$e"; done
$CLANG --target=wasm32 -O2 -nostdlib -ffreestanding -fno-builtin -Wall -Wno-unused-function \
    -DDINGHY_WEB -DDINGHY_HOST -DAPP_VER="\"$VER\"" "-D__attribute__(x)=" \
    -Ifirmware/src -Ifirmware/src/j6 -Ifirmware/src/dsp -Ifirmware/gen \
    -Wl,--no-entry -Wl,--export-memory -Wl,--initial-memory=8388608 $EX \
    -o web/hera.wasm firmware/src/hera_app.c
mkdir -p build/web
cc -std=c99 -Ifirmware/src/j6 -o build/web/gen_ids web/src/gen_ids.c && build/web/gen_ids > web/hera_ids.js
mkdir -p web/data web/screens && cp data/phrases_6_9.json web/data/ && cp docs/screens/*.png web/screens/
sed -i.bak -E "s/^const meta = .*/const meta = {\"version\": \"$VER\", \"product\": \"$PRODUCT\", \"pkg\": \"firmware\/hera.fwsc\"};/" web/install.html && rm -f web/install.html.bak
# cache-busting: Pages lets browsers keep files for 10 minutes, so every asset link carries this build's stamp
STAMP=${GITHUB_SHA:-$(git rev-parse --short HEAD 2>/dev/null || echo dev)}
STAMP=$(echo "$STAMP" | cut -c1-10)
for f in web/index.html web/manual.html web/hera.js; do
    sed -i.bak -E "s/\?v=[A-Za-z0-9]+/?v=$STAMP/g" "$f" && rm -f "$f.bak"
done
ls -la web/hera.wasm web/hera_ids.js
