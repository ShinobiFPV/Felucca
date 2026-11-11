/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 William Kew, ShinTech Electronics */
/* Host test of ShinTech's project format FUNB and the project extension (firmware/src/proj_ext.c, project.c's
 * PROJ_HOST part):
 *   - a project that uses none of the fork's features is FUN10 ("FUNA") as upstream 1.8 writes it: 114 parameters at
 *     byte 66 (P_LEVEL .. P_ATAMT, then E0..E7), byte 67 0, the motion's engine ids in FUN10's numbering (106..113),
 *     the FM6 patch bytes 0..127: upstream reads it as its own;
 *   - one that does (a CHORD parameter off its default, P_CHRD SET, a lock on a CHORD parameter) is FUNB: the same
 *     layout, byte 67 its P_COUNT, its record's digest in bit 7 of track 1's first 32 patch bytes; with its record in
 *     the store it loads back exactly (every parameter, the locks, the patches), without it (or with another) at the
 *     new parameters' defaults, the rest exactly; a FUNB of a newer P_COUNT (byte 67) is refused, one of an older
 *     one's motion moves as by count;
 *   - the store: one record for one content (two projects alike share it), px_put keeps only the named ones, a
 *     damaged store is refused, records of unknown chunks / ids skip what they do not know;
 *   - 2000 random projects: FUNA exactly when nothing new is used, every one loads back as it was.
 * Run by tests/run_tests.sh (needs build/gen from one firmware build). */
#define main hostsim_main
#include "hostsim.c"
#undef main
#define PROJ_HOST 1
#include "../firmware/src/project.c"

static int fails;
static void check(const char *what, int ok)
{
    printf("funb: %-86s %s\n", what, ok ? "ok" : "FAIL");
    fails += !ok;
}

static uint32_t rs = 0x2545F491u;
static uint32_t rnd(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

/* a project of today's values: every parameter in range (param_fit), steps, motion on sound parameters */
static void make(project_t *q, int chord)
{
    uint32_t t, i;
    memset(q, 0, sizeof *q);
    q->magic = PROJ_MAGIC;
    q->size = sizeof *q;
    for (i = 0; i < G_COUNT; i++)
        q->g[i] = GP[i].def;
    q->parts = NPART;
    q->phys = PROJ_PHYS;
    q->sel = 1;
    chain_defaults(&q->chain);
    for (t = 0; t < NTRK; t++) {
        q->t[t].engine = (uint8_t)(t == 3 ? ENGI_DRUM : t);   /* ANALOG, (DIGITAL: FM6), PHASE, DRUM */
        if (q->t[t].engine == ENGI_DIGITAL)
            q->t[t].engine = 12;                                 /* FM6 */
        for (i = 0; i < P_COUNT; i++)
            q->t[t].p[i] = param_desc_of(q->t[t].engine, i)->def;
        q->t[t].p[P_LEVEL] = (int16_t)(90 + t);
        q->t[t].p[P_E0 + 2] = (int16_t)(20 + t);
        for (i = 0; i < NSTEP; i++) {
            q->t[t].step[i].time = (uint8_t)(i % 3u);
            q->t[t].step[i].n = (uint8_t)(i % 3u == 0u);
            q->t[t].step[i].note[0] = (uint8_t)(48u + i % 24u);
            q->t[t].step[i].vel = 100;
        }
        memcpy(q->fm6[t], FM6_INIT, FM6_PACKED);
    }
    q->motion.count = 2;                                         /* a lock on E3 of track 1, automation of CUT on 2 */
    q->motion.event[0] = (motion_event_t){4, (uint8_t)(MOTION_LOCK | (P_E0 + 3u)), 33};
    q->motion.event[1] = (motion_event_t){(uint8_t)(64u | 8u), (uint8_t)(P_E0 + 4u), 70};
    if (chord) {
        q->t[0].p[P_CHRD] = CHRD_SET;
        q->t[0].p[P_CSET] = 42;
        q->t[0].p[P_CKEY] = 7;
        q->t[0].p[P_CVOIC] = 1;
        q->t[0].p[P_CINV] = 2;
        q->t[0].p[P_CRNG] = 25;
        q->t[0].p[P_CBASS] = 1;
        q->t[2].p[P_CSET] = 99;
    }
    memcpy(q->name, "FUNB TEST", 9);
    q->sum = proj_sum(q);
}

/* every value of a and b the same (parameters, engines, steps, motion, patches, name); skip_new: not the new ones */
static int same(const project_t *a, const project_t *b, int skip_new)
{
    uint32_t t, i;
    for (t = 0; t < NTRK; t++) {
        for (i = 0; i < P_COUNT; i++)
            if ((!skip_new || i < PX_FIRST || i >= P_E0) && a->t[t].p[i] != b->t[t].p[i])
                return 0;
        if (a->t[t].engine != b->t[t].engine || memcmp(a->t[t].step, b->t[t].step, sizeof a->t[t].step) ||
            memcmp(a->fm6[t], b->fm6[t], FM6_PACKED))
            return 0;
    }
    return !memcmp(a->g, b->g, sizeof a->g) && a->motion.count == b->motion.count &&
           !memcmp(a->motion.event, b->motion.event, a->motion.count * sizeof a->motion.event[0]) &&
           !memcmp(a->name, b->name, sizeof a->name) && !memcmp(&a->chain, &b->chain, sizeof a->chain);
}

/* the new parameters of q are their defaults */
static int new_defaults(const project_t *q)
{
    uint32_t t, i;
    for (t = 0; t < NTRK; t++)
        for (i = PX_FIRST; i < P_E0; i++)
            if (q->t[t].p[i] != param_desc_of(q->t[t].engine, i)->def)
                return 0;
    return 1;
}

/* the record of q into the store (keeping what it holds) */
static int store_rec(const project_t *q)
{
    uint8_t rec[PX_REC_MAX];
    uint32_t n = px_encode(q, rec), keep[64], nk = 0, at = 0;
    while (at + PX_REC_HDR <= px.used && nk < 64u) {
        keep[nk++] = px_u32(px.data + at);
        at += PX_REC_HDR + px_pad(px.data[at + 4u] | (uint32_t)px.data[at + 5u] << 8);
    }
    return px_put(px_digest(rec, n), rec, n, keep, nk);
}

static void rehash(project_store_t *s)
{
    uint32_t sum = proj_hash(s->raw, PROJ_STORE_SIZE - 4u);
    memcpy(s->raw + PROJ_STORE_SIZE - 4u, &sum, 4);
}

int main(void)
{
    static project_t a, b;
    static project_store_t s, s2;
    uint32_t t, i, ok;
    px_reset();
    check("the new parameters: SET KEY VOICING INVERSION RANGE BASS, STYLE VAR HOLD STRUM CPAT, POLY6 BBD VOX, past FUN10's 114, before E0..E7",
          PX_FIRST == 106u && P_CSET == 106u && P_CBASS == 111u && P_CSTY == 112u && P_CSTRM == 115u && P_CPAT == 116u && P_P6MODE == 118u && P_E0 == 119u &&
          P_COUNT == 127u && P_COUNT < 128u);

    /* 1. nothing new: FUN10 as upstream writes it */
    make(&a, 0);
    ok = proj_pack(&s, &a);
    check("a project without CHORD packs", ok);
    check(".. as FUNA (FUN10), 3840 bytes, 114 parameters at byte 66, byte 67 0",
          px_u32(s.raw) == PROJ_MAGIC && px_u32(s.raw + 4) == PROJ_STORE_SIZE && s.raw[66] == 114u && s.raw[67] == 0u);
    for (t = 0, ok = 1; t < NTRK; t++) {                           /* FUN10's parameter block, track by track */
        const uint8_t *p = s.raw + 68u + t * (114u + 2u + NSTEP * 9u);
        for (i = 0; i < 106u; i++)
            ok &= p[i] == (uint8_t)(a.t[t].p[i] + 64);
        for (i = 0; i < 8u; i++)
            ok &= p[106u + i] == (uint8_t)(a.t[t].p[P_E0 + i] + 64);
        ok &= p[114] == a.t[t].engine;
    }
    check(".. P_LEVEL .. P_ATAMT then E0..E7, then the engine byte, as FUN10's", ok);
    {
        const uint8_t *m = s.raw + 68u + NTRK * (114u + 2u + NSTEP * 9u) + sizeof(chain_config_t);
        check(".. the motion's engine ids in FUN10's numbering (E3 lock 106 + 3, E4 106 + 4)",
              m[0] == 2u && m[4 + 1] == (MOTION_LOCK | 109u) && m[4 + 3 + 1] == 110u);
    }
    for (i = PROJ_FM6_OFF, ok = 1; i < PROJ_FM6_OFF + NTRK * FM6_PACKED; i++)
        ok &= s.raw[i] <= 127u;
    check(".. the FM6 patch bytes 0..127 (no digest)", ok);
    check(".. loads back exactly", proj_import(&b, &s, sizeof s) && same(&a, &b, 0));

    /* 2. CHORD: FUNB */
    make(&a, 1);
    ok = proj_pack(&s, &a);
    check("a project with CHORD SET packs as FUNB: byte 66 114, byte 67 P_COUNT",
          ok && px_u32(s.raw) == PROJ_MAGIC_B && s.raw[66] == 114u && s.raw[67] == P_COUNT);
    px_reset();
    check(".. its record missing: it loads, the new parameters their defaults, the rest exactly",
          proj_import(&b, &s, sizeof s) && same(&a, &b, 1) && new_defaults(&b) && b.t[0].p[P_CHRD] == CHRD_SET);
    check(".. its record stored: it loads back exactly (every parameter, locks, patches)",
          store_rec(&a) && proj_import(&b, &s, sizeof s) && same(&a, &b, 0));
    {
        uint32_t n;
        check(".. the store holds one record, its digest the one the project names",
              px.used > 0u && px_find(px_digest_of(s.raw), &n) && px_valid(&px));
    }
    check(".. the patches come back without the digest's bits", proj_import(&b, &s, sizeof s) &&
          !memcmp(b.fm6[0], FM6_INIT, FM6_PACKED));
    {   /* a project with another CHORD setup names another record: missing, so defaults */
        project_t c;
        make(&c, 1);
        c.t[0].p[P_CKEY] = 3;
        c.sum = proj_sum(&c);
        proj_pack(&s2, &c);
        check("another CHORD setup names another digest; not stored: defaults",
              px_digest_of(s2.raw) != px_digest_of(s.raw) && proj_import(&b, &s2, sizeof s2) && new_defaults(&b));
    }
    {   /* a lock on a new parameter makes a FUNB too, and keeps its id */
        project_t c;
        make(&c, 0);
        c.motion.event[c.motion.count++] = (motion_event_t){5, (uint8_t)(MOTION_LOCK | P_CKEY), 5};
        c.sum = proj_sum(&c);
        ok = proj_pack(&s2, &c) && px_u32(s2.raw) == PROJ_MAGIC_B;
        check("a lock on KEY alone: FUNB; it loads back with the lock on KEY and E3's on E3",
              ok && store_rec(&c) && proj_import(&b, &s2, sizeof s2) && same(&c, &b, 0));
    }
    {   /* P_CHRD SET with every new parameter at its default: still FUNB (upstream knows no SET) */
        project_t c;
        make(&c, 0);
        c.t[1].p[P_CHRD] = CHRD_SET;
        c.sum = proj_sum(&c);
        check("CHRD SET with the rest at defaults: FUNB", proj_pack(&s2, &c) && px_u32(s2.raw) == PROJ_MAGIC_B);
    }

    /* 3. byte 67: the motion's numbering */
    make(&a, 1);
    proj_pack(&s, &a);
    s2 = s;
    s2.raw[67] = (uint8_t)(P_COUNT + 1u);
    rehash(&s2);
    check("a FUNB of a later P_COUNT (byte 67 above ours) is refused", !proj_import(&b, &s2, sizeof s2));
    s2 = s;
    s2.raw[67] = 100u;
    rehash(&s2);
    check("a FUNB whose byte 67 is below its 114 parameters is refused", !proj_import(&b, &s2, sizeof s2));
    {   /* written by a version with 3 new parameters (P_COUNT 117): its E ids 109.. move up to ours */
        uint8_t *m;
        s2 = s;
        s2.raw[67] = 117u;
        m = s2.raw + 68u + NTRK * (114u + 2u + NSTEP * 9u) + sizeof(chain_config_t);
        m[4 + 1] = (uint8_t)(MOTION_LOCK | 112u);              /* E3 at 109 + 3 */
        m[4 + 3 + 1] = 113u;                                   /* E4 */
        rehash(&s2);
        check("a FUNB of P_COUNT 117: its engine locks move to ours (E3, E4)",
              proj_import(&b, &s2, sizeof s2) && b.motion.event[0].param == (MOTION_LOCK | (P_E0 + 3u)) &&
                  b.motion.event[1].param == P_E0 + 4u);
    }
    s2 = s;
    s2.raw[100] ^= 1u;
    check("a FUNB with a wrong hash is refused", !proj_import(&b, &s2, sizeof s2));
    {   /* a FUN10 of 1.8 (114, FUNA) a byte of which we never write differently: loads, CHORD off */
        make(&a, 0);
        proj_pack(&s, &a);
        check("a FUN10 of 1.8 loads: CHORD OFF, the new parameters their defaults",
              proj_import(&b, &s, sizeof s) && new_defaults(&b) && b.t[0].p[P_CHRD] == 0);
    }

    /* 4. the store */
    px_reset();
    {
        project_t c, d;
        uint32_t used, n;
        make(&c, 1);
        make(&d, 1);                                           /* the same CHORD setup, another name and steps */
        memcpy(d.name, "OTHER", 6);
        d.t[1].step[3].note[0] = 77;
        d.sum = proj_sum(&d);
        store_rec(&c);
        used = px.used;
        store_rec(&d);
        check("two projects with one CHORD setup share one record", px.used == used);
        {
            uint8_t r1[PX_REC_MAX], r2[PX_REC_MAX];
            uint32_t n1, n2, d1, d2, keep[1];
            d.t[0].p[P_CKEY] = 1;
            n1 = px_encode(&c, r1);
            n2 = px_encode(&d, r2);
            d1 = px_digest(r1, n1);
            d2 = px_digest(r2, n2);
            keep[0] = d1;
            ok = px_put(d2, r2, n2, keep, 1);
            check("px_put keeps the named record and adds the new one",
                  ok && px_find(d1, &n) && px_find(d2, &n) && px_valid(&px));
            keep[0] = d2;
            d.t[0].p[P_CKEY] = 2;
            n2 = px_encode(&d, r2);
            ok = px_put(px_digest(r2, n2), r2, n2, keep, 1);
            check(".. and drops one no stored project names", ok && !px_find(d1, &n) && px_find(d2, &n) && px_valid(&px));
        }
        for (i = 0, ok = 1; i < 500u; i++) {                   /* many saves, a few named at a time: never full */
            uint8_t r[PX_REC_MAX];
            static uint32_t live[6];
            uint32_t nn, dd;
            d.t[i % 4u].p[P_CSET] = (int16_t)(rnd() % CS_NSETS);
            d.t[(i + 1u) % 4u].p[P_CKEY] = (int16_t)(rnd() % 12u);
            nn = px_encode(&d, r);
            dd = px_digest(r, nn);
            ok &= px_put(dd, r, nn, live, 6) && px_valid(&px) && px.used <= sizeof px.data;
            live[i % 6u] = dd;
        }
        check("500 saves with 6 projects named at a time: the store never fills, stays valid", ok);
    }
    {
        px_store_t bad;
        store_rec(&a);
        bad = px;
        bad.data[PX_REC_HDR] ^= 1u;
        check("a store whose record does not match its digest is not valid", px_valid(&px) && !px_valid(&bad));
        bad = px;
        bad.used = (uint16_t)(sizeof bad.data + 4u);
        check("a store longer than itself is not valid", !px_valid(&bad));
    }
    {   /* unknown chunks and ids are skipped */
        project_t c;
        uint8_t r[64] = {9, 3, 1, 2, 3,                 /* chunk 9: unknown */
                         1, 4 * 3, 1, P_CKEY, 64 + 5, 1, 200, 64 + 9, 1, 30, 64 + 1, 1, P_CBASS, 64 + 1};
        make(&c, 0);
        check("a record with an unknown chunk and unknown ids: the known values taken, the rest skipped",
              px_apply(&c, r, 19) && c.t[0].p[P_CKEY] == 5 && c.t[3].p[P_CBASS] == 1 && c.t[1].p[30] ==
                  param_desc_of(c.t[1].engine, 30)->def);
        check("a record whose chunk runs past its end is damaged", !px_apply(&c, r, 18));
    }

    /* 5. random projects */
    px_reset();
    for (i = 0, ok = 1; i < 2000u; i++) {
        uint32_t k, uses = rnd() & 1u;
        make(&a, 0);
        for (t = 0; t < NTRK; t++)
            for (k = 0; k < P_COUNT; k++) {
                const param_desc_t *dd = param_desc_of(a.t[t].engine, k);
                if (k == P_CHRD || (k >= PX_FIRST && k < P_E0 && !uses))
                    continue;
                if (rnd() % 3u == 0u)
                    a.t[t].p[k] = (int16_t)param_fit(dd, dd->min + (int32_t)(rnd() % (uint32_t)(dd->max - dd->min + 1)));
            }
        if (uses)
            a.t[rnd() % NTRK].p[PX_FIRST + rnd() % PX_NEW] = 1;   /* (at least one new value off its default, mostly) */
        a.sum = proj_sum(&a);
        if (!proj_pack(&s, &a)) {
            ok = 0;
            break;
        }
        if (px_uses(&a))
            store_rec(&a);
        ok &= (px_u32(s.raw) == PROJ_MAGIC_B) == (px_uses(&a) != 0);
        ok &= proj_import(&b, &s, sizeof s) && same(&a, &b, 0);
        if (!ok) {
            printf("funb: random project %u differs\n", i);
            break;
        }
        if (px.used > sizeof px.data - 600u)
            px_reset();                                        /* (no project names the old ones) */
    }
    check("2000 random projects: FUNB exactly when one uses a new feature, each loads back as it was", ok);
    printf("funb: %s\n", fails ? "FAILED" : "all ok");
    return fails != 0;
}
