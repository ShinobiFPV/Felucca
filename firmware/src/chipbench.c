/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 William Kew, ShinTech Electronics */
/* ShinTech Phase 1: the OPN2 core benchmark (FELUCCA_CHIPBENCH=1 builds it in; off in every other build). It runs one
 * of the two candidate YM2612 cores in the audio ISR, after the parts and before the FX buses (fx.c mix_block), on
 * the load of chipbench_script.h (six channels, SSG-EG, AM / PM LFO), at the chip's native rate (53,267 Hz: 38.64
 * native samples per 32-sample block), and times it with TIMER4 (24 MHz).
 *
 * Safety: it is OFF at every boot and starts only from the USB serial console. DUTY n renders n / 32 of the native
 * samples of every block (the chip runs slow; its cost is measured per native sample, so the projection to full time
 * holds at any duty, and no block holds more than its share). The script goes in one write a block. If the ISR's load
 * (song.cpu_q8, the INFO screen's CPU) goes over 90 % for 4 halves in a row, it switches itself off and says so
 * (TRIP): a core that does not fit never starves the main loop long enough for the watchdog.
 *
 * Console (any terminal on the FM-1's serial port):
 *   bench                     state and the last second's figures
 *   bench nuked | ymfm | off  the core
 *   bench duty N              1..32 (32nds of the native samples rendered; 8 at start)
 *   bench dac 0|1             channel 6 as the DAC, a sample written every native sample
 *   bench fx 0|1              the chip's output into the chorus, delay and reverb sends (the buses always run)
 *   bench c1 0|1              render on the second core (MENU > SYSTEM > 2ND CORE ON, from power-on); the parts stay
 *                             on the first core while it does
 *   bench vol N               0..15, its level in the mix (0 silent; 6 at start)
 *   bench load 0|1            0 the worst case (SSG-EG, AM and PM everywhere), 1 typical patches (chipbench_script.h)
 * Figures: est_permille = the core alone at full duty, in 1/1000 of a CPU core (its ticks per native sample against a
 * native sample's time); cpu_pct = the first core's whole ISR as measured now (MENU > SYSTEM > INFO). */
#if FELUCCA_CHIPBENCH
#define OPN2_POOL __attribute__((section(".pool")))
#define OPN2_FAST __attribute__((section(".fast_text")))
#include "chips/opn2.c"
#include "chips/ym3438.c"
#include "chipbench_script.h"

enum { CB_OFF, CB_NUKED, CB_YMFM };
#define CB_STEP_Q16 2532050u            /* native samples per 32-sample block, Q16: 7670453 / 144 / (24e6 / 544) x 32 */
#define CB_WINDOW 1378u                 /* blocks per figure: ~1 s */
#define CB_TICKS_PER_NATIVE_X10 4506u   /* 24e6 / 53267 x 10: a native sample's whole time in TIMER4 ticks */
#define CB_DUTY_FULL 32u

static ym3438_t cb_nuked __attribute__((section(".pool")));
static opn2_t cb_opn2 __attribute__((section(".pool")));
static cb_write_t cb_script[CB_SCRIPT_MAX] __attribute__((section(".pool")));
static struct {
    volatile uint8_t mode, req;         /* req: the console's mode, taken at the next block (0xFF none) */
    uint8_t duty, dac, fx, vol, c1, tripped, c1_failed, over, c1_busy, typical;
    uint32_t pos_q16, duty_acc, dacph, nscript, sidx, ns;
    int32_t last_l, last_r;
    volatile uint32_t r_ticks;          /* the last render's ticks (either core) */
    uint32_t w_ticks, w_samples, w_blocks;   /* this window */
    volatile uint32_t est_permille, samples, peak, c1_blocks;   /* the last window's */
} cb = {CB_OFF, 0xFF, 8, 0, 0, 6};

static void cb_write(uint32_t port, uint32_t data)
{
    if (cb.mode == CB_NUKED)
        OPN2_Write(&cb_nuked, port, (Bit8u)data);
    else
        opn2_write(&cb_opn2, port, (uint8_t)data);
}
static void cb_sample(int32_t *l, int32_t *r)
{
    if (cb.mode == CB_NUKED) {          /* 24 internal cycles, their multiplexed outputs summed */
        Bit16s b[2];
        int32_t sl = 0, sr = 0;
        uint32_t k;
        for (k = 0; k < 24u; k++) {
            OPN2_Clock(&cb_nuked, b);
            sl += b[0];
            sr += b[1];
        }
        *l = sl << 4;                   /* ~ the port's scale */
        *r = sr << 4;
    } else {
        int32_t o[2];
        OPN2_FAR(opn2_generate)(&cb_opn2, o);   /* (in RAM: .fast_text) */
        *l = o[0];
        *r = o[1];
    }
}

static void cb_start(void)              /* (in the ISR, at a block start: the mode just changed) */
{
    cb.nscript = cb_script_build(cb_script, cb.typical);
    cb.sidx = 0;
    if (cb.mode == CB_NUKED) {
        OPN2_SetChipType(ym3438_mode_ym2612);
        OPN2_Reset(&cb_nuked);
    } else {
        opn2_reset(&cb_opn2);
    }
    cb.pos_q16 = cb.duty_acc = cb.w_ticks = cb.w_samples = cb.w_blocks = 0;
    cb.over = 0;
}

/* the script, one write a block (then two samples: Nuked commits a write in its operator's slot), then the DAC's
 * enable: 1 = still starting */
static int cb_starting(void)
{
    int32_t l, r;
    if (cb.sidx > cb.nscript)
        return 0;
    if (cb.sidx < cb.nscript) {
        cb_write(cb_script[cb.sidx].port, cb_script[cb.sidx].reg);
        cb_write(cb_script[cb.sidx].port + 1u, cb_script[cb.sidx].data);
        cb_sample(&l, &r);
        cb_sample(&l, &r);
    } else {
        cb_write(0, 0x2B);
        cb_write(1, cb.dac ? 0x80 : 0x00);
    }
    cb.sidx++;
    return 1;
}

/* ns native samples, timed (on either core) */
static void cb_render(uint32_t ns)
{
    uint32_t s, t0 = fm1_ticks();       /* (TIMER4: the same counter from both cores) */
    int32_t l = cb.last_l, r = cb.last_r;
    for (s = 0; s < ns; s++) {
        if (cb.dac) {
            cb_write(0, 0x2A);
            cb_write(1, (cb.dacph += 7u) & 0xFFu);
        }
        cb_sample(&l, &r);
    }
    cb.r_ticks = fm1_ticks() - t0;
    cb.last_l = l;
    cb.last_r = r;
}
static void cb_c1_job(uint32_t ns) { cb_render(ns); }

/* the start of a block (fx.c mix_block, before the parts): the samples this block renders; on the second core it
 * starts them now. 1 = the second core has the chip (the parts stay on the first) */
static int cb_begin(void)
{
    uint32_t full;
    cb.ns = 0;
    cb.c1_busy = 0;
    if (cb.req != 0xFF) {
        cb.mode = cb.req;
        cb.req = 0xFF;
        if (cb.mode != CB_OFF)
            cb_start();
    }
    if (cb.mode == CB_OFF || cb_starting())
        return 0;
    if (song.cpu_q8 > 230u) {           /* the trip: over 90 % (230 / 256) for 16 blocks = 4 halves */
        if (++cb.over >= 16u) {
            cb.mode = CB_OFF;
            cb.tripped = 1;
            return 0;
        }
    } else {
        cb.over = 0;
    }
    cb.pos_q16 += CB_STEP_Q16;
    full = cb.pos_q16 >> 16;
    cb.pos_q16 &= 0xFFFFu;
    cb.duty_acc += full * cb.duty;
    cb.ns = cb.duty_acc / CB_DUTY_FULL;
    cb.duty_acc %= CB_DUTY_FULL;
#if FELUCCA_CPU2
    if (cb.c1 && C1_ON() && cb.ns && C1_RUN(cb_c1_job, cb.ns)) {
        cb.c1_busy = 1;
        cb.c1_blocks++;
    }
#endif
    return cb.c1_busy;
}

/* after the parts (fx.c mix_block): the chip's samples (rendered here, or waited for), into the dry mix and, with FX,
 * the sends */
static void cb_block(int32_t *mix_l, int32_t *mix_r, int32_t *snd_c, int32_t *snd_d, int32_t *snd_r, uint32_t n)
{
    uint32_t i, k;
    if (cb.mode == CB_OFF)
        return;
    if (cb.c1_busy) {
#if FELUCCA_CPU2
        if (C1_WAIT()) {                /* it stopped answering: held from now on, one core */
            cb.c1_failed = 1;
            cb.c1 = 0;
            cb.r_ticks = 0;
        }
#endif
    } else if (cb.ns) {
        cb_render(cb.ns);
    }
    if (cb.ns) {
        cb.w_ticks += cb.r_ticks;
        cb.w_samples += cb.ns;
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
            uint32_t tps_x10 = cb.w_ticks / cb.w_samples * 10u + cb.w_ticks % cb.w_samples * 10u / cb.w_samples;
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
        if (ok && v >= 1u && v <= CB_DUTY_FULL)
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
    } else if (con_word(&p, "c1")) {
        v = con_num(&p, &ok);
        if (ok)
            cb.c1 = (uint8_t)(v != 0), cb.peak = 0;
    } else if (con_word(&p, "load")) {
        v = con_num(&p, &ok);
        if (ok) {
            cb.typical = (uint8_t)(v != 0);
            if (cb.mode != CB_OFF)
                cb.req = cb.mode;       /* restart with the other script */
            cb.peak = 0;
        }
    } else if (con_word(&p, "vol")) {
        v = con_num(&p, &ok);
        if (ok && v <= 15u)
            cb.vol = (uint8_t)v;
    } else if (*p) {
        con_puts("bench [nuked|ymfm|off] [duty 1-32] [dac 0|1] [fx 0|1] [c1 0|1] [load 0|1] [vol 0-15]\r\n");
        return;
    }
    con_puts("core ");
    con_puts(NAMES[cb.mode % 3u]);
    con_puts("\r\n");
    con_kv("duty_32nds", cb.duty);
    con_kv("dac", cb.dac);
    con_kv("load_typical", cb.typical);
    con_kv("fx", cb.fx);
    con_kv("c1", cb.c1);
#if FELUCCA_CPU2
    con_kv("c1_on", C1_ON());
#endif
    con_kv("c1_failed", cb.c1_failed);
    con_kv("vol", cb.vol);
    con_kv("tripped", cb.tripped);
    con_kv("starting", cb.mode != CB_OFF && cb.sidx <= cb.nscript);
    con_kv("native_samples_per_s", (int32_t)cb.samples);
    con_kv("est_permille", (int32_t)cb.est_permille);
    con_kv("est_peak_permille", (int32_t)cb.peak);
    con_kv("cpu_pct", (int32_t)(song.cpu_q8 * 100u / 256u));
}
#endif /* FELUCCA_CHIPBENCH */
