/* SPDX-License-Identifier: GPL-3.0-only */
/* MORPHEUS: Authentic M-VAVE FM-1 In-Browser Emulator */

(() => {
  'use strict';

  // Constants
  // Constants
  const NUM_TRACKS = 8;
  const TOTAL_PATS = 32;
  const MAX_STEPS = 32;
  const ENGINE_NAMES = ['303A', '303B', 'D808', 'D909', 'BRK', 'FM', 'SUB', 'MIDI'];
  const DEFAULT_ENGINES = [0, 1, 2, 3, 4, 5, 6, 7];
  const DEFAULT_MIDI_CH = [1, 2, 10, 10, 5, 6, 7, 8];
  const DRUM_LABELS = ['ACC', 'CH', 'OH', 'CY', 'CB', 'CP', 'RS', 'HT', 'MT', 'LT', 'SD', 'BD'];
  const DRUM_FULL_NAMES = [
    'ACCENT', 'CLOSED HAT', 'OPEN HAT', 'CYMBAL', 'COWBELL', 'HANDCLAP',
    'RIM SHOT', 'HI TOM', 'MID TOM', 'LOW TOM', 'SNARE DRUM', 'BASS DRUM'
  ];
  const DRUM_LANE_BD = 11;

  // Global State
  const state = {
    bpm: 128,
    masterVol: 0.8,
    isRunning: false,
    activeTrack: 0,
    activeStep: 0,
    viewMode: 'keys', // 'keys' or 'curve'
    viewOctave: 3,    // C3..B3
    viewPage: 0,      // 0 = 1-16, 1 = 17-32
    recMode: false,
    shiftMode: false,
    drumLaneSel: [11, 11, 11, 11, 11, 11, 11, 11], // 11=BD default
    menuMode: 'none', // 'none' or 'fx'
    menuSel: 0,       // active item in modal menu
    screenEditEnabled: false, // Hardware faithful by default; toggleable
    statusMsg: null,
    statusTimer: 0,
    heldStatus: null,
    clipboard: null,
    hasStarted: false,
    splashUntil: 0,
    tracks: []
  };

  function dismissSplash() {
    if (state.splashUntil > 0) {
      state.splashUntil = 0;
    }
  }

  const NOTE_NAMES_SHORT = ['C-', 'C#', 'D-', 'D#', 'E-', 'F-', 'F#', 'G-', 'G#', 'A-', 'A#', 'B-'];
  function getNoteName(midiNote) {
    const semi = midiNote % 12;
    const oct = Math.floor(midiNote / 12) - 1;
    return `${NOTE_NAMES_SHORT[semi]}${oct}`;
  }

  function showStatus(msg, durationMs = 1800) {
    state.statusMsg = msg;
    state.statusTimer = Date.now() + durationMs;
    showHint(msg);
  }

  function setHeldStatus(msg) {
    state.heldStatus = msg;
    showHint(msg);
  }

  function clearHeldStatus() {
    state.heldStatus = null;
    resetHint();
  }

  // Tempo format helper: cap tempo display to 1 decimal place max (e.g. 128 or 128.5)
  function formatBpm(bpm) {
    const n = Number(bpm);
    if (isNaN(n)) return '120';
    return n % 1 === 0 ? n.toString() : n.toFixed(1);
  }

  // Factory Patterns initialization for 8 ReBirth tracks
  function createDefaultPattern(trackIdx) {
    const pat = {
      length: 16,
      resolution: 16,
      shuffle: 0,
      midiCh: DEFAULT_MIDI_CH[trackIdx],
      mute: false,
      curveCC: 74,
      engine: DEFAULT_ENGINES[trackIdx],
      steps: []
    };

    for (let s = 0; s < MAX_STEPS; s++) {
      pat.steps.push({ note: 48, gate: 0, tie: 0, curve: 64, drums: 0 });
    }

    if (trackIdx === 0) {
      // Track 1: TB-303 Acid Bass A
      const notes  = [48, 48, 60, 58, 55, 55, 53, 55, 51, 51, 53, 53, 48, 60, 58, 55];
      const gates  = [110,  0, 127, 95, 115,  0, 100, 120, 90,  0, 105, 115, 120, 127, 95,  0];
      const ties   = [  0,  0,   1,  0,   0,  0,   0,   1,  0,  0,   0,   1,   0,   1,  0,  0];
      const curves = [ 30, 45, 120, 80,  60, 40,  75, 110, 35, 20,  70, 100, 115, 125, 80, 40];
      for (let s = 0; s < 16; s++) {
        pat.steps[s] = { note: notes[s], gate: gates[s], tie: ties[s], curve: curves[s], drums: 0 };
      }
    } else if (trackIdx === 1) {
      // Track 2: TB-303 Acid Bass B (Counter-line)
      const notes  = [60, 63, 60, 65, 60, 67, 65, 63, 60, 58, 60, 63, 67, 70, 67, 65];
      const gates  = [  0, 115,  0, 120,  0, 127, 110,  0, 105,  0, 115,  0, 125,  0, 110,  0];
      const ties   = [  0,   0,  0,   1,  0,   0,   1,  0,   0,  0,   1,  0,   0,  0,   1,  0];
      const curves = [ 60,  75, 40,  95, 50, 115, 100, 60,  80, 50,  90, 70, 120, 85, 105, 50];
      for (let s = 0; s < 16; s++) {
        pat.steps[s] = { note: notes[s], gate: gates[s], tie: ties[s], curve: curves[s], drums: 0 };
      }
    } else if (trackIdx === 2) {
      // Track 3: TR-808 Drum Machine Kit
      for (let s = 0; s < 16; s++) {
        let mask = (1 << 1); // Closed Hat (lane 1, bit 10)
        if (s === 0 || s === 8 || s === 10) mask |= (1 << 0) | (1 << 11); // BD (bit 0) + ACC (bit 11)
        else if (s === 4 || s === 12) mask |= (1 << 0) | (1 << 1); // BD (bit 0) + SD (bit 1)
        else if (s === 6 || s === 14) mask |= (1 << 5); // CP (bit 5)
        if (s === 2 || s === 10) mask |= (1 << 2); // OH (bit 2)
        pat.steps[s] = {
          note: 36,
          gate: (mask & (1 << 11)) ? 127 : 100,
          tie: 0,
          curve: 64,
          drums: mask
        };
      }
    } else if (trackIdx === 3) {
      // Track 4: TR-909 Drum Machine Kit
      for (let s = 0; s < 16; s++) {
        let mask = 0;
        if (s === 0 || s === 4 || s === 8 || s === 12) mask |= (1 << 0); // 4-on-the-floor BD
        if (s === 4 || s === 12) mask |= (1 << 1) | (1 << 5); // SD + CP
        if (s % 2 === 1) mask |= (1 << 2); // Offbeat OH
        if (s === 0 || s === 8) mask |= (1 << 3) | (1 << 11); // CY + ACC
        pat.steps[s] = {
          note: 38,
          gate: (mask & (1 << 11)) ? 127 : 105,
          tie: 0,
          curve: 80,
          drums: mask
        };
      }
    } else if (trackIdx === 4) {
      // Track 5: Breakbeat Slicer (amen loop slices 0..7)
      for (let s = 0; s < 16; s++) {
        pat.steps[s] = {
          note: (s % 8),
          gate: (s % 2 === 0) ? 120 : 95,
          tie: (s === 7 || s === 15) ? 1 : 0,
          curve: 64,
          drums: 0
        };
      }
    } else if (trackIdx === 5) {
      // Track 6: 2-Op FM Lead Synth
      const notes = [72, 72, 75, 75, 77, 79, 77, 75, 72, 70, 72, 75, 79, 82, 79, 77];
      const gates = [100, 100, 110, 110, 120, 125, 115, 105, 95, 95, 110, 115, 127, 120, 110, 100];
      for (let s = 0; s < 16; s++) {
        pat.steps[s] = {
          note: notes[s],
          gate: gates[s],
          tie: (s === 3 || s === 6 || s === 11 || s === 14) ? 1 : 0,
          curve: 70 + (s * 3),
          drums: 0
        };
      }
    } else if (trackIdx === 6) {
      // Track 7: Sub Synth Rolling Arp
      const notes = [36, 43, 48, 43, 39, 43, 46, 43, 41, 44, 48, 44, 43, 46, 50, 46];
      for (let s = 0; s < 16; s++) {
        pat.steps[s] = {
          note: notes[s],
          gate: (s % 2 === 0) ? 95 : 85,
          tie: 0,
          curve: 50 + (s * 4),
          drums: 0
        };
      }
    } else if (trackIdx === 7) {
      // Track 8: External MIDI Lead
      for (let s = 0; s < 16; s++) {
        pat.steps[s] = {
          note: 60 + ((s * 5) % 24),
          gate: (s % 4 === 0) ? 120 : (s % 2 === 0 ? 90 : 0),
          tie: 0,
          curve: 64,
          drums: 0
        };
      }
    }

    return pat;
  }

  function initTracks() {
    state.tracks = [];
    for (let t = 0; t < NUM_TRACKS; t++) {
      const trk = {
        curBank: 0,
        curPat: 0,
        nextBank: 0,
        nextPat: 0,
        curStep: 0,
        engine: DEFAULT_ENGINES[t],
        patterns: []
      };
      for (let p = 0; p < TOTAL_PATS; p++) {
        trk.patterns.push(createDefaultPattern(t));
      }
      state.tracks.push(trk);
    }

    try {
      const saved = localStorage.getItem('morpheus_patterns_v2');
      if (saved) {
        const parsed = JSON.parse(saved);
        if (Array.isArray(parsed) && parsed.length === NUM_TRACKS) {
          state.tracks = parsed;
        }
      }
    } catch (e) {
      console.warn('Could not load localStorage:', e);
    }
  }

  initTracks();

  // ===========================================================================
  // Live Hint System
  // ===========================================================================
  const hintText = document.getElementById("hintText");
  const DEFAULT_HINT = '💡 Click "Switch on" then tap PLAY. Drag K1 Cutoff and K2 Res, or click the screen grid to add notes!';

  function showHint(txt) {
    if (hintText) hintText.textContent = txt;
  }

  function resetHint() {
    if (hintText) hintText.textContent = DEFAULT_HINT;
  }

  const HINTS = {
    MASTER: "🔊 MASTER: Adjusts hardware audio volume (0–100%).",
    SELECT: "📍 SELECT: Scrubs active step cursor. In FX Menu: Selects tool. With EDIT: Transpose ±12 semitones.",
    TEMPO: "⏱️ TEMPO: Adjusts sequencer tempo from 50 to 240 BPM. With EDIT: Swing / Shuffle (0–75%).",
    TRACK: "🔀 TRACK: Switches between the 4 independent sequencer tracks (T1: Bass, T2: Lead, T3: Arp, T4: Drums).",
    KNOB1: "🎛️ KNOB 1 (Cutoff): Shapes 303 lowpass filter cutoff. In EDIT mode: Changes active step note pitch.",
    KNOB2: "🎛️ KNOB 2 (Resonance): Diode ladder resonance squelch. In EDIT mode: Toggles tied note portamento slide.",
    KNOB3: "🎛️ KNOB 3 (Decay): Envelope decay time. In CURVE mode: Adjusts CC modulation depth.",
    KNOB4: "🎛️ KNOB 4 (Accent): 303 accent overdrive amount. In EDIT mode: Adjusts track length (1–32 steps).",
    "OCT-": "🎹 OCTAVE DOWN: Scrolls the 12-semitone grid octave window downward (C1 to B5).",
    "OCT+": "🎹 OCTAVE UP: Scrolls the 12-semitone grid octave window upward (C1 to B5).",
    PLAY: "▶️ PLAY / STOP: Starts or pauses sequencer playback across all 4 synced tracks.",
    REC: "🔴 REC (Step Record): Tap REC, then press silicone keys to punch in notes step-by-step and auto-advance!",
    FX: "⚙️ FX / Tools Menu: Toggles Pattern Tools on screen (Undo, Redo, Shift, Transpose, Alter, Randomize, Clear). Hold EDIT + FX for instant Randomize.",
    SEL: "📄 SEL (Page): Toggles between Page 1 (Steps 1–16) and Page 2 (Steps 17–32).",
    ENV: "📈 ENV (Mode): Switches between KEYS view (note pitches) and CURVE view (filter modulation curves).",
    LFO: "🔊 LFO (Engine): Cycles sound engines: 303 Acid Bass, Subtractive Lead, 2-OP FM, or silent MUTE mode.",
    EDIT: "⚡ EDIT (Shift): Secondary modifier button. Hold with HOME for Undo, with SAVE for Redo, or with FX for Randomize.",
    GLO: "📏 GLO (Length): Cycles track sequence length (16, 24, or 32 steps) for polymetric grooves.",
    HOME: "🏠 HOME (Track): Cycles through Tracks T1–T4. In FX Menu: Executes selected tool. With EDIT: Instant Undo!",
    SAVE: "💾 SAVE: Writes patterns to browser storage. With EDIT: Instant Redo!",
    ARP: "📚 ARP (Bank): Cycles between Banks A, B, C, and D (each bank holds 8 patterns).",
    SEQ: "🔢 SEQ (Pattern): Cycles through patterns 1 to 8 in the active bank."
  };

  // ===========================================================================
  // Web Audio Synthesizer Engine
  // ===========================================================================
  let audioCtx = null;
  let masterGain = null;
  let synthCutoff = 80;
  let synthResonance = 95;
  let synthDecay = 70;
  let synthAccent = 90;
  let activeVoice303 = [null, null]; // Independent voices for 303A and 303B

  function initAudio() {
    if (audioCtx) return;
    try {
      const AudioContext = window.AudioContext || window.webkitAudioContext;
      audioCtx = new AudioContext({ sampleRate: 44100 });

      masterGain = audioCtx.createGain();
      masterGain.gain.setValueAtTime(state.masterVol, audioCtx.currentTime);
      masterGain.connect(audioCtx.destination);
    } catch (e) {
      console.warn('AudioContext init error:', e);
    }
  }

  function midiToFreq(m) {
    return 440 * Math.pow(2, (m - 69) / 12);
  }

  // 303 Acid Bass Voice (independent instances for 303A and 303B)
  function play303Note(voiceIdx, note, velocity, isTied, curveVal) {
    if (!audioCtx) return;
    const vIdx = voiceIdx === 1 ? 1 : 0;
    const now = audioCtx.currentTime;
    const freq = midiToFreq(note);
    const isAccent = velocity >= 105;

    const cutoffBase = (synthCutoff / 127) * 4500 + 100;
    const modOffset = (curveVal - 64) * 20;
    const accentBoost = isAccent ? ((synthAccent / 127) * 4200 + 1200) : 1800;
    const peakFreq = Math.min(12000, Math.max(150, cutoffBase + accentBoost + modOffset));
    const filterQ = (synthResonance / 127) * 18 + 1.2;
    const decayTime = 0.06 + (synthDecay / 127) * 0.40;

    let voice = activeVoice303[vIdx];

    if (isTied && voice) {
      voice.osc.frequency.cancelScheduledValues(now);
      voice.osc.frequency.linearRampToValueAtTime(freq, now + 0.055);

      voice.filter.frequency.cancelScheduledValues(now);
      voice.filter.frequency.linearRampToValueAtTime(peakFreq, now + 0.045);
      voice.filter.frequency.exponentialRampToValueAtTime(cutoffBase, now + decayTime);
      return;
    }

    if (voice) {
      try {
        voice.gain.gain.cancelScheduledValues(now);
        voice.gain.gain.linearRampToValueAtTime(0.001, now + 0.01);
        voice.osc.stop(now + 0.02);
      } catch (e) {}
    }

    const osc = audioCtx.createOscillator();
    const filter = audioCtx.createBiquadFilter();
    const gain = audioCtx.createGain();

    osc.type = (vIdx === 1) ? 'square' : 'sawtooth';
    osc.frequency.setValueAtTime(freq, now);

    filter.type = 'lowpass';
    filter.Q.setValueAtTime(filterQ, now);
    filter.frequency.setValueAtTime(peakFreq, now);
    filter.frequency.exponentialRampToValueAtTime(Math.max(80, cutoffBase), now + decayTime);

    const ampVol = (velocity / 127) * (isAccent ? 0.45 : 0.32);
    gain.gain.setValueAtTime(0.001, now);
    gain.gain.linearRampToValueAtTime(ampVol, now + 0.005);
    gain.gain.exponentialRampToValueAtTime(0.001, now + decayTime + 0.05);

    osc.connect(filter);
    filter.connect(gain);
    gain.connect(masterGain);

    osc.start(now);
    osc.stop(now + decayTime + 0.1);

    activeVoice303[vIdx] = { osc, filter, gain };
  }

  // TR-808 & TR-909 Drum Voice Engine
  function playDrumVoice(engine, lane, velocity, isAccent) {
    if (!audioCtx) return;
    const now = audioCtx.currentTime;
    const is909 = (engine === 3);
    const gainMul = (velocity / 127) * (isAccent ? 1.3 : 1.0);

    if (lane === 11) {
      // BD: Bass Drum
      const osc = audioCtx.createOscillator();
      const gain = audioCtx.createGain();
      const sweepTime = is909 ? 0.055 : 0.075;
      const decayTime = is909 ? 0.28 : 0.45;
      const startF = is909 ? 165 : 135;
      const endF = is909 ? 48 : 40;

      osc.frequency.setValueAtTime(startF, now);
      osc.frequency.exponentialRampToValueAtTime(endF, now + sweepTime);

      gain.gain.setValueAtTime(0.7 * gainMul, now);
      gain.gain.exponentialRampToValueAtTime(0.001, now + decayTime);

      osc.connect(gain);
      gain.connect(masterGain);
      osc.start(now);
      osc.stop(now + decayTime + 0.05);
    } else if (lane === 10) {
      // SD: Snare Drum
      const osc = audioCtx.createOscillator();
      const oscGain = audioCtx.createGain();
      osc.frequency.setValueAtTime(is909 ? 215 : 185, now);
      osc.frequency.exponentialRampToValueAtTime(80, now + 0.06);
      oscGain.gain.setValueAtTime(0.35 * gainMul, now);
      oscGain.gain.exponentialRampToValueAtTime(0.001, now + 0.14);
      osc.connect(oscGain);
      oscGain.connect(masterGain);
      osc.start(now);
      osc.stop(now + 0.16);

      // Noise component
      const bufSize = Math.floor(audioCtx.sampleRate * 0.22);
      const buffer = audioCtx.createBuffer(1, bufSize, audioCtx.sampleRate);
      const data = buffer.getChannelData(0);
      for (let i = 0; i < bufSize; i++) data[i] = Math.random() * 2 - 1;
      const noise = audioCtx.createBufferSource();
      noise.buffer = buffer;
      const nFilter = audioCtx.createBiquadFilter();
      nFilter.type = is909 ? 'highpass' : 'bandpass';
      nFilter.frequency.setValueAtTime(is909 ? 1500 : 2200, now);
      const nGain = audioCtx.createGain();
      nGain.gain.setValueAtTime(0.4 * gainMul, now);
      nGain.gain.exponentialRampToValueAtTime(0.001, now + (is909 ? 0.20 : 0.16));
      noise.connect(nFilter);
      nFilter.connect(nGain);
      nGain.connect(masterGain);
      noise.start(now);
      noise.stop(now + 0.22);
    } else if (lane === 9 || lane === 8 || lane === 7) {
      // Toms: LT, MT, HT
      const osc = audioCtx.createOscillator();
      const gain = audioCtx.createGain();
      const startF = lane === 9 ? 110 : (lane === 8 ? 160 : 220);
      const endF = lane === 9 ? 65 : (lane === 8 ? 95 : 145);
      osc.frequency.setValueAtTime(startF, now);
      osc.frequency.exponentialRampToValueAtTime(endF, now + 0.08);
      gain.gain.setValueAtTime(0.45 * gainMul, now);
      gain.gain.exponentialRampToValueAtTime(0.001, now + 0.25);
      osc.connect(gain);
      gain.connect(masterGain);
      osc.start(now);
      osc.stop(now + 0.28);
    } else if (lane === 6) {
      // RS: Rimshot
      const osc = audioCtx.createOscillator();
      const gain = audioCtx.createGain();
      osc.frequency.setValueAtTime(1600, now);
      gain.gain.setValueAtTime(0.5 * gainMul, now);
      gain.gain.exponentialRampToValueAtTime(0.001, now + 0.035);
      osc.connect(gain);
      gain.connect(masterGain);
      osc.start(now);
      osc.stop(now + 0.045);
    } else if (lane === 5) {
      // CP: Clap
      const bufSize = Math.floor(audioCtx.sampleRate * 0.18);
      const buffer = audioCtx.createBuffer(1, bufSize, audioCtx.sampleRate);
      const data = buffer.getChannelData(0);
      for (let i = 0; i < bufSize; i++) data[i] = Math.random() * 2 - 1;
      const noise = audioCtx.createBufferSource();
      noise.buffer = buffer;
      const filter = audioCtx.createBiquadFilter();
      filter.type = 'bandpass';
      filter.frequency.setValueAtTime(1200, now);
      const gain = audioCtx.createGain();
      gain.gain.setValueAtTime(0.45 * gainMul, now);
      gain.gain.exponentialRampToValueAtTime(0.001, now + 0.16);
      noise.connect(filter);
      filter.connect(gain);
      gain.connect(masterGain);
      noise.start(now);
      noise.stop(now + 0.18);
    } else if (lane === 4) {
      // CB: Cowbell (808 dual square waves)
      const osc1 = audioCtx.createOscillator();
      const osc2 = audioCtx.createOscillator();
      const gain = audioCtx.createGain();
      const filter = audioCtx.createBiquadFilter();
      osc1.type = 'square'; osc1.frequency.setValueAtTime(540, now);
      osc2.type = 'square'; osc2.frequency.setValueAtTime(800, now);
      filter.type = 'bandpass';
      filter.frequency.setValueAtTime(750, now);
      filter.Q.setValueAtTime(5, now);
      gain.gain.setValueAtTime(0.35 * gainMul, now);
      gain.gain.exponentialRampToValueAtTime(0.001, now + 0.28);
      osc1.connect(filter); osc2.connect(filter);
      filter.connect(gain); gain.connect(masterGain);
      osc1.start(now); osc2.start(now);
      osc1.stop(now + 0.3); osc2.stop(now + 0.3);
    } else if (lane === 1 || lane === 2 || lane === 3) {
      // Hats & Cymbal: CH, OH, CY
      const bufSize = Math.floor(audioCtx.sampleRate * (lane === 3 ? 0.6 : (lane === 2 ? 0.26 : 0.05)));
      const buffer = audioCtx.createBuffer(1, bufSize, audioCtx.sampleRate);
      const data = buffer.getChannelData(0);
      for (let i = 0; i < bufSize; i++) data[i] = Math.random() * 2 - 1;
      const noise = audioCtx.createBufferSource();
      noise.buffer = buffer;
      const filter = audioCtx.createBiquadFilter();
      filter.type = 'highpass';
      filter.frequency.setValueAtTime(lane === 3 ? 6000 : 8000, now);
      const gain = audioCtx.createGain();
      const dur = lane === 3 ? 0.55 : (lane === 2 ? 0.24 : 0.045);
      gain.gain.setValueAtTime(0.28 * gainMul, now);
      gain.gain.exponentialRampToValueAtTime(0.001, now + dur);
      noise.connect(filter);
      filter.connect(gain);
      gain.connect(masterGain);
      noise.start(now);
      noise.stop(now + dur + 0.02);
    }
  }

  // Breakbeat Slice Voice
  function playBreakSlice(slice, velocity) {
    if (!audioCtx) return;
    const now = audioCtx.currentTime;
    const osc = audioCtx.createOscillator();
    const gain = audioCtx.createGain();
    const pitches = [180, 220, 160, 260, 200, 150, 240, 190];
    osc.frequency.setValueAtTime(pitches[slice % 8], now);
    osc.frequency.exponentialRampToValueAtTime(60, now + 0.08);
    gain.gain.setValueAtTime((velocity / 127) * 0.45, now);
    gain.gain.exponentialRampToValueAtTime(0.001, now + 0.12);
    osc.connect(gain);
    gain.connect(masterGain);
    osc.start(now);
    osc.stop(now + 0.14);
  }

  // 2-Op FM Lead Voice
  function playFmLead(note, velocity, curveVal) {
    if (!audioCtx) return;
    const now = audioCtx.currentTime;
    const carrierFreq = midiToFreq(note);
    const modFreq = carrierFreq * 2;
    const modIndex = 150 + ((curveVal / 127) * 600);

    const carrier = audioCtx.createOscillator();
    const modulator = audioCtx.createOscillator();
    const modGain = audioCtx.createGain();
    const gain = audioCtx.createGain();

    carrier.frequency.setValueAtTime(carrierFreq, now);
    modulator.frequency.setValueAtTime(modFreq, now);

    modGain.gain.setValueAtTime(modIndex, now);
    modGain.gain.exponentialRampToValueAtTime(10, now + 0.25);

    modulator.connect(modGain);
    modGain.connect(carrier.frequency);

    gain.gain.setValueAtTime((velocity / 127) * 0.35, now);
    gain.gain.exponentialRampToValueAtTime(0.001, now + 0.28);

    carrier.connect(gain);
    gain.connect(masterGain);

    modulator.start(now);
    carrier.start(now);
    modulator.stop(now + 0.3);
    carrier.stop(now + 0.3);
  }

  // Sub Synth Voice
  function playSubSynth(note, velocity) {
    if (!audioCtx) return;
    const now = audioCtx.currentTime;
    const freq = midiToFreq(note);

    const subOsc = audioCtx.createOscillator();
    const mainOsc = audioCtx.createOscillator();
    const filter = audioCtx.createBiquadFilter();
    const gain = audioCtx.createGain();

    subOsc.type = 'sine';
    subOsc.frequency.setValueAtTime(freq * 0.5, now);
    mainOsc.type = 'sawtooth';
    mainOsc.frequency.setValueAtTime(freq, now);

    filter.type = 'lowpass';
    filter.frequency.setValueAtTime(450, now);
    filter.Q.setValueAtTime(3, now);

    gain.gain.setValueAtTime((velocity / 127) * 0.40, now);
    gain.gain.exponentialRampToValueAtTime(0.001, now + 0.32);

    subOsc.connect(filter);
    mainOsc.connect(filter);
    filter.connect(gain);
    gain.connect(masterGain);

    subOsc.start(now);
    mainOsc.start(now);
    subOsc.stop(now + 0.35);
    mainOsc.stop(now + 0.35);
  }

  // Sequencer Step Scheduler
  let stepTimer = null;

  function stepSequencer() {
    if (!state.isRunning) return;

    for (let t = 0; t < NUM_TRACKS; t++) {
      const trk = state.tracks[t];
      const pat = trk.patterns[trk.curBank * 8 + trk.curPat];
      if (pat.mute) continue;

      const s = trk.curStep;
      const step = pat.steps[s];
      const eng = trk.engine !== undefined ? trk.engine : pat.engine;

      if (eng === 0 || eng === 1) {
        // 303A / 303B
        if (step.gate > 0) {
          play303Note(eng, step.note, step.gate, step.tie, step.curve);
        }
      } else if (eng === 2 || eng === 3) {
        // D808 / D909
        if (step.drums > 0) {
          const isAcc = (step.drums & (1 << 11)) !== 0;
          for (let b = 0; b <= 10; b++) {
            if ((step.drums >> b) & 1) {
              const lane = 11 - b;
              playDrumVoice(eng, lane, step.gate || 100, isAcc);
            }
          }
        }
      } else if (eng === 4) {
        // BRK Breakbeat
        if (step.gate > 0) {
          playBreakSlice(step.note % 8, step.gate);
        }
      } else if (eng === 5) {
        // FM Lead
        if (step.gate > 0) {
          playFmLead(step.note, step.gate, step.curve);
        }
      } else if (eng === 6) {
        // SUB Synth
        if (step.gate > 0) {
          playSubSynth(step.note, step.gate);
        }
      }

      trk.curStep = (trk.curStep + 1) % pat.length;

      if (trk.curStep === 0) {
        if (trk.nextPat !== trk.curPat || trk.nextBank !== trk.curBank) {
          trk.curPat = trk.nextPat;
          trk.curBank = trk.nextBank;
        }
      }
    }

    const stepMs = (60000 / (state.bpm * 4));
    stepTimer = setTimeout(stepSequencer, stepMs);
  }

  function startSequencer() {
    initAudio();
    if (audioCtx && audioCtx.state === 'suspended') {
      audioCtx.resume();
    }
    state.isRunning = true;
    for (let t = 0; t < NUM_TRACKS; t++) {
      state.tracks[t].curStep = 0;
    }
    stepSequencer();
    updateButtonVisuals();
  }

  function stopSequencer() {
    state.isRunning = false;
    clearTimeout(stepTimer);
    updateButtonVisuals();
  }

  // ===========================================================================
  // 240x240 Canvas Renderer (Faithful to matrix_ui.c)
  // ===========================================================================
  const canvas = document.getElementById('matrixCanvas');
  const ctx = canvas.getContext('2d');
  ctx.imageSmoothingEnabled = false;

  const PIANO_IS_BLACK = [0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0];

  const RABBIT_32 = [
    "................................",
    ".........█████..................",
    "........███████......█████......",
    "........██...██.....███████.....",
    "........██...██.....██...██.....",
    "........██...██....██....██.....",
    "........██...██....██....██.....",
    "........██...██...██....██......",
    "........██...██...██....██......",
    ".........██.██....██...██.......",
    ".........██.██...██...██........",
    "..........███....██████.........",
    ".........██████████████.........",
    ".......██████████████████.......",
    "......████████████████████......",
    "......██.███..████.███..███.....",
    "......██.███..████.███..███.....",
    "......█████████████████████.....",
    "......████████..███████████.....",
    "......██████......█████████.....",
    ".......████████████████████.....",
    "........█████████████████.......",
    ".......██████████████████.......",
    "......████████████████████......",
    ".....██..██████████████..██.....",
    ".....██..██████████████..██.....",
    "....███...████.████.███..██.....",
    "....████..█████████████..█......",
    ".....██...██████..█████.........",
    "..........█████....████.........",
    "........███████...███████.......",
    ".......█████████.█████████......",
  ];

  function drawSplashScreen(ctx, now) {
    ctx.fillStyle = '#010502';
    ctx.fillRect(0, 0, 240, 240);

    // Matrix grid raster
    ctx.fillStyle = 'rgba(0, 24, 8, 0.45)';
    for (let y = 0; y < 240; y += 2) {
      ctx.fillRect(0, y, 240, 1);
    }

    // Double Matrix Border
    ctx.strokeStyle = '#00ff66';
    ctx.lineWidth = 1;
    ctx.strokeRect(2.5, 2.5, 235, 235);
    ctx.strokeStyle = '#004d1e';
    ctx.strokeRect(4.5, 4.5, 231, 231);

    // Rabbit Glow Halo (subtle breathing effect)
    const pulse = Math.sin(now * 0.0035) * 0.15 + 0.85;
    const radGlow = ctx.createRadialGradient(120, 62, 8, 120, 62, 54);
    radGlow.addColorStop(0, `rgba(0, 255, 102, ${0.45 * pulse})`);
    radGlow.addColorStop(0.55, `rgba(0, 200, 80, ${0.18 * pulse})`);
    radGlow.addColorStop(1, 'rgba(0, 0, 0, 0)');
    ctx.fillStyle = radGlow;
    ctx.fillRect(50, 8, 140, 110);

    // 8-Bit Pixel Rabbit (32x32 scaled to 96x96, centered at x=72, y=16)
    const scale = 3;
    const ox = 72;
    const oy = 16;
    ctx.fillStyle = '#ffffff';
    for (let ry = 0; ry < 32; ry++) {
      const row = RABBIT_32[ry];
      for (let rx = 0; rx < 32; rx++) {
        if (row[rx] === '█') {
          ctx.fillRect(ox + rx * scale, oy + ry * scale, scale, scale);
        }
      }
    }

    // Cyan Cybernetic Eye Glint (at rx=11, ry=15)
    ctx.fillStyle = '#00f0ff';
    ctx.fillRect(ox + 11 * scale, oy + 15 * scale, scale, scale);
    ctx.fillStyle = '#ffffff';
    ctx.fillRect(ox + 11 * scale + 1, oy + 15 * scale + 1, 1, 1);

    // "MORPHEUS" Typography
    ctx.textAlign = 'center';
    ctx.font = '900 20px "Courier New", monospace';
    // Green text glow shadow
    ctx.fillStyle = '#00ff66';
    ctx.fillText('MORPHEUS', 119, 137);
    ctx.fillText('MORPHEUS', 121, 137);
    ctx.fillText('MORPHEUS', 120, 136);
    ctx.fillText('MORPHEUS', 120, 138);
    // White text core
    ctx.fillStyle = '#ffffff';
    ctx.fillText('MORPHEUS', 120, 137);

    // Subtitles
    ctx.font = 'bold 8.5px "Courier New", monospace';
    ctx.fillStyle = '#00ff66';
    ctx.fillText('VERSION 1.0.0', 120, 159);

    ctx.font = 'bold 8.5px "Courier New", monospace';
    ctx.fillStyle = '#ffb71a';
    ctx.fillText('8-TRACK REBIRTH GROOVEBOX', 120, 177);

    ctx.font = 'bold 8px "Courier New", monospace';
    ctx.fillStyle = '#00f0ff';
    ctx.fillText('FM-1 OPEN // ID: 8000013', 120, 195);

    // Boot prompt / Ticker
    if (!state.hasStarted) {
      const blink = (Math.floor(now / 500) % 2 === 0);
      ctx.font = 'bold 8px "Courier New", monospace';
      ctx.fillStyle = blink ? '#50c070' : '#1e4828';
      ctx.fillText('[ CLICK "SWITCH ON" TO BOOT ]', 120, 222);
    } else {
      ctx.font = 'bold 8px "Courier New", monospace';
      ctx.fillStyle = '#00ff66';
      ctx.fillText('[ SYSTEM READY // 48kHz STEREO DSP ]', 120, 222);
    }
  }

  function renderScreen() {
    const now = performance.now();
    if (!state.hasStarted || now < state.splashUntil) {
      drawSplashScreen(ctx, now);
      return;
    }

    ctx.fillStyle = '#000000';
    ctx.fillRect(0, 0, 240, 240);

    const trkIdx = state.activeTrack;
    const trk = state.tracks[trkIdx];
    const pat = trk.patterns[trk.curBank * 8 + trk.curPat];
    const stepOffset = state.viewPage * 16;
    const curPlayStep = trk.curStep;

    // 1. Top Header Bar (y: 0..25)
    ctx.fillStyle = '#080e0a';
    ctx.fillRect(0, 0, 240, 25);
    ctx.fillStyle = '#183420';
    ctx.fillRect(0, 25, 240, 1);

    // Track Badge: [T1..T8]
    ctx.fillStyle = '#00ff66';
    ctx.fillRect(4, 4, 20, 17);
    ctx.fillStyle = '#000000';
    ctx.font = 'bold 10px monospace';
    ctx.textAlign = 'center';
    ctx.fillText(`T${trkIdx + 1}`, 14, 16);

    // 8 Track Activity Meters
    for (let t = 0; t < 8; t++) {
      ctx.fillStyle = (t === trkIdx) ? '#00ff66' : '#122c19';
      ctx.fillRect(27 + t * 3.5, 10, 2.5, 5);
    }

    // Pattern Badge: A1..B8
    ctx.fillStyle = '#0c1a10';
    ctx.fillRect(57, 4, 18, 17);
    ctx.strokeStyle = '#1e4a28';
    ctx.lineWidth = 1;
    ctx.strokeRect(57.5, 4.5, 17, 16);
    ctx.fillStyle = '#ffffff';
    ctx.font = 'bold 9px monospace';
    ctx.fillText(`${String.fromCharCode(65 + trk.curBank)}${trk.curPat + 1}`, 66, 16);

    // Engine Badge: 303A, 303B, D808, D909, BRK, FM, SUB, MIDI
    const eng = trk.engine !== undefined ? trk.engine : pat.engine;
    const engColors = ['#ffb71a', '#ffb71a', '#ffb71a', '#00f0ff', '#f050d0', '#00ff66', '#ffe040', '#80a0c0'];
    ctx.fillStyle = engColors[eng % 8];
    ctx.fillRect(78, 4, 34, 17);
    ctx.fillStyle = '#000000';
    ctx.font = 'bold 8px monospace';
    ctx.fillText(ENGINE_NAMES[eng % 8], 95, 16);

    // Mode Badge: KEYS / DRUM / CURV
    const isDrum = (eng === 2 || eng === 3);
    const modeLabel = state.viewMode === 'curve' ? 'CURV' : (isDrum ? 'DRUM' : 'KEYS');
    ctx.fillStyle = state.viewMode === 'curve' ? '#00f0ff' : '#00ff66';
    ctx.fillRect(115, 4, 38, 17);
    ctx.fillStyle = '#000000';
    ctx.fillText(modeLabel, 134, 16);

    // MIDI CH
    ctx.fillStyle = '#50be6e';
    ctx.font = 'bold 8px monospace';
    ctx.fillText(`CH${pat.midiCh < 10 ? '0' + pat.midiCh : pat.midiCh}`, 174, 16);

    // Run / Stop
    ctx.fillStyle = state.isRunning ? '#00ff66' : '#ffb71a';
    ctx.beginPath();
    ctx.arc(205, 12, 3.5, 0, Math.PI * 2);
    ctx.fill();
    ctx.font = 'bold 8px monospace';
    ctx.textAlign = 'left';
    ctx.fillText(state.isRunning ? 'RUN' : 'STOP', 212, 15);

    // 2. Upper Note/Drum Grid (y: 26..148)
    const gridX = 22;
    const gridW = 216;
    const rowH = 10;
    const colW = 13;

    // Left Rail (y: 26..148)
    ctx.fillStyle = '#080e0a';
    ctx.fillRect(0, 26, gridX, 122);
    ctx.fillStyle = '#183420';
    ctx.fillRect(gridX - 1, 26, 1, 122);

    if (isDrum && state.viewMode !== 'curve') {
      // 12 Drum voice labels
      for (let r = 0; r < 12; r++) {
        const ky = 26 + r * rowH;
        const isSel = (r === state.drumLaneSel[trkIdx]);
        if (isSel) {
          ctx.fillStyle = '#ffb71a';
          ctx.fillRect(0, ky, gridX - 1, rowH - 1);
          ctx.fillStyle = '#000000';
        } else {
          ctx.fillStyle = (r === 0) ? '#ffb71a' : '#50be6e';
        }
        ctx.font = 'bold 7.5px monospace';
        ctx.textAlign = 'center';
        ctx.fillText(DRUM_LABELS[r], (gridX - 1) / 2, ky + 7.5);
      }
    } else {
      // Piano keyboard rail
      for (let r = 0; r < 12; r++) {
        const ky = 26 + r * rowH;
        if (PIANO_IS_BLACK[r]) {
          ctx.fillStyle = '#080e0a';
          ctx.fillRect(0, ky, 13, rowH - 1);
          ctx.fillStyle = '#050a07';
          ctx.fillRect(13, ky, gridX - 14, rowH - 1);
        } else {
          ctx.fillStyle = '#e1f5e6';
          ctx.fillRect(0, ky, gridX - 2, rowH - 1);
        }
      }
    }

    // Grid Area Background & Grid Lines
    ctx.fillStyle = '#030704';
    ctx.fillRect(gridX, 26, gridW, 120);

    for (let r = 0; r <= 12; r++) {
      ctx.strokeStyle = '#0c1c10';
      ctx.lineWidth = 1;
      ctx.beginPath();
      ctx.moveTo(gridX, 26 + r * rowH);
      ctx.lineTo(gridX + gridW, 26 + r * rowH);
      ctx.stroke();
    }

    for (let c = 0; c <= 16; c++) {
      ctx.strokeStyle = (c % 4 === 0) ? '#1e502a' : '#0c1c10';
      ctx.lineWidth = (c % 4 === 0) ? 1.5 : 1;
      ctx.beginPath();
      ctx.moveTo(gridX + c * colW, 26);
      ctx.lineTo(gridX + c * colW, 146);
      ctx.stroke();
    }

    // Steps Rendering
    for (let sIdx = 0; sIdx < 16; sIdx++) {
      const actualStep = stepOffset + sIdx;
      if (actualStep >= pat.length) {
        ctx.fillStyle = '#000000';
        ctx.fillRect(gridX + sIdx * colW, 26, colW, 120);
        continue;
      }

      const st = pat.steps[actualStep];

      if (state.isRunning && actualStep === curPlayStep) {
        ctx.fillStyle = 'rgba(0, 255, 102, 0.22)';
        ctx.fillRect(gridX + sIdx * colW, 26, colW, 120);
      }

      if (actualStep === state.activeStep) {
        ctx.strokeStyle = '#ffffff';
        ctx.lineWidth = 1.5;
        ctx.strokeRect(gridX + sIdx * colW + 1, 27, colW - 2, 118);
      }

      if (state.viewMode === 'curve') {
        const barH = (st.curve / 127) * 116;
        const cvY = 146 - barH;
        if (state.isRunning && actualStep === curPlayStep) {
          ctx.shadowColor = '#ffffff';
          ctx.shadowBlur = 8;
          ctx.fillStyle = '#ffffff';
        } else {
          ctx.shadowColor = '#00f0ff';
          ctx.shadowBlur = 4;
          ctx.fillStyle = '#00f0ff';
        }
        ctx.fillRect(gridX + sIdx * colW + 2, cvY, colW - 4, barH);
        ctx.shadowBlur = 0;
      } else if (isDrum) {
        // 12-lane Drum Matrix rendering
        const isAcc = (st.drums & (1 << 11)) !== 0;
        for (let r = 0; r < 12; r++) {
          const bit = 11 - r;
          if ((st.drums >> bit) & 1) {
            const dy = 26 + r * rowH;
            if (state.isRunning && actualStep === curPlayStep) {
              ctx.shadowColor = '#ffffff';
              ctx.shadowBlur = 6;
              ctx.fillStyle = '#ffffff';
            } else if (r === 0 || isAcc) {
              ctx.shadowColor = '#ffb71a';
              ctx.shadowBlur = 4;
              ctx.fillStyle = '#ffb71a';
            } else {
              ctx.shadowColor = '#00ff66';
              ctx.shadowBlur = 3;
              ctx.fillStyle = '#00ff66';
            }
            ctx.fillRect(gridX + sIdx * colW + 2, dy + 1, colW - 3, rowH - 2);
            ctx.shadowBlur = 0;
          }
        }
      } else {
        // Chromatic Synth Piano Roll
        if (st.gate > 0) {
          const noteOct = Math.floor(st.note / 12) - 1;
          const noteSemi = st.note % 12;
          const rowIdx = 11 - noteSemi;

          if (noteOct === state.viewOctave && rowIdx >= 0 && rowIdx < 12) {
            const ny = 26 + rowIdx * rowH;
            if (state.isRunning && actualStep === curPlayStep) {
              ctx.shadowColor = '#ffffff';
              ctx.shadowBlur = 8;
              ctx.fillStyle = '#ffffff';
            } else if (actualStep === state.activeStep) {
              ctx.shadowColor = '#00ff66';
              ctx.shadowBlur = 6;
              ctx.fillStyle = '#ffffff';
            } else if (st.gate > 105) {
              ctx.shadowColor = '#ffb71a';
              ctx.shadowBlur = 5;
              ctx.fillStyle = '#ffb71a';
            } else {
              ctx.shadowColor = '#00ff66';
              ctx.shadowBlur = 4;
              ctx.fillStyle = '#00ff66';
            }
            ctx.fillRect(gridX + sIdx * colW + 2, ny + 1, colW - 3, rowH - 2);
            ctx.shadowBlur = 0;

            if (st.tie && sIdx < 15) {
              ctx.fillStyle = '#ffffff';
              ctx.fillRect(gridX + sIdx * colW + colW - 2, ny + 3, 3, 3);
            }
          } else if (noteOct > state.viewOctave) {
            ctx.fillStyle = '#ffb71a';
            ctx.fillRect(gridX + sIdx * colW + 4, 27, colW - 7, 3);
          } else {
            ctx.fillStyle = '#ffb71a';
            ctx.fillRect(gridX + sIdx * colW + 4, 142, colW - 7, 3);
          }
        }
      }
    }

    // 3. Lower Section: Velocity Bars / Tied Bridges (y: 148..218)
    ctx.fillStyle = '#050a07';
    ctx.fillRect(0, 148, 240, 72);
    ctx.fillStyle = '#183420';
    ctx.fillRect(0, 148, 240, 1);

    ctx.fillStyle = '#50be6e';
    ctx.font = 'bold 8px monospace';
    ctx.textAlign = 'left';
    ctx.fillText('GATE', 2, 160);
    ctx.fillText('VEL', 4, 170);

    for (let sIdx = 0; sIdx < 16; sIdx++) {
      const actualStep = stepOffset + sIdx;
      if (actualStep >= pat.length) continue;

      const st = pat.steps[actualStep];
      const bx = gridX + sIdx * colW;

      if (st.gate > 0) {
        const barH = Math.max(4, Math.min(56, (st.gate / 127) * 58));
        const by = 216 - barH;

        if (state.isRunning && actualStep === curPlayStep) {
          ctx.shadowColor = '#ffffff';
          ctx.shadowBlur = 6;
          ctx.fillStyle = '#ffffff';
        } else if (actualStep === state.activeStep) {
          ctx.shadowColor = '#00ff66';
          ctx.shadowBlur = 4;
          ctx.fillStyle = '#ffffff';
        } else if (st.gate > 105) {
          ctx.shadowColor = '#ffb71a';
          ctx.shadowBlur = 4;
          ctx.fillStyle = '#ffb71a';
        } else {
          ctx.shadowColor = '#00ff66';
          ctx.shadowBlur = 2;
          ctx.fillStyle = '#00ff66';
        }
        ctx.fillRect(bx + 2, by, colW - 4, barH);
        ctx.shadowBlur = 0;

        if (st.tie && sIdx < 15) {
          ctx.fillStyle = '#ffffff';
          ctx.fillRect(bx + colW - 3, by + 4, 5, 4);
        }
      } else {
        ctx.fillStyle = '#183420';
        ctx.fillRect(bx + 2, 214, colW - 4, 2);
      }
    }

    // 4. Bottom Status Bar (y: 220..240)
    ctx.fillStyle = '#080e0a';
    ctx.fillRect(0, 220, 240, 20);
    ctx.fillStyle = '#183420';
    ctx.fillRect(0, 220, 240, 1);

    const statusToShow = state.heldStatus || (state.statusMsg && Date.now() < state.statusTimer ? state.statusMsg : null);
    if (statusToShow) {
      const isHeld = !!state.heldStatus;
      ctx.fillStyle = isHeld ? '#142016' : '#0a1a0e';
      ctx.fillRect(3, 222, 234, 16);
      ctx.strokeStyle = isHeld ? '#ffb71a' : '#00ff66';
      ctx.lineWidth = 1;
      ctx.strokeRect(3.5, 222.5, 233, 15);

      ctx.fillStyle = isHeld ? '#ffb71a' : '#ffffff';
      ctx.font = 'bold 9px monospace';
      ctx.textAlign = 'left';
      ctx.fillText(statusToShow, 8, 233);
    } else {
      ctx.fillStyle = '#ffb71a';
      ctx.font = 'bold 9px monospace';
      ctx.textAlign = 'left';
      if (isDrum) {
        ctx.fillText(`P${state.viewPage + 1} ${DRUM_LABELS[state.drumLaneSel[trkIdx]]}`, 6, 233);
      } else {
        ctx.fillText(`P${state.viewPage + 1} OCT:${state.viewOctave}`, 6, 233);
      }

      ctx.fillStyle = '#ffffff';
      ctx.fillText(`${formatBpm(state.bpm)}BPM`, 82, 233);

      ctx.fillStyle = '#50be6e';
      ctx.fillText(state.screenEditEnabled ? 'TOUCH:EDIT' : 'K1-4:EDIT', 152, 233);
    }

    // 5. Modal Pattern Tools Overlay (when FX menu is open)
    if (state.menuMode === 'fx') {
      const menuW = 196;
      const menuH = 158;
      const mx = (240 - menuW) / 2;
      const my = 38;

      ctx.fillStyle = 'rgba(4, 10, 6, 0.94)';
      ctx.fillRect(0, 26, 240, 194);

      ctx.fillStyle = '#09160d';
      ctx.fillRect(mx, my, menuW, menuH);
      ctx.strokeStyle = '#00ff66';
      ctx.lineWidth = 1;
      ctx.strokeRect(mx + 0.5, my + 0.5, menuW - 1, menuH - 1);

      ctx.fillStyle = '#a3ff3b';
      ctx.font = 'bold 9px monospace';
      ctx.textAlign = 'center';
      ctx.fillText('-- PATTERN TOOLS --', 120, my + 13);

      for (let m = 0; m < FX_MENU_ITEMS.length; m++) {
        const iy = my + 18 + m * 13;
        if (state.menuSel === m) {
          ctx.fillStyle = '#12331c';
          ctx.fillRect(mx + 4, iy, menuW - 8, 12);
          ctx.fillStyle = '#b8ff5c';
        } else {
          ctx.fillStyle = '#98cfa6';
        }
        ctx.font = 'bold 8px monospace';
        ctx.textAlign = 'left';
        ctx.fillText(FX_MENU_ITEMS[m], mx + 8, iy + 9);
      }
    }
  }

  // ===========================================================================
  // Multi-Level Undo / Redo History System
  // ===========================================================================
  const undoStack = [];
  const redoStack = [];
  const MAX_UNDO = 32;

  function updateUndoUi() {
    const u = document.getElementById("btnUndo");
    const r = document.getElementById("btnRedo");
    if (u) u.disabled = undoStack.length === 0;
    if (r) r.disabled = redoStack.length === 0;
  }

  function pushUndo(actionName = "Edit") {
    const trk = state.tracks[state.activeTrack];
    const patIndex = trk.curBank * 8 + trk.curPat;
    undoStack.push({
      track: state.activeTrack,
      bank: trk.curBank,
      pat: trk.curPat,
      action: actionName,
      pattern: JSON.parse(JSON.stringify(trk.patterns[patIndex]))
    });
    if (undoStack.length > MAX_UNDO) undoStack.shift();
    redoStack.length = 0;
    updateUndoUi();
  }

  function undo() {
    if (undoStack.length === 0) {
      showHint("⚠️ Nothing to undo.");
      return false;
    }
    const entry = undoStack.pop();
    const trk = state.tracks[entry.track];
    const patIndex = entry.bank * 8 + entry.pat;

    redoStack.push({
      track: entry.track,
      bank: entry.bank,
      pat: entry.pat,
      action: entry.action,
      pattern: JSON.parse(JSON.stringify(trk.patterns[patIndex]))
    });

    trk.patterns[patIndex] = entry.pattern;
    state.activeTrack = entry.track;
    trk.curBank = entry.bank;
    trk.curPat = entry.pat;

    updateUndoUi();
    showHint(`↶ Undone: ${entry.action} on Track ${entry.track + 1}`);
    return true;
  }

  function redo() {
    if (redoStack.length === 0) {
      showHint("⚠️ Nothing to redo.");
      return false;
    }
    const entry = redoStack.pop();
    const trk = state.tracks[entry.track];
    const patIndex = entry.bank * 8 + entry.pat;

    undoStack.push({
      track: entry.track,
      bank: entry.bank,
      pat: entry.pat,
      action: entry.action,
      pattern: JSON.parse(JSON.stringify(trk.patterns[patIndex]))
    });

    trk.patterns[patIndex] = entry.pattern;
    state.activeTrack = entry.track;
    trk.curBank = entry.bank;
    trk.curPat = entry.pat;

    updateUndoUi();
    showHint(`↷ Redone: ${entry.action} on Track ${entry.track + 1}`);
    return true;
  }

  // Keyboard Shortcuts for Undo/Redo
  window.addEventListener("keydown", (e) => {
    if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'z') {
      e.preventDefault();
      if (e.shiftKey) redo();
      else undo();
    } else if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'y') {
      e.preventDefault();
      redo();
    }
  });

  // ===========================================================================
  // Pattern Edit Operations (Shift, Transpose, Alter, Randomize, Clear)
  // ===========================================================================
  const FX_MENU_ITEMS = [
    "1. UNDO LAST EDIT",
    "2. REDO EDIT",
    "3. SHIFT LEFT",
    "4. SHIFT RIGHT",
    "5. TRANSPOSE +1",
    "6. TRANSPOSE -1",
    "7. ALTER PATTERN",
    "8. RANDOMIZE PATTERN",
    "9. CLEAR PATTERN",
    "10. EXIT MENU"
  ];

  function shiftPatternHorizontal(dir) {
    const trk = state.tracks[state.activeTrack];
    const pat = trk.patterns[trk.curBank * 8 + trk.curPat];
    if (pat.length <= 1) return;
    pushUndo("Shift Pattern");
    if (dir > 0) {
      const last = Object.assign({}, pat.steps[pat.length - 1]);
      for (let i = pat.length - 1; i > 0; i--) pat.steps[i] = Object.assign({}, pat.steps[i - 1]);
      pat.steps[0] = last;
      showHint(`▶ Shifted Track ${state.activeTrack + 1} pattern right.`);
    } else {
      const first = Object.assign({}, pat.steps[0]);
      for (let i = 0; i < pat.length - 1; i++) pat.steps[i] = Object.assign({}, pat.steps[i + 1]);
      pat.steps[pat.length - 1] = first;
      showHint(`◀ Shifted Track ${state.activeTrack + 1} pattern left.`);
    }
  }

  function shiftPatternVertical(semitones) {
    const trk = state.tracks[state.activeTrack];
    const pat = trk.patterns[trk.curBank * 8 + trk.curPat];
    pushUndo("Transpose");
    for (let i = 0; i < pat.length; i++) {
      let n = pat.steps[i].note + semitones;
      pat.steps[i].note = Math.max(0, Math.min(127, n));
    }
    showHint(`${semitones > 0 ? '▲ Transposed +1' : '▼ Transposed -1'} semitone on Track ${state.activeTrack + 1}.`);
  }

  function alterPattern() {
    const trk = state.tracks[state.activeTrack];
    const pat = trk.patterns[trk.curBank * 8 + trk.curPat];
    pushUndo("Alter Pattern");
    for (let i = 0; i < pat.length; i++) {
      if (Math.random() < 0.25 && pat.steps[i].gate > 0) {
        const delta = Math.random() < 0.5 ? 2 : -2;
        pat.steps[i].note = Math.max(24, Math.min(100, pat.steps[i].note + delta));
        pat.steps[i].tie = Math.random() < 0.3 ? 1 : 0;
      }
    }
    showHint(`🎲 Altered groove variations on Track ${state.activeTrack + 1}.`);
  }

  function randomizePattern() {
    const trk = state.tracks[state.activeTrack];
    const pat = trk.patterns[trk.curBank * 8 + trk.curPat];
    pushUndo("Randomize Pattern");
    const scale = [0, 3, 5, 7, 10, 12, 15];
    for (let i = 0; i < pat.length; i++) {
      if (Math.random() < 0.25) {
        pat.steps[i].gate = 0;
        pat.steps[i].tie = 0;
      } else {
        pat.steps[i].note = 48 + scale[Math.floor(Math.random() * scale.length)];
        pat.steps[i].gate = 85 + Math.floor(Math.random() * 42);
        pat.steps[i].tie = Math.random() < 0.2 ? 1 : 0;
      }
      pat.steps[i].curve = Math.floor(Math.random() * 127);
    }
    showHint(`🎲 Random pattern generated for Track ${state.activeTrack + 1}! (Press Undo or Ctrl+Z to revert)`);
  }

  function clearPattern() {
    const trk = state.tracks[state.activeTrack];
    const pat = trk.patterns[trk.curBank * 8 + trk.curPat];
    pushUndo("Clear Pattern");
    pat.steps.forEach(st => { st.gate = 0; st.tie = 0; st.curve = 64; });
    showHint(`🧹 Cleared all notes in Pattern ${String.fromCharCode(65 + trk.curBank)}${trk.curPat + 1}. (Press Undo to revert)`);
  }

  function executeFxMenuItem(sel) {
    state.menuMode = 'none';
    switch (sel) {
      case 0: undo(); break;
      case 1: redo(); break;
      case 2: shiftPatternHorizontal(-1); break;
      case 3: shiftPatternHorizontal(1); break;
      case 4: shiftPatternVertical(1); break;
      case 5: shiftPatternVertical(-1); break;
      case 6: alterPattern(); break;
      case 7: randomizePattern(); break;
      case 8: clearPattern(); break;
      case 9: showHint("Pattern tools closed."); break;
    }
  }

  function loop() {
    renderScreen();
    requestAnimationFrame(loop);
  }

  // ===========================================================================
  // Interactive Direct Grid Editing on Canvas & Hint Hover
  // ===========================================================================
  canvas.addEventListener('mousemove', (e) => {
    const rect = canvas.getBoundingClientRect();
    const scale = 240 / rect.width;
    const cx = (e.clientX - rect.left) * scale;
    const cy = (e.clientY - rect.top) * scale;

    if (state.menuMode === 'fx') {
      const menuW = 196;
      const mx = (240 - menuW) / 2;
      const my = 38;
      if (cx >= mx && cx <= mx + menuW && cy >= my + 18 && cy <= my + 18 + 10 * 13) {
        const itemIdx = Math.floor((cy - (my + 18)) / 13);
        if (itemIdx >= 0 && itemIdx < FX_MENU_ITEMS.length) {
          state.menuSel = itemIdx;
        }
      }
      showHint("⚙️ PATTERN TOOLS: Click any option on screen, or use SELECT + HOME. Tap FX to exit.");
      return;
    }

    if (!state.screenEditEnabled) {
      showHint("🖥️ 240×240 LCD (Hardware Faithful Mode): Controlled via panel knobs, buttons & keys. Enable 'Screen Edit' in toolbar to draw notes with mouse.");
      return;
    }

    if (cy >= 26 && cy <= 146) {
      showHint(state.viewMode === 'keys'
        ? "✏️ NOTE GRID: Click any square on the screen to toggle notes on/off and audition them!"
        : "📈 MODULATION CURVE: Click or drag in the grid to draw filter cutoff automation curves!");
    } else if (cy >= 150 && cy <= 218) {
      showHint("📊 VELOCITY BARS: Click or drag in the lower bars to set loudness. Velocity ≥ 105 triggers 303 accent!");
    } else {
      showHint("🖥️ 240x240 LCD DISPLAY: Header shows active Track, Bank/Pattern, Engine, Mode, and Tempo.");
    }
  });

  canvas.addEventListener('mouseleave', resetHint);

  canvas.addEventListener('click', (e) => {
    if (!state.hasStarted) {
      if (btnStart) btnStart.click();
      return;
    }
    if (performance.now() < state.splashUntil) {
      dismissSplash();
      return;
    }
    initAudio();
    const rect = canvas.getBoundingClientRect();
    const scale = 240 / rect.width;
    const cx = (e.clientX - rect.left) * scale;
    const cy = (e.clientY - rect.top) * scale;

    // Handle clicks inside FX Modal Menu
    if (state.menuMode === 'fx') {
      const menuW = 196;
      const mx = (240 - menuW) / 2;
      const my = 38;
      if (cx >= mx && cx <= mx + menuW && cy >= my + 18 && cy <= my + 18 + 10 * 13) {
        const itemIdx = Math.floor((cy - (my + 18)) / 13);
        if (itemIdx >= 0 && itemIdx < FX_MENU_ITEMS.length) {
          state.menuSel = itemIdx;
          executeFxMenuItem(itemIdx);
        }
      } else {
        state.menuMode = 'none';
        showHint("Pattern tools closed.");
      }
      return;
    }

    // Hardware Faithful Mode: direct screen editing is disabled unless user enabled it
    if (!state.screenEditEnabled) {
      showHint("🔒 Screen mouse editing is OFF (Hardware Faithful). Use REC + keys or SELECT/K1-K4 to edit, or toggle 'Screen Edit' in the toolbar!");
      return;
    }

    const trk = state.tracks[state.activeTrack];
    const pat = trk.patterns[trk.curBank * 8 + trk.curPat];
    const stepOffset = state.viewPage * 16;
    const gridX = 22;
    const colW = 13;
    const rowH = 10;

    const eng = trk.engine !== undefined ? trk.engine : pat.engine;
    const isDrum = (eng === 2 || eng === 3);

    // Left Rail click in Drum mode: select active drum lane & audition
    if (isDrum && state.viewMode !== 'curve' && cx < gridX && cy >= 26 && cy <= 146) {
      const row = Math.floor((cy - 26) / rowH);
      if (row >= 0 && row < 12) {
        state.drumLaneSel[state.activeTrack] = row;
        playDrumVoice(eng, row, 120, row === 0);
        showStatus(`DRUM LANE: ${DRUM_LABELS[row]} (${DRUM_FULL_NAMES[row]})`);
      }
      return;
    }

    // 1. Clicked inside the Note / Drum Grid (y: 26..146)
    if (cx >= gridX && cx <= gridX + 16 * colW && cy >= 26 && cy <= 146) {
      const col = Math.floor((cx - gridX) / colW);
      const row = Math.floor((cy - 26) / rowH);
      const actualStep = stepOffset + col;

      if (actualStep < pat.length && row >= 0 && row < 12) {
        state.activeStep = actualStep;
        const st = pat.steps[actualStep];

        if (state.viewMode === 'curve') {
          pushUndo("Draw Curve");
          const curveVal = Math.min(127, Math.max(0, Math.round((146 - cy) / 116 * 127)));
          st.curve = curveVal;
          showStatus(`STEP ${actualStep + 1} CURVE CC: ${curveVal}/127`);
        } else if (isDrum) {
          pushUndo("Toggle Drum Trigger");
          const bit = 11 - row;
          st.drums ^= (1 << bit);
          if (st.gate === 0) st.gate = 115;
          const isOn = (st.drums & (1 << bit)) !== 0;
          if (isOn) {
            playDrumVoice(eng, row, 115, (st.drums & (1 << 11)) !== 0);
          }
          showStatus(`S${actualStep + 1} ${DRUM_LABELS[row]} [${isOn ? 'TRIGGER ON' : 'OFF'}]`);
        } else {
          const noteSemi = 11 - row;
          const targetNote = (state.viewOctave + 1) * 12 + noteSemi;

          pushUndo("Toggle Note");
          if (st.gate > 0 && st.note === targetNote) {
            st.gate = 0; // Toggle off
            showStatus(`STEP ${actualStep + 1}/32: [REST]`);
          } else {
            st.note = targetNote;
            st.gate = 115;
            st.tie = 0;
            play303Note(eng, targetNote, 115, false, st.curve);
            showStatus(`STEP ${actualStep + 1}/32: ${getNoteName(targetNote)} (V:115)`);
          }
        }
      }
    }

    // 2. Clicked inside Velocity Bar area (y: 150..218)
    else if (cx >= gridX && cx <= gridX + 16 * colW && cy >= 150 && cy <= 218) {
      const col = Math.floor((cx - gridX) / colW);
      const actualStep = stepOffset + col;

      if (actualStep < pat.length) {
        state.activeStep = actualStep;
        const st = pat.steps[actualStep];
        const vel = Math.min(127, Math.max(0, Math.round((216 - cy) / 58 * 127)));
        pushUndo("Adjust Velocity");
        st.gate = vel;
        showStatus(`STEP ${actualStep + 1} VEL: ${vel}/127`);
        if (vel > 0) {
          if (isDrum) {
            playDrumVoice(eng, state.drumLaneSel[state.activeTrack], vel, (st.drums & (1 << 11)) !== 0);
          } else {
            play303Note(eng, st.note, vel, false, st.curve);
          }
        }
      }
    }
  });

  // ===========================================================================
  // SVG Front Panel & Controls Generation (1:1 with M-VAVE FM-1)
  // ===========================================================================
  const NS = "http://www.w3.org/2000/svg";
  const BODY = "#161e18", TRAY = "#0e1510", KEY_W = "#405448", KEY_B = "#233329", KNOB = "#101812", BTNC = "#1b2720", LABEL = "#b2dfc0";
  const WHITE_K = [0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26];
  const BLACK_K = [1, 3, 5, 8, 10, 13, 15, 17, 20, 22, 25];
  const BLACK_AFTER = [0, 1, 2, 4, 5, 7, 8, 9, 11, 12, 14];

  const svg = document.getElementById("panel");
  const controls = document.getElementById("controls");

  function el(tag, attrs, parent = svg) {
    const e = document.createElementNS(NS, tag);
    for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v);
    parent.appendChild(e);
    return e;
  }

  function text(parent, x, y, s, size, fill = LABEL, weight = 600) {
    const t = el("text", {
      x, y, "text-anchor": "middle",
      "font-family": "'Barlow Semi Condensed', 'Arial Narrow', Arial, sans-serif",
      "font-size": size, "font-weight": weight, fill,
      "letter-spacing": ".05em", class: "btn", "pointer-events": "none"
    }, parent);
    t.textContent = s;
    return t;
  }

  // 1. Chassis Body
  el("rect", { x: 4, y: 4, width: 992, height: 632, rx: 44, fill: BODY });

  // 2. Center Screen Bezel
  el("rect", { x: 300, y: 44, width: 236, height: 236, rx: 26, fill: "#08100a" });

  // 3. Knobs Builder
  const knobs = {};
  function knob(x, y, r, label, role) {
    const g = el("g", { class: "ctl knob" });
    el("circle", { cx: x, cy: y, r: r + 5, fill: TRAY }, g);
    el("circle", { cx: x, cy: y, r, fill: KNOB, stroke: "#283b30", "stroke-width": 2 }, g);
    const ind = el("line", { x1: x, y1: y - r + 5, x2: x, y2: y - r + 15, stroke: "#00ff66", "stroke-width": 3, "stroke-linecap": "round" }, g);
    text(g, x, y - r - 12, label, 16);
    el("circle", { cx: x, cy: y, r: r + 14, fill: "transparent" }, g);
    knobs[label] = {
      g, ind, x, y, angle: 0, role, label,
      fallbackBox: { x: x - r - 8, y: y - r - 18, width: (r + 8) * 2, height: (r + 18) * 2 }
    };
    return knobs[label];
  }

  knob(105, 100, 24, "MASTER", "master");
  knob(215, 100, 24, "SELECT", "select");
  knob(105, 205, 24, "TEMPO", "tempo");
  knob(215, 205, 24, "TRACK", "track");

  knob(610, 100, 22, "KNOB1", "k1");
  knob(714, 100, 22, "KNOB2", "k2");
  knob(818, 100, 22, "KNOB3", "k3");
  knob(922, 100, 22, "KNOB4", "k4");

  // 4. Buttons Builder
  const buttons = {};
  function button(x, y, w, h, label, two) {
    const g = el("g", { class: "ctl" });
    el("rect", { x, y, width: w, height: h, rx: 9, fill: BTNC, stroke: "#283b30", "stroke-width": 1.5, class: "btnface" }, g);
    if (two) {
      text(g, x + w / 2, y + h / 2 - 1, label, 12);
      text(g, x + w / 2, y + h / 2 + 12, two, 12);
    } else {
      text(g, x + w / 2, y + h / 2 + 5, label, 15);
    }
    buttons[label] = { g, label, fallbackBox: { x, y, width: w, height: h } };
  }

  el("rect", { x: 58, y: 258, width: 210, height: 58, rx: 14, fill: TRAY });
  button(72, 268, 82, 38, "OCT-");
  button(170, 268, 82, 38, "OCT+");

  el("rect", { x: 568, y: 160, width: 400, height: 140, rx: 18, fill: TRAY });
  const ROW1 = ["FX", "SEL", "ENV", "LFO", "EDIT", "GLO"];
  const ROW2 = ["HOME", "SAVE", "ARP", "SEQ", "PLAY", "REC"];
  for (let i = 0; i < 6; i++) {
    button(582 + i * 63, 174, 54, 48, ROW1[i]);
    if (ROW2[i] === "PLAY") {
      button(582 + i * 63, 236, 54, 48, "PLAY", "STOP");
    } else {
      button(582 + i * 63, 236, 54, 48, ROW2[i]);
    }
  }

  // 5. 27 Silicone Keys
  const keys = [];
  (function buildKeybed(x0, y0, w, h) {
    el("rect", { x: x0, y: y0, width: w, height: h, rx: 18, fill: TRAY });
    const pitch = (w - 24) / 16;
    const kw = pitch * 0.78;

    for (let i = 0; i < 16; i++) {
      const g = el("g", { class: "ctl" });
      const kx = x0 + 12 + i * pitch + (pitch - kw) / 2;
      const ky = y0 + h * 0.43;
      const kh = h * 0.52;
      el("rect", { x: kx, y: ky, width: kw, height: kh, rx: kw / 2, fill: KEY_W, class: "keyface white" }, g);
      keys[WHITE_K[i]] = { g, idx: WHITE_K[i], isBlack: false, fallbackBox: { x: kx, y: ky, width: kw, height: kh } };
    }

    BLACK_AFTER.forEach((a, k) => {
      const g = el("g", { class: "ctl" });
      const kx = x0 + 12 + (a + 1) * pitch - kw / 2;
      const ky = y0 + h * 0.06;
      const kh = h * 0.33;
      el("rect", { x: kx, y: ky, width: kw, height: kh, rx: kw / 2, fill: KEY_B, class: "keyface black" }, g);
      keys[BLACK_K[k]] = { g, idx: BLACK_K[k], isBlack: true, fallbackBox: { x: kx, y: ky, width: kw, height: kh } };
    });
  })(36, 340, 928, 270);

  // 6. Interactive Transparent Hit-Target Layer (With foolproof bounding box fallback)
  function createHit(c, kind, hintKey) {
    const d = document.createElement(kind === "knob" ? "div" : "button");
    d.className = "hit" + (kind === "knob" ? " knob" : "");
    if (kind !== "knob") d.type = "button";

    let b = null;
    try {
      b = c.g.getBBox();
    } catch (e) {}
    if (!b || b.width === 0 || b.height === 0) {
      b = c.fallbackBox || { x: 0, y: 0, width: 30, height: 30 };
    }

    Object.assign(d.style, {
      left: `${(b.x / 10)}%`,
      top: `${(b.y / 6.4)}%`,
      width: `${(b.width / 10)}%`,
      height: `${(b.height / 6.4)}%`
    });

    d.setAttribute("aria-label", c.label || "Control");
    d.addEventListener("touchstart", (e) => e.preventDefault(), { passive: false });

    // Live Hover & Focus Hints
    if (hintKey && HINTS[hintKey]) {
      d.addEventListener("pointerenter", () => showHint(HINTS[hintKey]));
      d.addEventListener("pointerleave", resetHint);
      d.addEventListener("focus", () => showHint(HINTS[hintKey]));
      d.addEventListener("blur", resetHint);
    }

    controls.appendChild(d);
    c.hit = d;
  }

  Object.values(knobs).forEach(k => createHit(k, "knob", k.label));
  Object.values(buttons).forEach(b => createHit(b, "button", b.label));
  keys.forEach(k => {
    if (k) {
      createHit(k, "key");
      k.hit.addEventListener("pointerenter", () => showHint("🎹 SILICONE KEY: Click to audition note pitch. In REC mode, writes note into active step!"));
      k.hit.addEventListener("pointerleave", resetHint);
    }
  });

  function drawKnob(k) {
    k.ind.setAttribute("transform", `rotate(${k.angle} ${k.x} ${k.y})`);
  }

  function bindKnob(k, onTurn) {
    let drag = null;
    k.hit.addEventListener("pointerdown", (e) => {
      dismissSplash();
      e.preventDefault();
      k.hit.setPointerCapture(e.pointerId);
      drag = { y: e.clientY };
    });

    k.hit.addEventListener("pointermove", (e) => {
      if (!drag) return;
      const dy = drag.y - e.clientY;
      drag.y = e.clientY;
      onTurn(dy);
    });

    k.hit.addEventListener("pointerup", (e) => {
      drag = null;
      try { k.hit.releasePointerCapture(e.pointerId); } catch (err) {}
    });

    k.hit.addEventListener("wheel", (e) => {
      dismissSplash();
      e.preventDefault();
      onTurn(e.deltaY < 0 ? 3 : -3);
    }, { passive: false });
  }

  // Initialize Knobs
  bindKnob(knobs.MASTER, (dy) => {
    state.masterVol = Math.max(0, Math.min(1.0, state.masterVol + dy * 0.01));
    knobs.MASTER.angle = -135 + state.masterVol * 270;
    drawKnob(knobs.MASTER);
    if (masterGain && audioCtx) {
      masterGain.gain.setValueAtTime(state.masterVol, audioCtx.currentTime);
    }
    showStatus(`MASTER VOL: ${Math.round(state.masterVol * 100)}%`);
  });
  knobs.MASTER.angle = -135 + state.masterVol * 270;
  drawKnob(knobs.MASTER);

  bindKnob(knobs.SELECT, (dy) => {
    if (state.menuMode === 'fx') {
      state.menuSel = (state.menuSel + (dy > 0 ? 1 : -1) + FX_MENU_ITEMS.length) % FX_MENU_ITEMS.length;
      knobs.SELECT.angle = (knobs.SELECT.angle + (dy > 0 ? 15 : -15)) % 360;
      drawKnob(knobs.SELECT);
      showStatus(FX_MENU_ITEMS[state.menuSel]);
      return;
    }
    state.activeStep = (state.activeStep + (dy > 0 ? 1 : -1) + 32) % 32;
    state.viewPage = state.activeStep >= 16 ? 1 : 0;
    knobs.SELECT.angle = (knobs.SELECT.angle + (dy > 0 ? 15 : -15)) % 360;
    drawKnob(knobs.SELECT);
    const trk = state.tracks[state.activeTrack];
    const pat = trk.patterns[trk.curBank * 8 + trk.curPat];
    const st = pat.steps[state.activeStep];
    showStatus(st.gate > 0 ? `STEP ${state.activeStep + 1}/32: ${getNoteName(st.note)} (V:${st.gate})` : `STEP ${state.activeStep + 1}/32: [REST]`);
  });

  bindKnob(knobs.TEMPO, (dy) => {
    const delta = state.shiftMode ? (dy * 0.1) : dy;
    const nextBpm = Math.min(240, Math.max(50, state.bpm + delta));
    state.bpm = Math.round(nextBpm * 10) / 10;
    knobs.TEMPO.angle = ((state.bpm - 50) / 190) * 270 - 135;
    drawKnob(knobs.TEMPO);
    showStatus(`TEMPO: ${formatBpm(state.bpm)} BPM${state.shiftMode ? ' (FINE)' : ''}`);
    updateButtonVisuals();
  });
  knobs.TEMPO.angle = ((state.bpm - 50) / 190) * 270 - 135;
  drawKnob(knobs.TEMPO);

  bindKnob(knobs.TRACK, (dy) => {
    if (Math.abs(dy) >= 1) {
      state.activeTrack = (state.activeTrack + (dy > 0 ? 1 : -1) + NUM_TRACKS) % NUM_TRACKS;
      knobs.TRACK.angle = (state.activeTrack / (NUM_TRACKS - 1)) * 270 - 135;
      drawKnob(knobs.TRACK);
      const trk = state.tracks[state.activeTrack];
      const eng = trk.engine !== undefined ? trk.engine : 0;
      showStatus(`TRACK ${state.activeTrack + 1}: ${ENGINE_NAMES[eng % ENGINE_NAMES.length]}`);
    }
  });

  bindKnob(knobs.KNOB1, (dy) => {
    const trk = state.tracks[state.activeTrack];
    const pat = trk.patterns[trk.curBank * 8 + trk.curPat];
    const eng = trk.engine !== undefined ? trk.engine : pat.engine;
    const isDrum = (eng === 2 || eng === 3);

    if (isDrum && state.viewMode !== 'curve') {
      state.drumLaneSel[state.activeTrack] = Math.min(11, Math.max(0, state.drumLaneSel[state.activeTrack] + (dy > 0 ? 1 : -1)));
      const lane = state.drumLaneSel[state.activeTrack];
      showStatus(`DRUM LANE: ${DRUM_LABELS[lane]} (${DRUM_FULL_NAMES[lane]})`);
      return;
    }

    if (state.shiftMode) {
      const st = pat.steps[state.activeStep];
      pushUndo("Step Pitch");
      st.note = Math.min(127, Math.max(0, st.note + (dy > 0 ? 1 : -1)));
      if (st.gate === 0) st.gate = 110;
      play303Note(eng, st.note, st.gate, false, st.curve);
      showStatus(`STEP ${state.activeStep + 1} PITCH: ${getNoteName(st.note)} (${st.note})`);
      return;
    }
    synthCutoff = Math.min(127, Math.max(0, synthCutoff + dy));
    knobs.KNOB1.angle = (synthCutoff / 127) * 270 - 135;
    drawKnob(knobs.KNOB1);
    showStatus(`CUTOFF: ${synthCutoff}/127 (${Math.round((synthCutoff / 127) * 100)}%)`);
  });
  knobs.KNOB1.angle = (synthCutoff / 127) * 270 - 135;
  drawKnob(knobs.KNOB1);

  bindKnob(knobs.KNOB2, (dy) => {
    const trk = state.tracks[state.activeTrack];
    const pat = trk.patterns[trk.curBank * 8 + trk.curPat];
    const eng = trk.engine !== undefined ? trk.engine : pat.engine;
    const isDrum = (eng === 2 || eng === 3);

    if (isDrum && state.viewMode !== 'curve') {
      const st = pat.steps[state.activeStep];
      const lane = state.drumLaneSel[state.activeTrack];
      const bit = 11 - lane;
      pushUndo("Toggle Drum Trigger");
      st.drums ^= (1 << bit);
      if (st.gate === 0) st.gate = 115;
      const isOn = (st.drums & (1 << bit)) !== 0;
      showStatus(`STEP ${state.activeStep + 1} ${DRUM_LABELS[lane]}: [${isOn ? 'TRIGGER ON' : 'OFF'}]`);
      if (isOn) playDrumVoice(eng, lane, 115, (st.drums & (1 << 11)) !== 0);
      return;
    }

    if (state.shiftMode) {
      const st = pat.steps[state.activeStep];
      pushUndo("Step Velocity");
      st.gate = Math.min(127, Math.max(0, st.gate + dy * 2));
      if (dy > 3 && st.tie === 0) st.tie = 1;
      else if (dy < -3 && st.tie === 1) st.tie = 0;
      showStatus(`STEP ${state.activeStep + 1} VEL: ${st.gate} [TIE:${st.tie ? 'ON' : 'OFF'}]`);
      return;
    }
    synthResonance = Math.min(127, Math.max(0, synthResonance + dy));
    knobs.KNOB2.angle = (synthResonance / 127) * 270 - 135;
    drawKnob(knobs.KNOB2);
    showStatus(`RESONANCE: ${synthResonance}/127 (${Math.round((synthResonance / 127) * 100)}%)`);
  });
  knobs.KNOB2.angle = (synthResonance / 127) * 270 - 135;
  drawKnob(knobs.KNOB2);

  bindKnob(knobs.KNOB3, (dy) => {
    if (state.shiftMode || state.viewMode === 'curve') {
      const trk = state.tracks[state.activeTrack];
      const pat = trk.patterns[trk.curBank * 8 + trk.curPat];
      const st = pat.steps[state.activeStep];
      pushUndo("Step Curve");
      st.curve = Math.min(127, Math.max(0, st.curve + dy * 2));
      showStatus(`STEP ${state.activeStep + 1} CURVE CC: ${st.curve}/127`);
      return;
    }
    synthDecay = Math.min(127, Math.max(0, synthDecay + dy));
    knobs.KNOB3.angle = (synthDecay / 127) * 270 - 135;
    drawKnob(knobs.KNOB3);
    showStatus(`DECAY: ${synthDecay}/127 (${Math.round((synthDecay / 127) * 100)}%)`);
  });
  knobs.KNOB3.angle = (synthDecay / 127) * 270 - 135;
  drawKnob(knobs.KNOB3);

  bindKnob(knobs.KNOB4, (dy) => {
    const trk = state.tracks[state.activeTrack];
    const pat = trk.patterns[trk.curBank * 8 + trk.curPat];
    const eng = trk.engine !== undefined ? trk.engine : pat.engine;
    const isDrum = (eng === 2 || eng === 3);

    if (isDrum && state.viewMode !== 'curve') {
      const st = pat.steps[state.activeStep];
      pushUndo("Toggle Accent");
      st.drums ^= (1 << 11);
      const isAcc = (st.drums & (1 << 11)) !== 0;
      st.gate = isAcc ? 127 : 100;
      showStatus(`STEP ${state.activeStep + 1} ACCENT: [${isAcc ? 'ON' : 'OFF'}]`);
      return;
    }

    if (state.shiftMode) {
      pat.length = Math.min(32, Math.max(1, pat.length + (dy > 0 ? 1 : -1)));
      showStatus(`TRACK ${state.activeTrack + 1} LENGTH: ${pat.length} STEPS`);
      return;
    }
    synthAccent = Math.min(127, Math.max(0, synthAccent + dy));
    knobs.KNOB4.angle = (synthAccent / 127) * 270 - 135;
    drawKnob(knobs.KNOB4);
    showStatus(`ACCENT INTENSITY: ${synthAccent}/127`);
  });
  knobs.KNOB4.angle = (synthAccent / 127) * 270 - 135;
  drawKnob(knobs.KNOB4);

  // Button Actions & Visual State
  function updateButtonVisuals() {
    if (buttons.PLAY) {
      buttons.PLAY.g.classList.toggle("lit", state.isRunning);
    }
    if (buttons.REC) {
      buttons.REC.g.classList.toggle("lit", state.recMode);
    }
    if (buttons.EDIT) {
      buttons.EDIT.g.classList.toggle("lit", state.shiftMode);
    }
    const info = document.getElementById("statusInfo");
    if (info) {
      const trk = state.tracks[state.activeTrack];
      const eng = trk ? (trk.engine !== undefined ? trk.engine : 0) : 0;
      info.textContent = `${formatBpm(state.bpm)} BPM • T${state.activeTrack + 1} (${ENGINE_NAMES[eng % ENGINE_NAMES.length]}) • ${state.isRunning ? 'RUNNING' : 'STOPPED'}`;
    }
  }

  function bindButton(b, onClick) {
    b.hit.addEventListener("pointerdown", (e) => {
      dismissSplash();
      e.preventDefault();
      b.g.classList.add("held");
      if (b.label === "EDIT") {
        setHeldStatus("⚡ SHIFT: +HOME=UNDO +SAVE=REDO +FX=RND");
      }
    });
    b.hit.addEventListener("pointerup", (e) => {
      b.g.classList.remove("held");
      if (b.label === "EDIT") {
        clearHeldStatus();
      }
    });
    b.hit.addEventListener("click", () => {
      onClick();
      updateButtonVisuals();
    });
  }

  bindButton(buttons["OCT-"], () => {
    if (state.viewOctave > 1) {
      state.viewOctave--;
      showStatus(`OCTAVE: C${state.viewOctave}..B${state.viewOctave} (OCT ${state.viewOctave})`);
    }
  });

  bindButton(buttons["OCT+"], () => {
    if (state.viewOctave < 5) {
      state.viewOctave++;
      showStatus(`OCTAVE: C${state.viewOctave}..B${state.viewOctave} (OCT ${state.viewOctave})`);
    }
  });

  bindButton(buttons.PLAY, () => {
    if (state.isRunning) {
      stopSequencer();
      showStatus("STOP: SEQUENCER STOPPED");
    } else {
      startSequencer();
      showStatus("PLAY: SEQUENCER RUNNING");
    }
  });

  bindButton(buttons.REC, () => {
    state.recMode = !state.recMode;
    showStatus(state.recMode ? "REC: TAP KEY TO RECORD" : "REC: STEP REC OFF");
  });

  bindButton(buttons.FX, () => {
    if (state.shiftMode) {
      randomizePattern();
      showStatus("🎲 RANDOMIZED PATTERN");
      return;
    }
    state.menuMode = (state.menuMode === 'fx') ? 'none' : 'fx';
    showStatus(state.menuMode === 'fx' ? "PATTERN TOOLS OPEN" : "PATTERN TOOLS CLOSED");
  });

  bindButton(buttons.SEL, () => {
    state.viewPage = state.viewPage === 0 ? 1 : 0;
    showStatus(state.viewPage === 0 ? "PAGE 1: STEPS 1–16" : "PAGE 2: STEPS 17–32");
  });

  bindButton(buttons.ENV, () => {
    state.viewMode = state.viewMode === 'keys' ? 'curve' : 'keys';
    showStatus(state.viewMode === 'keys' ? "VIEW: NOTE PITCH / DRUM" : "VIEW: CC CURVE (CURV)");
  });

  bindButton(buttons.LFO, () => {
    const trk = state.tracks[state.activeTrack];
    const pat = trk.patterns[trk.curBank * 8 + trk.curPat];
    pushUndo("Change Engine");
    trk.engine = ((trk.engine || 0) + 1) % ENGINE_NAMES.length;
    pat.engine = trk.engine;
    showStatus(`TRACK ${state.activeTrack + 1} ENGINE: ${ENGINE_NAMES[trk.engine]}`);
  });

  bindButton(buttons.EDIT, () => {
    state.shiftMode = !state.shiftMode;
    showStatus(state.shiftMode ? "⚡ SHIFT ACTIVE (+HOME:UNDO)" : "SHIFT INACTIVE", 1200);
  });

  bindButton(buttons.GLO, () => {
    const trk = state.tracks[state.activeTrack];
    const pat = trk.patterns[trk.curBank * 8 + trk.curPat];
    pat.length = pat.length === 16 ? 32 : (pat.length === 32 ? 24 : 16);
    showStatus(`TRACK ${state.activeTrack + 1} LENGTH: ${pat.length} STEPS`);
  });

  bindButton(buttons.HOME, () => {
    if (state.shiftMode) {
      undo();
      showStatus("UNDO: RESTORED STATE");
      return;
    }
    if (state.menuMode === 'fx') {
      executeFxMenuItem(state.menuSel);
      showStatus("ACTION EXECUTED");
      return;
    }
    state.activeTrack = (state.activeTrack + 1) % NUM_TRACKS;
    const trk = state.tracks[state.activeTrack];
    const eng = trk.engine !== undefined ? trk.engine : 0;
    showStatus(`TRACK ${state.activeTrack + 1}: ${ENGINE_NAMES[eng % ENGINE_NAMES.length]}`);
  });

  bindButton(buttons.SAVE, () => {
    if (state.shiftMode) {
      redo();
      showStatus("REDO: RE-APPLIED EDIT");
      return;
    }
    try {
      localStorage.setItem('morpheus_patterns', JSON.stringify(state.tracks));
      showStatus("💾 SAVED ALL PATTERNS TO STORAGE");
    } catch (e) {
      alert('Could not save to localStorage: ' + e);
    }
  });

  bindButton(buttons.ARP, () => {
    const trk = state.tracks[state.activeTrack];
    trk.nextBank = (trk.curBank + 1) % 4;
    trk.curBank = trk.nextBank;
    showStatus(`BANK: BANK ${String.fromCharCode(65 + trk.curBank)}`);
  });

  bindButton(buttons.SEQ, () => {
    const trk = state.tracks[state.activeTrack];
    trk.nextPat = (trk.curPat + 1) % 8;
    trk.curPat = trk.nextPat;
    showStatus(`PATTERN: PAT ${trk.curPat + 1} (BANK ${String.fromCharCode(65 + trk.curBank)})`);
  });

  // 27 Silicone Keys Handling
  const KEY_NOTES = [
    53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65,
    66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79
  ];

  keys.forEach((k, idx) => {
    if (!k) return;
    const midiNote = KEY_NOTES[idx];

    k.hit.addEventListener("pointerdown", (e) => {
      dismissSplash();
      e.preventDefault();
      initAudio();
      k.g.classList.add("held");

      const trk = state.tracks[state.activeTrack];
      const pat = trk.patterns[trk.curBank * 8 + trk.curPat];
      const eng = trk.engine !== undefined ? trk.engine : pat.engine;
      const isDrum = (eng === 2 || eng === 3);

      if (isDrum) {
        const bit = idx % 12;
        const lane = 11 - bit;
        if (state.recMode) {
          pushUndo("Record Drum");
          pat.steps[state.activeStep].drums ^= (1 << bit);
          if (pat.steps[state.activeStep].gate === 0) pat.steps[state.activeStep].gate = 115;
          showStatus(`REC S${state.activeStep + 1}: ${DRUM_LABELS[lane]}`);
          state.activeStep = (state.activeStep + 1) % pat.length;
          state.viewPage = state.activeStep >= 16 ? 1 : 0;
        } else {
          showStatus(`AUDITION: ${DRUM_LABELS[lane]} (${DRUM_FULL_NAMES[lane]})`);
        }
        playDrumVoice(eng, lane, 115, bit === 11);
        return;
      }

      if (state.recMode) {
        pushUndo("Record Note");
        pat.steps[state.activeStep].note = midiNote;
        pat.steps[state.activeStep].gate = 115;
        showStatus(`RECORD STEP ${state.activeStep + 1}: ${getNoteName(midiNote)} (${midiNote})`);
        state.activeStep = (state.activeStep + 1) % pat.length;
        state.viewPage = state.activeStep >= 16 ? 1 : 0;
      } else if (idx < 16) {
        state.activeStep = (state.viewPage * 16) + idx;
        const st = pat.steps[state.activeStep];
        showStatus(st.gate > 0 ? `STEP ${state.activeStep + 1}/32: ${getNoteName(st.note)} (V:${st.gate})` : `STEP ${state.activeStep + 1}/32: [REST]`);
      } else {
        showStatus(`AUDITION: ${getNoteName(midiNote)} (${midiNote}) VEL:110`);
      }

      play303Note(eng, midiNote, 110, false, 64);
    });

    k.hit.addEventListener("pointerup", () => {
      k.g.classList.remove("held");
    });

    k.hit.addEventListener("pointercancel", () => {
      k.g.classList.remove("held");
    });
  });

  // Start Overlay Switch On Button
  const btnStart = document.getElementById("btnStart");
  const startOverlay = document.getElementById("start");
  const statusEl = document.getElementById("status");

  btnStart.addEventListener("click", () => {
    initAudio();
    startOverlay.hidden = true;
    startOverlay.style.display = "none";
    state.hasStarted = true;
    state.splashUntil = performance.now() + 1800;
    startSequencer();
    if (statusEl) {
      statusEl.textContent = "Running. 8-Track ReBirth Groovebox & Matrix Sequencer active.";
    }
    showHint("▶️ MORPHEUS v1.0.0 running! 808/909 drums & dual 303 synths ready.");
  });

  // Bottom bar buttons
  const muteBtn = document.getElementById("muteBtn");
  if (muteBtn) {
    muteBtn.addEventListener("click", () => {
      const isMuted = muteBtn.getAttribute("aria-pressed") === "true";
      muteBtn.setAttribute("aria-pressed", String(!isMuted));
      if (masterGain && audioCtx) {
        masterGain.gain.setValueAtTime(!isMuted ? 0 : state.masterVol, audioCtx.currentTime);
      }
      muteBtn.textContent = !isMuted ? "Unmute Audio" : "Mute Audio";
      showHint(!isMuted ? "🔇 Audio Muted" : "🔊 Audio Unmuted");
    });
  }

  const resetBtn = document.getElementById("resetBtn");
  if (resetBtn) {
    resetBtn.addEventListener("click", () => {
      if (confirm("Reset patterns to factory default?")) {
        localStorage.removeItem("morpheus_patterns");
        initTracks();
        showHint("Factory patterns reloaded.");
      }
    });
  }

  // ===========================================================================
  // Preset Grooves Selector
  // ===========================================================================
  function loadPreset(style) {
    pushUndo("Load Groove");
    initTracks();
    if (style === 'acid') {
      state.bpm = 128;
      synthCutoff = 80;
      synthResonance = 100;
      synthDecay = 70;
      synthAccent = 95;
      state.activeTrack = 0;
      showHint("⚡ Loaded Acid Techno Groove: 128 BPM, ReBirth dual 303s, and 808/909 drums.");
    } else if (style === 'electro') {
      state.bpm = 135;
      synthCutoff = 105;
      synthResonance = 80;
      synthDecay = 45;
      synthAccent = 90;
      state.activeTrack = 2; // D808
      showHint("🏙️ Loaded Detroit Electro Groove: 135 BPM, TR-808 drums, FM lead & punchy sub bass.");
    } else if (style === 'dub') {
      state.bpm = 114;
      synthCutoff = 40;
      synthResonance = 70;
      synthDecay = 115;
      synthAccent = 85;
      state.activeTrack = 4; // Breaks
      showHint("🌌 Loaded Ambient Dub Groove: 114 BPM, chopped breakbeats, deep sub, and dual 303 resonance.");
    } else if (style === 'polymetric') {
      state.bpm = 126;
      synthCutoff = 90;
      synthResonance = 95;
      synthDecay = 65;
      synthAccent = 100;
      state.activeTrack = 0;
      state.tracks[0].patterns[0].length = 7; // 7 steps polyrhythm
      showHint("🌀 Loaded Polymetric 7/8 Groove: 7-step acid bass against 16-step TR-909 beat!");
    }

    knobs.KNOB1.angle = (synthCutoff / 127) * 270 - 135;
    drawKnob(knobs.KNOB1);
    knobs.KNOB2.angle = (synthResonance / 127) * 270 - 135;
    drawKnob(knobs.KNOB2);
    knobs.KNOB3.angle = (synthDecay / 127) * 270 - 135;
    drawKnob(knobs.KNOB3);
    knobs.KNOB4.angle = (synthAccent / 127) * 270 - 135;
    drawKnob(knobs.KNOB4);
    knobs.TEMPO.angle = ((state.bpm - 50) / 190) * 270 - 135;
    drawKnob(knobs.TEMPO);

    document.querySelectorAll(".preset-btn").forEach(btn => btn.classList.remove("active"));
    const activeBtn = document.getElementById("preset" + style.charAt(0).toUpperCase() + style.slice(1));
    if (activeBtn) activeBtn.classList.add("active");
    updateButtonVisuals();
  }

  const pAcid = document.getElementById("presetAcid");
  if (pAcid) pAcid.addEventListener("click", () => loadPreset('acid'));
  const pElectro = document.getElementById("presetElectro");
  if (pElectro) pElectro.addEventListener("click", () => loadPreset('electro'));
  const pDub = document.getElementById("presetDub");
  if (pDub) pDub.addEventListener("click", () => loadPreset('dub'));
  const pPolymetric = document.getElementById("presetPolymetric");
  if (pPolymetric) pPolymetric.addEventListener("click", () => loadPreset('polymetric'));

  // ===========================================================================
  // Pattern Management Toolbar Handlers (Import / Export / Copy / Paste)
  // ===========================================================================
  const btnExportJson = document.getElementById("btnExportJson");
  if (btnExportJson) {
    btnExportJson.addEventListener("click", () => {
      const dataStr = "data:text/json;charset=utf-8," + encodeURIComponent(JSON.stringify(state.tracks, null, 2));
      const downloadAnchor = document.createElement("a");
      downloadAnchor.setAttribute("href", dataStr);
      downloadAnchor.setAttribute("download", `morpheus_patterns_${Date.now()}.json`);
      document.body.appendChild(downloadAnchor);
      downloadAnchor.click();
      downloadAnchor.remove();
      showHint("💾 Patterns downloaded to JSON file.");
    });
  }

  const btnImportJson = document.getElementById("btnImportJson");
  const fileImportJson = document.getElementById("fileImportJson");
  if (btnImportJson && fileImportJson) {
    btnImportJson.addEventListener("click", () => fileImportJson.click());
    fileImportJson.addEventListener("change", (e) => {
      const file = e.target.files[0];
      if (!file) return;
      const reader = new FileReader();
      reader.onload = (event) => {
        try {
          const parsed = JSON.parse(event.target.result);
          if (Array.isArray(parsed) && parsed.length === NUM_TRACKS) {
            state.tracks = parsed;
            localStorage.setItem("morpheus_patterns", JSON.stringify(state.tracks));
            showHint("✅ Successfully imported custom patterns into MORPHEUS!");
          } else {
            alert("Error: Invalid MORPHEUS pattern file structure.");
          }
        } catch (err) {
          alert("Error parsing JSON: " + err);
        }
      };
      reader.readAsText(file);
    });
  }

  const btnCopyPat = document.getElementById("btnCopyPat");
  if (btnCopyPat) {
    btnCopyPat.addEventListener("click", () => {
      const trk = state.tracks[state.activeTrack];
      const pat = trk.patterns[trk.curBank * 8 + trk.curPat];
      state.clipboard = JSON.parse(JSON.stringify(pat));
      showHint(`📋 Copied Pattern ${String.fromCharCode(65 + trk.curBank)}${trk.curPat + 1} to clipboard.`);
    });
  }

  const btnUndo = document.getElementById("btnUndo");
  if (btnUndo) {
    btnUndo.addEventListener("click", undo);
  }

  const btnRedo = document.getElementById("btnRedo");
  if (btnRedo) {
    btnRedo.addEventListener("click", redo);
  }

  const btnPastePat = document.getElementById("btnPastePat");
  if (btnPastePat) {
    btnPastePat.addEventListener("click", () => {
      if (!state.clipboard) {
        showHint("⚠️ Clipboard is empty! Click 'Copy' on a pattern first.");
        return;
      }
      pushUndo("Paste Pattern");
      const trk = state.tracks[state.activeTrack];
      trk.patterns[trk.curBank * 8 + trk.curPat] = JSON.parse(JSON.stringify(state.clipboard));
      showHint(`📋 Pasted into Pattern ${String.fromCharCode(65 + trk.curBank)}${trk.curPat + 1}!`);
    });
  }

  const btnRandomize = document.getElementById("btnRandomize");
  if (btnRandomize) {
    btnRandomize.addEventListener("click", randomizePattern);
  }

  const btnClear = document.getElementById("btnClear");
  if (btnClear) {
    btnClear.addEventListener("click", clearPattern);
  }

  // Toggle Screen Mouse/Touch Editing (Disabled by default for hardware fidelity)
  const btnToggleScreenEdit = document.getElementById("btnToggleScreenEdit");
  const screenEditState = document.getElementById("screenEditState");
  const deviceEl = document.getElementById("device");

  if (btnToggleScreenEdit) {
    btnToggleScreenEdit.addEventListener("click", () => {
      state.screenEditEnabled = !state.screenEditEnabled;
      btnToggleScreenEdit.setAttribute("aria-pressed", state.screenEditEnabled ? "true" : "false");
      btnToggleScreenEdit.classList.toggle("active-toggle", state.screenEditEnabled);
      if (deviceEl) deviceEl.classList.toggle("screen-edit-active", state.screenEditEnabled);

      if (screenEditState) {
        screenEditState.textContent = state.screenEditEnabled ? "ON (Mouse/Touch)" : "OFF (Hardware Faithful)";
      }

      if (state.screenEditEnabled) {
        showHint("✏️ Screen Edit ENABLED: Click or drag directly on the 240x240 LCD to draw notes and curves!");
      } else {
        showHint("🔒 Hardware Faithful Mode: Screen mouse editing disabled. Compose using panel knobs, buttons, and silicone keys.");
      }
    });
  }

  // =========================================================================
  // INTERACTIVE WALKTHROUGH ENGINE (Roland J-6 & MPC Style Creation Tutorial)
  // =========================================================================
  const pinLayer = document.createElement("div");
  pinLayer.className = "pin-layer";
  pinLayer.id = "pinLayer";
  if (deviceEl) deviceEl.appendChild(pinLayer);

  const WALKTHROUGH_STEPS = [
    {
      id: 1,
      badge: "Step 1 of 9",
      title: "Set Project Tempo & Starting Fresh",
      desc: "Turn the <strong>TEMPO</strong> knob clockwise to set the master clock to <strong>135.0 BPM</strong>. Watch the LCD screen update in real time with 0.1 BPM precision.",
      soundTip: "🎧 What You Hear: The internal sequencer master clock locks to a driving acid techno pulse.",
      spotlight: {
        turn: ["TEMPO"],
        pins: [{ label: "1", x: "10.5%", y: "32%", type: "press" }]
      },
      applyLive: () => {
        state.bpm = 135;
        knobs.TEMPO.angle = ((state.bpm - 50) / 190) * 270 - 135;
        drawKnob(knobs.TEMPO);
        showHint("⏱️ Master clock set to 135.0 BPM.");
        updateButtonVisuals();
      }
    },
    {
      id: 2,
      badge: "Step 2 of 9",
      title: "Engage the 303 Acid Bass Sound Engine",
      desc: "Press the <strong>LFO</strong> button to cycle the sound generator to the <strong>303 Acid Bass</strong> engine. MORPHEUS models a transistor diode ladder with non-linear saturation.",
      soundTip: "🎧 What You Hear: Raw, buzzy analog saw waveform with resonant diode-ladder saturation.",
      spotlight: {
        press: ["LFO"],
        pins: [{ label: "1", x: "79.8%", y: "30.9%", type: "press" }]
      },
      applyLive: () => {
        state.soundEngine = 0; // 303
        showHint("🎛️ Sound Engine set to 303 Acid Bass.");
        updateButtonVisuals();
      }
    },
    {
      id: 3,
      badge: "Step 3 of 9",
      title: "Record Bassline with Silicone Keys",
      desc: "Tap <strong>REC</strong> to turn on recording mode, then punch in notes on the <strong>silicone keys</strong> (C2, C2, Eb2, F2, G2, Bb2). Turn <strong>SELECT</strong> forward to insert rests!",
      soundTip: "🎧 What You Hear: Each silicone key press auditions the note live through the 303 engine and plots directly into the 16-step grid.",
      spotlight: {
        press: ["REC"],
        keys: [0, 3, 5, 7, 10],
        turn: ["SELECT"],
        pins: [
          { label: "1", x: "92.4%", y: "40.6%", type: "press" },
          { label: "2", x: "50%", y: "78%", type: "press" },
          { label: "3", x: "21.5%", y: "15.6%", type: "turn" }
        ]
      },
      applyLive: () => {
        pushUndo("Walkthrough Bassline");
        const trk = state.tracks[state.activeTrack];
        const pat = trk.patterns[trk.curBank * 8 + trk.curPat];
        pat.length = 16;
        const notes = [36, 36, 0, 39, 41, 0, 43, 46, 36, 0, 39, 0, 41, 43, 46, 0];
        for (let i = 0; i < 16; i++) {
          pat.steps[i].note = notes[i] ? notes[i] : 36;
          pat.steps[i].gate = notes[i] ? 110 : 0;
          pat.steps[i].tie = 0;
        }
        state.recMode = false;
        showHint("🎹 16-step minor pentatonic acid bassline recorded!");
        updateButtonVisuals();
      }
    },
    {
      id: 4,
      badge: "Step 4 of 9",
      title: "Sculpt Acid Slides (Ties) & Punchy Accents",
      desc: "Turn <strong>SELECT</strong> to step 4 and turn <strong>KNOB 2 (Resonance / Tie)</strong> to tie it for a smooth acid slide! Next, hold <strong>EDIT</strong> and turn <strong>KNOB 1</strong> to set velocity to 120 on steps 1 & 11 for screaming accents!",
      soundTip: "🎧 What You Hear: Tied steps glide pitch smoothly at 60ms without re-triggering the envelope. Accents snap the diode filter wide open with bite!",
      spotlight: {
        turn: ["KNOB2", "KNOB1", "SELECT"],
        hold: ["EDIT"],
        pins: [
          { label: "1", x: "71.4%", y: "15.6%", type: "turn" },
          { label: "2", x: "86.1%", y: "30.9%", type: "hold" }
        ]
      },
      applyLive: () => {
        pushUndo("Walkthrough Slides & Accents");
        const trk = state.tracks[state.activeTrack];
        const pat = trk.patterns[trk.curBank * 8 + trk.curPat];
        pat.steps[3].tie = 1;  // Step 4 slides into step 5
        pat.steps[7].tie = 1;  // Step 8 slides into step 9
        pat.steps[0].gate = 125; // Accent on step 1
        pat.steps[10].gate = 120; // Accent on step 11
        showHint("🌊 Portamento slides added on steps 4 & 8; Accents set on steps 1 & 11!");
        updateButtonVisuals();
      }
    },
    {
      id: 5,
      badge: "Step 5 of 9",
      title: "Perform with the Diode-Ladder Filter (K1–K4)",
      desc: "Tap <strong>PLAY</strong> to start playback. Now twist <strong>KNOB 1 (Cutoff)</strong> down to 60 and crank <strong>KNOB 2 (Resonance)</strong> up to 95 for maximum squelch!",
      soundTip: "🎧 What You Hear: Authentic self-oscillating acid resonance biting through the saturated transistor filter.",
      spotlight: {
        press: ["PLAY"],
        turn: ["KNOB1", "KNOB2"],
        pins: [
          { label: "1", x: "86.1%", y: "40.6%", type: "press" },
          { label: "2", x: "61.0%", y: "15.6%", type: "turn" },
          { label: "3", x: "71.4%", y: "15.6%", type: "turn" }
        ]
      },
      applyLive: () => {
        if (!state.isRunning) startSequencer();
        synthCutoff = 60;
        synthResonance = 122; // 96%
        synthDecay = 75;
        knobs.KNOB1.angle = (synthCutoff / 127) * 270 - 135;
        drawKnob(knobs.KNOB1);
        knobs.KNOB2.angle = (synthResonance / 127) * 270 - 135;
        drawKnob(knobs.KNOB2);
        knobs.KNOB3.angle = (synthDecay / 127) * 270 - 135;
        drawKnob(knobs.KNOB3);
        showHint("⚡ Sequencer started! Filter Cutoff: 60/127, Resonance: 122/127 (Iconic 303 squelch)");
        updateButtonVisuals();
      }
    },
    {
      id: 6,
      badge: "Step 6 of 9",
      title: "Automate Filter Sweeps on the Modulation Curve Track",
      desc: "Press <strong>ENV</strong> to switch into <strong>CURVE view</strong>. Turn <strong>KNOB 3 (Curve Value)</strong> while scrubbing steps to draw an ascending 16-step filter envelope assigned to MIDI CC 74!",
      soundTip: "🎧 What You Hear: Dynamic rhythmic filter opening automated across the bar, simultaneously sent over MIDI CC.",
      spotlight: {
        press: ["ENV"],
        turn: ["KNOB3", "SELECT"],
        pins: [
          { label: "1", x: "73.5%", y: "30.9%", type: "press" },
          { label: "2", x: "81.8%", y: "15.6%", type: "turn" }
        ]
      },
      applyLive: () => {
        state.viewMode = "curve";
        const trk = state.tracks[state.activeTrack];
        const pat = trk.patterns[trk.curBank * 8 + trk.curPat];
        for (let i = 0; i < 16; i++) {
          pat.steps[i].curve = Math.min(127, Math.floor(25 + (i * 6.8)));
        }
        showHint("📈 Ascending 16-step modulation curve drawn and routed to CC 74 (Cutoff).");
        updateButtonVisuals();
      }
    },
    {
      id: 7,
      badge: "Step 7 of 9",
      title: "Layer 909 Drums & Polymetric Cross-Rhythms",
      desc: "Turn <strong>TRACK</strong> to switch to Track 4 (MIDI Channel 10 for drums). Next, switch to Track 2 (Lead Synth) and tap <strong>GLO</strong> to set its length to 7 steps for a hypnotic 7/8 polyrhythm over 4/4!",
      soundTip: "🎧 What You Hear: Solid kick/snare driving underneath while the 7-step lead phase-shifts across the 16-step bassline.",
      spotlight: {
        turn: ["TRACK"],
        press: ["GLO"],
        pins: [
          { label: "1", x: "21.5%", y: "32%", type: "turn" },
          { label: "2", x: "92.4%", y: "30.9%", type: "press" }
        ]
      },
      applyLive: () => {
        // T4 drums on Ch 10
        const t4 = state.tracks[3].patterns[0];
        t4.length = 16;
        for (let i = 0; i < 16; i++) {
          if (i % 4 === 0) { t4.steps[i].note = 36; t4.steps[i].gate = 120; }
          else if (i % 4 === 2) { t4.steps[i].note = 38; t4.steps[i].gate = 110; }
          else if (i % 2 === 1) { t4.steps[i].note = 42; t4.steps[i].gate = 90; }
          else { t4.steps[i].gate = 0; }
        }
        // T2 polymetric 7-step lead
        const t2 = state.tracks[1].patterns[0];
        t2.length = 7;
        const leadNotes = [60, 63, 65, 0, 67, 70, 0];
        for (let i = 0; i < 7; i++) {
          t2.steps[i].note = leadNotes[i] || 60;
          t2.steps[i].gate = leadNotes[i] ? 100 : 0;
        }
        state.activeTrack = 0;
        state.viewMode = "keys";
        showHint("🥁 Drum triggers programmed on T4 (Ch 10); 7-step polymetric lead layered on T2!");
        updateButtonVisuals();
      }
    },
    {
      id: 8,
      badge: "Step 8 of 9",
      title: "Performance Tools & Multi-Level Undo / Redo",
      desc: "Tap <strong>FX</strong> to open Pattern Tools (Shift, Transpose, Alter, Randomize). Made an accidental edit? Hold <strong>EDIT</strong> and press <strong>HOME</strong> to instantly Undo! Hold <strong>EDIT</strong> and press <strong>SAVE</strong> to Redo!",
      soundTip: "🎧 What You Hear: The 8-level undo stack snapshots every edit, allowing fearless real-time improvisation.",
      spotlight: {
        press: ["FX"],
        hold: ["EDIT"],
        pins: [
          { label: "1", x: "86.1%", y: "30.9%", type: "hold" },
          { label: "2", x: "60.9%", y: "40.6%", type: "press" },
          { label: "3", x: "60.9%", y: "30.9%", type: "press" }
        ]
      },
      applyLive: () => {
        pushUndo("Pattern Tools Demo");
        state.menuMode = "fx";
        state.menuSel = 0;
        showHint("🛡️ Pattern Tools menu open! Undo/Redo protects your sequences from accidental edits.");
        updateButtonVisuals();
      }
    },
    {
      id: 9,
      badge: "Step 9 of 9",
      title: "Save to Hardware Flash & Export JSON Library",
      desc: "Press <strong>SAVE</strong> to write all 128 patterns to hardware flash memory (sector 0xFC000). In the browser, click <strong>Export JSON</strong> to download your full project library as a shareable file!",
      soundTip: "🎧 What You Hear: Confirmation flash write beep; patterns will now persist through power cycles.",
      spotlight: {
        press: ["SAVE"],
        pins: [{ label: "1", x: "67.2%", y: "40.6%", type: "press" }]
      },
      applyLive: () => {
        try {
          localStorage.setItem('morpheus_patterns', JSON.stringify(state.tracks));
        } catch (_) {}
        state.menuMode = "none";
        showHint("💾 Project saved to on-board flash storage (sector 0xFC000)!");
        updateButtonVisuals();
      }
    }
  ];

  let currentStepIdx = 0;
  let isWalkthroughActive = false;

  function clearSpotlights() {
    if (deviceEl) deviceEl.classList.remove("tutorial-mode");
    if (svg) svg.classList.remove("tutorial-dimmed");
    svg.querySelectorAll(".spotlight-press, .spotlight-hold, .spotlight-turn, .spotlight-key").forEach(el => {
      el.classList.remove("spotlight-press", "spotlight-hold", "spotlight-turn", "spotlight-key");
    });
    if (pinLayer) pinLayer.innerHTML = "";
  }

  function applyStepSpotlight(step) {
    clearSpotlights();
    if (!step || !step.spotlight) return;

    if (deviceEl) deviceEl.classList.add("tutorial-mode");
    if (svg) svg.classList.add("tutorial-dimmed");

    const sp = step.spotlight;

    if (sp.press) {
      sp.press.forEach(name => {
        const btn = buttons[name];
        if (btn && btn.g) btn.g.classList.add("spotlight-press");
        const knb = knobs[name];
        if (knb && knb.g) knb.g.classList.add("spotlight-press");
      });
    }

    if (sp.hold) {
      sp.hold.forEach(name => {
        const btn = buttons[name];
        if (btn && btn.g) btn.g.classList.add("spotlight-hold");
      });
    }

    if (sp.turn) {
      sp.turn.forEach(name => {
        const knb = knobs[name];
        if (knb && knb.g) knb.g.classList.add("spotlight-press");
      });
    }

    if (sp.keys) {
      sp.keys.forEach(kIdx => {
        const k = keys[kIdx];
        if (k && k.g) k.g.classList.add("spotlight-key");
      });
    }

    if (sp.pins && pinLayer) {
      sp.pins.forEach(pin => {
        const pEl = document.createElement("div");
        pEl.className = "tutorial-pin" + (pin.type === "hold" ? " pin-hold" : "");
        pEl.textContent = pin.label || "1";
        pEl.style.left = pin.x;
        pEl.style.top = pin.y;
        pinLayer.appendChild(pEl);
      });
    }
  }

  function renderWalkthroughHud() {
    const hud = document.getElementById("walkthroughHud");
    if (!hud) return;

    if (!isWalkthroughActive) {
      hud.style.display = "none";
      clearSpotlights();
      return;
    }

    hud.style.display = "flex";
    const step = WALKTHROUGH_STEPS[currentStepIdx];
    applyStepSpotlight(step);

    let dotsHtml = WALKTHROUGH_STEPS.map((s, idx) => {
      let cls = "walkthrough-dot";
      if (idx === currentStepIdx) cls += " active";
      else if (idx < currentStepIdx) cls += " completed";
      return `<span class="${cls}" data-idx="${idx}" title="Step ${idx+1}: ${s.title}"></span>`;
    }).join("");

    hud.innerHTML = `
      <div class="walkthrough-header">
        <div class="walkthrough-badge">🎓 Walkthrough &bull; ${step.badge}</div>
        <div class="walkthrough-progress-track">${dotsHtml}</div>
        <button class="walkthrough-btn-close" id="btnWalkthroughClose" type="button">Exit Walkthrough</button>
      </div>
      <h3 class="walkthrough-title">${step.title}</h3>
      <div class="walkthrough-body">${step.desc}</div>
      <div class="walkthrough-sound-tip">${step.soundTip}</div>
      <div class="walkthrough-actions">
        <div class="walkthrough-nav">
          <button class="walkthrough-btn" id="btnWalkthroughPrev" type="button" ${currentStepIdx === 0 ? "disabled" : ""}>&larr; Previous</button>
          <button class="walkthrough-btn" id="btnWalkthroughNext" type="button" ${currentStepIdx === WALKTHROUGH_STEPS.length - 1 ? "disabled" : ""}>Next Step &rarr;</button>
        </div>
        <button class="walkthrough-btn walkthrough-btn-primary" id="btnWalkthroughTry" type="button">▶ Perform Step Live</button>
      </div>
    `;

    const btnClose = document.getElementById("btnWalkthroughClose");
    if (btnClose) btnClose.addEventListener("click", stopWalkthrough);

    const btnPrev = document.getElementById("btnWalkthroughPrev");
    if (btnPrev) btnPrev.addEventListener("click", prevStep);

    const btnNext = document.getElementById("btnWalkthroughNext");
    if (btnNext) btnNext.addEventListener("click", nextStep);

    const btnTry = document.getElementById("btnWalkthroughTry");
    if (btnTry) btnTry.addEventListener("click", () => {
      step.applyLive();
    });

    hud.querySelectorAll(".walkthrough-dot").forEach(dot => {
      dot.addEventListener("click", (e) => {
        goToStep(parseInt(e.target.dataset.idx, 10));
      });
    });
  }

  function startWalkthrough(initialStep = 0) {
    isWalkthroughActive = true;
    currentStepIdx = initialStep;
    renderWalkthroughHud();
    const btn = document.getElementById("btnStartWalkthrough");
    if (btn) btn.classList.add("active-toggle");
    showHint(`🎓 Interactive Walkthrough active: Step ${currentStepIdx+1} of ${WALKTHROUGH_STEPS.length}. Click the highlighted controls or tap 'Perform Step Live'!`);
  }

  function stopWalkthrough() {
    isWalkthroughActive = false;
    clearSpotlights();
    const hud = document.getElementById("walkthroughHud");
    if (hud) hud.style.display = "none";
    const btn = document.getElementById("btnStartWalkthrough");
    if (btn) btn.classList.remove("active-toggle");
    showHint("Exited walkthrough mode. Free play active.");
  }

  function nextStep() {
    if (currentStepIdx < WALKTHROUGH_STEPS.length - 1) {
      currentStepIdx++;
      renderWalkthroughHud();
    }
  }

  function prevStep() {
    if (currentStepIdx > 0) {
      currentStepIdx--;
      renderWalkthroughHud();
    }
  }

  function goToStep(idx) {
    if (idx >= 0 && idx < WALKTHROUGH_STEPS.length) {
      currentStepIdx = idx;
      renderWalkthroughHud();
    }
  }

  const btnStartWalkthrough = document.getElementById("btnStartWalkthrough");
  if (btnStartWalkthrough) {
    btnStartWalkthrough.addEventListener("click", () => {
      if (isWalkthroughActive) {
        stopWalkthrough();
      } else {
        startWalkthrough(0);
      }
    });
  }

  // Check URL hash for direct deep-link into tutorial steps (e.g. #walkthrough or #step3)
  if (window.location.hash.includes("walkthrough") || window.location.hash.includes("step")) {
    const match = window.location.hash.match(/step(\d+)/);
    const startIdx = match ? Math.max(0, parseInt(match[1], 10) - 1) : 0;
    setTimeout(() => {
      startWalkthrough(startIdx);
    }, 400);
  }

  window.MorpheusWalkthrough = {
    start: startWalkthrough,
    stop: stopWalkthrough,
    next: nextStep,
    prev: prevStep,
    goToStep: goToStep,
    getSteps: () => WALKTHROUGH_STEPS,
    isActive: () => isWalkthroughActive
  };

  // Kick off screen rendering loop
  loop();
  updateButtonVisuals();
})();
