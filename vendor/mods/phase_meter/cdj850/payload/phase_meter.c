/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * phase_meter: CDJ-350 beat & phase meter on INFO Page 2 (CDJ-850 MAIN 1.13).
 *
 * Inserts 3 unused DB item codes (0x13, 0x14, 0x15 -> valid empty buffer
 * rec + 0x7BC in FUN_0C074824) at indices 2..4 of each loaded USB DB record
 * (0x0C9D44D4), shifting original Page 2+ items down by one page. When the
 * user views INFO Page 2 (mode 11, cursor 2), overrides outgoing 0x80 mode to
 * 7 (3-row centered layout with no icon column or scrollbar) and renders
 * centered, unhighlighted, icon-free rows (tag 0x11..0x13, attr0 0x00) showing
 * beat-in-bar, actual BPM, 4-beat boxes, and a 13-step sub-beat phase sweep
 * using only 32-bit integer math and glyphs supported by the CDJ-850 dot matrix
 * & vfd_render.h. Also snaps INFO cursor across 3..4 for 1-click Page 2 nav.
 */
#include "c850.h"

#define PM_MAX_ITEMS    18
#define PM_REC_COUNT(r) C850_U8((r) + 0x851u)
#define PM_REC_ITEM(r, i) C850_U8((r) + 0x852u + (u32)(i))
#define PM_REC_TRACK(r) C850_S16((r) + 0x14u)
#define PM_REC_VALID    3u

#define PM_CODE_0       0x13u
#define PM_CODE_1       0x14u
#define PM_CODE_2       0x15u

#define PM_MODE_CENTER  7u
#define PM_MODE_INFO    11u
#define PM_PHASE_STEPS  13u

volatile u32 phase_meter_on = 1;
volatile u32 phase_meter_records;      /* DB records updated with Page 2 slot */
volatile u32 phase_meter_updates;      /* row rewrites when Page 2 is displayed */

static u8 pm_last_cursor;

static int pm_is_code(u8 c)
{
    return c == PM_CODE_0 || c == PM_CODE_1 || c == PM_CODE_2;
}

/* Insert {PM_CODE_0, PM_CODE_1, PM_CODE_2} at indices 2..4 of `list[0..n)`.
 * Returns the new count (<= PM_MAX_ITEMS), or 0 if unchanged. */
int phase_meter_insert_items(volatile u8 *list, int n)
{
    u8 tmp[PM_MAX_ITEMS];
    int i, m = 0, out_n;

    if (n < 2 || n > PM_MAX_ITEMS)
        return 0;
    if (n >= 5 && list[2] == PM_CODE_0 && list[3] == PM_CODE_1 && list[4] == PM_CODE_2)
        return 0;

    for (i = 0; i < n && m < PM_MAX_ITEMS; i++) {
        if (!pm_is_code(list[i]))
            tmp[m++] = list[i];
    }
    if (m < 2)
        return 0;

    out_n = m + 3;
    if (out_n > PM_MAX_ITEMS)
        out_n = PM_MAX_ITEMS;

    list[0] = tmp[0];
    list[1] = tmp[1];
    list[2] = PM_CODE_0;
    list[3] = PM_CODE_1;
    list[4] = PM_CODE_2;
    for (i = 5; i < out_n; i++)
        list[i] = tmp[i - 3];
    return out_n;
}

static void pm_update_records(void)
{
    u32 rec = C850_DB_RECORDS;
    int i;

    for (i = 0; i < 3; i++, rec += C850_DB_RECORD_SIZE) {
        int new_n;

        if (C850_U32(rec) != PM_REC_VALID)
            continue;
        new_n = phase_meter_insert_items((volatile u8 *)(rec + 0x852u), (int)PM_REC_COUNT(rec));
        if (new_n) {
            PM_REC_COUNT(rec) = (u8)new_n;
            if (PM_REC_TRACK(rec) == C850_INFO_TRACK)
                C850_INFO_COUNT = (u8)new_n;
            phase_meter_records++;
        }
    }
}

typedef struct {
    u8 valid;
    u8 from_grid;
    u8 locked;
    u8 beat;      /* 1..4 */
    u8 fine;      /* 0..12 */
    u32 bpm100;   /* actual played BPM * 100 */
} pm_state_t;

static u32 pm_actual_bpm100(u32 bpm100, s32 pitch)
{
    u32 x;

    if (pitch < -10000)
        pitch = -10000;
    if (pitch > 10000)
        pitch = 10000;
    x = (bpm100 * (u32)(10000 + pitch) + 5000u) / 10000u;
    return x > 99999u ? 99999u : x;
}

static int pm_grid_phase(u32 ms, s32 pitch, pm_state_t *out)
{
    u32 w = C850_GRID_WINDOW_INDEX;
    u32 total = C850_GRID_BEAT_COUNT;
    u32 first = w * 500u;
    u32 n, lo, hi, dur, off, beat, grid_bpm100;

    if (C850_IS_USB_MODE != 1 || total <= first)
        return 0;
    n = total - first;
    if (n > 1000u)
        n = 1000u;
    if (n == 0u)
        return 0;
    if (ms < C850_GRID_TIME_MS(0)) {
        u32 t0, b0, before, pos;

        if (w != 0u)
            return 0;
        t0 = C850_GRID_TIME_MS(0);
        grid_bpm100 = C850_GRID_BPM100(0);
        dur = (n > 1u && C850_GRID_TIME_MS(1) > t0)
              ? (C850_GRID_TIME_MS(1) - t0)
              : (grid_bpm100 ? (6000000u / grid_bpm100) : 500u);
        if (dur == 0u)
            dur = 500u;
        before = (t0 - ms + dur - 1u) / dur;
        pos = dur - ((t0 - ms) % dur);
        if (pos == dur)
            pos = 0u;
        b0 = C850_GRID_BEAT_IN_BAR(0);
        if (b0 < 1u || b0 > 4u)
            b0 = 1u;
        beat = ((b0 - 1u + 4u - (before % 4u)) % 4u) + 1u;
        out->valid = 1;
        out->from_grid = 1;
        out->beat = (u8)beat;
        out->fine = (u8)((pos * PM_PHASE_STEPS) / dur);
        if (out->fine >= PM_PHASE_STEPS)
            out->fine = PM_PHASE_STEPS - 1u;
        out->bpm100 = pm_actual_bpm100(grid_bpm100, pitch);
        return 1;
    }
    if (ms >= C850_GRID_TIME_MS(n - 1u) && first + n < total)
        return 0;

    lo = 0u;
    hi = n;
    while (hi - lo > 1u) {
        u32 mid = (lo + hi) >> 1;
        if (C850_GRID_TIME_MS(mid) <= ms)
            lo = mid;
        else
            hi = mid;
    }

    grid_bpm100 = C850_GRID_BPM100(lo);
    if (lo + 1u < n && C850_GRID_TIME_MS(lo + 1u) > C850_GRID_TIME_MS(lo))
        dur = C850_GRID_TIME_MS(lo + 1u) - C850_GRID_TIME_MS(lo);
    else if (lo > 0u && C850_GRID_TIME_MS(lo) > C850_GRID_TIME_MS(lo - 1u))
        dur = C850_GRID_TIME_MS(lo) - C850_GRID_TIME_MS(lo - 1u);
    else
        dur = grid_bpm100 ? (6000000u / grid_bpm100) : 500u;
    if (dur == 0u)
        dur = 500u;

    beat = C850_GRID_BEAT_IN_BAR(lo);
    if (beat < 1u || beat > 4u)
        beat = ((first + lo) % 4u) + 1u;

    off = ms - C850_GRID_TIME_MS(lo);
    if (off >= dur) {
        u32 extra = off / dur;
        off %= dur;
        beat = ((beat - 1u + extra) % 4u) + 1u;
    }

    out->valid = 1;
    out->from_grid = 1;
    out->beat = (u8)beat;
    out->fine = (u8)((off * PM_PHASE_STEPS) / dur);
    if (out->fine >= PM_PHASE_STEPS)
        out->fine = PM_PHASE_STEPS - 1u;
    out->bpm100 = pm_actual_bpm100(grid_bpm100, pitch);
    return 1;
}

void phase_meter_compute(u32 frame, u32 base_tenths, s32 pitch, u8 locked, pm_state_t *out)
{
    u32 ms = frame * 40u / 3u;

    out->valid = 0;
    out->from_grid = 0;
    out->locked = locked;
    out->beat = 0;
    out->fine = 0;
    out->bpm100 = 0;

    if (pm_grid_phase(ms, pitch, out))
        return;

    if (base_tenths == 0u)
        return;

    {
        /* 32-bit exact bar/beat phase in 0.1 ms units (no 64-bit libgcc div). */
        u32 bar_len = 24000000u / base_tenths;
        u32 beat_len = bar_len / 4u;
        u32 in_bar, in_beat;

        if (beat_len == 0u)
            beat_len = 1u;
        if (bar_len == 0u)
            bar_len = 4u;
        in_bar = (ms * 10u) % bar_len;
        out->beat = (u8)(in_bar / beat_len + 1u);
        if (out->beat > 4u)
            out->beat = 4u;
        in_beat = in_bar % beat_len;
        out->fine = (u8)((in_beat * PM_PHASE_STEPS) / beat_len);
        if (out->fine >= PM_PHASE_STEPS)
            out->fine = PM_PHASE_STEPS - 1u;
        out->bpm100 = pm_actual_bpm100(base_tenths * 10u, pitch);
        out->valid = 1;
    }
}

static int pm_put_u(char *out, u32 v)
{
    char digits[8];
    int k = 0, p = 0;

    do {
        digits[k++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v && k < 8);
    while (k)
        out[p++] = digits[--k];
    return p;
}

void phase_meter_format(const pm_state_t *st, char rows[3][24], u8 lens[3])
{
    int p = 0, b, i;

    if (!st->valid) {
        static const char no_bpm[] = "BEAT ---.-- ----";
        for (i = 0; no_bpm[i]; i++)
            rows[0][i] = no_bpm[i];
        rows[0][i] = 0;
        lens[0] = (u8)i;
    } else {
        const char *tag = st->locked ? "LOCK" : (st->from_grid ? "GRID" : "BPM");
        u32 whole = st->bpm100 / 100u;
        u32 frac = st->bpm100 % 100u;

        rows[0][p++] = st->locked ? 'L' : 'B';
        rows[0][p++] = (char)('0' + st->beat);
        rows[0][p++] = '/';
        rows[0][p++] = '4';
        rows[0][p++] = ' ';
        p += pm_put_u(rows[0] + p, whole);
        rows[0][p++] = '.';
        rows[0][p++] = (char)('0' + frac / 10u);
        rows[0][p++] = (char)('0' + frac % 10u);
        rows[0][p++] = ' ';
        for (i = 0; tag[i]; i++)
            rows[0][p++] = tag[i];
        rows[0][p] = 0;
        lens[0] = (u8)p;
    }

    p = 0;
    for (b = 1; b <= 4; b++) {
        if (b > 1)
            rows[1][p++] = ' ';
        if (st->valid && (int)st->beat == b) {
            rows[1][p++] = '(';
            rows[1][p++] = (char)('0' + b);
            rows[1][p++] = ')';
        } else {
            rows[1][p++] = '_';
            rows[1][p++] = (char)('0' + b);
            rows[1][p++] = '_';
        }
    }
    rows[1][p] = 0;
    lens[1] = (u8)p;

    p = 0;
    rows[2][p++] = '[';
    for (i = 0; i < (int)PM_PHASE_STEPS; i++)
        rows[2][p++] = (st->valid && (int)st->fine == i) ? '+' : '-';
    rows[2][p++] = ']';
    rows[2][p] = 0;
    lens[2] = (u8)p;
}

static __attribute__((noinline)) void pm_write_row(int r, const char *text, u8 len)
{
    volatile u8 *row = (volatile u8 *)C850_MATRIX_ROW(r);
    volatile u8 *src = (volatile u8 *)C850_ROW_SOURCE(r);
    u8 tag = (u8)(0x11u + (u32)r);
    int same = (row[0] == tag && row[1] == 0u && row[4] == len);
    int i;

    for (i = 0; i < (int)len; i++) {
        if (row[5 + i] != (u8)text[i])
            same = 0;
        src[i] = (u8)text[i];
    }
    src[len] = 0;
    if (same)
        return;

    row[0] = tag;
    row[1] = 0u;
    row[2] = 0u;
    row[3] = 0u;
    for (i = 0; i < (int)len; i++)
        row[5 + i] = (u8)text[i];
    row[4] = len;
    phase_meter_updates++;
}

void phase_meter_hook(u32 *regs, u32 *sp)
{
    pm_state_t st;
    char rows[3][24];
    u8 lens[3];
    u8 cursor, locked;
    s32 pitch;
    int r;

    (void)regs;
    (void)sp;
    if (!phase_meter_on)
        return;

    if (C850_IS_USB_MODE == 1)
        pm_update_records();

    if (C850_DISPLAY_MODE != PM_MODE_INFO) {
        pm_last_cursor = C850_INFO_CURSOR;
        return;
    }
    cursor = C850_INFO_CURSOR;
    if (cursor == 3u || cursor == 4u) {
        if (pm_last_cursor == 2u) {
            if (C850_INFO_COUNT > 5u) {
                C850_INFO_CURSOR = 5u;
                pm_last_cursor = 5u;
                return;
            }
            C850_INFO_CURSOR = 2u;
            cursor = 2u;
        } else {
            C850_INFO_CURSOR = 2u;
            cursor = 2u;
        }
    }
    pm_last_cursor = cursor;
    if (cursor != 2u)
        return;

    locked = C850_BPMLOCK_ACTIVE == 1;
    pitch = c850_effective_pitch();
    phase_meter_compute(C850_PLAY_FRAME, C850_BPM_BASE_TENTHS, pitch, locked, &st);
    phase_meter_format(&st, rows, lens);
    for (r = 0; r < 3; r++)
        pm_write_row(r, rows[r], lens[r]);

    /* Switch the outgoing 0x80 header from INFO list mode (0x0B) to 3-row
     * centered mode (0x07) so the physical Panel CPU centers all 3 rows across
     * the full 100 px width without reserving a left icon/category column.
     * FUN_0C0622B0 restores C850_DISPLAY_MODE = 11 on the next DisplayTask pass. */
    C850_DISPLAY_MODE = PM_MODE_CENTER;
    C850_U8(0x0C9D0AFCu) = 0u;
    C850_U8(0x0C9D0AFDu) = 0u;
    C850_U16(0x0C9D0AFEu) = 0u;
    C850_U16(0x0C9D0B00u) = 0u;
    C850_U8(0x0C9D0B14u) &= 0x1Fu;
}
