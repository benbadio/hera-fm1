# HERA manual

Everything HERA does, control by control, and the history of each version. New to HERA? Start with the [guide](GUIDE.md) or the guided tour in the [Workbench](https://benbadio.github.io/hera-fm1/). The Workbench also has this manual with screenshots: [manual.html](https://benbadio.github.io/hera-fm1/manual.html).

## Contents

1. [The screen](#the-screen)
2. [Controls](#controls)
3. [The pages](#the-pages)
4. [Settings](#settings)
5. [MIDI and sync](#midi-and-sync)
6. [Saving, and what HERA keeps](#saving-and-what-hera-keeps)
7. [Installing and going back](#installing-and-going-back)
8. [Version history](#version-history)

## The screen

The FM-1's 240 × 240 screen always has the same frame:
- **Top bar:** the pattern (A1 to H8, with `*` when it has unsaved changes; `>` and a name when another is queued), the page name, the tempo and the transport (▶ playing, ■ stopped, a red mark while recording). While you record, the page name is replaced by what recording is waiting for or doing: *Press PLAY*, *Count-in N*, *REC step N*.
- **Bottom bar:** what the four knobs K1 to K4 do (hold GLO for their second function), or a short pop-up with the new value after any change.

## Controls

**GLO** is the shift button. "Hold" means a press of about half a second.

### Transport and recording
| Control | What it does |
|---|---|
| **PLAY** tap | Play from step 1, or stop |
| **PLAY** hold | Full stop: stops and cuts all sound (a 6 ms fade, no click) |
| **GLO + PLAY** | Play from the selected step (step 1 if none is selected) |
| **REC** | Arms recording. The screen says *Press PLAY*. **PLAY** starts it (after the count-in, if one is set) |
| **REC** during playback | Punch in: starts recording at once |
| **REC** during recording | Punch out: stops recording, playback continues |
| **PLAY** during recording | Stops both |

Recording puts what you play into the step that is playing (the one flashing under the white keys). A chord replaces the step; a single note adds to it, up to four. It keeps going round the loop, adding to what is there.

### Playing
| Control | What it does |
|---|---|
| **Keys** (27, F3 to G5) | Notes; chords with chord mode on. They play on every page; on the sequencer page, with a step selected, they enter notes into it |
| **SEL** tap | Chord mode on / off |
| **SEL** hold, or **GLO + SEL** | The chord map |
| **ARP** tap | Phrase on / off |
| **ARP** hold | The phrase page |
| **ENV** | HOLD on / off: notes keep sounding. On a selected step: ties the step |
| **OCT − / +** | Octave down / up |
| **GLO + OCT − / +** | Key transpose down / up |
| **SELECT** knob | Phrase style, 1 to 9 |
| **ALGORITHM** knob | Phrase variation, 1 to 12 |
| **MASTER** knob | Output volume |

### Knobs K1 to K4
| Knob | Alone | With GLO |
|---|---|---|
| **K1** | Filter cutoff | Resonance |
| **K2** | Envelope release | Envelope attack |
| **K3** | Delay level | Delay time |
| **K4** | Reverb level | Reverb time |

On the Settings page, K1 changes the selected setting and K2 to K4 do nothing.

### Pages
| Control | Opens |
|---|---|
| **HOME** | Play |
| **SEQ** | Sequencer. Press again on the sequencer page: Patterns |
| **GLO + SEQ** | Patterns, from anywhere |
| **LFO** | Sounds |
| **FX** | Effects |
| **EDIT** | Settings; EDIT again leaves it and saves the settings |
| **SEL** hold | Chord map |
| **ARP** hold | Phrase |

### Saving
| Control | What it does |
|---|---|
| **SAVE** | Save the current pattern |
| **GLO + SAVE** | Save all patterns |

### The PRESETS knob
It depends on the page.
| Page | PRESETS | GLO + PRESETS |
|---|---|---|
| Play, Phrase, Effects | Tempo, 1 BPM a click | Tempo, 0.1 BPM a click |
| Chord map | Browse the 100 chord sets | |
| Sounds | One sound at a time | A bank of 8 |
| Patterns | One pattern at a time (queued if playing) | A bank of 8 |
| Sequencer | Select the step to edit (the first turn picks the first step of the page), then move along | Tempo, 0.1 BPM a click |
| Settings | Choose a setting | |

### The sequencer page
| Control | What it does |
|---|---|
| **PRESETS** | Select the step to edit, then move along |
| **Keys** | Play; with a step selected, enter notes or a chord |
| **ENV** | Tie the selected step |
| **SEQ** | Finish editing (deselect the step) |
| **OCT − / +** | With no step selected: the previous / next page of 16 steps |
| **GLO + F3 / G3 / A3 / B3 / C4** | With a step selected: copy / paste / insert / clear the step, double the pattern |

The lights under the white keys are the 16 steps of the page: lit = the step has notes, flashing = playing now, blinking = being edited. While you record they follow the recording on any page.

### At power-on
Hold **OCT −** while switching on for the USB rescue.

## The pages

- **Play:** the chord or note you last played, the chord set and its genre, the sound, the phrase in words, lights for PHRASE, CHORD and HOLD, and a live scope.
- **Chord map:** which chord is on which key for the current set. PRESETS browses the sets; the key you play lights.
- **Phrase:** the style and variation by name, on or off, and, for styles 6 to 9, a piano roll of the phrase.
- **Sequencer:** 16 steps at a time, with each step's chord or note, ties, per-step phrases and the last step.
- **Patterns:** all 64 patterns on one grid: the current one, ones with notes, unsaved ones, and a queued one.
- **Sounds:** the 64 sounds, 8 banks of 8: pads, strings, brass, keys, bass, leads, plucks and effects.
- **Effects:** the delay and reverb, with levels and times.
- **Settings:** see below.

## Settings

Press **EDIT**, choose with **PRESETS**, change with **K1**. Settings marked *pattern* are saved with the pattern; the others are global.

| Setting | Range | Default | |
|---|---|---|---|
| Tempo | 40.0 to 300.0 BPM | 120.0 | pattern |
| Last step | 1 to 64 | 8 | pattern: where the pattern loops |
| Step length | 1/16, 1/8 triplet, 1/8, 1 to 8 beats | 4 beats | pattern |
| Shuffle | −90 to 90 % | 0 | pattern |
| Pattern volume | 0 to 127 | 100 | pattern |
| Transpose | −12 to 12 | 0 | pattern: transposes the sound |
| Accent velocity | 1 to 127 | 127 | pattern: the velocity of accents in phrases |
| Pattern change | Next step, 1 to 8 beats | Next step | pattern: when a queued pattern starts |
| Delay tempo sync | Off / On | Off | pattern |
| Key velocity | 1 to 127 | 100 | |
| Key (transpose) | −12 to 12 | 0 | |
| Hold on step edit | Release / Keep | Release | new in HERA: Release lets go of HOLD when you start editing a step |
| Chord legato | Off / On | On | new in HERA: keeps a chord sounding when you move to a neighbouring chord |
| Count-in | Off, 2 to 4 beats | Off | before recording starts |
| MIDI channel | 1 to 16 | 1 | |
| MIDI thru | Off / On | Off | |
| Send prog. change | Off / On | On | |
| Receive prog. ch. | Off / On | On | |
| Prog. change ch. | 1 to 16 | 16 | |
| Clock source | Auto, Internal, MIDI only, USB only | Auto | |
| Sync pulses/beat | 1, 2, 3, 4, 6, 8, 12, 24 | 2 | |
| Tune | 433.0 to 448.0 Hz | 440.0 | |
| USB audio level | Volume knob, or 1 to 127 | Volume knob | |

## MIDI and sync

HERA works over USB MIDI. It sends and receives notes on the MIDI channel, and sends and receives program changes (0 to 63 select a pattern) on their own channel. It can send MIDI clock, start and stop, and follow an incoming clock from the source you choose. The details are in [docs/MIDI.md](docs/MIDI.md).

## Saving, and what HERA keeps

HERA keeps its 64 patterns and its settings in its own area of the FM-1's flash. Patterns survive switching off once you save them (**SAVE**); unsaved changes don't. HERA doesn't read the projects, presets or samples of the factory firmware or of other firmwares, and installing another firmware can overwrite HERA's patterns. There's no way yet to copy patterns off the FM-1.

In the Workbench, the **Project** tab saves everything in the browser, or exports it as a file.

## Installing and going back

Use the web [installer](https://benbadio.github.io/hera-fm1/install.html) in Chrome or Edge with the FM-1 connected by a USB data cable. HERA carries the standard FM-1 update loader, so any other firmware, or the factory one, installs on top of it in the usual way. If an install is cut off, hold **OCT −** while switching on (USB rescue) and install again. To build it yourself, see [BUILDING.md](BUILDING.md).

## Version history

The firmware's changes are listed first for each version; Workbench changes that matter to users have their own list. Every release on the [Releases](../../releases) page carries the same notes.

### 0.1

The first preview.

**Firmware**
- 100 chord sets, each mapping a chord to every key, with a chord map page.
- 9 phrase styles × 12 variations, shown by name, with a piano roll for the chord and strum styles.
- A 64-pattern step sequencer with ties, per-step phrases, shuffle, step tools, and real-time recording with punch in and out and an optional count-in.
- A 4-voice Juno-style synth with 64 sounds, delay and reverb.
- A Patterns page that shows all 64 patterns at once.
- A colour screen with plain-word pages and settings.
- Two new settings: Hold on step edit, Chord legato.
- Hold PLAY for a full stop.
- The keys always play, on every page. PRESETS picks sounds, patterns and steps.
- The lights under the white keys show the sequencer's steps.

**Workbench**
- HERA running in your browser on a virtual FM-1, with an editor and a guided tour.
- A manual, and an installer for HERA and the factory firmware.
