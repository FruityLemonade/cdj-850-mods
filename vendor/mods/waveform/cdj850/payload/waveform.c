/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * waveform: zoomed scrolling half-waveform in the 100x5 INFO Page 1 strip
 * (CDJ-850 MAIN 1.13).
 *
 * How it works:
 *   - panel_tx_waveform (0x0C0662AE) packs the 100-byte buffer at 0x0C9D1199
 *     into SCIF1 frames 0x70..0x77 and bumps sequence 0x0C9D1184 whenever
 *     0x0C9D1188 == 1. Note that a 0 byte terminates the strip, so every
 *     active column must be in 1..15 (mapped to 1..5 VFD dot rows by IC1007).
 *   - waveform_hook runs at 50 Hz in DisplayTask on the shared display_stage_bpm
 *     tail (after FUN_0C066402 and info_order, before panel_tx_waveform). It
 *     never touches dot-matrix text rows or DB records, so it does not
 *     interfere with info_order or phase_meter.
 *   - waveform_rotary_hook runs in FUN_0C0E655C (0x0C0E782E) when the rotary
 *     encoder is turned in INFO mode (nav_view_selector == 0, not in BPM Lock
 *     edit mode). At cursor 0 on Page 1:
 *       CCW zooms out: 0 (close, 4 s) -> 1 (medium, 16 s) -> 2 (overview)
 *       CW zooms back in: 2 -> 1 -> 0, and once at 0 advances cursor normally
 *       to row 2 / INFO Page 2+.
 *   - Audio data comes directly from the 2400-sector (32 s) PCM ring buffer at
 *     0x0C1E8E5C (cached per ring slot so each newly decoded sector is scanned
 *     once), seamlessly falling back to the 400-column ANLZ PWAV buffer at
 *     0x0CE65862 or the 100-column overview at 0x0CE65B82 for frames outside
 *     the currently buffered PCM window.
 */
#include "c850.h"

#define WF_COLS              100
#define WF_PLAYHEAD_COL      50

#define WF_ZOOM_CLOSE        0u   /* 3 frames/col = 300 frames = 4.0 s window */
#define WF_ZOOM_MEDIUM       1u   /* 12 frames/col = 1200 frames = 16.0 s window */
#define WF_ZOOM_OVERVIEW     2u   /* stock whole-track 100-col overview */

#define WF_STEP_CLOSE        3
#define WF_STEP_MEDIUM       12

#define WF_MODE_TITLE        10u  /* INFO Page 1 (title + artist/BPM + waveform) */
#define WF_SLOT_VALID_BIT    0x80000000u

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
static u8 wf_last_sent[WF_COLS];

/* Per-ring-slot cached amplitude level (1..15) keyed by (valid | track<<20 | frame). */
static u8 wf_slot_level[C850_PCM_RING_SLOTS];
static u32 wf_slot_key[C850_PCM_RING_SLOTS];

/* Pioneer's 2 dB/step logarithmic amplitude thresholds from FUN_0C0D5590. */
static const u16 wf_log_thresh[13] = {
    0x0517u, 0x066Au, 0x0813u, 0x0A2Au, 0x0CCCu,
    0x101Du, 0x1449u, 0x198Au, 0x2026u, 0x287Au,
    0x32F5u, 0x4026u, 0x50C3u
};

/* Map mean absolute 16-bit PCM amplitude to 1..15 (never 0). */
u8 waveform_amp_to_height(u32 avg)
{
    int i;

    if (avg == 0u)
        return 1u;
    for (i = 0; i < 13; i++) {
        if (avg <= (u32)wf_log_thresh[i])
            return (u8)(i + 2);
    }
    return 15u;
}

/* Scan one 2352-byte PCM ring sector (588 stereo 16-bit BE frames) into 1..15. */
static u8 wf_scan_pcm_slot(u32 slot)
{
    const volatile s16 *pcm = (const volatile s16 *)(C850_PCM_RING_BASE +
                                                     slot * C850_PCM_SECTOR_BYTES);
    u32 sum = 0;
    int i;

    /* 64 stereo pairs at stride 9 (18 s16s): indices 0..1135 (< 1176). */
    for (i = 0; i < 64; i++) {
        s32 l = (s32)pcm[0];
        s32 r = (s32)pcm[1];
        if (l < 0)
            l = -l;
        if (r < 0)
            r = -r;
        sum += (u32)l + (u32)r;
        pcm += 18;
    }
    return waveform_amp_to_height(sum >> 7);
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

/* Fallback height (1..15) for a frame from 400-col PWAV or 100-col overview. */
static u8 wf_anlz_frame_level(u32 frame, u32 track_len, int has_pwav)
{
    u32 idx;

    if (track_len == 0u || frame >= track_len)
        return 1u;

    if (has_pwav) {
        if (frame <= 10000000u)
            idx = (frame * 400u) / track_len;
        else
            idx = ((frame >> 4) * 400u) / (track_len >> 4);
        if (idx >= 400u)
            idx = 399u;
        {
            u32 h = C850_U8(C850_WAVEFORM_PWAV + idx * 2u) & 0x1Fu;
            u32 scaled = (h * 15u + 15u) / 31u;
            waveform_pwav_hits++;
            return scaled < 1u ? 1u : (u8)scaled;
        }
    }

    if (frame <= 40000000u)
        idx = (frame * 100u) / track_len;
    else
        idx = ((frame >> 4) * 100u) / (track_len >> 4);
    if (idx >= 100u)
        idx = 99u;
    {
        u8 v = C850_U8(C850_WAVEFORM_OVERVIEW + idx);
        if (v < 1u)
            return 1u;
        if (v > 15u)
            return 15u;
        return v;
    }
}

/* Render 100 columns for zoom 0 (close) or 1 (medium) into wf_buf[100]. */
static void wf_render_zoomed(u32 zoom)
{
    s32 step = (zoom == WF_ZOOM_CLOSE) ? WF_STEP_CLOSE : WF_STEP_MEDIUM;
    s32 play_frame = (s32)C850_PLAY_FRAME;
    u32 track_len = C850_TRACK_LENGTH;
    u32 r_idx = C850_RING_READ_INDEX;
    u32 w_idx = C850_RING_WRITE_INDEX;
    int ring_ok = 0;
    s32 r_frame = 0;
    u32 cur_track = 0;
    s32 ahead = 0;
    s32 behind = 0;
    int has_pwav = wf_pwav_available();
    int col;

    if (r_idx < C850_PCM_RING_SLOTS && w_idx < C850_PCM_RING_SLOTS) {
        r_frame = (s32)c850_ring_sector_to_frame((int)r_idx);
        cur_track = c850_sector_to_track((int)r_idx);
        ahead = (s32)((w_idx + C850_PCM_RING_SLOTS - r_idx) % C850_PCM_RING_SLOTS);
        behind = (s32)C850_PCM_RING_SLOTS - ahead - 1;
        ring_ok = 1;
    }

    for (col = 0; col < WF_COLS; col++) {
        s32 f0 = play_frame + (col - WF_PLAYHEAD_COL) * step;
        u8 peak = 1u;
        int k;

        for (k = 0; k < step; k++) {
            s32 f = f0 + k;
            u8 lvl = 1u;

            if (f < 0 || (track_len > 0u && (u32)f >= track_len)) {
                continue;
            }

            if (ring_ok) {
                s32 delta = f - r_frame;
                if ((delta >= 0 && delta < ahead) || (delta < 0 && -delta <= behind)) {
                    s32 s = (s32)r_idx + delta;
                    while (s < 0)
                        s += (s32)C850_PCM_RING_SLOTS;
                    while (s >= (s32)C850_PCM_RING_SLOTS)
                        s -= (s32)C850_PCM_RING_SLOTS;
                    if (c850_ring_sector_to_frame(s) == (u32)f &&
                        c850_sector_to_track(s) == cur_track) {
                        lvl = wf_pcm_level_cached((u32)s, cur_track, (u32)f);
                        waveform_pcm_hits++;
                    } else {
                        lvl = wf_anlz_frame_level((u32)f, track_len, has_pwav);
                    }
                } else {
                    lvl = wf_anlz_frame_level((u32)f, track_len, has_pwav);
                }
            } else {
                lvl = wf_anlz_frame_level((u32)f, track_len, has_pwav);
            }

            if (lvl > peak)
                peak = lvl;
        }
        wf_buf[col] = peak;
    }
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
    u8 st;
    u32 zoom;
    int i, changed;
    volatile u8 *dst;

    (void)regs;
    (void)sp;

    if (!waveform_on) {
        if (wf_was_zoomed) {
            wf_restore_overview();
            wf_was_zoomed = 0;
        }
        return;
    }

    if (C850_DISPLAY_MODE != WF_MODE_TITLE) {
        wf_was_mode10 = 0;
        return;
    }

    st = C850_PLAYER_STATE;
    if (st == 0u || st == 2u || st == 10u || st == 14u) {
        wf_was_mode10 = 0;
        return;
    }

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

    wf_render_zoomed(zoom);

    dst = (volatile u8 *)C850_WAVEFORM_SAMPLES;
    changed = (wf_force_redraw != 0u) || (!wf_was_zoomed);
    for (i = 0; i < WF_COLS; i++) {
        u8 v = wf_buf[i];
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
 *   CCW zooms out: 0 (close) -> 1 (medium) -> 2 (overview).
 *   CW zooms back in: 2 -> 1 -> 0 (consuming the CW click by setting dir = 0,
 *   so stock takes the CCW cursor-0 no-op path), then advances cursor normally. */
void waveform_rotary_hook(u32 *regs, u32 *sp)
{
    u8 st;
    volatile s8 *dir_p;

    (void)regs;

    if (!waveform_on)
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
