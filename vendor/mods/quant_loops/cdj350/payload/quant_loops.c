/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * quant_loops (CDJ-350 MAIN 1.14): loops snap to the rekordbox beat grid and
 * wrap to the feeder block (see ../patch.py and docs/cdj350.md, "Quantized
 * loops" and "Block-precise loops").
 *
 * Only when a beat grid of the loaded USB track is in RAM; otherwise every
 * path below hands the stock values through unchanged.
 *
 * Entry points (pool-word redirects, ../patch.py):
 *   quant_loops_auto_in    next_sector in auto_beat_loop_set: snaps the in
 *                          sector, computes the grid out (pre-send), keeps
 *                          the snapped sector (post-send)
 *   quant_loops_auto_send  audio_send_cmd there: the grid out into the cmd
 *                          0x1e new-loop request, arms the block loop
 *   quant_loops_loop_in    next_sector in the LOOP IN paths
 *   quant_loops_loop_out   audio_send_cmd in the LOOP OUT paths: the out
 *                          sector, arms the block loop
 *   quant_loops_tick_entry keys_state4_looping (state-4 player cycle): which
 *                          loop the block path owns, then stock
 *   quant_loops_out_isr    the feeder ISR's forward loop-out test: the
 *                          block-precise wrap, then stock
 *
 * Precision: blocks of 84 samples (1/7 frame, 1.9 ms), as on the CDJ-850
 * (mods/quant_loops/cdj850, docs/cdj850-block-loops-design.md). The CDJ-350
 * differs in two places, both from MAIN 1.14:
 *   - the stock loop-out test (0x0C0161C8) notes the out record's ring sector
 *     when the play frame equals the out frame, and the feeder's "out frame
 *     reached" branch (flag 0x0C7E4F25) starts LOOP DIVIDE's beat module
 *     there. So the stock out record of a block loop is the frame that holds
 *     the last block of the first pass ((out_blk - 1) / 7): the play reaches
 *     it every pass, and for the first pass the stock wrap (play frame > out)
 *     is not due before the block wrap (later passes may end a block into
 *     the next frame; the hook owns those wraps and ignores the stock verdict).
 *   - after its in-ring jump the stock wrap trims the fill ahead (USB:
 *     0x0C00B71C) and starts the beat module when it has not started
 *     (0x0C016474); the block wrap does the same.
 *
 * Integer only (no FPU). Products stay below 2^32: ms * 75 for ms < 57e6
 * (15.9 h), ms * 21, q * 7875000 for q <= 512 (32 beats).
 */
#include "c350.h"
#include "c350_ui.h"

#define QL_BPM_CLAMP_STOCK  1800    /* 180.0 BPM clamp */
#define QL_NONE             (-1)    /* no beat (lead-in before the first beat) */
#define QL_AWAY             (-2)    /* the window in RAM does not cover the position */

/* Run-time switch. 1 = the mod may snap; 0 = stock. Snapping also needs the
 * on-deck setting (UTILITY QUANTIZE, saved in the settings record, c350_ui.h). */
volatile u32 quant_loops_on = 1;

static volatile u8 qb_armed;         /* the block loop (quant_loops_out_hook) owns the wraps */

static int ql_on(void)
{
    return quant_loops_on != 0 && c350_ui_quantize();
}

/* UTILITY: QUANTIZE ON/OFF and Q SIZE after Ver (c350_ui.c owns the list).
 * Quantize is on on a fresh deck. */
static const char *ql_quant_value(void)
{
    if (!quant_loops_on)
        return 0;
    return c350_ui_quantize() ? "ON" : "OFF";
}

static void ql_quant_select(void)
{
    int off = c350_ui_quantize();

    c350_ui_set_flag(C350_UI_FLAG_QUANT_OFF, off);
    if (off)
        qb_armed = 0;                /* quantize off: the loop wraps stock from now on */
}

static const c350_ui_utility_item ql_quant_item = {
    10u, "QUANTIZE", ql_quant_value, ql_quant_select,
};
C350_UI_UTILITY_ITEM(ql_quant_item);

/* Q SIZE: the grid step of LOOP IN, LOOP OUT and the auto loop's in,
 * 1 -> 1/2 -> 1/4 beat (saved; hidden while QUANTIZE is OFF). */
static const char *ql_size_value(void)
{
    u32 d;

    if (!quant_loops_on || !c350_ui_quantize())
        return 0;
    d = c350_ui_quant_div();
    return d == 4u ? "1/4" : d == 2u ? "1/2" : "1";
}

static void ql_size_select(void)
{
    u32 d = c350_ui_quant_div();

    c350_ui_set_quant_div(d == 1u ? 2u : d == 2u ? 4u : 1u);
}

static const c350_ui_utility_item ql_size_item = {
    15u, "Q SIZE", ql_size_value, ql_size_select,
};
C350_UI_UTILITY_ITEM(ql_size_item);

/* Pending loop of the auto loop being set (quant_loops_auto_in ->
 * quant_loops_auto_send -> post-send quant_loops_auto_in). */
static u32 ql_pending;
static u32 ql_post_send;
static s16 ql_in_sector = -1;
static u32 ql_pending_out;

/* Evidence for tests: last values and counters. */
volatile u32 quant_loops_last_in;
volatile u32 quant_loops_last_out;
volatile u32 quant_loops_last_pos;
volatile u32 quant_loops_auto_count;
volatile u32 quant_loops_in_count;
volatile u32 quant_loops_out_count;
volatile u32 quant_loops_refused;

/* ---- frames and the grid window ---- */

u32 quant_loops_ms_to_frames(u32 ms)
{
    return (ms * 75u + 500u) / 1000u;
}

u32 quant_loops_window_n(void)
{
    u32 total = C350_GRID_BEAT_COUNT;
    u32 first = (u32)C350_GRID_WINDOW_INDEX * 500u;
    u32 n;

    if (total <= first)
        return 0;
    n = total - first;
    return n > 1000u ? 1000u : n;
}

static int ql_more_after(u32 n)
{
    return (u32)C350_GRID_WINDOW_INDEX * 500u + n < C350_GRID_BEAT_COUNT;
}

u32 quant_loops_beat_frame(int i)
{
    return quant_loops_ms_to_frames(C350_GRID_TIME_MS(i));
}

int quant_loops_beat_at_or_before(u32 frame)
{
    u32 n = quant_loops_window_n();
    u32 lo, hi;

    if (n == 0)
        return QL_AWAY;
    if (frame < quant_loops_beat_frame(0))
        return C350_GRID_WINDOW_INDEX == 0 ? QL_NONE : QL_AWAY;
    if (frame >= quant_loops_beat_frame(n - 1))
        return ql_more_after(n) ? QL_AWAY : (int)(n - 1);
    lo = 0;
    hi = n - 1;
    while (hi - lo > 1) {
        u32 mid = (lo + hi) >> 1;
        if (quant_loops_beat_frame(mid) <= frame)
            lo = mid;
        else
            hi = mid;
    }
    return (int)lo;
}

int quant_loops_beat_nearest(u32 frame)
{
    int i = quant_loops_beat_at_or_before(frame);
    u32 n = quant_loops_window_n();

    if (i == QL_AWAY)
        return QL_AWAY;
    if (i == QL_NONE)
        return 0;
    if ((u32)i + 1 >= n)
        return i;
    if (frame - quant_loops_beat_frame(i) <= quant_loops_beat_frame(i + 1) - frame)
        return i;
    return i + 1;
}

static u32 ql_beats_ms(u32 beats, u32 bpm100)
{
    if (bpm100 == 0)
        bpm100 = 13000;
    return (beats * 6000000u + bpm100 / 2u) / bpm100;
}

u32 quant_loops_beat_ms_after(int i, u32 k)
{
    u32 n = quant_loops_window_n();
    if ((u32)i + k < n)
        return C350_GRID_TIME_MS(i + k);
    return C350_GRID_TIME_MS(i) + ql_beats_ms(k, C350_GRID_BPM100(i));
}

/* Grid time (ms) of position p sixteenths of a beat after beat i. */
u32 quant_loops_grid_ms(int i, u32 p)
{
    u32 t0 = quant_loops_beat_ms_after(i, p >> 4), r = p & 15u;

    if (r == 0)
        return t0;
    return t0 + (r * (quant_loops_beat_ms_after(i, (p >> 4) + 1u) - t0) + 8u) / 16u;
}

static __attribute__((noinline)) u32 ql_pt_frame(int i, u32 s)
{
    return quant_loops_ms_to_frames(quant_loops_grid_ms(i, s));
}

/* ---- quantize points (UTILITY Q SIZE: 1, 1/2 or 1/4 beat) ----
 *
 * A point is (beat i, step s): s sixteenths of a beat after beat i, s a
 * multiple of ql_step() (16, 8 or 4). Frame of a point: ql_pt_frame. */
static u32 ql_step(void)
{
    return 16u / c350_ui_quant_div();
}

/* Nearest point every `step` sixteenths to `frame` (ties to the earlier one):
 * beat index in *pi, step in *ps. Before the first beat of the grid: beat 0;
 * on the last beat of the window: that beat. Returns 0 or QL_AWAY. */
static __attribute__((noinline)) int ql_point_nearest(u32 frame, u32 step, int *pi, u32 *ps)
{
    u32 n = quant_loops_window_n(), s, best = 0, best_d = 0xFFFFFFFFu;
    int i = quant_loops_beat_at_or_before(frame);

    if (i == QL_AWAY)
        return QL_AWAY;
    *ps = 0;
    if (i == QL_NONE) {
        *pi = 0;
        return 0;
    }
    if ((u32)i + 1 >= n) {
        *pi = i;
        return 0;
    }
    for (s = 0; s <= 16u; s += step) {
        u32 f = ql_pt_frame(i, s), d = f > frame ? f - frame : frame - f;
        if (d < best_d) {
            best_d = d;
            best = s;
        }
    }
    if (best == 16u) {
        i++;
        best = 0;
    }
    *pi = i;
    *ps = best;
    return 0;
}

int quant_loops_grid_ok(int i)
{
    u32 base = C350_BPM_BASE_TENTHS;
    u32 n = quant_loops_window_n();
    int j;

    if (!ql_on() || C350_IS_USB_MODE != 1 || n == 0 || C350_GRID_WINDOW_OK == 0 || base == 0)
        return 0;
    for (j = i - 8; j <= i + 8; j++) {
        u32 t;
        if (j < 0 || (u32)j >= n)
            continue;
        t = (C350_GRID_BPM100(j) + 5u) / 10u;
        if (t == base || (t > base && base == QL_BPM_CLAMP_STOCK))
            return 1;
    }
    return 0;
}


/* ---- blocks ----
 *
 * The feeder plays one block of 84 stereo samples (0x150 bytes, 1.9 ms) per
 * interrupt; a frame (ring sector) is exactly 7 blocks. Grid ms -> blocks to
 * the nearest block: (ms * 21 + 20) / 40 (525 blocks a second). A loop's
 * length is kept exactly as num / den in 1/40 blocks: q/16 beats at a constant
 * BPMx100 is q * 7875000 / BPMx100, a tempo change inside the loop gives the
 * grid duration in ms * 21 / 1. */
#define QL_BLK(ms)          (((ms) * 21u + 20u) / 40u)
#define QL_BLK_REM(ms)      (((ms) * 21u + 20u) % 40u)
#define QL_OFF_GRID         17u      /* ql_step_of: the in is on no 1/16 step */
#define QL_Q_CONST_MAX      512u     /* q * 7875000 < 2^32 */
#define QL_MAX_Q            512u

/* A block loop: in block, its rounding residue a0 (the exact in is in_blk +
 * (a0 - 20) / 40 blocks) and the length num / den in 1/40 blocks. */
typedef struct {
    u32 in_blk, a0, num, den;
} ql_blk_t;

/* Frame of a grid point used as a loop in: the frame that holds its block, so
 * the stock entry (the in sector's start) is never after it. */
static __attribute__((noinline)) u32 ql_pt_in_frame(int i, u32 s)
{
    return QL_BLK(quant_loops_grid_ms(i, s)) / 7u;
}

/* The 1/16 step (0..16, 16 = beat i + 1) of beat i whose in frame is `in`,
 * else the one whose rounded frame is (a loop made before block loops); else
 * QL_OFF_GRID. */
static __attribute__((noinline)) u32 ql_step_of(int i, u32 in)
{
    u32 s;

    for (s = 0; s <= 16u; s++)
        if (ql_pt_in_frame(i, s) == in)
            return s;
    for (s = 0; s <= 16u; s++)
        if (ql_pt_frame(i, s) == in)
            return s;
    return QL_OFF_GRID;
}

/* Length of q/16 beats from step p0 of beat i, in 1/40 blocks: *num / *den. */
void quant_loops_block_len(int i, u32 p0, u32 q, u32 *num, u32 *den)
{
    u32 n = quant_loops_window_n(), bpm = C350_GRID_BPM100(i), last = (u32)i + (p0 + q + 15u) / 16u, j;

    for (j = (u32)i + 1u; j <= last && j < n; j++)
        if (C350_GRID_BPM100(j) != bpm)
            break;
    if (bpm != 0 && q <= QL_Q_CONST_MAX && (j > last || j >= n)) {
        *num = q * 7875000u;
        *den = bpm;
    } else {
        *num = (quant_loops_grid_ms(i, p0 + q) - quant_loops_grid_ms(i, p0)) * 21u;
        *den = 1u;
    }
}

/* Blocks of the first pass (from the in) of a loop: in + exact length, rounded. */
static u32 ql_len0(const ql_blk_t *b)
{
    return (b->num + b->a0 * b->den) / (40u * b->den);
}

/* The in of a block loop at grid point (i, s). */
static void ql_spec_point(ql_blk_t *b, int i, u32 s)
{
    u32 ms = quant_loops_grid_ms(i, s);

    b->in_blk = QL_BLK(ms);
    b->a0 = QL_BLK_REM(ms);
}

/* Block loop b: the in at grid point (i, s), q/16 beats long. */
static __attribute__((noinline)) void ql_point_loop(ql_blk_t *b, int i, u32 s, u32 q)
{
    ql_spec_point(b, i, s);
    quant_loops_block_len(i, s, q, &b->num, &b->den);
}

/* Stock out record of a block loop whose first pass ends at out block ob:
 * the frame that holds the pass's last block (the CDJ-350's stock wrap plays
 * the out frame whole and wraps when the play frame is past it, and its "out
 * frame reached" branch must still run every pass, see the top of the file). */
static u32 ql_out_rec(u32 ob)
{
    return (ob - 1u) / 7u;
}

/* Stock out record of block loop b. */
static __attribute__((noinline)) u32 ql_out_frame(const ql_blk_t *b)
{
    u32 l = b->in_blk + ql_len0(b);

    return ql_out_rec(l > b->in_blk ? l : b->in_blk + 1u);
}

/* The in of a loop whose in record holds frame `in`: the grid point that
 * frame names (ql_step_of), else the frame's first block. *pi = the beat at or
 * before in, *pp0 = the step (0 when off the grid). 2 = on the grid, 1 = off
 * it, 0 = the grid cannot say (no grid, another window, quantize off). */
static __attribute__((noinline)) int ql_in_spec(u32 in, ql_blk_t *b, int *pi, u32 *pp0)
{
    int i = quant_loops_beat_at_or_before(in);
    u32 s;

    if (i == QL_AWAY)
        return 0;
    if (i == QL_NONE)
        i = 0;
    if (!quant_loops_grid_ok(i))
        return 0;
    *pi = i;
    s = ql_step_of(i, in);
    if (s <= 16u) {
        ql_spec_point(b, i, s);
        *pp0 = s;
        return 2;
    }
    b->in_blk = in * 7u;
    b->a0 = 20u;
    *pp0 = 0;
    return 1;
}

/*
 * The auto beat loop on the grid: in = the quantize point (UTILITY Q SIZE)
 * nearest pos, behind or ahead of it (ahead: the deck plays on to it, then
 * loops), `beats` beats of grid time long from there. Fills *in (the frame
 * that holds the in block) and *out (the stock out record: the frame that
 * holds the first pass's last block), and the block loop in ql_ap. 0 when the
 * grid cannot say, or the playhead would be past the out.
 */
static ql_blk_t ql_ap;               /* the block loop of the last auto_points */
static ql_blk_t ql_tmp;              /* scratch (playertask) */

int quant_loops_auto_points(u32 pos, u32 beats, u32 *in, u32 *out)
{
    u32 fin, fout, s;
    int i;

    if (beats == 0 || beats > 16)
        return 0;
    if (ql_point_nearest(pos, ql_step(), &i, &s) < 0 || !quant_loops_grid_ok(i))
        return 0;
    ql_point_loop(&ql_ap, i, s, beats * 16u);
    fin = ql_ap.in_blk / 7u;
    fout = ql_out_frame(&ql_ap);
    if (fout < fin)
        fout = fin;
    if (fout <= pos)
        return 0;
    *in = fin;
    *out = fout;
    return 1;
}

/* LOOP IN: the quantize point nearest pos as a loop in frame (the frame that
 * holds its block), or pos when the grid cannot say. */
u32 quant_loops_in_point(u32 pos)
{
    int i;
    u32 s;

    if (ql_point_nearest(pos, ql_step(), &i, &s) < 0 || !quant_loops_grid_ok(i))
        return pos;
    return ql_pt_in_frame(i, s);
}

/* LOOP OUT: the quantize point nearest pos, or the first one after the loop
 * in when that is not after it; as the stock out record of the block loop
 * from the in (ql_op, ql_op_ok). pos when the grid cannot say. The length is
 * the grid's from the in point (in on the grid), else from the in frame's
 * first block to the point's block. */
static ql_blk_t ql_op;
static u8 ql_op_ok;
static u8 ql_op_on;                  /* 2: ql_op's in is on the grid */

static __attribute__((noinline)) u32 ql_op_close(int k, u32 s, u32 p0, u32 pos)
{
    u32 step = ql_step();

    while (QL_BLK(quant_loops_grid_ms(k, s)) <= ql_op.in_blk) {
        s += step;                   /* the next point after in */
        if (s >= p0 + 64u)
            return pos;
    }
    if (ql_op_on == 2u) {
        quant_loops_block_len(k, p0, s - p0, &ql_op.num, &ql_op.den);
    } else {
        ql_op.num = (QL_BLK(quant_loops_grid_ms(k, s)) - ql_op.in_blk) * 40u;
        ql_op.den = 1u;
    }
    ql_op_ok = 1;
    return ql_out_frame(&ql_op);
}

u32 quant_loops_out_point(u32 in, u32 pos)
{
    u32 step = ql_step(), s, p0;
    int i, k, on;

    ql_op_ok = 0;
    if (ql_point_nearest(pos, step, &i, &s) < 0 || !quant_loops_grid_ok(i))
        return pos;
    on = ql_in_spec(in, &ql_op, &k, &p0);
    if (!on)
        return pos;
    s = i < k ? 0u : s + (u32)(i - k) * 16u;   /* the point, from beat k */
    ql_op_on = (u8)on;
    return ql_op_close(k, s, p0, pos);
}

/* The ring sector that holds `frame`, found from sector s (which holds
 * frame `pos`); -1 if that slot does not hold it or it is another track. */
int quant_loops_sector_of(int s, u32 pos, u32 frame)
{
    int t = c350_ring_add(s, (int)(frame - pos));
    if (t < 0 || t >= 2400)
        return -1;
    if (c350_ring_sector_to_frame(t) != frame || c350_sector_track(t) != c350_sector_track(s))
        return -1;
    return t;
}

/* ---- the ring: position and the in-ring jump (feeder ISR context) ---- */

#define QL_STOCK_OUT_REACHED C350_FN(0x0C0161C8, int, (int sector, u32 out, int z))
#define QL_CLEAR_SOURCE      C350_FN(0x0C01684E, void, (void))     /* LOOP DIVIDE source 0x0C7E5034 = -1 */
#define QL_RING_TRIM_USB     C350_FN(0x0C00B71C, int, (int track))  /* the CD path's is 0x0C00B5F4 */
#define QL_SHORT_LOOP        C350_FN(0x0C00B4A4, u32, (void))      /* out - in <= 1125 frames */
#define QL_BEAT_START        C350_FN(0x0C016474, void, (void))     /* beat_lamp_update */
#define QL_BEAT_STARTED      C350_U32(0x0C1AB7D4)
#define QL_WRAP_FLAG_A       C350_U8(0x0C7E4FB0)
#define QL_WRAP_FLAG_B       C350_U8(0x0C1D4CFD)
#define QL_TRIMMED           C350_U8(0x0C7E4F2D)

static u32 ql_mod(s32 v)
{
    while (v < 0)
        v += (s32)C350_PCM_RING_SLOTS;
    while (v >= (s32)C350_PCM_RING_SLOTS)
        v -= (s32)C350_PCM_RING_SLOTS;
    return (u32)v;
}

/* Position (blocks: frame * 7 + block in the sector) of the block the read
 * pointer names. Read from the pointer each time: any jump is seen at once. */
u32 quant_loops_read_pos(void)
{
    u32 s = (u32)C350_RING_READ_INDEX;
    u32 off = C350_PCM_READ_PTR - (C350_PCM_RING_BASE + s * C350_PCM_SECTOR_BYTES), b = 0;

    while (off >= C350_PCM_BLOCK_BYTES && b < C350_BLOCKS_PER_FRAME - 1u) {
        off -= C350_PCM_BLOCK_BYTES;
        b++;
    }
    return c350_ring_sector_to_frame((int)s) * C350_BLOCKS_PER_FRAME + b;
}

/* Move the read pointer to block `pos` when that audio is in the PCM ring:
 * ahead up to two sectors before the write index, behind no further than the
 * oldest sector the ring keeps, same track, and the sector table confirms the
 * frame. Then the next block is fetched from the pointer (as after a stock
 * wrap). 1 = moved, 0 = not in the ring (nothing changed). Stays on 336-byte
 * boundaries from the ring base (the feeder's rule). */
int quant_loops_ring_jump(u32 pos)
{
    u32 f = pos / C350_BLOCKS_PER_FRAME, sub = pos - f * C350_BLOCKS_PER_FRAME;
    u32 cur = (u32)C350_RING_READ_INDEX, fcur = c350_ring_sector_to_frame((int)cur);
    u32 s;

    if (f >= fcur) {
        u32 ahead = f - fcur, have = ql_mod((s32)C350_RING_WRITE_INDEX - (s32)cur);
        if (ahead + 2u > have)
            return 0;
        s = ql_mod((s32)(cur + ahead));
    } else {
        u32 back = fcur - f, kept = ql_mod((s32)cur - (s32)C350_RING_OLDEST);
        if (back > kept)
            return 0;
        s = ql_mod((s32)cur - (s32)back);
    }
    if (c350_ring_sector_to_frame((int)s) != f || c350_sector_track((int)s) != c350_sector_track((int)cur))
        return 0;
    C350_PCM_READ_PTR = C350_PCM_RING_BASE + s * C350_PCM_SECTOR_BYTES + sub * C350_PCM_BLOCK_BYTES;
    C350_RING_READ_INDEX = (u16)s;
    QL_CLEAR_SOURCE();
    return 1;
}

/* What the stock wrap does after its jump in USB mode (feeder ISR
 * 0x0C06ECC4..0x0C06ECF4): trim the fill ahead of the read pointer (the jump
 * back left more than 1200 sectors ahead) and note it, and start LOOP
 * DIVIDE's beat module for a short loop that has not started it. */
static void ql_wrap_tail(void)
{
    if ((s8)QL_RING_TRIM_USB(0) == 1)
        QL_TRIMMED = 1u;
    if (QL_BEAT_STARTED == 0u && C350_AUDIO_LOOP_PLAYING && (QL_SHORT_LOOP() & 0xFFu))
        QL_BEAT_START();
}

/* ---- the block loop: state the ISR hook owns the wraps with ----
 *
 * Written by playertask (qb_arm, the tick, the UTILITY switch), read by the
 * ISR hook, which also moves qb_out_blk / qb_acc at each wrap. playertask
 * clears qb_armed before it writes the rest and sets it last. */
#define QB_CONT   0xFFFFFFFFu        /* qb_last: the next block continues the play (no entry) */
#define QB_ENTRY  0xFFFFFFFEu        /* qb_last: the next block may be a stock entry at the in sector */
#define QB_EXPECT_TICKS 50u          /* player cycles the audio task may take to apply a loop */

static volatile u8 qb_miss;          /* the ring did not hold the in: the stock wraps this pass */
static volatile u8 qb_skip;          /* the pass under way is not a whole one (not measured) */
static volatile u8 qb_wrapped;       /* the loop wrapped since it was armed */
static volatile u32 qb_in_frame;     /* stock in record of the loop (frame) */
static volatile u32 qb_out_frame;    /* stock out record it was set with */
static volatile u32 qb_in_blk, qb_a0, qb_num, qb_den, qb_out_blk, qb_acc;
static volatile u32 qb_last;         /* block position of the last call, or QB_CONT / QB_ENTRY */
static volatile u32 qb_n;            /* blocks played in this pass */
static u8 qb_expect, qb_expect_ticks;
static u32 qb_seen_in, qb_seen_out;  /* the loop the tick last looked at */

/* Evidence for tests. */
volatile u32 quant_loops_blk_arms;       /* block loops set (writers and the tick) */
volatile u32 quant_loops_blk_derived;    /* of those, found by the tick */
volatile u32 quant_loops_blk_calls;      /* owned ISR calls */
volatile u32 quant_loops_blk_wraps;      /* wraps the hook made */
volatile u32 quant_loops_blk_realigns;   /* entries moved from the in sector start to the in block */
volatile u32 quant_loops_blk_misses;     /* wraps left to the stock test (the in not in the ring) */
volatile u32 quant_loops_blk_passes;     /* whole passes measured since the loop was armed */
volatile u32 quant_loops_blk_len_last;   /* blocks of the last whole pass */
volatile u32 quant_loops_blk_len_min;
volatile u32 quant_loops_blk_len_max;
volatile u32 quant_loops_blk_len_sum;    /* blocks of the whole passes */
volatile u32 quant_loops_blk_blocks;     /* blocks played since the first wrap */
volatile u32 quant_loops_blk_tick0;      /* 1 ms tick at the first wrap */
volatile u32 quant_loops_blk_tick;       /* 1 ms tick at the last owned call */
volatile u32 quant_loops_blk_pos;        /* block position at the last owned call */
volatile u32 quant_loops_blk_in;         /* the armed loop: in block, its residue, first out block, */
volatile u32 quant_loops_blk_a0;         /* length num / den in 1/40 blocks */
volatile u32 quant_loops_blk_out0;
volatile u32 quant_loops_blk_next;       /* out block of the pass under way */
volatile u32 quant_loops_blk_num;
volatile u32 quant_loops_blk_den;

/* Arm the block loop b with stock in record in_frame; `last` = QB_CONT (the
 * play continues into it) or QB_ENTRY (the stock may have just entered it at
 * the in sector). Returns the stock out record of its first pass. */
static __attribute__((noinline)) u32 qb_arm(const ql_blk_t *b, u32 in_frame, u32 last)
{
    u32 d = 40u * b->den, n = b->num + b->a0 * b->den;

    qb_armed = 0;
    qb_in_frame = in_frame;
    qb_in_blk = b->in_blk;
    qb_a0 = b->a0;
    qb_num = b->num;
    qb_den = b->den;
    qb_out_blk = b->in_blk + n / d;
    if (qb_out_blk <= qb_in_blk)
        qb_out_blk = qb_in_blk + 1u;
    qb_acc = n % d;
    qb_out_frame = ql_out_rec(qb_out_blk);
    qb_last = last;
    qb_n = 0;
    qb_miss = 0;
    qb_skip = 1;                     /* the first pass starts wherever the play is */
    qb_wrapped = 0;
    quant_loops_blk_passes = 0;
    quant_loops_blk_len_sum = 0;
    quant_loops_blk_len_min = 0xFFFFFFFFu;
    quant_loops_blk_len_max = 0;
    quant_loops_blk_blocks = 0;
    quant_loops_blk_in = b->in_blk;
    quant_loops_blk_a0 = b->a0;
    quant_loops_blk_out0 = qb_out_blk;
    quant_loops_blk_next = qb_out_blk;
    quant_loops_blk_num = b->num;
    quant_loops_blk_den = b->den;
    quant_loops_blk_arms++;
    qb_armed = 1;
    return qb_out_frame;
}

/* Every writer here (auto loop, LOOP OUT) arms a loop through this: the block
 * loop [in_blk (residue a0), + num/den 1/40 blocks) whose in record is the
 * frame in_blk / 7, the play continuing into it. The tick then waits up to
 * QB_EXPECT_TICKS cycles for the audio task to apply the stock records.
 * Returns the stock out record. */
u32 quant_loops_set_block_loop(u32 in_blk, u32 a0, u32 num, u32 den)
{
    ql_blk_t b;
    u32 out;

    b.in_blk = in_blk;
    b.a0 = a0;
    b.num = num;
    b.den = den;
    out = qb_arm(&b, in_blk / 7u, QB_CONT);
    qb_expect = 1;
    qb_expect_ticks = 0;
    return out;
}

static __attribute__((noinline)) u32 qb_set(const ql_blk_t *b)
{
    return quant_loops_set_block_loop(b->in_blk, b->a0, b->num, b->den);
}

/* Once per block while a loop plays forward (the feeder ISR, after it moved
 * the read pointer to the next block): owns the wraps of the armed loop. At
 * out_blk it moves the read pointer to the in block plus the overshoot
 * (normally 0) and returns 0 (no stock wrap); before that it returns 0 as
 * well. -1 = not ours (the stock test decides): nothing armed, another loop,
 * not USB data (0x0C7E4F78), or the in not in the ring (the stock wrap, frame precise,
 * re-reads it). Interrupt context: no RTOS calls. */
int quant_loops_out_hook(int sector, u32 out, int z)
{
    u32 pos, last, f0, len, over, n, d;

    if (!qb_armed || C350_LOOP_IN_FRAME != qb_in_frame || C350_LOOP_ACTIVE != 1 ||
        C350_PLAYER_STATE != C350_STATE_LOOPING || C350_AUDIO_CD_MODE != 0u ||
        C350_AUDIO_ALT_FILL != 0u)
        return -1;
    pos = quant_loops_read_pos();
    last = qb_last;
    qb_last = pos;
    quant_loops_blk_calls++;
    quant_loops_blk_pos = pos;
    quant_loops_blk_tick = C350_TICK_MS;
    if (qb_wrapped)
        quant_loops_blk_blocks++;
    qb_n++;
    if (last != QB_CONT && pos != last + 1u) {
        /* Not the next block: someone else moved the pointer. The stock
         * entries (RELOOP) land on the in sector's start, up to 6 blocks
         * before the in block: move on to the in block (plus what has played
         * since) and start the passes from the in. */
        f0 = qb_in_frame * 7u;
        qb_miss = 0;
        qb_skip = 1;
        if (pos >= f0 && pos < f0 + 7u) {
            u32 t;

            n = qb_num + qb_a0 * qb_den;
            d = 40u * qb_den;
            qb_out_blk = qb_in_blk + n / d;
            qb_acc = n % d;
            quant_loops_blk_next = qb_out_blk;
            qb_n = pos - f0;
            qb_skip = 0;
            t = qb_in_blk + qb_n;
            if (t != pos && t < qb_out_blk && quant_loops_ring_jump(t)) {
                qb_last = t;
                quant_loops_blk_realigns++;
                return 0;
            }
        }
    }
    if (pos < qb_out_blk) {
        /* The stock test for its side effects only: at the out frame it notes
         * the out record's ring sector for the beat module. Its verdict is
         * ignored: a pass of the Bresenham sequence may end one block into
         * the frame after the out record, where stock would wrap early. */
        (void)QL_STOCK_OUT_REACHED(sector, out, z);
        return 0;
    }
    if (qb_miss)
        return -1;
    len = qb_out_blk - qb_in_blk;
    over = pos - qb_out_blk;
    if (over >= len)
        over %= len;
    QL_WRAP_FLAG_A = 0u;             /* as the stock wrap writes them */
    QL_WRAP_FLAG_B = 1u;
    if (!quant_loops_ring_jump(qb_in_blk + over)) {
        qb_miss = 1;                 /* this pass wraps stock, frame precise */
        quant_loops_blk_misses++;
        return -1;
    }
    ql_wrap_tail();
    qb_last = qb_in_blk + over;
    if (!qb_skip) {
        u32 k = qb_n;

        quant_loops_blk_len_last = k;
        quant_loops_blk_len_sum += k;
        if (k < quant_loops_blk_len_min)
            quant_loops_blk_len_min = k;
        if (k > quant_loops_blk_len_max)
            quant_loops_blk_len_max = k;
        quant_loops_blk_passes++;
    }
    qb_skip = 0;
    qb_n = 0;
    if (!qb_wrapped) {
        qb_wrapped = 1;
        quant_loops_blk_tick0 = quant_loops_blk_tick;
        quant_loops_blk_blocks = 0;
    }
    /* the next pass: Bresenham on the exact length */
    n = qb_num + qb_acc;
    d = 40u * qb_den;
    qb_acc = n % d;
    qb_out_blk = qb_in_blk + n / d;
    quant_loops_blk_next = qb_out_blk;
    quant_loops_blk_wraps++;
    return 0;
}

/* Pool 0x0C06EDB0 (read at 0x0C06EBF6 in the feeder ISR 0x0C06E118, forward
 * loop-out test): the block loop, else the stock test. The other loop-out
 * word (0x0C06E72C, the path that can play in reverse) stays stock. */
int quant_loops_out_isr(int sector, u32 out, int z)
{
    int r = quant_loops_out_hook(sector, out, z);

    if (r < 0)
        r = QL_STOCK_OUT_REACHED(sector, out, z);
    return r;
}

/* ---- the state-4 tick: which loop the block path owns ---- */

static u32 ql_tz(u32 q)
{
    u32 n = 0;

    while (q && !(q & 1u)) {
        q >>= 1;
        n++;
    }
    return n;
}

static __attribute__((noinline)) u32 ql_out_blk_q(ql_blk_t *b, int i, u32 p0, u32 q)
{
    quant_loops_block_len(i, p0, q, &b->num, &b->den);
    return b->in_blk + ql_len0(b);
}

static ql_blk_t ql_fq;                /* scratch of quant_loops_find_q */

/* q (1/16 beats, 1..QL_MAX_Q) of the loop from in spec (in_blk, a0; beat i,
 * step p0) whose stock out record is `out`: the out block of its first pass
 * within a frame of 7 * out + 2 (block loops' records name the frame of the
 * last block, 7 out + 1 .. 7 out + 7; loops made before block loops rounded
 * their out, 7 out +- 3.5); two neighbours: the closer, then the one with
 * more factors of two. 0 = none. */
u32 quant_loops_find_q(u32 in_blk, u32 a0, int i, u32 p0, u32 out)
{
    ql_blk_t *b = &ql_fq;
    u32 lo = 1, hi = QL_MAX_Q, q, best = 0, d_best = 8u, want = out * 7u + 2u, ob, d;

    b->in_blk = in_blk;
    b->a0 = a0;
    b->num = 0;
    b->den = 1;
    if (want <= in_blk)
        return 0;
    while (lo < hi) {                 /* first q whose out block is >= want */
        u32 mid = (lo + hi) >> 1;
        if (ql_out_blk_q(b, i, p0, mid) < want)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (q = lo > 1u ? lo - 1u : lo; q <= lo; q++) {
        ob = ql_out_blk_q(b, i, p0, q);
        d = ob > want ? ob - want : want - ob;
        if (d < d_best || (d == d_best && best && ql_tz(q) > ql_tz(best))) {
            d_best = d;
            best = q;
        }
    }
    return best;
}

volatile u32 quant_loops_ticks;          /* tick calls */

/* The block loop of the stock loop [in, out] frames, armed with an entry; 0
 * when the grid cannot name it. */
static __attribute__((noinline)) int qb_derive(u32 in, u32 out)
{
    ql_blk_t *b = &ql_tmp;
    u32 p0, q;
    int i;

    if (out <= in || !ql_in_spec(in, b, &i, &p0))
        return 0;
    q = quant_loops_find_q(b->in_blk, b->a0, i, p0, out);
    if (!q)
        return 0;
    quant_loops_block_len(i, p0, q, &b->num, &b->den);
    qb_arm(b, in, QB_ENTRY);
    qb_out_frame = out;              /* the loop's own stock out, not ours */
    quant_loops_blk_derived++;
    return 1;
}

/* Once per player cycle while a loop plays (state 4), before the stock
 * handler. A loop no writer here armed (RELOOP of a loop made with quantize
 * off, an out changed elsewhere) is looked up on the grid from its stock
 * records. Not found, quantize off, no grid: the loop stays stock. */
void quant_loops_loop_tick(void)
{
    u32 in, out;

    quant_loops_ticks++;
    if (!ql_on() || C350_IS_USB_MODE != 1 || C350_LOOP_ACTIVE != 1 ||
        C350_PLAYER_STATE != C350_STATE_LOOPING) {
        qb_armed = 0;
        qb_expect = 0;
        return;
    }
    in = C350_LOOP_IN_FRAME;
    out = C350_LOOP_OUT_RECORD;
    if (qb_armed && in == qb_in_frame && out == qb_out_frame) {
        qb_expect = 0;               /* the stock records show the armed loop */
        qb_seen_in = in;
        qb_seen_out = out;
        return;
    }
    if (qb_expect) {                 /* a writer armed it: the audio task applies it later */
        if (++qb_expect_ticks < QB_EXPECT_TICKS) {
            qb_seen_in = in;
            qb_seen_out = out;
            return;
        }
        qb_expect = 0;
    }
    if (in == qb_seen_in && out == qb_seen_out)
        return;                      /* looked at already: stock */
    qb_seen_in = in;
    qb_seen_out = out;
    if (!qb_derive(in, out))
        qb_armed = 0;
}

/* Pool 0x0C0C7F7C (keys_state4_looping 0x0C0D28AC, read only at 0x0C0C7D92
 * in player_key_dispatch_by_state, state 4). */
void quant_loops_tick_entry(void)
{
    quant_loops_loop_tick();
    c350_state4_keys();
}

/* ---- redirect entry points ---- */

/* Pending block loop of the auto loop being set. */
static ql_blk_t ql_pend;
static u8 ql_pend_blk;

/* The pending loop with its in at frame pos (the grid point that frame
 * names, else the frame), the same length; its stock out record. */
static __attribute__((noinline)) u32 ql_pend_at(u32 pos)
{
    u32 p0;
    int i;

    if (ql_in_spec(pos, &ql_tmp, &i, &p0)) {
        ql_pend.in_blk = ql_tmp.in_blk;
        ql_pend.a0 = ql_tmp.a0;
    } else {
        ql_pend.in_blk = pos * 7u;
        ql_pend.a0 = 20u;
    }
    return ql_out_frame(&ql_pend);
}

/* Pool 0x0C0CE8B4 (0x0C006A92) in auto_beat_loop_set (0x0C0CDE6E), called
 * before the out calculation (0x0C0CE696) and after audio_send_cmd (0x0C0CE848). */
void quant_loops_auto_in(void)
{
    int s;
    u32 pos, in, out, tracklen, slot;

    if (ql_post_send) {
        ql_post_send = 0;
        if (ql_in_sector >= 0) {
            C350_IN_SECTOR = ql_in_sector;
            ql_in_sector = -1;
        }
        return;
    }
    c350_next_sector();
    ql_pending = 0;
    ql_pend_blk = 0;
    ql_in_sector = -1;
    if (C350_IS_USB_MODE != 1 || !ql_on())
        return;
    s = C350_IN_SECTOR;
    if (s < 0 || s >= 2400)
        return;
    pos = c350_ring_sector_to_frame(s);
    slot = c350_sector_track(s);
    if (!quant_loops_auto_points(pos, 4u, &in, &out))
        return;
    ql_pend = ql_ap;
    tracklen = C350_TRACKLEN(slot & 3u);
    if (in != pos) {
        int t = quant_loops_sector_of(s, pos, in);
        if (t >= 0) {
            C350_IN_SECTOR = (s16)t;
            ql_in_sector = (s16)t;
            quant_loops_in_count++;
        } else {
            quant_loops_refused++;
            in = pos;
            out = ql_pend_at(pos);
        }
    }
    ql_pend_blk = 1;
    if (tracklen != 0 && out >= tracklen) {
        out = tracklen - 1u;         /* the track ends inside the loop: frames, stock wraps */
        ql_pend_blk = 0;
    }
    if (out < pos || out < in)
        return;
    ql_pending_out = out;
    ql_pending = 1;
    quant_loops_last_pos = pos;
    quant_loops_last_in = in;
}

/* Pool 0x0C0CEBCC (audio_send_cmd) in auto_beat_loop_set (0x0C0CDE6E). */
void quant_loops_auto_send(int cmd, int flag, u32 frame, int resize, int a5, int a6, int a7, int src)
{
    if ((cmd & 0xFF) == 0x1E && (resize & 0xFF) == 0) {
        if (ql_pending) {
            ql_pending = 0;
            if (ql_in_sector >= 0)
                C350_IN_SECTOR = ql_in_sector;
            ql_post_send = 1;
            frame = ql_pending_out;
            C350_LOOP_OUT_CMD = frame;
            C350_LOOP_D1C0 = frame;
            quant_loops_auto_count++;
            quant_loops_last_out = frame;
            if (ql_pend_blk)
                qb_set(&ql_pend);
        } else if (C350_IS_USB_MODE == 1 && ql_on() && C350_PLAYER_STATE == C350_STATE_CUED) {
            /* state 6 (cued): the in is the cue itself, only the length
             * comes from the grid */
            u32 pos = C350_CUE_FRAME, in, out, tracklen;
            if (quant_loops_auto_points(pos, 4u, &in, &out)) {
                ql_pend = ql_ap;
                out = ql_pend_at(pos);
                tracklen = C350_TRACKLEN(C350_CUE_TRACK & 3u);
                if (out >= pos && (tracklen == 0 || out < tracklen)) {
                    frame = out;
                    C350_LOOP_OUT_CMD = frame;
                    C350_LOOP_D1C0 = frame;
                    quant_loops_auto_count++;
                    quant_loops_last_pos = pos;
                    quant_loops_last_in = pos;
                    quant_loops_last_out = frame;
                    qb_set(&ql_pend);
                }
            }
        }
    }
    c350_audio_send_cmd(cmd, flag, frame, resize, a5, a6, a7, src);
}

/* Pools 0x0C0D1380, 0x0C0D2728, 0x0C0D31B8 (0x0C006A92) in the LOOP IN paths. */
void quant_loops_loop_in(void)
{
    int s, t;
    u32 pos, in;

    c350_next_sector();
    if (C350_IS_USB_MODE != 1 || !ql_on())
        return;
    s = C350_IN_SECTOR;
    if (s < 0 || s >= 2400)
        return;
    pos = c350_ring_sector_to_frame(s);
    in = quant_loops_in_point(pos);
    quant_loops_last_pos = pos;
    quant_loops_last_in = in;
    if (in == pos)
        return;
    t = quant_loops_sector_of(s, pos, in);
    if (t < 0) {
        quant_loops_refused++;
        quant_loops_last_in = pos;
        return;
    }
    C350_IN_SECTOR = (s16)t;
    quant_loops_in_count++;
}

/* Pools 0x0C0D1688, 0x0C0D1984 and 0x0C0D1BB8 (audio_send_cmd) in the LOOP
 * OUT paths. */
void quant_loops_loop_out(int cmd, int flag, u32 a3, int mode, int sector, int a6, int a7, int src)
{
    if ((cmd & 0xFF) == 1 && (mode & 0xFF) == 3 && C350_IS_USB_MODE == 1 && ql_on()) {
        int s = sector & 0xFFFF, t = -1;
        u32 pos, in, out;
        if (s < 2400) {
            pos = c350_ring_sector_to_frame(s);
            in = C350_LOOP_IN_FRAME;
            out = quant_loops_out_point(in, pos);
            quant_loops_last_pos = pos;
            quant_loops_last_out = out;
            if (out != pos) {
                t = quant_loops_sector_of(s, pos, out);
                if (t >= 0) {
                    sector = t;
                    quant_loops_out_count++;
                } else {
                    quant_loops_refused++;
                    quant_loops_last_out = pos;
                }
            }
            if (ql_op_ok && (out == pos || t >= 0))
                qb_set(&ql_op);
        }
    }
    c350_audio_send_cmd(cmd, flag, a3, mode, sector, a6, a7, src);
}
