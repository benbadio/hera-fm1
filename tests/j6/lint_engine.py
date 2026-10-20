#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Lint the J-6 engine sources (firmware/src/j6) for the firmware rules a host compiler cannot see.

    python3 -I tests/j6/lint_engine.py [--platform-root firmware] FILE...

Checks, on the source with comments and string literals removed:
  1. no float / double / long double, no floating-point literal (1.0, .5, 1e3, 2.f);
  2. no malloc / calloc / realloc / free / alloca, no stdio;
  3. #include only "j6*.h" (same directory), <stdint.h> and <stddef.h>;
  4. single translation unit naming (docs/J6_ENGINE_DESIGN.md 9):
     - in a module .c file, file-scope static names, struct / enum tags, enumerators and macros carry the
       module's private prefix (j6c_ chord, j6p_ phrase, j6s_ seq, j6y_ synth, j6e_ glue; macros J6C_ ...);
       non-static file-scope names are public: j6_ / J6_;
     - in a header every such name starts with j6 / J6;
     - no identifier may be a macro the platform defines (FS, MQ, RGB, KEY_LED ...);
  5. no mutable static or global state outside the glue (j6_engine*.c): file-scope objects and static
     locals must be const.
Exit status 1 if anything is found; each finding is printed as file:line: message.
"""
import os
import re
import sys

PRIVATE = [("j6_chord", "j6c"), ("j6_phrase", "j6p"), ("j6_seq", "j6s"), ("j6_synth", "j6y"),
           ("j6_engine", "j6e")]
ALLOWED_SYS_INCLUDES = {"stdint.h", "stddef.h"}
FORBIDDEN_WORDS = {
    "float": "no float in engine code (fixed point only)",
    "double": "no double in engine code (fixed point only)",
    "malloc": "no malloc: fixed-size static storage only",
    "calloc": "no calloc: fixed-size static storage only",
    "realloc": "no realloc: fixed-size static storage only",
    "free": "no free: fixed-size static storage only",
    "alloca": "no alloca",
    "printf": "no stdio in engine code",
    "fprintf": "no stdio in engine code",
    "sprintf": "no stdio in engine code",
    "snprintf": "no stdio in engine code",
    "puts": "no stdio in engine code",
    "FILE": "no stdio in engine code",
    "__int128": "no 128-bit integers",
}
PLATFORM_SOURCES = ["src/platform.c", "src/libc.c", "src/gfx.c", "src/lcd.c", "src/usb.c", "src/ota.c",
                    "src/recovery.c", "src/bootguard.h", "src/uac_fir.h"]
KEYWORDS = {
    "auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else", "enum",
    "extern", "float", "for", "goto", "if", "inline", "int", "long", "register", "restrict", "return",
    "short", "signed", "sizeof", "static", "struct", "switch", "typedef", "union", "unsigned", "void",
    "volatile", "while", "_Bool", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "int8_t", "int16_t",
    "int32_t", "int64_t", "size_t", "ptrdiff_t", "uintptr_t", "intptr_t",
}


def strip(src):
    """Blank comments, string and char literals (keeping newlines and column positions)."""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", src[i:j]))
            i = j
        elif src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and src[j] != c:
                if src[j] == "\\":
                    j += 1
                if j < n and src[j] == "\n":
                    break
                j += 1
            j = min(j + 1, n)
            out.append(c + " " * max(0, j - i - 2) + (c if j - i >= 2 else ""))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def platform_macros(root):
    names = set()
    paths = [os.path.join(root, p) for p in PLATFORM_SOURCES]
    hal = os.path.join(root, "hal")
    if os.path.isdir(hal):
        paths += [os.path.join(hal, f) for f in sorted(os.listdir(hal)) if f.endswith(".h")]
    host = os.path.join(root, "..", "tests", "host.c")
    paths.append(host)
    for p in paths:
        try:
            with open(p, encoding="utf-8", errors="replace") as f:
                for m in re.finditer(r"^\s*#\s*define\s+([A-Za-z_]\w*)", f.read(), re.M):
                    names.add(m.group(1))
        except OSError:
            pass
    return {x for x in names if not x.upper().startswith("J6")}


TOKEN = re.compile(r"0[xX][0-9a-fA-F]+[uUlL]*|(?:\d+\.\d*|\.\d+|\d+)(?:[eE][+-]?\d+)?[fFuUlL]*|[A-Za-z_]\w*|\S")
FLOAT_LIT = re.compile(r"^(?:\d+\.\d*|\.\d+)(?:[eE][+-]?\d+)?[fFlL]?$|^\d+[eE][+-]?\d+[fFlL]?$")


def tokens(text):
    """(token, line) pairs; preprocessor lines are left out (they are checked separately)."""
    res = []
    for ln, line in enumerate(text.split("\n"), 1):
        if line.lstrip().startswith("#"):
            continue
        for m in TOKEN.finditer(line):
            res.append((m.group(0), ln))
    return res


def lint(path, plat_macros):
    errs = []
    base = os.path.basename(path)
    is_header = base.endswith(".h")
    is_glue = base.startswith("j6_engine")
    priv = None
    for pre, p in PRIVATE:
        if base.startswith(pre):
            priv = p
    if is_header or priv is None:
        priv = "j6"

    def err(line, msg):
        errs.append("%s:%d: %s" % (path, line, msg))

    with open(path, encoding="utf-8", errors="replace") as f:
        raw = f.read()
    text = strip(raw)
    # join continued preprocessor lines for the directive checks, keeping line numbers of the start
    lines = text.split("\n")

    def ok_name(name, public_ok):
        low = name.lower()
        if is_header:
            return low.startswith("j6")
        if public_ok and low.startswith("j6_"):
            return True
        return low.startswith(priv + "_")

    # ---- preprocessor directives
    for ln, line in enumerate(lines, 1):
        s = line.strip()
        if not s.startswith("#"):
            continue
        m = re.match(r"#\s*include\s*([<\"])([^>\"]*)[>\"]", raw.split("\n")[ln - 1].strip())
        if m:
            kind, name = m.group(1), m.group(2)
            if kind == "<" and name not in ALLOWED_SYS_INCLUDES:
                err(ln, "#include <%s>: engine code may only include <stdint.h> and <stddef.h>" % name)
            if kind == '"' and (not re.match(r"^j6\w*\.h$", name)):
                err(ln, "#include \"%s\": engine code may only include j6*.h from firmware/src/j6" % name)
            continue
        m = re.match(r"#\s*define\s+([A-Za-z_]\w*)", s)
        if m:
            name = m.group(1)
            if not (name.upper().startswith(priv.upper() + "_") or (is_header and name.upper().startswith("J6"))):
                if not (name.upper().startswith("J6_") and not is_header and priv == "j6"):
                    err(ln, "macro %s: file-local macros start with %s_ (single translation unit)" % (name, priv.upper()))
            if name in plat_macros:
                err(ln, "macro %s is also defined by the platform" % name)

    # ---- tokens
    toks = tokens(text)
    for t, ln in toks:
        if t in FORBIDDEN_WORDS:
            err(ln, "%s: %s" % (t, FORBIDDEN_WORDS[t]))
        elif FLOAT_LIT.match(t):
            err(ln, "floating-point literal %s: use integers / fixed point" % t)
        elif t in plat_macros:
            err(ln, "identifier %s is a platform macro: rename it" % t)

    # ---- declarations: a small brace-depth scanner over the file-scope token stream
    stack = []          # kinds of open braces: 'enum', 'struct', 'func', 'init', 'block'
    decl = []           # tokens of the file-scope declaration being read ('{}' marks a skipped body)
    expect_enumerator = False
    for i, (t, ln) in enumerate(toks):
        depth = len(stack)
        if t == "{":
            if depth == 0:
                words = [x for x, _ in decl]
                if "=" in words:
                    kind = "init"
                elif any(x in ("enum", "struct", "union") for x in words[-2:]):
                    kind = "enum" if "enum" in words[-2:] else "struct"
                elif ")" in words:
                    kind = "func"
                else:
                    kind = "block"
                if kind == "func":
                    check_decl(decl, err, ok_name, is_glue, is_header, func_body=True)
                    decl = []
                elif kind in ("enum", "struct"):
                    decl.append(("{}", ln))
            else:
                prev = [x for x, _ in toks[max(0, i - 2):i]]
                kind = "enum" if "enum" in prev else "struct" if ("struct" in prev or "union" in prev) else "block"
                if stack[-1] == "init":
                    kind = "init"
            stack.append(kind)
            expect_enumerator = kind == "enum"
            continue
        if t == "}":
            if stack:
                stack.pop()
            expect_enumerator = False
            continue
        if stack and stack[-1] == "enum":
            if expect_enumerator and re.match(r"^[A-Za-z_]\w*$", t):
                if len(stack) == 1 and not ok_name(t, True):
                    err(ln, "enumerator %s: use the prefix %s_ (J6_ for public ones)" % (t, priv.upper()))
                expect_enumerator = False
            elif t == ",":
                expect_enumerator = True
            continue
        if depth == 0:
            if t == ";":
                check_decl(decl, err, ok_name, is_glue, is_header, func_body=False)
                decl = []
            else:
                decl.append((t, ln))
        elif stack[-1] in ("func", "block") and t == "static" and not is_glue:
            words = []
            for x, _ in toks[i + 1:]:
                if x in (";", "=", "("):
                    break
                words.append(x)
            if "const" not in words:
                err(ln, "mutable static local: keep all state in the instance struct")
    return errs


IDENT = re.compile(r"^[A-Za-z_]\w*$")


def check_decl(decl, err, ok_name, is_glue, is_header, func_body):
    """One file-scope declaration (tokens up to ';' or a function body's '{')."""
    if not decl:
        return
    words = [x for x, _ in decl]
    ln = decl[0][1]
    # tags being defined: struct / enum / union TAG {}
    for k in range(len(words) - 2):
        if words[k] in ("struct", "enum", "union") and IDENT.match(words[k + 1]) and words[k + 2] == "{}":
            if not ok_name(words[k + 1], True):
                err(ln, "%s %s: tags need the module prefix (single translation unit)" % (words[k], words[k + 1]))
    if words[0] == "typedef":
        name = None
        for k in range(len(words) - 2):            # typedef R (*NAME)(...)
            if words[k] == "(" and words[k + 1] == "*" and IDENT.match(words[k + 2]):
                name = words[k + 2]
                break
        if name is None:
            names = [x for x in words if IDENT.match(x) and x not in KEYWORDS]
            name = names[-1] if names else None
        if name and not ok_name(name, True):
            err(ln, "typedef %s: use the module prefix" % name)
        return
    if "{}" in words:                              # struct / enum definition, maybe with declarators
        rest = words[len(words) - words[::-1].index("{}"):]
        if not any(IDENT.match(x) for x in rest):
            return
        head = [x for x in words[:words.index("{}")]]
        words = head + rest
    is_static = "static" in words
    is_extern = "extern" in words
    is_const = "const" in words
    func = False
    name = None
    if "(" in words and ("=" not in words or words.index("(") < words.index("=")):
        k = words.index("(")
        if k + 2 < len(words) and words[k + 1] == "*" and IDENT.match(words[k + 2]):
            name = words[k + 2]                    # R (*name)(...): a function pointer object
        else:
            name = words[k - 1] if k > 0 else None
            func = True
    else:
        for k, w in enumerate(words):
            if w in ("[", "=", ","):
                name = words[k - 1] if k > 0 else None
                break
        if name is None:
            name = words[-1]
    if not name or not IDENT.match(name) or name in KEYWORDS:
        return
    if not ok_name(name, not is_static):
        err(ln, "%s %s: file-scope names need the module prefix (single translation unit)" %
            ("static" if is_static else "public", name))
    if not func and not is_extern and not is_const:
        if is_header:
            err(ln, "%s: headers may not define mutable objects" % name)
        elif not is_glue:
            err(ln, "%s: mutable file-scope object; keep state in the instance struct" % name)


def main(argv):
    root = "firmware"
    files = []
    k = 1
    while k < len(argv):
        if argv[k] == "--platform-root":
            root = argv[k + 1]
            k += 2
            continue
        files.append(argv[k])
        k += 1
    plat = platform_macros(root)
    errs = []
    for f in files:
        errs += lint(f, plat)
    for e in errs:
        print(e)
    if errs:
        print("lint: %d finding(s) in %d file(s)" % (len(errs), len(files)))
        return 1
    print("lint: %d file(s) clean" % len(files))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
