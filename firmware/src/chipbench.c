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
 *   bench split 0|1           the six channels on two cores: 4..6 on the second (started before the parts), 1..3 on the
 *                             first (after them); est_permille is then the first core's part, est1_permille the
 *                             second's (MENU > SYSTEM > 2ND CORE ON)
 * Figures: est_permille = the core alone at full duty, in 1/1000 of a CPU core (its ticks per native sample against a
 * native sample's time); cpu_pct = the first core's whole ISR as measured now (MENU > SYSTEM > INFO). */
#if FELUCCA_CHIPBENCH
#include "chips/opn2.c"                 /* (already in: eng_opn2.c, with its placement of the tables and the code) */
#include "chips/ym3438.c"
#include "chipbench_script.h"

enum { CB_OFF, CB_NUKED, CB_YMFM };
#define CB_STEP_Q16 2532050u            /* native samples per 32-sample block, Q16: 7670453 / 144 / (24e6 / 544) x 32 */
#define CB_WINDOW 1378u                 /* blocks per figure: ~1 s */
#define CB_TICKS_PER_NATIVE_X10 4506u   /* 24e6 / 53267 x 10: a native sample's whole time in TIMER4 ticks */
#define CB_DUTY_FULL 32u

static ym3438_t cb_nuked __attribute__((section(".pool")));
static opn2_t cb_opn2;            /* (.bss: quicker loads than the pool, bench cal) */
static cb_write_t cb_script[CB_SCRIPT_MAX] __attribute__((section(".pool")));
#define CB_NS_MAX 48u
static opn2_blk_t cb_blk[4] __attribute__((section(".pool")));            /* bench split: the begun blocks */
static int32_t cb_chout[CB_NS_MAX][OPN2_CHANNELS][2] __attribute__((section(".pool")));   /* .. the channels' outputs */
static struct {
    volatile uint8_t mode, req;         /* req: the console's mode, taken at the next block (0xFF none) */
    uint8_t duty, dac, fx, vol, c1, tripped, c1_failed, over, c1_busy, typical;
    uint8_t probe;                      /* bench probe: 0 the whole chip, 1 clocked only (no channel heard) */
    uint8_t split, split_on;            /* bench split: channels 4..6 on the second core; this block does */
    uint32_t split_n, split_nb;         /* the samples and blocks begun for the split */
    volatile uint32_t r_ticks1;         /* the second core's part of the last split render */
    uint32_t w_ticks1;
    volatile uint32_t est1_permille;
    volatile uint8_t keys_req;          /* bench keys: 0 none, 1 key on / 2 key off all six channels at the next block */
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
        opn2_generate(&cb_opn2, o);
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

/* ns native samples, timed (on either core). The port renders them in one call, as an engine will (with the DAC on,
 * one sample a call: the DAC's sample is written between them); Nuked one at a time */
static void cb_render(uint32_t ns)
{
    uint32_t s, t0 = fm1_ticks();       /* (TIMER4: the same counter from both cores) */
    int32_t l = cb.last_l, r = cb.last_r;
    if (cb.mode == CB_YMFM && !cb.dac && ns <= CB_NS_MAX) {
        if (cb.probe == 1) {            /* (bench probe 1: clocked, none heard) */
            int32_t t[8][OPN2_CHANNELS][2];
            for (s = 0; s < ns; s += 8u)
                OPN2_FAR(opn2_render_n)(&cb_opn2, t, ns - s < 8u ? ns - s : 8u, 0);
            l = t[0][0][0];
            r = t[0][0][1];
        } else {
            int32_t lr[CB_NS_MAX][2];
            OPN2_FAR(opn2_generate_n)(&cb_opn2, lr, ns);
            if (ns) {
                l = lr[ns - 1u][0];
                r = lr[ns - 1u][1];
            }
        }
    } else {
        for (s = 0; s < ns; s++) {
            if (cb.dac) {
                cb_write(0, 0x2A);
                cb_write(1, (cb.dacph += 7u) & 0xFFu);
            }
            cb_sample(&l, &r);
        }
    }
    cb.r_ticks = fm1_ticks() - t0;
    cb.last_l = l;
    cb.last_r = r;
}
static void cb_c1_job(uint32_t ns) { cb_render(ns); }

/* bench split, the second core's part: channels 4..6 of the begun blocks */
static void cb_split_job(uint32_t unused)
{
    uint32_t t0 = fm1_ticks();
    (void)unused;
    OPN2_FAR(opn2_channels_n)(&cb_opn2, cb_blk, cb.split_nb, cb_chout, 3, 6);
    cb.r_ticks1 = fm1_ticks() - t0;
}

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
    if (cb.keys_req && cb.mode == CB_YMFM) {   /* (bench keys: the six key-on registers at once; the port takes them so) */
        uint32_t ch;
        for (ch = 0; ch < 6u; ch++) {
            cb_write(0, 0x28);
            cb_write(1, (cb.keys_req == 1u ? 0xF0u : 0u) | (ch < 3u ? ch : ch + 1u));
        }
        cb.keys_req = 0;
    }
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
    cb.split_on = 0;
#if FELUCCA_CPU2
    if (cb.split && C1_ON() && cb.mode == CB_YMFM && !cb.dac && !cb.probe && cb.ns && cb.ns <= CB_NS_MAX) {
        uint32_t nb, t0 = fm1_ticks();
        cb.split_n = OPN2_FAR(opn2_begin_n)(&cb_opn2, cb_blk, 4, cb.ns, 6, &nb);
        cb.split_nb = nb;
        cb.r_ticks = fm1_ticks() - t0;         /* (the begin: on the first core's account) */
        if (C1_RUN(cb_split_job, 0)) {
            cb.c1_busy = cb.split_on = 1;
            cb.c1_blocks++;
        } else {                                /* (no second core after all: the first renders all six) */
            OPN2_FAR(opn2_channels_n)(&cb_opn2, cb_blk, nb, cb_chout, 3, 6);
            cb.split_on = 2;
        }
        return cb.c1_busy;
    }
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
    if (cb.split_on) {                  /* bench split: channels 1..3 here, while the second core has 4..6 */
        int32_t lr[CB_NS_MAX][2];
        uint32_t t0 = fm1_ticks(), t1 = 0;
        OPN2_FAR(opn2_channels_n)(&cb_opn2, cb_blk, cb.split_nb, cb_chout, 0, 3);
        t1 = fm1_ticks() - t0;
#if FELUCCA_CPU2
        if (cb.split_on == 1 && C1_WAIT()) {
            cb.c1_failed = 1;
            cb.split = 0;
        }
#endif
        t0 = fm1_ticks();
        OPN2_FAR(opn2_mix_n)(&cb_opn2, (const int32_t (*)[OPN2_CHANNELS][2])cb_chout, lr, cb.split_n);
        if (cb.split_n < cb.ns)                 /* (a sweep's prepare came first: the rest in one piece) */
            OPN2_FAR(opn2_generate_n)(&cb_opn2, lr + cb.split_n, cb.ns - cb.split_n);
        t1 += fm1_ticks() - t0;
        cb.last_l = lr[cb.ns - 1u][0];
        cb.last_r = lr[cb.ns - 1u][1];
        cb.w_ticks += cb.r_ticks + t1;          /* the first core's part: the begin, channels 1..3, the mix */
        cb.w_ticks1 += cb.split_on == 1 ? cb.r_ticks1 : 0u;
        cb.w_samples += cb.ns;
    } else {
        if (cb.c1_busy) {
#if FELUCCA_CPU2
            if (C1_WAIT()) {            /* it stopped answering: held from now on, one core */
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
            tps_x10 = cb.w_ticks1 / cb.w_samples * 10u + cb.w_ticks1 % cb.w_samples * 10u / cb.w_samples;
            cb.est1_permille = tps_x10 * 1000u / CB_TICKS_PER_NATIVE_X10;
            if (cb.est_permille > cb.peak)
                cb.peak = cb.est_permille;
        }
        cb.samples = cb.w_samples;
        cb.w_ticks = cb.w_ticks1 = cb.w_samples = cb.w_blocks = 0;
    }
}

/* the console's helpers (console.c, later in the build) */
static void con_kv(const char *k, int32_t v);
static void con_puts(const char *s);
static uint32_t con_num(const char **p, int *ok);
static int con_word(const char **p, const char *w);

/* ------------------------------------------------------------ calibration --- */
/* "bench cal": what this core's operations cost, in TIMER4 ticks (24 MHz) per 1024 iterations, run in the main loop
 * (the audio ISR preempts it: the figures include its share, cpu_pct, printed beside them). Each loop is a dependent
 * chain, so its time is the latency of one step plus the loop's own. */
#define CB_CAL_N 65536u
static uint32_t cb_chase_ram[256];                                  /* .bss (RAM) */
static uint32_t cb_chase_pool[256] __attribute__((section(".pool")));
static const uint32_t CB_CHASE_XIP[256] = {                         /* XIP: i -> (i * 97 + 31) & 255, a full cycle */
#define CB_C4(i) (((i) * 97u + 31u) & 255u), ((((i) + 1u) * 97u + 31u) & 255u), ((((i) + 2u) * 97u + 31u) & 255u), \
                 ((((i) + 3u) * 97u + 31u) & 255u)
#define CB_C16(i) CB_C4(i), CB_C4((i) + 4u), CB_C4((i) + 8u), CB_C4((i) + 12u)
#define CB_C64(i) CB_C16(i), CB_C16((i) + 16u), CB_C16((i) + 32u), CB_C16((i) + 48u)
    CB_C64(0u), CB_C64(64u), CB_C64(128u), CB_C64(192u)};
#undef CB_C64
#undef CB_C16
#undef CB_C4
static __attribute__((noinline)) uint32_t cb_cal_alu(uint32_t n, uint32_t x)   /* 8 dependent ALU ops a step */
{
    while (n--)
        x = ((((x ^ (x << 5)) + 7u) ^ (x >> 3)) + 11u) ^ ((x << 1) + 3u);
    return x;
}
static __attribute__((noinline, section(".fast_text"))) uint32_t cb_cal_alu_ram(uint32_t n, uint32_t x)   /* the same, in RAM */
{
    while (n--)
        x = ((((x ^ (x << 5)) + 7u) ^ (x >> 3)) + 11u) ^ ((x << 1) + 3u);
    return x;
}
static __attribute__((noinline)) uint32_t cb_cal_mul(uint32_t n, uint32_t x)   /* a dependent multiply-add a step */
{
    while (n--)
        x = x * 1664525u + 1013904223u;
    return x;
}
static __attribute__((noinline)) uint32_t cb_cal_chase(const uint32_t *t, uint32_t n)   /* a dependent load a step */
{
    uint32_t i = 0;
    while (n--)
        i = t[i];
    return i;
}
static uint32_t cb_cal_sink;
static void cb_cal(void)
{
    static const char *const NAMES[] = {"alu8_xip", "alu8_ram", "mul", "load_ram", "load_pool", "load_xip", "empty"};
    uint32_t k, i, t0, t;
    for (i = 0; i < 256u; i++)
        cb_chase_ram[i] = cb_chase_pool[i] = CB_CHASE_XIP[i];
    for (k = 0; k < 7u; k++) {
        t0 = fm1_ticks();
        switch (k) {
        case 0: cb_cal_sink += cb_cal_alu(CB_CAL_N, t0); break;
        case 1: cb_cal_sink += OPN2_FAR(cb_cal_alu_ram)(CB_CAL_N, t0); break;
        case 2: cb_cal_sink += cb_cal_mul(CB_CAL_N, t0); break;
        case 3: cb_cal_sink += cb_cal_chase(cb_chase_ram, CB_CAL_N); break;
        case 4: cb_cal_sink += cb_cal_chase(cb_chase_pool, CB_CAL_N); break;
        case 5: cb_cal_sink += cb_cal_chase(CB_CHASE_XIP, CB_CAL_N); break;
        default: cb_cal_sink += cb_cal_alu(0, t0); break;
        }
        t = fm1_ticks() - t0;
        con_kv(NAMES[k], (int32_t)(t / (CB_CAL_N / 1024u)));   /* ticks per 1024 steps */
    }
    con_kv("cpu_pct", (int32_t)(song.cpu_q8 * 100u / 256u));
}

/* the console (console.c con_exec: "bench ..."). Main loop */
static void cb_console(const char *p)
{
    static const char *const NAMES[] = {"off", "nuked", "ymfm"};
    int ok = 0;
    uint32_t v;
    if (con_word(&p, "cal")) {
        cb_cal();
        return;
    }
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
    } else if (con_word(&p, "split")) {
        v = con_num(&p, &ok);
        if (ok)
            cb.split = (uint8_t)(v != 0), cb.peak = 0;
    } else if (con_word(&p, "probe")) {
        v = con_num(&p, &ok);
        if (ok && v <= 1u)
            cb.probe = (uint8_t)v, cb.peak = 0;
    } else if (con_word(&p, "keys")) {
        v = con_num(&p, &ok);
        if (ok)
            cb.keys_req = (uint8_t)(v ? 1u : 2u), cb.peak = 0;
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
    con_kv("probe", cb.probe);
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
    con_kv("split", cb.split);
    con_kv("est_permille", (int32_t)cb.est_permille);
    con_kv("est1_permille", (int32_t)cb.est1_permille);
    con_kv("est_peak_permille", (int32_t)cb.peak);
    con_kv("cpu_pct", (int32_t)(song.cpu_q8 * 100u / 256u));
}
#endif /* FELUCCA_CHIPBENCH */
