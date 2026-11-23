# SPDX-License-Identifier: GPL-2.0-or-later
"""Present native FLAC to the CDJ-350 v1.14's USB WAV playback path."""
from sigpatch import Anchor, EntryRedirect, PoolRedirect

CLASSIFIER = 0x0C03EB94
CLASSIFIER_PROLOGUE = bytes.fromhex("4cf07fdc6c5362526d43842b")

# (name, pool word, stock function, payload symbol, reader signature)
AUDIO_CALLS = (
    ("flac_header", 0x0C0832E4, 0x0C000BC4, "flac_wav_header_read",
     (0x7708, 0x55F9, 0xD257, 0x420B, 0x2F12, 0x2008)),
    ("flac_pcm", 0x0C055BA4, 0x0C0B2E46, "flac_wav_pcm_read",
     (0x6411, 0x67B3, 0xD2A2, 0x420B, 0x6683, 0x7F08)),
    # Both file-size calls in the USB open routine use the same pool word.
    ("flac_size", 0x0C055634, 0x0C0F8F68, "flac_audio_source_size",
     (0x2902, 0x6403, 0xD2C3, 0x420B, 0x65B3, 0x65B2)),
    ("flac_file_close", 0x0C055644, 0x0C0F8A1C, "flac_file_close",
     (0xA00B, 0x2562, 0xD274, 0x424B, 0x6E03, 0xE600)),
    ("flac_audio_open", 0x0C0B32D0, 0x0C0551F4, "flac_audio_open",
     (0x6651, 0x770C, 0xD280, 0x420B, 0x6511, 0x7F04)),
    ("flac_audio_open2", 0x0C0B37B4, 0x0C0551F4, "flac_audio_open",
     (0x6422, 0x7710, 0xDE69, 0x4E0B, 0x6511, 0x6403)),
)

PAYLOAD_STACK_LIMITS = {"decoder.c": 4608}

MODS = {
    "flac": EntryRedirect(
        what="FLAC: .flac files in the directory classifier",
        target="main", fw_versions=("1.14",),
        anchor=Anchor(sig=[0x656C, 0x6E73, 0x8709, 0x8F02, 0x6603, 0xA132],
                      index=-6, literal=False),
        old=CLASSIFIER_PROLOGUE, routine="payload:flac_file_classify",
        stock="payload:flac_stock_classify",
    ),
}
MODS.update({
    name: PoolRedirect(
        what="FLAC: " + name.removeprefix("flac_").replace("_", " "),
        target="main", fw_versions=("1.14",),
        anchor=Anchor(sig=list(sig), index=2, literal=True),
        old=stock, routine="payload:" + routine,
    )
    for name, _, stock, routine, sig in AUDIO_CALLS
})
