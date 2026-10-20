/* SPDX-License-Identifier: GPL-3.0-only */
/* j6_seq_clock.c - seq module: audio frames or MIDI clock in -> ticks.
 *
 * Design: docs/J6_ENGINE_DESIGN.md 2.2. Behaviour: C-5, MI-8 and section 10 of docs/J6_SPEC.md.
 *
 * Internal clock: acc_q12 += frames << 12; every time it reaches spt_q12 = j6_spt_q12(tempo10) a tick
 * begins. At 120.0 BPM spt_q12 is 940800 (229.6875 frames, exact), so 44100 frames hold exactly 192 ticks.
 * A tick "begins" when the accumulator crosses spt_q12: the tick in progress at init is not counted.
 *
 * External clock: every j6_clock_ext_pulse() is worth J6_MIDI_CLOCK_TICKS (4) ticks. The first begins at
 * the pulse (it is released by the next j6_clock_advance()); the other 3 follow at the measured tick
 * length, the average of the last 8 pulse intervals divided by 4. A pulse that comes before the 3 are out
 * releases the rest at once, so the tick count never drifts from 4 per pulse. When all 4 are out the clock
 * waits for the next pulse. An interval of J6_EXT_CLOCK_TIMEOUT_MS or more (a stopped clock) is not
 * averaged: it clears the measured intervals, so a sender that restarts at another tempo is measured
 * afresh (until its second pulse the internal tick length applies). j6_clock_ext_alive() reports the
 * timeout and the glue falls back to the internal clock.
 *
 * Only 32-bit arithmetic: the accumulator is fed at most J6S_CHUNK frames at a time, so frames << 12
 * stays below 2^24, and the measured tick length is at most 22050 << 10 (< 2^25). */
#include "j6_seq_state.h"

#define J6S_CHUNK 4096u                                                     /* frames per accumulator step */
#define J6S_TIMEOUT_FRAMES (J6_FS * J6_EXT_CLOCK_TIMEOUT_MS / 1000u)                 /* 22050 */
#define J6S_SPT_MIN 4096u                                                   /* a measured tick lasts >= 1 frame */

static uint32_t j6s_tempo_clamp(uint32_t t)
{
    return t < J6_TEMPO10_MIN ? J6_TEMPO10_MIN : t > J6_TEMPO10_MAX ? J6_TEMPO10_MAX : t;
}

/* the tick length the clock runs on now */
static uint32_t j6s_spt(const j6_clock_t *c)
{
    if (c->ext && c->ext_n) return c->ext_spt_q12;
    return c->spt_q12;
}

/* acc scaled from a tick of `from` to a tick of `to` (the same phase), 18-bit phase, 32-bit maths:
 * acc < from < 2^22, so acc << 9 and the remainder << 9 stay below 2^31 */
static uint32_t j6s_rescale(uint32_t acc, uint32_t from, uint32_t to)
{
    uint32_t q1, r, q2, ph;
    if (!from || !to) return 0;
    if (acc >= from) acc = from - 1u;
    q1 = (acc << 9) / from;
    r = (acc << 9) % from;
    q2 = (r << 9) / from;
    ph = (q1 << 9) | q2;                                     /* phase in 1/2^18 of a tick */
    /* ph * to >> 18 without 64 bits: to = hi * 512 + lo */
    acc = ((ph * (to >> 9)) >> 9) + ((ph * (to & 511u)) >> 18);
    return acc < to ? acc : to - 1u;
}

void j6_clock_init(j6_clock_t *c, uint32_t tempo10)
{
    uint32_t i;
    if (!c) return;
    c->tempo10 = (uint16_t)j6s_tempo_clamp(tempo10);
    c->spt_q12 = j6_spt_q12(c->tempo10);
    c->acc_q12 = 0;
    c->ticks = 0;
    c->frames = 0;
    c->ext_spt_q12 = 0;
    c->ext_last = 0;
    c->ext_due = 0;
    for (i = 0; i < J6S_EXT_AVG; i++) c->ext_iv[i] = 0;
    c->ext = 0;
    c->ext_owed = 0;
    c->ext_n = 0;
    c->ext_i = 0;
    c->ext_seen = 0;
    c->reserved = 0;
}

/* the position inside the current tick is kept (j6_clock_sub() gives the same value) */
void j6_clock_set_tempo(j6_clock_t *c, uint32_t tempo10)
{
    uint32_t spt;
    if (!c) return;
    tempo10 = j6s_tempo_clamp(tempo10);
    spt = j6_spt_q12(tempo10);
    if (!c->ext) c->acc_q12 = j6s_rescale(c->acc_q12, c->spt_q12, spt);
    c->spt_q12 = spt;
    c->tempo10 = (uint16_t)tempo10;
}

uint32_t j6_clock_advance(j6_clock_t *c, uint32_t frames)
{
    uint32_t n = 0, spt, ch;
    if (!c) return 0;
    c->frames += frames;
    spt = j6s_spt(c);
    if (spt < J6S_SPT_MIN) spt = J6S_SPT_MIN;
    if (c->ext) {
        n = c->ext_due;                  /* each pulse's first tick, and the catch-up */
        c->ext_due = 0;
        while (frames) {
            ch = frames < J6S_CHUNK ? frames : J6S_CHUNK;
            frames -= ch;
            c->acc_q12 += ch << 12;
            while (c->ext_owed && c->acc_q12 >= spt) {
                c->acc_q12 -= spt;
                c->ext_owed--;
                n++;
            }
            if (!c->ext_owed && c->acc_q12 >= spt) c->acc_q12 = spt - 1u;   /* wait for the next pulse */
        }
    } else {
        while (frames) {
            ch = frames < J6S_CHUNK ? frames : J6S_CHUNK;
            frames -= ch;
            c->acc_q12 += ch << 12;
            while (c->acc_q12 >= spt) {
                c->acc_q12 -= spt;
                n++;
            }
        }
    }
    c->ticks += n;
    return n;
}

uint32_t j6_clock_sub(const j6_clock_t *c)
{
    uint32_t spt, v;
    if (!c) return 0;
    spt = j6s_spt(c);
    if (spt < J6S_SPT_MIN) spt = J6S_SPT_MIN;
    v = (c->acc_q12 * J6_SUB) / spt;     /* acc < 2^26: fits */
    return v > J6_SUB - 1u ? J6_SUB - 1u : v;
}

uint32_t j6_clock_ticks(const j6_clock_t *c)
{
    return c ? c->ticks : 0u;
}

uint32_t j6_clock_spt_q12(const j6_clock_t *c)
{
    return c ? j6s_spt(c) : 0u;
}

uint32_t j6_clock_tempo10(const j6_clock_t *c)
{
    uint32_t spt;
    if (!c) return 0;
    if (!(c->ext && c->ext_n)) return c->tempo10;
    spt = c->ext_spt_q12 < J6S_SPT_MIN ? J6S_SPT_MIN : c->ext_spt_q12;
    return (J6_SPT_Q12_K + spt / 2u) / spt;
}

void j6_clock_ext(j6_clock_t *c, uint32_t on)
{
    if (!c) return;
    on = on ? 1u : 0u;
    if (on == c->ext) return;
    c->ext = (uint8_t)on;
    c->ext_owed = 0;
    c->ext_due = 0;
    c->acc_q12 = 0;                      /* internal: a full tick from now; external: wait for a pulse */
}

void j6_clock_ext_pulse(j6_clock_t *c)
{
    uint32_t iv, i, sum = 0;
    if (!c) return;
    if (c->ext_seen) {
        iv = c->frames - c->ext_last;
        if (iv < J6S_TIMEOUT_FRAMES) {   /* a gap of the timeout or more is a restart, not an interval */
            c->ext_iv[c->ext_i] = iv;
            c->ext_i = (uint8_t)((c->ext_i + 1u) % J6S_EXT_AVG);
            if (c->ext_n < J6S_EXT_AVG) c->ext_n++;
            for (i = 0; i < c->ext_n; i++) sum += c->ext_iv[i];
            /* frames per pulse * 4096 / 4 ticks = sum << 10 / n; sum <= 8 * 22050 */
            c->ext_spt_q12 = (sum << 10) / c->ext_n;
            if (c->ext_spt_q12 < J6S_SPT_MIN) c->ext_spt_q12 = J6S_SPT_MIN;
        } else {                         /* the sender stopped and restarted (maybe at another tempo):
                                            forget the old intervals, measure afresh */
            c->ext_n = 0;
            c->ext_i = 0;
        }
    }
    c->ext_seen = 1;
    c->ext_last = c->frames;
    if (c->ext) {
        c->ext_due += (uint32_t)c->ext_owed + 1u;   /* catch-up, then this pulse's first tick */
        c->ext_owed = (uint8_t)(J6_MIDI_CLOCK_TICKS - 1u);
        c->acc_q12 = 0;
    }
}

/* MI-8: a Start puts beat 1 on the first pulse after it, so the ticks the previous pulse still owes
 * (released or spread) are dropped; the measured interval is kept */
void j6_clock_ext_resync(j6_clock_t *c)
{
    if (!c) return;
    c->ext_owed = 0;
    c->ext_due = 0;
    c->acc_q12 = 0;
}

uint32_t j6_clock_ext_alive(const j6_clock_t *c)
{
    if (!c || !c->ext_seen) return 0;
    return (c->frames - c->ext_last) < J6S_TIMEOUT_FRAMES ? 1u : 0u;
}
