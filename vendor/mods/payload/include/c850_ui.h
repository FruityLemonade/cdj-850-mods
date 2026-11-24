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
 * player_keys_latch_prev). Used: RELOOP (hot_cues), LOOP IN (quant_loops), LOOP OUT (loop_roll).
 *
 * Mod flags. One byte of the settings record keeps the mods' on-deck
 * switches across restarts: +0x22 (0x0C9DA25E), the last byte of the HISTORY
 * name field; the record has no free flag bit (section 4b.6). Valid values
 * are 0x50..0x5F (a name change or factory reset clears the byte: every flag
 * then reads 0). 0x5A, what hot_cues wrote before this module, still means
 * hot-cue mode on, quantize on.
 *
 * UTILITY items. Mods add settings after VERSION No. (stock item 6) in the
 * UTILITY list (menu level 2, 0x0C1E108C; item 0x0C1E108D). A mod registers
 * a c850_ui_utility_item with C850_UI_UTILITY_ITEM(item) (section
 * .c850_ui_items, gathered like the key filters) and redirects the rotary
 * pool 0x0C0E797C to c850_ui_utility_scroll and the encoder-press pool
 * 0x0C0E7F10 to c850_ui_utility_select (the second redirect finds them
 * there). The visible items, in ascending `order`, become items 7, 8, ...;
 * c850_ui_utility_draw (called from the display hooks) writes the title and
 * the value on rows 1 and 2. Stock FUN_0C067B8C draws nothing for items > 6.
 *
 * Settings file on the stick (prototype). When a USB stick mounts
 * (c850_ui_usb_mount on pool 0x0C0C11E8, after the stock ANLZ manager set-up
 * FUN_0C0C45B4) the mod flags are read from <drive>:/PIONEER/MODSET.DAT; from
 * then on they come from that file and every c850_ui_set_flags also rewrites
 * it, so the settings travel with the stick (and need no room in the
 * HISTORY name). A stick without the file keeps the flags of the flash
 * record until the first change, which creates the file. File: 16 bytes,
 * "C850MODS", version 1, length 4, two zero bytes, the flags word (u32 BE).
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

/* ---- mod flags (settings record +0x22, +0x21) ---- */
#define C850_UI_FLAGS_BYTE       C850_U8(C850_SETTINGS + 0x22)
#define C850_UI_FLAGS2_BYTE      C850_U8(C850_SETTINGS + 0x21)
#define C850_UI_FLAGS_MAGIC      0x52            /* written with any flag set; 0 = none */
#define C850_UI_FLAGS2_MAGIC     0x50            /* written with any ext flag set; 0 = none */
#define C850_UI_FLAG_QUANT_OFF   0x01            /* quantize off (default on) */
#define C850_UI_FLAG_HOT_CUES    0x08            /* hot-cue mode on */
#define C850_UI_FLAG_WAVE_LEFT   0x04
#define C850_UI_FLAGS_MASK       (C850_UI_FLAG_QUANT_OFF | C850_UI_FLAG_HOT_CUES | C850_UI_FLAG_WAVE_LEFT)
#define C850_UI_FLAG_WAVE_CENTER 0x10            /* stored in +0x21 bit 0 */
#define C850_UI_FLAG_DRIFT_OFF   0x20            /* stored in +0x21 bit 1: loop drift correction off (default on) */
#define C850_UI_FLAGS2_MASK      0x03

/* Additional flags persisted in PIONEER/MODSET.DAT (and RAM when no stick is mounted).
 * Default 0 = mod enabled; bits 14..15 store the Auto Standby timeout index (0..3). */
#define C850_UI_FLAG_BPM_DECIMALS_OFF 0x0040u
#define C850_UI_FLAG_BPM_LOCK_OFF     0x0080u
#define C850_UI_FLAG_PHASE_METER_OFF  0x0100u
#define C850_UI_FLAG_INFO_ORDER_OFF   0x0200u
#define C850_UI_FLAG_BROWSE_OFF       0x0400u
#define C850_UI_FLAG_HOT_CUES_OFF     0x0800u
#define C850_UI_FLAG_WAVEFORM_OFF     0x1000u
#define C850_UI_FLAG_LOOP_ROLL_OFF    0x2000u
#define C850_UI_FLAG_STANDBY_SHIFT    14u
#define C850_UI_FLAG_STANDBY_MASK     0xC000u    /* 0 = 20m, 1 = 40m, 2 = 60m, 3 = 120m */

int c850_ui_flag(u32 flag);                     /* 1 = set */
int c850_ui_set_flag(u32 flag, int on);         /* 1 = changed and saved, 0 = no room / no stick */
int c850_ui_set_flags(u32 mask, u32 value);     /* atomic multi-flag update + single save */
int c850_ui_quantize(void);                     /* 1 = grid snapping on */
u32 c850_ui_standby_index(void);                /* 0..3 for 20/40/60/120 min */

/* ---- settings file on the USB stick (pool 0x0C0C11E8) ---- */
#define C850_UI_USB_MAGIC        "C850MODS"
#define C850_UI_USB_VERSION      1
#define C850_UI_USB_SIZE         16
int c850_ui_usb_mount(void *anlz, u32 arg);    /* stock FUN_0C0C45B4, then load the file */

/* ---- UTILITY items (after VERSION No.) ---- */
#define C850_UI_UTILITY_FIRST    7u              /* VERSION No. is stock item 6 */

typedef struct {
    u32 order;                                  /* place among the mod items, ascending */
    const char *title;                          /* row 1 */
    const char *(*value)(void);                 /* row 2; NULL = item hidden (mod off) */
    void (*select)(void);                       /* encoder press: next value */
} c850_ui_utility_item;

#define C850_UI_UTILITY_ITEM(it) \
    static const c850_ui_utility_item *const c850_ui_item_##it \
        __attribute__((used, section(".c850_ui_items"))) = &it

void c850_ui_utility_scroll(s8 direction, u8 steps);  /* pool 0x0C0E797C */
void c850_ui_utility_select(u32 level, int item);     /* pool 0x0C0E7F10 */
void c850_ui_utility_draw(void);                       /* from a DisplayTask hook */

/* ---- display flash ---- */
#define C850_UI_FLASH_MS        1500
void c850_ui_flash(const char *text);
void c850_ui_flash_hook(u32 *regs, u32 *sp);

#endif /* C850_UI_H */
