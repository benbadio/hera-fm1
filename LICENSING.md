# Licensing

HERA is free software under the GNU General Public License, version 3 only (`GPL-3.0-only`, full text in `LICENSE`). You may use, study, change and share it. If you give someone a firmware built from it, give them its complete source under the same licence too.

## Who made what

| Component | Authors & Projects |
| :--- | :--- |
| **HERA** | Ben Badio: the J-6-style engine (`firmware/src/j6/`: chord sets, phrase styles, 64-pattern sequencer, synth, glue), the HERA app and screen (`firmware/src/hera_*.c`), the J-6 capture tools (`capture/`) and the data tools in `tools/`. A behavioral recreation of the Roland J-6; no Roland code, firmware or samples. |
| **Dinghy** (the starter HERA was built from) | zvenson: the smallest FM-1 firmware to build on ([dinghy](https://github.com/zvenson/dinghy)), itself based on Felucca with its platform as SLOOP carries it. |
| **JUNO voice** | Juno-style voice and BBD chorus after Clipper (`eng_juno.c`, `dsp.c`, `fx.c`), Copyright (C) 2026 3dSam, GPL-3.0 ([clipper-fm1](https://github.com/isod89/clipper-fm1)). |
| **MORPHEUS** (Ben's earlier firmware, built on Dinghy: the repository HERA grew from) | Ben Badio & Contributors: 8-track ReBirth groovebox architecture, Reason Matrix 240x240 LCD sequencer UI, transient status bar, 12-lane Drum Matrix, multi-engine routing, WebAudio interactive emulator, 1-click web flasher, and guided manual. |
| **ReBirth Multi-Engine DSP Suite** | Based on X0X DSP suite by Leo Kuroshita (@kurogedelic):<br>&bull; **TB-303:** Robin Schmidt (Open303, MIT) and schwung-303 (GPL-3.0), dm-Rat distortion (davemollen, GPL-3.0).<br>&bull; **TR-808:** Analog circuit model (8W8 / sc808, GPL-3.0).<br>&bull; **TR-909:** Hybrid synthesis & sample model (9W9 / ER-99 by Matthew Cieplak, GPL-3.0).<br>&bull; **Breaks:** 8-slice player & BB Gen generator ported from schwung-breakbeat by mestela (GPL-3.0).<br>&bull; **FX Bus & Master:** 9W9 send reverb, tempo delay, and glue compressor. |
| **Platform & Hardware HAL** | Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments ([Felucca](https://github.com/hugelton/Felucca)), with changes from [SLOOP](https://github.com/isod89/sloop-fm1) (isod89): `firmware/hal/`, `firmware/loader/`, `crt0.S`, `app.ld`, `firmware/src/` (`usb.c`, `ota.c`, `recovery.c`, `lcd.c`, `gfx.c`, `libc.c`), and `tools/`. |

## Third-party material

| What | Licence | Where |
| :--- | :--- | :--- |
| Terminus font 8x16 (ter-u16n), rendered into `firmware/gen/felucca_font.h` | SIL OFL 1.1 | `assets/fonts/Terminus-LICENSE.txt` |
| Playfair Display Black: its glyph shapes, rendered into the boot-screen wordmark bitmap `firmware/gen/hera_wordmark.h` by `tools/gen_wordmark.py` (the font file itself is not included) | SIL OFL 1.1 | <https://github.com/google/fonts/tree/main/ofl/playfairdisplay> |
| JieLi AC79 SDK: `uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin` | Apache-2.0 | <https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK> |

## Trademarks

"Felucca" and "Hügelton Instruments" are names of Hügelton Instruments; "M-VAVE" and "FM-1" belong to their respective owners; "Propellerhead", "Reason", and "ReBirth" are trademarks of their respective owners. "Roland", "J-6", "JUNO" and "AIRA" are trademarks of Roland Corporation. HERA is not made, affiliated with, or endorsed by any of them.
