/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 William Kew, ShinTech Electronics */
/* PSG: an SN76489-class square-wave chip (the 16BIT mode's PSG engine, eng_psg.c), written from its documented
 * behaviour (no code taken from another emulator). Driven by register bytes as on the chip:
 *   1 r r r d d d d   latch: register r (0 tone 1, 1 vol 1, 2 tone 2, 3 vol 2, 4 tone 3, 5 vol 3, 6 noise, 7 vol 4),
 *                     its low 4 bits d
 *   0 - d d d d d d   data: a tone register's high 6 bits (the latched one); a volume or noise register's 4 bits
 * A write to the noise register (either way) resets the noise shift register.
 * Clocked at CLOCK / 16 (223,722 Hz from NTSC's 3,579,545 Hz): each tone counter counts down from its 10-bit value
 * and flips its output at 0 (a square of CLOCK / (32 N); N 0 or 1: held high, the trick sample players use); the noise
 * counter reloads 0x10 / 0x20 / 0x40 or follows tone 3's flips, and its shift register steps on each rising edge:
 *   SEGA (the console's VDP PSG): 16 bits, white noise feedback bit 0 ^ bit 3, periodic noise 16 steps, from 0x8000
 *   TI   (SN76489):               15 bits, white noise feedback bit 0 ^ bit 1, periodic noise 15 steps, from 0x4000
 * Output: each channel high = its volume (4 bits of attenuation, 2 dB a step, 15 off), low = 0: unipolar, as on the
 * chip (the engine's high-pass takes the DC away). psg_render averages the native ticks of each 44.1 kHz sample.
 * The 16BIT extension (not the chip): per tone channel a shape other than the square, drawn from the same counter
 * (pulse widths, and ramps built from the chip's volume steps, as fast volume writes would make them), and two short
 * noise sequences (METAL, BUZZ) */
#ifndef PSG_C_INCLUDED
#define PSG_C_INCLUDED

#define PSG_CLOCK 3579545u
#define PSG_TICK_Q16 ((uint32_t)(((uint64_t)PSG_CLOCK << 16) / 16u / 44100u))   /* native ticks per sample, Q16 */
enum { PSG_SQR, PSG_P25, PSG_P12, PSG_P6, PSG_SAW, PSG_TRI, PSG_RSAW };       /* tone shapes (PSG_SQR: the chip) */
enum { PSG_NZ_CHIP, PSG_NZ_METAL, PSG_NZ_BUZZ };                             /* noise sequences (CHIP: the chip) */

/* 2 dB a step, 15 off: the unipolar level of a channel high, of 8191 */
static const uint16_t PSG_VOL[16] = {8191, 6507, 5168, 4105, 3261, 2590, 2057, 1634, 1298, 1031, 819, 650, 517,
                                     410, 326, 0};
/* the ramps' 16 steps (a step k of 15 of the way up) as attenuation from the channel's: the nearest volume step to a
 * linear k / 15 (the chip has only its log steps; 15: off) */
static const uint8_t PSG_RAMP_ATT[16] = {15, 12, 9, 7, 6, 5, 4, 3, 3, 2, 2, 1, 1, 1, 0, 0};

typedef struct {
    uint16_t tone[3];                /* the tone registers, 10 bits */
    uint8_t vol[4];                  /* the volume registers (attenuation, 15 off) */
    uint8_t noise;                   /* the noise register: bit 2 white (1) / periodic (0), bits 0..1 the rate */
    uint8_t latch;                   /* the latched register */
    uint8_t ti;                      /* 1: TI's 15-bit shift register, 0: SEGA's 16-bit */
    uint8_t nz_seq;                  /* PSG_NZ_*: the extension's noise sequences */
    uint8_t shape[3];                /* PSG_SQR..: the extension's tone shapes */
    uint16_t cnt[4];                 /* the counters */
    uint8_t ff[4];                   /* the flip-flops (the noise's: its clock) */
    uint16_t lfsr;
    uint32_t tick_frac;              /* the resampler: native ticks owed, Q16 */
    uint32_t rcp[3];                 /* (psg_render) 8 x 65536 / the tone value: the ramps' steps without a divide */
} psg_t;

static void psg_lfsr_reset(psg_t *c) { c->lfsr = c->nz_seq == PSG_NZ_METAL ? 0x40u : c->nz_seq == PSG_NZ_BUZZ ? 0x10u : c->ti ? 0x4000u : 0x8000u; }

static void psg_reset(psg_t *c, int ti)
{
    uint32_t i;
    memset(c, 0, sizeof *c);
    c->ti = (uint8_t)(ti != 0);
    for (i = 0; i < 4u; i++)
        c->vol[i] = 15;
    psg_lfsr_reset(c);
}

static void psg_write(psg_t *c, uint32_t b)
{
    uint32_t r;
    if (b & 0x80u) {
        c->latch = (uint8_t)((b >> 4) & 7u);
        r = c->latch;
        if (r & 1u)
            c->vol[r >> 1] = (uint8_t)(b & 15u);
        else if (r < 6u)
            c->tone[r >> 1] = (uint16_t)((c->tone[r >> 1] & 0x3F0u) | (b & 15u));
        else {
            c->noise = (uint8_t)(b & 7u);
            psg_lfsr_reset(c);
        }
        return;
    }
    r = c->latch;
    if (r & 1u)
        c->vol[r >> 1] = (uint8_t)(b & 15u);
    else if (r < 6u)
        c->tone[r >> 1] = (uint16_t)((c->tone[r >> 1] & 15u) | (b & 0x3Fu) << 4);
    else {
        c->noise = (uint8_t)(b & 7u);
        psg_lfsr_reset(c);
    }
}

/* the noise shift register one step (its clock's rising edge) */
static void psg_lfsr_step(psg_t *c)
{
    uint32_t l = c->lfsr, fb;
    if (c->nz_seq == PSG_NZ_METAL) {             /* (extension) 7 bits, bit 0 ^ bit 1: 127 steps, metallic */
        fb = (l ^ (l >> 1)) & 1u;
        c->lfsr = (uint16_t)((l >> 1) | fb << 6);
    } else if (c->nz_seq == PSG_NZ_BUZZ) {       /* (extension) 5 bits, bit 0 ^ bit 2: 31 steps, a buzz */
        fb = (l ^ (l >> 2)) & 1u;
        c->lfsr = (uint16_t)((l >> 1) | fb << 4);
    } else if (c->ti) {
        fb = c->noise & 4u ? (l ^ (l >> 1)) & 1u : l & 1u;
        c->lfsr = (uint16_t)((l >> 1) | fb << 14);
    } else {
        fb = c->noise & 4u ? (l ^ (l >> 3)) & 1u : l & 1u;
        c->lfsr = (uint16_t)((l >> 1) | fb << 15);
    }
}

/* tone channel k's level now: its shape at its counter's place in the period */
static uint32_t psg_tone_level(const psg_t *c, uint32_t k)
{
    uint32_t n = c->tone[k], v = c->vol[k], pos, per, s;
    if (n <= 1u)
        return PSG_VOL[v];                       /* (held high) */
    if (c->shape[k] == PSG_SQR)
        return c->ff[k] ? PSG_VOL[v] : 0u;
    per = 2u * n;
    pos = (c->ff[k] ? n : 0u) + (n - (c->cnt[k] > n ? n : c->cnt[k]));   /* 0 .. per - 1, from the counter */
    switch (c->shape[k]) {
    case PSG_P25: return pos * 4u < per ? PSG_VOL[v] : 0u;
    case PSG_P12: return pos * 8u < per ? PSG_VOL[v] : 0u;
    case PSG_P6: return pos * 16u < per ? PSG_VOL[v] : 0u;
    case PSG_SAW: s = 15u - ((pos * c->rcp[k]) >> 16); break;                 /* (pos 16 / per) */
    case PSG_RSAW: s = (pos * c->rcp[k]) >> 16; break;
    default: s = (pos * c->rcp[k]) >> 15; s = s < 16u ? s : 31u - s; break;  /* PSG_TRI (pos 32 / per) */
    }
    s = v + PSG_RAMP_ATT[s & 15u];
    return PSG_VOL[s > 15u ? 15u : s];
}

/* one native tick (CLOCK / 16) */
static void psg_tick(psg_t *c)
{
    uint32_t k, rise = 0;
    for (k = 0; k < 3u; k++) {
        if (c->cnt[k])
            c->cnt[k]--;
        if (!c->cnt[k]) {
            c->cnt[k] = c->tone[k];
            c->ff[k] ^= 1u;
            if (k == 2u && c->ff[k])
                rise = (c->noise & 3u) == 3u;    /* (tone 3's rising edge clocks the noise at rate 3) */
        }
    }
    if ((c->noise & 3u) == 3u) {
        if (rise)
            psg_lfsr_step(c);
        return;
    }
    if (c->cnt[3])
        c->cnt[3]--;
    if (!c->cnt[3]) {
        c->cnt[3] = (uint16_t)(0x10u << (c->noise & 3u));
        c->ff[3] ^= 1u;
        if (c->ff[3])
            psg_lfsr_step(c);
    }
}

/* n (<= 32) samples at 44.1 kHz: each channel's level averaged over its native ticks, into out[k][i] (0..8191) for the
 * channels in mask (bit k). As psg_tick tick by tick (tests/psg_engine_test.c checks it sample for sample against
 * that), but a channel at a time and a flip at a time: between two flips a square's level holds, so a run of d ticks
 * is the old level d - 1 times and the new one once (a tick's level is taken after its flip). The extension's shapes
 * (their level moves inside a half period) step tick by tick. A channel not in mask is not clocked (silent, its phase
 * does not matter; the noise's clock, tone 3 at rate 3, is advanced by the noise) */
static void psg_render(psg_t *c, int32_t out[][32], uint32_t n, uint32_t mask)
{
    static const uint16_t RECIP[8] = {0, 65535, 32768, 21845, 16384, 13107, 10923, 9362};   /* 65536 / k: no divide */
    uint8_t nts[32];
    uint32_t i, k, tot = 0;
    for (k = 0; k < 3u; k++)                     /* (once a block: a tone value changes only between blocks) */
        c->rcp[k] = c->tone[k] > 1u ? (8u << 16) / c->tone[k] : 0u;
    for (i = 0; i < n; i++) {                    /* the native ticks of each sample: 5 or 6 (223,722 / 44,100) */
        c->tick_frac += PSG_TICK_Q16;
        nts[i] = (uint8_t)(c->tick_frac >> 16);
        c->tick_frac &= 0xFFFFu;
        tot += nts[i];
    }
    if (mask & 8u) {                             /* the noise (first: at rate 3 on a copy of tone 3's counter) */
        uint32_t r = c->noise & 3u, cnt = r == 3u ? c->cnt[2] : c->cnt[3], ff = r == 3u ? c->ff[2] : c->ff[3];
        uint32_t rl = r == 3u ? c->tone[2] : 0x10u << r, lv = PSG_VOL[c->vol[3]];
        for (i = 0; i < n; i++) {
            uint32_t rem = nts[i], acc = 0;
            while (rem) {
                uint32_t d = cnt ? cnt : 1u;
                if (d > rem) {
                    acc += (c->lfsr & 1u ? lv : 0u) * rem;
                    cnt -= rem;
                    break;
                }
                acc += (c->lfsr & 1u ? lv : 0u) * (d - 1u);
                cnt = rl;
                ff ^= 1u;
                if (ff)
                    psg_lfsr_step(c);            /* (the clock's rising edge) */
                acc += c->lfsr & 1u ? lv : 0u;
                rem -= d;
            }
            out[3][i] = (int32_t)((acc * RECIP[nts[i] & 7u]) >> 16);
        }
        if (r != 3u) {
            c->cnt[3] = (uint16_t)cnt;
            c->ff[3] = (uint8_t)ff;
        } else if (!(mask & 4u)) {               /* (tone 3 only the clock: its counter is the copy's) */
            c->cnt[2] = (uint16_t)cnt;
            c->ff[2] = (uint8_t)ff;
        }
    }
    for (k = 0; k < 3u; k++) {
        uint32_t nn = c->tone[k], lv = PSG_VOL[c->vol[k]];
        if (!(mask >> k & 1u))
            continue;
        if (nn <= 1u) {                          /* held high (flipping every tick) */
            for (i = 0; i < n; i++)
                out[k][i] = (int32_t)((lv * nts[i] * RECIP[nts[i] & 7u]) >> 16);
            c->ff[k] ^= (uint8_t)(tot & 1u);
            c->cnt[k] = (uint16_t)nn;
        } else if (c->shape[k] == PSG_SQR) {     /* the chip's square: a flip at a time */
            uint32_t cnt = c->cnt[k], ff = c->ff[k];
            for (i = 0; i < n; i++) {
                uint32_t rem = nts[i], acc = 0;
                while (rem) {
                    uint32_t d = cnt ? cnt : 1u;
                    if (d > rem) {
                        acc += (ff ? lv : 0u) * rem;
                        cnt -= rem;
                        break;
                    }
                    acc += (ff ? lv : 0u) * (d - 1u);
                    cnt = nn;
                    ff ^= 1u;
                    acc += ff ? lv : 0u;
                    rem -= d;
                }
                out[k][i] = (int32_t)((acc * RECIP[nts[i] & 7u]) >> 16);
            }
            c->cnt[k] = (uint16_t)cnt;
            c->ff[k] = (uint8_t)ff;
        } else {                                 /* the extension's shapes: tick by tick */
            for (i = 0; i < n; i++) {
                uint32_t t, acc = 0;
                for (t = 0; t < nts[i]; t++) {
                    if (c->cnt[k])
                        c->cnt[k]--;
                    if (!c->cnt[k]) {
                        c->cnt[k] = (uint16_t)nn;
                        c->ff[k] ^= 1u;
                    }
                    acc += psg_tone_level(c, k);
                }
                out[k][i] = (int32_t)((acc * RECIP[nts[i] & 7u]) >> 16);
            }
        }
    }
}
#endif
