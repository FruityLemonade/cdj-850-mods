# SPDX-License-Identifier: GPL-2.0-or-later
"""bpm_decimals on the CDJ-850 MAIN 1.13: the actual BPM with two decimals in
the INFO view of a loaded track, in place of the rekordbox BPM.

The INFO item "128.0 bpm" (DB tempo) reads "132.25" while a beat
grid gave the BPM: the grid BPMx100 at the current beat times the pitch.
Without a grid (BPM counter, CD) the row is exactly stock. Code is C in
payload/bpm_decimals.c (shared SDRAM payload, mods/payloadbuild.py).

* bpm_decimals: hook (payload routine bpm_decimals_hook) on the tail of
  display_stage_bpm, span 0x0C0642AA..0x0C0642B8, the span bpm999, bpm_probe
  and payload_demo hook too (shared trampoline, no new cave bytes).
  display_stage_bpm is outside the boot-critical closure. DisplayTask runs
  FUN_0C0614EA (INFO row filler FUN_0C06543E, on scroll/track events),
  then FUN_0C062F80 (display_stage_bpm), then FUN_0C064CA4 (serialises the
  rows; the text sequence only changes when a byte did). The routine
  rewrites the BPM row record in between, only when its text changes.
* bpm_decimals_grid: pool redirect of 0x0C073830, the word FUN_0C073788 (the
  only caller of grid_find_beat, 0x0C0012E4) loads at 0x0C0737B8; FUN_0C073788
  is outside the closure and nothing else reads the word. The payload calls
  grid_find_beat and notes the BPMx100 of the beat stock looked up: the proof
  that the base BPM word came from a grid.
"""
from sigpatch import Anchor, Mod, PoolRedirect

PAYLOAD_COMMON = ('c850_ui',)

# Tail of display_stage_bpm, as in mods/bpm999/cdj850/patch.py.
BPM_SITE = dict(
    sig=[0xD62E, 0x6260, 0x2228, 0x8902, 0xD62A, 0x6061, 0x8154],
    start=0,
    end=7,
)

MODS = {
    'bpm_decimals': Mod(
        what='BPM decimals: actual BPM with two decimals in place of the DB BPM in the INFO view (grid tracks)',
        target='main',
        fw_versions=('1.13',),
        args=[],
        literal=False,
        hook=True,
        after=('info_order',),
        routine='payload:bpm_decimals_hook',
        **BPM_SITE,
    ),
    # FUN_0C073788 at 0x0C0737AC: mov.l @(pool),r4 (magic 1/75); mov #0,r6; dmulu.l;
    # mov.l @(pool),r4 (grid ctx); sts mach,r5; shlr2 r5; mov.l @(pool),r7 (grid_find_beat,
    # index 6); jsr @r7; shlr r5; cmp/eq #-1,r0.
    'bpm_decimals_grid': PoolRedirect(
        what='BPM decimals: note the grid BPMx100 of the stock tempo lookup (grid_find_beat)',
        target='main',
        fw_versions=('1.13',),
        anchor=Anchor(sig=[0xD41F, 0xE600, 0x34E5, 0xD41C, 0x050A, 0x4509, 0xD71D, 0x470B,
                           0x4501, 0x88FF],
                      index=6, literal=True),
        old=0x0C0012E4,
        routine='payload:bpm_decimals_find_beat',
    ),
}
