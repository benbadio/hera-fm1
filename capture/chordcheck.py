# SPDX-License-Identifier: GPL-3.0-only
"""Follow-up to preflight: MIDI thru, chord output, and audio level.

Each step can be repeated: type r at the prompt to redo the step you just did.
Results go to raw/chordcheck.json.
"""
import json
import os
import time

import j6cap as c

SET1_C = [48, 55, 62, 64]          # manual: chord set 1, key C = C3 G3 D4 E4


def step(title, fn):
    """Run fn until the user accepts the result."""
    while True:
        r = c.ask(title)
        if r == "skip":
            return None
        out = fn()
        again = input("   Result OK? [Enter]=yes  r=redo this step : ").strip().lower()
        if again != "r":
            return out


def notes(ev, ch):
    return [n for _, n, _ in c.notes_on(ev, ch)]


def names(ns):
    return " ".join(c.note_name(n) for n in ns)


def main():
    a = c.base_args(__doc__).parse_args()
    j6 = c.J6(a.port, a.channel)
    rec = c.AudioRec()
    ch = j6.channel
    res = {}

    def send_c4(hold=0.8):
        j6.clear()
        j6.note_on(60, 100)
        time.sleep(hold)
        j6.note_off(60)
        time.sleep(0.5)
        return notes(j6.take(), ch)

    # A. MIDI thru
    def a_thru():
        n = send_c4()
        print(f"   J-6 sent back: {names(n) or '(nothing)'}"
              f"{'   <- echo still on' if n else '   <- good, no echo'}")
        return n
    res["thru_off_echo"] = step(
        "Open the menu (SHIFT+B), find 'thrv' (MIDI thru) and set it to OFF. "
        "CHORD OFF, STYLE OFF. Press Enter: I send C4.", a_thru)

    # B. local key with CHORD on
    def b_local():
        j6.clear()
        time.sleep(4)
        n = notes(j6.take(), ch)
        print(f"   J-6 sent: {names(n) or '(nothing)'}")
        return n
    res["local_chord"] = step(
        "CHORD ON with CHORD SET 1, STYLE OFF. Press Enter, then press and "
        "release the J-6's C key once (within 4 s).", b_local)

    # C. MIDI-in with CHORD on (+ audio)
    def c_midi():
        n = []

        def go():
            n.extend(send_c4(1.0))
        rec.record(os.path.join(c.RAW, "chordcheck_midi_chord.wav"), 2.0, go)
        print(f"   J-6 sent: {names(n) or '(nothing)'}")
        heard = input("   Did the J-6 play a CHORD (several notes) or one note? "
                      "[c]=chord  [1]=one note : ").strip().lower()
        return {"notes": n, "heard": heard}
    res["midi_chord"] = step(
        "Same settings (CHORD ON, set 1, STYLE OFF). Press Enter: I send C4 "
        "over MIDI -- listen to the J-6.", c_midi)

    # D. CHORD + STYLE: arpeggiated chord tones should come out on MIDI
    def d_arp():
        n = send_c4(2.5)
        uniq = sorted(set(n))
        print(f"   notes: {names(n)}")
        print(f"   unique: {names(uniq)}   manual says set 1 C = {names(SET1_C)}"
              f"   {'MATCH' if uniq == SET1_C else ''}")
        return {"notes": n, "unique": uniq}
    res["chord_arp"] = step(
        "CHORD ON (set 1) AND STYLE ON with STYLE = 1, VARIATION = 1. "
        "Press Enter: I hold C4 over MIDI for 2.5 s.", d_arp)

    # E. audio level
    def e_level():
        import numpy as np
        import soundfile as sf
        p = os.path.join(c.RAW, "chordcheck_level.wav")
        rec.record(p, 2.0, lambda: send_c4(1.0))
        x, _ = sf.read(p, dtype="float32")
        pk = 20 * np.log10(float(np.max(np.abs(x))) + 1e-9)
        verdict = "good" if -15 <= pk <= -3 else ("too quiet" if pk < -15 else "too hot")
        print(f"   peak {pk:.1f} dBFS -> {verdict} (aim -12..-3)")
        return round(pk, 1)
    if rec.enabled:
        res["audio_peak_dbfs"] = step(
            "CHORD OFF, STYLE OFF. Turn the J-6 VOLUME up (about 3 o'clock "
            "or higher). Press Enter: I play C4 and measure.", e_level)

    j6.all_off()
    j6.close()
    os.makedirs(c.RAW, exist_ok=True)
    with open(os.path.join(c.RAW, "chordcheck.json"), "w") as f:
        json.dump(res, f, indent=1)
    print("\nSaved raw/chordcheck.json -- tell Claude it's done.")


if __name__ == "__main__":
    main()
