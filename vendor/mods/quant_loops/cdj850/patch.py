# SPDX-License-Identifier: GPL-2.0-or-later
"""quant_loops on the CDJ-850 MAIN 1.13: loops snap to the rekordbox beat grid.

Only while a beat grid of the loaded USB track is in RAM (the grid gave the
deck's tempo, the same proof bpm_decimals uses); otherwise every routine
passes the stock values through. Code is C in payload/quant_loops.c (shared
SDRAM payload, mods/payloadbuild.py) plus the shared payload/common/c850_ui.c
(PAYLOAD_COMMON) and loop_tick.c; the patches are eleven pool-word redirects and
one hook on a shared span, no cave bytes. Section 4b.9 of
docs/cdj850-modding.md has the trace.

* quant_loops: pool 0x0C0E5D4C, read only by the mov.l at
  0x0C0E5C0E in auto_beat_loop_set's USB new-loop path (FUN_0C073954, track
  -> length slot). It runs after D1C0 (the in frame) and the in ring sector
  0x0C7F6468 are set and before beat_loop_out_from_bpm: the payload moves
  both to the grid beat nearest the in point (behind or ahead) and computes
  the grid out.
* quant_loops_auto_send / _auto_send2: pools 0x0C0E5FC8 and 0x0C0E5D08
  (audio_send_cmd), read only inside auto_beat_loop_set: the cmd 6 request
  of a new loop gets the grid out (resize sends, flag 1, are untouched).
* quant_loops_resize: pool 0x0C0E62CC (audio_send_cmd), read only in the state-4
  resize sender of auto_beat_loop_set (0x0C0E606E .. 0x0C0E623A): the resize (cmd 6,
  resize flag; BEAT n in a loop, loop_resize) gets the block loop of the new length.
* quant_loops_resize_req: pool 0x0C0E62BC (FUN_0C00A4CC), read only at 0x0C0E602E and
  0x0C0E61B4 in the same sender: the out request 0x0C7F64DC a resize writes (the send
  follows only when it returns 1), so the block frame is requested either way.
* quant_loops_auto_keep: pool 0x0C0E5FD0 (FUN_0C0059D2), read only at
  0x0C0E5DAA, right after those sends: stock refreshes the in sector word
  0x0C7F6468 there before the audio task reads it for the queued cmd 6; with
  a snapped in pending the payload leaves the refresh out.
* quant_loops_loop_in: pool 0x0C0DFA80 (FUN_0C0059D2), read only at
  0x0C0DF95E in the LOOP IN path: the in sector moves to the nearest beat.
* quant_loops_drift: pool 0x0C0D7B98 (keys_state4_looping, 0x0C0E0F34), read
  only by the tail jump at 0x0C0D79A6 in player_key_dispatch_by_state (state
  4), to the shared tick c850_loop_tick (mods/payload/include/c850.h; the
  same word and symbol as loop_resize_tick, so with both mods the second is
  'present'). Once per player cycle while a loop plays the payload counts the
  wraps and moves the out of the next pass so that the sum of the pass
  lengths follows the exact grid length (whole frames alternate), then the
  stock handler runs.
* quant_loops_loop_out / _loop_out2: pools 0x0C0E0090 and 0x0C0E02C0
  (audio_send_cmd), read only in the LOOP OUT paths: the out sector (param 5
  of cmd 1 / mode 3) moves to the nearest beat after the loop in.

* quant_loops_keys: pool 0x0C0ED038 (FUN_0C0DD492), read only at 0x0C0ECEFC
  in panel_keys_to_internal_words: the shared key filter chain c850_ui_keys
  (hot_cues redirects the same word to the same routine; the second finds it
  there). It calls quant_loops_key_filter: BEAT SELECT + LOOP IN switches
  quantize (saved in the settings record, c850_ui.h); off = every path stock.
* quant_loops_utility_scroll / _utility_select: pools 0x0C0E797C
  (utility_scroll_item, read at 0x0C0E77C4) and 0x0C0E7F10
  (utility_set_menu_level, read at 0x0C0E7E64): the shared UTILITY list
  c850_ui_utility_scroll / _select (waveform redirects the same words to the
  same routines). Item QUANTIZE after VERSION No. picks the quantize value
  1, 1/2 or 1/4 beat (saved, c850_ui.h; shown while quantize is on); its rows
  are drawn by c850_ui_flash_hook.
* quant_loops_usb_settings: pool 0x0C0C11E8 (FUN_0C0C45B4, the USB ANLZ
  manager set-up of a newly mounted stick), read only at 0x0C0C1010 in
  FUN_0C0C0F88: the shared c850_ui_usb_mount, which calls it and then reads
  the mod settings file /PIONEER/MODSET.DAT from the stick (c850_ui.h;
  waveform redirects the same word to the same routine).
* quant_loops_flash: hook (c850_ui_flash_hook) on the tail of
  display_stage_bpm, 0x0C0642AA..0x0C0642B8 (the shared bpm999 /
  bpm_decimals span), after bpm_decimals: QUANTIZE ON / QUANTIZE OFF on the
  dot matrix for 1.5 s.

Every pool word and every instruction that reads it is outside the
boot-critical closure (mods/bootcrit.py; tests/test_cdj850_quant_loops.py).
"""
from sigpatch import Anchor, Mod, PoolRedirect

PAYLOAD_COMMON = ('c850_ui', 'c850_loop_out')

# Tail of display_stage_bpm, as in mods/bpm999/cdj850/patch.py.
BPM_SITE = dict(
    sig=[0xD62E, 0x6260, 0x2228, 0x8902, 0xD62A, 0x6061, 0x8154],
    start=0,
    end=7,
)


def _redirect(what, sig, index, old, routine):
    return PoolRedirect(what=what, target='main', fw_versions=('1.13',),
                        anchor=Anchor(sig=sig, index=index, literal=True),
                        old=old, routine='payload:' + routine)


AUDIO_SEND_CMD = 0x0C0D7464

MODS = {
    # 0x0C0E5C0A: mov.l r2,@r5 (D1C0); mov.w r4,@r12; mov.l @(0x0C0E5D4C),r5; jsr/n @r5;
    # and #3,r0; mov r15,r5; mov r0,r6
    'quant_loops': _redirect(
        'quant loops: auto beat loop in on the nearest grid beat',
        [0x2522, 0x2C41, 0xD54F, 0x454B, 0xC903, 0x65F3, 0x6603], 2,
        0x0C073954, 'quant_loops_auto_in'),
    # 0x0C0E5D8A: mov #6,r4; mov.l @(D1C0),r12; mov.l @r12,r6; mov.l @(0x0C0E5FC8),r3;
    # jsr @r3; mov r5,r7; add #16,r15; movu.b @(0,r13),r4
    'quant_loops_auto_send': _redirect(
        'quant loops: auto beat loop out on the grid (play path)',
        [0xE406, 0xDC8D, 0x66C2, 0xD38D, 0x430B, 0x6753, 0x7F10, 0x34D1, 0x8000], 3,
        AUDIO_SEND_CMD, 'quant_loops_auto_send'),
    # 0x0C0E5CC2: mov.l r5,@-r15 x2; mov #6,r4; mov.l @(0x0C0E5D08),r3; jsr @r3; mov r5,r7;
    # bra; add #16,r15
    'quant_loops_auto_send2': _redirect(
        'quant loops: auto beat loop out on the grid (cued path)',
        [0x2F56, 0x2F56, 0xE406, 0xD30F, 0x430B, 0x6753, 0xA063, 0x7F10], 3,
        AUDIO_SEND_CMD, 'quant_loops_auto_send'),
    # 0x0C0E6068: mov #6,r4; mov.l @(D1C0),r13; mov.l @r13,r6; mov.l @(0x0C0E62CC),r3; jsr @r3;
    # mov #1,r7; bra (the state-4 resize sender after FUN_0C00A4CC: audio cmd 6 with the
    # resize flag, BEAT n inside a loop and loop_resize; 11 readers, all in this sender)
    'quant_loops_resize': _redirect(
        'quant loops: a resize inside a loop gets the block loop of the new length',
        [0xE406, 0xDD93, 0x66D2, 0xD397, 0x430B, 0xE701, 0xA0E5], 3,
        AUDIO_SEND_CMD, 'quant_loops_auto_send'),
    # 0x0C0E6026: mov #3,r6; mov #0,r5; mov.b r6,@r1; mov.l @(D1C0),r6; mov.l @(0x0C0E62BC),r2;
    # jsr @r2; mov.l @r6,r4; extu.b r0,r0 (the state-4 resize sender: FUN_0C00A4CC(out, 0)
    # writes the out request; the cmd 6 send above follows only when it returns 1)
    'quant_loops_resize_req': _redirect(
        'quant loops: a resize inside a loop requests the block loop of the new length',
        [0xE603, 0xE500, 0x2160, 0xD6A2, 0xD2A3, 0x420B, 0x6462, 0x600C], 4,
        0x0C00A4CC, 'quant_loops_resize_req'),
    # 0x0C0E5DA6: cmp/eq #6,r0; bt; mov.l @(0x0C0E5FD0),r4; jsr/n @r4; mov.l @(0x0C7F6468),r14;
    # mov.l @(FUN_0C00586C),r5; jsr @r5; mov.w @r14,r4
    'quant_loops_auto_keep': _redirect(
        'quant loops: keep the snapped auto loop in sector (post-send refresh)',
        [0x8806, 0x890D, 0xD489, 0x444B, 0xDE89, 0xD589, 0x450B, 0x64E1], 2,
        0x0C0059D2, 'quant_loops_auto_keep'),
    # 0x0C0DF95A: mov #1,r7; mov.l @(D0EC),r12; mov.l @(0x0C0DFA80),r10; jsr @r10;
    # mov.b r7,@r12; mov.l @(0x0C7F6468),r13; mov.w @r13,r4
    'quant_loops_loop_in': _redirect(
        'quant loops: LOOP IN on the nearest grid beat',
        [0xE701, 0xDC47, 0xDA48, 0x4A0B, 0x2C70, 0xDD47, 0x64D1], 2,
        0x0C0059D2, 'quant_loops_loop_in'),
    # 0x0C0E001C: mov #3,r7; bf/s; mov #1,r4; mov.l @(0x0C0E0090),r3; jsr @r3; mov #2,r5;
    # bra 0x0C0E0DFA; add #16,r15
    'quant_loops_loop_out': _redirect(
        'quant loops: LOOP OUT on the nearest grid beat after the loop in',
        [0xE703, 0x8F05, 0xE401, 0xD31B, 0x430B, 0xE502, 0xA6E7, 0x7F10], 3,
        AUDIO_SEND_CMD, 'quant_loops_loop_out'),
    # 0x0C0E0132: the same sequence (second path), bra 0x0C0E0DFA from further on
    'quant_loops_loop_out2': _redirect(
        'quant loops: LOOP OUT on the nearest grid beat after the loop in (second path)',
        [0xE703, 0x8F05, 0xE401, 0xD361, 0x430B, 0xE502, 0xA65C, 0x7F10], 3,
        AUDIO_SEND_CMD, 'quant_loops_loop_out'),
    # 0x0C0ECEF8: the hot_cues key-filter word (mods/hot_cues/cdj850/patch.py)
    'quant_loops_keys': _redirect(
        'quant loops: BEAT SELECT + LOOP IN switches quantize (shared key filter)',
        [0xD44E, 0x2AD1, 0xDE4E, 0x4E0B, 0x2401, 0xD94E], 2,
        0x0C0DD492, 'c850_ui_keys'),
    # The waveform mod's UTILITY words (mods/waveform/cdj850/patch.py), same routines.
    'quant_loops_utility_scroll': _redirect(
        'quant loops: UTILITY list continues after VERSION No. (shared c850_ui items)',
        [0x35F1, 0x4020, 0xD26D, 0x424B, 0xD56D, 0xB6CC, 0x6450], 2,
        0x0C0E4DF6, 'c850_ui_utility_scroll'),
    'quant_loops_utility_select': _redirect(
        'quant loops: QUANTIZE value (1, 1/2, 1/4 beat) in UTILITY (shared c850_ui items)',
        [0x3551, 0x8000, 0xD22A, 0x420B, 0xE403, 0xA1CF, 0x0009], 2,
        0x0C0E4C14, 'c850_ui_utility_select'),
    # 0x0C0C100C: mov.l r0,@(4,r13); mov.l @(0x0C0C11E4),r5; mov.l @(0x0C0C11E8),r7; jsr @r7;
    # mov r13,r4; tst r0,r0; bt
    'quant_loops_usb_settings': _redirect(
        'quant loops: read the mod settings file on the stick at mount (shared c850_ui)',
        [0x1D01, 0xD575, 0xD775, 0x470B, 0x64D3, 0x2008, 0x8903], 2, 0x0C0C45B4, 'c850_ui_usb_mount'),
    'quant_loops_flash': Mod(
        what='quant loops: QUANTIZE ON / OFF on the display after a toggle',
        target='main',
        fw_versions=('1.13',),
        args=[],
        literal=False,
        hook=True,
        after=('bpm_decimals', 'phase_meter'),
        routine='payload:c850_ui_flash_hook',
        **BPM_SITE,
    ),
    # 0x0C0D79A6: mov.l @(0x0C0D7B98),r3; jmp @r3; lds.l @r15+,pr (state 4 ->
    # keys_state4_looping), the four handlers after it, and the jsr/n of the default.
    # The same word and symbol as loop_resize_tick (the shared tick, c850.h).
    'quant_loops_tick': _redirect(
        'quant loops: which loop the block path owns (memory, hot-cue, RELOOP loops; state-4 tick)',
        [0xD37C, 0x432B, 0x4F26, 0xD37B, 0x432B, 0x4F26, 0xD37B, 0x432B, 0x4F26, 0xD37A, 0x432B,
         0x4F26, 0xD37A, 0x432B, 0x4F26, 0xD379, 0x434B], 0,
        0x0C0E0F34, 'c850_loop_tick'),
    # 0x0C07715A: mov.w @r7,r4; mov #0,r6; mov.l @(0x0C07730C),r13; jsr @r13;
    # mov.l @(16,r15),r5; extu.b r0,r14 (irq7_dspdreq_isr, the forward loop-out test).
    # The same word and symbol as loop_roll (the shared dispatcher, c850_loop_out.c).
    'quant_loops_blocks': _redirect(
        'quant loops: block-precise loop wraps on the grid (IRQ7 loop-out test)',
        [0x6471, 0xE600, 0xDD6B, 0x4D0B, 0x55F4, 0x6E0C], 2,
        0x0C0158D4, 'c850_loop_out_isr'),
}
