#!/usr/bin/env python3
"""Generate MORPHEUS 8-bit White Rabbit Matrix Logo & Splash Screen assets."""

import os
import subprocess
import math

# 32x32 Pixel Art White Rabbit Silhouette
RABBIT_32 = [
    "................................",
    ".........█████..................",
    "........███████......█████......",
    "........██...██.....███████.....",
    "........██...██.....██...██.....",
    "........██...██....██....██.....",
    "........██...██....██....██.....",
    "........██...██...██....██......",
    "........██...██...██....██......",
    ".........██.██....██...██.......",
    ".........██.██...██...██........",
    "..........███....██████.........",
    ".........██████████████.........",
    ".......██████████████████.......",
    "......████████████████████......",
    "......██.███..████.███..███.....",
    "......██.███..████.███..███.....",
    "......█████████████████████.....",
    "......████████..███████████.....",
    "......██████......█████████.....",
    ".......████████████████████.....",
    "........█████████████████.......",
    ".......██████████████████.......",
    "......████████████████████......",
    ".....██..██████████████..██.....",
    ".....██..██████████████..██.....",
    "....███...████.████.███..██.....",
    "....████..█████████████..█......",
    ".....██...██████..█████.........",
    "..........█████....████.........",
    "........███████...███████.......",
    ".......█████████.█████████......",
]

# Complete 5x7 ASCII Bitmap Font
FONT_5x7 = {
    'A': ["01110", "10001", "10001", "11111", "10001", "10001", "10001"],
    'B': ["11110", "10001", "10001", "11110", "10001", "10001", "11110"],
    'C': ["01111", "10000", "10000", "10000", "10000", "10000", "01111"],
    'D': ["11110", "10001", "10001", "10001", "10001", "10001", "11110"],
    'E': ["11111", "10000", "10000", "11110", "10000", "10000", "11111"],
    'F': ["11111", "10000", "10000", "11110", "10000", "10000", "10000"],
    'G': ["01111", "10000", "10000", "10111", "10001", "10001", "01111"],
    'H': ["10001", "10001", "10001", "11111", "10001", "10001", "10001"],
    'I': ["11111", "00100", "00100", "00100", "00100", "00100", "11111"],
    'J': ["00011", "00001", "00001", "00001", "10001", "10001", "01110"],
    'K': ["10001", "10010", "10100", "11000", "10100", "10010", "10001"],
    'L': ["10000", "10000", "10000", "10000", "10000", "10000", "11111"],
    'M': ["10001", "11011", "10101", "10001", "10001", "10001", "10001"],
    'N': ["10001", "11001", "10101", "10011", "10001", "10001", "10001"],
    'O': ["01110", "10001", "10001", "10001", "10001", "10001", "01110"],
    'P': ["11110", "10001", "10001", "11110", "10000", "10000", "10000"],
    'Q': ["01110", "10001", "10001", "10001", "10101", "10010", "01101"],
    'R': ["11110", "10001", "10001", "11110", "10100", "10010", "10001"],
    'S': ["01111", "10000", "10000", "01110", "00001", "00001", "11110"],
    'T': ["11111", "00100", "00100", "00100", "00100", "00100", "00100"],
    'U': ["10001", "10001", "10001", "10001", "10001", "10001", "01110"],
    'V': ["10001", "10001", "10001", "10001", "10001", "01010", "00100"],
    'W': ["10001", "10001", "10001", "10101", "10101", "11011", "10001"],
    'X': ["10001", "10001", "01010", "00100", "01010", "10001", "10001"],
    'Y': ["10001", "10001", "01010", "00100", "00100", "00100", "00100"],
    'Z': ["11111", "00001", "00010", "00100", "01000", "10000", "11111"],
    '0': ["01110", "10011", "10101", "11001", "10001", "10001", "01110"],
    '1': ["00100", "01100", "00100", "00100", "00100", "00100", "01110"],
    '2': ["01110", "10001", "00001", "00010", "00100", "01000", "11111"],
    '3': ["01110", "10001", "00001", "00110", "00001", "10001", "01110"],
    '4': ["00010", "00110", "01010", "10010", "11111", "00010", "00010"],
    '5': ["11111", "10000", "11110", "00001", "00001", "10001", "01110"],
    '6': ["01110", "10000", "11110", "10001", "10001", "10001", "01110"],
    '7': ["11111", "00001", "00010", "00100", "00100", "00100", "00100"],
    '8': ["01110", "10001", "10001", "01110", "10001", "10001", "01110"],
    '9': ["01110", "10001", "10001", "01111", "00001", "00001", "01110"],
    ' ': ["00000", "00000", "00000", "00000", "00000", "00000", "00000"],
    '-': ["00000", "00000", "00000", "11111", "00000", "00000", "00000"],
    '/': ["00001", "00010", "00010", "00100", "01000", "01000", "10000"],
    '.': ["00000", "00000", "00000", "00000", "00000", "01100", "01100"],
    ':': ["00000", "01100", "01100", "00000", "01100", "01100", "00000"],
}

def make_ppm():
    W, H = 240, 240
    pixels = [[[0, 0, 0] for _ in range(W)] for _ in range(H)]

    # 1. Phosphor Matrix Background: Deep void with scanline raster
    for y in range(H):
        for x in range(W):
            scan = 0.82 if (y % 2 == 0) else 1.0
            # Faint digital phosphor matrix pattern
            noise = (x * 11 + y * 19) % 9
            r = int(1 * scan)
            g = int((4 + (noise if noise < 4 else 0)) * scan)
            b = int(2 * scan)
            pixels[y][x] = [r, g, b]

    # 2. Rabbit Coordinates (Scale 3 -> 96x96 px)
    scale = 3
    ox = (W - 32 * scale) // 2
    oy = 20

    glow_grid = [[0.0 for _ in range(W)] for _ in range(H)]
    rabbit_pixels = []

    for ry in range(32):
        row = RABBIT_32[ry]
        for rx in range(32):
            if rx < len(row) and row[rx] == '█':
                for sy in range(scale):
                    for sx in range(scale):
                        px = ox + rx * scale + sx
                        py = oy + ry * scale + sy
                        rabbit_pixels.append((px, py))

    # Glow field calculation
    for px, py in rabbit_pixels:
        for dy in range(-10, 11):
            for dx in range(-10, 11):
                gx, gy = px + dx, py + dy
                if 0 <= gx < W and 0 <= gy < H:
                    dist = math.sqrt(dx * dx + dy * dy)
                    if dist <= 10.0:
                        intensity = (1.0 - dist / 10.0) ** 1.6
                        glow_grid[gy][gx] = max(glow_grid[gy][gx], intensity)

    # Render Phosphor Glow Layer
    for y in range(H):
        for x in range(W):
            g_val = glow_grid[y][x]
            if g_val > 0.04:
                base = pixels[y][x]
                gr = int(0)
                gg = int(255 * g_val * 0.8)
                gb = int(102 * g_val * 0.5)
                pixels[y][x] = [
                    min(255, base[0] + gr),
                    min(255, base[1] + gg),
                    min(255, base[2] + gb)
                ]

    # Render Rabbit Body: Crisp white with radiant phosphor edge
    for px, py in rabbit_pixels:
        pixels[py][px] = [255, 255, 255]

    # Eye glint: Cyan cybernetic eye with white center
    eye_x = ox + 11 * scale
    eye_y = oy + 15 * scale
    for dy in range(scale):
        for dx in range(scale):
            if 0 <= eye_x + dx < W and 0 <= eye_y + dy < H:
                pixels[eye_y + dy][eye_x + dx] = [0, 240, 255]
    if 0 <= eye_x + 1 < W and 0 <= eye_y + 1 < H:
        pixels[eye_y + 1][eye_x + 1] = [255, 255, 255]

    # Helper to draw bitmap text with glow
    def draw_text(text, start_x, start_y, char_scale, color, glow_color=None):
        cx = start_x
        for ch in text:
            pattern = FONT_5x7.get(ch, FONT_5x7[' '])
            for fy in range(7):
                row = pattern[fy]
                for fx in range(5):
                    if row[fx] == '1':
                        for sy in range(char_scale):
                            for sx in range(char_scale):
                                px = cx + fx * char_scale + sx
                                py = start_y + fy * char_scale + sy
                                if 0 <= px < W and 0 <= py < H:
                                    if glow_color:
                                        for gdy in (-1, 0, 1):
                                            for gdx in (-1, 0, 1):
                                                if 0 <= px + gdx < W and 0 <= py + gdy < H:
                                                    base = pixels[py + gdy][px + gdx]
                                                    pixels[py + gdy][px + gdx] = [
                                                        max(base[0], glow_color[0]),
                                                        max(base[1], glow_color[1]),
                                                        max(base[2], glow_color[2])
                                                    ]
                                    pixels[py][px] = color
            cx += 6 * char_scale

    # Title: "MORPHEUS"
    title = "MORPHEUS"
    title_scale = 3
    tx = (W - (len(title) * 6 - 1) * title_scale) // 2
    # First draw title glow
    draw_text(title, tx, 136, title_scale, [255, 255, 255], [0, 255, 102])

    # Version Tagline: "VERSION 1.0.0"
    tag = "VERSION 1.0.0"
    tag_scale = 1
    tag_x = (W - (len(tag) * 6 - 1) * tag_scale) // 2
    draw_text(tag, tag_x, 172, tag_scale, [0, 255, 102], [0, 140, 50])

    # Subtitle: "8-TRACK REBIRTH GROOVEBOX"
    sub = "8-TRACK REBIRTH GROOVEBOX"
    sub_scale = 1
    sx = (W - (len(sub) * 6 - 1) * sub_scale) // 2
    draw_text(sub, sx, 188, sub_scale, [255, 185, 30], None)

    # Secondary Subtitle: "FM-1 OPEN // PLATFORM 8000013"
    sub2 = "FM-1 OPEN // ID: 8000013"
    s2x = (W - (len(sub2) * 6 - 1) * sub_scale) // 2
    draw_text(sub2, s2x, 204, sub_scale, [0, 220, 255], None)

    # Frame Border: Matrix Phosphor Green Double Bezel
    for x in range(W):
        pixels[2][x] = [0, 200, 80]
        pixels[H - 3][x] = [0, 200, 80]
        pixels[4][x] = [0, 90, 35]
        pixels[H - 5][x] = [0, 90, 35]
    for y in range(H):
        pixels[y][2] = [0, 200, 80]
        pixels[y][W - 3] = [0, 200, 80]
        pixels[y][4] = [0, 90, 35]
        pixels[y][W - 5] = [0, 90, 35]

    # Save to PPM
    out_ppm = "build/host/morpheus_splash.ppm"
    os.makedirs("build/host", exist_ok=True)
    with open(out_ppm, "wb") as f:
        f.write(f"P6\n{W} {H}\n255\n".encode())
        raw = bytearray()
        for y in range(H):
            for x in range(W):
                raw.extend(pixels[y][x])
        f.write(raw)

    print(f"Generated PPM: {out_ppm}")
    out_png = "assets/morpheus_splash.png"
    subprocess.run(["ffmpeg", "-y", "-i", out_ppm, out_png], check=True)
    print(f"Generated PNG: {out_png}")

    # Also generate a high-res 800x800 banner version for GitHub Readme
    banner_png = "assets/morpheus_banner.png"
    subprocess.run(["ffmpeg", "-y", "-i", out_ppm, "-vf", "scale=720:720:flags=neighbor", banner_png], check=True)
    print(f"Generated Banner PNG: {banner_png}")

if __name__ == "__main__":
    make_ppm()
