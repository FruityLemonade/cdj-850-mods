# SPDX-License-Identifier: GPL-2.0-or-later
"""Install the shared SDRAM payload before CDJ-350 MAIN clears statics."""
import c850loader
from players.cdj350 import LOADER_STARTUP
from sigpatch import ImagePatch, NotApplied


def apply(image, env):
    try:
        return c850loader.install(image, env.get('cdj850.payload_blob', b''), startup=LOADER_STARTUP)
    except c850loader.LoaderError as exc:
        raise NotApplied(str(exc)) from exc


MODS = {
    'loader': ImagePatch(
        what='loader stub: copy a custom blob to SDRAM 0x0DE00000 at boot',
        target='main', fw_versions=('1.14',), apply=apply,
    ),
}
