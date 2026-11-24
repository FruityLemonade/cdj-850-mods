# SPDX-License-Identifier: GPL-2.0-or-later
"""quant_loops on the CDJ-350 MAIN 1.14: loops snap to the rekordbox beat grid.

Only while a beat grid of the loaded USB track is in RAM (the grid gave the
deck's tempo); otherwise every routine passes the stock values through. Code is
C in payload/quant_loops.c (shared SDRAM payload, mods/payloadbuild.py); the
patches are eight pool-word redirects, no cave bytes.

* quant_loops: pool 0x0C0CE8B4 (0x0C006A92, next_sector) in auto_beat_loop_set's
  USB new-loop path (0x0C0CDE6E), read at 0x0C0CE696 (before the out
  calculation) and 0x0C0CE848 (post-send refresh). Pre-send: snaps IN_SECTOR
  to the nearest beat and computes the 4-beat grid out; post-send: keeps the
  snapped IN_SECTOR.
* quant_loops_auto_send: pool 0x0C0CEBCC (audio_send_cmd, 0x0C0C7850), read
  only inside auto_beat_loop_set: puts the grid out into the cmd 0x1e
  (resize 0) request (and snaps from CUE_FRAME in state 6).
* quant_loops_loop_in / _loop_in2 / _loop_in3: pools 0x0C0D1380, 0x0C0D2728,
  0x0C0D31B8 (0x0C006A92) in the LOOP IN paths: snap IN_SECTOR to the nearest
  beat.
* quant_loops_loop_out / _loop_out2: pools 0x0C0D1688 and 0x0C0D1984
  (audio_send_cmd), read only in the state-3 LOOP OUT paths: snap the out
  sector of cmd 1 / mode 3 to the nearest beat after the loop in.
* quant_loops_drift: pool 0x0C0C7F7C (keys_state4_looping, 0x0C0D28AC), read
  only at 0x0C0C7D92 in player_key_dispatch_by_state (state 4): runs per-pass
  drift correction (cmd 0x1e, resize 1), then the stock handler.
* quant_loops_utility_scroll / _select / _draw: pools 0x0C0C902C
  (utility_scroll 0x0C0D641A, read at 0x0C0C8EFE), 0x0C0C9670
  (utility_set_level 0x0C0D6374, read at 0x0C0C955A) and 0x0C0666A4
  (utility_draw_list 0x0C063C88, read at 0x0C0665E8): the shared UTILITY list
  of payload/common/c350_ui.c (PAYLOAD_COMMON). Items QUANTIZE and DRIFT FIX
  after Ver switch snapping and the drift correction ON/OFF, saved in the
  settings record (+0x17, c350_ui.h); both default on.
"""
from sigpatch import Anchor, PoolRedirect

NEXT_SECTOR = 0x0C006A92
AUDIO_SEND_CMD = 0x0C0C7850
STATE4_KEYS = 0x0C0D28AC

PAYLOAD_COMMON = ('c350_ui',)


def _redirect(what, sig, index, old, routine):
    return PoolRedirect(what=what, target='main', fw_versions=('1.14',),
                        anchor=Anchor(sig=sig, index=index, literal=True),
                        old=old, routine='payload:' + routine)


MODS = {
    # 0x0C0CE696: cmp/eq #6,r0; bt; mov.l @(0x0C0CE8B4),r12; jsr/n @r12
    'quant_loops': _redirect(
        'quant loops: auto beat loop in on the nearest grid beat',
        [0x8806, 0x890C, 0xDC87, 0x4C4B, 0xDE87, 0xDC87, 0x4C0B, 0x64E1], 2,
        NEXT_SECTOR, 'quant_loops_auto_in'),
    # 0x0C0CE82E: mov #0x1e,r4; mov.l @r10,r6; mov.l @(0x0C0CEBCC),r3; jsr @r3
    'quant_loops_auto_send': _redirect(
        'quant loops: auto beat loop out on the grid',
        [0xE41E, 0x66A2, 0xD3E7, 0x430B, 0x6753, 0x7F10, 0x34B1, 0x8000], 2,
        AUDIO_SEND_CMD, 'quant_loops_auto_send'),
    # 0x0C0D1270: mov.l @(0x0C0D1380),r9; jsr/n @r9 (state 3 LOOP IN)
    'quant_loops_loop_in': _redirect(
        'quant loops: LOOP IN on the nearest grid beat (state 3)',
        [0xD943, 0x2970, 0xD943, 0x494B, 0xDD43, 0x64D1, 0xDB43, 0x4B0B], 2,
        NEXT_SECTOR, 'quant_loops_loop_in'),
    # 0x0C0D260A: mov.l @(0x0C0D2728),r7; jsr/n @r7 (LOOP IN path 2)
    'quant_loops_loop_in2': _redirect(
        'quant loops: LOOP IN on the nearest grid beat (path 2)',
        [0xD226, 0x424B, 0xD747, 0x474B, 0xD147, 0xD247, 0x420B, 0x6411], 2,
        NEXT_SECTOR, 'quant_loops_loop_in'),
    # 0x0C0D30C2: mov.l @(0x0C0D31B8),r1; jsr/n @r1 (state 4 LOOP IN)
    'quant_loops_loop_in3': _redirect(
        'quant loops: LOOP IN on the nearest grid beat (state 4)',
        [0xD21D, 0x424B, 0xD13D, 0x414B, 0xDD3D, 0xD73D, 0x470B, 0x64D1], 2,
        NEXT_SECTOR, 'quant_loops_loop_in'),
    # 0x0C0D1628: mov #1,r4; mov #2,r5; mov.l @(0x0C0D1688),r3; jsr @r3 (LOOP OUT path 1)
    'quant_loops_loop_out': _redirect(
        'quant loops: LOOP OUT on the nearest grid beat after the loop in',
        [0x2F56, 0x8F07, 0xE401, 0xE502, 0xD317, 0x430B, 0xE703, 0xD335, 0x432B, 0x7F10], 4,
        AUDIO_SEND_CMD, 'quant_loops_loop_out'),
    # 0x0C0D1782: bf/s; mov #1,r4; mov.l @(0x0C0D1984),r3; jsr @r3 (LOOP OUT path 2)
    'quant_loops_loop_out2': _redirect(
        'quant loops: LOOP OUT on the nearest grid beat after the loop in (second path)',
        [0x6653, 0xE703, 0x8F06, 0xE401, 0xD380, 0x430B, 0xE502, 0xD37F, 0x432B, 0x7F10], 4,
        AUDIO_SEND_CMD, 'quant_loops_loop_out'),
    # 0x0C0D1A3C: bf/s; mov #1,r4; mov.l @(0x0C0D1BB8),r3; jsr @r3 (LOOP OUT path 3)
    'quant_loops_loop_out3': _redirect(
        'quant loops: LOOP OUT on the nearest grid beat after the loop in (third path)',
        [0x2F26, 0xE703, 0x8F05, 0xE401, 0xD35E, 0x430B, 0xE502, 0xA6CA, 0x7F10], 4,
        AUDIO_SEND_CMD, 'quant_loops_loop_out'),
    # 0x0C0C7D92: mov.l @(0x0C0C7F7C),r3; jmp @r3; lds.l @r15+,pr (state 4 -> keys_state4_looping)
    'quant_loops_drift': _redirect(
        'quant loops: loop drift correction (per-pass out on the grid length, state-4 tick)',
        [0x00A8, 0x0098, 0x00A8, 0x0098, 0xD37B, 0x432B, 0x4F26, 0xD37A,
         0x432B, 0x4F26, 0xD37A, 0x432B, 0x4F26], 10,
        STATE4_KEYS, 'quant_loops_drift_entry'),
    # 0x0C0C8EF6: mov.b @(0x18,r15),r4; mov.b @(0x1c,r15),r5; mov.l @(0x0C0C902C),r3; jsr/n @r3 (level 2 rotary)
    'quant_loops_utility_scroll': _redirect(
        'quant loops: UTILITY list continues after Ver (shared c350_ui items)',
        [0x8802, 0x8B07, 0x34F1, 0x4018, 0x35F1, 0x401C, 0xD34B, 0x434B], 6,
        0x0C0D641A, 'c350_ui_utility_scroll'),
    # 0x0C0C9554: mov.l @(cursor),r6; movu.b @r6,r5; mov.l @(0x0C0C9670),r3; jsr @r3; mov #3,r4 (list push)
    'quant_loops_utility_select': _redirect(
        'quant loops: QUANTIZE / DRIFT FIX ON/OFF in UTILITY (shared c350_ui items)',
        [0x8802, 0x8B07, 0xD645, 0x3561, 0x8000, 0xD345, 0x430B, 0xE403], 5,
        0x0C0D6374, 'c350_ui_utility_select'),
    # 0x0C0665E8: mov.l @(0x0C0666A4),r3; jmp @r3 (utility_display, level 2)
    'quant_loops_utility_draw': _redirect(
        'quant loops: UTILITY rows of the mod items (shared c350_ui items)',
        [0xD230, 0x3021, 0x8000, 0x8802, 0x8B02, 0xD32E, 0x432B, 0x4F26], 5,
        0x0C063C88, 'c350_ui_utility_draw'),
}
