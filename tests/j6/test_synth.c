/* SPDX-License-Identifier: GPL-3.0-only */
/* test_synth.c - host tests of the synth module (firmware/src/j6/j6_synth*.c), docs/J6_ENGINE_DESIGN.md
 * 7.5 and docs/J6_SPEC.md section 7 (SD-1 .. SD-8). Links only the synth module's own sources.
 * Writes build/j6test/synth_demo.wav for listening and prints the host time per 256-frame block. */
#define _POSIX_C_SOURCE 200809L
#include "j6_test.h"
#include "j6_synth_state.h"
#include "j6_synth_tables.h"

#include <sys/stat.h>
#include <time.h>

#define SR 44100u
#define MS(x) ((uint32_t)(x) * SR / 1000u)          /* frames, rounded down */
#define CHUNK_UP(f) (((f) + J6_CR - 1u) / J6_CR * J6_CR)
#define MAXF (SR * 12u)

static int16_t fxa[J6_FX_MEM_LEN], fxb[J6_FX_MEM_LEN], fxc[J6_FX_MEM_LEN + 64];
static j6_synth_t sa, sb, sc;
static int32_t bufa[2u * MAXF], bufb[2u * MAXF], bufc[2u * MAXF];

/* a plain saw, filter open, no effects: for pitch and level measurements */
static const j6_sound_t t_saw = {"T saw", {127, 0, 30, 0, 0, 64, 0, 64}, 127, 0, 64, 0, 0, 64, 127, 64, 0, 0, 127, 0};
/* a saw through a low cutoff, no effects: smooth, so a click stands out against the signal's own steps */
static const j6_sound_t t_soft = {"T soft", {60, 0, 30, 0, 0, 64, 0, 64}, 127, 0, 64, 0, 0, 64, 127, 64, 0, 0, 127, 0};
/* everything on at once: the most expensive voice path, for the timing */
static const j6_sound_t t_all = {"T all", {80, 60, 90, 20, 60, 90, 80, 100}, 100, 80, 40, 60, 30, 90, 100, 90, 64, 60,
                                 110, J6_SND_CHORUS};

/* ---- helpers ------------------------------------------------------------------------------------- */

/* render `frames` (rounded up to whole chunks) into out (zeroed first), in blocks of `blk` frames */
static void render_blk(j6_synth_t *s, int32_t *out, uint32_t frames, uint32_t blk)
{
    uint32_t f;
    frames = CHUNK_UP(frames);
    memset(out, 0, (size_t)frames * 2u * sizeof out[0]);
    for (f = 0; f < frames; f += blk) j6_synth_render(s, out + 2u * f, frames - f < blk ? frames - f : blk);
}
static void render(j6_synth_t *s, int32_t *out, uint32_t frames) { render_blk(s, out, frames, J6_BLOCK); }

static int all_zero(const int32_t *lr, uint32_t frames)
{
    uint32_t i;
    for (i = 0; i < 2u * frames; i++)
        if (lr[i]) return 0;
    return 1;
}

/* render blocks until no voice is active; frames rendered (max + 1 if it never happens) */
static uint32_t until_inactive(j6_synth_t *s, uint32_t max)
{
    int32_t blk[2u * J6_CR];
    uint32_t f = 0;
    while (j6_synth_active(s)) {
        if (f > max) return max + 1u;
        memset(blk, 0, sizeof blk);
        j6_synth_render(s, blk, J6_CR);
        f += J6_CR;
    }
    return f;
}

/* render until the effects are idle; frames rendered (max + 1 if never) */
static uint32_t until_fx_idle(j6_synth_t *s, uint32_t max)
{
    int32_t blk[2u * J6_BLOCK];
    uint32_t f = 0;
    while (!j6_synth_fx_idle(s) || j6_synth_active(s)) {
        if (f > max) return max + 1u;
        memset(blk, 0, sizeof blk);
        j6_synth_render(s, blk, J6_BLOCK);
        f += J6_BLOCK;
    }
    return f;
}

static struct j6y_voice *voice_of(j6_synth_t *s, uint32_t note)
{
    uint32_t i;
    for (i = 0; i < J6_VOICES; i++)
        if (s->v[i].stage != J6Y_ST_IDLE && s->v[i].note == note) return &s->v[i];
    return NULL;
}

/* frequency of the left channel from its rising zero crossings (interpolated), frames [from, to) */
static double freq_of(const int32_t *lr, uint32_t from, uint32_t to)
{
    double first = -1.0, last = -1.0;
    uint32_t i, n = 0;
    for (i = from + 1u; i < to; i++) {
        int32_t a = lr[2u * (i - 1u)], b = lr[2u * i];
        if (a < 0 && b >= 0) {
            double t = (double)(i - 1u) + (double)(-a) / (double)(b - a);
            if (first < 0.0) first = t;
            last = t;
            n++;
        }
    }
    if (n < 2u || !(last > first)) return 0.0;
    return (double)(n - 1u) * (double)SR / (last - first);
}

static double rms_d(const int32_t *lr, uint32_t frames) { return (double)j6t_rms(lr, frames); }

/* the largest sample-to-sample step |x[n] - x[n-1]| of either channel, n in (from, to) */
static int32_t max_step(const int32_t *lr, uint32_t from, uint32_t to)
{
    int32_t m = 0;
    uint32_t i, c;
    for (c = 0; c < 2u; c++)
        for (i = from + 1u; i < to; i++) {
            int32_t d = lr[2u * i + c] - lr[2u * (i - 1u) + c];
            if (d < 0) d = -d;
            if (d > m) m = d;
        }
    return m;
}

/* a click: the steps at a switch against the signal's own steady steps (before it and after the ramps) */
static int no_click(const char *what, const int32_t *lr, uint32_t sw, uint32_t win, uint32_t end)
{
    int32_t before = max_step(lr, sw - win, sw), at = max_step(lr, sw - 1u, sw + win);
    int32_t after = max_step(lr, sw + win, end), steady = before > after ? before : after;
    printf("    %s: max step %d at the switch, steady %d before / %d after\n", what, (int)at, (int)before,
           (int)after);
    return at <= steady + steady / 4 + 32;
}

static void init_nofx(j6_synth_t *s) { j6_synth_init(s, NULL, 0); }

/* host math without libm (the tests link no -lm) */
static double ln_d(double x)
{
    double z, z2, t, sum = 0.0;
    int k = 0, n;
    if (!(x > 0.0)) return -1.0e300;
    while (x > 2.0) {
        x /= 2.0;
        k++;
    }
    while (x < 1.0) {
        x *= 2.0;
        k--;
    }
    z = (x - 1.0) / (x + 1.0);
    z2 = z * z;
    t = z;
    for (n = 1; n < 80; n += 2) {
        sum += t / (double)n;
        t *= z2;
    }
    return 2.0 * sum + (double)k * 0.69314718055994531;
}
static double exp_d(double x)
{
    double t = 1.0, sum = 1.0;
    int k = 0, n;
    while (x > 0.5) {
        x -= 0.69314718055994531;
        k++;
    }
    while (x < -0.5) {
        x += 0.69314718055994531;
        k--;
    }
    for (n = 1; n < 30; n++) {
        t *= x / (double)n;
        sum += t;
    }
    for (; k > 0; k--) sum *= 2.0;
    for (; k < 0; k++) sum /= 2.0;
    return sum;
}
static double pow_approx(double a, double b) { return exp_d(b * ln_d(a)); }
static double log10_approx(double x) { return ln_d(x) / ln_d(10.0); }
static size_t strnlen_12(const char *p)
{
    size_t n = 0;
    while (n < 12u && p[n]) n++;
    return n;
}

/* ---- generated tables -------------------------------------------------------------------------- */

static void test_tables(void)
{
    uint32_t i;
    CHECK_EQ(j6y_inc[69], 42852281u);                 /* 440 * 2^32 / 44100 = 42852281.4 */
    CHECK_RANGE(j6y_inc[81], 2u * j6y_inc[69] - 1u, 2u * j6y_inc[69] + 1u);   /* an octave doubles */
    for (i = 1; i < 128u; i++) CHECK(j6y_inc[i] > j6y_inc[i - 1u]);
    for (i = 1; i < 134u; i++) CHECK(j6y_cut_g[i] > j6y_cut_g[i - 1u]);
    for (i = 1; i < 128u; i++) {
        CHECK(j6y_cut_knob[i] > j6y_cut_knob[i - 1u]);
        CHECK(j6y_res_k[i] < j6y_res_k[i - 1u]);      /* more resonance = less damping */
        CHECK(j6y_att_c[i] > j6y_att_c[i - 1u]);      /* longer times = coefficient nearer 1 */
        CHECK(j6y_dec_c[i] > j6y_dec_c[i - 1u]);
        CHECK(j6y_rel_c[i] > j6y_rel_c[i - 1u]);
        CHECK(j6y_dly_free[i] > j6y_dly_free[i - 1u]);
        CHECK(j6y_rv_fb[i] >= j6y_rv_fb[i - 1u]);
    }
    CHECK(j6y_rel_c[127] < (1u << 30));
    CHECK(j6y_res_k[127] > 0);
    CHECK_EQ(j6y_cut_knob[0], 24 * 256);
    CHECK_EQ(j6y_cut_knob[127], J6Y_CUT_NOTE_MAX * 256u);
    CHECK_EQ(j6y_dly_free[0], 441);                   /* 10 ms */
    CHECK_EQ(j6y_dly_free[127], 61740);               /* 1400 ms */
    CHECK_EQ(j6y_rv_fb[0], 6554);                     /* 0.2 */
    CHECK_EQ(j6y_rv_fb[127], 30474);                  /* 0.93 */
    CHECK_EQ(J6Y_RV_LEN + J6Y_CH_LEN + J6_FX_DELAY_LEN <= J6_FX_MEM_LEN, 1);
    /* the per-chunk tables and ramps are for this J6_CR (also static asserts in the synth sources) */
    CHECK_EQ(J6Y_TABLES_CR, J6_CR);
    CHECK_EQ(1u << J6Y_CR_SHIFT, J6_CR);
    /* the CUTOFF key quoted in j6_synth_sounds.c: 0 = 33 Hz, 64 = 758 Hz, 75 = 1.3 kHz, 127 = 16.7 kHz */
    {
        static const struct { uint32_t knob; double hz; } key[] = {
            {0, 32.7}, {32, 157.0}, {64, 758.0}, {75, 1300.0}, {96, 3700.0}, {127, 16744.0}};
        for (i = 0; i < sizeof key / sizeof key[0]; i++) {
            double hz = 440.0 * pow_approx(2.0, ((double)j6y_cut_knob[key[i].knob] / 256.0 - 69.0) / 12.0);
            CHECK(hz > key[i].hz * 0.97 && hz < key[i].hz * 1.03);
        }
    }
    {
        uint32_t sum = 0;
        for (i = 0; i < 8u; i++) sum += j6y_rv_comb[i];
        for (i = 0; i < 4u; i++) sum += j6y_rv_ap[i];
        CHECK_EQ(sum, J6Y_RV_LEN);
    }
}

/* ---- filter -------------------------------------------------------------------------------------- */

/* every cutoff x resonance: the section's state matrix is stable (Jury: |det| < 1, |tr| < 1 + det) */
static void test_filter_stable_everywhere(void)
{
    uint32_t n, r, bad = 0;
    for (n = 0; n < 134u; n++)
        for (r = 0; r <= 128u; r++) {
            struct j6y_svf c;
            double a1, a2, a3, det, tr;
            j6y_svf_coefs(&c, j6y_cut_g[n], r < 128u ? j6y_res_k[r] : (int32_t)J6Y_K2_Q20);
            a1 = (double)c.a1 / 1073741824.0;
            a2 = (double)c.a2 / 1073741824.0;
            a3 = (double)c.a3 / 1073741824.0;
            det = (2.0 * a1 - 1.0) * (1.0 - 2.0 * a3) + 4.0 * a2 * a2;
            tr = 2.0 * a1 - 2.0 * a3;
            if (!(det < 1.0 && det > -1.0 && tr < 1.0 + det && -tr < 1.0 + det)) bad++;
            if (c.a1 <= 0 || c.a2 <= 0 || c.a3 < 0) bad++;
        }
    CHECK_EQ(bad, 0);
}

/* the integer section really decays (truncation does not make it grow), and RESO 127 rings for long:
 * close to self-oscillation */
/* j6y_svf_coefs does its variable shift on 32-bit halves (no 64-bit variable shift, which may be a
 * libcall on the chip): a2 must equal the plain 64-bit reference for every table g and k */
static void test_svf_coefs_shift(void)
{
    uint32_t n, r, bad = 0;
    for (n = 0; n <= J6Y_CUT_NOTE_MAX; n++)
        for (r = 0; r <= 128u; r++) {
            int32_t g = j6y_cut_g[n], k = r < 128u ? j6y_res_k[r] : (int32_t)J6Y_K2_Q20, a2;
            uint32_t den, sh = 0, d, rc;
            struct j6y_svf c;
            if (g < 1) g = 1;
            if (k < 1) k = 1;
            den = (1u << 20) + (uint32_t)(((uint64_t)(uint32_t)g * (uint32_t)(g + k)) >> 20);
            while ((den >> sh) >= 65536u) sh++;
            d = den >> sh;
            rc = 0x7FFFFFFFu / d;
            a2 = (int32_t)(((uint64_t)(uint32_t)g * rc) >> (1u + sh));
            j6y_svf_coefs(&c, j6y_cut_g[n], r < 128u ? j6y_res_k[r] : (int32_t)J6Y_K2_Q20);
            if (c.a2 != a2) bad++;
        }
    CHECK_EQ(bad, 0);
    {   /* and between the table entries: every g from 1 to the highest cutoff, in fine steps */
        int32_t g;
        bad = 0;
        for (g = 1; g <= j6y_cut_g[J6Y_CUT_NOTE_MAX]; g = g + g / 997 + 1) {
            uint32_t den, sh = 0, d, rc;
            int32_t a2;
            struct j6y_svf c;
            den = (1u << 20) + (uint32_t)(((uint64_t)(uint32_t)g * (uint32_t)(g + 1482910)) >> 20);
            while ((den >> sh) >= 65536u) sh++;
            d = den >> sh;
            rc = 0x7FFFFFFFu / d;
            a2 = (int32_t)(((uint64_t)(uint32_t)g * rc) >> (1u + sh));
            j6y_svf_coefs(&c, g, 1482910);
            if (c.a2 != a2) bad++;
        }
        CHECK_EQ(bad, 0);
    }
}

static void test_filter_decay_and_ring(void)
{
    static const uint32_t notes[] = {12, 36, 60, 84, 108, 132};
    static const uint32_t resos[] = {0, 64, 100, 120, 127};
    uint32_t a, b, i, grew = 0;
    int32_t smax = 0;
    for (a = 0; a < sizeof notes / sizeof notes[0]; a++)
        for (b = 0; b < sizeof resos / sizeof resos[0]; b++) {
            struct j6y_svf c;
            int32_t st[2] = {0, 0}, y, early = 0, late = 0;
            const uint32_t len = 200000u;
            j6y_svf_coefs(&c, j6y_cut_g[notes[a]], j6y_res_k[resos[b]]);
            for (i = 0; i < len; i++) {
                y = j6y_svf_tick(&c, st, i == 0 ? (1 << 22) : 0);
                if (y < 0) y = -y;
                if (i < len / 10u && y > early) early = y;
                if (i >= len - len / 10u && y > late) late = y;
                if (st[0] > smax || -st[0] > smax) smax = st[0] > 0 ? st[0] : -st[0];
                if (st[1] > smax || -st[1] > smax) smax = st[1] > 0 ? st[1] : -st[1];
            }
            if (late > early / 2 && late > 16) grew++;
        }
    CHECK_EQ(grew, 0);
    /* the tick clamps its states to +-J6Y_FLT_LIM, so test against less: a half-scale impulse keeps the
     * states well inside, i.e. the clamp is a safety net and never what bounds a stable section */
    printf("    impulse: largest filter state %d (clamp %d)\n", (int)smax, (int)J6Y_FLT_LIM);
    CHECK(smax < J6Y_FLT_LIM / 2);

    /* ringing time to -60 dB at 1 kHz (note 83): long at RESO 127, short at RESO 0 */
    {
        uint32_t r, t60[2];
        for (r = 0; r < 2u; r++) {
            struct j6y_svf c;
            int32_t st[2] = {0, 0}, y, peak = 0;
            uint32_t last = 0;
            j6y_svf_coefs(&c, j6y_cut_g[83], j6y_res_k[r ? 127 : 0]);
            for (i = 0; i < 4u * SR; i++) {
                y = j6y_svf_tick(&c, st, i == 0 ? (1 << 18) : 0);   /* small: stays linear */
                if (y < 0) y = -y;
                if (y > peak) peak = y;
                if (y > peak / 1000) last = i;
            }
            t60[r] = last;
        }
        printf("    filter ring to -60 dB at 1 kHz: reso 0 %u frames, reso 127 %u frames (%.2f s)\n",
               (unsigned)t60[0], (unsigned)t60[1], (double)t60[1] / SR);
        CHECK(t60[0] < 400u);
        CHECK(t60[1] > SR / 4u);                       /* rings > 0.25 s: near self-oscillation */
    }
}

/* SD-3 CUTOFF and RESO swept all the way while a loud chord plays: bounded, decays afterwards */
static void test_filter_sweep_bounded(void)
{
    uint32_t c, i, pinned = 0;
    int32_t blk[2u * J6_CR], peak = 0;
    j6_synth_init(&sa, fxa, J6_FX_MEM_LEN);
    j6y_sound_load(&sa, &t_saw);
    j6_synth_set(&sa, J6_SP_VOLUME, 127);
    j6_synth_note_on(&sa, 36, 127);
    j6_synth_note_on(&sa, 48, 127);
    j6_synth_note_on(&sa, 55, 127);
    j6_synth_note_on(&sa, 60, 127);
    for (c = 0; c < 6u * SR / J6_CR; c++) {
        uint32_t ph = c % 512u, tri = ph < 256u ? ph / 2u : (511u - ph) / 2u;
        j6_synth_set(&sa, J6_SP_CUTOFF, (int32_t)tri);
        j6_synth_set(&sa, J6_SP_RESO, (int32_t)((c / 512u) * 16u % 128u + (c / 4096u) % 16u));
        memset(blk, 0, sizeof blk);
        j6_synth_render(&sa, blk, J6_CR);
        for (i = 0; i < J6_VOICES; i++) {
            int32_t k;
            for (k = 0; k < 4; k++) pinned += sa.v[i].f[k] == J6Y_FLT_LIM || sa.v[i].f[k] == -J6Y_FLT_LIM;
        }
        if (j6t_peak(blk, J6_CR) > peak) peak = j6t_peak(blk, J6_CR);
    }
    /* the tick clamps the states, so count how often they sit on the clamp: rarely, even here (an
     * unstable section would sit there all the time) */
    printf("    sweep: %u of %u chunk-end filter states on the clamp\n", (unsigned)pinned,
           (unsigned)(6u * SR / J6_CR * 4u * J6_VOICES));
    CHECK(pinned * 1000u < 6u * SR / J6_CR * 4u * J6_VOICES);
    CHECK_RANGE(peak, 1000, J6_OUT_MAX);
    j6_synth_all_off(&sa);
    CHECK(until_inactive(&sa, 3u * SR) <= 3u * SR);
    CHECK(until_fx_idle(&sa, 60u * SR) <= 60u * SR);
    render(&sa, bufa, SR / 2u);
    CHECK(all_zero(bufa, SR / 2u));
}

/* resonance boosts the band around the cutoff (noise in: the narrow peak always finds energy) */
static void test_resonance_boosts(void)
{
    static const j6_sound_t t_noise = {"T noise", {50, 0, 30, 0, 0, 64, 0, 64}, 0, 0, 64, 0, 127, 64, 127, 64, 0, 0,
                                       127, 0};
    double r0 = 0.0, r1 = 0.0;
    uint32_t r;
    for (r = 0; r < 2u; r++) {
        init_nofx(&sa);
        j6y_sound_load(&sa, &t_noise);
        j6_synth_set(&sa, J6_SP_RESO, r ? 127 : 0);
        j6_synth_note_on(&sa, 60, 100);
        render(&sa, bufa, SR);
        if (r)
            r1 = rms_d(bufa + 2u * (SR / 2u), SR / 2u);
        else
            r0 = rms_d(bufa + 2u * (SR / 2u), SR / 2u);
    }
    printf("    noise rms at cutoff 50: reso 0 %.0f, reso 127 %.0f\n", r0, r1);
    /* the Juno ladder (Clipper's TPT ladder) has a narrower peak than the old 2 x SVF: on white noise its
     * resonance at full lifts the RMS ~1.9x (it self-oscillates there, see test_filter_decay_and_ring) */
    CHECK(r1 > 1.5 * r0);
    CHECK(r1 < 32767.0);
}

/* ---- silence, envelopes ----------------------------------------------------------------------- */

static void test_idle_is_exact_silence(void)
{
    j6_synth_init(&sa, fxa, J6_FX_MEM_LEN);
    CHECK_EQ(j6_synth_active(&sa), 0);
    CHECK_EQ(j6_synth_fx_idle(&sa), 1);
    render(&sa, bufa, SR);
    CHECK(all_zero(bufa, SR));
    init_nofx(&sb);
    CHECK_EQ(j6_synth_fx_idle(&sb), 1);
    render(&sb, bufb, SR);
    CHECK(all_zero(bufb, SR));
    /* the out buffer is ADDed to: an idle synth leaves it unchanged */
    {
        uint32_t i;
        for (i = 0; i < 2u * J6_BLOCK; i++) bufa[i] = (int32_t)i - 256;
        j6_synth_render(&sa, bufa, J6_BLOCK);
        for (i = 0; i < 2u * J6_BLOCK; i++) CHECK_EQ(bufa[i], (int32_t)i - 256);
    }
    /* ... but the sum is clamped to +-J6_OUT_MAX (j6.h 9.6) whether the synth adds anything or not, so
     * what the glue summed in first (MIX IN) clips the same with the synth idle as with a note playing */
    {
        static const int32_t over[] = {40000, -40000, 2000000000, -2000000000};
        uint32_t i, k, bad = 0;
        for (k = 0; k < sizeof over / sizeof over[0]; k++) {
            for (i = 0; i < 2u * J6_BLOCK; i++) bufa[i] = over[k];
            j6_synth_render(&sa, bufa, J6_BLOCK);
            for (i = 0; i < 2u * J6_BLOCK; i++) bad += bufa[i] != (over[k] > 0 ? J6_OUT_MAX : -J6_OUT_MAX);
        }
        CHECK_EQ(bad, 0);
        CHECK_EQ(j6_synth_active(&sa), 0);
        CHECK_EQ(j6_synth_fx_idle(&sa), 1);
    }
}

static void test_note_sounds(void)
{
    int32_t p;
    j6_synth_init(&sa, fxa, J6_FX_MEM_LEN);
    j6_synth_note_on(&sa, 60, 100);
    CHECK_EQ(j6_synth_active(&sa), 1);
    CHECK_EQ(j6_synth_fx_idle(&sa), 1);              /* nothing came out yet */
    render(&sa, bufa, SR);
    p = j6t_peak(bufa, SR);
    printf("    sound 1-1 note 60 vel 100: peak %d\n", (int)p);
    CHECK_RANGE(p, 1000, 30000);
    CHECK_EQ(j6_synth_active(&sa), 1);
    CHECK_EQ(j6_synth_fx_idle(&sa), 0);
    CHECK(!all_zero(bufa + 2u * (SR - 256u), 256u));
}

/* a single saw voice at velocity 127, level 127, vOL 100: about -12 dBFS (design 7.1) */
static void test_voice_level(void)
{
    int32_t p;
    init_nofx(&sa);
    j6y_sound_load(&sa, &t_saw);
    j6_synth_note_on(&sa, 57, 127);
    render(&sa, bufa, SR / 2u);
    p = j6t_peak(bufa + 2u * (SR / 4u), SR / 4u);
    printf("    one saw voice vel 127: peak %d (%.1f dBFS)\n", (int)p, 20.0 * log10_approx((double)p / 32767.0));
    CHECK_RANGE(p, 6000, 11000);
}

/* note-off decays to exactly 0 within the release time (RELEASE knob, 5 ms .. 5 s) */
static void test_release_exact_zero(void)
{
    static const int32_t rel[] = {0, 40, 64, 90, 127};
    uint32_t k;
    for (k = 0; k < sizeof rel / sizeof rel[0]; k++) {
        double t_ms = 5.0 * pow_approx(1000.0, (double)rel[k] / 127.0);
        uint32_t lim = (uint32_t)(t_ms * 44.1 * 1.05) + 64u, got;
        init_nofx(&sa);
        j6y_sound_load(&sa, &t_saw);
        j6_synth_set(&sa, J6_SP_RELEASE, rel[k]);
        j6_synth_note_on(&sa, 60, 127);
        render(&sa, bufa, MS(300));
        j6_synth_note_off(&sa, 60);
        CHECK_EQ(j6_synth_active(&sa), 1);           /* releasing still counts */
        got = until_inactive(&sa, 8u * SR);
        printf("    release %3d: silent after %6u frames (nominal %.0f ms = %u)\n", (int)rel[k], (unsigned)got,
               t_ms, (unsigned)(t_ms * 44.1));
        CHECK(got <= lim);
        CHECK(got + 64u >= (uint32_t)(t_ms * 44.1 * 0.8));
        render(&sa, bufa, SR / 4u);
        CHECK(all_zero(bufa, SR / 4u));
        CHECK_EQ(sa.v[0].amp, 0);
        CHECK_EQ(sa.v[0].env, 0);
    }
    /* with the effects: a chorus sound, delay and reverb levels 0: exactly 0 once the lines are empty */
    j6_synth_init(&sa, fxa, J6_FX_MEM_LEN);
    j6_synth_sound(&sa, 0);
    j6_synth_set(&sa, J6_SP_DLY_LEVEL, 0);
    j6_synth_set(&sa, J6_SP_REV_LEVEL, 0);
    j6_synth_set(&sa, J6_SP_RELEASE, 20);
    j6_synth_note_on(&sa, 60, 127);
    render(&sa, bufa, MS(500));
    j6_synth_note_off(&sa, 60);
    CHECK(until_fx_idle(&sa, 2u * SR) <= 2u * SR);
    render(&sa, bufa, SR / 2u);
    CHECK(all_zero(bufa, SR / 2u));
}

/* attack from the knob (1 ms .. 3 s), decay and sustain from the preset (SD-6) */
static void test_attack_decay_sustain(void)
{
    static const int32_t att[] = {0, 64, 100, 127};
    uint32_t k;
    for (k = 0; k < sizeof att / sizeof att[0]; k++) {
        double t_ms = pow_approx(3000.0, (double)att[k] / 127.0);
        uint32_t f = 0;
        init_nofx(&sa);
        j6y_sound_load(&sa, &t_saw);
        j6_synth_set(&sa, J6_SP_ATTACK, att[k]);
        j6_synth_note_on(&sa, 60, 100);
        while (sa.v[0].stage == J6Y_ST_ATTACK && f < 5u * SR) {
            int32_t blk[2u * J6_CR] = {0};
            j6_synth_render(&sa, blk, J6_CR);
            f += J6_CR;
        }
        printf("    attack %3d: peak after %6u frames (nominal %.0f ms = %u)\n", (int)att[k], (unsigned)f, t_ms,
               (unsigned)(t_ms * 44.1));
        CHECK(f <= (uint32_t)(t_ms * 44.1 * 1.1) + 48u);
        CHECK(f + 48u >= (uint32_t)(t_ms * 44.1 * 0.9));
        CHECK_EQ(sa.v[0].env, 1 << 30);
    }
    /* decay to the preset's sustain: Strings (sustain 115) */
    init_nofx(&sa);
    j6_synth_sound(&sa, 8);
    j6_synth_set(&sa, J6_SP_ATTACK, 0);
    j6_synth_note_on(&sa, 60, 100);
    render(&sa, bufa, 8u * SR);
    CHECK_RANGE(sa.v[0].env, (int32_t)(115.0 / 127.0 * 1073741824.0 * 0.98),
                (int32_t)(115.0 / 127.0 * 1073741824.0 * 1.02));
    CHECK_EQ(sa.v[0].stage, J6Y_ST_DECAY);
    /* sustain 0: a pluck decays to silence while held, the voice stays allocated (gated) */
    init_nofx(&sa);
    j6_synth_sound(&sa, 48);
    j6_synth_note_on(&sa, 60, 100);
    render(&sa, bufa, 6u * SR);
    CHECK(all_zero(bufa + 2u * 5u * SR, SR));
    CHECK_EQ(j6_synth_active(&sa), 1);
}

/* ---- voices (SD-5) -------------------------------------------------------------------------------- */

static void test_four_voices_and_steal_oldest(void)
{
    static const uint32_t notes[4] = {60, 62, 64, 65};
    struct j6y_voice *v, *was60;
    uint32_t i;
    init_nofx(&sa);
    j6y_sound_load(&sa, &t_saw);
    for (i = 0; i < 4u; i++) {
        j6_synth_note_on(&sa, notes[i], 100);
        render(&sa, bufa, MS(20));
    }
    CHECK_EQ(j6_synth_active(&sa), 4);
    was60 = voice_of(&sa, 60);
    CHECK(was60 != NULL);
    j6_synth_note_on(&sa, 67, 100);                  /* 5th note: the oldest (60) is stolen */
    CHECK_EQ(j6_synth_active(&sa), 4);
    v = voice_of(&sa, 67);
    CHECK(v == was60);
    CHECK(voice_of(&sa, 60) == NULL);
    CHECK(v && v->stage == J6Y_ST_FADE);
    CHECK(voice_of(&sa, 62) && voice_of(&sa, 64) && voice_of(&sa, 65));
    /* the fade: the stolen voice's gain falls to 0 over J6Y_FADE_CHUNKS chunks, then the new note starts */
    if (v) {
        int32_t last = v->amp;
        CHECK(last > 0);
        for (i = 0; i < J6Y_FADE_CHUNKS; i++) {
            int32_t blk[2u * J6_CR] = {0};
            CHECK_EQ(v->stage, J6Y_ST_FADE);
            j6_synth_render(&sa, blk, J6_CR);
            CHECK(v->amp < last || v->amp == 0);
            last = v->amp;
        }
        CHECK_EQ(v->stage, J6Y_ST_ATTACK);
        CHECK_EQ(v->amp, 0);
        CHECK_EQ(v->env, 0);
        render(&sa, bufa, MS(50));
        CHECK(v->amp > 0);
    }
    CHECK_RANGE(J6Y_FADE_CHUNKS * J6_CR * 10000u / SR, 10, 20);   /* the fade is 1 .. 2 ms (design 7.1) */
    j6_synth_note_off(&sa, 60);                      /* the stolen note's off changes nothing */
    CHECK_EQ(j6_synth_active(&sa), 4);
    for (i = 0; i < J6_VOICES; i++) CHECK_EQ(sa.v[i].gate, 1);
    /* the next steal takes the next oldest (62) */
    j6_synth_note_on(&sa, 69, 100);
    CHECK(voice_of(&sa, 62) == NULL);
    CHECK(voice_of(&sa, 69) != NULL);
    j6_synth_note_off(&sa, 67);
    CHECK(voice_of(&sa, 67) && voice_of(&sa, 67)->stage == J6Y_ST_RELEASE);
}

/* a note released during the steal fade does not start; a re-steal of a fading voice keeps one fade */
static void test_steal_edge_cases(void)
{
    struct j6y_voice *v;
    uint32_t i;
    init_nofx(&sa);
    j6y_sound_load(&sa, &t_saw);
    for (i = 0; i < 4u; i++) j6_synth_note_on(&sa, 60u + i, 100);
    render(&sa, bufa, MS(20));
    j6_synth_note_on(&sa, 70, 100);
    v = voice_of(&sa, 70);
    CHECK(v && v->stage == J6Y_ST_FADE);
    j6_synth_note_off(&sa, 70);                      /* released before it began */
    render(&sa, bufa, MS(5));
    CHECK(v && v->stage == J6Y_ST_IDLE);
    CHECK_EQ(j6_synth_active(&sa), 3);
    /* the same note during its own fade: still one voice */
    j6_synth_note_on(&sa, 72, 100);                  /* takes the idle voice */
    j6_synth_note_on(&sa, 74, 90);                   /* steals 61 */
    j6_synth_note_on(&sa, 74, 120);                  /* again: the same (fading) voice */
    v = voice_of(&sa, 74);
    CHECK(v != NULL);
    {
        uint32_t n74 = 0;
        for (i = 0; i < J6_VOICES; i++) n74 += sa.v[i].stage != J6Y_ST_IDLE && sa.v[i].note == 74u;
        CHECK_EQ(n74, 1);
    }
    CHECK(v && v->vel == 120u);
    render(&sa, bufa, MS(5));
    CHECK(v && v->gate && (v->stage == J6Y_ST_ATTACK || v->stage == J6Y_ST_DECAY) && v->amp > 0);
}

/* the steal fade keeps the old note's filter: key tracking follows the sounding note, not the pending one,
 * so a steal into a far register does not throw the fading voice's cutoff (and a click) into the fade */
static int32_t steal_fade_step(uint32_t kt, uint32_t newnote, int32_t *steady)
{
    static const uint32_t held[4] = {36, 40, 43, 47};
    j6_sound_t k = t_saw;
    const uint32_t sw = CHUNK_UP(MS(200)), fade = J6Y_FADE_CHUNKS * J6_CR;
    uint32_t i;
    k.knob[J6_KNOB_CUTOFF] = 40;
    k.key_track = (uint8_t)kt;
    init_nofx(&sa);
    j6y_sound_load(&sa, &k);
    for (i = 0; i < 4u; i++) j6_synth_note_on(&sa, held[i], 127);
    render(&sa, bufa, sw);
    j6_synth_note_on(&sa, newnote, 127);              /* steals the 36 voice */
    CHECK(voice_of(&sa, newnote) && voice_of(&sa, newnote)->stage == J6Y_ST_FADE);
    render(&sa, bufa + 2u * sw, fade);
    *steady = max_step(bufa, sw - MS(50), sw);
    return max_step(bufa, sw - 1u, sw + fade);
}

static void test_steal_fade_keeps_old_filter(void)
{
    static const uint32_t kts[2] = {127, 64}, nn[4] = {84, 96, 108, 120};
    uint32_t a, b;
    for (a = 0; a < 2u; a++)
        for (b = 0; b < 4u; b++) {
            int32_t steady, at = steal_fade_step(kts[a], nn[b], &steady);
            printf("    key track %u, steal into note %u: max step %d in the fade, steady %d\n", (unsigned)kts[a],
                   (unsigned)nn[b], (int)at, (int)steady);
            CHECK(at <= steady + steady / 4 + 32);
        }
    /* the stolen voice still begins its new note after the fade, at the new note's pitch */
    {
        struct j6y_voice *v = voice_of(&sa, 120);
        CHECK(v && v->stage == J6Y_ST_ATTACK && v->snote == 120u);
    }
}

/* idle voices first, then the quietest releasing voice, never a gated one while those exist */
static void test_alloc_order(void)
{
    struct j6y_voice *v62, *v64;
    uint32_t i;
    init_nofx(&sa);
    j6y_sound_load(&sa, &t_saw);
    j6_synth_set(&sa, J6_SP_RELEASE, 110);           /* long releases */
    j6_synth_note_on(&sa, 60, 100);
    j6_synth_note_on(&sa, 62, 100);
    j6_synth_note_on(&sa, 64, 100);
    render(&sa, bufa, MS(100));
    j6_synth_note_off(&sa, 62);
    render(&sa, bufa, MS(300));
    j6_synth_note_off(&sa, 64);
    render(&sa, bufa, MS(10));
    v62 = voice_of(&sa, 62);
    v64 = voice_of(&sa, 64);
    CHECK(v62 && v64 && v62->amp < v64->amp);         /* 62 released earlier: quieter */
    j6_synth_note_on(&sa, 65, 100);                   /* one voice is still idle: it gets the note */
    CHECK(voice_of(&sa, 62) == v62 && voice_of(&sa, 64) == v64);
    CHECK_EQ(j6_synth_active(&sa), 4);
    j6_synth_note_on(&sa, 67, 50);                    /* none idle: the quietest releasing (62) */
    CHECK(voice_of(&sa, 67) == v62);
    CHECK(v62 && v62->stage == J6Y_ST_ATTACK);        /* no fade for a releasing voice */
    CHECK(v62 && v62->env > 0);                       /* the envelope rises from where it was */
    CHECK(v62 && v62->inc == j6y_inc[67]);            /* ... at the new note's pitch and velocity */
    CHECK(v62 && v62->vel_q15 == 50 * 258);
    CHECK(voice_of(&sa, 64) == v64);
    j6_synth_note_on(&sa, 69, 100);                   /* then 64, the only releasing one */
    CHECK(voice_of(&sa, 69) == v64);
    CHECK(v64 && v64->inc == j6y_inc[69]);
    CHECK(voice_of(&sa, 60) != NULL);                 /* the gated ones survived */
    CHECK(voice_of(&sa, 65) != NULL);
    /* and it sounds like it: alone (the other voices silenced by hand), the reused voice plays G4 at the
     * level of a fresh voice at velocity 50, not note 62 at velocity 100 */
    if (v62) {
        double f;
        int32_t p_reused, p_fresh;
        for (i = 0; i < J6_VOICES; i++)
            if (&sa.v[i] != v62) {
                sa.v[i].stage = J6Y_ST_IDLE;
                sa.v[i].gate = 0;
                sa.v[i].amp = 0;
            }
        render(&sa, bufa, SR / 2u);
        f = freq_of(bufa, SR / 4u, SR / 2u);
        p_reused = j6t_peak(bufa + 2u * (SR / 4u), SR / 4u);
        init_nofx(&sb);
        j6y_sound_load(&sb, &t_saw);
        j6_synth_note_on(&sb, 67, 50);
        render(&sb, bufb, SR / 2u);
        p_fresh = j6t_peak(bufb + 2u * (SR / 4u), SR / 4u);
        printf("    reused releasing voice: %.2f Hz (G4 392.00), peak %d (fresh voice %d)\n", f, (int)p_reused,
               (int)p_fresh);
        CHECK(f > 392.0 * 0.995 && f < 392.0 * 1.005);
        CHECK_RANGE(p_reused, p_fresh - p_fresh / 20, p_fresh + p_fresh / 20);
    }
}

/* SD-5: the same note again reuses its own voice, gated or releasing */
static void test_same_note_retrigger(void)
{
    struct j6y_voice *v;
    int32_t env_before;
    init_nofx(&sa);
    j6y_sound_load(&sa, &t_saw);
    j6_synth_note_on(&sa, 60, 100);
    v = voice_of(&sa, 60);
    render(&sa, bufa, MS(50));
    j6_synth_note_on(&sa, 60, 80);
    CHECK_EQ(j6_synth_active(&sa), 1);
    CHECK(voice_of(&sa, 60) == v);
    CHECK(v && v->vel == 80u);
    j6_synth_note_off(&sa, 60);
    render(&sa, bufa, MS(20));
    CHECK(v && v->stage == J6Y_ST_RELEASE);
    env_before = v ? v->env : 0;
    j6_synth_note_on(&sa, 60, 100);                  /* releasing: retriggered in place, no fade */
    CHECK(voice_of(&sa, 60) == v);
    CHECK(v && v->stage == J6Y_ST_ATTACK && v->gate == 1u);
    CHECK(v && v->env == env_before);
    CHECK_EQ(j6_synth_active(&sa), 1);
}

/* notes outside the contract */
static void test_note_edges(void)
{
    init_nofx(&sa);
    j6_synth_note_on(&sa, 128, 100);                 /* not a MIDI note */
    CHECK_EQ(j6_synth_active(&sa), 0);
    j6_synth_note_on(&sa, 60, 0);                    /* velocity 0 = note-off: nothing playing */
    CHECK_EQ(j6_synth_active(&sa), 0);
    j6_synth_note_on(&sa, 60, 200);                  /* clamped */
    CHECK(voice_of(&sa, 60) && voice_of(&sa, 60)->vel == 127u);
    j6_synth_note_on(&sa, 60, 0);                    /* note-off by velocity 0 */
    CHECK(voice_of(&sa, 60) && voice_of(&sa, 60)->stage == J6Y_ST_RELEASE);
    j6_synth_note_off(&sa, 61);                      /* not playing: ignored */
    j6_synth_note_on(&sa, 0, 64);                    /* the extremes of the range play */
    j6_synth_note_on(&sa, 127, 64);
    render(&sa, bufa, MS(100));
    CHECK_RANGE(j6t_peak(bufa, MS(100)), 1, J6_OUT_MAX);
    j6_synth_all_off(&sa);
    {
        uint32_t i;
        for (i = 0; i < J6_VOICES; i++) CHECK_EQ(sa.v[i].gate, 0);
    }
    CHECK(until_inactive(&sa, 2u * SR) <= 2u * SR);
}

/* velocity scales the level */
static void test_velocity_scales(void)
{
    int32_t p127, p64;
    init_nofx(&sa);
    j6y_sound_load(&sa, &t_saw);
    j6_synth_note_on(&sa, 57, 127);
    render(&sa, bufa, SR / 2u);
    p127 = j6t_peak(bufa + 2u * (SR / 4u), SR / 4u);
    init_nofx(&sa);
    j6y_sound_load(&sa, &t_saw);
    j6_synth_note_on(&sa, 57, 64);
    render(&sa, bufa, SR / 2u);
    p64 = j6t_peak(bufa + 2u * (SR / 4u), SR / 4u);
    CHECK_RANGE(p127 * 100 / (p64 ? p64 : 1), 190, 210);
}

/* 4 voices at velocity 127, every knob at 127, vOL 127, every third preset: within +-32767 and never
 * wrapped. A wrapped int32 sum would flip the sign, so the output at vOL 127 must have the sign of the
 * output at vOL 120 wherever both are loud (everything after the voices is linear or saturating). */
static void worst_case(j6_synth_t *s, int16_t *mem, uint32_t snd, int32_t vol, int32_t *out)
{
    uint32_t k;
    j6_synth_init(s, mem, J6_FX_MEM_LEN);
    j6_synth_sound(s, snd);
    for (k = 0; k < J6_KNOBS; k++) j6_synth_set(s, k, 127);
    j6_synth_set(s, J6_SP_ATTACK, 0);
    j6_synth_set(s, J6_SP_VOLUME, vol);
    j6_synth_note_on(s, 36, 127);
    j6_synth_note_on(s, 43, 127);
    j6_synth_note_on(s, 48, 127);
    j6_synth_note_on(s, 55, 127);
    render(s, out, SR);
}

static void test_worst_case_bounded(void)
{
    uint32_t snd, i, flips = 0, loud = 0;
    int32_t peak = 0;
    for (snd = 0; snd < J6_SOUNDS; snd += 3u) {
        worst_case(&sa, fxa, snd, 127, bufa);
        worst_case(&sb, fxb, snd, 120, bufb);
        for (i = 0; i < 2u * SR; i++) {
            int32_t a = bufa[i], b = bufb[i];
            if (a > J6_OUT_MAX || a < -J6_OUT_MAX) flips++;
            if ((a > 20000 && b > 20000) || (a < -20000 && b < -20000)) loud++;
            if ((a > 20000 && b < -20000) || (a < -20000 && b > 20000)) flips++;
        }
        if (j6t_peak(bufa, SR) > peak) peak = j6t_peak(bufa, SR);
    }
    printf("    worst case peak %d, %u loud samples agree in sign\n", (int)peak, (unsigned)loud);
    CHECK(peak <= J6_OUT_MAX);                        /* the output clamp (contract); the voices are tested
                                                       * by the sign agreement: a wrapped sum flips it */
    CHECK(loud > SR);
    CHECK_EQ(flips, 0);
    /* added to a buffer already near full scale, or to garbage: still clamped */
    for (i = 0; i < 2u * J6_BLOCK; i++) bufb[i] = (i & 1u) ? -32000 : 32000;
    j6_synth_render(&sa, bufb, J6_BLOCK);
    CHECK(j6t_peak(bufb, J6_BLOCK) <= J6_OUT_MAX);
    for (i = 0; i < 2u * J6_BLOCK; i++) bufb[i] = (i & 1u) ? -2000000000 : 2000000000;
    j6_synth_render(&sa, bufb, J6_BLOCK);
    CHECK(j6t_peak(bufb, J6_BLOCK) <= J6_OUT_MAX);
}

/* ---- pitch (SD-8) -------------------------------------------------------------------------------- */

static double pitch_of(uint32_t note, int32_t tune10)
{
    init_nofx(&sa);
    j6y_sound_load(&sa, &t_saw);
    if (tune10) j6_synth_set(&sa, J6_SP_TUNE10, tune10);
    j6_synth_note_on(&sa, note, 100);
    render(&sa, bufa, SR + SR / 10u);
    return freq_of(bufa, SR / 10u, SR + SR / 10u);
}

static void test_pitch_and_tune(void)
{
    double f;
    f = pitch_of(69, 0);
    printf("    A4 %.3f Hz\n", f);
    CHECK(f > 440.0 * 0.995 && f < 440.0 * 1.005);
    f = pitch_of(81, 0);
    CHECK(f > 880.0 * 0.995 && f < 880.0 * 1.005);
    f = pitch_of(57, 0);
    CHECK(f > 220.0 * 0.995 && f < 220.0 * 1.005);
    f = pitch_of(69, 4330);
    printf("    A4 at tUnE 433.0: %.3f Hz\n", f);
    CHECK(f > 433.0 * 0.995 && f < 433.0 * 1.005);
    f = pitch_of(69, 4480);
    printf("    A4 at tUnE 448.0: %.3f Hz\n", f);
    CHECK(f > 448.0 * 0.995 && f < 448.0 * 1.005);
    /* the tune factor of SD-8 */
    init_nofx(&sa);
    CHECK_EQ(sa.tune_q16, 65536u);
    j6_synth_set(&sa, J6_SP_TUNE10, 4330);
    CHECK_EQ(sa.tune_q16, (4330u * 65536u + 2200u) / 4400u);
    j6_synth_set(&sa, J6_SP_TUNE10, 4480);
    CHECK_EQ(sa.tune_q16, (4480u * 65536u + 2200u) / 4400u);
    /* tUnE moves a note that is already sounding */
    init_nofx(&sa);
    j6y_sound_load(&sa, &t_saw);
    j6_synth_note_on(&sa, 69, 100);
    render(&sa, bufa, SR / 10u);
    j6_synth_set(&sa, J6_SP_TUNE10, 4480);
    render(&sa, bufa, SR);
    f = freq_of(bufa, SR / 10u, SR);
    CHECK(f > 448.0 * 0.995 && f < 448.0 * 1.005);
}

/* ---- oscillators, filter tracking, decay (SD-6, design 7.1) ------------------------------------- */

/* the pulse alone, filter open, A2 for 1 s (110 whole cycles): the fraction of samples above the middle
 * between its two levels is the duty (the edges overshoot the same way, so the middle of the extremes) */
static double duty_of(uint32_t pw)
{
    j6_sound_t p = t_saw;
    const uint32_t from = SR / 4u;
    uint32_t i, hi = 0;
    int32_t lo = 0, top = 0, mid;
    p.saw = 0;
    p.pulse = 127;
    p.pw = (uint8_t)pw;
    init_nofx(&sa);
    j6y_sound_load(&sa, &p);
    j6_synth_note_on(&sa, 45, 127);
    render(&sa, bufa, from + SR);
    for (i = from; i < from + SR; i++) {
        if (bufa[2u * i] < lo) lo = bufa[2u * i];
        if (bufa[2u * i] > top) top = bufa[2u * i];
    }
    mid = (lo + top) / 2;
    for (i = from; i < from + SR; i++) hi += bufa[2u * i] > mid;
    return (double)hi / (double)SR;
}

/* pw 64 is a square; each unit away from 64 narrows the pulse by 0.45 / 64 of a cycle, so pw 0 is 5 % */
static void test_pulse_width(void)
{
    static const struct {
        uint32_t pw;
        double duty;
    } want[] = {{64, 0.5}, {32, 0.275}, {96, 0.275}, {10, 0.5 - 54.0 * 0.45 / 64.0}, {0, 0.05},
                {127, 0.05 + 0.45 / 64.0}};
    uint32_t k;
    for (k = 0; k < sizeof want / sizeof want[0]; k++) {
        double d = duty_of(want[k].pw);
        if (want[k].pw == 0u || want[k].pw == 64u) printf("    pw %3u: duty %.3f\n", (unsigned)want[k].pw, d);
        CHECK(d > want[k].duty - 0.01 && d < want[k].duty + 0.01);
    }
}

/* RMS in windows of 5 periods of A3 (1002 frames) over [from, to): the smallest over the largest (1 = a
 * steady A3, about 0.5 = two saws beating) */
static double rms_swing(const int32_t *lr, uint32_t from, uint32_t to)
{
    const uint32_t win = 1002u;
    double lo = 1.0e300, hi = 0.0;
    uint32_t w;
    for (w = from; w + win <= to; w += win) {
        double r = rms_d(lr + 2u * w, win);
        if (r < lo) lo = r;
        if (r > hi) hi = r;
    }
    return hi > 0.0 ? lo / hi : 0.0;
}

/* detune adds a second saw detune x 8 / 65536 sharper (127: +26.6 cents): at A3 the two beat at 3.4 Hz */
static void test_detune(void)
{
    j6_sound_t d = t_saw;
    double sw[2];
    uint32_t k;
    for (k = 0; k < 2u; k++) {
        struct j6y_voice *v;
        d.detune = k ? 127u : 0u;
        init_nofx(&sa);
        j6y_sound_load(&sa, &d);
        j6_synth_note_on(&sa, 57, 127);
        v = voice_of(&sa, 57);
        CHECK(v && v->inc == j6y_inc[57]);
        CHECK(v && v->inc2 == (k ? (uint32_t)(((uint64_t)j6y_inc[57] * (65536u + 127u * 8u)) >> 16) : j6y_inc[57]));
        render(&sa, bufa, SR / 4u + SR);
        sw[k] = rms_swing(bufa, SR / 4u, SR / 4u + SR);
    }
    printf("    A3 rms swing (min / max over 23 ms windows): detune 0 %.3f, detune 127 %.3f\n", sw[0], sw[1]);
    CHECK(sw[0] > 0.98);
    CHECK(sw[1] < 0.7);
}

/* a sound loaded while a note sounds: the note keeps its pitch, its second saw takes the new detune */
static void test_sound_load_repitches(void)
{
    j6_sound_t det = t_saw;
    struct j6y_voice *v;
    uint32_t want2 = (uint32_t)(((uint64_t)j6y_inc[57] * (65536u + 127u * 8u)) >> 16);
    det.detune = 127;
    init_nofx(&sa);
    j6y_sound_load(&sa, &t_saw);
    j6_synth_note_on(&sa, 57, 127);
    render(&sa, bufa, MS(50));
    v = voice_of(&sa, 57);
    CHECK(v && v->inc2 == v->inc);
    j6y_sound_load(&sa, &det);
    CHECK(v && v->inc == j6y_inc[57]);
    CHECK(v && v->inc2 == want2);
    render(&sa, bufa, SR);
    CHECK(rms_swing(bufa, 0, SR) < 0.7);              /* and it beats */
    j6y_sound_load(&sa, &t_saw);
    CHECK(v && v->inc2 == v->inc);
    /* with tUnE moved: a sound load keeps the tuning */
    j6_synth_set(&sa, J6_SP_TUNE10, 4480);
    j6y_sound_load(&sa, &det);
    CHECK(v && v->inc == (uint32_t)(((uint64_t)j6y_inc[57] * sa.tune_q16) >> 16));
    CHECK(v && v->inc2 == (uint32_t)(((uint64_t)v->inc * (65536u + 127u * 8u)) >> 16));
}

/* RMS of note 84 over note 36 through a low cutoff (233 Hz), for a key tracking amount */
static double key_track_ratio(uint32_t kt)
{
    j6_sound_t k = t_saw;
    double r[2];
    uint32_t n;
    k.knob[J6_KNOB_CUTOFF] = 40;
    k.key_track = (uint8_t)kt;
    for (n = 0; n < 2u; n++) {
        init_nofx(&sa);
        j6y_sound_load(&sa, &k);
        j6_synth_note_on(&sa, n ? 84u : 36u, 127);
        render(&sa, bufa, SR / 2u);
        r[n] = rms_d(bufa + 2u * (SR / 4u), SR / 4u);
    }
    return r[1] / r[0];
}

/* key tracking moves the cutoff with the note (127: a semitone per semitone, around note 60) */
static void test_key_tracking(void)
{
    double r0 = key_track_ratio(0), r64 = key_track_ratio(64), r127 = key_track_ratio(127);
    printf("    note 84 / note 36 rms through 233 Hz: key track 0 %.3f, 64 %.3f, 127 %.3f\n", r0, r64, r127);
    CHECK(r64 > 2.0 * r0);
    CHECK(r127 > 2.0 * r64);
    CHECK(r127 > 0.5 && r127 < 2.0);                  /* full tracking: about as bright 4 octaves apart */
}

/* the preset's decay: from the peak to 99 % of the way to its sustain in 5 ms .. 10 s (SD-6) */
static void test_preset_decay(void)
{
    static const uint32_t dec[] = {0, 30, 90, 127};
    const int32_t one = 1 << 30, sus = 64 * (one / 127), near = (one - sus) / 100;
    uint32_t k;
    for (k = 0; k < sizeof dec / sizeof dec[0]; k++) {
        j6_sound_t d = t_saw;
        double t_ms = 5.0 * pow_approx(2000.0, (double)dec[k] / 127.0);
        uint32_t f = 0, want = (uint32_t)(t_ms * 44.1);
        int32_t blk[2u * J6_CR];
        d.sustain = 64;
        d.decay = (uint8_t)dec[k];
        init_nofx(&sa);
        j6y_sound_load(&sa, &d);
        j6_synth_set(&sa, J6_SP_ATTACK, 0);
        j6_synth_note_on(&sa, 60, 100);
        while (sa.v[0].stage == J6Y_ST_ATTACK) j6_synth_render(&sa, blk, J6_CR);
        CHECK_EQ(sa.v[0].env, one);
        while (sa.v[0].env - sus > near && f < 12u * SR) {
            j6_synth_render(&sa, blk, J6_CR);
            f += J6_CR;
        }
        printf("    decay %3u: 99 %% of the way to sustain after %6u frames (nominal %.0f ms = %u)\n",
               (unsigned)dec[k], (unsigned)f, t_ms, (unsigned)want);
        CHECK(f <= want + want / 10u + 2u * J6_CR);
        CHECK(f + 2u * J6_CR >= want - want / 10u);
        render(&sa, bufa, J6_BLOCK);
        CHECK_RANGE(sa.v[0].env, sus - near, sus + near);   /* it settles there, still in decay */
        CHECK_EQ(sa.v[0].stage, J6Y_ST_DECAY);
    }
}

/* ---- delay (SD-4) -------------------------------------------------------------------------------- */

/* an impulse of 20000 into the effects (post vOL): the frame its first echo comes out, 0 if none */
static uint32_t first_echo(j6_synth_t *s, uint32_t max)
{
    int32_t lr[2u * J6_CR];
    uint32_t c, i;
    for (c = 0; c * J6_CR < max; c++) {
        memset(lr, 0, sizeof lr);
        if (c == 0) lr[0] = lr[1] = 20000;
        j6y_fx_chunk(s, lr);
        for (i = 0; i < J6_CR; i++) {
            if (c == 0 && i == 0) {
                CHECK_EQ(lr[0], 20000);              /* dry: the echo is never at 0 */
                continue;
            }
            if (lr[2u * i] != 0) return c * J6_CR + i;
        }
    }
    return 0;
}

static void delay_setup(j6_synth_t *s, int16_t *mem, uint32_t len)
{
    j6_synth_init(s, mem, len);
    j6_synth_set(s, J6_SP_DLY_LEVEL, 127);
    j6_synth_set(s, J6_SP_REV_LEVEL, 0);
}

static void test_delay_free_time(void)
{
    static const int32_t vals[] = {0, 1, 32, 64, 100, 127};
    uint32_t k, got;
    delay_setup(&sa, fxa, J6_FX_MEM_LEN);
    CHECK_EQ(sa.fx.dly_len, J6_FX_DELAY_LEN);
    for (k = 0; k < sizeof vals / sizeof vals[0]; k++) {
        j6_synth_panic(&sa);
        j6_synth_set(&sa, J6_SP_DLY_TIME, vals[k]);
        got = first_echo(&sa, 70000u);
        CHECK_EQ(got, j6y_dly_free[vals[k]]);
    }
    /* the range is 10 .. 1400 ms */
    CHECK_RANGE(j6y_dly_free[0], MS(10) - 1u, MS(10) + 1u);
    CHECK_RANGE(j6y_dly_free[127], MS(1400) - 1u, MS(1400) + 1u);
    /* the echo level follows DLY_LEVEL, and DLY_LEVEL 0 gives no echo at all */
    j6_synth_panic(&sa);
    j6_synth_set(&sa, J6_SP_DLY_TIME, 0);
    j6_synth_set(&sa, J6_SP_DLY_LEVEL, 0);
    CHECK_EQ(first_echo(&sa, 4000u), 0);
    CHECK_EQ(j6_synth_fx_idle(&sa), 1);
}

static void test_delay_sync_time(void)
{
    uint32_t idx, got;
    delay_setup(&sa, fxa, J6_FX_MEM_LEN);
    j6_synth_set(&sa, J6_SP_DLY_SYNC, 1);
    j6_synth_set(&sa, J6_SP_TEMPO10, 1200);
    for (idx = 0; idx < J6_DSYNC_VALUES; idx++) {
        int32_t v = (int32_t)((idx * 128u + 11u) / 12u);  /* the lowest knob value of this note value */
        uint32_t want = (j6_dsync_ticks(idx) * j6_spt_q12(1200)) >> 12;
        CHECK_EQ((uint32_t)v * 12u / 128u, idx);
        j6_synth_panic(&sa);
        j6_synth_set(&sa, J6_SP_DLY_TIME, v);
        got = first_echo(&sa, 70000u);
        CHECK_EQ(got, want);
    }
    /* 120 BPM, 1/8 = 48 ticks = 11025 samples (design 7.5) */
    j6_synth_panic(&sa);
    j6_synth_set(&sa, J6_SP_DLY_TIME, 54);
    CHECK_EQ(first_echo(&sa, 70000u), 11025);
    /* SD-4: the time follows the tempo: 60 BPM doubles it */
    j6_synth_panic(&sa);
    j6_synth_set(&sa, J6_SP_TEMPO10, 600);
    CHECK_EQ(first_echo(&sa, 70000u), 22050);
    /* longer than the line: halved until it fits. 40 BPM, 1/2 note = 192 ticks = 132300 -> 33075 */
    j6_synth_panic(&sa);
    j6_synth_set(&sa, J6_SP_TEMPO10, 400);
    j6_synth_set(&sa, J6_SP_DLY_TIME, 127);
    CHECK_EQ(first_echo(&sa, 70000u), 33075);
    /* d.Syn off: the tempo does not matter */
    j6_synth_panic(&sa);
    j6_synth_set(&sa, J6_SP_DLY_SYNC, 0);
    j6_synth_set(&sa, J6_SP_DLY_TIME, 64);
    got = first_echo(&sa, 70000u);
    j6_synth_panic(&sa);
    j6_synth_set(&sa, J6_SP_TEMPO10, 2400);
    CHECK_EQ(first_echo(&sa, 70000u), got);
    CHECK_EQ(got, j6y_dly_free[64]);
}

/* feedback about 40 %, darkened: each echo smaller, and the line empties to exactly 0 */
static void test_delay_feedback_decays(void)
{
    int32_t lr[2u * J6_CR];
    uint32_t c, i, d;
    int64_t e[4] = {0, 0, 0, 0};
    delay_setup(&sa, fxa, J6_FX_MEM_LEN);
    j6_synth_set(&sa, J6_SP_DLY_TIME, 20);
    d = j6y_dly_free[20];
    for (c = 0; c * J6_CR < 5u * d; c++) {
        memset(lr, 0, sizeof lr);
        if (c == 0) lr[0] = lr[1] = 20000;
        j6y_fx_chunk(&sa, lr);
        for (i = 0; i < J6_CR; i++) {
            uint32_t f = c * J6_CR + i, k = f / d;
            if (f > 0u && k >= 1u && k <= 4u) e[k - 1u] += lr[2u * i] < 0 ? -lr[2u * i] : lr[2u * i];
        }
    }
    printf("    delay echoes (sum |x|): %lld %lld %lld %lld\n", (long long)e[0], (long long)e[1], (long long)e[2],
           (long long)e[3]);
    CHECK(e[0] > 0 && e[1] > 0 && e[2] > 0);
    CHECK(e[1] * 100 < e[0] * 50 && e[1] * 100 > e[0] * 25);   /* about 40 % */
    CHECK(e[2] < e[1]);
    c = 0;
    while (!j6_synth_fx_idle(&sa) && c < 60u * SR / J6_CR) {
        memset(lr, 0, sizeof lr);
        j6y_fx_chunk(&sa, lr);
        c++;
    }
    CHECK_EQ(j6_synth_fx_idle(&sa), 1);
    for (c = 0; c < 100u; c++) {
        memset(lr, 0, sizeof lr);
        j6y_fx_chunk(&sa, lr);
        for (i = 0; i < 2u * J6_CR; i++) CHECK_EQ(lr[i], 0);
    }
}

/* a time change while the delay sounds glides (fractional read): no step in the wet signal */
static void test_delay_time_glides(void)
{
    int32_t lr[2u * J6_CR], prev = 0, maxd = 0, maxd_steady = 0;
    uint32_t c, i, f = 0;
    double ph = 0.0;
    delay_setup(&sa, fxa, J6_FX_MEM_LEN);
    j6_synth_set(&sa, J6_SP_DLY_TIME, 60);
    for (c = 0; c < 4u * SR / J6_CR; c++) {
        int32_t in[J6_CR];
        if (c == SR / J6_CR) j6_synth_set(&sa, J6_SP_DLY_TIME, 100);   /* 1 s in: ~100 ms -> ~490 ms */
        if (c == 2u * SR / J6_CR) j6_synth_set(&sa, J6_SP_DLY_TIME, 10);
        for (i = 0; i < J6_CR; i++) {
            /* 300 Hz sine, amplitude 10000 (a polynomial sine: no libm) */
            double x = ph - (double)(int)ph, s;
            x = x < 0.5 ? x : x - 1.0;
            s = 4.0 * x * (1.0 - 2.0 * (x < 0.0 ? -x : x)) * 2.0;
            in[i] = (int32_t)(10000.0 * s);
            lr[2u * i] = lr[2u * i + 1u] = in[i];
            ph += 300.0 / (double)SR;
        }
        j6y_fx_chunk(&sa, lr);
        for (i = 0; i < J6_CR; i++, f++) {
            int32_t wet = lr[2u * i] - in[i], dd = wet - prev;
            if (dd < 0) dd = -dd;
            if (f > 2u && f < SR / J6_CR * J6_CR) {   /* before the first change (frame 44096, not SR) */
                if (dd > maxd_steady) maxd_steady = dd;
            } else if (f > 2u) {
                if (dd > maxd) maxd = dd;
            }
            prev = wet;
        }
    }
    printf("    delay wet max step: steady %d, while gliding %d\n", (int)maxd_steady, (int)maxd);
    CHECK(maxd_steady > 0);
    CHECK(maxd < 3 * maxd_steady + 200);             /* a jump would be ~2 x 8900 */
    CHECK_EQ(sa.fx.dly_cur_q8, sa.fx.dly_tgt_q8);      /* it got there */
    CHECK_EQ(sa.fx.dly_tgt_q8, (int32_t)j6y_dly_free[10] * 256);
}

/* the effects on a short pool: the delay shrinks, the reverb needs its room, nothing outside is touched */
static void test_short_fx_len(void)
{
    static const uint32_t lens[] = {4096, 17126, 20000, 40000};
    uint32_t k, i;
    for (k = 0; k < sizeof lens / sizeof lens[0]; k++) {
        uint32_t len = lens[k], ok = 1;
        for (i = 0; i < J6_FX_MEM_LEN + 64u; i++) fxc[i] = 0x5A5A;
        delay_setup(&sc, fxc, len);
        CHECK(sc.fx.dly_len >= 4096u);
        CHECK(sc.fx.dly_len <= len);
        CHECK_EQ(sc.rv_on, len >= J6Y_RV_LEN + J6Y_CH_LEN + 4096u);
        if (sc.rv_on) CHECK(sc.fx.dly_len + J6Y_RV_LEN + J6Y_CH_LEN <= len);
        j6_synth_set(&sc, J6_SP_DLY_TIME, 127);       /* 1400 ms, halved into the line */
        {
            uint32_t want = 61740u;
            while (want > sc.fx.dly_len - 2u) want >>= 1;
            CHECK_EQ(first_echo(&sc, 70000u), want);
        }
        j6_synth_panic(&sc);
        j6_synth_sound(&sc, 59);                       /* Space: chorus, delay, reverb */
        j6_synth_note_on(&sc, 60, 127);
        j6_synth_note_on(&sc, 64, 127);
        render(&sc, bufc, SR);
        j6_synth_all_off(&sc);
        CHECK(until_fx_idle(&sc, 40u * SR) <= 40u * SR);
        for (i = len; i < J6_FX_MEM_LEN + 64u; i++)
            if (fxc[i] != 0x5A5A) ok = 0;
        CHECK(ok);
    }
}

/* ---- reverb -------------------------------------------------------------------------------------- */

static uint32_t reverb_tail(int32_t rev_time, uint32_t *tail_frames)
{
    uint32_t f, i, last = 0;
    j6_synth_init(&sa, fxa, J6_FX_MEM_LEN);
    j6_synth_sound(&sa, 16);                          /* Juno Brass: no chorus */
    j6_synth_set(&sa, J6_SP_DLY_LEVEL, 0);
    j6_synth_set(&sa, J6_SP_REV_LEVEL, 127);
    j6_synth_set(&sa, J6_SP_REV_TIME, rev_time);
    j6_synth_set(&sa, J6_SP_RELEASE, 10);
    j6_synth_note_on(&sa, 60, 127);
    j6_synth_note_on(&sa, 67, 127);
    render(&sa, bufa, SR / 2u);
    j6_synth_all_off(&sa);
    f = until_inactive(&sa, SR);
    CHECK(f <= SR);
    /* now only the reverb sounds: record its tail */
    render(&sa, bufa, 10u * SR);
    for (i = 0; i < 2u * 10u * SR; i++)
        if (bufa[i]) last = i / 2u;
    *tail_frames = last;
    f = until_fx_idle(&sa, 60u * SR);
    return f;
}

static void test_reverb_decays_to_idle(void)
{
    uint32_t tail0, tail1, idle0, idle1;
    idle0 = reverb_tail(0, &tail0);
    idle1 = reverb_tail(127, &tail1);
    printf("    reverb tail after the voices: time 0 %u frames, time 127 %u frames\n", (unsigned)tail0,
           (unsigned)tail1);
    CHECK(tail0 > 100u);                              /* there is a tail */
    CHECK(tail1 > 2u * tail0);                        /* REV_TIME lengthens it */
    CHECK(idle1 <= 60u * SR);                         /* and it ends */
    CHECK(idle0 <= 60u * SR);
    CHECK_EQ(j6_synth_fx_idle(&sa), 1);
    render(&sa, bufa, SR);
    CHECK(all_zero(bufa, SR));
    /* REV_LEVEL 0: no tail at all */
    j6_synth_init(&sa, fxa, J6_FX_MEM_LEN);
    j6_synth_sound(&sa, 16);
    j6_synth_set(&sa, J6_SP_DLY_LEVEL, 0);
    j6_synth_set(&sa, J6_SP_REV_LEVEL, 0);
    j6_synth_note_on(&sa, 60, 127);
    render(&sa, bufa, SR / 4u);
    j6_synth_all_off(&sa);
    until_inactive(&sa, SR);
    CHECK(sa.fx.rv_quiet >= J6Y_RV_QUIET);            /* the reverb never took any input */
    CHECK(sa.fx.dly_quiet >= sa.fx.dly_len);
    render(&sa, bufa, SR / 4u);
    CHECK(all_zero(bufa, SR / 4u));
    CHECK_EQ(j6_synth_fx_idle(&sa), 1);               /* and the chorus line (it records a dry sound too) is
                                                       * empty J6Y_CH_LEN frames after the voices */
}

/* the reverb (and the chorus) are stereo; without the effects the output is mono */
static void test_stereo(void)
{
    uint32_t i, diff = 0;
    j6_synth_init(&sa, fxa, J6_FX_MEM_LEN);
    j6_synth_sound(&sa, 0);                           /* chorus on */
    j6_synth_set(&sa, J6_SP_REV_LEVEL, 0);
    j6_synth_set(&sa, J6_SP_DLY_LEVEL, 0);
    j6_synth_note_on(&sa, 60, 100);
    render(&sa, bufa, SR / 2u);
    for (i = 0; i < SR / 2u; i++) diff += bufa[2u * i] != bufa[2u * i + 1u];
    CHECK(diff > SR / 4u);
    init_nofx(&sb);
    j6_synth_sound(&sb, 0);
    j6_synth_note_on(&sb, 60, 100);
    render(&sb, bufb, SR / 2u);
    diff = 0;
    for (i = 0; i < SR / 2u; i++) diff += bufb[2u * i] != bufb[2u * i + 1u];
    CHECK_EQ(diff, 0);
    /* a sound without chorus, effects at 0: mono too */
    j6_synth_init(&sa, fxa, J6_FX_MEM_LEN);
    j6_synth_sound(&sa, 16);
    j6_synth_set(&sa, J6_SP_REV_LEVEL, 0);
    j6_synth_set(&sa, J6_SP_DLY_LEVEL, 0);
    j6_synth_note_on(&sa, 60, 100);
    render(&sa, bufa, SR / 2u);
    diff = 0;
    for (i = 0; i < SR / 2u; i++) diff += bufa[2u * i] != bufa[2u * i + 1u];
    CHECK_EQ(diff, 0);
}

/* ---- housekeeping ---------------------------------------------------------------------------------- */

static void test_panic(void)
{
    j6_synth_init(&sa, fxa, J6_FX_MEM_LEN);
    j6_synth_sound(&sa, 59);                          /* Space: everything on */
    j6_synth_note_on(&sa, 60, 127);
    j6_synth_note_on(&sa, 64, 127);
    j6_synth_note_on(&sa, 67, 127);
    render(&sa, bufa, SR);
    CHECK_EQ(j6_synth_active(&sa), 3);
    CHECK_EQ(j6_synth_fx_idle(&sa), 0);
    j6_synth_panic(&sa);
    CHECK_EQ(j6_synth_active(&sa), 0);
    CHECK_EQ(j6_synth_fx_idle(&sa), 1);
    render(&sa, bufa, SR);
    CHECK(all_zero(bufa, SR));
    j6_synth_note_on(&sa, 60, 100);                   /* and it plays again afterwards */
    render(&sa, bufa, SR / 4u);
    CHECK(!all_zero(bufa, SR / 4u));
}

/* the output does not depend on how the frames are split into render calls */
static void script(j6_synth_t *s, uint32_t blk)
{
    switch (blk) {
    case 0:
        j6_synth_sound(s, 59);
        j6_synth_note_on(s, 60, 100);
        j6_synth_note_on(s, 64, 90);
        j6_synth_note_on(s, 67, 80);
        break;
    case 20:
        j6_synth_note_on(s, 72, 127);
        j6_synth_set(s, J6_SP_CUTOFF, 30);
        break;
    case 40:
        j6_synth_note_on(s, 76, 110);                 /* steals 60 */
        j6_synth_set(s, J6_SP_RESO, 120);
        break;
    case 60:
        j6_synth_note_off(s, 64);
        j6_synth_set(s, J6_SP_DLY_TIME, 110);         /* glides */
        j6_synth_set(s, J6_SP_VOLUME, 60);            /* ramps */
        break;
    case 80:
        j6_synth_set(s, J6_SP_REV_TIME, 127);
        j6_synth_note_on(s, 48, 127);
        j6_synth_sound(s, 0);
        break;
    case 100:
        j6_synth_set(s, J6_SP_TUNE10, 4480);
        j6_synth_set(s, J6_SP_DLY_SYNC, 1);
        break;
    case 140:
        j6_synth_all_off(s);
        break;
    default:
        break;
    }
}

static void test_chunking_identical(void)
{
    static const uint32_t split[] = {16, 48, 32, 64, 96};   /* sums to 256 */
    const uint32_t blocks = 400;
    uint32_t b, k;
    j6_synth_init(&sa, fxa, J6_FX_MEM_LEN);
    j6_synth_init(&sb, fxb, J6_FX_MEM_LEN);
    j6_synth_init(&sc, fxc, J6_FX_MEM_LEN);
    memset(bufa, 0, (size_t)blocks * J6_BLOCK * 2u * sizeof bufa[0]);
    memset(bufb, 0, (size_t)blocks * J6_BLOCK * 2u * sizeof bufb[0]);
    memset(bufc, 0, (size_t)blocks * J6_BLOCK * 2u * sizeof bufc[0]);
    for (b = 0; b < blocks; b++) {
        int32_t *pa = bufa + 2u * J6_BLOCK * b, *pb = bufb + 2u * J6_BLOCK * b, *pc = bufc + 2u * J6_BLOCK * b;
        uint32_t f;
        script(&sa, b);
        script(&sb, b);
        script(&sc, b);
        for (f = 0; f < J6_BLOCK; f += J6_CR) j6_synth_render(&sa, pa + 2u * f, J6_CR);
        j6_synth_render(&sb, pb, J6_BLOCK);
        for (k = 0, f = 0; k < sizeof split / sizeof split[0]; k++) {
            j6_synth_render(&sc, pc + 2u * f, split[k]);
            f += split[k];
        }
    }
    CHECK_MEM(bufa, bufb, (size_t)blocks * J6_BLOCK * 2u * sizeof bufa[0]);
    CHECK_MEM(bufa, bufc, (size_t)blocks * J6_BLOCK * 2u * sizeof bufa[0]);
    CHECK(!all_zero(bufa, blocks * J6_BLOCK));
    /* n that is not a multiple of 16 renders only the whole chunks (the contract says it never happens) */
    memset(bufa, 0, 64u * sizeof bufa[0]);
    j6_synth_render(&sa, bufa, 8);
    CHECK(all_zero(bufa, 16u));
}

/* SD-1, SD-3: 64 sounds in 8 banks; each loads its own knob values and sounds */
static void test_presets(void)
{
    uint32_t i, j, k, bad_name = 0, dup = 0, silent = 0, stuck = 0, knobs_ok = 1, fields_ok = 1;
    CHECK_EQ(sizeof j6_sounds / sizeof j6_sounds[0], 64);
    CHECK_EQ(J6_SOUNDS, J6_BANKS * 8u);
    for (i = 0; i < J6_SOUNDS; i++) {
        const j6_sound_t *p = &j6_sounds[i];
        size_t n = strnlen_12(p->name);
        int32_t peak;
        if (n < 1u || n > 11u) bad_name++;
        for (j = 0; j < n; j++)
            if (p->name[j] < 32 || p->name[j] > 126) bad_name++;
        for (j = 0; j < i; j++)
            if (strcmp(p->name, j6_sounds[j].name) == 0) dup++;
        if (p->saw > 127u || p->pulse > 127u || p->pw > 127u || p->sub > 127u || p->noise > 127u ||
            p->decay > 127u || p->sustain > 127u || p->env_amt > 127u || p->key_track > 127u ||
            p->detune > 127u || p->level > 127u || p->level == 0u ||
            (p->flags & ~(uint32_t)(J6_SND_CHORUS | J6_SND_HPF_MASK | J6_SND_CHMODE_MASK)) ||
            ((p->flags & J6_SND_CHMODE_MASK) >> J6_SND_CHMODE_SHIFT) > 2u)
            fields_ok = 0;
        init_nofx(&sa);
        j6_synth_set(&sa, J6_SP_CUTOFF, 3);           /* a moved knob ... */
        j6_synth_sound(&sa, i);                       /* ... takes the sound's value again */
        for (k = 0; k < J6_KNOBS; k++) {
            if (p->knob[k] > 127u) fields_ok = 0;
            if (j6_synth_get(&sa, k) != (int32_t)p->knob[k]) knobs_ok = 0;
        }
        j6_synth_note_on(&sa, 60, 100);
        j6_synth_note_on(&sa, 64, 100);
        render(&sa, bufa, MS(600));
        peak = j6t_peak(bufa, MS(600));
        if (peak < 300) {
            silent++;
            printf("    preset %u %s: peak %d\n", (unsigned)i, p->name, (int)peak);
        }
        j6_synth_all_off(&sa);
        if (until_inactive(&sa, 6u * SR) > 6u * SR) stuck++;
    }
    CHECK_EQ(bad_name, 0);
    CHECK_EQ(dup, 0);
    CHECK_EQ(silent, 0);
    CHECK_EQ(stuck, 0);
    CHECK(knobs_ok);
    CHECK(fields_ok);
    /* SD-3: a knob replaces the sound's value (no pickup) */
    init_nofx(&sa);
    j6_synth_sound(&sa, 5);
    j6_synth_set(&sa, J6_SP_REV_TIME, 7);
    CHECK_EQ(j6_synth_get(&sa, J6_SP_REV_TIME), 7);
    j6_synth_sound(&sa, 99);                          /* out of range: the last sound */
    CHECK(sa.snd == &j6_sounds[63]);
    CHECK_EQ(j6_synth_get(&sa, J6_SP_CUTOFF), j6_sounds[63].knob[0]);
}

/* design 7.1 levels: every preset with its own knobs, 4-note chords at velocity 127 (A.vLo accents), vOL
 * 100, delay and reverb as the preset sets them: the output never reaches the clamp. The presets'
 * `level` fields are balanced so these chords peak around -2.7 dBFS. */
static void test_preset_headroom(void)
{
    static const uint8_t chords[3][4] = {{36, 43, 48, 55}, {48, 55, 62, 64}, {60, 67, 72, 76}};
    const uint32_t held = CHUNK_UP(SR), tail = CHUNK_UP(SR / 2u);
    uint32_t i, c, k, hot = 0;
    int32_t worst = 0, quietest = J6_OUT_MAX;
    for (i = 0; i < J6_SOUNDS; i++) {
        int32_t pk = 0;
        for (c = 0; c < 3u; c++) {
            j6_synth_init(&sa, fxa, J6_FX_MEM_LEN);
            j6_synth_sound(&sa, i);
            for (k = 0; k < 4u; k++) j6_synth_note_on(&sa, chords[c][k], 127);
            render(&sa, bufa, held);
            j6_synth_all_off(&sa);
            render(&sa, bufa + 2u * held, tail);
            if (j6t_peak(bufa, held + tail) > pk) pk = j6t_peak(bufa, held + tail);
        }
        if (pk >= J6_OUT_MAX) {
            hot++;
            printf("    preset %u %s reaches the output clamp\n", (unsigned)i, j6_sounds[i].name);
        }
        if (pk > worst) worst = pk;
        if (pk < quietest) quietest = pk;
    }
    printf("    4-note chords at vel 127, vOL 100: loudest preset peak %d, quietest %d\n", (int)worst,
           (int)quietest);
    CHECK_EQ(hot, 0);
    CHECK(worst < 28000);                             /* about 1.4 dB of headroom at least */
    CHECK(quietest > 3000);
}

/* parameters: defaults, clamping (j6.h 9.6) */
static void test_params(void)
{
    uint32_t k;
    j6_synth_init(&sa, fxa, J6_FX_MEM_LEN);
    CHECK(sa.snd == &j6_sounds[0]);                   /* init loads sound 0 */
    for (k = 0; k < J6_KNOBS; k++) CHECK_EQ(j6_synth_get(&sa, k), j6_sounds[0].knob[k]);
    CHECK_EQ(j6_synth_get(&sa, J6_SP_VOLUME), 100);   /* vOL default 100 [MANUAL FM *1] */
    CHECK_EQ(j6_synth_get(&sa, J6_SP_DLY_SYNC), 0);   /* d.Syn OFF */
    CHECK_EQ(j6_synth_get(&sa, J6_SP_TEMPO10), 1200);
    CHECK_EQ(j6_synth_get(&sa, J6_SP_TUNE10), 4400);  /* tUnE 440.0 [MANUAL FM] */
    for (k = 0; k < 9u; k++) {                        /* the knobs and vOL: 0..127 */
        j6_synth_set(&sa, k, -5);
        CHECK_EQ(j6_synth_get(&sa, k), 0);
        j6_synth_set(&sa, k, 300);
        CHECK_EQ(j6_synth_get(&sa, k), 127);
        j6_synth_set(&sa, k, 77);
        CHECK_EQ(j6_synth_get(&sa, k), 77);
    }
    j6_synth_set(&sa, J6_SP_DLY_SYNC, 5);
    CHECK_EQ(j6_synth_get(&sa, J6_SP_DLY_SYNC), 1);
    j6_synth_set(&sa, J6_SP_TEMPO10, 0);
    CHECK_EQ(j6_synth_get(&sa, J6_SP_TEMPO10), 400);
    j6_synth_set(&sa, J6_SP_TEMPO10, 99999);
    CHECK_EQ(j6_synth_get(&sa, J6_SP_TEMPO10), 3000);
    j6_synth_set(&sa, J6_SP_TUNE10, 4000);            /* 433.0 .. 448.0 Hz */
    CHECK_EQ(j6_synth_get(&sa, J6_SP_TUNE10), 4330);
    j6_synth_set(&sa, J6_SP_TUNE10, 5000);
    CHECK_EQ(j6_synth_get(&sa, J6_SP_TUNE10), 4480);
    j6_synth_set(&sa, J6_SP_COUNT, 5);                /* unknown: ignored */
    CHECK_EQ(j6_synth_get(&sa, J6_SP_COUNT), 0);
    CHECK_EQ(j6_synth_get(&sa, 1000), 0);
    /* vOL gain vol^2 * 4096 / 10000 (SD-7) */
    j6_synth_set(&sa, J6_SP_VOLUME, 100);
    CHECK_EQ(sa.vol_q12, 4096);
    j6_synth_set(&sa, J6_SP_VOLUME, 127);
    CHECK_EQ(sa.vol_q12, 127 * 127 * 4096 / 10000);
    j6_synth_set(&sa, J6_SP_VOLUME, 0);
    CHECK_EQ(sa.vol_q12, 0);
}

/* SD-7: vOL is a gain before the effects: 0 = silence (no echo either), 127 = +4.1 dB */
static void test_volume(void)
{
    double r100, r127;
    j6_synth_init(&sa, fxa, J6_FX_MEM_LEN);
    j6_synth_sound(&sa, 46);                          /* Echo Lead: loud delay */
    j6_synth_set(&sa, J6_SP_VOLUME, 0);
    j6_synth_note_on(&sa, 60, 127);
    render(&sa, bufa, SR);
    j6_synth_all_off(&sa);
    render(&sa, bufb, 2u * SR);
    CHECK(all_zero(bufa, SR));
    CHECK(all_zero(bufb, 2u * SR));
    CHECK_EQ(j6_synth_fx_idle(&sa), 1);
    init_nofx(&sa);
    j6y_sound_load(&sa, &t_saw);
    j6_synth_note_on(&sa, 57, 100);
    render(&sa, bufa, SR / 2u);
    r100 = rms_d(bufa + 2u * (SR / 4u), SR / 4u);
    init_nofx(&sa);
    j6y_sound_load(&sa, &t_saw);
    j6_synth_set(&sa, J6_SP_VOLUME, 127);
    j6_synth_note_on(&sa, 57, 100);
    render(&sa, bufa, SR / 2u);
    r127 = rms_d(bufa + 2u * (SR / 4u), SR / 4u);
    CHECK(r127 / r100 > 1.613 * 0.97 && r127 / r100 < 1.613 * 1.03);
    /* a change ramps: no step bigger than the signal's own (a smooth sound: a saw's own edges would
     * hide a click), and the gain takes 16 chunks to get there, not one */
    {
        const uint32_t sw = CHUNK_UP(SR / 4u);
        uint32_t c, chunks = 0;
        init_nofx(&sa);
        j6y_sound_load(&sa, &t_soft);
        j6_synth_note_on(&sa, 45, 127);
        render(&sa, bufa, sw);
        CHECK_EQ(sa.vol_cur_q12, 4096);
        j6_synth_set(&sa, J6_SP_VOLUME, 0);
        memset(bufa + 2u * sw, 0, (size_t)MS(20) * 2u * sizeof bufa[0]);
        for (c = 0; c * J6_CR < MS(20); c++) {
            j6_synth_render(&sa, bufa + 2u * (sw + c * J6_CR), J6_CR);
            if (c == 0) CHECK_RANGE(sa.vol_cur_q12, 1, 4095);   /* still on its way after one chunk */
            if (sa.vol_cur_q12 == 0 && !chunks) chunks = c + 1u;
        }
        CHECK_EQ(chunks, 16);                               /* 1/16 of unity per chunk */
        CHECK_EQ(sa.vol_cur_q12, 0);
        CHECK(all_zero(bufa + 2u * (sw + MS(10)), MS(10)));
        CHECK(max_step(bufa, sw - 1u, sw + MS(20)) <= max_step(bufa, sw / 2u, sw) + 32);
        CHECK(max_step(bufa, sw / 2u, sw) > 100);           /* the signal is there to compare with */
    }
}

/* the out buffer is ADDed to */
static void test_render_adds(void)
{
    const uint32_t nf = CHUNK_UP(SR / 4u);
    uint32_t i, ok = 1;
    init_nofx(&sa);
    j6y_sound_load(&sa, &t_saw);
    j6_synth_note_on(&sa, 60, 100);
    init_nofx(&sb);
    j6y_sound_load(&sb, &t_saw);
    j6_synth_note_on(&sb, 60, 100);
    render(&sa, bufa, nf);
    for (i = 0; i < 2u * nf; i++) bufb[i] = 1000;
    for (i = 0; i < nf; i += J6_BLOCK) j6_synth_render(&sb, bufb + 2u * i, nf - i < J6_BLOCK ? nf - i : J6_BLOCK);
    for (i = 0; i < 2u * nf; i++)
        if (bufb[i] != j6_clamp(bufa[i] + 1000, -J6_OUT_MAX, J6_OUT_MAX)) ok = 0;
    CHECK(ok);
}

/* a sound change while notes are held: they keep sounding, and no switch clicks. The oscillator mix and
 * the knobs change at once, so the steps around a switch may exceed the signal's own a little, but not by
 * a click's margin (a cut tail, a dry gain jump, an empty chorus line coming in).
 * DLY_TIME is held at one value here. A delay time change glides (SD-4): the echoes play faster or slower,
 * so their steps change with whatever the line holds, and the steps next to the switch tell nothing about
 * those. Example: from Noise Swp (490 ms) to Wind (120 ms, DLY_LEVEL 0), the read position, which was
 * standing still (a time growing at the slew limit: the echo a constant), sweeps at twice the speed over
 * the 12 % pulse edge Thin Lead left in the line 290 ms before (up to 940 per 2 slots, Q14) while the
 * delay fades out (0.29 by then): a wet step of 278 against 71 steady around it. That is the stored
 * signal, not a click. test_sound_change_delay_glide checks the glide on a held smooth note, where the
 * stored signal is known. */
static void test_sound_change_while_playing(void)
{
    const uint32_t seg = CHUNK_UP(MS(30)), win = 320u;   /* the ramps take 256 frames */
    uint32_t i, clicks = 0;
    int32_t worst = 0;
    j6_synth_init(&sa, fxa, J6_FX_MEM_LEN);
    j6_synth_set(&sa, J6_SP_DLY_TIME, 64);
    j6_synth_note_on(&sa, 48, 120);
    j6_synth_note_on(&sa, 55, 120);
    render(&sa, bufa, seg);
    for (i = 1; i < J6_SOUNDS; i++) {
        int32_t before, at, after, steady;
        j6_synth_sound(&sa, i);
        j6_synth_set(&sa, J6_SP_DLY_TIME, 64);
        render(&sa, bufa + 2u * seg, seg);
        before = max_step(bufa, seg / 3u, seg);
        at = max_step(bufa, seg - 1u, seg + win);
        after = max_step(bufa, seg + win, 2u * seg);
        steady = before > after ? before : after;
        if (at * 100 / (steady + 32) > worst) worst = at * 100 / (steady + 32);
        if (at > 3 * steady + 64) {
            clicks++;
            printf("    sound %u -> %u (%s): step %d at the switch, steady %d / %d\n", (unsigned)(i - 1u),
                   (unsigned)i, j6_sounds[i].name, (int)at, (int)before, (int)after);
        }
        memmove(bufa, bufa + 2u * seg, (size_t)seg * 2u * sizeof bufa[0]);
    }
    printf("    63 sound changes mid-chord: largest step at a switch %d %% of the steady one\n", (int)worst);
    CHECK_EQ(clicks, 0);
    CHECK_EQ(j6_synth_active(&sa), 2);
    j6_synth_panic(&sa);
}

/* one sound change while a smooth note sounds through the delay, rendered twice: as it is, and with both
 * sounds' DLY_LEVEL at 0 (the dry signal alone; the reverb is off in both). The difference is the echo
 * alone. Returns the echo's largest step in the `win` frames from the switch on; *steady is its largest
 * step in the 0.5 s before, *peak its largest value there. */
static int32_t glide_switch(const j6_sound_t *from, const j6_sound_t *to, uint32_t win, int32_t *steady,
                            int32_t *peak)
{
    const uint32_t sw = CHUNK_UP(SR);
    j6_sound_t dfrom = *from, dto = *to;
    uint32_t i;
    int32_t d0;
    dfrom.knob[J6_KNOB_DLY_LEVEL] = 0;
    dto.knob[J6_KNOB_DLY_LEVEL] = 0;
    j6_synth_init(&sa, fxa, J6_FX_MEM_LEN);
    j6_synth_init(&sb, fxb, J6_FX_MEM_LEN);
    j6y_sound_load(&sa, from);
    j6y_sound_load(&sb, &dfrom);
    j6_synth_note_on(&sa, 45, 127);
    j6_synth_note_on(&sb, 45, 127);
    render(&sa, bufa, sw);                            /* 1 s: the line holds the note, echoes settled */
    render(&sb, bufb, sw);
    j6y_sound_load(&sa, to);
    j6y_sound_load(&sb, &dto);
    d0 = sa.fx.dly_cur_q8;
    render(&sa, bufa + 2u * sw, J6_CR);
    CHECK(sa.fx.dly_cur_q8 != d0);                    /* the time moves ... */
    CHECK_RANGE(sa.fx.dly_cur_q8 - d0, -256 * (int32_t)J6_CR, 256 * (int32_t)J6_CR);   /* ... <= 1 per sample */
    render(&sa, bufa + 2u * (sw + J6_CR), win - J6_CR);
    render(&sb, bufb + 2u * sw, win);
    for (i = 0; i < 2u * (sw + win); i++) bufc[i] = bufa[i] - bufb[i];
    *steady = max_step(bufc, sw / 2u, sw);
    *peak = j6t_peak(bufc + 2u * (sw / 2u), sw / 2u);
    return max_step(bufc, sw - 1u, sw + win);
}

/* SD-4 on a sound change (a PATTERN change during playback): the new DLY_TIME glides like a knob turn.
 * The read position moves at most 1 sample per sample and never jumps, so the echo plays at up to twice
 * its speed (a shorter time) or slower, down to a standstill (a longer time). Each window below only
 * reads what the line held before the switch (the echoes settled on a held smooth note), so the echo's
 * steps must stay within twice its steady ones, or below them. A jump of the read position would step
 * by up to twice the echo's peak. */
static void test_sound_change_delay_glide(void)
{
    j6_sound_t slow = t_soft, fast = t_soft, gone = t_soft;
    int32_t m, steady, peak;
    slow.knob[J6_KNOB_DLY_LEVEL] = 100;
    slow.knob[J6_KNOB_DLY_TIME] = 100;                /* 490 ms = 21609 samples */
    fast.knob[J6_KNOB_DLY_LEVEL] = 100;
    fast.knob[J6_KNOB_DLY_TIME] = 64;                 /* 120 ms = 5292 samples */
    gone.knob[J6_KNOB_DLY_TIME] = 64;                 /* 120 ms and DLY_LEVEL 0 (as Noise Swp -> Wind) */
    /* shorter: at twice the speed the read reaches the switch after 21609 / 2 frames */
    m = glide_switch(&slow, &fast, 8192u, &steady, &peak);
    printf("    echo, 490 -> 120 ms: max step %d in 8192 frames, steady %d (peak %d)\n", (int)m, (int)steady,
           (int)peak);
    CHECK(steady > 50);
    CHECK(peak > 8 * steady);                         /* a jump would show */
    CHECK(m > steady + steady / 2);                   /* it plays faster ... */
    CHECK(m <= 2 * steady + 8);                       /* ... at most twice as fast */
    /* longer: slower than 1 sample per sample, the read stays before the switch for 5292 frames */
    m = glide_switch(&fast, &slow, 4096u, &steady, &peak);
    printf("    echo, 120 -> 490 ms: max step %d in 4096 frames, steady %d (peak %d)\n", (int)m, (int)steady,
           (int)peak);
    CHECK(peak > 8 * steady);
    CHECK(m < steady * 3 / 4);                        /* slower (at the slew limit: standing still) */
    /* shorter while the delay fades out: no faster than twice, and then gone */
    m = glide_switch(&slow, &gone, 8192u, &steady, &peak);
    printf("    echo, 490 -> 120 ms and DLY_LEVEL 100 -> 0: max step %d, steady %d\n", (int)m, (int)steady);
    CHECK(m <= 2 * steady + 8);
    CHECK_EQ(sa.fx.dly_lvl, 0);
    CHECK(all_zero(bufc + 2u * (CHUNK_UP(SR) + 512u), 8192u - 512u));
}

/* SD-7 chorus on / off while a note sounds (a PATTERN change from a bass pattern to a pad one): it ramps
 * in and out over 16 chunks. A dry sound with the delay and reverb at 0 leaves every effect but the
 * chorus line idle while the note sounds: that is when a jump of the mix (dry gain 1 -> 0.75 in one
 * sample) slipped through, and an empty chorus line would come in with a step when its first sample
 * reached the taps. */
static void test_chorus_switch_mid_note(void)
{
    j6_sound_t wet = t_soft;
    const uint32_t sw = CHUNK_UP(SR / 2u), sw2 = sw + CHUNK_UP(SR / 2u), end = sw2 + CHUNK_UP(SR / 2u);
    wet.flags = J6_SND_CHORUS;
    j6_synth_init(&sa, fxa, J6_FX_MEM_LEN);
    j6y_sound_load(&sa, &t_soft);
    j6_synth_note_on(&sa, 45, 127);
    render(&sa, bufa, sw);
    CHECK_EQ(sa.fx.ch_mix, 0);
    CHECK_EQ(sa.fx.dly_quiet, sa.fx.dly_len);         /* delay and reverb idle */
    CHECK_EQ(sa.fx.rv_quiet, J6Y_RV_QUIET);
    j6y_sound_load(&sa, &wet);                        /* chorus on */
    CHECK_EQ(sa.fx.ch_mix, 0);                        /* a voice sounds: no jump */
    render(&sa, bufa + 2u * sw, J6_CR);
    CHECK_RANGE(sa.fx.ch_mix, 1, 32767);              /* a ramp, more than one chunk long */
    render(&sa, bufa + 2u * (sw + J6_CR), sw2 - sw - J6_CR);
    CHECK_EQ(sa.fx.ch_mix, 32768);
    CHECK(no_click("chorus on mid-note", bufa, sw, 512u, sw2));
    j6y_sound_load(&sa, &t_soft);                     /* chorus off */
    render(&sa, bufa + 2u * sw2, J6_CR);
    CHECK_RANGE(sa.fx.ch_mix, 1, 32767);
    render(&sa, bufa + 2u * (sw2 + J6_CR), end - sw2 - J6_CR);
    CHECK_EQ(sa.fx.ch_mix, 0);
    CHECK(no_click("chorus off mid-note", bufa, sw2, 512u, end));
    /* nothing sounding: the mix just takes the new value */
    j6_synth_panic(&sa);
    j6y_sound_load(&sa, &wet);
    CHECK_EQ(sa.fx.ch_mix, 32768);
}

/* a 300 Hz sine of amplitude 10000 straight into the effects (post vOL) for `frames`; level `param` jumps
 * from 127 to 0 at `sw`. Returns 1 if the wet signal fades out without a click. */
static int fx_level_cut(uint32_t param, uint32_t sw, uint32_t frames)
{
    int32_t lr[2u * J6_CR], *wet = bufa;
    uint32_t c, i, f = 0;
    double ph = 0.0;
    j6_synth_init(&sa, fxa, J6_FX_MEM_LEN);
    j6_synth_set(&sa, J6_SP_DLY_LEVEL, param == J6_SP_DLY_LEVEL ? 127 : 0);
    j6_synth_set(&sa, J6_SP_REV_LEVEL, param == J6_SP_REV_LEVEL ? 127 : 0);
    j6_synth_set(&sa, J6_SP_DLY_TIME, 60);
    for (c = 0; c * J6_CR < frames; c++) {
        int32_t in[J6_CR];
        if (c * J6_CR == sw) j6_synth_set(&sa, param, 0);
        if (c * J6_CR == sw + J6_CR) {
            int32_t now = param == J6_SP_DLY_LEVEL ? sa.fx.dly_lvl : sa.fx.rv_lvl;
            CHECK(now > 0 && now < 127 * (param == J6_SP_DLY_LEVEL ? 230 : 258));   /* on its way */
        }
        for (i = 0; i < J6_CR; i++) {
            double x = ph - (double)(int)ph, v;
            x = x < 0.5 ? x : x - 1.0;
            v = 4.0 * x * (1.0 - 2.0 * (x < 0.0 ? -x : x)) * 2.0;
            in[i] = (int32_t)(10000.0 * v);
            lr[2u * i] = lr[2u * i + 1u] = in[i];
            ph += 300.0 / (double)SR;
        }
        j6y_fx_chunk(&sa, lr);
        for (i = 0; i < J6_CR; i++, f++) wet[2u * f] = wet[2u * f + 1u] = lr[2u * i] - in[i];
    }
    CHECK_EQ(param == J6_SP_DLY_LEVEL ? sa.fx.dly_lvl : sa.fx.rv_lvl, 0);
    CHECK(max_step(wet, sw / 2u, sw) > 100);          /* there was a wet signal to cut */
    CHECK(all_zero(wet + 2u * (sw + 512u), frames - sw - 512u));
    return no_click(param == J6_SP_DLY_LEVEL ? "DLY_LEVEL 127 -> 0, wet" : "REV_LEVEL 127 -> 0, wet", wet, sw,
                    512u, sw);
}

/* a smooth note; `param` jumps from 0 to 127 at `sw`: the first echo / the reverb comes in without a step */
static int fx_level_onset(uint32_t param, uint32_t sw, uint32_t win, uint32_t frames)
{
    j6_synth_init(&sa, fxa, J6_FX_MEM_LEN);
    j6y_sound_load(&sa, &t_soft);
    j6_synth_set(&sa, J6_SP_DLY_TIME, 30);           /* 32 ms */
    j6_synth_set(&sa, J6_SP_REV_TIME, 100);
    j6_synth_note_on(&sa, 45, 127);
    render(&sa, bufa, sw);
    j6_synth_set(&sa, param, 127);
    render(&sa, bufa + 2u * sw, J6_CR);
    CHECK_RANGE(param == J6_SP_DLY_LEVEL ? sa.fx.dly_in : sa.fx.rv_in, 1, 32767);   /* the input gate ramps */
    render(&sa, bufa + 2u * (sw + J6_CR), frames - sw - J6_CR);
    return no_click(param == J6_SP_DLY_LEVEL ? "DLY_LEVEL 0 -> 127, first echo" : "REV_LEVEL 0 -> 127, onset",
                    bufa, sw, win, frames);
}

/* DLY_LEVEL and REV_LEVEL ramp, like vOL (SD-3: a knob jumps with no pickup, and PATTERN loads the
 * pattern's knob values): a ringing tail fades out over 16 chunks instead of being cut in one sample, and
 * the input gate opens the same way, so the first echo does not start with a step either */
static void test_fx_level_ramps(void)
{
    const uint32_t sw = CHUNK_UP(SR / 2u);
    CHECK(fx_level_cut(J6_SP_DLY_LEVEL, sw, sw + SR / 4u));
    CHECK(fx_level_cut(J6_SP_REV_LEVEL, sw, sw + SR / 4u));
    CHECK(fx_level_onset(J6_SP_DLY_LEVEL, sw, j6y_dly_free[30] + 512u, sw + SR / 2u));
    CHECK(fx_level_onset(J6_SP_REV_LEVEL, sw, 1400u + 512u, sw + SR / 2u));
    /* once the note is released, the tails drain and the effects go idle */
    j6_synth_all_off(&sa);
    CHECK(until_fx_idle(&sa, 60u * SR) <= 60u * SR);
    CHECK_EQ(j6_synth_active(&sa), 0);
    CHECK_EQ(j6_synth_fx_idle(&sa), 1);
    render(&sa, bufa, SR / 4u);
    CHECK(all_zero(bufa, SR / 4u));
    /* nothing sounding and the lines empty: a level change applies at once */
    j6_synth_panic(&sa);
    j6_synth_set(&sa, J6_SP_DLY_LEVEL, 100);
    j6_synth_set(&sa, J6_SP_REV_LEVEL, 50);
    CHECK_EQ(sa.fx.dly_lvl, 100 * 230);
    CHECK_EQ(sa.fx.dly_in, 32768);
    CHECK_EQ(sa.fx.rv_lvl, 50 * 258);
    CHECK_EQ(sa.fx.rv_in, 32768);
}

/* no voice sounds, but the effect in front still rings (the delay's echoes into the reverb, the chorus
 * tail into the delay): `param` jumps from 0 to 127 then. The input gate must ramp, not snap open, or the
 * ringing tail enters the line with a step that comes out one line length later. sa plays, sb is its twin
 * that keeps `param` at 0; sa - sb is what the switched effect adds. Returns that signal's largest step as it starts. */
static int32_t fx_gate_tail(const j6_sound_t *snd, uint32_t param, uint32_t frames)
{
    uint32_t k, i;
    j6_synth_t *t[2] = {&sa, &sb};
    int16_t *m[2] = {fxa, fxb};
    for (k = 0; k < 2u; k++) {
        j6_synth_init(t[k], m[k], J6_FX_MEM_LEN);
        j6y_sound_load(t[k], snd);
        j6_synth_set(t[k], J6_SP_DLY_TIME, 64);
        j6_synth_set(t[k], J6_SP_REV_TIME, 100);
        j6_synth_set(t[k], J6_SP_DLY_LEVEL, param == J6_SP_DLY_LEVEL ? 0 : 127);
        j6_synth_set(t[k], J6_SP_REV_LEVEL, 0);
        j6_synth_note_on(t[k], 45, 127);
        j6_synth_note_on(t[k], 52, 127);
        render(t[k], bufa, SR / 4u);
        j6_synth_all_off(t[k]);
        CHECK(until_inactive(t[k], 10u * SR) <= 10u * SR);
    }
    if (param == J6_SP_DLY_LEVEL) {
        CHECK(sa.fx.ch_quiet < J6Y_CH_LEN);           /* the chorus line still sends its tail */
    } else {
        CHECK(sa.fx.dly_quiet < sa.fx.dly_len);       /* the echoes still ring */
        CHECK(sa.fx.rv_quiet >= J6Y_RV_QUIET);        /* into an empty reverb */
    }
    j6_synth_set(&sa, param, 127);
    render(&sa, bufa, J6_CR);
    render(&sb, bufb, J6_CR);
    CHECK_RANGE(param == J6_SP_DLY_LEVEL ? sa.fx.dly_in : sa.fx.rv_in, 1, 32767);   /* the gate ramps */
    render(&sa, bufa + 2u * J6_CR, frames - J6_CR);
    render(&sb, bufb + 2u * J6_CR, frames - J6_CR);
    for (i = 0; i < 2u * frames; i++) bufc[i] = bufa[i] - bufb[i];
    for (i = 0; i < frames && !bufc[2u * i] && !bufc[2u * i + 1u]; i++) {}
    CHECK(i + 64u < frames);                          /* the effect did come in */
    if (i + 64u >= frames) return 0x7fffffff;
    printf("    onset at frame %u, steady step %d\n", (unsigned)i, (int)max_step(bufc, i + 32u, frames));
    return max_step(bufc, i ? i - 1u : 0u, i + 32u);  /* its first samples: from silence */
}

/* a gate snapped open on a ringing tail starts the wet signal with a step out of silence (48 for the
 * reverb, 16 for the delay measured with the snap); ramped, it starts within a quantum or two */
static void test_fx_gate_tail(void)
{
    j6_sound_t ch = t_soft;
    int32_t st;
    ch.flags |= J6_SND_CHORUS;
    st = fx_gate_tail(&t_soft, J6_SP_REV_LEVEL, 1600u);
    printf("    REV_LEVEL 0 -> 127 on a ringing delay, no voice: reverb onset step %d\n", (int)st);
    CHECK(st <= 24);
    st = fx_gate_tail(&t_saw, J6_SP_REV_LEVEL, 1600u);
    printf("    same, bright saw: reverb onset step %d\n", (int)st);
    CHECK(st <= 24);
    st = fx_gate_tail(&ch, J6_SP_DLY_LEVEL, CHUNK_UP(j6y_dly_free[64] + 512u));
    printf("    DLY_LEVEL 0 -> 127 on a ringing chorus, no voice: echo onset step %d\n", (int)st);
    CHECK(st <= 8);
}

/* ---- listening and timing -------------------------------------------------------------------------- */

static void test_demo_wav(void)
{
    static const uint32_t snd[] = {0, 8, 16, 24, 32, 40, 48, 59};
    static const uint8_t chord[4] = {48, 55, 60, 64};
    uint32_t k, n, f = 0;
    const uint32_t seg = CHUNK_UP(SR * 13u / 10u), step = CHUNK_UP(MS(160));   /* 1.3 s per sound */
    j6_synth_init(&sa, fxa, J6_FX_MEM_LEN);
    memset(bufa, 0, sizeof bufa);
    for (k = 0; k < sizeof snd / sizeof snd[0] && f + seg <= MAXF; k++) {
        uint32_t t, tr = k == 4u ? 0u : k == 5u ? 24u : 12u;   /* chord + tr - 12 */
        j6_synth_sound(&sa, snd[k]);
        for (t = 0; t < seg; t += J6_CR, f += J6_CR) {
            if (k == 4u || k == 5u) {                 /* bass and lead: an arpeggio */
                if (t % step == 0u) {
                    n = t / step;
                    if (n) j6_synth_note_off(&sa, chord[(n - 1u) % 4u] + tr - 12u);
                    if (t + step < seg) j6_synth_note_on(&sa, chord[n % 4u] + tr - 12u, 110);
                }
            } else if (t == 0) {
                for (n = 0; n < 4u; n++) j6_synth_note_on(&sa, chord[n] + tr - 12u + (k == 6u ? 12u : 0u), 100);
            } else if (t == seg * 3u / 5u) {
                j6_synth_all_off(&sa);
            }
            j6_synth_render(&sa, bufa + 2u * f, J6_CR);
        }
        j6_synth_all_off(&sa);
    }
    mkdir("build", 0755);
    mkdir("build/j6test", 0755);
    CHECK_EQ(j6t_wav_write("build/j6test/synth_demo.wav", bufa, MAXF), 0);
    CHECK(!all_zero(bufa, MAXF));
    CHECK(j6t_peak(bufa, MAXF) < J6_OUT_MAX);         /* never on the output clamp: chords at vOL 100 fit */
    printf("    wrote build/j6test/synth_demo.wav (%u s, peak %d)\n", (unsigned)(MAXF / SR), (int)j6t_peak(bufa, MAXF));
}

static double block_time_ms(const j6_sound_t *snd, uint32_t blocks)
{
    int32_t blk[2u * J6_BLOCK];
    clock_t t0, t1;
    uint32_t b;
    j6_synth_init(&sa, fxa, J6_FX_MEM_LEN);
    j6y_sound_load(&sa, snd);
    j6_synth_set(&sa, J6_SP_DLY_LEVEL, 80);
    j6_synth_set(&sa, J6_SP_REV_LEVEL, 90);
    j6_synth_note_on(&sa, 48, 100);
    j6_synth_note_on(&sa, 55, 100);
    j6_synth_note_on(&sa, 60, 100);
    j6_synth_note_on(&sa, 64, 100);
    t0 = clock();
    for (b = 0; b < blocks; b++) {
        memset(blk, 0, sizeof blk);
        if (b % 64u == 63u) j6_synth_set(&sa, J6_SP_CUTOFF, (int32_t)(b % 128u));
        j6_synth_render(&sa, blk, J6_BLOCK);
    }
    t1 = clock();
    CHECK_EQ(j6_synth_active(&sa), 4);
    CHECK_EQ(j6_synth_fx_idle(&sa), 0);
    return (double)(t1 - t0) * 1000.0 / (double)CLOCKS_PER_SEC / (double)blocks;
}

static void test_cpu_time(void)
{
    double ms_pad = block_time_ms(&j6_sounds[0], 4000), ms_all = block_time_ms(&t_all, 4000);
    printf("    host time per 256-frame block, 4 voices + chorus + delay + reverb: %.4f ms (Juno Pad), "
           "%.4f ms (all oscillators); budget 2.9 ms on the chip, host target < 0.1 ms\n", ms_pad, ms_all);
    CHECK(ms_all < 2.0);                              /* only catches a pathological slowdown */
}

/* D2: j6_synth_kill fades voices and effect tails to exact silence within 256 frames, without a click */
static void test_kill_fades(void)
{
    static const uint8_t ch[4] = {48, 60, 64, 67};
    uint32_t i, sw = 8192u, end = 16384u;
    j6_synth_init(&sa, fxa, J6_FX_MEM_LEN);
    j6_synth_set(&sa, J6_SP_REV_LEVEL, 127);
    j6_synth_set(&sa, J6_SP_DLY_LEVEL, 127);
    for (i = 0; i < 4; i++) j6_synth_note_on(&sa, ch[i], 127);
    render(&sa, bufa, sw);
    CHECK(j6t_peak(bufa + 2u * (sw - 256u), 256u) > 1000);
    j6_synth_kill(&sa);
    render(&sa, bufa + 2u * sw, end - sw);
    CHECK(no_click("kill", bufa, sw, 256u, end));
    CHECK(all_zero(bufa + 2u * (sw + 256u), end - sw - 256u));
    CHECK_EQ(j6_synth_active(&sa), 0);
    CHECK(j6_synth_fx_idle(&sa));
    j6_synth_kill(&sa);                                    /* already silent: no pending kill left over */
    render(&sa, bufa, 64u);
    j6_synth_note_on(&sa, 60, 100);
    render(&sa, bufa, 2048u);
    CHECK(j6t_peak(bufa + 2u * 1024u, 1024u) > 0);
}

int main(void)
{
    RUN(test_kill_fades);
    RUN(test_tables);
    RUN(test_filter_stable_everywhere);
    RUN(test_filter_decay_and_ring);
    RUN(test_filter_sweep_bounded);
    RUN(test_resonance_boosts);
    RUN(test_idle_is_exact_silence);
    RUN(test_note_sounds);
    RUN(test_voice_level);
    RUN(test_svf_coefs_shift);
    RUN(test_release_exact_zero);
    RUN(test_attack_decay_sustain);
    RUN(test_four_voices_and_steal_oldest);
    RUN(test_steal_edge_cases);
    RUN(test_steal_fade_keeps_old_filter);
    RUN(test_alloc_order);
    RUN(test_same_note_retrigger);
    RUN(test_note_edges);
    RUN(test_velocity_scales);
    RUN(test_worst_case_bounded);
    RUN(test_pitch_and_tune);
    RUN(test_pulse_width);
    RUN(test_detune);
    RUN(test_sound_load_repitches);
    RUN(test_key_tracking);
    RUN(test_preset_decay);
    RUN(test_delay_free_time);
    RUN(test_delay_sync_time);
    RUN(test_delay_feedback_decays);
    RUN(test_delay_time_glides);
    RUN(test_short_fx_len);
    RUN(test_reverb_decays_to_idle);
    RUN(test_stereo);
    RUN(test_panic);
    RUN(test_chunking_identical);
    RUN(test_presets);
    RUN(test_preset_headroom);
    RUN(test_params);
    RUN(test_volume);
    RUN(test_render_adds);
    RUN(test_sound_change_while_playing);
    RUN(test_sound_change_delay_glide);
    RUN(test_chorus_switch_mid_note);
    RUN(test_fx_level_ramps);
    RUN(test_fx_gate_tail);
    RUN(test_demo_wav);
    RUN(test_cpu_time);
    return j6t_report("synth");
}
