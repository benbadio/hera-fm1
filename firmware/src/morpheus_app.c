/* SPDX-License-Identifier: GPL-3.0-only */
/* MORPHEUS: 4-Track Reason Matrix Pattern Sequencer & Synth for M-VAVE FM-1 */

#ifndef APP_NAME
#define APP_NAME "MORPHEUS"
#endif
#ifndef FELUCCA_ID
#define FELUCCA_ID "FM-1_988"
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
            /* Note On: Live keyboard transposition or audition */
            uint8_t trk = g_matrix.active_track;
            if (ch == g_matrix.patterns[trk][0].midi_ch) {
                int8_t transpose_delta = (int8_t)d1 - 48; /* Relative to C3 */
                matrix_shift_vertical(trk, transpose_delta);
            }
            if (trk == g_matrix.sound_track) {
                matrix_synth_note_on(d1, d2, 0);
            }
        } else if (cmd == 0x80u || (cmd == 0x90u && d2 == 0)) {
            /* Note Off */
            if (g_matrix.active_track == g_matrix.sound_track) {
                matrix_synth_note_off();
            }
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

/* ---- Sequencer Timing & Step Advancement */
static void step_track(uint8_t trk)
{
    matrix_pattern_t *pat = &g_matrix.patterns[trk][g_matrix.cur_bank[trk] * PATS_PER_BANK + g_matrix.cur_pat[trk]];
    if (pat->mute) return;

    uint8_t step_idx = g_matrix.cur_step[trk];
    matrix_step_t *st = &pat->steps[step_idx];
    uint8_t ch = pat->midi_ch - 1;

    /* 1. Send Note Off for previous step if not tied */
    if (g_matrix.last_gate[trk] > 0 && !st->tie) {
        midi_send(0x80u | ch, g_matrix.last_note[trk], 0);
        if (trk == g_matrix.sound_track) {
            matrix_synth_note_off();
        }
    }

    /* 2. Play current step if gate > 0 */
    if (st->gate > 0) {
        midi_send(0x90u | ch, st->note, st->gate);
        g_matrix.last_note[trk] = st->note;
        g_matrix.last_gate[trk] = st->gate;

        if (trk == g_matrix.sound_track) {
            /* Pass to internal synth */
            g_synth.curve_mod = (int16_t)st->curve - 64;
            matrix_synth_note_on(st->note, st->gate, st->tie);
        }
    } else {
        g_matrix.last_gate[trk] = 0;
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

/* ---- Main Application Loop */
static void app_main(void)
{
    matrix_core_init();
    matrix_synth_init();
    matrix_ui_init();

    /* Try to load saved patterns from flash */
    settings_load(&g_matrix.patterns, sizeof(g_matrix.patterns));

    uint32_t last_tick_us[NUM_TRACKS];
    uint32_t t;
    uint32_t now_us = fm1_micros();
    for (t = 0; t < NUM_TRACKS; t++) {
        last_tick_us[t] = now_us;
    }

    uint32_t last_ui_ms = 0;
    uint32_t last_save_ms = 0;
    uint8_t save_pending = 0;
    uint32_t prev_notes = 0;

    for (;;) {
        platform_poll();
        handle_midi_in();

        now_us = fm1_micros();
        uint32_t now_ms = fm1_ms;

        /* =====================================================================
         * 1. 4-Track Sequencer Engine Clock
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

        /* Button Hold Tracking */
        if (fm1_in.buttons & (1u << BTN_EDIT)) {
            matrix_ui_set_held_status("⚡ SHIFT: +HOME=UNDO +SAVE=REDO");
        } else if (released & (1u << BTN_EDIT)) {
            matrix_ui_clear_held_status();
            matrix_ui_notify("SHIFT RELEASED");
        }

        if (pressed & (1u << BTN_PLAY)) {
            g_matrix.is_running = !g_matrix.is_running;
            if (!g_matrix.is_running) {
                /* All notes off when stopped */
                for (t = 0; t < NUM_TRACKS; t++) {
                    matrix_pattern_t *pat = &g_matrix.patterns[t][g_matrix.cur_bank[t] * PATS_PER_BANK + g_matrix.cur_pat[t]];
                    midi_send(0xB0u | (pat->midi_ch - 1), 123, 0); /* All Notes Off */
                }
                matrix_synth_note_off();
                matrix_ui_notify("STOP: SEQUENCER STOPPED");
            } else {
                matrix_ui_notify("PLAY: SEQUENCER RUNNING");
            }
        }

        if (pressed & (1u << BTN_REC)) {
            g_matrix.rec_mode = !g_matrix.rec_mode;
            matrix_ui_notify(g_matrix.rec_mode ? "REC: TAP KEY TO WRITE" : "REC: STEP REC OFF");
        }

        /* BTN_SEL: Cycle active track (T1 -> T2 -> T3 -> T4) */
        if (pressed & (1u << BTN_SEL)) {
            g_matrix.active_track = (g_matrix.active_track + 1u) % NUM_TRACKS;
            if (g_matrix.active_track == 3) matrix_ui_notify("TRACK 4: DRUMS (CH10)");
            else if (g_matrix.active_track == 0) matrix_ui_notify("TRACK 1: 303 ACID BASS");
            else if (g_matrix.active_track == 1) matrix_ui_notify("TRACK 2: SYNTH LEAD");
            else matrix_ui_notify("TRACK 3: ARPEGGIATOR");
        }

        /* BTN_EDIT: Toggle KEYS vs CURVE mode */
        if (pressed & (1u << BTN_EDIT)) {
            g_matrix.view_mode = (g_matrix.view_mode == MODE_KEYS) ? MODE_CURVE : MODE_KEYS;
            matrix_ui_notify(g_matrix.view_mode == MODE_KEYS ? "VIEW: NOTE PITCH (KEYS)" : "VIEW: CC CURVE (CURV)");
        }

        /* BTN_OCT_DOWN / BTN_OCT_UP: Shift view octave (1..5) */
        if (pressed & (1u << BTN_OCT_DOWN)) {
            if (g_matrix.view_octave > 1) g_matrix.view_octave--;
            notify_knob_val("OCTAVE", g_matrix.view_octave, 5);
        }
        if (pressed & (1u << BTN_OCT_UP)) {
            if (g_matrix.view_octave < 5) g_matrix.view_octave++;
            notify_knob_val("OCTAVE", g_matrix.view_octave, 5);
        }

        /* BTN_SEQ: Toggle 16-step Page (1-16 vs 17-32) */
        if (pressed & (1u << BTN_SEQ)) {
            g_matrix.view_page = !g_matrix.view_page;
            matrix_ui_notify(g_matrix.view_page ? "PAGE 2: STEPS 17-32" : "PAGE 1: STEPS 1-16");
        }

        /* BTN_ENV: Cycle Sound Engine (303 -> SUB -> FM -> MUTE) */
        if (pressed & (1u << BTN_ENV)) {
            g_matrix.sound_engine = (g_matrix.sound_engine + 1u) % NUM_SOUND_ENGINES;
            g_synth.engine_mode = g_matrix.sound_engine;
            switch (g_matrix.sound_engine) {
                case 0: matrix_ui_notify("SOUND: 303 ACID BASS"); break;
                case 1: matrix_ui_notify("SOUND: SUB SYNTH LEAD"); break;
                case 2: matrix_ui_notify("SOUND: 2-OP FM SYNTH"); break;
                default: matrix_ui_notify("SOUND: MUTE (MIDI ONLY)"); break;
            }
        }

        /* BTN_FX: Open/Close Pattern FX menu, or Shift+FX for Randomize */
        if (pressed & (1u << BTN_FX)) {
            if (fm1_in.buttons & (1u << BTN_EDIT)) {
                /* Shift + FX = Deliberate Randomize with Undo */
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

        /* BTN_SAVE: Save patterns to flash, or Shift+SAVE for Redo */
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

        /* BTN_HOME: Cycle track, Execute Menu, or Shift+HOME for Undo */
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
                if (g_matrix.active_track == 3) matrix_ui_notify("TRACK 4: DRUMS (CH10)");
                else if (g_matrix.active_track == 0) matrix_ui_notify("TRACK 1: 303 ACID BASS");
                else if (g_matrix.active_track == 1) matrix_ui_notify("TRACK 2: SYNTH LEAD");
                else matrix_ui_notify("TRACK 3: ARPEGGIATOR");
            }
        }

        /* =====================================================================
         * 3. Rotary Encoders
         * ===================================================================== */
        int32_t enc_sel = fm1_enc_take(ENC_SELECT);
        if (enc_sel != 0) {
            if (g_matrix.menu_mode == MENU_FX) {
                int8_t ms = g_matrix.menu_sel + (int8_t)enc_sel;
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
                notify_step_note(g_matrix.active_step, st->note, st->gate);
            }
        }

        int32_t enc_bpm = fm1_enc_take(ENC_ALGORITHM);
        if (enc_bpm != 0) {
            int32_t b = (int32_t)g_matrix.bpm + enc_bpm;
            if (b < 40) b = 40;
            if (b > 250) b = 250;
            g_matrix.bpm = (uint16_t)b;
            notify_bpm(g_matrix.bpm);
        }

        int32_t enc_pat = fm1_enc_take(ENC_PRESETS);
        if (enc_pat != 0) {
            uint8_t trk_idx = g_matrix.active_track;
            int32_t p = (int32_t)g_matrix.cur_pat[trk_idx] + enc_pat;
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

        /* KNOB 2: Gate Velocity & Tie */
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

        /* KNOB 3: Cutoff filter */
        int32_t enc_k3 = fm1_enc_take(ENC_KNOB3);
        if (enc_k3 != 0) {
            int32_t cut = (int32_t)g_synth.knob_cutoff + enc_k3 * 2;
            if (cut < 10) cut = 10;
            if (cut > 127) cut = 127;
            g_synth.knob_cutoff = (uint8_t)cut;
            notify_knob_val("CUTOFF", g_synth.knob_cutoff, 127);
        }

        /* KNOB 4: Resonance */
        int32_t enc_k4 = fm1_enc_take(ENC_KNOB4);
        if (enc_k4 != 0) {
            int32_t res = (int32_t)g_synth.knob_resonance + enc_k4 * 2;
            if (res < 0) res = 0;
            if (res > 127) res = 127;
            g_synth.knob_resonance = (uint8_t)res;
            notify_knob_val("RESONANCE", g_synth.knob_resonance, 127);
        }

        /* =====================================================================
         * 4. 27 Silicone Keys: Step Entry & Note Audition
         * ===================================================================== */
        uint32_t cur_notes = fm1_in.notes;
        uint32_t note_down = cur_notes & ~prev_notes;
        prev_notes = cur_notes;

        if (note_down) {
            uint32_t k;
            for (k = 0; k < 27u; k++) {
                if (note_down & (1u << k)) {
                    /* Key 0 is F3 = MIDI 53 */
                    uint8_t midi_note = 53u + (uint8_t)k;
                    matrix_pattern_t *p = &g_matrix.patterns[g_matrix.active_track][g_matrix.cur_bank[g_matrix.active_track] * PATS_PER_BANK + g_matrix.cur_pat[g_matrix.active_track]];

                    if (g_matrix.rec_mode) {
                        /* Record note directly to current step, then advance */
                        matrix_undo_push(g_matrix.active_track);
                        p->steps[g_matrix.active_step].note = midi_note;
                        p->steps[g_matrix.active_step].gate = 110;
                        notify_step_note(g_matrix.active_step, midi_note, 110);
                        g_matrix.active_step = (g_matrix.active_step + 1u) % p->length;
                        g_matrix.view_page = (g_matrix.active_step >= 16);
                    } else if (k < 16u) {
                        /* Keys 0..15 jump directly to step 0..15 on active page! */
                        g_matrix.active_step = (g_matrix.view_page * 16u) + (uint8_t)k;
                        notify_step_note(g_matrix.active_step, p->steps[g_matrix.active_step].note, p->steps[g_matrix.active_step].gate);
                    } else {
                        notify_step_note(g_matrix.active_step, midi_note, 110);
                    }

                    /* Audition note */
                    if (g_matrix.active_track == g_matrix.sound_track) {
                        matrix_synth_note_on(midi_note, 110, 0);
                    }
                    midi_send(0x90u | (p->midi_ch - 1), midi_note, 110);
                }
            }
        }

        /* =====================================================================
         * 5. LED Indicator Feedback
         * ===================================================================== */
        fm1_led_key(BTN_PLAY, g_matrix.is_running);
        fm1_led_key(BTN_REC, g_matrix.rec_mode);
        fm1_led_key(BTN_EDIT, g_matrix.view_mode == MODE_CURVE);

        /* =====================================================================
         * 6. Save to Flash (safe deferred save)
         * ===================================================================== */
        if (save_pending && (now_ms - last_save_ms > 1000u)) {
            settings_save(&g_matrix.patterns, sizeof(g_matrix.patterns));
            save_pending = 0;
        }

        /* =====================================================================
         * 7. Refresh LCD Display (every 25ms = 40 FPS)
         * ===================================================================== */
        if (now_ms - last_ui_ms >= 25u) {
            last_ui_ms = now_ms;
            matrix_ui_draw();
        }
    }
}
