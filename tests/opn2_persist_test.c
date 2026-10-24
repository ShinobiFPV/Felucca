/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 William Kew, ShinTech Electronics */
/* OPN2 patches in the stores (src/project.c FUN11, src/upreset.c version 6, src/up_fm6.c), on the host, through
 * ui_test.c as fun10_test.c.
 *   build/shintech/host/opn2_persist_test
 * 1. a project with an OPN2 track is written FUNB ("FUN11") and loads back with the track's own patch, byte for byte;
 *    SLOT shows OWN for an edited patch, F n for a factory one
 * 2. a project without one is still written FUNA (FUN10), as Felucca 1.6 writes it
 * 3. a FUNA (any older project) still loads, its tracks as they were
 * 4. a user preset of an OPN2 sound is a version 6 record and gives its patch back; another engine's keeps its
 *    version (4)
 * 5. favourites of the 16BIT engines in the favourites rows' unused upper halves
 * 6. the operator pages (ui_opn2op.c): each knob its byte, undo, DT's order, RATIO mode; the charts' tables */
#define UI_TEST_NO_MAIN 1
#include "ui_test.c"

static int pbad;
static void pcheck(const char *what, int ok)
{
    printf("opn2 store: %-88s %s\n", what, ok ? "ok" : "FAIL");
    pbad += !ok;
}

static op2_patch_t edited(void)                  /* a factory patch, edited: no factory patch equals it */
{
    op2_patch_t p = OP2_FACTORY[6];
    p.op[1].tl = 17;
    p.op[2].mul = 9;
    p.fb = 2;
    memcpy(p.name, "MY BRASS        ", 16);
    return p;
}

/* 6. the operator pages (ui_opn2op.c) and the chart's tables */
static int o2_diff(const op2_patch_t *a, const op2_patch_t *b, uint32_t *at)   /* bytes that differ (the last at *at) */
{
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    uint32_t i;
    int n = 0;
    for (i = 0; i < OP2_PATCH_SIZE; i++)
        if (x[i] != y[i]) { n++; *at = i; }
    return n;
}
static void o2_pick(uint32_t opn) { turn(EN_K1, -4); turn(EN_K1, (int32_t)opn - 1); }   /* KNOB 1: OP opn (1..4) */
static void o2_stage(uint32_t s) { turn(EN_K2, -4); turn(EN_K2, (int32_t)s); }           /* OP ENV KNOB 2: stage s */
static int o2_own(void) { return TSEL->p[P_E7] == (int16_t)OP2_OWN && op2_slot[song.sel] == OP2_OWN; }
#define O2B(i, f) ((uint32_t)__builtin_offsetof(op2_patch_t, op) + (i) * (uint32_t)sizeof(op2_op_t) + O2_OFF(f))
static void opn2_ops(void)
{
    static const char *const CYC[] = {"EDIT 1", "EDIT 2", "OPERATOR", "OP ENV", "OPERATOR 2", "VOICE", "VOICE 2", "VOICE 3",
                                      "EDIT 1"};
    static const struct { const char *page; uint32_t k, stg, off, max; } F[] = {   /* (stg 9: no stage) */
        {"OPERATOR", EN_K2, 9, O2_OFF(mul), 15}, {"OPERATOR", EN_K4, 9, O2_OFF(tl), 127},
        {"OPERATOR 2", EN_K3, 9, O2_OFF(ks), 3}, {"OPERATOR 2", EN_K4, 9, O2_OFF(am), 1},
        {"OP ENV", EN_K3, 0, O2_OFF(ar), 31}, {"OP ENV", EN_K3, 1, O2_OFF(dr), 31}, {"OP ENV", EN_K3, 2, O2_OFF(sr), 31},
        {"OP ENV", EN_K3, 3, O2_OFF(rr), 15}, {"OP ENV", EN_K4, 1, O2_OFF(sl), 15}};
    int ok;
    uint32_t tr, opn, f, at, n = 0, a, k, j;
    op2_patch_t before, v;
    ui_power_on();
    song.sel = 1;
    tr = song.sel;
    set_engine(ENGI_OPN2);
    TSEL->p[P_E7] = 6;                            /* F7 BRASS */
    op2_poll();
    frame();
    pcheck("ops: OPN2's EDIT cycle EDIT 1 EDIT 2 OPERATOR OP ENV OPERATOR 2 VOICE VOICE 2 VOICE 3 EDIT 1",
           TSEL->eng_req == ENGI_OPN2 && engine_cycle(CYC, NELEM(CYC)));
    go_title("OPERATOR"); frame();
    o2_pick(3);
    before = op2_patch[tr];
    turn(EN_K4, 3);                               /* OP 3 TL, one gesture of two turns */
    turn(EN_K4, 2);
    ok = o2_diff(&before, &op2_patch[tr], &at) == 1 && at == O2B(2, tl) && op2_patch[tr].op[2].tl == before.op[2].tl + 5u &&
         o2_own() && o2p_op == 2u;
    pcheck("ops: OPERATOR KNOB 4 on OP 3: its TL byte only; F7 -> SLOT OWN", ok);
    hold(B_SAVE);
    ok = !memcmp(&before, &op2_patch[tr], OP2_PATCH_SIZE) && TSEL->p[P_E7] == 6 && op2_slot[tr] == 6u && msg_is("UNDO/REDO T2");
    hold(B_SAVE);
    ok &= op2_patch[tr].op[2].tl == before.op[2].tl + 5u && o2_own();
    pcheck("ops: SAVE held undoes the gesture (patch and SLOT F7 back), again redoes", ok);

    ok = 1;                                       /* every operator, every byte: that byte only, +-1, 0..its highest */
    for (opn = 1; opn <= 4u; opn++)
        for (f = 0; f < NELEM(F); f++) {
            uint32_t b = O2B(opn - 1u, ar) - O2_OFF(ar) + F[f].off;
            int32_t d;
            go_title(F[f].page); frame();
            o2_pick(opn);
            if (F[f].stg < 4u) o2_stage(F[f].stg);
            v = op2_patch[tr];
            d = ((uint8_t *)&v)[b] < F[f].max ? 1 : -1;
            frames(1600);
            turn(F[f].k, d);
            if (o2_diff(&v, &op2_patch[tr], &at) != 1 || at != b || ((uint8_t *)&op2_patch[tr])[b] != (uint8_t)(((uint8_t *)&v)[b] + d) ||
                !o2_own()) {
                printf("opn2 store: ops: OP %u %s KNOB %u: byte %u, not %u\n", opn, F[f].page, F[f].k - EN_K1 + 1u, at, b);
                ok = 0;
            }
            turn(F[f].k, 300);
            ok &= ((uint8_t *)&op2_patch[tr])[b] == F[f].max;
            turn(F[f].k, -300);
            ok &= ((uint8_t *)&op2_patch[tr])[b] == 0u;
            n++;
        }
    pcheck("ops: 4 operators x 9 bytes (MUL TL KS AM AR DR SR RR SL), each knob its byte only, 0..its highest, SLOT OWN",
           ok && n == 36u);
    go_title("OPERATOR"); frame();               /* DT: -3..+3 as the register's 7 6 5 0 1 2 3 (4: -0, never written) */
    o2_pick(2);
    turn(EN_K3, -10);
    ok = op2_patch[tr].op[1].dt == 7u;
    turn(EN_K3, 3);
    ok &= op2_patch[tr].op[1].dt == 0u;
    turn(EN_K3, 1);
    ok &= op2_patch[tr].op[1].dt == 1u;
    turn(EN_K3, 10);
    ok &= op2_patch[tr].op[1].dt == 3u;
    go_title("OPERATOR 2"); frame();
    turn(EN_K2, -1);
    ok &= op2_patch[tr].op[1].dt == 2u;
    go_title("OP ENV"); frame();
    o2_stage(0);
    v = op2_patch[tr];
    turn(EN_K4, 5);                               /* LEVEL on ATK: nothing to set */
    ok &= !memcmp(&v, &op2_patch[tr], OP2_PATCH_SIZE);
    pcheck("ops: DT -3..+3 (the register's 7..3), on OPERATOR and OPERATOR 2; LEVEL off DEC writes nothing", ok);
    v = op2_patch[tr];                            /* RATIO mode: KNOB 2 RATIO (0..31), KNOB 3 FINE (0..99) */
    v.ratio = 1;
    op2_set_patch(tr, &v);
    go_title("OPERATOR"); frame();
    o2_pick(4);
    turn(EN_K2, 300);
    turn(EN_K3, 300);
    ok = op2_patch[tr].op[3].rcoarse == 31u && op2_patch[tr].op[3].rfine == 99u && op2_patch[tr].op[3].mul == v.op[3].mul &&
         op2_patch[tr].op[3].dt == v.op[3].dt;
    pcheck("ops: a RATIO patch: OPERATOR edits RATIO (to 31) and FINE (to 99), not MUL or DT", ok);

    ok = 1;                                       /* the chart: carriers on row 0 and only they; a modulator one row
                                                   * above each it modulates; one operator per cell; the carriers those
                                                   * modulating nothing (OP2_CARRIERS, the engine's own); OP1 feeds back */
    for (a = 0; a < 8u; a++)
        for (k = 0; k < 4u; k++) {
            uint32_t row = O2_CELL[a][k] >> 4, outs = 0;
            ok &= (row == 0u) == ((OP2_CARRIERS[a] >> k) & 1u) && (O2_CELL[a][k] & 0x0Fu) <= 6u;
            ok &= !(O2_MOD[a][k] >> k & 1u) && !(O2_MOD[a][k] >> 4);
            for (j = 0; j < 4u; j++) {
                if (j != k && O2_CELL[a][j] == O2_CELL[a][k]) ok = 0;
                if (O2_MOD[a][j] >> k & 1u) { outs++; ok &= O2_CELL[a][k] >> 4 == (O2_CELL[a][j] >> 4) + 1u; }
            }
            ok &= (outs == 0u) == ((OP2_CARRIERS[a] >> k) & 1u);
        }
    pcheck("ops: the eight charts: carriers row 0 (OP2_CARRIERS), modulators one row above, one per cell", ok);

    ok = 1;                                       /* the pages on FM6 and OPN2 only */
    song.sel = 0;
    set_engine(0);
    for (f = 0; f < NPAGES; f++)
        if (PAGES[f].graph >= GR_FMOP && PAGES[f].graph <= GR_FMENV) ok &= !page_visible(f);
    song.sel = 1;
    for (f = 0; f < NPAGES; f++)
        if (PAGES[f].graph >= GR_FMOP && PAGES[f].graph <= GR_FMENV) ok &= page_visible(f);
    pcheck("ops: the operator pages on an OPN2 track, not on ANALOG", ok);
}

int main(void)
{
    static project_t a, b;
    static project_store_t st;
    op2_patch_t mine = edited();
    uint32_t magic;
    int ok;

    /* 1. FUNB */
    ui_power_on();
    set_engine_of(&trk[1], ENGI_OPN2);
    trk[1].p[P_E7] = 6;                           /* F7 BRASS */
    op2_poll();
    op2_set_patch(1, &mine);
    op2_adopt(1);
    pcheck("an edited patch: SLOT shows OWN", trk[1].p[P_E7] == (int16_t)OP2_OWN);
    project_capture(&a);
    ok = proj_pack(&st, &a);
    memcpy(&magic, st.raw, 4);
    pcheck("a project with an OPN2 track packs as FUNB (FUN11), 3840 bytes", ok && magic == 0x46554E42u &&
           ((uint32_t *)st.raw)[1] == 3840u);
    ok = proj_import(&b, &st, sizeof st) && !memcmp(&a, &b, sizeof a);
    pcheck("FUNB imports back to the same project", ok);
    op2_set_patch(1, &OP2_INIT);                  /* (something else meanwhile) */
    project_restore_runtime(&b);
    pcheck("restored: track 2 is OPN2 with its own patch, byte for byte, SLOT OWN",
           trk[1].eng_req == ENGI_OPN2 && !memcmp(&op2_patch[1], &mine, OP2_PATCH_SIZE) && trk[1].p[P_E7] == (int16_t)OP2_OWN);
    op2_set_patch(1, &OP2_FACTORY[3]);
    op2_adopt(1);
    project_capture(&a);
    proj_pack(&st, &a);
    proj_import(&b, &st, sizeof st);
    op2_set_patch(1, &OP2_INIT);
    project_restore_runtime(&b);
    pcheck("a factory patch comes back as F4 (SLOT 3)", trk[1].p[P_E7] == 3 && !memcmp(&op2_patch[1], &OP2_FACTORY[3],
                                                                                            OP2_PATCH_SIZE));

    /* 2. FUNA without one, 3. FUNA loads */
    ui_power_on();
    project_capture(&a);
    ok = proj_pack(&st, &a);
    memcpy(&magic, st.raw, 4);
    pcheck("a project without a 16BIT track still packs as FUNA (FUN10), as Felucca 1.6", ok && magic == 0x46554E41u);
    ok = proj_import(&b, &st, sizeof st) && !memcmp(&a, &b, sizeof a);
    pcheck("FUNA imports as before", ok);

    /* 4. user presets */
    ui_power_on();
    song.sel = 2;
    set_engine(ENGI_OPN2);
    op2_set_patch(2, &mine);
    op2_adopt(2);
    up_store(5, "MYBRASS");
    pcheck("an OPN2 user preset: a version 6 record, engine 14", up_used(5) && up_rec(5)->ver == UP_VER_16BIT &&
           up_rec(5)->engine == ENGI_OPN2);
    op2_set_patch(2, &OP2_INIT);
    song.sel = 0;
    set_engine(0);                                /* (the record loads into the selected track: track 1 now) */
    up_load(5);
    pcheck("loading it: OPN2, its patch byte for byte, SLOT OWN", trk[0].eng_req == ENGI_OPN2 &&
           !memcmp(&op2_patch[0], &mine, OP2_PATCH_SIZE) && trk[0].p[P_E7] == (int16_t)OP2_OWN);
    set_engine(ENGI_FM6);
    up_store(6, "FMSOUND");
    pcheck("an FM6 user preset keeps version 4", up_used(6) && up_rec(6)->ver == UP_VER);

    /* 5. favourites of the 16BIT engines: in rows 0 / 2 / 3's unused upper halves */
    memset(&favorites, 0, sizeof favorites);
    favorite_set(0, 3, 1);                        /* ANALOG preset 4 */
    ok = favorite_set(ENGI_OPN2, 31, 1) && favorite_has(ENGI_OPN2, 31) && !favorite_has(ENGI_OPN2, 30) &&
         favorite_has(0, 3) && favorites.factory[0][16 + 31 / 8] == 1u << (31 % 8);
    pcheck("an OPN2 favourite: set and read back, in ANALOG's row's unused upper half, ANALOG's own untouched", ok);
    ok = favorite_set(ENGI_OPN2, 31, 0) && !favorite_has(ENGI_OPN2, 31) && favorite_has(0, 3) &&
         !favorite_set(ENGI_OPN2, 128, 1);
    pcheck("cleared again; a preset number past 127 refused", ok);

    opn2_ops();

    /* 7. PSG (engine 15): a project with a PSG track (and none of OPN2) is FUNB and loads back; a user preset of it a
     *    version 6 record; its favourites in row 2's unused upper half */
    ui_power_on();
    set_engine_of(&trk[2], ENGI_PSG);
    apply_preset_to(&trk[2], 8);                  /* MAJOR ARP */
    trk[2].p[P_E6] = -12;                         /* (an edit) */
    project_capture(&a);
    ok = proj_pack(&st, &a);
    memcpy(&magic, st.raw, 4);
    ok &= magic == 0x46554E42u && proj_import(&b, &st, sizeof st) && !memcmp(&a, &b, sizeof a);
    trk[2].p[P_E6] = 0;
    project_restore_runtime(&b);
    pcheck("PSG: a project with a PSG track packs as FUNB and comes back (T3 PSG, its SWP -12)",
           ok && trk[2].eng_req == ENGI_PSG && trk[2].p[P_E6] == -12);
    ui_power_on();
    song.sel = 2;
    set_engine(ENGI_PSG);
    up_store(7, "PSGARP");
    pcheck("PSG: a user preset of it: a version 6 record, engine 15", up_used(7) && up_rec(7)->ver == UP_VER_16BIT &&
           up_rec(7)->engine == ENGI_PSG);
    memset(&favorites, 0, sizeof favorites);
    favorite_set(2, 1, 1);                        /* PHASE preset 2 */
    ok = favorite_set(ENGI_PSG, 25, 1) && favorite_has(ENGI_PSG, 25) && !favorite_has(ENGI_OPN2, 25) &&
         favorite_has(2, 1) && favorites.factory[2][16 + 25 / 8] == 1u << (25 % 8);
    pcheck("PSG: a favourite in PHASE's row's unused upper half, PHASE's and OPN2's own untouched", ok);

    /* 8. PCM8 (engine 16): FUNB, version 6, favourites in row 3's upper half */
    ui_power_on();
    set_engine_of(&trk[3], ENGI_PCM8);
    apply_preset_to(&trk[3], 2);                  /* CRUNCH KIT */
    project_capture(&a);
    ok = proj_pack(&st, &a);
    memcpy(&magic, st.raw, 4);
    ok &= magic == 0x46554E42u && proj_import(&b, &st, sizeof st) && !memcmp(&a, &b, sizeof a);
    trk[3].p[P_E2] = 0;
    project_restore_runtime(&b);
    pcheck("PCM8: a project with a PCM8 track packs as FUNB and comes back (T4 PCM8, 4 bits)",
           ok && trk[3].eng_req == ENGI_PCM8 && trk[3].p[P_E2] == 1);
    ui_power_on();
    song.sel = 3;
    set_engine(ENGI_PCM8);
    up_store(8, "PCMKIT");
    pcheck("PCM8: a user preset of it: a version 6 record, engine 16", up_used(8) && up_rec(8)->ver == UP_VER_16BIT &&
           up_rec(8)->engine == ENGI_PCM8);
    memset(&favorites, 0, sizeof favorites);
    favorite_set(3, 0, 1);                        /* LOFI preset 1 */
    ok = favorite_set(ENGI_PCM8, 13, 1) && favorite_has(ENGI_PCM8, 13) && !favorite_has(ENGI_PSG, 13) &&
         favorite_has(3, 0) && favorites.factory[3][16 + 13 / 8] == 1u << (13 % 8);
    pcheck("PCM8: a favourite in LOFI's row's unused upper half, LOFI's and PSG's own untouched", ok);

    printf("opn2_persist_test: %s\n", pbad ? "FAILED" : "all checks ok");
    return pbad != 0;
}
