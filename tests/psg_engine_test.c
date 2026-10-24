/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 William Kew, ShinTech Electronics */
/* PSG engine test (src/eng_psg.c over src/chips/psg.c) on the host, through hostsim.c as regress.c.
 *   build/shintech/host/psg_engine_test [DEMODIR]          (tests/run_shintech.sh: build/shintech/psg_demo)
 * 1. the chip: the volume table's 2 dB steps; register writes (latch, data, the noise register's reset)
 * 2. dividers: every note's tone value = CLOCK / (32 f) rounded, 1023 below the chip's range; A4 rendered measures
 *    CLOCK / (32 N)
 * 3. the noise: the shift registers' sequences (SEGA 16-bit white 57,337 steps from 0x8000, periodic 16; TI 15-bit
 *    white 32,767, periodic 15; the first 32 output bits against the definition), the three rates, TRK through tone 3
 * 4. the shapes: the pulses' duty, the ramps' steps
 * 5. register writes from notes: a note writes its tone value (latch + data) then its volume; a note off silences it
 * 6. sound: every factory preset sounds, has no DC (the high-pass), does not clip, and frees its voice and channel
 * 7. CHIP LIMIT: five tone notes over two tracks keep three tone channels; a noise note the noise channel; TRK noise
 *    takes tone 3 too
 * 8. the volume in the chip's steps: DRV decays 2 dB every DRV frames; the ADSR's levels land on the 16 steps
 * 9. the macros: ARP steps the tone value through its chord, VIB moves it after the delay, SWP glides it
 * 10. demos into DEMODIR: every factory preset playing its suggested pattern */
#include <stdint.h>
static uint8_t reglog[4096];
static uint32_t nreg;
#define PSG_REG_LOG(b) (nreg < 4096u ? (reglog[nreg++] = (uint8_t)(b)) : 0u)
#define main hostsim_main
#include "hostsim.c"
#undef main
#include <math.h>

static int bad;
static void check(const char *what, int ok)
{
    printf("psg: %-93s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}

/* ------------------------------------------------------------ helpers --- */
static void psg_reset_all(void)
{
    memset(trk, 0, sizeof trk);
    memset(psg_ch, 0, sizeof psg_ch);
    memset(psg_hp, 0, sizeof psg_hp);
    psg_ready = 0;
    psg_age = 0;
    host_tracks_init();
}
static void psg_setup(uint32_t t, uint32_t pi)
{
    host_preset(&trk[t], ENGI_PSG, pi);
    trk[t].p[P_CHOR] = trk[t].p[P_DLY] = trk[t].p[P_REV] = trk[t].p[P_DIST] = 0;
}
static void block(uint32_t t, double *y)
{
    static int32_t b[CTL];
    uint32_t k;
    psg_mix_begin();
    track_render(&trk[t], b, CTL);
    for (k = 0; k < CTL && y; k++)
        y[k] = b[k];
}
static void blocks(uint32_t t, uint32_t n) { while (n--) block(t, 0); }
static uint32_t busy_channels(void)
{
    uint32_t c, n = 0;
    for (c = 0; c < PSG_CHANNELS; c++) n += psg_ch[c].busy;
    return n;
}
static uint32_t active_voices(uint32_t t)
{
    uint32_t i, n = 0;
    for (i = 0; i < NVOICE; i++) n += trk[t].v[i].active;
    return n;
}
static uint32_t lfsr_period(psg_t *c, uint32_t max)   /* steps until the shift register is back where it began */
{
    uint32_t s0 = c->lfsr, n = 0;
    do {
        psg_lfsr_step(c);
        n++;
    } while (c->lfsr != s0 && n < max);
    return n;
}
static double note_hz(uint32_t note) { return 440.0 * pow(2.0, ((double)note - 69.0) / 12.0); }

/* ------------------------------------------------------------- 1 the chip --- */
static void chip(void)
{
    psg_t c;
    uint32_t k, ok = 1;
    for (k = 0; k < 14u; k++) {
        double db = 20.0 * log10((double)PSG_VOL[k] / PSG_VOL[k + 1u]);
        ok &= fabs(db - 2.0) < 0.05;
    }
    check("volume: 15 steps of 2 dB (+-0.05) from 8191, 15 silent", ok && PSG_VOL[15] == 0 && PSG_VOL[0] == 8191);
    psg_reset(&c, 0);
    psg_write(&c, 0x80 | 0x0E);                   /* latch tone 1, low 4 bits E */
    psg_write(&c, 0x3F);                          /* data: high 6 bits */
    ok = c.tone[0] == 0x3FEu;
    psg_write(&c, 0xC5);                          /* latch tone 3: low 5 */
    psg_write(&c, 0x01);
    ok &= c.tone[2] == 0x015u;
    psg_write(&c, 0x9A);                          /* volume 1: 10 */
    ok &= c.vol[0] == 10u;
    psg_write(&c, 0x03);                          /* data after a volume latch: that volume */
    ok &= c.vol[0] == 3u;
    psg_write(&c, 0xFF);                          /* volume 4 (noise) off */
    ok &= c.vol[3] == 15u;
    c.lfsr = 0x1234;
    psg_write(&c, 0xE5);                          /* noise: white, rate 1; resets the shift register */
    ok &= c.noise == 5u && c.lfsr == 0x8000u;
    c.lfsr = 0x1234;
    psg_write(&c, 0x02);                          /* data after the noise latch: noise again, reset again */
    ok &= c.noise == 2u && c.lfsr == 0x8000u;
    check("registers: latch + data (tone 1 = 0x3FE, tone 3 = 0x015), volumes, the noise register resets the sequence", ok);
}

/* ----------------------------------------------------------- 2 dividers --- */
static void dividers(void)
{
    uint32_t note, ok = 1, n, crossings = 0, i, prev = 0;
    double worst = 0;
    static int32_t out[4][32];
    psg_t c;
    for (note = 21; note <= 108u; note++) {
        double f = note_hz(note), want = 3579545.0 / (32.0 * f);
        uint32_t w = want > 1023.0 ? 1023u : (uint32_t)(want + 0.5);
        n = psg_divider((int32_t)note * 16, 0, 1u);
        if (n != w && !(fabs(want - (w - 0.5)) < 0.002 || fabs(want - (w + 0.5)) < 0.002)) {   /* (a tie: either) */
            ok = 0;
            printf("psg: note %u: divider %u, want %u (%.3f)\n", note, n, w, want);
        }
        if (want <= 1023.0 && want >= 1.0) {
            double got = 3579545.0 / (32.0 * n), cents = fabs(1200.0 * log2(got / f));
            if (note <= 84u && cents > worst) worst = cents;
        }
    }
    check("dividers: every note A0..C8 its tone value CLOCK / (32 f) rounded; below ~109 Hz the chip's 1023", ok);
    printf("psg: dividers: the chip's own detuning, A2..C6: worst %.1f cents (above, the 10-bit divider is coarse: C7 +13)\n",
           worst);
    check("dividers: within 10 cents of the note from A2 to C6 (the 10-bit divider's own error)", worst < 10.0);
    psg_reset(&c, 0);
    n = psg_divider(69 * 16, 0, 1u);
    psg_write(&c, 0x80 | (n & 15u));
    psg_write(&c, n >> 4);
    psg_write(&c, 0x90);                          /* volume 1 full */
    for (i = 0; i < 44100u; i += 32u) {           /* one second: count the square's rising edges */
        uint32_t j;
        psg_render(&c, out, 32, 1u);
        for (j = 0; j < 32u; j++) {
            uint32_t hi = out[0][j] > 4096;
            crossings += hi && !prev;
            prev = hi;
        }
    }
    printf("psg: A4: tone value %u, %u cycles in 1 s (CLOCK / (32 N) = %.2f Hz)\n", n, crossings, 3579545.0 / (32.0 * n));
    check("A4 rendered: its cycles in 1 s = CLOCK / (32 N) (+-1)", fabs(crossings - 3579545.0 / (32.0 * n)) <= 1.5 && n == 254u);
}

/* -------------------------------------------------------------- 3 noise --- */
static uint32_t ref_bits(int ti, int white, uint32_t n)   /* the first n output bits from the definition */
{
    uint32_t l = ti ? 0x4000u : 0x8000u, i, out = 0;
    for (i = 0; i < n; i++) {
        uint32_t fb = white ? ((l & 1u) ^ ((l >> (ti ? 1 : 3)) & 1u)) : (l & 1u);
        l = (l >> 1) | fb << (ti ? 14 : 15);
        out |= (l & 1u) << i;
    }
    return out;
}
static void noise(void)
{
    psg_t c;
    uint32_t pw, pp, tw, tp, pm, pb, i, bits, ok, steps[3], r;
    psg_reset(&c, 0); psg_write(&c, 0xE4); pw = lfsr_period(&c, 70000);
    psg_reset(&c, 0); psg_write(&c, 0xE0); pp = lfsr_period(&c, 70000);
    psg_reset(&c, 1); psg_write(&c, 0xE4); tw = lfsr_period(&c, 70000);
    psg_reset(&c, 1); psg_write(&c, 0xE0); tp = lfsr_period(&c, 70000);
    psg_reset(&c, 0); c.nz_seq = PSG_NZ_METAL; psg_lfsr_reset(&c); pm = lfsr_period(&c, 70000);
    psg_reset(&c, 0); c.nz_seq = PSG_NZ_BUZZ; psg_lfsr_reset(&c); pb = lfsr_period(&c, 70000);
    printf("psg: noise periods: SEGA white %u periodic %u; TI white %u periodic %u; METAL %u BUZZ %u\n", pw, pp, tw, tp, pm, pb);
    check("LFSR: SEGA 16-bit white 57,337 steps, periodic 16; TI 15-bit white 32,767, periodic 15",
          pw == 57337u && pp == 16u && tw == 32767u && tp == 15u);
    check("LFSR (extension): METAL 127 steps, BUZZ 31", pm == 127u && pb == 31u);
    ok = 1;
    for (r = 0; r < 4u; r++) {                    /* SEGA / TI x white / periodic: the first 32 output bits */
        psg_reset(&c, (int)(r >> 1));
        psg_write(&c, 0xE0u | (r & 1u ? 4u : 0u));
        for (i = 0, bits = 0; i < 32u; i++) {
            psg_lfsr_step(&c);
            bits |= (c.lfsr & 1u) << i;
        }
        ok &= bits == ref_bits((int)(r >> 1), (int)(r & 1u), 32);
    }
    check("LFSR: the first 32 output bits of each (SEGA / TI, white / periodic) = the definition's", ok);
    for (r = 0; r < 3u; r++) {                    /* the rates: shifts in one second */
        uint32_t n = 0, prev;
        psg_reset(&c, 0);
        psg_write(&c, 0xE4u | r);
        prev = c.lfsr;
        for (i = 0; i < 223722u; i++) {
            psg_tick(&c);
            n += c.lfsr != prev;
            prev = c.lfsr;
        }
        steps[r] = n;
    }
    printf("psg: noise rates: %u %u %u shifts/s (CLOCK / 512 / 1024 / 2048 = 6991 3496 1748)\n", steps[0], steps[1], steps[2]);
    check("noise rates HI / MID / LO: CLOCK / 512, / 1024, / 2048 (+-2 %)", fabs(steps[0] - 6991.0) < 140 &&
          fabs(steps[1] - 3495.6) < 70 && fabs(steps[2] - 1747.8) < 35);
    {                                             /* rate 3: tone 3's rising edges clock it */
        uint32_t n = 0, prev;
        psg_reset(&c, 0);
        psg_write(&c, 0xC0 | 10); psg_write(&c, 0x00);   /* tone 3 = 10: 11,186 Hz */
        psg_write(&c, 0xE7);
        prev = c.lfsr;
        for (i = 0; i < 223722u; i++) {
            psg_tick(&c);
            n += c.lfsr != prev;
            prev = c.lfsr;
        }
        check("noise rate 3: one shift per tone 3 cycle (tone 3 = 10: 11,186 /s, +-1 %)", fabs(n - 3579545.0 / 320.0) < 112);
    }
}

/* the reference render: every channel tick by tick (psg_tick), as psg_render must equal sample for sample */
static void psg_render_ref(psg_t *c, int32_t out[][32], uint32_t n)
{
    static const uint16_t RECIP[8] = {0, 65535, 32768, 21845, 16384, 13107, 10923, 9362};
    uint32_t i, k, t;
    for (k = 0; k < 3u; k++)
        c->rcp[k] = c->tone[k] > 1u ? (8u << 16) / c->tone[k] : 0u;
    for (i = 0; i < n; i++) {
        uint32_t acc[4] = {0, 0, 0, 0}, nt;
        c->tick_frac += PSG_TICK_Q16;
        nt = c->tick_frac >> 16;
        c->tick_frac &= 0xFFFFu;
        for (t = 0; t < nt; t++) {
            psg_tick(c);
            for (k = 0; k < 3u; k++)
                acc[k] += psg_tone_level(c, k);
            acc[3] += c->lfsr & 1u ? PSG_VOL[c->vol[3]] : 0u;
        }
        for (k = 0; k < 4u; k++)
            out[k][i] = (int32_t)((acc[k] * RECIP[nt & 7u]) >> 16);
    }
}
static void render_exact(void)
{
    static int32_t a[4][32], b[4][32];
    psg_t x, y;
    uint32_t seed = 12345, blk, i, k, diff = 0, cases = 0;
#define RND() (seed = seed * 1103515245u + 12345u, seed >> 16)
    for (cases = 0; cases < 400u; cases++) {     /* random registers, shapes, noise modes, kinds; 40 blocks each */
        psg_reset(&x, (int)(RND() & 1u));
        for (k = 0; k < 3u; k++) {
            uint32_t nn = RND() % 7u == 0u ? RND() % 2u : 2u + RND() % 1022u;   /* (some held high: 0, 1) */
            psg_write(&x, 0x80u | k << 5 | (nn & 15u));
            psg_write(&x, nn >> 4);
            psg_write(&x, 0x90u | k << 5 | (RND() % 16u));
            x.shape[k] = (uint8_t)(RND() % 7u);
        }
        psg_write(&x, 0xE0u | (RND() % 8u));
        psg_write(&x, 0xF0u | (RND() % 16u));
        x.nz_seq = (uint8_t)(RND() % 3u);
        psg_lfsr_reset(&x);
        y = x;
        for (blk = 0; blk < 40u; blk++) {
            if (blk == 20u) {                    /* a tone value changed mid-way (a new note) */
                uint32_t nn = 2u + RND() % 1022u, kk = RND() % 3u;
                psg_write(&x, 0x80u | kk << 5 | (nn & 15u)); psg_write(&x, nn >> 4);
                psg_write(&y, 0x80u | kk << 5 | (nn & 15u)); psg_write(&y, nn >> 4);
            }
            psg_render(&x, a, 32, 15u);
            psg_render_ref(&y, b, 32);
            for (k = 0; k < 4u; k++)
                for (i = 0; i < 32u; i++)
                    diff += a[k][i] != b[k][i];
        }
    }
#undef RND
    printf("psg: render: %u random chips x 40 blocks x 4 channels x 32 samples: %u samples differ from tick by tick\n", cases, diff);
    check("render: a flip at a time = tick by tick, sample for sample (squares, held high, the extension's shapes, noise "
          "at every rate and kind)", diff == 0u);
}

/* ------------------------------------------------------------- 4 shapes --- */
static void shapes(void)
{
    static const double DUTY[4] = {0.5, 0.25, 0.125, 0.0625};
    static int32_t out[4][32];
    uint32_t s, i, j, ok = 1;
    psg_t c;
    for (s = 0; s < 4u; s++) {                    /* SQR P25 P12 P6: the share of samples high */
        uint32_t hi = 0, n = 0;
        psg_reset(&c, 0);
        c.shape[0] = (uint8_t)s;
        psg_write(&c, 0x80 | 0); psg_write(&c, 0x20);   /* tone 1 = 0x200: ~218 Hz */
        psg_write(&c, 0x90);
        for (i = 0; i < 44100u; i += 32u) {
            psg_render(&c, out, 32, 1u);
            for (j = 0; j < 32u; j++, n++) hi += out[0][j] > 4096;
        }
        printf("psg: shape %u: high %.4f of the time (want %.4f)\n", s, (double)hi / n, DUTY[s]);
        ok &= fabs((double)hi / n - DUTY[s]) < 0.01;
    }
    check("shapes: SQUARE 50 %, PLS 25 / 12 / 6 their duty (+-1 %)", ok);
    ok = 1;
    for (s = PSG_SAW; s <= PSG_RSAW; s++) {       /* SAW TRI R-SAW: staircases on the chip's levels */
        uint32_t lv[16], nlv = 0, k;
        psg_reset(&c, 0);
        c.shape[0] = (uint8_t)s;
        psg_write(&c, 0x80 | 0); psg_write(&c, 0x20);
        psg_write(&c, 0x90);
        psg_render(&c, out, 1, 1u);               /* (a render: the ramps' per-block step, rcp) */
        for (i = 0; i < 4410u; i++) {
            uint32_t v;
            psg_tick(&c);
            v = psg_tone_level(&c, 0);
            for (k = 0; k < nlv && lv[k] != v; k++)
                ;
            if (k == nlv && nlv < 16u) lv[nlv++] = v;
            for (k = 0; k < 16u && PSG_VOL[k] != v; k++)
                ;
            ok &= k < 16u;                        /* (each level one of the chip's) */
        }
        printf("psg: shape %u: %u distinct levels\n", s, nlv);
        ok &= nlv >= 10u;
    }
    check("shapes: SAW TRI R-SAW step through >= 10 of the chip's own levels, nothing else", ok);
}

/* ------------------------------------------------ 5 register writes from notes --- */
static int has(uint32_t from, uint32_t b)
{
    uint32_t i;
    for (i = from; i < nreg; i++)
        if (reglog[i] == b) return (int)i + 1;
    return 0;
}
static void note_writes(void)
{
    uint32_t n, at, ok;
    psg_reset_all();
    psg_setup(0, 4);                              /* SQUARE LEAD */
    blocks(0, 2);
    nreg = 0;
    trk_note_on(&trk[0], 69, 127);                /* A4: tone value 254 = 0x0FE */
    blocks(0, 3);
    n = psg_divider(69 * 16, 0, 1u);
    at = (uint32_t)has(0, 0x80u | (n & 15u));
    ok = n == 254u && at && reglog[at] == (n >> 4) && has(at, 0x90u);   /* latch + data, then volume 1 = 0 (full) */
    check("a note writes tone 1 (latch 0x8E, data 0x0F: 254) then volume 1 (0x90: full at velocity 127)", ok);
    nreg = 0;
    trk_note_off(&trk[0], 69);
    blocks(0, (uint32_t)(FS * 2u / CTL));
    check("note off: its volume to 15 (0x9F) and the channel free", has(0, 0x9Fu) && !busy_channels());
    psg_setup(0, 22);                             /* PSG SNARE: WHITE, MID */
    nreg = 0;
    trk_note_on(&trk[0], 60, 100);
    blocks(0, 2);
    {
        uint32_t i, v4 = 0;
        for (i = 0; i < nreg; i++) v4 |= reglog[i] >= 0xF0u && reglog[i] < 0xFFu;
        check("a noise note writes the noise register (0xE5: white, MID) and volume 4 (0xF0 + its level)", has(0, 0xE5u) && v4);
    }
}

/* -------------------------------------------------------------- 6 sound --- */
static double rms(const double *y, uint32_t n)
{
    double s = 0;
    uint32_t i;
    for (i = 0; i < n; i++) s += y[i] * y[i];
    return n ? sqrt(s / n) : 0;
}
static void sound(void)
{
    static double y[(44100 / CTL) * CTL];
    uint32_t pi, quiet = 0, dc_bad = 0, clip = 0, hang = 0, i, nb = 44100u / CTL;
    for (pi = 0; pi < NELEM(PSG_PRESETS); pi++) {
        double mean = 0, peak = 0, r;
        psg_reset_all();
        psg_setup(0, pi);
        trk_note_on(&trk[0], 60, 100);
        for (i = 0; i < nb; i++) block(0, y + i * CTL);
        trk_note_off(&trk[0], 60);
        for (i = 0; i < nb * CTL; i++) { mean += y[i]; peak = fabs(y[i]) > peak ? fabs(y[i]) : peak; }
        mean /= nb * CTL;
        r = rms(y, nb * CTL / 3u);                /* (its first third: the drums and FX are short) */
        if (r < VOICE_FS / 80.0) { quiet++; printf("psg: preset %u %s quiet (RMS %.0f)\n", pi, PSG_PRESETS[pi].name, r); }
        if (fabs(mean) > VOICE_FS * 0.01) { dc_bad++; printf("psg: preset %u %s DC %.0f\n", pi, PSG_PRESETS[pi].name, mean); }
        if (peak > 2.0 * VOICE_FS) clip++;
        for (i = 0; i < 8u * 1378u && (active_voices(0) || busy_channels()); i++) block(0, 0);
        if (active_voices(0) || busy_channels()) { hang++; printf("psg: preset %u %s hangs\n", pi, PSG_PRESETS[pi].name); }
    }
    check("sound: every factory preset sounds (RMS over 1/80 of VOICE_FS in its first 1/3 s)", !quiet);
    check("sound: no DC (|mean| under 1 % of VOICE_FS: the high-pass)", !dc_bad);
    check("sound: no voice beyond 2 x VOICE_FS", !clip);
    check("sound: every voice and channel free within 8 s of the release", !hang);
}

/* ---------------------------------------------------------- 7 CHIP LIMIT --- */
static void chip_limit(void)
{
    static int32_t b[CTL];
    uint32_t i, ok;
    psg_reset_all();
    psg_setup(0, 4);                              /* SQUARE LEAD, POLY */
    psg_setup(1, 13);                             /* TRI PAD */
    trk[0].p[P_VOICE] = trk[1].p[P_VOICE] = V_POLY;
    for (i = 0; i < 3u; i++) trk_note_on(&trk[0], 60 + i, 100);
    psg_mix_begin(); track_render(&trk[0], b, CTL); track_render(&trk[1], b, CTL);
    for (i = 0; i < 2u; i++) trk_note_on(&trk[1], 48 + i, 100);
    psg_mix_begin(); track_render(&trk[0], b, CTL); track_render(&trk[1], b, CTL);
    ok = psg_ch[0].busy && psg_ch[1].busy && psg_ch[2].busy && !psg_ch[3].busy;
    check("CHIP LIMIT: five tone notes over two PSG tracks keep three tone channels, the noise channel free", ok);
    psg_setup(1, 22);                             /* PSG SNARE: a noise note */
    trk_note_on(&trk[1], 60, 100);
    psg_mix_begin(); track_render(&trk[0], b, CTL); track_render(&trk[1], b, CTL);
    check("CHIP LIMIT: a noise note takes the noise channel, the three tone channels play on",
          psg_ch[3].busy && psg_ch[0].busy && psg_ch[1].busy && psg_ch[2].busy);
    psg_setup(1, 0);                              /* PERIOD BASS: noise at TRK */
    trk_note_on(&trk[1], 36, 100);
    psg_mix_begin(); track_render(&trk[0], b, CTL); track_render(&trk[1], b, CTL);
    check("TRK noise: takes the noise channel and tone 3 (its clock, silent)", psg_ch[3].busy && psg_ch[3].trk3 &&
          psg_ch[2].busy && psg_ch[2].trk3 && psg_chip.vol[2] == 15u);
}

/* ------------------------------------------------- 8 the volume in the chip's steps --- */
static void volume_steps(void)
{
    uint32_t i, ok = 1, prev = 99, changes = 0;
    psg_reset_all();
    psg_setup(0, 14);                             /* SQUARE PLUCK: DRV 1 */
    trk[0].p[P_E2] = 2;                           /* DRV 2: a step every 2 frames */
    trk_note_on(&trk[0], 60, 127);
    for (i = 0; i < 40u && psg_chip.vol[0] != 0u; i++) block(0, 0);   /* (its first volume: full) */
    prev = 0;
    for (i = 0; i < 1000u; i++) {                 /* ~0.73 s: 43 frames, past the 15 steps (30 frames) */
        uint32_t v;
        block(0, 0);
        v = psg_chip.vol[0];
        if (v != prev) { changes++; if (v != prev + 1u) ok = 0; prev = v; }
    }
    printf("psg: DRV 2: %u volume changes, each one step (2 dB), ending at %u\n", changes, prev);
    check("DRV: the driver decay steps the volume one step (2 dB) at a time, down to off", ok && prev == 15u && changes >= 14u);
    {
        uint32_t f0 = 0, f1 = 0;
        psg_reset_all();
        psg_setup(0, 14);
        trk[0].p[P_E2] = 4;                       /* DRV 4: 4 frames (1/15 s) a step */
        trk_note_on(&trk[0], 60, 127);
        for (i = 0; i < 40u && psg_chip.vol[0] != 0u; i++) block(0, 0);
        for (; i < 1378u && psg_chip.vol[0] < 1u; i++) block(0, 0);
        f0 = i;
        for (; i < 1378u && psg_chip.vol[0] < 5u; i++) block(0, 0);
        f1 = i;
        printf("psg: DRV 4: step 1 at block %u, step 5 at block %u: %.1f frames a step\n", f0, f1,
               (double)(f1 - f0) * 60.0 * CTL / FS / 4.0);
        check("DRV 4: a step every 4 frames (1/60 s each, +-10 %)", fabs((double)(f1 - f0) * 60.0 * CTL / FS / 4.0 - 4.0) < 0.4);
    }
    ok = 1;
    for (i = 0; i < 32768u; i += 97u)             /* any amplitude lands on the nearest step */
        ok &= psg_att_of((int32_t)i) <= 15u;
    check("ADSR: an amplitude lands on one of the 16 levels (full 0, silence 15)", ok && psg_att_of(32767) == 0u &&
          psg_att_of(0) == 15u && psg_att_of(32767 / 2) == 3u);
}

/* ------------------------------------------------------------ 9 macros --- */
static void macros(void)
{
    uint32_t i, seen = 0, n0, n4, n7, nv, k;
    int32_t lo = 99999, hi = 0, first = 0, last = 0;
    psg_reset_all();
    psg_setup(0, 8);                              /* MAJOR ARP: ARP MAJ, 2 frames a step */
    n0 = psg_divider(60 * 16, 0, 1u);
    n4 = psg_divider(64 * 16, 0, 1u);
    n7 = psg_divider(67 * 16, 0, 1u);
    trk_note_on(&trk[0], 60, 100);
    for (i = 0; i < 300u; i++) {
        block(0, 0);
        k = psg_chip.tone[0];
        seen |= (k == n0) | (k == n4) << 1 | (k == n7) << 2;
    }
    check("ARP MAJ: tone 1 steps through C, E and G", seen == 7u);
    psg_reset_all();
    psg_setup(0, 4);                              /* SQUARE LEAD: VIB 3 */
    trk[0].p[P_E5] = 15;
    nv = psg_divider(69 * 16, 0, 1u);
    trk_note_on(&trk[0], 69, 100);
    for (i = 0; i < 200u; i++) {                  /* the delay (12 frames ~ 276 blocks): steady */
        block(0, 0);
        if (i > 2u && psg_chip.tone[0] != nv) seen = 99;
    }
    for (i = 0; i < 1000u; i++) {
        block(0, 0);
        lo = (int32_t)psg_chip.tone[0] < lo ? (int32_t)psg_chip.tone[0] : lo;
        hi = (int32_t)psg_chip.tone[0] > hi ? (int32_t)psg_chip.tone[0] : hi;
    }
    printf("psg: VIB 15: tone value %u held, then %d..%d\n", nv, lo, hi);
    check("VIB: steady through its delay, then moves the tone value both ways", seen != 99u && lo < (int32_t)nv && hi > (int32_t)nv);
    psg_reset_all();
    psg_setup(0, 17);                             /* LASER: SWP -40 */
    trk_note_on(&trk[0], 84, 127);
    blocks(0, 3);
    first = psg_chip.tone[0];
    blocks(0, 200);
    last = psg_chip.tone[0];
    check("SWP -40: the pitch falls (the tone value grows)", last > first);
}

/* ------------------------------------------------------------- 10 demos --- */
static void demo(const char *dir, uint32_t pi)
{
    static int32_t o[2 * CTL];
    const preset_t *pr = &PSG_PRESETS[pi];
    uint32_t pat = pr->pat ? pr->pat - 1u : 2u, step = FS / 8u, s, i, held = 0, frames = 0;
    char path[512], nm[32];
    FILE *w;
    for (i = 0; pr->name[i] && i < 31u; i++)
        nm[i] = pr->name[i] == ' ' || pr->name[i] == '.' || pr->name[i] == '-' ? '_' : pr->name[i];
    nm[i] = 0;
    snprintf(path, sizeof path, "%s/%02u_%s.wav", dir, pi + 1u, nm);
    if (!(w = fopen(path, "wb")))
        return;
    wav_hdr(w, 0);
    psg_reset_all();
    psg_setup(0, pi);
    for (s = 0; s < 32u; s++) {
        uint32_t k = s % 16u, note = PATTERNS[pat].note[k], fl = PATTERNS[pat].flags[k];
        if (!(fl & 4u) && held) { trk_note_off(&trk[0], held); held = 0; }
        if (note && !(fl & 4u)) { trk_note_on(&trk[0], note, fl & 1u ? 120 : 90); held = note; }
        for (i = 0; i < step; i += CTL) {
            uint32_t j;
            mix_block(o, CTL);
            for (j = 0; j < CTL; j++) wav_put(w, o[2 * j], o[2 * j + 1]);
            frames += CTL;
        }
    }
    if (held) trk_note_off(&trk[0], held);
    for (i = 0; i < FS * 2u; i += CTL) {
        uint32_t j;
        mix_block(o, CTL);
        for (j = 0; j < CTL; j++) wav_put(w, o[2 * j], o[2 * j + 1]);
        frames += CTL;
    }
    fseek(w, 0, SEEK_SET);
    wav_hdr(w, frames);
    fclose(w);
}

int main(int argc, char **argv)
{
    uint32_t pi;
    chip();
    dividers();
    noise();
    shapes();
    render_exact();
    note_writes();
    sound();
    chip_limit();
    volume_steps();
    macros();
    if (argc > 1) {
        for (pi = 0; pi < NELEM(PSG_PRESETS); pi++)
            demo(argv[1], pi);
        printf("psg: demos in %s: the %u presets, each playing its suggested pattern\n", argv[1], (uint32_t)NELEM(PSG_PRESETS));
    }
    printf("psg_engine_test: %s\n", bad ? "FAILED" : "all checks ok");
    return bad != 0;
}
