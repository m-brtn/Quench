#!/usr/bin/env python3
"""Writes the engine demo's blend shape creature, the blob: a jelly body with two eyes as glTF (one skin of three
joints, one material, vertex colors), four blend shapes (morph targets) and a clip that sways it and, through the blend
shapes' weights, squashes it at the ends of its sway and stretches it in between. Procedural, CC0
(see examples/demo/assets/CREDITS.md); standard library only. The demo's build runs it (examples/demo/CMakeLists.txt)
and cooks the output; nothing it writes is committed.

    python3 tools/assets/make_demo_blob.py <output directory>

writes blob.gltf, blob.bin and blob.anim.json. Meters, Y up, the blob faces +Z. Its blend shapes, named in the mesh's
extras.targetNames as Blender writes them:
    squash   0.62 times as high and 1.28 times as wide
    stretch  1.4 times as high and 0.84 times as wide
    puff     its middle bulges out
    spikes   bumps along the body's normals (not on the eyes)
The file has no normals: the cook computes the base's and the targets' from their positions (docs/assets.md).
"""

import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_demo_character import FPS, TAU, Writer, qaxis, qmul, rounded

# name, parent, translation in the parent's frame
JOINTS = [("root", None, (0.0, 0.0, 0.0)), ("body", "root", (0.0, 0.3, 0.0)), ("top", "body", (0.0, 0.3, 0.0))]
INDEX = {name: i for i, (name, _, _) in enumerate(JOINTS)}
REST = {"root": 0.0, "body": 0.3, "top": 0.6}  # the joints' heights
RADIUS, HALF_HEIGHT = 0.4, 0.45  # the body: an ellipsoid standing on y = 0
BODY = (0.15, 0.55, 0.5, 1.0)
EYE = (0.04, 0.04, 0.05, 1.0)
TARGETS = ["squash", "stretch", "puff", "spikes"]
SWAY = 2.4  # s: the clip's length


def smooth(t):
    t = min(max(t, 0.0), 1.0)
    return t * t * (3.0 - 2.0 * t)


def influences(y):
    """Root below 0.1 m, the body joint in the middle, the top joint above 0.6 m, blended in between."""
    if y < 0.25:
        t = smooth((y - 0.05) / 0.2)
        return [(INDEX["root"], 1.0 - t), (INDEX["body"], t)]
    t = smooth((y - 0.4) / 0.25)
    return [(INDEX["body"], 1.0 - t), (INDEX["top"], t)]


class Mesh:
    def __init__(self):
        self.positions, self.colors, self.joints, self.weights, self.indices = [], [], [], [], []
        self.body = []  # per vertex: (azimuth, polar) on the body, None on the eyes

    def vertex(self, position, color, skin, body=None):
        joints = [j for j, _ in skin] + [0] * (4 - len(skin))
        weights = [w for _, w in skin] + [0.0] * (4 - len(skin))
        self.positions.append(position)
        self.colors.append(color)
        self.joints.append(joints)
        self.weights.append(weights)
        self.body.append(body)
        return len(self.positions) - 1

    def ellipsoid(self, center, radii, color, rings, sides, body, skin=None):
        """Counterclockwise seen from outside; one vertex per pole."""
        first = len(self.positions)
        for r in range(rings + 1):
            polar = math.pi * r / rings
            for s in range(1 if r in (0, rings) else sides):
                a = TAU * s / sides
                p = (center[0] + radii[0] * math.sin(polar) * math.cos(a), center[1] + radii[1] * math.cos(polar),
                     center[2] + radii[2] * math.sin(polar) * math.sin(a))
                self.vertex(p, color, skin or influences(p[1]), (a, polar) if body else None)

        def at(r, s):
            if r == 0:
                return first
            if r == rings:
                return first + 1 + (rings - 1) * sides
            return first + 1 + (r - 1) * sides + s % sides

        for r in range(rings):
            for s in range(sides):
                a, b, c, d = at(r, s), at(r, s + 1), at(r + 1, s), at(r + 1, s + 1)
                if r > 0:
                    self.indices += [a, b, d]
                if r < rings - 1:
                    self.indices += [a, d, c]


def build_mesh():
    m = Mesh()
    m.ellipsoid((0.0, HALF_HEIGHT, 0.0), (RADIUS, HALF_HEIGHT, RADIUS), BODY, 24, 40, True)
    for side in (-1.0, 1.0):
        m.ellipsoid((0.13 * side, 0.62, 0.3), (0.065, 0.075, 0.065), EYE, 8, 12, False, [(INDEX["top"], 1.0)])
    return m


def offsets(mesh, name):
    """The blend shape's position offset of every vertex."""
    out = []
    for p, body in zip(mesh.positions, mesh.body):
        x, y, z = p
        if name == "squash":
            q = (x * 1.28, y * 0.62, z * 1.28)
        elif name == "stretch":
            q = (x * 0.84, y * 1.4, z * 0.84)
        elif name == "puff":
            k = 1.0 + 0.35 * math.exp(-((y - 0.4) / 0.2) ** 2)
            q = (x * k, y, z * k)
        elif name == "spikes" and body is not None:
            a, polar = body
            bump = (0.5 + 0.5 * math.cos(7.0 * a)) * (0.5 + 0.5 * math.cos(9.0 * polar)) * math.sin(polar)
            # Along the ellipsoid's normal at this point.
            n = (x / RADIUS ** 2, (y - HALF_HEIGHT) / HALF_HEIGHT ** 2, z / RADIUS ** 2)
            length = math.sqrt(sum(c * c for c in n)) or 1.0
            q = tuple(c + 0.11 * bump ** 3 * nc / length for c, nc in zip(p, n))
        else:
            q = p
        out.append(tuple(b - a for a, b in zip(p, q)))
    return out


def sway(t):
    """The joints' rotations at t (w, x, y, z): the body leans side to side, the top a quarter turn later forward."""
    w = TAU * t / SWAY
    return {"body": qaxis((0.0, 0.0, 1.0), 0.12 * math.sin(w)),
            "top": qmul(qaxis((1.0, 0.0, 0.0), 0.14 * math.sin(w + 0.5 * math.pi)),
                        qaxis((0.0, 0.0, 1.0), 0.1 * math.sin(w)))}


def bounce(t):
    """The blend shapes' weights at t (squash, stretch, puff, spikes): squashed where the sway turns, stretched where
    it passes the middle."""
    c = math.cos(2.0 * TAU * t / SWAY)
    return [0.35 * max(0.0, -c), 0.3 * max(0.0, c), 0.0, 0.0]


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    w = Writer()

    mesh = build_mesh()
    attributes = {
        "POSITION": w.accessor([rounded(p) for p in mesh.positions], "VEC3", minmax=True),
        "COLOR_0": w.accessor([rounded(c) for c in mesh.colors], "VEC4"),
        "JOINTS_0": w.accessor(mesh.joints, "VEC4", 5121),
        "WEIGHTS_0": w.accessor([rounded(x) for x in mesh.weights], "VEC4"),
    }
    indices = w.accessor(mesh.indices, "SCALAR", 5125)
    targets = [{"POSITION": w.accessor([rounded(d) for d in offsets(mesh, name)], "VEC3", minmax=True)}
               for name in TARGETS]

    nodes = []
    for name, parent, offset in JOINTS:
        node = {"name": name, "translation": list(offset)}
        children = [i for i, (_, p, _) in enumerate(JOINTS) if p == name]
        if children:
            node["children"] = children
        nodes.append(node)
    body = len(nodes)
    nodes.append({"name": "blob", "mesh": 0, "skin": 0})
    inverse_bind = [[1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -REST[name], 0, 1] for name, _, _ in JOINTS]

    frames = int(round(SWAY * FPS))
    times = [round(min(SWAY, k / FPS), 6) for k in range(frames + 1)]
    time_accessor = w.accessor(times, "SCALAR", minmax=True)
    samplers, channels = [], []
    for joint in ("body", "top"):
        rotations = [sway(t)[joint] for t in times]
        samplers.append({"input": time_accessor,
                         "output": w.accessor([rounded((q[1], q[2], q[3], q[0])) for q in rotations], "VEC4"),
                         "interpolation": "LINEAR"})
        channels.append({"sampler": len(samplers) - 1, "target": {"node": INDEX[joint], "path": "rotation"}})
    # The node's weights: per key one value per blend shape, in the mesh's order.
    samplers.append({"input": time_accessor, "output": w.accessor([round(x, 5) for t in times for x in bounce(t)],
                                                                  "SCALAR"),
                     "interpolation": "LINEAR"})
    channels.append({"sampler": len(samplers) - 1, "target": {"node": body, "path": "weights"}})

    doc = {
        "asset": {"version": "2.0", "generator": "tools/assets/make_demo_blob.py"},
        "scene": 0,
        "scenes": [{"nodes": [INDEX["root"], body]}],
        "nodes": nodes,
        "meshes": [{"name": "blob", "weights": [0.0] * len(TARGETS), "extras": {"targetNames": TARGETS},
                    "primitives": [{"attributes": attributes, "indices": indices, "material": 0,
                                    "targets": targets}]}],
        "materials": [{"name": "blob", "pbrMetallicRoughness": {"baseColorFactor": [1.0, 1.0, 1.0, 1.0],
                                                                  "metallicFactor": 0.0, "roughnessFactor": 0.35}}],
        "skins": [{"name": "blob", "joints": list(range(len(JOINTS))), "skeleton": INDEX["root"],
                   "inverseBindMatrices": w.accessor(inverse_bind, "MAT4")}],
        "animations": [{"name": "sway", "samplers": samplers, "channels": channels}],
        "accessors": w.accessors,
        "bufferViews": w.views,
        "buffers": [{"byteLength": len(w.blob), "uri": "blob.bin"}],
    }
    with open(os.path.join(out, "blob.bin"), "wb") as f:
        f.write(bytes(w.blob))
    with open(os.path.join(out, "blob.gltf"), "w", newline="\n") as f:
        json.dump(doc, f, indent=1)
        f.write("\n")
    with open(os.path.join(out, "blob.anim.json"), "w", newline="\n") as f:
        json.dump({"clips": {"sway": {"loop": True}}}, f, indent=2)
        f.write("\n")
    print(f"blob: {len(mesh.positions)} vertices, {len(mesh.indices) // 3} triangles, {len(JOINTS)} joints, "
          f"{len(TARGETS)} blend shapes, {len(w.blob)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
