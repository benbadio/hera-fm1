/* SPDX-License-Identifier: GPL-3.0-only */
/* j6_phrase_held.c - the held-note tracker with the HOLD latch (phrase module, j6.h 9.2).
 *
 * Design: docs/J6_ENGINE_DESIGN.md 5.1. Behaviour: PH-4, HD-1 .. HD-3 and CH-6 in docs/J6_SPEC.md.
 * The tracker remembers which ids are physically down (in press order) and, with the latch on, the latched
 * set. The effective set is derived from those on demand:
 *   poly, latch off   the last J6_HELD_MAX ids pressed among those still down;
 *   poly, latch on    the latched set (HD-2);
 *   mono, latch off   the last id pressed that is still down (CH-6);
 *   mono, latch on    the same while anything is down; with nothing down the latch keeps the last one
 *                     that sounded (HD-2: in chord mode the latched set is the last chord).
 * Every operation compares the effective set before and after, so `changed` (j6_held_take_change) is set
 * exactly when the set really changed. */
#include "j6.h"
#include "j6_phrase_state.h"

/* the effective set, ascending; returns the count */
static uint32_t j6p_hd_eff(const j6_held_t *h, uint8_t out[J6_HELD_MAX])
{
    uint32_t n = 0, i, j;
    if (h->mono) {
        if (h->ndown) out[n++] = h->down[h->ndown - 1u];
        else if (h->latch_on && h->nlatch) out[n++] = h->latch[0];
    } else if (h->latch_on) {
        for (i = 0; i < h->nlatch && n < J6_HELD_MAX; i++) out[n++] = h->latch[i];
    } else {
        i = h->ndown > J6_HELD_MAX ? h->ndown - J6_HELD_MAX : 0u;
        for (; i < h->ndown; i++) out[n++] = h->down[i];
    }
    for (i = 1; i < n; i++) {        /* insertion sort, at most 4 */
        uint8_t v = out[i];
        for (j = i; j > 0 && out[j - 1u] > v; j--) out[j] = out[j - 1u];
        out[j] = v;
    }
    return n;
}

/* set `changed` if the effective set differs from the one saved in before[0..nb-1] */
static void j6p_hd_note_change(j6_held_t *h, const uint8_t *before, uint32_t nb)
{
    uint8_t now[J6_HELD_MAX];
    uint32_t n = j6p_hd_eff(h, now), i;
    if (n != nb) {
        h->changed = 1;
        return;
    }
    for (i = 0; i < n; i++)
        if (now[i] != before[i]) {
            h->changed = 1;
            return;
        }
}

static int32_t j6p_hd_find(const uint8_t *a, uint32_t n, uint32_t id)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        if (a[i] == id) return (int32_t)i;
    return -1;
}

/* remove a[k], keeping the order */
static void j6p_hd_remove_at(uint8_t *a, uint8_t *n, uint32_t k)
{
    uint32_t i;
    for (i = k; i + 1u < *n; i++) a[i] = a[i + 1u];
    (*n)--;
}

/* append id as the newest entry of a list of at most cap; the oldest falls out when it is full */
static void j6p_hd_push_newest(uint8_t *a, uint8_t *n, uint32_t cap, uint32_t id)
{
    int32_t k = j6p_hd_find(a, *n, id);
    if (k >= 0) j6p_hd_remove_at(a, n, (uint32_t)k);     /* again: it becomes the newest */
    if (*n >= cap) j6p_hd_remove_at(a, n, 0);
    a[(*n)++] = (uint8_t)id;
}

/* the latch takes what is down now (turning HOLD on, HD-2) */
static void j6p_hd_latch_down(j6_held_t *h)
{
    uint32_t i;
    h->nlatch = 0;
    if (!h->ndown) return;
    if (h->mono) {
        h->latch[0] = h->down[h->ndown - 1u];
        h->nlatch = 1;
        return;
    }
    i = h->ndown > J6_HELD_MAX ? h->ndown - J6_HELD_MAX : 0u;
    for (; i < h->ndown; i++) h->latch[h->nlatch++] = h->down[i];
}

void j6_held_init(j6_held_t *h)
{
    uint32_t i;
    uint8_t *b = (uint8_t *)h;
    for (i = 0; i < sizeof *h; i++) b[i] = 0;
}

void j6_held_press(j6_held_t *h, uint32_t id)
{
    uint8_t before[J6_HELD_MAX];
    uint32_t nb, was_down;
    id &= 0xFFu;
    if (j6p_hd_find(h->down, h->ndown, id) >= 0) return;          /* already down: ignored */
    nb = j6p_hd_eff(h, before);
    was_down = h->ndown;
    if (h->ndown >= J6_HELD_DOWN_MAX) j6p_hd_remove_at(h->down, &h->ndown, 0);   /* forget the oldest */
    h->down[h->ndown++] = (uint8_t)id;
    if (h->mono) {
        h->latch[0] = (uint8_t)id;                             /* the last chord that sounded */
        h->nlatch = 1;
    } else if (h->latch_on) {
        if (was_down) {
            j6p_hd_push_newest(h->latch, &h->nlatch, J6_HELD_MAX, id);   /* HD-2: adds, a 5th drops the oldest */
        } else {
            h->latch[0] = (uint8_t)id;                         /* HD-2: after all were up it replaces */
            h->nlatch = 1;
        }
    }
    j6p_hd_note_change(h, before, nb);
}

void j6_held_release(j6_held_t *h, uint32_t id)
{
    uint8_t before[J6_HELD_MAX];
    uint32_t nb;
    int32_t k = j6p_hd_find(h->down, h->ndown, id & 0xFFu);
    if (k < 0) return;                                         /* not down: ignored */
    nb = j6p_hd_eff(h, before);
    j6p_hd_remove_at(h->down, &h->ndown, (uint32_t)k);            /* poly latch on: the set stays (HD-2) */
    if (h->mono && h->ndown) {                                 /* CH-6: the previous still-down id */
        h->latch[0] = h->down[h->ndown - 1u];
        h->nlatch = 1;
    }
    j6p_hd_note_change(h, before, nb);
}

void j6_held_latch(j6_held_t *h, uint32_t on)
{
    uint8_t before[J6_HELD_MAX];
    uint32_t nb;
    on = on ? 1u : 0u;
    if (on == h->latch_on) return;
    nb = j6p_hd_eff(h, before);
    h->latch_on = (uint8_t)on;
    if (on) j6p_hd_latch_down(h);      /* HD-2: turning it on latches what is down */
    else h->nlatch = 0;             /* HD-2: turning it off leaves only what is down */
    j6p_hd_note_change(h, before, nb);
}

void j6_held_mono(j6_held_t *h, uint32_t on)
{
    uint8_t before[J6_HELD_MAX];
    uint32_t nb;
    on = on ? 1u : 0u;
    if (on == h->mono) return;
    nb = j6p_hd_eff(h, before);
    h->mono = (uint8_t)on;
    h->ndown = 0;                   /* the ids change meaning (notes / chord keys): start over */
    h->nlatch = 0;
    j6p_hd_note_change(h, before, nb);
}

void j6_held_clear(j6_held_t *h)
{
    uint8_t before[J6_HELD_MAX];
    uint32_t nb = j6p_hd_eff(h, before);
    h->ndown = 0;
    h->nlatch = 0;
    j6p_hd_note_change(h, before, nb);
}

uint32_t j6_held_get(const j6_held_t *h, uint8_t out[J6_HELD_MAX])
{
    return j6p_hd_eff(h, out);
}

uint32_t j6_held_down(const j6_held_t *h)
{
    return h->ndown;
}

uint32_t j6_held_latched(const j6_held_t *h)
{
    return h->latch_on;
}

uint32_t j6_held_take_change(j6_held_t *h)
{
    uint32_t c = h->changed;
    h->changed = 0;
    return c;
}
