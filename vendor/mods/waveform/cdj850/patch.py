# SPDX-License-Identifier: GPL-2.0-or-later
"""waveform on the CDJ-850 MAIN 1.13: zoomed scrolling half-waveform on INFO Page 1.

Streams a 100-column zoomed/scrolling waveform into the existing 100x5 VFD strip
(0x0C9D1199 / 0x0C9D1188, transmitted by panel_tx_waveform at 0x0C0662AE) at up
to 50 Hz while keeping rows 1-2 untouched (fully compatible with info_order and
phase_meter). Scrolling counterclockwise on INFO Page 1 (cursor 0) zooms out:
close (4 s) -> medium (16 s) -> whole-track overview; scrolling clockwise zooms
back in before advancing to INFO Page 2. Code is C in payload/waveform.c.
"""
from sigpatch import Anchor, Mod, PoolRedirect

PAYLOAD_COMMON = ('c850_ui',)

# Tail of display_stage_bpm, as in mods/bpm999/cdj850/patch.py.
BPM_SITE = dict(
    sig=[0xD62E, 0x6260, 0x2228, 0x8902, 0xD62A, 0x6061, 0x8154],
    start=0,
    end=7,
)

# INFO screen rotary scroll handler in FUN_0C0E655C (0x0C0E782E, 2-mod-4 aligned):
#   movu.b @(0,r12),r0; mov.l @(0x0C0E7998,pc),r5; mov.b @(r0,r5),r9;
#   mov #2,r2; extu.b r9,r6; cmp/ge r2,r6
ROTARY_INFO_SITE = dict(
    sig=[0x30C1, 0x8000, 0xD559, 0x095C, 0xE202, 0x669C, 0x3623],
    start=0,
    end=6,
)

MODS = {
    'waveform': Mod(
        what='Waveform: 50 Hz zoomed scrolling half-waveform in the 100x5 INFO Page 1 strip',
        target='main',
        fw_versions=('1.13',),
        args=[],
        literal=False,
        hook=True,
        after=('bpm999', 'bpm_decimals', 'bpm_lock_display', 'info_order', 'phase_meter'),
        routine='payload:waveform_hook',
        **BPM_SITE,
    ),
    'waveform_rotary': Mod(
        what='Waveform: INFO Page 1 rotary CCW/CW zoom control (close -> medium -> overview)',
        target='main',
        fw_versions=('1.13',),
        args=[],
        literal=False,
        hook=True,
        routine='payload:waveform_rotary_hook',
        **ROTARY_INFO_SITE,
    ),
    'waveform_utility_scroll': PoolRedirect(
        what='Waveform: UTILITY list continues after VERSION No. (shared c850_ui items)',
        target='main', fw_versions=('1.13',),
        anchor=Anchor(sig=[0x35F1, 0x4020, 0xD26D, 0x424B, 0xD56D, 0xB6CC, 0x6450],
                      index=2, literal=True),
        old=0x0C0E4DF6, routine='payload:c850_ui_utility_scroll',
    ),
    'waveform_utility_select': PoolRedirect(
        what='Waveform: cycle playhead between Left 1/3, Center, and Left in UTILITY',
        target='main', fw_versions=('1.13',),
        anchor=Anchor(sig=[0x3551, 0x8000, 0xD22A, 0x420B, 0xE403, 0xA1CF, 0x0009],
                      index=2, literal=True),
        old=0x0C0E4C14, routine='payload:c850_ui_utility_select',
    ),
    # 0x0C0C1010: the USB ANLZ manager set-up at mount (FUN_0C0C45B4), then the mod
    # settings file on the stick (shared c850_ui, as in quant_loops).
    'waveform_usb_settings': PoolRedirect(
        what='Waveform: read the mod settings file on the stick at mount (shared c850_ui)',
        target='main', fw_versions=('1.13',),
        anchor=Anchor(sig=[0x1D01, 0xD575, 0xD775, 0x470B, 0x64D3, 0x2008, 0x8903], index=2, literal=True),
        old=0x0C0C45B4, routine='payload:c850_ui_usb_mount',
    ),
    'waveform_center_marker': PoolRedirect(
        what='Waveform: mark playhead, cues, hotcues, pending loop-in, and loops on the red strip',
        target='main', fw_versions=('1.13',),
        anchor=Anchor(sig=[0x430B, 0x2610, 0xD33A, 0x434B,
                           0xD435, 0x6042, 0xE700], index=2, literal=True),
        old=0x0C0664A8, routine='payload:waveform_center_marker',
    ),
    'waveform_cue_column': PoolRedirect(
        what='Waveform: clear whole-track cue bar and show zoomed active cue on the waveform',
        target='main', fw_versions=('1.13',),
        anchor=Anchor(sig=[0xD350, 0x434B, 0xD350, 0x434B, 0xD350, 0x434B, 0xB005, 0x0009],
                      index=2, literal=True),
        old=0x0C0642CC, routine='payload:waveform_cue_column',
    ),
}
