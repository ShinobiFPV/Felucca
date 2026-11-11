# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 William Kew, ShinTech Electronics
"""CHORD mode's styles: tools/shintech/gen_styles.py (the event grammar, the checks, the C table made from today's data).
python3 -m unittest tests/shintech/test_styles.py (from the tree's root)"""
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "shintech"))
import gen_styles as g  # noqa: E402


class Events(unittest.TestCase):
    def test_notes(self):
        cases = {"A": (32, 0), "B": (33, 0), "U": (34, 0), "0": (0, 0), "15": (15, 0), "-1": (16, 0), "-8": (23, 0),
                 "^": (40, 0), "v": (41, 0), "~": (42, 0), "?": (43, 0), "0'": (0, 1), "A''": (32, 2), "B,": (33, -1)}
        for tok, want in cases.items():
            with self.subTest(tok=tok):
                self.assertEqual(g.parse_notes(tok), want)
        for bad in ("16", "-9", "X", "0'''", "A,,", "", "a"):
            with self.subTest(bad=bad):
                self.assertRaises(g.StyleError, g.parse_notes, bad)

    def test_event(self):
        self.assertEqual(g.parse_event("3 A 100 .75 d", 16), (3, 32, 0, 100, 3, "d"))
        self.assertEqual(g.parse_event("0 -1 64 16", 16), (0, 16, 0, 64, 64, None))
        self.assertEqual(g.encode(g.parse_event("5 U' 90 2 u", 8)), (5, 34 | 1 << 6, 90, 7 | 2 << 6))
        self.assertEqual(g.encode(g.parse_event("1 B, 1 .25", 8)), (1, 33 | 2 << 6, 1, 0))
        for bad in ("16 A 100 1", "0 A 0 1", "0 A 128 1", "0 A 100 .3", "0 A 100 0", "0 A 100 16.25",
                    "0 0 100 1 d", "0 ^ 100 1 u", "0 A 100 1 x", "0 A 100", "a A 100 1"):
            with self.subTest(bad=bad):
                self.assertRaises(g.StyleError, g.parse_event, bad, 16)


def write(d, files):
    for name, body in files.items():
        (Path(d) / name).write_text(json.dumps(body), encoding="utf-8")


def style(n, code="ARP1", name="ARP", nvar=12, **var):
    v = {"name": "V", "rate": "1/16", "steps": 4, "events": ["0 ^ 100 1"]}
    v.update(var)
    return {"style": n, "code": code, "name": name,
            "variations": [dict(v, name=f"V{k}") for k in range(nvar)]}


class Checks(unittest.TestCase):
    def good(self):
        return {f"{n}.json": style(n, code=f"S{n}") for n in range(1, 10)}

    def load(self, files):
        with tempfile.TemporaryDirectory() as d:
            write(d, files)
            return g.load_styles(d)

    def test_good(self):
        st = self.load(self.good())
        self.assertEqual(len(st), 9)
        self.assertEqual(st[0]["vars"][0]["rate"], 3)

    def test_rejects(self):
        cases = {
            "a style missing": lambda f: f.pop("9.json"),
            "eleven variations": lambda f: f.update({"1.json": style(1, code="S1", nvar=11)}),
            "a long code": lambda f: f.update({"1.json": style(1, code="ARPEGG")}),
            "a bad rate": lambda f: f.update({"1.json": style(1, code="S1", rate="1/12")}),
            "65 steps": lambda f: f.update({"1.json": style(1, code="S1", steps=65)}),
            "oct 4": lambda f: f.update({"1.json": style(1, code="S1", oct=4)}),
            "strum 64": lambda f: f.update({"1.json": style(1, code="S1", strum=64)}),
            "no events": lambda f: f.update({"1.json": style(1, code="S1", events=[])}),
            "33 events": lambda f: f.update({"1.json": style(1, code="S1", events=["0 A 100 1"] * 33)}),
            "a bad event": lambda f: f.update({"1.json": style(1, code="S1", events=["4 A 100 1"])}),
            "the same style twice": lambda f: f.update({"10.json": style(1, code="S1")}),
        }
        for what, change in cases.items():
            with self.subTest(what=what):
                f = self.good()
                change(f)
                self.assertRaises(SystemExit, self.load, f)

    def test_sorted_by_step(self):
        f = self.good()
        f["1.json"] = style(1, code="S1", events=["3 A 100 1", "0 B 100 1", "3 U 90 1"])
        ev = self.load(f)[0]["vars"][0]["events"]
        self.assertEqual([e[0] for e in ev], [0, 3, 3])
        self.assertEqual([e[1] for e in ev], [33, 32, 34])        # (a step's events in the file's order)


class Factory(unittest.TestCase):
    def test_factory_data(self):
        st = g.load_styles(ROOT / "tools" / "shintech" / "styles")
        self.assertEqual([s["code"] for s in st],
                         ["ARP1", "ARP2", "BEAT1", "BEAT2", "BEAT3", "STRM1", "STRM2", "STRM3", "STRM4"])
        rates = {v["rname"] for s in st[:2] for v in s["vars"]}
        for r in ("1/4", "1/8", "1/16", "1/32", "8T", "16T", "32T"):           # the arps: 1/4 .. 1/32 and triplets
            self.assertIn(r, rates)
        for s in st[:2]:                                                     # .. up, down, up/down, random
            toks = {e[1] for v in s["vars"] for e in v["events"]}
            self.assertTrue({40, 41, 42, 43} <= toks | {e[1] for v in st[0]["vars"] for e in v["events"]})
        for s in st[2:5]:                                                    # beats replay the chord
            self.assertTrue(all(any(e[1] in (32, 34) for e in v["events"]) for v in s["vars"]), s["code"])
        self.assertTrue(any(v["rname"] in ("8T", "16T", "4T") for v in st[3]["vars"]))   # BEAT2: triplets
        for s in st[5:]:                                                     # strums: up and down, a strum time
            strums = {e[5] for v in s["vars"] for e in v["events"]}
            self.assertTrue({"d", "u"} <= strums, s["code"])
            self.assertTrue(all(v["strum"] > 0 for v in s["vars"] if any(e[5] for e in v["events"])), s["code"])

    def test_table_is_todays(self):
        built = ROOT / "build" / "gen" / "chord_styles.h"
        if not built.exists():
            self.skipTest("no build/gen/chord_styles.h (build the firmware once)")
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "chord_styles.h"
            g.main([str(out)])
            self.assertEqual(out.read_text(), built.read_text(), "build/gen/chord_styles.h is not today's data: rebuild")


if __name__ == "__main__":
    unittest.main()
