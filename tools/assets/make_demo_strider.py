#!/usr/bin/env python3
"""Writes the engine demo's second character, the strider: a jointed figure as glTF (one skin of 21 joints, one
material) without clips of its own, and the bone map that retargets the mannequin's clips onto it
(tools/assets/make_demo_character.py; docs/animation.md, "Retargeting"). Procedural, CC0 (see
examples/demo/assets/CREDITS.md); standard library only. The demo's build runs it (examples/demo/CMakeLists.txt) and
cooks the output; nothing it writes is committed.

    python3 tools/assets/make_demo_strider.py <output directory>

writes strider.gltf, strider.bin and strider.retarget.json. Meters, Y up, the figure faces +Z (its left is +X). It
differs from the mannequin in what retargeting has to handle:
    names       Root, Hips, Spine, Spine1, Neck, Head, LeftArm, LeftForeArm, LeftHand, LeftUpLeg, LeftLeg, LeftFoot,
                LeftToeBase and the right side's (instead of root, pelvis, spine, chest, ...)
    joints      Spine2 between Spine1 and the neck and shoulders, which the map leaves out
    proportions hips 1.15 m high and legs 1.15 times the mannequin's, a shorter torso, arms 1.1 times as long
    rest pose   a T-pose (the mannequin's arms hang), and rest rotations that point each limb bone along its joint's
                +Y (the arms' joints turned by 90 degrees about Z, the legs' by 180 degrees about X)
"""

import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_demo_character import (TAU, Mesh, Writer, add, cross, mul, normalize, qconj, qmul, qrot, rounded, rx, rz,
                                 smooth, sub)

LEG = 1.15  # the legs and the hips' height relative to the mannequin's
ARM = 1.1
HALF_TURN = math.pi
# name, parent, translation in the parent's frame, rest rotation relative to the parent (w, x, y, z)
IDENTITY = (1.0, 0.0, 0.0, 0.0)
JOINTS = [
    ("Root", None, (0.0, 0.0, 0.0), IDENTITY),
    ("Hips", "Root", (0.0, 1.0 * LEG, 0.0), IDENTITY),
    ("Spine", "Hips", (0.0, 0.11, 0.0), IDENTITY),
    ("Spine1", "Spine", (0.0, 0.17, 0.0), IDENTITY),
    ("Spine2", "Spine1", (0.0, 0.09, 0.0), IDENTITY),
    ("Neck", "Spine2", (0.0, 0.13, 0.0), IDENTITY),
    ("Head", "Neck", (0.0, 0.11, 0.0), IDENTITY),
    ("LeftArm", "Spine2", (0.2, 0.06, 0.0), rz(-0.5 * math.pi)),
    ("LeftForeArm", "LeftArm", (0.0, 0.28 * ARM, 0.0), IDENTITY),
    ("LeftHand", "LeftForeArm", (0.0, 0.25 * ARM, 0.0), IDENTITY),
    ("RightArm", "Spine2", (-0.2, 0.06, 0.0), rz(0.5 * math.pi)),
    ("RightForeArm", "RightArm", (0.0, 0.28 * ARM, 0.0), IDENTITY),
    ("RightHand", "RightForeArm", (0.0, 0.25 * ARM, 0.0), IDENTITY),
    ("LeftUpLeg", "Hips", (0.11, -0.06 * LEG, 0.0), rx(HALF_TURN)),
    ("LeftLeg", "LeftUpLeg", (0.0, 0.44 * LEG, 0.0), IDENTITY),
    ("LeftFoot", "LeftLeg", (0.0, 0.42 * LEG, 0.0), IDENTITY),
    ("LeftToeBase", "LeftFoot", (0.0, 0.05 * LEG, -0.12 * LEG), IDENTITY),
    ("RightUpLeg", "Hips", (-0.11, -0.06 * LEG, 0.0), rx(HALF_TURN)),
    ("RightLeg", "RightUpLeg", (0.0, 0.44 * LEG, 0.0), IDENTITY),
    ("RightFoot", "RightLeg", (0.0, 0.42 * LEG, 0.0), IDENTITY),
    ("RightToeBase", "RightFoot", (0.0, 0.05 * LEG, -0.12 * LEG), IDENTITY),
]
INDEX = {name: i for i, (name, _, _, _) in enumerate(JOINTS)}
ANKLE_HEIGHT = 0.08 * LEG

# The bone map: strider joint -> mannequin joint. Spine2 is not in it.
MAP = {
    "Root": "root", "Hips": "pelvis", "Spine": "spine", "Spine1": "chest", "Neck": "neck", "Head": "head",
    "LeftArm": "upperarm_l", "LeftForeArm": "lowerarm_l", "LeftHand": "hand_l",
    "RightArm": "upperarm_r", "RightForeArm": "lowerarm_r", "RightHand": "hand_r",
    "LeftUpLeg": "thigh_l", "LeftLeg": "shin_l", "LeftFoot": "foot_l", "LeftToeBase": "toe_l",
    "RightUpLeg": "thigh_r", "RightLeg": "shin_r", "RightFoot": "foot_r", "RightToeBase": "toe_r",
}
RETARGET = {"joints": MAP, "translations": ["Root", "Hips"], "scale": "Hips"}


def rest_model():
    """Per joint: (position, rotation) in model space at rest."""
    out = {}
    for name, parent, offset, rotation in JOINTS:
        if parent is None:
            out[name] = (offset, rotation)
        else:
            p, q = out[parent]
            out[name] = (add(p, qrot(q, offset)), qmul(q, rotation))
    return out


REST = rest_model()


def pos(name):
    return REST[name][0]


class StriderMesh(Mesh):
    def vertex(self, position, normal, influences):
        """influences: (joint name, weight) pairs, at most four, by the strider's joint names."""
        joints = [INDEX[name] for name, _ in influences] + [0] * (4 - len(influences))
        weights = [w for _, w in influences] + [0.0] * (4 - len(influences))
        total = sum(weights)
        self.positions.append(position)
        self.normals.append(normalize(normal))
        self.joints.append(joints)
        self.weights.append([w / total for w in weights])
        return len(self.positions) - 1


def tube(mesh, rings, axis, sides=12):
    """rings: (center, radius a, radius b, influences) along `axis`; capped at both ends."""
    axis = normalize(axis)
    helper = (0.0, 0.0, 1.0) if abs(axis[2]) < 0.9 else (1.0, 0.0, 0.0)
    u = normalize(cross(helper, axis))
    v = cross(axis, u)
    first = len(mesh.positions)
    for center, ra, rb, influences in rings:
        for s in range(sides):
            a = TAU * s / sides
            c, d = math.cos(a), math.sin(a)
            offset = add(mul(u, ra * c), mul(v, rb * d))
            mesh.vertex(add(center, offset), add(mul(u, c / ra), mul(v, d / rb)), influences)
    for r in range(len(rings) - 1):
        for s in range(sides):
            a, b = first + r * sides + s, first + r * sides + (s + 1) % sides
            mesh.triangle(a, b, b + sides)
            mesh.triangle(a, b + sides, a + sides)
    for r, sign in ((0, -1.0), (len(rings) - 1, 1.0)):
        center = mesh.vertex(rings[r][0], mul(axis, sign), rings[r][3])
        for s in range(sides):
            mesh.triangle(center, first + r * sides + s, first + r * sides + (s + 1) % sides)


def chain(joints, distance, blend_length=0.07):
    """Influences at `distance` along a straight chain of joints: blended over blend_length around each inner joint."""
    marks = [math.dist(pos(joints[0]), pos(j)) for j in joints]
    influences = [(joints[0], 1.0)]
    for k in range(1, len(joints) - 1):
        if distance >= marks[k] - blend_length:
            t = smooth((distance - marks[k] + blend_length) / (2.0 * blend_length))
            if t >= 1.0:
                influences = [(joints[k], 1.0)]
            elif t <= 0.0:
                influences = [(joints[k - 1], 1.0)]
            else:
                influences = [(joints[k - 1], 1.0 - t), (joints[k], t)]
    return influences


def limb(mesh, joints, profile):
    """A tube from the first joint toward the last. profile: (distance from the first joint, radius a, radius b)."""
    start = pos(joints[0])
    axis = normalize(sub(pos(joints[-1]), start))
    rings = [(add(start, mul(axis, d)), ra, rb, chain(joints, d)) for d, ra, rb in profile]
    tube(mesh, rings, axis)


def build_mesh():
    m = StriderMesh()
    # Torso: from below the hips to the neck, Hips, Spine, Spine1, Spine2 and Neck blended at their joints.
    base = pos("Hips")[1]
    torso = [(-0.16, 0.14, 0.1), (-0.07, 0.17, 0.115), (0.02, 0.16, 0.11), (0.1, 0.135, 0.095), (0.18, 0.13, 0.09),
             (0.26, 0.15, 0.1), (0.34, 0.18, 0.115), (0.41, 0.19, 0.12), (0.46, 0.175, 0.11), (0.5, 0.11, 0.08),
             (0.53, 0.05, 0.05), (0.62, 0.045, 0.045)]
    spine = ["Hips", "Spine", "Spine1", "Spine2", "Neck"]
    rings = [((0.0, base + d, 0.0), ra, rb, chain(spine, max(d, 0.0), 0.05)) for d, ra, rb in torso]
    tube(m, rings, (0.0, 1.0, 0.0))
    # Head, with a nose so that its facing shows.
    head = add(pos("Head"), (0.0, 0.12, 0.01))
    m.sphere(head, 0.12, [("Head", 1.0)])
    m.box(add(head, (-0.016, -0.03, 0.105)), add(head, (0.016, 0.0, 0.15)), [("Head", 1.0)])
    for side, sign in (("Left", 1.0), ("Right", -1.0)):
        limb(m, [side + "UpLeg", side + "Leg", side + "Foot"],
             [(-0.02, 0.09, 0.09), (0.06, 0.1, 0.1), (0.23, 0.088, 0.088), (0.41, 0.068, 0.068),
              (0.5, 0.064, 0.064), (0.6, 0.068, 0.068), (0.71, 0.07, 0.07), (0.87, 0.052, 0.052),
              (0.98, 0.046, 0.046)])
        limb(m, [side + "Arm", side + "ForeArm", side + "Hand"],
             [(-0.03, 0.052, 0.052), (0.04, 0.058, 0.058), (0.2, 0.047, 0.047), (0.31, 0.042, 0.042),
              (0.44, 0.04, 0.04), (0.57, 0.031, 0.031)])
        ankle = pos(side + "Foot")
        m.box(add(ankle, (-0.05, -ANKLE_HEIGHT, -0.07)), add(ankle, (0.05, 0.023, 0.138)), [(side + "Foot", 1.0)])
        m.box(add(ankle, (-0.047, -ANKLE_HEIGHT, 0.138)), add(ankle, (0.047, -0.04, 0.22)),
              [(side + "ToeBase", 1.0)])
        wrist = pos(side + "Hand")
        low, high = (0.0, -0.025, -0.04), (0.11, 0.025, 0.04)
        if sign < 0.0:
            low, high = (-0.11, -0.025, -0.04), (0.0, 0.025, 0.04)
        m.box(add(wrist, low), add(wrist, high), [(side + "Hand", 1.0)])
    return m


def inverse_bind(name):
    """The inverse of the joint's rest model matrix, column-major."""
    p, q = REST[name]
    inv = qconj(q)
    x, y, z = qrot(inv, (1.0, 0.0, 0.0)), qrot(inv, (0.0, 1.0, 0.0)), qrot(inv, (0.0, 0.0, 1.0))
    t = mul(qrot(inv, p), -1.0)
    return [x[0], x[1], x[2], 0.0, y[0], y[1], y[2], 0.0, z[0], z[1], z[2], 0.0, t[0], t[1], t[2], 1.0]


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    w = Writer()
    build = build_mesh()
    attributes = {
        "POSITION": w.accessor([rounded(p) for p in build.positions], "VEC3", minmax=True),
        "NORMAL": w.accessor([rounded(n) for n in build.normals], "VEC3"),
        "JOINTS_0": w.accessor(build.joints, "VEC4", 5121),
        "WEIGHTS_0": w.accessor([rounded(x) for x in build.weights], "VEC4"),
    }
    indices = w.accessor(build.indices, "SCALAR", 5125)
    nodes = []
    for name, parent, offset, rotation in JOINTS:
        node = {"name": name, "translation": rounded(offset)}
        if rotation != IDENTITY:
            node["rotation"] = rounded((rotation[1], rotation[2], rotation[3], rotation[0]))
        children = [i for i, (_, p, _, _) in enumerate(JOINTS) if p == name]
        if children:
            node["children"] = children
        nodes.append(node)
    body = len(nodes)
    nodes.append({"name": "body", "mesh": 0, "skin": 0})
    doc = {
        "asset": {"version": "2.0", "generator": "tools/assets/make_demo_strider.py"},
        "scene": 0,
        "scenes": [{"nodes": [INDEX["Root"], body]}],
        "nodes": nodes,
        "meshes": [{"name": "strider", "primitives": [{"attributes": attributes, "indices": indices,
                                                       "material": 0}]}],
        "materials": [{"name": "strider", "pbrMetallicRoughness": {"baseColorFactor": [0.2, 0.33, 0.5, 1.0],
                                                                     "metallicFactor": 0.0,
                                                                     "roughnessFactor": 0.5}}],
        "skins": [{"name": "strider", "joints": list(range(len(JOINTS))), "skeleton": INDEX["Root"],
                   "inverseBindMatrices": w.accessor([rounded(inverse_bind(n)) for n, _, _, _ in JOINTS], "MAT4")}],
        "accessors": w.accessors,
        "bufferViews": w.views,
        "buffers": [{"byteLength": len(w.blob), "uri": "strider.bin"}],
    }
    with open(os.path.join(out, "strider.bin"), "wb") as f:
        f.write(bytes(w.blob))
    with open(os.path.join(out, "strider.gltf"), "w", newline="\n") as f:
        json.dump(doc, f, indent=1)
        f.write("\n")
    with open(os.path.join(out, "strider.retarget.json"), "w", newline="\n") as f:
        json.dump(RETARGET, f, indent=2)
        f.write("\n")
    print(f"strider: {len(build.positions)} vertices, {len(build.indices) // 3} triangles, {len(JOINTS)} joints, "
          f"{len(w.blob)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
