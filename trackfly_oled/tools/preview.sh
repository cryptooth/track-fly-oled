#!/bin/zsh
# Renders the screens to tools/preview/*.png with the firmware's own drawing code.
set -e
cd "${0:A:h}/.."
mkdir -p tools/preview
OUT=$(mktemp -d)
cc -c -o "$OUT/qr.o" tf_qrcode.c
c++ -std=c++17 -O1 -o "$OUT/preview" tools/preview.cpp gfx.cpp screens.cpp "$OUT/qr.o" -lz
"$OUT/preview"
rm -rf "$OUT"
