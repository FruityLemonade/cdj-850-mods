# SPDX-License-Identifier: GPL-2.0-or-later
"""Check FLAC audio and transport in the two synthetic QEMU probes."""
import argparse
from array import array
from pathlib import Path
import re
import struct
import sys
import wave

def elapsed(log, begin, end):
    points = re.findall(r"display time=(\d\d):(\d\d):(\d\d) remain=0 \((\d+) ms\)", log)
    return [int(m) * 4500 + int(s) * 75 + int(f)
            for m, s, f, when in points if begin <= int(when) <= end]

def diagnostics(directory):
    opened, pcm_bytes, seeks, failures = struct.unpack(
        ">4I", (directory / "diagnostics.bin").read_bytes())
    assert opened > 0 and pcm_bytes >= 44100 * 4, (opened, pcm_bytes)
    assert seeks > 0 and failures == 0, (seeks, failures)
    return seeks

def cdj350_times(capture, begin, end):
    """Remaining time in CD frames from SCIF1 indicator page 3."""
    digits = {segment: str(i) for i, segment in enumerate(
        (0x77, 0x12, 0x6B, 0x5B, 0x1E, 0x5D, 0x7D, 0x13, 0x7F, 0x5F))}
    with capture.open() as bus:
        for line in bus:
            fields = line.split()
            if len(fields) != 7 or fields[0] != "T":
                continue
            nanos, raw = fields[-1].split(":")
            if not begin <= int(nanos) // 1000000 <= end:
                continue
            page = bytes.fromhex(raw)
            if page[0] == 0x32 and all(value in digits for value in page[6:12]):
                time = "".join(digits[value] for value in page[6:12])
                yield int(time[:2]) * 4500 + int(time[2:4]) * 75 + int(time[4:])


def verify_cdj350_export(directory):
    seeks = diagnostics(directory)
    log = (directory / "panel.log").read_text().split("input press disc", 1)[0]
    assert re.search(r"\[\s+Candidate\s+\] \[\s+FLAC CHECK\s+\]", log) and "Not Loaded" not in log
    playing = list(cdj350_times(directory / "bus.scif1", 15500, 21000))
    assert playing and playing[0] - playing[-1] >= 75, "time readout did not advance"
    assert "input press search_next" in log, "search input was missed"
    during_search = list(cdj350_times(directory / "bus.scif1", 26400, 27400))
    assert any(a - b >= 75 for a, b in zip(during_search, during_search[1:])), "search did not seek"
    return seeks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", choices=("cdj850", "cdj350"), default="cdj850")
    parser.add_argument("wav", type=Path)
    parser.add_argument("folder", type=Path)
    parser.add_argument("export", type=Path)
    parser.add_argument("--wav-run", type=Path,
                        help="folder-wav probe of the same fixture: for other than 16-bit "
                             "44.1 kHz, the FLAC ring must hold the stock WAV ring's audio")
    args = parser.parse_args()

    with wave.open(str(args.wav), "rb") as source:
        rate, frames = source.getframerate(), source.getnframes()
        native = (rate, source.getsampwidth()) == (44100, 2)
        pcm = array("h")
        pcm.frombytes(source.readframes(frames) if native else b"")
    if sys.byteorder == "little":
        pcm.byteswap()
    diagnostics(args.folder)
    folder = (args.folder / "panel.log").read_text()
    if args.model == "cdj350":
        assert "[    Candidate.f ]" in folder
        playing = list(cdj350_times(args.folder / "bus.scif1", 14500, 19000))
        assert playing and playing[0] - playing[-1] >= 75, "time readout did not advance"
    else:
        assert "[Candidate.flac]" in folder and "display track=02" in folder
    assert "Not Loaded" not in folder.split("input press disc", 1)[0]
    ring = (args.folder / "pcm-ring.bin").read_bytes()
    if native:
        reference = pcm.tobytes()
        assert reference in ring + ring, "full reference PCM missing from the emulated ring"
        matched = f"entire {len(reference):,}-byte PCM match"
    else:
        # The ring holds what pcm_rate_copy made of the 16-bit samples, so compare
        # with the stock WAV path's ring. That ring is not one clean pass of the
        # track (stale and re-read sectors), so cut its audio into 64 KB chunks:
        # nearly the whole track's worth must be in the FLAC ring as well.
        assert args.wav_run, "a 48 kHz or 24-bit fixture needs --wav-run"
        wav_log = (args.wav_run / "panel.log").read_text()
        assert "Baseline.wav" in wav_log and "Not Loaded" not in wav_log.split("input press disc", 1)[0]
        stock = (args.wav_run / "pcm-ring.bin").read_bytes()
        flac_ring = ring + ring
        track = frames * 44100 // rate * 4
        if args.model == "cdj350":
            assert stock[:track] in flac_ring, "full WAV reference missing from the FLAC ring"
            matched = f"entire {track:,}-byte match with the stock WAV ring"
        else:
            chunk = 0x10000
            audio = [stock[i:i + chunk] for i in range(0, len(stock) - chunk + 1, chunk)]
            audio = [c for c in audio if c.count(0) != chunk]  # each 64 KB holds a beat attack
            found = sum(c in flac_ring for c in audio)
            assert found * chunk >= track * 9 // 10, (found, len(audio), track)
            matched = f"{found}/{len(audio)} 64 KB chunks of the stock WAV ring in the FLAC ring"

    if args.model == "cdj350":
        seeks = verify_cdj350_export(args.export)
        print(f"PASS: {matched}, folder and rekordbox playback, search seek ({seeks} decoder seeks)")
        return

    seeks = diagnostics(args.export)
    export = (args.export / "panel.log").read_text()
    assert "[Baseline] | [FLAC CHECK]" in export
    assert "[Candidate] | [FLAC CHECK]" in export
    assert "Not Loaded" not in export
    assert "display bpm=120" in export and "display playing wave=1 memory=2" in export
    assert 8 * 75 in elapsed(export, 18300, 20000), "8-second memory cue missed"
    loop = elapsed(export, 21700, 24000)
    assert any(first - second >= 40 for first, second in zip(loop, loop[1:])), "loop did not wrap"
    rev = elapsed(export, 25200, 27000)
    assert rev and rev[0] - rev[-1] >= 75, "reverse did not move the playhead back"
    assert "input jog -9" in export and "input jog +7" in export
    assert "display master_tempo=1" in export
    print(f"PASS: {matched}, WAV/FLAC load, cue, loop, "
          f"reverse, jog input and Master Tempo ({seeks} FLAC decoder seeks)")

if __name__ == "__main__":
    main()
