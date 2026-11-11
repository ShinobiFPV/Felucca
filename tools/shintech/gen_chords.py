#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 William Kew, ShinTech Electronics
"""CHORD mode's chord sets: tools/shintech/chordsets/<genre>.json -> build/gen/chord_sets.h.

A set file holds one genre:

    {"genre": "POP", "sets": [{"name": "FIRST LIGHT", "chords": ["C", "G/B", "Am7", "Fmaj7", ...]}]}

Each set has exactly 12 chords: chord k plays on key k of the CHORD octave (C4..B4). A chord is a symbol, optionally
followed by its default voicing (close, open, spread) and inversion (i0..i3), space-separated: "Fmaj9 open i1".
Symbols (the root: A..G with # or b):
    quality   m min - (minor), dim o, dim7 o7, m7b5 ø (half-diminished), aug + (#5), sus2, sus4 sus, 5 (power)
    sevenths  7 (dominant), maj7 M7 (major), mM7 (minor-major); 9 11 13 after any of them (maj9, m11, 13 ...):
              9 = 7 + 9, 11 = 7 + 9 + 11, 13 = 7 + 9 + 13
    sixths    6, 69 (6 + 9)
    adds      add9 add2 add11 add4 add13
    altered   b5 #5 b9 #9 #11 b13, no3 no5
    bass      /E (any note: a slash chord's bass, below the voicing)
Spellings are free: the firmware names each chord again from its notes (chordset.c cs_name), so "Gb" may show as
"F#". A chord has at most CS_MAXN notes above its bass; past that the fifth, then the 11th are dropped.

The C table: CS_SHAPE[] the distinct interval sets (semitones above the root, ascending, -1 padded), CS_SET[] each
set's name, genre and 12 chords of 3 bytes: root (bits 0..3) | voicing (4..5) | inversion (6..7), shape index,
bass (0: none, else 1 + its pitch class). --names writes, for the tests, one line a chord: set, key, root, shape's
pitch-class mask, bass + 1, symbol.
"""
import argparse
import json
import re
import sys
from pathlib import Path

CS_MAXN = 6                                    # notes above the bass (CHORD_SET_MAX in chordset.c)
NAME_LEN = 12                                  # a set's name: upper-case ASCII, at most 12 (the screen's width)
GENRES = ["POP", "SYNTHWAVE", "HOUSE", "TECHNO", "R&B", "NEO SOUL", "JAZZ", "LO-FI", "CINEMATIC", "FUNK", "GOSPEL",
          "MINIMAL"]
VOICINGS = {"close": 0, "open": 1, "spread": 2}
NOTE = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}


class ChordError(ValueError):
    pass


def parse_note(s):
    if not s or s[0] not in NOTE:
        raise ChordError(f"no note in {s!r}")
    n, i = NOTE[s[0]], 1
    while i < len(s) and s[i] in "#b":
        n += 1 if s[i] == "#" else -1
        i += 1
    return n % 12, s[i:]


# the quality after the root -> its intervals above the root. Tokens are read left to right
TOKENS = [
    ("m7b5", lambda c: c.update(third=3, fifth=6, seventh=10)),
    ("ø", lambda c: c.update(third=3, fifth=6, seventh=10)),
    ("dim7", lambda c: c.update(third=3, fifth=6, seventh=9)),
    ("o7", lambda c: c.update(third=3, fifth=6, seventh=9)),
    ("dim", lambda c: c.update(third=3, fifth=6)),
    ("mM", lambda c: c.update(third=3, major7=True)),
    ("maj", lambda c: c.update(major7=True)),
    ("min", lambda c: c.update(third=3)),
    ("aug", lambda c: c.update(fifth=8)),
    ("sus2", lambda c: c.update(third=None, sus=2)),
    ("sus4", lambda c: c.update(third=None, sus=5)),
    ("sus", lambda c: c.update(third=None, sus=5)),
    ("add9", lambda c: c["add"].add(14)),
    ("add2", lambda c: c["add"].add(14)),
    ("add11", lambda c: c["add"].add(17)),
    ("add4", lambda c: c["add"].add(17)),
    ("add13", lambda c: c["add"].add(21)),
    ("no3", lambda c: c.update(third=None)),
    ("no5", lambda c: c.update(fifth=None)),
    ("b5", lambda c: c.update(fifth=6)),
    ("#5", lambda c: c.update(fifth=8)),
    ("b9", lambda c: c["add"].add(13)),
    ("#9", lambda c: c["add"].add(15)),
    ("#11", lambda c: c["add"].add(18)),
    ("b13", lambda c: c["add"].add(20)),
    ("69", lambda c: c["add"].update({9, 14})),
    ("13", lambda c: c.update(num=13)),
    ("11", lambda c: c.update(num=11)),
    ("9", lambda c: c.update(num=9)),
    ("7", lambda c: c.update(num=7)),
    ("6", lambda c: c["add"].add(9)),
    ("5", lambda c: c.update(third=None, power=True)),
    ("M", lambda c: c.update(major7=True)),
    ("Δ", lambda c: c.update(major7=True)),
    ("m", lambda c: c.update(third=3)),
    ("-", lambda c: c.update(third=3)),
    ("o", lambda c: c.update(third=3, fifth=6)),
    ("+", lambda c: c.update(fifth=8)),
    ("(", lambda c: None),
    (")", lambda c: None),
]


def parse_quality(q):
    """the intervals (semitones above the root, ascending) of a chord's quality"""
    c = {"third": 4, "fifth": 7, "seventh": None, "num": None, "major7": False, "sus": None, "power": False,
         "add": set()}
    s = q
    while s:
        for tok, act in TOKENS:
            if s.startswith(tok):
                act(c)
                s = s[len(tok):]
                break
        else:
            raise ChordError(f"cannot read {s!r} in {q!r}")
    num = c["num"]
    if num or c["major7"]:
        if c["seventh"] is None:
            c["seventh"] = 11 if c["major7"] else 10
        num = num or 7
    elif c["seventh"] is not None:
        num = 7                                    # dim7, m7b5
    iv = {0}
    if c["third"] is not None:
        iv.add(c["third"])
    if c["sus"]:
        iv.add(c["sus"])
    if c["fifth"] is not None:
        iv.add(c["fifth"])
    if c["seventh"] is not None:
        iv.add(c["seventh"])
    if num and num >= 9:
        iv.add(14)
    if num == 11:
        iv.add(17)
    if num == 13:
        iv.add(21)
    alt9 = {13, 15} & c["add"]
    if alt9 and num and num >= 9:
        iv.discard(14)                             # 7b9 / 7#9 written as 9-chords: the altered ninth only
    iv |= c["add"]
    if c["power"]:
        iv = {0, 7}
    iv = sorted(iv)
    for drop in (7, 17):                           # too many: the fifth, then the 11th
        if len(iv) > CS_MAXN and drop in iv and drop != iv[0]:
            iv.remove(drop)
    if len(iv) > CS_MAXN:
        raise ChordError(f"{q!r}: {len(iv)} notes, at most {CS_MAXN}")
    return iv


def parse_chord(text):
    """'Fmaj9/A open i1' -> (root, intervals, bass pitch class or None, voicing, inversion)"""
    parts = text.split()
    if not parts:
        raise ChordError("empty chord")
    sym, attrs = parts[0], parts[1:]
    root, rest = parse_note(sym)
    bass = None
    rest = rest.replace("6/9", "69")
    if "/" in rest:
        rest, b = rest.rsplit("/", 1)
        bass, tail = parse_note(b)
        if tail:
            raise ChordError(f"{sym!r}: bass {b!r}")
    iv = parse_quality(rest)
    voicing, inv = 0, 0
    for a in attrs:
        if a in VOICINGS:
            voicing = VOICINGS[a]
        elif re.fullmatch(r"i[0-3]", a):
            inv = int(a[1])
        else:
            raise ChordError(f"{text!r}: unknown {a!r} (close open spread, i0..i3)")
    if inv >= len(iv):
        raise ChordError(f"{text!r}: inversion {inv} of {len(iv)} notes")
    if bass == root:
        bass = None
    return root, iv, bass, voicing, inv


def pc_mask(iv):
    m = 0
    for i in iv:
        m |= 1 << (i % 12)
    return m


def load_sets(src):
    files = sorted(Path(src).glob("*.json"))
    if not files:
        raise SystemExit(f"gen_chords: no chord set files in {src}")
    sets = []
    for f in files:
        d = json.loads(f.read_text(encoding="utf-8"))
        g = d.get("genre")
        if g not in GENRES:
            raise SystemExit(f"{f.name}: genre {g!r} is not one of {', '.join(GENRES)}")
        for s in d["sets"]:
            name = s["name"]
            where = f"{f.name}: set {name!r}"
            if not re.fullmatch(r"[A-Z0-9 &'.\-+#!]{1,%d}" % NAME_LEN, name):
                raise SystemExit(f"{where}: a name is 1..{NAME_LEN} upper-case letters, digits, space &'.-+#!")
            if len(s["chords"]) != 12:
                raise SystemExit(f"{where}: {len(s['chords'])} chords, a set has 12")
            chords = []
            for k, c in enumerate(s["chords"]):
                try:
                    chords.append((c,) + parse_chord(c))
                except ChordError as e:
                    raise SystemExit(f"{where}, chord {k + 1}: {e}")
            sets.append({"name": name, "genre": GENRES.index(g), "chords": chords, "file": f.name})
    names = [s["name"] for s in sets]
    dup = {n for n in names if names.count(n) > 1}
    if dup:
        raise SystemExit(f"gen_chords: set names used twice: {', '.join(sorted(dup))}")
    sets.sort(key=lambda s: s["genre"])          # by genre (stable: each file's order kept)
    if len(sets) > 255:
        raise SystemExit("gen_chords: at most 255 sets")
    return sets


def build(sets):
    shapes, index = [], {}
    rows = []
    for s in sets:
        ch = []
        for sym, root, iv, bass, voicing, inv in s["chords"]:
            key = tuple(iv)
            if key not in index:
                index[key] = len(shapes)
                shapes.append(key)
            ch.append((root | voicing << 4 | inv << 6, index[key], 0 if bass is None else bass + 1))
        rows.append(ch)
    if len(shapes) > 256:
        raise SystemExit("gen_chords: more than 256 distinct chord shapes")
    return shapes, rows


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def write_header(out, sets, shapes, rows):
    L = ["/* generated by tools/shintech/gen_chords.py from tools/shintech/chordsets/<genre>.json: do not edit */",
         "#ifndef CHORD_SETS_H", "#define CHORD_SETS_H",
         f"#define CS_NSETS {len(sets)}u",
         f"#define CS_NSHAPES {len(shapes)}u",
         f"#define CS_MAXN {CS_MAXN}u",
         f"#define CS_NGENRES {len(GENRES)}u",
         f"#define CS_NAME_LEN {NAME_LEN}u",
         "static const char *const CS_GENRE[CS_NGENRES] = {" + ", ".join(c_str(g) for g in GENRES) + "};",
         "static const int8_t CS_SHAPE[CS_NSHAPES][CS_MAXN] = {"]
    for sh in shapes:
        L.append("    {" + ", ".join(str(x) for x in list(sh) + [-1] * (CS_MAXN - len(sh))) + "},")
    L += ["};",
          "typedef struct { char name[CS_NAME_LEN]; uint8_t genre; uint8_t ch[12][3]; } cs_set_t;",
          "static const cs_set_t CS_SET[CS_NSETS] = {"]
    for s, ch in zip(sets, rows):
        body = ", ".join("{%d, %d, %d}" % c for c in ch)
        L.append(f"    {{{c_str(s['name'])}, {s['genre']}, {{{body}}}}},")
    L += ["};", "/* the first set of each genre (CS_NSETS: none) */",
          "static const uint8_t CS_GENRE_FIRST[CS_NGENRES] = {"
          + ", ".join(str(next((i for i, s in enumerate(sets) if s["genre"] == g), len(sets)))
                      for g in range(len(GENRES))) + "};", "#endif"]
    Path(out).write_text("\n".join(L) + "\n", encoding="ascii")


def write_names(out, sets):
    L = []
    for i, s in enumerate(sets):
        for k, (sym, root, iv, bass, _v, _i) in enumerate(s["chords"]):
            L.append(f"{i} {k} {root} {pc_mask(iv)} {0 if bass is None else bass + 1} {sym.split()[0]}")
    Path(out).write_text("\n".join(L) + "\n", encoding="utf-8")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("out", help="the C header to write (build/gen/chord_sets.h)")
    ap.add_argument("--src", default=str(Path(__file__).resolve().parent / "chordsets"))
    ap.add_argument("--names", help="also write the chords one a line, for the tests")
    a = ap.parse_args(argv)
    sets = load_sets(a.src)
    shapes, rows = build(sets)
    write_header(a.out, sets, shapes, rows)
    if a.names:
        write_names(a.names, sets)
    per = {}
    for s in sets:
        per[GENRES[s["genre"]]] = per.get(GENRES[s["genre"]], 0) + 1
    size = len(sets) * (NAME_LEN + 1 + 36) + len(shapes) * CS_MAXN
    print(f"chord sets: {len(sets)} sets ({', '.join(f'{g} {n}' for g, n in per.items())}), {len(shapes)} shapes, "
          f"{size} B -> {a.out}")


if __name__ == "__main__":
    sys.exit(main())
