# SPDX-License-Identifier: GPL-2.0-or-later
"""Played markers and track sorting for the CDJ-850 USB browser on MAIN 1.13."""

from sigpatch import Anchor, Mod, PoolRedirect

PAYLOAD_COMMON = ('c850_ui',)

BPM_SITE = dict(
    sig=[0xD62E, 0x6260, 0x2228, 0x8902, 0xD62A, 0x6061, 0x8154],
    start=0, end=7,
)


MODS = {
    'browse_played': Mod(
        what='Measure playback time for rekordbox USB tracks',
        target='main', fw_versions=('1.13',), args=[], literal=False,
        hook=True, routine='payload:browse_playback_tick',
        **BPM_SITE,
    ),
    # Browse reply loop at 0x0C02CAF4. Its call at 0x0C02CB04 gets
    # the type in r5 and a pointer to the cache entry's UTF-16 title in r7.
    'browse_rows': Mod(
        what='Prefix played track titles in USB browse replies',
        target='main', fw_versions=('1.13',), args=[], literal=False,
        hook=True, routine='payload:browse_mark_title',
        sig=[0x85E6, 0x67E3, 0x7714, 0x6493, 0x6603, 0x85E2, 0x3ECC, 0x7D01,
             0x4B0B, 0x6503, 0x3DA3, 0x8BF3, 0x65F2, 0x6493, 0xD21B, 0x7F0C,
             0x48F4, 0x422B, 0x0009],
        start=0, end=8,
    ),
    # The successful export.pdb open calls the sort-table loader here.
    'browse_mount': PoolRedirect(
        what='Reset played markers for each mounted rekordbox export',
        target='main', fw_versions=('1.13',),
        anchor=Anchor(sig=[0xD233, 0x420B, 0x64A3, 0xE501, 0x52F2, 0x2228],
                      index=0, literal=True),
        old=0x0C0750D8, routine='payload:browse_mount_export',
    ),
    'browse_unmount': PoolRedirect(
        what='Drop played markers when the USB database closes',
        target='main', fw_versions=('1.13',),
        anchor=Anchor(sig=[0x64D3, 0xD667, 0x460B, 0x6583, 0x60B3, 0x07AE],
                      index=1, literal=True),
        old=0x0C04F2B0, routine='payload:browse_unmount_export',
    ),
    'browse_sort_cache': PoolRedirect(
        what='Key the USB menu cache by sort order',
        target='main', fw_versions=('1.13',),
        anchor=Anchor(sig=[0x55D5, 0x7714, 0xD210, 0x420B, 0x6603, 0x4011, 0x8F02],
                      index=2, literal=True),
        old=0x0C0BBABE, routine='payload:browse_track_cache',
    ),
    'browse_sort_results': PoolRedirect(
        what='Order USB track lists by BPM or key after the stock finalizer',
        target='main', fw_versions=('1.13',),
        anchor=Anchor(sig=[0x6CD3, 0x3C6C, 0xD75E, 0x470B, 0x64C3, 0x60F0, 0x8801],
                      index=2, literal=True),
        old=0x0C0BABAC, routine='payload:browse_sort_results',
    ),
    'browse_sort_menu': PoolRedirect(
        what='Cycle the track sort order on a short MENU press',
        target='main', fw_versions=('1.13',),
        anchor=Anchor(sig=[0x4F16, 0x4BF0, 0xD33E, 0x434B, 0xD63E, 0x6060],
                      index=2, literal=True),
        old=0x0C0EB388, routine='payload:c850_ui_tick',
    ),
    'browse_sort_display': Mod(
        what='Show the selected track sort order',
        target='main', fw_versions=('1.13',), args=[], literal=False,
        hook=True, routine='payload:c850_ui_flash_hook',
        after=('browse_played', 'bpm_lock_display', 'bpm_decimals', 'phase_meter'),
        **BPM_SITE,
    ),
}
