/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 William Kew, ShinTech Electronics */
/* The host tests' view of the OPN2 cores: firmware/src/chips/opn2.c (the ymfm port, all static, as the firmware
 * includes it) and Nuked-OPN2 (chips/ym3438.c), behind plain functions a C++ test or a benchmark can call. */
#include "../firmware/src/chips/opn2.c"
#include "../firmware/src/chips/ym3438.c"

static opn2_t host_opn2;
void h_opn2_reset(void) { opn2_reset(&host_opn2); }
void h_opn2_write(unsigned port, unsigned data) { opn2_write(&host_opn2, port, (uint8_t)data); }
void h_opn2_generate(int *lr)
{
    int32_t o[2];
    opn2_generate(&host_opn2, o);
    lr[0] = o[0];
    lr[1] = o[1];
}
void h_opn2_ch(unsigned ch, int *lr)   /* the last clocked sample's channel output (call after h_opn2_generate) */
{
    int32_t o[2];
    opn2_ch_output(&host_opn2, ch, o);
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
