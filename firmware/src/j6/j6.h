/* SPDX-License-Identifier: GPL-3.0-only */
/* j6.h - the J-6 engine for the FM-1: the one shared public header.
 *
 * Everything that crosses a module boundary is declared here: constants, the time base, the event
 * queue, the pattern / step data model, the phrase table format, the sound preset format, the global
 * settings, and the API of every module plus the engine glue. Design: docs/J6_ENGINE_DESIGN.md.
 * Behaviour: docs/J6_SPEC.md (rule IDs such as SQ-26 are cited next to the code that implements them).
 *
 * Modules (each one implemented and tested on its own; none calls another module's functions, only the
 * glue j6_engine.c does):
 *   chord   j6_chord.c                                  chord sets, KEY, octave, key -> notes
 *   phrase  j6_phrase.c, j6_phrase_held.c               HOLD latch, phrase styles 1-9, accents
 *   seq     j6_seq.c, j6_seq_pattern.c, j6_seq_clock.c  patterns, steps, playback, recording, clock
 *   synth   j6_synth.c, j6_synth_fx.c, j6_synth_sounds.c  4-voice synth, delay, reverb, 64 sounds
 *   glue    j6_engine.c                                 routing, MIDI, commands, storage (written last)
 *
 * The private state of a module (struct j6_seq_s ...) is defined in that module's own header
 * j6_<module>_state.h, which only that module, its test and the glue include. This header only names
 * the types, so a module can change its private layout without touching anyone else.
 *
 * Rules for all engine code (checked by tests/j6/run_tests.sh):
 *   - C99, platform independent: include only "j6*.h", <stdint.h> and <stddef.h>.
 *   - No float, no double, no float literal. No 64-bit division or modulo (int64_t multiply is fine).
 *   - No malloc, no VLA, no static mutable state outside the glue: all state lives in the instance
 *     structs, so two instances (the live and the sequencer phrase) and every test are independent.
 *     const tables are fine (they live in flash).
 *   - The firmware builds every engine .c file into ONE translation unit after platform.c. So every
 *     file-scope name (static functions, static tables, macros) carries the module prefix:
 *     chord j6c_ / J6C_, phrase j6p_ / J6P_, seq j6s_ / J6S_, synth j6y_ / J6Y_, glue j6e_ / J6E_.
 *     Public names use j6_ / J6_. Never use a platform macro name (FS, MQ, RGB, KEY_LED ...) as an
 *     identifier.
 *
 * Index conventions: patterns 0..63 (= 1-1 .. 8-8, bank = idx / 8), steps 0..63, pages 0..7, sounds
 * 0..63, chord sets 0..99 (the J-6 shows 1..100), chord index 0..11 (C .. B). Phrase styles 1..9 and
 * variations 1..12 are 1-based, because they are knob positions on the J-6.
 */
#ifndef J6_H
#define J6_H

#include <stdint.h>
#include <stddef.h>

#define J6_API_VERSION 1

#define J6_STATIC_ASSERT(name, cond) typedef char j6_static_assert_##name[(cond) ? 1 : -1]
#define J6_UNUSED(x) ((void)(x))

/* ======================================================================================================
 * 1. Behaviour switches for the [ASSUMED] rules the J-6 capture can still overturn (docs/J6_SPEC.md 11).
 *    Each module implements the switches it names. Override with -D, never edit the default silently.
 * ==================================================================================================== */

#ifndef J6_CFG_ARP_RANGE2
#define J6_CFG_ARP_RANGE2 1          /* phrase PH-12 / Q-1: 1 = "2" suffix is a 2-octave range;
                                        0 = "2" repeats the turnaround notes (C E G G E C) */
#endif
#ifndef J6_CFG_S5_RUN_ON
#define J6_CFG_S5_RUN_ON 1           /* phrase PH-16 / Q-6: 1 = style 5 v7-12 (72-tick loop) runs across
                                        bar lines (pt % 72); 0 = restarts every bar (pt % 384 % 72) */
#endif
#ifndef J6_CFG_WRAP_KEEPS_PHRASE
#define J6_CFG_WRAP_KEEPS_PHRASE 1   /* seq SQ-26 / Q-13: 1 = at the loop wrap the current step phrase
                                        carries over; 0 = it resets to the pattern phrase */
#endif
#ifndef J6_CFG_REC_NEAREST
#define J6_CFG_REC_NEAREST 0         /* seq SQ-32 / Q-17: 0 = a recorded note goes to the step playing
                                        (floor); 1 = to the nearest step start */
#endif
#ifndef J6_CFG_PLAIN_LEGATO
#define J6_CFG_PLAIN_LEGATO 0        /* seq SQ-23 / Q-11: 0 = a STYLE OFF window sounds j6_gate(len);
                                        1 = it sounds the full window */
#endif
#ifndef J6_CFG_CHORD_MIDI_OUT
#define J6_CFG_CHORD_MIDI_OUT 0      /* glue MI-2 / Q-23: 1 = block chords played on the own keys in
                                        CHORD mode are sent on MIDI out (the preflight saw none) */
#endif
#ifndef J6_CFG_CLOCK_OUT_STOPPED
#define J6_CFG_CLOCK_OUT_STOPPED 1   /* glue MI-7 / Q-25: 1 = MIDI clock is sent while stopped */
#endif

/* ======================================================================================================
 * 2. Constants
 * ==================================================================================================== */

/* audio */
#define J6_FS 44100u                 /* sample rate (do not use the platform's FS: master.c redefines it) */
#define J6_BLOCK 256u                /* frames per app_audio() call */
#define J6_CR 16u                    /* control-rate chunk: the glue runs time and events every 16 frames,
                                        j6_synth_render() is always called with a multiple of it */
#define J6_Q15_ONE 32768
#define J6_OUT_MAX 32767             /* engine output is Q15 in int32_t, interleaved L R, +-32767 */

/* time base (spec C-2 .. C-5) */
#define J6_PPQN 96u                  /* ticks per quarter note (one beat) */
#define J6_SUB 16u                   /* sub-ticks per tick: only shuffle and strum offsets use them */
#define J6_SUB_SHIFT 4u
#define J6_TICKS_16TH 24u
#define J6_TICKS_BAR 384u            /* 4/4 only (C-4) */
#define J6_MIDI_CLOCK_TICKS 4u       /* one MIDI clock (24 PPQN) = 4 ticks */
#define J6_SPT_Q12_K 1128960000u     /* samples per tick in Q12 = J6_SPT_Q12_K / tempo10 (C-5) */
#define J6_TEMPO10_MIN 400u          /* 40.0 BPM  [ASSUMED SQ-6] */
#define J6_TEMPO10_MAX 3000u         /* 300.0 BPM [ASSUMED SQ-6] */
#define J6_TEMPO10_DEF 1200u
#define J6_EXT_CLOCK_TIMEOUT_MS 500u /* SYnC AUTO falls back to the internal clock (MI-8) */

/* sizes */
#define J6_PATTERNS 64u
#define J6_BANKS 8u
#define J6_STEPS 64u
#define J6_PAGE_STEPS 8u
#define J6_PAGES 8u
#define J6_STEP_NOTES 4u             /* notes per step (SQ-3) */
#define J6_VOICES 4u                 /* SD-5 */
#define J6_HELD_MAX 4u               /* notes in a phrase's held set H (PH-4) and in a latch (HD-2) */
#define J6_HELD_DOWN_MAX 16u         /* physical keys / MIDI notes a held tracker remembers as down */
#define J6_SOUNDS 64u
#define J6_KNOBS 8u
#define J6_CHORD_SETS 100u
#define J6_CHORDS 12u                /* chords per set, one per key C .. B */
#define J6_CHORD_NOTES 4u            /* notes per chord, at most */
#define J6_STYLES 9u
#define J6_VARS 12u
#define J6_TABLE_STYLE_FIRST 6u      /* styles 6..9 come from the phrase tables (PH-17) */
#define J6_TABLE_STYLES 4u
#define J6_PLISTS (J6_TABLE_STYLES * J6_VARS * J6_HELD_MAX)   /* 192 event lists */

/* markers */
#define J6_NO_NOTE 0xFFu
#define J6_NO_CHORD 0xFFu
#define J6_KNOB_UNSET 0xFFu          /* pattern knob slot not moved: the sound's own value applies */
#define J6_NONE 0xFFu                /* no pattern / no step (queued pattern, selected step ...) */

/* keyboard. A "key code" is the key's distance in semitones from the keyboard's low C, before octave
 * and KEY: the J-6 has keys 0..12 (C .. C). The FM-1 has 27 keys F3..G5, fm1_in.notes bit b is key code
 * b - 7 (bit 7 = C4 = 60 at octave 0). Chord mode generalises CH-5 to every key code: the chord is
 * index key mod 12, one octave up per 12 keys (key 12 = the C chord one octave up). */
#define J6_KEY_MIN (-7)
#define J6_KEY_MAX 19
#define J6_KEY_COUNT 27
#define J6_KEY_BASE_NOTE 60          /* key code 0 at octave 0, KEY 0 (KB-1) [ASSUMED] */
#define J6_OCT_MIN (-3)              /* KB-3 [ASSUMED] */
#define J6_OCT_MAX 3
#define J6_KEYTR_MIN (-12)           /* KEY transpose, KB-4 [ASSUMED] */
#define J6_KEYTR_MAX 12

/* defaults (spec section 6) */
#define J6_VELO_DEF 100u             /* vELo */
#define J6_AVEL_DEF 127u             /* A.vLo */
#define J6_VOL_DEF 100u              /* vOL: 100 = unity gain */
#define J6_LAST_DEF 8u
#define J6_SHUFFLE_MIN (-90)
#define J6_SHUFFLE_MAX 90
#define J6_TUNE10_MIN 4330u
#define J6_TUNE10_MAX 4480u
#define J6_TUNE10_DEF 4400u

/* delay and reverb memory: one int16_t array the glue puts in the pool (.pool section) and hands to
 * j6_synth_init(). 65536 samples of delay = 1.486 s at 44.1 kHz; the rest is for the reverb. */
#define J6_FX_DELAY_LEN 65536u
#define J6_FX_REVERB_LEN 16384u
#define J6_FX_MEM_LEN (J6_FX_DELAY_LEN + J6_FX_REVERB_LEN)    /* int16_t count: 163840 bytes */

/* RAM budgets, bytes (docs/J6_ENGINE_DESIGN.md 8). Each state header asserts its struct fits. */
#define J6_RAM_SEQ_MAX 36864u        /* j6_seq_t: 64 working patterns (34304) + playback state */
#define J6_RAM_CLOCK_MAX 128u        /* j6_clock_t */
#define J6_RAM_PHRASE_MAX 768u       /* j6_phrase_t, per instance (the glue has two) */
#define J6_RAM_HELD_MAX 64u          /* j6_held_t, per instance (the glue has two) */
#define J6_RAM_SYNTH_MAX 4096u       /* j6_synth_t, without the J6_FX_MEM_LEN pool array */
#define J6_RAM_ENGINE_MAX 8192u      /* the glue's own state: rings, settings, scheduler */
#define J6_RAM_TOTAL_MAX 61440u      /* everything above together: < 60 KB */

/* ======================================================================================================
 * 3. Shared helpers (header-only, so every module computes them the same way)
 * ==================================================================================================== */

static inline int32_t j6_clamp(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* C-5: samples per tick in Q12 at tempo10 (tenths of a BPM). 1200 -> 940800 (229.6875 samples). */
static inline uint32_t j6_spt_q12(uint32_t tempo10)
{
    if (tempo10 < J6_TEMPO10_MIN) tempo10 = J6_TEMPO10_MIN;
    if (tempo10 > J6_TEMPO10_MAX) tempo10 = J6_TEMPO10_MAX;
    return J6_SPT_Q12_K / tempo10;
}

/* frames from the start of a tick to sub-tick `sub` (0..15): (sub * spt_q12 / 16) >> 12 */
static inline uint32_t j6_sub_frames(uint32_t sub, uint32_t spt_q12)
{
    return (sub * spt_q12) >> (12u + J6_SUB_SHIFT);
}

/* PH-7 / SQ-23 gate: a note of nominal length L ticks sounds g(L) = L - min(L/5, 10) ticks.
 * 16->13, 24->20, 32->26, 48->39, 96->86, 384->374. [ASSUMED - the capture will tell (Q-4)] */
static inline uint32_t j6_gate(uint32_t len_ticks)
{
    uint32_t cut = len_ticks / 5u;
    return len_ticks - (cut > 10u ? 10u : cut);
}

/* SQ-7 shuffle: offset in sub-ticks (signed) for an event whose nominal onset is `onset_sub` sub-ticks
 * from the phrase / pattern origin. Events on an off-beat sixteenth (tick % 48 == 24), or up to 6 ticks
 * after one (the later notes of a strum), move by shuffle % of a sixteenth. Triplet grids never land
 * there. Note-offs move with their note-on. */
static inline int32_t j6_shuffle_offset(uint32_t onset_sub, int32_t shuffle)
{
    uint32_t m = onset_sub % (48u * J6_SUB);
    if (m >= 24u * J6_SUB && m < 30u * J6_SUB)
        return shuffle * (int32_t)(J6_TICKS_16TH * J6_SUB) / 100;
    return 0;
}

/* SQ-4 step length. bEAt values: */
enum {
    J6_BEAT_1_4 = 0, /* 1/4 beat = 24 ticks (display 1_4) */
    J6_BEAT_1_3 = 1, /* 32 ticks */
    J6_BEAT_1_2 = 2, /* 48 ticks */
    J6_BEAT_1 = 3,   /* J6_BEAT_1 + (N-1): N beats = 96 * N ticks, N = 1..8 */
    J6_BEAT_4 = 6,   /* default [ASSUMED SQ-4] */
    J6_BEAT_8 = 10,
    J6_BEAT_COUNT = 11
};
static inline uint32_t j6_beat_ticks(uint32_t beat)
{
    if (beat == J6_BEAT_1_4) return 24u;
    if (beat == J6_BEAT_1_3) return 32u;
    if (beat == J6_BEAT_1_2) return 48u;
    if (beat > J6_BEAT_8) beat = J6_BEAT_8;
    return J6_PPQN * (beat - J6_BEAT_1 + 1u);
}

/* S.cLk index 0..7 -> sync pulses per beat (1, 2, 3, 4, 6, 8, 12, 24); one pulse = 96 / ppb ticks */
#define J6_SCLK_COUNT 8u
#define J6_SCLK_DEF 1u
static inline uint32_t j6_sclk_ppb(uint32_t idx)
{
    static const uint8_t ppb[J6_SCLK_COUNT] = {1, 2, 3, 4, 6, 8, 12, 24};
    return ppb[idx < J6_SCLK_COUNT ? idx : J6_SCLK_DEF];
}

/* A phrase setting packed in one byte: bit 7 = STYLE on; bits 0..6 = (style-1)*12 + (var-1), 0..107.
 * Used for the pattern phrase and for each step's phrase (PH-20, SQ-15, SQ-26). 0 = off, style 1 var 1. */
#define J6_PHR_ON 0x80u
#define J6_PHR_DEF 0x00u
static inline uint8_t j6_phr_pack(uint32_t on, uint32_t style, uint32_t var)
{
    if (style < 1u) style = 1u;
    if (style > J6_STYLES) style = J6_STYLES;
    if (var < 1u) var = 1u;
    if (var > J6_VARS) var = J6_VARS;
    return (uint8_t)((on ? J6_PHR_ON : 0u) | ((style - 1u) * J6_VARS + (var - 1u)));
}
static inline uint32_t j6_phr_on(uint8_t p) { return (p & J6_PHR_ON) ? 1u : 0u; }
static inline uint32_t j6_phr_style(uint8_t p)
{
    uint32_t c = p & 0x7Fu;
    return (c < J6_STYLES * J6_VARS ? c : 0u) / J6_VARS + 1u;
}
static inline uint32_t j6_phr_var(uint8_t p)
{
    uint32_t c = p & 0x7Fu;
    return (c < J6_STYLES * J6_VARS ? c : 0u) % J6_VARS + 1u;
}

/* phrase categories (PH-3) */
enum { J6_CAT_ARP = 1, J6_CAT_BEAT = 2, J6_CAT_PHRASE = 3 };
static inline uint32_t j6_style_cat(uint32_t style)
{
    return style <= 2u ? J6_CAT_ARP : style <= 5u ? J6_CAT_BEAT : J6_CAT_PHRASE;
}

/* Key codes and chord tags. A chord tag (step field `chord`, SQ-3) records which chord key entered a
 * step's notes: bits 0..3 chord index 0..11, bits 4..7 chord octave + 8, where chord octave =
 * octave shift + floor(key / 12). J6_NO_CHORD = none. */
static inline int32_t j6_key_floor12(int32_t key) { return (key + 120) / 12 - 10; }   /* floor(key/12), key >= -120 */
static inline uint32_t j6_key_chord_idx(int32_t key) { return (uint32_t)(key - 12 * j6_key_floor12(key)); }
static inline uint8_t j6_chord_tag(uint32_t idx, int32_t chord_oct)
{
    return (uint8_t)((idx & 0x0Fu) | ((uint32_t)j6_clamp(chord_oct + 8, 0, 15) << 4));
}
static inline uint32_t j6_chord_tag_idx(uint8_t tag) { return tag & 0x0Fu; }
static inline int32_t j6_chord_tag_oct(uint8_t tag) { return (int32_t)(tag >> 4) - 8; }
/* the key code that shows (lights) a chord tag at keyboard octave `oct` (may be off the keyboard) */
static inline int32_t j6_chord_tag_key(uint8_t tag, int32_t oct)
{
    return (int32_t)j6_chord_tag_idx(tag) + 12 * (j6_chord_tag_oct(tag) - oct);
}

/* ======================================================================================================
 * 4. Events: what the phrase and sequencer modules produce for one tick
 *
 * A producer's tick function appends the events of that tick to a j6_evq_t; the glue drains it right
 * after the call. `sub` places an event inside the tick (0 = at the tick, 15 = 15/16 into it); the glue
 * fires it j6_sub_frames(sub) frames after the tick (at J6_CR resolution). Rules for producers:
 *   - events of one tick only; anything later stays in the producer's own state until its tick;
 *   - for the same note at the same sub, push NOTE_OFF before NOTE_ON (so a retrigger works);
 *   - every NOTE_ON gets exactly one matching NOTE_OFF later (also on stop / set changes / re-init);
 *   - functions that are not tick functions (stop, set_held, select ...) push with sub = 0: "now".
 * ==================================================================================================== */

enum {
    J6_EV_NONE = 0,
    J6_EV_NOTE_ON,    /* note, vel 1..127 */
    J6_EV_NOTE_OFF,   /* note (vel 0) */
    J6_EV_HELD,       /* seq: the sequencer's phrase input changed: the glue reads j6_seq_window() and
                         gives its notes (n may be 0) and phrase to the sequencer's phrase instance.
                         The glue applies it at once (ignoring sub), before the phrase ticks of this tick */
    J6_EV_STEP,       /* seq: the playhead entered step arg (nominal boundary, for LEDs / display) */
    J6_EV_LOOP,       /* seq: the pattern wrapped to its first step */
    J6_EV_PATTERN,    /* seq: pattern arg is now the current pattern (immediate or queued change);
                         the glue loads its tempo, sound, knobs and settings */
    J6_EV_START,      /* seq: playback started at step arg (after any count-in): MIDI Start */
    J6_EV_STOP,       /* seq: playback stopped: MIDI Stop */
    J6_EV_COUNTIN,    /* seq: a count-in beat began, arg = beats left including this one */
    J6_EV_REC         /* seq: recording switched, arg = 1 on / 0 off */
};

/* producer tags (which part of the engine played a note); the glue's MIDI-out rules depend on them */
enum {
    J6_SRC_KEY = 0,       /* own keys, CHORD off, STYLE off: direct notes */
    J6_SRC_CHORD,         /* own keys, CHORD on, STYLE off: block chords */
    J6_SRC_MIDI,          /* MIDI-in notes played directly */
    J6_SRC_SEQ,           /* sequencer, STYLE OFF windows (SQ-23) */
    J6_SRC_PHRASE_LIVE,   /* the live phrase instance (keys / chords / MIDI in, PH-4 1-3) */
    J6_SRC_PHRASE_SEQ,    /* the sequencer's phrase instance (PH-4 4, SQ-24) */
    J6_SRC_COUNT
};

typedef struct {
    uint8_t type;   /* J6_EV_* */
    uint8_t src;    /* J6_SRC_* (the producer's tag) */
    uint8_t sub;    /* 0..15 */
    uint8_t note;   /* NOTE_ON / NOTE_OFF: MIDI note 0..127 */
    uint8_t vel;    /* NOTE_ON: 1..127, else 0 */
    uint8_t arg;    /* STEP / START: step; PATTERN: pattern; COUNTIN: beats left; REC: 0/1 */
} j6_event_t;       /* 6 bytes */

#define J6_EVQ_CAP 64u
typedef struct {
    j6_event_t ev[J6_EVQ_CAP];
    uint8_t n;      /* events in ev[0 .. n-1] */
    uint8_t lost;   /* pushes refused because the queue was full (a bug: size producers below the cap) */
} j6_evq_t;

static inline void j6_evq_clear(j6_evq_t *q) { q->n = 0; q->lost = 0; }
/* 0 = queued, -1 = full (counted in q->lost) */
static inline int j6_evq_push(j6_evq_t *q, uint32_t type, uint32_t src, uint32_t sub, uint32_t note,
                              uint32_t vel, uint32_t arg)
{
    j6_event_t *e;
    if (q->n >= J6_EVQ_CAP) {
        if (q->lost < 255u) q->lost++;
        return -1;
    }
    e = &q->ev[q->n++];
    e->type = (uint8_t)type;
    e->src = (uint8_t)src;
    e->sub = (uint8_t)(sub > J6_SUB - 1u ? J6_SUB - 1u : sub);
    e->note = (uint8_t)note;
    e->vel = (uint8_t)vel;
    e->arg = (uint8_t)arg;
    return 0;
}

/* ======================================================================================================
 * 5. Sequencer data: steps and patterns (SQ-1 .. SQ-5, section 6.1). Plain bytes: the same layout on the
 *    host and the chip, no packing needed. The flash format is separate (j6_pattern_encode).
 * ==================================================================================================== */

/* step flags */
#define J6_STF_TIE 0x01u             /* the step continues the previous window (SQ-17, SQ-22) */
#define J6_STF_CMASK_SHIFT 4u        /* bits 4..7: which of note[0..3] belong to the chord tag (SQ-13) */
#define J6_STF_CMASK (0x0Fu << J6_STF_CMASK_SHIFT)

typedef struct {
    uint8_t note[J6_STEP_NOTES];     /* ascending, distinct; the empty slots are J6_NO_NOTE at the end.
                                        Absolute MIDI notes, already shifted by KEY and octave at entry */
    uint8_t flags;                   /* J6_STF_TIE | chord mask << J6_STF_CMASK_SHIFT; bits 1..3 zero */
    uint8_t phrase;                  /* step phrase (j6_phr_pack); bit 7 = the step's STYLE flag (SQ-15) */
    uint8_t chord;                   /* chord tag of the key that entered the notes, or J6_NO_CHORD */
    uint8_t spare;                   /* 0 */
} j6_step_t;                         /* 8 bytes. Empty step: notes all J6_NO_NOTE, flags 0, phrase 0,
                                        chord J6_NO_CHORD, spare 0 (j6_step_clear). A step with no notes
                                        and no tie is a rest. */

/* P.chg values (SQ-27) */
#define J6_PCHG_STEP 0u              /* switch at the next step boundary; 1..8 = every N beats */

typedef struct {
    uint16_t tempo10;                /* J6_TEMPO10_MIN..MAX (SQ-6) */
    uint8_t last;                    /* last step 1..64 (SQ-5) */
    uint8_t beat;                    /* bEAt, J6_BEAT_* (SQ-4) */
    int8_t shuffle;                  /* -90..90 (SQ-7) [ASSUMED per pattern] */
    uint8_t phrase;                  /* pattern phrase (j6_phr_pack), bit 7 = STYLE on (PH-20) */
    uint8_t sound;                   /* 0..63 (SD-2) */
    uint8_t vol;                     /* vOL 0..127 */
    int8_t tran;                     /* trAn -12..12 (KB-5) */
    uint8_t dsync;                   /* d.Syn 0/1 */
    uint8_t avel;                    /* A.vLo 1..127 */
    uint8_t pchg;                    /* P.chg: J6_PCHG_STEP or 1..8 */
    uint8_t sclk;                    /* S.cLk index 0..7 (j6_sclk_ppb) */
    uint8_t knob[J6_KNOBS];          /* knob values 0..127 in J6_KNOB_* order, J6_KNOB_UNSET = sound's own */
    uint8_t reserved[3];             /* 0 */
    j6_step_t step[J6_STEPS];
} j6_pattern_t;                      /* 536 bytes */

J6_STATIC_ASSERT(step_is_8_bytes, sizeof(j6_step_t) == 8u);
J6_STATIC_ASSERT(pattern_is_536_bytes, sizeof(j6_pattern_t) == 536u);

/* Flash format of one pattern: J6_PAT_BYTES bytes, little endian, written by j6_pattern_encode():
 *   [0] J6_PAT_FORMAT  [1..2] tempo10  [3] last  [4] beat  [5] shuffle  [6] phrase  [7] sound
 *   [8] vol  [9] tran  [10] dsync  [11] avel  [12] pchg  [13] sclk  [14..21] knob[8]  [22..31] zero
 *   [32 + 8*s .. ] step s: note[4] flags phrase chord spare
 * The glue adds its own length and CRC around it (one 4 KiB flash sector per pattern). */
#define J6_PAT_FORMAT 1u
#define J6_PAT_HDR_BYTES 32u
#define J6_PAT_BYTES (J6_PAT_HDR_BYTES + 8u * J6_STEPS)          /* 544 */

/* step-edit results */
enum {
    J6_EDIT_NONE = 0,     /* nothing changed (ignored press) */
    J6_EDIT_ADDED,        /* a note was added */
    J6_EDIT_REMOVED,      /* a note (or a whole chord) was removed */
    J6_EDIT_FULL,         /* the step already holds 4 notes: nothing changed */
    J6_EDIT_REPLACED,     /* the step's notes were replaced by a chord */
    J6_EDIT_TIE_ADDED,    /* a tie was written (into *target) */
    J6_EDIT_TIE_REMOVED   /* the selected step's tie was removed */
};

/* The window the sequencer is playing (SQ-22): a step with notes and no tie, plus the T tie steps after it. */
typedef struct {
    uint8_t n;                       /* notes 0..4 (0 = no window: a rest, or stopped) */
    uint8_t note[J6_STEP_NOTES];     /* ascending, without trAn (the glue adds trAn for the synth) */
    uint8_t phrase;                  /* the effective phrase of the window (SQ-26), packed, bit 7 = on */
    uint8_t step;                    /* the step that opened it */
    uint8_t ties;                    /* T */
    uint32_t start;                  /* pattern position (ticks, nominal) of its first step */
    uint32_t len;                    /* step_len * (1 + T) ticks */
} j6_window_t;

/* sequencer states */
enum {
    J6_SEQ_STOPPED = 0,
    J6_SEQ_STANDBY,                  /* REC pressed while stopped: REC lit, PLAY blinks (SQ-30) */
    J6_SEQ_COUNTIN,                  /* Cnt.I beats before recording starts (SQ-31) */
    J6_SEQ_PLAYING
};

/* ======================================================================================================
 * 6. Phrase tables for styles 6..9 (PH-17 .. PH-19). Generated by tools/gen_phrase_tables.py into
 *    j6_phrase_tables.h from placeholders (PH-19) and, list by list, from the J-6 capture
 *    (capture/raw/phrases.jsonl). Placeholders and captured lists use the same format and code path.
 * ==================================================================================================== */

#define J6_PEV_SRC_MASK 0x03u        /* src bits 0..1: index into the held set H (0 = lowest note) */
#define J6_PEV_ACCENT 0x80u          /* src bit 7: accent, plays at A.vLo instead of vELo (PH-8) */

typedef struct {
    uint16_t t;                      /* onset in sub-ticks from the loop start, < loop * 16 */
    uint16_t len;                    /* gate in sub-ticks, >= 1 */
    uint8_t src;                     /* index into H | J6_PEV_ACCENT */
    int8_t semi;                     /* semitones added to H[src] (usually 0, +-12, +-24) */
} j6_pev_t;                          /* 6 bytes */

#define J6_PL_CAPTURED 0x01u         /* the list comes from the J-6 capture (0 = PLACEHOLDER, PH-19) */
#define J6_PL_STRUM_FIXED 0x02u      /* reserved: strum offsets are fixed-time (Q-8). v1 treats every
                                        time as tempo-relative */

typedef struct {
    uint16_t loop;                   /* loop length in ticks, 1..4095; 0 = empty list (plays nothing) */
    uint16_t first;                  /* index of the first event in the bank's pool */
    uint16_t count;                  /* number of events, sorted by t */
    uint8_t flags;                   /* J6_PL_* */
    uint8_t reserved;
} j6_plist_t;                        /* 8 bytes */

typedef struct {
    const j6_plist_t *lists;         /* J6_PLISTS entries, indexed by j6_plist_index() */
    const j6_pev_t *pool;
    uint16_t pool_len;
} j6_phrase_bank_t;

/* list for style 6..9, variation 1..12, n = 1..4 held notes */
static inline uint32_t j6_plist_index(uint32_t style, uint32_t var, uint32_t n)
{
    return ((style - J6_TABLE_STYLE_FIRST) * J6_VARS + (var - 1u)) * J6_HELD_MAX + (n - 1u);
}

/* ======================================================================================================
 * 7. Sounds and synth parameters (section 7)
 * ==================================================================================================== */

/* the 8 knob parameters (SD-3), in panel order; also the J6_SP_* ids 0..7 and pattern knob[] slots */
enum {
    J6_KNOB_CUTOFF = 0,   /* FILTER */
    J6_KNOB_RESO,         /* SHIFT + FILTER */
    J6_KNOB_RELEASE,      /* ENVELOPE */
    J6_KNOB_ATTACK,       /* SHIFT + ENVELOPE */
    J6_KNOB_DLY_LEVEL,    /* DELAY */
    J6_KNOB_DLY_TIME,     /* SHIFT + DELAY */
    J6_KNOB_REV_LEVEL,    /* REVERB */
    J6_KNOB_REV_TIME      /* SHIFT + REVERB */
};

/* j6_synth_set / j6_synth_get parameter ids */
enum {
    J6_SP_CUTOFF = J6_KNOB_CUTOFF,   /* 0..127 */
    J6_SP_RESO = J6_KNOB_RESO,       /* 0..127 */
    J6_SP_RELEASE = J6_KNOB_RELEASE, /* 0..127 */
    J6_SP_ATTACK = J6_KNOB_ATTACK,   /* 0..127 */
    J6_SP_DLY_LEVEL = J6_KNOB_DLY_LEVEL, /* 0..127 */
    J6_SP_DLY_TIME = J6_KNOB_DLY_TIME,   /* 0..127: free time (d.Syn off) or note value (d.Syn on, SD-4) */
    J6_SP_REV_LEVEL = J6_KNOB_REV_LEVEL, /* 0..127 */
    J6_SP_REV_TIME = J6_KNOB_REV_TIME,   /* 0..127 */
    J6_SP_VOLUME,         /* vOL 0..127, gain before the effects (SD-7); 100 = unity */
    J6_SP_DLY_SYNC,       /* d.Syn 0/1 */
    J6_SP_TEMPO10,        /* tempo for d.Syn, J6_TEMPO10_MIN..MAX */
    J6_SP_TUNE10,         /* tUnE J6_TUNE10_MIN..MAX (SD-8) */
    J6_SP_COUNT
};

/* d.Syn note values (SD-4), in ticks; J6_SP_DLY_TIME v selects index v * 12 / 128 [ASSUMED] */
#define J6_DSYNC_VALUES 12u
static inline uint32_t j6_dsync_ticks(uint32_t idx)
{
    static const uint8_t t[J6_DSYNC_VALUES] = {12, 16, 24, 32, 36, 48, 64, 72, 96, 128, 144, 192};
    return t[idx < J6_DSYNC_VALUES ? idx : J6_DSYNC_VALUES - 1u];
}

/* One preset sound (SD-1, SD-6). The J-6 has 64 fixed sounds; these are the FM-1's "Juno-ish" stand-ins,
 * defined by the synth module in j6_synth_sounds.c. All values 0..127 unless noted. A later Juno engine
 * port may reinterpret the oscillator fields but keeps the struct. */
typedef struct {
    char name[12];                   /* NUL-terminated ASCII, for the FM-1 screen */
    uint8_t knob[J6_KNOBS];          /* the sound's own knob values, J6_KNOB_* order */
    uint8_t saw;                     /* saw level */
    uint8_t pulse;                   /* pulse level */
    uint8_t pw;                      /* pulse width, 64 = square */
    uint8_t sub;                     /* sub oscillator (square, one octave down) level */
    uint8_t noise;                   /* noise level */
    uint8_t decay;                   /* envelope decay time */
    uint8_t sustain;                 /* envelope sustain level */
    uint8_t env_amt;                 /* filter envelope depth, 64 = none, above opens, below closes */
    uint8_t key_track;               /* filter keyboard tracking */
    uint8_t detune;                  /* second-oscillator detune / chorus width, 0 = off */
    uint8_t level;                   /* output level of the sound */
    uint8_t flags;                   /* J6_SND_* */
} j6_sound_t;                        /* 32 bytes */
#define J6_SND_CHORUS 0x01u          /* chorus / stereo spread on */
#define J6_SND_HPF_SHIFT 2u          /* bits 2..3: the Juno HPF: 0 flat, 1 bass boost, 2 / 3 high-pass ~240 / ~720 Hz */
#define J6_SND_HPF_MASK (3u << J6_SND_HPF_SHIFT)
#define J6_SND_CHMODE_SHIFT 4u       /* bits 4..5: chorus mode 0 = I, 1 = II, 2 = I+II (Juno-60 buttons) */
#define J6_SND_CHMODE_MASK (3u << J6_SND_CHMODE_SHIFT)
#define J6_SND_MONO_ENV 0x02u        /* reserved */

/* ======================================================================================================
 * 8. Global settings (section 6.2), kept by the glue, saved in storage slot J6_SLOT_GLOBAL.
 * ==================================================================================================== */

enum { J6_SYNC_AUTO = 0, J6_SYNC_INT, J6_SYNC_MIDI, J6_SYNC_USB };
enum { J6_PORT_USB = 0, J6_PORT_TRS = 1 };

typedef struct {
    uint8_t ch;                      /* CH 1..16 (notes) */
    uint8_t sync;                    /* SYnC J6_SYNC_* */
    uint8_t thru;                    /* thrv 0/1 */
    uint8_t txpc;                    /* txPc 0/1 */
    uint8_t rxpc;                    /* rxPc 0/1 */
    uint8_t pcch;                    /* Pc.Ch 1..16 */
    uint8_t velo;                    /* vELo 1..127 */
    uint8_t usbd;                    /* USb.d 0 = OFF, 1..127 */
    uint16_t tune10;                 /* tUnE */
    uint8_t alnk;                    /* A.LnK 0/1 (stored, ignored on the FM-1) */
    uint8_t cnti;                    /* Cnt.I 0 = OFF, 2..4 */
    uint8_t chord_set;               /* 0..99 [ASSUMED global] */
    int8_t key;                      /* KEY transpose -12..12 [ASSUMED global] */
    uint8_t pattern;                 /* pattern selected at power-off, 0..63 */
    uint8_t opts;                    /* J6_OPT_* owner options (docs/J6_DECISIONS.md) */
} j6_global_t;                       /* 16 bytes */
#define J6_OPT_HOLD_KEEP 0x01u       /* HoLd.E = KEEP: entering step edit leaves the HOLD latch on (D1) */
#define J6_OPT_CHORD_RETRIG 0x02u    /* cHd.L = OFF: a chord change retriggers shared notes too (D3) */
#define J6_GLOBAL_FORMAT 1u
#define J6_GLOBAL_BYTES 32u          /* encoded size in storage (format byte + fields + zero padding) */

/* ======================================================================================================
 * 9. Module APIs. The private state types are defined in j6_<module>_state.h.
 * ==================================================================================================== */

typedef struct j6_held_s j6_held_t;      /* j6_phrase_state.h */
typedef struct j6_phrase_s j6_phrase_t;  /* j6_phrase_state.h */
typedef struct j6_clock_s j6_clock_t;    /* j6_seq_state.h */
typedef struct j6_seq_s j6_seq_t;        /* j6_seq_state.h */
typedef struct j6_synth_s j6_synth_t;    /* j6_synth_state.h */

/* ---- 9.1 chord (j6_chord.c, tables in j6_chord_tables.h from tools/gen_tables.py) ------------------- */

/* KB-1, KB-6: the note of key code `key` with CHORD off: 60 + key + 12*oct + key_tr; -1 if outside 0..127. */
int32_t j6_key_note(int32_t key, int32_t oct, int32_t key_tr);

/* CH-2, CH-4: the chord of set 0..99, index 0..11 exactly as listed (ascending, duplicates removed:
 * set 43 key F gives 3 notes). Returns the count (2..4), 0 if set or idx is out of range. */
uint32_t j6_chord_raw(uint32_t set, uint32_t idx, uint8_t out[J6_CHORD_NOTES]);

/* CH-3, CH-5: the notes chord key `key` plays: chord index j6_key_chord_idx(key), shifted by
 * 12 * (oct + j6_key_floor12(key)) + key_tr. Notes outside 0..127 are dropped (KB-6); the rest are
 * ascending and distinct. Returns the count 0..4. If tag is not NULL it receives the chord tag
 * j6_chord_tag(idx, oct + j6_key_floor12(key)) for the step (SQ-3, SQ-11). */
uint32_t j6_chord_notes(uint32_t set, int32_t key, int32_t oct, int32_t key_tr,
                        uint8_t out[J6_CHORD_NOTES], uint8_t *tag);

const char *j6_chord_name(uint32_t set, uint32_t idx);   /* "Cadd9"; "" if out of range */
const char *j6_chord_genre(uint32_t set);               /* "Pop"; "" if out of range */

/* "C4", "F#3", "C-1" (C-1: C4 = 60) into buf; "--" for a note above 127 */
void j6_note_name(uint32_t note, char buf[5]);

/* ---- 9.2 held-note tracker with the HOLD latch (j6_phrase_held.c) ----------------------------------
 * Turns key / MIDI presses and releases into the effective set that sounds or feeds a phrase (PH-4,
 * HD-1 .. HD-3, CH-6). Ids are opaque bytes: MIDI notes in poly mode, chord key codes + 64 in mono mode.
 * Poly, latch off: the effective set is the last J6_HELD_MAX ids pressed among those still down.
 * Poly, latch on (HD-2): while any id is down, presses add to the latched set (at most 4, a 5th drops
 *   the oldest); a press after all ids were up replaces the set; releases change nothing; turning the
 *   latch on latches what is down; turning it off leaves only what is down.
 * Mono (CHORD on, CH-6): the effective set is one id: the last pressed that is still down; on its release
 *   the previous still-down id sounds again; nothing down -> empty, unless the latch keeps the last one. */
void j6_held_init(j6_held_t *h);
void j6_held_press(j6_held_t *h, uint32_t id);       /* a press of an id already down is ignored */
void j6_held_release(j6_held_t *h, uint32_t id);     /* a release of an id not down is ignored */
void j6_held_latch(j6_held_t *h, uint32_t on);       /* HOLD button or CC64 >= 64 (HD-3) */
void j6_held_mono(j6_held_t *h, uint32_t on);        /* changing the mode clears the tracker */
void j6_held_clear(j6_held_t *h);                    /* forget everything (panic, All Notes Off) */
uint32_t j6_held_get(const j6_held_t *h, uint8_t out[J6_HELD_MAX]);  /* effective set, ascending; count */
uint32_t j6_held_down(const j6_held_t *h);           /* ids physically down now */
uint32_t j6_held_latched(const j6_held_t *h);        /* 1 if the latch is on */
uint32_t j6_held_take_change(j6_held_t *h);          /* 1 once after the effective set changed, else 0 */

/* ---- 9.3 phrase engine (j6_phrase.c, tables in j6_phrase_tables.h) ---------------------------------
 * Plays a held set H as phrase style 1..9 / variation 1..12 on a phrase clock pt (section 4).
 * Styles 1-2 arpeggios (PH-10 .. PH-13), 3 note values (PH-14), 4-5 rhythm strings (PH-15, PH-16),
 * 6-9 event lists from a j6_phrase_bank_t (PH-17 .. PH-19). One instance per source: the glue keeps one
 * for live keys and one for the sequencer.
 * Timing contract: j6_phrase_tick() is called exactly once per elapsed tick, with that tick's pt. pt may
 * jump (pattern loop, play from a step) but time always advances one tick per call; note-offs are timed
 * by elapsed ticks, not by pt. A hit belongs to tick pt when its (shuffled) onset in sub-ticks lies in
 * [pt*16, pt*16 + 16). The instance never plays when H is empty.
 * Set changes (PH-6, PH-13, [ASSUMED]):
 *   - H becomes empty: every sounding note gets its NOTE_OFF at once, no new hits.
 *   - H changes to a different non-empty set: sounding notes whose source note left H stop at once;
 *     the arp index resets (PH-13); for BEAT and PHRASE styles a hit still inside its gate plays the
 *     new set's notes for the rest of that gate ("late join": staggered key presses sound at once),
 *     unless a hit onset falls in the current tick anyway.
 *   - the same set again (new step, tie): nothing happens, the arp index carries on. */
extern const j6_phrase_bank_t j6_phrase_builtin;  /* the generated tables (placeholder / captured) */

void j6_phrase_init(j6_phrase_t *p, uint32_t src);   /* src: J6_SRC_* written into every event */
void j6_phrase_set_bank(j6_phrase_t *p, const j6_phrase_bank_t *bank);   /* NULL = &j6_phrase_builtin */
void j6_phrase_set_style(j6_phrase_t *p, uint32_t style, uint32_t var);  /* 1..9, 1..12; from the next hit */
void j6_phrase_set_vel(j6_phrase_t *p, uint32_t vel, uint32_t accent_vel); /* vELo, A.vLo (PH-8) */
void j6_phrase_set_shuffle(j6_phrase_t *p, int32_t shuffle);           /* -90..90 (SQ-7) */
/* H := notes[0..n-1] (any order; sorted, deduplicated; with more than 4 the last 4 given are kept).
 * Pushes the immediate NOTE_OFFs / late-join NOTE_ONs of the set change into q (sub 0). */
void j6_phrase_set_held(j6_phrase_t *p, const uint8_t *notes, uint32_t n, j6_evq_t *q);
void j6_phrase_tick(j6_phrase_t *p, uint32_t pt, j6_evq_t *q);
void j6_phrase_stop(j6_phrase_t *p, j6_evq_t *q);    /* NOTE_OFF everything now, forget H */
uint32_t j6_phrase_sounding(const j6_phrase_t *p);   /* notes sounding now */
/* the loop length in ticks of a style / variation for n held notes (arps: cycle * division; for UI) */
uint32_t j6_phrase_loop_ticks(const j6_phrase_bank_t *bank, uint32_t style, uint32_t var, uint32_t n);
/* J6_PL_* flags of a table list (0 for styles 1..5): lets the UI mark placeholder phrases */
uint32_t j6_phrase_list_flags(const j6_phrase_bank_t *bank, uint32_t style, uint32_t var, uint32_t n);

/* ---- 9.4 clock (j6_seq_clock.c): audio frames or MIDI clock in -> ticks (C-5, MI-8, section 10) -----
 * Internal: a Q12 sample accumulator against j6_spt_q12(tempo10). External: every j6_clock_ext_pulse()
 * (a received 0xF8) is worth 4 ticks, spread evenly over the measured pulse interval (average of the
 * last 8); a pulse that arrives before its 4 ticks are out releases the rest at once. The clock runs
 * whether or not the sequencer plays (phrases and MIDI clock out need it). */
void j6_clock_init(j6_clock_t *c, uint32_t tempo10);
void j6_clock_set_tempo(j6_clock_t *c, uint32_t tempo10);   /* internal tempo; keeps the phase */
/* advance by `frames` (any count, the glue uses J6_CR); returns the ticks that began inside them */
uint32_t j6_clock_advance(j6_clock_t *c, uint32_t frames);
uint32_t j6_clock_sub(const j6_clock_t *c);         /* 0..15: position inside the current tick */
uint32_t j6_clock_ticks(const j6_clock_t *c);       /* ticks since init (wraps) */
uint32_t j6_clock_spt_q12(const j6_clock_t *c);     /* samples per tick in Q12 now (measured if external) */
uint32_t j6_clock_tempo10(const j6_clock_t *c);     /* tempo now (measured if external) */
void j6_clock_ext(j6_clock_t *c, uint32_t on);      /* 1 = follow j6_clock_ext_pulse() */
void j6_clock_ext_pulse(j6_clock_t *c);             /* one MIDI clock received */
void j6_clock_ext_resync(j6_clock_t *c);            /* Start received: drop the ticks still owed, the next
                                                       pulse begins the next tick (MI-8) */
uint32_t j6_clock_ext_alive(const j6_clock_t *c);   /* a pulse came in the last J6_EXT_CLOCK_TIMEOUT_MS */

/* ---- 9.5 sequencer (j6_seq.c, j6_seq_pattern.c) ----------------------------------------------------- */

/* pattern and step operations on one working copy (j6_seq_pattern.c). Pure functions; the caller marks
 * the pattern unsaved (j6_seq_edit). Step indices outside 0..63 are ignored. */
void j6_pattern_init(j6_pattern_t *p);       /* SQ-46 init: every field to its default, steps empty */
void j6_pattern_clear(j6_pattern_t *p);      /* SQ-44 CLEAR: steps empty, last 8, phrase default; keeps
                                                tempo, the *1 settings, sound and knobs */
void j6_step_clear(j6_step_t *s);            /* SQ-16: notes, tie, chord tag and phrase */
uint32_t j6_step_count(const j6_step_t *s);  /* notes in the step */
int32_t j6_step_find(const j6_step_t *s, uint32_t note);  /* slot 0..3 of note, -1 if absent */
/* SQ-12: CHORD off key in step edit: adds the note (clears the step's tie) or removes it if present */
uint32_t j6_step_key_note(j6_pattern_t *p, uint32_t step, uint32_t note);
/* SQ-11, SQ-13: CHORD on key in step edit: if the step holds a chord with this tag, the chord's notes
 * are removed; otherwise the step's notes become notes[0..n-1] with tag, tie cleared */
uint32_t j6_step_key_chord(j6_pattern_t *p, uint32_t step, const uint8_t *notes, uint32_t n, uint32_t tag);
/* SQ-17: HOLD in step edit. *target receives the step that got or lost the tie (J6_NONE if nothing) */
uint32_t j6_step_tie(j6_pattern_t *p, uint32_t step, uint8_t *target);
/* SQ-15: the step's phrase byte (bit 7 = STYLE flag) */
void j6_step_set_phrase(j6_pattern_t *p, uint32_t step, uint32_t phrase);
void j6_pattern_insert(j6_pattern_t *p, uint32_t step);  /* SQ-42 INSR */
void j6_pattern_dupl(j6_pattern_t *p);                   /* SQ-43 DUPL */
void j6_pattern_set_last(j6_pattern_t *p, uint32_t last); /* clamped to 1..64 */
void j6_pattern_page(j6_pattern_t *p, uint32_t page);    /* SQ-5: choosing page 0..7 may raise last */
uint32_t j6_pattern_step_ticks(const j6_pattern_t *p);   /* j6_beat_ticks(beat) */
uint32_t j6_pattern_len_ticks(const j6_pattern_t *p);    /* last * step ticks */
/* the window step `step` belongs to (SQ-22), without the phrase (the playback rule SQ-26 sets that) */
void j6_pattern_window_at(const j6_pattern_t *p, uint32_t step, j6_window_t *w);
/* flash format (section 5 above). encode returns J6_PAT_BYTES. decode returns 0, or -1 for a bad or
 * unknown buffer, in which case p is j6_pattern_init()ed. Out-of-range fields are clamped. */
uint32_t j6_pattern_encode(const j6_pattern_t *p, uint8_t out[J6_PAT_BYTES]);
int32_t j6_pattern_decode(j6_pattern_t *p, const uint8_t in[J6_PAT_BYTES]);

/* the sequencer instance (j6_seq.c): 64 working copies, transport, playback, recording */
void j6_seq_init(j6_seq_t *s);               /* every pattern j6_pattern_init()ed and clean, pattern 0, stopped */
const j6_pattern_t *j6_seq_pat(const j6_seq_t *s, uint32_t idx);   /* working copy to read */
j6_pattern_t *j6_seq_edit(j6_seq_t *s, uint32_t idx);             /* working copy to write: marks it unsaved */
uint32_t j6_seq_cur(const j6_seq_t *s);      /* current pattern */
uint32_t j6_seq_dirty(const j6_seq_t *s, uint32_t idx);   /* 1 = edited since saved / loaded (PS-2) */
void j6_seq_set_saved(j6_seq_t *s, uint32_t idx);         /* the glue wrote it to flash (or loaded it) */
void j6_seq_copy_pattern(j6_seq_t *s, uint32_t src, uint32_t dst);  /* SQ-45: dst := src working copy */
void j6_seq_step_copy(j6_seq_t *s, uint32_t step);        /* SQ-40: current pattern's step -> clipboard */
int32_t j6_seq_step_paste(j6_seq_t *s, uint32_t step);    /* SQ-41: 0, or -1 when the clipboard is empty */

void j6_seq_set_vel(j6_seq_t *s, uint32_t vel);           /* vELo for STYLE OFF notes (SQ-23) */
void j6_seq_set_countin(j6_seq_t *s, uint32_t beats);     /* Cnt.I: 0 = off, 2..4 */
/* SQ-27, SQ-28: stopped -> the switch is immediate (EV_PATTERN pushed now); playing -> queued, the switch
 * happens at the point the current pattern's P.chg gives (EV_PATTERN from j6_seq_tick). Selecting the
 * queued or the current pattern again while playing cancels the queue. */
void j6_seq_select(j6_seq_t *s, uint32_t idx, j6_evq_t *q);
uint32_t j6_seq_queued(const j6_seq_t *s);   /* the queued pattern or J6_NONE */
/* SQ-20, SQ-30, SQ-31: start at `from_step` (0 = PLAY); START(from_step) comes with the first tick.
 * From STANDBY: with Cnt.I off, playback and recording start together at from_step; with Cnt.I set,
 * the count-in runs first and then playback and recording start at step 0. Ignored unless STOPPED or
 * STANDBY (the glue turns PLAY while playing into j6_seq_stop). */
void j6_seq_play(j6_seq_t *s, uint32_t from_step, j6_evq_t *q);
void j6_seq_stop(j6_seq_t *s, j6_evq_t *q);  /* SQ-21: stop (also count-in, standby); NOTE_OFFs, HELD n=0, STOP */
/* the REC button (SQ-30, SQ-35): STOPPED -> STANDBY, STANDBY -> STOPPED, PLAYING -> punch in / out
 * (REC event), COUNTIN -> STOPPED [ASSUMED] */
void j6_seq_rec(j6_seq_t *s, j6_evq_t *q);
/* One tick of playback (call once per clock tick; does nothing when STOPPED or STANDBY). Processes the
 * position j6_seq_pos() will then report: step boundaries, windows (SQ-22, SQ-26), STYLE OFF notes
 * (SQ-23), shuffle (SQ-7), queued pattern changes (SQ-27), loop wrap, count-in. */
void j6_seq_tick(j6_seq_t *s, j6_evq_t *q);
uint32_t j6_seq_state(const j6_seq_t *s);    /* J6_SEQ_* */
uint32_t j6_seq_recording(const j6_seq_t *s);
uint32_t j6_seq_pos(const j6_seq_t *s);      /* pattern position (ticks) of the tick last processed: the
                                                sequencer's phrase clock pt (PH-5) */
uint32_t j6_seq_step_now(const j6_seq_t *s); /* the step playing, J6_NONE when not playing */
uint32_t j6_seq_countin_left(const j6_seq_t *s);  /* count-in beats left, 0 when none */
/* the phrase window playing now (n = 0: none, or a STYLE OFF window; j6_pattern_window_at() gives any
 * window's extent) */
const j6_window_t *j6_seq_window(const j6_seq_t *s);
/* SQ-32 .. SQ-34 real-time recording, from the glue while j6_seq_recording(): a key, chord key or MIDI
 * note went down. tag = J6_NO_CHORD for a single note (added, up to 4; never removes) or a chord tag
 * (replaces the step). phrase: bit 7 set = also write this step phrase (STYLE ON while recording). */
void j6_seq_rec_input(j6_seq_t *s, const uint8_t *notes, uint32_t n, uint32_t tag, uint32_t phrase);
void j6_seq_rec_held(j6_seq_t *s, uint32_t any_held);  /* SQ-33: while 1, empty steps entered get a tie */

/* ---- 9.6 synth and effects (j6_synth.c, j6_synth_fx.c, j6_synth_sounds.c, j6_synth_tables.h) ------- */

extern const j6_sound_t j6_sounds[J6_SOUNDS];

/* fx_mem: J6_FX_MEM_LEN int16_t (the glue's pool array); init zeroes it. A shorter fx_len (>= 4096)
 * gives a shorter delay; NULL disables delay and reverb (host tests of the voices). Loads sound 0. */
void j6_synth_init(j6_synth_t *s, int16_t *fx_mem, uint32_t fx_len);
void j6_synth_sound(j6_synth_t *s, uint32_t sound);        /* load a preset; the 8 knob params take the
                                                              preset's values */
void j6_synth_set(j6_synth_t *s, uint32_t param, int32_t v);   /* J6_SP_*; out-of-range values clamp */
int32_t j6_synth_get(const j6_synth_t *s, uint32_t param);
/* SD-5: an idle voice, else the quietest releasing one, else the oldest held one (after a 1.45 ms fade);
 * the same note again retriggers its own voice. vel 1..127 */
void j6_synth_note_on(j6_synth_t *s, uint32_t note, uint32_t vel);
void j6_synth_note_off(j6_synth_t *s, uint32_t note);      /* release the voice playing note */
void j6_synth_all_off(j6_synth_t *s);                      /* release every voice */
void j6_synth_panic(j6_synth_t *s);                        /* silence now: voices cut, delay and reverb cleared */
void j6_synth_kill(j6_synth_t *s);                         /* fade the whole output (voices + effects) to 0 over
                                                              J6Y_KILL_CHUNKS x J6_CR frames, then panic (D2) */
/* ADD n stereo frames (L R L R, Q15) to out. n is a multiple of J6_CR, at most J6_BLOCK. The sum is
 * clamped to +-J6_OUT_MAX. Runs in the audio interrupt: no division per sample, control rate per call. */
void j6_synth_render(j6_synth_t *s, int32_t *out, uint32_t n);
uint32_t j6_synth_active(const j6_synth_t *s);   /* voices sounding (gated or releasing) */
uint32_t j6_synth_fx_idle(const j6_synth_t *s);  /* 1 = delay, reverb and chorus lines are silent */

/* ======================================================================================================
 * 10. Engine glue (j6_engine.c, written in the integration step). One engine per firmware: a singleton.
 *
 * Threads: j6_engine_render() runs in the audio interrupt. Every other j6_engine_* function belongs to
 * the main loop. Input functions (key, MIDI in, cmd, set) only queue a command; the next render or
 * advance applies it, so they are safe while a render runs and their effect lags by at most one block.
 * Getters read state that may be one block old. On the host everything runs in one thread and is
 * deterministic: input, then render / advance, then read.
 * ==================================================================================================== */

/* Storage: slots 0..63 hold patterns (J6_PAT_BYTES), slot J6_SLOT_GLOBAL the globals (J6_GLOBAL_BYTES).
 * read / write return 0 on success. The firmware maps a slot to one 4 KiB flash sector with a length and
 * a CRC; the host tests use RAM. NULL store = nothing persists. */
#define J6_SLOT_GLOBAL 64u
typedef struct {
    int (*read)(void *ctx, uint32_t slot, uint8_t *buf, uint32_t len);
    int (*write)(void *ctx, uint32_t slot, const uint8_t *buf, uint32_t len);
    void *ctx;
} j6_store_t;

/* settings and panel controls for j6_engine_set / j6_engine_get */
enum {
    /* the current pattern's settings (a write marks the pattern unsaved) */
    J6_P_TEMPO10 = 0,     /* tempo, tenths of a BPM (SQ-6) */
    J6_P_LAST,            /* last step 1..64 */
    J6_P_BEAT,            /* bEAt J6_BEAT_* */
    J6_P_SHUFFLE,         /* -90..90 */
    J6_P_VOL,             /* vOL */
    J6_P_TRAN,            /* trAn */
    J6_P_DSYNC,           /* d.Syn */
    J6_P_AVEL,            /* A.vLo */
    J6_P_PCHG,            /* P.chg */
    J6_P_SCLK,            /* S.cLk index */
    J6_P_SOUND,           /* sound 0..63: loads it, clears the pattern's knob values (SD-2, SD-3) */
    J6_P_KNOB0,           /* J6_P_KNOB0 + J6_KNOB_*: the 8 knob parameters 0..127 */
    J6_P_KNOB_LAST = J6_P_KNOB0 + J6_KNOBS - 1,
    /* the phrase controls: outside step edit the current pattern's phrase (PH-20), in step edit the
       selected step's phrase (SQ-15) */
    J6_P_STYLE_ON,        /* STYLE [ON] 0/1 */
    J6_P_STYLE,           /* STYLE knob 1..9 */
    J6_P_VAR,             /* VARIATION knob 1..12 */
    /* globals (saved automatically, PS-3) */
    J6_P_CH, J6_P_SYNC, J6_P_THRU, J6_P_TXPC, J6_P_RXPC, J6_P_PCCH, J6_P_VELO, J6_P_TUNE10, J6_P_USBD,
    J6_P_ALNK, J6_P_CNTI,
    J6_P_CHORD_SET,       /* 0..99 */
    J6_P_KEY,             /* KEY transpose -12..12 */
    J6_P_HOLDE,           /* HoLd.E: 0 = OFF (entering step edit releases HOLD), 1 = KEEP (D1) */
    J6_P_CHDL,            /* cHd.L: 1 = On (shared chord notes keep sounding), 0 = OFF retrigger (D3) */
    /* panel state (not saved, HD-4) */
    J6_P_OCT,             /* octave -3..3 */
    J6_P_HOLD,            /* HOLD latch 0/1 (outside step edit) */
    J6_P_CHORD,           /* CHORD mode 0/1 */
    J6_P_COUNT
};

typedef struct {
    int16_t min, max, def;
    uint8_t per_pattern;             /* 1 = saved with the pattern (*1), 0 = global or panel */
    char name[7];                    /* the J-6 display text: "vOL", "trAn", "d.Syn" ... */
} j6_param_info_t;
const j6_param_info_t *j6_param_info(uint32_t id);   /* NULL for an unknown id */

/* commands for j6_engine_cmd (main-loop button presses with the J-6's meaning) */
enum {
    J6_CMD_PLAY = 0,      /* PLAY: start at step 1 (or the count-in from STANDBY); stop when playing */
    J6_CMD_PLAY_FROM,     /* arg = step: SHIFT + PLAY / hold a step + PLAY (SQ-20) */
    J6_CMD_STOP,
    J6_CMD_REC,           /* REC (SQ-30, SQ-35) */
    J6_CMD_PATTERN,       /* arg = pattern 0..63: select, or queue during playback (SQ-27) */
    J6_CMD_STEP_SELECT,   /* arg = step 0..63 to enter / move step edit, -1 to leave it (UI-4) */
    J6_CMD_PAGE,          /* arg = page 0..7 (SQ-5) */
    J6_CMD_HOLD,          /* the HOLD button: tie in step edit (SQ-17), latch toggle otherwise (HD-1) */
    J6_CMD_STYLE,         /* the STYLE [ON] button: the step's flag in step edit, else the pattern's */
    J6_CMD_STEP_CLEAR,    /* SQ-16 (after ENTER) */
    J6_CMD_STEP_COPY,     /* SQ-40 */
    J6_CMD_STEP_PASTE,    /* SQ-41 */
    J6_CMD_STEP_INSERT,   /* SQ-42 */
    J6_CMD_PATTERN_DUPL,  /* SQ-43 */
    J6_CMD_PATTERN_CLEAR, /* SQ-44 (after ENTER) */
    J6_CMD_PATTERN_INIT,  /* SQ-46 (after ENTER) */
    J6_CMD_PATTERN_COPY,  /* arg = destination pattern: SQ-45, saved to storage by j6_engine_poll() */
    J6_CMD_PANIC,         /* all notes off now, latch off, effects cleared */
    J6_CMD_COUNT
};

typedef struct {
    uint8_t seq_state;               /* J6_SEQ_* */
    uint8_t recording;               /* 0/1 */
    uint8_t pattern;                 /* current pattern */
    uint8_t queued;                  /* queued pattern or J6_NONE */
    uint8_t step_now;                /* step playing or J6_NONE */
    uint8_t sel_step;                /* step edit selection or J6_NONE */
    uint8_t countin_left;            /* beats */
    uint8_t voices;                  /* voices sounding */
    uint8_t fx_idle;                 /* 1 = effects silent */
    uint8_t hold, chord, style_on;   /* panel LEDs (style_on: the selected step's flag in step edit) */
    uint8_t ext_clock;               /* 1 = following an external clock */
    uint8_t last_note;               /* last note entered / played from the keys (display), J6_NO_NOTE */
    uint8_t last_chord;              /* chord tag of the last chord key, J6_NO_CHORD */
    uint8_t reserved;
    uint16_t tempo10;                /* tempo now (measured when external) */
    uint32_t dirty[2];               /* unsaved patterns: bit i of dirty[i / 32] */
    uint32_t keys_lit;               /* key LEDs, bit b = key code b + J6_KEY_MIN (FM-1 key b): step edit:
                                        the step's notes / chord key (SQ-14); else the keys sounding */
    uint32_t keys_dim;               /* step edit: notes outside the visible keys (SQ-14) */
    uint32_t ticks;                  /* free-running tick counter (blink phase) */
} j6_status_t;

/* fx_mem: J6_FX_MEM_LEN int16_t in the pool. Loads the globals and the 64 patterns from store (or the
 * defaults), selects the saved pattern. Safe to call again (host tests reset with it). */
void j6_engine_init(int16_t *fx_mem, uint32_t fx_len, const j6_store_t *store);
/* The input functions queue a command for the next render. The ring holds 256; its last 32 slots take
 * only releases (key up, Note Off / Note On velocity 0, CC 64 / 120 / 121 / 123, Stop), so a burst never
 * loses the end of a note. j6_engine_key / _midi_in return 1 if queued, 0 if the ring is full; the main
 * loop stops draining midi_get() while j6_engine_cmd_space() is 0 and goes on after the next render. */
uint32_t j6_engine_key(int32_t key, uint32_t down);  /* own keyboard, key code J6_KEY_MIN..J6_KEY_MAX */
/* one message from midi_get(); real-time messages (0xF8, 0xFA, 0xFC, 0xFE) come with d1 = d2 = 0 */
uint32_t j6_engine_midi_in(uint32_t port, uint32_t st, uint32_t d1, uint32_t d2);
uint32_t j6_engine_cmd_space(void);              /* free slots for any command (0 = only releases) */
void j6_engine_cmd(uint32_t cmd, int32_t arg);
void j6_engine_set(uint32_t param, int32_t v);   /* J6_P_*; clamps to j6_param_info() */
int32_t j6_engine_get(uint32_t param);
void j6_engine_status(j6_status_t *st);
const j6_pattern_t *j6_engine_pattern(uint32_t idx);   /* read-only view of a working copy (UI) */
const j6_synth_t *j6_engine_synth(void);         /* read-only view of the engine's synth (j6_synth_state.h:
                                                    host tests, a voice meter); may be one block old */
/* main loop: deferred storage work (PATTERN_COPY, global settings once silent for 2 s), Active Sensing */
void j6_engine_poll(void);
/* PS-1 WRITE: the current pattern (all = 0) or every pattern (all = 1), and the global settings if they
 * changed (PS-3). Main loop; blocks while the flash is written; the output is muted meanwhile. 0 = saved. */
int32_t j6_engine_write(uint32_t all);
/* PS-3: the app calls it when a menu or screen is left: unsaved global settings are written now, with a
 * short mute even while notes sound (j6_engine_poll() only saves them once silent). 0 = saved / nothing to do */
int32_t j6_engine_save_globals(void);
void j6_engine_factory_reset(void);              /* PS-4: defaults everywhere, written to storage */
/* the next MIDI message to send (main loop: while (j6_engine_midi_out(&a, &b, &c)) midi_send(a, b, c));
 * real-time messages have d1 = d2 = 0. Returns 1 if a message was taken. */
uint32_t j6_engine_midi_out(uint8_t *st, uint8_t *d1, uint8_t *d2);
/* Audio interrupt: apply queued commands, advance time by n frames in J6_CR chunks (ticks, phrases,
 * sequencer, events), render the synth and effects, ADD to out (n stereo frames, n % J6_CR == 0). */
void j6_engine_render(int32_t *out, uint32_t n);
/* the same without audio, for host tests that only look at events and MIDI (any n % J6_CR == 0) */
void j6_engine_advance(uint32_t n);

/* Observation tap for host tests and tools (the firmware leaves it NULL). fn is called from the render /
 * advance call for every event the glue handles, at the moment it takes effect: producer events (seq,
 * phrases) and the direct notes of keys and MIDI in (src KEY / CHORD / MIDI, sub 0). A NOTE_ON / NOTE_OFF
 * is reported when it reaches the synth, so a sub-tick event comes in the chunk where it fires. */
typedef struct {
    uint32_t frame;                  /* frames rendered since j6_engine_init() at the start of the chunk
                                        in which the event takes effect */
    uint32_t tick;                   /* engine ticks begun since init (the tick the event belongs to) */
    j6_event_t ev;                   /* the event as produced (note before trAn) */
    uint8_t synth_note;              /* NOTE_ON / NOTE_OFF: note + trAn as given to the synth, J6_NO_NOTE
                                        if it fell outside 0..127 */
    uint8_t reserved[3];
} j6_tap_t;
typedef void (*j6_tap_fn)(void *ctx, const j6_tap_t *t);
void j6_engine_set_tap(j6_tap_fn fn, void *ctx);   /* NULL = off; survives j6_engine_init() */

#endif /* J6_H */
