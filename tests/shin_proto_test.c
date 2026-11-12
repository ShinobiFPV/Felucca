/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 William Kew, ShinTech Electronics */
/* ShinTech CHORD editor commands (firmware/src/editor_shin.c, cmds 90..96) and its backup ids (editor_backup.c 40..49) over the
 * firmware's own SysEx handler: the chord sets (factory read back as cs_get has them; a user set PUT, read back, played,
 * refused, erased), the styles (a factory variation as the table; USER PUT, read back, played; refused), the chord patterns
 * (PUT, GET, refused), BACKUP_LIST's 23 objects, a pattern object read, the user's sets restored. The harness: tests/editor_test.c's.
 * Run by tests/run_tests.sh (needs build/gen from one firmware build). */
static unsigned char host_samples[3][0x14000];
#define SMP_USER_XIP(k) host_samples[k]
#define FELUCCA_OTA 1
#define FELUCCA_FLASH 0
#define FELUCCA_VERSION "TEST"
#define FELUCCA_CDC 1                                    /* (as the firmware: MENU > USB SERIAL switches the console) */
#define main hostsim_main
#include "hostsim.c"
#undef main
#define MENU_N (32u + FELUCCA_CPU2)                  /* 1.6.1 (core.h FELUCCA_CPU2, the default): 2ND CORE, id 32 */

static uint32_t host_progress = 1, host_erases, host_writes;
static uint8_t host_wire[4096];
static uint32_t host_wire_n;
static void host_drain(void)
{
    while (so_r != so_w) {
        uint32_t p = sx_out_q[so_r++ % SXQ], cin = p & 15u, i;
        uint32_t n = cin == 4u || cin == 7u ? 3u : cin == 6u ? 2u : 1u;
        for (i = 0; i < n && host_wire_n < sizeof host_wire; i++)
            host_wire[host_wire_n++] = (uint8_t)(p >> (8u * (i + 1u)));
    }
}
static uint32_t ota_now_ms(void) { return fm1_ms; }
static void ota_idle(void) { host_drain(); fm1_ms++; }
static void fm1_wdt_feed(void)
{
    fm1_ms++;
    if (host_progress && transport_req == 2u) { seq_stop(); transport_req = 0; }
}
static void fm1_irq_off(void) {}
static void fm1_irq_on(void) {}
static int32_t fm1_enc_take(uint32_t e) { (void)e; return 0; }
static void lcd_sync(void) {}
static void lcd_power(uint32_t s) { (void)s; }   /* (MENU > SCREEN OFF: lcd.c) */
static void lcd_wake_now(void) {}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *p)
{ (void)x; (void)y; (void)w; (void)h; (void)p; }
#include "../firmware/src/gfx.c"
#include "../firmware/src/panel.c"
#include "../firmware/src/ui.c"
#include "../firmware/src/menu_items.c"
static void panel_setup(void) {}
#include "../firmware/src/upreset.c"
#include "../firmware/src/project.c"

static uint32_t flash_ok = 1;
static void audio_silence(void) {}
static void fl_inval(uint32_t off, uint32_t n) { (void)off; (void)n; }
static uint8_t *host_flash_ptr(uint32_t off) { return &host_samples[0][0] + off - SMP_USER_BASE; }
static int fl_erase4k(uint32_t off, uint32_t *took)
{
    memset(host_flash_ptr(off), 0xFF, 4096); *took = 0; host_erases++; return 0;
}
static int fl_write(uint32_t off, const void *p, uint32_t n)
{
    memcpy(host_flash_ptr(off), p, n); host_writes++; return 0;
}
static int st_read(uint32_t off, void *p, uint32_t n) { (void)off; (void)p; (void)n; return -1; }
static int st_prog(uint32_t off, const void *p, uint32_t n) { (void)off; (void)p; (void)n; return -1; }
static int st_erase(uint32_t off) { (void)off; return -1; }
#include "../firmware/src/storage.c"
#include "../firmware/src/editor.c"

static void reset(void)
{
    uint32_t t, i;
    memset(&song, 0, sizeof song); memset(trk, 0, sizeof trk);
    memset(&chain, 0, sizeof chain); chain_defaults(&chain_config);
    memset(&ed_w, 0, sizeof ed_w); memset(&ui, 0, sizeof ui);
    memset(&favorites, 0, sizeof favorites); memset(&settings, 0, sizeof settings); settings_init();
    memset(proj_slot, 0, sizeof proj_slot); memset(up_bank, 0, sizeof up_bank);
    memset(&um, 0, sizeof um); memset(usr_nz, 0, sizeof usr_nz);
    host_progress = 1; host_erases = host_writes = host_wire_n = 0;
    transport_req = panic_req = 0; sx_ready = sx_collect = sx_busy = 0;
    so_r = so_w = mi_r = mi_w = 0; midi_in_overflow = 0; usb.config = 1;
    for (i = 0; i < G_COUNT; i++) song.g[i] = GP[i].def;
    for (t = 0; t < NTRK; t++) {
        track_defaults(&trk[t]); set_engine_of(&trk[t], 0); apply_preset_to(&trk[t], 0);
        trk[t].engine = trk[t].eng_req; track_defaults_steps(&trk[t]);
    }
}
static uint32_t request(uint32_t cmd, const uint8_t *a, uint32_t n)
{
    uint32_t i;
    host_wire_n = 0; ota_frame_done();
    sysex_byte(0xF0); sysex_byte(ED_HDR0); sysex_byte(ED_HDR1); sysex_byte(ED_HDR2); sysex_byte((uint8_t)cmd);
    for (i = 0; i < n; i++) sysex_byte(a[i]);
    sysex_byte(0xF7); ed_service(); host_drain();
    return host_wire_n;
}
static uint32_t pack7(const uint8_t *p, uint32_t n, uint8_t *a)
{
    uint32_t o = 0, i;
    while (n) {
        uint32_t k = n > 7u ? 7u : n, m = o++;
        a[m] = 0;
        for (i = 0; i < k; i++, n--) { a[m] |= (*p >> 7) << i; a[o++] = *p++ & 127u; }
    }
    return o;
}


static int bad;
static int ck(const char *what, int ok)
{
    printf("shin_proto: %-92s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
    return ok;
}
static uint32_t cmd_rc(void) { return host_wire[6]; }   /* (most replies: arg, rc) */
/* host_wire's payload from byte 5 (after F0 7D 46 4C cmd) */
#define W(i) host_wire[5u + (i)]

/* a reply's string at payload offset *at -> s; *at after its 0 */
static void rstr(uint32_t *at, char *s, uint32_t max)
{
    uint32_t k = 0;
    while (W(*at) && k + 1u < max)
        s[k++] = (char)W((*at)++);
    s[k] = 0;
    while (W(*at))
        (*at)++;
    (*at)++;
}

int main(void)
{
    uint8_t a[700];
    uint32_t n, i, k, at, ok;
    char s[32];

    /* chord sets: the list */
    reset();
    memset(&cs_user, 0, sizeof cs_user);
    a[0] = 0; a[1] = 16;
    request(ED_CS_LIST, a, 2);
    at = 4;
    for (i = 0, ok = 1; i < 16u; i++) {
        ok &= W(at) == 1u && W(at + 1u) == CS_SET[i].genre;
        at += 2;
        rstr(&at, s, sizeof s);
        ok &= !strncmp(s, CS_SET[i].name, CS_NAME_LEN);
    }
    ck("CS_LIST 0, 16: sixteen factory sets (used, genre, name); 100 factory, 16 user", W(0) == 0 && W(1) == 16u &&
       W(2) == CS_NSETS && W(3) == CS_NUSER && ok);
    a[0] = CS_NSETS; a[1] = 16;
    request(ED_CS_LIST, a, 2);
    ck("CS_LIST of the user's: sixteen, empty (unused, no name)", W(1) == 16u && W(4) == 0u && W(5) == CS_NGENRES && W(6) == 0u);

    /* a factory set's chords, as the device plays them */
    a[0] = 3;
    request(ED_CS_GET, a, 1);
    at = 2;
    rstr(&at, s, sizeof s);
    at++;
    for (k = 0, ok = W(1) == 0u && !strncmp(s, CS_SET[3].name, CS_NAME_LEN); k < 12u; k++, at += 8u) {
        cs_chord_t c;
        uint32_t m = W(at + 4u) | (uint32_t)W(at + 5u) << 7 | (uint32_t)W(at + 6u) << 14 | (uint32_t)W(at + 7u) << 21, j, mm = 0;
        cs_get(3, k, &c);
        for (j = 1; j < c.n; j++)
            mm |= 1u << (c.iv[j] - 1);
        ok &= W(at) == c.root && W(at + 1u) == (uint32_t)(c.bass + 1) && W(at + 2u) == c.voicing && W(at + 3u) == c.inv && m == mm;
    }
    ck("CS_GET of a factory set: its name, then each chord's root, bass, voicing, inversion, notes (as cs_get)", (int)ok);

    /* the user's: PUT, GET back, played */
    {
        static const uint8_t ROOTS[12] = {0, 5, 7, 9, 2, 4, 0, 5, 7, 9, 11, 1};
        uint32_t len;
        a[0] = 2; a[1] = 1;
        memcpy(a + 2, "MY SET", 7);
        len = 9;
        for (k = 0; k < 12u; k++, len += 8u) {          /* triads with a seventh (0 4 7 11), C / E on chord 1 */
            a[len] = ROOTS[k]; a[len + 1u] = k == 0u ? 5u : 0u; a[len + 2u] = 0; a[len + 3u] = 0;
            a[len + 4u] = (uint8_t)((1u << 3 | 1u << 6 | 1u << 10) & 127u); a[len + 5u] = (uint8_t)((1u << 10) >> 7);
            a[len + 6u] = 0; a[len + 7u] = 0;
        }
        request(ED_CS_PUT, a, len);
        ok = W(0) == 2u && W(1) == 0u && cs_user.set[2].used && !strcmp(cs_user.set[2].name, "MY SET");
        ck("CS_PUT USR3 \"MY SET\": rc 0, stored", (int)ok);
        a[0] = (uint8_t)(CS_NSETS + 2u);
        n = request(ED_CS_GET, a, 1);
        at = 2;
        rstr(&at, s, sizeof s);
        ok = W(1) == 0u && !strcmp(s, "MY SET") && W(at) == CS_NGENRES;
        at++;
        ok &= W(at) == 0u && W(at + 1u) == 5u && W(at + 4u) == (uint8_t)((1u << 3 | 1u << 6 | 1u << 10) & 127u) && W(at + 8u) == 5u;
        ck("CS_GET USR3: the same back (C/E, then F ..)", (int)ok);
        {
            track_t *t = &trk[0];
            uint8_t out[CHORD_NOTES];
            char b[CS_NAME_MAX];
            uint32_t m, pcs = 0;
            t->p[P_CHRD] = CHRD_SET;
            t->p[P_VOICE] = V_POLY;
            t->p[P_CSET] = (int16_t)(CS_NSETS + 2u);
            m = chord_build(t, 61u, out);                  /* chord 2: F maj7 */
            for (i = 0; i < m; i++)
                pcs |= 1u << (out[i] % 12u);
            cs_name(b, chord_last[0].root % 12u, chord_last[0].mask, chord_last[0].bass);
            ck("a track on SET USR3 plays its chord 2: F A C E (Fmaj7)", pcs == (1u << 5 | 1u << 9 | 1u << 0 | 1u << 4) &&
               !strcmp(b, "Fmaj7"));
            {
                char v[8];
                const char *u;
                param_format(&TP[P_CSET], CS_NSETS + 2, v, &u);
                ck("the SET knob shows it USR3", !strcmp(v, "USR3"));
            }
        }
        a[len - 8u] = 12;                                  /* (a root of 12) */
        request(ED_CS_PUT, a, len);
        ck("CS_PUT with a root of 12: rc 1, nothing changed", W(1) == 1u && cs_user.set[2].ch[11].root == 1u);
        a[0] = 2; a[1] = 0;
        request(ED_CS_PUT, a, 2);
        a[0] = (uint8_t)(CS_NSETS + 2u);
        request(ED_CS_GET, a, 1);
        ck("CS_PUT used 0: erased; CS_GET answers rc 2 (empty)", W(1) == 2u && !cs_user.set[2].used);
    }

    /* styles */
    a[0] = 0; a[1] = 1;                                   /* ARP1 var 2: UP 1/16 */
    request(ED_STY_GET, a, 2);
    at = 3;
    rstr(&at, s, sizeof s);
    {
        const cst_var_t *v = &CST_VAR[1];
        ok = W(2) == 0u && !strncmp(s, v->name, CST_NAME_LEN) && W(at) == v->steps && W(at + 1u) == v->rate &&
             W(at + 2u) == v->strum && W(at + 3u) == v->oct && W(at + 4u) == v->nev;
        for (i = 0, at += 5u; ok && i < v->nev; i++, at += 6u) {
            const uint8_t *e = CST_EV[v->ev + i];
            ok &= W(at) == e[0] && W(at + 1u) == (e[1] & 63u) && W(at + 2u) == (e[1] >> 6u) && W(at + 3u) == e[2] &&
                  W(at + 4u) == (e[3] & 63u) && W(at + 5u) == (e[3] >> 6u);
        }
        ck("STY_GET ARP1 2: its name, steps, rate, strum, octaves and events (as the table)", (int)ok);
    }
    a[0] = 9; a[1] = 0;
    request(ED_STY_GET, a, 2);
    ck("STY_GET USER 1, none stored: rc 2", W(2) == 2u);
    {
        uint32_t len = 0;
        a[len++] = 0;                                     /* USER 1: "MY STRUM", 8 x 1/8, strum 20 ms */
        memcpy(a + len, "MY STRUM", 9); len += 9;
        a[len++] = 8; a[len++] = 2; a[len++] = 20; a[len++] = 1; a[len++] = 2;
        a[len++] = 0; a[len++] = 32; a[len++] = 0; a[len++] = 110; a[len++] = 7; a[len++] = 1;   /* step 1: A, down, 2 steps */
        a[len++] = 4; a[len++] = 34; a[len++] = 0; a[len++] = 90; a[len++] = 3; a[len++] = 2;    /* step 5: U, up, 1 step */
        request(ED_STY_PUT, a, len);
        ok = W(0) == 0u && W(1) == 0u && sty_v(STY_NUSER) != &STY_EMPTY && sty_v(STY_NUSER)->nev == 2u;
        ck("STY_PUT USER 1: rc 0, stored (two events)", (int)ok);
        a[0] = 9; a[1] = 0;
        request(ED_STY_GET, a, 2);
        at = 3;
        rstr(&at, s, sizeof s);
        ck("STY_GET USER 1: the same back", W(2) == 0u && !strcmp(s, "MY STRUM") && W(at) == 8u && W(at + 4u) == 2u &&
           W(at + 5u) == 0u && W(at + 6u) == 32u && W(at + 10u) == 1u && W(at + 11u) == 4u && W(at + 16u) == 2u);
        trk[0].p[P_CSTY] = (int16_t)(CST_NSTY + 1u);
        trk[0].p[P_CVAR] = 0;
        {
            char v[8];
            const char *u;
            param_format(&TP[P_CSTY], CST_NSTY + 1, v, &u);
            ck("STYLE's last value USER; a track on it plays variation USER 1", !strcmp(v, "USER") &&
               sty_var_of(&trk[0]) == STY_NUSER);
        }
        a[0] = 1;                                         /* (a strum on a single note: refused) */
        a[len - 5u] = 3;
        request(ED_STY_PUT, a, len);
        ck("STY_PUT with a strum on a single note: rc 1, nothing stored", W(1) == 1u && sty_v(STY_NUSER + 1u) == &STY_EMPTY);
    }

    /* chord patterns */
    {
        static cpat_t p, q;
        uint32_t len;
        memset(&p, 0, sizeof p);
        p.len = 8; p.div = 1; p.next = 3;
        p.step[0].chord = CPC_SET + 4; p.step[0].style = 2; p.step[1].chord = CPC_TIE; p.step[2].note = 72; p.step[3].tempo = 81;
        a[0] = 5;
        len = 1u + pack7((const uint8_t *)&p, sizeof p, a + 1);
        request(ED_CPAT_PUT, a, len);
        ck("CPAT_PUT 5: rc 0, in the bank", W(0) == 5u && W(1) == 0u && cp_get(5) && !memcmp(cp_get(5), &p, sizeof p));
        a[0] = 5;
        n = request(ED_CPAT_GET, a, 1);
        ok = W(1) == 0u && ed_unpack7(&W(2), n - 8u, (uint8_t *)&q, sizeof q) == sizeof q && !memcmp(&p, &q, sizeof p);
        ck("CPAT_GET 5: the same 328 bytes back", (int)ok);
        p.step[5].chord = 14;                            /* (neither a set chord nor a hand chord) */
        a[0] = 6;
        len = 1u + pack7((const uint8_t *)&p, sizeof p, a + 1);
        request(ED_CPAT_PUT, a, len);
        ck("CPAT_PUT with a chord of 14: rc 1", W(1) == 1u);
    }

    /* the backup */
    request(ED_BACKUP_LIST, a, 0);
    {
        static const uint8_t IDS[23] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 32, 33, 34};
        ok = W(1) == 0u && W(2) == 23u;
        for (i = 0; i < 23u; i++)
            ok &= W(3u + i * 11u) == IDS[i];
        ck("BACKUP_LIST: 23 objects, ShinTech's 40..49 after id 9", (int)ok);
    }
    {
        uint8_t b[16];
        static cpat_t p;
        a[0] = 41; a[1] = 0; a[2] = 0; a[3] = 0; a[4] = 0; a[5] = 0; a[6] = 64; a[7] = 0;   /* 41: offset 0, 64 bytes */
        request(ED_BACKUP_GET, a, 8);
        ed_unpack7(&W(9), 80, b, 16);
        cp_read(5, &p);
        ck("BACKUP_GET 41 (patterns 1..10): \"CPT1\" first", W(1) == 0u && b[0] == 'C' && b[3] == '1');
        a[1] = (uint8_t)((4u + 4u * 328u) & 127u); a[2] = (uint8_t)((4u + 4u * 328u) >> 7);   /* pattern 5's bytes */
        a[6] = 16;
        request(ED_BACKUP_GET, a, 8);
        ed_unpack7(&W(9), 40, b, 16);
        ck(".. at 4 + 4 x 328 pattern 5 (as CPAT_GET)", W(1) == 0u && !memcmp(b, &p, 16));
    }
    {   /* a restore of 48, the user's sets: begin, data, commit */
        static cs_user_t keep;
        uint32_t len = sizeof cs_user, crc, off, step;
        memset(&cs_user, 0, sizeof cs_user);
        cs_user.magic = CS_USER_MAGIC;
        cs_user.set[7].used = 1;
        str_cpy(cs_user.set[7].name, "RESTORED", CS_NAME_LEN);
        keep = cs_user;
        crc = st_crc32(&keep, len);
        memset(&cs_user, 0, sizeof cs_user);
        a[0] = 0; a[1] = 48;
        for (i = 0; i < 5u; i++) { a[2 + i] = (uint8_t)((len >> (7u * i)) & 127u); a[7 + i] = (uint8_t)((crc >> (7u * i)) & 127u); }
        request(ED_BACKUP_PUT, a, 12);
        ok = W(2) == 0u;
        for (off = 0; off < len; off += step) {
            step = len - off < 200u ? len - off : 200u;
            a[0] = 1; a[1] = 48;
            for (i = 0; i < 5u; i++) a[2 + i] = (uint8_t)((off >> (7u * i)) & 127u);
            n = 7u + pack7((const uint8_t *)&keep + off, step, a + 7);
            request(ED_BACKUP_PUT, a, n);
            ok &= W(2) == 0u;
        }
        a[0] = 2; a[1] = 48;
        request(ED_BACKUP_PUT, a, 2);
        ck("BACKUP_PUT 48: the user's chord sets restored (USR8 \"RESTORED\")", ok && W(2) == 0u && cs_user.set[7].used &&
           !strcmp(cs_user.set[7].name, "RESTORED"));
    }

    printf(bad ? "shin_proto: FAILED\n" : "shin_proto: all ok\n");
    return bad != 0;
}
