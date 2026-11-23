# SPDX-License-Identifier: GPL-2.0-or-later
"""CDJ-350 MAIN v1.14 patch and startup layout."""
from c850loader import Startup
from sigpatch import ImageProfile

# The panel power-on check clears the pending byte and latches 1 (USB) or
# 2 (CD) into the update-mode byte when the corresponding keys are held.
MAIN_PROFILE = ImageProfile(
    flash_base=0x0C000000, loadtab_off=0, loadtab_n=0,
    ram_base=None, ram_end=None, byteorder='>',
    gate=(0x0C1CAF3D, 0x0C1CAF4A),
)
LOADER_STARTUP = Startup(
    clear=0x0C0AE3F4, bounds=0x0C139864,
    statics=0x0C1CC678, statics_end=0x0DCC2D34, blob_addr=0x0C1CC700,
)

# These USB audio pool words fall in the static boot-critical closure.
# The validator requires the stock branches of their service-mode gates.
GATED_BOOT_POOLS = frozenset((0x0C055BA4, 0x0C055634, 0x0C055644, 0x0C0B32D0))
