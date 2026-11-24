#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Replace vendor/ with the CDJ-850 tools and mods from an emulator checkout.

    python3 sync_vendor.py path/to/cdj-850-emulator

Copies launcher/, models/, mods/ and scripts/firmware/ (documentation and
tests left out), records the emulator commit in vendor/UPSTREAM, and reports
CDJ-850 mods the patcher does not offer yet. Prints "changed" or "unchanged";
an emulator commit that leaves the copied files as they are keeps the old
vendor/UPSTREAM and counts as unchanged.
"""
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
VENDOR = ROOT / "vendor"
TREES = ("launcher", "models", "mods", "scripts/firmware")
SKIP_NAMES = {"Makefile", "__pycache__", ".DS_Store"}
SKIP_SUFFIXES = {".md", ".pyc"}
# Mod folders that are support code or developer tools, never offered on the site.
NOT_OFFERED = {"loader", "payload", "players", "bpm_probe", "dsp_probe", "payload_demo"}


def wanted(path):
    if path.name in SKIP_NAMES or path.suffix in SKIP_SUFFIXES:
        return False
    return not path.name.startswith("test")


def snapshot(base):
    if not base.exists():
        return {}
    return {p.relative_to(base).as_posix(): p.read_bytes()
            for p in sorted(base.rglob("*")) if p.is_file()}


def mod_models(conf):
    for line in conf.read_text(encoding="utf-8").splitlines():
        if line.startswith("MOD_MODELS="):
            return line.split("=", 1)[1].strip().strip('"').replace(",", " ").split()
    return []


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    upstream = Path(sys.argv[1]).resolve()
    for tree in TREES:
        if not (upstream / tree).is_dir():
            sys.exit(f"{upstream} has no {tree}/; is it a cdj-850-emulator checkout?")
    commit = subprocess.run(["git", "-C", str(upstream), "rev-parse", "HEAD"],
                            check=True, capture_output=True, text=True).stdout.strip()

    before = snapshot(VENDOR)
    recorded = before.pop("UPSTREAM", None)
    shutil.rmtree(VENDOR, ignore_errors=True)
    for tree in TREES:
        for source in sorted((upstream / tree).rglob("*")):
            relative = source.relative_to(upstream)
            if source.is_file() and all(wanted(Path(part)) for part in relative.parts):
                target = VENDOR / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, target)
    changed = before != snapshot(VENDOR)
    if changed or recorded is None:
        (VENDOR / "UPSTREAM").write_text(commit + "\n", encoding="utf-8")
    else:
        (VENDOR / "UPSTREAM").write_bytes(recorded)

    sys.dont_write_bytecode = True
    sys.path[:0] = [str(ROOT), str(VENDOR), str(VENDOR / "mods"), str(VENDOR / "scripts/firmware")]
    import patcher  # noqa: E402  (vendor/ must be in place first)
    offered = set(patcher.MODS) | NOT_OFFERED
    for conf in sorted((VENDOR / "mods").glob("*/mod.conf")):
        name = conf.parent.name
        if patcher.MODEL in mod_models(conf) and name not in offered:
            print(f"::warning title=New CDJ-850 mod::{name} is in the emulator but not offered "
                  f"by the patcher. Add it to BUILDS in patcher.py (or NOT_OFFERED in sync_vendor.py).")

    print("changed" if changed or recorded is None else "unchanged")


if __name__ == "__main__":
    main()
