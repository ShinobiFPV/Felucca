/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 William Kew, ShinTech Electronics */
/* CHORD mode's chord sets (firmware/src/chordset.c, build/gen/chord_sets.h): every factory chord reads back as the
 * generator wrote it, the names of a table of shapes, and every factory chord's name written to argv[1] (set key
 * root mask bass name, one a line) for tests/shintech/test_chordsets.py, which reads each name back to its notes.
 * cc -Ibuild/gen -Ifirmware/src tests/chordset_test.c */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned str_len(const char *s) { return (unsigned)strlen(s); }
static void str_cpy(char *d, const char *s, unsigned max)   /* as bsp/chip/libc.c */
{
    unsigned i = 0;
    while (s[i] && i + 1 < max) {
        d[i] = s[i];
        i++;
    }
    if (max)
        d[i] = 0;
}
static const char *const N_NOTE[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
#include "chordset.c"

static int fails;
static void check(int ok, const char *what)
{
    printf("chordset: %-72s %s\n", what, ok ? "ok" : "FAIL");
    fails += !ok;
}

static uint32_t mask_of(const int *iv)
{
    uint32_t m = 0;
    for (; *iv >= 0; iv++)
        m |= 1u << (*iv % 12);
    return m;
}

/* a chord by hand: root, intervals (-1 ends), bass (-1 none), default voicing and inversion */
static cs_chord_t mk(uint32_t root, const int *iv, int bass, uint32_t voicing, uint32_t inv)
{
    cs_chord_t c;
    uint32_t i;
    memset(&c, 0, sizeof c);
    c.root = (uint8_t)root;
    for (i = 0; iv[i] >= 0; i++)
        c.iv[i] = (int8_t)iv[i];
    c.n = (uint8_t)i;
    c.bass = (int8_t)bass;
    c.voicing = (uint8_t)voicing;
    c.inv = (uint8_t)inv;
    return c;
}

/* out (n notes) is exactly want (-1 ends); prints the notes */
static void notes_are(const char *what, const uint8_t *out, uint32_t n, const int *want)
{
    char b[160], w[200];
    uint32_t i, ok = 1, k = 0;
    b[0] = 0;
    for (i = 0; i < n; i++)
        snprintf(b + strlen(b), sizeof b - strlen(b), "%s%u", i ? " " : "", out[i]);
    for (i = 0; want[i] >= 0; i++)
        ok &= i < n && out[i] == (uint8_t)want[i];
    ok &= i == n;
    k = (uint32_t)snprintf(w, sizeof w, "%s: %s", what, b);
    (void)k;
    check((int)ok, w);
}

static uint32_t moved(const uint8_t *a, const uint8_t *b, uint32_t n)   /* the semitones each voice moves, in sum */
{
    uint32_t i, s = 0;
    for (i = 0; i < n; i++)
        s += (uint32_t)(a[i] > b[i] ? a[i] - b[i] : b[i] - a[i]);
    return s;
}

static void voicing_tests(void)
{
    static const int MAJ[] = {0, 4, 7, -1}, MIN[] = {0, 3, 7, -1}, MAJ7[] = {0, 4, 7, 11, -1};
    static const int M13[] = {0, 4, 7, 10, 14, 21, -1}, M7[] = {0, 3, 7, 10, -1}, D7[] = {0, 4, 7, 10, -1};
    cs_chord_t c = mk(0, MAJ, -1, CSV_CLOSE, 0), f = mk(5, MAJ, -1, CSV_CLOSE, 0), g = mk(7, MAJ, -1, CSV_CLOSE, 0);
    uint8_t o[CS_VMAX], p[CS_VMAX], q[CS_VMAX];
    uint32_t n, i, auto_mv = 0, root_mv = 0, all = 1;
    n = cs_voice(&c, 0, CSV_AUTO, CS_INV_AUTO, 60, 0, 0, 0, o);
    notes_are("C, RANGE C4, the first chord: its default (root position)", o, n, (const int[]){60, 64, 67, -1});
    n = cs_voice(&c, 7, CSV_AUTO, CS_INV_AUTO, 60, 0, 0, 0, o);
    notes_are("C in KEY G: G B D, its lowest in the window", o, n, (const int[]){67, 71, 74, -1});
    n = cs_voice(&c, 0, CSV_AUTO, 1, 60, 0, 0, 0, o);
    notes_are("INVERSION 1: E G C", o, n, (const int[]){64, 67, 72, -1});
    n = cs_voice(&c, 0, CSV_AUTO, 2, 60, 0, 0, 0, o);
    notes_are("INVERSION 2: G C E", o, n, (const int[]){67, 72, 76, -1});
    n = cs_voice(&c, 0, CSV_AUTO, 3, 60, 0, 0, 0, o);
    notes_are("INVERSION 3 of a triad: its last (2)", o, n, (const int[]){67, 72, 76, -1});
    /* voice leading: C -> F -> G -> C, each the closest inversion in the window */
    n = cs_voice(&c, 0, CSV_AUTO, CS_INV_AUTO, 60, 0, 0, 0, p);
    n = cs_voice(&f, 0, CSV_AUTO, CS_INV_AUTO, 60, 0, p, n, o);
    notes_are("voice leading C -> F: C F A (the common C kept, E G up one and two)", o, n, (const int[]){60, 65, 69, -1});
    memcpy(p, o, n);
    n = cs_voice(&g, 0, CSV_AUTO, CS_INV_AUTO, 60, 0, p, n, o);
    notes_are("voice leading F -> G: D G B (each voice up two)", o, n, (const int[]){62, 67, 71, -1});
    memcpy(p, o, n);
    n = cs_voice(&c, 0, CSV_AUTO, CS_INV_AUTO, 60, 0, p, n, o);
    notes_are("voice leading G -> C: E G C (G kept, D up two, B up one)", o, n, (const int[]){64, 67, 72, -1});
    {   /* ii V I vi IV in C, over and over: voice leading moves less than root position, every chord its own notes */
        const cs_chord_t prog[5] = {mk(2, M7, -1, 0, 0), mk(7, D7, -1, 0, 0), mk(0, MAJ7, -1, 0, 0), mk(9, M7, -1, 0, 0),
                                    mk(5, MAJ7, -1, 0, 0)};
        uint32_t np = 0, nr = 0, k;
        uint8_t pr[CS_VMAX];
        for (k = 0; k < 20u; k++) {
            const cs_chord_t *x = &prog[k % 5u];
            uint32_t na = cs_voice(x, 0, CSV_AUTO, CS_INV_AUTO, 55, 0, p, np, o);
            uint32_t nb = cs_voice(x, 0, CSV_AUTO, 0, 55, 0, 0, 0, q);
            for (i = 0; i < na; i++) {
                uint32_t pc = (o[i] + 12u - x->root) % 12u, j, hit = 0;
                for (j = 0; j < x->n; j++)
                    hit |= pc == (uint32_t)x->iv[j] % 12u;
                all &= hit && o[i] >= 55u && o[0] < 67u;
            }
            if (k) {
                auto_mv += moved(o, p, na < np ? na : np);
                root_mv += moved(q, pr, nb < nr ? nb : nr);
            }
            memcpy(p, o, na);
            np = na;
            memcpy(pr, q, nb);
            nr = nb;
        }
        {
            char w[120];
            snprintf(w, sizeof w, "ii V I vi IV x4: voice leading moves %u semitones, root position %u", auto_mv, root_mv);
            check(auto_mv * 2u < root_mv, w);
        }
        check((int)all, ".. every note one of the chord's, the lowest in the window, none below it");
    }
    {
        cs_chord_t m7 = mk(0, MAJ7, -1, CSV_CLOSE, 0);
        n = cs_voice(&m7, 0, CSV_OPEN, 0, 60, 0, 0, 0, o);
        notes_are("Cmaj7 OPEN (drop 2): G C E B", o, n, (const int[]){67, 72, 76, 83, -1});
        n = cs_voice(&m7, 0, CSV_SPREAD, 0, 60, 0, 0, 0, o);
        notes_are("Cmaj7 SPREAD (drop 2 and 4): C G E B", o, n, (const int[]){60, 67, 76, 83, -1});
        n = cs_voice(&c, 0, CSV_SPREAD, 0, 60, 0, 0, 0, o);
        notes_are("C SPREAD (a triad, 1-5-10): C G E", o, n, (const int[]){60, 67, 76, -1});
        m7.voicing = CSV_SPREAD;
        n = cs_voice(&m7, 0, CSV_AUTO, 0, 60, 0, 0, 0, o);
        notes_are("VOICING AUTO: the chord's own default (SPREAD)", o, n, (const int[]){60, 67, 76, 83, -1});
    }
    {
        cs_chord_t am_g = mk(9, MIN, 7, CSV_CLOSE, 0);
        n = cs_voice(&am_g, 0, CSV_AUTO, 0, 60, 1, 0, 0, o);
        notes_are("Am/G with BASS: G below the window, then A C E", o, n, (const int[]){55, 69, 72, 76, -1});
        n = cs_voice(&c, 0, CSV_AUTO, 0, 60, 1, 0, 0, o);
        notes_are("C with BASS (no slash): its root below the window", o, n, (const int[]){48, 60, 64, 67, -1});
        n = cs_voice(&am_g, 0, CSV_AUTO, 0, 60, 0, 0, 0, o);
        notes_are("Am/G without BASS: the slash bass is not played", o, n, (const int[]){69, 72, 76, -1});
    }
    {
        cs_chord_t c13 = mk(0, M13, -1, CSV_CLOSE, 0);
        n = cs_voice(&c13, 0, CSV_AUTO, 0, 48, 1, 0, 0, o);
        notes_are("C13 with BASS: seven notes (CS_VMAX)", o, n, (const int[]){36, 48, 52, 55, 58, 62, 69, -1});
        n = cs_voice(&c13, 0, CSV_AUTO, 0, 120, 1, 0, 0, o);
        for (i = 0, all = 1; i < n; i++)
            all &= o[i] <= 127u && (!i || o[i] > o[i - 1u]);
        check(all && n < 7u, "RANGE at the top: notes above 127 left out, the rest ascending");
    }
}

int main(int argc, char **argv)
{
    static const struct { uint32_t root; int iv[8]; int bass; const char *name; } NAMES[] = {
        {0, {0, 4, 7, -1}, -1, "C"},            {9, {0, 3, 7, -1}, -1, "Am"},
        {5, {0, 4, 7, 11, 14, -1}, -1, "Fmaj9"}, {2, {0, 3, 7, 10, 14, 17}, -1, "Dm11"},
        {7, {0, 4, 7, 10, 14, 21}, -1, "G13"},  {7, {0, 5, 7, 10, -1}, -1, "G7sus4"},
        {2, {0, 3, 6, 10, -1}, -1, "Dm7b5"},    {4, {0, 4, 7, 10, 15, -1}, -1, "E7#9"},
        {9, {0, 4, 7, 10, 13, -1}, -1, "A7b9"}, {10, {0, 3, 6, 9, -1}, -1, "A#dim7"},
        {0, {0, 4, 7, 9, 14, -1}, -1, "C69"},   {7, {0, 4, 7, 11, 18, -1}, -1, "Gmaj7#11"},
        {9, {0, 3, 7, -1}, 7, "Am/G"},          {0, {0, 2, 7, -1}, -1, "Csus2"},
        {4, {0, 7, -1}, -1, "E5"},              {0, {0, 3, 7, 11, -1}, -1, "CmM7"},
        {0, {0, 3, 7, 14, -1}, -1, "Cmadd9"},   {0, {0, 4, 8, -1}, -1, "Caug"},
        {0, {0, 3, 6, -1}, -1, "Cdim"},         {3, {0, 3, 7, 9, -1}, -1, "D#m6"},
        {0, {0, 4, 7, 10, 13, 21}, -1, "C13b9"}, {0, {0, 4, 7, 14, -1}, -1, "Cadd9"},
        {0, {0, 4, 7, 11, -1}, 4, "Cmaj7/E"},   {0, {0, 4, 10, 14, 21, -1}, -1, "C13"},
    };
    uint32_t i, k, s, bad = 0, bad_shape = 0, longest = 0, nchords = 0;
    char b[CS_NAME_MAX], what[96];
    FILE *f = argc > 1 ? fopen(argv[1], "w") : 0;
    if (argc > 1 && !f) {
        perror(argv[1]);
        return 1;
    }
    for (i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++) {
        cs_name(b, NAMES[i].root, mask_of(NAMES[i].iv), NAMES[i].bass);
        snprintf(what, sizeof what, "name of %s (got %s)", NAMES[i].name, b);
        check(!strcmp(b, NAMES[i].name), what);
    }
    for (s = 0; s < CS_NSETS; s++)
        for (k = 0; k < 12u; k++) {
            cs_chord_t c;
            if (!cs_get(s, k, &c)) {
                bad++;
                continue;
            }
            nchords++;
            if (c.n < 1u || c.n > CS_MAXN || c.iv[0] != 0 || c.root > 11u || c.bass > 11 || c.inv >= c.n ||
                c.voicing > CSV_SPREAD)
                bad_shape++;
            for (i = 1; i < c.n; i++)
                bad_shape += c.iv[i] <= c.iv[i - 1u];
            cs_name(b, c.root, cs_mask(&c), c.bass);
            if (str_len(b) > longest)
                longest = str_len(b);
            if (f)
                fprintf(f, "%u %u %u %u %d %s\n", s, k, c.root, cs_mask(&c), c.bass + 1, b);
        }
    snprintf(what, sizeof what, "%u factory chords read back (%u sets)", nchords, CS_NSETS);
    check(!bad && nchords == CS_NSETS * 12u, what);
    check(!bad_shape, "every chord: 1..CS_MAXN notes ascending from 0, root, bass, voicing, inversion in range");
    snprintf(what, sizeof what, "every name fits its buffer (the longest %u of %u)", longest, CS_NAME_MAX - 1u);
    check(longest < CS_NAME_MAX - 1u, what);
    {
        cs_chord_t c;
        check(!cs_get(CS_NSETS, 0, &c) && !cs_get(0, 12, &c), "a set or key out of range is refused");
    }
    for (i = 0, k = 1; i < CS_NGENRES; i++)
        if (CS_GENRE_FIRST[i] < CS_NSETS && (CS_SET[CS_GENRE_FIRST[i]].genre != i ||
                                             (CS_GENRE_FIRST[i] && CS_SET[CS_GENRE_FIRST[i] - 1u].genre >= i)))
            k = 0;
    check(k, "CS_GENRE_FIRST: each genre's first set, the sets in genre order");
    voicing_tests();
    if (f)
        fclose(f);
    printf("chordset: %s\n", fails ? "FAILED" : "all ok");
    return fails != 0;
}
