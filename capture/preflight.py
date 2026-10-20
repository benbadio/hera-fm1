# SPDX-License-Identifier: GPL-3.0-only
"""10-minute pre-flight: what does the J-6 send, and how can we capture it?

Answers, and saves to raw/preflight.json:
  * which MIDI port / audio device is the J-6
  * does the J-6 transmit the notes you play on its keys? on which channel?
  * does it send MIDI clock while idle?
  * do notes we SEND trigger chords, and are the chord notes transmitted back?
  * are phrase (STYLE) notes transmitted, from MIDI-in and from its own keys?
  * is USB audio coming through, and at what level?
"""
import json
import os
import time

import mido

import j6cap as c


def listen(j6, secs):
    j6.clear()
    time.sleep(secs)
    return j6.take()


def count(events, typ):
    return sum(1 for _, m in events if m.type == typ)


def notes(events):
    return [(m.channel + 1, m.note, m.velocity) for _, m in events
            if m.type == "note_on" and m.velocity > 0]


def main():
    ap = c.base_args(__doc__)
    ap.add_argument("--no-audio", action="store_true")
    a = ap.parse_args()
    res = {}

    print("MIDI inputs :", mido.get_input_names())
    print("MIDI outputs:", mido.get_output_names())
    j6 = c.J6(a.port, a.channel)
    res["midi_in"], res["midi_out"] = j6.in_name, j6.out_name
    print(f"Using MIDI in '{j6.in_name}', out '{j6.out_name}'")
    rec = c.AudioRec(enabled=not a.no_audio)
    res["audio_device"] = rec.dev

    # 1. Idle traffic / clock
    print("\n[1] Listening 3 s with the J-6 idle (not playing)...")
    ev = listen(j6, 3)
    res["idle_clock_msgs"] = count(ev, "clock")
    print(f"    clock messages while idle: {res['idle_clock_msgs']}")

    # 2. Local keys -> MIDI out
    c.ask("Make sure CHORD and STYLE are OFF. Then press Enter and play "
          "C, E, G on the J-6 keys (one at a time) within 5 s.")
    ev = listen(j6, 5)
    n = notes(ev)
    res["local_keys_notes"] = n
    print(f"    received: {n}")
    if n:
        res["synth_channel"] = n[0][0]
        print(f"    -> J-6 transmits its keys on channel {n[0][0]}")
        j6.channel = n[0][0] - 1
    else:
        print("    !! No notes. Check the J-6 menu: CH, and that USB MIDI is on.")

    # 3. MIDI-in note -> echoed?
    c.ask("Still CHORD and STYLE OFF. Press Enter: I will send C4 over MIDI.")
    j6.clear()
    j6.note_on(60); time.sleep(0.8); j6.note_off(60); time.sleep(0.5)
    n = notes(j6.take())
    res["midi_in_plain_echo"] = n
    print(f"    J-6 sent back: {n}  (empty = no echo, which is normal)")

    # 4. MIDI-in note with CHORD on -> chord transmitted?
    c.ask("Turn CHORD ON (any chord set; leave STYLE OFF). Press Enter: "
          "I will send C4 over MIDI.")
    j6.clear()
    j6.note_on(60); time.sleep(0.8); j6.note_off(60); time.sleep(0.5)
    n = notes(j6.take())
    res["midi_in_chord_out"] = n
    print(f"    J-6 sent back: {n}")
    c.ask("Keep CHORD ON. Press Enter, then press the C key on the J-6 once.")
    n = notes(listen(j6, 4))
    res["local_chord_out"] = n
    print(f"    J-6 sent: {n}")

    # 5. Phrases
    c.ask("Turn CHORD OFF, STYLE ON, STYLE knob = 1, VARIATION = 1. "
          "Press Enter: I will hold C4+E4+G4 over MIDI for 3 s.")
    j6.clear()
    for k in (60, 64, 67):
        j6.note_on(k)
    time.sleep(3)
    for k in (60, 64, 67):
        j6.note_off(k)
    time.sleep(0.5)
    ev = j6.take()
    n = notes(ev)
    res["midi_in_phrase_out"] = n
    res["phrase_clock_msgs"] = count(ev, "clock")
    print(f"    phrase notes back: {len(n)} {n[:12]}")
    print(f"    clock msgs during phrase: {res['phrase_clock_msgs']}")
    c.ask("Same settings. Press Enter, then HOLD the C, E and G keys on the "
          "J-6 together for about 3 s.")
    ev = listen(j6, 5)
    n = notes(ev)
    res["local_phrase_out"] = n
    print(f"    phrase notes sent: {len(n)} {n[:12]}")

    # 6. Playback clock
    c.ask("Press Enter, then press PLAY on the J-6 (any pattern), wait ~2 s, "
          "press PLAY again to stop.")
    ev = listen(j6, 5)
    res["play_clock_msgs"] = count(ev, "clock")
    res["play_start_stop"] = [m.type for _, m in ev if m.type in ("start", "stop")]
    print(f"    clock msgs: {res['play_clock_msgs']}, transport: {res['play_start_stop']}")

    # 7. Audio
    if rec.enabled:
        c.ask("STYLE OFF, CHORD OFF, VOLUME around 3 o'clock. Press Enter: "
              "I will play C4 for 1 s and record USB audio.")
        path = os.path.join(c.RAW, "preflight_audio.wav")

        def play():
            j6.note_on(60, 100); time.sleep(1.0); j6.note_off(60)
        rec.record(path, 2.5, play)
        import soundfile as sf
        import numpy as np
        x, _ = sf.read(path, dtype="float32")
        pk = float(np.max(np.abs(x))) if len(x) else 0.0
        res["audio_peak_dbfs"] = round(20 * np.log10(pk + 1e-9), 1)
        print(f"    peak level: {res['audio_peak_dbfs']} dBFS "
              "(aim for -12..-3; adjust J-6 VOLUME)")

    j6.all_off()
    j6.close()

    # Verdict
    print("\n==== Verdict ====")
    chord_auto = len(res.get("midi_in_chord_out", [])) > 1
    phrase_auto = len(res.get("midi_in_phrase_out", [])) > 3
    phrase_local = len(res.get("local_phrase_out", [])) > 3
    res["chord_mode"] = "auto" if chord_auto else (
        "local" if len(res.get("local_chord_out", [])) > 1 else "audio")
    res["phrase_mode"] = "auto" if phrase_auto else (
        "local" if phrase_local else "audio")
    res["clock_source"] = "j6" if res.get("phrase_clock_msgs", 0) > 10 else "ours"
    print(f"  chord capture : {res['chord_mode']}")
    print(f"  phrase capture: {res['phrase_mode']}")
    print(f"  timebase      : {'J-6 clock' if res['clock_source']=='j6' else 'we send clock'}")
    os.makedirs(c.RAW, exist_ok=True)
    with open(os.path.join(c.RAW, "preflight.json"), "w") as f:
        json.dump(res, f, indent=1)
    print(f"  saved {os.path.join(c.RAW, 'preflight.json')}")


if __name__ == "__main__":
    main()
