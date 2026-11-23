/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * bpm_decimals: the actual BPM with two decimals in place of the track's BPM
 * in the INFO view of a loaded track (see ../patch.py).
 *
 * The INFO item list shows the rekordbox DB BPM as "128.0 bpm" (row record
 * attr0 0x0D, text built by FUN_0C06734E). While a beat grid gave the BPM,
 * that row reads "132.25" (the DB value is dropped):
 *
 *   x100 = round(grid BPMx100 at the current beat * (10000 + pitch) / 10000)
 *
 * as whole.hundredths. Otherwise the row is exactly stock.
 *
 * bpm_decimals_find_beat  replaces grid_find_beat (0x0C0012E4) for its only
 *                         caller, FUN_0C073788 (the grid tempo behind the
 *                         base BPM word), through the pool word 0x0C073830.
 *                         It notes the BPMx100 of the beat stock looked up:
 *                         the proof that the base BPM came from a grid.
 * bpm_decimals_hook       hook on the tail of display_stage_bpm (0x0C0642AA,
 *                         bpm999's span). DisplayTask runs, per pass:
 *                         FUN_0C0614EA (which calls the INFO row filler
 *                         FUN_0C06543E, on events only), FUN_0C062F80 (which
 *                         calls display_stage_bpm), then FUN_0C064CA4 (which
 *                         serialises the three row records for the panel and
 *                         bumps the text sequence only when a byte changed).
 *                         So a row written here is the one the panel gets in
 *                         the same pass, and an unchanged row is not resent.
 *
 * Integer only (no FPU). Every product fits in 32 bits: BPMx100 <= 65535,
 * 10000 + pitch <= 20000 (WIDE +100 %), 65535 * 20000 + 5000 < 2^32.
 */
#include "c850.h"

/* Pitch in 1/100 % (s16), written by FUN_0C0D9B44 (pool 0x0C0D9E9C = 0x0CE6CA50; the
 * byte 0x0CE6CA4C, pool 0x0C0D9E98, is its WIDE flag); clamped there to
 * +-6/10/16 % or +-100 % (WIDE). */
#define BPMD_PITCH            C850_PITCH
/* Stock clamps the base tenths at 180.0 (FUN_0C073788), or 999.0 with bpm999. */
#define BPMD_CLAMP_STOCK      1800
#define BPMD_CLAMP_BPM999     9990
/* Text sources of the three rows (NUL-terminated), 0x100 apart; row r is
 * filled from source r by FUN_0C06543E (pool 0x0C065578..0x0C065580). */
#define BPMD_ROW_SOURCE(r)    (0x0C9D196F + (r) * 0x100)
/* Display mode word (FUN_0C065F18: header byte 2 of the 0x80 frame) */
#define BPMD_DISPLAY_MODE     C850_U32(0x0C9D0AF8)
#define BPMD_MODE_LOADED      10          /* loaded track, title view */
#define BPMD_MODE_INFO        11          /* loaded track, INFO item list [emu] */
#define BPMD_ATTR_BPM         0x0D        /* attr0 of the INFO view's BPM item [emu] */
#define BPMD_TEXT_MAX         24

volatile u32 bpm_decimals_grid_ok;      /* 1 after a stock grid lookup succeeded */
volatile u32 bpm_decimals_grid_bpm100;  /* BPMx100 of the beat stock looked up */
volatile u32 bpm_decimals_x100_last;    /* last value shown (evidence, tests) */
volatile u32 bpm_decimals_writes;       /* row rewrites (when the record differs from our text) */

/* round(bpm100 * (10000 + pitch) / 10000), at most 99999 (999.99). */
u32 bpm_decimals_x100(u32 bpm100, s32 pitch)
{
    u32 f, x;
    if (pitch < -10000)
        pitch = -10000;
    if (pitch > 10000)
        pitch = 10000;
    f = (u32)(10000 + pitch);
    x = (bpm100 * f + 5000u) / 10000u;
    return x > 99999u ? 99999u : x;
}

/* BPMx100 of the beat that holds `ms` (the last beat with time <= ms) in the
 * grid window as it is in RAM, without paging. 0 when the window cannot say:
 * empty, or `ms` lies outside it and another window would hold it. */
u32 bpm_decimals_beat_bpm100(u32 ms)
{
    u32 w = C850_GRID_WINDOW_INDEX;
    u32 total = C850_GRID_BEAT_COUNT;
    u32 first = w * 500u;
    u32 n, lo, hi;

    if (total <= first)
        return 0;
    n = total - first;
    if (n > 1000u)
        n = 1000u;
    if (ms < C850_GRID_TIME_MS(0)) {
        if (w != 0)
            return 0;                       /* earlier window */
        return C850_GRID_BPM100(0);         /* before the first beat */
    }
    if (ms >= C850_GRID_TIME_MS(n - 1) && first + n < total)
        return 0;                           /* a later window */
    lo = 0;
    hi = n;                                 /* t[lo] <= ms < t[hi] (t[n] = inf) */
    while (hi - lo > 1) {
        u32 mid = (lo + hi) >> 1;
        if (C850_GRID_TIME_MS(mid) <= ms)
            lo = mid;
        else
            hi = mid;
    }
    return C850_GRID_BPM100(lo);
}

/* Pool redirect of grid_find_beat for FUN_0C073788 (same ABI). */
int bpm_decimals_find_beat(u32 ctx, u32 time_ms, int mode)
{
    int idx = c850_grid_find_beat(ctx, time_ms, mode);
    if (idx != -1) {
        bpm_decimals_grid_bpm100 = C850_GRID_BPM100(idx);
        bpm_decimals_grid_ok = 1;
    }
    return idx;
}

/* Does the base tempo word hold what FUN_0C073788 makes of `bpm100`? */
static int bpmd_base_from_grid(u32 base, u32 bpm100)
{
    u32 t = (bpm100 + 5u) / 10u;
    if (base == t)
        return 1;
    return t > base && (base == BPMD_CLAMP_STOCK || base == BPMD_CLAMP_BPM999);
}

/* The actual BPM x100 when a grid gave the BPM, else 0 (row stays stock). */
u32 bpm_decimals_actual_x100(void)
{
    u32 base = C850_BPM_BASE_TENTHS;
    u32 bpm100;

    if (!bpm_decimals_grid_ok)              /* nothing until a grid lookup succeeded */
        return 0;
    if (base == 0) {                        /* track change / no tempo yet */
        bpm_decimals_grid_ok = 0;
        return 0;
    }
    if (C850_IS_USB_MODE != 1)
        return 0;
    if (!bpmd_base_from_grid(base, bpm_decimals_grid_bpm100))
        return 0;                           /* BPM counter or another source: stock */
    bpm100 = bpm_decimals_beat_bpm100(C850_PLAY_FRAME * 40u / 3u);   /* frames * 1000 / 75 */
    if (bpm100 == 0)
        bpm100 = bpm_decimals_grid_bpm100;
    return bpm_decimals_x100(bpm100, c850_effective_pitch());
}

/* Stock BPM item text: digits '.' digit " bpm" (FUN_0C06734E). Returns the
 * length of the number part (before " bpm"), 0 if `t` is not such a text. */
int bpm_decimals_stock_number(const volatile u8 *t, int len)
{
    int i, n = len - 4;
    if (n < 3 || n > 8 || t[n] != ' ' || t[n + 1] != 'b' || t[n + 2] != 'p' || t[n + 3] != 'm')
        return 0;
    if (t[n - 2] != '.')
        return 0;
    for (i = 0; i < n; i++)
        if (i != n - 2 && (t[i] < '0' || t[i] > '9'))
            return 0;
    return n;
}

/* whole "." hundredths into out (nothing else); returns the length. */
int bpm_decimals_format(u8 *out, u32 x100)
{
    u8 digits[4];
    u32 whole = x100 / 100u, frac = x100 % 100u;
    int k = 0, len = 0;

    do {
        digits[k++] = (u8)('0' + whole % 10u);
        whole /= 10u;
    } while (whole && k < 4);
    while (k)
        out[len++] = digits[--k];
    out[len++] = '.';
    out[len++] = (u8)('0' + frac / 10u);
    out[len++] = (u8)('0' + frac % 10u);
    return len;
}

static int bpmd_strlen(const volatile u8 *s, int max)
{
    int n = 0;
    while (n < max && s[n])
        n++;
    return n;
}

/* Write len/text of row record `row` only if they differ. */
static void bpmd_set_row(volatile u8 *row, const u8 *text, int len)
{
    int i, same = row[4] == (u8)len;
    for (i = 0; same && i < len; i++)
        same = row[5 + i] == text[i];
    if (same)
        return;
    for (i = 0; i < len; i++)
        row[5 + i] = text[i];
    row[4] = (u8)len;
    bpm_decimals_writes++;
}

void bpm_decimals_rows(void)
{
    u8 text[BPMD_TEXT_MAX];
    u32 x100, mode;
    int r;

    mode = BPMD_DISPLAY_MODE;
    if (mode != BPMD_MODE_LOADED && mode != BPMD_MODE_INFO)
        return;
    x100 = bpm_decimals_actual_x100();
    for (r = 0; r < 3; r++) {
        volatile u8 *row = (volatile u8 *)C850_MATRIX_ROW(r);
        const volatile u8 *src = (const volatile u8 *)BPMD_ROW_SOURCE(r);
        int slen, n, len;

        if (row[1] != BPMD_ATTR_BPM)
            continue;
        slen = bpmd_strlen(src, 0x40);
        n = bpm_decimals_stock_number(src, slen);
        if (n == 0)
            continue;                       /* not the stock BPM item text: leave it */
        if (x100 == 0) {                    /* no grid: the stock text */
            for (len = 0; len < slen; len++)
                text[len] = src[len];
        } else {
            len = bpm_decimals_format(text, x100);
            bpm_decimals_x100_last = x100;
        }
        bpmd_set_row(row, text, len);
    }
}

/* Hook routine on display_stage_bpm's tail (contract: regs = saved r0..r7). */
void bpm_decimals_hook(u32 *regs, u32 *sp)
{
    (void)regs;
    (void)sp;
    bpm_decimals_rows();
}
