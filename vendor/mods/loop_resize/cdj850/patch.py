# SPDX-License-Identifier: GPL-2.0-or-later
"""loop_resize on the CDJ-850 MAIN 1.13: CALL </> halve / double an active loop.

Three pool-word redirects, no hook, no cave bytes; the code is C in
payload/loop_resize.c (shared SDRAM payload, mods/payloadbuild.py).
Section 4b.10 of docs/cdj850-modding.md has the trace.

* loop_resize: pool 0x0C0D9994 (cue_call_prev_next, 0x0C0D3B1A), read only by
  the mov.l at 0x0C0D98AC (CALL >, r4 = 1) and 0x0C0D98EA (CALL <, r4 = 0) in
  player_handle_tempo_and_time_buttons. While a USB loop plays (state 4,
  0x0CE6D0ED == 1) the payload resizes it through the stock state-4 BEAT
  handler FUN_0C0E5390; otherwise it tail-calls cue_call_prev_next.

* loop_resize_tick: pool 0x0C0D7B98 (keys_state4_looping, 0x0C0E0F34), read
  only by the mov.l at 0x0C0D79A6 in player_key_dispatch_by_state (tail jump,
  state 4), to the shared tick c850_loop_tick (mods/payload/include/c850.h;
  the same word and symbol as quant_loops_tick). Its hook loop_resize_state4
  follows which loop out is in effect and sends a halving that had to wait
  for the next loop pass (playhead past the new out); the stock handler runs
  after the hooks.
* loop_resize_memory: pool 0x0C0D9714 (memory_button, 0x0C0D4172), read only
  at 0x0C0D9664. In a resized loop the out record (0x0C7F64B0 + M/S/F) gets
  the effective out for the call, so MEMORY stores the resized loop.

The grid mode follows the on-deck quantize switch (payload/common/c850_ui.c,
PAYLOAD_COMMON; quant_loops toggles it): with quantize off every resize is
in frames.

The pool words and the instructions that read them are outside the
boot-critical closure (mods/bootcrit.py; tests/test_cdj850_loop_resize.py).
"""
from sigpatch import Anchor, PoolRedirect

PAYLOAD_COMMON = ('c850_ui', 'c850_loop_out')


def _redirect(what, sig, index, old, routine):
    return PoolRedirect(what=what, target='main', fw_versions=('1.13',),
                        anchor=Anchor(sig=sig, index=index, literal=True),
                        old=old, routine='payload:' + routine)


MODS = {
    # 0x0C0D98A8: cmp/eq #3,r0; bf; mov.l @(0x0C0D9994),r2; jsr @r2; mov #1,r4; bra 0x0C0D9B3E
    'loop_resize': PoolRedirect(
        what='loop resize: CALL </> halve / double the active loop',
        target='main',
        fw_versions=('1.13',),
        anchor=Anchor(sig=[0x8803, 0x8B04, 0xD239, 0x420B, 0xE401, 0xA144], index=2, literal=True),
        old=0x0C0D3B1A,
        routine='payload:loop_resize_call',
    ),
    # 0x0C0D79A6: mov.l @(0x0C0D7B98),r3; jmp @r3; lds.l @r15+,pr (state 4 ->
    # keys_state4_looping), the four handlers after it, and the jsr/n of the default
    'loop_resize_tick': _redirect(
        'loop resize: follow the loop out and send deferred halvings (state-4 key handler)',
        [0xD37C, 0x432B, 0x4F26, 0xD37B, 0x432B, 0x4F26, 0xD37B, 0x432B, 0x4F26, 0xD37A, 0x432B,
         0x4F26, 0xD37A, 0x432B, 0x4F26, 0xD379, 0x434B], 0,
        0x0C0E0F34, 'c850_loop_tick'),
    # 0x0C0D9660: movrt r11; mov.l @(..),r6; mov.l @(0x0C0D9714),r2; jsr @r2; mov.l r11,@r6;
    # bra 0x0C0D9B3E
    'loop_resize_memory': _redirect(
        'loop resize: MEMORY stores the resized loop',
        [0x0B39, 0xD62B, 0xD22B, 0x420B, 0x26B2, 0xA268], 2,
        0x0C0D4172, 'loop_resize_memory_button'),
    'loop_resize_req': _redirect(
        'loop resize: smooth CDJ-2000NXS2/3000 resize without resetting to loop_in when play < out',
        [0xE603, 0xE500, 0x2160, 0xD6A2, 0xD2A3, 0x420B, 0x6462, 0x600C], 4,
        0x0C00A4CC, 'c850_resize_req'),
}
