/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 William Kew, ShinTech Electronics */
/* PSG: the 16BIT mode's square-wave chip, an SN76489-class PSG (chips/psg.c) driven through its registers as a game's
 * sound driver drives it.
 *
 * One chip for the whole instrument, as OPN2's (eng_opn2.c): three tone channels and one noise channel, shared by
 * every PSG track (CHIP LIMIT: a note past them takes the channel of the oldest released note, else of the oldest
 * held one). A tone WAVE takes a tone channel, a noise WAVE the noise channel; noise at TRK follows its note through
 * tone 3, as on the chip, so it takes tone 3 too (silenced). A voice owns its channel while it sounds: note_on asks for
 * it, and once a block before the parts (psg_mix_begin, fx.c mix_block) the engine frees the channels of voices gone,
 * starts the waiting notes, writes each channel's divider (from the voice's pitch: its arpeggio, vibrato and sweep
 * on top) and its 4-bit volume, renders the block (CLOCK / 16, averaged per 44.1 kHz sample) and high-passes each
 * channel (~55 Hz, the output's DC: the chip is unipolar). A voice's render adds its channel as it is: the volume is
 * the chip's own, in its 2 dB steps.
 *
 * The volume: the track's ADSR and the velocity (voice.c, as any engine's), each block rounded to the nearest of the
 * chip's 16 levels; DRV 1..15 instead the stepped decay of a driver: full at the key, one step (2 dB) quieter every DRV
 * frames (1/60 s), the ADSR still gating it (its release ends the note). Frames: the drivers' 60 Hz tick, so the
 * arpeggio, vibrato and sweep move in its steps too.
 *
 * Shared by its nature, as on the chip: the noise register (white / periodic and its rate: the last noise note's) and
 * the shift register's kind (LFSR: SEGA 16-bit, TI 15-bit; the last note's).
 *
 * The EDIT values (P_E0..P_E7):
 *   WAVE  the chip's: SQUARE, WHITE, PERIOD (noise); the extension's: PLS 25 / 12 / 6, SAW, TRI, R-SAW (from the
 *         chip's volume steps), METAL, BUZZ (short noise sequences). The card reads WAVE over the chip's, WAVE+ over
 *         the extension's; the panel AUTHENTIC / EXTENDED.
 *   NOIS  a noise WAVE's rate: TRK (the note, through tone 3), HI / MID / LO (the chip's three)
 *   DRV   ADSR (the track's ENV, in the chip's steps), 1..15 the driver decay (frames a step)
 *   ARP   the arpeggio: OFF, OCT, MAJ, MIN, SUS4, 5TH, MAJ7, MIN7, DIM, AUG, OCT2
 *   ARPS  its speed: frames a step, 1..8
 *   VIB   vibrato depth, 0..15 (10 cents a step; 6 Hz after a 0.2 s delay, in frames)
 *   SWP   pitch sweep, semitones per 1/4 s (- down, + up)
 *   LFSR  SEGA (16-bit, the default) / TI (15-bit) */

#include "chips/psg.c"

#ifndef ENGI_PSG
#define ENGI_PSG 15u                  /* engines.c ENGINES[] (append-only) */
#endif
#define PSG_CHANNELS 4u               /* 3 tone, the noise */
#define PSG_NOISE 3u

enum { PW_SQR, PW_P25, PW_P12, PW_P6, PW_SAW, PW_TRI, PW_RSAW, PW_WHITE, PW_PERIOD, PW_METAL, PW_BUZZ, PW_N };
static int psg_noise_wave(uint32_t w) { return w >= PW_WHITE; }
static int psg_chip_wave(uint32_t w) { return w == PW_SQR || w == PW_WHITE || w == PW_PERIOD; }   /* authentic */
#define PSG_FRAME_Q16 2853u           /* frames (60 Hz) a block, Q16: 60 * 32 / 44100 */

typedef struct {
    uint8_t busy, part, voice, trk3;  /* trk3: a TRK noise note holding tone 3 as its clock */
    uint32_t age;
} psg_chan_t;

static psg_t psg_chip;
static uint8_t psg_ready;
static psg_chan_t psg_ch[PSG_CHANNELS];
static uint32_t psg_age;
static uint16_t psg_sh_tone[3];       /* the registers as written (writes only what changes) */
static uint8_t psg_sh_vol[4], psg_sh_noise = 0xFF;
static int32_t psg_out44[PSG_CHANNELS][CTL] __attribute__((section(".pool")));   /* each channel, this block */
static int32_t psg_hp[PSG_CHANNELS];  /* the high-pass state, Q7 */
static uint32_t psg_idle_blocks;      /* blocks the chip was silent and not clocked (console) */

/* ---------------------------------------------------------- the registers --- */
static void psg_reg(uint32_t b)
{
#ifdef PSG_REG_LOG
    PSG_REG_LOG(b);
#endif
    psg_write(&psg_chip, b);
}
static void psg_set_tone(uint32_t k, uint32_t n)
{
    n &= 0x3FFu;
    if (psg_sh_tone[k] == n)
        return;
    psg_sh_tone[k] = (uint16_t)n;
    psg_reg(0x80u | k << 5 | (n & 15u));
    psg_reg(n >> 4);
}
static void psg_set_vol(uint32_t k, uint32_t a)
{
    if (psg_sh_vol[k] == a)
        return;
    psg_sh_vol[k] = (uint8_t)a;
    psg_reg(0x90u | k << 5 | (a & 15u));
}
static void psg_set_noise(uint32_t v, int force)  /* (a write resets the shift register: at a note's key) */
{
    if (psg_sh_noise == v && !force)
        return;
    psg_sh_noise = (uint8_t)v;
    psg_reg(0xE0u | (v & 7u));
}

/* ------------------------------------------------------------- the pitch --- */
/* the divider for pitch16 (1/16 semitone) and fine (1/4096 of it, as vmod_t): CLOCK / (32 f), 1..1023 (the chip's
 * range: below ~109 Hz it holds at 1023); div: the extra division (16 SEGA / 15 TI for periodic noise at TRK) */
#define PSG_K8 ((uint32_t)((((uint64_t)PSG_CLOCK << 32) / ((uint64_t)FS * 32u)) >> 8))   /* CLOCK 2^24 / (32 FS) */
static uint32_t psg_divider(int32_t p16, int32_t fine, uint32_t div)
{
    uint32_t inc = pitch_inc((uint32_t)clamp(p16, 0, 2047)), i8, n;
    if (fine)
        inc += (uint32_t)((int32_t)(inc >> 12) * fine);
    i8 = (inc >> 8) * div;                       /* (32-bit: f / FS 2^24; no 64-bit divide on the target) */
    if (!i8)
        return 1023u;
    n = (PSG_K8 + i8 / 2u) / i8;
    return n < 1u ? 1u : n > 1023u ? 1023u : n;
}

/* the arpeggios: semitones, then 0xFF */
static const uint8_t PSG_ARP[11][5] = {
    {0, 0xFF}, {0, 12, 0xFF}, {0, 4, 7, 0xFF}, {0, 3, 7, 0xFF}, {0, 5, 7, 0xFF}, {0, 7, 0xFF},
    {0, 4, 7, 11, 0xFF}, {0, 3, 7, 10, 0xFF}, {0, 3, 6, 0xFF}, {0, 4, 8, 0xFF}, {0, 12, 24, 0xFF}};
static const int8_t PSG_SIN10[10] = {0, 59, 95, 95, 59, 0, -59, -95, -95, -59};   /* a 6 Hz cycle in frames, of 100 */

/* the voice's pitch (1/16 semitone) at frame f of its note: the base and its macros */
static int32_t psg_pitch_at(const track_t *t, int32_t base, uint32_t f)
{
    uint32_t a = (uint32_t)clamp(t->p[P_E3], 0, 10), sp = (uint32_t)clamp(t->p[P_E4], 1, 8), len, k;
    int32_t p = base, vib = clamp(t->p[P_E5], 0, 15), swp = t->p[P_E6];
    for (len = 0; len < 5u && PSG_ARP[a][len] != 0xFFu; len++)
        ;
    k = len > 1u ? f / sp % len : 0u;
    p += PSG_ARP[a][k] * 16;
    if (vib && f >= 12u)                         /* (10 cents = 1.6 / 16 semitone a step) */
        p += PSG_SIN10[(f - 12u) % 10u] * vib * 16 / 1000;
    if (swp)
        p += swp * (int32_t)f * 16 / 15;         /* (15 frames = 1/4 s) */
    return p;
}

/* Q15 amplitude -> the nearest of the chip's levels (attenuation 0..15; 15 off) */
static uint32_t psg_att_of(int32_t a)
{
    static const uint16_t THR[15] = {29205, 23198, 18427, 14637, 11627, 9235, 7336, 5827, 4629, 3677, 2921, 2320,
                                     1843, 1464, 0};   /* (between steps k and k + 1: 32767 x 10^(-(2k + 1) / 20)) */
    uint32_t k = 0;
    if (a <= 0)
        return 15u;
    while (k < 14u && a < THR[k])
        k++;
    return a < 1163 ? 15u : k;                   /* (below step 14's half: off) */
}

/* ----------------------------------------------------------- the channels --- */
static void voice_kill(voice_t *v);             /* (voice.c) */

static void psg_steal(uint32_t ch)              /* channel ch's note ends now (CHIP LIMIT) */
{
    psg_chan_t *c = &psg_ch[ch];
    voice_t *ov = &trk[c->part % NPART].v[c->voice % NVOICE];
    if (ov->s[0] == (int32_t)ch + 1) {
        ov->s[0] = 0;
        voice_kill(ov);
    }
    if (c->trk3 && ch == PSG_NOISE)              /* (tone 3 was its clock) */
        psg_ch[2].busy = psg_ch[2].trk3 = 0;
    c->busy = c->trk3 = 0;
    psg_set_vol(ch, 15);
}

/* the oldest of channels lo..hi-1 to give up: a released note's, else a held one's */
static uint32_t psg_oldest(uint32_t lo, uint32_t hi)
{
    uint32_t ch, best = PSG_CHANNELS;
    for (ch = lo; ch < hi; ch++) {
        const voice_t *v = &trk[psg_ch[ch].part % NPART].v[psg_ch[ch].voice % NVOICE];
        if (!v->gate && (best == PSG_CHANNELS || psg_ch[ch].age < psg_ch[best].age))
            best = ch;
    }
    if (best == PSG_CHANNELS)
        for (ch = lo; ch < hi; ch++)
            if (best == PSG_CHANNELS || psg_ch[ch].age < psg_ch[best].age)
                best = ch;
    return best;
}

/* a channel for a note of wave w (noise at TRK: trk3) */
static uint32_t psg_alloc(uint32_t w, int trk3)
{
    uint32_t ch;
    if (psg_noise_wave(w)) {
        if (psg_ch[PSG_NOISE].busy)
            psg_steal(PSG_NOISE);
        if (trk3 && psg_ch[2].busy)               /* (TRK: tone 3 is its clock, as on the chip) */
            psg_steal(2);
        return PSG_NOISE;
    }
    for (ch = 0; ch < 3u; ch++)
        if (!psg_ch[ch].busy)
            return ch;
    ch = psg_oldest(0, 3u);
    if (psg_ch[ch].trk3)                         /* (tone 3 clocking a noise note: that note goes with it) */
        psg_steal(PSG_NOISE);
    psg_steal(ch);
    return ch;
}

/* voice v of track t starts (its key waited for this block) */
static void psg_start(track_t *t, voice_t *v)
{
    uint32_t tr = (uint32_t)(t - trk), w = (uint32_t)clamp(t->p[P_E0], 0, PW_N - 1), ch;
    int trk3 = psg_noise_wave(w) && t->p[P_E1] == 0;
    int32_t own = v->s[0] - 1;
    if (own >= 0 && own < (int32_t)PSG_CHANNELS && psg_ch[own].busy && psg_ch[own].part == tr &&
        psg_ch[own].voice == (uint32_t)(v - t->v) && psg_noise_wave(w) == (own == (int32_t)PSG_NOISE) &&
        (int)psg_ch[own].trk3 == trk3)
        ch = (uint32_t)own;                      /* a retrigger: its channel */
    else
        ch = psg_alloc(w, trk3);
    psg_ch[ch].busy = 1;
    psg_ch[ch].part = (uint8_t)tr;
    psg_ch[ch].voice = (uint8_t)(v - t->v);
    psg_ch[ch].trk3 = (uint8_t)trk3;
    psg_ch[ch].age = ++psg_age;
    if (trk3) {                                  /* tone 3: the noise's clock, silent */
        psg_ch[2].busy = psg_ch[2].trk3 = 1;
        psg_ch[2].part = (uint8_t)tr;
        psg_ch[2].voice = (uint8_t)(v - t->v);
        psg_set_vol(2, 15);
    }
    if (psg_chip.ti != (t->p[P_E7] != 0)) {      /* LFSR: the last note's */
        psg_chip.ti = (uint8_t)(t->p[P_E7] != 0);
        psg_sh_noise = 0xFF;
    }
    if (ch == PSG_NOISE) {
        static const uint8_t RATE[4] = {3, 0, 1, 2};   /* TRK HI MID LO -> the register's 3 0 1 2 */
        psg_chip.nz_seq = (uint8_t)(w == PW_METAL ? PSG_NZ_METAL : w == PW_BUZZ ? PSG_NZ_BUZZ : PSG_NZ_CHIP);
        psg_set_noise((w == PW_PERIOD ? 0u : 4u) | RATE[clamp(t->p[P_E1], 0, 3)], 1);   /* (its key: a fresh sequence) */
    } else
        psg_chip.shape[ch] = (uint8_t)w;          /* PW_SQR..PW_RSAW = PSG_SQR..PSG_RSAW */
    psg_set_vol(ch, 15);                         /* (its volume from its first render: one block on) */
    v->s[0] = (int32_t)ch + 1;
    v->s[1] = 0;
}

/* the channels: an owner gone frees its channel (off); each owner's divider and volume */
static void psg_upkeep(void)
{
    uint32_t ch;
    for (ch = 0; ch < PSG_CHANNELS; ch++) {
        psg_chan_t *c = &psg_ch[ch];
        track_t *t = &trk[c->part % NPART];
        voice_t *v = &t->v[c->voice % NVOICE];
        if (!c->busy || (ch == 2u && c->trk3))
            continue;
        if (!v->active || v->stage == 4u || t->engine != ENGI_PSG || v->s[0] != (int32_t)ch + 1) {
            if (c->trk3)
                psg_ch[2].busy = psg_ch[2].trk3 = 0;
            c->busy = c->trk3 = 0;
            psg_set_vol(ch, 15);
            continue;
        }
        if (ch == PSG_NOISE) {
            static const uint8_t RATE[4] = {3, 0, 1, 2};
            uint32_t w = (uint32_t)clamp(t->p[P_E0], 0, PW_N - 1);
            if (psg_noise_wave(w))                /* (NOIS or WHITE / PERIOD turned while it sounds) */
                psg_set_noise((w == PW_PERIOD ? 0u : 4u) | RATE[clamp(t->p[P_E1], 0, 3)], 0);
            if (c->trk3)                          /* TRK: the note through tone 3 (periodic: / 16 or 15 more) */
                psg_set_tone(2, psg_divider(v->s[2], v->s[5], (psg_sh_noise & 4u) ? 16u : psg_chip.ti ? 15u : 16u));
        } else {
            uint32_t w = (uint32_t)clamp(t->p[P_E0], 0, PW_N - 1);
            if (!psg_noise_wave(w))
                psg_chip.shape[ch] = (uint8_t)w;  /* (WAVE turned among the tone shapes while it sounds) */
            psg_set_tone(ch, psg_divider(v->s[2], v->s[5], 1u));
        }
        psg_set_vol(ch, (uint32_t)v->s[4] & 15u);
    }
}

/* ------------------------------------------------------ the block, before the parts --- */
static volatile uint8_t psg_pending;  /* a block to render (psg_mix_prepare): the second core takes it with OPN2's
                                       * channels when it has a job (eng_opn2.c op2_c1_job), else psg_mix_finish */
static uint32_t psg_mask;             /* .. its channels */

/* the block's render: the chip, then each channel's high-pass (~55 Hz) and the voice level */
__attribute__((noinline)) static void psg_mix_render(void)   /* (out of line: its own target budget) */
{
    uint32_t ch, i;
    psg_render(&psg_chip, psg_out44, CTL, psg_mask);
    for (ch = 0; ch < PSG_CHANNELS; ch++) {
        int32_t hp = psg_hp[ch];
        if (!(psg_mask >> ch & 1u))
            continue;
        for (i = 0; i < CTL; i++) {
            int32_t y = psg_out44[ch][i] - (hp >> 7);
            hp += y;
            psg_out44[ch][i] = y * (VOICE_FS / 64) >> 7;   /* (a square at full: +-4096 -> +-VOICE_FS / 2) */
        }
        psg_hp[ch] = hp;
    }
    psg_pending = 0;
}
static void psg_c1_hook(void)          /* (the second core, after OPN2's channels 4..6) */
{
    if (psg_pending)
        psg_mix_render();
}

/* once a block, first (fx.c mix_block: before OPN2's, whose second-core job may render it): the channels, the
 * waiting notes, the registers; a block to render: psg_pending */
__attribute__((noinline)) static void psg_mix_prepare(void)   /* (out of line: its own target budget) */
{
    uint32_t p, ch, any = 0, mask = 0, i;
    for (p = 0; p < NPART; p++)
        any |= trk[p].engine == ENGI_PSG;
    for (ch = 0; ch < PSG_CHANNELS; ch++)
        any |= psg_ch[ch].busy;
    if (!any)
        return;                                  /* (no PSG track: no chip) */
    if (!psg_ready) {
        psg_reset(&psg_chip, 0);
        memset(psg_sh_tone, 0, sizeof psg_sh_tone);
        memset(psg_sh_vol, 15, sizeof psg_sh_vol);
        psg_sh_noise = 0xFF;
        psg_ready = 1;
    }
    psg_upkeep();
    for (p = 0; p < NPART; p++) {                /* the notes that wait for a channel */
        track_t *t = &trk[p];
        if (t->engine != ENGI_PSG)
            continue;
        for (i = 0; i < NVOICE; i++)
            if (t->v[i].active && t->v[i].s[1])
                psg_start(t, &t->v[i]);
    }
    for (ch = 0; ch < PSG_CHANNELS; ch++)
        mask |= (uint32_t)(psg_ch[ch].busy && !(ch == 2u && psg_ch[ch].trk3)) << ch;
    if (!mask) {
        psg_idle_blocks++;                       /* (nothing sounds: nothing to clock) */
        return;
    }
    psg_mask = mask;
    psg_pending = 1;
}
/* after OPN2's block (fx.c mix_block): the render, when the second core has not done it */
static void psg_mix_finish(void)
{
    if (psg_pending)
        psg_mix_render();
}
static void psg_mix_begin(void)                 /* (both: the tests, and any caller without OPN2's) */
{
    psg_mix_prepare();
    psg_mix_finish();
}

/* ------------------------------------------------------------ the voice --- */
static void psg_note_on(track_t *t, voice_t *v)
{
    v->s[1] = 1;                                 /* its channel at the next block start (psg_mix_begin) */
    v->s[2] = v->pitch_cur;
    v->s[3] = 0;                                 /* (blocks since its key: the frames of its macros) */
    v->s[4] = 15;
    v->s[5] = 0;
    (void)t;
}

/* DRV 1..15: the driver's stepped decay, under the ADSR (which still gates the note) */
static int32_t psg_amp(track_t *t, voice_t *v, int32_t adsr)
{
    uint32_t drv = (uint32_t)clamp(t->p[P_E2], 0, 15), f, s;
    if (!drv)
        return adsr;
    f = (uint32_t)v->s[3] * PSG_FRAME_Q16 >> 16;
    s = f / drv;
    return s >= 15u ? 0 : (int32_t)(((int64_t)adsr * PSG_VOL[s]) >> 13);   /* (8191 / 8192: no divide) */
}

static void psg_voice_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    int32_t ch = v->s[0] - 1;
    uint32_t i, f = (uint32_t)v->s[3] * PSG_FRAME_Q16 >> 16;
    const int32_t *x;
    v->s[2] = psg_pitch_at(t, m->pitch16, f);    /* (the next block's divider: the base, its macros) */
    v->s[5] = m->fine;
    v->s[4] = (int32_t)psg_att_of(m->amp1);      /* (its volume in the chip's steps; ADSR or DRV, the velocity) */
    v->s[3]++;
    if (ch < 0 || ch >= (int32_t)PSG_CHANNELS || !psg_ch[ch].busy || n != CTL)
        return;
    x = psg_out44[ch];
    for (i = 0; i < CTL; i++)
        out[i] += x[i];
}

/* --------------------------------------------------------- the engine --- */
static const char *const N_PSG_WAVE[] = {"SQUARE", "PLS 25", "PLS 12", "PLS 6", "SAW", "TRI", "R-SAW", "WHITE",
                                         "PERIOD", "METAL", "BUZZ", 0};
static const char *const N_PSG_NOIS[] = {"TRK", "HI", "MID", "LO", 0};
static const char *const N_PSG_DRV[] = {"ADSR", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13",
                                        "14", "15", 0};
static const char *const N_PSG_ARP[] = {"OFF", "OCT", "MAJ", "MIN", "SUS4", "5TH", "MAJ7", "MIN7", "DIM", "AUG",
                                        "OCT2", 0};
static const char *const N_PSG_LFSR[] = {"SEGA", "TI", 0};

/* WAVE's card: WAVE over the chip's own waves, WAVE+ over the extension's (the authentic mode marked; the EDIT 1 panel
 * spells it: ui_psg.c) */
static const param_desc_t PSG_WAVE_CHIP = {"WAVE", F_ENUM, 0, PW_N - 1, 0, N_PSG_WAVE, 0};
static const param_desc_t PSG_WAVE_EXT = {"WAVE+", F_ENUM, 0, PW_N - 1, 0, N_PSG_WAVE, 0};
static const param_desc_t *psg_desc(const track_t *t, uint32_t k)
{
    if (k != 0u)
        return 0;
    return psg_chip_wave((uint32_t)clamp(t->p[P_E0], 0, PW_N - 1)) ? &PSG_WAVE_CHIP : &PSG_WAVE_EXT;
}

#include "psg_factory.h"                        /* PSG_PRESETS[]: original sounds */

static const engine_t ENG_PSG = {
    .name = "PSG",
    .page_title = {"PSG", "MACROS"},
    .edit = {
        {"WAVE", F_ENUM, 0, PW_N - 1, 0, N_PSG_WAVE, 0},
        {"NOIS", F_ENUM, 0, 3, 0, N_PSG_NOIS, 0},
        {"DRV", F_ENUM, 0, 15, 0, N_PSG_DRV, 0},
        {"ARP", F_ENUM, 0, 10, 0, N_PSG_ARP, 0},
        {"ARPS", F_INT, 1, 8, 2, 0, 0},
        {"VIB", F_INT, 0, 15, 0, 0, 0},
        {"SWP", F_INT, -64, 63, 0, 0, 0},
        {"LFSR", F_ENUM, 0, 1, 0, N_PSG_LFSR, 0},
    },
    .presets = PSG_PRESETS,
    .npresets = NELEM(PSG_PRESETS),
    .note_on = psg_note_on,
    .render = psg_voice_render,
    .amp = psg_amp,
    .desc = psg_desc,
    .knob = {P_E0, P_E2, P_E3, P_E6},
    .poly = PSG_CHANNELS,
    .keep = 0x01,                               /* s[0]: the channel (a retrigger keeps it) */
};
