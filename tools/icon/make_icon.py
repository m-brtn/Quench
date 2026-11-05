#!/usr/bin/env python3
"""Writes the procedural app icons of the engine's examples and the game template (1024 x 1024 PNG, MIT like the
engine): an orange ring on a dark blue square; with --demo (the engine demo) a teal ring around an orange sphere.
Standard library only.

    python3 tools/icon/make_icon.py examples/hello/icon/icon.png
    python3 tools/icon/make_icon.py --demo examples/demo/icon/icon.png
"""

import math
import struct
import sys
import zlib

SIZE = 1024


def coverage_between(r: float, inner: float, outer: float) -> float:
    # 2 px antialiased edges.
    return max(0.0, min(1.0, (r - inner) / 2.0 + 0.5)) * max(0.0, min(1.0, (outer - r) / 2.0 + 0.5))


def blend(bg: tuple[int, int, int], color: tuple[int, int, int], coverage: float) -> tuple[int, int, int]:
    return tuple(int(b + (c - b) * coverage) for b, c in zip(bg, color))


def pixel(x: float, y: float, demo: bool) -> tuple[int, int, int]:
    # Background: dark blue, slightly lighter towards the top.
    t = y / SIZE
    color = (int(18 + 14 * (1 - t)), int(28 + 18 * (1 - t)), int(52 + 30 * (1 - t)))
    cx, cy = SIZE * 0.5, SIZE * 0.5
    r = math.hypot(x - cx, y - cy)
    # Ring between radii 250 and 390 px; shading from the top left.
    inner, outer = 250.0, 390.0
    coverage = coverage_between(r, inner, outer)
    if coverage > 0.0:
        tube = (r - (inner + outer) / 2) / ((outer - inner) / 2)  # -1 .. 1 across the tube
        nx = (x - cx) / max(r, 1.0) * tube
        ny = (y - cy) / max(r, 1.0) * tube
        nz = math.sqrt(max(0.0, 1.0 - tube * tube))
        light = max(0.0, -0.45 * nx - 0.55 * ny + 0.7 * nz)
        if demo:
            ring = (int(min(255, 10 + 60 * light)), int(min(255, 80 + 150 * light)), int(min(255, 90 + 140 * light)))
        else:
            ring = (int(min(255, 90 + 165 * light)), int(min(255, 30 + 80 * light)), int(min(255, 10 + 30 * light)))
        color = blend(color, ring, coverage)
    if demo:
        # A lit sphere of radius 150 px inside the ring.
        coverage = coverage_between(r, -2.0, 150.0)
        if coverage > 0.0:
            nx, ny = (x - cx) / 150.0, (y - cy) / 150.0
            nz = math.sqrt(max(0.0, 1.0 - nx * nx - ny * ny))
            light = max(0.0, -0.45 * nx - 0.55 * ny + 0.7 * nz)
            sphere = (int(min(255, 120 + 135 * light)), int(min(255, 45 + 120 * light)), int(min(255, 10 + 40 * light)))
            color = blend(color, sphere, coverage)
    return color


def main() -> None:
    args = sys.argv[1:]
    demo = "--demo" in args
    paths = [a for a in args if a != "--demo"]
    if len(paths) != 1:
        sys.exit(__doc__)
    rows = bytearray()
    for y in range(SIZE):
        rows.append(0)  # filter: none
        for x in range(SIZE):
            rows.extend(pixel(x + 0.5, y + 0.5, demo))

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", SIZE, SIZE, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(rows), 9))
    png += chunk(b"IEND", b"")
    with open(paths[0], "wb") as f:
        f.write(png)


if __name__ == "__main__":
    main()
