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
 *   opn2_clock(c)                   one sample at the native rate (clock / 144: 53,267 Hz at 7.67 MHz)
 *   opn2_ch_output(c, ch, lr)       channel ch's 9-bit output of this sample, L and R (0 when muted or silent)
 *   opn2_generate(c, lr)            ymfm's ym2612::generate: the six channels with the DAC discontinuity, x 128 / 6
 *
 * Every function is static: Felucca builds as one translation unit (felucca.c); the host tests include this file. */
#include <stdint.h>

#define OPN2_CHANNELS 6u
#define OPN2_OPERATORS 24u
#define OPN2_ALL_CHANNELS 0x3Fu
#define OPN2_EG_QUIET 0x380u
#define OPN2_PHASE_DYNAMIC 1u     /* opdata_cache::PHASE_STEP_DYNAMIC: recompute each sample (PM LFO) */
enum { OPN2_EG_ATTACK = 1, OPN2_EG_DECAY = 2, OPN2_EG_SUSTAIN = 3, OPN2_EG_RELEASE = 4, OPN2_EG_STATES = 6 };

typedef struct {
    uint32_t phase;               /* 10.10 */
    uint16_t env_att;             /* 4.6 attenuation */
    uint8_t env_state, ssg_inv, key_state, keyon_live;
    uint16_t opoffs, choffs;      /* register offsets */
    /* opdata_cache */
    uint32_t phase_step, block_freq, multiple, total_level, eg_sustain;
    int32_t detune;
    uint8_t eg_rate[OPN2_EG_STATES];
} opn2_op_t;

typedef struct {
    int16_t fb[2], fb_in;         /* operator 1's feedback memory; fb_in: set by the output */
    uint16_t choffs;
} opn2_ch_t;

typedef struct {
    uint8_t reg[0x200];
    opn2_op_t op[OPN2_OPERATORS];
    opn2_ch_t ch[OPN2_CHANNELS];
    uint32_t env_counter;         /* low 2 bits: sub-counter */
    uint32_t lfo_counter;
    uint32_t prepare_count;
    uint8_t lfo_am;
    uint8_t active, modified;     /* channel masks */
    uint8_t dac_en;
    uint16_t addr, dac_data;      /* address register (bit 8: the upper port), 9-bit DAC */
} opn2_t;

/* operators of each channel, in ymfm's order: carrier 1, carrier 2, modulator 1, modulator 2 as the register map puts
 * them, i.e. OP1, OP3, OP2, OP4 by slot (opn_registers_base<true>::operator_map) */
static const uint8_t OPN2_MAP[OPN2_CHANNELS][4] = {
    {0, 6, 3, 9}, {1, 7, 4, 10}, {2, 8, 5, 11}, {12, 18, 15, 21}, {13, 19, 16, 22}, {14, 20, 17, 23}};

static inline uint32_t opn2_bf(uint32_t v, uint32_t start, uint32_t len) { return (v >> start) & ((1u << len) - 1u); }
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

/* fm_channel::output_4op s_algorithm_ops[0..7] */
#define OPN2_ALG(op2in, op3in, op4in, op1out, op2out, op3out) \
    ((op2in) | ((op3in) << 1) | ((op4in) << 4) | ((op1out) << 7) | ((op2out) << 8) | ((op3out) << 9))
static const uint16_t OPN2_ALGS[8] = {
    OPN2_ALG(1, 2, 3, 0, 0, 0), OPN2_ALG(0, 5, 3, 0, 0, 0), OPN2_ALG(0, 2, 6, 0, 0, 0), OPN2_ALG(1, 0, 7, 0, 0, 0),
    OPN2_ALG(1, 0, 3, 0, 1, 0), OPN2_ALG(1, 1, 1, 0, 1, 1), OPN2_ALG(1, 0, 0, 0, 1, 1), OPN2_ALG(0, 0, 0, 1, 1, 1)};
#undef OPN2_ALG

static const uint8_t OPN2_LFO_MAX[8] = {109, 78, 72, 68, 63, 45, 9, 6};

/* ------------------------------------------------------------- helpers --- */
static inline uint32_t opn2_waveform(uint32_t index)   /* m_waveform[0][index]: |sin| attenuation, bit 15 the sign */
{
    uint32_t in = index;
    if (opn2_bf(in, 8, 1))
        in = ~in;
    return OPN2_SIN[in & 0xFFu] | (opn2_bf(index, 9, 1) << 15);
}
static inline uint32_t opn2_att_to_vol(uint32_t input) { return OPN2_POW[input & 0xFFu] >> (input >> 8); }
static inline uint32_t opn2_eff_rate(uint32_t raw, uint32_t ksr) { return raw == 0 ? 0 : (raw + ksr < 63u ? raw + ksr : 63u); }
static inline int32_t opn2_clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

static int32_t opn2_detune(uint32_t detune, uint32_t keycode)
{
    int32_t r = OPN2_DT[keycode][detune & 3u];
    return opn2_bf(detune, 2, 1) ? -r : r;
}

static int32_t opn2_pm_adjust(uint32_t fnum_bits, uint32_t pm_sens, int32_t lfo_raw_pm)
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

static uint32_t opn2_phase_step(const opn2_t *c, const opn2_op_t *o, int32_t lfo_raw_pm)   /* compute_phase_step */
{
    uint32_t fnum = opn2_bf(o->block_freq, 0, 11) << 1, pm_sens = OPN2_RB(c, 0xB4, 0, 3, o->choffs), block, step;
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
        o->phase_step = opn2_phase_step(c, o, 0);
    else
        o->phase_step = OPN2_PHASE_DYNAMIC;
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

static int32_t opn2_clock_lfo(opn2_t *c)   /* clock_noise_and_lfo: the raw PM value */
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

static inline uint32_t opn2_lfo_am_offset(const opn2_t *c, uint32_t choffs)
{
    uint32_t am_shift = (1u << (OPN2_RB(c, 0xB4, 4, 2, choffs) ^ 3u)) - 1u;
    return ((uint32_t)c->lfo_am << 1) >> am_shift;
}

/* ------------------------------------------------------- the operator --- */
static void opn2_start_attack(const opn2_t *c, opn2_op_t *o, int is_restart)
{
    if (o->env_state == OPN2_EG_ATTACK)
        return;
    o->env_state = OPN2_EG_ATTACK;
    if (!is_restart)
        o->ssg_inv = (uint8_t)(OPN2_RB(c, 0x90, 3, 1, o->opoffs) & opn2_bf(OPN2_RB(c, 0x90, 0, 3, o->opoffs), 2, 1));
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

static void opn2_clock_keystate(const opn2_t *c, opn2_op_t *o, uint32_t keystate)
{
    if ((keystate ^ o->key_state) != 0) {
        o->key_state = (uint8_t)keystate;
        if (keystate)
            opn2_start_attack(c, o, 0);
        else
            opn2_start_release(o);
    }
}

static int opn2_op_prepare(const opn2_t *c, opn2_op_t *o)
{
    opn2_cache(c, o);
    opn2_clock_keystate(c, o, o->keyon_live != 0);
    o->keyon_live &= (uint8_t)~(1u << 2);     /* KEYON_CSM */
    return o->env_state != OPN2_EG_RELEASE || o->env_att < OPN2_EG_QUIET;
}

static void opn2_clock_ssg(const opn2_t *c, opn2_op_t *o)   /* clock_ssg_eg_state (SSG-EG on) */
{
    uint32_t mode;
    if (!opn2_bf(o->env_att, 9, 1))
        return;
    mode = OPN2_RB(c, 0x90, 0, 3, o->opoffs);
    if (opn2_bf(mode, 0, 1)) {                 /* hold modes 1 3 5 7 */
        o->ssg_inv = (uint8_t)(opn2_bf(mode, 2, 1) ^ opn2_bf(mode, 1, 1));
        if (o->env_state != OPN2_EG_ATTACK)
            o->env_att = o->ssg_inv ? 0x200 : 0x3FF;
    } else {                                   /* continuous modes 0 2 4 6 */
        o->ssg_inv ^= (uint8_t)opn2_bf(mode, 1, 1);
        if (o->env_state == OPN2_EG_DECAY || o->env_state == OPN2_EG_SUSTAIN)
            opn2_start_attack(c, o, 1);
        if (opn2_bf(mode, 1, 1) == 0)
            o->phase = 0;
    }
    if (o->env_state == OPN2_EG_RELEASE)
        o->env_att = 0x3FF;
}

static void opn2_clock_envelope(const opn2_t *c, opn2_op_t *o, uint32_t env_counter)
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
        if (!OPN2_RB(c, 0x90, 3, 1, o->opoffs))
            o->env_att = (uint16_t)(o->env_att + inc);
        else if (o->env_att < 0x200)
            o->env_att = (uint16_t)(o->env_att + 4 * inc);
        if (o->env_att >= 0x400)
            o->env_att = 0x3FF;
    }
}

static inline void opn2_op_clock(const opn2_t *c, opn2_op_t *o, uint32_t env_counter, int32_t lfo_raw_pm)
{
    uint32_t step;
    if (OPN2_RB(c, 0x90, 3, 1, o->opoffs))
        opn2_clock_ssg(c, o);
    else
        o->ssg_inv = 0;
    if (opn2_bf(env_counter, 0, 2) == 0)
        opn2_clock_envelope(c, o, env_counter >> 2);
    step = o->phase_step;
    if (step == OPN2_PHASE_DYNAMIC)
        step = opn2_phase_step(c, o, lfo_raw_pm);
    o->phase += step;
}

static inline uint32_t opn2_env_att(const opn2_t *c, const opn2_op_t *o, uint32_t am_offset)   /* envelope_attenuation */
{
    uint32_t r = o->env_att;
    if (o->ssg_inv)
        r = (0x200u - r) & 0x3FFu;
    if (OPN2_RB(c, 0x60, 7, 1, o->opoffs))
        r += am_offset;
    r += o->total_level;
    return r < 0x3FFu ? r : 0x3FFu;
}

static inline int32_t opn2_volume(const opn2_t *c, const opn2_op_t *o, uint32_t phase, uint32_t am_offset)   /* compute_volume */
{
    uint32_t sin_att, env_att;
    int32_t r;
    if (o->env_att > OPN2_EG_QUIET)
        return 0;
    sin_att = opn2_waveform(phase & 0x3FFu);
    env_att = opn2_env_att(c, o, am_offset) << 2;
    r = (int32_t)opn2_att_to_vol((sin_att & 0x7FFFu) + env_att);
    return opn2_bf(sin_att, 15, 1) ? -r : r;
}

/* --------------------------------------------------------- the channel --- */
/* fm_channel::output_4op with ymfm's add_to_output: channel ch's value into lr[0] (L) and lr[1] (R) */
static void opn2_output_4op(opn2_t *c, uint32_t ch, int32_t *lr, uint32_t rshift, int32_t clipmax)
{
    opn2_ch_t *h = &c->ch[ch];
    const opn2_op_t *o1 = &c->op[OPN2_MAP[ch][0]], *o2 = &c->op[OPN2_MAP[ch][1]];
    const opn2_op_t *o3 = &c->op[OPN2_MAP[ch][2]], *o4 = &c->op[OPN2_MAP[ch][3]];
    uint32_t choffs = h->choffs, am = opn2_lfo_am_offset(c, choffs), fb = OPN2_RB(c, 0xB0, 3, 3, choffs), ops;
    int32_t opmod = 0, op1, result, clipmin = -clipmax - 1;
    int16_t opout[8];
    if (fb != 0)
        opmod = (h->fb[0] + h->fb[1]) >> (10 - fb);
    op1 = h->fb_in = (int16_t)opn2_volume(c, o1, (o1->phase >> 10) + (uint32_t)opmod, am);
    if (OPN2_RB(c, 0xB4, 6, 2, choffs) == 0)
        return;
    ops = OPN2_ALGS[OPN2_RB(c, 0xB0, 0, 3, choffs)];
    opout[0] = 0;
    opout[1] = (int16_t)op1;
    opmod = opout[opn2_bf(ops, 0, 1)] >> 1;
    opout[2] = (int16_t)opn2_volume(c, o2, (o2->phase >> 10) + (uint32_t)opmod, am);
    opout[5] = (int16_t)(opout[1] + opout[2]);
    opmod = opout[opn2_bf(ops, 1, 3)] >> 1;
    opout[3] = (int16_t)opn2_volume(c, o3, (o3->phase >> 10) + (uint32_t)opmod, am);
    opout[6] = (int16_t)(opout[1] + opout[3]);
    opout[7] = (int16_t)(opout[2] + opout[3]);
    opmod = opout[opn2_bf(ops, 4, 3)] >> 1;
    result = opn2_volume(c, o4, (o4->phase >> 10) + (uint32_t)opmod, am);
    result >>= rshift;
    if (opn2_bf(ops, 7, 1))
        result = opn2_clampi(result + (opout[1] >> rshift), clipmin, clipmax);
    if (opn2_bf(ops, 8, 1))
        result = opn2_clampi(result + (opout[2] >> rshift), clipmin, clipmax);
    if (opn2_bf(ops, 9, 1))
        result = opn2_clampi(result + (opout[3] >> rshift), clipmin, clipmax);
    if (OPN2_RB(c, 0xB4, 7, 1, choffs))
        lr[0] += result;
    if (OPN2_RB(c, 0xB4, 6, 1, choffs))
        lr[1] += result;
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
            opn2_op_t *o = &c->op[OPN2_MAP[ch][k]];
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
        for (k = 0; k < 4u; k++)
            c->op[OPN2_MAP[ch][k]].choffs = c->ch[ch].choffs;
    }
    for (i = 0; i < OPN2_OPERATORS; i++) {
        c->op[i].opoffs = (uint16_t)((i % 12u) + (i % 12u) / 3u + 0x100u * (i / 12u));
        c->op[i].env_att = 0x3FF;
        c->op[i].env_state = OPN2_EG_RELEASE;
    }
    c->active = c->modified = OPN2_ALL_CHANNELS;
    opn2_fm_write(c, 0x27, 0);
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

static void opn2_clock(opn2_t *c)   /* fm_engine_base::clock(ALL_CHANNELS) */
{
    uint32_t ch, k;
    int32_t pm;
    if (c->modified != 0 || c->prepare_count++ >= 4096u) {
        c->active = 0;
        for (ch = 0; ch < OPN2_CHANNELS; ch++) {
            int on = 0;
            for (k = 0; k < 4u; k++)
                on |= opn2_op_prepare(c, &c->op[OPN2_MAP[ch][k]]);
            if (on)
                c->active |= (uint8_t)(1u << ch);
        }
        c->modified = 0;
        c->prepare_count = 0;
    }
    if (opn2_bf(++c->env_counter, 0, 2) == 3)   /* EG_CLOCK_DIVIDER 3 */
        c->env_counter += 1;
    pm = opn2_clock_lfo(c);
    for (ch = 0; ch < OPN2_CHANNELS; ch++) {
        opn2_ch_t *h = &c->ch[ch];
        h->fb[0] = h->fb[1];
        h->fb[1] = h->fb_in;
        for (k = 0; k < 4u; k++)
            opn2_op_clock(c, &c->op[OPN2_MAP[ch][k]], c->env_counter, pm);
    }
}

/* channel ch's 9-bit output this sample (fm_engine_base::output(.., 5, 256, 1 << ch)): lr[0] L, lr[1] R */
static void opn2_ch_output(opn2_t *c, uint32_t ch, int32_t *lr)
{
    lr[0] = lr[1] = 0;
    if ((c->active >> ch) & 1u)
        opn2_output_4op(c, ch, lr, 5, 256);
}

static inline int32_t opn2_dac_disc(int32_t v) { return v < 0 ? v - 3 : v + 4; }   /* the YM2612's DAC discontinuity */

static void opn2_generate(opn2_t *c, int32_t *lr)   /* ym2612::generate, one sample */
{
    uint32_t ch, last;
    int32_t t[2];
    opn2_clock(c);
    lr[0] = lr[1] = 0;
    last = c->dac_en ? 5u : 6u;
    for (ch = 0; ch < last; ch++) {
        opn2_ch_output(c, ch, t);
        lr[0] += opn2_dac_disc(t[0]);
        lr[1] += opn2_dac_disc(t[1]);
    }
    if (c->dac_en) {
        int32_t dacval = opn2_dac_disc((int16_t)(c->dac_data << 7) >> 7);
        lr[0] += OPN2_RB(c, 0xB4, 7, 1, 0x102) ? dacval : opn2_dac_disc(0);
        lr[1] += OPN2_RB(c, 0xB4, 6, 1, 0x102) ? dacval : opn2_dac_disc(0);
    }
    lr[0] = (lr[0] * 128) * 64 / (6 * 65);
    lr[1] = (lr[1] * 128) * 64 / (6 * 65);
}
