# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 William Kew, ShinTech Electronics
"""CHORD mode's chord set data: tools/shintech/gen_chords.py (the symbols, the checks, the C table) and the names the
firmware gives the factory chords (tests/chordset_test.c writes them: CHORDSET_NAMES=path), each read back to its
notes. python3 -m unittest tests/shintech/test_chordsets.py (from the tree's root)"""
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "shintech"))
import gen_chords as g  # noqa: E402

P5 = 1 << 7


def chord(sym):
    root, iv, bass, voicing, inv = g.parse_chord(sym)
    return root, iv, bass


class Symbols(unittest.TestCase):
    def test_qualities(self):
        cases = {
            "C": [0, 4, 7], "Am": [0, 3, 7], "Bb7": [0, 4, 7, 10], "Fmaj7": [0, 4, 7, 11], "FM7": [0, 4, 7, 11],
            "Dm7": [0, 3, 7, 10], "DmM7": [0, 3, 7, 11], "Fmaj9": [0, 4, 7, 11, 14], "Dm11": [0, 3, 7, 10, 14, 17],
            "G13": [0, 4, 7, 10, 14, 21], "G7sus4": [0, 5, 7, 10], "Csus2": [0, 2, 7], "Csus": [0, 5, 7],
            "Bm7b5": [0, 3, 6, 10], "Bø": [0, 3, 6, 10], "Bdim7": [0, 3, 6, 9], "Bdim": [0, 3, 6],
            "Caug": [0, 4, 8], "C+": [0, 4, 8], "C5": [0, 7], "C6": [0, 4, 7, 9], "C69": [0, 4, 7, 9, 14],
            "C6/9": [0, 4, 7, 9, 14], "Cadd9": [0, 4, 7, 14], "Cmadd9": [0, 3, 7, 14], "E7#9": [0, 4, 7, 10, 15],
            "A7b9": [0, 4, 7, 10, 13], "C13b9": [0, 4, 7, 10, 13, 21], "Gmaj7#11": [0, 4, 7, 11, 18],
            "C7no3": [0, 7, 10], "Cm(maj7)": [0, 3, 7, 11], "C-7": [0, 3, 7, 10],
        }
        for sym, iv in cases.items():
            with self.subTest(sym=sym):
                self.assertEqual(chord(sym)[1], iv)

    def test_roots_and_bass(self):
        self.assertEqual(chord("F#m")[0], 6)
        self.assertEqual(chord("Gb")[0], 6)
        self.assertEqual(chord("Cb")[0], 11)
        self.assertEqual(chord("Am/G")[2], 7)
        self.assertEqual(chord("C/C")[2], None)                    # the root as its bass: no slash
        self.assertEqual(chord("Cm6/9")[1:], ([0, 3, 7, 9, 14], None))

    def test_attributes(self):
        self.assertEqual(g.parse_chord("Fmaj9 open i1")[3:], (1, 1))
        self.assertEqual(g.parse_chord("Dm spread")[3:], (2, 0))
        for bad in ("C wide", "C i4", "C5 i2"):
            with self.subTest(bad=bad):
                self.assertRaises(g.ChordError, g.parse_chord, bad)

    def test_too_many_notes(self):
        self.assertEqual(chord("C13#11")[1], [0, 4, 10, 14, 18, 21])   # 7 notes: the fifth dropped
        self.assertRaises(g.ChordError, g.parse_chord, "C13#11b9#9")

    def test_errors(self):
        for bad in ("H", "Cxyz", "C/H", ""):
            with self.subTest(bad=bad):
                self.assertRaises(g.ChordError, g.parse_chord, bad)


class Files(unittest.TestCase):
    def write(self, d, name, data):
        (Path(d) / name).write_text(json.dumps(data), encoding="utf-8")

    def sets_ok(self, n=1, name="A"):
        return {"genre": "POP", "sets": [{"name": f"{name}{i}", "chords": ["C"] * 12} for i in range(n)]}

    def test_checks(self):
        bad = [
            {"genre": "POLKA", "sets": []},
            {"genre": "POP", "sets": [{"name": "SHORT", "chords": ["C"] * 11}]},
            {"genre": "POP", "sets": [{"name": "lower", "chords": ["C"] * 12}]},
            {"genre": "POP", "sets": [{"name": "THIRTEEN CHAR", "chords": ["C"] * 12}]},
            {"genre": "POP", "sets": [{"name": "X", "chords": ["C"] * 11 + ["Q"]}]},
        ]
        for data in bad:
            with self.subTest(data=data), tempfile.TemporaryDirectory() as d:
                self.write(d, "a.json", data)
                self.assertRaises(SystemExit, g.load_sets, d)
        with tempfile.TemporaryDirectory() as d:          # one name in two files
            self.write(d, "a.json", self.sets_ok())
            self.write(d, "b.json", self.sets_ok())
            self.assertRaises(SystemExit, g.load_sets, d)

    def test_genre_order_and_shapes(self):
        with tempfile.TemporaryDirectory() as d:
            self.write(d, "a.json", {"genre": "JAZZ", "sets": [{"name": "J", "chords": ["Cmaj7"] * 6 + ["Dm7"] * 6}]})
            self.write(d, "b.json", {"genre": "POP", "sets": [{"name": "P", "chords": ["C"] * 11 + ["Am/G"]}]})
            sets = g.load_sets(d)
            self.assertEqual([s["name"] for s in sets], ["P", "J"])     # POP before JAZZ, whatever the files
            shapes, rows = g.build(sets)
            self.assertEqual(len(shapes), 4)                             # major, minor, maj7, m7
            self.assertEqual(rows[0][11], (9, 1, 8))                     # A, the minor shape, bass G + 1
            out = Path(d) / "cs.h"
            g.write_header(out, sets, shapes, rows)
            h = out.read_text()
            self.assertIn("#define CS_NSETS 2u", h)
            self.assertIn('{"P", 0, {', h)

    def test_factory_data(self):
        sets = g.load_sets(ROOT / "tools" / "shintech" / "chordsets")
        self.assertGreaterEqual(len(sets), 100)
        self.assertEqual({s["genre"] for s in sets}, set(range(len(g.GENRES))))   # every genre has sets
        built = ROOT / "build" / "gen" / "chord_sets.h"
        if built.exists():                                               # the build's table is today's data
            with tempfile.TemporaryDirectory() as d:
                g.write_header(Path(d) / "cs.h", sets, *g.build(sets))
                self.assertEqual((Path(d) / "cs.h").read_text(), built.read_text())


@unittest.skipUnless(os.environ.get("CHORDSET_NAMES"), "CHORDSET_NAMES: the names tests/chordset_test.c wrote")
class FirmwareNames(unittest.TestCase):
    def test_names_read_back(self):
        lines = Path(os.environ["CHORDSET_NAMES"]).read_text().split("\n")
        n = 0
        for ln in filter(None, lines):
            s, k, root, mask, bass1, name = ln.split()
            with self.subTest(set=s, key=k, name=name):
                r, iv, b = chord(name)
                self.assertEqual(r, int(root))
                self.assertEqual(g.pc_mask(iv) & ~P5, int(mask) & ~P5)   # a fifth left out is not named
                self.assertEqual(-1 if b is None else b, int(bass1) - 1)
            n += 1
        self.assertGreaterEqual(n, 1200)


if __name__ == "__main__":
    unittest.main()
