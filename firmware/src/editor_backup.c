/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Complete musical archive: no raw addresses are accepted. Small objects are staged
 * in main-loop RAM and fully validated before the existing A/B commit path writes.
 * Sample restore uses SMP_BEGIN/WRITE/END and its existing CRC/header-last commit.
 * A disconnected sample restore can lose that sample; the exported file is retained.
 * Id 8 (the FM6 patch bank of 1.0..1.0.2) is listed empty since 1.0.3; a PUT of it from an older archive moves its
 * patches into the user presets restored before it (ids 6, 7), as the first boot after the update does (up_fm6.c).
 * Id 9 is the user presets' FM6 patches (up_fm6.c), appended in 1.0.3.
 */
/* ShinTech (Phase 5): 40 the project extension (proj_ext.c), 41..47 the chord pattern bank's seven objects (cpat_store.c:
 * "CPT1" and ten patterns, the last four), 48 the user's chord sets, 49 the user's style (as flash keeps them) */
static const uint8_t ED_BK_IDS[23] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 32, 33, 34};
#define ED_BK_CP_LEN (4u + CP_PER * (uint32_t)sizeof(cpat_t))
static cpat_t ed_bk_cp __attribute__((section(".pool")));   /* (a pattern read for a chunk of 41..47) */
/* bytes off .. off + n of pattern object k (0..6) -> out (built from the bank, cpat_store.c cp_read) */
static void ed_bk_cp_bytes(uint32_t k, uint32_t off, uint32_t n, uint8_t *out)
{
    static const uint8_t MAG[4] = {'C', 'P', 'T', '1'};
    uint32_t i, id = 0;
    for (i = 0; i < n; i++, off++) {
        if (off < 4u) {
            out[i] = MAG[off];
            continue;
        }
        if (k * CP_PER + (off - 4u) / sizeof(cpat_t) + 1u != id) {
            id = k * CP_PER + (off - 4u) / sizeof(cpat_t) + 1u;
            if (id <= CP_N)
                cp_read(id, &ed_bk_cp);
            else
                memset(&ed_bk_cp, 0, sizeof ed_bk_cp);
        }
        out[i] = ((const uint8_t *)&ed_bk_cp)[(off - 4u) % sizeof(cpat_t)];
    }
}
static uint32_t ed_bk_cp_crc(uint32_t k)
{
    uint8_t b[64];
    uint32_t c = 0xFFFFFFFFu, off;
    for (off = 0; off < ED_BK_CP_LEN; off += sizeof b) {
        uint32_t n = ED_BK_CP_LEN - off < sizeof b ? ED_BK_CP_LEN - off : sizeof b;
        ed_bk_cp_bytes(k, off, n, b);
        c = st_crc32_add(c, b, n);
    }
    return ~c;
}
#define ED_BK_N ((uint32_t)sizeof ED_BK_IDS)
#define ED_BK_MAX ((uint32_t)sizeof proj_wire_u.raw)
#define ED_BK_RAW (proj_wire_u.raw)  /* reuse the existing serialized main-loop scratch */
_Static_assert(sizeof proj_wire_u.raw >= sizeof(upf_t) && sizeof proj_wire_u.raw >= sizeof(fm6_bank_t), "backup staging");
static persist_t ed_bk_settings __attribute__((section(".pool")));
static uint8_t ed_bk_valid, ed_bk_put, ed_bk_id, ed_bk_gen;
static uint32_t ed_bk_len, ed_bk_crc, ed_bk_pos, ed_bk_ms, ed_bk_usb;
static void ed_bk_u32(uint32_t n) { for (uint32_t i = 0; i < 5u; i++) ed_b((n >> (i * 7u)) & 127u); }
static uint32_t ed_bk_r32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 7 | (uint32_t)p[2] << 14 | (uint32_t)p[3] << 21 | (uint32_t)p[4] << 28;
}
static void ed_bk_pack(const uint8_t *p, uint32_t n)
{
    while (n) {
        uint32_t k = n > 7u ? 7u : n, mask = 0;
        for (uint32_t i = 0; i < k; i++) mask |= (uint32_t)(p[i] >> 7) << i;
        ed_b(mask);
        for (uint32_t i = 0; i < k; i++) ed_b(p[i]);
        p += k; n -= k;
    }
}
static const uint8_t *ed_bk_object(uint32_t id, uint32_t *len)
{
    *len = 0;
    if (id == 0u) { *len = sizeof(project_store_t); return ED_BK_RAW; }
    if (id == 1u) { *len = sizeof ed_bk_settings; return (const uint8_t *)&ed_bk_settings; }
    if (id >= 2u && id <= 5u) {
        if (project_used(id - 2u)) *len = sizeof proj_slot[0];
        return (const uint8_t *)&proj_slot[id - 2u];
    }
    if (id == 6u || id == 7u) {
        if (up_bank[id - 6u].magic == UP_BANK_MAGIC) *len = sizeof up_bank[0];
        return (const uint8_t *)&up_bank[id - 6u];
    }
    if (id == 8u)                                   /* the retired FM6 patch bank: always empty */
        return ED_BK_RAW;
    if (id == 9u) {                                 /* the user presets' FM6 patches */
        if (upf_valid(&upf)) *len = sizeof upf;
        return (const uint8_t *)&upf;
    }
    if (id == 40u) {                                /* ShinTech: the project extension */
        *len = 8u + px.used;
        return (const uint8_t *)&px;
    }
    if (id >= 41u && id <= 47u) {                   /* .. the chord pattern bank: built as it is read (ed_bk_cp_bytes) */
        *len = ED_BK_CP_LEN;
        return (const uint8_t *)&ed_bk_cp;
    }
    if (id == 48u || id == 49u) {                   /* .. the user's chord sets, style */
        *len = id == 48u ? sizeof cs_user : sizeof sty_user;
        return id == 48u ? (const uint8_t *)&cs_user : (const uint8_t *)&sty_user;
    }
    if (id >= 32u && id < 32u + SMP_USER_SLOTS) {
        uint32_t k = id - 32u;
        const smp_user_hdr_t *h = (const smp_user_hdr_t *)smp_user_xip(k);
        if (usr_nz[k] && h->magic == SMP_USER_MAGIC && h->data_len <= SMP_USER_SIZE - SMP_USER_DATA)
            *len = SMP_USER_DATA + h->data_len;
        return smp_user_xip(k);
    }
    return 0;
}
static uint32_t ed_bk_capture(void)
{
    if (ed_flash_stop()) return 3;
    cp_flush();                                     /* (ShinTech: the patterns' edits in flash, before they are read) */
    project_capture(&proj_scratch);
    if (!proj_pack((project_store_t *)ED_BK_RAW, &proj_scratch)) return 2;
    ed_bk_gen = ++proj_wire_gen;
#if FELUCCA_FLASH
    ed_bk_settings = persist_saved;                 /* fields absent from this build survive */
#else
    memset(&ed_bk_settings, 0, sizeof ed_bk_settings);
#endif
    settings_export(&ed_bk_settings);
    ed_bk_put = 0; ed_bk_valid = 1; ed_bk_usb = usb.resets;
    return 0;
}
static int ed_bk_panel_valid(const panel_t *p)
{
    uint32_t b = 0, e = 0;
    if (p->magic != PANEL_MAGIC) return 0;
    for (uint32_t i = 0; i < NB; i++) {
        if (p->btn[i] >= NB || (b & (1u << p->btn[i]))) return 0;
        b |= 1u << p->btn[i];
    }
    for (uint32_t i = 0; i < NE; i++) {
        if (p->enc[i] >= NE || (e & (1u << p->enc[i])) || (p->dir[i] != 1 && p->dir[i] != -1)) return 0;
        e |= 1u << p->enc[i];
    }
    return 1;
}
/* ShinTech's backup objects 40..49 (ed_bk_len bytes in raw, the CRC checked): 0 ok, 2 invalid, 4 flash */
static uint32_t ed_bk_shin_commit(const uint8_t *raw)
{
    uint32_t i;
    if (ed_bk_id == 40u) {                          /* the project extension: whole and its records true */
        px_store_t s;
        memset(&s, 0, sizeof s);
        memcpy(&s, raw, ed_bk_len);
        if (8u + s.used != ed_bk_len || !px_valid(&s)) return 2;
        px = s;
#if FELUCCA_FLASH
        if (!flash_ok || st_save(OBJ_PROJEXT, &px, 8u + px.used)) return 4;
#endif
        return 0;
    }
    if (ed_bk_id <= 47u) {                          /* a pattern object: "CPT1", ten patterns the player can play */
        uint32_t k = ed_bk_id - 41u;
        if (raw[0] != 'C' || raw[1] != 'P' || raw[2] != 'T' || raw[3] != '1') return 2;
        for (i = 0; i < CP_PER; i++) {
            memcpy(&ed_bk_cp, raw + 4u + i * sizeof(cpat_t), sizeof ed_bk_cp);
            if (!cp_valid(&ed_bk_cp)) return 2;
        }
        for (i = 0; i < CP_SLOTS; i++)              /* (the cache's copies of these: read again) */
            if (cp_ram[i].id > k * CP_PER && cp_ram[i].id <= (k + 1u) * CP_PER) {
                cp_ram[i].id = 0;
                cp_ram[i].dirty = 0;
            }
#if FELUCCA_FLASH
        if (!flash_ok || st_save(OBJ_CPAT0 + k, raw, ed_bk_len)) return 4;
#else
        for (i = 0; i < CP_PER && k * CP_PER + i < CP_N; i++)
            (void)cp_write(k * CP_PER + i + 1u, (const cpat_t *)(const void *)(raw + 4u + i * sizeof(cpat_t)));
#endif
        return 0;
    }
    if (ed_bk_id == 48u) {
        if (((const cs_user_t *)(const void *)raw)->magic != CS_USER_MAGIC) return 2;
        memcpy(&cs_user, raw, sizeof cs_user);
        ui.force = 1;
        return user_save(OBJ_UCSET) ? 4u : 0u;
    }
    if (((const sty_user_t *)(const void *)raw)->magic != STY_USER_MAGIC) return 2;
    memcpy(&sty_user, raw, sizeof sty_user);
    ui.force = 1;
    return user_save(OBJ_USTY) ? 4u : 0u;
}
static uint32_t ed_bk_commit(void)
{
    uint8_t *raw = ED_BK_RAW;
    uint32_t obj;
    if (ed_bk_pos != ed_bk_len || st_crc32(raw, ed_bk_len) != ed_bk_crc) return 2;
    if (ed_bk_id == 0u || (ed_bk_id >= 2u && ed_bk_id <= 5u)) {
        if (ed_bk_len && !proj_import(&proj_scratch, raw, (int)ed_bk_len)) return 2;
        if (ed_bk_len) {                            /* an older format becomes FUN10 inside its ranges */
            proj_bound(&proj_scratch);
            if (!proj_pack((project_store_t *)raw, &proj_scratch)) return 2;
            ed_bk_len = sizeof(project_store_t);
        }
        if (ed_bk_id == 0u) {
            if (!ed_bk_len) return 2;
            return project_restore_runtime(&proj_scratch) ? 2u : 0u;
        }
        obj = OBJ_PROJECT0 + ed_bk_id - 2u;
    } else if (ed_bk_id == 1u) {
        const persist_t *p = (const persist_t *)raw;
        if (ed_bk_len != sizeof *p || p->magic != PERSIST_MAGIC || !palette_stored_ok(p->palette) ||
            p->lowcut > 2u || !leds_stored_ok(p->zoom) || !hold_stored_ok(p->bold) || p->favorites.filter > 1u || !ed_bk_panel_valid(&p->panel)) return 2;
        obj = OBJ_SETTINGS;
    } else if (ed_bk_id == 6u || ed_bk_id == 7u) {
        const up_bank_t *p = (const up_bank_t *)raw;
        if (ed_bk_len && (ed_bk_len != sizeof *p || p->magic != UP_BANK_MAGIC ||
            p->rsize != sizeof(up_rec_t) || p->nslot != UP_PER_BANK)) return 2;
        /* Unknown record versions remain inert bytes, preserving future/older bank data. */
        obj = OBJ_UPRESET0 + ed_bk_id - 6u;
    } else if (ed_bk_id == 8u) {                    /* an older archive's FM6 bank: into the restored user presets */
        if (!ed_bk_len) return 0;
        if (ed_bk_len != sizeof(fm6_bank_t) || !fm6_bank_valid((const fm6_bank_t *)raw)) return 2;
        upf_migrate((const fm6_bank_t *)raw);
        sync_reload = 1; ui.force = 1;
        return upf_save() == 2 ? 4u : 0u;
    } else if (ed_bk_id == 9u) {                    /* the user presets' FM6 patches (0: none) */
        const upf_t *u = (const upf_t *)raw;
        if (ed_bk_len && (ed_bk_len != sizeof *u || !upf_valid(u) || (u->used >> (UP_SLOTS - 1u) >> 1))) return 2;
        if (ed_bk_len) memcpy(&upf, raw, sizeof upf); else upf_empty();
        sync_reload = 1; ui.force = 1;
        return upf_save() == 2 ? 4u : 0u;           /* (a failed write keeps the restored RAM copy) */
    } else if (ed_bk_id >= 40u && ed_bk_id <= 49u) {   /* ShinTech: checked, into RAM and flash */
        return ed_bk_shin_commit(raw);
    } else return 1;
#if FELUCCA_FLASH
    if (!flash_ok || st_save(obj, raw, ed_bk_len)) return 4;
#else
    (void)obj;
#endif
    if (ed_bk_id >= 2u && ed_bk_id <= 5u) {
        memset(&proj_slot[ed_bk_id - 2u], 0, sizeof proj_slot[0]);
        if (proj_cur == ed_bk_id - 2u)
            proj_cur = PROJ_NO_SLOT;                     /* (another project there now: the music keeps its name) */
        if (ed_bk_len) memcpy(&proj_slot[ed_bk_id - 2u], raw, ed_bk_len);
    } else if (ed_bk_id == 1u) {
        memcpy(&ed_bk_settings, raw, sizeof ed_bk_settings);
        settings_import(&ed_bk_settings, sizeof ed_bk_settings);
        panel_init(); settings_init(); palette_apply();
#if FELUCCA_FLASH
        persist_saved = ed_bk_settings; persist_pending = 0;
#endif
    } else {
        uint32_t b = ed_bk_id - 6u;
        memset(&up_bank[b], 0, sizeof up_bank[b]);
        if (ed_bk_len) memcpy(&up_bank[b], raw, ed_bk_len);
        up_bank_check(b, (int)ed_bk_len); up_gen++;
    }
    sync_reload = 1; ui.force = 1;
    return 0;
}
/* a project object's length: FUN10 (today's, 1.2), FUN9 (1.1), FUN8 (before the DRUM lane levels, 1.0.x), FUN7 */
static int proj_store_len(uint32_t len)
{
    return len == sizeof(project_store_t) || len == PROJ_STORE_V9 || len == PROJ_STORE_V8 || len == PROJ_STORE_V7;
}
static uint32_t ed_bk_write(const uint8_t *a, uint32_t n)
{
    /* the arguments first: a malformed request never stops the transport */
    if (n < 2u || a[0] > 3u || (a[1] > 9u && (a[1] < 40u || a[1] > 49u))) return 1;
    if (a[0] == 0u) {
        if (n != 12u || a[6] > 15u || a[11] > 15u) return 1;
        uint32_t len = ed_bk_r32(a + 2);
        if (len > ED_BK_MAX || (a[1] == 0u && !proj_store_len(len)) ||
            (a[1] == 1u && len != sizeof(persist_t)) ||
            (a[1] >= 2u && a[1] <= 5u && len && !proj_store_len(len)) ||
            ((a[1] == 6u || a[1] == 7u) && len && len != sizeof(up_bank_t)) ||
            (a[1] == 8u && len && len != sizeof(fm6_bank_t)) || (a[1] == 9u && len && len != sizeof(upf_t)) ||
            (a[1] == 40u && (len < 8u || len > sizeof px)) || (a[1] >= 41u && a[1] <= 47u && len != ED_BK_CP_LEN) ||
            (a[1] == 48u && len != sizeof cs_user) || (a[1] == 49u && len != sizeof sty_user)) return 1;
        if (ed_flash_stop()) return 3;
        ed_bk_valid = 0; ed_bk_put = 1; ed_bk_id = a[1]; ed_bk_len = len; ed_bk_gen = ++proj_wire_gen;
        ed_bk_crc = ed_bk_r32(a + 7); ed_bk_pos = 0;
        ed_bk_usb = usb.resets; ed_bk_ms = fm1_ms;
        return 0;
    }
    if (!ed_bk_put || ed_bk_id != a[1] || ed_bk_usb != usb.resets || fm1_ms - ed_bk_ms > 15000u ||
        ed_bk_gen != proj_wire_gen) {                 /* (a project save / load reused the staging RAM) */
        ed_bk_put = 0; return 5;
    }
    ed_bk_ms = fm1_ms;
    if (a[0] == 3u) { ed_bk_put = 0; return n == 2u ? 0u : 1u; }
    if (a[0] == 2u) {
        if (n != 2u) return 1;
        if (ed_flash_stop()) return 3;
        uint32_t rc = ed_bk_commit(); ed_bk_put = 0; return rc;
    }
    if (n < 9u || a[6] > 15u || ed_bk_r32(a + 2) != ed_bk_pos) return 1;
    uint32_t count = ed_unpack7(a + 7, n - 7u, ed_smp_buf, 256u);
    if (!count || count > ed_bk_len - ed_bk_pos) return 1;
    if (ed_flash_stop()) return 3;
    memcpy(ED_BK_RAW + ed_bk_pos, ed_smp_buf, count); ed_bk_pos += count;
    return 0;
}
static int ed_backup_handle(uint32_t cmd, const uint8_t *a, uint32_t n)
{
    if (cmd == ED_BACKUP_LIST) {
        uint32_t rc = n ? 1u : ed_bk_capture();
        ed_b(1); ed_b(rc); ed_b(rc ? 0u : ED_BK_N);
        if (!rc) for (uint32_t i = 0; i < ED_BK_N; i++) {
            uint32_t len;
            const uint8_t *p = ed_bk_object(ED_BK_IDS[i], &len);
            ed_b(ED_BK_IDS[i]); ed_bk_u32(len);
            ed_bk_u32(ED_BK_IDS[i] >= 41u && ED_BK_IDS[i] <= 47u ? ed_bk_cp_crc(ED_BK_IDS[i] - 41u) : st_crc32(p, len));
            fm1_wdt_feed();
        }
        return 1;
    }
    if (cmd == ED_BACKUP_GET) {
        uint32_t len = 0, off = n >= 6u ? ed_bk_r32(a + 1) : 0u;
        uint32_t count = n == 8u ? (uint32_t)a[6] | (uint32_t)a[7] << 7 : 0u;
        const uint8_t *p = n ? ed_bk_object(a[0], &len) : 0;
        uint32_t rc = (!ed_bk_valid || ed_bk_usb != usb.resets || (n && !a[0] && ed_bk_gen != proj_wire_gen)) ? 5u : transport_busy() ? 3u :
            n != 8u || a[5] > 15u || !p || !count || count > 256u || off > len || count > len - off ? 1u : 0u;
        ed_b(n ? a[0] : 127u); ed_b(rc); ed_bk_u32(off); ed_b(rc ? 0u : count & 127u); ed_b(rc ? 0u : count >> 7);
        if (!rc && a[0] >= 41u && a[0] <= 47u) {        /* (ShinTech: the pattern bank, built from it) */
            ed_bk_cp_bytes(a[0] - 41u, off, count, ed_smp_buf);
            ed_bk_pack(ed_smp_buf, count);
        } else if (!rc) ed_bk_pack(p + off, count);
        return 1;
    }
    if (cmd == ED_BACKUP_PUT) {
        uint32_t rc = ed_bk_write(a, n);
        ed_b(n ? a[0] : 127u); ed_b(n >= 2u ? a[1] : 127u); ed_b(rc);
        return 1;
    }
    return 0;
}
