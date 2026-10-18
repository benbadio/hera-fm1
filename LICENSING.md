# Licensing

MORPHEUS is free software under the GNU General Public License, version 3 only (`GPL-3.0-only`, full text in `LICENSE`). You may use, study, change and share it. If you give someone a firmware built from it, give them its complete source under the same licence too.

## Who made what

| Component | Authors & Projects |
| :--- | :--- |
| **MORPHEUS Application & Architecture** | Ben Badio & Contributors: 8-track ReBirth groovebox architecture, Reason Matrix 240x240 LCD sequencer UI, dual-phosphor matrix theme, transient status bar, 12-lane Drum Matrix, multi-engine routing, WebAudio interactive emulator, 1-click web flasher, and guided manual. |
| **ReBirth Multi-Engine DSP Suite** | Based on X0X DSP suite by Leo Kuroshita (@kurogedelic):<br>&bull; **TB-303:** Robin Schmidt (Open303, MIT) and schwung-303 (GPL-3.0), dm-Rat distortion (davemollen, GPL-3.0).<br>&bull; **TR-808:** Analog circuit model (8W8 / sc808, GPL-3.0).<br>&bull; **TR-909:** Hybrid synthesis & sample model (9W9 / ER-99 by Matthew Cieplak, GPL-3.0).<br>&bull; **Breaks:** 8-slice player & BB Gen generator ported from schwung-breakbeat by mestela (GPL-3.0).<br>&bull; **FX Bus & Master:** 9W9 send reverb, tempo delay, and glue compressor. |
| **Platform & Hardware HAL** | Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments ([Felucca](https://github.com/hugelton/Felucca)), with changes from [SLOOP](https://github.com/isod89/sloop-fm1) (isod89): `firmware/hal/`, `firmware/loader/`, `crt0.S`, `app.ld`, `firmware/src/` (`usb.c`, `ota.c`, `recovery.c`, `lcd.c`, `gfx.c`, `libc.c`), and `tools/`. |

## Third-party material

| What | Licence | Where |
| :--- | :--- | :--- |
| Terminus font 8x16 (ter-u16n), rendered into `firmware/gen/felucca_font.h` | SIL OFL 1.1 | `assets/fonts/Terminus-LICENSE.txt` |
| JieLi AC79 SDK: `uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin` | Apache-2.0 | <https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK> |

## Trademarks

"Felucca" and "Hügelton Instruments" are names of Hügelton Instruments; "M-VAVE" and "FM-1" belong to their respective owners; "Propellerhead", "Reason", and "ReBirth" are trademarks of their respective owners. MORPHEUS is not made, affiliated with, or endorsed by them.
