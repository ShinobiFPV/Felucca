/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 William Kew, ShinTech Electronics */
/* POLY6: a six-voice-style analogue polysynth (ShinTech; docs/shintech/CHORD-PLAN.md, Phase 4). Included by engines.c.
 * Per voice: one DCO (a band-limited saw and pulse from the same phase, as one oscillator's two outputs), a square
 * sub oscillator an octave down, white noise, mixed into a 24 dB / octave low-pass ladder: four trapezoidal (TPT,
 * zero-delay-feedback) one-pole stages, the feedback solved linearly for the sample, then a soft clip in the loop (the
 * transistor ladder's tanh: it bounds the resonance, which self-oscillates near the top of RES; the bass thins as RES
 * rises, as a ladder's does). Over the part (the post hook, on the sum of its voices): the 4-position high-pass of the
 * original's signal path (BOOST: a low shelf up, FLAT, two rising corners) and the BBD chorus.
 *   EDIT 1 DCO    WAVE (SAW PULSE BOTH NONE)  PW (the pulse's width, 50 % .. 6 %)  SUB  NOISE
 *   EDIT 2 VCF    CUT  RES  KYBD (the cutoff follows the key, 127: one to one)  HPF (BOOST FLAT 1 2)
 *   POLY6 page    CHORUS (OFF I II I+II: P_P6CHO)  VOICES (4 or 8, the amp's ENV or GATE: P_P6MODE)  and the LFO's
 *                 RATE and DELAY (P_LRATE, P_LFADE: the track's LFO, its fade-in the delay)
 * The rest is the track's, as for every engine: one ADSR for the VCF (ENV DEST FLT: its amount, its sign the polarity)
 * and the VCA (VOICES ..GATE: the amp follows the key, the ADSR the filter only), the LFO's routings (LFO DEST PIT: the
 * DCO's LFO, FLT: the VCF's, SHP: the PWM; ENV DEST SHP: PWM by the envelope), LEVEL, the sends to the delay and reverb.
 * The chorus (poly6_post): a bucket-brigade delay modelled as one delay line (the part's input, band-limited as a BBD's
 * input filter does), read at two taps whose delays a triangle LFO sweeps in opposite directions, left and right: I
 * 0.5 Hz, II 0.83 Hz over 1.66 .. 5.35 ms, I+II a faster shallow 9.75 Hz around 3.3 ms; each tap through the output
 * filter's roll-off (one pole near 8 kHz), with a little noise (the BBD's hiss, -66 dB), mixed with the dry signal
 * left and right. Stereo through the host's side path (voice.c poly6_side, fx.c mix_spread: the part's sum is L + R,
 * the side buffer L). Its memory: the part's engine region (engines.c eng_mem). */
static const char *const N_P6_WAVE[] = {"SAW", "PULSE", "BOTH", "NONE"};
static const char *const N_P6_HPF[] = {"BOOST", "FLAT", "1", "2"};
enum { P6W_SAW, P6W_PULSE, P6W_BOTH, P6W_NONE };
enum { P6C_OFF, P6C_I, P6C_II, P6C_I_II };
enum { P6M_4ENV, P6M_8ENV, P6M_4GATE, P6M_8GATE };

/* --------------------------------------------------------------- the ladder --- */
/* the per-block coefficients: G = g / (1 + g) (Q12, g = tan(pi fc / FS)), G^4, the feedback k (Q10, 0..4.3) and
 * 1 / (1 + k G^4) (Q12) for the feedback's linear solve */
typedef struct { int32_t G, G4, k, inv; } p6_lad_t;
static void p6_lad_coef(p6_lad_t *c, int32_t cut, int32_t res)   /* cut 0..127 << 8, res 0..127 */
{
    int32_t i, g, G2;
    cut = clamp(cut, 0, 127 << 8);
    i = cut >> 8;
    g = SVF_G[i];
    if (i < 127)
        g += ((SVF_G[i + 1] - g) * (cut & 255)) >> 8;
    c->G = (g << 12) / (4096 + g);
    G2 = (c->G * c->G) >> 12;
    c->G4 = (G2 * G2) >> 12;
    c->k = res * 4400 / 127;                         /* (4.3 at 127: past 4, self-oscillation) */
    c->inv = (4096 << 12) / (4096 + ((c->k * c->G4) >> 10));
}
/* one sample: u in (about +-32768), s[4] the stages' states (twice the signal's scale, the products rounded: a
 * quieter floor at low cutoffs); returns the fourth stage (the low-pass) */
static inline int32_t p6_ladder(const p6_lad_t *c, int32_t u, int32_t *s)
{
    int32_t G = c->G, S, y, x, v, i;
    u <<= 1;
    /* the stages' outputs with x = 0: S_i = s_i (1 - G); their sum through the later stages */
    S = s[0] - ((s[0] * G + 2048) >> 12);
    S = ((S * G + 2048) >> 12) + s[1] - ((s[1] * G + 2048) >> 12);
    S = ((S * G + 2048) >> 12) + s[2] - ((s[2] * G + 2048) >> 12);
    S = ((S * G + 2048) >> 12) + s[3] - ((s[3] * G + 2048) >> 12);
    y = ((((c->G4 * u + 2048) >> 12) + S) * c->inv + 2048) >> 12;   /* the output, the feedback solved */
    x = softclip((u - ((c->k * y + 512) >> 10)) >> 1) << 1;         /* (the loop's saturation) */
    for (i = 0; i < 4; i++) {
        v = ((x - s[i]) * G + 2048) >> 12;
        y = v + s[i];
        s[i] = clamp(y + v, -300000, 300000);
        x = y;
    }
    return x >> 1;
}

/* ------------------------------------------------------------------- voices --- */
static void poly6_note_on(track_t *t, voice_t *v)
{
    (void)t;
    v->s[0] = v->s[1] = v->s[2] = v->s[3] = 0;       /* the ladder */
    if (!v->s[4])
        v->s[4] = 0x2468ACE + (int32_t)v->age;       /* noise */
}

static void poly6_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t wave = (uint32_t)p[P_E0] & 3u, i, inc = m->inc, isub = inc >> 1;
    int64_t pwm = 0x80000000LL - (int64_t)p[P_E1] * 0x00E00000LL + ((int64_t)(m->shape - (64 << 8)) << 15);
    uint32_t pw = (uint32_t)(pwm < 0x08000000LL ? 0x08000000LL : pwm > 0xF8000000LL ? 0xF8000000LL : pwm);   /* (6 %
                                                                                   * .. 94 % of the cycle high) */
    int32_t gs = wave == P6W_PULSE || wave == P6W_NONE ? 0 : 20000, gp = wave == P6W_SAW || wave == P6W_NONE ? 0 : 20000;
    int32_t gsub = p[P_E2] * 150, gn = p[P_E3] * 130;
    int32_t cut = (p[P_E4] << 8) + m->cutoff + ((p[P_E6] * (v->pitch16 - 60 * 16) * 151) >> 10);
    p6_lad_t lc;
    uint32_t ph = v->ph[0], phs = v->ph[1];
    int32_t st[4] = {v->s[0], v->s[1], v->s[2], v->s[3]}, nst = v->s[4];
    if (wave == P6W_BOTH)
        gs = gp = 14000;
    p6_lad_coef(&lc, cut, p[P_E5]);
    for (i = 0; i < n; i++) {
        int32_t s = 0;
        if (gs)
            s += mulq15(osc_saw(ph, inc), gs);
        if (gp)
            s += mulq15(osc_pulse(ph, inc, pw), gp);
        if (gsub)
            s += mulq15(osc_pulse(phs, isub, 0x80000000u), gsub);
        if (gn)
            s += mulq15((int32_t)(noise32(&nst) >> 17) - 16384, gn);
        ph += inc;
        phs += isub;
        s = p6_ladder(&lc, s, st);
        out[i] += voice_amp(soft_knee(s, 24000), m, i) * 2;
    }
    v->ph[0] = ph;
    v->ph[1] = phs;
    v->s[0] = st[0];
    v->s[1] = st[1];
    v->s[2] = st[2];
    v->s[3] = st[3];
    v->s[4] = nst;
}

/* VOICES ..GATE: the VCA follows the key (the ADSR goes on to the filter only); its release the block's ramp */
static int32_t poly6_amp(track_t *t, voice_t *v, int32_t adsr)
{
    return t->p[P_P6MODE] >= P6M_4GATE ? (v->gate ? 32767 : 0) : adsr;
}

/* ---------------------------------------------------------- HPF and chorus --- */
#define P6_DLEN 512u                                 /* the delay line: 11.6 ms at 44.1 kHz (5.35 ms + the sweep's) */
typedef struct {
    int16_t line[P6_DLEN];
    uint32_t w;                                      /* the write index */
    uint32_t lfo;                                    /* the triangle's phase */
    int32_t in_lp, out_l, out_r;                     /* the BBD's input filter, the taps' output filters */
    int32_t hp_lp;                                   /* the HPF's low-pass state */
    int32_t dc;                                      /* the DC blocker's (x 16) */
    int32_t nst;                                     /* the hiss */
    uint32_t live;                                   /* blocks the wet tail still sounds */
} p6_mem_t;
ENG_MEM_FITS("POLY6", sizeof(p6_mem_t));
static int32_t *poly6_side(track_t *t);              /* (voice.c: the part's side buffer, L) */

/* a chorus mode's sweep: the triangle's rate (phase a sample, 2^32 a cycle), the delay's centre and depth (samples
 * Q8): I 0.513 Hz, II 0.863 Hz, 1.66 .. 5.35 ms; I+II 9.75 Hz, 3.0 .. 3.6 ms */
static void p6_sweep(uint32_t mode, uint32_t *inc, int32_t *centre, int32_t *depth)
{
    static const uint16_t RATE_MHZ[4] = {0, 513, 863, 9750};
    *inc = (uint32_t)((uint64_t)RATE_MHZ[mode & 3u] * 4294967296ull / 1000u / FS);
    if (mode == P6C_I_II) {
        *centre = (int32_t)(3300u * FS / 1000u * 256u / 1000u);
        *depth = (int32_t)(300u * FS / 1000u * 256u / 1000u);
    } else {
        *centre = (int32_t)(3505u * FS / 1000u * 256u / 1000u);   /* (1.66 + 5.35) / 2 ms */
        *depth = (int32_t)(1845u * FS / 1000u * 256u / 1000u);    /* (5.35 - 1.66) / 2 ms */
    }
}
/* the delay line read d samples back (Q8: linearly between two) */
static inline int32_t p6_tap(const p6_mem_t *c, int32_t d)
{
    uint32_t at = c->w - 1u - (uint32_t)(d >> 8), f = (uint32_t)d & 255u;
    int32_t a = c->line[at & (P6_DLEN - 1u)], b = c->line[(at - 1u) & (P6_DLEN - 1u)];
    return a + (((b - a) * (int32_t)f) >> 8);
}
/* the part's delay at the triangle's phase ph, + / - its sweep (Q8 samples) */
static inline int32_t p6_delay(uint32_t ph, int32_t centre, int32_t depth, int sign)
{
    int32_t tri = (int32_t)((ph < 0x80000000u ? ph : ~ph) >> 15) - 32768;   /* -32768 .. 32767 */
    return centre + sign * ((tri * depth) >> 15);
}

static uint32_t poly6_post(track_t *t, int32_t *out, uint32_t n)
{
    p6_mem_t *c = (p6_mem_t *)ENG_MEM((uint32_t)(t - trk) % NPART);
    uint32_t mode = (uint32_t)clamp(t->p[P_P6CHO], 0, 3), hpf = (uint32_t)t->p[P_E7] & 3u, i, inc, any = 0;
    int32_t centre, depth, *side = poly6_side(t);
    static const int16_t HP_A[4] = {32418, 0, 31648, 29406};   /* the low-pass poles at 44.1 kHz: 75 Hz, -, 240 Hz, 720 Hz */
    int32_t ha = fe_pole(HP_A[hpf]);
    for (i = 0; i < n && !any; i++)
        any = out[i] != 0;
    if (any)
        c->live = 40;                                /* (the tail: the delay line and the filters, ~30 ms) */
    else if (!c->live || !--c->live)
        return 0;
    for (i = 0; i < n; i++) {                        /* the HPF over the voices' sum, AC coupled (~14 Hz: a PWM's subsonic swing too) */
        int32_t x = out[i];
        c->dc += ((x << 4) - c->dc) >> 9;
        x -= c->dc >> 4;
        if (hpf != 1u) {
            c->hp_lp += mulq15(x - c->hp_lp, 32767 - ha);
            x = hpf ? x - c->hp_lp : x + (c->hp_lp >> 1);   /* BOOST: +3.5 dB below ~75 Hz */
        }
        out[i] = x;
    }
    if (!mode)
        return 1;
    p6_sweep(mode, &inc, &centre, &depth);
    if (!c->nst)
        c->nst = 0x13579BD;
    for (i = 0; i < n; i++) {
        int32_t x = out[i], l, r;
        c->in_lp += mulq15((x >> 3) - c->in_lp, 32767 - fe_pole(11000));   /* the BBD's input filter (~8 kHz); the line
                                                                            * holds an eighth (16 bits: 8 voices fit) */
        c->line[c->w++ & (P6_DLEN - 1u)] = (int16_t)clamp(c->in_lp, -32768, 32767);
        c->lfo += inc;
        l = p6_tap(c, p6_delay(c->lfo, centre, depth, 1)) << 2;   /* (back to half the dry) */
        r = p6_tap(c, p6_delay(c->lfo, centre, depth, -1)) << 2;
        l += (int32_t)(noise32(&c->nst) >> 26) - 32;   /* (the hiss, ~-66 dB) */
        r += (int32_t)(noise32(&c->nst) >> 26) - 32;
        c->out_l += mulq15(l - c->out_l, 32767 - fe_pole(13000));      /* the output filter (~8 kHz) */
        c->out_r += mulq15(r - c->out_r, 32767 - fe_pole(13000));
        l = (x >> 1) + c->out_l;                     /* L, R: dry + wet (the line holds half the dry) */
        r = (x >> 1) + c->out_r;
        out[i] = l + r;                              /* the part: L + R, its side buffer L (fx.c mix_spread) */
        side[i] = l;
    }
    return 1;
}

/* -------------------------------------------------------------- presets --- */
/* 32 original sounds. e: WAVE PW SUB NOISE CUT RES KYBD HPF; env: ATK DEC SUS REL; fenv: ENV -> VCF (the sign: the
 * polarity); mono; the sends. Their chorus, voices and LFO are POLY6_X (engines.c applies them with the sound) */
static const preset_t POLY6_PRESETS[] = {
    {"WARM PAD", {2, 30, 40, 0, 52, 20, 40, 1}, {85, 90, 110, 95}, 12, 0, FX(0, 0, 25, 60)},
    {"GLASS PAD", {1, 70, 0, 0, 70, 50, 70, 2}, {70, 100, 100, 90}, 20, 0, FX(0, 0, 30, 70)},
    {"SLOW SWEEP", {0, 0, 50, 0, 30, 70, 50, 1}, {100, 110, 115, 100}, 40, 0, FX(0, 0, 35, 75)},
    {"DARK PAD", {2, 20, 70, 0, 38, 30, 30, 0}, {90, 90, 115, 100}, 6, 0, FX(0, 0, 20, 65)},
    {"STRINGS", {0, 0, 30, 0, 66, 10, 64, 2}, {75, 90, 112, 88}, 6, 0, FX(0, 0, 20, 60)},
    {"OCT STRINGS", {2, 40, 90, 0, 72, 8, 64, 2}, {70, 90, 112, 85}, 4, 0, FX(0, 0, 25, 62)},
    {"SOFT STRING", {0, 0, 40, 0, 56, 0, 50, 1}, {95, 90, 115, 96}, 0, 0, FX(0, 0, 20, 70)},
    {"BRASS", {0, 0, 40, 0, 40, 25, 70, 1}, {40, 72, 96, 50}, 46, 0, FX(0, 0, 15, 35)},
    {"SOFT BRASS", {2, 30, 30, 0, 44, 20, 60, 1}, {55, 80, 100, 60}, 36, 0, FX(0, 0, 20, 45)},
    {"BRASS STAB", {0, 0, 50, 0, 36, 35, 70, 2}, {10, 66, 60, 40}, 50, 0, FX(0, 0, 20, 30)},
    {"POLY STAB", {2, 50, 60, 0, 48, 40, 64, 2}, {0, 62, 30, 40}, 44, 0, FX(0, 0, 25, 30)},
    {"HOUSE STAB", {1, 60, 40, 0, 58, 55, 64, 2}, {0, 58, 0, 34}, 40, 0, FX(0, 0, 30, 25)},
    {"RAVE CHORD", {2, 20, 70, 0, 62, 45, 64, 2}, {0, 70, 60, 40}, 24, 0, FX(5, 0, 30, 25)},
    {"HOUSE ORGAN", {1, 0, 80, 0, 80, 10, 90, 2}, {2, 40, 100, 25}, 0, 0, FX(0, 0, 15, 25)},
    {"PERC ORGAN", {1, 40, 60, 0, 64, 20, 80, 2}, {0, 52, 60, 30}, 28, 0, FX(0, 0, 15, 25)},
    {"E-PIANO", {1, 80, 20, 0, 50, 15, 90, 1}, {0, 84, 20, 66}, 34, 0, FX(0, 0, 20, 40)},
    {"HOUSE PIANO", {2, 70, 30, 0, 56, 10, 90, 2}, {0, 80, 10, 60}, 40, 0, FX(0, 0, 25, 35)},
    {"CLAV", {1, 100, 0, 0, 46, 40, 80, 3}, {0, 60, 0, 40}, 52, 0, FX(0, 0, 15, 20)},
    {"PLUCK", {0, 0, 30, 0, 30, 35, 64, 2}, {0, 66, 0, 55}, 56, 0, FX(0, 0, 40, 35)},
    {"SYNC PLUCK", {2, 90, 0, 0, 34, 60, 70, 2}, {0, 60, 0, 50}, 60, 0, FX(0, 0, 45, 30)},
    {"HARP", {1, 30, 0, 0, 44, 20, 80, 2}, {0, 80, 0, 80}, 48, 0, FX(0, 0, 30, 60)},
    {"BELL KEYS", {1, 110, 0, 0, 74, 80, 100, 3}, {0, 92, 20, 84}, 20, 0, FX(0, 0, 35, 55)},
    {"FAT BASS", {2, 30, 100, 0, 34, 30, 30, 0}, {0, 60, 70, 20}, 36, 1, FX(0, 0, 0, 10)},
    {"SUB BASS", {3, 0, 127, 0, 30, 0, 20, 0}, {0, 60, 100, 20}, 0, 1, FX(0, 0, 0, 5)},
    {"ACID BASS", {0, 0, 40, 0, 28, 100, 40, 1}, {0, 56, 20, 26}, 50, 1, FX(0, 0, 20, 10)},
    {"PULSE BASS", {1, 60, 60, 0, 36, 40, 40, 1}, {0, 58, 40, 24}, 40, 1, FX(0, 0, 10, 10)},
    {"SEQ BASS", {2, 20, 80, 0, 26, 50, 50, 1}, {0, 50, 10, 20}, 52, 1, FX(0, 0, 25, 10)},
    {"LEAD", {0, 0, 40, 0, 60, 30, 64, 1}, {4, 70, 100, 50}, 20, 1, FX(0, 0, 35, 30)},
    {"SQUARE LEAD", {1, 0, 50, 0, 66, 20, 64, 1}, {2, 70, 100, 44}, 12, 1, FX(0, 0, 35, 30)},
    {"WHISTLE", {3, 0, 0, 8, 70, 127, 127, 2}, {20, 80, 110, 50}, 0, 1, FX(0, 0, 35, 45)},
    {"NOISE SWEEP", {3, 0, 0, 110, 20, 90, 0, 2}, {100, 120, 100, 100}, 60, 0, FX(0, 0, 40, 80)},
    {"WIND", {3, 0, 0, 120, 40, 70, 0, 1}, {90, 100, 110, 95}, -20, 0, FX(0, 0, 30, 75)},
};
/* each preset's POLY6 page and LFO: CHORUS, VOICES, the LFO's RATE and DELAY (fade), LFO DEST PIT / FLT / SHP, ENV DEST
 * SHP (engines.c poly6_preset_x) */
typedef struct { uint8_t cho, mode, rate, delay; int8_t lpit, lflt, lshp, eshp; } p6_extra_t;
static const p6_extra_t POLY6_X[NELEM(POLY6_PRESETS)] = {
    {P6C_I, P6M_8ENV, 30, 50, 0, 0, 20, 0},   {P6C_II, P6M_8ENV, 40, 40, 0, 8, 0, 0},
    {P6C_I, P6M_8ENV, 10, 0, 0, 30, 0, 0},    {P6C_I_II, P6M_8ENV, 30, 60, 0, 0, 10, 0},
    {P6C_II, P6M_8ENV, 50, 70, 3, 0, 0, 0},   {P6C_I, P6M_8ENV, 50, 60, 2, 0, 15, 0},
    {P6C_I_II, P6M_8ENV, 40, 80, 2, 0, 0, 0}, {P6C_I, P6M_4ENV, 60, 60, 2, 0, 0, 0},
    {P6C_II, P6M_4ENV, 50, 70, 2, 0, 0, 0},   {P6C_I, P6M_4ENV, 60, 0, 0, 0, 0, 0},
    {P6C_II, P6M_4ENV, 60, 0, 0, 0, 0, 0},    {P6C_I, P6M_4ENV, 60, 0, 0, 0, 0, 0},
    {P6C_II, P6M_4ENV, 60, 0, 0, 0, 15, 0},   {P6C_I_II, P6M_4GATE, 60, 0, 0, 0, 0, 0},
    {P6C_I, P6M_4ENV, 60, 0, 0, 0, 0, 0},     {P6C_I, P6M_8ENV, 60, 70, 2, 0, 0, 0},
    {P6C_II, P6M_8ENV, 60, 0, 0, 0, 0, 0},    {P6C_OFF, P6M_4ENV, 60, 0, 0, 0, 0, 0},
    {P6C_I, P6M_8ENV, 60, 0, 0, 0, 0, 0},     {P6C_II, P6M_8ENV, 50, 0, 0, 0, 0, 24},
    {P6C_I, P6M_8ENV, 60, 0, 0, 0, 0, 0},     {P6C_II, P6M_8ENV, 60, 0, 0, 0, 0, 0},
    {P6C_OFF, P6M_4ENV, 60, 0, 0, 0, 0, 0},   {P6C_OFF, P6M_4ENV, 60, 0, 0, 0, 0, 0},
    {P6C_OFF, P6M_4ENV, 60, 0, 0, 0, 0, 0},   {P6C_I, P6M_4ENV, 70, 0, 0, 0, 20, 0},
    {P6C_OFF, P6M_4ENV, 60, 0, 0, 0, 0, 0},   {P6C_I, P6M_4ENV, 64, 80, 4, 0, 0, 0},
    {P6C_II, P6M_4ENV, 64, 80, 3, 0, 0, 0},   {P6C_I, P6M_4ENV, 70, 60, 6, 0, 0, 0},
    {P6C_II, P6M_8ENV, 6, 0, 0, 40, 0, 0},    {P6C_I_II, P6M_8ENV, 8, 0, 0, 30, 0, 0},
};

static const engine_t ENG_POLY6 = {
    .name = "POLY6",
    .page_title = {"DCO", "VCF"},
    .edit = {
        {"WAVE", F_ENUM, 0, 3, 2, N_P6_WAVE, 0},
        {"PW", F_PCT, 0, 127, 30, 0, 0},
        {"SUB", F_PCT, 0, 127, 40, 0, 0},
        {"NOIS", F_PCT, 0, 127, 0, 0, 0},
        {"CUT", F_CUTOFF, 0, 127, 64, 0, 0},
        {"RES", F_PCT, 0, 127, 20, 0, 0},
        {"KYBD", F_PCT, 0, 127, 64, 0, 0},
        {"HPF", F_ENUM, 0, 3, 1, N_P6_HPF, 0},
    },
    .presets = POLY6_PRESETS,
    .npresets = NELEM(POLY6_PRESETS),
    .note_on = poly6_note_on,
    .render = poly6_render,
    .amp = poly6_amp,
    .knob = {P_E4, P_E5, P_ATK, P_REL},
    .keep = 0x0F,                /* the ladder */
    .mem = sizeof(p6_mem_t),
    .post = poly6_post,
};

/* the part's chorus is on: its mix takes the two sides (fx.c mix_part, mix_spread) */
static int poly6_wide(const track_t *t) { return t->engine == ENGI_POLY6 && t->p[P_P6CHO] > 0; }
/* a POLY6 preset pi loaded (ui.c apply_preset_to): its CHORUS, VOICES and LFO */
static void poly6_preset_x(track_t *t, uint32_t pi)
{
    const p6_extra_t *x;
    if (t->eng_req != ENGI_POLY6)
        return;
    x = &POLY6_X[pi % NELEM(POLY6_X)];
    t->p[P_P6CHO] = x->cho;
    t->p[P_P6MODE] = x->mode;
    t->p[P_LRATE] = x->rate;
    t->p[P_LFADE] = x->delay;
    t->p[P_LD_PIT] = x->lpit;
    t->p[P_LD_FLT] = x->lflt;
    t->p[P_LD_SHP] = x->lshp;
    t->p[P_ED_SHP] = x->eshp;
}
