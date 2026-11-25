# SPDX-License-Identifier: GPL-2.0-or-later
"""What the CDJ-850's mods share: the shape of its MAIN and DSP images (v1.13)."""
from dsppatch import DspShiProfile
from sigpatch import Cave, ImageProfile

# Service-mode gate (docs/cdj850-modding.md, "Service mode gate"): no mod may
# act while either byte is nonzero.
#   0x0C1E109A  1 or 2 once panel_keys_to_internal_words (0x0C0ED00E) saw
#               RELOOP held at power-on (USB/CD firmware update); its only
#               store in MAIN, 0 in .data otherwise.
#   0x0C1E10AA  1 in .data until that function has looked at the power-on
#               keys (cleared at 0x0C0ECF58, never set again): the mode is
#               not known yet, so mods stay out until it is.
SERVICE_GATE = (0x0C1E109A, 0x0C1E10AA)

# MAIN runs in place from flash 0x0C000000 on the big-endian SH7263 (SH-2A).
# Erased flash in the image is sparse, so routines go into six unused debug
# shell handlers (1,118 bytes) that only the shell command table points to.
MAIN_PROFILE = ImageProfile(
    flash_base=0x0C000000, loadtab_off=0, loadtab_n=0,
    ram_base=None, ram_end=None, byteorder='>',
    cave=Cave(
        handlers=(b'show semaphore info', b'show event flag info',
                  b'show data queue info', b'show mail box info',
                  b'show mutex info'),
        end=b'show message buffer info',
        sha256='891c18aac15fe3179e6b6f72df866a24c9bdb443359cfdd63fe2465e0ee6bba8',
    ),
    gate=SERVICE_GATE,
)

# Pool words inside the static boot-critical closure that a mod redirects
# through the service-mode gate. The validator checks that each jumps to its
# stock callee whenever either SERVICE_GATE byte is nonzero.
#   0x0C063124  display_stage_jog_flags (0x0C064D10), read at 0x0C062FE2 in
#               FUN_0C062F80 (loop_roll_jog).
#   0x0C063128  display_stage_active_cue_column (0x0C0642CC), read at 0x0C062FE6
#               in FUN_0C062F80 (waveform_cue_column).
#   0x0C064DD8  panel_tx_waveform (0x0C0662AE), read at 0x0C064CEC in the
#               DisplayTask display-update routine (waveform_tx_waveform).
#   0x0C064DDC  panel_tx_saved_cues (0x0C0664A8), read at 0x0C064CF2 in the
#               DisplayTask display-update routine (waveform_center_marker).
#   0x0C0F54D8  FUN_0C008C8A (the ring read request), read only at 0x0C0F534C, the
#               USB cue prefill of the audio task (hot_cues_prefill).
GATED_BOOT_POOLS = frozenset((0x0C063124, 0x0C063128, 0x0C064DD8, 0x0C064DDC, 0x0C0F54D8))

# The DSP56371 program MAIN uploads over SHI as four contiguous big-endian
# segments (0x0C155B28..0x0C1593A1, 0x3879 bytes). Segments 1-3 are stage
# loaders that re-enter the DSP bootstrap ROM via JMP $FF0000; segment 4 holds
# the main application with unused zero vector space P:0080..P:00FE.
DSP_PROFILE = DspShiProfile(
    head=(0x000009, 0x000000, 0x300013, 0x04D03F),
    length=0x3879,
    seg_count=4,
    free=(0x0080, 0x00FE),
)
