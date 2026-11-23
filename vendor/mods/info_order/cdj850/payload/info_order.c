/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * info_order: the loaded-track / INFO screens in the order
 *
 *   screen 1 (title view)   Track, "128.23 12A *****" (actual BPM, Camelot key, rating), waveform
 *   screen 2 (jog on)       Artist, Rating, Key
 *   screen 3                Album, Comment, then any other item
 *
 * instead of stock Track, Artist, waveform / Album, BPM, Comment / Key, Rating.
 * See ../patch.py and docs/cdj850-modding.md section 4b.8.
 *
 * Where the order comes from (MAIN 1.13, traced): when a USB track loads, the
 * track-metadata reader (0x0C0387xx) appends the code of each field it gets to
 * a list, and FUN_0C038642 copies the count and the codes into the track's DB
 * record (3 slots at 0x0C9D44D4, 0x864 bytes each, valid when +0 == 3):
 * +0x851 = count, +0x852.. = codes (rekordbox field IDs: 04 title, 07 artist,
 * 02 album, 0D BPM, 23 comment, 0F key, 0A rating, ...). The INFO screen walks
 * that list: cursor 0/1 is the title view (mode word 10), which always shows
 * the title and artist (FUN_0C0669D6 formats codes 4 and 7, row 2 is the
 * waveform); cursor >= 2 shows list entries three at a time (mode 11). Each
 * row record carries the code of its item in attr0 (byte 1).
 *
 * info_order_hook (tail of display_stage_bpm, the bpm999 span; DisplayTask,
 * after the row filler and before the text serialiser, so a row written here
 * is the one the panel gets) does two things in every pass:
 *   1. puts each valid record's list in the order above (title, BPM first:
 *      list entries 0 and 1 are the ones the title view stands for), when the
 *      list has both a title and a BPM entry; otherwise the list stays stock;
 *   2. in the title view of a track whose list is in that order, shows in row 1
 *      (attr0 0x0D, the BPM item) instead of the artist: the BPM the deck plays
 *      with two decimals, the key in Camelot notation and the rating as asterisks.
 *        BPM  mods/bpm_decimals' grid value at the current beat when that mod
 *             is built and the grid gave the tempo (bpm_decimals_actual_x100,
 *             weak here), else the record's BPMx100 (+0x1C) times the pitch.
 *        key  the record's key text (+0x730, what the INFO key item shows:
 *             "Am", "F#m", "Db", "8A" or Open Key "1m"), as Camelot; with
 *             MASTER TEMPO off it moves by the semitones the pitch is away
 *             from 0 (nearest semitone; +1 semitone = +7 on the wheel).
 *        rating  filled stars in the record's INFO item, one asterisk per star.
 *      Row and text source are both written (the source is not in the stock
 *      "NNN.N bpm" form, so bpm_decimals leaves the row alone).
 * The database request wrapper enables the USB rating field for the stock
 * metadata builder. The display hook only reorders the resulting fields.
 */
#include "c850.h"

#define IO_RECORDS          0x0C9D44D4u          /* 3 DB records of loaded tracks */
#define IO_RECORD_SIZE      0x864u
#define IO_RECORD_VALID     3u                   /* +0: 3 = in use (FUN_0C073F7C) */
#define IO_REC_TRACK(rec)   C850_S16((rec) + 0x14)
#define IO_REC_BPM_OK(rec)  C850_U32((rec) + 0x18)   /* FUN_0C0745F8: 1 = the BPM item has a value */
#define IO_REC_TENTHS(rec)  C850_U32((rec) + 0x1C)   /* FUN_0C07460C: DB tempo, BPM x 100 [emu: 12800] */
#define IO_REC_COUNT(rec)   C850_U8((rec) + 0x851)
#define IO_REC_ITEM(rec, i) C850_U8((rec) + 0x852 + (i))
#define IO_MAX_ITEMS        18                   /* FUN_0C038642 copies 18 bytes */
#define IO_TRACK            C850_S16(0x0C9D1D96) /* track the INFO screens show */
#define IO_DISPLAY_MODE     C850_U32(0x0C9D0AF8)
#define IO_MODE_TITLE       10
#define IO_ROW_SOURCE(r)    (0x0C9D196F + (r) * 0x100)

#define IO_CODE_TITLE       0x04
#define IO_CODE_ARTIST      0x07
#define IO_CODE_RATING      0x0A
#define IO_CODE_BPM         0x0D
#define IO_TEXT_MAX         32
#define IO_RATED_ROW_MAX    16                   /* full BPM, Camelot key and five stars */

/* The wanted order; codes not named here follow in their stock order. */
static const u8 io_order[] = { 0x04 /* title */, 0x0D /* BPM */,
                               0x13, 0x14, 0x15 /* phase_meter Page 2 */,
                               0x07 /* artist */, IO_CODE_RATING,
                               0x0F /* key */, 0x02 /* album */, 0x23 /* comment */ };

volatile u32 info_order_on = 1;        /* run-time switch: 0 = stock */
volatile u32 info_order_lists;         /* lists put in order (evidence, tests) */
volatile u32 info_order_rows;          /* title-view rows rewritten with the BPM */

/* Order list[0..n) into out; 0 when the list has no title or no BPM entry. */
int info_order_sort(const volatile u8 *list, int n, u8 *out)
{
    u8 used[IO_MAX_ITEMS];
    int i, k, m = 0, title = 0, bpm = 0;

    if (n < 0 || n > IO_MAX_ITEMS)
        return 0;
    for (i = 0; i < n; i++) {
        used[i] = 0;
        title |= list[i] == IO_CODE_TITLE;
        bpm |= list[i] == IO_CODE_BPM;
    }
    if (!title || !bpm)
        return 0;
    for (k = 0; k < (int)sizeof(io_order); k++)
        for (i = 0; i < n; i++)
            if (!used[i] && list[i] == io_order[k]) {
                out[m++] = list[i];
                used[i] = 1;
                break;
            }
    for (i = 0; i < n; i++)
        if (!used[i])
            out[m++] = list[i];
    return 1;
}

/* Put one record's list in order (written only when it differs). */
static void io_order_record(u32 rec)
{
    u8 out[IO_MAX_ITEMS];
    int i, n = IO_REC_COUNT(rec), same = 1;

    if (!info_order_sort((const volatile u8 *)(rec + 0x852), n, out))
        return;
    for (i = 0; i < n; i++)
        same &= IO_REC_ITEM(rec, i) == out[i];
    if (same)
        return;
    for (i = 0; i < n; i++)
        IO_REC_ITEM(rec, i) = out[i];
    info_order_lists++;
}

/* The 0x2002 server request builds and caches its metadata synchronously.
 * Enable the rating field for that request when USB INFO has it disabled.
 * The stock builder reads the rating with the other
 * fields and adds its normal type 0x0A item to the response. */
int info_order_track_info(u32 db, u32 request, u32 selection, u32 args)
{
    int (*stock)(u32, u32, u32, u32) = C850_FN(C850_DB_TRACK_INFO, int, (u32, u32, u32, u32));
    int result;

    if (!info_order_on || (selection & 0xFFFFu) != C850_DB_USB_INFO || C850_DB_USB_INFO_RATING(db))
        return stock(db, request, selection, args);

    C850_DB_USB_INFO_RATING(db) = 1;
    result = stock(db, request, selection, args);
    C850_DB_USB_INFO_RATING(db) = 0;
    return result;
}

/* ---- BPM, key and rating text ---- */

#define IO_REC_KEY(rec)     ((rec) + 0x730)      /* key text, NUL-terminated (FUN_0C074824 case 0x0F) */
#define IO_KEY_MAX          16

/* mods/bpm_decimals: the actual BPM x100 from the grid, 0 when it cannot say
 * (no grid gave the tempo). Weak default for builds without that mod. */
u32 bpm_decimals_actual_x100(void) __attribute__((weak, noinline));
u32 bpm_decimals_actual_x100(void)
{
    return 0;
}

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

/* Pitch thresholds (1/100 %) of semitones 1..12: ratio 2^((k - 1/2) / 12). */
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

/* Parse a key text into Camelot (number 1..12, minor = A); 0 if it is none.
 * Accepts classic ("Am", "F#m", "Dbm", "C", "Ebmaj", "Amin"), Camelot ("8A",
 * "12b") and Open Key ("1m", "6d"). */
int info_order_camelot(const volatile u8 *t, int *minor)
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
        if (c == 'a' || c == 'b') {          /* Camelot */
            *minor = c == 'a';
            return n;
        }
        if (c == 'm' || c == 'd') {          /* Open Key: 1m = Am = 8A */
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
    if (t[i] == '#') {                       /* "F#m" */
        pc++;
        i++;
    } else if (t[i] == 'b') {                /* "Bb", "Ebm": the note letter is upper case */
        pc += 11;
        i++;
    }
    pc %= 12;
    *minor = t[i] == 'm' && t[i + 1] != 'a';  /* "m", "min"; not "maj" */
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
    v = (u32)(n - 1) + 7u * (u32)(s + 12);   /* unsigned: no negative operand of % */
    return (int)(v % 12u) + 1;
}

static int io_put_u(u8 *out, u32 v)
{
    u8 digits[10];
    int k = 0, len = 0;

    do {
        digits[k++] = (u8)('0' + v % 10u);
        v /= 10u;
    } while (v && k < 10);
    while (k)
        out[len++] = digits[--k];
    return len;
}

static int io_rating(u32 rec)
{
    const volatile u8 *text = (const volatile u8 *)C850_INFO_RATING_TEXT(rec);
    int i, n = IO_REC_COUNT(rec), stars = 0;

    for (i = 0; i < n && i < IO_MAX_ITEMS; i++) {
        if (IO_REC_ITEM(rec, i) == IO_CODE_RATING) {
            for (i = 0; i < C850_INFO_RATING_MAX && text[i]; i++)
                stars += text[i] == C850_INFO_RATING_FILLED;
            return stars;
        }
    }
    return 0;
}

/* The parts that have a value, separated by spaces. Returns the length. */
int info_order_title_text(u8 *out, u32 x100, int bpm_ok, const volatile u8 *key,
                          int mt_on, s32 pitch, int stars)
{
    int len = 0, n, minor = 0, i, key_limit = IO_TEXT_MAX - 1;

    if (stars < 0 || stars > C850_INFO_RATING_MAX)
        stars = 0;
    if (stars)
        key_limit = IO_RATED_ROW_MAX - 1 - stars;

    if (bpm_ok) {
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
        } else {                               /* not a key we know: as it is */
            for (i = 0; i < IO_KEY_MAX && len < key_limit && key[i]; i++)
                out[len++] = key[i];
        }
    }
    if (stars) {
        if (len)
            out[len++] = ' ';
        for (i = 0; i < stars; i++)
            out[len++] = '*';
    }
    out[len] = 0;
    return len;
}

/* The record of the track the INFO screens show, 0 if none is in the table. */
static u32 io_shown_record(void)
{
    u32 rec = IO_RECORDS;
    int i;

    for (i = 0; i < 3; i++, rec += IO_RECORD_SIZE)
        if (C850_U32(rec) == IO_RECORD_VALID && IO_REC_TRACK(rec) == IO_TRACK)
            return rec;
    return 0;
}

/* Title view: row 1 shows the BPM, key and rating. */
static void io_title_row(void)
{
    volatile u8 *row = (volatile u8 *)C850_MATRIX_ROW(1);
    volatile u8 *src = (volatile u8 *)IO_ROW_SOURCE(1);
    u8 text[IO_TEXT_MAX];
    u32 rec, x100;
    s32 pitch = c850_effective_pitch();
    int i, len, same, bpm_ok;

    if (IO_DISPLAY_MODE != IO_MODE_TITLE || (row[1] != IO_CODE_ARTIST && row[1] != IO_CODE_BPM))
        return;
    rec = io_shown_record();
    if (!rec || IO_REC_COUNT(rec) < 2 || IO_REC_ITEM(rec, 1) != IO_CODE_BPM)
        return;                         /* the list is stock: so is the screen */
    bpm_ok = IO_REC_BPM_OK(rec) == 1;
    x100 = bpm_decimals_actual_x100();
    if (x100 == 0)
        x100 = info_order_x100(IO_REC_TENTHS(rec), pitch);
    else
        bpm_ok = 1;
    len = info_order_title_text(text, x100, bpm_ok, (const volatile u8 *)IO_REC_KEY(rec),
                                C850_MASTER_TEMPO == 1, pitch, io_rating(rec));
    same = row[1] == IO_CODE_BPM && row[4] == (u8)len;
    for (i = 0; i <= len; i++) {
        same &= i == len || row[5 + i] == text[i];
        src[i] = text[i];               /* NUL included */
    }
    if (same)
        return;
    row[1] = IO_CODE_BPM;
    for (i = 0; i < len; i++)
        row[5 + i] = text[i];
    row[4] = (u8)len;
    info_order_rows++;
}

void info_order_pass(void)
{
    u32 rec = IO_RECORDS;
    int i;

    if (!info_order_on || C850_IS_USB_MODE != 1)
        return;
    for (i = 0; i < 3; i++, rec += IO_RECORD_SIZE)
        if (C850_U32(rec) == IO_RECORD_VALID)
            io_order_record(rec);
    io_title_row();
}

/* Hook routine on display_stage_bpm's tail (contract: regs = saved r0..r7). */
void info_order_hook(u32 *regs, u32 *sp)
{
    (void)regs;
    (void)sp;
    info_order_pass();
}
