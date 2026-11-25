/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * loop_roll: slip mode on CDJ-850 MAIN 1.13 (see ../patch.py, ../../README.md,
 * docs/cdj850-modding.md section 12b, and docs/cdj850-slip-design.md).
 *
 *   BEAT SELECT + LOOP OUT  toggles slip mode (not saved: off after power-on)
 *                           and flashes SLIP ON / SLIP OFF on the display.
 *   BEAT 1/2/4/8 held       (slip mode, playing, not looping) loop roll: the
 *                           stock auto beat loop plays, quant_loops' snap and
 *                           drift included; another beat button while held
 *                           resizes it (stock). On release, lands on the shadow.
 *   REV / VINYL scratch /   (slip mode, playing) a shadow playhead keeps
 *   pause                   advancing at the tempo-fader speed over the 1 ms
 *                           tick; when normal forward playback resumes, jumps
 *                           in-ring (IRQ7 masked) to the shadow (or waits for
 *                           the next quantize point if quantize is on). Past
 *                           the buffered ring, flashes SLIP MISS and stays put.
 *
 * Shadow playhead. For loop roll, the IRQ7 feeder hook loop_roll_out_hook counts
 * 84-sample blocks (0x150 bytes) on the sample clock and jumps inside the ISR.
 * For reverse, VINYL scratch/backspin, and pause, playertask integrates
 * c850_effective_pitch() over the 1 ms tick C850_TICK_MS in exact integer
 * units (blocks and remainder mod 400000) and lands from playertask with
 * interrupts masked (c850_irq_save / c850_irq_restore) once the audio path
 * returns to normal forward playback on a 336-byte block boundary.
 */
#include "c850.h"
#include "c850_ui.h"

#define LR_BEATS             (C850_K1_BEAT1 | C850_K1_BEAT2 | C850_K1_BEAT4 | C850_K1_BEAT8)
#define LR_LATE              10u                    /* blocks (19 ms): a point this far behind counts as reached */
#define LR_STALL             50u                    /* player cycles (~0.6 s) without a feeder block: give up */
#define SLIP_CLOCK_DEN       400000u                /* exact 525/10000000 = 21/400000 blocks/ms */
#define SLIP_DT_MAX          1000u                  /* cap dt (ms) to keep dt * 21 * 20000 < 2^32 */
#define SLIP_LAND_TIMEOUT    250u                   /* player cycles (~3 s) waiting for normal play */

enum { LR_IDLE, LR_ROLL, LR_JUMP, LR_JUMPED, LR_MISS, LR_EXIT };
enum { SLIP_IDLE, SLIP_ACTIVE, SLIP_LAND };
enum { SLIP_NONE, SLIP_REV, SLIP_SCRATCH, SLIP_PAUSE };

static volatile u8 lr_phase;
static volatile u8 lr_counting;
static volatile u32 lr_pos;          /* un-looped position of the next block, in blocks */
static volatile u32 lr_target;       /* LR_JUMP: land when lr_pos reaches it */
static u32 lr_seen;                  /* loop_roll_isr_calls at the last tick (stall check) */
static u8 lr_mode;                   /* slip mode on */
static u8 lr_reloop_down, lr_wait;

static u8 slip_phase;
static u8 slip_kind;
static u8 slip_was_playing;
static u16 slip_land_wait;
static u32 slip_pos;                 /* shadow playhead, whole blocks */
static u32 slip_rem;                 /* fractional remainder mod SLIP_CLOCK_DEN */
static u32 slip_last_tick;
static u32 slip_target;
static u32 slip_track_len;

/* evidence for tests */
volatile u32 loop_roll_rolls;        /* rolls started (beat press in roll mode) */
volatile u32 loop_roll_isr_calls;    /* blocks counted */
volatile u32 loop_roll_jumps;        /* landed on the shadow playhead */
volatile u32 loop_roll_misses;       /* shadow past the buffered audio: stock EXIT */
volatile u32 loop_roll_aborts;       /* the loop ended some other way */
volatile u32 loop_roll_exits;        /* RELOOP presses given */
volatile u32 loop_roll_toggles;
volatile u32 loop_roll_start_pos;    /* first counted block of the last roll */
volatile u32 loop_roll_last_target;  /* block the last jump went to */
volatile u32 loop_roll_last_from;    /* read position (blocks) the last jump left */
volatile u32 loop_roll_last_sector;
volatile u32 loop_roll_start_tick;   /* 1 ms tick at the first counted block */
volatile u32 loop_roll_jump_tick;    /* and at the landing */
volatile u32 loop_roll_release_pos;  /* shadow (blocks) at the last release */
volatile u32 loop_roll_quantized;    /* releases that waited for a quantize point */
volatile u32 loop_roll_cancels;      /* beat presses that took a waiting release back */

/* evidence for slip gestures (reverse, scratch, pause) */
volatile u32 loop_roll_slip_starts;
volatile u32 loop_roll_slip_jumps;
volatile u32 loop_roll_slip_misses;
volatile u32 loop_roll_slip_aborts;
volatile u32 loop_roll_slip_quantized;
volatile u32 loop_roll_slip_kind;
volatile u32 loop_roll_slip_start_pos;
volatile u32 loop_roll_slip_last_target;
volatile u32 loop_roll_slip_last_from;
volatile u32 loop_roll_slip_start_tick;
volatile u32 loop_roll_slip_jump_tick;
volatile u32 loop_roll_slip_release_pos;

/* quant_loops' grid (strong when it is built): the first quantize point at or
 * after pos - late, in blocks. Without quant_loops: no grid, land at once. */
int quant_loops_roll_target(u32 pos_block, u32 late, u32 *target) __attribute__((weak, noinline));
int quant_loops_roll_target(u32 pos_block, u32 late, u32 *target)
{
    (void)pos_block;
    (void)late;
    (void)target;
    return 0;
}

/* Move the read pointer to block `pos` (payload/common/c850_loop_out.c); 0 when
 * that audio is not in the ring. */
static int lr_jump(u32 pos)
{
    u32 from = c850_read_pos();

    if (!c850_ring_jump(pos))
        return 0;
    loop_roll_last_from = from;
    loop_roll_last_target = pos;
    loop_roll_last_sector = (u32)C850_RING_READ_INDEX;
    return 1;
}

/* First hook of c850_loop_out_isr (pool 0x0C07730C, the forward loop-out test
 * of irq7_dspdreq_isr), once per block while a loop plays; the read pointer
 * already names the next block. -1 = not mine (the block loop, then the stock
 * test decide), 0 = no wrap. */
int loop_roll_out_hook(int sector, u32 out, int z)
{
    u32 ph = lr_phase;

    (void)sector;
    (void)out;
    (void)z;

    if (ph == LR_ROLL || ph == LR_JUMP) {
        if (!lr_counting) {
            lr_pos = c850_read_pos();
            loop_roll_start_pos = lr_pos;
            loop_roll_start_tick = C850_TICK_MS;
            lr_counting = 1;
        } else {
            lr_pos++;
        }
        loop_roll_isr_calls++;
        if (ph == LR_JUMP && lr_pos >= lr_target) {
            if (lr_jump(lr_pos)) {
                lr_phase = LR_JUMPED;
                loop_roll_jump_tick = C850_TICK_MS;
                loop_roll_jumps++;
                return 0;
            }
            lr_phase = LR_MISS;
            loop_roll_misses++;
        }
    } else if (ph == LR_JUMPED) {
        return 0;                    /* no wrap until playertask has left the loop */
    }
    return -1;
}

static int lr_looping(void)
{
    return C850_PLAYER_STATE == C850_STATE_LOOPING && C850_LOOP_ACTIVE == 1;
}

/* Shared key filter (c850_ui_keys, pool 0x0C0ED038), playertask, on key changes. */
static void loop_roll_key_filter(const u16 *k, const u16 *p, u16 *own)
{
    if (c850_ui_shift(k, p, own, C850_K0_LOOP_OUT)) {
        lr_mode = !lr_mode;
        loop_roll_toggles++;
        if (!lr_mode && slip_phase != SLIP_IDLE) {
            slip_phase = SLIP_IDLE;
            loop_roll_slip_aborts++;
        }
        c850_ui_flash(lr_mode ? "SLIP ON" : "SLIP OFF");
        return;
    }
    if (lr_phase == LR_IDLE) {
        int hc_active = c850_ui_flag(C850_UI_FLAG_HOT_CUES);
        if (lr_mode && slip_phase == SLIP_IDLE && (p[1] & LR_BEATS) && !hc_active &&
            C850_IS_USB_MODE == 1 && C850_PLAYER_STATE == C850_STATE_PLAYING) {
            lr_counting = 0;
            lr_phase = LR_ROLL;      /* the stock handler makes the loop from this press */
            loop_roll_rolls++;
        }
    } else if (lr_phase == LR_ROLL && !(k[1] & LR_BEATS)) {
        if (lr_counting && lr_looping()) {
            u32 now = lr_pos, t = now;

            loop_roll_release_pos = now;
            if (quant_loops_roll_target(now, LR_LATE, &t) && t > now)
                loop_roll_quantized++;
            else
                t = now;             /* no grid, quantize off, or a point just passed */
            lr_target = t;           /* written before the phase the ISR reads */
            lr_seen = loop_roll_isr_calls;
            lr_wait = 0;
            lr_phase = LR_JUMP;      /* the ISR lands at block t */
        } else {
            lr_phase = LR_IDLE;      /* the press made no loop */
        }
    } else if (lr_phase == LR_JUMP && (p[1] & LR_BEATS)) {
        lr_phase = LR_ROLL;          /* pressed again before the point: roll on (stock resizes) */
        loop_roll_cancels++;
    }
}
C850_UI_KEY_FILTER(loop_roll_key_filter);

/* Advance the tick-integrated shadow playhead at the tempo-fader speed.
 * 44100 Hz / 84 samples = 525 blocks/s; 525 * factor / 1e7 = 21 * factor / 400000. */
static void slip_advance(void)
{
    u32 now = C850_TICK_MS;
    u32 dt = now - slip_last_tick;
    s32 p = c850_effective_pitch();
    u32 factor, add;

    slip_last_tick = now;
    if (dt == 0u)
        return;
    if (dt > SLIP_DT_MAX)
        dt = SLIP_DT_MAX;
    if (p < -10000)
        p = -10000;
    else if (p > 10000)
        p = 10000;
    factor = (u32)(10000 + p);
    add = dt * 21u * factor;
    slip_rem += add;
    slip_pos += slip_rem / SLIP_CLOCK_DEN;
    slip_rem %= SLIP_CLOCK_DEN;
}

static int slip_ptr_aligned(void)
{
    u32 s = (u32)C850_RING_READ_INDEX;
    u32 base = C850_PCM_RING_BASE + s * C850_PCM_SECTOR_BYTES;
    u32 ptr = C850_PCM_READ_PTR;
    u32 off;

    if (ptr < base)
        return 0;
    off = ptr - base;
    if (off >= C850_PCM_SECTOR_BYTES)
        return 0;
    while (off >= C850_PCM_BLOCK_BYTES)
        off -= C850_PCM_BLOCK_BYTES;
    return off == 0u;
}

static int slip_ready(void)
{
    if (C850_IS_USB_MODE != 1u || C850_PLAYER_STATE != C850_STATE_PLAYING)
        return 0;
    if (C850_AUDIO_STATE != 3u || C850_AUDIO_REVERSE != 0u || C850_SPEED_SUBSTATE != 0u)
        return 0;
    if (C850_JOG_MODE_VINYL == 1u && C850_JOG_TOUCHED == 1u)
        return 0;
    return slip_ptr_aligned();
}

/* Jump to `pos` from playertask with interrupts masked so the IRQ7 feeder
 * never observes a half-updated read pointer / sector index.
 * Returns 1 = jumped, 0 = missed ring, -1 = not ready (retry next tick). */
static int slip_try_jump(u32 pos)
{
    u32 sr = c850_irq_save();
    u32 from;
    int ok;

    if (C850_AUDIO_REVERSE != 0u || !slip_ptr_aligned()) {
        c850_irq_restore(sr);
        return -1;
    }
    from = c850_read_pos();
    ok = c850_ring_jump(pos);
    c850_irq_restore(sr);

    if (ok) {
        loop_roll_slip_last_from = from;
        loop_roll_slip_last_target = pos;
        loop_roll_slip_jump_tick = C850_TICK_MS;
        loop_roll_slip_jumps++;
        return 1;
    }
    loop_roll_slip_misses++;
    c850_ui_flash("SLIP MISS");
    return 0;
}

static int slip_gesture_ended(u8 kind)
{
    switch (kind) {
    case SLIP_REV:
        return C850_AUDIO_REVERSE == 0u;
    case SLIP_SCRATCH:
        return C850_JOG_TOUCHED == 0u;
    case SLIP_PAUSE:
        return C850_PLAYER_STATE == C850_STATE_PLAYING;
    default:
        return 1;
    }
}

static int slip_gesture_resumed(u8 kind)
{
    switch (kind) {
    case SLIP_REV:
        return C850_AUDIO_REVERSE != 0u && C850_PLAYER_STATE == C850_STATE_PLAYING;
    case SLIP_SCRATCH:
        return C850_JOG_MODE_VINYL == 1u && C850_JOG_TOUCHED == 1u &&
               C850_PLAYER_STATE == C850_STATE_PLAYING;
    case SLIP_PAUSE:
        return C850_PLAYER_STATE == C850_STATE_PAUSED_CDJ;
    default:
        return 0;
    }
}

static void slip_try_land(void)
{
    if (slip_ready()) {
        slip_land_wait = 0;
        if (slip_pos >= slip_target) {
            int r = slip_try_jump(slip_pos);
            if (r >= 0) {
                slip_phase = SLIP_IDLE;
                slip_was_playing = 1;
            }
        }
    } else if (++slip_land_wait > SLIP_LAND_TIMEOUT) {
        slip_phase = SLIP_IDLE;
        loop_roll_slip_aborts++;
    }
}

static void slip_tick(void)
{
    int normal = (C850_IS_USB_MODE == 1u && C850_PLAYER_STATE == C850_STATE_PLAYING &&
                  C850_AUDIO_STATE == 3u && C850_AUDIO_REVERSE == 0u &&
                  !(C850_JOG_MODE_VINYL == 1u && C850_JOG_TOUCHED == 1u));

    if (slip_phase == SLIP_IDLE) {
        if (lr_mode && slip_was_playing && C850_IS_USB_MODE == 1u) {
            u8 kind = SLIP_NONE;

            if (C850_AUDIO_REVERSE != 0u && C850_PLAYER_STATE == C850_STATE_PLAYING)
                kind = SLIP_REV;
            else if (C850_JOG_MODE_VINYL == 1u && C850_JOG_TOUCHED == 1u &&
                     C850_PLAYER_STATE == C850_STATE_PLAYING)
                kind = SLIP_SCRATCH;
            else if (C850_PLAYER_STATE == C850_STATE_PAUSED_CDJ)
                kind = SLIP_PAUSE;

            if (kind != SLIP_NONE) {
                slip_pos = c850_read_pos();
                slip_rem = 0u;
                slip_last_tick = C850_TICK_MS;
                slip_track_len = C850_TRACK_LENGTH;
                slip_kind = kind;
                slip_phase = SLIP_ACTIVE;
                loop_roll_slip_kind = kind;
                loop_roll_slip_start_pos = slip_pos;
                loop_roll_slip_start_tick = slip_last_tick;
                loop_roll_slip_starts++;
                slip_was_playing = 0;
                return;
            }
        }
        slip_was_playing = (u8)normal;
        return;
    }

    if (!lr_mode || C850_IS_USB_MODE != 1u || C850_TRACK_LENGTH != slip_track_len ||
        (C850_PLAYER_STATE != C850_STATE_PLAYING && C850_PLAYER_STATE != C850_STATE_PAUSED_CDJ)) {
        slip_phase = SLIP_IDLE;
        slip_was_playing = 0;
        loop_roll_slip_aborts++;
        return;
    }

    if (slip_phase == SLIP_ACTIVE) {
        slip_advance();
        if (slip_gesture_ended(slip_kind)) {
            u32 now = slip_pos, t = now;

            loop_roll_slip_release_pos = now;
            if (quant_loops_roll_target(now, LR_LATE, &t) && t > now)
                loop_roll_slip_quantized++;
            else
                t = now;
            slip_target = t;
            slip_land_wait = 0;
            slip_phase = SLIP_LAND;
            slip_try_land();
        }
    } else if (slip_phase == SLIP_LAND) {
        if (slip_gesture_resumed(slip_kind)) {
            slip_phase = SLIP_ACTIVE;
            slip_advance();
            return;
        }
        slip_advance();
        slip_try_land();
    }
}

/* Shared state tick (c850_ui_tick, pool 0x0C0EC838): every player cycle,
 * before the key handlers. */
static void loop_roll_tick(void)
{
    switch (lr_phase) {
    case LR_ROLL:
        if (lr_counting && !lr_looping()) {
            lr_phase = LR_IDLE;      /* EXIT, CUE, a jump... ended it */
            loop_roll_aborts++;
        }
        break;
    case LR_JUMP: {
        u32 c = loop_roll_isr_calls;

        if (c != lr_seen) {          /* the feeder is counting: waiting for the point is fine */
            lr_seen = c;
            lr_wait = 0;
        }
        if (!lr_looping() || ++lr_wait > LR_STALL) {
            lr_phase = LR_IDLE;
            lr_wait = 0;
            loop_roll_aborts++;
        }
        break;
    }
    case LR_JUMPED:
    case LR_MISS:
        lr_wait = 0;
        if (lr_looping()) {          /* RELOOP/EXIT: one press, released next cycle */
            C850_K0 |= C850_K0_RELOOP;
            lr_reloop_down = 1;
            loop_roll_exits++;
        }
        lr_phase = LR_EXIT;
        break;
    case LR_EXIT:
        if (lr_reloop_down) {
            lr_reloop_down = 0;
            if (!(c850_ui_kt[0] & C850_K0_RELOOP))
                C850_K0 &= ~C850_K0_RELOOP;
        }
        lr_phase = LR_IDLE;
        break;
    default:
        lr_wait = 0;
        break;
    }

    if (lr_phase == LR_IDLE) {
        slip_tick();
    } else {
        slip_was_playing = 0;
        if (slip_phase != SLIP_IDLE) {
            slip_phase = SLIP_IDLE;
            loop_roll_slip_aborts++;
        }
    }
}
C850_UI_TICK_HOOK(loop_roll_tick);
