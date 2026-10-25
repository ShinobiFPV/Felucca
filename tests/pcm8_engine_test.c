/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 William Kew, ShinTech Electronics */
/* PCM8 engine test (src/eng_pcm8.c over DRUM's and SLICE's voices) on the host, through hostsim.c.
 *   build/shintech/host/pcm8_engine_test [DEMODIR]          (tests/run_shintech.sh: build/shintech/pcm8_demo)
 * 1. the DAC: RATE 12 / 24 / 48 kHz holds its level that long (the changes a second); BITS 8 / 4: every level on the
 *    8-bit grid (16 levels at 4), the ladder's step at zero (LADR); LGCY smooths (EARLY the most)
 * 2. the kits: each of the nine plays the GM kick, snare and hat, and its hits end by themselves
 * 3. the slices: BREAK's GM keys play its slices in GM order (36 slice 1, 38 slice 2, 42 slice 3)
 * 4. MODE 1 SHOT: one stream (a hit cuts the one before); 3 VOICE: three at once
 * 5. SRC from a kit to a slice source and back: the voices end, the memory is cleared, the new source plays
 * 6. sound: every factory preset with material sounds, no DC, no clipping, frees its voices
 * 7. demos into DEMODIR: every factory preset playing BEAT */
#define main hostsim_main
#include "hostsim.c"
#undef main
#include <math.h>

static int bad;
static void check(const char *what, int ok)
{
    printf("pcm8: %-92s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}
static void setup(uint32_t t, uint32_t pi)
{
    memset(trk, 0, sizeof trk);
    host_tracks_init();
    host_preset(&trk[t], ENGI_PCM8, pi);
    trk[t].p[P_CHOR] = trk[t].p[P_DLY] = trk[t].p[P_REV] = trk[t].p[P_DIST] = 0;
}
static void block(uint32_t t, double *y)
{
    static int32_t b[CTL];
    uint32_t k;
    track_render(&trk[t], b, CTL);
    for (k = 0; k < CTL && y; k++)
        y[k] = b[k];
}
static uint32_t active_voices(uint32_t t)
{
    uint32_t i, n = 0;
    for (i = 0; i < NVOICE; i++) n += trk[t].v[i].active;
    return n;
}
static double rms(const double *y, uint32_t n)
{
    double s = 0;
    uint32_t i;
    for (i = 0; i < n; i++) s += y[i] * y[i];
    return n ? sqrt(s / n) : 0;
}

/* ------------------------------------------------------------- 1 the DAC --- */
static void dac(void)
{
    static const double RATE[3] = {12000, 24000, 48000};
    uint32_t r, i, b, ok = 1, grid = 1, lv4 = 0;
    int32_t out[CTL];
    setup(0, 0);
    for (r = 0; r < 3u; r++) {                   /* a sine in the mix: its held steps a second */
        uint32_t changes = 0;
        int32_t prev = 0x7FFFFFFF;
        pcm8_dac_t *d = pcm8_dac(&trk[0]);
        trk[0].p[P_E1] = (int16_t)r;
        trk[0].p[P_E2] = 0;
        trk[0].p[P_E6] = 0;
        trk[0].p[P_E7] = 0;                       /* CLEAN: the held levels themselves */
        for (b = 0; b < 1378u; b++) {
            for (i = 0; i < CTL; i++)
                d->mix[i] = (int32_t)(VOICE_FS * sin(2 * M_PI * 3000.0 * (b * CTL + i) / FS));   /* 3 kHz: every tick moves */
            memset(out, 0, sizeof out);
            pcm8_post(&trk[0], out, CTL);
            for (i = 0; i < CTL; i++) {
                changes += out[i] != prev;
                prev = out[i];
                if (out[i] % PCM8_STEP) grid = 0;
            }
        }
        printf("pcm8: RATE %.0f: %u level changes in 1 s\n", RATE[r], changes);
        ok &= fabs(changes - (r == 2u ? 44100.0 : RATE[r])) < (r == 2u ? 2000.0 : RATE[r] * 0.02);
    }
    check("RATE 12K / 24K / 48K: the level changes 12,000 / 24,000 / every sample a second (+-2 %)", ok);
    check("BITS 8, LADR 0: every level on the 8-bit grid (2 VOICE_FS / 128)", grid);
    {
        int32_t seen[64], ns = 0, k;
        pcm8_dac_t *d = pcm8_dac(&trk[0]);
        trk[0].p[P_E1] = 2;
        trk[0].p[P_E2] = 1;                       /* 4 bits */
        for (b = 0; b < 200u; b++) {
            for (i = 0; i < CTL; i++)
                d->mix[i] = (int32_t)(2 * VOICE_FS * sin(2 * M_PI * 440.0 * (b * CTL + i) / FS));
            memset(out, 0, sizeof out);
            pcm8_post(&trk[0], out, CTL);
            for (i = 0; i < CTL; i++) {
                for (k = 0; k < ns && seen[k] != out[i]; k++)
                    ;
                if (k == ns && ns < 64) seen[ns++] = out[i];
            }
        }
        lv4 = (uint32_t)ns;
        printf("pcm8: BITS 4: %u distinct levels\n", lv4);
        check("BITS 4: 16 levels at most (a full-scale sine reaches >= 14 of them)", lv4 <= 16u && lv4 >= 14u);
    }
    {
        pcm8_dac_t *d = pcm8_dac(&trk[0]);
        int32_t o0, o1;
        trk[0].p[P_E2] = 0;
        trk[0].p[P_E6] = 64;                      /* LADR 64: one step either side of zero */
        for (i = 0; i < CTL; i++) d->mix[i] = 1;  /* a whisper above zero: q 0 */
        memset(out, 0, sizeof out);
        pcm8_post(&trk[0], out, CTL);
        o0 = out[CTL - 1];
        for (i = 0; i < CTL; i++) d->mix[i] = -PCM8_STEP;   /* q -1 */
        memset(out, 0, sizeof out);
        pcm8_post(&trk[0], out, CTL);
        o1 = out[CTL - 1];
        printf("pcm8: LADR 64: q 0 -> %d, q -1 -> %d (a step %d)\n", o0, o1, PCM8_STEP);
        check("LADR 64: the ladder's step away from zero: q 0 stays 0 (silence), q -1 a step further down", o0 == 0 &&
              o1 == -2 * PCM8_STEP);
    }
}

/* ------------------------------------------------------------- 2 the kits --- */
static void kits(void)
{
    static double y[1378 * CTL];
    static const uint8_t GM[3] = {36, 38, 42};
    uint32_t k, g, i, quiet = 0, hang = 0;
    for (k = 0; k < PCM8_NKIT; k++)
        for (g = 0; g < 3u; g++) {
            setup(0, 0);
            trk[0].p[P_E0] = (int16_t)k;
            block(0, 0);
            trk_note_on(&trk[0], GM[g], 110);
            for (i = 0; i < 345u; i++) block(0, y + i * CTL);   /* 1/4 s */
            if (rms(y, 345u * CTL) < VOICE_FS / 100.0) { quiet++; printf("pcm8: kit %s note %u quiet\n", N_PCM8_SRC[k], GM[g]); }
            for (i = 0; i < 4u * 1378u && active_voices(0); i++) block(0, 0);
            if (active_voices(0)) { hang++; printf("pcm8: kit %s note %u hangs\n", N_PCM8_SRC[k], GM[g]); }
        }
    check("kits: each of the nine plays the GM kick, snare and closed hat (RMS over 1 % of VOICE_FS)", !quiet);
    check("kits: every hit ends by itself within 4 s (one-shot)", !hang);
}

/* ------------------------------------------------------------ 3 the slices --- */
static void slices(void)
{
    static const uint8_t GM[3] = {36, 38, 42};
    uint32_t g, ok = 1, i, sound = 1;
    static double y[345 * CTL];
#if FELUCCA_SLICE
    for (g = 0; g < 3u; g++) {
        voice_t *v = 0;
        setup(0, 9);                             /* BREAK CHOPS */
        block(0, 0);
        trk_note_on(&trk[0], GM[g], 110);
        for (i = 0; i < NVOICE; i++)
            if (trk[0].v[i].active) v = &trk[0].v[i];
        if (!v || (((uint32_t)v->s[4] >> 8) & 0xFFu) != g || v->note != GM[g])
            printf("pcm8: note %u: voice %s, s[4] 0x%x (slice %u), note %u\n", GM[g], v ? "found" : "none",
                   v ? (unsigned)v->s[4] : 0u, v ? ((unsigned)v->s[4] >> 8) & 0xFFu : 0u, v ? v->note : 0u);
        ok &= v && (((uint32_t)v->s[4] >> 8) & 0xFFu) == g && v->note == GM[g];   /* (slice j in s[4]; the note kept) */
        for (i = 0; i < 345u; i++) block(0, y + i * CTL);
        sound &= rms(y, 345u * CTL) > VOICE_FS / 100.0;
    }
    check("slices: BREAK's GM keys 36 / 38 / 42 play slices 1 / 2 / 3, the voice's note kept", ok);
    check("slices: they sound", sound);
#else
    (void)g; (void)ok; (void)i; (void)sound; (void)y; (void)GM;
#endif
}

/* ---------------------------------------------------- 4 modes, 5 a source change --- */
static void modes(void)
{
    uint32_t i, n3, n1;
    setup(0, 0);                                 /* 3 VOICE */
    block(0, 0);
    trk_note_on(&trk[0], 36, 110);
    trk_note_on(&trk[0], 38, 110);
    trk_note_on(&trk[0], 42, 110);
    for (i = 0; i < 3u; i++) block(0, 0);
    n3 = active_voices(0);
    setup(0, 8);                                 /* ONE SHOT KIT */
    block(0, 0);
    trk_note_on(&trk[0], 36, 110);
    block(0, 0);
    trk_note_on(&trk[0], 38, 110);
    for (i = 0; i < 3u; i++) block(0, 0);
    n1 = active_voices(0);
    printf("pcm8: 3 VOICE: %u voices after three hits; 1 SHOT: %u after two\n", n3, n1);
    check("MODE: 3 VOICE plays three hits at once; 1 SHOT one stream (the second hit cut the first)", n3 == 3u && n1 == 1u);
#if FELUCCA_SLICE
    {
        static double y[345 * CTL];
        setup(0, 0);
        block(0, 0);
        trk_note_on(&trk[0], 36, 110);
        block(0, 0);
        trk[0].p[P_E0] = PCM8_SRC_BREAK;         /* a kit -> BREAK while the kick sounds */
        for (i = 0; i < 4u; i++) block(0, 0);
        n1 = active_voices(0);
        trk_note_on(&trk[0], 38, 110);
        for (i = 0; i < 345u; i++) block(0, y + i * CTL);
        check("SRC kit -> BREAK: the kit's voice ends, BREAK's snare slice plays", n1 == 0u && rms(y, 345u * CTL) > VOICE_FS / 100.0);
    }
#endif
}

/* ------------------------------------------------------------- 6 sound --- */
static void sound(void)
{
    static double y[1378 * CTL];
    uint32_t pi, i, quiet = 0, dc = 0, clip = 0, hang = 0;
    for (pi = 0; pi < NELEM(PCM8_PRESETS); pi++) {
        double mean = 0, peak = 0;
        int32_t src = PCM8_PRESETS[pi].e[0];
        if (src > PCM8_SRC_BREAK)                /* (the user slots: empty on the host, their sine) */
            continue;
        setup(0, pi);
        block(0, 0);
        trk_note_on(&trk[0], 36, 110);
        trk_note_on(&trk[0], 42, 90);
        for (i = 0; i < 1378u; i++) block(0, y + i * CTL);
        for (i = 0; i < 1378u * CTL; i++) { mean += y[i]; peak = fabs(y[i]) > peak ? fabs(y[i]) : peak; }
        mean /= 1378.0 * CTL;
        if (rms(y, 345u * CTL) < VOICE_FS / 100.0) { quiet++; printf("pcm8: %s quiet\n", PCM8_PRESETS[pi].name); }
        if (fabs(mean) > VOICE_FS * 0.02) { dc++; printf("pcm8: %s DC %.0f\n", PCM8_PRESETS[pi].name, mean); }
        if (peak > 2.2 * VOICE_FS) { clip++; printf("pcm8: %s peak %.0f\n", PCM8_PRESETS[pi].name, peak); }
        for (i = 0; i < 6u * 1378u && active_voices(0); i++) block(0, 0);
        hang += active_voices(0) != 0;
    }
    check("sound: every preset with material plays a kick and a hat", !quiet);
    check("sound: no DC (|mean| under 2 % of VOICE_FS), nothing past the DAC's full scale", !dc && !clip);
    check("sound: its voices free within 6 s", !hang);
}

/* ------------------------------------------------------------- 7 demos --- */
static void demo(const char *dir, uint32_t pi)
{
    static int32_t o[2 * CTL];
    const preset_t *pr = &PCM8_PRESETS[pi];
    uint32_t pat = pr->pat ? pr->pat - 1u : 11u, step = FS / 8u, s, i, frames = 0;
    char path[512], nm[32];
    FILE *w;
    for (i = 0; pr->name[i] && i < 31u; i++)
        nm[i] = pr->name[i] == ' ' ? '_' : pr->name[i];
    nm[i] = 0;
    snprintf(path, sizeof path, "%s/%02u_%s.wav", dir, pi + 1u, nm);
    if (!(w = fopen(path, "wb")))
        return;
    wav_hdr(w, 0);
    setup(0, pi);
    for (s = 0; s < 32u; s++) {
        uint32_t k = s % 16u, note = PATTERNS[pat].note[k];
        if (note)
            trk_note_on(&trk[0], note, PATTERNS[pat].flags[k] & 1u ? 120 : 95);
        for (i = 0; i < step; i += CTL) {
            uint32_t j;
            mix_block(o, CTL);
            for (j = 0; j < CTL; j++) wav_put(w, o[2 * j], o[2 * j + 1]);
            frames += CTL;
        }
    }
    for (i = 0; i < FS; i += CTL) {
        uint32_t j;
        mix_block(o, CTL);
        for (j = 0; j < CTL; j++) wav_put(w, o[2 * j], o[2 * j + 1]);
        frames += CTL;
    }
    fseek(w, 0, SEEK_SET);
    wav_hdr(w, frames);
    fclose(w);
}

/* 8. the DAC is FM channel 6: with a PCM8 track OPN2 has five channels, six again without */
static void dac_channel(void)
{
    static int32_t b[CTL];
    uint32_t i, busy = 0, c;
    memset(trk, 0, sizeof trk);
    host_tracks_init();
    host_preset(&trk[1], ENGI_OPN2, 13);          /* T2 OPN2 ORGAN */
    trk[1].p[P_VOICE] = V_POLY;
    host_preset(&trk[3], ENGI_PCM8, 0);           /* T4 PCM8 */
    op2_poll();
    for (i = 0; i < 6u; i++)
        trk_note_on(&trk[1], 48 + 2 * i, 100);
    for (i = 0; i < 4u; i++) {
        op2_mix_begin();
        track_render(&trk[1], b, CTL);
        track_render(&trk[3], b, CTL);
    }
    for (c = 0; c < OPN2_CHANNELS; c++) busy += op2_ch[c].busy;
    check("a PCM8 track: OPN2 has 5 channels (6 is the DAC), six notes keep five", op2_nchan() == 5u && busy == 5u &&
          !op2_ch[5].busy);
    host_preset(&trk[3], ENGI_DRUM, 0);
    for (i = 0; i < 300u; i++) {                  /* (the engine switch's fade) */
        op2_mix_begin();
        track_render(&trk[1], b, CTL);
        track_render(&trk[3], b, CTL);
    }
    check("no PCM8 track: six again", op2_nchan() == 6u);
}

/* 9. the hit cache: a kit hit kept as it plays, then played from the cache */
static voice_t *last_voice(void)
{
    uint32_t i;
    voice_t *v = 0;
    for (i = 0; i < NVOICE; i++)
        if (trk[0].v[i].active) v = &trk[0].v[i];
    return v;
}
static double hit(uint32_t note, uint32_t vel, double *y, uint32_t *mode)
{
    uint32_t i;
    voice_t *v;
    trk_note_on(&trk[0], note, vel);
    v = last_voice();
    *mode = v ? (uint32_t)v->s[7] : 99u;
    for (i = 0; i < 345u; i++) block(0, y ? y + i * CTL : 0);
    for (i = 0; i < 4u * 1378u && active_voices(0); i++) block(0, 0);
    return y ? rms(y, 345u * CTL) : 0;
}
static void cache(void)
{
    static double y1[345 * CTL], y2[345 * CTL];
    pcm8_cache_t *c;
    uint32_t m1, m2, m3, m4, i;
    double r1, r2, r3, d = 0, n = 0;
    setup(0, 0);                                 /* STD 8BIT */
    block(0, 0);
    c = pcm8_cache(&trk[0]);
    r1 = hit(36, 110, y1, &m1);
    r2 = hit(36, 110, y2, &m2);
    for (i = 0; i < 345u * CTL; i++) { d += (y1[i] - y2[i]) * (y1[i] - y2[i]); n += y1[i] * y1[i]; }
    printf("pcm8: cache: kick 1 %s (RMS %.0f), kick 2 %s (RMS %.0f), difference %.1f %% of its energy\n",
           m1 == PV_REC ? "live, kept" : "?", r1, m2 == PV_PLAY ? "from the cache" : "?", r2, 100.0 * d / (n ? n : 1));
    check("cache: the first kick plays live and is kept; the second plays from the cache, as the first (+-3 % of its energy)",
          m1 == PV_REC && m2 == PV_PLAY && c->ent[0].state == 2u && d < 0.03 * n && r2 > 0);
    r3 = hit(36, 55, y2, &m3);
    printf("pcm8: cache: velocity 55: RMS %.0f (110: %.0f)\n", r3, r2);
    check("cache: a softer hit from the cache plays softer (velocity 55: 30..70 % of velocity 110's RMS)", m3 == PV_PLAY &&
          r3 > 0.3 * r2 && r3 < 0.7 * r2);
    trk[0].p[P_E4] = 5;                          /* TUNE +5: another key */
    hit(36, 110, 0, &m4);
    check("cache: TUNE moved: the kick is another sound, kept anew (live)", m4 == PV_REC);
    trk[0].p[P_E4] = 0;
    trk[0].p[P_E1] = 2;                          /* 48K: the crash (long) outgrows its room */
    trk[0].p[P_E5] = 127;
    {
        uint32_t m5, m6;
        hit(49, 110, 0, &m5);
        hit(49, 110, 0, &m6);
        printf("pcm8: cache: the crash at 48K: first %u, second %u (0 live, 1 kept, 2 from the cache)\n", m5, m6);
        check("cache: a hit longer than its room stays live, whole (the crash at 48K, DCAY 127)", m6 != PV_PLAY && !c->rec);
    }
    trk[0].p[P_E1] = 1;
    trk[0].p[P_E5] = 64;
    {                                            /* the choke: a closed hat cuts an open one from the cache */
        uint32_t m7, k;
        hit(46, 110, 0, &m7);                    /* kept */
        trk_note_on(&trk[0], 46, 110);           /* from the cache */
        block(0, 0);
        trk_note_on(&trk[0], 42, 110);
        for (k = 0; k < 4u; k++) block(0, 0);
        {
            uint32_t open = 0;
            for (k = 0; k < NVOICE; k++)
                open += trk[0].v[k].active && trk[0].v[k].s[7] == PV_PLAY && c->ent[trk[0].v[k].s[6] % PCM8_NENT].note == 46u;
            check("cache: a closed hat chokes an open hat played from the cache", !open);
        }
    }
}

int main(int argc, char **argv)
{
    uint32_t pi;
    cache();
    dac_channel();
    dac();
    kits();
    slices();
    modes();
    sound();
    if (argc > 1) {
        for (pi = 0; pi < NELEM(PCM8_PRESETS); pi++)
            if (PCM8_PRESETS[pi].e[0] <= PCM8_SRC_BREAK)
                demo(argv[1], pi);
        printf("pcm8: demos in %s\n", argv[1]);
    }
    printf("pcm8_engine_test: %s\n", bad ? "FAILED" : "all checks ok");
    return bad != 0;
}
