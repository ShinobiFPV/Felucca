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
    if (f)
        fclose(f);
    printf("chordset: %s\n", fails ? "FAILED" : "all ok");
    return fails != 0;
}
