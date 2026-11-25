/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * loop_roll: hold a beat button for a slip loop (see ../patch.py, ../../README.md
 * and docs/cdj850-modding.md section 12b).
 *
 *   BEAT SELECT + LOOP OUT  toggles roll mode (not saved: off after power-on) and
 *                           flashes ROLL ON / ROLL OFF on the display.
 *   BEAT 1/2/4/8 held       (roll mode, playing, not looping) the stock auto beat
 *                           loop plays, quant_loops' snap and drift included; another
 *                           beat button while held resizes it (stock).
 *   all beat buttons up     playback lands where the track would be had it never
 *                           looped (the shadow playhead), then the loop is left.
 *                           Quantize on (and a grid): the roll plays on to the
 *                           next quantize point (UTILITY QUANTIZE: 1, 1/2 or 1/4
 *                           beat) of the shadow and lands there; a beat press
 *                           before that takes the roll back (and resizes it).
 *
 * Shadow playhead. The IRQ7 feeder plays one block of 84 stereo samples
 * (0x150 bytes, 1/7 of a CD frame) per DSPDREQ; while a loop plays it asks
 * isr_loop_out_reached (0x0C0158D4) whether to wrap, once per block, through
 * pool 0x0C07730C (the forward path), which the shared dispatcher
 * c850_loop_out_isr (payload/common/c850_loop_out.c) owns: it asks
 * loop_roll_out_hook first, then quant_loops' block loop, then the stock
 * test. loop_roll_out_hook: from the
 * first call of a roll it counts blocks from the read position, so the count is
 * the un-looped position in blocks, on the sample clock and at whatever pitch
 * the DSP plays. On release it moves the read pointer to that block when the
 * block is still in the PCM ring (sector table checked), clears the loop
 * segments as a stock wrap does, and stops the stock wraps until playertask has
 * left the loop with an injected RELOOP/EXIT press. Past the buffered audio it
 * leaves the loop where it is (stock EXIT) and counts a miss.
 *
 * Contexts: loop_roll_out_hook runs inside irq7_dspdreq_isr (interrupt stack, keep
 * it tiny, no RTOS calls); the key filter and the tick run on playertask. The
 * ISR only reads lr_phase transitions playertask makes and makes the
 * ROLL/JUMP -> JUMPED/MISS ones itself.
 */
#include "c850.h"
#include "c850_ui.h"

#define LR_BEATS             (C850_K1_BEAT1 | C850_K1_BEAT2 | C850_K1_BEAT4 | C850_K1_BEAT8)
#define LR_LATE              10u                    /* blocks (19 ms): a point this far behind counts as reached */
#define LR_STALL             50u                    /* player cycles (~0.6 s) without a feeder block: give up */

enum { LR_IDLE, LR_ROLL, LR_JUMP, LR_JUMPED, LR_MISS, LR_EXIT };

static volatile u8 lr_phase;
static volatile u8 lr_counting;
static volatile u32 lr_pos;          /* un-looped position of the next block, in blocks */
static volatile u32 lr_target;       /* LR_JUMP: land when lr_pos reaches it */
static u32 lr_seen;                  /* loop_roll_isr_calls at the last tick (stall check) */
static u8 lr_mode;                   /* roll mode on */
static u8 lr_reloop_down, lr_wait;

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
        c850_ui_flash(lr_mode ? "ROLL ON" : "ROLL OFF");
        return;
    }
    if (lr_phase == LR_IDLE) {
        int hc_active = c850_ui_flag(C850_UI_FLAG_HOT_CUES);
        if (lr_mode && (p[1] & LR_BEATS) && !hc_active &&
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
}
C850_UI_TICK_HOOK(loop_roll_tick);
