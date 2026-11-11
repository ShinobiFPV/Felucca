/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 William Kew, ShinTech Electronics */
/* ShinTech CHORD styles (firmware/src/style.c, build/gen/chord_styles.h) against the real keys, MIDI IN, voices and
 * MIDI OUT: the style data (9 x 12, every event inside its loop), an arp's notes at their steps at 90 / 120 / 150 BPM
 * (within one block), SWING on a straight rate and not on a triplet one, a strum's notes STRUM ms apart (the
 * variation's own, the knob's; down low to high, up high to low), lengths, a key let go stops it, CHOLD keeps it
 * playing until another chord or CHOLD off, a chord changed while it plays strikes again what still sounds, VAR
 * changed on the go, STYLE OFF stops it, PLAY restarts it, the external clock's pulses step it (six a 1/16) and its
 * Stop holds it, MIDI IN plays it, CHOLD with STYLE OFF holds a chord, and every one of the 108 variations over two
 * bars: notes in 0..127, MIDI OUT balanced, nothing left sounding.
 * Run by tests/run_tests.sh (needs build/gen from one firmware build). */
#define UI_TEST_NO_MAIN 1
#include "ui_test.c"
static void eng(uint32_t e) { set_engine_of(TSEL, e); }

static int bad;
static int ck(const char *what, int ok)
{
    printf("style: %-96s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
    return ok;
}

/* MIDI OUT as the test reads it: each note-on / off with the sample it went out in (a block's start) */
typedef struct { uint32_t at; uint8_t on, note, vel; } mev_t;
static mev_t ev[4096];
static uint32_t nev, now;
static void drain(void)
{
    while (mo_r != mo_w) {
        uint32_t p = midi_out_q[mo_r++ % MQ], st = (p >> 8) & 0xF0u;
        if ((st == 0x90u || st == 0x80u) && nev < 4096u)
            ev[nev++] = (mev_t){now, (uint8_t)(st == 0x90u), (uint8_t)((p >> 16) & 0x7Fu), (uint8_t)((p >> 24) & 0x7Fu)};
    }
}
static void run_for(uint32_t samples)              /* run this long, block by block */
{
    uint32_t b;
    for (b = 0; b < samples / CTL; b++) {
        events_block(CTL);
        now += CTL;
        drain();
    }
}
static void clear_log(void) { drain(); nev = 0; }
static uint32_t ons(void)
{
    uint32_t i, n = 0;
    for (i = 0; i < nev; i++)
        n += ev[i].on;
    return n;
}
static const mev_t *on_k(uint32_t k)              /* the k-th note-on */
{
    uint32_t i;
    for (i = 0; i < nev; i++)
        if (ev[i].on && !k--)
            return &ev[i];
    return 0;
}
static uint32_t off_after(uint32_t i0, uint32_t note)   /* the sample the note-on at ev[i0]'s note ends */
{
    uint32_t i;
    for (i = i0 + 1u; i < nev; i++)
        if (ev[i].note == note)
            return ev[i].on ? 0xFFFFFFFFu : ev[i].at;
    return 0xFFFFFFFFu;
}
static int balanced(void)                         /* every note-on has its off */
{
    int32_t d[128] = {0};
    uint32_t i;
    for (i = 0; i < nev; i++)
        d[ev[i].note] += ev[i].on ? 1 : -1;
    for (i = 0; i < 128u; i++)
        if (d[i])
            return 0;
    return 1;
}
static uint32_t sounding(void)
{
    uint32_t i, n = 0;
    for (i = 0; i < NVOICE; i++)
        n += trk[0].v[i].active && trk[0].v[i].gate;
    return n;
}
static int near(uint32_t a, uint32_t b) { return (a > b ? a - b : b - a) <= CTL; }   /* within one block */

static uint32_t set_of(const char *name)
{
    uint32_t k;
    for (k = 0; k < CS_NSETS && strncmp(CS_SET[k].name, name, CS_NAME_LEN); k++)
        ;
    return k;
}
static uint32_t var_of(uint32_t style, const char *name)  /* style 1..9's variation named name, 0..11 */
{
    uint32_t k;
    for (k = 0; k < CST_NVAR && strncmp(CST_VAR[(style - 1u) * CST_NVAR + k].name, name, CST_NAME_LEN); k++)
        ;
    return k;
}
static void reset(void)
{
    ui_power_on();
    memset(kb_chn, 0, sizeof kb_chn); memset(mchord, 0, sizeof mchord); memset(chord_last, 0, sizeof chord_last);
    memset(chord_vl, 0, sizeof chord_vl); memset(sty, 0, sizeof sty);
    fm1_in.notes = kb_prev = 0; mo_w = mo_r = 0; usb.config = 1; kb_single = 0; kb_sty = 0;
    eng(0);                                                     /* ANALOG */
    trk[0].p[P_VOICE] = V_POLY;
    trk[0].p[P_CHRD] = CH_SET;
    trk[0].p[P_CSET] = (int16_t)set_of("FIRST LIGHT");          /* C G/B Am F ..: C4 plays C E G (60 64 67) */
    trk[0].p[P_REL] = 0;
    song.g[G_BPM] = 120;
    song.g[G_SWING] = 0;
    events_block(CTL);
    nev = 0;
    now = 0;
}
static void style(uint32_t s, const char *var)
{
    trk[0].p[P_CSTY] = (int16_t)s;
    trk[0].p[P_CVAR] = (int16_t)var_of(s, var);
}
#define K(n) ((uint32_t)(n) - 53u)

/* an arp of one note a step: note-ons at k * step (from the press), notes, velocities, lengths */
static void arp_check(const char *what, uint32_t bpm, uint32_t sw, uint32_t step, const int *notes)
{
    uint32_t k, ok = 1, sw_s = sw * step / 250u;
    char b[200];
    reset();
    song.g[G_BPM] = (int16_t)bpm;
    trk[0].p[P_SSWING] = (int16_t)sw;
    style(1, "UP 1/16");
    key_down(K(60));
    drain();
    run_for(9u * step);
    for (k = 0; k < 8u; k++) {
        const mev_t *e = on_k(k);
        uint32_t want = k * step + ((k & 1u) ? sw_s : 0u);
        ok &= e && near(e->at, want) && e->note == (uint8_t)notes[k % 3u] &&
              e->vel == (uint8_t)(k % 4u == 0u ? 112u : k % 4u == 2u ? 96u : 84u);
        ok &= e && near(off_after((uint32_t)(e - ev), e->note), e->at + (step * 3u / 4u));
    }
    snprintf(b, sizeof b, "%s: 8 notes C E G C .. at k x %u samples%s, 112 84 96 84, 3/4 of a step long", what, step,
             sw ? " + SWING on the odd ones" : "");
    if (!ck(b, (int)ok))
        for (k = 0; k < 8u && on_k(k); k++)
            printf("  on %u note %u vel %u, off %u\n", on_k(k)->at, on_k(k)->note, on_k(k)->vel,
                   off_after((uint32_t)(on_k(k) - ev), on_k(k)->note));
    key_up(K(60));
}

int main(void)
{
    track_t *t = &trk[0];
    uint32_t i, k, ok, n;

    /* the data */
    for (i = 0, ok = 1, n = 0; i < CST_NSTY * CST_NVAR; i++) {
        const cst_var_t *v = &CST_VAR[i];
        ok &= v->nev >= 1u && v->nev <= 32u && v->steps >= 1u && v->steps <= 64u && v->rate < 9u && v->oct >= 1u &&
              v->oct <= 3u && v->strum <= 63u && v->ev + v->nev <= CST_NEV && v->name[0];
        for (k = 0; k < v->nev; k++) {
            const uint8_t *e = CST_EV[v->ev + k];
            ok &= e[0] < v->steps && (k == 0u || CST_EV[v->ev + k - 1u][0] <= e[0]) && e[2] >= 1u && e[2] <= 127u &&
                  ((e[1] & 63u) <= 23u || ((e[1] & 63u) >= SS_A && (e[1] & 63u) <= SS_U) ||
                   ((e[1] & 63u) >= SS_UP && (e[1] & 63u) <= SS_RND)) && (e[3] >> 6) != 3u;
        }
        n += v->nev;
    }
    ck("9 styles x 12 variations, each 1..32 events inside its 1..64 steps, by step; known rates and notes", ok &&
       n == CST_NEV && CST_NSTY == 9u && CST_NVAR == 12u);
    for (i = 0, ok = 1; i < CST_NSTY; i++)
        ok &= !strcmp(N_CSTY[i + 1u], i < 2u ? (i ? "ARP2" : "ARP1") : i < 5u ? (const char *[]){"BEAT1", "BEAT2", "BEAT3"}[i - 2u]
                                                                              : (const char *[]){"STRM1", "STRM2", "STRM3", "STRM4"}[i - 5u]);
    ck("STYLE's values: OFF, ARP1 ARP2 (arps), BEAT1..3 (beats), STRM1..4 (strums)", ok && !strcmp(N_CSTY[0], "OFF"));
    {
        char v[8];
        const char *u;
        param_format(&TP[P_CVAR], 11, v, &u);
        ok = !strcmp(v, "12");
        param_format(&TP[P_CSTRM], 0, v, &u);
        ok &= !strcmp(v, "AUTO");
        param_format(&TP[P_CSTRM], 25, v, &u);
        ck("VAR shows 1..12; STRUM AUTO, 25ms", ok && !strcmp(v, "25") && !strcmp(u, "ms"));
    }

    /* timing: tempo, swing, triplets */
    arp_check("ARP1 UP 1/16 at 120 BPM", 120, 0, 22050u / 4u, (const int[]){60, 64, 67});
    arp_check("at 90 BPM", 90, 0, 29400u / 4u, (const int[]){60, 64, 67});
    arp_check("at 150 BPM", 150, 0, 17640u / 4u, (const int[]){60, 64, 67});
    arp_check("SWING 50 at 120 BPM", 120, 50, 22050u / 4u, (const int[]){60, 64, 67});
    reset();
    t->p[P_SSWING] = 50;
    style(1, "UP 16T");
    key_down(K(60));
    drain();
    run_for(7u * 3675u);
    for (k = 0, ok = 1; k < 6u; k++)
        ok &= on_k(k) && near(on_k(k)->at, k * 3675u);
    ck("UP 16T at 120 BPM: a note each 3675 samples, SWING 50 or not (a triplet rate is straight)", (int)ok);
    key_up(K(60));
    reset();
    song.g[G_SWING] = 30;                                        /* the global SWING too */
    style(1, "UP 1/16");
    key_down(K(60));
    drain();
    run_for(3u * 5512u);
    ck("the global SWING 30 swings it (track SWING 0): the second note 5512 + 661 late", on_k(1) && near(on_k(1)->at, 5512u + 661u));
    key_up(K(60));

    /* a strum */
    reset();
    style(6, "WHOLE");                                           /* strum 25 ms, D */
    key_down(K(60));
    drain();
    run_for(4000);
    ok = ons() == 3u && on_k(0)->note == 60u && on_k(1)->note == 64u && on_k(2)->note == 67u;
    ck("STRM1 WHOLE: a down strum, C E G low to high, 25 ms apart (1102 samples)",
       ok && near(on_k(0)->at, 0) && near(on_k(1)->at, 1102u) && near(on_k(2)->at, 2204u));
    ck(".. they sound together after", sounding() == 3u);
    run_for(8u * 11025u);
    ck(".. a bar long, then struck again", ons() == 6u && near(on_k(3)->at, 88200u));
    key_up(K(60));
    ck("the key let go: every note ends, MIDI OUT balanced", sounding() == 0u && (drain(), balanced()));
    reset();
    style(6, "WHOLE");
    t->p[P_CSTRM] = 10;
    key_down(K(60));
    drain();
    run_for(2000);
    ck("STRUM 10ms: 441 samples apart", ons() == 3u && near(on_k(1)->at, 441u) && near(on_k(2)->at, 882u));
    key_up(K(60));
    reset();
    style(6, "UPSTROKES");
    key_down(K(60));
    drain();
    run_for(2000);
    ck("STRM1 UPSTROKES: up, high to low (G E C)", ons() == 3u && on_k(0)->note == 67u && on_k(1)->note == 64u &&
                                                   on_k(2)->note == 60u && near(on_k(1)->at, 18u * 441u / 10u));
    key_up(K(60));

    /* release, CHOLD, a chord change */
    reset();
    style(1, "UP 1/16");
    key_down(K(60));
    run_for(3u * 5512u);
    key_up(K(60));
    drain();
    n = ons();
    run_for(4u * 5512u);
    ck("let go: it stops, nothing more plays, nothing left", ons() == n && sounding() == 0u && balanced());
    reset();
    style(1, "UP 1/16");
    t->p[P_CHOLD] = 1;
    key_down(K(60));
    key_up(K(60));
    run_for(4u * 5512u);
    ck("CHOLD: let go, it plays on", ons() >= 4u && sty[0].on);
    key_down(K(61));                                             /* G/B: B D G (59 62 67) */
    key_up(K(61));
    clear_log();
    run_for(3u * 5512u);
    ck(".. another chord takes over (G/B: B D G)", ons() >= 2u && on_k(0)->note != 60u && on_k(0)->note != 64u);
    t->p[P_CHOLD] = 0;
    run_for(CTL * 2u);
    ck(".. CHOLD off: it stops, nothing left, MIDI OUT balanced", !sty[0].on && sounding() == 0u && (drain(), 1));
    reset();
    style(5, "PAD BAR");
    key_down(K(60));
    run_for(4u * 5512u);
    ck("BEAT3 PAD BAR: the chord held", sounding() == 3u && ons() == 3u);
    clear_log();
    key_down(K(63));                                             /* F, C still held: C F A (60 65 69) */
    run_for(CTL);
    ck(".. F pressed on the way: C E G end, C F A strike at once, unstrummed",
       sounding() == 3u && ons() == 3u && on_k(0)->at == on_k(2)->at);
    key_up(K(60));
    run_for(CTL);
    ck(".. C let go (no longer the style's chord): it plays on", sty[0].on && sounding() == 3u);
    key_up(K(63));
    run_for(CTL);
    ck(".. F let go: it stops", !sty[0].on && sounding() == 0u);

    /* VAR, STYLE OFF, PLAY */
    reset();
    style(1, "UP 1/16");
    key_down(K(60));
    run_for(2u * 5512u + 100u);
    t->p[P_CVAR] = (int16_t)var_of(1, "DOWN 1/16");
    clear_log();
    run_for(5512u);
    ck("VAR changed while it plays: on at the same step, DOWN", sty[0].var == var_of(1, "DOWN 1/16") && ons() == 1u &&
                                                              sty[0].step == 3u);
    t->p[P_CSTY] = 0;
    run_for(CTL);
    ck("STYLE OFF: it stops, nothing left", !sty[0].on && sounding() == 0u);
    key_up(K(60));
    style(1, "UP 1/16");
    key_down(K(60));
    run_for(5512u + 2000u);
    transport_req = 1;
    clear_log();
    run_for(CTL * 2u);
    if (!ck("PLAY: a style playing starts again at its step 1 with the sequencer", song.playing && sty[0].step == 0u &&
                                                                               ons() >= 1u && on_k(0)->note == 60u))
        for (k = 0; k < nev; k++)
            printf("  at %u %s %u vel %u\n", ev[k].at, ev[k].on ? "on" : "off", ev[k].note, ev[k].vel);
    key_up(K(60));
    transport_req = 2;
    run_for(CTL);

    /* the external clock (USB): six pulses a 1/16 step */
    reset();
    song.g[G_CLOCK] = 1;
    events_block(CTL);
    style(1, "UP 1/16");
    key_down(K(60));
    run_for(CTL);
    fm1_ms = 1000;
    midi_enqueue(0xFu | 0xFAu << 8, 1);                          /* Start */
    run_for(CTL);
    clear_log();
    for (k = 0; k < 48u; k++) {                                  /* two beats at 120 BPM: a pulse each 20.8 ms */
        fm1_ms = 1000u + (k + 1u) * 125u / 6u;
        midi_enqueue(0xFu | 0xF8u << 8, 1);
        run_for(917u);                                            /* (20.8 ms) */
    }
    n = ons();
    if (!ck("external clock: 48 pulses (two beats) step it 8 times, 6 pulses a step", n >= 7u && n <= 9u))
        printf("  %u note-ons, step %u, playing %u, beat %u\n", n, sty[0].step, song.playing, beat_samples());
    midi_enqueue(0xFu | 0xFCu << 8, 1);                          /* Stop */
    run_for(CTL);
    k = sty[0].step;
    clear_log();
    run_for(4u * 5512u);
    ck(".. Stop: it stays on its step, its notes end", sty[0].step == k && ons() == 0u && sounding() == 0u);
    key_up(K(60));
    song.g[G_CLOCK] = 0;

    /* MIDI IN, CHOLD with STYLE OFF */
    reset();
    style(1, "UP 1/8");
    midi_enqueue(0x9u | 0x90u << 8 | 60u << 16 | 100u << 24, 1);
    run_for(CTL);
    run_for(2u * 11025u);
    ck("MIDI IN C: the style plays chord 1 (C E G)", ons() >= 2u && on_k(0)->note == 60u && on_k(1)->note == 64u);
    midi_enqueue(0x8u | 0x80u << 8 | 60u << 16, 1);
    run_for(CTL * 2u);
    ck(".. its note-off: it stops, nothing left, MIDI OUT balanced", !sty[0].on && sounding() == 0u && (drain(), balanced()));
    reset();
    t->p[P_CHOLD] = 1;
    key_down(K(60));
    key_up(K(60));
    run_for(CTL);
    ck("CHOLD, STYLE OFF: C4 let go, C E G sound on", sounding() == 3u);
    key_down(K(63));
    run_for(CTL);
    ck(".. F4: C E G end, C F A sound", sounding() == 3u && trk[0].v[0].note != 0);
    key_up(K(63));
    t->p[P_CHOLD] = 0;
    run_for(CTL);
    ck(".. CHOLD off: nothing left", sounding() == 0u);

    /* every variation, two bars on a four-note chord (Dm7) */
    for (i = 0, ok = 1; i < CST_NSTY * CST_NVAR; i++) {
        reset();
        t->p[P_CSTY] = (int16_t)(i / CST_NVAR + 1u);
        t->p[P_CVAR] = (int16_t)(i % CST_NVAR);
        key_down(K(64));
        run_for(2u * 88200u);
        key_up(K(64));
        run_for(CTL * 2u);
        drain();
        if (!(ons() > 0u && balanced() && sounding() == 0u && !sty[0].nslot)) {
            printf("  %s %.*s: %u note-ons, balanced %d, %u left\n", N_CSTY[i / CST_NVAR + 1u], (int)CST_NAME_LEN,
                   CST_VAR[i].name, ons(), balanced(), sounding());
            ok = 0;
        }
    }
    ck("all 108 variations, two bars: notes played, MIDI OUT balanced, nothing left after", (int)ok);

    printf(bad ? "style: FAILED\n" : "style: all ok\n");
    return bad != 0;
}
