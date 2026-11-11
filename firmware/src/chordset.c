/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 William Kew, ShinTech Electronics */
/* CHORD mode's chord sets (ShinTech): the factory sets of build/gen/chord_sets.h (tools/shintech/gen_chords.py, from
 * tools/shintech/chordsets/<genre>.json), a chord of a set, and a chord's name from its notes.
 * A set: a name, a genre and 12 chords; chord k plays on key k of the CHORD octave. A chord: its root (a pitch class),
 * a shape (CS_SHAPE: semitones above the root, ascending, up to CS_MAXN), an optional bass (a pitch class played
 * below the voicing: a slash chord) and its default voicing and inversion. Pure: no track state, built by the host
 * tests as it is (tests/chordset_test.c). Included by seq.c after chord.c (N_NOTE: params.c). */
#include "chord_sets.h"

enum { CSV_CLOSE, CSV_OPEN, CSV_SPREAD };           /* a chord's default voicing (the generator's VOICINGS) */
#define CS_NAME_MAX 16u                             /* cs_name's buffer: "C#mM9add11/G#" and its 0 */

typedef struct {
    uint8_t root;                                   /* pitch class 0..11 */
    uint8_t n;                                      /* notes of the shape, 1..CS_MAXN */
    int8_t iv[CS_MAXN];                             /* semitones above the root, ascending (iv[0] = 0) */
    int8_t bass;                                    /* pitch class of a slash chord's bass, -1 none */
    uint8_t voicing, inv;                           /* defaults: CSV_*, 0..3 */
} cs_chord_t;

/* chord `key` (0..11) of set `set` -> c; 0 = no such set */
static int cs_get(uint32_t set, uint32_t key, cs_chord_t *c)
{
    const uint8_t *b;
    uint32_t i;
    if (set >= CS_NSETS || key >= 12u)
        return 0;
    b = CS_SET[set].ch[key];
    c->root = (uint8_t)(b[0] & 15u);
    c->voicing = (uint8_t)((b[0] >> 4) & 3u);
    c->inv = (uint8_t)(b[0] >> 6);
    c->bass = (int8_t)(b[2] ? b[2] - 1 : -1);
    for (i = 0; i < CS_MAXN && CS_SHAPE[b[1]][i] >= 0; i++)
        c->iv[i] = CS_SHAPE[b[1]][i];
    c->n = (uint8_t)i;
    for (; i < CS_MAXN; i++)
        c->iv[i] = -1;
    return 1;
}

/* the shape's pitch classes above the root (bit i: i semitones mod 12) */
static uint32_t cs_mask(const cs_chord_t *c)
{
    uint32_t i, m = 0;
    for (i = 0; i < c->n; i++)
        m |= 1u << ((uint32_t)c->iv[i] % 12u);
    return m;
}

static void cs_cat(char *b, const char *s)
{
    uint32_t n = str_len(b);
    str_cpy(b + n, s, CS_NAME_MAX - n);
}

/* a chord's name from its root, its pitch classes above the root (bit i: i semitones) and its bass (a pitch class,
 * -1 none) -> b (CS_NAME_MAX bytes): "C", "Am7", "Fmaj9", "G7sus4", "Dm7b5", "E7#9", "Bbdim7" (sharps: "A#dim7"),
 * "C69", "Gmaj7#11", "Am/G". An omitted fifth is not named; a ninth with an altered ninth is "add9" after it.
 * tools/shintech/gen_chords.py reads every name back to the same notes (tests/chordset_test.c, test_chordsets.py) */
static void cs_name(char *b, uint32_t root, uint32_t m, int32_t bass)
{
    uint32_t b9 = m >> 1 & 1u, n9 = m >> 2 & 1u, b3 = m >> 3 & 1u, M3 = m >> 4 & 1u, p4 = m >> 5 & 1u;
    uint32_t b5 = m >> 6 & 1u, p5 = m >> 7 & 1u, s5 = m >> 8 & 1u, M6 = m >> 9 & 1u, b7 = m >> 10 & 1u;
    uint32_t M7 = m >> 11 & 1u;
    uint32_t minor = !M3 && b3, third = M3 || b3, has7 = b7 || M7, s9 = M3 && b3;
    uint32_t sus4 = 0, sus2 = 0, eleven, thirteen, sixth, nineish, num;
    uint32_t fb5 = !p5 && b5, fs5 = !p5 && s5 && !fb5;   /* the fifth itself flattened / raised */
    char nb[4];
    str_cpy(b, N_NOTE[root % 12u], CS_NAME_MAX);
    if (!third) {
        if (p4)
            sus4 = 1;
        else if (n9) {
            sus2 = 1;
            n9 = 0;
        }
    }
    if (minor && fb5 && M6 && !has7) {               /* the diminished seventh (its 6th is the bb7) */
        cs_cat(b, "dim7");
        M6 = 0;
        goto adds;
    }
    eleven = p4 && third;
    thirteen = M6 && has7;
    sixth = M6 && !has7;
    nineish = n9 || b9 || s9;
    if (!third && !sus4 && !sus2 && !has7 && !sixth && !nineish && (m & ~0x81u) == 0u && p5) {
        cs_cat(b, "5");                              /* power chord */
        goto bass;
    }
    if (minor && fb5 && !has7 && !sixth) {
        cs_cat(b, "dim");
        fb5 = 0;
    } else if (M3 && fs5 && !has7 && !sixth) {
        cs_cat(b, "aug");
        fs5 = 0;
    } else if (minor)
        cs_cat(b, "m");
    if (has7) {
        if (M7)
            cs_cat(b, minor ? "M" : "maj");
        num = thirteen && nineish ? 13u : eleven && nineish ? 11u : n9 ? 9u : 7u;   /* 7b9, 13b9 */
        cs_cat(b, num == 13u ? "13" : num == 11u ? "11" : num == 9u ? "9" : "7");
        if (num == 13u)
            thirteen = 0;
        if (num == 11u)
            eleven = 0;
        if (num >= 9u && n9 && (b9 || s9))
            n9 = 1;                                  /* both: the altered one names it, add9 below */
        else if (num >= 9u)
            n9 = 0;
    } else if (sixth) {
        cs_cat(b, n9 ? "69" : "6");
        n9 = 0;
        thirteen = 0;
    }
    if (sus4)
        cs_cat(b, "sus4");
    if (sus2)
        cs_cat(b, "sus2");
    if (!third && !sus4 && !sus2)
        cs_cat(b, "no3");
    if (n9)
        cs_cat(b, "add9");
    if (eleven)
        cs_cat(b, "add11");
    if (thirteen)
        cs_cat(b, "add13");
    if (fb5)
        cs_cat(b, "b5");
    if (fs5)
        cs_cat(b, "#5");
    if (b9)
        cs_cat(b, "b9");
    if (s9)
        cs_cat(b, "#9");
    if (p5 && b5)
        cs_cat(b, "#11");
    if (p5 && s5)
        cs_cat(b, "b13");
    goto bass;
adds:
    if (n9)
        cs_cat(b, "add9");
bass:
    if (bass >= 0 && (uint32_t)bass != root % 12u) {
        nb[0] = '/';
        str_cpy(nb + 1, N_NOTE[(uint32_t)bass % 12u], 3);
        cs_cat(b, nb);
    }
}
