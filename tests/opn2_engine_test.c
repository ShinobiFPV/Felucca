/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 William Kew, ShinTech Electronics */
/* OPN2 engine test (src/eng_opn2.c over src/chips/opn2.c) on the host, through hostsim.c as regress.c.
 *   build/host/opn2_engine_test [DEMODIR]          (tests/run_shintech.sh: build/shintech/opn2_demo)
 * 1. frequency: every note's F-number and block against 440 x 2^((n - 69) / 12) (the chip's formula, 7.670453 MHz)
 * 2. registers: a note on a fresh chip writes its channel's operator registers, B0, B4, the frequency and the key on,
 *    in that order; the next note of the same patch writes only what differs; a note off keys off; a retrigger keys
 *    off, then on in the next block
 * 3. sound: every factory preset sounds, has no DC, does not clip, and its voice ends after the release
 * 4. CHIP LIMIT: eight notes over two OPN2 tracks keep six channels; the oldest released note's channel goes first;
 *    all voices free after the release
 * 5. voice modes: LEGATO: a second key held moves the pitch without a new key on
 * 6. the shared budget: an OPN2 track's voices take nothing from another track's eight
 * 7. velocity: a softer note is quieter (the carriers' TL); VSEN 0 makes it the same
 * 8. Legacy: LADR and S&H change the sound; EARLY / LATE lower the brightness against CLEAN
 * 9. demos into DEMODIR: every factory preset playing its suggested pattern */
#include <stdint.h>
static struct { uint32_t port, reg, data; } reglog[4096];
static uint32_t nreg;
#define OP2_REG_LOG(p, r, d) (nreg < 4096u ? (reglog[nreg].port = (p), reglog[nreg].reg = (r), reglog[nreg].data = (d), nreg++) : 0u)
#define main hostsim_main
#include "hostsim.c"
#undef main
#include <math.h>

static int bad;
static void check(const char *what, int ok)
{
    printf("opn2: %-92s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}

/* ------------------------------------------------------------ helpers --- */
static void op2_reset_all(void)
{
    memset(trk, 0, sizeof trk);
    memset(op2_ch, 0, sizeof op2_ch);
    memset(op2_lgen, 0, sizeof op2_lgen);
    op2_ready = 0;
    op2_rs_ph = 0;
    op2_age = 0;
    host_tracks_init();
    op2_init();
}

/* track t plays OPN2 preset pi (its patch from SLOT, as the main loop would) */
static void op2_setup(uint32_t t, uint32_t pi)
{
    host_preset(&trk[t], ENGI_OPN2, pi);
    trk[t].p[P_CHOR] = trk[t].p[P_DLY] = trk[t].p[P_REV] = 0;
    op2_poll();
}

/* one block of track t (the chip's block first, as fx.c mix_block); its samples into y */
static void block(uint32_t t, double *y)
{
    static int32_t b[CTL];
    uint32_t k;
    op2_mix_begin();
    track_render(&trk[t], b, CTL);
    for (k = 0; k < CTL; k++)
        if (y)
            y[k] = b[k];
}
static void blocks(uint32_t t, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        block(t, 0);
}
static uint32_t active_voices(uint32_t t)
{
    uint32_t i, n = 0;
    for (i = 0; i < NVOICE; i++)
        n += trk[t].v[i].active;
    return n;
}
static uint32_t busy_channels(void)
{
    uint32_t ch, n = 0;
    for (ch = 0; ch < OPN2_CHANNELS; ch++)
        n += op2_ch[ch].busy;
    return n;
}
static double rms(const double *y, uint32_t n)
{
    double s = 0;
    uint32_t i;
    for (i = 0; i < n; i++)
        s += y[i] * y[i];
    return n ? sqrt(s / n) : 0;
}
static double centroid(const double *y, uint32_t n)   /* brightness: the first difference's RMS over the RMS, x 1000 */
{
    double d = 0, a = 0;
    uint32_t i;
    for (i = 1; i < n; i++) {
        d += (y[i] - y[i - 1]) * (y[i] - y[i - 1]);
        a += y[i] * y[i];
    }
    return a > 0 ? 1000.0 * sqrt(d / a) : 0;
}

/* track 0: note at vel, n blocks held then r released; the samples into y (n + r blocks of CTL) */
static void note_render(uint32_t note, uint32_t vel, uint32_t n, uint32_t r, double *y)
{
    uint32_t i;
    trk_note_on(&trk[0], note, vel);
    for (i = 0; i < n + r; i++) {
        if (i == n)
            trk_note_off(&trk[0], note);
        block(0, y ? y + i * CTL : 0);
    }
}

/* ------------------------------------------------------------ 1 frequency --- */
static void frequency(void)
{
    double worst = 0;
    uint32_t n, worst_n = 0;
    for (n = 21; n <= 108; n++) {                     /* A0 .. C8 */
        uint32_t bf = op2_fnum((int32_t)n * 16, 0), blk = bf >> 11, f = bf & 2047u;
        double hz = f * 7670453.0 / (144.0 * pow(2.0, 21.0 - blk)), want = 440.0 * pow(2.0, (n - 69.0) / 12.0);
        double cents = fabs(1200.0 * log2(hz / want));
        if (cents > worst)
            worst = cents, worst_n = n;
    }
    printf("opn2: frequency: worst %.2f cents (note %u) over A0..C8\n", worst, worst_n);
    check("frequency: every note A0..C8 within 3 cents (F-number and block)", worst < 3.0);
    {
        uint32_t a = op2_fnum(69 * 16, 0);
        check("frequency: A4 = block 4, F-number 1083 (440 Hz)", (a >> 11) == 4u && (a & 2047u) == 1083u);
    }
}

/* ------------------------------------------------------------ 2 registers --- */
static int has_write(uint32_t from, uint32_t port, uint32_t reg, uint32_t data)
{
    uint32_t i;
    for (i = from; i < nreg; i++)
        if (reglog[i].port == port && reglog[i].reg == reg && reglog[i].data == data)
            return (int)i + 1;
    return 0;
}
static void registers(void)
{
    const op2_patch_t *p = &OP2_FACTORY[0];
    uint32_t k, n_first, n_second, ok = 1, order = 1, key;
    op2_reset_all();
    op2_setup(0, 0);
    trk[0].p[P_VOICE] = V_POLY;
    blocks(0, 1);                                     /* (the chip's reset and the patch copies) */
    nreg = 0;
    trk_note_on(&trk[0], 60, 127);
    block(0, 0);
    n_first = nreg;
    for (k = 0; k < 4u; k++) {                        /* channel 1: OP1 0x30, OP2 0x38, OP3 0x34, OP4 0x3C .. */
        const op2_op_t *o = &p->op[k];
        uint32_t slot = OP2_SLOT[k];
        ok &= !!has_write(0, 0, 0x30 + slot, (uint32_t)o->dt << 4 | o->mul);
        ok &= !!has_write(0, 0, 0x50 + slot, (uint32_t)o->ks << 6 | o->ar);
        ok &= !!has_write(0, 0, 0x80 + slot, (uint32_t)o->sl << 4 | o->rr);
    }
    ok &= !!has_write(0, 0, 0xB0, (uint32_t)p->fb << 3 | p->alg);
    check("registers: the first note writes channel 1's operators (DT MUL, KS AR, SL RR) and B0 from the patch", ok);
    key = (uint32_t)has_write(0, 0, 0x28, 0xF0);
    order = key && has_write(0, 0, 0xA4, op2_fnum(60 * 16, 0) >> 8) && has_write(0, 0, 0xA0, op2_fnum(60 * 16, 0) & 0xFF) &&
            (uint32_t)has_write(0, 0, 0xA0, op2_fnum(60 * 16, 0) & 0xFF) < key && key == nreg;
    check("registers: then A4 / A0 (C4's F-number, block 3) and last the key on 0x28 = F0 (channel 1)", order);
    nreg = 0;
    trk_note_off(&trk[0], 60);
    block(0, 0);
    check("registers: a note off keys channel 1 off (0x28 = 00)", has_write(0, 0, 0x28, 0x00) != 0);
    blocks(0, 400);                                   /* (released and gone) */
    nreg = 0;
    trk_note_on(&trk[0], 60, 127);
    block(0, 0);
    n_second = nreg;
    printf("opn2: registers: first note %u writes, the same patch again %u\n", n_first, n_second);
    check("registers: the same patch again on its free channel writes only what differs (the frequency, the key on)",
          n_second < n_first / 4u && has_write(0, 0, 0x28, 0xF0));
    nreg = 0;
    trk_note_on(&trk[0], 60, 127);                    /* the same key again: a retrigger */
    block(0, 0);
    k = nreg;
    block(0, 0);
    if (getenv("OP2_DEBUG")) {
        uint32_t i;
        for (i = 0; i < nreg; i++)
            printf("  retrigger %s write %u: port %u reg %02X data %02X\n", i < k ? "block 1" : "block 2", i,
                   reglog[i].port, reglog[i].reg, reglog[i].data);
    }
    {
        uint32_t off = (uint32_t)has_write(0, 0, 0x28, 0x00), on = (uint32_t)has_write(0, 0, 0x28, 0xF0);
        check("registers: a retrigger keys off in its block and on in the next", off && off <= k && on > k);
    }
}

/* -------------------------------------------------------------- 3 sound --- */
static void sound(void)
{
    static double y[CTL * 3000u];
    uint32_t pi, quiet = 0, dc_bad = 0, clip = 0, hang = 0;
    for (pi = 0; pi < NELEM(OP2_PRESETS); pi++) {
        uint32_t i, nb = FS / CTL, n = nb * 2u;      /* 1 s held, then up to 8 s for the release */
        double r, mean = 0, peak = 0;
        op2_reset_all();
        op2_setup(0, pi);
        trk[0].p[P_VOICE] = V_POLY;
        blocks(0, 1);
        note_render(pi >= 29u ? 48 : 60, 100, nb, nb, y);
        r = rms(y, nb * CTL);
        for (i = 0; i < n * CTL; i++) {
            mean += y[i];
            peak = fabs(y[i]) > peak ? fabs(y[i]) : peak;
        }
        mean /= n * CTL;
        for (i = 0; i < nb * 8u && (active_voices(0) || busy_channels()); i++)
            block(0, 0);                              /* (a voice ends in a block; its channel goes at the next) */
        printf("opn2: sound: %-14s rms %6.0f  peak %6.0f  mean %+5.0f  ends after %4.2f s\n", OP2_PRESETS[pi].name, r, peak,
               mean, (double)(nb + i) * CTL / FS);
        quiet += r < VOICE_FS / 40.0;
        dc_bad += fabs(mean) > VOICE_FS / 100.0;
        clip += peak > 2.0 * VOICE_FS;
        hang += active_voices(0) != 0 || busy_channels() != 0;
    }
    check("sound: every factory preset sounds (RMS over 1/40 of VOICE_FS in its first second)", !quiet);
    check("sound: no DC (|mean| under 1 % of VOICE_FS)", !dc_bad);
    check("sound: no voice beyond 2 x VOICE_FS", !clip);
    check("sound: every voice and channel free within 8 s of the release", !hang);
}

/* ---------------------------------------------------------- 4 CHIP LIMIT --- */
static void chip_limit(void)
{
    uint32_t i, ok_count, released_first;
    op2_reset_all();
    op2_setup(0, 13);                                 /* ORGAN on track 1 */
    op2_setup(1, 17);                                 /* WARM PAD on track 2 */
    trk[0].p[P_VOICE] = trk[1].p[P_VOICE] = V_POLY;
    blocks(0, 1);
    for (i = 0; i < 4u; i++)
        trk_note_on(&trk[0], 60 + i, 100);
    blocks(0, 2);
    trk_note_off(&trk[0], 60);                        /* the first note released: its channel goes first */
    blocks(0, 2);
    for (i = 0; i < 4u; i++)
        trk_note_on(&trk[1], 48 + i, 100);
    op2_mix_begin();
    {
        static int32_t b[CTL];
        track_render(&trk[0], b, CTL);
        track_render(&trk[1], b, CTL);
    }
    ok_count = busy_channels() == 6u;
    released_first = !trk[0].v[0].active || trk[0].v[0].stage == 4u || trk[0].v[0].s[0] == 0;
    printf("opn2: CHIP LIMIT: channels %u, track 1 voices %u, track 2 voices %u\n", busy_channels(), active_voices(0),
           active_voices(1));
    check("CHIP LIMIT: eight notes over two OPN2 tracks: six channels busy", ok_count);
    check("CHIP LIMIT: the released note's channel was taken first", released_first);
    for (i = 0; i < 4u; i++) {
        trk_note_off(&trk[0], 60 + i);
        trk_note_off(&trk[1], 48 + i);
    }
    for (i = 0; i < FS * 8u / CTL; i++) {
        static int32_t b[CTL];
        op2_mix_begin();
        track_render(&trk[0], b, CTL);
        track_render(&trk[1], b, CTL);
    }
    check("CHIP LIMIT: all voices and channels free after the release",
          !active_voices(0) && !active_voices(1) && !busy_channels());
}

/* ---------------------------------------------------------- 5 voice modes --- */
static void voice_modes(void)
{
    uint32_t keyons = 0, i, ch;
    op2_reset_all();
    op2_setup(0, 6);                                  /* BRASS */
    trk[0].p[P_VOICE] = V_LEGATO;
    blocks(0, 1);
    trk_note_on(&trk[0], 60, 100);
    blocks(0, 4);
    ch = (uint32_t)trk[0].v[0].s[0] - 1u;
    nreg = 0;
    trk_note_on(&trk[0], 64, 100);                    /* held over: legato */
    blocks(0, 4);
    for (i = 0; i < nreg; i++)
        keyons += reglog[i].reg == 0x28;
    check("voice modes: LEGATO: the second key moves the pitch (A4 / A0) without a key on or off",
          !keyons && has_write(0, ch < 3u ? 0u : 2u, 0xA0 + ch % 3u, op2_fnum(64 * 16, 0) & 0xFF));
    check("voice modes: LEGATO: one voice, one channel", active_voices(0) == 1u && busy_channels() == 1u);
}

/* ------------------------------------------------------ 6 the shared budget --- */
static void budget(void)
{
    uint32_t i;
    op2_reset_all();
    op2_setup(0, 17);
    trk[0].p[P_VOICE] = V_POLY;
    host_preset(&trk[1], 0, 0);                       /* ANALOG on track 2 */
    trk[1].p[P_VOICE] = V_POLY;
    blocks(0, 1);
    for (i = 0; i < 6u; i++)
        trk_note_on(&trk[0], 48 + i, 100);
    for (i = 0; i < 8u; i++)
        trk_note_on(&trk[1], 60 + i, 100);
    blocks(0, 2);
    check("budget: six OPN2 notes and eight ANALOG notes all sound (the chip's voices are its own)",
          active_voices(0) == 6u && active_voices(1) == 8u);
}

/* ------------------------------------------------------ 7 velocity, 8 Legacy --- */
/* the RMS of preset pi's note at vel; vsen, ladr, lgcy < 0: the preset's */
static double level_of(uint32_t pi, uint32_t vel, int16_t vsen, int16_t ladr, int16_t lgcy)
{
    static double y[CTL * 100u];
    op2_reset_all();
    op2_setup(0, pi);
    trk[0].p[P_VOICE] = V_POLY;
    if (vsen >= 0)
        trk[0].p[P_E4] = vsen;
    if (ladr >= 0)
        trk[0].p[P_E5] = ladr;
    if (lgcy >= 0)
        trk[0].p[P_E6] = lgcy;
    blocks(0, 1);
    note_render(60, vel, 60, 0, y);
    return rms(y + 20 * CTL, 40 * CTL);
}
static double bright_of(uint32_t pi, int16_t lgcy)
{
    static double y[CTL * 100u];
    op2_reset_all();
    op2_setup(0, pi);
    trk[0].p[P_VOICE] = V_POLY;
    trk[0].p[P_E6] = lgcy;
    blocks(0, 1);
    note_render(72, 110, 60, 0, y);
    return centroid(y + 20 * CTL, 40 * CTL);
}
static void velocity_legacy(void)
{
    double loud = level_of(12, 127, -1, -1, -1), soft = level_of(12, 30, -1, -1, -1);
    double flat_l = level_of(12, 127, 0, -1, -1), flat_s = level_of(12, 30, 0, -1, -1);
    double c = bright_of(8, 0), e = bright_of(8, 1), l = bright_of(8, 2);
    double a = level_of(8, 100, -1, 0, 0), b = level_of(8, 100, -1, 127, 0);
    double s = level_of(8, 100, -1, 64, 3), k = level_of(8, 100, -1, 64, 0);
    printf("opn2: velocity: E.PIANO rms at 127 %.0f, at 30 %.0f; VSEN 0: %.0f, %.0f\n", loud, soft, flat_l, flat_s);
    check("velocity: a softer note is quieter (the carriers' TL, VSEN)", soft < loud * 0.7);
    check("velocity: VSEN 0: the same level at any velocity", fabs(flat_l - flat_s) < flat_l * 0.02);
    printf("opn2: Legacy: SAW LEAD brightness CLEAN %.0f EARLY %.0f LATE %.0f; rms LADR 0 %.0f, 127 %.0f; S&H %.0f vs %.0f\n",
           c, e, l, a, b, s, k);
    check("Legacy: EARLY and LATE lower the brightness against CLEAN (EARLY the most)", e < l && l < c);
    check("Legacy: LADR changes the sound (0 against 127)", fabs(a - b) > 1.0);
    check("Legacy: S&H changes the sound", fabs(s - k) > 1.0);
}

/* ----------------------------------------------------------------- demos --- */
static void demo(const char *dir, uint32_t pi)
{
    static int32_t o[2 * CTL];
    const preset_t *pr = &OP2_PRESETS[pi];
    uint32_t pat = pr->pat ? pr->pat - 1u : 2u, step = FS / 8u, s, i, held = 0, frames = 0;
    char path[512], nm[32];
    FILE *w;
    for (i = 0; pr->name[i] && i < 31u; i++)
        nm[i] = pr->name[i] == ' ' || pr->name[i] == '.' ? '_' : pr->name[i];
    nm[i] = 0;
    snprintf(path, sizeof path, "%s/%02u_%s.wav", dir, pi + 1u, nm);
    if (!(w = fopen(path, "wb")))
        return;
    wav_hdr(w, 0);
    op2_reset_all();
    op2_setup(0, pi);
    for (s = 0; s < 32u; s++) {                       /* twice through the pattern's 16 steps, 120 BPM 1/16 */
        uint32_t k = s % 16u, note = PATTERNS[pat].note[k], fl = PATTERNS[pat].flags[k];
        if (pi >= 29u && note)                        /* (the drums: one pitch) */
            note = pi == 29u ? 36 : pi == 30u ? 50 : 45;
        if (!(fl & 4u) && held) {
            trk_note_off(&trk[0], held);
            held = 0;
        }
        if (note && !(fl & 4u)) {
            trk_note_on(&trk[0], note, fl & 1u ? 120 : 90);
            held = note;
        }
        for (i = 0; i < step; i += CTL) {
            uint32_t j;
            mix_block(o, CTL);
            for (j = 0; j < CTL; j++)
                wav_put(w, o[2 * j], o[2 * j + 1]);
            frames += CTL;
        }
    }
    if (held)
        trk_note_off(&trk[0], held);
    for (i = 0; i < FS * 2u; i += CTL) {
        uint32_t j;
        mix_block(o, CTL);
        for (j = 0; j < CTL; j++)
            wav_put(w, o[2 * j], o[2 * j + 1]);
        frames += CTL;
    }
    fseek(w, 0, SEEK_SET);
    wav_hdr(w, frames);
    fclose(w);
}

int main(int argc, char **argv)
{
    uint32_t pi;
    frequency();
    registers();
    sound();
    chip_limit();
    voice_modes();
    budget();
    velocity_legacy();
    if (argc > 1) {
        for (pi = 0; pi < NELEM(OP2_PRESETS); pi++)
            demo(argv[1], pi);
        printf("opn2: demos in %s: the %u presets, each playing its suggested pattern\n", argv[1],
               (uint32_t)NELEM(OP2_PRESETS));
    }
    printf("opn2_engine_test: %s\n", bad ? "FAILED" : "all checks ok");
    return bad != 0;
}
