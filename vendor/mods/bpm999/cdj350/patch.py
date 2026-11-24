# SPDX-License-Identifier: GPL-2.0-or-later
"""bpm999 on the CDJ-350 MAIN 1.14: rekordbox grid tempos above 180.0.

Three immediates, no code (see ../README.md):

* bpm999grid: the beat-grid tempo routine clamps tenths to 0x708 = 180.0
  (movi20 at 0x0C06B866, same code as the CDJ-850's FUN_0c073788). Raised to
  9990 (999.0).
* bpm999range: BPM LOCK takes the deck's base tempo (0x0C9B8E24) as the
  master BPM when it lies in 60.0..199.9 (cmp/gt movi20 #1999 at 0x0C0C6064)
  and falls back to 130.0 ("Default 130") outside it. The upper bound becomes
  9990, so a 200+ track locks at its own tempo.
* bpm999lock: the rotary edit of the master BPM (0x0C0C61BC) steps within
  40.0..199.9 (movi20 #1999 at 0x0C0C61D0). The upper bound becomes 9990.

The auto BPM counter (0x0C06A86E, 0x0C06A924: 0x708) is the 70-180
detection window and stays as it is.
"""
from sigpatch import DataPatch, Edit

MODS = {
    'bpm999grid': DataPatch(
        what='BPM 999: beat-grid tempo clamp 180.0 -> 999.0 (0x0C06B866)',
        target='main',
        fw_versions=('1.14',),
        table=bytes.fromhex('60530e2d64ed74053145'),
        before=0,
        reach=14,
        edits=(Edit(bytes.fromhex('04000708'), bytes.fromhex('04002706')),),
    ),
    'bpm999range': DataPatch(
        what='BPM 999: BPM LOCK takes base tempos 60.0..999.0 as master BPM (0x0C0C6064)',
        target='main',
        fw_versions=('1.14',),
        table=bytes.fromhex('8b1dd44c3641900026688908'),
        before=0,
        reach=16,
        edits=(Edit(bytes.fromhex('050007cf'), bytes.fromhex('05002706')),),
    ),
    # 0x0C0C61C8: mov.l r1,@r2; mov.l @(4,r7),r2; tst r4,r4; mov #0,r6; movi20 #1999,r4
    'bpm999lock': DataPatch(
        what='BPM 999: master BPM edit range 40.0..199.9 -> 40.0..999.0 (0x0C0C61D0)',
        target='main',
        fw_versions=('1.14',),
        table=bytes.fromhex('221252712448e600'),
        before=0,
        reach=12,
        edits=(Edit(bytes.fromhex('040007cf'), bytes.fromhex('04002706')),),
    ),
}
