# SPDX-License-Identifier: GPL-2.0-or-later
"""phase_meter on the CDJ-850 MAIN 1.13: Beat & phase meter on INFO Page 2.

Inserts three placeholder items (codes 0x13, 0x14, 0x15) at indices 2..4 of
loaded USB track DB records so INFO Page 2 becomes the Beat/Phase Meter and
the stock Page 2+ items shift down by one page. Works whether BPM Lock is
enabled, disabled, or not built. Code is C in payload/phase_meter.c.
"""
from sigpatch import Mod

# Tail of display_stage_bpm, as in mods/bpm999/cdj850/patch.py.
BPM_SITE = dict(
    sig=[0xD62E, 0x6260, 0x2228, 0x8902, 0xD62A, 0x6061, 0x8154],
    start=0,
    end=7,
)

MODS = {
    'phase_meter': Mod(
        what='Phase meter: 4-beat bar and sub-beat phase indicator on INFO Page 2',
        target='main',
        fw_versions=('1.13',),
        args=[],
        literal=False,
        hook=True,
        after=('bpm999', 'bpm_decimals', 'bpm_lock_display', 'info_order'),
        routine='payload:phase_meter_hook',
        **BPM_SITE,
    ),
}
