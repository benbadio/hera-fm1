/* SPDX-License-Identifier: GPL-3.0-only */
/* MORPHEUS: ReBirth Multi-Engine Groovebox DSP Suite Implementation */
#include "matrix_synth.h"
#include "matrix_core.h"

/* Include ReBirth DSP sources into single compilation unit */
#define X0X_DRUM_TABLES_DEFINE
#include "dsp/fastmath.h"
#include "dsp/x0x_param.h"
#include "dsp/bass303.h"
#include "dsp/drum808.h"
#include "dsp/drum909.h"
#include "dsp/breaks.h"
#include "dsp/fxbus.h"
#include "dsp/master.h"

#include "dsp/bass303.c"
#include "dsp/drum808.c"
#include "dsp/drum909.c"
#include "dsp/breaks.c"
#include "dsp/fxbus.c"
#include "dsp/master.c"

#ifndef POOL
#define POOL __attribute__((section(".pool")))
#endif

matrix_synth_params_t g_synth_params;

/* Large DSP structures in .pool memory to preserve fast SRAM */
static bass303_t g_303[2] POOL;
static drum808_t g_d808 POOL;
static drum909_t g_d909 POOL;
static breaks_t g_breaks POOL;
static fxbus_t g_fxbus POOL;
static master_t g_master;

#define DLY_LEN 44100
static int16_t g_dly_buf[DLY_LEN] POOL;

/* Scratch audio render buffers (256 stereo frames) */
#define MAX_AUDIO_FRAMES 256
static float s_dry_l[MAX_AUDIO_FRAMES];
static float s_dry_r[MAX_AUDIO_FRAMES];
static float s_rev[MAX_AUDIO_FRAMES];
static float s_dly[MAX_AUDIO_FRAMES];
static float s_fx_l[MAX_AUDIO_FRAMES];
static float s_fx_r[MAX_AUDIO_FRAMES];
static float s_mono_tmp[MAX_AUDIO_FRAMES];

/* Fast integer synthesizer state for FM / Sub tracks */
static uint32_t s_fm_phase = 0;
static uint32_t s_fm_inc = 0;
static int32_t  s_fm_amp = 0;
static uint32_t s_sub_phase = 0;
static uint32_t s_sub_inc = 0;
static int32_t  s_sub_amp = 0;

static const uint32_t INC_C9_SYNTH[12] = {
    815363807u,  863847862u,  915214929u,  969636441u,
    1027294024u, 1088380105u, 1153098554u, 1221665363u,
    1294309365u, 1371273005u, 1452813141u, 1539201906u
};

static uint32_t midi_to_inc(uint8_t note)
{
    if (note > 127) note = 127;
    return INC_C9_SYNTH[note % 12u] >> (10u - (note / 12u));
}

void matrix_synth_init(void)
{
    memset(&g_synth_params, 0, sizeof(g_synth_params));

    /* Initialize 303 Engines */
    bass303_init(&g_303[0]);
    bass303_init(&g_303[1]);
    bass303_set(&g_303[0], 0, 75);  /* Cutoff */
    bass303_set(&g_303[0], 1, 95);  /* Resonance */
    bass303_set(&g_303[0], 2, 80);  /* Env Mod */
    bass303_set(&g_303[0], 3, 65);  /* Decay */
    bass303_set(&g_303[0], 4, 30);  /* Drive */

    bass303_set(&g_303[1], 0, 85);
    bass303_set(&g_303[1], 1, 90);
    bass303_set(&g_303[1], 2, 70);
    bass303_set(&g_303[1], 3, 55);
    bass303_set(&g_303[1], 4, 45);

    /* Initialize 808 & 909 Drum Kits */
    drum808_init(&g_d808);
    drum909_init(&g_d909);

    /* Initialize Breaks Slicer */
    breaks_init(&g_breaks);

    /* Initialize Master Bus and Effects */
    fxbus_init(&g_fxbus, g_dly_buf, DLY_LEN);
    fxbus_set_bpm(&g_fxbus, 128.0f);
    fxbus_set(&g_fxbus, FX_DL_TIME, 64);   /* 3/16th delay */
    fxbus_set(&g_fxbus, FX_DL_FDBK, 50);
    fxbus_set(&g_fxbus, FX_RV_LEVEL, 40);
    fxbus_set(&g_fxbus, FX_RV_DECAY, 60);

    master_init(&g_master);
    master_set(&g_master, 0, 65);          /* Punchy glue compressor */
}

void matrix_synth_trigger_step(uint8_t track_idx, const matrix_step_t *st)
{
    if (track_idx >= NUM_TRACKS || !st) return;
    uint8_t engine = g_matrix.track_types[track_idx];

    switch (engine) {
        case TRACK_TYPE_303_A:
            if (st->gate > 0) {
                int accent = (st->gate >= 105) ? 1 : 0;
                bass303_note_on(&g_303[0], st->note, accent, st->tie);
                /* Curve CV dynamically modulates cutoff */
                bass303_set(&g_303[0], 0, st->curve);
            }
            break;

        case TRACK_TYPE_303_B:
            if (st->gate > 0) {
                int accent = (st->gate >= 105) ? 1 : 0;
                bass303_note_on(&g_303[1], st->note, accent, st->tie);
                bass303_set(&g_303[1], 0, st->curve);
            }
            break;

        case TRACK_TYPE_808: {
            uint16_t m = st->drums;
            float vel = (m & (1u << DRUM_LANE_ACC)) ? 1.0f : 0.65f;
            if (m & (1u << DRUM_LANE_BD)) drum808_trigger(&g_d808, D8_BD, vel);
            if (m & (1u << DRUM_LANE_SD)) drum808_trigger(&g_d808, D8_SD, vel);
            if (m & (1u << DRUM_LANE_LT)) drum808_trigger(&g_d808, D8_LT, vel);
            if (m & (1u << DRUM_LANE_MT)) drum808_trigger(&g_d808, D8_MT, vel);
            if (m & (1u << DRUM_LANE_HT)) drum808_trigger(&g_d808, D8_HT, vel);
            if (m & (1u << DRUM_LANE_RS)) drum808_trigger(&g_d808, D8_RS, vel);
            if (m & (1u << DRUM_LANE_CP)) drum808_trigger(&g_d808, D8_CP, vel);
            if (m & (1u << DRUM_LANE_CB)) drum808_trigger(&g_d808, D8_CB, vel);
            if (m & (1u << DRUM_LANE_CY)) drum808_trigger(&g_d808, D8_CY, vel);
            if (m & (1u << DRUM_LANE_OH)) drum808_trigger(&g_d808, D8_OH, vel);
            if (m & (1u << DRUM_LANE_CH)) drum808_trigger(&g_d808, D8_CH, vel);
            break;
        }

        case TRACK_TYPE_909: {
            uint16_t m = st->drums;
            float vel = (m & (1u << DRUM_LANE_ACC)) ? 1.0f : 0.65f;
            if (m & (1u << DRUM_LANE_BD)) drum909_trigger(&g_d909, DR_BD, vel);
            if (m & (1u << DRUM_LANE_SD)) drum909_trigger(&g_d909, DR_SD, vel);
            if (m & (1u << DRUM_LANE_LT)) drum909_trigger(&g_d909, DR_LT, vel);
            if (m & (1u << DRUM_LANE_MT)) drum909_trigger(&g_d909, DR_MT, vel);
            if (m & (1u << DRUM_LANE_HT)) drum909_trigger(&g_d909, DR_HT, vel);
            if (m & (1u << DRUM_LANE_RS)) drum909_trigger(&g_d909, DR_RS, vel);
            if (m & (1u << DRUM_LANE_CP)) drum909_trigger(&g_d909, DR_CP, vel);
            if (m & (1u << DRUM_LANE_CH)) drum909_trigger(&g_d909, DR_CH, vel);
            if (m & (1u << DRUM_LANE_OH)) drum909_trigger(&g_d909, DR_OH, vel);
            if (m & (1u << DRUM_LANE_CY)) drum909_trigger(&g_d909, DR_CR, vel);
            if (m & (1u << DRUM_LANE_CB)) drum909_trigger(&g_d909, DR_RD, vel);
            break;
        }

        case TRACK_TYPE_BREAKS:
            if (st->gate > 0) {
                float sp16 = 44100.0f / (g_matrix.bpm * 4.0f / 60.0f);
                breaks_step(&g_breaks, st->note % 8, 0, sp16, 1);
            }
            break;

        case TRACK_TYPE_FM:
            if (st->gate > 0) {
                s_fm_inc = midi_to_inc(st->note);
                s_fm_amp = (int32_t)st->gate * 200;
            }
            break;

        case TRACK_TYPE_SUB:
            if (st->gate > 0) {
                s_sub_inc = midi_to_inc(st->note);
                s_sub_amp = (int32_t)st->gate * 200;
            }
            break;

        case TRACK_TYPE_MIDI:
        default:
            /* Audio handled externally via MIDI */
            break;
    }
}

void matrix_synth_note_off(uint8_t track_idx)
{
    if (track_idx >= NUM_TRACKS) return;
    uint8_t engine = g_matrix.track_types[track_idx];
    if (engine == TRACK_TYPE_303_A) bass303_note_off(&g_303[0]);
    else if (engine == TRACK_TYPE_303_B) bass303_note_off(&g_303[1]);
    else if (engine == TRACK_TYPE_BREAKS) breaks_stop(&g_breaks);
    else if (engine == TRACK_TYPE_FM) s_fm_amp = 0;
    else if (engine == TRACK_TYPE_SUB) s_sub_amp = 0;
}

void matrix_synth_set_param(uint8_t track_idx, uint8_t param_id, uint8_t val)
{
    if (track_idx >= NUM_TRACKS) return;
    uint8_t engine = g_matrix.track_types[track_idx];
    if (engine == TRACK_TYPE_303_A) bass303_set(&g_303[0], param_id, val);
    else if (engine == TRACK_TYPE_303_B) bass303_set(&g_303[1], param_id, val);
    else if (engine == TRACK_TYPE_808) drum808_set(&g_d808, D8_KIT, param_id, val);
    else if (engine == TRACK_TYPE_909) drum909_set(&g_d909, DR_KIT, param_id, val);
}

void matrix_synth_render(int32_t *out, uint32_t n)
{
    if (n > MAX_AUDIO_FRAMES) n = MAX_AUDIO_FRAMES;
    uint32_t i;

    /* 1. Clear scratch mixing buffers */
    memset(s_dry_l, 0, n * sizeof(float));
    memset(s_dry_r, 0, n * sizeof(float));
    memset(s_rev, 0, n * sizeof(float));
    memset(s_dly, 0, n * sizeof(float));

    /* 2. Render TB-303 Acid Bass A (panned slightly left) */
    bass303_render(&g_303[0], s_mono_tmp, (int)n);
    for (i = 0; i < n; i++) {
        s_dry_l[i] += s_mono_tmp[i] * 0.95f;
        s_dry_r[i] += s_mono_tmp[i] * 0.75f;
        s_dly[i]   += s_mono_tmp[i] * 0.35f;
    }

    /* 3. Render TB-303 Acid Bass B (panned slightly right) */
    bass303_render(&g_303[1], s_mono_tmp, (int)n);
    for (i = 0; i < n; i++) {
        s_dry_l[i] += s_mono_tmp[i] * 0.75f;
        s_dry_r[i] += s_mono_tmp[i] * 0.95f;
        s_rev[i]   += s_mono_tmp[i] * 0.30f;
    }

    /* 4. Render TR-808 Drum Kit (stereo + sends) */
    drum808_render_st(&g_d808, s_dry_l, s_dry_r, s_rev, s_dly, (int)n);

    /* 5. Render TR-909 Drum Kit (stereo + sends) */
    drum909_render_st(&g_d909, s_dry_l, s_dry_r, s_rev, s_dly, (int)n);

    /* 6. Render Breakbeat Slicer */
    breaks_render(&g_breaks, s_mono_tmp, (int)n);
    for (i = 0; i < n; i++) {
        s_dry_l[i] += s_mono_tmp[i] * 0.8f;
        s_dry_r[i] += s_mono_tmp[i] * 0.8f;
    }

    /* 7. Render FM Lead & Sub Voices */
    if (s_fm_amp > 0 || s_sub_amp > 0) {
        for (i = 0; i < n; i++) {
            float s_fm = 0.0f, s_sub = 0.0f;
            if (s_fm_amp > 0) {
                s_fm_phase += s_fm_inc;
                s_fm = ((float)((int32_t)(s_fm_phase >> 16) - 32768) / 32768.0f) * ((float)s_fm_amp / 25000.0f);
                if (s_fm_amp > 50) s_fm_amp -= 35; else s_fm_amp = 0;
            }
            if (s_sub_amp > 0) {
                s_sub_phase += s_sub_inc;
                s_sub = ((s_sub_phase < 0x80000000u) ? 0.6f : -0.6f) * ((float)s_sub_amp / 25000.0f);
                if (s_sub_amp > 50) s_sub_amp -= 45; else s_sub_amp = 0;
            }
            s_dry_l[i] += (s_fm * 0.7f + s_sub * 0.5f);
            s_dry_r[i] += (s_fm * 0.7f + s_sub * 0.5f);
        }
    }

    /* 8. Process Master Effects: Reverb and Tape Delay */
    fxbus_process_st(&g_fxbus, s_dry_l, s_dry_r, s_rev, s_dly, s_fx_l, s_fx_r, (int)n);

    /* 9. Master Dynamics: Glue Bus Compressor and Limiter */
    master_process_st(&g_master, s_fx_l, s_fx_r, (int)n, 0.95f);

    /* 10. Write final master out into int32 DAC frames */
    for (i = 0; i < n; i++) {
        int32_t val_l = (int32_t)(s_fx_l[i] * 30000.0f);
        int32_t val_r = (int32_t)(s_fx_r[i] * 30000.0f);
        if (val_l > 32767) val_l = 32767; else if (val_l < -32767) val_l = -32767;
        if (val_r > 32767) val_r = 32767; else if (val_r < -32767) val_r = -32767;

        out[2u * i]     += val_l;
        out[2u * i + 1] += val_r;
    }
}
