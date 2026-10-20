/* SPDX-License-Identifier: GPL-3.0-only */
/* j6_synth_state.h - private state of the synth module (j6_synth.c, j6_synth_fx.c, j6_synth_sounds.c).
 *
 * OWNER: the synth module. Only j6_synth*.c, tests/j6/test_synth.c and the glue (to allocate the
 * instance) include this file. Nobody outside the synth module reads these fields; they use the
 * j6_synth_* API. The delay, reverb and chorus sample memory is NOT in here: it is the J6_FX_MEM_LEN
 * int16_t pool array the glue passes to j6_synth_init().
 *
 * Number formats: oscillators Q15 from 32-bit phases; filter states Q23 (Q15 x 256) with Q30
 * coefficients (int64 products); envelope Q30; gains Q15 (vOL Q12); output Q15 in int32_t.
 *
 * fx_mem layout (J6_FX_MEM_LEN = 81920 int16_t):
 *   [0, 65536)          delay line, mono, Q14 (a power of two: J6_FX_DELAY_LEN)
 *   [65536, 77542)      reverb: 8 combs (4 per side) then 4 allpasses (2 per side), J6Y_RV_LEN, Q11
 *   [77542, 78566)      chorus line, mono, Q14, J6Y_CH_LEN
 *   [78566, 81920)      unused
 * A shorter fx_len keeps the reverb and the chorus and gives the delay the largest power of two left;
 * below J6Y_RV_LEN + J6Y_CH_LEN + 4096 the reverb and the chorus are off and the delay takes it all. */
#ifndef J6_SYNTH_STATE_H
#define J6_SYNTH_STATE_H

#include "j6.h"

#define J6Y_CR_SHIFT 4u                  /* log2(J6_CR): per-chunk linear ramps divide by J6_CR as >> this */
#define J6Y_CH_LEN 1024u                 /* chorus line (power of two) */
#define J6Y_FADE_CHUNKS 4u               /* steal fade: 4 x 16 frames = 1.45 ms (SD-5) */
#define J6Y_FLT_LIM (1 << 25)            /* filter state clamp, Q23: 4x full scale */

/* every per-chunk ramp (voice gain, vOL, chorus taps and mix, delay read, wet levels) is a linear
 * interpolation over J6_CR frames done with >> J6Y_CR_SHIFT: they only end exactly on their target when
 * J6_CR is that power of two. The generated tables say which J6_CR they were computed for (j6_synth.c). */
J6_STATIC_ASSERT(synth_chunk_is_pow2, J6_CR == (1u << J6Y_CR_SHIFT));

/* voice stages */
enum {
    J6Y_ST_IDLE = 0,
    J6Y_ST_ATTACK,
    J6Y_ST_DECAY,                        /* decay, then sustain (the same exponential) */
    J6Y_ST_RELEASE,
    J6Y_ST_FADE                          /* stolen: fading out, then `note` starts (if still gated) */
};

/* one voice: 32-bit phase oscillators, a 4-pole resonant low-pass (2 TPT SVF sections), an ADSR */
struct j6y_voice {
    uint32_t ph;                         /* main phase: saw, pulse (ph + pw offset), sub (toggles on wrap) */
    uint32_t ph2;                        /* detuned second saw */
    uint32_t inc, inc2;                  /* phase increments (tuned) */
    uint32_t rcp, rcp2;                  /* polyBLEP: 2^31 / (inc >> rsh), so t/dt is a multiply */
    int32_t f[4];                        /* filter states Q23: section 1 ic1, ic2; section 2 ic1, ic2 */
    int32_t env;                         /* ADSR (amplitude and filter), Q30, 0 .. 1 */
    int32_t amp;                         /* output gain now (env x velocity x level), Q15 */
    int32_t fade_dec;                    /* steal fade: gain decrement per chunk */
    int32_t vel_q15;                     /* velocity gain */
    uint32_t age;                        /* note-on order, for stealing the oldest (SD-5) */
    uint8_t rsh, rsh2;
    uint8_t note, vel;                   /* in J6Y_ST_FADE: the note that starts after the fade */
    uint8_t snote;                       /* the note sounding now (the old one during a steal fade) */
    uint8_t stage;                       /* J6Y_ST_* */
    uint8_t gate;                        /* key held */
    uint8_t sub;                         /* sub oscillator: 1 = high half */
    uint8_t fade;                        /* steal fade chunks left */
    int32_t hp, bb;                      /* Juno HPF: the high-pass's low-pass state, the bass boost's */
};

/* the coefficients of one TPT state-variable section (per chunk) */
struct j6y_svf {
    int32_t a1, a2, a3;                  /* Q30 */
};

/* delay (SD-4), reverb and chorus, on the pool memory */
struct j6y_fx {
    int16_t *dly;                        /* delay line, mono, Q14, length dly_len (power of two) */
    uint32_t dly_len, dly_mask, dly_w;
    int32_t dly_cur_q8, dly_tgt_q8;      /* delay time now / target, samples Q8 (slewed, no clicks) */
    int32_t dly_lp;                      /* darkening one-pole in the feedback, Q22 (Q14 x 256) */
    uint32_t dly_quiet;                  /* samples since the last non-zero write */
    int16_t *comb[8];                    /* reverb combs (0..3 left, 4..7 right), Q11 */
    int16_t *ap[4];                      /* allpasses (0..1 left, 2..3 right) */
    uint16_t comb_pos[8], ap_pos[4];
    int32_t comb_lp[8];                  /* comb damping states, Q19 (Q11 x 256) */
    uint32_t rv_quiet;
    int16_t *ch;                         /* chorus line, mono, Q14 */
    uint32_t ch_w, ch_quiet, ch_lfo;
    int32_t ch_mix;                      /* chorus wet amount now, Q15, ramps to the sound's flag */
    int32_t ch_dl_q8, ch_dr_q8;          /* chorus delays now (left, right), samples Q8 */
    /* DLY_LEVEL / REV_LEVEL now (they ramp to the knob, so a level change never cuts a tail in one
     * sample): the wet gain, and the input gain Q15 (on while the level is not 0) */
    int32_t dly_lvl, dly_in;
    int32_t rv_lvl, rv_in;
};

struct j6_synth_s {
    struct j6y_voice v[J6_VOICES];
    struct j6y_fx fx;
    const j6_sound_t *snd;               /* current preset */
    int32_t param[J6_SP_COUNT];          /* J6_SP_* values as set (clamped) */
    uint32_t tune_q16;                   /* tUnE factor (SD-8) */
    int32_t vol_q12, vol_cur_q12;        /* vOL gain target / now */
    uint32_t kill;                       /* j6_synth_kill: chunks left in the fade-out (0 = off) */
    int32_t g_saw, g_pulse, g_sub, g_noise;   /* oscillator mix of the sound, Q15, sum <= 1 */
    uint32_t pw_off;                     /* pulse: phase offset of the second saw (duty) */
    uint32_t det_q16;                    /* detuned saw: increment ratio, Q16 (0 = off) */
    int32_t lvl_q15;                     /* voice level of the sound (1 voice at level 127 = -12 dBFS) */
    uint32_t age;                        /* note-on counter */
    uint32_t noise;                      /* xorshift32 noise state */
    uint8_t fx_on, rv_on;                /* 1 = fx_mem given; 1 = room for the reverb and the chorus */
};

J6_STATIC_ASSERT(synth_fits_budget, sizeof(struct j6_synth_s) <= J6_RAM_SYNTH_MAX);

/* ---- module-internal functions (j6_synth.c <-> j6_synth_fx.c; tests may call them) ---------------- */

/* SVF coefficients for g = tan(pi fc / fs) and damping k (both Q20). Stable for any g, k > 0: a2 is
 * computed first and a1, a3 are derived from it, so the section's determinant stays below 1. */
void j6y_svf_coefs(struct j6y_svf *c, int32_t g_q20, int32_t k_q20);
/* one sample through a section; st[0..1] are its states (Q23, clamped to +-J6Y_FLT_LIM); returns low-pass */
static inline int32_t j6y_svf_tick(const struct j6y_svf *c, int32_t *st, int32_t x)
{
    int32_t s1 = st[0], s2 = st[1], v3 = x - s2, v1, v2;
    v1 = (int32_t)(((int64_t)c->a1 * s1 + (int64_t)c->a2 * v3) >> 30);
    v2 = s2 + (int32_t)(((int64_t)c->a2 * s1 + (int64_t)c->a3 * v3) >> 30);
    s1 = 2 * v1 - s1;
    s2 = 2 * v2 - s2;
    st[0] = s1 > J6Y_FLT_LIM ? J6Y_FLT_LIM : s1 < -J6Y_FLT_LIM ? -J6Y_FLT_LIM : s1;
    st[1] = s2 > J6Y_FLT_LIM ? J6Y_FLT_LIM : s2 < -J6Y_FLT_LIM ? -J6Y_FLT_LIM : s2;
    return v2;
}

/* load a sound (j6_synth_sound with any j6_sound_t, e.g. a test sound): its knob values become the 8
 * knob params, its oscillator mix applies to the sounding voices too */
void j6y_sound_load(j6_synth_t *s, const j6_sound_t *snd);

/* effects (j6_synth_fx.c) */
void j6y_fx_init(j6_synth_t *s, int16_t *mem, uint32_t len);   /* layout, zero the memory */
void j6y_fx_clear(j6_synth_t *s);                              /* lines zero, idle (panic) */
/* delay target from the params; whatever cannot be heard yet (an empty line, no voice) jumps to its target */
void j6y_fx_params(j6_synth_t *s);
/* the chorus: one chunk of mono dry voices -> stereo lr (L R ...), J6_CR frames */
void j6y_fx_chorus(j6_synth_t *s, const int32_t *mono, int32_t *lr);
/* delay then reverb on one chunk of stereo (post vOL), in place, J6_CR frames */
void j6y_fx_chunk(j6_synth_t *s, int32_t *lr);
/* a chunk with no input while idle: keep the per-chunk smoothing where it would be */
void j6y_fx_skip(j6_synth_t *s);

#endif /* J6_SYNTH_STATE_H */
