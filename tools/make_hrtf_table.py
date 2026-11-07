#!/usr/bin/env python3
"""Writes src/audio/HrtfTable.inc, the head-related impulse responses of quench_audio's HRTF, from the SADIE
database in Resonance Audio's repository (third_party/SADIE_hrtf_database: Subject 002, diffuse-field compensated,
48 kHz, 24 bit, 256 taps; Apache-2.0, the licence text is in THIRD_PARTY_LICENSES).

Usage: tools/make_hrtf_table.py path/to/resonance-audio-<commit>.tar.gz [output]
(curl -L -o resonance-audio.tar.gz https://github.com/resonance-audio/resonance-audio/archive/<commit>.tar.gz)

The set is reduced to a grid: rings every 10 degrees of elevation from -70 to +80 with an azimuth every 15 degrees
(SADIE's convention: 0 in front, 90 to the left), and the two poles; 386 directions. For each direction and ear the
onset (the first sample at a tenth of the ear's peak) is cut off but for 3 samples and kept as a delay in samples
(less the smallest delay of the set), and the next 128 taps are kept with a half-Hann fade over the last 16. The set
is scaled so that its diffuse-field power (the mean of both ears' energy over the sphere, the directions weighted by
the solid angle they stand for) is 1, the power of the mixer's constant-power panning, and stored as 16-bit
integers with one scale.
"""

import hashlib
import io
import math
import sys
import tarfile
import wave
from pathlib import Path

COMMIT = "4556a46afd4ffae092aa281bfd072eb0279d3a29"
SHA256 = "f7f3fbb53dbcd4f14812a7e4c2825c37423f45487a4710a34ff3f922a5efec77"
FOLDER = f"resonance-audio-{COMMIT}/third_party/SADIE_hrtf_database/WAV/Subject_002/DFC/48K_24bit"
RATE = 48000
TAPS = 128
PRE = 3
FADE = 16
FIRST_ELEVATION = -70
ELEVATION_STEP = 10
RINGS = 16
AZIMUTH_STEP = 15
AZIMUTHS = 360 // AZIMUTH_STEP


def directions() -> list[tuple[int, int]]:
    grid = [(a * AZIMUTH_STEP, FIRST_ELEVATION + r * ELEVATION_STEP) for r in range(RINGS) for a in range(AZIMUTHS)]
    return grid + [(0, -90), (0, 90)]


def solid_angle(elevation: int) -> float:
    """The solid angle a grid direction stands for: its share of the band halfway to the next rings, or the cap."""
    top = FIRST_ELEVATION + (RINGS - 1) * ELEVATION_STEP
    if elevation == -90:
        return 2 * math.pi * (1 - math.sin(math.radians(-FIRST_ELEVATION + ELEVATION_STEP / 2)))
    if elevation == 90:
        return 2 * math.pi * (1 - math.sin(math.radians(top + ELEVATION_STEP / 2)))
    low = math.radians(elevation - ELEVATION_STEP / 2)
    high = math.radians(elevation + ELEVATION_STEP / 2)
    return 2 * math.pi * (math.sin(high) - math.sin(low)) / AZIMUTHS


def read_pair(archive: tarfile.TarFile, azimuth: int, elevation: int) -> list[list[float]]:
    member = archive.extractfile(f"{FOLDER}/azi_{azimuth}_ele_{elevation}_DFC.wav")
    with wave.open(io.BytesIO(member.read())) as w:
        if (w.getnchannels(), w.getsampwidth(), w.getframerate()) != (2, 3, RATE):
            raise SystemExit(f"azi {azimuth} ele {elevation}: not 2 channels of 24 bit at {RATE} Hz")
        raw = w.readframes(w.getnframes())
    values = [int.from_bytes(raw[i : i + 3], "little", signed=True) / 8388608.0 for i in range(0, len(raw), 3)]
    return [values[0::2], values[1::2]]


def cut(ir: list[float]) -> tuple[int, list[float], float]:
    """The delay, the kept taps and the share of the ear's energy they hold."""
    peak = max(abs(v) for v in ir)
    onset = next(i for i, v in enumerate(ir) if abs(v) >= 0.1 * peak)
    start = max(onset - PRE, 0)
    kept = ir[start : start + TAPS]
    kept += [0.0] * (TAPS - len(kept))
    for k in range(TAPS - FADE, TAPS):
        kept[k] *= 0.5 * (1.0 + math.cos(math.pi * (k - (TAPS - FADE) + 1) / FADE))
    share = sum(v * v for v in ir[start : start + TAPS]) / sum(v * v for v in ir)
    return start, kept, share


def main() -> None:
    if len(sys.argv) not in (2, 3):
        raise SystemExit(__doc__)
    archive_path = Path(sys.argv[1])
    output = Path(sys.argv[2]) if len(sys.argv) == 3 else Path(__file__).parent.parent / "src/audio/HrtfTable.inc"
    digest = hashlib.sha256(archive_path.read_bytes()).hexdigest()
    if digest != SHA256:
        raise SystemExit(f"{archive_path}: SHA256 {digest}, expected {SHA256} (Resonance Audio at {COMMIT})")

    grid = directions()
    delays: list[int] = []
    irs: list[list[float]] = []
    shares: list[float] = []
    with tarfile.open(archive_path, "r:gz") as archive:
        for azimuth, elevation in grid:
            for ear in read_pair(archive, azimuth, elevation):
                delay, kept, share = cut(ear)
                delays.append(delay)
                irs.append(kept)
                shares.append(share)
    first = min(delays)
    delays = [d - first for d in delays]

    weights = [solid_angle(elevation) for _, elevation in grid]
    power = sum(
        w * (sum(v * v for v in irs[2 * d]) + sum(v * v for v in irs[2 * d + 1])) for d, w in enumerate(weights)
    ) / sum(weights)
    gain = 1.0 / math.sqrt(power)
    largest = max(abs(v) for ir in irs for v in ir) * gain
    scale = largest / 32767.0
    quantized = [[round(v * gain / scale) for v in ir] for ir in irs]

    lines = [
        "// Generated by tools/make_hrtf_table.py from Resonance Audio's third_party/SADIE_hrtf_database",
        f"// ({COMMIT}, archive SHA256 {SHA256}):",
        "// SADIE Subject 002, diffuse-field compensated, 48 kHz. Apache-2.0; the licence text is in THIRD_PARTY_LICENSES.",
        "// Changed from the source: reduced to the grid below, the onsets cut off and kept as delays, truncated to 128",
        "// taps with a fade, scaled to a diffuse-field power of 1 and quantized to 16 bits.",
        "//",
        f"// {len(grid)} directions: {RINGS} rings from {FIRST_ELEVATION} degrees of elevation in steps of {ELEVATION_STEP},",
        f"// each with {AZIMUTHS} azimuths from 0 in steps of {AZIMUTH_STEP} (0 in front, 90 to the left), then the poles at -90",
        "// and +90. Per direction the left ear, then the right: kHrtfDelay in samples at kHrtfRate, kHrtfIr the taps",
        "// (times kHrtfScale).",
        "// clang-format off",
        f"inline constexpr std::uint32_t kHrtfRate = {RATE};",
        f"inline constexpr std::uint32_t kHrtfTaps = {TAPS};",
        f"inline constexpr int kHrtfFirstElevation = {FIRST_ELEVATION};",
        f"inline constexpr int kHrtfElevationStep = {ELEVATION_STEP};",
        f"inline constexpr int kHrtfRings = {RINGS};",
        f"inline constexpr int kHrtfAzimuths = {AZIMUTHS};",
        f"inline constexpr std::uint32_t kHrtfDirections = {len(grid)};",
        f"inline constexpr float kHrtfScale = {scale:.9e}f;",
        f"inline constexpr std::uint8_t kHrtfDelay[{len(delays)}] = {{",
    ]
    for d in range(0, len(delays), 32):
        lines.append(", ".join(str(v) for v in delays[d : d + 32]) + ",")
    lines.append("};")
    lines.append(f"inline constexpr std::int16_t kHrtfIr[{len(quantized) * TAPS}] = {{")
    for ir in quantized:
        for k in range(0, TAPS, 32):
            lines.append(",".join(str(v) for v in ir[k : k + 32]) + ",")
    lines.append("};")
    lines.append("// clang-format on")
    output.write_text("\n".join(lines) + "\n")

    shares.sort()
    print(
        f"{output}: {len(grid)} directions, {TAPS} taps, delays 0 to {max(delays)} samples (onsets from {first + PRE}),"
        f" gain {gain:.4f}, scale {scale:.3e}; energy kept: lowest {100 * shares[0]:.2f}%, median"
        f" {100 * shares[len(shares) // 2]:.2f}%"
    )


if __name__ == "__main__":
    main()
