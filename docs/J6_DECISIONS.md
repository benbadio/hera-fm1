# Owner decisions (2026-10-20)

Behaviour the J-6 manual doesn't specify, decided by the project owner.

| # | Topic | Decision | Implementation |
|---|---|---|---|
| D1 | Entering step edit | Turns HOLD off **by default**. | New global setting `HoLd.E` (Hold on step edit): `OFF` (default, HOLD is released) / `KEEP`. |
| D2 | STOP button | Press once: stop the sequencer; a phrase you're holding live keeps playing. Press again while stopped: **kill all sound**, meaning every synth voice (with a short fade, no click), the live phrase, the HOLD latch, and delay/reverb tails. Like the MPC. | Engine: a second STOP while stopped calls a "panic" path. Also sends All Sound Off (CC120) on CH. |
| D3 | Chord change, shared notes | Selectable. | New global setting `cHd.L` (chord legato): `On` = shared notes keep sounding (default, the current behaviour) / `OFF` = every chord change retriggers all notes. |
| D4 | SYnC = MIDI or USB | Follow clock from that port only. | As built. |
| D5 | Program Change at power-on | None is sent for the pattern selected at boot. | As built. |
| D6 | Active Sensing | Keep: the J-6 sends it (see its MIDI chart). | Left where it is: the engine's timed loop queues it every 250 ms. Everything the engine sends goes through that one outgoing queue, so moving it would add a second writer to the queue for no benefit. |
