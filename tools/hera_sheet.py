#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Tile host-simulator screenshots (PPM) into one contact sheet: hera_sheet.py OUT.ppm A.ppm B.ppm ..."""
import sys
files = sys.argv[2:]
cols = 4; W = H = 240; G = 6
rows = (len(files) + cols - 1) // cols
SW, SH = cols * (W + G), rows * (H + G)
out = bytearray([40, 40, 40] * (SW * SH))
for n, f in enumerate(files):
    d = open(f, 'rb').read()
    i = d.index(b'255\n') + 4
    px = d[i:]
    ox, oy = (n % cols) * (W + G), (n // cols) * (H + G)
    for y in range(H):
        o = ((oy + y) * SW + ox) * 3
        out[o:o + W * 3] = px[y * W * 3:(y + 1) * W * 3]
open(sys.argv[1], 'wb').write(b'P6 %d %d 255\n' % (SW, SH) + out)
