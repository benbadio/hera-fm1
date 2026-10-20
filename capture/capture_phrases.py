# SPDX-License-Identifier: GPL-3.0-only
"""Capture all 9 x 12 J-6 phrase styles/variations from its MIDI output.

For each STYLE/VARIATION you set the two knobs by hand; the script then holds
1-, 2-, 3- and 4-note chords over MIDI (--mode auto) and records exactly what
the J-6 plays: note numbers, velocities, on/off times, plus any MIDI clock so
timing can be converted to ticks later.

Set the J-6 TEMPO to the --bpm value first (default 120.0).
With --clock ours the script also sends MIDI clock at --bpm (J-6 SYnC=AUTO).

Output: raw/phrases.jsonl, one record per (style, variation, chord size).
Resumable: finished items are skipped (use --redo s3v07n4 to redo one).
"""
import time

import j6cap as c

CHORDS = {
    1: [60],
    2: [60, 67],
    3: [60, 64, 67],
    4: [60, 64, 67, 71],
}


def main():
    ap = c.base_args(__doc__)
    ap.add_argument("--styles", default="1-9")
    ap.add_argument("--bpm", type=float, default=120.0)
    ap.add_argument("--bars", type=float, default=4,
                    help="bars to hold each chord (style 3 uses 2x)")
    ap.add_argument("--sizes", default="1,2,3,4")
    ap.add_argument("--clock", choices=["j6", "ours"], default="j6")
    ap.add_argument("--audio", action="store_true",
                    help="also record USB audio per item (for phrase_mode=audio)")
    a = ap.parse_args()
    lo, hi = (int(x) for x in a.styles.split("-"))
    sizes = [int(x) for x in a.sizes.split(",")]
    store = c.Store("phrases")
    j6 = c.J6(a.port, a.channel)
    rec = c.AudioRec(enabled=a.audio)
    beat_s = 60.0 / a.bpm
    print(f"Phrase capture styles {lo}-{hi}, sizes {sizes}, {a.bpm} BPM, "
          f"clock={a.clock}. {len(store.done)} items already done.")
    print("Before starting on the J-6: STYLE ON, CHORD OFF, HOLD OFF, KEY = C,\n"
          f"  TEMPO {a.bpm:g} if convenient (any tempo works: timing comes from\n"
          "  the J-6's own clock), and MIDI thru OFF (menu SHIFT+B -> thrv).")

    pairs = [(s, v) for s in range(lo, hi + 1) for v in range(1, 13)]
    i = 0
    while i < len(pairs):
        s, v = pairs[i]
        keys = [f"s{s}v{v:02d}n{n}" for n in sizes]
        if all(store.has(k, a.redo) for k in keys):
            i += 1
            continue
        r = c.ask(f"Set STYLE = {s}, VARIATION = {v} (STYLE ON). Then Enter.")
        if r == "skip":
            i += 1
            continue
        if r == "redo" and i > 0:
            i -= 1
            ps, pv = pairs[i]
            a.redo += [f"s{ps}v{pv:02d}n{n}" for n in sizes]
            continue
        hold_s = a.bars * 4 * beat_s * (2 if s == 3 else 1)
        for n, key in zip(sizes, keys):
            if store.has(key, a.redo):
                continue
            notes = CHORDS[n]
            clk = c.ClockSender(j6, a.bpm) if a.clock == "ours" else None
            if clk:
                clk.start()
                time.sleep(4 * beat_s)          # let the J-6 lock to tempo
            j6.clear()
            t0 = c.now_ns()
            marks = {}

            def play():
                marks["on"] = (c.now_ns() - t0) // 1000
                for k in notes:
                    j6.note_on(k, 100)
                time.sleep(hold_s)
                marks["off"] = (c.now_ns() - t0) // 1000
                for k in notes:
                    j6.note_off(k)
                time.sleep(1.5)                  # catch release behaviour

            wav = None
            if rec.enabled:
                wav = f"{c.RAW}/phrases/{key}.wav"
                rec.record(wav, hold_s + 1.7, play)
            else:
                play()
            if clk:
                clk.stop()
                marks["clock_t0_us"] = (clk.t0 - t0) // 1000
                marks["clock_bpm"] = a.bpm
            ev = j6.take()
            ons = c.notes_on(ev, j6.channel)
            print(f"   {key}: {len(ons)} notes "
                  f"{' '.join(c.note_name(x[1]) for x in ons[:10])}"
                  f"{' ...' if len(ons) > 10 else ''}")
            store.put(key, style=s, variation=v, held=notes, bpm=a.bpm,
                      clock=a.clock, marks_us=marks, wav=wav,
                      events=c.ev_to_json(ev, t0))
            time.sleep(0.3)
        i += 1

    j6.all_off()
    j6.close()
    print("Done. Data in", store.path)


if __name__ == "__main__":
    main()
