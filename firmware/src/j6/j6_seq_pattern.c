/* SPDX-License-Identifier: GPL-3.0-only */
/* j6_seq_pattern.c - seq module: pattern and step operations and the flash format.
 *
 * Design: docs/J6_ENGINE_DESIGN.md 6.1 and 6.2. Behaviour: SQ-3 .. SQ-5, SQ-11 .. SQ-17, SQ-22,
 * SQ-42 .. SQ-46 and section 6.1 in docs/J6_SPEC.md. Pure functions on one j6_pattern_t; the caller
 * marks the pattern unsaved (j6_seq_edit).
 *
 * Step invariants every function here keeps (and decode restores):
 *   - note[0..k-1] ascending and distinct, 0..127; note[k..3] J6_NO_NOTE;
 *   - flags: J6_STF_TIE and the chord mask only; mask bits only on filled slots;
 *   - chord tag J6_NO_CHORD exactly when the mask is 0; a tag has a chord index 0..11;
 *   - a step with notes has no tie (adding a note to a tie step clears the tie, SQ-17);
 *   - phrase: bit 7 the STYLE flag, bits 0..6 a code 0..107. */
#include "j6.h"

#define J6S_MASK_OF(st) (((uint32_t)(st)->flags >> J6_STF_CMASK_SHIFT) & 0x0Fu)
#define J6S_NOTE_MAX 127u
#define J6S_PHR_CODES (J6_STYLES * J6_VARS)   /* 108 */
#define J6S_PUT_MAX 8u                        /* candidate notes j6s_put() sorts */

static void j6s_zero(void *dst, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = 0;
}

static uint32_t j6s_last(const j6_pattern_t *p)
{
    return (uint32_t)j6_clamp((int32_t)p->last, 1, (int32_t)J6_STEPS);
}

/* a chord tag a step may hold: chord index 0..11, any octave */
static uint32_t j6s_tag_ok(uint32_t tag)
{
    return tag <= 0xFFu && tag != J6_NO_CHORD && (tag & 0x0Fu) < J6_CHORDS;
}

/* a phrase byte with an out-of-range code becomes style 1 var 1, keeping the STYLE flag
 * (the same reading j6_phr_style() / j6_phr_var() give it) */
static uint8_t j6s_phr_fix(uint32_t b)
{
    uint32_t code = b & 0x7Fu;
    if (code >= J6S_PHR_CODES) code = 0;
    return (uint8_t)((b & J6_PHR_ON) | code);
}

/* the step's notes into notes[] and its chord mask re-indexed to them (bit i = notes[i] is a chord note) */
static uint32_t j6s_get(const j6_step_t *s, uint8_t notes[J6_STEP_NOTES], uint32_t *cm)
{
    uint32_t i, k = 0, m = 0, mask = J6S_MASK_OF(s);
    for (i = 0; i < J6_STEP_NOTES; i++) {
        if (s->note[i] > J6S_NOTE_MAX) continue;
        if (mask & (1u << i)) m |= 1u << k;
        notes[k++] = s->note[i];
    }
    *cm = m;
    return k;
}

/* Write notes[0..n-1] into s: notes above 127 dropped, sorted ascending, duplicates merged (a note is a
 * chord note if any copy was), at most 4 (the lowest). cm bit i marks notes[i] as a chord note. The tie
 * flag and the phrase are kept; the mask is rebuilt; with no chord note left the tag is cleared. */
static void j6s_put(j6_step_t *s, const uint8_t *notes, uint32_t n, uint32_t cm)
{
    uint8_t v[J6S_PUT_MAX], c[J6S_PUT_MAX];
    uint32_t i, j, m, k = 0, mask = 0;
    if (n > J6S_PUT_MAX) n = J6S_PUT_MAX;
    for (i = 0; i < n; i++) {
        uint8_t x = notes[i], b = (uint8_t)((cm >> i) & 1u);
        if (x > J6S_NOTE_MAX) continue;
        for (j = 0; j < k && v[j] < x; j++) {
        }
        if (j < k && v[j] == x) {
            c[j] = (uint8_t)(c[j] | b);
            continue;
        }
        for (m = k; m > j; m--) {
            v[m] = v[m - 1];
            c[m] = c[m - 1];
        }
        v[j] = x;
        c[j] = b;
        k++;
    }
    if (k > J6_STEP_NOTES) k = J6_STEP_NOTES;
    for (i = 0; i < J6_STEP_NOTES; i++) {
        s->note[i] = i < k ? v[i] : (uint8_t)J6_NO_NOTE;
        if (i < k && c[i]) mask |= 1u << i;
    }
    s->flags = (uint8_t)((s->flags & J6_STF_TIE) | (mask << J6_STF_CMASK_SHIFT));
    if (!mask) s->chord = J6_NO_CHORD;
}

/* ---- defaults ------------------------------------------------------------------------------------- */

void j6_step_clear(j6_step_t *s)
{
    uint32_t i;
    if (!s) return;
    for (i = 0; i < J6_STEP_NOTES; i++) s->note[i] = J6_NO_NOTE;
    s->flags = 0;
    s->phrase = J6_PHR_DEF;
    s->chord = J6_NO_CHORD;
    s->spare = 0;
}

/* SQ-46, section 6.1 */
void j6_pattern_init(j6_pattern_t *p)
{
    uint32_t i;
    if (!p) return;
    j6s_zero(p, sizeof *p);
    p->tempo10 = J6_TEMPO10_DEF;
    p->last = J6_LAST_DEF;
    p->beat = J6_BEAT_4;
    p->shuffle = 0;
    p->phrase = J6_PHR_DEF;
    p->sound = 0;
    p->vol = J6_VOL_DEF;
    p->tran = 0;
    p->dsync = 0;
    p->avel = J6_AVEL_DEF;
    p->pchg = J6_PCHG_STEP;
    p->sclk = J6_SCLK_DEF;
    for (i = 0; i < J6_KNOBS; i++) p->knob[i] = J6_KNOB_UNSET;
    for (i = 0; i < J6_STEPS; i++) j6_step_clear(&p->step[i]);
}

/* SQ-44: steps, last and the pattern phrase; tempo, shuffle, the *1 settings, sound and knobs stay */
void j6_pattern_clear(j6_pattern_t *p)
{
    uint32_t i;
    if (!p) return;
    for (i = 0; i < J6_STEPS; i++) j6_step_clear(&p->step[i]);
    p->last = J6_LAST_DEF;
    p->phrase = J6_PHR_DEF;
}

/* ---- step queries ---------------------------------------------------------------------------------- */

uint32_t j6_step_count(const j6_step_t *s)
{
    uint32_t i, k = 0;
    if (!s) return 0;
    for (i = 0; i < J6_STEP_NOTES; i++)
        if (s->note[i] <= J6S_NOTE_MAX) k++;
    return k;
}

int32_t j6_step_find(const j6_step_t *s, uint32_t note)
{
    uint32_t i;
    if (!s || note > J6S_NOTE_MAX) return -1;
    for (i = 0; i < J6_STEP_NOTES; i++)
        if (s->note[i] == note) return (int32_t)i;
    return -1;
}

/* ---- step edit (SQ-11 .. SQ-17) ------------------------------------------------------------------- */

/* SQ-12: add the note (up to 4; clears the tie) or remove it if present */
uint32_t j6_step_key_note(j6_pattern_t *p, uint32_t step, uint32_t note)
{
    uint8_t v[J6S_PUT_MAX];
    uint32_t k, cm, i;
    j6_step_t *s;
    if (!p || step >= J6_STEPS || note > J6S_NOTE_MAX) return J6_EDIT_NONE;
    s = &p->step[step];
    k = j6s_get(s, v, &cm);
    for (i = 0; i < k; i++) {
        uint32_t j;
        if (v[i] != note) continue;
        /* remove v[i]: drop its note and its mask bit, keep the rest in order */
        cm = (cm & ((1u << i) - 1u)) | ((cm >> (i + 1u)) << i);
        for (j = i; j + 1u < k; j++) v[j] = v[j + 1u];
        j6s_put(s, v, k - 1u, cm);
        return J6_EDIT_REMOVED;
    }
    if (k >= J6_STEP_NOTES) return J6_EDIT_FULL;
    v[k] = (uint8_t)note;                /* a single note: not a chord note (cm bit k is 0) */
    j6s_put(s, v, k + 1u, cm);
    s->flags = (uint8_t)(s->flags & ~J6_STF_TIE);
    return J6_EDIT_ADDED;
}

/* SQ-11, SQ-13: the same chord again removes the chord's notes (single notes added to it stay);
 * otherwise the chord replaces the step's notes, with its tag and mask, and clears the tie */
uint32_t j6_step_key_chord(j6_pattern_t *p, uint32_t step, const uint8_t *notes, uint32_t n, uint32_t tag)
{
    uint8_t v[J6S_PUT_MAX];
    uint32_t i, k, cm, valid = 0;
    j6_step_t *s;
    if (!p || step >= J6_STEPS) return J6_EDIT_NONE;
    s = &p->step[step];
    if (j6s_tag_ok(tag) && s->chord == tag && J6S_MASK_OF(s)) {
        uint32_t kk = 0;
        uint8_t w[J6_STEP_NOTES];
        k = j6s_get(s, v, &cm);
        for (i = 0; i < k; i++)
            if (!(cm & (1u << i))) w[kk++] = v[i];
        j6s_put(s, w, kk, 0);
        s->chord = J6_NO_CHORD;
        return J6_EDIT_REMOVED;
    }
    if (!notes) return J6_EDIT_NONE;
    if (n > J6S_PUT_MAX) n = J6S_PUT_MAX;
    for (i = 0; i < n; i++)
        if (notes[i] <= J6S_NOTE_MAX) valid++;
    if (!valid) return J6_EDIT_NONE;     /* every note off the MIDI range (KB-6): nothing to enter */
    s->chord = j6s_tag_ok(tag) ? (uint8_t)tag : (uint8_t)J6_NO_CHORD;
    j6s_put(s, notes, n, j6s_tag_ok(tag) ? 0xFFu : 0u);
    s->flags = (uint8_t)(s->flags & ~J6_STF_TIE);
    return J6_EDIT_REPLACED;
}

/* SQ-17: HOLD in step edit */
uint32_t j6_step_tie(j6_pattern_t *p, uint32_t step, uint8_t *target)
{
    uint32_t k, last;
    j6_step_t *s;
    if (target) *target = J6_NONE;
    if (!p || step >= J6_STEPS) return J6_EDIT_NONE;
    s = &p->step[step];
    if (s->flags & J6_STF_TIE) {         /* the selected step is a tie: remove it */
        s->flags = (uint8_t)(s->flags & ~J6_STF_TIE);
        if (target) *target = (uint8_t)step;
        return J6_EDIT_TIE_REMOVED;
    }
    if (!j6_step_count(s)) return J6_EDIT_NONE;
    last = j6s_last(p);
    for (k = step + 1u; k < last && (p->step[k].flags & J6_STF_TIE); k++) {
    }
    if (k >= last) return J6_EDIT_NONE;  /* ties do not wrap past the last step */
    if (j6_step_count(&p->step[k])) return J6_EDIT_NONE;   /* the target must be empty */
    p->step[k].flags = (uint8_t)(p->step[k].flags | J6_STF_TIE);
    if (target) *target = (uint8_t)k;
    return J6_EDIT_TIE_ADDED;
}

/* SQ-15 */
void j6_step_set_phrase(j6_pattern_t *p, uint32_t step, uint32_t phrase)
{
    if (!p || step >= J6_STEPS) return;
    p->step[step].phrase = (uint8_t)phrase;
}

/* ---- pattern edit (SQ-5, SQ-42, SQ-43) ------------------------------------------------------------ */

/* SQ-42 INSR: `step` and the steps after it move one later, step 64 is lost, last + 1 */
void j6_pattern_insert(j6_pattern_t *p, uint32_t step)
{
    uint32_t i, last;
    if (!p || step >= J6_STEPS) return;
    for (i = J6_STEPS - 1u; i > step; i--) p->step[i] = p->step[i - 1u];
    j6_step_clear(&p->step[step]);
    last = j6s_last(p) + 1u;
    p->last = (uint8_t)(last > J6_STEPS ? J6_STEPS : last);
}

/* SQ-43 DUPL: steps 1..last copied to last+1..2*last, stopping at step 64; last doubles (at most 64) */
void j6_pattern_dupl(j6_pattern_t *p)
{
    uint32_t i, last;
    if (!p) return;
    last = j6s_last(p);
    for (i = 0; i < last && last + i < J6_STEPS; i++) p->step[last + i] = p->step[i];
    p->last = (uint8_t)(2u * last > J6_STEPS ? J6_STEPS : 2u * last);
}

void j6_pattern_set_last(j6_pattern_t *p, uint32_t last)
{
    if (!p) return;
    p->last = (uint8_t)(last < 1u ? 1u : last > J6_STEPS ? J6_STEPS : last);
}

/* SQ-5: choosing page p (0-based) with p*8 + 1 > last sets last = 8*(p+1) */
void j6_pattern_page(j6_pattern_t *p, uint32_t page)
{
    if (!p || page >= J6_PAGES) return;
    if (page * J6_PAGE_STEPS + 1u > j6s_last(p)) p->last = (uint8_t)(J6_PAGE_STEPS * (page + 1u));
}

uint32_t j6_pattern_step_ticks(const j6_pattern_t *p)
{
    return p ? j6_beat_ticks(p->beat) : j6_beat_ticks(J6_BEAT_4);
}

uint32_t j6_pattern_len_ticks(const j6_pattern_t *p)
{
    return p ? j6s_last(p) * j6_pattern_step_ticks(p) : 0u;
}

/* SQ-22: the window step `step` belongs to. A step with notes and no tie opens a window lasting
 * step_len * (1 + T), T = the tie steps right after it, counted no further than the last step. A tie
 * step belongs to the window before it; after a rest (or at step 1) it is an orphan: no window. Steps at
 * or after `last` are not played: no window. The phrase is left 0 (playback sets it, SQ-26). */
void j6_pattern_window_at(const j6_pattern_t *p, uint32_t step, j6_window_t *w)
{
    uint32_t i, o, k, last, sl, t = 0;
    if (!w) return;
    w->n = 0;
    for (i = 0; i < J6_STEP_NOTES; i++) w->note[i] = J6_NO_NOTE;
    w->phrase = 0;
    w->step = J6_NONE;
    w->ties = 0;
    w->start = 0;
    w->len = 0;
    if (!p || step >= J6_STEPS) return;
    last = j6s_last(p);
    if (step >= last) return;
    for (o = step; o > 0 && (p->step[o].flags & J6_STF_TIE); o--) {
    }
    if (p->step[o].flags & J6_STF_TIE) return;       /* a tie chain from step 1: orphan */
    if (!j6_step_count(&p->step[o])) return;          /* a rest, or the tie chain after one */
    for (k = o + 1u; k < last && (p->step[k].flags & J6_STF_TIE); k++) t++;
    sl = j6_pattern_step_ticks(p);
    for (i = 0; i < J6_STEP_NOTES; i++) {
        uint8_t x = p->step[o].note[i];
        if (x <= J6S_NOTE_MAX) w->note[w->n++] = x;
    }
    w->step = (uint8_t)o;
    w->ties = (uint8_t)t;
    w->start = o * sl;
    w->len = sl * (1u + t);
}

/* ---- flash format (j6.h section 5) ---------------------------------------------------------------- */

uint32_t j6_pattern_encode(const j6_pattern_t *p, uint8_t out[J6_PAT_BYTES])
{
    uint32_t i, s;
    if (!out) return 0;
    for (i = 0; i < J6_PAT_BYTES; i++) out[i] = 0;
    if (!p) return 0;
    out[0] = J6_PAT_FORMAT;
    out[1] = (uint8_t)(p->tempo10 & 0xFFu);
    out[2] = (uint8_t)(p->tempo10 >> 8);
    out[3] = p->last;
    out[4] = p->beat;
    out[5] = (uint8_t)p->shuffle;
    out[6] = p->phrase;
    out[7] = p->sound;
    out[8] = p->vol;
    out[9] = (uint8_t)p->tran;
    out[10] = p->dsync;
    out[11] = p->avel;
    out[12] = p->pchg;
    out[13] = p->sclk;
    for (i = 0; i < J6_KNOBS; i++) out[14u + i] = p->knob[i];
    for (s = 0; s < J6_STEPS; s++) {
        uint8_t *b = out + J6_PAT_HDR_BYTES + 8u * s;
        const j6_step_t *st = &p->step[s];
        for (i = 0; i < J6_STEP_NOTES; i++) b[i] = st->note[i];
        b[4] = st->flags;
        b[5] = st->phrase;
        b[6] = st->chord;
        b[7] = 0;                        /* spare */
    }
    return J6_PAT_BYTES;
}

static int32_t j6s_s8(uint8_t b)
{
    return b < 128u ? (int32_t)b : (int32_t)b - 256;
}

static uint8_t j6s_u8_max(uint8_t b, uint32_t max)
{
    return (uint8_t)(b > max ? max : b);
}

int32_t j6_pattern_decode(j6_pattern_t *p, const uint8_t in[J6_PAT_BYTES])
{
    uint32_t i, s;
    if (!p) return -1;
    j6_pattern_init(p);
    if (!in || in[0] != J6_PAT_FORMAT) return -1;
    p->tempo10 = (uint16_t)j6_clamp((int32_t)(in[1] | ((uint32_t)in[2] << 8)), (int32_t)J6_TEMPO10_MIN,
                                    (int32_t)J6_TEMPO10_MAX);
    p->last = (uint8_t)j6_clamp(in[3], 1, (int32_t)J6_STEPS);
    p->beat = j6s_u8_max(in[4], J6_BEAT_8);
    p->shuffle = (int8_t)j6_clamp(j6s_s8(in[5]), J6_SHUFFLE_MIN, J6_SHUFFLE_MAX);
    p->phrase = j6s_phr_fix(in[6]);
    p->sound = j6s_u8_max(in[7], J6_SOUNDS - 1u);
    p->vol = j6s_u8_max(in[8], 127u);
    p->tran = (int8_t)j6_clamp(j6s_s8(in[9]), J6_KEYTR_MIN, J6_KEYTR_MAX);
    p->dsync = in[10] ? 1u : 0u;
    p->avel = (uint8_t)j6_clamp(in[11], 1, 127);
    p->pchg = j6s_u8_max(in[12], 8u);
    p->sclk = j6s_u8_max(in[13], J6_SCLK_COUNT - 1u);
    for (i = 0; i < J6_KNOBS; i++) {
        uint8_t k = in[14u + i];
        p->knob[i] = k == J6_KNOB_UNSET ? (uint8_t)J6_KNOB_UNSET : j6s_u8_max(k, 127u);
    }
    for (s = 0; s < J6_STEPS; s++) {
        const uint8_t *b = in + J6_PAT_HDR_BYTES + 8u * s;
        j6_step_t *st = &p->step[s];
        uint32_t tag_ok = j6s_tag_ok(b[6]);
        uint32_t cm = 0, k = 0;
        uint8_t v[J6_STEP_NOTES];
        /* keep the mask bits with their notes while empty slots are squeezed out */
        for (i = 0; i < J6_STEP_NOTES; i++) {
            if (b[i] > J6S_NOTE_MAX) continue;
            if (tag_ok && (b[4] & (1u << (J6_STF_CMASK_SHIFT + i)))) cm |= 1u << k;
            v[k++] = b[i];
        }
        st->flags = (uint8_t)(b[4] & J6_STF_TIE);
        st->chord = tag_ok ? b[6] : (uint8_t)J6_NO_CHORD;
        j6s_put(st, v, k, cm);
        if (j6_step_count(st)) st->flags = (uint8_t)(st->flags & ~J6_STF_TIE);
        st->phrase = j6s_phr_fix(b[5]);
        st->spare = 0;
    }
    return 0;
}
