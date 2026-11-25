/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * loop_resize: while a loop plays, CUE/LOOP CALL < halves it and CALL >
 * doubles it (CDJ-2000 style). The loop in stays put. Outside a loop CALL
 * keeps its stock memory-cue function (see ../patch.py and
 * docs/cdj850-modding.md section 4b.10).
 *
 * Entry points (redirected pool words, see ../patch.py):
 *   loop_resize_call           0x0C0D9994 (cue_call_prev_next, r4 = 0 CALL <,
 *                              1 CALL >), the two CALL branches of
 *                              player_handle_tempo_and_time_buttons.
 *   loop_resize_state4         hook of the shared tick c850_loop_tick
 *                              (0x0C0D7B98, keys_state4_looping; c850.h,
 *                              loop_tick.c), once per player cycle in state 4:
 *                              follows the out in effect, sends a halving
 *                              that waited.
 *   loop_resize_memory_button  0x0C0D9714 (memory_button): MEMORY stores the
 *                              loop that plays.
 *
 * The resize itself goes through the stock state-4 beat-loop handler
 * FUN_0C0E5390(beats) (what BEAT 1/2/4/8 do inside a loop): it runs
 * auto_beat_loop_set, whose resize branch computes
 *     out = in + (CA90 - in) / ((8 / beats) * (D154 * 15 + 1))
 * (FUN_0C0E5668), sets D1C0, checks the buffer (FUN_0C00A4CC, which also
 * writes the requested out 0x0C7F64DC) and sends audio cmd 6 with the resize
 * flag, with every flag (D12D/D136, vinyl brake, state) as stock does. This
 * routine only sets CA90 so that the division gives the wanted out exactly
 * and puts CA90 back afterwards, so BEAT buttons keep resizing from their
 * own 8-beat reference.
 *
 * Lengths:
 *   grid     (quantize on, c850_ui.h, and a rekordbox grid gave the deck's
 *            tempo, as in quant_loops) the
 *            loop is q sixteenths of a beat on the grid, counted from the
 *            loop in (the in on a beat or a 1/16 step of it: exact grid
 *            frames; otherwise in + the grid duration). < gives q/2, > 2q;
 *            1/16 beat <= length <= 32 beats.
 *   frames   (no grid, or a loop whose length is not a whole number of
 *            1/16 beats within one frame) len/2 or 2*len frames, at least
 *            one frame.
 *   Out past the track end: clamped to tracklen - 1 (then ignored once
 *   there). A press at a limit is ignored.
 *
 * Integer only (no FPU). Products stay below 2^32 (ms * 75 for ms < 57e6).
 */
#include "c850.h"
#include "c850_ui.h"

#define LR_CA90             C850_U32(0x0CE6CA90)   /* 8-beat reference of the beat resize */
#define LR_GRID_WINDOW_OK   C850_U32(C850_GRID_CTX + 0x70)
#define LR_BEAT_HANDLER4    C850_FN(0x0C0E5390, int, (int beats))  /* state-4 BEAT handler */
#define LR_BPM_CLAMP_STOCK  1800
#define LR_BPM_CLAMP_BPM999 9990
#define LR_MAX_Q            512u                   /* 32 beats in 1/16 beats */

/* Run-time switch (1 = resize; 0 = CALL is always stock). */
volatile u32 loop_resize_on = 1;

/* Evidence for tests. */
volatile u32 loop_resize_count;       /* resizes sent */
volatile u32 loop_resize_ignored;     /* presses at a limit */
volatile u32 loop_resize_passed;      /* CALL presses handed to the stock routine */
volatile u32 loop_resize_last_in;
volatile u32 loop_resize_last_old;    /* out the press started from */
volatile u32 loop_resize_last_new;    /* out it asked for */
volatile u32 loop_resize_last_q;      /* grid length in 1/16 beats after the press, 0 = frames */
volatile u32 loop_resize_last_mode;   /* 1 grid, 2 frames */

/* ---- grid helpers (the same arithmetic as quant_loops.c) ---- */

u32 loop_resize_ms_to_frames(u32 ms)
{
    return (ms * 75u + 500u) / 1000u;
}

static u32 lr_window_n(void)
{
    u32 total = C850_GRID_BEAT_COUNT;
    u32 first = (u32)C850_GRID_WINDOW_INDEX * 500u;
    u32 n;

    if (total <= first)
        return 0;
    n = total - first;
    return n > 1000u ? 1000u : n;
}

static u32 lr_beat_frame(int i)
{
    return loop_resize_ms_to_frames(C850_GRID_TIME_MS(i));
}

/* Index of the last beat at or before `frame` in the window in RAM; -1
 * before the grid or when the window does not cover the position. */
int loop_resize_beat_at_or_before(u32 frame)
{
    u32 n = lr_window_n();
    u32 lo, hi;

    if (n == 0 || frame < lr_beat_frame(0))
        return -1;
    if (frame >= lr_beat_frame(n - 1))
        return (u32)C850_GRID_WINDOW_INDEX * 500u + n < C850_GRID_BEAT_COUNT ? -1 : (int)(n - 1);
    lo = 0;
    hi = n - 1;
    while (hi - lo > 1) {
        u32 mid = (lo + hi) >> 1;
        if (lr_beat_frame(mid) <= frame)
            lo = mid;
        else
            hi = mid;
    }
    return (int)lo;
}

/* Grid time (ms) of beat i + k (k >= 0): the window entry, else beat i plus
 * k beats at its tempo. */
static u32 lr_beat_ms(int i, u32 k)
{
    u32 n = lr_window_n(), bpm100;

    if ((u32)i + k < n)
        return C850_GRID_TIME_MS(i + k);
    bpm100 = C850_GRID_BPM100(i);
    if (bpm100 == 0)
        bpm100 = 13000;
    return C850_GRID_TIME_MS(i) + (k * 6000000u + bpm100 / 2u) / bpm100;
}

/* Grid time of position p sixteenths of a beat after beat i. */
u32 loop_resize_grid_ms(int i, u32 p)
{
    u32 t0 = lr_beat_ms(i, p >> 4), r = p & 15u;

    if (r == 0)
        return t0;
    return t0 + (r * (lr_beat_ms(i, (p >> 4) + 1u) - t0) + 8u) / 16u;
}

/* Quantize is on and the grid gave the deck's tempo (quant_loops_grid_ok).
 * Quantize off (BEAT SELECT + LOOP IN with quant_loops): frame mode. */
static int lr_grid_ok(int i)
{
    u32 base = C850_BPM_BASE_TENTHS;
    u32 n = lr_window_n();
    int j;

    if (!c850_ui_quantize() || C850_IS_USB_MODE != 1 || n == 0 || LR_GRID_WINDOW_OK == 0 || base == 0)
        return 0;
    for (j = i - 8; j <= i + 8; j++) {
        u32 t;
        if (j < 0 || (u32)j >= n)
            continue;
        t = (C850_GRID_BPM100(j) + 5u) / 10u;
        if (t == base || (t > base && (base == LR_BPM_CLAMP_STOCK || base == LR_BPM_CLAMP_BPM999)))
            return 1;
    }
    return 0;
}

/* The loop's grid: beat i at or before in, and the in's 1/16 step s on it
 * (16 = not on a step: lengths are then added to in). 0 = no grid. */
struct lr_grid { int i; u32 s; u32 in; };

static int lr_grid_of(u32 in, struct lr_grid *g)
{
    int i = loop_resize_beat_at_or_before(in);
    u32 s;

    if (i < 0 || !lr_grid_ok(i))
        return 0;
    g->i = i;
    g->in = in;
    for (s = 0; s < 16u; s++)
        if (loop_resize_ms_to_frames(loop_resize_grid_ms(i, s)) == in)
            break;
    g->s = s;
    return 1;
}

/* Out frame of a loop q sixteenths of a beat long on grid g: in + the grid
 * duration rounded once (as quant_loops makes its outs), so the first pass
 * is within half a frame of the grid length. */
u32 loop_resize_grid_out(const struct lr_grid *g, u32 q)
{
    u32 b = g->s < 16u ? g->s : 0u;

    return g->in + loop_resize_ms_to_frames(loop_resize_grid_ms(g->i, b + q) - loop_resize_grid_ms(g->i, b));
}

/* Trailing zero bits (q's power of two: 64 = 4 beats beats 63). */
static u32 lr_tz(u32 q)
{
    u32 n = 0;

    while (q && !(q & 1u)) {
        q >>= 1;
        n++;
    }
    return n;
}

/* The q (1..512) whose grid out is within one frame of `out`, else 0. Two
 * neighbours can both be (frames are coarser than 1/16 beat at an in off a
 * beat): the closer wins, then the one with more factors of two. */
static u32 lr_grid_q(const struct lr_grid *g, u32 out)
{
    u32 lo = 1, hi = LR_MAX_Q, best = 0, d_best = 2;

    while (lo < hi) {                     /* first q with grid out >= out */
        u32 mid = (lo + hi) >> 1;
        if (loop_resize_grid_out(g, mid) < out)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (hi = lo > 1 ? lo - 1 : lo; hi <= lo; hi++) {
        u32 f = loop_resize_grid_out(g, hi), d = f > out ? f - out : out - f;
        if (d < d_best || (d == d_best && best && lr_tz(hi) > lr_tz(best))) {
            d_best = d;
            best = hi;
        }
    }
    return best;
}

/*
 * The new out for a press (dir 0 = halve, 1 = double) of a loop [in, out)
 * in a track of `tracklen` frames; 0 = ignore the press. Sets *q to the new
 * grid length (0 in frame mode) and *mode (1 grid, 2 frames).
 */
u32 loop_resize_new_out(u32 in, u32 out, u32 tracklen, u32 dir, u32 *q, u32 *mode)
{
    struct lr_grid g;
    u32 k = 0, nout, len;

    *q = 0;
    *mode = 2;
    if (out <= in || tracklen == 0 || in >= tracklen - 1u)
        return 0;
    if (lr_grid_of(in, &g))
        k = lr_grid_q(&g, out);
    if (k) {
        *mode = 1;
        if (dir) {
            if (2u * k > LR_MAX_Q)
                return 0;
            k *= 2u;
        } else {
            if (k < 2u)
                return 0;
            k >>= 1;
        }
        nout = loop_resize_grid_out(&g, k);
        *q = k;
    } else {
        len = out - in;
        if (dir) {
            nout = in + 2u * len;
            if (nout < in)
                return 0;
        } else {
            if (len < 2u)
                return 0;
            nout = in + len / 2u;
        }
    }
    if (nout >= tracklen) {
        if (out >= tracklen - 1u)
            return 0;                     /* already at the end */
        nout = tracklen - 1u;
        *q = 0;
    }
    if (nout <= in || nout == out)
        return 0;
    return nout;
}

/*
 * Which out the loop plays (traced, docs 4b.10): a resize (cmd 6 flag 1, or
 * FUN_0C00A4CC) writes the requested out 0x0C7F64DC and the feeder loops to
 * it from then on, while the out record 0x0C7F64B0 keeps the old out until
 * EXIT/RELOOP. A new loop from LOOP OUT, CALL (memory loop) or RELOOP sets
 * 0x0C7F64B0 and leaves 0x0C7F64DC alone (stale). So the effective out is
 * 0x0C7F64DC when it changed after 0x0C7F64B0 last did, else 0x0C7F64B0 (both
 * changed between two looks: the record).
 * lr_track() follows both words from the CALL, state-4 and MEMORY entries.
 */
static u32 lr_dc_seen, lr_b0_seen, lr_fresh;
static u32 lr_pending, lr_pend_in, lr_pend_out, lr_pend_old, lr_pend_q;

volatile u32 loop_resize_ticks;       /* state-4 handler calls seen */
volatile u32 loop_resize_deferred;    /* halvings left for the next loop pass */
volatile u32 loop_resize_memory;      /* MEMORY presses given the effective out */

static void lr_track(void)
{
    u32 dc = C850_LOOP_OUT_REQUEST, b0 = C850_LOOP_OUT_FRAME;
    int dch = dc != lr_dc_seen, bch = b0 != lr_b0_seen;

    if (dch)
        lr_pending = 0;               /* someone else resized meanwhile */
    if (bch)
        lr_fresh = 0;                 /* a new loop (or EXIT/RELOOP applied the request) */
    else if (dch)
        lr_fresh = 1;                 /* a resize */
    lr_dc_seen = dc;
    lr_b0_seen = b0;
}

u32 loop_resize_effective_out(u32 in)
{
    u32 dc = C850_LOOP_OUT_REQUEST;

    if (lr_fresh && dc > in)
        return dc;
    return C850_LOOP_OUT_FRAME;
}

/* BEAT button for the LEDs: the one whose stock loop the new length is
 * (q = beats * 16, or beats in fraction mode), else 0 (no beat LED). */
static u32 lr_led_beats(u32 q, u32 frac)
{
    u32 b;

    for (b = 1; b <= 8u; b <<= 1)
        if (q == (frac ? b : b * 16u))
            return b;
    return 0;
}

/* 1 = the stock handler took the press (0 when 0x0CE6D26A == 0xFF). */
static int lr_resize(u32 in, u32 old, u32 nout, u32 q)
{
    u32 frac = C850_BEAT_FRACTION_MODE ? 1u : 0u;
    u32 led = q ? lr_led_beats(q, frac) : 0;
    u32 b = led ? led : 8u;
    u32 saved = LR_CA90;
    int done;

    /* the stock resize gives in + (CA90 - in) / ((8 / b) * (frac * 15 + 1)) */
    LR_CA90 = in + (nout - in) * (8u / b) * (frac * 15u + 1u);
    done = LR_BEAT_HANDLER4((int)b) & 0xFF;
    LR_CA90 = saved;
    lr_track();
    if (!done)
        return 0;
    C850_AUTO_LOOP_BEATS = (u8)led;
    loop_resize_count++;
    loop_resize_last_in = in;
    loop_resize_last_old = old;
    loop_resize_last_new = nout;
    loop_resize_last_q = q;
    return 1;
}

/* A new out behind the playhead would close the loop at once (the feeder
 * jumps to in: phase lost). A halving is sent only while the playhead is in
 * the first half of the new loop; otherwise it waits for the next pass (the
 * old length is a multiple of the new one, so the loop stays in phase). */
static int lr_in_first_half(u32 in, u32 nout)
{
    u32 pos = C850_PLAY_FRAME;

    return pos >= in && pos - in < (nout - in + 1u) / 2u;
}

/* Pool 0x0C0D9994 (cue_call_prev_next): CALL < (0) / CALL > (1). */
void loop_resize_call(int next)
{
    u32 in, old, nout, q, mode;

    if (!loop_resize_on || C850_IS_USB_MODE != 1 || C850_LOOP_ACTIVE != 1 ||
        C850_PLAYER_STATE != C850_STATE_LOOPING) {
        loop_resize_passed++;
        c850_cue_call_prev_next(next);
        return;
    }
    lr_track();
    in = C850_LOOP_IN_FRAME;
    if (lr_pending && lr_pend_in != in)
        lr_pending = 0;
    old = lr_pending ? lr_pend_out : loop_resize_effective_out(in);
    nout = loop_resize_new_out(in, old, C850_TRACK_LENGTH, (u32)(next & 0xFF) ? 1u : 0u, &q, &mode);
    loop_resize_last_mode = mode;
    if (nout == 0) {
        loop_resize_ignored++;
        return;
    }
    lr_pending = 0;
    if (nout < loop_resize_effective_out(in) && !lr_in_first_half(in, nout)) {
        lr_pending = 1;
        lr_pend_in = in;
        lr_pend_out = nout;
        lr_pend_old = old;
        lr_pend_q = q;
        loop_resize_deferred++;
        loop_resize_last_new = nout;
        loop_resize_last_q = q;
        return;
    }
    if (!lr_resize(in, old, nout, q))
        loop_resize_ignored++;
}

/* Hook of the shared state-4 tick c850_loop_tick (pool 0x0C0D7B98,
 * keys_state4_looping 0x0C0E0F34, c850.h): runs once per player cycle in
 * state 4, before the stock handler. Sends a deferred halving once the loop
 * has wrapped. */
void loop_resize_state4(void)
{
    loop_resize_ticks++;
    lr_track();
    if (lr_pending) {
        u32 in = C850_LOOP_IN_FRAME;
        if (C850_LOOP_ACTIVE != 1 || C850_PLAYER_STATE != C850_STATE_LOOPING || in != lr_pend_in) {
            lr_pending = 0;
        } else if (lr_in_first_half(in, lr_pend_out)) {
            lr_pending = 0;
            lr_resize(in, lr_pend_old, lr_pend_out, lr_pend_q);
        }
    }
}


/* Pool 0x0C0D9714 (memory_button, 0x0C0D4172). memory_cue_add stores a loop
 * from the out record (0x0C7F64B0 and its M/S/F at 0x0C7F64B8), which a
 * resize does not update until EXIT: while a resized loop plays, the record
 * is given the effective out for the call and put back after it. */
#define LR_MEMORY_BUTTON C850_FN(0x0C0D4172, void, (void))
#define LR_OUT_MSF(k)    C850_U16(0x0C7F64B8 + 2 * (k))
void loop_resize_memory_button(void)
{
    u32 in, eff, b0;
    u16 m, sec, f;

    lr_track();
    in = C850_LOOP_IN_FRAME;
    b0 = C850_LOOP_OUT_FRAME;
    eff = loop_resize_effective_out(in);
    if (!loop_resize_on || C850_IS_USB_MODE != 1 || C850_LOOP_ACTIVE != 1 ||
        C850_PLAYER_STATE != C850_STATE_LOOPING || eff == b0 || eff <= in) {
        LR_MEMORY_BUTTON();
        return;
    }
    m = LR_OUT_MSF(0);
    sec = LR_OUT_MSF(1);
    f = LR_OUT_MSF(2);
    C850_LOOP_OUT_FRAME = eff;
    LR_OUT_MSF(0) = (u16)(eff / 4500u);
    LR_OUT_MSF(1) = (u16)((eff / 75u) % 60u);
    LR_OUT_MSF(2) = (u16)(eff % 75u);
    loop_resize_memory++;
    LR_MEMORY_BUTTON();
    /* put the record back unless something rewrote it meanwhile */
    if (C850_LOOP_OUT_FRAME == eff) {
        C850_LOOP_OUT_FRAME = b0;
        LR_OUT_MSF(0) = m;
        LR_OUT_MSF(1) = sec;
        LR_OUT_MSF(2) = f;
    }
    lr_b0_seen = C850_LOOP_OUT_FRAME;
}
