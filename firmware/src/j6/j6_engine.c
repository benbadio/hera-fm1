/* SPDX-License-Identifier: GPL-3.0-only */
/* j6_engine.c - the glue: wires chord, phrase, seq and synth to each other, to the FM-1 keys, to MIDI and
 * to storage. API: j6.h section 10. Design: docs/J6_ENGINE_DESIGN.md 2.3, 3 and 8.
 *
 * Threads. Every j6_engine_* input function (key, MIDI in, cmd, set) runs in the main loop and only puts
 * a command into a single-producer / single-consumer ring; j6_engine_render() (the audio interrupt)
 * applies the commands, then runs time. Storage work (write, poll, factory reset) sets `busy` first: the
 * chip has one core, so once busy is published no render runs until it is cleared, and the main loop may
 * touch the module state. Getters read state that may be one block old.
 *
 * Time (design 2.3). Per J6_CR chunk: the clock gives the ticks that began; for each tick the pending
 * sub-tick events of the previous tick fire, then seq -> (HELD) -> seq phrase -> live phrase run and their
 * events are routed; MIDI clock goes out every 4 ticks. Then pending events whose sub the clock has
 * reached fire, and the synth renders the chunk.
 *
 * Routing. Every NOTE_ON / NOTE_OFF reaches the synth as note + trAn (KB-5; outside 0..127 dropped,
 * KB-6). A (src, note) -> synth note map makes the NOTE_OFF match even when trAn changed in between, and
 * a per-note count lets two producers share a synth note (the voice is released when the last one ends).
 * MIDI out (MI-2, design 8.3) is never transposed, goes on CH, and is counted per note the same way.
 *
 * Live input (design 8.2). Two held trackers: keys (poly by MIDI note, or mono by chord key code + 64 in
 * CHORD mode, CH-6) and MIDI in (poly, CH-9). With the live phrase on (the pattern phrase's STYLE flag,
 * PH-20, outside step edit) their merged set is H of the live phrase instance; otherwise it sounds as
 * direct notes (src KEY / CHORD / MIDI). While the sequencer plays the live phrase runs on the pattern
 * position (PH-21); stopped it runs on its own pt, which restarts at 0 on the press that makes H
 * non-empty (PH-5).
 *
 * Decisions not fixed by the spec ([ASSUMED], marked in the code too):
 *   - entering step edit turns the HOLD latch off (HOLD then writes ties, SQ-17), unless the global
 *     HoLd.E = KEEP (D1, docs/J6_DECISIONS.md);
 *   - on STOP only the sequencer's phrase stops; a live phrase still held carries on on its own pt;
 *     STOP again while stopped kills all sound with a 6 ms fade (D2, like an MPC);
 *   - a chord change keeps the notes both chords share sounding, unless cHd.L = OFF (D3);
 *   - SYnC MIDI / USB behave like AUTO restricted to that port (the internal clock takes over 500 ms after
 *     the last pulse);
 *   - Active Sensing (0xFE) is sent from the render every 250 ms, so the MIDI-out ring has one producer;
 *   - a pattern-copy destination (SQ-45) is written to storage from j6_engine_poll();
 *   - no Program Change is sent for the pattern selected at boot. */
#include "j6_phrase_state.h"
#include "j6_seq_state.h"
#include "j6_synth_state.h"

#ifndef J6E_PUBLISH
#define J6E_PUBLISH() ((void)0)      /* the app defines it as the platform's RING_PUBLISH() barrier */
#endif
#ifndef J6E_SEQ_SECTION
#define J6E_SEQ_SECTION              /* the app may put the 34 KB sequencer in .pool */
#endif

#define J6E_CMD_RING 256u            /* power of two; 4x the platform's USB-MIDI queue (usb.c MQ 64) */
#define J6E_CMD_RESERVE 32u          /* the last slots take only releases (key up, Note Off, CC 64/120/
                                        121/123, Stop): a burst of other input never loses an end */
#define J6E_MO_RING 128u             /* power of two */
#define J6E_PEND_MAX 32u
#define J6E_MAP_MAX 64u
#define J6E_DIRECT_MAX 8u
#define J6E_LIVE_MAX (2u * J6_HELD_MAX)
#define J6E_AS_OUT_FRAMES 11025u     /* 250 ms: Active Sensing out (MI-5) */
#define J6E_AS_IN_FRAMES 13230u      /* 300 ms: Active Sensing in timeout (MI-5) */
#define J6E_GSAVE_FRAMES (2u * J6_FS)    /* PS-3: globals saved 2 s after the last change */
#define J6E_ID_NONE 0xFFu
#define J6E_CHORD_ID 64u             /* mono tracker ids: chord key code + 64 */

enum { J6E_C_KEY = 0, J6E_C_MIDI, J6E_C_CMD, J6E_C_SET };

typedef struct {
    uint8_t type;                    /* J6E_C_* */
    uint8_t a, b, c;                 /* KEY: down; MIDI: st d1 d2; CMD: cmd; SET: param */
    int32_t v;                       /* KEY: key code; MIDI: port; CMD / SET: argument */
} j6e_cmd_t;

typedef struct {
    j6_event_t ev;
    uint32_t tick;
} j6e_pend_t;

typedef struct {
    uint8_t src, note, snote, used;
} j6e_map_t;

typedef struct {
    uint8_t note, src, vel, reserved;
} j6e_direct_t;

struct j6e_state {
    /* main loop -> render */
    j6e_cmd_t cmd[J6E_CMD_RING];
    volatile uint32_t cmd_w, cmd_r;
    uint32_t cmd_lost;
    /* render -> main loop */
    uint8_t mo[J6E_MO_RING][4];
    volatile uint32_t mo_w, mo_r;
    uint32_t mo_lost;
    volatile uint32_t busy;          /* storage work in the main loop: render does nothing */
    uint32_t inited;

    int16_t *fx;
    uint32_t fx_len;
    j6_store_t store;
    uint32_t have_store;

    j6_global_t g;
    uint32_t g_dirty, g_changed;     /* PS-3 */
    uint32_t copy_save[2];           /* SQ-45 destinations to write (bit per pattern) */

    /* panel */
    int8_t oct;
    uint8_t hold;                    /* latch on (outside step edit) */
    uint8_t chord;                   /* CHORD mode */
    uint8_t sel;                     /* step edit selection, J6_NONE */
    uint8_t kstyle, kvar;            /* STYLE / VARIATION knob positions */
    uint8_t last_note, last_chord;
    uint8_t booting;

    /* keys */
    uint8_t kid[J6_KEY_COUNT];       /* tracker id of each key down, J6E_ID_NONE when up */
    int8_t koct[J6_KEY_COUNT];       /* octave, KEY and chord set at the key's last press */
    int8_t ktr[J6_KEY_COUNT];
    uint8_t kset[J6_KEY_COUNT];
    uint8_t mvel[128];               /* MIDI-in velocity per note (direct notes, MI-3) */

    /* sounding */
    j6e_direct_t dir[J6E_DIRECT_MAX];
    uint8_t ndir;
    uint8_t live_n;                  /* notes given to the live phrase */
    uint8_t live_style, live_var;
    uint8_t scount[128];             /* producers sounding each synth note */
    uint8_t ocount[128];             /* note-ons sent on MIDI out per note, not yet ended */
    j6e_map_t map[J6E_MAP_MAX];
    j6e_pend_t pend[J6E_PEND_MAX];
    uint32_t npend;

    /* time */
    uint32_t frames;                 /* frames run since init */
    uint32_t tick;                   /* ticks begun since init */
    uint32_t live_pt;                /* pt of the live phrase's next tick */
    uint32_t seq_pos;                /* last position the sequencer played */
    uint16_t synth_tempo;            /* tempo10 last given to the synth */
    uint8_t as_armed;                /* Active Sensing received (MI-5) */
    uint8_t ext;                     /* the clock follows MIDI clock in */
    uint32_t as_last;                /* frame of the last message received */
    uint32_t as_next;                /* frame of the next 0xFE out */

    j6_evq_t q, q2;
    j6_tap_fn tap;
    void *tap_ctx;
};

J6_STATIC_ASSERT(engine_fits_budget, sizeof(struct j6e_state) <= J6_RAM_ENGINE_MAX);

static struct j6e_state j6e_s;
static j6_seq_t j6e_seq J6E_SEQ_SECTION;
static j6_clock_t j6e_clock;
static j6_phrase_t j6e_plive, j6e_pseq;
static j6_held_t j6e_hkeys, j6e_hmidi;
static j6_synth_t j6e_synth;

/* ---- parameters ----------------------------------------------------------------------------------- */

static const j6_param_info_t j6e_params[J6_P_COUNT] = {
    /* min, max, def, per_pattern, name */
    {(int16_t)J6_TEMPO10_MIN, (int16_t)J6_TEMPO10_MAX, (int16_t)J6_TEMPO10_DEF, 1, "tEMPo"},
    {1, (int16_t)J6_STEPS, (int16_t)J6_LAST_DEF, 1, "LASt"},
    {0, J6_BEAT_COUNT - 1, J6_BEAT_4, 1, "bEAt"},
    {J6_SHUFFLE_MIN, J6_SHUFFLE_MAX, 0, 1, "SHUF"},
    {0, 127, (int16_t)J6_VOL_DEF, 1, "vOL"},
    {-12, 12, 0, 1, "trAn"},
    {0, 1, 0, 1, "d.Syn"},
    {1, 127, (int16_t)J6_AVEL_DEF, 1, "A.vLo"},
    {0, 8, (int16_t)J6_PCHG_STEP, 1, "P.chg"},
    {0, (int16_t)J6_SCLK_COUNT - 1, (int16_t)J6_SCLK_DEF, 1, "S.cLk"},
    {0, (int16_t)J6_SOUNDS - 1, 0, 1, "Snd"},
    {0, 127, 64, 1, "CUt"},
    {0, 127, 64, 1, "rES"},
    {0, 127, 64, 1, "rEL"},
    {0, 127, 64, 1, "Att"},
    {0, 127, 64, 1, "dLy"},
    {0, 127, 64, 1, "dLy.t"},
    {0, 127, 64, 1, "rEv"},
    {0, 127, 64, 1, "rEv.t"},
    {0, 1, 0, 1, "StyL"},
    {1, (int16_t)J6_STYLES, 1, 1, "StyLE"},
    {1, (int16_t)J6_VARS, 1, 1, "vAr"},
    {1, 16, 1, 0, "CH"},
    {J6_SYNC_AUTO, J6_SYNC_USB, J6_SYNC_AUTO, 0, "SYnC"},
    {0, 1, 0, 0, "thrv"},
    {0, 1, 1, 0, "txPc"},
    {0, 1, 1, 0, "rxPc"},
    {1, 16, 16, 0, "Pc.Ch"},
    {1, 127, (int16_t)J6_VELO_DEF, 0, "vELo"},
    {(int16_t)J6_TUNE10_MIN, (int16_t)J6_TUNE10_MAX, (int16_t)J6_TUNE10_DEF, 0, "tUnE"},
    {0, 127, 0, 0, "USb.d"},
    {0, 1, 0, 0, "A.LnK"},
    {0, 4, 0, 0, "Cnt.I"},
    {0, (int16_t)J6_CHORD_SETS - 1, 0, 0, "Chrd"},
    {J6_KEYTR_MIN, J6_KEYTR_MAX, 0, 0, "KEY"},
    {0, 1, 0, 0, "HoLd.E"},
    {0, 1, 1, 0, "cHd.L"},
    {J6_OCT_MIN, J6_OCT_MAX, 0, 0, "Oct"},
    {0, 1, 0, 0, "HOLd"},
    {0, 1, 0, 0, "CHrd"},
};

const j6_param_info_t *j6_param_info(uint32_t id)
{
    return id < J6_P_COUNT ? &j6e_params[id] : NULL;
}

static int32_t j6e_pclamp(uint32_t id, int32_t v)
{
    return j6_clamp(v, j6e_params[id].min, j6e_params[id].max);
}

/* ---- small helpers -------------------------------------------------------------------------------- */

static void j6e_zero(void *dst, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = 0;
}

static const j6_pattern_t *j6e_pat(void)
{
    return j6_seq_pat(&j6e_seq, j6_seq_cur(&j6e_seq));
}

static uint32_t j6e_playing(void)
{
    return j6_seq_state(&j6e_seq) == J6_SEQ_PLAYING && j6_seq_step_now(&j6e_seq) != J6_NONE;
}

static uint32_t j6e_step_edit(void)
{
    return j6e_s.sel < J6_STEPS;
}

static void j6e_tap(const j6_event_t *e, uint32_t tick, uint32_t snote)
{
    j6_tap_t t;
    if (!j6e_s.tap) return;
    t.frame = j6e_s.frames;
    t.tick = tick;
    t.ev = *e;
    t.synth_note = (uint8_t)snote;
    t.reserved[0] = t.reserved[1] = t.reserved[2] = 0;
    j6e_s.tap(j6e_s.tap_ctx, &t);
}

/* ---- MIDI out ring -------------------------------------------------------------------------------- */

static void j6e_mo(uint32_t st, uint32_t d1, uint32_t d2)
{
    uint32_t w = j6e_s.mo_w;
    if (w - j6e_s.mo_r >= J6E_MO_RING) {
        j6e_s.mo_lost++;
        return;
    }
    j6e_s.mo[w & (J6E_MO_RING - 1u)][0] = (uint8_t)st;
    j6e_s.mo[w & (J6E_MO_RING - 1u)][1] = (uint8_t)(d1 & 0x7Fu);
    j6e_s.mo[w & (J6E_MO_RING - 1u)][2] = (uint8_t)(d2 & 0x7Fu);
    J6E_PUBLISH();
    j6e_s.mo_w = w + 1u;
}

uint32_t j6_engine_midi_out(uint8_t *st, uint8_t *d1, uint8_t *d2)
{
    uint32_t r = j6e_s.mo_r;
    if (r == j6e_s.mo_w) return 0;
    if (st) *st = j6e_s.mo[r & (J6E_MO_RING - 1u)][0];
    if (d1) *d1 = j6e_s.mo[r & (J6E_MO_RING - 1u)][1];
    if (d2) *d2 = j6e_s.mo[r & (J6E_MO_RING - 1u)][2];
    J6E_PUBLISH();
    j6e_s.mo_r = r + 1u;
    return 1;
}

/* MI-2 / design 8.3: which producers are heard on MIDI out */
static uint32_t j6e_src_out(uint32_t src)
{
    if (src == J6_SRC_MIDI) return 0;
    if (src == J6_SRC_CHORD) return J6_CFG_CHORD_MIDI_OUT ? 1u : 0u;
    return src < J6_SRC_COUNT;
}

static void j6e_out_note(uint32_t on, uint32_t note, uint32_t vel)
{
    uint32_t ch = (uint32_t)(j6e_s.g.ch - 1u) & 0x0Fu;
    if (note > 127u) return;
    if (on) {
        if (j6e_s.ocount[note] < 255u) j6e_s.ocount[note]++;
        j6e_mo(0x90u | ch, note, vel ? vel : 1u);
    } else if (j6e_s.ocount[note]) {
        if (--j6e_s.ocount[note] == 0u) j6e_mo(0x80u | ch, note, 0);
    }
}

/* every note sent and not ended gets its Note Off (panic) */
static void j6e_out_all_off(void)
{
    uint32_t n, ch = (uint32_t)(j6e_s.g.ch - 1u) & 0x0Fu;
    for (n = 0; n < 128u; n++)
        if (j6e_s.ocount[n]) {
            j6e_s.ocount[n] = 0;
            j6e_mo(0x80u | ch, n, 0);
        }
}

/* ---- notes to the synth --------------------------------------------------------------------------- */

static uint32_t j6e_tran_note(uint32_t note)
{
    int32_t n = (int32_t)note + j6e_pat()->tran;
    return n < 0 || n > 127 ? (uint32_t)J6_NO_NOTE : (uint32_t)n;
}

static void j6e_note_on(const j6_event_t *e, uint32_t tick)
{
    uint32_t i, sn = j6e_tran_note(e->note);
    for (i = 0; i < J6E_MAP_MAX; i++)
        if (!j6e_s.map[i].used) {
            j6e_s.map[i].used = 1;
            j6e_s.map[i].src = e->src;
            j6e_s.map[i].note = e->note;
            j6e_s.map[i].snote = (uint8_t)sn;
            break;
        }
    if (sn != J6_NO_NOTE) {
        if (j6e_s.scount[sn] < 255u) j6e_s.scount[sn]++;
        j6_synth_note_on(&j6e_synth, sn, e->vel ? e->vel : 1u);
    }
    if (j6e_src_out(e->src)) j6e_out_note(1, e->note, e->vel);
    j6e_tap(e, tick, sn);
}

static void j6e_note_off(const j6_event_t *e, uint32_t tick)
{
    uint32_t i, sn = J6E_ID_NONE;
    for (i = 0; i < J6E_MAP_MAX; i++)
        if (j6e_s.map[i].used && j6e_s.map[i].src == e->src && j6e_s.map[i].note == e->note) {
            sn = j6e_s.map[i].snote;
            j6e_s.map[i].used = 0;
            break;
        }
    if (i == J6E_MAP_MAX) sn = j6e_tran_note(e->note);   /* not mapped (map full, or after a panic) */
    if (sn <= 127u && j6e_s.scount[sn]) {
        if (--j6e_s.scount[sn] == 0u) j6_synth_note_off(&j6e_synth, sn);
    }
    if (j6e_src_out(e->src)) j6e_out_note(0, e->note, 0);
    j6e_tap(e, tick, sn);
}

/* ---- event routing (design 3) --------------------------------------------------------------------- */

static void j6e_route(j6_evq_t *q, uint32_t tick);
static void j6e_load_pattern(void);
static void j6e_live_update(void);

static void j6e_fire(const j6_event_t *e, uint32_t tick)
{
    if (e->type == J6_EV_NOTE_ON) j6e_note_on(e, tick);
    else if (e->type == J6_EV_NOTE_OFF) j6e_note_off(e, tick);
}

/* fire the pending sub-tick events with sub <= upto (all of them with upto >= J6_SUB), in order */
static void j6e_pend_fire(uint32_t upto)
{
    uint32_t i, k = 0;
    for (i = 0; i < j6e_s.npend; i++) {
        if (j6e_s.pend[i].ev.sub <= upto) j6e_fire(&j6e_s.pend[i].ev, j6e_s.pend[i].tick);
        else j6e_s.pend[k++] = j6e_s.pend[i];
    }
    j6e_s.npend = k;
}

/* HELD: the sequencer's phrase input changed (design 3, SQ-24) */
static void j6e_seq_held(uint32_t tick)
{
    const j6_window_t *w = j6_seq_window(&j6e_seq);
    const j6_pattern_t *p = j6e_pat();
    if (w->n) j6_phrase_set_style(&j6e_pseq, j6_phr_style(w->phrase), j6_phr_var(w->phrase));
    j6_phrase_set_vel(&j6e_pseq, j6e_s.g.velo, p->avel);
    j6_evq_clear(&j6e_s.q2);
    j6_phrase_set_held(&j6e_pseq, w->note, w->n, &j6e_s.q2);
    j6e_route(&j6e_s.q2, tick);
}

static void j6e_route(j6_evq_t *q, uint32_t tick)
{
    uint32_t i;
    for (i = 0; i < q->n; i++) {
        const j6_event_t *e = &q->ev[i];
        switch (e->type) {
        case J6_EV_NOTE_ON:
        case J6_EV_NOTE_OFF:
            if (e->sub == 0u || j6e_s.npend >= J6E_PEND_MAX) {
                j6e_fire(e, tick);
            } else {
                j6e_s.pend[j6e_s.npend].ev = *e;
                j6e_s.pend[j6e_s.npend].tick = tick;
                j6e_s.npend++;
            }
            break;
        case J6_EV_HELD:
            j6e_tap(e, tick, J6_NO_NOTE);
            j6e_seq_held(tick);
            break;
        case J6_EV_PATTERN:
            j6e_tap(e, tick, J6_NO_NOTE);
            if (j6e_s.g.pattern != e->arg) {
                j6e_s.g.pattern = e->arg;
                j6e_s.g_dirty = 1;
                j6e_s.g_changed = j6e_s.frames;
            }
            if (j6e_s.g.txpc && !j6e_s.booting) j6e_mo(0xC0u | ((uint32_t)(j6e_s.g.pcch - 1u) & 0x0Fu), e->arg, 0);
            j6e_load_pattern();
            break;
        case J6_EV_START:
            j6e_tap(e, tick, J6_NO_NOTE);
            j6e_mo(0xFAu, 0, 0);
            break;
        case J6_EV_STOP:
            j6e_tap(e, tick, J6_NO_NOTE);
            j6e_mo(0xFCu, 0, 0);
            j6_evq_clear(&j6e_s.q2);
            j6_phrase_stop(&j6e_pseq, &j6e_s.q2);
            j6e_route(&j6e_s.q2, tick);
            j6e_s.live_pt = j6e_s.seq_pos + 1u;   /* a live phrase carries on from where the pattern was */
            break;
        default:                             /* STEP, LOOP, COUNTIN, REC: status only */
            j6e_tap(e, tick, J6_NO_NOTE);
            break;
        }
    }
    q->n = 0;
}

/* a direct note (keys, MIDI in) now */
static void j6e_direct_ev(uint32_t type, uint32_t src, uint32_t note, uint32_t vel)
{
    j6_event_t e;
    e.type = (uint8_t)type;
    e.src = (uint8_t)src;
    e.sub = 0;
    e.note = (uint8_t)note;
    e.vel = (uint8_t)vel;
    e.arg = 0;
    j6e_fire(&e, j6e_s.tick);
}

/* ---- pattern settings -> modules ------------------------------------------------------------------ */

static void j6e_apply_tempo(void)
{
    const j6_pattern_t *p = j6e_pat();
    if (!j6e_s.ext) j6_clock_set_tempo(&j6e_clock, p->tempo10);
    j6e_s.synth_tempo = (uint16_t)j6_clock_tempo10(&j6e_clock);
    if (!j6e_s.ext) j6e_s.synth_tempo = p->tempo10;
    j6_synth_set(&j6e_synth, J6_SP_TEMPO10, j6e_s.synth_tempo);
}

static void j6e_apply_live_style(void)
{
    uint8_t ph = j6e_pat()->phrase;
    uint32_t st = j6_phr_style(ph), va = j6_phr_var(ph);
    if (st != j6e_s.live_style || va != j6e_s.live_var) {
        j6e_s.live_style = (uint8_t)st;
        j6e_s.live_var = (uint8_t)va;
        j6_phrase_set_style(&j6e_plive, st, va);
    }
}

static void j6e_apply_vel(void)
{
    const j6_pattern_t *p = j6e_pat();
    j6_seq_set_vel(&j6e_seq, j6e_s.g.velo);
    j6_phrase_set_vel(&j6e_plive, j6e_s.g.velo, p->avel);
    j6_phrase_set_vel(&j6e_pseq, j6e_s.g.velo, p->avel);
}

/* PATTERN (SQ-27, SQ-28): the current pattern's tempo, sound, knobs and settings */
static void j6e_load_pattern(void)
{
    const j6_pattern_t *p = j6e_pat();
    uint32_t k;
    j6e_apply_tempo();
    j6_synth_sound(&j6e_synth, p->sound);
    for (k = 0; k < J6_KNOBS; k++)
        if (p->knob[k] != J6_KNOB_UNSET) j6_synth_set(&j6e_synth, k, p->knob[k]);
    j6_synth_set(&j6e_synth, J6_SP_VOLUME, p->vol);
    j6_synth_set(&j6e_synth, J6_SP_DLY_SYNC, p->dsync);
    j6_phrase_set_shuffle(&j6e_plive, p->shuffle);
    j6_phrase_set_shuffle(&j6e_pseq, p->shuffle);
    j6e_apply_vel();
    j6e_s.kstyle = (uint8_t)j6_phr_style(p->phrase);
    j6e_s.kvar = (uint8_t)j6_phr_var(p->phrase);
    j6e_apply_live_style();
    j6e_live_update();
}

/* edits of the current pattern: unsaved only when something really changed */
static j6_pattern_t *j6e_edit_begin(uint32_t *was_dirty)
{
    uint32_t cur = j6_seq_cur(&j6e_seq);
    *was_dirty = j6_seq_dirty(&j6e_seq, cur);
    return j6_seq_edit(&j6e_seq, cur);
}

static void j6e_edit_end(uint32_t was_dirty, uint32_t changed)
{
    if (!changed && !was_dirty) j6_seq_set_saved(&j6e_seq, j6_seq_cur(&j6e_seq));
}

/* ---- live input (design 8.2) ---------------------------------------------------------------------- */

static uint32_t j6e_live_phrase_on(void)
{
    return !j6e_step_edit() && j6_phr_on(j6e_pat()->phrase);
}

/* the notes of the live trackers: keys first (src KEY / CHORD), then MIDI in; returns the count */
static uint32_t j6e_live_notes(uint8_t *notes, uint8_t *srcs, uint8_t *vels)
{
    uint8_t ids[J6_HELD_MAX];
    uint32_t n = 0, i, j, k, m;
    k = j6_held_get(&j6e_hkeys, ids);
    for (i = 0; i < k; i++) {
        if (j6e_s.chord) {
            uint8_t cn[J6_CHORD_NOTES];
            int32_t key = (int32_t)ids[i] - (int32_t)J6E_CHORD_ID;
            uint32_t ki = (uint32_t)(key - J6_KEY_MIN), c;
            if (ki >= J6_KEY_COUNT) continue;
            c = j6_chord_notes(j6e_s.kset[ki], key, j6e_s.koct[ki], j6e_s.ktr[ki], cn, NULL);
            for (j = 0; j < c && n < J6_HELD_MAX; j++) {
                notes[n] = cn[j];
                srcs[n] = J6_SRC_CHORD;
                vels[n] = j6e_s.g.velo;
                n++;
            }
        } else if (ids[i] <= 127u && n < J6_HELD_MAX) {
            notes[n] = ids[i];
            srcs[n] = J6_SRC_KEY;
            vels[n] = j6e_s.g.velo;
            n++;
        }
    }
    m = j6_held_get(&j6e_hmidi, ids);
    for (i = 0; i < m; i++) {
        for (j = 0; j < n; j++)
            if (notes[j] == ids[i]) break;
        if (j < n || ids[i] > 127u) continue;
        notes[n] = ids[i];
        srcs[n] = J6_SRC_MIDI;
        vels[n] = j6e_s.mvel[ids[i]] ? j6e_s.mvel[ids[i]] : j6e_s.g.velo;
        n++;
    }
    return n;
}

static void j6e_direct_off_all(void)
{
    while (j6e_s.ndir) {
        j6e_s.ndir--;
        j6e_direct_ev(J6_EV_NOTE_OFF, j6e_s.dir[j6e_s.ndir].src, j6e_s.dir[j6e_s.ndir].note, 0);
    }
}

/* bring the sounding live notes in line with the trackers and the mode */
static void j6e_live_update(void)
{
    uint8_t notes[J6E_LIVE_MAX], srcs[J6E_LIVE_MAX], vels[J6E_LIVE_MAX];
    uint32_t n = j6e_live_notes(notes, srcs, vels), i, j;
    if (j6e_live_phrase_on()) {
        j6e_direct_off_all();
        j6e_apply_live_style();
        if (!j6e_s.live_n && n && !j6e_playing()) j6e_s.live_pt = 0;   /* PH-5: pt from the press */
        j6_evq_clear(&j6e_s.q2);
        j6_phrase_set_held(&j6e_plive, notes, n, &j6e_s.q2);
        j6e_route(&j6e_s.q2, j6e_s.tick);
        j6e_s.live_n = (uint8_t)(n > J6_HELD_MAX ? J6_HELD_MAX : n);
        return;
    }
    if (j6e_s.live_n) {
        j6_evq_clear(&j6e_s.q2);
        j6_phrase_stop(&j6e_plive, &j6e_s.q2);
        j6e_route(&j6e_s.q2, j6e_s.tick);
        j6e_s.live_n = 0;
    }
    if (j6e_s.g.opts & J6_OPT_CHORD_RETRIG) {   /* D3: any chord change restarts every chord note */
        uint32_t changed = 0, k;
        for (i = 0; i < j6e_s.ndir && !changed; i++) {
            if (j6e_s.dir[i].src != J6_SRC_CHORD) continue;
            for (k = 0; k < n; k++)
                if (notes[k] == j6e_s.dir[i].note && srcs[k] == J6_SRC_CHORD) break;
            if (k == n) changed = 1;
        }
        for (k = 0; k < n && !changed; k++) {
            if (srcs[k] != J6_SRC_CHORD) continue;
            for (i = 0; i < j6e_s.ndir; i++)
                if (j6e_s.dir[i].note == notes[k] && j6e_s.dir[i].src == J6_SRC_CHORD) break;
            if (i == j6e_s.ndir) changed = 1;
        }
        i = 0;
        while (changed && i < j6e_s.ndir) {
            if (j6e_s.dir[i].src != J6_SRC_CHORD) {
                i++;
                continue;
            }
            j6e_direct_ev(J6_EV_NOTE_OFF, j6e_s.dir[i].src, j6e_s.dir[i].note, 0);
            j6e_s.dir[i] = j6e_s.dir[--j6e_s.ndir];
        }
    }
    i = 0;                                   /* direct notes that are no longer wanted */
    while (i < j6e_s.ndir) {
        for (j = 0; j < n; j++)
            if (notes[j] == j6e_s.dir[i].note && srcs[j] == j6e_s.dir[i].src) break;
        if (j < n) {
            i++;
            continue;
        }
        j6e_direct_ev(J6_EV_NOTE_OFF, j6e_s.dir[i].src, j6e_s.dir[i].note, 0);
        j6e_s.dir[i] = j6e_s.dir[--j6e_s.ndir];
    }
    for (j = 0; j < n; j++) {                /* and the new ones */
        for (i = 0; i < j6e_s.ndir; i++)
            if (notes[j] == j6e_s.dir[i].note && srcs[j] == j6e_s.dir[i].src) break;
        if (i < j6e_s.ndir || j6e_s.ndir >= J6E_DIRECT_MAX) continue;
        j6e_s.dir[j6e_s.ndir].note = notes[j];
        j6e_s.dir[j6e_s.ndir].src = srcs[j];
        j6e_s.dir[j6e_s.ndir].vel = vels[j];
        j6e_s.ndir++;
        j6e_direct_ev(J6_EV_NOTE_ON, srcs[j], notes[j], vels[j]);
    }
}

static void j6e_rec_held(void)
{
    j6_seq_rec_held(&j6e_seq, j6_held_down(&j6e_hkeys) + j6_held_down(&j6e_hmidi) > 0u);
}

/* SQ-34: the phrase byte recording writes (STYLE ON: the pattern phrase with its flag) */
static uint32_t j6e_rec_phrase(void)
{
    uint8_t ph = j6e_pat()->phrase;
    return j6_phr_on(ph) ? ph : 0u;
}

static void j6e_key(int32_t key, uint32_t down)
{
    uint32_t ki = (uint32_t)(key - J6_KEY_MIN), was;
    if (key < J6_KEY_MIN || key > J6_KEY_MAX) return;
    if (!down) {
        uint32_t id = j6e_s.kid[ki];
        if (id == J6E_ID_NONE) return;
        j6e_s.kid[ki] = J6E_ID_NONE;
        j6_held_release(&j6e_hkeys, id);
        j6e_rec_held();
        j6e_live_update();
        return;
    }
    if (j6e_s.kid[ki] != J6E_ID_NONE) return;
    j6e_s.koct[ki] = j6e_s.oct;
    j6e_s.ktr[ki] = j6e_s.g.key;
    j6e_s.kset[ki] = j6e_s.g.chord_set;
    if (j6e_s.chord) {
        uint8_t cn[J6_CHORD_NOTES], tag = J6_NO_CHORD;
        uint32_t c = j6_chord_notes(j6e_s.g.chord_set, key, j6e_s.oct, j6e_s.g.key, cn, &tag);
        j6e_s.kid[ki] = (uint8_t)(key + (int32_t)J6E_CHORD_ID);
        j6e_s.last_chord = tag;
        if (c) j6e_s.last_note = cn[0];
        if (j6e_step_edit() && c) {                                 /* SQ-11, SQ-13 */
            j6_pattern_t *p = j6e_edit_begin(&was);
            uint32_t r = j6_step_key_chord(p, j6e_s.sel, cn, c, tag);
            j6e_edit_end(was, r != J6_EDIT_NONE && r != J6_EDIT_FULL);
        } else if (c && j6_seq_recording(&j6e_seq)) {
            j6_seq_rec_input(&j6e_seq, cn, c, tag, j6e_rec_phrase());   /* SQ-33 */
        }
    } else {
        int32_t note = j6_key_note(key, j6e_s.oct, j6e_s.g.key);
        if (note < 0) {
            j6e_s.kid[ki] = J6E_ID_NONE;
            return;
        }
        j6e_s.kid[ki] = (uint8_t)note;
        j6e_s.last_note = (uint8_t)note;
        if (j6e_step_edit()) {                                      /* SQ-12 */
            j6_pattern_t *p = j6e_edit_begin(&was);
            uint32_t r = j6_step_key_note(p, j6e_s.sel, (uint32_t)note);
            j6e_edit_end(was, r != J6_EDIT_NONE && r != J6_EDIT_FULL);
        } else if (j6_seq_recording(&j6e_seq)) {
            uint8_t n8 = (uint8_t)note;
            j6_seq_rec_input(&j6e_seq, &n8, 1, J6_NO_CHORD, j6e_rec_phrase());
        }
    }
    j6_held_press(&j6e_hkeys, j6e_s.kid[ki]);
    j6e_rec_held();
    j6e_live_update();
}

/* ---- latch, panic --------------------------------------------------------------------------------- */

static void j6e_set_latch(uint32_t on)
{
    j6e_s.hold = on ? 1u : 0u;
    j6_held_latch(&j6e_hkeys, on);
    j6_held_latch(&j6e_hmidi, on);
    j6e_live_update();
}

/* silence now: phrases, direct notes, synth, MIDI out; with forget, also the trackers and the latch */
static void j6e_silence(uint32_t forget)
{
    j6_evq_clear(&j6e_s.q2);
    j6_phrase_stop(&j6e_plive, &j6e_s.q2);
    j6_phrase_stop(&j6e_pseq, &j6e_s.q2);
    j6e_route(&j6e_s.q2, j6e_s.tick);
    j6e_pend_fire(J6_SUB);
    j6e_s.live_n = 0;
    j6e_direct_off_all();
    if (forget) {
        uint32_t i;
        j6_held_clear(&j6e_hkeys);
        j6_held_clear(&j6e_hmidi);
        j6e_s.hold = 0;
        j6_held_latch(&j6e_hkeys, 0);
        j6_held_latch(&j6e_hmidi, 0);
        for (i = 0; i < J6_KEY_COUNT; i++) j6e_s.kid[i] = J6E_ID_NONE;
    }
    j6_synth_kill(&j6e_synth);               /* D2: 6 ms fade, then cut (no click) */
    j6e_zero(j6e_s.scount, sizeof j6e_s.scount);
    j6e_zero(j6e_s.map, sizeof j6e_s.map);
    j6e_out_all_off();
    j6e_mo(0xB0u | ((uint32_t)(j6e_s.g.ch - 1u) & 0x0Fu), 120, 0);   /* All Sound Off (MI-11) */
}

/* ---- commands ------------------------------------------------------------------------------------- */

static void j6e_mark_global(void)
{
    j6e_s.g_dirty = 1;
    j6e_s.g_changed = j6e_s.frames;
}

static void j6e_play(uint32_t from)
{
    j6_evq_clear(&j6e_s.q);
    j6_seq_play(&j6e_seq, from, &j6e_s.q);
    j6e_route(&j6e_s.q, j6e_s.tick);
}

static void j6e_stop(void)
{
    j6_evq_clear(&j6e_s.q);
    j6_seq_stop(&j6e_seq, &j6e_s.q);
    j6e_route(&j6e_s.q, j6e_s.tick);
}

static void j6e_select(uint32_t idx)
{
    if (idx >= J6_PATTERNS) return;
    j6_evq_clear(&j6e_s.q);
    j6_seq_select(&j6e_seq, idx, &j6e_s.q);
    j6e_route(&j6e_s.q, j6e_s.tick);
}

static void j6e_set_step_phrase(j6_pattern_t *p, uint32_t ph)
{
    j6_step_set_phrase(p, j6e_s.sel, ph);
}

static void j6e_cmd(uint32_t cmd, int32_t arg)
{
    uint32_t st = j6_seq_state(&j6e_seq), was, cur = j6_seq_cur(&j6e_seq);
    j6_pattern_t *p;
    switch (cmd) {
    case J6_CMD_PLAY:                        /* SQ-20, SQ-30, SQ-35 (PLAY while recording stops both) */
        if (st == J6_SEQ_PLAYING || st == J6_SEQ_COUNTIN) j6e_stop();
        else j6e_play(0);
        break;
    case J6_CMD_PLAY_FROM:
        if (arg < 0 || arg >= (int32_t)J6_STEPS) break;
        if (st == J6_SEQ_PLAYING || st == J6_SEQ_COUNTIN) j6e_stop();
        j6e_play((uint32_t)arg);
        break;
    case J6_CMD_STOP:                        /* D2: stop; STOP again while stopped kills all sound */
        if (st == J6_SEQ_STOPPED) j6e_silence(1);
        else j6e_stop();
        break;
    case J6_CMD_REC:
        j6_evq_clear(&j6e_s.q);
        j6_seq_rec(&j6e_seq, &j6e_s.q);
        j6e_route(&j6e_s.q, j6e_s.tick);
        j6e_rec_held();
        break;
    case J6_CMD_PATTERN:
        if (arg >= 0) j6e_select((uint32_t)arg);
        break;
    case J6_CMD_STEP_SELECT:                 /* UI-4 */
        if (arg >= 0 && arg < (int32_t)J6_STEPS) {
            if (!j6e_step_edit() && j6e_s.hold && !(j6e_s.g.opts & J6_OPT_HOLD_KEEP))
                j6e_set_latch(0);            /* D1: HOLD now means tie, unless HoLd.E = KEEP */
            j6e_s.sel = (uint8_t)arg;
        } else {
            j6e_s.sel = J6_NONE;
        }
        j6e_live_update();
        break;
    case J6_CMD_PAGE:
        if (arg >= 0 && arg < (int32_t)J6_PAGES) {
            uint32_t last = j6e_pat()->last;
            p = j6e_edit_begin(&was);
            j6_pattern_page(p, (uint32_t)arg);
            j6e_edit_end(was, p->last != last);
        }
        break;
    case J6_CMD_HOLD:
        if (j6e_step_edit()) {               /* SQ-17 */
            uint8_t target = J6_NONE;
            uint32_t r;
            p = j6e_edit_begin(&was);
            r = j6_step_tie(p, j6e_s.sel, &target);
            j6e_edit_end(was, r == J6_EDIT_TIE_ADDED || r == J6_EDIT_TIE_REMOVED);
        } else {                             /* HD-1 */
            j6e_set_latch(!j6e_s.hold);
        }
        break;
    case J6_CMD_STYLE:
        if (j6e_step_edit()) {               /* SQ-15: switching the flag on copies the knob positions */
            uint8_t ph = j6_seq_pat(&j6e_seq, cur)->step[j6e_s.sel].phrase;
            p = j6_seq_edit(&j6e_seq, cur);
            j6e_set_step_phrase(p, j6_phr_on(ph) ? (uint32_t)(ph & 0x7Fu) : j6_phr_pack(1, j6e_s.kstyle, j6e_s.kvar));
        } else {                             /* PH-20 */
            p = j6_seq_edit(&j6e_seq, cur);
            p->phrase = (uint8_t)(p->phrase ^ J6_PHR_ON);
            j6e_live_update();
        }
        break;
    case J6_CMD_STEP_CLEAR:                  /* SQ-16 */
        if (j6e_step_edit()) j6_step_clear(&j6_seq_edit(&j6e_seq, cur)->step[j6e_s.sel]);
        break;
    case J6_CMD_STEP_COPY:
        if (j6e_step_edit()) j6_seq_step_copy(&j6e_seq, j6e_s.sel);
        break;
    case J6_CMD_STEP_PASTE:
        if (j6e_step_edit()) (void)j6_seq_step_paste(&j6e_seq, j6e_s.sel);
        break;
    case J6_CMD_STEP_INSERT:
        if (j6e_step_edit()) j6_pattern_insert(j6_seq_edit(&j6e_seq, cur), j6e_s.sel);
        break;
    case J6_CMD_PATTERN_DUPL:
        j6_pattern_dupl(j6_seq_edit(&j6e_seq, cur));
        break;
    case J6_CMD_PATTERN_CLEAR:               /* SQ-44 */
        j6_pattern_clear(j6_seq_edit(&j6e_seq, cur));
        j6e_load_pattern();
        break;
    case J6_CMD_PATTERN_INIT:                /* SQ-46: the sound too */
        j6_pattern_init(j6_seq_edit(&j6e_seq, cur));
        j6e_load_pattern();
        break;
    case J6_CMD_PATTERN_COPY:                /* SQ-45: saved by j6_engine_poll() */
        if (arg >= 0 && arg < (int32_t)J6_PATTERNS) {
            j6_seq_copy_pattern(&j6e_seq, cur, (uint32_t)arg);
            j6e_s.copy_save[(uint32_t)arg >> 5] |= 1u << ((uint32_t)arg & 31u);
        }
        break;
    case J6_CMD_PANIC:
        j6e_silence(1);
        break;
    default:
        break;
    }
}

/* ---- settings ------------------------------------------------------------------------------------- */

static void j6e_set_phrase_ctl(uint32_t param, int32_t v)
{
    uint32_t cur = j6_seq_cur(&j6e_seq);
    const j6_pattern_t *rp = j6_seq_pat(&j6e_seq, cur);
    if (param == J6_P_STYLE) j6e_s.kstyle = (uint8_t)v;
    if (param == J6_P_VAR) j6e_s.kvar = (uint8_t)v;
    if (j6e_step_edit()) {                   /* SQ-15: the selected step's phrase */
        uint8_t ph = rp->step[j6e_s.sel].phrase;
        uint32_t on = j6_phr_on(ph), nph;
        if (param == J6_P_STYLE_ON) {
            nph = v ? (on ? ph : j6_phr_pack(1, j6e_s.kstyle, j6e_s.kvar)) : (uint32_t)(ph & 0x7Fu);
        } else if (!on) {
            return;                          /* the knobs set the step phrase only while its flag is on */
        } else if (param == J6_P_STYLE) {
            nph = j6_phr_pack(1, (uint32_t)v, j6_phr_var(ph));
        } else {
            nph = j6_phr_pack(1, j6_phr_style(ph), (uint32_t)v);
        }
        if (nph != ph) j6e_set_step_phrase(j6_seq_edit(&j6e_seq, cur), nph);
        return;
    }
    {                                        /* PH-20: the pattern phrase */
        uint8_t ph = rp->phrase;
        uint32_t nph;
        if (param == J6_P_STYLE_ON) nph = (uint32_t)(ph & 0x7Fu) | (v ? J6_PHR_ON : 0u);
        else if (param == J6_P_STYLE) nph = j6_phr_pack(j6_phr_on(ph), (uint32_t)v, j6_phr_var(ph));
        else nph = j6_phr_pack(j6_phr_on(ph), j6_phr_style(ph), (uint32_t)v);
        if (nph != ph) j6_seq_edit(&j6e_seq, cur)->phrase = (uint8_t)nph;
        j6e_apply_live_style();
        j6e_live_update();
    }
}

static void j6e_set(uint32_t param, int32_t v)
{
    uint32_t cur = j6_seq_cur(&j6e_seq);
    const j6_pattern_t *rp = j6_seq_pat(&j6e_seq, cur);
    j6_pattern_t *p;
    if (param >= J6_P_COUNT) return;
    v = j6e_pclamp(param, v);
    if (param >= J6_P_KNOB0 && param <= J6_P_KNOB_LAST) {   /* SD-3: replaces the sound's value */
        uint32_t k = param - J6_P_KNOB0;
        if (rp->knob[k] != (uint8_t)v) j6_seq_edit(&j6e_seq, cur)->knob[k] = (uint8_t)v;
        j6_synth_set(&j6e_synth, k, v);
        return;
    }
    switch (param) {
    case J6_P_TEMPO10:
        if (rp->tempo10 != (uint16_t)v) j6_seq_edit(&j6e_seq, cur)->tempo10 = (uint16_t)v;
        j6e_apply_tempo();
        break;
    case J6_P_LAST:
        if (rp->last != (uint8_t)v) j6_pattern_set_last(j6_seq_edit(&j6e_seq, cur), (uint32_t)v);
        break;
    case J6_P_BEAT:
        if (rp->beat != (uint8_t)v) j6_seq_edit(&j6e_seq, cur)->beat = (uint8_t)v;
        break;
    case J6_P_SHUFFLE:
        if (rp->shuffle != (int8_t)v) j6_seq_edit(&j6e_seq, cur)->shuffle = (int8_t)v;
        j6_phrase_set_shuffle(&j6e_plive, v);
        j6_phrase_set_shuffle(&j6e_pseq, v);
        break;
    case J6_P_VOL:
        if (rp->vol != (uint8_t)v) j6_seq_edit(&j6e_seq, cur)->vol = (uint8_t)v;
        j6_synth_set(&j6e_synth, J6_SP_VOLUME, v);
        break;
    case J6_P_TRAN:                          /* sounding notes keep their synth note (the map) */
        if (rp->tran != (int8_t)v) j6_seq_edit(&j6e_seq, cur)->tran = (int8_t)v;
        break;
    case J6_P_DSYNC:
        if (rp->dsync != (uint8_t)v) j6_seq_edit(&j6e_seq, cur)->dsync = (uint8_t)v;
        j6_synth_set(&j6e_synth, J6_SP_DLY_SYNC, v);
        break;
    case J6_P_AVEL:
        if (rp->avel != (uint8_t)v) j6_seq_edit(&j6e_seq, cur)->avel = (uint8_t)v;
        j6e_apply_vel();
        break;
    case J6_P_PCHG:
        if (rp->pchg != (uint8_t)v) j6_seq_edit(&j6e_seq, cur)->pchg = (uint8_t)v;
        break;
    case J6_P_SCLK:
        if (rp->sclk != (uint8_t)v) j6_seq_edit(&j6e_seq, cur)->sclk = (uint8_t)v;
        break;
    case J6_P_SOUND:                         /* SD-2, SD-3: the sound's own knob values again */
        p = j6_seq_edit(&j6e_seq, cur);
        p->sound = (uint8_t)v;
        {
            uint32_t k;
            for (k = 0; k < J6_KNOBS; k++) p->knob[k] = J6_KNOB_UNSET;
        }
        j6_synth_sound(&j6e_synth, (uint32_t)v);
        break;
    case J6_P_STYLE_ON:
    case J6_P_STYLE:
    case J6_P_VAR:
        j6e_set_phrase_ctl(param, v);
        break;
    case J6_P_CH:                            /* sounding notes end on the channel they began on */
        if ((uint8_t)v != j6e_s.g.ch) j6e_out_all_off();
        j6e_s.g.ch = (uint8_t)v;
        j6e_mark_global();
        break;
    case J6_P_SYNC:
        j6e_s.g.sync = (uint8_t)v;
        if (v == J6_SYNC_INT && j6e_s.ext) {
            j6e_s.ext = 0;
            j6_clock_ext(&j6e_clock, 0);
            j6e_apply_tempo();
        }
        j6e_mark_global();
        break;
    case J6_P_THRU:
        j6e_s.g.thru = (uint8_t)v;
        j6e_mark_global();
        break;
    case J6_P_TXPC:
        j6e_s.g.txpc = (uint8_t)v;
        j6e_mark_global();
        break;
    case J6_P_RXPC:
        j6e_s.g.rxpc = (uint8_t)v;
        j6e_mark_global();
        break;
    case J6_P_PCCH:
        j6e_s.g.pcch = (uint8_t)v;
        j6e_mark_global();
        break;
    case J6_P_VELO:
        j6e_s.g.velo = (uint8_t)v;
        j6e_apply_vel();
        j6e_mark_global();
        break;
    case J6_P_TUNE10:
        j6e_s.g.tune10 = (uint16_t)v;
        j6_synth_set(&j6e_synth, J6_SP_TUNE10, v);
        j6e_mark_global();
        break;
    case J6_P_USBD:
        j6e_s.g.usbd = (uint8_t)v;
        j6e_mark_global();
        break;
    case J6_P_ALNK:
        j6e_s.g.alnk = (uint8_t)v;
        j6e_mark_global();
        break;
    case J6_P_CNTI:
        if (v == 1) v = 2;
        j6e_s.g.cnti = (uint8_t)v;
        j6_seq_set_countin(&j6e_seq, (uint32_t)v);
        j6e_mark_global();
        break;
    case J6_P_CHORD_SET:
        j6e_s.g.chord_set = (uint8_t)v;
        j6e_mark_global();
        break;
    case J6_P_KEY:
        j6e_s.g.key = (int8_t)v;
        j6e_mark_global();
        break;
    case J6_P_HOLDE:
        j6e_s.g.opts = (uint8_t)(v ? (j6e_s.g.opts | J6_OPT_HOLD_KEEP) : (j6e_s.g.opts & ~J6_OPT_HOLD_KEEP));
        j6e_mark_global();
        break;
    case J6_P_CHDL:
        j6e_s.g.opts = (uint8_t)(v ? (j6e_s.g.opts & ~J6_OPT_CHORD_RETRIG) : (j6e_s.g.opts | J6_OPT_CHORD_RETRIG));
        j6e_mark_global();
        break;
    case J6_P_OCT:
        j6e_s.oct = (int8_t)v;
        break;
    case J6_P_HOLD:
        if (!j6e_step_edit()) j6e_set_latch((uint32_t)v);
        break;
    case J6_P_CHORD:
        if ((uint32_t)v != j6e_s.chord) {      /* the keys tracker changes mode (clears it) */
            uint32_t i;
            j6e_s.chord = (uint8_t)v;
            j6_held_mono(&j6e_hkeys, (uint32_t)v);
            j6_held_latch(&j6e_hkeys, j6e_s.hold);
            for (i = 0; i < J6_KEY_COUNT; i++) j6e_s.kid[i] = J6E_ID_NONE;
            j6e_rec_held();
            j6e_live_update();
        }
        break;
    default:
        break;
    }
}

int32_t j6_engine_get(uint32_t param)
{
    const j6_pattern_t *p = j6e_pat();
    if (param >= J6_P_KNOB0 && param <= J6_P_KNOB_LAST) {
        uint32_t k = param - J6_P_KNOB0;
        return p->knob[k] != J6_KNOB_UNSET ? (int32_t)p->knob[k] : j6_synth_get(&j6e_synth, k);
    }
    switch (param) {
    case J6_P_TEMPO10: return p->tempo10;
    case J6_P_LAST: return p->last;
    case J6_P_BEAT: return p->beat;
    case J6_P_SHUFFLE: return p->shuffle;
    case J6_P_VOL: return p->vol;
    case J6_P_TRAN: return p->tran;
    case J6_P_DSYNC: return p->dsync;
    case J6_P_AVEL: return p->avel;
    case J6_P_PCHG: return p->pchg;
    case J6_P_SCLK: return p->sclk;
    case J6_P_SOUND: return p->sound;
    case J6_P_STYLE_ON:
        return (int32_t)j6_phr_on(j6e_step_edit() ? p->step[j6e_s.sel].phrase : p->phrase);
    case J6_P_STYLE:
    case J6_P_VAR: {
        uint8_t ph = p->phrase;
        if (j6e_step_edit() && j6_phr_on(p->step[j6e_s.sel].phrase)) ph = p->step[j6e_s.sel].phrase;
        return (int32_t)(param == J6_P_STYLE ? j6_phr_style(ph) : j6_phr_var(ph));
    }
    case J6_P_CH: return j6e_s.g.ch;
    case J6_P_SYNC: return j6e_s.g.sync;
    case J6_P_THRU: return j6e_s.g.thru;
    case J6_P_TXPC: return j6e_s.g.txpc;
    case J6_P_RXPC: return j6e_s.g.rxpc;
    case J6_P_PCCH: return j6e_s.g.pcch;
    case J6_P_VELO: return j6e_s.g.velo;
    case J6_P_TUNE10: return j6e_s.g.tune10;
    case J6_P_USBD: return j6e_s.g.usbd;
    case J6_P_ALNK: return j6e_s.g.alnk;
    case J6_P_CNTI: return j6e_s.g.cnti;
    case J6_P_CHORD_SET: return j6e_s.g.chord_set;
    case J6_P_KEY: return j6e_s.g.key;
    case J6_P_HOLDE: return (j6e_s.g.opts & J6_OPT_HOLD_KEEP) ? 1 : 0;
    case J6_P_CHDL: return (j6e_s.g.opts & J6_OPT_CHORD_RETRIG) ? 0 : 1;
    case J6_P_OCT: return j6e_s.oct;
    case J6_P_HOLD: return j6e_s.hold;
    case J6_P_CHORD: return j6e_s.chord;
    default: return 0;
    }
}

/* ---- MIDI in (MI-1 .. MI-8) ----------------------------------------------------------------------- */

static uint32_t j6e_sync_port(uint32_t port)
{
    switch (j6e_s.g.sync) {
    case J6_SYNC_AUTO: return 1;
    case J6_SYNC_MIDI: return port == J6_PORT_TRS;
    case J6_SYNC_USB: return port == J6_PORT_USB;
    default: return 0;                       /* InT */
    }
}

static void j6e_midi_note(uint32_t note, uint32_t vel)
{
    uint32_t was;
    if (note > 127u) return;
    if (vel) {
        j6e_s.mvel[note] = (uint8_t)vel;
        j6e_s.last_note = (uint8_t)note;
        if (j6e_step_edit()) {
            j6_pattern_t *p = j6e_edit_begin(&was);
            uint32_t r = j6_step_key_note(p, j6e_s.sel, note);
            j6e_edit_end(was, r != J6_EDIT_NONE && r != J6_EDIT_FULL);
        } else if (j6_seq_recording(&j6e_seq)) {
            uint8_t n8 = (uint8_t)note;
            j6_seq_rec_input(&j6e_seq, &n8, 1, J6_NO_CHORD, j6e_rec_phrase());
        }
        j6_held_press(&j6e_hmidi, note);
    } else {
        j6_held_release(&j6e_hmidi, note);
    }
    j6e_rec_held();
    j6e_live_update();
}

static void j6e_midi(uint32_t port, uint32_t st, uint32_t d1, uint32_t d2)
{
    uint32_t type = st & 0xF0u, chn = (st & 0x0Fu) + 1u;
    j6e_s.as_last = j6e_s.frames;
    if (st >= 0xF8u) {
        switch (st) {
        case 0xF8u:                          /* MI-8 */
            if (!j6e_sync_port(port)) break;
            if (!j6e_s.ext) {
                j6e_s.ext = 1;
                j6_clock_ext(&j6e_clock, 1);
            }
            j6_clock_ext_pulse(&j6e_clock);
            break;
        case 0xFAu:
            if (!j6e_sync_port(port)) break;
            if (j6_seq_state(&j6e_seq) == J6_SEQ_PLAYING || j6_seq_state(&j6e_seq) == J6_SEQ_COUNTIN) j6e_stop();
            /* MI-8: step 1 is the first 0xF8 after the Start, not the ticks the last one still owes */
            if (j6e_s.ext) j6_clock_ext_resync(&j6e_clock);
            j6e_play(0);
            break;
        case 0xFCu:
            if (j6e_sync_port(port)) j6e_stop();
            break;
        case 0xFEu:                          /* MI-5 */
            j6e_s.as_armed = 1;
            break;
        default:                             /* Continue and the rest: ignored */
            break;
        }
        return;
    }
    if (st < 0x80u || st >= 0xF0u) return;
    d1 &= 0x7Fu;
    d2 &= 0x7Fu;
    if (type == 0xC0u && chn == j6e_s.g.pcch) {    /* MI-6 */
        if (j6e_s.g.rxpc && d1 < J6_PATTERNS) j6e_select(d1);
        return;
    }
    if (chn != j6e_s.g.ch) return;             /* MI-1 */
    if (type == 0x90u) {
        j6e_midi_note(d1, d2);
    } else if (type == 0x80u) {
        j6e_midi_note(d1, 0);
    } else if (type == 0xB0u) {              /* MI-4 */
        if (d1 == 64u) {
            if (!j6e_step_edit()) j6e_set_latch(d2 >= 64u);
        } else if (d1 == 120u) {
            j6e_silence(0);
        } else if (d1 == 121u) {
            j6e_set_latch(0);
        } else if (d1 == 123u) {
            j6_held_clear(&j6e_hmidi);
            j6e_rec_held();
            j6e_live_update();
        }
    }
}

/* ---- the command ring ----------------------------------------------------------------------------- */

/* a command that ends something: it may use the reserved slots */
static uint32_t j6e_is_release(uint32_t type, uint32_t a, uint32_t b, uint32_t c)
{
    if (type == J6E_C_KEY) return a == 0u;
    if (type != J6E_C_MIDI) return 0;
    if (a == 0xFCu) return 1;
    if ((a & 0xF0u) == 0x80u) return 1;
    if ((a & 0xF0u) == 0x90u) return (c & 0x7Fu) == 0u;
    if ((a & 0xF0u) == 0xB0u) return b == 64u || b == 120u || b == 121u || b == 123u;
    return 0;
}

/* 1 = queued, 0 = the ring is full (counted in cmd_lost) */
static uint32_t j6e_put(uint32_t type, uint32_t a, uint32_t b, uint32_t c, int32_t v)
{
    uint32_t w = j6e_s.cmd_w, used = w - j6e_s.cmd_r;
    j6e_cmd_t *e;
    if (used >= J6E_CMD_RING || (used >= J6E_CMD_RING - J6E_CMD_RESERVE && !j6e_is_release(type, a, b, c))) {
        j6e_s.cmd_lost++;
        return 0;
    }
    e = &j6e_s.cmd[w & (J6E_CMD_RING - 1u)];
    e->type = (uint8_t)type;
    e->a = (uint8_t)a;
    e->b = (uint8_t)b;
    e->c = (uint8_t)c;
    e->v = v;
    J6E_PUBLISH();
    j6e_s.cmd_w = w + 1u;
    return 1;
}

uint32_t j6_engine_key(int32_t key, uint32_t down)
{
    return j6e_put(J6E_C_KEY, down ? 1u : 0u, 0, 0, key);
}

uint32_t j6_engine_midi_in(uint32_t port, uint32_t st, uint32_t d1, uint32_t d2)
{
    return j6e_put(J6E_C_MIDI, st, d1, d2, (int32_t)port);
}

uint32_t j6_engine_cmd_space(void)
{
    uint32_t used = j6e_s.cmd_w - j6e_s.cmd_r;
    return used >= J6E_CMD_RING - J6E_CMD_RESERVE ? 0u : J6E_CMD_RING - J6E_CMD_RESERVE - used;
}

void j6_engine_cmd(uint32_t cmd, int32_t arg)
{
    (void)j6e_put(J6E_C_CMD, cmd, 0, 0, arg);
}

void j6_engine_set(uint32_t param, int32_t v)
{
    (void)j6e_put(J6E_C_SET, param, 0, 0, v);
}

static void j6e_apply_commands(void)
{
    uint32_t r = j6e_s.cmd_r;
    while (r != j6e_s.cmd_w) {
        j6e_cmd_t c = j6e_s.cmd[r & (J6E_CMD_RING - 1u)];
        switch (c.type) {
        case J6E_C_KEY: j6e_key(c.v, c.a); break;
        case J6E_C_MIDI: j6e_midi((uint32_t)c.v, c.a, c.b, c.c); break;
        case J6E_C_CMD: j6e_cmd(c.a, c.v); break;
        case J6E_C_SET: j6e_set(c.a, c.v); break;
        default: break;
        }
        r++;
        J6E_PUBLISH();
        j6e_s.cmd_r = r;
    }
}

/* ---- time (design 2.3) ---------------------------------------------------------------------------- */

static void j6e_tick(void)
{
    uint32_t playing, pt, tick;
    j6e_pend_fire(J6_SUB);                   /* what is left of the previous tick */
    tick = ++j6e_s.tick;
    j6_evq_clear(&j6e_s.q);
    j6_seq_tick(&j6e_seq, &j6e_s.q);
    j6e_route(&j6e_s.q, tick);
    playing = j6e_playing();
    if (playing) j6e_s.seq_pos = j6_seq_pos(&j6e_seq);
    pt = playing ? j6e_s.seq_pos : j6e_s.live_pt;
    j6_evq_clear(&j6e_s.q);
    j6_phrase_tick(&j6e_pseq, pt, &j6e_s.q);
    j6e_route(&j6e_s.q, tick);
    j6_evq_clear(&j6e_s.q);
    j6_phrase_tick(&j6e_plive, pt, &j6e_s.q);
    j6e_route(&j6e_s.q, tick);
    j6e_s.live_pt = pt + 1u;
    if (!j6e_s.ext && (J6_CFG_CLOCK_OUT_STOPPED || playing) &&
        ((playing ? j6e_s.seq_pos : tick) % J6_MIDI_CLOCK_TICKS) == 0u)
        j6e_mo(0xF8u, 0, 0);                 /* MI-7 */
    if (j6e_s.ext) {                     /* d.Syn follows a measured tempo */
        uint32_t t = j6_clock_tempo10(&j6e_clock);
        if (t != j6e_s.synth_tempo) {
            j6e_s.synth_tempo = (uint16_t)t;
            j6_synth_set(&j6e_synth, J6_SP_TEMPO10, (int32_t)t);
        }
    }
}

static void j6e_housekeeping(void)
{
    if (j6e_s.ext && !j6_clock_ext_alive(&j6e_clock)) {   /* MI-8: back to the internal clock */
        j6e_s.ext = 0;
        j6_clock_ext(&j6e_clock, 0);
        j6e_apply_tempo();
    }
    if (j6e_s.as_armed && j6e_s.frames - j6e_s.as_last > J6E_AS_IN_FRAMES) {   /* MI-5 */
        j6e_s.as_armed = 0;
        j6_held_clear(&j6e_hmidi);
        j6e_rec_held();
        j6e_live_update();
    }
    if ((int32_t)(j6e_s.frames - j6e_s.as_next) >= 0) {
        j6e_s.as_next = j6e_s.frames + J6E_AS_OUT_FRAMES;
        j6e_mo(0xFEu, 0, 0);
    }
}

static void j6e_run(int32_t *out, uint32_t n)
{
    uint32_t c, k;
    if (!j6e_s.inited || j6e_s.busy) return;
    j6e_apply_commands();
    j6e_housekeeping();
    n -= n % J6_CR;
    for (c = 0; c < n; c += J6_CR) {
        k = j6_clock_advance(&j6e_clock, J6_CR);
        while (k--) j6e_tick();
        if (j6e_s.npend) j6e_pend_fire(j6_clock_sub(&j6e_clock));
        if (out) j6_synth_render(&j6e_synth, out + 2u * c, J6_CR);
        j6e_s.frames += J6_CR;
    }
}

void j6_engine_render(int32_t *out, uint32_t n)
{
    j6e_run(out, n);
}

void j6_engine_advance(uint32_t n)
{
    j6e_run(NULL, n);
}

/* ---- storage (design 8.4) ------------------------------------------------------------------------- */

static void j6e_global_defaults(j6_global_t *g)
{
    j6e_zero(g, sizeof *g);
    g->ch = 1;
    g->sync = J6_SYNC_AUTO;
    g->txpc = 1;
    g->rxpc = 1;
    g->pcch = 16;
    g->velo = J6_VELO_DEF;
    g->tune10 = J6_TUNE10_DEF;
}

static void j6e_global_encode(const j6_global_t *g, uint8_t *b)
{
    j6e_zero(b, J6_GLOBAL_BYTES);
    b[0] = J6_GLOBAL_FORMAT;
    b[1] = g->ch;
    b[2] = g->sync;
    b[3] = g->thru;
    b[4] = g->txpc;
    b[5] = g->rxpc;
    b[6] = g->pcch;
    b[7] = g->velo;
    b[8] = g->usbd;
    b[9] = (uint8_t)(g->tune10 & 0xFFu);
    b[10] = (uint8_t)(g->tune10 >> 8);
    b[11] = g->alnk;
    b[12] = g->cnti;
    b[13] = g->chord_set;
    b[14] = (uint8_t)g->key;
    b[15] = g->pattern;
    b[16] = g->opts;
}

static int32_t j6e_global_decode(j6_global_t *g, const uint8_t *b)
{
    j6e_global_defaults(g);
    if (b[0] != J6_GLOBAL_FORMAT) return -1;
    g->ch = (uint8_t)j6e_pclamp(J6_P_CH, b[1]);
    g->sync = (uint8_t)j6e_pclamp(J6_P_SYNC, b[2]);
    g->thru = b[3] ? 1u : 0u;
    g->txpc = b[4] ? 1u : 0u;
    g->rxpc = b[5] ? 1u : 0u;
    g->pcch = (uint8_t)j6e_pclamp(J6_P_PCCH, b[6]);
    g->velo = (uint8_t)j6e_pclamp(J6_P_VELO, b[7]);
    g->usbd = (uint8_t)j6e_pclamp(J6_P_USBD, b[8]);
    g->tune10 = (uint16_t)j6e_pclamp(J6_P_TUNE10, (int32_t)(b[9] | (uint32_t)b[10] << 8));
    g->alnk = b[11] ? 1u : 0u;
    g->cnti = (uint8_t)j6e_pclamp(J6_P_CNTI, b[12]);
    if (g->cnti == 1u) g->cnti = 2;
    g->chord_set = (uint8_t)j6e_pclamp(J6_P_CHORD_SET, b[13]);
    g->key = (int8_t)j6e_pclamp(J6_P_KEY, (int8_t)b[14]);
    g->pattern = (uint8_t)(b[15] < J6_PATTERNS ? b[15] : 0u);
    g->opts = (uint8_t)(b[16] & (J6_OPT_HOLD_KEEP | J6_OPT_CHORD_RETRIG));   /* older data: 0 = defaults */
    return 0;
}

static int j6e_store_write(uint32_t slot, const uint8_t *buf, uint32_t len)
{
    if (!j6e_s.have_store || !j6e_s.store.write) return -1;
    return j6e_s.store.write(j6e_s.store.ctx, slot, buf, len);
}

static int32_t j6e_save_pattern(uint32_t idx)
{
    uint8_t buf[J6_PAT_BYTES];
    j6_pattern_encode(j6_seq_pat(&j6e_seq, idx), buf);
    if (j6e_store_write(idx, buf, J6_PAT_BYTES) != 0) return -1;
    j6_seq_set_saved(&j6e_seq, idx);
    return 0;
}

static int32_t j6e_save_globals(void)
{
    uint8_t buf[J6_GLOBAL_BYTES];
    j6e_global_encode(&j6e_s.g, buf);
    if (j6e_store_write(J6_SLOT_GLOBAL, buf, J6_GLOBAL_BYTES) != 0) return -1;
    j6e_s.g_dirty = 0;
    return 0;
}

/* everything to its power-on state; load = read the store (else defaults) */
static void j6e_setup(int16_t *fx_mem, uint32_t fx_len, uint32_t load)
{
    uint32_t i;
    uint8_t buf[J6_PAT_BYTES];
    j6_tap_fn tap = j6e_s.tap;
    void *tap_ctx = j6e_s.tap_ctx;
    j6_store_t store = j6e_s.store;
    uint32_t have_store = j6e_s.have_store;
    j6e_zero(&j6e_s, sizeof j6e_s);
    j6e_s.busy = 1;
    j6e_s.tap = tap;
    j6e_s.tap_ctx = tap_ctx;
    j6e_s.store = store;
    j6e_s.have_store = have_store;
    j6e_s.fx = fx_mem;
    j6e_s.fx_len = fx_len;
    j6e_s.sel = J6_NONE;
    j6e_s.last_note = J6_NO_NOTE;
    j6e_s.last_chord = J6_NO_CHORD;
    for (i = 0; i < J6_KEY_COUNT; i++) j6e_s.kid[i] = J6E_ID_NONE;

    j6_seq_init(&j6e_seq);
    j6_clock_init(&j6e_clock, J6_TEMPO10_DEF);
    j6_phrase_init(&j6e_plive, J6_SRC_PHRASE_LIVE);
    j6_phrase_init(&j6e_pseq, J6_SRC_PHRASE_SEQ);
    j6_phrase_set_bank(&j6e_plive, NULL);
    j6_phrase_set_bank(&j6e_pseq, NULL);
    j6_held_init(&j6e_hkeys);
    j6_held_init(&j6e_hmidi);
    j6_synth_init(&j6e_synth, fx_mem, fx_len);
    j6e_s.live_style = 1;
    j6e_s.live_var = 1;
    j6_phrase_set_style(&j6e_plive, 1, 1);

    j6e_global_defaults(&j6e_s.g);
    if (load && j6e_s.have_store && j6e_s.store.read) {
        if (j6e_s.store.read(j6e_s.store.ctx, J6_SLOT_GLOBAL, buf, J6_GLOBAL_BYTES) != 0 ||
            j6e_global_decode(&j6e_s.g, buf) != 0)
            j6e_global_defaults(&j6e_s.g);
        for (i = 0; i < J6_PATTERNS; i++) {
            j6_pattern_t *p = j6_seq_edit(&j6e_seq, i);
            if (j6e_s.store.read(j6e_s.store.ctx, i, buf, J6_PAT_BYTES) != 0 || j6_pattern_decode(p, buf) != 0)
                j6_pattern_init(p);
            j6_seq_set_saved(&j6e_seq, i);
        }
    }
    j6_seq_set_countin(&j6e_seq, j6e_s.g.cnti);
    j6_synth_set(&j6e_synth, J6_SP_TUNE10, j6e_s.g.tune10);
    j6e_s.inited = 1;
    j6e_s.booting = 1;
    j6e_select(j6e_s.g.pattern);               /* stopped: PATTERN now, which loads the pattern */
    j6e_s.booting = 0;
    j6e_s.g_dirty = 0;
    j6e_s.busy = 0;
    J6E_PUBLISH();
}

void j6_engine_init(int16_t *fx_mem, uint32_t fx_len, const j6_store_t *store)
{
    if (store) {
        j6e_s.store = *store;
        j6e_s.have_store = 1;
    } else {
        j6e_zero(&j6e_s.store, sizeof j6e_s.store);
        j6e_s.have_store = 0;
    }
    j6e_setup(fx_mem, fx_len, 1);
}

int32_t j6_engine_write(uint32_t all)
{
    uint32_t i, cur;
    int32_t rc = 0;
    if (!j6e_s.inited || !j6e_s.have_store || !j6e_s.store.write) return -1;
    j6e_s.busy = 1;                            /* mute: no render until the flash is written */
    J6E_PUBLISH();
    cur = j6_seq_cur(&j6e_seq);
    for (i = 0; i < J6_PATTERNS; i++) {
        if (all ? !j6_seq_dirty(&j6e_seq, i) : i != cur) continue;
        if (j6e_save_pattern(i) != 0) rc = -1;
    }
    if (j6e_s.g_dirty && j6e_save_globals() != 0) rc = -1;   /* PS-3: already muted, so the globals too */
    j6e_s.busy = 0;
    J6E_PUBLISH();
    return rc;
}

int32_t j6_engine_save_globals(void)
{
    int32_t rc;
    if (!j6e_s.inited || !j6e_s.have_store || !j6e_s.store.write) return -1;
    if (!j6e_s.g_dirty) return 0;
    j6e_s.busy = 1;                            /* PS-3: menu / screen left: a short mute, sounding or not */
    J6E_PUBLISH();
    rc = j6e_save_globals();
    j6e_s.busy = 0;
    J6E_PUBLISH();
    return rc;
}

void j6_engine_poll(void)
{
    uint32_t i;
    if (!j6e_s.inited) return;
    if (j6e_s.copy_save[0] | j6e_s.copy_save[1]) {   /* SQ-45 */
        j6e_s.busy = 1;
        J6E_PUBLISH();
        for (i = 0; i < J6_PATTERNS; i++)
            if (j6e_s.copy_save[i >> 5] & (1u << (i & 31u))) {
                j6e_s.copy_save[i >> 5] &= ~(1u << (i & 31u));
                (void)j6e_save_pattern(i);
            }
        j6e_s.busy = 0;
        J6E_PUBLISH();
    }
    if (j6e_s.g_dirty && j6e_s.frames - j6e_s.g_changed >= J6E_GSAVE_FRAMES && !j6_synth_active(&j6e_synth) &&
        j6_synth_fx_idle(&j6e_synth)) {       /* PS-3 */
        j6e_s.busy = 1;
        J6E_PUBLISH();
        if (j6e_save_globals() != 0) j6e_s.g_dirty = 0;   /* no store: nothing to retry */
        j6e_s.busy = 0;
        J6E_PUBLISH();
    }
}

void j6_engine_factory_reset(void)
{
    uint32_t i;
    if (!j6e_s.inited) return;
    j6e_s.busy = 1;
    J6E_PUBLISH();
    j6e_setup(j6e_s.fx, j6e_s.fx_len, 0);
    j6e_s.busy = 1;
    for (i = 0; i < J6_PATTERNS; i++) (void)j6e_save_pattern(i);
    (void)j6e_save_globals();
    j6e_s.busy = 0;
    J6E_PUBLISH();
}

/* ---- status --------------------------------------------------------------------------------------- */

static uint32_t j6e_key_bit(int32_t key)
{
    return key >= J6_KEY_MIN && key <= J6_KEY_MAX ? 1u << (uint32_t)(key - J6_KEY_MIN) : 0u;
}

void j6_engine_status(j6_status_t *st)
{
    const j6_pattern_t *p = j6e_pat();
    uint32_t i;
    if (!st) return;
    j6e_zero(st, sizeof *st);
    st->seq_state = (uint8_t)j6_seq_state(&j6e_seq);
    st->recording = (uint8_t)j6_seq_recording(&j6e_seq);
    st->pattern = (uint8_t)j6_seq_cur(&j6e_seq);
    st->queued = (uint8_t)j6_seq_queued(&j6e_seq);
    st->step_now = (uint8_t)j6_seq_step_now(&j6e_seq);
    st->sel_step = j6e_s.sel;
    st->countin_left = (uint8_t)j6_seq_countin_left(&j6e_seq);
    st->voices = (uint8_t)j6_synth_active(&j6e_synth);
    st->fx_idle = (uint8_t)j6_synth_fx_idle(&j6e_synth);
    st->chord = j6e_s.chord;
    st->style_on = (uint8_t)j6_engine_get(J6_P_STYLE_ON);
    st->hold = j6e_step_edit() ? (uint8_t)(p->step[j6e_s.sel].flags & J6_STF_TIE) : j6e_s.hold;
    st->ext_clock = j6e_s.ext;
    st->last_note = j6e_s.last_note;
    st->last_chord = j6e_s.last_chord;
    st->tempo10 = (uint16_t)j6_clock_tempo10(&j6e_clock);
    for (i = 0; i < J6_PATTERNS; i++)
        if (j6_seq_dirty(&j6e_seq, i)) st->dirty[i >> 5] |= 1u << (i & 31u);
    if (j6e_step_edit()) {                   /* SQ-14: the step's notes (window 60 + KEY + 12 oct), the chord's key */
        const j6_step_t *s = &p->step[j6e_s.sel];
        uint32_t cm = (uint32_t)(s->flags & J6_STF_CMASK) >> J6_STF_CMASK_SHIFT;
        if (cm && s->chord != J6_NO_CHORD) st->keys_lit |= j6e_key_bit(j6_chord_tag_key(s->chord, j6e_s.oct));
        for (i = 0; i < J6_STEP_NOTES; i++) {
            int32_t k;
            if (s->note[i] > 127u || (cm & (1u << i))) continue;
            /* the key that plays the note (j6_key_note: KEY included) is the key that deletes it (SQ-12) */
            k = (int32_t)s->note[i] - J6_KEY_BASE_NOTE - 12 * j6e_s.oct - j6e_s.g.key;
            if (j6e_key_bit(k)) {
                st->keys_lit |= j6e_key_bit(k);
            } else {                         /* outside the keys: the same pitch class at the edge, dim */
                while (k > J6_KEY_MAX) k -= 12;
                while (k < J6_KEY_MIN) k += 12;
                st->keys_dim |= j6e_key_bit(k);
            }
        }
    } else {
        for (i = 0; i < J6_KEY_COUNT; i++)
            if (j6e_s.kid[i] != J6E_ID_NONE) st->keys_lit |= 1u << i;
    }
    st->ticks = j6e_s.tick;
}

const j6_pattern_t *j6_engine_pattern(uint32_t idx)
{
    return j6_seq_pat(&j6e_seq, idx);
}

const j6_synth_t *j6_engine_synth(void)
{
    return &j6e_synth;
}

void j6_engine_set_tap(j6_tap_fn fn, void *ctx)
{
    j6e_s.tap = fn;
    j6e_s.tap_ctx = ctx;
}
