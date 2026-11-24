#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compile the two mod payloads and prepare a static site. No firmware input is needed."""
import hashlib
import json
import os
import shutil
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

ROOT = Path(__file__).resolve().parent
VENDOR = ROOT / "vendor"
DIST = ROOT / "dist"

sys.path[:0] = [str(VENDOR / base) for base in ("", "mods", "scripts/firmware")]
import patcher  # noqa: E402
import payloadbuild  # noqa: E402
from launcher.model import load  # noqa: E402


def package_engine(path, sources):
    payload_mods = set(payloadbuild.payload_mods(patcher.MODEL))
    groups = sorted({
        tuple(sorted(set(spec["mods"]) & payload_mods))
        for spec in patcher.BUILDS.values()
        if spec["mods"]
    })
    with ZipFile(path, "w", compression=ZIP_DEFLATED) as archive:
        for source, name in sources:
            archive.writestr(ZipInfo(name), source.read_bytes(), compress_type=ZIP_DEFLATED)
        with ProcessPoolExecutor(max_workers=2) as pool:
            for group, payload in zip(groups, pool.map(payloadbuild.build, groups)):
                if tuple(payload.mods) != group:
                    raise ValueError("Wrong payload returned for " + ", ".join(group))
                key = "payloads/" + "+".join(group)
                archive.writestr(ZipInfo(key + ".bin"), payload.blob, compress_type=ZIP_DEFLATED)
                archive.writestr(ZipInfo(key + ".json"), json.dumps(payload.symbols, sort_keys=True),
                                 compress_type=ZIP_DEFLATED)
    print(f"Packaged {len(groups)} payload builds", flush=True)


def main():
    web = ROOT / "web"
    sources = [(ROOT / "patcher.py", "patcher.py")]
    mod_dirs = patcher.MODS.keys() | {"loader", "payload", "players"}
    for path in sorted(VENDOR.rglob("*")):
        if not path.is_file() or path.suffix not in {".py", ".conf", ".bin", ".s", ".S", ".c", ".h", ".ld"}:
            continue
        name = path.relative_to(VENDOR)
        if name.parts[0] == "mods" and len(name.parts) > 2 and name.parts[1] not in mod_dirs:
            continue
        sources.append((path, name.as_posix()))
    model = load(patcher.MODEL)
    config = {
        "stockVersion": model.fw_version,
        "stockHash": model.upd_sha256[0],
        "counterUrl": os.environ.get("PATCH_COUNTER_URL", "").strip(),
        "builds": [
            {
                "id": build_id,
                "title": spec["title"],
                "badge": spec["badge"],
                "description": spec["description"],
                "alwaysOn": spec["alwaysOn"],
                "toggleable": spec["toggleable"],
            }
            for build_id, spec in patcher.BUILDS.items()
        ],
    }
    stock_module = f"export default {json.dumps(config)};\n".encode("utf-8")
    DIST.mkdir(exist_ok=True)
    bundle = DIST / "patcher.zip"
    package_engine(bundle, sources)
    assets = sorted(path for path in web.iterdir() if path.name != "index.html")
    fingerprint = hashlib.sha256(bundle.read_bytes())
    fingerprint.update(stock_module)
    for asset in assets:
        fingerprint.update(asset.read_bytes())
    key = fingerprint.hexdigest()[:12]
    output = DIST / "assets" / key
    output.mkdir(parents=True, exist_ok=True)
    bundle.replace(output / bundle.name)
    for asset in assets:
        shutil.copyfile(asset, output / asset.name)
    (output / "stock.mjs").write_bytes(stock_module)
    index = (web / "index.html").read_text(encoding="utf-8")
    (DIST / "index.html").write_text(index.replace("__ASSET_PATH__", f"./assets/{key}"), encoding="utf-8")
    shutil.copyfile(ROOT / "LICENSE", DIST / "LICENSE")
    (DIST / ".nojekyll").touch()
    print(f"Built assets/{key}", flush=True)


if __name__ == "__main__":
    main()
