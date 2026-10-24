/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
#define FELUCCA_FAVORITES 1
/* Stable engine/preset references; fixed capacity independent of optional engines. */
typedef struct {
    uint8_t factory[16][32]; /* engine 0..15, preset 0..255 */
    uint32_t user, filter;
} favorites_t;
static favorites_t favorites __attribute__((section(".pool")));
/* the user presets in this API (favorites.user); no engine has this number. The editor protocol says NENGINES for
 * them, as its INFO reports it (editor_preferences.c turns it into this; the PRESETS list too, ui.c) */
#define FAV_USER 0xFEu
#ifndef NENG_V16
#define NENG_V16 14u                       /* (core.h; tests/settings_test.c builds this file on its own) */
#endif
/* ShinTech: engines from 14 on (OPN2 ..) have no rows of their own (factory[14] and [15] hold the MIDI LEARN map and
 * the palette); their presets 0..127 live in the upper halves (bits 128..255) of rows 0, 2 and 3, which no engine
 * uses (none has 128 presets; row 1, DIGITAL's, is cleared by a migration: settings_persist.c). Firmware without
 * these engines never reads those bits */
static const uint8_t FAV_ROW16[3] = {0, 2, 3};
/* engine e's preset k -> its byte, *bit its bit; 0 = none */
static uint8_t *fav_bit(uint32_t e, uint32_t k, uint32_t *bit)
{
    if (e < NENG_V16 && k < 256u) {
        *bit = k % 8u;
        return &favorites.factory[e][k / 8u];
    }
    if (e >= NENG_V16 && e - NENG_V16 < sizeof FAV_ROW16 && k < 128u) {
        *bit = k % 8u;
        return &favorites.factory[FAV_ROW16[e - NENG_V16]][16u + k / 8u];
    }
    return 0;
}
static int favorite_has(uint32_t engine, uint32_t preset)
{
    uint32_t bit;
    const uint8_t *b;
    if (engine == FAV_USER || engine == NENGINES)   /* (NENGINES: the PRESETS list's own name for them, ui.c) */
        return preset < UP_SLOTS && ((favorites.user >> preset) & 1u);
    b = engine < NENGINES ? fav_bit(engine, preset, &bit) : 0;
    return b && ((*b >> bit) & 1u);
}
static int favorite_set(uint32_t engine, uint32_t preset, int on)
{
    uint32_t bit;
    uint8_t *b;
    if (engine == NENGINES)
        engine = FAV_USER;
    if (engine == FAV_USER) {
        if (preset >= UP_SLOTS || favorite_has(engine, preset) == !!on)
            return 0;
        if (on) favorites.user |= 1u << preset;
        else favorites.user &= ~(1u << preset);
        return 1;
    }
    b = engine < NENGINES ? fav_bit(engine, preset, &bit) : 0;
    if (!b || favorite_has(engine, preset) == !!on)
        return 0;
    if (on) *b |= (uint8_t)(1u << bit);
    else *b &= (uint8_t)~(1u << bit);
    return 1;
}

/* MENU > DISPLAY > COLOR's 11th value, CUSTOM (1.6, #244, Discussion #239): a palette of the user's, five RGB565 colours (BG
 * SURF TEXT THEME ACCENT, 16 bits little endian) in favorites.factory[15][0..9], flags in [12] (bit 0: one is stored, bit 1:
 * it is the palette in use), set from the editor (PALETTE_SET, editor_palette.c) and saved with the settings. settings.palette
 * stays a built-in palette (the one in use before, or after): firmware before 1.6 shows that and keeps the bytes. The value
 * is in COLOR only while one is stored. Zero (every older setting) = none */
#define ui_cpal (&favorites.factory[15][0])
#define ui_cpal_flags (favorites.factory[15][12])
#define CPAL_STORED 1u
#define CPAL_ACTIVE 2u
static uint32_t cpal_on(void) { return (ui_cpal_flags & (CPAL_STORED | CPAL_ACTIVE)) == (CPAL_STORED | CPAL_ACTIVE); }
static uint32_t palette_n(void) { return NPALETTES + ((ui_cpal_flags & CPAL_STORED) ? 1u : 0u); }     /* COLOR's values */
static uint32_t palette_cur(void) { return cpal_on() ? NPALETTES : settings.palette % NPALETTES; }
static const char *palette_name(uint32_t v) { return v == NPALETTES ? "CUSTOM" : UI_PALETTES[v % NPALETTES].name; }
/* the palette in use into gfx.c (the settings changed under it: a start, an import, a restore, a STYLE change) */
static void palette_apply(void)
{
    if (cpal_on()) {
        palette_custom_load(ui_cpal);
        palette_set(NPALETTES);
    } else {
        palette_set(settings.palette % NPALETTES);
    }
}

/* the five colours into the settings' bytes (stored; not yet the palette in use: palette_choose / palette_apply) */
static void cpal_store(const uint16_t *c)
{
    uint32_t i;
    for (i = 0; i < 5u; i++) {
        ui_cpal[2u * i] = (uint8_t)c[i];
        ui_cpal[2u * i + 1u] = (uint8_t)(c[i] >> 8);
    }
    ui_cpal_flags |= CPAL_STORED;
}
