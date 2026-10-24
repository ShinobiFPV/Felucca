/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 William Kew, ShinTech Electronics */
/* PCM8: the 16BIT mode's sample track, the console's 8-bit DAC (its FM channel 6 turned sample player) as drivers used
 * it: a few voices mixed in software and written to one 8-bit DAC at a fixed rate.
 *
 * Its sounds are Felucca's own, nothing new kept in flash (the flash and the pool have little to spare; the user's
 * material goes in the three user slots the editor already uploads, records and trims):
 *   SRC STD .. PERC    a DRUM kit (eng_drum.c; its nine, not the retired aliases): its synthesized voices, the GM map
 *   SRC BREAK          SLICE's built-in loop (eng_slice.c), its hits as slices
 *   SRC USR1..USR3     a user slot, sliced by SLICE (AUTO onsets, or its MAN slices when it has them): one hit per
 *                      slice, in GM order on the keys (slice 1 the kick 36, 2 the snare 38, 3 the closed hat 42,
 *                      4 the open hat 46, 5 the clap 39, 6 the low tom 45, 7 the high tom 50, 8 the crash 49, ..):
 *                      a kit recorded or uploaded as a row of hits plays as a GM kit
 * A voice is the source engine's own voice (its note_on, render and amp, with PCM8's values put in its EDIT places for
 * the call), rendered into the part's mix (engine memory); after the part's voices (engine_t.post) the mix goes
 * through the DAC: sampled and held at RATE (12 / 24 / 48 kHz, no interpolation: the grit of a low rate), rounded to
 * BITS (8, or 4 for more crunch), the ladder step at zero (LADR: the console's DAC's crossover), the output's shaping
 * (LGCY, as OPN2's: CLEAN, EARLY ~3.4 kHz two poles, LATE ~9 kHz one).
 * MODE: 3 VOICE (three hits at once, the oldest taken) or 1 SHOT (one stream: a hit cuts the one before, as a driver
 * with one DAC channel). The DAC is PCM8's own: it does not take OPN2's channel 6 (an option for later).
 * The source's material changes under the part's memory, so a change of SRC between a kit and a slice source clears
 * it and ends the voices (pcm8_block). */

#ifndef ENGI_PCM8
#define ENGI_PCM8 16u                 /* engines.c ENGINES[] (append-only) */
#endif
/* the sources: DRUM's nine kits (their stored KIT values), BREAK, USR1..3 */
static const uint8_t PCM8_KITS[] = {DK_STD, DK_80, DK_10, DK_66, DK_55, DK_77, DK_DEEP, DK_TIGHT, DK_PERC};
#define PCM8_NKIT ((uint32_t)NELEM(PCM8_KITS))
#define PCM8_SRC_BREAK ((int32_t)PCM8_NKIT)
#define PCM8_NSRC (PCM8_NKIT + 4u)
#if FELUCCA_SLICE
#define PCM8_WRAP_MEM (sizeof(slc_rbuf_t) > sizeof(drum_lane_t) * DV_NLANE ? sizeof(slc_rbuf_t) : sizeof(drum_lane_t) * DV_NLANE)
#else
#define PCM8_WRAP_MEM (sizeof(drum_lane_t) * DV_NLANE)
#endif

typedef struct {                      /* the part's DAC, after the source engine's memory */
    int32_t mix[CTL];                 /* the voices' sum this block */
    uint32_t ph;                      /* the DAC clock, Q16 ticks per sample */
    int32_t hold;                     /* the level it holds */
    int32_t lp1, lp2;                 /* LGCY's poles */
    int16_t src;                      /* the SRC its memory holds (-1: none yet) */
    uint8_t src_ok;
} pcm8_dac_t;
#define PCM8_MEM (PCM8_WRAP_MEM + sizeof(pcm8_dac_t))
ENG_MEM_FITS("PCM8", PCM8_MEM);
static pcm8_dac_t *pcm8_dac(const track_t *t) { return (pcm8_dac_t *)((uint8_t *)ENG_MEM((uint32_t)(t - trk) % NPART) + PCM8_WRAP_MEM); }

static int pcm8_is_kit(const track_t *t) { return t->p[P_E0] < PCM8_SRC_BREAK || !FELUCCA_SLICE; }

/* PCM8's values into the source engine's EDIT places for one call (save: the 8 to put back) */
static void pcm8_swap(track_t *t, int16_t *save)
{
    int16_t *p = &t->p[P_E0];
    int32_t tune = clamp(p[4], -12, 12), dcay = clamp(p[5], 0, 127);
    memcpy(save, p, 8u * sizeof *p);
    if (pcm8_is_kit(t)) {                        /* DRUM: KIT TUNE TONE DECY SNAP ACC KICK DRV (asked before p[0] changes) */
        p[0] = (int16_t)PCM8_KITS[clamp(save[0], 0, (int32_t)PCM8_NKIT - 1)];
        p[1] = (int16_t)clamp(64 + tune * 64 / 12, 0, 127);
        p[2] = 64;
        p[3] = (int16_t)dcay;
        p[4] = 64;
        p[5] = 100;
        p[6] = 0;
        p[7] = 0;
    }
#if FELUCCA_SLICE
    else {                                       /* SLICE: SRC DIV START PTCH MODE REV DCAY TONE */
        uint32_t src = (uint32_t)(save[0] - PCM8_SRC_BREAK);   /* 0 BREAK, 1..3 USR1..3: SLICE's own numbers */
        const slc_src_t *s = slc_get(src);
        p[0] = (int16_t)src;
        p[1] = (int16_t)(s && slc_man_of(s) ? SLC_DIV_MAN : SLC_DIV_AUTO);
        p[2] = 0;
        p[3] = (int16_t)tune;
        p[4] = SLC_ONE;
        p[5] = 0;
        p[6] = (int16_t)(dcay >= 64 ? 127 : dcay * 2);   /* (64 and over: the whole hit) */
        p[7] = 127;
    }
#endif
}
static void pcm8_unswap(track_t *t, const int16_t *save) { memcpy(&t->p[P_E0], save, 8u * sizeof *save); }

#if FELUCCA_SLICE
/* a GM note's slice (the order a user kit is laid out in), -1: none (then the note's place round the slices) */
static int32_t pcm8_gm_slice(uint32_t note)
{
    switch (note) {
    case 35: case 36: return 0;                  /* kick */
    case 38: case 40: return 1;                  /* snare */
    case 42: case 44: return 2;                  /* closed hat, pedal hat */
    case 46: return 3;                           /* open hat */
    case 39: return 4;                           /* clap */
    case 41: case 43: case 45: return 5;         /* low toms */
    case 47: case 48: case 50: return 6;         /* high toms */
    case 49: case 52: case 55: case 57: return 7;   /* crashes */
    case 37: return 8;                           /* rim */
    case 51: case 53: case 59: return 9;         /* rides */
    case 56: return 10;                          /* cowbell */
    case 54: return 11;                          /* tambourine */
    default: return -1;
    }
}
static const uint8_t PCM8_GM_KEYS[12] = {36, 38, 42, 46, 39, 45, 50, 49, 37, 51, 56, 54};   /* slice k's note */
#endif

static void voice_kill(voice_t *v);             /* (voice.c) */

/* once a block, before the voices: a change of SRC between a kit and a slice source clears the memory, ends the
 * voices (their state was the other engine's) */
static void pcm8_block(track_t *t)
{
    pcm8_dac_t *d = pcm8_dac(t);
    int32_t src = t->p[P_E0];
    uint32_t i;
    if (d->src_ok && (d->src < PCM8_SRC_BREAK) == (src < PCM8_SRC_BREAK) && (d->src < PCM8_SRC_BREAK || d->src == src))
        return;
    for (i = 0; i < NVOICE; i++)
        if (t->v[i].active)
            voice_kill(&t->v[i]);
    memset(ENG_MEM((uint32_t)(t - trk) % NPART), 0, PCM8_WRAP_MEM);
    d->src = (int16_t)src;
    d->src_ok = 1;
}

static void pcm8_note_on(track_t *t, voice_t *v)
{
    int16_t save[8];
    uint32_t i;
    int kit = pcm8_is_kit(t);                   /* (before the swap: it puts the source engine's values there) */
    if (t->p[P_E3]) {                            /* 1 SHOT: one stream, the new hit cuts the old */
        for (i = 0; i < NVOICE; i++)
            if (&t->v[i] != v && t->v[i].active)
                voice_kill(&t->v[i]);
    }
    pcm8_swap(t, save);
    if (kit)
        drum_note_on(t, v);
#if FELUCCA_SLICE
    else {
        uint32_t note = v->note, n;
        const slc_src_t *s = slc_get((uint32_t)t->p[P_E0]);
        int32_t j = pcm8_gm_slice(note);
        n = s ? slc_count(s, (uint32_t)t->p[P_E1]) : 1u;
        if (j < 0 || (uint32_t)j >= n)
            j = (int32_t)(note % (n ? n : 1u));
        v->note = (uint8_t)clamp(SLC_BASE + t->p[P_ROOT] + j, 0, 127);   /* (SLICE's key for slice j, START 0) */
        slice_note_on(t, v);
        v->note = (uint8_t)note;
    }
#endif
    pcm8_unswap(t, save);
}

static int32_t pcm8_amp(track_t *t, voice_t *v, int32_t adsr)
{
    int16_t save[8];
    int32_t a;
    int kit = pcm8_is_kit(t);
    pcm8_swap(t, save);
#if FELUCCA_SLICE
    a = kit ? drum_amp(t, v, adsr) : slice_amp(t, v, adsr);
#else
    a = drum_amp(t, v, adsr);
#endif
    pcm8_unswap(t, save);
    return a;
}

static void pcm8_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    int32_t b[CTL];
    int16_t save[8];
    pcm8_dac_t *d = pcm8_dac(t);
    uint32_t i;
    int kit = pcm8_is_kit(t);
    (void)out;
    if (n != CTL)
        return;
    memset(b, 0, sizeof b);
    pcm8_swap(t, save);
#if FELUCCA_SLICE
    if (kit)
        drum_render(t, v, b, n, m);
    else
        slice_render(t, v, b, n, m);
#else
    drum_render(t, v, b, n, m);
#endif
    pcm8_unswap(t, save);
    for (i = 0; i < CTL; i++)                    /* into the part's mix: the DAC takes it after the voices */
        d->mix[i] += b[i];
}

/* after the voices: the mix through the DAC into out */
static void pcm8_post(track_t *t, int32_t *out, uint32_t n)
{
    static const uint32_t RATE_Q16[3] = {(12000u << 16) / FS, (24000u << 16) / FS, (48000u << 16) / FS};
    pcm8_dac_t *d = pcm8_dac(t);
    const int16_t *p = &t->p[P_E0];
    uint32_t i, rq = RATE_Q16[clamp(p[1], 0, 2)], bits4 = p[2] != 0, model = (uint32_t)clamp(p[7], 0, 2);
    /* full scale 2 VOICE_FS (two voices at their peak): an 8-bit step = full / 128 = PCM8_STEP; inv: 128 / full, Q16 */
#define PCM8_STEP (2 * VOICE_FS / 128)
    int32_t inv = (int32_t)((128u << 16) / (2u * VOICE_FS)), ladr = clamp(p[6], 0, 127) * PCM8_STEP / 64;   /* 64: a step */
    int32_t l1 = d->lp1, l2 = d->lp2, hold = d->hold;
    uint32_t ph = d->ph;
    if (n != CTL)
        return;
    for (i = 0; i < CTL; i++) {
        int32_t y;
        ph += rq;
        if (ph >> 16) {                       /* a DAC tick in this sample: it takes the mix as it is now */
            int32_t q = clamp((d->mix[i] * inv + 0x8000) >> 16, -128, 127);
            ph &= 0xFFFFu;
            if (bits4)
                q = clamp((q + 8) >> 4, -8, 7) * 16;   /* (16 levels, zero one of them: silence stays 0) */
            hold = q * PCM8_STEP + (q > 0 ? ladr : q < 0 ? -ladr : 0);   /* the ladder: a step away from zero */
        }
        y = hold;
        if (model == 1u) {                       /* EARLY: two poles near 3.4 kHz */
            l1 += ((y - l1) * 12583) >> 15;
            l2 += ((l1 - l2) * 12583) >> 15;
            y = l2;
        } else if (model == 2u) {                /* LATE: one pole near 9 kHz */
            l1 += ((y - l1) * 23660) >> 15;
            y = l1;
        }
        out[i] += y;
        d->mix[i] = 0;
    }
    d->ph = ph;
    d->hold = hold;
    d->lp1 = l1;
    d->lp2 = l2;
}

/* the panel's keys: a kit's as DRUM's (its GM map); a slice source's, slice k on key k (in GM order) */
static int32_t pcm8_keys(const track_t *t, uint32_t k)
{
    if (pcm8_is_kit(t)) {
        int16_t save[8];
        int32_t r;
        pcm8_swap((track_t *)t, save);
        r = drum_keys(t, k);
        pcm8_unswap((track_t *)t, save);
        return r;
    }
#if FELUCCA_SLICE
    return k < NELEM(PCM8_GM_KEYS) ? PCM8_GM_KEYS[k] : -1;
#else
    return -1;
#endif
}

/* ------------------------------------------------------------ the engine --- */
static const char *const N_PCM8_RATE[] = {"12K", "24K", "48K", 0};
static const char *const N_PCM8_BITS[] = {"8", "4", 0};
static const char *const N_PCM8_MODE[] = {"3 VOICE", "1 SHOT", 0};
static const char *const N_PCM8_LGCY[] = {"CLEAN", "EARLY", "LATE", 0};
static const char *const N_PCM8_SRC[] = {"STD", "80", "10", "66", "55", "77", "DEEP", "TIGHT", "PERC", "BREAK", "USR1",
                                         "USR2", "USR3", 0};
typedef char pcm8_src_names[NELEM(N_PCM8_SRC) == PCM8_NSRC + 1u ? 1 : -1];   /* (PCM8_KITS, then the slices) */

#include "pcm8_factory.h"                       /* PCM8_PRESETS[] */

static const engine_t ENG_PCM8 = {
    .name = "PCM8",
    .page_title = {"PCM8", "DAC"},
    .edit = {
        {"SRC", F_ENUM, 0, PCM8_NSRC - 1u, 0, N_PCM8_SRC, 0},
        {"RATE", F_ENUM, 0, 2, 1, N_PCM8_RATE, 0},
        {"BITS", F_ENUM, 0, 1, 0, N_PCM8_BITS, 0},
        {"MODE", F_ENUM, 0, 1, 0, N_PCM8_MODE, 0},
        {"TUNE", F_SEMI, -12, 12, 0, 0, 0},
        {"DCAY", F_PCT, 0, 127, 64, 0, 0},
        {"LADR", F_PCT, 0, 127, 64, 0, 0},
        {"LGCY", F_ENUM, 0, 2, 1, N_PCM8_LGCY, 0},
    },
    .presets = PCM8_PRESETS,
    .npresets = NELEM(PCM8_PRESETS),
    .note_on = pcm8_note_on,
    .render = pcm8_render,
    .amp = pcm8_amp,
    .block = pcm8_block,
    .post = pcm8_post,
    .keys = pcm8_keys,
    .knob = {P_E0, P_E1, P_E2, P_E5},
    .poly = 3,
    .sampled = 1,
    .oneshot = 1,
    .mem = (uint16_t)PCM8_MEM,
};
