/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 William Kew, ShinTech Electronics */
/* OPN2: the 16BIT mode's FM, an emulated YM2612 (chips/opn2.c, the ymfm port) driven through its registers as a sound
 * driver drives the chip; the synthesis is the chip's.
 *
 * One chip for the whole instrument: its six channels are shared by every OPN2 track (CHIP LIMIT: six notes at most;
 * a seventh takes the channel of the oldest released note, else of the oldest held one, from any OPN2 track). A voice
 * (voice.c) owns a channel while it sounds: note_on asks for a key on, and once a block, before the parts
 * (op2_mix_begin, fx.c mix_block), the engine
 *   - looks after the channels: a voice gone or stolen frees its channel (key off), a released gate keys off, a
 *     pitch that moved (glide, bend, LFO, the matrix: the voice's last vmod_t) writes the channel's F-number, macros
 *     that changed rewrite the patch's registers;
 *   - starts the waiting notes: a channel (CHIP LIMIT), the track's patch into its registers (only the bytes that
 *     differ), the F-number, the carriers' TL from the velocity, key on. A retrigger keys off first and on in the next
 *     block (the chip needs a sample with the key off to start an attack again);
 *   - renders the block's native samples (53,267 Hz: 38 or 39 per 32-sample block), channels 1..3 here and 4..6 on
 *     the second core when it answers (chips/opn2.c opn2_begin_n / opn2_channels_n), then each channel's Legacy mode
 *     (the DAC ladder, the output shaping, sample and hold) and the resampling to 44.1 kHz (linear; S&H: held).
 * A voice's render then adds its channel's block at the voice's amplitude ramp (vmod_t amp0 -> amp1): the chip's
 * envelopes are the sound (engine_t.ownenv), and LFO -> AMP, the matrix's AMP, the fades and the track's mix apply.
 * The voice ends when its channel is silent for good (done). Chip voices are not part of the shared budget of
 * NVOICE (voice.c voice_budgeted): the chip has its own six.
 *
 * Shared by its nature, as on the chip: the LFO (register 0x22: the last note keyed with LFO ON sets its rate) and
 * channel 3's special mode (0x27: a CH3 patch always plays on channel 3, one note at a time).
 *
 * The patch (op2_patch_t, OP2_PATCH_SIZE bytes, every byte 0..127: it travels in SysEx as it is) is the track's: the
 * web editor sends it, a factory slot loads it, projects and user presets keep it. The EDIT values are macros over it:
 *   ALG   PAT = the patch's algorithm, 1..8 another
 *   FB    added to its feedback
 *   MLVL  the modulators' level (TL down: brighter)
 *   MEG   the modulators' envelope rates (+ slower, - faster)
 *   VSEN  velocity -> the carriers' TL (0: none)
 *   LADR  Legacy: the DAC's ladder step (64 = the YM2612's, 0 none)
 *   LGCY  Legacy: the output shaping, CLEAN / EARLY (MD1-like low-pass) / LATE (milder), each also with S&H (the chip's
 *         native rate held, no interpolation)
 *   SLOT  F1..F32 the factory patches, OWN the track's own (as FM6's SLOT)
 * Six voices per track at most (engine_t.poly): the chip's channels. */

#define OPN2_POOL __attribute__((section(".pool")))
#define OPN2_FAST __attribute__((section(".fast_text")))
#include "chips/opn2.c"

#ifndef ENGI_OPN2
#define ENGI_OPN2 14u                 /* engines.c ENGINES[] (append-only) */
#endif
#if FELUCCA_CPU2
#ifndef C1_RUN                         /* (as fx.c: no second core on the host, in the browser) */
#define C1_ON() 0
#define C1_RUN(fn, arg) 0
#define C1_WAIT() 0
#endif
#endif

/* ------------------------------------------------------------ the patch --- */
typedef struct {                       /* one operator: the registers' fields, and the patch's own */
    uint8_t ar, dr, sr, rr, sl, tl, ks, mul, dt, am, ssg;   /* AR 0..31 .. DT 0..7 (the register's), AM 0/1, SSG 0..15 */
    uint8_t rcoarse, rfine;            /* RATIO mode: the ratio rcoarse.rfine (0..31, 0..99); 0.0 = the MUL register */
    uint8_t foff;                      /* CH3 mode: this operator's offset in semitones, 64 = none */
    uint8_t rsv[2];
} op2_op_t;
typedef struct {
    uint8_t alg, fb, lfo, lfo_rate, ams, fms, lr, ch3;   /* 0..7, 0..7, 0/1, 0..7, 0..3, 0..7, bit 0 L bit 1 R, 0/1 */
    uint8_t ratio, transpose;          /* RATIO mode 0/1; transpose in semitones, 64 = none */
    uint8_t rsv[6];
    op2_op_t op[4];                    /* OP1 OP2 OP3 OP4 (the musical order; the registers hold OP1 OP3 OP2 OP4) */
    char name[16];                     /* printable ASCII, space padded */
} op2_patch_t;
#define OP2_PATCH_SIZE 96u
_Static_assert(sizeof(op2_patch_t) == OP2_PATCH_SIZE, "op2_patch_t layout");

static const uint8_t OP2_SLOT[4] = {0, 8, 4, 12};   /* OP1..OP4 -> the register offset (0x30 + slot + channel) */
/* the carriers of each algorithm, bit k = OP k+1 */
static const uint8_t OP2_CARRIERS[8] = {0x8, 0x8, 0x8, 0x8, 0xA, 0xE, 0xE, 0xF};

/* the init patch: two 2-operator stacks (algorithm 4), a plain bright tone */
#define OP2_INIT_P                                                                         \
    {4, 3, 0, 0, 0, 0, 3, 0, 0, 64, {0},                                                   \
     {{31, 8, 0, 6, 3, 34, 0, 1, 0, 0, 0, 0, 0, 64, {0}}, {31, 6, 2, 7, 2, 0, 0, 1, 0, 0, 0, 0, 0, 64, {0}}, \
      {31, 10, 0, 6, 4, 40, 0, 2, 0, 0, 0, 0, 0, 64, {0}}, {31, 6, 2, 7, 2, 4, 0, 1, 0, 0, 0, 0, 0, 64, {0}}}, \
     "INIT            "}
static const op2_patch_t OP2_INIT = OP2_INIT_P;

/* every value inside its range (bytes the editor or a store may have got wrong) */
static void op2_sanitize(op2_patch_t *p)
{
    static const uint8_t OPMAX[16] = {31, 31, 31, 15, 15, 127, 3, 15, 7, 1, 15, 31, 99, 127, 0, 0};
    static const uint8_t PMAX[16] = {7, 7, 1, 7, 3, 7, 3, 1, 1, 127, 0, 0, 0, 0, 0, 0};
    uint8_t *b = (uint8_t *)p;
    uint32_t i;
    for (i = 0; i < 16u; i++)
        if (b[i] > PMAX[i])
            b[i] = PMAX[i];
    for (i = 16u; i < 80u; i++)
        if (b[i] > OPMAX[(i - 16u) % 16u])
            b[i] = OPMAX[(i - 16u) % 16u];
    for (i = 80u; i < OP2_PATCH_SIZE; i++)
        if (b[i] < 32u || b[i] > 126u)
            b[i] = ' ';
}

/* ------------------------------------------------------------- the chip --- */
static opn2_t op2_chip;                         /* (.bss: its loads are quicker than the pool's) */
static uint8_t op2_ready;                       /* the chip was reset */
_Static_assert(NTRK == 4, "op2_patch / op2_live: one init patch per track");
/* the tracks' patches, the init patch until one is set (also where nothing calls op2_init: the host tests, the
 * browser emulator): the main loop's (op2_set_patch) */
static op2_patch_t op2_patch[NTRK] = {OP2_INIT_P, OP2_INIT_P, OP2_INIT_P, OP2_INIT_P};
static volatile uint8_t op2_pgen[NTRK];         /* its writes: odd while one is under way, +2 for each */
static op2_patch_t op2_live[NTRK] = {OP2_INIT_P, OP2_INIT_P, OP2_INIT_P, OP2_INIT_P};   /* the audio ISR's copies,
                                                * taken while op2_pgen is even (op2_mix_begin) */
static uint8_t op2_lgen[NTRK];                  /* the op2_pgen each copy was taken at */

typedef struct {                                /* a chip channel */
    uint8_t busy;                               /* owned by a voice */
    uint8_t part, voice;                        /* its owner: trk[part].v[voice] */
    uint8_t keyed;                              /* key on written (and not off since) */
    uint8_t restart;                            /* keyed off for a retrigger: key on next block */
    uint8_t pgen, ptrack;                       /* the patch in its registers: track and generation (0xFF: none) */
    uint8_t mac_alg, mac_fb, vel;               /* the macro values and velocity its registers hold */
    int8_t mac_lvl, mac_eg, mac_vs;
    uint32_t age;                               /* the start of its note (stealing: the oldest) */
    int32_t pitch;                              /* the pitch its F-number holds (1/16 semitone, with fine: 1/65536) */
    uint8_t shadow[32];                         /* its registers as written: 28 operator bytes, B0, B4 */
    uint8_t shadow_ok;
    /* Legacy mode and the resampler */
    int32_t prev, dc, lp1, lp2;                 /* the last native sample, the DC blocker (Q10), the shaping's poles */
} op2_chan_t;
static op2_chan_t op2_ch[OPN2_CHANNELS];
static uint32_t op2_age;
static uint8_t op2_lfo_reg = 0xFF, op2_mode_reg = 0xFF;   /* 0x22, 0x27 as written */
static uint32_t op2_rs_ph;                      /* the resampler's position, Q16, from the last native sample */
static int32_t op2_out44[OPN2_CHANNELS][CTL] __attribute__((section(".pool")));   /* each channel, this block */
static int32_t op2_nat[48][OPN2_CHANNELS][2] __attribute__((section(".pool")));   /* the block's native samples */
static opn2_blk_t op2_blk[4] __attribute__((section(".pool")));
static uint32_t op2_nb;
#define OP2_STEP_Q16 79128u                     /* native samples per output sample, Q16: (7670453 / 144) / (24e6 / 544) */

static void op2_reg(uint32_t port, uint32_t reg, uint32_t data)
{
#ifdef OP2_REG_LOG
    OP2_REG_LOG(port, reg, data);               /* (tests/opn2_engine_test.c: the register writes, in order) */
#endif
    opn2_write(&op2_chip, port, (uint8_t)reg);
    opn2_write(&op2_chip, port + 1u, (uint8_t)data);
}

/* channel ch's register at index k of its shadow (0..27 operator bytes: field k / 4, operator k % 4; 28 B0, 29 B4),
 * written only when it differs */
static void op2_creg(uint32_t ch, uint32_t k, uint32_t data)
{
    op2_chan_t *c = &op2_ch[ch];
    uint32_t port = ch < 3u ? 0u : 2u, base = ch % 3u, reg;
    if (c->shadow_ok && c->shadow[k] == (uint8_t)data)
        return;
    c->shadow[k] = (uint8_t)data;
    reg = k < 28u ? 0x30u + (k / 4u) * 0x10u + OP2_SLOT[k % 4u] + base : (k == 28u ? 0xB0u : 0xB4u) + base;
    op2_reg(port, reg, data);
}

/* -------------------------------------------------------------- pitch --- */
/* F-number x 2 of one octave from C at block 3 (C4 = MIDI 60 = 1288), in 1/16 semitones: 440 x 2^((n - 69) / 12) x 144 x
 * 2^21 / 7670453 / 2^3. Worst error 1.2 cents (tests/opn2_engine_test.c checks every note) */
static const uint16_t OP2_FNUM2[192] = {
    1288, 1292, 1297, 1302, 1306, 1311, 1316, 1320, 1325, 1330, 1335, 1340, 1345, 1349, 1354, 1359,
    1364, 1369, 1374, 1379, 1384, 1389, 1394, 1399, 1404, 1409, 1414, 1419, 1424, 1430, 1435, 1440,
    1445, 1450, 1456, 1461, 1466, 1472, 1477, 1482, 1488, 1493, 1498, 1504, 1509, 1515, 1520, 1526,
    1531, 1537, 1542, 1548, 1553, 1559, 1565, 1570, 1576, 1582, 1587, 1593, 1599, 1605, 1611, 1616,
    1622, 1628, 1634, 1640, 1646, 1652, 1658, 1664, 1670, 1676, 1682, 1688, 1694, 1700, 1706, 1712,
    1719, 1725, 1731, 1737, 1744, 1750, 1756, 1763, 1769, 1775, 1782, 1788, 1795, 1801, 1808, 1814,
    1821, 1827, 1834, 1841, 1847, 1854, 1861, 1867, 1874, 1881, 1888, 1895, 1901, 1908, 1915, 1922,
    1929, 1936, 1943, 1950, 1957, 1964, 1971, 1979, 1986, 1993, 2000, 2007, 2015, 2022, 2029, 2036,
    2044, 2051, 2059, 2066, 2074, 2081, 2089, 2096, 2104, 2111, 2119, 2127, 2134, 2142, 2150, 2158,
    2165, 2173, 2181, 2189, 2197, 2205, 2213, 2221, 2229, 2237, 2245, 2253, 2261, 2269, 2278, 2286,
    2294, 2302, 2311, 2319, 2328, 2336, 2344, 2353, 2361, 2370, 2378, 2387, 2396, 2404, 2413, 2422,
    2431, 2439, 2448, 2457, 2466, 2475, 2484, 2493, 2502, 2511, 2520, 2529, 2538, 2547, 2557, 2566};

/* the block << 11 | F-number of pitch p (1/16 semitone, 0 = MIDI note 0) times (1 + fine / 4096) */
static uint32_t op2_fnum(int32_t p, int32_t fine)
{
    int32_t d = p - 60 * 16, oct = d >= 0 ? d / 192 : -((-d + 191) / 192), blk = 3 + oct;
    uint32_t f = OP2_FNUM2[d - oct * 192];
    if (fine)
        f = (uint32_t)((int32_t)f + (int32_t)(((int64_t)(int32_t)f * fine) >> 12));
    if (f >= 2048u) {                           /* over 11 bits at this block: the next one, half the number */
        f = (f + 1u) >> 1;
        blk++;
    }
    while (blk < 0) {                           /* below block 0: fewer bits */
        f >>= 1;
        blk++;
    }
    if (blk > 7) {                              /* above the chip's range: its top */
        blk = 7;
        f = 2047u;
    }
    if (f > 2047u)
        f = 2047u;
    return (uint32_t)blk << 11 | f;
}

static void op2_write_freq(uint32_t ch, uint32_t bf)
{
    uint32_t port = ch < 3u ? 0u : 2u, c = ch % 3u;
    op2_reg(port, 0xA4u + c, bf >> 8);          /* (the upper half latches, the lower applies both) */
    op2_reg(port, 0xA0u + c, bf & 0xFFu);
}

/* channel 3's special mode: OP1, OP2, OP3 each their own frequency (A9 / AA / A8 and AD / AE / AC); OP4 the channel's */
static void op2_write_ch3(const op2_patch_t *p, int32_t pitch, int32_t fine)
{
    static const uint8_t LO[3] = {0xA9, 0xAA, 0xA8};
    uint32_t k;
    for (k = 0; k < 3u; k++) {
        uint32_t bf = op2_fnum(pitch + ((int32_t)p->op[k].foff - 64) * 16, fine);
        op2_reg(0, LO[k] + 4u, bf >> 8);
        op2_reg(0, LO[k], bf & 0xFFu);
    }
}

/* RATIO mode (with channel 3's special mode: only then has an operator its own frequency): the ratio rcoarse.rfine as
 * a pitch offset from the note, 16 x 12 x log2(ratio) (1/16 semitones; MUL is then 1). 0.00 = none (the MUL register) */
static int32_t op2_ratio_offset(const op2_op_t *o)
{
    static const uint8_t LOG[65] = {     /* 192 x log2(1 + i / 64) */
        0, 4, 9, 13, 17, 21, 25, 29, 33, 36, 40, 44, 48, 51, 55, 58, 62, 65, 69, 72, 75, 79, 82, 85, 88, 91, 94, 97,
        101, 104, 106, 109, 112, 115, 118, 121, 124, 126, 129, 132, 134, 137, 140, 142, 145, 147, 150, 153, 155, 157,
        160, 162, 165, 167, 169, 172, 174, 176, 179, 181, 183, 185, 188, 190, 192};
    uint32_t r = (uint32_t)o->rcoarse * 100u + o->rfine, f, i;   /* the ratio x 100 */
    int32_t off = 0;
    if (!r)
        return 0;
    while (r >= 200u) {
        r = (r + 1u) / 2u;
        off += 192;
    }
    while (r < 100u) {
        r *= 2u;
        off -= 192;
    }
    f = (r - 100u) * 64u;                       /* (0 .. 6336: an index into LOG and its fraction / 100) */
    i = f / 100u;
    return off + LOG[i] + (int32_t)((LOG[i < 64u ? i + 1u : 64u] - LOG[i]) * (f % 100u) / 100u);
}

/* ------------------------------------------------- the patch's registers --- */
/* the carriers' TL from the velocity: (127 - vel) x depth / 256 steps of 0.75 dB (depth 127: up to ~47 dB) */
static uint32_t op2_vel_att(uint32_t vel, int32_t depth)
{
    return ((127u - (vel > 127u ? 127u : vel)) * (uint32_t)clamp(depth, 0, 127)) >> 8;
}

/* a track's patch through its macros (e: P_E0.., vel the note's) into channel ch's registers (only what differs) */
static void op2_patch_regs(uint32_t ch, const op2_patch_t *p, const int16_t *e, uint32_t vel)
{
    uint32_t alg = e[0] >= 1 && e[0] <= 8 ? (uint32_t)e[0] - 1u : p->alg, car = OP2_CARRIERS[alg & 7u], k;
    uint32_t fb = (uint32_t)clamp(p->fb + e[1], 0, 7), lr = p->lr & 3u;
    int32_t lvl = e[2] / 2, eg = e[3] / 4;
    for (k = 0; k < 4u; k++) {
        const op2_op_t *o = &p->op[k];
        int32_t tl = o->tl, ar = o->ar, dr = o->dr, sr = o->sr, rr = o->rr;
        uint32_t mul = o->mul;
        if ((car >> k) & 1u) {
            tl += (int32_t)op2_vel_att(vel, e[4]);
        } else {                                /* a modulator: MLVL (+ brighter), MEG (+ slower) */
            tl -= lvl;
            ar -= eg;
            dr -= eg;
            sr -= eg;
            rr -= eg / 2;
        }
        if (p->ch3 && p->ratio && k < 3u && (o->rcoarse | o->rfine))
            mul = 1;                            /* (its ratio is in its own F-number: op2_pitch) */
        op2_creg(ch, 0u * 4u + k, (uint32_t)(o->dt & 7u) << 4 | (mul & 15u));
        op2_creg(ch, 1u * 4u + k, (uint32_t)clamp(tl, 0, 127));
        op2_creg(ch, 2u * 4u + k, (uint32_t)(o->ks & 3u) << 6 | (uint32_t)clamp(ar, 0, 31));
        op2_creg(ch, 3u * 4u + k, (uint32_t)(o->am & 1u) << 7 | (uint32_t)clamp(dr, 0, 31));
        op2_creg(ch, 4u * 4u + k, (uint32_t)clamp(sr, 0, 31));
        op2_creg(ch, 5u * 4u + k, (uint32_t)(o->sl & 15u) << 4 | (uint32_t)clamp(rr, 0, 15));
        op2_creg(ch, 6u * 4u + k, o->ssg & 15u);
    }
    op2_creg(ch, 28, fb << 3 | (alg & 7u));
    op2_creg(ch, 29, (lr & 1u) << 7 | (lr & 2u) << 5 | (p->lfo ? (uint32_t)(p->ams & 3u) << 4 | (p->fms & 7u) : 0u));
    op2_ch[ch].shadow_ok = 1;
}

static void op2_key(uint32_t ch, int on)
{
    op2_reg(0, 0x28, (on ? 0xF0u : 0u) | (ch < 3u ? ch : ch + 1u));
}

/* the note's pitch on channel ch (1/16 semitone, fine 1/4096), through the patch's transpose; with CH3 mode on
 * channel 3, OP1..OP3 their own (the note + their offset, + their ratio in RATIO mode) */
static void op2_pitch(uint32_t ch, const op2_patch_t *p, int32_t pitch, int32_t fine)
{
    static const uint8_t LO[3] = {0xA9, 0xAA, 0xA8};   /* OP1, OP2, OP3's low bytes (+ 4: the high ones) */
    op2_chan_t *c = &op2_ch[ch];
    int32_t pt = pitch + ((int32_t)p->transpose - 64) * 16;
    uint32_t k;
    if (c->pitch == pt * 4096 + fine)
        return;
    c->pitch = pt * 4096 + fine;
    op2_write_freq(ch, op2_fnum(pt, fine));
    if (!p->ch3 || ch != 2u)
        return;
    for (k = 0; k < 3u; k++) {
        int32_t off = ((int32_t)p->op[k].foff - 64) * 16 + (p->ratio ? op2_ratio_offset(&p->op[k]) : 0);
        uint32_t bf = op2_fnum(pt + off, fine);
        op2_reg(0, LO[k] + 4u, bf >> 8);
        op2_reg(0, LO[k], bf & 0xFFu);
    }
}

/* the chip's shared registers for a note of patch p: the LFO (the last note keyed with LFO ON sets its rate) and
 * channel 3's special mode (while a CH3 patch has channel 3) */
static void op2_globals(const op2_patch_t *p)
{
    uint32_t mode = op2_ch[2].busy && op2_ch[2].ptrack < NTRK && op2_live[op2_ch[2].ptrack].ch3 ? 0x40u : 0u;
    if (p->lfo && op2_lfo_reg != (0x08u | (p->lfo_rate & 7u))) {
        op2_lfo_reg = (uint8_t)(0x08u | (p->lfo_rate & 7u));
        op2_reg(0, 0x22, op2_lfo_reg);
    }
    if (op2_mode_reg != mode) {
        op2_mode_reg = (uint8_t)mode;
        op2_reg(0, 0x27, mode);
    }
}

/* ------------------------------------------------------- the channels --- */
static void voice_kill(voice_t *v);             /* (voice.c) */

/* the channels notes may take: all six; on a device without its second core answering (MENU > SYSTEM > 2ND CORE
 * OFF, or it gave up) OP2_ONE_CORE_CAP (felucca.c): one core cannot render six channels and the rest in time */
static uint32_t op2_nchan(void)
{
#ifdef OP2_ONE_CORE_CAP
#if FELUCCA_CPU2
    if (C1_ON())
        return OPN2_CHANNELS;
#endif
    return OP2_ONE_CORE_CAP;
#else
    return OPN2_CHANNELS;
#endif
}

/* channel ch's note ends now (CHIP LIMIT, the cap): its voice fades over a block, silent; the channel is free */
static void op2_steal(uint32_t ch)
{
    op2_chan_t *c = &op2_ch[ch];
    voice_t *ov = &trk[c->part % NPART].v[c->voice % NVOICE];
    if (ov->s[0] == (int32_t)ch + 1) {
        ov->s[0] = 0;
        voice_kill(ov);
    }
    if (c->keyed)
        op2_key(ch, 0);
    c->busy = c->keyed = c->restart = 0;
}

/* a channel for a new note of track tr's patch p: CH3 patches channel 3; else a free one (one that holds the track's
 * patch first: fewer writes), else CHIP LIMIT takes the oldest released note's, else the oldest held one's */
static uint32_t op2_alloc(uint32_t tr, const op2_patch_t *p)
{
    uint32_t ch, best = OPN2_CHANNELS, lo = p->ch3 ? 2u : 0u, hi = p->ch3 ? 3u : op2_nchan();
    for (ch = lo; ch < hi; ch++)
        if (!op2_ch[ch].busy && (best == OPN2_CHANNELS || (op2_ch[ch].ptrack == tr && op2_ch[ch].pgen == op2_lgen[tr])))
            best = ch;
    if (best < OPN2_CHANNELS)
        return best;
    for (ch = lo; ch < hi; ch++)                /* released, oldest */
        if (!op2_ch[ch].keyed && !op2_ch[ch].restart && (best == OPN2_CHANNELS || op2_ch[ch].age < op2_ch[best].age))
            best = ch;
    if (best == OPN2_CHANNELS)
        for (ch = lo; ch < hi; ch++)            /* held, oldest */
            if (best == OPN2_CHANNELS || op2_ch[ch].age < op2_ch[best].age)
                best = ch;
    op2_steal(best);                            /* CHIP LIMIT: its note ends */
    return best;
}

static void op2_macros_take(op2_chan_t *c, const track_t *t)
{
    c->mac_alg = (uint8_t)t->p[P_E0];
    c->mac_fb = (uint8_t)t->p[P_E1];
    c->mac_lvl = (int8_t)t->p[P_E2];
    c->mac_eg = (int8_t)t->p[P_E3];
    c->mac_vs = (int8_t)t->p[P_E4];
}
static int op2_macros_moved(const op2_chan_t *c, const track_t *t)
{
    return c->mac_alg != (uint8_t)t->p[P_E0] || c->mac_fb != (uint8_t)t->p[P_E1] || c->mac_lvl != (int8_t)t->p[P_E2] ||
           c->mac_eg != (int8_t)t->p[P_E3] || c->mac_vs != (int8_t)t->p[P_E4];
}

/* voice v of track t starts on the chip (its key on waited for this block) */
static void op2_start(track_t *t, voice_t *v)
{
    uint32_t tr = (uint32_t)(t - trk), vi = (uint32_t)(v - t->v), ch;
    const op2_patch_t *p = &op2_live[tr];
    op2_chan_t *c;
    int32_t own = v->s[0] - 1;
    if (own >= 0 && own < (int32_t)OPN2_CHANNELS && op2_ch[own].busy && op2_ch[own].part == tr && op2_ch[own].voice == vi
        && (!p->ch3 || own == 2))
        ch = (uint32_t)own;                     /* a retrigger: its channel */
    else
        ch = op2_alloc(tr, p);
    c = &op2_ch[ch];
    if (!c->busy) {                             /* a fresh start: the Legacy and resampler states from 0 */
        c->prev = c->dc = c->lp1 = c->lp2 = 0;
        c->keyed = c->restart = 0;
    }
    c->busy = 1;
    c->part = (uint8_t)tr;
    c->voice = (uint8_t)vi;
    c->age = ++op2_age;
    c->ptrack = (uint8_t)tr;
    c->pgen = op2_lgen[tr];
    c->vel = (uint8_t)(v->mvel ? v->mvel : v->vel);
    op2_macros_take(c, t);
    op2_patch_regs(ch, p, &t->p[P_E0], c->vel);
    c->pitch = 0x7FFFFFFF;
    op2_pitch(ch, p, v->s[2], v->s[3]);
    op2_globals(p);
    if (c->keyed) {                             /* retrigger: off now, on next block */
        op2_key(ch, 0);
        c->keyed = 0;
        c->restart = 1;
    } else {
        op2_key(ch, 1);
        c->keyed = 1;
    }
    v->s[0] = (int32_t)ch + 1;
    v->s[1] = 0;
}

/* channel upkeep: its owner gone (freed), a retrigger's key on, a released gate (key off), the patch or its macros
 * moved (its registers again), the pitch */
static void op2_upkeep(void)
{
    uint32_t ch, nch = op2_nchan();
    for (ch = 0; ch < OPN2_CHANNELS; ch++) {
        op2_chan_t *c = &op2_ch[ch];
        track_t *t = &trk[c->part % NPART];
        voice_t *v = &t->v[c->voice % NVOICE];
        const op2_patch_t *p;
        if (!c->busy)
            continue;
        if (ch >= nch) {                        /* past the cap (the second core gave up): it ends */
            op2_steal(ch);
            continue;
        }
        if (!v->active || v->stage == 4u || t->engine != ENGI_OPN2 || v->s[0] != (int32_t)ch + 1) {
            if (c->keyed)
                op2_key(ch, 0);
            c->busy = c->keyed = c->restart = 0;
            continue;
        }
        p = &op2_live[c->part % NTRK];
        if (c->restart) {
            op2_key(ch, 1);
            c->keyed = 1;
            c->restart = 0;
        } else if (!v->gate && c->keyed) {
            op2_key(ch, 0);
            c->keyed = 0;
        }
        if (c->pgen != op2_lgen[c->part % NTRK] || op2_macros_moved(c, t)) {
            c->pgen = op2_lgen[c->part % NTRK];
            op2_macros_take(c, t);
            op2_patch_regs(ch, p, &t->p[P_E0], c->vel);
            c->pitch = 0x7FFFFFFF;              /* (transpose, CH3 offsets) */
        }
        op2_pitch(ch, p, v->s[2], v->s[3]);
    }
}

/* ------------------------------------------------- the block, before the parts --- */
static uint8_t op2_c1_failed;                   /* the second core stopped answering during a chip job (console) */
static uint32_t op2_c1_blocks;                  /* blocks the second core rendered channels 4..6 of (console) */
static uint32_t op2_idle_blocks;                /* blocks the chip was silent and not clocked (console) */
#if FELUCCA_CPU2
static void psg_c1_hook(void);                  /* (eng_psg.c: PSG's block, on the second core with channels 4..6) */
static void op2_c1_job(uint32_t off)
{
    OPN2_FAR(opn2_channels_n)(&op2_chip, op2_blk, op2_nb, op2_nat + off, 3, 6);
    psg_c1_hook();                              /* (once: it clears its own flag) */
}
#endif

/* the chip's nn native samples into op2_nat: channels 1..3 here, 4..6 on the second core when it answers */
static void op2_render_chip(uint32_t nn)
{
    uint32_t done = 0, nb, m, nout = op2_chip.dac_en ? 5u : 6u;
    while (done < nn) {
        m = OPN2_FAR(opn2_begin_n)(&op2_chip, op2_blk, 4, nn - done, nout, &nb);
        op2_nb = nb;
#if FELUCCA_CPU2
        if (C1_ON() && C1_RUN(op2_c1_job, done)) {
            OPN2_FAR(opn2_channels_n)(&op2_chip, op2_blk, nb, op2_nat + done, 0, 3);
            op2_c1_blocks++;
            if (C1_WAIT()) {                    /* it stopped (held from now on): here, one glitch */
                op2_c1_failed = 1;
                OPN2_FAR(opn2_channels_n)(&op2_chip, op2_blk, nb, op2_nat + done, 3, 6);
            }
        } else
#endif
            OPN2_FAR(opn2_channels_n)(&op2_chip, op2_blk, nb, op2_nat + done, 0, 6);
        done += m;
    }
}

/* one channel's native samples -> its block at 44.1 kHz in op2_out44: the ladder (at the native rate: a
 * non-linearity), the resampling (linear, or held with S&H), the shaping, a DC blocker, at the voice level */
__attribute__((noinline)) static void op2_post(uint32_t ch, uint32_t nn, uint32_t ph0)   /* (out of line: tests/target_budget.py) */
{
    op2_chan_t *c = &op2_ch[ch];
    const track_t *t = &trk[c->part % NPART];
    int32_t x[49], d = clamp(t->p[P_E5], 0, 127) * 56 / 64, lg = clamp(t->p[P_E6], 0, 5);   /* (x16: 3.5 x 16) */
    int32_t model = lg % 3, hold = lg >= 3, dc = c->dc, l1 = c->lp1, l2 = c->lp2;
    uint32_t s, j, ph = ph0;
    x[0] = c->prev;
    for (s = 0; s < nn; s++) {
        int32_t l = op2_nat[s][ch][0], r = op2_nat[s][ch][1], v = l ? l : r;   /* (a channel heard on L, R or both) */
        x[s + 1u] = v * 16 + (v ? (v > 0 ? d : -d) : 0);   /* (silence stays 0: no step at rest) */
    }
    c->prev = x[nn];
    for (j = 0; j < CTL; j++, ph += OP2_STEP_Q16) {
        uint32_t i = ph >> 16, f = ph & 0xFFFFu;   /* between x[i] and x[i + 1] (x[0]: the last block's last) */
        int32_t y = hold ? x[i] : x[i] + (int32_t)(((int64_t)(x[i + 1u] - x[i]) * (int32_t)f) >> 16);
        if (model == 1) {                       /* EARLY: two poles near 3.4 kHz */
            l1 += ((y - l1) * 12583) >> 15;
            l2 += ((l1 - l2) * 12583) >> 15;
            y = l2;
        } else if (model == 2) {                /* LATE: one pole near 9 kHz */
            l1 += ((y - l1) * 23660) >> 15;
            y = l1;
        }
        y -= dc >> 10;                          /* DC blocker, ~7 Hz (its state Q10: no rounding drift) */
        dc += y;
        op2_out44[ch][j] = (y * VOICE_FS) >> 12;   /* (one channel at full, 256 x 16: VOICE_FS) */
    }
    c->dc = dc;
    c->lp1 = l1;
    c->lp2 = l2;
}

/* once a block, after the events (the notes) and before the parts (fx.c mix_block) */
__attribute__((noinline)) static void op2_mix_begin(void)   /* (out of line: its own target budget) */
{
    uint32_t p, ch, any = 0, nn;
    for (p = 0; p < NPART; p++)
        any |= trk[p].engine == ENGI_OPN2;
    for (ch = 0; ch < OPN2_CHANNELS; ch++)
        any |= op2_ch[ch].busy;
    if (!any)
        return;                                 /* (no OPN2 track: no chip) */
    if (!op2_ready) {
        opn2_reset(&op2_chip);
        for (ch = 0; ch < OPN2_CHANNELS; ch++)
            op2_ch[ch].ptrack = 0xFF;
        op2_lfo_reg = op2_mode_reg = 0xFF;
        op2_ready = 1;
    }
    for (p = 0; p < NTRK; p++) {                /* the patches the main loop wrote since: the ISR's copies */
        uint8_t g = op2_pgen[p];
        if (!(g & 1u) && g != op2_lgen[p]) {    /* (odd: a write under way, the last copy stays) */
            op2_live[p] = op2_patch[p];
            op2_lgen[p] = g;
        }
    }
    op2_upkeep();
    for (p = 0; p < NPART; p++) {               /* the notes that wait for a channel */
        track_t *t = &trk[p];
        uint32_t i;
        if (t->engine != ENGI_OPN2)
            continue;
        for (i = 0; i < NVOICE; i++)
            if (t->v[i].active && t->v[i].s[1])
                op2_start(t, &t->v[i]);
    }
    for (ch = 0, any = 0; ch < OPN2_CHANNELS; ch++)
        any |= op2_ch[ch].busy;
    if (!any && opn2_silent(&op2_chip)) {       /* no note, every operator released to silence: no chip this block */
        op2_idle_blocks++;
        return;
    }
    nn =((op2_rs_ph + (CTL - 1u) * OP2_STEP_Q16) >> 16) + 1u;   /* the native samples this block needs (38 or 39) */
    op2_render_chip(nn);
    for (ch = 0; ch < OPN2_CHANNELS; ch++)
        if (op2_ch[ch].busy)
            op2_post(ch, nn, op2_rs_ph);
    op2_rs_ph = op2_rs_ph + CTL * OP2_STEP_Q16 - (nn << 16);
}

/* ------------------------------------------------------------ the voice --- */
static void op2_note_on(track_t *t, voice_t *v)
{
    v->s[1] = 1;                                /* a key on, at the next block start (op2_mix_begin) */
    v->s[2] = v->pitch_cur;                     /* (the render keeps it up to date from the voice's vmod_t) */
    v->s[3] = 0;
    (void)t;
}

static void op2_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    int32_t ch = v->s[0] - 1, a = m->amp0, da;
    const int32_t *x;
    uint32_t i;
    (void)t;
    v->s[2] = m->pitch16;                       /* the pitch for the next block's F-number */
    v->s[3] = m->fine;
    if (ch < 0 || ch >= (int32_t)OPN2_CHANNELS || !op2_ch[ch].busy || n != CTL)
        return;
    x = op2_out44[ch];
    da = (m->amp1 - m->amp0) >> CTL_LOG2;
    for (i = 0; i < CTL; i++) {
        a += da;
        out[i] += (int32_t)(((int64_t)x[i] * a) >> 15);
    }
}

/* the voice ends once its channel is silent for good: released, and every carrier of its algorithm below the chip's
 * quiet level (a modulator still releasing under a silent carrier is not heard) */
static int op2_done(track_t *t, voice_t *v)
{
    int32_t ch = v->s[0] - 1;
    uint32_t k;
    (void)t;
    if (v->s[1])
        return 0;                               /* (its key on is still to come) */
    if (ch < 0 || ch >= (int32_t)OPN2_CHANNELS || !op2_ch[ch].busy)
        return 1;
    if (v->gate || op2_ch[ch].keyed || op2_ch[ch].restart)
        return 0;
    for (k = 0; k < 4u; k++) {
        const opn2_op_t *o = OPN2_OP(&op2_chip, (uint32_t)ch, k);
        if (((OP2_CARRIERS[op2_ch[ch].shadow[28] & 7u] >> k) & 1u) &&
            (o->env_state != OPN2_EG_RELEASE || o->env_att < OPN2_EG_QUIET))
            return 0;
    }
    return 1;
}

/* -------------------------------------------------------- the patches --- */
#include "opn2_factory.h"                       /* OP2_FACTORY[]: the factory patches (F1..), original sounds */
#define OP2_NFACTORY ((uint32_t)NELEM(OP2_FACTORY))
#define OP2_OWN OP2_NFACTORY                    /* SLOT: the track's own patch */
static uint8_t op2_slot[NTRK];                  /* the SLOT value last loaded (main loop); 0xFF = none */
static op2_patch_t op2_own[NTRK] __attribute__((section(".pool")));   /* the own patch while SLOT shows a factory one */
static uint8_t op2_own_ok;                      /* bit tr: op2_own[tr] holds it */

/* track tr's patch = p (sanitized); main loop. op2_pgen odd while it writes: the audio ISR (which this loop never
 * preempts) takes its copy only from an even one (op2_mix_begin), so it never sees half a patch */
static void op2_set_patch(uint32_t tr, const op2_patch_t *p)
{
    op2_patch_t s = *p;
    op2_sanitize(&s);
    tr %= NTRK;
    op2_pgen[tr] = (uint8_t)(op2_pgen[tr] | 1u);
    RING_PUBLISH();
    op2_patch[tr] = s;
    RING_PUBLISH();
    op2_pgen[tr] = (uint8_t)(op2_pgen[tr] + 1u);
}

static void op2_init(void)                      /* power-on: every track the init patch */
{
    uint32_t tr;
    for (tr = 0; tr < NTRK; tr++) {
        op2_set_patch(tr, &OP2_INIT);
        op2_slot[tr] = 0xFF;
    }
}

/* track tr's patch was just put in as its own (a project, a user preset, the editor): an OPN2 track's SLOT shows the
 * factory patch it equals (F n), else OWN */
static void op2_adopt(uint32_t tr)
{
    track_t *t = &trk[tr % NTRK];
    uint32_t k;
    tr %= NTRK;
    op2_own_ok &= (uint8_t)~(1u << tr);
    if (t->eng_req != ENGI_OPN2)
        return;
    for (k = 0; k < OP2_NFACTORY; k++) {
        op2_patch_t f = OP2_FACTORY[k];
        op2_sanitize(&f);
        if (!memcmp(&f, &op2_patch[tr], OP2_PATCH_SIZE))
            break;
    }
    t->p[P_E7] = (int16_t)(k < OP2_NFACTORY ? k : OP2_OWN);
    op2_slot[tr] = (uint8_t)t->p[P_E7];
}

/* main loop: SLOT turned (a knob, the editor, MIDI, motion, a factory preset) -> that factory patch; from OWN the own
 * patch is kept aside, and turning back to OWN brings it back (as FM6's fm6_poll) */
static void op2_poll(void)
{
    uint32_t tr;
    for (tr = 0; tr < NTRK; tr++) {
        int32_t s = trk[tr].p[P_E7];
        uint32_t bit = 1u << tr;
        if (trk[tr].eng_req != ENGI_OPN2 || s == op2_slot[tr])
            continue;
        if (s < 0 || (uint32_t)s >= OP2_NFACTORY) {   /* OWN */
            if (op2_own_ok & bit)
                op2_set_patch(tr, &op2_own[tr]);
            op2_own_ok &= (uint8_t)~bit;
            op2_slot[tr] = (uint8_t)OP2_OWN;
            trk[tr].p[P_E7] = (int16_t)OP2_OWN;
            continue;
        }
        if (op2_slot[tr] == OP2_OWN && !(op2_own_ok & bit)) {
            op2_own[tr] = op2_patch[tr];
            op2_own_ok |= (uint8_t)bit;
        }
        op2_set_patch(tr, &OP2_FACTORY[s]);
        op2_slot[tr] = (uint8_t)s;
    }
}

/* --------------------------------------------------------- the engine --- */
static const char *const N_OP2_ALG[] = {"PAT", "1", "2", "3", "4", "5", "6", "7", "8", 0};
static const char *const N_OP2_LGCY[] = {"CLEAN", "EARLY", "LATE", "CLN S&H", "ERL S&H", "LAT S&H", 0};
static const char *const N_OP2_SLOT[] = {"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
                                         "F13", "F14", "F15", "F16", "F17", "F18", "F19", "F20", "F21", "F22", "F23",
                                         "F24", "F25", "F26", "F27", "F28", "F29", "F30", "F31", "F32", "OWN", 0};
_Static_assert(OP2_NFACTORY <= 32u, "SLOT names: F1..F32");

static const engine_t ENG_OPN2 = {
    .name = "OPN2",
    .page_title = {"OPN2", "PATCH"},
    .edit = {
        {"ALG", F_INT, 0, 8, 0, N_OP2_ALG, 0},
        {"FB", F_INT, -7, 7, 0, 0, 0},
        {"MLVL", F_BIPCT, -64, 63, 0, 0, 0},
        {"MEG", F_BIPCT, -64, 63, 0, 0, 0},
        {"VSEN", F_PCT, 0, 127, 64, 0, 0},
        {"LADR", F_PCT, 0, 127, 64, 0, 0},
        {"LGCY", F_INT, 0, 5, 1, N_OP2_LGCY, 0},
        {"SLOT", F_INT, 0, (int16_t)OP2_OWN, 0, N_OP2_SLOT, 0},
    },
    .presets = OP2_PRESETS,
    .npresets = NELEM(OP2_PRESETS),
    .note_on = op2_note_on,
    .render = op2_render,
    .knob = {P_E0, P_E2, P_E3, P_E7},
    .poly = OPN2_CHANNELS,
    .keep = 0x01,                               /* s[0]: the channel (a retrigger keeps it) */
    .ownenv = 1,
    .done = op2_done,
};
