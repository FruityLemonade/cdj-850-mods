/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * c850_ui.c -- the key filter chain, the mod flags byte and the display
 * flash shared by the CDJ-850 payload mods (see include/c850_ui.h).
 *
 * Built once into the payload when any built mod names 'c850_ui' in
 * PAYLOAD_COMMON (mods/payloadbuild.py). Users: hot_cues (BEAT SELECT +
 * RELOOP, hot-cue mode), quant_loops (BEAT SELECT + LOOP IN, quantize),
 * loop_resize (reads the quantize flag), loop_roll (BEAT SELECT + LOOP OUT,
 * roll mode, kept in RAM: the flags byte has no free bit); the UTILITY items
 * after VERSION No. (waveform: WAVE PLAYHEAD, quant_loops: QUANTIZE).
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

/* The HISTORY name ends before the flag byte(s) (limit 0x1F for +0x22, 0x1E for +0x21). */
static int ui_name_short(u32 limit)
{
    u32 k;

    for (k = 0; k < limit; k++)
        if (C850_U8(UI_NAME + k) == 0)
            return 1;
    return 0;
}

static u32 ui_flags1(void)
{
    u32 b = C850_UI_FLAGS_BYTE;

    if ((b & 0xF0) != 0x50 || !ui_name_short(0x1F))
        return 0;
    return b & C850_UI_FLAGS_MASK;
}

static u32 ui_flags2(void)
{
    u32 b = C850_UI_FLAGS2_BYTE;

    if ((b & 0xF0) != 0x50 || !ui_name_short(0x1E))
        return 0;
    return b & C850_UI_FLAGS2_MASK;
}

/* ---- settings file on the stick ---- */

#define UI_ANLZ_MOUNT       C850_FN(0x0C0C45B4, int, (void *, u32))  /* stock word of pool 0x0C0C11E8 */
#define UI_FOPEN            C850_FN(0x0C088F74, void *, (const u16 *, u32, const u16 *, const char *))
#define UI_FREAD            C850_FN(0x0C0890E6, int, (void *, u32, void *))
#define UI_FWRITE           C850_FN(0x0C08910C, int, (const void *, u32, void *))
#define UI_FCLOSE           C850_FN(0x0C088FCE, int, (void *))

static const u16 ui_usb_path[] = {
    '/', 'P', 'I', 'O', 'N', 'E', 'E', 'R', '/', 'M', 'O', 'D', 'S', 'E', 'T', '.', 'D', 'A', 'T', 0,
};
static u16 ui_usb_drive;             /* drive letter of the mounted stick; 0 = none seen */
static u8 ui_usb_have;               /* 1: the flags below are the stick's */
static u32 ui_usb_flags;
static u32 ui_ram_flags;             /* RAM backing for bits 6..15 when no stick is mounted */

volatile u32 c850_ui_usb_loads;      /* evidence: files read at mount */
volatile u32 c850_ui_usb_saves;      /* files written */
volatile u32 c850_ui_usb_errors;     /* opens or writes that failed, bad files */

static const char ui_usb_magic[8] = C850_UI_USB_MAGIC;
#define UI_FLAGS_ALL        (C850_UI_FLAGS_MASK | (C850_UI_FLAGS2_MASK << 4) | 0xFFC0u | C850_UI_FLAG_QVALUE_MASK)

static void ui_usb_load(void)
{
    u8 buf[C850_UI_USB_SIZE];
    void *f;
    int n, i;

    ui_usb_have = 0;
    f = UI_FOPEN(&ui_usb_drive, 0, ui_usb_path, "r");
    if (f == 0)
        return;                      /* no file yet: the flash flags stay */
    n = UI_FREAD(buf, sizeof buf, f);
    UI_FCLOSE(f);
    if (n != (int)sizeof buf || buf[8] != C850_UI_USB_VERSION || buf[9] < 4u) {
        c850_ui_usb_errors++;
        return;
    }
    for (i = 0; i < 8; i++)
        if (buf[i] != (u8)ui_usb_magic[i]) {
            c850_ui_usb_errors++;
            return;
        }
    ui_usb_flags = ((u32)buf[12] << 24 | (u32)buf[13] << 16 | (u32)buf[14] << 8 | buf[15]) & UI_FLAGS_ALL;
    ui_ram_flags = ui_usb_flags;
    ui_usb_have = 1;
    c850_ui_usb_loads++;
}

static int ui_usb_save(u32 flags)
{
    u8 buf[C850_UI_USB_SIZE];
    void *f;
    int i, n;

    if (ui_usb_drive == 0)
        return 0;
    for (i = 0; i < 8; i++)
        buf[i] = (u8)ui_usb_magic[i];
    buf[8] = C850_UI_USB_VERSION;
    buf[9] = 4;
    buf[10] = buf[11] = 0;
    buf[12] = (u8)(flags >> 24);
    buf[13] = (u8)(flags >> 16);
    buf[14] = (u8)(flags >> 8);
    buf[15] = (u8)flags;
    f = UI_FOPEN(&ui_usb_drive, 0, ui_usb_path, "w+");
    if (f == 0) {
        c850_ui_usb_errors++;
        return 0;
    }
    n = UI_FWRITE(buf, sizeof buf, f);
    UI_FCLOSE(f);
    if (n != (int)sizeof buf) {
        c850_ui_usb_errors++;
        return 0;
    }
    c850_ui_usb_saves++;
    return 1;
}

/* Pool 0x0C0C11E8 (read only at 0x0C0C1010 in FUN_0C0C0F88): the stock USB
 * ANLZ manager set-up of a newly mounted stick, which opens (and creates)
 * <drive>:/PIONEER/USBANLZ/USBMNG.DAT. The drive letter is the first
 * halfword of its drive record (ctx + 0xC, as anlz_find_or_create uses it). */
int c850_ui_usb_mount(void *anlz, u32 arg)
{
    int r = UI_ANLZ_MOUNT(anlz, arg);

    if (anlz != 0 && ((u32)anlz & 3u) == 0) {
        ui_usb_drive = *(const u16 *)((const u8 *)anlz + 0xC);
        ui_usb_load();
    }
    return r;
}

static u32 ui_flags(void)
{
    if (ui_usb_have)
        return ui_usb_flags;
    return (ui_ram_flags & ~0x3Fu) | ui_flags1() | (ui_flags2() << 4);
}

int c850_ui_flag(u32 flag)
{
    if (ui_usb_have)
        return (ui_usb_flags & flag) != 0;
    if ((flag & ~0x3Fu) == flag)
        return (ui_ram_flags & flag) != 0;
    return (ui_flags() & flag) != 0;
}

int c850_ui_set_flags(u32 mask, u32 value)
{
    u32 m1 = mask & C850_UI_FLAGS_MASK;
    u32 m2 = (mask >> 4) & C850_UI_FLAGS2_MASK;
    u32 all;
    u32 v1, v2;
    int saved = 0;

    if (!m1 && !m2 && ui_usb_drive == 0) {
        ui_ram_flags = (ui_ram_flags & ~mask) | (value & mask & ~0x3Fu);
        if (ui_usb_have)
            ui_usb_flags = (ui_usb_flags & ~(mask & ~0x3Fu)) | (value & mask & ~0x3Fu);
        return 0;
    }
    all = (ui_flags() & ~mask) | (value & mask & UI_FLAGS_ALL);
    ui_ram_flags = all;
    /* the stick first: it has room whatever the HISTORY name */
    if (ui_usb_save(all)) {
        ui_usb_flags = all;
        ui_usb_have = 1;
        saved = 1;
    } else if (ui_usb_have) {
        /* The stick refused the write, but c850_ui_flag still reads the
         * stick's copy: keep every bit of the change there (not only bits
         * 6+), or quantize and the playhead stay at the file's value. */
        ui_usb_flags = all;
    }
    if (!m1 && !m2)
        return saved;
    if ((m2 && !ui_name_short(0x1E)) || (m1 && !ui_name_short(0x1F)))
        return saved;
    if (m1) {
        v1 = (ui_flags1() & ~m1) | (value & m1);
        C850_UI_FLAGS_BYTE = v1 ? (u8)(C850_UI_FLAGS_MAGIC | v1) : 0;
    }
    if (m2) {
        v2 = (ui_flags2() & ~m2) | ((value >> 4) & m2);
        C850_UI_FLAGS2_BYTE = v2 ? (u8)(C850_UI_FLAGS2_MAGIC | v2) : 0;
    }
    UI_SETTINGS_SAVE(2, C850_SETTINGS);
    return 1;
}

int c850_ui_set_flag(u32 flag, int on)
{
    return c850_ui_set_flags(flag, on ? flag : 0u);
}

/* RAM-only during-performance arming state (never persisted to flash/USB). */
static u8 ui_quantize_armed = 1u;
static u8 ui_hot_cues_armed;

int c850_ui_quantize(void)
{
    return !c850_ui_flag(C850_UI_FLAG_QUANT_OFF) && ui_quantize_armed != 0u;
}

int c850_ui_quantize_armed(void)
{
    return ui_quantize_armed != 0u;
}

void c850_ui_set_quantize_armed(int on)
{
    ui_quantize_armed = on ? 1u : 0u;
}

int c850_ui_hot_cues_active(void)
{
    return !c850_ui_flag(C850_UI_FLAG_HOT_CUES_OFF) && ui_hot_cues_armed != 0u;
}

int c850_ui_hot_cues_armed(void)
{
    return ui_hot_cues_armed != 0u;
}

void c850_ui_set_hot_cues_armed(int on)
{
    ui_hot_cues_armed = on ? 1u : 0u;
}

u32 c850_ui_standby_index(void)
{
    u32 f = ui_usb_have ? ui_usb_flags : ui_ram_flags;
    return (f & C850_UI_FLAG_STANDBY_MASK) >> C850_UI_FLAG_STANDBY_SHIFT;
}

u32 c850_ui_quant_div(void)
{
    u32 f = ui_usb_have ? ui_usb_flags : ui_ram_flags;
    u32 v = (f & C850_UI_FLAG_QVALUE_MASK) >> C850_UI_FLAG_QVALUE_SHIFT;

    return v == 2u ? 4u : v == 1u ? 2u : 1u;    /* 3 (unused) reads as 1 beat */
}

/* ---- UTILITY items ---- */

#define UI_UTIL_MODE        12u
#define UI_UTIL_LEVEL       C850_U8(0x0C1E108C)
#define UI_UTIL_CURSOR      C850_U8(0x0C1E108D)
#define UI_UTIL_TIMEOUT     C850_U32(0x0C1E1078)
#define UI_UTIL_SCROLL      C850_FN(0x0C0E4DF6, void, (s8, u8))   /* utility_scroll_item */
#define UI_UTIL_SET_LEVEL   C850_FN(0x0C0E4C14, void, (u32, int)) /* utility_set_menu_level */
#define UI_UTIL_REDRAW      C850_FN(0x0C06F71C, void, (int))
#define UI_UTIL_MAX         24u

const c850_ui_utility_item *const c850_ui_items_start[1]
    __attribute__((used, section(".c850_ui_items.a"))) = {0};
const c850_ui_utility_item *const c850_ui_items_end[1]
    __attribute__((used, section(".c850_ui_items.z"))) = {0};

volatile u32 c850_ui_utility_selects;  /* evidence: presses on a mod item */

/* How many mod items are currently visible (up to UI_UTIL_MAX). */
static u32 ui_util_count(void)
{
    u32 f, end = (u32)c850_ui_items_end, n = 0;

    for (f = (u32)(c850_ui_items_start + 1); f < end && n < UI_UTIL_MAX; f += 4) {
        const c850_ui_utility_item *it = *(const c850_ui_utility_item *const *)f;
        if (it->value() != 0)
            n++;
    }
    return n;
}

static const c850_ui_utility_item *ui_util_item(u32 cursor)
{
    u32 f, g, end = (u32)c850_ui_items_end, target;

    if (cursor < C850_UI_UTILITY_FIRST)
        return 0;
    target = cursor - C850_UI_UTILITY_FIRST;
    if (target >= UI_UTIL_MAX)
        return 0;
    for (f = (u32)(c850_ui_items_start + 1); f < end; f += 4) {
        const c850_ui_utility_item *it = *(const c850_ui_utility_item *const *)f;
        u32 rank = 0;

        if (it->value() == 0)
            continue;
        for (g = (u32)(c850_ui_items_start + 1); g < end; g += 4) {
            const c850_ui_utility_item *other = *(const c850_ui_utility_item *const *)g;
            if (other->value() == 0)
                continue;
            if (other->order < it->order || (other->order == it->order && g < f))
                rank++;
        }
        if (rank == target)
            return it;
    }
    return 0;
}

/* Pool 0x0C0E797C (read at 0x0C0E77C4): rotary in UTILITY level 2, 0 = CCW,
 * 1 = CW. Stock clamps at item 6; the mod items continue the list. */
void c850_ui_utility_scroll(s8 direction, u8 steps)
{
    s32 item = (s32)UI_UTIL_CURSOR;
    s32 next = item + (direction == 1 ? (s32)steps : -(s32)steps);
    s32 last = (s32)C850_UI_UTILITY_FIRST - 1 + (s32)ui_util_count();

    if (item < (s32)C850_UI_UTILITY_FIRST && next < (s32)C850_UI_UTILITY_FIRST) {
        UI_UTIL_SCROLL(direction, steps);
        return;
    }
    if (next < 0)
        next = 0;
    if (next > last)
        next = last;
    UI_UTIL_TIMEOUT = 15000u;
    if (next != item) {
        UI_UTIL_CURSOR = (u8)next;
        UI_UTIL_REDRAW(1);
    }
}

/* Pool 0x0C0E7F10 (read at 0x0C0E7E64): encoder press in the UTILITY list,
 * stock utility_set_menu_level(3, item). A mod item switches its value in
 * place (the list stays at level 2). */
void c850_ui_utility_select(u32 level, int item)
{
    const c850_ui_utility_item *it = 0;

    if (level == 3u && item >= (int)C850_UI_UTILITY_FIRST)
        it = ui_util_item((u32)item);
    if (it == 0) {
        UI_UTIL_SET_LEVEL(level, item);
        return;
    }
    it->select();
    c850_ui_utility_selects++;
    UI_UTIL_TIMEOUT = 15000u;
    UI_UTIL_REDRAW(1);
}

static void ui_util_row(u32 row, u8 tag, u8 arrows, const char *text)
{
    volatile u8 *record = (volatile u8 *)C850_MATRIX_ROW(row);
    u8 len = 0;

    record[0] = tag;
    record[1] = 0;
    record[2] = arrows;
    record[3] = 0;
    while (text[len] != '\0') {
        record[5u + len] = (u8)text[len];
        len++;
    }
    record[4] = len;
}

/* Rows 1 (title, scroll arrows) and 2 (value) of the UTILITY list on a mod
 * item, as stock writes them for its own items. */
void c850_ui_utility_draw(void)
{
    const c850_ui_utility_item *it;

    if (C850_U32(0x0C9D0AF8) != UI_UTIL_MODE || UI_UTIL_LEVEL != 2u ||
        UI_UTIL_CURSOR < C850_UI_UTILITY_FIRST)
        return;
    it = ui_util_item(UI_UTIL_CURSOR);
    if (it == 0)
        return;
    ui_util_row(1, 0x06, 0x0B, it->title);
    ui_util_row(2, 0x43, 0, it->value());
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
static volatile u8 fl_hold_req;
static u32 fl_seen, fl_until;
static u8 fl_msg[UI_TEXT_MAX];       /* the message, copied when the request is seen */
static u8 fl_mlen, fl_active, fl_hold;
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
    fl_hold_req = 0u;
    fl_text = text;
    fl_req++;
}

void c850_ui_flash_hold(const char *text)
{
    fl_hold_req = 1u;
    fl_text = text;
    fl_req++;
}

void c850_ui_flash_clear(void)
{
    fl_hold_req = 0u;
    fl_text = 0;
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
    c850_ui_utility_draw();
    if (req != fl_seen) {
        const u8 *t = (const u8 *)fl_text;
        u32 n = 0;

        fl_seen = req;
        fl_hold = fl_hold_req;
        if (t) {
            while (n < UI_TEXT_MAX && t[n]) {
                fl_msg[n] = t[n];
                n++;
            }
            fl_mlen = (u8)n;
            fl_until = now + C850_UI_FLASH_MS;
            fl_active = 1;
            c850_ui_flashes++;
        } else {
            for (r = 0; r < UI_ROWS; r++)
                ui_unflash(r);
            fl_active = 0;
        }
    }
    if (!fl_active)
        return;
    if (!fl_hold && (s32)(now - fl_until) >= 0) {
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
