# SPDX-License-Identifier: GPL-2.0-or-later
"""info_order on the CDJ-850 MAIN 1.13: the INFO screens in the order
Track, BPM / Camelot key / stars, waveform / Artist, Rating, Key / Album, Comment.

The 0x2002 USB track metadata request temporarily enables rating in the stock
builder. The display hook reorders the INFO items and formats the title row.
See docs/cdj850-modding.md section 4b.8.
"""
from sigpatch import Anchor, Mod, PoolRedirect

PAYLOAD_COMMON = ('c850_ui',)

BPM_SITE = dict(
    sig=[0xD62E, 0x6260, 0x2228, 0x8902, 0xD62A, 0x6061, 0x8154],
    start=0,
    end=7,
)

MODS = {
    'info_order': Mod(
        what='INFO order: BPM, Camelot key and rating on the title screen',
        target='main',
        fw_versions=('1.13',),
        args=[],
        literal=False,
        hook=True,
        routine='payload:info_order_hook',
        **BPM_SITE,
    ),
    # The 0x2002 dispatch at 0x0C052C18 loads its handler from 0x0C052D80.
    'info_order_rating': PoolRedirect(
        what='INFO order: include the USB track rating in the metadata reply',
        target='main',
        fw_versions=('1.13',),
        anchor=Anchor(sig=[0x67A3, 0x64B3, 0x6593, 0xD258, 0x420B, 0x66C3, 0xA01F, 0x6E03],
                      index=3, literal=True),
        old=0x0C054A36,
        routine='payload:info_order_track_info',
    ),
}
