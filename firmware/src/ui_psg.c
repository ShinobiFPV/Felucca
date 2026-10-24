/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 William Kew, ShinTech Electronics */
/* PSG's EDIT panels (ShinTech 16BIT; eng_psg.c, chips/psg.c). Included by ui_graph.c after OPN2's.
 *   EDIT 1 (PSG): the WAVE as the chip makes it, two periods (a tone shape on the chip's own levels; a noise its shift
 *                 register's first bits), its name top left and top right AUTHENTIC (the chip's own: THEME) or
 *                 EXTENDED (the 16BIT extension: ACCENT): the authentic mode marked. The knob just turned in ACCENT.
 *   EDIT 2 (MACROS): one second of the driver, in its 60 Hz frames: the pitch the macros make of a held note (ARP's
 *                 steps, VIB after its delay, SWP's glide; the note's line MID, the pitch THEME) and behind it in
 *                 RAISE the volume DRV steps it down by (ADSR: flat, the track's ENV page shapes it) */

/* tone shape s's level (0..15 attenuation from full, 15 off) at k of 64 steps of a period */
static uint32_t psg_ui_level(uint32_t s, uint32_t k)
{
    uint32_t r;
    switch (s) {
    case PW_P25: return k < 16u ? 0u : 15u;
    case PW_P12: return k < 8u ? 0u : 15u;
    case PW_P6: return k < 4u ? 0u : 15u;
    case PW_SAW: return PSG_RAMP_ATT[15u - k / 4u];
    case PW_RSAW: return PSG_RAMP_ATT[k / 4u];
    case PW_TRI: r = k / 2u; return PSG_RAMP_ATT[r < 16u ? r : 31u - r];
    default: return k < 32u ? 0u : 15u;          /* SQUARE */
    }
}

static void graph_psg_wave(const track_t *t, uint16_t c)
{
    uint32_t w = (uint32_t)clamp(t->p[P_E0], 0, PW_N - 1), k, hot = ui.hot_t ? (uint32_t)cur_page()->id[ui.hot_col & 3u] : 0u;
    int32_t top = 26, bot = 86, x0 = 12, px = x0, py = bot;
    int auth = psg_chip_wave(w);
    uint16_t lc = hot == P_E0 ? T_ACCENT : c;
    cv_rect(PANEL_X0, bot + 2, PANEL_W, 1, T_RAISE);
    if (psg_noise_wave(w)) {                     /* the shift register's first bits, 2 px each */
        psg_t n;
        psg_reset(&n, t->p[P_E7] != 0);
        n.nz_seq = (uint8_t)(w == PW_METAL ? PSG_NZ_METAL : w == PW_BUZZ ? PSG_NZ_BUZZ : PSG_NZ_CHIP);
        n.noise = (uint8_t)(w == PW_PERIOD ? 0u : 4u);
        psg_lfsr_reset(&n);
        for (k = 0; k < 108u; k++) {
            int32_t x = x0 + (int32_t)k * 2, y;
            psg_lfsr_step(&n);
            y = n.lfsr & 1u ? top : bot;
            if (k && y != py)
                cv_rect(x, y < py ? y : py, 1, (y < py ? py - y : y - py) + 1, lc);
            cv_rect(x, y, 2, 2, lc);
            py = y;
        }
    } else {
        for (k = 0; k < 128u; k++) {             /* two periods of 64 steps, 1.7 px a step: a staircase */
            uint32_t a = psg_ui_level(w, k % 64u);
            int32_t x = x0 + (int32_t)(k * 216u / 128u), x1 = x0 + (int32_t)((k + 1u) * 216u / 128u);
            int32_t y = bot - (int32_t)PSG_VOL[a] * (bot - top) / 8191;
            if (k && y != py)                    /* the riser at x */
                cv_rect(x, y < py ? y : py, 2, (y < py ? py - y : y - py) + 2, lc);
            cv_rect(x, y, x1 - x + 2, 2, lc);    /* the tread */
            py = y;
        }
        (void)px;
    }
    cv_text(10, 4 - GOY, &AF_S, N_PSG_WAVE[w], hot == P_E0 ? T_ACCENT : T_MID);
    cv_text_r(228, 4 - GOY, &AF_S, auth ? "AUTHENTIC" : "EXTENDED", auth ? T_THEME : T_ACCENT, T_SURF);
}

static void graph_psg_macros(const track_t *t, uint16_t c)
{
    uint32_t f, drv = (uint32_t)clamp(t->p[P_E2], 0, 15), hot = ui.hot_t ? (uint32_t)cur_page()->id[ui.hot_col & 3u] : 0u;
    int32_t top = 26, bot = 86, mid = 56, x0 = 12, lo = 0, hi = 0, span, px = 0, py = 0, vy = top, pvy = top;
    char b[24];
    for (f = 0; f < 60u; f++) {                  /* the range the macros reach (at least an octave both ways) */
        int32_t p = psg_pitch_at(t, 0, f);
        lo = p < lo ? p : lo;
        hi = p > hi ? p : hi;
    }
    span = (hi > -lo ? hi : -lo);
    span = span < 12 * 16 ? 12 * 16 : span;
    cv_rect(PANEL_X0, bot + 2, PANEL_W, 1, T_RAISE);
    for (f = 0; f < 60u; f++) {                  /* the volume behind: DRV's steps (ADSR: flat at full) */
        uint32_t s = drv ? f / drv : 0u;
        int32_t x = x0 + (int32_t)f * 216 / 60;
        vy = bot - (int32_t)PSG_VOL[s > 15u ? 15u : s] * (bot - top) / 8191;
        if (f && vy != pvy)
            cv_rect(x, vy < pvy ? vy : pvy, 1, (vy < pvy ? pvy - vy : vy - pvy) + 1, T_RAISE);
        cv_rect(x, vy, 216 / 60 + 1, 1, T_RAISE);
        pvy = vy;
    }
    cv_rect(x0, mid, 216, 1, T_MID);             /* the note */
    for (f = 0; f < 60u; f++) {                  /* the pitch, a step a frame */
        int32_t p = psg_pitch_at(t, 0, f), x = x0 + (int32_t)f * 216 / 60, y = mid - p * (mid - top) / span;
        uint16_t lc = (hot == P_E3 || hot == P_E4) && t->p[P_E3] ? T_ACCENT : hot == P_E5 && f >= 12u ? T_ACCENT :
                      hot == P_E6 ? T_ACCENT : c;
        y = clamp(y, top, bot);
        if (f && y != py)
            cv_rect(x, y < py ? y : py, 1, (y < py ? py - y : y - py) + 1, lc);
        cv_rect(x, y, 216 / 60 + 1, 2, lc);
        px = x;
        py = y;
    }
    (void)px;
    cv_text(10, 4 - GOY, &AF_S, "1 S, 60 FRAMES", T_MID);
    str_cpy(b, drv ? "DRV " : "ADSR", sizeof b);
    if (drv)
        fmt_int(b + str_len(b), (int32_t)drv);
    cv_text_r(228, 4 - GOY, &AF_S, b, hot == P_E2 ? T_ACCENT : T_TEXT, T_SURF);
}
