#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 William Kew, ShinTech Electronics
"""CHORD mode's styles: tools/shintech/styles/<n>-<code>.json -> build/gen/chord_styles.h.

A style file holds one of the nine styles and its twelve variations:

    {"style": 3, "code": "BEAT1", "name": "BEAT 1", "variations": [
        {"name": "TRESILLO", "rate": "1/16", "steps": 16, "events": ["0 A 112 .75", "3 A 84 .75", ...]}, ...]}

A variation is a loop of `steps` steps of `rate` (1/2 1/4 1/8 1/16 1/32, triplets 4T 8T 16T 32T); optional
"oct" (1..3: the octaves the arp tokens climb, default 1) and "strum" (ms between a strum's notes, 0..63, default
0). Each event is "<step> <notes> <velocity> <length> [d|u]":
    step      0 .. steps - 1
    notes     relative to the chord playing (its notes low to high, the bass first when it has one):
              A all; B the lowest; U all but the lowest; 0..15 the n-th from the bottom (past the top: the
              chord again an octave up); -1..-8 the n-th from the top (-1 the top; past the bottom: an octave
              down); ^ v ~ ? the arp's next note UP, DOWN, UP/DOWN or at random (over "oct" octaves of the
              chord). A suffix ' raises an octave ('' two), , lowers one: "0'", "A,".
    velocity  1..127 (the keys' velocity 100 plays it as it is; MIDI IN's scales it)
    length    in steps, a multiple of 0.25 (0.25 .. 16)
    d / u     a strum, down (low to high) or up (high to low), "strum" ms between the notes (the track's
              STRUM may set its own); without: every note at once
Names: the style's code (the STYLE knob, at most 5 characters) and name, and each variation's name: upper-case
ASCII, at most 10. A variation has at most 32 events.

The C table: CST_VAR[9 * 12] {first event, events, steps, rate, strum, oct}, CST_EV[] 4 bytes an event:
step | notes (bits 0..5) + octave (6..7: 0, +1, -1, +2) | velocity | length in quarter steps - 1 (0..5) + strum
(6..7: 0 none, 1 down, 2 up).
"""
import argparse
import json
import re
import sys
from fractions import Fraction
from pathlib import Path

NSTY = 9
NVAR = 12
NAME_LEN = 10                                  # a variation's / style's name (the STYLE page's width)
CODE_LEN = 5                                   # the STYLE knob's value
MAX_EV = 32
MAX_STEPS = 64
# rate -> (index, a step's length as quarter notes: num / den); the index is style.c's CST_RATE order
RATES = {"1/2": (0, 2, 1), "1/4": (1, 1, 1), "1/8": (2, 1, 2), "1/16": (3, 1, 4), "1/32": (4, 1, 8),
         "4T": (5, 2, 3), "8T": (6, 1, 3), "16T": (7, 1, 6), "32T": (8, 1, 12)}
SEL_A, SEL_B, SEL_U = 32, 33, 34
ARPS = {"^": 40, "v": 41, "~": 42, "?": 43}
OCT_CODE = {0: 0, 1: 1, -1: 2, 2: 3}
STRUM = {None: 0, "d": 1, "u": 2}


class StyleError(ValueError):
    pass


def parse_notes(s):
    """the notes token -> (selector code, octave shift)"""
    m = re.fullmatch(r"([ABU^v~?]|-?\d+)('{0,2}|,?)", s)
    if not m:
        raise StyleError(f"notes {s!r}: A B U, 0..15, -1..-8 or ^ v ~ ?, then ' '' or ,")
    t, o = m.groups()
    shift = len(o) if o.startswith("'") else -1 if o == "," else 0
    if t in "ABU":
        code = {"A": SEL_A, "B": SEL_B, "U": SEL_U}[t]
    elif t in ARPS:
        code = ARPS[t]
    else:
        n = int(t)
        if 0 <= n <= 15:
            code = n
        elif -8 <= n <= -1:
            code = 15 - n                      # -1 -> 16 .. -8 -> 23
        else:
            raise StyleError(f"notes {s!r}: an index is 0..15 from the bottom or -1..-8 from the top")
    return code, shift


def parse_event(text, steps):
    f = text.split()
    if len(f) not in (4, 5):
        raise StyleError(f"event {text!r}: \"<step> <notes> <velocity> <length> [d|u]\"")
    try:
        step, vel, ln = int(f[0]), int(f[2]), Fraction(f[3])
    except ValueError:
        raise StyleError(f"event {text!r}: step, velocity and length are numbers")
    if not 0 <= step < steps:
        raise StyleError(f"event {text!r}: step {step} outside 0..{steps - 1}")
    code, shift = parse_notes(f[1])
    if not 1 <= vel <= 127:
        raise StyleError(f"event {text!r}: velocity 1..127")
    q = ln * 4
    if q.denominator != 1 or not 1 <= q <= 64:
        raise StyleError(f"event {text!r}: length 0.25..16 steps, in quarters of a step")
    strum = f[4] if len(f) == 5 else None
    if strum not in STRUM:
        raise StyleError(f"event {text!r}: strum d or u")
    if strum and code not in (SEL_A, SEL_U):
        raise StyleError(f"event {text!r}: a strum plays several notes: A or U")
    return step, code, shift, vel, int(q), strum


def load_styles(src):
    files = sorted(Path(src).glob("*.json"))
    styles = {}
    for f in files:
        d = json.loads(f.read_text(encoding="utf-8"))
        n = d.get("style")
        where = f.name
        if not isinstance(n, int) or not 1 <= n <= NSTY or n in styles:
            raise SystemExit(f"{where}: style {n!r}: 1..{NSTY}, each once")
        code, name = d.get("code", ""), d.get("name", "")
        if not re.fullmatch(r"[A-Z0-9]{1,%d}" % CODE_LEN, code):
            raise SystemExit(f"{where}: code {code!r}: 1..{CODE_LEN} upper-case letters and digits")
        if not re.fullmatch(r"[A-Z0-9 &'.\-+#/]{1,%d}" % NAME_LEN, name):
            raise SystemExit(f"{where}: name {name!r}: 1..{NAME_LEN} upper-case letters, digits, space &'.-+#/")
        vs = d.get("variations", [])
        if len(vs) != NVAR:
            raise SystemExit(f"{where}: {len(vs)} variations, a style has {NVAR}")
        out = []
        for k, v in enumerate(vs):
            vw = f"{where}: variation {k + 1} {v.get('name')!r}"
            if not re.fullmatch(r"[A-Z0-9 &'.\-+#/]{1,%d}" % NAME_LEN, v.get("name", "")):
                raise SystemExit(f"{vw}: a name is 1..{NAME_LEN} upper-case letters, digits, space &'.-+#/")
            if v.get("rate") not in RATES:
                raise SystemExit(f"{vw}: rate one of {' '.join(RATES)}")
            steps, octs, strum = v.get("steps"), v.get("oct", 1), v.get("strum", 0)
            if not isinstance(steps, int) or not 1 <= steps <= MAX_STEPS:
                raise SystemExit(f"{vw}: steps 1..{MAX_STEPS}")
            if octs not in (1, 2, 3):
                raise SystemExit(f"{vw}: oct 1..3")
            if not isinstance(strum, int) or not 0 <= strum <= 63:
                raise SystemExit(f"{vw}: strum 0..63 ms")
            evs = []
            for e in v.get("events", []):
                try:
                    evs.append(parse_event(e, steps))
                except StyleError as x:
                    raise SystemExit(f"{vw}: {x}")
            if not 1 <= len(evs) <= MAX_EV:
                raise SystemExit(f"{vw}: 1..{MAX_EV} events")
            evs.sort(key=lambda e: e[0])           # by step (stable: the file's order within a step)
            out.append({"name": v["name"], "rate": RATES[v["rate"]][0], "rname": v["rate"], "steps": steps,
                        "oct": octs, "strum": strum, "events": evs})
        names = [v["name"] for v in out]
        if len(set(names)) != len(names):
            raise SystemExit(f"{where}: a variation name used twice")
        styles[n] = {"code": code, "name": name, "vars": out}
    if sorted(styles) != list(range(1, NSTY + 1)):
        raise SystemExit(f"gen_styles: styles {sorted(set(range(1, NSTY + 1)) - set(styles))} missing in {src}")
    return [styles[i] for i in range(1, NSTY + 1)]


def encode(e):
    step, code, shift, vel, q, strum = e
    return (step, code | OCT_CODE[shift] << 6, vel, (q - 1) | STRUM[strum] << 6)


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def write_header(out, styles):
    L = ["/* generated by tools/shintech/gen_styles.py from tools/shintech/styles/<n>-<code>.json: do not edit */",
         "#ifndef CHORD_STYLES_H", "#define CHORD_STYLES_H",
         f"#define CST_NSTY {NSTY}u", f"#define CST_NVAR {NVAR}u", f"#define CST_NAME_LEN {NAME_LEN}u",
         "#define CST_CODES " + ", ".join(c_str(s["code"]) for s in styles) + "   /* (params.c N_CSTY) */",
         "static const char *const CST_NAME[CST_NSTY] = {" + ", ".join(c_str(s["name"]) for s in styles) + "};",
         "typedef struct { uint16_t ev; uint8_t nev, steps, rate, strum, oct, rsv; char name[CST_NAME_LEN]; } cst_var_t;",
         "static const cst_var_t CST_VAR[CST_NSTY * CST_NVAR] = {"]
    evs, first = [], 0
    for s in styles:
        for v in s["vars"]:
            L.append(f"    {{{first}, {len(v['events'])}, {v['steps']}, {v['rate']}, {v['strum']}, {v['oct']}, 0, "
                     f"{c_str(v['name'])}}},")
            evs += [encode(e) for e in v["events"]]
            first += len(v["events"])
    L += ["};", f"#define CST_NEV {len(evs)}u", "static const uint8_t CST_EV[CST_NEV][4] = {"]
    for i in range(0, len(evs), 6):
        L.append("    " + " ".join("{%d, %d, %d, %d}," % e for e in evs[i:i + 6]))
    L += ["};", "#endif"]
    Path(out).write_text("\n".join(L) + "\n", encoding="ascii")
    return len(evs)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("out", help="the C header to write (build/gen/chord_styles.h)")
    ap.add_argument("--src", default=str(Path(__file__).resolve().parent / "styles"))
    a = ap.parse_args(argv)
    styles = load_styles(a.src)
    nev = write_header(a.out, styles)
    size = NSTY * NVAR * (8 + NAME_LEN) + nev * 4
    print(f"chord styles: {NSTY} styles x {NVAR} variations, {nev} events, {size} B -> {a.out}")


if __name__ == "__main__":
    sys.exit(main())
