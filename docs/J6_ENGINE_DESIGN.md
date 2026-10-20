# J-6 engine design for the FM-1

This document describes how the J-6 engine in `firmware/src/j6/` is built: the modules, what each one owns,
how they meet, the time base, the memory budget and the tests. The behaviour each module implements is in
[J6_SPEC.md](J6_SPEC.md). Rule IDs such as `SQ-26` refer to it. The shared types and every function
prototype are in [`firmware/src/j6/j6.h`](../firmware/src/j6/j6.h), which is the contract: when this
document and `j6.h` disagree, `j6.h` wins and this document gets fixed.

Four modules are written in parallel, each by its own author, against `j6.h`. The glue (`j6_engine.c`),
which wires them to each other, to MIDI and to storage, comes afterwards. So does the FM-1 app (panel,
screen, LEDs).

## 1. Module map

```
           FM-1 keys / buttons / encoders            USB MIDI in                     flash
                         |                                |                            ^
                         v                                v                            |
   main loop    +-----------------------------------------------------------------------------+
                |  j6_engine_key / _cmd / _set / _midi_in   ->  command ring   j6_engine_write  |
                +-----------------------------------------------------------------------------+
   audio ISR                                   | (drained at each render)
                +-----------------------------------------------------------------------------+
                |  glue: j6_engine.c                                                           |
                |                                                                              |
                |   chord  --notes-->  held tracker x2 --H--> phrase (live) ---\               |
                |   (key -> notes)     (keys, MIDI in)                          \              |
                |                                                               events --> route --> synth + fx --> out
                |   clock --ticks--> seq --NOTE_ON/OFF, HELD, STEP...----------/   |          |
                |                     \--HELD--> phrase (seq) -----------------/     v          |
                |                                                              MIDI out ring   |
                +-----------------------------------------------------------------------------+
                                                                                     |
   main loop                                                    j6_engine_midi_out -> midi_send
```

No module calls another module's functions. The glue is the only code that knows them all. Modules share
only `j6.h`: its constants, its inline helpers (`j6_gate`, `j6_shuffle_offset`, `j6_spt_q12`, ...), the event
queue and the data types.

### 1.1 Who owns which file

| Module | Owns (paths from the repository root) | Test |
|---|---|---|
| **chord** | `tools/gen_tables.py`, `firmware/src/j6/j6_chord_tables.h` (generated), `firmware/src/j6/j6_chord.c` | `tests/j6/test_chord.c` |
| **phrase** | `tools/gen_phrase_tables.py`, `firmware/src/j6/j6_phrase_tables.h` (generated), `firmware/src/j6/j6_phrase.c`, `firmware/src/j6/j6_phrase_held.c`, `firmware/src/j6/j6_phrase_state.h` | `tests/j6/test_phrase.c` |
| **seq** | `firmware/src/j6/j6_seq.c`, `firmware/src/j6/j6_seq_pattern.c`, `firmware/src/j6/j6_seq_clock.c`, `firmware/src/j6/j6_seq_state.h` | `tests/j6/test_seq.c` |
| **synth** | `tools/gen_synth_tables.py`, `firmware/src/j6/j6_synth_tables.h` (generated), `firmware/src/j6/j6_synth.c`, `firmware/src/j6/j6_synth_fx.c`, `firmware/src/j6/j6_synth_sounds.c`, `firmware/src/j6/j6_synth_state.h` | `tests/j6/test_synth.c` |
| glue (later) | `firmware/src/j6/j6_engine.c` | `tests/j6/test_engine.c` |
| design (this step) | `firmware/src/j6/j6.h`, `docs/J6_ENGINE_DESIGN.md`, `tests/j6/j6_test.h`, `tests/j6/run_tests.sh`, `tests/j6/lint_engine.py`, `tests/j6/test_header.c` | `tests/j6/test_header.c` |

A module author creates and edits only the files in their row. `j6.h` is frozen for the parallel phase.
If a module finds that `j6.h` is wrong or missing something, it says so in its report and works around it
inside its own files. It does not edit `j6.h`.

### 1.2 Private state headers

Each module with state has a header `j6_<module>_state.h` that defines its structs (`struct j6_seq_s` and so
on). `j6.h` only names these types (`typedef struct j6_seq_s j6_seq_t;`), so its prototypes compile
without them. Only three kinds of code include a state header:

- the module's own `.c` files;
- the module's test;
- the glue, which allocates the instances statically.

The design step wrote a first layout for each state header. The module owns that file from then on and
may change it freely, as long as it keeps:

- the struct name;
- the `J6_STATIC_ASSERT` that checks the size against the budget in `j6.h`.

Nobody outside the module reads the fields.

### 1.3 One translation unit

The firmware is built as one C file: the app includes `platform.c`, then every engine `.c` file. This is
the house style (`morpheus_app.c` does the same), and it means neither `build.py` nor `run_host.sh` needs a
new source list. It has two consequences:

- Every file-scope name must be unique across the whole engine and must not clash with the platform. That
  covers static functions, static tables, struct and enum tags, enumerators and macros. Each module uses
  its prefix:

  | Module | Names | Macros |
  |---|---|---|
  | chord | `j6c_` | `J6C_` |
  | phrase | `j6p_` | `J6P_` |
  | seq | `j6s_` | `J6S_` |
  | synth | `j6y_` | `J6Y_` |
  | glue | `j6e_` | `J6E_` |

  Public API names use `j6_` / `J6_`. The lint checks this.
- Platform macros leak into engine code. `FS`, `MQ`, `RGB`, `KEY_LED`, `CV_MAX` and the rest must never
  be used as identifiers. The lint reads the macro list from the platform sources and checks this too.

The host tests compile each engine file separately. `run_tests.sh` also compiles all of them as one
translation unit, so collisions show up before the firmware build.

## 2. Time

### 2.1 Units

| Unit | Value | Where |
|---|---|---|
| frame | 1/44100 s | audio |
| control chunk | `J6_CR` = 16 frames (0.36 ms) | the glue runs time and events per chunk; the synth updates coefficients per chunk |
| block | `J6_BLOCK` = 256 frames (5.8 ms) | one `app_audio()` call |
| tick | 1/96 quarter note (`J6_PPQN`) | every musical time: steps, phrases, gates, MIDI clock (4 ticks) |
| sub-tick | 1/16 tick (`J6_SUB`) | only shuffle and strum offsets (C-3) |
| tempo | tenths of a BPM (`tempo10`) | 1200 = 120.0 BPM |

Samples per tick in Q12 are `j6_spt_q12(tempo10) = 1128960000 / tempo10`. At 120 BPM this gives 940800,
which is 229.6875 samples exactly. One second at 120 BPM is exactly 192 ticks. Even at 300 BPM a tick lasts
92 frames, so at most one tick starts per control chunk.

### 2.2 The clock (`j6_seq_clock.c`, seq module)

The clock turns frames into ticks.

- **Internal clock:** `acc_q12 += frames << 12`; while `acc_q12 >= spt_q12`, subtract and count a tick.
- **External clock:** each MIDI clock received (`j6_clock_ext_pulse`) is worth 4 ticks. They are released
  evenly over the average of the last 8 pulse intervals, measured in frames. If the next pulse arrives
  first, the ticks still owed are released at once, so the count never drifts.

The clock runs all the time, whether or not the sequencer plays: the live phrase and MIDI clock out need
it. `j6_clock_sub()` gives the position inside the current tick, 0..15, which the glue uses to place
sub-tick events.

This replaces the morpheus scheme of polling `fm1_ms` in the main loop. That scheme had 1 ms resolution
and lost the loop latency on every step (platform study, section 4).

### 2.3 The audio-rate loop (glue)

```
j6_engine_render(out, n):
    apply every queued command (keys, MIDI in, buttons, settings)
    for each 16-frame chunk c:
        k = j6_clock_advance(clock, 16)                 # 0 or 1 ticks
        for each new tick:
            evq.clear; j6_seq_tick(seq, evq);           route(evq)    # seq first: HELD changes H
            evq.clear; j6_phrase_tick(seq_phr, j6_seq_pos(seq), evq); route(evq)
            evq.clear; j6_phrase_tick(live_phr, live_pt, evq);        route(evq)
            if tick % 4 == 0: queue MIDI clock 0xF8
        fire pending events whose sub <= j6_clock_sub(clock)
        j6_synth_render(synth, out + 32*c, 16)
```

`route()` sends an event with `sub == 0` on at once and puts the others in a small pending list (32
entries). A pending event fires in the first chunk where `j6_clock_sub()` has reached its `sub`, or when
the next tick starts. Event timing is therefore exact to one chunk (16 frames, 0.36 ms), which is
inaudible, and never drifts.

`live_pt` is the live phrase's clock (PH-5). While the sequencer plays it equals `j6_seq_pos()`, so live
phrases lock to the pattern (PH-21). While stopped it counts ticks from 0, starting at the press that makes
the live held set non-empty.

## 3. Events

Phrase and sequencer tick functions append the events of one tick to a `j6_evq_t`. That is a plain array of
64 six-byte `j6_event_t`, with an inline `j6_evq_push()`. The glue drains it after every call. The rules
(also in `j6.h` section 4):

- A tick function pushes only the events of that tick. Anything later stays in the producer's own state.
- At the same note and the same sub-tick, NOTE_OFF comes before NOTE_ON, so a retrigger works.
- Every NOTE_ON gets exactly one NOTE_OFF later, including on stop, on set changes and on re-init. The
  tests check this with `j6t_log_balanced()`.
- Functions that are not tick functions (`j6_seq_stop`, `j6_phrase_set_held`, `j6_seq_select`, ...) push
  with `sub = 0`, meaning "now".
- `lost` counts refused pushes. It must stay 0; a producer never needs more than about 24 events in a tick.

`src` is the producer's tag (`J6_SRC_*`). The glue's MIDI-out rules depend on it (section 8.3).

| Event | From | Meaning | Glue does |
|---|---|---|---|
| `NOTE_ON` / `NOTE_OFF` | phrase, seq | a note starts / ends (`note`, `vel`) | synth note on/off with `note + trAn`; MIDI out by `src` |
| `HELD` | seq | the sequencer's phrase input changed | reads `j6_seq_window()`; on the seq phrase instance: `set_style` from `win.phrase`, `set_vel`, `set_held(win.note, win.n)`; applied at once, before the phrase ticks of the same tick |
| `STEP` | seq | playhead at step `arg` | LEDs / screen |
| `LOOP` | seq | wrapped to the first step | (nothing yet) |
| `PATTERN` | seq | pattern `arg` became current | load tempo into the clock, sound and knobs into the synth, trAn, shuffle into both phrases; Program Change out if `txPc` |
| `START` / `STOP` | seq | playback started / stopped | MIDI 0xFA / 0xFC; on STOP, stop both phrases |
| `COUNTIN` | seq | count-in beat, `arg` beats left | blink PLAY |
| `REC` | seq | recording on/off (`arg`) | REC LED |

## 4. Module: chord

**Files:**

- `tools/gen_tables.py`
- `firmware/src/j6/j6_chord_tables.h`
- `firmware/src/j6/j6_chord.c`
- `tests/j6/test_chord.c`

**State:** none. Pure functions plus const tables.

### 4.1 Behaviour

- **Single notes:** `j6_key_note(key, oct, key_tr) = 60 + key + 12*oct + key_tr`, or -1 outside 0..127
  (KB-1, KB-6).
- **Key codes:** semitones above the keyboard's low C. The J-6 has 0..12. The FM-1 has -7..19: bit `b` of
  `fm1_in.notes` is key `b - 7`, so F3 is -7 and C4 is 0.
- **Chord keys:** `j6_chord_notes(set, key, oct, key_tr, out, &tag)`:
  - chord index `j6_key_chord_idx(key)` (key mod 12), octave shift `j6_key_floor12(key) + oct`;
  - so key 12 plays the C chord one octave up (CH-5), and FM-1 keys below C play the chords one octave
    down: the same rule, extended to 27 keys;
  - notes are `listed + 12*(octave shift) + key_tr` (CH-3);
  - notes outside 0..127 are dropped; the rest come out ascending and distinct;
  - the tag is `j6_chord_tag(idx, octave shift)`.
- **Raw chords:** `j6_chord_raw` returns the chord exactly as listed, minus duplicates (CH-4: set 43 key F
  has 3 notes). Notes are never computed from the chord's name (CH-2).
- **Names:** `j6_chord_name` / `j6_chord_genre` return the names from the data, for the FM-1 screen.
  `j6_note_name` gives "C4", "F#3", "C-1" (C-1: C4 = 60).
- **Indices:** sets are 0..99 in the API; the UI shows 1..100.

### 4.2 Generator `tools/gen_tables.py`

- **Input:** `data/chordsets.json`, which it only reads. No other input.
- **Output:** `firmware/src/j6/j6_chord_tables.h`, included only by `j6_chord.c`. Every name in it starts
  with `j6c_`. Suggested layout:
  - `static const uint8_t j6c_notes[100][12][4]`, with 0 for an unused slot (the lowest real note is 29);
  - a string pool of chord names with `uint16_t` offsets;
  - a genre index per set and a genre string pool.

  The notes are written exactly as in the JSON, duplicates included; the C code removes duplicates. This
  keeps the table faithful to the data, so a corrected `chordsets.json` (after the chord capture) only
  needs a regeneration.
- **Format:** the first line is a "generated by tools/gen_tables.py from data/chordsets.json - do not
  edit" comment, with the SHA-256 of the input.
- **`--check`:** regenerates in memory and exits 1 if the committed header differs. `run_tests.sh` runs it.
- **Error handling:** it fails loudly if the JSON breaks an assumption: 100 sets, 12 chords per set, 2–4
  notes per chord, notes in 0..127, ASCII names of at most 12 characters.
- **Running it:** it runs with `python3 -I` and the standard library only.

### 4.3 Tests (`test_chord.c`)

- **Spot checks against the data:**
  - set 1 (index 0) key C is 48 55 62 64;
  - a few chords across the sets, including set 43 key D (a chord over G#);
  - set 43 key F has 3 notes: 53 58 62.
- **Statistics:**
  - after `j6_chord_raw` removes duplicates: 1083 four-note chords, 81 three-note and 36 two-note. The
    JSON lists 1084 / 80 / 36, and set 43 key F is the only chord with a duplicate;
  - the overall note range is 29..98;
  - every listed chord is already ascending.
- **`j6_key_note`:** every FM-1 key at octaves -3..3 and KEY -12..12, including the -1 cases.
- **`j6_chord_notes`:**
  - key 12 gives the C chord +12;
  - key -7 gives the F chord -12;
  - the octave and `key_tr` shifts;
  - out-of-range notes are dropped (set a note near 127 at octave +3);
  - the tag, and `j6_chord_tag_key(tag, oct) == key`.
- **Names and strings:** names, genres, `j6_note_name` at 0, 60, 61 and 127, and the empty string for an
  index out of range.

## 5. Module: phrase

**Files:**

- `tools/gen_phrase_tables.py`
- `firmware/src/j6/j6_phrase_tables.h`
- `firmware/src/j6/j6_phrase.c`
- `firmware/src/j6/j6_phrase_held.c`
- `firmware/src/j6/j6_phrase_state.h`
- `tests/j6/test_phrase.c`

### 5.1 Held tracker (`j6_held_*`)

The tracker turns presses and releases into the effective set: what sounds directly with STYLE off, or
what feeds a phrase as H. The glue keeps two trackers:

- **keys:** CHORD off: poly with MIDI notes as ids; CHORD on: mono with chord key codes + 64 as ids;
- **MIDI in:** always poly.

The HOLD button and CC64 drive the latch of both (HD-3). The rules are in `j6.h` 9.2:

- **Poly, latch off:** the effective set is the last 4 ids pressed among those still down. With C D E F G
  held, it is D E F G; release G and C comes back.
- **Poly, latch on (HD-2):**
  - while any id is down, presses add to the latched set (a 5th drops the oldest);
  - a press after everything was up replaces the set;
  - releases change nothing;
  - turning the latch on latches what is down; turning it off leaves only what is down.
- **Mono (CH-6):** the last pressed id still down. Releasing it brings back the previous still-down one.
  With the latch on, the last chord stays.
- **`j6_held_take_change()`:** returns 1 once after each change of the effective set. The glue polls it.

### 5.2 Phrase engine (`j6_phrase_*`)

The engine plays H, the sorted held set of at most 4 notes, as style `s` and variation `v` on the clock
`pt`. There is one instance per source; each tags its events with `src` (`J6_SRC_PHRASE_LIVE` or
`J6_SRC_PHRASE_SEQ`).

**Timing contract.** `j6_phrase_tick(p, pt, q)` is called exactly once per elapsed tick. A hit belongs to
the tick in which its shuffled onset falls, measured in sub-ticks: `[pt*16, pt*16+16)`.

- `pt` may jump (pattern loop, play from a step), but time always advances by one tick per call.
- Note-offs are timed by an internal elapsed-tick counter, not by `pt`.

The styles:

| Styles | How hits are made | Notes per hit | Gate |
|---|---|---|---|
| 1–2 arpeggio (PH-10..13) | division 48 / 24 ticks (v7–12: 32 / 16); hit at `pt % div == 0`; order `ORD[(v-1) % 6]` over the base list B (range 2 = H then H+12 when `J6_CFG_ARP_RANGE2`); UP&DOWN without repeated ends; index `idx % cycle_len` | 1 | `j6_gate(div)` |
| 3 note values (PH-14) | L from the table (768 … 16); hit at `pt % L == 0` | all of H | `j6_gate(L)` |
| 4–5 rhythm strings (PH-15/16) | the 24 strings as onset/length tables in ticks; loop 96, or 72 for style 5 v7–12 (`J6_CFG_S5_RUN_ON`: `pt % 72`; else `pt % 384 % 72`) | all of H | `j6_gate(len)` |
| 6–9 tables (PH-17..19) | list `bank->lists[j6_plist_index(s, v, n)]`, n = \|H\|; event e fires when `(pt % loop) * 16 + sub == e.t` | one per event: `H[e.src & 3] + e.semi` | `e.len` sub-ticks |

The expected arpeggio sequences for n = 1..4 are the table in PH-12. Test all 24 cells note for note.

**Velocity.** `vel` (vELo) for every note. An event with `J6_PEV_ACCENT` uses `avel` (A.vLo) instead
(PH-8). Styles 1–5 have no accents. MIDI-in velocity is never used (PH-9).

**Shuffle.** The onset of every hit is shifted by `j6_shuffle_offset(onset_sub, shuffle)`. It is measured
from the phrase origin, which is 0 for algorithmic styles and the loop start for tables. The note-off
moves with its note-on.

- **Positive shuffle:** the hit is computed at its nominal tick and parked in the pending list until its
  shifted tick.
- **Negative shuffle:** the engine must look ahead up to 22 ticks (90% of a sixteenth is 21.6 ticks).

**Set changes.** These rules are partly assumed (PH-6, PH-13); the late join is a design decision:

- **H becomes empty:** NOTE_OFF for everything sounding, at once; no further hits.
- **H changes to a different non-empty set:**
  - notes whose source note left H stop at once;
  - the arp index resets;
  - *late join*, for BEAT and PHRASE styles: if a hit is still inside its gate, the new set's notes start
    now and run for the rest of that gate, unless a hit onset falls in this tick anyway.

  Without the late join, keys pressed a few ms apart, or a chord change in the middle of a two-bar style 3
  note, would stay silent until the next grid position.
- **The same set again** (a new step with the same notes, a tie): nothing happens and the index carries on.

**Tables (`j6_phrase_builtin`).** These are defined in `j6_phrase.c` from the generated
`j6_phrase_tables.h`. `j6_phrase_set_bank()` swaps in a test bank, so tests of the table mechanism do not
depend on placeholder content.

### 5.3 Generator `tools/gen_phrase_tables.py`

**Default mode (no arguments).** Writes the PH-19 placeholders for all 192 lists, with flags 0
(placeholder), a 384-tick loop and gates `g(len)` in sub-ticks:

- **Style 6:** style 4's hits ×4 per bar, plus `H[0] - 12` on the first hit of the bar.
- **Style 7:** style 5's hits with an accent on the first hit of each beat.
- **Style 8:** style 6, strummed upward: note i is 2·i ticks late.
- **Style 9:** style 7, strummed: even hits up, odd hits down.

**`--capture capture/raw/phrases.jsonl`.** Converts every captured key `s{style}v{var:02d}n{n}` by the
PH-18 rules and marks those lists `J6_PL_CAPTURED`. Lists not captured stay placeholders. Hints from the
data captured so far:

- the J-6's own `F8` clocks are a better time base than the note-on times;
- there are only two velocities: 100 = vELo and 127 = A.vLo, the accents;
- loops of 768 ticks (2 bars) are common;
- gates are almost the full nominal length;
- a phrase's first note fires at the key press and the rest sit on the J-6's grid.

Choose and document a quantisation rule. Lists may need up to about 256 events; `count` is 16 bits.

**Header.** The first line records the mode and the SHA-256 of the capture file.

**`--check`.** Regenerates with the recorded mode and compares. In capture mode, when the capture file's
hash no longer matches, it prints "capture changed, not checked" and exits 0, so the tests don't fail as
the capture grows. Only the standard library is used, and the script never writes anything under
`capture/`.

### 5.4 Tests (`test_phrase.c`)

**Held tracker:**

- every HD-2 bullet;
- CH-6 mono fallback;
- the last-4 rule;
- `take_change` firing once.

**Algorithmic styles:**

- every cell of the PH-12 table, for the six orders × n = 1..4 in style 1 v1–6 and style 2 v7–12;
- the divisions, onsets and gates of every style 1–5 variation, including both `J6_CFG_S5_RUN_ON` cases.

**PH-13 resets:** empty → set; a different set; the same set.

**Set changes:**

- empty cuts everything at once;
- the late join on style 3 v1;
- staggered presses (C at tick 0, E at tick 2) make one chord.

**Table styles:**

- a test bank: notes, semis, accent → avel, loop wrap, list chosen by n;
- `j6_phrase_builtin`: every list non-empty, sorted, `t < loop*16`, `src < n`.

**Shuffle:**

- +50 and -90 on style 2 v1 move the odd sixteenths only;
- style 1 v7 (triplets) is never moved.

**Invariants:**

- `pt` jumps (wrap from 3071 to 0) keep gates correct;
- every log balanced (`j6t_log_balanced`);
- `q.lost == 0` throughout.

## 6. Module: seq

**Files:**

- `firmware/src/j6/j6_seq.c` (instance, transport, playback, recording)
- `firmware/src/j6/j6_seq_pattern.c` (pattern and step operations, flash format)
- `firmware/src/j6/j6_seq_clock.c` (section 2.2)
- `firmware/src/j6/j6_seq_state.h`
- `tests/j6/test_seq.c`

### 6.1 Data

`j6_step_t` (8 bytes) and `j6_pattern_t` (536 bytes) are public and fixed in `j6.h`. They are plain bytes,
so the layout is the same on the host and the chip without any packing, and the size asserts hold.

- **A step:**
  - up to 4 ascending distinct notes, then `J6_NO_NOTE`;
  - a tie flag;
  - a chord-note mask (bits 4–7, a bit per `note[]` slot);
  - the step phrase byte, with the STYLE flag in bit 7;
  - the chord tag.

  A step with neither notes nor a tie is a rest.
- **A pattern:** the header holds tempo, last, bEAt, shuffle, the pattern phrase, sound, vOL, trAn, d.Syn,
  A.vLo, P.chg, S.cLk and 8 knob values (`J6_KNOB_UNSET` = the sound's own).
- **`j6_pattern_init` (SQ-46):** tempo 120.0, last 8, bEAt 4, shuffle 0, phrase OFF style 1 var 1,
  sound 0, vOL 100, trAn 0, d.Syn off, A.vLo 127, P.chg StEP, S.cLk 2, knobs unset, all steps empty.
- **`j6_pattern_clear` (SQ-44):** steps, last and phrase back to the defaults; tempo, the *1 settings,
  sound and knobs are kept.

**Flash format** (`j6_pattern_encode` / `_decode`): 544 bytes. A 32-byte header, laid out in `j6.h`
section 5, with a format byte and a little-endian tempo, followed by 64 × 8 bytes of steps.

- `decode` rejects an unknown format byte (returning -1 after `j6_pattern_init`) and clamps every field.
- A note above 127 becomes empty, notes are re-sorted, the mask is fixed, and a bad phrase byte decodes to
  a valid one.

The glue adds its own magic, length and CRC per flash sector.

**Working copies (PS-2).** `j6_seq_t` holds all 64 patterns in RAM plus a dirty bit for each.

- `j6_seq_edit(idx)` returns a writable copy and marks it dirty.
- `j6_seq_set_saved(idx)` clears the bit once the glue has written that pattern, or loaded it at boot.
- Recording and every step operation go through `j6_seq_edit`.

### 6.2 Step and pattern operations

| Function | Rule | Details to get right |
|---|---|---|
| `j6_step_key_note` | SQ-12 | add (if < 4 notes; clears the step's tie), remove if present; keep the order and fix the chord mask |
| `j6_step_key_chord` | SQ-11, SQ-13 | same tag present → remove the chord's notes only (keep added single notes); else replace the whole step, set tag and mask, clear the tie |
| `j6_step_tie` | SQ-17 | step has notes → tie into the first step after the tie chain, if it is < last and empty, else nothing; step is a tie → remove its tie; returns the target step |
| `j6_step_set_phrase` | SQ-15 | writes the byte as given |
| `j6_pattern_insert` | SQ-42 | the steps from `step` on move one later, step 63 is lost, `step` becomes empty, `last = min(last+1, 64)` |
| `j6_pattern_dupl` | SQ-43 | steps 0..last-1 copied after last, `last = min(2*last, 64)` |
| `j6_pattern_page` | SQ-5 | `page*8 + 1 > last` → `last = 8*(page+1)` |
| `j6_seq_step_copy` / `_paste` | SQ-40/41 | one-step clipboard in the instance |
| `j6_seq_copy_pattern` | SQ-45 | working copy to working copy, dst dirty (the glue saves dst) |

### 6.3 Transport

```
STOPPED --PLAY--> PLAYING                     STOPPED --REC--> STANDBY --REC--> STOPPED
STANDBY --PLAY--> COUNTIN (Cnt.I set) --N beats--> PLAYING + rec        (count-in: SQ-31)
STANDBY --PLAY--> PLAYING + rec (Cnt.I off)
PLAYING --REC--> rec on (punch in) / rec off (punch out)                (SQ-35)
any --STOP--> STOPPED (NOTE_OFFs, HELD n=0, STOP)                       PLAY while playing = STOP (glue)
COUNTIN --REC--> STOPPED [ASSUMED]
```

### 6.4 One tick (`j6_seq_tick`)

Let `p` be the position the tick processes, `sl` the step length (`j6_beat_ticks`) and `len = last * sl`.

1. **Count-in:**
   - push `COUNTIN(beats left)` on each beat (every 96 ticks);
   - after N×96 ticks the next tick is position 0 with `START(0)` and `REC(1)`.
2. **Wrap:** if `p >= len`, set p to 0 and push `LOOP`. With `J6_CFG_WRAP_KEEPS_PHRASE` 0, `cur` resets to
   the pattern phrase (Q-13).
3. **Queued pattern (SQ-27):**
   - the switch point is set by the *current* pattern's P.chg: StEP at the next step boundary
     (`p % sl == 0`), N at `p % (N*96) == 0`;
   - at the switch: end the window, `cur` = the new pattern, p = 0, `cur_phrase` = its phrase, push
     `PATTERN(new)`, then continue with step 0 of the new pattern.
4. **Step boundary** (`p % sl == 0`, s = p / sl):
   - push `STEP(s)`;
   - apply SQ-26: if step s has its STYLE flag on, `cur = step.phrase`;
   - then the window:
     - **a step with notes and no tie:** close the previous window and open a new one. Its length is
       `sl * (1 + T)`, where T counts the tie steps after it, no further than `last`. Its phrase is `cur`
       *at its opening* (STYLE flags on tie steps inside a window only affect later windows: a
       simplification);
     - **a tie step:** nothing; an orphan tie stays silent;
     - **an empty step:** close the window (it is a rest).
   - while recording with `rec_held`, an empty step entered gets a tie first (SQ-33).
5. **Window audio** (at the shifted time `p*16 + j6_shuffle_offset(p*16, shuffle)`):
   - **phrase on:** push `HELD`; the glue gives H to the seq phrase instance;
   - **phrase off:** NOTE_ON for each note at `vel`, and NOTE_OFF after `j6_gate(len)`, or after `len`
     with `J6_CFG_PLAIN_LEGATO` (SQ-23);
   - closing a phrase window pushes `HELD` with n = 0, unless a new phrase window opens at the same
     moment;
   - negative shuffle needs a look-ahead of up to 22 ticks, as in the phrase engine. Only the 1-4 step
     length puts boundaries on off-beat sixteenths.
6. **Position:** `j6_seq_pos()` reports p until the next tick.

**`j6_seq_play(from)`:** position `from*sl`, `cur` = the pattern phrase (SQ-26 rule 3), then `START(from)`
on the first tick.

**`j6_seq_select`:**

- **stopped:** immediate, pushing `PATTERN` (SQ-28);
- **playing:** queued; selecting the current or the queued pattern again cancels the queue.

### 6.5 Real-time recording

`j6_seq_rec_input(notes, n, tag, phrase)` writes into step `floor(pos/sl)` of the current pattern, or the
nearest step with `J6_CFG_REC_NEAREST` (SQ-32).

- **A chord tag:** replaces the step.
- **No tag:** adds the notes up to 4 and never removes one (SQ-33).
- **Phrase byte with bit 7 set:** also writes the step's phrase (SQ-34).
- **Ties:** `j6_seq_rec_held(1)` makes the following empty steps get ties while a key is held.

Recording overdubs across loops (SQ-36).

### 6.6 Tests (`test_seq.c`)

**Data and storage:**

- init and clear defaults;
- every row of the 6.2 table, including the edge cases (full step, tie target with notes, ties at `last`,
  INSR at step 63, DUPL with last > 32, page raising last);
- encode/decode round trip on a randomised pattern;
- decode of garbage and of a wrong format byte;
- the dirty and saved bits.

**Playback:**

- windows and ties (SQ-22);
- SQ-26 on the Q-13 probe pattern;
- plain note times and gates for each bEAt;
- shuffle ±;
- StEP and N pattern changes;
- the count-in;
- stop leaves everything balanced;
- `q.lost == 0`.

**Recording:** floor, chord replace, add up to 4, ties while held, STYLE ON.

**Clock:**

- exactly 192 ticks in 44100 frames at 1200;
- a tempo change keeps the phase;
- external pulses: spread, catch-up, the 500 ms timeout.

## 7. Module: synth

**Files:**

- `tools/gen_synth_tables.py`
- `firmware/src/j6/j6_synth_tables.h`
- `firmware/src/j6/j6_synth.c` (voices)
- `firmware/src/j6/j6_synth_fx.c` (delay, reverb)
- `firmware/src/j6/j6_synth_sounds.c` (the 64 presets, `j6_sounds[]`)
- `firmware/src/j6/j6_synth_state.h`
- `tests/j6/test_synth.c`

A placeholder that only has to sound "Juno-ish" and stay within the CPU and memory budgets. A JUNO-60
engine port replaces the voices later, behind the same API.

### 7.1 Voice (×4)

- **Oscillators:**
  - a saw and a pulse, from 32-bit phases;
  - polyBLEP at the edges: `t/dt` uses a reciprocal of `inc` computed once per chunk, so there is no
    division per sample;
  - a sub square one octave down, and noise (xorshift);
  - the levels come from the preset (`saw`, `pulse`, `pw`, `sub`, `noise`);
  - `detune` adds a slightly detuned second saw for width.
- **Pitch:** `inc` from a 128-entry table (generated) × `tune_q16` (SD-8: `(tune10*65536 + 2200) / 4400`),
  using an int64 multiply.
- **Filter:** a resonant low-pass, 2- or 4-pole, in int32. It must:
  - be stable at every cutoff × resonance setting (clamp the states, no overflow);
  - reach near self-oscillation at resonance 127;
  - take cutoff from the knob + `env_amt` × filter envelope + key tracking, with coefficients per chunk
    from a table.
- **Envelope:** ADSR. Attack and release come from the knobs, decay and sustain from the preset.
  - attack 0..127 maps to about 1 ms .. 3 s; release to about 5 ms .. 5 s;
  - segments are exponential, with the value computed per chunk and ramped linearly inside it, so there
    is no zipper noise;
  - the filter envelope shares the timing.
- **Voice allocation (SD-5):**
  - the same note again reuses its own voice;
  - otherwise an idle voice;
  - otherwise the quietest releasing voice;
  - otherwise the oldest gated voice, stolen with a 1–2 ms fade to avoid a click.
- **Levels:** velocity scales the level. Keep one voice at vel 127 near -12 dBFS, so 4 voices plus the
  effects stay under ±32767. The final sum is clamped anyway.

### 7.2 Effects (on `fx_mem`, `J6_FX_MEM_LEN` int16)

The synth decides how to divide the pool memory: a delay line of up to 65536 samples, reverb combs and
allpasses (about 12 k samples), and an optional chorus in what is left. The path follows SD-7: voices →
vOL (Q12 gain `vol²·4096/10000`, so 100 is unity) → delay → reverb → out.

- **Delay:**
  - **level:** knob 4;
  - **time:** knob 5. Free (d.Syn off) is about 10 .. 1400 ms from a table. Synced (d.Syn on) uses
    `j6_dsync_ticks(v*12/128)` × `spt` samples; while the result is longer than the line, it is halved;
  - **feedback:** fixed at about 40%, with a darkening one-pole;
  - **time changes:** slew with a fractional read, so they glide rather than click;
  - the structure can follow `firmware/src/dsp/fxbus.c`, rewritten in integers.
- **Reverb:**
  - a Freeverb-style design: 4 combs + 2 allpasses per side, as in `fxbus.c`;
  - REV_TIME 0..127 sets feedback 0.2..0.93 (Q15); damping is fixed;
  - REV_LEVEL 0..127 sets the wet level;
  - it skips work once input and tails are silent (`j6_synth_fx_idle`).

### 7.3 Presets and tables

- **Presets:** `j6_sounds[64]` in 8 banks of 8, with ASCII names of at most 11 characters and their own
  knob values. Suggested banks: pads, strings, brass, keys, bass, leads, plucks, fx. The J-6's real sound
  list is not in the manual; these are FM-1 sounds.
- **Generator:** `tools/gen_synth_tables.py` writes `j6_synth_tables.h` with `j6y_` names: pitch
  increments, cutoff curve, envelope rates, delay times, reverb feedback. It has a `--check` like the
  others.
- **Constants:** every coefficient is an integer computed by the generator or by integer code. No float
  anywhere in the firmware sources.

### 7.4 CPU

`j6_synth_render` runs in the audio interrupt. The budget is 2.9 ms per 256-frame block on the chip, and
the voices and effects should take well under half of it.

- Work per sample: oscillators, filter, envelope ramp, effects. Work per chunk: coefficients, envelope
  segments, BLEP reciprocal.
- No division per sample. 32-bit divisions per chunk are fine.
- The test prints the host time per block. As a rough proxy, stay under 0.1 ms per block with 4 voices and
  both effects on; the chip is roughly 30× slower. The real measurement (`fm1_ticks`) happens on the device
  in the integration step.

### 7.5 Tests (`test_synth.c`)

**Silence and envelopes:**

- idle output is exactly 0;
- a note sounds; after note-off it decays to exactly 0 within the release time.

**Voices:**

- stealing the oldest; same-note retrigger;
- 4 voices at vel 127 with every knob at 127 never exceed ±32767 and never wrap.

**Filter:** a cutoff and resonance sweep stays bounded.

**Pitch:** A4 = 440 Hz ± 0.5% by zero crossings; tUnE 4330 / 4480 move it as expected.

**Delay:** an impulse comes back at the expected time for free and synced times (120 BPM, 1/8 = 11025
samples).

**Reverb and housekeeping:**

- the reverb tail decays to idle;
- panic;
- `n` = 16 / 256 render calls give identical output;
- every preset loads and sounds.

**Listening and timing:**

- writes `build/j6test/synth_demo.wav` (a few presets, chords, delay and reverb), for listening;
- prints the time per block.

## 8. Glue (`j6_engine.c`, the integration step)

This section is here so the module APIs can be checked against their single consumer. It is not part of
the parallel work.

### 8.1 State

The glue state is static inside `j6_engine.c`:

- **Module instances:** `j6_seq_t` (put it in `.pool` if `.bss` gets tight), `j6_clock_t`,
  2 × `j6_phrase_t` (live, seq), 2 × `j6_held_t` (keys, MIDI in), `j6_synth_t`.
- **Settings:** the `j6_global_t` and the panel state (octave, HOLD, CHORD, step-edit selection).
- **Buffers:**
  - the command ring, 256 × 8 B (4x the platform's 64-entry USB-MIDI queue; the last 32 slots only take
    releases, and `j6_engine_key` / `_midi_in` return 0 when full);
  - the MIDI-out ring, 128 × 4 B;
  - the pending sub-tick events, 32;
  - one `j6_evq_t`;
  - the sounding direct notes;
  - a key → note map for the 27 keys, so releases match even if KEY or the octave changed while a key was
    held.
- **Budget:** `J6_RAM_ENGINE_MAX` = 8 KB.

### 8.2 Notes in

**A key goes down** (`j6_engine_key(k, 1)`):

1. **In step edit:**
   - CHORD on: `j6_chord_notes` then `j6_step_key_chord`;
   - CHORD off: `j6_key_note` then `j6_step_key_note`;
   - either way, the note or chord sounds while the key is held, as a preview.
2. **Otherwise:**
   - feed the keys tracker (a note id, or a chord id = key + 64);
   - on a change, take the effective set (chord ids expanded with `j6_chord_notes`), merged with the MIDI
     tracker (at most 4);
   - **STYLE on:** `j6_phrase_set_held(live)`. If the set was empty before and the sequencer is stopped,
     `live_pt = 0`;
   - **STYLE off:** compare with the direct notes sounding and push NOTE_ON / NOTE_OFF (src `KEY` or
     `CHORD`, vel vELo).
3. **While recording:** `j6_seq_rec_input` with the notes, the chord tag, and the pattern phrase when
   STYLE is on. `j6_seq_rec_held` follows the physical keys.

**MIDI in on CH** feeds the MIDI tracker: chords are never expanded (CH-9), and velocity is used for direct
notes only.

- **CC64:** latches both trackers (HD-3).
- **CC120:** `j6_synth_panic` plus both phrases stopped. **CC121:** latch off. **CC123:** clear the MIDI
  tracker.
- **Program Change on Pc.Ch** with rxPc on: `j6_seq_select` (MI-6).
- **0xF8, 0xFA, 0xFC:** clock and transport, as SYnC allows (MI-8). Continue is ignored.
- **0xFE:** arms the 300 ms Active Sensing timeout (MI-5).

**Synth notes.** Every NOTE_ON / NOTE_OFF reaching the synth gets `+ trAn` (KB-5). Notes outside 0..127
are dropped.

### 8.3 MIDI out

All MIDI out goes on CH with velocity vELo, or A.vLo for accents, and is never transposed by trAn (MI-2).

| src | Sent? |
|---|---|
| `KEY` | yes |
| `CHORD` | only with `J6_CFG_CHORD_MIDI_OUT` (preflight: no) |
| `MIDI` | no (no echo; thru is TRS-only, and the FM-1 has only USB) |
| `SEQ`, `PHRASE_LIVE`, `PHRASE_SEQ` | yes |

The rest of the traffic:

- **Clock (`J6_CFG_CLOCK_OUT_STOPPED`):** 0xF8 every 4 ticks while the clock is internal, also when stopped.
- **Transport:** Start on `START`, Stop on `STOP`.
- **Program Change:** on `PATTERN` when txPc is on, on Pc.Ch with the value = pattern index (MI-6).
- **Active Sensing:** every 250 ms. **All Sound Off:** on panic.

The ring is drained by the main loop (`j6_engine_midi_out` then `midi_send`), because `midi_send` is not
allowed in the interrupt.

### 8.4 Storage

**Patterns.** Each pattern gets its own 4 KiB flash sector, so a WRITE never has to read, modify and write
a shared sector. The sector holds a magic, the length, a CRC32 and then the 544 bytes.

- **Location:** 64 sectors in the family data range, for example 0x97000..0xD6FFF. That needs
  `FL_RANGE_OK` widened in `platform.c` (API.md). The range lies below the OTA staging area at 0xE0000.
- **Globals:** go through `settings_load` / `settings_save` (32 bytes), so they need no new sector.
- **Boot:** decode each sector. A bad or missing sector gives `j6_pattern_init`, and every pattern is
  marked saved.
- **WRITE (PS-1):** mute, then encode and write the current pattern (Ptn) or every dirty pattern (ALL),
  calling `platform_poll()` between sectors. Each erase takes about 50 ms with the interrupts off, so the
  user hears the expected short gap.
- **PATTERN_COPY (SQ-45):** saves the destination from `j6_engine_poll()`.
- **Globals timing (PS-3):** the globals are saved 2 s after the last change, once `voices == 0` and
  `fx_idle`; also at once by `j6_engine_save_globals()` (the app calls it when a menu is left) and by
  WRITE, both muted.
- **Factory reset:** everything back to `init` and written out.

### 8.5 Firmware and host integration

- **The app:** a new `firmware/src/j6_app.c`.
  - Sets `APP_NAME "J-6"` and a new `FELUCCA_ID` of the form `FM-1_9NN` (MAKE-IT-YOURS.md).
  - Defines `J6_` overrides if any.
  - Includes `platform.c`, then each `j6/j6_*.c`, then the app UI.
  - Declares `static int16_t j6_fx_mem[J6_FX_MEM_LEN] __attribute__((section(".pool")));` and calls
    `j6_engine_render(out, n)` from `app_audio`.
  - The main loop polls MIDI in and out, the panel, the screen and `j6_engine_poll()`.
- **Builds:**
  - `build.py:154-162`: compile `j6_app.c` instead of `morpheus_app.c`, or as a second target.
  - `tests/run_host.sh`: add a call to `tests/j6/run_tests.sh` so CI runs the engine tests.
- **Gaps to fill:**
  - **Host simulator:** `tests/host.c` needs real `fm1_enc_take`, `fm1_input_note_edges` and LED stubs
    before UI tests can run (platform study, gotcha 4).
  - **Ring indices:** the engine's command and MIDI rings need a compiler barrier between writing an entry
    and publishing the index. The app supplies the platform's `RING_PUBLISH()` to the glue, for example as
    `#define J6E_PUBLISH() RING_PUBLISH()` before including it; on the host it is a no-op.
- **Panel mapping:** the mapping of the FM-1 panel to the J-6's is the UI step's job. The FM-1 has 4 knobs
  (FILTER, ENVELOPE, DELAY, REVERB; a held button selects the SHIFT layer), 3 more encoders (TEMPO/VALUE,
  STYLE, VARIATION) and 14 buttons. The J-6's 8 step buttons have no physical counterpart: use the screen
  plus keys or encoders.

## 9. Coding rules for module authors

Every rule below is checked by `run_tests.sh`.

**The language.**

- C99, only `"j6*.h"`, `<stdint.h>` and `<stddef.h>`.
- No float, no double, no float literal. Host tests may use float; engine code may not.
- No 64-bit division or modulo; `int64_t` multiply is fine.
  - Division by a constant counts: the ARM check compiles at -O0, so `x / 3` on an `int64_t` is caught.
  - Shift instead, or reduce to 32 bits first.
- Divisions by zero do not trap on the chip; they return garbage. Guard every divisor (RULES.md).

**Fixed point.** Q15 for levels and samples (`±32767` in `int32_t`), Q12 for gains, 32-bit wrapping phases
for oscillators, Q16 for ratios.

- Left-shifting a negative value is undefined in C99: multiply instead (`x * 256`), or shift the unsigned
  value.
- Right shifts of negative values are arithmetic on both compilers, which is fine.
- Saturate before narrowing. Never build a constant in the chip ROM range 0xFFC00000..0xFFCFFFFF
  (RULES.md: `-32767 << 7` is one).

**Memory.**

- No malloc and no VLA. All state goes in the instance struct, so two instances and every test are
  independent.
- No mutable static or global variable outside the glue. const tables are fine, and they live in flash.

**Single translation unit.** Use the prefixes of section 1.3 for every file-scope name and macro. Name your
`J6_STATIC_ASSERT`s after the module (`phrase_fits_budget`), since the typedef names must be unique too.

**The audio path.** No flash and no screen. The engine never touches either; that is the glue's job.

## 10. Memory budget

| Item | Where | Bytes | Budget (`j6.h`) |
|---|---|---|---|
| `j6_seq_t` (64 × 536 B working patterns + state) | .bss or .pool | 34504 (first layout) | 36864 |
| `j6_clock_t` | .bss | 60 | 128 |
| 2 × `j6_phrase_t` | .bss | 2 × 232 | 2 × 768 |
| 2 × `j6_held_t` | .bss | 2 × 26 | 2 × 64 |
| `j6_synth_t` | .bss | 608 | 4096 |
| glue state | .bss | about 2.5 K | 8192 |
| **engine RAM** | | **about 38 K** | **61440 (< 60 KB)** |
| delay + reverb memory `J6_FX_MEM_LEN` | .pool | 163840 | < 200 KB |
| UI canvas (`CV_MAX` 240×128) | .pool | 61440 | |
| pool total, with `j6_seq_t` moved there | .pool | about 260 K | 336 K |
| chord tables + names | flash | about 15 K | |
| phrase tables (192 lists) | flash | about 10 K placeholder; ≤ 74 K captured | |
| synth tables + 64 presets | flash | about 5 K | |

`run_tests.sh` prints the real sizes of the state structs (the "ram" step) and fails if the modules plus
the glue budget exceed `J6_RAM_TOTAL_MAX`. Each state header also asserts its own budget at compile time.

## 11. Tests

```sh
tests/j6/run_tests.sh                     # everything (from any directory)
J6_ISOLATED=1 tests/j6/run_tests.sh seq   # one module, linked only with its own sources
J6_SANITIZE=1 tests/j6/run_tests.sh       # with AddressSanitizer + UBSan
```

`run_tests.sh` runs these steps, in order:

1. **lint:** `tests/j6/lint_engine.py`, the rules of section 9.
2. **arm:** each engine `.c` file is compiled for `armv7-none-eabi -mfloat-abi=soft -O0`. Any float helper
   (`__aeabi_f*`, `__aeabi_d*`, ...) or 64-bit division (`__aeabi_ldivmod`) among the undefined symbols
   fails. This needs a clang that targets ARM: Apple's does; on Linux install clang or set `J6_ARM_CC`.
3. **one-tu:** all engine files are compiled as one translation unit, as the firmware builds them. Full
   mode only.
4. **tables:** `--check` on each table generator that exists.
5. **ram:** the struct sizes against the budgets. Full mode only.
6. **tests:** each `tests/j6/test_<name>.c` is built with `cc -std=c99 -O2 -g -Wall -Wextra -Werror
   -Wdouble-promotion -Wfloat-equal -Wvla` and every engine `.c` (or only `j6_<name>*.c` in isolated mode),
   then run from the repository root. A non-zero exit status fails.

The exit status is 1 on any failure. A missing `python3` or ARM clang is reported as a skip, and
`J6_STRICT=1` turns skips into failures.

**Writing a test.** `tests/j6/j6_test.h` provides:

- **checks:** `CHECK`, `CHECK_EQ`, `CHECK_RANGE`, `CHECK_MEM`, `CHECK_STR`;
- **running:** `RUN(fn)` and `j6t_report(name)` as main's return value;
- **events:** `j6t_evq_dump/count/find`;
- **timelines:** `j6t_log_*`, an event recorder with `j6t_log_balanced`;
- **audio:** `j6t_wav_write`, `j6t_peak` and `j6t_rms`.

Keep big instances `static`. Files a test writes go in `build/j6test/`.

`tests/j6/test_header.c` checks the inline helpers in `j6.h` against the spec. It is the only test that
exists before the modules.

`tests/j6/test_engine.c` tests the glue end to end with every module linked (`engine` always links all
engine sources, also with `J6_ISOLATED=1`). It observes the engine through `j6_engine_set_tap()` (j6.h
section 10: every event when it takes effect, with its tick, frame and synth note) and
`j6_engine_midi_out()`. In full mode `run_tests.sh` also runs `tools/j6_render.sh`, which builds the host
demo `tools/j6_render.c` (chord set 1; styles 6 and 9 from the captured tables, then style 1 arpeggios, at
120 BPM) and writes `build/j6/demo.wav`. `J6_SANITIZE=1` uses the undefined-behaviour sanitizer only on
macOS, where AddressSanitizer hangs at startup (`J6_SANITIZE=asan` forces it).

## 12. When the capture answers a question

Most `[ASSUMED]` rules live in one place each, so a capture result is a one-line change.

| Question | Switch / place | Default |
|---|---|---|
| Q-1 arp "2" = 2-octave range | `J6_CFG_ARP_RANGE2` (phrase) | 1 |
| Q-4 gate length | `j6_gate()` in `j6.h` (phrase, seq) | `L - min(L/5, 10)` |
| Q-6 style 5 v7–12 loop | `J6_CFG_S5_RUN_ON` (phrase) | 1 (run on) |
| Q-8 styles 6–9 | `tools/gen_phrase_tables.py --capture` | placeholders |
| Q-11 plain step length | `J6_CFG_PLAIN_LEGATO` (seq) | 0 (gate) |
| Q-13 phrase at loop wrap | `J6_CFG_WRAP_KEEPS_PHRASE` (seq) | 1 (keep) |
| Q-15 shuffle amount | `j6_shuffle_offset()` in `j6.h` | shuffle % of a sixteenth |
| Q-17 record to floor / nearest step | `J6_CFG_REC_NEAREST` (seq) | 0 (floor) |
| Q-23 chord notes on MIDI out | `J6_CFG_CHORD_MIDI_OUT` (glue) | 0 |
| Q-25 clock while stopped | `J6_CFG_CLOCK_OUT_STOPPED` (glue) | 1 |
| Q-22 real chord notes | `data/chordsets.json` + `tools/gen_tables.py` | manual data |

The phrase capture also records styles 1–5 at 120 BPM (`capture_phrases.py` covers all 9 × 12). Once it
is complete, a host test can compare the engine's styles 1–5 against it note for note. That test would be
a later addition to `test_phrase.c`, reading `capture/raw/phrases.jsonl` without changing it.
