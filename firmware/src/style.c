/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 William Kew, ShinTech Electronics */
/* ShinTech CHORD styles (CHRD SET with STYLE on: SCL > STYLE). Included by seq.c after the arp; runs in the audio
 * ISR with the rest of the note input. The data: tools/shintech/styles/<n>-<code>.json -> gen_styles.py ->
 * build/gen/chord_styles.h (its header comment has the event format).
 *   STYLE OFF ARP1 ARP2 BEAT1..3 STRM1..4 (P_CSTY), VAR 1..12 (P_CVAR), CHOLD (P_CHOLD), STRUM AUTO / 1..60 ms
 *   (P_CSTRM, AUTO: the variation's own).
 * A chord key (or a MIDI note) of a track with a style does not sound its chord: it hands the voiced chord
 * (chord.c chord_build: VOICING, INVERSION, RANGE, BASS, the voice leading) to the track's player, sty_chord. The
 * player loops the variation's steps at its rate on the song's tempo (BPM, or the external clock's pulses, as
 * the ARP: sty_advance), the track's SWING on its straight rates, and plays each step's events on the chord
 * playing: the notes they name (relative to the chord: any chord), their velocity and length, strummed a few ms
 * apart when they say so. Every note it plays goes to MIDI OUT on the track's channel too.
 *   - The first chord starts the variation at its step 1, at once. A chord played while it runs changes the
 *     chord in place: the notes of the chord before end, and the events still sounding at this point (a pad, a
 *     long stab, at least half a step of it left) strike again on the new one, without a strum.
 *   - Letting go of the chord's key stops it (its notes end), unless CHOLD is on: then it plays on, until another
 *     chord (it takes over) or CHOLD off. A key let go that no longer holds the playing chord does nothing.
 *   - CHOLD with STYLE OFF holds the chord itself: the notes a chord key started sound on after its release, until
 *     the next chord or CHOLD off (sty_latch).
 *   - PLAY restarts a running style at its step 1 with the sequencer (sty_restart). STYLE OFF, CHRD not SET or a
 *     kit stops it; another VAR or STYLE goes on at the same step (of the new loop's length).
 * Notes: each sounding or strum-pending note is a slot (sty_slot_t: its note, velocity, the samples until it starts
 * and then until it ends), 16 a track; a note struck again takes over the slot sounding it (the voice is
 * retriggered: voice.c reuses a note's voice). Off the audio ISR's loop (noinline, tests/target_budget.py): the tick
 * loop calls sty_block only while a track has a style running or a note of it sounding. */
#include "chord_styles.h"

enum { SS_A = 32, SS_B, SS_U, SS_UP = 40, SS_DN, SS_UD, SS_RND };   /* gen_styles.py's selectors */
#define STY_SLOTS 16u
#define STY_INF 0xFFFFFFFFu                /* a held chord's note: until it is let go (sty_unlatch) */
/* a step of rate r is quarter * CST_NUM[r] / CST_DEN[r] (gen_styles.py RATES' order) */
static const uint8_t CST_NUM[9] = {2, 1, 1, 1, 1, 2, 1, 1, 1};
static const uint8_t CST_DEN[9] = {1, 1, 2, 4, 8, 3, 3, 6, 12};
static const char *const CST_RATE_NAME[9] = {"1/2", "1/4", "1/8", "1/16", "1/32", "4T", "8T", "16T", "32T"};
typedef struct { uint8_t note, vel; uint16_t wait; uint32_t left; } sty_slot_t;   /* vel 0: a held chord's (CHOLD) */
typedef struct {
    uint8_t on;                  /* a style playing */
    uint8_t restart;             /* PLAY: from its step 1 on the next block */
    uint8_t n, note[CHORD_NOTES];   /* the chord, low to high */
    uint8_t vel;                 /* its velocity (100: as the variation says) */
    uint8_t key;                 /* the key + 1 holding it, 0: none (MIDI, or let go under CHOLD) */
    uint16_t msrc;               /* the MIDI note holding it: channel << 7 | note, + 1; 0 none */
    uint8_t var;                 /* the variation playing: style * 12 + variation */
    uint8_t step;                /* its step playing */
    uint8_t ai;                  /* the arp tokens' count */
    uint8_t count;               /* steps played since it began (the STYLE page's playhead, ui_graph.c graph_style) */
    uint8_t nslot;
    uint32_t pos;                /* samples into the step */
    uint32_t len;                /* the step's length (swung) */
    midi_grid_t grid;            /* the external clock's remainder (seq.c midi_grid_advance) */
    sty_slot_t slot[STY_SLOTS];
} sty_t;
static sty_t sty[NTRK] __attribute__((section(".pool")));

/* the track plays styles: STYLE on a CHRD SET track (no kit) */
static int sty_wants(const track_t *t) { return t->p[P_CSTY] > 0 && chord_set_on(t); }
static uint32_t sty_var_of(const track_t *t)
{
    return (uint32_t)(clamp(t->p[P_CSTY], 1, CST_NSTY) - 1) * CST_NVAR + (uint32_t)clamp(t->p[P_CVAR], 0, CST_NVAR - 1);
}

/* a step of the variation, straight (its length, whatever the swing) and as it plays (swung, a straight rate) */
static uint32_t sty_base(const cst_var_t *v)
{
    return beat_samples() / CST_DEN[v->rate] * CST_NUM[v->rate];
}
static uint32_t sty_step_len(const track_t *t, const cst_var_t *v, uint32_t step)
{
    uint32_t b = sty_base(v);
    return CST_DEN[v->rate] % 3u ? swing_step_len(t, b, step) : b;   /* (triplets: straight) */
}

/* the external clock's samples for its pulses (seq.c midi_grid_advance, the quarter a multiple of the rate's
 * denominator so its steps never drift from the pulses) */
static uint32_t sty_advance(sty_t *s, uint32_t n, uint32_t den)
{
    uint32_t quarter = beat_samples() / den * den, num = n * quarter + s->grid.remainder;
    s->grid.remainder = num % MIDI_CLOCK_GRID;
    return num / MIDI_CLOCK_GRID;
}

static void sty_midi(const track_t *t, uint32_t note, uint32_t vel)
{
    uint32_t mc = trk_midi_ch(trk_index(t));
    midi_out_event(vel ? 0x09u | (0x90u | mc) << 8 | note << 16 | vel << 24 : 0x08u | (0x80u | mc) << 8 | note << 16);
}

static void sty_end(track_t *t, sty_t *s, uint32_t i)    /* slot i ends (a held chord's note: as its key would) */
{
    sty_slot_t *x = &s->slot[i];
    if (!x->vel)
        input_off(t, x->note);
    else if (!x->wait)
        trk_note_off(t, x->note);
    if (!x->wait)
        sty_midi(t, x->note, 0);
    *x = s->slot[--s->nslot];
}

static void sty_end_all(track_t *t, sty_t *s, int held)   /* the style's notes end (held: a held chord's too) */
{
    uint32_t i = s->nslot;
    while (i--)
        if (held || s->slot[i].vel)
            sty_end(t, s, i);
}

static void sty_sound(track_t *t, sty_t *s, uint32_t i)   /* slot i's note starts: it takes over its note */
{
    uint32_t j = s->nslot, note = s->slot[i].note;
    while (j--)
        if (j != i && s->slot[j].note == note && !s->slot[j].wait && s->slot[j].vel) {
            sty_midi(t, note, 0);                      /* (the voice is struck again; MIDI OUT gets its off) */
            if (i == s->nslot - 1u)
                i = j;
            s->slot[j] = s->slot[--s->nslot];
        }
    trk_note_on(t, note, s->slot[i].vel);
    sty_midi(t, note, s->slot[i].vel);
}

static void sty_add(track_t *t, sty_t *s, uint32_t note, uint32_t vel, uint32_t wait, uint32_t left)
{
    sty_slot_t *x;
    if (s->nslot == STY_SLOTS) {                       /* full: the first one ends */
        if (!s->slot[0].vel)
            return;
        sty_end(t, s, 0);
    }
    x = &s->slot[s->nslot++];
    x->note = (uint8_t)note;
    x->vel = (uint8_t)vel;
    x->wait = (uint16_t)(wait < 0xFFFFu ? wait : 0xFFFFu);
    x->left = left > wait ? left - wait : 64u;
    if (!x->wait)
        sty_sound(t, s, s->nslot - 1u);
}

/* note k of the chord counted from the bottom (past the top: the chord again an octave up; below: down) */
static int32_t sty_note(const sty_t *s, int32_t k)
{
    int32_t n = s->n, o = k >= 0 ? k / n : -((-k + n - 1) / n);
    return s->note[k - o * n] + 12 * o;
}

/* event e's notes on the chord playing -> out, low to high; ->the count */
static uint32_t sty_notes(sty_t *s, const cst_var_t *v, const uint8_t *e, int32_t *out)
{
    static const int8_t OCT[4] = {0, 12, -12, 24};
    uint32_t sel = e[1] & 63u, n = 0, i, tot = s->n * v->oct, k, cyc;
    int32_t sh = OCT[e[1] >> 6];
    if (!s->n)
        return 0;
    if (sel == SS_A || sel == SS_U) {
        for (i = sel == SS_U && s->n > 1u ? 1u : 0u; i < s->n; i++)
            out[n++] = s->note[i] + sh;
        return n;
    }
    if (sel >= SS_UP) {                                /* the arp: over oct octaves of the chord */
        k = s->ai++;
        cyc = tot > 1u ? 2u * tot - 2u : 1u;
        k = sel == SS_UP ? k % tot : sel == SS_DN ? tot - 1u - k % tot
          : sel == SS_UD ? (k % cyc < tot ? k % cyc : cyc - k % cyc) : rng() % tot;
        out[0] = sty_note(s, (int32_t)k) + sh;
        return 1;
    }
    out[0] = (sel == SS_B ? s->note[0] : sel < 16u ? sty_note(s, (int32_t)sel) : sty_note(s, s->n - (int32_t)(sel - 15u)))
             + sh;
    return 1;
}

/* event e plays: left samples of it (its length; less when struck again part way) */
static void sty_event(track_t *t, sty_t *s, const cst_var_t *v, const uint8_t *e, uint32_t left, int strum)
{
    int32_t nn[CHORD_NOTES];
    uint32_t n = sty_notes(s, v, e, nn), i, vel = (uint32_t)clamp((int32_t)(e[2] * s->vel + 50u) / 100, 1, 127);
    uint32_t dir = strum ? e[3] >> 6 : 0u, ms = t->p[P_CSTRM] > 0 ? (uint32_t)t->p[P_CSTRM] : v->strum;
    uint32_t gap = dir ? ms * (FS / 100u) / 10u : 0u;
    for (i = 0; i < n; i++) {
        int32_t x = nn[dir == 2u ? n - 1u - i : i];
        if (x >= 0 && x <= 127)
            sty_add(t, s, (uint32_t)x, vel, i * gap, left);
    }
}

static uint32_t sty_ev_len(const cst_var_t *v, const uint8_t *e) { return sty_base(v) * ((e[3] & 63u) + 1u) / 4u; }

static void sty_fire(track_t *t, sty_t *s)           /* the step starts: its events */
{
    const cst_var_t *v = &CST_VAR[s->var];
    uint32_t i;
    for (i = 0; i < v->nev; i++) {
        const uint8_t *e = CST_EV[v->ev + i];
        if (e[0] == s->step)
            sty_event(t, s, v, e, sty_ev_len(v, e), 1);
    }
}

/* a new chord while it plays: what still sounds of the events now (half a step of it left at least) strikes
 * again on it, unstrummed */
static void sty_restrike(track_t *t, sty_t *s)
{
    const cst_var_t *v = &CST_VAR[s->var];
    uint32_t i, base = sty_base(v);
    for (i = 0; i < v->nev; i++) {
        const uint8_t *e = CST_EV[v->ev + i];
        uint32_t d = (s->step + v->steps - e[0]) % v->steps, at = d * base + s->pos, ln = sty_ev_len(v, e);
        if ((e[1] & 63u) < SS_UP && at < ln && ln - at >= base / 2u)
            sty_event(t, s, v, e, ln - at, 0);
    }
}

static void sty_begin(track_t *t, sty_t *s)          /* from step 1, now */
{
    const cst_var_t *v;
    s->var = (uint8_t)sty_var_of(t);
    v = &CST_VAR[s->var];
    s->step = 0;
    s->pos = 0;
    s->ai = 0;
    s->count = 0;
    s->grid.remainder = 0;
    s->len = sty_step_len(t, v, 0);
    s->restart = 0;
    sty_fire(t, s);
}

static void sty_unlatch(track_t *t)                  /* CHOLD with STYLE OFF: the held chord's notes end */
{
    sty_t *s = &sty[trk_index(t)];
    uint32_t i = s->nslot;
    while (i--)
        if (!s->slot[i].vel)
            sty_end(t, s, i);
}

/* a chord for the style (notes, n of them, low to high; vel; held by key k + 1 or MIDI msrc) */
static void sty_chord(track_t *t, const uint8_t *notes, uint32_t n, uint32_t vel, uint32_t key, uint32_t msrc)
{
    sty_t *s = &sty[trk_index(t)];
    uint32_t i, j, x;
    sty_end_all(t, s, 1);
    if (n > CHORD_NOTES)
        n = CHORD_NOTES;
    for (i = 0; i < n; i++) {
        s->note[i] = notes[i];
        for (j = i; j > 0 && s->note[j - 1u] > s->note[j]; j--) {
            x = s->note[j];
            s->note[j] = s->note[j - 1u];
            s->note[j - 1u] = (uint8_t)x;
        }
    }
    s->n = (uint8_t)n;
    s->vel = (uint8_t)clamp((int32_t)vel, 1, 127);
    s->key = (uint8_t)key;
    s->msrc = (uint16_t)msrc;
    if (!s->on) {
        s->on = 1;
        sty_begin(t, s);
    } else
        sty_restrike(t, s);
}

static void sty_stop(track_t *t)
{
    sty_t *s = &sty[trk_index(t)];
    sty_end_all(t, s, 0);
    s->on = 0;
    s->key = 0;
    s->msrc = 0;
}

/* the key k + 1 / the MIDI note msrc that holds a style chord is let go: 1 if it was the style's */
static int sty_release(track_t *t, uint32_t key, uint32_t msrc)
{
    sty_t *s = &sty[trk_index(t)];
    if (!s->on || (key ? s->key != key : !msrc || s->msrc != msrc))
        return 0;
    if (t->p[P_CHOLD]) {
        s->key = 0;
        s->msrc = 0;
    } else
        sty_stop(t);
    return 1;
}

/* CHOLD, STYLE OFF: chord key k is let go, its n notes sound on (they end as the key's would: sty_unlatch) */
static void sty_latch(track_t *t, const uint8_t *notes, uint32_t n)
{
    sty_t *s = &sty[trk_index(t)];
    uint32_t i;
    for (i = 0; i < n; i++)
        sty_add(t, s, notes[i], 0, 0, STY_INF);
}

static void sty_forget(uint32_t i)                   /* panic: its notes are gone (trk_all_off); MIDI OUT's too */
{
    sty_t *s = &sty[i];
    while (s->nslot)
        if (!s->slot[--s->nslot].wait)
            sty_midi(&trk[i], s->slot[s->nslot].note, 0);
    s->on = 0;
    s->key = 0;
    s->msrc = 0;
}

static void sty_restart(void)                        /* PLAY: a running style from its step 1 with the sequencer */
{
    uint32_t i;
    for (i = 0; i < NTRK; i++)
        if (sty[i].on)
            sty[i].restart = 1;
}

static int sty_busy(uint32_t i) { return sty[i].on | sty[i].nslot; }

/* once a block: n samples (seq_n: the external clock's pulses with clock_mode) */
static __attribute__((noinline)) void sty_block(track_t *t, uint32_t n, uint32_t seq_n, uint32_t clock_mode)
{
    sty_t *s = &sty[trk_index(t)];
    const cst_var_t *v;
    uint32_t i, adv, real;
    if (s->on && (!sty_wants(t) || (!t->p[P_CHOLD] && !s->key && !s->msrc)))
        sty_stop(t);                                   /* STYLE OFF, CHRD not SET, a kit; CHOLD off, nothing held */
    if (!s->on && s->nslot && (!t->p[P_CHOLD] || !chord_set_on(t)))
        sty_unlatch(t);                                /* (stopped, its slots are a held chord's) */
    v = &CST_VAR[s->var];
    adv = clock_mode ? sty_advance(s, seq_n, CST_DEN[v->rate]) : n;
    real = clock_mode && song.playing ? adv : n;
    for (i = s->nslot; i--;) {                         /* the notes: strums start, lengths run out */
        sty_slot_t *x = &s->slot[i];
        if (x->wait) {
            if (x->wait > real) {
                x->wait = (uint16_t)(x->wait - real);
                continue;
            }
            x->wait = 0;
            sty_sound(t, s, i);                        /* (it may drop a slot it takes over: once a block, harmless) */
        } else if (x->left != STY_INF) {
            if (x->left <= real) {
                sty_end(t, s, i);
                continue;
            }
            x->left -= real;
        }
    }
    if (!s->on)
        return;
    if (s->restart) {
        sty_end_all(t, s, 0);
        sty_begin(t, s);
        return;
    }
    if (s->var != sty_var_of(t)) {                     /* another VAR / STYLE: on at this step */
        s->var = (uint8_t)sty_var_of(t);
        v = &CST_VAR[s->var];
        s->step = (uint8_t)(s->step % v->steps);
        s->len = sty_step_len(t, v, s->step);
    }
    s->pos += adv;
    while (s->pos >= s->len) {
        s->pos -= s->len;
        s->step = (uint8_t)((s->step + 1u) % v->steps);
        s->count++;
        s->len = sty_step_len(t, v, s->step);
        sty_fire(t, s);
        if (!s->len)
            break;
    }
}
