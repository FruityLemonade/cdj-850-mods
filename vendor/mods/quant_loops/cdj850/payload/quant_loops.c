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
 *   quant_loops_auto_send  audio_send_cmd for the USB new-loop sends of
 *                          auto_beat_loop_set: puts the grid out into the
 *                          cmd 6 (resize flag 0) request, D1C0 and CA90.
 *   quant_loops_auto_keep  FUN_0C0059D2 after that send: keeps the snapped
 *                          in sector (stock refreshes it before the audio
 *                          task reads it).
 *   quant_loops_loop_in    FUN_0C0059D2 in the LOOP IN path: snaps 0x0C7F6468
 *                          to the nearest beat.
 *   quant_loops_loop_out   audio_send_cmd in the USB LOOP OUT path: snaps the
 *                          out sector of cmd 1 / 3 to the nearest beat after in.
 *   c850_ui_keys           0x0C0ED038, the shared key filter chain
 *                          (payload/common/c850_ui.c): quant_loops_key_filter.
 *   c850_ui_flash_hook     tail of display_stage_bpm: the QUANTIZE ON/OFF flash.
 *   quant_loops_drift_tick hook of the shared state-4 tick c850_loop_tick
 *                          (0x0C0D7B98, keys_state4_looping; c850.h,
 *                          loop_tick.c): the per-pass drift correction below.
 *
 * Precision: whole frames (1/75 s). ms -> frames rounds to the nearest frame
 * (quant_loops_ms_to_frames). The rounding of a loop's length does not add up
 * over the passes: the drift correction (end of this file) alternates the
 * pass lengths so that their sum follows the exact grid length.
 *
 * Integer only (no FPU). Products stay below 2^32: ms * 75 for ms < 57e6
 * (15.9 h), beats * 6000000 for beats <= 8.
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
/* In frame of the last loop quant_loops made (auto beat loop or LOOP OUT on
 * a USB grid track), all ones = none. The drift correction takes on only a
 * loop with this in: a hot-cue or memory loop loaded from the stick plays
 * (and is saved back) exactly as stored, as on a stock deck. */
volatile u32 quant_loops_drift_owner = 0xFFFFFFFFu;
volatile u32 quant_loops_last_out;       /* last out frame (auto / LOOP OUT) */
volatile u32 quant_loops_last_pos;       /* stock position the snap started from */
volatile u32 quant_loops_auto_count;     /* auto loops given a grid out */
volatile u32 quant_loops_in_count;       /* in points moved (auto or LOOP IN) */
volatile u32 quant_loops_out_count;      /* LOOP OUT sectors moved */
volatile u32 quant_loops_refused;        /* snaps refused (sector not in the ring, track change) */

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

/*
 * The auto beat loop on the grid. `pos` = stock in frame (the playhead);
 * beats = 1/2/4/8; frac = BEAT SELECT fraction mode (the loop is then beats/16
 * of a beat). Fills *in, *out and *out8 (8 whole beats after in: the
 * reference the beat buttons resize from, CA90). Returns 0 when the grid
 * cannot say (no grid, window elsewhere, past the end of the grid): stock
 * then.
 *
 * The in is the point of the grid closest to the playhead, behind or ahead
 * of it (ties to the earlier one), as LOOP IN snaps. Ahead: the deck plays on
 * to the in and loops from there, as it does for a LOOP OUT ahead of it.
 *
 *   whole beats  in = the beat nearest pos, out = the grid time of beat
 *                in + beats (the in beat's tempo past the window).
 *   fraction     L = beats/16 of the interval of the beat at or before pos;
 *                in = the multiple of L after that beat (up to the next beat)
 *                nearest pos, out = in + L. 1/2 pressed 1/5 into a beat:
 *                in = the beat; 2/5 into it: in = beat + 1/2; 4/5: the next beat.
 *
 * Invariant (checked, else 0): in < out and pos < out (the playhead is never
 * past the out of the new loop).
 */
int quant_loops_auto_points(u32 pos, u32 beats, u32 frac, u32 *in, u32 *out, u32 *out8)
{
    u32 n = quant_loops_window_n();
    u32 t_beat, t_in, t_out, fin, fout;
    int i;

    if (beats == 0 || beats > 16)
        return 0;
    if (frac) {
        u32 d, k, steps = 16u / beats, best = 0, best_dist = 0xFFFFFFFFu;

        i = quant_loops_beat_at_or_before(pos);
        if (i == QL_NONE)
            i = 0;                   /* lead-in: the first beat is the nearest */
        if (i < 0 || !quant_loops_grid_ok(i))
            return 0;
        t_beat = C850_GRID_TIME_MS(i);
        d = quant_loops_beat_ms_after(i, 1) - t_beat;          /* this beat's interval */
        for (k = 0; k <= steps; k++) {                         /* k == steps: the next beat */
            u32 f = quant_loops_ms_to_frames(t_beat + (k * beats * d + 8u) / 16u);
            u32 dist = f > pos ? f - pos : pos - f;
            if (dist < best_dist) {
                best_dist = dist;
                best = k;
            }
        }
        k = best;
        if (k == steps) {            /* the next beat is nearest: its interval from there */
            if ((u32)i + 1u >= n)
                return 0;
            i++;
            t_beat = C850_GRID_TIME_MS(i);
            d = quant_loops_beat_ms_after(i, 1) - t_beat;
            k = 0;
        }
        t_in = t_beat + (k * beats * d + 8u) / 16u;
        t_out = t_beat + ((k + 1u) * beats * d + 8u) / 16u;
    } else {
        i = quant_loops_beat_nearest(pos);
        if (i < 0 || !quant_loops_grid_ok(i))
            return 0;
        t_beat = t_in = C850_GRID_TIME_MS(i);
        t_out = quant_loops_beat_ms_after(i, beats);
    }
    fin = quant_loops_ms_to_frames(t_in);
    fout = quant_loops_ms_to_frames(t_out);
    if (fout <= fin)
        fout = fin + 1;
    if (fout <= pos)
        return 0;
    *in = fin;
    *out = fout;
    *out8 = quant_loops_ms_to_frames(t_in + (quant_loops_beat_ms_after(i, 8) - t_beat));
    return 1;
}

/* LOOP IN: the nearest beat to pos (frame), or pos when the grid cannot say. */
u32 quant_loops_in_point(u32 pos)
{
    int i = quant_loops_beat_nearest(pos);
    if (i < 0 || !quant_loops_grid_ok(i))
        return pos;
    return quant_loops_beat_frame(i);
}

/* LOOP OUT: the beat nearest pos whose frame is after `in`; when that is not
 * after in, the next beat after in. pos when the grid cannot say. */
u32 quant_loops_out_point(u32 in, u32 pos)
{
    u32 n = quant_loops_window_n();
    int i = quant_loops_beat_nearest(pos);

    if (i < 0 || !quant_loops_grid_ok(i))
        return pos;
    if (quant_loops_beat_frame(i) <= in) {
        i = quant_loops_beat_at_or_before(in);
        if (i == QL_AWAY)
            return pos;
        i = i == QL_NONE ? 0 : i + 1;
        if ((u32)i >= n || quant_loops_beat_frame(i) <= in)
            return pos;              /* no beat after in in this window */
    }
    return quant_loops_beat_frame(i);
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

/* ---- redirect entry points ---- */

/* Pool 0x0C0E5D4C (FUN_0C073954) in auto_beat_loop_set's USB new-loop path. */
u32 quant_loops_auto_in(u32 track)
{
    u32 slot = QL_TRACK_SLOT(track);
    u32 pos = C850_LOOP_D1C0, in, out, out8, len, tracklen;

    ql_pending = 0;
    ql_keep_sector = 0;
    if (!quant_loops_auto_points(pos, C850_AUTO_LOOP_BEATS, C850_BEAT_FRACTION_MODE, &in, &out, &out8))
        return slot;
    tracklen = QL_TRACKLEN(slot & 3);
    if (out >= tracklen)
        out = tracklen - 1;
    if (out8 >= tracklen)
        out8 = tracklen - 1;
    if (out <= pos || out <= in)
        return slot;                 /* the track ends before the loop would: stock */
    len = out - in;
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
            out8 = pos + (out8 - in);    /* keep the lengths from the stock in */
            in = pos;
            out = pos + len;
        }
    }
    if (out >= tracklen)                 /* the stock-in fallback moved it */
        out = tracklen - 1;
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
            quant_loops_drift_owner = quant_loops_last_in;
        }
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
    QL_IN_SECTOR = (s16)t;
    quant_loops_in_count++;
}

/* Pools 0x0C0E0090 and 0x0C0E02C0 (audio_send_cmd) in the LOOP OUT path. */
void quant_loops_loop_out(int cmd, int flag, u32 a3, int mode, int sector, int a6, int a7, int src)
{
    if ((cmd & 0xFF) == 1 && (mode & 0xFF) == 3 && C850_IS_USB_MODE == 1 && ql_on()) {
        int s = sector & 0xFFFF, t;
        u32 pos, in, out;
        if (s < 2400) {
            pos = QL_SECTOR_FRAME(s);
            in = C850_LOOP_IN_FRAME;
            out = quant_loops_out_point(in, pos);
            quant_loops_drift_owner = in;
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
        }
    }
    c850_audio_send_cmd(cmd, flag, a3, mode, sector, a6, a7, src);
}

/* ---- drift: per-pass loop length correction (docs 4b.9, "Loop drift") ----
 *
 * A loop's points are whole frames, so a loop of q/16 grid beats is L =
 * q * 28125 / BPMx100 frames (constant tempo) but plays n = out - in whole
 * frames per pass. Each wrap jumps back n instead of L: the track drifts
 * against the grid by n - L per pass (5..8 ms at 128 BPM), and an EXIT keeps
 * the sum. This tick counts the wraps and keeps e = sum(n) - passes * L,
 * and after each wrap sets the out for the next pass to in + round(L - e)
 * (Bresenham): passes alternate between floor(L) and ceil(L) frames and |e|
 * stays within half a frame (6.7 ms) after the second wrap, however long the
 * loop plays. The first pass keeps the out the loop was made with.
 *
 * Only loops quant_loops made (quant_loops_drift_owner: an auto beat loop or
 * LOOP IN/OUT, also after loop_resize changed its out, since the in stays);
 * hot-cue and memory loops from the stick are never moved, so recording one
 * back saves the stored out. Of those, only loops the grid can name: the in on the grid (as quant_loops_auto_in
 * and loop_resize find it) and a length within one frame of q/16 beats,
 * 1 <= q <= 512, at least QL_DRIFT_MIN frames. Everything else is left alone.
 *
 * The out moves through the stock resize (auto_beat_loop_set, its state-4
 * branch FUN_0C0E5668 -> FUN_0C00A4CC -> audio cmd 6 with the resize flag,
 * exactly what a BEAT button does inside a loop), with CA90 set for one call
 * so that FUN_0C0E5668 gives the wanted out (as loop_resize does). Called
 * directly, not through FUN_0C0E5390 (that one also resets display state,
 * FUN_0C06A244); the bytes the call writes besides the loop records (beat
 * LED 0x0C1E1088, 0x0CE6D155/0x0CE6D156, CA90) are put back.
 */
#define QL_DRIFT_MIN        12u      /* frames: shorter loops are not corrected */
#define QL_DRIFT_SLACK      6u       /* frames a tick may lag the playhead at a wrap */
#define QL_MAX_Q            512u     /* 32 beats in 1/16 beats */
#define QL_AUTO_BEAT_LOOP   C850_FN(0x0C0E570A, u32, (u32 beats))  /* auto_beat_loop_set */
#define QL_BEAT_LOCK        C850_U8(0x0CE6D26A)    /* 0xFF: FUN_0C0E5390 does nothing */
#define QL_D155             C850_U8(0x0CE6D155)
#define QL_D156             C850_U8(0x0CE6D156)

/* Run-time switch (1 = correct; 0 = loops play the out they were given).
 * Correction also needs the on-deck setting (UTILITY QUANTIZE DRIFT, ql_drift). */
volatile u32 quant_loops_drift_on = 1;

/* A full HISTORY name leaves no room for the flag: keep the choice in RAM
 * until power-off (0 = none, 1 = on, 2 = off). */
static u8 qd_override;

static int ql_drift(void)
{
    if (qd_override != 0u)
        return qd_override == 1u;
    return !c850_ui_flag(C850_UI_FLAG_DRIFT_OFF);
}

static const char *ql_q_utility_value(void)
{
    if (!quant_loops_on)
        return 0;
    return c850_ui_quantize() ? "ON" : "OFF";
}

static void ql_q_utility_select(void)
{
    c850_ui_set_flag(C850_UI_FLAG_QUANT_OFF, c850_ui_quantize());
}

static const c850_ui_utility_item ql_q_utility_item = {
    15u, "QUANTIZE", ql_q_utility_value, ql_q_utility_select,
};
C850_UI_UTILITY_ITEM(ql_q_utility_item);

/* UTILITY: QUANTIZE DRIFT ON/OFF after VERSION No. (and after QUANTIZE /
 * STANDBY TIME); c850_ui.c owns the list. Saved in the mod flags
 * (settings +0x21 bit 1); a fresh deck has drift correction on. */
static const char *ql_utility_value(void)
{
    if (!quant_loops_on || !c850_ui_quantize())
        return 0;
    return ql_drift() ? "ON" : "OFF";
}

static void ql_utility_select(void)
{
    int on = !ql_drift();

    if (c850_ui_set_flag(C850_UI_FLAG_DRIFT_OFF, !on))
        qd_override = 0;
    else
        qd_override = on ? 1u : 2u;
}

static const c850_ui_utility_item ql_utility_item = {
    20u, "QUANTIZE DRIFT", ql_utility_value, ql_utility_select,
};
C850_UI_UTILITY_ITEM(ql_utility_item);

/* Evidence for tests (the loop being tracked). */
volatile u32 quant_loops_drift_ticks;      /* tick calls */
volatile u32 quant_loops_drift_loops;      /* loops taken on (reset with a grid length) */
volatile u32 quant_loops_drift_passes;     /* wraps counted in this loop */
volatile u32 quant_loops_drift_sum;        /* sum of their lengths (frames) */
volatile s32 quant_loops_drift_err;        /* sum - passes * L, in 1/den frames */
volatile u32 quant_loops_drift_num;        /* L = num / den frames, 0 = not tracked */
volatile u32 quant_loops_drift_den;
volatile u32 quant_loops_drift_q;          /* loop length in 1/16 beats */
volatile u32 quant_loops_drift_moves;      /* outs moved (all loops) */
volatile u32 quant_loops_drift_refused;    /* moves the stock resize did not take */
volatile u32 quant_loops_drift_lost;       /* tracking dropped (a jump that is no wrap) */

/* Which out plays: the request 0x0C7F64DC when it changed after the record
 * 0x0C7F64B0 last did, else the record (loop_resize.c, lr_track: a resize
 * writes the request, a new loop / RELOOP / EXIT the record). */
static u32 qd_dc, qd_b0, qd_fresh;
static u32 qd_in, qd_out, qd_last, qd_want, qd_have;
static u32 qd_expect;                     /* an out we asked for that the request has not shown yet */

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

/* Grid time (ms) of position p sixteenths of a beat after beat i. */
u32 quant_loops_grid_ms(int i, u32 p)
{
    u32 t0 = quant_loops_beat_ms_after(i, p >> 4), r = p & 15u;

    if (r == 0)
        return t0;
    return t0 + (r * (quant_loops_beat_ms_after(i, (p >> 4) + 1u) - t0) + 8u) / 16u;
}

/* Out frame of a loop q/16 beats long from in, which is step s (0..15) of
 * beat i, or off the grid (s = 16: in + the grid duration). */
static u32 qd_grid_out(int i, u32 s, u32 in, u32 q)
{
    if (s < 16u)
        return quant_loops_ms_to_frames(quant_loops_grid_ms(i, s + q));
    return in + quant_loops_ms_to_frames(quant_loops_grid_ms(i, q) - C850_GRID_TIME_MS(i));
}

static u32 qd_tz(u32 q)
{
    u32 n = 0;

    while (q && !(q & 1u)) {
        q >>= 1;
        n++;
    }
    return n;
}

/*
 * The exact length of the loop [in, out) on the grid: L = *num / *den frames
 * and its q (1/16 beats). 0 when the grid cannot name it. Constant tempo over
 * the loop: den = BPMx100 and num = q * 28125 (q/16 beats * 60/BPM s * 75);
 * a tempo change inside it: den = 1000 and num = the grid duration in ms * 75.
 */
u32 quant_loops_drift_length(u32 in, u32 out, u32 *num, u32 *den)
{
    int i = quant_loops_beat_at_or_before(in);
    u32 n = quant_loops_window_n();
    u32 s, lo, hi, q, best = 0, d_best = 2, p0, bpm, j, last;

    if (i < 0 || out <= in || out - in < QL_DRIFT_MIN || !quant_loops_grid_ok(i))
        return 0;
    for (s = 0; s < 16u; s++)
        if (quant_loops_ms_to_frames(quant_loops_grid_ms(i, s)) == in)
            break;
    lo = 1;
    hi = QL_MAX_Q;
    while (lo < hi) {                     /* first q with grid out >= out */
        u32 mid = (lo + hi) >> 1;
        if (qd_grid_out(i, s, in, mid) < out)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (q = lo > 1 ? lo - 1 : lo; q <= lo; q++) {
        u32 f = qd_grid_out(i, s, in, q), d = f > out ? f - out : out - f;
        if (d < d_best || (d == d_best && best && qd_tz(q) > qd_tz(best))) {
            d_best = d;
            best = q;
        }
    }
    if (!best)
        return 0;
    p0 = s < 16u ? s : 0;
    bpm = C850_GRID_BPM100(i);
    last = (u32)i + (p0 + best + 15u) / 16u;
    for (j = (u32)i + 1u; j <= last && j < n; j++)
        if (C850_GRID_BPM100(j) != bpm)
            break;
    if (bpm != 0 && (j > last || j >= n)) {
        *num = best * 28125u;
        *den = bpm;
    } else {
        *num = (quant_loops_grid_ms(i, p0 + best) - quant_loops_grid_ms(i, p0)) * 75u;
        *den = 1000u;
    }
    return best;
}

/* The out of the next pass after a wrap: in + round(L - e) frames. */
u32 quant_loops_drift_next(u32 num, u32 den, s32 err)
{
    s32 t = (s32)num - err;               /* > 0: L >= QL_DRIFT_MIN frames, |e| < 1 frame */

    return ((u32)t + den / 2u) / den;
}

/* Move the loop out to nout through the stock resize; 1 = the request took it. */
static int qd_move(u32 in, u32 nout)
{
    u32 frac = C850_BEAT_FRACTION_MODE ? 1u : 0u;
    u32 ca90 = QL_CA90;
    u8 beats = C850_AUTO_LOOP_BEATS, d155 = QL_D155, d156 = QL_D156;

    if (QL_BEAT_LOCK == 0xFF)
        return 0;
    /* FUN_0C0E5668 with beats 8: out = in + (CA90 - in) / (frac * 15 + 1) */
    QL_CA90 = in + (nout - in) * (frac * 15u + 1u);
    QL_AUTO_BEAT_LOOP(8);
    QL_CA90 = ca90;
    C850_AUTO_LOOP_BEATS = beats;
    QL_D155 = d155;
    QL_D156 = d156;
    qd_track();                           /* the request is ours: fresh */
    return C850_LOOP_OUT_REQUEST == nout;
}

/* Start tracking the loop [in, out) with the playhead at pos. */
static void qd_reset(u32 in, u32 out, u32 pos)
{
    u32 num = 0, den = 1;

    qd_in = in;
    qd_out = out;
    qd_last = pos;
    qd_want = 0;
    qd_expect = 0;
    quant_loops_drift_q = quant_loops_drift_length(in, out, &num, &den);
    quant_loops_drift_num = quant_loops_drift_q ? num : 0;
    quant_loops_drift_den = den;
    quant_loops_drift_passes = 0;
    quant_loops_drift_sum = 0;
    quant_loops_drift_err = 0;
    if (quant_loops_drift_q)
        quant_loops_drift_loops++;
    qd_have = 1;
}

/* Hook of the shared state-4 tick c850_loop_tick (c850.h): once per player
 * cycle while a loop plays, before the stock handler. */
void quant_loops_drift_tick(void)
{
    u32 in, out, pos, n;

    quant_loops_drift_ticks++;
    qd_track();
    if (!ql_on() || !quant_loops_drift_on || !ql_drift() || C850_IS_USB_MODE != 1 ||
        C850_LOOP_ACTIVE != 1 || C850_PLAYER_STATE != C850_STATE_LOOPING) {
        qd_have = 0;
        return;
    }
    in = C850_LOOP_IN_FRAME;
    out = quant_loops_effective_out(in);
    pos = C850_PLAY_FRAME;
    if (qd_have && in == qd_in && out != qd_out && out == qd_expect) {
        qd_out = out;                     /* our move, applied by the audio task later */
        qd_expect = 0;
        quant_loops_drift_moves++;
    }
    if (in != quant_loops_drift_owner) {
        quant_loops_drift_owner = 0xFFFFFFFFu;  /* another loop played: ownership ends */
        qd_have = 0;
        return;
    }
    if (!qd_have || in != qd_in || out != qd_out) {
        qd_reset(in, out, pos);           /* a new loop, a resize, EXIT/RELOOP */
        return;
    }
    if (!quant_loops_drift_num) {
        qd_last = pos;
        return;
    }
    if (pos < qd_last) {
        if (qd_last + QL_DRIFT_SLACK < out || pos > in + QL_DRIFT_SLACK) {
            quant_loops_drift_lost++;     /* a jump that is no wrap (jog, a seek) */
            qd_reset(in, out, pos);
            return;
        }
        n = out - in;
        quant_loops_drift_passes++;
        quant_loops_drift_sum += n;
        quant_loops_drift_err += (s32)(n * quant_loops_drift_den) - (s32)quant_loops_drift_num;
        n = quant_loops_drift_next(quant_loops_drift_num, quant_loops_drift_den, quant_loops_drift_err);
        qd_want = n != out - in ? in + n : 0;
    }
    qd_last = pos;
    /* only while the playhead is in the first half of the next pass, so the
     * new out is never behind it (the feeder would jump to in at once) */
    if (qd_want && pos >= in && pos - in < (qd_want - in) / 2u) {
        u32 want = qd_want;
        qd_want = 0;
        if (qd_move(in, want)) {
            qd_out = want;
            quant_loops_drift_moves++;
        } else {
            qd_expect = want;             /* counted when it shows up, else refused */
            quant_loops_drift_refused++;
        }
    }
}

