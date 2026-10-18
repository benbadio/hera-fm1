/* SPDX-License-Identifier: GPL-3.0-only */
/* MORPHEUS: 8-Track Reason Matrix Pattern Sequencer & ReBirth Groovebox Core */
#include "matrix_core.h"

matrix_core_t g_matrix;

/* Pseudo-random number generator for integer target */
static uint32_t s_rand = 0x12345678u;
static uint32_t m_rand(void)
{
    s_rand = s_rand * 1664525u + 1013904223u;
    return s_rand;
}

void matrix_core_init(void)
{
    uint32_t t, p, s;
    memset(&g_matrix, 0, sizeof(g_matrix));
    
    g_matrix.bpm = 128;
    g_matrix.is_running = 0;
    g_matrix.ext_sync = 0;
    
    g_matrix.active_track = 0;
    g_matrix.active_step = 0;
    g_matrix.view_mode = MODE_KEYS;
    g_matrix.view_octave = 3; /* C3..B3 */
    g_matrix.view_page = 0;
    g_matrix.menu_mode = MENU_NONE;
    
    /* Default Engine Types for each of the 8 tracks */
    static const uint8_t DEFAULT_ENGINES[NUM_TRACKS] = {
        TRACK_TYPE_303_A,
        TRACK_TYPE_303_B,
        TRACK_TYPE_808,
        TRACK_TYPE_909,
        TRACK_TYPE_BREAKS,
        TRACK_TYPE_FM,
        TRACK_TYPE_SUB,
        TRACK_TYPE_MIDI
    };

    static const uint8_t DEFAULT_MIDI_CH[NUM_TRACKS] = {
        1, 2, 10, 10, 5, 6, 7, 8
    };

    /* Initialize defaults across all 8 tracks and 16 patterns */
    for (t = 0; t < NUM_TRACKS; t++) {
        g_matrix.track_types[t] = DEFAULT_ENGINES[t];
        g_matrix.drum_lane_sel[t] = DRUM_LANE_BD;
        g_matrix.cur_bank[t] = 0;
        g_matrix.cur_pat[t] = 0;
        g_matrix.next_bank[t] = 0;
        g_matrix.next_pat[t] = 0;
        g_matrix.cur_step[t] = 0;
        g_matrix.last_note[t] = 255;
        g_matrix.last_gate[t] = 0;

        for (p = 0; p < TOTAL_PATS; p++) {
            matrix_pattern_t *pat = &g_matrix.patterns[t][p];
            pat->length = 16;
            pat->resolution = RES_1_16;
            pat->shuffle = 0;
            pat->curve_bipolar = 0;
            pat->curve_cc = 74; /* Standard Cutoff / Brightness */
            pat->midi_ch = DEFAULT_MIDI_CH[t];
            pat->mute = 0;
            pat->track_type = DEFAULT_ENGINES[t];

            for (s = 0; s < MAX_STEPS; s++) {
                pat->steps[s].note = 48; /* C3 */
                pat->steps[s].gate = 0;  /* Rest */
                pat->steps[s].tie = 0;
                pat->steps[s].curve = 64;
                pat->steps[s].drums = 0;
            }
        }
    }

    matrix_factory_patterns();
}

void matrix_factory_patterns(void)
{
    matrix_pattern_t *p1 = &g_matrix.patterns[0][0]; /* Track 1: TB-303 Acid Bass A */
    matrix_pattern_t *p2 = &g_matrix.patterns[1][0]; /* Track 2: TB-303 Acid Bass B */
    matrix_pattern_t *p3 = &g_matrix.patterns[2][0]; /* Track 3: TR-808 Drum Kit */
    matrix_pattern_t *p4 = &g_matrix.patterns[3][0]; /* Track 4: TR-909 Drum Kit */
    matrix_pattern_t *p5 = &g_matrix.patterns[4][0]; /* Track 5: Breakbeat Slicer */
    matrix_pattern_t *p6 = &g_matrix.patterns[5][0]; /* Track 6: FM Lead Synth */
    matrix_pattern_t *p7 = &g_matrix.patterns[6][0]; /* Track 7: Sub Synth Arp */
    matrix_pattern_t *p8 = &g_matrix.patterns[7][0]; /* Track 8: External MIDI */

    /* --- Track 1: TB-303 Acid Bass A (C3=48) --- */
    static const uint8_t T1_NOTES[16] = {48, 48, 60, 58, 55, 55, 53, 55, 51, 51, 53, 53, 48, 60, 58, 55};
    static const uint8_t T1_GATES[16] = {110,  0, 127, 95, 115,  0, 100, 120, 90,  0, 105, 115, 120, 127, 95,  0};
    static const uint8_t T1_TIES[16]  = {  0,  0,   1,  0,   0,  0,   0,   1,  0,  0,   0,   1,   0,   1,  0,  0};
    static const uint8_t T1_CURVE[16] = { 30, 45, 120, 80,  60, 40,  75, 110, 35, 20,  70, 100, 115, 125, 80, 40};

    /* --- Track 2: TB-303 Acid Bass B (Counter-line, D3=50) --- */
    static const uint8_t T2_NOTES[16] = {60, 63, 60, 65, 60, 67, 65, 63, 60, 58, 60, 63, 67, 70, 67, 65};
    static const uint8_t T2_GATES[16] = {  0, 115,  0, 120,  0, 127, 110,  0, 105,  0, 115,  0, 125,  0, 110,  0};
    static const uint8_t T2_TIES[16]  = {  0,   0,  0,   1,  0,   0,   1,  0,   0,  0,   1,  0,   0,  0,   1,  0};
    static const uint8_t T2_CURVE[16] = { 60,  75, 40,  95, 50, 115, 100, 60,  80, 50,  90, 70, 120, 85, 105, 50};

    /* --- Track 3: TR-808 Drum Machine Pattern --- */
    /* BD on 0, 4, 8, 12; SD on 4, 12; CH on every step; ACC on 0, 8, 10 */
    uint32_t s;
    for (s = 0; s < 16; s++) {
        uint16_t mask = (1u << DRUM_LANE_CH); /* Closed Hat on every 16th */
        if (s == 0 || s == 8 || s == 10) mask |= (1u << DRUM_LANE_BD) | (1u << DRUM_LANE_ACC);
        else if (s == 4 || s == 12) mask |= (1u << DRUM_LANE_BD) | (1u << DRUM_LANE_SD);
        else if (s == 6 || s == 14) mask |= (1u << DRUM_LANE_CP);
        if (s == 2 || s == 10) mask |= (1u << DRUM_LANE_OH);

        p3->steps[s].drums = mask;
        p3->steps[s].gate = (mask & (1u << DRUM_LANE_ACC)) ? 127 : 100;
        p3->steps[s].note = 36; /* General MIDI Kick */
        p3->steps[s].curve = 64;
    }

    /* --- Track 4: TR-909 Drum Machine Pattern --- */
    for (s = 0; s < 16; s++) {
        uint16_t mask = 0;
        if (s == 0 || s == 4 || s == 8 || s == 12) mask |= (1u << DRUM_LANE_BD); /* 4-on-the-floor */
        if (s == 4 || s == 12) mask |= (1u << DRUM_LANE_SD) | (1u << DRUM_LANE_CP);
        if (s % 2 == 1) mask |= (1u << DRUM_LANE_OH); /* Offbeat Open Hat */
        if (s == 0 || s == 8) mask |= (1u << DRUM_LANE_CY) | (1u << DRUM_LANE_ACC);

        p4->steps[s].drums = mask;
        p4->steps[s].gate = (mask & (1u << DRUM_LANE_ACC)) ? 127 : 105;
        p4->steps[s].note = 38; /* General MIDI Snare */
        p4->steps[s].curve = 80;
    }

    /* --- Track 5: Breakbeat Slicer / Loop --- */
    for (s = 0; s < 16; s++) {
        p5->steps[s].note = (s % 8); /* Slice 0..7 */
        p5->steps[s].gate = (s % 2 == 0) ? 120 : 95;
        p5->steps[s].tie = (s == 7 || s == 15);
        p5->steps[s].curve = 64;
    }

    /* --- Track 6: 2-Op FM Lead Synth --- */
    static const uint8_t T6_NOTES[16] = {72, 72, 75, 75, 77, 79, 77, 75, 72, 70, 72, 75, 79, 82, 79, 77};
    static const uint8_t T6_GATES[16] = {100, 100, 110, 110, 120, 125, 115, 105, 95, 95, 110, 115, 127, 120, 110, 100};
    for (s = 0; s < 16; s++) {
        p6->steps[s].note = T6_NOTES[s];
        p6->steps[s].gate = T6_GATES[s];
        p6->steps[s].tie = (s == 3 || s == 6 || s == 11 || s == 14);
        p6->steps[s].curve = 70 + (s * 3);
    }

    /* --- Track 7: Sub Synth Rolling Arp --- */
    static const uint8_t T7_NOTES[16] = {36, 43, 48, 43, 39, 43, 46, 43, 41, 44, 48, 44, 43, 46, 50, 46};
    for (s = 0; s < 16; s++) {
        p7->steps[s].note = T7_NOTES[s];
        p7->steps[s].gate = (s % 2 == 0) ? 95 : 85;
        p7->steps[s].tie = 0;
        p7->steps[s].curve = 50 + (s * 4);
    }

    /* --- Track 8: External MIDI Lead --- */
    for (s = 0; s < 16; s++) {
        p8->steps[s].note = 60 + ((s * 5) % 24);
        p8->steps[s].gate = (s % 4 == 0) ? 120 : (s % 2 == 0 ? 90 : 0);
        p8->steps[s].tie = 0;
        p8->steps[s].curve = 64;
    }

    /* Apply T1 and T2 arrays */
    for (s = 0; s < 16; s++) {
        p1->steps[s].note = T1_NOTES[s];
        p1->steps[s].gate = T1_GATES[s];
        p1->steps[s].tie  = T1_TIES[s];
        p1->steps[s].curve = T1_CURVE[s];

        p2->steps[s].note = T2_NOTES[s];
        p2->steps[s].gate = T2_GATES[s];
        p2->steps[s].tie  = T2_TIES[s];
        p2->steps[s].curve = T2_CURVE[s];
    }
}

/* Pattern Edit Operations */
void matrix_shift_horizontal(uint8_t track_idx, int8_t dir)
{
    if (track_idx >= NUM_TRACKS) return;
    matrix_undo_push(track_idx);
    matrix_pattern_t *pat = &g_matrix.patterns[track_idx][g_matrix.cur_bank[track_idx] * PATS_PER_BANK + g_matrix.cur_pat[track_idx]];
    uint8_t len = pat->length;
    if (len <= 1) return;

    if (dir > 0) {
        /* Shift Right */
        matrix_step_t last = pat->steps[len - 1];
        uint32_t i;
        for (i = len - 1; i > 0; i--) pat->steps[i] = pat->steps[i - 1];
        pat->steps[0] = last;
    } else if (dir < 0) {
        /* Shift Left */
        matrix_step_t first = pat->steps[0];
        uint32_t i;
        for (i = 0; i < len - 1; i++) pat->steps[i] = pat->steps[i + 1];
        pat->steps[len - 1] = first;
    }
}

void matrix_shift_vertical(uint8_t track_idx, int8_t semitones)
{
    if (track_idx >= NUM_TRACKS) return;
    matrix_undo_push(track_idx);
    matrix_pattern_t *pat = &g_matrix.patterns[track_idx][g_matrix.cur_bank[track_idx] * PATS_PER_BANK + g_matrix.cur_pat[track_idx]];
    uint32_t i;
    for (i = 0; i < pat->length; i++) {
        int32_t n = (int32_t)pat->steps[i].note + semitones;
        if (n < 0) n = 0;
        if (n > 127) n = 127;
        pat->steps[i].note = (uint8_t)n;
    }
}

void matrix_randomize(uint8_t track_idx)
{
    if (track_idx >= NUM_TRACKS) return;
    matrix_undo_push(track_idx);
    matrix_pattern_t *pat = &g_matrix.patterns[track_idx][g_matrix.cur_bank[track_idx] * PATS_PER_BANK + g_matrix.cur_pat[track_idx]];
    uint8_t engine = g_matrix.track_types[track_idx];

    if (engine == TRACK_TYPE_808 || engine == TRACK_TYPE_909) {
        /* Drum Randomizer: Create musical grooves */
        uint32_t i;
        for (i = 0; i < pat->length; i++) {
            uint16_t mask = 0;
            if (i % 4 == 0) mask |= (1u << DRUM_LANE_BD);
            if (i % 8 == 4) mask |= (1u << DRUM_LANE_SD);
            if ((m_rand() & 1) == 0) mask |= (1u << DRUM_LANE_CH);
            if ((m_rand() & 7) == 0) mask |= (1u << DRUM_LANE_OH);
            if ((m_rand() & 15) == 0) mask |= (1u << DRUM_LANE_CP);
            if (i % 8 == 0) mask |= (1u << DRUM_LANE_ACC);

            pat->steps[i].drums = mask;
            pat->steps[i].gate = (mask & (1u << DRUM_LANE_ACC)) ? 127 : 100;
            pat->steps[i].curve = (m_rand() >> 8) & 127;
        }
    } else {
        /* Melodic Scale Root C3 (48), pentatonic / blues minor intervals */
        static const uint8_t SCALE[7] = {0, 3, 5, 7, 10, 12, 15};
        uint32_t i;
        for (i = 0; i < pat->length; i++) {
            uint32_t r = m_rand();
            if ((r & 3) == 0) {
                pat->steps[i].gate = 0; /* 25% rest */
                pat->steps[i].tie = 0;
            } else {
                uint8_t note_idx = (r >> 2) % 7;
                pat->steps[i].note = 48 + SCALE[note_idx];
                pat->steps[i].gate = 80 + ((r >> 5) & 47); /* 80..127 */
                pat->steps[i].tie = ((r >> 11) & 7) == 0;  /* 12.5% tie slide */
            }
            pat->steps[i].curve = (m_rand() >> 8) & 127;
        }
    }
}

void matrix_alter(uint8_t track_idx)
{
    if (track_idx >= NUM_TRACKS) return;
    matrix_undo_push(track_idx);
    matrix_pattern_t *pat = &g_matrix.patterns[track_idx][g_matrix.cur_bank[track_idx] * PATS_PER_BANK + g_matrix.cur_pat[track_idx]];
    uint32_t i;
    for (i = 0; i < pat->length; i++) {
        uint32_t r = m_rand();
        if ((r & 3) == 0) {
            if (pat->steps[i].gate > 0) {
                int8_t delta = ((r >> 2) & 1) ? 2 : -2;
                int32_t n = (int32_t)pat->steps[i].note + delta;
                if (n >= 24 && n <= 100) pat->steps[i].note = (uint8_t)n;
                pat->steps[i].tie ^= ((r >> 4) & 1);
            }
        }
    }
}

void matrix_clear(uint8_t track_idx)
{
    if (track_idx >= NUM_TRACKS) return;
    matrix_undo_push(track_idx);
    matrix_pattern_t *pat = &g_matrix.patterns[track_idx][g_matrix.cur_bank[track_idx] * PATS_PER_BANK + g_matrix.cur_pat[track_idx]];
    uint32_t i;
    for (i = 0; i < MAX_STEPS; i++) {
        pat->steps[i].gate = 0;
        pat->steps[i].tie = 0;
        pat->steps[i].curve = 64;
        pat->steps[i].drums = 0;
    }
}

void matrix_copy(uint8_t track_idx)
{
    if (track_idx >= NUM_TRACKS) return;
    g_matrix.clipboard = g_matrix.patterns[track_idx][g_matrix.cur_bank[track_idx] * PATS_PER_BANK + g_matrix.cur_pat[track_idx]];
    g_matrix.has_clipboard = 1;
}

void matrix_paste(uint8_t track_idx)
{
    if (track_idx >= NUM_TRACKS || !g_matrix.has_clipboard) return;
    matrix_undo_push(track_idx);
    g_matrix.patterns[track_idx][g_matrix.cur_bank[track_idx] * PATS_PER_BANK + g_matrix.cur_pat[track_idx]] = g_matrix.clipboard;
}

/* =========================================================================
 * Undo / Redo System
 * ========================================================================= */
void matrix_undo_push(uint8_t track_idx)
{
    if (track_idx >= NUM_TRACKS) return;

    if (g_matrix.undo_count >= MATRIX_UNDO_DEPTH) {
        uint32_t i;
        for (i = 0; i < MATRIX_UNDO_DEPTH - 1; i++) {
            g_matrix.undo_stack[i] = g_matrix.undo_stack[i + 1];
        }
        g_matrix.undo_count = MATRIX_UNDO_DEPTH - 1;
    }

    uint8_t bank = g_matrix.cur_bank[track_idx];
    uint8_t pat = g_matrix.cur_pat[track_idx];
    g_matrix.undo_stack[g_matrix.undo_count].track_idx = track_idx;
    g_matrix.undo_stack[g_matrix.undo_count].bank_idx = bank;
    g_matrix.undo_stack[g_matrix.undo_count].pat_idx = pat;
    g_matrix.undo_stack[g_matrix.undo_count].pattern = g_matrix.patterns[track_idx][bank * PATS_PER_BANK + pat];
    g_matrix.undo_count++;

    /* New edit clears redo stack */
    g_matrix.redo_count = 0;
}

uint8_t matrix_undo(void)
{
    if (g_matrix.undo_count == 0) return 0;

    g_matrix.undo_count--;
    uint8_t trk = g_matrix.undo_stack[g_matrix.undo_count].track_idx;
    uint8_t bank = g_matrix.undo_stack[g_matrix.undo_count].bank_idx;
    uint8_t pat = g_matrix.undo_stack[g_matrix.undo_count].pat_idx;

    /* Push current pattern to redo stack */
    if (g_matrix.redo_count < MATRIX_UNDO_DEPTH) {
        g_matrix.redo_stack[g_matrix.redo_count].track_idx = trk;
        g_matrix.redo_stack[g_matrix.redo_count].bank_idx = bank;
        g_matrix.redo_stack[g_matrix.redo_count].pat_idx = pat;
        g_matrix.redo_stack[g_matrix.redo_count].pattern = g_matrix.patterns[trk][bank * PATS_PER_BANK + pat];
        g_matrix.redo_count++;
    }

    /* Restore from undo */
    g_matrix.patterns[trk][bank * PATS_PER_BANK + pat] = g_matrix.undo_stack[g_matrix.undo_count].pattern;
    return 1;
}

uint8_t matrix_redo(void)
{
    if (g_matrix.redo_count == 0) return 0;

    g_matrix.redo_count--;
    uint8_t trk = g_matrix.redo_stack[g_matrix.redo_count].track_idx;
    uint8_t bank = g_matrix.redo_stack[g_matrix.redo_count].bank_idx;
    uint8_t pat = g_matrix.redo_stack[g_matrix.redo_count].pat_idx;

    /* Push current back to undo stack */
    if (g_matrix.undo_count < MATRIX_UNDO_DEPTH) {
        g_matrix.undo_stack[g_matrix.undo_count].track_idx = trk;
        g_matrix.undo_stack[g_matrix.undo_count].bank_idx = bank;
        g_matrix.undo_stack[g_matrix.undo_count].pat_idx = pat;
        g_matrix.undo_stack[g_matrix.undo_count].pattern = g_matrix.patterns[trk][bank * PATS_PER_BANK + pat];
        g_matrix.undo_count++;
    }

    /* Restore from redo */
    g_matrix.patterns[trk][bank * PATS_PER_BANK + pat] = g_matrix.redo_stack[g_matrix.redo_count].pattern;
    return 1;
}

uint8_t matrix_can_undo(void) { return g_matrix.undo_count > 0; }
uint8_t matrix_can_redo(void) { return g_matrix.redo_count > 0; }

/* Timing helper: microsecond duration of a sequencer step with swing */
uint32_t matrix_get_step_interval_us(uint8_t resolution, uint16_t bpm, uint8_t step_idx, uint8_t shuffle)
{
    if (bpm < 40) bpm = 40;
    if (bpm > 250) bpm = 250;

    /* Base quarter note (1 beat) duration in microseconds: (60,000,000 / bpm) */
    uint32_t beat_us = 60000000u / bpm;
    uint32_t step_us = beat_us / 4u; /* Default 1/16th note */

    switch (resolution) {
        case RES_1_2:   step_us = beat_us * 2u; break;
        case RES_1_4:   step_us = beat_us; break;
        case RES_1_8:   step_us = beat_us / 2u; break;
        case RES_1_16:  step_us = beat_us / 4u; break;
        case RES_1_32:  step_us = beat_us / 8u; break;
        case RES_1_8T:  step_us = (beat_us * 2u) / 3u; break;
        case RES_1_16T: step_us = (beat_us * 4u) / 3u / 4u; break;
        default: break;
    }

    /* Apply shuffle (swing) on alternating (odd) steps */
    if (shuffle > 0 && (step_idx & 1u) == 1u) {
        uint32_t swing_us = (step_us * shuffle) / 200u;
        step_us += swing_us;
    } else if (shuffle > 0 && (step_idx & 1u) == 0u) {
        uint32_t swing_us = (step_us * shuffle) / 200u;
        if (step_us > swing_us) step_us -= swing_us;
    }

    return step_us;
}
