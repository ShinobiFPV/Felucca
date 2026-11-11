/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 William Kew, ShinTech Electronics */
/* ShinTech CHORD patterns' pages (cpat.c): SEQ > CHORD PAT, PAT STEP, PAT SETUP, PAT TOOLS, after AUTOMATION, on a CHRD
 * SET track only (ui.c page_visible). They edit the pattern the selected track's CPAT names (cp_ui_id: the cache
 * holds it, cpat_store.c), at the cursor step (cpe_cur, as STEP's: KNOB 2, or the PRESETS knob):
 *   CHORD PAT   CPAT (the track's parameter: OFF, 1..64)  STEP  CHORD (REST TIE 1..12 H1..H16)  STYLE (KEEP OFF, a
 *               style and variation)
 *   PAT STEP    STEP  NOTE (none, a single note)  TEMPO (KEEP, 40..240 BPM)
 *   PAT SETUP   LEN (1..64)  DIV (1/4 ..)  NEXT (LOOP: itself, or a pattern after it)  INPUT (SET: the chord keys;
 *               HAND: the keys play their notes, a chord played on them is a hand chord)
 *   PAT TOOLS   ON (STEP: the cursor step / PAT: the whole pattern), COPY, PASTE, CLEAR (actions: the knob picks one,
 *               OCT+ does it, as on SAVE > TOOLS)
 * Step entry (the track armed, the transport stopped, as on STEP): a chord key writes its chord at the cursor (and
 * STYLE / VAR as they are then) and moves on a step; C5..G5's white keys write their note there (the cursor stays);
 * INPUT HAND: the keys held together, when the last is let go, a hand chord (the pattern keeps 16), and on a step.
 * Armed and playing, the keys record live (cpat.c). Each edit (a knob gesture, a key, a tool) is one undo: SAVE held
 * swaps the pattern back (ui.c undo, UNDO_CPAT). Included by ui.c (before the action pages' act_*); the panel
 * (graph_cpat) is ui_graph.c's, the cards ui_draw.c's, the knobs ui_input.c's. */
enum { CPF_NONE, CPF_PAT, CPF_STEP, CPF_CHORD, CPF_STYLE, CPF_NOTE, CPF_TEMPO, CPF_LEN, CPF_DIV, CPF_NEXT, CPF_INPUT, CPF_ON,
       CPF_COPY, CPF_PASTE, CPF_CLEAR };
static const uint8_t CPF_PAGE[4][4] = {
    {CPF_PAT, CPF_STEP, CPF_CHORD, CPF_STYLE},            /* CHORD PAT */
    {CPF_STEP, CPF_NOTE, CPF_TEMPO, CPF_NONE},            /* PAT STEP */
    {CPF_LEN, CPF_DIV, CPF_NEXT, CPF_INPUT},              /* PAT SETUP */
    {CPF_ON, CPF_COPY, CPF_PASTE, CPF_CLEAR},             /* PAT TOOLS */
};
static uint8_t cpe_cur;                              /* the cursor step 0..63 */
static uint8_t cpe_on_pat;                           /* PAT TOOLS' ON: 0 the step, 1 the pattern */
static struct { uint8_t kind; cp_step_t step; cp_hand_t hand; cpat_t pat; } cpe_clip
    __attribute__((section(".pool")));              /* kind: 0 empty, 1 a step (hand: its hand chord), 2 a pattern */
static uint8_t cpe_hand[8], cpe_hand_n;              /* INPUT HAND: the notes of the keys held together */
static uint32_t cpe_undo_ms, cpe_undo_tag;

static int cpe_page(uint32_t g) { return g >= GR_CPAT && g <= GR_CTOOLS; }
static uint32_t cpe_field(uint32_t slot) { return CPF_PAGE[(cur_page()->graph - GR_CPAT) & 3u][slot & 3u]; }
/* the pattern the pages edit (the selected track's CPAT), 0 none */
static cpat_t *cpe_pat(void)
{
    cp_ui_id = chord_set_on(TSEL) ? (uint8_t)clamp(TSEL->p[P_CPAT], 0, CP_N) : 0u;
    return cp_ui_id ? cp_edit(cp_ui_id) : 0;
}
static uint32_t cpe_cursor(const cpat_t *p)
{
    if (cpe_cur >= cp_len(p))
        cpe_cur = (uint8_t)(cp_len(p) - 1u);
    return cpe_cur;
}

/* the undo copy of the pattern before an edit; a knob gesture (the same tag within 1.5 s) shares one */
static void cpe_undo_take(uint32_t tag)
{
    cpat_t *p = cp_get(cp_ui_id);
    if (!p)
        return;
    if (tag && tag == cpe_undo_tag && undo.what == UNDO_CPAT && undo.trk == song.sel + 1u && undo.cp_id == cp_ui_id &&
        fm1_ms - cpe_undo_ms < 1500u) {
        cpe_undo_ms = fm1_ms;
        return;
    }
    edit_undo_take(TSEL, 0, UNDO_CPAT);
    undo.cp_id = cp_ui_id;
    undo.cpat = *p;
    cpe_undo_tag = tag;
    cpe_undo_ms = fm1_ms;
}
static void cpe_edited(cpat_t *p)                    /* written: the track's set and KEY its own, saved later */
{
    cp_mark_set(TSEL, p);
    cp_dirty(cp_ui_id);
    motion_undo_done(TSEL);
}

/* field f's value now, and its range */
static int32_t cpe_value(uint32_t f, const cpat_t *p, int32_t *lo, int32_t *hi)
{
    const cp_step_t *st = &p->step[cpe_cursor(p)];
    *lo = 0;
    switch (f) {
    case CPF_STEP:
        *hi = (int32_t)cp_len(p) - 1;
        return cpe_cur;
    case CPF_CHORD:                                   /* 0 REST, 1 TIE, 2..13 the set's, 14.. the hand chords */
        *hi = 13 + p->nhand;
        return st->chord >= CPC_HAND ? 14 + st->chord - CPC_HAND : st->chord;
    case CPF_STYLE:
        *hi = 1 + (int32_t)(CST_NSTY * CST_NVAR);
        return st->style;
    case CPF_NOTE:                                    /* 0 none, 24 (C1) .. 108 (C8) */
        *hi = 108;
        return st->note;
    case CPF_TEMPO:
        *hi = 201;
        return st->tempo;
    case CPF_LEN:
        *lo = 1;
        *hi = CP_STEPS;
        return (int32_t)cp_len(p);
    case CPF_DIV:
        *hi = 9;
        return p->div % 10u;
    case CPF_NEXT:
        *hi = CP_N;
        return p->next;
    case CPF_INPUT:
        *hi = 1;
        return cp_hand_in;
    case CPF_ON:
        *hi = 1;
        return cpe_on_pat;
    default:
        *hi = 0;
        return 0;
    }
}
static void cpe_set(uint32_t f, cpat_t *p, int32_t v, uint32_t slot)
{
    cp_step_t *st = &p->step[cpe_cursor(p)];
    int32_t lo, hi, old = cpe_value(f, p, &lo, &hi);
    v = clamp(v, lo, hi);
    if (f == CPF_NOTE && v && v < 24)
        v = old ? 0 : 24;                            /* (none below C1) */
    if (v == old)
        return;
    if (f == CPF_STEP) {
        cpe_cur = (uint8_t)v;
        return;
    }
    if (f == CPF_INPUT) {
        cp_hand_in = (uint8_t)v;
        cpe_hand_n = 0;
        return;
    }
    if (f == CPF_ON) {
        cpe_on_pat = (uint8_t)v;
        return;
    }
    cpe_undo_take(0x100u | slot | (uint32_t)cp_ui_id << 4);
    switch (f) {
    case CPF_CHORD:
        st->chord = (uint8_t)(v >= 14 ? CPC_HAND + (uint32_t)(v - 14) : (uint32_t)v);
        break;
    case CPF_STYLE:
        st->style = (uint8_t)v;
        break;
    case CPF_NOTE:
        st->note = (uint8_t)v;
        break;
    case CPF_TEMPO:
        st->tempo = (uint8_t)v;
        break;
    case CPF_LEN:
        p->len = (uint8_t)v;
        break;
    case CPF_DIV:
        p->div = (uint8_t)v;
        break;
    default:                                         /* CPF_NEXT */
        p->next = (uint8_t)v;
        break;
    }
    cpe_edited(p);
}

/* KNOB slot turned by steps on a pattern page: 1 = done here (CPAT, the track's parameter: no) */
static int cpe_knob(uint32_t slot, int32_t steps)
{
    uint32_t f = cpe_field(slot);
    cpat_t *p;
    int32_t lo, hi, v;
    if (f == CPF_PAT || f == CPF_NONE)
        return f == CPF_NONE;
    if (f >= CPF_COPY)
        return 0;                                    /* (an action column: act_cols) */
    if (!(p = cpe_pat()) || !steps)
        return 1;
    v = cpe_value(f, p, &lo, &hi);
    cpe_set(f, p, v + (f == CPF_STYLE || f == CPF_NOTE || f == CPF_TEMPO || f == CPF_STEP ?
                       accel(EN_K1 + slot, steps, hi - lo) : steps), slot);
    return 1;
}

/* a pattern step's chord name (its set and KEY, or the track's) -> b (CS_NAME_MAX); "" REST / TIE */
static void cpe_chord_name(const track_t *t, const cpat_t *p, const cp_step_t *st, char *b)
{
    cs_chord_t c;
    uint32_t key = p->key ? (uint32_t)(p->key - 1u) % 12u : (uint32_t)clamp(t->p[P_CKEY], 0, 11);
    uint32_t set = p->set ? (uint32_t)clamp(p->set - 1, 0, CS_NSETS - 1) : (uint32_t)clamp(t->p[P_CSET], 0, CS_NSETS - 1);
    b[0] = 0;
    if (st->chord >= CPC_SET && st->chord < CPC_SET + 12u) {
        if (!cs_get(set, st->chord - CPC_SET, &c))
            return;
    } else if (st->chord >= CPC_HAND && st->chord - CPC_HAND < p->nhand)
        cp_hand_chord(&p->hand[st->chord - CPC_HAND], &c);
    else
        return;
    cs_name(b, (c.root + key) % 12u, cs_mask(&c), c.bass >= 0 ? (int32_t)(((uint32_t)c.bass + key) % 12u) : -1);
}

/* card slot: its label, value (12), unit, gauge (0..1000, -1 none), icon; vo >= 0: as if it held vo (MENU > LARGE's
 * face); 0 = DIM (nothing there: REST, KEEP, none) */
static int cpe_card_at(uint32_t slot, int32_t vo, const char **label, char *val, const char **unit, int32_t *ratio,
                       uint32_t *icon)
{
    static const char *const LABEL[] = {"", "CPAT", "STEP", "CHORD", "STYLE", "NOTE", "TEMPO", "LEN", "DIV", "NEXT",
                                        "INPUT", "ON", "COPY", "PASTE", "CLEAR"};
    static char un[8];
    uint32_t f = cpe_field(slot);
    const cpat_t *p = cp_get(cp_ui_id);
    int32_t lo, hi, v;
    *label = LABEL[f];
    *unit = "";
    *ratio = -1;
    *icon = ICON_AUTO;
    val[0] = 0;
    if (!p)
        return 0;
    v = cpe_value(f, p, &lo, &hi);
    if (vo >= 0)
        v = vo;
    if (hi > lo && f != CPF_INPUT && f != CPF_ON)
        *ratio = (v - lo) * 1000 / (hi - lo);
    switch (f) {
    case CPF_STEP:
        fmt_int(val, v + 1);
        un[0] = '/';
        fmt_int(un + 1, (int32_t)cp_len(p));
        *unit = un;
        *icon = ICON_STEPS;
        return 1;
    case CPF_CHORD:
        if (v < 2)
            str_cpy(val, v ? "TIE" : "REST", 12);
        else if (v < 14)
            fmt_int(val, v - 1);
        else {
            val[0] = 'H';
            fmt_int(val + 1, v - 13);
        }
        *icon = ICON_SCALE;
        return v != 0;
    case CPF_STYLE:
        if (v < 2)
            str_cpy(val, v ? "OFF" : "KEEP", 12);
        else {
            str_cpy(val, N_CSTY[1u + (uint32_t)(v - 2) / CST_NVAR], 12);
            un[0] = 'V';
            fmt_int(un + 1, (v - 2) % (int32_t)CST_NVAR + 1);
            *unit = un;
        }
        *icon = ICON_ARP;
        return v != 0;
    case CPF_NOTE:
        if (!v)
            str_cpy(val, "--", 12);
        else {
            str_cpy(val, N_NOTE[v % 12], 12);
            fmt_int(val + str_len(val), v / 12 - 1);
        }
        *icon = ICON_PITCH;
        return v != 0;
    case CPF_TEMPO:
        if (!v)
            str_cpy(val, "KEEP", 12);
        else {
            fmt_int(val, 39 + v);
            *unit = "BPM";
        }
        *icon = ICON_TEMPO;
        return v != 0;
    case CPF_LEN:
        fmt_int(val, v);
        *icon = ICON_LENGTH;
        return 1;
    case CPF_DIV:
        str_cpy(val, N_DIV[v % 10], 12);
        *icon = ICON_DIVISION;
        return 1;
    case CPF_NEXT:
        if (!v)
            str_cpy(val, "LOOP", 12);
        else
            fmt_int(val, v);
        *icon = ICON_LOOP;
        return 1;
    case CPF_INPUT:
        str_cpy(val, v ? "HAND" : "SET", 12);
        *icon = ICON_SCALE;
        return 1;
    case CPF_ON:
        str_cpy(val, v ? "PAT" : "STEP", 12);
        *icon = ICON_X_PATTERN;
        return 1;
    default:
        return 1;
    }
}
static int cpe_card(uint32_t slot, const char **label, char *val, const char **unit, int32_t *ratio, uint32_t *icon)
{
    return cpe_card_at(slot, -1, label, val, unit, ratio, icon);
}

/* step entry (armed, stopped): pressed, the keys just pressed */
static void cpe_keys(uint32_t pressed)
{
    track_t *t = TSEL;
    cpat_t *p = cpe_pat();
    uint32_t k;
    if (!p)
        return;
    for (k = 0; k < 27u; k++) {
        cp_step_t *st;
        if (!((pressed >> k) & 1u))
            continue;
        if (cp_hand_in) {                            /* HAND: the notes held together */
            uint32_t n = kb_map(t, k);
            if (n != KB_SILENT && cpe_hand_n < sizeof cpe_hand)
                cpe_hand[cpe_hand_n++] = (uint8_t)n;
            continue;
        }
        st = &p->step[cpe_cursor(p)];
        if (k >= 7u && k <= 18u) {                   /* a chord key: its chord, on a step */
            cpe_undo_take(0);
            st->chord = (uint8_t)(CPC_SET + k - 7u);
            st->style = cp_style_code(t);
            cpe_edited(p);
            cpe_cur = (uint8_t)((cpe_cur + 1u) % cp_len(p));
        } else if (k >= 19u && !key_black(k)) {      /* C5..G5: its note, the cursor stays */
            uint32_t n = kb_map(t, k);
            if (n == KB_SILENT)
                continue;
            cpe_undo_take(0);
            st->note = (uint8_t)n;
            cpe_edited(p);
        }
    }
    if (cp_hand_in && cpe_hand_n && !(fm1_in.notes & ~kb_layer)) {   /* HAND: the last key let go */
        uint32_t i, j;
        cp_hand_t h;
        int x;
        for (i = 1; i < cpe_hand_n; i++)
            for (j = i; j > 0 && cpe_hand[j - 1u] > cpe_hand[j]; j--) {
                uint8_t y = cpe_hand[j];
                cpe_hand[j] = cpe_hand[j - 1u];
                cpe_hand[j - 1u] = y;
            }
        cp_hand_of(cpe_hand, cpe_hand_n, &h);
        cpe_hand_n = 0;
        cpe_undo_take(0);
        if ((x = cp_hand_add(p, &h)) < 0) {
            ui_message("16 HAND CHORDS: FULL");
            return;
        }
        p->step[cpe_cursor(p)].chord = (uint8_t)(CPC_HAND + (uint32_t)x);
        p->step[cpe_cur].style = cp_style_code(t);
        cpe_edited(p);
        cpe_cur = (uint8_t)((cpe_cur + 1u) % cp_len(p));
    }
}

/* PAT TOOLS: action column c (1 COPY, 2 PASTE, 3 CLEAR) would do something now */
static int cpe_act_ready(uint32_t c)
{
    const cpat_t *p = cp_get(cp_ui_id);
    uint32_t i;
    if (!p)
        return 0;
    if (c == 1u)
        return 1;
    if (c == 2u)
        return cpe_clip.kind == (cpe_on_pat ? 2u : 1u);
    if (cpe_on_pat) {
        for (i = 0; i < CP_STEPS; i++)
            if (p->step[i].chord || p->step[i].style || p->step[i].note || p->step[i].tempo)
                return 1;
        return p->len || p->div || p->next || p->nhand;
    }
    i = cpe_cursor(p);
    return p->step[i].chord || p->step[i].style || p->step[i].note || p->step[i].tempo;
}
static const char *cpe_act_name(uint32_t c) { return c == 1u ? "COPY" : c == 2u ? "PASTE" : "CLEAR"; }
static void cpe_act(uint32_t c)
{
    cpat_t *p = cpe_pat();
    cp_step_t *st;
    if (!p)
        return;
    if (!cpe_act_ready(c)) {
        ui_message(c == 2u ? "NOTHING TO PASTE" : "NOTHING TO CLEAR");
        return;
    }
    st = &p->step[cpe_cursor(p)];
    if (c == 1u) {                                   /* COPY */
        if (cpe_on_pat) {
            cpe_clip.pat = *p;
            cpe_clip.kind = 2;
        } else {
            cpe_clip.step = *st;
            if (st->chord >= CPC_HAND && st->chord - CPC_HAND < p->nhand)
                cpe_clip.hand = p->hand[st->chord - CPC_HAND];
            cpe_clip.kind = 1;
        }
        ui_message(cpe_on_pat ? "PATTERN COPIED" : "STEP COPIED");
        return;
    }
    cpe_undo_take(0);
    if (c == 2u && cpe_on_pat) {                     /* PASTE */
        *p = cpe_clip.pat;
        ui_message("PATTERN PASTED");
    } else if (c == 2u) {
        cp_step_t s = cpe_clip.step;
        if (s.chord >= CPC_HAND) {                   /* (its hand chord into this pattern's) */
            int x = cp_hand_add(p, &cpe_clip.hand);
            if (x < 0) {
                ui_message("16 HAND CHORDS: FULL");
                return;
            }
            s.chord = (uint8_t)(CPC_HAND + (uint32_t)x);
        }
        *st = s;
        cpe_cur = (uint8_t)((cpe_cur + 1u) % cp_len(p));
        ui_message("STEP PASTED");
    } else if (cpe_on_pat) {                         /* CLEAR */
        memset(p, 0, sizeof *p);
        ui_message("PATTERN CLEARED");
    } else {
        memset(st, 0, sizeof *st);
        ui_message("STEP CLEARED");
    }
    cp_dirty(cp_ui_id);
    motion_undo_done(TSEL);
}
