// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 William Kew, ShinTech Electronics
// The OPN2 port (firmware/src/chips/opn2.c) against ymfm itself, sample for sample. Needs a ymfm checkout:
//   YMFM=~/src/ymfm tests/run_shintech.sh       (without YMFM the test is skipped)
// Random register traffic on both ports (patches, key on / off, LFO, SSG-EG, channel 3's special mode, the DAC,
// stray addresses), several seeds; every sample of ym2612::generate must match opn2_generate exactly, also when the
// port renders blocks of 1..40 samples between the writes (opn2_generate_n, as the engine does), and when it renders
// them by channel, channels 4..6 before 1..3 (opn2_begin_n / opn2_channels_n, as two cores do).
#include <cstdio>
#include <cstdint>
#include "ymfm_opn.h"

extern "C" {
void h_opn2_reset(void);
void h_opn2_write(unsigned port, unsigned data);
void h_opn2_generate(int *lr);
void h_opn2_generate_n(int *lr, unsigned n);
void h_opn2_generate_split(int *lr, unsigned n);
}

namespace {
struct host_intf : ymfm::ymfm_interface {};

uint32_t rng_state;
uint32_t rnd() { rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5; return rng_state; }

// one register write to both chips: port pair 0/1 or 2/3
void both(ymfm::ym2612 &ref, unsigned hi, unsigned reg, unsigned data)
{
    ref.write(hi * 2, reg);
    ref.write(hi * 2 + 1, data);
    h_opn2_write(hi * 2, reg);
    h_opn2_write(hi * 2 + 1, data);
}

// a random but plausible write: mostly the FM registers of real channels, sometimes anything
void random_write(ymfm::ym2612 &ref)
{
    unsigned r = rnd() % 100, hi = rnd() & 1, reg, data = rnd() & 0xFF;
    if (r < 55) {                                   // operator registers 0x30..0x9F
        reg = 0x30 + (rnd() % 0x70);
    } else if (r < 70) {                            // frequency: A4/A0 pairs (also channel 3's A8..AE)
        unsigned ch = rnd() % 3, upper = 0xA4 + ch, lower = 0xA0 + ch;
        if (rnd() % 4 == 0)
            upper = 0xAC + ch, lower = 0xA8 + ch;
        both(ref, hi, upper, rnd() & 0x3F);
        reg = lower;
    } else if (r < 78) {                            // B0 (alg / fb), B4 (L R AMS FMS)
        reg = (rnd() & 1 ? 0xB0 : 0xB4) + rnd() % 3;
    } else if (r < 88) {                            // key on / off
        reg = 0x28;
        hi = 0;
        data = (rnd() & 0xF0) | (rnd() % 7);
    } else if (r < 92) {                            // LFO
        reg = 0x22;
        hi = 0;
    } else if (r < 95) {                            // mode: channel 3 special mode (timers not modelled by either)
        reg = 0x27;
        hi = 0;
        data &= 0xC0;
        if ((data & 0xC0) == 0x80)
            data = 0x40;                            // CSM needs timers: never
    } else if (r < 98) {                            // the DAC
        reg = 0x2A + rnd() % 3;
        hi = 0;
    } else {
        reg = rnd() & 0xFF;                         // anything
        if (reg == 0x27 || (reg >= 0x24 && reg <= 0x27))
            reg = 0x21;
    }
    both(ref, hi, reg, data);
}
}

int main()
{
    static const uint32_t SEEDS[] = {1, 2, 3, 0x1234567u, 0xDEADBEEFu, 0x600DF00Du, 77, 4242};
    const uint32_t NSAMP = 400000;                  // per seed, ~7.5 s at 53,267 Hz
    int fails = 0;
    for (uint32_t seed : SEEDS) {
        host_intf intf;
        ymfm::ym2612 ref(intf);
        ref.reset();
        h_opn2_reset();
        rng_state = seed;
        uint64_t loud = 0;
        int peak = 0;
        const int SILENT = (6 * 4 * 128) * 64 / (6 * 65);   // six silent channels: the DAC step's offset
        uint32_t bad = 0, first_bad = 0;
        for (uint32_t s = 0; s < NSAMP; s++) {
            unsigned nw = rnd() % 64 == 0 ? 1 + rnd() % 40 : (rnd() % 8 == 0);   // bursts and single writes
            for (unsigned w = 0; w < nw; w++)
                random_write(ref);
            unsigned m = rnd() % 4 == 0 ? 1 + rnd() % 40 : 1, k;   // a block, or one sample
            if (s + m > NSAMP)
                m = NSAMP - s;
            int lr[2 * 40];
            if (m == 1)
                h_opn2_generate(lr);
            else if (rnd() & 1)
                h_opn2_generate_n(lr, m);
            else
                h_opn2_generate_split(lr, m);      // by channel, as two cores render it
            for (k = 0; k < m; k++) {
                ymfm::ym2612::output_data out;
                ref.generate(&out, 1);
                if (out.data[0] != lr[2 * k] || out.data[1] != lr[2 * k + 1]) {
                    if (!bad++)
                        first_bad = s + k;
                }
                int dev = out.data[0] - SILENT < 0 ? SILENT - out.data[0] : out.data[0] - SILENT;
                peak = dev > peak ? dev : peak;
                loud += dev > 256;
            }
            s += m - 1;
        }
        printf("opn2 vs ymfm: seed %08X  %u samples, %u differ (first at %u), %.0f %% sounding, peak %d  %s\n", seed, NSAMP,
               bad, bad ? first_bad : 0u, 100.0 * loud / NSAMP, peak, bad ? "FAIL" : "ok");
        fails += bad != 0 || loud < NSAMP / 10;   // a test of two silent chips proves nothing
    }
    printf(fails ? "opn2 vs ymfm: FAILED\n" : "opn2 vs ymfm: all ok (bit-exact)\n");
    return fails != 0;
}
