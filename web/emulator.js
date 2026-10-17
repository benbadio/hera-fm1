/* SPDX-License-Identifier: GPL-3.0-only */
/* MORPHEUS: In-Browser M-VAVE FM-1 Interactive Emulator */

(() => {
  'use strict';

  // Constants
  const NUM_TRACKS = 4;
  const TOTAL_PATS = 32;
  const MAX_STEPS = 32;
  const NOTE_NAMES = ['B', 'A#', 'A', 'G#', 'G', 'F#', 'F', 'E', 'D#', 'D', 'C#', 'C'];
  const PIANO_IS_BLACK = [0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0];
  const SOUND_MODES = ['303', 'SUB', 'FM', 'MUTE'];

  // Global State
  const state = {
    bpm: 128,
    isRunning: false,
    activeTrack: 0,
    activeStep: 0,
    viewMode: 'keys', // 'keys' or 'curve'
    viewOctave: 3,    // C3..B3
    viewPage: 0,      // 0 = 1-16, 1 = 17-32
    recMode: false,
    soundEngine: 0,   // 0=303, 1=SUB, 2=FM, 3=MUTE
    menuMode: null,
    menuSel: 0,
    clipboard: null,
    tracks: []
  };

  // Factory Patterns initialization
  function createDefaultPattern(trackIdx) {
    const pat = {
      length: 16,
      resolution: 16, // 16th notes
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
      const notes = [48, 48, 60, 58, 55, 55, 53, 55, 51, 51, 53, 53, 48, 60, 58, 55];
      const gates = [110,  0, 127, 95, 115,  0, 100, 120, 90,  0, 105, 115, 120, 127, 95,  0];
      const ties  = [  0,  0,   1,  0,   0,  0,   0,   1,  0,  0,   0,   1,   0,   1,  0,  0];
      const curves = [ 30, 45, 120, 80,  60, 40,  75, 110, 35, 20,  70, 100, 115, 125, 80, 40];
      for (let s = 0; s < 16; s++) {
        pat.steps[s] = { note: notes[s], gate: gates[s], tie: ties[s], curve: curves[s] };
      }
    } else if (trackIdx === 1) {
      // Track 2: Lead Synth
      const notes = [60, 60, 63, 63, 65, 67, 65, 63, 60, 58, 60, 63, 67, 70, 67, 65];
      const gates = [100, 100, 110, 110, 120, 125, 115, 105, 95, 95, 110, 115, 127, 120, 110, 100];
      const ties  = [  0,   0,   0,   1,   0,   0,   1,   0,  0,  0,   0,   1,   0,   0,   1,   0];
      const curves = [ 60,  65,  70,  75,  80,  90, 100,  85, 70, 65,  75,  90, 110, 120,  95,  70];
      for (let s = 0; s < 16; s++) {
        pat.steps[s] = { note: notes[s], gate: gates[s], tie: ties[s], curve: curves[s] };
      }
    } else if (trackIdx === 2) {
      // Track 3: Arpeggiator
      const notes = [48, 55, 60, 55, 51, 55, 58, 55, 53, 56, 60, 56, 55, 58, 62, 58];
      const gates = [90, 85, 100, 85, 90, 85, 95, 85, 90, 85, 100, 85, 95, 85, 110, 85];
      const curves = [40, 50,  60, 70, 80, 90, 80, 70, 60, 50,  60, 70, 80, 90, 100, 80];
      for (let s = 0; s < 16; s++) {
        pat.steps[s] = { note: notes[s], gate: gates[s], tie: 0, curve: curves[s] };
      }
    } else if (trackIdx === 3) {
      // Track 4: Drum triggers (Kick=36, Snare=38, Hat=42, OHat=46)
      const notes = [36, 42, 38, 42, 36, 42, 38, 46, 36, 42, 38, 42, 36, 42, 38, 46];
      const gates = [127, 80, 115, 80, 120, 80, 115, 85, 127, 80, 115, 80, 120, 90, 120, 95];
      const curves = [ 20, 30,  40, 50,  60, 70,  80, 90, 100, 90,  80, 70,  60, 50,  40, 30];
      for (let s = 0; s < 16; s++) {
        pat.steps[s] = { note: notes[s], gate: gates[s], tie: 0, curve: curves[s] };
      }
    }

    return pat;
  }

  // Initialize all 4 tracks with 32 patterns each
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

    // Try load saved from localStorage
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
    const AudioContext = window.AudioContext || window.webkitAudioContext;
    audioCtx = new AudioContext({ sampleRate: 44100 });

    masterGain = audioCtx.createGain();
    masterGain.gain.setValueAtTime(0.7, audioCtx.currentTime);
    masterGain.connect(audioCtx.destination);
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

    // Filter calculations
    const cutoffBase = (synthCutoff / 127) * 4500 + 100;
    const modOffset = (curveVal - 64) * 20;
    const accentBoost = isAccent ? ((synthAccent / 127) * 4000 + 1000) : 1800;
    const peakFreq = Math.min(12000, Math.max(150, cutoffBase + accentBoost + modOffset));
    const filterQ = (synthResonance / 127) * 18 + 1; // 1..19 Q
    const decayTime = 0.06 + (synthDecay / 127) * 0.40;

    if (isTied && activeVoice) {
      // ACID SLIDE: smoothly glide pitch without re-triggering attack
      activeVoice.osc.frequency.cancelScheduledValues(now);
      activeVoice.osc.frequency.linearRampToValueAtTime(freq, now + 0.06);

      activeVoice.filter.frequency.cancelScheduledValues(now);
      activeVoice.filter.frequency.linearRampToValueAtTime(peakFreq, now + 0.05);
      activeVoice.filter.frequency.exponentialRampToValueAtTime(cutoffBase, now + decayTime);
      return;
    }

    // Stop previous voice
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
      // 303 Sawtooth
      osc.type = 'sawtooth';
    } else if (state.soundEngine === 1) {
      // Subtractive Square
      osc.type = 'square';
    } else {
      // FM-ish triangle
      osc.type = 'triangle';
    }

    osc.frequency.setValueAtTime(freq, now);

    // Resonant Filter
    filter.type = 'lowpass';
    filter.Q.setValueAtTime(filterQ, now);
    filter.frequency.setValueAtTime(peakFreq, now);
    filter.frequency.exponentialRampToValueAtTime(Math.max(80, cutoffBase), now + decayTime);

    // VCA Envelope
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

  // Drum Synthesizer for Track 4
  function playDrumNote(note, velocity) {
    if (!audioCtx || state.soundEngine === 3) return;
    const now = audioCtx.currentTime;

    if (note === 36) {
      // 909 Kick
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
      // Snare
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
      // Hi-Hat
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

  // ===========================================================================
  // Sequencer Engine Step Scheduler
  // ===========================================================================
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
          // Play secondary synth note
          play303Note(step.note, step.gate * 0.7, step.tie, step.curve);
        }
      }

      // Step advancement
      trk.curStep = (trk.curStep + 1) % pat.length;

      // Downbeat pattern jump
      if (trk.curStep === 0) {
        if (trk.nextPat !== trk.curPat || trk.nextBank !== trk.curBank) {
          trk.curPat = trk.nextPat;
          trk.curBank = trk.nextBank;
        }
      }
    }

    // Schedule next 16th step based on BPM
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
    updateUIButtons();
  }

  function stopSequencer() {
    state.isRunning = false;
    clearTimeout(stepTimer);
    updateUIButtons();
  }

  // ===========================================================================
  // 240x240 Canvas Renderer (Faithful to matrix_ui.c)
  // ===========================================================================
  const canvas = document.getElementById('matrixCanvas');
  const ctx = canvas.getContext('2d');
  ctx.imageSmoothingEnabled = false;

  function renderScreen() {
    ctx.fillStyle = '#080a0c';
    ctx.fillRect(0, 0, 240, 240);

    const trkIdx = state.activeTrack;
    const trk = state.tracks[trkIdx];
    const pat = trk.patterns[trk.curBank * 8 + trk.curPat];
    const stepOffset = state.viewPage * 16;
    const curPlayStep = trk.curStep;

    // --- 1. Top Bar (y: 0..25) ---
    ctx.fillStyle = '#14181c';
    ctx.fillRect(0, 0, 240, 25);
    ctx.fillStyle = '#222830';
    ctx.fillRect(0, 25, 240, 1);

    // Track Badge: [T1]
    ctx.fillStyle = '#ff3700';
    ctx.fillRect(4, 4, 20, 17);
    ctx.fillStyle = '#0a0c0e';
    ctx.font = 'bold 10px monospace';
    ctx.textAlign = 'center';
    ctx.fillText(`T${trkIdx + 1}`, 14, 16);

    // Track mini indicators
    for (let t = 0; t < 4; t++) {
      ctx.fillStyle = (t === trkIdx) ? '#ff3700' : '#444d56';
      ctx.fillRect(28 + t * 6, 10, 4, 5);
    }

    // Pattern Badge: A1
    ctx.fillStyle = '#222830';
    ctx.fillRect(54, 4, 18, 17);
    ctx.fillStyle = '#e0e6ed';
    ctx.font = 'bold 9px monospace';
    ctx.fillText(`${String.fromCharCode(65 + trk.curBank)}${trk.curPat + 1}`, 63, 16);

    // Sound Badge
    const snd = SOUND_MODES[state.soundEngine];
    ctx.fillStyle = snd === 'MUTE' ? '#555555' : '#8833cc';
    ctx.fillRect(76, 5, 34, 15);
    ctx.fillStyle = '#ffffff';
    ctx.font = 'bold 8px monospace';
    ctx.fillText(snd, 93, 15);

    // Mode Badge
    ctx.fillStyle = state.viewMode === 'keys' ? '#00cc88' : '#3399ff';
    ctx.fillRect(114, 5, 42, 15);
    ctx.fillStyle = '#0a0c0e';
    ctx.fillText(state.viewMode === 'keys' ? 'KEYS' : 'CURV', 135, 15);

    // MIDI CH
    ctx.fillStyle = '#808b96';
    ctx.font = '8px monospace';
    ctx.fillText(`CH${pat.midiCh < 10 ? '0' + pat.midiCh : pat.midiCh}`, 174, 15);

    // Run / Stop
    ctx.fillStyle = state.isRunning ? '#00e676' : '#607080';
    ctx.beginPath();
    ctx.arc(205, 12, 3.5, 0, Math.PI * 2);
    ctx.fill();
    ctx.font = 'bold 8px monospace';
    ctx.textAlign = 'left';
    ctx.fillText(state.isRunning ? 'RUN' : 'STOP', 212, 15);

    // --- 2. Upper Matrix Note Grid (y: 26..148) ---
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

    // Grid Lines
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
        ctx.fillRect(gridX + sIdx * colW + 1, 27, colW - 1, 118);
        continue;
      }

      const st = pat.steps[actualStep];
      const isPlay = state.isRunning && actualStep === curPlayStep;
      const isCursor = actualStep === state.activeStep;

      if (isCursor) {
        ctx.fillStyle = '#ffffff';
        ctx.fillRect(gridX + sIdx * colW, 26, colW, 1);
        ctx.fillRect(gridX + sIdx * colW, 145, colW, 1);
      }

      if (isPlay) {
        ctx.fillStyle = '#ff3700';
        ctx.fillRect(gridX + sIdx * colW + 1, 26, colW - 2, 2);
      }

      if (state.viewMode === 'keys') {
        if (st.gate > 0) {
          const noteOct = Math.floor(st.note / 12) - 1;
          const noteInOct = st.note % 12;
          if (noteOct === state.viewOctave) {
            const noteRow = 11 - noteInOct;
            const cellY = 26 + noteRow * rowH + 1;
            ctx.fillStyle = isPlay ? '#ffeedd' : '#ff3700';
            ctx.shadowColor = isPlay ? '#ffffff' : '#ff4400';
            ctx.shadowBlur = isPlay ? 6 : 2;
            ctx.fillRect(gridX + sIdx * colW + 2, cellY + 1, colW - 3, rowH - 2);
            ctx.shadowBlur = 0;
          } else if (noteOct > state.viewOctave) {
            ctx.fillStyle = '#ff7700';
            ctx.fillRect(gridX + sIdx * colW + 4, 27, colW - 7, 3);
          } else {
            ctx.fillStyle = '#ff7700';
            ctx.fillRect(gridX + sIdx * colW + 4, 142, colW - 7, 3);
          }
        }
      } else {
        // Curve mode
        const barH = (st.curve / 127) * 116;
        const barY = 146 - barH;
        ctx.fillStyle = isPlay ? '#ffeedd' : '#0088ff';
        ctx.shadowColor = '#00aaff';
        ctx.shadowBlur = isPlay ? 6 : 2;
        ctx.fillRect(gridX + sIdx * colW + 2, barY, colW - 4, barH);
        ctx.shadowBlur = 0;
      }
    }

    // --- 3. Lower Gate / Velocity Strips (y: 148..208) ---
    ctx.fillStyle = '#101317';
    ctx.fillRect(0, 148, 240, 60);
    ctx.fillStyle = '#222830';
    ctx.fillRect(0, 148, 240, 1);

    ctx.fillStyle = '#14181c';
    ctx.fillRect(0, 149, gridX, 59);
    ctx.fillRect(gridX - 1, 149, 1, 59);
    ctx.fillStyle = '#808b96';
    ctx.font = 'bold 8px monospace';
    ctx.textAlign = 'center';
    ctx.fillText('GATE', 11, 172);
    ctx.fillText('VEL', 11, 186);

    for (let sIdx = 0; sIdx < 16; sIdx++) {
      const actualStep = stepOffset + sIdx;
      if (actualStep >= pat.length) continue;

      const st = pat.steps[actualStep];
      const isPlay = state.isRunning && actualStep === curPlayStep;
      const barX = gridX + sIdx * colW + 2;

      if (st.gate > 0) {
        const barH = Math.max(4, (st.gate / 127) * 50);
        const barY = 206 - barH;
        ctx.fillStyle = isPlay ? '#ffeedd' : '#e64000';
        ctx.fillRect(barX, barY, colW - 4, barH);

        if (st.tie && sIdx < 15) {
          ctx.fillStyle = '#ff7700';
          ctx.fillRect(barX + (colW - 4), barY + barH - 4, 4, 4);
        }
      } else {
        ctx.fillStyle = '#222830';
        ctx.fillRect(barX + 2, 202, colW - 8, 2);
      }
    }

    // --- 4. Bottom Status Bar (y: 208..240) ---
    ctx.fillStyle = '#14181c';
    ctx.fillRect(0, 208, 240, 32);
    ctx.fillStyle = '#222830';
    ctx.fillRect(0, 208, 240, 1);

    ctx.font = '8px monospace';
    ctx.textAlign = 'left';
    ctx.fillStyle = '#ff7700';
    ctx.fillText(`P${state.viewPage + 1} OCT:${state.viewOctave}`, 6, 226);

    ctx.fillStyle = '#e0e6ed';
    ctx.fillText(`${state.bpm}BPM`, 82, 226);

    ctx.fillStyle = '#808b96';
    ctx.fillText('K1-4:EDIT', 148, 226);
  }

  function loop() {
    renderScreen();
    requestAnimationFrame(loop);
  }

  // ===========================================================================
  // Physical Panel & Keyboard Event Handlers
  // ===========================================================================
  const overlay = document.getElementById('audioOverlay');
  const btnStart = document.getElementById('btnStart');

  btnStart.addEventListener('click', () => {
    initAudio();
    overlay.style.display = 'none';
    startSequencer();
  });

  // Buttons
  const btnPlay = document.getElementById('btnPlay');
  const btnRec = document.getElementById('btnRec');
  const btnTrack = document.getElementById('btnTrack');
  const btnMode = document.getElementById('btnMode');
  const btnSound = document.getElementById('btnSound');
  const btnOctDown = document.getElementById('btnOctDown');
  const btnOctUp = document.getElementById('btnOctUp');
  const btnPage = document.getElementById('btnPage');
  const btnSave = document.getElementById('btnSave');
  const btnFX = document.getElementById('btnFX');

  function updateUIButtons() {
    if (btnPlay) {
      btnPlay.classList.toggle('lit-green', state.isRunning);
    }
    if (btnRec) {
      btnRec.classList.toggle('lit', state.recMode);
    }
  }

  btnPlay.addEventListener('click', () => {
    if (state.isRunning) {
      stopSequencer();
    } else {
      startSequencer();
    }
  });

  btnRec.addEventListener('click', () => {
    state.recMode = !state.recMode;
    updateUIButtons();
  });

  btnTrack.addEventListener('click', () => {
    state.activeTrack = (state.activeTrack + 1) % NUM_TRACKS;
  });

  btnMode.addEventListener('click', () => {
    state.viewMode = (state.viewMode === 'keys') ? 'curve' : 'keys';
  });

  btnSound.addEventListener('click', () => {
    state.soundEngine = (state.soundEngine + 1) % SOUND_MODES.length;
  });

  btnOctDown.addEventListener('click', () => {
    if (state.viewOctave > 1) state.viewOctave--;
  });

  btnOctUp.addEventListener('click', () => {
    if (state.viewOctave < 5) state.viewOctave++;
  });

  btnPage.addEventListener('click', () => {
    state.viewPage = state.viewPage === 0 ? 1 : 0;
  });

  btnSave.addEventListener('click', () => {
    try {
      localStorage.setItem('morpheus_patterns', JSON.stringify(state.tracks));
      alert('MORPHEUS: Patterns and settings saved successfully to browser storage!');
    } catch (e) {
      alert('Could not save to localStorage: ' + e);
    }
  });

  btnFX.addEventListener('click', () => {
    // Quick randomizer demo
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
  });

  // Knobs: drag up/down
  function setupKnob(el, onChange) {
    let startY = 0;
    let dragging = false;

    el.addEventListener('mousedown', (e) => {
      startY = e.clientY;
      dragging = true;
      e.preventDefault();
    });

    window.addEventListener('mousemove', (e) => {
      if (!dragging) return;
      const dy = startY - e.clientY;
      startY = e.clientY;
      onChange(dy);
    });

    window.addEventListener('mouseup', () => {
      dragging = false;
    });
  }

  const btnBank = document.getElementById('btnBank');
  if (btnBank) {
    btnBank.addEventListener('click', () => {
      const trk = state.tracks[state.activeTrack];
      trk.nextBank = (trk.curBank + 1) % 4;
      trk.curBank = trk.nextBank;
    });
  }

  const btnPat = document.getElementById('btnPat');
  if (btnPat) {
    btnPat.addEventListener('click', () => {
      const trk = state.tracks[state.activeTrack];
      trk.nextPat = (trk.curPat + 1) % 8;
      trk.curPat = trk.nextPat;
    });
  }

  const btnMute = document.getElementById('btnMute');
  if (btnMute) {
    btnMute.addEventListener('click', () => {
      const trk = state.tracks[state.activeTrack];
      const pat = trk.patterns[trk.curBank * 8 + trk.curPat];
      pat.mute = !pat.mute;
      btnMute.classList.toggle('lit', pat.mute);
    });
  }

  const btnShift = document.getElementById('btnShift');
  if (btnShift) {
    btnShift.addEventListener('click', () => {
      state.shiftMode = !state.shiftMode;
      btnShift.classList.toggle('lit', state.shiftMode);
    });
  }

  const kCutoff = document.getElementById('knobCutoff');
  const pCutoff = document.getElementById('ptrCutoff');
  if (kCutoff && pCutoff) {
    pCutoff.style.transform = `rotate(${(synthCutoff / 127) * 270 - 135}deg)`;
    setupKnob(kCutoff, (dy) => {
      synthCutoff = Math.min(127, Math.max(0, synthCutoff + dy));
      const deg = (synthCutoff / 127) * 270 - 135;
      pCutoff.style.transform = `rotate(${deg}deg)`;
    });
  }

  const kRes = document.getElementById('knobRes');
  const pRes = document.getElementById('ptrRes');
  if (kRes && pRes) {
    pRes.style.transform = `rotate(${(synthResonance / 127) * 270 - 135}deg)`;
    setupKnob(kRes, (dy) => {
      synthResonance = Math.min(127, Math.max(0, synthResonance + dy));
      const deg = (synthResonance / 127) * 270 - 135;
      pRes.style.transform = `rotate(${deg}deg)`;
    });
  }

  const kDecay = document.getElementById('knobDecay');
  const pDecay = document.getElementById('ptrDecay');
  if (kDecay && pDecay) {
    pDecay.style.transform = `rotate(${(synthDecay / 127) * 270 - 135}deg)`;
    setupKnob(kDecay, (dy) => {
      synthDecay = Math.min(127, Math.max(0, synthDecay + dy));
      const deg = (synthDecay / 127) * 270 - 135;
      pDecay.style.transform = `rotate(${deg}deg)`;
    });
  }

  const kAccent = document.getElementById('knobAccent');
  const pAccent = document.getElementById('ptrAccent');
  if (kAccent && pAccent) {
    pAccent.style.transform = `rotate(${(synthAccent / 127) * 270 - 135}deg)`;
    setupKnob(kAccent, (dy) => {
      synthAccent = Math.min(127, Math.max(0, synthAccent + dy));
      const deg = (synthAccent / 127) * 270 - 135;
      pAccent.style.transform = `rotate(${deg}deg)`;
    });
  }

  const kBpm = document.getElementById('knobBpm');
  const pBpm = document.getElementById('ptrBpm');
  if (kBpm && pBpm) {
    pBpm.style.transform = `rotate(${((state.bpm - 50) / 190) * 270 - 135}deg)`;
    setupKnob(kBpm, (dy) => {
      state.bpm = Math.min(240, Math.max(50, state.bpm + dy));
      const deg = ((state.bpm - 50) / 190) * 270 - 135;
      pBpm.style.transform = `rotate(${deg}deg)`;
    });
  }

  const kStep = document.getElementById('knobStep');
  const pStep = document.getElementById('ptrStep');
  if (kStep && pStep) {
    pStep.style.transform = `rotate(${(state.activeStep / 31) * 270 - 135}deg)`;
    setupKnob(kStep, (dy) => {
      state.activeStep = (state.activeStep + (dy > 0 ? 1 : -1) + 32) % 32;
      state.viewPage = state.activeStep >= 16 ? 1 : 0;
      const deg = (state.activeStep / 31) * 270 - 135;
      pStep.style.transform = `rotate(${deg}deg)`;
    });
  }

  // 27 Silicone Keys
  const keysContainer = document.getElementById('siliconeKeys');
  const keyNotes = [
    53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65,
    66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79
  ];

  keyNotes.forEach((midiNote, idx) => {
    const isBlack = [1, 3, 6, 8, 10, 13, 15, 18, 20, 22, 25].includes(idx);
    const keyEl = document.createElement('div');
    keyEl.className = `key-silicone ${isBlack ? 'black' : 'white'}`;
    keyEl.innerHTML = `<div class="key-led"></div><span>${idx + 1}</span>`;

    keyEl.addEventListener('mousedown', () => {
      initAudio();
      keyEl.classList.add('pressed');

      // Record or audition
      const trk = state.tracks[state.activeTrack];
      const pat = trk.patterns[trk.curBank * 8 + trk.curPat];

      if (state.recMode) {
        pat.steps[state.activeStep].note = midiNote;
        pat.steps[state.activeStep].gate = 115;
        state.activeStep = (state.activeStep + 1) % pat.length;
        state.viewPage = state.activeStep >= 16 ? 1 : 0;
      } else if (idx < 16) {
        // Direct step jump on current page
        state.activeStep = (state.viewPage * 16) + idx;
      }

      play303Note(midiNote, 110, false, 64);
    });

    window.addEventListener('mouseup', () => {
      keyEl.classList.remove('pressed');
    });

    keysContainer.appendChild(keyEl);
  });

  loop();
})();
