/* SPDX-License-Identifier: GPL-3.0-only */
/* HERA: a J-6-style chord groovebox for the M-VAVE FM-1 (chord sets, phrase styles, a 64-pattern step
 * sequencer, a Juno-style synth). Juno's Greek counterpart is Hera.
 *
 * One translation unit: platform.c (the Felucca / Dinghy platform: USB, MIDI, keys, screen, updates), the
 * engine (firmware/src/j6: chord, phrase, sequencer, synth, glue; host-tested in tests/j6), the screen
 * (hera_ui.c) and this file: the FM-1 controls, storage and the main loop.
 *
 *   PLAY        tap: play / stop          hold 0.6 s: full stop (all sound off)    GLO+PLAY: play from step
 *   REC         record                    SCL tap: CHORD on/off   hold (or GLO+SCL): the chord map
 *   ARP tap     STYLE (phrase) on/off     hold: the phrase page    ENV: HOLD (in step edit: tie)
 *   HOME        play page   SEQ: sequencer (GLO+SEQ: patterns)   LFO: sounds   FX: effects   EDIT: settings
 *   SAVE        write the pattern (GLO+SAVE: all)                OCT -/+: octave (GLO: key; sequencer: page)
 *   K1..K4      filter, envelope, delay, reverb (GLO: resonance, attack, delay time, reverb time)
 *   SELECT      style 1..9     ALGORITHM: variation 1..12    PRESETS: tempo, or what the page browses
 *   keys        notes / chords; on the sequencer page the white keys pick steps
 * Owner decisions: docs/J6_DECISIONS.md. */
#ifndef APP_NAME
#define APP_NAME "HERA"
#endif
#ifndef FELUCCA_ID
#define FELUCCA_ID "FM-1_4300100"
#endif

/* storage: 65 sectors of 4 KiB in Felucca's data area (patterns 0..63, then the globals) */
#define APP_STORE_LO 0x00097000u
#define APP_STORE_HI (APP_STORE_LO + 65u * 0x1000u)
#include "platform.c"

#include "j6/j6_chord.c"
#include "j6/j6_phrase.c"
#include "j6/j6_phrase_held.c"
#include "j6/j6_seq.c"
#include "j6/j6_seq_clock.c"
#include "j6/j6_seq_pattern.c"
#include "j6/j6_synth.c"
#include "j6/j6_synth_fx.c"
#include "j6/j6_synth_sounds.c"
#include "j6/j6_engine.c"
#include "hera_ui.c"

static int16_t hera_fx[J6_FX_MEM_LEN] __attribute__((section(".pool")));

/* ---- audio (the ISR): the engine renders, the scope samples it --------------------------------- */
static void app_audio(int32_t *out, uint32_t n)
{
    uint32_t i, w = scope_w;
    j6_engine_render(out, n);
    for (i = 0; i < n; i += 4u) {
        int32_t v = (out[2u * i] + out[2u * i + 1u]) >> 1;
        scope[w] = (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
        w = w + 1u < SCOPE_N ? w + 1u : 0u;
    }
    scope_w = w;
}

/* ---- storage: one sector per slot: {magic, length, crc32} then the data --------------------------- */
#define HERA_MAGIC 0x41524548u               /* "HERA" */
#ifdef DINGHY_HOST
static uint8_t host_flash[65][4096];
static int hs_read(uint32_t off, void *d, uint32_t n)
{
    uint32_t s = (off - APP_STORE_LO) / 4096u, o = (off - APP_STORE_LO) % 4096u;
    memcpy(d, &host_flash[s][o], n);
    return 0;
}
static int hs_erase(uint32_t off)
{
    memset(host_flash[(off - APP_STORE_LO) / 4096u], 0xFF, 4096u);
    return 0;
}
static int hs_write(uint32_t off, const void *p, uint32_t n)
{
    uint32_t s = (off - APP_STORE_LO) / 4096u, o = (off - APP_STORE_LO) % 4096u;
    memcpy(&host_flash[s][o], p, n);
    return 0;
}
#define flash_ok 1
static uint32_t crc32(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFu, k;
    while (n--) {
        c ^= *p++;
        for (k = 0; k < 8u; k++) c = c & 1u ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    return ~c;
}
#else
static int hs_read(uint32_t off, void *d, uint32_t n) { return st_read(off, d, n); }
static int hs_erase(uint32_t off)
{
    uint32_t took;
    return fl_erase4k(off, &took);
}
static int hs_write(uint32_t off, const void *p, uint32_t n) { return fl_write(off, p, n); }
#endif

static int store_read(void *ctx, uint32_t slot, uint8_t *buf, uint32_t len)
{
    uint32_t h[3], off = APP_STORE_LO + slot * 0x1000u;
    (void)ctx;
    if (!flash_ok || slot > J6_SLOT_GLOBAL || len > 4096u - sizeof h) return -1;
    if (hs_read(off, h, sizeof h) || h[0] != HERA_MAGIC || h[1] != len) return -1;
    if (hs_read(off + sizeof h, buf, len) || crc32(buf, len) != h[2]) return -1;
    return 0;
}
static int store_write(void *ctx, uint32_t slot, const uint8_t *buf, uint32_t len)
{
    uint32_t h[3], off = APP_STORE_LO + slot * 0x1000u;
    int rc;
    (void)ctx;
    if (!flash_ok || slot > J6_SLOT_GLOBAL || len > 4096u - sizeof h) return -1;
    h[0] = HERA_MAGIC;
    h[1] = len;
    h[2] = crc32(buf, len);
    if ((rc = hs_erase(off)) != 0) return rc;
    if ((rc = hs_write(off + sizeof h, buf, len)) != 0) return rc;
    return hs_write(off, h, sizeof h);           /* the header last: a cut write stays invalid */
}
static const j6_store_t hera_store = {store_read, store_write, NULL};

/* ---- the controls -------------------------------------------------------------------------------- */
#define LONG_MS 600u
#define BTN(b) (1u << (b))
static const int8_t WHITE_STEP[27] = {      /* FM-1 key (0 = F3) -> white key index 0..15, -1 black */
    0, -1, 1, -1, 2, -1, 3, 4, -1, 5, -1, 6, 7, -1, 8, -1, 9, -1, 10, 11, -1, 12, -1, 13, 14, -1, 15};
static uint32_t prev_notes, consumed, btn_t0[14], btn_long;

static int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

static void set_popup_param(const char *label, uint32_t id)
{
    char b[24];
    h_value(b, id, j6_engine_get(id));
    ui_popup(label, b);
}
static void popup_num(const char *label, int32_t v)
{
    char b[12];
    h_num(b, v);
    ui_popup(label, b);
}

static void settings_move(int32_t d)
{
    int32_t c = ui.set_cursor;
    while (d) {
        int32_t step = d > 0 ? 1 : -1, n = c + step;
        while (n >= 0 && n < (int32_t)SET_N && SET_ITEMS[n].id < 0) n += step;
        if (n < 0 || n >= (int32_t)SET_N) break;
        c = n;
        d -= step;
    }
    ui.set_cursor = (uint8_t)c;
    if (ui.set_cursor < ui.set_top + 1u) ui.set_top = (uint8_t)(ui.set_cursor > 0 ? ui.set_cursor - 1u : 0u);
    if (ui.set_cursor >= ui.set_top + SET_ROWS) ui.set_top = (uint8_t)(ui.set_cursor - SET_ROWS + 1u);
}

static void enc_presets(int32_t d, const j6_status_t *s)
{
    uint32_t pg = ui.page;
    if (pg == PG_SETTINGS) {
        settings_move(d);
    } else if (pg == PG_CHORDS) {
        int32_t v = (j6_engine_get(J6_P_CHORD_SET) + d + 1000) % (int32_t)J6_CHORD_SETS;
        j6_engine_set(J6_P_CHORD_SET, v);
        popup_num("Chord set", v + 1);
    } else if (pg == PG_SOUNDS) {              /* a sound a detent, GLO a bank */
        j6_engine_set(J6_P_SOUND, clampi(j6_engine_get(J6_P_SOUND) + d * (ui.glo ? 8 : 1), 0, (int32_t)J6_SOUNDS - 1));
    } else if (pg == PG_PATTERNS) {
        j6_engine_cmd(J6_CMD_PATTERN, clampi((int32_t)s->pattern + d, 0, (int32_t)J6_PATTERNS - 1));
    } else if (pg == PG_SEQ && s->sel_step != J6_NONE) {
        int32_t n = clampi((int32_t)s->sel_step + d, 0, (int32_t)J6_STEPS - 1);
        j6_engine_cmd(J6_CMD_STEP_SELECT, n);
        ui.seq_page = (uint8_t)(n / 16);
    } else {                                   /* tempo: 1 BPM a detent, GLO 0.1 */
        j6_engine_set(J6_P_TEMPO10, j6_engine_get(J6_P_TEMPO10) + d * (ui.glo ? 1 : 10));
        set_popup_param("Tempo", J6_P_TEMPO10);
    }
}

static void encoders(const j6_status_t *s)
{
    int32_t d;
    uint32_t k;
    for (k = 0; k < 4u; k++) {
        if (!(d = fm1_enc_take(ENC_KNOB1 + k))) continue;
        if (ui.page == PG_SETTINGS) {
            if (k == 0u) {
                uint32_t id = (uint32_t)SET_ITEMS[ui.set_cursor].id;
                j6_engine_set(id, j6_engine_get(id) + d * set_step(id));
                set_popup_param(SET_ITEMS[ui.set_cursor].label, id);
            }
            continue;
        }
        {
            uint32_t pi = knob_param(k, ui.glo);
            int32_t v = clampi(j6_engine_get(J6_P_KNOB0 + pi) + d * 2, 0, 127);
            j6_engine_set(J6_P_KNOB0 + pi, v);
            popup_num(KNOB_LONG[pi], v);
        }
    }
    if ((d = fm1_enc_take(ENC_SELECT)) != 0) {
        int32_t v = clampi(j6_engine_get(J6_P_STYLE) + d, 1, (int32_t)J6_STYLES);
        j6_engine_set(J6_P_STYLE, v);
        ui_popup("Style", HERA_STYLE_NAME[v]);
    }
    if ((d = fm1_enc_take(ENC_ALGORITHM)) != 0) {
        int32_t sty = j6_engine_get(J6_P_STYLE), v = clampi(j6_engine_get(J6_P_VAR) + d, 1, (int32_t)J6_VARS);
        j6_engine_set(J6_P_VAR, v);
        ui_popup("Variation", HERA_VAR_NAME[sty >= 1 && sty <= 9 ? sty : 1][v]);
    }
    if ((d = fm1_enc_take(ENC_PRESETS)) != 0) enc_presets(d, s);
}

/* a white key on a page that uses them for choosing (1 = consumed) */
static uint32_t page_key(uint32_t key, const j6_status_t *s)
{
    int32_t w = WHITE_STEP[key];
    if (ui.page == PG_SEQ) {
        if (s->sel_step == J6_NONE) {
            if (w < 0) return 0;
            j6_engine_cmd(J6_CMD_STEP_SELECT, (int32_t)ui.seq_page * 16 + w);
            return 1;
        }
        if (ui.glo) {                          /* step tools */
            static const uint8_t TOOL[5] = {J6_CMD_STEP_COPY, J6_CMD_STEP_PASTE, J6_CMD_STEP_INSERT,
                                            J6_CMD_STEP_CLEAR, J6_CMD_PATTERN_DUPL};
            static const char *const TN[5] = {"Step copied", "Step pasted", "Step inserted", "Step cleared",
                                              "Pattern doubled"};
            if (w >= 0 && w < 5) {
                j6_engine_cmd(TOOL[w], 0);
                ui_popup(TN[w], "");
            }
            return 1;
        }
        return 0;                              /* step edit: the key enters notes */
    }
    if (w < 0) return 0;
    if (ui.page == PG_PATTERNS) {
        int32_t p = w < 8 ? (int32_t)(s->pattern / 8u) * 8 + w : (w - 8) * 8 + (int32_t)(s->pattern % 8u);
        j6_engine_cmd(J6_CMD_PATTERN, p);
        return 1;
    }
    return 0;                                  /* the sounds page: keys play, so you hear each sound */
}

static void keys(const j6_status_t *s)
{
    uint32_t now = fm1_in.notes, ch = now ^ prev_notes, k;
    for (k = 0; k < 27u; k++) {
        uint32_t bit = 1u << k;
        if (!(ch & bit)) continue;
        if (now & bit) {
            if (page_key(k, s)) {
                consumed |= bit;
                continue;
            }
            j6_engine_key((int32_t)k + J6_KEY_MIN, 1);
        } else {
            if (consumed & bit) {
                consumed &= ~bit;
                continue;
            }
            j6_engine_key((int32_t)k + J6_KEY_MIN, 0);
        }
    }
    prev_notes = now;
}

static void buttons(const j6_status_t *s)
{
    uint32_t rel = 0, pr = fm1_input_edges(&rel), held = fm1_in.buttons, b;
    ui.glo = (held & BTN(BTN_GLO)) ? 1u : 0u;
    for (b = 0; b < 14u; b++)
        if (pr & BTN(b)) btn_t0[b] = fm1_ms, btn_long &= ~BTN(b);
    /* long presses fire while held */
    if ((held & BTN(BTN_PLAY)) && !(btn_long & BTN(BTN_PLAY)) && fm1_ms - btn_t0[BTN_PLAY] >= LONG_MS && !ui.glo) {
        btn_long |= BTN(BTN_PLAY);
        j6_engine_cmd(J6_CMD_STOP, 0);
        j6_engine_cmd(J6_CMD_PANIC, 0);        /* D2: full stop, all sound off (6 ms fade) */
        ui_popup("Full stop", "all sound off");
    }
    if ((held & BTN(BTN_SEL)) && !(btn_long & BTN(BTN_SEL)) && fm1_ms - btn_t0[BTN_SEL] >= LONG_MS) {
        btn_long |= BTN(BTN_SEL);
        ui_page(PG_CHORDS);
    }
    if ((held & BTN(BTN_ARP)) && !(btn_long & BTN(BTN_ARP)) && fm1_ms - btn_t0[BTN_ARP] >= LONG_MS) {
        btn_long |= BTN(BTN_ARP);
        ui_page(PG_PHRASE);
    }
    /* taps act on release (so a long press is not also a tap) */
    if ((rel & BTN(BTN_PLAY)) && !(btn_long & BTN(BTN_PLAY))) {
        if (ui.glo) j6_engine_cmd(J6_CMD_PLAY_FROM, s->sel_step != J6_NONE ? s->sel_step : 0);
        else j6_engine_cmd(J6_CMD_PLAY, 0);
    }
    if ((rel & BTN(BTN_SEL)) && !(btn_long & BTN(BTN_SEL))) {
        if (ui.glo) ui_page(PG_CHORDS);
        else {
            j6_engine_set(J6_P_CHORD, !s->chord);
            ui_popup("Chord mode", s->chord ? "off" : "on");
        }
    }
    if ((rel & BTN(BTN_ARP)) && !(btn_long & BTN(BTN_ARP))) {
        j6_engine_cmd(J6_CMD_STYLE, 0);
        ui_popup("Phrase", s->style_on ? "off" : "on");
    }
    if (pr & BTN(BTN_REC)) j6_engine_cmd(J6_CMD_REC, 0);
    if (pr & BTN(BTN_ENV)) {
        j6_engine_cmd(J6_CMD_HOLD, 0);
        if (s->sel_step == J6_NONE) ui_popup("Hold", s->hold ? "off" : "on");
    }
    if (pr & BTN(BTN_HOME)) ui_page(PG_PLAY);
    if (pr & BTN(BTN_SEQ)) {
        if (ui.glo) ui_page(PG_PATTERNS);
        else if (ui.page == PG_SEQ && s->sel_step != J6_NONE) j6_engine_cmd(J6_CMD_STEP_SELECT, -1);
        else ui_page(PG_SEQ);
    }
    if (pr & BTN(BTN_LFO)) ui_page(PG_SOUNDS);
    if (pr & BTN(BTN_FX)) ui_page(PG_FX);
    if (pr & BTN(BTN_EDIT)) ui_page(ui.page == PG_SETTINGS ? PG_PLAY : PG_SETTINGS);
    if (pr & BTN(BTN_SAVE)) {
        char b[12];
        h_pat(b, s->pattern);
        if (j6_engine_write(ui.glo ? 1u : 0u) == 0) ui_popup(ui.glo ? "Saved" : "Saved", ui.glo ? "all patterns" : b);
        else ui_popup("Save failed", "");
    }
    if (pr & (BTN(BTN_OCT_DOWN) | BTN(BTN_OCT_UP))) {
        int32_t d = (pr & BTN(BTN_OCT_UP)) ? 1 : -1;
        if (ui.page == PG_SEQ && s->sel_step == J6_NONE) {
            ui.seq_page = (uint8_t)clampi((int32_t)ui.seq_page + d, 0, 3);
        } else if (ui.glo) {
            j6_engine_set(J6_P_KEY, j6_engine_get(J6_P_KEY) + d);
            set_popup_param("Key", J6_P_KEY);
        } else {
            j6_engine_set(J6_P_OCT, j6_engine_get(J6_P_OCT) + d);
            set_popup_param("Octave", J6_P_OCT);
        }
    }
}

static void leds(const j6_status_t *s)
{
    uint32_t k, blink = (fm1_ms / 250u) & 1u;
    for (k = 0; k < 27u; k++) {
        uint32_t on = (s->keys_lit >> k) & 1u;
        if (ui.page == PG_SEQ && s->sel_step == J6_NONE && WHITE_STEP[k] >= 0) {
            uint32_t step = ui.seq_page * 16u + (uint32_t)WHITE_STEP[k];
            const j6_pattern_t *p = j6_engine_pattern(s->pattern);
            on = (step < p->last && p->step[step].note[0] != J6_NO_NOTE) || (s->step_now == step && blink);
        }
        fm1_led_key(KEY_LED(k), (int)on);
    }
    fm1_led_key(BTN_PLAY, s->seq_state == J6_SEQ_PLAYING || (s->seq_state == J6_SEQ_STANDBY && blink));
    fm1_led_key(BTN_REC, s->recording || s->seq_state == J6_SEQ_STANDBY);
    fm1_led_key(BTN_SEL, s->chord);
    fm1_led_key(BTN_ARP, s->style_on);
    fm1_led_key(BTN_ENV, s->hold);
    fm1_led_key(BTN_HOME, ui.page == PG_PLAY);
    fm1_led_key(BTN_SEQ, ui.page == PG_SEQ || ui.page == PG_PATTERNS);
    fm1_led_key(BTN_LFO, ui.page == PG_SOUNDS);
    fm1_led_key(BTN_FX, ui.page == PG_FX);
    fm1_led_key(BTN_EDIT, ui.page == PG_SETTINGS);
    fm1_led_key(BTN_GLO, ui.glo);
}

static void splash(void)
{
    cv_oy = 0;
    lcd_fill(0, 0, 240, 240, H_BG);
    cv_begin(240, 90, H_BG);
    h_text_c(120, 10, "HERA", H_ACC, H_BG, 4u);
    h_text_c(120, 76, "chord groovebox", H_DIM, H_BG, 1u);
    cv_blit(0, 60);
    cv_begin(240, 20, H_BG);
    h_text_c(120, 2, "v" APP_VER, H_LINE, H_BG, 1u);
    cv_blit(0, 200);
}

static uint32_t t_ui, t_led, t_splash;

static void hera_setup(void)
{
    j6_engine_init(hera_fx, J6_FX_MEM_LEN, &hera_store);
    ui.page = PG_PLAY;
    ui.chord_key = 0xFFu;
    ui.set_cursor = 1;
    splash();
    t_splash = fm1_ms + 1200u;
}

/* one pass of the main loop (the browser build calls it from requestAnimationFrame) */
static void hera_step(void)
{
    j6_status_t s;
    uint8_t a, b, c;
    j6_engine_status(&s);
    buttons(&s);
    keys(&s);
    encoders(&s);
    while (j6_engine_cmd_space() && midi_get(&a, &b, &c))
        j6_engine_midi_in(J6_PORT_USB, a, b, c);
    while (j6_engine_midi_out(&a, &b, &c))
        midi_send(a, b, c);
    j6_engine_poll();
    if ((int32_t)(fm1_ms - t_led) >= 20) {
        t_led = fm1_ms;
        leds(&s);
    }
    if ((int32_t)(fm1_ms - t_splash) >= 0 && (int32_t)(fm1_ms - t_ui) >= 33) {
        if (t_splash) {
            t_splash = 0;
            ui.force = 1;
        }
        t_ui = fm1_ms;
        ui_draw();
    }
}

#ifndef DINGHY_WEB
static void app_main(void)
{
    hera_setup();
    for (;;) {
        platform_poll();
        hera_step();
    }
}
#endif

#ifdef DINGHY_WEB
/* the browser build's extra exports (web/src/web_platform.c has the panel and the audio) */
uint8_t *web_store(void) { return &host_flash[0][0]; }
uint32_t web_store_len(void) { return (uint32_t)sizeof host_flash; }
void web_reload(void)                            /* after the page restored web_store(): load it */
{
    j6_engine_init(hera_fx, J6_FX_MEM_LEN, &hera_store);
    ui.force = 1;
}
static j6_status_t web_st;
const j6_status_t *web_status(void)
{
    j6_engine_status(&web_st);
    return &web_st;
}
const char *web_style_name(uint32_t s) { return HERA_STYLE_NAME[s <= 9u ? s : 0]; }
const char *web_var_name(uint32_t s, uint32_t v) { return HERA_VAR_NAME[s <= 9u ? s : 0][v <= 12u ? v : 0]; }
const char *web_sound_name(uint32_t i) { return j6_sounds[i % J6_SOUNDS].name; }
const char *web_param_label(uint32_t id)         /* the settings page's words, "" if not listed there */
{
    uint32_t i;
    for (i = 0; i < SET_N; i++)
        if (SET_ITEMS[i].id == (int16_t)id) return SET_ITEMS[i].label;
    return "";
}
static char web_vbuf[32];
const char *web_param_value(uint32_t id, int32_t v)
{
    h_value(web_vbuf, id, v);
    return web_vbuf;
}
uint32_t web_page(void) { return ui.page; }
static uint8_t web_scratch_buf[64];
uint8_t *web_scratch(void) { return web_scratch_buf; }   /* out buffer for j6_chord_raw and friends */
uint32_t web_status_size(void) { return (uint32_t)sizeof(j6_status_t); }
#endif
