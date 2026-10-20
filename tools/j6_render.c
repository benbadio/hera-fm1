/* SPDX-License-Identifier: GPL-3.0-only */
/* j6_render.c - host-only demo: drives the whole J-6 engine (firmware/src/j6, j6_engine.c and every
 * module) like a player would, through j6_engine_key / _set / _cmd, and writes the audio as a 16-bit
 * stereo 44.1 kHz WAV. Float is allowed here (host tool), never in the engine.
 *
 *   tools/j6_render.sh [out.wav]          (default build/j6/demo.wav)
 *
 * The demo, at 120 BPM with chord set 1 (CHORD on, STYLE on), one chord per bar C - A - F - G
 * (Cadd9, FM/A, FM9, Em7/G):
 *   bars 1-4    style 6 var 2   (captured J-6 phrase table)   sound 30 Soft Keys
 *   bars 5-8    style 9 var 6   (captured J-6 phrase table)   sound 49 Harp
 *   bars 9-12   style 1 arps: var 2 (UP&DOWN 8ths) for 2 bars, var 8 (UP&DOWN 16ths) for 2 bars, sound 48
 * then the keys are released and 2 s of effect tails follow. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "j6.h"

#define BARS 12u
#define TAIL_FRAMES (2u * J6_FS)
#define MAX_FRAMES (BARS * 88200u + TAIL_FRAMES + 4096u)

static int16_t fx_mem[J6_FX_MEM_LEN];
static int32_t mix[MAX_FRAMES * 2u];
static uint32_t frames;
static unsigned long notes_on;

static void count_notes(void *ctx, const j6_tap_t *t)
{
    (void)ctx;
    if (t->ev.type == J6_EV_NOTE_ON) notes_on++;
}

static uint32_t ticks_now(void)
{
    j6_status_t st;
    j6_engine_status(&st);
    return st.ticks;
}

static void render(uint32_t n)
{
    uint8_t a, b, c;
    while (n >= J6_CR && frames + J6_CR <= MAX_FRAMES) {
        j6_engine_render(mix + 2u * frames, J6_CR);
        frames += J6_CR;
        n -= J6_CR;
        while (j6_engine_midi_out(&a, &b, &c)) {}
    }
}

/* render until `tick` is the next tick to begin: commands queued now apply right before it */
static void render_to_tick(uint32_t tick)
{
    while ((int32_t)(ticks_now() + 1u - tick) < 0 && frames + J6_CR <= MAX_FRAMES) render(J6_CR);
}

static int wav_write(const char *path, const int32_t *lr, uint32_t n)
{
    FILE *f = fopen(path, "wb");
    uint32_t i, data = n * 4u, v32;
    uint16_t v16;
    int32_t peak = 1;
    double gain;
    if (!f) return -1;
    for (i = 0; i < n * 2u; i++) {
        int32_t a = lr[i] < 0 ? -lr[i] : lr[i];
        if (a > peak) peak = a;
    }
    gain = peak > 29000 ? 29000.0 / (double)peak : 1.0;   /* never louder than the engine, -1 dBFS max */
#define W32(x) (v32 = (uint32_t)(x), fwrite(&v32, 4, 1, f))
#define W16(x) (v16 = (uint16_t)(x), fwrite(&v16, 2, 1, f))
    fwrite("RIFF", 1, 4, f);
    W32(36u + data);
    fwrite("WAVEfmt ", 1, 8, f);
    W32(16);
    W16(1);
    W16(2);
    W32(J6_FS);
    W32(J6_FS * 4u);
    W16(4);
    W16(16);
    fwrite("data", 1, 4, f);
    W32(data);
    for (i = 0; i < n * 2u; i++) {
        double s = (double)lr[i] * gain;
        int32_t v = (int32_t)(s < 0 ? s - 0.5 : s + 0.5);
        W16((int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v));
    }
#undef W32
#undef W16
    printf("  peak %d of %d (gain %.3f)\n", (int)peak, J6_OUT_MAX, gain);
    return fclose(f) ? -1 : 0;
}

int main(int argc, char **argv)
{
    static const int32_t prog[4] = {0, 9, 5, 7};     /* C A F G: Cadd9 FM/A FM9 Em7/G in chord set 1 */
    const char *out = argc > 1 ? argv[1] : "build/j6/demo.wav";
    uint32_t bar, t0;
    int32_t held = -100;

    j6_engine_set_tap(count_notes, NULL);
    j6_engine_init(fx_mem, J6_FX_MEM_LEN, NULL);
    j6_engine_set(J6_P_TEMPO10, 1200);
    j6_engine_set(J6_P_CHORD_SET, 0);                /* chord set 1 */
    j6_engine_set(J6_P_CHORD, 1);
    j6_engine_set(J6_P_STYLE_ON, 1);
    render(J6_CR);
    t0 = ticks_now() + 1u;

    for (bar = 0; bar < BARS; bar++) {
        int32_t key = prog[bar % 4u];
        render_to_tick(t0 + bar * J6_TICKS_BAR);
        if (bar == 0) {
            j6_engine_set(J6_P_SOUND, 30);
            j6_engine_set(J6_P_STYLE, 6);
            j6_engine_set(J6_P_VAR, 2);
            j6_engine_set(J6_P_KNOB0 + J6_KNOB_REV_LEVEL, 50);
        } else if (bar == 4) {
            j6_engine_set(J6_P_SOUND, 49);
            j6_engine_set(J6_P_STYLE, 9);
            j6_engine_set(J6_P_VAR, 6);
        } else if (bar == 8) {
            j6_engine_set(J6_P_SOUND, 48);
            j6_engine_set(J6_P_STYLE, 1);
            j6_engine_set(J6_P_VAR, 2);
            j6_engine_set(J6_P_KNOB0 + J6_KNOB_DLY_LEVEL, 45);
            j6_engine_set(J6_P_VOL, 127);            /* single notes: a little louder */
        } else if (bar == 10) {
            j6_engine_set(J6_P_VAR, 8);
        }
        j6_engine_key(key, 1);                       /* mono chord keys (CH-6): the new chord replaces */
        if (held != -100) j6_engine_key(held, 0);
        held = key;
    }
    render_to_tick(t0 + BARS * J6_TICKS_BAR);
    j6_engine_key(held, 0);
    render(TAIL_FRAMES);

    printf("  %u frames (%.2f s), %lu notes\n", (unsigned)frames, (double)frames / J6_FS, notes_on);
    if (wav_write(out, mix, frames) != 0) {
        fprintf(stderr, "j6_render: cannot write %s\n", out);
        return 1;
    }
    printf("  wrote %s\n", out);
    return notes_on > 0 ? 0 : 1;
}
