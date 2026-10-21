# HERA guide

A walk through HERA, from your first chord to a saved pattern. For every control and setting in one place, see the [manual](HERA.md). To learn by doing, open the [Workbench](https://benbadio.github.io/hera-fm1/) and press **Guided tour**: it runs HERA in your browser and checks off each step as you do it.

Button names are the ones printed on the FM-1. **GLO** is the shift button: hold it while you press or turn something else.

## 1. Switch on and play

Switch the FM-1 on. HERA's splash shows the version, then the **Play** page: the chord or note you last played in large type, the chord set, the sound and the phrase.

Play any key. With chord mode off, each key is a note. The 27 keys are F3 to G5.

## 2. Chords

Tap **SEL** to turn chord mode on (it lights). Now every key plays a whole chord. Which chord depends on the **chord set**, one of 100.

To see them, **hold SEL**: the **chord map** shows which chord is on which key. Turn **PRESETS** to browse the sets (the set number and genre are at the top), and play keys to hear each chord. Press **HOME** to go back.

Tip: in the chord map, the keys light as you play them, so you can audition a whole set before you choose.

## 3. Phrases

A phrase plays your chord for you, as an arpeggio, a rhythm or a strum. **Tap ARP** to turn it on. Then:
- turn **SELECT** to choose the **style** (1 to 9),
- turn **ALGORITHM** to choose the **variation** (1 to 12).

Hold the keys down: the phrase plays while you hold them and stops when you let go. **Hold ARP** to open the phrase page, which draws a piano roll of the phrase for styles 6 to 9 so you can see the rhythm before you hear it.

**ENV** is **HOLD**: tap it and the notes keep sounding after you let go.

## 4. Sounds

Press **LFO** to open the **Sounds** page: 64 sounds in 8 banks of 8. Turn **PRESETS** to step through them. The keys keep playing, so you hear each sound as you go. Hold **GLO** while turning to jump a whole bank.

The four knobs shape the sound, and the bottom bar of the screen always says what they do:

| Knob | Alone | With GLO |
|---|---|---|
| K1 | Filter cutoff | Resonance |
| K2 | Envelope release | Envelope attack |
| K3 | Delay level | Delay time |
| K4 | Reverb level | Reverb time |

**FX** opens the effects page, with the delay and reverb settings in full.

## 5. The sequencer

A pattern is up to 64 **steps**. Each step holds a chord or notes and lasts four beats by default (you can change that in Settings). Press **SEQ** to see 16 steps at a time; **OCT − / +** turns the page.

**To enter a step by hand:**
1. Turn **PRESETS**. The first turn selects step 1; turn again to move to the next step.
2. Play a key (or a chord, with chord mode on). It goes into the step. A chord replaces what was there; a single note adds to it, up to four.
3. Tap **ENV** to tie the step to the next one.
4. Press **SEQ** when you're done.

The lights under the white keys are the 16 steps of the page: **lit** means the step has notes, **flashing** is the step playing now, and **blinking** is the step you're editing.

**Step tools:** with a step selected, hold **GLO** and press a white key: F3 copy, G3 paste, A3 insert, B3 clear, C4 double the pattern.

**Last step:** the pattern loops at its last step. Change it in Settings.

## 6. Play, stop and record

- **PLAY** (tap): play from step 1, or stop.
- **PLAY** (hold for half a second): a **full stop**. Everything stops and all sound is cut, with no click.
- **GLO + PLAY:** play from the selected step.

**To record as you play:**
1. Press **REC**. The screen says **Press PLAY**, and the pattern waits.
2. Press **PLAY**. (If you set a count-in in Settings, it counts first.)
3. Play. Whatever you play goes into the step that is playing, the one flashing under the white keys. A step is long, so play one chord per step.
4. It loops and adds to what's there. Press **REC** again to stop recording while the pattern keeps playing, or **REC** during playback to punch in. **PLAY** stops everything.

## 7. Patterns and saving

You have 64 patterns, in 8 banks (A to H) of 8. A pattern remembers its steps, its sound, its tempo, its phrase and its knob settings.

Press **SEQ** twice (once on the sequencer page) to open the **Patterns** grid. Turn **PRESETS** to pick one; hold **GLO** while turning to jump a bank. While a pattern is playing, the one you pick waits for the next step (a yellow frame), so changes stay in time. Press **SEQ** again to go back to the steps.

A `*` next to the pattern name means unsaved changes. Press **SAVE** to keep the pattern, or **GLO + SAVE** to save them all. Unsaved changes are lost when you switch off.

## 8. Settings

Press **EDIT**. Turn **PRESETS** to choose a setting and **K1** to change it. Leaving the page (EDIT again) saves them. The settings are in plain words: step length, shuffle, volume, transpose, MIDI channel, clock source, tune and more. Two are new in HERA:
- **Hold on step edit**: whether HOLD is released when you start editing a step.
- **Chord legato**: whether a chord keeps sounding when you move to a neighbouring chord.

## 9. Starting fresh

Settings has a **RESET** group at the bottom. Turn **PRESETS** to the item you want, then **hold SAVE** until the row fills (about a second and a half). A tap does nothing, so you can't do it by accident.
- **Clear pattern** empties the steps of the current pattern. Its sound, tempo and settings stay. Press **SAVE** afterwards to keep the empty pattern.
- **Factory reset** erases all 64 patterns and every setting, and puts HERA back as it was when you first installed it. It can't be undone.

HERA keeps its patterns and settings when you install a different firmware and come back, so a factory reset is also how you start from nothing after reinstalling.

## 10. Tempo

On most pages, turn **PRESETS** for the tempo (hold **GLO** for 0.1 BPM steps). On pages where PRESETS browses something else (chord sets, sounds, patterns, settings, steps), use **HOME** and turn it there.

## Quick reference

| | |
|---|---|
| **SEL** tap / hold | chord mode / the chord map |
| **ARP** tap / hold | phrase on-off / the phrase page |
| **ENV** | hold (a tie while editing a step) |
| **HOME, SEQ, LFO, FX, EDIT** | pages: play, sequencer, sounds, effects, settings |
| **SEQ** on the sequencer | the patterns page |
| **PLAY** tap / hold | play-stop / full stop |
| **REC**, then **PLAY** | record |
| **SAVE** / **GLO + SAVE** | save the pattern / save all |
| **OCT − / +** | octave (GLO: key transpose) |
| **SELECT / ALGORITHM** | phrase style / variation |
| **PRESETS** | tempo, or the page's list |
| **K1 to K4** | filter, envelope, delay, reverb (GLO: the second function) |

## Going back

Hold **OCT −** while switching the FM-1 on to open the USB rescue, then install any firmware again. The [installer](https://benbadio.github.io/hera-fm1/install.html) can also put the factory firmware back.
