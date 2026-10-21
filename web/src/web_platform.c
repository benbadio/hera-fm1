/* SPDX-License-Identifier: GPL-3.0-only */
/* web_platform.c - the FM-1 platform in a browser: HERA (firmware/src/hera_app.c) compiled to WebAssembly,
 * unchanged. platform.c includes this file instead of the hardware when DINGHY_WEB is set (built by
 * web/build.sh, freestanding: no libc). JavaScript drives it through the exported web_* functions:
 *
 *   web_init()                          once: the firmware's setup (engine, storage, splash)
 *   web_step(ms)                        advance the clock to ms and run one pass of the main loop
 *   web_audio(n)                        render n stereo frames into web_audio_buf() (int32 L R ..., Q15)
 *   web_button(id, down) web_keys(mask) web_enc(id, delta)     the panel
 *   web_midi_in(st, d1, d2) / web_midi_out() (packed st | d1 << 8 | d2 << 16, 0 = none)
 *   web_fb()  240 x 240 RGB565 screen     web_leds()  41 LED bytes (14 buttons, then 27 keys)
 *   web_store() / web_store_len()       the pattern + settings flash (65 x 4 KiB), to save / restore
 * The editor also calls the engine directly (j6_engine_get / set / cmd / status ..., exported too). */
#include <stdint.h>
#include <stddef.h>

#ifndef APP_NAME
#define APP_NAME "HERA"
#endif
#ifndef APP_VER
#define APP_VER "web"
#endif
#define FS 44100u
#define HALF_FRAMES 256u

/* ---- freestanding C library bits the firmware uses ------------------------------------------------ */
void *memset(void *d, int c, size_t n)
{
    unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}
void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    while (n--) *p++ = *q++;
    return d;
}
void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    if (p < q) while (n--) *p++ = *q++;
    else { p += n; q += n; while (n--) *--p = *--q; }
    return d;
}
int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a, *q = b;
    for (; n; n--, p++, q++)
        if (*p != *q) return *p - *q;
    return 0;
}
static inline unsigned str_len(const char *s)
{
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}
static inline void str_cpy(char *d, const char *s, unsigned max)
{
    unsigned i = 0;
    while (s[i] && i + 1 < max) {
        d[i] = s[i];
        i++;
    }
    d[i] = 0;
}
static inline void fmt_int(char *b, int32_t v)
{
    char t[12];
    unsigned n = 0, u = v < 0 ? (unsigned)-v : (unsigned)v;
    if (v < 0) *b++ = '-';
    do { t[n++] = (char)('0' + u % 10u); u /= 10u; } while (u);
    while (n) *b++ = t[--n];
    *b = 0;
}

/* ---- time, input, LEDs ---------------------------------------------------------------------------- */
static volatile uint32_t fm1_ms;
static inline uint32_t fm1_micros(void) { return fm1_ms * 1000u; }
static inline uint32_t fm1_ticks(void) { return fm1_ms * 24000u; }
static struct { uint32_t notes, buttons; } fm1_in;
static uint32_t web_edges, web_released;
static int32_t web_encs[7];
static uint8_t web_led[41];
static uint32_t fm1_input_edges(uint32_t *released)
{
    uint32_t b = web_edges;
    web_edges = 0;
    *released = web_released;
    web_released = 0;
    return b;
}
static uint32_t fm1_input_note_edges(void) { return 0; }
static int32_t fm1_enc_take(uint32_t e)
{
    int32_t d;
    if (e >= 7u) return 0;
    d = web_encs[e];
    web_encs[e] = 0;
    return d;
}
static void fm1_led_key(uint32_t id, int on)
{
    if (id < sizeof web_led) web_led[id] = (uint8_t)(on ? 1 : 0);
}

/* ---- the screen: 240 x 240 RGB565 (what the ST7789 shows) ----------------------------------------- */
static uint16_t fb[240 * 240];
static void lcd_sync(void) {}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    for (j = y; j < y + h && j < 240u; j++)
        for (i = x; i < x + w && i < 240u; i++) fb[j * 240u + i] = c;
}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    uint32_t i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            if (x + i < 240u && y + j < 240u) {
                uint16_t c = px[j * w + i];
                fb[(y + j) * 240u + x + i] = (uint16_t)(c >> 8 | c << 8);
            }
}

#ifndef CV_MAX
#define CV_MAX (240u * 128u)
#endif
#include "gfx.c"

/* ---- settings sector (unused by HERA; kept for the platform API) ---------------------------------- */
static uint8_t web_settings[260];
static int settings_load(void *p, uint32_t n)
{
    if (web_settings[0] != 1 || n > 256u) return -1;
    memcpy(p, web_settings + 4, n);
    return 0;
}
static int settings_save(const void *p, uint32_t n)
{
    if (n > 256u) return -1;
    web_settings[0] = 1;
    memcpy(web_settings + 4, p, n);
    return 0;
}

/* ---- MIDI: in from the page (Web MIDI or the editor), out to the page ------------------------------ */
#define WMQ 256u
static uint32_t wmi[WMQ], wmo[WMQ];
static uint32_t wmi_r, wmi_w, wmo_r, wmo_w;
static int midi_get(uint8_t *st, uint8_t *d1, uint8_t *d2)
{
    uint32_t m;
    if (wmi_r == wmi_w) return 0;
    m = wmi[wmi_r++ % WMQ];
    *st = (uint8_t)m;
    *d1 = (uint8_t)(m >> 8);
    *d2 = (uint8_t)(m >> 16);
    return 1;
}
static void midi_send(uint32_t st, uint32_t d1, uint32_t d2)
{
    if (wmo_w - wmo_r < WMQ) wmo[wmo_w++ % WMQ] = (st & 0xFFu) | (d1 & 0x7Fu) << 8 | (d2 & 0x7Fu) << 16;
}

static void app_audio(int32_t *out, uint32_t n);
static void hera_setup(void);
static void hera_step(void);
static void platform_poll(void) {}

/* ---- the exports ---------------------------------------------------------------------------------- */
static int32_t web_abuf[2u * 1024u];
static uint8_t web_ready;
void web_init(void)
{
    if (web_ready) return;
    web_ready = 1;
    hera_setup();
}
void web_step(uint32_t ms)
{
    fm1_ms = ms;
    hera_step();
}
int32_t *web_audio_buf(void) { return web_abuf; }
void web_audio(uint32_t n)                       /* n <= 1024, a multiple of 16 */
{
    uint32_t i, done = 0;
    if (n > 1024u) n = 1024u;
    for (i = 0; i < 2u * n; i++) web_abuf[i] = 0;
    while (done < n) {
        uint32_t k = n - done > HALF_FRAMES ? HALF_FRAMES : n - done;
        app_audio(web_abuf + 2u * done, k);
        done += k;
    }
}
void web_button(uint32_t id, uint32_t down)
{
    if (id >= 14u) return;
    if (down) {
        fm1_in.buttons |= 1u << id;
        web_edges |= 1u << id;
    } else {
        fm1_in.buttons &= ~(1u << id);
        web_released |= 1u << id;
    }
}
void web_keys(uint32_t mask) { fm1_in.notes = mask & 0x7FFFFFFu; }
void web_enc(uint32_t id, int32_t d)
{
    if (id < 7u) web_encs[id] += d;
}
void web_midi_in(uint32_t st, uint32_t d1, uint32_t d2)
{
    if (wmi_w - wmi_r < WMQ) wmi[wmi_w++ % WMQ] = (st & 0xFFu) | (d1 & 0x7Fu) << 8 | (d2 & 0x7Fu) << 16;
}
uint32_t web_midi_out(void) { return wmo_r == wmo_w ? 0u : wmo[wmo_r++ % WMQ]; }
uint16_t *web_fb(void) { return fb; }
uint8_t *web_leds(void) { return web_led; }
