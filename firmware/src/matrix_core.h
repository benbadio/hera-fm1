/* SPDX-License-Identifier: GPL-3.0-only */
/* MORPHEUS: 4-Track Reason Matrix Pattern Sequencer & Synth Core Model */
#ifndef MATRIX_CORE_H
#define MATRIX_CORE_H

#include <stdint.h>

#define MAX_STEPS 32u
#define NUM_BANKS 4u             /* A, B, C, D */
#define PATS_PER_BANK 8u         /* 1..8 */
#define TOTAL_PATS (NUM_BANKS * PATS_PER_BANK) /* 32 */
#define NUM_TRACKS 4u            /* T1, T2, T3, T4 */

enum {
    RES_1_2 = 0,
    RES_1_4 = 1,
    RES_1_8 = 2,
    RES_1_16 = 3,
    RES_1_32 = 4,
    RES_1_8T = 5,
    RES_1_16T = 6,
    NUM_RES = 7
};

enum {
    MODE_KEYS = 0,
    MODE_CURVE = 1
};

enum {
    SOUND_303 = 0,
    SOUND_SUBTRACTIVE = 1,
    SOUND_FM = 2,
    SOUND_MUTED = 3,
    NUM_SOUND_ENGINES = 4
};

enum {
    MENU_NONE = 0,
    MENU_FX = 1,        /* Shift, Transpose, Alter, Randomize, Copy, Paste, Clear */
    MENU_SOUND = 2,     /* 303, Sub, FM, Mute */
    MENU_TRACK = 3,     /* Track length, MIDI Channel, Curve CC */
    MENU_GLOBAL = 4     /* BPM, Clock Sync, Master Vol */
};

typedef struct {
    uint8_t note;        /* MIDI pitch 0..127 */
    uint8_t gate;        /* Velocity 0..127 (0 = rest) */
    uint8_t tie;         /* 1 = tied/legato to next step, 0 = normal */
    uint8_t curve;       /* 0..127 unipolar or -64..+63 bipolar */
} matrix_step_t;

typedef struct {
    matrix_step_t steps[MAX_STEPS];
    uint8_t length;      /* 1..32 (default 16) */
    uint8_t resolution;  /* RES_1_16 default */
    uint8_t shuffle;     /* 0..75% swing */
    uint8_t curve_bipolar; /* 0 = unipolar, 1 = bipolar */
    uint8_t curve_cc;    /* MIDI CC destination (default 74 = Cutoff) */
    uint8_t midi_ch;     /* 1..16 */
    uint8_t mute;        /* 0 = active, 1 = muted */
} matrix_pattern_t;

typedef struct {
    /* 4 tracks x 32 patterns */
    matrix_pattern_t patterns[NUM_TRACKS][TOTAL_PATS];
    
    /* Playback state per track */
    uint8_t cur_bank[NUM_TRACKS];      /* 0..3 (Bank A..D) */
    uint8_t cur_pat[NUM_TRACKS];       /* 0..7 (Pattern 1..8) */
    uint8_t next_bank[NUM_TRACKS];     /* Queued for next bar */
    uint8_t next_pat[NUM_TRACKS];
    uint8_t cur_step[NUM_TRACKS];      /* 0..31 currently playing step */
    uint32_t step_ticks[NUM_TRACKS];   /* Sub-step clock tick accumulator */
    uint8_t last_note[NUM_TRACKS];     /* Note playing right now */
    uint8_t last_gate[NUM_TRACKS];     /* Gate playing right now */
    
    /* Global Sequencer Settings */
    uint16_t bpm;                      /* 40..250 BPM (default 128) */
    uint8_t is_running;                /* 0 = stop, 1 = run */
    uint8_t ext_sync;                  /* 0 = internal clock, 1 = external MIDI clock */
    uint8_t sound_engine;              /* SOUND_303, SOUND_SUBTRACTIVE, etc. */
    uint8_t sound_track;               /* Which track drives internal synth (0..3, default 0 = T1) */
    
    /* UI / Editor State */
    uint8_t active_track;              /* 0..3 (the track shown on screen) */
    uint8_t active_step;               /* 0..31 (cursor position) */
    uint8_t view_mode;                 /* MODE_KEYS or MODE_CURVE */
    uint8_t view_octave;               /* 1..5 (default 3: C3..B3) */
    uint8_t view_page;                 /* 0 = Steps 1..16, 1 = Steps 17..32 */
    uint8_t rec_mode;                  /* 0 = off, 1 = step record advance */
    uint8_t menu_mode;                 /* MENU_NONE, MENU_FX, etc. */
    int8_t menu_sel;                   /* Selected item in menu */

    /* Pattern Clipboard */
    matrix_pattern_t clipboard;
    uint8_t has_clipboard;
} matrix_core_t;

/* Global instance */
extern matrix_core_t g_matrix;

/* Sequencer helper functions */
void matrix_core_init(void);
void matrix_factory_patterns(void);

/* Pattern operations */
void matrix_shift_horizontal(uint8_t track_idx, int8_t dir);
void matrix_shift_vertical(uint8_t track_idx, int8_t semitones);
void matrix_randomize(uint8_t track_idx);
void matrix_alter(uint8_t track_idx);
void matrix_clear(uint8_t track_idx);
void matrix_copy(uint8_t track_idx);
void matrix_paste(uint8_t track_idx);

/* Clock & tick calculation */
uint32_t matrix_get_step_interval_us(uint8_t resolution, uint16_t bpm, uint8_t step_idx, uint8_t shuffle);

#endif /* MATRIX_CORE_H */
