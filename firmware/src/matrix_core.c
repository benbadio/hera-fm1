/* SPDX-License-Identifier: GPL-3.0-only */
/* MORPHEUS: Core Pattern Sequencer Implementation */
#include "matrix_core.h"
#include <string.h>

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
    g_matrix.sound_engine = SOUND_303;
    g_matrix.sound_track = 0; /* T1 drives internal synth */
    
    g_matrix.active_track = 0;
    g_matrix.active_step = 0;
    g_matrix.view_mode = MODE_KEYS;
    g_matrix.view_octave = 3; /* C3..B3 */
    g_matrix.view_page = 0;
    g_matrix.menu_mode = MENU_NONE;
    
    /* Initialize defaults across all 4 tracks and 32 patterns */
    for (t = 0; t < NUM_TRACKS; t++) {
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
            pat->midi_ch = (t == 3) ? 10 : (t + 1); /* T1=1, T2=2, T3=3, T4=10 */
            pat->mute = 0;

            for (s = 0; s < MAX_STEPS; s++) {
                pat->steps[s].note = 48; /* C3 */
                pat->steps[s].gate = 0;  /* Rest */
                pat->steps[s].tie = 0;
                pat->steps[s].curve = 64;
            }
        }
    }

    matrix_factory_patterns();
}

void matrix_factory_patterns(void)
{
    matrix_pattern_t *p1 = &g_matrix.patterns[0][0]; /* Track 1, Pattern A1: 303 Acid Bass */
    matrix_pattern_t *p2 = &g_matrix.patterns[1][0]; /* Track 2, Pattern A1: Lead Synth */
    matrix_pattern_t *p3 = &g_matrix.patterns[2][0]; /* Track 3, Pattern A1: Rolling Arp */
    matrix_pattern_t *p4 = &g_matrix.patterns[3][0]; /* Track 4, Pattern A1: 909/808 Beat */

    /* --- Track 1: Classic Acid 303 Bassline (C3=48) --- */
    static const uint8_t T1_NOTES[16] = {48, 48, 60, 58, 55, 55, 53, 55, 51, 51, 53, 53, 48, 60, 58, 55};
    static const uint8_t T1_GATES[16] = {110,  0, 127, 95, 115,  0, 100, 120, 90,  0, 105, 115, 120, 127, 95,  0};
    static const uint8_t T1_TIES[16]  = {  0,  0,   1,  0,   0,  0,   0,   1,  0,  0,   0,   1,   0,   1,  0,  0};
    static const uint8_t T1_CURVE[16] = { 30, 45, 120, 80,  60, 40,  75, 110, 35, 20,  70, 100, 115, 125, 80, 40};

    /* --- Track 2: Melodic Lead Hook (C4=60) --- */
    static const uint8_t T2_NOTES[16] = {60, 60, 63, 63, 65, 67, 65, 63, 60, 58, 60, 63, 67, 70, 67, 65};
    static const uint8_t T2_GATES[16] = {100, 100, 110, 110, 120, 125, 115, 105, 95, 95, 110, 115, 127, 120, 110, 100};
    static const uint8_t T2_TIES[16]  = {  0,   0,   0,   1,   0,   0,   1,   0,  0,  0,   0,   1,   0,   0,   1,   0};
    static const uint8_t T2_CURVE[16] = { 60,  65,  70,  75,  80,  90, 100,  85, 70, 65,  75,  90, 110, 120,  95,  70};

    /* --- Track 3: Driving 16th Arpeggiator (C3..G4) --- */
    static const uint8_t T3_NOTES[16] = {48, 55, 60, 55, 51, 55, 58, 55, 53, 56, 60, 56, 55, 58, 62, 58};
    static const uint8_t T3_GATES[16] = {90, 85, 100, 85, 90, 85, 95, 85, 90, 85, 100, 85, 95, 85, 110, 85};
    static const uint8_t T3_TIES[16]  = { 0,  0,   0,  0,  0,  0,  0,  0,  0,  0,   0,  0,  0,  0,   0,  0};
    static const uint8_t T3_CURVE[16] = {40, 50,  60, 70, 80, 90, 80, 70, 60, 50,  60, 70, 80, 90, 100, 80};

    /* --- Track 4: Drum Machine Pattern (Kick=36, Snare=38, Hat=42, OHat=46) --- */
    static const uint8_t T4_NOTES[16] = {36, 42, 38, 42, 36, 42, 38, 46, 36, 42, 38, 42, 36, 42, 38, 46};
    static const uint8_t T4_GATES[16] = {127, 80, 115, 80, 120, 80, 115, 85, 127, 80, 115, 80, 120, 90, 120, 95};
    static const uint8_t T4_TIES[16]  = {  0,  0,   0,  0,   0,  0,   0,  0,   0,  0,   0,  0,   0,  0,   0,  0};
    static const uint8_t T4_CURVE[16] = { 20, 30,  40, 50,  60, 70,  80, 90, 100, 90,  80, 70,  60, 50,  40, 30};

    uint32_t s;
    for (s = 0; s < 16; s++) {
        p1->steps[s].note = T1_NOTES[s];
        p1->steps[s].gate = T1_GATES[s];
        p1->steps[s].tie  = T1_TIES[s];
        p1->steps[s].curve = T1_CURVE[s];

        p2->steps[s].note = T2_NOTES[s];
        p2->steps[s].gate = T2_GATES[s];
        p2->steps[s].tie  = T2_TIES[s];
        p2->steps[s].curve = T2_CURVE[s];

        p3->steps[s].note = T3_NOTES[s];
        p3->steps[s].gate = T3_GATES[s];
        p3->steps[s].tie  = T3_TIES[s];
        p3->steps[s].curve = T3_CURVE[s];

        p4->steps[s].note = T4_NOTES[s];
        p4->steps[s].gate = T4_GATES[s];
        p4->steps[s].tie  = T4_TIES[s];
        p4->steps[s].curve = T4_CURVE[s];
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
    /* Scale root C3 (48), pentatonic / blues minor intervals */
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

void matrix_alter(uint8_t track_idx)
{
    if (track_idx >= NUM_TRACKS) return;
    matrix_undo_push(track_idx);
    matrix_pattern_t *pat = &g_matrix.patterns[track_idx][g_matrix.cur_bank[track_idx] * PATS_PER_BANK + g_matrix.cur_pat[track_idx]];
    uint32_t i;
    for (i = 0; i < pat->length; i++) {
        uint32_t r = m_rand();
        /* Mutate about 25% of steps */
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

    /* Reset redo stack on new action */
    g_matrix.redo_count = 0;
}

uint8_t matrix_can_undo(void)
{
    return g_matrix.undo_count > 0;
}

uint8_t matrix_can_redo(void)
{
    return g_matrix.redo_count > 0;
}

uint8_t matrix_undo(void)
{
    if (g_matrix.undo_count == 0) return 0;

    g_matrix.undo_count--;
    uint8_t t = g_matrix.undo_stack[g_matrix.undo_count].track_idx;
    uint8_t b = g_matrix.undo_stack[g_matrix.undo_count].bank_idx;
    uint8_t p = g_matrix.undo_stack[g_matrix.undo_count].pat_idx;
    if (t >= NUM_TRACKS || b >= NUM_BANKS || p >= PATS_PER_BANK) return 0;

    /* Push current into redo stack */
    if (g_matrix.redo_count < MATRIX_UNDO_DEPTH) {
        g_matrix.redo_stack[g_matrix.redo_count].track_idx = t;
        g_matrix.redo_stack[g_matrix.redo_count].bank_idx = b;
        g_matrix.redo_stack[g_matrix.redo_count].pat_idx = p;
        g_matrix.redo_stack[g_matrix.redo_count].pattern = g_matrix.patterns[t][b * PATS_PER_BANK + p];
        g_matrix.redo_count++;
    }

    /* Restore previous pattern */
    g_matrix.patterns[t][b * PATS_PER_BANK + p] = g_matrix.undo_stack[g_matrix.undo_count].pattern;
    g_matrix.active_track = t;
    g_matrix.cur_bank[t] = b;
    g_matrix.cur_pat[t] = p;
    return 1;
}

uint8_t matrix_redo(void)
{
    if (g_matrix.redo_count == 0) return 0;

    g_matrix.redo_count--;
    uint8_t t = g_matrix.redo_stack[g_matrix.redo_count].track_idx;
    uint8_t b = g_matrix.redo_stack[g_matrix.redo_count].bank_idx;
    uint8_t p = g_matrix.redo_stack[g_matrix.redo_count].pat_idx;
    if (t >= NUM_TRACKS || b >= NUM_BANKS || p >= PATS_PER_BANK) return 0;

    /* Push current into undo stack */
    if (g_matrix.undo_count < MATRIX_UNDO_DEPTH) {
        g_matrix.undo_stack[g_matrix.undo_count].track_idx = t;
        g_matrix.undo_stack[g_matrix.undo_count].bank_idx = b;
        g_matrix.undo_stack[g_matrix.undo_count].pat_idx = p;
        g_matrix.undo_stack[g_matrix.undo_count].pattern = g_matrix.patterns[t][b * PATS_PER_BANK + p];
        g_matrix.undo_count++;
    }

    /* Restore redo pattern */
    g_matrix.patterns[t][b * PATS_PER_BANK + p] = g_matrix.redo_stack[g_matrix.redo_count].pattern;
    g_matrix.active_track = t;
    g_matrix.cur_bank[t] = b;
    g_matrix.cur_pat[t] = p;
    return 1;
}

uint32_t matrix_get_step_interval_us(uint8_t resolution, uint16_t bpm, uint8_t step_idx, uint8_t shuffle)
{
    if (bpm < 20) bpm = 20;
    /* One quarter note in microseconds: 60,000,000 / bpm */
    uint32_t q_us = 60000000u / (uint32_t)bpm;
    uint32_t base_us;

    switch (resolution) {
        case RES_1_2:   base_us = q_us * 2u; break;
        case RES_1_4:   base_us = q_us; break;
        case RES_1_8:   base_us = q_us / 2u; break;
        case RES_1_16:  base_us = q_us / 4u; break;
        case RES_1_32:  base_us = q_us / 8u; break;
        case RES_1_8T:  base_us = q_us / 3u; break;
        case RES_1_16T: base_us = q_us / 6u; break;
        default:        base_us = q_us / 4u; break;
    }

    /* Apply shuffle (swing) on 16th notes */
    if (resolution == RES_1_16 && shuffle > 0 && shuffle <= 75) {
        /* Swing shifts odd steps backward by up to base_us / 3 */
        uint32_t delta = (base_us * (uint32_t)shuffle) / 300u;
        if (step_idx & 1u) {
            /* Second step in pair is delayed (longer wait after, shorter wait before) */
            return base_us > delta ? base_us - delta : base_us;
        } else {
            return base_us + delta;
        }
    }

    return base_us;
}
