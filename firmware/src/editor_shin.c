/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 William Kew, ShinTech Electronics */
/* ShinTech CHORD's editor commands (90..96; web/EDITOR_PROTOCOL.md "ShinTech: chord sets, styles, chord patterns"):
 * the chord sets (the 100 factory ones and the user's 16), the styles (the 9 factory ones and USER's 12 variations)
 * and the 64 chord patterns, read and written by the web editor. INFO advertises `59 01 1`. Included by editor.c.
 * A set index is 0..CS_NALL - 1 (factory, then USR1..USR16); a style 0..9 (9 USER), a variation 0..11; a pattern 1..64.
 * A chord on the wire: root 0..11, bass (0 none, 1..12: its pitch class + 1), voicing 0..2, inversion 0..3, then the
 * notes above the root as a 24-bit mask in four 7-bit bytes (bit i - 1: i semitones, 1..24). An event: step, notes
 * (gen_styles.py's selectors: 0..15, 16..23 from the top, 32 A, 33 B, 34 U, 40..43 the arp), octave (0 none, 1 up,
 * 2 down, 3 two up), velocity 1..127, length in quarter steps - 1 (0..63), strum (0 none, 1 down, 2 up).
 * Writes of the user's sets and style go to flash (the transport stopped first, as every editor write); a pattern goes
 * into the patterns' cache and is written as the device writes its own edits (cpat_store.c: when quiet). */
enum { ED_CS_LIST = 90, ED_CS_GET, ED_CS_PUT, ED_STY_GET, ED_STY_PUT, ED_CPAT_GET, ED_CPAT_PUT };

static void ed_shin_mask(uint32_t m)            /* a 24-bit mask as four 7-bit bytes */
{
    ed_b(m); ed_b(m >> 7); ed_b(m >> 14); ed_b(m >> 21);
}
static int ed_shin_name(const uint8_t *a, uint32_t n, char *out)   /* a 0-ended name of 1..12 printable bytes: its length + 1 */
{
    uint32_t i;
    for (i = 0; i < n && a[i]; i++)
        if (i >= CS_NAME_LEN || a[i] < 32u || a[i] > 126u)
            return 0;
    if (i == n || !i)
        return 0;
    memset(out, 0, CS_NAME_LEN);
    memcpy(out, a, i);
    return (int)i + 1;
}

static void ed_cs_get(uint32_t s)
{
    cs_chord_t c;
    char nm[CS_NAME_LEN + 1u];
    uint32_t k, i, m;
    ed_b(s);
    if (s >= CS_NALL) { ed_b(1); return; }
    if (!cs_set_ok(s)) { ed_b(2); return; }
    ed_b(0);
    cs_set_name(s, nm);
    ed_str(nm, CS_NAME_LEN + 1u);
    ed_b(s < CS_NSETS ? CS_SET[s].genre : CS_NGENRES);
    for (k = 0; k < 12u; k++) {
        (void)cs_get(s, k, &c);
        for (i = 1, m = 0; i < c.n; i++)
            if (c.iv[i] >= 1 && c.iv[i] <= 24)
                m |= 1u << (c.iv[i] - 1);
        ed_b(c.root); ed_b((uint32_t)(c.bass + 1)); ed_b(c.voicing); ed_b(c.inv);
        ed_shin_mask(m);
    }
}
/* user set u, used, name, 12 chords -> 0 ok, 1 arguments, 2 flash, 3 the transport did not stop */
static uint32_t ed_cs_put(const uint8_t *a, uint32_t n)
{
    cs_uset_t s;
    uint32_t k, at;
    int l;
    if (n < 2u || a[0] >= CS_NUSER || a[1] > 1u)
        return 1;
    memset(&s, 0, sizeof s);
    if (a[1]) {
        if ((l = ed_shin_name(a + 2, n - 2u, s.name)) == 0 || n != 2u + (uint32_t)l + 12u * 8u)
            return 1;
        for (k = 0, at = 2u + (uint32_t)l; k < 12u; k++, at += 8u) {
            const uint8_t *c = a + at;
            uint32_t m = c[4] | (uint32_t)c[5] << 7 | (uint32_t)c[6] << 14 | (uint32_t)c[7] << 21;
            if (c[0] > 11u || c[1] > 12u || c[2] > 2u || c[3] > 3u || c[7] > 7u)
                return 1;
            s.ch[k] = (cs_uchord_t){c[0], c[1], (uint8_t)(c[2] | c[3] << 2), {(uint8_t)m, (uint8_t)(m >> 8), (uint8_t)(m >> 16)}};
        }
        s.used = 1;
    } else if (n != 2u)
        return 1;
    if (ed_flash_stop())
        return 3;
    cs_user.set[a[0]] = s;
    cs_user.magic = CS_USER_MAGIC;
    ui.force = 1;
    return user_save(OBJ_UCSET) ? 2u : 0u;
}

static void ed_sty_get(uint32_t st, uint32_t k)
{
    uint32_t var = st * CST_NVAR + k, i;
    const cst_var_t *v;
    char nm[CST_NAME_LEN + 1u];
    ed_b(st); ed_b(k);
    if (st > CST_NSTY || k >= CST_NVAR) { ed_b(1); return; }
    v = sty_v(var);
    if (v == &STY_EMPTY) { ed_b(2); return; }
    ed_b(0);
    memcpy(nm, v->name, CST_NAME_LEN);
    nm[CST_NAME_LEN] = 0;
    ed_str(nm, CST_NAME_LEN + 1u);
    ed_b(v->steps); ed_b(v->rate); ed_b(v->strum); ed_b(v->oct); ed_b(v->nev);
    for (i = 0; i < v->nev; i++) {
        const uint8_t *e = sty_e(var, i);
        ed_b(e[0]); ed_b(e[1] & 63u); ed_b(e[1] >> 6); ed_b(e[2]); ed_b(e[3] & 63u); ed_b(e[3] >> 6);
    }
}
static int ed_sty_sel_ok(uint32_t s) { return s <= 23u || (s >= 32u && s <= 34u) || (s >= 40u && s <= 43u); }
/* USER variation k: name, steps (0: none, it is erased), rate, strum, oct, nev, nev x 6 -> 0 ok, 1 args, 2 flash, 3 */
static uint32_t ed_sty_put(const uint8_t *a, uint32_t n)
{
    cst_var_t v;
    uint8_t ev[STY_MAXEV][4];
    uint32_t i, at;
    int l;
    char nm[CS_NAME_LEN];
    if (n < 3u || a[0] >= CST_NVAR)
        return 1;
    memset(&v, 0, sizeof v);
    memset(ev, 0, sizeof ev);
    if ((l = ed_shin_name(a + 1, n - 1u, nm)) == 0 || (uint32_t)l > CST_NAME_LEN + 1u)
        return 1;
    at = 1u + (uint32_t)l;
    if (n < at + 5u)
        return 1;
    v.steps = a[at]; v.rate = a[at + 1u]; v.strum = a[at + 2u]; v.oct = a[at + 3u]; v.nev = a[at + 4u];
    memcpy(v.name, nm, CST_NAME_LEN);
    if (v.steps > 64u || v.rate > 8u || v.strum > 63u || v.oct < 1u || v.oct > 3u || v.nev > STY_MAXEV ||
        (!v.steps && v.nev) || n != at + 5u + v.nev * 6u)
        return 1;
    for (i = 0, at += 5u; i < v.nev; i++, at += 6u) {
        const uint8_t *e = a + at;
        if (e[0] >= v.steps || !ed_sty_sel_ok(e[1]) || e[2] > 3u || !e[3] || e[4] > 63u || e[5] > 2u ||
            (e[5] && e[1] != 32u && e[1] != 34u))
            return 1;
        ev[i][0] = e[0]; ev[i][1] = (uint8_t)(e[1] | e[2] << 6); ev[i][2] = e[3]; ev[i][3] = (uint8_t)(e[4] | e[5] << 6);
    }
    if (ed_flash_stop())
        return 3;
    sty_user.var[a[0]] = v;
    memcpy(sty_user.ev[a[0]], ev, sizeof ev);
    sty_user.magic = STY_USER_MAGIC;
    ui.force = 1;
    return user_save(OBJ_USTY) ? 2u : 0u;
}

static int ed_shin_handle(uint32_t cmd, const uint8_t *a, uint32_t n)
{
    switch (cmd) {
    case ED_CS_LIST: {                                   /* start, count 1..16 */
        uint32_t s, i, cnt;
        if (n != 2u || !a[1] || a[1] > 16u)
            return 0;
        cnt = a[0] >= CS_NALL ? 0u : CS_NALL - a[0] < a[1] ? CS_NALL - a[0] : a[1];
        ed_b(a[0]); ed_b(cnt); ed_b(CS_NSETS); ed_b(CS_NUSER);
        for (i = 0; i < cnt; i++) {
            char nm[CS_NAME_LEN + 1u];
            s = a[0] + i;
            ed_b(cs_set_ok(s));
            ed_b(s < CS_NSETS ? CS_SET[s].genre : CS_NGENRES);
            cs_set_name(s, nm);
            ed_str(cs_set_ok(s) ? nm : "", CS_NAME_LEN + 1u);
        }
        return 1;
    }
    case ED_CS_GET:
        if (n != 1u)
            return 0;
        ed_cs_get(a[0]);
        return 1;
    case ED_CS_PUT:
        ed_b(n ? a[0] : 127u);
        ed_b(ed_cs_put(a, n));
        return 1;
    case ED_STY_GET:
        if (n != 2u)
            return 0;
        ed_sty_get(a[0], a[1]);
        return 1;
    case ED_STY_PUT:
        ed_b(n ? a[0] : 127u);
        ed_b(ed_sty_put(a, n));
        return 1;
    case ED_CPAT_GET: {
        cpat_t *p;
        if (n != 1u)
            return 0;
        ed_b(a[0]);
        if (!a[0] || a[0] > CP_N || !(p = cp_edit(a[0]))) {
            ed_b(1);
            return 1;
        }
        ed_b(0);
        ed_bk_pack((const uint8_t *)p, sizeof *p);
        return 1;
    }
    case ED_CPAT_PUT: {
        static cpat_t q __attribute__((section(".pool")));
        cpat_t *p;
        uint32_t rc = 0;
        ed_b(n ? a[0] : 127u);
        if (n < 2u || !a[0] || a[0] > CP_N || ed_unpack7(a + 1, n - 1u, (uint8_t *)&q, sizeof q) != sizeof q || !cp_valid(&q))
            rc = 1;
        else if (!(p = cp_edit(a[0])))
            rc = 2;
        else {
            *p = q;
            cp_dirty(a[0]);
            ui.force = 1;
            if (!transport_busy())                     /* (stopped: written now, else when quiet) */
                cp_flush();
        }
        ed_b(rc);
        return 1;
    }
    default:
        return 0;
    }
}
