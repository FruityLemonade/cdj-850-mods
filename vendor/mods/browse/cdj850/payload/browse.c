/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "c850_ui.h"

#define PLAYED_MS 20001u
#define HISTORY_SIZE 1024u
#define TITLE_WORDS 128
#define DB_TITLE_COL 4u
#define DB_BPM_COL 8u
#define DB_KEY_COL 0x14u

struct played_track {
    u32 id;
    u32 elapsed;
};

/* The display task is the only writer of the table. The database task
 * publishes the mount generation and reads the table to format replies. */
static volatile struct played_track played[HISTORY_SIZE];
static volatile u32 mount_generation;
static volatile u8 export_mounted;
static u8 active_sort;  /* database task, order used to navigate the open track list */
static volatile u32 history_generation;
static u32 previous_id;
static u32 previous_tick;

void browse_mount_export(u32 drive)
{
    C850_FN(C850_DB_LOAD_SORT_MENU, void, (u32))(drive);
    if (drive == C850_DB_USB_DRIVE) {
        active_sort = 0;
        mount_generation++;
        export_mounted = 1;
    }
}

int browse_unmount_export(u32 db, u32 drive)
{
    if (drive == C850_DB_USB_DRIVE)
        export_mounted = 0;
    return C850_FN(C850_DB_UNMOUNT, int, (u32, u32))(db, drive);
}

static volatile struct played_track *history_slot(u32 id)
{
    u32 start = (id ^ (id >> 10)) & (HISTORY_SIZE - 1);
    u32 index = start;

    do {
        if (!played[index].id || played[index].id == id)
            return &played[index];
        index = (index + 1) & (HISTORY_SIZE - 1);
    } while (index != start);
    return 0;
}

/* A browse reply only runs when the list changes. Mark a visible loaded
 * row in the display pass as well, once its original title matches the
 * loaded record. A prefixed row then differs from that original title. */
static void mark_loaded_row(u32 record)
{
    const volatile u8 *title = (const volatile u8 *)C850_DB_RECORD_TITLE(record);
    unsigned r;

    if (C850_NAV_VIEW != C850_NAV_BROWSE || C850_DISPLAY_MODE != 1 ||
        C850_BROWSE_MENU_LEVEL >= 2)
        return;
    for (r = 0; r < 3; r++) {
        volatile u8 *row = (volatile u8 *)C850_MATRIX_ROW(r);
        unsigned i, length = row[4];

        if (row[1] != 0x5A || (row[0] & 0x10) || !length || length == 255)
            continue;
        for (i = 0; i < length && title[i] && row[5 + i] == title[i]; i++)
            ;
        if (i != length || title[length])
            continue;
        for (i = length; i > 0; i--)
            row[5 + i] = row[5 + i - 1];
        row[5] = '*';
        row[4] = (u8)(length + 1);
    }
}

/* Use elapsed transport time so pitch changes, cue jumps and loop wraps
 * cannot advance the timer by jumping the playhead forward. */
void browse_playback_tick(u32 *regs, u32 *sp)
{
    volatile struct played_track *slot;
    u32 generation = mount_generation;
    u32 now = C850_TICK_MS;
    u32 elapsed = now - previous_tick;
    u32 state, record, id, i;

    (void)regs;
    (void)sp;
    previous_tick = now;
    if (c850_ui_flag(C850_UI_FLAG_BROWSE_OFF)) {
        previous_id = 0;
        return;
    }
    if (generation != history_generation) {
        for (i = 0; i < HISTORY_SIZE; i++) {
            played[i].id = 0;
            played[i].elapsed = 0;
        }
        previous_id = 0;
        history_generation = generation;
    }
    if (!export_mounted || C850_SOURCE_INDEX != 2 || !C850_IS_USB_MODE) {
        previous_id = 0;
        return;
    }
    state = C850_PLAYER_STATE;
    if ((state != C850_STATE_PLAYING && state != C850_STATE_LOOPING) ||
        !C850_DSP_TARGET_SPEED) {
        previous_id = 0;
        return;
    }
    record = c850_db_record_of_track(C850_INFO_TRACK);
    id = record && C850_DB_RECORD_DRIVE(record) == C850_DB_USB_DRIVE
         ? C850_DB_RECORD_ID(record) : 0;
    if (id && id == previous_id && elapsed < PLAYED_MS) {
        slot = history_slot(id);
        if (slot && slot->elapsed < PLAYED_MS) {
            elapsed += slot->elapsed;
            slot->elapsed = elapsed < PLAYED_MS ? elapsed : PLAYED_MS;
            slot->id = id;
        }
        if (slot && slot->elapsed >= PLAYED_MS)
            mark_loaded_row(record);
    }
    previous_id = id;
}

/* Replace only the title pointer handed to the stock browse reply builder.
 * The cached title and ID are also used by selection and track navigation. */
void browse_mark_title(u32 *regs, u32 *sp)
{
    static u16 marked_title[TITLE_WORDS];
    const volatile u16 *title = (const volatile u16 *)regs[7];
    volatile struct played_track *slot;
    u32 type = regs[0] & 0xFFu;  /* stock JSR delay slot moves r0 to r5 */
    u32 id;
    int n;

    (void)sp;
    if (c850_ui_flag(C850_UI_FLAG_BROWSE_OFF) ||
        !export_mounted || history_generation != mount_generation ||
        C850_SOURCE_INDEX != 2 || C850_NAV_VIEW != C850_NAV_BROWSE ||
        (type != 0x04 && type != 0x5A && type != 0x5D))
        return;
    id = C850_U32((u32)title - C850_BROWSE_CACHE_TITLE_OFFSET);
    if (!id || id == 0xFFFFFFFFu || !title[0])
        return;
    slot = history_slot(id);
    if (!slot || slot->id != id || slot->elapsed < PLAYED_MS)
        return;

    marked_title[0] = '*';
    for (n = 1; n < TITLE_WORDS - 1 && title[n - 1]; n++)
        marked_title[n] = title[n - 1];
    if (marked_title[n - 1] >= 0xD800 && marked_title[n - 1] <= 0xDBFF)
        n--;
    marked_title[n] = 0;
    regs[7] = (u32)marked_title;
}

enum sort_mode { SORT_DEFAULT, SORT_BPM, SORT_KEY };

static volatile u8 selected_sort;
static u8 request_sort;  /* database task, latched when checking the menu cache */
static u8 query_sort;  /* database task, valid inside browse_track_query */
static u8 preserve_cursor;
static u8 menu_held;
static u32 menu_pressed;

static void browse_sort_menu(void)
{
    u32 held = C850_K2 & C850_K2_MENU;
    u32 now = C850_TICK_MS;

    if (c850_ui_flag(C850_UI_FLAG_BROWSE_OFF) ||
        !export_mounted || C850_SOURCE_INDEX != 2 || C850_NAV_VIEW != C850_NAV_BROWSE ||
        C850_BROWSE_MENU_LEVEL >= 2 || C850_UI_LOCK) {
        menu_held = 0;
        return;
    }
    if (held) {
        if (!menu_held) {
            menu_pressed = now;
            menu_held = 1;
        }
    } else if (menu_held) {
        menu_held = 0;
        if (now - menu_pressed < 1000u) {
            selected_sort = selected_sort == SORT_KEY ? SORT_DEFAULT : selected_sort + 1;
            if (selected_sort == SORT_BPM)
                c850_ui_flash("SORT BPM");
            else if (selected_sort == SORT_KEY)
                c850_ui_flash("SORT KEY");
            else
                c850_ui_flash("SORT DEFAULT");
        }
    }
}
C850_UI_TICK_HOOK(browse_sort_menu);

/* The CDJ-850 preserves the second TRACK argument in its menu cache key
 * and ignores it when building the stock cursor. Include the chosen order
 * in the key so reopening a list cannot reuse results in a different order. */
int browse_track_cache(u32 cache, u32 selection, u32 request, volatile u32 *arguments)
{
    request_sort = SORT_DEFAULT;
    preserve_cursor = 0;
    if (!c850_ui_flag(C850_UI_FLAG_BROWSE_OFF) &&
        request == 0x1004 && (selection & 0xFFFFu) == 0x0301u && export_mounted) {
        if ((selection >> 16 & 0xFFu) == 1)
            active_sort = selected_sort;
        request_sort = active_sort;
        arguments[1] = request_sort;
    }
    return C850_FN(C850_DB_MENU_CACHE, int, (u32, u32, u32, volatile u32 *))
        (cache, selection, request, arguments);
}

/* The stock TRACK handler opens a cursor over djdbContent. Bit 7 requests
 * the materialized cursor that the stock engine can sort before it emits
 * any menu items. A track load reuses that same database path. */
u32 browse_track_query(u32 flags, u32 kind, u32 filter1, u32 filter2, u32 filter3, u32 selection)
{
    u32 cursor;
    u32 (*query)(u32, u32, u32, u32, u32, u32) =
        C850_FN(C850_DB_TRACK_QUERY, u32, (u32, u32, u32, u32, u32, u32));

    if (c850_ui_flag(C850_UI_FLAG_BROWSE_OFF) ||
        kind != 4 || (selection & 0xFFFFu) != 0x0301u || !export_mounted)
        return query(flags, kind, filter1, filter2, filter3, selection);
    query_sort = request_sort;
    cursor = query(flags | (query_sort ? 0x80u : 0), kind, filter1, filter2, filter3, selection);
    if (!cursor && query_sort) {
        request_sort = SORT_DEFAULT;
        cursor = query(flags, kind, filter1, filter2, filter3, selection);
    }
    preserve_cursor = cursor && request_sort;
    query_sort = SORT_DEFAULT;
    return cursor;
}

int browse_sort_fields(u32 cursor, u32 count, const u32 *columns, const u8 *ascending)
{
    static const u32 bpm_columns[] = { DB_BPM_COL, DB_TITLE_COL };
    static const u32 key_columns[] = { DB_KEY_COL, DB_TITLE_COL };
    int (*sort)(u32, u32, const u32 *, const u8 *) =
        C850_FN(C850_DB_SORT_CURSOR, int, (u32, u32, const u32 *, const u8 *));

    if (!c850_ui_flag(C850_UI_FLAG_BROWSE_OFF)) {
        if (query_sort == SORT_BPM)
            columns = bpm_columns;
        else if (query_sort == SORT_KEY)
            columns = key_columns;
    }
    return sort(cursor, count, columns, ascending);
}

/* TRACK normally builds an index sorted by title. The rows arrive in the
 * cursor's order, so build an identity index and let stock release its sort
 * workspace and publish the same result object. */
int browse_sort_results(u32 results)
{
    int (*finish)(u32) = C850_FN(C850_DB_FINISH_RESULTS, int, (u32));
    volatile u16 *index;
    u32 count, i;
    u8 primary;
    int status;

    if (c850_ui_flag(C850_UI_FLAG_BROWSE_OFF) || !preserve_cursor || !results)
        return finish(results);
    preserve_cursor = 0;
    index = (volatile u16 *)C850_DB_RESULTS_INDEX(results);
    if (!index)
        return finish(results);
    count = C850_DB_RESULTS_COUNT(results);
    for (i = 0; i < count; i++)
        index[i] = (u16)i;
    primary = C850_DB_RESULTS_SORT(results);
    C850_DB_RESULTS_SORT(results) = 0;
    status = finish(results);
    C850_DB_RESULTS_SORT(results) = primary;
    return status;
}

static const char *browse_utility_value(void)
{
    return c850_ui_flag(C850_UI_FLAG_BROWSE_OFF) ? "OFF" : "ON";
}

static void browse_utility_select(void)
{
    int off = !c850_ui_flag(C850_UI_FLAG_BROWSE_OFF);

    if (off) {
        selected_sort = SORT_DEFAULT;
        active_sort = SORT_DEFAULT;
    }
    c850_ui_set_flag(C850_UI_FLAG_BROWSE_OFF, off);
}

static const c850_ui_utility_item browse_utility_item = {
    70u, "BROWSE MODS", browse_utility_value, browse_utility_select,
};
C850_UI_UTILITY_ITEM(browse_utility_item);
