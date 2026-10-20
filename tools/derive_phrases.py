#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Turn captured J-6 phrase recordings into a tick-exact phrase table.

    python3 tools/derive_phrases.py capture/raw/phrases.jsonl data/phrases_6_9.json

For every (style, variation, held-chord size) the J-6 played while we held a
C-major-ish chord (sizes 1..4: C4 / C4 G4 / C4 E4 G4 / C4 E4 G4 B4) we:
  * convert event times to 96 PPQN ticks from the key press, using the
    J-6's own MIDI clock (24 ppqn) as the tempo reference;
  * find the loop length (1, 2 or 4 bars of 4/4) by checking repetition;
  * express each note of one loop as (tick, length, tone index into the
    sorted held notes, octave offset, accent).
Output JSON: {"PPQN": 96, "phrases": {"s6v01": {"1": {...}, ... "4": {...}}}}
"""
import json
import statistics
import sys

PPQN = 96
BAR = 4 * PPQN
HELD = {1: [60], 2: [60, 67], 3: [60, 64, 67], 4: [60, 64, 67, 71]}


def parse(rec):
    t_on = rec["marks_us"]["on"]
    t_off = rec["marks_us"]["off"]
    clk = [e["us"] for e in rec["events"] if e["msg"] == "F8"]
    per = [b - a for a, b in zip(clk, clk[1:]) if 15000 < b - a < 30000]
    beat_us = 24 * statistics.mean(per)
    notes, open_ = [], {}
    for e in rec["events"]:
        b = e["msg"].split()
        if b[0] not in ("90", "80"):
            continue
        n, v = int(b[1], 16), int(b[2], 16)
        tk = (e["us"] - t_on) / beat_us * PPQN
        if b[0] == "90" and v > 0:
            if n in open_:                      # retrigger without off
                s = open_.pop(n)
                notes.append([s[0], tk - s[0], n, s[1]])
            open_[n] = (tk, v)
        elif n in open_:
            s = open_.pop(n)
            notes.append([s[0], tk - s[0], n, s[1]])
    end_tk = (t_off - t_on) / beat_us * PPQN
    for n, s in open_.items():
        notes.append([s[0], end_tk - s[0], n, s[1]])
    notes.sort(key=lambda x: (x[0], x[2]))
    return notes, end_tk, 60e6 / beat_us


def align(notes, grid=4):
    """The J-6 phrase grid sits a constant 1-3 ticks away from our press
    timestamp (MIDI latency).  Find the offset that puts onsets on a 4-tick
    grid (1/24 beat, the strum spacing), shift by it, and snap the
    immediate first notes (fired on key press) to tick 0."""
    best, best_d = 0.0, None
    for i in range(-40, 6):            # latency: -4.0 .. +0.5 ticks
        d = i / 10
        cost = sum(min((t - d) % grid, grid - (t - d) % grid)
                   for t, _, _, _ in notes if t > 8)
        if best_d is None or cost < best_d:
            best, best_d = d, cost
    out = []
    for t, dur, n, v in notes:
        # Notes the J-6 fires the instant the key is pressed arrive ~0.5
        # tick after our press stamp; they are grid tick 0.  Everything
        # else follows the J-6 grid, `best` ticks away from our stamp.
        t2 = 0.0 if t < 1.0 else t - best
        out.append([t2, dur, n, v])
    return out, best


def quant(notes):
    out, err = [], 0.0
    for t, d, n, v in notes:
        qt, qd = round(t), round(d)
        err = max(err, abs(qt - t))
        out.append((qt, qd, n, v))
    return out, err


def same(a, b, tol=2):
    """Multiset match of (t, n, v) onsets with +-tol tick tolerance."""
    if len(a) != len(b):
        return False
    rest = list(b)
    for t, n, v in a:
        hit = next((x for x in rest if x[1] == n and x[2] == v
                    and abs(x[0] - t) <= tol), None)
        if hit is None:
            return False
        rest.remove(hit)
    return True


def loop_len(notes, end_tk, guard=6):
    """Smallest L (1, 2 or 4 bars) such that every onset at t >= L repeats
    the onset at t - L.  The capture is ~4 bars minus a few ticks (the J-6
    ran slightly under 120 BPM), so only the window that exists is compared;
    `guard` ticks at the very end are ignored (a note there may be cut)."""
    ons = [(t, n, v) for t, d, n, v in notes]
    usable = end_tk - guard
    for L in range(PPQN, 14 * PPQN, PPQN):   # any whole number of beats
        if usable - L < BAR / 2:          # need at least half a bar of overlap
            break
        # the J-6 fires the first event on the key press itself, wherever
        # it sits on the grid, so the first 8 ticks are not compared
        later = [(t - L, n, v) for t, n, v in ons if L + 8 <= t < usable]
        early = [(t, n, v) for t, n, v in ons if 8 <= t < usable - L]
        if same(early, later):
            return L, True
    return 4 * BAR, None                  # >= 4 bars: one full cycle captured


def tone(n, held):
    for o in (0, -1, 1, -2, 2):
        for i, h in enumerate(held):
            if h + 12 * o == n:
                return i, o
    return None


def main(src, dst):
    recs = {}
    for line in open(src):
        r = json.loads(line)
        recs[r["key"]] = r                         # last take wins
    out, worst, problems = {}, 0.0, []
    for key in sorted(recs):
        r = recs[key]
        size = len(r["held"])
        held = sorted(r["held"])
        raw, end_tk, bpm = parse(r)
        raw, offset = align(raw)
        end_tk -= offset
        q, err = quant(raw)
        offgrid = sum(1 for t, _, _, _ in q if t % 4)
        if offgrid:
            problems.append(f"{key}: {offgrid} onsets off the 4-tick grid (offset {offset:+.1f})")
        worst = max(worst, err)
        L, repeats = loop_len(q, end_tk)
        # Take one loop from the cleanest window: the 2nd cycle when it was
        # captured completely, else the 1st cycle with its first 8 ticks
        # replaced by the start of the 2nd cycle (press-time artifact).
        if 2 * L <= end_tk - 6:
            src = [(t - L, d, n, v) for t, d, n, v in q if L <= t < 2 * L]
        else:
            src = [x for x in q if 8 <= x[0] < L] + \
                  [(t - L, d, n, v) for t, d, n, v in q if L <= t < L + 8]
            if not any(L <= t < L + 8 for t, *_ in q):
                src += [x for x in q if x[0] < 8]
            src.sort(key=lambda x: (x[0], x[2]))
        loop = []
        for t, d, n, v in src:
            m = tone(n, held)
            if m is None:
                problems.append(f"{key}: note {n} not a held tone/octave")
                continue
            t = 4 * round(t / 4) % L          # J-6 phrase grid is 4 ticks
            loop.append({"t": t, "len": max(1, min(d, L)), "tone": m[0],
                         "oct": m[1], "acc": 1 if v >= 120 else 0})
        if repeats is None and any(t >= end_tk - 12 for t, d, n, v in q):
            problems.append(f"{key}: 4-bar phrase has notes in the last 12 ticks (may be cut)")
        pv = key[:5]
        out.setdefault(pv, {})[str(size)] = {
            "loop": L, "repeats": repeats, "bpm": round(bpm, 2),
            "notes": loop}
    json.dump({"PPQN": PPQN, "held": HELD, "phrases": out}, open(dst, "w"),
              indent=0)
    print(f"{len(out)} variations -> {dst}; worst timing rounding {worst:.2f} ticks")
    for pv in sorted(out):
        row = "  ".join(f"n{s}:{out[pv][s]['loop'] / PPQN:g}bt/{len(out[pv][s]['notes'])}"
                        f"{'' if out[pv][s]['repeats'] else '*'}"
                        for s in sorted(out[pv]))
        print(f"  {pv}  {row}")
    if problems:
        print(f"{len(problems)} problems:")
        for p in problems[:40]:
            print("  " + p)


if __name__ == "__main__":
    main(*sys.argv[1:3])
