/* SPDX-License-Identifier: GPL-3.0-only */
/* MORPHEUS: 8-Track Reason Matrix Pattern Sequencer & ReBirth Groovebox for M-VAVE FM-1 */

#ifndef APP_NAME
#define APP_NAME "MORPHEUS"
#endif
#ifndef FELUCCA_ID
#define FELUCCA_ID "FM-1_8000013"
#endif

#include "platform.c"
#include "matrix_core.h"
#include "matrix_synth.h"
#include "matrix_ui.h"

/* Include implementations into the single compilation unit */
#include "matrix_core.c"
#include "matrix_synth.c"
#include "matrix_ui.c"

/* ---- Audio Interrupt (44.1 kHz, 256 stereo frames) */
static void app_audio(int32_t *out, uint32_t n)
{
    matrix_synth_render(out, n);
}

/* ---- MIDI In processing */
static void handle_midi_in(void)
{
    uint8_t st, d1, d2;
    while (midi_get(&st, &d1, &d2)) {
        uint8_t cmd = st & 0xF0u;
        uint8_t ch = (st & 0x0Fu) + 1u;

        if (cmd == 0x90u && d2 > 0) {
            /* Note On */
            uint8_t trk = g_matrix.active_track;
            if (ch == g_matrix.patterns[trk][0].midi_ch) {
                int8_t transpose_delta = (int8_t)d1 - 48; /* Relative to C3 */
                matrix_shift_vertical(trk, transpose_delta);
            }
            matrix_step_t live_step;
            live_step.note = d1;
            live_step.gate = d2;
            live_step.tie = 0;
            live_step.curve = 64;
            live_step.drums = (1u << DRUM_LANE_BD);
            matrix_synth_trigger_step(trk, &live_step);
        } else if (cmd == 0x80u || (cmd == 0x90u && d2 == 0)) {
            /* Note Off */
            matrix_synth_note_off(g_matrix.active_track);
        } else if (cmd == 0xC0u) {
            /* Program change: switch pattern */
            uint8_t trk = g_matrix.active_track;
            g_matrix.next_pat[trk] = d1 % PATS_PER_BANK;
            g_matrix.next_bank[trk] = (d1 / PATS_PER_BANK) % NUM_BANKS;
        } else if (st == 0xFAu) {
            /* MIDI Clock Start */
            g_matrix.is_running = 1;
        } else if (st == 0xFCu) {
            /* MIDI Clock Stop */
            g_matrix.is_running = 0;
        }
    }
}

static const uint8_t GM_DRUM_NOTES[12] = {
    36, /* BD - Bass Drum */
    38, /* SD - Snare Drum */
    45, /* LT - Low Tom */
    48, /* MT - Mid Tom */
    50, /* HT - High Tom */
    37, /* RS - Rim Shot */
    39, /* CP - Hand Clap */
    56, /* CB - Cowbell */
    49, /* CY - Crash Cymbal */
    46, /* OH - Open Hi-Hat */
    42, /* CH - Closed Hi-Hat */
    0   /* ACC - Total Accent */
};

static const char *const DRUM_NAMES_FULL[12] = {
    "BASS DRUM (BD)",
    "SNARE DRUM (SD)",
    "LOW TOM (LT)",
    "MID TOM (MT)",
    "HIGH TOM (HT)",
    "RIM SHOT (RS)",
    "HAND CLAP (CP)",
    "COWBELL (CB)",
    "CRASH CYMBAL (CY)",
    "OPEN HI-HAT (OH)",
    "CLOSED HI-HAT (CH)",
    "TOTAL ACCENT (ACC)"
};

/* ---- Sequencer Timing & Step Advancement */
static void step_track(uint8_t trk)
{
    matrix_pattern_t *pat = &g_matrix.patterns[trk][g_matrix.cur_bank[trk] * PATS_PER_BANK + g_matrix.cur_pat[trk]];
    if (pat->mute) return;

    uint8_t step_idx = g_matrix.cur_step[trk];
    matrix_step_t *st = &pat->steps[step_idx];
    uint8_t ch = (pat->midi_ch > 0 && pat->midi_ch <= 16) ? (pat->midi_ch - 1) : trk;
    uint8_t engine = g_matrix.track_types[trk];
    int is_drum_track = (engine == TRACK_TYPE_808 || engine == TRACK_TYPE_909);

    if (is_drum_track) {
        /* Drum Engine: Trigger internal sound and MIDI Out */
        matrix_synth_trigger_step(trk, st);
        uint16_t m = st->drums;
        if (m > 0) {
            uint8_t vel = (m & (1u << DRUM_LANE_ACC)) ? 127 : 100;
            uint8_t l;
            for (l = 0; l < 11; l++) {
                if (m & (1u << l)) {
                    midi_send(0x90u | ch, GM_DRUM_NOTES[l], vel);
                }
            }
        }
    } else {
        /* Pitched Synth Engine: 303 A, 303 B, FM, Sub, External MIDI */
        /* 1. Send Note Off for previous step if not tied */
        if (g_matrix.last_gate[trk] > 0 && !st->tie) {
            midi_send(0x80u | ch, g_matrix.last_note[trk], 0);
            matrix_synth_note_off(trk);
        }

        /* 2. Play current step if gate > 0 */
        if (st->gate > 0) {
            midi_send(0x90u | ch, st->note, st->gate);
            g_matrix.last_note[trk] = st->note;
            g_matrix.last_gate[trk] = st->gate;
            matrix_synth_trigger_step(trk, st);
        } else {
            g_matrix.last_gate[trk] = 0;
        }
    }

    /* 3. Send Curve CV as MIDI CC */
    midi_send(0xB0u | ch, pat->curve_cc, st->curve);

    /* 4. Advance step */
    uint8_t next_s = (step_idx + 1u);
    if (next_s >= pat->length) {
        next_s = 0;
        /* Downbeat: apply queued pattern changes */
        if (g_matrix.next_pat[trk] != g_matrix.cur_pat[trk] || g_matrix.next_bank[trk] != g_matrix.cur_bank[trk]) {
            g_matrix.cur_pat[trk] = g_matrix.next_pat[trk];
            g_matrix.cur_bank[trk] = g_matrix.next_bank[trk];
        }
    }
    g_matrix.cur_step[trk] = next_s;
}

static const char *const NOTE_NAMES_SHORT[12] = {
    "C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"
};

static void notify_knob_val(const char *name, int32_t val, int32_t max_val)
{
    char buf[32];
    str_cpy(buf, name, sizeof(buf));
    unsigned len = str_len(buf);
    buf[len++] = ':';
    buf[len++] = ' ';
    fmt_int(buf + len, val);
    len = str_len(buf);
    if (max_val > 0) {
        buf[len++] = '/';
        fmt_int(buf + len, max_val);
    }
    matrix_ui_notify(buf);
}

static void notify_step_note(uint8_t step_idx, uint8_t note, uint8_t vel)
{
    char buf[32];
    str_cpy(buf, "STEP ", sizeof(buf));
    buf[5] = '0' + ((step_idx + 1u) / 10u) % 10u;
    buf[6] = '0' + ((step_idx + 1u) % 10u);
    buf[7] = ' ';
    buf[8] = '\0';
    if (vel == 0) {
        str_cpy(buf + 8, "[REST]", sizeof(buf) - 8);
    } else {
        uint8_t oct = (note / 12u) - 1u;
        const char *nm = NOTE_NAMES_SHORT[note % 12u];
        buf[8] = nm[0];
        buf[9] = nm[1];
        buf[10] = '0' + (oct % 10u);
        buf[11] = ' ';
        buf[12] = 'V';
        buf[13] = ':';
        fmt_int(buf + 14, vel);
    }
    matrix_ui_notify(buf);
}

static void notify_drum_step(uint8_t step_idx, uint8_t lane, int is_on)
{
    char buf[32];
    str_cpy(buf, "S", sizeof(buf));
    buf[1] = '0' + ((step_idx + 1u) / 10u) % 10u;
    buf[2] = '0' + ((step_idx + 1u) % 10u);
    buf[3] = ' ';
    str_cpy(buf + 4, DRUM_LABELS[lane], sizeof(buf) - 4);
    unsigned len = str_len(buf);
    if (is_on) {
        str_cpy(buf + len, "[TRIGGER ON]", sizeof(buf) - len);
    } else {
        str_cpy(buf + len, "[OFF]", sizeof(buf) - len);
    }
    matrix_ui_notify(buf);
}

static void notify_bpm(uint16_t bpm)
{
    char buf[32];
    str_cpy(buf, "TEMPO: ", sizeof(buf));
    fmt_int(buf + 7, bpm);
    unsigned len = str_len(buf);
    buf[len++] = ' ';
    buf[len++] = 'B';
    buf[len++] = 'P';
    buf[len++] = 'M';
    buf[len] = '\0';
    matrix_ui_notify(buf);
}

/* 16 White Keys mapping on 27-key silicone keyboard (F3..G5) */
static const int8_t WHITE_KEY_TO_STEP[27] = {
     0, -1,  1, -1,  2, -1,  3,  /* 0..6:   F3..B3 */
     4, -1,  5, -1,  6,          /* 7..11:  C4..E4 */
     7, -1,  8, -1,  9, -1, 10,  /* 12..18: F4..B4 */
    11, -1, 12, -1, 13,          /* 19..23: C5..E5 */
    14, -1, 15                  /* 24..26: F5..G5 */
};

/* Hardware LED Manager: Drives Key and Button LEDs */
static void update_hardware_leds(void)
{
    uint32_t k;
    uint8_t cur_t = g_matrix.active_track;
    uint8_t eng = g_matrix.track_types[cur_t];
    int is_drum = (eng == TRACK_TYPE_808 || eng == TRACK_TYPE_909);
    matrix_pattern_t *pat = &g_matrix.patterns[cur_t][g_matrix.cur_bank[cur_t] * PATS_PER_BANK + g_matrix.cur_pat[cur_t]];
    uint8_t page_offset = g_matrix.view_page * 16u;
    uint8_t cur_play_step = g_matrix.cur_step[cur_t];

    /* Note Key LEDs (0..26) */
    for (k = 0; k < 27u; k++) {
        int is_held = (fm1_in.notes & (1u << k)) != 0;
        int is_lit = is_held;

        if (is_drum) {
            int8_t step_idx = WHITE_KEY_TO_STEP[k];
            if (step_idx >= 0) {
                uint8_t actual_step = page_offset + (uint8_t)step_idx;
                if (actual_step < pat->length) {
                    uint8_t lane = g_matrix.drum_lane_sel[cur_t];
                    int has_trig = (pat->steps[actual_step].drums & (1u << lane)) != 0;
                    int is_playhead = (g_matrix.is_running && actual_step == cur_play_step);
                    if (has_trig || is_playhead) {
                        is_lit = 1;
                    }
                }
            }
        } else {
            /* Pitched synth track: light up key if it matches sounding note */
            uint8_t midi_note = 53u + (uint8_t)k;
            if (g_matrix.last_gate[cur_t] > 0 && g_matrix.last_note[cur_t] == midi_note) {
                is_lit = 1;
            }
        }

        fm1_led_key(KEY_LED(k), is_lit);
    }

    /* Button LEDs */
    fm1_led_key(BTN_PLAY, g_matrix.is_running);
    fm1_led_key(BTN_REC, g_matrix.rec_mode);
    fm1_led_key(BTN_FX, g_matrix.menu_mode == MENU_FX);
    fm1_led_key(BTN_SEQ, g_matrix.view_page == 1);
    fm1_led_key(BTN_EDIT, g_matrix.view_mode == MODE_CURVE);
}

/* ---- Main Application Loop */
static void app_main(void)
{
    matrix_core_init();
    matrix_synth_init();
    matrix_ui_init();

    /* Try to load saved patterns from flash */
    settings_load(&g_matrix.patterns, sizeof(g_matrix.patterns));

    uint32_t last_ui_ms = 0;
    uint32_t last_tick_us[NUM_TRACKS] = {0};
    uint32_t last_save_ms = 0;
    uint8_t save_pending = 0;

    while (1) {
        platform_poll();
        uint32_t now_ms = fm1_ms;
        uint32_t now_us = now_ms * 1000u;
        uint32_t t;

        /* Check for incoming MIDI */
        handle_midi_in();

        /* =====================================================================
         * 1. 8-Track Sequencer Engine Clock
         * ===================================================================== */
        if (g_matrix.is_running) {
            for (t = 0; t < NUM_TRACKS; t++) {
                matrix_pattern_t *pat = &g_matrix.patterns[t][g_matrix.cur_bank[t] * PATS_PER_BANK + g_matrix.cur_pat[t]];
                uint32_t interval = matrix_get_step_interval_us(pat->resolution, g_matrix.bpm, g_matrix.cur_step[t], pat->shuffle);
                if (now_us - last_tick_us[t] >= interval) {
                    last_tick_us[t] = now_us;
                    step_track(t);
                }
            }
        }

        /* =====================================================================
         * 2. Physical Button & Function Handling
         * ===================================================================== */
        uint32_t released = 0;
        uint32_t pressed = fm1_input_edges(&released);

        /* Dismiss splash screen early if user touches buttons or keys */
        if ((pressed & ~(1u << BTN_PLAY)) || (fm1_in.notes != 0)) {
            matrix_ui_dismiss_splash();
        }

        /* Button Hold Tracking */
        if (fm1_in.buttons & (1u << BTN_EDIT)) {
            matrix_ui_set_held_status("⚡ SHIFT: +HOME=UNDO +SAVE=REDO +ENV=ENGINE");
        } else if (released & (1u << BTN_EDIT)) {
            matrix_ui_clear_held_status();
            matrix_ui_notify("SHIFT RELEASED");
        }

        if (pressed & (1u << BTN_PLAY)) {
            g_matrix.is_running = !g_matrix.is_running;
            if (!g_matrix.is_running) {
                for (t = 0; t < NUM_TRACKS; t++) {
                    matrix_pattern_t *pat = &g_matrix.patterns[t][g_matrix.cur_bank[t] * PATS_PER_BANK + g_matrix.cur_pat[t]];
                    midi_send(0xB0u | (pat->midi_ch - 1), 123, 0); /* All Notes Off */
                    matrix_synth_note_off(t);
                }
                matrix_ui_notify("STOP: SEQUENCER STOPPED");
            } else {
                matrix_ui_notify("PLAY: SEQUENCER RUNNING");
            }
        }

        if (pressed & (1u << BTN_REC)) {
            g_matrix.rec_mode = !g_matrix.rec_mode;
            matrix_ui_notify(g_matrix.rec_mode ? "REC: TAP KEY TO WRITE" : "REC: STEP REC OFF");
        }

        /* BTN_SEL: Cycle active track (T1 -> T2 -> ... -> T8) */
        if (pressed & (1u << BTN_SEL)) {
            g_matrix.active_track = (g_matrix.active_track + 1u) % NUM_TRACKS;
            uint8_t cur_t = g_matrix.active_track;
            uint8_t eng = g_matrix.track_types[cur_t];
            char t_buf[32];
            str_cpy(t_buf, "TRACK ", sizeof(t_buf));
            t_buf[6] = '1' + cur_t;
            t_buf[7] = ':';
            t_buf[8] = ' ';
            str_cpy(t_buf + 9, ENGINE_NAMES[eng % NUM_TRACK_TYPES], sizeof(t_buf) - 9);
            matrix_ui_notify(t_buf);
        }

        /* BTN_EDIT: Toggle KEYS vs CURVE mode */
        if (pressed & (1u << BTN_EDIT)) {
            g_matrix.view_mode = (g_matrix.view_mode == MODE_KEYS) ? MODE_CURVE : MODE_KEYS;
            matrix_ui_notify(g_matrix.view_mode == MODE_KEYS ? "VIEW: STEP NOTE / DRUM" : "VIEW: CC CURVE (CURV)");
        }

        /* BTN_OCT_DOWN / BTN_OCT_UP: Shift view octave or drum lane */
        uint8_t cur_trk = g_matrix.active_track;
        int cur_is_drum = (g_matrix.track_types[cur_trk] == TRACK_TYPE_808 || g_matrix.track_types[cur_trk] == TRACK_TYPE_909);

        if (pressed & (1u << BTN_OCT_DOWN)) {
            if (cur_is_drum) {
                if (g_matrix.drum_lane_sel[cur_trk] > 0) g_matrix.drum_lane_sel[cur_trk]--;
                matrix_ui_notify(DRUM_NAMES_FULL[g_matrix.drum_lane_sel[cur_trk]]);
            } else {
                if (g_matrix.view_octave > 1) g_matrix.view_octave--;
                notify_knob_val("OCTAVE", g_matrix.view_octave, 5);
            }
        }
        if (pressed & (1u << BTN_OCT_UP)) {
            if (cur_is_drum) {
                if (g_matrix.drum_lane_sel[cur_trk] < 11) g_matrix.drum_lane_sel[cur_trk]++;
                matrix_ui_notify(DRUM_NAMES_FULL[g_matrix.drum_lane_sel[cur_trk]]);
            } else {
                if (g_matrix.view_octave < 5) g_matrix.view_octave++;
                notify_knob_val("OCTAVE", g_matrix.view_octave, 5);
            }
        }

        /* BTN_SEQ: Toggle 16-step Page (1-16 vs 17-32) */
        if (pressed & (1u << BTN_SEQ)) {
            g_matrix.view_page = !g_matrix.view_page;
            matrix_ui_notify(g_matrix.view_page ? "PAGE 2: STEPS 17-32" : "PAGE 1: STEPS 1-16");
        }

        /* BTN_ENV: Cycle Sound Engine for active track (303A -> 303B -> 808 -> 909 -> BRK -> FM -> SUB -> MIDI) */
        if (pressed & (1u << BTN_ENV)) {
            g_matrix.track_types[cur_trk] = (g_matrix.track_types[cur_trk] + 1u) % NUM_TRACK_TYPES;
            char s_buf[32];
            str_cpy(s_buf, "ENGINE: ", sizeof(s_buf));
            str_cpy(s_buf + 8, ENGINE_NAMES[g_matrix.track_types[cur_trk]], sizeof(s_buf) - 8);
            matrix_ui_notify(s_buf);
        }

        /* BTN_FX: Open/Close Pattern FX menu, or Shift+FX for Randomize */
        if (pressed & (1u << BTN_FX)) {
            if (fm1_in.buttons & (1u << BTN_EDIT)) {
                matrix_randomize(g_matrix.active_track);
                matrix_ui_notify("RANDOMIZED PATTERN");
            } else {
                if (g_matrix.menu_mode == MENU_FX) {
                    g_matrix.menu_mode = MENU_NONE;
                    matrix_ui_notify("PATTERN TOOLS CLOSED");
                } else {
                    g_matrix.menu_mode = MENU_FX;
                    g_matrix.menu_sel = 0;
                    matrix_ui_notify("PATTERN TOOLS OPEN");
                }
            }
        }

        /* BTN_SAVE: Save to flash, or Shift+SAVE for REDO */
        if (pressed & (1u << BTN_SAVE)) {
            if (fm1_in.buttons & (1u << BTN_EDIT)) {
                matrix_redo();
                matrix_ui_notify("REDO: RE-APPLIED EDIT");
            } else {
                save_pending = 1;
                last_save_ms = now_ms;
                matrix_ui_notify("SAVED PATTERNS TO FLASH");
            }
        }

        /* BTN_HOME: Undo, track cycle, or Menu execute */
        if (pressed & (1u << BTN_HOME)) {
            if (fm1_in.buttons & (1u << BTN_EDIT)) {
                matrix_undo();
                matrix_ui_notify("UNDO: RESTORED STATE");
            } else if (g_matrix.menu_mode == MENU_FX) {
                uint8_t cur_t = g_matrix.active_track;
                switch (g_matrix.menu_sel) {
                    case 0: matrix_undo(); matrix_ui_notify("UNDO EXECUTED"); break;
                    case 1: matrix_redo(); matrix_ui_notify("REDO EXECUTED"); break;
                    case 2: matrix_shift_horizontal(cur_t, -1); matrix_ui_notify("SHIFTED LEFT"); break;
                    case 3: matrix_shift_horizontal(cur_t, 1); matrix_ui_notify("SHIFTED RIGHT"); break;
                    case 4: matrix_shift_vertical(cur_t, 1); matrix_ui_notify("TRANSPOSED +1"); break;
                    case 5: matrix_shift_vertical(cur_t, -1); matrix_ui_notify("TRANSPOSED -1"); break;
                    case 6: matrix_alter(cur_t); matrix_ui_notify("ALTERED PATTERN"); break;
                    case 7: matrix_randomize(cur_t); matrix_ui_notify("RANDOMIZED PATTERN"); break;
                    case 8: matrix_clear(cur_t); matrix_ui_notify("CLEARED PATTERN"); break;
                    case 9: matrix_ui_notify("MENU EXITED"); break;
                }
                g_matrix.menu_mode = MENU_NONE;
            } else {
                g_matrix.active_track = (g_matrix.active_track + 1u) % NUM_TRACKS;
                uint8_t cur_t = g_matrix.active_track;
                char t_buf[32];
                str_cpy(t_buf, "TRACK ", sizeof(t_buf));
                t_buf[6] = '1' + cur_t;
                t_buf[7] = ':';
                t_buf[8] = ' ';
                str_cpy(t_buf + 9, ENGINE_NAMES[g_matrix.track_types[cur_t]], sizeof(t_buf) - 9);
                matrix_ui_notify(t_buf);
            }
        }

        /* =====================================================================
         * 3. Encoders & Knobs Handling
         * ===================================================================== */
        int32_t enc_sel = fm1_enc_take(ENC_SELECT);
        if (enc_sel != 0) {
            matrix_ui_dismiss_splash();
            if (g_matrix.menu_mode == MENU_FX) {
                int32_t ms = (int32_t)g_matrix.menu_sel + enc_sel;
                if (ms < 0) ms = 9;
                if (ms > 9) ms = 0;
                g_matrix.menu_sel = ms;
                matrix_ui_notify(FX_MENU_ITEMS[g_matrix.menu_sel]);
            } else {
                int32_t as = (int32_t)g_matrix.active_step + enc_sel;
                if (as < 0) as = 31;
                if (as > 31) as = 0;
                g_matrix.active_step = (uint8_t)as;
                g_matrix.view_page = (g_matrix.active_step >= 16);
                matrix_pattern_t *p = &g_matrix.patterns[g_matrix.active_track][g_matrix.cur_bank[g_matrix.active_track] * PATS_PER_BANK + g_matrix.cur_pat[g_matrix.active_track]];
                matrix_step_t *st = &p->steps[g_matrix.active_step];
                if (cur_is_drum) {
                    notify_drum_step(g_matrix.active_step, g_matrix.drum_lane_sel[cur_trk], (st->drums & (1u << g_matrix.drum_lane_sel[cur_trk])) != 0);
                } else {
                    notify_step_note(g_matrix.active_step, st->note, st->gate);
                }
            }
        }

        int32_t enc_tempo = fm1_enc_take(ENC_ALGORITHM);
        if (enc_tempo != 0) {
            int32_t b = (int32_t)g_matrix.bpm + enc_tempo;
            if (b < 40) b = 40;
            if (b > 250) b = 250;
            g_matrix.bpm = (uint16_t)b;
            fxbus_set_bpm(&g_fxbus, (float)g_matrix.bpm);
            notify_bpm(g_matrix.bpm);
        }

        int32_t enc_pat = fm1_enc_take(ENC_PRESETS);
        if (enc_pat != 0) {
            uint8_t trk_idx = g_matrix.active_track;
            int32_t p = (int32_t)g_matrix.next_pat[trk_idx] + enc_pat;
            if (p < 0) p = 7;
            if (p > 7) p = 0;
            g_matrix.next_pat[trk_idx] = (uint8_t)p;
            notify_knob_val("PATTERN", g_matrix.next_pat[trk_idx] + 1, 8);
        }

        /* KNOB 1: Pitch (in KEYS mode) or Curve (in CURVE mode) */
        int32_t enc_k1 = fm1_enc_take(ENC_KNOB1);
        if (enc_k1 != 0) {
            matrix_pattern_t *p = &g_matrix.patterns[g_matrix.active_track][g_matrix.cur_bank[g_matrix.active_track] * PATS_PER_BANK + g_matrix.cur_pat[g_matrix.active_track]];
            matrix_step_t *st = &p->steps[g_matrix.active_step];
            if (g_matrix.view_mode == MODE_KEYS) {
                int32_t n = (int32_t)st->note + enc_k1;
                if (n < 24) n = 24;
                if (n > 108) n = 108;
                st->note = (uint8_t)n;
                notify_step_note(g_matrix.active_step, st->note, st->gate);
            } else {
                int32_t c = (int32_t)st->curve + enc_k1 * 4;
                if (c < 0) c = 0;
                if (c > 127) c = 127;
                st->curve = (uint8_t)c;
                notify_knob_val("CURVE CC", st->curve, 127);
            }
        }

        /* KNOB 2: Gate / Velocity */
        int32_t enc_k2 = fm1_enc_take(ENC_KNOB2);
        if (enc_k2 != 0) {
            matrix_pattern_t *p = &g_matrix.patterns[g_matrix.active_track][g_matrix.cur_bank[g_matrix.active_track] * PATS_PER_BANK + g_matrix.cur_pat[g_matrix.active_track]];
            matrix_step_t *st = &p->steps[g_matrix.active_step];
            int32_t v = (int32_t)st->gate + enc_k2 * 4;
            if (v < 0) v = 0;
            if (v > 127) v = 127;
            st->gate = (uint8_t)v;
            notify_knob_val(st->tie ? "VEL [TIE:ON]" : "VELOCITY", st->gate, 127);
        }

        /* KNOB 3: Cutoff filter / Tone */
        int32_t enc_k3 = fm1_enc_take(ENC_KNOB3);
        if (enc_k3 != 0) {
            matrix_synth_set_param(cur_trk, 0, (uint8_t)(75 + enc_k3 * 4));
            notify_knob_val("CUTOFF/TONE", 75 + enc_k3 * 4, 127);
        }

        /* KNOB 4: Resonance / Drive / FX */
        int32_t enc_k4 = fm1_enc_take(ENC_KNOB4);
        if (enc_k4 != 0) {
            matrix_synth_set_param(cur_trk, 1, (uint8_t)(90 + enc_k4 * 4));
            notify_knob_val("RESONANCE/FX", 90 + enc_k4 * 4, 127);
        }

        /* =====================================================================
         * 4. Keyboard Notes & Step Keys: Drum Toggles or Pitched Note Input
         * ===================================================================== */
        uint32_t note_down = fm1_input_note_edges();
        uint32_t k;
        for (k = 0; k < 27u; k++) {
            if (note_down & (1u << k)) {
                matrix_pattern_t *p = &g_matrix.patterns[g_matrix.active_track][g_matrix.cur_bank[g_matrix.active_track] * PATS_PER_BANK + g_matrix.cur_pat[g_matrix.active_track]];

                if (cur_is_drum) {
                    /* Drum Track: Only the 16 white keys toggle drum triggers (16 steps) */
                    int8_t step_i = WHITE_KEY_TO_STEP[k];
                    if (step_i >= 0) {
                        uint8_t target_s = (g_matrix.view_page * 16u) + (uint8_t)step_i;
                        uint8_t lane = g_matrix.drum_lane_sel[cur_trk];
                        matrix_undo_push(cur_trk);
                        p->steps[target_s].drums ^= (1u << lane);
                        int is_on = (p->steps[target_s].drums & (1u << lane)) != 0;
                        notify_drum_step(target_s, lane, is_on);

                        /* Audition drum sound */
                        matrix_step_t audit;
                        audit.drums = (1u << lane);
                        audit.gate = 110;
                        audit.tie = 0;
                        audit.curve = 64;
                        matrix_synth_trigger_step(cur_trk, &audit);
                    }
                } else {
                    /* Pitched Synth Track: Key note input or step jump */
                    uint8_t midi_note = 53u + (uint8_t)k; /* Key 0 = F3 */
                    if (g_matrix.rec_mode) {
                        matrix_undo_push(g_matrix.active_track);
                        p->steps[g_matrix.active_step].note = midi_note;
                        p->steps[g_matrix.active_step].gate = 110;
                        notify_step_note(g_matrix.active_step, midi_note, 110);
                        g_matrix.active_step = (g_matrix.active_step + 1u) % p->length;
                        g_matrix.view_page = (g_matrix.active_step >= 16);
                    } else if (k < 16u) {
                        g_matrix.active_step = (g_matrix.view_page * 16u) + (uint8_t)k;
                        notify_step_note(g_matrix.active_step, p->steps[g_matrix.active_step].note, p->steps[g_matrix.active_step].gate);
                    } else {
                        notify_step_note(g_matrix.active_step, midi_note, 110);
                    }

                    /* Audition note */
                    matrix_step_t audit;
                    audit.note = midi_note;
                    audit.gate = 110;
                    audit.tie = 0;
                    audit.curve = 64;
                    audit.drums = 0;
                    matrix_synth_trigger_step(g_matrix.active_track, &audit);
                }
            }
        }

        /* Periodic Flash Save if pending */
        if (save_pending && (now_ms - last_save_ms >= 500u)) {
            save_pending = 0;
            settings_save(&g_matrix.patterns, sizeof(g_matrix.patterns));
        }

        /* Update Panel and Keyboard LEDs */
        update_hardware_leds();

        /* 30 FPS Display Redraw */
        if (now_ms - last_ui_ms >= 33u) {
            last_ui_ms = now_ms;
            matrix_ui_draw();
        }
    }
}
