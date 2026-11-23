/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * c850_ui.c -- the key filter chain, the mod flags byte and the display
 * flash shared by the CDJ-850 payload mods (see include/c850_ui.h).
 *
 * Built once into the payload when any built mod names 'c850_ui' in
 * PAYLOAD_COMMON (mods/payloadbuild.py). Users: hot_cues (BEAT SELECT +
 * RELOOP, hot-cue mode), quant_loops (BEAT SELECT + LOOP IN, quantize),
 * loop_resize (reads the quantize flag).
 */
#include "c850_ui.h"

#define UI_JOG              C850_FN(0x0C0DD492, u32, (void))   /* the word's stock function */
#define UI_TICK_STOCK       C850_FN(0x0C0EB388, void, (void))  /* pool 0x0C0EC838 stock fn */
#define UI_SETTINGS_SAVE    C850_FN(0x0C08E9B0, int, (int kind, u32 record))
#define UI_D156             C850_U8(0x0CE6D156)                 /* toggled with D154 */
#define UI_NAME             (C850_SETTINGS + 3)                 /* HISTORY name, 32 bytes */

/* ---- keys ---- */

u16 c850_ui_kt[3];                   /* K0..K2 as the remap built them (true held state) */
static u16 ui_own[3];                /* held bits hidden from the stock handlers */
static u8 ui_bs_ok, ui_bs_d154, ui_bs_d156;   /* D154/D156 before the BEAT SELECT press */

volatile u32 c850_ui_shifts;         /* evidence: BEAT SELECT combos taken */

/* The registered filters (C850_UI_KEY_FILTER) lie between these two words:
 * payload.ld places .c850_ui_filters.a, then every .c850_ui_filters, then
 * .c850_ui_filters.z. (Symbols the linker script defines, like weak
 * references, would leave a .rela section the payload link refuses.) */
c850_ui_key_filter_fn *const c850_ui_filters_start[1]
    __attribute__((used, section(".c850_ui_filters.a"))) = {0};
c850_ui_key_filter_fn *const c850_ui_filters_end[1]
    __attribute__((used, section(".c850_ui_filters.z"))) = {0};

/* Pool 0x0C0ED038 (FUN_0C0DD492), read only at 0x0C0ECEFC inside
 * panel_keys_to_internal_words, right after K0..K2 were rebuilt and only when
 * keys changed. Clearing bits here hides them from every handler. */
u32 c850_ui_keys(void)
{
    u32 r = UI_JOG();
    u16 k[3], p[3];
    u32 f, end;
    int i;

    k[0] = C850_K0;
    k[1] = C850_K1;
    k[2] = C850_K2;
    for (i = 0; i < 3; i++) {
        p[i] = k[i] & ~c850_ui_kt[i];
        c850_ui_kt[i] = k[i];
        ui_own[i] &= k[i];            /* a released key is free again */
    }
    if (p[0] & C850_K0_BEAT_SELECT) {
        /* stock toggles D154/D156 on this press (player_keys_latch_prev) */
        ui_bs_d154 = C850_BEAT_FRACTION_MODE;
        ui_bs_d156 = UI_D156;
        ui_bs_ok = 1;
    }
    if (!(k[0] & C850_K0_BEAT_SELECT))
        ui_bs_ok = 0;
    if (C850_UI_LOCK == 0) {
        /* addresses, not pointers: the table spans two C objects */
        end = (u32)c850_ui_filters_end;
        for (f = (u32)(c850_ui_filters_start + 1); f < end; f += 4)
            (*(c850_ui_key_filter_fn *const *)f)(k, p, ui_own);
    }
    C850_K0 = k[0] & ~ui_own[0];
    C850_K1 = k[1] & ~ui_own[1];
    C850_K2 = k[2] & ~ui_own[2];
    return r;
}

int c850_ui_shift(const u16 *k, const u16 *p, u16 *own, u16 key_k0)
{
    if (!(p[0] & key_k0) || !(k[0] & C850_K0_BEAT_SELECT))
        return 0;
    if (ui_bs_ok) {                  /* undo the fraction toggle of this BEAT SELECT */
        C850_BEAT_FRACTION_MODE = ui_bs_d154;
        UI_D156 = ui_bs_d156;
    }
    own[0] |= key_k0 | C850_K0_BEAT_SELECT;
    c850_ui_shifts++;
    return 1;
}

int c850_ui_shift_k1(const u16 *k, const u16 *p, u16 *own, u16 key_k1)
{
    if (!(p[1] & key_k1) || !(k[0] & C850_K0_BEAT_SELECT))
        return 0;
    if (ui_bs_ok) {                  /* undo the fraction toggle of this BEAT SELECT */
        C850_BEAT_FRACTION_MODE = ui_bs_d154;
        UI_D156 = ui_bs_d156;
    }
    own[0] |= C850_K0_BEAT_SELECT;
    own[1] |= key_k1;
    c850_ui_shifts++;
    return 1;
}

/* ---- state tick hooks ---- */

c850_ui_tick_fn *const c850_ui_ticks_start[1]
    __attribute__((used, section(".c850_ui_ticks.a"))) = {0};
c850_ui_tick_fn *const c850_ui_ticks_end[1]
    __attribute__((used, section(".c850_ui_ticks.z"))) = {0};

/* Pool 0x0C0EC838 (FUN_0C0EB388): first call of FUN_0C0EC73C (state tick).
 * Calls the stock function, then every registered tick hook. */
void c850_ui_tick(void)
{
    u32 f, end = (u32)c850_ui_ticks_end;

    UI_TICK_STOCK();
    for (f = (u32)(c850_ui_ticks_start + 1); f < end; f += 4)
        (*(c850_ui_tick_fn *const *)f)();
}

/* ---- mod flags ---- */

/* The HISTORY name ends before the flags byte (a 31-character name holds it). */
static int ui_name_short(void)
{
    u32 k;

    for (k = 0; k < 0x1F; k++)
        if (C850_U8(UI_NAME + k) == 0)
            return 1;
    return 0;
}

static u32 ui_flags(void)
{
    u32 b = C850_UI_FLAGS_BYTE;

    if ((b & 0xF0) != 0x50 || !ui_name_short())
        return 0;
    return b & C850_UI_FLAGS_MASK;
}

int c850_ui_flag(u32 flag)
{
    return (ui_flags() & flag) != 0;
}

int c850_ui_set_flag(u32 flag, int on)
{
    u32 v;

    if (!ui_name_short())
        return 0;
    v = ui_flags();
    v = on ? (v | flag) : (v & ~flag);
    C850_UI_FLAGS_BYTE = v ? (u8)(C850_UI_FLAGS_MAGIC | v) : 0;
    UI_SETTINGS_SAVE(2, C850_SETTINGS);
    return 1;
}

int c850_ui_quantize(void)
{
    return !c850_ui_flag(C850_UI_FLAG_QUANT_OFF);
}

/* ---- display flash ----
 * Rows (section 4b.7): three records [tag, attr0, attr1, attr2, len, text]
 * at 0x0C9D0B18 + r*0x105, serialised for the panel by FUN_0C064CA4 after
 * display_stage_bpm in every DisplayTask pass; a row is resent only when a
 * byte changed. The loaded-track view (mode 10) shows row 0 as the inverted
 * title line, row 1 the artist and the overview waveform where row 2 would
 * be: the message takes the title line. Other views (browser, INFO, tag
 * list) show three text lines: the message is the middle one alone, as the
 * stock single-message screens are. The tag and attribute bytes stay as the
 * view set them. While the flash lasts, a row the view rewrites is noted
 * again (that is what comes back); afterwards each row the flash still holds
 * gets its noted text back. */
#define UI_DISPLAY_MODE     C850_U32(0x0C9D0AF8)
#define UI_MODE_LOADED      10
#define UI_ROWS             3
#define UI_TEXT_MAX         40

static const char *volatile fl_text;
static volatile u32 fl_req;
static u32 fl_seen, fl_until;
static u8 fl_msg[UI_TEXT_MAX];       /* the message, copied when the request is seen */
static u8 fl_mlen, fl_active;
static u8 fl_wrote[UI_ROWS];         /* 1: the row holds what we wrote */
static u8 fl_wlen[UI_ROWS];
static u8 fl_wtext[UI_ROWS][UI_TEXT_MAX];   /* what we wrote (a copy: the message may change) */
static u8 fl_len[UI_ROWS];           /* the row as the view had it */
static u8 fl_save[UI_ROWS][255];
static const u8 fl_empty[1];

volatile u32 c850_ui_flashes;        /* evidence: flashes started */
volatile u32 c850_ui_flash_restores; /* rows given back */

void c850_ui_flash(const char *text)
{
    fl_text = text;
    fl_req++;
}

static int ui_row_is(volatile u8 *row, const u8 *t, u32 len)
{
    u32 i;

    if (row[4] != len)
        return 0;
    for (i = 0; i < len; i++)
        if (row[5 + i] != t[i])
            return 0;
    return 1;
}

static void ui_row_set(volatile u8 *row, const u8 *t, u32 len)
{
    u32 i;

    for (i = 0; i < len; i++)
        row[5 + i] = t[i];
    row[4] = (u8)len;
}

static void ui_unflash(int r)
{
    volatile u8 *row = (volatile u8 *)C850_MATRIX_ROW(r);

    if (!fl_wrote[r])
        return;
    if (ui_row_is(row, fl_wtext[r], fl_wlen[r])) {
        ui_row_set(row, fl_save[r], fl_len[r]);
        c850_ui_flash_restores++;
    }
    fl_wrote[r] = 0;
}

static void ui_show(int r, const u8 *t, u32 len)
{
    volatile u8 *row = (volatile u8 *)C850_MATRIX_ROW(r);
    u32 i;

    if (!fl_wrote[r] || !ui_row_is(row, fl_wtext[r], fl_wlen[r])) {
        fl_len[r] = row[4];          /* what the view has there now */
        for (i = 0; i < fl_len[r]; i++)
            fl_save[r][i] = row[5 + i];
    }
    if (!ui_row_is(row, t, len))
        ui_row_set(row, t, len);
    fl_wrote[r] = 1;
    for (i = 0; i < len; i++)
        fl_wtext[r][i] = t[i];
    fl_wlen[r] = (u8)len;
}

/* Hook on the tail of display_stage_bpm (span 0x0C0642AA..0x0C0642B8, the
 * shared trampoline of bpm999/bpm_decimals), after bpm_decimals' hook. */
void c850_ui_flash_hook(u32 *regs, u32 *sp)
{
    u32 now = C850_TICK_MS, req = fl_req;
    int r;

    (void)regs;
    (void)sp;
    if (req != fl_seen) {
        const u8 *t = (const u8 *)fl_text;
        u32 n = 0;

        fl_seen = req;
        if (t) {
            while (n < UI_TEXT_MAX && t[n]) {
                fl_msg[n] = t[n];
                n++;
            }
            fl_mlen = (u8)n;
            fl_until = now + C850_UI_FLASH_MS;
            fl_active = 1;
            c850_ui_flashes++;
        }
    }
    if (!fl_active)
        return;
    if ((s32)(now - fl_until) >= 0) {
        for (r = 0; r < UI_ROWS; r++)
            ui_unflash(r);
        fl_active = 0;
        return;
    }
    if (UI_DISPLAY_MODE == UI_MODE_LOADED) {
        ui_show(0, fl_msg, fl_mlen);
        ui_unflash(1);
        ui_unflash(2);
    } else {
        ui_show(0, fl_empty, 0);
        ui_show(1, fl_msg, fl_mlen);
        ui_show(2, fl_empty, 0);
    }
}
