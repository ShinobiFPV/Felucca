/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 William Kew, ShinTech Electronics */
/* OPN2's operator pages and charts (ShinTech 16BIT): the EDIT family's OPERATOR, OP ENV and OPERATOR 2 pages (FM6's,
 * params.c PAGES: shown for an OPN2 track too, ui.c page_visible) edit the four operators of the track's own patch
 * (eng_opn2.c op2_patch) on the device, as ui_fm6op.c does FM6's. KNOB 1 is the operator (OP 1..4, kept while one goes
 * round the three pages), KNOB 2..4 its fields:
 *   OPERATOR    OP  MUL (0.5, 1..15)    DT (-3..+3)       TL (0..127)          RATIO mode patches: RATIO, FINE
 *   OP ENV      OP  STAGE (ATK DEC SUS REL)  RATE (AR DR SR 0..31, RR 0..15)  LEVEL (DEC: SL 0..15; else none)
 *   OPERATOR 2  OP  DT                  KS (0..3)         AM (OFF / ON)
 * (SSG-EG and CH3 mode's operator offsets: the web editor's.) A detent writes the patch (op2_set_patch: the audio ISR
 * takes it at its next block) and makes it the track's own: SLOT OWN; each knob gesture is one undo (ui.c
 * edit_undo_take: the patch and SLOT with the sound). Nothing here is automatable or lockable; the values are the
 * patch's own, EDIT 1 / 2's macros apply on top (ALG, FB, MLVL, MEG, VSEN).
 * The panels: OPERATOR and OPERATOR 2 the algorithm chart (graph_opn2) with the operator picked in the accent and its
 * frequency top right; OP ENV its envelope (graph_opn2env), the other three faint behind it, the stage picked in the
 * accent. EDIT 1 / 2 of an OPN2 track: the chart, the knob turned in the accent. Included by ui_graph.c after FM6's
 * charts (their parts: fm_route, fm_loop, fm_bus); the cards: ui_draw.c, the knobs: ui_input.c edit_param. */
#define O2_OFF(f) ((uint32_t)__builtin_offsetof(op2_op_t, f))   /* a field's byte in op2_op_t */
static uint8_t o2p_op;                               /* the operator shown: 0..3 = OP 1..4 (op2_patch_t's order) */
static uint8_t o2p_stg;                              /* OP ENV: the stage, 0..3 ATK DEC SUS REL */

enum { O2_OP, O2_STG, O2_MUL, O2_DTF, O2_TL, O2_RATE, O2_LVL, O2_DT, O2_KS, O2_AM };
static const uint8_t O2_PAGE[3][4] = {
    {O2_OP, O2_MUL, O2_DTF, O2_TL},                  /* OPERATOR */
    {O2_OP, O2_DT, O2_KS, O2_AM},                    /* OPERATOR 2 */
    {O2_OP, O2_STG, O2_RATE, O2_LVL},                /* OP ENV */
};

static int o2p_ok(void) { return TSEL->eng_req % NENGINES == ENGI_OPN2; }
static uint32_t o2p_field(uint32_t slot)
{
    uint32_t g = cur_page()->graph;
    return O2_PAGE[g == GR_FMOP2 ? 1u : g == GR_FMENV ? 2u : 0u][slot & 3u];
}
static const op2_patch_t *o2p_patch(uint32_t tr) { return &op2_patch[tr % NTRK]; }

/* field f of the selected operator: its byte's offset in op2_op_t and its largest value; -1: none (LEVEL off DEC) */
static int32_t o2p_where(const op2_patch_t *p, uint32_t f, uint32_t *max)
{
    static const uint8_t RATE_OFF[4] = {O2_OFF(ar), O2_OFF(dr), O2_OFF(sr),
                                        O2_OFF(rr)};
    switch (f) {
    case O2_MUL: *max = p->ratio ? 31u : 15u; return p->ratio ? (int32_t)O2_OFF(rcoarse) : (int32_t)O2_OFF(mul);
    case O2_DTF: if (p->ratio) { *max = 99u; return (int32_t)O2_OFF(rfine); }
                 /* fallthrough: DT */
    case O2_DT: *max = 6u; return (int32_t)O2_OFF(dt);
    case O2_TL: *max = 127u; return (int32_t)O2_OFF(tl);
    case O2_RATE: *max = (o2p_stg & 3u) == 3u ? 15u : 31u; return RATE_OFF[o2p_stg & 3u];
    case O2_LVL: *max = 15u; return (o2p_stg & 3u) == 1u ? (int32_t)O2_OFF(sl) : -1;
    case O2_KS: *max = 3u; return (int32_t)O2_OFF(ks);
    case O2_AM: *max = 1u; return (int32_t)O2_OFF(am);
    default: *max = 0; return -1;
    }
}
/* DT: the register's 0..3 = +0..+3, 4..7 = -0..-3; as a knob position 0..6 = -3..+3 */
static uint32_t o2p_dt_pos(uint32_t r) { return r < 4u ? r + 3u : (r > 4u ? 7u - r : 3u); }
static uint32_t o2p_dt_reg(uint32_t pos) { return pos >= 3u ? pos - 3u : 7u - pos; }
static int o2p_is_dt(const op2_patch_t *p, uint32_t f) { return f == O2_DT || (f == O2_DTF && !p->ratio); }

/* the knob position of field f (the byte, or DT's 0..6) */
static uint32_t o2p_pos(uint32_t tr, uint32_t f)
{
    const op2_patch_t *p = o2p_patch(tr);
    uint32_t max;
    int32_t off = o2p_where(p, f, &max);
    uint32_t v = off < 0 ? 0u : ((const uint8_t *)&p->op[o2p_op & 3u])[off];
    return o2p_is_dt(p, f) ? o2p_dt_pos(v & 7u) : v;
}

/* field f of the selected track's patch = position pos: the track's own patch now (SLOT OWN), one undo per gesture */
static void o2p_write(uint32_t f, uint32_t pos)
{
    uint32_t tr = song.sel % NTRK, max;
    track_t *t = TSEL;
    op2_patch_t p = *o2p_patch(tr);
    int32_t off = o2p_where(&p, f, &max);
    uint8_t *b;
    if (off < 0)
        return;
    b = (uint8_t *)&p.op[o2p_op & 3u] + off;
    pos = o2p_is_dt(&p, f) ? o2p_dt_reg(pos) : pos;
    if (*b == pos)
        return;
    edit_undo_take(t, 0x2000u | (uint32_t)(o2p_op & 3u) << 5 | (uint32_t)off, UNDO_SOUND);   /* (one copy per byte) */
    *b = (uint8_t)pos;
    op2_set_patch(tr, &p);
    op2_own_ok &= (uint8_t)~(1u << tr);              /* (an own patch kept aside for SLOT: this one replaces it) */
    op2_slot[tr] = (uint8_t)OP2_OWN;                 /* (op2_poll: nothing reloads over it) */
    t->p[P_E7] = (int16_t)OP2_OWN;
    motion_undo_done(t);
}

/* KNOB slot turned by steps on an operator page */
static void o2p_knob(uint32_t slot, int32_t steps)
{
    uint32_t f = o2p_field(slot), max;
    if (!o2p_ok() || !steps)
        return;
    if (f == O2_OP) {
        o2p_op = (uint8_t)clamp((int32_t)o2p_op + steps, 0, 3);
        return;
    }
    if (f == O2_STG) {
        o2p_stg = (uint8_t)clamp((int32_t)o2p_stg + steps, 0, 3);
        return;
    }
    if (o2p_where(o2p_patch(song.sel), f, &max) < 0)
        return;
    o2p_write(f, (uint32_t)clamp((int32_t)o2p_pos(song.sel, f) + accel(EN_K1 + slot, steps, (int32_t)max), 0,
                                 (int32_t)max));
}

/* the operator's frequency, s holds 12: "x2" (MUL; 0 = x0.5), "x1.50" (RATIO mode: rcoarse.rfine; 0.00: MUL) */
static void o2p_freq(const op2_patch_t *p, uint32_t k, char *s)
{
    const op2_op_t *o = &p->op[k & 3u];
    s[0] = 'x';
    if (p->ratio && (o->rcoarse || o->rfine)) {
        fop_dec(s + 1, o->rcoarse, o->rfine % 100u, 2u);
        return;
    }
    if (!o->mul)
        str_cpy(s + 1, "0.5", 4);
    else
        fmt_int(s + 1, o->mul & 15u);
}
/* "+2", "-1", "0" */
static void o2p_signed(char *s, int32_t v)
{
    s[0] = v > 0 ? '+' : 0;
    fmt_int(s + (v > 0), v);
}

/* card slot of an operator page: its label, value (12), unit, gauge (0..1000, -1 none), icon; 0 = shown DIM (TL 127:
 * the operator silent; LEVEL on a stage without one). o2p_card_at: as if the field were at position vo (>= 0; MENU >
 * LARGE's face, ui_draw.c span_o2p) */
static int o2p_card_at(uint32_t slot, int32_t vo, const char **label, char *val, const char **unit, int32_t *ratio,
                       uint32_t *icon)
{
    static const char *const STG[4] = {"ATK", "DEC", "SUS", "REL"};
    static const char *const RATE[4] = {"AR", "DR", "SR", "RR"};
    static const uint32_t STG_ICON[4] = {ICON_ATTACK, ICON_DECAY, ICON_SUSTAIN, ICON_RELEASE};
    const op2_patch_t *p = o2p_patch(song.sel);
    uint32_t f = o2p_field(slot), v, max = 0;
    *unit = "";
    *ratio = -1;
    if (f == O2_OP) {
        *label = "OP";
        fmt_int(val, (int32_t)o2p_op + 1);
        *unit = "/4";
        *ratio = (int32_t)o2p_op * 333;
        *icon = ICON_ALGORITHM;
        return 1;
    }
    if (f == O2_STG) {
        *label = "STAGE";
        str_cpy(val, STG[o2p_stg & 3u], 12);
        *ratio = (int32_t)o2p_stg * 333;
        *icon = ICON_ENV;
        return 1;
    }
    if (o2p_where(p, f, &max) < 0) {                 /* LEVEL off DEC: nothing to set */
        *label = "LEVEL";
        str_cpy(val, "-", 12);
        *icon = ICON_LEVEL;
        return 0;
    }
    v = vo >= 0 ? (uint32_t)vo : o2p_pos(song.sel, f);
    *ratio = max ? (int32_t)(v * 1000u / max) : -1;
    fmt_int(val, (int32_t)v);
    if (o2p_is_dt(p, f)) {
        *label = "DT";
        o2p_signed(val, (int32_t)v - 3);
        *icon = ICON_DETUNE;
        return 1;
    }
    switch (f) {
    case O2_MUL:
        *icon = ICON_RATIO;
        if (p->ratio) {
            *label = "RATIO";
            fop_dec(val, v, p->op[o2p_op & 3u].rfine % 100u, 2u);
        } else {
            *label = "MUL";
            if (!v)
                str_cpy(val, "0.5", 12);
        }
        return 1;
    case O2_DTF:                                     /* (RATIO mode: FINE) */
        *label = "FINE";
        *icon = ICON_TUNE;
        return 1;
    case O2_TL:
        *label = "TL";
        *icon = ICON_LEVEL;
        return v < 127u;
    case O2_RATE:
        *label = RATE[o2p_stg & 3u];
        *icon = STG_ICON[o2p_stg & 3u];
        return 1;
    case O2_LVL:
        *label = "SL";
        *icon = ICON_LEVEL;
        return 1;
    case O2_KS:
        *label = "KS";
        *icon = ICON_KEYTRACK;
        return 1;
    default:                                         /* O2_AM */
        *label = "AM";
        str_cpy(val, v ? "ON" : "OFF", 12);
        *icon = ICON_LFO_WAVE;
        return 1;
    }
}
static int o2p_card(uint32_t slot, const char **label, char *val, const char **unit, int32_t *ratio, uint32_t *icon)
{
    return o2p_card_at(slot, -1, label, val, unit, ratio, icon);
}
/* the card's knob range (MENU > LARGE: ui_draw.c card_span_by); 0 = none (OP, STAGE, LEVEL off DEC) */
static uint32_t o2p_span(uint32_t slot)
{
    uint32_t f = o2p_field(slot), max = 0;
    if (f == O2_OP || f == O2_STG || o2p_where(o2p_patch(song.sel), f, &max) < 0)
        return 0;
    return max;
}

/* ---------------------------------------------------------------- the chart --- */
/* The YM2612's eight algorithms as FM charts (the parts and the rules of FM6's, graph_fm6). O2_CELL: per operator 1..4
 * its cell, the column in 24 px steps (bits 0..2) and the row above the output bus (bits 4..5; row 0 = the carriers);
 * O2_MOD: per operator k the operators modulating it, bit j = operator j + 1 (the chip's algorithms, OP1 feeding
 * itself); the carriers: eng_opn2.c OP2_CARRIERS. ui_test.c checks them against each other (a modulator one row above
 * what it modulates, the carriers on row 0 and only they, one operator per cell) */
static const uint8_t O2_CELL[8][4] = {
    {0x30, 0x20, 0x10, 0x00},                        /* 1: 1 > 2 > 3 > 4 */
    {0x20, 0x22, 0x11, 0x01},                        /* 2: (1 + 2) > 3 > 4 */
    {0x10, 0x22, 0x12, 0x01},                        /* 3: (1 + (2 > 3)) > 4 */
    {0x20, 0x10, 0x12, 0x01},                        /* 4: ((1 > 2) + 3) > 4 */
    {0x10, 0x00, 0x12, 0x02},                        /* 5: (1 > 2) + (3 > 4) */
    {0x12, 0x00, 0x02, 0x04},                        /* 6: 1 > (2, 3, 4) */
    {0x10, 0x00, 0x02, 0x04},                        /* 7: (1 > 2) + 3 + 4 */
    {0x00, 0x02, 0x04, 0x06},                        /* 8: 1 + 2 + 3 + 4 */
};
static const uint8_t O2_MOD[8][4] = {
    {0, 1, 2, 4}, {0, 0, 3, 4}, {0, 0, 2, 5}, {0, 1, 0, 6}, {0, 1, 0, 4}, {0, 1, 1, 1}, {0, 1, 0, 0}, {0, 0, 0, 0},
};
/* the algorithm OPN2 plays on track t (0..7): ALG, or the patch's at PAT */
static uint32_t o2p_alg_of(const track_t *t)
{
    return t->p[P_E0] >= 1 && t->p[P_E0] <= 8 ? (uint32_t)t->p[P_E0] - 1u : op2_patch[(t - trk) % NTRK].alg & 7u;
}

/* OPN2's chart: "ALG 3" top left; sel < 4 (the operator pages): that operator in the accent and "OP2 x2 DT+1" top
 * right, else (EDIT 1 / 2) the patch's name. Carriers THEME with INK numerals, modulators RAISE with THEME ones, an
 * operator at TL 127 a DIM numeral; routes THEME, OP1's feedback loop MID (DIM at feedback 0), the bus MID. The knob
 * just turned in the accent: ALG and SLOT the label, FB the loop, MLVL and MEG the modulators, VSEN the carriers */
static void graph_opn2(const track_t *t, uint16_t c, uint32_t sel)
{
    uint32_t alg = o2p_alg_of(t), k, j, car = OP2_CARRIERS[alg], top = 0, lo = 7, hi = 0, hotset = 0;
    uint32_t hot = ui.hot_t ? (uint32_t)cur_page()->id[ui.hot_col & 3u] : 0u;
    const op2_patch_t *p = &op2_patch[(t - trk) % NTRK];
    int32_t x[4], y[4], bus, cl = 240, cr = 0, fb = clamp((int32_t)p->fb + t->p[P_E1], 0, 7);
    uint8_t fan[4] = {0, 0, 0, 0};
    uint16_t rc = c;
    char b[7] = {'A', 'L', 'G', ' ', (char)('1' + alg), 0};
    if (hot == P_E2 || hot == P_E3)
        hotset = 15u & ~car;
    else if (hot == P_E4)
        hotset = car;
    for (k = 0; k < 4u; k++) {
        uint32_t col = O2_CELL[alg][k] & 7u, row = O2_CELL[alg][k] >> 4;
        lo = col < lo ? col : lo;
        hi = col > hi ? col : hi;
        top = row > top ? row : top;
        for (j = 0; j < 4u; j++)
            fan[j] += (uint8_t)(O2_MOD[alg][k] >> j & 1u);
    }
    bus = 50 + (27 + 23 * (int32_t)top) / 2;         /* the chart centred on the 100 px scale */
    for (k = 0; k < 4u; k++) {
        x[k] = 120 + (int32_t)(O2_CELL[alg][k] & 7u) * 24 - (int32_t)(lo + hi) * 12;
        y[k] = bus - 6 - FM_BH - 23 * (int32_t)(O2_CELL[alg][k] >> 4);
        if (car >> k & 1u) { cl = x[k] < cl ? x[k] : cl; cr = x[k] > cr ? x[k] : cr; }
    }
    for (k = 0; k < 4u; k++) {                       /* into k: its sources down to its jog row, across, down */
        uint32_t m = O2_MOD[alg][k];
        for (j = 0; j < 4u; j++)
            if (m >> j & 1u) {
                fm_route(x[j], y[j] + FM_BH, x[k], y[k] - 3, y[k], rc);
                if (fan[j] > 1u) fm_dot(x[j], y[k] - 3, rc);   /* one operator modulating several */
                if (m & (m - 1u)) fm_dot(x[k], y[k] - 3, rc);  /* several modulating one */
            }
        if (car >> k & 1u) {
            cv_rect(x[k], y[k] + FM_BH, 1, bus - y[k] - FM_BH, T_MID);
            if (x[k] != cl) fm_dot(x[k], bus, T_MID);
        }
    }
    fm_bus(cl, cr, bus);
    fm_loop(x[0], y[0], hot == P_E1 ? T_ACCENT : fb ? T_MID : T_DIM);
    for (k = 0; k < 4u; k++) {
        uint32_t on = p->op[k].tl < 127u, fill_c = (car >> k & 1u) && on;
        uint16_t f = hotset >> k & 1u ? T_ACCENT : c, fill = k == sel ? T_ACCENT : fill_c ? f : T_RAISE;
        char n[2] = {(char)('1' + k), 0};
        cv_rrect(x[k] - 10, y[k], 21, FM_BH, 3, fill, T_SURF);
        GFX_HOOK_ALIGN(x[k] - 10, y[k], x[k] + 11, y[k] + FM_BH, AL_HV, "OPN2 operator number in its box");
        cv_text_in(x[k] - 10, y[k] + CAP_IN(S, FM_BH), 21, &AF_S, n, fill_c || k == sel ? T_INK : on ? f : T_DIM, fill);
    }
    cv_text(10, 4 - GOY, &AF_S, b, hot == P_E0 || hot == P_E7 ? T_ACCENT : T_MID);
    if (sel < 4u) {                                  /* the operator picked: "OP2 x2 DT+1" */
        char fq[24] = {'O', 'P', (char)('1' + sel), ' ', 0};
        uint32_t d = o2p_dt_pos(p->op[sel].dt & 7u);
        o2p_freq(p, sel, fq + 4);
        if (d != 3u) {
            str_cpy(fq + str_len(fq), " DT", 4);
            o2p_signed(fq + str_len(fq), (int32_t)d - 3);
        }
        cv_text_r(228, 4 - GOY, &AF_S, fq, T_TEXT, T_SURF);
    } else {                                         /* EDIT 1 / 2: the patch's name */
        char nm[17];
        for (k = 0; k < 16u; k++)
            nm[k] = p->name[k] >= ' ' && p->name[k] < 127 ? p->name[k] : ' ';
        for (nm[16] = 0; k && nm[k - 1] == ' '; k--)
            nm[k - 1] = 0;
        cv_text_r(228, 4 - GOY, &AF_S, nm, T_TEXT, T_SURF);
    }
}

/* ------------------------------------------------------------- the envelope --- */
/* An operator's envelope as the chip runs it, drawn in dB (the chip's attenuation is linear in dB: the decays are
 * straight lines; the attack, an exponential approach, a bow): from silence up at AR to the peak, down at DR to SL
 * (3 dB a step; 15: silence), on down at SR while the key is held (SR 0: held at SL), from the key let go (a RAISE
 * mark) down at RR to silence. A stage's width shrinks as its rate rises (4..44 px; 31 (RR 15) a step; 0: never: the
 * stage holds to the end). x[] / y[]: the five points silence, peak, SL, the key let go, silence */
static void o2p_env_points(const op2_op_t *o, int32_t top, int32_t bot, int32_t *x, int32_t *y)
{
#define O2W(r) (4 + (31 - (int32_t)(r)) * (31 - (int32_t)(r)) * 40 / 961)
    int32_t sl = (o->sl & 15u) == 15u ? bot : top + (int32_t)(o->sl & 15u) * 3 * (bot - top) / 96, rel = 228;
    uint32_t rr = (o->rr & 15u) * 2u + 1u;           /* (RR's rate: 2 RR + 1 on the chip's 0..31 scale) */
    x[4] = rel;
    x[3] = rel - O2W(rr);
    y[0] = bot;
    x[0] = 12;
    if (!o->ar) {                                    /* AR 0: it never sounds */
        x[1] = x[2] = x[3];
        y[1] = y[2] = y[3] = y[4] = bot;
        return;
    }
    x[1] = x[0] + O2W(o->ar);
    y[1] = top;
    if (!o->dr) {                                    /* DR 0: it holds at the peak */
        x[2] = x[3];
        y[2] = y[3] = top;
    } else {
        x[2] = x[1] + O2W(o->dr);
        y[2] = sl;
        y[3] = o->sr ? sl + (x[3] - x[2]) * (bot - top) / (4 * O2W(o->sr)) : sl;   /* SR: on down while held */
        y[3] = y[3] > bot ? bot : y[3];
    }
    y[4] = bot;
#undef O2W
}
/* the attack's bow from (x0, bot) up to (x1, top): the chip's attack steps the attenuation by a part of itself, an
 * exponential approach in dB, steep at first */
static void o2p_attack(int32_t x0, int32_t x1, int32_t top, int32_t bot, uint16_t c, uint32_t w)
{
    int32_t k, n = x1 - x0, px = x0, py = bot;
    if (n < 2) {
        cv_line_t(x0, bot, x1, top, c, w);
        return;
    }
    for (k = 1; k <= n; k++) {                       /* y = bot - (bot - top) (1 - (1 - k / n)^3) */
        int32_t r = n - k, yy = bot - (bot - top) + (bot - top) * r * r / n * r / n / n;
        cv_line_t(px, py, x0 + k, yy, c, w);
        px = x0 + k;
        py = yy;
    }
}
static void o2p_env_draw(const op2_op_t *o, int32_t top, int32_t bot, uint16_t c, uint16_t hc, int32_t hs, uint32_t w)
{
    int32_t x[5], y[5], k;
    o2p_env_points(o, top, bot, x, y);
    if (o->ar)
        o2p_attack(x[0], x[1], top, bot, hs == 0 ? hc : c, w);
    else
        cv_line_t(x[0], bot, x[3], bot, hs == 0 ? hc : c, w);
    for (k = 1; k < 4; k++)                          /* DEC, SUS (the hold), REL */
        if (x[k + 1] > x[k] || y[k + 1] != y[k])
            cv_line_t(x[k], y[k], x[k + 1], y[k + 1], hs == k ? hc : c, w);
}
/* OP ENV (ui_opn2op.c): the picked operator's envelope, THEME, the stage STAGE picks in the accent with a dot where it
 * ends; the other three operators faint (RAISE) behind it; "OP2" top left, the stage's values top right */
static void graph_opn2env(const track_t *t, uint16_t c)
{
    static const char *const STG[4] = {"ATK  AR ", "DEC  DR ", "SUS  SR ", "REL  RR "};
    const op2_patch_t *p = &op2_patch[(t - trk) % NTRK];
    const op2_op_t *o = &p->op[o2p_op & 3u];
    int32_t top = 24, bot = 88, x[5], y[5], k;
    uint32_t sg = o2p_stg & 3u;
    char b[24] = {'O', 'P', (char)('1' + (o2p_op & 3u)), 0};
    for (k = 0; k < 4; k++)
        if ((uint32_t)k != (o2p_op & 3u))
            o2p_env_draw(&p->op[k], top, bot, T_RAISE, T_RAISE, -1, 1);
    cv_rect(PANEL_X0, bot + 2, PANEL_W, 1, T_RAISE);
    o2p_env_points(o, top, bot, x, y);
    cv_rect(x[3], top, 1, bot - top + 1, T_RAISE);   /* the key let go */
    o2p_env_draw(o, top, bot, c, T_ACCENT, (int32_t)sg, 2);
    k = sg + 1u;
    fm_dot(x[k], y[k], T_ACCENT);
    cv_text(10, 4 - GOY, &AF_S, b, T_MID);
    str_cpy(b, STG[sg], sizeof b);
    fmt_int(b + str_len(b), sg == 0u ? o->ar : sg == 1u ? o->dr : sg == 2u ? o->sr : o->rr);
    if (sg == 1u) {
        str_cpy(b + str_len(b), "  SL ", 6);
        fmt_int(b + str_len(b), o->sl);
    }
    cv_text_r(228, 4 - GOY, &AF_S, b, T_TEXT, T_SURF);
}
