/* SPDX-License-Identifier: GPL-3.0-only */
/* j6_chord.c - the chord module of the J-6 engine: chord sets, KEY, octave, key -> notes.
 *
 * Design: docs/J6_ENGINE_DESIGN.md 4. Behaviour: KB-1, KB-4, KB-6 and CH-2 .. CH-5 in docs/J6_SPEC.md.
 * No state at all: pure functions over the const tables in j6_chord_tables.h, which tools/gen_tables.py
 * writes from data/chordsets.json. Arithmetic on the caller's key / octave / KEY values is done in 64 bits
 * (add and multiply only), so no input can overflow; there is no division of a 64-bit value. */
#include "j6.h"
#include "j6_chord_tables.h"

/* the generated tables have the shapes j6.h promises */
J6_STATIC_ASSERT(chord_tab_sets, sizeof j6c_notes / sizeof j6c_notes[0] == J6_CHORD_SETS);
J6_STATIC_ASSERT(chord_tab_chords, sizeof j6c_notes[0] / sizeof j6c_notes[0][0] == J6_CHORDS);
J6_STATIC_ASSERT(chord_tab_slots, sizeof j6c_notes[0][0] == J6_CHORD_NOTES);
J6_STATIC_ASSERT(chord_tab_count, sizeof j6c_count == J6_CHORD_SETS * J6_CHORDS);
J6_STATIC_ASSERT(chord_tab_names, sizeof j6c_name_off == J6_CHORD_SETS * J6_CHORDS * 2u);
J6_STATIC_ASSERT(chord_tab_genres, sizeof j6c_genre_idx == J6_CHORD_SETS);
J6_STATIC_ASSERT(chord_tab_pools, sizeof j6c_name_pool == J6C_NAME_POOL_LEN &&
                                      sizeof j6c_genre_pool == J6C_GENRE_POOL_LEN);

#define J6C_NOTE_MAX 127

/* note names for j6_note_name: letter and sharp per pitch class */
static const char j6c_pc_letter[12] = {'C', 'C', 'D', 'D', 'E', 'F', 'F', 'G', 'G', 'A', 'A', 'B'};
static const uint8_t j6c_pc_sharp[12] = {0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0};

/* floor(key / 12) and key mod 12 (0..11) for any int32_t. Same values as j6_key_floor12() and
 * j6_key_chord_idx() in j6.h, which are only valid for key >= -120. */
static int32_t j6c_floor12(int32_t key, uint32_t *idx)
{
    int32_t q = key / 12, r = key % 12;   /* C99: truncates toward zero, r has the sign of key */
    if (r < 0) {
        q--;
        r += 12;
    }
    *idx = (uint32_t)r;
    return q;
}

/* KB-1, KB-4, KB-6: 60 + key + 12*oct + key_tr, -1 outside 0..127 */
int32_t j6_key_note(int32_t key, int32_t oct, int32_t key_tr)
{
    int64_t n = (int64_t)J6_KEY_BASE_NOTE + key + (int64_t)oct * 12 + key_tr;
    return (n < 0 || n > J6C_NOTE_MAX) ? -1 : (int32_t)n;
}

/* CH-2, CH-4: the listed chord, ascending, without duplicates; unused slots of out are J6_NO_NOTE.
 * out may be NULL (only the count is wanted). */
uint32_t j6_chord_raw(uint32_t set, uint32_t idx, uint8_t out[J6_CHORD_NOTES])
{
    uint8_t tmp[J6_CHORD_NOTES];
    uint8_t *o = out ? out : tmp;
    uint32_t i, n = 0, cnt;

    for (i = 0; i < J6_CHORD_NOTES; i++) o[i] = J6_NO_NOTE;
    if (set >= J6_CHORD_SETS || idx >= J6_CHORDS) return 0;
    cnt = j6c_count[set][idx];
    if (cnt > J6_CHORD_NOTES) cnt = J6_CHORD_NOTES;   /* the generator guarantees 2..4 */
    for (i = 0; i < cnt; i++) {
        uint8_t v = j6c_notes[set][idx][i];
        uint32_t p = 0, k;
        while (p < n && o[p] < v) p++;
        if (p < n && o[p] == v) continue;              /* CH-4: set 43 key F lists F3 twice */
        for (k = n; k > p; k--) o[k] = o[k - 1];
        o[p] = v;
        n++;
    }
    return n;
}

/* CH-3, CH-5, KB-6: the notes chord key `key` plays at octave `oct` and KEY `key_tr` */
uint32_t j6_chord_notes(uint32_t set, int32_t key, int32_t oct, int32_t key_tr,
                        uint8_t out[J6_CHORD_NOTES], uint8_t *tag)
{
    uint8_t raw[J6_CHORD_NOTES], tmp[J6_CHORD_NOTES];
    uint8_t *o = out ? out : tmp;
    uint32_t idx, i, n = 0, cnt;
    int64_t shift, off;

    shift = (int64_t)oct + j6c_floor12(key, &idx);     /* CH-5: one octave per 12 keys */
    off = shift * 12 + key_tr;                         /* CH-3 */
    if (tag) {
        /* j6_chord_tag() saturates the octave to -8..7; clamp first so the int32 cannot overflow */
        int32_t t = shift < -9 ? -9 : shift > 8 ? 8 : (int32_t)shift;
        *tag = j6_chord_tag(idx, t);
    }
    cnt = j6_chord_raw(set, idx, raw);
    for (i = 0; i < J6_CHORD_NOTES; i++) o[i] = J6_NO_NOTE;
    for (i = 0; i < cnt; i++) {
        int64_t v = (int64_t)raw[i] + off;
        if (v < 0 || v > J6C_NOTE_MAX) continue;       /* KB-6: dropped */
        o[n++] = (uint8_t)v;                           /* raw is ascending and distinct, so is o */
    }
    return n;
}

const char *j6_chord_name(uint32_t set, uint32_t idx)
{
    if (set >= J6_CHORD_SETS || idx >= J6_CHORDS) return "";
    return &j6c_name_pool[j6c_name_off[set][idx]];
}

const char *j6_chord_genre(uint32_t set)
{
    uint32_t g;
    if (set >= J6_CHORD_SETS) return "";
    g = j6c_genre_idx[set];
    if (g >= J6C_TAB_GENRES) return "";
    return &j6c_genre_pool[j6c_genre_off[g]];
}

/* C-1: C4 = 60, so 0 is C-1 and 127 is G9; "--" above 127 */
void j6_note_name(uint32_t note, char buf[5])
{
    uint32_t k = 0, pc, oct;
    if (!buf) return;
    if (note > (uint32_t)J6C_NOTE_MAX) {
        buf[0] = '-';
        buf[1] = '-';
        buf[2] = '\0';
        return;
    }
    pc = note % 12u;
    oct = note / 12u;                                   /* 0..10 = octave -1..9 */
    buf[k++] = j6c_pc_letter[pc];
    if (j6c_pc_sharp[pc]) buf[k++] = '#';
    if (oct == 0u) {
        buf[k++] = '-';
        buf[k++] = '1';
    } else {
        buf[k++] = (char)('0' + (int)(oct - 1u));
    }
    buf[k] = '\0';
}
