/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 William Kew, ShinTech Electronics */
/* PCM8's factory presets (eng_pcm8.c): Felucca's own drums (the DRUM kits, SLICE's BREAK) and the user slots, through
 * the 8-bit DAC. Per preset: SRC RATE BITS MODE TUNE DCAY LADR LGCY, the ADSR (one-shot hits: their own ends), the
 * sends, the pattern (12: BEAT). Categories in category.c PRESET_CAT[16]: all drums */
#define PCP(name, src, rate, bits, mode, tune, dcay, ladr, lgcy, fx) \
    {name, {src, rate, bits, mode, tune, dcay, ladr, lgcy}, {0, 100, 127, 100}, 0, 0, fx, PAT(12)}
static const preset_t PCM8_PRESETS[] = {
    PCP("STD 8BIT", 0, 1, 0, 0, 0, 64, 64, 1, FX(0, 0, 0, 10)),
    PCP("STD 12K", 0, 0, 0, 0, 0, 64, 64, 1, FX(0, 0, 0, 10)),
    PCP("CRUNCH KIT", 0, 0, 1, 0, 0, 64, 90, 2, FX(10, 0, 0, 10)),
    PCP("66 8BIT", 3, 1, 0, 0, 0, 64, 64, 1, FX(0, 0, 0, 10)),
    PCP("80 12K", 1, 0, 0, 0, 0, 70, 64, 1, FX(0, 0, 0, 10)),
    PCP("DEEP 8BIT", 6, 1, 0, 0, 0, 64, 64, 1, FX(0, 0, 0, 15)),
    PCP("TIGHT 8BIT", 7, 1, 0, 0, 0, 50, 64, 1, FX(0, 0, 0, 10)),
    PCP("PERC 24K", 8, 1, 0, 0, 0, 64, 64, 2, FX(0, 0, 10, 20)),
    PCP("ONE SHOT KIT", 0, 1, 0, 1, 0, 64, 64, 1, FX(0, 0, 0, 10)),
    PCP("BREAK CHOPS", 9, 1, 0, 0, 0, 64, 64, 1, FX(0, 0, 0, 10)),
    PCP("BREAK 4BIT", 9, 0, 1, 0, 0, 64, 80, 2, FX(10, 0, 0, 10)),
    PCP("USR1 KIT", 10, 1, 0, 0, 0, 64, 64, 1, FX(0, 0, 0, 10)),
    PCP("USR2 KIT", 11, 1, 0, 0, 0, 64, 64, 1, FX(0, 0, 0, 10)),
    PCP("USR3 KIT", 12, 1, 0, 0, 0, 64, 64, 1, FX(0, 0, 0, 10)),
};
