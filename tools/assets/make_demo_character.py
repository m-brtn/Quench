#!/usr/bin/env python3
"""Writes the engine demo's animated character: a jointed mannequin as glTF (one skin of 20 joints, one material) with
its clips, and the clip settings quench_cook reads next to it. Procedural, CC0 (see examples/demo/assets/CREDITS.md);
standard library only. The demo's build runs it (examples/demo/CMakeLists.txt) and cooks the output; nothing it
writes is committed.

    python3 tools/assets/make_demo_character.py <output directory>

writes mannequin.gltf, mannequin.bin and mannequin.anim.json. Meters, Y up, the character faces +Z (its left is +X),
every joint's rest rotation is the identity. The clips:
    idle  3 s loop      breathing and a slow sway
    walk  0.9 s loop    1.1 m/s by root motion on "root", footstep events
    run   2/3 s loop    2.7 m/s by root motion, footstep events
    jump  1.3 s         crouch, flight, landing (a "land" event)
    wave  2 s           the right arm (for a masked layer)
    lean  0.5 s         a lean to the left, additive (the first frame is the rest pose)
    walk_back  0.9 s loop    0.9 m/s backward, footstep events      (the 2D blend space's points, at their
    strafe_l   0.7 s loop    0.75 m/s to the left (+X), footsteps    velocities; the feet land at the same
    strafe_r   0.7 s loop    0.75 m/s to the right, footsteps        phases as in walk: 0.1 and 0.6)
The legs of every clip are solved by two-bone IK from foot paths, so feet in contact with the ground stay where they
are while root motion carries the body (no foot sliding by construction).
"""

import json
import math
import os
import struct
import sys

FPS = 30
TAU = 2.0 * math.pi

# name, parent, translation relative to the parent
JOINTS = [
    ("root", None, (0.0, 0.0, 0.0)),
    ("pelvis", "root", (0.0, 1.0, 0.0)),
    ("spine", "pelvis", (0.0, 0.12, 0.0)),
    ("chest", "spine", (0.0, 0.2, 0.0)),
    ("neck", "chest", (0.0, 0.22, 0.0)),
    ("head", "neck", (0.0, 0.1, 0.0)),
    ("upperarm_l", "chest", (0.19, 0.17, 0.0)),
    ("lowerarm_l", "upperarm_l", (0.0, -0.28, 0.0)),
    ("hand_l", "lowerarm_l", (0.0, -0.25, 0.0)),
    ("upperarm_r", "chest", (-0.19, 0.17, 0.0)),
    ("lowerarm_r", "upperarm_r", (0.0, -0.28, 0.0)),
    ("hand_r", "lowerarm_r", (0.0, -0.25, 0.0)),
    ("thigh_l", "pelvis", (0.1, -0.06, 0.0)),
    ("shin_l", "thigh_l", (0.0, -0.44, 0.0)),
    ("foot_l", "shin_l", (0.0, -0.42, 0.0)),
    ("toe_l", "foot_l", (0.0, -0.05, 0.12)),
    ("thigh_r", "pelvis", (-0.1, -0.06, 0.0)),
    ("shin_r", "thigh_r", (0.0, -0.44, 0.0)),
    ("foot_r", "shin_r", (0.0, -0.42, 0.0)),
    ("toe_r", "foot_r", (0.0, -0.05, 0.12)),
]
INDEX = {name: i for i, (name, _, _) in enumerate(JOINTS)}
THIGH, SHIN = 0.44, 0.42
ANKLE_HEIGHT = 0.08


# --- vectors and quaternions (w, x, y, z) -------------------------------------------------------------------------

def add(a, b):
    return tuple(x + y for x, y in zip(a, b))


def sub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def mul(a, s):
    return tuple(x * s for x in a)


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def length(a):
    return math.sqrt(dot(a, a))


def normalize(a):
    n = length(a)
    return mul(a, 1.0 / n) if n > 1e-12 else a


def qmul(a, b):
    aw, ax, ay, az = a
    bw, bx, by, bz = b
    return (aw * bw - ax * bx - ay * by - az * bz, aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx, aw * bz + ax * by - ay * bx + az * bw)


def qconj(q):
    return (q[0], -q[1], -q[2], -q[3])


def qaxis(axis, angle):
    s = math.sin(angle / 2.0)
    return (math.cos(angle / 2.0), axis[0] * s, axis[1] * s, axis[2] * s)


def rx(a):
    return qaxis((1.0, 0.0, 0.0), a)


def ry(a):
    return qaxis((0.0, 1.0, 0.0), a)


def rz(a):
    return qaxis((0.0, 0.0, 1.0), a)


def qrot(q, v):
    p = qmul(qmul(q, (0.0, v[0], v[1], v[2])), qconj(q))
    return (p[1], p[2], p[3])


def qframe(x, y, z):
    """The rotation whose columns are the orthonormal axes x, y, z."""
    m00, m11, m22 = x[0], y[1], z[2]
    trace = m00 + m11 + m22
    if trace > 0.0:
        s = math.sqrt(trace + 1.0) * 2.0
        return (0.25 * s, (y[2] - z[1]) / s, (z[0] - x[2]) / s, (x[1] - y[0]) / s)
    if m00 > m11 and m00 > m22:
        s = math.sqrt(1.0 + m00 - m11 - m22) * 2.0
        return ((y[2] - z[1]) / s, 0.25 * s, (y[0] + x[1]) / s, (z[0] + x[2]) / s)
    if m11 > m22:
        s = math.sqrt(1.0 + m11 - m00 - m22) * 2.0
        return ((z[0] - x[2]) / s, (y[0] + x[1]) / s, 0.25 * s, (z[1] + y[2]) / s)
    s = math.sqrt(1.0 + m22 - m00 - m11) * 2.0
    return ((x[1] - y[0]) / s, (z[0] + x[2]) / s, (z[1] + y[2]) / s, 0.25 * s)


def smooth(t):
    t = min(1.0, max(0.0, t))
    return t * t * (3.0 - 2.0 * t)


def ease(keys, t):
    """Smooth interpolation through (time, value) keys, held before the first and after the last."""
    if t <= keys[0][0]:
        return keys[0][1]
    for (t0, v0), (t1, v1) in zip(keys, keys[1:]):
        if t <= t1:
            return v0 + (v1 - v0) * smooth((t - t0) / (t1 - t0))
    return keys[-1][1]


# --- poses --------------------------------------------------------------------------------------------------------

class Pose:
    """Local rotations per joint, translations of the root and the pelvis, set by the clip functions."""

    def __init__(self):
        self.rotation = {name: (1.0, 0.0, 0.0, 0.0) for name, _, _ in JOINTS}
        self.root = (0.0, 0.0, 0.0)
        self.pelvis = JOINTS[INDEX["pelvis"]][2]

    def solve_leg(self, side, ankle, pitch=0.0):
        """Thigh, shin and foot rotations that put the ankle at `ankle` (relative to the root joint, knee forward),
        the foot pitched by `pitch` (positive: toes down) in the root's frame."""
        pelvis_rotation = self.rotation["pelvis"]
        hip = add(self.pelvis, qrot(pelvis_rotation, JOINTS[INDEX["thigh_" + side]][2]))
        to_ankle = sub(ankle, hip)
        distance = min(max(length(to_ankle), 0.2), 0.999 * (THIGH + SHIN))
        direction = normalize(to_ankle)
        forward = (0.0, 0.0, 1.0)
        perpendicular = normalize(sub(forward, mul(direction, dot(forward, direction))))
        cos_hip = (THIGH * THIGH + distance * distance - SHIN * SHIN) / (2.0 * THIGH * distance)
        angle = math.acos(max(-1.0, min(1.0, cos_hip)))
        knee = add(hip, mul(add(mul(direction, math.cos(angle)), mul(perpendicular, math.sin(angle))), THIGH))
        end = add(hip, mul(direction, distance))

        def bone(a, b):
            down = normalize(sub(b, a))  # the bone points along -Y of its joint
            y = mul(down, -1.0)
            z = normalize(sub(forward, mul(y, dot(forward, y))))
            return qframe(cross(y, z), y, z)

        thigh_model = bone(hip, knee)
        shin_model = bone(knee, end)
        foot_model = rx(pitch)
        self.rotation["thigh_" + side] = qmul(qconj(pelvis_rotation), thigh_model)
        self.rotation["shin_" + side] = qmul(qconj(thigh_model), shin_model)
        self.rotation["foot_" + side] = qmul(qconj(shin_model), foot_model)

    def arm(self, side, swing, spread, bend, twist=0.0):
        """Shoulder swing (positive: forward), spread away from the body, elbow bend (positive: forearm forward)."""
        sign = 1.0 if side == "l" else -1.0
        self.rotation["upperarm_" + side] = qmul(rz(sign * spread), qmul(rx(-swing), ry(twist)))
        self.rotation["lowerarm_" + side] = rx(-bend)


# --- clips ---------------------------------------------------------------------------------------------------------

def idle(t):
    p = Pose()
    breath = math.sin(TAU * t / 3.0)
    sway = math.sin(TAU * t / 3.0 + 0.7)
    p.pelvis = (0.012 * sway, 0.975 + 0.004 * breath, 0.0)
    p.rotation["pelvis"] = rz(0.01 * sway)
    p.rotation["spine"] = rz(-0.012 * sway)
    p.rotation["chest"] = rx(-0.025 * breath)
    p.rotation["head"] = qmul(ry(0.05 * math.sin(TAU * t / 3.0 + 1.9)), rx(0.015 * breath))
    p.arm("l", 0.04 + 0.02 * breath, 0.1, 0.12)
    p.arm("r", 0.04 + 0.02 * breath, 0.1, 0.12)
    p.solve_leg("l", (0.11, ANKLE_HEIGHT, 0.03))
    p.solve_leg("r", (-0.11, ANKLE_HEIGHT, -0.03))
    return p


def gait(speed, period, duty, lift, height, bob, lean, arm_swing, elbow, running, direction=(0.0, 1.0), width=0.1,
         twist=1.0):
    """A walk or run cycle: each foot is in contact for `duty` of the period (the left from phase 0.1, the right
    from 0.6), moving back under the body at `speed` while root motion carries the body at `speed` along
    `direction` (x to the character's left, z forward; the character keeps facing +Z). The feet stand `width` to the
    sides; `twist` scales the hips' and shoulders' turn."""
    stance = speed * duty * period
    dx, dz = direction

    def foot(phase):
        if phase < duty:  # in contact: fixed in the world, so moving back relative to the root
            return 0.0, 0.5 * stance - stance * phase / duty, 0.0
        s = (phase - duty) / (1.0 - duty)
        z = -0.5 * stance + stance * (0.5 - 0.5 * math.cos(math.pi * s))
        y = lift * math.sin(math.pi * s) ** (0.7 if running else 1.0)
        return y, z, 0.32 * math.sin(TAU * s)

    def pose(t):
        p = Pose()
        phase = t / period
        p.root = (dx * speed * t, 0.0, dz * speed * t)
        if running:  # lowest at mid-stance
            y = height - bob * math.cos(TAU * (2.0 * (phase - 0.1) - duty))
        else:  # lowest when both feet are down
            y = height - bob * math.cos(TAU * 2.0 * (phase - 0.1))
        p.pelvis = (0.015 * math.cos(TAU * (phase - 0.1 - 0.5 * duty)), y, 0.0)
        turn = math.cos(TAU * (phase - 0.1))
        p.rotation["pelvis"] = ry(-0.09 * twist * turn)
        p.rotation["spine"] = rx(lean)
        p.rotation["chest"] = ry(0.16 * twist * turn)
        p.rotation["neck"] = ry(-0.07 * twist * turn)
        p.rotation["head"] = rx(-lean * 0.8)
        swing = math.cos(TAU * (phase - 0.6))
        p.arm("l", arm_swing * swing, 0.08, elbow + 0.15 * elbow * swing)
        p.arm("r", -arm_swing * swing, 0.08, elbow - 0.15 * elbow * swing)
        for side, offset, x in (("l", 0.1, width), ("r", 0.6, -width)):
            lift_y, along, pitch = foot((phase - offset) % 1.0)
            p.solve_leg(side, (x + dx * along, ANKLE_HEIGHT + lift_y, dz * along), dz * pitch)
        return p

    return pose


WALK = dict(speed=1.1, period=0.9, duty=0.6, lift=0.11, height=0.955, bob=0.025, lean=0.05, arm_swing=0.38,
            elbow=0.25, running=False)
RUN = dict(speed=2.7, period=2.0 / 3.0, duty=0.32, lift=0.24, height=0.92, bob=0.03, lean=0.18, arm_swing=0.7,
           elbow=1.4, running=True)
# For the 2D blend space (velocity x to the left, z forward): a slower walk backward, and side steps with the feet
# wider apart and a shorter stance, so that they do not cross.
WALK_BACK = dict(speed=0.9, period=0.9, duty=0.6, lift=0.09, height=0.955, bob=0.02, lean=-0.03, arm_swing=0.22,
                 elbow=0.25, running=False, direction=(0.0, -1.0), width=0.1, twist=0.6)
STRAFE = dict(speed=0.75, period=0.7, duty=0.55, lift=0.08, height=0.95, bob=0.02, lean=0.02, arm_swing=0.1,
              elbow=0.2, running=False, width=0.22, twist=0.3)


def jump(t):
    p = Pose()
    p.pelvis = (0.0, ease([(0.0, 0.975), (0.35, 0.8), (0.5, 0.995), (0.9, 0.97), (1.02, 0.82), (1.3, 0.975)], t), 0.0)
    flight = (t - 0.5) / 0.4
    air = 4.0 * flight * (1.0 - flight) if 0.0 < flight < 1.0 else 0.0
    p.root = (0.0, 0.28 * air, 0.0)
    tuck = math.sin(math.pi * flight) if 0.0 < flight < 1.0 else 0.0
    p.rotation["spine"] = rx(ease([(0.0, 0.0), (0.35, 0.35), (0.5, 0.0), (0.9, 0.05), (1.02, 0.3), (1.3, 0.0)], t))
    p.rotation["head"] = rx(ease([(0.0, 0.0), (0.35, -0.25), (0.5, 0.0), (1.02, -0.2), (1.3, 0.0)], t))
    swing = ease([(0.0, 0.04), (0.35, -0.8), (0.5, 2.4), (0.9, 1.2), (1.05, 0.3), (1.3, 0.04)], t)
    p.arm("l", swing, 0.15, 0.3)
    p.arm("r", swing, 0.15, 0.3)
    p.solve_leg("l", (0.11, ANKLE_HEIGHT + 0.2 * tuck, 0.03 + 0.05 * tuck), 0.3 * tuck)
    p.solve_leg("r", (-0.11, ANKLE_HEIGHT + 0.2 * tuck, -0.03 + 0.05 * tuck), 0.3 * tuck)
    return p


def wave(t):
    p = idle(t)
    raise_arm = ease([(0.0, 0.0), (0.4, 1.0), (1.6, 1.0), (2.0, 0.0)], t)
    shake = 0.4 * math.sin(TAU * 2.5 * (t - 0.4)) if 0.4 < t < 1.6 else 0.0
    p.rotation["upperarm_r"] = qmul(rz(-1.95 * raise_arm - 0.1 * (1.0 - raise_arm)), rx(-0.25 * raise_arm))
    p.rotation["lowerarm_r"] = rz(-(1.0 + shake) * raise_arm)
    p.rotation["hand_r"] = rz(-0.2 * raise_arm)
    return p


def lean(t):
    p = Pose()
    amount = t / 0.5
    p.rotation["spine"] = rz(-0.09 * amount)
    p.rotation["chest"] = rz(-0.09 * amount)
    p.rotation["neck"] = rz(0.08 * amount)
    return p


CLIPS = [
    ("idle", idle, 3.0),
    ("walk", gait(**WALK), WALK["period"]),
    ("run", gait(**RUN), RUN["period"]),
    ("jump", jump, 1.3),
    ("wave", wave, 2.0),
    ("lean", lean, 0.5),
    ("walk_back", gait(**WALK_BACK), WALK_BACK["period"]),
    ("strafe_l", gait(**STRAFE, direction=(1.0, 0.0)), STRAFE["period"]),
    ("strafe_r", gait(**STRAFE, direction=(-1.0, 0.0)), STRAFE["period"]),
]


def steps(period):
    """Root motion and the footstep events of a gait clip (the left foot lands at phase 0.1, the right at 0.6)."""
    return {"loop": True, "rootMotion": {"joint": "root"},
            "events": [{"time": round(0.1 * period, 4), "name": "footstep", "value": 0},
                       {"time": round(0.6 * period, 4), "name": "footstep", "value": 1}]}


SETTINGS = {
    "clips": {
        "idle": {"loop": True},
        "walk": steps(WALK["period"]),
        "run": steps(RUN["period"]),
        "jump": {"loop": False, "events": [{"time": 0.9, "name": "land", "value": 0}]},
        "wave": {"loop": False},
        "lean": {"loop": False, "additive": True},
        "walk_back": steps(WALK_BACK["period"]),
        "strafe_l": steps(STRAFE["period"]),
        "strafe_r": steps(STRAFE["period"]),
    }
}


# --- mesh ----------------------------------------------------------------------------------------------------------

def rest_positions():
    out = {}
    for name, parent, offset in JOINTS:
        out[name] = add(out[parent], offset) if parent else offset
    return out


REST = rest_positions()


class Mesh:
    def __init__(self):
        self.positions, self.normals, self.joints, self.weights, self.indices = [], [], [], [], []

    def vertex(self, position, normal, influences):
        """influences: (joint name, weight) pairs, at most four."""
        joints = [INDEX[name] for name, _ in influences] + [0] * (4 - len(influences))
        weights = [w for _, w in influences] + [0.0] * (4 - len(influences))
        total = sum(weights)
        self.positions.append(position)
        self.normals.append(normalize(normal))
        self.joints.append(joints)
        self.weights.append([w / total for w in weights])
        return len(self.positions) - 1

    def triangle(self, a, b, c):
        """Wound counterclockwise seen from outside (the vertex normals' side)."""
        face = cross(sub(self.positions[b], self.positions[a]), sub(self.positions[c], self.positions[a]))
        if dot(face, add(add(self.normals[a], self.normals[b]), self.normals[c])) < 0.0:
            b, c = c, b
        self.indices += [a, b, c]

    def tube(self, rings, sides=12):
        """rings: (center, radius x, radius z, influences); the axis is Y, the ends are capped."""
        first = len(self.positions)
        for center, rx_, rz_, influences in rings:
            for s in range(sides):
                a = TAU * s / sides
                c, d = math.cos(a), math.sin(a)
                self.vertex(add(center, (rx_ * c, 0.0, rz_ * d)), (c / rx_, 0.0, d / rz_), influences)
        for r in range(len(rings) - 1):
            for s in range(sides):
                a, b = first + r * sides + s, first + r * sides + (s + 1) % sides
                self.triangle(a, b, b + sides)
                self.triangle(a, b + sides, a + sides)
        for r, sign in ((0, -1.0), (len(rings) - 1, 1.0)):
            if rings[0][0][1] > rings[-1][0][1]:
                sign = -sign
            center = self.vertex(rings[r][0], (0.0, sign, 0.0), rings[r][3])
            for s in range(sides):
                self.triangle(center, first + r * sides + s, first + r * sides + (s + 1) % sides)

    def box(self, low, high, influences):
        for axis in range(3):
            for sign in (-1.0, 1.0):
                normal = [0.0, 0.0, 0.0]
                normal[axis] = sign
                u, v = (axis + 1) % 3, (axis + 2) % 3
                corners = []
                for cu, cv in ((0, 0), (1, 0), (1, 1), (0, 1)):
                    p = [0.0, 0.0, 0.0]
                    p[axis] = high[axis] if sign > 0 else low[axis]
                    p[u] = high[u] if cu else low[u]
                    p[v] = high[v] if cv else low[v]
                    corners.append(self.vertex(tuple(p), tuple(normal), influences))
                self.triangle(corners[0], corners[1], corners[2])
                self.triangle(corners[0], corners[2], corners[3])

    def sphere(self, center, radius, influences, rings=10, sides=14):
        first = len(self.positions)
        for r in range(rings + 1):
            polar = math.pi * r / rings
            for s in range(sides):
                a = TAU * s / sides
                n = (math.sin(polar) * math.cos(a), math.cos(polar), math.sin(polar) * math.sin(a))
                self.vertex(add(center, mul(n, radius)), n, influences)
        for r in range(rings):
            for s in range(sides):
                a, b = first + r * sides + s, first + r * sides + (s + 1) % sides
                if r > 0:
                    self.triangle(a, b, b + sides)
                if r < rings - 1:
                    self.triangle(a, b + sides, a + sides)


def blend(a, b, t):
    """Influences between joints a and b: t = 0 all a, t = 1 all b."""
    t = smooth(t)
    if t <= 0.0:
        return [(a, 1.0)]
    if t >= 1.0:
        return [(b, 1.0)]
    return [(a, 1.0 - t), (b, t)]


def limb(mesh, joints, profile, blend_length=0.07):
    """A tube down a chain of joints (rest positions straight below each other). profile: (distance from the first
    joint, radius x, radius z); a vertex near a joint is shared by the bones on both sides of it."""
    top = REST[joints[0]]
    bounds = [REST[j][1] for j in joints]
    rings = []
    for distance, rx_, rz_ in profile:
        y = top[1] - distance
        center = (top[0], y, top[2])
        influences = [(joints[0], 1.0)]
        for k in range(1, len(joints) - 1):
            if y <= bounds[k] + blend_length:
                influences = blend(joints[k - 1], joints[k], (bounds[k] + blend_length - y) / (2.0 * blend_length))
        rings.append((center, rx_, rz_, influences))
    mesh.tube(rings)


def build_mesh():
    m = Mesh()
    # Torso: from below the hips to the neck, the pelvis, spine and chest blended at their joints.
    torso = [(0.86, 0.13, 0.1), (0.94, 0.16, 0.11), (1.02, 0.155, 0.105), (1.1, 0.13, 0.095), (1.18, 0.125, 0.09),
             (1.26, 0.14, 0.1), (1.34, 0.165, 0.11), (1.42, 0.175, 0.115), (1.48, 0.165, 0.11), (1.53, 0.11, 0.08),
             (1.56, 0.05, 0.05), (1.66, 0.045, 0.045)]
    rings = []
    for y, rx_, rz_ in torso:
        if y < REST["spine"][1] - 0.04:
            influences = [("pelvis", 1.0)]
        elif y < REST["chest"][1] - 0.05:
            influences = blend("pelvis", "spine", (y - REST["spine"][1] + 0.04) / 0.08)
        elif y < REST["neck"][1] - 0.02:
            influences = blend("spine", "chest", (y - REST["chest"][1] + 0.05) / 0.1)
        else:
            influences = blend("chest", "neck", (y - REST["neck"][1] + 0.02) / 0.06)
        rings.append(((0.0, y, 0.0), rx_, rz_, influences))
    m.tube(rings)
    # Head, with a nose so that its facing shows.
    head = add(REST["head"], (0.0, 0.11, 0.01))
    m.sphere(head, 0.11, [("head", 1.0)])
    m.box(add(head, (-0.015, -0.03, 0.095)), add(head, (0.015, 0.0, 0.135)), [("head", 1.0)])
    for side in ("l", "r"):
        limb(m, ["thigh_" + side, "shin_" + side, "foot_" + side],
             [(-0.02, 0.085, 0.085), (0.05, 0.09, 0.09), (0.2, 0.08, 0.08), (0.36, 0.062, 0.062),
              (0.44, 0.058, 0.058), (0.52, 0.062, 0.062), (0.62, 0.064, 0.064), (0.76, 0.048, 0.048),
              (0.86, 0.042, 0.042)])
        limb(m, ["upperarm_" + side, "lowerarm_" + side, "hand_" + side],
             [(-0.03, 0.05, 0.05), (0.04, 0.055, 0.055), (0.18, 0.045, 0.045), (0.28, 0.04, 0.04),
              (0.4, 0.038, 0.038), (0.52, 0.03, 0.03)])
        ankle = REST["foot_" + side]
        m.box(add(ankle, (-0.045, -ANKLE_HEIGHT, -0.06)), add(ankle, (0.045, 0.02, 0.12)), [("foot_" + side, 1.0)])
        m.box(add(ankle, (-0.042, -ANKLE_HEIGHT, 0.12)), add(ankle, (0.042, -0.035, 0.19)), [("toe_" + side, 1.0)])
        wrist = REST["hand_" + side]
        m.box(add(wrist, (-0.022, -0.1, -0.035)), add(wrist, (0.022, 0.0, 0.035)), [("hand_" + side, 1.0)])
    return m


# --- glTF ----------------------------------------------------------------------------------------------------------

class Writer:
    def __init__(self):
        self.blob = bytearray()
        self.views, self.accessors = [], []

    def accessor(self, values, kind, component=5126, minmax=False):
        """component 5126: float, 5121: unsigned byte, 5125: unsigned int."""
        while len(self.blob) % 4:
            self.blob.append(0)
        width = {"SCALAR": 1, "VEC3": 3, "VEC4": 4, "MAT4": 16}[kind]
        fmt = {5126: "f", 5121: "B", 5125: "I"}[component]
        start = len(self.blob)
        for v in values:
            self.blob += struct.pack("<" + fmt * width, *(v if width > 1 else [v]))
        self.views.append({"buffer": 0, "byteOffset": start, "byteLength": len(self.blob) - start})
        accessor = {"bufferView": len(self.views) - 1, "componentType": component, "count": len(values),
                    "type": kind}
        if minmax:
            rows = [v if width > 1 else [v] for v in values]
            accessor["min"] = [min(v[i] for v in rows) for i in range(width)]
            accessor["max"] = [max(v[i] for v in rows) for i in range(width)]
        self.accessors.append(accessor)
        return len(self.accessors) - 1


def rounded(values, digits=6):
    return [round(v, digits) + 0.0 for v in values]


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
        "NORMAL": w.accessor([rounded(n) for n in mesh.normals], "VEC3"),
        "JOINTS_0": w.accessor(mesh.joints, "VEC4", 5121),
        "WEIGHTS_0": w.accessor([rounded(x) for x in mesh.weights], "VEC4"),
    }
    indices = w.accessor(mesh.indices, "SCALAR", 5125)

    nodes = []
    for name, parent, offset in JOINTS:
        node = {"name": name, "translation": list(offset)}
        children = [i for i, (_, p, _) in enumerate(JOINTS) if p == name]
        if children:
            node["children"] = children
        nodes.append(node)
    body = len(nodes)
    nodes.append({"name": "body", "mesh": 0, "skin": 0})
    inverse_bind = []
    for name, _, _ in JOINTS:
        x, y, z = REST[name]
        inverse_bind.append([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -x, -y, -z, 1])

    animations = []
    for name, fn, duration in CLIPS:
        frames = int(round(duration * FPS))
        times = [round(min(duration, k / FPS), 6) for k in range(frames + 1)]
        poses = [fn(t) for t in times]
        time_accessor = w.accessor(times, "SCALAR", minmax=True)
        samplers, channels = [], []

        def channel(node, path, values):
            samplers.append({"input": time_accessor, "output": w.accessor(values, "VEC4" if path == "rotation"
                                                                          else "VEC3"), "interpolation": "LINEAR"})
            channels.append({"sampler": len(samplers) - 1, "target": {"node": node, "path": path}})

        channel(INDEX["root"], "translation", [rounded(p.root) for p in poses])
        channel(INDEX["pelvis"], "translation", [rounded(p.pelvis) for p in poses])
        for joint, (joint_name, _, _) in enumerate(JOINTS):
            rotations = [p.rotation[joint_name] for p in poses]
            if all(abs(q[0] - 1.0) < 1e-9 for q in rotations):
                continue
            for k in range(1, len(rotations)):  # one hemisphere, for interpolation
                if dot(rotations[k], rotations[k - 1]) < 0.0:
                    rotations[k] = mul(rotations[k], -1.0)
            channel(joint, "rotation", [rounded((q[1], q[2], q[3], q[0])) for q in rotations])
        animations.append({"name": name, "samplers": samplers, "channels": channels})

    doc = {
        "asset": {"version": "2.0", "generator": "tools/assets/make_demo_character.py"},
        "scene": 0,
        "scenes": [{"nodes": [INDEX["root"], body]}],
        "nodes": nodes,
        "meshes": [{"name": "mannequin", "primitives": [{"attributes": attributes, "indices": indices,
                                                         "material": 0}]}],
        "materials": [{"name": "mannequin", "pbrMetallicRoughness": {"baseColorFactor": [0.5, 0.31, 0.17, 1.0],
                                                                       "metallicFactor": 0.0,
                                                                       "roughnessFactor": 0.55}}],
        "skins": [{"name": "mannequin", "joints": list(range(len(JOINTS))), "skeleton": INDEX["root"],
                   "inverseBindMatrices": w.accessor(inverse_bind, "MAT4")}],
        "animations": animations,
        "accessors": w.accessors,
        "bufferViews": w.views,
        "buffers": [{"byteLength": len(w.blob), "uri": "mannequin.bin"}],
    }
    with open(os.path.join(out, "mannequin.bin"), "wb") as f:
        f.write(bytes(w.blob))
    with open(os.path.join(out, "mannequin.gltf"), "w", newline="\n") as f:
        json.dump(doc, f, indent=1)
        f.write("\n")
    with open(os.path.join(out, "mannequin.anim.json"), "w", newline="\n") as f:
        json.dump(SETTINGS, f, indent=2)
        f.write("\n")
    print(f"mannequin: {len(mesh.positions)} vertices, {len(mesh.indices) // 3} triangles, {len(JOINTS)} joints, "
          f"{len(CLIPS)} clips, {len(w.blob)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
