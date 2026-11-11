/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 William Kew, ShinTech Electronics */
/* ShinTech CHORD patterns: a sequencer of chords for a CHRD SET track (docs/shintech/CHORD-PLAN.md). Included by seq.c
 * after style.c; the flash side (the bank, the cache's loads and writes) is cpat_store.c, the pages ui_cpat.c.
 * One bank of CP_N (64) patterns for the whole device (as user presets are, not a project's). A pattern (cpat_t, 328
 * bytes): LEN 1..64 steps of DIV (N_DIV, 1/4 by default), the chord set and KEY it was written with, NEXT (the pattern
 * that follows it, or itself again), up to 16 chords played by hand, and 64 steps, each:
 *   chord    REST, TIE (the chord before sounds on), chord 1..12 of the set, or hand chord H1..H16
 *   style    KEEP, OFF, or a style and variation (STYLE / VAR from this step on)
 *   note     none, or a single note over the chord (one step long)
 *   tempo    KEEP, or BPM 40..240 from this step (the internal clock only)
 * All zero is the empty pattern: LEN 16, 1/4, the track's set, every step a REST.
 * A track plays pattern CPAT (P_CPAT, 1..64; OFF none: a lock or automation may change it) while the transport runs:
 * from its step 1 at PLAY (with the sequencer), at each SONG section (the section's slot's CPAT: chain_apply), and when
 * CPAT changes. Its chords go where a chord key's would: voiced (chord.c chord_build: VOICING, INVERSION, RANGE, BASS,
 * the voice leading), to the style (style.c sty_chord) with a STYLE on, else held as a key holds them (input_on: the
 * ARP takes them), and to MIDI OUT on the track's channel. At its end it goes on with NEXT (and NEXT's own NEXT), its
 * set and KEY applied as each starts. STOP ends what it sounds.
 * Recording (the track armed, the transport running, no song): a chord key writes its chord (and STYLE / VAR then) into
 * the step playing, or the next one past its half (as live notes go into steps); the steps that start while it is held
 * become TIEs, the first after its release a REST. The white keys C5..G5 write their note. While it records, the track's
 * own steps take nothing from these keys (rec_on).
 * Memory: the bank is in flash (cpat_store.c: 7 objects of 10 patterns, A / B); the audio side reads patterns from a
 * cache of CP_SLOTS in .pool, which the main loop fills with what the tracks may play next (cpat_poll) and writes back
 * when the transport is stopped. */
#define CP_N 64u
#define CP_STEPS 64u
#define CP_HANDS 16u
#define CP_SLOTS 12u
#define CP_SRC 28u                       /* the style's "key" for a pattern's chord (keys are 1..27: style.c s->key) */
enum { CPC_REST, CPC_TIE, CPC_SET = 2, CPC_HAND = 16 };   /* cp_step_t.chord: REST, TIE, 2..13 set chord, 16..31 hand */
typedef struct { uint8_t chord, style, note, tempo; } cp_step_t;   /* style 0 KEEP, 1 OFF, 2 + style * 12 + var;
                                                                    * tempo 0 KEEP, else BPM 39 + tempo */
typedef struct { uint8_t root, iv[3]; } cp_hand_t;  /* root 0..11; iv: bit i - 1 = a note i semitones above (1..24) */
typedef struct {
    uint8_t len, div, set, key;          /* len 0: 16; set / key + 1, 0 = the track's own */
    uint8_t next, nhand, rsv[2];         /* next 1..64, 0 = this one again */
    cp_hand_t hand[CP_HANDS];
    cp_step_t step[CP_STEPS];
} cpat_t;
_Static_assert(sizeof(cpat_t) == 328u, "cpat_t: 328 bytes (docs/shintech/CHORD-PLAN.md)");

static struct { volatile uint8_t id; uint8_t dirty; uint32_t t_ms; cpat_t p; } cp_ram[CP_SLOTS]
    __attribute__((section(".pool")));
typedef struct {
    uint8_t on;                          /* playing */
    uint8_t restart;                     /* from step 1 on the next block (PLAY, a section) */
    uint8_t id, base;                    /* the pattern playing (1..64), the CPAT it started from (NEXT moves id) */
    uint8_t step;
    uint8_t sty;                         /* its chord is the style's (else held as notes: chord[]) */
    uint8_t n, chord[CHORD_NOTES];       /* the chord it holds */
    uint8_t note;                        /* a single note sounding, 0 none */
    uint8_t rec_held, rec_step, rec_rest;   /* recording: a chord key held (its step), a REST due at the next step */
    uint8_t rec_key;                     /* .. the key + 1 holding it */
    uint8_t rec_skip;                    /* step + 1 whose chord the key sounds already (recorded ahead), 0 none */
    uint8_t gen;                         /* the song section it started in (song_chain.c cp_song_gen) */
    uint32_t pos, len;                   /* samples into the step, its length */
    midi_grid_t grid;
} cp_play_t;
static cp_play_t cpp[NTRK] __attribute__((section(".pool")));
static volatile uint8_t cp_hand_in;      /* the PATTERN pages' INPUT HAND: the keys of a CHRD SET track play notes */

static uint8_t cp_ui_id;                 /* the pattern the PATTERN pages show (ui_cpat.c), 0 none */
static void cp_read(uint32_t id, cpat_t *dst);    /* (cpat_store.c) */
static cpat_t *cp_edit(uint32_t id);

static uint32_t cp_len(const cpat_t *p) { return p->len ? (uint32_t)clamp(p->len, 1, CP_STEPS) : 16u; }
static int cp_find(uint32_t id)                  /* id's cache slot, -1 none */
{
    uint32_t i;
    for (i = 0; i < CP_SLOTS; i++)
        if (id && cp_ram[i].id == id)
            return (int)i;
    return -1;
}
static cpat_t *cp_get(uint32_t id) { int i = cp_find(id); return i < 0 ? 0 : &cp_ram[i].p; }
static void cp_dirty(uint32_t id)
{
    int i = cp_find(id);
    if (i >= 0) {
        cp_ram[i].dirty = 1;
        cp_ram[i].t_ms = fm1_ms;
    }
}
/* the pattern track k plays now (a song's section, else CPAT), 0 none */
static uint32_t cp_want(uint32_t k)
{
    return chain.running ? cp_song[k] : (uint32_t)clamp(trk[k].p[P_CPAT], 0, CP_N);
}
/* the style code of a step for the track's STYLE / VAR now (cp_step_t.style) */
static uint8_t cp_style_code(const track_t *t)
{
    return t->p[P_CSTY] > 0 ? (uint8_t)(2 + (t->p[P_CSTY] - 1) * (int32_t)CST_NVAR + clamp(t->p[P_CVAR], 0, CST_NVAR - 1))
                            : 1u;
}

/* a hand chord as chordset.c's chord: its root, the notes above (at most CS_MAXN in all, the lowest) */
static void cp_hand_chord(const cp_hand_t *h, cs_chord_t *c)
{
    uint32_t i, m = h->iv[0] | (uint32_t)h->iv[1] << 8 | (uint32_t)h->iv[2] << 16;
    c->root = (uint8_t)(h->root % 12u);
    c->n = 1;
    c->iv[0] = 0;
    for (i = 0; i < 24u && c->n < CS_MAXN; i++)
        if ((m >> i) & 1u)
            c->iv[c->n++] = (int8_t)(i + 1u);
    c->bass = -1;
    c->voicing = CSV_CLOSE;
    c->inv = 0;
}
/* notes as played (ascending) -> a hand chord: the lowest one's pitch class, the rest above it within two octaves */
static void cp_hand_of(const uint8_t *notes, uint32_t n, cp_hand_t *h)
{
    uint32_t i, m = 0, d;
    h->root = (uint8_t)(notes[0] % 12u);
    for (i = 1; i < n; i++) {
        d = notes[i] - notes[0];
        while (d > 24u)
            d -= 12u;
        if (d)
            m |= 1u << (d - 1u);
    }
    h->iv[0] = (uint8_t)m;
    h->iv[1] = (uint8_t)(m >> 8);
    h->iv[2] = (uint8_t)(m >> 16);
}
/* the hand chord h in p: its index (an equal one again, else a new one), -1 when the 16 are taken */
static int cp_hand_add(cpat_t *p, const cp_hand_t *h)
{
    uint32_t i;
    for (i = 0; i < p->nhand && i < CP_HANDS; i++)
        if (!memcmp(&p->hand[i], h, sizeof *h))
            return (int)i;
    if (p->nhand >= CP_HANDS)
        return -1;
    p->hand[p->nhand] = *h;
    return p->nhand++;
}

/* --------------------------------------------------------- the player --- */
static void cp_chord_off(track_t *t, cp_play_t *c)   /* the chord it holds ends */
{
    uint32_t i;
    if (c->sty) {
        sty_t *s = &sty[trk_index(t)];
        if (s->on && (s->key == CP_SRC || (!s->key && !s->msrc)))
            sty_stop(t);
    } else
        for (i = 0; i < c->n; i++) {
            input_off(t, c->chord[i]);
            sty_midi(t, c->chord[i], 0);
        }
    c->n = 0;
    c->sty = 0;
}
static void cp_chord_on(track_t *t, cp_play_t *c, const uint8_t *notes, uint32_t n)
{
    uint32_t i;
    c->n = (uint8_t)(n < CHORD_NOTES ? n : CHORD_NOTES);
    memcpy(c->chord, notes, c->n);
    if (sty_wants(t)) {
        c->sty = 1;
        sty_chord(t, c->chord, c->n, 100, CP_SRC, 0);
        return;
    }
    c->sty = 0;
    for (i = 0; i < c->n; i++) {
        input_on(t, c->chord[i], 100);
        sty_midi(t, c->chord[i], 100);
    }
}
static void cp_note_off(track_t *t, cp_play_t *c)
{
    if (c->note) {
        trk_note_off(t, c->note);
        sty_midi(t, c->note, 0);
        c->note = 0;
    }
}
static void cp_silence(track_t *t, cp_play_t *c)    /* what it sounds ends */
{
    if (c->n)
        cp_chord_off(t, c);
    cp_note_off(t, c);
}

static void cp_apply_style(track_t *t, uint32_t code)
{
    if (code == 1u)
        t->p[P_CSTY] = 0;
    else if (code >= 2u && code < 2u + CST_NSTY * CST_NVAR) {
        t->p[P_CSTY] = (int16_t)((code - 2u) / CST_NVAR + 1u);
        t->p[P_CVAR] = (int16_t)((code - 2u) % CST_NVAR);
    }
}
static void cp_begin_pattern(track_t *t, const cpat_t *p)   /* a pattern starts: its set and KEY */
{
    if (p->set)
        t->p[P_CSET] = (int16_t)clamp(p->set - 1, 0, CS_NSETS - 1);
    if (p->key)
        t->p[P_CKEY] = (int16_t)((p->key - 1) % 12);
}

/* step c->step starts: its tempo, style, note and chord (recording: a held key's TIE, a REST after its release) */
static void cp_fire(track_t *t, cp_play_t *c, cpat_t *p)
{
    cp_step_t *st = &p->step[c->step];
    uint8_t out[CHORD_NOTES];
    uint32_t n, was = c->sty, skip = c->rec_skip == c->step + 1u;
    c->rec_skip = 0;
    if (c->rec_held && c->step != c->rec_step) {      /* recording: under a held chord key */
        st->chord = CPC_TIE;
        cp_dirty(c->id);
    } else if (c->rec_rest) {
        c->rec_rest = 0;
        st->chord = CPC_REST;
        cp_dirty(c->id);
    }
    if (st->tempo && !song.g[G_CLOCK])
        song.g[G_BPM] = (int16_t)clamp(39 + st->tempo, 40, 240);
    cp_apply_style(t, st->style);
    cp_note_off(t, c);
    if (st->note && st->note < 128u) {
        c->note = st->note;
        trk_note_on(t, c->note, 100);
        sty_midi(t, c->note, 100);
    }
    if (skip || st->chord == CPC_TIE) {
        if (c->n && !c->rec_held && (uint32_t)sty_wants(t) != was) {   /* STYLE on / off under a held chord: */
            uint8_t keep[CHORD_NOTES];                                  /* the chord again, the other way */
            uint32_t k = c->n;
            memcpy(keep, c->chord, k);
            cp_chord_off(t, c);
            cp_chord_on(t, c, keep, k);
        }
        return;
    }
    if (c->n)
        cp_chord_off(t, c);
    if (st->chord >= CPC_SET && st->chord < CPC_SET + 12u)
        n = chord_build(t, 60u + st->chord - CPC_SET, out);
    else if (st->chord >= CPC_HAND && st->chord < CPC_HAND + CP_HANDS && st->chord - CPC_HAND < p->nhand) {
        cs_chord_t hc;
        cp_hand_chord(&p->hand[st->chord - CPC_HAND], &hc);
        n = chord_build_hand(t, &hc, out);
    } else
        return;                                       /* REST */
    if (n)
        cp_chord_on(t, c, out, n);
}

static void cp_start(track_t *t, cp_play_t *c, uint32_t id)   /* pattern id from its step 1, now */
{
    cpat_t *p = cp_get(id);
    cp_silence(t, c);
    c->on = 1;
    c->restart = 0;
    c->id = c->base = (uint8_t)id;
    c->gen = cp_song_gen;
    c->step = 0;
    c->pos = 0;
    c->grid.remainder = 0;
    c->rec_held = c->rec_rest = c->rec_skip = 0;
    if (!p) {                                         /* (not in the cache yet: cpat_poll; silent until it is) */
        c->len = div_samples(0);
        return;
    }
    cp_begin_pattern(t, p);
    c->len = div_samples((uint32_t)p->div % 10u);
    cp_fire(t, c, p);
}

static void cp_stop(track_t *t)
{
    cp_play_t *c = &cpp[trk_index(t)];
    cp_silence(t, c);
    c->on = 0;
    c->rec_held = c->rec_rest = c->rec_skip = 0;
}

static void cp_restart_all(void)                     /* PLAY: every pattern from its step 1 with the sequencer */
{
    uint32_t i;
    for (i = 0; i < NTRK; i++)
        cpp[i].restart = 1;
}
static void cp_stop_all(void)                        /* STOP */
{
    uint32_t i;
    for (i = 0; i < NTRK; i++)
        if (cpp[i].on)
            cp_stop(&trk[i]);
}
static void cp_forget(uint32_t i)                    /* panic: its notes are gone (trk_all_off) */
{
    cpp[i].n = cpp[i].note = cpp[i].sty = 0;
    cpp[i].on = 0;
}
static int cp_busy(uint32_t i) { return cpp[i].on | (song.playing && cp_want(i) != 0u); }

/* once a block while it plays or may start: n samples (seq_n: the external clock's pulses with clock_mode) */
static __attribute__((noinline)) void cp_block(track_t *t, uint32_t n, uint32_t seq_n, uint32_t clock_mode)
{
    uint32_t k = trk_index(t), want = cp_want(k), adv;
    cp_play_t *c = &cpp[k];
    cpat_t *p;
    if (!song.playing || !want || !chord_set_on(t)) {
        if (c->on)
            cp_stop(t);
        return;
    }
    if (!c->on || c->restart || want != c->base || (chain.running && c->gen != cp_song_gen)) {
        cp_start(t, c, want);
        return;
    }
    p = cp_get(c->id);
    adv = clock_mode ? midi_grid_advance(&c->grid, seq_n, p ? (uint32_t)p->div % 10u : 0u) : n;
    c->pos += adv;
    while (c->pos >= c->len && c->len) {
        c->pos -= c->len;
        if (!p) {                                     /* (loaded since?) */
            cp_start(t, c, c->id);
            return;
        }
        if (++c->step >= cp_len(p)) {                 /* the end: NEXT, or again */
            c->step = 0;
            if (p->next && p->next <= CP_N && p->next != c->id) {
                cpat_t *q = cp_get(p->next);
                if (q) {
                    c->id = p->next;
                    p = q;
                    cp_begin_pattern(t, p);
                }
            }
        }
        c->len = div_samples((uint32_t)p->div % 10u);
        cp_fire(t, c, p);
    }
}

/* ------------------------------------------------------------ recording --- */
static int cp_recording(const track_t *t)
{
    uint32_t k = trk_index(t);
    return ((song.rec >> k) & 1u) && song.playing && !chain.running && cpp[k].on && cp_get(cpp[k].id) != 0;
}
/* the step a note played now goes into: the one playing, the next past its half */
static uint32_t cp_rec_target(const cp_play_t *c, const cpat_t *p)
{
    return c->pos < c->len / 2u ? c->step : (c->step + 1u) % cp_len(p);
}
static void cp_mark_set(const track_t *t, cpat_t *p)   /* a pattern written now: the track's set and KEY its own */
{
    p->set = (uint8_t)(clamp(t->p[P_CSET], 0, CS_NSETS - 1) + 1);
    p->key = (uint8_t)(clamp(t->p[P_CKEY], 0, 11) + 1);
}
/* a chord key (chord: CPC_SET + k) pressed on a recording track: into the pattern; the pattern's chord ends (the key
 * sounds it) */
static __attribute__((noinline)) void cp_rec_chord(track_t *t, uint32_t chord, uint32_t key)
{
    cp_play_t *c = &cpp[trk_index(t)];
    cpat_t *p = cp_get(c->id);
    uint32_t s;
    if (!p || !cp_recording(t))
        return;
    s = cp_rec_target(c, p);
    p->step[s].chord = (uint8_t)chord;
    p->step[s].style = cp_style_code(t);
    cp_mark_set(t, p);
    cp_dirty(c->id);
    if (c->n)
        cp_chord_off(t, c);
    c->rec_held = 1;
    c->rec_key = (uint8_t)key;
    c->rec_step = (uint8_t)s;
    c->rec_rest = 0;
    c->rec_skip = s != c->step ? (uint8_t)(s + 1u) : 0u;
}
static __attribute__((noinline)) void cp_rec_release(track_t *t, uint32_t key)   /* .. its key let go */
{
    cp_play_t *c = &cpp[trk_index(t)];
    if (!c->rec_held || c->rec_key != key)
        return;
    c->rec_held = 0;
    c->rec_rest = 1;
}
static __attribute__((noinline)) void cp_rec_note(track_t *t, uint32_t note)   /* a single note (C5..G5) on a recording track */
{
    cp_play_t *c = &cpp[trk_index(t)];
    cpat_t *p = cp_get(c->id);
    if (!p || !cp_recording(t))
        return;
    p->step[cp_rec_target(c, p)].note = (uint8_t)note;
    cp_dirty(c->id);
}
