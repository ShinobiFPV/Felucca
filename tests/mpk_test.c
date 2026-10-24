/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 William Kew, ShinTech Electronics */
/* The Akai MPK mini IV by default (ShinTech), on the host through ui_test.c, with the MIDI it sends as captured from
 * one (a User preset: keys and knobs on channel 1, the knobs CC 24..31 with KnobM Rel, the pads notes 36.. on 10):
 *   build/shintech/host/mpk_test
 * 1. ROUT is MPK from power-on (appended, the default): the keys (channel 1) play the selected track, whichever it is
 * 2. channel 10 (the pads) plays the first DRUM track, whichever track is selected; without a DRUM track, the
 *    selected track; a note-off finds its note-on's track
 * 3. the knobs (CC 24..31, relative: 1..63 up, 65..127 down) step EDIT 1 / 2's macros P_E0..P_E7 of the selected
 *    track by the steps sent, within the parameter's range, never a jump; 0 and 64 move nothing
 * 4. a CC learned (MIDI LEARN) is the user's: CC 24 learned onto T1 LEVEL sets LEVEL and steps no macro
 * 5. other ROUT values as upstream: CH1-4 sends channel 10 nowhere; SEL sends it to the selected track */
#define UI_TEST_NO_MAIN 1
#include "ui_test.c"

static int mbad;
static void mcheck(const char *what, int ok)
{
    printf("mpk: %-96s %s\n", what, ok ? "ok" : "FAIL");
    mbad += !ok;
}
static void cc(uint32_t n, uint32_t v) { midi_event(0xB0u, 0, n, v); }

int main(void)
{
    uint32_t drum = 0, k;
    int ok;
    int16_t v0;

    /* 1, 2: the routing */
    ui_power_on();
    song.g[G_ROUTE] = GP[G_ROUTE].def;            /* (ui_test.c's power-on sets upstream's CH1-4 for its tests) */
    for (k = 0; k < NPART; k++)
        if (drum_track(&trk[k])) { drum = k; break; }
    mcheck("ROUT is MPK from power-on (the default; upstream: CH1-4), named MPK; T4 is the DRUM track",
           song.g[G_ROUTE] == ROUTE_MPK && str_eq(GP[G_ROUTE].names[ROUTE_MPK], "MPK") && drum == 3u);
    song.sel = 1;
    midi_event(0x90, 0, 60, 100);
    midi_event(0x90, 9, 36, 100);
    ok = midi_sel_on[0][60] == 2u && midi_sel_on[9][36] == drum + 1u && gated_notes(&trk[1]) == 1u;
    song.sel = 0;
    midi_event(0x90, 0, 64, 100);
    midi_event(0x90, 9, 38, 100);
    ok &= midi_sel_on[0][64] == 1u && midi_sel_on[9][38] == drum + 1u;
    mcheck("keys (channel 1): the selected track (T2, then T1); pads (channel 10): the DRUM track (T4) either way", ok);
    midi_event(0x80, 0, 60, 0);
    midi_event(0x80, 9, 36, 0);
    midi_event(0x80, 0, 64, 0);
    midi_event(0x80, 9, 38, 0);
    mcheck("note-offs find their note-ons' tracks", !midi_sel_on[0][60] && !midi_sel_on[9][36] && !midi_sel_on[0][64] &&
           !midi_sel_on[9][38] && !gated_notes(&trk[1]));
    set_engine_of(&trk[3], 0);                    /* no DRUM track now */
    song.sel = 2;
    midi_event(0x90, 9, 40, 100);
    mcheck("without a DRUM track: channel 10 plays the selected track", midi_sel_on[9][40] == 3u);
    midi_event(0x80, 9, 40, 0);

    /* 3: the knobs */
    ui_power_on();
    song.g[G_ROUTE] = ROUTE_MPK;
    song.sel = 1;
    set_engine(ENGI_OPN2);
    TSEL->p[P_E2] = 0;                            /* MLVL 0 (-64..64) */
    cc(26, 1); cc(26, 3);                         /* K3: +1, +3 (a faster turn) */
    ok = TSEL->p[P_E2] == 4;
    cc(26, 127); cc(26, 125);                     /* -1, -3 */
    ok &= TSEL->p[P_E2] == 0;
    cc(26, 0); cc(26, 64);
    ok &= TSEL->p[P_E2] == 0;
    mcheck("K3 (CC 26): MLVL +1 +3 -1 -3 = 0, relative; 0 and 64 move nothing", ok);
    for (k = 0; k < 40u; k++) cc(26, 4);
    ok = TSEL->p[P_E2] == param_desc_of(ENGI_OPN2, P_E2)->max;
    for (k = 0; k < 60u; k++) cc(26, 124);
    ok &= TSEL->p[P_E2] == param_desc_of(ENGI_OPN2, P_E2)->min;
    mcheck("within the parameter's range (MLVL -64..+64): never past either end", ok);
    ok = 1;
    for (k = 0; k < 8u; k++) {                    /* K1..K8: E0..E7, each its own */
        int16_t before[8];
        uint32_t j, id = P_E0 + k;
        for (j = 0; j < 8u; j++) before[j] = TSEL->p[P_E0 + j];
        cc(24u + k, TSEL->p[id] < param_desc_of(ENGI_OPN2, id)->max ? 1u : 127u);
        for (j = 0; j < 8u; j++)
            ok &= j == k ? TSEL->p[P_E0 + j] != before[j] : TSEL->p[P_E0 + j] == before[j];
    }
    mcheck("K1..K8 (CC 24..31) step E0..E7 (ALG FB MLVL MEG VSEN LADR LGCY SLOT), each its own", ok);
    song.sel = 0;
    v0 = trk[1].p[P_E1];
    {
        int16_t t1 = trk[0].p[P_E1];
        cc(25, t1 < param_desc_of(eng_idx(trk[0].eng_req), P_E1)->max ? 1u : 127u);
        mcheck("they follow the selection (SEL): T1's E1 moves, T2's stays", trk[1].p[P_E1] == v0 && trk[0].p[P_E1] != t1);
    }

    /* 4: MIDI LEARN first */
    ui_power_on();
    (void)ml_learn(24, 0, P_LEVEL);
    v0 = trk[0].p[P_E0];
    cc(24, 0);
    ok = trk[0].p[P_LEVEL] == param_desc_of(eng_idx(trk[0].eng_req), P_LEVEL)->min && trk[0].p[P_E0] == v0;
    (void)ml_clear_all();
    mcheck("CC 24 learned onto T1 LEVEL: it sets LEVEL (absolute), no macro steps", ok);

    /* 5: CH1-4 as upstream */
    ui_power_on();
    song.g[G_ROUTE] = 0;
    midi_event(0x90, 9, 36, 100);
    mcheck("ROUT CH1-4 (as upstream): channel 10 plays nothing", !midi_sel_on[9][36]);
    ui_power_on();
    song.g[G_ROUTE] = ROUTE_SEL;
    song.sel = 1;
    midi_event(0x90, 9, 36, 100);
    mcheck("ROUT SEL (as upstream): channel 10 plays the selected track", midi_sel_on[9][36] == 2u);

    printf("mpk_test: %s\n", mbad ? "FAILED" : "all checks ok");
    return mbad != 0;
}
