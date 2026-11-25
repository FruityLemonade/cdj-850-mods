# SPDX-License-Identifier: GPL-2.0-or-later
"""hot_cues on the CDJ-850 MAIN 1.13: BEAT 1/2/4 as hot cues A/B/C.

Six pool-word redirects and one hook on a shared span, no cave bytes; the
code is C in payload/hot_cues.c and the shared payload/common/c850_ui.c
(PAYLOAD_COMMON; shared SDRAM payload, mods/payloadbuild.py). Section 4b.11
of docs/cdj850-modding.md has the trace; ../README.md the behaviour.

* hot_cues: pool 0x0C0ED038 (FUN_0C0DD492), read only by the mov.l at
  0x0C0ECEFC in panel_keys_to_internal_words, right after K0..K2 are rebuilt
  from the panel: the shared key filter chain c850_ui_keys, which calls
  hot_cues_key_filter (BEAT SELECT + RELOOP toggles the mode; in hot-cue mode
  BEAT 1/2/4/8 are kept from the stock handlers and queued). quant_loops
  redirects the same word to the same routine; whichever goes in second finds
  it already there.
* hot_cues_tick: pool 0x0C0EC838 (FUN_0C0EB388), read only at 0x0C0EC73E, the
  first call of the state tick FUN_0C0EC73C (before every key handler): runs
  a queued jump/record/delete, gives PLAY after a jump that was playing, and
  checks the loaded track's ANLZ.
* hot_cues_select / hot_cues_select2: pools 0x0C0D41BC and 0x0C0D3CC8
  (cue_select_from_list), read only at 0x0C0D3F5C and 0x0C0D3B94, the two USB
  branches of cue_call_prev_next (0x0CE6D254 = 0/1 or other): for a hot-cue
  jump the selection is the hot cue (the stock memory-cue CALL otherwise).
* hot_cues_leds: pool 0x0C0ED244 (wai_flg), read only at 0x0C0ED1B0 in the
  playertask loop: while playertask waits, D154 and 0x0C1E1088 hold the LED
  pattern for display_stage_navigation_leds (DisplayTask) and their real
  values otherwise.
* hot_cues_anlz: pool 0x0C0C195C (FUN_0C0C5960), read only at 0x0C0C17CE in
  the ANLZ hot-cue setter FUN_0C0C166C: empties the slot for a delete and keeps
  the PCOB count/length right before the section write.
* hot_cues_flash: hook (c850_ui_flash_hook) on the tail of display_stage_bpm,
  0x0C0642AA..0x0C0642B8, the span bpm999 and bpm_decimals use (shared
  trampoline), after bpm_decimals: shows HOT CUE ON / HOT CUE OFF on the
  dot matrix for 1.5 s after a toggle and puts the rows back.

Every pool word, the instructions that read them and the routines called are
outside the boot-critical closure (mods/bootcrit.py; tests/test_cdj850_hot_cues.py).
"""
from sigpatch import Anchor, Mod, PoolRedirect

PAYLOAD_COMMON = ('c850_ui',)

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


MODS = {
    # 0x0C0ECEF8: mov.l @(..),r4; mov.w r13,@r10; mov.l @(0x0C0ED038),r14; jsr @r14;
    # mov.w r0,@r4; mov.l @(..),r9
    'hot_cues': _redirect(
        'hot cues: key filter after the panel key remap',
        [0xD44E, 0x2AD1, 0xDE4E, 0x4E0B, 0x2401, 0xD94E], 2,
        0x0C0DD492, 'c850_ui_keys'),
    # 0x0C0EC73A: mov.l r1,@-r15; movmu.l r11,@-r15; mov.l @(0x0C0EC838),r3; jsr/n @r3;
    # mov.l @(..),r6; mov.b @r6,r0
    'hot_cues_tick': _redirect(
        'hot cues: actions and PLAY at the start of the state tick',
        [0x4F16, 0x4BF0, 0xD33E, 0x434B, 0xD63E, 0x6060], 2,
        0x0C0EB388, 'c850_ui_tick'),
    # 0x0C0D3F58: ..; ..; mov.l @(0x0C0D41BC),r3; jsr @r3; mov #1,r5; ..
    'hot_cues_select': _redirect(
        'hot cues: the hot cue as the CALL selection',
        [0x0CE6, 0xCA88, 0xD397, 0x430B, 0xE501, 0x6E0C], 2,
        0x0C0D275A, 'hot_cues_select'),
    # 0x0C0D3B90: nop; mov #2,r5; mov.l @(0x0C0D3CC8),r3; jsr @r3; mov r1,r4; extu.b r0,r14
    'hot_cues_select2': _redirect(
        'hot cues: the hot cue as the CALL selection (second USB branch)',
        [0x0009, 0xE502, 0xD34C, 0x430B, 0x6413, 0x6E0C], 2,
        0x0C0D275A, 'hot_cues_select'),
    # 0x0C0ED1AC: mov #1,r6; mov #0x37,r5; mov.l @(0x0C0ED244),r2; jsr @r2; mov #6,r4; mov r0,r5
    'hot_cues_leds': _redirect(
        'hot cues: beat-loop LED pattern while playertask waits',
        [0xE601, 0xE537, 0xD224, 0x420B, 0xE406, 0x6503], 2,
        0x0C0C9E7C, 'hot_cues_wait'),
    # 0x0C0C17CA: mov r10,r4; mov r12,r5; mov.l @(0x0C0C195C),r14; jsr @r14; mov r11,r7; bra
    'hot_cues_anlz': _redirect(
        'hot cues: delete and PCOB count in the ANLZ hot-cue write',
        [0x64A3, 0x65C3, 0xDE63, 0x4E0B, 0x67B3, 0xA001], 2,
        0x0C0C5960, 'hot_cues_anlz_write'),
    # 0x0C0F5348: mov.b @r6,r5; mov.l @(..),r7; mov.l @(0x0C0F54D8),r2; jsr @r2; mov r13,r6;
    # add #12,r15 -- the cue prefill request FUN_0C008C8A(4, .., 400, 120, ..) of the audio task.
    # The pool word is inside the boot-critical closure: players/cdj850.GATED_BOOT_POOLS.
    'hot_cues_prefill': _redirect(
        'hot cues: a short ring prefill after a hot-cue CALL (gated, audio task)',
        [0x6560, 0xD760, 0xD262, 0x420B, 0x66D3, 0x7F0C], 2,
        0x0C008C8A, 'hot_cues_ring_request'),
    'hot_cues_flash': Mod(
        what='hot cues: HOT CUE ON / OFF on the display after a toggle',
        target='main',
        fw_versions=('1.13',),
        args=[],
        literal=False,
        hook=True,
        after=('bpm_decimals', 'phase_meter'),
        routine='payload:c850_ui_flash_hook',
        **BPM_SITE,
    ),
    'hot_cues_utility_scroll': _redirect(
        'hot cues: UTILITY list continues after VERSION No. (shared c850_ui items)',
        [0x35F1, 0x4020, 0xD26D, 0x424B, 0xD56D, 0xB6CC, 0x6450], 2,
        0x0C0E4DF6, 'c850_ui_utility_scroll'),
    'hot_cues_utility_select': _redirect(
        'hot cues: HOT CUES ON/OFF in UTILITY (shared c850_ui items)',
        [0x3551, 0x8000, 0xD22A, 0x420B, 0xE403, 0xA1CF, 0x0009], 2,
        0x0C0E4C14, 'c850_ui_utility_select'),
    'hot_cues_usb_settings': _redirect(
        'hot cues: read the mod settings file on the stick at mount (shared c850_ui)',
        [0x1D01, 0xD575, 0xD775, 0x470B, 0x64D3, 0x2008, 0x8903], 2,
        0x0C0C45B4, 'c850_ui_usb_mount'),
}
