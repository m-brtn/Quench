#!/usr/bin/env python3
"""Writes the engine's default weather sounds (audio::WeatherAudio, docs/audio.md "Weather sounds") as 16-bit stereo
WAV files: loops of rain and wind and one-shots of thunder, procedural, CC0. Standard library only. The CMake
function quench_weather_sounds() runs it at build time and quench_cook checks and copies the files into a game's assets
as quench/weather/*.wav; nothing it writes is committed.

    python3 tools/assets/make_weather_sounds.py <output directory>

The rain loops are 8 s and the wind loops 4 s, at 32 kHz. A loop's noise is filtered as a periodic signal (the
filters run twice around the loop, the second pass kept), its drops are placed around the loop (a drop's tail
continues at the start) and its swells are whole cycles in it, so the loops have no seam.

Rain is made of noise only, never of tones: a drop that strikes a surface is a click, a few milliseconds of noise
with the colour of what it struck, and a tone per drop at a pitch of its own turns a thousand drops into a tune
(the first version's decaying sines did, most of all under the roof). Each rain loop has three parts: a bed of
noise through second-order filters (steep enough to give it a colour; one pole leaves the hiss of a detuned
radio), a crackle of sparse impulses of very uneven size through a wide band (rain is a crowd of small impacts,
rougher than even noise), and the nearer drops, each a burst of noise through a filter of its own, placed between
the two channels. The loudest drops are held back so that no single one is known again when the loop comes round,
and the whole breathes a little, unevenly.

The layers differ where the engine's checks look: the light rain's energy is high (fine drops, 2 to 9 kHz), the
heavy rain's broad and loud, the distant rain's low (below about 700 Hz), the roof's dull thuds lower than the open
air's; the light wind is a low rush that swells, the strong wind louder with a howl that rises and falls. The
thunder is 6 s (near: a crack, then rolling rumble) and 8 s (far: a low rumble only), fading to silence at its end.
Their levels leave room for each other: heavy rain, a strong wind and near thunder together (a windy storm) peak
below full scale.
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


def scaled(signal, gain):
    return [x * gain for x in signal]


def mixed(*signals):
    return [sum(values) for values in zip(*signals)]


# ---- The rain

RAIN_SECONDS = 8.0
RAIN_FRAMES = int(RATE * RAIN_SECONDS)


def lowpass_k(cutoff, q):
    """Coefficients (b0, b1, b2, a1, a2) of a second-order low-pass (Bristow-Johnson's cookbook)."""
    w0 = TAU * min(cutoff, 0.45 * RATE) / RATE
    alpha = math.sin(w0) / (2.0 * q)
    c = math.cos(w0)
    a0 = 1.0 + alpha
    return ((1.0 - c) * 0.5 / a0, (1.0 - c) / a0, (1.0 - c) * 0.5 / a0, -2.0 * c / a0, (1.0 - alpha) / a0)


def highpass_k(cutoff, q):
    w0 = TAU * min(cutoff, 0.45 * RATE) / RATE
    alpha = math.sin(w0) / (2.0 * q)
    c = math.cos(w0)
    a0 = 1.0 + alpha
    return ((1.0 + c) * 0.5 / a0, -(1.0 + c) / a0, (1.0 + c) * 0.5 / a0, -2.0 * c / a0, (1.0 - alpha) / a0)


def bandpass_k(centre, q):
    """A band-pass with 0 dB at its centre."""
    w0 = TAU * min(centre, 0.45 * RATE) / RATE
    alpha = math.sin(w0) / (2.0 * q)
    a0 = 1.0 + alpha
    return (alpha / a0, 0.0, -alpha / a0, -2.0 * math.cos(w0) / a0, (1.0 - alpha) / a0)


def filtered(signal, k):
    """A periodic signal through a second-order filter: two passes around the loop, the second kept."""
    b0, b1, b2, a1, a2 = k
    x1 = x2 = y1 = y2 = 0.0
    for x in signal:
        y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        x2, x1 = x1, x
        y2, y1 = y1, y
    out = [0.0] * len(signal)
    for i, x in enumerate(signal):
        y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        x2, x1 = x1, x
        y2, y1 = y1, y
        out[i] = y
    return out


def band(signal, low, high):
    """What a signal has between two frequencies, 12 dB per octave outside them."""
    return filtered(filtered(signal, highpass_k(low, 0.7071)), lowpass_k(high, 0.7071))


def crackle(rng, frames, per_second, power):
    """Sparse impulses at random places, most of them tiny and a few large (a random number to `power`): the
    roughness of many small impacts once a filter has given them a colour."""
    out = [0.0] * frames
    for _ in range(int(per_second * frames / RATE)):
        out[rng.randrange(frames)] += (rng.random() ** power) * (1.0 if rng.random() < 0.5 else -1.0)
    return out


def add_impacts(left, right, rng, per_second, design, low_hz, high_hz, q_low, q_high, decay_low, decay_high,
                loudness, floor=0.2):
    """Drops striking near the listener: each a burst of noise that dies away within a few milliseconds, through a
    filter of its own (`design`: lowpass_k for a dull thud, bandpass_k for a click or a splat), somewhere between
    the channels. Their sizes run from `floor` to 1 of `loudness`, most of them small."""
    frames = len(left)
    for _ in range(int(per_second * frames / RATE)):
        start = rng.randrange(frames)
        b0, b1, b2, a1, a2 = design(math.exp(rng.uniform(math.log(low_hz), math.log(high_hz))),
                                    rng.uniform(q_low, q_high))
        decay = rng.uniform(decay_low, decay_high)
        k = math.exp(-1.0 / (decay * RATE))
        envelope = loudness * (floor + (1.0 - floor) * rng.random() ** 3)
        pan = rng.uniform(0.0, 0.5 * math.pi)
        gain_left, gain_right = math.cos(pan), math.sin(pan)
        x1 = x2 = y1 = y2 = 0.0
        for j in range(int(decay * 5.0 * RATE) + 8):
            x = rng.uniform(-1.0, 1.0) * envelope
            y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
            x2, x1 = x1, x
            y2, y1 = y1, y
            at = (start + j) % frames
            left[at] += y * gain_left
            right[at] += y * gain_right
            envelope *= k


def breathing(signal, rng, depth):
    """The loop a little louder and quieter as it goes, unevenly: slow sines of whole cycles in the loop at random
    phases (0.125 to 0.9 Hz in 8 s)."""
    frames = len(signal)
    terms = [(cycles, rng.uniform(0.0, TAU), 1.0 / math.sqrt(cycles)) for cycles in (1, 2, 3, 5, 7)]
    norm = sum(weight for _, _, weight in terms)
    step = 256
    knots = [1.0 + depth * sum(weight * math.sin(TAU * cycles * i / frames + phase)
                               for cycles, phase, weight in terms) / norm
             for i in range(0, frames + step, step)]
    return [x * (knots[i // step] + (knots[i // step + 1] - knots[i // step]) * (i % step) / step)
            for i, x in enumerate(signal)]


def held(channels, crest):
    """The loudest drops held back: a soft limit at `crest` times the loop's RMS, so that no single drop stands
    out enough to be known again when the loop comes round."""
    rms = math.sqrt(sum(x * x for channel in channels for x in channel) / sum(len(channel) for channel in channels))
    limit = crest * rms
    return [[limit * math.tanh(x / limit) for x in channel] for channel in channels]


def rain_light(rng):
    """Fine rain on the ground and on leaves: a soft, high hiss, a fine crackle and single small drops nearby."""
    channels = []
    for _ in range(2):
        hiss = scaled(band(noise(rng, RAIN_FRAMES), 2000.0, 7000.0), 0.4)
        fine = filtered(crackle(rng, RAIN_FRAMES, 2500, 4.0), bandpass_k(4200.0, 0.8))
        channels.append(mixed(hiss, scaled(fine, 4.0)))
    add_impacts(channels[0], channels[1], rng, 70, bandpass_k, 1800.0, 7000.0, 1.2, 3.0, 0.0004, 0.0012, 2.2)
    return held([breathing(channel, rng, 0.12) for channel in channels], 7.0), 0.06


def rain_heavy(rng):
    """A downpour: a broad roar with a low body, hiss above it, a dense crackle, the splats of large drops and a few heavy ones."""
    channels = []
    for _ in range(2):
        roar = scaled(band(noise(rng, RAIN_FRAMES), 110.0, 2600.0), 1.4)
        low = scaled(band(noise(rng, RAIN_FRAMES), 90.0, 800.0), 1.6)
        hiss = scaled(band(noise(rng, RAIN_FRAMES), 2600.0, 10000.0), 0.5)
        dense = filtered(crackle(rng, RAIN_FRAMES, 7000, 3.0), bandpass_k(2400.0, 0.6))
        channels.append(mixed(roar, low, hiss, scaled(dense, 3.5)))
    add_impacts(channels[0], channels[1], rng, 220, bandpass_k, 600.0, 4500.0, 0.8, 2.5, 0.0008, 0.003, 4.5)
    add_impacts(channels[0], channels[1], rng, 25, lowpass_k, 250.0, 700.0, 0.7, 1.0, 0.004, 0.009, 5.0)
    return held([breathing(channel, rng, 0.15) for channel in channels], 5.0), 0.13


def rain_far(rng):
    """Rain a few kilometres off: what the air leaves of it, a low wash that comes and goes."""
    channels = []
    for _ in range(2):
        wash = filtered(band(noise(rng, RAIN_FRAMES), 70.0, 520.0), lowpass_k(900.0, 0.7071))
        channels.append(breathing(wash, rng, 0.3))
    return channels, 0.1


def rain_cover(rng):
    """Rain on a roof over the listener: a close drumming of small dull thuds, larger ones among them, and the
    roof's own low body. The roof answers every drop in the same few wide bands (its panels), so nothing in it
    has a pitch of its own."""
    left = [0.0] * RAIN_FRAMES
    right = [0.0] * RAIN_FRAMES
    add_impacts(left, right, rng, 1400, lowpass_k, 600.0, 2400.0, 0.7, 1.0, 0.001, 0.003, 0.45, 0.1)
    add_impacts(left, right, rng, 110, lowpass_k, 400.0, 1500.0, 0.7, 1.1, 0.002, 0.005, 1.0, 0.25)
    channels = []
    for thuds in (left, right):
        body = mixed(filtered(thuds, bandpass_k(190.0, 2.5)), filtered(thuds, bandpass_k(410.0, 3.0)))
        patter = band(noise(rng, RAIN_FRAMES), 90.0, 1100.0)
        channels.append(breathing(mixed(thuds, scaled(body, 0.8), scaled(patter, 0.08)), rng, 0.12))
    return held(channels, 6.0), 0.12


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
