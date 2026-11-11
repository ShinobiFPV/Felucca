/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 William Kew, ShinTech Electronics */
/* POLY6 (firmware/src/eng_poly6.c), through hostsim.c as the other engine tests.
 *   build/host/poly6_test [DEMODIR]          (run_tests.sh: build/poly6_demo)
 * 1. the ladder's frequency response (RES 0) at three cutoffs: flat well below (fc / 8: 0 +- 1.5 dB), about -12 dB at
 *    the cutoff (four poles, -3 dB each: -12 +- 3), 24 dB / octave above (2 fc -> 4 fc: 24 +- 4 dB); RES raises the
 *    cutoff's gain (RES 100: +8 dB at least); at RES 127 it rings on by itself after an impulse (self-oscillation) near
 *    the cutoff (+- 15 %); every cutoff and RES with a loud input stays bounded.
 * 2. the chorus's delay modulation: I and II sweep 1.66 .. 5.35 ms (+- 0.03 ms) at 0.513 / 0.863 Hz (the period +- 1 %),
 *    I+II 3.0 .. 3.6 ms at 9.75 Hz; left and right in opposite directions (their sum constant); an impulse through the
 *    part comes out of the left side at the left delay (+- 1 sample); OFF: no side, the part as it was.
 * 3. stereo: chorus on, the mix's left and right differ (correlated, not equal); off, centre: the same.
 * 4. VOICES 4 / 8: the part's voices held; GATE: the amp at full while the key is held, the release silent.
 * 5. every preset: an 8-note chord for 3 s sounds, no DC (|mean| of the last 2 s < 1 % of 8 voices), no voice sum past 2 x VOICE_FS a
 *    voice, no mix sample at full scale.
 * 6. cost (an estimate: this host has no instruction counter): POLY6's render time against ANALOG's, 8 voices, its
 *    chorus on, scaled by ANALOG's cost on the device (fx.c ENG_VCOST: 163 host instructions a voice and sample, ~1.7 %
 *    of the device per 100), printed for 4 and 8 voices with the chorus.
 * 7. demos into DEMODIR: a chord of each preset. */
#define main hostsim_main
#include "hostsim.c"
#undef main
#include <math.h>
#include <time.h>

static int bad;
static void check(const char *what, int ok)
{
    printf("poly6: %-96s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}
static double cut_hz(int v) { return 30.0 * pow(16000.0 / 30.0, v / 127.0); }   /* (gen_engine_tables.py cutoff_hz) */
/* four trapezoidal one-poles at fc, the gain at f (dB): 1 / (1 + j w)^4, w warped as the bilinear transform does */
static double tpt4(double fc, double f)
{
    double w = tan(M_PI * f / FS) / tan(M_PI * fc / FS);
    return -20.0 * log10((1.0 + w * w) * (1.0 + w * w));
}

/* the ladder's gain (dB) for a sine at f, cutoff value cut, RES res */
static double lad_gain(int cut, int res, double f)
{
    p6_lad_t c;
    int32_t s[4] = {0, 0, 0, 0}, y;
    uint32_t i, n = FS * 2u;
    double in2 = 0, out2 = 0;
    p6_lad_coef(&c, cut << 8, res);
    for (i = 0; i < n; i++) {
        int32_t x = (int32_t)(6000.0 * sin(2.0 * M_PI * f * i / FS));
        y = p6_ladder(&c, x, s);
        if (i >= n / 2u) {
            in2 += (double)x * x;
            out2 += (double)y * y;
        }
    }
    return 10.0 * log10((out2 + 1e-9) / in2);
}

static void setup(uint32_t pi)
{
    memset(trk, 0, sizeof trk);
    host_tracks_init();
    host_preset(&trk[0], ENGI_POLY6, pi);
    poly6_preset_x(&trk[0], pi);
    trk[0].p[P_DLY] = trk[0].p[P_REV] = trk[0].p[P_CHOR] = 0;
}
static uint32_t held(void)
{
    uint32_t i, n = 0;
    for (i = 0; i < NVOICE; i++)
        n += trk[0].v[i].active && trk[0].v[i].gate;
    return n;
}

int main(int argc, char **argv)
{
    const char *demo = argc > 1 ? argv[1] : 0;
    uint32_t i, k, ok;

    /* 1. the ladder */
    {
        static const int CUTS[3] = {50, 75, 95};
        for (k = 0, ok = 1; k < 3u; k++) {
            double fc = cut_hz(CUTS[k]), lo = lad_gain(CUTS[k], 0, fc / 8.0), at = lad_gain(CUTS[k], 0, fc);
            double f2 = lad_gain(CUTS[k], 0, fc * 2.0), f4 = lad_gain(CUTS[k], 0, fc * 4.0);
            printf("  CUT %d (%.0f Hz): fc/8 %+.1f dB, fc %+.1f dB, 2fc %+.1f dB, 4fc %+.1f dB (slope %.1f dB/oct)\n", CUTS[k],
                   fc, lo, at, f2, f4, f2 - f4);
            ok &= fabs(lo - tpt4(fc, fc / 8.0)) <= 1.0 && fabs(at - tpt4(fc, fc)) <= 1.0 && fabs(f2 - tpt4(fc, 2.0 * fc)) <= 1.5 &&
                  fabs(f4 - tpt4(fc, 4.0 * fc)) <= 2.0 && f2 - f4 >= 19.0;
        }
        check("the ladder at RES 0: four poles' response (TPT): 0 dB below, -12 dB at the cutoff, ~24 dB / octave above", (int)ok);
        ok = lad_gain(75, 100, cut_hz(75)) >= lad_gain(75, 0, cut_hz(75)) + 8.0;
        check("RES 100: the cutoff's gain up 8 dB at least", (int)ok);
    }
    {
        p6_lad_t c;
        int32_t s[4] = {0, 0, 0, 0}, y, prev = 0;
        uint32_t n = FS, zc = 0;
        double e2 = 0;
        p6_lad_coef(&c, 70 << 8, 127);
        for (i = 0; i < n; i++) {
            y = p6_ladder(&c, i < 32u ? 12000 : 0, s);
            if (i >= n / 2u) {
                e2 += (double)y * y;
                zc += (prev < 0) != (y < 0);
            }
            prev = y;
        }
        {
            double rms = sqrt(e2 / (n / 2u)), f = zc / 2.0 / 0.5;
            printf("  RES 127, CUT 70 (%.0f Hz): after an impulse, the second half second: rms %.0f, %.0f Hz\n", cut_hz(70), rms, f);
            check("RES 127: it rings on by itself (self-oscillation), near the cutoff", rms > 2000.0 &&
                  fabs(f / cut_hz(70) - 1.0) <= 0.15);
        }
    }
    {
        int cut, res;
        ok = 1;
        for (cut = 0; cut <= 127; cut += 9)
            for (res = 0; res <= 127; res += 21) {
                p6_lad_t c;
                int32_t s[4] = {0, 0, 0, 0}, y, mx = 0;
                p6_lad_coef(&c, cut << 8, res);
                for (i = 0; i < 4000u; i++) {
                    y = p6_ladder(&c, (i / 20u) & 1u ? 40000 : -40000, s);
                    mx = abs(y) > mx ? abs(y) : mx;
                }
                ok &= mx < 150000 && abs(s[0]) < 200000 && abs(s[3]) < 200000;
            }
        check("every CUT and RES, a loud square in: bounded", (int)ok);
    }

    /* 2. the chorus's delay */
    {
        static const uint32_t MODE[3] = {P6C_I, P6C_II, P6C_I_II};
        static const double RATE[3] = {0.513, 0.863, 9.75}, LO[3] = {1.66, 1.66, 3.0}, HI[3] = {5.35, 5.35, 3.6};
        for (k = 0, ok = 1; k < 3u; k++) {
            uint32_t inc, ph = 0, period = 0, first = 0, tops = 0, at_top = 0;
            int32_t c, d, mn = 1 << 30, mxd = 0, sum_ok = 1;
            p6_sweep(MODE[k], &inc, &c, &d);
            for (i = 0; i < FS * 6u; i++, ph += inc) {     /* the extremes, the two taps' sum */
                int32_t l = p6_delay(ph, c, d, 1), r = p6_delay(ph, c, d, -1);
                mn = l < mn ? l : mn;
                mxd = l > mxd ? l : mxd;
                sum_ok &= abs(l + r - 2 * c) <= 2;
            }
            for (i = 0, ph = 0; i < FS * 6u && tops < 3u; i++, ph += inc) {   /* the period: from top to top */
                int32_t l = p6_delay(ph, c, d, 1);
                if (l >= mxd - 1 && !at_top) {
                    if (tops == 1u)
                        first = i;
                    else if (tops == 2u)
                        period = i - first;
                    tops++;
                }
                at_top = l >= mxd - 1;
            }
            {
                double lo = mn / 256.0 / FS * 1000.0, hi = mxd / 256.0 / FS * 1000.0, rate = period ? (double)FS / period : 0;
                printf("  chorus %s: %.2f .. %.2f ms at %.3f Hz\n", k == 2u ? "I+II" : k ? "II" : "I", lo, hi, rate);
                ok &= fabs(lo - LO[k]) <= 0.03 && fabs(hi - HI[k]) <= 0.03 && fabs(rate / RATE[k] - 1.0) <= 0.01 && sum_ok;
            }
        }
        check("the chorus's delays: I / II 1.66 .. 5.35 ms at 0.513 / 0.863 Hz, I+II 3.0 .. 3.6 ms at 9.75 Hz, L / R opposite",
              (int)ok);
    }
    {
        static int32_t b[CTL];
        p6_mem_t *c;
        uint32_t at = 0, peak_at = 0, want;
        int32_t peak = 0, *side;
        setup(0);
        trk[0].p[P_P6CHO] = P6C_I;
        trk[0].p[P_E7] = 1;                            /* (HPF FLAT) */
        T_SPREAD(&trk[0]) = 1;
        c = (p6_mem_t *)ENG_MEM(0);
        side = poly6_side(&trk[0]);
        for (k = 0; k < 40u; k++) {                    /* an impulse in block 20 */
            memset(b, 0, sizeof b);
            if (k == 20u)
                b[0] = 20000;
            if (k == 20u) {
                at = k * CTL;
                uint32_t inc;
                int32_t cc, dd;
                p6_sweep(P6C_I, &inc, &cc, &dd);
                want = (uint32_t)((p6_delay(c->lfo + inc, cc, dd, 1) + 128) >> 8);
            }
            (void)poly6_post(&trk[0], b, CTL);
            for (i = 0; i < CTL; i++)
                if (k * CTL + i > at + 4u && side[i] - b[i] / 2 > peak) {
                    peak = side[i] - b[i] / 2;
                    peak_at = k * CTL + i;
                }
        }
        printf("  an impulse at sample %u: the left tap's peak %u samples later (its delay then %u)\n", at, peak_at - at, want);
        check("an impulse through the chorus: out of the left side at the left tap's delay (+- 2 samples)",
              peak > 1000 && (int32_t)(peak_at - at) - (int32_t)want <= 2 && (int32_t)want - (int32_t)(peak_at - at) <= 2);
    }

    /* 3. stereo, 4. voices and GATE */
    {
        static int32_t out[2 * CTL];
        double dl = 0, dr = 0, ddiff = 0;
        uint32_t b2;
        for (k = 0; k < 2u; k++) {
            setup(0);                                  /* WARM PAD */
            trk[0].p[P_P6CHO] = k ? P6C_I : P6C_OFF;
            trk[0].p[P_PAN] = 0;
            for (i = 0; i < 3u; i++)
                trk_note_on(&trk[0], 60 + 4 * i, 100);
            dl = dr = ddiff = 0;
            for (b2 = 0; b2 < FS / CTL; b2++) {
                mix_block(out, CTL);
                for (i = 0; i < CTL; i++) {
                    dl += (double)out[2 * i] * out[2 * i];
                    dr += (double)out[2 * i + 1] * out[2 * i + 1];
                    ddiff += (double)(out[2 * i] - out[2 * i + 1]) * (out[2 * i] - out[2 * i + 1]);
                }
            }
            printf("  chorus %s: L rms %.0f, R rms %.0f, L-R rms %.0f\n", k ? "I" : "OFF", sqrt(dl / FS), sqrt(dr / FS),
                   sqrt(ddiff / FS));
            if (!k)
                check("chorus OFF, PAN centre: left and right the same", ddiff == 0.0 && dl > 0.0);
            else
                check("chorus I: left and right differ (L-R at least a tenth of L), as loud", sqrt(ddiff) > 0.1 * sqrt(dl) &&
                      fabs(10.0 * log10(dl / dr)) < 1.5);
        }
    }
    {
        setup(0);
        trk[0].p[P_P6MODE] = P6M_4ENV;
        for (i = 0; i < 8u; i++)
            trk_note_on(&trk[0], 48 + 3 * i, 100);
        ok = held() == 4u;
        setup(0);
        trk[0].p[P_P6MODE] = P6M_8ENV;
        for (i = 0; i < 8u; i++)
            trk_note_on(&trk[0], 48 + 3 * i, 100);
        check("VOICES 4: four voices of eight notes; 8: eight", (int)ok && held() == 8u);
    }
    {
        voice_t v = {0};
        setup(0);
        trk[0].p[P_P6MODE] = P6M_4GATE;
        v.gate = 1;
        ok = poly6_amp(&trk[0], &v, 1000) == 32767;
        v.gate = 0;
        ok &= poly6_amp(&trk[0], &v, 20000) == 0;
        trk[0].p[P_P6MODE] = P6M_4ENV;
        check("GATE: the amp full while held, nothing after; ENV: the ADSR", (int)ok && poly6_amp(&trk[0], &v, 20000) == 20000);
    }

    /* 5. every preset */
    {
        static int32_t b[CTL], out[2 * CTL];
        uint32_t pi, b2;
        for (pi = 0, ok = 1; pi < ENG_POLY6.npresets; pi++) {
            double sum = 0, e2 = 0;
            int32_t vmax = 0, omax = 0;
            setup(pi);
            trk[0].p[P_P6MODE] = P6M_8ENV;
            trk[0].p[P_VOICE] = V_POLY;
            for (i = 0; i < 8u; i++)
                trk_note_on(&trk[0], 48 + 4 * i, 127);
            for (b2 = 0; b2 < 3u * FS / CTL; b2++) {
                memset(b, 0, sizeof b);
                track_render(&trk[0], b, CTL);
                for (i = 0; i < CTL; i++) {
                    if (b2 >= FS / CTL)                        /* (held: the last 2 s, the LFO's cycles in it) */
                        sum += b[i];
                    e2 += (double)b[i] * b[i];
                    vmax = abs(b[i]) > vmax ? abs(b[i]) : vmax;
                }
            }
            setup(pi);
            trk[0].p[P_LEVEL] = 127;
            for (i = 0; i < 8u; i++)
                trk_note_on(&trk[0], 48 + 4 * i, 127);
            for (b2 = 0; b2 < FS / CTL; b2++) {
                mix_block(out, CTL);
                for (i = 0; i < 2u * CTL; i++)
                    omax = abs(out[i]) > omax ? abs(out[i]) : omax;
            }
            {
                double mean = sum / (2.0 * FS), rms = sqrt(e2 / (3.0 * FS));
                int pok = rms > 300.0 && fabs(mean) < 0.01 * VOICE_FS * 8.0 && vmax <= 2 * VOICE_FS * 8 && omax < 32767;
                if (!pok)
                    printf("  %s: rms %.0f mean %.0f peak %d mix peak %d\n", POLY6_PRESETS[pi].name, rms, mean, vmax, omax);
                ok &= pok;
            }
        }
        check("all 32 presets: an 8-note chord sounds, no DC, no voice sum or mix sample at full scale", (int)ok &&
              ENG_POLY6.npresets == 32u);
    }

    /* 6. cost */
    {
        static int32_t b[CTL];
        double t_an, t_p6, t_p6c, est8, est4;
        uint32_t rep, b2;
        uint64_t t0;
        struct { uint32_t e, pi, cho; double *t; } R[3] = {{0, 0, 0, &t_an}, {ENGI_POLY6, 0, 0, &t_p6}, {ENGI_POLY6, 0, 1, &t_p6c}};
        for (k = 0; k < 3u; k++) {
            double best = 1e30;
            for (rep = 0; rep < 5u; rep++) {
                memset(trk, 0, sizeof trk);
                host_tracks_init();
                host_preset(&trk[0], R[k].e, R[k].pi);
                trk[0].p[P_VOICE] = V_POLY;
                if (R[k].e == ENGI_POLY6) {
                    trk[0].p[P_P6MODE] = P6M_8ENV;
                    trk[0].p[P_P6CHO] = R[k].cho ? P6C_I : P6C_OFF;
                    T_SPREAD(&trk[0]) = R[k].cho != 0;
                }
                for (i = 0; i < 8u; i++)
                    trk_note_on(&trk[0], 48 + 3 * i, 100);
                t0 = now_ns();
                for (b2 = 0; b2 < 2u * FS / CTL; b2++) {
                    memset(b, 0, sizeof b);
                    track_render(&trk[0], b, CTL);
                }
                best = fmin(best, (double)(now_ns() - t0));
            }
            *R[k].t = best;
        }
        est8 = t_p6c / t_an * 163.0 * 8.0 * 0.017;   /* % of the device: 8 voices */
        est4 = (t_p6 / t_an * 163.0 * 4.0 + (t_p6c - t_p6) / t_an * 163.0 * 8.0) * 0.017;   /* 4 voices + the chorus */
        printf("  cost (host time, 8 voices): ANALOG 1.00, POLY6 %.2f, POLY6 with its chorus %.2f\n", t_p6 / t_an, t_p6c / t_an);
        printf("  device estimate (ANALOG 163 instr / voice / sample, 1.7 %% per 100): POLY6 8 voices + chorus %.1f %%, "
               "4 voices + chorus %.1f %%\n", est8, est4);
        check("cost: POLY6 under 2.5 x ANALOG a voice (an estimate on this host)", t_p6c / t_an < 2.5);
    }

    /* 7. demos */
    if (demo) {
        static int32_t out[2 * CTL];
        uint32_t pi, b2;
        char path[512];
        for (pi = 0; pi < ENG_POLY6.npresets; pi++) {
            FILE *f;
            snprintf(path, sizeof path, "%s/%02u_%s.wav", demo, pi + 1u, POLY6_PRESETS[pi].name);
            for (i = 0; path[i]; i++)
                if (path[i] == ' ')
                    path[i] = '_';
            if (!(f = fopen(path, "wb")))
                continue;
            wav_hdr(f, 3u * FS);
            setup(pi);
            for (i = 0; i < 4u; i++)
                trk_note_on(&trk[0], (POLY6_PRESETS[pi].mono ? 36 : 57) + (int)(i * 4u - (i > 1u)), 100);
            for (b2 = 0; b2 < 3u * FS / CTL; b2++) {
                if (b2 == 2u * FS / CTL)
                    for (i = 0; i < 4u; i++)
                        trk_note_off(&trk[0], (POLY6_PRESETS[pi].mono ? 36 : 57) + (int)(i * 4u - (i > 1u)));
                mix_block(out, CTL);
                for (i = 0; i < CTL; i++)
                    wav_put(f, out[2 * i], out[2 * i + 1]);
            }
            fclose(f);
        }
        printf("poly6: demos in %s\n", demo);
    }
    printf(bad ? "poly6: FAILED\n" : "poly6: all ok\n");
    return bad != 0;
}
