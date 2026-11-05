#!/usr/bin/env python3
"""Writes the procedural source assets of the engine demo's Assets section (examples/demo/assets_src, CC0 like its
CREDITS.md says): small PNG and JPEG textures and glTF / GLB models that quench_cook turns into KTX2 and .qmodel files
at build time. Standard library only, plus macOS `sips` for the one JPEG. The output is committed; run this again
only to change the content.

    python3 tools/assets/make_demo_assets.py examples/demo/assets_src
"""

import base64
import json
import math
import os
import struct
import subprocess
import sys
import tempfile
import zlib


# ---------------------------------------------------------------- images

def png_bytes(width: int, height: int, rgb: bytes, channels: int = 3) -> bytes:
    stride = width * channels
    raw = b"".join(b"\x00" + rgb[y * stride:(y + 1) * stride] for y in range(height))

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", width, height, 8, 2 if channels == 3 else 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(raw, 9)) +
            chunk(b"IEND", b""))


def image(width: int, height: int, fn) -> bytes:
    out = bytearray()
    for y in range(height):
        for x in range(width):
            out += bytes(max(0, min(255, int(round(c)))) for c in fn(x, y))
    return bytes(out)


TILE_COLORS = [(196, 92, 52), (214, 170, 64), (96, 158, 82), (64, 140, 170), (92, 104, 190), (156, 90, 170),
               (190, 80, 110), (140, 140, 132)]


def tile(index: int):
    base = TILE_COLORS[index]
    dark = tuple(c * 0.55 for c in base)

    def fn(x: int, y: int):
        u, v = (x + 0.5) / 64.0, (y + 0.5) / 64.0
        kind = index % 8
        if kind == 0:
            on = int(u * 8) % 2 == 0
        elif kind == 1:
            on = (int(u * 4) + int(v * 4)) % 2 == 0
        elif kind == 2:
            on = math.hypot((u * 4) % 1 - 0.5, (v * 4) % 1 - 0.5) > 0.3
        elif kind == 3:
            row = int(v * 8)
            on = not ((v * 8) % 1 < 0.15 or ((u * 4 + (row % 2) * 0.5) % 1) < 0.06)
        elif kind == 4:
            on = int((u + v) * 6) % 2 == 0
        elif kind == 5:
            on = not ((u * 8) % 1 < 0.12 or (v * 8) % 1 < 0.12)
        elif kind == 6:
            on = int(math.hypot(u - 0.5, v - 0.5) * 10) % 2 == 0
        else:
            on = (v + 0.08 * math.sin(u * math.tau * 2)) * 6 % 1 < 0.5
        # A light border shows the tile edges in the streaming grid.
        if x < 2 or y < 2 or x > 61 or y > 61:
            return (235, 232, 220)
        return base if on else dark

    return fn


def planks(x: int, y: int):
    plank = y // 16
    shade = [1.0, 0.86, 0.95, 0.8, 0.92, 0.84, 0.98, 0.88][plank % 8]
    grain = 0.9 + 0.1 * math.sin(x * 0.35 + plank * 1.7 + 3.0 * math.sin(y * 0.4))
    seam = 0.45 if y % 16 in (0, 15) or (x + plank * 37) % 64 == 0 else 1.0
    s = shade * grain * seam
    return (176 * s, 120 * s, 70 * s)


def stripes(x: int, y: int):
    t = y / 63.0
    band = (x // 16) % 2
    if band:
        return (40 + 30 * t, 110 + 60 * t, 150 + 70 * t)
    return (225 - 40 * t, 220 - 40 * t, 205 - 40 * t)


def metal_roughness(x: int, y: int):
    # glTF: green = roughness, blue = metallic. Horizontal bands: rough paint, polished metal.
    metal = (y // 8) % 2 == 1
    return (0, 60 if metal else 200, 255 if metal else 0)


def jpeg_from_png(png: bytes) -> bytes:
    with tempfile.TemporaryDirectory() as tmp:
        src, dst = os.path.join(tmp, "in.png"), os.path.join(tmp, "out.jpg")
        with open(src, "wb") as f:
            f.write(png)
        subprocess.run(["sips", "-s", "format", "jpeg", "-s", "formatOptions", "85", src, "--out", dst],
                       check=True, stdout=subprocess.DEVNULL)
        with open(dst, "rb") as f:
            return f.read()


# ---------------------------------------------------------------- meshes

def box(size: float):
    h = size / 2.0
    faces = [((1, 0, 0), (0, 0, -1)), ((-1, 0, 0), (0, 0, 1)), ((0, 1, 0), (1, 0, 0)), ((0, -1, 0), (1, 0, 0)),
             ((0, 0, 1), (1, 0, 0)), ((0, 0, -1), (-1, 0, 0))]
    positions, normals, uvs, indices = [], [], [], []
    for n, t in faces:
        b = (n[1] * t[2] - n[2] * t[1], n[2] * t[0] - n[0] * t[2], n[0] * t[1] - n[1] * t[0])
        base = len(positions)
        for su, sv in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            p = tuple(h * (n[i] + su * t[i] + sv * b[i]) + (h if i == 1 else 0.0) for i in range(3))
            positions.append(p)
            normals.append(n)
            uvs.append(((su + 1) / 2, (1 - sv) / 2))
        indices += [base, base + 1, base + 2, base, base + 2, base + 3]
    return positions, normals, uvs, indices


def cylinder(radius: float, height: float, segments: int):
    positions, normals, uvs, indices = [], [], [], []
    for i in range(segments + 1):
        a = math.tau * i / segments
        c, s = math.cos(a), math.sin(a)
        for k, y in enumerate((0.0, height)):
            positions.append((radius * c, y, radius * s))
            normals.append((c, 0.0, s))
            uvs.append((i / segments, 1.0 - k))
    for i in range(segments):
        a, b = 2 * i, 2 * i + 2
        indices += [a, a + 1, b + 1, a, b + 1, b]
    for y, ny in ((0.0, -1.0), (height, 1.0)):
        center = len(positions)
        positions.append((0.0, y, 0.0))
        normals.append((0.0, ny, 0.0))
        uvs.append((0.5, 0.5))
        for i in range(segments):
            a = math.tau * i / segments
            positions.append((radius * math.cos(a), y, radius * math.sin(a)))
            normals.append((0.0, ny, 0.0))
            uvs.append((0.5 + 0.5 * math.cos(a), 0.5 + 0.5 * math.sin(a)))
        for i in range(segments):
            j = center + 1 + i
            k = center + 1 + (i + 1) % segments
            indices += [center, j, k] if ny > 0 else [center, k, j]
    return positions, normals, uvs, indices


def icosphere(radius: float, subdivisions: int):
    t = (1.0 + math.sqrt(5.0)) / 2.0
    verts = [(-1, t, 0), (1, t, 0), (-1, -t, 0), (1, -t, 0), (0, -1, t), (0, 1, t), (0, -1, -t), (0, 1, -t),
             (t, 0, -1), (t, 0, 1), (-t, 0, -1), (-t, 0, 1)]
    verts = [tuple(c / math.sqrt(sum(v * v for v in p)) for c in p) for p in verts]
    faces = [(0, 11, 5), (0, 5, 1), (0, 1, 7), (0, 7, 10), (0, 10, 11), (1, 5, 9), (5, 11, 4), (11, 10, 2),
             (10, 7, 6), (7, 1, 8), (3, 9, 4), (3, 4, 2), (3, 2, 6), (3, 6, 8), (3, 8, 9), (4, 9, 5), (2, 4, 11),
             (6, 2, 10), (8, 6, 7), (9, 8, 1)]
    for _ in range(subdivisions):
        cache = {}

        def mid(a, b):
            key = (min(a, b), max(a, b))
            if key not in cache:
                p = tuple((verts[a][i] + verts[b][i]) / 2 for i in range(3))
                length = math.sqrt(sum(c * c for c in p))
                verts.append(tuple(c / length for c in p))
                cache[key] = len(verts) - 1
            return cache[key]

        faces = [f for a, b, c in faces for f in ((a, mid(a, b), mid(c, a)), (b, mid(b, c), mid(a, b)),
                                                  (c, mid(c, a), mid(b, c)), (mid(a, b), mid(b, c), mid(c, a)))]
    positions = [(radius * x, radius * y + radius, radius * z) for x, y, z in verts]
    # Linear vertex colors: warm at the top, cool at the bottom.
    colors = [(0.9 * (0.5 + 0.5 * y) + 0.1, 0.35 + 0.2 * x, 0.9 * (0.5 - 0.5 * y) + 0.1, 1.0) for x, y, z in verts]
    return positions, colors, [i for f in faces for i in f]


# ---------------------------------------------------------------- glTF

class Gltf:
    def __init__(self):
        self.doc = {"asset": {"version": "2.0", "generator": "quench make_demo_assets.py"}, "scene": 0,
                    "scenes": [{"nodes": []}], "nodes": [], "meshes": [], "accessors": [], "bufferViews": [],
                    "buffers": [], "materials": []}
        self.blob = bytearray()

    def view(self, data: bytes, target=None) -> int:
        while len(self.blob) % 4:
            self.blob += b"\x00"
        view = {"buffer": 0, "byteOffset": len(self.blob), "byteLength": len(data)}
        if target:
            view["target"] = target
        self.blob += data
        self.doc["bufferViews"].append(view)
        return len(self.doc["bufferViews"]) - 1

    def accessor(self, values, kind: str) -> int:
        width = {"VEC2": 2, "VEC3": 3, "VEC4": 4}[kind]
        flat = [c for v in values for c in v]
        view = self.view(struct.pack(f"<{len(flat)}f", *flat), 34962)
        acc = {"bufferView": view, "componentType": 5126, "count": len(values), "type": kind}
        if kind == "VEC3":
            acc["min"] = [min(v[i] for v in values) for i in range(3)]
            acc["max"] = [max(v[i] for v in values) for i in range(3)]
        self.doc["accessors"].append(acc)
        return len(self.doc["accessors"]) - 1

    def indices(self, values) -> int:
        view = self.view(struct.pack(f"<{len(values)}H", *values), 34963)
        self.doc["accessors"].append({"bufferView": view, "componentType": 5123, "count": len(values),
                                      "type": "SCALAR"})
        return len(self.doc["accessors"]) - 1

    def mesh(self, name: str, attributes: dict, indices: int, material: int, translation=None):
        self.doc["meshes"].append({"name": name, "primitives": [{"attributes": attributes, "indices": indices,
                                                                 "material": material}]})
        node = {"name": name, "mesh": len(self.doc["meshes"]) - 1}
        if translation:
            node["translation"] = translation
        self.doc["nodes"].append(node)
        self.doc["scenes"][0]["nodes"].append(len(self.doc["nodes"]) - 1)

    def image_view(self, data: bytes, mime: str) -> int:
        self.doc.setdefault("images", []).append({"bufferView": self.view(data), "mimeType": mime})
        return self.texture(len(self.doc["images"]) - 1)

    def image_uri(self, uri: str) -> int:
        self.doc.setdefault("images", []).append({"uri": uri})
        return self.texture(len(self.doc["images"]) - 1)

    def texture(self, image: int) -> int:
        self.doc.setdefault("textures", []).append({"source": image})
        return len(self.doc["textures"]) - 1

    def write_gltf(self, path: str, bin_name=None):
        if bin_name:
            self.doc["buffers"] = [{"byteLength": len(self.blob), "uri": bin_name}]
            with open(os.path.join(os.path.dirname(path), bin_name), "wb") as f:
                f.write(self.blob)
        else:
            uri = "data:application/octet-stream;base64," + base64.b64encode(bytes(self.blob)).decode()
            self.doc["buffers"] = [{"byteLength": len(self.blob), "uri": uri}]
        with open(path, "w") as f:
            json.dump(self.doc, f, indent=1)
            f.write("\n")

    def write_glb(self, path: str):
        self.doc["buffers"] = [{"byteLength": len(self.blob)}]
        text = json.dumps(self.doc, separators=(",", ":")).encode()
        text += b" " * (-len(text) % 4)
        blob = bytes(self.blob) + b"\x00" * (-len(self.blob) % 4)
        body = struct.pack("<II", len(text), 0x4E4F534A) + text + struct.pack("<II", len(blob), 0x004E4942) + blob
        with open(path, "wb") as f:
            f.write(struct.pack("<III", 0x46546C67, 2, 12 + len(body)) + body)


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 1
    out = sys.argv[1]
    os.makedirs(os.path.join(out, "tiles"), exist_ok=True)
    os.makedirs(os.path.join(out, "models"), exist_ok=True)

    for i in range(8):
        with open(os.path.join(out, "tiles", f"tile_{i}.png"), "wb") as f:
            f.write(png_bytes(64, 64, image(64, 64, tile(i))))

    # A crate: glTF with an external buffer and an external PNG.
    with open(os.path.join(out, "models", "crate_planks.png"), "wb") as f:
        f.write(png_bytes(128, 128, image(128, 128, planks)))
    g = Gltf()
    g.doc["images"] = [{"uri": "crate_planks.png"}]
    g.doc["textures"] = [{"source": 0}]
    g.doc["materials"].append({"name": "crate", "pbrMetallicRoughness": {
        "baseColorTexture": {"index": 0}, "metallicFactor": 0.0, "roughnessFactor": 0.75}})
    p, n, uv, idx = box(1.0)
    g.mesh("crate", {"POSITION": g.accessor(p, "VEC3"), "NORMAL": g.accessor(n, "VEC3"),
                     "TEXCOORD_0": g.accessor(uv, "VEC2")}, g.indices(idx), 0)
    g.write_gltf(os.path.join(out, "models", "crate.gltf"), "crate.bin")

    # A pillar: GLB with an embedded JPEG base color and an embedded PNG metallic-roughness texture.
    g = Gltf()
    color = g.image_view(jpeg_from_png(png_bytes(128, 64, image(128, 64, stripes))), "image/jpeg")
    mr = g.image_view(png_bytes(32, 32, image(32, 32, metal_roughness)), "image/png")
    g.doc["materials"].append({"name": "pillar", "pbrMetallicRoughness": {
        "baseColorTexture": {"index": color}, "metallicRoughnessTexture": {"index": mr}}})
    p, n, uv, idx = cylinder(0.35, 1.6, 24)
    g.mesh("pillar", {"POSITION": g.accessor(p, "VEC3"), "NORMAL": g.accessor(n, "VEC3"),
                      "TEXCOORD_0": g.accessor(uv, "VEC2")}, g.indices(idx), 0)
    g.write_glb(os.path.join(out, "models", "pillar.glb"))

    # A ball: glTF with its buffer in a data URI, vertex colors and no normals (quench_cook computes them); dense
    # enough for visible levels of detail.
    g = Gltf()
    g.doc["materials"].append({"name": "ball", "pbrMetallicRoughness": {"metallicFactor": 0.0,
                                                                        "roughnessFactor": 0.35}})
    p, c, idx = icosphere(0.5, 3)
    g.mesh("ball", {"POSITION": g.accessor(p, "VEC3"), "COLOR_0": g.accessor(c, "VEC4")}, g.indices(idx), 0)
    g.write_gltf(os.path.join(out, "models", "ball.gltf"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
