#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Parse the J-6 manual's Chord Set List page into data/chordsets.json.

    python3 tools/parse_manual_chords.py data/manual/28645807.html data/chordsets.json

Each set: {"set": n, "genre": str, "chords": [{"key": 0..11, "name": str,
"notes": [midi, ...ascending]}]}.  Note names use C4 = 60 (Roland convention).
"""
import html
import json
import re
import sys

NOTE = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}


def midi(tok):
    m = re.fullmatch(r"([A-G])([#b]?)(-?\d)", tok)
    if not m:
        raise ValueError(f"bad note {tok!r}")
    n = NOTE[m.group(1)] + {"#": 1, "b": -1, "": 0}[m.group(2)]
    return n + (int(m.group(3)) + 1) * 12


def text(cell):
    t = re.sub(r"<[^>]+>", " ", cell)
    return " ".join(html.unescape(t).replace("\xa0", " ").split())


def main(src, dst):
    s = open(src, encoding="utf-8").read()
    rows = re.findall(r"<tr>(.*?)</tr>", s, re.S)
    sets, cur = [], None
    for r in rows:
        ths = re.findall(r"<th[^>]*>(.*?)</th>", r, re.S)
        tds = re.findall(r"<td[^>]*>(.*?)</td>", r, re.S)
        if len(ths) >= 2 and len(tds) == 12 and text(ths[0]).isdigit():
            cur = {"set": int(text(ths[0])), "genre": text(ths[1]),
                   "names": [text(t) for t in tds]}
        elif cur and len(tds) == 12 and not ths:
            chords = []
            for k, (name, cell) in enumerate(zip(cur["names"], tds)):
                toks = re.findall(r"[A-G][#b]?-?\d", text(cell))
                chords.append({"key": k, "name": name,
                               "notes": sorted(midi(t) for t in toks)})
            sets.append({"set": cur["set"], "genre": cur["genre"],
                         "chords": chords})
            cur = None

    # sanity checks
    nums = [x["set"] for x in sets]
    assert nums == list(range(1, 101)), f"sets found: {nums[:5]}..{nums[-5:]} ({len(nums)})"
    sizes = {}
    for x in sets:
        for ch in x["chords"]:
            n = len(ch["notes"])
            assert 1 <= n <= 4, (x["set"], ch)
            sizes.setdefault(n, set()).add(x["set"])
    with open(dst, "w") as f:
        json.dump(sets, f, indent=1)
    print(f"{len(sets)} sets -> {dst}")
    for n in sorted(sizes):
        ss = sorted(sizes[n])
        print(f"  {n}-note chords in {len(ss)} sets: {ss if len(ss) < 30 else str(ss[:10]) + '...'}")


if __name__ == "__main__":
    main(*sys.argv[1:3])
