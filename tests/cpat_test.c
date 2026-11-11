/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 William Kew, ShinTech Electronics */
/* ShinTech CHORD patterns (firmware/src/cpat.c, cpat_store.c) against the real sequencer, keys, voices and MIDI OUT:
 * a pattern's chords at their steps (1/4 at 120 BPM: within one block), TIE and REST, a single note, a step's STYLE
 * (the style plays the chord) and tempo, a hand-played chord voiced as the set's are, LEN, NEXT, CPAT changed while
 * it plays, STOP, a SONG section's pattern, live recording (the chord into the step playing or the next past its half,
 * TIEs while held, a REST after; single notes; the track's own steps untouched), the cache (a pattern not loaded is
 * silent until cpat_poll loads it; an edit is written back once quiet), MIDI OUT balanced. Without flash (the bank in
 * RAM); the flash side: tests/cpat_store_test.c.
 * Run by tests/run_tests.sh (needs build/gen from one firmware build). */
#define UI_TEST_NO_MAIN 1
#include "ui_test.c"
static void eng(uint32_t e) { set_engine_of(TSEL, e); }

static int bad;
static int ck(const char *what, int ok)
{
    printf("cpat: %-98s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
    return ok;
}
static uint32_t notes_of(uint8_t *g)                    /* track 1's gated notes, ascending */
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
static int sounding_are(const char *what, const int *want)
{
    uint8_t g[NVOICE];
    uint32_t n = notes_of(g), i, ok = 1;
    char b[120], w[260];
    b[0] = 0;
    for (i = 0; i < n; i++)
        snprintf(b + strlen(b), sizeof b - strlen(b), "%s%u", i ? " " : "", g[i]);
    for (i = 0; want[i] >= 0; i++)
        ok &= i < n && g[i] == (uint8_t)want[i];
    ok &= i == n;
    snprintf(w, sizeof w, "%s: %s", what, b);
    return ck(w, (int)ok);
}
static uint32_t mo_on, mo_off;
static void drain(void)
{
    while (mo_r != mo_w) {
        uint32_t p = midi_out_q[mo_r++ % MQ], st = (p >> 8) & 0xF0u;
        mo_on += st == 0x90u;
        mo_off += st == 0x80u;
    }
}
static uint32_t now;
static void run_for(uint32_t samples)
{
    uint32_t b;
    for (b = 0; b < samples / CTL; b++) {
        events_block(CTL);
        now += CTL;
        drain();
    }
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
    memset(chord_vl, 0, sizeof chord_vl); memset(sty, 0, sizeof sty); memset(cpp, 0, sizeof cpp);
    memset(cp_ram, 0, sizeof cp_ram); memset(cp_bank, 0, sizeof cp_bank);
    fm1_in.notes = kb_prev = 0; mo_w = mo_r = 0; usb.config = 1; kb_single = 0; kb_sty = 0; cp_hand_in = 0;
    mo_on = mo_off = 0;
    eng(0);
    trk[0].p[P_VOICE] = V_POLY;
    trk[0].p[P_CHRD] = CH_SET;
    trk[0].p[P_CSET] = (int16_t)set_of("FIRST LIGHT");          /* 1 C, 2 G/B, 3 Am, 4 F, 5 Dm7, 6 Em7, .. */
    trk[0].p[P_REL] = 0;
    song.g[G_BPM] = 120;
    song.g[G_CLOCK] = 0;
    events_block(CTL);
    now = 0;
}
static void play(void) { transport_req = 1; cpat_poll(); run_for(CTL); run_for(11025u); }   /* (to mid-step 1) */
static void stop(void) { transport_req = 2; run_for(CTL); }
#define K(n) ((uint32_t)(n) - 53u)
#define BEAT 22050u                                          /* a quarter at 120 BPM */

int main(void)
{
    track_t *t = &trk[0];
    cpat_t *p;
    uint8_t g[NVOICE];
    uint32_t i, ok;

    ck("a pattern is 328 bytes; zero is the empty one: LEN 16, 1/4, every step a REST", sizeof(cpat_t) == 328u &&
       cp_len(&(cpat_t){0}) == 16u);

    /* a pattern: C, TIE, F + a note, REST */
    reset();
    p = &cp_bank[0];
    p->len = 4;
    p->step[0].chord = CPC_SET + 0;                              /* C */
    p->step[1].chord = CPC_TIE;
    p->step[2].chord = CPC_SET + 3;                              /* F */
    p->step[2].note = 84;                                        /* C6 over it */
    t->p[P_CPAT] = 1;
    play();
    sounding_are("PLAY: step 1, chord 1 (C) at once", (const int[]){60, 64, 67, -1});
    run_for(BEAT);
    sounding_are(".. step 2 (TIE, a beat later): C sounds on", (const int[]){60, 64, 67, -1});
    run_for(BEAT);
    sounding_are(".. step 3: F (C F A, led from C E G) and the single note C6", (const int[]){60, 65, 69, 84, -1});
    run_for(BEAT);
    ck(".. step 4: REST, nothing", notes_of(g) == 0u);
    run_for(BEAT);
    sounding_are(".. LEN 4: step 1 again", (const int[]){60, 64, 67, -1});
    {
        uint32_t at, s0 = cpp[0].step, lim = now + 3u * BEAT;   /* steps on the beat, within a block */
        while (cpp[0].step == s0 && now < lim)
            run_for(CTL);
        at = now;
        s0 = cpp[0].step;
        while (cpp[0].step == s0 && now < lim)
            run_for(CTL);
        ck("the steps a beat apart at 120 BPM, within a block", now - at <= BEAT + CTL && now - at + CTL >= BEAT);
    }
    stop();
    drain();
    ck("STOP: its notes end, MIDI OUT balanced (every note it played went out)", notes_of(g) == 0u && mo_on == mo_off &&
                                                                                   mo_on >= 9u);

    /* a step's style and tempo */
    reset();
    p = &cp_bank[0];
    p->len = 2;
    p->step[0].chord = CPC_SET + 0;
    p->step[0].style = (uint8_t)(2u + 4u * CST_NVAR + 5u);       /* BEAT3 (style 5) var 6: PAD BAR */
    p->step[1].chord = CPC_TIE;
    p->step[1].tempo = 100u - 39u;                               /* BPM 100 */
    t->p[P_CPAT] = 1;
    play();
    ck("a step's STYLE: the track's STYLE / VAR now BEAT3 6, the style plays the chord",
       t->p[P_CSTY] == 5 && t->p[P_CVAR] == 5 && sty[0].on && sty[0].key == CP_SRC && notes_of(g) == 3u);
    run_for(BEAT);
    ck("a step's tempo: BPM 100 from step 2; the style plays on under the TIE", song.g[G_BPM] == 100 && sty[0].on);
    stop();
    ck("STOP: the style stops", !sty[0].on && notes_of(g) == 0u);

    /* a hand-played chord */
    reset();
    p = &cp_bank[0];
    cp_hand_of((const uint8_t[]){62, 65, 69, 72}, 4, &p->hand[0]);   /* D F A C as played: Dm7 */
    p->nhand = 1;
    p->len = 1;
    p->step[0].chord = CPC_HAND + 0;
    t->p[P_CPAT] = 1;
    play();
    {
        uint32_t n = notes_of(g), pcs = 0;
        for (i = 0; i < n; i++)
            pcs |= 1u << (g[i] % 12u);
        ck("a hand chord (D F A C): voiced in the window, its pitch classes", n == 4u &&
           pcs == (1u << 2 | 1u << 5 | 1u << 9 | 1u << 0) && g[0] >= 55u && g[0] <= 66u);
        {
            char b[CS_NAME_MAX];
            cs_name(b, chord_last[0].root % 12u, chord_last[0].mask, chord_last[0].bass);
            ck(".. the screen names it Dm7", !strcmp(b, "Dm7"));
        }
    }
    {
        cp_hand_t h;
        cpat_t q = {0};
        cp_hand_of((const uint8_t[]){74, 77, 81, 84}, 4, &h);      /* (an octave higher: the same chord) */
        q.nhand = 0;
        ok = cp_hand_add(&q, &p->hand[0]) == 0 && cp_hand_add(&q, &h) == 0 && q.nhand == 1u;
        for (i = 1; i < 16u; i++) {
            cp_hand_t x = {(uint8_t)i, {1, 0, 0}};
            ok &= cp_hand_add(&q, &x) == (int)i;
        }
        cp_hand_t y = {0, {2, 0, 0}};
        ck("hand chords: the same one again its index, 16 at most", ok && cp_hand_add(&q, &y) < 0);
    }
    stop();

    /* NEXT, CPAT changed, the cache */
    reset();
    cp_bank[0].len = 1;
    cp_bank[0].step[0].chord = CPC_SET + 0;                      /* C */
    cp_bank[0].next = 2;
    cp_bank[1].len = 1;
    cp_bank[1].step[0].chord = CPC_SET + 2;                      /* Am */
    cp_bank[2].len = 1;
    cp_bank[2].step[0].chord = CPC_SET + 3;                      /* F */
    t->p[P_CPAT] = 1;
    play();
    sounding_are("pattern 1 (NEXT 2): C", (const int[]){60, 64, 67, -1});
    run_for(BEAT);
    ck(".. at its end pattern 2 (Am)", cpp[0].id == 2u && notes_of(g) == 3u && g[2] == 69u);
    run_for(BEAT);
    ck(".. pattern 2 (no NEXT) again", cpp[0].id == 2u);
    t->p[P_CPAT] = 3;
    run_for(CTL);
    ck("CPAT 3 while it plays, not in the cache: silent", cpp[0].id == 3u && notes_of(g) == 0u);
    cpat_poll();
    run_for(BEAT * 3u / 2u);
    ck(".. loaded (cpat_poll): F plays", cpp[0].id == 3u && notes_of(g) == 3u && g[1] == 65u);
    t->p[P_CPAT] = 0;
    run_for(CTL);
    ck("CPAT OFF: it stops, nothing left", !cpp[0].on && notes_of(g) == 0u);
    stop();
    t->p[P_CHRD] = CH_DIA3;
    t->p[P_CPAT] = 1;
    play();
    ck("CHRD not SET: no pattern plays", !cpp[0].on);
    stop();

    /* recording */
    reset();
    cp_bank[0].len = 8;
    t->p[P_CPAT] = 1;
    {
    static step_t own[NSTEP];
    t->step[0] = (step_t){{72}, 1, ST_NOTE, 0, 100};
    memcpy(own, t->step, sizeof own);
    song.rec = 1;
    transport_req = 1;
    cpat_poll();
    run_for(CTL * 11u);
    key_down(K(60));                                             /* C4: chord 1, early in step 1 */
    run_for(BEAT * 2u);                                          /* held over steps 2 and 3 */
    key_up(K(60));
    run_for(BEAT / 2u + CTL * 20u);                              /* (step 3, its second half) */
    key_down(K(63));                                             /* F4: chord 4, late in step 3: into step 4 */
    run_for(BEAT * 3u / 4u);
    key_up(K(63));
    key_down(K(79));                                             /* G5: a single note, early in step 4: into it */
    key_up(K(79));
    run_for(BEAT * 2u);
    p = cp_get(1);
    ck("recording: C into step 1, TIEs while held (2, 3), F into step 4 (pressed past step 3's half), REST after",
       p && p->step[0].chord == CPC_SET && p->step[1].chord == CPC_TIE && p->step[2].chord == CPC_TIE &&
       p->step[3].chord == CPC_SET + 3u && p->step[4].chord == CPC_REST);
    ck(".. the single note into step 4; the chords keep STYLE OFF; the track's set and KEY",
       p && p->step[3].note == 79u && p->step[0].style == 1u && p->set == t->p[P_CSET] + 1 && p->key == 1u);
    ck(".. the track's own steps untouched (rec_on: not while it records a pattern)", !memcmp(own, t->step, sizeof own));
    }
    song.rec = 0;
    stop();
    ck(".. edited: dirty in the cache, not in the bank yet", cp_ram[cp_find(1)].dirty && cp_bank[0].step[0].chord == 0u);
    fm1_ms += CP_IDLE_MS + 10u;
    cpat_poll();
    ck(".. quiet for 2 s: written back", !cp_ram[cp_find(1)].dirty && cp_bank[0].step[0].chord == CPC_SET &&
                                          cp_bank[0].step[3].chord == CPC_SET + 3u);
    play();
    sounding_are("the recorded pattern plays: C", (const int[]){60, 64, 67, -1});
    run_for(BEAT * 3u);
    sounding_are(".. step 4: F and G5", (const int[]){60, 65, 69, 79, -1});
    stop();

    /* a SONG section's pattern */
    reset();
    cp_bank[0].len = 1;
    cp_bank[0].step[0].chord = CPC_SET + 0;                      /* C */
    cp_bank[1].len = 1;
    cp_bank[1].step[0].chord = CPC_SET + 2;                      /* Am */
    t->p[P_SLEN] = 4;                                            /* (the section: one loop of track 1, 4 x 1/16) */
    t->p[P_SDIV] = 2;
    t->p[P_CPAT] = 1;
    project_save(0);
    t->p[P_CPAT] = 2;
    project_save(1);
    t->p[P_CPAT] = 0;
    chain_config.count = 2;
    chain_config.row[0] = chain_row_of(0, 1);
    chain_config.row[1] = chain_row_of(1, 1);
    ok = chain_prepare() == 0;
    cpat_poll();
    run_for(CTL * 2u);
    run_for(BEAT / 2u);
    ck("SONG: section 1 plays slot A's pattern (C), the track's CPAT OFF", ok && chain.running && cp_want(0) == 1u &&
                                                                         notes_of(g) == 3u && g[0] == 60u);
    run_for(BEAT);
    ck(".. section 2 slot B's (Am)", cp_want(0) == 2u && notes_of(g) == 3u && g[2] == 69u);
    stop();
    chain_config.count = 0;

    /* the pages */
    reset();
    {
        uint32_t pg[4] = {NPAGES, NPAGES, NPAGES, NPAGES};
        static const char *const TITLE[4] = {"CHORD PAT", "PAT STEP", "PAT SETUP", "PAT TOOLS"};
        uint32_t j;
        for (i = 0; i < NPAGES; i++)
            for (j = 0; j < 4u; j++)
                if (str_eq(PAGES[i].title, TITLE[j]))
                    pg[j] = i;
        ok = 1;
        for (j = 0; j < 4u; j++)
            ok &= pg[j] < NPAGES && PAGES[pg[j]].fam == FAM_SEQ && page_visible(pg[j]) && (!j || pg[j] == pg[j - 1u] + 1u);
        t->p[P_CHRD] = CH_DIA3;
        for (j = 0; j < 4u; j++)
            ok &= !page_visible(pg[j]);
        t->p[P_CHRD] = CH_SET;
        ck("CHORD PAT, PAT STEP, PAT SETUP, PAT TOOLS: SEQ's, in that order, with CHRD SET only", (int)ok);
        go_title("AUTOMATION");
        frame();
        press(B_SEQ); frames(400);
        ck("SEQ tapped on AUTOMATION: CHORD PAT", str_eq(cur_page()->title, "CHORD PAT"));
    }
    go_title("CHORD PAT");
    frame();
    ck("CPAT OFF: the panel says to pick one", !cpe_pat());
    turn(EN_K1, 2);
    ck("KNOB 1: CPAT 2 (the track's parameter)", t->p[P_CPAT] == 2 && cpe_pat() == cp_get(2));
    turn(EN_K1 + 1, 3);
    ck("KNOB 2: the cursor to step 4", cpe_cur == 3u);
    turn(EN_K1 + 2, 6);
    p = cp_get(2);
    ck("KNOB 3: step 4's chord 5 (REST TIE 1 2 3 4 5)", p && p->step[3].chord == CPC_SET + 4u);
    turn(EN_K1 + 3, 1);
    ck("KNOB 4: step 4's STYLE OFF (KEEP, OFF, ..)", p && p->step[3].style == 1u);
    ck(".. the pattern edited: dirty, the track's set and KEY its own", cp_ram[cp_find(2)].dirty && p->set == t->p[P_CSET] + 1);
    hold(B_SAVE);
    ck("SAVE held: the pattern back as before the last edit (STYLE KEEP), the chord stays", p->step[3].style == 0u &&
                                                                                        p->step[3].chord == CPC_SET + 4u);
    hold(B_SAVE);
    ck(".. held again: the edit again (redo)", p->step[3].style == 1u);
    go_title("PAT SETUP");
    frame();
    turn(EN_K1, -12);
    ck("PAT SETUP KNOB 1: LEN 4", cp_len(p) == 4u);
    turn(EN_K1 + 2, 3);
    ck("KNOB 3: NEXT 3", p->next == 3u);
    go_title("PAT STEP");
    frame();
    ck("PAT STEP: the cursor kept inside LEN (step 4)", cpe_cur == 3u);
    turn(EN_K1 + 1, 60);
    ck("KNOB 2: a single note (C1 first)", p->step[3].note >= 24u);
    turn(EN_K1 + 2, 81);
    ck("KNOB 3: the step's tempo 120", p->step[3].tempo == 81u);

    /* step entry: armed, stopped */
    go_title("CHORD PAT");
    frame();
    turn(EN_K1 + 1, -10);
    song.rec = 1;
    key_down(K(62)); frame(); key_up(K(62)); frame();          /* D4: chord 3 */
    key_down(K(63)); frame(); key_up(K(63)); frame();          /* D#4: chord 4 */
    ck("armed, stopped: chord keys write steps 1 and 2 (chords 3, 4), the cursor on", p->step[0].chord == CPC_SET + 2u &&
                                                                                      p->step[1].chord == CPC_SET + 3u &&
                                                                                      cpe_cur == 2u);
    key_down(K(76)); frame(); key_up(K(76)); frame();          /* E5: a note, the cursor stays */
    ck("C5..G5: the note into the cursor step, the cursor stays", p->step[2].note == 76u && cpe_cur == 2u);
    go_title("PAT SETUP");
    frame();
    turn(EN_K1 + 3, 1);
    ck("INPUT HAND", cp_hand_in == 1u);
    go_title("CHORD PAT");
    frame();
    key_down(K(62)); key_down(K(65)); key_down(K(69)); frame();   /* D F A together .. */
    key_down(K(72)); frame();                                  /* .. and C */
    ck(".. keys held: nothing written yet; they sound as notes", p->step[2].chord == 0u && notes_of(g) == 4u);
    key_up(K(62)); key_up(K(65)); key_up(K(69)); key_up(K(72)); frame();
    ck(".. let go: a hand chord (Dm7) on step 3, the cursor on", p->step[2].chord == CPC_HAND && p->nhand == 1u &&
                                                                  cpe_cur == 3u);
    {
        char b[CS_NAME_MAX];
        cpe_chord_name(t, p, &p->step[2], b);
        ck(".. its name: Dm7", !strcmp(b, "Dm7"));
    }
    cp_hand_in = 0;
    song.rec = 0;
    key_down(K(60)); frame(); key_up(K(60)); frame();
    ck("not armed: the keys write nothing", p->step[3].chord == CPC_SET + 4u);

    /* the tools */
    go_title("PAT TOOLS");
    frame();
    cpe_cur = 2;
    turn(EN_K1 + 1, 1);                                          /* COPY (the step) */
    press(B_OCTUP);
    cpe_cur = 3;
    turn(EN_K1 + 2, 1);                                          /* PASTE */
    press(B_OCTUP);
    ck("PAT TOOLS: COPY step 3, PASTE on step 4 (its hand chord, the same one)", p->step[3].chord == CPC_HAND &&
                                                                                  p->nhand == 1u && p->step[3].note == 76u);
    turn(EN_K1, 1);                                              /* ON PAT */
    turn(EN_K1 + 3, 1);                                          /* CLEAR */
    press(B_OCTUP);
    ck("ON PAT, CLEAR: the whole pattern empty", cp_len(p) == 16u && !p->nhand && !p->step[0].chord && !p->next);
    hold(B_SAVE);
    ck(".. SAVE held: back", cp_len(p) == 4u && p->nhand == 1u && p->step[0].chord == CPC_SET + 2u);
    turn(EN_K1 + 1, 1);                                          /* COPY the pattern */
    press(B_OCTUP);
    t->p[P_CPAT] = 9;
    frame();
    turn(EN_K1 + 2, 1);                                          /* PASTE it into pattern 9 */
    press(B_OCTUP);
    ck("COPY the pattern, CPAT 9, PASTE: pattern 9 the same", cp_get(9) && !memcmp(cp_get(9), cp_get(2), sizeof(cpat_t)));
    stop();

    printf(bad ? "cpat: FAILED\n" : "cpat: all ok\n");
    return bad != 0;
}
