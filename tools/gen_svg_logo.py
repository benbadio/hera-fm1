#!/usr/bin/env python3
"""Generate clean SVG vector logos and PNG for MORPHEUS using the Rabbit Emoji 🐇."""

import os
import subprocess

def generate_svg(filename, width=480, height=480):
    os.makedirs(os.path.dirname(filename), exist_ok=True)

    svg_content = f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" width="100%" height="100%" style="background:#020503;">
  <defs>
    <!-- Matrix Grid Lines -->
    <pattern id="scanlines" width="100" height="4" patternUnits="userSpaceOnUse">
      <line x1="0" y1="0" x2="100" y2="0" stroke="rgba(0,0,0,0.3)" stroke-width="1.5" />
    </pattern>

    <!-- Neon Green Glow Filter -->
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

    <radialGradient id="halo" cx="50%" cy="50%" r="50%">
      <stop offset="0%" stop-color="#00ff66" stop-opacity="0.35" />
      <stop offset="60%" stop-color="#00ff66" stop-opacity="0.10" />
      <stop offset="100%" stop-color="#000000" stop-opacity="0" />
    </radialGradient>
  </defs>

  <!-- Background Layer -->
  <rect width="{width}" height="{height}" fill="url(#matrixVignette)" />
  <rect width="{width}" height="{height}" fill="url(#scanlines)" pointer-events="none" />

  <!-- Double Matrix Border -->
  <rect x="12" y="12" width="{width - 24}" height="{height - 24}" fill="none" stroke="#00ff66" stroke-width="2" stroke-opacity="0.8" filter="url(#textGlow)" />
  <rect x="16" y="16" width="{width - 32}" height="{height - 32}" fill="none" stroke="#005522" stroke-width="1" stroke-opacity="0.6" />

  <!-- Corner Brackets -->
  <path d="M 8,24 L 8,8 L 24,8" fill="none" stroke="#00ff66" stroke-width="3" />
  <path d="M {width-8},24 L {width-8},8 L {width-24},8" fill="none" stroke="#00ff66" stroke-width="3" />
  <path d="M 8,{height-24} L 8,{height-8} L 24,{height-8}" fill="none" stroke="#00ff66" stroke-width="3" />
  <path d="M {width-8},{height-24} L {width-8},{height-8} L {width-24},{height-8}" fill="none" stroke="#00ff66" stroke-width="3" />

  <!-- Rabbit Ambient Glow Halo -->
  <circle cx="{width/2}" cy="{height * 0.32}" r="{width * 0.22}" fill="url(#halo)" />

  <!-- Rabbit Emoji 🐇 -->
  <text x="{width/2}" y="{height * 0.33}" font-size="{int(width * 0.30)}" text-anchor="middle" dominant-baseline="central">🐇</text>

  <!-- Typography: MORPHEUS -->
  <text x="50%" y="{height * 0.69}" text-anchor="middle" font-family="'Courier New', Courier, monospace" font-size="{int(width * 0.088)}" font-weight="900" letter-spacing="8" fill="#ffffff" stroke="#00ff66" stroke-width="1.5" filter="url(#matrixGlow)">
    MORPHEUS
  </text>

  <!-- Subtitles -->
  <text x="50%" y="{height * 0.78}" text-anchor="middle" font-family="'Courier New', Courier, monospace" font-size="{int(width * 0.035)}" font-weight="800" letter-spacing="4" fill="#00ff66" filter="url(#textGlow)">
    VERSION 1.0.0
  </text>

  <text x="50%" y="{height * 0.85}" text-anchor="middle" font-family="-apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, monospace" font-size="{int(width * 0.032)}" font-weight="700" letter-spacing="3" fill="#ffb71a">
    8-TRACK GROOVEBOX
  </text>

  <text x="50%" y="{height * 0.91}" text-anchor="middle" font-family="'Courier New', Courier, monospace" font-size="{int(width * 0.027)}" font-weight="600" letter-spacing="3" fill="#00f0ff" opacity="0.9">
    FM-1 OPEN // ID: 8000013
  </text>
</svg>
'''
    with open(filename, 'w') as f:
        f.write(svg_content)
    print(f"Generated SVG: {filename}")

if __name__ == '__main__':
    generate_svg("assets/morpheus_logo.svg", 480, 480)
    generate_svg("assets/logo.svg", 480, 480)
    
    # Render PNG logo using Chrome headless
    chrome_bin = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
    if os.path.exists(chrome_bin):
        svg_abs = os.path.abspath("assets/logo.svg")
        png_out = os.path.abspath("assets/logo.png")
        subprocess.run([
            chrome_bin, "--headless", f"--screenshot={png_out}",
            "--window-size=480,480", "--default-background-color=00000000",
            f"file://{svg_abs}"
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        print("Generated PNG: assets/logo.png")
