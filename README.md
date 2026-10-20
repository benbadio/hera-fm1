# HERA: a chord groovebox for the M-VAVE FM-1

HERA turns the FM-1 into a J-6-style chord machine:
- **100 chord sets**, each mapping a chord to every key.
- **9 phrase styles × 12 variations**: arpeggios, rhythms, and chord and strum phrases.
- **A 64-pattern step sequencer** with ties, per-step phrases, shuffle and real-time recording.
- **A Juno-style 4-voice synth** with delay and reverb.

Juno's Greek counterpart is Hera.

It is an independent, clean-room recreation of the Roland J-6's *behaviour*. No Roland code, firmware or samples are used. The chord voicings come from the J-6 owner's manual, and the phrase timing for styles 6–9 was measured from a J-6 over MIDI (`capture/`, `tools/derive_phrases.py`). It is not made by, affiliated with or endorsed by Roland.

## Using it

| FM-1 | HERA |
|---|---|
| **PLAY** | Tap: play / stop. **Hold**: full stop (all sound off). GLO + PLAY: play from the selected step |
| **REC** | Record (real time) |
| **SCL** | Tap: chord mode on/off. **Hold** (or GLO + SCL): the **chord map**, i.e. which chord is on which key |
| **ARP** | Tap: phrase (STYLE) on/off. Hold: the phrase page |
| **ENV** | HOLD (in step edit: tie) |
| **HOME / SEQ / LFO / FX / EDIT** | Pages: play, sequencer (GLO+SEQ: patterns), sounds, effects, settings |
| **SAVE** | Save the pattern (GLO + SAVE: all patterns) |
| **OCT − / +** | Octave (GLO: key transpose; on the sequencer page: steps page) |
| **K1–K4** | Filter, envelope (release), delay level, reverb level. GLO: resonance, attack, delay time, reverb time |
| **SELECT / ALGORITHM** | Phrase style / variation |
| **PRESETS** | Tempo, or whatever the page browses (chord sets, sounds, patterns, settings) |
| **Keys** | Notes, or chords in chord mode. On the sequencer page the white keys pick steps; then keys enter notes |

The settings page uses plain words: MIDI channel, clock source, hold on step edit, chord legato, count-in, tune and more. Leaving the page saves them.

## Building

- **GitHub Actions** (`.github/workflows/build.yml`, Ubuntu) builds `hera.fwsc` with JieLi's Linux toolchain and uploads it.
- **Locally on Linux x86-64:**
  1. `tools/get_toolchain.sh`
  2. Clone the AC79 SDK (tag `AC79NN_SDK_V1.2.1_2023-12-13`) to `~/fw-AC79_AIoT_SDK`.
  3. Run `./build.sh`, which writes `build/hera.fwsc`.

Tests run on macOS or Linux:
- `tests/j6/run_tests.sh`: the engine (chords, phrases, sequencer, synth, glue), about 800,000 checks.
- `tests/run_hera_host.sh`: the whole firmware in a simulator. It writes a PNG of every page and a WAV of a scripted session to `build/hera/`.

## Installing

Install it like any open FM-1 firmware:
- the Web MIDI installer, or
- `python3 tools/fm1_install.py build/hera.fwsc`.

The factory V15 restore and the USB rescue (hold **OCT −** while switching on) stay available.

## Credits and licence
GPL-3.0-only. See `LICENSING.md`.
