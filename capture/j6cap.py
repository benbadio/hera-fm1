# SPDX-License-Identifier: GPL-3.0-only
"""Shared helpers for the J-6 capture scripts.

Safety: these tools only open the J-6 as a normal class-compliant USB MIDI and
audio device. They send nothing but Note On/Off, CC64 (Hold) and MIDI
real-time clock/start/stop -- all listed on the J-6 MIDI implementation chart.
They never send Program Change, SysEx, or anything else, and never touch the
update / USB-drive mode.
"""
import argparse
import json
import os
import subprocess
import sys
import threading
import time

import mido

ROOT = os.path.dirname(os.path.abspath(__file__))
RAW = os.path.join(ROOT, "raw")

NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]

# Messages we are allowed to send. Anything else is refused.
_ALLOWED_OUT = {"note_on", "note_off", "clock", "start", "stop"}


def note_name(n):
    return f"{NOTE_NAMES[n % 12]}{n // 12 - 1}"


def now_ns():
    return time.perf_counter_ns()


def base_args(desc):
    p = argparse.ArgumentParser(description=desc)
    p.add_argument("--port", default="J-6",
                   help="substring of the J-6 MIDI port name (default: J-6)")
    p.add_argument("--channel", type=int, default=1,
                   help="J-6 synth MIDI channel, 1-16 (default 1)")
    p.add_argument("--redo", action="append", default=[],
                   help="re-capture an item key even if already done")
    return p


def find_port(names, sub):
    hits = [n for n in names if sub.lower() in n.lower()]
    if not hits:
        sys.exit(f"No MIDI port containing '{sub}'. Seen: {names}\n"
                 "Is the J-6 connected over USB-C and powered on?")
    return hits[0]


class J6:
    """Open MIDI in/out to the J-6 and record everything it sends."""

    def __init__(self, port_sub, channel=1):
        self.channel = channel - 1
        self.in_name = find_port(mido.get_input_names(), port_sub)
        self.out_name = find_port(mido.get_output_names(), port_sub)
        self._lock = threading.Lock()
        self._events = []
        self.inp = mido.open_input(self.in_name, callback=self._on_msg)
        self.out = mido.open_output(self.out_name)

    def _on_msg(self, msg):
        t = now_ns()
        if msg.type == "active_sensing":
            return
        with self._lock:
            self._events.append((t, msg))

    # -- recording ---------------------------------------------------
    def clear(self):
        with self._lock:
            self._events = []

    def take(self):
        with self._lock:
            ev, self._events = self._events, []
        return ev

    # -- sending (whitelisted) ---------------------------------------
    def send(self, msg):
        if msg.type not in _ALLOWED_OUT and not (
                msg.type == "control_change" and msg.control == 64):
            raise RuntimeError(f"refusing to send {msg.type} to the J-6")
        self.out.send(msg)

    def note_on(self, note, vel=100):
        self.send(mido.Message("note_on", channel=self.channel, note=note,
                               velocity=vel))

    def note_off(self, note):
        self.send(mido.Message("note_off", channel=self.channel, note=note,
                               velocity=0))

    def hold(self, on):
        self.send(mido.Message("control_change", channel=self.channel,
                               control=64, value=127 if on else 0))

    def all_off(self, notes=range(128)):
        for n in notes:
            self.note_off(n)
        self.hold(False)

    def close(self):
        try:
            self.inp.close()
            self.out.close()
        except Exception:
            pass


class ClockSender(threading.Thread):
    """Send 24 ppqn MIDI clock at a fixed BPM on an absolute schedule."""

    def __init__(self, j6, bpm, send_start=False):
        super().__init__(daemon=True)
        self.j6, self.bpm, self.send_start = j6, bpm, send_start
        self._halt = threading.Event()
        self.t0 = None

    def run(self):
        period = 60e9 / (self.bpm * 24)
        self.t0 = now_ns()
        if self.send_start:
            self.j6.send(mido.Message("start"))
        k = 0
        while not self._halt.is_set():
            target = self.t0 + k * period
            while True:
                d = target - now_ns()
                if d <= 0:
                    break
                time.sleep(min(d / 1e9, 0.0005) if d > 1_000_000 else 0)
            self.j6.send(mido.Message("clock"))
            k += 1

    def stop(self):
        self._halt.set()
        self.join(1)
        if self.send_start:
            self.j6.send(mido.Message("stop"))


def ev_to_json(events, t0):
    """Serialise recorded events relative to t0 (ns -> microseconds)."""
    return [{"us": (t - t0) // 1000, "msg": m.hex()} for t, m in events]


class Store:
    """Append-only JSONL store; one record per captured item, resumable."""

    def __init__(self, name):
        os.makedirs(RAW, exist_ok=True)
        self.path = os.path.join(RAW, name + ".jsonl")
        self.done = set()
        if os.path.exists(self.path):
            with open(self.path) as f:
                for line in f:
                    line = line.strip()
                    if line:
                        try:
                            self.done.add(json.loads(line)["key"])
                        except Exception:
                            pass

    def has(self, key, redo=()):
        return key in self.done and key not in redo

    def put(self, key, **rec):
        rec = {"key": key, "wall": time.strftime("%Y-%m-%dT%H:%M:%S"), **rec}
        with open(self.path, "a") as f:
            f.write(json.dumps(rec) + "\n")
            f.flush()
            os.fsync(f.fileno())
        self.done.add(key)


CHIME = "/System/Library/Sounds/Glass.aiff"


def chime():
    """Play a short sound so you know the script is waiting for you.
    Set J6_CHIME=0 to turn it off."""
    if os.environ.get("J6_CHIME", "1") == "0":
        return
    try:
        subprocess.Popen(["afplay", CHIME], stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL)
    except Exception:
        print("\a", end="", flush=True)


def ask(prompt):
    """Prompt the user. Returns 'go', 'redo', 'skip' or exits on quit."""
    chime()
    while True:
        try:
            a = input(f"\n>> {prompt}\n   [Enter]=go  r=redo last  s=skip  q=quit : ")
        except EOFError:
            a = "q"
        a = a.strip().lower()
        if a in ("", "y", "g"):
            return "go"
        if a == "r":
            return "redo"
        if a == "s":
            return "skip"
        if a == "q":
            print("Stopped. Everything captured so far is saved; re-run to resume.")
            sys.exit(0)


def notes_on(events, channel):
    """Note-on events (time_ns, note, vel) on a channel."""
    out = []
    for t, m in events:
        if m.type == "note_on" and m.velocity > 0 and m.channel == channel:
            out.append((t, m.note, m.velocity))
    return out


class AudioRec:
    """Record from the J-6 USB audio input to a WAV file (optional)."""

    def __init__(self, dev_sub="J-6", rate=44100, enabled=True):
        self.enabled = enabled
        self.dev = None
        self.rate = rate
        if not enabled:
            return
        import sounddevice as sd
        for i, d in enumerate(sd.query_devices()):
            if dev_sub.lower() in d["name"].lower() and d["max_input_channels"] >= 2:
                self.dev = i
                break
        if self.dev is None:
            print(f"!! No audio input containing '{dev_sub}' -- audio disabled.")
            self.enabled = False

    def record(self, path, seconds, during=None):
        """Record `seconds` of audio while running `during()` in parallel."""
        if not self.enabled:
            if during:
                during()
            return None
        import sounddevice as sd
        import soundfile as sf
        os.makedirs(os.path.dirname(path), exist_ok=True)
        frames = int(seconds * self.rate)
        buf = sd.rec(frames, samplerate=self.rate, channels=2, device=self.dev,
                     dtype="int32")
        t0 = now_ns()
        if during:
            during()
        sd.wait()
        sf.write(path, buf, self.rate, subtype="PCM_24")
        return t0
