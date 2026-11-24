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
OPTIONS = {
    "tempo_wide": ("USB WIDE tempo", "Enable ±100% tempo on USB. TEMPO cycles ±6% → ±10% → ±16% → WIDE, as on a CD."),
    "flac": ("USB FLAC playback", "Play stereo FLAC files from USB at 16 or 24 bits and 44.1 or 48 kHz."),
    "hot_cues": ("Hot cues", "Use BEAT 1, 2 and 4 for hot cues A–C, read from and saved to rekordbox. Hold BEAT 8 and press one to delete it. Toggle with BEAT SELECT + RELOOP. Unfinished: a jump takes a few seconds."),
    "quant_loops": ("Quantized loops", "Snap loops (and new hot cues) to the nearest beat of the rekordbox grid. Toggle with BEAT SELECT + LOOP IN."),
    "loop_resize": ("Loop resize", "While a loop plays, use CUE/LOOP CALL to halve or double its length."),
    "bpm999": ("BPM up to 999", "Raise the tempo ceiling for rekordbox grids and the panel display."),
    "bpm_decimals": ("BPM decimals", "Show the actual BPM to two decimal places in place of the rekordbox BPM in the INFO view, on grid tracks."),
    "bpm_lock": ("BPM lock", "Lock tracks to a Master BPM across ±100% range (auto-switches to WIDE beyond ±16%). Hold BEAT SELECT + MASTER TEMPO for 1 s to set Master BPM with the rotary selector; short-press to toggle lock on/off."),
    "phase_meter": ("Phase meter", "Show a 4-beat bar counter and sub-beat phase indicator on INFO Page 2 (shifting stock INFO pages down by one page). Works with BPM lock on or off."),
    "info_order": ("INFO screen order", "Show playing BPM, Camelot key and rekordbox rating beneath the USB track title (e.g. 128.23 12A *****). Turn the jog in INFO to see Artist, Rating and Key, then Album and Comment."),
    "waveform": ("Zoomed scrolling waveform", "Stream a live zoomed scrolling half-waveform in the 100×5 INFO Page 1 strip while keeping rows 1–2 intact (compatible with INFO screen order). Turn the rotary counterclockwise on INFO Page 1 to zoom out (close → medium → whole-track overview) and clockwise to zoom back in."),
    "auto_standby": ("20-minute auto standby", "Shorten the idle timeout to 20 minutes when AUTO STANDBY is enabled."),
}

rows = conf_rows()
MODS = {name: rows[name] for name in OPTIONS}


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


def patch_update(stock, selections, version, progress):
    if any(name not in MODS for name in selections):
        raise ValueError("Choose modifications from the available options.")
    if hashlib.sha256(stock).hexdigest() != modelfw.main_file(MODEL)[1]:
        raise ValueError("Choose the original CDJ-850 firmware version 1.13.")

    relabelled = repack.set_version(stock, version)
    if not selections:
        progress("Verifying the stock update…")
        if repack.check_upd(relabelled):
            raise ValueError("The stock update did not pass validation.")
        return relabelled

    progress("Applying selected modifications…")
    image = repack.unpack_main(stock)
    names = list(dict.fromkeys(patch for key in selections for patch in MODS[key]))
    names = patch_main.with_loader(names, MODEL)
    report, spans = {}, {}
    patched = patch_main.patch(image, names, MODEL, report, spans)
    if any(report.get(name, (None,))[0] != "applied" for name in names):
        raise ValueError("A selected modification could not be applied.")
    if patched == image:
        raise ValueError("The selected modifications produced no changes.")

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
