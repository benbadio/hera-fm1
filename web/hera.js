// SPDX-License-Identifier: GPL-3.0-only
// HERA Workbench: the real firmware (hera.wasm) on a virtual FM-1, plus an editor and a guided tour.
import { ID } from './hera_ids.js?v=09783e1';
import { startTour, startTourChapter, tourTick } from './tour.js?v=09783e1';

const $ = (s) => document.querySelector(s);
const NOTE = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
const noteName = (n) => NOTE[n % 12] + (Math.floor(n / 12) - 1);

// ---- the firmware ---------------------------------------------------------------------------------
const wasm = await WebAssembly.instantiate(await (await fetch('hera.wasm?v=09783e1')).arrayBuffer(), {});
const E = wasm.instance.exports;
const u8 = () => new Uint8Array(E.memory.buffer);
const cstr = (p) => { const m = u8(); let s = ''; while (m[p]) s += String.fromCharCode(m[p++]); return s; };
let clock0 = performance.now();
const nowMs = () => Math.floor(performance.now() - clock0) + 1;
E.web_init();

export const H = {
  E, ID, cstr,
  get: (id) => E.j6_engine_get(id),
  set: (id, v) => E.j6_engine_set(id, v),
  cmd: (c, a = 0) => E.j6_engine_cmd(c, a),
  status() {
    const p = E.web_status(), m = u8();
    const dv = new DataView(E.memory.buffer);
    return {
      seq: m[p + ID.ST_SEQ], rec: m[p + ID.ST_REC], pattern: m[p + ID.ST_PATTERN], queued: m[p + ID.ST_QUEUED],
      stepNow: m[p + ID.ST_STEP_NOW], sel: m[p + ID.ST_SEL], voices: m[p + ID.ST_VOICES], hold: m[p + ID.ST_HOLD],
      chord: m[p + ID.ST_CHORD], styleOn: m[p + ID.ST_STYLE_ON], lastNote: m[p + ID.ST_LAST_NOTE],
      lastChord: m[p + ID.ST_LAST_CHORD], tempo10: dv.getUint16(p + ID.ST_TEMPO, true),
      dirty0: dv.getUint32(p + ID.ST_DIRTY, true), dirty1: dv.getUint32(p + ID.ST_DIRTY + 4, true),
      page: E.web_page(),
    };
  },
  pattern(idx) {
    const p = E.j6_engine_pattern(idx), m = u8(), dv = new DataView(E.memory.buffer);
    const steps = [];
    for (let s = 0; s < ID.J6_STEPS; s++) {
      const o = p + ID.PAT_STEP_OFS + s * ID.STEP_SIZE;
      steps.push({ notes: [m[o], m[o + 1], m[o + 2], m[o + 3]].filter((n) => n !== ID.J6_NO_NOTE), flags: m[o + 4], phrase: m[o + 5], chord: m[o + 6] });
    }
    return { tempo10: dv.getUint16(p + ID.PAT_TEMPO, true), last: m[p + ID.PAT_LAST], phrase: m[p + ID.PAT_PHRASE], sound: m[p + ID.PAT_SOUND], steps };
  },
  chordName: (set, k) => cstr(E.j6_chord_name(set, k)),
  chordNotes(set, k) {
    const buf = E.web_scratch(), n = E.j6_chord_raw(set, k, buf);
    return Array.from(u8().slice(buf, buf + n));
  },
  genre: (set) => cstr(E.j6_chord_genre(set)),
  styleName: (s) => cstr(E.web_style_name(s)),
  varName: (s, v) => cstr(E.web_var_name(s, v)),
  soundName: (i) => cstr(E.web_sound_name(i)),
  button(id, down) { E.web_button(id, down ? 1 : 0); },
  enc(id, d) { E.web_enc(id, d); },
  keysDown: 0,
  setKey(k, down) { this.keysDown = down ? this.keysDown | (1 << k) : this.keysDown & ~(1 << k); E.web_keys(this.keysDown >>> 0); },
  midiIn(st, d1, d2) { E.web_midi_in(st, d1, d2); },
};
window.HERA = H;   // for the console

// ---- audio: the firmware renders 1024 frames per callback at 44.1 kHz --------------------------------
let ac = null, masterGain = 0.7, muted = false;
function startAudio() {
  if (ac) { ac.resume(); return; }
  ac = new (window.AudioContext || window.webkitAudioContext)({ sampleRate: 44100, latencyHint: 'interactive' });
  const sp = ac.createScriptProcessor(1024, 0, 2);
  sp.onaudioprocess = (ev) => {
    E.web_audio(1024);
    const a = new Int32Array(E.memory.buffer, E.web_audio_buf(), 2048);
    const L = ev.outputBuffer.getChannelData(0), R = ev.outputBuffer.getChannelData(1), g = (muted ? 0 : masterGain) / 32768;
    for (let i = 0; i < 1024; i++) { L[i] = a[2 * i] * g; R[i] = a[2 * i + 1] * g; }
  };
  sp.connect(ac.destination);
  $('#start').hidden = true;
  $('#devStatus').textContent = ac.sampleRate === 44100 ? 'Audio on (44.1 kHz)' : `Audio on (${ac.sampleRate} Hz: pitch is shifted)`;
}
$('#btnStart').onclick = startAudio;
$('#muteBtn').onclick = (e) => { muted = !muted; e.currentTarget.setAttribute('aria-pressed', muted); };
$('#fnToggle').onchange = (e) => { $('#device').classList.toggle('nofn', !e.target.checked); };
document.addEventListener('pointerdown', () => { if (!ac) startAudio(); }, { once: true });

// ---- the panel: the FM-1 faceplate, in the real layout (1000 x 640 units) ---------------------------
const dev = $('#device');
const place = (e, x, y, w, h) => { Object.assign(e.style, { left: x / 10 + '%', top: y / 6.4 + '%', width: w / 10 + '%', height: h / 6.4 + '%' }); return e; };
const add = (cls, x, y, w, h, html = '') => { const d = document.createElement('div'); d.className = cls; d.innerHTML = html; dev.appendChild(place(d, x, y, w, h)); return d; };
const hint = $('#hintText'), HINT0 = 'Hover or touch any knob, button, key or the screen to see what it does. Click “Switch on” to hear it.';
hint.textContent = HINT0;
const hintOn = (e, text) => { e.addEventListener('pointerenter', () => { hint.textContent = text; }); e.addEventListener('pointerleave', () => { hint.textContent = HINT0; }); };

add('tray', 300, 44, 236, 236).classList.replace('tray', 'bezel');
const scr = add('screen-box', 318, 62, 200, 200, '<canvas id="screen" width="240" height="240"></canvas>');
hintOn(scr, 'The 240 × 240 screen: exactly what the FM-1 draws. Everything is shown in words: chord names, the chord map, steps, sounds and settings.');

// knobs: [hardware label, HERA function, x, y, r, encoder id (null = MASTER), hint]
const KN = [
  ['MASTER', 'Volume', 105, 100, 24, null, 'MASTER: the output volume.'],
  ['SELECT', 'Style', 215, 100, 24, 0, 'SELECT: the phrase style, 1–9 (arpeggios, rhythms, chords, strums). Works on every page.'],
  ['PRESETS', 'Tempo / browse', 105, 205, 24, 6, 'PRESETS: the tempo. On a page that lists things (chord sets, sounds, patterns, settings, steps) it moves the cursor. GLO: fine tempo, or jump a bank.'],
  ['ALGORITHM', 'Variation', 215, 205, 24, 1, 'ALGORITHM: the phrase variation, 1–12.'],
  ['K1', 'Filter', 610, 100, 22, 2, 'K1: filter cutoff. With GLO: resonance.'],
  ['K2', 'Envelope', 714, 100, 22, 3, 'K2: envelope release. With GLO: attack.'],
  ['K3', 'Delay', 818, 100, 22, 4, 'K3: delay level. With GLO: delay time.'],
  ['K4', 'Reverb', 922, 100, 22, 5, 'K4: reverb level. With GLO: reverb time.'],
];
const encRot = [0, 0, 0, 0, 0, 0, 0];
for (const [name, fn, x, y, r, id, tip] of KN) {
  add('hw', x - 50, y - r - 24, 100, 18, name);
  const k = add('knob', x - r, y - r, 2 * r, 2 * r);
  const own = id === null ? 'master' : 'enc' + id;
  k.id = own;
  add('fn', x - 50, y + r + 12, 100, 14, fn);
  hintOn(k, tip);
  let y0 = null, acc = 0;
  const turn = (n) => {
    if (!n) return;
    if (id === null) { masterGain = Math.max(0, Math.min(1, masterGain + n * 0.03)); k.style.setProperty('--rot', (masterGain * 270 - 135) + 'deg'); return; }
    H.enc(id, n); encRot[id] += n * 15; k.style.setProperty('--rot', encRot[id] + 'deg');
  };
  if (id === null) k.style.setProperty('--rot', (masterGain * 270 - 135) + 'deg');
  k.addEventListener('pointerdown', (e) => { y0 = e.clientY; acc = 0; k.setPointerCapture(e.pointerId); });
  k.addEventListener('pointermove', (e) => { if (y0 === null) return; acc += y0 - e.clientY; y0 = e.clientY; const n = Math.trunc(acc / 8); acc -= n * 8; turn(n); });
  k.addEventListener('pointerup', () => { y0 = null; });
  k.addEventListener('wheel', (e) => { e.preventDefault(); turn(e.deltaY < 0 ? 1 : -1); }, { passive: false });
}

// buttons: [id, hardware label, HERA function, x, y, w, h, hint]
const capEl = [];
const BT = [
  [0, 'OCT-', 'Octave −', 72, 268, 82, 38, 'OCT −: octave down. With GLO: transpose down. On the sequencer page: the previous page of steps.'],
  [1, 'OCT+', 'Octave +', 170, 268, 82, 38, 'OCT +: octave up. With GLO: transpose up. On the sequencer page: the next page of steps.'],
];
const ROW1 = [[2, 'FX', 'Effects', 'FX: the effects page (delay and reverb).'], [3, 'SEL', 'Chord', 'SEL: tap = chord mode on / off. Hold = the chord map (which chord is on which key).'],
  [4, 'ENV', 'Hold', 'ENV: HOLD, which keeps notes sounding. On a selected step it ties the step.'], [5, 'LFO', 'Sounds', 'LFO: the sounds page. Browse with PRESETS; the keys keep playing so you hear each one.'],
  [6, 'EDIT', 'Settings', 'EDIT: the settings page. Press again to leave it (and save them).'], [7, 'GLO', 'Shift', 'GLO: the shift button (Shift on your computer keyboard). Hold it while you press or turn another control.']];
const ROW2 = [[8, 'HOME', 'Play page', 'HOME: the play page.'], [9, 'SAVE', 'Save', 'SAVE: save this pattern. With GLO: save all patterns.'],
  [10, 'ARP', 'Phrase', 'ARP: tap = phrase on / off. Hold = the phrase page.'], [11, 'SEQ', 'Sequencer', 'SEQ: the sequencer page; again: the patterns page; with a step selected: finish editing.'],
  [12, 'PLAY', '', 'PLAY / STOP: tap = play or stop (Space). Hold = full stop, silences everything. GLO + PLAY: play from the selected step.'], [13, 'REC', 'Record', 'REC: record in real time.']];
ROW1.forEach(([id, l, f, t], i) => BT.push([id, l, f, 582 + i * 63, 174, 54, 48, t]));
ROW2.forEach(([id, l, f, t], i) => BT.push([id, l, f, 582 + i * 63, 236, 54, 48, t]));
add('tray', 58, 258, 210, 58);
add('tray', 568, 160, 400, 140);
for (const [id, label, fn, x, y, w, h, tip] of BT) {
  const box = add('btnbox', x, y, w, h, `<div class="cap${id === 12 ? ' two' : ''}"><span class="hw">${id === 12 ? 'PLAY<br>STOP' : label}</span>${fn ? `<span class="fn">${fn}</span>` : ''}</div>`);
  box.id = 'btn' + id;
  const c = box.querySelector('.cap');
  capEl[id] = c;
  hintOn(c, tip);
  const up = () => { if (c.classList.contains('down')) { c.classList.remove('down'); H.button(id, 0); } };
  c.addEventListener('pointerdown', (e) => { c.classList.add('down'); H.button(id, 1); c.setPointerCapture(e.pointerId); });
  c.addEventListener('pointerup', up);
  c.addEventListener('pointercancel', up);
}

// the 27 keys, F3 .. G5: 16 white pills and 11 black ones, as on the FM-1
const keyEl = [];
const WHITE_K = [0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26], BLACK_K = [1, 3, 5, 8, 10, 13, 15, 17, 20, 22, 25], BLACK_AFTER = [0, 1, 2, 4, 5, 7, 8, 9, 11, 12, 14];
const bed = add('tray', 36, 340, 928, 270);
bed.id = 'keyboard';
(function keyboard() {
  const x0 = 36, y0 = 340, w = 928, h = 270, pitch = (w - 24) / 16, kw = pitch * 0.78;
  const mk = (k, black, x, y, kh) => {
    const n = 53 + k, d = add('key ' + (black ? 'b' : 'w'), x, y, kw, kh, `<div class="kl">${n % 12 === 0 ? noteName(n) : ''}</div>`);
    d.dataset.k = k;
    const up = () => { d.classList.remove('down'); H.setKey(k, false); };
    d.addEventListener('pointerdown', (e) => { d.classList.add('down'); H.setKey(k, true); d.setPointerCapture(e.pointerId); });
    d.addEventListener('pointerup', up);
    d.addEventListener('pointercancel', up);
    hintOn(d, 'Keys: notes, or whole chords with chord mode on. On the sequencer page they enter notes into the step chosen with PRESETS; the lights under the white keys show the 16 steps.');
    keyEl[k] = d;
  };
  WHITE_K.forEach((k, i) => mk(k, false, x0 + 12 + i * pitch + (pitch - kw) / 2, y0 + h * 0.43, h * 0.52));
  BLACK_AFTER.forEach((a, i) => mk(BLACK_K[i], true, x0 + 12 + (a + 1) * pitch - kw / 2, y0 + h * 0.06, h * 0.33));
})();

// computer keyboard
const KMAP = { z: 7, s: 8, x: 9, d: 10, c: 11, v: 12, g: 13, b: 14, h: 15, n: 16, j: 17, m: 18, ',': 19, q: 19, 2: 20, w: 21, 3: 22, e: 23, r: 24, 5: 25, t: 26 };
const typing = (e) => ['INPUT', 'SELECT', 'TEXTAREA'].includes(e.target.tagName);
window.addEventListener('keydown', (e) => {
  if (typing(e) || e.repeat) return;
  const k = KMAP[e.key.toLowerCase()];
  if (k !== undefined) { H.setKey(k, true); keyEl[k].classList.add('down'); }
  else if (e.code === 'Space') { e.preventDefault(); H.button(12, 1); }
  else if (e.key === 'Shift') H.button(7, 1);
});
window.addEventListener('keyup', (e) => {
  if (typing(e)) return;
  const k = KMAP[e.key.toLowerCase()];
  if (k !== undefined) { H.setKey(k, false); keyEl[k].classList.remove('down'); }
  else if (e.code === 'Space') H.button(12, 0);
  else if (e.key === 'Shift') H.button(7, 0);
});

// ---- the main loop: the firmware's loop, the screen and the LEDs --------------------------------------
const cv = $('#screen'), ctx = cv.getContext('2d'), img = ctx.createImageData(240, 240);
function drawScreen() {
  const fb = new Uint16Array(E.memory.buffer, E.web_fb(), 240 * 240), d = img.data;
  for (let i = 0; i < 240 * 240; i++) {
    const c = fb[i];
    d[4 * i] = ((c >> 11) & 31) * 255 / 31; d[4 * i + 1] = ((c >> 5) & 63) * 255 / 63; d[4 * i + 2] = (c & 31) * 255 / 31; d[4 * i + 3] = 255;
  }
  ctx.putImageData(img, 0, 0);
}
function drawLeds() {
  const l = new Uint8Array(E.memory.buffer, E.web_leds(), 41);
  for (let id = 0; id < 14; id++) if (capEl[id]) capEl[id].classList.toggle('lit', !!l[id]);
  for (let k = 0; k < 27; k++) keyEl[k].classList.toggle('lit', !!l[14 + k]);
}
let frame = 0, silentT = performance.now(), silentOwed = 0;
function tick() {                                // the firmware's main loop: on a timer (runs in background tabs too)
  E.web_step(nowMs());
  const t = performance.now();                   // before audio starts, keep the engine's time running silently
  if (!ac || ac.state !== 'running') {
    silentOwed = Math.min(silentOwed + (t - silentT) * 44.1, 8192);
    while (silentOwed >= 256) { E.web_audio(256); silentOwed -= 256; }
  }
  silentT = t;
  while (true) {
    const m = E.web_midi_out();
    if (!m) break;
    midiSend(m & 255, (m >> 8) & 127, (m >> 16) & 127);
  }
  tourTick();
  if ((frame++ & 15) === 0) refreshEditor();
}
function draw() {                                // the screen and the LEDs: once a frame
  drawScreen();
  drawLeds();
  requestAnimationFrame(draw);
}

// ---- the editor -----------------------------------------------------------------------------------
let pane = 'sounds';
document.querySelectorAll('.tab').forEach((t) => t.addEventListener('click', () => {
  document.querySelectorAll('.tab').forEach((x) => x.classList.toggle('on', x === t));
  pane = t.dataset.pane;
  document.querySelectorAll('.pane').forEach((p) => p.classList.toggle('on', p.id === 'pane-' + pane));
  refreshEditor(true);
}));
const el = (tag, cls, html) => { const d = document.createElement(tag); if (cls) d.className = cls; if (html !== undefined) d.innerHTML = html; return d; };
const patName = (p) => String.fromCharCode(65 + (p >> 3)) + ((p & 7) + 1);

// sounds
const KNOB_NAMES = ['Cutoff', 'Resonance', 'Release', 'Attack', 'Delay level', 'Delay time', 'Reverb level', 'Reverb time'];
let soundBank = -1;
for (let b = 0; b < 8; b++) {
  const t = el('button', 'btn small', String.fromCharCode(65 + b));
  t.onclick = () => { soundBank = b; refreshEditor(true); };
  $('#bankTabs').appendChild(t);
}
const knobInputs = KNOB_NAMES.map((n, i) => {
  const w = el('div'), lab = el('label', '', n), inp = el('input');
  inp.type = 'range'; inp.min = 0; inp.max = 127;
  inp.oninput = () => H.set(ID.J6_P_KNOB0 + i, +inp.value);
  w.append(lab, inp);
  $('#knobs8').appendChild(w);
  return inp;
});
function refreshSounds(full) {
  const cur = H.get(ID.J6_P_SOUND);
  if (soundBank < 0) soundBank = cur >> 3;
  const g = $('#soundGrid');
  if (full || g.dataset.bank != soundBank || g.dataset.cur != cur) {
    g.innerHTML = '';
    for (let i = 0; i < 8; i++) {
      const s = soundBank * 8 + i, c = el('div', 'cell' + (s === cur ? ' on' : ''), `<div class="n">${patName(s)}</div>${H.soundName(s)}`);
      c.onclick = () => H.set(ID.J6_P_SOUND, s);
      g.appendChild(c);
    }
    g.dataset.bank = soundBank; g.dataset.cur = cur;
  }
  knobInputs.forEach((inp, i) => { if (document.activeElement !== inp) inp.value = H.get(ID.J6_P_KNOB0 + i); });
}

// chords
for (let s = 0; s < ID.J6_CHORD_SETS; s++) $('#csSel').appendChild(new Option(`Set ${s + 1}: ${H.genre(s)}`, s));
$('#csSel').onchange = (e) => H.set(ID.J6_P_CHORD_SET, +e.target.value);
$('#csPrev').onclick = () => H.set(ID.J6_P_CHORD_SET, (H.get(ID.J6_P_CHORD_SET) + 99) % 100);
$('#csNext').onclick = () => H.set(ID.J6_P_CHORD_SET, (H.get(ID.J6_P_CHORD_SET) + 1) % 100);
$('#csToggle').onclick = () => H.set(ID.J6_P_CHORD, H.get(ID.J6_P_CHORD) ? 0 : 1);
function playChordKey(k) {
  if (!ac) startAudio();
  if (!H.get(ID.J6_P_CHORD)) H.set(ID.J6_P_CHORD, 1);
  const fk = k + 7;                                // chord key k (C = 0) is FM-1 key C4 + k
  H.setKey(fk, true);
  setTimeout(() => H.setKey(fk, false), 700);
}
function refreshChords(full) {
  const set = H.get(ID.J6_P_CHORD_SET), st = H.status(), box = $('#chordCards');
  $('#csSel').value = set;
  $('#csMode').textContent = st.chord ? 'CHORD on' : 'CHORD off';
  $('#csMode').classList.toggle('on', !!st.chord);
  const playing = st.chord && st.lastChord !== ID.J6_NO_CHORD ? st.lastChord & 15 : -1;
  if (full || box.dataset.set != set) {
    box.innerHTML = '';
    for (let k = 0; k < 12; k++) {
      const black = [1, 3, 6, 8, 10].includes(k);
      const c = el('div', 'ccard' + (black ? ' black' : ''), `<div class="k">${NOTE[k]}</div><div class="c">${H.chordName(set, k)}</div><div class="nn">${H.chordNotes(set, k).map(noteName).join(' ')}</div>`);
      c.onclick = () => playChordKey(k);
      box.appendChild(c);
    }
    box.dataset.set = set;
  }
  [...box.children].forEach((c, k) => c.classList.toggle('on', k === playing));
}

// phrases
let phrases = null;
fetch('data/phrases_6_9.json?v=09783e1').then((r) => r.json()).then((j) => { phrases = j; refreshEditor(true); }).catch(() => {});
for (let s = 1; s <= 9; s++) {
  const b = el('div', 'cell', `<div class="n">Style ${s}</div>${H.styleName(s)}`);
  b.onclick = () => H.set(ID.J6_P_STYLE, s);
  $('#styleList').appendChild(b);
}
$('#phToggle').onclick = () => H.cmd(ID.J6_CMD_STYLE);
$('#phSize').onchange = () => refreshEditor(true);
$('#phTry').onclick = () => {
  if (!ac) startAudio();
  if (!H.status().styleOn) H.cmd(ID.J6_CMD_STYLE);
  const n = [60, 64, 67, 71].slice(0, +$('#phSize').value);
  n.forEach((x) => H.midiIn(0x90, x, 100));
  const ms = 2 * 4 * 60000 / (H.status().tempo10 / 10);
  setTimeout(() => n.forEach((x) => H.midiIn(0x80, x, 0)), ms);
};
function refreshPhrases(full) {
  const st = H.status(), style = H.get(ID.J6_P_STYLE), v = H.get(ID.J6_P_VAR), size = +$('#phSize').value;
  $('#phOn').textContent = st.styleOn ? 'PHRASE on' : 'PHRASE off';
  $('#phOn').classList.toggle('on', !!st.styleOn);
  [...$('#styleList').children].forEach((c, i) => c.classList.toggle('on', i + 1 === style));
  const vl = $('#varList');
  if (full || vl.dataset.s != style) {
    vl.innerHTML = '';
    for (let i = 1; i <= 12; i++) {
      const c = el('div', 'cell', `<div class="n">${i}</div>${H.varName(style, i)}`);
      c.onclick = () => H.set(ID.J6_P_VAR, i);
      vl.appendChild(c);
    }
    vl.dataset.s = style;
  }
  [...vl.children].forEach((c, i) => c.classList.toggle('on', i + 1 === v));
  const roll = $('#roll'), key = `${style}/${v}/${size}/${!!phrases}`;
  if (roll.dataset.k === key && !full) return;
  roll.dataset.k = key;
  roll.innerHTML = '';
  const list = phrases && style >= 6 && phrases.phrases[`s${style}v${String(v).padStart(2, '0')}`];
  if (!list) { roll.innerHTML = `<div style="padding:40px;text-align:center;color:var(--dim)">${style >= 6 ? 'Loading…' : 'Arpeggio and rhythm styles: hold keys (or Audition) to hear them'}</div>`; return; }
  const p = list[String(size)], loop = p.loop, beats = loop / 96;
  for (let b = 1; b < beats; b++) roll.appendChild(Object.assign(el('div', 'beat'), { style: `left:${b * 100 / beats}%` }));
  for (const e of p.notes) {
    const d = el('div', 'ev' + (e.acc ? ' acc' : ''));
    d.style.left = (e.t * 100 / loop) + '%';
    d.style.width = Math.max(0.6, e.len * 100 / loop) + '%';
    d.style.top = (88 - e.tone * 26 - (4 - size) * 0) + 'px';
    roll.appendChild(d);
  }
  roll.appendChild(Object.assign(el('div', '', `${beats}-beat loop`), { style: 'position:absolute;right:8px;top:4px;font-size:11px;color:var(--dim)' }));
}

// sequencer
for (let p = 0; p < ID.J6_PATTERNS; p++) $('#patSel').appendChild(new Option(patName(p), p));
$('#patSel').onchange = (e) => H.cmd(ID.J6_CMD_PATTERN, +e.target.value);
$('#sqPlay').onclick = () => { if (!ac) startAudio(); H.cmd(ID.J6_CMD_PLAY); };
$('#sqRec').onclick = () => H.cmd(ID.J6_CMD_REC);
$('#sqStop').onclick = () => { H.cmd(ID.J6_CMD_STOP); H.cmd(ID.J6_CMD_PANIC); };
$('#tempo').onchange = (e) => H.set(ID.J6_P_TEMPO10, Math.round(+e.target.value * 10));
$('#last').onchange = (e) => H.set(ID.J6_P_LAST, +e.target.value);
$('#sqSave').onclick = () => { E.j6_engine_write(0); log('#prLog', `Pattern ${patName(H.status().pattern)} saved (in the browser's FM-1 memory).`); };
$('#sqClear').onclick = () => { if (confirm('Clear every step of this pattern?')) H.cmd(ID.J6_CMD_PATTERN_CLEAR); };
const stepEls = [];
for (let s = 0; s < 64; s++) {
  const c = el('div', 'cell');
  c.onclick = () => { const st = H.status(); H.cmd(ID.J6_CMD_STEP_SELECT, st.sel === s ? -1 : s); };
  $('#steps').appendChild(c);
  stepEls.push(c);
}
function stepText(step, set) {
  if (step.notes.length && step.chord !== ID.J6_NO_CHORD) return H.chordName(set, step.chord & 15);
  if (step.notes.length) return step.notes.map(noteName).join('<br>');
  return step.flags & ID.J6_STF_TIE ? '~' : '';
}
function refreshSeq() {
  const st = H.status(), pat = H.pattern(st.pattern), set = H.get(ID.J6_P_CHORD_SET);
  $('#patSel').value = st.pattern;
  if (document.activeElement !== $('#tempo')) $('#tempo').value = (pat.tempo10 / 10).toFixed(1);
  if (document.activeElement !== $('#last')) $('#last').value = pat.last;
  pat.steps.forEach((step, s) => {
    const c = stepEls[s], txt = `<div class="n">${s + 1}${step.phrase & 0x80 ? ' ◆' : ''}</div>${stepText(step, set)}`;
    if (c.dataset.t !== txt) { c.innerHTML = txt; c.dataset.t = txt; }
    c.classList.toggle('on', st.sel === s);
    c.classList.toggle('play', st.stepNow === s);
    c.classList.toggle('off', s >= pat.last);
  });
}

// settings
function refreshSettings(full) {
  const box = $('#setList');
  if (full || !box.children.length) {
    box.innerHTML = '';
    for (let id = 0; id < ID.J6_P_COUNT; id++) {
      const lab = H.cstr(E.web_param_label(id));
      if (!lab) continue;
      const step = id === ID.J6_P_TEMPO10 ? 10 : 1;
      const name = el('div', '', lab), val = el('div', 'val');
      const minus = el('button', 'btn small', '−'), plus = el('button', 'btn small', '+'), b = el('b');
      minus.onclick = () => H.set(id, H.get(id) - step);
      plus.onclick = () => H.set(id, H.get(id) + step);
      val.append(minus, b, plus);
      val.dataset.id = id;
      box.append(name, val);
    }
  }
  box.querySelectorAll('.val').forEach((v) => { const id = +v.dataset.id; v.querySelector('b').textContent = H.cstr(E.web_param_value(id, H.get(id))); });
}

function refreshEditor(full = false) {
  if (pane === 'sounds') refreshSounds(full);
  else if (pane === 'chords') refreshChords(full);
  else if (pane === 'phrases') refreshPhrases(full);
  else if (pane === 'seq') refreshSeq();
  else if (pane === 'settings') refreshSettings(full);
}

// ---- project: the FM-1's pattern memory, in the browser or a file ------------------------------------
function log(sel, msg) { $(sel).textContent = msg; }
function storeBytes() { E.j6_engine_write(1); return u8().slice(E.web_store(), E.web_store() + E.web_store_len()); }
function loadBytes(b) {
  if (b.length !== E.web_store_len()) throw new Error('not a HERA project (wrong size)');
  u8().set(b, E.web_store());
  E.web_reload();
}
const b64 = (b) => { let s = ''; for (let i = 0; i < b.length; i += 0x8000) s += String.fromCharCode(...b.subarray(i, i + 0x8000)); return btoa(s); };
const unb64 = (s) => Uint8Array.from(atob(s), (c) => c.charCodeAt(0));
function restoreFromBrowser(quiet) {
  try { const s = localStorage.getItem('hera.project'); if (s) { loadBytes(unb64(s)); if (!quiet) log('#prLog', 'Loaded the project saved in this browser.'); } else if (!quiet) log('#prLog', 'No project saved in this browser yet.'); }
  catch (e) { if (!quiet) log('#prLog', 'Could not load: ' + e.message); }
}
$('#prSave').onclick = () => { try { localStorage.setItem('hera.project', b64(storeBytes())); log('#prLog', 'Saved in this browser: 64 patterns and the settings.'); } catch (e) { log('#prLog', 'Could not save: ' + e.message); } };
$('#prLoad').onclick = () => restoreFromBrowser(false);
$('#prExport').onclick = () => {
  const a = el('a'); a.href = URL.createObjectURL(new Blob([storeBytes()], { type: 'application/octet-stream' }));
  a.download = 'hera-project.hera'; a.click(); log('#prLog', 'Exported hera-project.hera.');
};
$('#prImport').onchange = async (e) => {
  const f = e.target.files[0]; if (!f) return;
  try { loadBytes(new Uint8Array(await f.arrayBuffer())); log('#prLog', `Imported ${f.name}.`); } catch (err) { log('#prLog', 'Could not import: ' + err.message); }
};
$('#prReset').onclick = () => { if (!confirm('Erase all 64 patterns and the settings in the browser?')) return; u8().fill(0xFF, E.web_store(), E.web_store() + E.web_store_len()); E.web_reload(); log('#prLog', 'Factory reset done.'); };

// ---- MIDI ------------------------------------------------------------------------------------------
let midi = null, midiOut = null;
function midiSend(st, d1, d2) { if (midiOut) try { midiOut.send(st >= 0xF8 ? [st] : (st & 0xF0) === 0xC0 ? [st, d1] : [st, d1, d2]); } catch (e) {} }
async function connectMidi() {
  if (!navigator.requestMIDIAccess) { log('#midiLog', 'This browser has no Web MIDI: use Chrome or Edge.'); return; }
  try { midi = await navigator.requestMIDIAccess(); } catch (e) { log('#midiLog', 'MIDI was not allowed: ' + e.message); return; }
  const fill = () => {
    const ins = $('#midiIn'), outs = $('#midiOut'), vi = ins.value, vo = outs.value;
    ins.length = 1; outs.length = 1;
    midi.inputs.forEach((p) => ins.add(new Option(p.name, p.id)));
    midi.outputs.forEach((p) => outs.add(new Option(p.name, p.id)));
    ins.value = vi; outs.value = vo;
  };
  fill();
  midi.onstatechange = fill;
  $('#midiIn').onchange = () => {
    midi.inputs.forEach((p) => { p.onmidimessage = null; });
    const p = midi.inputs.get($('#midiIn').value);
    if (p) p.onmidimessage = (m) => { const [a, b = 0, c = 0] = m.data; if (a !== 0xF0) H.midiIn(a, b, c); };
    log('#midiLog', p ? `Playing from ${p.name}.` : 'No MIDI input.');
  };
  $('#midiOut').onchange = () => { midiOut = midi.outputs.get($('#midiOut').value) || null; log('#midiLog', midiOut ? `Sending to ${midiOut.name}.` : 'No MIDI output.'); };
  log('#midiLog', 'MIDI connected: choose an input and an output.');
  document.querySelector('.tab[data-pane=project]').click();
}
$('#midiBtn').onclick = connectMidi;

// ---- the controls reference --------------------------------------------------------------------------
const REF = [
  ['PLAY', 'Tap: play / stop. Hold: full stop (all sound off). GLO + PLAY: play from the selected step.'],
  ['REC', 'Real-time recording: REC, then PLAY (with a count-in if set).'],
  ['SEL', 'Tap: chord mode on / off. Hold (or GLO + SEL): the chord map.'],
  ['ARP', 'Tap: phrase (STYLE) on / off. Hold: the phrase page.'],
  ['ENV', 'HOLD: notes keep playing after you let go. In step edit: a tie to the next step.'],
  ['HOME · SEQ · LFO · FX · EDIT', 'Pages: play, sequencer (SEQ again: patterns), sounds, effects, settings.'],
  ['SAVE', 'Save the pattern (GLO + SAVE: all patterns).'],
  ['OCT − / +', 'Octave. GLO: key transpose. On the sequencer page: steps 1–16 / 17–32 / 33–48 / 49–64.'],
  ['K1 – K4', 'Filter, envelope (release), delay level, reverb level. GLO: resonance, attack, delay time, reverb time.'],
  ['SELECT · ALGORITHM', 'Phrase style 1–9 · variation 1–12.'],
  ['PRESETS', 'Tempo (GLO: 0.1 BPM), or what the page browses: chord sets, sounds, patterns, settings.'],
  ['Keys', 'Notes, or chords in chord mode. Sequencer: PRESETS picks the step; then keys enter notes, SEQ finishes; the lights under the white keys show the steps. GLO + F3 G3 A3 B3 C4: copy, paste, insert, clear, double.'],
];
$('#refTable').innerHTML = '<tr><th>Control</th><th>What it does in HERA</th></tr>' + REF.map(([a, b]) => `<tr><td><b>${a}</b></td><td>${b}</td></tr>`).join('');

restoreFromBrowser(true);
$('#tourBtn').onclick = () => startTour();
const hashTour = /tour=([A-Za-z]+)/.exec(location.hash);
if (hashTour) setTimeout(() => startTourChapter(hashTour[1]), 300);
else if (!localStorage.getItem('hera.toured')) setTimeout(() => startTour(), 600);
setInterval(tick, 8);
requestAnimationFrame(draw);
