/* SPDX-License-Identifier: GPL-3.0-only */
/* test_header.c - checks the shared helpers in firmware/src/j6/j6.h against docs/J6_SPEC.md.
 * Owned by the design step; needs no engine source (J6_ISOLATED=1 links nothing else). */
#include "j6_test.h"

static void test_time_base(void)
{
    CHECK_EQ(j6_spt_q12(1200), 940800);            /* C-5: 229.6875 samples, exact */
    CHECK_EQ(j6_spt_q12(1200) >> 12, 229);
    CHECK_EQ(j6_spt_q12(10), j6_spt_q12(J6_TEMPO10_MIN));   /* clamped, never divides by 0 */
    CHECK_EQ(j6_spt_q12(0), j6_spt_q12(J6_TEMPO10_MIN));
    CHECK_EQ(j6_spt_q12(9999), j6_spt_q12(J6_TEMPO10_MAX));
    CHECK(j6_spt_q12(J6_TEMPO10_MAX) >> 12 > J6_CR);        /* at most one tick per control chunk */
    CHECK_EQ(j6_sub_frames(0, 940800), 0);
    CHECK_EQ(j6_sub_frames(8, 940800), 114);                 /* half a tick */
    CHECK(15u * j6_spt_q12(J6_TEMPO10_MIN) < 0x80000000u);   /* no 32-bit overflow in j6_sub_frames */
}

static void test_gate(void)
{
    /* PH-7 table */
    CHECK_EQ(j6_gate(16), 13);
    CHECK_EQ(j6_gate(24), 20);
    CHECK_EQ(j6_gate(32), 26);
    CHECK_EQ(j6_gate(48), 39);
    CHECK_EQ(j6_gate(72), 62);
    CHECK_EQ(j6_gate(96), 86);
    CHECK_EQ(j6_gate(192), 182);
    CHECK_EQ(j6_gate(384), 374);
    CHECK_EQ(j6_gate(768), 758);
    CHECK_EQ(j6_gate(0), 0);
}

static void test_beat(void)
{
    CHECK_EQ(j6_beat_ticks(J6_BEAT_1_4), 24);
    CHECK_EQ(j6_beat_ticks(J6_BEAT_1_3), 32);
    CHECK_EQ(j6_beat_ticks(J6_BEAT_1_2), 48);
    CHECK_EQ(j6_beat_ticks(J6_BEAT_1), 96);
    CHECK_EQ(j6_beat_ticks(J6_BEAT_4), 384);
    CHECK_EQ(j6_beat_ticks(J6_BEAT_8), 768);
    CHECK_EQ(j6_beat_ticks(200), 768);
    CHECK_EQ(64u * j6_beat_ticks(J6_BEAT_8), 49152);          /* SQ-4 longest pattern */
}

static void test_shuffle(void)
{
    CHECK_EQ(j6_shuffle_offset(24u * 16u, 50), 192);          /* half a sixteenth */
    CHECK_EQ(j6_shuffle_offset(24u * 16u, 90), 345);
    CHECK_EQ(j6_shuffle_offset(24u * 16u, -90), -345);
    CHECK_EQ(j6_shuffle_offset(72u * 16u, 50), 192);          /* every off-beat sixteenth */
    CHECK_EQ(j6_shuffle_offset((24u + 2u) * 16u, 50), 192);   /* a strum note after it */
    CHECK_EQ(j6_shuffle_offset(0, 90), 0);
    CHECK_EQ(j6_shuffle_offset(48u * 16u, 90), 0);
    CHECK_EQ(j6_shuffle_offset(16u * 16u, 90), 0);            /* triplet grids are never shifted */
    CHECK_EQ(j6_shuffle_offset(32u * 16u, 90), 0);
    CHECK_EQ(j6_shuffle_offset(64u * 16u, 90), 0);
    CHECK_EQ(j6_shuffle_offset(80u * 16u, 90), 0);
    CHECK_EQ(j6_shuffle_offset(24u * 16u, 0), 0);
}

static void test_phrase_pack(void)
{
    uint32_t s, v;
    for (s = 1; s <= J6_STYLES; s++)
        for (v = 1; v <= J6_VARS; v++) {
            uint8_t p = j6_phr_pack(1, s, v), q = j6_phr_pack(0, s, v);
            CHECK_EQ(j6_phr_style(p), s);
            CHECK_EQ(j6_phr_var(p), v);
            CHECK_EQ(j6_phr_on(p), 1);
            CHECK_EQ(j6_phr_on(q), 0);
            CHECK_EQ(p & 0x7F, q);
        }
    CHECK_EQ(J6_PHR_DEF, j6_phr_pack(0, 1, 1));
    CHECK_EQ(j6_phr_style(0x7F), 1);                          /* garbage decodes to a valid phrase */
    CHECK_EQ(j6_phr_var(0x7F), 1);
    CHECK_EQ(j6_style_cat(1), J6_CAT_ARP);
    CHECK_EQ(j6_style_cat(2), J6_CAT_ARP);
    CHECK_EQ(j6_style_cat(3), J6_CAT_BEAT);
    CHECK_EQ(j6_style_cat(5), J6_CAT_BEAT);
    CHECK_EQ(j6_style_cat(6), J6_CAT_PHRASE);
    CHECK_EQ(j6_style_cat(9), J6_CAT_PHRASE);
    CHECK_EQ(j6_plist_index(6, 1, 1), 0);
    CHECK_EQ(j6_plist_index(9, 12, 4), J6_PLISTS - 1u);
}

static void test_keys_and_tags(void)
{
    int32_t k;
    CHECK_EQ(j6_key_floor12(0), 0);
    CHECK_EQ(j6_key_floor12(11), 0);
    CHECK_EQ(j6_key_floor12(12), 1);
    CHECK_EQ(j6_key_floor12(19), 1);
    CHECK_EQ(j6_key_floor12(-1), -1);
    CHECK_EQ(j6_key_floor12(-7), -1);
    CHECK_EQ(j6_key_chord_idx(12), 0);                         /* CH-5: upper C = C chord */
    CHECK_EQ(j6_key_chord_idx(-7), 5);                         /* FM-1 F3 = F chord, one octave down */
    for (k = J6_KEY_MIN; k <= J6_KEY_MAX; k++) {
        uint8_t tag = j6_chord_tag(j6_key_chord_idx(k), j6_key_floor12(k));
        CHECK_RANGE(j6_key_chord_idx(k), 0, 11);
        CHECK(tag != J6_NO_CHORD);
        CHECK_EQ(j6_chord_tag_key(tag, 0), k);                 /* the tag lights the key that made it */
    }
    CHECK_EQ(j6_chord_tag_key(j6_chord_tag(0, 1), 1), 0);      /* octave +1: key 0 at octave 1 */
    CHECK_EQ(j6_chord_tag_oct(j6_chord_tag(3, -3)), -3);
}

static void test_tables_and_layout(void)
{
    CHECK_EQ(j6_sclk_ppb(0), 1);
    CHECK_EQ(j6_sclk_ppb(7), 24);
    CHECK_EQ(j6_sclk_ppb(99), 2);
    CHECK_EQ(j6_dsync_ticks(0), 12);
    CHECK_EQ(j6_dsync_ticks(11), 192);
    CHECK_EQ(j6_dsync_ticks(99), 192);
    CHECK_EQ(sizeof(j6_step_t), 8);
    CHECK_EQ(sizeof(j6_pattern_t), 536);
    CHECK_EQ(sizeof(j6_event_t), 6);
    CHECK_EQ(sizeof(j6_pev_t), 6);
    CHECK_EQ(sizeof(j6_plist_t), 8);
    CHECK_EQ(sizeof(j6_sound_t), 32);
    CHECK_EQ(sizeof(j6_global_t), 16);
    CHECK(sizeof(j6_global_t) + 1u <= J6_GLOBAL_BYTES);
    CHECK_EQ(J6_PAT_BYTES, 544);
    CHECK_EQ(J6_FX_MEM_LEN * 2u, 163840);
    CHECK(J6_FX_MEM_LEN * 2u < 200u * 1024u);
    CHECK(J6_RAM_SEQ_MAX + J6_RAM_CLOCK_MAX + 2u * J6_RAM_PHRASE_MAX + 2u * J6_RAM_HELD_MAX +
              J6_RAM_SYNTH_MAX + J6_RAM_ENGINE_MAX <= J6_RAM_TOTAL_MAX);
    CHECK_EQ(J6_P_KNOB_LAST - J6_P_KNOB0 + 1, J6_KNOBS);
    CHECK_EQ(J6_SP_REV_TIME, J6_KNOB_REV_TIME);
}

static void test_evq(void)
{
    static j6_evq_t q;
    uint32_t i;
    j6_evq_clear(&q);
    CHECK_EQ(j6_evq_push(&q, J6_EV_NOTE_ON, J6_SRC_SEQ, 3, 60, 100, 0), 0);
    CHECK_EQ(j6_evq_push(&q, J6_EV_NOTE_OFF, J6_SRC_SEQ, 99, 60, 0, 0), 0);
    CHECK_EQ(q.n, 2);
    CHECK_EQ(q.ev[0].sub, 3);
    CHECK_EQ(q.ev[1].sub, 15);                                 /* clamped into the tick */
    CHECK_EQ(j6t_evq_count(&q, J6_EV_NOTE_ON), 1);
    CHECK_EQ(j6t_evq_find(&q, 0, J6_EV_NOTE_OFF, 60), 1);
    for (i = 2; i < J6_EVQ_CAP; i++) CHECK_EQ(j6_evq_push(&q, J6_EV_STEP, 0, 0, 0, 0, i), 0);
    CHECK_EQ(j6_evq_push(&q, J6_EV_STEP, 0, 0, 0, 0, 0), -1);
    CHECK_EQ(q.lost, 1);
    j6_evq_clear(&q);
    CHECK_EQ(q.n, 0);
}

int main(void)
{
    RUN(test_time_base);
    RUN(test_gate);
    RUN(test_beat);
    RUN(test_shuffle);
    RUN(test_phrase_pack);
    RUN(test_keys_and_tags);
    RUN(test_tables_and_layout);
    RUN(test_evq);
    return j6t_report("header");
}
