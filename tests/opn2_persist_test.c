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
 *    version (4) */
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

    printf("opn2_persist_test: %s\n", pbad ? "FAILED" : "all checks ok");
    return pbad != 0;
}
