/* SPDX-License-Identifier: GPL-3.0-only */
/* test_engine.c - end-to-end tests of the glue (firmware/src/j6/j6_engine.c) with every module linked:
 * FM-1 keys / MIDI in -> chord -> phrase or direct notes -> synth; sequencer -> phrase -> synth; step and
 * real-time recording; per-pattern and global settings; MIDI out; storage.
 *
 * The engine is observed through j6_engine_set_tap() (every event when it takes effect, with the engine
 * tick and the note given to the synth) and j6_engine_midi_out(). Time is driven with
 * j6_engine_advance() / j6_engine_render() in J6_CR chunks; at 120 BPM a tick is 229.6875 frames.
 * Styles 6-9 are checked against data/phrases_6_9.json (run from the repository root). */
#include "j6_test.h"
#include "j6_synth_state.h"

#define FRAMES_PER_TICK_Q4 3675u     /* 229.6875 * 16 at 120 BPM */

static int16_t FX[J6_FX_MEM_LEN];

/* ---- observation ---------------------------------------------------------------------------------- */

#define TL_MAX 65536u
static j6_tap_t TL[TL_MAX];
static uint32_t TLN, TL_LOST;

static void tap(void *ctx, const j6_tap_t *t)
{
    (void)ctx;
    if (TLN < TL_MAX) TL[TLN++] = *t;
    else TL_LOST++;
}

#define MO_MAX 65536u
typedef struct {
    uint8_t st, d1, d2;
    uint32_t tick;
} mo_t;
static mo_t MO[MO_MAX];
static uint32_t MON;

static uint32_t now_tick(void)
{
    j6_status_t st;
    j6_engine_status(&st);
    return st.ticks;
}

static void drain(void)
{
    uint8_t a, b, c;
    uint32_t t = now_tick();
    while (j6_engine_midi_out(&a, &b, &c))
        if (MON < MO_MAX) {
            MO[MON].st = a;
            MO[MON].d1 = b;
            MO[MON].d2 = c;
            MO[MON].tick = t;
            MON++;
        }
}

/* advance in J6_CR chunks, draining MIDI out after each */
static void run_frames(uint32_t frames)
{
    uint32_t i;
    for (i = 0; i < frames; i += J6_CR) {
        j6_engine_advance(J6_CR);
        drain();
    }
}
static void apply(void) { run_frames(J6_CR); }
static void run_ticks(uint32_t n)
{
    uint32_t target = now_tick() + n;
    while ((int32_t)(now_tick() - target) < 0) run_frames(J6_CR);
}

static void eng(void)
{
    j6_engine_set_tap(tap, NULL);
    j6_engine_init(FX, J6_FX_MEM_LEN, NULL);
    TLN = 0;
    TL_LOST = 0;
    drain();
    MON = 0;
}

static void key_tap(int32_t key)
{
    j6_engine_key(key, 1);
    apply();
    j6_engine_key(key, 0);
    apply();
}

/* note-ons of a source (src >= J6_SRC_COUNT: any) from log index `from` */
typedef struct {
    uint32_t on, off;                /* tick * 16 + sub */
    uint32_t on_frame, off_frame;
    uint8_t note, vel, src, snote;
} nt_t;
#define NT_MAX 4096u
static nt_t NT[NT_MAX];

static uint32_t notes_of(uint32_t src, uint32_t from)
{
    uint32_t i, j, k = 0;
    for (i = from; i < TLN && k < NT_MAX; i++) {
        const j6_tap_t *t = &TL[i];
        if (t->ev.type != J6_EV_NOTE_ON || (src < J6_SRC_COUNT && t->ev.src != src)) continue;
        NT[k].on = t->tick * J6_SUB + t->ev.sub;
        NT[k].on_frame = t->frame;
        NT[k].note = t->ev.note;
        NT[k].vel = t->ev.vel;
        NT[k].src = t->ev.src;
        NT[k].snote = t->synth_note;
        NT[k].off = 0xFFFFFFFFu;
        NT[k].off_frame = 0xFFFFFFFFu;
        for (j = i + 1; j < TLN; j++)
            if (TL[j].ev.type == J6_EV_NOTE_OFF && TL[j].ev.src == t->ev.src && TL[j].ev.note == t->ev.note) {
                NT[k].off = TL[j].tick * J6_SUB + TL[j].ev.sub;
                NT[k].off_frame = TL[j].frame;
                break;
            }
        k++;
    }
    return k;
}

static uint32_t count_ev(uint32_t type, uint32_t from)
{
    uint32_t i, k = 0;
    for (i = from; i < TLN; i++)
        if (TL[i].ev.type == type) k++;
    return k;
}

static int find_ev(uint32_t type, uint32_t from)
{
    uint32_t i;
    for (i = from; i < TLN; i++)
        if (TL[i].ev.type == type) return (int)i;
    return -1;
}

/* every NOTE_ON in the log has its NOTE_OFF (per source and note) */
static int tap_balanced(void)
{
    static int depth[J6_SRC_COUNT][128];
    uint32_t i, s, n;
    memset(depth, 0, sizeof depth);
    for (i = 0; i < TLN; i++) {
        const j6_event_t *e = &TL[i].ev;
        if (e->src >= J6_SRC_COUNT || e->note > 127u) continue;
        if (e->type == J6_EV_NOTE_ON) depth[e->src][e->note]++;
        if (e->type == J6_EV_NOTE_OFF && depth[e->src][e->note] > 0) depth[e->src][e->note]--;
    }
    for (s = 0; s < J6_SRC_COUNT; s++)
        for (n = 0; n < 128u; n++)
            if (depth[s][n]) return 0;
    return 1;
}

/* the notes of a source sounding now (by the log), ascending */
static uint32_t sounding(uint32_t src, uint8_t *out)
{
    int depth[128];
    uint32_t i, k = 0;
    memset(depth, 0, sizeof depth);
    for (i = 0; i < TLN; i++) {
        const j6_event_t *e = &TL[i].ev;
        if (e->src != src || e->note > 127u) continue;
        if (e->type == J6_EV_NOTE_ON) depth[e->note]++;
        if (e->type == J6_EV_NOTE_OFF && depth[e->note] > 0) depth[e->note]--;
    }
    for (i = 0; i < 128u; i++)
        if (depth[i]) out[k++] = (uint8_t)i;
    return k;
}

/* MIDI-out messages with this status (d1 < 128: and this first data byte) */
static uint32_t mo_count(uint32_t st, uint32_t d1, uint32_t from)
{
    uint32_t i, k = 0;
    for (i = from; i < MON; i++)
        if (MO[i].st == st && (d1 > 127u || MO[i].d1 == d1)) k++;
    return k;
}
static int mo_find(uint32_t st, uint32_t from)
{
    uint32_t i;
    for (i = from; i < MON; i++)
        if (MO[i].st == st) return (int)i;
    return -1;
}

/* ---- data/phrases_6_9.json: one list (host only, a targeted reader) ------------------------------- */

typedef struct {
    int t, len, tone, oct, acc;
} jn_t;

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    long sz;
    char *buf;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)sz + 1u);
    if (buf && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) buf[sz] = 0;
    return buf;
}

static int jfield(const char *obj, const char *end, const char *name, int *out)
{
    char key[16];
    const char *p;
    snprintf(key, sizeof key, "\"%s\"", name);
    p = strstr(obj, key);
    if (!p || p > end) return 0;
    p = strchr(p + strlen(key), ':');
    if (!p) return 0;
    *out = (int)strtol(p + 1, NULL, 10);
    return 1;
}

/* list s{style}v{var} for n held notes: loop and notes; returns the note count, -1 on error */
static int json_list(const char *js, uint32_t style, uint32_t var, uint32_t n, int *loop, jn_t *out, int max)
{
    char key[16];
    const char *p, *q, *end;
    int k = 0;
    snprintf(key, sizeof key, "\"s%uv%02u\"", (unsigned)style, (unsigned)var);
    p = strstr(js, key);
    if (!p) return -1;
    snprintf(key, sizeof key, "\"%u\"", (unsigned)n);
    p = strstr(p, key);
    if (!p) return -1;
    q = strstr(p, "\"notes\"");
    if (!q || !jfield(p, q, "loop", loop)) return -1;
    p = strchr(q, '[');
    if (!p) return -1;
    p++;
    for (;;) {
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ',') p++;
        if (*p != '{') break;
        end = strchr(p, '}');
        if (!end || k >= max) return -1;
        if (!jfield(p, end, "t", &out[k].t) || !jfield(p, end, "len", &out[k].len) ||
            !jfield(p, end, "tone", &out[k].tone) || !jfield(p, end, "oct", &out[k].oct) ||
            !jfield(p, end, "acc", &out[k].acc))
            return -1;
        k++;
        p = end + 1;
    }
    return *p == ']' ? k : -1;
}

static int jn_cmp(const void *a, const void *b)
{
    const jn_t *x = (const jn_t *)a, *y = (const jn_t *)b;
    if (x->t != y->t) return x->t < y->t ? -1 : 1;
    return x->tone - y->tone;
}
static int nt_cmp(const void *a, const void *b)
{
    const nt_t *x = (const nt_t *)a, *y = (const nt_t *)b;
    if (x->on != y->on) return x->on < y->on ? -1 : 1;
    return (int)x->note - (int)y->note;
}

/* ---- tests ---------------------------------------------------------------------------------------- */

/* chord set 1 + key C, CHORD on, STYLE off: a block chord C3 G3 D4 E4 on the synth, not on MIDI out */
static void test_chord_key_block_chord(void)
{
    static int32_t out[J6_BLOCK * 2];
    uint32_t k, i, from;
    j6_status_t st;
    eng();
    j6_engine_set(J6_P_CHORD, 1);
    apply();
    CHECK_EQ(j6_engine_get(J6_P_CHORD), 1);
    CHECK_EQ(j6_engine_get(J6_P_CHORD_SET), 0);
    from = TLN;
    j6_engine_key(0, 1);
    memset(out, 0, sizeof out);
    j6_engine_render(out, J6_BLOCK);
    drain();
    k = notes_of(J6_SRC_CHORD, from);
    CHECK_EQ(k, 4);
    if (k == 4) {
        static const uint8_t want[4] = {48, 55, 62, 64};   /* C3 G3 D4 E4 */
        for (i = 0; i < 4; i++) {
            CHECK_EQ(NT[i].note, want[i]);
            CHECK_EQ(NT[i].snote, want[i]);
            CHECK_EQ(NT[i].vel, J6_VELO_DEF);
        }
    }
    j6_engine_status(&st);
    CHECK_EQ(st.voices, 4);
    CHECK(j6t_peak(out, J6_BLOCK) > 0);
    CHECK_EQ(st.last_chord, j6_chord_tag(0, 0));
    CHECK_EQ(st.keys_lit, 1u << (0 - J6_KEY_MIN));
    CHECK_EQ(mo_count(0x90, 255, 0), 0);                   /* MI-2 / Q-23: no chord notes out */
    j6_engine_key(0, 0);
    apply();
    CHECK_EQ(count_ev(J6_EV_NOTE_OFF, from), 4);
    CHECK(tap_balanced());

    /* key 12 (FM-1 C5) plays the C chord one octave up (CH-5); key -7 (F3) the F chord one octave down */
    from = TLN;
    key_tap(12);
    k = notes_of(J6_SRC_CHORD, from);
    CHECK_EQ(k, 4);
    if (k == 4) CHECK_EQ(NT[0].note, 60);
    from = TLN;
    key_tap(-7);
    k = notes_of(J6_SRC_CHORD, from);
    {
        uint8_t raw[4];
        uint32_t nr = j6_chord_raw(0, 5, raw);
        CHECK_EQ(k, nr);
        if (k == nr && k) CHECK_EQ(NT[0].note, raw[0] - 12);
    }
    /* set 43 key F: 3 notes 53 58 62 (CH-4) */
    j6_engine_set(J6_P_CHORD_SET, 42);
    apply();
    from = TLN;
    key_tap(5);
    k = notes_of(J6_SRC_CHORD, from);
    CHECK_EQ(k, 3);
    if (k == 3) {
        CHECK_EQ(NT[0].note, 53);
        CHECK_EQ(NT[1].note, 58);
        CHECK_EQ(NT[2].note, 62);
    }
    /* mono (CH-6): a second chord key replaces the first; releasing it brings the first back */
    j6_engine_set(J6_P_CHORD_SET, 0);
    apply();
    {
        uint8_t raw[4], snd[16];
        uint32_t nr, ns;
        j6_engine_key(0, 1);
        apply();
        j6_engine_key(7, 1);
        apply();
        nr = j6_chord_raw(0, 7, raw);
        ns = sounding(J6_SRC_CHORD, snd);                  /* exactly the G chord (shared notes go on) */
        CHECK_EQ(ns, nr);
        for (i = 0; i < ns && i < nr; i++) CHECK_EQ(snd[i], raw[i]);
        j6_engine_status(&st);
        CHECK_EQ(st.voices, nr);
        j6_engine_key(7, 0);
        apply();
        ns = sounding(J6_SRC_CHORD, snd);                  /* the C chord again */
        CHECK_EQ(ns, 4);
        if (ns == 4) {
            CHECK_EQ(snd[0], 48);
            CHECK_EQ(snd[3], 64);
        }
    }
    j6_engine_key(0, 0);
    apply();
    CHECK(tap_balanced());
    CHECK_EQ(TL_LOST, 0);
}

/* CHORD off, STYLE on, style 1 var 1 (UP1 in 8ths), C-E-G held: 60 64 67 every 48 ticks at the tempo */
static void test_live_arp_style1(void)
{
    uint32_t k, i, from, t0, mo0;
    eng();
    j6_engine_set(J6_P_STYLE_ON, 1);
    apply();
    CHECK_EQ(j6_engine_get(J6_P_STYLE_ON), 1);
    CHECK_EQ(j6_engine_get(J6_P_STYLE), 1);
    CHECK_EQ(j6_engine_get(J6_P_VAR), 1);
    run_ticks(37);                                         /* off the grid of the free-running ticks */
    from = TLN;
    mo0 = MON;
    t0 = now_tick() + 1u;                                  /* PH-5: pt 0 is the first tick after the press */
    j6_engine_key(0, 1);
    j6_engine_key(4, 1);
    j6_engine_key(7, 1);
    run_ticks(2u * J6_TICKS_BAR);
    k = notes_of(J6_SRC_PHRASE_LIVE, from);
    CHECK_EQ(k, 16);                                       /* 2 bars of 8ths */
    CHECK_EQ(notes_of(J6_SRC_KEY, from), 0);               /* nothing direct with STYLE on */
    k = notes_of(J6_SRC_PHRASE_LIVE, from);
    for (i = 0; i < k; i++) {
        static const uint8_t up[3] = {60, 64, 67};
        CHECK_EQ(NT[i].note, up[i % 3u]);
        CHECK_EQ(NT[i].on, (t0 + 48u * i) * J6_SUB);
        CHECK_EQ(NT[i].off - NT[i].on, j6_gate(48) * J6_SUB);
        CHECK_EQ(NT[i].vel, J6_VELO_DEF);
        if (i) {                                           /* 48 ticks = 11025 frames at 120 BPM */
            uint32_t df = NT[i].on_frame - NT[0].on_frame, want = (48u * i * FRAMES_PER_TICK_Q4) >> 4;
            CHECK_RANGE(df, want - J6_CR, want + J6_CR);
        }
    }
    CHECK_EQ(NT[1].on_frame - NT[0].on_frame >= 11025u - J6_CR, 1);
    /* MIDI out: the phrase notes on CH 1 at vELo, clock every 4 ticks */
    CHECK_EQ(mo_count(0x90, 255, mo0), 16);
    CHECK_EQ(mo_count(0x90, 60, mo0), 6);
    CHECK_RANGE(mo_count(0xF8, 255, mo0), 2u * J6_TICKS_BAR / 4u - 1u, 2u * J6_TICKS_BAR / 4u + 1u);
    j6_engine_key(0, 0);
    j6_engine_key(4, 0);
    j6_engine_key(7, 0);
    apply();
    CHECK(tap_balanced());
    CHECK_EQ(mo_count(0x90, 255, mo0), mo_count(0x80, 255, mo0));

    /* 140 BPM: 48 ticks = 48 * 1128960000 / 1400 / 4096 frames */
    eng();
    j6_engine_set(J6_P_STYLE_ON, 1);
    j6_engine_set(J6_P_TEMPO10, 1400);
    apply();
    from = TLN;
    j6_engine_key(0, 1);
    run_ticks(200);
    k = notes_of(J6_SRC_PHRASE_LIVE, from);
    CHECK(k >= 4);
    if (k >= 4) {
        uint32_t want = (uint32_t)((3ull * 48ull * j6_spt_q12(1400)) >> 12);
        CHECK_RANGE(NT[3].on_frame - NT[0].on_frame, want - J6_CR, want + J6_CR);
    }
    j6_engine_key(0, 0);
    apply();
    CHECK(tap_balanced());
}

/* the captured style 6 var 2 (and 9 var 6) with chord set 1 key C (a 4-note chord): JSON timing */
static void check_captured(const char *js, uint32_t style, uint32_t var)
{
    static jn_t exp[64];
    int loop = 0, n = json_list(js, style, var, 4, &loop, exp, 64), i;
    uint32_t k, from, t0, L;
    static const uint8_t H[4] = {48, 55, 62, 64};
    CHECK(n > 0);
    if (n <= 0) return;
    qsort(exp, (size_t)n, sizeof exp[0], jn_cmp);
    L = (uint32_t)loop;
    eng();
    j6_engine_set(J6_P_CHORD, 1);
    j6_engine_set(J6_P_STYLE_ON, 1);
    j6_engine_set(J6_P_STYLE, (int32_t)style);
    j6_engine_set(J6_P_VAR, (int32_t)var);
    j6_engine_set(J6_P_AVEL, 120);                         /* told apart from vELo 100 */
    apply();
    CHECK_EQ(j6_engine_get(J6_P_STYLE), (int32_t)style);
    CHECK_EQ(j6_engine_get(J6_P_VAR), (int32_t)var);
    run_ticks(29);
    from = TLN;
    t0 = now_tick() + 1u;
    j6_engine_key(0, 1);
    run_ticks(3u * L);                                     /* two loops checked, the third ends their gates */
    j6_engine_key(0, 0);
    apply();
    k = notes_of(J6_SRC_PHRASE_LIVE, from);
    qsort(NT, k, sizeof NT[0], nt_cmp);
    while (k && NT[k - 1u].on >= (t0 + 2u * L) * J6_SUB) k--;
    CHECK_EQ(k, 2u * (uint32_t)n);
    for (i = 0; i < n && (uint32_t)i < k; i++) {
        uint32_t r;
        for (r = 0; r < 2u; r++) {
            const nt_t *x = &NT[(uint32_t)i + r * (uint32_t)n];
            if ((uint32_t)i + r * (uint32_t)n >= k) break;
            CHECK_EQ(x->on, (t0 + r * L + (uint32_t)exp[i].t) * J6_SUB);
            CHECK_EQ(x->off - x->on, (uint32_t)exp[i].len * J6_SUB);
            CHECK_EQ(x->note, H[exp[i].tone] + 12 * exp[i].oct);
            CHECK_EQ(x->vel, exp[i].acc ? 120 : J6_VELO_DEF);
        }
    }
    CHECK(tap_balanced());
}

static void test_captured_phrases(void)
{
    char *js = slurp("data/phrases_6_9.json");
    CHECK(js != NULL);
    if (!js) return;
    check_captured(js, 6, 2);
    check_captured(js, 9, 6);
    check_captured(js, 7, 5);
    free(js);
}

/* step edit: enter notes with the keys, ties with HOLD */
static void enter_step(uint32_t step, const int32_t *keys, uint32_t n)
{
    uint32_t i;
    j6_engine_cmd(J6_CMD_STEP_SELECT, (int32_t)step);
    apply();
    for (i = 0; i < n; i++) key_tap(keys[i]);
}

/* a 4-step pattern with a tie and LAST = 3 (bEAt 1 = 96 ticks): C tied over 2 steps, E, (G never) */
static void test_pattern_tie_last(void)
{
    static const int32_t kc[1] = {0}, ke[1] = {4}, kg[1] = {7};
    const j6_pattern_t *p;
    uint32_t k, from, t0, i, mo0;
    int ix;
    j6_status_t st;
    eng();
    j6_engine_set(J6_P_BEAT, J6_BEAT_1);
    apply();
    enter_step(0, kc, 1);
    j6_engine_cmd(J6_CMD_HOLD, 0);                         /* SQ-17: tie into step 1 */
    apply();
    j6_engine_status(&st);
    CHECK_EQ(st.sel_step, 0);
    enter_step(2, ke, 1);
    enter_step(3, kg, 1);
    j6_engine_cmd(J6_CMD_STEP_SELECT, -1);
    j6_engine_set(J6_P_LAST, 3);
    apply();
    p = j6_engine_pattern(0);
    CHECK_EQ(p->step[0].note[0], 60);
    CHECK_EQ(p->step[0].note[1], J6_NO_NOTE);
    CHECK_EQ(p->step[1].flags & J6_STF_TIE, J6_STF_TIE);
    CHECK_EQ(p->step[2].note[0], 64);
    CHECK_EQ(p->step[3].note[0], 67);
    CHECK_EQ(p->last, 3);
    j6_engine_status(&st);
    CHECK_EQ(st.dirty[0] & 1u, 1u);
    /* the step-edit preview sounded each key while held, as direct notes */
    CHECK_EQ(notes_of(J6_SRC_KEY, 0), 3);

    from = TLN;
    mo0 = MON;
    j6_engine_cmd(J6_CMD_PLAY, 0);
    run_ticks(2u * 288u + 10u);
    ix = find_ev(J6_EV_START, from);
    CHECK(ix >= 0);
    if (ix < 0) return;
    t0 = TL[ix].tick;
    k = notes_of(J6_SRC_SEQ, from);
    CHECK_EQ(k, 5);                                        /* C E | C E | C */
    for (i = 0; i < k; i++) {
        uint32_t loop = i / 2u, start = t0 + 288u * loop + (i % 2u ? 192u : 0u);
        CHECK_EQ(NT[i].note, i % 2u ? 64 : 60);
        CHECK_EQ(NT[i].on, start * J6_SUB);
        if (NT[i].off != 0xFFFFFFFFu) CHECK_EQ(NT[i].off - NT[i].on, (i % 2u ? j6_gate(96) : j6_gate(192)) * J6_SUB);
    }
    for (i = from; i < TLN; i++) {
        if (TL[i].ev.type == J6_EV_NOTE_ON) CHECK(TL[i].ev.note != 67);   /* step 4 is after LAST */
        if (TL[i].ev.type == J6_EV_STEP) CHECK(TL[i].ev.arg < 3);
    }
    CHECK_EQ(count_ev(J6_EV_LOOP, from), 2);
    /* MIDI: Start, then clocks; the sequencer notes on CH 1 */
    {
        int s = mo_find(0xFA, mo0);
        CHECK(s >= 0);
        CHECK_EQ(mo_count(0x90, 60, mo0), 3);
        CHECK_EQ(mo_count(0x90, 64, mo0), 2);
    }
    j6_engine_cmd(J6_CMD_PLAY, 0);                         /* PLAY while playing = stop */
    apply();
    j6_engine_status(&st);
    CHECK_EQ(st.seq_state, J6_SEQ_STOPPED);
    CHECK(mo_find(0xFC, mo0) >= 0);
    CHECK(tap_balanced());
}

/* SQ-26: a STYLE OFF step inherits the phrase of the step before it */
static void test_style_off_step_inherits(void)
{
    static const int32_t ceg[3] = {0, 4, 7}, dfa[3] = {2, 5, 9};
    const j6_pattern_t *p;
    uint32_t k, from, t0, i;
    int ix;
    eng();
    j6_engine_set(J6_P_BEAT, J6_BEAT_1);
    j6_engine_set(J6_P_LAST, 2);
    apply();
    enter_step(0, ceg, 3);
    j6_engine_cmd(J6_CMD_STYLE, 0);                        /* step flag on: knob positions style 1 var 1 */
    apply();
    CHECK_EQ(j6_engine_get(J6_P_STYLE_ON), 1);
    enter_step(1, dfa, 3);
    CHECK_EQ(j6_engine_get(J6_P_STYLE_ON), 0);
    j6_engine_cmd(J6_CMD_STEP_SELECT, -1);
    apply();
    p = j6_engine_pattern(0);
    CHECK_EQ(p->step[0].phrase, j6_phr_pack(1, 1, 1));
    CHECK_EQ(j6_phr_on(p->step[1].phrase), 0);
    CHECK_EQ(j6_phr_on(p->phrase), 0);                     /* the pattern phrase is OFF */
    CHECK_EQ(j6_engine_get(J6_P_STYLE_ON), 0);

    from = TLN;
    j6_engine_cmd(J6_CMD_PLAY, 0);
    run_ticks(2u * 192u + 4u);
    j6_engine_cmd(J6_CMD_STOP, 0);
    apply();
    ix = find_ev(J6_EV_START, from);
    CHECK(ix >= 0);
    if (ix < 0) return;
    t0 = TL[ix].tick;
    CHECK_EQ(notes_of(J6_SRC_SEQ, from), 0);               /* nothing plain: step 2 is an arpeggio too */
    k = notes_of(J6_SRC_PHRASE_SEQ, from);
    CHECK_EQ(k, 9);
    for (i = 0; i < k && i < 8u; i++) {
        static const uint8_t want[4] = {60, 64, 62, 65};   /* CEG: C E, then DFA from its start: D F */
        CHECK_EQ(NT[i].note, want[i % 4u]);
        CHECK_EQ(NT[i].on, (t0 + 48u * i) * J6_SUB);
        CHECK_EQ(NT[i].off - NT[i].on, j6_gate(48) * J6_SUB);
    }
    CHECK_EQ(count_ev(J6_EV_HELD, from) >= 4u, 1);
    CHECK(tap_balanced());

    /* and with the step flag off too, everything plays plainly (the pattern phrase is OFF) */
    eng();
    j6_engine_set(J6_P_BEAT, J6_BEAT_1);
    j6_engine_set(J6_P_LAST, 2);
    apply();
    enter_step(0, ceg, 3);
    enter_step(1, dfa, 3);
    j6_engine_cmd(J6_CMD_STEP_SELECT, -1);
    apply();
    from = TLN;
    j6_engine_cmd(J6_CMD_PLAY, 0);
    run_ticks(200);
    j6_engine_cmd(J6_CMD_STOP, 0);
    apply();
    CHECK_EQ(notes_of(J6_SRC_PHRASE_SEQ, from), 0);
    CHECK_EQ(notes_of(J6_SRC_SEQ, from), 9);               /* CEG, DFA, CEG */
    CHECK(tap_balanced());
}

/* SQ-27: P.chg StEP switches at the next step boundary. Queued here during the last step, where that
 * boundary is also the end of the pattern (the mid-pattern case: test_pattern_change_mid_pattern) */
static void test_pattern_change_last_step(void)
{
    static const int32_t kc[1] = {0}, ke[1] = {4}, kg[1] = {7};
    uint32_t from, t0, k, mo0;
    int ix, ip;
    j6_status_t st;
    eng();
    j6_engine_cmd(J6_CMD_PATTERN, 1);                      /* stopped: immediate */
    j6_engine_set(J6_P_BEAT, J6_BEAT_1);
    apply();
    j6_engine_status(&st);
    CHECK_EQ(st.pattern, 1);
    enter_step(0, kg, 1);
    j6_engine_cmd(J6_CMD_STEP_SELECT, -1);
    j6_engine_cmd(J6_CMD_PATTERN, 0);
    j6_engine_set(J6_P_BEAT, J6_BEAT_1);
    j6_engine_set(J6_P_LAST, 2);
    apply();
    CHECK_EQ(j6_engine_get(J6_P_PCHG), J6_PCHG_STEP);
    enter_step(0, kc, 1);
    enter_step(1, ke, 1);
    j6_engine_cmd(J6_CMD_STEP_SELECT, -1);
    apply();
    from = TLN;
    mo0 = MON;
    j6_engine_cmd(J6_CMD_PLAY, 0);
    run_ticks(120);                                        /* inside step 2 (the last) */
    j6_engine_cmd(J6_CMD_PATTERN, 1);
    apply();
    j6_engine_status(&st);
    CHECK_EQ(st.pattern, 0);
    CHECK_EQ(st.queued, 1);
    run_ticks(200);
    ix = find_ev(J6_EV_START, from);
    ip = find_ev(J6_EV_PATTERN, from);
    CHECK(ix >= 0 && ip >= 0);
    if (ix < 0 || ip < 0) return;
    t0 = TL[ix].tick;
    CHECK_EQ(TL[ip].tick, t0 + 192u);                      /* the next step boundary (= the end of 1-1) */
    CHECK_EQ(TL[ip].ev.arg, 1);
    k = notes_of(J6_SRC_SEQ, from);
    CHECK_EQ(k, 3);                                        /* C, E, then 1-2's G */
    if (k == 3) {
        CHECK_EQ(NT[0].note, 60);
        CHECK_EQ(NT[1].note, 64);
        CHECK_EQ(NT[1].on, (t0 + 96u) * J6_SUB);
        CHECK_EQ(NT[2].note, 67);
        CHECK_EQ(NT[2].on, (t0 + 192u) * J6_SUB);
    }
    j6_engine_status(&st);
    CHECK_EQ(st.pattern, 1);
    CHECK_EQ(st.queued, J6_NONE);
    CHECK_EQ(mo_count(0xCF, 1, mo0), 1);                   /* txPc on: PC 1 on Pc.Ch 16 (MI-6) */
    j6_engine_cmd(J6_CMD_STOP, 0);
    apply();
    CHECK(tap_balanced());

    /* P.chg 1: the switch waits for the next beat, not for the end of the 4-beat step */
    eng();
    j6_engine_set(J6_P_PCHG, 1);
    apply();
    enter_step(0, kc, 1);
    j6_engine_cmd(J6_CMD_STEP_SELECT, -1);
    apply();
    from = TLN;
    j6_engine_cmd(J6_CMD_PLAY, 0);
    run_ticks(40);
    j6_engine_cmd(J6_CMD_PATTERN, 3);
    run_ticks(100);
    ix = find_ev(J6_EV_START, from);
    ip = find_ev(J6_EV_PATTERN, from);
    CHECK(ix >= 0 && ip >= 0);
    if (ix >= 0 && ip >= 0) CHECK_EQ(TL[ip].tick, TL[ix].tick + 96u);
    j6_engine_cmd(J6_CMD_STOP, 0);
    apply();
    CHECK(tap_balanced());
}

/* MI-6: a Program Change 0..63 on Pc.Ch with rxPc selects the pattern */
static void test_program_change_in(void)
{
    j6_status_t st;
    uint32_t mo0;
    eng();
    mo0 = MON;
    j6_engine_midi_in(J6_PORT_USB, 0xCF, 5, 0);            /* Pc.Ch 16 (default) */
    apply();
    j6_engine_status(&st);
    CHECK_EQ(st.pattern, 5);
    CHECK_EQ(mo_count(0xCF, 5, mo0), 1);                   /* the change is echoed by txPc */
    j6_engine_midi_in(J6_PORT_USB, 0xCF, 63, 0);
    apply();
    j6_engine_status(&st);
    CHECK_EQ(st.pattern, 63);
    j6_engine_midi_in(J6_PORT_USB, 0xCF, 64, 0);           /* 64..127: not a pattern */
    apply();
    j6_engine_status(&st);
    CHECK_EQ(st.pattern, 63);
    j6_engine_midi_in(J6_PORT_USB, 0xC0, 7, 0);            /* the synth channel: nothing */
    apply();
    j6_engine_status(&st);
    CHECK_EQ(st.pattern, 63);
    j6_engine_midi_in(J6_PORT_TRS, 0xCF, 0, 0);
    apply();
    j6_engine_status(&st);
    CHECK_EQ(st.pattern, 0);
    j6_engine_set(J6_P_PCCH, 3);                           /* another Pc.Ch */
    apply();
    j6_engine_midi_in(J6_PORT_USB, 0xCF, 9, 0);
    j6_engine_midi_in(J6_PORT_USB, 0xC2, 10, 0);
    apply();
    j6_engine_status(&st);
    CHECK_EQ(st.pattern, 10);
    j6_engine_set(J6_P_RXPC, 0);                           /* rxPc off */
    apply();
    j6_engine_midi_in(J6_PORT_USB, 0xC2, 11, 0);
    apply();
    j6_engine_status(&st);
    CHECK_EQ(st.pattern, 10);
    /* during playback a PC is queued and follows P.chg (SQ-27) */
    j6_engine_set(J6_P_RXPC, 1);
    j6_engine_cmd(J6_CMD_PLAY, 0);
    run_ticks(10);
    j6_engine_midi_in(J6_PORT_USB, 0xC2, 20, 0);
    apply();
    j6_engine_status(&st);
    CHECK_EQ(st.pattern, 10);
    CHECK_EQ(st.queued, 20);
    run_ticks(J6_TICKS_BAR);                               /* bEAt 4: the next step is a bar later */
    j6_engine_status(&st);
    CHECK_EQ(st.pattern, 20);
    j6_engine_cmd(J6_CMD_STOP, 0);
    apply();
}

/* MIDI in notes on CH: direct notes at their velocity, never echoed; other channels ignored; CC64 */
static void test_midi_in_notes(void)
{
    uint32_t from, k, mo0;
    j6_status_t st;
    eng();
    from = TLN;
    mo0 = MON;
    j6_engine_midi_in(J6_PORT_USB, 0x90, 62, 77);
    j6_engine_midi_in(J6_PORT_USB, 0x91, 63, 77);          /* channel 2: ignored */
    apply();
    k = notes_of(J6_SRC_MIDI, from);
    CHECK_EQ(k, 1);
    if (k) {
        CHECK_EQ(NT[0].note, 62);
        CHECK_EQ(NT[0].vel, 77);
    }
    CHECK_EQ(mo_count(0x90, 255, mo0), 0);                 /* no echo */
    j6_engine_midi_in(J6_PORT_USB, 0x90, 62, 0);           /* note on, velocity 0 = off */
    apply();
    CHECK_EQ(count_ev(J6_EV_NOTE_OFF, from), 1);
    /* CC64 latches (HD-3): released notes keep sounding until CC64 < 64 */
    j6_engine_midi_in(J6_PORT_USB, 0xB0, 64, 127);
    j6_engine_midi_in(J6_PORT_USB, 0x90, 65, 90);
    j6_engine_midi_in(J6_PORT_USB, 0x80, 65, 0);
    apply();
    j6_engine_status(&st);
    CHECK_EQ(st.hold, 1);
    CHECK_EQ(notes_of(J6_SRC_MIDI, from), 2);
    CHECK_EQ(count_ev(J6_EV_NOTE_OFF, from), 1);
    j6_engine_midi_in(J6_PORT_USB, 0xB0, 64, 0);
    apply();
    CHECK_EQ(count_ev(J6_EV_NOTE_OFF, from), 2);
    /* MIDI notes feed the live phrase with STYLE on (MI-3), at vELo (PH-9) */
    j6_engine_set(J6_P_STYLE_ON, 1);
    apply();
    from = TLN;
    j6_engine_midi_in(J6_PORT_USB, 0x90, 60, 30);
    run_ticks(100);
    k = notes_of(J6_SRC_PHRASE_LIVE, from);
    CHECK_EQ(k, 3);
    if (k) CHECK_EQ(NT[0].vel, J6_VELO_DEF);
    j6_engine_midi_in(J6_PORT_USB, 0xB0, 123, 0);          /* All Notes Off */
    apply();
    run_ticks(100);
    CHECK_EQ(notes_of(J6_SRC_PHRASE_LIVE, from), 3);
    CHECK(tap_balanced());
}

/* KB-5: trAn moves what the synth plays, not MIDI out; a change while a note sounds keeps it matched */
static void test_tran_and_key(void)
{
    uint32_t from, k, mo0;
    eng();
    j6_engine_set(J6_P_TRAN, 2);
    apply();
    from = TLN;
    mo0 = MON;
    j6_engine_key(0, 1);
    apply();
    k = notes_of(J6_SRC_KEY, from);
    CHECK_EQ(k, 1);
    if (k) {
        CHECK_EQ(NT[0].note, 60);
        CHECK_EQ(NT[0].snote, 62);
    }
    CHECK_EQ(mo_count(0x90, 60, mo0), 1);                  /* MIDI out not transposed (MI-2) */
    j6_engine_set(J6_P_TRAN, -5);
    apply();
    j6_engine_key(0, 0);
    apply();
    {
        int i = find_ev(J6_EV_NOTE_OFF, from);
        CHECK(i >= 0);
        if (i >= 0) CHECK_EQ(TL[i].synth_note, 62);
    }
    CHECK_EQ(mo_count(0x80, 60, mo0), 1);
    /* KEY and octave move the keys (KB-3, KB-4); a release matches the note of the press */
    j6_engine_set(J6_P_TRAN, 0);
    j6_engine_set(J6_P_KEY, 3);
    j6_engine_set(J6_P_OCT, -1);
    apply();
    from = TLN;
    j6_engine_key(4, 1);
    apply();
    j6_engine_set(J6_P_OCT, 1);
    apply();
    j6_engine_key(4, 0);
    apply();
    k = notes_of(J6_SRC_KEY, from);
    CHECK_EQ(k, 1);
    if (k) {
        CHECK_EQ(NT[0].note, 60 + 4 + 3 - 12);
        CHECK(NT[0].off != 0xFFFFFFFFu);
    }
    CHECK(tap_balanced());
}

/* real-time recording (SQ-30 .. SQ-33): REC, PLAY, a key during step 3 -> step 3 holds it */
/* D7: a step selected for editing does not capture the keys once REC is armed */
static void test_record_over_step_edit(void)
{
    const j6_pattern_t *p;
    j6_status_t st;
    eng();
    j6_engine_set(J6_P_BEAT, J6_BEAT_1);
    j6_engine_cmd(J6_CMD_STEP_SELECT, 4);
    apply();
    j6_engine_status(&st);
    CHECK_EQ(st.sel_step, 4);
    j6_engine_cmd(J6_CMD_REC, 0);
    apply();
    j6_engine_status(&st);
    CHECK_EQ(st.sel_step, J6_NONE);
    j6_engine_cmd(J6_CMD_PLAY, 0);
    run_ticks(2u * 96u + 30u);                             /* inside step 3 */
    j6_engine_cmd(J6_CMD_STEP_SELECT, 4);                  /* even a step picked while recording */
    apply();
    j6_engine_key(7, 1);
    apply();
    j6_engine_key(7, 0);
    apply();
    p = j6_engine_pattern(0);
    CHECK_EQ(p->step[2].note[0], 67);
    CHECK_EQ(j6_step_count(&p->step[4]), 0);
}

static void test_realtime_record(void)
{
    const j6_pattern_t *p;
    j6_status_t st;
    uint32_t from;
    eng();
    j6_engine_set(J6_P_BEAT, J6_BEAT_1);
    apply();
    j6_engine_cmd(J6_CMD_REC, 0);
    apply();
    j6_engine_status(&st);
    CHECK_EQ(st.seq_state, J6_SEQ_STANDBY);
    from = TLN;
    j6_engine_cmd(J6_CMD_PLAY, 0);
    run_ticks(2u * 96u + 30u);                             /* inside step 3 */
    j6_engine_status(&st);
    CHECK_EQ(st.recording, 1);
    CHECK_EQ(st.step_now, 2);
    j6_engine_key(7, 1);
    apply();
    j6_engine_key(7, 0);
    apply();
    p = j6_engine_pattern(0);
    CHECK_EQ(p->step[2].note[0], 67);
    j6_engine_status(&st);
    CHECK_EQ(st.dirty[0] & 1u, 1u);
    /* SQ-32 floor, not nearest: a press 80 ticks into the 96-tick step 3 still goes into step 3 */
    {
        int ix = find_ev(J6_EV_START, from);
        CHECK(ix >= 0);
        if (ix >= 0) run_ticks(TL[ix].tick + 2u * 96u + 80u - now_tick());
    }
    j6_engine_key(2, 1);
    apply();
    j6_engine_key(2, 0);
    apply();
    CHECK_EQ(j6_step_count(&p->step[2]), 2);
    CHECK((p->step[2].note[0] == 62 && p->step[2].note[1] == 67) ||
          (p->step[2].note[0] == 67 && p->step[2].note[1] == 62));
    CHECK_EQ(j6_step_count(&p->step[3]), 0);
    /* a chord key while recording replaces the step with the chord and its tag */
    j6_engine_set(J6_P_CHORD, 1);
    run_ticks(96);                                         /* step 4 */
    j6_engine_key(5, 1);
    apply();
    j6_engine_key(5, 0);
    apply();
    {
        uint8_t raw[4];
        uint32_t nr = j6_chord_raw(0, 5, raw);
        CHECK_EQ(j6_step_count(&p->step[3]), nr);
        CHECK_EQ(p->step[3].chord, j6_chord_tag(5, 0));
    }
    /* the next loop plays them, each on its own step */
    run_ticks(8u * 96u);
    {
        int ix = find_ev(J6_EV_START, from);
        uint32_t t0, k, i, s2 = 0, s3 = 0, other = 0, m2 = 0, m3 = 0;
        uint8_t raw[4];
        uint32_t nr = j6_chord_raw(0, 5, raw), j;
        CHECK(ix >= 0);
        t0 = ix >= 0 ? TL[ix].tick : 0u;
        k = notes_of(J6_SRC_SEQ, from);
        for (i = 0; i < k; i++) {
            if (NT[i].on < (t0 + 768u) * J6_SUB || NT[i].on >= (t0 + 2u * 768u) * J6_SUB) continue;   /* loop 2 */
            if (NT[i].on == (t0 + 768u + 192u) * J6_SUB) {             /* step 3: G and D */
                s2++;
                if (NT[i].note == 67 || NT[i].note == 62) m2++;
            } else if (NT[i].on == (t0 + 768u + 288u) * J6_SUB) {      /* step 4: the F chord */
                s3++;
                for (j = 0; j < nr; j++)
                    if (NT[i].note == raw[j]) m3++;
            } else {
                other++;
            }
        }
        CHECK_EQ(s2, 2);
        CHECK_EQ(m2, 2);
        CHECK_EQ(s3, nr);
        CHECK_EQ(m3, nr);
        CHECK_EQ(other, 0);
    }
    j6_engine_cmd(J6_CMD_REC, 0);                          /* punch out: playback goes on */
    apply();
    j6_engine_status(&st);
    CHECK_EQ(st.recording, 0);
    CHECK_EQ(st.seq_state, J6_SEQ_PLAYING);
    j6_engine_cmd(J6_CMD_STOP, 0);
    apply();
    CHECK(tap_balanced());
}

/* the live phrase during playback runs on the pattern position (PH-21) */
static void test_live_phrase_locks_to_pattern(void)
{
    uint32_t from, k, t0;
    int ix;
    eng();
    j6_engine_set(J6_P_STYLE_ON, 1);                       /* style 1 var 1: 8ths */
    apply();
    from = TLN;
    j6_engine_cmd(J6_CMD_PLAY, 0);
    run_ticks(30);
    j6_engine_key(0, 1);                                   /* pressed at pos ~30 */
    run_ticks(100);
    j6_engine_key(0, 0);
    apply();
    ix = find_ev(J6_EV_START, from);
    CHECK(ix >= 0);
    if (ix < 0) return;
    t0 = TL[ix].tick;
    k = notes_of(J6_SRC_PHRASE_LIVE, from);
    CHECK(k >= 2u);
    if (k >= 2u) {
        CHECK_EQ(NT[0].on, (t0 + 48u) * J6_SUB);           /* the first hit waits for the grid */
        CHECK_EQ(NT[1].on, (t0 + 96u) * J6_SUB);
    }
    j6_engine_cmd(J6_CMD_STOP, 0);
    apply();
    CHECK(tap_balanced());
}

/* settings: per pattern vs global; clamping; sound select clears the knobs */
static void test_settings(void)
{
    const j6_param_info_t *pi;
    j6_status_t st;
    eng();
    pi = j6_param_info(J6_P_VOL);
    CHECK(pi != NULL);
    if (pi) {
        CHECK_EQ(pi->per_pattern, 1);
        CHECK_STR(pi->name, "vOL");
    }
    pi = j6_param_info(J6_P_VELO);
    CHECK(pi != NULL && pi->per_pattern == 0);
    CHECK(j6_param_info(J6_P_COUNT) == NULL);
    j6_engine_set(J6_P_SHUFFLE, 500);
    j6_engine_set(J6_P_VOL, 90);
    j6_engine_set(J6_P_VELO, 80);
    j6_engine_set(J6_P_KNOB0 + J6_KNOB_CUTOFF, 20);
    apply();
    CHECK_EQ(j6_engine_get(J6_P_SHUFFLE), 90);
    CHECK_EQ(j6_engine_get(J6_P_KNOB0 + J6_KNOB_CUTOFF), 20);
    CHECK_EQ(j6_engine_pattern(0)->knob[J6_KNOB_CUTOFF], 20);
    j6_engine_cmd(J6_CMD_PATTERN, 1);                      /* pattern 1-2 has its own settings */
    apply();
    CHECK_EQ(j6_engine_get(J6_P_SHUFFLE), 0);
    CHECK_EQ(j6_engine_get(J6_P_VOL), J6_VOL_DEF);
    CHECK_EQ(j6_engine_get(J6_P_VELO), 80);                /* global */
    CHECK_EQ(j6_engine_get(J6_P_KNOB0 + J6_KNOB_CUTOFF), j6_sounds[0].knob[J6_KNOB_CUTOFF]);
    j6_engine_cmd(J6_CMD_PATTERN, 0);
    apply();
    CHECK_EQ(j6_engine_get(J6_P_VOL), 90);
    CHECK_EQ(j6_engine_get(J6_P_KNOB0 + J6_KNOB_CUTOFF), 20);
    j6_engine_set(J6_P_SOUND, 9);
    apply();
    CHECK_EQ(j6_engine_get(J6_P_SOUND), 9);
    CHECK_EQ(j6_engine_pattern(0)->knob[J6_KNOB_CUTOFF], J6_KNOB_UNSET);
    CHECK_EQ(j6_engine_get(J6_P_KNOB0 + J6_KNOB_CUTOFF), j6_sounds[9].knob[J6_KNOB_CUTOFF]);
    j6_engine_set(J6_P_CNTI, 1);
    apply();
    CHECK_EQ(j6_engine_get(J6_P_CNTI), 2);
    j6_engine_status(&st);
    CHECK_EQ(st.dirty[0], 1u);                             /* only pattern 1-1 was edited */
    /* count-in: 2 beats, then playback and recording */
    j6_engine_cmd(J6_CMD_REC, 0);
    j6_engine_cmd(J6_CMD_PLAY, 0);
    apply();
    run_ticks(10);
    j6_engine_status(&st);
    CHECK_EQ(st.seq_state, J6_SEQ_COUNTIN);
    run_ticks(2u * 96u);
    j6_engine_status(&st);
    CHECK_EQ(st.seq_state, J6_SEQ_PLAYING);
    CHECK_EQ(st.recording, 1);
    j6_engine_cmd(J6_CMD_PANIC, 0);
    j6_engine_cmd(J6_CMD_STOP, 0);
    apply();
}

/* storage: WRITE (PS-1), boot load, unsaved edits lost, globals saved after 2 s of silence (PS-3) */
#define RS_SLOTS (J6_SLOT_GLOBAL + 1u)
static uint8_t RS[RS_SLOTS][J6_PAT_BYTES];
static uint32_t RS_LEN[RS_SLOTS], RS_WRITES;
static int rs_read(void *ctx, uint32_t slot, uint8_t *buf, uint32_t len)
{
    (void)ctx;
    if (slot >= RS_SLOTS || RS_LEN[slot] != len) return -1;
    memcpy(buf, RS[slot], len);
    return 0;
}
static int rs_write(void *ctx, uint32_t slot, const uint8_t *buf, uint32_t len)
{
    (void)ctx;
    if (slot >= RS_SLOTS || len > J6_PAT_BYTES) return -1;
    memcpy(RS[slot], buf, len);
    RS_LEN[slot] = len;
    RS_WRITES++;
    return 0;
}

static void test_storage(void)
{
    j6_store_t store;
    j6_status_t st;
    uint32_t i;
    store.read = rs_read;
    store.write = rs_write;
    store.ctx = NULL;
    memset(RS_LEN, 0, sizeof RS_LEN);
    j6_engine_set_tap(tap, NULL);
    j6_engine_init(FX, J6_FX_MEM_LEN, &store);             /* empty store: defaults */
    TLN = 0;
    j6_engine_set(J6_P_TEMPO10, 1355);
    j6_engine_set(J6_P_VELO, 64);
    j6_engine_set(J6_P_CHORD_SET, 7);
    j6_engine_cmd(J6_CMD_PATTERN_COPY, 9);
    apply();
    j6_engine_poll();                                      /* the copy is saved at once (SQ-45) */
    CHECK_EQ(RS_LEN[9], J6_PAT_BYTES);
    CHECK_EQ(RS_LEN[0], 0);
    CHECK_EQ(RS_LEN[J6_SLOT_GLOBAL], 0);
    CHECK_EQ(j6_engine_write(0), 0);                       /* WRITE Ptn */
    CHECK_EQ(RS_LEN[0], J6_PAT_BYTES);
    CHECK_EQ(RS_LEN[J6_SLOT_GLOBAL], J6_GLOBAL_BYTES);     /* PS-3: WRITE saves the changed globals too */
    CHECK_EQ(RS[J6_SLOT_GLOBAL][7], 64);                   /* vELo */
    RS_LEN[J6_SLOT_GLOBAL] = 0;
    i = RS_WRITES;
    CHECK_EQ(j6_engine_write(0), 0);                       /* nothing changed: the globals are not rewritten */
    CHECK_EQ(RS_LEN[J6_SLOT_GLOBAL], 0);
    CHECK_EQ(RS_WRITES, i + 1u);
    j6_engine_status(&st);
    CHECK_EQ(st.dirty[0], 0u);
    j6_engine_set(J6_P_TEMPO10, 900);                      /* unsaved: lost at power-off */
    j6_engine_cmd(J6_CMD_PATTERN, 2);
    apply();
    j6_engine_poll();
    CHECK_EQ(RS_LEN[J6_SLOT_GLOBAL], 0);                   /* not yet: 2 s */
    run_frames(2u * J6_FS + 64u);
    j6_engine_poll();
    CHECK_EQ(RS_LEN[J6_SLOT_GLOBAL], J6_GLOBAL_BYTES);
    j6_engine_init(FX, J6_FX_MEM_LEN, &store);             /* power cycle */
    apply();
    j6_engine_status(&st);
    CHECK_EQ(st.pattern, 2);                               /* the pattern selected at power-off */
    CHECK_EQ(j6_engine_get(J6_P_VELO), 64);
    CHECK_EQ(j6_engine_get(J6_P_CHORD_SET), 7);
    CHECK_EQ(j6_engine_pattern(0)->tempo10, 1355);
    CHECK_EQ(j6_engine_pattern(9)->tempo10, 1355);
    CHECK_EQ(st.dirty[0] | st.dirty[1], 0u);
    j6_engine_factory_reset();                             /* PS-4 */
    apply();
    CHECK_EQ(j6_engine_get(J6_P_VELO), J6_VELO_DEF);
    for (i = 0; i < J6_PATTERNS; i++) CHECK_EQ(RS_LEN[i], J6_PAT_BYTES);
    j6_engine_init(FX, J6_FX_MEM_LEN, &store);
    apply();
    CHECK_EQ(j6_engine_pattern(0)->tempo10, J6_TEMPO10_DEF);
    j6_engine_init(FX, J6_FX_MEM_LEN, NULL);
    CHECK_EQ(j6_engine_write(1), -1);                      /* no store */
}

/* MIDI clock in (MI-8): 24 PPQN at 100 BPM drives the sequencer; Start / Stop */
static void test_midi_clock_in(void)
{
    j6_status_t st;
    uint32_t i, from, frames_per_clock = 44100u * 60u / (100u * 24u);   /* 1102 */
    eng();
    from = TLN;
    j6_engine_midi_in(J6_PORT_USB, 0xFA, 0, 0);
    for (i = 0; i < 24u * 8u; i++) {
        j6_engine_midi_in(J6_PORT_USB, 0xF8, 0, 0);
        run_frames(frames_per_clock - frames_per_clock % J6_CR);
    }
    j6_engine_status(&st);
    CHECK_EQ(st.ext_clock, 1);
    CHECK_EQ(st.seq_state, J6_SEQ_PLAYING);
    CHECK_RANGE(st.tempo10, 990, 1060);
    CHECK(find_ev(J6_EV_START, from) >= 0);
    j6_engine_midi_in(J6_PORT_USB, 0xFC, 0, 0);
    apply();
    j6_engine_status(&st);
    CHECK_EQ(st.seq_state, J6_SEQ_STOPPED);
    run_frames(J6_FS);                                     /* 500 ms without clock: internal again */
    j6_engine_status(&st);
    CHECK_EQ(st.ext_clock, 0);
    CHECK_EQ(st.tempo10, J6_TEMPO10_DEF);
    j6_engine_set(J6_P_SYNC, J6_SYNC_INT);                 /* InT ignores clock in */
    apply();
    j6_engine_midi_in(J6_PORT_USB, 0xF8, 0, 0);
    j6_engine_midi_in(J6_PORT_USB, 0xFA, 0, 0);
    apply();
    j6_engine_status(&st);
    CHECK_EQ(st.ext_clock, 0);
    CHECK_EQ(st.seq_state, J6_SEQ_STOPPED);
}

/* the whole engine with audio: the output stays inside +-J6_OUT_MAX and the phrase is audible */
static void test_render_audio(void)
{
    static int32_t out[J6_FS * 2];
    uint32_t i;
    eng();
    j6_engine_set(J6_P_CHORD, 1);
    j6_engine_set(J6_P_STYLE_ON, 1);
    j6_engine_set(J6_P_STYLE, 6);
    j6_engine_set(J6_P_VAR, 2);
    j6_engine_key(0, 1);
    memset(out, 0, sizeof out);
    for (i = 0; i + J6_BLOCK <= J6_FS; i += J6_BLOCK) {
        j6_engine_render(out + 2u * i, J6_BLOCK);
        drain();
    }
    CHECK(j6t_peak(out, J6_FS) > 1000);
    CHECK(j6t_peak(out, J6_FS) <= J6_OUT_MAX);
    j6_engine_key(0, 0);
    j6_engine_cmd(J6_CMD_PANIC, 0);
    memset(out, 0, sizeof out);
    j6_engine_render(out, J6_BLOCK);
    j6_engine_render(out, J6_BLOCK);
    memset(out, 0, sizeof out);
    j6_engine_render(out, J6_BLOCK);
    CHECK_EQ(j6t_peak(out, J6_BLOCK), 0);
    CHECK(tap_balanced());
}

/* ---- review regressions ------------------------------------------------------------------------- */

/* SQ-12 / SQ-14 with KEY transpose: the key that lights is the key that deletes the note */
static void test_step_edit_key_transpose(void)
{
    const j6_pattern_t *p;
    j6_status_t st;
    eng();
    j6_engine_set(J6_P_KEY, 2);
    j6_engine_cmd(J6_CMD_STEP_SELECT, 0);
    apply();
    key_tap(0);                                            /* key C plays D (62) */
    p = j6_engine_pattern(0);
    CHECK_EQ(p->step[0].note[0], 62);
    CHECK_EQ(j6_step_count(&p->step[0]), 1);
    j6_engine_status(&st);
    CHECK_EQ(st.keys_lit, 1u << (0 - J6_KEY_MIN));         /* key C lights, not key D */
    CHECK_EQ(st.keys_dim, 0u);
    key_tap(0);                                            /* the lit key deletes 62 */
    CHECK_EQ(j6_step_count(&p->step[0]), 0);
    j6_engine_status(&st);
    CHECK_EQ(st.keys_lit, 0u);
    /* KEY -3, octave +1: key F plays 60 + 5 - 3 + 12 = 74; it lights and deletes it */
    j6_engine_set(J6_P_KEY, -3);
    j6_engine_set(J6_P_OCT, 1);
    j6_engine_cmd(J6_CMD_STEP_SELECT, 1);
    apply();
    key_tap(5);
    CHECK_EQ(p->step[1].note[0], 74);
    j6_engine_status(&st);
    CHECK_EQ(st.keys_lit, 1u << (5 - J6_KEY_MIN));
    key_tap(5);
    CHECK_EQ(j6_step_count(&p->step[1]), 0);
    /* a MIDI-in note outside the window lights its pitch class at the edge, dimly */
    j6_engine_set(J6_P_KEY, 0);
    j6_engine_set(J6_P_OCT, 0);
    j6_engine_midi_in(J6_PORT_USB, 0x90, 96, 100);         /* C7: key code 36, outside */
    j6_engine_midi_in(J6_PORT_USB, 0x80, 96, 0);
    apply();
    j6_engine_status(&st);
    CHECK_EQ(st.keys_lit, 0u);
    CHECK(st.keys_dim != 0u);
    j6_engine_cmd(J6_CMD_STEP_SELECT, -1);
    apply();
}

/* MI-8: a Start just after a clock begins the pattern on the next clock, not on the ticks still owed */
static void test_ext_start_resync(void)
{
    static const int32_t kc[1] = {0};
    const uint32_t period = 57u * J6_CR;                   /* 912 frames per clock: ~120.9 BPM */
    uint32_t i, from, t1, k;
    uint32_t pulse_tick[32];
    int ix;
    eng();
    j6_engine_set(J6_P_BEAT, J6_BEAT_1_4);                 /* 24-tick steps */
    apply();
    enter_step(0, kc, 1);
    j6_engine_cmd(J6_CMD_STEP_SELECT, -1);
    apply();
    for (i = 0; i < 40u; i++) {
        j6_engine_midi_in(J6_PORT_USB, 0xF8, 0, 0);
        run_frames(period);
    }
    j6_engine_midi_in(J6_PORT_USB, 0xF8, 0, 0);
    apply();                                               /* that clock's first tick has begun */
    from = TLN;
    j6_engine_midi_in(J6_PORT_USB, 0xFA, 0, 0);
    run_frames(period - J6_CR);
    CHECK_EQ(find_ev(J6_EV_START, from), -1);              /* nothing before the first clock after Start */
    CHECK_EQ(notes_of(J6_SRC_SEQ, from), 0);
    for (i = 0; i < 32u; i++) {
        pulse_tick[i] = now_tick() + 1u;                   /* the tick this clock begins */
        j6_engine_midi_in(J6_PORT_USB, 0xF8, 0, 0);
        run_frames(period);
    }
    ix = find_ev(J6_EV_START, from);
    CHECK(ix >= 0);
    if (ix < 0) return;
    t1 = TL[ix].tick;
    CHECK_EQ(t1, pulse_tick[0]);                           /* pos 0 on the first clock */
    k = notes_of(J6_SRC_SEQ, from);
    CHECK(k >= 1u);
    if (k) CHECK_EQ(NT[0].on, pulse_tick[0] * J6_SUB);
    for (i = 0; i < TLN; i++)                              /* every step on its clock: 24 ticks = 6 clocks */
        if ((uint32_t)i >= from && TL[i].ev.type == J6_EV_STEP && TL[i].tick - t1 < 31u * 4u) {
            uint32_t pos = TL[i].tick - t1;
            CHECK_EQ(pos % 24u, 0u);
            CHECK_EQ(TL[i].tick, pulse_tick[pos / 4u]);
        }
    j6_engine_midi_in(J6_PORT_USB, 0xFC, 0, 0);
    apply();
    CHECK(tap_balanced());
}

/* MIDI out: a note ends on the channel it began on, even if CH changed while it sounded */
static void test_midi_out_channel_change(void)
{
    uint32_t mo0;
    eng();
    mo0 = MON;
    j6_engine_key(0, 1);
    apply();
    CHECK_EQ(mo_count(0x90, 60, mo0), 1);
    j6_engine_set(J6_P_CH, 2);
    apply();
    CHECK_EQ(mo_count(0x80, 60, mo0), 1);                  /* ended on channel 1 at the change */
    j6_engine_key(0, 0);
    apply();
    CHECK_EQ(mo_count(0x81, 60, mo0), 0);                  /* no stray Note Off on channel 2 */
    CHECK_EQ(mo_count(0x80, 60, mo0), 1);
    j6_engine_key(4, 1);                                   /* now on channel 2 */
    apply();
    CHECK_EQ(mo_count(0x91, 64, mo0), 1);
    j6_engine_cmd(J6_CMD_PANIC, 0);
    apply();
    CHECK_EQ(mo_count(0x81, 64, mo0), 1);
    CHECK_EQ(mo_count(0xB1, 120, mo0), 1);
    j6_engine_set(J6_P_CH, 2);                             /* the same channel again: nothing sent */
    mo0 = MON;
    apply();
    CHECK_EQ(mo_count(0x80, 255, mo0) + mo_count(0x81, 255, mo0), 0);
    j6_engine_key(4, 0);
    apply();
}

/* PS-3: global settings changed while notes keep sounding are saved by WRITE and on menu exit */
static void test_globals_saved_while_sounding(void)
{
    static const int32_t kc[1] = {0};
    j6_store_t store;
    store.read = rs_read;
    store.write = rs_write;
    store.ctx = NULL;
    memset(RS_LEN, 0, sizeof RS_LEN);
    j6_engine_set_tap(tap, NULL);
    j6_engine_init(FX, J6_FX_MEM_LEN, &store);
    TLN = 0;
    enter_step(0, kc, 1);
    j6_engine_cmd(J6_CMD_STEP_SELECT, -1);
    j6_engine_cmd(J6_CMD_PLAY, 0);
    j6_engine_key(7, 1);                                   /* and a key held */
    apply();
    j6_engine_set(J6_P_VELO, 70);
    run_frames(3u * J6_FS);
    j6_engine_poll();
    CHECK_EQ(RS_LEN[J6_SLOT_GLOBAL], 0);                   /* never silent: poll does not save */
    CHECK_EQ(j6_engine_save_globals(), 0);                 /* the menu is left */
    CHECK_EQ(RS_LEN[J6_SLOT_GLOBAL], J6_GLOBAL_BYTES);
    CHECK_EQ(RS[J6_SLOT_GLOBAL][7], 70);
    RS_LEN[J6_SLOT_GLOBAL] = 0;
    CHECK_EQ(j6_engine_save_globals(), 0);                 /* nothing changed since: no write */
    CHECK_EQ(RS_LEN[J6_SLOT_GLOBAL], 0);
    j6_engine_set(J6_P_CHORD_SET, 11);
    apply();
    CHECK_EQ(j6_engine_write(0), 0);                       /* WRITE saves them too */
    CHECK_EQ(RS_LEN[J6_SLOT_GLOBAL], J6_GLOBAL_BYTES);
    CHECK_EQ(RS[J6_SLOT_GLOBAL][13], 11);
    j6_engine_key(7, 0);
    j6_engine_cmd(J6_CMD_STOP, 0);
    apply();
    j6_engine_init(FX, J6_FX_MEM_LEN, &store);             /* power cycle */
    CHECK_EQ(j6_engine_get(J6_P_VELO), 70);
    CHECK_EQ(j6_engine_get(J6_P_CHORD_SET), 11);
    j6_engine_init(FX, J6_FX_MEM_LEN, NULL);
    CHECK_EQ(j6_engine_save_globals(), -1);                /* no store */
}

/* the command ring: a burst bigger than the platform queue never loses a release */
static void test_cmd_ring_burst(void)
{
    uint32_t i, ok = 0, from, mo0;
    eng();
    from = TLN;
    mo0 = MON;
    CHECK_EQ(j6_engine_key(0, 1), 1);
    CHECK_EQ(j6_engine_midi_in(J6_PORT_USB, 0x90, 70, 100), 1);
    apply();
    CHECK_EQ(notes_of(J6_SRC_KEY, from), 1);
    CHECK_EQ(notes_of(J6_SRC_MIDI, from), 1);
    CHECK(j6_engine_cmd_space() >= 4u * 64u - 64u);       /* room for a whole platform queue, 3x over */
    for (i = 0; i < 400u; i++) ok += j6_engine_midi_in(J6_PORT_USB, 0xB0, 7, i & 127u);   /* a CC sweep */
    CHECK_EQ(ok, 256u - 32u);                              /* the reserve is kept for releases */
    CHECK_EQ(j6_engine_cmd_space(), 0);
    CHECK_EQ(j6_engine_key(1, 1), 0);                      /* a new press is refused (the app can retry) */
    CHECK_EQ(j6_engine_key(0, 0), 1);                      /* the releases still go in */
    CHECK_EQ(j6_engine_midi_in(J6_PORT_USB, 0x80, 70, 0), 1);
    apply();
    CHECK_EQ(count_ev(J6_EV_NOTE_OFF, from), 2);
    CHECK_EQ(mo_count(0x80, 60, mo0), 1);
    CHECK(tap_balanced());
    CHECK_EQ(j6_engine_cmd_space(), 256u - 32u);
}

/* SQ-33 ties and SQ-34 STYLE ON through the engine's own recording glue */
static void test_record_ties_and_style(void)
{
    const j6_pattern_t *p;
    uint32_t i;
    eng();
    j6_engine_set(J6_P_BEAT, J6_BEAT_1);
    apply();
    j6_engine_cmd(J6_CMD_REC, 0);
    j6_engine_cmd(J6_CMD_PLAY, 0);
    apply();
    run_ticks(4u * 96u + 10u);                             /* inside step 5 (index 4) */
    j6_engine_key(2, 1);
    run_ticks(200);                                        /* held across 2 step boundaries */
    j6_engine_key(2, 0);
    apply();
    p = j6_engine_pattern(0);
    CHECK_EQ(p->step[4].note[0], 62);
    CHECK_EQ(p->step[5].flags & J6_STF_TIE, J6_STF_TIE);
    CHECK_EQ(p->step[6].flags & J6_STF_TIE, J6_STF_TIE);
    CHECK_EQ(p->step[7].flags & J6_STF_TIE, 0);
    CHECK_EQ(j6_step_count(&p->step[5]), 0);
    for (i = 0; i < 4u; i++) CHECK_EQ(p->step[i].flags & J6_STF_TIE, 0);
    j6_engine_cmd(J6_CMD_STOP, 0);
    apply();
    /* STYLE ON, style 3: the recorded step gets the flag and the knob style / variation */
    eng();
    j6_engine_set(J6_P_BEAT, J6_BEAT_1);
    j6_engine_set(J6_P_STYLE_ON, 1);
    j6_engine_set(J6_P_STYLE, 3);
    apply();
    j6_engine_cmd(J6_CMD_REC, 0);
    j6_engine_cmd(J6_CMD_PLAY, 0);
    apply();
    run_ticks(20);
    key_tap(0);
    p = j6_engine_pattern(0);
    CHECK_EQ(p->step[0].note[0], 60);
    CHECK_EQ(p->step[0].phrase, j6_phr_pack(1, 3, 1));
    CHECK_EQ(p->step[1].phrase, 0);
    j6_engine_set(J6_P_STYLE_ON, 0);                       /* STYLE OFF: a plain step */
    run_ticks(96);
    key_tap(4);
    CHECK_EQ(p->step[1].note[0], 64);
    CHECK_EQ(p->step[1].phrase, 0);
    j6_engine_cmd(J6_CMD_STOP, 0);
    apply();
}

/* SQ-31 count-in through the engine: Cnt.I 2 = 2 beats of 96 ticks, then START, REC and step 1 together */
static void test_countin_timing(void)
{
    static const int32_t kc[1] = {0};
    uint32_t i, from, c0, ncnt = 0, k;
    int ix, ir;
    j6_status_t st;
    eng();
    enter_step(0, kc, 1);
    j6_engine_cmd(J6_CMD_STEP_SELECT, -1);
    j6_engine_set(J6_P_CNTI, 2);
    apply();
    from = TLN;
    j6_engine_cmd(J6_CMD_REC, 0);
    j6_engine_cmd(J6_CMD_PLAY, 0);
    apply();
    run_ticks(3u * 96u);
    c0 = 0;
    for (i = from; i < TLN; i++)
        if (TL[i].ev.type == J6_EV_COUNTIN) {
            if (!ncnt) c0 = TL[i].tick;
            CHECK_EQ(TL[i].ev.arg, 2u - ncnt);
            CHECK_EQ(TL[i].tick, c0 + 96u * ncnt);
            ncnt++;
        }
    CHECK_EQ(ncnt, 2);
    ix = find_ev(J6_EV_START, from);
    ir = find_ev(J6_EV_REC, from);
    CHECK(ix >= 0 && ir >= 0);
    if (ix < 0 || ir < 0) return;
    CHECK_EQ(TL[ix].tick, c0 + 192u);
    CHECK_EQ(TL[ir].tick, c0 + 192u);
    CHECK_EQ(TL[ix].ev.arg, 0);
    k = notes_of(J6_SRC_SEQ, from);
    CHECK(k >= 1u);
    if (k) CHECK_EQ(NT[0].on, (c0 + 192u) * J6_SUB);       /* no sequencer note during the count-in */
    j6_engine_status(&st);
    CHECK_EQ(st.recording, 1);
    j6_engine_cmd(J6_CMD_STOP, 0);
    apply();
    /* step_now right after the count-in */
    eng();
    j6_engine_set(J6_P_CNTI, 2);
    j6_engine_cmd(J6_CMD_REC, 0);
    j6_engine_cmd(J6_CMD_PLAY, 0);
    apply();
    from = TLN;
    run_ticks(200);
    ix = find_ev(J6_EV_START, from);
    CHECK(ix >= 0);
    j6_engine_status(&st);
    CHECK_EQ(st.seq_state, J6_SEQ_PLAYING);
    CHECK_EQ(st.step_now, 0);
    j6_engine_cmd(J6_CMD_STOP, 0);
    apply();
}

/* CH-3 through the engine: chord + KEY + 12 * oct, trAn on top for the synth; a held chord keeps the
 * octave of its press */
static void test_chord_key_oct_tran(void)
{
    uint8_t raw[4], snd[16];
    uint32_t nr, k, i, from, ns;
    eng();
    j6_engine_set(J6_P_CHORD, 1);
    j6_engine_set(J6_P_KEY, 2);
    j6_engine_set(J6_P_OCT, 1);
    j6_engine_set(J6_P_TRAN, 1);
    apply();
    nr = j6_chord_raw(0, 0, raw);
    CHECK_EQ(nr, 4);
    from = TLN;
    key_tap(0);
    k = notes_of(J6_SRC_CHORD, from);
    CHECK_EQ(k, nr);
    for (i = 0; i < k && i < nr; i++) {
        CHECK_EQ(NT[i].note, raw[i] + 14u);
        CHECK_EQ(NT[i].snote, raw[i] + 15u);
    }
    /* hold the C chord at oct +1, move the octave, play and release G: the C chord comes back at +1 */
    from = TLN;
    j6_engine_key(0, 1);
    apply();
    j6_engine_set(J6_P_OCT, 0);
    apply();
    j6_engine_key(7, 1);
    apply();
    {
        uint8_t rg[4];
        uint32_t ng = j6_chord_raw(0, 7, rg);
        ns = sounding(J6_SRC_CHORD, snd);
        CHECK_EQ(ns, ng);
        for (i = 0; i < ns && i < ng; i++) CHECK_EQ(snd[i], rg[i] + 2u);   /* G at oct 0 */
    }
    j6_engine_key(7, 0);
    apply();
    ns = sounding(J6_SRC_CHORD, snd);
    CHECK_EQ(ns, nr);
    for (i = 0; i < ns && i < nr; i++) CHECK_EQ(snd[i], raw[i] + 14u);
    j6_engine_key(0, 0);
    apply();
    CHECK(tap_balanced());
    /* step entry (SQ-11) writes the same notes: chord + KEY + 12 * oct, never trAn */
    j6_engine_set(J6_P_OCT, 1);
    j6_engine_cmd(J6_CMD_STEP_SELECT, 3);
    apply();
    key_tap(0);
    {
        const j6_step_t *stp = &j6_engine_pattern(0)->step[3];
        CHECK_EQ(j6_step_count(stp), nr);
        for (i = 0; i < nr; i++) CHECK_EQ(stp->note[i], raw[i] + 14u);
    }
    j6_engine_cmd(J6_CMD_STEP_SELECT, -1);
    apply();
}

/* SQ-27 StEP queued in a step that is not the last: the switch is at the next step boundary */
static void test_pattern_change_mid_pattern(void)
{
    uint32_t from, t0;
    int ix, ip;
    eng();
    j6_engine_set(J6_P_BEAT, J6_BEAT_1);
    apply();
    CHECK_EQ(j6_engine_get(J6_P_LAST), 8);
    CHECK_EQ(j6_engine_get(J6_P_PCHG), J6_PCHG_STEP);
    from = TLN;
    j6_engine_cmd(J6_CMD_PLAY, 0);
    run_ticks(2);                                          /* START comes with the first tick */
    ix = find_ev(J6_EV_START, from);
    CHECK(ix >= 0);
    if (ix < 0) return;
    t0 = TL[ix].tick;
    run_ticks(t0 + 130u - now_tick());                     /* pos 130: inside step 2 of 8 */
    j6_engine_cmd(J6_CMD_PATTERN, 4);
    run_ticks(200);
    ip = find_ev(J6_EV_PATTERN, from);
    CHECK(ip >= 0);
    if (ip >= 0) {
        CHECK_EQ(TL[ip].tick, t0 + 192u);                  /* the next step boundary, not t0 + 768 */
        CHECK_EQ(TL[ip].ev.arg, 4);
    }
    j6_engine_cmd(J6_CMD_STOP, 0);
    apply();
}

/* SD-4 end to end: the engine gives the synth the pattern tempo and the measured external tempo */
static uint32_t echo_onset(void)
{
    static int32_t out[J6_FS * 2];
    uint32_t i, on = 0xFFFFFFFFu, quiet = 0, f;
    j6_engine_key(0, 1);
    memset(out, 0, sizeof out);
    for (i = 0; i + J6_BLOCK <= J6_FS; i += J6_BLOCK) {
        j6_engine_render(out + 2u * i, J6_BLOCK);
        drain();
        if (i == 0) j6_engine_key(0, 0);                   /* a 256-frame note */
    }
    for (f = 0; f < J6_FS; f++) {                          /* the first sound after >= 2000 silent frames */
        int32_t a = out[2u * f] < 0 ? -out[2u * f] : out[2u * f];
        if (a > 64) {
            if (quiet >= 2000u) {
                on = f;
                break;
            }
            quiet = 0;
        } else {
            quiet++;
        }
    }
    return on;
}

static void delay_engine_setup(void)
{
    eng();
    j6_engine_set(J6_P_DSYNC, 1);
    j6_engine_set(J6_P_KNOB0 + J6_KNOB_DLY_TIME, 54);      /* 1/8 */
    j6_engine_set(J6_P_KNOB0 + J6_KNOB_DLY_LEVEL, 127);
    j6_engine_set(J6_P_KNOB0 + J6_KNOB_REV_LEVEL, 0);
    j6_engine_set(J6_P_KNOB0 + J6_KNOB_RELEASE, 0);
    j6_engine_set(J6_P_KNOB0 + J6_KNOB_ATTACK, 0);
    apply();
}

static void test_delay_sync_engine(void)
{
    const uint32_t frames_per_clock = 1104u;               /* 44100 * 60 / (100 * 24) = 1102.5, J6_CR multiple */
    uint32_t e120, e60, eext, i;
    delay_engine_setup();
    CHECK_EQ(j6_engine_synth()->param[J6_SP_TEMPO10], 1200);
    e120 = echo_onset();
    delay_engine_setup();
    j6_engine_set(J6_P_TEMPO10, 600);                      /* the pattern tempo */
    apply();
    CHECK_EQ(j6_engine_synth()->param[J6_SP_TEMPO10], 600);
    e60 = echo_onset();
    printf("    d.Syn 1/8 echo onset: %u frames at 120 BPM, %u at 60 BPM\n", (unsigned)e120, (unsigned)e60);
    CHECK_RANGE(e120, 11025u - J6_CR, 11025u + 2u * J6_CR);
    CHECK_RANGE(e60, 22050u - J6_CR, 22050u + 2u * J6_CR);
    /* a pattern load brings its tempo to the synth too */
    delay_engine_setup();
    j6_engine_set(J6_P_TEMPO10, 900);
    j6_engine_cmd(J6_CMD_PATTERN, 1);
    apply();
    CHECK_EQ(j6_engine_synth()->param[J6_SP_TEMPO10], 1200);
    j6_engine_cmd(J6_CMD_PATTERN, 0);
    apply();
    CHECK_EQ(j6_engine_synth()->param[J6_SP_TEMPO10], 900);
    /* an external clock at ~100 BPM: the measured tempo, 1/8 = 12 clocks */
    delay_engine_setup();
    for (i = 0; i < 48u; i++) {
        j6_engine_midi_in(J6_PORT_USB, 0xF8, 0, 0);
        run_frames(frames_per_clock);
    }
    CHECK_RANGE(j6_engine_synth()->param[J6_SP_TEMPO10], 995, 1005);
    {   /* render in J6_CR chunks with each clock on its exact frame (1104 = 69 chunks) */
        static int32_t out[J6_CR * 2];
        static int32_t mono[J6_FS];
        uint32_t f, on = 0xFFFFFFFFu, quiet = 0, n;
        j6_engine_key(0, 1);
        for (n = 0; n + J6_CR <= J6_FS; n += J6_CR) {
            if (n % frames_per_clock == 0u) j6_engine_midi_in(J6_PORT_USB, 0xF8, 0, 0);
            memset(out, 0, sizeof out);
            j6_engine_render(out, J6_CR);
            drain();
            if (n == J6_BLOCK) j6_engine_key(0, 0);
            for (f = 0; f < J6_CR; f++) mono[n + f] = out[2u * f];
        }
        for (f = 0; f < n; f++) {
            int32_t a = mono[f] < 0 ? -mono[f] : mono[f];
            if (a > 64) {
                if (quiet >= 2000u) {
                    on = f;
                    break;
                }
                quiet = 0;
            } else {
                quiet++;
            }
        }
        eext = on;
        printf("    d.Syn 1/8 echo onset under a 100 BPM clock: %u frames (12 clocks = %u)\n", (unsigned)eext,
               (unsigned)(12u * frames_per_clock));
        CHECK_RANGE(eext, 12u * frames_per_clock - J6_CR, 12u * frames_per_clock + 2u * J6_CR);
    }
}

/* SD-5 with audio: 4 voices shared by the sequencer and the keys; the oldest is stolen; a note two
 * producers sound together lasts until the last one ends */
static uint32_t voice_with(uint32_t note)
{
    const j6_synth_t *s = j6_engine_synth();
    uint32_t i, k = 0;
    for (i = 0; i < J6_VOICES; i++)
        if (s->v[i].stage != J6Y_ST_IDLE && s->v[i].stage != J6Y_ST_RELEASE && s->v[i].snote == note) k++;
    return k;
}

static void render_ticks(uint32_t n)
{
    static int32_t out[J6_CR * 2];
    uint32_t target = now_tick() + n;
    while ((int32_t)(now_tick() - target) < 0) {
        j6_engine_render(out, J6_CR);
        drain();
    }
}

static void test_voice_sharing_render(void)
{
    static const int32_t ceg[4] = {0, 4, 7, 11};
    static const int32_t kc[1] = {0};
    j6_status_t st;
    uint32_t i, from;
    int ix;
    eng();
    j6_engine_set(J6_P_BEAT, J6_BEAT_1);
    j6_engine_set(J6_P_KNOB0 + J6_KNOB_RELEASE, 0);        /* 5 ms */
    j6_engine_set(J6_P_KNOB0 + J6_KNOB_DLY_LEVEL, 0);
    j6_engine_set(J6_P_KNOB0 + J6_KNOB_REV_LEVEL, 0);
    apply();
    enter_step(0, ceg, 4);                                 /* C E G B on 4 voices */
    j6_engine_cmd(J6_CMD_STEP_SELECT, -1);
    apply();
    from = TLN;
    j6_engine_cmd(J6_CMD_PLAY, 0);
    render_ticks(20);
    j6_engine_status(&st);
    CHECK_EQ(st.voices, 4);
    for (i = 0; i < 4u; i++) CHECK_EQ(voice_with(60u + (uint32_t)ceg[i]), 1);
    j6_engine_key(2, 1);                                   /* a fifth note from the keys: D */
    render_ticks(4);                                       /* past the steal fade */
    j6_engine_status(&st);
    CHECK_EQ(st.voices, 4);
    CHECK_EQ(voice_with(62), 1);
    CHECK_EQ(voice_with(60), 0);                           /* the oldest (C, the first note-on) went */
    CHECK_EQ(voice_with(64) + voice_with(67) + voice_with(71), 3);
    j6_engine_key(2, 0);
    j6_engine_cmd(J6_CMD_STOP, 0);
    render_ticks(20);
    j6_engine_status(&st);
    CHECK_EQ(st.voices, 0);                                /* released to silence (with audio) */
    CHECK(tap_balanced());
    /* SEQ and KEY on the same note: releasing the key keeps it until the sequencer's note-off */
    eng();
    j6_engine_set(J6_P_BEAT, J6_BEAT_1);
    j6_engine_set(J6_P_LAST, 2);
    j6_engine_set(J6_P_KNOB0 + J6_KNOB_RELEASE, 0);
    j6_engine_set(J6_P_KNOB0 + J6_KNOB_DLY_LEVEL, 0);
    j6_engine_set(J6_P_KNOB0 + J6_KNOB_REV_LEVEL, 0);
    apply();
    enter_step(0, kc, 1);                                  /* step 1 C, step 2 empty */
    j6_engine_cmd(J6_CMD_STEP_SELECT, -1);
    apply();
    from = TLN;
    j6_engine_cmd(J6_CMD_PLAY, 0);
    render_ticks(10);
    j6_engine_key(0, 1);                                   /* C from the keys too */
    render_ticks(10);
    j6_engine_key(0, 0);
    render_ticks(10);
    CHECK(voice_with(60) >= 1u);                           /* still sounding: the sequencer holds it */
    ix = find_ev(J6_EV_START, from);
    CHECK(ix >= 0);
    if (ix >= 0) {
        uint32_t t0 = TL[ix].tick, k = notes_of(J6_SRC_SEQ, from);
        CHECK(k >= 1u);
        if (k) {
            CHECK(NT[0].off != 0xFFFFFFFFu || now_tick() < t0 + 96u);
        }
        render_ticks(t0 + 96u + 30u - now_tick());         /* past the step's end and the release */
        k = notes_of(J6_SRC_SEQ, from);
        CHECK(k >= 1u && NT[0].off != 0xFFFFFFFFu);
        CHECK(NT[0].off <= (t0 + 96u) * J6_SUB);
    }
    CHECK_EQ(voice_with(60), 0);
    j6_engine_status(&st);
    CHECK_EQ(st.voices, 0);
    j6_engine_cmd(J6_CMD_STOP, 0);
    render_ticks(4);
    CHECK(tap_balanced());
}

/* owner decisions (docs/J6_DECISIONS.md) */
static uint32_t ev_note(uint32_t type, uint32_t note, uint32_t from)
{
    uint32_t i, k = 0;
    for (i = from; i < TLN; i++)
        if (TL[i].ev.type == type && TL[i].ev.note == note) k++;
    return k;
}

static void chord_change(uint32_t legato, uint32_t *on62, uint32_t *off62, uint32_t *on52)
{
    uint32_t from;
    eng();
    if (!legato) j6_engine_set(J6_P_CHDL, 0);
    j6_engine_set(J6_P_CHORD, 1);
    apply();
    j6_engine_key(0, 1);                                   /* set 1 C: 48 55 62 64 */
    apply();
    from = TLN;
    j6_engine_key(4, 1);                                   /* set 1 E: 52 60 62 67 (shares 62) */
    apply();
    *on62 = ev_note(J6_EV_NOTE_ON, 62, from);
    *off62 = ev_note(J6_EV_NOTE_OFF, 62, from);
    *on52 = ev_note(J6_EV_NOTE_ON, 52, from);
    j6_engine_key(4, 0);
    j6_engine_key(0, 0);
    apply();
    CHECK(tap_balanced());
}

static void test_owner_decisions(void)
{
    static int32_t out[J6_BLOCK * 2];
    uint32_t on62, off62, on52, i;
    int32_t pk;
    j6_status_t st;

    /* defaults */
    eng();
    CHECK_EQ(j6_engine_get(J6_P_HOLDE), 0);
    CHECK_EQ(j6_engine_get(J6_P_CHDL), 1);
    CHECK_EQ(j6_param_info(J6_P_HOLDE)->per_pattern, 0);
    CHECK(strcmp(j6_param_info(J6_P_CHDL)->name, "cHd.L") == 0);

    /* D3: cHd.L On keeps the shared note, OFF retriggers it */
    chord_change(1, &on62, &off62, &on52);
    CHECK_EQ(on52, 1);
    CHECK_EQ(on62, 0);
    CHECK_EQ(off62, 0);
    chord_change(0, &on62, &off62, &on52);
    CHECK_EQ(on52, 1);
    CHECK_EQ(on62, 1);
    CHECK_EQ(off62, 1);

    /* D1: entering step edit releases HOLD by default, keeps it with HoLd.E = KEEP */
    eng();
    j6_engine_cmd(J6_CMD_HOLD, 0);
    apply();
    CHECK_EQ(j6_engine_get(J6_P_HOLD), 1);
    j6_engine_cmd(J6_CMD_STEP_SELECT, 0);
    j6_engine_cmd(J6_CMD_STEP_SELECT, -1);
    apply();
    CHECK_EQ(j6_engine_get(J6_P_HOLD), 0);
    j6_engine_set(J6_P_HOLDE, 1);
    j6_engine_cmd(J6_CMD_HOLD, 0);
    apply();
    j6_engine_cmd(J6_CMD_STEP_SELECT, 0);
    j6_engine_cmd(J6_CMD_STEP_SELECT, -1);
    apply();
    CHECK_EQ(j6_engine_get(J6_P_HOLD), 1);

    /* D2: STOP while playing stops only the sequencer; STOP again kills everything with a fade */
    eng();
    j6_engine_set(J6_P_KNOB0 + J6_KNOB_REV_LEVEL, 127);
    j6_engine_key(0, 1);
    j6_engine_cmd(J6_CMD_PLAY, 0);
    for (i = 0; i < 40; i++) j6_engine_render(memset(out, 0, sizeof out), J6_BLOCK);
    j6_engine_cmd(J6_CMD_STOP, 0);
    j6_engine_render(memset(out, 0, sizeof out), J6_BLOCK);
    j6_engine_status(&st);
    CHECK(st.voices > 0);                                  /* the held key still sounds */
    MON = 0;
    j6_engine_cmd(J6_CMD_STOP, 0);                         /* second STOP: kill */
    j6_engine_render(memset(out, 0, sizeof out), J6_BLOCK);
    drain();
    CHECK(mo_count(0xB0, 120, 0) >= 1);                    /* All Sound Off sent */
    /* the first 256 frames after the kill fade out: no step larger than the signal itself */
    for (i = 0; i < 8; i++) j6_engine_render(memset(out, 0, sizeof out), J6_BLOCK);
    pk = j6t_peak(out, J6_BLOCK);
    CHECK_EQ(pk, 0);                                       /* voices and reverb tail gone */
    j6_engine_status(&st);
    CHECK_EQ(st.voices, 0);
    CHECK_EQ(j6_engine_get(J6_P_HOLD), 0);
    j6_engine_key(0, 0);
    apply();
    j6_engine_key(5, 1);                                   /* plays normally afterwards */
    j6_engine_render(memset(out, 0, sizeof out), J6_BLOCK);
    j6_engine_render(memset(out, 0, sizeof out), J6_BLOCK);
    CHECK(j6t_peak(out, J6_BLOCK) > 0);
    j6_engine_key(5, 0);
    apply();
}

int main(void)
{
    RUN(test_chord_key_block_chord);
    RUN(test_owner_decisions);
    RUN(test_live_arp_style1);
    RUN(test_captured_phrases);
    RUN(test_pattern_tie_last);
    RUN(test_style_off_step_inherits);
    RUN(test_pattern_change_last_step);
    RUN(test_program_change_in);
    RUN(test_midi_in_notes);
    RUN(test_tran_and_key);
    RUN(test_realtime_record);
    RUN(test_record_over_step_edit);
    RUN(test_live_phrase_locks_to_pattern);
    RUN(test_settings);
    RUN(test_storage);
    RUN(test_midi_clock_in);
    RUN(test_render_audio);
    RUN(test_step_edit_key_transpose);
    RUN(test_ext_start_resync);
    RUN(test_midi_out_channel_change);
    RUN(test_globals_saved_while_sounding);
    RUN(test_cmd_ring_burst);
    RUN(test_record_ties_and_style);
    RUN(test_countin_timing);
    RUN(test_chord_key_oct_tran);
    RUN(test_pattern_change_mid_pattern);
    RUN(test_delay_sync_engine);
    RUN(test_voice_sharing_render);
    return j6t_report("engine");
}
