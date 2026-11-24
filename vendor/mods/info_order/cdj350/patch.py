# SPDX-License-Identifier: GPL-2.0-or-later
"""info_order on the CDJ-350 MAIN 1.14: INFO pages "Title - Artist",
"128.23 8A" (actual BPM with two decimals, Camelot key) and the rating.

Code is C in payload/info_order.c (shared SDRAM payload); the patches are
four pool-word redirects, no cave bytes. docs/cdj350.md, "INFO pages".

* info_order_page0: pool 0x0C068428 (the artist getter 0x0C06C3DC, read at
  0x0C06833A by the track view's row 2 on INFO page 0): "Title - Artist".
* info_order_page1: pool 0x0C06842C (the album getter 0x0C06C3F8, read at
  0x0C068392 on INFO page 1): "128.23 8A", or "RATING ***" on the sub-page.
* info_order_scroll: pool 0x0C0C9048 (info_page_scroll 0x0C0D664A, read at
  0x0C0C8F8A): page 1 takes a second rotary step (the rating sub-page)
  before the stock page 2 (phase meter).
* info_order_meta: pool 0x0C051AEC (the DB server's track-row fetch
  0x0C0719A4, read at 0x0C051A32 in the 0x2002 builder 0x0C0519D0): notes
  each track's rating (row +0x1C) and key name (key id at row +0x2C, looked
  up with 0x0C07D14C). The reply itself is unchanged (it carries four
  fields, none of them the key or rating).
"""
from sigpatch import Anchor, PoolRedirect


def _redirect(what, sig, index, old, routine):
    return PoolRedirect(what=what, target='main', fw_versions=('1.14',),
                        anchor=Anchor(sig=sig, index=index, literal=True),
                        old=old, routine='payload:' + routine)


MODS = {
    # 0x0C068336: mov.l @(copy),r6; cmp/eq #1,r0; mov.l @(0x0C068428),r2; bf/s; mov.l r6,@r15
    'info_order_page0': _redirect(
        'INFO order: page 0 shows "Title - Artist"',
        [0xD63A, 0x8801, 0xD23B, 0x8F09, 0x2F62, 0xD636, 0x420B, 0x6461], 2,
        0x0C06C3DC, 'info_order_page0'),
    # 0x0C06838E: mov.l @(copy),r6; cmp/eq #1,r0; mov.l @(0x0C06842C),r2; bf/s; mov.l r6,@r15
    'info_order_page1': _redirect(
        'INFO order: page 1 shows the actual BPM and Camelot key, then the rating',
        [0xD624, 0x8801, 0xD226, 0x8F09, 0x2F62, 0xDD20, 0x420B, 0x64D1], 2,
        0x0C06C3F8, 'info_order_page1'),
    # 0x0C0C8F80: mov.b @(24,r15),r4; mov.b @(28,r15),r5; mov.l @(page),r13; mov.l @(0x0C0C9048),r3; jsr/n @r3
    'info_order_scroll': _redirect(
        'INFO order: the rating sub-page after page 1',
        [0x34F1, 0x4018, 0x35F1, 0x401C, 0xDD2E, 0xD32F, 0x434B], 5,
        0x0C0D664A, 'info_order_scroll'),
    # 0x0C051A2C: mov.l @(112,r15),r4; mov r15,r5; mov.l @(0x0C051AEC),r8; jsr @r8; add #40,r5
    'info_order_meta': _redirect(
        'INFO order: note each track\'s rating and key name in the DB server',
        [0x34F1, 0x601C, 0x65F3, 0xD82E, 0x480B, 0x7528], 3,
        0x0C0719A4, 'info_order_track_row'),
}
