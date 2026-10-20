/* SPDX-License-Identifier: GPL-3.0-only */
/* j6_phrase.c - the phrase engine of the J-6 (phrase module, j6.h 9.3): styles 1-9, accents, shuffle.
 *
 * Design: docs/J6_ENGINE_DESIGN.md 5.2. Behaviour: PH-4 .. PH-19 and SQ-7 in docs/J6_SPEC.md.
 *
 * One model for every style: a LOOP of hits, each with an onset and a gate in sub-ticks.
 *   styles 1-2 (ARP)     one hit per division (PH-10, PH-11), playing the next note of the arp cycle
 *                        (PH-12) at arp index idx (PH-13); a 24-tick division is written as a 48-tick loop
 *                        of two hits so the shuffle sees the off-beat sixteenth;
 *   styles 3-5 (BEAT)    the PH-14 note value or the PH-15 / PH-16 rhythm string; every hit plays all of H;
 *   styles 6-9 (PHRASE)  the bank's event list for |H| notes (PH-17 .. PH-19): each event plays
 *                        H[src] + semi at vELo, or at A.vLo when it carries J6_PEV_ACCENT.
 * Styles 1-5 build their loop into vev[] when the style changes; styles 6-9 read the bank directly.
 *
 * Timing (j6.h 9.3). j6_phrase_tick(pt) plays the hits whose SHUFFLED onset lies in [pt*16, pt*16+16):
 * the occurrence k of a hit with loop offset t sits at k*loop*16 + t + shift (k >= 0: nothing before the
 * phrase origin pt = 0). Because the tick asks "which onsets land in me", positive and negative shuffle
 * need no pending list and no look-ahead state: a hit moved up to 22 ticks earlier or later is found
 * from the tick it lands in. pt may jump. Note-offs are kept in elapsed sub-ticks (now * 16 + sub), so a
 * jump never changes a gate.
 *
 * Shuffle (SQ-7): j6_shuffle_offset() of the hit's onset from the loop start (= the phrase origin for
 * styles 1-5, whose loops are whole multiples of 48 ticks wherever an off-beat can occur). A table strum
 * (onsets at most 4 ticks apart, PH-18) moves as one: every note of it uses the offset of its first note,
 * so a strum on an off-beat sixteenth keeps its spacing and its order. A change of the shuffle while the
 * phrase plays takes effect at the next tick and settles for a while (j6p_shuffle_step, j6p_collect):
 * every hit plays exactly once, under the old or the new timing, and a strum never splits.
 *
 * Set changes (j6.h 9.3): an empty H cuts everything at once; a different set stops the notes whose held
 * note left H at once (in set_held's queue), resets the arp index and, for BEAT and PHRASE styles, makes
 * the next tick LATE JOIN: every hit still inside its gate at that tick plays the new set's notes for the
 * rest of that gate (a note already sounding just takes the new end), unless the same notes start in that
 * tick anyway. The late join runs in the first tick after the change, not in set_held: only the tick
 * knows its pt (the glue applies the sequencer's HELD right before that tick, and a live press lands at
 * most one tick before it). The same set again changes nothing.
 *
 * Events of one tick are pushed in time order: for each sub-tick, the due NOTE_OFFs first, then the
 * NOTE_ONs; a note started while it still sounds gets its NOTE_OFF first (retrigger). There is one
 * act[] entry per MIDI note, so every NOTE_ON gets exactly one NOTE_OFF. A tick starts at most
 * J6P_TICK_ONS_MAX notes; with act[] full, the note ending first is stopped to make room. So a tick
 * pushes at most 16 + 2 * 16 = 48 events (J6_EVQ_CAP 64). */
#include "j6.h"
#include "j6_phrase_state.h"
#include "j6_phrase_tables.h"

J6_STATIC_ASSERT(phrase_tab_lists, sizeof j6p_lists / sizeof j6p_lists[0] == J6_PLISTS);
J6_STATIC_ASSERT(phrase_tab_pool, J6P_POOL_LEN <= 65535u);

const j6_phrase_bank_t j6_phrase_builtin = {j6p_lists, j6p_pool, (uint16_t)J6P_POOL_LEN};

#define J6P_TICK_ONS_MAX 16u          /* notes started in one tick at most */
#define J6P_STRUM_GAP (4u * J6_SUB)   /* table onsets this close belong to one strum (shuffle moves it whole) */
#define J6P_NOTE_MAX 127u
#define J6P_VEL_MAX 127u

/* PH-10 arp orders, ORD[(var - 1) % 6] */
enum { J6P_UP1 = 0, J6P_UPDOWN1, J6P_DOWN1, J6P_DOWN2, J6P_UPDOWN2, J6P_UP2, J6P_ORDERS };

/* PH-14 note values in ticks, var 1..12 */
static const uint16_t j6p_s3_len[J6_VARS] = {768, 384, 192, 96, 48, 24, 512, 256, 128, 64, 32, 16};

/* PH-15 / PH-16 rhythm strings, var 1..12: o = hit, _ = sixteenth rest, ~ = the hit lasts a sixteenth more */
static const char j6p_s4[J6_VARS][5] = {"o___", "_o__", "__o_", "___o", "o~o_", "o_o~",
                                        "o~~o", "_o~o", "ooo_", "oo_o", "o_oo", "_ooo"};
static const char j6p_s5[J6_VARS][5] = {"oo__", "o_o_", "o__o", "_oo_", "_o_o", "__oo",
                                        "o__",  "_o_",  "__o",  "oo_",  "o_o",  "_oo"};

/* one note to start in this tick */
struct j6p_hit {
    uint16_t gate;                    /* sub-ticks */
    uint8_t sub;                      /* 0..15 */
    uint8_t note;
    uint8_t from;                     /* the held note it comes from */
    uint8_t vel;
    uint8_t join;                     /* late join: a note already sounding only takes the new end */
};

/* ---- small helpers ---------------------------------------------------------------------------------- */

static uint32_t j6p_cat(const j6_phrase_t *p)
{
    return j6_style_cat(p->style);
}

static void j6p_push(const j6_phrase_t *p, j6_evq_t *q, uint32_t type, uint32_t sub, uint32_t note,
                     uint32_t vel)
{
    if (q) j6_evq_push(q, type, p->src, sub, note, vel, 0);
}

static void j6p_act_remove(j6_phrase_t *p, uint32_t k)
{
    p->act[k] = p->act[p->nact - 1u];
    p->nact--;
}

static int32_t j6p_act_find(const j6_phrase_t *p, uint32_t note)
{
    uint32_t i;
    for (i = 0; i < p->nact; i++)
        if (p->act[i].note == note) return (int32_t)i;
    return -1;
}

static uint32_t j6p_in_held(const j6_phrase_t *p, uint32_t note)
{
    uint32_t i;
    for (i = 0; i < p->n; i++)
        if (p->held[i] == note) return 1;
    return 0;
}

/* NOTE_OFF (sub 0) for everything sounding */
static void j6p_cut_all(j6_phrase_t *p, j6_evq_t *q)
{
    uint32_t i;
    for (i = 0; i < p->nact; i++) j6p_push(p, q, J6_EV_NOTE_OFF, 0, p->act[i].note, 0);
    p->nact = 0;
}

/* ---- styles 1-2: the arp cycle (PH-12) ------------------------------------------------------------- */

static uint32_t j6p_arp_div(uint32_t style, uint32_t var)
{
    if (style == 1u) return var <= 6u ? 48u : 32u;
    return var <= 6u ? 24u : 16u;
}

/* The notes of one arp cycle for H = h[0..n-1] (ascending), order 0..5. Range 2 (J6_CFG_ARP_RANGE2 = 1):
 * B = H then H + 12 for the "2" orders. With range2 = 0 (Q-1's other reading) B = H and UP&DOWN2 repeats
 * the turnaround notes (C E G G E C); UP2 / DOWN2 are then UP1 / DOWN1. Returns the length (0 if n = 0). */
static uint32_t j6p_arp_cycle(const uint8_t *h, uint32_t n, uint32_t order, uint32_t range2, uint8_t *out,
                              uint8_t *from)
{
    uint8_t b[2u * J6_HELD_MAX], bf[2u * J6_HELD_MAX];
    uint32_t m = 0, k, len = 0;
    if (n > J6_HELD_MAX) n = J6_HELD_MAX;
    for (k = 0; k < n; k++) {
        b[m] = h[k];
        bf[m++] = h[k];
    }
    if (range2 && (order == J6P_DOWN2 || order == J6P_UPDOWN2 || order == J6P_UP2))
        for (k = 0; k < n; k++)
            if (h[k] + 12u <= J6P_NOTE_MAX) {
                b[m] = (uint8_t)(h[k] + 12u);
                bf[m++] = h[k];
            }
    if (!m) return 0;
#define J6P_CYC_ADD(i) (out[len] = b[i], from[len] = bf[i], len++)
    switch (order) {
    case J6P_DOWN1:
    case J6P_DOWN2:
        for (k = m; k-- > 0;) J6P_CYC_ADD(k);
        break;
    case J6P_UPDOWN1:
    case J6P_UPDOWN2:
        for (k = 0; k < m; k++) J6P_CYC_ADD(k);
        if (!range2 && order == J6P_UPDOWN2)
            for (k = m; k-- > 0;) J6P_CYC_ADD(k);         /* turnaround notes repeated */
        else
            for (k = m - 1u; k-- > 1u;) J6P_CYC_ADD(k);    /* B[m-2] .. B[1]: ends not repeated */
        break;
    default:                                              /* UP1, UP2 */
        for (k = 0; k < m; k++) J6P_CYC_ADD(k);
        break;
    }
#undef J6P_CYC_ADD
    return len;
}

static void j6p_build_cycle(j6_phrase_t *p)
{
    p->ncyc = 0;
    if (p->style <= 2u && p->n)
        p->ncyc = (uint8_t)j6p_arp_cycle(p->held, p->n, (p->var - 1u) % J6P_ORDERS, p->cfg_range2, p->cyc,
                                         p->cyc_from);
}

/* ---- styles 1-5: the loop of hits ------------------------------------------------------------------ */

/* one hit at `t` ticks of nominal length `len` ticks (vev[].len holds ticks until j6p_build_vev ends) */
static void j6p_vev_add(j6_phrase_t *p, uint32_t t, uint32_t len)
{
    if (p->nvev >= J6P_VEV_MAX) return;
    p->vev[p->nvev].t = (uint16_t)(t * J6_SUB);
    p->vev[p->nvev].len = (uint16_t)len;
    p->vev[p->nvev].src = 0;
    p->vev[p->nvev].semi = 0;
    p->nvev++;
}

/* the hits of rhythm string s placed at `offset` ticks, those starting before `limit` (PH-15) */
static void j6p_vev_string(j6_phrase_t *p, const char *s, uint32_t offset, uint32_t limit)
{
    uint32_t i, last = J6P_VEV_MAX;
    for (i = 0; s[i]; i++) {
        uint32_t t = offset + i * J6_TICKS_16TH;
        if (s[i] == 'o') {
            last = J6P_VEV_MAX;
            if (t < limit && p->nvev < J6P_VEV_MAX) {
                last = p->nvev;
                j6p_vev_add(p, t, J6_TICKS_16TH);
            }
        } else if (s[i] == '~' && last < J6P_VEV_MAX) {
            p->vev[last].len = (uint16_t)(p->vev[last].len + J6_TICKS_16TH);
        }
    }
}

static uint32_t j6p_strlen(const char *s)
{
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

static void j6p_build_vev(j6_phrase_t *p)
{
    uint32_t i, var = p->var;
    p->nvev = 0;
    p->vloop = 0;
    if (p->style <= 2u) {                                 /* PH-10, PH-11 */
        uint32_t div = j6p_arp_div(p->style, var);
        if (div == J6_TICKS_16TH) {
            p->vloop = 48;
            j6p_vev_add(p, 0, div);
            j6p_vev_add(p, J6_TICKS_16TH, div);
        } else {
            p->vloop = (uint16_t)div;
            j6p_vev_add(p, 0, div);
        }
    } else if (p->style == 3u) {                          /* PH-14 */
        uint32_t len = j6p_s3_len[var - 1u];
        if (len == J6_TICKS_16TH) {
            p->vloop = 48;
            j6p_vev_add(p, 0, len);
            j6p_vev_add(p, J6_TICKS_16TH, len);
        } else {
            p->vloop = (uint16_t)len;
            j6p_vev_add(p, 0, len);
        }
    } else if (p->style == 4u) {                          /* PH-15 */
        p->vloop = 96;
        j6p_vev_string(p, j6p_s4[var - 1u], 0, 96);
    } else if (p->style == 5u) {                          /* PH-15, PH-16 */
        const char *s = j6p_s5[var - 1u];
        uint32_t len = j6p_strlen(s) * J6_TICKS_16TH;
        if (len == 96u) {
            p->vloop = 96;
            j6p_vev_string(p, s, 0, 96);
        } else if (p->cfg_s5_run_on) {                    /* pt % 72, written as two loops: 144 */
            p->vloop = (uint16_t)(2u * len);
            j6p_vev_string(p, s, 0, 2u * len);
            j6p_vev_string(p, s, len, 2u * len);
        } else {                                          /* pt % 384 % 72: restarts every bar */
            p->vloop = J6_TICKS_BAR;
            for (i = 0; i * len < J6_TICKS_BAR; i++) j6p_vev_string(p, s, i * len, J6_TICKS_BAR);
        }
    }
    for (i = 0; i < p->nvev; i++) p->vev[i].len = (uint16_t)(j6_gate(p->vev[i].len) * J6_SUB);   /* PH-7 */
}

/* ---- the current loop -------------------------------------------------------------------------------- */

/* the hits of the current style for |H| = n: events, count; *loop in ticks. 0 = nothing to play */
static uint32_t j6p_loop(const j6_phrase_t *p, const j6_pev_t **ev, uint32_t *loop)
{
    const j6_phrase_bank_t *b = p->bank;
    const j6_plist_t *l;
    if (!p->n) return 0;
    if (p->style < J6_TABLE_STYLE_FIRST) {
        if (!p->vloop || !p->nvev) return 0;
        *ev = p->vev;
        *loop = p->vloop;
        return p->nvev;
    }
    if (!b->lists || !b->pool) return 0;
    l = &b->lists[j6_plist_index(p->style, p->var, p->n)];
    if (!l->loop || !l->count || (uint32_t)l->first + l->count > b->pool_len) return 0;
    *ev = b->pool + l->first;
    *loop = l->loop;
    return l->count;
}

/* the shuffle offset of each event: of the first note of its strum (sorted events); *anchor = its onset */
static int32_t j6p_shift(int32_t shuffle, const j6_pev_t *ev, uint32_t i, uint32_t *anchor)
{
    if (i == 0 || ev[i].t > ev[i - 1u].t + J6P_STRUM_GAP) *anchor = ev[i].t;
    if (!shuffle) return 0;
    return j6_shuffle_offset(*anchor, shuffle);
}

/* the sub-tick in tick pt where loop offset t moved by `shift` lands (k >= 0), or -1 */
static int32_t j6p_land(uint32_t pt, uint32_t loop, uint32_t t, int32_t shift)
{
    uint32_t pos16 = (pt % loop) * J6_SUB, l16 = loop * J6_SUB;
    int32_t r = (int32_t)t + shift - (int32_t)pos16;
    if (r < 0) return -1;                                 /* later in this loop */
    if (r >= (int32_t)l16) {                              /* moved past the loop end: an earlier loop's */
        uint32_t back = (uint32_t)r / l16;
        if (pt / loop < back) return -1;                  /* that loop was before the phrase origin */
        r -= (int32_t)(back * l16);
    }
    return r < (int32_t)J6_SUB ? r : -1;
}

static void j6p_add(struct j6p_hit *hit, uint32_t *nh, uint32_t sub, uint32_t note, uint32_t from,
                    uint32_t vel, uint32_t gate, uint32_t join)
{
    struct j6p_hit *h;
    if (*nh >= J6P_TICK_ONS_MAX) return;
    h = &hit[(*nh)++];
    h->sub = (uint8_t)sub;
    h->note = (uint8_t)note;
    h->from = (uint8_t)from;
    h->vel = (uint8_t)vel;
    h->gate = (uint16_t)(gate ? gate : 1u);
    h->join = (uint8_t)join;
}

/* the notes of one hit of event e */
static void j6p_hit_notes(j6_phrase_t *p, const j6_pev_t *e, uint32_t sub, uint32_t gate, uint32_t join,
                          struct j6p_hit *hit, uint32_t *nh)
{
    uint32_t cat = j6p_cat(p), k;
    if (cat == J6_CAT_ARP) {
        if (!p->ncyc) return;
        k = p->arp_idx % p->ncyc;
        p->arp_idx++;
        j6p_add(hit, nh, sub, p->cyc[k], p->cyc_from[k], p->vel, gate, join);
    } else if (cat == J6_CAT_BEAT) {
        for (k = 0; k < p->n; k++) j6p_add(hit, nh, sub, p->held[k], p->held[k], p->vel, gate, join);
    } else {
        uint32_t i = e->src & J6_PEV_SRC_MASK;
        int32_t note;
        if (i >= p->n) return;
        note = (int32_t)p->held[i] + e->semi;
        if (note < 0 || note > (int32_t)J6P_NOTE_MAX) return;
        if (join) {                                       /* the same note starts in this tick anyway */
            for (k = 0; k < *nh; k++)
                if (!hit[k].join && hit[k].note == (uint32_t)note) return;
        }
        j6p_add(hit, nh, sub, (uint32_t)note, p->held[i], (e->src & J6_PEV_ACCENT) ? p->avel : p->vel, gate,
                join);
    }
}

/* the hits whose shuffled onset lies in [pt*16, pt*16 + 16).
 * While a shuffle change settles (shuf_tr; old shuffle so, new sn, first new tick C = shuf_pt), every
 * occurrence plays exactly once: a strum whose first note began under the old timing (Na + so < C), or
 * whose new time is already past (Na + sn < C), keeps the old timing for all its notes; any other takes
 * the new one. Na = the occurrence's anchor onset; everything relative to C, so no absolute time. */
static void j6p_collect(j6_phrase_t *p, uint32_t pt, const j6_pev_t *ev, uint32_t count, uint32_t loop,
                        struct j6p_hit *hit, uint32_t *nh)
{
    uint32_t pos16 = (pt % loop) * J6_SUB, i, anchor = 0;
    int32_t dc = p->shuf_tr ? (int32_t)((pt - p->shuf_pt) * J6_SUB) : 0;   /* pt*16 - C*16 while settling */
    if (!p->shuf_eff && !p->shuf_tr) {                    /* sorted: binary search the tick */
        uint32_t lo = 0, hi = count;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2u;
            if (ev[mid].t < pos16) lo = mid + 1u;
            else hi = mid;
        }
        for (i = lo; i < count && ev[i].t < pos16 + J6_SUB; i++)
            j6p_hit_notes(p, &ev[i], ev[i].t - pos16, ev[i].len, 0, hit, nh);
        return;
    }
    for (i = 0; i < count; i++) {
        int32_t sn = j6p_shift(p->shuf_eff, ev, i, &anchor), so, d, r = j6p_land(pt, loop, ev[i].t, sn);
        so = p->shuf_tr ? j6_shuffle_offset(anchor, p->shuf_old) : sn;
        if (so == sn) {                                   /* the same time either way */
            if (r >= 0) j6p_hit_notes(p, &ev[i], (uint32_t)r, ev[i].len, 0, hit, nh);
            continue;
        }
        d = (int32_t)(ev[i].t - anchor);                  /* note - anchor, the same occurrence */
        if (d > (int32_t)p->shuf_span) p->shuf_span = (uint16_t)d;
        /* new timing: Na - C = r - sn + dc - d; it plays iff Na + sn >= C and Na + so >= C */
        if (r >= 0 && r - d + dc >= 0 && r - sn + so - d + dc >= 0)
            j6p_hit_notes(p, &ev[i], (uint32_t)r, ev[i].len, 0, hit, nh);
        /* old timing: Na - C = r - so + dc - d; it plays iff Na + so < C or Na + sn < C */
        r = j6p_land(pt, loop, ev[i].t, so);
        if (r >= 0 && (r - d + dc < 0 || r - so + sn - d + dc < 0))
            j6p_hit_notes(p, &ev[i], (uint32_t)r, ev[i].len, 0, hit, nh);
    }
}

/* late join: the hits still inside their gate at pt*16 play the current H for the rest of the gate */
static void j6p_late_join(j6_phrase_t *p, uint32_t pt, const j6_pev_t *ev, uint32_t count, uint32_t loop,
                          struct j6p_hit *hit, uint32_t *nh)
{
    uint32_t cat = j6p_cat(p), pos16 = (pt % loop) * J6_SUB, l16 = loop * J6_SUB, kk = pt / loop;
    uint32_t i, anchor = 0;
    if (cat == J6_CAT_ARP) return;
    if (cat == J6_CAT_BEAT && *nh) return;                /* a hit falls in this tick anyway */
    for (i = 0; i < count; i++) {
        int32_t e = (int32_t)pos16 - ((int32_t)ev[i].t + j6p_shift(p->shuf_eff, ev, i, &anchor));
        uint32_t back = 0;
        while (e <= 0) {                                  /* the latest occurrence before this tick */
            e += (int32_t)l16;
            back++;
        }
        if (back > kk) continue;                          /* none since the phrase origin */
        if ((uint32_t)e >= ev[i].len) continue;           /* its gate is over */
        if (pt - p->hold_pt < ev[i].len && (uint32_t)e > (pt - p->hold_pt) * J6_SUB)
            continue;                                     /* began before H did: wait for the grid (PH-21) */
        j6p_hit_notes(p, &ev[i], 0, ev[i].len - (uint32_t)e, 1, hit, nh);
        if (cat == J6_CAT_BEAT) return;                   /* one hit of all of H */
    }
}

/* start one note at h->sub (t0 = now * 16); returns its NOTE_OFF time relative to t0 */
static uint32_t j6p_start(j6_phrase_t *p, const struct j6p_hit *h, uint32_t t0, j6_evq_t *q)
{
    int32_t k = j6p_act_find(p, h->note);
    struct j6p_active *a;
    if (h->join && k >= 0) {                              /* sounding already: it ends with this hit */
        p->act[k].off = t0 + h->sub + h->gate;
        p->act[k].from = h->from;
        return (uint32_t)h->sub + h->gate;
    }
    if (k >= 0) {
        j6p_push(p, q, J6_EV_NOTE_OFF, h->sub, h->note, 0);   /* retrigger: OFF before ON */
    } else if (p->nact >= J6P_ACTIVE_MAX) {
        uint32_t i, best = 0;                             /* full: stop the note that ends first */
        for (i = 1; i < p->nact; i++)
            if ((int32_t)(p->act[i].off - p->act[best].off) < 0) best = i;
        k = (int32_t)best;
        j6p_push(p, q, J6_EV_NOTE_OFF, h->sub, p->act[k].note, 0);
    } else {
        k = (int32_t)p->nact++;
    }
    j6p_push(p, q, J6_EV_NOTE_ON, h->sub, h->note, h->vel);
    a = &p->act[k];
    a->note = h->note;
    a->from = h->from;
    a->off = t0 + h->sub + h->gate;
    return (uint32_t)h->sub + h->gate;
}

/* this tick's NOTE_OFFs and NOTE_ONs in time order: per sub-tick the due NOTE_OFFs, then the NOTE_ONs.
 * `mask` marks the sub-ticks with anything to do (a note started here may also end here). */
static void j6p_emit(j6_phrase_t *p, const struct j6p_hit *hit, uint32_t nh, j6_evq_t *q)
{
    uint32_t t0 = p->now * J6_SUB, mask = 0, sub, i;
    for (i = 0; i < nh; i++) mask |= 1u << hit[i].sub;
    for (i = 0; i < p->nact; i++) {
        int32_t d = (int32_t)(p->act[i].off - t0);
        if (d < (int32_t)J6_SUB) mask |= 1u << (d < 0 ? 0 : d);
    }
    for (sub = 0; mask >> sub; sub++) {
        if (!((mask >> sub) & 1u)) continue;
        for (i = 0; i < p->nact;) {
            if ((int32_t)(p->act[i].off - t0) <= (int32_t)sub) {
                j6p_push(p, q, J6_EV_NOTE_OFF, sub, p->act[i].note, 0);
                j6p_act_remove(p, i);
            } else {
                i++;
            }
        }
        for (i = 0; i < nh; i++) {
            if (hit[i].sub == sub) {
                uint32_t end = j6p_start(p, &hit[i], t0, q);
                if (end < J6_SUB) mask |= 1u << end;
            }
        }
    }
}

/* ---- API ------------------------------------------------------------------------------------------- */

void j6_phrase_init(j6_phrase_t *p, uint32_t src)
{
    uint32_t i;
    uint8_t *b = (uint8_t *)p;
    for (i = 0; i < sizeof *p; i++) b[i] = 0;
    p->bank = &j6_phrase_builtin;
    p->src = (uint8_t)src;
    p->style = 1;
    p->var = 1;
    p->vel = J6_VELO_DEF;
    p->avel = J6_AVEL_DEF;
    p->cfg_range2 = J6_CFG_ARP_RANGE2 ? 1u : 0u;
    p->cfg_s5_run_on = J6_CFG_S5_RUN_ON ? 1u : 0u;
    j6p_build_vev(p);
}

void j6_phrase_set_bank(j6_phrase_t *p, const j6_phrase_bank_t *bank)
{
    p->bank = bank ? bank : &j6_phrase_builtin;
}

void j6_phrase_set_style(j6_phrase_t *p, uint32_t style, uint32_t var)
{
    if (style < 1u) style = 1u;
    if (style > J6_STYLES) style = J6_STYLES;
    if (var < 1u) var = 1u;
    if (var > J6_VARS) var = J6_VARS;
    p->style = (uint8_t)style;
    p->var = (uint8_t)var;
    j6p_build_vev(p);             /* from the next hit; the arp index carries on (modulo the new cycle) */
    j6p_build_cycle(p);
}

void j6_phrase_set_vel(j6_phrase_t *p, uint32_t vel, uint32_t accent_vel)
{
    p->vel = (uint8_t)(vel < 1u ? 1u : vel > J6P_VEL_MAX ? J6P_VEL_MAX : vel);
    p->avel = (uint8_t)(accent_vel < 1u ? 1u : accent_vel > J6P_VEL_MAX ? J6P_VEL_MAX : accent_vel);
}

void j6_phrase_set_shuffle(j6_phrase_t *p, int32_t shuffle)
{
    p->shuffle = (int8_t)j6_clamp(shuffle, J6_SHUFFLE_MIN, J6_SHUFFLE_MAX);
}

void j6_phrase_set_held(j6_phrase_t *p, const uint8_t *notes, uint32_t n, j6_evq_t *q)
{
    uint8_t h[J6_HELD_MAX];
    uint32_t m = 0, i, j, was = p->n;
    /* PH-4: ascending, distinct; the last J6_HELD_MAX distinct notes given win */
    for (i = notes ? n : 0u; i-- > 0 && m < J6_HELD_MAX;) {
        uint8_t v = notes[i];
        if (v > J6P_NOTE_MAX) continue;
        for (j = 0; j < m && h[j] != v; j++) {
        }
        if (j == m) h[m++] = v;
    }
    for (i = 1; i < m; i++) {
        uint8_t v = h[i];
        for (j = i; j > 0 && h[j - 1u] > v; j--) h[j] = h[j - 1u];
        h[j] = v;
    }
    if (m == p->n) {                                      /* the same set again: nothing (PH-13) */
        for (i = 0; i < m && h[i] == p->held[i]; i++) {
        }
        if (i == m) return;
    }
    if (!m) {                                             /* PH-6: everything stops at once */
        j6p_cut_all(p, q);
        p->n = 0;
        p->join = 0;
        p->arp_idx = 0;
        p->ncyc = 0;
        return;
    }
    for (i = 0; i < m; i++) p->held[i] = h[i];
    p->n = (uint8_t)m;
    for (i = 0; i < p->nact;) {                           /* notes whose held note left H stop now */
        if (!j6p_in_held(p, p->act[i].from)) {
            j6p_push(p, q, J6_EV_NOTE_OFF, 0, p->act[i].note, 0);
            j6p_act_remove(p, i);
        } else {
            i++;
        }
    }
    p->arp_idx = 0;                                       /* PH-13 */
    j6p_build_cycle(p);
    p->join = was ? 1u : 0u;                              /* late join at the next tick */
    if (!was) p->fresh = 1;                               /* the next tick is the first of this hold */
}

/* SQ-7 shuffle changes (j6_phrase_set_shuffle may run between any two ticks, also mid-loop): the new value
 * takes effect at a tick C and, when that tick follows the previous one (pt = last + 1), settles
 * (j6p_collect) until no strum can still be due under the old timing: 48 ticks plus the longest strum
 * after C (2 x 21.6 ticks of shift < 48). A change made while one settles waits for it, then applies the
 * value set last. After a pt jump nothing settles: the new value applies at once. */
static void j6p_shuffle_step(j6_phrase_t *p, uint32_t pt)
{
    uint32_t consec = p->ticked && pt == p->last_pt + 1u;
    if (p->shuf_tr && (!consec || (pt - p->shuf_pt) * J6_SUB >= 48u * J6_SUB + p->shuf_span)) p->shuf_tr = 0;
    if (p->shuffle != p->shuf_eff && !p->shuf_tr) {
        p->shuf_old = p->shuf_eff;
        p->shuf_eff = p->shuffle;
        p->shuf_pt = pt;
        p->shuf_span = 0;
        p->shuf_tr = (uint8_t)consec;
    }
    p->ticked = 1;
    p->last_pt = pt;
}

void j6_phrase_tick(j6_phrase_t *p, uint32_t pt, j6_evq_t *q)
{
    struct j6p_hit hit[J6P_TICK_ONS_MAX];
    const j6_pev_t *ev = NULL;
    uint32_t nh = 0, loop = 0, count = j6p_loop(p, &ev, &loop);
    if (p->fresh || (int32_t)(pt - p->hold_pt) < 0) {    /* new hold, or the clock went back */
        p->hold_pt = pt;
        p->fresh = 0;
    }
    j6p_shuffle_step(p, pt);
    if (count && loop) {
        j6p_collect(p, pt, ev, count, loop, hit, &nh);
        if (p->join) j6p_late_join(p, pt, ev, count, loop, hit, &nh);
    }
    p->join = 0;
    j6p_emit(p, hit, nh, q);
    p->now++;
}

void j6_phrase_stop(j6_phrase_t *p, j6_evq_t *q)
{
    j6p_cut_all(p, q);
    p->n = 0;
    p->join = 0;
    p->arp_idx = 0;
    p->ncyc = 0;
}

uint32_t j6_phrase_sounding(const j6_phrase_t *p)
{
    return p->nact;
}

uint32_t j6_phrase_loop_ticks(const j6_phrase_bank_t *bank, uint32_t style, uint32_t var, uint32_t n)
{
    if (style < 1u || style > J6_STYLES || var < 1u || var > J6_VARS || !n) return 0;
    if (n > J6_HELD_MAX) n = J6_HELD_MAX;
    if (style <= 2u) {
        static const uint8_t h[J6_HELD_MAX] = {0, 1, 2, 3};
        uint8_t c[J6P_CYCLE_MAX], f[J6P_CYCLE_MAX];
        return j6p_arp_cycle(h, n, (var - 1u) % J6P_ORDERS, J6_CFG_ARP_RANGE2 ? 1u : 0u, c, f) *
               j6p_arp_div(style, var);
    }
    if (style == 3u) return j6p_s3_len[var - 1u];
    if (style == 4u) return 96u;
    if (style == 5u) return var <= 6u ? 96u : J6_CFG_S5_RUN_ON ? 72u : J6_TICKS_BAR;
    if (!bank) bank = &j6_phrase_builtin;
    if (!bank->lists) return 0;
    return bank->lists[j6_plist_index(style, var, n)].loop;
}

uint32_t j6_phrase_list_flags(const j6_phrase_bank_t *bank, uint32_t style, uint32_t var, uint32_t n)
{
    if (style < J6_TABLE_STYLE_FIRST || style > J6_STYLES || var < 1u || var > J6_VARS || !n) return 0;
    if (n > J6_HELD_MAX) n = J6_HELD_MAX;
    if (!bank) bank = &j6_phrase_builtin;
    if (!bank->lists) return 0;
    return bank->lists[j6_plist_index(style, var, n)].flags;
}
