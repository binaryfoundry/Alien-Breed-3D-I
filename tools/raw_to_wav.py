#!/usr/bin/env python3
"""
Convert raw 8-bit signed PCM files in data/sounds to .wav (8-bit unsigned, mono).
Amiga LoadFromDisk.s loads these as raw; Paula plays 8-bit signed.
Output: data/sounds/<name>.wav so load_one_sample can load them.
Run from repo root:  python tools/raw_to_wav.py
Sample rate matches Amiga: PAL CPU 7093789 Hz / period 443 = 16013 Hz (AB3DI.s).
"""
from pathlib import Path
import struct
import sys

# Amiga AB3DI.s sets AUDxPER = 443 for all SFX channels; PAL clock 7093789 Hz
SAMPLE_RATE = 7093789 // 443  # 16013 Hz
SOUNDS_DIR = Path(__file__).resolve().parent.parent / "data" / "sounds"


def write_wav_header(f, num_samples: int) -> None:
    """Write minimal 44-byte WAV header (RIFF + fmt + data)."""
    data_size = num_samples
    riff_size = 36 + data_size
    # RIFF header
    f.write(b"RIFF")
    f.write(struct.pack("<I", riff_size))
    f.write(b"WAVE")
    # fmt chunk: PCM 8-bit mono
    f.write(b"fmt ")
    f.write(struct.pack("<I", 16))  # chunk size
    f.write(struct.pack("<H", 1))   # PCM
    f.write(struct.pack("<H", 1))   # mono
    f.write(struct.pack("<I", SAMPLE_RATE))
    f.write(struct.pack("<I", SAMPLE_RATE))  # byte rate
    f.write(struct.pack("<H", 1))   # block align
    f.write(struct.pack("<H", 8))   # bits per sample
    # data chunk
    f.write(b"data")
    f.write(struct.pack("<I", data_size))


def main() -> int:
    if not SOUNDS_DIR.is_dir():
        print(f"[raw_to_wav] No directory: {SOUNDS_DIR}", file=sys.stderr)
        return 0  # not fatal

    converted = 0
    for p in sorted(SOUNDS_DIR.iterdir()):
        if not p.is_file():
            continue
        if p.suffix.lower() == ".wav":
            continue
        if p.name.startswith(".") or p.name.lower() == "readme.md":
            continue

        raw = p.read_bytes()
        if len(raw) == 0:
            continue

        # Amiga 8-bit signed (file) -> WAV 8-bit unsigned: u = (b + 128) % 256
        samples = bytearray((b + 128) & 0xFF for b in raw)

        out = p.parent / (p.name.lower() + ".wav")
        with open(out, "wb") as f:
            write_wav_header(f, len(samples))
            f.write(samples)
        converted += 1
        print(f"  {p.name} -> {out.name}")

    if converted:
        print(f"[raw_to_wav] Converted {converted} file(s) to .wav in {SOUNDS_DIR}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
