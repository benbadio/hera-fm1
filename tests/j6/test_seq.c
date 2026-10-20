/* SPDX-License-Identifier: GPL-3.0-only */
/* test_seq.c - host tests of the seq module (firmware/src/j6/j6_seq.c, j6_seq_pattern.c, j6_seq_clock.c).
 *
 * Rules: SQ-1 .. SQ-46, section 6.1, PS-2, C-5, MI-8 in docs/J6_SPEC.md; design: docs/J6_ENGINE_DESIGN.md
 * 2.2 and 6. Needs only the seq sources (J6_ISOLATED=1 links nothing else). The [ASSUMED] switches
 * (J6_CFG_WRAP_KEEPS_PHRASE, J6_CFG_REC_NEAREST, J6_CFG_PLAIN_LEGATO) are copied into the instance at
 * init; the tests flip those copies to cover both values of each.
 *
 * Times in the event log are tick * 16 + sub, where tick counts every j6_seq_tick() call of a test. */
#include "j6_test.h"
#include "j6_seq_state.h"

/* ---- fixtures ------------------------------------------------------------------------------------- */

static j6_seq_t S;                    /* 35 KB: static, never on the stack */
static j6_evq_t Q;
static j6t_log_t L;
static uint32_t T;                    /* ticks run since ts_begin() */
static uint32_t LOST;                 /* q.lost summed over every call */
static uint32_t MAXQ;                 /* most events in one queue */

#define P_STYLE(st, v) j6_phr_pack(1, (st), (v))

static void ts_begin(void)
{
    j6_seq_init(&S);
    j6t_log_clear(&L);
    T = 0;
    LOST = 0;
    MAXQ = 0;
}

static void ts_log(void)
{
    uint32_t i;
    for (i = 0; i < Q.n; i++) {
        const j6_event_t *e = &Q.ev[i];
        CHECK(e->src == J6_SRC_SEQ);
        CHECK(e->sub < J6_SUB);
        if (e->type == J6_EV_NOTE_ON) CHECK(e->vel >= 1 && e->vel <= 127 && e->note <= 127);
        if (e->type == J6_EV_NOTE_OFF) CHECK(e->vel == 0 && e->note <= 127);
    }
    j6t_log_add(&L, &Q, T);
    LOST += Q.lost;
    if (Q.n > MAXQ) MAXQ = Q.n;
}

static void ts_play(uint32_t from)
{
    j6_evq_clear(&Q);
    j6_seq_play(&S, from, &Q);
    ts_log();
}

static void ts_stop(void)
{
    j6_evq_clear(&Q);
    j6_seq_stop(&S, &Q);
    ts_log();
}

static void ts_rec(void)
{
    j6_evq_clear(&Q);
    j6_seq_rec(&S, &Q);
    ts_log();
}

static void ts_select(uint32_t idx)
{
    j6_evq_clear(&Q);
    j6_seq_select(&S, idx, &Q);
    ts_log();
}

static void ts_run(uint32_t n)
{
    while (n--) {
        j6_evq_clear(&Q);
        j6_seq_tick(&S, &Q);
        ts_log();
        T++;
    }
}

/* run until T == t */
static void ts_run_to(uint32_t t)
{
    if (t > T) ts_run(t - T);
}

static j6_pattern_t *ts_pat(void)
{
    return j6_seq_edit(&S, j6_seq_cur(&S));
}

/* step `st` of pattern `p` := notes (n of them, single notes) */
static void ts_set(j6_pattern_t *p, uint32_t st, const uint8_t *notes, uint32_t n)
{
    uint32_t i;
    j6_step_clear(&p->step[st]);
    for (i = 0; i < n; i++) j6_step_key_note(p, st, notes[i]);
}

static void ts_set1(j6_pattern_t *p, uint32_t st, uint32_t note)
{
    uint8_t n = (uint8_t)note;
    ts_set(p, st, &n, 1);
}

static void ts_tie(j6_pattern_t *p, uint32_t st)
{
    j6_step_clear(&p->step[st]);
    p->step[st].flags = J6_STF_TIE;
}

/* index of the first logged event of a type (note < 0: any note) at or after index `from`, -1 if none */
static int ts_find(uint32_t from, uint32_t type, int note)
{
    uint32_t i;
    for (i = from; i < L.n; i++)
        if (L.e[i].ev.type == type && (note < 0 || L.e[i].ev.note == (uint32_t)note)) return (int)i;
    return -1;
}

/* time of the k-th (0-based) logged event of a type / note, or 0xFFFFFFFF */
static uint32_t ts_time(uint32_t type, int note, uint32_t k)
{
    int i = -1;
    uint32_t from = 0;
    for (;;) {
        i = ts_find(from, type, note);
        if (i < 0) return 0xFFFFFFFFu;
        if (k-- == 0) return L.e[i].time;
        from = (uint32_t)i + 1u;
    }
}

static uint32_t ts_count(uint32_t type, int note)
{
    uint32_t i, k = 0;
    for (i = 0; i < L.n; i++)
        if (L.e[i].ev.type == type && (note < 0 || L.e[i].ev.note == (uint32_t)note)) k++;
    return k;
}

/* arg of the k-th event of a type, -1 if none */
static int ts_arg(uint32_t type, uint32_t k)
{
    uint32_t i;
    for (i = 0; i < L.n; i++)
        if (L.e[i].ev.type == type && k-- == 0) return L.e[i].ev.arg;
    return -1;
}

/* Strict note check, closer to what a synth sees than j6t_log_balanced(): the log is stably sorted by
 * time (queue order kept within one time); a NOTE_ON of a note already on, or a NOTE_OFF of a note that is
 * off, fails. all_off: nothing may still sound at the end. */
static j6t_logged_t TS_SORT[J6T_LOG_MAX];
static int ts_strict(int all_off)
{
    uint32_t i, j, n = L.n;
    int on[128];
    memset(on, 0, sizeof on);
    for (i = 0; i < n; i++) {
        j6t_logged_t x = L.e[i];
        for (j = i; j > 0 && TS_SORT[j - 1].time > x.time; j--) TS_SORT[j] = TS_SORT[j - 1];
        TS_SORT[j] = x;
    }
    for (i = 0; i < n; i++) {
        const j6_event_t *e = &TS_SORT[i].ev;
        if (e->type == J6_EV_NOTE_ON) {
            if (on[e->note]) {
                fprintf(stderr, "  strict: NOTE_ON %u at %u while on\n", e->note, TS_SORT[i].time);
                return 0;
            }
            on[e->note] = 1;
        } else if (e->type == J6_EV_NOTE_OFF) {
            if (!on[e->note]) {
                fprintf(stderr, "  strict: NOTE_OFF %u at %u while off\n", e->note, TS_SORT[i].time);
                return 0;
            }
            on[e->note] = 0;
        }
    }
    if (all_off)
        for (i = 0; i < 128; i++)
            if (on[i]) {
                fprintf(stderr, "  strict: note %u still on\n", i);
                return 0;
            }
    return 1;
}

/* a small deterministic generator */
static uint32_t RNG = 12345u;
static uint32_t ts_rand(void)
{
    RNG = RNG * 1103515245u + 12345u;
    return (RNG >> 8) & 0xFFFFFFu;
}
static uint32_t ts_rn(uint32_t n)
{
    return n ? ts_rand() % n : 0u;
}

/* every invariant a decoded / edited step keeps */
static int ts_step_valid(const j6_step_t *s)
{
    uint32_t i, k = j6_step_count(s), mask = (uint32_t)(s->flags >> J6_STF_CMASK_SHIFT) & 0x0Fu;
    for (i = 0; i < 4; i++) {
        if (i < k) {
            if (s->note[i] > 127) return 0;
            if (i > 0 && s->note[i] <= s->note[i - 1]) return 0;
        } else {
            if (s->note[i] != J6_NO_NOTE) return 0;
            if (mask & (1u << i)) return 0;
        }
    }
    if (s->flags & 0x0Eu) return 0;
    if ((mask == 0) != (s->chord == J6_NO_CHORD)) return 0;
    if (s->chord != J6_NO_CHORD && (s->chord & 0x0Fu) >= 12) return 0;
    if (k && (s->flags & J6_STF_TIE)) return 0;
    if ((s->phrase & 0x7Fu) >= 108) return 0;
    if (s->spare) return 0;
    return 1;
}

static int ts_pattern_valid(const j6_pattern_t *p)
{
    uint32_t i;
    if (p->tempo10 < J6_TEMPO10_MIN || p->tempo10 > J6_TEMPO10_MAX) return 0;
    if (p->last < 1 || p->last > 64) return 0;
    if (p->beat > J6_BEAT_8) return 0;
    if (p->shuffle < -90 || p->shuffle > 90) return 0;
    if ((p->phrase & 0x7Fu) >= 108) return 0;
    if (p->sound > 63 || p->vol > 127 || p->tran < -12 || p->tran > 12 || p->dsync > 1) return 0;
    if (p->avel < 1 || p->avel > 127 || p->pchg > 8 || p->sclk > 7) return 0;
    for (i = 0; i < 8; i++)
        if (p->knob[i] > 127 && p->knob[i] != J6_KNOB_UNSET) return 0;
    for (i = 0; i < 3; i++)
        if (p->reserved[i]) return 0;
    for (i = 0; i < 64; i++)
        if (!ts_step_valid(&p->step[i])) return 0;
    return 1;
}

static int ts_notes_are(const j6_step_t *s, const int *want, uint32_t n)
{
    uint32_t i;
    if (j6_step_count(s) != n) return 0;
    for (i = 0; i < n; i++)
        if (s->note[i] != want[i]) return 0;
    return 1;
}
#define CHECK_NOTES(st, ...)                                                                        \
    do {                                                                                            \
        static const int ts_w_[] = {__VA_ARGS__};                                                   \
        CHECK(ts_notes_are((st), ts_w_, sizeof ts_w_ / sizeof ts_w_[0]));                            \
    } while (0)
#define CHECK_EMPTY(st) CHECK_EQ(j6_step_count(st), 0)

/* ======================================================================================================
 * Data: defaults (SQ-46, SQ-44)
 * ==================================================================================================== */

static void test_pattern_init(void)
{
    j6_pattern_t p;
    uint32_t i;
    memset(&p, 0xA5, sizeof p);
    j6_pattern_init(&p);
    CHECK_EQ(p.tempo10, 1200);
    CHECK_EQ(p.last, 8);
    CHECK_EQ(p.beat, J6_BEAT_4);
    CHECK_EQ(j6_beat_ticks(p.beat), 384);
    CHECK_EQ(p.shuffle, 0);
    CHECK_EQ(p.phrase, 0);
    CHECK_EQ(j6_phr_on(p.phrase), 0);
    CHECK_EQ(j6_phr_style(p.phrase), 1);
    CHECK_EQ(j6_phr_var(p.phrase), 1);
    CHECK_EQ(p.sound, 0);
    CHECK_EQ(p.vol, 100);
    CHECK_EQ(p.tran, 0);
    CHECK_EQ(p.dsync, 0);
    CHECK_EQ(p.avel, 127);
    CHECK_EQ(p.pchg, J6_PCHG_STEP);
    CHECK_EQ(j6_sclk_ppb(p.sclk), 2);
    for (i = 0; i < 8; i++) CHECK_EQ(p.knob[i], J6_KNOB_UNSET);
    for (i = 0; i < 3; i++) CHECK_EQ(p.reserved[i], 0);
    for (i = 0; i < 64; i++) {
        const j6_step_t *s = &p.step[i];
        CHECK(s->note[0] == J6_NO_NOTE && s->note[1] == J6_NO_NOTE && s->note[2] == J6_NO_NOTE &&
              s->note[3] == J6_NO_NOTE);
        CHECK(s->flags == 0 && s->phrase == 0 && s->chord == J6_NO_CHORD && s->spare == 0);
    }
    CHECK(ts_pattern_valid(&p));
    CHECK_EQ(j6_pattern_step_ticks(&p), 384);
    CHECK_EQ(j6_pattern_len_ticks(&p), 3072);
}

static void test_pattern_clear(void)
{
    j6_pattern_t p, ref;
    uint32_t i;
    j6_pattern_init(&p);
    p.tempo10 = 955;
    p.last = 33;
    p.beat = J6_BEAT_1_3;
    p.shuffle = -40;
    p.phrase = P_STYLE(7, 11);
    p.sound = 42;
    p.vol = 77;
    p.tran = -5;
    p.dsync = 1;
    p.avel = 64;
    p.pchg = 3;
    p.sclk = 6;
    for (i = 0; i < 8; i++) p.knob[i] = (uint8_t)(10 * i);
    for (i = 0; i < 64; i++) {
        ts_set1(&p, i, 40 + i);
        p.step[i].phrase = P_STYLE(2, 3);
    }
    ts_tie(&p, 5);
    ref = p;
    j6_pattern_clear(&p);
    for (i = 0; i < 64; i++) {
        CHECK_EMPTY(&p.step[i]);
        CHECK(p.step[i].flags == 0 && p.step[i].phrase == 0 && p.step[i].chord == J6_NO_CHORD);
    }
    CHECK_EQ(p.last, 8);
    CHECK_EQ(p.phrase, 0);
    /* kept: tempo, the *1 settings, shuffle, sound and knobs */
    CHECK_EQ(p.tempo10, ref.tempo10);
    CHECK_EQ(p.beat, ref.beat);
    CHECK_EQ(p.shuffle, ref.shuffle);
    CHECK_EQ(p.sound, ref.sound);
    CHECK_EQ(p.vol, ref.vol);
    CHECK_EQ(p.tran, ref.tran);
    CHECK_EQ(p.dsync, ref.dsync);
    CHECK_EQ(p.avel, ref.avel);
    CHECK_EQ(p.pchg, ref.pchg);
    CHECK_EQ(p.sclk, ref.sclk);
    CHECK_MEM(p.knob, ref.knob, 8);
    CHECK(ts_pattern_valid(&p));
    /* init afterwards resets the sound as well (SQ-46) */
    j6_pattern_init(&p);
    CHECK_EQ(p.sound, 0);
    CHECK_EQ(p.tempo10, 1200);
}

static void test_step_basics(void)
{
    j6_step_t s;
    memset(&s, 0x11, sizeof s);
    j6_step_clear(&s);
    CHECK_EQ(j6_step_count(&s), 0);
    CHECK(s.flags == 0 && s.phrase == 0 && s.chord == J6_NO_CHORD && s.spare == 0);
    CHECK_EQ(j6_step_find(&s, 60), -1);
    s.note[0] = 48;
    s.note[1] = 60;
    CHECK_EQ(j6_step_count(&s), 2);
    CHECK_EQ(j6_step_find(&s, 60), 1);
    CHECK_EQ(j6_step_find(&s, 48), 0);
    CHECK_EQ(j6_step_find(&s, 61), -1);
    CHECK_EQ(j6_step_find(&s, 255), -1);
    CHECK_EQ(j6_step_count(NULL), 0);
    CHECK_EQ(j6_step_find(NULL, 60), -1);
}

/* ======================================================================================================
 * Step edit (SQ-11 .. SQ-17)
 * ==================================================================================================== */

static void test_key_note(void)
{
    j6_pattern_t p;
    const uint8_t chord[3] = {60, 64, 67};
    j6_pattern_init(&p);
    /* SQ-12: add, kept ascending */
    CHECK_EQ(j6_step_key_note(&p, 3, 64), J6_EDIT_ADDED);
    CHECK_EQ(j6_step_key_note(&p, 3, 60), J6_EDIT_ADDED);
    CHECK_EQ(j6_step_key_note(&p, 3, 72), J6_EDIT_ADDED);
    CHECK_NOTES(&p.step[3], 60, 64, 72);
    CHECK(ts_step_valid(&p.step[3]));
    CHECK_EQ(p.step[3].chord, J6_NO_CHORD);
    CHECK_EQ(j6_step_key_note(&p, 3, 48), J6_EDIT_ADDED);
    CHECK_NOTES(&p.step[3], 48, 60, 64, 72);
    /* a 5th note: nothing happens */
    CHECK_EQ(j6_step_key_note(&p, 3, 50), J6_EDIT_FULL);
    CHECK_NOTES(&p.step[3], 48, 60, 64, 72);
    /* a note that is there is deleted, the order stays */
    CHECK_EQ(j6_step_key_note(&p, 3, 60), J6_EDIT_REMOVED);
    CHECK_NOTES(&p.step[3], 48, 64, 72);
    CHECK(ts_step_valid(&p.step[3]));
    /* a full step can still delete */
    CHECK_EQ(j6_step_key_note(&p, 3, 50), J6_EDIT_ADDED);
    CHECK_EQ(j6_step_key_note(&p, 3, 72), J6_EDIT_REMOVED);
    CHECK_NOTES(&p.step[3], 48, 50, 64);
    /* down to empty */
    j6_step_key_note(&p, 3, 48);
    j6_step_key_note(&p, 3, 50);
    CHECK_EQ(j6_step_key_note(&p, 3, 64), J6_EDIT_REMOVED);
    CHECK_EMPTY(&p.step[3]);
    CHECK(ts_step_valid(&p.step[3]));
    /* adding a note to a tie step clears its tie (SQ-17) */
    ts_tie(&p, 4);
    CHECK_EQ(j6_step_key_note(&p, 4, 61), J6_EDIT_ADDED);
    CHECK_EQ(p.step[4].flags & J6_STF_TIE, 0);
    /* SQ-12: a single note added to a chord, and the chord mask follows the slots */
    CHECK_EQ(j6_step_key_chord(&p, 6, chord, 3, j6_chord_tag(0, 0)), J6_EDIT_REPLACED);
    CHECK_EQ(p.step[6].flags, 0x70);
    CHECK_EQ(j6_step_key_note(&p, 6, 62), J6_EDIT_ADDED);
    CHECK_NOTES(&p.step[6], 60, 62, 64, 67);
    CHECK_EQ(p.step[6].flags, 0xD0);      /* slots 0, 2, 3 are chord notes */
    CHECK_EQ(p.step[6].chord, j6_chord_tag(0, 0));
    CHECK_EQ(j6_step_key_note(&p, 6, 64), J6_EDIT_REMOVED);
    CHECK_NOTES(&p.step[6], 60, 62, 67);
    CHECK_EQ(p.step[6].flags, 0x50);
    CHECK(ts_step_valid(&p.step[6]));
    /* the last chord note gone: the tag goes too */
    j6_step_key_note(&p, 6, 60);
    j6_step_key_note(&p, 6, 67);
    CHECK_NOTES(&p.step[6], 62);
    CHECK_EQ(p.step[6].flags, 0);
    CHECK_EQ(p.step[6].chord, J6_NO_CHORD);
    /* the phrase stays through note edits */
    p.step[7].phrase = P_STYLE(4, 2);
    j6_step_key_note(&p, 7, 70);
    CHECK_EQ(p.step[7].phrase, P_STYLE(4, 2));
    /* ignored input */
    CHECK_EQ(j6_step_key_note(&p, 64, 60), J6_EDIT_NONE);
    CHECK_EQ(j6_step_key_note(&p, 200, 60), J6_EDIT_NONE);
    CHECK_EQ(j6_step_key_note(&p, 0, 128), J6_EDIT_NONE);
    CHECK_EMPTY(&p.step[0]);
    CHECK_EQ(j6_step_key_note(NULL, 0, 60), J6_EDIT_NONE);
    /* MIDI extremes */
    CHECK_EQ(j6_step_key_note(&p, 9, 0), J6_EDIT_ADDED);
    CHECK_EQ(j6_step_key_note(&p, 9, 127), J6_EDIT_ADDED);
    CHECK_NOTES(&p.step[9], 0, 127);
}

static void test_key_chord(void)
{
    j6_pattern_t p;
    const uint8_t c_maj[3] = {60, 64, 67}, f_maj[3] = {65, 69, 72}, messy[5] = {67, 60, 200, 64, 60};
    const uint8_t high[2] = {130, 255};
    const uint8_t five[5] = {50, 52, 54, 56, 58};
    uint8_t tag_c = j6_chord_tag(0, 0), tag_f = j6_chord_tag(5, 0), tag_c_up = j6_chord_tag(0, 1);
    j6_pattern_init(&p);
    /* SQ-11: a chord replaces the step, sets tag and mask */
    p.step[2].phrase = P_STYLE(6, 1);
    CHECK_EQ(j6_step_key_chord(&p, 2, c_maj, 3, tag_c), J6_EDIT_REPLACED);
    CHECK_NOTES(&p.step[2], 60, 64, 67);
    CHECK_EQ(p.step[2].flags, 0x70);
    CHECK_EQ(p.step[2].chord, tag_c);
    CHECK_EQ(p.step[2].phrase, P_STYLE(6, 1));   /* the step phrase is not the chord's business */
    /* a single note added, then SQ-13: the chord key again removes the chord's notes only */
    j6_step_key_note(&p, 2, 72);
    CHECK_NOTES(&p.step[2], 60, 64, 67, 72);
    CHECK_EQ(j6_step_key_chord(&p, 2, c_maj, 3, tag_c), J6_EDIT_REMOVED);
    CHECK_NOTES(&p.step[2], 72);
    CHECK_EQ(p.step[2].chord, J6_NO_CHORD);
    CHECK_EQ(p.step[2].flags, 0);
    CHECK(ts_step_valid(&p.step[2]));
    /* a different chord replaces everything, single notes included */
    j6_step_key_note(&p, 2, 62);
    CHECK_EQ(j6_step_key_chord(&p, 2, f_maj, 3, tag_f), J6_EDIT_REPLACED);
    CHECK_NOTES(&p.step[2], 65, 69, 72);
    CHECK_EQ(p.step[2].chord, tag_f);
    CHECK_EQ(p.step[2].flags, 0x70);
    /* the same chord one octave up is another tag: replaces */
    CHECK_EQ(j6_step_key_chord(&p, 2, c_maj, 3, tag_c), J6_EDIT_REPLACED);
    CHECK_EQ(j6_step_key_chord(&p, 2, c_maj, 3, tag_c_up), J6_EDIT_REPLACED);
    CHECK_EQ(p.step[2].chord, tag_c_up);
    /* a chord on a tie step clears the tie */
    ts_tie(&p, 3);
    CHECK_EQ(j6_step_key_chord(&p, 3, c_maj, 3, tag_c), J6_EDIT_REPLACED);
    CHECK_EQ(p.step[3].flags & J6_STF_TIE, 0);
    CHECK(ts_step_valid(&p.step[3]));
    /* unsorted, duplicated and out-of-range input */
    CHECK_EQ(j6_step_key_chord(&p, 4, messy, 5, tag_c), J6_EDIT_REPLACED);
    CHECK_NOTES(&p.step[4], 60, 64, 67);
    CHECK_EQ(p.step[4].flags, 0x70);
    /* more than 4 notes: the lowest 4 */
    CHECK_EQ(j6_step_key_chord(&p, 5, five, 5, tag_c), J6_EDIT_REPLACED);
    CHECK_NOTES(&p.step[5], 50, 52, 54, 56);
    CHECK_EQ(p.step[5].flags, 0xF0);
    /* nothing playable: nothing changes */
    CHECK_EQ(j6_step_key_chord(&p, 4, high, 2, tag_f), J6_EDIT_NONE);
    CHECK_EQ(j6_step_key_chord(&p, 4, c_maj, 0, tag_f), J6_EDIT_NONE);
    CHECK_EQ(j6_step_key_chord(&p, 4, NULL, 3, tag_f), J6_EDIT_NONE);
    CHECK_NOTES(&p.step[4], 60, 64, 67);
    /* no tag: the notes replace, no chord mask */
    CHECK_EQ(j6_step_key_chord(&p, 6, c_maj, 3, J6_NO_CHORD), J6_EDIT_REPLACED);
    CHECK_NOTES(&p.step[6], 60, 64, 67);
    CHECK_EQ(p.step[6].flags, 0);
    CHECK_EQ(p.step[6].chord, J6_NO_CHORD);
    /* a bad tag (chord index 12..15) is no tag */
    CHECK_EQ(j6_step_key_chord(&p, 7, c_maj, 3, 0x8Cu), J6_EDIT_REPLACED);
    CHECK_EQ(p.step[7].chord, J6_NO_CHORD);
    CHECK(ts_step_valid(&p.step[7]));
    CHECK_EQ(j6_step_key_chord(&p, 64, c_maj, 3, tag_c), J6_EDIT_NONE);
    CHECK_EQ(j6_step_key_chord(NULL, 0, c_maj, 3, tag_c), J6_EDIT_NONE);
}

static void test_tie(void)
{
    j6_pattern_t p;
    uint8_t tg = 0;
    j6_pattern_init(&p);
    ts_set1(&p, 0, 60);
    /* SQ-17: HOLD on a step with notes ties the next step, then the step after the chain */
    CHECK_EQ(j6_step_tie(&p, 0, &tg), J6_EDIT_TIE_ADDED);
    CHECK_EQ(tg, 1);
    CHECK_EQ(j6_step_tie(&p, 0, &tg), J6_EDIT_TIE_ADDED);
    CHECK_EQ(tg, 2);
    CHECK(p.step[1].flags & J6_STF_TIE);
    CHECK(p.step[2].flags & J6_STF_TIE);
    /* the step after the chain has notes: ignored */
    ts_set1(&p, 3, 62);
    CHECK_EQ(j6_step_tie(&p, 0, &tg), J6_EDIT_NONE);
    CHECK_EQ(tg, J6_NONE);
    CHECK_EQ(p.step[3].flags & J6_STF_TIE, 0);
    CHECK_NOTES(&p.step[3], 62);
    /* HOLD on a tie step removes it; the later tie stays (now an orphan) */
    CHECK_EQ(j6_step_tie(&p, 1, &tg), J6_EDIT_TIE_REMOVED);
    CHECK_EQ(tg, 1);
    CHECK_EQ(p.step[1].flags & J6_STF_TIE, 0);
    CHECK(p.step[2].flags & J6_STF_TIE);
    /* an empty step: nothing */
    CHECK_EQ(j6_step_tie(&p, 1, &tg), J6_EDIT_NONE);
    CHECK_EQ(tg, J6_NONE);
    /* ties stop at the last step (no wrap) */
    ts_set1(&p, 6, 64);
    CHECK_EQ(j6_step_tie(&p, 6, &tg), J6_EDIT_TIE_ADDED);
    CHECK_EQ(tg, 7);
    CHECK_EQ(j6_step_tie(&p, 6, &tg), J6_EDIT_NONE);
    CHECK_EQ(tg, J6_NONE);
    CHECK_EQ(p.step[8].flags & J6_STF_TIE, 0);
    CHECK_EQ(p.step[0].flags & J6_STF_TIE, 0);
    /* the last step itself cannot tie forward */
    ts_set1(&p, 7, 65);
    CHECK_EQ(j6_step_tie(&p, 7, &tg), J6_EDIT_NONE);
    /* a step beyond last has no step to tie into */
    ts_set1(&p, 20, 66);
    CHECK_EQ(j6_step_tie(&p, 20, &tg), J6_EDIT_NONE);
    /* after SQ-5 raises last, the chain goes on */
    j6_pattern_page(&p, 1);
    CHECK_EQ(p.last, 16);
    CHECK_EQ(j6_step_tie(&p, 7, &tg), J6_EDIT_TIE_ADDED);
    CHECK_EQ(tg, 8);
    /* a tie into a step whose only content is a phrase flag is fine (no notes) */
    p.step[9].phrase = P_STYLE(3, 3);
    CHECK_EQ(j6_step_tie(&p, 7, &tg), J6_EDIT_TIE_ADDED);
    CHECK_EQ(tg, 9);
    CHECK_EQ(p.step[9].phrase, P_STYLE(3, 3));
    /* last = 64: up to step 64 */
    j6_pattern_set_last(&p, 64);
    ts_set1(&p, 62, 70);
    CHECK_EQ(j6_step_tie(&p, 62, &tg), J6_EDIT_TIE_ADDED);
    CHECK_EQ(tg, 63);
    CHECK_EQ(j6_step_tie(&p, 62, &tg), J6_EDIT_NONE);
    CHECK_EQ(j6_step_tie(&p, 64, &tg), J6_EDIT_NONE);
    CHECK_EQ(tg, J6_NONE);
    CHECK_EQ(j6_step_tie(&p, 0, NULL), J6_EDIT_TIE_ADDED);   /* NULL target is allowed */
    CHECK(p.step[1].flags & J6_STF_TIE);
    CHECK(ts_pattern_valid(&p));
}

static void test_step_phrase(void)
{
    j6_pattern_t p;
    j6_pattern_init(&p);
    j6_step_set_phrase(&p, 5, P_STYLE(9, 12));
    CHECK_EQ(p.step[5].phrase, P_STYLE(9, 12));
    CHECK_EQ(j6_phr_on(p.step[5].phrase), 1);
    j6_step_set_phrase(&p, 5, j6_phr_pack(0, 9, 12));
    CHECK_EQ(p.step[5].phrase, j6_phr_pack(0, 9, 12));
    j6_step_set_phrase(&p, 64, P_STYLE(2, 2));          /* ignored */
    CHECK_EQ(j6_step_count(&p.step[5]), 0);
}

/* ======================================================================================================
 * Pattern edit (SQ-5, SQ-42, SQ-43)
 * ==================================================================================================== */

static void test_insert(void)
{
    j6_pattern_t p;
    uint32_t i;
    j6_pattern_init(&p);
    for (i = 0; i < 64; i++) ts_set1(&p, i, 20 + i);
    j6_pattern_insert(&p, 2);
    CHECK_NOTES(&p.step[0], 20);
    CHECK_NOTES(&p.step[1], 21);
    CHECK_EMPTY(&p.step[2]);
    CHECK_NOTES(&p.step[3], 22);
    CHECK_NOTES(&p.step[63], 82);        /* old step 64 (note 83) is lost */
    CHECK_EQ(p.last, 9);
    /* at the last step: nothing moves, step 64 is cleared */
    j6_pattern_insert(&p, 63);
    CHECK_EMPTY(&p.step[63]);
    CHECK_NOTES(&p.step[62], 81);
    CHECK_EQ(p.last, 10);
    /* last never passes 64 */
    j6_pattern_set_last(&p, 64);
    j6_pattern_insert(&p, 0);
    CHECK_EQ(p.last, 64);
    CHECK_EMPTY(&p.step[0]);
    CHECK_NOTES(&p.step[1], 20);
    /* a tie moves with its step */
    j6_pattern_init(&p);
    ts_set1(&p, 0, 60);
    ts_tie(&p, 1);
    j6_pattern_insert(&p, 1);
    CHECK_EMPTY(&p.step[1]);
    CHECK(p.step[2].flags & J6_STF_TIE);
    j6_pattern_insert(&p, 64);           /* ignored */
    CHECK_EQ(p.last, 9);
    CHECK(ts_pattern_valid(&p));
}

static void test_dupl(void)
{
    j6_pattern_t p;
    uint32_t i;
    j6_pattern_init(&p);
    for (i = 0; i < 64; i++) ts_set1(&p, i, 20 + i);
    j6_pattern_set_last(&p, 3);
    p.step[1].phrase = P_STYLE(5, 5);
    j6_pattern_dupl(&p);
    CHECK_EQ(p.last, 6);
    CHECK_NOTES(&p.step[3], 20);
    CHECK_NOTES(&p.step[4], 21);
    CHECK_NOTES(&p.step[5], 22);
    CHECK_EQ(p.step[4].phrase, P_STYLE(5, 5));
    CHECK_NOTES(&p.step[6], 26);         /* beyond 2*last untouched */
    /* last > 32: the copy stops at step 64, last = 64 */
    j6_pattern_init(&p);
    for (i = 0; i < 64; i++) ts_set1(&p, i, 20 + i);
    j6_pattern_set_last(&p, 40);
    j6_pattern_dupl(&p);
    CHECK_EQ(p.last, 64);
    for (i = 40; i < 64; i++) {
        CHECK_EQ(j6_step_count(&p.step[i]), 1);
        CHECK_EQ(p.step[i].note[0], 20 + i - 40);
    }
    CHECK_NOTES(&p.step[39], 59);
    /* last 32 -> 64 exactly; last 64 -> nothing to copy */
    j6_pattern_set_last(&p, 32);
    j6_pattern_dupl(&p);
    CHECK_EQ(p.last, 64);
    CHECK_NOTES(&p.step[63], 51);
    j6_pattern_dupl(&p);
    CHECK_EQ(p.last, 64);
    CHECK_NOTES(&p.step[63], 51);
    /* last 1 */
    j6_pattern_set_last(&p, 1);
    j6_pattern_dupl(&p);
    CHECK_EQ(p.last, 2);
    CHECK_NOTES(&p.step[1], 20);
}

static void test_last_and_page(void)
{
    j6_pattern_t p;
    j6_pattern_init(&p);
    j6_pattern_page(&p, 0);
    CHECK_EQ(p.last, 8);
    j6_pattern_page(&p, 1);              /* page 2: (2-1)*8 + 1 = 9 > 8 -> last 16 */
    CHECK_EQ(p.last, 16);
    j6_pattern_page(&p, 0);              /* never lowers last */
    CHECK_EQ(p.last, 16);
    j6_pattern_page(&p, 1);
    CHECK_EQ(p.last, 16);
    j6_pattern_set_last(&p, 20);
    j6_pattern_page(&p, 2);              /* 17 <= 20: unchanged */
    CHECK_EQ(p.last, 20);
    j6_pattern_page(&p, 3);              /* 25 > 20 -> 32 */
    CHECK_EQ(p.last, 32);
    j6_pattern_page(&p, 7);
    CHECK_EQ(p.last, 64);
    j6_pattern_set_last(&p, 3);
    j6_pattern_page(&p, 8);              /* ignored */
    CHECK_EQ(p.last, 3);
    j6_pattern_set_last(&p, 0);
    CHECK_EQ(p.last, 1);
    j6_pattern_set_last(&p, 65);
    CHECK_EQ(p.last, 64);
    j6_pattern_set_last(&p, 1000);
    CHECK_EQ(p.last, 64);
    j6_pattern_set_last(&p, 33);
    CHECK_EQ(p.last, 33);
}

static void test_step_ticks(void)
{
    static const uint32_t want[J6_BEAT_COUNT] = {24, 32, 48, 96, 192, 288, 384, 480, 576, 672, 768};
    j6_pattern_t p;
    uint32_t b;
    j6_pattern_init(&p);
    for (b = 0; b < J6_BEAT_COUNT; b++) {
        p.beat = (uint8_t)b;
        CHECK_EQ(j6_pattern_step_ticks(&p), want[b]);
        j6_pattern_set_last(&p, 64);
        CHECK_EQ(j6_pattern_len_ticks(&p), 64 * want[b]);
    }
    CHECK_EQ(j6_pattern_len_ticks(&p), 49152);   /* the longest pattern */
    j6_pattern_set_last(&p, 5);
    p.beat = J6_BEAT_1_3;
    CHECK_EQ(j6_pattern_len_ticks(&p), 160);
}

/* SQ-22 */
static void test_window_at(void)
{
    j6_pattern_t p;
    j6_window_t w;
    const uint8_t ch[3] = {60, 64, 67};
    uint32_t i;
    j6_pattern_init(&p);
    p.beat = J6_BEAT_1_2;
    j6_step_key_chord(&p, 0, ch, 3, j6_chord_tag(0, 0));
    ts_tie(&p, 1);
    ts_tie(&p, 2);
    /* 3 rest */
    ts_tie(&p, 4);                       /* orphan */
    ts_set1(&p, 5, 62);
    ts_tie(&p, 6);
    ts_tie(&p, 7);
    for (i = 0; i < 3; i++) {
        j6_pattern_window_at(&p, i, &w);
        CHECK_EQ(w.n, 3);
        CHECK(w.note[0] == 60 && w.note[1] == 64 && w.note[2] == 67 && w.note[3] == J6_NO_NOTE);
        CHECK_EQ(w.step, 0);
        CHECK_EQ(w.ties, 2);
        CHECK_EQ(w.start, 0);
        CHECK_EQ(w.len, 144);
        CHECK_EQ(w.phrase, 0);
    }
    j6_pattern_window_at(&p, 3, &w);
    CHECK_EQ(w.n, 0);
    CHECK_EQ(w.step, J6_NONE);
    CHECK_EQ(w.len, 0);
    j6_pattern_window_at(&p, 4, &w);     /* orphan tie: no window */
    CHECK_EQ(w.n, 0);
    for (i = 5; i < 8; i++) {
        j6_pattern_window_at(&p, i, &w);
        CHECK_EQ(w.n, 1);
        CHECK_EQ(w.step, 5);
        CHECK_EQ(w.ties, 2);
        CHECK_EQ(w.start, 240);
        CHECK_EQ(w.len, 144);
    }
    /* ties are counted no further than the last step */
    j6_pattern_set_last(&p, 7);
    j6_pattern_window_at(&p, 5, &w);
    CHECK_EQ(w.ties, 1);
    CHECK_EQ(w.len, 96);
    j6_pattern_window_at(&p, 7, &w);     /* not played */
    CHECK_EQ(w.n, 0);
    /* a tie at step 1 is an orphan */
    ts_tie(&p, 0);
    j6_pattern_window_at(&p, 0, &w);
    CHECK_EQ(w.n, 0);
    j6_pattern_window_at(&p, 2, &w);
    CHECK_EQ(w.n, 0);
    j6_pattern_window_at(&p, 64, &w);
    CHECK_EQ(w.n, 0);
    /* a window to the end with last = 64 */
    j6_pattern_init(&p);
    j6_pattern_set_last(&p, 64);
    p.beat = J6_BEAT_8;
    ts_set1(&p, 0, 40);
    for (i = 1; i < 64; i++) ts_tie(&p, i);
    j6_pattern_window_at(&p, 63, &w);
    CHECK_EQ(w.step, 0);
    CHECK_EQ(w.ties, 63);
    CHECK_EQ(w.len, 49152);
}

/* ======================================================================================================
 * Flash format
 * ==================================================================================================== */

static void test_encode_layout(void)
{
    j6_pattern_t p;
    uint8_t b[J6_PAT_BYTES];
    const uint8_t ch[3] = {60, 64, 67};
    uint32_t i;
    j6_pattern_init(&p);
    p.tempo10 = 1234;
    p.last = 17;
    p.beat = J6_BEAT_1_3;
    p.shuffle = -5;
    p.phrase = P_STYLE(4, 7);
    p.sound = 33;
    p.vol = 90;
    p.tran = -3;
    p.dsync = 1;
    p.avel = 101;
    p.pchg = 4;
    p.sclk = 5;
    for (i = 0; i < 8; i++) p.knob[i] = (uint8_t)(i * 3);
    p.knob[7] = J6_KNOB_UNSET;
    j6_step_key_chord(&p, 2, ch, 3, j6_chord_tag(4, -1));
    j6_step_key_note(&p, 2, 72);
    p.step[2].phrase = P_STYLE(8, 2);
    ts_tie(&p, 3);
    memset(b, 0xEE, sizeof b);
    CHECK_EQ(j6_pattern_encode(&p, b), J6_PAT_BYTES);
    CHECK_EQ(b[0], J6_PAT_FORMAT);
    CHECK_EQ(b[1], 1234 & 0xFF);
    CHECK_EQ(b[2], 1234 >> 8);
    CHECK_EQ(b[3], 17);
    CHECK_EQ(b[4], J6_BEAT_1_3);
    CHECK_EQ(b[5], 0xFB);
    CHECK_EQ(b[6], P_STYLE(4, 7));
    CHECK_EQ(b[7], 33);
    CHECK_EQ(b[8], 90);
    CHECK_EQ(b[9], 0xFD);
    CHECK_EQ(b[10], 1);
    CHECK_EQ(b[11], 101);
    CHECK_EQ(b[12], 4);
    CHECK_EQ(b[13], 5);
    for (i = 0; i < 7; i++) CHECK_EQ(b[14 + i], i * 3);
    CHECK_EQ(b[21], J6_KNOB_UNSET);
    for (i = 22; i < 32; i++) CHECK_EQ(b[i], 0);
    /* step 3 (index 2) at 32 + 16 */
    CHECK_EQ(b[48], 60);
    CHECK_EQ(b[49], 64);
    CHECK_EQ(b[50], 67);
    CHECK_EQ(b[51], 72);
    CHECK_EQ(b[52], 0x70);
    CHECK_EQ(b[53], P_STYLE(8, 2));
    CHECK_EQ(b[54], j6_chord_tag(4, -1));
    CHECK_EQ(b[55], 0);
    CHECK_EQ(b[56 + 4], J6_STF_TIE);
    /* an empty step */
    CHECK(b[32] == 0xFF && b[33] == 0xFF && b[34] == 0xFF && b[35] == 0xFF);
    CHECK(b[36] == 0 && b[37] == 0 && b[38] == 0xFF && b[39] == 0);
    CHECK(b[J6_PAT_BYTES - 2] == 0xFF && b[J6_PAT_BYTES - 1] == 0);
    CHECK_EQ(j6_pattern_encode(NULL, b), 0);
}

static void ts_random_pattern(j6_pattern_t *p)
{
    uint32_t i, s;
    j6_pattern_init(p);
    p->tempo10 = (uint16_t)(J6_TEMPO10_MIN + ts_rn(J6_TEMPO10_MAX - J6_TEMPO10_MIN + 1));
    p->last = (uint8_t)(1 + ts_rn(64));
    p->beat = (uint8_t)ts_rn(J6_BEAT_COUNT);
    p->shuffle = (int8_t)((int)ts_rn(181) - 90);
    p->phrase = j6_phr_pack(ts_rn(2), 1 + ts_rn(9), 1 + ts_rn(12));
    p->sound = (uint8_t)ts_rn(64);
    p->vol = (uint8_t)ts_rn(128);
    p->tran = (int8_t)((int)ts_rn(25) - 12);
    p->dsync = (uint8_t)ts_rn(2);
    p->avel = (uint8_t)(1 + ts_rn(127));
    p->pchg = (uint8_t)ts_rn(9);
    p->sclk = (uint8_t)ts_rn(8);
    for (i = 0; i < 8; i++) p->knob[i] = ts_rn(4) ? (uint8_t)ts_rn(128) : (uint8_t)J6_KNOB_UNSET;
    for (s = 0; s < 64; s++) {
        uint32_t what = ts_rn(6), k;
        if (what == 0) continue;
        if (what == 1) {
            ts_tie(p, s);
        } else if (what == 2) {
            uint8_t ch[4];
            uint32_t n = 2 + ts_rn(3);
            for (k = 0; k < n; k++) ch[k] = (uint8_t)ts_rn(128);
            j6_step_key_chord(p, s, ch, n, j6_chord_tag(ts_rn(12), (int32_t)ts_rn(16) - 8));
            if (ts_rn(2)) j6_step_key_note(p, s, ts_rn(128));
        } else {
            uint32_t n = 1 + ts_rn(4);
            for (k = 0; k < n; k++) j6_step_key_note(p, s, ts_rn(128));
        }
        if (ts_rn(3) == 0) p->step[s].phrase = j6_phr_pack(ts_rn(2), 1 + ts_rn(9), 1 + ts_rn(12));
    }
}

static void test_encode_roundtrip(void)
{
    static j6_pattern_t p, d;
    static uint8_t b[J6_PAT_BYTES], b2[J6_PAT_BYTES];
    uint32_t it, bad = 0, invalid = 0;
    RNG = 777u;
    for (it = 0; it < 2000; it++) {
        ts_random_pattern(&p);
        if (!ts_pattern_valid(&p)) invalid++;
        j6_pattern_encode(&p, b);
        memset(&d, 0x5A, sizeof d);
        if (j6_pattern_decode(&d, b) != 0) bad++;
        if (memcmp(&p, &d, sizeof p) != 0) bad++;
        j6_pattern_encode(&d, b2);
        if (memcmp(b, b2, sizeof b) != 0) bad++;
    }
    CHECK_EQ(invalid, 0);
    CHECK_EQ(bad, 0);
    /* the defaults round-trip too */
    j6_pattern_init(&p);
    j6_pattern_encode(&p, b);
    CHECK_EQ(j6_pattern_decode(&d, b), 0);
    CHECK_MEM(&p, &d, sizeof p);
}

static void test_decode_garbage(void)
{
    static j6_pattern_t p, ref, d;
    static uint8_t b[J6_PAT_BYTES], b2[J6_PAT_BYTES];
    uint32_t it, i, bad = 0, fmt_bad = 0;
    j6_pattern_init(&ref);
    /* wrong / unknown format byte: -1 and an init'ed pattern */
    RNG = 4242u;
    for (it = 0; it < 500; it++) {
        for (i = 0; i < J6_PAT_BYTES; i++) b[i] = (uint8_t)ts_rand();
        if (b[0] == J6_PAT_FORMAT) b[0] = 0;
        memset(&p, 0x33, sizeof p);
        if (j6_pattern_decode(&p, b) != -1) fmt_bad++;
        if (memcmp(&p, &ref, sizeof p) != 0) fmt_bad++;
    }
    CHECK_EQ(fmt_bad, 0);
    memset(b, 0, sizeof b);
    b[0] = 2;
    CHECK_EQ(j6_pattern_decode(&p, b), -1);
    CHECK_MEM(&p, &ref, sizeof p);
    memset(&p, 0x33, sizeof p);
    CHECK_EQ(j6_pattern_decode(&p, NULL), -1);
    CHECK_MEM(&p, &ref, sizeof p);
    CHECK_EQ(j6_pattern_decode(NULL, b), -1);
    /* random garbage with the right format byte: accepted, every field valid, and stable */
    for (it = 0; it < 2000; it++) {
        for (i = 0; i < J6_PAT_BYTES; i++) b[i] = (uint8_t)ts_rand();
        b[0] = J6_PAT_FORMAT;
        if (j6_pattern_decode(&p, b) != 0) bad++;
        if (!ts_pattern_valid(&p)) bad++;
        j6_pattern_encode(&p, b2);
        if (j6_pattern_decode(&d, b2) != 0 || memcmp(&p, &d, sizeof p) != 0) bad++;
    }
    CHECK_EQ(bad, 0);
    /* field by field clamping */
    j6_pattern_encode(&ref, b);
    b[1] = 0;
    b[2] = 0;                            /* tempo 0 */
    b[3] = 0;                            /* last 0 */
    b[4] = 200;                          /* beat */
    b[5] = 127;                          /* shuffle +127 */
    b[6] = 0xFF;                         /* phrase: on, code 127 */
    b[7] = 99;                           /* sound */
    b[8] = 200;                          /* vol */
    b[9] = 100;                          /* tran */
    b[10] = 5;                           /* d.Syn */
    b[11] = 0;                           /* A.vLo */
    b[12] = 50;                          /* P.chg */
    b[13] = 9;                           /* S.cLk */
    b[14] = 200;
    b[15] = J6_KNOB_UNSET;
    b[16] = 127;
    b[25] = 7;                           /* a reserved header byte */
    CHECK_EQ(j6_pattern_decode(&p, b), 0);
    CHECK_EQ(p.tempo10, J6_TEMPO10_MIN);
    CHECK_EQ(p.last, 1);
    CHECK_EQ(p.beat, J6_BEAT_8);
    CHECK_EQ(p.shuffle, 90);
    CHECK_EQ(p.phrase, J6_PHR_ON);
    CHECK_EQ(p.sound, 63);
    CHECK_EQ(p.vol, 127);
    CHECK_EQ(p.tran, 12);
    CHECK_EQ(p.dsync, 1);
    CHECK_EQ(p.avel, 1);
    CHECK_EQ(p.pchg, 8);
    CHECK_EQ(p.sclk, 7);
    CHECK_EQ(p.knob[0], 127);
    CHECK_EQ(p.knob[1], J6_KNOB_UNSET);
    CHECK_EQ(p.knob[2], 127);
    CHECK(ts_pattern_valid(&p));
    b[1] = 0xFF;
    b[2] = 0xFF;
    b[3] = 200;
    b[5] = 0x80;                         /* -128 */
    b[6] = 108;                          /* off, code 108: out of range */
    b[9] = 0x80;
    CHECK_EQ(j6_pattern_decode(&p, b), 0);
    CHECK_EQ(p.tempo10, J6_TEMPO10_MAX);
    CHECK_EQ(p.last, 64);
    CHECK_EQ(p.shuffle, -90);
    CHECK_EQ(p.phrase, 0);
    CHECK_EQ(p.tran, -12);
    /* steps: out-of-range notes dropped, re-sorted with their chord bits, duplicates merged */
    j6_pattern_encode(&ref, b);
    {
        uint8_t *s0 = b + 32, *s1 = b + 40, *s2 = b + 48, *s3 = b + 56, *s4 = b + 64, *s5 = b + 72;
        s0[0] = 67; s0[1] = 0xFF; s0[2] = 60; s0[3] = 200;
        s0[4] = 0x50 | 0x0E;             /* chord bits on slots 0 and 2, junk bits 1..3 */
        s0[5] = 0x7F;                    /* bad phrase code, flag off */
        s0[6] = j6_chord_tag(0, 0);
        s0[7] = 9;                       /* spare */
        s1[0] = 60; s1[1] = 60; s1[2] = 64; s1[3] = 0xFF;
        s1[4] = J6_STF_TIE;              /* tie and notes: the notes win */
        s1[5] = 0; s1[6] = 0xFF; s1[7] = 0;
        s2[0] = 60; s2[1] = 64; s2[2] = 0xFF; s2[3] = 0xFF;
        s2[4] = 0x30;                    /* mask, but a bad tag (index 12) */
        s2[5] = 0; s2[6] = 0x8C; s2[7] = 0;
        s3[0] = 60; s3[1] = 0xFF; s3[2] = 0xFF; s3[3] = 0xFF;
        s3[4] = 0x00;                    /* a tag without mask */
        s3[5] = 0; s3[6] = j6_chord_tag(2, 1); s3[7] = 0;
        s4[0] = 0xFF; s4[1] = 0xFF; s4[2] = 0xFF; s4[3] = 0xFF;
        s4[4] = J6_STF_TIE | 0xF0;       /* a tie with mask bits on empty slots */
        s4[5] = 0xEB; s4[6] = j6_chord_tag(2, 1); s4[7] = 0;
        s5[0] = 72; s5[1] = 64; s5[2] = 64; s5[3] = 60;
        s5[4] = 0x20;                    /* chord bit on one copy of the duplicate 64 */
        s5[5] = 0; s5[6] = j6_chord_tag(9, 0); s5[7] = 0;
    }
    CHECK_EQ(j6_pattern_decode(&p, b), 0);
    CHECK(ts_pattern_valid(&p));
    CHECK_NOTES(&p.step[0], 60, 67);
    CHECK_EQ(p.step[0].flags, 0x30);
    CHECK_EQ(p.step[0].chord, j6_chord_tag(0, 0));
    CHECK_EQ(p.step[0].phrase, 0);
    CHECK_EQ(p.step[0].spare, 0);
    CHECK_NOTES(&p.step[1], 60, 64);
    CHECK_EQ(p.step[1].flags, 0);
    CHECK_NOTES(&p.step[2], 60, 64);
    CHECK_EQ(p.step[2].flags, 0);
    CHECK_EQ(p.step[2].chord, J6_NO_CHORD);
    CHECK_EQ(p.step[3].chord, J6_NO_CHORD);
    CHECK_EMPTY(&p.step[4]);
    CHECK_EQ(p.step[4].flags, J6_STF_TIE);
    CHECK_EQ(p.step[4].chord, J6_NO_CHORD);
    CHECK_EQ(p.step[4].phrase, 0xEB);    /* on, code 107 (style 9 var 12): valid */
    CHECK_NOTES(&p.step[5], 60, 64, 72);
    CHECK_EQ(p.step[5].flags, 0x20);     /* 64 is slot 1 now */
    CHECK_EQ(p.step[5].chord, j6_chord_tag(9, 0));
}

/* ======================================================================================================
 * Working copies (PS-2), clipboard (SQ-40, SQ-41), copy (SQ-45)
 * ==================================================================================================== */

static void test_dirty(void)
{
    uint32_t i, any = 0;
    j6_pattern_t ref;
    ts_begin();
    j6_pattern_init(&ref);
    for (i = 0; i < 64; i++) {
        any |= j6_seq_dirty(&S, i);
        if (memcmp(j6_seq_pat(&S, i), &ref, sizeof ref) != 0) any |= 2;
    }
    CHECK_EQ(any, 0);
    CHECK_EQ(j6_seq_cur(&S), 0);
    CHECK_EQ(j6_seq_state(&S), J6_SEQ_STOPPED);
    CHECK_EQ(j6_seq_queued(&S), J6_NONE);
    /* reading does not mark */
    (void)j6_seq_pat(&S, 5);
    CHECK_EQ(j6_seq_dirty(&S, 5), 0);
    /* editing marks only that pattern */
    j6_seq_edit(&S, 5)->tempo10 = 1000;
    CHECK_EQ(j6_seq_dirty(&S, 5), 1);
    CHECK_EQ(j6_seq_dirty(&S, 4), 0);
    CHECK_EQ(j6_seq_dirty(&S, 6), 0);
    CHECK_EQ(j6_seq_pat(&S, 5)->tempo10, 1000);
    j6_seq_edit(&S, 40);
    j6_seq_edit(&S, 63);
    CHECK_EQ(j6_seq_dirty(&S, 40), 1);
    CHECK_EQ(j6_seq_dirty(&S, 63), 1);
    CHECK_EQ(j6_seq_dirty(&S, 39), 0);
    j6_seq_set_saved(&S, 5);
    CHECK_EQ(j6_seq_dirty(&S, 5), 0);
    CHECK_EQ(j6_seq_dirty(&S, 40), 1);
    j6_seq_set_saved(&S, 40);
    j6_seq_set_saved(&S, 63);
    for (i = 0; i < 64; i++) any |= j6_seq_dirty(&S, i);
    CHECK_EQ(any, 0);
    /* out-of-range indices */
    CHECK_EQ(j6_seq_dirty(&S, 64), 0);
    j6_seq_set_saved(&S, 64);
    CHECK(j6_seq_pat(&S, 64) == j6_seq_pat(&S, 0));   /* taken modulo 64: never outside the array */
    /* SQ-45: the working copy, unsaved edits included, to dst; dst unsaved */
    ts_set1(j6_seq_edit(&S, 3), 0, 61);
    j6_seq_edit(&S, 3)->sound = 12;
    j6_seq_set_saved(&S, 3);
    j6_seq_copy_pattern(&S, 3, 9);
    CHECK_EQ(j6_seq_dirty(&S, 9), 1);
    CHECK_EQ(j6_seq_dirty(&S, 3), 0);
    CHECK_MEM(j6_seq_pat(&S, 9), j6_seq_pat(&S, 3), sizeof(j6_pattern_t));
    j6_seq_copy_pattern(&S, 3, 64);      /* ignored */
    j6_seq_copy_pattern(&S, 70, 4);
    CHECK_EQ(j6_seq_dirty(&S, 4), 0);
    /* playback alone never marks a pattern */
    j6_seq_set_saved(&S, 9);
    ts_set1(j6_seq_edit(&S, 0), 0, 60);
    j6_seq_set_saved(&S, 0);
    ts_play(0);
    ts_run(5000);
    ts_stop();
    CHECK_EQ(j6_seq_dirty(&S, 0), 0);
}

static void test_clipboard(void)
{
    j6_pattern_t *p;
    ts_begin();
    p = ts_pat();
    j6_seq_set_saved(&S, 0);
    CHECK_EQ(j6_seq_step_paste(&S, 4), -1);   /* empty clipboard */
    CHECK_EQ(j6_seq_dirty(&S, 0), 0);
    ts_set1(p, 2, 60);
    j6_step_key_note(p, 2, 67);
    p->step[2].phrase = P_STYLE(3, 9);
    ts_tie(p, 6);
    j6_seq_step_copy(&S, 2);
    j6_seq_set_saved(&S, 0);
    CHECK_EQ(j6_seq_step_paste(&S, 6), 0);   /* replaces the tie */
    CHECK_EQ(j6_seq_dirty(&S, 0), 1);
    CHECK_MEM(&p->step[6], &p->step[2], sizeof(j6_step_t));
    /* the clipboard is independent of later edits of the source */
    j6_step_key_note(p, 2, 72);
    CHECK_EQ(j6_seq_step_paste(&S, 7), 0);
    CHECK_NOTES(&p->step[7], 60, 67);
    /* an empty step copied pastes as an empty step */
    j6_seq_step_copy(&S, 10);
    CHECK_EQ(j6_seq_step_paste(&S, 7), 0);
    CHECK_EMPTY(&p->step[7]);
    CHECK_EQ(j6_seq_step_paste(&S, 64), -1);
    j6_seq_step_copy(&S, 64);            /* ignored: the clipboard keeps the empty step */
    CHECK_EQ(j6_seq_step_paste(&S, 8), 0);
    CHECK_EMPTY(&p->step[8]);
    /* the clipboard works on the current pattern */
    ts_select(1);
    ts_set1(ts_pat(), 0, 50);
    j6_seq_step_copy(&S, 0);
    ts_select(0);
    CHECK_EQ(j6_seq_step_paste(&S, 0), 0);
    CHECK_NOTES(&p->step[0], 50);
}

/* ======================================================================================================
 * Transport (design 6.3)
 * ==================================================================================================== */

static void test_select(void)
{
    ts_begin();
    /* SQ-28: stopped, immediate */
    ts_select(5);
    CHECK_EQ(j6_seq_cur(&S), 5);
    CHECK_EQ(L.n, 1);
    CHECK_EQ(L.e[0].ev.type, J6_EV_PATTERN);
    CHECK_EQ(L.e[0].ev.arg, 5);
    CHECK_EQ(j6_seq_queued(&S), J6_NONE);
    ts_select(5);                        /* the same again: loads it again */
    CHECK_EQ(ts_count(J6_EV_PATTERN, -1), 2);
    ts_select(64);
    CHECK_EQ(L.n, 2);
    /* SQ-27: playing, queued */
    ts_play(0);
    ts_run(3);
    ts_select(7);
    CHECK_EQ(j6_seq_cur(&S), 5);
    CHECK_EQ(j6_seq_queued(&S), 7);
    CHECK_EQ(ts_count(J6_EV_PATTERN, -1), 2);
    ts_select(7);                        /* the queued one again: cancel */
    CHECK_EQ(j6_seq_queued(&S), J6_NONE);
    ts_select(9);
    CHECK_EQ(j6_seq_queued(&S), 9);
    ts_select(11);                       /* another one replaces the queue */
    CHECK_EQ(j6_seq_queued(&S), 11);
    ts_select(5);                        /* the current one: cancel */
    CHECK_EQ(j6_seq_queued(&S), J6_NONE);
    ts_run(2000);
    CHECK_EQ(j6_seq_cur(&S), 5);
    CHECK_EQ(ts_count(J6_EV_PATTERN, -1), 2);
    /* [ASSUMED] stop with a pattern queued: it becomes current at once */
    ts_select(12);
    ts_stop();
    CHECK_EQ(j6_seq_cur(&S), 12);
    CHECK_EQ(j6_seq_queued(&S), J6_NONE);
    CHECK_EQ(L.e[L.n - 1].ev.type, J6_EV_PATTERN);
    CHECK_EQ(L.e[L.n - 1].ev.arg, 12);
    /* PLAY pressed, no tick yet: still immediate */
    ts_play(0);
    ts_select(13);
    CHECK_EQ(j6_seq_cur(&S), 13);
    ts_run(1);
    CHECK_EQ(L.e[L.n - 2].ev.type, J6_EV_START);
    CHECK(LOST == 0);
}

static void test_transport(void)
{
    int i;
    ts_begin();
    CHECK_EQ(j6_seq_state(&S), J6_SEQ_STOPPED);
    CHECK_EQ(j6_seq_recording(&S), 0);
    CHECK_EQ(j6_seq_step_now(&S), J6_NONE);
    CHECK_EQ(j6_seq_pos(&S), 0);
    CHECK_EQ(j6_seq_countin_left(&S), 0);
    CHECK_EQ(j6_seq_window(&S)->n, 0);
    /* stopped: ticks do nothing */
    ts_run(10);
    CHECK_EQ(L.n, 0);
    T = 0;
    /* SQ-20: PLAY; START comes with the first tick */
    ts_play(0);
    CHECK_EQ(j6_seq_state(&S), J6_SEQ_PLAYING);
    CHECK_EQ(L.n, 0);
    CHECK_EQ(j6_seq_step_now(&S), J6_NONE);
    ts_run(1);
    CHECK_EQ(L.e[0].ev.type, J6_EV_START);
    CHECK_EQ(L.e[0].ev.arg, 0);
    CHECK_EQ(L.e[1].ev.type, J6_EV_STEP);
    CHECK_EQ(L.e[1].ev.arg, 0);
    CHECK_EQ(j6_seq_step_now(&S), 0);
    CHECK_EQ(j6_seq_pos(&S), 0);
    ts_run(400);
    CHECK_EQ(j6_seq_pos(&S), 400);
    CHECK_EQ(j6_seq_step_now(&S), 1);
    CHECK_EQ(ts_count(J6_EV_STEP, -1), 2);
    ts_play(3);                          /* ignored while playing */
    CHECK_EQ(ts_count(J6_EV_START, -1), 1);
    /* the loop: 8 steps of 384 */
    ts_run_to(3073);
    CHECK_EQ(ts_count(J6_EV_LOOP, -1), 1);
    CHECK_EQ(ts_time(J6_EV_LOOP, -1, 0), 3072u * 16u);
    CHECK_EQ(j6_seq_pos(&S), 0);
    /* SQ-21 stop */
    ts_stop();
    CHECK_EQ(j6_seq_state(&S), J6_SEQ_STOPPED);
    CHECK_EQ(ts_count(J6_EV_STOP, -1), 1);
    CHECK_EQ(j6_seq_step_now(&S), J6_NONE);
    i = (int)L.n;
    ts_stop();                           /* stopped already: nothing */
    CHECK_EQ((int)L.n, i);
    /* SQ-20: from a step; a step at or after last plays from step 1 */
    ts_play(3);
    ts_run(1);
    CHECK_EQ(ts_arg(J6_EV_START, 1), 3);
    CHECK_EQ(j6_seq_pos(&S), 3 * 384);
    CHECK_EQ(j6_seq_step_now(&S), 3);
    ts_stop();
    ts_play(10);
    ts_run(1);
    CHECK_EQ(ts_arg(J6_EV_START, 2), 0);
    ts_stop();
    /* SQ-30: REC -> STANDBY -> REC -> STOPPED, no events */
    i = (int)L.n;
    ts_rec();
    CHECK_EQ(j6_seq_state(&S), J6_SEQ_STANDBY);
    CHECK_EQ(j6_seq_recording(&S), 0);
    ts_run(5);                           /* standby does not tick */
    ts_rec();
    CHECK_EQ(j6_seq_state(&S), J6_SEQ_STOPPED);
    CHECK_EQ((int)L.n, i);
    /* STANDBY -> STOP: stopped, nothing sent */
    ts_rec();
    ts_stop();
    CHECK_EQ(j6_seq_state(&S), J6_SEQ_STOPPED);
    CHECK_EQ((int)L.n, i);
    /* STANDBY -> PLAY with Cnt.I off: playback and recording start together, from the step */
    ts_rec();
    ts_play(2);
    CHECK_EQ(j6_seq_recording(&S), 0);
    ts_run(1);
    CHECK_EQ(j6_seq_recording(&S), 1);
    CHECK(ts_find((uint32_t)i, J6_EV_START, -1) >= 0);
    CHECK_EQ(L.e[ts_find((uint32_t)i, J6_EV_START, -1)].ev.arg, 2);
    CHECK_EQ(L.e[ts_find((uint32_t)i, J6_EV_REC, -1)].ev.arg, 1);
    /* SQ-35: punch out and in */
    ts_run(10);
    ts_rec();
    CHECK_EQ(j6_seq_recording(&S), 0);
    CHECK_EQ(j6_seq_state(&S), J6_SEQ_PLAYING);
    CHECK_EQ(L.e[L.n - 1].ev.type, J6_EV_REC);
    CHECK_EQ(L.e[L.n - 1].ev.arg, 0);
    ts_rec();
    CHECK_EQ(j6_seq_recording(&S), 1);
    CHECK_EQ(L.e[L.n - 1].ev.arg, 1);
    /* stop ends both: REC(0) then STOP */
    ts_stop();
    CHECK_EQ(j6_seq_recording(&S), 0);
    CHECK_EQ(L.e[L.n - 2].ev.type, J6_EV_REC);
    CHECK_EQ(L.e[L.n - 2].ev.arg, 0);
    CHECK_EQ(L.e[L.n - 1].ev.type, J6_EV_STOP);
    /* REC between PLAY and the first tick decides whether the first tick records */
    ts_play(0);
    ts_rec();
    CHECK_EQ(j6_seq_recording(&S), 0);
    ts_run(1);
    CHECK_EQ(j6_seq_recording(&S), 1);
    ts_stop();
    /* PLAY then STOP before the first tick: no START, so no STOP */
    i = (int)L.n;
    ts_play(0);
    ts_stop();
    CHECK_EQ((int)L.n, i);
    CHECK(LOST == 0);
    CHECK(ts_strict(1));
}

/* SQ-31 */
static void test_countin(void)
{
    uint8_t n60 = 60;
    int k;
    ts_begin();
    j6_seq_set_countin(&S, 3);
    ts_set(ts_pat(), 0, &n60, 1);
    ts_rec();
    ts_play(5);                          /* the step is ignored: recording starts at step 1 */
    CHECK_EQ(j6_seq_state(&S), J6_SEQ_COUNTIN);
    CHECK_EQ(j6_seq_countin_left(&S), 3);
    CHECK_EQ(L.n, 0);
    ts_run(1);
    CHECK_EQ(L.n, 1);
    CHECK_EQ(L.e[0].ev.type, J6_EV_COUNTIN);
    CHECK_EQ(L.e[0].ev.arg, 3);
    CHECK_EQ(j6_seq_step_now(&S), J6_NONE);
    CHECK_EQ(j6_seq_recording(&S), 0);
    ts_run_to(96);
    CHECK_EQ(j6_seq_countin_left(&S), 3);
    ts_run(1);
    CHECK_EQ(j6_seq_countin_left(&S), 2);
    ts_run_to(288);
    CHECK_EQ(ts_count(J6_EV_COUNTIN, -1), 3);
    CHECK_EQ(ts_time(J6_EV_COUNTIN, -1, 1), 96u * 16u);
    CHECK_EQ(ts_time(J6_EV_COUNTIN, -1, 2), 192u * 16u);
    CHECK_EQ(ts_arg(J6_EV_COUNTIN, 1), 2);
    CHECK_EQ(ts_arg(J6_EV_COUNTIN, 2), 1);
    CHECK_EQ(ts_count(J6_EV_START, -1), 0);
    CHECK_EQ(ts_count(J6_EV_NOTE_ON, -1), 0);
    CHECK_EQ(j6_seq_countin_left(&S), 1);
    CHECK_EQ(j6_seq_pos(&S), 0);
    /* after 3 * 96 ticks: START(0), REC(1), step 1 plays */
    ts_run(1);
    CHECK_EQ(ts_time(J6_EV_START, -1, 0), 288u * 16u);
    CHECK_EQ(ts_arg(J6_EV_START, 0), 0);
    CHECK_EQ(ts_arg(J6_EV_REC, 0), 1);
    CHECK_EQ(ts_time(J6_EV_NOTE_ON, 60, 0), 288u * 16u);
    k = ts_find(0, J6_EV_START, -1);
    CHECK(k >= 0 && L.e[k + 1].ev.type == J6_EV_REC && L.e[k + 2].ev.type == J6_EV_STEP);
    CHECK_EQ(j6_seq_recording(&S), 1);
    CHECK_EQ(j6_seq_state(&S), J6_SEQ_PLAYING);
    CHECK_EQ(j6_seq_countin_left(&S), 0);
    CHECK_EQ(j6_seq_step_now(&S), 0);
    ts_stop();
    CHECK(ts_strict(1));
    /* Cnt.I values */
    j6_seq_set_countin(&S, 1);
    CHECK_EQ(S.countin, 2);
    j6_seq_set_countin(&S, 9);
    CHECK_EQ(S.countin, 4);
    j6_seq_set_countin(&S, 0);
    CHECK_EQ(S.countin, 0);
    /* REC or STOP during the count-in: stopped, no START so no STOP [ASSUMED] */
    ts_begin();
    j6_seq_set_countin(&S, 2);
    ts_rec();
    ts_play(0);
    ts_run(100);
    ts_rec();
    CHECK_EQ(j6_seq_state(&S), J6_SEQ_STOPPED);
    CHECK_EQ(ts_count(J6_EV_STOP, -1), 0);
    CHECK_EQ(j6_seq_countin_left(&S), 0);
    ts_rec();
    ts_play(0);
    ts_run(10);
    ts_stop();
    CHECK_EQ(j6_seq_state(&S), J6_SEQ_STOPPED);
    CHECK_EQ(ts_count(J6_EV_STOP, -1), 0);
    /* PLAY without STANDBY never counts in */
    ts_play(0);
    ts_run(1);
    CHECK_EQ(ts_count(J6_EV_START, -1), 1);
    CHECK_EQ(j6_seq_recording(&S), 0);
    CHECK(LOST == 0);
}

/* ======================================================================================================
 * Playback: STYLE OFF notes (SQ-21 .. SQ-23)
 * ==================================================================================================== */

static void ts_plain_beat(uint32_t beat, uint32_t legato)
{
    j6_pattern_t *p;
    const uint8_t c[2] = {60, 64};
    uint32_t sl = j6_beat_ticks(beat), len = 3 * sl, g = legato ? sl : j6_gate(sl), t0;
    ts_begin();
    S.cfg_plain_legato = (uint8_t)legato;
    j6_seq_set_vel(&S, 90);
    p = ts_pat();
    p->beat = (uint8_t)beat;
    p->last = 3;
    ts_set(p, 0, c, 2);
    ts_set1(p, 1, 62);                   /* step 2 is a rest */
    ts_play(0);
    ts_run(2 * len);
    ts_stop();
    t0 = ts_time(J6_EV_START, -1, 0);
    CHECK_EQ(t0, 0);
    CHECK_EQ(ts_count(J6_EV_NOTE_ON, -1), 6);
    CHECK_EQ(ts_time(J6_EV_NOTE_ON, 60, 0), 0);
    CHECK_EQ(ts_time(J6_EV_NOTE_ON, 64, 0), 0);
    CHECK_EQ(ts_time(J6_EV_NOTE_OFF, 60, 0), g * 16u);
    CHECK_EQ(ts_time(J6_EV_NOTE_OFF, 64, 0), g * 16u);
    CHECK_EQ(ts_time(J6_EV_NOTE_ON, 62, 0), sl * 16u);
    CHECK_EQ(ts_time(J6_EV_NOTE_OFF, 62, 0), (sl + g) * 16u);
    CHECK_EQ(ts_time(J6_EV_NOTE_ON, 60, 1), len * 16u);
    CHECK_EQ(ts_time(J6_EV_NOTE_OFF, 60, 1), (len + g) * 16u);
    CHECK_EQ(ts_time(J6_EV_NOTE_ON, 62, 1), (len + sl) * 16u);
    CHECK_EQ(L.e[ts_find(0, J6_EV_NOTE_ON, 60)].ev.vel, 90);
    CHECK_EQ(L.e[ts_find(0, J6_EV_NOTE_ON, 62)].ev.vel, 90);
    CHECK_EQ(ts_count(J6_EV_STEP, -1), 6);
    CHECK_EQ(ts_time(J6_EV_STEP, -1, 2), 2u * sl * 16u);
    CHECK_EQ(ts_count(J6_EV_HELD, -1), 0);   /* STYLE OFF windows never feed the phrase */
    CHECK_EQ(ts_count(J6_EV_LOOP, -1), 1);
    CHECK(ts_strict(1));
    CHECK(j6t_log_balanced(&L));
    CHECK(LOST == 0);
}

static void test_plain_beats(void)
{
    uint32_t b;
    for (b = 0; b < J6_BEAT_COUNT; b++) {
        ts_plain_beat(b, 0);
        ts_plain_beat(b, 1);             /* J6_CFG_PLAIN_LEGATO (Q-11) */
    }
}

/* the same note in consecutive steps: retrigger, OFF before ON at the same time (legato) */
static void test_plain_retrigger(void)
{
    j6_pattern_t *p;
    int a, b;
    ts_begin();
    S.cfg_plain_legato = 1;
    p = ts_pat();
    p->beat = J6_BEAT_1_4;
    p->last = 2;
    ts_set1(p, 0, 60);
    ts_set1(p, 1, 60);
    ts_play(0);
    ts_run(48 * 3);
    ts_stop();
    CHECK_EQ(ts_count(J6_EV_NOTE_ON, 60), 6);
    CHECK_EQ(ts_time(J6_EV_NOTE_OFF, 60, 0), 24u * 16u);
    CHECK_EQ(ts_time(J6_EV_NOTE_ON, 60, 1), 24u * 16u);
    a = ts_find(0, J6_EV_NOTE_OFF, 60);
    b = ts_find((uint32_t)ts_find(0, J6_EV_NOTE_ON, 60) + 1u, J6_EV_NOTE_ON, 60);
    CHECK(a >= 0 && b > a);
    CHECK(ts_strict(1));
    CHECK(LOST == 0);
    /* the gate: a 1-step window of 1 beat sounds 86 ticks */
    ts_begin();
    p = ts_pat();
    p->beat = J6_BEAT_1;
    p->last = 1;
    ts_set1(p, 0, 48);
    ts_play(0);
    ts_run(96 * 2);
    ts_stop();
    CHECK_EQ(ts_time(J6_EV_NOTE_OFF, 48, 0), 86u * 16u);
    CHECK_EQ(ts_time(J6_EV_NOTE_ON, 48, 1), 96u * 16u);
    CHECK(ts_strict(1));
}

/* SQ-22 ties in playback, STYLE OFF and STYLE ON */
static void test_ties_playback(void)
{
    j6_pattern_t *p;
    const j6_window_t *w;
    ts_begin();
    p = ts_pat();
    p->beat = J6_BEAT_1_2;               /* 48 */
    ts_set1(p, 0, 60);
    ts_tie(p, 1);
    ts_tie(p, 2);
    /* 3 rest */
    ts_tie(p, 4);                        /* orphan */
    ts_set1(p, 5, 62);
    ts_tie(p, 6);
    ts_tie(p, 7);
    ts_play(0);
    ts_run(384 + 1);
    ts_stop();
    CHECK_EQ(ts_count(J6_EV_NOTE_ON, 60), 2);     /* no retrigger on the ties */
    CHECK_EQ(ts_time(J6_EV_NOTE_OFF, 60, 0), j6_gate(144) * 16u);
    CHECK_EQ(ts_count(J6_EV_NOTE_ON, -1), 3);     /* the orphan tie stays silent */
    CHECK_EQ(ts_time(J6_EV_NOTE_ON, 62, 0), 240u * 16u);
    CHECK_EQ(ts_time(J6_EV_NOTE_OFF, 62, 0), (240u + j6_gate(144)) * 16u);
    CHECK_EQ(ts_time(J6_EV_NOTE_ON, 60, 1), 384u * 16u);   /* no wrap of the ties at the end */
    CHECK_EQ(ts_count(J6_EV_STEP, -1), 9);
    CHECK(ts_strict(1));
    /* PLAY from a tie step: that window is not playing, silent until the next window */
    ts_begin();
    p = ts_pat();
    p->beat = J6_BEAT_1_2;
    ts_set1(p, 0, 60);
    ts_tie(p, 1);
    ts_tie(p, 2);
    ts_set1(p, 3, 64);
    ts_play(1);
    ts_run(48 * 2);
    CHECK_EQ(ts_count(J6_EV_NOTE_ON, -1), 0);
    ts_run(1);
    CHECK_EQ(ts_count(J6_EV_NOTE_ON, 64), 1);
    ts_stop();
    CHECK(ts_strict(1));
    /* the same with a phrase: HELD on the window opener only, HELD n = 0 at the rest */
    ts_begin();
    p = ts_pat();
    p->beat = J6_BEAT_1_2;
    p->phrase = P_STYLE(3, 4);
    {
        const uint8_t c[3] = {60, 64, 67};
        j6_step_key_chord(p, 0, c, 3, j6_chord_tag(0, 0));
    }
    ts_tie(p, 1);
    ts_tie(p, 2);
    ts_tie(p, 4);
    ts_set1(p, 5, 62);
    ts_tie(p, 6);
    ts_tie(p, 7);
    ts_play(0);
    ts_run(1);
    w = j6_seq_window(&S);
    CHECK_EQ(w->n, 3);
    CHECK(w->note[0] == 60 && w->note[1] == 64 && w->note[2] == 67);
    CHECK_EQ(w->step, 0);
    CHECK_EQ(w->ties, 2);
    CHECK_EQ(w->start, 0);
    CHECK_EQ(w->len, 144);
    CHECK_EQ(w->phrase, P_STYLE(3, 4));
    CHECK_EQ(ts_count(J6_EV_HELD, -1), 1);
    ts_run_to(144);
    CHECK_EQ(ts_count(J6_EV_HELD, -1), 1);        /* the ties: nothing */
    CHECK_EQ(j6_seq_window(&S)->n, 3);
    ts_run(1);                                    /* step 4: the rest */
    CHECK_EQ(ts_count(J6_EV_HELD, -1), 2);
    CHECK_EQ(ts_time(J6_EV_HELD, -1, 1), 144u * 16u);
    CHECK_EQ(j6_seq_window(&S)->n, 0);
    ts_run_to(241);
    CHECK_EQ(ts_count(J6_EV_HELD, -1), 3);        /* orphan tie: nothing; step 6 opens */
    CHECK_EQ(ts_time(J6_EV_HELD, -1, 2), 240u * 16u);
    CHECK_EQ(j6_seq_window(&S)->n, 1);
    CHECK_EQ(j6_seq_window(&S)->start, 240);
    ts_run_to(385);
    CHECK_EQ(ts_count(J6_EV_HELD, -1), 4);        /* phrase -> phrase at the loop: one HELD */
    CHECK_EQ(ts_time(J6_EV_HELD, -1, 3), 384u * 16u);
    CHECK_EQ(ts_count(J6_EV_NOTE_ON, -1), 0);     /* the phrase instance plays, not the sequencer */
    ts_stop();
    CHECK_EQ(ts_count(J6_EV_HELD, -1), 5);        /* stop: HELD with n = 0 */
    CHECK_EQ(j6_seq_window(&S)->n, 0);
    CHECK(LOST == 0);
}

/* phrase window -> STYLE OFF window: one HELD reading n = 0, and the plain notes at once */
static void test_phrase_to_plain(void)
{
    j6_pattern_t *p;
    int h;
    ts_begin();
    S.cfg_wrap_keeps = 0;
    p = ts_pat();
    p->beat = J6_BEAT_1_4;
    p->last = 2;
    ts_set1(p, 0, 60);                   /* pattern phrase OFF: plain */
    ts_set1(p, 1, 62);
    p->step[1].phrase = P_STYLE(2, 1);   /* flag on: a phrase window */
    ts_play(0);
    ts_run(1);
    CHECK_EQ(ts_count(J6_EV_NOTE_ON, 60), 1);
    CHECK_EQ(ts_count(J6_EV_HELD, -1), 0);
    ts_run_to(25);
    CHECK_EQ(ts_count(J6_EV_HELD, -1), 1);
    CHECK_EQ(j6_seq_window(&S)->n, 1);
    CHECK_EQ(j6_seq_window(&S)->phrase, P_STYLE(2, 1));
    CHECK_EQ(ts_count(J6_EV_NOTE_ON, 62), 0);
    ts_run_to(49);                       /* the wrap: cur back to the pattern phrase (OFF) */
    CHECK_EQ(ts_count(J6_EV_HELD, -1), 2);
    CHECK_EQ(j6_seq_window(&S)->n, 0);   /* what the glue reads on that HELD */
    h = ts_find(0, J6_EV_HELD, -1);
    h = ts_find((uint32_t)h + 1u, J6_EV_HELD, -1);
    CHECK_EQ(L.e[h].time, 48u * 16u);
    CHECK_EQ(ts_time(J6_EV_NOTE_ON, 60, 1), 48u * 16u);
    ts_stop();
    CHECK(ts_strict(1));
}

/* ======================================================================================================
 * SQ-26: which phrase a step uses, and the Q-13 probe
 * ==================================================================================================== */

/* the Q-13 probe: 8 steps; 1-4 STYLE OFF with the pattern phrase at style 3 v4, 5 STYLE ON style 2 v1 */
static void ts_q13(uint32_t keeps)
{
    j6_pattern_t *p;
    uint32_t s, loop, ph[16];
    uint8_t base = 48;
    ts_begin();
    S.cfg_wrap_keeps = (uint8_t)keeps;
    p = ts_pat();
    p->beat = J6_BEAT_1_4;
    p->phrase = P_STYLE(3, 4);
    for (s = 0; s < 8; s++) ts_set1(p, s, base + s);
    p->step[4].phrase = P_STYLE(2, 1);
    ts_play(0);
    for (loop = 0; loop < 2; loop++)
        for (s = 0; s < 8; s++) {
            ts_run(1);
            ph[loop * 8 + s] = j6_seq_window(&S)->phrase;
            CHECK_EQ(j6_seq_window(&S)->step, s);
            ts_run(23);
        }
    ts_stop();
    for (s = 0; s < 4; s++) CHECK_EQ(ph[s], P_STYLE(3, 4));          /* rule 3 */
    for (s = 4; s < 8; s++) CHECK_EQ(ph[s], P_STYLE(2, 1));          /* rules 1, 2 */
    for (s = 8; s < 12; s++) CHECK_EQ(ph[s], keeps ? P_STYLE(2, 1) : P_STYLE(3, 4));   /* Q-13 */
    for (s = 12; s < 16; s++) CHECK_EQ(ph[s], P_STYLE(2, 1));
    CHECK_EQ(ts_count(J6_EV_HELD, -1), 17);   /* each step opens a window; stop closes */
    CHECK_EQ(ts_count(J6_EV_NOTE_ON, -1), 0);
    CHECK(LOST == 0);
}

static void test_sq26(void)
{
    j6_pattern_t *p;
    uint32_t s;
    ts_q13(1);
    ts_q13(0);
    /* rule 3: PLAY from a step after the flagged one uses the pattern phrase */
    ts_begin();
    p = ts_pat();
    p->beat = J6_BEAT_1_4;
    p->phrase = P_STYLE(3, 4);
    for (s = 0; s < 8; s++) ts_set1(p, s, 50 + s);
    p->step[4].phrase = P_STYLE(2, 1);
    ts_play(5);
    ts_run(1);
    CHECK_EQ(j6_seq_window(&S)->phrase, P_STYLE(3, 4));
    ts_stop();
    /* a rest and a tie step with the flag on still set cur */
    ts_begin();
    p = ts_pat();
    p->beat = J6_BEAT_1_4;
    p->phrase = P_STYLE(3, 4);
    ts_set1(p, 0, 60);
    p->step[1].phrase = P_STYLE(5, 6);   /* a rest with the flag */
    ts_set1(p, 2, 62);
    ts_tie(p, 3);
    p->step[3].phrase = P_STYLE(7, 8);   /* a tie with the flag: affects only later windows */
    ts_set1(p, 4, 64);
    ts_play(0);
    ts_run(1);
    CHECK_EQ(j6_seq_window(&S)->phrase, P_STYLE(3, 4));
    ts_run_to(25);
    CHECK_EQ(j6_seq_window(&S)->n, 0);
    ts_run_to(49);
    CHECK_EQ(j6_seq_window(&S)->phrase, P_STYLE(5, 6));
    ts_run_to(73);
    CHECK_EQ(j6_seq_window(&S)->phrase, P_STYLE(5, 6));
    CHECK_EQ(j6_seq_window(&S)->step, 2);
    ts_run_to(97);
    CHECK_EQ(j6_seq_window(&S)->phrase, P_STYLE(7, 8));
    ts_stop();
    /* the pattern phrase OFF and every flag off: everything plain */
    ts_begin();
    p = ts_pat();
    p->beat = J6_BEAT_1_4;
    for (s = 0; s < 8; s++) ts_set1(p, s, 50 + s);
    ts_play(0);
    ts_run(400);
    ts_stop();
    CHECK_EQ(ts_count(J6_EV_HELD, -1), 0);
    CHECK(ts_count(J6_EV_NOTE_ON, -1) >= 16);
    CHECK(ts_strict(1));
    /* the pattern phrase ON and every flag off: everything uses it */
    ts_begin();
    p = ts_pat();
    p->beat = J6_BEAT_1_4;
    p->phrase = P_STYLE(9, 12);
    for (s = 0; s < 8; s++) ts_set1(p, s, 50 + s);
    ts_play(0);
    for (s = 0; s < 20; s++) {
        ts_run(24);
        CHECK_EQ(j6_seq_window(&S)->phrase, P_STYLE(9, 12));
    }
    ts_stop();
    CHECK_EQ(ts_count(J6_EV_NOTE_ON, -1), 0);
}

/* PH-20 / SQ-26 notes: the pattern phrase changed during playback */
static void test_sq26_live(void)
{
    j6_pattern_t *p;
    ts_begin();
    p = ts_pat();
    p->phrase = P_STYLE(3, 4);           /* bEAt 4: one window per bar */
    ts_set1(p, 0, 60);
    ts_set1(p, 1, 62);
    ts_set1(p, 2, 64);
    p->step[2].phrase = P_STYLE(1, 1);
    ts_play(0);
    ts_run(10);
    CHECK_EQ(ts_count(J6_EV_HELD, -1), 1);
    ts_pat()->phrase = P_STYLE(1, 2);    /* the knobs turned: applies at once to an inheriting window */
    ts_run(1);
    CHECK_EQ(ts_count(J6_EV_HELD, -1), 2);
    CHECK_EQ(ts_time(J6_EV_HELD, -1, 1), 10u * 16u);
    CHECK_EQ(j6_seq_window(&S)->phrase, P_STYLE(1, 2));
    CHECK_EQ(j6_seq_window(&S)->n, 1);
    ts_run(10);
    CHECK_EQ(ts_count(J6_EV_HELD, -1), 2);
    /* STYLE turned off (PH-20): at once; the window's notes play STYLE OFF for the rest of the window */
    ts_pat()->phrase = j6_phr_pack(0, 1, 2);
    ts_run(1);
    CHECK_EQ(ts_count(J6_EV_HELD, -1), 3);
    CHECK_EQ(ts_time(J6_EV_HELD, -1, 2), 21u * 16u);
    CHECK_EQ(j6_seq_window(&S)->n, 0);
    CHECK_EQ(ts_time(J6_EV_NOTE_ON, 60, 0), 21u * 16u);
    ts_run_to(385);
    CHECK_EQ(ts_time(J6_EV_NOTE_OFF, 60, 0), (21u + j6_gate(384u - 21u)) * 16u);
    CHECK_EQ(ts_count(J6_EV_HELD, -1), 3);
    CHECK_EQ(j6_seq_window(&S)->n, 0);
    CHECK_EQ(ts_count(J6_EV_NOTE_ON, 62), 1);
    /* after a flagged step the pattern phrase no longer matters (until PLAY) */
    ts_run_to(769);
    CHECK_EQ(j6_seq_window(&S)->phrase, P_STYLE(1, 1));
    ts_pat()->phrase = P_STYLE(6, 6);
    ts_run(5);
    CHECK_EQ(j6_seq_window(&S)->phrase, P_STYLE(1, 1));
    ts_pat()->phrase = j6_phr_pack(0, 6, 6);
    ts_run(5);
    CHECK_EQ(j6_seq_window(&S)->n, 1);
    CHECK_EQ(j6_seq_window(&S)->phrase, P_STYLE(1, 1));
    CHECK_EQ(ts_count(J6_EV_HELD, -1), 4);
    ts_stop();
    CHECK(ts_strict(1));
}

/* PH-20: STYLE ON/OFF of the pattern phrase applies at once to an inheriting window, also a long one
 * (review finding: bEAt 8, a tie: the window lasts 1536 ticks) */
static void test_ph20_style_onoff_live(void)
{
    j6_pattern_t *p;
    uint32_t g;
    ts_begin();
    p = ts_pat();
    p->beat = J6_BEAT_8;
    p->last = 2;
    p->phrase = P_STYLE(2, 1);
    ts_set1(p, 0, 60);
    ts_tie(p, 1);
    ts_play(0);
    ts_run(100);
    CHECK_EQ(ts_count(J6_EV_HELD, -1), 1);
    CHECK_EQ(j6_seq_window(&S)->n, 1);
    /* ON -> OFF: the phrase stops now, note 60 sounds STYLE OFF for the rest of the window */
    ts_pat()->phrase = j6_phr_pack(0, 2, 1);
    ts_run(1);
    CHECK_EQ(ts_count(J6_EV_HELD, -1), 2);
    CHECK_EQ(ts_time(J6_EV_HELD, -1, 1), 100u * 16u);
    CHECK_EQ(j6_seq_window(&S)->n, 0);
    CHECK_EQ(ts_time(J6_EV_NOTE_ON, 60, 0), 100u * 16u);
    /* OFF -> ON: the STYLE OFF note stops, the window is a phrase window again */
    ts_run_to(200);
    CHECK_EQ(ts_count(J6_EV_NOTE_OFF, 60), 0);
    ts_pat()->phrase = P_STYLE(3, 2);
    ts_run(1);
    CHECK_EQ(ts_count(J6_EV_HELD, -1), 3);
    CHECK_EQ(ts_time(J6_EV_HELD, -1, 2), 200u * 16u);
    CHECK_EQ(ts_time(J6_EV_NOTE_OFF, 60, 0), 200u * 16u);
    CHECK_EQ(j6_seq_window(&S)->n, 1);
    CHECK_EQ(j6_seq_window(&S)->step, 0);
    CHECK_EQ(j6_seq_window(&S)->len, 1536);
    CHECK_EQ(j6_seq_window(&S)->phrase, P_STYLE(3, 2));
    /* the tie step boundary changes nothing */
    ts_run_to(800);
    CHECK_EQ(ts_count(J6_EV_HELD, -1), 3);
    ts_stop();
    CHECK(ts_strict(1));

    /* the reverse start: a plain window, STYLE turned on mid-window */
    ts_begin();
    p = ts_pat();
    p->beat = J6_BEAT_8;
    p->last = 2;
    p->phrase = j6_phr_pack(0, 2, 1);
    ts_set1(p, 0, 60);
    ts_tie(p, 1);
    ts_play(0);
    ts_run(100);
    CHECK_EQ(ts_count(J6_EV_HELD, -1), 0);
    CHECK_EQ(ts_count(J6_EV_NOTE_ON, 60), 1);
    ts_pat()->phrase = P_STYLE(2, 1);
    ts_run(1);
    CHECK_EQ(ts_count(J6_EV_HELD, -1), 1);
    CHECK_EQ(ts_time(J6_EV_HELD, -1, 0), 100u * 16u);
    CHECK_EQ(ts_time(J6_EV_NOTE_OFF, 60, 0), 100u * 16u);
    CHECK_EQ(j6_seq_window(&S)->n, 1);
    CHECK_EQ(j6_seq_window(&S)->phrase, P_STYLE(2, 1));
    /* and off again: back to STYLE OFF for the rest (gate measured from now) */
    ts_run_to(300);
    ts_pat()->phrase = j6_phr_pack(0, 2, 1);
    ts_run(1);
    CHECK_EQ(ts_count(J6_EV_HELD, -1), 2);
    CHECK_EQ(ts_time(J6_EV_NOTE_ON, 60, 1), 300u * 16u);
    g = j6_gate(1536u - 300u);
    ts_run_to(300u + g + 1u);
    CHECK_EQ(ts_time(J6_EV_NOTE_OFF, 60, 1), (300u + g) * 16u);
    ts_stop();
    CHECK(ts_strict(1));

    /* a window that does not inherit (flagged step) ignores STYLE ON/OFF of the pattern phrase */
    ts_begin();
    p = ts_pat();
    p->beat = J6_BEAT_8;
    p->last = 2;
    p->phrase = P_STYLE(2, 1);
    ts_set1(p, 0, 60);
    p->step[0].phrase = P_STYLE(4, 4);
    ts_tie(p, 1);
    ts_play(0);
    ts_run(100);
    ts_pat()->phrase = j6_phr_pack(0, 2, 1);
    ts_run(10);
    CHECK_EQ(ts_count(J6_EV_HELD, -1), 1);
    CHECK_EQ(j6_seq_window(&S)->n, 1);
    CHECK_EQ(ts_count(J6_EV_NOTE_ON, 60), 0);
    ts_stop();
    CHECK(ts_strict(1));
}

/* ======================================================================================================
 * Shuffle (SQ-7)
 * ==================================================================================================== */

static void ts_shuffle(int32_t sh)
{
    j6_pattern_t *p;
    uint32_t s, loop;
    const uint8_t notes[4] = {60, 62, 64, 65};
    int32_t off = sh * 384 / 100;
    ts_begin();
    p = ts_pat();
    p->beat = J6_BEAT_1_4;
    p->last = 4;
    p->shuffle = (int8_t)sh;
    for (s = 0; s < 4; s++) ts_set1(p, s, notes[s]);
    ts_play(0);
    ts_run(96 * 2 + 40);                 /* the last shifted note of loop 2 ends too */
    ts_stop();
    for (loop = 0; loop < 2; loop++)
        for (s = 0; s < 4; s++) {
            uint32_t nominal = (loop * 96 + s * 24) * 16;
            uint32_t on = (uint32_t)((int32_t)nominal + ((s & 1) ? off : 0));
            CHECK_EQ(ts_time(J6_EV_NOTE_ON, notes[s], loop), on);
            CHECK_EQ(ts_time(J6_EV_NOTE_OFF, notes[s], loop), on + 20u * 16u);   /* the gate stays */
            CHECK_EQ(ts_time(J6_EV_STEP, -1, loop * 4 + s), nominal);           /* STEP stays nominal */
        }
    CHECK(ts_strict(1));
    CHECK(LOST == 0);
}

static void test_shuffle(void)
{
    static const int32_t v[] = {0, 1, 3, 4, 5, 25, 50, 90, -1, -4, -5, -25, -50, -90};
    j6_pattern_t *p;
    uint32_t i, s;
    for (i = 0; i < sizeof v / sizeof v[0]; i++) ts_shuffle(v[i]);
    /* longer steps never land on an off-beat sixteenth */
    for (i = J6_BEAT_1_3; i < J6_BEAT_COUNT; i++) {
        ts_begin();
        p = ts_pat();
        p->beat = (uint8_t)i;
        p->last = 4;
        p->shuffle = (int8_t)(i & 1 ? 90 : -90);
        for (s = 0; s < 4; s++) ts_set1(p, s, 60 + s);
        ts_play(0);
        ts_run(4 * j6_beat_ticks(i));
        ts_stop();
        for (s = 0; s < 4; s++) CHECK_EQ(ts_time(J6_EV_NOTE_ON, 60 + (int)s, 0), s * j6_beat_ticks(i) * 16u);
    }
    /* phrase windows: positive shuffle switches H at the nominal boundary (the phrase engine shifts its
     * own hit), negative opens the window at the shifted time (the phrase engine looks ahead) */
    for (i = 0; i < 2; i++) {
        int32_t sh = i ? -90 : 90;
        ts_begin();
        p = ts_pat();
        p->beat = J6_BEAT_1_4;
        p->last = 4;
        p->shuffle = (int8_t)sh;
        p->phrase = P_STYLE(2, 1);
        for (s = 0; s < 4; s++) ts_set1(p, s, 60 + s);
        ts_play(0);
        ts_run(96);
        ts_stop();
        CHECK_EQ(ts_time(J6_EV_HELD, -1, 0), 0);
        CHECK_EQ(ts_time(J6_EV_HELD, -1, 1), sh > 0 ? 24u * 16u : (uint32_t)(24 * 16 + sh * 384 / 100));
        CHECK_EQ(ts_time(J6_EV_HELD, -1, 2), 48u * 16u);
        CHECK_EQ(ts_count(J6_EV_NOTE_ON, -1), 0);
    }
    /* negative shuffle, the same note on and off the beat: retriggered early, OFF before ON */
    ts_begin();
    p = ts_pat();
    p->beat = J6_BEAT_1_4;
    p->last = 2;
    p->shuffle = -90;
    ts_set1(p, 0, 60);
    ts_set1(p, 1, 60);
    ts_play(0);
    ts_run(48 * 2);
    ts_stop();
    CHECK_EQ(ts_time(J6_EV_NOTE_OFF, 60, 0), 39u);
    CHECK_EQ(ts_time(J6_EV_NOTE_ON, 60, 1), 39u);
    CHECK_EQ(ts_time(J6_EV_NOTE_OFF, 60, 1), 39u + 320u);
    CHECK(ts_strict(1));
    /* negative shuffle never reaches across a wrap or a queued pattern change */
    ts_begin();
    p = ts_pat();
    p->beat = J6_BEAT_1_4;
    p->last = 3;                         /* steps at 0, 24 (shifted), 48; the wrap at 72 */
    p->shuffle = -50;
    for (s = 0; s < 3; s++) ts_set1(p, s, 60 + s);
    ts_set1(j6_seq_edit(&S, 1), 0, 70);
    ts_set1(j6_seq_edit(&S, 1), 1, 72);
    ts_play(0);
    ts_run(80);
    CHECK_EQ(ts_time(J6_EV_NOTE_ON, 61, 0), 24u * 16u - 192u);
    CHECK_EQ(ts_time(J6_EV_NOTE_ON, 60, 1), 72u * 16u);
    ts_select(1);                        /* switch at the next step boundary (96 = step 2 of loop 2) */
    ts_run_to(97);
    CHECK_EQ(ts_count(J6_EV_NOTE_ON, 61), 1);   /* step 2 of the old pattern never opens early */
    CHECK_EQ(ts_time(J6_EV_PATTERN, -1, 0), 96u * 16u);
    CHECK_EQ(ts_time(J6_EV_NOTE_ON, 70, 0), 96u * 16u);
    ts_stop();
    CHECK(ts_strict(1));
    /* negative shuffle, a pattern queued after the look-ahead opened the next window: that window has
     * begun, so it plays whole (no cut a few ticks in) and the change waits for the following boundary */
    for (i = 0; i < 2; i++) {
        ts_begin();
        S.cfg_plain_legato = 0;
        p = ts_pat();
        p->beat = J6_BEAT_1_4;
        p->last = 4;
        p->shuffle = -50;
        if (i) p->phrase = P_STYLE(2, 1);
        for (s = 0; s < 4; s++) ts_set1(p, s, 60 + s);
        ts_set1(j6_seq_edit(&S, 1), 0, 70);
        if (i) j6_seq_edit(&S, 1)->phrase = P_STYLE(2, 1);
        ts_play(0);
        ts_run(14);                      /* tick 12 opened step 2 early (24 * 16 - 192) */
        if (i) CHECK_EQ(ts_time(J6_EV_HELD, -1, 1), 24u * 16u - 192u);
        else CHECK_EQ(ts_time(J6_EV_NOTE_ON, 61, 0), 24u * 16u - 192u);
        ts_select(1);
        ts_run_to(50);
        CHECK_EQ(ts_count(J6_EV_PATTERN, -1), 1);
        CHECK_EQ(ts_time(J6_EV_PATTERN, -1, 0), 48u * 16u);
        CHECK_EQ(j6_seq_cur(&S), 1);
        if (i) {
            CHECK_EQ(ts_time(J6_EV_HELD, -1, 2), 48u * 16u);   /* the new pattern's window, nothing between */
            CHECK_EQ(ts_count(J6_EV_NOTE_ON, -1), 0);
        } else {
            CHECK_EQ(ts_time(J6_EV_NOTE_OFF, 61, 0), 24u * 16u - 192u + j6_gate(24) * 16u);
            CHECK_EQ(ts_count(J6_EV_NOTE_ON, 61), 1);
            CHECK_EQ(ts_count(J6_EV_NOTE_ON, 62), 0);
            CHECK_EQ(ts_time(J6_EV_NOTE_ON, 70, 0), 48u * 16u);
        }
        ts_stop();
        CHECK(ts_strict(1));
    }
    /* PLAY from a shifted step with negative shuffle: starts at once */
    ts_begin();
    p = ts_pat();
    p->beat = J6_BEAT_1_4;
    p->shuffle = -50;
    ts_set1(p, 1, 61);
    ts_play(1);
    ts_run(30);
    ts_stop();
    CHECK_EQ(ts_time(J6_EV_NOTE_ON, 61, 0), 0);
    CHECK(ts_strict(1));
}

/* ======================================================================================================
 * Pattern change (SQ-27)
 * ==================================================================================================== */

static void test_pchg_step(void)
{
    j6_pattern_t *a, *b;
    int k;
    ts_begin();
    S.cfg_plain_legato = 1;              /* so a note still sounds at the switch */
    a = j6_seq_edit(&S, 0);
    a->beat = J6_BEAT_1_2;
    a->last = 4;
    ts_set1(a, 0, 60);
    ts_set1(a, 1, 61);
    ts_set1(a, 2, 62);
    ts_set1(a, 3, 63);
    b = j6_seq_edit(&S, 1);
    b->beat = J6_BEAT_1_4;
    b->last = 2;
    ts_set1(b, 0, 70);
    ts_set1(b, 1, 72);
    ts_play(0);
    ts_run(10);
    ts_select(1);
    CHECK_EQ(j6_seq_queued(&S), 1);
    ts_run_to(48);
    CHECK_EQ(j6_seq_cur(&S), 0);
    CHECK_EQ(ts_count(J6_EV_PATTERN, -1), 0);
    ts_run(1);                           /* tick 48: the next step boundary */
    CHECK_EQ(j6_seq_cur(&S), 1);
    CHECK_EQ(j6_seq_queued(&S), J6_NONE);
    CHECK_EQ(ts_time(J6_EV_PATTERN, -1, 0), 48u * 16u);
    CHECK_EQ(ts_arg(J6_EV_PATTERN, 0), 1);
    CHECK_EQ(ts_time(J6_EV_NOTE_OFF, 60, 0), 48u * 16u);   /* the old window ends */
    CHECK_EQ(ts_time(J6_EV_NOTE_ON, 70, 0), 48u * 16u);
    CHECK_EQ(ts_count(J6_EV_NOTE_ON, 61), 0);
    CHECK_EQ(ts_count(J6_EV_LOOP, -1), 0);
    CHECK_EQ(j6_seq_pos(&S), 0);
    CHECK_EQ(j6_seq_step_now(&S), 0);
    k = ts_find(0, J6_EV_PATTERN, -1);
    CHECK(L.e[k + 1].ev.type == J6_EV_STEP && L.e[k + 1].ev.arg == 0);
    CHECK(ts_find(0, J6_EV_NOTE_OFF, 60) < k);   /* the old notes end before the switch */
    /* the new pattern runs with its own step length */
    ts_run_to(49 + 24);
    CHECK_EQ(ts_time(J6_EV_NOTE_ON, 72, 0), 72u * 16u);
    ts_run_to(49 + 48);
    CHECK_EQ(ts_count(J6_EV_LOOP, -1), 1);
    ts_stop();
    CHECK(ts_strict(1));
    CHECK(LOST == 0);
}

static void test_pchg_n(void)
{
    j6_pattern_t *a;
    ts_begin();
    a = j6_seq_edit(&S, 0);
    a->beat = J6_BEAT_1_4;
    a->last = 16;                        /* 384 ticks */
    a->pchg = 2;                         /* every 2 beats = 192 ticks from position 0 */
    ts_set1(j6_seq_edit(&S, 2), 0, 80);
    ts_play(0);
    ts_run(30);
    ts_select(2);
    ts_run_to(192);
    CHECK_EQ(ts_count(J6_EV_PATTERN, -1), 0);
    ts_run(1);
    CHECK_EQ(ts_time(J6_EV_PATTERN, -1, 0), 192u * 16u);
    CHECK_EQ(ts_time(J6_EV_NOTE_ON, 80, 0), 192u * 16u);
    ts_stop();
    /* N beats longer than the rest of the loop: the count restarts at the wrap */
    ts_begin();
    a = j6_seq_edit(&S, 0);
    a->beat = J6_BEAT_1_4;
    a->last = 16;
    a->pchg = 3;                         /* 288 */
    ts_play(0);
    ts_run(300);
    ts_select(2);
    ts_run_to(385);
    CHECK_EQ(ts_time(J6_EV_PATTERN, -1, 0), 384u * 16u);
    ts_stop();
    /* N = 1 inside a long step: the window is cut where the beat falls */
    ts_begin();
    a = j6_seq_edit(&S, 0);
    a->pchg = 1;                         /* bEAt 4: one step = 384 */
    ts_set1(a, 0, 60);
    ts_set1(j6_seq_edit(&S, 2), 0, 80);
    ts_play(0);
    ts_run(10);
    ts_select(2);
    ts_run_to(97);
    CHECK_EQ(ts_time(J6_EV_PATTERN, -1, 0), 96u * 16u);
    CHECK_EQ(ts_time(J6_EV_NOTE_OFF, 60, 0), 96u * 16u);
    CHECK_EQ(ts_time(J6_EV_NOTE_ON, 80, 0), 96u * 16u);
    ts_stop();
    CHECK(ts_strict(1));
    /* the new pattern's own P.chg decides the next change */
    ts_begin();
    j6_seq_edit(&S, 0)->beat = J6_BEAT_1_4;
    j6_seq_edit(&S, 1)->beat = J6_BEAT_1_4;
    j6_seq_edit(&S, 1)->pchg = 4;        /* 384 */
    j6_seq_edit(&S, 1)->last = 16;       /* 384 ticks long */
    ts_play(0);
    ts_run(5);
    ts_select(1);
    ts_run_to(25);
    CHECK_EQ(ts_time(J6_EV_PATTERN, -1, 0), 24u * 16u);
    ts_select(0);
    ts_run_to(24 + 384);
    CHECK_EQ(ts_count(J6_EV_PATTERN, -1), 1);
    ts_run(1);
    CHECK_EQ(ts_time(J6_EV_PATTERN, -1, 1), (24u + 384u) * 16u);
    ts_stop();
    CHECK(LOST == 0);
}

/* ======================================================================================================
 * Stop and balance
 * ==================================================================================================== */

static void test_stop_balanced(void)
{
    j6_pattern_t *p;
    const uint8_t c[4] = {48, 55, 60, 64};
    uint32_t t;
    /* STYLE OFF notes sounding at stop: their NOTE_OFFs come with the stop */
    ts_begin();
    p = ts_pat();
    ts_set(p, 0, c, 4);
    ts_play(0);
    ts_run(100);
    ts_stop();
    CHECK_EQ(ts_count(J6_EV_NOTE_OFF, -1), 4);
    CHECK_EQ(ts_time(J6_EV_NOTE_OFF, 48, 0), 100u * 16u);
    CHECK_EQ(L.e[L.n - 1].ev.type, J6_EV_STOP);
    CHECK(j6t_log_balanced(&L));
    CHECK(ts_strict(1));
    /* a phrase window at stop: HELD (n = 0) */
    ts_begin();
    p = ts_pat();
    p->phrase = P_STYLE(1, 1);
    ts_set(p, 0, c, 4);
    ts_play(0);
    ts_run(100);
    ts_stop();
    CHECK_EQ(ts_count(J6_EV_HELD, -1), 2);
    CHECK_EQ(j6_seq_window(&S)->n, 0);
    /* a parked (positive shuffle) note at stop never starts */
    ts_begin();
    p = ts_pat();
    p->beat = J6_BEAT_1_4;
    p->shuffle = 90;
    ts_set1(p, 1, 61);
    ts_play(0);
    ts_run(30);                          /* step 2 starts at 24 + 21.6 */
    ts_stop();
    ts_run(30);
    CHECK_EQ(ts_count(J6_EV_NOTE_ON, -1), 0);
    CHECK(ts_strict(1));
    /* stop at every tick of a busy pattern */
    for (t = 1; t < 200; t += 7) {
        ts_begin();
        p = ts_pat();
        p->beat = J6_BEAT_1_4;
        p->shuffle = (int8_t)(t & 1 ? 60 : -60);
        ts_set(p, 0, c, 4);
        ts_set(p, 1, c + 1, 3);
        ts_set1(p, 2, 60);
        ts_tie(p, 3);
        ts_set(p, 4, c, 2);
        ts_play(0);
        ts_run(t);
        ts_stop();
        CHECK(ts_strict(1));
    }
    CHECK(LOST == 0);
}

/* ======================================================================================================
 * Real-time recording (SQ-32 .. SQ-36)
 * ==================================================================================================== */

static void ts_rec_start(uint32_t beat)
{
    ts_begin();
    ts_pat()->beat = (uint8_t)beat;
    ts_rec();
    ts_play(0);
    ts_run(1);
}

static void test_recording(void)
{
    j6_pattern_t *p;
    const uint8_t n60 = 60, n62 = 62, quad[4] = {64, 65, 67, 69}, cm[3] = {64, 67, 71};
    const uint8_t cm2[3] = {52, 55, 59};
    uint8_t tag_e = j6_chord_tag(4, 0);
    /* SQ-32 floor: the step playing */
    ts_rec_start(J6_BEAT_1_4);
    p = ts_pat();
    j6_seq_set_saved(&S, 0);
    ts_run_to(31);                       /* pos 30: step 2 (index 1) */
    j6_seq_rec_input(&S, &n60, 1, J6_NO_CHORD, 0);
    CHECK_NOTES(&p->step[1], 60);
    CHECK_EQ(j6_seq_dirty(&S, 0), 1);
    ts_run_to(48);                       /* pos 47: still step index 1 with floor */
    j6_seq_rec_input(&S, &n62, 1, J6_NO_CHORD, 0);
    CHECK_NOTES(&p->step[1], 60, 62);
    /* the seq_rt_record probe: 0.49 s at 120 BPM = 94 ticks: floor -> step index 3 */
    ts_run_to(95);
    j6_seq_rec_input(&S, &n60, 1, J6_NO_CHORD, 0);
    CHECK_NOTES(&p->step[3], 60);
    CHECK_EMPTY(&p->step[4]);
    /* J6_CFG_REC_NEAREST: index 4; and the end of the loop goes to step 1 */
    S.cfg_rec_nearest = 1;
    j6_seq_rec_input(&S, &n62, 1, J6_NO_CHORD, 0);
    CHECK_NOTES(&p->step[4], 62);
    ts_run_to(192);                      /* pos 191: nearest is the next loop's step 1 */
    j6_seq_rec_input(&S, &n62, 1, J6_NO_CHORD, 0);
    CHECK_NOTES(&p->step[0], 62);
    CHECK_EMPTY(&p->step[7]);
    ts_run_to(192 + 60);                 /* pos 59: 59 + 12 = 71 -> index 2 */
    j6_seq_rec_input(&S, &n62, 1, J6_NO_CHORD, 0);
    CHECK_NOTES(&p->step[2], 62);
    S.cfg_rec_nearest = 0;
    ts_stop();

    /* SQ-33: a chord replaces; the same chord again does not delete it */
    ts_rec_start(J6_BEAT_1_4);
    p = ts_pat();
    ts_set1(p, 0, 48);
    j6_step_key_note(p, 0, 50);
    j6_seq_rec_input(&S, cm, 3, tag_e, 0);
    CHECK_NOTES(&p->step[0], 64, 67, 71);
    CHECK_EQ(p->step[0].chord, tag_e);
    CHECK_EQ(p->step[0].flags, 0x70);
    j6_seq_rec_input(&S, cm, 3, tag_e, 0);
    CHECK_NOTES(&p->step[0], 64, 67, 71);
    CHECK_EQ(p->step[0].chord, tag_e);
    j6_seq_rec_input(&S, cm2, 3, tag_e, 0);   /* same tag, other notes (KEY moved): replaced */
    CHECK_NOTES(&p->step[0], 52, 55, 59);
    /* single notes add up to 4 and never delete */
    ts_run_to(25);
    j6_seq_rec_input(&S, &n60, 1, J6_NO_CHORD, 0);
    j6_seq_rec_input(&S, quad, 4, J6_NO_CHORD, 0);
    CHECK_NOTES(&p->step[1], 60, 64, 65, 67);
    j6_seq_rec_input(&S, &n60, 1, J6_NO_CHORD, 0);
    CHECK_NOTES(&p->step[1], 60, 64, 65, 67);
    /* a single note on a chord step adds to it */
    ts_run_to(192 + 1);                  /* step 1 of the next loop */
    j6_seq_rec_input(&S, &n62, 1, J6_NO_CHORD, 0);
    CHECK_NOTES(&p->step[0], 52, 55, 59, 62);
    CHECK_EQ(p->step[0].chord, tag_e);
    /* SQ-34: STYLE ON writes the step phrase; without bit 7 the phrase stays */
    ts_run_to(192 + 49);
    j6_seq_rec_input(&S, &n60, 1, J6_NO_CHORD, P_STYLE(2, 5));
    CHECK_EQ(p->step[2].phrase, P_STYLE(2, 5));
    j6_seq_rec_input(&S, &n62, 1, J6_NO_CHORD, j6_phr_pack(0, 7, 7));
    CHECK_EQ(p->step[2].phrase, P_STYLE(2, 5));
    CHECK_NOTES(&p->step[2], 60, 62);
    ts_stop();

    /* SQ-33 ties while held; they stop at a step with notes and never wrap */
    ts_rec_start(J6_BEAT_1_4);
    p = ts_pat();
    ts_set1(p, 4, 70);
    ts_run_to(30);                       /* step index 1 */
    j6_seq_rec_input(&S, &n60, 1, J6_NO_CHORD, 0);
    j6_seq_rec_held(&S, 1);
    ts_run_to(24 * 6 + 1);
    CHECK(p->step[2].flags & J6_STF_TIE);
    CHECK(p->step[3].flags & J6_STF_TIE);
    CHECK_EQ(p->step[4].flags & J6_STF_TIE, 0);
    CHECK_NOTES(&p->step[4], 70);
    CHECK_EQ(p->step[5].flags & J6_STF_TIE, 0);
    CHECK_EQ(p->step[6].flags & J6_STF_TIE, 0);
    j6_seq_rec_held(&S, 0);
    /* the last step held across the wrap */
    ts_run_to(192 - 10);                 /* step index 7 */
    j6_seq_rec_input(&S, &n62, 1, J6_NO_CHORD, 0);
    j6_seq_rec_held(&S, 1);
    ts_run_to(192 + 30);
    CHECK_EQ(p->step[0].flags & J6_STF_TIE, 0);
    CHECK_EQ(p->step[1].flags & J6_STF_TIE, 0);
    j6_seq_rec_held(&S, 0);
    /* released: no more ties */
    ts_run_to(192 + 24 * 5 + 5);         /* step index 5 */
    j6_seq_rec_input(&S, &n62, 1, J6_NO_CHORD, 0);
    ts_run_to(192 + 24 * 7 + 5);
    CHECK_EQ(p->step[6].flags & J6_STF_TIE, 0);
    ts_stop();
    CHECK(ts_strict(1));

    /* SQ-36 overdub: the recorded notes play in the next loop and more can be added */
    ts_rec_start(J6_BEAT_1_4);
    p = ts_pat();
    ts_run_to(26);
    j6_seq_rec_input(&S, &n60, 1, J6_NO_CHORD, 0);
    CHECK_EQ(ts_count(J6_EV_NOTE_ON, 60), 0);   /* the glue plays the key live, the seq does not */
    ts_run_to(192 + 25);
    CHECK_EQ(ts_count(J6_EV_NOTE_ON, 60), 1);
    CHECK_EQ(ts_time(J6_EV_NOTE_ON, 60, 0), (192u + 24u) * 16u);
    j6_seq_rec_input(&S, &n62, 1, J6_NO_CHORD, 0);
    CHECK_NOTES(&p->step[1], 60, 62);
    ts_stop();
    CHECK(ts_strict(1));

    /* not recording: ignored */
    ts_begin();
    p = ts_pat();
    j6_seq_rec_input(&S, &n60, 1, J6_NO_CHORD, 0);
    ts_play(0);
    ts_run(30);
    j6_seq_rec_input(&S, &n60, 1, J6_NO_CHORD, 0);
    CHECK_EMPTY(&p->step[0]);
    /* punch in, then out */
    ts_rec();
    j6_seq_rec_input(&S, &n60, 1, J6_NO_CHORD, 0);
    CHECK_NOTES(&p->step[0], 60);
    ts_rec();
    j6_seq_rec_input(&S, &n62, 1, J6_NO_CHORD, 0);
    CHECK_NOTES(&p->step[0], 60);
    ts_stop();
    /* count-in: nothing is written before recording starts */
    ts_begin();
    p = ts_pat();
    j6_seq_set_countin(&S, 2);
    ts_rec();
    ts_play(0);
    ts_run(50);
    j6_seq_rec_input(&S, &n60, 1, J6_NO_CHORD, 0);
    CHECK_EMPTY(&p->step[0]);
    ts_run_to(193);
    j6_seq_rec_input(&S, &n60, 1, J6_NO_CHORD, 0);
    CHECK_NOTES(&p->step[0], 60);
    j6_seq_rec_input(&S, NULL, 1, J6_NO_CHORD, 0);
    j6_seq_rec_input(&S, &n62, 0, J6_NO_CHORD, 0);
    CHECK_NOTES(&p->step[0], 60);
    ts_stop();
    CHECK(LOST == 0);
}

/* SQ-33 across loops: a key held again over ties written in an earlier loop continues the chain through
 * them and ties the empty steps after them; re-recording what is there already leaves the pattern saved */
static void test_rec_overdub_ties(void)
{
    j6_pattern_t *p;
    const uint8_t n60 = 60, c_maj[3] = {60, 64, 67};
    uint8_t tag_c = j6_chord_tag(0, 0);
    ts_rec_start(J6_BEAT_1_4);           /* 8 steps of 24 ticks: one loop = 192 ticks */
    p = ts_pat();
    /* loop 1: C held from step 2 into step 5 */
    ts_run_to(30);                       /* pos 29: step index 1 */
    j6_seq_rec_input(&S, &n60, 1, J6_NO_CHORD, 0);
    j6_seq_rec_held(&S, 1);
    ts_run_to(24 * 4 + 1);               /* boundaries 2, 3, 4 */
    j6_seq_rec_held(&S, 0);
    CHECK_NOTES(&p->step[1], 60);
    CHECK(p->step[2].flags & J6_STF_TIE);
    CHECK(p->step[3].flags & J6_STF_TIE);
    CHECK(p->step[4].flags & J6_STF_TIE);
    CHECK_EQ(p->step[5].flags & J6_STF_TIE, 0);
    /* loop 2: the same note again, nothing new: the pattern stays saved */
    j6_seq_set_saved(&S, 0);
    ts_run_to(192 + 30);
    j6_seq_rec_input(&S, &n60, 1, J6_NO_CHORD, 0);
    CHECK_EQ(j6_seq_dirty(&S, 0), 0);
    /* ... held longer this time: through the old ties, then steps 6 and 7 get ties */
    j6_seq_rec_held(&S, 1);
    ts_run_to(192 + 24 * 6 + 1);
    j6_seq_rec_held(&S, 0);
    CHECK(p->step[2].flags & J6_STF_TIE);
    CHECK(p->step[4].flags & J6_STF_TIE);
    CHECK(p->step[5].flags & J6_STF_TIE);
    CHECK(p->step[6].flags & J6_STF_TIE);
    CHECK_EQ(p->step[7].flags & J6_STF_TIE, 0);
    CHECK_EQ(j6_seq_dirty(&S, 0), 1);
    CHECK(ts_pattern_valid(p));
    /* the window of step 2 now lasts 6 steps in the next loop: one NOTE_ON, gate g(144) */
    ts_run_to(3 * 192 + 1);
    {
        uint32_t on = ts_time(J6_EV_NOTE_ON, 60, 1);
        CHECK_EQ(on, (2u * 192u + 24u) * 16u);
        CHECK_EQ(ts_time(J6_EV_NOTE_OFF, 60, 1), on + j6_gate(144) * 16u);
    }
    ts_stop();
    CHECK(ts_strict(1));
    /* a chord recorded again over the same chord: unchanged, still saved */
    ts_rec_start(J6_BEAT_1_4);
    p = ts_pat();
    j6_seq_rec_input(&S, c_maj, 3, tag_c, 0);
    CHECK_NOTES(&p->step[0], 60, 64, 67);
    j6_seq_set_saved(&S, 0);
    j6_seq_rec_input(&S, c_maj, 3, tag_c, 0);
    CHECK_NOTES(&p->step[0], 60, 64, 67);
    CHECK_EQ(p->step[0].chord, tag_c);
    CHECK_EQ(j6_seq_dirty(&S, 0), 0);
    /* SQ-34 with a phrase byte whose code is out of range: STYLE on, style 1 var 1 */
    j6_seq_rec_input(&S, c_maj, 3, tag_c, 0xFFu);
    CHECK_EQ(p->step[0].phrase, J6_PHR_ON);
    CHECK_EQ(j6_seq_dirty(&S, 0), 1);
    CHECK(ts_step_valid(&p->step[0]));
    ts_stop();
    CHECK(LOST == 0);
}

/* SQ-32: the step a key goes into is the step playing (its button blinks: j6_seq_step_now()), at every
 * tick and every bEAt; with J6_CFG_REC_NEAREST the nearest step start, step 1 after the last half step */
static void test_rec_step_sweep(void)
{
    static j6_pattern_t snap;
    uint32_t b, t, k, len, sl, bad = 0, bad_near = 0;
    const uint8_t n = 77;
    for (b = 0; b < J6_BEAT_COUNT; b++) {
        ts_begin();
        ts_pat()->beat = (uint8_t)b;
        ts_pat()->last = 3;
        sl = j6_beat_ticks(b);
        len = 3 * sl;
        ts_rec();
        ts_play(0);
        for (t = 0; t < len; t++) {
            uint32_t want_near = (t + sl / 2u) / sl;
            int found = -1;
            ts_run(1);
            if (want_near >= 3) want_near = 0;
            snap = *j6_seq_pat(&S, 0);
            S.cfg_rec_nearest = (uint8_t)(t & 1);
            j6_seq_rec_input(&S, &n, 1, J6_NO_CHORD, 0);
            for (k = 0; k < 3; k++)
                if (j6_step_find(&j6_seq_pat(&S, 0)->step[k], n) >= 0) found = (int)k;
            if (t & 1) {
                if (found != (int)want_near) bad_near++;
            } else {
                if (found != (int)j6_seq_step_now(&S) || found != (int)(t / sl)) bad++;
            }
            *j6_seq_edit(&S, 0) = snap;      /* undo, so the sweep never fills a step */
        }
        ts_stop();
    }
    CHECK_EQ(bad, 0);
    CHECK_EQ(bad_near, 0);
}

/* SQ-31: one count-in beat is a quarter note (96 ticks) whatever the step length; 2..4 beats */
static void test_countin_beats(void)
{
    static const uint8_t beats[3] = {J6_BEAT_1_4, J6_BEAT_1_3, J6_BEAT_8};
    uint32_t i, n, k;
    for (i = 0; i < 3; i++)
        for (n = 2; n <= 4; n++) {
            ts_begin();
            ts_pat()->beat = beats[i];
            ts_set1(ts_pat(), 0, 60);
            j6_seq_set_countin(&S, n);
            ts_rec();
            ts_play(0);
            ts_run(n * 96u);
            CHECK_EQ(ts_count(J6_EV_COUNTIN, -1), n);
            for (k = 0; k < n; k++) {
                CHECK_EQ(ts_time(J6_EV_COUNTIN, -1, k), k * 96u * 16u);
                CHECK_EQ(ts_arg(J6_EV_COUNTIN, k), (int)(n - k));
            }
            CHECK_EQ(ts_count(J6_EV_START, -1), 0);
            CHECK_EQ(j6_seq_recording(&S), 0);
            ts_run(1);
            CHECK_EQ(ts_time(J6_EV_START, -1, 0), n * 96u * 16u);
            CHECK_EQ(ts_time(J6_EV_NOTE_ON, 60, 0), n * 96u * 16u);
            CHECK_EQ(j6_seq_recording(&S), 1);
            ts_stop();
            CHECK(ts_strict(1));
        }
    CHECK(LOST == 0);
}

/* SQ-35: PLAY during recording is turned into a stop by the glue; the sequencer ignores a second PLAY,
 * and stop ends recording and playback together */
static void test_punch_play(void)
{
    ts_begin();
    ts_set1(ts_pat(), 0, 60);
    ts_play(0);
    ts_run(10);
    ts_rec();                            /* punch in during plain playback */
    CHECK_EQ(j6_seq_recording(&S), 1);
    ts_play(0);
    ts_play(4);
    CHECK_EQ(j6_seq_state(&S), J6_SEQ_PLAYING);
    CHECK_EQ(j6_seq_recording(&S), 1);
    ts_run(10);
    CHECK_EQ(ts_count(J6_EV_START, -1), 1);
    CHECK_EQ(j6_seq_pos(&S), 19);        /* playback went on from where it was */
    ts_stop();
    CHECK_EQ(j6_seq_recording(&S), 0);
    CHECK_EQ(j6_seq_state(&S), J6_SEQ_STOPPED);
    CHECK_EQ(L.e[L.n - 2].ev.type, J6_EV_REC);
    CHECK_EQ(L.e[L.n - 2].ev.arg, 0);
    CHECK_EQ(L.e[L.n - 1].ev.type, J6_EV_STOP);
    CHECK(ts_strict(1));
    /* the next PLAY is plain playback again (recording does not resume) */
    ts_play(0);
    ts_run(1);
    CHECK_EQ(j6_seq_recording(&S), 0);
    ts_stop();
}

/* SQ-1: 64 patterns, each its own working copy; any of them can be selected and played */
static void test_64_patterns(void)
{
    uint32_t k, bad = 0;
    ts_begin();
    for (k = 0; k < J6_PATTERNS; k++) {
        ts_set1(j6_seq_edit(&S, k), 0, 20 + k);
        j6_seq_set_saved(&S, k);
    }
    for (k = 0; k < J6_PATTERNS; k++) {
        uint32_t i0 = L.n;
        ts_select(k);
        if (L.n != i0 + 1 || L.e[i0].ev.type != J6_EV_PATTERN || L.e[i0].ev.arg != k) bad++;
        if (j6_seq_cur(&S) != k) bad++;
        ts_play(0);
        ts_run(1);
        if (ts_find(i0, J6_EV_NOTE_ON, (int)(20 + k)) < 0) bad++;
        ts_stop();
        if (j6_seq_dirty(&S, k)) bad++;  /* selecting and playing never marks a pattern unsaved */
    }
    CHECK_EQ(bad, 0);
    CHECK_EQ(ts_count(J6_EV_NOTE_ON, -1), 64);
    CHECK(ts_strict(1));
}

/* SQ-28, PS-2: switching patterns (stopped or queued while playing) keeps the unsaved edits of every
 * pattern in RAM, with their dirty bits */
static void test_switch_keeps_edits(void)
{
    uint32_t k, any = 0;
    ts_begin();
    j6_seq_edit(&S, 0)->beat = J6_BEAT_1_4;
    ts_set1(j6_seq_edit(&S, 0), 0, 60);
    ts_select(1);
    ts_set1(j6_seq_edit(&S, 1), 0, 62);
    j6_seq_edit(&S, 1)->tempo10 = 900;
    ts_select(0);
    CHECK_NOTES(&j6_seq_pat(&S, 0)->step[0], 60);
    CHECK_NOTES(&j6_seq_pat(&S, 1)->step[0], 62);
    CHECK_EQ(j6_seq_pat(&S, 1)->tempo10, 900);   /* the tempo belongs to the pattern (SQ-6) */
    CHECK_EQ(j6_seq_pat(&S, 0)->tempo10, 1200);
    CHECK_EQ(j6_seq_dirty(&S, 0), 1);
    CHECK_EQ(j6_seq_dirty(&S, 1), 1);
    for (k = 2; k < J6_PATTERNS; k++) any |= j6_seq_dirty(&S, k);
    CHECK_EQ(any, 0);
    /* queued while playing: the switch plays pattern 1's edit, pattern 0's stays */
    ts_play(0);
    ts_run(5);
    ts_select(1);
    ts_run_to(25);
    CHECK_EQ(j6_seq_cur(&S), 1);
    CHECK_EQ(ts_count(J6_EV_NOTE_ON, 62), 1);
    ts_stop();
    ts_select(0);
    CHECK_NOTES(&j6_seq_pat(&S, 0)->step[0], 60);
    CHECK_EQ(j6_seq_dirty(&S, 0), 1);
    CHECK_EQ(j6_seq_dirty(&S, 1), 1);
    CHECK(ts_strict(1));
}

/* SQ-27: the new pattern starts at its step 1 with its own settings: its bEAt, its pattern phrase
 * (SQ-26 rule 3, whatever phrase the old pattern's flagged steps had set) and its shuffle */
static void test_pchg_settings(void)
{
    j6_pattern_t *a, *b;
    const j6_window_t *w;
    ts_begin();
    a = j6_seq_edit(&S, 0);
    a->beat = J6_BEAT_1_4;
    a->last = 4;
    ts_set1(a, 0, 60);
    a->step[0].phrase = P_STYLE(2, 1);   /* cur = 2-1 from step 1 on */
    ts_set1(a, 1, 61);
    b = j6_seq_edit(&S, 1);
    b->beat = J6_BEAT_1_2;
    b->last = 2;
    b->phrase = P_STYLE(3, 4);
    b->shuffle = 90;                     /* never matters at 1-2 steps */
    ts_set1(b, 0, 70);
    ts_set1(b, 1, 71);
    ts_play(0);
    ts_run(1);
    CHECK_EQ(j6_seq_window(&S)->phrase, P_STYLE(2, 1));
    ts_run(5);
    ts_select(1);
    ts_run_to(25);                       /* tick 24: the switch */
    w = j6_seq_window(&S);
    CHECK_EQ(j6_seq_cur(&S), 1);
    CHECK_EQ(j6_seq_pos(&S), 0);
    CHECK_EQ(j6_seq_step_now(&S), 0);
    CHECK_EQ(w->n, 1);
    CHECK_EQ(w->note[0], 70);
    CHECK_EQ(w->phrase, P_STYLE(3, 4));
    CHECK_EQ(w->len, 48);
    CHECK_EQ(ts_time(J6_EV_HELD, -1, 1), 24u * 16u);
    ts_run_to(24 + 48 + 1);              /* the new pattern's step 2, 48 ticks later */
    CHECK_EQ(j6_seq_window(&S)->note[0], 71);
    CHECK_EQ(j6_seq_window(&S)->phrase, P_STYLE(3, 4));
    CHECK_EQ(ts_time(J6_EV_HELD, -1, 2), (24u + 48u) * 16u);
    CHECK_EQ(ts_time(J6_EV_STEP, -1, 2), (24u + 48u) * 16u);
    CHECK_EQ(ts_count(J6_EV_NOTE_ON, -1), 0);
    ts_stop();
    CHECK_EQ(j6_seq_window(&S)->n, 0);
    CHECK(LOST == 0);
}

/* PS-1, PS-2: the working copies against the saved ones. WRITE ALL / WRITE Ptn encode and mark saved,
 * boot decodes into clean working copies, an unsaved edit is lost at power-off (the glue reloads) */
static void test_save_load(void)
{
    static j6_seq_t S2;
    static uint8_t flash[J6_PATTERNS][J6_PAT_BYTES];
    uint32_t k, bad = 0;
    ts_begin();
    RNG = 31337u;
    for (k = 0; k < J6_PATTERNS; k++) ts_random_pattern(j6_seq_edit(&S, k));
    for (k = 0; k < J6_PATTERNS; k++) bad += j6_seq_dirty(&S, k) ? 0u : 1u;
    CHECK_EQ(bad, 0);
    /* WRITE ALL: every unsaved pattern */
    for (k = 0; k < J6_PATTERNS; k++)
        if (j6_seq_dirty(&S, k)) {
            CHECK_EQ(j6_pattern_encode(j6_seq_pat(&S, k), flash[k]), J6_PAT_BYTES);
            j6_seq_set_saved(&S, k);
        }
    for (k = 0; k < J6_PATTERNS; k++) bad += j6_seq_dirty(&S, k);
    CHECK_EQ(bad, 0);
    /* boot: a second instance loads them all, clean */
    j6_seq_init(&S2);
    for (k = 0; k < J6_PATTERNS; k++) {
        if (j6_pattern_decode(j6_seq_edit(&S2, k), flash[k]) != 0) bad++;
        j6_seq_set_saved(&S2, k);
        if (memcmp(j6_seq_pat(&S2, k), j6_seq_pat(&S, k), sizeof(j6_pattern_t)) != 0) bad++;
        if (j6_seq_dirty(&S2, k)) bad++;
    }
    CHECK_EQ(bad, 0);
    /* WRITE Ptn: only the current pattern is saved; another edited one stays unsaved */
    ts_set1(j6_seq_edit(&S2, 3), 0, 99);
    ts_set1(j6_seq_edit(&S2, 5), 0, 98);
    j6_pattern_encode(j6_seq_pat(&S2, 3), flash[3]);
    j6_seq_set_saved(&S2, 3);
    CHECK_EQ(j6_seq_dirty(&S2, 3), 0);
    CHECK_EQ(j6_seq_dirty(&S2, 5), 1);
    /* power-off: the unsaved edit of pattern 5 returns to its saved state, pattern 3 keeps its edit */
    j6_seq_init(&S2);
    for (k = 0; k < J6_PATTERNS; k++) {
        j6_pattern_decode(j6_seq_edit(&S2, k), flash[k]);
        j6_seq_set_saved(&S2, k);
    }
    CHECK_NOTES(&j6_seq_pat(&S2, 3)->step[0], 99);
    CHECK_MEM(j6_seq_pat(&S2, 5), j6_seq_pat(&S, 5), sizeof(j6_pattern_t));
    /* an erased (or foreign) sector loads as an init pattern */
    memset(flash[7], 0xFF, J6_PAT_BYTES);
    CHECK_EQ(j6_pattern_decode(j6_seq_edit(&S2, 7), flash[7]), -1);
    {
        j6_pattern_t ref;
        j6_pattern_init(&ref);
        CHECK_MEM(j6_seq_pat(&S2, 7), &ref, sizeof ref);
    }
    /* SQ-40 [ASSUMED]: the step clipboard does not survive a power cycle (init) */
    j6_seq_step_copy(&S2, 0);
    CHECK_EQ(j6_seq_step_paste(&S2, 1), 0);
    j6_seq_init(&S2);
    CHECK_EQ(j6_seq_step_paste(&S2, 1), -1);
}

/* ======================================================================================================
 * Randomised stress: edits, selects, stops while playing; nothing stuck, nothing lost
 * ==================================================================================================== */

static void test_stress(void)
{
    uint32_t round, t, k, s;
    RNG = 99u;
    for (round = 0; round < 60; round++) {
        ts_begin();
        S.cfg_plain_legato = (uint8_t)(round & 1);
        S.cfg_wrap_keeps = (uint8_t)((round >> 1) & 1);
        S.cfg_rec_nearest = (uint8_t)((round >> 2) & 1);
        for (k = 0; k < 4; k++) {
            j6_pattern_t *p = j6_seq_edit(&S, k);
            ts_random_pattern(p);
            p->last = (uint8_t)(1 + ts_rn(12));
            p->beat = (uint8_t)ts_rn(4);
        }
        ts_play(ts_rn(8));
        for (t = 0; t < 3000; t++) {
            uint32_t r = ts_rn(1000);
            if (r < 4) ts_select(ts_rn(4));
            else if (r < 6) ts_rec();
            else if (r < 7) {
                ts_stop();
                ts_play(ts_rn(8));
            } else if (r < 12) {
                uint8_t n[4];
                uint32_t cnt = 1 + ts_rn(4);
                for (s = 0; s < cnt; s++) n[s] = (uint8_t)(40 + ts_rn(40));
                j6_seq_rec_input(&S, n, cnt, ts_rn(2) ? J6_NO_CHORD : j6_chord_tag(ts_rn(12), 0),
                                 ts_rn(2) ? P_STYLE(1 + ts_rn(9), 1 + ts_rn(12)) : 0);
            } else if (r < 14) {
                j6_seq_rec_held(&S, ts_rn(2));
            } else if (r < 18) {
                j6_pattern_t *p = ts_pat();     /* edits under the playhead */
                switch (ts_rn(6)) {
                case 0: j6_step_key_note(p, ts_rn(16), 40 + ts_rn(40)); break;
                case 1: { uint8_t g; j6_step_tie(p, ts_rn(16), &g); } break;
                case 2: p->shuffle = (int8_t)((int)ts_rn(181) - 90); break;
                case 3: p->last = (uint8_t)(1 + ts_rn(16)); break;
                case 4: p->phrase = j6_phr_pack(ts_rn(2), 1 + ts_rn(9), 1 + ts_rn(12)); break;
                default: p->beat = (uint8_t)ts_rn(J6_BEAT_COUNT); break;
                }
            }
            ts_run(1);
            if (L.n > J6T_LOG_MAX - 200) {
                /* keep the log from overflowing: check and restart it while nothing sounds */
                ts_stop();
                if (!ts_strict(1)) {
                    CHECK(0);
                    break;
                }
                j6t_log_clear(&L);
                ts_play(0);
            }
        }
        ts_stop();
        CHECK(ts_strict(1));
        CHECK(L.overflow == 0);
        CHECK_EQ(j6_seq_window(&S)->n, 0);
        for (k = 0; k < 4; k++) CHECK(ts_pattern_valid(j6_seq_pat(&S, k)));
    }
    CHECK_EQ(LOST, 0);
    CHECK_RANGE(MAXQ, 1, 24);            /* the design's per-call bound */
}

/* ======================================================================================================
 * Clock (design 2.2, C-5, MI-8)
 * ==================================================================================================== */

static void test_clock_internal(void)
{
    j6_clock_t c;
    uint32_t i, n = 0, f;
    j6_clock_init(&c, 1200);
    CHECK_EQ(j6_clock_spt_q12(&c), 940800);
    CHECK_EQ(j6_clock_tempo10(&c), 1200);
    CHECK_EQ(j6_clock_ticks(&c), 0);
    CHECK_EQ(j6_clock_sub(&c), 0);
    /* exactly 192 ticks in 44100 frames at 120.0 BPM, in J6_CR chunks */
    for (i = 0; i < 44100 / J6_CR; i++) n += j6_clock_advance(&c, J6_CR);
    n += j6_clock_advance(&c, 44100 % J6_CR);
    CHECK_EQ(n, 192);
    CHECK_EQ(j6_clock_ticks(&c), 192);
    CHECK_EQ(c.acc_q12, 0);
    /* and in one call, and over a minute */
    j6_clock_init(&c, 1200);
    CHECK_EQ(j6_clock_advance(&c, 44100), 192);
    for (i = 0, n = 0; i < 60; i++) n += j6_clock_advance(&c, 44100);
    CHECK_EQ(n, 192 * 60);
    /* at most one tick per J6_CR chunk even at 300 BPM; 480 ticks per second there, 64 at 40 */
    j6_clock_init(&c, 3000);
    for (i = 0, n = 0; i < 44100 / J6_CR; i++) {
        uint32_t k = j6_clock_advance(&c, J6_CR);
        CHECK(k <= 1);
        n += k;
    }
    n += j6_clock_advance(&c, 44100 % J6_CR);
    CHECK_EQ(n, 480);
    j6_clock_init(&c, 400);
    CHECK_EQ(j6_clock_advance(&c, 44100), 64);
    /* out-of-range tempos clamp (SQ-6) */
    j6_clock_init(&c, 10);
    CHECK_EQ(j6_clock_tempo10(&c), J6_TEMPO10_MIN);
    j6_clock_init(&c, 9999);
    CHECK_EQ(j6_clock_tempo10(&c), J6_TEMPO10_MAX);
    /* tick starts, frame by frame: tick k begins in the frame where k * 229.6875 is crossed */
    j6_clock_init(&c, 1200);
    for (f = 1, n = 0; f <= 1000; f++)
        if (j6_clock_advance(&c, 1)) {
            n++;
            CHECK_EQ(f, (n * 940800u + 4095u) / 4096u);   /* ceil(n * 229.6875) */
        }
    CHECK_EQ(n, 4);
    /* sub: 115 frames into a tick of 229.6875 is 8/16 */
    j6_clock_init(&c, 1200);
    j6_clock_advance(&c, 115);
    CHECK_EQ(j6_clock_sub(&c), 8);
    j6_clock_advance(&c, 114);
    CHECK_EQ(j6_clock_sub(&c), 15);
    CHECK_EQ(j6_clock_advance(&c, 1), 1);
    CHECK_EQ(j6_clock_sub(&c), 0);
}

static void test_clock_tempo_change(void)
{
    j6_clock_t c;
    uint32_t sub;
    j6_clock_init(&c, 1200);
    j6_clock_advance(&c, 44100);
    j6_clock_advance(&c, 115);
    sub = j6_clock_sub(&c);
    CHECK_EQ(sub, 8);
    j6_clock_set_tempo(&c, 600);         /* half the speed, the same phase */
    CHECK_EQ(j6_clock_sub(&c), sub);
    CHECK_EQ(j6_clock_ticks(&c), 192);
    CHECK_EQ(j6_clock_spt_q12(&c), 1881600);
    CHECK_EQ(j6_clock_tempo10(&c), 600);
    /* half a tick of 459.375 frames is left */
    CHECK_EQ(j6_clock_advance(&c, 228), 0);
    CHECK_EQ(j6_clock_advance(&c, 2), 1);
    CHECK_EQ(j6_clock_advance(&c, 458), 0);
    CHECK_EQ(j6_clock_advance(&c, 2), 1);
    /* to 300 BPM in the middle of a tick */
    j6_clock_advance(&c, 459 / 4);
    sub = j6_clock_sub(&c);
    j6_clock_set_tempo(&c, 3000);
    CHECK_RANGE(j6_clock_sub(&c), sub > 0 ? sub - 1 : 0, sub);
    CHECK_EQ(j6_clock_advance(&c, 70), 1);   /* 3/4 of 91.875 frames left */
    /* the same tempo again changes nothing */
    j6_clock_advance(&c, 40);
    sub = c.acc_q12;
    j6_clock_set_tempo(&c, 3000);
    CHECK_RANGE(c.acc_q12, sub - 2, sub);
}

static void test_clock_external(void)
{
    j6_clock_t c;
    uint32_t p, f, n, k, first[4], bad = 0;
    j6_clock_init(&c, 1200);
    CHECK_EQ(j6_clock_ext_alive(&c), 0);
    j6_clock_ext(&c, 1);
    /* no pulse: no ticks */
    CHECK_EQ(j6_clock_advance(&c, 10000), 0);
    /* pulses every 928 frames (118.8 BPM): 4 ticks each, spread over the interval */
    for (p = 0; p < 20; p++) {
        j6_clock_ext_pulse(&c);
        n = 0;
        for (f = 0; f < 928; f += J6_CR) {
            k = j6_clock_advance(&c, J6_CR);
            if (k > 1) bad++;
            if (k && n < 4) first[n] = f;
            n += k;
            if (j6_clock_sub(&c) > 15) bad++;
        }
        if (n != 4) bad++;
        if (p >= 1) {                    /* every interval measured is 928 */
            if (first[0] != 0) bad++;
            if (first[1] < 224 || first[1] > 240) bad++;
            if (first[2] < 448 || first[2] > 464) bad++;
            if (first[3] < 672 || first[3] > 688) bad++;
        }
    }
    CHECK_EQ(bad, 0);
    CHECK_EQ(j6_clock_ticks(&c), 80);
    CHECK_EQ(j6_clock_spt_q12(&c), 928u * 1024u);
    CHECK_EQ(j6_clock_tempo10(&c), (1128960000u + 928u * 512u) / (928u * 1024u));
    CHECK_RANGE(j6_clock_tempo10(&c), 1187, 1189);
    CHECK_EQ(j6_clock_ext_alive(&c), 1);
    /* after the 4 ticks the clock waits for the next pulse */
    CHECK_EQ(j6_clock_advance(&c, 5000), 0);
    CHECK_EQ(j6_clock_sub(&c), 15);
    /* catch-up: the tempo doubles; a pulse before the spread ticks are out releases them at once */
    for (p = 0; p < 12; p++) {
        uint32_t before = j6_clock_ticks(&c);
        j6_clock_ext_pulse(&c);
        k = j6_clock_advance(&c, J6_CR);
        /* every tick of the previous pulse is out now, and this pulse's first */
        if (j6_clock_ticks(&c) != 80u + 4u * p + 1u) bad++;
        if (p > 0 && k < 1) bad++;
        for (f = J6_CR; f < 460; f += J6_CR) j6_clock_advance(&c, J6_CR);
        if (j6_clock_ticks(&c) - before > 7) bad++;
    }
    CHECK_EQ(bad, 0);
    j6_clock_advance(&c, 5000);
    CHECK_EQ(j6_clock_ticks(&c), 80 + 4 * 12);   /* never drifts from 4 per pulse */
    /* several pulses between two advances (the glue drains MIDI once per block) */
    j6_clock_ext_pulse(&c);
    j6_clock_ext_pulse(&c);
    j6_clock_ext_pulse(&c);
    CHECK_EQ(j6_clock_advance(&c, J6_CR), 4 + 4 + 1);
    j6_clock_advance(&c, 20000);
    CHECK_EQ(j6_clock_ticks(&c), 80 + 48 + 12);
    CHECK(j6_clock_spt_q12(&c) >= 4096);
    /* MI-8 timeout: 500 ms after the last pulse */
    j6_clock_init(&c, 1200);
    j6_clock_ext(&c, 1);
    j6_clock_ext_pulse(&c);
    CHECK_EQ(j6_clock_ext_alive(&c), 1);
    j6_clock_advance(&c, 22049);
    CHECK_EQ(j6_clock_ext_alive(&c), 1);
    j6_clock_advance(&c, 1);
    CHECK_EQ(j6_clock_ext_alive(&c), 0);
    /* the gap is not averaged: the tick length stays the internal one */
    j6_clock_ext_pulse(&c);
    CHECK_EQ(j6_clock_spt_q12(&c), 940800);
    CHECK_EQ(j6_clock_tempo10(&c), 1200);
    /* back to the internal clock (the glue does this after the timeout) */
    j6_clock_ext(&c, 0);
    n = j6_clock_ticks(&c);
    CHECK_EQ(j6_clock_advance(&c, 44100), 192);
    CHECK_EQ(j6_clock_ticks(&c) - n, 192);
    /* pulses while internal: measured and reported, but they do not tick */
    j6_clock_init(&c, 1200);
    for (p = 0; p < 10; p++) {
        j6_clock_ext_pulse(&c);
        j6_clock_advance(&c, 460);
    }
    CHECK_EQ(j6_clock_ticks(&c), 4600u * 4096u / 940800u);
    CHECK_EQ(j6_clock_ext_alive(&c), 1);
    CHECK_EQ(j6_clock_tempo10(&c), 1200);        /* internal: its own tempo */
    j6_clock_ext(&c, 1);
    CHECK_EQ(j6_clock_spt_q12(&c), 460u * 1024u);
    CHECK_RANGE(j6_clock_tempo10(&c), 2396, 2398);
}

/* MI-8: an external clock that stops (500 ms timeout) and restarts at another tempo is measured afresh,
 * not averaged with the intervals from before the stop; still exactly 4 ticks per pulse */
static void test_clock_restart(void)
{
    j6_clock_t c;
    uint32_t p, ticks;
    j6_clock_init(&c, 1200);
    j6_clock_ext(&c, 1);
    for (p = 0; p < 10; p++) {
        j6_clock_ext_pulse(&c);
        j6_clock_advance(&c, 928);
    }
    CHECK_EQ(j6_clock_spt_q12(&c), 928u * 1024u);
    CHECK_EQ(j6_clock_ticks(&c), 40);
    /* the sender stops */
    j6_clock_advance(&c, 30000);
    CHECK_EQ(j6_clock_ext_alive(&c), 0);
    CHECK_EQ(j6_clock_ticks(&c), 40);
    /* and restarts at half the tempo: the first pulse has nothing measured yet (internal tick length) */
    j6_clock_ext_pulse(&c);
    CHECK_EQ(j6_clock_ext_alive(&c), 1);
    CHECK_EQ(j6_clock_spt_q12(&c), 940800);
    CHECK_EQ(j6_clock_tempo10(&c), 1200);
    j6_clock_advance(&c, 1856);
    j6_clock_ext_pulse(&c);
    CHECK_EQ(j6_clock_spt_q12(&c), 1856u * 1024u);   /* the new interval alone, not mixed with 928 */
    CHECK_RANGE(j6_clock_tempo10(&c), 593, 595);
    for (p = 0; p < 6; p++) {
        j6_clock_advance(&c, 1856);
        j6_clock_ext_pulse(&c);
    }
    CHECK_EQ(j6_clock_spt_q12(&c), 1856u * 1024u);
    j6_clock_advance(&c, 5000);
    ticks = j6_clock_ticks(&c);
    CHECK_EQ(ticks, 40u + 8u * 4u);
}

int main(void)
{
    RUN(test_pattern_init);
    RUN(test_pattern_clear);
    RUN(test_step_basics);
    RUN(test_key_note);
    RUN(test_key_chord);
    RUN(test_tie);
    RUN(test_step_phrase);
    RUN(test_insert);
    RUN(test_dupl);
    RUN(test_last_and_page);
    RUN(test_step_ticks);
    RUN(test_window_at);
    RUN(test_encode_layout);
    RUN(test_encode_roundtrip);
    RUN(test_decode_garbage);
    RUN(test_dirty);
    RUN(test_clipboard);
    RUN(test_select);
    RUN(test_transport);
    RUN(test_countin);
    RUN(test_plain_beats);
    RUN(test_plain_retrigger);
    RUN(test_ties_playback);
    RUN(test_phrase_to_plain);
    RUN(test_sq26);
    RUN(test_sq26_live);
    RUN(test_ph20_style_onoff_live);
    RUN(test_shuffle);
    RUN(test_pchg_step);
    RUN(test_pchg_n);
    RUN(test_stop_balanced);
    RUN(test_recording);
    RUN(test_rec_overdub_ties);
    RUN(test_rec_step_sweep);
    RUN(test_countin_beats);
    RUN(test_punch_play);
    RUN(test_64_patterns);
    RUN(test_switch_keeps_edits);
    RUN(test_pchg_settings);
    RUN(test_save_load);
    RUN(test_stress);
    RUN(test_clock_internal);
    RUN(test_clock_tempo_change);
    RUN(test_clock_external);
    RUN(test_clock_restart);
    printf("  sizeof(j6_seq_t) = %u, sizeof(j6_clock_t) = %u\n", (unsigned)sizeof(j6_seq_t),
           (unsigned)sizeof(j6_clock_t));
    return j6t_report("seq");
}
