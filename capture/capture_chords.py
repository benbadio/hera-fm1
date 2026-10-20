# SPDX-License-Identifier: GPL-3.0-only
"""Capture all 100 J-6 chord sets (12 chords each) from its MIDI output.

Per set you select the chord set on the J-6 by hand (SHIFT+CHORD, turn
TEMPO/VALUE, press CHORD); the script then plays each root over MIDI
(--mode auto) or asks you to press the 12 keys C..B in order (--mode local)
and records the chord notes the J-6 transmits.

Output: raw/chords.jsonl (one record per set, raw events kept).
Resumable: re-run and finished sets are skipped (use --redo set042 to redo).
"""
import time

import j6cap as c


def cluster(ons, gap_ns=120_000_000):
    """Group note-ons into chords separated by > gap."""
    groups, cur, last = [], [], None
    for t, n, v in ons:
        if last is not None and t - last > gap_ns and cur:
            groups.append(cur)
            cur = []
        cur.append((t, n, v))
        last = t
    if cur:
        groups.append(cur)
    return groups


def fmt(notes):
    return " ".join(c.note_name(n) for n in sorted(notes))


def main():
    ap = c.base_args(__doc__)
    ap.add_argument("--mode", choices=["auto", "local"], default="auto",
                    help="auto: script sends roots over MIDI (needs preflight "
                         "chord_mode=auto); local: you press the keys")
    ap.add_argument("--sets", default="1-100", help="e.g. 1-100 or 14-26")
    ap.add_argument("--base", type=int, default=60,
                    help="MIDI note of the C root to send (default 60 = C4)")
    ap.add_argument("--octave-check", action="store_true",
                    help="also send each root one octave up (13..24 notes)")
    a = ap.parse_args()
    lo, hi = (int(x) for x in a.sets.split("-"))
    store = c.Store("chords")
    j6 = c.J6(a.port, a.channel)
    ch = a.channel - 1
    print(f"Chord capture, sets {lo}-{hi}, mode {a.mode}. "
          f"{len(store.done)} sets already done.")
    print("Before starting: CHORD ON, STYLE OFF, KEY transpose = C (0).")

    s = lo
    while s <= hi:
        key = f"set{s:03d}"
        if store.has(key, a.redo):
            s += 1
            continue
        r = c.ask(f"Select CHORD SET {s} on the J-6 (SHIFT+CHORD, turn "
                  f"VALUE to {s}, press CHORD). Then press Enter.")
        if r == "skip":
            s += 1
            continue
        if r == "redo" and s > lo:
            s -= 1
            a.redo.append(f"set{s:03d}")
            continue
        j6.clear()
        t0 = c.now_ns()
        roots = list(range(a.base, a.base + 12))
        if a.octave_check:
            roots += list(range(a.base + 12, a.base + 24))
        sent = []
        if a.mode == "auto":
            for root in roots:
                ts = c.now_ns()
                j6.note_on(root, 100)
                time.sleep(0.45)
                j6.note_off(root)
                sent.append({"root": root, "us": (ts - t0) // 1000})
                time.sleep(0.30)
            time.sleep(0.3)
        else:
            input("   Now press C, C#, D ... B (12 keys, one at a time, "
                  "~half a second each). Press Enter when done: ")
        ev = j6.take()
        groups = cluster(c.notes_on(ev, ch))
        chords = [sorted({n for _, n, _ in g}) for g in groups]
        print(f"   got {len(chords)} chords:")
        for i, ch_notes in enumerate(chords):
            print(f"     {c.NOTE_NAMES[i % 12]:>2}: {fmt(ch_notes)}")
        if len(chords) != len(roots):
            print(f"   !! expected {len(roots)} chords, got {len(chords)}.")
            if c.ask("Save anyway? (Enter=save, r=retry this set)") == "redo":
                continue
        store.put(key, set=s, mode=a.mode, sent=sent, chords=chords,
                  events=c.ev_to_json(ev, t0))
        s += 1

    j6.all_off()
    j6.close()
    print("Done. Data in", store.path)


if __name__ == "__main__":
    main()
