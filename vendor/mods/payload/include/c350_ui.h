/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * c350_ui.h -- deck-side settings shared by the CDJ-350 payload mods
 * (mods/payload/common/c350_ui.c). A mod gets the code by naming 'c350_ui' in
 * PAYLOAD_COMMON in its cdj350/patch.py (mods/payloadbuild.py builds it once).
 *
 * Mod flags. The CDJ-350 user settings record is 24 bytes at 0x0C9BE004
 * (docs/cdj350.md, "UTILITY menu"); MAIN saves it whole with
 * settings_save(2, record) (0x0C084DE4), appending it to a log in flash
 * sector 0x3F8000, and at boot loads the last entry the scanner (0x0C085368)
 * accepts. The scanner stops at an entry with +0 bit 4, +1 bits 4..6 or +3
 * bits 4..7 set (its test for erased flash), so those bits are not free.
 * Every other bit is a stock field, except +0x14..+0x17: four bytes of error
 * detail that MAIN writes only when it logs disc error 0x12 or 0x15
 * (0x0C085584). The mods keep their switches in +0x17 as 0x50 | flags, the
 * flags being "off" bits; any other value (0 after the defaults routine
 * 0x0C085606 or a factory reset, or a disc error's detail) reads as no flag
 * set, so every mod switch is on.
 *
 * UTILITY items. Stock UTILITY (hold DISPLAY/UTILITY) is a list at level 2
 * (0x0C1CAF1A) of five items, cursor 0x0C1CAF1B: 0 A.CUE LEVEL, 1 MIDI CH,
 * 2 AUTO STANDBY, 3 LIBRARY CREATOR, 4 Ver. A mod registers a
 * c350_ui_utility_item with C350_UI_UTILITY_ITEM(item) (section
 * .c350_ui_items, gathered by mods/payload/payload.ld between c350_ui.c's
 * sentinels) and redirects the three UTILITY pool words to the shared
 * routines (the second mod finds them there):
 *   0x0C0C902C  utility_scroll (0x0C0D641A), read at 0x0C0C8EFE  -> c350_ui_utility_scroll
 *   0x0C0C9670  utility_set_level (0x0C0D6374), read at 0x0C0C955A -> c350_ui_utility_select
 *   0x0C0666A4  utility_draw_list (0x0C063C88), read at 0x0C0665E8 -> c350_ui_utility_draw
 * The visible items, in ascending `order`, become items 5, 6, ... The list
 * shows UTILITY over "<title> <value>" (13 characters, the value right
 * aligned); a push switches the value in place and the list stays at level 2.
 */
#ifndef C350_UI_H
#define C350_UI_H

#include "c350.h"

/* ---- mod flags (settings record +0x17) ---- */
#define C350_UI_SETTINGS         0x0C9BE004u
#define C350_UI_FLAGS_BYTE       C350_U8(C350_UI_SETTINGS + 0x17)
#define C350_UI_FLAGS_MAGIC      0x50u           /* high nibble of a valid flags byte */
#define C350_UI_FLAG_QUANT_OFF   0x01u           /* quantize off (default on) */
#define C350_UI_FLAG_DRIFT_OFF   0x02u           /* loop drift correction off (default on) */
#define C350_UI_FLAG_SORT_MASK   0x0Cu           /* browse: TRACK sort 0 default, 1 BPM, 2 key */
#define C350_UI_FLAG_SORT_SHIFT  2u
#define C350_UI_FLAGS_MASK       0x0Fu

int c350_ui_flag(u32 flag);                     /* 1 = set */
void c350_ui_set_flag(u32 flag, int on);        /* changes the bit and saves the record */
u32 c350_ui_field(u32 mask, u32 shift);         /* a multi-bit field of the flags */
void c350_ui_set_field(u32 mask, u32 shift, u32 value);  /* ... and saves the record */
int c350_ui_quantize(void);                     /* 1 = grid snapping on */
int c350_ui_drift(void);                        /* 1 = loop drift correction on */

/* ---- UTILITY items (after Ver) ---- */
#define C350_UI_UTILITY_FIRST    5u              /* Ver is stock item 4 */
#define C350_UI_ROW_CHARS        13u             /* visible characters of a row */

typedef struct {
    u32 order;                                  /* place among the mod items, ascending */
    const char *title;                          /* left part of row 2 */
    const char *(*value)(void);                 /* right part of row 2; NULL = item hidden */
    void (*select)(void);                       /* encoder push: next value */
} c350_ui_utility_item;

#define C350_UI_UTILITY_ITEM(it) \
    static const c350_ui_utility_item *const c350_ui_item_##it \
        __attribute__((used, section(".c350_ui_items"))) = &it

void c350_ui_utility_scroll(u32 direction, u32 steps);   /* pool 0x0C0C902C */
void c350_ui_utility_select(u32 level, int item);        /* pool 0x0C0C9670 */
void c350_ui_utility_draw(void);                         /* pool 0x0C0666A4 */

/* For tests: the row text an item draws (NUL-terminated, at most 13 chars). */
void c350_ui_utility_row(const c350_ui_utility_item *it, char *out);

#endif /* C350_UI_H */
