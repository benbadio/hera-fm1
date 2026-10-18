/* SPDX-License-Identifier: GPL-3.0-only */
/* MORPHEUS: ReBirth Groovebox Multi-Engine DSP Suite Header */
#ifndef MATRIX_SYNTH_H
#define MATRIX_SYNTH_H

#include <stdint.h>
#include "matrix_core.h"

/* Sound Engine Knobs and Parameters */
typedef struct {
    /* 303 A & B Parameters */
    uint8_t bass_a_cutoff;       /* 0..127 */
    uint8_t bass_a_resonance;    /* 0..127 */
    uint8_t bass_a_env_mod;      /* 0..127 */
    uint8_t bass_a_decay;        /* 0..127 */
    uint8_t bass_a_wave;         /* 0 = Saw, 1 = Square */
    uint8_t bass_a_drive;        /* 0..127 (Devilfish RAT) */

    uint8_t bass_b_cutoff;
    uint8_t bass_b_resonance;
    uint8_t bass_b_env_mod;
    uint8_t bass_b_decay;
    uint8_t bass_b_wave;
    uint8_t bass_b_drive;

    /* 808 Parameters */
    uint8_t drum808_bd_decay;
    uint8_t drum808_bd_tune;
    uint8_t drum808_sd_snap;
    uint8_t drum808_sd_tone;

    /* 909 Parameters */
    uint8_t drum909_bd_tune;
    uint8_t drum909_bd_decay;
    uint8_t drum909_sd_tune;
    uint8_t drum909_sd_snappy;

    /* FX & Master */
    uint8_t fx_delay_time;       /* 0..127 */
    uint8_t fx_delay_feedback;   /* 0..127 */
    uint8_t fx_reverb_level;     /* 0..127 */
    uint8_t fx_comp_glue;        /* 0..127 */
} matrix_synth_params_t;

extern matrix_synth_params_t g_synth_params;

void matrix_synth_init(void);
void matrix_synth_trigger_step(uint8_t track_idx, const matrix_step_t *st);
void matrix_synth_note_off(uint8_t track_idx);
void matrix_synth_render(int32_t *out, uint32_t n);
void matrix_synth_set_param(uint8_t track_idx, uint8_t param_id, uint8_t val);

#endif /* MATRIX_SYNTH_H */
