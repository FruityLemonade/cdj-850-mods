/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * c850_loop_out.c -- the forward loop-out test of the IRQ7 feeder, shared by
 * the mods that wrap a loop themselves (docs/cdj850-modding.md section 12b,
 * docs/cdj850-block-loops-design.md).
 *
 * irq7_dspdreq_isr calls isr_loop_out_reached(play_sector, out_frame, 0)
 * (0x0C0158D4) through pool 0x0C07730C once per 84-sample block while a loop
 * plays forward; the read pointer already names the next block. One pool word
 * can point at one routine, so every mod that wants it redirects the word to
 * c850_loop_out_isr (the second redirect finds it 'present') and names
 * 'c850_loop_out' in PAYLOAD_COMMON. The dispatcher asks, in this order:
 *
 *   loop_roll_out_hook    (mods/loop_roll)   a roll counts and lands
 *   quant_loops_out_hook  (mods/quant_loops) the block-precise grid loop
 *
 * and then the stock test. A hook returns -1 for "not mine" or the value the
 * ISR gets (0 = no stock wrap). The weak no-ops below stand in for a mod that
 * is not built. Reverse play (pool 0x0C076C74) stays stock.
 *
 * Context: interrupt (irq7_dspdreq_isr's stack): no RTOS calls, small frames.
 */
#include "c850.h"

#define LO_STOCK_OUT_REACHED C850_FN(0x0C0158D4, int, (int sector, u32 out, int z))
#define LO_CLEAR_SEGMENTS    C850_FN(0x0C015B18, void, (void))
#define LO_REQUEST_OUT       C850_FN(0x0C00A4CC, int, (u32 frame, int z))
#define LO_SEG_SOURCE        C850_U32(0x0C7F677C)   /* block source while loop segments exist */
#define LO_RESIZE_COUNT      C850_U8(0x0C7F66F8)    /* FUN_0C00A4CC forces reset past 0x1e */
#define LO_BEHIND_MAX        1000u                  /* played sectors the ring keeps (<= 1001) */

u32 quant_loops_resize_adjust(u32 frame) __attribute__((weak, noinline));
u32 quant_loops_resize_adjust(u32 frame)
{
    return frame;
}

/* Pool 0x0C0E62BC (FUN_0C00A4CC) in auto_beat_loop_set's state-4 resize sender.
 * Doubles and first-half halvings (play_frame < new_out) suppress stock's
 * unconditional reset to loop_in so playback continues smoothly; halvings
 * past new_out (play_frame >= new_out) immediately jump back to loop_in. */
int c850_resize_req(u32 frame, int z)
{
    u32 in = C850_LOOP_IN_FRAME;

    frame = quant_loops_resize_adjust(frame);
    if (C850_IS_USB_MODE == 1u && C850_LOOP_ACTIVE == 1u &&
        C850_PLAYER_STATE == C850_STATE_LOOPING && frame > in) {
        u32 play = C850_PLAY_FRAME;
        if (play < frame) {
            int r;
            C850_PLAY_FRAME = in;
            LO_RESIZE_COUNT = 0u;
            r = LO_REQUEST_OUT(frame, z);
            if (C850_PLAY_FRAME == in)
                C850_PLAY_FRAME = play;
            return r;
        }
        if (play <= in + 1u) {
            LO_RESIZE_COUNT = 0x1Fu;
            return LO_REQUEST_OUT(frame, z);
        }
    }
    return LO_REQUEST_OUT(frame, z);
}

int loop_roll_out_hook(int sector, u32 out, int z) __attribute__((weak, noinline));
int loop_roll_out_hook(int sector, u32 out, int z)
{
    (void)sector;
    (void)out;
    (void)z;
    return -1;
}

int quant_loops_out_hook(int sector, u32 out, int z) __attribute__((weak, noinline));
int quant_loops_out_hook(int sector, u32 out, int z)
{
    (void)sector;
    (void)out;
    (void)z;
    return -1;
}

/* Pool 0x0C07730C (forward path of irq7_dspdreq_isr). */
int c850_loop_out_isr(int sector, u32 out, int z)
{
    int r = loop_roll_out_hook(sector, out, z);

    if (r < 0)
        r = quant_loops_out_hook(sector, out, z);
    if (r < 0)
        r = LO_STOCK_OUT_REACHED(sector, out, z);
    return r;
}

static u32 lo_mod(s32 v)
{
    while (v < 0)
        v += (s32)C850_PCM_RING_SLOTS;
    while (v >= (s32)C850_PCM_RING_SLOTS)
        v -= (s32)C850_PCM_RING_SLOTS;
    return (u32)v;
}

/* Position (blocks: frame * 7 + block in the sector) of the block the read
 * pointer names. Read from the pointer each time: any jump, ours or not, is
 * seen at once. */
u32 c850_read_pos(void)
{
    u32 s = (u32)C850_RING_READ_INDEX;
    u32 off = C850_PCM_READ_PTR - (C850_PCM_RING_BASE + s * C850_PCM_SECTOR_BYTES), b = 0;

    while (off >= C850_PCM_BLOCK_BYTES && b < C850_BLOCKS_PER_FRAME - 1u) {
        off -= C850_PCM_BLOCK_BYTES;
        b++;
    }
    return c850_ring_sector_to_frame((int)s) * C850_BLOCKS_PER_FRAME + b;
}

/* Move the read pointer to block `pos` when that audio is in the PCM ring:
 * ahead up to two sectors before the write index, behind up to LO_BEHIND_MAX
 * sectors, and the sector table confirms the frame. Clears the loop segments
 * as a stock wrap does. 1 = moved, 0 = not in the ring (nothing changed).
 * Stays on 336-byte boundaries from the ring base (the feeder's rule). */
int c850_ring_jump(u32 pos)
{
    u32 f = pos / C850_BLOCKS_PER_FRAME, sub = pos - f * C850_BLOCKS_PER_FRAME;
    u32 cur = (u32)C850_RING_READ_INDEX, fcur = c850_ring_sector_to_frame((int)cur);
    u32 s;

    if (f >= fcur) {
        u32 ahead = f - fcur, have = lo_mod((s32)C850_RING_WRITE_INDEX - (s32)cur);
        if (ahead + 2u > have)
            return 0;
        s = lo_mod((s32)(cur + ahead));
    } else {
        if (fcur - f > LO_BEHIND_MAX)
            return 0;
        s = lo_mod((s32)cur - (s32)(fcur - f));
    }
    if (c850_ring_sector_to_frame((int)s) != f)
        return 0;
    C850_PCM_READ_PTR = C850_PCM_RING_BASE + s * C850_PCM_SECTOR_BYTES + sub * C850_PCM_BLOCK_BYTES;
    C850_RING_READ_INDEX = (u16)s;
    LO_CLEAR_SEGMENTS();             /* no segment remap past the loop (as a stock wrap does) */
    LO_SEG_SOURCE = 0xFFFFFFFFu;
    return 1;
}
