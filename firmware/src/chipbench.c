/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 William Kew, ShinTech Electronics */
/* ShinTech Phase 1: the OPN2 core benchmark (FELUCCA_CHIPBENCH=1 builds it in; off in every other build). It runs one
 * of the two candidate YM2612 cores in the audio ISR, after the parts and before the FX buses (fx.c mix_block), on
 * the load of chipbench_script.h (six channels, SSG-EG, AM / PM LFO), at the chip's native rate (53,267 Hz: 38.64
 * native samples per 32-sample block), and times it with TIMER4 (24 MHz).
 *
 * Safety: it is OFF at every boot and starts only from the USB serial console. DUTY n renders the chip in n of every
 * 8 blocks; its cost is measured per native sample, so the projection to full time holds at any duty. If the ISR's
 * load (song.cpu_q8, the INFO screen's CPU) goes over 90 % for 4 halves in a row, it switches itself off and says so
 * (TRIP): a core that does not fit never starves the main loop long enough for the watchdog.
 *
 * Console (115200-style CDC, any terminal):
 *   bench                     state and the last second's figures
 *   bench nuked | ymfm | off  the core
 *   bench duty N              1..8 (eighths of the blocks rendered; 2 at start)
 *   bench dac 0|1             channel 6 as the DAC, a sample written every native sample
 *   bench fx 0|1              the chip's output into the chorus, delay and reverb sends (the buses always run)
 *   bench vol N               0..15, its level in the mix (0 silent; 6 at start)
 * Figures: est_permille = the core alone at full duty, in 1/1000 of the CPU (from its ticks per native sample);
 * cpu_pct = the whole ISR as measured now (what MENU > SYSTEM > INFO shows). */
#if FELUCCA_CHIPBENCH
#include "chips/opn2.c"
#include "chips/ym3438.c"
#include "chipbench_script.h"

enum { CB_OFF, CB_NUKED, CB_YMFM };
#define CB_STEP_Q16 2532050u            /* native samples per 32-sample block, Q16: 7670453 / 144 / (24e6 / 544) x 32 */
#define CB_WINDOW 1378u                 /* blocks per figure: ~1 s */
#define CB_TICKS_PER_NATIVE_X10 4506u   /* 24e6 / 53267 x 10: a native sample's whole time in TIMER4 ticks */

static ym3438_t cb_nuked __attribute__((section(".pool")));
static opn2_t cb_opn2 __attribute__((section(".pool")));
static cb_write_t cb_script[CB_SCRIPT_MAX] __attribute__((section(".pool")));
static struct {
    volatile uint8_t mode, req;         /* req: the console's mode, taken at the next block (0xFF none) */
    uint8_t duty, dac, fx, vol, tripped, over;
    uint32_t pos_q16, blk, dacph, nscript, sidx;
    int32_t last_l, last_r;
    uint32_t w_ticks, w_samples, w_blocks;   /* this window */
    volatile uint32_t est_permille, samples, peak;   /* the last window's */
} cb = {CB_OFF, 0xFF, 2, 0, 0, 6};

/* the core's reset and the script, at the native timing (Nuked commits a write in its operator's slot: two samples
 * between writes, as the host benchmark) */
static void cb_nuked_sample(int32_t *l, int32_t *r)
{
    Bit16s b[2];
    int32_t sl = 0, sr = 0;
    uint32_t k;
    for (k = 0; k < 24u; k++) {
        OPN2_Clock(&cb_nuked, b);
        sl += b[0];
        sr += b[1];
    }
    *l = sl << 4;                       /* ~ the port's scale */
    *r = sr << 4;
}
static void cb_ymfm_sample(int32_t *l, int32_t *r)
{
    int32_t o[2];
    opn2_generate(&cb_opn2, o);
    *l = o[0];
    *r = o[1];
}
static void cb_write(uint32_t port, uint32_t data)
{
    if (cb.mode == CB_NUKED)
        OPN2_Write(&cb_nuked, port, (Bit8u)data);
    else
        opn2_write(&cb_opn2, port, (uint8_t)data);
}
static void cb_sample(int32_t *l, int32_t *r)
{
    if (cb.mode == CB_NUKED)
        cb_nuked_sample(l, r);
    else
        cb_ymfm_sample(l, r);
}
static void cb_start(void)              /* (in the ISR, at a block start: the mode just changed) */
{
    cb.nscript = cb_script_build(cb_script);
    cb.sidx = 0;
    if (cb.mode == CB_NUKED) {
        OPN2_SetChipType(ym3438_mode_ym2612);
        OPN2_Reset(&cb_nuked);
    } else {
        opn2_reset(&cb_opn2);
    }
    cb.pos_q16 = cb.w_ticks = cb.w_samples = cb.w_blocks = 0;
    cb.over = 0;
}

/* the script, 8 writes a block (each followed by two samples), then the DAC's enable: 1 = still starting */
static int cb_starting(void)
{
    uint32_t k;
    int32_t l, r;
    if (cb.sidx > cb.nscript)
        return 0;
    for (k = 0; k < 8u && cb.sidx < cb.nscript; k++, cb.sidx++) {
        cb_write(cb_script[cb.sidx].port, cb_script[cb.sidx].reg);
        cb_write(cb_script[cb.sidx].port + 1u, cb_script[cb.sidx].data);
        cb_sample(&l, &r);
        cb_sample(&l, &r);
    }
    if (cb.sidx == cb.nscript) {
        cb_write(0, 0x2B);
        cb_write(1, cb.dac ? 0x80 : 0x00);
        cb.sidx++;
    }
    return 1;
}

/* once a block (fx.c mix_block, after the parts): the chip into the dry mix (and the sends with FX) */
static void cb_block(int32_t *mix_l, int32_t *mix_r, int32_t *snd_c, int32_t *snd_d, int32_t *snd_r, uint32_t n)
{
    uint32_t i, ns, s, t0, k;
    int32_t l, r;
    if (cb.req != 0xFF) {
        cb.mode = cb.req;
        cb.req = 0xFF;
        if (cb.mode != CB_OFF)
            cb_start();
    }
    if (cb.mode == CB_OFF || cb_starting())
        return;
    /* the trip: the ISR over 90 % (230 / 256) for 4 halves (about 4 x 128 frames) */
    if (song.cpu_q8 > 230u) {
        if (++cb.over >= 16u) {         /* 16 blocks = 4 halves */
            cb.mode = CB_OFF;
            cb.tripped = 1;
            return;
        }
    } else {
        cb.over = 0;
    }
    cb.pos_q16 += CB_STEP_Q16;
    ns = cb.pos_q16 >> 16;
    cb.pos_q16 &= 0xFFFFu;
    if ((cb.blk++ & 7u) < cb.duty) {
        t0 = fm1_ticks();
        for (s = 0; s < ns; s++) {
            if (cb.dac) {
                cb_write(0, 0x2A);
                cb_write(1, (cb.dacph += 7u) & 0xFFu);
            }
            cb_sample(&l, &r);
        }
        cb.w_ticks += fm1_ticks() - t0;
        cb.w_samples += ns;
        cb.last_l = l;
        cb.last_r = r;
    }
    k = cb.vol;
    if (k)
        for (i = 0; i < n; i++) {        /* (held: the benchmark measures, it does not resample) */
            int32_t xl = (cb.last_l >> 4) * (int32_t)k, xr = (cb.last_r >> 4) * (int32_t)k;
            mix_l[i] += xl;
            mix_r[i] += xr;
            if (cb.fx) {
                snd_c[i] += xl >> 2;
                snd_d[i] += xl >> 2;
                snd_r[i] += xl >> 1;
            }
        }
    if (++cb.w_blocks >= CB_WINDOW) {
        if (cb.w_samples) {
            uint32_t tps_x10 = cb.w_ticks * 10u / cb.w_samples;   /* (w_ticks <= 24e6: no overflow) */
            cb.est_permille = tps_x10 * 1000u / CB_TICKS_PER_NATIVE_X10;
            if (cb.est_permille > cb.peak)
                cb.peak = cb.est_permille;
        }
        cb.samples = cb.w_samples;
        cb.w_ticks = cb.w_samples = cb.w_blocks = 0;
    }
}

/* the console (console.c con_exec: "bench ..."). Main loop */
static void con_kv(const char *k, int32_t v);
static void con_puts(const char *s);
static uint32_t con_num(const char **p, int *ok);
static int con_word(const char **p, const char *w);
static void cb_console(const char *p)
{
    static const char *const NAMES[] = {"off", "nuked", "ymfm"};
    int ok = 0;
    uint32_t v;
    if (con_word(&p, "off"))
        cb.req = CB_OFF;
    else if (con_word(&p, "nuked"))
        cb.req = CB_NUKED, cb.tripped = 0, cb.peak = 0;
    else if (con_word(&p, "ymfm"))
        cb.req = CB_YMFM, cb.tripped = 0, cb.peak = 0;
    else if (con_word(&p, "duty")) {
        v = con_num(&p, &ok);
        if (ok && v >= 1u && v <= 8u)
            cb.duty = (uint8_t)v, cb.peak = 0;
    } else if (con_word(&p, "dac")) {
        v = con_num(&p, &ok);
        if (ok) {
            cb.dac = (uint8_t)(v != 0);
            if (cb.mode != CB_OFF)
                cb.req = cb.mode;       /* restart: the DAC's enable is written at the start */
            cb.peak = 0;
        }
    } else if (con_word(&p, "fx")) {
        v = con_num(&p, &ok);
        if (ok)
            cb.fx = (uint8_t)(v != 0);
    } else if (con_word(&p, "vol")) {
        v = con_num(&p, &ok);
        if (ok && v <= 15u)
            cb.vol = (uint8_t)v;
    } else if (*p) {
        con_puts("bench [nuked|ymfm|off] [duty 1-8] [dac 0|1] [fx 0|1] [vol 0-15]\r\n");
        return;
    }
    con_puts("core ");
    con_puts(NAMES[cb.mode % 3u]);
    con_puts("\r\n");
    con_kv("duty_8ths", cb.duty);
    con_kv("dac", cb.dac);
    con_kv("fx", cb.fx);
    con_kv("vol", cb.vol);
    con_kv("tripped", cb.tripped);
    con_kv("native_samples_per_s", (int32_t)cb.samples);
    con_kv("est_permille", (int32_t)cb.est_permille);
    con_kv("est_peak_permille", (int32_t)cb.peak);
    con_kv("cpu_pct", (int32_t)(song.cpu_q8 * 100u / 256u));
}
#endif /* FELUCCA_CHIPBENCH */
