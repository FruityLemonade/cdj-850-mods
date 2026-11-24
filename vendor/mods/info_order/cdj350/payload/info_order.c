/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * info_order (CDJ-350 MAIN 1.14): the INFO pages of the track view (see
 * ../patch.py and docs/cdj350.md, "INFO pages").
 *
 * Row 1 stays the title. Row 2, paged with the rotary:
 *   0  "Title - Artist"          (scrolls; stock: the artist)
 *   1  "128.23 8A"               (actual BPM with two decimals, Camelot key;
 *                                 stock: the album)
 *   1' "RATING ****"             (one more click on page 1; payload sub-page)
 *   2  the stock phase meter page
 *
 * The CDJ-350's metadata reply carries four fields only and its track record
 * has no key or rating, so the DB server's track-row fetch is wrapped: it
 * notes the rating and the key name of each track it reads, by rekordbox id.
 *
 * Integer only (no FPU).
 */
#include "c350.h"

volatile u32 info_order_on = 1;

/* ---- key and rating per track, from the DB server ---- */

#define IO_CACHE            4u
#define IO_KEY_MAX          8u

typedef struct {
    u32 id;                 /* rekordbox track id, 0 = free */
    u8 rating;              /* 0..5 */
    u8 key[IO_KEY_MAX];     /* ASCII, NUL-terminated */
} io_meta;

static io_meta io_cache[IO_CACHE];
static u32 io_next;

volatile u32 info_order_rows;           /* evidence: rows noted */
volatile u32 info_order_last_key_id;
volatile u8 info_order_last_key_raw[16];

io_meta *info_order_meta(u32 id)
{
    u32 i;

    if (id == 0)
        return 0;
    for (i = 0; i < IO_CACHE; i++)
        if (io_cache[i].id == id)
            return &io_cache[i];
    return 0;
}

/* UTF-16 (big endian, as the DB keeps it) to ASCII; stops at NUL or a
 * character outside 0x20..0x7E. */
void info_order_key_ascii(const volatile u16 *t, u8 *out)
{
    u32 k = 0;

    while (t && k < IO_KEY_MAX - 1 && t[k] >= 0x20 && t[k] < 0x7F) {
        out[k] = (u8)t[k];
        k++;
    }
    out[k] = 0;
}

/* Pool 0x0C051AEC (read at 0x0C051A32 in the 0x2002 builder): fetch the
 * track row, then note its rating and key name. */
int info_order_track_row(u32 id, u32 row)
{
    int r = c350_db_track_row(id, row);
    io_meta *m;
    u32 key, text = 0, k;

    if (r != 0 || !info_order_on || id == 0)
        return r;
    m = info_order_meta(id);
    if (m == 0) {
        m = &io_cache[io_next];
        io_next = (io_next + 1u) % IO_CACHE;
    }
    m->id = id;
    m->rating = C350_DB_ROW_RATING(row);
    m->key[0] = 0;
    key = C350_DB_ROW_KEY(row);
    info_order_last_key_id = key;
    if (key != 0 && key != 0xFFFFFFFFu && c350_db_key_name(key, &text) == 0 && text) {
        for (k = 0; k < 16; k++)
            info_order_last_key_raw[k] = ((const volatile u8 *)text)[k];
        info_order_key_ascii((const volatile u16 *)text, m->key);
    }
    info_order_rows++;
    return r;
}

/* ---- BPM ---- */

/* round(bpm100 * (10000 + pitch) / 10000); pitch in 1/100 %, +-100 % at most. */
u32 info_order_x100(u32 bpm100, s32 pitch)
{
    u32 x;

    if (pitch < -10000)
        pitch = -10000;
    if (pitch > 10000)
        pitch = 10000;
    x = (bpm100 * (u32)(10000 + pitch) + 5000u) / 10000u;
    return x > 99999u ? 99999u : x;
}

/* The grid's BPM x100 at the beat at or before the play position, 0 when
 * the grid window in RAM does not cover it. */
u32 info_order_grid_bpm100(void)
{
    u32 total = C350_GRID_BEAT_COUNT, first = (u32)C350_GRID_WINDOW_INDEX * 500u, n;
    u32 frame = C350_PLAY_FRAME, lo, hi, mid;

    if (C350_GRID_WINDOW_OK == 0 || C350_IS_USB_MODE != 1 || total <= first)
        return 0;
    n = total - first;
    if (n > 1000u)
        n = 1000u;
    if (frame < c350_beat_grid_frame(0))
        return first == 0 ? C350_GRID_BPM100(0) : 0;
    lo = 0;
    hi = n - 1;
    while (lo < hi) {
        mid = (lo + hi + 1u) / 2u;
        if (c350_beat_grid_frame(mid) <= frame)
            lo = mid;
        else
            hi = mid - 1u;
    }
    if (lo == n - 1u && first + n < total)
        return 0;                       /* beyond the window */
    return C350_GRID_BPM100(lo);
}

/* ---- key text (as the CDJ-850's info_order) ---- */

static const s16 io_semi_up[12] = { 294, 906, 1554, 2241, 2969, 3740, 4557, 5423, 6340, 7311, 8341, 9431 };
static const s16 io_semi_down[12] = { -285, -830, -1345, -1831, -2289, -2722, -3131, -3516, -3880, -4224,
                                      -4548, -4854 };

/* Semitones the pitch moves the key, to the nearest one (-12..12). */
int info_order_semitones(s32 pitch)
{
    int k = 0;

    if (pitch > 0) {
        while (k < 12 && pitch >= io_semi_up[k])
            k++;
        return k;
    }
    while (k < 12 && pitch <= io_semi_down[k])
        k++;
    return -k;
}

/* A key text as Camelot (1..12, minor = A); 0 if it is none. Classic ("Am",
 * "F#m", "Db"), Camelot ("8A") and Open Key ("1m", "6d"). */
int info_order_camelot(const u8 *t, int *minor)
{
    static const u8 pc_of[7] = { 9, 11, 0, 2, 4, 5, 7 };  /* A..G */
    int i = 0, n = 0, pc;
    u8 c;

    while (t[i] == ' ')
        i++;
    c = t[i];
    if (c >= '0' && c <= '9') {
        while (t[i] >= '0' && t[i] <= '9' && n < 100)
            n = n * 10 + (t[i++] - '0');
        c = t[i] | 0x20;
        if (n < 1 || n > 12)
            return 0;
        if (c == 'a' || c == 'b') {
            *minor = c == 'a';
            return n;
        }
        if (c == 'm' || c == 'd') {
            *minor = c == 'm';
            return (n - 1 + 7) % 12 + 1;
        }
        return 0;
    }
    c |= 0x20;
    if (c < 'a' || c > 'g')
        return 0;
    pc = pc_of[c - 'a'];
    i++;
    if (t[i] == '#') {
        pc++;
        i++;
    } else if (t[i] == 'b') {
        pc += 11;
        i++;
    }
    pc %= 12;
    *minor = t[i] == 'm' && t[i + 1] != 'a';
    return (int)((7u + 7u * (u32)(pc + 12 - (*minor ? 9 : 0))) % 12u) + 1;
}

/* Camelot number moved by s semitones (+1 semitone = +7 on the wheel). */
int info_order_camelot_shift(int n, int s)
{
    u32 v;

    if (s < -12)
        s = -12;
    if (s > 12)
        s = 12;
    v = (u32)(n - 1) + 7u * (u32)(s + 12);
    return (int)(v % 12u) + 1;
}

static u32 io_put_u(u8 *out, u32 v)
{
    u8 digits[10];
    u32 k = 0, len = 0;

    do {
        digits[k++] = (u8)('0' + v % 10u);
        v /= 10u;
    } while (v && k < 10);
    while (k)
        out[len++] = digits[--k];
    return len;
}

/* "128.23 8A": BPM x100 (0 = none), the key text (empty = none). */
u32 info_order_bpm_key_text(u8 *out, u32 x100, const u8 *key, int mt_on, s32 pitch)
{
    u32 len = 0, i;
    int n, minor = 0;

    if (x100) {
        len += io_put_u(out + len, x100 / 100u);
        out[len++] = '.';
        out[len++] = (u8)('0' + x100 / 10u % 10u);
        out[len++] = (u8)('0' + x100 % 10u);
    }
    if (key[0]) {
        if (len)
            out[len++] = ' ';
        n = info_order_camelot(key, &minor);
        if (n) {
            if (!mt_on)
                n = info_order_camelot_shift(n, info_order_semitones(pitch));
            len += io_put_u(out + len, (u32)n);
            out[len++] = minor ? 'A' : 'B';
        } else {
            for (i = 0; i < IO_KEY_MAX && key[i] && len < 13u; i++)
                out[len++] = key[i];
        }
    }
    out[len] = 0;
    return len;
}

/* "RATING ***", or "NO RATING". */
u32 info_order_rating_text(u8 *out, u32 stars)
{
    static const u8 head[] = "RATING ";
    u32 len = 0, i;

    if (stars == 0 || stars > 5) {
        static const u8 none[] = "NO RATING";
        for (i = 0; none[i]; i++)
            out[len++] = none[i];
    } else {
        for (i = 0; head[i]; i++)
            out[len++] = head[i];
        for (i = 0; i < stars; i++)
            out[len++] = '*';
    }
    out[len] = 0;
    return len;
}

/* ---- the pages ---- */

#define IO_ROW_MAX          96u

static u8 io_row[IO_ROW_MAX];
static u8 io_sub;                       /* page 1: 0 BPM/key, 1 rating */
volatile u32 info_order_pages;          /* evidence: rows made */

static u32 io_strcpy(u8 *out, u32 at, const volatile u8 *s, u32 max)
{
    while (*s && at < max)
        out[at++] = *s++;
    out[at] = 0;
    return at;
}

/* Pool 0x0C068428 (read at 0x0C06833A, INFO page 0): stock returns the
 * artist; the mod returns "Title - Artist". */
u32 info_order_page0(int track)
{
    u32 rec, len;

    if (!info_order_on || (rec = c350_record_of_track(track)) == 0)
        return c350_track_artist(track);
    len = io_strcpy(io_row, 0, (const volatile u8 *)C350_REC_TITLE(rec), IO_ROW_MAX - 1);
    if (*(const volatile u8 *)C350_REC_ARTIST(rec)) {
        if (len)
            len = io_strcpy(io_row, len, (const volatile u8 *)" - ", IO_ROW_MAX - 1);
        io_strcpy(io_row, len, (const volatile u8 *)C350_REC_ARTIST(rec), IO_ROW_MAX - 1);
    }
    info_order_pages++;
    return (u32)io_row;
}

/* The actual BPM x100 of a record: the grid's at the current beat, else the
 * rekordbox BPM; times the pitch. 0 = none. */
static u32 io_actual_x100(u32 rec)
{
    u32 bpm100 = info_order_grid_bpm100();

    if (bpm100 == 0 && C350_REC_BPM_OK(rec))
        bpm100 = C350_REC_BPM100(rec);
    return bpm100 ? info_order_x100(bpm100, C350_PITCH) : 0;
}

/* Pool 0x0C06842C (read at 0x0C068392, INFO page 1): stock returns the
 * album; the mod returns "128.23 8A", or the rating on the sub-page. */
u32 info_order_page1(int track)
{
    u32 rec;
    io_meta *m;
    static const u8 nokey[1] = { 0 };

    if (!info_order_on || (rec = c350_record_of_track(track)) == 0)
        return c350_track_album(track);
    m = info_order_meta(C350_REC_ID(rec));
    if (io_sub)
        info_order_rating_text(io_row, m ? m->rating : 0u);
    else if (info_order_bpm_key_text(io_row, io_actual_x100(rec), m ? m->key : nokey,
                                     C350_MASTER_TEMPO == 1, C350_PITCH) == 0)
        return c350_track_album(track);
    info_order_pages++;
    return (u32)io_row;
}

/* Pool 0x0C0C9048 (read at 0x0C0C8F8A): the INFO rotary. Page 1 gets a
 * second step (the rating) before the stock page 2. The caller asks for a
 * redraw while the page is 0 or 1. */
void info_order_scroll(u32 dir, u32 steps)
{
    u32 page = C350_INFO_PAGE;

    if (!info_order_on) {
        c350_info_page_scroll(dir, steps);
        return;
    }
    if ((dir & 0xFFu) == 1u) {
        if (page == 1u && io_sub == 0u) {
            io_sub = 1;
            return;
        }
        c350_info_page_scroll(dir, steps);
        if (page == 0u)
            io_sub = 0;
        return;
    }
    if (page == 1u && io_sub == 1u) {
        io_sub = 0;
        return;
    }
    c350_info_page_scroll(dir, steps);
    if (page == 2u && C350_INFO_PAGE == 1u)
        io_sub = 1;
}
