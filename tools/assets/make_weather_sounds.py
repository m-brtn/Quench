#!/usr/bin/env python3
"""Writes the engine's default weather sounds (audio::WeatherAudio, docs/audio.md "Weather sounds") as 16-bit stereo
WAV loops: procedural, CC0. Standard library only. The CMake function quench_weather_sounds() runs it at build time
and quench_cook checks and copies the files into a game's assets as quench/weather/*.wav; nothing it writes is
committed.

    python3 tools/assets/make_weather_sounds.py <output directory>

Each loop is 4 s at 32 kHz. Its noise is filtered as a periodic signal (the filters run twice around the loop, the
second pass kept) and its drops are placed around the loop (a drop's tail continues at the start), so the loops have
no seam. The layers differ where the engine's checks look: the light rain's energy is high (fine drops, 2 to 7 kHz),
the heavy rain's broad and loud, the distant rain's low (below about 700 Hz), the roof's drops low and ringing.
"""

import math
import os
import random
import struct
import sys
import wave

RATE = 32000
SECONDS = 4.0
FRAMES = int(RATE * SECONDS)
TAU = 2.0 * math.pi


def lowpass(signal, cutoff):
    """One-pole low-pass of a periodic signal: two passes around the loop, the second kept."""
    a = math.exp(-TAU * cutoff / RATE)
    b = 1.0 - a
    y = 0.0
    for x in signal:
        y = b * x + a * y
    out = [0.0] * len(signal)
    for i, x in enumerate(signal):
        y = b * x + a * y
        out[i] = y
    return out


def highpass(signal, cutoff):
    low = lowpass(signal, cutoff)
    return [x - l for x, l in zip(signal, low)]


def noise(rng):
    return [rng.uniform(-1.0, 1.0) for _ in range(FRAMES)]


def add_drops(out, rng, per_second, low_hz, high_hz, decay_low, decay_high, loudness=1.0):
    """Short decaying sine bursts at random places around the loop; most are quiet, a few loud."""
    count = int(per_second * SECONDS)
    for _ in range(count):
        start = rng.randrange(FRAMES)
        frequency = math.exp(rng.uniform(math.log(low_hz), math.log(high_hz)))
        decay = rng.uniform(decay_low, decay_high)
        amplitude = loudness * rng.random() ** 3
        length = int(decay * 5.0 * RATE)
        w = TAU * frequency / RATE
        k = math.exp(-1.0 / (decay * RATE))
        attack = max(1, int(0.0003 * RATE))
        envelope = amplitude
        phase = rng.uniform(0.0, TAU)
        for j in range(length):
            ramp = j / attack if j < attack else 1.0
            out[(start + j) % FRAMES] += envelope * ramp * math.sin(phase + w * j)
            envelope *= k


def scaled(signal, gain):
    return [x * gain for x in signal]


def mixed(*signals):
    return [sum(values) for values in zip(*signals)]


def rain_light(rng):
    channels = []
    for _ in range(2):
        bed = scaled(lowpass(highpass(noise(rng), 1500.0), 9000.0), 0.12)
        add_drops(bed, rng, 450, 2000.0, 7000.0, 0.001, 0.003)
        channels.append(bed)
    return channels, 0.06


def rain_heavy(rng):
    channels = []
    for _ in range(2):
        hiss = scaled(lowpass(highpass(noise(rng), 200.0), 6000.0), 0.6)
        rumble = scaled(lowpass(lowpass(noise(rng), 300.0), 300.0), 1.5)
        bed = mixed(hiss, rumble)
        add_drops(bed, rng, 900, 1000.0, 5000.0, 0.002, 0.005, 0.8)
        channels.append(bed)
    return channels, 0.2


def rain_far(rng):
    channels = []
    for c in range(2):
        wash = highpass(lowpass(lowpass(noise(rng), 700.0), 700.0), 60.0)
        # One slow swell per loop (0.25 Hz), the channels a little apart.
        phase = 1.3 * c
        channels.append([x * (1.0 + 0.3 * math.sin(TAU * 0.25 * i / RATE + phase)) for i, x in enumerate(wash)])
    return channels, 0.1


def rain_cover(rng):
    channels = []
    for _ in range(2):
        bed = scaled(lowpass(noise(rng), 1500.0), 0.25)
        add_drops(bed, rng, 500, 250.0, 1500.0, 0.008, 0.025)
        add_drops(bed, rng, 50, 2000.0, 4000.0, 0.015, 0.03, 0.5)
        channels.append(bed)
    return channels, 0.12


def write_wav(path, channels, rms):
    """Scaled to the RMS given (both channels together), lowered where a peak would pass 0.98."""
    total = sum(x * x for channel in channels for x in channel)
    current = math.sqrt(total / (len(channels) * FRAMES)) or 1.0
    peak = max(abs(x) for channel in channels for x in channel) or 1.0
    gain = min(rms / current, 0.98 / peak)
    data = bytearray()
    for i in range(FRAMES):
        for channel in channels:
            data += struct.pack("<h", max(-32767, min(32767, int(round(channel[i] * gain * 32767.0)))))
    with wave.open(path, "wb") as out:
        out.setnchannels(len(channels))
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(bytes(data))


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    out_dir = sys.argv[1]
    os.makedirs(out_dir, exist_ok=True)
    sounds = {"rain_light.wav": rain_light, "rain_heavy.wav": rain_heavy, "rain_far.wav": rain_far,
              "rain_cover.wav": rain_cover}
    for seed, (name, make) in enumerate(sounds.items()):
        channels, rms = make(random.Random(1000 + seed))
        write_wav(os.path.join(out_dir, name), channels, rms)
    return 0


if __name__ == "__main__":
    sys.exit(main())
