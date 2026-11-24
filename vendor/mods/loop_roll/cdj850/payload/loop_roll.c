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
 *
 * Shadow playhead. The IRQ7 feeder plays one block of 84 stereo samples
 * (0x150 bytes, 1/7 of a CD frame) per DSPDREQ; while a loop plays it asks
 * isr_loop_out_reached (0x0C0158D4) whether to wrap, once per block, through
 * pool 0x0C07730C (the forward path). loop_roll_isr wraps that call: from the
 * first call of a roll it counts blocks from the read position, so the count is
 * the un-looped position in blocks, on the sample clock and at whatever pitch
 * the DSP plays. On release it moves the read pointer to that block when the
 * block is still in the PCM ring (sector table checked), clears the loop
 * segments as a stock wrap does, and stops the stock wraps until playertask has
 * left the loop with an injected RELOOP/EXIT press. Past the buffered audio it
 * leaves the loop where it is (stock EXIT) and counts a miss.
 *
 * Contexts: loop_roll_isr runs inside irq7_dspdreq_isr (interrupt stack, keep
 * it tiny, no RTOS calls); the key filter and the tick run on playertask. The
 * ISR only reads lr_phase transitions playertask makes and makes the
 * ROLL/JUMP -> JUMPED/MISS ones itself.
 */
#include "c850.h"
#include "c850_ui.h"

#define LR_STOCK_OUT_REACHED C850_FN(0x0C0158D4, int, (int sector, u32 out, int z))
#define LR_CLEAR_SEGMENTS    C850_FN(0x0C015B18, void, (void))
#define LR_READ_PTR          C850_U32(0x0C7F6470)   /* next block the feeder plays */
#define LR_PLAY_SECTOR       C850_S16(0x0C7F645C)   /* (ptr - base) / 0x930 */
#define LR_WRITE_SECTOR      C850_S16(0x0C7F645E)
#define LR_SEG_SOURCE        C850_U32(0x0C7F677C)   /* block source while loop segments exist */
#define LR_BLOCK_BYTES       0x150u
#define LR_BLOCKS            7u                     /* per sector */
#define LR_BEHIND_MAX        1000u                  /* played sectors the ring keeps (<= 1001) */
#define LR_BEATS             (C850_K1_BEAT1 | C850_K1_BEAT2 | C850_K1_BEAT4 | C850_K1_BEAT8)

enum { LR_IDLE, LR_ROLL, LR_JUMP, LR_JUMPED, LR_MISS, LR_EXIT };

static volatile u8 lr_phase;
static volatile u8 lr_counting;
static volatile u32 lr_pos;          /* un-looped position of the next block, in blocks */
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

static u32 lr_mod(s32 v)
{
    while (v < 0)
        v += (s32)C850_PCM_RING_SLOTS;
    while (v >= (s32)C850_PCM_RING_SLOTS)
        v -= (s32)C850_PCM_RING_SLOTS;
    return (u32)v;
}

/* Position (blocks) of the block the read pointer names. */
static u32 lr_read_pos(void)
{
    u32 s = (u32)(u16)LR_PLAY_SECTOR;
    u32 off = LR_READ_PTR - (C850_PCM_RING_BASE + s * C850_PCM_SECTOR_BYTES);

    return c850_ring_sector_to_frame((int)s) * LR_BLOCKS + off / LR_BLOCK_BYTES;
}

/* Move the read pointer to block `pos`; 0 when that audio is not in the ring. */
static int lr_jump(u32 pos)
{
    u32 f = pos / LR_BLOCKS, sub = pos - f * LR_BLOCKS;
    u32 cur = (u32)(u16)LR_PLAY_SECTOR, fcur = c850_ring_sector_to_frame((int)cur);
    u32 s;

    if (f >= fcur) {
        u32 ahead = f - fcur, have = lr_mod((s32)LR_WRITE_SECTOR - (s32)cur);
        if (ahead + 2u > have)
            return 0;
        s = lr_mod((s32)(cur + ahead));
    } else {
        if (fcur - f > LR_BEHIND_MAX)
            return 0;
        s = lr_mod((s32)cur - (s32)(fcur - f));
    }
    if (c850_ring_sector_to_frame((int)s) != f)
        return 0;
    loop_roll_last_from = lr_read_pos();
    LR_READ_PTR = C850_PCM_RING_BASE + s * C850_PCM_SECTOR_BYTES + sub * LR_BLOCK_BYTES;
    LR_PLAY_SECTOR = (s16)s;
    LR_CLEAR_SEGMENTS();             /* no segment remap past the loop (as a stock wrap does) */
    LR_SEG_SOURCE = 0xFFFFFFFFu;
    loop_roll_last_target = pos;
    loop_roll_last_sector = s;
    return 1;
}

/* Pool 0x0C07730C: the forward loop-out test of irq7_dspdreq_isr, once per
 * block while a loop plays; the read pointer already names the next block. */
int loop_roll_isr(int sector, u32 out, int z)
{
    u32 ph = lr_phase;

    if (ph == LR_ROLL || ph == LR_JUMP) {
        if (!lr_counting) {
            lr_pos = lr_read_pos();
            loop_roll_start_pos = lr_pos;
            loop_roll_start_tick = C850_TICK_MS;
            lr_counting = 1;
        } else {
            lr_pos++;
        }
        loop_roll_isr_calls++;
        if (ph == LR_JUMP) {
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
    return LR_STOCK_OUT_REACHED(sector, out, z);
}

static int lr_looping(void)
{
    return C850_PLAYER_STATE == C850_STATE_LOOPING && C850_LOOP_ACTIVE == 1;
}

/* Shared key filter (c850_ui_keys, pool 0x0C0ED038), playertask, on key changes. */
static void loop_roll_key_filter(const u16 *k, const u16 *p, u16 *own)
{
    if (c850_ui_flag(C850_UI_FLAG_LOOP_ROLL_OFF))
        return;
    if (c850_ui_shift(k, p, own, C850_K0_LOOP_OUT)) {
        lr_mode = !lr_mode;
        loop_roll_toggles++;
        c850_ui_flash(lr_mode ? "ROLL ON" : "ROLL OFF");
        return;
    }
    if (lr_phase == LR_IDLE) {
        int hc_active = !c850_ui_flag(C850_UI_FLAG_HOT_CUES_OFF) && c850_ui_flag(C850_UI_FLAG_HOT_CUES);
        if (lr_mode && (p[1] & LR_BEATS) && !hc_active &&
            C850_IS_USB_MODE == 1 && C850_PLAYER_STATE == C850_STATE_PLAYING) {
            lr_counting = 0;
            lr_phase = LR_ROLL;      /* the stock handler makes the loop from this press */
            loop_roll_rolls++;
        }
    } else if (lr_phase == LR_ROLL && !(k[1] & LR_BEATS)) {
        if (lr_counting && lr_looping())
            lr_phase = LR_JUMP;      /* the ISR lands at its next block */
        else
            lr_phase = LR_IDLE;      /* the press made no loop */
    }
}
C850_UI_KEY_FILTER(loop_roll_key_filter);

/* Shared state tick (c850_ui_tick, pool 0x0C0EC838): every player cycle,
 * before the key handlers. */
static void loop_roll_tick(void)
{
    if (c850_ui_flag(C850_UI_FLAG_LOOP_ROLL_OFF)) {
        lr_mode = 0;
        if (lr_phase == LR_IDLE)
            return;
    }
    switch (lr_phase) {
    case LR_ROLL:
        if (lr_counting && !lr_looping()) {
            lr_phase = LR_IDLE;      /* EXIT, CUE, a jump... ended it */
            loop_roll_aborts++;
        }
        break;
    case LR_JUMP:
        if (!lr_looping() || ++lr_wait > 50) {   /* no feeder block for ~0.6 s */
            lr_phase = LR_IDLE;
            lr_wait = 0;
            loop_roll_aborts++;
        }
        break;
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

static const char *lr_utility_value(void)
{
    return c850_ui_flag(C850_UI_FLAG_LOOP_ROLL_OFF) ? "OFF" : "ON";
}

static void lr_utility_select(void)
{
    int off = !c850_ui_flag(C850_UI_FLAG_LOOP_ROLL_OFF);

    if (off)
        lr_mode = 0;
    c850_ui_set_flag(C850_UI_FLAG_LOOP_ROLL_OFF, off);
}

static const c850_ui_utility_item lr_utility_item = {
    100u, "LOOP ROLL", lr_utility_value, lr_utility_select,
};
C850_UI_UTILITY_ITEM(lr_utility_item);
