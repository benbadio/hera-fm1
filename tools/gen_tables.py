#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Generate firmware/src/j6/j6_chord_tables.h (the J-6 chord sets) from data/chordsets.json.

    python3 -I tools/gen_tables.py            write the header
    python3 -I tools/gen_tables.py --check    exit 1 if the committed header differs from what it would write
    (--input FILE / --output FILE override the two paths, for testing the generator)

Standard library only. data/chordsets.json is only read. It must hold 100 sets, each
{"set": 1..100 in order, "genre": str, "chords": [12 x {"key": 0..11 in order, "name": str, "notes": [...]}]},
with 2..4 notes per chord in 0..127, at least 2 of them distinct (j6_chord.c drops a repeated note, so
[60, 60] would be a one-voice chord), ASCII names of at most 12 characters, and no key twice in one object
(json.loads would keep only the last value). Anything else stops the generator with an error (exit
status 1), because j6_chord.c relies on it. A chord that lists a note more than once but still has 2
distinct notes (set 43 key F) is accepted and named in the header comment.

The header (docs/J6_ENGINE_DESIGN.md 4.2) holds j6c_-prefixed static const data only:
  j6c_notes[100][12][4]   the notes exactly as listed, duplicates kept (CH-4), 0 in an unused slot
  j6c_count[100][12]      the number of listed notes (2..4), so a real note 0 is never taken for a gap
  j6c_name_pool, j6c_name_off[100][12]    NUL-terminated chord names, deduplicated, uint16_t offsets
  j6c_genre_pool, j6c_genre_off[], j6c_genre_idx[100]   the genre names and each set's genre
"""
import hashlib
import json
import os
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
DEFAULT_INPUT = os.path.join(ROOT, "data", "chordsets.json")
DEFAULT_OUTPUT = os.path.join(ROOT, "firmware", "src", "j6", "j6_chord_tables.h")

SETS = 100
CHORDS = 12
SLOTS = 4
MIN_NOTES = 2
NAME_MAX = 12
GENRE_MAX = 31
POOL_MAX = 65535          # offsets are uint16_t
GENRES_MAX = 255          # genre indices are uint8_t
KEY_NAMES = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")


class DataError(Exception):
    pass


def fail(where, msg):
    raise DataError("%s: %s" % (where, msg))


def is_int(v):
    return isinstance(v, int) and not isinstance(v, bool)


def no_duplicate_keys(pairs):
    """json.loads object_pairs_hook: an object that names a key twice is an error, not "the last one wins"."""
    seen = set()
    for k, _ in pairs:
        if k in seen:
            ctx = ", ".join("%s=%r" % (kk, vv) for kk, vv in pairs if not isinstance(vv, (list, dict)))
            raise DataError("duplicate key \"%s\" in an object (%s)" % (k, ctx))
        seen.add(k)
    return dict(pairs)


def repeated_notes(notes):
    """The notes a chord lists more than once, ascending."""
    return sorted(n for n in set(notes) if notes.count(n) > 1)


def check_text(where, v, max_len, allow_empty):
    if not isinstance(v, str):
        fail(where, "expected a string, got %r" % (v,))
    if not allow_empty and not v:
        fail(where, "empty string")
    if len(v) > max_len:
        fail(where, "%r is %d characters, at most %d allowed" % (v, len(v), max_len))
    for ch in v:
        if not (0x20 <= ord(ch) <= 0x7E):
            fail(where, "%r: only printable ASCII is allowed (found %r)" % (v, ch))
    return v


def validate(data, input_label):
    """Check every assumption the C code makes; return [(genre, [(name, [notes])] * 12)] * 100."""
    if not isinstance(data, list):
        fail(input_label, "the top level must be a list of sets")
    if len(data) != SETS:
        fail(input_label, "%d sets, expected %d" % (len(data), SETS))
    sets = []
    for si, s in enumerate(data):
        where = "set #%d" % (si + 1)
        if not isinstance(s, dict):
            fail(where, "expected an object")
        if set(s.keys()) != {"set", "genre", "chords"}:
            fail(where, "keys %s, expected chords, genre, set" % sorted(s.keys()))
        if not is_int(s["set"]) or s["set"] != si + 1:
            fail(where, "\"set\" is %r, expected %d (sets in order 1..%d)" % (s["set"], si + 1, SETS))
        genre = check_text(where + " genre", s["genre"], GENRE_MAX, allow_empty=False)
        chords = s["chords"]
        if not isinstance(chords, list) or len(chords) != CHORDS:
            fail(where, "expected %d chords, got %s" % (CHORDS, len(chords) if isinstance(chords, list) else chords))
        out = []
        for ci, c in enumerate(chords):
            cw = "%s chord #%d" % (where, ci)
            if not isinstance(c, dict):
                fail(cw, "expected an object")
            if set(c.keys()) != {"key", "name", "notes"}:
                fail(cw, "keys %s, expected key, name, notes" % sorted(c.keys()))
            if not is_int(c["key"]) or c["key"] != ci:
                fail(cw, "\"key\" is %r, expected %d (keys in order C..B)" % (c["key"], ci))
            name = check_text(cw + " name", c["name"], NAME_MAX, allow_empty=True)
            notes = c["notes"]
            if not isinstance(notes, list) or not MIN_NOTES <= len(notes) <= SLOTS:
                fail(cw, "expected %d..%d notes, got %r" % (MIN_NOTES, SLOTS, notes))
            for n in notes:
                if not is_int(n) or not 0 <= n <= 127:
                    fail(cw, "note %r is not a MIDI note 0..127" % (n,))
            distinct = len(set(notes))
            if distinct < MIN_NOTES:
                fail(cw, "notes %r have %d distinct note%s, at least %d needed (j6_chord_raw drops a repeat)"
                     % (notes, distinct, "" if distinct == 1 else "s", MIN_NOTES))
            out.append((name, list(notes)))
        sets.append((genre, out))
    return sets


def c_string(s):
    """A C string literal body: printable ASCII only (validated); escapes quote, backslash and '?' (trigraphs)."""
    return "".join("\\" + ch if ch in "\"\\?" else ch for ch in s)


def c_comment(s):
    return s.replace("*/", "*_/").replace("/*", "/_*")


class Pool:
    """Deduplicated NUL-terminated strings in one char array, in first-use order."""

    def __init__(self):
        self.items = []
        self.offset = {}
        self.size = 0

    def add(self, s):
        if s not in self.offset:
            self.offset[s] = self.size
            self.items.append(s)
            self.size += len(s) + 1
        return self.offset[s]

    def emit(self, c_name, len_macro, width=100):
        """Concatenated literals, each "name\\0"; the last one relies on the literal's own terminator, so
        sizeof(array) == self.size."""
        parts = []
        for k, s in enumerate(self.items):
            last = k == len(self.items) - 1
            parts.append('"%s%s"' % (c_string(s), "" if last else "\\0"))
        lines, cur = [], "   "
        for p in parts:
            if len(cur) + 1 + len(p) > width and cur.strip():
                lines.append(cur)
                cur = "   "
            cur += " " + p
        lines.append(cur)
        return "static const char %s[%s] =\n%s;\n" % (c_name, len_macro, "\n".join(lines))


def generate(raw, input_label):
    try:
        data = json.loads(raw.decode("utf-8"), object_pairs_hook=no_duplicate_keys)
    except (UnicodeDecodeError, ValueError) as e:
        raise DataError("%s: not valid UTF-8 JSON: %s" % (input_label, e))
    except DataError as e:
        raise DataError("%s: %s" % (input_label, e))
    sets = validate(data, input_label)
    digest = hashlib.sha256(raw).hexdigest()

    names = Pool()
    genres = Pool()
    genre_list = []          # distinct genres in first-use order
    name_off = []
    genre_idx = []
    for genre, chords in sets:
        if genre not in genre_list:
            genre_list.append(genre)
            genres.add(genre)
        genre_idx.append(genre_list.index(genre))
        name_off.append([names.add(name) for name, _ in chords])
    if max(names.offset.values()) > POOL_MAX or max(genres.offset.values()) > POOL_MAX:
        raise DataError("%s: string pools too big for uint16_t offsets" % input_label)
    if len(genre_list) > GENRES_MAX:
        raise DataError("%s: %d genres, at most %d" % (input_label, len(genre_list), GENRES_MAX))

    counts = {}
    for _, chords in sets:
        for _, notes in chords:
            counts[len(notes)] = counts.get(len(notes), 0) + 1
    all_notes = [n for _, chords in sets for _, notes in chords for n in notes]
    repeats = ["set %d key %s (%s)" % (si + 1, KEY_NAMES[ci], " ".join(str(n) for n in repeated_notes(notes)))
               for si, (_, chords) in enumerate(sets) for ci, (_, notes) in enumerate(chords)
               if repeated_notes(notes)]

    o = []
    w = o.append
    w("/* generated by tools/gen_tables.py from %s (sha256 %s) - do not edit */\n" % (input_label, digest))
    w("/* SPDX-License-Identifier: GPL-3.0-only */\n")
    w("/* j6_chord_tables.h - the J-6 chord sets (CH-2), included by j6_chord.c and test_chord.c only.\n")
    w(" *\n")
    w(" * %d sets x %d chords (key C .. B), the notes exactly as the manual's chord set list gives them at\n"
      % (SETS, CHORDS))
    w(" * octave 0 and KEY 0, duplicates kept (j6_chord.c sorts and removes them, CH-4).\n")
    w(" * Listed: %s; notes %d..%d.\n"
      % (", ".join("%d chords of %d notes" % (counts[k], k) for k in sorted(counts, reverse=True)),
         min(all_notes), max(all_notes)))
    w(" * Chords that list a note more than once: %s.\n" % (", ".join(repeats) if repeats else "none"))
    w(" * Regenerate with: python3 -I tools/gen_tables.py */\n")
    w("#ifndef J6C_CHORD_TABLES_H\n")
    w("#define J6C_CHORD_TABLES_H\n")
    w("\n")
    w("#include <stdint.h>\n")
    w("\n")
    w("#define J6C_TAB_SETS %d\n" % SETS)
    w("#define J6C_TAB_CHORDS %d\n" % CHORDS)
    w("#define J6C_TAB_SLOTS %d\n" % SLOTS)
    w("#define J6C_TAB_GENRES %d\n" % len(genre_list))
    w("#define J6C_NAME_POOL_LEN %d\n" % names.size)
    w("#define J6C_GENRE_POOL_LEN %d\n" % genres.size)
    w("\n")

    w("/* [set][chord index][slot]: the listed notes, 0 in an unused slot */\n")
    w("static const uint8_t j6c_notes[J6C_TAB_SETS][J6C_TAB_CHORDS][J6C_TAB_SLOTS] = {\n")
    for si, (genre, chords) in enumerate(sets):
        w("    /* %d %s */\n" % (si + 1, c_comment(genre)))
        cells = ["{%s}" % ", ".join(str(n) for n in notes + [0] * (SLOTS - len(notes))) for _, notes in chords]
        half = CHORDS // 2
        w("    {%s,\n" % ", ".join(cells[:half]))
        w("     %s},\n" % ", ".join(cells[half:]))
    w("};\n")
    w("\n")

    w("/* [set][chord index]: how many slots of j6c_notes are listed notes (%d..%d) */\n" % (MIN_NOTES, SLOTS))
    w("static const uint8_t j6c_count[J6C_TAB_SETS][J6C_TAB_CHORDS] = {\n")
    for si, (_, chords) in enumerate(sets):
        w("    {%s},  /* %d */\n" % (", ".join(str(len(notes)) for _, notes in chords), si + 1))
    w("};\n")
    w("\n")

    w("/* chord names, NUL-terminated, deduplicated; j6c_name_off[set][idx] is the offset of a name */\n")
    w(names.emit("j6c_name_pool", "J6C_NAME_POOL_LEN"))
    w("\n")
    w("static const uint16_t j6c_name_off[J6C_TAB_SETS][J6C_TAB_CHORDS] = {\n")
    for si, offs in enumerate(name_off):
        w("    {%s},  /* %d */\n" % (", ".join(str(x) for x in offs), si + 1))
    w("};\n")
    w("\n")

    w("/* genre names, NUL-terminated; j6c_genre_off[g] is the offset of genre g */\n")
    w(genres.emit("j6c_genre_pool", "J6C_GENRE_POOL_LEN"))
    w("\n")
    w("static const uint16_t j6c_genre_off[J6C_TAB_GENRES] = {\n")
    for g in genre_list:
        w("    %d,  /* %s */\n" % (genres.offset[g], c_comment(g)))
    w("};\n")
    w("\n")
    w("/* [set]: index into j6c_genre_off */\n")
    w("static const uint8_t j6c_genre_idx[J6C_TAB_SETS] = {\n")
    for k in range(0, SETS, 20):
        w("    %s,\n" % ", ".join(str(x) for x in genre_idx[k:k + 20]))
    w("};\n")
    w("\n")
    w("#endif /* J6C_CHORD_TABLES_H */\n")
    return "".join(o).encode("ascii")


def label_for(path):
    try:
        rel = os.path.relpath(os.path.abspath(path), ROOT)
    except ValueError:
        rel = path
    if rel.startswith(".."):
        rel = os.path.basename(path)
    return rel.replace(os.sep, "/")


def main(argv):
    check = False
    inp, outp = DEFAULT_INPUT, DEFAULT_OUTPUT
    k = 1
    while k < len(argv):
        a = argv[k]
        if a == "--check":
            check = True
        elif a in ("--input", "--output") and k + 1 < len(argv):
            if a == "--input":
                inp = argv[k + 1]
            else:
                outp = argv[k + 1]
            k += 1
        elif a in ("-h", "--help"):
            print(__doc__)
            return 0
        else:
            sys.stderr.write("gen_tables.py: unknown argument %r (see --help)\n" % a)
            return 2
        k += 1

    try:
        with open(inp, "rb") as f:
            raw = f.read()
    except OSError as e:
        sys.stderr.write("gen_tables.py: ERROR: cannot read %s: %s\n" % (inp, e))
        return 1
    try:
        text = generate(raw, label_for(inp))
    except DataError as e:
        sys.stderr.write("gen_tables.py: ERROR: %s\n" % e)
        return 1

    if check:
        try:
            with open(outp, "rb") as f:
                old = f.read()
        except OSError:
            old = None
        if old != text:
            sys.stderr.write("gen_tables.py: %s is %s; run: python3 -I tools/gen_tables.py\n"
                             % (label_for(outp), "missing" if old is None else "out of date"))
            return 1
        print("gen_tables.py: %s is up to date" % label_for(outp))
        return 0

    tmp = outp + ".tmp"
    with open(tmp, "wb") as f:
        f.write(text)
    os.replace(tmp, outp)
    print("gen_tables.py: wrote %s (%d bytes)" % (label_for(outp), len(text)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
