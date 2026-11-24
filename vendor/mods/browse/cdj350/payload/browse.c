/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * browse (CDJ-350 MAIN 1.14): played markers and TRACK sorting in the
 * rekordbox USB browser (see ../patch.py and docs/cdj350.md, "Browse").
 *
 * The rekordbox DB client is the same library as the CDJ-850's, so the
 * played table and the sort hooks follow mods/browse/cdj850/payload/browse.c.
 * What differs on the CDJ-350: the played time counts in the 1 ms key tick
 * (no display-pass hook), the sort order is a UTILITY item (SORT DEFAULT /
 * BPM / KEY, saved; the CDJ-350's MENU key is DISPLAY), and the title order
 * of the results is undone in their heap comparator (finish_results is
 * called directly, not through a pool word).
 */
#include "c350_ui.h"

#define PLAYED_MS       20001u
#define HISTORY_SIZE    1024u
#define TITLE_WORDS     128
#define DB_TITLE_COL    4u
#define DB_BPM_COL      8u
#define DB_KEY_COL      0x14u
#define DB_USB_DRIVE    3u
#define DB_USB_TRACKS   0x0301u                 /* drive 3, rekordbox DB kind 1 */
#define CACHE_TITLE_OFF 0x14u                   /* UTF-16 title after a cache entry's track id */

#define TICK_MS         C350_U32(0x0CD2DCC0)    /* +1 per 1 ms tick */
#define VIEW_SHOWN      C350_U8(0x0CD48534)     /* 1: the track view shows 0x0CD48546 */
#define VIEW_TRACK_A    C350_S16(0x0CD48546)
#define VIEW_TRACK_B    C350_S16(0x0C9B8162)

#define STOCK_KEYS_TICK     C350_FN(0x0C0C6EBC, void, (void))
#define STOCK_ADD_ITEM      C350_FN(0x0C02B900, int, (u32, u32, u32, u32))
#define STOCK_SORT_MENU     C350_FN(0x0C06CC04, void, (u32))
#define STOCK_UNMOUNT       C350_FN(0x0C04CA9C, int, (u32, u32))
#define STOCK_MENU_CACHE    C350_FN(0x0C0B083C, int, (u32, u32, u32, volatile u32 *))
#define STOCK_TRACK_QUERY   C350_FN(0x0C073C68, u32, (u32, u32, u32, u32, u32, u32))
#define STOCK_SORT_CURSOR   C350_FN(0x0C1073C4, int, (u32, u32, const u32 *, const u8 *))
#define STOCK_RESULT_CMP    C350_FN(0x0C0AF72E, int, (u32, u32, u32))

volatile u32 browse_on = 1;

struct played_track {
    u32 id;
    u32 elapsed;
};

/* The 1 ms tick is the only writer of the table. The database task
 * publishes the mount generation and reads the table to format replies. */
static volatile struct played_track played[HISTORY_SIZE];
static volatile u32 mount_generation;
static volatile u8 export_mounted;
static volatile u32 history_generation;
static u32 previous_id;

volatile u32 browse_marked;             /* evidence: titles prefixed */
volatile u32 browse_sorted;             /* evidence: sorted TRACK lists */

/* Pool 0x0C053D94 (read at 0x0C053CAC): the successful export.pdb open
 * loads the sort menu; a new USB export clears the played table. */
void browse_mount_export(u32 drive)
{
    STOCK_SORT_MENU(drive);
    if (drive == DB_USB_DRIVE) {
        mount_generation++;
        export_mounted = 1;
    }
}

/* Pool 0x0C0542E4 (read at 0x0C05409A): the DB closes a drive. */
int browse_unmount_export(u32 db, u32 drive)
{
    if (drive == DB_USB_DRIVE)
        export_mounted = 0;
    return STOCK_UNMOUNT(db, drive);
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

/* The rekordbox id of the loaded USB track, 0 if none. */
static u32 loaded_id(void)
{
    int track = VIEW_SHOWN == 1 ? VIEW_TRACK_A : VIEW_TRACK_B;
    u32 rec;

    if (track == 0 || track == -1)
        return 0;
    rec = c350_record_of_track(track);
    return rec ? C350_REC_ID(rec) : 0;
}

/* Pool 0x0C0AE44C (read at 0x0C0AE422 in the 1 ms tick): keys_tick_1ms,
 * then one millisecond for the loaded track while it plays or loops. */
void browse_tick(void)
{
    volatile struct played_track *slot;
    u32 generation = mount_generation;
    u32 state, id, i;

    STOCK_KEYS_TICK();
    if (generation != history_generation) {
        for (i = 0; i < HISTORY_SIZE; i++) {
            played[i].id = 0;
            played[i].elapsed = 0;
        }
        previous_id = 0;
        history_generation = generation;
    }
    if (!browse_on || !export_mounted || C350_SOURCE_INDEX != 2 || C350_IS_USB_MODE != 1) {
        previous_id = 0;
        return;
    }
    state = C350_PLAYER_STATE;
    if (state != C350_STATE_PLAYING && state != C350_STATE_LOOPING) {
        previous_id = 0;
        return;
    }
    id = loaded_id();
    if (id && id == previous_id) {
        slot = history_slot(id);
        if (slot && slot->elapsed < PLAYED_MS) {
            slot->id = id;
            slot->elapsed++;
        }
    }
    previous_id = id;
}

int browse_played(u32 id)
{
    volatile struct played_track *slot = history_slot(id);

    return slot && slot->id == id && slot->elapsed >= PLAYED_MS;
}

/* Pool 0x0C02CE98 (read at 0x0C02CDF2): the browse reply adds its header
 * and each item through this routine (r5 item type, r7 the UTF-16 title in
 * the 0x114-byte cache entry). A played track's title gets a '*'. */
int browse_add_item(u32 reply, u32 type, u32 attr, u32 title)
{
    static u16 marked_title[TITLE_WORDS];
    const volatile u16 *t = (const volatile u16 *)title;
    u32 id;
    int n;

    type &= 0xFFu;
    if (!browse_on || !export_mounted || history_generation != mount_generation ||
        C350_SOURCE_INDEX != 2 || (type != 0x04 && type != 0x5A && type != 0x5D) ||
        !title || (title & 1))
        return STOCK_ADD_ITEM(reply, type, attr, title);
    id = C350_U32(title - CACHE_TITLE_OFF);
    if (!id || id == 0xFFFFFFFFu || !t[0] || !browse_played(id))
        return STOCK_ADD_ITEM(reply, type, attr, title);
    marked_title[0] = '*';
    for (n = 1; n < TITLE_WORDS - 1 && t[n - 1]; n++)
        marked_title[n] = t[n - 1];
    if (marked_title[n - 1] >= 0xD800 && marked_title[n - 1] <= 0xDBFF)
        n--;
    marked_title[n] = 0;
    browse_marked++;
    return STOCK_ADD_ITEM(reply, type, attr, (u32)marked_title);
}

/* ---- TRACK sort ---- */

enum sort_mode { SORT_DEFAULT, SORT_BPM, SORT_KEY };

static u8 active_sort;      /* database task: the order of the open track list */
static u8 request_sort;     /* database task, latched in the menu cache call */
static u8 query_sort;       /* database task, valid inside browse_track_query */
static u8 preserve_cursor;  /* database task: the results keep the cursor order */

static u32 selected_sort(void)
{
    u32 s = c350_ui_field(C350_UI_FLAG_SORT_MASK, C350_UI_FLAG_SORT_SHIFT);

    return s <= SORT_KEY ? s : SORT_DEFAULT;
}

/* Pool 0x0C04F478 (read at 0x0C04F434): the menu cache. The chosen order
 * goes into the TRACK request's cache key, so a list reopened in another
 * order is built again. */
int browse_track_cache(u32 cache, u32 selection, u32 request, volatile u32 *arguments)
{
    int r;

    request_sort = SORT_DEFAULT;
    preserve_cursor = 0;
    if (browse_on && request == 0x1004 && (selection & 0xFFFFu) == DB_USB_TRACKS && export_mounted) {
        if ((selection >> 16 & 0xFFu) == 1)
            active_sort = (u8)selected_sort();
        request_sort = active_sort;
        arguments[1] = request_sort;
    }
    r = STOCK_MENU_CACHE(cache, selection, request, arguments);
    preserve_cursor = 0;
    return r;
}

/* Pool 0x0C06CB34 (read at 0x0C06C8F8 and 0x0C06C96C): the track query.
 * Bit 7 of the flags asks for a materialized cursor the engine can sort. */
u32 browse_track_query(u32 flags, u32 kind, u32 filter1, u32 filter2, u32 filter3, u32 selection)
{
    u32 cursor;

    if (kind != 4 || (selection & 0xFFFFu) != DB_USB_TRACKS || !export_mounted)
        return STOCK_TRACK_QUERY(flags, kind, filter1, filter2, filter3, selection);
    query_sort = request_sort;
    cursor = STOCK_TRACK_QUERY(flags | (query_sort ? 0x80u : 0u), kind, filter1, filter2, filter3, selection);
    if (!cursor && query_sort) {
        request_sort = SORT_DEFAULT;
        query_sort = SORT_DEFAULT;
        cursor = STOCK_TRACK_QUERY(flags, kind, filter1, filter2, filter3, selection);
    }
    preserve_cursor = cursor && request_sort;
    if (preserve_cursor)
        browse_sorted++;
    query_sort = SORT_DEFAULT;
    return cursor;
}

/* Pool 0x0C07411C (read at 0x0C073F5A, 0x0C073FF6, 0x0C07406E): sorts the
 * cursor; during the TRACK query by BPM or key, then title. */
int browse_sort_fields(u32 cursor, u32 count, const u32 *columns, const u8 *ascending)
{
    static const u32 bpm_columns[] = { DB_BPM_COL, DB_TITLE_COL };
    static const u32 key_columns[] = { DB_KEY_COL, DB_TITLE_COL };

    if (query_sort == SORT_BPM)
        columns = bpm_columns;
    else if (query_sort == SORT_KEY)
        columns = key_columns;
    return STOCK_SORT_CURSOR(cursor, count, columns, ascending);
}

/* Pool 0x0C0AFAB4 (read at 0x0C0AFA90 in finish_results 0x0C0AFA30): the
 * heap comparator that orders the TRACK results by title. A sorted TRACK
 * list keeps the cursor's order: row index order. */
int browse_result_compare(u32 results, u32 a, u32 b)
{
    if (!preserve_cursor)
        return STOCK_RESULT_CMP(results, a, b);
    return (int)(a & 0xFFFFu) - (int)(b & 0xFFFFu);
}

/* UTILITY: SORT DEFAULT / BPM / KEY (c350_ui.c owns the list). */
static const char *br_sort_value(void)
{
    static const char *const names[] = { "DEFAULT", "BPM", "KEY" };

    if (!browse_on)
        return 0;
    return names[selected_sort()];
}

static void br_sort_select(void)
{
    c350_ui_set_field(C350_UI_FLAG_SORT_MASK, C350_UI_FLAG_SORT_SHIFT, (selected_sort() + 1u) % 3u);
}

static const c350_ui_utility_item br_sort_item = {
    30u, "SORT", br_sort_value, br_sort_select,
};
C350_UI_UTILITY_ITEM(br_sort_item);
