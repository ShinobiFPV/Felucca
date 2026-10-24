#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 William Kew, ShinTech Electronics
"""Drive a connected FM-1 running this firmware over USB MIDI, for the ShinTech device tests (needs mido with
python-rtmidi; the ports are named "Felucca ..."): the editor protocol's TRACK / PRESET (web/EDITOR_PROTOCOL.md) and
notes, from a small script of steps.

  fm1midi.py STEP [STEP ...]
    track N            select track N (1..4)
    preset E P         load preset P (0..) of engine E on the selected track (14: OPN2)
    on N [VEL [CH]]    note on (channel CH 1..16, default the selected track's: CH1-4)
    off N [CH]         note off
    chord N1,N2,..     notes on, together
    release            every note this script turned on, off
    wait MS            wait
"""
import sys
import time

import mido

HDR = [0x7D, 0x46, 0x4C]                      # the editor protocol's SysEx header (F0 and F7 are mido's)


def port(names, what):
    p = [n for n in names if "felucca" in n.lower()]
    if not p:
        raise SystemExit(f"fm1midi: no Felucca {what} port in {names}")
    return p[0]


def main(argv):
    out = mido.open_output(port(mido.get_output_names(), "output"))
    inp = mido.open_input(port(mido.get_input_names(), "input"))
    track, held = 1, []

    def request(cmd, *args, wait=0.3):
        out.send(mido.Message("sysex", data=HDR + [cmd, *args]))
        t0, reply = time.time(), None
        while time.time() - t0 < wait:
            for m in inp.iter_pending():
                if m.type == "sysex" and list(m.data[:4]) == HDR + [cmd]:
                    reply = list(m.data[4:])
            if reply is not None:
                return reply
            time.sleep(0.005)
        return reply

    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "track":
            track = int(argv[i + 1])
            r = request(27, track - 1)
            print(f"track {track}: {r[:2] if r else 'no reply'}")
            i += 2
        elif a == "preset":
            e, p = int(argv[i + 1]), int(argv[i + 2])
            r = request(8, e, p)
            print(f"preset {e} {p}: {r if r else 'no reply'}")
            i += 3
        elif a in ("on", "off"):
            n = int(argv[i + 1])
            vel = int(argv[i + 2]) if a == "on" and i + 2 < len(argv) and argv[i + 2].isdigit() else 100
            ch = track - 1
            out.send(mido.Message("note_on" if a == "on" else "note_off", note=n, velocity=vel if a == "on" else 0,
                                  channel=ch))
            if a == "on":
                held.append((n, ch))
            i += 3 if a == "on" and i + 2 < len(argv) and argv[i + 2].isdigit() else 2
        elif a == "chord":
            for n in map(int, argv[i + 1].split(",")):
                out.send(mido.Message("note_on", note=n, velocity=100, channel=track - 1))
                held.append((n, track - 1))
            i += 2
        elif a == "release":
            for n, ch in held:
                out.send(mido.Message("note_off", note=n, velocity=0, channel=ch))
            held = []
            i += 1
        elif a == "wait":
            time.sleep(int(argv[i + 1]) / 1000)
            i += 2
        else:
            raise SystemExit(f"fm1midi: unknown step {a!r}")
    out.close()
    inp.close()


if __name__ == "__main__":
    main(sys.argv[1:])
