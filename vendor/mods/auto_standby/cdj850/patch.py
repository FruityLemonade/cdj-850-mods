# SPDX-License-Identifier: GPL-2.0-or-later
"""auto_standby on the CDJ-850 MAIN 1.13: AUTO STANDBY timeout in UTILITY (20/40/60/120 min).

The idle countdown is FUN_0c0ef844 (0x0C0EF844). At entry it calls FUN_0C08F186()
through pool 0x0C0EF998 to read the stock AUTO STANDBY mode (0 = OFF, 1 = ON).
When ON and the idle counter (0x0C1E10AE s, 0x0C1E10B0 ms) reaches the limit
chosen in UTILITY (STANDBY TIME: 20/40/60/120 MIN, persisted in PIONEER/MODSET.DAT),
auto_standby_get_mode sets 0x0C1E10AE to 0x383F so the stock comparison against
table[1] (0x3840 = 4 hours at 0x0C18CCDE) triggers standby on that tick.

The stock idle gate FUN_0C0EF7D6 never counts on the USB source while a stick is
mounted (0x0CE6D147 = 1), so there auto_standby_get_mode keeps its own idle count
and makes the stock request (FUN_0C0EFB10(2), FUN_0C0EFABA) at the limit. The
stock key-activity reset FUN_0C0EFAEC reads the mode through its own pool word
0x0C0EFB84, redirected to auto_standby_activity, which also clears that count.
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
    'autostandby': PoolRedirect(
        what='AUTO STANDBY timeout: 20/40/60/120 MIN selectable in UTILITY (saved to USB)',
        target='main',
        fw_versions=('1.13',),
        anchor=Anchor(sig=[0x4EF0, 0xD254, 0x424B, 0xD554, 0x6E0C, 0x6050, 0x2008, 0x891F],
                      index=1, literal=True),
        old=0x0C08F186,
        routine='payload:auto_standby_get_mode',
    ),
    'autostandby_activity': PoolRedirect(
        what='AUTO STANDBY: key activity also restarts the USB (stick mounted) idle count',
        target='main',
        fw_versions=('1.13',),
        anchor=Anchor(sig=[0x4F22, 0xD125, 0x414B, 0x640C, 0x2448, 0x8909, 0xBE6D],
                      index=1, literal=True),
        old=0x0C08F186,
        routine='payload:auto_standby_activity',
    ),
    'autostandby_usb_settings': PoolRedirect(
        what='AUTO STANDBY: read the mod settings file on the stick at mount (shared c850_ui)',
        target='main',
        fw_versions=('1.13',),
        anchor=Anchor(sig=[0x1D01, 0xD575, 0xD775, 0x470B, 0x64D3, 0x2008, 0x8903],
                      index=2, literal=True),
        old=0x0C0C45B4,
        routine='payload:c850_ui_usb_mount',
    ),
    'autostandby_utility_scroll': PoolRedirect(
        what='AUTO STANDBY: UTILITY list continues after VERSION No. (shared c850_ui items)',
        target='main',
        fw_versions=('1.13',),
        anchor=Anchor(sig=[0x35F1, 0x4020, 0xD26D, 0x424B, 0xD56D, 0xB6CC, 0x6450],
                      index=2, literal=True),
        old=0x0C0E4DF6,
        routine='payload:c850_ui_utility_scroll',
    ),
    'autostandby_utility_select': PoolRedirect(
        what='AUTO STANDBY: cycle STANDBY TIME in UTILITY (shared c850_ui items)',
        target='main',
        fw_versions=('1.13',),
        anchor=Anchor(sig=[0x3551, 0x8000, 0xD22A, 0x420B, 0xE403, 0xA1CF, 0x0009],
                      index=2, literal=True),
        old=0x0C0E4C14,
        routine='payload:c850_ui_utility_select',
    ),
    'autostandby_flash': Mod(
        what='AUTO STANDBY: draw STANDBY TIME item in UTILITY',
        target='main',
        fw_versions=('1.13',),
        args=[],
        literal=False,
        hook=True,
        after=('bpm_decimals', 'phase_meter'),
        routine='payload:c850_ui_flash_hook',
        **BPM_SITE,
    ),
}
