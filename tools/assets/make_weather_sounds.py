#!/usr/bin/env python3
"""Writes the engine's default weather sounds (audio::WeatherAudio, docs/audio.md "Weather sounds") as 16-bit stereo
WAV files: loops of rain and wind and one-shots of thunder, procedural, CC0. Standard library only. The CMake
function quench_weather_sounds() runs it at build time and quench_cook checks and copies the files into a game's assets
as quench/weather/*.wav; nothing it writes is committed.

    python3 tools/assets/make_weather_sounds.py <output directory>

Each loop is 4 s at 32 kHz. Its noise is filtered as a periodic signal (the filters run twice around the loop, the
second pass kept), its drops are placed around the loop (a drop's tail continues at the start) and its swells are
whole cycles in it, so the loops have no seam. The layers differ where the engine's checks look: the light rain's
energy is high (fine drops, 2 to 7 kHz), the heavy rain's broad and loud, the distant rain's low (below about
700 Hz), the roof's drops low and ringing; the light wind is a low rush that swells, the strong wind louder with a
howl that rises and falls. The thunder is 6 s (near: a crack, then rolling rumble) and 8 s (far: a low rumble only),
fading to silence at its end. Their levels leave room for each other: heavy rain, a strong wind and near thunder
together (a windy storm) peak below full scale.
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


def lowpass(signal, cutoff, periodic=True):
    """One-pole low-pass; of a periodic signal two passes around the loop, the second kept."""
    a = math.exp(-TAU * cutoff / RATE)
    b = 1.0 - a
    y = 0.0
    if periodic:
        for x in signal:
            y = b * x + a * y
    out = [0.0] * len(signal)
    for i, x in enumerate(signal):
        y = b * x + a * y
        out[i] = y
    return out


def highpass(signal, cutoff, periodic=True):
    low = lowpass(signal, cutoff, periodic)
    return [x - l for x, l in zip(signal, low)]


def bandpass(signal, centre, q):
    """A resonant band-pass (two poles, 0 dB at its centre) of a periodic signal whose centre (Hz) is a function of the
    time (s), periodic in the loop: two passes around the loop, the second kept."""
    x1 = x2 = y1 = y2 = 0.0
    out = [0.0] * len(signal)
    for keep in (False, True):
        for i, x in enumerate(signal):
            w0 = TAU * centre(i / RATE) / RATE
            alpha = math.sin(w0) / (2.0 * q)
            a0 = 1.0 + alpha
            y = (alpha * x - alpha * x2 + 2.0 * math.cos(w0) * y1 - (1.0 - alpha) * y2) / a0
            x2, x1 = x1, x
            y2, y1 = y1, y
            if keep:
                out[i] = y
    return out


def noise(rng, frames=FRAMES):
    return [rng.uniform(-1.0, 1.0) for _ in range(frames)]


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
    return channels, 0.13


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


def swells(i, phase, *terms):
    """1 plus slow sines (Hz, depth), whole cycles in the loop."""
    t = i / RATE
    return 1.0 + sum(depth * math.sin(TAU * hz * t + phase * (k + 1)) for k, (hz, depth) in enumerate(terms))


def wind_light(rng):
    channels = []
    for c in range(2):
        rush = highpass(lowpass(lowpass(noise(rng), 600.0), 600.0), 90.0)
        rustle = scaled(lowpass(highpass(noise(rng), 2500.0), 6000.0), 0.04)
        bed = mixed(rush, rustle)
        channels.append([x * swells(i, 0.9 * c, (0.5, 0.3), (0.75, 0.12)) for i, x in enumerate(bed)])
    return channels, 0.05


def wind_strong(rng):
    channels = []
    for c in range(2):
        roar = highpass(lowpass(lowpass(noise(rng), 1200.0), 1200.0), 80.0)
        # The howl: a narrow band whose pitch rises and falls once around the loop, and its fifth above.
        source = noise(rng)
        howl = bandpass(source, lambda t: 470.0 + 80.0 * math.sin(TAU * 0.25 * t + 0.7 * c), 9.0)
        fifth = bandpass(source, lambda t: 705.0 + 120.0 * math.sin(TAU * 0.25 * t + 0.7 * c), 12.0)
        bed = mixed(roar, scaled(howl, 2.5), scaled(fifth, 1.0))
        channels.append([x * swells(i, 1.1 * c, (0.5, 0.35), (0.75, 0.2), (1.25, 0.1)) for i, x in enumerate(bed)])
    return channels, 0.11


def rumble(rng, frames, cutoff, bumps, start, spread, decay_low, decay_high, rise):
    """Low noise under an envelope of overlapping swells (each rising over `rise` s, then fading), faded to silence
    over the last 1.5 s."""
    bed = lowpass(lowpass(noise(rng, frames), cutoff, False), cutoff, False)
    times = [start + rng.uniform(0.0, spread) for _ in range(bumps)]
    decays = [rng.uniform(decay_low, decay_high) for _ in range(bumps)]
    sizes = [rng.uniform(0.4, 1.0) for _ in range(bumps)]
    seconds = frames / RATE
    out = [0.0] * frames
    for i, x in enumerate(bed):
        t = i / RATE
        envelope = 0.0
        for at, decay, size in zip(times, decays, sizes):
            if t > at:
                age = t - at
                envelope += size * (1.0 - math.exp(-age / rise)) * math.exp(-age / decay)
        tail = min(max((seconds - t) / 1.5, 0.0), 1.0)
        out[i] = x * envelope * tail * tail
    return out


def thunder_near(rng):
    frames = int(RATE * 6.0)
    channels = []
    for _ in range(2):
        crack = [0.0] * frames
        burst = highpass(noise(rng, frames), 800.0, False)
        for at, size in ((0.0, 1.0), (0.06, 0.6), (0.15, 0.4)):
            begin = int(at * RATE)
            for i in range(begin, min(frames, begin + int(0.4 * RATE))):
                crack[i] += burst[i] * size * math.exp(-(i - begin) / (0.05 * RATE))
        roll = rumble(rng, frames, 220.0, 6, 0.1, 2.4, 0.6, 1.6, 0.08)
        channels.append(mixed(scaled(crack, 0.35), scaled(roll, 6.0)))
    return channels, 0.1


def thunder_far(rng):
    frames = int(RATE * 8.0)
    channels = [scaled(rumble(rng, frames, 120.0, 4, 0.0, 3.0, 1.5, 2.5, 0.4), 6.0) for _ in range(2)]
    return channels, 0.08


def write_wav(path, channels, rms):
    """Scaled to the RMS given (both channels together), lowered where a peak would pass 0.98."""
    frames = len(channels[0])
    total = sum(x * x for channel in channels for x in channel)
    current = math.sqrt(total / (len(channels) * frames)) or 1.0
    peak = max(abs(x) for channel in channels for x in channel) or 1.0
    gain = min(rms / current, 0.98 / peak)
    data = bytearray()
    for i in range(frames):
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
              "rain_cover.wav": rain_cover, "wind_light.wav": wind_light, "wind_strong.wav": wind_strong,
              "thunder_near.wav": thunder_near, "thunder_far.wav": thunder_far}
    for seed, (name, make) in enumerate(sounds.items()):
        channels, rms = make(random.Random(1000 + seed))
        write_wav(os.path.join(out_dir, name), channels, rms)
    return 0


if __name__ == "__main__":
    sys.exit(main())
