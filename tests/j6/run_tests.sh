#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Host tests for the J-6 engine (firmware/src/j6). Run from anywhere:
#
#   tests/j6/run_tests.sh                    every tests/j6/test_*.c, each linked with every engine .c
#   tests/j6/run_tests.sh chord seq          only test_chord.c and test_seq.c (still every engine .c)
#   J6_ISOLATED=1 tests/j6/run_tests.sh seq  link test_seq.c with firmware/src/j6/j6_seq*.c only, and
#                                            lint / check only that module: for parallel module work
#   J6_SANITIZE=1 ...                        also build with -fsanitize=address,undefined (on macOS:
#                                            undefined only, AddressSanitizer hangs at startup there)
#   J6_SANITIZE=ubsan / asan ...             force -fsanitize=undefined / address,undefined
#   J6_RENDER=0 ...                          skip the demo render (tools/j6_render.sh) in full mode
#
# The engine glue (test_engine.c) needs every module, so `engine` always links all engine sources.
# Steps (any failure makes the exit status 1):
#   1. lint      tests/j6/lint_engine.py: no float, no malloc, allowed includes, single-TU naming
#   2. arm       each engine .c compiled for armv7 soft-float (-O0): no float helper, no 64-bit division
#      thumb     each engine .c for thumbv6m (-O0, -Os): no 64-bit shift by a variable count (a libcall)
#   3. one-tu    all engine .c files #included into one translation unit, as the firmware build does
#      rom-consts  that unit as ARM IR (-O0/-Os/-O2): no constant in the chip ROM range 0xFFC00000..
#   4. tables    tools/gen_*.py --check: the committed generated headers match their generators
#   5. ram       sizeof of every module state struct against the budgets in j6.h
#   6. tests     build and run each test; a non-zero exit status is a failure
#   7. render    tools/j6_render.sh: the demo through the whole engine -> build/j6/demo.wav
# Steps 3, 5 and 7 run only in full mode. Steps 1, 4 skip without python3; step 2 skips without an ARM
# capable clang (J6_STRICT=1 turns those skips into failures).
set -u
cd "$(dirname "$0")/../.." || exit 1

CC=${CC:-cc}
ARMCC=${J6_ARM_CC:-clang}
OUT=build/j6test
SRC=firmware/src/j6
CFLAGS="-std=c99 -O2 -g -Wall -Wextra -Werror -Wdouble-promotion -Wfloat-equal -Wvla -I$SRC -Itests/j6"
case "${J6_SANITIZE:-0}" in
    0) ;;
    ubsan) CFLAGS="$CFLAGS -fsanitize=undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer" ;;
    asan) CFLAGS="$CFLAGS -fsanitize=address,undefined -fno-omit-frame-pointer" ;;
    *)
        if [ "$(uname -s)" = Darwin ]; then
            echo "  (J6_SANITIZE: macOS, undefined-behaviour sanitizer only; J6_SANITIZE=asan forces ASan)"
            CFLAGS="$CFLAGS -fsanitize=undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
        else
            CFLAGS="$CFLAGS -fsanitize=address,undefined -fno-omit-frame-pointer"
        fi ;;
esac
ISOLATED=${J6_ISOLATED:-0}
STRICT=${J6_STRICT:-0}
mkdir -p "$OUT"

fails=0
failed_list=""
fail() { fails=$((fails + 1)); failed_list="$failed_list $1"; echo "  FAIL: $1"; }
skip() { if [ "$STRICT" = 1 ]; then fail "$1 (skipped, J6_STRICT=1)"; else echo "  skip: $1"; fi; }

# ---- which tests
tests=""
if [ $# -gt 0 ]; then
    for n in "$@"; do
        n=${n#test_}; n=${n%.c}
        if [ -f "tests/j6/test_$n.c" ]; then tests="$tests $n"; else fail "no tests/j6/test_$n.c"; fi
    done
else
    for f in tests/j6/test_*.c; do
        [ -e "$f" ] || continue
        n=${f#tests/j6/test_}; n=${n%.c}
        tests="$tests $n"
    done
fi

all_c=""
for f in "$SRC"/*.c; do [ -e "$f" ] && all_c="$all_c $f"; done
all_h=""
for f in "$SRC"/*.h; do [ -e "$f" ] && all_h="$all_h $f"; done

module_c() {   # engine .c files of module $1 (isolated mode); the glue needs them all
    if [ "$1" = engine ]; then echo "$all_c"; return; fi
    m=""
    for f in "$SRC"/j6_"$1"*.c; do [ -e "$f" ] && m="$m $f"; done
    echo "$m"
}
module_h() {
    if [ "$1" = engine ]; then echo "$all_h"; return; fi
    m="$SRC/j6.h"
    for f in "$SRC"/j6_"$1"*.h; do [ -e "$f" ] && m="$m $f"; done
    echo "$m"
}
generator() {  # the table generator a module owns
    case "$1" in
        chord) echo tools/gen_tables.py ;;
        phrase) echo tools/gen_phrase_tables.py ;;
        synth) echo tools/gen_synth_tables.py ;;
        *) echo "" ;;
    esac
}

if [ "$ISOLATED" = 1 ]; then
    scope_c=""; scope_h=""; gens=""
    for n in $tests; do
        scope_c="$scope_c $(module_c "$n")"
        scope_h="$scope_h $(module_h "$n")"
        g=$(generator "$n"); [ -n "$g" ] && gens="$gens $g"
    done
    echo "==> J-6 engine tests (isolated:$tests)"
else
    scope_c="$all_c"; scope_h="$all_h"
    gens="tools/gen_tables.py tools/gen_phrase_tables.py tools/gen_synth_tables.py"
    echo "==> J-6 engine tests (full)"
fi

# ---- 1. lint
echo "--> lint"
if command -v python3 >/dev/null 2>&1; then
    # shellcheck disable=SC2086
    python3 -I tests/j6/lint_engine.py --platform-root firmware $scope_c $scope_h || fail lint
else
    skip "lint (no python3)"
fi

# ---- 2. ARM soft-float compile: float helpers or 64-bit division would need libgcc the FM-1 lacks
echo "--> arm (no float, no 64-bit division)"
if [ -z "$(echo $scope_c)" ]; then
    echo "  (no engine .c files yet)"
elif printf 'int j6_probe;\n' > "$OUT/arm_probe.c" &&
     $ARMCC -target armv7-none-eabi -mfloat-abi=soft -ffreestanding -c "$OUT/arm_probe.c" -o "$OUT/arm_probe.o" 2>/dev/null; then
    for f in $scope_c; do
        o="$OUT/arm_$(basename "$f" .c).o"
        if ! $ARMCC -target armv7-none-eabi -mfloat-abi=soft -ffreestanding -std=c99 -O0 -Wall -Werror \
                -I"$SRC" -c "$f" -o "$o"; then
            fail "arm compile $f"
            continue
        fi
        bad=$(nm -u "$o" 2>/dev/null | awk '{print $NF}' |
              grep -E '^__aeabi_(f[a-z0-9]*|d[a-z0-9]*|c[fd][a-z0-9]*|u?i2[fd]|u?l2[fd]|u?ldivmod)$|^__(u?div|u?mod)di3$|^__[a-z0-9]*[sd]f[0-9]?$|^__float|^__fix' || true)
        if [ -n "$bad" ]; then
            fail "arm $f needs:$(echo $bad | tr '\n' ' ')"
        fi
    done
    # armv7 inlines every 64-bit shift; Thumb-1 makes a variable-count one a libcall (__aeabi_llsl / llsr /
    # lasr), as a 32-bit core without a funnel shift may (pi32v2: not verified). firmware/src/libc.c has
    # none of them, so a 64-bit shift by a variable count must not appear (shift by constants instead).
    if $ARMCC -target thumbv6m-none-eabi -mfloat-abi=soft -ffreestanding -c "$OUT/arm_probe.c" \
            -o "$OUT/arm_probe.o" 2>/dev/null; then
        for f in $scope_c; do
            for lvl in -O0 -Os; do
                o="$OUT/t1_$(basename "$f" .c).o"
                $ARMCC -target thumbv6m-none-eabi -mfloat-abi=soft -ffreestanding -std=c99 $lvl -Wall -Werror \
                    -I"$SRC" -c "$f" -o "$o" || { fail "thumb compile $f"; break; }
                bad=$(nm -u "$o" 2>/dev/null | awk '{print $NF}' |
                      grep -E '^__aeabi_(llsl|llsr|lasr)$|^__(ashl|lshr|ashr)di3$' || true)
                if [ -n "$bad" ]; then
                    fail "thumb $lvl $f needs (64-bit variable shift):$(echo $bad | tr '\n' ' ')"
                    break
                fi
            done
        done
    else
        skip "64-bit shift check ($ARMCC cannot target thumbv6m-none-eabi)"
    fi
else
    skip "arm check ($ARMCC cannot target armv7-none-eabi; set J6_ARM_CC)"
fi

# ---- 3. one translation unit (the firmware #includes every engine file after platform.c)
if [ "$ISOLATED" != 1 ] && [ -n "$(echo $all_c)" ]; then
    echo "--> one-tu"
    {
        echo "/* generated by tests/j6/run_tests.sh: every engine file in one translation unit */"
        for f in $all_c; do echo "#include \"$(basename "$f")\""; done
    } > "$OUT/j6_all.c"
    # shellcheck disable=SC2086
    $CC $CFLAGS -Wno-unused-function -c "$OUT/j6_all.c" -o "$OUT/j6_all.o" || fail one-tu
    # no constant may look like a chip ROM address (docs/RULES.md; tools/build.py check() scans the
    # firmware disassembly for 0xFFC00000..0xFFCFFFFF): the one TU as IR, at the levels the app may use
    echo "--> rom-consts"
    if command -v python3 >/dev/null 2>&1 &&
       $ARMCC -target armv7-none-eabi -mfloat-abi=soft -ffreestanding -S -emit-llvm -o /dev/null \
            "$OUT/arm_probe.c" 2>/dev/null; then
        lls=""
        for lvl in O0 Os O2; do
            ll="$OUT/j6_all_$lvl.ll"
            # shellcheck disable=SC2086
            $ARMCC -target armv7-none-eabi -mfloat-abi=soft -ffreestanding -std=c99 -$lvl -I"$SRC" \
                -Wno-unused-function -S -emit-llvm "$OUT/j6_all.c" -o "$ll" || { fail "rom-consts IR -$lvl"; continue; }
            lls="$lls $ll"
        done
        # shellcheck disable=SC2086
        [ -n "$lls" ] && { python3 -I tests/j6/scan_rom_consts.py $lls || fail rom-consts; }
    else
        skip "rom-consts (no python3 or no ARM capable $ARMCC)"
    fi
fi

# ---- 4. generated tables are up to date
echo "--> tables"
if command -v python3 >/dev/null 2>&1; then
    for g in $gens; do
        [ -f "$g" ] || continue
        python3 -I "$g" --check || fail "tables $g --check"
    done
else
    skip "tables (no python3)"
fi

# ---- 5. RAM budget of the module state structs
if [ "$ISOLATED" != 1 ]; then
    echo "--> ram"
    {
        echo '#include <stdio.h>'
        echo '#include "j6.h"'
        for h in phrase seq synth; do
            [ -f "$SRC/j6_${h}_state.h" ] && echo "#include \"j6_${h}_state.h\"" && echo "#define J6T_HAVE_$h 1"
        done
        cat <<'EOF'
int main(void)
{
    unsigned long total = 0, sz;
#define J6T_SZ(T, k) (sz = (unsigned long)sizeof(T) * (k), total += sz, printf("  %-14s x%d %7lu bytes\n", #T, k, sz))
#ifdef J6T_HAVE_seq
    J6T_SZ(j6_seq_t, 1);
    J6T_SZ(j6_clock_t, 1);
#endif
#ifdef J6T_HAVE_phrase
    J6T_SZ(j6_phrase_t, 2);
    J6T_SZ(j6_held_t, 2);
#endif
#ifdef J6T_HAVE_synth
    J6T_SZ(j6_synth_t, 1);
#endif
    printf("  modules        %7lu bytes (+ glue <= %u) of %u; fx pool %u bytes\n", total,
           (unsigned)J6_RAM_ENGINE_MAX, (unsigned)J6_RAM_TOTAL_MAX, (unsigned)(J6_FX_MEM_LEN * 2u));
    return total + J6_RAM_ENGINE_MAX > J6_RAM_TOTAL_MAX;
}
EOF
    } > "$OUT/ram_report.c"
    # shellcheck disable=SC2086
    if $CC $CFLAGS -o "$OUT/ram_report" "$OUT/ram_report.c"; then
        "$OUT/ram_report" || fail "ram budget"
    else
        fail "ram report build"
    fi
fi

# ---- 6. the tests
echo "--> tests"
[ -n "$(echo $tests)" ] || echo "  (no tests/j6/test_*.c)"
for n in $tests; do
    if [ "$ISOLATED" = 1 ]; then srcs=$(module_c "$n"); else srcs="$all_c"; fi
    exe="$OUT/test_$n"
    echo "  [$n]"
    # shellcheck disable=SC2086
    if ! $CC $CFLAGS -o "$exe" "tests/j6/test_$n.c" $srcs; then
        fail "build test_$n"
        continue
    fi
    "$exe" || fail "test_$n"
done

# ---- 7. the demo render through the whole engine (full mode)
if [ "$ISOLATED" != 1 ] && [ $# -eq 0 ] && [ "${J6_RENDER:-1}" = 1 ]; then
    echo "--> render"
    CC="$CC" J6_CFLAGS="$CFLAGS" sh tools/j6_render.sh || fail render
fi

echo "==> J-6 engine tests: $fails failure(s)${failed_list:+:$failed_list}"
[ "$fails" -eq 0 ]
