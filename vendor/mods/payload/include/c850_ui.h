/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * c850_ui.h -- deck-side controls shared by the CDJ-850 payload mods
 * (mods/payload/common/c850_ui.c). A mod gets the code by naming 'c850_ui' in
 * PAYLOAD_COMMON in its patch.py (mods/payloadbuild.py builds it once).
 *
 * Keys. One routine owns the pool word that panel_keys_to_internal_words
 * calls right after it rebuilt K0..K2 (0x0C0ED038, FUN_0C0DD492; docs
 * section 4b.1): c850_ui_keys. Every mod that wants a key or a combo
 * redirects that word to it (the second redirect finds it already there) and
 * defines its filter below; c850_ui_keys calls the filters that are linked
 * in. A mod registers its filter with C850_UI_KEY_FILTER(fn): a pointer in
 * the section .c850_ui_filters, which mods/payload/payload.ld gathers
 * between two sentinel words of c850_ui.c (sections .a / .z). A filter sees the
 * true held words k[0..2] and the new presses p[0..2], and ORs into
 * own[0..2] the bits it takes: they stay hidden from the stock handlers until
 * the key is released.
 *
 * BEAT SELECT combos. Holding BEAT SELECT and pressing a second key is the
 * mods' shift: c850_ui_shift() says whether `key` (K0 bit) was pressed with
 * BEAT SELECT held, takes both keys and undoes the fraction-mode toggle the
 * BEAT SELECT press made (stock toggles 0x0CE6D154/D156 on the press, in
 * player_keys_latch_prev). Used: RELOOP (hot_cues), LOOP IN (quant_loops).
 *
 * Mod flags. One byte of the settings record keeps the mods' on-deck
 * switches across restarts: +0x22 (0x0C9DA25E), the last byte of the HISTORY
 * name field; the record has no free flag bit (section 4b.6). Valid values
 * are 0x50..0x5F (a name change or factory reset clears the byte: every flag
 * then reads 0). 0x5A, what hot_cues wrote before this module, still means
 * hot-cue mode on, quantize on.
 *
 * Flash. c850_ui_flash(text) shows `text` on the dot matrix for
 * C850_UI_FLASH_MS (from playertask; the display hook c850_ui_flash_hook runs
 * in DisplayTask on the tail of display_stage_bpm and puts the rows back).
 */
#ifndef C850_UI_H
#define C850_UI_H

#include "c850.h"

/* ---- keys ---- */
extern u16 c850_ui_kt[3];                       /* K0..K2 as the remap built them (true held state) */
u32 c850_ui_keys(void);
int c850_ui_shift(const u16 *k, const u16 *p, u16 *own, u16 key_k0);
int c850_ui_shift_k1(const u16 *k, const u16 *p, u16 *own, u16 key_k1);

typedef void c850_ui_key_filter_fn(const u16 *k, const u16 *p, u16 *own);
#define C850_UI_KEY_FILTER(fn) \
    static c850_ui_key_filter_fn *const c850_ui_filter_##fn \
        __attribute__((used, section(".c850_ui_filters"))) = fn

/* ---- state tick hooks (pool 0x0C0EC838, FUN_0C0EB388) ---- */
void c850_ui_tick(void);

typedef void c850_ui_tick_fn(void);
#define C850_UI_TICK_HOOK(fn) \
    static c850_ui_tick_fn *const c850_ui_tick_##fn \
        __attribute__((used, section(".c850_ui_ticks"))) = fn

/* ---- mod flags (settings record +0x22) ---- */
#define C850_UI_FLAGS_BYTE      C850_U8(C850_SETTINGS + 0x22)
#define C850_UI_FLAGS_MAGIC     0x52            /* written with any flag set; 0 = none */
#define C850_UI_FLAG_QUANT_OFF  0x01            /* quantize off (default on) */
#define C850_UI_FLAG_HOT_CUES   0x08            /* hot-cue mode on */
#define C850_UI_FLAGS_MASK      (C850_UI_FLAG_QUANT_OFF | C850_UI_FLAG_HOT_CUES)

int c850_ui_flag(u32 flag);                     /* 1 = set */
int c850_ui_set_flag(u32 flag, int on);         /* 1 = changed and saved, 0 = no room (long name) */
int c850_ui_quantize(void);                     /* 1 = grid snapping on */

/* ---- display flash ---- */
#define C850_UI_FLASH_MS        1500
void c850_ui_flash(const char *text);
void c850_ui_flash_hook(u32 *regs, u32 *sp);

#endif /* C850_UI_H */
