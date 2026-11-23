#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Unpack, repack and check flat CDJ-850 and CDJ-350 updates.

    c850_repack.py unpack <in.UPD> <image.bin>
    c850_repack.py pack   [--version D.DD] <base.UPD> <image.bin> <out.UPD>
    c850_repack.py check  <file.UPD>

What the CDJ-850 firmware does with the file (docs/cdj850-modding.md,
"Firmware Container (C850MAIN.UPD), Flash Layout & Bootloader", has the addresses):

* The updater in MAIN (0x0C08EC44) reads C:/C850MAIN.UPD into a 0xFF-filled
  4 MB buffer and programs flash from it, 64 KB sector by sector, with a
  read-back compare. The service-mode USB update (USB SELECT + RELOOP at
  power-on) writes 0x060000-0x3EFFFF and skips a file whose version at
  0x60010 equals the running one (installs_over() below has the traced
  rule: a label starting below '1', like the mod label "0.13", is always
  written); a second mode the firmware also has writes from 0x000000,
  boot region included. No checksum, label or model check -- so `pack`
  takes everything below the packed MAIN from a base file and refuses a
  base whose boot region is not the shipped one. Relabel (`--version`,
  e.g. 0.13) or the deck will skip an update labelled like it runs.
* Size: the packed MAIN may run up to 0x3F0000 (MAIN_END), past the
  0x200000 the stock layout ends at. Nothing else in MAIN writes flash
  below 0x3F8000 (the settings/log sectors), and an update to a 3.5 MB
  file ending at 0x370E5F was verified in the emulator
  (docs/cdj850-modding.md, section 2).
* The bootloader's second stage (boot.bin 0x10000, run at 0x0DDEFA20) reads
  a BE32 length at flash 0x60020, adds up the bytes 0x60020..0x60024+len and
  compares the low 16 bits with the BE16 that follows the stream. On a match
  it LZSS-decodes exactly len stream bytes to 0x0C000000 and jumps there; on
  a mismatch it boots the recovery image at flash 0x20000 (same format),
  and hangs only if that is bad too.

The image is the decoded MAIN as the bootloader leaves it in SDRAM (the
`unpack` output, 0x38FFE0 bytes for v1.13). c850_unpack.py's longer
main_unpacked.bin (0x39231E) also decodes the trailer and pad after the
stream; those extra bytes are zeros the bootloader never writes and are
dropped here.

`pack` reproduces the shipped file byte for byte when given the shipped
image (lzss_encode.pack_tree is the reference encoder's own search).
Standard library only; nothing here writes outside the named output file.
"""
import hashlib
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lzss_decode import unpack as lzss_unpack   # noqa: E402
from lzss_encode import pack_tree               # noqa: E402

LABEL_OFF = 0x60000          # 32-byte label: b"CDJ-850 MAIN" ... b"1.13"
LABEL_SIZE = 0x20
PACKED_OFF = 0x60020         # BE32 stream length, then the stream
STREAM_OFF = PACKED_OFF + 4
BOOT_END = 0x60000           # boot + recovery + erased: never changed
PAD_LEN = 0x10000            # zeros after the checksum in the shipped file
MAIN_END = 0x3F0000          # where the packed MAIN (length, stream, sum)
                             # must end: the updater programs up to here, and
                             # the 64 KB above holds the settings/log sectors
                             # it never writes. The bootloader has no bound
                             # of its own (it decodes the BE32 length), and
                             # stock ends at 0x1FE70C. Ending here also keeps
                             # the file (+ PAD_LEN) inside the 4 MB buffer.
FLASH_WRITE_END = 0x3F0000   # the updater programs 63 x 64 KB from 0
RECOVERY_OFF = 0x20000       # bootloader's fallback image (same format)

STOCK_BOOT_SHA256 = (
    "eb4f9f9885d6157d7521b13a18b20f685706152fabfe3ef2c3f6c9a5cd51d58c")
# Both models use the same compressed layout. Pin each boot region and label
# independently because a model's updater can write its boot region as well.
FLAT_MODELS = {
    "cdj850": (b"CDJ-850 MAIN", STOCK_BOOT_SHA256),
    "cdj350": (b"CDJ-350 MAIN", "8934d314696e8f7b456eb3fe4e97ce1dd2c0c43c4f31e996914735da21b07cbd"),
}
STOCK_IMAGE_LEN = 0x38FFE0
STOCK_IMAGE_SHA256 = (
    "a048888cec0352136d7eefc3e3d1d44bde6cea52476e28a2f7636902b5d52a75")


def sum16(data):
    """The bootloader's checksum: low 16 bits of the plain byte sum."""
    return sum(data) & 0xFFFF


def read_packed(upd, off=PACKED_OFF):
    """(stream, stored checksum, computed checksum) of the packed image at
    `off` (0x60020 for MAIN, 0x20000 for the recovery image)."""
    if len(upd) < off + 4:
        raise ValueError("file too short for a length field at 0x%x" % off)
    (n,) = struct.unpack_from(">I", upd, off)
    end = off + 4 + n
    if end + 2 > len(upd):
        raise ValueError("stream length 0x%x at 0x%x runs past the file"
                         % (n, off))
    (stored,) = struct.unpack_from(">H", upd, end)
    return upd[off + 4:end], stored, sum16(upd[off:end])


def unpack_main(upd):
    """The SDRAM image the bootloader decodes from this file."""
    stream, _, _ = read_packed(upd)
    return lzss_unpack(stream, 0)


def normalise_image(image):
    """Drop the decoder-overrun zeros of c850_unpack.py's longer output."""
    if len(image) > STOCK_IMAGE_LEN and not any(image[STOCK_IMAGE_LEN:]):
        return image[:STOCK_IMAGE_LEN]
    return image


VERSION_OFF = LABEL_OFF + 16   # b"1.13" in a NUL-padded 16-byte field


def set_version(base, version):
    """base with the label's version replaced. The deck skips an update
    whose version matches the running one on the first character and the
    two after the dot (0x0C08EC44), so keep the shipped D.DD shape."""
    if not re.fullmatch(r"[0-9]\.[0-9]{2}", version):
        raise ValueError("version %r is not D.DD (like 1.13)" % version)
    field = version.encode("ascii").ljust(16, b"\0")
    return base[:VERSION_OFF] + field + base[VERSION_OFF + 16:]


def get_version(upd):
    """The label's version field as text ("1.13")."""
    return upd[VERSION_OFF:VERSION_OFF + 16].split(b"\0")[0].decode("latin1")


# What the service-mode USB update (mode 6) does with a file, traced in
# upd_flash_write 0x0C08EC44..0x0C08ECCA: it copies the file's label bytes
# 0x60010..0x60014 to 0x0C08EE78 and SKIPS flashing (straight to COMPLETE!)
# only when ALL of these hold:
#   file[0] >= '1'  (cmp/ge #0x31 on the zero-extended byte),
#   mode == 6,
#   memcmp(file + 2, running + 2, 2) == 0  (the two chars after the dot),
#   file[0] == running[0].
# The running version is 0x0CE6D242, sprintf("%s") of the label of the MAIN
# in flash (0x0C0B8D68, 16 bytes). The dot is never compared. So a file whose
# label starts below '1' ("0.13") is ALWAYS written, and a stock "1.13" file
# is written over any deck running a label that starts with another char.
MOD_MAJOR = "0"


def installs_over(file_label, running_label):
    """Whether the service USB update writes a file labelled `file_label` on a
    deck running `running_label` (False = skipped, COMPLETE! with nothing written)."""
    f = file_label.encode("latin1").ljust(4, b"\0")
    r = running_label.encode("latin1").ljust(4, b"\0")
    return f[0] < 0x31 or f[2:4] != r[2:4] or f[0] != r[0]


def mod_label(version):
    """The label a modded file of firmware `version` (D.DD) gets: the same
    with the first digit 0 ("1.13" -> "0.13"). The updater always writes such
    a file (first char below '1'), whatever the deck runs; the stock file
    ("1.13") is always written over it (first chars differ); the version
    screen shows "Ver 0.13", so a modded deck is told apart from a stock one."""
    if not re.fullmatch(r"[0-9]\.[0-9]{2}", version):
        raise ValueError("version %r is not D.DD (like 1.13)" % version)
    return MOD_MAJOR + version[1:]


def base_version(label):
    """The firmware version a label stands for: a mod label "0.DD" is a modded
    "1.DD" (every CDJ-850 release is 1.xx); anything else is itself."""
    if re.fullmatch(r"0\.[0-9]{2}", label):
        return "1" + label[1:]
    return label


def build_upd(base, image, check_boot=True, version=None, *, model="cdj850"):
    """A new update file: base's bytes below 0x60020 (boot, recovery,
    label), then length, LZSS stream, checksum and the 64 KB zero pad."""
    if len(base) < PACKED_OFF:
        raise ValueError("base file too short")
    label, boot_hash = FLAT_MODELS[model]
    if base[LABEL_OFF:LABEL_OFF + len(label)] != label:
        raise ValueError("base file is missing the %s label" % label.decode())
    if check_boot:
        got = hashlib.sha256(base[:0x40000]).hexdigest()
        if got != boot_hash:
            raise ValueError(
                "base boot region is not the shipped one (sha256 %s);"
                " the updater reflashes it, refusing" % got)
    if version is not None:
        base = set_version(base, version)
    image = normalise_image(bytes(image))
    stream = pack_tree(image)
    packed = struct.pack(">I", len(stream)) + stream
    end = PACKED_OFF + len(packed) + 2
    if end > MAIN_END:
        raise ValueError(
            "packed MAIN ends at 0x%x, past the MAIN region end 0x%x "
            "(%d bytes over)" % (end, MAIN_END, end - MAIN_END))
    out = (base[:PACKED_OFF] + packed + struct.pack(">H", sum16(packed))
           + bytes(PAD_LEN))
    problems = check_upd(out, image=image, check_boot=check_boot, model=model)
    if problems:
        raise ValueError("self-check failed: " + "; ".join(problems))
    return out


def check_upd(upd, image=None, check_boot=True, *, model="cdj850"):
    """Everything the bootloader and updater would object to, as a list of
    strings (empty = fine)."""
    problems = []
    if len(upd) > 0x400000:
        problems.append("file is %d bytes, over the updater's 4 MB buffer"
                        % len(upd))
    label, boot_hash = FLAT_MODELS[model]
    if upd[LABEL_OFF:LABEL_OFF + len(label)] != label:
        problems.append("no '%s' label at 0x60000" % label.decode())
    if check_boot:
        got = hashlib.sha256(upd[:0x40000]).hexdigest()
        if got != boot_hash:
            problems.append("boot region differs from the shipped one")
    for name, off in (("MAIN", PACKED_OFF), ("recovery", RECOVERY_OFF)):
        try:
            stream, stored, calc = read_packed(upd, off)
        except ValueError as e:
            problems.append("%s: %s" % (name, e))
            continue
        if stored != calc:
            problems.append("%s checksum 0x%04x stored, 0x%04x computed"
                            % (name, stored, calc))
        if name == "MAIN":
            end = off + 4 + len(stream) + 2
            if end > MAIN_END:
                problems.append("packed MAIN ends at 0x%x, past 0x%x"
                                % (end, MAIN_END))
            if image is not None and lzss_unpack(stream, 0) != image:
                problems.append("MAIN does not decode back to the image")
    return problems


def _read(path):
    with open(path, "rb") as f:
        return f.read()


def _write(path, data):
    with open(path, "wb") as f:
        f.write(data)


def main(argv):
    if len(argv) < 2 or argv[1] in ("-h", "--help"):
        print(__doc__)
        return 2
    cmd, args = argv[1], argv[2:]
    if cmd == "unpack" and len(args) == 2:
        upd = _read(args[0])
        image = unpack_main(upd)
        _write(args[1], image)
        print("unpacked 0x%x bytes, sha256 %s"
              % (len(image), hashlib.sha256(image).hexdigest()))
        return 0
    version = None
    if cmd == "pack" and len(args) >= 2 and args[0] == "--version":
        version, args = args[1], args[2:]
    if cmd == "pack" and len(args) == 3:
        base, image = _read(args[0]), _read(args[1])
        try:
            out = build_upd(base, image, version=version)
        except ValueError as e:
            print("c850_repack: %s" % e, file=sys.stderr)
            return 1
        _write(args[2], out)
        stream, stored, _ = read_packed(out)
        print("wrote %s: %d bytes, stream 0x%x, checksum 0x%04x, sha256 %s"
              % (args[2], len(out), len(stream), stored,
                 hashlib.sha256(out).hexdigest()))
        return 0
    if cmd == "check" and len(args) == 1:
        upd = _read(args[0])
        problems = check_upd(upd)
        stream, stored, calc = read_packed(upd)
        print("%s: %d bytes, version %r, stream 0x%x, checksum 0x%04x/0x%04x"
              % (args[0], len(upd),
                 upd[LABEL_OFF + 16:LABEL_OFF + 24].rstrip(b"\0").decode(
                     "ascii", "replace"),
                 len(stream), stored, calc))
        for p in problems:
            print("  PROBLEM: %s" % p)
        print("  OK" if not problems else "  NOT OK")
        return 0 if not problems else 1
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
