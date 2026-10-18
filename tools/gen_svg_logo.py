#!/usr/bin/env python3
"""Generate crisp SVG vector logos for MORPHEUS 8-Bit White Rabbit with Matrix Green Glow."""

import os

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

def generate_svg(filename, width=480, height=480):
    os.makedirs(os.path.dirname(filename), exist_ok=True)
    scale = 8  # 32 * 8 = 256px rabbit
    rx_offset = (width - 32 * scale) // 2
    ry_offset = 32

    rects = []
    for y in range(32):
        row = RABBIT_32[y]
        for x in range(32):
            if x < len(row) and row[x] == '█':
                px = rx_offset + x * scale
                py = ry_offset + y * scale
                rects.append(f'<rect x="{px}" y="{py}" width="{scale}" height="{scale}" />')

    # Eye position
    eye_x = rx_offset + 11 * scale
    eye_y = ry_offset + 15 * scale

    svg_content = f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" width="100%" height="100%" style="background:#050706;">
  <defs>
    <!-- Matrix Grid Lines -->
    <pattern id="scanlines" width="100" height="4" patternUnits="userSpaceOnUse">
      <line x1="0" y1="0" x2="100" y2="0" stroke="rgba(0,0,0,0.4)" stroke-width="1.5" />
    </pattern>

    <!-- Neon Matrix Green Glow Filter -->
    <filter id="matrixGlow" x="-30%" y="-30%" width="160%" height="160%">
      <feGaussianBlur stdDeviation="12" result="blur1" />
      <feGaussianBlur stdDeviation="6" result="blur2" />
      <feMerge>
        <feMergeNode in="blur1" />
        <feMergeNode in="blur2" />
        <feMergeNode in="SourceGraphic" />
      </feMerge>
    </filter>

    <filter id="textGlow" x="-20%" y="-20%" width="140%" height="140%">
      <feGaussianBlur stdDeviation="4" result="blur" />
      <feMerge>
        <feMergeNode in="blur" />
        <feMergeNode in="SourceGraphic" />
      </feMerge>
    </filter>

    <!-- Radial Background Vignette -->
    <radialGradient id="matrixVignette" cx="50%" cy="40%" r="55%">
      <stop offset="0%" stop-color="#0a2212" stop-opacity="0.8" />
      <stop offset="65%" stop-color="#040d07" stop-opacity="0.95" />
      <stop offset="100%" stop-color="#010302" stop-opacity="1" />
    </radialGradient>
  </defs>

  <!-- Background Layer -->
  <rect width="{width}" height="{height}" fill="url(#matrixVignette)" />
  <rect width="{width}" height="{height}" fill="url(#scanlines)" pointer-events="none" />

  <!-- Cyberpunk Matrix Double Bezel -->
  <rect x="12" y="12" width="{width - 24}" height="{height - 24}" fill="none" stroke="#00ff66" stroke-width="2" stroke-opacity="0.8" filter="url(#textGlow)" />
  <rect x="16" y="16" width="{width - 32}" height="{height - 32}" fill="none" stroke="#005522" stroke-width="1" stroke-opacity="0.6" />

  <!-- Corner Brackets -->
  <path d="M 8,24 L 8,8 L 24,8" fill="none" stroke="#00ff66" stroke-width="3" />
  <path d="M {width-8},24 L {width-8},8 L {width-24},8" fill="none" stroke="#00ff66" stroke-width="3" />
  <path d="M 8,{height-24} L 8,{height-8} L 24,{height-8}" fill="none" stroke="#00ff66" stroke-width="3" />
  <path d="M {width-8},{height-24} L {width-8},{height-8} L {width-24},{height-8}" fill="none" stroke="#00ff66" stroke-width="3" />

  <!-- 8-Bit White Rabbit with Green Glow -->
  <!-- 1. Ambient Green Glow Halo -->
  <g fill="#00ff66" filter="url(#matrixGlow)" opacity="0.9">
    {' '.join(rects)}
  </g>

  <!-- 2. Solid Crisp 8-Bit White Rabbit Body -->
  <g fill="#ffffff">
    {' '.join(rects)}
  </g>

  <!-- 3. Cybernetic Cyan Eye -->
  <rect x="{eye_x}" y="{eye_y}" width="{scale * 2}" height="{scale * 2}" fill="#00f0ff" filter="url(#textGlow)" />
  <rect x="{eye_x + 2}" y="{eye_y + 2}" width="{scale}" height="{scale}" fill="#ffffff" />

  <!-- Typography: MORPHEUS -->
  <text x="50%" y="335" text-anchor="middle" font-family="'Courier New', Courier, monospace" font-size="42" font-weight="900" letter-spacing="8" fill="#ffffff" stroke="#00ff66" stroke-width="1.5" filter="url(#matrixGlow)">
    MORPHEUS
  </text>

  <!-- Subtitles -->
  <text x="50%" y="375" text-anchor="middle" font-family="'Courier New', Courier, monospace" font-size="14" font-weight="800" letter-spacing="5" fill="#00ff66" filter="url(#textGlow)">
    VERSION 1.0.0
  </text>

  <text x="50%" y="405" text-anchor="middle" font-family="-apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, monospace" font-size="13" font-weight="700" letter-spacing="3" fill="#ffb71a">
    8-TRACK REBIRTH GROOVEBOX
  </text>

  <text x="50%" y="432" text-anchor="middle" font-family="'Courier New', Courier, monospace" font-size="12" font-weight="600" letter-spacing="4" fill="#00f0ff" opacity="0.9">
    FM-1 OPEN // ID: FM-1_8000013
  </text>
</svg>
'''
    with open(filename, 'w') as f:
        f.write(svg_content)
    print(f"Generated SVG: {filename}")

if __name__ == '__main__':
    generate_svg("assets/morpheus_logo.svg", 480, 480)
    generate_svg("assets/logo.svg", 480, 480)
