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

# Every mod here has been tested on a CDJ-850. Standard holds the ones with no
# known issues; Experimental adds the ones that work but still have minor issues.
ALWAYS_ON = (
    "version_display",
    "bpm999",
    "flac",
    "tempo_wide",
    "loop_resize",
    "auto_standby",
)
SWITCHABLE = (
    "quant_loops",
    "bpm_decimals",
    "bpm_lock",
    "phase_meter",
    "info_order",
    "browse",
)
KNOWN_ISSUES = (
    "hot_cues",
    "waveform",
    "loop_roll",
)

ALWAYS_ON_LABELS = [
    "Version display (VERSION No. in UTILITY)",
    "BPM up to 999",
    "USB FLAC playback (16/24-bit, 44.1/48 kHz)",
    "USB WIDE tempo (±100%)",
    "Loop resize (CUE/LOOP CALL ◀ / ▶ in a loop)",
    "Auto standby (20 / 40 / 60 / 120 min, set in UTILITY)",
]
SWITCHABLE_LABELS = [
    "Quantized loops (UTILITY: QUANTIZE)",
    "BPM decimals in INFO (UTILITY: BPM DECIMALS)",
    "BPM lock, ±100% (UTILITY: BPM LOCK)",
    "Phase meter on INFO page 2 (UTILITY: PHASE METER)",
    "INFO screen order (UTILITY: INFO ORDER)",
    "Browse played markers & BPM/key sort (UTILITY: BROWSE MODS)",
]
KNOWN_ISSUES_LABELS = [
    "Hot cues on BEAT 1/2/4 (BEAT SELECT + RELOOP)",
    "Zoomed scrolling waveform (UTILITY: WAVEFORM)",
    "Loop roll, a slip loop (BEAT SELECT + LOOP OUT)",
]

BUILDS = {
    "stable": {
        "title": "Standard build",
        "badge": "Hardware tested",
        "description": (
            "Every mod that has been tested on a CDJ-850 with no known issues. "
            "Optional mods are switched on the deck."
        ),
        "alwaysOn": ALWAYS_ON_LABELS,
        "toggleable": SWITCHABLE_LABELS,
        "mods": ALWAYS_ON + SWITCHABLE,
    },
    "experimental": {
        "title": "Experimental build",
        "badge": "Minor issues",
        "description": (
            "Standard plus Hot cues, the zoomed scrolling waveform and Loop roll. "
            "These are hardware tested too, but still have minor known issues."
        ),
        "alwaysOn": ALWAYS_ON_LABELS,
        "toggleable": SWITCHABLE_LABELS + KNOWN_ISSUES_LABELS,
        "mods": ALWAYS_ON + SWITCHABLE + KNOWN_ISSUES,
    },
    "stock": {
        "title": "Restore stock",
        "badge": "Original",
        "description": (
            "The unmodified Pioneer v1.13 firmware with a new version number, "
            "to roll back over a modded build."
        ),
        "alwaysOn": [],
        "toggleable": [],
        "mods": (),
    },
}

rows = conf_rows()
MODS = {name: rows[name] for name in ALWAYS_ON + SWITCHABLE + KNOWN_ISSUES}


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
