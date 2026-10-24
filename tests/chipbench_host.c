/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 William Kew, ShinTech Electronics */
/* Host benchmark of the two OPN2 cores (Phase 1): Nuked-OPN2 (chips/ym3438.c) and the ymfm port (chips/opn2.c), on
 * the same load the device benchmark plays (firmware/src/chipbench_script.h): six channels keyed, SSG-EG on every
 * operator, the LFO with AMS and FMS on every channel; then the same with the DAC on (channel 6 plays samples written
 * every sample). Prints the time per native sample and the share of one host core that real time (53,267 Hz) takes.
 * A host figure: only the ratio between the cores carries over to the FM-1 (the device benchmark measures that).
 *   cc -O2 -Ifirmware/src tests/chipbench_host.c -o build/chipbench && build/chipbench */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <time.h>
#include "opn2_host.c"
#include "../firmware/src/chipbench_script.h"

#define NATIVE_HZ 53267.0

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static volatile int sink;

/* a core behind the host functions */
typedef struct {
    const char *name;
    void (*reset)(void);
    void (*write)(unsigned, unsigned);
    void (*sample)(int *);
} core_t;

static void opn2_sample(int *lr) { h_opn2_generate(lr); }

static void play_script(const core_t *c, int dac)
{
    static cb_write_t w[CB_SCRIPT_MAX];
    uint32_t i, n = cb_script_build(w, 0);
    int lr[2];
    c->reset();
    for (i = 0; i < n; i++) {
        c->write(w[i].port, w[i].reg);
        c->write(w[i].port + 1u, w[i].data);
        c->sample(lr);                          /* one sample between writes: Nuked commits a write in its slot */
        c->sample(lr);
    }
    c->write(0, 0x2B);
    c->write(1, dac ? 0x80 : 0x00);
}

static double run(const core_t *c, int dac, double seconds)
{
    uint32_t n = (uint32_t)(seconds * NATIVE_HZ), s;
    int lr[2], acc = 0;
    double t0;
    play_script(c, dac);
    t0 = now();
    for (s = 0; s < n; s++) {
        if (dac) {                              /* the DAC: a sample written every native sample, as a driver does */
            c->write(0, 0x2A);
            c->write(1, (s * 7u) & 0xFFu);
        }
        c->sample(lr);
        acc += lr[0] ^ lr[1];
    }
    sink = acc;
    return (now() - t0) / n;                    /* seconds per native sample */
}

int main(void)
{
    static const core_t CORES[] = {
        {"Nuked-OPN2", h_nuked_reset, h_nuked_write, h_nuked_sample},
        {"ymfm port ", h_opn2_reset, h_opn2_write, opn2_sample},
    };
    uint32_t k;
    int dac;
    double t[2][2];
    for (dac = 0; dac < 2; dac++)
        for (k = 0; k < 2; k++) {
            run(&CORES[k], dac, 0.5);           /* warm the caches */
            t[k][dac] = run(&CORES[k], dac, 5.0);
            printf("chipbench: %s  %s  %7.1f ns / sample   %5.2f %% of a host core in real time\n", CORES[k].name,
                   dac ? "6 FM + SSG-EG + LFO, DAC on " : "6 FM + SSG-EG + LFO, DAC off", t[k][dac] * 1e9,
                   t[k][dac] * NATIVE_HZ * 100.0);
        }
    printf("chipbench: Nuked / ymfm port = %.1f x (DAC off), %.1f x (DAC on)\n", t[0][0] / t[1][0], t[0][1] / t[1][1]);
    return 0;
}
