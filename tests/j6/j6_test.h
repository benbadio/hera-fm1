/* SPDX-License-Identifier: GPL-3.0-only */
/* j6_test.h - a tiny shared helper for the J-6 engine host tests (tests/j6/test_*.c).
 *
 * Host only: tests may use stdio, stdlib and float; the engine (firmware/src/j6) may not.
 *
 * Convention for a test file tests/j6/test_<name>.c:
 *
 *     #include "j6_test.h"
 *     #include "j6_<name>_state.h"        (if the module has state)
 *
 *     static void test_something(void)
 *     {
 *         CHECK(j6_gate(24) == 20);
 *         CHECK_EQ(j6_beat_ticks(J6_BEAT_1_2), 48);
 *     }
 *
 *     int main(void)
 *     {
 *         RUN(test_something);
 *         return j6t_report("<name>");      (exit status 1 if any CHECK failed)
 *     }
 *
 * tests/j6/run_tests.sh builds it with every engine source (or, with J6_ISOLATED=1, with
 * firmware/src/j6/j6_<name>*.c only), runs it from the repository root and fails the run on a non-zero
 * exit status. Big instances (j6_seq_t is 35 KB) belong in static storage, not on the stack.
 * Files a test writes (WAV, dumps) go under build/j6test/. */
#ifndef J6_TEST_H
#define J6_TEST_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "j6.h"

static int j6t_failed;   /* failed checks */
static int j6t_checks;   /* checks run */
static const char *j6t_current = "";

#define CHECK(cond)                                                                              \
    do {                                                                                         \
        j6t_checks++;                                                                            \
        if (!(cond)) {                                                                           \
            j6t_failed++;                                                                        \
            fprintf(stderr, "%s:%d: %s: CHECK(%s) failed\n", __FILE__, __LINE__, j6t_current, #cond); \
        }                                                                                        \
    } while (0)

/* integers of any type up to 64 bits; prints both values on failure */
#define CHECK_EQ(a, b)                                                                           \
    do {                                                                                         \
        long long j6t_a_ = (long long)(a), j6t_b_ = (long long)(b);                              \
        j6t_checks++;                                                                            \
        if (j6t_a_ != j6t_b_) {                                                                  \
            j6t_failed++;                                                                        \
            fprintf(stderr, "%s:%d: %s: CHECK_EQ(%s, %s) failed: %lld != %lld\n", __FILE__, __LINE__, \
                    j6t_current, #a, #b, j6t_a_, j6t_b_);                                        \
        }                                                                                        \
    } while (0)

/* lo <= v <= hi */
#define CHECK_RANGE(v, lo, hi)                                                                   \
    do {                                                                                         \
        long long j6t_v_ = (long long)(v), j6t_lo_ = (long long)(lo), j6t_hi_ = (long long)(hi);   \
        j6t_checks++;                                                                            \
        if (j6t_v_ < j6t_lo_ || j6t_v_ > j6t_hi_) {                                              \
            j6t_failed++;                                                                        \
            fprintf(stderr, "%s:%d: %s: CHECK_RANGE(%s) failed: %lld not in [%lld, %lld]\n", __FILE__, \
                    __LINE__, j6t_current, #v, j6t_v_, j6t_lo_, j6t_hi_);                        \
        }                                                                                        \
    } while (0)

/* n bytes equal */
#define CHECK_MEM(a, b, n)                                                                       \
    do {                                                                                         \
        j6t_checks++;                                                                            \
        if (memcmp((a), (b), (n)) != 0) {                                                        \
            j6t_failed++;                                                                        \
            fprintf(stderr, "%s:%d: %s: CHECK_MEM(%s, %s, %s) failed\n", __FILE__, __LINE__,       \
                    j6t_current, #a, #b, #n);                                                    \
        }                                                                                        \
    } while (0)

#define CHECK_STR(a, b)                                                                          \
    do {                                                                                         \
        const char *j6t_sa_ = (a), *j6t_sb_ = (b);                                               \
        j6t_checks++;                                                                            \
        if (strcmp(j6t_sa_, j6t_sb_) != 0) {                                                     \
            j6t_failed++;                                                                        \
            fprintf(stderr, "%s:%d: %s: CHECK_STR(%s, %s) failed: \"%s\" != \"%s\"\n", __FILE__,   \
                    __LINE__, j6t_current, #a, #b, j6t_sa_, j6t_sb_);                            \
        }                                                                                        \
    } while (0)

/* run one test function; prints a line per test */
#define RUN(fn)                                                                                  \
    do {                                                                                         \
        int j6t_f0_ = j6t_failed;                                                                \
        j6t_current = #fn;                                                                       \
        fn();                                                                                    \
        printf("  %-40s %s\n", #fn, j6t_failed == j6t_f0_ ? "ok" : "FAILED");                     \
        j6t_current = "";                                                                        \
    } while (0)

/* the summary line and main()'s return value */
static inline int j6t_report(const char *suite)
{
    printf("%s: %d checks, %d failed\n", suite, j6t_checks, j6t_failed);
    return j6t_failed ? 1 : 0;
}

/* ---- event helpers ---- */

static inline const char *j6t_ev_name(uint32_t type)
{
    static const char *const n[] = {"NONE", "ON", "OFF", "HELD", "STEP", "LOOP", "PATTERN", "START",
                                    "STOP", "COUNTIN", "REC"};
    return type < sizeof n / sizeof n[0] ? n[type] : "?";
}

/* print a queue, one event per line, prefixed with a tick number (debugging) */
static inline void j6t_evq_dump(const j6_evq_t *q, uint32_t tick)
{
    uint32_t i;
    for (i = 0; i < q->n; i++) {
        const j6_event_t *e = &q->ev[i];
        printf("    t%u+%u %s src%u note%u vel%u arg%u\n", (unsigned)tick, (unsigned)e->sub,
               j6t_ev_name(e->type), (unsigned)e->src, (unsigned)e->note, (unsigned)e->vel,
               (unsigned)e->arg);
    }
}

/* number of events of a type in a queue */
static inline uint32_t j6t_evq_count(const j6_evq_t *q, uint32_t type)
{
    uint32_t i, k = 0;
    for (i = 0; i < q->n; i++)
        if (q->ev[i].type == type) k++;
    return k;
}

/* index of the first event of a type (and note, if note < 128) at or after `from`, -1 if none */
static inline int j6t_evq_find(const j6_evq_t *q, uint32_t from, uint32_t type, uint32_t note)
{
    uint32_t i;
    for (i = from; i < q->n; i++)
        if (q->ev[i].type == type && (note > 127u || q->ev[i].note == note)) return (int)i;
    return -1;
}

/* A timeline recorder: tests that tick a producer for a while append each tick's queue to it and then
 * look at absolute times. time = tick * 16 + sub. */
#define J6T_LOG_MAX 4096u
typedef struct {
    uint32_t time;                   /* tick * J6_SUB + sub */
    j6_event_t ev;
} j6t_logged_t;
typedef struct {
    j6t_logged_t e[J6T_LOG_MAX];
    uint32_t n;
    uint32_t overflow;
} j6t_log_t;

static inline void j6t_log_clear(j6t_log_t *l) { l->n = 0; l->overflow = 0; }
static inline void j6t_log_add(j6t_log_t *l, const j6_evq_t *q, uint32_t tick)
{
    uint32_t i;
    for (i = 0; i < q->n; i++) {
        if (l->n >= J6T_LOG_MAX) {
            l->overflow++;
            continue;
        }
        l->e[l->n].time = tick * J6_SUB + q->ev[i].sub;
        l->e[l->n].ev = q->ev[i];
        l->n++;
    }
}
/* the note-ons in order: notes[] and their times (tick*16+sub) - returns the count (at most max) */
static inline uint32_t j6t_log_note_ons(const j6t_log_t *l, uint8_t *notes, uint32_t *times, uint32_t max)
{
    uint32_t i, k = 0;
    for (i = 0; i < l->n && k < max; i++)
        if (l->e[i].ev.type == J6_EV_NOTE_ON) {
            if (notes) notes[k] = l->e[i].ev.note;
            if (times) times[k] = l->e[i].time;
            k++;
        }
    return k;
}
/* 1 if every NOTE_ON in the log has a later NOTE_OFF of the same note (no stuck notes) */
static inline int j6t_log_balanced(const j6t_log_t *l)
{
    int depth[128];
    uint32_t i;
    memset(depth, 0, sizeof depth);
    for (i = 0; i < l->n; i++) {
        const j6_event_t *e = &l->e[i].ev;
        if (e->note > 127u) continue;
        if (e->type == J6_EV_NOTE_ON) depth[e->note]++;
        if (e->type == J6_EV_NOTE_OFF && depth[e->note] > 0) depth[e->note]--;
    }
    for (i = 0; i < 128u; i++)
        if (depth[i]) return 0;
    return 1;
}

/* ---- audio helpers (synth tests) ---- */

/* write interleaved stereo int32 Q15 frames as a 16-bit WAV (for listening); 0 = written */
static inline int j6t_wav_write(const char *path, const int32_t *lr, uint32_t frames)
{
    FILE *f = fopen(path, "wb");
    uint32_t i, data = frames * 4u, v32;
    uint16_t v16;
    if (!f) return -1;
#define J6T_W32(x) (v32 = (uint32_t)(x), fwrite(&v32, 4, 1, f))
#define J6T_W16(x) (v16 = (uint16_t)(x), fwrite(&v16, 2, 1, f))
    fwrite("RIFF", 1, 4, f);
    J6T_W32(36u + data);
    fwrite("WAVEfmt ", 1, 8, f);
    J6T_W32(16);
    J6T_W16(1);
    J6T_W16(2);
    J6T_W32(J6_FS);
    J6T_W32(J6_FS * 4u);
    J6T_W16(4);
    J6T_W16(16);
    fwrite("data", 1, 4, f);
    J6T_W32(data);
    for (i = 0; i < frames * 2u; i++) {
        int32_t s = lr[i] > 32767 ? 32767 : lr[i] < -32768 ? -32768 : lr[i];
        J6T_W16((int16_t)s);
    }
#undef J6T_W32
#undef J6T_W16
    return fclose(f) ? -1 : 0;
}

/* peak |sample| and RMS (as an integer) of interleaved frames */
static inline int32_t j6t_peak(const int32_t *lr, uint32_t frames)
{
    uint32_t i;
    int32_t p = 0;
    for (i = 0; i < frames * 2u; i++) {
        int32_t a = lr[i] < 0 ? -lr[i] : lr[i];
        if (a > p) p = a;
    }
    return p;
}
static inline int32_t j6t_rms(const int32_t *lr, uint32_t frames)
{
    uint32_t i;
    double acc = 0.0;
    if (!frames) return 0;
    for (i = 0; i < frames * 2u; i++) acc += (double)lr[i] * (double)lr[i];
    acc /= (double)(frames * 2u);
    {
        double r = acc, x = acc > 1.0 ? acc : 1.0;  /* Newton sqrt: no libm needed */
        int k;
        for (k = 0; k < 40; k++) x = 0.5 * (x + r / x);
        return (int32_t)x;
    }
}

#endif /* J6_TEST_H */
