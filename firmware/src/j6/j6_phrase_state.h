/* SPDX-License-Identifier: GPL-3.0-only */
/* j6_phrase_state.h - private state of the phrase module (j6_phrase.c, j6_phrase_held.c).
 *
 * OWNER: the phrase module. Only j6_phrase*.c, tests/j6/test_phrase.c and the glue (to allocate
 * instances) include this file. Nobody outside the phrase module reads these fields; they use the
 * j6_held_* / j6_phrase_* API. The size asserts keep the structs inside the budgets of j6.h. */
#ifndef J6_PHRASE_STATE_H
#define J6_PHRASE_STATE_H

#include "j6.h"

/* ---- held-note tracker with the HOLD latch (j6_phrase_held.c) ---- */
struct j6_held_s {
    uint8_t down[J6_HELD_DOWN_MAX];  /* ids physically down, oldest press first */
    uint8_t ndown;
    uint8_t latch[J6_HELD_MAX];      /* latched ids, oldest first (latch on; mono: at most one) */
    uint8_t nlatch;
    uint8_t latch_on;                /* HOLD / CC64 */
    uint8_t mono;                    /* CHORD mode: one id at a time (CH-6) */
    uint8_t changed;                 /* effective set changed since j6_held_take_change() */
    uint8_t reserved;
};

/* ---- phrase engine (j6_phrase.c) ---- */
#define J6P_ACTIVE_MAX 16u           /* notes sounding at once (one entry per MIDI note) */
#define J6P_VEV_MAX 12u              /* hits in the loop of an algorithmic style (style 5 v7-12 restarting
                                        every bar: 6 x 2 - 1 = 11) */
#define J6P_CYCLE_MAX 16u            /* arp cycle: UP&DOWN2 over 4 notes is 2 * 8 - 2 = 14 notes */

struct j6p_active {                  /* one sounding note */
    uint32_t off;                    /* elapsed sub-tick (now * 16 + sub, wrapping) of its NOTE_OFF */
    uint8_t note;
    uint8_t from;                    /* the held note it was built from (set changes, PH-6) */
    uint8_t reserved[2];
};

struct j6_phrase_s {
    const j6_phrase_bank_t *bank;    /* styles 6-9 tables, never NULL after init */
    uint32_t now;                    /* elapsed ticks (tick() calls so far): the note-off time base */
    uint8_t src;                     /* J6_SRC_* tag for every event */
    uint8_t style;                   /* 1..9 */
    uint8_t var;                     /* 1..12 */
    uint8_t vel;                     /* vELo */
    uint8_t avel;                    /* A.vLo */
    int8_t shuffle;                  /* -90..90: the value set last (j6_phrase_set_shuffle) */
    uint8_t n;                       /* notes in H */
    uint8_t held[J6_HELD_MAX];       /* H, ascending */
    uint8_t nact;                    /* entries in act[] */
    uint8_t join;                    /* H changed between two non-empty sets: late join at the next tick */
    uint8_t cfg_range2;              /* J6_CFG_ARP_RANGE2 (copied at init; tests may flip it, then call
                                        j6_phrase_set_style / set_held to rebuild) */
    uint8_t cfg_s5_run_on;           /* J6_CFG_S5_RUN_ON (same) */
    uint8_t ncyc;                    /* arp cycle length (0 when H is empty) */
    uint8_t nvev;                    /* hits in vev[] */
    uint8_t fresh;                   /* H became non-empty since the last tick: that tick sets hold_pt */
    uint16_t vloop;                  /* loop of vev[] in ticks */
    uint32_t arp_idx;                /* arp hits since the last reset (PH-13); 32 bits: never wraps */
    int8_t shuf_eff;                 /* the shuffle the timing uses (takes `shuffle` at a tick, see SQ-7 in
                                        j6_phrase.c) */
    int8_t shuf_old;                 /* during a shuffle change: the value before it */
    uint8_t shuf_tr;                 /* a shuffle change is settling (since tick shuf_pt) */
    uint8_t ticked;                  /* a tick ran since init: last_pt is valid */
    uint16_t shuf_span;              /* during a change: the longest strum seen (sub-ticks, anchor to note) */
    uint32_t shuf_pt;                /* pt of the first tick with the new shuffle */
    uint32_t last_pt;                /* pt of the last tick */
    uint32_t hold_pt;                /* pt of the first tick of the current non-empty H: a late join only
                                        takes hits whose onset is at or after it (PH-21) */
    uint8_t cyc[J6P_CYCLE_MAX];      /* arp cycle: the notes */
    uint8_t cyc_from[J6P_CYCLE_MAX]; /* arp cycle: the held note each one comes from */
    j6_pev_t vev[J6P_VEV_MAX];       /* styles 1-5 as a loop of hits (t, len in sub-ticks; src, semi 0) */
    struct j6p_active act[J6P_ACTIVE_MAX];
};

J6_STATIC_ASSERT(held_fits_budget, sizeof(struct j6_held_s) <= J6_RAM_HELD_MAX);
J6_STATIC_ASSERT(phrase_fits_budget, sizeof(struct j6_phrase_s) <= J6_RAM_PHRASE_MAX);

#endif /* J6_PHRASE_STATE_H */
