/* SPDX-License-Identifier: GPL-3.0-only */
/* MORPHEUS: Reason Matrix Interface with Dual-Phosphor CRT Theme & Drum Matrix View */
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

static const char *const ENGINE_NAMES[NUM_TRACK_TYPES] = {
    "303A", "303B", "D808", "D909", "BRK", "FM", "SUB", "MIDI"
};

static const char *const DRUM_LABELS[12] = {
    "BD ", "SD ", "LT ", "MT ", "HT ", "RS ", "CP ", "CB ", "CY ", "OH ", "CH ", "ACC"
};

static const uint8_t PIANO_IS_BLACK[12] = {
    0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0
};

static const char *const FX_MENU_ITEMS[10] = {
    "1. UNDO EDIT",
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
static uint32_t g_splash_until_ms = 0;

/* 32x32 8-Bit White Rabbit Matrix Icon (128 bytes) */
static const uint32_t RABBIT_BITS[32] = {
    0x00000000u, 0x00781E00u, 0x00FC3F00u, 0x00CC3300u,
    0x00CC3300u, 0x00CC3300u, 0x00CC3300u, 0x00CC3300u,
    0x00781E00u, 0x003FFC00u, 0x007FFE00u, 0x00FFFF00u,
    0x01F3FF80u, 0x03F3FF80u, 0x03FFFF00u, 0x01FFFE00u,
    0x00FFFC00u, 0x01FFFC00u, 0x03FFFE00u, 0x07FFFF00u,
    0x0FFFFF8Eu, 0x1FFFFFDFu, 0x3FFFFFFFu, 0x3FFFFFFEu,
    0x7FFFFFFCu, 0x7FFFFFF8u, 0x7F9FFFF0u, 0x7F0FFFC0u,
    0x7F0FFFC0u, 0x3E07FF80u, 0x1C03FF00u, 0x00000000u
};

void matrix_ui_dismiss_splash(void)
{
    g_splash_until_ms = 0;
}

uint8_t matrix_ui_is_splash_active(void)
{
    return (fm1_ms < g_splash_until_ms);
}

static void matrix_ui_draw_splash(void)
{
    uint32_t ry, rx;
    const int32_t ox = 72;
    const int32_t oy = 16;

    /* -------------------------------------------------------------------------
     * STRIP A: Top Half (y: 0..119, h: 120) - 8-Bit White Rabbit & Phosphor Glow
     * ------------------------------------------------------------------------- */
    cv_begin(240, 120, C_BG);

    /* Double Matrix Phosphor Border */
    cv_rect(2, 2, 236, 1, C_GREEN_LED);
    cv_rect(2, 2, 1, 118, C_GREEN_LED);
    cv_rect(237, 2, 1, 118, C_GREEN_LED);
    cv_rect(4, 4, 232, 1, C_DIVIDER);
    cv_rect(4, 4, 1, 116, C_DIVIDER);
    cv_rect(235, 4, 1, 116, C_DIVIDER);

    /* Phosphor glow halo behind rabbit */
    for (ry = 0; ry < 32; ry++) {
        uint32_t bits = RABBIT_BITS[ry];
        if (!bits) continue;
        for (rx = 0; rx < 32; rx++) {
            if (bits & (1u << (31 - rx))) {
                cv_rect(ox + (int32_t)rx * 3 - 2, oy + (int32_t)ry * 3 - 2, 7, 7, RGB(0, 50, 20));
            }
        }
    }
    for (ry = 0; ry < 32; ry++) {
        uint32_t bits = RABBIT_BITS[ry];
        if (!bits) continue;
        for (rx = 0; rx < 32; rx++) {
            if (bits & (1u << (31 - rx))) {
                cv_rect(ox + (int32_t)rx * 3 - 1, oy + (int32_t)ry * 3 - 1, 5, 5, RGB(0, 110, 44));
            }
        }
    }

    /* 8-bit White Rabbit Body */
    for (ry = 0; ry < 32; ry++) {
        uint32_t bits = RABBIT_BITS[ry];
        if (!bits) continue;
        for (rx = 0; rx < 32; rx++) {
            if (bits & (1u << (31 - rx))) {
                cv_rect(ox + (int32_t)rx * 3, oy + (int32_t)ry * 3, 3, 3, C_TEXT_WHITE);
            }
        }
    }

    /* Cyan Cybernetic Eye (at rx=11, ry=12) */
    cv_rect(ox + 11 * 3, oy + 12 * 3, 3, 3, C_BLUE_LED);
    cv_pset(ox + 11 * 3 + 1, oy + 12 * 3 + 1, C_TEXT_WHITE);

    cv_blit(0, 0);

    /* -------------------------------------------------------------------------
     * STRIP B: Bottom Half (y: 120..239, h: 120) - MORPHEUS & System Info
     * ------------------------------------------------------------------------- */
    cv_begin(240, 120, C_BG);

    /* Double Matrix Phosphor Border */
    cv_rect(2, 0, 1, 118, C_GREEN_LED);
    cv_rect(237, 0, 1, 118, C_GREEN_LED);
    cv_rect(2, 117, 236, 1, C_GREEN_LED);
    cv_rect(4, 0, 1, 116, C_DIVIDER);
    cv_rect(235, 0, 1, 116, C_DIVIDER);
    cv_rect(4, 115, 232, 1, C_DIVIDER);

    /* "MORPHEUS" in 32px Font (FONT_L width = 8 * 16 = 128px, centered at x=56) */
    /* Neon green glow shadow */
    cv_text(55, 8, &FONT_L, "MORPHEUS", C_GREEN_LED);
    cv_text(57, 8, &FONT_L, "MORPHEUS", C_GREEN_LED);
    cv_text(56, 7, &FONT_L, "MORPHEUS", C_GREEN_LED);
    cv_text(56, 9, &FONT_L, "MORPHEUS", C_GREEN_LED);
    /* White core */
    cv_text(56, 8, &FONT_L, "MORPHEUS", C_TEXT_WHITE);

    /* Subtitles in 16px Font (FONT_S width = N * 8) */
    /* "FOLLOW THE RABBIT" (17 chars * 8 = 136px, centered at x=52) */
    cv_text(52, 46, &FONT_S, "FOLLOW THE RABBIT", C_GREEN_LED);

    /* "8-TRACK GROOVEBOX" (17 chars * 8 = 136px, centered at x=52) */
    cv_text(52, 66, &FONT_S, "8-TRACK GROOVEBOX", C_ORANGE_LED);

    /* "FM-1 OPEN // ID: 8000013" (24 chars * 8 = 192px, centered at x=24) */
    cv_text(24, 86, &FONT_S, "FM-1 OPEN // ID: 8000013", C_BLUE_LED);

    /* Status prompt: "[ READY // 48kHz DSP ]" (22 chars * 8 = 176px, centered at x=32) */
    cv_text(32, 102, &FONT_S, "[ READY // 48kHz DSP ]", C_TEXT_DIM);

    cv_blit(0, 120);
}

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
    g_splash_until_ms = fm1_ms + 1800u;
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
    if (fm1_ms < g_splash_until_ms) {
        matrix_ui_draw_splash();
        return;
    }

    uint8_t trk = g_matrix.active_track;
    matrix_pattern_t *pat = &g_matrix.patterns[trk][g_matrix.cur_bank[trk] * PATS_PER_BANK + g_matrix.cur_pat[trk]];
    uint8_t step_offset = g_matrix.view_page * 16u;
    uint8_t cur_play_step = g_matrix.cur_step[trk];
    uint8_t is_running = g_matrix.is_running;
    uint8_t engine = g_matrix.track_types[trk];
    int is_drum_track = (engine == TRACK_TYPE_808 || engine == TRACK_TYPE_909);
    char str[32];

    /* =========================================================================
     * STRIP 1: Top Bar (y: 0..25, h: 26)
     * ========================================================================= */
    cv_begin(240, 26, C_TOPBAR);
    cv_rect(0, 25, 240, 1, C_DIVIDER);

    /* Track badge: [T1]..[T8] */
    cv_rect(4, 4, 20, 17, C_RED_LED);
    str[0] = 'T'; str[1] = '1' + trk; str[2] = '\0';
    cv_text(7, 5, &FONT_S, str, C_BLACK);

    /* 8 Track Indicator Dots (T1..T8) */
    uint32_t t;
    for (t = 0; t < NUM_TRACKS; t++) {
        uint16_t c = (t == trk) ? C_RED_LED : (g_matrix.patterns[t][g_matrix.cur_bank[t] * PATS_PER_BANK + g_matrix.cur_pat[t]].mute ? C_DIVIDER : C_TEXT_DIM);
        cv_rect(27 + t * 4, 10, 3, 5, c);
    }

    /* Bank/Pattern badge: "A1".."B8" */
    str[0] = 'A' + g_matrix.cur_bank[trk];
    str[1] = '1' + g_matrix.cur_pat[trk];
    str[2] = '\0';
    cv_rect(61, 4, 18, 17, C_DIVIDER);
    cv_text(63, 5, &FONT_S, str, C_TEXT_WHITE);

    /* Sound Engine Badge */
    const char *snd_name = (engine < NUM_TRACK_TYPES) ? ENGINE_NAMES[engine] : "UNKN";
    uint16_t snd_c = (engine == TRACK_TYPE_MIDI) ? C_TEXT_DIM : C_PURPLE_LED;
    cv_rect(82, 5, 34, 15, snd_c);
    cv_text(84, 5, &FONT_S, snd_name, C_BLACK);

    /* Mode Badge: KEYS vs CURV */
    uint16_t mode_c = (g_matrix.view_mode == MODE_KEYS) ? C_GREEN_LED : C_BLUE_LED;
    cv_rect(120, 5, 38, 15, mode_c);
    cv_text(122, 5, &FONT_S, (g_matrix.view_mode == MODE_KEYS) ? (is_drum_track ? "DRUM" : "KEYS") : "CURV", C_BLACK);

    /* MIDI Channel: e.g. "CH01" */
    str[0] = 'C'; str[1] = 'H';
    fmt_num(str + 2, pat->midi_ch);
    cv_text(162, 5, &FONT_S, str, C_TEXT_DIM);

    /* Run / Stop status */
    if (is_running) {
        cv_rect(204, 7, 7, 10, C_GREEN_LED);
        cv_text(214, 5, &FONT_S, "RUN", C_GREEN_LED);
    } else {
        cv_rect(204, 7, 7, 10, C_TEXT_DIM);
        cv_text(214, 5, &FONT_S, "STOP", C_TEXT_DIM);
    }

    cv_blit(0, 0);

    /* =========================================================================
     * STRIP 2: Upper Matrix Note/Drum Grid (y: 26..148, h: 122)
     * ========================================================================= */
    cv_begin(240, 122, C_BG);

    const int32_t grid_x = 24;
    const int32_t grid_w = 214;
    const int32_t row_h = 10;
    const int32_t col_w = 13;

    /* Left Sidebar: Piano Keys or 12 Drum Lane Labels */
    cv_rect(0, 0, grid_x, 122, C_TOPBAR);
    cv_rect(grid_x - 1, 0, 1, 122, C_DIVIDER);

    uint32_t r;
    if (is_drum_track) {
        /* Drum Lane Column */
        uint8_t focused_lane = g_matrix.drum_lane_sel[trk];
        for (r = 0; r < 12; r++) {
            uint8_t lane_idx = 11 - r; /* Row 0 = ACC, Row 11 = BD */
            int32_t ky = r * row_h;
            int is_focused = (lane_idx == focused_lane);
            if (is_focused) {
                cv_rect(0, ky, grid_x - 1, row_h - 1, RGB(18, 48, 26));
            }
            uint16_t lbl_col = is_focused ? C_ORANGE_LED : C_TEXT_DIM;
            cv_text(2, ky + 1, &FONT_S, DRUM_LABELS[lane_idx], lbl_col);
        }
    } else {
        /* Piano Keyboard Column */
        for (r = 0; r < 12; r++) {
            int32_t ky = r * row_h;
            if (PIANO_IS_BLACK[r]) {
                cv_rect(0, ky, 14, row_h - 1, C_KEY_BLACK);
                cv_rect(14, ky, grid_x - 15, row_h - 1, C_TOPBAR);
            } else {
                cv_rect(0, ky, grid_x - 2, row_h - 1, C_KEY_WHITE);
            }
        }
    }

    /* Grid Glass Background */
    cv_rect(grid_x, 0, grid_w, 120, C_GRID_BG);

    /* Horizontal grid lines */
    for (r = 0; r <= 12; r++) {
        cv_line(grid_x, r * row_h, grid_x + grid_w, r * row_h, C_GRID_LINE);
    }

    /* Vertical grid lines (Beat lines highlighted every 4th step) */
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
            cv_rect(grid_x + s_idx * col_w + 1, 1, col_w - 1, 118, RGB(4, 8, 4));
            continue;
        }

        matrix_step_t *st = &pat->steps[actual_step];
        int is_current_play = (is_running && actual_step == cur_play_step);
        int is_cursor = (actual_step == g_matrix.active_step);

        /* Cursor border */
        if (is_cursor) {
            cv_rect(grid_x + s_idx * col_w, 0, col_w, 1, C_WHITE);
            cv_rect(grid_x + s_idx * col_w, 119, col_w, 1, C_WHITE);
        }

        /* Playhead cursor across the top */
        if (is_current_play) {
            cv_rect(grid_x + s_idx * col_w + 1, 0, col_w - 2, 2, C_PLAY_HL);
        }

        if (g_matrix.view_mode == MODE_KEYS) {
            if (is_drum_track) {
                /* Render 12 Drum Lane hits */
                uint16_t dmask = st->drums;
                uint8_t lane;
                for (lane = 0; lane < 12; lane++) {
                    if (dmask & (1u << lane)) {
                        int32_t drum_row = 11 - lane;
                        int32_t cell_y = drum_row * row_h + 1;
                        uint16_t led_col = is_current_play ? C_PLAY_HL : ((lane == DRUM_LANE_ACC || st->gate >= 105) ? C_ORANGE_LED : C_RED_LED);
                        cv_rect(grid_x + s_idx * col_w + 2, cell_y + 1, col_w - 3, row_h - 2, led_col);
                    }
                }
            } else {
                /* Render Pitched Synth Note */
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
            }
        } else {
            /* Curve CV vertical level bar in Electric Cyan */
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

        if (st->gate > 0 || (is_drum_track && st->drums > 0)) {
            uint8_t effective_gate = st->gate > 0 ? st->gate : 100;
            int32_t bar_h = ((int32_t)effective_gate * 50) / 127;
            if (bar_h < 4) bar_h = 4;
            if (bar_h > 52) bar_h = 52;
            int32_t bar_y = 56 - bar_h;
            uint16_t g_col = is_current_play ? C_PLAY_HL : ((effective_gate > 105) ? C_ORANGE_LED : C_RED_LED);

            cv_rect(bar_x, bar_y, col_w - 4, bar_h, g_col);

            /* Tie / Slide bridge to next step */
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
        if (is_drum_track) {
            str[0] = 'P'; str[1] = '1' + g_matrix.view_page;
            str[2] = ' '; str[3] = '\0';
            str_cpy(str + 3, DRUM_LABELS[g_matrix.drum_lane_sel[trk]], sizeof(str) - 3);
            cv_text(4, 8, &FONT_S, str, C_ORANGE_LED);
        } else {
            str[0] = 'P'; str[1] = '1' + g_matrix.view_page;
            str[2] = ' '; str[3] = 'O'; str[4] = 'C'; str[5] = 'T';
            str[6] = ':'; str[7] = '0' + g_matrix.view_octave; str[8] = '\0';
            cv_text(4, 8, &FONT_S, str, C_ORANGE_LED);
        }

        fmt_int(str, g_matrix.bpm);
        {
            unsigned slen = str_len(str);
            str[slen] = 'B';
            str[slen + 1] = 'P';
            str[slen + 2] = 'M';
            str[slen + 3] = '\0';
        }
        cv_text(86, 8, &FONT_S, str, C_TEXT_WHITE);

        cv_text(152, 8, &FONT_S, "K1-4:EDIT", C_TEXT_DIM);
    }

    cv_blit(0, 208);

    /* Render Menu Overlay if active */
    if (g_matrix.menu_mode == MENU_FX) {
        cv_begin(180, 160, RGB(10, 22, 14));
        cv_rect(0, 0, 180, 160, RGB(10, 22, 14));
        cv_rect(0, 0, 180, 1, C_ORANGE_LED);
        cv_rect(0, 159, 180, 1, C_ORANGE_LED);
        cv_rect(0, 0, 1, 160, C_ORANGE_LED);
        cv_rect(179, 0, 1, 160, C_ORANGE_LED);

        cv_text(8, 6, &FONT_S, "--- PATTERN TOOLS ---", C_ORANGE_LED);

        uint32_t i;
        for (i = 0; i < 10; i++) {
            int32_t my = 22 + i * 13;
            uint16_t tc = (i == (uint32_t)g_matrix.menu_sel) ? C_BLACK : C_TEXT_WHITE;
            if (i == (uint32_t)g_matrix.menu_sel) {
                cv_rect(6, my - 1, 168, 12, C_GREEN_LED);
            }
            cv_text(8, my, &FONT_S, FX_MENU_ITEMS[i], tc);
        }
        cv_blit(30, 40);
    }
}
