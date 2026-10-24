/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 William Kew, ShinTech Electronics */
/* PSG's factory presets (eng_psg.c): original sounds, made for this engine (no copy of a device's presets or of a game's
 * sounds). Per preset: WAVE NOIS DRV ARP ARPS VIB SWP LFSR, then the track's ADSR (ATK DEC SUS REL, F_TIME steps), MONO,
 * the sends and the suggested pattern. Categories in category.c PRESET_CAT[15], in this order: basses, leads, keys and
 * arpeggios, pads, plucks, FX, drums */
#define PSP(name, w, nz, drv, arp, arps, vib, swp, a, d, s, r, mono, fx, pat) \
    {name, {w, nz, drv, arp, arps, vib, swp, 0}, {a, d, s, r}, 0, mono, fx, pat}
static const preset_t PSG_PRESETS[] = {
    /* basses (PERIOD at TRK: the chip's bass below its tone range) */
    PSP("PERIOD BASS", PW_PERIOD, 0, 0, 0, 2, 0, 0, 0, 40, 100, 15, 1, FX(0, 0, 0, 10), PAT(2)),
    PSP("PULSE BASS", PW_P25, 0, 0, 0, 2, 0, 0, 0, 30, 90, 10, 1, FX(0, 0, 5, 10), PAT(1)),
    PSP("SAW BASS", PW_SAW, 0, 0, 0, 2, 0, 0, 0, 35, 80, 10, 1, FX(0, 0, 0, 10), PAT(2)),
    PSP("BUZZ BASS", PW_BUZZ, 0, 0, 0, 2, 0, 0, 0, 30, 90, 10, 1, FX(10, 0, 0, 10), PAT(8)),
    /* leads */
    PSP("SQUARE LEAD", PW_SQR, 0, 0, 0, 2, 3, 0, 0, 0, 127, 15, 1, FX(0, 10, 20, 25), PAT(3)),
    PSP("PULSE LEAD", PW_P12, 0, 0, 0, 2, 4, 0, 0, 0, 127, 15, 1, FX(0, 10, 25, 25), PAT(3)),
    PSP("TRI LEAD", PW_TRI, 0, 0, 0, 2, 3, 0, 0, 0, 127, 20, 1, FX(0, 15, 20, 30), PAT(3)),
    PSP("THIN LEAD", PW_P6, 0, 0, 0, 2, 5, 0, 0, 0, 127, 15, 1, FX(0, 10, 30, 25), PAT(3)),
    /* keys and arpeggios */
    PSP("MAJOR ARP", PW_SQR, 0, 0, 2, 2, 0, 0, 0, 0, 127, 10, 0, FX(0, 10, 25, 20), PAT(5)),
    PSP("MINOR ARP", PW_SQR, 0, 0, 3, 2, 0, 0, 0, 0, 127, 10, 0, FX(0, 10, 25, 20), PAT(5)),
    PSP("OCTAVE ARP", PW_P25, 0, 0, 1, 3, 0, 0, 0, 0, 127, 10, 0, FX(0, 10, 25, 20), PAT(5)),
    PSP("CHIP BELL", PW_SQR, 0, 3, 0, 2, 0, 0, 0, 0, 127, 30, 0, FX(0, 20, 20, 35), PAT(6)),
    /* pads */
    PSP("SOFT PULSE", PW_P25, 0, 0, 0, 2, 2, 0, 60, 0, 127, 50, 0, FX(0, 40, 20, 50), PAT(5)),
    PSP("TRI PAD", PW_TRI, 0, 0, 0, 2, 2, 0, 70, 0, 127, 60, 0, FX(0, 40, 20, 55), PAT(5)),
    /* plucks */
    PSP("SQUARE PLUCK", PW_SQR, 0, 1, 0, 2, 0, 0, 0, 0, 127, 10, 0, FX(0, 10, 20, 20), PAT(6)),
    PSP("PULSE PLUCK", PW_P12, 0, 2, 0, 2, 0, 0, 0, 0, 127, 10, 0, FX(0, 10, 20, 20), PAT(6)),
    /* FX */
    PSP("COIN", PW_SQR, 0, 3, 1, 4, 0, 0, 0, 0, 127, 10, 1, FX(0, 0, 10, 15), PAT(0)),
    PSP("LASER", PW_SQR, 0, 1, 0, 2, 0, -40, 0, 0, 127, 5, 1, FX(0, 0, 20, 15), PAT(0)),
    PSP("JUMP", PW_SQR, 0, 2, 0, 2, 0, 16, 0, 0, 127, 5, 1, FX(0, 0, 10, 10), PAT(0)),
    PSP("EXPLOSION", PW_WHITE, 3, 6, 0, 2, 0, 0, 0, 0, 127, 30, 1, FX(10, 0, 0, 30), PAT(0)),
    PSP("MOTOR", PW_PERIOD, 0, 0, 0, 2, 8, 0, 0, 0, 127, 10, 1, FX(0, 0, 0, 10), PAT(0)),
    /* drums */
    PSP("PSG KICK", PW_SQR, 0, 1, 0, 2, 0, -60, 0, 0, 127, 5, 0, FX(0, 0, 0, 10), PAT(8)),
    PSP("PSG SNARE", PW_WHITE, 2, 1, 0, 2, 0, 0, 0, 0, 127, 5, 0, FX(0, 0, 0, 15), PAT(12)),
    PSP("PSG HAT", PW_WHITE, 1, 0, 0, 2, 0, 0, 0, 12, 0, 5, 0, FX(0, 0, 0, 10), PAT(12)),
    PSP("METAL HIT", PW_METAL, 1, 2, 0, 2, 0, 0, 0, 0, 127, 10, 0, FX(0, 0, 10, 20), PAT(12)),
    PSP("PSG TOM", PW_TRI, 0, 2, 0, 2, 0, -16, 0, 0, 127, 10, 0, FX(0, 0, 0, 15), PAT(6)),
};
