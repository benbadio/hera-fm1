# SPDX-License-Identifier: GPL-3.0-only
"""Fail on any integer constant in LLVM IR that looks like a chip ROM address (docs/RULES.md).

tools/build.py check() rejects the firmware when morpheus.dis references 0xFFC00000..0xFFCFFFFF, and a
constant that merely has that value counts (the compiler may put it in an immediate). The J-6 engine is
compiled to IR by tests/j6/run_tests.sh (one translation unit, several -O levels); this scans every i32
integer literal (both 32-bit halves of a 64-bit one: a 32-bit target splits them) for that range.

    python3 -I tests/j6/scan_rom_consts.py file.ll [file.ll ...]
"""
import re
import sys

ROM_LO, ROM_HI = 0xFFC00000, 0xFFD00000
# every integer literal of an instruction or initialiser (not %names, @names, !metadata, #attributes);
# the IR prints integers in decimal and leaves the type implicit for the second operand
CONST = re.compile(r"(?<![\w%@!#.$-])(-?\d+)\b(?!\.)")
SKIP = ("!", "attributes", "target ", "source_filename", "declare", ";")


def in_rom(v):
    return ROM_LO <= (v & 0xFFFFFFFF) < ROM_HI


def scan(path):
    bad = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for ln, line in enumerate(f, 1):
            if line.lstrip().startswith(SKIP):
                continue
            line = line.split(", !")[0]                     # trailing metadata attachments
            for m in CONST.finditer(line):
                v = int(m.group(1))
                if any(in_rom(h) for h in (v, v >> 32)):     # an i64 is two words on the chip
                    bad.append((ln, m.group(1), line.strip()))
    return bad


def main(argv):
    fails = 0
    for path in argv[1:]:
        for ln, tok, line in scan(path):
            fails += 1
            print("  %s:%d: constant %s = 0x%08X is in the chip ROM range 0xFFC00000..0xFFCFFFFF: %s"
                  % (path, ln, tok, int(tok) & 0xFFFFFFFF, line[:120]))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
