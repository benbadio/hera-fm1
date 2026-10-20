#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Generate firmware/src/j6/j6_synth_tables.h: every coefficient of the J-6 synth module.

    python3 -I tools/gen_synth_tables.py           write the header
    python3 -I tools/gen_synth_tables.py --check   exit 1 if the committed header differs
Both exit 1 if firmware/src/j6/j6_synth_sounds.c does not quote the CUTOFF knob key this generator makes.

Standard library only. The firmware has no FPU, so the synth (firmware/src/j6/j6_synth*.c) only reads
integers: everything that needs exp / tan / pow is computed here. docs/J6_ENGINE_DESIGN.md 7.

Formats (the C code relies on them):
  j6y_inc[128]        phase increment per sample of MIDI note n at A4 = 440 Hz: f * 2^32 / 44100
  j6y_cut_g[134]      filter g = tan(pi f / fs) in Q20 for a cutoff of note n (0..133, 1 semitone apart)
  j6y_cut_knob[128]   CUTOFF knob -> cutoff note in Q8 (note 24 .. 132)
  j6y_res_k[128]      RESO knob -> SVF damping k in Q20 (1.414 = no resonance .. J6Y_K_MIN)
  j6y_att_c[128]      attack: per-16-frame-chunk coefficient Q30 of an exponential toward 1.2
  j6y_dec_c[128]      decay: per-chunk coefficient Q30 toward the sustain level
  j6y_rel_c[128]      release: per-chunk coefficient Q30 toward -1/512 (so it reaches exactly 0)
  j6y_dly_free[128]   DLY_TIME knob (d.Syn off) -> delay in samples, 10 .. 1400 ms
  j6y_rv_fb[128]      REV_TIME knob -> comb feedback Q15, 0.2 .. 0.93
  j6y_rv_comb[8], j6y_rv_ap[4]   reverb line lengths (Freeverb, 4 combs + 2 allpasses per side)
"""
import math
import os
import re
import sys

FS = 44100.0
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "firmware", "src", "j6", "j6_synth_tables.h")
J6_H = os.path.join(HERE, "..", "firmware", "src", "j6", "j6.h")
SOUNDS = os.path.join(HERE, "..", "firmware", "src", "j6", "j6_synth_sounds.c")


def engine_chunk():
    """J6_CR from j6.h: the per-chunk coefficients are only right for the chunk the engine really uses"""
    with open(J6_H, encoding="utf-8") as f:
        m = re.search(r"^#define\s+J6_CR\s+(\d+)u?\b", f.read(), re.M)
    if not m:
        raise SystemExit("gen_synth_tables: no '#define J6_CR' in %s" % os.path.relpath(J6_H))
    n = int(m.group(1))
    if n < 1 or n & (n - 1):
        raise SystemExit("gen_synth_tables: J6_CR %d is not a power of two (the synth ramps shift by log2)" % n)
    return n


CHUNK = engine_chunk()          # J6_CR: control-rate chunk in frames (j6_synth.c asserts J6Y_TABLES_CR)

# ---- design values (the only place they live)
CUT_NOTE_LO, CUT_NOTE_HI = 24, 132      # cutoff knob range: 32.7 Hz .. 16.7 kHz
CUT_NOTES = 134                         # table notes 0..133 (133 is the interpolation guard)
K_NONE = math.sqrt(2.0)                 # SVF damping of a Butterworth section (Q 0.707)
K_MIN = 0.012                           # RESO 127: Q 83, rings for ~1 s, close to self-oscillation
RES_CURVE = 1.3                         # resonance knob curve: most of the travel is musical
ATT_MS = (1.0, 3000.0)                  # attack 0..127
DEC_MS = (5.0, 10000.0)                 # decay 0..127 (preset field)
REL_MS = (5.0, 5000.0)                  # release 0..127
ATT_OVERSHOOT = 1.2                     # attack aims at 1.2 and stops at 1.0 (analog-style curve)
REL_UNDERSHOOT = 1.0 / 512.0            # release aims at -1/512 and stops at exactly 0. Not 1/256:
                                        # -2^22 is 0xFFC00000, a chip ROM address (docs/RULES.md)
DLY_MS = (10.0, 1400.0)                 # free delay time
DLY_FB = 0.40                           # delay feedback
DLY_LP_HZ = 2500.0                      # darkening one-pole in the delay feedback
RV_FB = (0.20, 0.93)                    # REV_TIME range
RV_FB_CURVE = 0.6
RV_DAMP = 0.40                          # comb damping (Freeverb "damp"), fixed
RV_COMB_L = [1116, 1188, 1277, 1356]    # Freeverb comb lengths at 44.1 kHz
RV_AP_L = [556, 441]
RV_SPREAD = 23                          # right side = left + 23 (Freeverb stereo spread)
CH_LFO_HZ = 0.513                       # Juno-60 chorus I rate
CH_CENTER_MS = 3.5                      # chorus delay centre and depth (BBD 1.66 .. 5.35 ms)
CH_DEPTH_MS = 1.85
PW_SPAN = 0.45                          # pulse duty: pw 64 = 50 %, each unit away from 64 narrows by 0.45/64


def q(x, bits):
    return int(math.floor(x * (1 << bits) + 0.5))


def note_hz(n):
    return 440.0 * 2.0 ** ((n - 69) / 12.0)


def chunk_coef(tau_s):
    """exp(-CHUNK / (tau * fs)) in Q30"""
    return q(math.exp(-CHUNK / (tau_s * FS)), 30)


def knob_ms(lo, hi, v):
    return lo * (hi / lo) ** (v / 127.0)


def tables():
    t = []  # (ctype, name, values, comment)
    t.append(("uint32_t", "j6y_inc", [q(note_hz(n) / FS, 32) for n in range(128)],
              "phase increment of MIDI note n at A4 = 440 Hz (x tune_q16 >> 16)"))
    t.append(("int32_t", "j6y_cut_g", [q(math.tan(math.pi * note_hz(n) / FS), 20) for n in range(CUT_NOTES)],
              "SVF g = tan(pi f / fs), Q20, for cutoff note 0..133"))
    t.append(("uint16_t", "j6y_cut_knob",
              [q(cut_knob_note(v), 8) for v in range(128)],
              "CUTOFF knob -> cutoff note, Q8"))
    t.append(("int32_t", "j6y_res_k",
              [q(K_NONE * (K_MIN / K_NONE) ** ((v / 127.0) ** RES_CURVE), 20) for v in range(128)],
              "RESO knob -> SVF damping k (1/Q), Q20"))
    t.append(("uint32_t", "j6y_att_c",
              [chunk_coef(knob_ms(*ATT_MS, v) / 1000.0 / math.log(ATT_OVERSHOOT / (ATT_OVERSHOOT - 1.0)))
               for v in range(128)],
              "attack per-chunk coefficient Q30 (1 ms .. 3 s from 0 to the peak)"))
    t.append(("uint32_t", "j6y_dec_c",
              [chunk_coef(knob_ms(*DEC_MS, v) / 1000.0 / math.log(100.0)) for v in range(128)],
              "decay per-chunk coefficient Q30 (5 ms .. 10 s to 99 % of the way to sustain)"))
    t.append(("uint32_t", "j6y_rel_c",
              [chunk_coef(knob_ms(*REL_MS, v) / 1000.0 /
                          math.log((1.0 + REL_UNDERSHOOT) / REL_UNDERSHOOT)) for v in range(128)],
              "release per-chunk coefficient Q30 (5 ms .. 5 s from full level to exactly 0)"))
    t.append(("uint16_t", "j6y_dly_free",
              [int(math.floor(knob_ms(*DLY_MS, v) * FS / 1000.0 + 0.5)) for v in range(128)],
              "DLY_TIME knob (d.Syn off) -> delay time in samples (10 .. 1400 ms)"))
    t.append(("uint16_t", "j6y_rv_fb",
              [q(RV_FB[0] + (RV_FB[1] - RV_FB[0]) * (v / 127.0) ** RV_FB_CURVE, 15) for v in range(128)],
              "REV_TIME knob -> comb feedback Q15 (0.2 .. 0.93)"))
    t.append(("uint16_t", "j6y_rv_comb", RV_COMB_L + [x + RV_SPREAD for x in RV_COMB_L],
              "comb lengths: 0..3 left, 4..7 right"))
    t.append(("uint16_t", "j6y_rv_ap", RV_AP_L + [x + RV_SPREAD for x in RV_AP_L],
              "allpass lengths: 0..1 left, 2..3 right"))
    t.append(("uint16_t", "j6y_tanh_q15", [int(32767 * math.tanh(i / 256 * 2)) for i in range(257)],
              "tanh(x) Q15 for |x| = i / 128 (0 .. 2): the ladder's input saturation (after Clipper's softclip)"))
    return t


def constants():
    rv_len = sum(RV_COMB_L) * 2 + RV_SPREAD * 4 + sum(RV_AP_L) * 2 + RV_SPREAD * 2
    c = [
        ("J6Y_TABLES_CR", CHUNK, "J6_CR the per-chunk coefficients are computed for"),
        ("J6Y_CUT_NOTE_MAX", CUT_NOTE_HI, "highest cutoff note (the table has one more entry)"),
        ("J6Y_K2_Q20", q(K_NONE, 20), "damping of the second (non-resonant) filter section"),
        ("J6Y_ATT_TGT", q(ATT_OVERSHOOT, 30), "attack target, Q30"),
        ("J6Y_REL_TGT", -q(REL_UNDERSHOOT, 30), "release target, Q30"),
        ("J6Y_DLY_FB_Q15", q(DLY_FB, 15), "delay feedback"),
        ("J6Y_DLY_LP_Q14", q(1.0 - math.exp(-2.0 * math.pi * DLY_LP_HZ / FS), 14),
         "delay feedback one-pole coefficient"),
        ("J6Y_RV_LP_Q15", q(1.0 - RV_DAMP, 15), "comb damping one-pole coefficient (1 - damp)"),
        ("J6Y_RV_LEN", rv_len, "int16_t samples the reverb lines take"),
        ("J6Y_RV_QUIET", max(RV_COMB_L + RV_AP_L) + RV_SPREAD,
         "all-zero writes for this long = every reverb line is empty"),
        ("J6Y_CH_LFO_INC", q(CH_LFO_HZ * CHUNK / FS, 32), "chorus LFO phase step per chunk"),
        ("J6Y_CH_CENTER_Q8", q(CH_CENTER_MS * FS / 1000.0, 8), "chorus delay centre, samples Q8"),
        ("J6Y_CH_DEPTH_Q8", q(CH_DEPTH_MS * FS / 1000.0, 8), "chorus delay depth, samples Q8"),
        ("J6Y_PW_STEP", q(PW_SPAN / 64.0, 32), "pulse duty step per pw unit away from 64 (phase)"),
    ]
    return c


def cut_knob_note(v):
    return CUT_NOTE_LO + (CUT_NOTE_HI - CUT_NOTE_LO) * v / 127.0


def hz_text(f):
    return "%.1f kHz" % (f / 1000.0) if f >= 1000.0 else "%.0f Hz" % f


CUT_KEY_KNOBS = (0, 32, 64, 75, 96, 127)


def cutoff_key():
    """the CUTOFF knob key quoted in j6_synth_sounds.c (the knob alone: no envelope, no key tracking)"""
    return ", ".join("%d = %s" % (v, hz_text(note_hz(cut_knob_note(v)))) for v in CUT_KEY_KNOBS)


def sounds_quote_key():
    """the presets are chosen with the CUTOFF key j6_synth_sounds.c quotes: it must be this generator's
    (a wrong key once made every preset cutoff about 9 semitones darker than meant)"""
    try:
        with open(SOUNDS, encoding="utf-8") as f:
            text = f.read()
    except OSError:
        return True                     # no presets (yet): nothing quotes the key
    flat = " ".join(re.sub(r"^\s*(/\*+|\*+/|\*)\s?", "", line) for line in text.splitlines())
    return cutoff_key() in re.sub(r"\s+", " ", flat)


def render():
    lines = [
        "/* SPDX-License-Identifier: GPL-3.0-only */",
        "/* j6_synth_tables.h - GENERATED by tools/gen_synth_tables.py: do not edit, run the generator.",
        " * Every coefficient of the synth module (docs/J6_ENGINE_DESIGN.md 7.3); the C code is integer only.",
        " * CUTOFF knob key, the knob alone (no envelope, no key tracking; quoted in j6_synth_sounds.c):",
        " *   " + cutoff_key() + ". */",
        "#ifndef J6_SYNTH_TABLES_H",
        "#define J6_SYNTH_TABLES_H",
        "",
        "#include <stdint.h>",
        "",
    ]
    for name, val, com in constants():
        v = "%du" % val if val >= 0 else "(%d)" % val
        if name in ("J6Y_CH_LFO_INC", "J6Y_PW_STEP"):
            v = "%du" % val
        lines.append("#define %-18s %-12s /* %s */" % (name, v, com))
    lines.append("")
    for ctype, name, vals, com in tables():
        lines.append("/* %s */" % com)
        lines.append("static const %s %s[%d] = {" % (ctype, name, len(vals)))
        per = 8
        suffix = "u" if ctype.startswith("uint") else ""
        for i in range(0, len(vals), per):
            chunk = vals[i:i + per]
            lines.append("    " + ", ".join("%d%s" % (x, suffix) for x in chunk) +
                         ("," if i + per < len(vals) else ""))
        lines.append("};")
        lines.append("")
    lines.append("#endif /* J6_SYNTH_TABLES_H */")
    return "\n".join(lines) + "\n"


def check_ranges():
    for ctype, name, vals, _ in tables():
        lim = {"uint16_t": (0, 65535), "int32_t": (-2 ** 31, 2 ** 31 - 1), "uint32_t": (0, 2 ** 32 - 1)}[ctype]
        for x in vals:
            if not lim[0] <= x <= lim[1]:
                raise SystemExit("gen_synth_tables: %s value %d out of %s range" % (name, x, ctype))


def main(argv):
    check_ranges()
    if not sounds_quote_key():
        print("gen_synth_tables: %s does not quote the CUTOFF key '%s'" % (os.path.relpath(SOUNDS), cutoff_key()))
        return 1
    text = render()
    if "--check" in argv[1:]:
        try:
            with open(OUT, encoding="utf-8") as f:
                old = f.read()
        except OSError:
            old = None
        if old != text:
            print("gen_synth_tables: %s is out of date (run python3 -I tools/gen_synth_tables.py)" %
                  os.path.relpath(OUT))
            return 1
        print("gen_synth_tables: %s is up to date" % os.path.relpath(OUT))
        return 0
    with open(OUT, "w", encoding="utf-8") as f:
        f.write(text)
    print("gen_synth_tables: wrote %s" % os.path.relpath(OUT))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
