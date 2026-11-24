# SPDX-License-Identifier: GPL-2.0-or-later
"""Browser adapter for the emulator project's CDJ-850 tools."""
import hashlib
import json
from pathlib import Path

import c850_repack as repack
import modelfw
import patch_main
import payloadbuild
import validate
from patch_update import conf_rows

MODEL = "cdj850"

ALWAYS_ON = (
    "version_display",
    "bpm999",
    "flac",
    "tempo_wide",
    "loop_resize",
    "auto_standby",
)
TESTED_TOGGLEABLE = (
    "quant_loops",
    "bpm_decimals",
    "bpm_lock",
    "phase_meter",
    "info_order",
    "browse",
)
DEV_TOGGLEABLE = (
    "hot_cues",
    "waveform",
    "loop_roll",
)

ALWAYS_ON_LABELS = [
    "Version display (custom + base 1.13 in UTILITY)",
    "BPM up to 999",
    "USB FLAC playback (16/24-bit, 44.1/48 kHz)",
    "USB WIDE tempo (±100%)",
    "Loop resize (CUE/LOOP CALL </>)",
    "Auto standby (20 / 40 / 60 / 120 min in UTILITY)",
]
TESTED_TOGGLE_LABELS = [
    "Quantized loops & drift fix",
    "BPM decimals in INFO view",
    "BPM lock (±100%)",
    "Phase meter (INFO Page 2)",
    "INFO screen order",
    "Browse played markers & BPM/key sort",
]
DEV_TOGGLE_LABELS = [
    "Hot cues (BEAT 1/2/4)",
    "Zoomed scrolling waveform",
    "Loop roll",
]

BUILDS = {
    "stable": {
        "title": "Standard build",
        "badge": "Tested",
        "description": (
            "All emulator-tested modifications. Optional features can be enabled or "
            "disabled in the UTILITY menu and saved to PIONEER/MODSET.DAT on your USB stick."
        ),
        "alwaysOn": ALWAYS_ON_LABELS,
        "toggleable": TESTED_TOGGLE_LABELS,
        "mods": ALWAYS_ON + TESTED_TOGGLEABLE,
    },
    "experimental": {
        "title": "Experimental build",
        "badge": "In development",
        "description": (
            "Everything in Standard plus in-development features (Hot cues, "
            "Zoomed scrolling waveform, Loop roll). Toggle features in UTILITY (saved to USB)."
        ),
        "alwaysOn": ALWAYS_ON_LABELS,
        "toggleable": TESTED_TOGGLE_LABELS + DEV_TOGGLE_LABELS,
        "mods": ALWAYS_ON + TESTED_TOGGLEABLE + DEV_TOGGLEABLE,
    },
    "stock": {
        "title": "Restore stock",
        "badge": "Original",
        "description": (
            "Repack the unmodified Pioneer v1.13 firmware with a new version number "
            "to roll back over a modded build."
        ),
        "alwaysOn": [],
        "toggleable": [],
        "mods": (),
    },
}

rows = conf_rows()
MODS = {name: rows[name] for name in ALWAYS_ON + TESTED_TOGGLEABLE + DEV_TOGGLEABLE}


def prebuilt_payload(folders, model):
    """Load the exact payload combination that the site compiled at build time."""
    key = "+".join(sorted(folders))
    base = Path(__file__).with_name("payloads") / key
    try:
        blob = base.with_suffix(".bin").read_bytes()
        symbols = json.loads(base.with_suffix(".json").read_text(encoding="utf-8"))
    except OSError as error:
        raise payloadbuild.PayloadError("Missing prebuilt payload for " + key) from error
    return payloadbuild.Payload(blob, symbols, tuple(folders), "", {})


def patch_update(stock, build_id, version, progress):
    if build_id not in BUILDS:
        raise ValueError("Choose a valid firmware build option.")
    if hashlib.sha256(stock).hexdigest() != modelfw.main_file(MODEL)[1]:
        raise ValueError("Choose the original CDJ-850 firmware version 1.13.")

    selections = BUILDS[build_id]["mods"]
    relabelled = repack.set_version(stock, version)
    if not selections:
        progress("Verifying the stock update…")
        if repack.check_upd(relabelled):
            raise ValueError("The stock update did not pass validation.")
        return relabelled

    progress("Applying modifications…")
    image = repack.unpack_main(stock)
    names = list(dict.fromkeys(patch for key in selections for patch in MODS[key]))
    names = patch_main.with_loader(names, MODEL)
    report, spans = {}, {}
    patched = patch_main.patch(image, names, MODEL, report, spans)
    if any(report.get(name, (None,))[0] not in ("applied", "present") for name in names):
        raise ValueError("A modification could not be applied.")
    if patched == image:
        raise ValueError("The selected build produced no changes.")

    progress("Compressing the modified firmware…")
    output = repack.build_upd(stock, patched, version=version)
    progress("Verifying the repacked update…")
    profile = patch_main.PATCH_SETS[MODEL].profile
    checks = validate.verify(
        stock, output, MODEL, {"main"}, {"main": spans}, {"main": profile}
    )
    if checks.exit_code:
        print(checks.text())
        raise ValueError("The repacked firmware did not pass validation.")
    return output
