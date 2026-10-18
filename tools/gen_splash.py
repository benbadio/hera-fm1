#!/usr/bin/env python3
"""Generate MORPHEUS Rabbit Emoji 🐇 Splash Screen and Banner assets."""

import os
import subprocess

def make_svg(width, height, is_banner=False):
    halo_r = int(width * 0.21)
    halo_cy = int(height * (0.24 if is_banner else 0.23))
    text_y = int(height * (0.27 if is_banner else 0.26))
    emoji_size = int(width * (0.29 if is_banner else 0.29))

    title_size = int(width * (0.097 if is_banner else 0.10))
    title_y = int(height * (0.57 if is_banner else 0.57))

    sub1_size = int(width * (0.047 if is_banner else 0.05))
    sub1_y = int(height * (0.68 if is_banner else 0.68))

    sub2_size = int(width * (0.047 if is_banner else 0.05))
    sub2_y = int(height * (0.76 if is_banner else 0.77))

    sub3_size = int(width * (0.036 if is_banner else 0.042))
    sub3_y = int(height * (0.84 if is_banner else 0.85))

    sub4_size = int(width * (0.033 if is_banner else 0.038))
    sub4_y = int(height * (0.91 if is_banner else 0.92))

    border_outer_w = 3 if is_banner else 1
    border_inner_w = 2 if is_banner else 1
    bo_inset = 8 if is_banner else 2.5
    bi_inset = 16 if is_banner else 4.5

    corner_brackets = ""
    if is_banner:
        corner_brackets = f'''
  <!-- Corner Brackets -->
  <path d="M 24,64 L 24,24 L 64,24" fill="none" stroke="#00ff66" stroke-width="4" />
  <path d="M {width-24},64 L {width-24},24 L {width-64},24" fill="none" stroke="#00ff66" stroke-width="4" />
  <path d="M 24,{height-64} L 24,{height-24} L 64,{height-24}" fill="none" stroke="#00ff66" stroke-width="4" />
  <path d="M {width-24},{height-64} L {width-24},{height-24} L {width-64},{height-24}" fill="none" stroke="#00ff66" stroke-width="4" />
'''

    return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" width="{width}" height="{height}" style="background:#000000;">
  <defs>
    <filter id="glow" x="-30%" y="-30%" width="160%" height="160%">
      <feGaussianBlur stdDeviation="{10 if is_banner else 4}" result="blur" />
      <feMerge>
        <feMergeNode in="blur" />
        <feMergeNode in="SourceGraphic" />
      </feMerge>
    </filter>
    <radialGradient id="halo" cx="50%" cy="50%" r="50%">
      <stop offset="0%" stop-color="#00ff66" stop-opacity="0.35" />
      <stop offset="60%" stop-color="#00ff66" stop-opacity="0.10" />
      <stop offset="100%" stop-color="#000000" stop-opacity="0" />
    </radialGradient>
  </defs>

  <!-- Double Matrix Border -->
  <rect x="{bo_inset}" y="{bo_inset}" width="{width - bo_inset*2}" height="{height - bo_inset*2}" fill="none" stroke="#00ff66" stroke-width="{border_outer_w}" />
  <rect x="{bi_inset}" y="{bi_inset}" width="{width - bi_inset*2}" height="{height - bi_inset*2}" fill="none" stroke="#00441b" stroke-width="{border_inner_w}" />
{corner_brackets}
  <!-- Rabbit Ambient Glow Halo -->
  <circle cx="{width/2}" cy="{halo_cy}" r="{halo_r}" fill="url(#halo)" />

  <!-- Rabbit Emoji 🐇 (centered at x={width/2}) -->
  <text x="{width/2}" y="{text_y}" font-size="{emoji_size}" text-anchor="middle" dominant-baseline="central">🐇</text>

  <!-- Typography -->
  <text x="{width/2}" y="{title_y}" font-family="monospace, Courier, sans-serif" font-size="{title_size}" font-weight="900" fill="#00ff66" letter-spacing="{12 if is_banner else 4}" text-anchor="middle" filter="url(#glow)">MORPHEUS</text>

  <text x="{width/2}" y="{sub1_y}" font-family="monospace, Courier, sans-serif" font-size="{sub1_size}" font-weight="bold" fill="#00ff66" letter-spacing="{6 if is_banner else 2}" text-anchor="middle">VERSION 1.0.0</text>

  <text x="{width/2}" y="{sub2_y}" font-family="monospace, Courier, sans-serif" font-size="{sub2_size}" font-weight="bold" fill="#ffb700" letter-spacing="{4 if is_banner else 1.5}" text-anchor="middle">8-TRACK GROOVEBOX</text>

  <text x="{width/2}" y="{sub3_y}" font-family="monospace, Courier, sans-serif" font-size="{sub3_size}" font-weight="bold" fill="#00e5ff" letter-spacing="{3 if is_banner else 1}" text-anchor="middle">FM-1 OPEN // ID: 8000013</text>

  <text x="{width/2}" y="{sub4_y}" font-family="monospace, Courier, sans-serif" font-size="{sub4_size}" font-weight="bold" fill="#00ff66" letter-spacing="{3 if is_banner else 1}" text-anchor="middle">[ READY // 48kHz DSP ]</text>
</svg>'''

def main():
    os.makedirs("assets", exist_ok=True)
    os.makedirs("build/host", exist_ok=True)

    # 1. Generate 240x240 Splash SVG
    splash_svg = "build/host/morpheus_splash.svg"
    with open(splash_svg, "w") as f:
        f.write(make_svg(240, 240, is_banner=False))

    # 2. Generate 720x720 Banner SVG
    banner_svg = "build/host/morpheus_banner.svg"
    with open(banner_svg, "w") as f:
        f.write(make_svg(720, 720, is_banner=True))

    chrome_bin = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
    splash_png = os.path.abspath("assets/morpheus_splash.png")
    banner_png = os.path.abspath("assets/morpheus_banner.png")

    if os.path.exists(chrome_bin):
        # Render Splash PNG
        subprocess.run([
            chrome_bin, "--headless", f"--screenshot={splash_png}",
            "--window-size=240,240", "--default-background-color=00000000",
            f"file://{os.path.abspath(splash_svg)}"
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
        print(f"Generated PNG: assets/morpheus_splash.png")

        # Render Banner PNG
        subprocess.run([
            chrome_bin, "--headless", f"--screenshot={banner_png}",
            "--window-size=720,720", "--default-background-color=00000000",
            f"file://{os.path.abspath(banner_svg)}"
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
        print(f"Generated Banner PNG: assets/morpheus_banner.png")

        # Convert to PPM for host test tooling
        subprocess.run([
            "ffmpeg", "-i", splash_png, "-f", "rawvideo", "-pix_fmt", "rgb24",
            "build/host/morpheus_splash.ppm", "-y"
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        print("Generated PPM: build/host/morpheus_splash.ppm")

if __name__ == '__main__':
    main()
