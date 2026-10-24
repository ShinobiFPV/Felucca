/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2021, Aaron Giles (ymfm, https://github.com/aaronsgiles/ymfm)
 * C port: Copyright (C) 2026 William Kew, ShinTech Electronics
 * LICENSES/BSD-3-Clause-ymfm.txt */
/* OPN2: the YM2612's FM, a C port of ymfm's OPN2 path (ymfm_fm.ipp fm_operator / fm_channel / fm_engine_base,
 * ymfm_opn.cpp opn_registers_base<true> and ym2612), from ymfm 81aec25 (2026-07-27). Integer only; nothing from the
 * other chips ymfm models (no rhythm, noise, OPL waveforms, timers or CSM: ymfm's default interface never fires its
 * timers either, so the output is the same). The port keeps ymfm's names and order so it can be read side by side,
 * and tests/opn2_ymfm_test.cpp checks it against ymfm sample for sample.
 *
 *   opn2_reset(c)                   power-on state (ymfm: construct + reset)
 *   opn2_write(c, port, data)       the chip's four ports: 0 address, 1 data, 2 address (upper), 3 data (upper)
 *   opn2_render_n(c, out, n, nout)  n samples at the native rate (clock / 144: 53,267 Hz at 7.67 MHz): every channel
 *                                   clocked, channels 0..nout-1 heard (out[s][ch] its 9-bit L and R, 0 muted or silent)
 *   opn2_generate_n(c, lr, n)       ymfm's ym2612::generate, n samples: the six channels with the DAC discontinuity
 *   opn2_render, opn2_generate      the same, one sample
 *
 * Speed (the FM-1 renders it in the audio ISR): what ymfm reads from the registers every sample is cached when the
 * channels are prepared (after any write, as ymfm prepares them), the PM LFO's phase steps are recomputed only when its
 * value changes, the sine and the power tables are in RAM, and an operator that is silent for good (released, at full
 * attenuation, no SSG-EG) is not clocked: its phase is never heard and a key on resets it. An operator's attenuation
 * (envelope, SSG-EG inversion, AM, TL: ymfm's envelope_attenuation, << 2) is kept in att4 and made again only when one
 * of its inputs moves: an envelope tick (every third sample), SSG-EG, a new AM value, a prepare. Each channel is
 * clocked and heard in one pass, and each algorithm has its own path. None of it changes a sample.
 *
 * Every function is static: Felucca builds as one translation unit (felucca.c); the host tests include this file.
 * OPN2_POOL places the shared tables (the firmware: its .pool section), OPN2_FAST the per-sample code. */
#ifndef OPN2_C_INCLUDED                /* (the engine and the benchmark both include it) */
#define OPN2_C_INCLUDED
#include <stdint.h>
#ifndef OPN2_POOL
#define OPN2_POOL
#endif
#ifndef OPN2_FAST
#define OPN2_FAST                 /* the per-sample functions (the firmware: its .fast_text section, run from RAM) */
#endif
/* (the firmware builds at -Os, which would leave these out of line; code in RAM cannot reach a call into XIP) */
#define OPN2_AI __attribute__((always_inline)) inline
/* a call across RAM and XIP: through a pointer the compiler cannot see through (as hal/fm1_flash.h FL_FAR). Callers in
 * XIP call the OPN2_FAST entries (opn2_render_n, opn2_generate_n) as OPN2_FAR(opn2_render_n)(...); the one-sample
 * opn2_render and opn2_generate are in XIP and do it themselves */
static OPN2_AI void *opn2_far(void *p) { void *volatile q = p; return q; }
#define OPN2_FAR(fn) ((__typeof__(&fn))opn2_far((void *)&fn))

#define OPN2_CHANNELS 6u
#define OPN2_OPERATORS 24u
#define OPN2_ALL_CHANNELS 0x3Fu
#define OPN2_EG_QUIET 0x380u
#define OPN2_PHASE_DYNAMIC 1u     /* opdata_cache::PHASE_STEP_DYNAMIC: recompute each sample (PM LFO) */
enum { OPN2_EG_ATTACK = 1, OPN2_EG_DECAY = 2, OPN2_EG_SUSTAIN = 3, OPN2_EG_RELEASE = 4, OPN2_EG_STATES = 6 };

typedef struct {                  /* (64 bytes, what the render reads first: one shift to index, short offsets) */
    uint32_t phase;               /* 10.10 */
    uint32_t phase_step;          /* opdata_cache's, or PHASE_DYNAMIC */
    uint32_t dyn_step;            /* PHASE_DYNAMIC: the step for dyn_pm */
    int32_t dyn_pm;               /* .. the LFO PM value it was made for */
    uint16_t env_att;             /* 4.6 attenuation */
    uint16_t att4;                /* envelope_attenuation(am) << 2, as last made (opn2_att_update) */
    uint8_t env_state;
    uint8_t quiet;                /* env_att > EG_QUIET: compute_volume's early out */
    uint8_t idle;                 /* released, at full attenuation, no SSG-EG: silent for good, not clocked */
    uint8_t ssg_en, ssg_inv, ssg_mode, am_en;   /* (ssg_en, ssg_mode, am_en: the registers at the last prepare) */
    uint8_t key_state, keyon_live;
    uint8_t eg_rate[OPN2_EG_STATES];
    uint8_t pad0[3];
    uint16_t opoffs, choffs;      /* register offsets */
    /* the rest of opdata_cache */
    uint32_t block_freq, multiple, total_level, eg_sustain;
    int32_t detune;
} opn2_op_t;
_Static_assert(sizeof(opn2_op_t) == 64u, "opn2_op_t: 64 bytes (a shift to index)");

typedef struct {
    int16_t fb[2], fb_in;         /* operator 1's feedback memory; fb_in: set by the output */
    uint16_t choffs;
    uint8_t alg;                  /* the registers, as cached at the last prepare: the algorithm */
    uint8_t fb_shift;             /* 10 - feedback, 0 = no feedback */
    uint8_t out_l, out_r, am_shift, pm_sens;
} opn2_ch_t;

typedef struct {
    opn2_op_t op[OPN2_OPERATORS];     /* (first, and reg[] last: every field the render reads is a short offset from its
                                       * pointer; the target's loads take a short offset in one instruction) */
    opn2_ch_t ch[OPN2_CHANNELS];
    uint32_t env_counter;         /* low 2 bits: sub-counter */
    uint32_t lfo_counter;
    uint32_t prepare_count;
    uint8_t lfo_am, lfo_am_prev;  /* this sample's AM value, the last sample's */
    uint8_t active, modified;     /* channel masks */
    uint8_t dac_en;
    uint16_t addr, dac_data;      /* address register (bit 8: the upper port), 9-bit DAC */
    uint8_t reg[0x200];
} opn2_t;

/* operators of each channel, in ymfm's order: carrier 1, carrier 2, modulator 1, modulator 2 as the register map puts
 * them, i.e. OP1, OP3, OP2, OP4 by slot (opn_registers_base<true>::operator_map): ymfm's operator numbers. The port
 * stores channel ch's k-th as op[ch * 4 + k] (OPN2_OP), its register offset from its ymfm number */
static const uint8_t OPN2_MAP[OPN2_CHANNELS][4] = {
    {0, 6, 3, 9}, {1, 7, 4, 10}, {2, 8, 5, 11}, {12, 18, 15, 21}, {13, 19, 16, 22}, {14, 20, 17, 23}};
#define OPN2_OP(c, ch, k) (&(c)->op[(ch) * 4u + (k)])

static OPN2_AI uint32_t opn2_bf(uint32_t v, uint32_t start, uint32_t len) { return (v >> start) & ((1u << len) - 1u); }
#define OPN2_RB(c, off, s, l, x) opn2_bf((c)->reg[(off) + (x)], (s), (l))

/* -------------------------------------------------------------- tables --- */
/* |sin| as 4.8 attenuation, quarter wave (ymfm abs_sin_attenuation: the OPN die's table) */
static const uint16_t OPN2_SIN[256] = {
    0x859,0x6c3,0x607,0x58b,0x52e,0x4e4,0x4a6,0x471,0x443,0x41a,0x3f5,0x3d3,0x3b5,0x398,0x37e,0x365,
    0x34e,0x339,0x324,0x311,0x2ff,0x2ed,0x2dc,0x2cd,0x2bd,0x2af,0x2a0,0x293,0x286,0x279,0x26d,0x261,
    0x256,0x24b,0x240,0x236,0x22c,0x222,0x218,0x20f,0x206,0x1fd,0x1f5,0x1ec,0x1e4,0x1dc,0x1d4,0x1cd,
    0x1c5,0x1be,0x1b7,0x1b0,0x1a9,0x1a2,0x19b,0x195,0x18f,0x188,0x182,0x17c,0x177,0x171,0x16b,0x166,
    0x160,0x15b,0x155,0x150,0x14b,0x146,0x141,0x13c,0x137,0x133,0x12e,0x129,0x125,0x121,0x11c,0x118,
    0x114,0x10f,0x10b,0x107,0x103,0x0ff,0x0fb,0x0f8,0x0f4,0x0f0,0x0ec,0x0e9,0x0e5,0x0e2,0x0de,0x0db,
    0x0d7,0x0d4,0x0d1,0x0cd,0x0ca,0x0c7,0x0c4,0x0c1,0x0be,0x0bb,0x0b8,0x0b5,0x0b2,0x0af,0x0ac,0x0a9,
    0x0a7,0x0a4,0x0a1,0x09f,0x09c,0x099,0x097,0x094,0x092,0x08f,0x08d,0x08a,0x088,0x086,0x083,0x081,
    0x07f,0x07d,0x07a,0x078,0x076,0x074,0x072,0x070,0x06e,0x06c,0x06a,0x068,0x066,0x064,0x062,0x060,
    0x05e,0x05c,0x05b,0x059,0x057,0x055,0x053,0x052,0x050,0x04e,0x04d,0x04b,0x04a,0x048,0x046,0x045,
    0x043,0x042,0x040,0x03f,0x03e,0x03c,0x03b,0x039,0x038,0x037,0x035,0x034,0x033,0x031,0x030,0x02f,
    0x02e,0x02d,0x02b,0x02a,0x029,0x028,0x027,0x026,0x025,0x024,0x023,0x022,0x021,0x020,0x01f,0x01e,
    0x01d,0x01c,0x01b,0x01a,0x019,0x018,0x017,0x017,0x016,0x015,0x014,0x014,0x013,0x012,0x011,0x011,
    0x010,0x00f,0x00f,0x00e,0x00d,0x00d,0x00c,0x00c,0x00b,0x00a,0x00a,0x009,0x009,0x008,0x008,0x007,
    0x007,0x007,0x006,0x006,0x005,0x005,0x005,0x004,0x004,0x004,0x003,0x003,0x003,0x002,0x002,0x002,
    0x002,0x001,0x001,0x001,0x001,0x001,0x001,0x001,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000};

/* 5.8 attenuation -> 13-bit volume (ymfm attenuation_to_volume: the 0x400 bit folded in, << 2, reversed) */
#define OPN2_X(a) (((a) | 0x400) << 2)
static const uint16_t OPN2_POW[256] = {
    OPN2_X(0x3fa),OPN2_X(0x3f5),OPN2_X(0x3ef),OPN2_X(0x3ea),OPN2_X(0x3e4),OPN2_X(0x3df),OPN2_X(0x3da),OPN2_X(0x3d4),
    OPN2_X(0x3cf),OPN2_X(0x3c9),OPN2_X(0x3c4),OPN2_X(0x3bf),OPN2_X(0x3b9),OPN2_X(0x3b4),OPN2_X(0x3ae),OPN2_X(0x3a9),
    OPN2_X(0x3a4),OPN2_X(0x39f),OPN2_X(0x399),OPN2_X(0x394),OPN2_X(0x38f),OPN2_X(0x38a),OPN2_X(0x384),OPN2_X(0x37f),
    OPN2_X(0x37a),OPN2_X(0x375),OPN2_X(0x370),OPN2_X(0x36a),OPN2_X(0x365),OPN2_X(0x360),OPN2_X(0x35b),OPN2_X(0x356),
    OPN2_X(0x351),OPN2_X(0x34c),OPN2_X(0x347),OPN2_X(0x342),OPN2_X(0x33d),OPN2_X(0x338),OPN2_X(0x333),OPN2_X(0x32e),
    OPN2_X(0x329),OPN2_X(0x324),OPN2_X(0x31f),OPN2_X(0x31a),OPN2_X(0x315),OPN2_X(0x310),OPN2_X(0x30b),OPN2_X(0x306),
    OPN2_X(0x302),OPN2_X(0x2fd),OPN2_X(0x2f8),OPN2_X(0x2f3),OPN2_X(0x2ee),OPN2_X(0x2e9),OPN2_X(0x2e5),OPN2_X(0x2e0),
    OPN2_X(0x2db),OPN2_X(0x2d6),OPN2_X(0x2d2),OPN2_X(0x2cd),OPN2_X(0x2c8),OPN2_X(0x2c4),OPN2_X(0x2bf),OPN2_X(0x2ba),
    OPN2_X(0x2b5),OPN2_X(0x2b1),OPN2_X(0x2ac),OPN2_X(0x2a8),OPN2_X(0x2a3),OPN2_X(0x29e),OPN2_X(0x29a),OPN2_X(0x295),
    OPN2_X(0x291),OPN2_X(0x28c),OPN2_X(0x288),OPN2_X(0x283),OPN2_X(0x27f),OPN2_X(0x27a),OPN2_X(0x276),OPN2_X(0x271),
    OPN2_X(0x26d),OPN2_X(0x268),OPN2_X(0x264),OPN2_X(0x25f),OPN2_X(0x25b),OPN2_X(0x257),OPN2_X(0x252),OPN2_X(0x24e),
    OPN2_X(0x249),OPN2_X(0x245),OPN2_X(0x241),OPN2_X(0x23c),OPN2_X(0x238),OPN2_X(0x234),OPN2_X(0x230),OPN2_X(0x22b),
    OPN2_X(0x227),OPN2_X(0x223),OPN2_X(0x21e),OPN2_X(0x21a),OPN2_X(0x216),OPN2_X(0x212),OPN2_X(0x20e),OPN2_X(0x209),
    OPN2_X(0x205),OPN2_X(0x201),OPN2_X(0x1fd),OPN2_X(0x1f9),OPN2_X(0x1f5),OPN2_X(0x1f0),OPN2_X(0x1ec),OPN2_X(0x1e8),
    OPN2_X(0x1e4),OPN2_X(0x1e0),OPN2_X(0x1dc),OPN2_X(0x1d8),OPN2_X(0x1d4),OPN2_X(0x1d0),OPN2_X(0x1cc),OPN2_X(0x1c8),
    OPN2_X(0x1c4),OPN2_X(0x1c0),OPN2_X(0x1bc),OPN2_X(0x1b8),OPN2_X(0x1b4),OPN2_X(0x1b0),OPN2_X(0x1ac),OPN2_X(0x1a8),
    OPN2_X(0x1a4),OPN2_X(0x1a0),OPN2_X(0x19c),OPN2_X(0x199),OPN2_X(0x195),OPN2_X(0x191),OPN2_X(0x18d),OPN2_X(0x189),
    OPN2_X(0x185),OPN2_X(0x181),OPN2_X(0x17e),OPN2_X(0x17a),OPN2_X(0x176),OPN2_X(0x172),OPN2_X(0x16f),OPN2_X(0x16b),
    OPN2_X(0x167),OPN2_X(0x163),OPN2_X(0x160),OPN2_X(0x15c),OPN2_X(0x158),OPN2_X(0x154),OPN2_X(0x151),OPN2_X(0x14d),
    OPN2_X(0x149),OPN2_X(0x146),OPN2_X(0x142),OPN2_X(0x13e),OPN2_X(0x13b),OPN2_X(0x137),OPN2_X(0x134),OPN2_X(0x130),
    OPN2_X(0x12c),OPN2_X(0x129),OPN2_X(0x125),OPN2_X(0x122),OPN2_X(0x11e),OPN2_X(0x11b),OPN2_X(0x117),OPN2_X(0x114),
    OPN2_X(0x110),OPN2_X(0x10c),OPN2_X(0x109),OPN2_X(0x106),OPN2_X(0x102),OPN2_X(0x0ff),OPN2_X(0x0fb),OPN2_X(0x0f8),
    OPN2_X(0x0f4),OPN2_X(0x0f1),OPN2_X(0x0ed),OPN2_X(0x0ea),OPN2_X(0x0e7),OPN2_X(0x0e3),OPN2_X(0x0e0),OPN2_X(0x0dc),
    OPN2_X(0x0d9),OPN2_X(0x0d6),OPN2_X(0x0d2),OPN2_X(0x0cf),OPN2_X(0x0cc),OPN2_X(0x0c8),OPN2_X(0x0c5),OPN2_X(0x0c2),
    OPN2_X(0x0be),OPN2_X(0x0bb),OPN2_X(0x0b8),OPN2_X(0x0b5),OPN2_X(0x0b1),OPN2_X(0x0ae),OPN2_X(0x0ab),OPN2_X(0x0a8),
    OPN2_X(0x0a4),OPN2_X(0x0a1),OPN2_X(0x09e),OPN2_X(0x09b),OPN2_X(0x098),OPN2_X(0x094),OPN2_X(0x091),OPN2_X(0x08e),
    OPN2_X(0x08b),OPN2_X(0x088),OPN2_X(0x085),OPN2_X(0x082),OPN2_X(0x07e),OPN2_X(0x07b),OPN2_X(0x078),OPN2_X(0x075),
    OPN2_X(0x072),OPN2_X(0x06f),OPN2_X(0x06c),OPN2_X(0x069),OPN2_X(0x066),OPN2_X(0x063),OPN2_X(0x060),OPN2_X(0x05d),
    OPN2_X(0x05a),OPN2_X(0x057),OPN2_X(0x054),OPN2_X(0x051),OPN2_X(0x04e),OPN2_X(0x04b),OPN2_X(0x048),OPN2_X(0x045),
    OPN2_X(0x042),OPN2_X(0x03f),OPN2_X(0x03c),OPN2_X(0x039),OPN2_X(0x036),OPN2_X(0x033),OPN2_X(0x030),OPN2_X(0x02d),
    OPN2_X(0x02a),OPN2_X(0x028),OPN2_X(0x025),OPN2_X(0x022),OPN2_X(0x01f),OPN2_X(0x01c),OPN2_X(0x019),OPN2_X(0x016),
    OPN2_X(0x014),OPN2_X(0x011),OPN2_X(0x00e),OPN2_X(0x00b),OPN2_X(0x008),OPN2_X(0x006),OPN2_X(0x003),OPN2_X(0x000)};
#undef OPN2_X

/* ymfm attenuation_increment: 8 steps of 4 bits per rate */
static const uint32_t OPN2_INC[64] = {
    0x00000000, 0x00000000, 0x10101010, 0x10101010, 0x10101010, 0x10101010, 0x11101110, 0x11101110,
    0x10101010, 0x10111010, 0x11101110, 0x11111110, 0x10101010, 0x10111010, 0x11101110, 0x11111110,
    0x10101010, 0x10111010, 0x11101110, 0x11111110, 0x10101010, 0x10111010, 0x11101110, 0x11111110,
    0x10101010, 0x10111010, 0x11101110, 0x11111110, 0x10101010, 0x10111010, 0x11101110, 0x11111110,
    0x10101010, 0x10111010, 0x11101110, 0x11111110, 0x10101010, 0x10111010, 0x11101110, 0x11111110,
    0x10101010, 0x10111010, 0x11101110, 0x11111110, 0x10101010, 0x10111010, 0x11101110, 0x11111110,
    0x11111111, 0x21112111, 0x21212121, 0x22212221, 0x22222222, 0x42224222, 0x42424242, 0x44424442,
    0x44444444, 0x84448444, 0x84848484, 0x88848884, 0x88888888, 0x88888888, 0x88888888, 0x88888888};

/* ymfm detune_adjustment: keycode x DT1 & 3 */
static const uint8_t OPN2_DT[32][4] = {
    {0, 0, 1, 2},  {0, 0, 1, 2},  {0, 0, 1, 2},  {0, 0, 1, 2},  {0, 1, 2, 2},  {0, 1, 2, 3},  {0, 1, 2, 3},  {0, 1, 2, 3},
    {0, 1, 2, 4},  {0, 1, 3, 4},  {0, 1, 3, 4},  {0, 1, 3, 5},  {0, 2, 4, 5},  {0, 2, 4, 6},  {0, 2, 4, 6},  {0, 2, 5, 7},
    {0, 2, 5, 8},  {0, 3, 6, 8},  {0, 3, 6, 9},  {0, 3, 7, 10}, {0, 4, 8, 11}, {0, 4, 8, 12}, {0, 4, 9, 13}, {0, 5, 10, 14},
    {0, 5, 11, 16}, {0, 6, 12, 17}, {0, 6, 13, 19}, {0, 7, 14, 20}, {0, 8, 16, 22}, {0, 8, 16, 22}, {0, 8, 16, 22}, {0, 8, 16, 22}};

/* ymfm opn_lfo_pm_phase_adjustment: two shifts of the top 7 F-number bits per PM depth and LFO step */
static const uint8_t OPN2_PM_SHIFTS[8][8] = {
    {0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77}, {0x77, 0x77, 0x77, 0x77, 0x72, 0x72, 0x72, 0x72},
    {0x77, 0x77, 0x77, 0x72, 0x72, 0x72, 0x17, 0x17}, {0x77, 0x77, 0x72, 0x72, 0x17, 0x17, 0x12, 0x12},
    {0x77, 0x77, 0x72, 0x17, 0x17, 0x17, 0x12, 0x07}, {0x77, 0x77, 0x17, 0x12, 0x07, 0x07, 0x02, 0x01},
    {0x77, 0x77, 0x17, 0x12, 0x07, 0x07, 0x02, 0x01}, {0x77, 0x77, 0x17, 0x12, 0x07, 0x07, 0x02, 0x01}};

static const uint8_t OPN2_LFO_MAX[8] = {109, 78, 72, 68, 63, 45, 9, 6};

/* ------------------------------------------------------------- helpers --- */
static uint16_t opn2_wave[1024] OPN2_POOL;     /* m_waveform[0]: |sin| attenuation, bit 15 the sign (opn2_reset) */
static uint16_t opn2_pow[256] OPN2_POOL;       /* OPN2_POW in RAM */
static void opn2_wave_init(void)
{
    uint32_t i, in;
    for (i = 0; i < 1024u; i++) {
        in = opn2_bf(i, 8, 1) ? ~i : i;
        opn2_wave[i] = (uint16_t)(OPN2_SIN[in & 0xFFu] | (opn2_bf(i, 9, 1) << 15));
    }
    for (i = 0; i < 256u; i++)
        opn2_pow[i] = OPN2_POW[i];
}
static OPN2_AI uint32_t opn2_eff_rate(uint32_t raw, uint32_t ksr) { return raw == 0 ? 0 : (raw + ksr < 63u ? raw + ksr : 63u); }
static OPN2_AI int32_t opn2_clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

static int32_t opn2_detune(uint32_t detune, uint32_t keycode)
{
    int32_t r = OPN2_DT[keycode][detune & 3u];
    return opn2_bf(detune, 2, 1) ? -r : r;
}

static OPN2_AI int32_t opn2_pm_adjust(uint32_t fnum_bits, uint32_t pm_sens, int32_t lfo_raw_pm)
{
    int32_t abs_pm = lfo_raw_pm < 0 ? -lfo_raw_pm : lfo_raw_pm;
    uint32_t shifts = OPN2_PM_SHIFTS[pm_sens][opn2_bf((uint32_t)abs_pm, 0, 3)];
    int32_t adjust = (int32_t)((fnum_bits >> opn2_bf(shifts, 0, 4)) + (fnum_bits >> opn2_bf(shifts, 4, 4)));
    if (pm_sens > 5)
        adjust <<= pm_sens - 5;
    adjust >>= 2;
    return lfo_raw_pm < 0 ? -adjust : adjust;
}

/* ------------------------------------------------------ the registers --- */
static inline uint32_t opn2_ch_block_freq(const opn2_t *c, uint32_t choffs)
{
    return OPN2_RB(c, 0xA4, 0, 6, choffs) << 8 | OPN2_RB(c, 0xA0, 0, 8, choffs);
}
static inline uint32_t opn2_multi_block_freq(const opn2_t *c, uint32_t num)
{
    return OPN2_RB(c, 0xAC, 0, 6, num) << 8 | OPN2_RB(c, 0xA8, 0, 8, num);
}

static OPN2_FAST uint32_t opn2_phase_step(const opn2_op_t *o, uint32_t pm_sens, int32_t lfo_raw_pm)   /* compute_phase_step */
{
    uint32_t fnum = opn2_bf(o->block_freq, 0, 11) << 1, block, step;
    if (pm_sens) {
        fnum += (uint32_t)opn2_pm_adjust(opn2_bf(o->block_freq, 4, 7), pm_sens, lfo_raw_pm);
        fnum &= 0xFFFu;
    }
    block = opn2_bf(o->block_freq, 11, 3);
    step = (fnum << block) >> 2;
    step += (uint32_t)o->detune;
    step &= 0x1FFFFu;
    return (step * o->multiple) >> 1;
}

static void opn2_cache(const opn2_t *c, opn2_op_t *o)   /* cache_operator_data */
{
    uint32_t choffs = o->choffs, opoffs = o->opoffs, bfq, keycode, ksrval;
    bfq = o->block_freq = opn2_ch_block_freq(c, choffs);
    if (OPN2_RB(c, 0x27, 6, 2, 0) != 0 && choffs == 2) {   /* multi-frequency mode, channel 3 */
        if (opoffs == 2)
            bfq = o->block_freq = opn2_multi_block_freq(c, 1);
        else if (opoffs == 10)
            bfq = o->block_freq = opn2_multi_block_freq(c, 2);
        else if (opoffs == 6)
            bfq = o->block_freq = opn2_multi_block_freq(c, 0);
    }
    keycode = opn2_bf(bfq, 10, 4) << 1;
    keycode |= opn2_bf(0xFE80u, opn2_bf(bfq, 7, 4), 1);
    o->detune = opn2_detune(OPN2_RB(c, 0x30, 4, 3, opoffs), keycode);
    o->multiple = OPN2_RB(c, 0x30, 0, 4, opoffs) * 2u;
    if (o->multiple == 0)
        o->multiple = 1;
    if (OPN2_RB(c, 0x22, 3, 1, 0) == 0 || OPN2_RB(c, 0xB4, 0, 3, choffs) == 0)
        o->phase_step = OPN2_FAR(opn2_phase_step)(o, OPN2_RB(c, 0xB4, 0, 3, choffs), 0);   /* (in RAM) */
    else
        o->phase_step = OPN2_PHASE_DYNAMIC;
    o->dyn_pm = INT32_MIN;
    o->ssg_en = (uint8_t)OPN2_RB(c, 0x90, 3, 1, opoffs);
    o->ssg_mode = (uint8_t)OPN2_RB(c, 0x90, 0, 3, opoffs);
    o->am_en = (uint8_t)OPN2_RB(c, 0x60, 7, 1, opoffs);
    o->total_level = OPN2_RB(c, 0x40, 0, 7, opoffs) << 3;
    o->eg_sustain = OPN2_RB(c, 0x80, 4, 4, opoffs);
    o->eg_sustain |= (o->eg_sustain + 1u) & 0x10u;
    o->eg_sustain <<= 5;
    ksrval = keycode >> (OPN2_RB(c, 0x50, 6, 2, opoffs) ^ 3u);
    o->eg_rate[OPN2_EG_ATTACK] = (uint8_t)opn2_eff_rate(OPN2_RB(c, 0x50, 0, 5, opoffs) * 2u, ksrval);
    o->eg_rate[OPN2_EG_DECAY] = (uint8_t)opn2_eff_rate(OPN2_RB(c, 0x60, 0, 5, opoffs) * 2u, ksrval);
    o->eg_rate[OPN2_EG_SUSTAIN] = (uint8_t)opn2_eff_rate(OPN2_RB(c, 0x70, 0, 5, opoffs) * 2u, ksrval);
    o->eg_rate[OPN2_EG_RELEASE] = (uint8_t)opn2_eff_rate(OPN2_RB(c, 0x80, 0, 4, opoffs) * 4u + 2u, ksrval);
}

static OPN2_AI int32_t opn2_clock_lfo(opn2_t *c)   /* clock_noise_and_lfo: the raw PM value */
{
    uint32_t subcount;
    int32_t pm;
    if (!OPN2_RB(c, 0x22, 3, 1, 0)) {
        c->lfo_counter = 0;
        c->lfo_am = 0x3F;                     /* LFO off: the counter rests at 0, which is AM 0x3F */
        return 0;
    }
    subcount = (uint8_t)c->lfo_counter++;
    if (subcount >= OPN2_LFO_MAX[OPN2_RB(c, 0x22, 0, 3, 0)])
        c->lfo_counter += 0x101u - subcount;
    c->lfo_am = (uint8_t)opn2_bf(c->lfo_counter, 8, 6);
    if (opn2_bf(c->lfo_counter, 8 + 6, 1) == 0)
        c->lfo_am ^= 0x3F;
    pm = (int32_t)opn2_bf(c->lfo_counter, 10, 3);
    if (opn2_bf(c->lfo_counter, 10 + 3, 1))
        pm ^= 7;
    return opn2_bf(c->lfo_counter, 10 + 4, 1) ? -pm : pm;
}

static void opn2_ch_cache(const opn2_t *c, opn2_ch_t *h)   /* the channel registers the clock and the output read */
{
    uint32_t choffs = h->choffs, fb = OPN2_RB(c, 0xB0, 3, 3, choffs);
    h->alg = (uint8_t)OPN2_RB(c, 0xB0, 0, 3, choffs);
    h->fb_shift = (uint8_t)(fb ? 10u - fb : 0u);
    h->out_l = (uint8_t)OPN2_RB(c, 0xB4, 7, 1, choffs);
    h->out_r = (uint8_t)OPN2_RB(c, 0xB4, 6, 1, choffs);
    h->am_shift = (uint8_t)((1u << (OPN2_RB(c, 0xB4, 4, 2, choffs) ^ 3u)) - 1u);
    h->pm_sens = (uint8_t)OPN2_RB(c, 0xB4, 0, 3, choffs);
}

/* ------------------------------------------------------- the operator --- */
static OPN2_AI void opn2_start_attack(opn2_op_t *o, int is_restart)
{
    if (o->env_state == OPN2_EG_ATTACK)
        return;
    o->env_state = OPN2_EG_ATTACK;
    if (!is_restart)
        o->ssg_inv = (uint8_t)(o->ssg_en & opn2_bf(o->ssg_mode, 2, 1));
    if (!is_restart)
        o->phase = 0;
    if (o->eg_rate[OPN2_EG_ATTACK] >= 62)
        o->env_att = 0;
}

static void opn2_start_release(opn2_op_t *o)
{
    if (o->env_state >= OPN2_EG_RELEASE)
        return;
    o->env_state = OPN2_EG_RELEASE;
    if (o->ssg_inv) {
        o->env_att = (uint16_t)((0x200u - o->env_att) & 0x3FFu);
        o->ssg_inv = 0;
    }
}

static void opn2_clock_keystate(opn2_op_t *o, uint32_t keystate)
{
    if ((keystate ^ o->key_state) != 0) {
        o->key_state = (uint8_t)keystate;
        if (keystate)
            opn2_start_attack(o, 0);
        else
            opn2_start_release(o);
    }
}

static int opn2_op_prepare(const opn2_t *c, opn2_op_t *o)
{
    opn2_cache(c, o);
    opn2_clock_keystate(o, o->keyon_live != 0);
    o->keyon_live &= (uint8_t)~(1u << 2);     /* KEYON_CSM */
    return o->env_state != OPN2_EG_RELEASE || o->env_att < OPN2_EG_QUIET;
}

static OPN2_AI void opn2_clock_ssg(opn2_op_t *o)   /* clock_ssg_eg_state (SSG-EG on) */
{
    uint32_t mode;
    if (!opn2_bf(o->env_att, 9, 1))
        return;
    mode = o->ssg_mode;
    if (opn2_bf(mode, 0, 1)) {                 /* hold modes 1 3 5 7 */
        o->ssg_inv = (uint8_t)(opn2_bf(mode, 2, 1) ^ opn2_bf(mode, 1, 1));
        if (o->env_state != OPN2_EG_ATTACK)
            o->env_att = o->ssg_inv ? 0x200 : 0x3FF;
    } else {                                   /* continuous modes 0 2 4 6 */
        o->ssg_inv ^= (uint8_t)opn2_bf(mode, 1, 1);
        if (o->env_state == OPN2_EG_DECAY || o->env_state == OPN2_EG_SUSTAIN)
            opn2_start_attack(o, 1);
        if (opn2_bf(mode, 1, 1) == 0)
            o->phase = 0;
    }
    if (o->env_state == OPN2_EG_RELEASE)
        o->env_att = 0x3FF;
}

static OPN2_AI void opn2_clock_envelope(opn2_op_t *o, uint32_t env_counter)
{
    uint32_t rate, rate_shift, relevant, inc;
    if (o->env_state == OPN2_EG_ATTACK && o->env_att == 0)
        o->env_state = OPN2_EG_DECAY;
    if (o->env_state == OPN2_EG_DECAY && o->env_att >= o->eg_sustain)
        o->env_state = OPN2_EG_SUSTAIN;
    rate = o->eg_rate[o->env_state];
    rate_shift = rate >> 2;
    env_counter <<= rate_shift;
    if (opn2_bf(env_counter, 0, 11) != 0)
        return;
    relevant = opn2_bf(env_counter, rate_shift <= 11 ? 11 : rate_shift, 3);
    inc = opn2_bf(OPN2_INC[rate], 4 * relevant, 4);
    if (o->env_state == OPN2_EG_ATTACK) {
        if (rate < 62)
            o->env_att = (uint16_t)(o->env_att + ((~(uint32_t)o->env_att * inc) >> 4));
    } else {
        if (!o->ssg_en)
            o->env_att = (uint16_t)(o->env_att + inc);
        else if (o->env_att < 0x200)
            o->env_att = (uint16_t)(o->env_att + 4 * inc);
        if (o->env_att >= 0x400)
            o->env_att = 0x3FF;
    }
}


/* envelope_attenuation(am) << 2 and compute_volume's early out, made again whenever an input moved */
static OPN2_AI void opn2_att_update(opn2_op_t *o, uint32_t am_offset)
{
    uint32_t r = o->env_att;
    if (o->ssg_inv)
        r = (0x200u - r) & 0x3FFu;
    if (o->am_en)
        r += am_offset;
    r += o->total_level;
    o->att4 = (uint16_t)((r < 0x3FFu ? r : 0x3FFu) << 2);
    o->quiet = o->env_att > OPN2_EG_QUIET;
}

static OPN2_AI int32_t opn2_vol(const opn2_op_t *o, uint32_t phase)   /* compute_volume */
{
    uint32_t s, a;
    int32_t r;
    if (o->quiet)
        return 0;
    s = opn2_wave[phase & 0x3FFu];
    a = (s & 0x7FFFu) + o->att4;
    r = (int32_t)(opn2_pow[a & 0xFFu] >> (a >> 8));
    return (s & 0x8000u) ? -r : r;
}

/* --------------------------------------------------------- the channel --- */
/* fm_channel::output_4op (rshift 5, clipmax 256) with ymfm's add_to_output, one path per algorithm (its
 * s_algorithm_ops): the operators' modulation inputs are the earlier outputs >> 1, sums of two as ymfm's int16 opout */
#define OPN2_CL(x) ((x) < -257 ? -257 : (x) > 256 ? 256 : (x))
static OPN2_AI void opn2_out(opn2_t *c, opn2_ch_t *h, uint32_t ch, int32_t *lr)
{
    const opn2_op_t *o1 = OPN2_OP(c, ch, 0), *o2 = o1 + 1, *o3 = o1 + 2, *o4 = o1 + 3;
    int32_t m = 0, v1, v2, v3, r;
    uint32_t p2, p3, p4;
    if (h->fb_shift)
        m = (h->fb[0] + h->fb[1]) >> h->fb_shift;
    v1 = h->fb_in = (int16_t)opn2_vol(o1, (o1->phase >> 10) + (uint32_t)m);
    if (!(h->out_l | h->out_r))
        return;
    p2 = o2->phase >> 10;
    p3 = o3->phase >> 10;
    p4 = o4->phase >> 10;
    switch (h->alg) {
    case 0:                                         /* O1 -> O2 -> O3 -> O4 */
        v2 = opn2_vol(o2, p2 + (uint32_t)(v1 >> 1));
        v3 = opn2_vol(o3, p3 + (uint32_t)(v2 >> 1));
        r = opn2_vol(o4, p4 + (uint32_t)(v3 >> 1)) >> 5;
        break;
    case 1:                                         /* (O1 + O2) -> O3 -> O4 */
        v2 = opn2_vol(o2, p2);
        v3 = opn2_vol(o3, p3 + (uint32_t)((int16_t)(v1 + v2) >> 1));
        r = opn2_vol(o4, p4 + (uint32_t)(v3 >> 1)) >> 5;
        break;
    case 2:                                         /* (O1 + (O2 -> O3)) -> O4 */
        v2 = opn2_vol(o2, p2);
        v3 = opn2_vol(o3, p3 + (uint32_t)(v2 >> 1));
        r = opn2_vol(o4, p4 + (uint32_t)((int16_t)(v1 + v3) >> 1)) >> 5;
        break;
    case 3:                                         /* ((O1 -> O2) + O3) -> O4 */
        v2 = opn2_vol(o2, p2 + (uint32_t)(v1 >> 1));
        v3 = opn2_vol(o3, p3);
        r = opn2_vol(o4, p4 + (uint32_t)((int16_t)(v2 + v3) >> 1)) >> 5;
        break;
    case 4:                                         /* (O1 -> O2) + (O3 -> O4) */
        v2 = opn2_vol(o2, p2 + (uint32_t)(v1 >> 1));
        v3 = opn2_vol(o3, p3);
        r = opn2_vol(o4, p4 + (uint32_t)(v3 >> 1)) >> 5;
        r = OPN2_CL(r + (v2 >> 5));
        break;
    case 5:                                         /* O1 -> each of O2 O3 O4 */
        v2 = opn2_vol(o2, p2 + (uint32_t)(v1 >> 1));
        v3 = opn2_vol(o3, p3 + (uint32_t)(v1 >> 1));
        r = opn2_vol(o4, p4 + (uint32_t)(v1 >> 1)) >> 5;
        r = OPN2_CL(r + (v2 >> 5));
        r = OPN2_CL(r + (v3 >> 5));
        break;
    case 6:                                         /* (O1 -> O2) + O3 + O4 */
        v2 = opn2_vol(o2, p2 + (uint32_t)(v1 >> 1));
        v3 = opn2_vol(o3, p3);
        r = opn2_vol(o4, p4) >> 5;
        r = OPN2_CL(r + (v2 >> 5));
        r = OPN2_CL(r + (v3 >> 5));
        break;
    default:                                        /* O1 + O2 + O3 + O4 */
        v2 = opn2_vol(o2, p2);
        v3 = opn2_vol(o3, p3);
        r = opn2_vol(o4, p4) >> 5;
        r = OPN2_CL(r + (v1 >> 5));
        r = OPN2_CL(r + (v2 >> 5));
        r = OPN2_CL(r + (v3 >> 5));
        break;
    }
    if (h->out_l)
        lr[0] += r;
    if (h->out_r)
        lr[1] += r;
}

/* ----------------------------------------------------------- the chip --- */
static void opn2_fm_write(opn2_t *c, uint32_t index, uint8_t data)   /* fm_engine_base::write + the registers' */
{
    uint32_t ch, opmask, k;
    index &= 0x1FFu;
    c->modified = OPN2_ALL_CHANNELS;
    if (index == 0x27) {                       /* REG_MODE (engine_mode_write; the timers are not modelled) */
        c->reg[index] = data;
        return;
    }
    if ((index & 0xF0u) == 0xA0u) {            /* latched pairs: the upper half waits in 0xB8 / 0xB9 */
        uint32_t latch;
        if (opn2_bf(index, 0, 2) == 3)
            return;
        latch = 0xB8u | opn2_bf(index, 3, 1);
        if (opn2_bf(index, 2, 1))
            c->reg[latch] = data & 0x3Fu;
        else {
            c->reg[index] = data;
            c->reg[index | 4u] = c->reg[latch];
        }
        return;
    }
    if ((index & 0xF8u) == 0xB8u)              /* 0xB8..0xBF: the latches */
        return;
    c->reg[index] = data;
    if (index == 0x28) {
        ch = opn2_bf(data, 0, 2);
        if (ch == 3)
            return;
        ch += opn2_bf(data, 2, 1) * 3u;
        opmask = opn2_bf(data, 4, 4);
        for (k = 0; k < 4u; k++) {
            opn2_op_t *o = OPN2_OP(c, ch, k);
            o->keyon_live = (uint8_t)((o->keyon_live & ~1u) | opn2_bf(opmask, k, 1));
        }
    }
}

static void opn2_reset(opn2_t *c)
{
    uint32_t i, ch, k;
    uint8_t *p = (uint8_t *)c;
    for (i = 0; i < sizeof *c; i++)
        p[i] = 0;
    c->reg[0xB4] = c->reg[0xB5] = c->reg[0xB6] = 0xC0;   /* both outputs on */
    c->reg[0x1B4] = c->reg[0x1B5] = c->reg[0x1B6] = 0xC0;
    for (ch = 0; ch < OPN2_CHANNELS; ch++) {
        c->ch[ch].choffs = (uint16_t)((ch % 3u) + 0x100u * (ch / 3u));
        for (k = 0; k < 4u; k++) {
            opn2_op_t *o = OPN2_OP(c, ch, k);
            i = OPN2_MAP[ch][k];                            /* ymfm's operator number: its register offset */
            o->choffs = c->ch[ch].choffs;
            o->opoffs = (uint16_t)((i % 12u) + (i % 12u) / 3u + 0x100u * (i / 12u));
            o->env_att = 0x3FF;
            o->env_state = OPN2_EG_RELEASE;
            o->quiet = o->idle = 1;
            o->att4 = 0x3FF << 2;
        }
    }
    c->active = c->modified = OPN2_ALL_CHANNELS;
    opn2_fm_write(c, 0x27, 0);
    if (!opn2_wave[1])
        opn2_wave_init();
}

static void opn2_write(opn2_t *c, uint32_t port, uint8_t data)   /* ym2612::write */
{
    switch (port & 3u) {
    case 0:
        c->addr = data;
        break;
    case 1:
        if (opn2_bf(c->addr, 8, 1))
            break;
        if (c->addr == 0x2A)
            c->dac_data = (uint16_t)((c->dac_data & ~0x1FEu) | ((data ^ 0x80u) << 1));
        else if (c->addr == 0x2B)
            c->dac_en = (uint8_t)opn2_bf(data, 7, 1);
        else if (c->addr == 0x2C)
            c->dac_data = (uint16_t)((c->dac_data & ~1u) | opn2_bf(data, 3, 1));
        else
            opn2_fm_write(c, c->addr, data);
        break;
    case 2:
        c->addr = (uint16_t)(0x100u | data);
        break;
    default:
        if (opn2_bf(c->addr, 8, 1))
            opn2_fm_write(c, c->addr, data);
        break;
    }
}

/* (ShinTech, not ymfm) 1: nothing of the chip can be heard until a write: no write waiting for its prepare, the DAC
 * off, every operator idle (released, at full attenuation, no SSG-EG). A caller may then leave it unclocked; only the
 * envelope and LFO counters' phase differ from a chip clocked meanwhile */
static int opn2_silent(const opn2_t *c)
{
    uint32_t i;
    if (c->modified || c->dac_en)
        return 0;
    for (i = 0; i < OPN2_CHANNELS * 4u; i++)
        if (!c->op[i].idle)
            return 0;
    return 1;
}

/* fm_engine_base::clock's prepare: after any write (and every 4096 samples) the channels' and operators' caches,
 * the key states; 1 = it ran */
static int opn2_prepare(opn2_t *c)
{
    uint32_t ch, k;
    if (c->modified == 0 && c->prepare_count++ < 4096u)
        return 0;
    c->active = 0;
    for (ch = 0; ch < OPN2_CHANNELS; ch++) {
        int on = 0;
        opn2_ch_cache(c, &c->ch[ch]);
        for (k = 0; k < 4u; k++) {
            opn2_op_t *o = OPN2_OP(c, ch, k);
            on |= opn2_op_prepare(c, o);
            o->idle = o->env_state == OPN2_EG_RELEASE && o->env_att == 0x3FF && !o->ssg_en;
        }
        if (on)
            c->active |= (uint8_t)(1u << ch);
    }
    c->modified = 0;
    c->prepare_count = 0;
    return 1;
}

/* n samples: fm_engine_base::clock(ALL_CHANNELS) then output(.., 5, 256, 1 << ch) of channels 0..nout-1, each sample;
 * out[s][ch][0] L, [1] R. Channels from nout on are clocked, not heard, and get 0: an output updates its channel's
 * feedback memory, so a channel ymfm does not hear (channel 6 under the DAC) must not be computed either.
 *
 * In blocks of up to OPN2_BLK samples, channel by channel: first the block's prepare (if one is due, before its first
 * sample) and its global steps (the envelope counter, the LFO's PM and AM values: opn2_block_begin), then each channel
 * over the whole block, its four operators clocked and its output made sample by sample (opn2_block_channels). The
 * same samples as ymfm's order: a channel's operators read only their own state and those global values, its output
 * only its operators and its feedback; and a block never holds a write (the caller writes between calls) or a prepare
 * after its first sample (a block ends before the 4096-sample sweep, which can change which channels are active). So
 * any channels can be rendered apart, on another core too, once the block has begun. */
#define OPN2_BLK 16u
static OPN2_AI int32_t opn2_dac_disc(int32_t v) { return v < 0 ? v - 3 : v + 4; }   /* the YM2612's DAC discontinuity */
typedef struct {                       /* a block's global steps (opn2_block_begin), read by every channel */
    uint32_t m, nout, refresh;
    uint32_t envc[OPN2_BLK];
    int32_t pm[OPN2_BLK];
    uint8_t amv[OPN2_BLK], am_mv[OPN2_BLK];
} opn2_blk_t;

static OPN2_AI void opn2_block_begin(opn2_t *c, opn2_blk_t *b, uint32_t m, uint32_t nout)
{
    uint32_t s;
    b->m = m;
    b->nout = nout;
    if (c->modified == 0 && c->prepare_count < 4096u) {   /* (opn2_prepare's own test, without the call) */
        c->prepare_count++;
        b->refresh = 0;
    } else {
        b->refresh = (uint32_t)OPN2_FAR(opn2_prepare)(c);   /* (cold code, in XIP) */
    }
    c->prepare_count += m - 1u;                    /* (the block's other samples: no sweep among them, opn2_render_n) */
    for (s = 0; s < m; s++) {
        if (opn2_bf(++c->env_counter, 0, 2) == 3)   /* EG_CLOCK_DIVIDER 3 */
            c->env_counter += 1;
        b->envc[s] = c->env_counter;
        b->pm[s] = opn2_clock_lfo(c);
        b->amv[s] = c->lfo_am;
        b->am_mv[s] = c->lfo_am != c->lfo_am_prev;
        c->lfo_am_prev = c->lfo_am;
    }
}

/* channels lo..hi-1 over the begun block */
static OPN2_FAST void opn2_block_channels(opn2_t *c, const opn2_blk_t *b, int32_t (*out)[OPN2_CHANNELS][2], uint32_t lo,
                                          uint32_t hi)
{
    uint32_t ch, k, s, m = b->m;
    for (ch = lo; ch < hi; ch++) {
        opn2_ch_t *h = &c->ch[ch];
        opn2_op_t *o0 = OPN2_OP(c, ch, 0);
        uint32_t heard = ch < b->nout && ((c->active >> ch) & 1u);
        for (s = 0; s < m; s++) {
            uint32_t env = b->envc[s], tick = (env & 3u) == 0, am = ((uint32_t)b->amv[s] << 1) >> h->am_shift;
            uint32_t mv = b->am_mv[s], refresh = s == 0 ? b->refresh : 0u;
            int32_t pm = b->pm[s];
            h->fb[0] = h->fb[1];
            h->fb[1] = h->fb_in;
            for (k = 0; k < 4u; k++) {
                opn2_op_t *o = o0 + k;
                uint32_t upd, step;
                if (o->idle)
                    continue;                       /* silent for good: nothing of it is heard before a key on */
                upd = refresh | (mv & o->am_en);
                if (o->ssg_en) {
                    opn2_clock_ssg(o);
                    upd = 1;
                } else if (o->ssg_inv) {
                    o->ssg_inv = 0;
                    upd = 1;
                }
                if (tick) {
                    opn2_clock_envelope(o, env >> 2);
                    upd = 1;
                }
                step = o->phase_step;
                if (step == OPN2_PHASE_DYNAMIC) {
                    if (pm != o->dyn_pm) {          /* (the LFO's PM value moves every few hundred samples) */
                        o->dyn_pm = pm;
                        o->dyn_step = opn2_phase_step(o, h->pm_sens, pm);
                    }
                    step = o->dyn_step;
                }
                o->phase += step;
                if (upd) {
                    opn2_att_update(o, am);
                    o->idle = o->env_state == OPN2_EG_RELEASE && o->env_att == 0x3FF && !o->ssg_en;
                }
            }
            out[s][ch][0] = out[s][ch][1] = 0;
            if (heard)
                opn2_out(c, h, ch, out[s][ch]);
        }
    }
}

/* two cores: the global steps of up to nbmax blocks covering at most n samples, all at once, so their channels can be
 * rendered apart (opn2_channels_n, on either core). Stops before a block that would start with the sweep's prepare
 * (it must come after the blocks before it are rendered): returns the samples covered, *nb the blocks */
static OPN2_FAST uint32_t opn2_begin_n(opn2_t *c, opn2_blk_t *b, uint32_t nbmax, uint32_t n, uint32_t nout, uint32_t *nb)
{
    uint32_t done = 0, k = 0;
    while (done < n && k < nbmax) {
        uint32_t m = n - done < OPN2_BLK ? n - done : OPN2_BLK;
        if (k > 0 && !(c->modified == 0 && c->prepare_count < 4096u))
            break;                                  /* (a prepare is due: the caller's next call) */
        if (c->modified == 0 && c->prepare_count < 4096u && m > 4096u - c->prepare_count)
            m = 4096u - c->prepare_count;
        opn2_block_begin(c, &b[k], m, nout);
        done += m;
        k++;
    }
    *nb = k;
    return done;
}
static OPN2_FAST void opn2_channels_n(opn2_t *c, const opn2_blk_t *b, uint32_t nb, int32_t (*out)[OPN2_CHANNELS][2],
                                      uint32_t lo, uint32_t hi)
{
    uint32_t k;
    for (k = 0; k < nb; k++) {
        opn2_block_channels(c, &b[k], out, lo, hi);
        out += b[k].m;
    }
}
/* ym2612::generate's mix of samples rendered by channel (opn2_render_n / opn2_channels_n): lr[s] from out[s] */
static OPN2_FAST void opn2_mix_n(const opn2_t *c, const int32_t (*out)[OPN2_CHANNELS][2], int32_t (*lr)[2], uint32_t n)
{
    uint32_t s, ch, last = c->dac_en ? 5u : 6u;
    for (s = 0; s < n; s++) {
        int32_t l = 0, r = 0;
        for (ch = 0; ch < last; ch++) {
            l += opn2_dac_disc(out[s][ch][0]);
            r += opn2_dac_disc(out[s][ch][1]);
        }
        if (c->dac_en) {
            int32_t dacval = opn2_dac_disc((int16_t)(c->dac_data << 7) >> 7);
            l += OPN2_RB(c, 0xB4, 7, 1, 0x102) ? dacval : opn2_dac_disc(0);
            r += OPN2_RB(c, 0xB4, 6, 1, 0x102) ? dacval : opn2_dac_disc(0);
        }
        lr[s][0] = (l * 128) * 64 / (6 * 65);
        lr[s][1] = (r * 128) * 64 / (6 * 65);
    }
}

/* n samples, in blocks that end before the 4096-sample prepare sweep */
static OPN2_FAST void opn2_render_n(opn2_t *c, int32_t (*out)[OPN2_CHANNELS][2], uint32_t n, uint32_t nout)
{
    opn2_blk_t b;
    while (n) {
        uint32_t m = n < OPN2_BLK ? n : OPN2_BLK;
        if (c->modified == 0 && c->prepare_count < 4096u && m > 4096u - c->prepare_count)
            m = 4096u - c->prepare_count;          /* (the sweep is due at the next block's first sample) */
        opn2_block_begin(c, &b, m, nout);
        opn2_block_channels(c, &b, out, 0, OPN2_CHANNELS);
        out += m;
        n -= m;
    }
}
static void opn2_render(opn2_t *c, int32_t (*out)[2], uint32_t nout)   /* one sample */
{
    OPN2_FAR(opn2_render_n)(c, (int32_t (*)[OPN2_CHANNELS][2])out, 1, nout);
}


/* ym2612::generate, n samples: lr[s][0] L, lr[s][1] R (in steps of 8: the channels' outputs on the stack) */
static OPN2_FAST void opn2_generate_n(opn2_t *c, int32_t (*lr)[2], uint32_t n)
{
    int32_t t[8][OPN2_CHANNELS][2];
    uint32_t s, k, ch, m, last;
    for (s = 0; s < n; s += m) {
        m = n - s < 8u ? n - s : 8u;
        last = c->dac_en ? 5u : 6u;                 /* (a DAC write comes between calls, never inside one) */
        opn2_render_n(c, t, m, last);
        for (k = 0; k < m; k++) {
            int32_t l = 0, r = 0;
            for (ch = 0; ch < last; ch++) {
                l += opn2_dac_disc(t[k][ch][0]);
                r += opn2_dac_disc(t[k][ch][1]);
            }
            if (c->dac_en) {
                int32_t dacval = opn2_dac_disc((int16_t)(c->dac_data << 7) >> 7);
                l += OPN2_RB(c, 0xB4, 7, 1, 0x102) ? dacval : opn2_dac_disc(0);
                r += OPN2_RB(c, 0xB4, 6, 1, 0x102) ? dacval : opn2_dac_disc(0);
            }
            lr[s + k][0] = (l * 128) * 64 / (6 * 65);
            lr[s + k][1] = (r * 128) * 64 / (6 * 65);
        }
    }
}
static void opn2_generate(opn2_t *c, int32_t *lr)   /* one sample */
{
    OPN2_FAR(opn2_generate_n)(c, (int32_t (*)[2])lr, 1);
}
#endif /* OPN2_C_INCLUDED */
