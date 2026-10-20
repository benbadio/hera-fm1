# Roland J-6: behavioural specification for the FM-1 port

This document says what the J-6 does, in enough detail to implement it in `firmware/src/j6/` and to test it
on the host. It covers the function only (chords, phrases, sequencer, menus, MIDI). The sound only has to be
"Juno-ish", and section 7 lists just the parameters the sound engine has to expose.

Every rule has an ID (`CH-3`, `SQ-26`, ...) so code comments and host tests can refer to it, and a provenance tag.

## 0. How to read this

### 0.1 Provenance tags

| Tag | Meaning |
|---|---|
| `[MANUAL <page> §<section>]` | Stated in the J-6 Owner's Manual v1.02 (`data/manual/text/`). Paraphrased; the page and section are cited. |
| `[DATA <file>]` | Taken from `data/chordsets.json` or `data/phrase_list.json` (both parsed from the manual). |
| `[WEB roland-article]` | Roland's own article "Getting to Know AIRA Compact: J-6 Chord Synth" (articles.roland.com). A secondary source: it agrees with the manual wherever both cover a point. |
| `[WEB p6-manual]` | The manual of the AIRA Compact P-6, a sibling product with the same panel conventions. A hint only. |
| `[ASSUMED - verify with capture]` | Not in the sources. This is a best guess, with the reason given. The MIDI capture (`capture/`) can settle it. |
| `[ASSUMED - verify with video]` | Not in the sources, and MIDI can't show it (display text, LEDs, knob feel). A phone video of the J-6 or one of the many YouTube demos can settle it. |

Manual page abbreviations (files in `data/manual/text/`):

| Abbr. | File | Abbr. | File |
|---|---|---|---|
| TP | Top_Panel.txt | FM | Functions_and_Menus.txt |
| RP | Rear_Panel.txt | PL | Phrase_List.txt |
| SP | Selecting_Patterns.txt | CSL | Chord_Set_List.txt |
| SS | Selecting_Sounds.txt | MIC | MIDI_Implementation_Chart.txt |
| SEQ | Using_the_Sequencer.txt | SPEC | Main_Specifications.txt |
| CM | Using_Chord_Mode.txt | CONN | Connecting_to_a_Computer_or_Mobile_Device.txt |
| PF | Using_the_Phrase_Function.txt | FR | Factory_Reset_and_Backup_Restore.txt |

The Signal Flow page is only an image, which is not in `data/manual/`, so section 7.4 is assumed.

### 0.2 Conventions

- **C-1 Note numbers.** C4 = MIDI 60 (Roland convention). The chord set list writes notes as C3, E4 and so on,
  and `chordsets.json` maps C3 to 48. `[MANUAL CSL]` `[DATA chordsets.json]`
- **C-2 Time base.** 1 tick = 1/96 of a quarter note (96 PPQN). Every value the J-6 names is a whole number
  of ticks at this resolution, and so is every sync-clock division (`S.cLk` 1, 2, 3, 4, 6, 8, 12, 24 per beat
  give 96, 48, 32, 24, 16, 12, 8, 4 ticks). The 96 PPQN is a design choice, not something the J-6 states.

  | Value | Ticks | Value | Ticks |
  |---|---|---|---|
  | double whole | 768 | double whole triplet | 512 |
  | whole | 384 | whole triplet | 256 |
  | half | 192 | half triplet | 128 |
  | quarter (= 1 beat) | 96 | quarter triplet | 64 |
  | dotted eighth | 72 | eighth triplet | 32 |
  | eighth | 48 | sixteenth triplet | 16 |
  | sixteenth | 24 | MIDI clock (24 PPQN) | 4 |

- **C-3 Sub-ticks.** Event times may also carry a fraction: 1 sub-tick = 1/16 tick (1536 PPQN). Only shuffle
  offsets (SQ-7) and strum offsets (PH-15) use them. This is the one place where 96 PPQN is not enough: one
  tick is 5.2 ms at 120 BPM, which is too coarse for a strum, and a shuffle needs steps finer than 1/24 of a
  sixteenth.
- **C-4 Bar.** A beat is a quarter note (96 ticks) and a bar is 4 beats (384 ticks). The manual never
  mentions a time signature. `[ASSUMED - verify with capture]` that 4/4 is the only meter.
- **C-5 Tempo.** Tempo is stored as tenths of a BPM (`uint16_t tempo10`, so 120.0 BPM is 1200). Samples per
  tick at 44.1 kHz, in Q12, use 32-bit arithmetic only: `spt_q12 = 1128960000u / tempo10`. For 1200 this
  gives 940800, which is 229.6875 samples (exact). The constant is 44100 × 60 × 4096 / 96 × 10, and it fits in
  31 bits.

## 1. Panel controls and SHIFT functions

### 1.1 Controls

| Control | Plain function | With SHIFT (or other modifier) | Source |
|---|---|---|---|
| Display | 4-digit 7-segment LED; shows the tempo on the home screen | | `[MANUAL TP §2]` `[MANUAL SEQ §Setting the tempo]` |
| TEMPO/VALUE knob | Tempo in 1.0 BPM steps on the home screen; otherwise edits the shown value | SHIFT: tempo in 0.1 BPM steps. Hold PATTERN: edits `bEAt` | `[MANUAL TP §2]` `[MANUAL SEQ §Setting the tempo, §Changing the step length]` |
| SHIFT | Modifier | | `[MANUAL TP §2]` |
| SOUND | Enter/leave sound select | Hold SOUND + step n: sound bank n | `[MANUAL SS]` |
| PATTERN | Enter pattern select | Hold PATTERN + step n: pattern bank n | `[MANUAL SP]` |
| PLAY | Play / stop | SHIFT + PLAY: play from a chosen step | `[MANUAL TP §3]` `[MANUAL SEQ §Starting playback from a specific step]` |
| REC | Record standby; during playback, punch in/out | | `[MANUAL TP §3]` `[MANUAL SEQ §Inputting notes in real time]` |
| Step 1–8 | Select a step for editing (hold + key enters a note or chord); in pattern/sound mode, select a pattern/sound | SHIFT + step n: page n | `[MANUAL TP §3]` `[MANUAL SEQ §Switching between pages]` |
| FILTER knob | Cutoff | SHIFT: resonance | `[MANUAL TP §4]` |
| ENVELOPE knob | Release | SHIFT: attack | `[MANUAL TP §4]` |
| DELAY knob | Delay level | SHIFT: delay time | `[MANUAL TP §4]` |
| REVERB knob | Reverb level | SHIFT: reverb time | `[MANUAL TP §4]` |
| STYLE knob | Phrase style 1–9 (absolute position) | SHIFT + turn: show the current style and variation without changing them | `[MANUAL TP §4]` `[MANUAL PF]` |
| VARIATION knob | Variation 1–12 (absolute position) | SHIFT + turn: same display as above | `[MANUAL TP §4]` `[MANUAL PF]` |
| STYLE [ON] | Phrase function on/off; in step edit, the step's STYLE flag | | `[MANUAL TP §4]` `[MANUAL SEQ §Basic operations]` |
| HOLD | Latch held keys; in step edit, add/remove ties | Hold HOLD at power-on: factory reset | `[MANUAL TP §4]` `[MANUAL SEQ §Inputting consecutive ties]` `[MANUAL FR]` |
| CHORD | Chord mode on/off | SHIFT + CHORD: chord set select | `[MANUAL TP §4]` `[MANUAL CM]` |
| Keyboard (13 keys) | Play / enter notes | SHIFT + key: functions (1.2) | `[MANUAL TP §5]` |
| VOLUME knob | Output level at MIX OUT | | `[MANUAL TP §1]` |
| MIX IN / MIX OUT, SYNC IN / SYNC OUT, MIDI IN / OUT (TRS), USB-C (audio + MIDI) | Jacks | | `[MANUAL TP §1]` `[MANUAL RP]` |

There are 13 keys, from C to the C above it. The manual names a [C (EXIT)] on the left and a [C (WRITE)] on
the right. `[MANUAL SS]` `[MANUAL SP]` Retailer spec sheets also say 13 keys.

### 1.2 SHIFT + keyboard functions

| Key (k) | Label | SHIFT + key does | Source |
|---|---|---|---|
| C (0, left) | EXIT | Leave a menu or function screen. Inside those screens the key works without SHIFT. | `[MANUAL FM §List of functions]` `[MANUAL SS]` |
| C# (1) | OCTAVE- | Keyboard octave down | `[MANUAL FM]` |
| D (2) | ENTER | Confirm a value or item (CLEAR, WRITE, MENU). Inside those screens the key works without SHIFT. | `[MANUAL FM]` |
| D# (3) | OCTAVE+ | Keyboard octave up | `[MANUAL FM]` |
| E (4) | SHUFFLE | Shuffle setting screen; EXIT leaves | `[MANUAL FM]` |
| F (5) | LAST | Last step of the current pattern; EXIT leaves | `[MANUAL FM]` `[MANUAL SEQ]` |
| F# (6) | (none) | No function listed. `[ASSUMED - verify with video]` that it plays its note. | |
| G (7) | CLEAR | Initialise the current pattern, or the selected step if one is selected; ENTER confirms | `[MANUAL FM]` `[MANUAL SEQ §Deleting]` |
| G# (8) | (none) | No function listed | |
| A (9) | KEY | Keyboard transpose screen; EXIT leaves | `[MANUAL FM]` `[MANUAL CM]` |
| A# (10) | (none) | No function listed | |
| B (11) | MENU | Menu (section 6). With a step selected: the step menu COPY/PSTE/INSR/DUPL. | `[MANUAL FM §Using the menus]` `[MANUAL SEQ §Useful sequencer functions]` |
| C (12, right) | WRITE | Save: choose `Ptn` or `ALL`, ENTER saves, EXIT cancels | `[MANUAL FM]` `[MANUAL SP §Saving a pattern]` |

### 1.3 Power-on combinations

| Hold while switching on | Effect | Source |
|---|---|---|
| HOLD | Factory reset: CHORD blinks; press CHORD to run it (power off to cancel); when all buttons blink, switch off and on again | `[MANUAL FR]` |
| PLAY | Backup/restore USB drive mode | `[MANUAL FR]` |
| C (EXIT) | Battery-only mode | `[MANUAL FM *2]` `[MANUAL CONN]` |

On the FM-1, implement only the factory reset, with whatever key combination the FM-1 allows.

## 2. UI modes

- **UI-1 Home.** The display always shows the tempo. `[MANUAL SEQ §Setting the tempo]` The format is
  `120.0`, four digits with a decimal point. `[ASSUMED - verify with video]`
- **UI-2 Pattern select.** PATTERN switches the step buttons to choosing a pattern in the current bank, and
  holding PATTERN + step n chooses bank n. `[MANUAL SP]` Pressing PATTERN again returns home.
  `[ASSUMED - verify with video]` (by analogy with SOUND)
- **UI-3 Sound select.** SOUND enters sound select: steps 1–8 or the VALUE knob choose the sound. Holding
  SOUND + step n chooses bank n, and the bank LED stays lit while SOUND is held. Pressing SOUND again, or
  SHIFT + C (EXIT), goes back to the previous screen. `[MANUAL SS]`
- **UI-4 Step edit.** From home, pressing step n (on the current page) selects that step. SHIFT blinks, the
  keyboard LEDs show the step's notes, and the display shows the note of the key pressed. Pressing the same
  step again ends step edit: the keyboard goes dark and the display returns to the tempo.
  `[MANUAL SEQ §Basic operations]` Pressing a different step while editing moves the selection.
  `[ASSUMED - verify with video]` A second way to enter notes is to hold step n and press keys.
  `[MANUAL TP §3]`
- **UI-5 Function screens** (KEY, SHUFFLE, LAST, CLEAR, WRITE, chord set). VALUE edits; D (ENTER) confirms
  CLEAR, WRITE and MENU; C (EXIT) leaves. After CLEAR or WRITE runs, the unit returns to the previous screen.
  `[MANUAL FM §Using the functions]` A value edit takes effect immediately, without ENTER, for KEY, SHUFFLE and
  LAST. `[ASSUMED - verify with video]`
- **UI-6 Menu.** The menu has two levels. On the list level, VALUE scrolls the items and ENTER opens one. On
  the value level, VALUE edits and EXIT goes back to the list; EXIT on the list closes the menu.
  `[MANUAL FM §Using the menus]` The items, in order: `vOL trAn d.Syn bEAt A.vLo P.chg S.cLk CH SYnC thrv txPc
  rxPc Pc.Ch vELo tUnE USb.d A.LnK Cnt.I COPY init`. `[MANUAL FM §List of menus]`
- **UI-7 Step menu.** With a step selected, MENU lists `COPY PSTE INSR DUPL`; ENTER runs the chosen item.
  `[MANUAL SEQ]` Only these four appear in that context. `[ASSUMED - verify with video]`
- **UI-8 Keys inside menus.** While a function or menu screen is open, keys other than EXIT and ENTER still
  play notes. `[ASSUMED - verify with video]`
- **UI-9 Display strings** (from the manual, written as on the 7-segment display): `COPY PSTE INSR DUPL Ptn ALL
  vOL trAn d.Syn bEAt A.vLo P.chg S.cLk CH SYnC thrv txPc rxPc Pc.Ch vELo tUnE USb.d A.LnK Cnt.I init AUTO InT
  NIDI USB OFF On StEP 1_4`. The manual text shows `SñEP`, which is an encoding artefact of `StEP`.
  `[MANUAL FM]` `[MANUAL SEQ]`

## 3. Keyboard, transpose and Chord mode

### 3.1 Keyboard (CHORD off)

- **KB-1 Single notes.** Key k (0–12) plays the note `60 + k + 12*oct + key_tr`, where oct is the octave
  shift and key_tr the KEY transpose. Up to 4 notes sound at once (SD-5). The manual doesn't state the base
  note. `[ASSUMED - verify with capture]` that it is 60: the chord sets sit around C3–C5, and the capture
  preflight saw keys produce 60/64/67 (Appendix A).
- **KB-2 Velocity.** The keys are not velocity-sensitive. Every key note uses the global velocity `vELo`
  (1–127). `[MANUAL FM vELo]` `[ASSUMED - verify with capture]` that the keys really ignore touch: the
  setting would make no sense otherwise.
- **KB-3 Octave.** SHIFT + C# and SHIFT + D# move the keyboard by one octave, in both CHORD off and CHORD on.
  `[MANUAL FM]` The range is not given. `[ASSUMED - verify with video]` -3..+3, global and not saved per
  pattern.
- **KB-4 KEY transpose.** SHIFT + A opens the screen, VALUE edits and EXIT leaves. `[MANUAL CM]` `[MANUAL FM]`
  It transposes the keyboard: the notes keys produce, including chords and the notes recorded into steps. It
  does **not** transpose data already in the sequencer. `[ASSUMED - verify with capture]`, because the manual
  says it transposes the keyboard. **The manual gives no range for KEY.** The -12..12 range in the manual
  belongs to `trAn` (KB-5). `[ASSUMED - verify with video]` -12..+12 semitones, global. The display may show a
  number or a key name (C, C#, ...); a video will tell.
- **KB-5 trAn.** `trAn` (per pattern, -12..12) transposes what the sound generator plays: keys, sequencer and
  phrases. `[MANUAL FM]` `[ASSUMED - verify with capture]` that MIDI out is **not** transposed: the manual
  speaks of the sound generator only.
- **KB-6 Range.** Notes outside 0–127 after transposition are dropped. `[ASSUMED - verify with capture]`

### 3.2 Chord mode

- **CH-1 Toggle.** CHORD turns chord mode on and off (LED lit = on). Each key then plays the chord stored for
  it. `[MANUAL TP §4]` `[MANUAL CM]`
- **CH-2 Chord data.** There are 100 chord sets of 12 chords, one per key from C to B. Set s, key k plays
  `chordsets[s-1].chords[k].notes`: absolute MIDI notes at oct = 0 and key_tr = 0. `[DATA chordsets.json]`
  `[MANUAL CSL]` `[ASSUMED - verify with capture]` that oct = 0 means exactly the listed notes.
  Facts from the data:
  - 1084 chords have 4 notes, 80 have 3 and 36 have 2. Sets 14–16 (Oct/4th/5th Stack) are all 2-note.
    Sets 17, 18, 27, 45, 46 and 79 are all 3-note. Sets 30, 91 and 95 mix sizes.
  - Notes range from 29 to 98.
  - The lowest note matches the key's pitch class in only 735 of 1200 chords (set 43 key D plays a chord over
    G#). The key is an index, not a root: never compute a chord from the key name.
- **CH-3 Transposition.** The notes played are `chord_notes + key_tr + 12*oct`. KB-6 applies.
  `[ASSUMED - verify with capture]`
- **CH-4 Duplicate note.** Set 43, key F (A#/F) lists F3 twice: [53, 53, 58, 62]. `[DATA chordsets.json]`
  Remove the duplicate, so the chord has 3 voices. `[ASSUMED - verify with capture]` that this is a manual
  typo; the chord capture will show the real notes.
- **CH-5 Upper C.** The 13th key in chord mode plays the C chord (index 0) one octave up.
  `[ASSUMED - verify with capture]`: the chord list has only 12 columns.
- **CH-6 One chord at a time.** The last chord key pressed sounds. If you release it while another chord key
  is still held, that chord sounds again; otherwise the chord stops (unless HOLD is on).
  `[ASSUMED - verify with capture]`: chords already use all 4 voices.
- **CH-7 Choosing a set.** SHIFT + CHORD shows the set number (1–100) while CHORD and SHIFT blink. VALUE
  chooses, and pressing CHORD finishes. `[MANUAL CM §Selecting the chord set]`
  `[ASSUMED - verify with video]` the following details:
  - The CHORD press that finishes does not toggle chord mode.
  - The new set takes effect at once, so keys preview it.
  - The set is a global, saved setting.
- **CH-8 STYLE off.** With STYLE OFF, a chord key plays a block chord that sounds while the key is held. The
  manual's advice is to turn STYLE off to hear a chord plainly. `[MANUAL CM §Basic operations]` With STYLE ON,
  the chord's notes become the phrase's held notes. `[MANUAL PF §Basic operations]`
- **CH-9 MIDI in.** Chord mode applies to the J-6's own keys only. MIDI-in notes are not expanded into
  chords. `[ASSUMED - verify with capture]` (Appendix A hint)
- **CH-10 In step edit.** In step edit, a chord key writes the chord into the step (SQ-11).

### 3.3 HOLD (latch, outside step edit)

- **HD-1 Latch.** With HOLD on (lit), the last keys pressed keep sounding after release; turning HOLD off
  cancels. `[MANUAL TP §4]`
- **HD-2 Latch rules.** `[ASSUMED - verify with capture]` These follow the usual Roland latch:
  - While at least one key is physically down, newly pressed keys are added to the latched set.
  - Pressing a key after all keys were up replaces the set.
  - The set holds at most 4 notes; a 5th note drops the oldest.
  - In chord mode, the latched set is the last chord.
  - Turning HOLD off releases every latched note that is not physically down.
- **HD-3 MIDI CC64.** CC64 ≥ 64 acts like HOLD on and < 64 like HOLD off, for MIDI-in notes and keys.
  The MIDI chart calls CC64 "Hold", recognized but not transmitted (8.1). `[MANUAL MIC]`
  `[ASSUMED - verify with capture]` (probe `hold_cc64`) that it is a latch and not a plain sustain pedal.
- **HD-4 Not saved.** The HOLD, CHORD and STYLE ON states of the panel are not saved, except that the pattern
  stores its phrase (PH-16). `[ASSUMED - verify with video]`

## 4. Phrase function

### 4.1 Common rules

- **PH-1 STYLE [ON].** STYLE [ON] turns the phrase function on and off. When it is on, the notes you hold are
  played as arpeggios or phrases according to STYLE and VARIATION. `[MANUAL TP §4]` `[MANUAL PF]`
- **PH-2 Knobs.** The STYLE knob has 9 positions (1–9) and VARIATION has 12 (1–12). Both are absolute: the
  value follows the knob position. `[MANUAL PF §Selecting the phrase type]` `[MANUAL PL]` Use hysteresis
  around the position boundaries. `[ASSUMED]` (implementation detail)
- **PH-3 Categories.** Styles 1–2 are ARPEGGIO, 3–5 BEAT and 6–9 PHRASE. `[MANUAL PL]`
  `[DATA phrase_list.json]` `[WEB roland-article]` says the beat styles play rhythmically rather than as
  arpeggios.
- **PH-4 Held set H.** H is the sorted list (ascending, no duplicates) of the notes feeding the phrase. Its
  source is one of these:
  1. the keys held or latched (CHORD off);
  2. the current chord (CHORD on);
  3. MIDI-in notes;
  4. while the sequencer plays, the notes of the current window (SQ-22).

  H holds at most 4 notes; with more, the 4 most recently added are kept. `[ASSUMED - verify with capture]`:
  pitch order is the Roland norm, the capture sends all notes of a chord at once, and the 4-note limit matches
  the polyphony.
- **PH-5 Phrase clock.** Each phrase is a function of `pt`, the phrase tick.
  - When the sequencer plays, pt is the pattern position in ticks. It is 0 at PLAY and at every loop.
  - When the sequencer is stopped, pt starts at 0 on the key press that makes H non-empty, and runs at the
    current pattern's tempo.

  Phrase timing does not depend on the step length. `[ASSUMED - verify with capture]` Support for this: the
  manual says that with short steps (`1_4`) you should also pick a STYLE 3 phrase of the same length
  `[MANUAL SEQ §Changing the step length]`, which implies the phrase runs on its own grid, not on the steps.
- **PH-6 Stop.** When H becomes empty (no keys, HOLD off, or the sequencer window ends), every phrase note gets
  its note-off at once and no new hits start. `[ASSUMED - verify with capture]`, so that a "double whole note"
  chord does not ring for two bars after the keys are released.
- **PH-7 Gate.** A note of nominal length L ticks sounds for `g(L) = L - min(L/5, 10)` ticks (integer
  division): 16→13, 24→20, 32→26, 48→39, 72→62, 96→86, 192→182, 384→374, 768→758.
  `[ASSUMED - verify with capture]` (the capture records note-off times). The same function sets the gate of
  STYLE OFF sequencer notes (SQ-23).
- **PH-8 Velocity and accent.** Ordinary phrase notes use `vELo`. Accented notes use `A.vLo` (1–127, per
  pattern), which the manual describes as the velocity of the accents included in phrases. `[MANUAL FM A.vLo]`
  Styles 1–5 contain no accents and accents exist only in the captured tables of styles 6–9.
  `[ASSUMED - verify with capture]`
- **PH-9 MIDI-in velocity.** MIDI-in velocity does not change phrase velocity. `[ASSUMED - verify with capture]`

### 4.2 Styles 1–2: arpeggios

- **PH-10 Division.** `[MANUAL PL]` `[DATA phrase_list.json]` The division is 48 ticks (8th) for style 1 and
  24 ticks (16th) for style 2. Variations 7–12 use triplets: 32 ticks (8th triplet) in style 1 and 16 ticks
  (16th triplet) in style 2. Variation v uses order `ORD[(v-1) % 6]`, where
  `ORD = {UP1, UP&DOWN1, DOWN1, DOWN2, UP&DOWN2, UP2}`. (The manual spells "Tripet"; it means triplet.)
- **PH-11 Hits.** Hit i plays at `pt = i * div`, one note at a time, with gate `g(div)`.
- **PH-12 Note order.** The "1"/"2" suffix is the octave range. `[ASSUMED - verify with capture]` Reasons: an
  "UP2" only makes sense as a range, and the variation list is laid out as a mirror (UP1 … DOWN1 | DOWN2 … UP2),
  which is how a range sweep would read. Let B be the base list:
  - range 1: `B = H`;
  - range 2: `B = H` followed by `H + 12`, so `m = 2n` notes.

  The orders:
  - UP plays `B[0..m-1]`.
  - DOWN plays B reversed.
  - UP&DOWN plays `B[0..m-1]` then `B[m-2..1]`: the top and bottom notes are not repeated, so the cycle is
    `2m-2` notes (or 1 when m = 1).

  Hit i plays `cycle[idx % len]`, where idx counts hits since the last arp reset (PH-13).

  Expected sequences for the capture's test chords (`capture_phrases.py` holds these exact notes):

  | Order | n=1 [60] | n=2 [60,67] | n=3 [60,64,67] | n=4 [60,64,67,71] |
  |---|---|---|---|---|
  | UP1 | 60 | 60 67 | 60 64 67 | 60 64 67 71 |
  | UP&DOWN1 | 60 | 60 67 | 60 64 67 64 | 60 64 67 71 67 64 |
  | DOWN1 | 60 | 67 60 | 67 64 60 | 71 67 64 60 |
  | DOWN2 | 72 60 | 79 72 67 60 | 79 76 72 67 64 60 | 83 79 76 72 71 67 64 60 |
  | UP&DOWN2 | 60 72 | 60 67 72 79 72 67 | 60 64 67 72 76 79 76 72 67 64 | 60 64 67 71 72 76 79 83 79 76 72 71 67 64 |
  | UP2 | 60 72 | 60 67 72 79 | 60 64 67 72 76 79 | 60 64 67 71 72 76 79 83 |

  The other reading, "2" = turnaround notes repeated (C E G G E C), is listed in Q-1.
- **PH-13 Arp reset.** The arp index resets to 0 in two cases:
  - H goes from empty to non-empty;
  - H changes to a *different* note set, for example a new sequencer step with other notes, or a different
    chord key.

  A new step with the same notes, or a tie step, does not reset it. When H changes size, idx continues modulo
  the new cycle length. `[ASSUMED - verify with capture]`

### 4.3 Style 3: note-value chords (BEAT)

- **PH-14** Every hit plays all the notes of H together, with gate `g(L)`, at `pt = i * L`.
  `[MANUAL PL]` `[WEB roland-article]` L by variation:

  | v | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 |
  |---|---|---|---|---|---|---|---|---|---|---|---|---|
  | value | 2 whole | whole | half | quarter | 8th | 16th | 2 whole T | whole T | half T | quarter T | 8th T | 16th T |
  | L (ticks) | 768 | 384 | 192 | 96 | 48 | 24 | 512 | 256 | 128 | 64 | 32 | 16 |

  The triplet values (512, 256) do not divide a bar. They run on from the phrase origin (PH-5), modulo L.
  `[ASSUMED - verify with capture]`

### 4.4 Styles 4–5: rhythm strings (BEAT)

- **PH-15 Rhythm strings.** Each string is a loop of sixteenths (24 ticks each). `o` starts a hit, `_` is a
  sixteenth rest, and each `~` lengthens the preceding hit by a sixteenth: `o~` is an eighth and `o~~` a dotted
  eighth. `[MANUAL PL legend]` Each hit plays all the notes of H together.
  `[ASSUMED - verify with capture]` (BEAT category; Roland's article says the beat styles are not
  arpeggiated). Each hit sounds for `g(len)`, and hit times are `pt mod loop`. Hits written as onset/length in
  ticks:

  | v | Style 4 string | loop | hits | Style 5 string | loop | hits |
  |---|---|---|---|---|---|---|
  | 1 | `o___` | 96 | 0/24 | `oo__` | 96 | 0/24, 24/24 |
  | 2 | `_o__` | 96 | 24/24 | `o_o_` | 96 | 0/24, 48/24 |
  | 3 | `__o_` | 96 | 48/24 | `o__o` | 96 | 0/24, 72/24 |
  | 4 | `___o` | 96 | 72/24 | `_oo_` | 96 | 24/24, 48/24 |
  | 5 | `o~o_` | 96 | 0/48, 48/24 | `_o_o` | 96 | 24/24, 72/24 |
  | 6 | `o_o~` | 96 | 0/24, 48/48 | `__oo` | 96 | 48/24, 72/24 |
  | 7 | `o~~o` | 96 | 0/72, 72/24 | `o__` | 72 | 0/24 |
  | 8 | `_o~o` | 96 | 24/48, 72/24 | `_o_` | 72 | 24/24 |
  | 9 | `ooo_` | 96 | 0/24, 24/24, 48/24 | `__o` | 72 | 48/24 |
  | 10 | `oo_o` | 96 | 0/24, 24/24, 72/24 | `oo_` | 72 | 0/24, 24/24 |
  | 11 | `o_oo` | 96 | 0/24, 48/24, 72/24 | `o_o` | 72 | 0/24, 48/24 |
  | 12 | `_ooo` | 96 | 24/24, 48/24, 72/24 | `_oo` | 72 | 24/24, 48/24 |

  Strings and loop lengths: `[MANUAL PL]` `[DATA phrase_list.json]`
- **PH-16 Style 5, variations 7–12.** These are marked "16th, Syncopation" `[MANUAL PL]`: a loop of 3
  sixteenths (72 ticks) set against 4/4. It runs on across bar lines (`pt mod 72`, period 3 beats).
  `[ASSUMED - verify with capture]` The other possibility is a restart every bar (hits at 0, 72, 144, 216,
  288, 360, then 384 = new bar); see Q-6.

### 4.5 Styles 6–9: captured phrase tables (PHRASE)

- **PH-17 Names.** 6 = Chord Phrases (Simple), 7 = Chord Phrases (Rhythmic), 8 = Strummed Chord Phrases
  (Simple), 9 = Strummed Chord Phrases (Rhythmic). 12 variations each, no names. `[MANUAL PL]`
  `[DATA phrase_list.json]` The note data comes from the J-6 capture (`capture/raw/phrases.jsonl`, keys
  `s{style}v{var:02d}n{n}` with n = 1–4 held notes).
- **PH-18 Table slot.** One event list per (style 6–9, variation 1–12, n 1–4) = 192 lists. Proposed
  `firmware/src/j6/j6_phrase_tbl.h`, generated by a host tool from the capture:

  ```c
  typedef struct {          /* one note event of a phrase loop */
      uint16_t t;           /* onset in sub-ticks (1/16 tick) from loop start */
      uint16_t len;         /* gate in sub-ticks */
      uint8_t  src;         /* bits 0-1: index into H (0 = lowest); bit 7: accent (A.vLo) */
      int8_t   semi;        /* semitones added to H[src] (usually 0, +-12, +-24) */
  } j6_pev_t;               /* 6 bytes */

  typedef struct {
      uint16_t loop;        /* loop length in ticks (96 PPQN) */
      uint16_t first;       /* index of the first event in the pool */
      uint8_t  count;       /* number of events */
      uint8_t  flags;       /* bit 0: CAPTURED (0 = placeholder); bit 1: strum offsets are fixed-time, not tempo-relative */
  } j6_plist_t;

  extern const j6_plist_t j6_phrase_lists[4][12][4];   /* [style-6][var-1][n-1] */
  extern const j6_pev_t   j6_phrase_pool[];
  ```

  Rules:
  - Each captured note becomes `(src, semi)` with the smallest `|semi|` such that `H[src] + semi == note`;
    on a tie, the lowest src wins.
  - `t` is the capture time converted to ticks at the recorded tempo, times 16, with 0 at the first note-on.
  - `loop` is the shortest repeat period found in the 4 captured bars.
  - Events that start in the same tick at different sub-ticks are a strum.

  The table is const data in flash. 192 lists of up to 64 events take at most 74 KB; about 25 KB is expected.
- **PH-19 PLACEHOLDER (not J-6 behaviour).** Until a list has the CAPTURED flag, the engine plays these.
  Every list has a 384-tick (one bar) loop.
  - **Style 6, variation v:** the hits of style 4 variation v, repeated 4× per bar. The first hit of the bar
    also plays `H[0] - 12`.
  - **Style 7, variation v:** the hits of style 5 variation v, with the accent flag on the first hit of each
    beat.
  - **Style 8, variation v:** like style 6 variation v, strummed upward: note i of H (ascending) is delayed
    by `2*i` ticks.
  - **Style 9, variation v:** like style 7 variation v, strummed. Hits with an even index strum up and hits
    with an odd index strum down.

  Placeholders are built by the same code path as captured lists, so swapping in real data changes no logic.

### 4.6 Where the phrase setting is stored

- **PH-20 Two levels.** A phrase setting is (STYLE on/off, style, variation). It exists at two levels:
  - **Pattern phrase:** the panel state outside step edit, saved with the pattern.
  - **Step phrase:** set in step edit (SQ-15).

  `[MANUAL PF memo]` Turning the STYLE/VARIATION knobs or STYLE ON outside step edit changes the current
  pattern's phrase at once, also during playback. `[ASSUMED - verify with capture]` Precedence at playback is
  rule SQ-26.
- **PH-21 Live keys during playback.** While the sequencer plays, live keys use the pattern phrase on the
  sequencer's phrase clock: the first hit waits for the next grid position. `[ASSUMED - verify with capture]`

## 5. Sequencer

### 5.1 Data

- **SQ-1 Patterns.** 64 patterns = 8 banks × 8, named `b-p` (1-1 to 8-8). `[MANUAL SP]` Patterns 1-1 to 2-8
  hold factory data `[MANUAL SP]` `[WEB roland-article]`; the rest are empty. The FM-1 has no factory content
  yet: ship 1-1 to 2-8 as simple demo patterns or leave them empty.
- **SQ-2 Steps.** Up to 64 steps per pattern = 8 pages × 8 steps. `[MANUAL SEQ §Switching between pages]`
  The spec page calls the maximum "64 measures" `[MANUAL SPEC]`, and Roland's article calls a step a measure:
  on the J-6 one step usually holds one chord for `bEAt` beats.
- **SQ-3 Step content.** `[MANUAL SEQ]` `[MANUAL PF memo]`
  - **notes:** 0–4 absolute MIDI notes, already shifted by KEY and octave at entry. `[ASSUMED]`: the manual
    says the chord's notes are input, so the step stores notes, not a reference to the set.
  - **chord tag:** key 0–12 and octave of the chord key that entered the notes, plus a mask of which notes
    belong to it. Used for the LEDs and for deleting the chord as a unit (SQ-13).
  - **tie:** one flag.
  - **step phrase:** STYLE on/off, style 1–9, variation 1–12.
  - **velocity:** none per step. `[ASSUMED - verify with capture]` (probe `seq_velocity`)
- **SQ-4 Step length `bEAt`.** Per pattern. The menu lists 1-4, 1-3, 1-2 and 1–8 `[MANUAL FM bEAt]`, and the
  same setting can be edited with PATTERN held `[MANUAL SEQ]`. Step length by value:

  | value | display | step length | ticks |
  |---|---|---|---|
  | 1-4 | `1_4` | 1/4 beat (sixteenth) | 24 |
  | 1-3 | `1_3` | 1/3 beat (eighth triplet) | 32 |
  | 1-2 | `1_2` | 1/2 beat (eighth) | 48 |
  | 1 … 8 | `1` … `8` | N beats | 96 × N |

  The manual's wording for "1–8" is ambiguous. Roland's article says the values go up to 8 beats, so "1–8"
  means 8 separate values, 1 to 8 beats per step `[WEB roland-article]`, 11 values in all. The default for a
  new pattern is 4 (one bar per step, which matches "64 measures"). `[ASSUMED - verify with video]`
  (see Q-14) The longest pattern is 64 × 768 = 49152 ticks.
- **SQ-5 Last step.** 1–64, set by SHIFT + F. `[MANUAL FM]` `[MANUAL SEQ]` The default is 8.
  `[ASSUMED - verify with video]` Choosing page p (SHIFT + step p) with `(p-1)*8 + 1 > last` sets
  `last = 8*p`. `[MANUAL SEQ §Switching between pages]` While SHIFT is held, the step LEDs show the page and
  the page that is playing blinks. `[MANUAL SEQ]`
- **SQ-6 Tempo.** Tempo is per pattern `[MANUAL SEQ]`, in 0.1 BPM steps: the knob alone changes it by 1.0 and
  SHIFT + knob by 0.1. `[MANUAL SEQ]` `[ASSUMED - verify with video]` the range 40.0–300.0 and the default
  120.0.
- **SQ-7 Shuffle.** SHIFT + E opens the screen. `[MANUAL FM]` The J-6 manual gives no range.
  `[ASSUMED - verify with capture]`: per pattern, -90..+90, default 0, as on the P-6 `[WEB p6-manual]`.
  - An event whose onset tick satisfies `tick % 48 == 24` (an off-beat sixteenth) is moved later by
    `shuffle * 384 / 100` sub-ticks, that is `shuffle`% of a sixteenth. Negative values move it earlier.
  - This covers step starts and phrase hits. Triplet grids are never on those positions, so triplets are
    never shifted.
  - Note-offs move with their note-on, so the gate stays the same.

### 5.2 Step entry (step edit)

- **SQ-10 Entering notes.** Select a step (UI-4) and press keys, or hold the step and press keys.
  `[MANUAL SEQ]` `[MANUAL TP §3]`
- **SQ-11 CHORD on.** A chord key **replaces** the whole step with the chord's notes (CH-3), sets the chord
  tag and clears the tie. `[MANUAL SEQ §Basic operations]`
- **SQ-12 CHORD off.** A key whose note is not in the step adds that note, provided the step has fewer than 4
  notes; otherwise nothing happens. `[MANUAL SEQ]` A key whose note *is* in the step (its LED is lit) deletes
  that note. `[MANUAL SEQ §Deleting the contents of a step]` A single note added to a step that holds a 2- or
  3-note chord is allowed, up to 4 notes in total. `[ASSUMED - verify with video]`
- **SQ-13 Deleting a chord.** In a chord step, pressing the chord's lit key deletes all of the chord's notes.
  `[MANUAL SEQ §Deleting]`
- **SQ-14 Key LEDs.** The LEDs show the step's notes inside the current 13-key window. Notes outside the
  window light the keys dimly, and SHIFT + C# / D# moves the window so those notes can be deleted.
  `[MANUAL SEQ §Deleting]` The window is `60 + KEY + 12*oct … +12`: the lit key is the key that plays the
  note, so pressing it deletes the note (SQ-12) whatever KEY is. `[ASSUMED - verify with video]`
- **SQ-15 Step phrase.** In step edit, STYLE [ON] toggles the step's STYLE flag. While the flag is on, the
  STYLE and VARIATION knobs set the step's phrase. `[MANUAL SEQ §Basic operations]` `[MANUAL PF memo]`
  Switching the flag on copies the current knob positions into the step. `[ASSUMED - verify with video]`
- **SQ-16 Clearing a step.** SHIFT + G in step edit clears the step's notes, tie and phrase, after ENTER
  confirms. `[MANUAL SEQ §Deleting]` `[MANUAL FM]`
- **SQ-17 Ties.** `[MANUAL SEQ §Inputting consecutive ties]`
  - On a selected step **with notes**, HOLD puts a tie into the next step. Each further press adds a tie to
    the step after the end of the chain.
  - The step that receives a tie must have no notes. If it has notes, the press is ignored.
    `[ASSUMED - verify with video]` (the manual only says the step must be empty)
  - On a selected step that **holds a tie**, HOLD removes it, and the HOLD LED goes dark. The HOLD LED shows
    the selected step's tie flag.
  - Later ties in the chain stay, but they now continue a rest, so they are silent.
    `[ASSUMED - verify with capture]`
  - Ties do not wrap from the last step back to step 1. `[ASSUMED]`
  - Adding a note to a tie step clears its tie. `[ASSUMED]`

### 5.3 Playback

- **SQ-20 Start and stop.** PLAY starts at step 1 and pressing it again stops. `[MANUAL TP §3]` SHIFT + PLAY
  starts from the selected step `[MANUAL SEQ]`; Roland's article describes holding a step and pressing PLAY
  `[WEB roland-article]`. Accept both.
- **SQ-21 Timeline.** `step_len` comes from SQ-4, and the pattern position `pos` runs from 0 to
  `last*step_len - 1` and loops. Step s starts at `s*step_len` (s from 0). On stop, every note gets its
  note-off at once. `[ASSUMED]`
- **SQ-22 Windows.** A step that has notes and no tie opens a window lasting `step_len*(1+T)`, where T is the
  number of tie steps right after it, counted no further than the last step. Tie steps never retrigger. An
  empty step, with no notes and no tie, is a rest that ends the previous window.
  `[MANUAL SEQ §Inputting consecutive ties]` (ties make long notes) `[ASSUMED - verify with capture]` for the
  exact handling (probe `seq_tie`).
- **SQ-23 Plain playback.** With the effective phrase OFF, all of the window's notes (plus trAn) start at the
  window start and stop after `g(window_len)` (PH-7). Velocity is `vELo`. `[ASSUMED - verify with capture]`
  (probes `seq_basic`, `seq_beat`) An alternative, legato until the next non-empty step, is Q-11.
- **SQ-24 Phrase playback.** With the effective phrase ON, `H` is the window's notes for the length of the
  window, and the phrase engine (section 4) runs on `pt = pos`. Hits fall on the phrase grid, not on step
  starts, and window ends cut notes (PH-6). `[ASSUMED - verify with capture]` (probe `seq_style_step`)
- **SQ-25 Live keys.** Live keys during playback play on top of the sequencer and share the 4 voices. They
  are not recorded unless recording is on. `[ASSUMED - verify with capture]`
- **SQ-26 Which phrase a step uses.** The manual states three rules. `[MANUAL PF memo]` `[MANUAL SEQ §Basic operations]`
  1. A step whose STYLE flag is on uses its own phrase, which has priority.
  2. A step whose STYLE flag is off keeps using the phrase of the step before it.
  3. When the STYLE OFF steps run on from the step where playback began, they use the pattern phrase. When
     every step is OFF, the pattern phrase applies to all of them.

  As an algorithm:

  ```
  at PLAY (from step p0):           cur = pattern.phrase              -- rule 3
  at every step boundary s:         if step[s].style_on: cur = step[s].phrase   -- rule 1
                                    (otherwise cur is kept)                     -- rule 2
                                    effective(s) = cur
  at the loop wrap (last -> 1):     cur is kept                       -- ASSUMED - verify with capture (Q-13)
  ```

  Notes on the algorithm:
  - When `pattern.phrase` has STYLE OFF, the inherited steps play plainly (SQ-23).
  - A tie step or an empty step with its flag on still updates `cur`. `[ASSUMED]`
  - Changing the pattern phrase on the panel during playback affects only the steps that inherit it (rule 3),
    and only until the next step whose flag is on. `[ASSUMED]`
- **SQ-27 Pattern change (`P.chg`, per pattern).** `[MANUAL FM P.chg]` Choosing another pattern during
  playback queues it, and the queued pattern blinks. `[ASSUMED - verify with video]` The switch point depends
  on the *current* pattern's `P.chg`. `[ASSUMED - verify with capture]` (probe `seq_pchg`):
  - `StEP`: at the next step boundary.
  - `N` (1–8): at the next multiple of `N*96` ticks counted from the current pattern's position 0.

  The new pattern starts at its step 1, with its own tempo, sound and settings. If `txPc` is On, the switch
  sends a Program Change (MI-6). Stopped, a pattern change is immediate.
- **SQ-28 Switching patterns while stopped.** This loads the pattern's tempo, sound, phrase and settings.
  Unsaved edits to the old pattern stay in RAM (PS-2).

### 5.4 Real-time recording

- **SQ-30 Starting.** REC enters standby: REC lights and PLAY blinks. PLAY then starts recording, and PLAY
  lights. `[MANUAL SEQ §Inputting notes in real time]`
- **SQ-31 Count-in.** With `Cnt.I` at 2–4, PLAY blinks that many beats before recording starts. `[MANUAL SEQ]`
  `[MANUAL FM Cnt.I]` `[ASSUMED - verify with capture]` the details:
  - One beat is a quarter note (96 ticks) at the pattern tempo.
  - There is no metronome sound.
  - Playback and recording start together at step 1 after the count-in.
- **SQ-32 Which step gets the note.** Keys pressed while a step is playing (its button blinks) go into that
  step. `[MANUAL SEQ]` The step is `floor(pos / step_len)`, with no rounding to the nearest step.
  `[ASSUMED - verify with capture]`: the manual names the step that is playing. Probe `seq_rt_record` can tell
  floor from rounding with its 0.49 s note: floor gives step index 3, nearest gives 4.
- **SQ-33 How notes are written.** The rules are those of step entry (SQ-11 / SQ-12): a chord replaces, a
  single note adds up to 4. A key held across step boundaries writes ties into the following steps while it is
  held, if those steps are empty. MIDI-in notes are recorded like keys. `[ASSUMED - verify with capture]`
- **SQ-34 Recording with STYLE ON.** If STYLE is ON while recording, each step written gets its STYLE flag on
  and the current knob style and variation. `[ASSUMED - verify with capture]`
- **SQ-35 Punch in and out.** `[MANUAL SEQ]`
  - REC during plain playback starts recording at once (punch-in).
  - REC during recording stops recording, and playback continues.
  - PLAY during recording stops both.
- **SQ-36 Looping.** Recording carries on across loops, adding to what is there (overdub).
  `[ASSUMED - verify with capture]`

### 5.5 Step operations and pattern operations

Each of these is chosen with VALUE and run with ENTER. `[MANUAL SEQ §Useful sequencer functions]`
`[MANUAL FM]`

- **SQ-40 COPY (step).** Copies the selected step to a one-step clipboard. The clipboard is lost at power-off.
  `[ASSUMED]`
- **SQ-41 PSTE.** Pastes the clipboard into the selected step, replacing its content. `[MANUAL SEQ]`
- **SQ-42 INSR.** Inserts an empty step at the selected position; that step and the ones after it move one
  step later. `[MANUAL SEQ]` `[ASSUMED - verify with video]` the rest: step 64 is lost and
  `last = min(last+1, 64)`.
- **SQ-43 DUPL.** Works on the whole pattern: steps 1..last are copied to last+1..2·last and last doubles.
  `[MANUAL SEQ]` If `2*last > 64`, the copy stops at step 64 and last = 64. `[ASSUMED - verify with video]`
- **SQ-44 CLEAR (pattern).** SHIFT + G with no step selected initialises the current pattern after ENTER.
  `[MANUAL FM]` `[ASSUMED - verify with video]`: CLEAR resets all steps, last = 8 and the pattern phrase, but
  keeps the tempo, the *1 settings and the sound. The difference from `init` (SQ-46) is that the manual
  mentions the sounds only for `init`.
- **SQ-45 COPY (menu).** Copies the current pattern, including unsaved edits, to a destination chosen with
  VALUE (`1-1` … `8-8`). ENTER saves and EXIT cancels. `[MANUAL FM COPY]` The destination is written to flash
  at once, since the manual calls it saving. `[ASSUMED - verify with video]`
- **SQ-46 init (menu).** Initialises the current pattern, including its sound. `[MANUAL FM init]` Defaults
  are in section 6.

## 6. Settings: per-pattern and global

Settings marked *1 in the menu belong to the current pattern and are saved with it. `[MANUAL FM *1]`
Defaults marked † are `[ASSUMED - verify with video]`.

### 6.1 Per pattern

| Setting | Range | Default | Notes | Source |
|---|---|---|---|---|
| `vOL` | 0–127 | 100† | Pattern volume: gain on the synth before the effects | `[MANUAL FM *1]` |
| `trAn` | -12..12 | 0† | Transposes the sound generator (KB-5) | `[MANUAL FM *1]` |
| `d.Syn` | OFF / On | OFF† | Delay time follows the tempo (SD-4) | `[MANUAL FM *1]` |
| `bEAt` | 1-4, 1-3, 1-2, 1..8 | 4† | Step length (SQ-4) | `[MANUAL FM *1]` |
| `A.vLo` | 1–127 | 127† | Accent velocity (PH-8) | `[MANUAL FM *1]` |
| `P.chg` | StEP, 1–8 | StEP† | Pattern change timing (SQ-27) | `[MANUAL FM *1]` |
| `S.cLk` | 1, 2, 3, 4, 6, 8, 12, 24 | 2† | Pulses per beat on SYNC IN/OUT; one pulse = `96 / S.cLk` ticks | `[MANUAL FM *1]` |
| tempo | 40.0–300.0† | 120.0† | 0.1 BPM steps | `[MANUAL SEQ]` |
| last step | 1–64 | 8† | | `[MANUAL FM SHIFT+F]` |
| pattern phrase | STYLE on/off, 1–9, 1–12 | OFF, 1, 1† | | `[MANUAL PF memo]` |
| steps | 64 × step (SQ-3) | empty | | `[MANUAL SEQ]` |
| sound | 1-1 … 8-8 | 1-1† | `init` also resets the sound, so the pattern holds a sound | `[MANUAL FM init]` |
| knob values | cutoff, resonance, release, attack, delay level, delay time, reverb level, reverb time | the sound's own | `[ASSUMED - verify with video]` that they are saved per pattern | |
| shuffle | -90..90 | 0 | `[ASSUMED - verify with capture]` that shuffle is per pattern (it is on the P-6) | `[WEB p6-manual]` |

### 6.2 Global (no *1 mark)

| Setting | Range | Default | Behaviour | Source |
|---|---|---|---|---|
| `CH` | 1–16 | 1 | MIDI channel for sending and receiving notes | `[MANUAL FM]` `[MANUAL MIC default 1 (Synth)]` |
| `SYnC` | AUTO, InT, NIDI, USB | AUTO† | AUTO follows incoming clock from any port; InT uses the internal clock; NIDI takes clock from the TRS MIDI IN only; USB from USB MIDI only. A cable in SYNC IN always wins. | `[MANUAL FM]` `[MANUAL TP §1]` |
| `thrv` | OFF / On | OFF† | Copies MIDI IN (TRS) to MIDI OUT (TRS); USB is not copied `[ASSUMED]` | `[MANUAL FM]` |
| `txPc` | OFF / On | On† | Send a Program Change when the pattern changes | `[MANUAL FM]` |
| `rxPc` | OFF / On | On† | A received Program Change changes the pattern | `[MANUAL FM]` |
| `Pc.Ch` | 1–16 | 16 | Channel for pattern Program Changes | `[MANUAL FM]` `[MANUAL MIC default 16 (PC)]` |
| `vELo` | 1–127 | 100† | Velocity of key notes and ordinary phrase notes | `[MANUAL FM]` |
| `tUnE` | 433.0–448.0 Hz, 0.1 steps | 440.0 | Master tuning | `[MANUAL FM]` |
| `USb.d` | OFF, 1–127 | OFF† | USB audio level: OFF follows the VOLUME knob, 1–127 is fixed | `[MANUAL FM]` |
| `A.LnK` | OFF / On | OFF† | AIRA LINK; takes effect after a power cycle. Store it but ignore it on the FM-1. | `[MANUAL FM *2]` `[MANUAL CONN]` |
| `Cnt.I` | OFF, 2–4 | OFF† | Count-in beats for recording | `[MANUAL FM]` |
| chord set | 1–100 | 1† | `[ASSUMED - verify with video]` that it is global | `[MANUAL CM]` |
| KEY | -12..12† | 0 | `[ASSUMED - verify with video]` that it is global (KB-4) | `[MANUAL CM]` |
| octave | -3..3† | 0 | Not saved `[ASSUMED]` | `[MANUAL FM]` |

## 7. Sounds and effects

- **SD-1 Sounds.** 64 sounds = 8 banks × 8. `[MANUAL SS]` `[MANUAL SPEC]` (64 preset patches) They are chosen
  as in UI-3. The display shows `b-n`. `[ASSUMED - verify with video]` The sounds are presets and cannot be
  written: no sound-write function appears anywhere in the manual.
- **SD-2 Sound per pattern.** The pattern holds its sound (see `init`). `[MANUAL FM init]` Choosing a sound
  changes the current pattern's sound in RAM, so it is saved by WRITE. `[ASSUMED - verify with video]`
- **SD-3 Knobs.** Eight parameters on four knobs, as listed in section 1. `[MANUAL TP §4]`
  - Roland's article describes the ENVELOPE knob as decay/release and SHIFT + FILTER as adding resonance and
    contour (envelope depth). `[WEB roland-article]`
  - Values are 0–127 internally. A knob is absolute and *replaces* the sound's value once it is moved
    (jump, no pickup). `[ASSUMED - verify with video]`
  - The knobs send no MIDI CC: the MIDI chart lists only CC64, and only as received. `[MANUAL MIC]`
- **SD-4 Delay sync.** With `d.Syn` On, the delay time follows the tempo. `[MANUAL FM]` `[WEB roland-article]`
  SHIFT + DELAY then steps through note values. `[ASSUMED - verify with capture]`
  (probe `fx_delay`, variant `d.Syn on`): 1/32, 1/16T, 1/16, 1/8T, 1/16., 1/8, 1/4T, 1/8., 1/4, 1/2T, 1/4.,
  1/2, in ticks 12, 16, 24, 32, 36, 48, 64, 72, 96, 128, 144, 192. With `d.Syn` OFF the time is free, with a
  range to be measured.
- **SD-5 Voices.** 4 voices. `[WEB roland-article]` This also fits the 4-note step limit `[MANUAL SEQ]`.
  `[ASSUMED - verify with capture]` (probes `voice_steal`, `voice_retrigger`):
  - Voices are shared by keys, sequencer, phrases and MIDI in.
  - A new note takes a free voice; if none is free, it takes the oldest voice.
  - The same note played again reuses its own voice.
- **SD-6 Engine.** The real J-6 models the JUNO-60 (ACB). `[WEB roland-article]` On the FM-1 a placeholder
  "Juno-ish" engine is enough. It needs: an oscillator mix, cutoff, resonance, attack and release (with a
  fixed decay and sustain per preset), delay level and time, reverb level and time.
- **SD-7 Signal path.** Synth (4 voices) → `vOL` → delay → reverb → + MIX IN → VOLUME → MIX OUT. USB audio
  is taken before VOLUME when `USb.d` is 1–127 and after it when OFF. `[ASSUMED]`: the signal-flow image is
  not available. MIX IN reaching MIX OUT is in the manual. `[MANUAL TP §1]`
- **SD-8 Tuning.** The `tUnE` pitch factor is Q16: `(tune10 * 65536 + 2200) / 4400`. For 4400 this gives
  65536. The products fit in 32 bits. Combine it with the semitone table of the oscillator.

## 8. MIDI implementation

### 8.1 Chart (exact copy of the manual's chart: Model J-6, Feb. 10 2022, Version 1.02) `[MANUAL MIC]`

| Function | | Transmitted | Recognized | Remarks |
|---|---|---|---|---|
| Basic channel | Default | 1 (Synth), 16 (PC) | 1 (Synth), 16 (PC) | Memorized |
| | Changed | 1–16 | 1–16 | |
| Mode | Default | Mode 3 | Mode 3 | |
| | Messages | x | x | |
| | Altered | x | x | |
| Note number | | 0–127 | 0–127 | |
| | True voice | ********** | 0–127 | |
| Velocity | Note on | o | o | |
| | Note off | o | o | |
| After touch | Key's | x | x | |
| | Channel's | x | x | |
| Pitch bend | | x | x | |
| Control change | 64 | x | o | Hold |
| Program change | | o | o | |
| | True number | 0–63 | 0–63 | |
| System exclusive | | x | x | |
| System common | Song position | x | x | |
| | Song select | x | x | |
| | Tune request | x | x | |
| System real time | Clock | o | o | |
| | Start | o | o | |
| | Continue | x | x | |
| | Stop | o | o | |
| Aux messages | All sound off | o | o | Remark for the whole Aux block, as printed: "Transmitted: MIDI OFFLINE" |
| | Reset all controllers | x | o | |
| | All notes off | x | o | |
| | Omni mode off | x | x | |
| | Omni mode on | x | x | |
| | Mono mode on | x | x | |
| | Poly mode on | x | x | |
| | Active sensing | o | o | |
| | System reset | x | x | |

o = yes, x = no. Mode 3 = Omni Off, Poly.

### 8.2 Behaviour

- **MI-1 Channels.** Notes are sent and received on `CH` (default 1) and pattern Program Changes on `Pc.Ch`
  (default 16). Both are saved ("Memorized"). `[MANUAL MIC]` Messages on other channels are ignored, apart
  from the system messages.
- **MI-2 What is sent.** `[ASSUMED - verify with capture]` for all of the following except where marked:
  - Key notes (CHORD off), with velocity `vELo`.
  - Phrase notes.
  - Sequencer notes: Roland says the J-6 can play its sequencer on external synths `[WEB roland-article]`.
  - Note Off is sent as a real Note Off message: the chart marks Note Off velocity as transmitted
    `[MANUAL MIC]`. The velocity value is to be measured.
  - MIDI out is not shifted by `trAn`.
  - Chord-mode notes from the J-6's own keys: unknown (Q-23). The preflight saw none (Appendix A).
- **MI-3 What is received.** Note On/Off on `CH` plays the internal synth like keys with CHORD off: it feeds
  the phrase when STYLE is ON (CH-9), and its velocity is used for direct notes but not for phrases (PH-9).
  `[ASSUMED - verify with capture]`
- **MI-4 Control changes.** CC64 is "Hold" (HD-3). No other CC is received or sent, so knobs and buttons
  have no MIDI. `[MANUAL MIC]` Received channel-mode messages:
  - All Sound Off (CC120): silence every voice at once, with no release.
  - Reset All Controllers (CC121): Hold off.
  - All Notes Off (CC123): release every MIDI-held note.

  The behaviour of each is `[ASSUMED]` (standard MIDI meaning); that they are recognized is `[MANUAL MIC]`.
- **MI-5 Active Sensing.** Sent as 0xFE about every 250 ms. When received and then missing for more than
  300 ms, all MIDI-held notes are released. `[MANUAL MIC]` `[ASSUMED]` (standard MIDI timing)
- **MI-6 Program Change.** Values 0–63 select a pattern. `[MANUAL MIC]` `[MANUAL FM txPc/rxPc]` The mapping is
  `PC = (bank-1)*8 + (pattern-1)`, so 0 = 1-1 and 63 = 8-8. `[ASSUMED - verify with capture]` (switch
  patterns on the J-6 with `txPc` On and watch MIDI out; the capture scripts never send PC)
  - A PC received during playback follows `P.chg` (SQ-27). `[ASSUMED]`
  - A PC on the synth channel does nothing (sounds are not on PC). `[ASSUMED - verify with capture]`
- **MI-7 Clock out.** 24 PPQN, one clock every 4 ticks. Start (0xFA) on PLAY and Stop (0xFC) on stop.
  Continue and Song Position are never sent `[MANUAL MIC]`, so SHIFT + PLAY from step n also sends Start.
  `[ASSUMED - verify with capture]` Clock is also sent while stopped. `[ASSUMED - verify with capture]`
  (Appendix A hint)
- **MI-8 Clock in.** When `SYnC` is not InT and clock arrives on an allowed port, every 0xF8 moves the
  sequencer on by 4 ticks. The 4 ticks are spread by the measured interval between clocks (the average of
  the last 8). `[ASSUMED]` Start (0xFA) starts the current pattern at step 1; Stop (0xFC) stops; Continue is
  ignored. `[MANUAL MIC]` With AUTO, the internal clock takes over again after 500 ms without clock.
  `[ASSUMED - verify with capture]`
- **MI-9 SYNC jacks.** SYNC IN pulses count as `96 / S.cLk` ticks each and override `SYnC`.
  `[MANUAL TP §1]` `[MANUAL FM SYnC, S.cLk]` SYNC OUT sends `S.cLk` pulses per beat, each 5 ms long.
  `[ASSUMED]` On FM-1 hardware without the jacks, store the settings only.
- **MI-10 Thru.** With `thrv` On, every byte received on the TRS MIDI IN is copied to the TRS MIDI OUT.
  `[MANUAL FM]`
- **MI-11 "MIDI OFFLINE".** The chart's remark "MIDI OFFLINE" next to All Sound Off is unexplained.
  `[ASSUMED - verify with capture]`: send All Sound Off on `CH` when the unit stops sending MIDI (power-off,
  USB disconnect) and on a panic.

## 9. Saving

- **PS-1 WRITE.** SHIFT + C (WRITE), then `Ptn` (current pattern) or `ALL` (all 64), then ENTER; EXIT
  cancels. `[MANUAL SP §Saving a pattern]` `[MANUAL FM]` At power-off, every unsaved pattern returns to its
  last saved state. `[MANUAL SP]`
- **PS-2 Edits in RAM.** All 64 patterns have a working copy in RAM. Switching patterns keeps unsaved edits:
  the manual speaks of "any unsaved patterns", and WRITE ALL exists. `[MANUAL SP]` (inferred) Boot loads the
  working copies from flash. On the FM-1 a flash erase stops the audio for tens of milliseconds
  (docs/RULES.md), so WRITE mutes the output while it saves; this is acceptable because the user started it.
- **PS-3 Global settings.** Global settings, the chord set and KEY are saved automatically when the menu or
  screen is left. `[ASSUMED - verify with video]` (power-cycle the J-6 after changing `vELo`)
  FM-1: the app calls `j6_engine_save_globals()` when a menu is left (a short mute, like WRITE); WRITE saves
  them too, and otherwise they are saved 2 s after the last change once the output is silent.
- **PS-4 Factory reset.** Restores every pattern and setting to the factory state (1.3). `[MANUAL FR]`
- **PS-5 Backup/restore.** The J-6's USB-drive backup and restore are out of scope for the FM-1. `[MANUAL FR]`

## 10. Implementation figures (engine side, integer only)

- **Step record:** 8 bytes:
  - notes[4], 0xFF = empty;
  - flags: bit 0 tie, bit 1 style_on, bits 2–5 the chord-note mask;
  - style (4 bits) | var (4 bits);
  - chord key (4 bits) | chord octave + 8 (4 bits);
  - 1 spare byte.
- **Pattern:** 64 × 8 + a 32-byte header holding tempo10, last, beat, shuffle, the phrase, the sound, the 8
  knob values and the *1 settings = 544 bytes. All 64 patterns take 34 816 bytes of RAM working copy, plus the
  same in flash. If the FM-1's RAM pool is too small, keep in RAM only the current pattern plus the dirty ones,
  and refuse new edits when the dirty pool is full.
- **Scheduler:** runs at control rate, every 16 frames (0.36 ms), so event jitter stays at 16 samples or
  less. A Q12 sample accumulator against `spt_q12` (C-5) advances the tick. A sub-tick event fires when
  `acc >= frac * spt_q12 / 16`.
- **Clock out:** one 0xF8 when `tick % 4 == 0`. Clock in is MI-8.
- **Divisions:** only 32-bit divisions; no 64-bit `/` or `%` anywhere (docs/RULES.md). `pos % loop` uses
  32-bit values, since the longest loop is 49152 ticks.

## 11. Open questions for the J-6 capture

Each question names the capture that can answer it. (V) means a phone or YouTube video answers it more easily
than MIDI.

**Phrases**

- **Q-1 (s1/s2 v1–6, n = 1–4).** Does the "1"/"2" suffix mean a range of 1 or 2 octaves? Does UP&DOWN repeat
  the top and bottom notes? Compare the PH-12 table note for note.
- **Q-2 (new probe).** Are notes ordered by pitch or by the order they were pressed? Send G4, then C4, then E4,
  50 ms apart, with style 1 v1.
- **Q-3.** When stopped, does the first phrase note sound at once on note-on (PH-5)? Is the arp index reset
  when the chord changes, and when the same chord comes back (PH-13)?
- **Q-4.** Gate, measured as note-off minus note-on in ticks, for every division in styles 1–5 (PH-7).
- **Q-5.** Which notes are accents, in which styles? Set `A.vLo` = 127 and `vELo` = 64 during the phrase
  capture so the two can be told apart. Does MIDI-in velocity reach phrase notes (PH-8 / PH-9)?
- **Q-6 (s5 v7–12).** Does the 3-sixteenth loop run on across bar lines, or restart every bar (PH-16)?
  Check the onsets in bar 2.
- **Q-7 (s3).** Do the long values (768, 512, 384) retrigger exactly every L ticks while the notes are held?
  Is there any sustain after release (PH-6)?
- **Q-8 (s6–s9).** For styles 6–9:
  - the full event lists for n = 1–4;
  - the loop length (is 4 bars enough to see a repeat?);
  - accent positions;
  - strum spacing: capture one strummed variation at 120 and at 90 BPM to see whether the strum follows the
    tempo (PH-18 flag bit 1);
  - whether any note is not an octave of a held note (the `semi` field).
- **Q-9.** With more than 4 notes held, which 4 does the phrase use (PH-4)?
- **Q-10.** When a window ends or the chord changes in the middle of a phrase note, is the note cut or left to
  finish its gate (PH-6 / SQ-24)?

**Sequencer**

- **Q-11 (`seq_basic`, `seq_beat`).** Plain (STYLE OFF) steps: is the note length `g(step)`, the full step, or
  legato until the next note (SQ-23)? The manual's advice to use STYLE 3 with short steps suggests that plain
  steps do not sound short.
- **Q-12 (`seq_tie`).** Is a tie one continuous note with no retrigger? On a phrase step, does a tie keep the
  phrase running without an arp reset? What does an orphan tie do?
- **Q-13 (new probe).** At the loop wrap, does `cur` carry over, or reset to the pattern phrase (SQ-26)? Set
  up 8 steps: steps 1–4 STYLE OFF with the pattern phrase at style 3 v4, step 5 STYLE ON with style 2 v1.
  Listen to steps 1–4 in the second loop.
- **Q-14 (`seq_beat`, V).** Does `bEAt` 1–8 mean 1..8 quarter notes per step? Try 1, 4 and 8. What are the
  `bEAt` and LAST of an empty (init) pattern?
- **Q-15 (`seq_shuffle`, V).** Shuffle range and display. Delay in ticks for each value. Does shuffle move
  eighth-note arpeggios and 1-2 steps? Add a probe: style 1 v1 with maximum shuffle.
- **Q-16 (`seq_pchg`).** The exact switch point for StEP and for N. Does the new pattern start at step 1?
  Does its tempo apply? Is a PC sent at the switch with `txPc` On, and with which number?
- **Q-17 (`seq_rt_record`).** Floor or nearest step (the 0.49 s note tells). Do held keys write ties? Is
  velocity stored? Does a chord merge with or replace existing notes during recording?
- **Q-18 (`seq_countin`).** Is a count-in beat a quarter note? Are clock and Start sent at the start of the
  count-in or at the start of recording?
- **Q-19 (V).** DUPL when last > 32. Does INSR change last? What exactly does CLEAR reset, compared with
  `init`?

**Keyboard and chords**

- **Q-20 (V).** KEY range and display. Is KEY global or per pattern? Does it affect playback or MIDI out?
- **Q-21.** Octave range. Which note does the low C play at octave 0 (60 expected)? Does the octave shift
  apply to chords?
- **Q-22 (chord capture).** What does the upper C play in chord mode? With two chord keys held, does the
  last one win? Real notes of set 43 key F, the duplicate F3.
- **Q-23.** Are chord-mode chords played on the J-6's own keys sent on MIDI out? Are MIDI-in notes expanded
  into chords when CHORD is on? The preflight says no to both (Appendix A); confirm.
- **Q-24.** Velocity of key, sequencer and phrase notes on MIDI out (is it `vELo`?). Note-off format: 0x80
  with which velocity?

**MIDI, sync, sound**

- **Q-25.** Is clock sent while stopped? Does SHIFT + PLAY from step n send Start? Does the J-6 start at step
  1 on an external Start? How long until AUTO falls back to the internal clock?
- **Q-26 (`hold_cc64`).** Is CC64 a latch (like HOLD) or a sustain pedal?
- **Q-27 (`voice_steal`, `voice_retrigger`).** Which voice is stolen? Does a repeated note retrigger its own
  voice?
- **Q-28 (`fx_delay`, `fx_reverb`).** Delay and reverb time ranges. The note values `d.Syn` offers.
- **Q-29 (V).** Tempo range (minimum and maximum) and the display format.
- **Q-30.** The Program Change numbering (watch `txPc` while switching patterns on the J-6).
- **Q-31.** When is All Sound Off sent ("MIDI OFFLINE")? How often is Active Sensing sent?
- **Q-32 (V).** Do knobs jump or pick up? Are knob values saved per pattern? Does SHIFT + knob move only the
  second parameter?
- **Q-33 (V).** Display formats for octave, KEY, shuffle, chord set, style-variation, the note name in step
  edit, pattern number and sound number. LED behaviour while a pattern change is queued, and for the page
  shown during playback.
- **Q-34 (V).** Do the chord set, KEY and shuffle belong to the pattern? Is the sound choice saved with
  WRITE? Are global settings saved without WRITE?

## Appendix A: hints from the capture preflight (not used as sources)

`capture/raw/preflight.json` already exists. It was read only to target the questions above, and none of the
rules rely on it. It suggests:

- Keys with CHORD off send notes on channel 1 (60, 64, 67 at velocity 100).
- Chord-mode presses on the J-6's own keys sent **no** MIDI (`local_chord_out` empty, verdict
  `chord_mode: audio`).
- A MIDI-in note with CHORD on came back as the same single note (thru or echo, not expanded).
- Phrase notes **are** sent, both for local keys and for MIDI-in notes (an UP pattern 60, 64, 67 …).
- MIDI clock flows while the J-6 is idle.

All of these feed Q-23, Q-25 and CH-9 / MI-2 / MI-7 and still need confirming.

## Appendix B: sources beyond the manual

- Roland, "Getting to Know AIRA Compact: J-6 Chord Synth", articles.roland.com/getting-to-know-aira-compact-j-6.
  Used for: the style categories, beat styles not being arpeggiated, banks 1–2 holding the factory patterns,
  `bEAt` values up to 8 beats, 4 voices, the JUNO-60 model, delay sync, and starting from a held step.
- Roland P-6 manual, the SHUFFLE page (static.roland.com/manuals/p-6): shuffle range -90..90 and 0 = straight.
  Used only for SQ-7.
- YouTube: there are many J-6 demos and tutorials. They are the cheapest way to answer the questions marked
  (V): display text, LEDs, knob behaviour, ranges. Their content has not been reviewed for this spec, so
  nothing here relies on them.
