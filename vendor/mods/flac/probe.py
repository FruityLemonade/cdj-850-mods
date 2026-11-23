#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the emulator with a disposable snapshot and record the panel trace."""
import argparse
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import time

SCRIPTS = {
    "folder": ("6000:usb:200,9000:enter:200,10500:rot+1,11500:enter:200,"
               "18000:disc:200"),
    # the same, loading the WAV (track 1) through the stock path
    "folder-wav": "6000:usb:200,9000:enter:200,11500:enter:200,18000:disc:200",
    "export": ("6000:usb:200,7600:rot+2,9000:enter:200,11500:enter:200,"
               "14500:next:200,17000:time:200,18300:call_next:200,20000:play:200,"
               "21400:beat_2:200,24000:reloop:200,24800:rev:200,27500:rev:200,"
               "28500:vinyl:200,29100:vinyl:200,29800:jog_touch:2000,29900:jog-9:25,"
               "30700:jog+7:25,31400:jog0,32900:master_tempo:200,34100:disc:200"),
}

CDJ350_SCRIPTS = {
    "folder": ("6000:usb:1500,10000:push:200,11500:rot+1,13000:push:200,"
               "20000:play:200,31000:disc:1500"),
    "folder-wav": ("6000:usb:1500,10000:push:200,13000:push:200,"
                   "20000:play:200,31000:disc:1500"),
    "export": ("6000:usb:1500,9500:rot+2,11000:push:200,12500:rot+1,"
               "14000:push:200,22000:play:200,26400:search_next:1500,35000:disc:1500"),
}

PCM_RINGS = {"cdj850": (0x0C1E8E5C, 0x562200), "cdj350": (0x0C1D7710, 0x562200)}


def execute(conn, command, arguments=None):
    request = {"execute": command}
    if arguments is not None:
        request["arguments"] = arguments
    conn.write((json.dumps(request) + "\n").encode())
    while True:
        message = json.loads(conn.readline())
        if "error" in message:
            raise RuntimeError(message["error"])
        if "return" in message:
            return message["return"]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", choices=PCM_RINGS, default="cdj850")
    parser.add_argument("--qemu", type=Path, required=True)
    parser.add_argument("--usb", type=Path, required=True)
    parser.add_argument("--flash", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--recipe", choices=SCRIPTS, default="export")
    parser.add_argument("--script")
    parser.add_argument("--trace")
    # the payload map of the flash's mods: python3 mods/payloadbuild.py <out>.bin <mods...>
    # writes <out>.map (an `nm -n` listing works too)
    parser.add_argument("--symbols", type=Path,
                        default=Path(__file__).parent / "build/payload.map")
    parser.add_argument("--with-info", action="store_true")
    parser.add_argument("--until", default="input release disc", help="finish on this panel log message")
    parser.add_argument("--plugin", type=Path, help="run the SH7263 cache plugin alongside the probe")
    # wall-clock seconds for the whole script; emulated time is fixed by -icount,
    # so a busy host only needs longer
    parser.add_argument("--timeout", type=float, default=120)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    overrides = ("CDJ850_DSP_BOOT_HLE", "CDJ850_PF16_HIGH", "CDJ850_DSP_PLAY_PF")
    if args.plugin:
        overrides += ("CDJ_NATIVE_COPY", "CDJ_PPOKE", "CDJ_PSTORE", "CDJ_RPOKE")
    for key in overrides:
        env.pop(key, None)
    capture = args.out / "bus.scif1"
    # QEMU will not overwrite an existing capture; a rerun into the same --out
    # then logged no time or waveform pages and failed verify_probe.
    capture.unlink(missing_ok=True)
    recipes = CDJ350_SCRIPTS if args.model == "cdj350" else SCRIPTS
    env.update({"CDJ850_PANEL_SCRIPT": args.script or recipes[args.recipe],
           "CDJ850_SCIF1_CAPTURE": str(capture)})
    log_path = args.out / "panel.log"
    report_path = args.out / "summary.txt"
    parent, child = socket.socketpair()
    with parent, log_path.open("wb") as log:
        parent.settimeout(10)
        command = [
            str(args.qemu), "-M", args.model, "-display", "none", "-monitor", "none",
            "-serial", "none", "-icount", "shift=2,sleep=off",
            "-drive", f"if=pflash,format=raw,file={args.flash},snapshot=on",
            "-drive", f"if=none,id=usbstk,format=raw,file={args.usb},snapshot=on",
            "-device", "usb-storage,drive=usbstk,port=1",
            "-chardev", f"socket,id=qmp,fd={child.fileno()}", "-mon", "chardev=qmp,mode=control",
        ]
        if args.plugin:
            command += ["-plugin", f"{args.plugin},out={args.out.resolve() / 'cache'},quiet=on"]
        if args.trace:
            command.extend(["-d", "in_asm,exec,nochain", "-dfilter", args.trace,
                            "-D", str(args.out / "cpu.log")])
        with child:
            process = subprocess.Popen(command, stderr=log, pass_fds=(child.fileno(),), env=env)
        with log_path.open("rb") as reader, parent.makefile("rwb", buffering=0) as conn:
            try:
                json.loads(conn.readline())
                execute(conn, "qmp_capabilities")
                deadline = time.monotonic() + args.timeout
                report = []
                finish = args.until.encode()
                done = False
                while time.monotonic() < deadline and process.poll() is None and not done:
                    for line in reader:
                        if any(marker in line for marker in (
                            b"display mode=", b"input ", b"display track=",
                            b"display bpm=", b"display master_tempo=", b"E-830", b"cdj350-panel: text",
                        )):
                            note = line.decode(errors="replace").strip()
                            if not report or report[-1] != note:
                                report.append(note)
                        if finish in line:
                            done = True
                    if not done:
                        time.sleep(0.15)
                regs = execute(conn, "human-monitor-command", {"command-line": "info registers"})
                (args.out / "registers.txt").write_text(regs)
                if args.with_info:
                    symbols = args.symbols
                    match = re.search(r"^([a-f0-9]+) +(?:\w )?_flac_diagnostics$", symbols.read_text(), re.M)
                    pcm_addr, pcm_size = PCM_RINGS[args.model]
                    for addr, size, name in [
                        (int(match[1], 16), 16, "diagnostics.bin"),
                        (pcm_addr, pcm_size, "pcm-ring.bin"),
                    ]:
                        destination = (args.out / name).resolve()
                        response = execute(conn, "human-monitor-command",
                            {"command-line": f'pmemsave {addr:#x} {size:#x} "{destination}"'})
                        if response:
                            raise RuntimeError(response)
                report_path.write_text("\n".join(report) + "\n")
                print(report_path.read_text(), end="")
                if not done:
                    raise RuntimeError("panel did not finish, see registers and log")
            finally:
                try:
                    execute(conn, "quit")
                    process.wait(timeout=5)
                except (OSError, ValueError, subprocess.TimeoutExpired):
                    process.kill()
                    process.wait()

if __name__ == "__main__":
    main()
