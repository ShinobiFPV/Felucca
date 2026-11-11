/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 William Kew, ShinTech Electronics */
/* ShinTech CHORD patterns' bank against NOR flash (firmware/src/cpat_store.c, storage.c OBJ_CPAT0..): every one of the 64
 * patterns written and read back after a power cycle (RAM lost); a write keeps the other patterns of its object; nothing
 * outside the bank's sectors changes; a cut between the erase and the commit record keeps the pattern as it was; a
 * damaged newest copy falls back to the older; an object never written reads as empty patterns; the cache writes an
 * edit back only when quiet, never while the transport runs; no write without an edit.
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
static void fl_plain_window_init(void) {}
#define FL_FAR(fn) (fn)

static uint8_t nor[0x100000], flash_ok = 1;
static uint32_t erases, cut_after;                  /* cut_after: the program calls left before the power fails, 0 none */
static int st_read(uint32_t off, void *dst, uint32_t n) { memcpy(dst, nor + off, n); return 0; }
static int st_erase(uint32_t off)
{
    erases++;
    memset(nor + off, 0xFF, 4096);
    return 0;
}
static int st_prog(uint32_t off, const void *src, uint32_t n)
{
    const uint8_t *s = src;
    uint32_t i;
    if (cut_after && !--cut_after)
        return -5;                                  /* (the power fails: this write and every one after it) */
    for (i = 0; i < n; i++) nor[off + i] &= s[i];
    return 0;
}
static uint32_t irq_save(void) { return 0; }
static void irq_restore(uint32_t f) { (void)f; }
static uint32_t fl_jedec_ram(void) { return 0; }
#include "../firmware/src/storage.c"
#include "../firmware/src/upreset.c"
#include "../firmware/src/project.c"

static int fails;
static void check(const char *what, int ok)
{
    printf("cpat_store: %-96s %s\n", what, ok ? "ok" : "FAIL");
    fails += !ok;
}
static void fill(cpat_t *p, uint32_t id)            /* a pattern of its own for id */
{
    uint32_t i;
    memset(p, 0, sizeof *p);
    p->len = (uint8_t)(1u + id % 64u);
    p->div = (uint8_t)(id % 10u);
    p->set = (uint8_t)(1u + id % 100u);
    p->next = (uint8_t)((id % 64u) + 1u);
    p->nhand = 1;
    p->hand[0] = (cp_hand_t){(uint8_t)(id % 12u), {(uint8_t)(id * 3u), 1, 0}};
    for (i = 0; i < CP_STEPS; i++)
        p->step[i] = (cp_step_t){(uint8_t)((i + id) % 14u), (uint8_t)(i % 7u), (uint8_t)(i == id % 64u ? 60u : 0u), 0};
}
static int same(uint32_t id)                        /* flash holds fill(id) */
{
    cpat_t a, b;
    fill(&a, id);
    cp_read(id, &b);
    return !memcmp(&a, &b, sizeof a);
}
static void power_cycle(void) { memset(cp_ram, 0, sizeof cp_ram); memset(cpp, 0, sizeof cpp); memset(&proj_scratch, 0xA5, sizeof proj_scratch); }

int main(void)
{
    static uint8_t snap[0x100000];
    cpat_t a, b;
    uint32_t id, ok, o, e0;
    memset(nor, 0xFF, sizeof nor);
    host_tracks_init();

    cp_read(17, &a);
    memset(&b, 0, sizeof b);
    check("nothing written: a pattern reads empty", !memcmp(&a, &b, sizeof a));

    for (id = 1, ok = 1; id <= CP_N; id++) {
        fill(&a, id);
        ok &= cp_write(id, &a) == 0;
    }
    power_cycle();
    for (id = 1; id <= CP_N; id++)
        ok &= same(id);
    check("all 64 written one by one, read back after a power cycle", ok);
    for (o = 0, ok = 1; o < 0x100000u; o += 4096u)
        if (nor[o] != 0xFFu || memcmp(nor + o, nor + o + 1, 4095))   /* (a sector written to) */
            ok &= o >= 0xEA000u && o < 0xF8000u;
    check("only the bank's sectors 0xEA000..0xF7FFF written", ok);

    memcpy(snap, nor, sizeof nor);
    fill(&a, 99);                                   /* pattern 23 changed: object 2 (21..30) */
    ok = cp_write(23, &a) == 0;
    cp_read(23, &b);
    ok &= !memcmp(&a, &b, sizeof a);
    for (id = 21; id <= 30; id++)
        if (id != 23)
            ok &= same(id);
    for (o = 0; o < 0x100000u; o += 4096u)
        if (o != st_sector(OBJ_CPAT0 + 2u, 0) && o != st_sector(OBJ_CPAT0 + 2u, 1))
            ok &= !memcmp(snap + o, nor + o, 4096);
    check("a write keeps the other patterns of its object, and touches no other object", ok);

    fill(&a, 99);                                   /* (23 holds 99 now) */
    fill(&b, 77);
    for (o = 1, ok = 1; o <= 16u; o++) {             /* the power fails at the o-th program of the write */
        cpat_t got;
        uint32_t olds = 0, news = 0;
        cut_after = o;
        (void)cp_write(23, &b);
        cut_after = 0;
        power_cycle();
        cp_read(23, &got);
        olds = !memcmp(&got, &a, sizeof got);
        news = !memcmp(&got, &b, sizeof got);
        ok &= olds || news;
        ok &= o < 15u ? olds : 1u;                   /* (before its commit record: the old one) */
        (void)cp_write(23, &a);
    }
    check("a power cut at any point of a write: the pattern before it, or the new one, whole", ok);

    fill(&a, 50);
    ok = cp_write(41, &a) == 0;                      /* 41's object (41..50): its newest copy damaged */
    {
        st_hdr_t h;
        int c = st_current(OBJ_CPAT0 + 4u, &h);
        nor[st_sector(OBJ_CPAT0 + 4u, (uint32_t)c) + ST_PAYLOAD_OFF + 100u] ^= 0x10u;
    }
    power_cycle();
    ok &= same(41);
    check("the newest copy damaged: the older copy (41 as it was) reads", ok);

    /* the cache: edits written back when quiet, not while it plays */
    power_cycle();
    trk[0].p[P_CHRD] = CHRD_SET;
    trk[0].p[P_CPAT] = 5;
    cpat_poll();
    ok = cp_find(5) >= 0 && same(5);
    {
        cpat_t *p = cp_edit(5);
        ok &= p != 0;
        p->step[0].chord = 7;
        cp_dirty(5);
    }
    e0 = erases;
    song.playing = 1;
    fm1_ms += 5000u;
    cpat_poll();
    ok &= erases == e0;
    song.playing = 0;
    cpat_poll();
    cp_read(5, &b);
    check("an edit in the cache: not written while the transport runs; written when stopped, 2 s after",
          ok && erases > e0 && b.step[0].chord == 7u && !cp_ram[cp_find(5)].dirty);
    e0 = erases;
    fm1_ms += 5000u;
    cpat_poll();
    check("nothing edited since: no write", erases == e0);
    trk[0].p[P_CPAT] = 0;
    for (id = 1; id <= 20u; id++) {                  /* the cache full of what the UI looked at: evicted, still */
        cp_ui_id = (uint8_t)id;                      /* loads what is wanted */
        cpat_poll();
    }
    cp_ui_id = 0;
    trk[0].p[P_CPAT] = 64;
    cpat_poll();
    check("the cache: patterns no longer wanted give way to one that is", cp_find(64) >= 0 && same(64));

    printf(fails ? "cpat_store: FAILED\n" : "cpat_store: all ok\n");
    return fails != 0;
}
