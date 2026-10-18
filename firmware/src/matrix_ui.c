/* SPDX-License-Identifier: GPL-3.0-only */
/* MORPHEUS: 240x240 LCD Reason Matrix UI Renderer */
#include "matrix_ui.h"
#include "matrix_core.h"
#include "matrix_synth.h"

/* Colors (RGB565) - High Contrast Matrix CRT Theme with Dual-Phosphor Pop */
#define C_BG           RGB(0, 0, 0)        /* Pure CRT Pitch Black Void */
#define C_TOPBAR       RGB(8, 14, 10)      /* Deep Graphite Bar */
#define C_DIVIDER      RGB(22, 54, 32)     /* Crisp Matrix Divider */
#define C_GRID_BG      RGB(2, 6, 3)        /* Deep CRT Grid Glass */
#define C_GRID_LINE    RGB(10, 24, 14)     /* Faint Step Guide */
#define C_BEAT_LINE    RGB(32, 85, 46)     /* High-Contrast Beat Line (Every 4th step) */
#define C_RED_LED      RGB(0, 255, 102)    /* Primary Electric Phosphor Green */
#define C_ORANGE_LED   RGB(255, 185, 30)   /* Warm Phosphor Amber Gold (Accents & Velocity) */
#define C_BLUE_LED     RGB(0, 240, 255)    /* Vibrant Electric Cyan (Curve Mode) */
#define C_PLAY_HL      RGB(255, 255, 255)  /* Incandescent White Phosphor Flash */
#define C_GREEN_LED    RGB(0, 255, 102)    /* Bright Matrix Green */
#define C_PURPLE_LED   RGB(255, 185, 30)   /* Amber Gold Badge */
#define C_TEXT_WHITE   RGB(240, 255, 245)  /* Crisp Phosphor White Text */
#define C_TEXT_DIM     RGB(70, 185, 100)   /* Readable Crisp Phosphor Green Text */
#define C_KEY_WHITE    RGB(230, 250, 235)  /* Bright Phosphor White Piano Key */
#define C_KEY_BLACK    RGB(6, 12, 8)       /* Deep Charcoal Black Piano Key */

static const char *const SOUND_NAMES[4] = {
    "303", "SUB", "FM", "MUTE"
};

/* Piano keyboard black/white key map (0 = B, 1 = A#, ..., 11 = C) */
static const uint8_t PIANO_IS_BLACK[12] = {
    0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0
};

#define NUM_FX_MENU_ITEMS 10
static const char *const FX_MENU_ITEMS[NUM_FX_MENU_ITEMS] = {
    "1. UNDO LAST EDIT",
    "2. REDO EDIT",
    "3. SHIFT LEFT",
    "4. SHIFT RIGHT",
    "5. TRANSPOSE +1",
    "6. TRANSPOSE -1",
    "7. ALTER PATTERN",
    "8. RANDOMIZE PAT",
    "9. CLEAR PATTERN",
    "10. EXIT MENU"
};

/* Transient and held status notification state */
static char g_transient_msg[32] = {0};
static uint32_t g_transient_expire_ms = 0;
static char g_held_msg[32] = {0};

void matrix_ui_notify(const char *msg)
{
    str_cpy(g_transient_msg, msg, sizeof(g_transient_msg));
    g_transient_expire_ms = fm1_ms + 1800u;
}

void matrix_ui_set_held_status(const char *msg)
{
    str_cpy(g_held_msg, msg, sizeof(g_held_msg));
}

void matrix_ui_clear_held_status(void)
{
    g_held_msg[0] = '\0';
}

void matrix_ui_init(void)
{
    lcd_fill(0, 0, 240, 240, C_BG);
}

static void fmt_num(char *buf, uint32_t val)
{
    buf[0] = '0' + (val / 10u) % 10u;
    buf[1] = '0' + (val % 10u);
    buf[2] = '\0';
}

void matrix_ui_draw(void)
{
    uint8_t trk = g_matrix.active_track;
    matrix_pattern_t *pat = &g_matrix.patterns[trk][g_matrix.cur_bank[trk] * PATS_PER_BANK + g_matrix.cur_pat[trk]];
    uint8_t step_offset = g_matrix.view_page * 16u;
    uint8_t cur_play_step = g_matrix.cur_step[trk];
    uint8_t is_running = g_matrix.is_running;
    char str[32];

    /* =========================================================================
     * STRIP 1: Top Bar (y: 0..25, h: 26)
     * ========================================================================= */
    cv_begin(240, 26, C_TOPBAR);
    cv_rect(0, 25, 240, 1, C_DIVIDER);

    /* Track badge: [T1] */
    cv_rect(4, 4, 20, 17, C_RED_LED);
    str[0] = 'T'; str[1] = '1' + trk; str[2] = '\0';
    cv_text(7, 5, &FONT_S, str, C_BLACK);

    /* Track mini lights (T1..T4) */
    uint32_t t;
    for (t = 0; t < NUM_TRACKS; t++) {
        uint16_t c = (t == trk) ? C_RED_LED : (g_matrix.patterns[t][g_matrix.cur_bank[t] * PATS_PER_BANK + g_matrix.cur_pat[t]].mute ? C_DIVIDER : C_TEXT_DIM);
        cv_rect(28 + t * 6, 10, 4, 5, c);
    }

    /* Bank/Pattern badge: "A1" */
    str[0] = 'A' + g_matrix.cur_bank[trk];
    str[1] = '1' + g_matrix.cur_pat[trk];
    str[2] = '\0';
    cv_rect(54, 4, 18, 17, C_DIVIDER);
    cv_text(56, 5, &FONT_S, str, C_TEXT_WHITE);

    /* Sound Engine Badge */
    const char *snd_name = SOUND_NAMES[g_matrix.sound_engine % 4];
    uint16_t snd_c = (g_matrix.sound_engine == SOUND_MUTED) ? C_TEXT_DIM : C_PURPLE_LED;
    cv_rect(76, 5, 34, 15, snd_c);
    cv_text(79, 5, &FONT_S, snd_name, (g_matrix.sound_engine == SOUND_MUTED) ? C_BLACK : C_TEXT_WHITE);

    /* Mode Badge: KEYS vs CURV */
    uint16_t mode_c = (g_matrix.view_mode == MODE_KEYS) ? C_GREEN_LED : C_BLUE_LED;
    cv_rect(114, 5, 42, 15, mode_c);
    cv_text(118, 5, &FONT_S, (g_matrix.view_mode == MODE_KEYS) ? "KEYS" : "CURV", C_BLACK);

    /* MIDI Channel: e.g. "CH01" */
    str[0] = 'C'; str[1] = 'H';
    fmt_num(str + 2, pat->midi_ch);
    cv_text(160, 5, &FONT_S, str, C_TEXT_DIM);

    /* Run / Stop status */
    if (is_running) {
        cv_rect(202, 7, 7, 10, C_GREEN_LED);
        cv_text(212, 5, &FONT_S, "RUN", C_GREEN_LED);
    } else {
        cv_rect(202, 7, 7, 10, C_TEXT_DIM);
        cv_text(212, 5, &FONT_S, "STOP", C_TEXT_DIM);
    }

    cv_blit(0, 0);

    /* =========================================================================
     * STRIP 2: Upper Matrix Note Grid (y: 26..148, h: 122)
     * ========================================================================= */
    cv_begin(240, 122, C_BG);

    const int32_t grid_x = 22;
    const int32_t grid_w = 216;
    const int32_t row_h = 10;
    const int32_t col_w = 13;

    /* Piano Keyboard Rail on Left (x: 0..20) */
    cv_rect(0, 0, grid_x, 122, C_TOPBAR);
    cv_rect(grid_x - 1, 0, 1, 122, C_DIVIDER);

    uint32_t r;
    for (r = 0; r < 12; r++) {
        int32_t ky = r * row_h;
        if (PIANO_IS_BLACK[r]) {
            /* Black key */
            cv_rect(0, ky, 13, row_h - 1, C_KEY_BLACK);
            cv_rect(13, ky, grid_x - 14, row_h - 1, C_TOPBAR);
        } else {
            /* White key */
            cv_rect(0, ky, grid_x - 2, row_h - 1, C_KEY_WHITE);
        }
    }
    /* Piano key rail is clean black & white keys */

    /* Grid Area */
    cv_rect(grid_x, 0, grid_w, 120, C_GRID_BG);

    /* Horizontal grid lines */
    for (r = 0; r <= 12; r++) {
        cv_line(grid_x, r * row_h, grid_x + grid_w, r * row_h, C_GRID_LINE);
    }

    /* Vertical grid lines */
    uint32_t c;
    for (c = 0; c <= 16; c++) {
        uint16_t lc = (c % 4 == 0) ? C_BEAT_LINE : C_GRID_LINE;
        cv_line(grid_x + c * col_w, 0, grid_x + c * col_w, 120, lc);
    }

    /* Steps Rendering */
    uint32_t s_idx;
    for (s_idx = 0; s_idx < 16; s_idx++) {
        uint8_t actual_step = step_offset + s_idx;
        if (actual_step >= pat->length) {
            cv_rect(grid_x + s_idx * col_w + 1, 1, col_w - 1, 118, RGB(4, 9, 5));
            continue;
        }

        matrix_step_t *st = &pat->steps[actual_step];
        int is_current_play = (is_running && actual_step == cur_play_step);
        int is_cursor = (actual_step == g_matrix.active_step);

        /* Cursor indicator */
        if (is_cursor) {
            cv_rect(grid_x + s_idx * col_w, 0, col_w, 1, C_WHITE);
            cv_rect(grid_x + s_idx * col_w, 119, col_w, 1, C_WHITE);
        }

        /* Playhead cursor across the top */
        if (is_current_play) {
            cv_rect(grid_x + s_idx * col_w + 1, 0, col_w - 2, 2, C_RED_LED);
        }

        if (g_matrix.view_mode == MODE_KEYS) {
            /* Note pitch: C3=48 -> note_oct = 48/12 - 1 = 3 */
            if (st->gate > 0) {
                int32_t note_oct = ((int32_t)st->note / 12) - 1;
                int32_t note_in_oct = st->note % 12;
                int32_t view_oct = (int32_t)g_matrix.view_octave;

                if (note_oct == view_oct) {
                    int32_t note_row = 11 - note_in_oct;
                    int32_t cell_y = note_row * row_h + 1;
                    uint16_t led_col = is_current_play ? C_PLAY_HL : ((st->gate > 105) ? C_ORANGE_LED : C_RED_LED);
                    cv_rect(grid_x + s_idx * col_w + 2, cell_y + 1, col_w - 3, row_h - 2, led_col);
                } else if (note_oct > view_oct) {
                    cv_rect(grid_x + s_idx * col_w + 4, 1, col_w - 7, 3, C_ORANGE_LED);
                } else {
                    cv_rect(grid_x + s_idx * col_w + 4, 116, col_w - 7, 3, C_ORANGE_LED);
                }
            }
        } else {
            /* Curve CV vertical level bar */
            int32_t bar_h = ((int32_t)st->curve * 116) / 127;
            if (bar_h > 116) bar_h = 116;
            int32_t bar_y = 120 - bar_h;
            uint16_t bar_col = is_current_play ? C_PLAY_HL : C_BLUE_LED;
            cv_rect(grid_x + s_idx * col_w + 2, bar_y, col_w - 4, bar_h, bar_col);
        }
    }

    cv_blit(0, 26);

    /* =========================================================================
     * STRIP 3: Lower Gate / Velocity Strips (y: 148..208, h: 60)
     * ========================================================================= */
    cv_begin(240, 60, C_BG);
    cv_rect(0, 0, 240, 1, C_DIVIDER);

    /* Gate rail label */
    cv_rect(0, 1, grid_x, 59, C_TOPBAR);
    cv_rect(grid_x - 1, 1, 1, 59, C_DIVIDER);
    cv_text(1, 16, &FONT_S, "GATE", C_TEXT_DIM);
    cv_text(3, 32, &FONT_S, "VEL", C_TEXT_DIM);

    /* Velocity Bars */
    for (s_idx = 0; s_idx < 16; s_idx++) {
        uint8_t actual_step = step_offset + s_idx;
        if (actual_step >= pat->length) continue;

        matrix_step_t *st = &pat->steps[actual_step];
        int is_current_play = (is_running && actual_step == cur_play_step);
        int32_t bar_x = grid_x + s_idx * col_w + 2;

        if (st->gate > 0) {
            int32_t bar_h = ((int32_t)st->gate * 50) / 127;
            if (bar_h < 4) bar_h = 4;
            if (bar_h > 52) bar_h = 52;
            int32_t bar_y = 56 - bar_h;
            uint16_t g_col = is_current_play ? C_PLAY_HL : ((st->gate > 105) ? C_ORANGE_LED : C_RED_LED);

            cv_rect(bar_x, bar_y, col_w - 4, bar_h, g_col);

            /* Tie bridge to next step */
            if (st->tie && s_idx < 15) {
                cv_rect(bar_x + (col_w - 4), bar_y + bar_h - 4, 4, 4, C_ORANGE_LED);
            }
        } else {
            /* Rest marker */
            cv_rect(bar_x + 2, 54, col_w - 8, 2, C_DIVIDER);
        }
    }

    cv_blit(0, 148);

    /* =========================================================================
     * STRIP 4: Bottom Status Bar (y: 208..240, h: 32)
     * ========================================================================= */
    cv_begin(240, 32, C_TOPBAR);
    cv_rect(0, 0, 240, 1, C_DIVIDER);

    const char *status_to_show = 0;
    uint16_t status_color = C_TEXT_WHITE;
    uint16_t banner_bg = C_TOPBAR;
    uint16_t border_col = C_DIVIDER;

    if (g_held_msg[0] != '\0') {
        status_to_show = g_held_msg;
        status_color = C_ORANGE_LED;
        banner_bg = RGB(14, 24, 16);
        border_col = C_ORANGE_LED;
    } else if (g_transient_msg[0] != '\0' && fm1_ms < g_transient_expire_ms) {
        status_to_show = g_transient_msg;
        status_color = C_PLAY_HL;
        banner_bg = RGB(10, 26, 14);
        border_col = C_GREEN_LED;
    }

    if (status_to_show) {
        /* High-contrast transient notification banner */
        cv_rect(3, 3, 234, 26, banner_bg);
        cv_rect(3, 3, 234, 1, border_col);
        cv_rect(3, 28, 234, 1, border_col);
        cv_rect(3, 3, 1, 26, border_col);
        cv_rect(236, 3, 1, 26, border_col);
        cv_text(8, 8, &FONT_S, status_to_show, status_color);
    } else {
        /* Default status display with high-contrast text and badges */
        str[0] = 'P'; str[1] = '1' + g_matrix.view_page;
        str[2] = ' '; str[3] = 'O'; str[4] = 'C'; str[5] = 'T';
        str[6] = ':'; str[7] = '0' + g_matrix.view_octave; str[8] = '\0';
        cv_text(4, 8, &FONT_S, str, C_ORANGE_LED);

        fmt_int(str, g_matrix.bpm);
        {
            unsigned slen = str_len(str);
            str[slen] = 'B';
            str[slen + 1] = 'P';
            str[slen + 2] = 'M';
            str[slen + 3] = '\0';
        }
        cv_text(78, 8, &FONT_S, str, C_TEXT_WHITE);

        cv_text(150, 8, &FONT_S, "K1-4:EDIT", C_TEXT_DIM);
    }

    cv_blit(0, 208);

    /* =========================================================================
     * MODAL MENUS
     * ========================================================================= */
    if (g_matrix.menu_mode == MENU_FX) {
        cv_begin(190, 144, C_TOPBAR);
        cv_rect(0, 0, 190, 1, C_RED_LED);
        cv_rect(0, 0, 1, 144, C_RED_LED);
        cv_rect(189, 0, 1, 144, C_RED_LED);
        cv_rect(0, 143, 190, 1, C_RED_LED);

        cv_text(30, 4, &FONT_S, "-- PATTERN TOOLS --", C_ORANGE_LED);
        uint32_t m;
        for (m = 0; m < NUM_FX_MENU_ITEMS; m++) {
            uint16_t tc = (g_matrix.menu_sel == (int8_t)m) ? C_PLAY_HL : C_TEXT_DIM;
            if (g_matrix.menu_sel == (int8_t)m) {
                cv_rect(6, 18 + m * 12, 178, 11, C_DIVIDER);
            }
            cv_text(10, 18 + m * 12, &FONT_S, FX_MENU_ITEMS[m], tc);
        }
        cv_blit(25, 48);
    }
}
