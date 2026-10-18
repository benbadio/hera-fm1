/* SPDX-License-Identifier: GPL-3.0-only */
/* MORPHEUS: 8-Track Reason Matrix Pattern Sequencer & ReBirth Groovebox Core Model */
#ifndef MATRIX_CORE_H
#define MATRIX_CORE_H

#include <stdint.h>

#define MAX_STEPS 32u
#define NUM_BANKS 2u                           /* Bank A, B */
#define PATS_PER_BANK 8u                       /* 1..8 */
#define TOTAL_PATS (NUM_BANKS * PATS_PER_BANK) /* 16 */
#define NUM_TRACKS 8u                          /* T1..T8 */

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

typedef enum {
    TRACK_TYPE_303_A  = 0,
    TRACK_TYPE_303_B  = 1,
    TRACK_TYPE_808    = 2,
    TRACK_TYPE_909    = 3,
    TRACK_TYPE_BREAKS = 4,
    TRACK_TYPE_FM     = 5,
    TRACK_TYPE_SUB    = 6,
    TRACK_TYPE_MIDI   = 7,
    NUM_TRACK_TYPES   = 8
} matrix_track_type_t;

/* 12-Lane Drum Map for 808 and 909 kits */
enum {
    DRUM_LANE_BD  = 0,  /* Bass Drum */
    DRUM_LANE_SD  = 1,  /* Snare Drum */
    DRUM_LANE_LT  = 2,  /* Low Tom / Conga */
    DRUM_LANE_MT  = 3,  /* Mid Tom / Conga */
    DRUM_LANE_HT  = 4,  /* High Tom / Conga */
    DRUM_LANE_RS  = 5,  /* Rim Shot / Claves */
    DRUM_LANE_CP  = 6,  /* Clap / Maracas */
    DRUM_LANE_CB  = 7,  /* Cowbell */
    DRUM_LANE_CY  = 8,  /* Crash Cymbal */
    DRUM_LANE_OH  = 9,  /* Open Hi-Hat */
    DRUM_LANE_CH  = 10, /* Closed Hi-Hat */
    DRUM_LANE_ACC = 11, /* Total Accent */
    NUM_DRUM_LANES = 12
};

enum {
    MENU_NONE = 0,
    MENU_FX = 1,        /* Shift, Transpose, Alter, Randomize, Copy, Paste, Clear */
    MENU_SOUND = 2,     /* 303 A, 303 B, 808, 909, Breaks, FM, Sub, MIDI-only */
    MENU_TRACK = 3,     /* Track length, MIDI Channel, Curve CC */
    MENU_GLOBAL = 4     /* BPM, Clock Sync, Master Vol */
};

typedef struct {
    uint8_t note;        /* MIDI pitch 0..127 or drum lane 0..11 */
    uint8_t gate;        /* Velocity 0..127 (0 = rest) */
    uint8_t tie;         /* 1 = slide/legato to next step, 0 = normal */
    uint8_t curve;       /* 0..127 unipolar or -64..+63 bipolar */
    uint16_t drums;      /* Bitmask of triggered drum voices for 808/909 */
} matrix_step_t;

typedef struct {
    matrix_step_t steps[MAX_STEPS];
    uint8_t length;        /* 1..32 (default 16) */
    uint8_t resolution;    /* RES_1_16 default */
    uint8_t shuffle;       /* 0..75% swing */
    uint8_t curve_bipolar; /* 0 = unipolar, 1 = bipolar */
    uint8_t curve_cc;      /* MIDI CC destination (default 74 = Cutoff) */
    uint8_t midi_ch;       /* 1..16 */
    uint8_t mute;          /* 0 = active, 1 = muted */
    uint8_t track_type;    /* TRACK_TYPE_303_A..TRACK_TYPE_MIDI */
} matrix_pattern_t;

typedef struct {
    /* 8 tracks x 16 patterns */
    matrix_pattern_t patterns[NUM_TRACKS][TOTAL_PATS];
    
    /* Playback state per track */
    uint8_t cur_bank[NUM_TRACKS];      /* 0..1 (Bank A..B) */
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
    uint8_t track_types[NUM_TRACKS];   /* Assigned engine for each track */
    
    /* UI / Editor State */
    uint8_t active_track;              /* 0..7 (the track shown on screen) */
    uint8_t active_step;               /* 0..31 (cursor position) */
    uint8_t drum_lane_sel[NUM_TRACKS]; /* Focused drum lane 0..11 for 808/909 step entry */
    uint8_t view_mode;                 /* MODE_KEYS or MODE_CURVE */
    uint8_t view_octave;               /* 1..5 (default 3: C3..B3) */
    uint8_t view_page;                 /* 0 = Steps 1..16, 1 = Steps 17..32 */
    uint8_t rec_mode;                  /* 0 = off, 1 = step record advance */
    uint8_t menu_mode;                 /* MENU_NONE, MENU_FX, etc. */
    int8_t menu_sel;                   /* Selected item in menu */

    /* Pattern Clipboard */
    matrix_pattern_t clipboard;
    uint8_t has_clipboard;

    /* Undo / Redo Stacks */
#define MATRIX_UNDO_DEPTH 8
    struct {
        uint8_t track_idx;
        uint8_t bank_idx;
        uint8_t pat_idx;
        matrix_pattern_t pattern;
    } undo_stack[MATRIX_UNDO_DEPTH];
    uint8_t undo_count;

    struct {
        uint8_t track_idx;
        uint8_t bank_idx;
        uint8_t pat_idx;
        matrix_pattern_t pattern;
    } redo_stack[MATRIX_UNDO_DEPTH];
    uint8_t redo_count;
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

/* Undo / Redo operations */
void matrix_undo_push(uint8_t track_idx);
uint8_t matrix_undo(void);
uint8_t matrix_redo(void);
uint8_t matrix_can_undo(void);
uint8_t matrix_can_redo(void);

/* Clock & tick calculation */
uint32_t matrix_get_step_interval_us(uint8_t resolution, uint16_t bpm, uint8_t step_idx, uint8_t shuffle);

#endif /* MATRIX_CORE_H */
