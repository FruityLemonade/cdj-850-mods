/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * c350_ui.c -- the mod flags and the UTILITY items after Ver shared by the
 * CDJ-350 payload mods (see include/c350_ui.h).
 *
 * Built once into the payload when any built mod names 'c350_ui' in
 * PAYLOAD_COMMON (mods/payloadbuild.py). Users: quant_loops (QUANTIZE,
 * DRIFT FIX), browse (SORT).
 */
#include "c350_ui.h"

#define UI_SETTINGS_SAVE    C350_FN(0x0C084DE4, int, (int kind, u32 record))

/* ---- mod flags ---- */

static u32 ui_flags(void)
{
    u32 b = C350_UI_FLAGS_BYTE;

    if ((b & 0xF0u) != C350_UI_FLAGS_MAGIC)
        return 0;
    return b & C350_UI_FLAGS_MASK;
}

int c350_ui_flag(u32 flag)
{
    return (ui_flags() & flag) != 0;
}

void c350_ui_set_flag(u32 flag, int on)
{
    u32 f = ui_flags();

    flag &= C350_UI_FLAGS_MASK;
    f = on ? (f | flag) : (f & ~flag);
    C350_UI_FLAGS_BYTE = f ? (u8)(C350_UI_FLAGS_MAGIC | f) : 0;
    UI_SETTINGS_SAVE(2, C350_UI_SETTINGS);
}

u32 c350_ui_field(u32 mask, u32 shift)
{
    return (ui_flags() & mask) >> shift;
}

void c350_ui_set_field(u32 mask, u32 shift, u32 value)
{
    u32 f = (ui_flags() & ~mask) | ((value << shift) & mask & C350_UI_FLAGS_MASK);

    C350_UI_FLAGS_BYTE = f ? (u8)(C350_UI_FLAGS_MAGIC | f) : 0;
    UI_SETTINGS_SAVE(2, C350_UI_SETTINGS);
}

int c350_ui_quantize(void)
{
    return !c350_ui_flag(C350_UI_FLAG_QUANT_OFF);
}

int c350_ui_drift(void)
{
    return !c350_ui_flag(C350_UI_FLAG_DRIFT_OFF);
}

/* ---- UTILITY items ---- */

#define UI_UTIL_LEVEL       C350_U8(0x0C1CAF1A)
#define UI_UTIL_CURSOR      C350_U8(0x0C1CAF1B)
#define UI_UTIL_TIMEOUT     C350_U32(0x0C1CAF04)
#define UI_UTIL_SCROLL      C350_FN(0x0C0D641A, u32, (u32, u32))  /* utility_scroll */
#define UI_UTIL_SET_LEVEL   C350_FN(0x0C0D6374, void, (u32, int)) /* utility_set_level */
#define UI_UTIL_DRAW_LIST   C350_FN(0x0C063C88, void, (void))     /* utility_draw_list */
#define UI_UTIL_REDRAW      C350_FN(0x0C06777A, u32, (int))       /* display_request */
#define UI_TEXT_COPY        C350_FN(0x0C0679CC, u32, (u32, int))  /* text rows -> slot 7 */
#define UI_TEXT_MODE        C350_U8(0x0C9B816F)
#define UI_TEXT_ROW1        ((volatile u8 *)0x0C9B87F3)
#define UI_TEXT_ROW2        ((volatile u8 *)0x0C9B88F3)
#define UI_UTIL_MAX         8u
#define UI_TIMEOUT_MS       15000u

const c350_ui_utility_item *const c350_ui_items_start[1]
    __attribute__((used, section(".c350_ui_items.a"))) = {0};
const c350_ui_utility_item *const c350_ui_items_end[1]
    __attribute__((used, section(".c350_ui_items.z"))) = {0};

volatile u32 c350_ui_utility_selects;  /* evidence: pushes on a mod item */
volatile u32 c350_ui_utility_draws;    /* evidence: mod rows drawn */

/* The visible items in ascending order; returns how many. */
static u32 ui_util_items(const c350_ui_utility_item **out)
{
    u32 f, end = (u32)c350_ui_items_end, n = 0, i;
    const c350_ui_utility_item *it;

    for (f = (u32)(c350_ui_items_start + 1); f < end && n < UI_UTIL_MAX; f += 4) {
        it = *(const c350_ui_utility_item *const *)f;
        if (it->value() == 0)
            continue;
        for (i = n; i > 0 && out[i - 1]->order > it->order; i--)
            out[i] = out[i - 1];
        out[i] = it;
        n++;
    }
    return n;
}

static const c350_ui_utility_item *ui_util_item(u32 cursor)
{
    const c350_ui_utility_item *items[UI_UTIL_MAX];
    u32 n = ui_util_items(items);

    if (cursor < C350_UI_UTILITY_FIRST || cursor - C350_UI_UTILITY_FIRST >= n)
        return 0;
    return items[cursor - C350_UI_UTILITY_FIRST];
}

/* Pool 0x0C0C902C (read at 0x0C0C8EFE): rotary at UTILITY level 2,
 * direction 1 = clockwise. Stock stops at Ver (item 4); the mod items
 * continue the list. */
void c350_ui_utility_scroll(u32 direction, u32 steps)
{
    const c350_ui_utility_item *items[UI_UTIL_MAX];
    s32 item = (s32)UI_UTIL_CURSOR;
    s32 delta = (s32)(steps & 0xFFu);
    s32 next = (direction & 0xFFu) == 1u ? item + delta : item - delta;
    s32 last = (s32)C350_UI_UTILITY_FIRST - 1 + (s32)ui_util_items(items);

    if (item < (s32)C350_UI_UTILITY_FIRST && next < (s32)C350_UI_UTILITY_FIRST) {
        UI_UTIL_SCROLL(direction, steps);
        return;
    }
    if (next < 0)
        next = 0;
    if (next > last)
        next = last;
    UI_UTIL_TIMEOUT = UI_TIMEOUT_MS;
    if (next != item) {
        UI_UTIL_CURSOR = (u8)next;
        UI_UTIL_REDRAW(1);
    }
}

/* Pool 0x0C0C9670 (read at 0x0C0C955A): encoder push in the UTILITY list,
 * stock utility_set_level(3, item). A mod item switches its value in place. */
void c350_ui_utility_select(u32 level, int item)
{
    const c350_ui_utility_item *it = 0;

    if ((level & 0xFFu) == 3u && item >= (int)C350_UI_UTILITY_FIRST)
        it = ui_util_item((u32)item);
    if (it == 0) {
        UI_UTIL_SET_LEVEL(level, item);
        return;
    }
    it->select();
    c350_ui_utility_selects++;
    UI_UTIL_TIMEOUT = UI_TIMEOUT_MS;
    UI_UTIL_REDRAW(1);
}

static u32 ui_strlen(const char *s)
{
    u32 n = 0;

    while (s[n] != '\0')
        n++;
    return n;
}

/* "<title> <value>" in 13 characters, the value right aligned; a long title
 * is cut to leave one space before the value. */
void c350_ui_utility_row(const c350_ui_utility_item *it, char *out)
{
    const char *v = it->value();
    u32 vl = ui_strlen(v), tl = ui_strlen(it->title), k, room;

    if (vl > C350_UI_ROW_CHARS)
        vl = C350_UI_ROW_CHARS;
    room = C350_UI_ROW_CHARS - vl;
    if (room > 0u)
        room--;                     /* the space before the value */
    if (tl > room)
        tl = room;
    for (k = 0; k < C350_UI_ROW_CHARS; k++)
        out[k] = ' ';
    for (k = 0; k < tl; k++)
        out[k] = it->title[k];
    for (k = 0; k < vl; k++)
        out[C350_UI_ROW_CHARS - vl + k] = v[k];
    out[C350_UI_ROW_CHARS] = '\0';
}

static void ui_text(volatile u8 *row, const char *t)
{
    u32 k = 0;

    while (t[k] != '\0') {
        row[k] = (u8)t[k];
        k++;
    }
    row[k] = 0;
}

/* Pool 0x0C0666A4 (read at 0x0C0665E8 in utility_display, level 2): draws
 * the list row. On a mod item, the same steps as utility_draw_list with the
 * item's row; otherwise stock. */
void c350_ui_utility_draw(void)
{
    const c350_ui_utility_item *it = ui_util_item(UI_UTIL_CURSOR);
    char row[C350_UI_ROW_CHARS + 1];

    if (it == 0) {
        UI_UTIL_DRAW_LIST();
        return;
    }
    c350_ui_utility_row(it, row);
    UI_TEXT_MODE = 3;
    ui_text(UI_TEXT_ROW1, "UTILITY");
    ui_text(UI_TEXT_ROW2, row);
    c350_ui_utility_draws++;
    UI_TEXT_COPY(3, 2);
}
