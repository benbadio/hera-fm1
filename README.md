# MORPHEUS // 8-Track ReBirth Groovebox & Matrix Sequencer

<p align="center">
  <img src="assets/screen_preview.png" width="380" alt="MORPHEUS Screen Preview">
</p>

<p align="center">
  <strong>8-Track ReBirth Groovebox (808, 909, Dual 303s, Amen Breaks, FM Lead, Sub Synth & MIDI) with a Reason Matrix-style Pattern Sequencer and Dual-Phosphor CRT UI for the M-VAVE FM-1.</strong>
</p>

<p align="center">
  <a href="https://github.com/benbadio/morpheus-fm1/actions/workflows/build.yml"><img src="https://github.com/benbadio/morpheus-fm1/actions/workflows/build.yml/badge.svg" alt="Build Status"></a>
  <a href="https://benbadio.github.io/morpheus-fm1/"><img src="https://img.shields.io/badge/Web%20Emulator-Live%20Demo-00ff66?logo=web&logoColor=black" alt="Live Demo"></a>
  <a href="https://benbadio.github.io/morpheus-fm1/install.html"><img src="https://img.shields.io/badge/Web%20Flasher-1--Click%20Install-ffb700?logo=usb&logoColor=black" alt="Web Flasher"></a>
  <a href="https://benbadio.github.io/morpheus-fm1/manual.html"><img src="https://img.shields.io/badge/Docs-User%20Manual-00d9ff" alt="User Manual"></a>
  <img src="https://img.shields.io/badge/Platform-FM--1__8000013-00ff66" alt="Platform FM-1 Open">
  <img src="https://img.shields.io/badge/License-GPL--3.0-blue.svg" alt="GPL-3.0 License">
</p>

---

## Overview

**MORPHEUS** transforms the ultra-portable **M-VAVE FM-1** into a standalone, battery-powered **8-track acid techno & electronic groovebox workstation**, blending the legendary multi-machine workflow of Propellerhead ReBirth RB-338 with the iconic visual sequencing paradigm of the Reason Matrix pattern sequencer.

- 🎛️ **[Live Web Audio Emulator](https://benbadio.github.io/morpheus-fm1/)** &mdash; Full in-browser simulation with SVG hardware chassis, playable silicone keyboard, draggable potentiometers, and audio engine.
- ⚡ **[1-Click Web MIDI Flasher](https://benbadio.github.io/morpheus-fm1/install.html)** &mdash; Flash the bare-metal firmware directly to your FM-1 over USB from Google Chrome or Microsoft Edge with zero command-line tools required.
- 📖 **[Interactive User Manual & Tutorial](https://benbadio.github.io/morpheus-fm1/manual.html)** &mdash; Complete hardware manual featuring MPC/Roland-style illustrated action tables and step-by-step walkthroughs.

---

## 8-Track Multi-Engine ReBirth Architecture

Each of the 8 tracks functions as an independent machine with its own pattern storage, polyrhythmic sequence length, step resolution, and dedicated MIDI output channel:

| Track | Engine / Role | Synthesis Model & Routing | MIDI Out Channel |
| :--- | :--- | :--- | :--- |
| **Track 1** | **TB-303 A** | Transistor diode-ladder acid bass with non-linear resonance, Devilfish envelope mod, and RAT overdrive | Channel 1 |
| **Track 2** | **TB-303 B** | Second independent 303 voice for counterpoint basslines, acid arps, and squelchy leads | Channel 2 |
| **Track 3** | **TR-808 Kit** | Analog circuit drum model: Kick, Snare, Low/Mid/Hi Toms, Congas, Rim Shot, Claves, Clap, Cowbell, Maracas, Hats, Cymbal | Channel 10 |
| **Track 4** | **TR-909 Kit** | Hybrid punchy drum model: Tuned Kick, Snappy Snare, Toms, Rim, Clap, and 6-bit sampled metallic Hi-Hats/Cymbals | Channel 10 |
| **Track 5** | **Amen Breaks** | 8-slice breakbeat loop player with real-time tempo pitch resampling and generative slice shuffling | Channel 5 |
| **Track 6** | **FM Lead** | Dynamic 2-operator frequency modulation synth for bright digital plucks, bells, and aggressive leads | Channel 6 |
| **Track 7** | **Sub Synth** | Analog-style sub-bass synthesizer with saturation and low-end reinforcement | Channel 7 |
| **Track 8** | **MIDI Master** | Pure external MIDI sequencer track with zero internal audio overhead for controlling external hardware | Channel 8 |

---

## 12-Lane Drum Matrix Mode

When selecting drum tracks (T3 TR-808 or T4 TR-909), MORPHEUS automatically transforms the 16-step matrix sequencer into a **12-Lane Drum Matrix**:

```text
 ┌───┬────────────────────────────────┐
 │ACC│  ·   ·   ·   ·   ·   ·   ·   · │ Total Accent
 │CH │ [█] [█] [█] [█] [█] [█] [█] [█]│ Closed Hi-Hat
 │OH │  ·   ·  [█]  ·   ·   ·  [█]  · │ Open Hi-Hat
 │CY │  ·   ·   ·   ·   ·   ·   ·  [█]│ Crash / Ride Cymbal
 │CB │  ·   ·   ·   ·  [█]  ·   ·   · │ Cowbell
 │CP │  ·   ·  [█]  ·   ·   ·  [█]  · │ Hand Clap / Maracas
 │RS │  ·   ·   ·   ·   ·   ·   ·   · │ Rim Shot / Claves
 │HT │  ·   ·   ·   ·   ·  [█]  ·   · │ High Tom / Conga
 │MT │  ·   ·   ·  [█]  ·   ·   ·   · │ Mid Tom / Conga
 │LT │  ·  [█]  ·   ·   ·   ·   ·   · │ Low Tom / Conga
 │SD │  ·   ·  [█]  ·   ·   ·  [█]  · │ Snare Drum
 │BD │ [█]  ·   ·   ·  [█]  ·   ·   · │ Bass Drum
 └───┴────────────────────────────────┘
        1   2   3   4   5   6   7   8   (Page 1 / Page 2: 1-16 / 17-32)
```

- **K1 (Select Lane):** Scroll through the 12 drum instruments.
- **K2 (Toggle Trigger):** Turn active drum voice on/off on the selected step.
- **K4 (Toggle Accent):** Enable total accent boost on that step.

---

## Dual-Phosphor Matrix CRT Theme & Dynamic Status Bar

- **High-Contrast Dual-Phosphor Visuals:** Designed for maximum legibility on the FM-1's 240x240 LCD. High-contrast amber LEDs represent active playheads, accented gates, and ties, set against a phosphor matrix green CRT grid.
- **Transient Real-Time Status Bar:** Any knob turn or held button temporarily displays immediate, detailed feedback (e.g. `K1 Cutoff: 84`, `Swing: 54%`, `Queued Bank B`) and smoothly restores the default track status after 1.5 seconds of idle.

---

## Sequencer & Performance Features

- **Polyrhythmic Step Lengths:** Each track can have its own independent sequence length (1 to 32 steps) for intricate polyrhythms and evolving phase loops.
- **Variable Clock Resolutions:** 1/2, 1/4, 1/8, 1/16, 1/32, plus triplet modes (1/4T, 1/8T, 1/16T).
- **Global & Track Swing:** Authentic MPC-style shuffle from 0% (straight) up to 75% (heavy triplet swing).
- **Dual Matrix Modes:**
  - **KEYS View:** Pitch piano roll, velocity gate bars, and tied legato slide bridges.
  - **CURVE View:** Bipolar (-64..+63) or Unipolar (0..127) per-step CV automation curves for filter cutoff sweeps or MIDI CC parameters.
- **Pattern Memory:** 2 Banks (Bank A & B) &times; 8 Patterns = 16 patterns per track (128 total patterns). Seamless bar-synchronized pattern queuing.
- **Master FX Bus:** Stereo send reverb (4 comb filters + 2 allpasses), tempo-synced stereo delay, and master glue compressor with kick-ducking sidechain PUMP.
- **Pattern Tools & Multi-Level Undo/Redo:**
  - Shift Left/Right (`FX + SELECT`)
  - Octave / Semitone Transpose (`EDIT + SELECT`)
  - Musical Alteration & Randomize (`Shift + FX`)
  - Multi-level Undo (`EDIT + HOME`) and Redo (`EDIT + SAVE`)
  - Flash persistence with 1-click JSON backup/restore in the emulator.

---

## Hardware Panel Mapping (M-VAVE FM-1)

| Hardware Control | Primary Function | With SHIFT (EDIT held) |
| :--- | :--- | :--- |
| **27 Silicone Keys** | Audition notes / Step record in REC mode | Select Step Jump (1&ndash;16 / 17&ndash;32) |
| **K1 (Cutoff)** | Synth Cutoff / Step Note Pitch / Select Drum Lane | Step Gate Velocity (0&ndash;127) |
| **K2 (Resonance)** | Synth Resonance / Gate Length / Toggle Drum Step | Tied Note Slide (On / Off) |
| **K3 (Decay)** | Envelope Decay / Curve Modulation Value | MIDI CC Modulation Depth |
| **K4 (Accent)** | 303 Accent / Pattern Length / Drum Accent | Step Resolution (1/16, 1/8T, 1/32...) |
| **TEMPO Knob** | Sequencer Tempo (50 &ndash; 240 BPM) | Shuffle / Swing (0 &ndash; 75%) |
| **SELECT Knob** | Scrub Active Step (1 &ndash; 32) | Pattern Transpose (&plusmn;12 Semitones) |
| **PLAY** | Start / Stop Sequencer Transport | Restart from Step 1 |
| **REC** | Step Record Mode (Audition + Step Auto-Advance) | Clear Active Step |
| **FX** | Open/Close Pattern Tools Menu | **Shift+FX:** Deliberate Musical Randomize |
| **HOME** | Cycle Active Track (T1 &rarr; T8) / Execute Menu Option | **Shift+HOME:** Multi-Level Undo |
| **SAVE** | Save all patterns & tracks to Flash Storage | **Shift+SAVE:** Multi-Level Redo |
| **EDIT** | Toggle KEYS (Notes/Drums) / CURVE (Modulation) View | Hold as SHIFT modifier |
| **SEL (Page)** | Toggle Page 1 (Steps 1&ndash;16) / Page 2 (Steps 17&ndash;32) | Set Loop End Point |
| **ENV / LFO** | Cycle Engine: 303A &rarr; 303B &rarr; 808 &rarr; 909 &rarr; BRK &rarr; FM &rarr; SUB &rarr; MIDI | Engine Parameters |
| **OCT- / OCT+** | Scroll Piano Roll Octave (C1 &hellip; B5) | Track Octave Transpose |
| **ARP / SEQ** | Cycle Bank A&ndash;B / Pattern 1&ndash;8 | Pattern Copy / Paste |

---

## Interactive Guided Walkthrough

MORPHEUS features a hardware walkthrough inspired by classic instrument user guides (Akai MPC and Roland Aira series). You can build a complete multi-track acid groove step-by-step:

- **Live On-Screen Hardware Spotlights:** Launch the walkthrough directly in the browser emulator (**[Interactive Walkthrough](https://benbadio.github.io/morpheus-fm1/#walkthrough)**). The physical FM-1 buttons, encoders, and silicone keys illuminate with action pins (`[1]`, `[2]`, `[HOLD]`) alongside real-time audio and LCD responses.
- **Illustrated Action Tables:** Follow the illustrated cards with Roland-style action callouts, parameter guides, and listening tips in the **[User Manual Section 3](https://benbadio.github.io/morpheus-fm1/manual.html#step1)**.

---

## Host Simulator & Unit Tests

You can test and verify MORPHEUS on your computer (macOS or Linux) without hardware:

```sh
# Run host test suite (renders 44.1 kHz audio WAV and generates LCD screenshots)
./tests/run_host.sh
```

Outputs generated in `build/host/`:
- `morpheus.wav`: 44.1 kHz 16-bit stereo audio render of the 8-track ReBirth groove.
- `morpheus-500.png`: LCD screenshot at 500 ms (Keys View).
- `morpheus-2000.png`: LCD screenshot at 2000 ms (303 B View).
- `morpheus-2800.png`: LCD screenshot at 2800 ms (12-Lane Drum Matrix View).
- `morpheus-3500.png`: LCD screenshot at 3500 ms (Curve Modulation View).
- `morpheus-5000.png`: LCD screenshot at 5000 ms (Subtractive Engine View).

---

## Building the Physical Firmware (`.fwsc`)

To compile the bare-metal package for the M-VAVE FM-1:

### 1. Requirements (Linux x86_64 or GitHub Actions runner)
```sh
pip3 install Pillow mido python-rtmidi
tools/get_toolchain.sh
git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 \
    https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/fw-AC79_AIoT_SDK
```

### 2. Compile Package
```sh
./build.sh
```
Outputs `build/morpheus.fwsc` with official FM-1 Open platform identity `FM-1_8000013`.

### 3. Installation to M-VAVE FM-1
- **1-Click Web MIDI Flasher (Recommended):** Visit [https://benbadio.github.io/morpheus-fm1/install.html](https://benbadio.github.io/morpheus-fm1/install.html) in Chrome or Edge and click **Install MORPHEUS**.
- **Command-line USB Cable:**
  ```sh
  python3 tools/fm1_install.py build/morpheus.fwsc
  ```

---

## Platform Identity & Compatibility

MORPHEUS complies with the **FM-1 Open platform standard**:
- **Product ID:** `FM-1_8000013`
- Compatible with official M-VAVE tools, third-party firmware hubs, and open-source Web MIDI flashers.
- Safely reversible: The installer includes a 1-click restore function to revert to M-VAVE factory stock firmware V15 at any time.

---

## License & Attribution

MORPHEUS is free software licensed under the **GNU General Public License v3.0** (`GPL-3.0-only`). See [LICENSE](LICENSE) and [LICENSING.md](LICENSING.md) for full details.

- **MORPHEUS Application & UI:** Ben Badio & Contributors.
- **ReBirth Multi-Engine DSP Suite:** Based on the X0X DSP suite by Leo Kuroshita (@kurogedelic), Robin Schmidt (Open303), schwung-303, dm-Rat (davemollen), 8W8/sc808, 9W9/ER-99 (Matthew Cieplak), and schwung-breakbeat (mestela).
- **Platform & Hardware HAL:** Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments ([Felucca](https://github.com/hugelton/Felucca)), with changes from [SLOOP](https://github.com/isod89/sloop-fm1) (isod89).
