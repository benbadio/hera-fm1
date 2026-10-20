/* SPDX-License-Identifier: GPL-3.0-only */
/* j6_seq.c - seq module: the instance (64 working copies), transport, playback, real-time recording.
 *
 * Design: docs/J6_ENGINE_DESIGN.md 6.3 .. 6.5. Behaviour: SQ-20 .. SQ-36, SQ-40, SQ-41, SQ-45, PS-2 in
 * docs/J6_SPEC.md. Step and pattern operations are in j6_seq_pattern.c, the clock in j6_seq_clock.c.
 *
 * One tick (j6_seq_tick), position p, step length sl, len = last * sl:
 *   1. count-in: COUNTIN(beats left) every 96 ticks; after N * 96 ticks the next tick plays position 0
 *      with START(0) and REC(1) (SQ-31). Otherwise the first tick after PLAY plays from_step * sl with
 *      START(from_step) (and REC(1) when started from STANDBY).
 *   2. wrap: p >= len -> p = 0, LOOP. With J6_CFG_WRAP_KEEPS_PHRASE 0, `cur` follows the pattern
 *      phrase again (SQ-26 / Q-13).
 *   3. queued pattern (SQ-27): at the point the current pattern's P.chg gives (StEP: p % sl == 0;
 *      N: p % (N * 96) == 0) the STYLE OFF notes stop, the pattern becomes current (PATTERN), p = 0.
 *      With negative shuffle, a boundary the look-ahead opened before the change was queued is not a
 *      change point (its window already sounds); the change happens at the next one.
 *   4. step boundary (p % sl == 0): STEP(s); SQ-33 ties while a recorded key is held; SQ-26 (a step
 *      whose STYLE flag is on sets cur; otherwise cur is kept; cur follows the pattern phrase until the
 *      first flagged step after PLAY); then the window (SQ-22): a step with notes and no tie opens one
 *      with phrase cur, a tie inside the window playing continues it, anything else closes it.
 *   5. window audio: a phrase window (cur has STYLE on) pushes HELD and becomes j6_seq_window(); the
 *      glue hands it to the sequencer's phrase instance. Closing it pushes HELD with n = 0 (no HELD when
 *      a new phrase window replaces it). A STYLE OFF window plays its notes at vELo for j6_gate(len)
 *      (J6_CFG_PLAIN_LEGATO: len) ticks (SQ-23); each note keeps its own gate, a note still sounding from
 *      the window before is retriggered (NOTE_OFF, NOTE_ON). j6_seq_window() reads n = 0 for a STYLE OFF
 *      window, so a HELD always describes the phrase input (j6_pattern_window_at() gives its extent).
 *   6. shuffle (SQ-7): window starts on an off-beat sixteenth (only bEAt 1-4 puts boundaries there) move by
 *      j6_shuffle_offset(). Positive: the phrase side switches at the nominal boundary (the phrase engine
 *      computes its own shuffled hits there) and the STYLE OFF notes are parked until their tick.
 *      Negative: the window opens early; each tick looks ahead to the next boundary (at most 22 ticks
 *      away, no wrap or pattern change in between) and opens it in the tick its shifted start falls in.
 * Event rules: NOTE_OFF before NOTE_ON for the same note and sub; every NOTE_ON gets one NOTE_OFF (stop and
 * pattern changes cut what sounds); at most about 24 events per call. */
#include "j6_seq_state.h"

#define J6S_PCHG_MAX 8u
#define J6S_REC_NOTES_MAX 16u

static int j6s_push(j6_evq_t *q, uint32_t type, uint32_t sub, uint32_t note, uint32_t vel, uint32_t arg)
{
    if (!q) return -1;
    return j6_evq_push(q, type, J6_SRC_SEQ, sub, note, vel, arg);
}

static void j6s_wipe(void *dst, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = 0;
}

static uint32_t j6s_plast(const j6_pattern_t *p)
{
    return (uint32_t)j6_clamp((int32_t)p->last, 1, (int32_t)J6_STEPS);
}

static uint32_t j6s_step_same(const j6_step_t *a, const j6_step_t *b)
{
    uint32_t i;
    for (i = 0; i < J6_STEP_NOTES; i++)
        if (a->note[i] != b->note[i]) return 0;
    return a->flags == b->flags && a->phrase == b->phrase && a->chord == b->chord && a->spare == b->spare;
}

static int32_t j6s_shuf(const j6_pattern_t *p)
{
    return j6_clamp(p->shuffle, J6_SHUFFLE_MIN, J6_SHUFFLE_MAX);
}

/* SQ-27: does a queued change happen at position pos of pattern p? */
static uint32_t j6s_switch_here(const j6_pattern_t *p, uint32_t pos, uint32_t sl)
{
    uint32_t pc = p->pchg > J6S_PCHG_MAX ? J6S_PCHG_MAX : p->pchg;
    if (pc == J6_PCHG_STEP) return sl && pos % sl == 0u;
    return pos % (pc * J6_PPQN) == 0u;
}

static void j6s_win_none(j6_seq_t *s)
{
    uint32_t i;
    s->win.n = 0;
    for (i = 0; i < J6_STEP_NOTES; i++) s->win.note[i] = J6_NO_NOTE;
    s->win.phrase = s->cur_phrase;
    s->win.step = J6_NONE;
    s->win.ties = 0;
    s->win.start = 0;
    s->win.len = 0;
    s->win_inh = 0;
}

/* ---- STYLE OFF notes (SQ-23) ---------------------------------------------------------------------- */

static void j6s_plain_drop(j6_seq_t *s, uint32_t i)
{
    s->nplain--;
    s->plain_note[i] = s->plain_note[s->nplain];
    s->plain_off[i] = s->plain_off[s->nplain];
}

/* the NOTE_OFFs due up to sub `upto` of the tick that starts at elapsed sub-tick now16 */
static void j6s_plain_due(j6_seq_t *s, j6_evq_t *q, uint32_t now16, uint32_t upto)
{
    uint32_t i = 0;
    while (i < s->nplain) {
        int32_t d = (int32_t)(s->plain_off[i] - now16);
        if (d <= (int32_t)upto) {
            j6s_push(q, J6_EV_NOTE_OFF, d < 0 ? 0u : (uint32_t)d, s->plain_note[i], 0, 0);
            j6s_plain_drop(s, i);
        } else {
            i++;
        }
    }
}

/* every STYLE OFF note off now (stop, pattern change) */
static void j6s_plain_cut(j6_seq_t *s, j6_evq_t *q, uint32_t sub)
{
    while (s->nplain) {
        s->nplain--;
        j6s_push(q, J6_EV_NOTE_OFF, sub, s->plain_note[s->nplain], 0, 0);
    }
}

/* A STYLE OFF window starts at sub x of the tick at now16: the notes that ended by then go off first; a
 * note of the window that still sounds is retriggered (NOTE_OFF then NOTE_ON); every note sounds
 * j6_gate(len) ticks (len with J6_CFG_PLAIN_LEGATO), measured from its own start. */
static void j6s_plain_open(j6_seq_t *s, j6_evq_t *q, uint32_t now16, uint32_t x, const uint8_t *notes,
                           uint32_t n, uint32_t len)
{
    uint32_t i, j, g = s->cfg_plain_legato ? len : j6_gate(len);
    if (!g) g = 1;
    j6s_plain_due(s, q, now16, x);
    for (i = 0; i < n && i < J6_STEP_NOTES; i++) {
        for (j = 0; j < s->nplain; j++) {
            if (s->plain_note[j] != notes[i]) continue;
            j6s_push(q, J6_EV_NOTE_OFF, x, notes[i], 0, 0);
            j6s_plain_drop(s, j);
            break;
        }
        if (s->nplain >= J6S_PLAIN_MAX) {
            /* only after a bEAt change during playback: end the note that would end first */
            uint32_t k = 0;
            for (j = 1; j < s->nplain; j++)
                if ((int32_t)(s->plain_off[j] - s->plain_off[k]) < 0) k = j;
            j6s_push(q, J6_EV_NOTE_OFF, x, s->plain_note[k], 0, 0);
            j6s_plain_drop(s, k);
        }
        if (j6s_push(q, J6_EV_NOTE_ON, x, notes[i], s->vel, 0) == 0) {
            s->plain_note[s->nplain] = notes[i];
            s->plain_off[s->nplain] = now16 + x + g * J6_SUB;
            s->nplain++;
        }
    }
}

/* ---- windows (SQ-22, SQ-24) ----------------------------------------------------------------------- */

/* The window action of the boundary of step st: open, continue or close (SQ-22). ph is the effective
 * phrase (SQ-26), inh = 1 when it is the inherited pattern phrase. The phrase side (HELD) happens at sub xh
 * of the tick at now16; the STYLE OFF notes start xp sub-ticks after now16 (xp >= 16: parked). */
static void j6s_window(j6_seq_t *s, j6_evq_t *q, const j6_pattern_t *p, uint32_t st, uint32_t ph,
                       uint32_t inh, uint32_t now16, uint32_t xh, uint32_t xp)
{
    j6_window_t w;
    uint32_t i;
    j6_pattern_window_at(p, st, &w);
    if (w.n && w.step == st) {
        s->wstep = (uint8_t)st;
        s->park_n = 0;
        s->wplain_inh = 0;
        if (ph & J6_PHR_ON) {
            w.phrase = (uint8_t)ph;
            s->win = w;
            s->win_inh = (uint8_t)inh;
            j6s_push(q, J6_EV_HELD, xh, 0, 0, 0);
            return;
        }
        if (s->win.n) {
            j6s_win_none(s);
            j6s_push(q, J6_EV_HELD, xh, 0, 0, 0);
        }
        s->wplain_inh = (uint8_t)inh;
        if (xp >= J6_SUB) {
            for (i = 0; i < J6_STEP_NOTES; i++) s->park_note[i] = w.note[i];
            s->park_n = w.n;
            s->park_at = now16 + xp;
            s->park_len = w.len;
        } else {
            j6s_plain_open(s, q, now16, xp, w.note, w.n, w.len);
        }
        return;
    }
    if (w.n && w.step == s->wstep) return;   /* a tie step inside the window playing */
    /* a rest, an orphan tie, or a tie of a window that is not playing (PLAY from a tie step) */
    s->wstep = J6_NONE;
    s->wplain_inh = 0;
    s->park_n = 0;
    if (s->win.n) {
        j6s_win_none(s);
        j6s_push(q, J6_EV_HELD, xh, 0, 0, 0);
    }
}

/* Negative shuffle: open the next boundary's window in the tick its shifted start falls in. Not across a
 * wrap or a pattern change (step 1 of a pattern is never shifted), not while a positive-shuffle start is
 * still parked. The boundary itself then only does STEP and SQ-26. */
static void j6s_lookahead(j6_seq_t *s, j6_evq_t *q, const j6_pattern_t *p, uint32_t pos, uint32_t sl,
                          uint32_t len, uint32_t now16)
{
    int32_t sh = j6s_shuf(p), off, rel;
    uint32_t nb, nst, ph, inh;
    const j6_step_t *ns;
    if (sh >= 0 || s->park_n || !sl) return;
    nb = (pos / sl + 1u) * sl;
    if (nb >= len) return;
    if (s->queued != J6_NONE && j6s_switch_here(p, nb, sl)) return;
    if (s->pf_valid && s->pf_pos == nb) return;
    off = j6_shuffle_offset(nb * J6_SUB, sh);
    if (off >= 0) return;
    rel = (int32_t)((nb - pos) * J6_SUB) + off;
    if (rel < 0 || rel >= (int32_t)J6_SUB) return;
    nst = nb / sl;
    ns = &p->step[nst];
    if (ns->phrase & J6_PHR_ON) {
        ph = ns->phrase;
        inh = 0;
    } else if (s->cur_inherit) {
        ph = p->phrase;
        inh = 1;
    } else {
        ph = s->cur_phrase;
        inh = 0;
    }
    j6s_window(s, q, p, nst, ph, inh, now16, (uint32_t)rel, (uint32_t)rel);
    s->pf_valid = 1;
    s->pf_pos = nb;
}

/* PH-20: the pattern phrase changed outside step edit while the window playing (wstep) inherits it.
 * Style / variation: the phrase window takes the new phrase (HELD). STYLE ON -> OFF: the phrase stops
 * (HELD n = 0) and the window's notes play STYLE OFF for the rest of the window. STYLE OFF -> ON: the
 * STYLE OFF notes stop and the window becomes a phrase window with the new phrase (HELD). */
static void j6s_live_phrase(j6_seq_t *s, j6_evq_t *q, const j6_pattern_t *p, uint32_t pos, uint32_t now16)
{
    j6_window_t w;
    uint32_t ph = p->phrase, end, rest;
    if (s->win.n && s->win_inh) {
        if (ph == s->win.phrase) return;
        s->cur_phrase = (uint8_t)ph;
        if (ph & J6_PHR_ON) {
            s->win.phrase = (uint8_t)ph;
            j6s_push(q, J6_EV_HELD, 0, 0, 0, 0);
            return;
        }
        w = s->win;
        j6s_win_none(s);
        j6s_push(q, J6_EV_HELD, 0, 0, 0, 0);
        s->wplain_inh = 1;
        end = (uint32_t)w.start + w.len;
        rest = end > pos ? end - pos : 1u;   /* a window opened early (negative shuffle) counts from now */
        j6s_plain_open(s, q, now16, 0, w.note, w.n, rest);
        return;
    }
    if (s->win.n || !s->wplain_inh || s->wstep == J6_NONE || !(ph & J6_PHR_ON)) return;
    j6_pattern_window_at(p, s->wstep, &w);
    if (!w.n || w.step != s->wstep) return;
    j6s_plain_cut(s, q, 0);
    s->park_n = 0;
    s->wplain_inh = 0;
    s->cur_phrase = (uint8_t)ph;
    w.phrase = (uint8_t)ph;
    s->win = w;
    s->win_inh = 1;
    j6s_push(q, J6_EV_HELD, 0, 0, 0, 0);
}

/* ---- the instance --------------------------------------------------------------------------------- */

/* forget everything of the last run (nothing may sound: callers stop first) */
static void j6s_reset_run(j6_seq_t *s)
{
    s->el = 0;
    s->pos = 0;
    s->countin_t = 0;
    s->countin_left = 0;
    s->nplain = 0;
    s->park_n = 0;
    s->pf_valid = 0;
    s->started = 0;
    s->from_step = 0;
    s->rec = 0;
    s->rec_on_start = 0;
    s->rec_chain = J6_NONE;
    s->step = J6_NONE;
    s->wstep = J6_NONE;
    s->cur_phrase = s->pat[s->cur].phrase;
    s->cur_inherit = 1;
    j6s_win_none(s);
}

void j6_seq_init(j6_seq_t *s)
{
    uint32_t i;
    if (!s) return;
    j6s_wipe(s, sizeof *s);
    for (i = 0; i < J6_PATTERNS; i++) j6_pattern_init(&s->pat[i]);
    j6_step_clear(&s->clip);
    s->clip_ok = 0;
    s->cur = 0;
    s->queued = J6_NONE;
    s->state = J6_SEQ_STOPPED;
    s->countin = 0;
    s->vel = J6_VELO_DEF;
    s->rec_held = 0;
    s->cfg_wrap_keeps = J6_CFG_WRAP_KEEPS_PHRASE ? 1u : 0u;
    s->cfg_rec_nearest = J6_CFG_REC_NEAREST ? 1u : 0u;
    s->cfg_plain_legato = J6_CFG_PLAIN_LEGATO ? 1u : 0u;
    j6s_reset_run(s);
}

/* pattern indices are taken modulo 64 (a bad index never reaches outside the array) */
const j6_pattern_t *j6_seq_pat(const j6_seq_t *s, uint32_t idx)
{
    return s ? &s->pat[idx & (J6_PATTERNS - 1u)] : NULL;
}

j6_pattern_t *j6_seq_edit(j6_seq_t *s, uint32_t idx)
{
    if (!s) return NULL;
    idx &= J6_PATTERNS - 1u;
    s->dirty[idx >> 5] |= 1u << (idx & 31u);
    return &s->pat[idx];
}

uint32_t j6_seq_cur(const j6_seq_t *s)
{
    return s ? s->cur : 0u;
}

uint32_t j6_seq_dirty(const j6_seq_t *s, uint32_t idx)
{
    if (!s || idx >= J6_PATTERNS) return 0;
    return (s->dirty[idx >> 5] >> (idx & 31u)) & 1u;
}

void j6_seq_set_saved(j6_seq_t *s, uint32_t idx)
{
    if (!s || idx >= J6_PATTERNS) return;
    s->dirty[idx >> 5] &= ~(1u << (idx & 31u));
}

/* SQ-45: the working copy, unsaved edits included; dst becomes unsaved (the glue saves it) */
void j6_seq_copy_pattern(j6_seq_t *s, uint32_t src, uint32_t dst)
{
    j6_pattern_t *d;
    if (!s || src >= J6_PATTERNS || dst >= J6_PATTERNS) return;
    d = j6_seq_edit(s, dst);
    if (src != dst) *d = s->pat[src];
}

/* SQ-40 */
void j6_seq_step_copy(j6_seq_t *s, uint32_t step)
{
    if (!s || step >= J6_STEPS) return;
    s->clip = s->pat[s->cur].step[step];
    s->clip_ok = 1;
}

/* SQ-41 */
int32_t j6_seq_step_paste(j6_seq_t *s, uint32_t step)
{
    if (!s || !s->clip_ok || step >= J6_STEPS) return -1;
    j6_seq_edit(s, s->cur)->step[step] = s->clip;
    return 0;
}

void j6_seq_set_vel(j6_seq_t *s, uint32_t vel)
{
    if (!s) return;
    s->vel = (uint8_t)(vel < 1u ? 1u : vel > 127u ? 127u : vel);
}

/* Cnt.I: 0 = off, 2..4 (1 reads as 2) */
void j6_seq_set_countin(j6_seq_t *s, uint32_t beats)
{
    if (!s) return;
    s->countin = (uint8_t)(beats == 0u ? 0u : beats < 2u ? 2u : beats > 4u ? 4u : beats);
}

/* ---- transport (design 6.3) ----------------------------------------------------------------------- */

void j6_seq_select(j6_seq_t *s, uint32_t idx, j6_evq_t *q)
{
    if (!s || idx >= J6_PATTERNS) return;
    if (s->state == J6_SEQ_PLAYING && s->started) {          /* SQ-27: queued */
        s->queued = (idx == s->cur || idx == s->queued) ? (uint8_t)J6_NONE : (uint8_t)idx;
        return;
    }
    s->cur = (uint8_t)idx;                                    /* SQ-28: immediate */
    s->queued = J6_NONE;
    if (s->state == J6_SEQ_STOPPED || s->state == J6_SEQ_STANDBY) s->cur_phrase = s->pat[idx].phrase;
    j6s_push(q, J6_EV_PATTERN, 0, 0, 0, idx);
}

uint32_t j6_seq_queued(const j6_seq_t *s)
{
    return s ? s->queued : (uint32_t)J6_NONE;
}

void j6_seq_play(j6_seq_t *s, uint32_t from_step, j6_evq_t *q)
{
    uint32_t standby;
    J6_UNUSED(q);                        /* START comes with the first tick */
    if (!s || (s->state != J6_SEQ_STOPPED && s->state != J6_SEQ_STANDBY)) return;
    standby = s->state == J6_SEQ_STANDBY;
    j6s_reset_run(s);
    if (standby && s->countin) {         /* SQ-31: count-in, then play and record from step 1 */
        s->state = J6_SEQ_COUNTIN;
        s->countin_left = s->countin;
        return;
    }
    s->from_step = (uint8_t)(from_step < J6_STEPS ? from_step : 0u);
    s->rec_on_start = (uint8_t)standby;  /* SQ-30 */
    s->state = J6_SEQ_PLAYING;
}

void j6_seq_stop(j6_seq_t *s, j6_evq_t *q)
{
    if (!s || s->state == J6_SEQ_STOPPED) return;
    j6s_plain_cut(s, q, 0);
    s->park_n = 0;
    s->pf_valid = 0;
    if (s->win.n) {
        j6s_win_none(s);
        j6s_push(q, J6_EV_HELD, 0, 0, 0, 0);
    }
    if (s->rec) {
        s->rec = 0;
        j6s_push(q, J6_EV_REC, 0, 0, 0, 0);
    }
    if (s->started) j6s_push(q, J6_EV_STOP, 0, 0, 0, 0);   /* only after a START */
    s->state = J6_SEQ_STOPPED;
    s->started = 0;
    s->rec_on_start = 0;
    s->rec_chain = J6_NONE;
    s->step = J6_NONE;
    s->wstep = J6_NONE;
    s->pos = 0;
    s->countin_t = 0;
    s->countin_left = 0;
    if (s->queued != J6_NONE) {          /* [ASSUMED] a pattern still queued at stop is selected now */
        s->cur = s->queued;
        s->queued = J6_NONE;
        j6s_push(q, J6_EV_PATTERN, 0, 0, 0, s->cur);
    }
    s->cur_phrase = s->pat[s->cur].phrase;
}

void j6_seq_rec(j6_seq_t *s, j6_evq_t *q)
{
    if (!s) return;
    switch (s->state) {
    case J6_SEQ_STOPPED:
        s->state = J6_SEQ_STANDBY;
        break;
    case J6_SEQ_STANDBY:
        s->state = J6_SEQ_STOPPED;
        break;
    case J6_SEQ_COUNTIN:                 /* [ASSUMED] */
        j6_seq_stop(s, q);
        break;
    default:                             /* PLAYING: punch in / out (SQ-35) */
        if (!s->started) {
            s->rec_on_start = (uint8_t)!s->rec_on_start;
            break;
        }
        s->rec = (uint8_t)!s->rec;
        if (!s->rec) s->rec_chain = J6_NONE;
        j6s_push(q, J6_EV_REC, 0, 0, 0, s->rec);
        break;
    }
}

/* ---- one tick (design 6.4) ------------------------------------------------------------------------ */

static void j6s_countin_tick(j6_seq_t *s, j6_evq_t *q)
{
    uint32_t beats = s->countin;
    if (s->countin_t < beats * J6_PPQN && s->countin_t % J6_PPQN == 0u) {
        uint32_t left = beats - s->countin_t / J6_PPQN;
        s->countin_left = (uint8_t)left;
        j6s_push(q, J6_EV_COUNTIN, 0, 0, 0, left);
    }
    s->countin_t++;
    if (s->countin_t >= beats * J6_PPQN) {   /* the next tick plays step 1 and records */
        s->state = J6_SEQ_PLAYING;
        s->started = 0;
        s->from_step = 0;
        s->rec_on_start = 1;
        s->el = 0;
    }
}

void j6_seq_tick(j6_seq_t *s, j6_evq_t *q)
{
    const j6_pattern_t *p;
    uint32_t pos, sl, len, now16, wrapped = 0, switched = 0;
    if (!s) return;
    if (s->state == J6_SEQ_COUNTIN) {
        j6s_countin_tick(s, q);
        return;
    }
    if (s->state != J6_SEQ_PLAYING) return;
    now16 = s->el * J6_SUB;
    p = &s->pat[s->cur];
    sl = j6_pattern_step_ticks(p);
    len = j6_pattern_len_ticks(p);

    /* 1. the first tick, or the next position; 2. wrap */
    if (!s->started) {
        uint32_t from = s->from_step < j6s_plast(p) ? s->from_step : 0u;
        s->started = 1;
        s->countin_left = 0;
        s->cur_inherit = 1;              /* SQ-26 rule 3 */
        pos = from * sl;
        j6s_push(q, J6_EV_START, 0, 0, 0, from);
        if (s->rec_on_start) {
            s->rec_on_start = 0;
            s->rec = 1;
            j6s_push(q, J6_EV_REC, 0, 0, 0, 1);
        }
    } else {
        pos = s->pos + 1u;
        if (pos >= len) {
            pos = 0;
            wrapped = 1;
            if (!s->cfg_wrap_keeps) s->cur_inherit = 1;   /* Q-13 */
        }
    }

    /* 3. queued pattern change (SQ-27). A boundary whose window the look-ahead already opened (negative
     * shuffle; the pattern was queued after that) has begun sounding: the change waits for the next
     * boundary, so the old step plays whole instead of being cut a few ticks in. The look-ahead never
     * opens a boundary early once a change is queued for it, so this only defers one step. */
    if (s->queued != J6_NONE && j6s_switch_here(p, pos, sl) &&
        !(!wrapped && s->pf_valid && s->pf_pos == pos)) {
        j6s_plain_due(s, q, now16, 0);
        j6s_plain_cut(s, q, 0);
        s->park_n = 0;
        s->wstep = J6_NONE;
        s->cur = s->queued;
        s->queued = J6_NONE;
        s->cur_inherit = 1;
        j6s_push(q, J6_EV_PATTERN, 0, 0, 0, s->cur);
        p = &s->pat[s->cur];
        sl = j6_pattern_step_ticks(p);
        len = j6_pattern_len_ticks(p);
        pos = 0;
        switched = 1;
    }
    if (wrapped && !switched) j6s_push(q, J6_EV_LOOP, 0, 0, 0, 0);
    if (wrapped || switched) s->pf_valid = 0;
    s->pos = pos;

    /* 4. step boundary */
    if (pos % sl == 0u) {
        uint32_t st = pos / sl, last = j6s_plast(p);
        const j6_step_t *stp = &p->step[st];
        s->step = (uint8_t)st;
        j6s_push(q, J6_EV_STEP, 0, 0, 0, st);
        /* SQ-33: a recorded key held across the boundary ties into an empty step (a step that is a tie
         * already, from an earlier loop, continues the chain without being written again) */
        if (s->rec && s->rec_held && s->rec_chain != J6_NONE && st != s->rec_chain) {
            if (st == s->rec_chain + 1u && st < last && !j6_step_count(stp)) {
                if (!(stp->flags & J6_STF_TIE)) {
                    j6_pattern_t *e = j6_seq_edit(s, s->cur);
                    e->step[st].flags = (uint8_t)(e->step[st].flags | J6_STF_TIE);
                }
                s->rec_chain = (uint8_t)st;
            } else {
                s->rec_chain = J6_NONE;
            }
        }
        /* SQ-26 */
        if (s->cur_inherit) s->cur_phrase = p->phrase;
        if (stp->phrase & J6_PHR_ON) {
            s->cur_phrase = stp->phrase;
            s->cur_inherit = 0;
        }
        if (s->pf_valid && s->pf_pos == pos) {
            s->pf_valid = 0;             /* already opened by the look-ahead (negative shuffle) */
        } else {
            int32_t off = j6_shuffle_offset(pos * J6_SUB, j6s_shuf(p));
            s->pf_valid = 0;
            j6s_window(s, q, p, st, s->cur_phrase, s->cur_inherit, now16, 0, off > 0 ? (uint32_t)off : 0u);
        }
    }

    /* 6. a parked STYLE OFF window start (positive shuffle) */
    if (s->park_n) {
        int32_t d = (int32_t)(s->park_at - now16);
        if (d < (int32_t)J6_SUB) {
            j6s_plain_open(s, q, now16, d < 0 ? 0u : (uint32_t)d, s->park_note, s->park_n, s->park_len);
            s->park_n = 0;
        }
    }

    /* PH-20 / SQ-26: the pattern phrase changed while the window playing inherits it: it applies at once */
    if (s->cur_inherit) j6s_live_phrase(s, q, p, pos, now16);

    j6s_lookahead(s, q, p, pos, sl, len, now16);
    j6s_plain_due(s, q, now16, J6_SUB - 1u);
    s->el++;
}

/* ---- queries -------------------------------------------------------------------------------------- */

uint32_t j6_seq_state(const j6_seq_t *s)
{
    return s ? s->state : (uint32_t)J6_SEQ_STOPPED;
}

uint32_t j6_seq_recording(const j6_seq_t *s)
{
    return s ? s->rec : 0u;
}

uint32_t j6_seq_pos(const j6_seq_t *s)
{
    return s ? s->pos : 0u;
}

uint32_t j6_seq_step_now(const j6_seq_t *s)
{
    if (!s || s->state != J6_SEQ_PLAYING || !s->started) return J6_NONE;
    return s->step;
}

uint32_t j6_seq_countin_left(const j6_seq_t *s)
{
    return s ? s->countin_left : 0u;
}

const j6_window_t *j6_seq_window(const j6_seq_t *s)
{
    return s ? &s->win : NULL;
}

/* ---- real-time recording (SQ-32 .. SQ-36) --------------------------------------------------------- */

void j6_seq_rec_input(j6_seq_t *s, const uint8_t *notes, uint32_t n, uint32_t tag, uint32_t phrase)
{
    const j6_pattern_t *p;
    j6_pattern_t *e;
    j6_step_t was;
    uint32_t sl, last, st, i, valid = 0;
    if (!s || !notes || !n || s->state != J6_SEQ_PLAYING || !s->started || !s->rec) return;
    if (n > J6S_REC_NOTES_MAX) n = J6S_REC_NOTES_MAX;
    for (i = 0; i < n; i++)
        if (notes[i] <= 127u) valid++;
    if (!valid) return;
    p = &s->pat[s->cur];
    sl = j6_pattern_step_ticks(p);
    last = j6s_plast(p);
    if (s->cfg_rec_nearest) {            /* Q-17: the nearest step start (the loop start after the end) */
        st = (s->pos + sl / 2u) / sl;
        if (st >= last) st = 0;
    } else {                             /* SQ-32: the step playing */
        st = s->pos / sl;
        if (st >= last) st = last - 1u;
    }
    e = &s->pat[s->cur];                 /* written in place; unsaved only if the step really changed */
    was = e->step[st];
    if (tag != J6_NO_CHORD) {            /* SQ-33: a chord replaces the step (never toggles it off) */
        j6_step_t *x = &e->step[st];
        if (x->chord == tag && (x->flags & J6_STF_CMASK))
            j6_step_key_chord(e, st, notes, n, tag);   /* the same chord: take it out first */
        j6_step_key_chord(e, st, notes, n, tag);
    } else {                             /* single notes add up to 4 and never delete */
        for (i = 0; i < n; i++) {
            if (notes[i] > 127u || j6_step_find(&e->step[st], notes[i]) >= 0) continue;
            if (j6_step_key_note(e, st, notes[i]) == J6_EDIT_FULL) break;
        }
    }
    if (phrase & J6_PHR_ON) {            /* SQ-34: STYLE ON writes the step phrase (a bad code reads 1-1) */
        uint32_t code = phrase & 0x7Fu;
        e->step[st].phrase = (uint8_t)(J6_PHR_ON | (code < J6_STYLES * J6_VARS ? code : 0u));
    }
    if (!j6s_step_same(&was, &e->step[st])) j6_seq_edit(s, s->cur);
    s->rec_chain = (uint8_t)st;
}

void j6_seq_rec_held(j6_seq_t *s, uint32_t any_held)
{
    if (!s) return;
    s->rec_held = any_held ? 1u : 0u;
    if (!any_held) s->rec_chain = J6_NONE;
}
