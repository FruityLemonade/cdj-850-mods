/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * quant_loops (CDJ-350 MAIN 1.14): loops snap to the rekordbox beat grid
 * (see ../patch.py and docs/cdj350.md).
 *
 * Only when a beat grid of the loaded USB track is in RAM; otherwise every
 * path below hands the stock values through unchanged.
 *
 * Integer only (no FPU). Products stay below 2^32: ms * 75 for ms < 57e6
 * (15.9 h), beats * 6000000 for beats <= 8.
 */
#include "c350.h"

#define QL_BPM_CLAMP_STOCK  1800    /* 180.0 BPM clamp */
#define QL_NONE             (-1)    /* no beat (lead-in before the first beat) */
#define QL_AWAY             (-2)    /* the window in RAM does not cover the position */

/* Run-time switch. 1 = the mod may snap; 0 = stock. */
volatile u32 quant_loops_on = 1;

static int ql_on(void)
{
    return quant_loops_on != 0;
}

/* Pending grid out of the auto loop being set (quant_loops_auto_in ->
 * quant_loops_auto_send -> post-send quant_loops_auto_in). */
static u32 ql_pending;
static u32 ql_post_send;
static s16 ql_in_sector = -1;
static u32 ql_pending_out;

/* Evidence for tests: last values and counters. */
volatile u32 quant_loops_last_in;
volatile u32 quant_loops_drift_owner = 0xFFFFFFFFu;
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

int quant_loops_auto_points(u32 pos, u32 beats, u32 *in, u32 *out)
{
    u32 t_in, t_out, fin, fout;
    int i;

    if (beats == 0 || beats > 16)
        return 0;
    i = quant_loops_beat_nearest(pos);
    if (i < 0 || !quant_loops_grid_ok(i))
        return 0;
    t_in = C350_GRID_TIME_MS(i);
    t_out = quant_loops_beat_ms_after(i, beats);
    fin = quant_loops_ms_to_frames(t_in);
    fout = quant_loops_ms_to_frames(t_out);
    if (fout <= fin)
        fout = fin + 1;
    if (fout <= pos)
        return 0;
    *in = fin;
    *out = fout;
    return 1;
}

u32 quant_loops_in_point(u32 pos)
{
    int i = quant_loops_beat_nearest(pos);
    if (i < 0 || !quant_loops_grid_ok(i))
        return pos;
    return quant_loops_beat_frame(i);
}

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
            return pos;
    }
    return quant_loops_beat_frame(i);
}

int quant_loops_sector_of(int s, u32 pos, u32 frame)
{
    int t = c350_ring_add(s, (int)(frame - pos));
    if (t < 0 || t >= 2400)
        return -1;
    if (c350_ring_sector_to_frame(t) != frame || c350_sector_track(t) != c350_sector_track(s))
        return -1;
    return t;
}

/* ---- redirect entry points ---- */

/* Pool 0x0C0CE8B4 (0x0C006A92) in auto_beat_loop_set (0x0C0CDE6E), called
 * before the out calculation (0x0C0CE696) and after audio_send_cmd (0x0C0CE848). */
void quant_loops_auto_in(void)
{
    int s;
    u32 pos, in, out, len, tracklen, slot;

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
    tracklen = C350_TRACKLEN(slot & 3u);
    if (tracklen != 0 && out >= tracklen)
        out = tracklen - 1u;
    if (out <= pos || out <= in)
        return;
    len = out - in;
    if (in != pos) {
        int t = quant_loops_sector_of(s, pos, in);
        if (t >= 0) {
            C350_IN_SECTOR = (s16)t;
            ql_in_sector = (s16)t;
            quant_loops_in_count++;
        } else {
            quant_loops_refused++;
            in = pos;
            out = pos + len;
            if (tracklen != 0 && out >= tracklen)
                out = tracklen - 1u;
        }
    }
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
            quant_loops_drift_owner = quant_loops_last_in;
        } else if (C350_IS_USB_MODE == 1 && ql_on() && C350_PLAYER_STATE == C350_STATE_CUED) {
            u32 pos = C350_CUE_FRAME, in, out, tracklen;
            if (quant_loops_auto_points(pos, 4u, &in, &out)) {
                out = pos + (out - in);
                tracklen = C350_TRACKLEN(C350_CUE_TRACK & 3u);
                if (tracklen != 0 && out >= tracklen)
                    out = tracklen - 1u;
                if (out > pos) {
                    frame = out;
                    C350_LOOP_OUT_CMD = frame;
                    C350_LOOP_D1C0 = frame;
                    quant_loops_auto_count++;
                    quant_loops_last_pos = pos;
                    quant_loops_last_in = pos;
                    quant_loops_last_out = frame;
                    quant_loops_drift_owner = pos;
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

/* Pools 0x0C0D1688 and 0x0C0D1984 (audio_send_cmd) in the LOOP OUT paths. */
void quant_loops_loop_out(int cmd, int flag, u32 a3, int mode, int sector, int a6, int a7, int src)
{
    if ((cmd & 0xFF) == 1 && (mode & 0xFF) == 3 && C350_IS_USB_MODE == 1 && ql_on()) {
        int s = sector & 0xFFFF, t;
        u32 pos, in, out;
        if (s < 2400) {
            pos = c350_ring_sector_to_frame(s);
            in = C350_LOOP_IN_FRAME;
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
    c350_audio_send_cmd(cmd, flag, a3, mode, sector, a6, a7, src);
}

/* ---- drift: per-pass loop length correction ---- */

#define QL_DRIFT_MIN        12u
#define QL_DRIFT_SLACK      6u
#define QL_MAX_Q            512u

volatile u32 quant_loops_drift_on = 1;

volatile u32 quant_loops_drift_ticks;
volatile u32 quant_loops_drift_loops;
volatile u32 quant_loops_drift_passes;
volatile u32 quant_loops_drift_sum;
volatile s32 quant_loops_drift_err;
volatile u32 quant_loops_drift_num;
volatile u32 quant_loops_drift_den;
volatile u32 quant_loops_drift_q;
volatile u32 quant_loops_drift_moves;
volatile u32 quant_loops_drift_refused;
volatile u32 quant_loops_drift_lost;

static u32 qd_dc, qd_b0, qd_fresh;
static u32 qd_in, qd_out, qd_last, qd_want, qd_have;
static u32 qd_expect;

static void qd_track(void)
{
    u32 dc = C350_LOOP_OUT_REQUEST, b0 = C350_LOOP_OUT_FRAME;
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

u32 quant_loops_grid_ms(int i, u32 p)
{
    u32 t0 = quant_loops_beat_ms_after(i, p >> 4), r = p & 15u;

    if (r == 0)
        return t0;
    return t0 + (r * (quant_loops_beat_ms_after(i, (p >> 4) + 1u) - t0) + 8u) / 16u;
}

static u32 qd_grid_out(int i, u32 s, u32 in, u32 q)
{
    if (s < 16u)
        return quant_loops_ms_to_frames(quant_loops_grid_ms(i, s + q));
    return in + quant_loops_ms_to_frames(quant_loops_grid_ms(i, q) - C350_GRID_TIME_MS(i));
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
    while (lo < hi) {
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
    bpm = C350_GRID_BPM100(i);
    last = (u32)i + (p0 + best + 15u) / 16u;
    for (j = (u32)i + 1u; j <= last && j < n; j++)
        if (C350_GRID_BPM100(j) != bpm)
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

u32 quant_loops_drift_next(u32 num, u32 den, s32 err)
{
    s32 t = (s32)num - err;

    return ((u32)t + den / 2u) / den;
}

static int qd_move(u32 nout)
{
    u32 tracklen = C350_TRACK_LENGTH;

    if (tracklen != 0 && nout >= tracklen)
        return 0;
    C350_LOOP_OUT_CMD = nout;
    c350_audio_send_cmd(0x1E, 0, nout, 1, 0, 0, 0, C350_SOURCE_INDEX);
    qd_track();
    return C350_LOOP_OUT_REQUEST == nout;
}

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

void quant_loops_drift_tick(void)
{
    u32 in, out, pos, n;

    quant_loops_drift_ticks++;
    qd_track();
    if (!ql_on() || !quant_loops_drift_on || C350_IS_USB_MODE != 1 ||
        C350_LOOP_ACTIVE != 1 || C350_PLAYER_STATE != C350_STATE_LOOPING) {
        qd_have = 0;
        return;
    }
    in = C350_LOOP_IN_FRAME;
    out = quant_loops_effective_out(in);
    pos = C350_PLAY_FRAME;
    if (qd_have && in == qd_in && out != qd_out && out == qd_expect) {
        qd_out = out;
        qd_expect = 0;
        quant_loops_drift_moves++;
    }
    if (in != quant_loops_drift_owner) {
        quant_loops_drift_owner = 0xFFFFFFFFu;
        qd_have = 0;
        return;
    }
    if (!qd_have || in != qd_in || out != qd_out) {
        qd_reset(in, out, pos);
        return;
    }
    if (!quant_loops_drift_num) {
        qd_last = pos;
        return;
    }
    if (pos < qd_last) {
        if (qd_last + QL_DRIFT_SLACK < out || pos > in + QL_DRIFT_SLACK) {
            quant_loops_drift_lost++;
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
    if (qd_want && pos >= in && pos - in < (qd_want - in) / 2u) {
        u32 want = qd_want;
        qd_want = 0;
        if (qd_move(want)) {
            qd_out = want;
            quant_loops_drift_moves++;
        } else {
            qd_expect = want;
            quant_loops_drift_refused++;
        }
    }
}

void quant_loops_drift_entry(void)
{
    quant_loops_drift_tick();
    c350_state4_keys();
}
