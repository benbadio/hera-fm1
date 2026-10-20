/* SPDX-License-Identifier: GPL-3.0-only */
/* j6_synth_fx.c - the synth module's effects on the fx_mem pool: chorus, delay (SD-4), reverb.
 * docs/J6_ENGINE_DESIGN.md 7.2; the memory layout is in j6_synth_state.h.
 *
 * Chorus: a Juno-60 style stereo chorus (one mono line, two taps on opposite phases of a 0.5 Hz
 *   triangle), part of the "synth" before vOL; on when the sound has J6_SND_CHORUS. The line is written
 *   while it is off too, so switching it on mid-note ramps in a wet signal that is already there.
 * Delay: mono, Q14 line. Level from DLY_LEVEL, time from DLY_TIME: free 10..1400 ms (table), or with
 *   d.Syn j6_dsync_ticks(v * 12 / 128) x samples per tick, halved while longer than the line. About 40 %
 *   feedback through a darkening one-pole. Time changes slew (fractional read), so they never click.
 * Reverb: Freeverb style, 4 combs + 2 allpasses per side, Q11 lines (input / 16). REV_TIME sets the
 *   comb feedback 0.2..0.93, REV_LEVEL the wet level, damping fixed.
 * DLY_LEVEL and REV_LEVEL ramp (at most 1/16 of full scale per chunk), and so does each effect's input
 * gate (on while its level is not 0): a knob jump fades a ringing tail out, or an echo in, over 5.8 ms.
 * Every feedback product truncates toward zero, so a tail reaches exactly 0. Each effect counts the
 * samples since its last non-zero write; once a whole line has been written with zeros it is empty, the
 * effect is idle and does no work until input comes (j6_synth_fx_idle). */
#include "j6.h"
#include "j6_synth_state.h"
#include "j6_synth_tables.h"

#define J6Y_DLY_MIN 4096u                /* shortest delay line when the pool is small */
#define J6Y_DLY_SLEW_DIV 64              /* delay time: exponential glide, time constant 64 chunks */
#define J6Y_DLY_SLEW_MAX (256 * (int32_t)J6_CR)   /* ... at most 1 sample per sample (Q8 per chunk) */
#define J6Y_DLY_WET 230                  /* DLY_LEVEL 127 -> 0.89 */
#define J6Y_RV_WET 258                   /* REV_LEVEL 127 -> 1.0 */
#define J6Y_CH_MIX_STEP 2048             /* chorus on / off ramp per chunk (16 chunks) */
#define J6Y_DLY_LVL_STEP (127 * J6Y_DLY_WET / 16)   /* DLY_LEVEL ramp per chunk: 1/16 of full scale */
#define J6Y_RV_LVL_STEP (127 * J6Y_RV_WET / 16)     /* REV_LEVEL ramp per chunk */
#define J6Y_FX_IN_STEP 2048              /* delay / reverb input gate ramp per chunk, Q15 (16 chunks) */

static int32_t j6y_sat16(int32_t x) { return x > 32767 ? 32767 : x < -32767 ? -32767 : x; }

/* (a * b) >> 23 rounded toward zero: a decaying feedback loop reaches exactly 0 (rounding down would
 * let a negative remainder sustain itself). The shift is a constant: a 64-bit shift by a variable count
 * may be a libcall (__ashrdi3) that the firmware does not link. */
static int32_t j6y_mulz23(int32_t a, int32_t b)
{
    int64_t p = (int64_t)a * b;
    return p >= 0 ? (int32_t)(p >> 23) : -(int32_t)((-p) >> 23);
}

/* x / 2 rounded toward zero (an allpass tail reaches exactly 0), without a division */
static int32_t j6y_half(int32_t x) { return (x + (int32_t)((uint32_t)x >> 31)) >> 1; }

static uint32_t j6y_ch_on(const j6_synth_t *s) { return (s->snd->flags & J6_SND_CHORUS) ? 1u : 0u; }

/* the targets the ramped levels move to: wet gains, and the input gates (Q15, on while the level is not 0) */
static int32_t j6y_dly_lvl(const j6_synth_t *s) { return s->param[J6_SP_DLY_LEVEL] * J6Y_DLY_WET; }
static int32_t j6y_rv_lvl(const j6_synth_t *s) { return s->param[J6_SP_REV_LEVEL] * J6Y_RV_WET; }
static int32_t j6y_dly_in(const j6_synth_t *s) { return s->param[J6_SP_DLY_LEVEL] ? 32768 : 0; }
static int32_t j6y_rv_in(const j6_synth_t *s) { return s->param[J6_SP_REV_LEVEL] ? 32768 : 0; }

/* a per-chunk ramp: the step from cur toward tgt, at most lim */
static int32_t j6y_ramp(int32_t cur, int32_t tgt, int32_t lim) { return j6_clamp(tgt - cur, -lim, lim); }

/* every ramped level at its target (nothing sounds through the effect, or nothing can be heard of it) */
static void j6y_fx_snap(j6_synth_t *s)
{
    struct j6y_fx *f = &s->fx;
    f->dly_cur_q8 = f->dly_tgt_q8;
    f->ch_mix = (s->rv_on && s->snd && j6y_ch_on(s)) ? 32768 : 0;
    f->dly_lvl = j6y_dly_lvl(s);
    f->dly_in = j6y_dly_in(s);
    f->rv_lvl = j6y_rv_lvl(s);
    f->rv_in = j6y_rv_in(s);
}

/* the chorus taps (Q8 samples) for the LFO phase now */
/* the three Juno-60 chorus modes (after Clipper's juno_chorus, fx.c, 3dSam, GPL-3.0): I 0.513 Hz and II
 * 0.863 Hz, wide (154.7 +- 81.5 samples); I+II 9.75 Hz, shallow (154.4 +- 8.9). LFO step per chunk. */
static const uint32_t j6y_ch_inc[3] = {49962u * J6_CR, 84048u * J6_CR, 949577u * J6_CR};
static const int32_t j6y_ch_center[3] = {39603, 39603, 39514};
static const int32_t j6y_ch_depth[3] = {20864, 20864, 2266};
static uint32_t j6y_ch_mode(const j6_synth_t *s)
{
    uint32_t m = s->snd ? (s->snd->flags & J6_SND_CHMODE_MASK) >> J6_SND_CHMODE_SHIFT : 0u;
    return m > 2u ? 2u : m;
}
static void j6y_ch_taps(const j6_synth_t *s, const struct j6y_fx *f, int32_t *dl, int32_t *dr)
{
    int32_t t = (int32_t)(f->ch_lfo >> 16), tri, m;
    uint32_t md = j6y_ch_mode(s);
    tri = (t < 32768 ? t : 65535 - t) * 2 - 32767;           /* triangle -32767 .. 32767 */
    m = (int32_t)(((int64_t)j6y_ch_depth[md] * tri) >> 15);
    *dl = j6y_ch_center[md] + m;
    *dr = j6y_ch_center[md] - m;
}

/* ---- setup ----------------------------------------------------------------------------------------- */

static void j6y_fx_reset(j6_synth_t *s)
{
    struct j6y_fx *f = &s->fx;
    uint32_t i;
    f->dly_w = 0;
    f->dly_lp = 0;
    f->dly_quiet = f->dly_len;
    for (i = 0; i < 8u; i++) {
        f->comb_pos[i] = 0;
        f->comb_lp[i] = 0;
    }
    for (i = 0; i < 4u; i++) f->ap_pos[i] = 0;
    f->rv_quiet = J6Y_RV_QUIET;
    f->ch_w = 0;
    f->ch_quiet = J6Y_CH_LEN;
    j6y_fx_snap(s);
    j6y_ch_taps(s, f, &f->ch_dl_q8, &f->ch_dr_q8);
}

void j6y_fx_init(j6_synth_t *s, int16_t *mem, uint32_t len)
{
    struct j6y_fx *f = &s->fx;
    uint32_t i, rest = len, dl = J6_FX_DELAY_LEN;
    s->fx_on = 0;
    s->rv_on = 0;
    f->dly = NULL;
    f->ch = NULL;
    f->dly_len = 0;
    if (!mem || len < 64u) {
        j6y_fx_reset(s);
        return;
    }
    for (i = 0; i < len; i++) mem[i] = 0;
    s->fx_on = 1;
    if (rest >= J6Y_RV_LEN + J6Y_CH_LEN + J6Y_DLY_MIN) {
        s->rv_on = 1;
        rest -= J6Y_RV_LEN + J6Y_CH_LEN;
    }
    while (dl > rest) dl >>= 1;
    f->dly = mem;
    f->dly_len = dl;
    f->dly_mask = dl - 1u;
    if (s->rv_on) {
        int16_t *p = mem + dl;
        for (i = 0; i < 8u; i++) {
            f->comb[i] = p;
            p += j6y_rv_comb[i];
        }
        for (i = 0; i < 4u; i++) {
            f->ap[i] = p;
            p += j6y_rv_ap[i];
        }
        f->ch = p;
    }
    f->dly_tgt_q8 = 256;
    j6y_fx_reset(s);
}

void j6y_fx_clear(j6_synth_t *s)
{
    struct j6y_fx *f = &s->fx;
    uint32_t i;
    if (s->fx_on) {
        for (i = 0; i < f->dly_len; i++) f->dly[i] = 0;
        if (s->rv_on) {
            int16_t *p = f->comb[0];
            for (i = 0; i < J6Y_RV_LEN + J6Y_CH_LEN; i++) p[i] = 0;   /* reverb and chorus are contiguous */
        }
    }
    j6y_fx_reset(s);
}

void j6y_fx_params(j6_synth_t *s)
{
    struct j6y_fx *f = &s->fx;
    uint32_t v = (uint32_t)s->param[J6_SP_DLY_TIME], smp, lim;
    if (s->param[J6_SP_DLY_SYNC]) {      /* SD-4: a note value of the tempo */
        uint32_t ticks = j6_dsync_ticks((v * J6_DSYNC_VALUES) >> 7);
        smp = (ticks * j6_spt_q12((uint32_t)s->param[J6_SP_TEMPO10])) >> 12;
    } else {
        smp = j6y_dly_free[v & 127u];
    }
    lim = f->dly_len > 2u ? f->dly_len - 2u : 1u;    /* the interpolating read needs one sample more */
    while (smp > lim) smp >>= 1;         /* longer than the line: half of it */
    if (smp < 1u) smp = 1u;
    f->dly_tgt_q8 = (int32_t)(smp * 256u);
    /* jump what nothing can be heard of yet. An empty line sounds the same at any read position and any
     * wet level. An input gate or the chorus mix (it moves the dry gain) only while no voice sounds:
     * with a voice they ramp, or the dry signal / the first echo would step. An input gate snaps only
     * when its whole input is silent: the chorus tail feeds the delay, and the chorus and the delay's
     * echoes feed the reverb, so a gate that snapped open on a ringing tail would let it in with a step. */
    if (f->dly_quiet >= f->dly_len) {
        f->dly_cur_q8 = f->dly_tgt_q8;
        f->dly_lvl = j6y_dly_lvl(s);
    }
    if (f->rv_quiet >= J6Y_RV_QUIET) f->rv_lvl = j6y_rv_lvl(s);
    if (!j6_synth_active(s)) {
        uint32_t ch_quiet = !s->rv_on || f->ch_quiet >= J6Y_CH_LEN;
        if (f->dly_quiet >= f->dly_len && ch_quiet) f->dly_in = j6y_dly_in(s);
        if (f->rv_quiet >= J6Y_RV_QUIET && f->dly_quiet >= f->dly_len && ch_quiet) f->rv_in = j6y_rv_in(s);
        if (f->ch_quiet >= J6Y_CH_LEN) f->ch_mix = (s->rv_on && j6y_ch_on(s)) ? 32768 : 0;
    }
}

uint32_t j6_synth_fx_idle(const j6_synth_t *s)
{
    const struct j6y_fx *f = &s->fx;
    if (!s->fx_on) return 1u;
    if (f->dly_quiet < f->dly_len) return 0u;
    if (s->rv_on && (f->rv_quiet < J6Y_RV_QUIET || f->ch_quiet < J6Y_CH_LEN)) return 0u;
    return 1u;
}

void j6y_fx_skip(j6_synth_t *s)
{
    struct j6y_fx *f = &s->fx;
    j6y_fx_snap(s);
    f->ch_lfo += j6y_ch_inc[j6y_ch_mode(s)];
    j6y_ch_taps(s, f, &f->ch_dl_q8, &f->ch_dr_q8);
}

/* ---- chorus ------------------------------------------------------------------------------------------ */

/* linear interpolation at d (samples Q8) behind write position w */
static int32_t j6y_tap(const int16_t *line, uint32_t mask, uint32_t w, int32_t d)
{
    uint32_t ip = (uint32_t)d >> 8;
    int32_t fr = d & 255, a = line[(w - ip) & mask], b = line[(w - ip - 1u) & mask];
    return a + (((b - a) * fr) >> 8);
}

void j6y_fx_chorus(j6_synth_t *s, const int32_t *mono, int32_t *lr)
{
    struct j6y_fx *f = &s->fx;
    int32_t m0 = f->ch_mix, m1, dm, dl0, dr0, dl1, dr1, ddl, ddr;
    uint32_t i, w = f->ch_w, q = f->ch_quiet;
    const uint32_t mask = J6Y_CH_LEN - 1u;
    int16_t *line = f->ch;
    if (!s->rv_on) {                     /* no chorus memory */
        for (i = 0; i < J6_CR; i++) lr[2u * i] = lr[2u * i + 1u] = mono[i];
        return;
    }
    m1 = j6y_ch_on(s) ? 32768 : 0;
    dm = j6_clamp(m1 - m0, -J6Y_CH_MIX_STEP, J6Y_CH_MIX_STEP);
    f->ch_lfo += j6y_ch_inc[j6y_ch_mode(s)];
    dl0 = f->ch_dl_q8;
    dr0 = f->ch_dr_q8;
    j6y_ch_taps(s, f, &dl1, &dr1);
    f->ch_dl_q8 = dl1;
    f->ch_dr_q8 = dr1;
    if (m0 == 0 && dm == 0) {            /* off: dry. The line still records the voices (it empties once
                                          * they are silent), so the chorus can come in with no gap */
        for (i = 0; i < J6_CR; i++) {
            int32_t x = mono[i], wv = j6y_sat16(x >> 1);
            lr[2u * i] = lr[2u * i + 1u] = x;
            line[w] = (int16_t)wv;
            q = wv ? 0u : q + 1u;
            w = (w + 1u) & mask;
        }
        f->ch_w = w;
        f->ch_quiet = q > J6Y_CH_LEN ? J6Y_CH_LEN : q;
        return;
    }
    ddl = dl1 - dl0;
    ddr = dr1 - dr0;
    for (i = 0; i < J6_CR; i++) {
        int32_t x = mono[i], wv = j6y_sat16(x >> 1), k = (int32_t)i + 1, m, gd, gw, yl, yr;
        line[w] = (int16_t)wv;
        q = wv ? 0u : q + 1u;
        yl = j6y_tap(line, mask, w, dl0 + ((ddl * k) >> J6Y_CR_SHIFT));   /* taps glide across the chunk */
        yr = j6y_tap(line, mask, w, dr0 + ((ddr * k) >> J6Y_CR_SHIFT));
        m = (m0 + ((dm * k) >> J6Y_CR_SHIFT)) >> 1;   /* Q14 */
        gd = 16384 - (m >> 2);             /* dry 1 .. 0.75 */
        gw = (m * 5) >> 3;                 /* wet 0 .. 0.625 (taps are Q14: x2 below) */
        lr[2u * i] = (x * gd + yl * 2 * gw) >> 14;
        lr[2u * i + 1u] = (x * gd + yr * 2 * gw) >> 14;
        w = (w + 1u) & mask;
    }
    f->ch_mix = m0 + dm;
    f->ch_w = w;
    f->ch_quiet = q > J6Y_CH_LEN ? J6Y_CH_LEN : q;
}

/* ---- delay and reverb -------------------------------------------------------------------------- */

static void j6y_delay(j6_synth_t *s, int32_t *lr)
{
    struct j6y_fx *f = &s->fx;
    int32_t cur = f->dly_cur_q8, step, lp = f->dly_lp, dl, dg;
    int32_t l0 = f->dly_lvl, l1 = j6y_dly_lvl(s), g0 = f->dly_in, g1 = j6y_dly_in(s);
    uint32_t i, any = 0, w = f->dly_w, mask = f->dly_mask, q = f->dly_quiet;
    int16_t *line = f->dly;
    if (g0 || g1)                        /* the input gate is open, or ramping */
        for (i = 0; i < 2u * J6_CR; i++) any |= (uint32_t)lr[i];
    if (q >= f->dly_len && !any) {       /* empty and nothing comes in: nothing to hear, glide or fade */
        f->dly_cur_q8 = f->dly_tgt_q8;
        f->dly_lvl = l1;
        f->dly_in = g1;
        return;
    }
    dl = j6y_ramp(l0, l1, J6Y_DLY_LVL_STEP);
    dg = j6y_ramp(g0, g1, J6Y_FX_IN_STEP);
    step = (f->dly_tgt_q8 - cur) / J6Y_DLY_SLEW_DIV;
    if (step == 0) step = f->dly_tgt_q8 - cur;
    step = j6_clamp(step, -J6Y_DLY_SLEW_MAX, J6Y_DLY_SLEW_MAX);
    for (i = 0; i < J6_CR; i++) {
        int32_t k = (int32_t)i + 1, y, x, wv, wet;
        int32_t lv = l0 + ((dl * k) >> J6Y_CR_SHIFT), g = g0 + ((dg * k) >> J6Y_CR_SHIFT);   /* ramps */
        y = j6y_tap(line, mask, w, cur + ((step * k) >> J6Y_CR_SHIFT));   /* Q14 */
        /* the one-pole runs 8 bits finer than the line, so it settles to within 1/256 of y and a
         * silent line cannot keep a rounding remainder circulating */
        lp += (int32_t)(((int64_t)(y * 256 - lp) * (int32_t)J6Y_DLY_LP_Q14) >> 14);
        x = (int32_t)(((int64_t)((lr[2u * i] + lr[2u * i + 1u]) >> 2) * g) >> 15);   /* mono Q14, gated */
        wv = j6y_sat16(x + j6y_mulz23(lp, (int32_t)J6Y_DLY_FB_Q15));      /* Q22 x Q15 -> Q14 */
        line[w] = (int16_t)wv;
        q = wv ? 0u : q + 1u;
        w = (w + 1u) & mask;
        wet = (y * lv) >> 14;
        lr[2u * i] += wet;
        lr[2u * i + 1u] += wet;
    }
    f->dly_cur_q8 = cur + step;
    f->dly_lvl = l0 + dl;
    f->dly_in = g0 + dg;
    f->dly_w = w;
    if (q >= f->dly_len) {               /* the whole line is zero now */
        q = f->dly_len;
        lp = 0;
        f->dly_cur_q8 = f->dly_tgt_q8;
        f->dly_lvl = l1;
    }
    f->dly_quiet = q;
    f->dly_lp = lp;
}

static void j6y_reverb(j6_synth_t *s, int32_t *lr)
{
    struct j6y_fx *f = &s->fx;
    int32_t fb = j6y_rv_fb[s->param[J6_SP_REV_TIME]], dl, dg;
    int32_t l0 = f->rv_lvl, l1 = j6y_rv_lvl(s), g0 = f->rv_in, g1 = j6y_rv_in(s);
    uint32_t i, c, any = 0, q = f->rv_quiet;
    if (g0 || g1)                        /* the input gate is open, or ramping */
        for (i = 0; i < 2u * J6_CR; i++) any |= (uint32_t)lr[i];
    if (q >= J6Y_RV_QUIET && !any) {     /* empty and nothing comes in */
        f->rv_lvl = l1;
        f->rv_in = g1;
        return;
    }
    dl = j6y_ramp(l0, l1, J6Y_RV_LVL_STEP);
    dg = j6y_ramp(g0, g1, J6Y_FX_IN_STEP);
    for (i = 0; i < J6_CR; i++) {
        int32_t k = (int32_t)i + 1, lv = l0 + ((dl * k) >> J6Y_CR_SHIFT), g = g0 + ((dg * k) >> J6Y_CR_SHIFT);
        int32_t x = (j6y_sat16((lr[2u * i] + lr[2u * i + 1u]) >> 5) * g) >> 15;   /* mono Q11, gated */
        int32_t acc[2] = {0, 0}, wr = 0, y;
        uint32_t side;
        for (c = 0; c < 8u; c++) {
            int16_t *b = f->comb[c];
            uint32_t p = f->comb_pos[c];
            int32_t o = b[p], k;
            acc[c >> 2] += o;
            /* damping one-pole 8 bits finer than the line (Q19): it settles to within 1/256 of o, so
             * no rounding remainder keeps circulating in a silent comb */
            f->comb_lp[c] += (int32_t)(((int64_t)(o * 256 - f->comb_lp[c]) * (int32_t)J6Y_RV_LP_Q15) >> 15);
            k = j6y_sat16(x + j6y_mulz23(f->comb_lp[c], fb));             /* Q19 x Q15 -> Q11 */
            b[p] = (int16_t)k;
            wr |= k;
            f->comb_pos[c] = (uint16_t)(p + 1u >= j6y_rv_comb[c] ? 0u : p + 1u);
        }
        for (side = 0; side < 2u; side++) {
            y = acc[side] >> 2;
            for (c = 2u * side; c < 2u * side + 2u; c++) {
                int16_t *b = f->ap[c];
                uint32_t p = f->ap_pos[c];
                int32_t bo = b[p], nv = j6y_sat16(y + j6y_half(bo));
                b[p] = (int16_t)nv;
                wr |= nv;
                y = bo - j6y_half(y);
                f->ap_pos[c] = (uint16_t)(p + 1u >= j6y_rv_ap[c] ? 0u : p + 1u);
            }
            lr[2u * i + side] += (y * lv) >> 11;                      /* Q11 x Q15 -> Q15 */
        }
        q = (wr == 0 && x == 0) ? q + 1u : 0u;
    }
    f->rv_lvl = l0 + dl;
    f->rv_in = g0 + dg;
    if (q >= J6Y_RV_QUIET) {             /* every line is zero now */
        q = J6Y_RV_QUIET;
        for (c = 0; c < 8u; c++) f->comb_lp[c] = 0;
        f->rv_lvl = l1;
    }
    f->rv_quiet = q;
}

void j6y_fx_chunk(j6_synth_t *s, int32_t *lr)
{
    if (!s->fx_on) return;
    j6y_delay(s, lr);                    /* SD-7: delay, then reverb */
    if (s->rv_on) j6y_reverb(s, lr);
}
