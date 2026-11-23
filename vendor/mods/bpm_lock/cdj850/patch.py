# SPDX-License-Identifier: GPL-2.0-or-later
"""bpm_lock on the CDJ-850 MAIN 1.13: CDJ-350 BPM Lock with ±100% range.

Enables MAIN's dormant BPM-lock dispatcher (FUN_0C0D1F14, state block
0x0CE649AC) via BEAT SELECT + MASTER TEMPO (short = lock/unlock, hold 1 s =
Master BPM edit mode with rotary selector). Code is C in payload/bpm_lock.c
plus the shared payload/common/c850_ui.c (PAYLOAD_COMMON).
"""
from sigpatch import Anchor, DataPatch, Edit, Mod, PoolRedirect

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
    # 0x0C0ECEF8: shared key filter chain c850_ui_keys (pool 0x0C0ED038).
    'bpm_lock': _redirect(
        'BPM lock: key filter after the panel key remap',
        [0xD44E, 0x2AD1, 0xDE4E, 0x4E0B, 0x2401, 0xD94E], 2,
        0x0C0DD492, 'c850_ui_keys'),
    # 0x0C0EC73A: shared state tick c850_ui_tick (pool 0x0C0EC838).
    'bpm_lock_tick': _redirect(
        'BPM lock: key hold, WIDE sync and Master BPM edit monitor at start of state tick',
        [0x4F16, 0x4BF0, 0xD33E, 0x434B, 0xD63E, 0x6060], 2,
        0x0C0EB388, 'c850_ui_tick'),
    # 0x0C0D1D22..0x0C0D1D2E in FUN_0C0D1CE6: widen ±16% (±0x640) check to ±100% (±0x2710).
    'bpm_lock_range1': DataPatch(
        what='BPM lock: widen FUN_0C0D1CE6 lock pitch range from ±16% to ±100%',
        target='main',
        fw_versions=('1.13',),
        table=bytes.fromhex('3017890b37078b02e001a00480ee3703'),
        before=12,
        reach=12,
        edits=(
            Edit(bytes.fromhex('01f0f9c0'), bytes.fromhex('01f0d8f0')),
            Edit(bytes.fromhex('01000640'), bytes.fromhex('01002710')),
        ),
    ),
    # 0x0C0D1D82..0x0C0D1D8E in FUN_0C0D1D56: widen ±16% (±0x640) check to ±100% (±0x2710).
    'bpm_lock_range2': DataPatch(
        what='BPM lock: widen FUN_0C0D1D56 lock pitch range from ±16% to ±100%',
        target='main',
        fw_versions=('1.13',),
        table=bytes.fromhex('32578901a001e200e2017f04027b'),
        before=12,
        reach=12,
        edits=(
            Edit(bytes.fromhex('05f0f9c0'), bytes.fromhex('05f0d8f0')),
            Edit(bytes.fromhex('05000640'), bytes.fromhex('05002710')),
        ),
    ),
    # 0x0C0D1B6E: bf; mov.l @(0x0C0D1DA4),r2; mov #0x64,r5; bra; mov #2,r4; mov.l @(..),r6; movu.w
    'bpm_lock_notify1': _redirect(
        'BPM lock: dot-matrix status flash for FUN_0C0D1B58..FUN_0C0D1CC6',
        [0x8B03, 0xD28C, 0xE564, 0xA031, 0xE402, 0xD68B, 0x3261, 0x9000], 1,
        0x0C06E58A, 'bpm_lock_notify'),
    # 0x0C0D1E2C: mov #0x19,r5; mov.l @(..),r7; mov.l r14,@r7; mov #4,r4; mov.l @(0x0C0D1F34),r2; movmu.l; jmp @r2; nop
    'bpm_lock_notify2': _redirect(
        'BPM lock: dot-matrix status flash for FUN_0C0D1DD8..FUN_0C0D1E40',
        [0xE519, 0xD740, 0x27E2, 0xE404, 0xD23F, 0x4EF4, 0x422B, 0x0009], 4,
        0x0C06E58A, 'bpm_lock_notify'),
    'bpm_lock_display': Mod(
        what='BPM lock: stage Master / locked BPM and locked pitch on the 7-segment display',
        target='main',
        fw_versions=('1.13',),
        args=[],
        literal=False,
        hook=True,
        after=('bpm999',),
        routine='payload:bpm_lock_display_hook',
        **BPM_SITE,
    ),
    'bpm_lock_flash': Mod(
        what='BPM lock: BPM LOCK ON/OFF and MASTER BPM flash on the dot matrix',
        target='main',
        fw_versions=('1.13',),
        args=[],
        literal=False,
        hook=True,
        after=('bpm_lock_display', 'bpm_decimals', 'phase_meter'),
        routine='payload:c850_ui_flash_hook',
        **BPM_SITE,
    ),
}
