# HERA and MIDI

What HERA sends and receives. HERA uses **USB MIDI** (class compliant, one cable). It has no other MIDI port, and it doesn't handle SysEx: that belongs to the FM-1's updater.

The settings are on the Settings page (EDIT). The channel numbers below are the defaults.

## Notes
- **Received** on the MIDI channel (1): Note On and Off play the synth like the keys with chord mode off. They feed the phrase when the phrase is on. Velocity is used for direct notes, but not for phrases.
- **Sent** on the MIDI channel: the notes you play, the phrase notes, and the sequencer's notes. Key notes use the *Key velocity* setting. Note Off is sent as a real Note Off message. The notes sent are not shifted by Transpose.
- Messages on other channels are ignored, apart from system messages.

## Controllers
Only these are received:
| Message | What it does |
|---|---|
| CC 64 | Hold |
| CC 120 | All Sound Off: silences every voice at once |
| CC 121 | Reset All Controllers: Hold off |
| CC 123 | All Notes Off: releases every note held over MIDI |

The knobs and buttons have no MIDI. When HERA stops sending, or on a full stop, it sends All Sound Off on the MIDI channel.

## Program change
Values 0 to 63 select a pattern: `(bank − 1) × 8 + (pattern − 1)`, so 0 is A1 and 63 is H8.
- **Send prog. change** sends one when the pattern changes, on the *Prog. change ch.* (16).
- **Receive prog. ch.** changes the pattern when one arrives on that channel. During playback it follows *Pattern change*; stopped, it's immediate.

## Clock and sync
- **Out:** MIDI clock at 24 pulses per quarter note, with Start on PLAY and Stop on stop. Continue and Song Position are never sent, so GLO + PLAY (from a step) also sends Start.
- **In:** when *Clock source* isn't Internal, every clock from an allowed source moves the sequencer on. Start starts the current pattern at step 1, and Stop stops it. Continue is ignored. With Auto, the internal clock takes over again after half a second without a clock.
- **Clock source:** Auto (any port), Internal, MIDI only, or USB only. HERA's only MIDI port is USB, so on the FM-1 *MIDI only* receives nothing from the FM-1's own port.
- **Sync pulses/beat** and **MIDI thru** are stored, as on the J-6, but have no effect on the FM-1, which has no sync jacks and no second MIDI port for them.

## Active Sensing
HERA sends Active Sensing (0xFE) about every 250 ms. If it receives it and then it stops for more than 300 ms, all notes held over MIDI are released.
