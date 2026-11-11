/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 William Kew, ShinTech Electronics */
/* ShinTech FUNB against NOR flash (project.c px_commit, proj_ext.c, storage.c OBJ_PROJEXT): the project extension
 * through the firmware's own save, load, rename, erase and autosave flows, with power-offs and cuts:
 *   - a CHORD project saved, the power off (RAM lost), on again: it loads with its CHORD setup;
 *   - a cut after the extension's write and before the project's: the slot is the old project with its own setup;
 *   - the extension lost or damaged: the project loads, CHORD at its defaults;
 *   - a rename writes no extension; a project without CHORD (FUN10) writes none;
 *   - the autosave's record is kept through many saves of the four slots, and restores at power-on;
 *   - the store holds no more records than the projects name.
 * Run by tests/run_tests.sh (needs build/gen from one firmware build). */
#define main hostsim_main
#include "hostsim.c"
#undef main

static int32_t fm1_enc_take(uint32_t e) { (void)e; return 0; }
static void fm1_irq_off(void) {}
static void fm1_irq_on(void) {}
static void lcd_sync(void) {}
static void lcd_power(uint32_t s) { (void)s; }
static void lcd_wake_now(void) {}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *p)
{ (void)x; (void)y; (void)w; (void)h; (void)p; }
#define FELUCCA_FLASH 1
#include "../firmware/src/gfx.c"
#include "../firmware/src/panel.c"
#include "../firmware/src/ui.c"
#include "../firmware/src/menu_items.c"
static void panel_setup(void) {}

static uint8_t nor[0x100000], flash_ok = 1;
static uint32_t px_erases;                       /* erases of the extension's sectors 0xE8000 / 0xE9000 */
static int st_read(uint32_t off, void *dst, uint32_t n) { memcpy(dst, nor + off, n); return 0; }
static int st_erase(uint32_t off)
{
    px_erases += off >= 0xE8000u && off < 0xEA000u;
    memset(nor + off, 0xFF, 4096);
    return 0;
}
static int st_prog(uint32_t off, const void *src, uint32_t n)
{
    const uint8_t *s = src;
    uint32_t i;
    for (i = 0; i < n; i++) nor[off + i] &= s[i];
    return 0;
}
static uint32_t irq_save(void) { return 0; }
static void irq_restore(uint32_t f) { (void)f; }
static uint32_t fl_jedec_ram(void) { return 0; }
static void fl_plain_window_init(void) {}
#define FL_FAR(fn) (fn)
#include "../firmware/src/storage.c"
#include "../firmware/src/upreset.c"
#include "../firmware/src/project.c"

static int fails;
static void check(const char *what, int ok)
{
    printf("funb_save: %-84s %s\n", what, ok ? "ok" : "FAIL");
    fails += !ok;
}

/* persist_boot's project part, in its order (the rest of it scans samples through the XIP window: not on the host):
 * the extension reset, read, then the slots */
static void boot_projects(void)
{
    uint32_t i;
    px_reset();
    px_boot();
    for (i = 0; i < 4u; i++)
        if (!proj_import(&proj_scratch, &proj_slot[i], sizeof(project_store_t)))
            proj_fetch(i);
}

/* the power off and on: RAM lost (the retained slots, the pool, the tracks), flash kept */
static void power_cycle(void)
{
    memset(proj_slot, 0, sizeof proj_slot);
    memset(&px, 0xA5, sizeof px);                /* (garbage: persist_boot resets it first) */
    px_as_dig = 0;
    memset(trk, 0, sizeof trk);
    memset(&song, 0, sizeof song);
    host_tracks_init();
    boot_projects();
}

static void chord_setup(uint32_t t, int set, int key)
{
    trk[t].p[P_CHRD] = CHRD_SET;
    trk[t].p[P_CSET] = (int16_t)set;
    trk[t].p[P_CKEY] = (int16_t)key;
    trk[t].p[P_CBASS] = 1;
}
static int has_setup(uint32_t t, int set, int key)
{
    return trk[t].p[P_CHRD] == CHRD_SET && trk[t].p[P_CSET] == set && trk[t].p[P_CKEY] == key && trk[t].p[P_CBASS] == 1;
}
static int at_defaults(uint32_t t)
{
    uint32_t i;
    for (i = PX_FIRST; i < P_E0; i++)
        if (trk[t].p[i] != TP[i].def)
            return 0;
    return 1;
}
static uint32_t px_records(void)
{
    uint32_t at = 0, n = 0;
    while (at + PX_REC_HDR <= px.used) {
        at += PX_REC_HDR + px_pad(px.data[at + 4u] | (uint32_t)px.data[at + 5u] << 8);
        n++;
    }
    return n;
}

int main(void)
{
    static uint8_t snap[0x100000];
    uint32_t e, i, ok;
    memset(nor, 0xFF, sizeof nor);
    host_tracks_init();
    boot_projects();
    check("power-on with an empty flash: the extension empty and valid", flash_ok && px_valid(&px) && px.used == 0u);

    /* 1. saved, power off, on */
    chord_setup(0, 5, 3);
    trk[0].p[P_LEVEL] = 77;
    e = px_erases;
    check("a CHORD project saves to slot A", project_save(0) == 0 && px_u32(proj_slot[0].raw) == PROJ_MAGIC_B);
    check(".. writing the extension once", px_erases == e + 1u);
    power_cycle();
    project_load(0);
    check("power off, on, load A: its CHORD setup and the rest back", has_setup(0, 5, 3) && trk[0].p[P_LEVEL] == 77);

    /* 2. a cut between the extension's write and the project's */
    memcpy(snap, nor, sizeof nor);
    chord_setup(0, 9, 11);
    check("slot A saved again with another setup", project_save(0) == 0);
    memcpy(nor + 0x97000u, snap + 0x97000u, 0x2000u);   /* the project's write undone: the cut came before it */
    power_cycle();
    project_load(0);
    check("a cut after the extension, before the project: A is the old project with its old setup", has_setup(0, 5, 3));
    check(".. the store holds both records (the old one kept for it)", px_records() == 2u);

    /* 3. the extension lost: the project still loads */
    chord_setup(0, 9, 11);
    project_save(0);
    memcpy(snap, nor, sizeof nor);
    memset(nor + 0xE8000u, 0xFF, 0x2000u);
    power_cycle();
    project_load(0);
    check("the extension erased: A loads, CHORD SET kept, its setup at the defaults",
          trk[0].p[P_CHRD] == CHRD_SET && at_defaults(0) && trk[0].p[P_LEVEL] == 77);
    memcpy(nor, snap, sizeof nor);
    {
        st_hdr_t h;
        int c = st_current(OBJ_PROJEXT, &h);
        nor[st_sector(OBJ_PROJEXT, (uint32_t)c) + ST_PAYLOAD_OFF + 20u] ^= 0x40u;   /* its newest copy damaged */
    }
    power_cycle();
    project_load(0);
    check("the newest extension copy damaged: the older one read, which lacks A's record: A loads at the defaults",
          trk[0].p[P_CHRD] == CHRD_SET && at_defaults(0) && trk[0].p[P_LEVEL] == 77 && px_valid(&px) && px_records() == 1u);
    memcpy(nor, snap, sizeof nor);
    power_cycle();

    /* 4. rename, FUN10 */
    e = px_erases;
    check("a rename of A writes no extension, A keeps its setup",
          project_rename(0, "RENAMED") == 0 && px_erases == e && (project_load(0), has_setup(0, 9, 11)));
    memset(trk, 0, sizeof trk);
    host_tracks_init();
    e = px_erases;
    check("a project without CHORD saves to B as FUN10, writing no extension",
          project_save(1) == 0 && px_u32(proj_slot[1].raw) == PROJ_MAGIC && px_erases == e);

    /* 5. the autosave's record through many saves */
    chord_setup(2, 33, 4);
    fm1_ms = 1000000u;
    autosave_poll();
    fm1_ms += AS_IDLE_MS + AS_GAP_MS + 1000u;
    autosave_poll();
    check("an autosave of a CHORD setup written (FUNB)", as.writes == 1u && px_as_dig != 0u);
    for (i = 0, ok = 1; i < 40u; i++) {
        memset(trk, 0, sizeof trk);
        host_tracks_init();
        chord_setup(i % 4u, (int)(i % CS_NSETS), (int)(i % 12u));
        ok &= project_save(i % 4u) == 0 && px_valid(&px);
    }
    check("40 saves of the four slots, each a new setup: every save done, the store valid", ok);
    check(".. the store holds at most the four slots' and the autosave's records, and one more", px_records() <= 6u);
    memset(trk, 0, sizeof trk);
    host_tracks_init();
    power_cycle();
    ui_prefs &= ~PREF_RESTORE_OFF;
    check("power off, on: the autosave restores with its CHORD setup", autosave_boot(1) == 1 && has_setup(2, 33, 4));
    project_load(3);
    check(".. and slot D its last one", has_setup(3, 39 % CS_NSETS, 39 % 12));

    /* 6. erased slots let their records go */
    for (i = 0; i < 4u; i++)
        project_erase(i);
    memset(trk, 0, sizeof trk);
    host_tracks_init();
    chord_setup(0, 1, 1);
    project_save(0);
    check("the slots erased, one saved: the store holds its record and the autosave's", px_records() == 2u);
    printf("funb_save: %s\n", fails ? "FAILED" : "all ok");
    return fails != 0;
}
