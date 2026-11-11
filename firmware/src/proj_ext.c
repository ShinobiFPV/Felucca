/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 William Kew, ShinTech Electronics */
/* The project extension (ShinTech): what a project of this fork holds that FUN10 has no room for, kept beside it.
 *
 * FUN10 is full (P_COUNT 114 left one spare byte), so this fork's track parameters past FUN10's (PX_FIRST .. P_E0 - 1:
 * the CHORD setup, P_CSET ..) are not in the project record. A project that uses none of this fork's features is
 * written as FUN10 ("FUNA") exactly as upstream writes it (its 114 parameters, its motion ids in FUN10's numbering):
 * upstream Felucca opens it. One that does (a new parameter off its default, P_CHRD SET, a lock on a new parameter) is
 * written as FUNB:
 *   - FUN10's layout and size (3840 bytes), the magic "FUNB": firmware that does not know it shows the slot EMPTY and
 *     leaves its flash alone (as any newer format); FUN10's 114 parameters at byte 66, as FUN10 holds them;
 *   - byte 67: the P_COUNT whose ids the motion records use (FUN10's are always remapped from 114): a lock on a new
 *     parameter keeps its id, and a later version that adds parameters before P_E0 moves the engine ones up as ever;
 *   - the digest of its extension record (px_digest, 32 bits) in bit 7 of the first 32 bytes of track 1's FM6 patch
 *     (bits a patch never uses: proj_pack refuses a byte over 127, proj_unpack drops bit 7).
 * The extension records live in one storage object (OBJ_PROJEXT, A/B in felucca-bsp's FL_XDATA1: 0xE8000 / 0xE9000), a
 * record per distinct content, found by its digest: two projects with the same CHORD setup share one; a rename (the
 * record's content unchanged) writes none. A save writes the extension first (keeping every record a stored project or
 * the autosave still names), then the project: a power cut leaves the old project with its record or the new one with
 * its. A FUNB whose record is missing (a backup restored without it, another device) loads with the new parameters at
 * their defaults: the project itself is never refused for it.
 * A record: a chunk list (type, length, data); type 1 = the parameters: for each track, their count, then (id, value +
 * 64) pairs. Unknown chunk types and ids are skipped, so a later version may add both. The payload stays below
 * PX_SIZE: a sector of FL_XDATA keeps bytes 0xF00..0xF07 free of an update record (fm1_flash.h). */
#define PROJ_NP_FUN10 114u                   /* the parameters FUN10 holds (P_COUNT of 1.8: P_E0 106) */
#define PX_FIRST (PROJ_NP_FUN10 - 8u)        /* the first parameter FUN10 does not have */
#define PX_NEW (P_E0 - PX_FIRST)             /* how many */
#define PX_MAGIC 0x31545853u                 /* "SXT1" */
#define PX_SIZE 3584u
#define PX_DIG_BITS 32u                      /* the digest's bits in FM6 patch bytes */
_Static_assert(PX_FIRST == P_CSET && P_E0 + 8u == P_COUNT && PX_NEW <= 64u, "proj_ext: the new parameters");

typedef struct {
    uint32_t magic;
    uint16_t used, rsv;                      /* bytes of data[] in use */
    uint8_t data[PX_SIZE - 8u];              /* records: digest (4), length (2), 2 reserved, data, padded to 4 */
} px_store_t;
_Static_assert(sizeof(px_store_t) == PX_SIZE, "proj_ext: the store");
static px_store_t px __attribute__((section(".pool")));   /* the object, mirrored (main loop only) */
#define PX_REC_HDR 8u
#define PX_REC_MAX 512u                      /* one record's data at most */

static uint32_t px_u32(const uint8_t *b) { return b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }
static uint32_t px_pad(uint32_t n) { return (n + 3u) & ~3u; }
static uint32_t px_digest(const uint8_t *d, uint32_t n) { return proj_hash(d, n) ^ n; }

static void px_reset(void)
{
    memset(&px, 0, sizeof px);
    px.magic = PX_MAGIC;
}

/* the store's records are whole and their digests their contents': 1 */
static int px_valid(const px_store_t *s)
{
    uint32_t at = 0;
    if (s->magic != PX_MAGIC || s->used > sizeof s->data)
        return 0;
    while (at < s->used) {
        uint32_t len;
        if (s->used - at < PX_REC_HDR)
            return 0;
        len = s->data[at + 4u] | (uint32_t)s->data[at + 5u] << 8;
        if (len > PX_REC_MAX || px_pad(len) > s->used - at - PX_REC_HDR ||
            px_u32(s->data + at) != px_digest(s->data + at + PX_REC_HDR, len))
            return 0;
        at += PX_REC_HDR + px_pad(len);
    }
    return at == s->used;
}

/* the record of digest d -> its data, *n its length; 0 = none */
static const uint8_t *px_find(uint32_t d, uint32_t *n)
{
    uint32_t at = 0;
    while (at + PX_REC_HDR <= px.used) {
        uint32_t len = px.data[at + 4u] | (uint32_t)px.data[at + 5u] << 8;
        if (px_u32(px.data + at) == d) {
            *n = len;
            return px.data + at + PX_REC_HDR;
        }
        at += PX_REC_HDR + px_pad(len);
    }
    return 0;
}

/* project q uses this fork's features: it is a FUNB, with an extension record */
static int px_uses(const project_t *q)
{
    uint32_t t, i;
    for (t = 0; t < NTRK; t++) {
        if (q->t[t].p[P_CHRD] >= CHRD_SET)
            return 1;
        for (i = PX_FIRST; i < P_E0; i++)
            if (q->t[t].p[i] != param_desc_of(q->t[t].engine % NENGINES, i)->def)
                return 1;
    }
    for (i = 0; i < q->motion.count && i < MOTION_MAX; i++)
        if (MOTION_ID(&q->motion.event[i]) >= PX_FIRST && MOTION_ID(&q->motion.event[i]) < P_E0)
            return 1;
    return 0;
}

/* q's extension record -> out (PX_REC_MAX), its length: always the same bytes for the same values */
static uint32_t px_encode(const project_t *q, uint8_t *out)
{
    uint32_t t, i, n = 2u;
    out[0] = 1u;                                 /* type 1: the parameters */
    for (t = 0; t < NTRK; t++) {
        out[n++] = (uint8_t)PX_NEW;
        for (i = PX_FIRST; i < P_E0; i++) {
            out[n++] = (uint8_t)i;
            out[n++] = (uint8_t)(clamp(q->t[t].p[i], -64, 127) + 64);
        }
    }
    out[1] = (uint8_t)(n - 2u);
    return n;
}

/* a record's data -> q's new parameters (values as stored: proj_bound fits them); 0 = damaged */
static int px_apply(project_t *q, const uint8_t *d, uint32_t n)
{
    uint32_t at = 0;
    while (at + 2u <= n) {
        uint32_t type = d[at], len = d[at + 1u], p = at + 2u, end = p + len, t;
        if (end > n)
            return 0;
        if (type == 1u)
            for (t = 0; t < NTRK; t++) {
                uint32_t k, cnt;
                if (p >= end)
                    return 0;
                cnt = d[p++];
                if (p + 2u * cnt > end)
                    return 0;
                for (k = 0; k < cnt; k++, p += 2u)
                    if (d[p] >= PX_FIRST && d[p] < P_E0)   /* (an id this version does not have: skipped) */
                        q->t[t].p[d[p]] = (int16_t)((int32_t)d[p + 1u] - 64);
            }
        at = end;
    }
    return at == n;
}

/* the digest a stored FUNB names (bit 7 of track 1's FM6 bytes 0..31) */
static uint32_t px_digest_of(const uint8_t *b)
{
    uint32_t i, d = 0;
    for (i = 0; i < PX_DIG_BITS; i++)
        d |= (uint32_t)(b[PROJ_FM6_OFF + i] >> 7) << i;
    return d;
}
static void px_digest_put(uint8_t *b, uint32_t d)
{
    uint32_t i;
    for (i = 0; i < PX_DIG_BITS; i++)
        b[PROJ_FM6_OFF + i] = (uint8_t)((b[PROJ_FM6_OFF + i] & 0x7Fu) | ((d >> i) & 1u) << 7);
}
/* the digest a stored project (any record) names, 0 = none (not a FUNB) */
static uint32_t px_named(const void *rec, uint32_t n)
{
    const uint8_t *b = rec;
    return n == PROJ_STORE_SIZE && px_u32(b) == PROJ_MAGIC_B ? px_digest_of(b) : 0u;
}

/* the record (d, data, n) in the store, keeping every record whose digest is one of keep[0..nk-1] and dropping the
 * others (no stored project names them); 1 = there (added or already), 0 = no room (the store as it was) */
static int px_put(uint32_t d, const uint8_t *data, uint32_t n, const uint32_t *keep, uint32_t nk)
{
    static px_store_t nx __attribute__((section(".pool")));
    uint32_t at = 0, w = 0, k, m;
    if (px_find(d, &m))
        return 1;
    if (n > PX_REC_MAX)
        return 0;
    nx.magic = PX_MAGIC;
    while (at + PX_REC_HDR <= px.used) {         /* the records still named */
        uint32_t len = px.data[at + 4u] | (uint32_t)px.data[at + 5u] << 8, rd = px_u32(px.data + at);
        for (k = 0; k < nk && keep[k] != rd; k++)
            ;
        if (k < nk) {
            memcpy(nx.data + w, px.data + at, PX_REC_HDR + px_pad(len));
            w += PX_REC_HDR + px_pad(len);
        }
        at += PX_REC_HDR + px_pad(len);
    }
    if (w + PX_REC_HDR + px_pad(n) > sizeof nx.data)
        return 0;
    memset(nx.data + w, 0, PX_REC_HDR + px_pad(n));
    for (k = 0; k < 4u; k++)
        nx.data[w + k] = (uint8_t)(d >> (8u * k));
    nx.data[w + 4u] = (uint8_t)n;
    nx.data[w + 5u] = (uint8_t)(n >> 8);
    memcpy(nx.data + w + PX_REC_HDR, data, n);
    w += PX_REC_HDR + px_pad(n);
    nx.used = (uint16_t)w;
    nx.rsv = 0;
    memset(nx.data + w, 0, sizeof nx.data - w);
    px = nx;
    return 1;
}
