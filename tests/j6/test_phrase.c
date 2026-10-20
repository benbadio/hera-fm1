/* SPDX-License-Identifier: GPL-3.0-only */
/* test_phrase.c - host tests of the phrase module: the held tracker (j6_phrase_held.c) and the phrase
 * engine (j6_phrase.c, j6_phrase_tables.h). docs/J6_ENGINE_DESIGN.md 5.4 lists what is covered.
 *
 * Styles 6-9 are checked against the real J-6 data: data/phrases_6_9.json is read here (run from the
 * repository root, as run_tests.sh does) and every one of the 192 lists is played through
 * j6_phrase_builtin and compared note for note: onset tick, gate, tone and accent. */
#include "j6_test.h"
#include "j6_phrase_state.h"

/* ======================================================================================================
 * harness
 * ==================================================================================================== */

static j6_phrase_t P;
static j6_held_t HT;
static j6t_log_t LOG;
static j6_evq_t Q;
static uint32_t SRC = J6_SRC_PHRASE_LIVE;
static uint8_t DEPTH[128];   /* notes sounding per MIDI note, from the events seen so far */

static const uint8_t HN[5][4] = {{0}, {60}, {60, 67}, {60, 64, 67}, {60, 64, 67, 71}};   /* the capture's H */

/* every event of a queue carries the instance's tag; nothing was lost; NOTE_ON velocities are 1..127;
 * a note is never started while it sounds and never stopped while silent (one NOTE_OFF per NOTE_ON) */
static void check_q(void)
{
    uint32_t i;
    CHECK_EQ(Q.lost, 0);
    for (i = 0; i < Q.n; i++) {
        const j6_event_t *e = &Q.ev[i];
        CHECK_EQ(e->src, SRC);
        CHECK(e->type == J6_EV_NOTE_ON || e->type == J6_EV_NOTE_OFF);
        CHECK_RANGE(e->sub, 0, J6_SUB - 1u);
        CHECK_RANGE(e->note, 0, 127);
        if (e->type == J6_EV_NOTE_ON) {
            CHECK_RANGE(e->vel, 1, 127);
            CHECK_EQ(DEPTH[e->note & 127u], 0);
            DEPTH[e->note & 127u] = 1;
        } else if (e->type == J6_EV_NOTE_OFF) {
            CHECK_EQ(DEPTH[e->note & 127u], 1);
            DEPTH[e->note & 127u] = 0;
        }
    }
}

static void ph_init_src(uint32_t style, uint32_t var, uint32_t src)
{
    SRC = src;
    memset(DEPTH, 0, sizeof DEPTH);
    j6_phrase_init(&P, src);
    j6_phrase_set_style(&P, style, var);
    j6_phrase_set_vel(&P, 100, 127);
    j6t_log_clear(&LOG);
}

static void ph_init(uint32_t style, uint32_t var)
{
    ph_init_src(style, var, J6_SRC_PHRASE_LIVE);
}

/* log time of the next tick = P.now (elapsed ticks since init) */
static void ph_held(const uint8_t *notes, uint32_t n)
{
    j6_evq_clear(&Q);
    j6_phrase_set_held(&P, notes, n, &Q);
    check_q();
    j6t_log_add(&LOG, &Q, P.now);
}

static void ph_tick(uint32_t pt)
{
    uint32_t t = P.now;
    j6_evq_clear(&Q);
    j6_phrase_tick(&P, pt, &Q);
    check_q();
    CHECK(Q.n <= 48u);
    j6t_log_add(&LOG, &Q, t);
}

static void ph_run(uint32_t pt0, uint32_t ticks)
{
    uint32_t i;
    for (i = 0; i < ticks; i++) ph_tick(pt0 + i);
}

/* stop, then the whole timeline must be balanced and nothing may sound */
static void ph_finish(void)
{
    uint32_t i;
    j6_evq_clear(&Q);
    j6_phrase_stop(&P, &Q);
    check_q();
    j6t_log_add(&LOG, &Q, P.now);
    CHECK(j6t_log_balanced(&LOG));
    CHECK_EQ(LOG.overflow, 0);
    CHECK_EQ(j6_phrase_sounding(&P), 0);
    for (i = 0; i < 128u; i++) CHECK_EQ(DEPTH[i], 0);
}

/* the notes of the log: ON time, OFF time (the next OFF of that note after its ON), note, velocity */
typedef struct {
    uint32_t on, off;
    uint8_t note, vel;
} nt_t;
#define NT_MAX 1024u
static nt_t NT[NT_MAX];

static uint32_t notes_of(nt_t *out, uint32_t max)
{
    uint32_t i, j, k = 0;
    for (i = 0; i < LOG.n && k < max; i++) {
        const j6t_logged_t *e = &LOG.e[i];
        if (e->ev.type != J6_EV_NOTE_ON) continue;
        out[k].on = e->time;
        out[k].off = 0xFFFFFFFFu;
        out[k].note = e->ev.note;
        out[k].vel = e->ev.vel;
        for (j = i + 1; j < LOG.n; j++)
            if (LOG.e[j].ev.type == J6_EV_NOTE_OFF && LOG.e[j].ev.note == e->ev.note) {
                out[k].off = LOG.e[j].time;
                break;
            }
        k++;
    }
    return k;
}

static int nt_cmp(const void *a, const void *b)
{
    const nt_t *x = (const nt_t *)a, *y = (const nt_t *)b;
    if (x->on != y->on) return x->on < y->on ? -1 : 1;
    return (int)x->note - (int)y->note;
}

/* notes with an ON time in [from, to) sub-ticks, sorted by (on, note) */
static uint32_t notes_in(nt_t *out, uint32_t max, uint32_t from, uint32_t to)
{
    static nt_t all[NT_MAX];
    uint32_t n = notes_of(all, NT_MAX), i, k = 0;
    for (i = 0; i < n && k < max; i++)
        if (all[i].on >= from && all[i].on < to) out[k++] = all[i];
    qsort(out, k, sizeof out[0], nt_cmp);
    return k;
}

/* ======================================================================================================
 * held tracker (j6.h 9.2, HD-1 .. HD-3, CH-6)
 * ==================================================================================================== */

static uint32_t held_is(const uint8_t *want, uint32_t n)
{
    uint8_t got[J6_HELD_MAX];
    uint32_t k = j6_held_get(&HT, got), i;
    if (k != n) return 0;
    for (i = 0; i < n; i++)
        if (got[i] != want[i]) return 0;
    return 1;
}
#define HELD_IS(...)                                                                             \
    do {                                                                                         \
        static const uint8_t w_[] = {__VA_ARGS__};                                               \
        CHECK(held_is(w_, sizeof w_));                                                           \
    } while (0)
#define HELD_EMPTY() CHECK(held_is(NULL, 0))

static void test_held_poly_last4(void)
{
    j6_held_init(&HT);
    HELD_EMPTY();
    CHECK_EQ(j6_held_take_change(&HT), 0);
    j6_held_press(&HT, 60);
    CHECK_EQ(j6_held_take_change(&HT), 1);
    CHECK_EQ(j6_held_take_change(&HT), 0);                 /* once per change */
    j6_held_press(&HT, 62);
    j6_held_press(&HT, 64);
    j6_held_press(&HT, 65);
    HELD_IS(60, 62, 64, 65);
    j6_held_press(&HT, 67);                                /* C D E F G: the last 4 pressed */
    HELD_IS(62, 64, 65, 67);
    CHECK_EQ(j6_held_down(&HT), 5);
    CHECK_EQ(j6_held_take_change(&HT), 1);
    j6_held_press(&HT, 62);                                /* already down: ignored */
    CHECK_EQ(j6_held_take_change(&HT), 0);
    j6_held_release(&HT, 70);                              /* not down: ignored */
    CHECK_EQ(j6_held_take_change(&HT), 0);
    j6_held_release(&HT, 67);                              /* release G: C comes back */
    HELD_IS(60, 62, 64, 65);
    CHECK_EQ(j6_held_take_change(&HT), 1);
    j6_held_release(&HT, 60);                              /* 4 down: all of them */
    HELD_IS(62, 64, 65);
    j6_held_release(&HT, 62);
    j6_held_release(&HT, 64);
    j6_held_release(&HT, 65);
    HELD_EMPTY();
    CHECK_EQ(j6_held_down(&HT), 0);
    CHECK_EQ(j6_held_take_change(&HT), 1);
    CHECK_EQ(j6_held_latched(&HT), 0);
}

static void test_held_down_overflow(void)
{
    uint32_t i;
    j6_held_init(&HT);
    for (i = 0; i < 17u; i++) j6_held_press(&HT, 40u + i);   /* one more than J6_HELD_DOWN_MAX */
    CHECK_EQ(j6_held_down(&HT), J6_HELD_DOWN_MAX);
    HELD_IS(53, 54, 55, 56);
    j6_held_take_change(&HT);
    j6_held_release(&HT, 40);                              /* forgotten: ignored */
    CHECK_EQ(j6_held_take_change(&HT), 0);
    CHECK_EQ(j6_held_down(&HT), J6_HELD_DOWN_MAX);
    j6_held_clear(&HT);
    HELD_EMPTY();
    CHECK_EQ(j6_held_down(&HT), 0);
    CHECK_EQ(j6_held_take_change(&HT), 1);
    j6_held_clear(&HT);
    CHECK_EQ(j6_held_take_change(&HT), 0);                 /* nothing changed */
}

static void test_held_latch_hd2(void)
{
    j6_held_init(&HT);
    /* turning the latch on with nothing down latches nothing */
    j6_held_latch(&HT, 1);
    CHECK_EQ(j6_held_latched(&HT), 1);
    HELD_EMPTY();
    CHECK_EQ(j6_held_take_change(&HT), 0);
    /* HD-2: a press after all keys were up replaces the set */
    j6_held_press(&HT, 60);
    HELD_IS(60);
    /* HD-2: while at least one key is down, new presses are added */
    j6_held_press(&HT, 64);
    HELD_IS(60, 64);
    CHECK_EQ(j6_held_take_change(&HT), 1);
    /* HD-2 / HD-1: releases change nothing, the set keeps sounding */
    j6_held_release(&HT, 60);
    j6_held_release(&HT, 64);
    HELD_IS(60, 64);
    CHECK_EQ(j6_held_take_change(&HT), 0);
    /* a press after all were up replaces */
    j6_held_press(&HT, 67);
    HELD_IS(67);
    CHECK_EQ(j6_held_take_change(&HT), 1);
    /* at most 4; a 5th drops the oldest */
    j6_held_press(&HT, 69);
    j6_held_press(&HT, 71);
    j6_held_press(&HT, 72);
    HELD_IS(67, 69, 71, 72);
    j6_held_press(&HT, 74);
    HELD_IS(69, 71, 72, 74);
    /* turning HOLD off releases every latched note that is not physically down */
    j6_held_release(&HT, 67);
    j6_held_release(&HT, 69);
    j6_held_release(&HT, 71);
    HELD_IS(69, 71, 72, 74);
    j6_held_take_change(&HT);
    j6_held_latch(&HT, 0);
    HELD_IS(72, 74);
    CHECK_EQ(j6_held_take_change(&HT), 1);
    /* turning it on latches what is down */
    j6_held_latch(&HT, 1);
    HELD_IS(72, 74);
    CHECK_EQ(j6_held_take_change(&HT), 0);
    j6_held_release(&HT, 72);
    j6_held_release(&HT, 74);
    HELD_IS(72, 74);
    j6_held_latch(&HT, 1);                                 /* again: no change */
    CHECK_EQ(j6_held_take_change(&HT), 0);
    j6_held_latch(&HT, 0);
    HELD_EMPTY();
    CHECK_EQ(j6_held_take_change(&HT), 1);
    /* latch on with more than 4 down: the last 4 pressed */
    j6_held_press(&HT, 50);
    j6_held_press(&HT, 52);
    j6_held_press(&HT, 53);
    j6_held_press(&HT, 55);
    j6_held_press(&HT, 57);
    j6_held_latch(&HT, 1);
    HELD_IS(52, 53, 55, 57);
    /* pressing an id that is already latched while keys are down changes nothing */
    j6_held_release(&HT, 52);
    j6_held_take_change(&HT);
    j6_held_press(&HT, 52);
    HELD_IS(52, 53, 55, 57);
    CHECK_EQ(j6_held_take_change(&HT), 0);
}

static void test_held_mono_ch6(void)
{
    j6_held_init(&HT);
    j6_held_press(&HT, 60);                                /* poly */
    j6_held_take_change(&HT);
    j6_held_mono(&HT, 1);                                  /* changing the mode clears */
    HELD_EMPTY();
    CHECK_EQ(j6_held_down(&HT), 0);
    CHECK_EQ(j6_held_take_change(&HT), 1);
    j6_held_mono(&HT, 1);
    CHECK_EQ(j6_held_take_change(&HT), 0);
    /* CH-6: one chord at a time, the last pressed */
    j6_held_press(&HT, 64 + 0);
    HELD_IS(64);
    j6_held_press(&HT, 64 + 7);
    HELD_IS(71);
    j6_held_press(&HT, 64 + 9);
    HELD_IS(73);
    CHECK_EQ(j6_held_take_change(&HT), 1);
    /* releasing it while others are held: the previous still-down chord sounds again */
    j6_held_release(&HT, 73);
    HELD_IS(71);
    CHECK_EQ(j6_held_take_change(&HT), 1);
    j6_held_release(&HT, 64);                              /* not the sounding one: no change */
    HELD_IS(71);
    CHECK_EQ(j6_held_take_change(&HT), 0);
    j6_held_release(&HT, 71);                              /* nothing down: the chord stops */
    HELD_EMPTY();
    CHECK_EQ(j6_held_take_change(&HT), 1);
    /* HD-2: in chord mode the latched set is the last chord */
    j6_held_latch(&HT, 1);
    j6_held_press(&HT, 66);
    HELD_IS(66);
    j6_held_press(&HT, 68);                                /* while 66 is down */
    HELD_IS(68);
    j6_held_release(&HT, 68);                              /* CH-6 still: 66 is down, it sounds again */
    HELD_IS(66);
    j6_held_release(&HT, 66);                              /* nothing down: the latch keeps the last */
    HELD_IS(66);
    CHECK_EQ(j6_held_down(&HT), 0);
    j6_held_press(&HT, 65);
    HELD_IS(65);
    j6_held_release(&HT, 65);
    j6_held_take_change(&HT);
    j6_held_latch(&HT, 0);                                 /* nothing down: empty */
    HELD_EMPTY();
    CHECK_EQ(j6_held_take_change(&HT), 1);
    /* latch on while a chord key is down keeps it */
    j6_held_press(&HT, 70);
    j6_held_latch(&HT, 1);
    j6_held_release(&HT, 70);
    HELD_IS(70);
    j6_held_mono(&HT, 0);                                  /* back to poly: cleared, latch still on */
    HELD_EMPTY();
    CHECK_EQ(j6_held_latched(&HT), 1);
}

/* ======================================================================================================
 * set_held normalisation, defaults, setters
 * ==================================================================================================== */

static void test_set_held_normalise(void)
{
    static const uint8_t a[] = {67, 60, 64, 60};
    static const uint8_t b[] = {60, 62, 64, 65, 67, 69};
    static const uint8_t c[] = {200, 61, 255};
    ph_init(3, 4);
    ph_held(a, 4);
    CHECK_EQ(P.n, 3);
    CHECK_EQ(P.held[0], 60);
    CHECK_EQ(P.held[1], 64);
    CHECK_EQ(P.held[2], 67);
    ph_held(b, 6);                                         /* the last 4 given */
    CHECK_EQ(P.n, 4);
    CHECK_EQ(P.held[0], 64);
    CHECK_EQ(P.held[3], 69);
    ph_held(c, 3);                                         /* notes above 127 are ignored */
    CHECK_EQ(P.n, 1);
    CHECK_EQ(P.held[0], 61);
    ph_held(NULL, 3);
    CHECK_EQ(P.n, 0);
    ph_finish();
}

static void test_defaults_and_setters(void)
{
    j6_phrase_init(&P, J6_SRC_PHRASE_SEQ);
    CHECK_EQ(P.src, J6_SRC_PHRASE_SEQ);
    CHECK_EQ(P.style, 1);
    CHECK_EQ(P.var, 1);
    CHECK_EQ(P.vel, J6_VELO_DEF);
    CHECK_EQ(P.avel, J6_AVEL_DEF);
    CHECK(P.bank == &j6_phrase_builtin);
    CHECK_EQ(j6_phrase_sounding(&P), 0);
    j6_phrase_set_vel(&P, 0, 300);
    CHECK_EQ(P.vel, 1);
    CHECK_EQ(P.avel, 127);
    j6_phrase_set_shuffle(&P, 200);
    CHECK_EQ(P.shuffle, 90);
    j6_phrase_set_shuffle(&P, -200);
    CHECK_EQ(P.shuffle, -90);
    j6_phrase_set_style(&P, 0, 0);
    CHECK_EQ(P.style, 1);
    CHECK_EQ(P.var, 1);
    j6_phrase_set_style(&P, 12, 40);
    CHECK_EQ(P.style, 9);
    CHECK_EQ(P.var, 12);
    j6_phrase_set_bank(&P, NULL);
    CHECK(P.bank == &j6_phrase_builtin);
    /* an empty H never plays */
    SRC = J6_SRC_PHRASE_SEQ;
    j6t_log_clear(&LOG);
    ph_run(0, 800);
    CHECK_EQ(LOG.n, 0);
    SRC = J6_SRC_PHRASE_LIVE;
}

/* ======================================================================================================
 * styles 1-2: arpeggios (PH-10 .. PH-13)
 * ==================================================================================================== */

static const uint8_t PH12[6][4][14] = {
    /* UP1 */ {{60}, {60, 67}, {60, 64, 67}, {60, 64, 67, 71}},
    /* UP&DOWN1 */ {{60}, {60, 67}, {60, 64, 67, 64}, {60, 64, 67, 71, 67, 64}},
    /* DOWN1 */ {{60}, {67, 60}, {67, 64, 60}, {71, 67, 64, 60}},
    /* DOWN2 */ {{72, 60}, {79, 72, 67, 60}, {79, 76, 72, 67, 64, 60}, {83, 79, 76, 72, 71, 67, 64, 60}},
    /* UP&DOWN2 */
    {{60, 72}, {60, 67, 72, 79, 72, 67}, {60, 64, 67, 72, 76, 79, 76, 72, 67, 64},
     {60, 64, 67, 71, 72, 76, 79, 83, 79, 76, 72, 71, 67, 64}},
    /* UP2 */ {{60, 72}, {60, 67, 72, 79}, {60, 64, 67, 72, 76, 79}, {60, 64, 67, 71, 72, 76, 79, 83}},
};
static const uint8_t PH12_LEN[6][4] = {{1, 2, 3, 4}, {1, 2, 4, 6}, {1, 2, 3, 4},
                                       {2, 4, 6, 8}, {2, 6, 10, 14}, {2, 4, 6, 8}};

/* plays style / var over H_n for two cycles from pt 0 and checks every hit */
static void check_arp(uint32_t style, uint32_t var, uint32_t n, const uint8_t *seq, uint32_t len, uint32_t div)
{
    uint32_t i, k;
    ph_init(style, var);
    ph_held(HN[n], n);
    ph_run(0, 2u * len * div);
    k = notes_in(NT, NT_MAX, 0, 2u * len * div * J6_SUB);
    CHECK_EQ(k, 2u * len);
    for (i = 0; i < k && i < 2u * len; i++) {
        CHECK_EQ(NT[i].note, seq[i % len]);
        CHECK_EQ(NT[i].on, i * div * J6_SUB);                       /* PH-11: hit i at i * div */
        CHECK_EQ(NT[i].off - NT[i].on, j6_gate(div) * J6_SUB);      /* gate g(div) */
        CHECK_EQ(NT[i].vel, 100);
    }
    CHECK_EQ(j6_phrase_loop_ticks(NULL, style, var, n), len * div);
    ph_finish();
}

static void test_arp_ph12_cells(void)
{
    uint32_t o, n;
    for (o = 0; o < 6u; o++)
        for (n = 1; n <= 4u; n++) {
            check_arp(1, o + 1u, n, PH12[o][n - 1u], PH12_LEN[o][n - 1u], 48);     /* style 1 v1-6: 8th */
            check_arp(2, o + 7u, n, PH12[o][n - 1u], PH12_LEN[o][n - 1u], 16);     /* style 2 v7-12: 16th T */
        }
}

static void test_arp_divisions(void)
{
    uint32_t o;
    for (o = 0; o < 6u; o++) {
        check_arp(1, o + 7u, 3, PH12[o][2], PH12_LEN[o][2], 32);    /* style 1 v7-12: 8th triplet */
        check_arp(2, o + 1u, 3, PH12[o][2], PH12_LEN[o][2], 24);    /* style 2 v1-6: 16th */
    }
}

static void test_arp_range2_off(void)
{
    static const uint8_t ud2[] = {60, 64, 67, 67, 64, 60};    /* Q-1's other reading */
    static const uint8_t up[] = {60, 64, 67}, down[] = {67, 64, 60}, ud1[] = {60, 64, 67, 64};
    static const uint8_t ud2n1[] = {60, 60};
    uint32_t i, k;
    struct {
        uint32_t var;
        const uint8_t *seq;
        uint32_t len, n;
    } c[] = {{5, ud2, 6, 3}, {6, up, 3, 3}, {4, down, 3, 3}, {2, ud1, 4, 3}, {5, ud2n1, 2, 1}};
    for (i = 0; i < sizeof c / sizeof c[0]; i++) {
        uint32_t j;
        ph_init(1, c[i].var);
        P.cfg_range2 = 0;
        j6_phrase_set_style(&P, 1, c[i].var);              /* rebuild with the switch */
        ph_held(HN[c[i].n], c[i].n);
        ph_run(0, 2u * c[i].len * 48u);
        k = notes_in(NT, NT_MAX, 0, 2u * c[i].len * 48u * J6_SUB);
        CHECK_EQ(k, 2u * c[i].len);
        for (j = 0; j < k && j < 2u * c[i].len; j++) {
            CHECK_EQ(NT[j].note, c[i].seq[j % c[i].len]);
            CHECK_EQ(NT[j].on, j * 48u * J6_SUB);
        }
        ph_finish();
    }
}

static void test_arp_high_notes(void)
{
    static const uint8_t h[] = {110, 120};
    uint32_t k;
    /* range 2 drops notes above 127: UP2 over [110 120] is 110 120 122 */
    ph_init(2, 6);
    ph_held(h, 2);
    ph_run(0, 6u * 24u);
    k = notes_in(NT, NT_MAX, 0, 6u * 24u * J6_SUB);
    CHECK_EQ(k, 6);
    CHECK_EQ(NT[0].note, 110);
    CHECK_EQ(NT[1].note, 120);
    CHECK_EQ(NT[2].note, 122);
    CHECK_EQ(NT[3].note, 110);
    ph_finish();
}

/* PH-13: the arp index resets on empty -> set and on a different set; the same set carries on */
static void test_arp_reset_ph13(void)
{
    static const uint8_t ceg[] = {60, 64, 67}, cea[] = {60, 64, 69}, eg[] = {64, 67, 60};
    uint32_t k;
    ph_init(2, 1);                                         /* UP1, 16ths */
    ph_held(ceg, 3);
    ph_run(0, 30);                                         /* hits at 0 (60) and 24 (64) */
    ph_held(eg, 3);                                        /* the same set (another order): nothing */
    CHECK_EQ(Q.n, 0);
    ph_run(30, 20);                                        /* hit at 48: 67 (idx 2) */
    ph_held(cea, 3);                                       /* a different set: reset */
    CHECK_EQ(Q.n, 1);                                      /* 67 (sounding) left H: stopped */
    CHECK(j6t_evq_find(&Q, 0, J6_EV_NOTE_OFF, 67) == 0);
    ph_run(50, 50);                                        /* hits at 72: 60, 96: 64 */
    ph_held(NULL, 0);
    ph_run(100, 10);
    ph_held(ceg, 3);                                       /* empty -> set: reset */
    ph_run(110, 40);                                       /* hits at 120: 60, 144: 64 */
    k = notes_in(NT, NT_MAX, 0, 0xFFFFFFFFu);
    CHECK_EQ(k, 7);
    CHECK_EQ(NT[0].note, 60);
    CHECK_EQ(NT[1].note, 64);
    CHECK_EQ(NT[2].note, 67);
    CHECK_EQ(NT[2].on, 48u * J6_SUB);
    CHECK_EQ(NT[3].note, 60);
    CHECK_EQ(NT[3].on, 72u * J6_SUB);
    CHECK_EQ(NT[4].note, 64);
    CHECK_EQ(NT[5].note, 60);
    CHECK_EQ(NT[5].on, 120u * J6_SUB);
    CHECK_EQ(NT[6].note, 64);
    ph_finish();
    /* a style change keeps the index (modulo the new cycle): after 2 UP1 hits, DOWN1 plays cycle[2] */
    ph_init(2, 1);
    ph_held(ceg, 3);
    ph_run(0, 30);
    j6_phrase_set_style(&P, 2, 3);
    ph_run(30, 20);
    k = notes_in(NT, NT_MAX, 0, 0xFFFFFFFFu);
    CHECK_EQ(k, 3);
    CHECK_EQ(NT[2].note, 60);                              /* DOWN1 = 67 64 60, idx 2 */
    ph_finish();
}

/* a set change while an arp note sounds: the note whose held note left H stops, no late join */
static void test_arp_set_change(void)
{
    static const uint8_t ce[] = {60, 64}, cg[] = {60, 67};
    uint32_t k;
    ph_init(1, 1);                                         /* UP1, 8ths, gate 39 */
    ph_held(ce, 2);
    ph_run(0, 50);                                         /* 60 at 0, 64 at 48 (sounding until 87) */
    ph_held(cg, 2);                                        /* 64 left H: OFF at once */
    CHECK_EQ(Q.n, 1);
    CHECK_EQ(Q.ev[0].type, J6_EV_NOTE_OFF);
    CHECK_EQ(Q.ev[0].note, 64);
    CHECK_EQ(Q.ev[0].sub, 0);
    ph_tick(50);                                           /* ARP: no late join */
    CHECK_EQ(Q.n, 0);
    ph_run(51, 50);                                        /* reset: 96 plays 60 */
    k = notes_in(NT, NT_MAX, 0, 0xFFFFFFFFu);
    CHECK_EQ(k, 3);
    CHECK_EQ(NT[1].off, 50u * J6_SUB);
    CHECK_EQ(NT[2].note, 60);
    CHECK_EQ(NT[2].on, 96u * J6_SUB);
    ph_finish();
}

/* ======================================================================================================
 * style 3 note values (PH-14), styles 4-5 rhythm strings (PH-15, PH-16)
 * ==================================================================================================== */

static void test_style3_values(void)
{
    static const uint32_t len[12] = {768, 384, 192, 96, 48, 24, 512, 256, 128, 64, 32, 16};
    uint32_t v, i, k, hits;
    for (v = 1; v <= 12u; v++) {
        uint32_t L = len[v - 1u], w = 2u * L + 7u;
        ph_init(3, v);
        ph_held(HN[3], 3);
        ph_run(0, w);
        hits = (w + L - 1u) / L;
        k = notes_in(NT, NT_MAX, 0, w * J6_SUB);
        CHECK_EQ(k, 3u * hits);
        for (i = 0; i < k; i++) {
            CHECK_EQ(NT[i].on, (i / 3u) * L * J6_SUB);
            CHECK_EQ(NT[i].note, HN[3][i % 3u]);
            if (NT[i].on + j6_gate(L) * J6_SUB <= w * J6_SUB) CHECK_EQ(NT[i].off - NT[i].on, j6_gate(L) * J6_SUB);
        }
        CHECK_EQ(j6_phrase_loop_ticks(NULL, 3, v, 2), L);
        ph_finish();
    }
}

typedef struct {
    uint8_t n;
    uint8_t t[3], len[3];
} rhy_t;
static const rhy_t S4[12] = {
    {1, {0}, {24}},          {1, {24}, {24}},           {1, {48}, {24}},           {1, {72}, {24}},
    {2, {0, 48}, {48, 24}},  {2, {0, 48}, {24, 48}},    {2, {0, 72}, {72, 24}},    {2, {24, 72}, {48, 24}},
    {3, {0, 24, 48}, {24, 24, 24}}, {3, {0, 24, 72}, {24, 24, 24}}, {3, {0, 48, 72}, {24, 24, 24}},
    {3, {24, 48, 72}, {24, 24, 24}},
};
static const rhy_t S5[12] = {
    {2, {0, 24}, {24, 24}}, {2, {0, 48}, {24, 24}}, {2, {0, 72}, {24, 24}}, {2, {24, 48}, {24, 24}},
    {2, {24, 72}, {24, 24}}, {2, {48, 72}, {24, 24}}, {1, {0}, {24}}, {1, {24}, {24}}, {1, {48}, {24}},
    {2, {0, 24}, {24, 24}}, {2, {0, 48}, {24, 24}}, {2, {24, 48}, {24, 24}},
};

/* expected hit (onset, length) list of a rhythm over [0, w) ticks */
static uint32_t rhythm_expect(const rhy_t *r, uint32_t loop, uint32_t restart_bar, uint32_t w, uint32_t *on,
                              uint32_t *ln)
{
    uint32_t k = 0, base, j;
    for (base = 0; base < w; base += restart_bar ? J6_TICKS_BAR : loop) {
        uint32_t rep;
        for (rep = 0; rep < (restart_bar ? J6_TICKS_BAR : loop); rep += loop)
            for (j = 0; j < r->n; j++) {
                uint32_t t = base + rep + r->t[j];
                if (restart_bar && rep + r->t[j] >= J6_TICKS_BAR) continue;
                if (t < w) {
                    on[k] = t;
                    ln[k++] = r->len[j];
                }
            }
    }
    return k;
}

static void check_rhythm(uint32_t style, uint32_t var, const rhy_t *r, uint32_t loop, uint32_t restart_bar,
                         uint32_t run_on)
{
    uint32_t on[64], ln[64], m, k, i, w = 2u * J6_TICKS_BAR;
    ph_init(style, var);
    P.cfg_s5_run_on = (uint8_t)run_on;
    j6_phrase_set_style(&P, style, var);
    ph_held(HN[2], 2);
    ph_run(0, w);
    m = rhythm_expect(r, loop, restart_bar, w, on, ln);
    k = notes_in(NT, NT_MAX, 0, w * J6_SUB);
    CHECK_EQ(k, 2u * m);
    for (i = 0; i < k && i / 2u < m; i++) {
        CHECK_EQ(NT[i].on, on[i / 2u] * J6_SUB);
        CHECK_EQ(NT[i].note, HN[2][i % 2u]);                   /* every hit plays all of H */
        CHECK_EQ(NT[i].off - NT[i].on, j6_gate(ln[i / 2u]) * J6_SUB);
        CHECK_EQ(NT[i].vel, 100);                              /* PH-8: no accents in styles 1-5 */
    }
    ph_finish();
}

static void test_style4_strings(void)
{
    uint32_t v;
    for (v = 1; v <= 12u; v++) {
        check_rhythm(4, v, &S4[v - 1u], 96, 0, 1);
        CHECK_EQ(j6_phrase_loop_ticks(NULL, 4, v, 3), 96);
    }
}

static void test_style5_strings(void)
{
    uint32_t v;
    for (v = 1; v <= 6u; v++) {
        check_rhythm(5, v, &S5[v - 1u], 96, 0, 1);
        check_rhythm(5, v, &S5[v - 1u], 96, 0, 0);
    }
    for (v = 7; v <= 12u; v++) {
        check_rhythm(5, v, &S5[v - 1u], 72, 0, 1);            /* J6_CFG_S5_RUN_ON 1: pt % 72 */
        check_rhythm(5, v, &S5[v - 1u], 72, 1, 0);            /* J6_CFG_S5_RUN_ON 0: pt % 384 % 72 */
    }
    CHECK_EQ(j6_phrase_loop_ticks(NULL, 5, 7, 1), J6_CFG_S5_RUN_ON ? 72u : 384u);
    CHECK_EQ(j6_phrase_loop_ticks(NULL, 5, 6, 1), 96);
}

/* ======================================================================================================
 * set changes: PH-6 cut, partial cut, late join, staggered presses
 * ==================================================================================================== */

static void test_empty_cuts_everything(void)
{
    static const uint8_t ceg[] = {60, 64, 67};
    uint32_t i;
    ph_init(3, 1);                                         /* a double whole note */
    ph_held(ceg, 3);
    ph_run(0, 100);
    CHECK_EQ(j6_phrase_sounding(&P), 3);
    ph_held(NULL, 0);
    CHECK_EQ(Q.n, 3);
    for (i = 0; i < Q.n; i++) {
        CHECK_EQ(Q.ev[i].type, J6_EV_NOTE_OFF);
        CHECK_EQ(Q.ev[i].sub, 0);
    }
    CHECK_EQ(j6_phrase_sounding(&P), 0);
    LOG.n = 0;
    ph_run(100, 2000);                                     /* no hit after the release */
    CHECK_EQ(LOG.n, 0);
    ph_finish();
}

static void test_late_join_style3(void)
{
    static const uint8_t ceg[] = {60, 64, 67}, cfa[] = {60, 65, 69};
    uint32_t k;
    ph_init(3, 1);                                         /* 768 ticks, gate 758 */
    ph_held(ceg, 3);
    ph_run(0, 100);
    ph_held(cfa, 3);                                       /* E and G left H: stop at once */
    CHECK_EQ(Q.n, 2);
    CHECK(j6t_evq_find(&Q, 0, J6_EV_NOTE_OFF, 64) >= 0);
    CHECK(j6t_evq_find(&Q, 0, J6_EV_NOTE_OFF, 67) >= 0);
    ph_tick(100);                                          /* late join: F A for the rest of the gate */
    CHECK_EQ(j6t_evq_count(&Q, J6_EV_NOTE_ON), 2);
    CHECK(j6t_evq_find(&Q, 0, J6_EV_NOTE_ON, 65) >= 0);
    CHECK(j6t_evq_find(&Q, 0, J6_EV_NOTE_ON, 69) >= 0);
    CHECK_EQ(Q.ev[0].sub, 0);
    ph_run(101, 700);
    k = notes_in(NT, NT_MAX, 0, 0xFFFFFFFFu);
    CHECK_EQ(k, 8);
    CHECK_EQ(NT[0].note, 60);                              /* C kept sounding to its own end */
    CHECK_EQ(NT[0].off, 758u * J6_SUB);
    CHECK_EQ(NT[1].off, 100u * J6_SUB);
    CHECK_EQ(NT[3].note, 65);
    CHECK_EQ(NT[3].on, 100u * J6_SUB);
    CHECK_EQ(NT[3].off, 758u * J6_SUB);                    /* the rest of the gate */
    CHECK_EQ(NT[4].note, 69);
    CHECK_EQ(NT[4].off, 758u * J6_SUB);
    CHECK_EQ(NT[5].on, 768u * J6_SUB);                     /* then the next hit plays C F A */
    CHECK_EQ(NT[5].note, 60);
    CHECK_EQ(NT[6].note, 65);
    CHECK_EQ(NT[7].note, 69);
    ph_finish();
}

static void test_staggered_presses(void)
{
    static const uint8_t c[] = {60}, ce[] = {60, 64}, cg[] = {60, 67};
    uint32_t k;
    /* style 3 v4: C at tick 0, E at tick 2 make one chord */
    ph_init(3, 4);
    ph_held(c, 1);
    ph_run(0, 2);
    ph_held(ce, 2);
    ph_run(2, 100);
    k = notes_in(NT, NT_MAX, 0, 96u * J6_SUB);
    CHECK_EQ(k, 2);
    CHECK_EQ(NT[0].note, 60);
    CHECK_EQ(NT[1].note, 64);
    CHECK_EQ(NT[1].on, 2u * J6_SUB);
    CHECK_EQ(NT[0].off, 86u * J6_SUB);
    CHECK_EQ(NT[1].off, 86u * J6_SUB);
    ph_finish();
    /* a captured table (s6v01): list n1 -> n2; both notes end where the n2 list's events end */
    ph_init(6, 1);
    ph_held(c, 1);
    ph_run(0, 2);
    ph_held(cg, 2);
    ph_run(2, 100);
    k = notes_in(NT, NT_MAX, 0, 96u * J6_SUB);
    CHECK_EQ(k, 2);
    CHECK_EQ(NT[1].note, 67);
    CHECK_EQ(NT[1].on, 2u * J6_SUB);
    CHECK_EQ(NT[0].off, 96u * J6_SUB);
    CHECK_EQ(NT[1].off, 96u * J6_SUB);
    k = notes_in(NT, NT_MAX, 96u * J6_SUB, 97u * J6_SUB);  /* s6v01 n2: tone 1 at 96 */
    CHECK_EQ(k, 1);
    CHECK_EQ(NT[0].note, 67);
    ph_finish();
}

static void test_late_join_rules(void)
{
    static const uint8_t ce[] = {60, 64}, cg[] = {60, 67};
    uint32_t k;
    /* a hit onset in the tick anyway: no late join, no double note */
    ph_init(3, 4);
    ph_held(ce, 2);
    ph_run(0, 96);
    ph_held(cg, 2);
    ph_tick(96);
    CHECK_EQ(j6t_evq_count(&Q, J6_EV_NOTE_ON), 2);
    ph_finish();
    /* gate already over: nothing until the next hit */
    ph_init(3, 4);
    ph_held(ce, 2);
    ph_run(0, 90);                                         /* gate 86 is over */
    ph_held(cg, 2);
    ph_tick(90);
    CHECK_EQ(j6t_evq_count(&Q, J6_EV_NOTE_ON), 0);
    ph_finish();
    /* from an empty H (play from the middle of a long note): wait for the grid (PH-21) */
    ph_init(3, 1);
    ph_held(ce, 2);
    ph_run(100, 700);
    k = notes_in(NT, NT_MAX, 0, 0xFFFFFFFFu);
    CHECK_EQ(k, 2);
    CHECK_EQ(NT[0].on, 668u * J6_SUB);                     /* pt 768 */
    ph_finish();
    /* review: a chord pressed one key per tick in the middle of a long note waits for the grid like a
       chord pressed in one tick (PH-21): the late join must not take a hit that began before H did */
    {
        static const uint8_t c[] = {60}, d[] = {62};
        ph_init(3, 1);                                     /* 768 ticks, gate 758 */
        ph_held(c, 1);
        ph_tick(100);
        CHECK_EQ(j6t_evq_count(&Q, J6_EV_NOTE_ON), 0);
        ph_held(ce, 2);                                    /* E one tick later */
        ph_tick(101);
        CHECK_EQ(j6t_evq_count(&Q, J6_EV_NOTE_ON), 0);
        ph_run(102, 200);
        ph_held(d, 1);                                     /* a change later in the same hold: silent */
        ph_run(302, 466);                                  /* up to pt 767 */
        k = notes_in(NT, NT_MAX, 0, 0xFFFFFFFFu);
        CHECK_EQ(k, 0);
        ph_held(ce, 2);
        ph_tick(768);                                      /* the grid: C E together */
        CHECK_EQ(j6t_evq_count(&Q, J6_EV_NOTE_ON), 2);
        CHECK(j6t_evq_find(&Q, 0, J6_EV_NOTE_ON, 60) >= 0);
        CHECK(j6t_evq_find(&Q, 0, J6_EV_NOTE_ON, 64) >= 0);
        ph_finish();
        /* but a hold that began before the hit still joins it */
        ph_init(3, 1);
        ph_held(c, 1);
        ph_run(0, 101);                                    /* C sounds from pt 0 */
        ph_held(ce, 2);
        ph_tick(101);
        CHECK_EQ(j6t_evq_count(&Q, J6_EV_NOTE_ON), 1);
        CHECK(j6t_evq_find(&Q, 0, J6_EV_NOTE_ON, 64) >= 0);
        ph_finish();
        /* and a hold that began after the hit's onset, inside a later loop, joins the next one: hold from
           pt 10 of loop 2 (pt 778), the hit of loop 2 began at 768: silent; it joins nothing before 1536 */
        ph_init(3, 1);
        ph_held(c, 1);
        ph_run(778, 2);
        ph_held(ce, 2);
        ph_tick(780);
        CHECK_EQ(j6t_evq_count(&Q, J6_EV_NOTE_ON), 0);
        ph_finish();
    }
}

/* ======================================================================================================
 * styles 6-9: tables
 * ==================================================================================================== */

static j6_plist_t TB_LISTS[J6_PLISTS];
static const j6_pev_t TB_POOL[] = {
    /* 0: style 6 v1 n2, loop 96 */
    {0, 160, 0, 0},
    {0, 160, 1 | J6_PEV_ACCENT, 12},
    {40, 48, 1, -12},
    {1500, 16, 0, 0},                                      /* tick 93, sub 12 */
    /* 4: style 6 v1 n1, loop 48 */
    {8, 100, 0, 24},
    /* 5: style 7 v1 n1, loop 96: src beyond H and a note above 127: both skipped */
    {0, 64, 2, 0},
    {16, 64, 0, 100},
    {32, 64, 0, 0},
    /* 8: style 7 v2 n1, loop 32: a gate longer than the loop retriggers */
    {0, 640, 0, 0},
    /* 9: style 8 v1 n4, loop 96: strums on the beat and on the off-beat sixteenth */
    {0, 128, 0, 0}, {64, 128, 1, 0}, {128, 128, 2, 0}, {192, 128, 3, 0},
    {384, 128, 0, 0}, {448, 128, 1, 0}, {512, 128, 2, 0}, {576, 128, 3, 0},
};
static j6_phrase_bank_t TB = {TB_LISTS, TB_POOL, (uint16_t)(sizeof TB_POOL / sizeof TB_POOL[0])};

static void tb_setup(void)
{
    memset(TB_LISTS, 0, sizeof TB_LISTS);
    TB_LISTS[j6_plist_index(6, 1, 2)] = (j6_plist_t){96, 0, 4, J6_PL_CAPTURED, 0};
    TB_LISTS[j6_plist_index(6, 1, 1)] = (j6_plist_t){48, 4, 1, 0, 0};
    TB_LISTS[j6_plist_index(7, 1, 1)] = (j6_plist_t){96, 5, 3, 0, 0};
    TB_LISTS[j6_plist_index(7, 2, 1)] = (j6_plist_t){32, 8, 1, 0, 0};
    TB_LISTS[j6_plist_index(8, 1, 4)] = (j6_plist_t){96, 9, 8, 0, 0};
    TB_LISTS[j6_plist_index(9, 1, 1)] = (j6_plist_t){96, 9, 200, 0, 0};    /* out of the pool: ignored */
}

static void test_bank_custom(void)
{
    static const uint8_t ce[] = {60, 64}, d[] = {62};
    uint32_t k, i;
    tb_setup();
    ph_init(6, 1);
    j6_phrase_set_bank(&P, &TB);
    j6_phrase_set_vel(&P, 90, 120);
    ph_held(ce, 2);                                        /* n = 2: list 0 */
    ph_run(0, 192);
    k = notes_in(NT, NT_MAX, 0, 96u * J6_SUB);
    CHECK_EQ(k, 4);
    CHECK_EQ(NT[0].note, 60);
    CHECK_EQ(NT[0].vel, 90);
    CHECK_EQ(NT[0].off, 160u);
    CHECK_EQ(NT[1].note, 76);                              /* H[1] + 12, accent -> A.vLo */
    CHECK_EQ(NT[1].vel, 120);
    CHECK_EQ(NT[2].note, 52);                              /* H[1] - 12 at sub-tick 40 */
    CHECK_EQ(NT[2].on, 40u);
    CHECK_EQ(NT[2].off, 88u);
    CHECK_EQ(NT[3].note, 60);
    CHECK_EQ(NT[3].on, 1500u);
    CHECK_EQ(NT[3].off, 1516u);
    k = notes_in(NT, NT_MAX, 96u * J6_SUB, 192u * J6_SUB);  /* the loop wraps */
    CHECK_EQ(k, 4);
    CHECK_EQ(NT[0].on, 96u * J6_SUB);
    CHECK_EQ(NT[3].on, 96u * J6_SUB + 1500u);
    CHECK_EQ(j6_phrase_loop_ticks(&TB, 6, 1, 2), 96);
    CHECK_EQ(j6_phrase_list_flags(&TB, 6, 1, 2), J6_PL_CAPTURED);
    CHECK_EQ(j6_phrase_list_flags(&TB, 6, 1, 1), 0);
    ph_finish();
    /* n = 1: list 4, another loop */
    ph_init(6, 1);
    j6_phrase_set_bank(&P, &TB);
    ph_held(d, 1);
    ph_run(0, 96);
    k = notes_in(NT, NT_MAX, 0, 96u * J6_SUB);
    CHECK_EQ(k, 2);
    CHECK_EQ(NT[0].note, 86);
    CHECK_EQ(NT[0].on, 8u);
    CHECK_EQ(NT[1].on, 48u * J6_SUB + 8u);
    ph_finish();
    /* skipped events */
    ph_init(7, 1);
    j6_phrase_set_bank(&P, &TB);
    ph_held(d, 1);
    ph_run(0, 96);
    k = notes_in(NT, NT_MAX, 0, 96u * J6_SUB);
    CHECK_EQ(k, 1);
    CHECK_EQ(NT[0].on, 32u);
    ph_finish();
    /* empty list (not set) and a list outside the pool play nothing */
    for (i = 0; i < 2u; i++) {
        ph_init(i ? 9 : 8, 1);
        j6_phrase_set_bank(&P, &TB);
        ph_held(d, 1);
        ph_run(0, 200);
        CHECK_EQ(LOG.n, 0);
        ph_finish();
    }
    CHECK_EQ(j6_phrase_loop_ticks(&TB, 8, 1, 1), 0);
    /* a gate longer than the loop: the next hit retriggers, NOTE_OFF before NOTE_ON */
    ph_init(7, 2);
    j6_phrase_set_bank(&P, &TB);
    ph_held(d, 1);
    ph_run(0, 32);
    ph_tick(32);
    CHECK_EQ(Q.n, 2);
    CHECK_EQ(Q.ev[0].type, J6_EV_NOTE_OFF);
    CHECK_EQ(Q.ev[1].type, J6_EV_NOTE_ON);
    CHECK_EQ(Q.ev[0].note, 62);
    CHECK_EQ(Q.ev[1].note, 62);
    CHECK_EQ(j6_phrase_sounding(&P), 1);
    ph_run(33, 100);
    ph_finish();
}

/* more notes in one tick than an instance can start: capped, balanced, nothing lost */
static void test_bank_overload(void)
{
    static j6_pev_t pool[40];
    static j6_plist_t lists[J6_PLISTS];
    j6_phrase_bank_t b = {lists, pool, 40};
    static const uint8_t lo[] = {20};
    uint32_t i, k;
    memset(lists, 0, sizeof lists);
    for (i = 0; i < 40u; i++) {
        pool[i].t = (uint16_t)(i < 20u ? 0u : 8u * J6_SUB);    /* 20 at tick 0, 20 at tick 8 */
        pool[i].len = 60u * J6_SUB;
        pool[i].src = 0;
        pool[i].semi = (int8_t)i;
    }
    lists[j6_plist_index(6, 3, 1)] = (j6_plist_t){96, 0, 40, 0, 0};
    ph_init(6, 3);
    j6_phrase_set_bank(&P, &b);
    ph_held(lo, 1);
    ph_tick(0);
    CHECK_EQ(j6t_evq_count(&Q, J6_EV_NOTE_ON), 16);        /* J6P_TICK_ONS_MAX */
    CHECK_EQ(j6_phrase_sounding(&P), J6P_ACTIVE_MAX);
    ph_run(1, 8);
    CHECK_EQ(j6t_evq_count(&Q, J6_EV_NOTE_ON), 16);        /* tick 8: the full act[] steals */
    CHECK_EQ(j6t_evq_count(&Q, J6_EV_NOTE_OFF), 16);
    CHECK_EQ(j6_phrase_sounding(&P), J6P_ACTIVE_MAX);
    ph_run(9, 300);
    k = notes_of(NT, NT_MAX);
    CHECK(k > 0);
    ph_finish();
}

static void test_builtin_sanity(void)
{
    const j6_phrase_bank_t *b = &j6_phrase_builtin;
    uint32_t s, v, n, i, events = 0;
    CHECK(b->lists != NULL && b->pool != NULL);
    for (s = 6; s <= 9u; s++)
        for (v = 1; v <= 12u; v++)
            for (n = 1; n <= 4u; n++) {
                const j6_plist_t *l = &b->lists[j6_plist_index(s, v, n)];
                CHECK(l->loop == 192u || l->loop == 288u || l->loop == 384u || l->loop == 576u || l->loop == 768u);
                CHECK(l->count > 0);
                CHECK((uint32_t)l->first + l->count <= b->pool_len);
                CHECK_EQ(l->flags, J6_PL_CAPTURED);
                CHECK_EQ(j6_phrase_list_flags(NULL, s, v, n), J6_PL_CAPTURED);
                CHECK_EQ(j6_phrase_loop_ticks(NULL, s, v, n), l->loop);
                events += l->count;
                for (i = 0; i < l->count && (uint32_t)l->first + i < b->pool_len; i++) {
                    const j6_pev_t *e = &b->pool[l->first + i];
                    CHECK(e->t < l->loop * J6_SUB);
                    CHECK(i == 0 || e->t >= e[-1].t);                    /* sorted */
                    CHECK((e->src & J6_PEV_SRC_MASK) < n);
                    CHECK_EQ(e->src & 0x7Cu, 0);
                    CHECK_EQ(e->semi, 0);                                /* only the held tones */
                    CHECK_EQ(e->t % (J6_MIDI_CLOCK_TICKS * J6_SUB), 0);  /* on the 4-tick grid */
                    CHECK(e->len >= J6_SUB && e->len % J6_SUB == 0);
                    CHECK(e->len < l->loop * J6_SUB);
                }
            }
    CHECK_EQ(events, b->pool_len);
    CHECK_EQ(events, 2273);
    CHECK_EQ(j6_phrase_list_flags(NULL, 3, 1, 1), 0);
    CHECK_EQ(j6_phrase_list_flags(NULL, 6, 1, 0), 0);
    CHECK_EQ(j6_phrase_loop_ticks(NULL, 6, 1, 0), 0);
    CHECK_EQ(j6_phrase_loop_ticks(NULL, 0, 1, 1), 0);
    CHECK_EQ(j6_phrase_loop_ticks(NULL, 10, 1, 1), 0);
    CHECK_EQ(j6_phrase_loop_ticks(NULL, 6, 13, 1), 0);
}

/* ---- data/phrases_6_9.json: a small JSON reader (host only) --------------------------------------- */

typedef struct {
    int t, len, tone, oct, acc;
} jn_t;
typedef struct {
    int present, loop, count;
    jn_t note[64];
} jl_t;
static jl_t JL[4][12][4];
static int JHELD[5][4], JHELD_N[5];
static const char *jp;
static int jerr;

static void jws(void)
{
    while (*jp == ' ' || *jp == '\n' || *jp == '\r' || *jp == '\t') jp++;
}
static int jlit(char c)
{
    jws();
    if (*jp == c) {
        jp++;
        return 1;
    }
    return 0;
}
static void jstr(char *buf, size_t cap)
{
    size_t k = 0;
    jws();
    if (*jp != '"') {
        jerr = 1;
        buf[0] = 0;
        return;
    }
    jp++;
    while (*jp && *jp != '"') {
        if (*jp == '\\' && jp[1]) jp++;
        if (k + 1 < cap) buf[k++] = *jp;
        jp++;
    }
    buf[k] = 0;
    if (*jp == '"') jp++;
    else jerr = 1;
}
static long jint(void)
{
    char *end;
    double v;
    jws();
    v = strtod(jp, &end);
    if (end == jp) jerr = 1;
    jp = end;
    return (long)(v < 0 ? v - 0.5 : v + 0.5);
}
static void jskip(void)
{
    char k[256];
    jws();
    if (*jp == '{') {
        jp++;
        if (jlit('}')) return;
        do {
            jstr(k, sizeof k);
            if (!jlit(':')) jerr = 1;
            jskip();
        } while (!jerr && jlit(','));
        if (!jlit('}')) jerr = 1;
    } else if (*jp == '[') {
        jp++;
        if (jlit(']')) return;
        do jskip();
        while (!jerr && jlit(','));
        if (!jlit(']')) jerr = 1;
    } else if (*jp == '"') {
        jstr(k, sizeof k);
    } else if (!strncmp(jp, "true", 4) || !strncmp(jp, "null", 4)) {
        jp += 4;
    } else if (!strncmp(jp, "false", 5)) {
        jp += 5;
    } else {
        (void)jint();
    }
}
/* object members: calls fn(key, ctx) with jp at each value; fn must consume the value */
static void jobj(void (*fn)(const char *key, void *ctx), void *ctx)
{
    char key[64];
    if (!jlit('{')) {
        jerr = 1;
        return;
    }
    if (jlit('}')) return;
    do {
        jstr(key, sizeof key);
        if (!jlit(':')) {
            jerr = 1;
            return;
        }
        fn(key, ctx);
    } while (!jerr && jlit(','));
    if (!jlit('}')) jerr = 1;
}
static void j_note(const char *key, void *ctx)
{
    jn_t *n = (jn_t *)ctx;
    if (!strcmp(key, "t")) n->t = (int)jint();
    else if (!strcmp(key, "len")) n->len = (int)jint();
    else if (!strcmp(key, "tone")) n->tone = (int)jint();
    else if (!strcmp(key, "oct")) n->oct = (int)jint();
    else if (!strcmp(key, "acc")) n->acc = (int)jint();
    else jskip();
}
static void j_list(const char *key, void *ctx)
{
    jl_t *l = (jl_t *)ctx;
    if (!strcmp(key, "loop")) {
        l->loop = (int)jint();
    } else if (!strcmp(key, "notes")) {
        if (!jlit('[')) {
            jerr = 1;
            return;
        }
        if (jlit(']')) return;
        do {
            jn_t tmp;
            memset(&tmp, 0, sizeof tmp);
            jobj(j_note, &tmp);
            if (l->count < 64) l->note[l->count++] = tmp;
            else jerr = 1;
        } while (!jerr && jlit(','));
        if (!jlit(']')) jerr = 1;
    } else {
        jskip();
    }
}
static void j_var(const char *key, void *ctx)
{
    jl_t *row = (jl_t *)ctx;
    int n = atoi(key);
    if (n >= 1 && n <= 4) {
        row[n - 1].present = 1;
        jobj(j_list, &row[n - 1]);
    } else {
        jskip();
    }
}
static void j_phrases(const char *key, void *ctx)
{
    int s, v;
    (void)ctx;
    if (sscanf(key, "s%dv%d", &s, &v) == 2 && s >= 6 && s <= 9 && v >= 1 && v <= 12) jobj(j_var, JL[s - 6][v - 1]);
    else jskip();
}
static void j_held(const char *key, void *ctx)
{
    int n = atoi(key);
    (void)ctx;
    if (n < 1 || n > 4 || !jlit('[')) {
        jskip();
        return;
    }
    if (jlit(']')) return;
    do {
        long x = jint();
        if (JHELD_N[n] < 4) JHELD[n][JHELD_N[n]++] = (int)x;
    } while (!jerr && jlit(','));
    if (!jlit(']')) jerr = 1;
}
static void j_top(const char *key, void *ctx)
{
    (void)ctx;
    if (!strcmp(key, "phrases")) jobj(j_phrases, NULL);
    else if (!strcmp(key, "held")) jobj(j_held, NULL);
    else jskip();
}

static int JSON_OK = -1;
static int json_load(void)
{
    FILE *f;
    long sz;
    char *buf;
    if (JSON_OK >= 0) return JSON_OK;
    JSON_OK = 0;
    f = fopen("data/phrases_6_9.json", "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)sz + 1u);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        free(buf);
        return 0;
    }
    fclose(f);
    buf[sz] = 0;
    memset(JL, 0, sizeof JL);
    jp = buf;
    jerr = 0;
    jobj(j_top, NULL);
    free(buf);
    JSON_OK = !jerr;
    return JSON_OK;
}

static int jn_cmp(const void *a, const void *b)
{
    const jn_t *x = (const jn_t *)a, *y = (const jn_t *)b;
    if (x->t != y->t) return x->t < y->t ? -1 : 1;
    return x->tone - y->tone;
}

/* play list (s, v, n) from the builtin bank for two loops and compare with the JSON, note for note */
static void check_list_vs_json(uint32_t s, uint32_t v, uint32_t n)
{
    jl_t *l = &JL[s - 6u][v - 1u][n - 1u];
    jn_t exp[64];
    uint32_t i, k, L;
    CHECK(l->present);
    if (!l->present) return;
    L = (uint32_t)l->loop;
    memcpy(exp, l->note, sizeof exp[0] * (size_t)l->count);
    qsort(exp, (size_t)l->count, sizeof exp[0], jn_cmp);
    ph_init(s, v);
    j6_phrase_set_vel(&P, 64, 120);                        /* vELo and A.vLo told apart */
    ph_held(HN[n], n);
    ph_run(0, 2u * L);
    CHECK_EQ(j6_phrase_loop_ticks(NULL, s, v, n), L);
    k = notes_in(NT, NT_MAX, 0, L * J6_SUB);
    CHECK_EQ(k, (uint32_t)l->count);
    for (i = 0; i < k && i < (uint32_t)l->count; i++) {
        CHECK_EQ(NT[i].on, (uint32_t)exp[i].t * J6_SUB);                       /* exact onset tick */
        CHECK_EQ(NT[i].off - NT[i].on, (uint32_t)exp[i].len * J6_SUB);         /* exact gate */
        CHECK_EQ(NT[i].note, (uint32_t)JHELD[n][exp[i].tone] + 12u * (uint32_t)exp[i].oct);  /* tone */
        CHECK_EQ(NT[i].vel, exp[i].acc ? 120 : 64);                             /* accent: A.vLo */
    }
    k = notes_in(NT, NT_MAX, L * J6_SUB, 2u * L * J6_SUB);  /* the next loop is the same */
    CHECK_EQ(k, (uint32_t)l->count);
    for (i = 0; i < k && i < (uint32_t)l->count; i++) {
        CHECK_EQ(NT[i].on, ((uint32_t)exp[i].t + L) * J6_SUB);
        CHECK_EQ(NT[i].note, (uint32_t)JHELD[n][exp[i].tone]);
    }
    ph_finish();
}

static void test_builtin_vs_json(void)
{
    uint32_t s, v, n, i;
    CHECK(json_load());
    if (JSON_OK != 1) return;
    for (n = 1; n <= 4u; n++) {                            /* the JSON's held sets are the capture's */
        CHECK_EQ(JHELD_N[n], (int)n);
        for (i = 0; i < n; i++) CHECK_EQ(JHELD[n][i], HN[n][i]);
    }
    for (s = 6; s <= 9u; s++)
        for (v = 1; v <= 12u; v++)
            for (n = 1; n <= 4u; n++) check_list_vs_json(s, v, n);
}

/* spot checks with the numbers written out (from data/phrases_6_9.json) */
static void test_captured_spot_lists(void)
{
    uint32_t k, i;
    /* s6v01 n4: loop 768 (2 bars); a chord, then single notes */
    ph_init(6, 1);
    ph_held(HN[4], 4);
    ph_run(0, 768);
    k = notes_in(NT, NT_MAX, 0, 768u * J6_SUB);
    CHECK_EQ(k, 12);
    for (i = 0; i < 4u; i++) {
        CHECK_EQ(NT[i].on, 0);
        CHECK_EQ(NT[i].note, HN[4][i]);
        CHECK_EQ(NT[i].off, 96u * J6_SUB);
        CHECK_EQ(NT[i].vel, 100);
    }
    CHECK_EQ(NT[4].on, 96u * J6_SUB);
    CHECK_EQ(NT[4].note, 64);
    CHECK_EQ(NT[4].off, 144u * J6_SUB);
    CHECK_EQ(NT[5].on, 144u * J6_SUB);
    CHECK_EQ(NT[5].note, 60);
    CHECK_EQ(NT[5].off, 288u * J6_SUB);
    CHECK_EQ(NT[6].on, 384u * J6_SUB);
    CHECK_EQ(NT[10].on, 480u * J6_SUB);
    CHECK_EQ(NT[10].note, 60);
    CHECK_EQ(NT[11].on, 528u * J6_SUB);
    CHECK_EQ(NT[11].note, 64);
    CHECK_EQ(NT[11].off, 672u * J6_SUB);
    CHECK_EQ(j6_phrase_loop_ticks(NULL, 6, 1, 4), 768);
    ph_finish();
    /* s9v08 n4: loop 384; an accented downward strum B G E C 4 ticks apart, all ending at tick 16 */
    ph_init(9, 8);
    ph_held(HN[4], 4);
    ph_run(0, 384);
    k = notes_in(NT, NT_MAX, 0, 384u * J6_SUB);
    CHECK_EQ(k, 32);
    for (i = 0; i < 4u; i++) {
        CHECK_EQ(NT[i].on, 4u * i * J6_SUB);
        CHECK_EQ(NT[i].note, HN[4][3u - i]);
        CHECK_EQ(NT[i].off, 16u * J6_SUB);
        CHECK_EQ(NT[i].vel, 127);
    }
    for (i = 4; i < 8u; i++) {                             /* block chord at 32, not accented */
        CHECK_EQ(NT[i].on, 32u * J6_SUB);
        CHECK_EQ(NT[i].note, HN[4][i - 4u]);
        CHECK_EQ(NT[i].off, 48u * J6_SUB);
        CHECK_EQ(NT[i].vel, 100);
    }
    CHECK_EQ(NT[12].on, 128u * J6_SUB);                    /* the third strum */
    CHECK_EQ(NT[16].on, 192u * J6_SUB);                    /* the long strum at beat 3 */
    CHECK_EQ(NT[16].off, 288u * J6_SUB);
    CHECK_EQ(NT[19].on, 204u * J6_SUB);
    CHECK_EQ(NT[19].note, 60);
    CHECK_EQ(NT[19].off, 288u * J6_SUB);
    CHECK_EQ(NT[20].on, 288u * J6_SUB);                    /* upward strum C E G B, plain */
    CHECK_EQ(NT[20].note, 60);
    CHECK_EQ(NT[23].on, 300u * J6_SUB);
    CHECK_EQ(NT[23].note, 71);
    CHECK_EQ(NT[23].vel, 100);
    ph_finish();
    /* s7v05 n1: one note per bar, 372 ticks long */
    ph_init(7, 5);
    ph_held(HN[1], 1);
    ph_run(0, 2u * 384u);
    k = notes_in(NT, NT_MAX, 0, 2u * 384u * J6_SUB);
    CHECK_EQ(k, 2);
    CHECK_EQ(NT[0].on, 0);
    CHECK_EQ(NT[0].note, 60);
    CHECK_EQ(NT[0].off, 372u * J6_SUB);
    CHECK_EQ(NT[1].on, 384u * J6_SUB);
    CHECK_EQ(NT[1].off, 756u * J6_SUB);
    ph_finish();
}

/* the J-6 capture's facts about styles 6-9 */
static void test_captured_facts(void)
{
    uint32_t k, i, s, v;
    /* the phrase starts at the press: H set after 37 idle ticks, pt starts at 0 there (PH-5) and the
       first events sound in that very tick, not at a bar line */
    ph_init(9, 8);
    ph_run(5000, 37);
    CHECK_EQ(LOG.n, 0);
    ph_held(HN[4], 4);
    ph_tick(0);
    CHECK_EQ(j6t_evq_count(&Q, J6_EV_NOTE_ON), 1);
    CHECK_EQ(Q.ev[0].note, 71);
    CHECK_EQ(Q.ev[0].sub, 0);
    ph_run(1, 205);                                        /* into the long strum at 192..204 (until 288) */
    CHECK_EQ(j6_phrase_sounding(&P), 4);
    /* it stops the instant all keys are released: every note cut now, no note afterwards */
    ph_held(NULL, 0);
    CHECK_EQ(j6t_evq_count(&Q, J6_EV_NOTE_OFF), 4);
    CHECK_EQ(j6t_evq_count(&Q, J6_EV_NOTE_ON), 0);
    k = LOG.n;
    ph_run(206, 1000);
    CHECK_EQ(LOG.n, k);
    ph_finish();
    /* only two velocities (vELo, A.vLo), only the held tones, strums on the 4-tick grid */
    for (s = 6; s <= 9u; s++)
        for (v = 1; v <= 12u; v += 11u) {
            ph_init(s, v);
            j6_phrase_set_vel(&P, 77, 127);
            ph_held(HN[4], 4);
            ph_run(0, 768);
            k = notes_of(NT, NT_MAX);
            CHECK(k > 0);
            for (i = 0; i < k; i++) {
                CHECK(NT[i].vel == 77 || NT[i].vel == 127);
                CHECK(NT[i].note == 60 || NT[i].note == 64 || NT[i].note == 67 || NT[i].note == 71);
                CHECK_EQ(NT[i].on % (J6_MIDI_CLOCK_TICKS * J6_SUB), 0);
            }
            ph_finish();
        }
}

/* ======================================================================================================
 * shuffle (SQ-7)
 * ==================================================================================================== */

static void check_shuffle_16ths(int32_t sh)
{
    static const uint8_t ceg[] = {60, 64, 67};
    int32_t off = sh * 384 / 100;
    uint32_t k, i;
    ph_init(2, 1);                                         /* 16ths, UP1 */
    j6_phrase_set_shuffle(&P, sh);
    ph_held(ceg, 3);
    ph_run(0, 24u * 13u);                                  /* the last gate ends inside the run */
    k = notes_in(NT, NT_MAX, 0, 24u * 12u * J6_SUB);
    CHECK_EQ(k, 12);
    for (i = 0; i < k; i++) {
        uint32_t nominal = i * 24u * J6_SUB;
        CHECK_EQ(NT[i].on, (i & 1u) ? (uint32_t)((int32_t)nominal + off) : nominal);   /* odd 16ths only */
        CHECK_EQ(NT[i].off - NT[i].on, j6_gate(24) * J6_SUB);                         /* gate kept */
        CHECK_EQ(NT[i].note, ceg[i % 3u]);                                            /* order kept */
    }
    ph_finish();
}

static void test_shuffle(void)
{
    static const uint8_t ceg[] = {60, 64, 67};
    static const struct {
        uint8_t style, var, div;
    } never[] = {{1, 7, 32}, {2, 7, 16}, {1, 1, 48}, {3, 11, 32}};
    uint32_t i, j, k;
    int32_t sh;
    check_shuffle_16ths(50);
    check_shuffle_16ths(-90);
    check_shuffle_16ths(90);
    /* -90: the second 16th fires 21.6 ticks early, in tick 2 at sub 7 */
    ph_init(2, 1);
    j6_phrase_set_shuffle(&P, -90);
    ph_held(ceg, 3);
    ph_run(0, 3);
    k = notes_in(NT, NT_MAX, 0, 3u * J6_SUB);
    CHECK_EQ(k, 2);
    CHECK_EQ(NT[1].on, 2u * J6_SUB + 7u);
    ph_finish();
    /* triplets and eighths never move */
    for (i = 0; i < sizeof never / sizeof never[0]; i++)
        for (sh = -90; sh <= 90; sh += 180) {
            ph_init(never[i].style, never[i].var);
            j6_phrase_set_shuffle(&P, sh);
            ph_held(ceg, 3);
            ph_run(0, 12u * never[i].div);
            k = notes_of(NT, NT_MAX);
            CHECK(k >= 12u);
            for (j = 0; j < k; j++) CHECK_EQ(NT[j].on % (never[i].div * J6_SUB), 0);
            ph_finish();
        }
    /* style 3 v6 (16ths, all of H) moves its off-beat hits */
    ph_init(3, 6);
    j6_phrase_set_shuffle(&P, 50);
    ph_held(ceg, 3);
    ph_run(0, 96);
    k = notes_in(NT, NT_MAX, 0, 96u * J6_SUB);
    CHECK_EQ(k, 12);
    CHECK_EQ(NT[3].on, 24u * J6_SUB + 192u);
    CHECK_EQ(NT[6].on, 48u * J6_SUB);
    ph_finish();
}

static int has_note(uint32_t k, uint32_t on, uint32_t note)
{
    uint32_t i;
    for (i = 0; i < k; i++)
        if (NT[i].on == on && NT[i].note == note) return 1;
    return 0;
}

/* a table strum on the off-beat sixteenth moves as one, keeping its spacing and order */
static void test_shuffle_table_strum(void)
{
    uint32_t k, i;
    tb_setup();
    ph_init(8, 1);
    j6_phrase_set_bank(&P, &TB);
    j6_phrase_set_shuffle(&P, 50);
    ph_held(HN[4], 4);
    ph_run(0, 96);
    k = notes_in(NT, NT_MAX, 0, 96u * J6_SUB);
    CHECK_EQ(k, 8);
    for (i = 0; i < 4u; i++) {
        CHECK_EQ(NT[i].on, i * 64u);                       /* the on-beat strum: not moved */
        CHECK_EQ(NT[i].note, HN[4][i]);
        CHECK_EQ(NT[4u + i].on, 384u + i * 64u + 192u);    /* the off-beat strum: +12 ticks, whole */
        CHECK_EQ(NT[4u + i].note, HN[4][i]);
        CHECK_EQ(NT[4u + i].off - NT[4u + i].on, 128u);
    }
    ph_finish();
    /* negative: the off-beat strum comes 21.6 ticks early, still in order */
    ph_init(8, 1);
    j6_phrase_set_bank(&P, &TB);
    j6_phrase_set_shuffle(&P, -90);
    ph_held(HN[4], 4);
    ph_run(0, 96);
    k = notes_in(NT, NT_MAX, 0, 96u * J6_SUB);
    CHECK_EQ(k, 8);
    for (i = 0; i < 4u; i++) {                             /* interleaved with the on-beat strum */
        CHECK(has_note(k, 384u + i * 64u - 345u, HN[4][i]));
        CHECK(has_note(k, i * 64u, HN[4][i]));
    }
    ph_finish();
}

/* the shuffle changed while the phrase plays (j6_phrase_set_shuffle between two ticks, as the glue does
 * when the value is edited): every hit plays exactly once, under the old or the new timing */
#define SC_N 8u                                            /* sixteenths checked */
static void check_shuffle_change(int32_t sh0, uint32_t c, int32_t sh1, int32_t sh2, const int32_t *odd)
{
    static const uint8_t ceg[] = {60, 64, 67};
    uint32_t k, i;
    ph_init(2, 1);                                         /* 16ths, UP1 */
    j6_phrase_set_shuffle(&P, sh0);
    ph_held(ceg, 3);
    ph_run(0, c);
    j6_phrase_set_shuffle(&P, sh1);
    ph_tick(c);
    j6_phrase_set_shuffle(&P, sh2);                        /* a second change while the first settles */
    ph_run(c + 1u, 24u * (SC_N + 1u) - c - 1u);
    k = notes_in(NT, NT_MAX, 0, 24u * SC_N * J6_SUB);
    CHECK_EQ(k, SC_N);
    for (i = 0; i < k && i < SC_N; i++) {
        uint32_t nominal = i * 24u * J6_SUB;
        CHECK_EQ(NT[i].on, (i & 1u) ? (uint32_t)((int32_t)nominal + odd[i / 2u]) : nominal);
        CHECK_EQ(NT[i].off - NT[i].on, j6_gate(24) * J6_SUB);
        CHECK_EQ(NT[i].note, ceg[i % 3u]);                 /* the arp order is unchanged */
    }
    ph_finish();
}

static void test_shuffle_change_mid_loop(void)
{
    /* offsets of the 4 off-beat sixteenths (16ths 1, 3, 5, 7) from their nominal onsets */
    static const int32_t a[] = {0, 345, 345, 345};         /* 0 -> +90 at pt 25: 16th 1 already played */
    static const int32_t b[] = {345, 0, 0, 0};             /* +90 -> 0 at pt 30: 16th 1 still due at 45.6 */
    static const int32_t c[] = {0, -345, -345, -345};      /* 0 -> -90 at pt 5: its new time 2.4 is past */
    static const int32_t d[] = {-345, 345, 345, 345};      /* -90 -> +90 at pt 10: played at 2.4 already */
    static const int32_t e[] = {345, 345, 345, 345};       /* +50 -> +90 at pt 30: not yet due, moves */
    static const int32_t f[] = {345, -345, -345, -345};    /* +90 -> -90 at pt 40: 45.6 kept, 16th 3 at 50.4 */
    static const int32_t g[] = {0, 192, 192, 192};         /* 0 -> +90 at 25, then +50: applied once the
                                                              first change has settled (pt 73) */
    static const uint8_t ceg[] = {60, 64, 67};
    uint32_t k, i;
    check_shuffle_change(0, 25, 90, 90, a);
    check_shuffle_change(90, 30, 0, 0, b);
    check_shuffle_change(0, 5, -90, -90, c);
    check_shuffle_change(-90, 10, 90, 90, d);
    check_shuffle_change(50, 30, 90, 90, e);
    check_shuffle_change(90, 40, -90, -90, f);
    check_shuffle_change(0, 25, 90, 50, g);
    /* BEAT (style 3 v6, 16ths of all of H): 3 notes per sixteenth, never doubled or dropped */
    for (i = 0; i < 2u; i++) {
        ph_init(3, 6);
        j6_phrase_set_shuffle(&P, i ? 90 : 0);
        ph_held(ceg, 3);
        ph_run(0, i ? 30u : 25u);
        j6_phrase_set_shuffle(&P, i ? 0 : 90);
        ph_run(i ? 30u : 25u, 96u + 24u - (i ? 30u : 25u));
        k = notes_in(NT, NT_MAX, 0, 96u * J6_SUB);
        CHECK_EQ(k, 12);
        CHECK_EQ(NT[3].on, i ? 24u * J6_SUB + 345u : 24u * J6_SUB);
        CHECK_EQ(NT[9].on, i ? 72u * J6_SUB : 72u * J6_SUB + 345u);
        ph_finish();
    }
    /* a table strum (style 8 v1, off-beat strum at 24, 28, 32, 36 ticks) never splits */
    tb_setup();
    for (i = 0; i < 3u; i++) {
        static const uint32_t cpt[3] = {26, 30, 10};
        static const int32_t sh0[3] = {0, 50, 0}, sh1[3] = {50, 0, 50}, first[3] = {0, 192, 192};
        uint32_t j;
        ph_init(8, 1);
        j6_phrase_set_bank(&P, &TB);
        j6_phrase_set_shuffle(&P, sh0[i]);
        ph_held(HN[4], 4);
        ph_run(0, cpt[i]);
        j6_phrase_set_shuffle(&P, sh1[i]);
        ph_run(cpt[i], 192u - cpt[i]);
        k = notes_in(NT, NT_MAX, 0, 192u * J6_SUB);
        CHECK_EQ(k, 16);
        for (j = 0; j < 4u; j++) {
            CHECK(has_note(k, 384u + j * 64u + (uint32_t)first[i], HN[4][j]));          /* loop 1: whole */
            CHECK(has_note(k, 96u * J6_SUB + 384u + j * 64u + (uint32_t)(sh1[i] * 384 / 100), HN[4][j]));
        }
        ph_finish();
    }
}

/* the builtin strums are short enough for the settling time (48 ticks + strum) to stay under a bar */
static void test_builtin_strum_span(void)
{
    const j6_phrase_bank_t *b = &j6_phrase_builtin;
    uint32_t i, j, mx = 0;
    for (i = 0; i < J6_PLISTS; i++) {
        const j6_plist_t *l = &b->lists[i];
        uint32_t a = 0;
        for (j = 0; j < l->count; j++) {
            const j6_pev_t *e = &b->pool[l->first + j];
            if (j == 0 || e->t > e[-1].t + 4u * J6_SUB) a = e->t;
            if (e->t - a > mx) mx = e->t - a;
        }
    }
    CHECK(mx <= 48u * J6_SUB);
}

/* ======================================================================================================
 * pt jumps, stop, invariants
 * ==================================================================================================== */

static void test_pt_jumps(void)
{
    static const uint8_t ceg[] = {60, 64, 67}, ce[] = {60, 64};
    uint32_t k, i;
    /* the pattern wraps from 3071 to 0: hits continue, the arp index carries on, gates intact */
    ph_init(2, 1);
    ph_held(ceg, 3);
    ph_run(3048, 24);
    ph_run(0, 70);
    k = notes_in(NT, NT_MAX, 0, 0xFFFFFFFFu);
    CHECK_EQ(k, 4);
    for (i = 0; i < k; i++) {
        CHECK_EQ(NT[i].on, i * 24u * J6_SUB);
        CHECK_EQ(NT[i].off - NT[i].on, j6_gate(24) * J6_SUB);
        CHECK_EQ(NT[i].note, ceg[i % 3u]);
    }
    ph_finish();
    /* a jump onto a hit while the previous hit sounds: NOTE_OFF then NOTE_ON (retrigger) */
    ph_init(3, 4);                                         /* 96 ticks, gate 86 */
    ph_held(ce, 2);
    ph_run(0, 150);                                        /* hits at 0 and 96 */
    ph_tick(0);                                            /* jump back to 0 at elapsed tick 150 */
    CHECK_EQ(Q.n, 4);
    CHECK(j6t_evq_find(&Q, 0, J6_EV_NOTE_OFF, 60) < j6t_evq_find(&Q, 0, J6_EV_NOTE_ON, 60));
    CHECK(j6t_evq_find(&Q, 0, J6_EV_NOTE_OFF, 64) < j6t_evq_find(&Q, 0, J6_EV_NOTE_ON, 64));
    ph_run(1, 100);
    k = notes_in(NT, NT_MAX, 0, 0xFFFFFFFFu);
    CHECK_EQ(k, 8);
    CHECK_EQ(NT[2].on, 96u * J6_SUB);
    CHECK_EQ(NT[2].off, 150u * J6_SUB);                    /* cut by the retrigger */
    CHECK_EQ(NT[4].on, 150u * J6_SUB);
    CHECK_EQ(NT[4].off, (150u + 86u) * J6_SUB);            /* a full gate in elapsed ticks */
    ph_finish();
    /* a jump to a position without a hit: the sounding note still ends after its gate (elapsed time) */
    ph_init(4, 1);                                         /* o___: 24 ticks, gate 20 */
    ph_held(ce, 2);
    ph_run(80, 20);                                        /* hit at pt 96 = elapsed 16 */
    ph_run(500, 40);
    k = notes_in(NT, NT_MAX, 0, 0xFFFFFFFFu);
    CHECK_EQ(k, 2);
    CHECK_EQ(NT[0].on, 16u * J6_SUB);
    CHECK_EQ(NT[0].off, 36u * J6_SUB);
    ph_finish();
}

static void test_stop_and_restart(void)
{
    uint32_t i;
    ph_init_src(6, 1, J6_SRC_PHRASE_SEQ);                  /* the sequencer's instance tags its events */
    ph_held(HN[4], 4);
    ph_run(0, 10);
    CHECK(j6_phrase_sounding(&P) > 0);
    j6_evq_clear(&Q);
    j6_phrase_stop(&P, &Q);
    check_q();
    j6t_log_add(&LOG, &Q, P.now);
    CHECK_EQ(j6t_evq_count(&Q, J6_EV_NOTE_OFF), Q.n);
    for (i = 0; i < Q.n; i++) CHECK_EQ(Q.ev[i].sub, 0);
    CHECK_EQ(P.n, 0);
    CHECK_EQ(j6_phrase_sounding(&P), 0);
    i = LOG.n;
    ph_run(10, 400);
    CHECK_EQ(LOG.n, i);
    ph_held(HN[4], 4);                                     /* H was forgotten: the same notes play again */
    ph_run(0, 400);
    CHECK(LOG.n > i);
    ph_finish();
    SRC = J6_SRC_PHRASE_LIVE;
}

/* a long mixed session: every style, set changes, shuffle, jumps; always balanced, never lost */
static void test_soak(void)
{
    static const uint8_t sets[6][4] = {{60, 64, 67, 71}, {62, 65}, {48}, {60, 64, 67, 71}, {0}, {57, 60, 64}};
    static const uint8_t nset[6] = {4, 2, 1, 4, 0, 3};
    uint32_t i, pt = 0, x = 12345u;
    ph_init(1, 1);
    for (i = 0; i < 20000u; i++) {
        if (LOG.n > J6T_LOG_MAX - 200u) LOG.n = 0;          /* DEPTH[] checks every event anyway */
        x = x * 1103515245u + 12345u;
        if ((x >> 16) % 97u == 0) j6_phrase_set_style(&P, 1u + (x >> 8) % 9u, 1u + (x >> 4) % 12u);
        if ((x >> 16) % 89u == 1) {
            uint32_t s = (x >> 12) % 6u;
            ph_held(sets[s], nset[s]);
        }
        if ((x >> 16) % 211u == 2) j6_phrase_set_shuffle(&P, (int32_t)((x >> 10) % 181u) - 90);
        if ((x >> 16) % 307u == 3) pt = (x >> 6) % 3072u;
        ph_tick(pt++);
        CHECK(j6_phrase_sounding(&P) <= J6P_ACTIVE_MAX);
    }
    ph_finish();
}

static void test_ram(void)
{
    printf("  ram: j6_phrase_t %u bytes (budget %u), j6_held_t %u bytes (budget %u)\n",
           (unsigned)sizeof(j6_phrase_t), (unsigned)J6_RAM_PHRASE_MAX, (unsigned)sizeof(j6_held_t),
           (unsigned)J6_RAM_HELD_MAX);
    CHECK(sizeof(j6_phrase_t) <= J6_RAM_PHRASE_MAX);
    CHECK(sizeof(j6_held_t) <= J6_RAM_HELD_MAX);
}

int main(void)
{
    RUN(test_held_poly_last4);
    RUN(test_held_down_overflow);
    RUN(test_held_latch_hd2);
    RUN(test_held_mono_ch6);
    RUN(test_set_held_normalise);
    RUN(test_defaults_and_setters);
    RUN(test_arp_ph12_cells);
    RUN(test_arp_divisions);
    RUN(test_arp_range2_off);
    RUN(test_arp_high_notes);
    RUN(test_arp_reset_ph13);
    RUN(test_arp_set_change);
    RUN(test_style3_values);
    RUN(test_style4_strings);
    RUN(test_style5_strings);
    RUN(test_empty_cuts_everything);
    RUN(test_late_join_style3);
    RUN(test_staggered_presses);
    RUN(test_late_join_rules);
    RUN(test_bank_custom);
    RUN(test_bank_overload);
    RUN(test_builtin_sanity);
    RUN(test_builtin_vs_json);
    RUN(test_captured_spot_lists);
    RUN(test_captured_facts);
    RUN(test_shuffle);
    RUN(test_shuffle_table_strum);
    RUN(test_shuffle_change_mid_loop);
    RUN(test_builtin_strum_span);
    RUN(test_pt_jumps);
    RUN(test_stop_and_restart);
    RUN(test_soak);
    RUN(test_ram);
    return j6t_report("phrase");
}
