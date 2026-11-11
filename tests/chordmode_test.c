/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 William Kew, ShinTech Electronics */
/* ShinTech CHORD mode, CHRD SET (firmware/src/chord.c chord_set_make, seq.c cset_key, chordset.c) against the real
 * keyboard, MIDI IN, voices, MIDI OUT and UI: C4..B4 play the set's twelve chords, voiced in the RANGE window with the
 * voice leading from the chord before (exact notes); a release ends exactly what its key started; BASS adds the slash
 * bass below the window; the shortcut keys (F3..B3, C#5 D#5) change SET, VOICING, BASS, KEY and sound nothing; C5..G5's
 * white keys play their own note; MIDI IN plays chord n % 12; MONO plays the root; a kit ignores SET; MIDI OUT sends
 * every note; the CHORD SET and VOICING pages show with SET only, and leave to SCL when it goes.
 * Run by tests/run_tests.sh (needs build/gen from one firmware build). */
#define UI_TEST_NO_MAIN 1
#include "ui_test.c"
static void eng(uint32_t e) { set_engine_of(TSEL, e); }   /* (as tests/ui_render.c) */

static int bad;
static int ck(const char *what, int ok)
{
    printf("chordmode: %-86s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
    return ok;
}

/* the notes gated on track 0, ascending -> g, their number */
static uint32_t held_notes(uint8_t *g)
{
    uint32_t i, j, n = 0;
    for (i = 0; i < NVOICE; i++)
        if (trk[0].v[i].active && trk[0].v[i].gate)
            g[n++] = trk[0].v[i].note;
    for (i = 1; i < n; i++)
        for (j = i; j > 0 && g[j - 1u] > g[j]; j--) {
            uint8_t x = g[j];
            g[j] = g[j - 1u];
            g[j - 1u] = x;
        }
    return n;
}
/* exactly these notes gated (-1 ends); prints them */
static int gated_are(const char *what, const int *want)
{
    uint8_t g[NVOICE];
    uint32_t n = held_notes(g), i, ok = 1;
    char b[160], w[240];
    b[0] = 0;
    for (i = 0; i < n; i++)
        snprintf(b + strlen(b), sizeof b - strlen(b), "%s%u", i ? " " : "", g[i]);
    for (i = 0; want[i] >= 0; i++)
        ok &= i < n && g[i] == (uint8_t)want[i];
    ok &= i == n;
    snprintf(w, sizeof w, "%s: %s", what, b);
    return ck(w, (int)ok);
}
static uint32_t set_of(const char *name)
{
    uint32_t k;
    for (k = 0; k < CS_NSETS && strncmp(CS_SET[k].name, name, CS_NAME_LEN); k++)
        ;
    return k;
}
static void reset(void)
{
    ui_power_on();
    memset(kb_chn, 0, sizeof kb_chn); memset(mchord, 0, sizeof mchord); memset(chord_last, 0, sizeof chord_last);
    memset(chord_vl, 0, sizeof chord_vl);
    fm1_in.notes = kb_prev = 0; mo_w = mo_r = 0; usb.config = 1; kb_single = 0;
    eng(0);                                                     /* ANALOG */
    trk[0].p[P_VOICE] = V_POLY;
    trk[0].p[P_CHRD] = CH_SET;
    trk[0].p[P_CSET] = (int16_t)set_of("FIRST LIGHT");          /* C G/B Am F Dm7 Em7 Fmaj7 G Am7 Csus2 Fadd9 Gsus4 */
    events_block(CTL);
}
static void midi(uint32_t st, uint32_t d1, uint32_t d2)
{
    midi_enqueue(st >> 4 | st << 8 | d1 << 16 | d2 << 24, 1);
    events_block(CTL);
}
#define K(n) ((uint32_t)(n) - 53u)                          /* the key of MIDI note n (F3 = 53 is key 0) */

int main(void)
{
    uint8_t g[NVOICE];
    track_t *t = &trk[0];
    uint32_t k, n, ok, before;
    reset();
    ck("FIRST LIGHT is a factory set; CHRD SET on track 1", set_of("FIRST LIGHT") < CS_NSETS && chord_set_on(t));

    /* the chord keys, the window (RANGE G3: 55 .. 66), voice leading */
    key_down(K(60));
    gated_are("C4: chord 1 (C), the first chord in its default inversion, lowest in G3 .. F#4", (const int[]){60, 64, 67, -1});
    key_up(K(60));
    ck("its release: nothing left", held_notes(g) == 0u);
    key_down(K(61));
    gated_are("C#4: chord 2 (G/B, no BASS), led from C E G: B D G", (const int[]){59, 62, 67, -1});
    key_up(K(61));
    key_down(K(62));
    gated_are("D4: chord 3 (Am), led from B D G: C E A (10 semitones; A C E 11)", (const int[]){60, 64, 69, -1});
    key_up(K(62));
    key_down(K(63));
    gated_are("D#4: chord 4 (F), led from C E A: C F A (one voice moves one semitone)", (const int[]){60, 65, 69, -1});
    {
        char b[CS_NAME_MAX];
        cs_name(b, chord_last[0].root % 12u, chord_last[0].mask, chord_last[0].bass);
        ck("the screen's chord: F, a SET chord", chord_last[0].set && !strcmp(b, "F"));
    }
    key_up(K(63));
    for (k = 0, ok = 1; k < 12u; k++) {                         /* every chord: its own pitch classes, in the window */
        cs_chord_t c;
        uint32_t i, j;
        cs_get((uint32_t)t->p[P_CSET], k, &c);
        key_down(K(60 + k));
        n = held_notes(g);
        ok &= n == c.n && g[0] >= 55u && g[0] <= 66u;
        for (i = 0; i < n; i++) {
            uint32_t hit = 0;
            for (j = 0; j < c.n; j++)
                hit |= (g[i] + 12u - c.root) % 12u == (uint32_t)c.iv[j] % 12u;
            ok &= hit;
        }
        key_up(K(60 + k));
        ok &= held_notes(g) == 0u;
    }
    ck("all twelve keys: each its chord's notes, the lowest in the window, nothing left after", ok);
    key_down(K(60));
    key_down(K(69));
    n = held_notes(g);
    key_up(K(60));
    ck("two chord keys held (C, Am7), the first let go: the second's notes sound, the first's shared ones too",
       n >= 4u && held_notes(g) >= 3u);
    key_up(K(69));
    ck(".. both up: nothing", held_notes(g) == 0u);

    /* BASS, KEY, VOICING, SET by the shortcut keys: silent */
    before = mo_w;
    key_down(K(58));                                             /* A#3: BASS on */
    ck("A#3: BASS on, silent (no voice, no MIDI OUT)", t->p[P_CBASS] == 1 && held_notes(g) == 0u && mo_w == before);
    key_up(K(58));
    memset(chord_vl, 0, sizeof chord_vl);
    key_down(K(61));
    gated_are("C#4 with BASS (G/B), a first chord: B2 below the window, then G3 B3 D4", (const int[]){47, 55, 59, 62, -1});
    key_up(K(61));
    key_down(K(58));
    key_up(K(58));
    ck("A#3 again: BASS off", t->p[P_CBASS] == 0);
    key_down(K(75));                                             /* D#5: KEY + */
    key_up(K(75));
    memset(chord_vl, 0, sizeof chord_vl);
    key_down(K(60));
    gated_are("D#5: KEY C#; C4 now plays C# (C# F G#), in the window", (const int[]){61, 65, 68, -1});
    key_up(K(60));
    key_down(K(73));                                             /* C#5: KEY - */
    key_up(K(73));
    key_down(K(73));
    key_up(K(73));
    ck("C#5 twice: KEY B (round the twelve)", t->p[P_CKEY] == 11);
    t->p[P_CKEY] = 0;
    key_down(K(56));                                             /* G#3: VOICING AUTO -> CLOSE */
    key_up(K(56));
    ck("G#3: VOICING AUTO -> CLOSE", t->p[P_CVOIC] == CSV_CLOSE);
    t->p[P_CVOIC] = CSV_AUTO;
    t->p[P_CSET] = 0;
    key_down(K(53));                                             /* F3: SET - from the first: the last */
    key_up(K(53));
    ck("F3 on the first set: the last one (round)", t->p[P_CSET] == (int16_t)(CS_NSETS - 1u));
    key_down(K(55));                                             /* G3: SET + */
    key_up(K(55));
    ck("G3: SET + (back to the first)", t->p[P_CSET] == 0);
    before = mo_w;
    for (k = 53; k <= 59; k++)                                   /* F3 .. B3, all seven */
        key_down(K(k));
    ck("F3 .. B3 held at once: silent", held_notes(g) == 0u && mo_w == before);
    for (k = 53; k <= 59; k++)
        key_up(K(k));
    t->p[P_CSET] = (int16_t)set_of("FIRST LIGHT");
    t->p[P_CBASS] = 0;

    /* single notes */
    key_down(K(76));
    gated_are("E5: its own note, no chord", (const int[]){76, -1});
    key_down(K(60));
    n = held_notes(g);
    ck("E5 held, then C4: the chord and E5", n == 4u && g[3] == 76u);
    key_up(K(60));
    key_up(K(76));
    ck(".. both up: nothing", held_notes(g) == 0u);

    /* MIDI OUT: every note of a chord, balanced */
    mo_w = mo_r = 0;
    memset(chord_vl, 0, sizeof chord_vl);
    key_down(K(60));
    key_up(K(60));
    ck("MIDI OUT: three note-ons, three note-offs for C4's chord", mo_w == 6u && ((midi_out_q[0] >> 8) & 0xF0u) == 0x90u &&
                                                                    ((midi_out_q[5] >> 8) & 0xF0u) == 0x80u);

    /* MIDI IN: chord n % 12 */
    memset(chord_vl, 0, sizeof chord_vl);
    midi(0x90, 38, 100);                                         /* D1: chord 3, Am */
    gated_are("MIDI IN D1 (any octave): chord 3 (Am) in the window", (const int[]){57, 60, 64, -1});
    midi(0x80, 38, 0);
    ck(".. its note-off: nothing left", held_notes(g) == 0u);

    /* MONO, a kit */
    t->p[P_VOICE] = V_MONO;
    memset(chord_vl, 0, sizeof chord_vl);
    key_down(K(62));
    gated_are("MONO: chord 3's root only (A in the window)", (const int[]){57, -1});
    key_up(K(62));
    t->p[P_VOICE] = V_POLY;
    eng(ENGI_DRUM);
    t->p[P_CHRD] = CH_SET;
    events_block(CTL);
    ck("on a kit (DRUM) SET does nothing (chord_set_on 0)", !chord_set_on(t));
    key_down(K(60));
    ck(".. C4 plays the kit's own key, one hit", held_notes(g) <= 1u);
    key_up(K(60));
    reset();

    /* the pages */
    {
        uint32_t i, set_page = NPAGES, voic_page = NPAGES;
        for (i = 0; i < NPAGES; i++) {
            if (str_eq(PAGES[i].title, "CHORD SET")) set_page = i;
            if (str_eq(PAGES[i].title, "VOICING")) voic_page = i;
        }
        ck("CHORD SET and VOICING pages in the SCL family", set_page < NPAGES && voic_page < NPAGES &&
                                                           PAGES[set_page].fam == FAM_SCL && PAGES[voic_page].fam == FAM_SCL);
        ck("with CHRD SET both show", page_visible(set_page) && page_visible(voic_page));
        t->p[P_CHRD] = CH_DIA3;
        ck("with CHRD DIA3 neither", !page_visible(set_page) && !page_visible(voic_page));
        t->p[P_CHRD] = CH_SET;
        go_title("SCL");
        frame();
        press(B_SCL); frames(400);
        ck("SCL tapped on SCL: CHORD", str_eq(cur_page()->title, "CHORD"));
        press(B_SCL); frames(400);
        if (!str_eq(cur_page()->title, "CHORD SET")) printf("  (on %s)\n", cur_page()->title);
        ck("again: CHORD SET", str_eq(cur_page()->title, "CHORD SET"));
        press(B_SCL); frames(400);
        ck("again: VOICING", str_eq(cur_page()->title, "VOICING"));
        press(B_SCL); frames(400);
        ck("again: SCL (round)", str_eq(cur_page()->title, "SCL"));
        go_title("CHORD SET");
        frame();
        t->p[P_CHRD] = 0;
        frame();
        ck("CHRD switched off on CHORD SET: the page leaves for SCL", str_eq(cur_page()->title, "SCL"));
        t->p[P_CHRD] = CH_SET;
        {
            char v[16];
            const char *u;
            param_format(&TP[P_CSET], (int32_t)set_of("DEEP CLUB"), v, &u);
            ck("the SET knob names a set by its genre and number: DEEP CLUB is HSE1", !strcmp(v, "HSE1"));
            param_format(&TP[P_CSET], (int32_t)CS_NSETS - 1, v, &u);
            ck("  the last set: MIN8", !strcmp(v, "MIN8"));
        }
    }
    printf("chordmode: %s\n", bad ? "FAILED" : "all ok");
    return bad != 0;
}
