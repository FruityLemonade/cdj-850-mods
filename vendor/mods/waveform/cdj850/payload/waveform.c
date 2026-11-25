/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * waveform: zoomed scrolling half-waveform in the 100x5 INFO Page 1 strip
 * (CDJ-850 MAIN 1.13).
 *
 * How it works:
 *   - waveform_tx_waveform (pool 0x0C064DD8 -> panel_tx_waveform 0x0C0662AE)
 *     packs the 100-byte buffer at 0x0C9D1199 into SCIF1 frames 0x70..0x77
 *     and bumps sequence 0x0C9D1184 whenever 0x0C9D1188 == 1. When zoomed,
 *     0..15 heights (0..5 VFD dots) are transmitted verbatim so true silence
 *     renders as 0 dots without terminating the strip; overview falls back to
 *     stock 0x0C0662AE.
 *   - waveform_hook runs at 50 Hz in DisplayTask on the shared display_stage_bpm
 *     tail (after FUN_0C066402 and info_order, before waveform_tx_waveform). It
 *     never touches dot-matrix text rows or DB records, so it does not
 *     interfere with info_order or phase_meter.
 *   - waveform_rotary_hook runs in FUN_0C0E655C (0x0C0E782E) when the rotary
 *     encoder is turned in INFO mode (nav_view_selector == 0, not in BPM Lock
 *     edit mode). At cursor 0 on Page 1:
 *       CCW zooms out: 0 (close, 4 s) -> 1 (mid, 8 s) -> 2 (medium, 16 s) -> 3 (overview)
 *       CW zooms back in: 3 -> 2 -> 1 -> 0, and once at 0 advances cursor normally
 *       to row 2 / INFO Page 2+.
 *   - waveform_center_marker replaces the 0x60 red strip on zoomed views with
 *     active loop bars, beat markers (at close & mid zoom when a beat grid is
 *     in RAM), and the playhead tick (playhead and beat markers are inverted/dark
 *     inside active loop spans). Loop-in and cue points stay on the bottom
 *     white/blue bar (0x0C9D0AEF, via waveform_cue_column).
 *   - Audio data comes directly from the 2400-sector (32 s) PCM ring buffer at
 *     0x0C1E8E5C (cached per ring slot so each newly decoded sector is scanned
 *     once with peak-blended transients and per-track loudness adaptation),
 *     seamlessly falling back to the 400-column ANLZ PWAV buffer at 0x0CE65862
 *     or the 100-column overview at 0x0CE65B82 outside the buffered PCM window.
 */
#include "c850_ui.h"

#define WF_COLS              100
#define WF_THIRD_COLUMN      (WF_COLS / 3)
#define WF_CENTER_COLUMN     (WF_COLS / 2)

#define WF_POS_THIRD         0u   /* default: 1/3 width, left-aligned (col 33) */
#define WF_POS_CENTER        1u   /* centered (col 50) */
#define WF_POS_LEFT          2u   /* left edge (col 0) */
#define WF_POS_COUNT         3u
#define WF_POS_FLAGS_MASK    (C850_UI_FLAG_WAVE_LEFT | C850_UI_FLAG_WAVE_CENTER)

#define WF_ZOOM_CLOSE        0u   /* 3 frames/col = 300 frames = 4.0 s window */
#define WF_ZOOM_MID          1u   /* 6 frames/col = 600 frames = 8.0 s window */
#define WF_ZOOM_MEDIUM       2u   /* 12 frames/col = 1200 frames = 16.0 s window */
#define WF_ZOOM_OVERVIEW     3u   /* stock whole-track 100-col overview */

#define WF_STEP_CLOSE        3
#define WF_STEP_MID          6
#define WF_STEP_MEDIUM       12
#define WF_DROPOUT_HOLD_MS   250u
#define WF_DROPOUT_HOLD_FRAMES ((s32)((WF_DROPOUT_HOLD_MS * 75u + 999u) / 1000u))

#define WF_MODE_TITLE        10u  /* INFO Page 1 (title + artist/BPM + waveform) */
#define WF_TX_LAST           C850_U32(0x0C9D0AC0)
#define WF_TX_FRAMES         0x0C9D07D0u
#define WF_SLOT_VALID_BIT    0x80000000u
#define WF_GRID_VALID        C850_U32(C850_GRID_CTX + 0x70u)

#define WF_REF_DEFAULT       29u  /* ~ -4.5 dBFS default track peak */
#define WF_REF_MIN_PWAV      20u  /* ~ -18 dBFS min track peak when PWAV confirms */
#define WF_REF_MIN_NO_PWAV   24u  /* ~ -12 dBFS min track peak without PWAV */
#define WF_NO_PWAV_ADAPT_MIN 18u  /* require >= -21 dBFS peak before boosting */
#define WF_NO_PWAV_ACTIVE    150u /* ~2 s of non-silent audio before lowering ref */

/* Run-time switches & diagnostic counters (readable via QMP/gdb in tests). */
volatile u32 waveform_on = 1;
volatile u32 waveform_zoom = WF_ZOOM_CLOSE;
volatile u32 waveform_updates;
volatile u32 waveform_rotary_events;
volatile u32 waveform_pcm_hits;
volatile u32 waveform_pwav_hits;

static volatile u8 wf_force_redraw = 1;
static u8 wf_was_zoomed;
static u8 wf_was_mode10;
static u8 wf_buf[WF_COLS];
static u8 wf_col_lvl[WF_COLS];
static u8 wf_last_sent[WF_COLS];
static u8 wf_last_had_pcm;
static u8 wf_last_source;
static s16 wf_last_track;
static s32 wf_last_pcm_frame;
static u32 wf_last_pcm_ms;
/* A full HISTORY name leaves no room for mod flags. Keep that choice in RAM. */
static u8 wf_position_override;
static u32 wf_loop_dc_seen;
static u32 wf_loop_b0_seen;
static u8 wf_loop_dc_fresh;

/* Per-track loudness adaptation state. */
static u8 wf_adapt_source;
static s16 wf_adapt_track = -1;
static u8 wf_adapt_ref = WF_REF_DEFAULT;
static u8 wf_pcm_peak;
static u8 wf_pwav_peak_confirmed;
static u32 wf_pcm_active;

static s32 wf_zoom_step(u32 zoom)
{
    if (zoom == WF_ZOOM_CLOSE)
        return WF_STEP_CLOSE;
    if (zoom == WF_ZOOM_MID)
        return WF_STEP_MID;
    return WF_STEP_MEDIUM;
}

static u32 wf_playhead_pos(void)
{
    if (wf_position_override != 0u)
        return (u32)(wf_position_override - 1u);
    if (c850_ui_flag(C850_UI_FLAG_WAVE_LEFT))
        return WF_POS_LEFT;
    if (c850_ui_flag(C850_UI_FLAG_WAVE_CENTER))
        return WF_POS_CENTER;
    return WF_POS_THIRD;
}

static s32 wf_playhead_column(u32 pos)
{
    if (pos == WF_POS_LEFT)
        return 0;
    if (pos == WF_POS_CENTER)
        return WF_CENTER_COLUMN;
    return WF_THIRD_COLUMN;
}

static const char *wf_playhead_label(u32 pos)
{
    if (pos == WF_POS_LEFT)
        return "LEFT";
    if (pos == WF_POS_CENTER)
        return "CENTER";
    return "LEFT 1/3";
}

static void wf_track_loop_out(void)
{
    u32 dc = C850_LOOP_OUT_REQUEST, b0 = C850_LOOP_OUT_FRAME;

    if (b0 != wf_loop_b0_seen)
        wf_loop_dc_fresh = 0;
    else if (dc != wf_loop_dc_seen)
        wf_loop_dc_fresh = 1;
    wf_loop_dc_seen = dc;
    wf_loop_b0_seen = b0;
}

static u32 wf_effective_loop_out(u32 in)
{
    wf_track_loop_out();
    if (wf_loop_dc_fresh && C850_LOOP_OUT_REQUEST > in)
        return C850_LOOP_OUT_REQUEST;
    return C850_LOOP_OUT_FRAME;
}

static const char *wf_mod_utility_value(void)
{
    if (!waveform_on)
        return 0;
    return c850_ui_flag(C850_UI_FLAG_WAVEFORM_OFF) ? "OFF" : "ON";
}

static void wf_mod_utility_select(void)
{
    c850_ui_set_flag(C850_UI_FLAG_WAVEFORM_OFF,
                     !c850_ui_flag(C850_UI_FLAG_WAVEFORM_OFF));
    wf_force_redraw = 1;
}

static const c850_ui_utility_item wf_mod_utility_item = {
    90u, "WAVEFORM", wf_mod_utility_value, wf_mod_utility_select,
};
C850_UI_UTILITY_ITEM(wf_mod_utility_item);

/* UTILITY: WAVE PLAYHEAD, visible while WAVEFORM is ON (c850_ui.c owns
 * the list; the encoder press cycles Left 1/3 -> Center -> Left). */
static const char *wf_utility_value(void)
{
    if (!waveform_on || c850_ui_flag(C850_UI_FLAG_WAVEFORM_OFF))
        return 0;
    return wf_playhead_label(wf_playhead_pos());
}

static void wf_utility_select(void)
{
    u32 next = (wf_playhead_pos() + 1u) % WF_POS_COUNT;
    u32 flags = (next == WF_POS_LEFT) ? C850_UI_FLAG_WAVE_LEFT
              : (next == WF_POS_CENTER) ? C850_UI_FLAG_WAVE_CENTER
              : 0u;

    if (c850_ui_set_flags(WF_POS_FLAGS_MASK, flags))
        wf_position_override = 0;
    else
        wf_position_override = (u8)(next + 1u);
    wf_force_redraw = 1;
}

static const c850_ui_utility_item wf_utility_item = {
    95u, "WAVE PLAYHEAD", wf_utility_value, wf_utility_select,
};
C850_UI_UTILITY_ITEM(wf_utility_item);

static void wf_set_red_col(volatile u8 *frame, u32 col)
{
    frame[2u + col / 8u] |= (u8)(0x80u >> (col % 8u));
}

static void wf_clear_red_col(volatile u8 *frame, u32 col)
{
    frame[2u + col / 8u] &= (u8)~(0x80u >> (col % 8u));
}

static int wf_red_col_is_set(const volatile u8 *frame, u32 col)
{
    return (frame[2u + col / 8u] & (u8)(0x80u >> (col % 8u))) != 0u;
}

static void wf_toggle_red_col(volatile u8 *frame, u32 col)
{
    frame[2u + col / 8u] ^= (u8)(0x80u >> (col % 8u));
}

static void wf_mark_span(volatile u8 *frame, s32 base, s32 step, u32 in_f, u32 out_f)
{
    s32 in_s = (s32)in_f;
    s32 out_s = (s32)out_f;
    s32 c0, c1, col;

    if (out_f <= in_f || out_s <= base)
        return;
    c0 = (in_s <= base) ? 0 : (in_s - base) / step;
    c1 = (out_s - 1 - base) / step;
    if (c1 >= WF_COLS)
        c1 = WF_COLS - 1;
    for (col = c0; col <= c1 && col < WF_COLS; col++)
        wf_set_red_col(frame, (u32)col);
}

static u32 wf_ms_to_frame(u32 ms)
{
    if (ms <= 50000000u)
        return (ms * 75u + 500u) / 1000u;
    return (ms / 1000u) * 75u + ((ms % 1000u) * 75u + 500u) / 1000u;
}

static u32 wf_frame_to_ms(u32 frame)
{
    return (frame / 3u) * 40u + ((frame % 3u) * 40u) / 3u;
}

/* Toggle a 1-column marker at every beat of the RAM beat grid inside
 * [base, base + WF_COLS * step). On an empty strip this lights beat ticks;
 * inside a pre-drawn loop span it cuts a dark/negative tick. */
static void wf_mark_beats(volatile u8 *frame, s32 base, s32 step)
{
    u32 total, first, n, base_ms, lo, hi, i;
    s32 end_f = base + WF_COLS * step;
    u32 last_col = WF_COLS;

    if (C850_IS_USB_MODE != 1u || end_f <= 0 || WF_GRID_VALID == 0u)
        return;

    total = C850_GRID_BEAT_COUNT;
    first = (u32)C850_GRID_WINDOW_INDEX * 500u;
    if (total <= first)
        return;
    n = total - first;
    if (n > 1000u)
        n = 1000u;
    if (n == 0u)
        return;

    base_ms = (base > 0) ? wf_frame_to_ms((u32)base) : 0u;
    if (base_ms > 15u)
        base_ms -= 15u;

    lo = 0u;
    hi = n;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2u;
        if (C850_GRID_TIME_MS(mid) < base_ms)
            lo = mid + 1u;
        else
            hi = mid;
    }

    for (i = lo; i < n; i++) {
        s32 f = (s32)wf_ms_to_frame(C850_GRID_TIME_MS(i));
        if (f >= end_f)
            break;
        if (f >= base) {
            u32 col = (u32)((f - base) / step);
            if (col != last_col) {
                wf_toggle_red_col(frame, col);
                last_col = col;
            }
        }
    }
}

/* Pool 0x0C063128 (display_stage_active_cue_column, 0x0C0642CC): on zoomed
 * views, clear the whole-track white cue column (S[0x17] = 0x0C9D0AEF)
 * and place the active cue point at its zoomed waveform column instead. */
void waveform_cue_column(void)
{
    C850_FN(0x0C0642CC, void, (void))();
    if (waveform_on && !c850_ui_flag(C850_UI_FLAG_WAVEFORM_OFF) &&
        wf_was_zoomed && waveform_zoom < WF_ZOOM_OVERVIEW &&
        C850_DISPLAY_MODE == WF_MODE_TITLE) {
        u8 st = C850_PLAYER_STATE;

        C850_U8(0x0C9D0AEFu) = 0u;
        if (st != 0u && st != 2u && st != 10u && st != 14u && C850_CUE_OR_LOOP_EXISTS != 0u) {
            u32 cue_f = C850_U8(0x0CE6D140u) ? C850_U32(0x0CE65C18u) : C850_LOOP_IN_FRAME;
            s32 playhead_col = wf_playhead_column(wf_playhead_pos());
            s32 step = wf_zoom_step(waveform_zoom);
            s32 base = (s32)C850_PLAY_FRAME - playhead_col * step;
            s32 p = (s32)cue_f;

            if (p >= base && p < base + WF_COLS * step)
                C850_U8(0x0C9D0AEFu) = (u8)(((p - base) / step) + 1);
        }
    }
}

/* On zoomed views, replace the stock 0x60 red memory-cue bitmap with only
 * active loop bars, beat markers (at close and mid zoom), and the playhead
 * tick. Both beat markers and the playhead are inverted/dark inside an active
 * loop span; outside a loop span the playhead is always lit. Cue / loop-in
 * points remain on the bottom white bar via waveform_cue_column. Overview
 * leaves 0x60 untouched. */
int waveform_center_marker(void)
{
    u32 before = WF_TX_LAST;
    int result = C850_FN(0x0C0664A8, int, (void))();
    u32 last = WF_TX_LAST;
    u32 zoom = waveform_zoom;

    if (waveform_on && !c850_ui_flag(C850_UI_FLAG_WAVEFORM_OFF) &&
        wf_was_zoomed && zoom < WF_ZOOM_OVERVIEW &&
        C850_DISPLAY_MODE == WF_MODE_TITLE &&
        last > before && last < 0x2Fu) {
        volatile u8 *frame = (volatile u8 *)(WF_TX_FRAMES + last * 16u);
        if (frame[0] == 0x60u) {
            s32 playhead_col = wf_playhead_column(wf_playhead_pos());
            s32 step = wf_zoom_step(zoom);
            s32 base = (s32)C850_PLAY_FRAME - playhead_col * step;
            int in_loop;
            u32 i;

            for (i = 2u; i <= 14u; i++)
                frame[i] = 0u;

            /* 1. Active loop span first so beats and playhead invert inside it. */
            if (C850_LOOP_ACTIVE == 1u) {
                u32 in_f = C850_LOOP_IN_FRAME;
                wf_mark_span(frame, base, step, in_f, wf_effective_loop_out(in_f));
            }
            in_loop = wf_red_col_is_set(frame, (u32)playhead_col);

            /* 2. Beat markers at closest and intermediate zoom stages. */
            if (zoom <= WF_ZOOM_MID)
                wf_mark_beats(frame, base, step);

            /* 3. Playhead: dark/inverted inside an active loop span, lit otherwise. */
            if (in_loop)
                wf_clear_red_col(frame, (u32)playhead_col);
            else
                wf_set_red_col(frame, (u32)playhead_col);
        }
    }
    return result;
}

/* Pool 0x0C064DD8 (panel_tx_waveform, 0x0C0662AE): when zoomed, transmit all
 * 100 columns verbatim (0..15) so true silence (0) yields 0 dots without
 * truncating subsequent columns. Overview calls stock 0x0C0662AE. */
u32 waveform_tx_waveform(void)
{
    u8 st;
    u32 last;

    if (!waveform_on || c850_ui_flag(C850_UI_FLAG_WAVEFORM_OFF) ||
        !wf_was_zoomed || waveform_zoom >= WF_ZOOM_OVERVIEW) {
        return C850_FN(0x0C0662AE, u32, (void))();
    }

    st = C850_PLAYER_STATE;
    if (st == 2u || st == 10u || st == 14u)
        return st;
    if ((C850_U8(0x0C9D0B14u) & 0x1Fu) != 0u)
        return 0u;
    if (C850_U32(0x0C9D0AF8u) != 10u || C850_U32(0x0C9D0B04u) != 0x200u)
        return 0u;

    last = WF_TX_LAST;
    if (last >= 0x2Fu)
        return last;

    if (C850_WAVEFORM_CHANGED == 1u) {
        C850_WAVEFORM_SEQUENCE = (u8)(C850_WAVEFORM_SEQUENCE + 1u);
        C850_WAVEFORM_CHANGED = 0u;
    }

    {
        const volatile u8 *samples = (const volatile u8 *)C850_WAVEFORM_SAMPLES;
        u32 idx = 0u;
        u32 frame_id = 0x70u;
        u32 f = last + 1u;

        for (; idx < WF_COLS && f < 0x2Fu; f++, frame_id++) {
            volatile u8 *pkt = (volatile u8 *)(WF_TX_FRAMES + f * 16u);
            u32 b;

            pkt[0] = (u8)frame_id;
            if (frame_id == 0x70u) {
                pkt[1] = C850_WAVEFORM_SEQUENCE;
                pkt[2] = 0u;
                b = 3u;
            } else {
                b = 1u;
            }
            for (; b < 15u; b++) {
                u8 v = (idx < WF_COLS) ? samples[idx++] : 0u;
                pkt[b] = (v > 15u) ? 15u : v;
            }
        }
        WF_TX_LAST = f - 1u;
    }
    return 0u;
}

/* Per-ring-slot cached log amplitude level (0..31) keyed by (valid | track<<20 | frame). */
static u8 wf_slot_level[C850_PCM_RING_SLOTS];
static u32 wf_slot_key[C850_PCM_RING_SLOTS];

/* 1.5 dB/step logarithmic thresholds from -46.5 dBFS (0x009B) to -3.0 dBFS (0x5A9F). */
static const u16 wf_log32_thresh[30] = {
    0x009Bu, 0x00B8u, 0x00DBu, 0x0104u, 0x0135u, 0x0170u,
    0x01B5u, 0x0207u, 0x0269u, 0x02DDu, 0x0368u, 0x040Cu,
    0x04D0u, 0x05B8u, 0x06CCu, 0x0814u, 0x0999u, 0x0B68u,
    0x0D8Fu, 0x101Du, 0x1327u, 0x16C3u, 0x1B0Du, 0x2027u,
    0x2636u, 0x2D6Bu, 0x35FAu, 0x4027u, 0x4C3Fu, 0x5A9Fu
};

/* Map relative-to-track-peak level (0..31) to 0..15 (0..5 VFD dot rows).
 * Quiet passages stay in 1..2 dots (3..6) with hits popping above sustain;
 * 5 dots (13..15) is reserved for the top 3 dB of the track. */
static const u8 wf_rel_to_byte[32] = {
    0,                                  /* 0: true silence -> 0 dots */
    1, 1, 1, 2, 2, 2, 3, 3, 3, 3, 3,    /* 1..11 -> 1 dot */
    4, 4, 5, 5, 6, 6, 6,                /* 12..18 -> 2 dots */
    7, 7, 8, 8, 9, 9,                   /* 19..24 -> 3 dots */
    10, 10, 11, 12,                     /* 25..28 -> 4 dots */
    13, 14, 15,                         /* 29..31 -> 5 dots */
};

/* Map blended 16-bit PCM amplitude to log level 0..31. */
u8 waveform_amp_to_level(u32 amp)
{
    u32 lo = 0u, hi = 30u;

    if (amp < 0x0048u)
        return 0u;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2u;
        if (amp <= (u32)wf_log32_thresh[mid])
            hi = mid;
        else
            lo = mid + 1u;
    }
    return (u8)(lo + 1u);
}

/* Map amplitude to un-adapted 0..15 strip byte (0 = silence, 1..15 = 1..5 dots). */
u8 waveform_amp_to_height(u32 amp)
{
    return wf_rel_to_byte[waveform_amp_to_level(amp)];
}

/* Scan one 2352-byte PCM ring sector (588 stereo 16-bit BE frames) into 0..31,
 * blending mean and peak amplitude so quiet transients remain distinct. */
static u8 wf_scan_pcm_slot(u32 slot)
{
    const volatile s16 *pcm = (const volatile s16 *)(C850_PCM_RING_BASE +
                                                     slot * C850_PCM_SECTOR_BYTES);
    u32 sum = 0u;
    u32 peak = 0u;
    u32 mean;
    int i;

    /* 64 stereo pairs at stride 9 (18 s16s): indices 0..1135 (< 1176). */
    for (i = 0; i < 64; i++) {
        s32 l = (s32)pcm[0];
        s32 r = (s32)pcm[1];
        u32 al = (u32)(l < 0 ? -l : l);
        u32 ar = (u32)(r < 0 ? -r : r);
        if (al > peak)
            peak = al;
        if (ar > peak)
            peak = ar;
        sum += al + ar;
        pcm += 18;
    }
    mean = sum >> 7;
    if (mean < 0x0040u && peak < 0x0060u)
        return 0u;
    return waveform_amp_to_level((mean + peak) >> 1);
}

static u8 wf_pcm_level_cached(u32 slot, u32 track, u32 frame)
{
    u32 key = WF_SLOT_VALID_BIT | ((track & 0x7FFu) << 20) | (frame & 0xFFFFFu);
    u8 lvl;

    if (wf_slot_key[slot] == key)
        return wf_slot_level[slot];

    lvl = wf_scan_pcm_slot(slot);
    wf_slot_level[slot] = lvl;
    wf_slot_key[slot] = key;
    return lvl;
}

/* Check whether 400-column ANLZ PWAV buffer (0x0CE65862) has any non-zero data. */
static int wf_pwav_available(void)
{
    const volatile u8 *pwav = (const volatile u8 *)C850_WAVEFORM_PWAV;
    int i;

    for (i = 0; i < 400; i += 8) {
        if (pwav[i * 2] != 0u)
            return 1;
    }
    return 0;
}

static u8 wf_pwav_bin(u32 frame, u32 track_len)
{
    u32 idx;

    if (track_len == 0u || frame >= track_len)
        return 0u;
    if (frame <= 10000000u)
        idx = (frame * 400u) / track_len;
    else
        idx = ((frame >> 4) * 400u) / (track_len >> 4);
    if (idx >= 400u)
        idx = 399u;
    return C850_U8(C850_WAVEFORM_PWAV + idx * 2u) & 0x1Fu;
}

/* Fallback log level (0..31) for a frame from 400-col PWAV or 100-col overview. */
static u8 wf_anlz_frame_level(u32 frame, u32 track_len, int has_pwav)
{
    u32 idx;

    if (track_len == 0u || frame >= track_len)
        return 0u;

    if (has_pwav) {
        u8 h = wf_pwav_bin(frame, track_len);
        waveform_pwav_hits++;
        return h;
    }

    if (frame <= 40000000u)
        idx = (frame * 100u) / track_len;
    else
        idx = ((frame >> 4) * 100u) / (track_len >> 4);
    if (idx >= 100u)
        idx = 99u;
    {
        u8 v = C850_U8(C850_WAVEFORM_OVERVIEW + idx);
        if (v == 0u)
            return 0u;
        if (v > 15u)
            v = 15u;
        return (u8)(v * 2u + 1u);
    }
}

static void wf_adapt_reset(u8 source, s16 track)
{
    wf_adapt_source = source;
    wf_adapt_track = track;
    wf_adapt_ref = WF_REF_DEFAULT;
    wf_pcm_peak = 0u;
    wf_pwav_peak_confirmed = 0u;
    wf_pcm_active = 0u;
}

static void wf_adapt_update(s32 play_frame, u32 track_len, int has_pwav)
{
    if (wf_pcm_peak > wf_adapt_ref)
        wf_adapt_ref = wf_pcm_peak;

    if (has_pwav && play_frame >= 0 && track_len > 0u) {
        u8 pwav_cur = wf_pwav_bin((u32)play_frame, track_len);
        if (pwav_cur >= 26u && wf_pcm_active >= 16u) {
            u32 est = (u32)wf_pcm_peak + (31u - (u32)pwav_cur) / 3u;
            if (est < WF_REF_MIN_PWAV)
                est = WF_REF_MIN_PWAV;
            if (est > 31u)
                est = 31u;
            wf_pwav_peak_confirmed = 1u;
            if (est < (u32)wf_adapt_ref)
                wf_adapt_ref = (u8)est;
        }
    } else if (!has_pwav && !wf_pwav_peak_confirmed &&
               wf_pcm_active >= WF_NO_PWAV_ACTIVE &&
               wf_pcm_peak >= WF_NO_PWAV_ADAPT_MIN) {
        u8 target = (wf_pcm_peak < WF_REF_MIN_NO_PWAV) ? WF_REF_MIN_NO_PWAV : wf_pcm_peak;
        if (target < wf_adapt_ref)
            wf_adapt_ref = target;
    }
}

static u8 wf_apply_adapt(u32 level, int from_pcm)
{
    u32 rel = level;

    if (level == 0u)
        return 0u;
    if (from_pcm && wf_adapt_ref < 31u)
        rel += (u32)(31u - wf_adapt_ref);
    if (rel > 31u)
        rel = 31u;
    return wf_rel_to_byte[rel];
}

/* Render the next 100 columns and report whether they include any live PCM. */
static __attribute__((noinline)) int wf_render_zoomed(u32 zoom, s32 play_frame, s32 playhead_col)
{
    s32 step = wf_zoom_step(zoom);
    u32 track_len = C850_TRACK_LENGTH;
    u32 r_idx = C850_RING_READ_INDEX;
    u32 w_idx = C850_RING_WRITE_INDEX;
    int ring_ok = 0;
    s32 r_frame = 0;
    u32 cur_track = 0;
    s32 ahead = 0;
    s32 behind = 0;
    int has_pwav = wf_pwav_available();
    int used_pcm = 0;
    int col;

    if (r_idx < C850_PCM_RING_SLOTS && w_idx < C850_PCM_RING_SLOTS) {
        r_frame = (s32)c850_ring_sector_to_frame((int)r_idx);
        cur_track = c850_sector_to_track((int)r_idx);
        ahead = (s32)((w_idx + C850_PCM_RING_SLOTS - r_idx) % C850_PCM_RING_SLOTS);
        behind = (s32)C850_PCM_RING_SLOTS - ahead - 1;
        ring_ok = 1;
    }

    for (col = 0; col < WF_COLS; col++) {
        s32 f0 = play_frame + (col - playhead_col) * step;
        u32 max_lvl = 0u;
        u32 sum_lvl = 0u;
        u32 pcm_count = 0u;
        int k;

        for (k = 0; ring_ok && k < step; k++) {
            s32 f = f0 + k;
            s32 delta = f - r_frame;
            s32 s;
            u8 lvl;

            if (f < 0 || (track_len > 0u && (u32)f >= track_len) ||
                delta < -behind || delta >= ahead) {
                continue;
            }

            s = (s32)r_idx + delta;
            if (s < 0)
                s += (s32)C850_PCM_RING_SLOTS;
            else if (s >= (s32)C850_PCM_RING_SLOTS)
                s -= (s32)C850_PCM_RING_SLOTS;
            if (c850_ring_sector_to_frame(s) != (u32)f ||
                c850_sector_to_track(s) != cur_track) {
                continue;
            }

            lvl = wf_pcm_level_cached((u32)s, cur_track, (u32)f);
            pcm_count++;
            sum_lvl += lvl;
            if (lvl > max_lvl)
                max_lvl = lvl;
            if (lvl > 0u) {
                wf_pcm_active++;
                if (lvl > wf_pcm_peak)
                    wf_pcm_peak = lvl;
            }
        }

        if (pcm_count != 0u) {
            u32 level;
            used_pcm = 1;
            waveform_pcm_hits += pcm_count;
            if (max_lvl == 0u) {
                level = 0u;
            } else if (zoom == WF_ZOOM_CLOSE) {
                level = max_lvl;
            } else if (zoom == WF_ZOOM_MID) {
                level = (max_lvl * 2u + sum_lvl / pcm_count) / 3u;
                if (level == 0u)
                    level = 1u;
            } else {
                level = (max_lvl + (sum_lvl * 2u) / pcm_count) / 3u;
                if (level == 0u)
                    level = 1u;
            }
            wf_col_lvl[col] = (u8)(level | 0x80u);
        } else {
            /* A coarse whole-track bin can drown out valid PCM at a refill
             * boundary. Use analysis only for columns with no PCM at all. */
            max_lvl = 0u;
            for (k = 0; k < step; k++) {
                s32 f = f0 + k;
                if (f >= 0) {
                    u8 lvl = wf_anlz_frame_level((u32)f, track_len, has_pwav);
                    if (lvl > max_lvl)
                        max_lvl = lvl;
                }
            }
            wf_col_lvl[col] = (u8)max_lvl;
        }
    }

    if (used_pcm)
        wf_adapt_update(play_frame, track_len, has_pwav);

    for (col = 0; col < WF_COLS; col++) {
        u8 raw = wf_col_lvl[col];
        wf_buf[col] = wf_apply_adapt((u32)(raw & 0x1Fu), (raw & 0x80u) != 0u);
    }

    return used_pcm;
}

/* Restore the stock 100-byte overview from 0x0CE65B82 into 0x0C9D1199. */
static void wf_restore_overview(void)
{
    volatile u8 *dst = (volatile u8 *)C850_WAVEFORM_SAMPLES;
    const volatile u8 *src = (const volatile u8 *)C850_WAVEFORM_OVERVIEW;
    int i;

    for (i = 0; i < WF_COLS; i++)
        dst[i] = src[i];
    C850_WAVEFORM_CHANGED = 1u;
}

/* DisplayTask hook (50 Hz, shared display_stage_bpm tail). */
void waveform_hook(u32 *regs, u32 *sp)
{
    u8 source, st;
    s16 track;
    s32 play_frame;
    u32 zoom, now;
    int i, changed, has_pcm;
    const u8 *rendered;
    volatile u8 *dst;

    (void)regs;
    (void)sp;

    if (!waveform_on || c850_ui_flag(C850_UI_FLAG_WAVEFORM_OFF)) {
        if (wf_was_zoomed) {
            wf_restore_overview();
            wf_was_zoomed = 0;
        }
        waveform_zoom = WF_ZOOM_CLOSE;
        wf_was_mode10 = 0;
        if (C850_DISPLAY_MODE != WF_MODE_TITLE)
            c850_ui_utility_draw();
        return;
    }

    if (C850_DISPLAY_MODE != WF_MODE_TITLE) {
        wf_was_mode10 = 0;
        c850_ui_utility_draw();
        return;
    }

    st = C850_PLAYER_STATE;
    if (st == 0u || st == 2u || st == 10u || st == 14u) {
        wf_was_mode10 = 0;
        return;
    }

    wf_track_loop_out();

    source = C850_SOURCE_INDEX;
    track = C850_INFO_TRACK;
    if (source != wf_adapt_source || track != wf_adapt_track)
        wf_adapt_reset(source, track);

    zoom = waveform_zoom;
    if (zoom >= WF_ZOOM_OVERVIEW) {
        if (wf_was_zoomed || wf_force_redraw) {
            wf_restore_overview();
            wf_was_zoomed = 0;
            wf_force_redraw = 0;
        }
        wf_was_mode10 = 1;
        return;
    }

    if (!wf_was_mode10) {
        wf_was_mode10 = 1;
        wf_force_redraw = 1;
    }

    play_frame = (s32)C850_PLAY_FRAME;
    has_pcm = wf_render_zoomed(zoom, play_frame, wf_playhead_column(wf_playhead_pos()));
    now = C850_TICK_MS;
    rendered = wf_buf;
    if (has_pcm) {
        wf_last_had_pcm = 1;
        wf_last_source = source;
        wf_last_track = track;
        wf_last_pcm_frame = play_frame;
        wf_last_pcm_ms = now;
    } else if (wf_last_had_pcm && wf_was_zoomed && !wf_force_redraw &&
               source == wf_last_source && track == wf_last_track &&
               now - wf_last_pcm_ms < WF_DROPOUT_HOLD_MS &&
               play_frame >= wf_last_pcm_frame - WF_DROPOUT_HOLD_FRAMES &&
               play_frame <= wf_last_pcm_frame + WF_DROPOUT_HOLD_FRAMES) {
        /* The stock stager can replace the buffer even while we hold a frame. */
        rendered = wf_last_sent;
    } else {
        wf_last_had_pcm = 0;
    }

    dst = (volatile u8 *)C850_WAVEFORM_SAMPLES;
    changed = (wf_force_redraw != 0u) || (!wf_was_zoomed);
    for (i = 0; i < WF_COLS; i++) {
        u8 v = rendered[i];
        dst[i] = v;
        if (wf_last_sent[i] != v) {
            wf_last_sent[i] = v;
            changed = 1;
        }
    }

    if (changed) {
        C850_WAVEFORM_CHANGED = 1u;
        wf_force_redraw = 0;
        wf_was_zoomed = 1;
        waveform_updates++;
    }
}

/* Rotary encoder hook in FUN_0C0E655C (0x0C0E782E, INFO screen branch).
 * sp + 0x1c holds cStack_54 (0 = CCW, 1 = CW).
 * At INFO cursor 0 on Page 1:
 *   CCW zooms out: 0 (close) -> 1 (mid) -> 2 (medium) -> 3 (overview).
 *   CW zooms back in: 3 -> 2 -> 1 -> 0 (consuming the CW click by setting dir = 0,
 *   so stock takes the CCW cursor-0 no-op path), then advances cursor normally. */
void waveform_rotary_hook(u32 *regs, u32 *sp)
{
    u8 st;
    volatile s8 *dir_p;

    (void)regs;

    if (!waveform_on || c850_ui_flag(C850_UI_FLAG_WAVEFORM_OFF))
        return;

    st = C850_PLAYER_STATE;
    if (st < 2u || st >= 10u)
        return;

    if (C850_INFO_CURSOR != 0u)
        return;

    dir_p = (volatile s8 *)((u8 *)sp + 0x1c);
    if (*dir_p == 0) {
        if (waveform_zoom < WF_ZOOM_OVERVIEW) {
            waveform_zoom++;
            wf_force_redraw = 1;
            waveform_rotary_events++;
        }
    } else {
        if (waveform_zoom > WF_ZOOM_CLOSE) {
            waveform_zoom--;
            wf_force_redraw = 1;
            waveform_rotary_events++;
            *dir_p = 0;
        }
    }
}
