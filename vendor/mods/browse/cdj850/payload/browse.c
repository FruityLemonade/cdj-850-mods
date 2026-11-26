/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "c850_ui.h"

#define PLAYED_MS 20001u
#define HISTORY_SIZE 1024u
#define TITLE_WORDS 128
#define DB_ID_COL 0u
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
static volatile u32 history_generation;
static u32 previous_id;
static u32 previous_tick;

void browse_mount_export(u32 drive)
{
    C850_FN(C850_DB_LOAD_SORT_MENU, void, (u32))(drive);
    if (drive == C850_DB_USB_DRIVE) {
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
    /* db_record_of_track only returns records of the USB source. Record
     * +0x16 is the size of the list the track was loaded from, not a drive. */
    record = c850_db_record_of_track(C850_INFO_TRACK);
    id = record ? C850_DB_RECORD_ID(record) : 0;
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
static u8 request_sort;  /* database task, the order of the request being served */
static u8 menu_held;
static u32 menu_pressed;

/* The highlighted dot-matrix row is a category of the top menu (ARTIST,
 * ALBUM, TRACK, PLAYLIST, ...). Inside a list MENU has stock uses (a playlist
 * row opens DELETE / CANCEL / OK), so the order is chosen up here. */
static int top_menu_highlighted(void)
{
    unsigned r;

    for (r = 0; r < 3; r++) {
        const volatile u8 *row = (const volatile u8 *)C850_MATRIX_ROW(r);

        if (row[0] & C850_MATRIX_ROW_CURSOR)
            return row[1] == C850_ITEM_MENU_CATEGORY;
    }
    return 0;
}

static void browse_sort_menu(void)
{
    u32 held = C850_K2 & C850_K2_MENU;
    u32 now = C850_TICK_MS;

    if (c850_ui_flag(C850_UI_FLAG_BROWSE_OFF) ||
        !export_mounted || C850_SOURCE_INDEX != 2 || C850_NAV_VIEW != C850_NAV_BROWSE ||
        C850_BROWSE_MENU_LEVEL >= 2 || C850_UI_LOCK || !top_menu_highlighted()) {
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

static int usb_request(u32 selection)
{
    return !c850_ui_flag(C850_UI_FLAG_BROWSE_OFF) && export_mounted &&
           (selection & 0xFFFFu) == C850_DB_USB_SELECTION;
}

/* Every database request passes the menu cache, which compares the request,
 * the selection's media byte and argument words 1..7. Word 7 is zero in
 * every USB browse request, so it carries the order: a list cached in one
 * order is never reused in another, and the requests that bypass the cache
 * (playlists, 0x1105) are rebuilt anyway. */
int browse_track_cache(u32 cache, u32 selection, u32 request, volatile u32 *arguments)
{
    request_sort = SORT_DEFAULT;
    if (usb_request(selection)) {
        request_sort = selected_sort;
        arguments[C850_DB_REQUEST_SPARE_ARG] = request_sort;
    }
    return C850_FN(C850_DB_MENU_CACHE, int, (u32, u32, u32, volatile u32 *))
        (cache, selection, request, arguments);
}

struct id_row {
    u32 id;
    u32 row;
};

static int id_row_before(const struct id_row *a, const struct id_row *b)
{
    return a->id < b->id;
}

static void sort_id_rows(struct id_row *v, u32 n)
{
    u32 gap, i, j;

    for (gap = n / 2; gap; gap /= 2)
        for (i = gap; i < n; i++) {
            struct id_row t = v[i];

            for (j = i; j >= gap && id_row_before(&t, &v[j - gap]); j -= gap)
                v[j] = v[j - gap];
            v[j] = t;
        }
}

static const struct id_row *find_id(const struct id_row *v, u32 n, u32 id)
{
    u32 lo = 0, hi = n;

    while (lo < hi) {
        u32 mid = (lo + hi) / 2;

        if (v[mid].id < id)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < n && v[lo].id == id ? &v[lo] : 0;
}

static u32 column_u32(u32 row, u32 column)
{
    u8 converted = 0;
    u32 *value = C850_FN(C850_DB_ROW_COLUMN, u32 *, (u32, u32, u8 *))(row, column, &converted);
    u32 v = value ? *value : 0;

    if (converted)
        C850_FN(C850_DB_FREE, void, (void *))(value);
    return v;
}

/* One pass over djdbContent: the sort value of every result row, by the
 * row's content ID. Rows whose ID the table lacks keep 0xFFFFFFFF (last). */
static int content_values(u32 selection, const struct id_row *ids, u32 count, u32 *values)
{
    u32 (*query)(u32, u32, u32, u32, u32, u32) =
        C850_FN(C850_DB_TRACK_QUERY, u32, (u32, u32, u32, u32, u32, u32));
    u32 column = request_sort == SORT_BPM ? DB_BPM_COL : DB_KEY_COL;
    u32 cursor, row, error = 0, i;

    for (i = 0; i < count; i++)
        values[i] = 0xFFFFFFFFu;
    cursor = query(0, 4, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, selection);
    if (!cursor)
        return 0;
    while ((row = C850_FN(C850_DB_CURSOR_NEXT, u32, (u32, u32 *))(cursor, &error)) != 0) {
        const struct id_row *hit = find_id(ids, count, column_u32(row, DB_ID_COL));

        if (hit) {
            u32 v = column_u32(row, column);
            const struct id_row *first = hit;

            /* a playlist may hold the same track twice */
            while (first > ids && first[-1].id == hit->id)
                first--;
            for (; first < ids + count && first->id == hit->id; first++)
                values[first->row] = v;
        }
    }
    C850_FN(C850_DB_CURSOR_CLOSE, int, (u32))(cursor);
    return 1;
}

/* Reorder the published index by value, keeping the stock order (title,
 * playlist position, ...) among equal values. */
static void order_index(volatile u16 *index, u32 count, const u32 *values, struct id_row *work)
{
    u32 gap, i, j;

    for (i = 0; i < count; i++) {
        work[i].id = values[index[i]];
        work[i].row = (i << 16) | index[i];
    }
    for (gap = count / 2; gap; gap /= 2)
        for (i = gap; i < count; i++) {
            struct id_row t = work[i];

            for (j = i; j >= gap && (t.id < work[j - gap].id ||
                 (t.id == work[j - gap].id && t.row < work[j - gap].row)); j -= gap)
                work[j] = work[j - gap];
            work[j] = t;
        }
    for (i = 0; i < count; i++)
        index[i] = (u16)work[i].row;
}

static void sort_results(u32 results)
{
    void *(*alloc)(u32) = C850_FN(C850_DB_MALLOC, void *, (u32));
    void (*release)(void *) = C850_FN(C850_DB_FREE, void, (void *));
    volatile u16 *index = (volatile u16 *)C850_DB_RESULTS_INDEX(results);
    u32 rows = C850_DB_RESULTS_ROWS(results);
    u32 count = C850_DB_RESULTS_COUNT(results);
    struct id_row *ids;
    u32 *values;
    u32 i;

    if (!index || !rows || count < 2)
        return;
    for (i = 0; i < count; i++)
        if (C850_DB_ROW_TYPE(rows + i * C850_DB_ROW_SIZE) != C850_DB_ITEM_TRACK)
            return;  /* not a track list (artists, playlists, keys, ...) */
    ids = alloc(count * sizeof *ids);
    values = alloc(count * sizeof *values);
    if (ids && values) {
        for (i = 0; i < count; i++) {
            ids[i].id = C850_DB_ROW_ID(rows + i * C850_DB_ROW_SIZE);
            ids[i].row = i;
        }
        sort_id_rows(ids, count);
        if (content_values(C850_DB_RESULTS_SELECTION(results), ids, count, values)) {
            order_index(index, count, values, ids);
            if (C850_DB_RESULTS_NUMBERED(results))
                for (i = 0; i < count; i++)
                    C850_DB_ROW_POSITION(rows + index[i] * C850_DB_ROW_SIZE) = (u16)(i + 1);
        }
    }
    release(ids);
    release(values);
}

/* Every list's results pass the stock finalizer, which builds the index in
 * the list's own order. Track lists are then reordered by BPM or key: TRACK,
 * playlists, an artist's or album's tracks, a key's tracks, HISTORY, ... */
int browse_sort_results(u32 results)
{
    int status = C850_FN(C850_DB_FINISH_RESULTS, int, (u32))(results);

    if (status == 0 && results && request_sort != SORT_DEFAULT &&
        usb_request(C850_DB_RESULTS_SELECTION(results)))
        sort_results(results);
    return status;
}

static const char *browse_utility_value(void)
{
    return c850_ui_flag(C850_UI_FLAG_BROWSE_OFF) ? "OFF" : "ON";
}

static void browse_utility_select(void)
{
    int off = !c850_ui_flag(C850_UI_FLAG_BROWSE_OFF);

    if (off)
        selected_sort = SORT_DEFAULT;
    c850_ui_set_flag(C850_UI_FLAG_BROWSE_OFF, off);
}

static const c850_ui_utility_item browse_utility_item = {
    70u, "BROWSE MODS", browse_utility_value, browse_utility_select,
};
C850_UI_UTILITY_ITEM(browse_utility_item);
