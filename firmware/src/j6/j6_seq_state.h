/* SPDX-License-Identifier: GPL-3.0-only */
/* j6_seq_state.h - private state of the sequencer module (j6_seq.c, j6_seq_pattern.c, j6_seq_clock.c).
 *
 * OWNER: the seq module. Only j6_seq*.c, tests/j6/test_seq.c and the glue (to allocate instances)
 * include this file. Nobody outside the seq module reads these fields; they use the j6_clock_* / j6_seq_*
 * API. The pattern data itself (j6_pattern_t) is public and fixed in j6.h.
 *
 * Time inside the sequencer: playback schedules NOTE_OFFs and shuffled note-ons on an elapsed clock
 * `el` (ticks processed since START) in sub-ticks, el * 16 + sub, compared with wrap-safe int32_t
 * differences. The pattern position (`pos`) jumps at loops and pattern changes; elapsed time does not. */
#ifndef J6_SEQ_STATE_H
#define J6_SEQ_STATE_H

#include "j6.h"

/* ---- clock (j6_seq_clock.c) ---- */
#define J6S_EXT_AVG 8u               /* MIDI clock intervals averaged (MI-8) */

struct j6_clock_s {
    uint32_t spt_q12;                /* internal samples per tick, Q12, from tempo10 */
    uint32_t acc_q12;                /* Q12 samples since the current tick began */
    uint32_t ticks;                  /* ticks begun since init */
    uint32_t frames;                 /* frames since init (wraps after 27 h; only differences are used) */
    uint32_t ext_spt_q12;            /* measured samples per tick, Q12 (valid when ext_n > 0) */
    uint32_t ext_last;               /* `frames` at the last pulse */
    uint32_t ext_due;                /* ticks to release at once at the next advance (pulse + catch-up) */
    uint32_t ext_iv[J6S_EXT_AVG];    /* last pulse intervals, frames */
    uint16_t tempo10;                /* internal tempo */
    uint8_t ext;                     /* 1 = following external pulses */
    uint8_t ext_owed;                /* ticks of the last pulse still to be spread (0..3) */
    uint8_t ext_n, ext_i;            /* intervals stored, next slot */
    uint8_t ext_seen;                /* a pulse arrived since init */
    uint8_t reserved;
};

/* ---- sequencer (j6_seq.c) ---- */
#define J6S_PLAIN_MAX 8u             /* STYLE OFF notes sounding at once: two overlapping windows of 4
                                        (a gate never reaches past the next window's next window) */

struct j6_seq_s {
    j6_pattern_t pat[J6_PATTERNS];   /* working copies (PS-2): 34304 bytes */
    uint32_t dirty[2];               /* unsaved patterns, bit i of dirty[i / 32] */
    j6_step_t clip;                  /* step clipboard (SQ-40) */
    j6_window_t win;                 /* the phrase window: what feeds the seq phrase instance (n = 0: none) */
    uint32_t pos;                    /* pattern position of the tick last processed */
    uint32_t el;                     /* playback ticks processed since START (elapsed time base) */
    uint32_t countin_t;              /* count-in ticks processed */
    uint32_t plain_off[J6S_PLAIN_MAX];  /* elapsed sub-tick time of each plain note's NOTE_OFF */
    uint32_t park_at;                /* elapsed sub-tick time of the parked window start (shuffle > 0) */
    uint32_t park_len;               /* window length (ticks) of the parked window */
    uint32_t pf_pos;                 /* boundary whose window was opened early (shuffle < 0) */
    uint8_t plain_note[J6S_PLAIN_MAX];  /* STYLE OFF notes sounding (SQ-23) */
    uint8_t park_note[J6_STEP_NOTES];   /* notes of the parked window */
    uint8_t nplain;
    uint8_t park_n;                  /* notes in the parked window, 0 = nothing parked */
    uint8_t pf_valid;                /* pf_pos is valid */
    uint8_t clip_ok;                 /* the clipboard holds a copied step */
    uint8_t cur;                     /* current pattern */
    uint8_t queued;                  /* queued pattern or J6_NONE */
    uint8_t state;                   /* J6_SEQ_* */
    uint8_t started;                 /* PLAYING and START pushed (the first tick ran) */
    uint8_t from_step;               /* start step for the first tick */
    uint8_t rec;                     /* recording now */
    uint8_t rec_on_start;            /* recording starts with the first tick (from STANDBY) */
    uint8_t rec_held;                /* SQ-33: a recorded key is held */
    uint8_t rec_chain;               /* last step recording wrote (note or tie) while held, J6_NONE */
    uint8_t countin;                 /* Cnt.I beats (0 = off) */
    uint8_t countin_left;            /* beats left in the count-in (j6_seq_countin_left) */
    uint8_t vel;                     /* vELo */
    uint8_t step;                    /* step playing, J6_NONE */
    uint8_t wstep;                   /* the step that opened the window playing (plain or phrase), J6_NONE */
    uint8_t cur_phrase;              /* SQ-26 `cur`, packed */
    uint8_t cur_inherit;             /* 1 = cur follows the pattern phrase (SQ-26 rule 3: no step flag yet) */
    uint8_t win_inh;                 /* the phrase window took its phrase from the pattern phrase */
    uint8_t cfg_wrap_keeps;          /* J6_CFG_WRAP_KEEPS_PHRASE (runtime copy so tests can try both) */
    uint8_t cfg_rec_nearest;         /* J6_CFG_REC_NEAREST */
    uint8_t cfg_plain_legato;        /* J6_CFG_PLAIN_LEGATO */
    uint8_t wplain_inh;              /* the STYLE OFF window playing (wstep) took STYLE OFF from the
                                        pattern phrase (PH-20: STYLE ON then applies at once) */
    uint8_t reserved[2];
};

J6_STATIC_ASSERT(clock_fits_budget, sizeof(struct j6_clock_s) <= J6_RAM_CLOCK_MAX);
J6_STATIC_ASSERT(seq_fits_budget, sizeof(struct j6_seq_s) <= J6_RAM_SEQ_MAX);

#endif /* J6_SEQ_STATE_H */
