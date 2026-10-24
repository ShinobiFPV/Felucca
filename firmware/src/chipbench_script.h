/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 William Kew, ShinTech Electronics */
/* The OPN2 benchmark's register script (Phase 1), shared by the device benchmark (chipbench.c) and the host one
 * (tests/chipbench_host.c): the heaviest ordinary load, all six channels keyed with SSG-EG on every operator, AM on
 * every operator, the LFO on (rate 5) with AMS 2 and FMS 3 on every channel (so every phase step is recomputed every
 * sample), a different algorithm and feedback per channel. typical = 1: the same six channels as ordinary patches
 * play them, no SSG-EG and no AM, vibrato (FMS 2) on channels 1 and 2 only. The DAC is switched by the caller. Writes are
 * {port, register, data}: port 0 for channels 1..3 and the global registers, 2 for channels 4..6. */
#include <stdint.h>

typedef struct { uint8_t port, reg, data; } cb_write_t;
#define CB_SCRIPT_MAX 256u

static uint32_t cb_script_build(cb_write_t *w, int typical)
{
    static const uint8_t ALG[6] = {0, 2, 4, 5, 6, 7}, FB[6] = {5, 3, 6, 4, 7, 2};
    static const uint16_t FNUM[6] = {644, 681, 765, 858, 963, 1020};   /* C D E F# G# A#-ish, block 4 */
    static const uint8_t SSG[4] = {0x08, 0x0A, 0x0C, 0x0E};          /* the four repeating SSG-EG modes */
    uint32_t n = 0, ch, op;
#define CB_W(p, r, d) (w[n].port = (uint8_t)(p), w[n].reg = (uint8_t)(r), w[n].data = (uint8_t)(d), n++)
    CB_W(0, 0x22, 0x08 | 5);                            /* LFO on, rate 5 */
    CB_W(0, 0x27, 0x00);                                /* normal channel 3 */
    for (ch = 0; ch < 6u; ch++) {
        uint32_t p = ch < 3u ? 0u : 2u, c = ch % 3u;
        CB_W(p, 0xB0 + c, FB[ch] << 3 | ALG[ch]);
        CB_W(p, 0xB4 + c, typical ? 0xC0 | (ch < 2u ? 2u : 0u) : 0xC0 | 2u << 4 | 3u);   /* L R, AMS 2, FMS 3 */
        for (op = 0; op < 4u; op++) {
            uint32_t o = op * 4u + c;
            CB_W(p, 0x30 + o, (op & 3u) << 4 | (1u + op * 2u));   /* DT, MUL 1 3 5 7 */
            CB_W(p, 0x40 + o, op == 3u ? 0x04 : 0x14 + op * 4u);  /* TL */
            CB_W(p, 0x50 + o, 1u << 6 | 31u);           /* KS 1, AR 31 */
            CB_W(p, 0x60 + o, (typical ? 0u : 0x80u) | (8u + op));   /* AM on, DR */
            CB_W(p, 0x70 + o, 4u + op);                 /* SR */
            CB_W(p, 0x80 + o, (3u + op) << 4 | 7u);     /* SL, RR */
            CB_W(p, 0x90 + o, typical ? 0u : SSG[(op + ch) & 3u]);   /* SSG-EG on */
        }
        CB_W(p, 0xA4 + c, 4u << 3 | FNUM[ch] >> 8);    /* block 4, F-number */
        CB_W(p, 0xA0 + c, FNUM[ch] & 0xFFu);
    }
    for (ch = 0; ch < 6u; ch++)
        CB_W(0, 0x28, 0xF0 | (ch < 3u ? ch : ch + 1u)); /* key on, all four operators */
#undef CB_W
    return n;
}
