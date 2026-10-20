# J-6 capture kit

These scripts record what the Roland J-6 does so the FM1 firmware can reproduce
it. They only **listen** to the J-6 and send it ordinary notes, sustain (CC64)
and MIDI clock. All three are normal messages from the J-6's own MIDI chart.

## Safety (the J-6 is sold — keep it pristine)
- Connect it with **USB-C only**. Never hold SHIFT while powering on, because that starts update mode.
- Never open the case. Nothing here needs it.
- The scripts **refuse** to send Program Change, SysEx or anything else (`J6.send` in `j6cap.py`).
- **Before you start:** take a photo of every menu setting (SHIFT+B), so you can restore them.
- Use **empty pattern slots** for the sequencer probes. After the session, put the settings back, or do a factory reset if you planned one before handing it over.

## Setup (once)
```bash
cd capture
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
```

## Session order (about 1 day total, resumable at any point)
Every script saves each item as soon as it's captured. You can quit at any prompt with `q` and re-run the same command later; finished items are skipped. To retake an item, pass `--redo <key>`.

| # | Command | Time | You do |
|---|---------|------|--------|
| 1 | `.venv/bin/python preflight.py` | 10 min | Follow the prompts. The run ends with a verdict for steps 2 and 3. |
| 2 | `.venv/bin/python capture_chords.py` | ~40 min | Select chord sets 1–100 one at a time. |
| 3 | `.venv/bin/python capture_phrases.py` | ~90 min | Set STYLE/VARIATION 9×12 times (TEMPO 120.0). |
| 4 | `.venv/bin/python capture_probes.py --only seq` | ~60 min | Program small test patterns in empty slots. |
| 5 | `.venv/bin/python capture_probes.py --only voice` and `--only velocity` / `--only hold` | 10 min | Pick a pad sound. |
| 6 | `.venv/bin/python capture_probes.py --only fx` | 15 min | Set delay/reverb knobs. |
| 7 | `.venv/bin/python capture_probes.py --only sounds` | 20 min | Select each of the 64 sounds (optional, lowest priority). |

**If the preflight verdict says:**
- `chord capture: local`: run `capture_chords.py --mode local`, and press the 12 keys yourself for each set.
- `phrase capture: audio`: run `capture_phrases.py --audio`. Onsets will be found in the audio.
- `timebase: we send clock`: add `--clock ours`, with the J-6 SYnC set to AUTO.

Also **film a short phone video** of each feature area: menu text, LED behavior when switching patterns, anything odd. Those details aren't in MIDI.

## Back up after every session
Copy `capture/raw/` somewhere safe, such as a cloud drive or a USB stick. This data can't be recaptured once the J-6 is gone.

## Testing without the J-6
`fake_j6.py` creates a virtual "J-6 FAKE" MIDI port that answers with chords and arpeggios:
```bash
.venv/bin/python fake_j6.py &          # then:
.venv/bin/python capture_chords.py --port FAKE --sets 1-2
```
Delete `raw/` afterwards so fake data isn't mixed with the real captures.
