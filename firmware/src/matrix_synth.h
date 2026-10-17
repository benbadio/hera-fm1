/* SPDX-License-Identifier: GPL-3.0-only */
/* MORPHEUS: Multi-Engine Digital Synthesizer (303, Subtractive, FM, Muted) */
#ifndef MATRIX_SYNTH_H
#define MATRIX_SYNTH_H

#include <stdint.h>

typedef struct {
    /* Pitch & Oscillator */
    uint32_t cur_phase;         /* 32-bit phase accumulator */
    uint32_t target_inc;        /* Target phase increment */
    uint32_t cur_inc;           /* Current increment (glides for 303 slide) */
    uint32_t glide_speed;       /* Slide rate */
    uint32_t osc2_phase;        /* For dual-osc subtractive */
    
    /* Envelopes (Q15 fixed point) */
    int32_t amp_level;          /* Current amplitude 0..32767 */
    int32_t flt_level;          /* Current filter env level 0..32767 */
    int32_t amp_decay;          /* Amp decay rate */
    int32_t flt_decay;          /* Filter decay rate */
    uint8_t gate_active;        /* 1 = key down, 0 = released */
    uint8_t is_accent;          /* 1 = accented step (velocity > 100) */
    
    /* 24dB / SVF Filter State (fixed-point Q14/Q15) */
    int32_t f_low;              /* Lowpass output */
    int32_t f_band;             /* Bandpass output */
    int32_t f_freq;             /* Filter cutoff coeff (0..16384) */
    int32_t f_q;                /* Resonance coeff (0..32767) */
    
    /* User Knobs & Settings */
    uint8_t knob_cutoff;        /* 0..127 */
    uint8_t knob_resonance;     /* 0..127 */
    uint8_t knob_decay;         /* 0..127 */
    uint8_t wave_type;          /* 0 = Saw, 1 = Square */
    uint8_t engine_mode;        /* SOUND_303, SOUND_SUBTRACTIVE, SOUND_FM, SOUND_MUTED */
    
    /* Modulation from Curve CV */
    int16_t curve_mod;          /* -64..+63 or 0..127 */
} matrix_synth_t;

extern matrix_synth_t g_synth;

void matrix_synth_init(void);
void matrix_synth_note_on(uint8_t note, uint8_t velocity, uint8_t is_tied);
void matrix_synth_note_off(void);
void matrix_synth_render(int32_t *out, uint32_t n);

#endif /* MATRIX_SYNTH_H */
