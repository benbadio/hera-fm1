/* SPDX-License-Identifier: GPL-3.0-only */
/* hera_ui.c - the Hera screen: pages, layout and drawing (included by hera_app.c after platform.c and the
 * engine). The screen is 240 x 240 RGB565; nothing keeps a framebuffer, so every region is drawn into the
 * canvas strip (gfx.c cv_*) and sent only when what it shows has changed (a key of its contents).
 *
 *   top bar    0..19    pattern, page title, tempo, transport
 *   body      20..199   the page (drawn in two 90-row halves)
 *   bottom   200..239   the four knob cells (K1..K4), or a short pop-up after a change
 *
 * Text: FONT_S (Terminus 16 px, 8 px a character, 30 to a line), and twice that size for the hero line. */
#include "hera_tables.h"

/* ---- colours ------------------------------------------------------------------------------------- */
#define H_BG RGB(8, 8, 12)
#define H_PANEL RGB(24, 24, 32)
#define H_PANEL2 RGB(36, 36, 46)
#define H_LINE RGB(56, 56, 70)
#define H_DIM RGB(120, 120, 136)
#define H_TXT RGB(236, 236, 242)
/* the colours: the J-6's blue leads; the rest are the Juno-60 panel's muted section and button colours */
#define H_ACC RGB(0, 160, 232)           /* J-6 blue: selection, the current thing */
#define H_ACCD RGB(52, 76, 132)          /* Juno DCO/chorus blue: pop-ups */
#define H_OK RGB(216, 192, 92)           /* Juno yellow button: playing, playhead, queued, external clock */
#define H_JORG RGB(216, 128, 16)         /* Juno orange button (ARPEGGIO): phrases */
#define H_JRED RGB(196, 62, 48)          /* Juno VCF/ENV red: filter and envelope, unsaved */
#define H_CREAM RGB(216, 208, 168)       /* Juno cream button (KEY HOLD), the keys */
#define H_REC H_JRED
#define H_BLACKKEY RGB(18, 18, 22)
#define H_WHITEKEY RGB(52, 52, 62)

enum { PG_PLAY = 0, PG_CHORDS, PG_PHRASE, PG_SEQ, PG_PATTERNS, PG_SOUNDS, PG_FX, PG_SETTINGS, PG_COUNT };
static const char *const PAGE_TITLE[PG_COUNT] = {"Play", "Chord map", "Phrase", "Sequencer", "Patterns",
                                                 "Sounds", "Effects", "Settings"};

typedef struct {
    uint8_t page;
    uint8_t glo;                         /* GLO (SHIFT) held */
    uint8_t seq_page;                    /* sequencer page 0..3 (16 steps each) */
    uint8_t set_cursor;                  /* settings list cursor */
    uint8_t set_top;                     /* first visible settings row */
    uint8_t force;                       /* redraw everything */
    uint8_t chord_key;                   /* last chord key 0..11 (for the map highlight), 0xFF none */
    uint32_t popup_until;                /* ms */
    char popup[32];
    uint32_t key_top, key_body, key_bot, key_scope;
} hera_ui_t;
static hera_ui_t ui;

/* the scope: app_audio writes one point every 4 frames (2 ms of sound across the 240 px strip at
 * 44.1 kHz ~ 22 ms) */
#define SCOPE_N 240u
static volatile int16_t scope[SCOPE_N];
static volatile uint32_t scope_w;

/* ---- small text helpers (no libc printf on the chip) --------------------------------------------- */
static char *h_num(char *b, int32_t v)
{
    char t[12];
    uint32_t n = 0, u = v < 0 ? (uint32_t)(-v) : (uint32_t)v;
    char *p = b;
    if (v < 0) *p++ = '-';
    do {
        t[n++] = (char)('0' + u % 10u);
        u /= 10u;
    } while (u && n < sizeof t);
    while (n) *p++ = t[--n];
    *p = 0;
    return p;
}
static char *h_signed(char *b, int32_t v)
{
    if (v > 0) *b++ = '+';
    return h_num(b, v);
}
static char *h_fix1(char *b, int32_t v10)           /* 1205 -> "120.5" */
{
    char *p = h_num(b, v10 / 10);
    *p++ = '.';
    *p++ = (char)('0' + (v10 < 0 ? -v10 : v10) % 10);
    *p = 0;
    return p;
}
static char *h_cat(char *b, const char *s)
{
    while (*s) *b++ = *s++;
    *b = 0;
    return b;
}
static const char *const NOTE_NM[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static char *h_note(char *b, uint32_t n)            /* 61 -> "C#4" */
{
    if (n > 127u) return h_cat(b, "-");
    b = h_cat(b, NOTE_NM[n % 12u]);
    return h_num(b, (int32_t)(n / 12u) - 1);
}
static char *h_pat(char *b, uint32_t p)             /* 0..63 -> "A1".."H8" */
{
    *b++ = (char)('A' + p / 8u);
    *b++ = (char)('1' + p % 8u);
    *b = 0;
    return b;
}
static uint32_t h_mix(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }
static uint32_t h_mixs(uint32_t h, const char *s)
{
    while (*s) h = h_mix(h, (uint8_t)*s++);
    return h;
}

/* ---- drawing on the canvas ----------------------------------------------------------------------- */
/* text blended between bg and fg (gfx.c's cv_text blends onto black only) */
static int32_t h_text_s(int32_t x, int32_t y, const char *s, uint16_t fg, uint16_t bg, uint32_t scale)
{
    const felucca_font_t *f = &FONT_S;
    uint16_t ramp[16];
    uint32_t a;
    int32_t fr = fg >> 11, fg6 = (fg >> 5) & 63, fb = fg & 31, br = bg >> 11, bg6 = (bg >> 5) & 63, bb = bg & 31;
    for (a = 0; a < 16u; a++)
        ramp[a] = (uint16_t)(((br + (fr - br) * (int32_t)a / 15) << 11) | ((bg6 + (fg6 - bg6) * (int32_t)a / 15) << 5) |
                             (bb + (fb - bb) * (int32_t)a / 15));
    for (; *s; s++) {
        uint32_t gi = glyph(f, (uint8_t)*s), gx, gy, w = f->bw[gi], bpr = (w + 1u) / 2u;
        const uint8_t *gd = f->data + f->off[gi];
        for (gy = 0; gy < f->h; gy++)
            for (gx = 0; gx < w; gx++) {
                uint32_t v = gd[gy * bpr + gx / 2u];
                v = (gx & 1u) ? (v & 15u) : (v >> 4);
                if (v) {
                    int32_t px = x + ((int32_t)gx - f->pad) * (int32_t)scale, py = y + (int32_t)gy * (int32_t)scale;
                    if (scale == 1u) cv_pset(px, py, ramp[v]);
                    else cv_rect(px, py, (int32_t)scale, (int32_t)scale, ramp[v]);
                }
            }
        x += f->adv[gi] * (int32_t)scale;
    }
    return x;
}
static int32_t h_text(int32_t x, int32_t y, const char *s, uint16_t fg, uint16_t bg)
{
    return h_text_s(x, y, s, fg, bg, 1u);
}
static int32_t h_tw(const char *s, uint32_t scale) { return text_w(&FONT_S, s) * (int32_t)scale; }
static void h_text_c(int32_t cx, int32_t y, const char *s, uint16_t fg, uint16_t bg, uint32_t scale)
{
    h_text_s(cx - h_tw(s, scale) / 2, y, s, fg, bg, scale);
}
static void h_text_r(int32_t rx, int32_t y, const char *s, uint16_t fg, uint16_t bg)
{
    h_text(rx - h_tw(s, 1u), y, s, fg, bg);
}
static void h_frame(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    cv_rect(x, y, w, 1, c);
    cv_rect(x, y + h - 1, w, 1, c);
    cv_rect(x, y, 1, h, c);
    cv_rect(x + w - 1, y, 1, h, c);
}
/* a rounded-ish pill with text: lit = accent fill */
static int32_t h_pill(int32_t x, int32_t y, const char *s, uint32_t lit, uint16_t col)
{
    int32_t w = h_tw(s, 1u) + 10;
    uint16_t bg = lit ? col : H_PANEL2, fg = lit ? H_BG : H_DIM;
    cv_rect(x + 1, y, w - 2, 18, bg);
    cv_rect(x, y + 1, w, 16, bg);
    h_text(x + 5, y + 1, s, fg, bg);
    return x + w + 4;
}
static void h_bar(int32_t x, int32_t y, int32_t w, int32_t h, int32_t v, int32_t max, uint16_t c)
{
    int32_t f = max > 0 ? v * w / max : 0;
    cv_rect(x, y, w, h, H_LINE);
    if (f > 0) cv_rect(x, y, f, h, c);
}

/* ---- what the knobs do --------------------------------------------------------------------------- */
static const char *const KNOB_LABEL[2][4] = {{"Filter", "Env", "Delay", "Reverb"},
                                             {"Reso", "Attack", "D.time", "R.time"}};
static const char *const KNOB_LONG[8] = {"Cutoff", "Resonance", "Release", "Attack", "Delay level", "Delay time",
                                         "Reverb level", "Reverb time"};
static uint32_t knob_param(uint32_t k, uint32_t glo)   /* K1..K4 (0..3) -> J6_KNOB_* */
{
    static const uint8_t m[2][4] = {{J6_KNOB_CUTOFF, J6_KNOB_RELEASE, J6_KNOB_DLY_LEVEL, J6_KNOB_REV_LEVEL},
                                    {J6_KNOB_RESO, J6_KNOB_ATTACK, J6_KNOB_DLY_TIME, J6_KNOB_REV_TIME}};
    return m[glo ? 1 : 0][k & 3u];
}

static void ui_popup(const char *label, const char *value)
{
    char *p = h_cat(ui.popup, label);
    if (value && *value) {
        p = h_cat(p, "  ");
        h_cat(p, value);
    }
    ui.popup_until = fm1_ms + 1200u;
}

/* ---- settings list (human-readable, grouped) ----------------------------------------------------- */
typedef struct {
    int16_t id;                          /* J6_P_*, or -1 for a group header */
    const char *label;
} hera_item_t;
static const hera_item_t SET_ITEMS[] = {
    {-1, "PATTERN"},
    {J6_P_TEMPO10, "Tempo"},
    {J6_P_LAST, "Last step"},
    {J6_P_BEAT, "Step length"},
    {J6_P_SHUFFLE, "Shuffle"},
    {J6_P_VOL, "Pattern volume"},
    {J6_P_TRAN, "Transpose"},
    {J6_P_AVEL, "Accent velocity"},
    {J6_P_PCHG, "Pattern change"},
    {J6_P_DSYNC, "Delay tempo sync"},
    {-1, "KEYS AND CHORDS"},
    {J6_P_VELO, "Key velocity"},
    {J6_P_KEY, "Key (transpose)"},
    {J6_P_HOLDE, "Hold on step edit"},
    {J6_P_CHDL, "Chord legato"},
    {J6_P_CNTI, "Count-in"},
    {-1, "MIDI"},
    {J6_P_CH, "MIDI channel"},
    {J6_P_THRU, "MIDI thru"},
    {J6_P_TXPC, "Send prog. change"},
    {J6_P_RXPC, "Receive prog. ch."},
    {J6_P_PCCH, "Prog. change ch."},
    {-1, "SYNC"},
    {J6_P_SYNC, "Clock source"},
    {J6_P_SCLK, "Sync pulses/beat"},
    {-1, "SYSTEM"},
    {J6_P_TUNE10, "Tune"},
    {J6_P_USBD, "USB audio level"},
};
#define SET_N (sizeof SET_ITEMS / sizeof SET_ITEMS[0])

static void h_value(char *b, uint32_t id, int32_t v)
{
    static const char *const SYNC[4] = {"Auto", "Internal", "MIDI only", "USB only"};
    switch (id) {
    case J6_P_TEMPO10: h_cat(h_fix1(b, v), " BPM"); return;
    case J6_P_TUNE10: h_cat(h_fix1(b, v), " Hz"); return;
    case J6_P_BEAT:
        if (v == J6_BEAT_1_4) h_cat(b, "1/16");
        else if (v == J6_BEAT_1_3) h_cat(b, "1/8 triplet");
        else if (v == J6_BEAT_1_2) h_cat(b, "1/8");
        else h_cat(h_num(b, v - J6_BEAT_1 + 1), v == J6_BEAT_1 ? " beat" : " beats");
        return;
    case J6_P_SHUFFLE: h_cat(h_signed(b, v), v ? "%" : ""); return;
    case J6_P_TRAN: case J6_P_KEY: case J6_P_OCT: h_signed(b, v); return;
    case J6_P_PCHG: if (v == 0) h_cat(b, "Next step"); else h_cat(h_cat(h_num(b, v), v == 1 ? " beat" : " beats"), ""); return;
    case J6_P_DSYNC: case J6_P_THRU: case J6_P_TXPC: case J6_P_RXPC: case J6_P_CHDL:
        h_cat(b, v ? "On" : "Off");
        return;
    case J6_P_HOLDE: h_cat(b, v ? "Keep" : "Release"); return;
    case J6_P_CNTI: if (!v) h_cat(b, "Off"); else h_cat(h_num(b, v), " beats"); return;
    case J6_P_SYNC: h_cat(b, SYNC[v & 3]); return;
    case J6_P_SCLK: h_num(b, (int32_t)j6_sclk_ppb((uint32_t)v)); return;
    case J6_P_USBD: if (!v) h_cat(b, "= Volume knob"); else h_num(b, v); return;
    default: h_num(b, v); return;
    }
}
static int32_t set_step(uint32_t id)                 /* one detent */
{
    return id == J6_P_TEMPO10 ? 10 : 1;
}

/* ---- regions ------------------------------------------------------------------------------------- */
static j6_status_t st;
static int32_t knobv[8];

static void draw_top(void)
{
    char b[24];
    int32_t x;
    uint32_t dirty = (st.dirty[st.pattern / 32u] >> (st.pattern % 32u)) & 1u;
    cv_oy = 0;
    cv_begin(240, 20, H_PANEL);
    if (dirty) h_cat(h_pat(b, st.pattern), "*");
    else h_pat(b, st.pattern);
    x = h_text(6, 2, b, H_ACC, H_PANEL);
    if (st.queued != J6_NONE) {
        char q[8];
        h_cat(h_pat(q + 1, st.queued), "");
        q[0] = '>';
        x = h_text(x + 2, 2, q, H_DIM, H_PANEL);
    }
    if (st.seq_state == J6_SEQ_STANDBY) {          /* record: say what it is waiting for, as the J-6's blinking PLAY does */
        h_pill(x + 4, 1, "Press PLAY", 1, H_REC);
    } else if (st.seq_state == J6_SEQ_COUNTIN) {
        char c[16];
        h_cat(h_num(h_cat(c, "Count-in "), (int32_t)st.countin_left), "");
        h_pill(x + 4, 1, c, 1, H_REC);
    } else if (st.recording) {
        char c[16];
        h_cat(h_num(h_cat(c, "REC step "), st.step_now == J6_NONE ? 1 : (int32_t)st.step_now + 1), "");
        h_pill(x + 4, 1, c, 1, H_REC);
    } else {
        h_text(x + 8, 2, PAGE_TITLE[ui.page], H_TXT, H_PANEL);
    }
    h_fix1(b, st.tempo10);
    h_text_r(212, 2, b, st.ext_clock ? H_OK : H_TXT, H_PANEL);
    /* transport glyph */
    if (st.recording || st.seq_state == J6_SEQ_STANDBY || st.seq_state == J6_SEQ_COUNTIN) {
        int32_t i;
        for (i = 0; i < 10; i++) cv_rect(222 + (i < 5 ? 5 - i : i - 4) / 2, 5 + i, 10 - (i < 5 ? 5 - i : i - 4), 1, H_REC);
        if (st.seq_state == J6_SEQ_COUNTIN) {
            b[0] = (char)('0' + st.countin_left % 10u);
            b[1] = 0;
            h_text(224, 2, b, H_TXT, H_REC);
        }
    } else if (st.seq_state == J6_SEQ_PLAYING) {
        int32_t i;
        for (i = 0; i < 12; i++) cv_rect(223, 4 + i, i < 6 ? i + 1 : 12 - i, 1, H_OK);
    } else {
        cv_rect(222, 5, 10, 10, H_DIM);
    }
    cv_blit(0, 0);
}

static void draw_bottom(void)
{
    uint32_t k;
    cv_oy = 0;
    cv_begin(240, 40, H_BG);
    cv_rect(0, 0, 240, 1, H_LINE);
    if (ui.popup_until && (int32_t)(ui.popup_until - fm1_ms) > 0) {
        cv_rect(0, 1, 240, 39, H_ACCD);
        h_text_c(120, 12, ui.popup, H_TXT, H_ACCD, 1u);
        cv_blit(0, 200);
        return;
    }
    if (ui.page == PG_SETTINGS) {
        h_text_c(120, 4, "PRESETS: choose   K1: change", H_DIM, H_BG, 1u);
        h_text_c(120, 22, "Leave the page to save", H_DIM, H_BG, 1u);
        cv_blit(0, 200);
        return;
    }
    for (k = 0; k < 4u; k++) {
        int32_t x = (int32_t)k * 60;
        uint32_t pi = knob_param(k, ui.glo);
        cv_rect(x + 8, 1, 44, 2, k < 2u ? H_JRED : H_ACCD);   /* the Juno's section bars: VCF/ENV red, effects blue */
        h_text_c(x + 30, 4, KNOB_LABEL[ui.glo ? 1 : 0][k], ui.glo ? H_ACC : H_DIM, H_BG, 1u);
        h_bar(x + 8, 26, 44, 6, knobv[pi], 127, ui.glo ? H_ACC : H_TXT);
        if (k) cv_rect(x, 4, 1, 32, H_LINE);
    }
    cv_blit(0, 200);
}

/* ---- page bodies (drawn twice: rows 0..89 then 90..179 of the body, cv_oy shifts) ----------------- */
static uint32_t live_chord_idx(void)
{
    return st.last_chord == J6_NO_CHORD ? 0xFFu : j6_chord_tag_idx(st.last_chord);
}

static void body_play(void)
{
    char b[40], *p;
    uint32_t set = (uint32_t)j6_engine_get(J6_P_CHORD_SET), ci = live_chord_idx();
    const j6_pattern_t *pat = j6_engine_pattern(st.pattern);
    uint32_t ph = pat->phrase, style = j6_phr_style((uint8_t)ph), var = j6_phr_var((uint8_t)ph), y;
    if (st.chord) {
        p = h_cat(b, "Chord set ");
        p = h_num(p, (int32_t)set + 1);
        p = h_cat(p, "  ");
        h_cat(p, j6_chord_genre(set));
        h_text(10, 6, b, H_DIM, H_BG);
    } else {
        h_text(10, 6, "Notes (chord mode off)", H_DIM, H_BG);
    }
    /* the hero line: the chord (or note) last played */
    if (st.chord && ci < 12u) h_cat(b, j6_chord_name(set, ci));
    else if (st.last_note != J6_NO_NOTE) h_note(b, st.last_note);
    else h_cat(b, "Hera");
    h_text_c(120, 28, b, H_ACC, H_BG, 2u);
    y = 72;
    p = h_cat(b, "Sound ");
    p = h_pat(p, pat->sound);
    p = h_cat(p, "  ");
    h_cat(p, j6_sounds[pat->sound % J6_SOUNDS].name);
    h_text(10, (int32_t)y, b, H_TXT, H_BG);
    y += 22;
    p = h_cat(b, HERA_STYLE_NAME[style <= 9u ? style : 0]);
    p = h_cat(p, " / ");
    h_cat(p, HERA_VAR_NAME[style <= 9u ? style : 0][var <= 12u ? var : 0]);
    b[28] = 0;
    h_text(10, (int32_t)y, b, st.style_on ? H_TXT : H_DIM, H_BG);
    y += 24;
    {
        int32_t x = 10;
        char k[12];
        x = h_pill(x, (int32_t)y, "PHRASE", st.style_on, H_JORG);
        x = h_pill(x, (int32_t)y, "CHORD", st.chord, H_ACC);
        x = h_pill(x, (int32_t)y, "HOLD", st.hold, H_CREAM);
        if (j6_engine_get(J6_P_KEY)) {
            p = h_cat(k, "K");
            h_signed(p, j6_engine_get(J6_P_KEY));
            x = h_pill(x, (int32_t)y, k, 1, H_ACC);
        }
        if (j6_engine_get(J6_P_OCT)) {
            p = h_cat(k, "O");
            h_signed(p, j6_engine_get(J6_P_OCT));
            h_pill(x, (int32_t)y, k, 1, H_ACC);
        }
    }
}
static uint32_t key_play(void)
{
    const j6_pattern_t *pat = j6_engine_pattern(st.pattern);
    uint32_t h = 2166136261u;
    h = h_mix(h, (uint32_t)j6_engine_get(J6_P_CHORD_SET));
    h = h_mix(h, st.chord | st.hold << 1 | st.style_on << 2);
    h = h_mix(h, st.last_chord);
    h = h_mix(h, st.last_note);
    h = h_mix(h, pat->sound | (uint32_t)pat->phrase << 8);
    h = h_mix(h, (uint32_t)(j6_engine_get(J6_P_KEY) + 64) | (uint32_t)(j6_engine_get(J6_P_OCT) + 8) << 8);
    return h;
}

static void draw_scope(void)
{
    uint32_t i, w = scope_w;
    int32_t prev = 0;
    cv_oy = 0;
    cv_begin(240, 46, H_BG);
    cv_rect(0, 23, 240, 1, H_PANEL2);
    for (i = 0; i < SCOPE_N; i++) {
        int32_t v = scope[(w + i) % SCOPE_N], y = 23 - v * 20 / 16384;
        if (y < 1) y = 1;
        if (y > 44) y = 44;
        if (i) cv_line((int32_t)i - 1, prev, (int32_t)i, y, H_ACC);
        prev = y;
    }
    cv_blit(0, 152);
}

static void body_chords(void)
{
    char b[40], *p;
    uint32_t set = (uint32_t)j6_engine_get(J6_P_CHORD_SET), k, ci = live_chord_idx();
    p = h_cat(b, "Set ");
    p = h_num(p, (int32_t)set + 1);
    h_cat(p, " of 100");
    h_text(10, 4, b, H_ACC, H_BG);
    h_text_r(230, 4, j6_chord_genre(set), H_TXT, H_BG);
    if (!st.chord) h_text(10, 22, "CHORD is off: tap SEL to play these", H_DIM, H_BG);
    else h_text(10, 22, "PRESETS knob: browse sets", H_DIM, H_BG);
    /* the key map: two columns of six keys, laid out like a keyboard (black keys darker) */
    for (k = 0; k < 12u; k++) {
        int32_t col = (int32_t)(k / 6u), row = (int32_t)(k % 6u), x = 6 + col * 117, y = 44 + row * 22;
        uint32_t black = (0x54Au >> k) & 1u, now = st.chord && ci == k;
        uint16_t bg = now ? H_ACC : black ? H_BLACKKEY : H_WHITEKEY, fg = now ? H_BG : H_TXT;
        cv_rect(x, y, 111, 20, bg);
        cv_rect(x, y, 30, 20, now ? H_ACC : black ? RGB(4, 4, 6) : H_CREAM);
        h_text_c(x + 15, y + 2, NOTE_NM[k], now ? H_BG : black ? H_TXT : H_BG, now ? H_ACC : black ? RGB(4, 4, 6) : H_CREAM, 1u);
        h_cat(b, j6_chord_name(set, k));
        b[9] = 0;
        h_text(x + 36, y + 2, b, fg, bg);
    }
}
static uint32_t key_chords(void)
{
    uint32_t h = 2166136261u;
    h = h_mix(h, (uint32_t)j6_engine_get(J6_P_CHORD_SET));
    h = h_mix(h, st.chord);
    return h_mix(h, st.chord ? st.last_chord : 0xFFu);
}

static void body_phrase(void)
{
    char b[40], *p;
    const j6_pattern_t *pat = j6_engine_pattern(st.pattern);
    uint32_t ph = st.sel_step != J6_NONE ? pat->step[st.sel_step].phrase : pat->phrase;
    uint32_t style = j6_phr_style((uint8_t)ph), var = j6_phr_var((uint8_t)ph);
    if (style < 1u || style > 9u) style = 1;
    if (var < 1u || var > 12u) var = 1;
    p = h_cat(b, "Style ");
    p = h_num(p, (int32_t)style);
    p = h_cat(p, " / Variation ");
    h_num(p, (int32_t)var);
    h_text(10, 4, b, H_DIM, H_BG);
    h_pill(186, 2, st.style_on ? "ON" : "OFF", st.style_on, H_JORG);
    h_text_c(120, 26, HERA_STYLE_NAME[style], H_ACC, H_BG, 2u);
    h_text_c(120, 64, HERA_VAR_NAME[style][var], H_TXT, H_BG, 1u);
    /* a piano-roll preview of the J-6 table (styles 6..9: the captured loops, 3 held notes) */
    cv_rect(8, 86, 224, 70, H_PANEL);
    if (style >= 6u) {
        const j6_plist_t *l = &j6_phrase_builtin.lists[j6_plist_index(style, var, 3u)];
        uint32_t i, loop = l->loop ? l->loop : 384u, beats = loop / 96u;
        for (i = 1; i < beats; i++) cv_rect(8 + (int32_t)(i * 224u / beats), 86, 1, 70, H_PANEL2);
        for (i = 0; i < l->count; i++) {
            const j6_pev_t *e = &j6_phrase_builtin.pool[l->first + i];
            uint32_t tone = e->src & J6_PEV_SRC_MASK, x0 = (uint32_t)e->t / 16u * 224u / loop,
                     w = (uint32_t)e->len / 16u * 224u / loop;
            int32_t y = 86 + 52 - (int32_t)tone * 22;
            if (w < 2u) w = 2u;
            if (x0 + w > 224u) w = 224u - x0;
            cv_rect(8 + (int32_t)x0, y, (int32_t)w, 14, (e->src & J6_PEV_ACCENT) ? H_JORG : H_TXT);
        }
        p = h_num(b, (int32_t)beats);
        h_cat(p, beats == 1u ? "-beat loop" : "-beat loop");
        h_text(10, 160, b, H_DIM, H_BG);
        cv_rect(160, 164, 10, 8, H_JORG);
        h_text_r(230, 160, "accent", H_DIM, H_BG);
    } else {
        h_text_c(120, 112, "Arpeggio / rhythm style", H_DIM, H_PANEL, 1u);
        h_text(10, 160, "Hold keys to hear it", H_DIM, H_BG);
    }
}
static uint32_t key_phrase(void)
{
    const j6_pattern_t *pat = j6_engine_pattern(st.pattern);
    uint32_t h = 2166136261u;
    h = h_mix(h, pat->phrase);
    h = h_mix(h, st.sel_step);
    if (st.sel_step != J6_NONE) h = h_mix(h, pat->step[st.sel_step].phrase);
    return h_mix(h, st.style_on);
}

static void step_label(char *b, const j6_step_t *s, uint32_t set)
{
    uint32_t n = 0, i;
    for (i = 0; i < J6_STEP_NOTES; i++)
        if (s->note[i] != J6_NO_NOTE) n++;
    b[0] = 0;
    if (s->chord != J6_NO_CHORD && n) {
        h_cat(b, j6_chord_name(set, j6_chord_tag_idx(s->chord)));
        b[7] = 0;
    } else if (n) {
        char *p = h_cat(b, NOTE_NM[s->note[0] % 12u]);
        if (n > 1u) {
            *p++ = '+';
            *p++ = (char)('0' + n - 1u);
            *p = 0;
        }
    } else if (s->flags & J6_STF_TIE) {
        h_cat(b, "~");
    }
}
static void body_seq(void)
{
    char b[40], *p;
    const j6_pattern_t *pat = j6_engine_pattern(st.pattern);
    uint32_t set = (uint32_t)j6_engine_get(J6_P_CHORD_SET), i, base = ui.seq_page * 16u;
    p = h_num(b, (int32_t)base + 1);
    p = h_cat(p, "-");
    p = h_num(p, (int32_t)base + 16);
    p = h_cat(p, " / last ");
    h_num(p, pat->last);
    h_text(10, 4, b, H_DIM, H_BG);
    if (st.sel_step != J6_NONE) {
        const j6_step_t *ss = &pat->step[st.sel_step];
        p = h_cat(b, "EDIT ");
        p = h_num(p, (int32_t)st.sel_step + 1);
        if (ss->chord != J6_NO_CHORD && ss->note[0] != J6_NO_NOTE) {
            p = h_cat(p, " ");
            h_cat(p, j6_chord_name(set, j6_chord_tag_idx(ss->chord)));
        }
        h_pill(236 - h_tw(b, 1u) - 10, 2, b, 1, H_ACC);
    }
    for (i = 0; i < 16u; i++) {
        uint32_t s = base + i, row = i / 8u, col = i % 8u;
        int32_t x = 2 + (int32_t)col * 30, y = 26 + (int32_t)row * 62;
        const j6_step_t *sp = &pat->step[s];
        uint32_t beyond = s >= pat->last, sel = st.sel_step == s, now = st.step_now == s;
        uint16_t bg = sel ? H_ACC : beyond ? H_BG : H_PANEL, fg = sel ? H_BG : H_TXT;
        cv_rect(x, y, 28, 58, bg);
        if (now) h_frame(x, y, 28, 58, H_OK);
        if (beyond) h_frame(x, y, 28, 58, H_PANEL);
        b[0] = 0;
        h_num(b, (int32_t)s + 1);
        h_text_c(x + 14, y + 2, b, sel ? H_BG : H_DIM, bg, 1u);
        step_label(b, sp, set);
        {
            uint32_t r = (b[0] && b[1] == '#') ? 2u : 1u;
            if (sp->chord != J6_NO_CHORD && str_len(b) > r) {
                char q[8];
                uint32_t j;
                str_cpy(q, b + r, sizeof q);
                for (j = 0; q[j]; j++)
                    if (q[j] == '(' || q[j] == '/') q[j] = 0;   /* the cell: the short form */
                q[3] = 0;
                b[r] = 0;
                h_text_c(x + 14, y + 18, b, fg, bg, 1u);
                h_text_c(x + 14, y + 33, q, sel ? H_BG : H_DIM, bg, 1u);
            } else {
                h_text_c(x + 14, y + 24, b, fg, bg, 1u);
            }
        }
        if (sp->flags & J6_STF_TIE) cv_rect(x - 2, y + 44, 8, 3, sel ? H_BG : H_ACC);
        if (j6_phr_on(sp->phrase)) cv_rect(x + 11, y + 50, 6, 4, H_JORG);
    }
    if (st.sel_step != J6_NONE) {
        if (ui.glo) {
            h_text(4, 150, "GLO + F3 copy   G3 paste", H_ACC, H_BG);
            h_text(4, 164, "A3 insert  B3 clear  C4 x2", H_ACC, H_BG);
        } else {
            h_text(4, 150, "Keys add notes   SEQ: done", H_DIM, H_BG);
            h_text(4, 164, "ENV: tie   PRESETS: move", H_DIM, H_BG);
        }
    } else {
        h_text(4, 150, "PRESETS: pick a step to edit", H_DIM, H_BG);
        h_text(4, 164, "OCT: page   SEQ: patterns", H_DIM, H_BG);
    }
}
static uint32_t key_seq(void)
{
    const j6_pattern_t *pat = j6_engine_pattern(st.pattern);
    uint32_t h = 2166136261u, i;
    const uint8_t *b = (const uint8_t *)&pat->step[ui.seq_page * 16u];
    for (i = 0; i < 16u * sizeof(j6_step_t); i++) h = h_mix(h, b[i]);
    h = h_mix(h, ui.seq_page | (uint32_t)pat->last << 8 | (uint32_t)st.sel_step << 16 | (uint32_t)ui.glo << 24);
    h = h_mix(h, st.step_now);
    return h_mix(h, (uint32_t)j6_engine_get(J6_P_CHORD_SET));
}

static uint32_t pat_has_notes(uint32_t idx)
{
    const j6_pattern_t *p = j6_engine_pattern(idx);
    uint32_t s;
    for (s = 0; s < p->last && s < J6_STEPS; s++)
        if (p->step[s].note[0] != J6_NO_NOTE) return 1;
    return 0;
}
static void body_patterns(void)
{
    uint32_t r, c;
    char b[4];
    for (c = 0; c < 8u; c++) {
        b[0] = (char)('1' + c);
        b[1] = 0;
        h_text_c(38 + (int32_t)c * 25, 2, b, H_DIM, H_BG, 1u);
    }
    for (r = 0; r < 8u; r++) {
        b[0] = (char)('A' + r);
        b[1] = 0;
        h_text(8, 21 + (int32_t)r * 17, b, H_DIM, H_BG);
        for (c = 0; c < 8u; c++) {
            uint32_t idx = r * 8u + c, cur = idx == st.pattern, q = idx == st.queued;
            uint32_t dirty = (st.dirty[idx / 32u] >> (idx % 32u)) & 1u;
            int32_t x = 27 + (int32_t)c * 25, y = 22 + (int32_t)r * 17;
            uint16_t bg = cur ? H_ACC : pat_has_notes(idx) ? H_PANEL2 : H_PANEL;
            cv_rect(x, y, 20, 15, bg);
            if (q) h_frame(x, y, 20, 15, H_OK);
            if (dirty) cv_rect(x + 15, y + 2, 3, 3, cur ? H_BG : H_JRED);
        }
    }
    h_text(8, 162, "PRESETS: pattern +GLO: bank", H_DIM, H_BG);
}
static uint32_t key_patterns(void)
{
    uint32_t h = 2166136261u, i;
    h = h_mix(h, st.pattern | (uint32_t)st.queued << 8);
    h = h_mix(h, st.dirty[0]);
    h = h_mix(h, st.dirty[1]);
    for (i = 0; i < J6_PATTERNS; i++) h = h_mix(h, pat_has_notes(i));
    return h;
}

static void body_sounds(void)
{
    const j6_pattern_t *pat = j6_engine_pattern(st.pattern);
    uint32_t cur = pat->sound, bank = cur / 8u, i;
    char b[32], *p;
    for (i = 0; i < 8u; i++) {
        b[0] = (char)('A' + i);
        b[1] = 0;
        h_pill(8 + (int32_t)i * 28, 2, b, i == bank, H_ACC);
    }
    for (i = 0; i < 8u; i++) {
        uint32_t s = bank * 8u + i, now = s == cur;
        int32_t y = 24 + (int32_t)i * 17;
        uint16_t bg = now ? H_ACC : H_BG;
        cv_rect(6, y, 228, 17, bg);
        p = h_pat(b, s);
        p = h_cat(p, "  ");
        h_cat(p, j6_sounds[s].name);
        h_text(12, y + 1, b, now ? H_BG : H_TXT, bg);
    }
    h_text(8, 162, "PRESETS: sound  +GLO: bank", H_DIM, H_BG);
}
static uint32_t key_sounds(void) { return h_mix(2166136261u, j6_engine_pattern(st.pattern)->sound); }

static void body_fx(void)
{
    char b[32], *p;
    int32_t dt = knobv[J6_KNOB_DLY_TIME], sync = j6_engine_get(J6_P_DSYNC);
    h_text(10, 6, "DELAY", H_ACC, H_BG);
    h_text(10, 28, "Level", H_DIM, H_BG);
    h_bar(80, 32, 140, 8, knobv[J6_KNOB_DLY_LEVEL], 127, H_TXT);
    h_text(10, 50, "Time", H_DIM, H_BG);
    if (sync) {
        static const char *const NV[J6_DSYNC_VALUES] = {"1/32", "1/16T", "1/16", "1/8T", "1/16.", "1/8",
                                                        "1/4T", "1/8.", "1/4", "1/2T", "1/4.", "1/2"};
        p = h_cat(b, NV[(uint32_t)dt * J6_DSYNC_VALUES / 128u]);
        h_cat(p, " (tempo sync)");
    } else {
        p = h_num(b, dt);
        h_cat(p, " / 127");
    }
    h_text(80, 50, b, H_TXT, H_BG);
    h_text(10, 90, "REVERB", H_ACC, H_BG);
    h_text(10, 112, "Level", H_DIM, H_BG);
    h_bar(80, 116, 140, 8, knobv[J6_KNOB_REV_LEVEL], 127, H_TXT);
    h_text(10, 134, "Time", H_DIM, H_BG);
    h_bar(80, 138, 140, 8, knobv[J6_KNOB_REV_TIME], 127, H_TXT);
    h_text(10, 160, "K3 / K4, hold GLO for time", H_DIM, H_BG);
}
static uint32_t key_fx(void)
{
    uint32_t h = 2166136261u, i;
    for (i = 4; i < 8u; i++) h = h_mix(h, (uint32_t)knobv[i]);
    return h_mix(h, (uint32_t)j6_engine_get(J6_P_DSYNC));
}

#define SET_ROWS 8u
static void body_settings(void)
{
    uint32_t r;
    char b[24];
    for (r = 0; r < SET_ROWS && ui.set_top + r < SET_N; r++) {
        const hera_item_t *it = &SET_ITEMS[ui.set_top + r];
        int32_t y = 2 + (int32_t)r * 22;
        uint32_t cur = ui.set_top + r == ui.set_cursor;
        if (it->id < 0) {
            h_text(8, y + 4, it->label, H_ACC, H_BG);
            cv_rect(8, y + 20, 224, 1, H_LINE);
            continue;
        }
        cv_rect(2, y, 236, 21, cur ? H_PANEL2 : H_BG);
        if (cur) cv_rect(2, y, 3, 21, H_ACC);
        h_text(10, y + 3, it->label, H_TXT, cur ? H_PANEL2 : H_BG);
        h_value(b, (uint32_t)it->id, j6_engine_get((uint32_t)it->id));
        h_text_r(232, y + 3, b, cur ? H_ACC : H_DIM, cur ? H_PANEL2 : H_BG);
    }
}
static uint32_t key_settings(void)
{
    uint32_t h = 2166136261u, i;
    h = h_mix(h, ui.set_cursor | (uint32_t)ui.set_top << 8);
    for (i = 0; i < SET_N; i++)
        if (SET_ITEMS[i].id >= 0) h = h_mix(h, (uint32_t)j6_engine_get((uint32_t)SET_ITEMS[i].id));
    return h;
}

static void draw_body(void)
{
    uint32_t half, rows = ui.page == PG_PLAY ? 132u : 180u;
    for (half = 0; half * 90u < rows; half++) {
        uint32_t h = rows - half * 90u > 90u ? 90u : rows - half * 90u;
        cv_begin(240, h, H_BG);
        cv_oy = -(int32_t)(half * 90u);
        switch (ui.page) {
        case PG_PLAY: body_play(); break;
        case PG_CHORDS: body_chords(); break;
        case PG_PHRASE: body_phrase(); break;
        case PG_SEQ: body_seq(); break;
        case PG_PATTERNS: body_patterns(); break;
        case PG_SOUNDS: body_sounds(); break;
        case PG_FX: body_fx(); break;
        default: body_settings(); break;
        }
        cv_oy = 0;
        cv_blit(0, 20u + half * 90u);
    }
}
static uint32_t key_body(void)
{
    uint32_t k;
    switch (ui.page) {
    case PG_PLAY: k = key_play(); break;
    case PG_CHORDS: k = key_chords(); break;
    case PG_PHRASE: k = key_phrase(); break;
    case PG_SEQ: k = key_seq(); break;
    case PG_PATTERNS: k = key_patterns(); break;
    case PG_SOUNDS: k = key_sounds(); break;
    case PG_FX: k = key_fx(); break;
    default: k = key_settings(); break;
    }
    return h_mix(k, ui.page);
}
static uint32_t key_top(void)
{
    uint32_t h = 2166136261u;
    h = h_mix(h, st.pattern | (uint32_t)st.queued << 8 | (uint32_t)st.seq_state << 16 | (uint32_t)st.recording << 24);
    h = h_mix(h, st.recording ? (uint32_t)st.step_now : 0u);
    h = h_mix(h, st.tempo10 | (uint32_t)st.ext_clock << 16 | (uint32_t)st.countin_left << 20);
    h = h_mix(h, (st.dirty[st.pattern / 32u] >> (st.pattern % 32u)) & 1u);
    return h_mix(h, ui.page);
}
static uint32_t key_bottom(void)
{
    uint32_t h = 2166136261u, i;
    int32_t pop = ui.popup_until && (int32_t)(ui.popup_until - fm1_ms) > 0;
    if (!pop) ui.popup_until = 0;
    h = h_mix(h, (uint32_t)pop | ui.glo << 1 | (uint32_t)(ui.page == PG_SETTINGS) << 2);
    if (pop) return h_mixs(h, ui.popup);
    for (i = 0; i < 8u; i++) h = h_mix(h, (uint32_t)knobv[i]);
    return h;
}

/* redraw what changed; call ~30 times a second from the main loop */
static void ui_draw(void)
{
    uint32_t k, i;
    j6_engine_status(&st);
    for (i = 0; i < 8u; i++) knobv[i] = j6_engine_get(J6_P_KNOB0 + i);
    if (ui.force) {
        ui.key_top = ui.key_body = ui.key_bot = ui.key_scope = 0;
        ui.force = 0;
    }
    if ((k = key_top()) != ui.key_top) {
        draw_top();
        ui.key_top = k;
    }
    if ((k = key_body()) != ui.key_body) {
        draw_body();
        ui.key_body = k;
        ui.key_scope = 0;
    }
    if ((k = key_bottom()) != ui.key_bot) {
        draw_bottom();
        ui.key_bot = k;
    }
    if (ui.page == PG_PLAY) {                /* live while anything sounds, then one flat frame */
        uint32_t live = st.voices || !st.fx_idle;
        if (live || ui.key_scope != 1u) {
            draw_scope();
            ui.key_scope = live ? 2u : 1u;
        }
    }
}

static void ui_page(uint32_t pg)
{
    if (ui.page == PG_SETTINGS && pg != PG_SETTINGS) j6_engine_save_globals();   /* PS-3: a menu is left */
    ui.page = (uint8_t)pg;
    ui.force = 1;
}
