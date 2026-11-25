/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * quant_loops: loops snap to the rekordbox beat grid (see ../patch.py and
 * docs/cdj850-modding.md section 4b.9).
 *
 * Only when a beat grid of the loaded USB track is in RAM; otherwise every
 * path below hands the stock values through unchanged.
 *
 * Where the loop points come from (MAIN 1.13, traced):
 *   loop in   the audio task builds the loop-in record (0x0C7F6498, frame,
 *             MSF, ring sector) from the ring sector in s16 0x0C7F6468 when
 *             it handles audio cmd 6 (auto beat loop) or cmd 7/2 (LOOP IN):
 *             FUN_0C009DB4 -> 0x0C0F01DC. DjControlTask writes that sector
 *             just before (FUN_0C0059D2: the next sector the feeder plays).
 *   auto out  audio_send_cmd(6, 0, out_frame, 0, ...) from auto_beat_loop_set.
 *   LOOP OUT  audio_send_cmd(1, 1|2, 0, 3, out_sector, ...): the audio task's
 *             cmd 1 handler takes param5 as the out ring sector (0x0C007124..,
 *             out record 0x0C7F64B0 from 0x0C0F025A); stock passes the
 *             current play sector 0x0C7F645C.
 *
 * Quantize switch: BEAT SELECT + LOOP IN toggles it (the mod flags byte of
 * c850_ui.h, saved to flash, default on) and flashes QUANTIZE ON / QUANTIZE
 * OFF on the display. Off: every path below is stock (loop_resize also
 * leaves the grid then).
 *
 * Entry points (all redirected pool words, see ../patch.py):
 *   quant_loops_auto_in    FUN_0C073954 (track -> tracklen slot) in the USB
 *                          new-loop path of auto_beat_loop_set: runs after
 *                          D1C0 (in frame) and 0x0C7F6468 are set and before
 *                          beat_loop_out_from_bpm. Snaps the in point to the
 *                          nearest beat (behind or ahead) and computes the grid out.
 *   quant_loops_auto_send  audio_send_cmd for the sends of auto_beat_loop_set:
 *                          puts the grid out into the cmd 6 new-loop request
 *                          (D1C0, CA90) and arms the block loop; a resize
 *                          (cmd 6, resize flag: BEAT buttons in a loop,
 *                          loop_resize) gets the block loop of the new length.
 *   quant_loops_auto_keep  FUN_0C0059D2 after that send: keeps the snapped
 *                          in sector (stock refreshes it before the audio
 *                          task reads it).
 *   quant_loops_loop_in    FUN_0C0059D2 in the LOOP IN path: snaps 0x0C7F6468
 *                          to the nearest beat.
 *   quant_loops_loop_out   audio_send_cmd in the USB LOOP OUT path: snaps the
 *                          out sector of cmd 1 / 3 to the nearest beat after
 *                          in and arms the block loop.
 *   c850_ui_keys           0x0C0ED038, the shared key filter chain
 *                          (payload/common/c850_ui.c): quant_loops_key_filter.
 *   c850_ui_flash_hook     tail of display_stage_bpm: the QUANTIZE ON/OFF flash.
 *   quant_loops_loop_tick  hook of the shared state-4 tick c850_loop_tick
 *                          (0x0C0D7B98, keys_state4_looping; c850.h,
 *                          loop_tick.c): finds the block loop of a loop no
 *                          writer here set (memory, hot-cue, RELOOP).
 *   quant_loops_out_hook   hook of the shared loop-out dispatcher
 *                          c850_loop_out_isr (pool 0x0C07730C, IRQ7;
 *                          payload/common/c850_loop_out.c): the block-precise
 *                          wrap.
 *
 * Precision: blocks of 84 samples (1/7 frame, 1.9 ms). Loop in and out come
 * from the grid in ms (ms -> blocks: (ms * 21 + 20) / 40), never via frames;
 * the length is kept exactly (num / den in 1/40 blocks) and each wrap takes
 * the next pass from a Bresenham remainder, so every pass is within a block
 * of the grid length and the average is exact. The stock loop records stay
 * frames around the block loop: in = the frame that holds the in block, out =
 * the first frame at or after the out block. Where the block path cannot run
 * (no grid, quantize off, the in no longer in the ring, reverse) the loop is
 * stock, frame precise. docs/cdj850-block-loops-design.md has the design.
 *
 * Integer only (no FPU). Products stay below 2^32: ms * 75 for ms < 57e6
 * (15.9 h), ms * 21, q * 7875000 for q <= 512 (32 beats).
 */
#include "c850.h"
#include "c850_ui.h"

/* ---- MAIN state and routines used here (section 4b.2 / 4b.9) ---- */
#define QL_IN_SECTOR        C850_S16(0x0C7F6468)   /* ring sector the loop in is taken from */
#define QL_PLAY_SECTOR      C850_U16(0x0C7F645C)   /* current ring read sector */
#define QL_CA90             C850_U32(0x0CE6CA90)   /* 8-beat out: reference of the beat resize */
#define QL_GRID_WINDOW_OK   C850_U32(C850_GRID_CTX + 0x70)
#define QL_TRACKLEN(slot)   C850_U32(0x0CE59B58 + (slot) * 0x3A4)  /* as auto_beat_loop_set reads it */
#define QL_RING_ADD         C850_FN(0x0C008294, int, (int sector, int delta))
#define QL_SECTOR_FRAME     c850_ring_sector_to_frame
#define QL_SECTOR_TRACK     C850_FN(0x0C00586C, u32, (int sector))
#define QL_TRACK_SLOT       C850_FN(0x0C073954, u32, (u32 track))
#define QL_NEXT_SECTOR      C850_FN(0x0C0059D2, void, (void))
#define QL_BPM_CLAMP_STOCK  1800                   /* FUN_0C073788 clamp (180.0) */
#define QL_BPM_CLAMP_BPM999 9990                   /* with mods/bpm999 */

#define QL_NONE   (-1)      /* no beat (lead-in before the first beat) */
#define QL_AWAY   (-2)      /* the window in RAM does not cover the position */

/* Run-time switch. 1 = the mod may snap (the build knob CDJ850_QUANT_LOOPS
 * turns the whole mod on); 0 = stock and no toggle. Snapping also needs the
 * on-deck quantize switch (ql_on). */
volatile u32 quant_loops_on = 1;

volatile u32 quant_loops_toggles;    /* evidence: BEAT SELECT + LOOP IN presses that switched */
volatile u32 quant_loops_toggle_failed;  /* presses that could not save (31-character name) */
static volatile u8 qb_armed;         /* the block loop (quant_loops_out_hook) owns the wraps */

static int ql_on(void)
{
    return quant_loops_on && c850_ui_quantize();
}

/* Called by c850_ui_keys (the shared filter on pool 0x0C0ED038) with the
 * true held words k, the new presses p, and the keys taken so far. BEAT
 * SELECT + LOOP IN switches quantize; LOOP IN alone stays stock. */
static void quant_loops_key_filter(const u16 *k, const u16 *p, u16 *own)
{
    int on;

    if (!quant_loops_on || !c850_ui_shift(k, p, own, C850_K0_LOOP_IN))
        return;
    on = !c850_ui_quantize();
    if (c850_ui_set_flag(C850_UI_FLAG_QUANT_OFF, !on)) {
        quant_loops_toggles++;
        if (!on)
            qb_armed = 0;            /* quantize off: the loop wraps stock from now on */
    } else {
        quant_loops_toggle_failed++;
        on = !on;
    }
    c850_ui_flash(on ? "QUANTIZE ON" : "QUANTIZE OFF");
}
C850_UI_KEY_FILTER(quant_loops_key_filter);

/* Pending grid out of the auto loop being set (quant_loops_auto_in ->
 * quant_loops_auto_send); consumed by the next send through the redirect. */
static u32 ql_pending;
static u32 ql_keep_sector;     /* the snapped in sector must survive the post-send refresh */
static s16 ql_in_sector;       /* that sector */
static u32 ql_pending_out;
static u32 ql_pending_out8;

/* Evidence for tests: last values and counters. */
volatile u32 quant_loops_last_in;        /* last snapped in frame (auto / LOOP IN) */
volatile u32 quant_loops_last_out;       /* last out frame (auto / LOOP OUT) */
volatile u32 quant_loops_last_pos;       /* stock position the snap started from */
volatile u32 quant_loops_auto_count;     /* auto loops given a grid out */
volatile u32 quant_loops_in_count;       /* in points moved (auto or LOOP IN) */
volatile u32 quant_loops_out_count;      /* LOOP OUT sectors moved */
volatile u32 quant_loops_refused;        /* snaps refused (sector not in the ring, track change) */
volatile u32 quant_loops_resizes;        /* resizes given a block loop */

/* ---- frames and the grid window ---- */

/* Grid ms -> frames, to the nearest frame (half_frame would plug in here). */
u32 quant_loops_ms_to_frames(u32 ms)
{
    return (ms * 75u + 500u) / 1000u;
}

/* Entries of the window in RAM (0 if there is none). */
u32 quant_loops_window_n(void)
{
    u32 total = C850_GRID_BEAT_COUNT;
    u32 first = (u32)C850_GRID_WINDOW_INDEX * 500u;
    u32 n;

    if (total <= first)
        return 0;
    n = total - first;
    return n > 1000u ? 1000u : n;
}

/* Does the window end before the grid does (another window holds more)? */
static int ql_more_after(u32 n)
{
    return (u32)C850_GRID_WINDOW_INDEX * 500u + n < C850_GRID_BEAT_COUNT;
}

u32 quant_loops_beat_frame(int i)
{
    return quant_loops_ms_to_frames(C850_GRID_TIME_MS(i));
}

/* Index (in the window) of the last beat whose frame is <= `frame`;
 * QL_NONE before the first beat of the grid, QL_AWAY when the window in RAM
 * cannot say (the position lies in another window). Read-only: never pages
 * the window (grid_find_beat would, from the file). */
int quant_loops_beat_at_or_before(u32 frame)
{
    u32 n = quant_loops_window_n();
    u32 lo, hi;

    if (n == 0)
        return QL_AWAY;
    if (frame < quant_loops_beat_frame(0))
        return C850_GRID_WINDOW_INDEX == 0 ? QL_NONE : QL_AWAY;
    if (frame >= quant_loops_beat_frame(n - 1))
        return ql_more_after(n) ? QL_AWAY : (int)(n - 1);
    lo = 0;
    hi = n - 1;                      /* f[lo] <= frame < f[hi] */
    while (hi - lo > 1) {
        u32 mid = (lo + hi) >> 1;
        if (quant_loops_beat_frame(mid) <= frame)
            lo = mid;
        else
            hi = mid;
    }
    return (int)lo;
}

/* Nearest beat to `frame` (ties to the earlier one); QL_AWAY as above.
 * Before the first beat of the grid: beat 0. */
int quant_loops_beat_nearest(u32 frame)
{
    int i = quant_loops_beat_at_or_before(frame);
    u32 n = quant_loops_window_n();

    if (i == QL_AWAY)
        return QL_AWAY;
    if (i == QL_NONE)
        return 0;
    if ((u32)i + 1 >= n)
        return i;                    /* last beat of the grid */
    if (frame - quant_loops_beat_frame(i) <= quant_loops_beat_frame(i + 1) - frame)
        return i;
    return i + 1;
}

static u32 ql_step(void);
static u32 ql_pt_frame(int i, u32 s);

/* Nearest quantize point to `frame` (ties to the earlier one): beat index in
 * *i, step in *s. QL_AWAY as above; before the first beat of the grid: beat
 * 0. Returns 0 or QL_AWAY. */
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
    if ((u32)i + 1 >= n) {           /* last beat of the grid */
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

/* ms length of `beats` beats at BPMx100 (stock math with the grid tempo). */
static u32 ql_beats_ms(u32 beats, u32 bpm100)
{
    if (bpm100 == 0)
        bpm100 = 13000;              /* stock: 0 -> 130 BPM */
    return (beats * 6000000u + bpm100 / 2u) / bpm100;
}

/* Grid time `k` beats after beat i (k >= 0) in ms: the grid entry when the
 * window has it, else beat i's time plus k beats at its tempo. */
u32 quant_loops_beat_ms_after(int i, u32 k)
{
    u32 n = quant_loops_window_n();
    if ((u32)i + k < n)
        return C850_GRID_TIME_MS(i + k);
    return C850_GRID_TIME_MS(i) + ql_beats_ms(k, C850_GRID_BPM100(i));
}

/* Grid time (ms) of position p sixteenths of a beat after beat i. */
u32 quant_loops_grid_ms(int i, u32 p)
{
    u32 t0 = quant_loops_beat_ms_after(i, p >> 4), r = p & 15u;

    if (r == 0)
        return t0;
    return t0 + (r * (quant_loops_beat_ms_after(i, (p >> 4) + 1u) - t0) + 8u) / 16u;
}

/* ---- quantize points (UTILITY QUANTIZE: 1, 1/2 or 1/4 beat) ----
 *
 * A point is (beat i, step s): s sixteenths of a beat after beat i, s a
 * multiple of ql_step() (16, 8 or 4). Frame of a point: ql_pt_frame. */
static u32 ql_step(void)
{
    return 16u / c850_ui_quant_div();
}

static __attribute__((noinline)) u32 ql_pt_frame(int i, u32 s)
{
    return quant_loops_ms_to_frames(quant_loops_grid_ms(i, s));
}

/* Did this grid give the deck's tempo? Same proof as mods/bpm_decimals: the
 * base BPM word holds min((BPMx100 + 5) / 10, clamp) of a beat near `i`. */
int quant_loops_grid_ok(int i)
{
    u32 base = C850_BPM_BASE_TENTHS;
    u32 n = quant_loops_window_n();
    int j;

    if (!ql_on() || C850_IS_USB_MODE != 1 || n == 0 || QL_GRID_WINDOW_OK == 0 || base == 0)
        return 0;
    for (j = i - 8; j <= i + 8; j++) {
        u32 t;
        if (j < 0 || (u32)j >= n)
            continue;
        t = (C850_GRID_BPM100(j) + 5u) / 10u;
        if (t == base || (t > base && (base == QL_BPM_CLAMP_STOCK || base == QL_BPM_CLAMP_BPM999)))
            return 1;
    }
    return 0;
}

/* ---- blocks (docs/cdj850-block-loops-design.md) ----
 *
 * The IRQ7 feeder plays one block of 84 stereo samples (0x150 bytes, 1.9 ms)
 * per DSPDREQ; a frame (ring sector) is exactly 7 blocks. Grid ms -> blocks to
 * the nearest block: (ms * 21 + 20) / 40 (525 blocks a second). A loop's
 * length is kept exactly as num / den in 1/40 blocks: q/16 beats at a constant
 * BPMx100 is q * 7875000 / BPMx100, a tempo change inside the loop gives the
 * grid duration in ms * 21 / 1. */
#define QL_BLK(ms)          (((ms) * 21u + 20u) / 40u)
#define QL_BLK_REM(ms)      (((ms) * 21u + 20u) % 40u)
#define QL_CEIL7(b)         (((b) + 6u) / 7u)
#define QL_OFF_GRID         17u      /* ql_step_of: the in is on no 1/16 step */
#define QL_Q_CONST_MAX      512u     /* q * 7875000 < 2^32 */

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
    u32 n = quant_loops_window_n(), bpm = C850_GRID_BPM100(i), last = (u32)i + (p0 + q + 15u) / 16u, j;

    for (j = (u32)i + 1u; j <= last && j < n; j++)
        if (C850_GRID_BPM100(j) != bpm)
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

/* Stock out frame of block loop b: the first frame at or after the out block
 * of its first pass. */
static __attribute__((noinline)) u32 ql_out_frame(const ql_blk_t *b)
{
    return QL_CEIL7(b->in_blk + ql_len0(b));
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
 * The auto beat loop on the grid. `pos` = stock in frame (the playhead);
 * beats = 1/2/4/8; frac = BEAT SELECT fraction mode (the loop is then beats/16
 * of a beat). Fills *in, *out and *out8 (8 whole beats after in: the
 * reference the beat buttons resize from, CA90) as frames, and the block loop
 * in ql_ap. Returns 0 when the grid cannot say (no grid, window elsewhere,
 * past the end of the grid): stock then.
 *
 * The in is the point of the grid closest to the playhead, behind or ahead
 * of it (ties to the earlier one), as LOOP IN snaps. Ahead: the deck plays on
 * to the in and loops from there, as it does for a LOOP OUT ahead of it.
 *
 *   whole beats  in = the quantize point (UTILITY QUANTIZE: 1, 1/2 or 1/4
 *                beat) nearest pos, length = `beats` beats on the grid from
 *                there (the in beat's tempo past the window).
 *   fraction     L = beats/16 beat; in = the multiple of L on the beat at or
 *                before pos (up to the next beat) nearest pos, out = in + L.
 *                1/2 pressed 1/5 into a beat: in = the beat; 2/5 into it:
 *                in = beat + 1/2; 4/5: the next beat.
 *
 * Frames for the stock records: in = the frame that holds the in block, out =
 * the first frame at or after the first pass's out block (so the stock wrap
 * is never due before the block loop's own, quant_loops_out_hook).
 *
 * Invariant (checked, else 0): in < out and pos < out (the playhead is never
 * past the out of the new loop).
 */
static ql_blk_t ql_ap;               /* the block loop of the last auto_points */
static ql_blk_t ql_tmp;              /* scratch (playertask) */

int quant_loops_auto_points(u32 pos, u32 beats, u32 frac, u32 *in, u32 *out, u32 *out8)
{
    u32 fin, fout, s, q;
    int i;

    if (beats == 0 || beats > 16 || (frac && beats > 8))
        return 0;
    /* fraction: points every L = beats/16 beat; whole beats: the quantize value */
    q = frac ? beats : beats * 16u;
    if (ql_point_nearest(pos, frac ? beats : ql_step(), &i, &s) < 0 || !quant_loops_grid_ok(i))
        return 0;
    ql_point_loop(&ql_ap, i, s, q);
    fin = ql_ap.in_blk / 7u;
    fout = ql_out_frame(&ql_ap);
    if (fout <= fin)
        fout = fin + 1;
    if (fout <= pos)
        return 0;
    ql_point_loop(&ql_tmp, i, s, 128u);
    *in = fin;
    *out = fout;
    *out8 = ql_out_frame(&ql_tmp);
    return 1;
}

/* LOOP IN (and a new hot cue): the quantize point nearest pos (frame), or
 * pos when the grid cannot say. Rounded frame (a cue point). */
u32 quant_loops_in_point(u32 pos)
{
    int i;
    u32 s;

    if (ql_point_nearest(pos, ql_step(), &i, &s) < 0 || !quant_loops_grid_ok(i))
        return pos;
    return ql_pt_frame(i, s);
}

/* LOOP IN: the same point as a loop in frame (the frame that holds its block). */
u32 quant_loops_loop_in_point(u32 pos)
{
    int i;
    u32 s;

    if (ql_point_nearest(pos, ql_step(), &i, &s) < 0 || !quant_loops_grid_ok(i))
        return pos;
    return ql_pt_in_frame(i, s);
}

/* LOOP OUT: the quantize point nearest pos, or the first one after the loop
 * in when that is not after it; as a frame: the first frame at or after the
 * out block of the first pass. The block loop goes to ql_op (ql_op_ok). pos
 * when the grid cannot say. The length is the grid's from the in point (in on
 * the grid), else from the in frame's first block to the point's block. */
static ql_blk_t ql_op;
static u8 ql_op_ok;

/* ql_op from the in (set) to point s of beat k, or the first point after the
 * in from there; the stock out frame, or pos when none is within 4 beats. */
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
    s = i < k ? 0u : s + (u32)(i - k) * 16u;   /* the point on beat k */
    ql_op_on = (u8)on;
    return ql_op_close(k, s, p0, pos);
}

/* loop_roll's quantized release: the first quantize point whose block (1/7
 * frame, 84 samples) is at or after pos_block - late; 1 and *target when the
 * grid can say (quantize on, a grid that gave the tempo), else 0. A point up
 * to `late` blocks behind counts as reached (a release just after it lands
 * at once). Read-only; playertask. */
int quant_loops_roll_target(u32 pos_block, u32 late, u32 *target)
{
    u32 step = ql_step(), s, b, lim = pos_block > late ? pos_block - late : 0u;
    int i = quant_loops_beat_at_or_before(pos_block / 7u);

    if (i == QL_AWAY)
        return 0;
    if (i == QL_NONE)
        i = 0;
    if (!quant_loops_grid_ok(i))
        return 0;
    for (s = 0; s <= 32u; s += step) {
        b = QL_BLK(quant_loops_grid_ms(i, s));   /* ms * 525 / 1000 */
        if (b >= lim) {
            *target = b;
            return 1;
        }
    }
    return 0;
}

/* The ring sector that holds `frame`, found from sector s (which holds
 * frame `pos`); -1 if that slot does not hold it (not buffered, a jump, the
 * ring wrapped over it) or it belongs to another track. */
int quant_loops_sector_of(int s, u32 pos, u32 frame)
{
    int t = QL_RING_ADD(s, (int)(frame - pos));
    if (t < 0 || t >= 2400)
        return -1;
    if (QL_SECTOR_FRAME(t) != frame || QL_SECTOR_TRACK(t) != QL_SECTOR_TRACK(s))
        return -1;
    return t;
}

/* ---- the block loop: state the ISR hook owns the wraps with ----
 *
 * Written by playertask (qb_set, the tick, the key filter), read by the ISR
 * hook, which also moves qb_out_blk / qb_acc at each wrap. playertask clears
 * qb_armed before it writes the rest and sets it last; the ISR (priority 7)
 * never runs in between unarmed state and a half-written loop. */
#define QB_CONT   0xFFFFFFFFu        /* qb_last: the next block continues the play (no entry) */
#define QB_ENTRY  0xFFFFFFFEu        /* qb_last: the next block may be a stock entry at the in sector */
#define QB_EXPECT_TICKS 50u          /* player cycles (~0.6 s) the audio task may take to apply a loop */


static volatile u8 qb_miss;          /* the ring did not hold the in: the stock wraps this pass */
static volatile u8 qb_skip;          /* the pass under way is not a whole one (not measured) */
static volatile u8 qb_wrapped;       /* the loop wrapped since it was armed */
static volatile u32 qb_in_frame;     /* stock in record of the loop (frame) */
static volatile u32 qb_out_frame;    /* stock out frame it was set with */
static volatile u32 qb_in_blk, qb_a0, qb_num, qb_den, qb_out_blk, qb_acc;
static volatile u32 qb_last;         /* block position of the last call, or QB_CONT / QB_ENTRY */
static volatile u32 qb_n;            /* blocks played in this pass */
static u8 qb_expect, qb_expect_ticks;
static u32 qb_seen_in, qb_seen_out;  /* the loop the tick last looked at */

/* Evidence for tests. */
volatile u32 quant_loops_blk_arms;       /* block loops set (writers and the tick) */
volatile u32 quant_loops_blk_derived;    /* of those, found by the tick (memory, hot cue, RELOOP...) */
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

/* Arm the block loop b with stock in frame in_frame; `last` = QB_CONT (the
 * play continues into it) or QB_ENTRY (the stock may have just entered it at
 * the in sector). Returns the stock out frame of its first pass. */
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
    qb_acc = n % d;
    qb_out_frame = QL_CEIL7(qb_out_blk);
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

/* The one entry every out writer of quant_loops (auto loop, LOOP OUT, the
 * resize of BEAT buttons and of loop_resize) arms a loop through: the block
 * loop [in_blk (residue a0), + num/den 1/40 blocks) whose in record is the
 * frame in_blk / 7, the play continuing into it. The tick then waits up to
 * QB_EXPECT_TICKS cycles for the audio task to apply the stock records.
 * Returns the stock out frame (the first frame at or after the out block). */
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

/* Hook of the shared loop-out dispatcher (pool 0x0C07730C,
 * payload/common/c850_loop_out.c), after loop_roll: once per block while a
 * loop plays forward; the read pointer already names the next block, so the
 * position is that of the next block to play. Owns the wraps of the armed
 * loop: at out_blk it moves the read pointer to the in block plus the
 * overshoot (normally 0) and returns 0 (no stock wrap); before that it returns
 * 0 as well. -1 = not ours (the stock test decides): nothing armed, another
 * loop (in record), no loop playing, or the in not in the ring (the stock
 * wrap, frame precise, re-reads it). The position is read from the pointer
 * every block, so a jump by anyone else (a roll landing, an entry) is seen at
 * once. Interrupt context: no RTOS calls. */
int quant_loops_out_hook(int sector, u32 out, int z)
{
    u32 pos, last, f0, len, over, n, d;

    (void)sector;
    (void)out;
    (void)z;
    if (!qb_armed || C850_LOOP_IN_FRAME != qb_in_frame || C850_LOOP_ACTIVE != 1 ||
        C850_PLAYER_STATE != C850_STATE_LOOPING)
        return -1;
    pos = c850_read_pos();
    last = qb_last;
    qb_last = pos;
    quant_loops_blk_calls++;
    quant_loops_blk_pos = pos;
    quant_loops_blk_tick = C850_TICK_MS;
    if (qb_wrapped)
        quant_loops_blk_blocks++;
    qb_n++;
    if (last != QB_CONT && pos != last + 1u) {
        /* Not the next block: someone else moved the pointer. The stock
         * entries (RELOOP, a memory or hot-cue loop) land on the in sector's
         * start, up to 6 blocks before the in block: move on to the in block
         * (plus what has played since) and start the passes from the in. */
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
            if (t != pos && t < qb_out_blk && c850_ring_jump(t)) {
                qb_last = t;
                quant_loops_blk_realigns++;
                return 0;
            }
        }
    }
    if (pos < qb_out_blk)
        return 0;
    if (qb_miss)
        return -1;
    len = qb_out_blk - qb_in_blk;
    over = pos - qb_out_blk;
    if (over >= len)
        over %= len;
    if (!c850_ring_jump(qb_in_blk + over)) {
        qb_miss = 1;                 /* this pass wraps stock, frame precise */
        quant_loops_blk_misses++;
        return -1;
    }
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

/* The tracklen slot of the loaded track as auto_beat_loop_set reads it. */
static u32 ql_tracklen(void)
{
    return QL_TRACKLEN(QL_TRACK_SLOT(QL_SECTOR_TRACK((int)QL_PLAY_SECTOR)) & 3u);
}

/* q (1/16 beats, 1..QL_MAX_Q) of the loop from in spec b (beat i, step p0)
 * whose out frame is `out`: the out block of its first pass within a frame of
 * the frame's middle (ceil outs and the rounded outs of loops made before
 * block loops both fall there); two neighbours: the closer, then the one with
 * more factors of two. 0 = none. */
#define QL_MAX_Q            512u

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

u32 quant_loops_find_q(u32 in_blk, u32 a0, int i, u32 p0, u32 out)
{
    ql_blk_t *b = &ql_fq;
    u32 lo = 1, hi = QL_MAX_Q, q, best = 0, d_best = 8u, want = out * 7u, ob, d;

    b->in_blk = in_blk;
    b->a0 = a0;
    b->num = 0;
    b->den = 1;
    if (want < 3u || want - 3u <= in_blk)
        return 0;
    want -= 3u;                       /* the middle of the out frame's blocks */
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

/* loop_resize's grid frames (when quant_loops is built): the stock out frame
 * of the block loop q/16 beats long from the loop whose in record is `in`;
 * 0 when the grid cannot say. */
u32 quant_loops_q_out(u32 in, u32 q)
{
    u32 p0;
    int i;

    if (!q || !ql_in_spec(in, &ql_tmp, &i, &p0))
        return 0;
    quant_loops_block_len(i, p0, q, &ql_tmp.num, &ql_tmp.den);
    return ql_out_frame(&ql_tmp);
}

/* loop_resize's length for the next resize send (0 = none): the q it asked
 * for, so the block loop takes it exactly. One shot. 1 = quant_loops is built
 * (loop_resize has a weak no-op that says 0). */
static u32 ql_hint_q;

int quant_loops_resize_hint(u32 q)
{
    ql_hint_q = q;
    return 1;
}

/* A resize (audio cmd 6 with the resize flag, from auto_beat_loop_set's
 * state-4 branch: BEAT buttons and loop_resize) to out frame `frame`: the
 * block loop of the new length from the same in. The length is loop_resize's
 * hint, else the stock's ratio to the 8-beat reference CA90 (BEAT b: 8/b, in
 * fraction mode 16 * 8/b) when CA90 is set, else the q whose out is that
 * frame. Returns the frame to send: the stock out of the block loop, or
 * `frame` when the block path cannot take it. */
static __attribute__((noinline)) u32 ql_resize(u32 frame)
{
    u32 in = C850_LOOP_IN_FRAME, q = ql_hint_q, p0, ref = QL_CA90, r, out;
    ql_blk_t *b = &ql_tmp;
    int i;

    ql_hint_q = 0;
    if (!ql_on() || C850_IS_USB_MODE != 1 || frame <= in || !ql_in_spec(in, b, &i, &p0))
        return frame;
    if (!q && ref > frame) {
        /* the stock division in + (CA90 - in) / r, r = 8/b (x16 in fraction mode):
         * the r whose division gives exactly this frame (the grid length of
         * those beats need not be 1/r of CA90's, across a tempo change) */
        for (r = 1u; r <= 128u && !q; r <<= 1)
            if ((ref - in) / r == frame - in)
                q = 128u / r;
    }
    if (!q)
        q = quant_loops_find_q(b->in_blk, b->a0, i, p0, frame);
    if (!q)
        return frame;
    quant_loops_block_len(i, p0, q, &b->num, &b->den);
    if (ql_out_frame(b) >= ql_tracklen())
        return frame;                /* past the track end: stock clamps it */
    out = qb_set(b);
    C850_LOOP_D1C0 = out;
    C850_LOOP_OUT_REQUEST = out;
    quant_loops_resizes++;
    return out;
}

/* Pool 0x0C0E62BC (FUN_0C00A4CC, read at 0x0C0E602E and 0x0C0E61B4) in the
 * state-4 resize sender: FUN_0C00A4CC(out, 0) writes the out request
 * 0x0C7F64DC (which the feeder loops to) and re-plans the ring; stock sends
 * audio cmd 6 (pool 0x0C0E62CC) only when it returns 1, with *D1C0. The
 * resize gets the block loop here, so the request and D1C0 carry its frame
 * whether the send follows or not. */
#define QL_REQUEST_OUT      C850_FN(0x0C00A4CC, int, (u32 out, int z))

int quant_loops_resize_req(u32 frame, int z)
{
    frame = ql_resize(frame);
    return QL_REQUEST_OUT(frame, z);
}

/* ---- redirect entry points ---- */

/* Pending block loop of the auto loop being set (quant_loops_auto_in ->
 * quant_loops_auto_send). */
static ql_blk_t ql_pend;
static u8 ql_pend_blk;

/* The pending loop with its in at frame pos (the grid point that frame
 * names, else the frame), the same length; its stock out frame. */
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

/* Pool 0x0C0E5D4C (FUN_0C073954) in auto_beat_loop_set's USB new-loop path. */
u32 quant_loops_auto_in(u32 track)
{
    u32 slot = QL_TRACK_SLOT(track);
    u32 pos = C850_LOOP_D1C0, in, out, out8, tracklen, clamped = 0;

    ql_pending = 0;
    ql_keep_sector = 0;
    ql_pend_blk = 0;
    if (!quant_loops_auto_points(pos, C850_AUTO_LOOP_BEATS, C850_BEAT_FRACTION_MODE, &in, &out, &out8))
        return slot;
    ql_pend = ql_ap;
    tracklen = QL_TRACKLEN(slot & 3);
    if (out >= tracklen) {
        out = tracklen - 1;
        clamped = 1;                 /* the track ends inside the loop: frames, stock wraps */
    }
    if (out8 >= tracklen)
        out8 = tracklen - 1;
    if (out <= pos || out <= in)
        return slot;                 /* the track ends before the loop would: stock */
    if (in != pos) {
        int s = -1, cued = C850_PLAYER_STATE == C850_STATE_CUED;
        /* state 6 (cued): the in is the cue itself (0x0C7F6468 is not set on
         * this path), so only the length comes from the grid */
        if (!cued)
            s = quant_loops_sector_of(QL_IN_SECTOR, pos, in);
        if (s >= 0) {
            QL_IN_SECTOR = (s16)s;
            ql_in_sector = (s16)s;
            C850_LOOP_D1C0 = in;
            ql_keep_sector = 1;
            quant_loops_in_count++;
        } else {
            if (!cued)
                quant_loops_refused++;
            out8 = pos + (out8 - in);
            in = pos;
            out = ql_pend_at(pos);
        }
    }
    if (out >= tracklen) {               /* the stock-in fallback moved it */
        out = tracklen - 1;
        clamped = 1;
    }
    ql_pend_blk = !clamped;
    if (out8 >= tracklen)
        out8 = tracklen - 1;
    ql_pending_out = out;
    ql_pending_out8 = out8;
    ql_pending = 1;
    quant_loops_last_pos = pos;
    quant_loops_last_in = in;
    return slot;
}

/* Pools 0x0C0E5FC8 and 0x0C0E5D08 (audio_send_cmd) in auto_beat_loop_set. */
void quant_loops_auto_send(int cmd, int flag, u32 frame, int resize, int a5, int a6, int a7, int src)
{
    if (ql_pending) {
        ql_pending = 0;
        if ((cmd & 0xFF) == 6 && (resize & 0xFF) == 0) {
            /* The audio task builds the in record from 0x0C7F6468 when it
             * handles this command. It runs at a lower priority (3) than this
             * task (playertask, 2), so in MAIN 1.13 that happens after the
             * refresh quant_loops_auto_keep covers; the sector is written
             * here as well so that it is right in either order. */
            if (ql_keep_sector)
                QL_IN_SECTOR = ql_in_sector;
            frame = ql_pending_out;
            C850_LOOP_D1C0 = frame;
            QL_CA90 = ql_pending_out8;
            quant_loops_auto_count++;
            quant_loops_last_out = frame;
            if (ql_pend_blk)
                qb_set(&ql_pend);
        }
    } else if ((cmd & 0xFF) == 6 && (resize & 0xFF) == 1 &&
               !(qb_armed && frame == qb_out_frame && C850_LOOP_IN_FRAME == qb_in_frame)) {
        frame = ql_resize(frame);    /* (quant_loops_resize_req has armed it already) */
    }
    c850_audio_send_cmd(cmd, flag, frame, resize, a5, a6, a7, src);
}

/* Pool 0x0C0E5FD0 (FUN_0C0059D2) right after the new-loop send of
 * auto_beat_loop_set (0x0C0E5DAA, both paths; skipped in state 6). Stock
 * refreshes the next-sector word 0x0C7F6468 there, and the audio task reads
 * it only when it handles the cmd 6 just queued, so the refresh would undo
 * the snapped in. With a snapped in pending the refresh is left out (the
 * word keeps the in sector, which is what stock then reads back for the
 * track number at 0x0CE6D2A6). */
void quant_loops_auto_keep(void)
{
    if (ql_keep_sector) {
        ql_keep_sector = 0;
        QL_IN_SECTOR = ql_in_sector;
        return;
    }
    QL_NEXT_SECTOR();
}

/* Pool 0x0C0DFA80 (FUN_0C0059D2) in the LOOP IN path. */
void quant_loops_loop_in(void)
{
    int s, t;
    u32 pos, in;

    QL_NEXT_SECTOR();
    if (C850_IS_USB_MODE != 1 || !ql_on())
        return;
    s = QL_IN_SECTOR;
    if (s < 0 || s >= 2400)
        return;
    pos = QL_SECTOR_FRAME(s);
    in = quant_loops_loop_in_point(pos);
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
    QL_IN_SECTOR = (s16)t;
    quant_loops_in_count++;
}

/* Pools 0x0C0E0090 and 0x0C0E02C0 (audio_send_cmd) in the LOOP OUT path. */
void quant_loops_loop_out(int cmd, int flag, u32 a3, int mode, int sector, int a6, int a7, int src)
{
    if ((cmd & 0xFF) == 1 && (mode & 0xFF) == 3 && C850_IS_USB_MODE == 1 && ql_on()) {
        int s = sector & 0xFFFF, t = -1;
        u32 pos, in, out;
        if (s < 2400) {
            pos = QL_SECTOR_FRAME(s);
            in = C850_LOOP_IN_FRAME;
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
    c850_audio_send_cmd(cmd, flag, a3, mode, sector, a6, a7, src);
}

/* ---- the state-4 tick: which loop the block path owns ----
 *
 * Hook of the shared state-4 tick c850_loop_tick (c850.h; pool 0x0C0D7B98,
 * keys_state4_looping), once per player cycle while a loop plays, before the
 * stock handler. A loop quant_loops did not set itself (a memory or hot-cue
 * loop, RELOOP of an older loop, an out changed elsewhere) is looked up on the
 * grid from its stock records: the in's grid point (or its frame), the length
 * whose out is the out frame. Found: armed, with the in-sector entry moved to
 * the in block on its next block. Not found, quantize off, no grid: the loop
 * stays stock (frame precise).
 *
 * Which out plays: the request 0x0C7F64DC when it changed after the record
 * 0x0C7F64B0 last did, else the record (loop_resize.c, lr_track: a resize
 * writes the request, a new loop / RELOOP / EXIT the record). */
static u32 qd_dc, qd_b0, qd_fresh;

static void qd_track(void)
{
    u32 dc = C850_LOOP_OUT_REQUEST, b0 = C850_LOOP_OUT_FRAME;
    int dch = dc != qd_dc, bch = b0 != qd_b0;

    if (bch)
        qd_fresh = 0;
    else if (dch)
        qd_fresh = 1;
    qd_dc = dc;
    qd_b0 = b0;
}

u32 quant_loops_effective_out(u32 in)
{
    return qd_fresh && qd_dc > in ? qd_dc : qd_b0;
}

volatile u32 quant_loops_ticks;          /* tick calls */

/* The block loop of the stock loop [in, out) frames, armed with an entry; 0
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

void quant_loops_loop_tick(void)
{
    u32 in, out;

    quant_loops_ticks++;
    qd_track();
    if (!ql_on() || C850_IS_USB_MODE != 1 || C850_LOOP_ACTIVE != 1 ||
        C850_PLAYER_STATE != C850_STATE_LOOPING) {
        qb_armed = 0;
        qb_expect = 0;
        return;
    }
    in = C850_LOOP_IN_FRAME;
    out = quant_loops_effective_out(in);
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

/* UTILITY QUANTIZE: the quantize value, 1 BEAT -> 1/2 BEAT -> 1/4 BEAT.
 * Quantize on/off stays on BEAT SELECT + LOOP IN. Kept in the file/RAM
 * flags (bits 16..17, c850_ui.h): it travels with the stick. */
static const char *ql_q_utility_value(void)
{
    u32 d;

    if (!quant_loops_on)
        return 0;
    d = c850_ui_quant_div();
    return d == 4u ? "1/4 BEAT" : d == 2u ? "1/2 BEAT" : "1 BEAT";
}

static void ql_q_utility_select(void)
{
    u32 d = c850_ui_quant_div();
    u32 v = d == 1u ? 1u : d == 2u ? 2u : 0u;      /* 1 -> 1/2 -> 1/4 -> 1 */

    c850_ui_set_flags(C850_UI_FLAG_QVALUE_MASK, v << C850_UI_FLAG_QVALUE_SHIFT);
}

static const c850_ui_utility_item ql_q_utility_item = {
    15u, "QUANTIZE", ql_q_utility_value, ql_q_utility_select,
};
C850_UI_UTILITY_ITEM(ql_q_utility_item);
