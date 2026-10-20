# SPDX-License-Identifier: GPL-3.0-only
"""A crude fake J-6 on a virtual MIDI port, for testing the capture scripts
without the real device.  One held note -> answers with a 4-note chord;
two or more held notes -> arpeggiates them in 8ths at 120 BPM.  Always sends
MIDI clock at 120 BPM.  Run it in one terminal, the capture script in another
with --port FAKE.
"""
import threading
import time

import mido

NAME = "J-6 FAKE"
held = set()
lock = threading.Lock()


def main():
    out = mido.open_output(NAME, virtual=True)

    def on_msg(m):
        with lock:
            if m.type == "note_on" and m.velocity > 0:
                held.add(m.note)
                if len(held) == 1:
                    for k in (0, 4, 7, 11):
                        out.send(mido.Message("note_on", note=m.note + k, velocity=m.velocity))
            elif m.type in ("note_off", "note_on"):
                if len(held) == 1 and m.note in held:
                    for k in (0, 4, 7, 11):
                        out.send(mido.Message("note_off", note=m.note + k))
                held.discard(m.note)

    mido.open_input(NAME, virtual=True, callback=on_msg)
    print(f"fake J-6 running as '{NAME}' (Ctrl-C to stop)")
    tick, step, last = 0, 0, None
    period = 60 / (120 * 24)
    t0 = time.perf_counter()
    while True:
        while time.perf_counter() - t0 < tick * period:
            time.sleep(0.0005)
        out.send(mido.Message("clock"))
        if tick % 12 == 0:
            with lock:
                notes = sorted(held)
                if last is not None:
                    out.send(mido.Message("note_off", note=last))
                    last = None
                if len(notes) >= 2:
                    last = notes[step % len(notes)]
                    out.send(mido.Message("note_on", note=last, velocity=90))
                    step += 1
        tick += 1


if __name__ == "__main__":
    main()
