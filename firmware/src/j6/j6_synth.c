/* SPDX-License-Identifier: GPL-3.0-only */
/* j6_synth.c - the synth module's voices: a placeholder "Juno-ish" 4-voice synth (SD-5, SD-6, SD-8).
 * docs/J6_ENGINE_DESIGN.md 7.1. The effects are in j6_synth_fx.c, the presets in j6_synth_sounds.c.
 *
 * One voice: a saw and a pulse from one 32-bit phase (polyBLEP edges), a square sub one octave down that
 * toggles on the main phase's wrap (so its edges get the same BLEP), noise, an optional detuned second
 * saw; a 4-pole low-pass made of two TPT state-variable sections (the first resonant); one ADSR for the
 * amplitude and the filter. Everything per chunk (J6_CR frames): envelope segment, gain ramp, filter
 * coefficients. Per sample: oscillators, filter, the linear gain ramp. No division per sample.
 *
 * Signal path (SD-7): voices -> [chorus] -> vOL -> delay -> reverb -> ADD into out, clamped. */
#include "j6.h"
#include "j6_synth_state.h"
#include "j6_synth_tables.h"

#define J6Y_ONE (1 << 30)                /* envelope full scale, Q30 */
#define J6Y_SUS_K (J6Y_ONE / 127)        /* sustain 0..127 -> Q30 */
#define J6Y_VOICE_FS 8192                /* one voice at velocity 127, level 127: -12 dBFS (design 7.1) */
#define J6Y_VOICE_CLIP 65535             /* a voice's filtered signal is clamped to +-2 FS before its gain */
#define J6Y_ENV_CUT_Q8 336               /* filter envelope depth: +-84 semitones at env_amt 0 / 127 */
#define J6Y_DETUNE_STEP 8u               /* detune 0..127 -> ratio 1 + 8/65536 per unit (127: +26.6 cents) */
#define J6Y_VOL_STEP 256                 /* vOL ramp: at most 1/16 of unity per chunk */

/* the envelope, release and chorus LFO coefficients are per chunk: made for this J6_CR */
J6_STATIC_ASSERT(synth_tables_chunk, J6Y_TABLES_CR == J6_CR);

static uint32_t j6y_u7(uint32_t x) { return x > 127u ? 127u : x; }

/* ---- pitch ----------------------------------------------------------------------------------------- */

/* polyBLEP reciprocal: t/dt = ((ph >> rsh) * rcp) >> 16 in Q15 for ph < inc, with one 32-bit division */
static void j6y_rcp(uint32_t inc, uint32_t *rcp, uint8_t *rsh)
{
    uint32_t sh = 0, d;
    while ((inc >> sh) >= 65536u) sh++;
    d = inc >> sh;
    if (d == 0u) d = 1u;
    *rcp = 0x7FFFFFFFu / d;              /* (ph >> sh) <= d, so the product stays below 2^31 */
    *rsh = (uint8_t)sh;
}

static void j6y_pitch(const j6_synth_t *s, struct j6y_voice *v)
{
    uint32_t inc = (uint32_t)(((uint64_t)j6y_inc[v->note & 127u] * s->tune_q16) >> 16);
    if (inc < 65536u) inc = 65536u;
    v->inc = inc;
    j6y_rcp(inc, &v->rcp, &v->rsh);
    v->inc2 = s->det_q16 ? (uint32_t)(((uint64_t)inc * s->det_q16) >> 16) : inc;
    j6y_rcp(v->inc2, &v->rcp2, &v->rsh2);
}

/* ---- the sound's oscillator mix ------------------------------------------------------------------ */

void j6y_sound_load(j6_synth_t *s, const j6_sound_t *p)
{
    uint32_t saw = j6y_u7(p->saw), pul = j6y_u7(p->pulse), sub = j6y_u7(p->sub), nz = j6y_u7(p->noise);
    uint32_t sum = saw + pul + sub + nz, den = sum > 127u ? sum : 127u;
    uint32_t pw = j6y_u7(p->pw), dev = pw > 64u ? pw - 64u : 64u - pw;
    uint32_t i, k;
    s->snd = p;
    for (k = 0; k < J6_KNOBS; k++) s->param[k] = (int32_t)j6y_u7(p->knob[k]);   /* SD-3 */
    /* the four gains add up to at most 1: the mix never exceeds one oscillator's full scale */
    s->g_saw = (int32_t)(saw * 32768u / den);
    s->g_pulse = (int32_t)(pul * 32768u / den);
    s->g_sub = (int32_t)(sub * 32768u / den);
    s->g_noise = (int32_t)(nz * 32768u / den);
    s->pw_off = 0x80000000u - dev * J6Y_PW_STEP;          /* duty 50 % (pw 64) .. 5 % (pw 0 / 127) */
    s->det_q16 = p->detune ? 65536u + j6y_u7(p->detune) * J6Y_DETUNE_STEP : 0u;
    s->lvl_q15 = (int32_t)(j6y_u7(p->level) * (uint32_t)J6Y_VOICE_FS / 127u);
    for (i = 0; i < J6_VOICES; i++)
        if (s->v[i].stage != J6Y_ST_IDLE && s->v[i].stage != J6Y_ST_FADE) j6y_pitch(s, &s->v[i]);
    j6y_fx_params(s);
}

/* ---- API: setup and parameters ------------------------------------------------------------------- */

void j6_synth_init(j6_synth_t *s, int16_t *fx_mem, uint32_t fx_len)
{
    unsigned char *b = (unsigned char *)s;
    size_t i;
    for (i = 0; i < sizeof *s; i++) b[i] = 0;
    s->noise = 0x2545F491u;
    s->param[J6_SP_VOLUME] = (int32_t)J6_VOL_DEF;
    s->param[J6_SP_DLY_SYNC] = 0;
    s->param[J6_SP_TEMPO10] = (int32_t)J6_TEMPO10_DEF;
    s->param[J6_SP_TUNE10] = (int32_t)J6_TUNE10_DEF;
    s->tune_q16 = 65536u;
    s->vol_q12 = 4096;
    s->vol_cur_q12 = 4096;
    s->snd = &j6_sounds[0];
    j6y_fx_init(s, fx_mem, fx_len);
    j6_synth_sound(s, 0);
}

void j6_synth_sound(j6_synth_t *s, uint32_t sound)
{
    if (sound >= J6_SOUNDS) sound = J6_SOUNDS - 1u;
    j6y_sound_load(s, &j6_sounds[sound]);
}

void j6_synth_set(j6_synth_t *s, uint32_t param, int32_t v)
{
    uint32_t i;
    if (param >= J6_SP_COUNT) return;
    switch (param) {
    case J6_SP_DLY_SYNC:
        v = v ? 1 : 0;
        break;
    case J6_SP_TEMPO10:
        v = j6_clamp(v, (int32_t)J6_TEMPO10_MIN, (int32_t)J6_TEMPO10_MAX);
        break;
    case J6_SP_TUNE10:
        v = j6_clamp(v, (int32_t)J6_TUNE10_MIN, (int32_t)J6_TUNE10_MAX);
        break;
    default:                             /* the 8 knobs and vOL */
        v = j6_clamp(v, 0, 127);
        break;
    }
    s->param[param] = v;
    if (param == J6_SP_VOLUME) {
        s->vol_q12 = v * v * 4096 / 10000;                   /* SD-7: 100 = unity, 127 = +4.1 dB */
        if (!j6_synth_active(s) && j6_synth_fx_idle(s)) s->vol_cur_q12 = s->vol_q12;   /* silent: no ramp */
    } else if (param == J6_SP_TUNE10) {
        s->tune_q16 = ((uint32_t)v * 65536u + 2200u) / 4400u;   /* SD-8 */
        for (i = 0; i < J6_VOICES; i++)
            if (s->v[i].stage != J6Y_ST_IDLE && s->v[i].stage != J6Y_ST_FADE) j6y_pitch(s, &s->v[i]);
    } else if (param == J6_SP_DLY_TIME || param == J6_SP_DLY_SYNC || param == J6_SP_TEMPO10 ||
               param == J6_SP_DLY_LEVEL || param == J6_SP_REV_LEVEL) {
        j6y_fx_params(s);
    }
}

int32_t j6_synth_get(const j6_synth_t *s, uint32_t param)
{
    return param < J6_SP_COUNT ? s->param[param] : 0;
}

/* ---- API: notes (SD-5) ---------------------------------------------------------------------------- */

/* a fresh start: silent voice, so the filter and the envelope begin from zero */
static void j6y_voice_begin(j6_synth_t *s, struct j6y_voice *v)
{
    v->f[0] = v->f[1] = v->f[2] = v->f[3] = 0;
    v->env = 0;
    v->amp = 0;
    v->stage = J6Y_ST_ATTACK;
    v->vel_q15 = (int32_t)v->vel * 258;
    v->snote = v->note;
    j6y_pitch(s, v);
}

void j6_synth_note_on(j6_synth_t *s, uint32_t note, uint32_t vel)
{
    struct j6y_voice *v = NULL;
    uint32_t i;
    int steal = 0, fresh = 0;
    if (note > 127u) return;
    if (vel == 0u) {                     /* MIDI convention: velocity 0 is a note-off */
        j6_synth_note_off(s, note);
        return;
    }
    if (vel > 127u) vel = 127u;
    for (i = 0; i < J6_VOICES && !v; i++)            /* 1. the same note again: its own voice */
        if (s->v[i].stage != J6Y_ST_IDLE && s->v[i].note == note) v = &s->v[i];
    for (i = 0; i < J6_VOICES && !v; i++)            /* 2. an idle voice */
        if (s->v[i].stage == J6Y_ST_IDLE) {
            v = &s->v[i];
            fresh = 1;
        }
    if (!v) {                                        /* 3. the quietest releasing voice */
        int32_t best = 0x7FFFFFFF;
        for (i = 0; i < J6_VOICES; i++) {
            struct j6y_voice *w = &s->v[i];
            if (!w->gate && (w->stage == J6Y_ST_RELEASE || w->stage == J6Y_ST_FADE) && w->amp < best) {
                best = w->amp;
                v = w;
            }
        }
    }
    if (!v) {                                        /* 4. the oldest gated voice, with a short fade */
        uint32_t oldest = 0;
        for (i = 0; i < J6_VOICES; i++)
            if (!v || s->age - s->v[i].age > oldest) {
                oldest = s->age - s->v[i].age;
                v = &s->v[i];
            }
        steal = 1;
    }
    s->age++;
    v->age = s->age;
    v->note = (uint8_t)note;
    v->vel = (uint8_t)vel;
    v->gate = 1;
    if (v->stage == J6Y_ST_FADE) return;             /* already fading: the new note starts after it */
    if (fresh) {
        j6y_voice_begin(s, v);
    } else if (steal) {
        v->stage = J6Y_ST_FADE;
        v->fade = (uint8_t)J6Y_FADE_CHUNKS;
        v->fade_dec = v->amp / (int32_t)J6Y_FADE_CHUNKS + 1;
    } else {                                         /* retrigger: the envelope rises from where it is */
        v->stage = J6Y_ST_ATTACK;
        v->vel_q15 = (int32_t)vel * 258;
        v->snote = v->note;
        j6y_pitch(s, v);
    }
}

void j6_synth_note_off(j6_synth_t *s, uint32_t note)
{
    uint32_t i;
    for (i = 0; i < J6_VOICES; i++) {
        struct j6y_voice *v = &s->v[i];
        if (v->stage == J6Y_ST_IDLE || v->note != note || !v->gate) continue;
        v->gate = 0;
        if (v->stage == J6Y_ST_ATTACK || v->stage == J6Y_ST_DECAY) v->stage = J6Y_ST_RELEASE;
    }
}

void j6_synth_all_off(j6_synth_t *s)
{
    uint32_t i;
    for (i = 0; i < J6_VOICES; i++) {
        struct j6y_voice *v = &s->v[i];
        v->gate = 0;
        if (v->stage == J6Y_ST_ATTACK || v->stage == J6Y_ST_DECAY) v->stage = J6Y_ST_RELEASE;
    }
}

#define J6Y_KILL_CHUNKS 16u              /* kill fade: 16 x 16 frames = 5.8 ms (D2) */

void j6_synth_kill(j6_synth_t *s)
{
    if (!s->kill) s->kill = J6Y_KILL_CHUNKS;
}

void j6_synth_panic(j6_synth_t *s)
{
    uint32_t i;
    for (i = 0; i < J6_VOICES; i++) {
        struct j6y_voice *v = &s->v[i];
        v->f[0] = v->f[1] = v->f[2] = v->f[3] = 0;
        v->env = 0;
        v->amp = 0;
        v->stage = J6Y_ST_IDLE;
        v->gate = 0;
        v->fade = 0;
    }
    s->vol_cur_q12 = s->vol_q12;
    s->kill = 0;
    j6y_fx_clear(s);
}

uint32_t j6_synth_active(const j6_synth_t *s)
{
    uint32_t i, n = 0;
    for (i = 0; i < J6_VOICES; i++)
        if (s->v[i].stage != J6Y_ST_IDLE) n++;
    return n;
}

/* ---- the filter coefficients ------------------------------------------------------------------------ */

void j6y_svf_coefs(struct j6y_svf *c, int32_t g_q20, int32_t k_q20)
{
    /* TPT SVF: a1 = 1/(1 + g(g+k)), a2 = g a1, a3 = g a2. a2 comes from one normalised 32-bit reciprocal;
     * a3 = g a2 and a1 = 1 - a3 - k a2 are derived from it, which keeps the section stable whatever
     * the rounding (the determinant is 1 - 2 k a2 + O(g eps)). */
    uint32_t den, d, r, sh = 0;
    int32_t a2;
    if (g_q20 < 1) g_q20 = 1;
    if (k_q20 < 1) k_q20 = 1;
    den = (1u << 20) + (uint32_t)(((uint64_t)(uint32_t)g_q20 * (uint32_t)(g_q20 + k_q20)) >> 20);
    while ((den >> sh) >= 65536u) sh++;
    d = den >> sh;                       /* 2^15 .. 2^16 */
    r = 0x7FFFFFFFu / d;
    {   /* g / den in Q30: (g r) >> (1 + sh), 1 + sh in 6 .. 17 (den >= 2^20, den < 2^32). Only the low 32
         * bits are kept, so the variable shift is done on 32-bit halves: a 64-bit shift by a variable
         * count may be a libcall (__lshrdi3) that the firmware does not link. */
        uint64_t prod = (uint64_t)(uint32_t)g_q20 * r;
        uint32_t s = 1u + sh;
        a2 = (int32_t)(((uint32_t)prod >> s) | ((uint32_t)(prod >> 32) << (32u - s)));
    }
    c->a2 = a2;
    c->a3 = (int32_t)(((int64_t)g_q20 * a2) >> 20);
    c->a1 = J6Y_ONE - c->a3 - (int32_t)(((int64_t)k_q20 * a2) >> 20);
}

/* ---- one chunk of one voice ---------------------------------------------------------------------- */

/* the envelope at the end of this chunk (exponential segment, Q30); may move to the next stage */
static int32_t j6y_env_step(const j6_synth_t *s, struct j6y_voice *v)
{
    int32_t e = v->env, tgt;
    uint32_t c;
    switch (v->stage) {
    case J6Y_ST_ATTACK:
        tgt = (int32_t)J6Y_ATT_TGT;
        c = j6y_att_c[s->param[J6_SP_ATTACK]];
        e = tgt + (int32_t)(((int64_t)(e - tgt) * c) >> 30);
        if (e >= J6Y_ONE) {
            e = J6Y_ONE;
            v->stage = J6Y_ST_DECAY;
        }
        break;
    case J6Y_ST_DECAY:
        tgt = (int32_t)j6y_u7(s->snd->sustain) * J6Y_SUS_K;
        c = j6y_dec_c[j6y_u7(s->snd->decay)];
        e = tgt + (int32_t)(((int64_t)(e - tgt) * c) >> 30);
        break;
    case J6Y_ST_RELEASE:
        tgt = J6Y_REL_TGT;
        c = j6y_rel_c[s->param[J6_SP_RELEASE]];
        e = tgt + (int32_t)(((int64_t)(e - tgt) * c) >> 30);
        if (e < 0) e = 0;                /* reaches exactly 0 in finite time */
        break;
    default:
        break;
    }
    return e;
}

/* polyBLEP correction near an edge, Q15: (1 - t/dt)^2 for a phase p (or 2^32 - p) below inc */
/* ---- the Juno ladder (after Clipper, Copyright (C) 2026 3dSam, GPL-3.0: dsp.c ladder_coef / ladder_tick,
 * after V. Zavalishin's TPT ladder): four trapezoidal one-poles, the feedback solved in the same sample, a
 * tanh at the input (the differential pair); the passband partly kept at high resonance. Signals Q12 inside,
 * coefficients Q16; 64-bit products (multiplies only: no 64-bit division on the chip). */
struct j6y_ladder {
    int32_t G, H, G4, inv, k;            /* G, 1 - G, G^4, 1 / (1 + k G^4): Q16; k: Q12 */
};
static int32_t j6y_mul_sh16(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 16); }
static int32_t j6y_mul_sh12(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 12); }
static int32_t j6y_softclip(int32_t x)   /* tanh(x / 32768) * 32767, x up to +-65536 */
{
    int32_t a = x < 0 ? -x : x, i = a >> 8, y;
    if (i >= 256)
        y = j6y_tanh_q15[256];
    else
        y = j6y_tanh_q15[i] + ((((int32_t)j6y_tanh_q15[i + 1] - (int32_t)j6y_tanh_q15[i]) * (a & 255)) >> 8);
    return x < 0 ? -y : y;
}
static void j6y_ladder_coef(struct j6y_ladder *c, int32_t g_q12, uint32_t res)
{
    int32_t g = j6_clamp(g_q12, 1, 3 * 4096), G, G2;
    G = (int32_t)(((uint32_t)g << 16) / (uint32_t)(4096 + g));     /* g / (1 + g) */
    G2 = j6y_mul_sh16(G, G);
    c->G = G;
    c->H = 65536 - G;
    c->G4 = j6y_mul_sh16(G2, G2);
    c->k = (int32_t)(res > 127u ? 127u : res) * 16900 / 127;      /* 0 .. 4.13: from ~4 it sings */
    c->inv = (int32_t)(0xFFFFFFFFu / (uint32_t)(65536 + j6y_mul_sh12(c->k, c->G4)));
}
static int32_t j6y_ladder_tick(const struct j6y_ladder *c, int32_t x, int32_t *s)
{
    int64_t in = (int64_t)j6_clamp(x, -131072, 131072) * 4096, S, y, u;
    int32_t G = c->G, uq, v, j;
    in += (in * c->k) >> 13;             /* x (1 + k / 2) */
    S = (((int64_t)G * s[0]) >> 16) + s[1];
    S = (((int64_t)G * S) >> 16) + s[2];
    S = (((int64_t)G * S) >> 16) + s[3];
    S = ((int64_t)c->H * S) >> 16;
    y = (((((int64_t)c->G4 * in) >> 16) + S) * c->inv) >> 16;
    u = in - ((c->k * y) >> 12);
    {
        int64_t h = u >> 13;
        if (h <= -65536 || h >= 65536)
            uq = j6y_softclip(h < 0 ? -65536 : 65536) * 8192;
        else
            uq = j6y_softclip((int32_t)h) * 8192 + (int32_t)(u - h * 8192);
    }
    for (j = 0; j < 4; j++) {
        v = (int32_t)((((int64_t)G * (uq - s[j])) + 32768) >> 16);
        uq = v + s[j];
        s[j] = uq + v;
    }
    return (uq + 2048) >> 12;
}

static int32_t j6y_blep(uint32_t p, uint32_t rsh, uint32_t rcp)
{
    int32_t d = 32768 - (int32_t)(((p >> rsh) * rcp) >> 16);
    return (d * d) >> 15;
}

static void j6y_voice_chunk(j6_synth_t *s, struct j6y_voice *v, int32_t *mix)
{
    const j6_sound_t *snd = s->snd;
    int32_t env = v->env, amp0 = v->amp, amp1, ddiff, acc = 0, cut, g, gain_in, gsaw, gpul, gsub, gnz;
    uint32_t i, res = (uint32_t)s->param[J6_SP_RESO], idx, fr;
    uint32_t ph = v->ph, inc = v->inc, rcp = v->rcp, rsh = v->rsh;
    uint32_t ph2 = v->ph2, inc2 = v->inc2, rcp2 = v->rcp2, rsh2 = v->rsh2;
    uint32_t pwo = s->pw_off, nz = s->noise, sub = v->sub, det = s->det_q16;
    struct j6y_ladder lad;
    uint32_t hpf = (snd->flags & J6_SND_HPF_MASK) >> J6_SND_HPF_SHIFT;
    int32_t hp = v->hp, bb = v->bb;
    int32_t *f = v->f;

    /* envelope / steal fade, and the gain at the end of the chunk */
    if (v->stage == J6Y_ST_FADE) {
        amp1 = amp0 - v->fade_dec;
        if (amp1 < 0) amp1 = 0;
    } else {
        env = j6y_env_step(s, v);
        amp1 = (int32_t)((((((uint32_t)env >> 15) * (uint32_t)v->vel_q15) >> 15) * (uint32_t)s->lvl_q15) >> 15);
    }

    /* cutoff = knob + env_amt x envelope + key tracking, in Q8 semitones -> g (design 7.1); the tracking
     * follows the sounding note, so a steal fade keeps the old note's filter (not the pending note's) */
    cut = (int32_t)j6y_cut_knob[s->param[J6_SP_CUTOFF]] +
          (((env >> 15) * ((int32_t)j6y_u7(snd->env_amt) - 64) * J6Y_ENV_CUT_Q8) >> 15) +
          ((int32_t)v->snote - 60) * (int32_t)j6y_u7(snd->key_track) * 2;
    cut = j6_clamp(cut, 0, (int32_t)J6Y_CUT_NOTE_MAX * 256);
    idx = (uint32_t)cut >> 8;
    fr = (uint32_t)cut & 255u;
    g = j6y_cut_g[idx] + (((j6y_cut_g[idx + 1u] - j6y_cut_g[idx]) * (int32_t)fr) >> 8);
    j6y_ladder_coef(&lad, g >> 8, res);   /* g Q20 -> Q12 */
    gain_in = 256;                       /* (the ladder keeps its own passband: x (1 + k / 2)) */

    gsaw = s->g_saw;
    gpul = s->g_pulse;
    gsub = s->g_sub;
    gnz = s->g_noise;
    ddiff = amp1 - amp0;
    for (i = 0; i < J6_CR; i++) {
        int32_t saw, sb = 0, x, o, a;
        uint32_t u;
        ph += inc;
        saw = (int32_t)(ph >> 16) - 32768;
        if (ph < inc) {                  /* wrapped in this sample: the sub flips */
            int32_t b = j6y_blep(ph, rsh, rcp);
            saw += b;
            sub ^= 1u;
            sb = sub ? -b : b;
        } else if ((u = 0u - ph) < inc) {   /* the edge comes before the next sample */
            int32_t b = j6y_blep(u, rsh, rcp);
            saw -= b;
            sb = sub ? -b : b;
        }
        x = gsub ? ((sub ? 32767 : -32767) + sb) * gsub : 0;
        if (gpul) {                      /* pulse = (saw(ph) - saw(ph + duty)) / 2 */
            uint32_t pp = ph + pwo;
            int32_t s2 = (int32_t)(pp >> 16) - 32768;
            if (pp < inc)
                s2 += j6y_blep(pp, rsh, rcp);
            else if ((u = 0u - pp) < inc)
                s2 -= j6y_blep(u, rsh, rcp);
            x += ((saw - s2) >> 1) * gpul;
        }
        if (det) {                       /* the detuned second saw */
            int32_t s3;
            ph2 += inc2;
            s3 = (int32_t)(ph2 >> 16) - 32768;
            if (ph2 < inc2)
                s3 += j6y_blep(ph2, rsh2, rcp2);
            else if ((u = 0u - ph2) < inc2)
                s3 -= j6y_blep(u, rsh2, rcp2);
            saw = (saw + s3) >> 1;
        }
        x += saw * gsaw;
        if (gnz) {
            nz ^= nz << 13;
            nz ^= nz >> 17;
            nz ^= nz << 5;
            x += ((int32_t)(nz >> 16) - 32768) * gnz;
        }
        x >>= 15;                        /* Q15 mix */
        if (hpf == 1u) {                 /* bass boost: a low shelf ~120 Hz (Clipper's HPF 0) */
            bb += ((x - bb) * 560) >> 15;
            x += bb >> 1;
        } else if (hpf >= 2u) {          /* one-pole high-pass ~240 / ~720 Hz */
            hp += ((x - hp) * (hpf == 2u ? 1120 : 3360)) >> 15;
            x -= hp;
        }
        x = j6y_ladder_tick(&lad, (x * gain_in) >> 8, f);
        o = j6_clamp(x, -J6Y_VOICE_CLIP, J6Y_VOICE_CLIP);
        acc += ddiff;
        a = amp0 + (acc >> J6Y_CR_SHIFT);   /* linear ramp, exactly amp1 at the last sample, never below */
        mix[i] += (o * a) >> 15;
    }
    v->ph = ph;
    v->ph2 = ph2;
    v->hp = hp;
    v->bb = bb;
    v->sub = (uint8_t)sub;
    s->noise = nz;
    v->env = env;
    v->amp = amp1;

    if (v->stage == J6Y_ST_FADE) {
        if (v->fade) v->fade--;
        if (!v->fade) {
            if (v->gate)
                j6y_voice_begin(s, v);   /* the stolen voice now plays its new note */
            else
                v->stage = J6Y_ST_IDLE;
        }
    } else if (v->stage == J6Y_ST_RELEASE && env == 0) {
        v->stage = J6Y_ST_IDLE;
        v->amp = 0;
    }
}

/* ---- render ------------------------------------------------------------------------------------------ */

static void j6y_chunk(j6_synth_t *s, int32_t *out)
{
    int32_t mono[J6_CR], lr[2u * J6_CR], v0, dv, acc = 0;
    uint32_t i, any = 0;
    for (i = 0; i < J6_CR; i++) mono[i] = 0;
    for (i = 0; i < J6_VOICES; i++)
        if (s->v[i].stage != J6Y_ST_IDLE) {
            j6y_voice_chunk(s, &s->v[i], mono);
            any = 1;
        }
    if (!any && j6_synth_fx_idle(s)) {   /* nothing sounds and the tails are out: add nothing */
        s->vol_cur_q12 = s->vol_q12;
        s->kill = 0;                     /* already silent: nothing left to fade */
        j6y_fx_skip(s);
        for (i = 0; i < 2u * J6_CR; i++) out[i] = j6_clamp(out[i], -J6_OUT_MAX, J6_OUT_MAX);   /* j6.h 9.6 */
        return;
    }
    j6y_fx_chorus(s, mono, lr);
    /* vOL (SD-7), ramped so a change does not click */
    v0 = s->vol_cur_q12;
    dv = j6_clamp(s->vol_q12 - v0, -J6Y_VOL_STEP, J6Y_VOL_STEP);
    for (i = 0; i < J6_CR; i++) {
        int32_t gq;
        acc += dv;
        gq = v0 + (acc >> J6Y_CR_SHIFT);
        lr[2u * i] = (lr[2u * i] * gq) >> 12;
        lr[2u * i + 1u] = (lr[2u * i + 1u] * gq) >> 12;
    }
    s->vol_cur_q12 = v0 + dv;
    j6y_fx_chunk(s, lr);
    if (s->kill) {                       /* D2: linear fade over the remaining chunks, then cut */
        int32_t g0 = (int32_t)(s->kill << 12) / (int32_t)J6Y_KILL_CHUNKS;
        int32_t step = g0 / (int32_t)(s->kill * J6_CR);
        for (i = 0; i < J6_CR; i++) {
            int32_t g = g0 - step * (int32_t)(i + 1u);
            lr[2u * i] = (lr[2u * i] * g) >> 12;
            lr[2u * i + 1u] = (lr[2u * i + 1u] * g) >> 12;
        }
        if (--s->kill == 0) {
            for (i = 0; i < 2u * J6_CR; i++)
                out[i] = j6_clamp(j6_clamp(out[i], -J6_OUT_MAX, J6_OUT_MAX) + lr[i], -J6_OUT_MAX, J6_OUT_MAX);
            j6_synth_panic(s);
            return;
        }
    }
    for (i = 0; i < 2u * J6_CR; i++)
        out[i] = j6_clamp(j6_clamp(out[i], -J6_OUT_MAX, J6_OUT_MAX) + lr[i], -J6_OUT_MAX, J6_OUT_MAX);
}

void j6_synth_render(j6_synth_t *s, int32_t *out, uint32_t n)
{
    uint32_t c;
    for (c = 0; c + J6_CR <= n; c += J6_CR) j6y_chunk(s, out + 2u * c);
}
