/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 William Kew, ShinTech Electronics */
/* The host tests' view of the OPN2 cores: firmware/src/chips/opn2.c (the ymfm port, all static, as the firmware
 * includes it) and Nuked-OPN2 (chips/ym3438.c), behind plain functions a C++ test or a benchmark can call. */
#include "../firmware/src/chips/opn2.c"
#include "../firmware/src/chips/ym3438.c"

static opn2_t host_opn2;
void h_opn2_reset(void) { opn2_reset(&host_opn2); }
void h_opn2_write(unsigned port, unsigned data) { opn2_write(&host_opn2, port, (uint8_t)data); }
void h_opn2_generate_n(int *lr, unsigned n)   /* n samples, L R interleaved (n <= 64) */
{
    int32_t o[64][2];
    unsigned i;
    opn2_generate_n(&host_opn2, o, n);
    for (i = 0; i < n; i++) {
        lr[2 * i] = o[i][0];
        lr[2 * i + 1] = o[i][1];
    }
}
void h_opn2_generate_split(int *lr, unsigned n)   /* as h_opn2_generate_n, by channel: 4..6 first, then 1..3 */
{
    opn2_blk_t b[4];
    int32_t out[64][OPN2_CHANNELS][2], o[64][2];
    unsigned i, done = 0, nb;
    while (done < n) {
        unsigned m = opn2_begin_n(&host_opn2, b, 4, n - done, host_opn2.dac_en ? 5u : 6u, &nb);
        opn2_channels_n(&host_opn2, b, nb, out, 3, 6);
        opn2_channels_n(&host_opn2, b, nb, out, 0, 3);
        opn2_mix_n(&host_opn2, (const int32_t (*)[OPN2_CHANNELS][2])out, o, m);
        for (i = 0; i < m; i++) {
            lr[2 * (done + i)] = o[i][0];
            lr[2 * (done + i) + 1] = o[i][1];
        }
        done += m;
    }
}
void h_opn2_generate(int *lr)
{
    int32_t o[2];
    opn2_generate(&host_opn2, o);
    lr[0] = o[0];
    lr[1] = o[1];
}

static ym3438_t host_nuked;
void h_nuked_reset(void)
{
    OPN2_SetChipType(ym3438_mode_ym2612);
    OPN2_Reset(&host_nuked);
}
void h_nuked_write(unsigned port, unsigned data) { OPN2_Write(&host_nuked, port, (Bit8u)data); }
void h_nuked_sample(int *lr)   /* one native sample: 24 internal cycles, their multiplexed outputs summed */
{
    Bit16s b[2];
    int k, l = 0, r = 0;
    for (k = 0; k < 24; k++) {
        OPN2_Clock(&host_nuked, b);
        l += b[0];
        r += b[1];
    }
    lr[0] = l;
    lr[1] = r;
}
