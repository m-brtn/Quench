#!/usr/bin/env python3
"""Writes the procedural textures of the engine demo's Tunnel section as PNG files: metal wall panels (color, normal,
metallic-roughness), wet asphalt (color, normal, metallic-roughness), a puddle's shape, the embossed plastic's and the
rubber's normal maps and the brushed aluminium's metallic-roughness streaks. Our own content (CC0, as the demo's
CREDITS.md says), standard library only, the same bytes on every run. Runs at build time (examples/demo/CMakeLists.txt);
quench_cook turns the files into KTX2.

    python3 tools/assets/make_demo_tunnel.py <output directory>

Normal maps are tangent space as in glTF: x along +u, y toward -v (up in the image), z out of the surface.
Metallic-roughness maps are as in glTF: green roughness, blue metallic.
"""

import math
import os
import random
import struct
import sys
import zlib


def png_bytes(width: int, height: int, pixels: bytes, channels: int) -> bytes:
    stride = width * channels
    raw = b"".join(b"\x00" + pixels[y * stride:(y + 1) * stride] for y in range(height))

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", width, height, 8, 2 if channels == 3 else 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(raw, 9)) +
            chunk(b"IEND", b""))


def write_png(path: str, width: int, height: int, rows, channels: int = 3) -> None:
    out = bytearray()
    for row in rows:
        for value in row:
            for c in value:
                out.append(max(0, min(255, int(c + 0.5))))
    with open(path, "wb") as f:
        f.write(png_bytes(width, height, bytes(out), channels))


def srgb(linear: float) -> float:
    linear = max(0.0, min(1.0, linear))
    return 255.0 * (12.92 * linear if linear <= 0.0031308 else 1.055 * linear ** (1.0 / 2.4) - 0.055)


# ---------------------------------------------------------------- tileable value noise

class Noise:
    """Value noise on a lattice of `period` cells that repeats over the texture."""

    def __init__(self, seed: int, period: int):
        rng = random.Random(seed)
        self.period = period
        self.values = [rng.random() for _ in range(period * period)]

    def at(self, u: float, v: float) -> float:  # u, v in texture units (0..1 repeats)
        p = self.period
        x, y = u * p, v * p
        x0, y0 = math.floor(x), math.floor(y)
        fx, fy = x - x0, y - y0
        fx, fy = fx * fx * (3.0 - 2.0 * fx), fy * fy * (3.0 - 2.0 * fy)
        x0 %= p
        y0 %= p
        x1, y1 = (x0 + 1) % p, (y0 + 1) % p
        v00, v10 = self.values[y0 * p + x0], self.values[y0 * p + x1]
        v01, v11 = self.values[y1 * p + x0], self.values[y1 * p + x1]
        return (v00 + (v10 - v00) * fx) * (1.0 - fy) + (v01 + (v11 - v01) * fx) * fy


class Fbm:
    def __init__(self, seed: int, period: int, octaves: int):
        self.layers = [Noise(seed + i * 101, period << i) for i in range(octaves)]

    def at(self, u: float, v: float) -> float:
        total, amplitude, norm = 0.0, 1.0, 0.0
        for layer in self.layers:
            total += layer.at(u, v) * amplitude
            norm += amplitude
            amplitude *= 0.5
        return total / norm


def normals_from_height(height, size: int, strength: float):
    """Rows of RGB normals from a size x size height field (wrapping): glTF tangent space."""
    rows = []
    for y in range(size):
        up, down = height[(y - 1) % size], height[(y + 1) % size]
        row, here = [], height[y]
        for x in range(size):
            dx = (here[(x + 1) % size] - here[(x - 1) % size]) * 0.5 * strength
            dy = (down[x] - up[x]) * 0.5 * strength
            nx, ny, nz = -dx, dy, 1.0
            inv = 1.0 / math.sqrt(nx * nx + ny * ny + nz * nz)
            row.append((127.5 + 127.5 * nx * inv, 127.5 + 127.5 * ny * inv, 127.5 + 127.5 * nz * inv))
        rows.append(row)
    return rows


def smoothstep(a: float, b: float, x: float) -> float:
    t = max(0.0, min(1.0, (x - a) / (b - a)))
    return t * t * (3.0 - 2.0 * t)


# ---------------------------------------------------------------- metal wall panels

PANEL = 256  # pixels per panel; the texture is 2 x 2 panels (3 x 3 m on the walls)


def panels(out: str) -> None:
    size = 2 * PANEL
    grime, fine, streaks = Fbm(11, 4, 4), Fbm(12, 32, 3), Noise(13, 64)
    rng = random.Random(14)
    # Per panel: tint, roughness, kind (0 plain, 1 vent slots, 2 raised plate).
    kinds = [(rng.uniform(-0.06, 0.04), rng.uniform(0.28, 0.48), k) for k in (0, 1, 2, 0)]
    height = [[0.0] * size for _ in range(size)]
    color, mr = [], []
    for y in range(size):
        crow, mrow = [], []
        py, ly = y // PANEL, y % PANEL
        for x in range(size):
            px, lx = x // PANEL, x % PANEL
            tint, rough, kind = kinds[py * 2 + px]
            u, v = x / size, y / size
            edge = min(lx, ly, PANEL - 1 - lx, PANEL - 1 - ly)
            h = 0.0
            seam = edge < 3
            if seam:
                h = -1.0
            elif edge < 9:
                h = -1.0 + smoothstep(3.0, 9.0, edge)
            # Rivets 14 px in from the corners and the middles of the edges.
            for cx in (14, PANEL // 2, PANEL - 15):
                for cy in (14, PANEL // 2, PANEL - 15):
                    if cx == PANEL // 2 and cy == PANEL // 2:
                        continue
                    d2 = (lx - cx) ** 2 + (ly - cy) ** 2
                    if d2 < 25:
                        h = max(h, 0.6 * math.sqrt(1.0 - d2 / 25.0))
            vent = False
            if kind == 1 and 150 <= ly < 222 and 52 <= lx < PANEL - 52:
                slot = (ly - 150) % 14
                if slot < 7:
                    vent = True
                    h = -1.2 + 0.3 * smoothstep(0.0, 2.0, min(slot, 6 - slot))
            if kind == 2 and 48 <= lx < PANEL - 48 and 48 <= ly < PANEL - 48:
                e = min(lx - 48, ly - 48, PANEL - 49 - lx, PANEL - 49 - ly)
                h = max(h, 0.5 * smoothstep(0.0, 6.0, e))
            h += (fine.at(u, v) - 0.5) * 0.05
            height[y][x] = h
            dirt = grime.at(u, v)
            streak = streaks.at(u, v * 0.125)
            near_seam = 1.0 - smoothstep(3.0, 26.0, edge)
            base = 0.42 + tint + (dirt - 0.5) * 0.12 - 0.10 * near_seam * dirt - 0.05 * streak
            if seam or vent:
                crow.append((srgb(0.03),) * 3)
                mrow.append((255, 230, 60))
                continue
            crow.append((srgb(base * 0.97), srgb(base), srgb(base * 1.06)))
            r = rough + (dirt - 0.5) * 0.18 + 0.15 * near_seam * dirt + 0.08 * streak
            mrow.append((255, 255.0 * max(0.08, min(1.0, r)), 255))
        color.append(crow)
        mr.append(mrow)
    write_png(os.path.join(out, "panel_color.png"), size, size, color)
    write_png(os.path.join(out, "panel_normal.png"), size, size, normals_from_height(height, size, 2.2))
    write_png(os.path.join(out, "panel_mr.png"), size, size, mr)


# ---------------------------------------------------------------- wet asphalt

def asphalt(out: str) -> None:
    size = 512  # 2 x 2 m on the floor
    wet, fine, tone = Fbm(21, 4, 3), Fbm(22, 64, 3), Fbm(23, 8, 3)
    rng = random.Random(24)
    height = [[0.0] * size for _ in range(size)]
    stone = [[0.0] * size for _ in range(size)]
    # The aggregate: small stones (radius 2 to 6 px) stamped into a wrapping grid.
    for _ in range(2600):
        cx, cy, r = rng.uniform(0, size), rng.uniform(0, size), rng.uniform(2.0, 6.0)
        shade = rng.uniform(-1.0, 1.0)
        for y in range(int(cy - r) - 1, int(cy + r) + 2):
            for x in range(int(cx - r) - 1, int(cx + r) + 2):
                d2 = ((x - cx) ** 2 + (y - cy) ** 2) / (r * r)
                if d2 < 1.0:
                    yy, xx = y % size, x % size
                    top = math.sqrt(1.0 - d2) * r / 6.0
                    if top > height[yy][xx]:
                        height[yy][xx] = top
                        stone[yy][xx] = shade
    color, mr = [], []
    for y in range(size):
        crow, mrow = [], []
        for x in range(size):
            u, v = x / size, y / size
            h = height[y][x] + (fine.at(u, v) - 0.5) * 0.35
            height[y][x] = h
            w = smoothstep(0.42, 0.62, wet.at(u, v))  # wetter patches: darker, smoother
            s = stone[y][x]
            base = 0.055 + 0.03 * (tone.at(u, v) - 0.5)
            if s != 0.0:
                base += 0.035 * s + 0.02
            base *= 1.0 - 0.35 * w
            crow.append((srgb(base), srgb(base), srgb(base * 1.04)))
            rough = 0.62 - 0.30 * w + (0.08 if s != 0.0 else 0.0)
            mrow.append((255, 255.0 * rough, 0))
        color.append(crow)
        mr.append(mrow)
    write_png(os.path.join(out, "asphalt_color.png"), size, size, color)
    write_png(os.path.join(out, "asphalt_normal.png"), size, size, normals_from_height(height, size, 1.0))
    write_png(os.path.join(out, "asphalt_mr.png"), size, size, mr)


def puddle(out: str) -> None:
    """The puddle's shape in alpha (alpha tested), its dark water color in rgb; not tiled."""
    size = 256
    shape = Fbm(31, 4, 4)
    rows = []
    for y in range(size):
        row = []
        for x in range(size):
            u, v = x / size, y / size
            dx, dy = (u - 0.5) / 0.46, (v - 0.5) / 0.40
            d = math.sqrt(dx * dx + dy * dy) + (shape.at(u, v) - 0.5) * 0.9
            row.append((srgb(0.012), srgb(0.013), srgb(0.016), 255 if d < 0.78 else 0))
        rows.append(row)
    write_png(os.path.join(out, "puddle.png"), size, size, rows, 4)


# ---------------------------------------------------------------- embossed plastic, rubber, brushed aluminium

def plastic(out: str) -> None:
    """Raised rounded diamonds in rows (an embossed plastic case), with a fine grain."""
    size, cells = 256, 8
    grain = Fbm(41, 64, 2)
    height = []
    for y in range(size):
        row = []
        for x in range(size):
            fx, fy = (x + 0.5) / size * cells, (y + 0.5) / size * cells
            cx, cy = abs(fx % 1.0 - 0.5), abs(fy % 1.0 - 0.5)
            d = cx + cy  # diamond distance
            row.append(0.9 * (1.0 - smoothstep(0.26, 0.36, d)) + (grain.at(x / size, y / size) - 0.5) * 0.12)
        height.append(row)
    write_png(os.path.join(out, "plastic_normal.png"), size, size, normals_from_height(height, size, 3.0))


def rubber(out: str) -> None:
    """A coin pattern (raised discs) of a rubber mat, with a fine grain."""
    size, cells = 256, 8
    grain = Fbm(51, 64, 2)
    height = []
    for y in range(size):
        row = []
        for x in range(size):
            fx, fy = (x + 0.5) / size * cells, (y + 0.5) / size * cells
            d = math.hypot(fx % 1.0 - 0.5, fy % 1.0 - 0.5)
            row.append(1.2 * (1.0 - smoothstep(0.24, 0.30, d)) + (grain.at(x / size, y / size) - 0.5) * 0.2)
        height.append(row)
    write_png(os.path.join(out, "rubber_normal.png"), size, size, normals_from_height(height, size, 2.5))


def brushed(out: str) -> None:
    """Brushed aluminium: roughness streaks along u (constant along each streak), metallic 1."""
    size = 256
    streaks = [Noise(61 + i, 256 >> i) for i in range(3)]
    smudge = Fbm(64, 4, 2)
    rows = []
    for y in range(size):
        v = y / size
        line = sum(n.at(0.0, v) * (0.5 ** i) for i, n in enumerate(streaks)) / 1.75
        row = []
        for x in range(size):
            u = x / size
            r = 0.22 + 0.14 * line + 0.06 * (smudge.at(u, v) - 0.5)
            row.append((255, 255.0 * r, 255))
        rows.append(row)
    write_png(os.path.join(out, "brushed_mr.png"), size, size, rows)


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    panels(out)
    asphalt(out)
    puddle(out)
    plastic(out)
    rubber(out)
    brushed(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
