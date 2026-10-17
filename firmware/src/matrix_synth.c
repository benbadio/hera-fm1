/* SPDX-License-Identifier: GPL-3.0-only */
/* MORPHEUS: Multi-Engine Digital Synthesizer Implementation */
#include "matrix_synth.h"
#include "matrix_core.h"

matrix_synth_t g_synth;

/* MIDI pitch increment table for C9..B9 (MIDI 120..131) at 44.1 kHz */
/* inc = f * 2^32 / 44100 */
static const uint32_t INC_C9[12] = {
    815363807u,  863847862u,  915214929u,  969636441u,
    1027294024u, 1088380105u, 1153098554u, 1221665363u,
    1294309365u, 1371273005u, 1452813141u, 1539201906u
};

/* Fast refined parabola sine approximation: 0..2^32 -> -32767..32767 */
static int32_t fast_sine(uint32_t ph)
{
    int32_t x = (int32_t)ph >> 16, ax = x < 0 ? -x : x, y, ay;
    y = (x * (32768 - ax)) >> 13;               /* 4x(1-|x|), Q15 */
    ay = y < 0 ? -y : y;
    y += ((((y * ay) >> 15) - y) * 7373) >> 15; /* + 0.225 (y|y| - y) */
    if (y > 32767) return 32767;
    if (y < -32767) return -32767;
    return y;
}

static int32_t soft_clip(int32_t x)
{
    if (x > 24000) return 24000 + ((x - 24000) >> 2);
    if (x < -24000) return -24000 + ((x + 24000) >> 2);
    return x;
}

void matrix_synth_init(void)
{
    memset(&g_synth, 0, sizeof(g_synth));
    g_synth.knob_cutoff = 80;
    g_synth.knob_resonance = 95; /* classic 303 squelch */
    g_synth.knob_decay = 70;
    g_synth.wave_type = 0;       /* Saw */
    g_synth.engine_mode = SOUND_303;
    g_synth.glide_speed = 32;
}

void matrix_synth_note_on(uint8_t note, uint8_t velocity, uint8_t is_tied)
{
    if (note > 127) return;

    uint32_t inc = INC_C9[note % 12u] >> (10u - (note / 12u));
    g_synth.target_inc = inc;
    g_synth.gate_active = 1;
    g_synth.is_accent = (velocity >= 105);

    if (!is_tied || g_synth.amp_level == 0) {
        /* New note: instant pitch jump and envelope retrigger */
        g_synth.cur_inc = inc;
        g_synth.amp_level = (int32_t)velocity * 240; /* 0..30480 */
        g_synth.flt_level = g_synth.is_accent ? 32000 : 22000;
    } else {
        /* Tied note: slide pitch smoothly (acid slide!), maintain envelope */
        if (g_synth.amp_level < 16000) g_synth.amp_level = 20000;
        if (g_synth.is_accent) g_synth.flt_level = 28000;
    }
}

void matrix_synth_note_off(void)
{
    g_synth.gate_active = 0;
}

void matrix_synth_render(int32_t *out, uint32_t n)
{
    if (g_synth.engine_mode == SOUND_MUTED) {
        return; /* Zero audio output */
    }

    uint32_t i;
    int32_t amp = g_synth.amp_level;
    int32_t flt_env = g_synth.flt_level;
    uint32_t ph = g_synth.cur_phase;
    uint32_t inc = g_synth.cur_inc;
    uint32_t target_inc = g_synth.target_inc;
    int32_t f_low = g_synth.f_low;
    int32_t f_band = g_synth.f_band;

    /* Calculate filter coefficient from Cutoff knob + Envelope + Curve modulation */
    int32_t cutoff_base = (int32_t)g_synth.knob_cutoff * 100; /* 0..12700 */
    int32_t curve_mod = (int32_t)g_synth.curve_mod * 80;      /* Modulation */
    int32_t q = 32767 - ((int32_t)g_synth.knob_resonance * 240);
    if (q < 1200) q = 1200; /* High resonance cap */

    /* Decay parameters */
    int32_t amp_step = 18 + ((127 - g_synth.knob_decay) >> 2);
    int32_t flt_step = 28 + ((127 - g_synth.knob_decay) >> 1);
    if (g_synth.is_accent) flt_step >>= 1; /* Longer decay on accent */

    for (i = 0; i < n; i++) {
        /* 1. Portamento glide toward target pitch */
        if (inc < target_inc) {
            inc += 3000u;
            if (inc > target_inc) inc = target_inc;
        } else if (inc > target_inc) {
            inc -= 3000u;
            if (inc < target_inc) inc = target_inc;
        }

        /* 2. Advance Phase */
        ph += inc;

        /* 3. Generate Raw Oscillator waveform */
        int32_t raw_osc;
        if (g_synth.engine_mode == SOUND_303) {
            if (g_synth.wave_type == 0) {
                /* Sawtooth: ramp -32767..+32767 */
                raw_osc = (int32_t)(ph >> 16) - 32768;
            } else {
                /* 303-style asymmetric square */
                raw_osc = (ph < 0x74000000u) ? 22000 : -22000;
            }
        } else if (g_synth.engine_mode == SOUND_SUBTRACTIVE) {
            /* Dual osc: Saw + Detuned Sub/PWM */
            int32_t saw = (int32_t)(ph >> 16) - 32768;
            uint32_t ph2 = ph + (ph >> 3);
            int32_t sq = (ph2 < 0x80000000u) ? 16000 : -16000;
            raw_osc = (saw * 3 + sq * 2) / 5;
        } else {
            /* FM Engine: 2-operator frequency modulation */
            uint32_t mod_ph = ph * 2u;
            int32_t mod_val = fast_sine(mod_ph);
            uint32_t carrier_ph = ph + ((uint32_t)(mod_val * (cutoff_base >> 6)));
            raw_osc = fast_sine(carrier_ph);
        }

        /* 4. Filter cutoff calculation per sample */
        int32_t f_coeff = cutoff_base + ((flt_env * 70) >> 8) + curve_mod;
        if (f_coeff < 400) f_coeff = 400;
        if (f_coeff > 15500) f_coeff = 15500;

        /* 5. 24dB / Resonant State Variable Filter */
        f_low += (f_coeff * f_band) >> 15;
        int32_t high = raw_osc - f_low - ((q * f_band) >> 15);
        f_band += (f_coeff * high) >> 15;

        /* Resonance soft saturation (produces 303 acid warmth) */
        f_band = soft_clip(f_band);

        int32_t filtered = f_low;
        if (g_synth.is_accent) {
            /* Accent punch / overdrive */
            filtered = (filtered * 5) >> 2;
            filtered = soft_clip(filtered);
        }

        /* 6. VCA Amp Envelope */
        int32_t sample = (filtered * amp) >> 15;

        /* 7. Envelope Decay Update */
        if (!g_synth.gate_active) {
            /* Fast release after note off */
            amp -= 60;
            if (amp < 0) amp = 0;
            flt_env -= 90;
            if (flt_env < 0) flt_env = 0;
        } else {
            /* Decay phase */
            if (amp > 1000) amp -= amp_step;
            if (flt_env > 200) flt_env -= flt_step;
        }

        /* 8. Output stereo frames (L, R) */
        out[2u * i] += sample;
        out[2u * i + 1u] += sample;
    }

    g_synth.amp_level = amp;
    g_synth.flt_level = flt_env;
    g_synth.cur_phase = ph;
    g_synth.cur_inc = inc;
    g_synth.f_low = f_low;
    g_synth.f_band = f_band;
}
