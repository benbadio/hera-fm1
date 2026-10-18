/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* The few libc routines the compiler may call implicitly (struct copies,
 * zeroing). Freestanding: no other libc. */
void *memset(void *d, int c, unsigned n)
{
    unsigned char *p = d;
    while (n--)
        *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *s, unsigned n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    while (n--)
        *p++ = *q++;
    return d;
}

int memcmp(const void *a, const void *b, unsigned n)
{
    const unsigned char *p = a, *q = b;
    for (; n; n--, p++, q++)
        if (*p != *q)
            return *p - *q;
    return 0;
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static unsigned str_len(const char *s)
{
    unsigned n = 0;
    while (s[n])
        n++;
    return n;
}

static void str_cpy(char *d, const char *s, unsigned max)
{
    unsigned i = 0;
    while (s[i] && i + 1 < max) {
        d[i] = s[i];
        i++;
    }
    d[i] = 0;
}

/* decimal with an optional fixed point: fmt_fix(buf, -63, 1) -> "-6.3" */
static void fmt_fix(char *b, int32_t v, int decimals)
{
    char t[12];
    int n = 0, neg = v < 0, i;
    uint32_t u = (uint32_t)(neg ? -v : v);
    do {
        t[n++] = (char)('0' + u % 10u);
        u /= 10u;
        if (n == decimals)
            t[n++] = '.';
    } while (u || n <= decimals);
    if (neg)
        t[n++] = '-';
    for (i = 0; i < n; i++)
        b[i] = t[n - 1 - i];
    b[n] = 0;
}

static void fmt_int(char *b, int32_t v) { fmt_fix(b, v, 0); }

static uint32_t rng_state = 0x1234567u;
static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* Compiler-rt soft-float conversion routines for freestanding JieLi toolchain */
float __floatsisf(int32_t i)
{
    if (i == 0) return 0.0f;
    uint32_t sign = 0;
    uint32_t u;
    if (i < 0) {
        sign = 0x80000000u;
        u = (uint32_t)(-(uint32_t)i);
    } else {
        u = (uint32_t)i;
    }
    int shift = 0;
    while ((u & 0x80000000u) == 0) {
        u <<= 1;
        shift++;
    }
    int exp = 127 + (31 - shift);
    uint32_t mantissa = (u >> 8) & 0x7FFFFFu;
    if ((u & 0x80u) && ((u & 0x7Fu) || (mantissa & 1u))) {
        mantissa++;
        if (mantissa & 0x800000u) {
            mantissa = 0;
            exp++;
        }
    }
    union { uint32_t u; float f; } b;
    b.u = sign | ((uint32_t)exp << 23) | mantissa;
    return b.f;
}

float __floatunsisf(uint32_t u)
{
    if (u == 0) return 0.0f;
    int shift = 0;
    while ((u & 0x80000000u) == 0) {
        u <<= 1;
        shift++;
    }
    int exp = 127 + (31 - shift);
    uint32_t mantissa = (u >> 8) & 0x7FFFFFu;
    if ((u & 0x80u) && ((u & 0x7Fu) || (mantissa & 1u))) {
        mantissa++;
        if (mantissa & 0x800000u) {
            mantissa = 0;
            exp++;
        }
    }
    union { uint32_t u; float f; } b;
    b.u = ((uint32_t)exp << 23) | mantissa;
    return b.f;
}

int32_t __fixsfsi(float f)
{
    union { float f; uint32_t u; } b;
    b.f = f;
    uint32_t sign = b.u >> 31;
    int exp = (int)((b.u >> 23) & 0xFF) - 127;
    if (exp < 0) return 0;
    if (exp >= 31) return sign ? (int32_t)0x80000000 : 0x7FFFFFFF;
    uint32_t mant = (b.u & 0x7FFFFFu) | 0x800000u;
    uint32_t val;
    if (exp <= 23)
        val = mant >> (23 - exp);
    else
        val = mant << (exp - 23);
    return sign ? -(int32_t)val : (int32_t)val;
}

uint32_t __fixunssfsi(float f)
{
    union { float f; uint32_t u; } b;
    b.f = f;
    if (b.u & 0x80000000u) return 0;
    int exp = (int)((b.u >> 23) & 0xFF) - 127;
    if (exp < 0) return 0;
    if (exp >= 32) return 0xFFFFFFFFu;
    uint32_t mant = (b.u & 0x7FFFFFu) | 0x800000u;
    if (exp <= 23)
        return mant >> (23 - exp);
    else
        return mant << (exp - 23);
}
