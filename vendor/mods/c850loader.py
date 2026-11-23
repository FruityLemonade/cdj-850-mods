#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The CDJ-850 loader stub: copy a blob of custom code and data into free SDRAM
at boot, with one changed word in MAIN's entry.

    usage: c850loader.py install <main.bin> <out.bin> [payload.bin]
           c850loader.py check   <stock-main.bin> <main.bin>
           c850loader.py ram     <dump.bin> [<dump base address>]

`main.bin` is the unpacked MAIN image as c850_repack.py leaves it (v1.13,
0x38FFE0 bytes). `ram` checks a dump of SDRAM taken with the emulator's
`pmemsave` (by default of the reserved region alone, starting at 0x0DE00000):
both guard words, the header and the payload checksum.

How it works (docs/cdj850-modding.md, section 8.4):

  MAIN's entry at 0x0C000000 calls three routines through literals:

      0C000000  d303  mov.l  @(0x0C000010),r3   ; 0x0C0B92AC clear statics
      0C000002  434b  jsr    @r3
      0C000004  d303  mov.l  @(0x0C000014),r3   ; delay slot: next callee
      0C000006  434b  jsr    @r3                ; 0x0C0B8FF4
      0C000008  d203  mov.l  @(0x0C000018),r2   ; 0x0C0B951C kernel start
      0C00000A  424b  jsr    @r2
      0C00000C  affe  bra    $
      0C00000E  0009  nop

  The first callee zeroes [0x0C14F6F4]..[0x0C14F6F8], i.e. 0x0C1E27E0 (end of
  .data) to 0x0DDEFA20. The packed image decodes to 0x0C38FFE0, so the stretch
  0x0C1E27E0..0x0C38FFE0 is part of the image (zeros in stock, covered by the
  bootloader's 16-bit sum) but is wiped before any task runs.

  install writes, at BLOB_ADDR in that stretch, the stub and behind it the
  blob (a 32-byte header and the payload), and points the literal at
  0x0C000010 at the stub. At boot the stub copies the blob to 0x0DE00000,
  writes the top guard word at 0x0DF6FFFC and jumps (not calls) to the
  original 0x0C0B92AC, which returns to 0x0C000006 as before. The clear then
  zeroes the stub and the blob's source copy along with everything else.

  The stub uses only r0-r2 and r4-r6. r3 must survive: the jsr's delay slot
  has already loaded it with the second callee, and 0x0C0B92AC leaves it
  alone too. PR is not touched, so the clear returns to the entry. The stub
  writes through the uncached SDRAM mirror (0x2Dxxxxxx, which the firmware
  itself uses for the CD cache above), so the copy is in SDRAM whatever the
  cache mode, and no instruction cache line can hold the region yet: nothing
  has ever run there.

  The stub sits in the statics stretch rather than in the debug-shell cave
  the hook mods share: it runs once, before the clear, and never again, so
  it costs no cave bytes.

The reserved region 0x0DE00000..0x0DF70000 (1.44 MB) is in nobody's map: it
lies above the kernel heap and the bootloader's dead stage 2, and below the
CD sector cache at 0x0DF79F50 (see docs/cdj850-modding.md, section 8.4).
Its first word (the header's GUARD_LO) and last word (GUARD_HI) are guards:
`ram` reports if either ever changes.

Standard library only.
"""
import struct
import sys
from dataclasses import dataclass

BASE = 0x0C000000                # MAIN's load and execution address

# The entry and the routine whose literal is rewritten (v1.13).
ENTRY = bytes.fromhex('d303434bd303434bd203424baffe0009')
ENTRY_LITERAL = 0x10             # offset of the first callee's literal
CLEAR = 0x0C0B92AC               # the start-up clear the stub chains to
CLEAR_CODE = bytes.fromhex('d1056212d405a002e600606342ab654232528bfa006b')
CLEAR_BOUNDS = 0x0C14F6F4        # [start, end] of the cleared statics
STATICS = 0x0C1E27E0             # what CLEAR_BOUNDS holds in v1.13
STATICS_END = 0x0DDEFA20
IMAGE_LEN = 0x38FFE0             # c850_repack.STOCK_IMAGE_LEN

BLOB_ADDR = 0x0C1E2800           # the stub, 0x100-aligned past STATICS
STUB_SIZE = 0x40                 # stub + pool, padded; the blob follows
REGION = 0x0DE00000              # where the blob is copied to
REGION_END = 0x0DF70000
UNCACHED = 0x20000000            # SDRAM's uncached mirror
GUARD_HI_ADDR = REGION_END - 4

GUARD_LO = 0x4C44523C            # b'LDR<', the header's first word
GUARD_HI = 0x3E4C4452            # b'>LDR', the region's last word
FORMAT = 1
HEADER = 0x20
PAYLOAD = REGION + HEADER        # where the payload runs from
MAX_PAYLOAD = GUARD_HI_ADDR - PAYLOAD

@dataclass(frozen=True)
class Startup:
    clear: int
    bounds: int
    statics: int
    statics_end: int
    blob_addr: int
    image_len: int = IMAGE_LEN


CDJ850_STARTUP = Startup(CLEAR, CLEAR_BOUNDS, STATICS, STATICS_END, BLOB_ADDR)


# The stub, hand-assembled from mods/loader/cdj850/main_loader.s. The pool
# (SRC, DST, WORDS, GUARD_HI_ADDR | UNCACHED, GUARD_HI, CLEAR) follows at
# +0x1C. mov.l @(disp,pc) reads (pc & ~3) + 4 + disp * 4.
STUB_CODE = (
    0xD106,   # +00 mov.l  @(+1C),r1      r1 = blob source in the image
    0xD207,   # +02 mov.l  @(+20),r2      r2 = REGION, uncached
    0xD407,   # +04 mov.l  @(+24),r4      r4 = words to copy (>= 8)
    0x6016,   # +06 loop: mov.l @r1+,r0
    0x2202,   # +08 mov.l  r0,@r2
    0x4410,   # +0A dt     r4
    0x8FFB,   # +0C bf/s   loop
    0x7204,   # +0E add    #4,r2          (delay slot)
    0xD505,   # +10 mov.l  @(+28),r5      r5 = GUARD_HI_ADDR, uncached
    0xD606,   # +12 mov.l  @(+2C),r6      r6 = GUARD_HI
    0x2562,   # +14 mov.l  r6,@r5
    0xD006,   # +16 mov.l  @(+30),r0      r0 = CLEAR
    0x402B,   # +18 jmp    @r0            returns to the entry through PR
    0x0009,   # +1A nop
)
POOL = 0x1C


class LoaderError(Exception):
    pass


def u32(data, off):
    return struct.unpack_from('>I', data, off)[0]


def sum32(data):
    data = data + b'\0' * (-len(data) % 4)
    return sum(struct.unpack('>%dI' % (len(data) // 4), data)) & 0xFFFFFFFF


def blob(payload):
    """The bytes the stub copies to REGION: header, then payload padded to a word."""
    if len(payload) > MAX_PAYLOAD:
        raise LoaderError('payload of %d bytes does not fit the region (%d)' % (len(payload), MAX_PAYLOAD))
    header = struct.pack('>IHHIIII8x', GUARD_LO, FORMAT, HEADER, len(payload), sum32(payload),
                         REGION_END, GUARD_HI_ADDR)
    return header + payload + b'\0' * (-len(payload) % 4)


def stub(n_bytes, *, startup=CDJ850_STARTUP):
    """The stub for a blob of n_bytes (a multiple of 4), padded to STUB_SIZE."""
    code = struct.pack('>%dH' % len(STUB_CODE), *STUB_CODE)
    pool = struct.pack('>6I', startup.blob_addr + STUB_SIZE, REGION | UNCACHED, n_bytes // 4,
                       GUARD_HI_ADDR | UNCACHED, GUARD_HI, startup.clear)
    out = code + pool
    return out + b'\0' * (STUB_SIZE - len(out))


def check_stock(image, *, startup=CDJ850_STARTUP):
    """Problems that keep `image` from being the stock MAIN the stub was written against."""
    if len(image) < startup.image_len:
        return ['image is %#x bytes, not %#x' % (len(image), startup.image_len)]
    problems = []
    if image[:ENTRY_LITERAL] != ENTRY:
        problems.append('the entry at %#x is not the one the stub was written for' % BASE)
    if image[startup.clear - BASE:startup.clear - BASE + len(CLEAR_CODE)] != CLEAR_CODE:
        problems.append('the start-up clear at %#x is not the one the stub was written for' % startup.clear)
    bounds = struct.unpack_from('>2I', image, startup.bounds - BASE)
    if bounds != (startup.statics, startup.statics_end):
        problems.append('the cleared statics are %#x-%#x, not %#x-%#x' % (bounds + (startup.statics, startup.statics_end)))
    return problems


def install(image, payload=b'', *, startup=CDJ850_STARTUP):
    """(new image, [(offset, size, kind)]) with the loader and `payload` in
    `image`. kind is 'loader' for the entry literal and 'loaderblob' for the
    stub and blob. An image that already carries a loader has it replaced."""
    image = uninstall(bytes(image), startup=startup)
    problems = check_stock(image, startup=startup)
    if not problems and u32(image, ENTRY_LITERAL) != startup.clear:
        problems.append('the entry literal at %#x is %#x, not %#x'
                        % (BASE + ENTRY_LITERAL, u32(image, ENTRY_LITERAL), startup.clear))
    if problems:
        raise LoaderError('; '.join(problems))
    packet = blob(payload)
    code = stub(len(packet), startup=startup) + packet
    lo = startup.blob_addr - BASE
    if lo + len(code) > startup.image_len:
        raise LoaderError('stub and blob (%d bytes) run past the image end %#x' % (len(code), BASE + startup.image_len))
    if any(image[lo:lo + len(code)]):
        raise LoaderError('%#x-%#x is not zero in this image' % (startup.blob_addr, startup.blob_addr + len(code)))
    out = bytearray(image)
    out[ENTRY_LITERAL:ENTRY_LITERAL + 4] = struct.pack('>I', startup.blob_addr)
    out[lo:lo + len(code)] = code
    return bytes(out), [(ENTRY_LITERAL, 4, 'loader'), (lo, len(code), 'loaderblob')]


def installed(image, *, startup=CDJ850_STARTUP):
    """The payload of the loader `image` carries, or None when its entry is
    stock. Raises LoaderError when the entry was changed but what it reaches is
    not exactly a stub and blob this module writes."""
    if len(image) < startup.image_len or image[:ENTRY_LITERAL] != ENTRY:
        return None
    lit = u32(image, ENTRY_LITERAL)
    if lit == startup.clear:
        return None
    if lit != startup.blob_addr:
        raise LoaderError('the entry literal points at %#x, neither the clear nor the stub' % lit)
    lo = startup.blob_addr - BASE
    n_bytes = u32(image, lo + POOL + 8) * 4
    if not HEADER <= n_bytes <= HEADER + MAX_PAYLOAD + 3:
        raise LoaderError('the stub copies %d bytes' % n_bytes)
    if image[lo:lo + STUB_SIZE] != stub(n_bytes, startup=startup):
        raise LoaderError('the code at %#x is not the loader stub' % startup.blob_addr)
    data = image[lo + STUB_SIZE:lo + STUB_SIZE + n_bytes]
    problems = check_blob(data)
    if problems:
        raise LoaderError('; '.join(problems))
    payload = data[HEADER:HEADER + u32(data, 8)]
    if data != blob(payload):
        raise LoaderError('the stub copies %d bytes, the header describes %d' % (len(data), len(blob(payload))))
    return payload


def uninstall(image, *, startup=CDJ850_STARTUP):
    """The image without its loader: the stock literal back, the stretch zeroed."""
    payload = installed(image, startup=startup)
    if payload is None:
        return bytes(image)
    out = bytearray(image)
    out[ENTRY_LITERAL:ENTRY_LITERAL + 4] = struct.pack('>I', startup.clear)
    lo = startup.blob_addr - BASE
    n = STUB_SIZE + len(blob(payload))
    out[lo:lo + n] = bytes(n)
    return bytes(out)


def check_blob(data):
    """Problems with a blob as the stub copies it (header + payload)."""
    if len(data) < HEADER:
        return ['blob of %d bytes is shorter than its header' % len(data)]
    magic, fmt, hlen, length, total, end, guard = struct.unpack_from('>IHHIIII', data, 0)
    problems = []
    if magic != GUARD_LO:
        problems.append('low guard is %#010x, not %#010x' % (magic, GUARD_LO))
    if (fmt, hlen, end, guard) != (FORMAT, HEADER, REGION_END, GUARD_HI_ADDR):
        problems.append('header fields %r are not format %d' % ((fmt, hlen, end, guard), FORMAT))
        return problems
    if HEADER + length > len(data):
        problems.append('payload length %d runs past the %d bytes there' % (length, len(data) - HEADER))
    elif sum32(data[HEADER:HEADER + length]) != total:
        problems.append('payload sum is %#010x, header says %#010x' % (sum32(data[HEADER:HEADER + length]), total))
    return problems


def verify(stock, out, *, startup=CDJ850_STARTUP):
    """Problems with the loader in `out`, judged against the stock image: its
    only writes may be the entry literal, and the stub and blob over zeros of
    the cleared statics inside the image. [] when `out` has no loader."""
    try:
        payload = installed(out, startup=startup)
    except LoaderError as e:
        return [str(e)]
    if payload is None:
        return []
    problems = check_stock(stock, startup=startup)
    if not problems and u32(stock, ENTRY_LITERAL) != startup.clear:
        problems.append('the stock entry literal is not %#x' % startup.clear)
    if problems:
        return problems
    lo, n = startup.blob_addr - BASE, STUB_SIZE + len(blob(payload))
    if not (startup.statics <= startup.blob_addr and startup.blob_addr + n <= BASE + startup.image_len):
        problems.append('stub and blob are not inside the cleared statics of the image')
    elif any(stock[lo:lo + n]):
        problems.append('the stock image is not zero at %#x-%#x' % (startup.blob_addr, startup.blob_addr + n))
    return problems


def spans(stock, out, *, startup=CDJ850_STARTUP):
    """[(offset, size, kind)] of the loader's writes in `out`, as install()
    reports them, or [] when `out` carries no loader verify() accepts."""
    try:
        payload = installed(out, startup=startup)
    except LoaderError:
        return []
    if payload is None or verify(stock, out, startup=startup):
        return []
    return [(ENTRY_LITERAL, 4, 'loader'), (startup.blob_addr - BASE, STUB_SIZE + len(blob(payload)), 'loaderblob')]


def check_ram(dump, base=REGION):
    """Problems with the reserved region in an SDRAM dump starting at `base`."""
    def word(addr):
        off = addr - base
        if off < 0 or off + 4 > len(dump):
            raise LoaderError('the dump (%#x-%#x) does not hold %#x' % (base, base + len(dump), addr))
        return u32(dump, off)
    problems = []
    if word(GUARD_HI_ADDR) != GUARD_HI:
        problems.append('high guard at %#x is %#010x, not %#010x' % (GUARD_HI_ADDR, word(GUARD_HI_ADDR), GUARD_HI))
    if word(REGION) != GUARD_LO:
        problems.append('low guard at %#x is %#010x, not %#010x' % (REGION, word(REGION), GUARD_LO))
        return problems
    length = word(REGION + 8)
    if length > MAX_PAYLOAD:
        problems.append('header length %#x is out of range' % length)
        return problems
    off = REGION - base
    return problems + check_blob(dump[off:off + HEADER + length])


def _read(path):
    with open(path, 'rb') as f:
        return f.read()


def main(argv):
    if len(argv) in (3, 4) and argv[0] == 'install':
        payload = _read(argv[3]) if len(argv) > 3 else b''
        out, _ = install(_read(argv[1]), payload)
        with open(argv[2], 'wb') as f:
            f.write(out)
        print('c850loader: stub at %#x, %d-byte payload copied to %#x at boot'
              % (BLOB_ADDR, len(payload), PAYLOAD))
        return 0
    if len(argv) == 3 and argv[0] == 'check':
        out = _read(argv[2])
        problems = verify(_read(argv[1]), out)
        for p in problems:
            print('c850loader: FAIL %s' % p)
        if not problems:
            payload = installed(out)
            print('c850loader: %s' % ('no loader' if payload is None else 'loader OK, %d-byte payload' % len(payload)))
        return 1 if problems else 0
    if len(argv) in (2, 3) and argv[0] == 'ram':
        problems = check_ram(_read(argv[1]), int(argv[2], 0) if len(argv) == 3 else REGION)
        for p in problems:
            print('c850loader: FAIL %s' % p)
        if not problems:
            print('c850loader: guards and payload intact')
        return 1 if problems else 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
