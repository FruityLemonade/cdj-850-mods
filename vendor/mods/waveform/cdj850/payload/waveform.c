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
#include "c850_ui.h"

#define WF_COLS              100
#define WF_CENTER_COLUMN     (WF_COLS / 2)

#define WF_ZOOM_CLOSE        0u   /* 3 frames/col = 300 frames = 4.0 s window */
#define WF_ZOOM_MEDIUM       1u   /* 12 frames/col = 1200 frames = 16.0 s window */
#define WF_ZOOM_OVERVIEW     2u   /* stock whole-track 100-col overview */

#define WF_STEP_CLOSE        3
#define WF_STEP_MEDIUM       12
#define WF_DROPOUT_HOLD_MS   250u
#define WF_DROPOUT_HOLD_FRAMES ((s32)((WF_DROPOUT_HOLD_MS * 75u + 999u) / 1000u))

#define WF_MODE_TITLE        10u  /* INFO Page 1 (title + artist/BPM + waveform) */
#define WF_MODE_UTILITY      12u
#define WF_UTILITY_LEVEL     C850_U8(0x0C1E108C)
#define WF_UTILITY_CURSOR    C850_U8(0x0C1E108D)
#define WF_UTILITY_TIMEOUT   C850_U32(0x0C1E1078)
#define WF_UTILITY_ITEM      7u   /* follows VERSION No. (item 6) */
#define WF_TX_LAST           C850_U32(0x0C9D0AC0)
#define WF_TX_FRAMES         0x0C9D07D0u
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
static u8 wf_last_had_pcm;
static u8 wf_last_source;
static s16 wf_last_track;
static s32 wf_last_pcm_frame;
static u32 wf_last_pcm_ms;
/* A full HISTORY name leaves no room for mod flags. Keep that choice in RAM. */
static u8 wf_position_override;

static int wf_playhead_left(void)
{
    if (wf_position_override != 0u)
        return wf_position_override == 2u;
    return c850_ui_flag(C850_UI_FLAG_WAVE_LEFT);
}

void waveform_utility_scroll(s8 direction, u8 steps)
{
    s32 item = (s32)WF_UTILITY_CURSOR;
    s32 next = item + (direction == 1 ? (s32)steps : -(s32)steps);

    if (!waveform_on || (item < (s32)WF_UTILITY_ITEM && next < (s32)WF_UTILITY_ITEM)) {
        C850_FN(0x0C0E4DF6, void, (s8, u8))(direction, steps);
        return;
    }
    if (next < 0)
        next = 0;
    if (next > (s32)WF_UTILITY_ITEM)
        next = (s32)WF_UTILITY_ITEM;
    WF_UTILITY_TIMEOUT = 15000u;
    if (next != item) {
        WF_UTILITY_CURSOR = (u8)next;
        C850_FN(0x0C06F71C, void, (int))(1);
    }
}

void waveform_utility_select(u32 level, int item)
{
    int left;

    if (!waveform_on || level != 3u || item != (int)WF_UTILITY_ITEM) {
        C850_FN(0x0C0E4C14, void, (u32, int))(level, item);
        return;
    }
    left = !wf_playhead_left();
    if (c850_ui_set_flag(C850_UI_FLAG_WAVE_LEFT, left))
        wf_position_override = 0;
    else
        wf_position_override = left ? 2u : 1u;
    wf_force_redraw = 1;
    WF_UTILITY_TIMEOUT = 15000u;
    C850_FN(0x0C06F71C, void, (int))(1);
}

static void wf_utility_row(u32 row, u8 tag, u8 arrows, const char *text)
{
    volatile u8 *record = (volatile u8 *)(0x0C9D0B18u + row * 0x105u);
    u8 len = 0;

    record[0] = tag;
    record[1] = 0;
    record[2] = arrows;
    record[3] = 0;
    while (text[len] != '\0') {
        record[5u + len] = (u8)text[len];
        len++;
    }
    record[4] = len;
}

/* Add the playhead to the stock bitmap before SCIF1 calculates its checksum. */
int waveform_center_marker(void)
{
    u32 before = WF_TX_LAST;
    int result = C850_FN(0x0C0664A8, int, (void))();
    u32 last = WF_TX_LAST;

    if (waveform_on && wf_was_zoomed && waveform_zoom < WF_ZOOM_OVERVIEW &&
        C850_DISPLAY_MODE == WF_MODE_TITLE && !wf_playhead_left() &&
        last > before && last < 0x2Fu) {
        volatile u8 *frame = (volatile u8 *)(WF_TX_FRAMES + last * 16u);
        if (frame[0] == 0x60u)
            frame[2u + WF_CENTER_COLUMN / 8u] |= (u8)(0x80u >> (WF_CENTER_COLUMN % 8u));
    }
    return result;
}

/* Per-ring-slot cached amplitude level (1..15) keyed by (valid | track<<20 | frame). */
static u8 wf_slot_level[C850_PCM_RING_SLOTS];
static u32 wf_slot_key[C850_PCM_RING_SLOTS];

/* Pioneer's 2 dB/step logarithmic amplitude thresholds from FUN_0C0D5590. */
static const u16 wf_log_thresh[13] = {
    0x0517u, 0x066Au, 0x0813u, 0x0A2Au, 0x0CCCu,
    0x101Du, 0x1449u, 0x198Au, 0x2026u, 0x287Au,
    0x32F5u, 0x4026u, 0x50C3u
};

/* Spread levels around 8 before the panel groups them into five dot heights.
 * Reserve the top row at medium zoom for sustained levels of 13 and above.
 * Keep silence at 1 because a zero byte terminates the strip. */
static const u8 wf_zoom_contrast[][15] = {
    [WF_ZOOM_CLOSE]  = {1, 1, 1, 3, 4, 5, 7, 8,  9, 11, 12, 13, 15, 15, 15},
    [WF_ZOOM_MEDIUM] = {1, 1, 1, 1, 1, 3, 5, 8, 10, 11, 12, 12, 13, 14, 15},
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

/* Render the next 100 columns and report whether they include any live PCM. */
static int wf_render_zoomed(u32 zoom, s32 play_frame, s32 playhead_col)
{
    const u8 *contrast = wf_zoom_contrast[zoom];
    s32 step = (zoom == WF_ZOOM_CLOSE) ? WF_STEP_CLOSE : WF_STEP_MEDIUM;
    u32 track_len = C850_TRACK_LENGTH;
    u32 r_idx = C850_RING_READ_INDEX;
    u32 w_idx = C850_RING_WRITE_INDEX;
    int ring_ok = 0;
    s32 r_frame = 0;
    u32 cur_track = 0;
    s32 ahead = 0;
    s32 behind = 0;
    int has_pwav = -1;
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
        u32 level = 0;
        u32 pcm_count = 0;
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
            if (zoom == WF_ZOOM_MEDIUM)
                level += lvl;
            else if (lvl > level)
                level = lvl;
        }

        if (pcm_count != 0u) {
            used_pcm = 1;
            waveform_pcm_hits += pcm_count;
            /* Preserve short transients at close zoom. At medium zoom a
             * single loud sector should not fill the entire 160 ms column. */
            if (zoom == WF_ZOOM_MEDIUM)
                level = (level + pcm_count / 2u) / pcm_count;
        } else {
            /* A coarse whole-track bin can drown out valid PCM at a refill
             * boundary. Use analysis only for columns with no PCM at all. */
            if (has_pwav < 0)
                has_pwav = wf_pwav_available();
            level = 1u;
            for (k = 0; k < step; k++) {
                s32 f = f0 + k;
                if (f >= 0) {
                    u8 lvl = wf_anlz_frame_level((u32)f, track_len, has_pwav);
                    if (lvl > level)
                        level = lvl;
                }
            }
        }
        wf_buf[col] = contrast[level - 1u];
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

    if (!waveform_on) {
        if (wf_was_zoomed) {
            wf_restore_overview();
            wf_was_zoomed = 0;
        }
        return;
    }

    if (C850_DISPLAY_MODE != WF_MODE_TITLE) {
        wf_was_mode10 = 0;
        if (C850_DISPLAY_MODE == WF_MODE_UTILITY && WF_UTILITY_LEVEL == 2u &&
            WF_UTILITY_CURSOR == WF_UTILITY_ITEM) {
            wf_utility_row(1, 0x06, 0x0B, "WAVE PLAYHEAD");
            wf_utility_row(2, 0x43, 0, wf_playhead_left() ? "LEFT" : "CENTER");
        }
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

    play_frame = (s32)C850_PLAY_FRAME;
    has_pcm = wf_render_zoomed(zoom, play_frame, wf_playhead_left() ? 0 : WF_CENTER_COLUMN);
    source = C850_SOURCE_INDEX;
    track = C850_INFO_TRACK;
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
