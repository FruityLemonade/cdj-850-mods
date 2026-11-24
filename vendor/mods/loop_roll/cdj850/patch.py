# SPDX-License-Identifier: GPL-2.0-or-later
"""loop_roll on the CDJ-850 MAIN 1.13: hold a beat button for a slip loop (loop roll).

Three pool-word redirects and one hook on a shared span, no cave bytes; the code
is C in payload/loop_roll.c and the shared payload/common/c850_ui.c
(PAYLOAD_COMMON). Section 12b of docs/cdj850-modding.md has the trace.

* loop_roll: pool 0x0C07730C (isr_loop_out_reached, 0x0C0158D4), read only by
  the mov.l at 0x0C07715E in irq7_dspdreq_isr (the forward path's loop-out test,
  once per 84-sample block while a loop plays). The ISR is boot-critical code;
  MAIN installs its vector at run time, so the static walk of mods/bootcrit.py
  does not reach it; the generated service-mode gate still returns to stock.
  loop_roll_isr counts blocks for the shadow playhead and, on release, moves the
  read pointer there instead of wrapping.
* loop_roll_keys / loop_roll_tick: the shared key filter (pool 0x0C0ED038) and
  state tick (pool 0x0C0EC838) of c850_ui, as hot_cues and quant_loops redirect
  them: BEAT SELECT + LOOP OUT toggles roll mode, beat presses start a roll, the
  release asks the ISR for the jump, and the tick then gives one RELOOP/EXIT.
* loop_roll_flash: ROLL ON / ROLL OFF on the dot matrix (c850_ui_flash_hook on
  the tail of display_stage_bpm, the shared span).
"""
from sigpatch import Anchor, Mod, PoolRedirect

PAYLOAD_COMMON = ('c850_ui',)

BPM_SITE = dict(
    sig=[0xD62E, 0x6260, 0x2228, 0x8902, 0xD62A, 0x6061, 0x8154],
    start=0,
    end=7,
)


def _redirect(what, sig, index, old, routine):
    return PoolRedirect(what=what, target='main', fw_versions=('1.13',),
                        anchor=Anchor(sig=sig, index=index, literal=True),
                        old=old, routine='payload:' + routine)


MODS = {
    # 0x0C07715A: mov.w @r7,r4; mov #0,r6; mov.l @(0x0C07730C),r13; jsr @r13;
    # mov.l @(16,r15),r5; extu.b r0,r14
    'loop_roll': _redirect(
        'loop roll: count feeder blocks and land on the shadow playhead (IRQ7 loop-out test)',
        [0x6471, 0xE600, 0xDD6B, 0x4D0B, 0x55F4, 0x6E0C], 2,
        0x0C0158D4, 'loop_roll_isr'),
    # 0x0C0ECEF8: the hot_cues key-filter word (mods/hot_cues/cdj850/patch.py)
    'loop_roll_keys': _redirect(
        'loop roll: BEAT SELECT + LOOP OUT, beat press and release (shared key filter)',
        [0xD44E, 0x2AD1, 0xDE4E, 0x4E0B, 0x2401, 0xD94E], 2,
        0x0C0DD492, 'c850_ui_keys'),
    # 0x0C0EC73A: the hot_cues tick word
    'loop_roll_tick': _redirect(
        'loop roll: leave the loop after the landing (shared state tick)',
        [0x4F16, 0x4BF0, 0xD33E, 0x434B, 0xD63E, 0x6060], 2,
        0x0C0EB388, 'c850_ui_tick'),
    'loop_roll_flash': Mod(
        what='loop roll: ROLL ON / OFF on the display after a toggle',
        target='main',
        fw_versions=('1.13',),
        args=[],
        literal=False,
        hook=True,
        after=('bpm_decimals', 'phase_meter'),
        routine='payload:c850_ui_flash_hook',
        **BPM_SITE,
    ),
}
