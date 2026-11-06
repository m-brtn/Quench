#!/usr/bin/env python3
"""Writes the sounds of the engine demo's Audio section as 16-bit WAV files: procedural, CC0 (see
examples/demo/assets/CREDITS.md). Standard library only. The demo's build runs it (examples/demo/CMakeLists.txt) and
quench_cook checks and copies the files into the demo's assets; nothing it writes is committed.

    python3 tools/assets/make_demo_sounds.py <output directory>

Every loop holds a whole number of cycles of each tone, and the music renders its notes around the loop (a note's
tail continues at the start), so the loops have no seam. The dominant tones are what the demo's scenario measures:
the fly-by's 440 Hz, the music tracks' drones at 110 Hz (A) and 146.83 Hz (B).
"""

import math
import os
import struct
import sys
import wave

TAU = 2.0 * math.pi


def write_wav(path: str, rate: int, channels, peak: float = 0.9) -> None:
    """channels: one list of float samples per channel, normalized to `peak`."""
    frames = len(channels[0])
    largest = max(max(abs(s) for s in channel) for channel in channels) or 1.0
    scale = peak / largest * 32767.0
    data = bytearray()
    for i in range(frames):
        for channel in channels:
            data += struct.pack("<h", max(-32767, min(32767, int(round(channel[i] * scale)))))
    with wave.open(path, "wb") as out:
        out.setnchannels(len(channels))
        out.setsampwidth(2)
        out.setframerate(rate)
        out.writeframes(bytes(data))


def motor(rate: int = 24000, seconds: float = 1.0):
    """The orbiting emitter: a 150 Hz buzz (harmonics 1..8 falling as 1/n) pulsing at 6 Hz."""
    n = int(rate * seconds)
    out = []
    for i in range(n):
        t = i / rate
        tone = sum(math.sin(TAU * 150.0 * k * t) / k for k in range(1, 9))
        out.append(tone * (0.75 + 0.25 * math.sin(TAU * 6.0 * t)))
    return out


def siren(rate: int = 24000, seconds: float = 1.0):
    """The fly-by emitter: 440 Hz with two quieter harmonics (880, 1320 Hz), so 440 Hz dominates 380..520 Hz."""
    n = int(rate * seconds)
    return [math.sin(TAU * 440.0 * i / rate) + 0.35 * math.sin(TAU * 880.0 * i / rate) +
            0.2 * math.sin(TAU * 1320.0 * i / rate) for i in range(n)]


def ping(rate: int = 48000, seconds: float = 0.5):
    """The UI sound: 1760 Hz and a fifth above, 2 ms attack, decaying in about 0.1 s."""
    n = int(rate * seconds)
    out = []
    for i in range(n):
        t = i / rate
        envelope = min(1.0, t / 0.002) * math.exp(-t / 0.08)
        out.append(envelope * (math.sin(TAU * 1760.0 * t) + 0.3 * math.sin(TAU * 2640.0 * t)))
    return out


def snap(frequency: float, seconds: float) -> float:
    """The nearest frequency with a whole number of cycles in the loop."""
    return round(frequency * seconds) / seconds


def music(drone: float, notes, step: float, rate: int = 22050, seconds: float = 6.0):
    """A stereo loop: a drone with a slow swell, and plucked notes (fundamental and octave, 0.3 s decay) placed
    alternately left and right, rendered around the loop."""
    n = int(rate * seconds)
    left = [0.0] * n
    right = [0.0] * n
    drone = snap(drone, seconds)
    for i in range(n):
        t = i / rate
        d = 0.55 * math.sin(TAU * drone * t) * (0.8 + 0.2 * math.sin(TAU * t / 2.0))
        left[i] += d
        right[i] += d
    tail = int(rate * 1.5)
    count = int(round(seconds / step))
    for k in range(count):
        frequency = notes[k % len(notes)]
        start = int(round(k * step * rate))
        gain_l, gain_r = (0.32, 0.12) if k % 2 == 0 else (0.12, 0.32)
        for j in range(tail):
            t = j / rate
            envelope = min(1.0, t / 0.004) * math.exp(-t / 0.3)
            s = envelope * (math.sin(TAU * frequency * t) + 0.25 * math.sin(TAU * 2.0 * frequency * t))
            index = (start + j) % n
            left[index] += gain_l * s
            right[index] += gain_r * s
    return [left, right]


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    write_wav(os.path.join(out, "motor.wav"), 24000, [motor()], 0.8)
    write_wav(os.path.join(out, "siren.wav"), 24000, [siren()], 0.8)
    write_wav(os.path.join(out, "ping.wav"), 48000, [ping()], 0.7)
    # A minor at 120 bpm in eighths; D major in triplets. Every note is above 200 Hz, the drones below.
    a_minor = [220.0, 261.63, 329.63, 440.0, 329.63, 261.63, 392.0, 329.63]
    d_major = [293.66, 369.99, 440.0, 587.33, 440.0, 369.99]
    write_wav(os.path.join(out, "music_a.wav"), 22050, music(110.0, a_minor, 0.25), 0.8)
    write_wav(os.path.join(out, "music_b.wav"), 22050, music(146.83, d_major, 1.0 / 3.0), 0.8)
    return 0


if __name__ == "__main__":
    sys.exit(main())
