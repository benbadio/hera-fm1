# SPDX-License-Identifier: GPL-3.0-only
"""Guided behaviour probes: sequencer, voices, MIDI, effects and sounds.

Each probe tells you what to set up on the J-6 (always in an EMPTY pattern
slot, so your own patterns are never overwritten), then records the J-6's
MIDI output and, where useful, its USB audio.

    python capture_probes.py --list            # show probes
    python capture_probes.py                   # run all not yet done
    python capture_probes.py --only seq_tie    # run one group/probe

Output: raw/probes.jsonl (+ raw/probes/*.wav). Resumable.
"""
import time

import j6cap as c

C4, E4, G4, B4 = 60, 64, 67, 71

# kind: "play"  -> you start the J-6 (PLAY); we record `secs`
#       "send"  -> we send `seq` = [(t_s, note, vel, dur_s), ...]
#       "rec"   -> you arm REC+PLAY; we send `seq` 1 bar after Enter
# variants: the probe is repeated once per label (you set each on the J-6)
# audio: also record USB audio
PROBES = [
    # ---- sequencer ---------------------------------------------------------
    dict(key="seq_basic", kind="play", secs=10,
         setup="Empty pattern, TEMPO 120.0, shuffle 0, bEAt 1-4. Step-enter C4 "
               "on steps 1, 3, 5, 7 (8 steps total, LAST=8). Stop at step 1."),
    dict(key="seq_tie", kind="play", secs=10,
         setup="Same pattern as seq_basic, but make step 1 tie into step 2 "
               "(HOLD on step 1), and step 5 tie through steps 6 and 7."),
    dict(key="seq_chord_step", kind="play", secs=10,
         setup="Empty pattern, 8 steps. CHORD ON with chord set 1: enter a "
               "chord (key C) on step 1 and (key F) on step 5."),
    dict(key="seq_poly_step", kind="play", secs=10,
         setup="Empty pattern, 8 steps, CHORD OFF: enter C4+E4+G4+B4 together "
               "on step 1 and a single D4 on step 5."),
    dict(key="seq_style_step", kind="play", secs=16,
         setup="Empty pattern, 16 steps. Step 1: C4+E4+G4 with STYLE ON, style 1 "
               "var 1. Step 9: F4+A4+C5 with STYLE OFF (should keep the phrase). "
               "Step 13: G4 with STYLE ON style 2 var 3."),
    dict(key="seq_last", kind="play", secs=10,
         setup="seq_basic pattern, then set LAST to step 5 (SHIFT+F)."),
    dict(key="seq_beat", kind="play", secs=12,
         variants=["1-4", "1-3", "1-2", "1-8"],
         setup="seq_basic pattern with bEAt set to the value shown."),
    dict(key="seq_shuffle", kind="play", secs=10,
         variants=["min", "25%", "50%", "75%", "max"],
         setup="Empty pattern, 16 steps, C4 on EVERY step, bEAt 1-4. Set "
               "SHUFFLE (SHIFT+E) to the amount shown; type the displayed "
               "value when asked."),
    dict(key="seq_tempo_fine", kind="play", secs=8, variants=["120.5", "87.3"],
         setup="seq_basic pattern with TEMPO set to the value shown."),
    dict(key="seq_velocity", kind="play", secs=10,
         setup="Empty pattern: enter C4 on steps 1-8 using different key "
               "velocities if the J-6 records them, plus ACCENT on step 3 if "
               "available (check A.vLo in menu)."),
    dict(key="seq_64", kind="play", secs=36,
         setup="Empty pattern: C4 on step 1, then use DUPL until the pattern is "
               "64 steps; put E4 on step 64."),
    dict(key="seq_pchg", kind="play", secs=20, variants=["STEP", "1"],
         setup="Two empty patterns A (C4 on every 4th step) and B (G4 on every "
               "4th step). Set P.chg to the value shown. After Enter: press PLAY "
               "on A, then select B halfway through a bar."),
    dict(key="seq_countin", kind="play", secs=10,
         setup="Set Cnt.I = 2. After Enter press REC then PLAY on an empty "
               "pattern and let it count in (don't play anything)."),
    dict(key="seq_rt_record", kind="rec", audio=False,
         seq=[(0.00, C4, 100, 0.20), (0.27, E4, 80, 0.10), (0.49, G4, 120, 0.4),
              (1.13, B4, 60, 0.05), (1.52, C4, 100, 0.40)],
         setup="Empty pattern, 8 steps, TEMPO 120, Cnt.I OFF. After Enter press "
               "REC then PLAY; I will play 5 notes. Then press PLAY to stop, "
               "and when asked, press PLAY again to play the recording back."),
    # ---- voices / MIDI -----------------------------------------------------
    dict(key="voice_steal", kind="send", audio=True,
         seq=[(0.0, 48, 100, 4.0), (0.4, 52, 100, 3.6), (0.8, 55, 100, 3.2),
              (1.2, 59, 100, 2.8), (1.6, 62, 100, 2.4), (2.0, 65, 100, 2.0)],
         setup="STYLE OFF, CHORD OFF, a sustained sound with long release "
               "(e.g. a pad). Delay and reverb OFF."),
    dict(key="voice_retrigger", kind="send", audio=True,
         seq=[(0.0, C4, 100, 0.3), (0.35, C4, 100, 0.3), (0.7, C4, 100, 1.0)],
         setup="Same sound as voice_steal."),
    dict(key="velocity_curve", kind="send", audio=True,
         seq=[(i * 0.5, C4, v, 0.3) for i, v in
              enumerate([1, 16, 32, 48, 64, 80, 96, 112, 127])],
         setup="Same sound. Menu vELo as you normally use it."),
    dict(key="hold_cc64", kind="send", audio=True, cc64=(0.2, 2.0),
         seq=[(0.0, C4, 100, 0.5), (0.6, E4, 100, 0.5), (2.5, G4, 100, 0.5)],
         setup="Same sound; I will send sustain (CC64) on at 0.2 s, off at 2 s."),
    # ---- effects ------------------------------------------------------------
    dict(key="fx_delay", kind="send", audio=True,
         variants=["time min", "time mid", "time max", "d.Syn on"],
         seq=[(0.0, 72, 120, 0.05)], tail=5,
         setup="A short plucky sound. REVERB level 0, DELAY level max, delay "
               "time as shown."),
    dict(key="fx_reverb", kind="send", audio=True,
         variants=["time min", "time mid", "time max"],
         seq=[(0.0, 72, 120, 0.05)], tail=6,
         setup="Same sound. DELAY level 0, REVERB level max, time as shown."),
]

SOUND_PROBE = dict(kind="send", audio=True, tail=2,
                   seq=[(0.0, 48, 100, 2.0), (0.0, 60, 100, 2.0),
                        (0.0, 64, 100, 2.0), (0.0, 67, 100, 2.0),
                        (3.0, 72, 110, 0.15), (3.25, 76, 90, 0.15),
                        (3.5, 79, 70, 0.15), (3.75, 84, 50, 0.6)])


def run_send(j6, seq, cc64=None):
    t0 = time.perf_counter()
    ev = []
    for t, n, v, d in seq:
        ev.append((t, "on", n, v))
        ev.append((t + d, "off", n, 0))
    if cc64:
        ev.append((cc64[0], "cc", 127, 0))
        ev.append((cc64[1], "cc", 0, 0))
    ev.sort(key=lambda e: e[0])
    for t, kind, n, v in ev:
        while time.perf_counter() - t0 < t:
            time.sleep(0.0005)
        if kind == "on":
            j6.note_on(n, v)
        elif kind == "off":
            j6.note_off(n)
        else:
            j6.hold(n > 0)
    return max(e[0] for e in ev)


def run_probe(j6, rec, store, p, key, label=None):
    extra = {}
    if label:
        extra["label"] = label
        typed = input(f"   (optional) value shown on the J-6 display for "
                      f"'{label}': ").strip()
        if typed:
            extra["display"] = typed
    j6.clear()
    t0 = c.now_ns()
    wav = f"{c.RAW}/probes/{key}.wav" if p.get("audio") and rec.enabled else None
    if p["kind"] == "play":
        print(f"   recording {p['secs']} s -- start the J-6 now.")
        if wav:
            rec.record(wav, p["secs"])
        else:
            time.sleep(p["secs"])
    elif p["kind"] == "send":
        dur = max(t + d for t, _, _, d in p["seq"]) + p.get("tail", 1.5)

        def go():
            run_send(j6, p["seq"], p.get("cc64"))
        if wav:
            rec.record(wav, dur, go)
        else:
            go()
            time.sleep(p.get("tail", 1.5))
    elif p["kind"] == "rec":
        print("   press REC then PLAY now; notes start in 2 s (wait for bar 1)...")
        time.sleep(2.0)
        run_send(j6, p["seq"])
        time.sleep(1.0)
        input("   Stop the J-6. Press Enter, then PLAY to play back the take: ")
        mark = (c.now_ns() - t0) // 1000
        time.sleep(8)
        extra["playback_from_us"] = mark
    ev = j6.take()
    ons = c.notes_on(ev, j6.channel)
    print(f"   captured {len(ev)} MIDI events ({len(ons)} note-ons)"
          f"{', audio ' + wav if wav else ''}")
    store.put(key, probe=p.get("key"), setup=p.get("setup"), seq=p.get("seq"),
              wav=wav, events=c.ev_to_json(ev, t0), **extra)


def main():
    ap = c.base_args(__doc__)
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--only", default=None, help="probe key prefix, or 'sounds'")
    ap.add_argument("--no-audio", action="store_true")
    a = ap.parse_args()
    if a.list:
        for p in PROBES:
            print(f"{p['key']:<16} {p['kind']:<5} {p.get('variants', '')}")
        print(f"{'sounds':<16} send  64 sounds (8 banks x 8)")
        return
    store = c.Store("probes")
    j6 = c.J6(a.port, a.channel)
    rec = c.AudioRec(enabled=not a.no_audio)

    items = []
    for p in PROBES:
        for lab in p.get("variants", [None]):
            k = p["key"] + (f"_{lab.replace(' ', '').replace('%', 'pc')}" if lab else "")
            items.append((k, p, lab, p["setup"]))
    for bank in range(1, 9):
        for num in range(1, 9):
            p = dict(SOUND_PROBE, key="sound")
            items.append((f"sound_{bank}{num}", p, None,
                          f"Select SOUND {bank}-{num} (SOUND + step {bank}, then "
                          f"step {num}, or turn VALUE). STYLE/CHORD OFF, delay "
                          "and reverb at your normal settings."))
    if a.only:
        items = [it for it in items if it[0].startswith(a.only)
                 or (a.only == "sounds" and it[0].startswith("sound_"))]

    for k, p, lab, setup in items:
        if store.has(k, a.redo):
            continue
        txt = setup + (f"\n   >>> Set: {lab}" if lab else "")
        while True:
            r = c.ask(f"[{k}] {txt}")
            if r == "skip":
                break
            run_probe(j6, rec, store, p, k, lab)
            again = input("   Keep it? [Enter]=yes  r=retake : ").strip().lower()
            if again != "r":
                break
            a.redo.append(k)
        time.sleep(0.2)

    j6.all_off()
    j6.close()
    print("Done. Data in", store.path)


if __name__ == "__main__":
    main()
