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

/* the user's chord sets (ShinTech, Phase 5: written by the web editor, editor_shin.c; flash: cpat_store.c), SET
 * CS_NSETS .. CS_NALL - 1 after the factory ones ("USR1".."USR16"). A chord: its root, a slash bass (0 none, else 1 + its
 * pitch class), its default voicing | inversion << 2, the notes above the root as a mask (bit i - 1: i semitones, 1..24;
 * at most CS_MAXN notes with the root, the lowest) */
#ifndef CS_NUSER                                    /* (params.c has them first; the host tests build this alone) */
#define CS_NUSER 16u
#define CS_NALL (CS_NSETS + CS_NUSER)
#endif
#define CS_USER_MAGIC 0x31534355u                   /* "UCS1" */
typedef struct { uint8_t root, bass, vi, iv[3]; } cs_uchord_t;
typedef struct { uint8_t used, rsv; char name[CS_NAME_LEN]; cs_uchord_t ch[12]; } cs_uset_t;
typedef struct { uint32_t magic; cs_uset_t set[CS_NUSER]; } cs_user_t;
static cs_user_t cs_user __attribute__((section(".pool")));
/* a user chord as chordset.c's (n: the root and the mask's notes, the lowest CS_MAXN) */
static void cs_from_user(const cs_uchord_t *u, cs_chord_t *c)
{
    uint32_t i, m = u->iv[0] | (uint32_t)u->iv[1] << 8 | (uint32_t)u->iv[2] << 16;
    c->root = (uint8_t)(u->root % 12u);
    c->voicing = (uint8_t)(u->vi & 3u) > 2u ? 0u : (uint8_t)(u->vi & 3u);
    c->inv = (uint8_t)((u->vi >> 2) & 3u);
    c->bass = (int8_t)(u->bass && u->bass <= 12u ? u->bass - 1 : -1);
    c->n = 1;
    c->iv[0] = 0;
    for (i = 0; i < 24u && c->n < CS_MAXN; i++)
        if ((m >> i) & 1u)
            c->iv[c->n++] = (int8_t)(i + 1u);
    for (i = c->n; i < CS_MAXN; i++)
        c->iv[i] = -1;
    if (c->inv >= c->n)
        c->inv = 0;
}
/* set s (factory or user) has a name and chords; its name -> b (CS_NAME_LEN + 1) */
static int cs_set_ok(uint32_t s) { return s < CS_NSETS || (s < CS_NALL && cs_user.set[s - CS_NSETS].used); }
static void cs_set_name(uint32_t s, char *b)
{
    const char *n = s < CS_NSETS ? CS_SET[s].name : s < CS_NALL && cs_user.set[s - CS_NSETS].used ? cs_user.set[s - CS_NSETS].name
                                                                                            : "EMPTY";
    uint32_t i;
    for (i = 0; i < CS_NAME_LEN && n[i]; i++)
        b[i] = n[i];
    b[i] = 0;
}

/* chord `key` (0..11) of set `set` -> c; 0 = no such set (or an empty user set) */
static int cs_get(uint32_t set, uint32_t key, cs_chord_t *c)
{
    const uint8_t *b;
    uint32_t i;
    if (set >= CS_NSETS && set < CS_NALL && key < 12u && cs_user.set[set - CS_NSETS].used) {
        cs_from_user(&cs_user.set[set - CS_NSETS].ch[key], c);
        return 1;
    }
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

/* ------------------------------------------------------------ voicing --- */
/* A chord as it plays: its tones in the octave window RANGE, voiced, inverted, with voice leading.
 *   RANGE  lo: the window's bottom note; the voicing's lowest note always lies in lo .. lo + 11, so a KEY (transpose)
 *          moves the chord inside the window, never by octaves
 *   VOICING CLOSE: the tones stacked in their order (root 3rd 5th 7th 9th ..), each the next of its pitch class above
 *          the one before, from the inversion's tone; OPEN: drop 2 (the second highest an octave down); SPREAD: drop 2
 *          and 4 (a triad: its middle tone an octave up, 1-5-10); AUTO: the chord's own default
 *   INVERSION 0..3: that tone lowest (as far as the chord has tones); AUTO: of every inversion in the window, the one
 *          closest to the chord before (the sum over each note of its distance to the nearest note of the other
 *          chord, both ways), on a tie the chord's default inversion, then the lower; the first chord takes its default
 *   BASS   the slash bass, else the root, in the octave below the window (left out of the voice leading): the one note
 *          below lo (every note of the voicing is lo or above)
 * At most CS_VMAX notes, ascending; notes outside 0..127 left out */
enum { CSV_AUTO = 3 };                              /* VOICING: the chord's default */
#define CS_INV_AUTO (-1)
#define CS_VMAX (CS_MAXN + 1u)                      /* the voicing and its bass */

static void cs_sort(int32_t *v, uint32_t n)
{
    uint32_t i, j;
    for (i = 1; i < n; i++)
        for (j = i; j > 0 && v[j - 1u] > v[j]; j--) {
            int32_t x = v[j];
            v[j] = v[j - 1u];
            v[j - 1u] = x;
        }
}

/* the chord's tones from inversion r, voiced, its lowest note in lo .. lo + 11 -> v (ascending), their number */
static uint32_t cs_shape_at(const cs_chord_t *c, uint32_t key, uint32_t voicing, uint32_t r, int32_t lo, int32_t *v)
{
    uint32_t n = c->n, i;
    int32_t prev = 0, shift;
    for (i = 0; i < n; i++) {                       /* stacked from tone r, in the chord's order */
        int32_t pc = (int32_t)((c->root + key + (uint32_t)c->iv[(r + i) % n]) % 12u);
        if (!i)
            v[0] = pc;
        else {
            v[i] = prev - (prev % 12) + pc;
            if (v[i] <= prev)
                v[i] += 12;
        }
        prev = v[i];
    }
    if (n >= 3u) {
        if (voicing == CSV_SPREAD && n == 3u)
            v[1] += 12;                             /* a triad: 1-5-10 */
        else if (voicing == CSV_OPEN || voicing == CSV_SPREAD) {
            v[n - 2u] -= 12;                        /* drop 2 */
            if (voicing == CSV_SPREAD && n >= 4u)
                v[n - 4u] -= 12;                    /* .. and 4 */
        }
        cs_sort(v, n);
    }
    shift = lo - v[0];                              /* the lowest note into lo .. lo + 11 */
    shift = shift > 0 ? (shift + 11) / 12 * 12 : -(-shift / 12 * 12);
    for (i = 0; i < n; i++)
        v[i] += shift;
    return n;
}

/* the distance between two chords: each note's distance to the nearest of the other, both ways */
static uint32_t cs_distance(const int32_t *a, uint32_t na, const uint8_t *b, uint32_t nb)
{
    uint32_t i, j, s = 0;
    for (i = 0; i < na; i++) {
        uint32_t m = 0xFFFFu;
        for (j = 0; j < nb; j++) {
            int32_t d = a[i] - (int32_t)b[j];
            uint32_t ad = (uint32_t)(d < 0 ? -d : d);
            if (ad < m)
                m = ad;
        }
        s += m;
    }
    for (j = 0; j < nb; j++) {
        uint32_t m = 0xFFFFu;
        for (i = 0; i < na; i++) {
            int32_t d = a[i] - (int32_t)b[j];
            uint32_t ad = (uint32_t)(d < 0 ? -d : d);
            if (ad < m)
                m = ad;
        }
        s += m;
    }
    return s;
}

/* chord c in key `key` (0..11, added to its root), voiced -> out (ascending, CS_VMAX), the number of notes. prev / np:
 * the chord sounding before (its notes without the bass, ascending), for INVERSION AUTO; np 0 = none */
static uint32_t cs_voice(const cs_chord_t *c, uint32_t key, uint32_t voicing, int32_t inv, int32_t lo, int bass,
                         const uint8_t *prev, uint32_t np, uint8_t *out)
{
    int32_t v[CS_MAXN], best[CS_MAXN];
    uint32_t n = c->n, r, i, m = 0, bd = 0xFFFFFFFFu, br = 0;
    if (!n)
        return 0;
    if (voicing > CSV_SPREAD)
        voicing = c->voicing;
    if (inv >= 0 || !np) {                          /* fixed, or the first chord: one inversion */
        br = inv >= 0 ? (uint32_t)inv : c->inv;
        br = br < n ? br : n - 1u;
        cs_shape_at(c, key, voicing, br, lo, best);
    } else
        for (r = 0; r < n; r++) {
            uint32_t d;
            cs_shape_at(c, key, voicing, r, lo, v);
            d = cs_distance(v, n, prev, np) * 4u + (r != c->inv) * 2u;   /* (a tie: the default inversion) */
            if (d < bd) {
                bd = d;
                br = r;
                for (i = 0; i < n; i++)
                    best[i] = v[i];
            }
        }
    if (bass) {
        int32_t pc = (int32_t)(((uint32_t)(c->bass >= 0 ? c->bass : c->root) + key) % 12u);
        int32_t b = lo - 12 + (pc - lo % 12 + 24) % 12;   /* lo - 12 .. lo - 1 */
        if (b >= 0 && b <= 127)
            out[m++] = (uint8_t)b;
    }
    for (i = 0; i < n; i++)
        if (best[i] >= 0 && best[i] <= 127 && (!m || out[m - 1u] != (uint8_t)best[i]))
            out[m++] = (uint8_t)best[i];
    return m;
}