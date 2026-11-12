/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 William Kew, ShinTech Electronics */
/* ShinTech CHORD patterns' bank (cpat.c) in flash, and the main loop's side of its cache. Included at the end of
 * project.c (proj_scratch, the storage).
 * The bank: CP_OBJS (7) storage objects of CP_PER (10) patterns (the last: 4), OBJ_CPAT0.. (storage.c): A 0xEA000 + 8 KiB a
 * object, B 4 KiB above, felucca-bsp's FL_XDATA1 after the project extension; each payload "CPT1" + its patterns (3,284
 * bytes: before 0xF00, the update loader's place). An object never written, or damaged in both copies, reads as empty
 * patterns. A write rewrites one object (its other patterns as they are in flash), into the copy that is not the
 * current one, its commit record last (storage.c st_save): a power cut keeps the old copy. Without flash (host tests)
 * a RAM bank.
 * cpat_poll (every UI frame, from autosave_poll): loads into the cache what the tracks may play next (each track's
 * CPAT, or its song section's, the pattern playing and their NEXTs) and what the PATTERN pages edit (cp_ui_id); writes
 * an edited pattern back once the music is quiet (project.c autosave_quiet: stopped, no key, nothing sounding) and
 * 2 s after its last edit. A flash erase silences the audio for a moment: never while it plays. */
#define CP_PER 10u
#define CP_OBJS ((CP_N + CP_PER - 1u) / CP_PER)
#define CP_MAGIC 0x31545043u                    /* "CPT1" */
#define CP_IDLE_MS 2000u
_Static_assert(4u + CP_PER * sizeof(cpat_t) <= 3584u, "a pattern object ends before 0xF00 (the update loader's)");

#if FELUCCA_FLASH
_Static_assert(sizeof(project_t) >= 4u + CP_PER * sizeof(cpat_t), "proj_scratch holds a pattern object");
static void cp_read(uint32_t id, cpat_t *dst)
{
    uint8_t *b = (uint8_t *)&proj_scratch;      /* (main loop work space: nothing else uses it meanwhile) */
    uint32_t o = (id - 1u) / CP_PER, i = (id - 1u) % CP_PER;
    int n;
    memset(dst, 0, sizeof *dst);
    if (!id || id > CP_N || !flash_ok)
        return;
    n = st_load(OBJ_CPAT0 + o, b, 4u + CP_PER * sizeof(cpat_t));
    if (n >= (int)(4u + (i + 1u) * sizeof(cpat_t)) && b[0] == 'C' && b[1] == 'P' && b[2] == 'T' && b[3] == '1')
        memcpy(dst, b + 4u + i * sizeof(cpat_t), sizeof *dst);
}
static int cp_write(uint32_t id, const cpat_t *src)   /* 0 written */
{
    uint8_t *b = (uint8_t *)&proj_scratch;
    uint32_t o = (id - 1u) / CP_PER, i = (id - 1u) % CP_PER, len = 4u + CP_PER * sizeof(cpat_t);
    int n;
    if (!id || id > CP_N || !flash_ok)
        return -1;
    n = st_load(OBJ_CPAT0 + o, b, len);
    if (n != (int)len || b[0] != 'C' || b[1] != 'P' || b[2] != 'T' || b[3] != '1') {
        memset(b, 0, len);                      /* (none yet, or unreadable: its other patterns empty) */
        b[0] = 'C';
        b[1] = 'P';
        b[2] = 'T';
        b[3] = '1';
    }
    memcpy(b + 4u + i * sizeof(cpat_t), src, sizeof *src);
    return st_save(OBJ_CPAT0 + o, b, len);
}
static int cp_quiet(void) { return autosave_quiet(); }
#else
static cpat_t cp_bank[CP_N];                    /* (host builds: the bank in RAM) */
static void cp_read(uint32_t id, cpat_t *dst)
{
    if (id && id <= CP_N)
        *dst = cp_bank[id - 1u];
    else
        memset(dst, 0, sizeof *dst);
}
static int cp_write(uint32_t id, const cpat_t *src)
{
    if (!id || id > CP_N)
        return -1;
    cp_bank[id - 1u] = *src;
    return 0;
}
static int cp_quiet(void) { return !song.playing && !chain_busy(); }
#endif

/* ShinTech Phase 5: the user's chord sets (chordset.c cs_user) and style (style.c sty_user), whole objects in flash,
 * a RAM copy the audio side reads. Power-on: read (none or damaged in both copies: empty). A write (the editor's,
 * editor_shin.c, after it stopped the transport): the new copy, its commit record last (storage.c st_save) */
_Static_assert(sizeof(cs_user_t) <= 3584u && sizeof(sty_user_t) <= 3584u, "a user store ends before 0xF00");
static void user_boot(void)
{
#if FELUCCA_FLASH
    if (!flash_ok || st_load(OBJ_UCSET, &cs_user, sizeof cs_user) != (int)sizeof cs_user || cs_user.magic != CS_USER_MAGIC)
        memset(&cs_user, 0, sizeof cs_user);
    if (!flash_ok || st_load(OBJ_USTY, &sty_user, sizeof sty_user) != (int)sizeof sty_user || sty_user.magic != STY_USER_MAGIC)
        memset(&sty_user, 0, sizeof sty_user);
#endif
    cs_user.magic = CS_USER_MAGIC;
    sty_user.magic = STY_USER_MAGIC;
}
static int user_save(uint32_t obj)            /* 0 written (no flash: kept in RAM) */
{
#if FELUCCA_FLASH
    if (!flash_ok)
        return -1;
    return obj == OBJ_UCSET ? st_save(OBJ_UCSET, &cs_user, sizeof cs_user) : st_save(OBJ_USTY, &sty_user, sizeof sty_user);
#else
    (void)obj;
    return 0;
#endif
}

/* the patterns wanted in the cache, a bit each (bit id - 1) */
static uint64_t cp_wanted(void)
{
    uint64_t w = 0;
    uint32_t k, id, j;
    for (k = 0; k < NTRK; k++) {
        uint32_t ids[3] = {cp_want(k), cpp[k].on ? cpp[k].id : 0u, 0u};
        if (!chord_set_on(&trk[k]))
            continue;
        for (j = 0; j < 2u; j++) {
            const cpat_t *p;
            id = ids[j];
            if (!id || id > CP_N)
                continue;
            w |= (uint64_t)1 << (id - 1u);
            if ((p = cp_get(id)) != 0 && p->next && p->next <= CP_N)
                w |= (uint64_t)1 << (p->next - 1u);
        }
    }
    if (chain.running || chain.armed)                /* a song: every pattern its slots name */
        for (j = 0; j < 4u; j++)
            for (k = 0; k < NTRK; k++)
                if ((id = chain.source[j].cpat[k]) != 0u && id <= CP_N && chord_set_on(&trk[k]))
                    w |= (uint64_t)1 << (id - 1u);
    if (cp_ui_id && cp_ui_id <= CP_N)
        w |= (uint64_t)1 << (cp_ui_id - 1u);
    return w;
}
/* pattern id into the cache (a slot that is free, or holds a pattern not wanted and written): 0 = there */
static int cp_load(uint32_t id, uint64_t want)
{
    uint32_t i, f = CP_SLOTS;
    if (cp_find(id) >= 0)
        return 0;
    for (i = 0; i < CP_SLOTS && f == CP_SLOTS; i++)
        if (!cp_ram[i].id)
            f = i;
    for (i = 0; i < CP_SLOTS && f == CP_SLOTS; i++)
        if (!cp_ram[i].dirty && !((want >> (cp_ram[i].id - 1u)) & 1u))
            f = i;
    if (f == CP_SLOTS)
        return -1;
    cp_ram[f].id = 0;                           /* (the audio side no longer finds the old one) */
    RING_PUBLISH();
    cp_read(id, &cp_ram[f].p);
    cp_ram[f].dirty = 0;
    RING_PUBLISH();
    cp_ram[f].id = (uint8_t)id;
    return 0;
}
static void cp_flush(void)                     /* every edited pattern written now (the caller: quiet) */
{
    uint32_t i;
    for (i = 0; i < CP_SLOTS; i++)
        if (cp_ram[i].id && cp_ram[i].dirty && cp_write(cp_ram[i].id, &cp_ram[i].p) == 0)
            cp_ram[i].dirty = 0;
}
static void cpat_poll(void)
{
    uint64_t w = cp_wanted();
    uint32_t i, pass;
    for (pass = 0; pass < 2u; pass++, w = cp_wanted())   /* (again: the NEXT of a pattern just loaded) */
        for (i = 0; i < CP_N; i++)
            if ((w >> i) & 1u)
                (void)cp_load(i + 1u, w);
    if (cp_quiet())
        for (i = 0; i < CP_SLOTS; i++)
            if (cp_ram[i].id && cp_ram[i].dirty && fm1_ms - cp_ram[i].t_ms >= CP_IDLE_MS &&
                cp_write(cp_ram[i].id, &cp_ram[i].p) == 0)
                cp_ram[i].dirty = 0;
}
/* the pattern id for editing (the PATTERN pages, the editor): in the cache (loaded now if need be), 0 = no room */
static cpat_t *cp_edit(uint32_t id)
{
    if (!id || id > CP_N)
        return 0;
    if (cp_find(id) < 0)
        (void)cp_load(id, cp_wanted() | (uint64_t)1 << (id - 1u));
    return cp_get(id);
}
