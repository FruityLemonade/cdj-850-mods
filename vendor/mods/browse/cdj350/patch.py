# SPDX-License-Identifier: GPL-2.0-or-later
"""Played markers and TRACK sorting for the CDJ-350 USB browser on MAIN 1.14.

Code is C in payload/browse.c plus the shared payload/common/c350_ui.c (the
SORT item in UTILITY); every patch is a pool-word redirect. docs/cdj350.md,
"Browse".

* browse_played: pool 0x0C0AE44C (keys_tick_1ms 0x0C0C6EBC, read at 0x0C0AE422
  in the 1 ms tick): counts the loaded USB track's play time.
* browse_rows: pool 0x0C02CE98 (the reply item adder 0x0C02B900, read at
  0x0C02CDF2 for the browse reply loop): '*' before a title played > 20 s.
* browse_mount / browse_unmount: pools 0x0C053D94 (load sort menu 0x0C06CC04
  after export.pdb opens) and 0x0C0542E4 (DB unmount 0x0C04CA9C): reset and
  drop the played table.
* browse_sort: pool 0x0C06CB34 (the track query 0x0C073C68), browse_sort_cache:
  0x0C04F478 (menu cache 0x0C0B083C), browse_sort_fields: 0x0C07411C (cursor
  sort 0x0C1073C4), browse_sort_order: 0x0C0AFAB4 (the results' title
  comparator 0x0C0AF72E in finish_results): TRACK in BPM or key order.
* browse_utility_scroll / _select / _draw: the shared UTILITY list (as
  quant_loops'), item SORT DEFAULT / BPM / KEY, saved in the mod flags.
"""
from sigpatch import Anchor, PoolRedirect

PAYLOAD_COMMON = ('c350_ui',)


def _redirect(what, sig, index, old, routine):
    return PoolRedirect(what=what, target='main', fw_versions=('1.14',),
                        anchor=Anchor(sig=sig, index=index, literal=True),
                        old=old, routine='payload:' + routine)


MODS = {
    'browse_played': _redirect(
        'Measure playback time for rekordbox USB tracks',
        [0x440B, 0x2212, 0xD50A, 0x454B], 2, 0x0C0C6EBC, 'browse_tick'),
    'browse_rows': _redirect(
        'Prefix played track titles in USB browse replies',
        [0x6493, 0xED00, 0xDB29, 0x4B0B], 2, 0x0C02B900, 'browse_add_item'),
    'browse_mount': _redirect(
        'Reset played markers for each mounted rekordbox export',
        [0x420B, 0x66A3, 0xD239, 0x420B, 0x64A3, 0xE501], 2, 0x0C06CC04, 'browse_mount_export'),
    'browse_unmount': _redirect(
        'Drop played markers when the USB database closes',
        [0x65C3, 0x64D3, 0xD692, 0x460B, 0x6583], 2, 0x0C04CA9C, 'browse_unmount_export'),
    'browse_sort': _redirect(
        'Request a sortable cursor for the USB TRACK list',
        [0xE504, 0xE400, 0xD18E, 0x410B], 2, 0x0C073C68, 'browse_track_query'),
    'browse_sort_cache': _redirect(
        'Cache USB TRACK query results by sort order',
        [0x55D5, 0x7714, 0xD210, 0x420B], 2, 0x0C0B083C, 'browse_track_cache'),
    'browse_sort_fields': _redirect(
        'Order the track cursor by BPM or key',
        [0x80FC, 0x66F3, 0xDA49, 0x4A0B], 2, 0x0C1073C4, 'browse_sort_fields'),
    'browse_sort_order': _redirect(
        'Keep cursor order when finishing the sorted TRACK results',
        [0xA09D, 0x0009, 0xD508, 0x66C3], 2, 0x0C0AF72E, 'browse_result_compare'),
    # The UTILITY words of quant_loops (mods/quant_loops/cdj350/patch.py), same routines.
    'browse_utility_scroll': _redirect(
        'Browse: UTILITY list continues after Ver (shared c350_ui items)',
        [0x8802, 0x8B07, 0x34F1, 0x4018, 0x35F1, 0x401C, 0xD34B, 0x434B], 6,
        0x0C0D641A, 'c350_ui_utility_scroll'),
    'browse_utility_select': _redirect(
        'Browse: SORT DEFAULT / BPM / KEY in UTILITY (shared c350_ui items)',
        [0x8802, 0x8B07, 0xD645, 0x3561, 0x8000, 0xD345, 0x430B, 0xE403], 5,
        0x0C0D6374, 'c350_ui_utility_select'),
    'browse_utility_draw': _redirect(
        'Browse: UTILITY rows of the mod items (shared c350_ui items)',
        [0xD230, 0x3021, 0x8000, 0x8802, 0x8B02, 0xD32E, 0x432B, 0x4F26], 5,
        0x0C063C88, 'c350_ui_utility_draw'),
}
