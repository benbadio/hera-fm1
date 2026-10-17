/* SPDX-License-Identifier: GPL-3.0-only */
/* MORPHEUS: Authentic M-VAVE FM-1 In-Browser Emulator */

(() => {
  'use strict';

  // Constants
  const NUM_TRACKS = 4;
  const TOTAL_PATS = 32;
  const MAX_STEPS = 32;
  const SOUND_MODES = ['303', 'SUB', 'FM', 'MUTE'];

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
    soundEngine: 0,   // 0=303, 1=SUB, 2=FM, 3=MUTE
    clipboard: null,
    tracks: []
  };

  // Factory Patterns initialization
  function createDefaultPattern(trackIdx) {
    const pat = {
      length: 16,
      resolution: 16,
      shuffle: 0,
      midiCh: trackIdx === 3 ? 10 : (trackIdx + 1),
      mute: false,
      curveCC: 74,
      steps: []
    };

    for (let s = 0; s < MAX_STEPS; s++) {
      pat.steps.push({ note: 48, gate: 0, tie: 0, curve: 64 });
    }

    if (trackIdx === 0) {
      // Track 1: 303 Acid Bassline
      const notes  = [48, 48, 60, 58, 55, 55, 53, 55, 51, 51, 53, 53, 48, 60, 58, 55];
      const gates  = [110,  0, 127, 95, 115,  0, 100, 120, 90,  0, 105, 115, 120, 127, 95,  0];
      const ties   = [  0,  0,   1,  0,   0,  0,   0,   1,  0,  0,   0,   1,   0,   1,  0,  0];
      const curves = [ 30, 45, 120, 80,  60, 40,  75, 110, 35, 20,  70, 100, 115, 125, 80, 40];
      for (let s = 0; s < 16; s++) {
        pat.steps[s] = { note: notes[s], gate: gates[s], tie: ties[s], curve: curves[s] };
      }
    } else if (trackIdx === 1) {
      // Track 2: Lead Synth
      const notes  = [60, 60, 63, 63, 65, 67, 65, 63, 60, 58, 60, 63, 67, 70, 67, 65];
      const gates  = [100, 100, 110, 110, 120, 125, 115, 105, 95, 95, 110, 115, 127, 120, 110, 100];
      const ties   = [  0,   0,   0,   1,   0,   0,   1,   0,  0,  0,   0,   1,   0,   0,   1,   0];
      const curves = [ 60,  65,  70,  75,  80,  90, 100,  85, 70, 65,  75,  90, 110, 120,  95,  70];
      for (let s = 0; s < 16; s++) {
        pat.steps[s] = { note: notes[s], gate: gates[s], tie: ties[s], curve: curves[s] };
      }
    } else if (trackIdx === 2) {
      // Track 3: Arpeggiator
      const notes  = [48, 55, 60, 55, 51, 55, 58, 55, 53, 56, 60, 56, 55, 58, 62, 58];
      const gates  = [90, 85, 100, 85, 90, 85, 95, 85, 90, 85, 100, 85, 95, 85, 110, 85];
      const curves = [40, 50,  60, 70, 80, 90, 80, 70, 60, 50,  60, 70, 80, 90, 100, 80];
      for (let s = 0; s < 16; s++) {
        pat.steps[s] = { note: notes[s], gate: gates[s], tie: 0, curve: curves[s] };
      }
    } else if (trackIdx === 3) {
      // Track 4: Drum triggers (Kick=36, Snare=38, Hat=42, OHat=46)
      const notes  = [36, 42, 38, 42, 36, 42, 38, 46, 36, 42, 38, 42, 36, 42, 38, 46];
      const gates  = [127, 80, 115, 80, 120, 80, 115, 85, 127, 80, 115, 80, 120, 90, 120, 95];
      const curves = [ 20, 30,  40, 50,  60, 70,  80, 90, 100, 90,  80, 70,  60, 50,  40, 30];
      for (let s = 0; s < 16; s++) {
        pat.steps[s] = { note: notes[s], gate: gates[s], tie: 0, curve: curves[s] };
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
        patterns: []
      };
      for (let p = 0; p < TOTAL_PATS; p++) {
        trk.patterns.push(createDefaultPattern(t));
      }
      state.tracks.push(trk);
    }

    try {
      const saved = localStorage.getItem('morpheus_patterns');
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
    SELECT: "📍 SELECT: Scrubs the active step cursor across the 32-step sequence. With EDIT: Transpose ±12 semitones.",
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
    FX: "🎲 FX (Randomize): Generates a fresh musical acid bassline using a minor pentatonic scale.",
    SEL: "📄 SEL (Page): Toggles between Page 1 (Steps 1–16) and Page 2 (Steps 17–32).",
    ENV: "📈 ENV (Mode): Switches between KEYS view (note pitches) and CURVE view (filter modulation curves).",
    LFO: "🔊 LFO (Engine): Cycles sound engines: 303 Acid Bass, Subtractive Lead, 2-OP FM, or silent MUTE mode.",
    EDIT: "⚡ EDIT (Shift): Secondary modifier button for track transpose, swing, and loop lengths.",
    GLO: "📏 GLO (Length): Cycles track sequence length (16, 24, or 32 steps) for polymetric grooves.",
    HOME: "🏠 HOME (Track): Cycles through Track 1 (Bass), Track 2 (Lead), Track 3 (Arp), and Track 4 (Drums).",
    SAVE: "💾 SAVE: Writes all 4 tracks, banks, and patterns to persistent storage.",
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
  let activeVoice = null;

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

  // 303 Acid Bass Voice
  function play303Note(note, velocity, isTied, curveVal) {
    if (!audioCtx || state.soundEngine === 3) return; // MUTE

    const now = audioCtx.currentTime;
    const freq = midiToFreq(note);
    const isAccent = velocity >= 105;

    const cutoffBase = (synthCutoff / 127) * 4500 + 100;
    const modOffset = (curveVal - 64) * 20;
    const accentBoost = isAccent ? ((synthAccent / 127) * 4000 + 1000) : 1800;
    const peakFreq = Math.min(12000, Math.max(150, cutoffBase + accentBoost + modOffset));
    const filterQ = (synthResonance / 127) * 18 + 1;
    const decayTime = 0.06 + (synthDecay / 127) * 0.40;

    if (isTied && activeVoice) {
      activeVoice.osc.frequency.cancelScheduledValues(now);
      activeVoice.osc.frequency.linearRampToValueAtTime(freq, now + 0.06);

      activeVoice.filter.frequency.cancelScheduledValues(now);
      activeVoice.filter.frequency.linearRampToValueAtTime(peakFreq, now + 0.05);
      activeVoice.filter.frequency.exponentialRampToValueAtTime(cutoffBase, now + decayTime);
      return;
    }

    if (activeVoice) {
      try {
        activeVoice.gain.gain.cancelScheduledValues(now);
        activeVoice.gain.gain.linearRampToValueAtTime(0.001, now + 0.01);
        activeVoice.osc.stop(now + 0.02);
      } catch (e) {}
    }

    const osc = audioCtx.createOscillator();
    const filter = audioCtx.createBiquadFilter();
    const gain = audioCtx.createGain();

    if (state.soundEngine === 0) {
      osc.type = 'sawtooth';
    } else if (state.soundEngine === 1) {
      osc.type = 'square';
    } else {
      osc.type = 'triangle';
    }

    osc.frequency.setValueAtTime(freq, now);

    filter.type = 'lowpass';
    filter.Q.setValueAtTime(filterQ, now);
    filter.frequency.setValueAtTime(peakFreq, now);
    filter.frequency.exponentialRampToValueAtTime(Math.max(80, cutoffBase), now + decayTime);

    const ampVol = (velocity / 127) * (isAccent ? 0.55 : 0.40);
    gain.gain.setValueAtTime(0.001, now);
    gain.gain.linearRampToValueAtTime(ampVol, now + 0.005);
    gain.gain.exponentialRampToValueAtTime(0.001, now + decayTime + 0.05);

    osc.connect(filter);
    filter.connect(gain);
    gain.connect(masterGain);

    osc.start(now);
    osc.stop(now + decayTime + 0.1);

    activeVoice = { osc, filter, gain };
  }

  // Drum Synth for Track 4
  function playDrumNote(note, velocity) {
    if (!audioCtx || state.soundEngine === 3) return;
    const now = audioCtx.currentTime;

    if (note === 36) {
      const osc = audioCtx.createOscillator();
      const gain = audioCtx.createGain();
      osc.frequency.setValueAtTime(140, now);
      osc.frequency.exponentialRampToValueAtTime(45, now + 0.08);

      gain.gain.setValueAtTime((velocity / 127) * 0.7, now);
      gain.gain.exponentialRampToValueAtTime(0.001, now + 0.22);

      osc.connect(gain);
      gain.connect(masterGain);
      osc.start(now);
      osc.stop(now + 0.25);
    } else if (note === 38) {
      const osc = audioCtx.createOscillator();
      const oscGain = audioCtx.createGain();
      osc.frequency.setValueAtTime(190, now);
      osc.frequency.exponentialRampToValueAtTime(80, now + 0.06);
      oscGain.gain.setValueAtTime(0.3, now);
      oscGain.gain.exponentialRampToValueAtTime(0.001, now + 0.12);
      osc.connect(oscGain);
      oscGain.connect(masterGain);
      osc.start(now);
      osc.stop(now + 0.15);
    } else if (note === 42 || note === 46) {
      const osc = audioCtx.createOscillator();
      const gain = audioCtx.createGain();
      osc.type = 'triangle';
      osc.frequency.setValueAtTime(6000, now);
      gain.gain.setValueAtTime((velocity / 127) * 0.2, now);
      gain.gain.exponentialRampToValueAtTime(0.001, now + (note === 46 ? 0.2 : 0.04));
      osc.connect(gain);
      gain.connect(masterGain);
      osc.start(now);
      osc.stop(now + 0.22);
    }
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

      if (step.gate > 0) {
        if (t === 0) {
          play303Note(step.note, step.gate, step.tie, step.curve);
        } else if (t === 3) {
          playDrumNote(step.note, step.gate);
        } else {
          play303Note(step.note, step.gate * 0.7, step.tie, step.curve);
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

  function renderScreen() {
    ctx.fillStyle = '#080a0c';
    ctx.fillRect(0, 0, 240, 240);

    const trkIdx = state.activeTrack;
    const trk = state.tracks[trkIdx];
    const pat = trk.patterns[trk.curBank * 8 + trk.curPat];
    const stepOffset = state.viewPage * 16;
    const curPlayStep = trk.curStep;

    // 1. Top Header Bar (y: 0..25)
    ctx.fillStyle = '#14181c';
    ctx.fillRect(0, 0, 240, 25);
    ctx.fillStyle = '#222830';
    ctx.fillRect(0, 25, 240, 1);

    // Track Badge: [T1]
    ctx.fillStyle = '#f0a043';
    ctx.fillRect(4, 4, 20, 17);
    ctx.fillStyle = '#0a0c0e';
    ctx.font = 'bold 10px monospace';
    ctx.textAlign = 'center';
    ctx.fillText(`T${trkIdx + 1}`, 14, 16);

    for (let t = 0; t < 4; t++) {
      ctx.fillStyle = (t === trkIdx) ? '#f0a043' : '#444d56';
      ctx.fillRect(28 + t * 6, 10, 4, 5);
    }

    // Pattern Badge: A1
    ctx.fillStyle = '#222830';
    ctx.fillRect(54, 4, 18, 17);
    ctx.fillStyle = '#e0e6ed';
    ctx.font = 'bold 9px monospace';
    ctx.fillText(`${String.fromCharCode(65 + trk.curBank)}${trk.curPat + 1}`, 63, 16);

    // Sound Mode badge
    ctx.fillStyle = '#ff9900';
    ctx.fillRect(76, 4, 34, 17);
    ctx.fillStyle = '#000';
    ctx.font = 'bold 8px monospace';
    ctx.fillText(SOUND_MODES[state.soundEngine], 93, 16);

    // View Mode badge
    ctx.fillStyle = state.viewMode === 'keys' ? '#00cc88' : '#3399ff';
    ctx.fillRect(114, 4, 42, 17);
    ctx.fillStyle = '#0a0c0e';
    ctx.fillText(state.viewMode === 'keys' ? 'KEYS' : 'CURV', 135, 16);

    // MIDI CH
    ctx.fillStyle = '#808b96';
    ctx.font = '8px monospace';
    ctx.fillText(`CH${pat.midiCh < 10 ? '0' + pat.midiCh : pat.midiCh}`, 174, 16);

    // Run / Stop
    ctx.fillStyle = state.isRunning ? '#00e676' : '#607080';
    ctx.beginPath();
    ctx.arc(205, 12, 3.5, 0, Math.PI * 2);
    ctx.fill();
    ctx.font = 'bold 8px monospace';
    ctx.textAlign = 'left';
    ctx.fillText(state.isRunning ? 'RUN' : 'STOP', 212, 15);

    // 2. Upper Note Grid (y: 26..148)
    const gridX = 22;
    const gridW = 216;
    const rowH = 10;
    const colW = 13;

    // Piano Keyboard Rail on Left
    ctx.fillStyle = '#14181c';
    ctx.fillRect(0, 26, gridX, 122);
    ctx.fillStyle = '#222830';
    ctx.fillRect(gridX - 1, 26, 1, 122);

    for (let r = 0; r < 12; r++) {
      const ky = 26 + r * rowH;
      if (PIANO_IS_BLACK[r]) {
        ctx.fillStyle = '#191c20';
        ctx.fillRect(0, ky, 13, rowH - 1);
        ctx.fillStyle = '#14181c';
        ctx.fillRect(13, ky, gridX - 14, rowH - 1);
      } else {
        ctx.fillStyle = '#d7dce1';
        ctx.fillRect(0, ky, gridX - 2, rowH - 1);
      }
    }

    // Grid Area
    ctx.fillStyle = '#0c0f12';
    ctx.fillRect(gridX, 26, gridW, 120);

    ctx.strokeStyle = '#181d22';
    ctx.lineWidth = 1;
    for (let r = 0; r <= 12; r++) {
      ctx.beginPath();
      ctx.moveTo(gridX, 26 + r * rowH);
      ctx.lineTo(gridX + gridW, 26 + r * rowH);
      ctx.stroke();
    }

    for (let c = 0; c <= 16; c++) {
      ctx.strokeStyle = (c % 4 === 0) ? '#28303a' : '#181d22';
      ctx.beginPath();
      ctx.moveTo(gridX + c * colW, 26);
      ctx.lineTo(gridX + c * colW, 146);
      ctx.stroke();
    }

    // Steps Rendering
    for (let sIdx = 0; sIdx < 16; sIdx++) {
      const actualStep = stepOffset + sIdx;
      if (actualStep >= pat.length) {
        ctx.fillStyle = '#06080a';
        ctx.fillRect(gridX + sIdx * colW, 26, colW, 120);
        continue;
      }

      const st = pat.steps[actualStep];

      if (state.isRunning && actualStep === curPlayStep) {
        ctx.fillStyle = 'rgba(240, 160, 67, 0.15)';
        ctx.fillRect(gridX + sIdx * colW, 26, colW, 120);
      }

      if (actualStep === state.activeStep) {
        ctx.strokeStyle = '#f0a043';
        ctx.lineWidth = 1.5;
        ctx.strokeRect(gridX + sIdx * colW + 1, 27, colW - 2, 118);
      }

      if (state.viewMode === 'keys') {
        if (st.gate > 0) {
          const noteOct = Math.floor(st.note / 12) - 1;
          const noteSemi = st.note % 12;
          const rowIdx = 11 - noteSemi;

          if (noteOct === state.viewOctave && rowIdx >= 0 && rowIdx < 12) {
            const ny = 26 + rowIdx * rowH;
            ctx.fillStyle = (actualStep === state.activeStep) ? '#fff' : (st.gate > 105 ? '#ff9900' : '#f0a043');
            ctx.fillRect(gridX + sIdx * colW + 2, ny + 2, colW - 3, rowH - 3);

            if (st.tie && sIdx < 15) {
              ctx.fillStyle = '#ffb366';
              ctx.fillRect(gridX + sIdx * colW + colW - 2, ny + 4, 3, 3);
            }
          }
        }
      } else {
        const cvY = 146 - (st.curve / 127) * 116;
        ctx.fillStyle = '#3399ff';
        ctx.fillRect(gridX + sIdx * colW + 2, cvY - 2, colW - 3, 5);
      }
    }

    // 3. Lower Section: Velocity Bars / Tied Bridges (y: 150..218)
    ctx.fillStyle = '#101418';
    ctx.fillRect(0, 148, 240, 72);
    ctx.fillStyle = '#222830';
    ctx.fillRect(0, 148, 240, 1);

    ctx.fillStyle = '#808b96';
    ctx.font = '8px monospace';
    ctx.textAlign = 'left';
    ctx.fillText('GATE', 2, 160);
    ctx.fillText('VEL', 4, 170);

    for (let sIdx = 0; sIdx < 16; sIdx++) {
      const actualStep = stepOffset + sIdx;
      if (actualStep >= pat.length) continue;

      const st = pat.steps[actualStep];
      const bx = gridX + sIdx * colW;

      if (st.gate > 0) {
        const barH = (st.gate / 127) * 58;
        const by = 216 - barH;

        ctx.fillStyle = (actualStep === state.activeStep) ? '#ffffff' : (st.gate > 105 ? '#ff9900' : '#f0a043');
        ctx.fillRect(bx + 2, by, colW - 4, barH);

        if (st.tie && sIdx < 15) {
          ctx.fillStyle = '#ff6600';
          ctx.fillRect(bx + colW - 3, by + 4, 5, 4);
        }
      } else {
        ctx.fillStyle = '#26303c';
        ctx.fillRect(bx + 2, 214, colW - 4, 2);
      }
    }

    // 4. Bottom Status Bar (y: 220..240)
    ctx.fillStyle = '#14181c';
    ctx.fillRect(0, 220, 240, 20);
    ctx.fillStyle = '#222830';
    ctx.fillRect(0, 220, 240, 1);

    ctx.fillStyle = '#f0a043';
    ctx.font = 'bold 9px monospace';
    ctx.fillText(`P${state.viewPage + 1} OCT:${state.viewOctave}`, 6, 233);

    ctx.fillStyle = '#e0e6ed';
    ctx.fillText(`${state.bpm}BPM`, 82, 233);

    ctx.fillStyle = '#808b96';
    ctx.fillText('CLICK:EDIT', 148, 233);
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
    const cy = (e.clientY - rect.top) * scale;
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
    initAudio();
    const rect = canvas.getBoundingClientRect();
    const scale = 240 / rect.width;
    const cx = (e.clientX - rect.left) * scale;
    const cy = (e.clientY - rect.top) * scale;

    const trk = state.tracks[state.activeTrack];
    const pat = trk.patterns[trk.curBank * 8 + trk.curPat];
    const stepOffset = state.viewPage * 16;
    const gridX = 22;
    const colW = 13;
    const rowH = 10;

    // 1. Clicked inside the Note Grid (y: 26..146)
    if (cx >= gridX && cx <= gridX + 16 * colW && cy >= 26 && cy <= 146) {
      const col = Math.floor((cx - gridX) / colW);
      const row = Math.floor((cy - 26) / rowH);
      const actualStep = stepOffset + col;

      if (actualStep < pat.length && row >= 0 && row < 12) {
        state.activeStep = actualStep;
        const st = pat.steps[actualStep];

        if (state.viewMode === 'keys') {
          const noteSemi = 11 - row;
          const targetNote = (state.viewOctave + 1) * 12 + noteSemi;

          if (st.gate > 0 && st.note === targetNote) {
            st.gate = 0; // Toggle off
          } else {
            st.note = targetNote;
            st.gate = 115;
            st.tie = 0;
            play303Note(targetNote, 115, false, st.curve);
          }
        } else {
          const curveVal = Math.min(127, Math.max(0, Math.round((146 - cy) / 116 * 127)));
          st.curve = curveVal;
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
        st.gate = vel;
        if (vel > 0) {
          play303Note(st.note, vel, false, st.curve);
        }
      }
    }
  });

  // ===========================================================================
  // SVG Front Panel & Controls Generation (1:1 with M-VAVE FM-1)
  // ===========================================================================
  const NS = "http://www.w3.org/2000/svg";
  const BODY = "#2e3238", TRAY = "#212428", KEY_W = "#5e636b", KEY_B = "#484d54", KNOB = "#1b1d20", BTNC = "#272a2e", LABEL = "#d0d4da";
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
  el("rect", { x: 300, y: 44, width: 236, height: 236, rx: 26, fill: "#111214" });

  // 3. Knobs Builder
  const knobs = {};
  function knob(x, y, r, label, role) {
    const g = el("g", { class: "ctl knob" });
    el("circle", { cx: x, cy: y, r: r + 5, fill: TRAY }, g);
    el("circle", { cx: x, cy: y, r, fill: KNOB, stroke: "#44474c", "stroke-width": 2 }, g);
    const ind = el("line", { x1: x, y1: y - r + 5, x2: x, y2: y - r + 15, stroke: LABEL, "stroke-width": 3, "stroke-linecap": "round" }, g);
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
    el("rect", { x, y, width: w, height: h, rx: 9, fill: BTNC, stroke: "#4a4d52", "stroke-width": 1.5, class: "btnface" }, g);
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
  });
  knobs.MASTER.angle = -135 + state.masterVol * 270;
  drawKnob(knobs.MASTER);

  bindKnob(knobs.SELECT, (dy) => {
    state.activeStep = (state.activeStep + (dy > 0 ? 1 : -1) + 32) % 32;
    state.viewPage = state.activeStep >= 16 ? 1 : 0;
    knobs.SELECT.angle = (knobs.SELECT.angle + (dy > 0 ? 15 : -15)) % 360;
    drawKnob(knobs.SELECT);
  });

  bindKnob(knobs.TEMPO, (dy) => {
    state.bpm = Math.min(240, Math.max(50, state.bpm + dy));
    knobs.TEMPO.angle = ((state.bpm - 50) / 190) * 270 - 135;
    drawKnob(knobs.TEMPO);
  });
  knobs.TEMPO.angle = ((state.bpm - 50) / 190) * 270 - 135;
  drawKnob(knobs.TEMPO);

  bindKnob(knobs.TRACK, (dy) => {
    if (Math.abs(dy) >= 1) {
      state.activeTrack = (state.activeTrack + (dy > 0 ? 1 : -1) + 4) % 4;
      knobs.TRACK.angle = (state.activeTrack / 3) * 180 - 90;
      drawKnob(knobs.TRACK);
      showHint(`🔀 Switched to Track ${state.activeTrack + 1} (${state.activeTrack === 3 ? 'Drums' : (state.activeTrack === 0 ? '303 Bass' : 'Synth')})`);
    }
  });

  bindKnob(knobs.KNOB1, (dy) => {
    synthCutoff = Math.min(127, Math.max(0, synthCutoff + dy));
    knobs.KNOB1.angle = (synthCutoff / 127) * 270 - 135;
    drawKnob(knobs.KNOB1);
    showHint(`🎛️ Cutoff: ${synthCutoff}/127 (${Math.round((synthCutoff / 127) * 100)}%)`);
  });
  knobs.KNOB1.angle = (synthCutoff / 127) * 270 - 135;
  drawKnob(knobs.KNOB1);

  bindKnob(knobs.KNOB2, (dy) => {
    synthResonance = Math.min(127, Math.max(0, synthResonance + dy));
    knobs.KNOB2.angle = (synthResonance / 127) * 270 - 135;
    drawKnob(knobs.KNOB2);
    showHint(`🎛️ Resonance: ${synthResonance}/127 (${Math.round((synthResonance / 127) * 100)}%)`);
  });
  knobs.KNOB2.angle = (synthResonance / 127) * 270 - 135;
  drawKnob(knobs.KNOB2);

  bindKnob(knobs.KNOB3, (dy) => {
    synthDecay = Math.min(127, Math.max(0, synthDecay + dy));
    knobs.KNOB3.angle = (synthDecay / 127) * 270 - 135;
    drawKnob(knobs.KNOB3);
    showHint(`🎛️ Decay: ${synthDecay}/127 (${Math.round((synthDecay / 127) * 100)}%)`);
  });
  knobs.KNOB3.angle = (synthDecay / 127) * 270 - 135;
  drawKnob(knobs.KNOB3);

  bindKnob(knobs.KNOB4, (dy) => {
    synthAccent = Math.min(127, Math.max(0, synthAccent + dy));
    knobs.KNOB4.angle = (synthAccent / 127) * 270 - 135;
    drawKnob(knobs.KNOB4);
    showHint(`🎛️ Accent Intensity: ${synthAccent}/127`);
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
      info.textContent = `${state.bpm} BPM • T${state.activeTrack + 1} (${SOUND_MODES[state.soundEngine]}) • ${state.isRunning ? 'RUNNING' : 'STOPPED'}`;
    }
  }

  function bindButton(b, onClick) {
    b.hit.addEventListener("pointerdown", (e) => {
      e.preventDefault();
      b.g.classList.add("held");
    });
    b.hit.addEventListener("pointerup", (e) => {
      b.g.classList.remove("held");
    });
    b.hit.addEventListener("click", () => {
      onClick();
      updateButtonVisuals();
    });
  }

  bindButton(buttons["OCT-"], () => {
    if (state.viewOctave > 1) {
      state.viewOctave--;
      showHint(`🎹 Octave shifted down to Octave ${state.viewOctave} (C${state.viewOctave}..B${state.viewOctave})`);
    }
  });

  bindButton(buttons["OCT+"], () => {
    if (state.viewOctave < 5) {
      state.viewOctave++;
      showHint(`🎹 Octave shifted up to Octave ${state.viewOctave} (C${state.viewOctave}..B${state.viewOctave})`);
    }
  });

  bindButton(buttons.PLAY, () => {
    if (state.isRunning) {
      stopSequencer();
      showHint("⏹️ Sequencer Paused. Click PLAY to resume.");
    } else {
      startSequencer();
      showHint("▶️ Sequencer Running! Jamming all 4 synced tracks.");
    }
  });

  bindButton(buttons.REC, () => {
    state.recMode = !state.recMode;
    if (state.recMode) {
      showHint("🔴 REC MODE ACTIVE: Tap any silicone key to punch in note pitch and auto-advance to next step!");
    } else {
      showHint("REC mode off.");
    }
  });

  function randomizePattern() {
    const trk = state.tracks[state.activeTrack];
    const pat = trk.patterns[trk.curBank * 8 + trk.curPat];
    const scale = [0, 3, 5, 7, 10, 12, 15];
    for (let i = 0; i < pat.length; i++) {
      if (Math.random() < 0.25) {
        pat.steps[i].gate = 0;
      } else {
        pat.steps[i].note = 48 + scale[Math.floor(Math.random() * scale.length)];
        pat.steps[i].gate = 85 + Math.floor(Math.random() * 42);
        pat.steps[i].tie = Math.random() < 0.2 ? 1 : 0;
      }
      pat.steps[i].curve = Math.floor(Math.random() * 127);
    }
    showHint(`🎲 Random pattern generated for Track ${state.activeTrack + 1}!`);
  }

  bindButton(buttons.FX, randomizePattern);

  bindButton(buttons.SEL, () => {
    state.viewPage = state.viewPage === 0 ? 1 : 0;
    showHint(`📄 Viewing Step Page ${state.viewPage + 1} (${state.viewPage === 0 ? 'Steps 1–16' : 'Steps 17–32'})`);
  });

  bindButton(buttons.ENV, () => {
    state.viewMode = state.viewMode === 'keys' ? 'curve' : 'keys';
    showHint(state.viewMode === 'keys' ? "🎹 Display: Note Pitch Grid (KEYS)" : "📈 Display: CC Modulation Curves (CURV)");
  });

  bindButton(buttons.LFO, () => {
    state.soundEngine = (state.soundEngine + 1) % SOUND_MODES.length;
    showHint(`🔊 Sound Engine switched to: ${SOUND_MODES[state.soundEngine]}`);
  });

  bindButton(buttons.EDIT, () => {
    state.shiftMode = !state.shiftMode;
    showHint(state.shiftMode ? "⚡ Shift Active: Knobs and buttons access secondary functions." : "Shift inactive.");
  });

  bindButton(buttons.GLO, () => {
    const trk = state.tracks[state.activeTrack];
    const pat = trk.patterns[trk.curBank * 8 + trk.curPat];
    pat.length = pat.length === 16 ? 32 : (pat.length === 32 ? 24 : 16);
    showHint(`📏 Sequence Length for Track ${state.activeTrack + 1} set to ${pat.length} steps.`);
  });

  bindButton(buttons.HOME, () => {
    state.activeTrack = (state.activeTrack + 1) % NUM_TRACKS;
    showHint(`🏠 Active Track: T${state.activeTrack + 1} (${state.activeTrack === 3 ? 'Drums' : (state.activeTrack === 0 ? '303 Bass' : 'Synth')})`);
  });

  bindButton(buttons.SAVE, () => {
    try {
      localStorage.setItem('morpheus_patterns', JSON.stringify(state.tracks));
      showHint('💾 All 4 tracks and 32 patterns saved successfully to browser storage!');
    } catch (e) {
      alert('Could not save to localStorage: ' + e);
    }
  });

  bindButton(buttons.ARP, () => {
    const trk = state.tracks[state.activeTrack];
    trk.nextBank = (trk.curBank + 1) % 4;
    trk.curBank = trk.nextBank;
    showHint(`📚 Bank switched to Bank ${String.fromCharCode(65 + trk.curBank)}`);
  });

  bindButton(buttons.SEQ, () => {
    const trk = state.tracks[state.activeTrack];
    trk.nextPat = (trk.curPat + 1) % 8;
    trk.curPat = trk.nextPat;
    showHint(`🔢 Switched to Pattern ${trk.curPat + 1} in Bank ${String.fromCharCode(65 + trk.curBank)}`);
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
      e.preventDefault();
      initAudio();
      k.g.classList.add("held");

      const trk = state.tracks[state.activeTrack];
      const pat = trk.patterns[trk.curBank * 8 + trk.curPat];

      if (state.recMode) {
        pat.steps[state.activeStep].note = midiNote;
        pat.steps[state.activeStep].gate = 115;
        state.activeStep = (state.activeStep + 1) % pat.length;
        state.viewPage = state.activeStep >= 16 ? 1 : 0;
        showHint(`🔴 Step ${state.activeStep}: Recorded Note ${midiNote}`);
      } else if (idx < 16) {
        state.activeStep = (state.viewPage * 16) + idx;
      }

      play303Note(midiNote, 110, false, 64);
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
    startSequencer();
    if (statusEl) {
      statusEl.textContent = "Running. 4-Track Pattern Sequencer & 303 Acid Synth active.";
    }
    showHint("▶️ Sequencer running! Click any square on the screen to add notes, or twist K1 Cutoff.");
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
    initTracks();
    if (style === 'acid') {
      state.bpm = 128;
      synthCutoff = 80;
      synthResonance = 100;
      synthDecay = 70;
      synthAccent = 95;
      state.activeTrack = 0;
      state.soundEngine = 0; // 303
      showHint("⚡ Loaded Acid Techno Groove: 128 BPM, 303 Acid Bass, and 909 Drums.");
    } else if (style === 'electro') {
      state.bpm = 135;
      synthCutoff = 105;
      synthResonance = 80;
      synthDecay = 45;
      synthAccent = 90;
      state.activeTrack = 1;
      state.soundEngine = 1; // Sub
      showHint("🏙️ Loaded Detroit Electro Groove: 135 BPM, Subtractive Lead & punchy rhythm.");
    } else if (style === 'dub') {
      state.bpm = 114;
      synthCutoff = 40;
      synthResonance = 70;
      synthDecay = 115;
      synthAccent = 85;
      state.activeTrack = 2;
      state.soundEngine = 2; // FM
      showHint("🌌 Loaded Ambient Dub Groove: 114 BPM, 2-OP FM metallic chimes & deep bass.");
    } else if (style === 'polymetric') {
      state.bpm = 126;
      synthCutoff = 90;
      synthResonance = 95;
      synthDecay = 65;
      synthAccent = 100;
      state.activeTrack = 0;
      state.soundEngine = 0;
      state.tracks[0].patterns[0].length = 7; // 7 steps polyrhythm
      showHint("🌀 Loaded Polymetric 7/8 Groove: 7-step acid bass against 16-step drums!");
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

  const btnPastePat = document.getElementById("btnPastePat");
  if (btnPastePat) {
    btnPastePat.addEventListener("click", () => {
      if (!state.clipboard) {
        showHint("⚠️ Clipboard is empty! Click 'Copy' on a pattern first.");
        return;
      }
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
    btnClear.addEventListener("click", () => {
      const trk = state.tracks[state.activeTrack];
      const pat = trk.patterns[trk.curBank * 8 + trk.curPat];
      pat.steps.forEach(st => { st.gate = 0; st.tie = 0; });
      showHint(`🧹 Cleared all notes in Pattern ${String.fromCharCode(65 + trk.curBank)}${trk.curPat + 1}.`);
    });
  }

  // Kick off screen rendering loop
  loop();
  updateButtonVisuals();
})();
