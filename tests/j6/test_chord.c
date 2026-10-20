/* SPDX-License-Identifier: GPL-3.0-only */
/* test_chord.c - host tests of the chord module (firmware/src/j6/j6_chord.c, j6_chord_tables.h).
 *
 * Rules: KB-1, KB-3, KB-4, KB-6, CH-2 .. CH-5, C-1 in docs/J6_SPEC.md; design: docs/J6_ENGINE_DESIGN.md 4.3.
 * Needs only j6_chord.c (J6_ISOLATED=1 links nothing else). Runs from the repository root: it reads
 * data/chordsets.json (read only) and checks every chord, name and genre of the generated table against it.
 * It also includes the generated header itself, to see the listed notes before j6_chord.c dedups them. */
#include <limits.h>
#include <time.h>

#include "j6_test.h"
#include "j6_chord_tables.h"

#define TC_JSON_PATH "data/chordsets.json"

/* ---- helpers -------------------------------------------------------------------------------------- */

/* the expected j6_chord_notes(): computed independently from j6_chord_raw() and the j6.h helpers */
static uint32_t tc_expect(uint32_t set, int32_t key, int32_t oct, int32_t key_tr, uint8_t out[4])
{
    uint8_t raw[4];
    uint32_t idx = j6_key_chord_idx(key), n = 0, i, cnt;
    int32_t off = 12 * (oct + j6_key_floor12(key)) + key_tr;
    cnt = j6_chord_raw(set, idx, raw);
    for (i = 0; i < 4; i++) out[i] = J6_NO_NOTE;
    for (i = 0; i < cnt; i++) {
        int32_t v = (int32_t)raw[i] + off;
        if (v >= 0 && v <= 127) out[n++] = (uint8_t)v;
    }
    return n;
}

/* 1 if n notes are strictly ascending, in 0..127, and the remaining slots are J6_NO_NOTE */
static int tc_well_formed(const uint8_t *o, uint32_t n)
{
    uint32_t i;
    if (n > 4) return 0;
    for (i = 0; i < n; i++) {
        if (o[i] > 127) return 0;
        if (i > 0 && o[i] <= o[i - 1]) return 0;
    }
    for (i = n; i < 4; i++)
        if (o[i] != J6_NO_NOTE) return 0;
    return 1;
}

static int tc_notes_eq(const uint8_t *o, uint32_t n, const int *want, uint32_t wn)
{
    uint32_t i;
    if (n != wn) return 0;
    for (i = 0; i < n; i++)
        if (o[i] != want[i]) return 0;
    return 1;
}

/* CHECK that j6_chord_raw(set, idx) gives exactly the notes in want[] */
#define CHECK_RAW(set, idx, ...)                                                                    \
    do {                                                                                            \
        static const int tc_w_[] = {__VA_ARGS__};                                                   \
        uint8_t tc_o_[4];                                                                           \
        uint32_t tc_n_ = j6_chord_raw((set), (idx), tc_o_);                                         \
        CHECK_EQ(tc_n_, sizeof tc_w_ / sizeof tc_w_[0]);                                            \
        CHECK(tc_notes_eq(tc_o_, tc_n_, tc_w_, (uint32_t)(sizeof tc_w_ / sizeof tc_w_[0])));        \
        CHECK(tc_well_formed(tc_o_, tc_n_));                                                        \
    } while (0)

/* ---- a minimal reader for data/chordsets.json (the fixed schema of tools/parse_manual_chords.py) ---- */

typedef struct {
    int key;
    char name[64];
    int notes[16];
    int n;
} tc_jchord_t;

typedef struct {
    int set;
    char genre[64];
    tc_jchord_t ch[12];
    int nch;
} tc_jset_t;

static char tc_buf[1 << 20];
static tc_jset_t tc_sets[100];
static int tc_nsets;
static const char *tc_p;
static int tc_non_ascii;

static void tc_ws(void)
{
    while (*tc_p == ' ' || *tc_p == '\n' || *tc_p == '\r' || *tc_p == '\t') tc_p++;
}
static int tc_lit(char c)
{
    tc_ws();
    if (*tc_p != c) return 0;
    tc_p++;
    return 1;
}
static int tc_str(char *buf, size_t cap)
{
    size_t k = 0;
    tc_ws();
    if (*tc_p != '"') return 0;
    tc_p++;
    while (*tc_p && *tc_p != '"') {
        unsigned char ch = (unsigned char)*tc_p++;
        if (ch == '\\') {
            ch = (unsigned char)*tc_p++;
            if (ch == 'u') {
                unsigned v = 0;
                int i;
                for (i = 0; i < 4; i++) {
                    char h = *tc_p++;
                    v = v * 16u + (unsigned)(h >= 'a' ? h - 'a' + 10 : h >= 'A' ? h - 'A' + 10 : h - '0');
                }
                if (v > 0x7Eu || v < 0x20u) tc_non_ascii++;
                ch = (unsigned char)(v & 0x7Fu);
            } else if (ch == 'n' || ch == 't' || ch == 'r' || ch == 'b' || ch == 'f') {
                tc_non_ascii++;
            } else if (ch == 0) {
                return 0;
            }
        } else if (ch < 0x20u || ch > 0x7Eu) {
            tc_non_ascii++;
        }
        if (k + 1 >= cap) return 0;
        buf[k++] = (char)ch;
    }
    if (*tc_p != '"') return 0;
    tc_p++;
    buf[k] = '\0';
    return 1;
}
static int tc_num(int *v)
{
    int neg = 0, any = 0;
    long x = 0;
    tc_ws();
    if (*tc_p == '-') {
        neg = 1;
        tc_p++;
    }
    while (*tc_p >= '0' && *tc_p <= '9') {
        x = x * 10 + (*tc_p++ - '0');
        any = 1;
        if (x > 100000) return 0;
    }
    if (!any || *tc_p == '.' || *tc_p == 'e' || *tc_p == 'E') return 0;
    *v = (int)(neg ? -x : x);
    return 1;
}
static int tc_chord(tc_jchord_t *c)
{
    char k[16];
    c->n = 0;
    c->key = -1;
    c->name[0] = '\0';
    if (!tc_lit('{')) return 0;
    do {
        if (!tc_str(k, sizeof k) || !tc_lit(':')) return 0;
        if (!strcmp(k, "key")) {
            if (!tc_num(&c->key)) return 0;
        } else if (!strcmp(k, "name")) {
            if (!tc_str(c->name, sizeof c->name)) return 0;
        } else if (!strcmp(k, "notes")) {
            if (!tc_lit('[')) return 0;
            if (!tc_lit(']')) {
                do {
                    if (c->n >= 16 || !tc_num(&c->notes[c->n])) return 0;
                    c->n++;
                } while (tc_lit(','));
                if (!tc_lit(']')) return 0;
            }
        } else {
            return 0;
        }
    } while (tc_lit(','));
    return tc_lit('}');
}
static int tc_set(tc_jset_t *s)
{
    char k[16];
    s->nch = 0;
    s->set = -1;
    s->genre[0] = '\0';
    if (!tc_lit('{')) return 0;
    do {
        if (!tc_str(k, sizeof k) || !tc_lit(':')) return 0;
        if (!strcmp(k, "set")) {
            if (!tc_num(&s->set)) return 0;
        } else if (!strcmp(k, "genre")) {
            if (!tc_str(s->genre, sizeof s->genre)) return 0;
        } else if (!strcmp(k, "chords")) {
            if (!tc_lit('[')) return 0;
            do {
                if (s->nch >= 12 || !tc_chord(&s->ch[s->nch])) return 0;
                s->nch++;
            } while (tc_lit(','));
            if (!tc_lit(']')) return 0;
        } else {
            return 0;
        }
    } while (tc_lit(','));
    return tc_lit('}');
}
/* 1 = parsed: tc_sets[0 .. tc_nsets) */
static int tc_load_json(void)
{
    FILE *f = fopen(TC_JSON_PATH, "rb");
    size_t len;
    if (!f) {
        fprintf(stderr, "test_chord: cannot open %s (run from the repository root)\n", TC_JSON_PATH);
        return 0;
    }
    len = fread(tc_buf, 1, sizeof tc_buf - 1, f);
    fclose(f);
    if (len == sizeof tc_buf - 1) {
        fprintf(stderr, "test_chord: %s is too big for the test's buffer\n", TC_JSON_PATH);
        return 0;
    }
    tc_buf[len] = '\0';
    tc_p = tc_buf;
    tc_nsets = 0;
    if (!tc_lit('[')) return 0;
    do {
        if (tc_nsets >= 100 || !tc_set(&tc_sets[tc_nsets])) {
            fprintf(stderr, "test_chord: %s: parse error near byte %ld\n", TC_JSON_PATH, (long)(tc_p - tc_buf));
            return 0;
        }
        tc_nsets++;
    } while (tc_lit(','));
    if (!tc_lit(']')) return 0;
    tc_ws();
    return *tc_p == '\0';
}

/* ---- the generated table ---------------------------------------------------------------------------- */

static void test_table_shape(void)
{
    uint32_t s, i, k, bad_count = 0, bad_pad = 0, bad_order = 0, bad_off = 0, bad_genre = 0;
    CHECK_EQ(J6C_TAB_SETS, J6_CHORD_SETS);
    CHECK_EQ(J6C_TAB_CHORDS, J6_CHORDS);
    CHECK_EQ(J6C_TAB_SLOTS, J6_CHORD_NOTES);
    CHECK_EQ(sizeof j6c_notes, 100u * 12u * 4u);
    CHECK_EQ(sizeof j6c_name_pool, (size_t)J6C_NAME_POOL_LEN);
    CHECK_EQ(sizeof j6c_genre_pool, (size_t)J6C_GENRE_POOL_LEN);
    CHECK_EQ(j6c_name_pool[J6C_NAME_POOL_LEN - 1], '\0');      /* the pools end with a terminator */
    CHECK_EQ(j6c_genre_pool[J6C_GENRE_POOL_LEN - 1], '\0');
    CHECK(sizeof j6c_notes + sizeof j6c_count + sizeof j6c_name_off + sizeof j6c_name_pool +
              sizeof j6c_genre_pool + sizeof j6c_genre_off + sizeof j6c_genre_idx < 16384u); /* ~15 K flash */
    for (s = 0; s < 100; s++) {
        if (j6c_genre_idx[s] >= J6C_TAB_GENRES) bad_genre++;
        for (i = 0; i < 12; i++) {
            uint32_t c = j6c_count[s][i];
            if (c < 2 || c > 4) bad_count++;
            for (k = c; k < 4u; k++)
                if (j6c_notes[s][i][k] != 0) bad_pad++;           /* 0 in an unused slot */
            for (k = 1; k < c && k < 4u; k++)                      /* the data is listed ascending */
                if (j6c_notes[s][i][k] < j6c_notes[s][i][k - 1]) bad_order++;
            if (j6c_name_off[s][i] >= J6C_NAME_POOL_LEN) bad_off++;
            else if (strlen(&j6c_name_pool[j6c_name_off[s][i]]) > 12) bad_off++;
        }
    }
    for (k = 0; k < J6C_TAB_GENRES; k++)
        if (j6c_genre_off[k] >= J6C_GENRE_POOL_LEN || (k > 0 && j6c_genre_off[k] <= j6c_genre_off[k - 1]))
            bad_genre++;
    CHECK_EQ(bad_count, 0);
    CHECK_EQ(bad_pad, 0);
    CHECK_EQ(bad_order, 0);
    CHECK_EQ(bad_off, 0);
    CHECK_EQ(bad_genre, 0);
    /* CH-4: the table keeps the duplicate as listed; j6_chord_raw removes it */
    CHECK_EQ(j6c_count[42][5], 4);
    CHECK_EQ(j6c_notes[42][5][0], 53);
    CHECK_EQ(j6c_notes[42][5][1], 53);
    CHECK_EQ(j6c_notes[42][5][2], 58);
    CHECK_EQ(j6c_notes[42][5][3], 62);
    /* a 2-note chord: padded with 0 */
    CHECK_EQ(j6c_count[13][0], 2);
    CHECK_EQ(j6c_notes[13][0][2], 0);
    CHECK_EQ(j6c_notes[13][0][3], 0);
}

/* Every chord, name and genre against data/chordsets.json (CH-2 [DATA]). */
static void test_against_json(void)
{
    int s, i, k;
    uint32_t bad_notes = 0, bad_raw = 0, bad_names = 0, bad_genres = 0, bad_keys = 0;
    int ok = tc_load_json();
    CHECK(ok);
    if (!ok) return;
    CHECK_EQ(tc_nsets, 100);
    CHECK_EQ(tc_non_ascii, 0);
    for (s = 0; s < tc_nsets; s++) {
        const tc_jset_t *js = &tc_sets[s];
        CHECK_EQ(js->set, s + 1);
        CHECK_EQ(js->nch, 12);
        if (strcmp(j6_chord_genre((uint32_t)s), js->genre) != 0) bad_genres++;
        for (i = 0; i < js->nch; i++) {
            const tc_jchord_t *jc = &js->ch[i];
            uint8_t o[4], want[4];
            uint32_t n, wn = 0;
            if (jc->key != i) bad_keys++;
            if (strcmp(j6_chord_name((uint32_t)s, (uint32_t)i), jc->name) != 0) bad_names++;
            /* the table: exactly as listed */
            if ((int)j6c_count[s][i] != jc->n) bad_notes++;
            for (k = 0; k < jc->n && k < 4; k++)
                if (j6c_notes[s][i][k] != jc->notes[k]) bad_notes++;
            /* j6_chord_raw: the listed notes sorted, duplicates removed (computed here independently) */
            for (k = 0; k < jc->n && k < 4; k++) {
                int v = jc->notes[k], p = 0, q;
                while (p < (int)wn && want[p] < v) p++;
                if (p < (int)wn && want[p] == v) continue;
                for (q = (int)wn; q > p; q--) want[q] = want[q - 1];
                want[p] = (uint8_t)v;
                wn++;
            }
            n = j6_chord_raw((uint32_t)s, (uint32_t)i, o);
            if (n != wn || memcmp(o, want, wn) != 0 || !tc_well_formed(o, n)) bad_raw++;
            /* CH-2: oct 0 and KEY 0 play exactly the listed chord; key == chord index */
            {
                uint8_t c[4];
                uint32_t cn = j6_chord_notes((uint32_t)s, i, 0, 0, c, NULL);
                if (cn != wn || memcmp(c, want, wn) != 0) bad_raw++;
            }
        }
    }
    CHECK_EQ(bad_keys, 0);
    CHECK_EQ(bad_notes, 0);
    CHECK_EQ(bad_raw, 0);
    CHECK_EQ(bad_names, 0);
    CHECK_EQ(bad_genres, 0);
}

/* ---- tools/gen_tables.py on bad input (design 4.2: it fails loudly when the JSON breaks an assumption) --
 * The generator runs on synthetic copies of the schema under build/j6test/ (never data/), through
 * system(), so this needs python3 like run_tests.sh step 4: without it the test is skipped (J6_STRICT=1
 * makes that a failure). The file names carry a per-run tag, so parallel runs sharing build/ do not
 * collide, and are removed at the end. Every rule of design 4.2 has a case that breaks it (exit status 1,
 * the rule named on stderr, no header written) and the accepted limits have one that must pass, so a
 * later edit that loosens or tightens a rule turns this red; test_generator_check does the same for
 * --check, whose failure path run_tests.sh step 4 never takes on the committed header. */

enum {
    TCG_VALID,          /* 100 sets x 12 chords of 48 52 55, named "C", genre "Pop" */
    TCG_EDGES,          /* the accepted limits: set 1 key C 0 127 (2 notes) named "~ ABCDEFGHIJ" (12
                           characters, 0x7E and 0x20), key C# 0 1 2 127 (4 notes), a 31-character genre */
    TCG_DUP_KEPT,       /* set 1 key C: 60 60 64, two distinct notes, like set 43 key F: accepted */
    TCG_ONE_OF_2,       /* set 1 key C: 60 60, one distinct note: j6_chord_raw would return 1 */
    TCG_ONE_OF_4,       /* set 1 key C: 60 60 60 60 */
    TCG_DUP_GENRE,      /* set 1 says "genre" twice: json.loads would keep the last one */
    TCG_DUP_NAME,       /* set 1 key C says "name" twice */
    TCG_DUP_NOTES,      /* set 1 key C says "notes" twice */
    TCG_DUP_SET,        /* set 1 says "set" twice, both 1 */
    TCG_SETS_99,        /* 99 sets */
    TCG_SETS_101,       /* 101 sets */
    TCG_CHORDS_11,      /* set 1 has 11 chords */
    TCG_CHORDS_13,      /* set 1 has 13 chords */
    TCG_NOTES_1,        /* set 1 key C: 60 */
    TCG_NOTES_5,        /* set 1 key C: 48 52 55 59 62 */
    TCG_NOTE_128,       /* set 1 key C: 48 52 128, which a uint8_t table would hold but MIDI not */
    TCG_NOTE_NEG,       /* set 1 key C: -1 52 55 */
    TCG_NOTE_TRUE,      /* set 1 key C: true 52 55 (JSON true is Python True, == 1 and an int) */
    TCG_NOTE_FLOAT,     /* set 1 key C: 48.0 52 55 (== 48) */
    TCG_NAME_13,        /* set 1 key C named "ABCDEFGHIJKLM" */
    TCG_NAME_FLAT,      /* set 1 key C named "B<U+266D>" (raw UTF-8, the manual's flat sign) */
    TCG_NAME_DEL,       /* set 1 key C named "C\u007f": one past the printable range */
    TCG_GENRE_EMPTY,    /* set 1 genre "" */
    TCG_GENRE_TAB,      /* set 1 genre "Pop\t" (a JSON escape) */
    TCG_GENRE_32,       /* set 1 genre of 32 characters */
    TCG_KEY_ORDER,      /* set 1 keys 1, 0, 2 .. 11 */
    TCG_KEY_BOOL,       /* set 1 key C: "key": false (== 0) */
    TCG_SET_ORDER,      /* sets 2, 1, 3 .. 100 */
    TCG_SET_BOOL        /* set 1: "set": true (== 1) */
};

static int tc_gen_write(const char *path, int defect)
{
    FILE *f = fopen(path, "wb");
    int s, k, nsets = defect == TCG_SETS_99 ? 99 : defect == TCG_SETS_101 ? 101 : 100;
    if (!f) return 0;
    fputs("[\n", f);
    for (s = 1; s <= nsets; s++) {
        int first = s == 1, nch = 12;
        const char *genre = "\"Pop\"";
        if (first && defect == TCG_CHORDS_11) nch = 11;
        if (first && defect == TCG_CHORDS_13) nch = 13;
        if (first && defect == TCG_EDGES) genre = "\"Cinematic/Synthwave/House ~ R&B\"";
        if (first && defect == TCG_GENRE_EMPTY) genre = "\"\"";
        if (first && defect == TCG_GENRE_TAB) genre = "\"Pop\\t\"";
        if (first && defect == TCG_GENRE_32) genre = "\"Cinematic/Synthwave/House ~ R&B!\"";
        if (first && defect == TCG_SET_BOOL) fputs(" {\"set\": true", f);
        else fprintf(f, " {\"set\": %d", defect == TCG_SET_ORDER && s <= 2 ? 3 - s : s);
        if (first && defect == TCG_DUP_SET) fputs(", \"set\": 1", f);
        fprintf(f, ", \"genre\": %s", genre);
        if (first && defect == TCG_DUP_GENRE) fputs(", \"genre\": \"Jazz\"", f);
        fputs(", \"chords\": [\n", f);
        for (k = 0; k < nch; k++) {
            int c0 = first && k == 0, c1 = first && k == 1;
            const char *notes = "[48, 52, 55]", *name = "\"C\"";
            char key[16];
            snprintf(key, sizeof key, "%d", defect == TCG_KEY_ORDER && (c0 || c1) ? 1 - k : k);
            if (c0 && defect == TCG_KEY_BOOL) snprintf(key, sizeof key, "false");
            if (c0 && defect == TCG_EDGES) notes = "[0, 127]", name = "\"~ ABCDEFGHIJ\"";
            if (c1 && defect == TCG_EDGES) notes = "[0, 1, 2, 127]";
            if (c0 && defect == TCG_DUP_KEPT) notes = "[60, 60, 64]";
            if (c0 && defect == TCG_ONE_OF_2) notes = "[60, 60]";
            if (c0 && defect == TCG_ONE_OF_4) notes = "[60, 60, 60, 60]";
            if (c0 && defect == TCG_NOTES_1) notes = "[60]";
            if (c0 && defect == TCG_NOTES_5) notes = "[48, 52, 55, 59, 62]";
            if (c0 && defect == TCG_NOTE_128) notes = "[48, 52, 128]";
            if (c0 && defect == TCG_NOTE_NEG) notes = "[-1, 52, 55]";
            if (c0 && defect == TCG_NOTE_TRUE) notes = "[true, 52, 55]";
            if (c0 && defect == TCG_NOTE_FLOAT) notes = "[48.0, 52, 55]";
            if (c0 && defect == TCG_NAME_13) name = "\"ABCDEFGHIJKLM\"";
            if (c0 && defect == TCG_NAME_FLAT) name = "\"B\xe2\x99\xad\"";
            if (c0 && defect == TCG_NAME_DEL) name = "\"C\\u007f\"";
            fprintf(f, "  {\"key\": %s, \"name\": %s", key, name);
            if (c0 && defect == TCG_DUP_NAME) fputs(", \"name\": \"Cm\"", f);
            fprintf(f, ", \"notes\": %s", notes);
            if (c0 && defect == TCG_DUP_NOTES) fputs(", \"notes\": [48, 51, 55]", f);
            fprintf(f, "}%s\n", k < nch - 1 ? "," : "");
        }
        fprintf(f, " ]}%s\n", s < nsets ? "," : "");
    }
    fputs("]\n", f);
    return fclose(f) == 0;
}

/* the whole file into buf (NUL-terminated); 0 if it cannot be read or does not fit */
static int tc_read_file(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    size_t len;
    if (!f) return 0;
    len = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[len] = '\0';
    return len < cap - 1;
}

static int tc_write_file(const char *path, const char *buf, size_t len)
{
    FILE *f = fopen(path, "wb");
    size_t n;
    if (!f) return 0;
    n = fwrite(buf, 1, len, f);
    return (fclose(f) == 0) & (n == len);
}

/* the files of one generator run under build/j6test/, tagged per run */
static struct {
    char json[96], hdr[96], err[96], st[96], log[96];
} tcg;
static char tcg_out[1 << 17];

/* 1 = python3 runs and the paths are set; 0 = the test is skipped (a failure with J6_STRICT=1) */
static int tcg_setup(const char *test)
{
    const char *strict = getenv("J6_STRICT");
    char base[64];
    if (system("python3 -I -c pass >/dev/null 2>&1") != 0) {
        fprintf(stderr, "  skip: %s (no python3)\n", test);
        CHECK(!(strict && strcmp(strict, "1") == 0));
        return 0;
    }
    /* a per-run tag: the time and a stack address (randomised per process) */
    snprintf(base, sizeof base, "build/j6test/chord_gen_%lx",
             (unsigned long)time(NULL) ^ (unsigned long)(uintptr_t)&base);
    snprintf(tcg.json, sizeof tcg.json, "%s.json", base);
    snprintf(tcg.hdr, sizeof tcg.hdr, "%s.h", base);
    snprintf(tcg.err, sizeof tcg.err, "%s.err", base);
    snprintf(tcg.st, sizeof tcg.st, "%s.status", base);
    snprintf(tcg.log, sizeof tcg.log, "%s.out", base);
    return 1;
}

static void tcg_cleanup(void)
{
    remove(tcg.json);
    remove(tcg.hdr);
    remove(tcg.err);
    remove(tcg.st);
    remove(tcg.log);
}

/* python3 -I tools/gen_tables.py [--check] --input tcg.json --output tcg.hdr: its exit status (-1 if
 * unknown); stdout in tcg.log, stderr in tcg.err */
static int tcg_run(int check)
{
    char cmd[640];
    int status = -1;
    snprintf(cmd, sizeof cmd, "python3 -I tools/gen_tables.py %s--input %s --output %s >%s 2>%s; echo $? >%s",
             check ? "--check " : "", tcg.json, tcg.hdr, tcg.log, tcg.err, tcg.st);
    remove(tcg.st);
    CHECK_EQ(system(cmd), 0);
    if (tc_read_file(tcg.st, tcg_out, sizeof tcg_out)) status = atoi(tcg_out);
    return status;
}

/* 1 if file path holds `want` (NUL-terminated); prints what it holds otherwise */
static int tcg_says(const char *path, const char *want)
{
    if (!tc_read_file(path, tcg_out, sizeof tcg_out)) return 0;
    if (strstr(tcg_out, want)) return 1;
    fprintf(stderr, "  %s does not say \"%s\": %s\n", path, want, tcg_out);
    return 0;
}

static void test_generator_input_checks(void)
{
    static const struct {
        int defect, status;
        const char *where, *why;   /* both in stderr when the status is 1; where "" = the input file's
                                      name (errors about the whole file or a repeated key name it) */
    } cases[] = {
        {TCG_VALID, 0, "", ""},
        {TCG_EDGES, 0, "", ""},
        {TCG_DUP_KEPT, 0, "", ""},
        {TCG_ONE_OF_2, 1, "set #1 chord #0:", "1 distinct note"},
        {TCG_ONE_OF_4, 1, "set #1 chord #0:", "1 distinct note"},
        {TCG_DUP_GENRE, 1, "", "duplicate key \"genre\""},
        {TCG_DUP_NAME, 1, "", "duplicate key \"name\""},
        {TCG_DUP_NOTES, 1, "", "duplicate key \"notes\""},
        {TCG_DUP_SET, 1, "", "duplicate key \"set\""},
        {TCG_SETS_99, 1, "", "99 sets, expected 100"},
        {TCG_SETS_101, 1, "", "101 sets, expected 100"},
        {TCG_CHORDS_11, 1, "set #1:", "expected 12 chords, got 11"},
        {TCG_CHORDS_13, 1, "set #1:", "expected 12 chords, got 13"},
        {TCG_NOTES_1, 1, "set #1 chord #0:", "expected 2..4 notes, got [60]"},
        {TCG_NOTES_5, 1, "set #1 chord #0:", "expected 2..4 notes, got [48, 52, 55, 59, 62]"},
        {TCG_NOTE_128, 1, "set #1 chord #0:", "note 128 is not a MIDI note 0..127"},
        {TCG_NOTE_NEG, 1, "set #1 chord #0:", "note -1 is not a MIDI note 0..127"},
        {TCG_NOTE_TRUE, 1, "set #1 chord #0:", "note True is not a MIDI note 0..127"},
        {TCG_NOTE_FLOAT, 1, "set #1 chord #0:", "note 48.0 is not a MIDI note 0..127"},
        {TCG_NAME_13, 1, "set #1 chord #0 name:", "is 13 characters, at most 12 allowed"},
        {TCG_NAME_FLAT, 1, "set #1 chord #0 name:", "only printable ASCII is allowed"},
        {TCG_NAME_DEL, 1, "set #1 chord #0 name:", "only printable ASCII is allowed"},
        {TCG_GENRE_EMPTY, 1, "set #1 genre:", "empty string"},
        {TCG_GENRE_TAB, 1, "set #1 genre:", "only printable ASCII is allowed"},
        {TCG_GENRE_32, 1, "set #1 genre:", "is 32 characters, at most 31 allowed"},
        {TCG_KEY_ORDER, 1, "set #1 chord #0:", "\"key\" is 1, expected 0 (keys in order C..B)"},
        {TCG_KEY_BOOL, 1, "set #1 chord #0:", "\"key\" is False, expected 0"},
        {TCG_SET_ORDER, 1, "set #1:", "\"set\" is 2, expected 1 (sets in order 1..100)"},
        {TCG_SET_BOOL, 1, "set #1:", "\"set\" is True, expected 1"},
    };
    size_t c;

    if (!tcg_setup("test_generator_input_checks")) return;
    for (c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        int status, have_header;
        if (!tc_gen_write(tcg.json, cases[c].defect)) {
            fprintf(stderr, "  cannot write %s (run from the repository root; build/j6test/ must exist)\n", tcg.json);
            CHECK(0);
            break;
        }
        remove(tcg.hdr);
        status = tcg_run(0);
        have_header = tc_read_file(tcg.hdr, tcg_out, sizeof tcg_out);
        if (status != cases[c].status) fprintf(stderr, "  generator case %d: exit status %d\n", (int)c, status);
        CHECK_EQ(status, cases[c].status);
        if (cases[c].status == 0) {
            CHECK(have_header);
            /* the header names every chord that lists a note more than once */
            if (cases[c].defect == TCG_DUP_KEPT) CHECK(have_header && strstr(tcg_out, "set 1 key C (60)") != NULL);
            else CHECK(have_header && strstr(tcg_out, "more than once: none") != NULL);
            if (cases[c].defect == TCG_EDGES) {   /* the limits reach the table as given */
                CHECK(have_header && strstr(tcg_out, "; notes 0..127.") != NULL);
                CHECK(have_header && strstr(tcg_out, "{{0, 127, 0, 0}, {0, 1, 2, 127}, {48, 52, 55, 0},") != NULL);
                CHECK(have_header && strstr(tcg_out, "\"~ ABCDEFGHIJ\\0\"") != NULL);
                CHECK(have_header && strstr(tcg_out, "\"Cinematic/Synthwave/House ~ R&B\\0\"") != NULL);
                CHECK(have_header && strstr(tcg_out, "{2, 4, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3},  /* 1 */") != NULL);
            }
        } else {
            CHECK(!have_header);                     /* nothing written on an error */
            CHECK(tcg_says(tcg.err, "gen_tables.py: ERROR: "));
            CHECK(tcg_says(tcg.err, cases[c].where[0] ? cases[c].where : tcg.json));
            CHECK(tcg_says(tcg.err, cases[c].why));
        }
    }
    tcg_cleanup();
}

/* the stale header body[0 .. blen) under --check: exit 1, "out of date" on stderr, the file left as it was */
static void tcg_check_stale(const char *body, size_t blen)
{
    CHECK(tc_write_file(tcg.hdr, body, blen));
    CHECK_EQ(tcg_run(1), 1);
    CHECK(tcg_says(tcg.err, tcg.hdr));
    CHECK(tcg_says(tcg.err, "is out of date; run: python3 -I tools/gen_tables.py"));
    CHECK(tc_read_file(tcg.log, tcg_out, sizeof tcg_out) && strstr(tcg_out, "up to date") == NULL);
    CHECK(tc_read_file(tcg.hdr, tcg_out, sizeof tcg_out));
    CHECK_EQ(strlen(tcg_out), blen);
    CHECK(memcmp(tcg_out, body, blen) == 0);
}

/* --check: exit 0 only when the header is byte for byte what the generator would write; it never writes */
static void test_generator_check(void)
{
    static char good[1 << 17], stale[(1 << 17) + 1];
    size_t len;

    if (!tcg_setup("test_generator_check")) return;
    if (!tc_gen_write(tcg.json, TCG_VALID)) {
        fprintf(stderr, "  cannot write %s (run from the repository root; build/j6test/ must exist)\n", tcg.json);
        CHECK(0);
        tcg_cleanup();
        return;
    }
    remove(tcg.hdr);
    CHECK_EQ(tcg_run(0), 0);                      /* the header to check against */
    CHECK(tc_read_file(tcg.hdr, good, sizeof good));
    len = strlen(good);
    CHECK(len > 1000);
    if (len <= 1000) {
        tcg_cleanup();
        return;
    }

    /* up to date: exit 0 */
    CHECK_EQ(tcg_run(1), 0);
    CHECK(tcg_says(tcg.log, "is up to date"));

    /* stale: one byte changed in the middle, the last byte dropped, one byte appended */
    memcpy(stale, good, len);
    stale[len / 2] = stale[len / 2] == '0' ? '1' : '0';
    tcg_check_stale(stale, len);
    tcg_check_stale(good, len - 1);
    memcpy(stale, good, len);
    stale[len] = '\n';
    tcg_check_stale(stale, len + 1);

    /* missing: exit 1, and --check does not create it */
    remove(tcg.hdr);
    CHECK_EQ(tcg_run(1), 1);
    CHECK(tcg_says(tcg.err, "is missing; run: python3 -I tools/gen_tables.py"));
    CHECK(!tc_read_file(tcg.hdr, tcg_out, sizeof tcg_out));

    /* bad input under --check: the data error, exit 1, the existing header untouched */
    CHECK(tc_write_file(tcg.hdr, good, len));
    CHECK(tc_gen_write(tcg.json, TCG_NOTE_128));
    CHECK_EQ(tcg_run(1), 1);
    CHECK(tcg_says(tcg.err, "note 128 is not a MIDI note 0..127"));
    CHECK(tc_read_file(tcg.hdr, tcg_out, sizeof tcg_out));
    CHECK(strcmp(tcg_out, good) == 0);
    tcg_cleanup();
}

/* ---- j6_chord_raw ---------------------------------------------------------------------------------- */

static void test_raw_spot(void)
{
    /* J-6 set N is index N-1; the chord index is the key C .. B (CH-2) */
    CHECK_RAW(0, 0, 48, 55, 62, 64);           /* set 1 Pop, key C: Cadd9 = C3 G3 D4 E4 */
    CHECK_RAW(0, 11, 47, 59, 62, 67);          /* set 1 key B: G/B */
    CHECK_RAW(1, 0, 48, 52, 59, 62);           /* set 2 key C: CM9 (manual: D4 B3 E3 C3) */
    CHECK_RAW(1, 1, 49, 55, 58, 64);           /* set 2 key C#: C#dim7 */
    CHECK_RAW(6, 0, 48, 64, 67, 72);           /* set 7 Trad Maj key C: C */
    CHECK_RAW(8, 8, 44, 60, 63, 68);           /* set 9 key G#: the manual lists C4 D#4 G#4 G#2 */
    CHECK_RAW(13, 0, 60, 72);                  /* set 14 Oct Stack: 2 notes */
    CHECK_RAW(13, 11, 71, 83);
    CHECK_RAW(14, 0, 60, 65);                  /* set 15 4th Stack */
    CHECK_RAW(15, 0, 60, 67);                  /* set 16 5th Stack */
    CHECK_RAW(16, 0, 48, 52, 55);              /* set 17 Utility: 3 notes */
    CHECK_RAW(26, 11, 76, 83, 98);             /* set 27 key B: the highest note, 98 */
    CHECK_RAW(28, 1, 29, 41, 60, 64);          /* set 29 key C#: the lowest note, 29 */
    CHECK_RAW(29, 0, 48, 55, 63);              /* set 30 mixes sizes: key C 3 notes ... */
    CHECK_RAW(29, 4, 55, 58, 62, 67);          /* ... key E 4 notes */
    CHECK_RAW(42, 2, 44, 58, 63, 67);          /* set 43 key D: D#/G#, a chord over G# */
    CHECK_RAW(42, 5, 53, 58, 62);              /* CH-4: set 43 key F lists 53 53 58 62 */
    CHECK_RAW(47, 8, 32, 44, 51, 60);          /* set 48 Trance key G# */
    CHECK_RAW(49, 6, 57, 64, 67, 72);          /* set 50 House key F#: Am7 */
    CHECK_RAW(63, 9, 57, 64, 66, 73);          /* set 64 EDM key A: A6 */
    CHECK_RAW(76, 3, 51, 58, 61, 66);          /* set 77 Neo-Soul key D#: D#m7 */
    CHECK_RAW(90, 4, 47, 55, 62);              /* set 91 Jazz key E: G/B, the 3-note chord of a 4-note set */
    CHECK_RAW(94, 0, 64, 65, 69);              /* set 95 Classical key C: FM7/E */
    CHECK_RAW(94, 3, 56, 59, 65, 74);
    CHECK_RAW(98, 7, 55, 60, 62, 71);          /* set 99 key G: CM9 (no3)/G */
    CHECK_RAW(99, 0, 48, 62, 64, 71);          /* set 100 Modern key C: CM9 */
    CHECK_RAW(99, 11, 58, 67, 74, 81);         /* set 100 key B: BbM13 */
}

static void test_raw_edges(void)
{
    uint8_t o[4] = {1, 2, 3, 4};
    CHECK_EQ(j6_chord_raw(100, 0, o), 0);      /* sets are 0..99: the J-6 shows 1..100 */
    CHECK_EQ(o[0], J6_NO_NOTE);                /* out is cleared even when nothing is returned */
    CHECK_EQ(o[3], J6_NO_NOTE);
    CHECK_EQ(j6_chord_raw(0, 12, o), 0);       /* chord index 0..11 */
    CHECK_EQ(j6_chord_raw(UINT32_MAX, 0, o), 0);
    CHECK_EQ(j6_chord_raw(0, UINT32_MAX, o), 0);
    CHECK_EQ(j6_chord_raw(99, 11, o), 4);      /* the last chord */
    CHECK_EQ(j6_chord_raw(42, 5, NULL), 3);    /* out may be NULL: just the count */
    CHECK_EQ(j6_chord_raw(13, 0, NULL), 2);
    CHECK_EQ(j6_chord_raw(100, 0, NULL), 0);
}

/* CH-2 facts from the data, after the CH-4 dedup */
static void test_raw_statistics(void)
{
    uint32_t s, i, by_size[5] = {0, 0, 0, 0, 0}, listed[5] = {0, 0, 0, 0, 0}, root_match = 0, changed = 0;
    uint32_t lo = 255, hi = 0, bad = 0;
    for (s = 0; s < 100; s++)
        for (i = 0; i < 12; i++) {
            uint8_t o[4];
            uint32_t n = j6_chord_raw(s, i, o), k;
            if (n < 2 || n > 4 || !tc_well_formed(o, n)) {
                bad++;
                continue;
            }
            by_size[n]++;
            listed[j6c_count[s][i]]++;
            if (n != j6c_count[s][i]) changed++;
            for (k = 0; k < n; k++) {
                if (o[k] < lo) lo = o[k];
                if (o[k] > hi) hi = o[k];
            }
            if (o[0] % 12u == i) root_match++;
        }
    CHECK_EQ(bad, 0);
    CHECK_EQ(by_size[4], 1083);
    CHECK_EQ(by_size[3], 81);
    CHECK_EQ(by_size[2], 36);
    CHECK_EQ(listed[4], 1084);                 /* as listed: 1084 / 80 / 36 */
    CHECK_EQ(listed[3], 80);
    CHECK_EQ(listed[2], 36);
    CHECK_EQ(changed, 1);                      /* set 43 key F is the only chord with a duplicate */
    CHECK_EQ(lo, 29);
    CHECK_EQ(hi, 98);
    CHECK_EQ(root_match, 735);                 /* the key is an index, not a root */

    /* sets 14-16 are all 2-note; 17, 18, 27, 45, 46, 79 all 3-note; 30, 91, 95 mix sizes */
    {
        static const uint32_t two[] = {14, 15, 16}, three[] = {17, 18, 27, 45, 46, 79}, mixed[] = {30, 91, 95};
        uint32_t k;
        for (k = 0; k < 3; k++)
            for (i = 0; i < 12; i++) CHECK_EQ(j6_chord_raw(two[k] - 1, i, NULL), 2);
        for (k = 0; k < 6; k++)
            for (i = 0; i < 12; i++) CHECK_EQ(j6_chord_raw(three[k] - 1, i, NULL), 3);
        for (k = 0; k < 3; k++) {
            uint32_t n3 = 0, n4 = 0;
            for (i = 0; i < 12; i++) {
                uint32_t n = j6_chord_raw(mixed[k] - 1, i, NULL);
                n3 += n == 3;
                n4 += n == 4;
            }
            CHECK(n3 > 0 && n4 > 0 && n3 + n4 == 12);
        }
        /* and no other set mixes sizes */
        for (s = 0; s < 100; s++) {
            uint32_t n0 = j6_chord_raw(s, 0, NULL), same = 1;
            for (i = 1; i < 12; i++)
                if (j6_chord_raw(s, i, NULL) != n0) same = 0;
            if (s == 29 || s == 90 || s == 94) CHECK(!same);
            else if (s == 42) CHECK(!same);    /* set 43: only through the CH-4 dedup */
            else CHECK(same);
        }
    }
}

/* ---- j6_key_note ------------------------------------------------------------------------------------- */

static void test_key_note(void)
{
    int32_t key, oct, tr;
    uint32_t bad = 0, minus1 = 0, n = 0;
    int32_t lo = 1000, hi = -1000;

    /* KB-1 base note [ASSUMED 60]: the J-6 keys 0..12 at octave 0 */
    CHECK_EQ(j6_key_note(0, 0, 0), 60);
    CHECK_EQ(j6_key_note(12, 0, 0), 72);
    CHECK_EQ(j6_key_note(4, 0, 0), 64);
    CHECK_EQ(j6_key_note(7, 0, 0), 67);
    /* the FM-1 keyboard F3..G5: bit b of fm1_in.notes is key b - 7 */
    CHECK_EQ(j6_key_note(0 - 7, 0, 0), 53);    /* bit 0 = F3 */
    CHECK_EQ(j6_key_note(7 - 7, 0, 0), 60);    /* bit 7 = C4 */
    CHECK_EQ(j6_key_note(26 - 7, 0, 0), 79);   /* bit 26 = G5 */
    CHECK_EQ(J6_KEY_MAX - J6_KEY_MIN + 1, J6_KEY_COUNT);

    /* every FM-1 key, octave -3..3 (KB-3), KEY -12..12 (KB-4) */
    for (key = J6_KEY_MIN; key <= J6_KEY_MAX; key++)
        for (oct = J6_OCT_MIN; oct <= J6_OCT_MAX; oct++)
            for (tr = J6_KEYTR_MIN; tr <= J6_KEYTR_MAX; tr++) {
                int32_t v = j6_key_note(key, oct, tr), want = 60 + key + 12 * oct + tr;
                n++;
                if (v != want) bad++;
                if (v < lo) lo = v;
                if (v > hi) hi = v;
            }
    CHECK_EQ(n, 27u * 7u * 25u);
    CHECK_EQ(bad, 0);
    CHECK_EQ(lo, 5);                           /* the whole range fits in 0..127: no -1 here ... */
    CHECK_EQ(hi, 127);
    CHECK_EQ(j6_key_note(J6_KEY_MAX, J6_OCT_MAX, J6_KEYTR_MAX), 127);   /* ... just */
    CHECK_EQ(j6_key_note(J6_KEY_MIN, J6_OCT_MIN, J6_KEYTR_MIN), 5);

    /* KB-6: wider octave / KEY values reach the -1 cases */
    bad = 0;
    for (key = J6_KEY_MIN; key <= J6_KEY_MAX; key++)
        for (oct = -12; oct <= 12; oct++)
            for (tr = -130; tr <= 130; tr++) {
                int32_t v = j6_key_note(key, oct, tr), want = 60 + key + 12 * oct + tr;
                if (want < 0 || want > 127) {
                    minus1++;
                    if (v != -1) bad++;
                } else if (v != want) {
                    bad++;
                }
            }
    CHECK_EQ(bad, 0);
    CHECK(minus1 > 1000);
    CHECK_EQ(j6_key_note(J6_KEY_MAX, J6_OCT_MAX, J6_KEYTR_MAX + 1), -1);  /* 128 */
    CHECK_EQ(j6_key_note(J6_KEY_MAX, J6_OCT_MAX + 1, 0), 127);            /* 60 + 19 + 48 */
    CHECK_EQ(j6_key_note(J6_KEY_MAX, J6_OCT_MAX + 1, 1), -1);
    CHECK_EQ(j6_key_note(J6_KEY_MAX, J6_OCT_MAX + 1, -1), 126);
    CHECK_EQ(j6_key_note(0, -5, 0), 0);                                    /* C-1 */
    CHECK_EQ(j6_key_note(0, -5, -1), -1);
    CHECK_EQ(j6_key_note(-7, -5, 7), 0);
    CHECK_EQ(j6_key_note(-7, -5, 6), -1);
    /* no overflow on absurd inputs (UBSan in J6_SANITIZE=1 runs) */
    CHECK_EQ(j6_key_note(INT32_MAX, INT32_MAX, INT32_MAX), -1);
    CHECK_EQ(j6_key_note(INT32_MIN, INT32_MIN, INT32_MIN), -1);
    CHECK_EQ(j6_key_note(INT32_MAX, 0, INT32_MIN), 59);                   /* 60 + (-1) */
    CHECK_EQ(j6_key_note(0, 1000000, -12000000), 60);
}

/* ---- j6_chord_notes ------------------------------------------------------------------------------------ */

/* every set, every FM-1 key, octave -3..3, KEY -12..12 against the independent expectation */
static void test_chord_notes_grid(void)
{
    uint32_t s, bad = 0, bad_tag = 0, bad_form = 0, dropped = 0, empty = 0, empty_corner = 0, calls = 0;
    int32_t key, oct, tr;
    for (s = 0; s < 100; s++)
        for (key = J6_KEY_MIN; key <= J6_KEY_MAX; key++)
            for (oct = J6_OCT_MIN; oct <= J6_OCT_MAX; oct++)
                for (tr = J6_KEYTR_MIN; tr <= J6_KEYTR_MAX; tr++) {
                    uint8_t o[4] = {0, 0, 0, 0}, w[4], tag = 0;
                    uint32_t n = j6_chord_notes(s, key, oct, tr, o, &tag);
                    uint32_t wn = tc_expect(s, key, oct, tr, w);
                    int32_t shift = oct + j6_key_floor12(key);
                    calls++;
                    if (n != wn || memcmp(o, w, 4) != 0) {
                        if (bad++ < 5)
                            fprintf(stderr, "  set %u key %d oct %d tr %d: %u notes, want %u\n", (unsigned)s,
                                    (int)key, (int)oct, (int)tr, (unsigned)n, (unsigned)wn);
                    }
                    if (!tc_well_formed(o, n)) bad_form++;
                    if (n < j6_chord_raw(s, j6_key_chord_idx(key), NULL)) dropped++;
                    if (n == 0) {
                        if (key < 0 && oct == J6_OCT_MIN && tr < 0) empty_corner++;
                        else empty++;
                    }
                    if (tag != j6_chord_tag(j6_key_chord_idx(key), shift) || j6_chord_tag_key(tag, oct) != key ||
                        j6_chord_tag_idx(tag) != j6_key_chord_idx(key) || j6_chord_tag_oct(tag) != shift)
                        bad_tag++;
                }
    CHECK_EQ(calls, 100u * 27u * 7u * 25u);
    CHECK_EQ(bad, 0);
    CHECK_EQ(bad_form, 0);
    CHECK_EQ(bad_tag, 0);
    CHECK(dropped > 0);                        /* KB-6 happens inside the real ranges ... */
    CHECK(empty_corner > 0);                   /* ... and takes whole chords only on the FM-1 keys below C
                                                  at octave -3 with KEY < 0 */
    CHECK_EQ(empty, 0);
}

static void test_chord_notes_shifts(void)
{
    uint8_t o[4], r[4], tag;
    uint32_t n, s, k, bad = 0;

    /* CH-2 [ASSUMED]: oct 0, KEY 0 = the listed notes */
    n = j6_chord_notes(0, 0, 0, 0, o, &tag);
    CHECK_EQ(n, 4);
    CHECK(o[0] == 48 && o[1] == 55 && o[2] == 62 && o[3] == 64);
    CHECK_EQ(tag, j6_chord_tag(0, 0));
    CHECK_EQ(j6_chord_tag_key(tag, 0), 0);

    /* CH-5: key 12 (the J-6's 13th key) = the C chord one octave up */
    n = j6_chord_notes(0, 12, 0, 0, o, &tag);
    CHECK_EQ(n, 4);
    CHECK(o[0] == 60 && o[1] == 67 && o[2] == 74 && o[3] == 76);
    CHECK_EQ(j6_chord_tag_idx(tag), 0);
    CHECK_EQ(j6_chord_tag_oct(tag), 1);
    CHECK_EQ(j6_chord_tag_key(tag, 0), 12);
    CHECK(tag != j6_chord_tag(0, 0));          /* the step remembers which C key entered it */

    /* the FM-1's keys below C: key -7 (F3) = the F chord one octave down */
    n = j6_chord_notes(0, -7, 0, 0, o, &tag);   /* set 1 key F: FM9 41 57 64 67 */
    CHECK_EQ(n, 4);
    CHECK(o[0] == 29 && o[1] == 45 && o[2] == 52 && o[3] == 55);
    CHECK_EQ(j6_chord_tag_idx(tag), 5);
    CHECK_EQ(j6_chord_tag_oct(tag), -1);
    CHECK_EQ(j6_chord_tag_key(tag, 0), -7);
    n = j6_chord_notes(0, -1, 0, 0, o, &tag);   /* key -1 (B3) = the B chord -12: 35 47 50 55 */
    CHECK_EQ(n, 4);
    CHECK(o[0] == 35 && o[1] == 47 && o[2] == 50 && o[3] == 55);
    CHECK_EQ(j6_chord_tag_key(tag, 0), -1);
    n = j6_chord_notes(0, 19, 0, 0, o, &tag);   /* key 19 (G5) = the G chord +12: 55 74 76 79 */
    CHECK_EQ(n, 4);
    CHECK(o[0] == 55 && o[1] == 74 && o[2] == 76 && o[3] == 79);
    CHECK_EQ(j6_chord_tag_idx(tag), 7);
    CHECK_EQ(j6_chord_tag_oct(tag), 1);

    /* KB-3 [MANUAL FM]: the octave buttons move chords by 12; KB-4 [MANUAL CM, FM]: KEY transposes chords
     * (CH-3: chord_notes + key_tr + 12*oct) */
    for (s = 0; s < 100; s++)
        for (k = 0; k < 12; k++) {
            uint32_t rn = j6_chord_raw(s, k, r), i;
            int32_t oct, tr;
            for (oct = -2; oct <= 2; oct++)
                for (tr = -12; tr <= 12; tr++) {
                    uint32_t m = j6_chord_notes(s, (int32_t)k, oct, tr, o, NULL);
                    int32_t off = 12 * oct + tr;
                    int fits = 1;
                    for (i = 0; i < rn; i++)
                        if ((int32_t)r[i] + off < 0 || (int32_t)r[i] + off > 127) fits = 0;
                    if (!fits) continue;
                    if (m != rn) bad++;
                    for (i = 0; i < rn && i < m; i++)
                        if ((int32_t)o[i] != (int32_t)r[i] + off) bad++;
                }
        }
    CHECK_EQ(bad, 0);
    n = j6_chord_notes(42, 5, 1, -3, o, NULL);  /* set 43 key F (53 58 62) +12 -3 */
    CHECK_EQ(n, 3);
    CHECK(o[0] == 62 && o[1] == 67 && o[2] == 71 && o[3] == J6_NO_NOTE);
    n = j6_chord_notes(42, 2, -1, 5, o, NULL);  /* set 43 key D (44 58 63 67) -12 +5 */
    CHECK_EQ(n, 4);
    CHECK(o[0] == 37 && o[1] == 51 && o[2] == 56 && o[3] == 60);

    /* the octave of a chord key is the keyboard octave plus the key's own octave */
    for (k = 0; k < 12; k++) {
        uint8_t a[4], b[4];
        uint32_t na = j6_chord_notes(5, (int32_t)k + 12, -1, 0, a, NULL);
        uint32_t nb = j6_chord_notes(5, (int32_t)k, 0, 0, b, NULL);
        CHECK_EQ(na, nb);
        CHECK_MEM(a, b, 4);
    }
}

/* KB-6: notes that leave 0..127 are dropped, the rest stay ascending */
static void test_chord_notes_range(void)
{
    uint8_t o[4] = {0, 0, 0, 0}, tag = 0;
    uint32_t n;

    /* set 27 key B (76 83 98): +3 octaves = 112 119 134 -> 134 dropped */
    n = j6_chord_notes(26, 11, 3, 0, o, &tag);
    CHECK_EQ(n, 2);
    CHECK(o[0] == 112 && o[1] == 119 && o[2] == J6_NO_NOTE && o[3] == J6_NO_NOTE);
    CHECK_EQ(j6_chord_tag_key(tag, 3), 11);
    n = j6_chord_notes(26, 11, 3, 8, o, NULL);   /* 120 127 (142) */
    CHECK_EQ(n, 2);
    CHECK(o[0] == 120 && o[1] == 127);
    n = j6_chord_notes(26, 11, 3, 9, o, NULL);   /* 121 (128) (143) */
    CHECK_EQ(n, 1);
    CHECK(o[0] == 121 && o[1] == J6_NO_NOTE);
    n = j6_chord_notes(26, 11, 2, 0, o, NULL);   /* 100 107 122: all fit */
    CHECK_EQ(n, 3);
    n = j6_chord_notes(26, 19, 3, 12, o, &tag);  /* key G5 = set 27 key G +12, octave 3, KEY 12 */
    CHECK(n < j6_chord_raw(26, 7, NULL));
    CHECK(tc_well_formed(o, n));
    CHECK_EQ(j6_chord_tag_key(tag, 3), 19);

    /* set 29 key C# (29 41 60 64): low end */
    n = j6_chord_notes(28, 1, -3, -12, o, NULL); /* -19 -7 12 16 */
    CHECK_EQ(n, 2);
    CHECK(o[0] == 12 && o[1] == 16 && o[2] == J6_NO_NOTE);
    n = j6_chord_notes(28, 1, -2, -5, o, NULL);  /* 0 12 31 35: 0 is a note */
    CHECK_EQ(n, 4);
    CHECK(o[0] == 0 && o[1] == 12 && o[2] == 31 && o[3] == 35);
    n = j6_chord_notes(28, 1, -2, -6, o, NULL);  /* -1 dropped */
    CHECK_EQ(n, 3);
    CHECK(o[0] == 11 && o[1] == 30 && o[2] == 34);
    n = j6_chord_notes(28, -11, -1, -5, o, NULL); /* key -11 = C# -12, octave -1: as oct -2 */
    CHECK_EQ(n, 4);
    CHECK_EQ(o[0], 0);

    /* everything out of range: 0 notes, but the tag still names the key */
    n = j6_chord_notes(0, 0, 10, 0, o, &tag);
    CHECK_EQ(n, 0);
    CHECK(o[0] == J6_NO_NOTE && o[3] == J6_NO_NOTE);
    CHECK_EQ(j6_chord_tag_idx(tag), 0);
    CHECK_EQ(j6_chord_tag_oct(tag), 7);         /* j6_chord_tag saturates the octave at 7 */
    n = j6_chord_notes(0, 0, -10, 0, o, &tag);
    CHECK_EQ(n, 0);
    CHECK_EQ(j6_chord_tag_oct(tag), -8);
    n = j6_chord_notes(0, 0, 0, 200, o, NULL);
    CHECK_EQ(n, 0);
    n = j6_chord_notes(0, 0, 0, -200, o, NULL);
    CHECK_EQ(n, 0);
}

static void test_chord_notes_edges(void)
{
    uint8_t o[4] = {9, 9, 9, 9}, r[4], tag = 0x5A;
    uint32_t n;

    /* set out of range: nothing, out cleared; the tag is a property of the key and still written */
    n = j6_chord_notes(100, 3, 0, 0, o, &tag);
    CHECK_EQ(n, 0);
    CHECK(o[0] == J6_NO_NOTE && o[1] == J6_NO_NOTE && o[2] == J6_NO_NOTE && o[3] == J6_NO_NOTE);
    CHECK_EQ(tag, j6_chord_tag(3, 0));
    CHECK_EQ(j6_chord_notes(UINT32_MAX, 0, 0, 0, o, NULL), 0);

    /* NULL out and NULL tag */
    CHECK_EQ(j6_chord_notes(42, 5, 0, 0, NULL, NULL), 3);
    CHECK_EQ(j6_chord_notes(26, 11, 3, 0, NULL, &tag), 2);
    CHECK_EQ(j6_chord_tag_key(tag, 3), 11);

    /* the module's floor is right for any key, also below j6_key_floor12's -120 limit */
    n = j6_chord_notes(0, -121, 11, 0, o, &tag);  /* -121 = -11 * 12 + 11: the B chord at shift 0 */
    CHECK_EQ(n, j6_chord_raw(0, 11, r));
    CHECK_MEM(o, r, 4);
    CHECK_EQ(j6_chord_tag_idx(tag), 11);
    CHECK_EQ(j6_chord_tag_oct(tag), 0);
    n = j6_chord_notes(0, -1200, 100, 0, o, &tag); /* compensating extremes: exactly the C chord */
    CHECK_EQ(n, 4);
    CHECK_EQ(o[0], 48);
    CHECK_EQ(tag, j6_chord_tag(0, 0));
    n = j6_chord_notes(7, 5, 1000, -12000, o, NULL);
    CHECK_EQ(n, j6_chord_raw(7, 5, r));
    CHECK_MEM(o, r, 4);
    for (n = 0; n < 300; n++) {                   /* same chord index as j6.h where j6.h is valid */
        int32_t key = (int32_t)n - 120;
        j6_chord_notes(0, key, 0, 0, NULL, &tag);
        CHECK_EQ(j6_chord_tag_idx(tag), j6_key_chord_idx(key));
    }

    /* no overflow on absurd inputs (UBSan in J6_SANITIZE=1 runs) */
    CHECK_EQ(j6_chord_notes(0, INT32_MIN, 0, 0, o, &tag), 0);
    CHECK_EQ(j6_chord_tag_idx(tag), 4);           /* -2147483648 = -178956971 * 12 + 4 */
    CHECK_EQ(j6_chord_tag_oct(tag), -8);
    CHECK_EQ(j6_chord_notes(0, INT32_MAX, 0, 0, o, &tag), 0);
    CHECK_EQ(j6_chord_tag_idx(tag), 7);           /* 2147483647 = 178956970 * 12 + 7 */
    CHECK_EQ(j6_chord_tag_oct(tag), 7);
    /* the tag of an absurd call: the int64 octave shift must be clamped before it is narrowed to int32,
     * or INT32_MAX + 178956970 wraps negative and the tag says octave -8 instead of 7 (and the reverse) */
    tag = 0x5A;
    CHECK_EQ(j6_chord_notes(0, INT32_MAX, INT32_MAX, INT32_MAX, o, &tag), 0);
    CHECK_EQ(j6_chord_tag_idx(tag), 7);           /* shift INT32_MAX + 178956970 */
    CHECK_EQ(j6_chord_tag_oct(tag), 7);
    tag = 0x5A;
    CHECK_EQ(j6_chord_notes(0, INT32_MIN, INT32_MIN, INT32_MIN, o, &tag), 0);
    CHECK_EQ(j6_chord_tag_idx(tag), 4);           /* shift INT32_MIN - 178956971 */
    CHECK_EQ(j6_chord_tag_oct(tag), -8);
    tag = 0x5A;
    CHECK_EQ(j6_chord_notes(0, 0, INT32_MIN, INT32_MAX, o, &tag), 0);
    CHECK_EQ(j6_chord_tag_idx(tag), 0);           /* shift INT32_MIN exactly */
    CHECK_EQ(j6_chord_tag_oct(tag), -8);
    tag = 0x5A;
    CHECK_EQ(j6_chord_notes(0, 11, INT32_MAX, INT32_MIN, o, &tag), 0);
    CHECK_EQ(j6_chord_tag_idx(tag), 11);          /* shift INT32_MAX exactly */
    CHECK_EQ(j6_chord_tag_oct(tag), 7);
    CHECK(tc_well_formed(o, 0));
}

/* ---- names ------------------------------------------------------------------------------------------- */

static void test_names(void)
{
    uint32_t s, i, bad = 0;
    CHECK_STR(j6_chord_name(0, 0), "Cadd9");
    CHECK_STR(j6_chord_name(0, 1), "C#M9/C");
    CHECK_STR(j6_chord_name(0, 11), "G/B");
    CHECK_STR(j6_chord_name(1, 3), "D# dim7");
    CHECK_STR(j6_chord_name(42, 2), "D#/G#");
    CHECK_STR(j6_chord_name(42, 5), "A#/F");
    CHECK_STR(j6_chord_name(26, 11), "Em");
    CHECK_STR(j6_chord_name(98, 7), "CM9 (no3)/G");
    CHECK_STR(j6_chord_name(99, 11), "BbM13");
    CHECK_STR(j6_chord_name(13, 0), "");          /* sets 14-16 (the stacks) have no names in the list */
    CHECK_STR(j6_chord_name(14, 11), "");
    CHECK_STR(j6_chord_name(15, 0), "");
    CHECK_STR(j6_chord_name(16, 0), "C");         /* set 17 does */
    CHECK_STR(j6_chord_name(16, 11), "B");
    CHECK_STR(j6_chord_name(100, 0), "");
    CHECK_STR(j6_chord_name(0, 12), "");
    CHECK_STR(j6_chord_name(UINT32_MAX, UINT32_MAX), "");
    for (s = 0; s < 100; s++)
        for (i = 0; i < 12; i++) {
            const char *p = j6_chord_name(s, i);
            size_t k, len = p ? strlen(p) : 99;
            if (len > 12) bad++;
            for (k = 0; k < len && k < 13; k++)
                if (p[k] < 0x20 || p[k] > 0x7E) bad++;
        }
    CHECK_EQ(bad, 0);
    /* deduplicated pool: the same name is the same pointer */
    CHECK(j6_chord_name(1, 0) == j6_chord_name(4, 0));   /* CM9 */
}

static void test_genres(void)
{
    uint32_t s, bad = 0, distinct = 0;
    CHECK_STR(j6_chord_genre(0), "Pop");
    CHECK_STR(j6_chord_genre(2), "Jazz");
    CHECK_STR(j6_chord_genre(5), "Blues");
    CHECK_STR(j6_chord_genre(13), "Oct Stack");
    CHECK_STR(j6_chord_genre(15), "5th Stack");
    CHECK_STR(j6_chord_genre(42), "Synthwave");
    CHECK_STR(j6_chord_genre(64), "Gospel/R&B");
    CHECK_STR(j6_chord_genre(33), "Cinematic/Synthwave");
    CHECK_STR(j6_chord_genre(34), "Cinematic/House");
    CHECK_STR(j6_chord_genre(99), "Modern");
    CHECK_STR(j6_chord_genre(100), "");
    CHECK_STR(j6_chord_genre(UINT32_MAX), "");
    for (s = 0; s < 100; s++) {
        const char *g = j6_chord_genre(s);
        if (!g || !g[0]) bad++;
        if (s == 0 || strcmp(g, j6_chord_genre(s - 1)) != 0) distinct++;
    }
    CHECK_EQ(bad, 0);
    CHECK(distinct >= J6C_TAB_GENRES);
    CHECK(j6_chord_genre(2) == j6_chord_genre(3));       /* one copy per genre */
}

static void test_note_name(void)
{
    char b[8];
    uint32_t n, bad = 0;
#define TC_NAME(note, want)                                                                         \
    do {                                                                                            \
        memset(b, 'x', sizeof b);                                                                   \
        j6_note_name((note), b);                                                                    \
        CHECK_STR(b, want);                                                                         \
        CHECK(b[5] == 'x' && b[6] == 'x');     /* never writes past buf[4] */                       \
    } while (0)
    TC_NAME(60, "C4");                         /* C-1: C4 = 60 (Roland) */
    TC_NAME(61, "C#4");
    TC_NAME(0, "C-1");
    TC_NAME(1, "C#-1");                        /* the longest: 4 characters + NUL */
    TC_NAME(11, "B-1");
    TC_NAME(12, "C0");
    TC_NAME(48, "C3");                         /* the chord list's C3 is 48 */
    TC_NAME(53, "F3");                         /* the FM-1's lowest key */
    TC_NAME(54, "F#3");
    TC_NAME(69, "A4");
    TC_NAME(70, "A#4");
    TC_NAME(79, "G5");                         /* the FM-1's highest key */
    TC_NAME(98, "D7");                         /* the highest chord note */
    TC_NAME(29, "F1");                         /* the lowest */
    TC_NAME(127, "G9");
    TC_NAME(128, "--");
    TC_NAME(255, "--");
    TC_NAME(UINT32_MAX, "--");
#undef TC_NAME
    /* the manual's names for set 1 key C (E4 D4 G3 C3) */
    {
        uint8_t o[4];
        static const char *const want[4] = {"C3", "G3", "D4", "E4"};
        uint32_t k;
        CHECK_EQ(j6_chord_raw(0, 0, o), 4);
        for (k = 0; k < 4; k++) {
            j6_note_name(o[k], b);
            CHECK_STR(b, want[k]);
        }
    }
    /* round trip: every name parses back to its note */
    for (n = 0; n < 128; n++) {
        static const int pcs[7] = {9, 11, 0, 2, 4, 5, 7};   /* A B C D E F G */
        const char *p = b;
        int pc, oct;
        j6_note_name(n, b);
        if (strlen(b) < 2 || strlen(b) > 4 || b[0] < 'A' || b[0] > 'G') {
            bad++;
            continue;
        }
        pc = pcs[b[0] - 'A'];
        p++;
        if (*p == '#') {
            pc++;
            p++;
        }
        oct = atoi(p);
        if (pc + 12 * (oct + 1) != (int)n) bad++;
    }
    CHECK_EQ(bad, 0);
}

int main(void)
{
    RUN(test_table_shape);
    RUN(test_against_json);
    RUN(test_generator_input_checks);
    RUN(test_generator_check);
    RUN(test_raw_spot);
    RUN(test_raw_edges);
    RUN(test_raw_statistics);
    RUN(test_key_note);
    RUN(test_chord_notes_grid);
    RUN(test_chord_notes_shifts);
    RUN(test_chord_notes_range);
    RUN(test_chord_notes_edges);
    RUN(test_names);
    RUN(test_genres);
    RUN(test_note_name);
    return j6t_report("chord");
}
