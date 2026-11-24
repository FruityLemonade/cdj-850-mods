# SPDX-License-Identifier: GPL-2.0-or-later
"""version_display on the CDJ-850 MAIN 1.13: show custom + base version in UTILITY.

Stock UTILITY -> VERSION No. (FUN_0c067b8c, item 6) copies "Ver. " into row 2
(0x0C9D1B6F) and calls FUN_0c067972, which hardcodes "1.13" instead of reading
the flash version label copied to 0x0CE6D242 at boot (version_label_init).

This cave-assembly hook sits on the shared display_stage_bpm tail (0x0C0642AA,
outside the boot-critical closure, running at 50 Hz in DisplayTask before
FUN_0C064CA4 serializes dot-matrix rows to the panel). In UTILITY mode (12),
level 2, item 6 (VERSION No.), it rewrites dot-matrix row 2 (0x0C9D0D22) to
"Ver. <running> (1.13)", e.g. "Ver. 1.23 (1.13)".
"""
from sigpatch import Mod

# Tail of display_stage_bpm, shared with bpm999 / waveform / info_order / etc.
BPM_SITE = dict(
    sig=[0xD62E, 0x6260, 0x2228, 0x8902, 0xD62A, 0x6061, 0x8154],
    start=0,
    end=7,
)

MODS = {
    'version_display': Mod(
        what='Version display: UTILITY VERSION No. shows "<custom> (1.13)"',
        target='main',
        fw_versions=('1.13',),
        args=[],
        literal=False,
        hook=True,
        **BPM_SITE,
    ),
}
