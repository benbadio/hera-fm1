// SPDX-License-Identifier: GPL-3.0-only
// The HERA guided tour: short hands-on lessons on the virtual FM-1, in the order of the J-6 owner's manual
// (sounds, chords, phrases, the sequencer, patterns, menus). Each step highlights the control to use and
// watches the firmware's own state, so it moves on by itself once you have done it.
const $ = (s) => document.querySelector(s);
const PG = { PLAY: 0, CHORDS: 1, PHRASE: 2, SEQ: 3, PATTERNS: 4, SOUNDS: 5, FX: 6, SETTINGS: 7 };
const H = () => window.HERA;
const st = () => H().status();
const get = (id) => H().get(id);
const ID = () => H().ID;

// a lesson: chapter, title, text (HTML), target (CSS selector of the control), done(base) -> true when finished.
// base is a snapshot taken when the lesson starts.
const LESSONS = [
  { ch: 'Welcome', t: 'This is HERA', target: '#device',
    x: `<p>This is a working FM-1 running the real HERA firmware. Everything you do here happens exactly as it would on the device.</p>
        <p>HERA is a chord groovebox in the style of the Roland J-6, with chords on every key, phrases that play them for you, and a 64-pattern sequencer.</p>
        <p>Click <b>Start audio</b> at the top first, then <b>Next</b>.</p>` },

  { ch: 'Sounds', t: 'Play a note', target: '#keyboard',
    x: `<p>Play any key, with the mouse or your computer keyboard (<kbd>Z</kbd> is C4).</p><p>The note shows in large text on the screen.</p>`,
    done: (b) => st().voices > 0 || (st().lastNote !== b.lastNote && st().lastNote !== ID().J6_NO_NOTE) || st().lastChord !== b.lastChord },
  { ch: 'Sounds', t: 'Choose a sound', target: '#btn5',
    x: `<p>Press <b>LFO</b> to open the <b>Sounds</b> page: 64 Juno-style sounds in 8 banks.</p>`,
    done: () => st().page === PG.SOUNDS },
  { ch: 'Sounds', t: 'Browse the sounds', target: '#enc6',
    x: `<p>Turn <b>PRESETS</b> (drag it up, or scroll on it) to step through the sounds. Play a key after each one.</p>
        <p>Hold <b>GLO</b> (<kbd>Shift</kbd>) while turning to jump a whole bank of 8. The keys always play, so you can audition as you go.</p>`,
    done: (b) => get(ID().J6_P_SOUND) !== b.sound },
  { ch: 'Sounds', t: 'Shape it with the knobs', target: '#enc2',
    x: `<p><b>K1</b> is the filter. Turn it down and play: the sound gets darker. <b>K2</b> is the release, <b>K3</b> the delay, <b>K4</b> the reverb.</p>
        <p>Hold <b>GLO</b> (or <kbd>Shift</kbd>) and the same knobs give resonance, attack, delay time and reverb time. The bottom of the screen always shows which is which.</p>`,
    done: (b) => get(ID().J6_P_KNOB0) !== b.cutoff },

  { ch: 'Chords', t: 'Chord mode', target: '#btn3',
    x: `<p>Go back to the play page with <b>HOME</b>, then <b>tap SEL</b> to turn chord mode on (it lights).</p><p>Now every key plays a full chord.</p>`,
    done: () => st().chord === 1 },
  { ch: 'Chords', t: 'Play some chords', target: '#keyboard',
    x: `<p>Play a few keys: C, F, G, A. The screen shows each chord's name.</p>
        <p>These are chord set 1 (Pop). There are 100 sets, from pop and jazz to trance, neo soul and classical.</p>`,
    done: (b) => st().lastChord !== b.lastChord && st().lastChord !== ID().J6_NO_CHORD },
  { ch: 'Chords', t: 'The chord map', target: '#btn3',
    x: `<p><b>Hold SEL</b> for a moment: the <b>chord map</b> opens. It shows which chord sits on which key, something the J-6 can't show.</p>`,
    done: () => st().page === PG.CHORDS },
  { ch: 'Chords', t: 'Pick another chord set', target: '#enc6',
    x: `<p>Turn <b>PRESETS</b> to browse the sets. The name of each set and all 12 chords appear at once; play keys to try them.</p>
        <p>The Chords tab of the editor on the right shows the same thing with the notes of every chord.</p>`,
    done: (b) => get(ID().J6_P_CHORD_SET) !== b.chordSet },

  { ch: 'Phrases', t: 'Turn on a phrase', target: '#btn10',
    x: `<p><b>Tap ARP</b> to turn the phrase function on. Then hold a key: instead of one chord you hear it played as an arpeggio.</p>`,
    done: () => st().styleOn === 1 },
  { ch: 'Phrases', t: 'Styles and variations', target: '#enc0',
    x: `<p>Turn <b>SELECT</b> to change the style (9 of them: arpeggios, note lengths, rhythms, chord phrases and strums). <b>ALGORITHM</b> picks one of 12 variations.</p>
        <p>Try style 8 (strum) and hold a key.</p>`,
    done: (b) => get(ID().J6_P_STYLE) !== b.style },
  { ch: 'Phrases', t: 'See the phrase', target: '#btn10',
    x: `<p><b>Hold ARP</b> to open the phrase page. For styles 6–9 it draws the phrase as a piano roll, so you can see the rhythm before you play it. The accents are in orange.</p>`,
    done: () => st().page === PG.PHRASE },
  { ch: 'Phrases', t: 'Hold', target: '#btn4',
    x: `<p>Press <b>ENV</b> (HOLD) and then a key. You can let go and the phrase keeps going. Press ENV again to stop it.</p>`,
    done: () => st().hold === 1 },

  { ch: 'Sequencer', t: 'The sequencer page', target: '#btn11',
    x: `<p>Turn HOLD off again (<b>ENV</b>), then press <b>SEQ</b>. You see 16 steps of the current pattern; a pattern has up to 64.</p>`,
    done: () => st().page === PG.SEQ && st().hold === 0 },
  { ch: 'Sequencer', t: 'Pick a step', target: '#keyboard',
    x: `<p>On this page the <b>white keys pick steps</b>. Press the leftmost white key (F3) to edit step 1.</p>`,
    done: () => st().sel === 0 },
  { ch: 'Sequencer', t: 'Enter a chord', target: '#keyboard',
    x: `<p>Now the keys enter notes again. With chord mode on, press one key: its chord goes into step 1.</p>
        <p>Turn <b>PRESETS</b> to move to the next step and add another chord, then press <b>SEQ</b> to finish editing.</p>`,
    done: () => st().sel === ID().J6_NONE && H().pattern(st().pattern).steps.some((s) => s.notes.length) },
  { ch: 'Sequencer', t: 'Play it', target: '#btn12',
    x: `<p><b>Tap PLAY</b> (or <kbd>Space</kbd>). The yellow frame follows the playhead. Tap PLAY again to stop.</p>`,
    done: () => st().seq === ID().J6_SEQ_PLAYING },
  { ch: 'Sequencer', t: 'Full stop', target: '#btn12',
    x: `<p>While it plays, <b>hold PLAY</b> for half a second: everything stops, and all sound fades out, delay and reverb tails included. Handy when a phrase is still holding.</p>`,
    done: () => st().seq !== ID().J6_SEQ_PLAYING && st().voices === 0 },
  { ch: 'Sequencer', t: 'Record in real time', target: '#btn13',
    x: `<p>Press <b>REC</b>, then <b>PLAY</b>, and play chords along with the pattern. They are recorded into the steps. Tap PLAY to stop.</p>`,
    done: () => st().rec === 1 },

  { ch: 'Patterns', t: 'Save your pattern', target: '#btn9',
    x: `<p>Stop playback (tap PLAY), then press <b>SAVE</b>. The <b>*</b> next to the pattern name at the top of the screen goes away.</p>
        <p>On the FM-1 it is kept through power-off. In the browser, use the Project tab to keep it.</p>`,
    done: () => !((st().pattern < 32 ? st().dirty0 >>> st().pattern : st().dirty1 >>> (st().pattern - 32)) & 1) && st().rec === 0 },
  { ch: 'Patterns', t: 'All 64 patterns', target: '#btn11',
    x: `<p>Press <b>SEQ</b> again (on the sequencer page): the pattern grid, 8 banks of 8. Turn <b>PRESETS</b> to pick a pattern; hold <b>GLO</b> (<kbd>Shift</kbd>) while turning to jump a bank. The keys keep playing.</p>
        <p>While a pattern plays, a pattern you pick waits for the next step (a yellow frame), so changes stay in time. Press <b>SEQ</b> once more to go back to the steps.</p>`,
    done: () => st().page === PG.PATTERNS },

  { ch: 'Settings', t: 'Settings in plain words', target: '#btn6',
    x: `<p>Press <b>EDIT</b>. Every J-6 menu item is here in plain words: MIDI channel, clock source, count-in, tune and more, plus HERA's own <i>Hold on step edit</i> and <i>Chord legato</i>.</p>
        <p><b>PRESETS</b> chooses, <b>K1</b> changes. Leaving the page saves.</p>`,
    done: () => st().page === PG.SETTINGS },
  { ch: 'Settings', t: "That's the tour", target: null,
    x: `<p class="done">You've used every part of HERA.</p>
        <p>The editor on the right does the same things with a mouse: sounds, chord sets, phrases, the step grid, settings, and saving projects. The <b>Controls</b> tab lists every control.</p>
        <p>When you're ready, <a href="install.html">install HERA on your FM-1</a>.</p>` },
];

let cur = -1, base = null, doneSet = new Set(JSON.parse(localStorage.getItem('hera.tourDone') || '[]'));
let lastTarget = null;

function snapshot() {
  const s = st();
  return { lastNote: s.lastNote, lastChord: s.lastChord, sound: get(ID().J6_P_SOUND), cutoff: get(ID().J6_P_KNOB0),
           chordSet: get(ID().J6_P_CHORD_SET), style: get(ID().J6_P_STYLE) };
}
function highlight(sel) {
  if (lastTarget) lastTarget.classList.remove('tour-target');
  lastTarget = sel ? $(sel) : null;
  if (lastTarget) {
    const t = lastTarget.querySelector('.cap, .knob') || lastTarget;
    lastTarget = t;
    t.classList.add('tour-target');
  }
}
function show(i) {
  cur = Math.max(0, Math.min(LESSONS.length - 1, i));
  const L = LESSONS[cur];
  base = snapshot();
  $('#tourTitle').textContent = `${L.ch} · ${cur + 1} of ${LESSONS.length}`;
  $('#tourBody').innerHTML = `<h3>${L.t}</h3>${L.x}${L.done ? '<p class="hint" id="tourWait" style="color:var(--dim)">Waiting for you to try it…</p>' : ''}`;
  $('#tourProg').style.width = `${(cur + 1) * 100 / LESSONS.length}%`;
  $('#tourPrev').disabled = cur === 0;
  $('#tourNext').textContent = cur === LESSONS.length - 1 ? 'Finish' : L.done ? 'Skip' : 'Next';
  highlight(L.target);
}
function finishLesson() {
  doneSet.add(cur);
  localStorage.setItem('hera.tourDone', JSON.stringify([...doneSet]));
  const w = $('#tourWait');
  if (w) { w.className = 'done'; w.textContent = '✓ Done!'; }
  setTimeout(() => { if (cur < LESSONS.length - 1) show(cur + 1); }, 900);
}
export function tourTick() {
  if (cur < 0 || !$('#tour').classList.contains('on')) return;
  const L = LESSONS[cur];
  if (L.done && !doneSet.has(-1 - cur) && $('#tourWait') && $('#tourWait').className !== 'done') {
    try { if (L.done(base)) finishLesson(); } catch (e) {}
  }
}
export function startTourChapter(name) {
  const i = LESSONS.findIndex((l) => l.ch.toLowerCase() === String(name).toLowerCase());
  startTour(i < 0 ? 0 : i);
}
export function startTour(i) {
  $('#tour').classList.add('on');
  show(i ?? Math.max(0, [...Array(LESSONS.length).keys()].find((k) => !doneSet.has(k)) ?? 0));
}
function chapters() {
  const names = [...new Set(LESSONS.map((l) => l.ch))];
  $('#tourBody').innerHTML = '<h3>Chapters</h3><div class="chapters">' + names.map((n) => {
    const idx = LESSONS.findIndex((l) => l.ch === n), all = LESSONS.map((l, k) => k).filter((k) => LESSONS[k].ch === n);
    return `<button data-i="${idx}" class="${all.every((k) => doneSet.has(k)) ? 'ok' : ''}">${n}</button>`;
  }).join('') + '</div><p style="margin-top:10px"><button class="btn small" id="tourReset">Start over</button></p>';
  $('#tourBody').querySelectorAll('[data-i]').forEach((b) => (b.onclick = () => show(+b.dataset.i)));
  $('#tourReset').onclick = () => { doneSet.clear(); localStorage.removeItem('hera.tourDone'); show(0); };
}
$('#tourPrev').onclick = () => show(cur - 1);
$('#tourNext').onclick = () => { if (cur === LESSONS.length - 1) { closeTour(); return; } show(cur + 1); };
$('#tourMenu').onclick = chapters;
function closeTour() { $('#tour').classList.remove('on'); highlight(null); localStorage.setItem('hera.toured', '1'); }
$('#tourClose').onclick = closeTour;
