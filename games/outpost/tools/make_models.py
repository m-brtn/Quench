"""Models the outpost and its walker in Blender and writes them as GLB files for the cook.

    /Applications/Blender.app/Contents/MacOS/Blender -b --python tools/make_models.py -- [--preview FILE.png]

Run in background mode (-b: no window). Everything is built from code, so the result is reproducible: boxes, lofts
and extruded profiles with bevels, a few booleans, flat-colored materials. Written files:

    assets_src/models/base.glb          the whole static outpost on its tile
    assets_src/models/pod_glow.glb      the lit lens of a hangar pod (shown by the game when the pod is charged)
    assets_src/models/walker_body.glb   the walker: body, and one thigh, shin and foot (used for both legs)
    assets_src/models/walker_thigh.glb
    assets_src/models/walker_shin.glb
    assets_src/models/walker_foot.glb
    assets_src/models/cell.glb          an energy cell
    assets_src/models/<name>_outline.glb  for each of them but the lens: its black outline shell (an inverted hull)
    assets/level.json                   places the game needs (lamps, pods, the door, blockers), in game coordinates

Blender is Z up with -Y toward the viewer; the exporter turns that into the engine's Y up (game x = x, y = z,
z = -y). Units are meters.
"""

import json
import math
import os
import sys

import bmesh
import bpy
from mathutils import Euler, Matrix, Vector

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODEL_DIR = os.path.join(ROOT, "assets_src", "models")
LEVEL_FILE = os.path.join(ROOT, "assets", "level.json")
rad = math.radians

# ---------------------------------------------------------------------------------------------------------------------
# Materials


def srgb(hexcolor):
    def lin(c):
        return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4

    h = hexcolor.lstrip("#")
    return tuple(lin(int(h[i:i + 2], 16) / 255.0) for i in (0, 2, 4))


MATERIALS = {}


def material(name, hexcolor, rough=0.6, metal=0.0, emit=None, strength=0.0):
    if name in MATERIALS:
        return MATERIALS[name]
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    bsdf = next(n for n in m.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
    bsdf.inputs["Base Color"].default_value = (*srgb(hexcolor), 1.0)
    bsdf.inputs["Roughness"].default_value = rough
    bsdf.inputs["Metallic"].default_value = metal
    if emit:
        bsdf.inputs["Emission Color"].default_value = (*srgb(emit), 1.0)
        bsdf.inputs["Emission Strength"].default_value = strength
    MATERIALS[name] = m
    return m


def materials():
    M = type("M", (), {})()
    M.armor = material("armor", "#E6E2D2", 0.55)
    M.armor2 = material("armor_shade", "#CFCBBB", 0.6)
    M.green = material("green", "#5B7C49", 0.6)
    M.green2 = material("green_dark", "#44613A", 0.65)
    M.slate = material("slate", "#2C313C", 0.5, 0.2)
    M.slate2 = material("slate_light", "#474E5C", 0.5, 0.2)
    M.deck = material("deck", "#383D48", 0.7)
    M.metal = material("metal", "#8B9099", 0.45, 0.6)
    M.metal2 = material("metal_dark", "#565C68", 0.45, 0.6)
    M.concrete = material("concrete", "#A7A9AD", 0.85)
    M.concrete2 = material("concrete_dark", "#85888E", 0.85)
    M.wall = material("wall", "#989BA1", 0.8)
    M.grass = material("grass", "#6B9A3A", 0.95)
    M.earth = material("earth", "#51402F", 0.95)
    M.dirt = material("dirt", "#C1743C", 0.95)
    M.red = material("red", "#CF4338", 0.55)
    M.yellow = material("yellow", "#E7B32C", 0.55)
    M.blue = material("barrel_blue", "#3C6CA6", 0.45, 0.3)
    M.cream = material("cream", "#D6CAA0", 0.7)
    M.glass = material("glass", "#93AEC2", 0.15, 0.1)
    M.pod = material("pod", "#3A6F8E", 0.35, 0.3)
    M.bay = material("bay", "#3B4556", 0.6, 0.1)
    M.lens = material("lens_dark", "#16242F", 0.12, 0.2)
    M.cyan = material("glow_cyan", "#0C2A33", 0.4, 0.0, "#4FDBFF", 7.0)
    M.cyan_soft = material("glow_cyan_soft", "#17333B", 0.4, 0.0, "#8FE9FF", 2.5)
    M.amber = material("glow_amber", "#3A2A10", 0.4, 0.0, "#FFC36B", 3.0)
    M.redlight = material("glow_red", "#3A1010", 0.4, 0.0, "#FF4A3A", 5.0)
    M.bush = material("bush", "#3F7A33", 0.95)
    M.bush2 = material("bush_light", "#56923C", 0.95)
    M.trunk = material("trunk", "#5A4632", 0.9)
    M.navy = material("walker_blue", "#2D4C84", 0.45, 0.3)
    M.visor = material("visor", "#111C33", 0.08, 0.3)
    return M


# ---------------------------------------------------------------------------------------------------------------------
# Building blocks

FRAMES = [Matrix.Identity(4)]
COLLECTIONS = []


class frame:
    """Objects made inside are placed in this local frame (nested frames multiply)."""

    def __init__(self, loc=(0, 0, 0), rot=(0, 0, 0)):
        self.m = Matrix.LocRotScale(Vector(loc), Euler(rot), None)

    def __enter__(self):
        FRAMES.append(FRAMES[-1] @ self.m)

    def __exit__(self, *exc):
        FRAMES.pop()


class collection:
    def __init__(self, name):
        self.c = bpy.data.collections.new(name)
        bpy.context.scene.collection.children.link(self.c)

    def __enter__(self):
        COLLECTIONS.append(self.c)
        return self.c

    def __exit__(self, *exc):
        COLLECTIONS.pop()


def place(ob, loc, rot):
    COLLECTIONS[-1].objects.link(ob)
    ob.matrix_world = FRAMES[-1] @ Matrix.LocRotScale(Vector(loc), Euler(rot), None)


def finish(name, bm, mat, loc=(0, 0, 0), rot=(0, 0, 0), bevel=0.0, segs=2, organic=False, sharp=40.0,
           bevel_angle=40.0, recalc=True):
    """A mesh object from a bmesh. bevel > 0: beveled sharp edges with face-weighted normals (hard surface look);
    organic: all smooth; else smooth below `sharp` degrees."""
    if recalc:
        bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    me.materials.append(mat)
    ob = bpy.data.objects.new(name, me)
    place(ob, loc, rot)
    me.shade_smooth()
    if organic:
        return ob
    if bevel > 0.0:
        b = ob.modifiers.new("bevel", "BEVEL")
        b.width = bevel
        b.segments = segs
        b.limit_method = "ANGLE"
        b.angle_limit = rad(bevel_angle)
        w = ob.modifiers.new("normals", "WEIGHTED_NORMAL")
        w.keep_sharp = False
        w.weight = 60
    else:
        me.set_sharp_from_angle(angle=rad(sharp))
    return ob


def box(name, size, loc, mat, bevel=0.04, segs=2, rot=(0, 0, 0)):
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    for v in bm.verts:
        v.co.x *= size[0]
        v.co.y *= size[1]
        v.co.z *= size[2]
    return finish(name, bm, mat, loc, rot, bevel, segs)


def box2(name, lo, hi, mat, bevel=0.04, segs=2):
    """A box from its corners."""
    size = [hi[i] - lo[i] for i in range(3)]
    loc = [(hi[i] + lo[i]) * 0.5 for i in range(3)]
    return box(name, size, loc, mat, bevel, segs)


def cyl(name, r, h, loc, mat, rot=(0, 0, 0), segs=24, r2=None, bevel=0.0, bsegs=2):
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=True, cap_tris=False, segments=segs, radius1=r, radius2=r if r2 is None else r2,
                          depth=h)
    return finish(name, bm, mat, loc, rot, bevel, bsegs, bevel_angle=50.0)


def sphere(name, r, loc, mat, segs=32, rings=16, scale=(1, 1, 1), rot=(0, 0, 0)):
    bm = bmesh.new()
    bmesh.ops.create_uvsphere(bm, u_segments=segs, v_segments=rings, radius=r)
    for v in bm.verts:
        v.co.x *= scale[0]
        v.co.y *= scale[1]
        v.co.z *= scale[2]
    return finish(name, bm, mat, loc, rot, organic=True)


def sphere_cap(name, r, zmin, loc, mat, rot=(0, 0, 0), segs=32, rings=16):
    """The part of a sphere above zmin: a shell (open below, seen from outside only)."""
    bm = bmesh.new()
    bmesh.ops.create_uvsphere(bm, u_segments=segs, v_segments=rings, radius=r)
    bmesh.ops.delete(bm, geom=[v for v in bm.verts if v.co.z < zmin], context="VERTS")
    return finish(name, bm, mat, loc, rot, organic=True, recalc=False)


def ico(name, r, loc, mat, subdiv=1, scale=(1, 1, 1), rot=(0, 0, 0)):
    """A faceted blob (flat shaded)."""
    bm = bmesh.new()
    bmesh.ops.create_icosphere(bm, subdivisions=subdiv, radius=r)
    for v in bm.verts:
        v.co.x *= scale[0]
        v.co.y *= scale[1]
        v.co.z *= scale[2]
    return finish(name, bm, mat, loc, rot, sharp=1.0)


def torus(name, R, r, loc, mat, rot=(0, 0, 0), segs=32, rsegs=10):
    bm = bmesh.new()
    rings = []
    for i in range(segs):
        a = 2 * math.pi * i / segs
        ring = []
        for j in range(rsegs):
            b = 2 * math.pi * j / rsegs
            d = R + r * math.cos(b)
            ring.append(bm.verts.new((d * math.cos(a), d * math.sin(a), r * math.sin(b))))
        rings.append(ring)
    for i in range(segs):
        a, b = rings[i], rings[(i + 1) % segs]
        for j in range(rsegs):
            k = (j + 1) % rsegs
            bm.faces.new((a[j], b[j], b[k], a[k]))
    return finish(name, bm, mat, loc, rot, organic=True)


def loft(name, loops, mat, loc=(0, 0, 0), rot=(0, 0, 0), bevel=0.0, sharp=40.0, organic=False):
    """Loops of 3D points (the same count each) joined by quads, capped at both ends."""
    bm = bmesh.new()
    vs = [[bm.verts.new(p) for p in loop] for loop in loops]
    n = len(vs[0])
    for a, b in zip(vs, vs[1:]):
        for j in range(n):
            k = (j + 1) % n
            bm.faces.new((a[j], a[k], b[k], b[j]))
    bm.faces.new(list(reversed(vs[0])))
    bm.faces.new(vs[-1])
    return finish(name, bm, mat, loc, rot, bevel, sharp=sharp, organic=organic)


def prism(name, pts, z0, z1, mat, bevel=0.0, loc=(0, 0, 0), rot=(0, 0, 0)):
    """A footprint in x, y between two heights."""
    return loft(name, [[(x, y, z0) for x, y in pts], [(x, y, z1) for x, y in pts]], mat, loc, rot, bevel)


def extrude_x(name, pts, x0, x1, mat, bevel=0.0, loc=(0, 0, 0), rot=(0, 0, 0)):
    """A profile in y, z between two x."""
    return loft(name, [[(x0, y, z) for y, z in pts], [(x1, y, z) for y, z in pts]], mat, loc, rot, bevel)


def extrude_y(name, pts, y0, y1, mat, bevel=0.0, loc=(0, 0, 0), rot=(0, 0, 0)):
    """A profile in x, z between two y."""
    return loft(name, [[(x, y0, z) for x, z in pts], [(x, y1, z) for x, z in pts]], mat, loc, rot, bevel)


def to_mesh_object(name, ob, mat):
    """A curve or text object turned into a mesh object in its place."""
    bpy.context.view_layer.update()
    dg = bpy.context.evaluated_depsgraph_get()
    me = bpy.data.meshes.new_from_object(ob.evaluated_get(dg))
    me.name = name
    me.materials.clear()
    me.materials.append(mat)
    new = bpy.data.objects.new(name, me)
    for c in ob.users_collection:
        c.objects.link(new)
    new.matrix_world = ob.matrix_world.copy()
    data = ob.data
    bpy.data.objects.remove(ob)
    bpy.data.curves.remove(data)
    return new


def tube(name, pts, radius, mat, loc=(0, 0, 0), rot=(0, 0, 0), segs=4, res=8):
    """A round pipe through the points (a smooth curve, ends at the first and last point)."""
    cu = bpy.data.curves.new(name, "CURVE")
    cu.dimensions = "3D"
    cu.bevel_depth = radius
    cu.bevel_resolution = segs
    cu.use_fill_caps = True
    cu.resolution_u = res
    sp = cu.splines.new("NURBS")
    sp.points.add(len(pts) - 1)
    for p, co in zip(sp.points, pts):
        p.co = (*co, 1.0)
    sp.order_u = min(3, len(pts))
    sp.use_endpoint_u = True
    ob = bpy.data.objects.new(name, cu)
    place(ob, loc, rot)
    new = to_mesh_object(name, ob, mat)
    new.data.shade_smooth()
    return new


def text(name, body, size, loc, rot, mat, extrude=0.015):
    cu = bpy.data.curves.new(name, "FONT")
    cu.body = body
    cu.size = size
    cu.extrude = extrude
    cu.align_x = "CENTER"
    cu.align_y = "CENTER"
    ob = bpy.data.objects.new(name, cu)
    place(ob, loc, rot)
    new = to_mesh_object(name, ob, mat)
    new.data.shade_flat()
    return new


def cut(ob, cutter):
    """Subtracts the cutter (an object that is removed from the collections) from the object, before its bevel."""
    b = ob.modifiers.new("cut", "BOOLEAN")
    b.operation = "DIFFERENCE"
    b.object = cutter
    b.solver = "EXACT"
    ob.modifiers.move(len(ob.modifiers) - 1, 0)
    cutter.hide_render = True
    cutter.display_type = "WIRE"
    CUTTERS.append(cutter)


CUTTERS = []

# ---------------------------------------------------------------------------------------------------------------------
# 2D helpers


def round_poly(pts, r, segs=5, skip=()):
    """The polygon with its corners rounded by arcs of radius r (indices in `skip` stay sharp)."""
    out = []
    n = len(pts)
    for i in range(n):
        p = Vector(pts[i])
        if i in skip:
            out.append(tuple(p))
            continue
        d1 = (Vector(pts[i - 1]) - p).normalized()
        d2 = (Vector(pts[(i + 1) % n]) - p).normalized()
        theta = d1.angle(d2)
        t = min(r / math.tan(theta / 2), 0.45 * min((Vector(pts[i - 1]) - p).length,
                                                      (Vector(pts[(i + 1) % n]) - p).length))
        rr = t * math.tan(theta / 2)
        a = p + d1 * t
        b = p + d2 * t
        c = p + (d1 + d2).normalized() * (rr / math.sin(theta / 2))
        va, vb = a - c, b - c
        phi = va.angle(vb)
        for k in range(segs + 1):
            s = k / segs
            v = (math.sin((1 - s) * phi) * va + math.sin(s * phi) * vb) / math.sin(phi)
            out.append(tuple(c + v))
    return out


def offset_poly(pts, d):
    """A counter-clockwise polygon grown by d (shrunk for d < 0), mitered."""
    out = []
    n = len(pts)
    for i in range(n):
        p0, p1, p2 = Vector(pts[i - 1]), Vector(pts[i]), Vector(pts[(i + 1) % n])
        e1, e2 = (p1 - p0).normalized(), (p2 - p1).normalized()
        n1, n2 = Vector((e1.y, -e1.x)), Vector((e2.y, -e2.x))
        m = (n1 + n2) / (1.0 + n1.dot(n2))
        out.append(tuple(p1 + m * d))
    return out


def rrect(hx, hy, r, segs=6, cx=0.0, cy=0.0):
    """A rounded rectangle, counter-clockwise."""
    out = []
    for sx, sy, a0 in ((1, -1, -90), (1, 1, 0), (-1, 1, 90), (-1, -1, 180)):
        for k in range(segs + 1):
            a = rad(a0 + 90.0 * k / segs)
            out.append((cx + sx * (hx - r) + r * math.cos(a), cy + sy * (hy - r) + r * math.sin(a)))
    return out


def spans(poly, axis, c):
    """The intervals where the line x = c (axis 0) or y = c (axis 1) is inside the polygon."""
    hits = []
    n = len(poly)
    o = 1 - axis
    for i in range(n):
        a, b = poly[i], poly[(i + 1) % n]
        if (a[axis] - c) * (b[axis] - c) < 0:
            t = (c - a[axis]) / (b[axis] - a[axis])
            hits.append(a[o] + t * (b[o] - a[o]))
    hits.sort()
    return list(zip(hits[0::2], hits[1::2]))


def blob(r, n=14, seed=0.0, wobble=0.22):
    return [(r * (1 + wobble * math.sin(3 * a + seed) + 0.5 * wobble * math.sin(5 * a + 2.3 * seed)) * math.cos(a),
             r * (1 + wobble * math.sin(3 * a + seed) + 0.5 * wobble * math.sin(5 * a + 2.3 * seed)) * math.sin(a))
            for a in (2 * math.pi * i / n for i in range(n))]


# ---------------------------------------------------------------------------------------------------------------------
# The level's places for the game (Blender coordinates here, converted when written)

LEVEL = {"lamps": [], "pods": [], "blockers": [], "interior_lights": []}


def game_point(p):
    return [round(p[0], 3), round(p[2], 3), round(-p[1], 3)]


def game_rect(x0, y0, x1, y1):
    return [round(x0, 3), round(-y1, 3), round(x1, 3), round(-y0, 3)]


def raised(x0, y0, x1, y1, top, top_back=None):
    """Ground the walker steps over that is higher than the pad: its top at the front (toward -y) and at the back."""
    LEVEL.setdefault("raised", []).append({"rect": game_rect(x0, y0, x1, y1), "front_y": round(top, 3),
                                           "back_y": round(top if top_back is None else top_back, 3)})


def block(x0, y0, x1, y1):
    LEVEL["blockers"].append(game_rect(min(x0, x1), min(y0, y1), max(x0, x1), max(y0, y1)))


def world(p):
    return FRAMES[-1] @ Vector(p)


# ---------------------------------------------------------------------------------------------------------------------
# The outpost

TILE = (-24.0, -16.5, 27.0, 16.5)
PAD = [(-21, -9.5), (-18, -13), (7, -13), (9, -14), (22, -14), (25, -11.5), (25, 9), (22, 13.5), (-17, 13.5),
       (-21, 9.5)]
PAD_Z = 0.2
HX0, HX1 = -6.0, 16.0       # the hangar along x
HY0, HY1 = -1.0, 11.0       # front and back walls
WALL_TOP = 5.6
S0, S1 = (-0.7, 6.1), (5.3, 9.0)  # the front roof slope in y, z
ROOF_Z = 9.0
DOOR_X0, DOOR_X1 = -3.5, 12.5
FLOOR_Z = 0.5               # the hangar floor
DOOR_TOP = 4.8
RAMP_Y = -8.0               # the ramp's front edge
SLOPE_ANGLE = math.atan2(S1[1] - S0[1], S1[0] - S0[0])
HANGAR_PROFILE = [(HY0, 0.0), (HY1, 0.0), (HY1, 6.4), (8.3, ROOF_Z), S1, S0, (HY0, WALL_TOP)]  # counter-clockwise


def hangar_profile(grow=0.0):
    return round_poly(offset_poly(HANGAR_PROFILE, grow), 0.55, 5, skip=(0, 1))


def roof_band(name, x0, x1, mat, lift, zmin=5.9):
    """A strip across the roof that follows the hangar's profile, above zmin."""
    outer = [p for p in hangar_profile(lift) if p[1] >= zmin]
    inner = [p for p in hangar_profile(-0.1) if p[1] >= zmin]
    extrude_x(name, outer + list(reversed(inner)), x0, x1, mat)


def outline(name, w, h, t, mat, depth=0.02):
    """A thin rectangle outline in the local x, z plane (facing -y)."""
    for i, (sx, sz, px, pz) in enumerate(((w, t, 0, h / 2), (w, t, 0, -h / 2), (t, h, w / 2, 0), (t, h, -w / 2, 0))):
        box(f"{name}_{i}", (sx + (t if sz == t else 0), depth, sz), (px, 0, pz), mat, bevel=0)


def foot(name, M, size=(1.1, 1.0, 1.2)):
    """A buttress block at a wall's base: the local -y is outward, the origin on the ground at the wall."""
    sx, sy, sz = size
    box(name, (sx, sy, sz), (0, -sy / 2 + 0.1, sz / 2), M.armor, bevel=0.14)
    box(name + "_pad", (sx * 0.72, 0.08, sz * 0.5), (0, -sy + 0.08, sz * 0.52), M.green, bevel=0.03)
    box(name + "_sole", (sx + 0.12, sy + 0.1, 0.16), (0, -sy / 2 + 0.1, 0.08), M.metal2, bevel=0.03)


def ground(M):
    x0, y0, x1, y1 = TILE
    box2("tile_grass", (x0, y0, -0.35), (x1, y1, 0.0), M.grass, bevel=0.08)
    box2("tile_earth", (x0 + 0.15, y0 + 0.15, -1.6), (x1 - 0.15, y1 - 0.15, -0.3), M.earth, bevel=0.1)
    prism("pad_rim", offset_poly(PAD, 0.4), -0.05, 0.11, M.concrete2, bevel=0.03)
    prism("pad", PAD, 0.0, PAD_Z, M.concrete, bevel=0.035)
    # Slab seams.
    n = 0
    for x in range(-18, 25, 6):
        for a, b in spans(PAD, 0, x + 0.5):
            box2(f"seam_x{n}", (x + 0.46, a + 0.15, PAD_Z), (x + 0.54, b - 0.15, PAD_Z + 0.012), M.concrete2, bevel=0)
            n += 1
    for y in (-7.5, -1.5, 4.5, 10.5):
        for a, b in spans(PAD, 1, y):
            box2(f"seam_y{n}", (a + 0.15, y - 0.04, PAD_Z), (b - 0.15, y + 0.04, PAD_Z + 0.012), M.concrete2, bevel=0)
            n += 1
    # Bare earth around the pad.
    for i, (x, y, r, s) in enumerate(((-21.5, -11.8, 2.3, 0.4), (-14.5, -14.6, 1.5, 1.7), (12.5, -15.2, 1.6, 2.9),
                                      (25.6, -5.0, 1.3, 4.1), (24.6, 12.0, 2.0, 5.3), (-21.8, 12.6, 2.2, 0.9),
                                      (2.0, 15.0, 1.4, 3.3), (-22.6, 1.0, 1.2, 2.2))):
        prism(f"dirt{i}", [(x + px, y + py * 0.7) for px, py in blob(r, 16, s)], 0.0, 0.03, M.dirt)
    # Markings on the pad: "320" with an arrow, and a yellow line along the ramp's approach.
    z = PAD_Z + 0.006
    white = material("marking", "#DADCDD", 0.8)
    text("pad_number", "320", 1.7, (-13.6, -11.2, z), (0, 0, 0), white)
    prism("pad_arrow", [(-17.9, -11.2), (-16.6, -10.55), (-16.6, -11.85)], z, z + 0.015, white)
    box2("pad_bar", (-16.3, -11.9, z), (-16.1, -10.5, z + 0.015), white, bevel=0)
    for dy in (-0.25, 0.25):
        box2(f"pad_dash{dy}", (-10.9, -11.3 + dy, z), (-9.9, -11.12 + dy, z + 0.015), white, bevel=0)
    for i, x in enumerate((-2.5, 0.5, 3.5, 6.5, 9.5)):
        box2(f"approach{i}", (x, -10.6, z), (x + 1.8, -10.44, z + 0.015), M.yellow, bevel=0)


def hangar(M):
    body = extrude_x("hangar", hangar_profile(), HX0, HX1, M.armor, bevel=0.12)
    body.modifiers["bevel"].angle_limit = rad(55)
    cutter = box2("hangar_cut", (DOOR_X0, HY0 - 1.0, FLOOR_Z), (DOOR_X1, 8.8, DOOR_TOP), M.slate, bevel=0)
    cut(body, cutter)
    block(HX0, HY0 - 0.6, DOOR_X0, HY1)
    block(DOOR_X1, HY0 - 0.6, HX1, HY1)
    block(DOOR_X0, 0.2, DOOR_X1, HY1)

    # The dark lining of the bay and its floor.
    e = 0.04
    box2("bay_floor", (DOOR_X0, HY0, FLOOR_Z), (DOOR_X1, 8.8, FLOOR_Z + 0.06), M.deck, bevel=0)
    box2("bay_back", (DOOR_X0, 8.8 - e, FLOOR_Z), (DOOR_X1, 8.8, DOOR_TOP), M.bay, bevel=0)
    box2("bay_left", (DOOR_X0, HY0 + 0.3, FLOOR_Z), (DOOR_X0 + e, 8.8, DOOR_TOP), M.bay, bevel=0)
    box2("bay_right", (DOOR_X1 - e, HY0 + 0.3, FLOOR_Z), (DOOR_X1, 8.8, DOOR_TOP), M.bay, bevel=0)
    box2("bay_ceiling", (DOOR_X0, HY0 + 0.3, DOOR_TOP - e), (DOOR_X1, 8.8, DOOR_TOP), M.bay, bevel=0)
    for i in range(4):  # ribs and light bars on the back wall
        x = DOOR_X0 + 2.0 + i * 4.0
        box2(f"bay_rib{i}", (x + 1.8, 8.45, FLOOR_Z), (x + 2.2, 8.8, DOOR_TOP), M.slate2, bevel=0.04)
        box2(f"bay_light{i}", (x - 1.0, 8.7, 3.9), (x + 1.0, 8.78, 4.1), M.cyan_soft, bevel=0)
        LEVEL["interior_lights"].append(game_point((x, -0.2, 4.2)))
    for i in range(8):  # light dashes on the floor at the threshold
        x = DOOR_X0 + 1.0 + i * 2.0
        box2(f"floor_dash{i}", (x - 0.65, -0.55, FLOOR_Z + 0.06), (x + 0.65, -0.3, FLOOR_Z + 0.085), M.cyan, bevel=0)

    # The door frame, proud of the wall, with chamfered upper corners and small lights on its lintel.
    fy0, fy1 = HY0 - 0.5, HY0 + 0.1
    box2("frame_left", (DOOR_X0 - 1.1, fy0, 0.0), (DOOR_X0, fy1, DOOR_TOP + 0.75), M.armor, bevel=0.12)
    box2("frame_right", (DOOR_X1, fy0, 0.0), (DOOR_X1 + 1.1, fy1, DOOR_TOP + 0.75), M.armor, bevel=0.12)
    box2("frame_top", (DOOR_X0 - 1.1, fy0, DOOR_TOP), (DOOR_X1 + 1.1, fy1, DOOR_TOP + 0.75), M.armor, bevel=0.12)
    g = 0.9
    extrude_y("gusset_left", [(DOOR_X0, DOOR_TOP - g), (DOOR_X0 + g, DOOR_TOP), (DOOR_X0, DOOR_TOP)], fy0 + 0.03, fy1,
              M.armor)
    extrude_y("gusset_right", [(DOOR_X1, DOOR_TOP - g), (DOOR_X1, DOOR_TOP), (DOOR_X1 - g, DOOR_TOP)], fy0 + 0.03, fy1,
              M.armor)
    for i in range(7):
        x = DOOR_X0 + 1.4 + i * 2.2
        with frame((x, fy0 - 0.01, DOOR_TOP + 0.38)):
            box("lintel_light", (1.0, 0.04, 0.26), (0, 0, 0), M.amber, bevel=0)
            outline("lintel_line", 1.2, 0.42, 0.035, M.red)
    box2("lintel_red", (DOOR_X0 + 0.9, fy0 - 0.02, DOOR_TOP - 0.05), (DOOR_X1 - 0.9, fy0 + 0.05, DOOR_TOP + 0.02), M.red,
         bevel=0)
    for i, x in enumerate((DOOR_X0 - 0.55, DOOR_X1 + 0.55)):  # pillar details
        with frame((x, fy0 - 0.01, 0.0)):
            box(f"pillar_plate{i}", (0.7, 0.06, 1.5), (0, 0, 2.6), M.green, bevel=0.02)
            box(f"pillar_hazard{i}", (0.7, 0.05, 0.3), (0, 0, 1.2), M.yellow, bevel=0)
            box(f"pillar_lamp{i}", (0.3, 0.08, 0.5), (0, 0, 4.2), M.amber, bevel=0.02)
        with frame((x, fy0, 0.0)):
            foot(f"pillar_foot{i}", M, (1.3, 1.5, 1.0))
        block(x - 0.8, fy0 - 1.6, x + 0.8, fy0)

    # The front wall right of the door.
    with frame((14.8, HY0 - 0.01, 0.0)):
        box("front_panel", (1.5, 0.08, 2.6), (0, 0, 3.0), M.green, bevel=0.03)
        outline("front_panel_line", 1.9, 3.1, 0.04, M.red)
        foot("front_foot", M, (1.3, 0.9, 1.1))
    block(14.0, HY0 - 1.2, 15.6, HY0)

    # Panel seams and the two green stripes across the roof.
    for i, x in enumerate((-2.75, 1.65, 5.95, 10.25, 14.55)):
        roof_band(f"roof_seam{i}", x - 0.035, x + 0.035, M.armor2, 0.012, zmin=-10.0)
    roof_band("roof_stripe0", 14.8, 15.12, M.green, 0.03)
    roof_band("roof_stripe1", 15.36, 15.68, M.green, 0.03)
    roof_band("roof_stripe2", -5.6, -5.2, M.green, 0.03)

    # The right end wall: a grey panel in a green truss with two windows, feet along its base.
    inner = [(y, max(z, 1.3)) for y, z in offset_poly(HANGAR_PROFILE, -0.75)]
    x = HX1
    extrude_x("end_panel", inner, x, x + 0.06, M.slate2)
    ring = offset_poly(HANGAR_PROFILE, -0.55)
    ring = [(y, max(z, 1.05)) for y, z in ring]
    hole = [(y, max(z, 1.5)) for y, z in offset_poly(HANGAR_PROFILE, -0.98)]
    bm = bmesh.new()  # the green border: a ring between two outlines
    loops = [[bm.verts.new((xx, y, z)) for y, z in pts] for xx in (x, x + 0.24) for pts in (ring, hole)]
    o0, h0, o1, h1 = loops
    n = len(ring)
    for j in range(n):
        k = (j + 1) % n
        bm.faces.new((o0[j], o0[k], o1[k], o1[j]))
        bm.faces.new((h0[j], h0[k], h1[k], h1[j]))
        bm.faces.new((o1[j], o1[k], h1[k], h1[j]))
        bm.faces.new((o0[j], o0[k], h0[k], h0[j]))
    finish("end_border", bm, M.green, bevel=0.03)

    def beam(name, a, b, w=0.38, mat=M.green):
        (y0, z0), (y1, z1) = a, b
        length = math.hypot(y1 - y0, z1 - z0)
        box(name, (0.2, length, w), (x + 0.14, (y0 + y1) / 2, (z0 + z1) / 2), mat, bevel=0.03,
            rot=(math.atan2(z1 - z0, y1 - y0), 0, 0))

    beam("end_post0", (3.3, 1.4), (3.3, 7.9))
    beam("end_post1", (6.9, 1.4), (6.9, 8.4))
    beam("end_diag0", (3.45, 1.5), (6.75, 5.2))
    beam("end_diag1", (3.45, 5.2), (6.75, 1.5))
    beam("end_tie", (3.3, 5.2), (6.9, 5.2), 0.3)
    beam("end_diag2", (0.2, 5.3), (3.2, 7.6), 0.3)
    beam("end_diag3", (7.0, 8.0), (10.0, 5.6), 0.3)
    bolt = [(0.32, 0.95), (-0.3, 0.08), (0.02, 0.08), (-0.32, -0.95), (0.3, -0.02), (-0.02, -0.02)]
    for i, (y0, y1) in enumerate(((0.35, 3.0), (7.2, 9.85))):
        box2(f"end_window{i}", (x + 0.06, y0, 1.9), (x + 0.11, y1, 4.9), M.glass, bevel=0)
        c = ((y0 + y1) / 2, 3.4)
        extrude_x(f"end_bolt{i}", [(c[0] + py * 1.1, c[1] + pz * 1.1) for py, pz in bolt], x + 0.11, x + 0.13, M.yellow)
        with frame((x + 0.11, (y0 + y1) / 2, 3.4), (0, 0, rad(90))):
            outline(f"end_window_line{i}", y1 - y0 - 0.3, 2.7, 0.04, M.red)
    for i, y in enumerate((0.3, 3.5, 6.7, 9.9)):
        with frame((x, y, 0.0), (0, 0, rad(90))):
            foot(f"end_foot{i}", M, (1.5, 1.1, 1.25))
    block(HX1, HY0, HX1 + 1.3, HY1)

    # The back wall, seen when the camera turns: panels, feet and a duct.
    for i in range(5):
        with frame((HX0 + 2.6 + i * 4.2, HY1 + 0.01, 0.0), (0, 0, rad(180))):
            box(f"back_panel{i}", (2.6, 0.08, 2.8), (0, 0, 3.1), M.green if i % 2 == 0 else M.armor2, bevel=0.03)
            foot(f"back_foot{i}", M, (1.4, 0.9, 1.1))
    tube("back_duct", [(HX0 + 1.0, HY1 + 0.35, 5.2), (4.0, HY1 + 0.35, 5.2), (HX1 - 1.0, HY1 + 0.35, 5.2)], 0.22, M.metal)
    block(HX0, HY1, HX1, HY1 + 1.2)


def pods(M):
    r = 1.6
    for i in range(4):
        c = Vector((DOOR_X0 + 2.15 + i * 3.9, 1.9, FLOOR_Z + 0.06 + 0.4 + r))
        LEVEL["pods"].append(game_point(c))
        with frame(c):
            sphere(f"pod{i}", r, (0, 0, 0), M.pod)
            cyl(f"pod_stand{i}", 1.05, 0.45, (0, 0, -r - 0.18), M.slate2, bevel=0.05)
            torus(f"pod_belt{i}", r + 0.01, 0.06, (0, 0, -0.25), M.metal2)
            cyl(f"pod_lens{i}", 0.95, 0.08, (0, -1.3, 0.0), M.lens, rot=(rad(90), 0, 0), segs=32)
            torus(f"pod_ring{i}", 1.05, 0.1, (0, -1.22, 0.0), M.metal, rot=(rad(90), 0, 0))
            box(f"pod_clamp{i}", (0.7, 0.7, 1.0), (0, 0.2, r + 0.25), M.slate2, bevel=0.08)
            tube(f"pod_hose{i}", [(0.9, 0.9, r - 0.4), (1.4, 1.6, r + 0.3), (1.3, 2.4, DOOR_TOP - FLOOR_Z - r - 0.6)],
                 0.09, M.cream)


def pod_glow(M):
    cyl("glow_lens", 0.93, 0.05, (0, -1.345, 0.0), M.cyan_soft, rot=(rad(90), 0, 0), segs=32)
    torus("glow_ring", 0.55, 0.07, (0, -1.38, 0.0), M.cyan, rot=(rad(90), 0, 0))
    cyl("glow_core", 0.2, 0.05, (0, -1.385, 0.0), M.cyan, rot=(rad(90), 0, 0), segs=20)


def ramp(M):
    z1 = FLOOR_Z + 0.06
    z0 = PAD_Z + 0.09
    extrude_x("ramp", [(HY0, PAD_Z - 0.1), (HY0, z1), (RAMP_Y, z0), (RAMP_Y, PAD_Z - 0.1)], DOOR_X0, DOOR_X1, M.deck)
    for i, x in enumerate((DOOR_X0, DOOR_X1)):  # side curbs
        extrude_x(f"ramp_curb{i}", [(HY0 - 0.6, PAD_Z - 0.1), (HY0 - 0.6, z1 + 0.05), (RAMP_Y - 0.25, z0 + 0.1),
                                    (RAMP_Y - 0.25, PAD_Z - 0.1)], x - 0.28, x + 0.28, M.metal, bevel=0.04)
    box2("ramp_lip", (DOOR_X0 - 0.28, RAMP_Y - 0.5, PAD_Z - 0.05), (DOOR_X1 + 0.28, RAMP_Y, z0 + 0.02), M.metal,
         bevel=0.04)
    for i in range(9):  # teeth along the lip
        x = DOOR_X0 + 0.9 + i * 1.775
        box2(f"ramp_tooth{i}", (x - 0.5, RAMP_Y - 0.95, PAD_Z - 0.02), (x + 0.5, RAMP_Y - 0.45, PAD_Z + 0.3),
             M.armor if i % 2 == 0 else M.green, bevel=0.07)
    # Markings in the ramp's plane.
    slope = math.atan2(z1 - z0, HY0 - RAMP_Y)
    with frame((0, HY0, z1 + 0.004), (slope, 0, 0)):
        t = 0.022
        y_front = -(HY0 - RAMP_Y) / math.cos(slope) + 0.45
        box2("ramp_line_front", (DOOR_X0 + 0.7, y_front, 0), (DOOR_X1 - 0.7, y_front + 0.09, t), M.red, bevel=0)
        for i, x in enumerate((DOOR_X0 + 0.7, DOOR_X1 - 0.79)):
            box2(f"ramp_line_side{i}", (x, y_front, 0), (x + 0.09, -0.5, t), M.red, bevel=0)
        for i, x in enumerate((0.5, 8.5)):  # two "T" guides with dots
            box2(f"guide_stem{i}", (x - 0.06, -5.2, 0), (x + 0.06, -1.6, t), M.yellow, bevel=0)
            box2(f"guide_bar{i}", (x - 1.3, -1.6, 0), (x + 1.3, -1.48, t), M.yellow, bevel=0)
            for k in range(4):
                box2(f"guide_dot{i}_{k}", (x - 1.9 + k * 0.45, -3.1, 0), (x - 1.72 + k * 0.45, -2.92, t), M.yellow,
                     bevel=0)
        for k in range(7):
            x = DOOR_X0 + 2.2 + k * 2.0
            box2(f"ramp_dash{k}", (x, -6.2, 0), (x + 0.9, -6.08, t), M.yellow, bevel=0)
    LEVEL["ramp"] = {"rect": game_rect(DOOR_X0, RAMP_Y, DOOR_X1, HY0 + 1.6), "front_y": round(z0, 3),
                     "back_y": round(z1, 3), "front_z": -RAMP_Y, "back_z": -HY0}
    # What the walker steps over around the ramp, for things drawn on the ground (the click marker): the tops of the
    # curbs (sloped: at the rect's front and back), the lip and the teeth.
    for x in (DOOR_X0, DOOR_X1):
        raised(x - 0.28, RAMP_Y - 0.25, x + 0.28, HY0 - 0.6, z0 + 0.1, z1 + 0.05)
    raised(DOOR_X0 - 0.28, RAMP_Y - 0.5, DOOR_X1 + 0.28, RAMP_Y, z0 + 0.02)
    for i in range(9):
        x = DOOR_X0 + 0.9 + i * 1.775
        raised(x - 0.5, RAMP_Y - 0.95, x + 0.5, RAMP_Y - 0.45, PAD_Z + 0.3)


def vents(M):
    mid = ((S0[0] + S1[0]) / 2, (S0[1] + S1[1]) / 2)
    w, l = 3.8, 5.5
    for i, x in enumerate((-0.5, 3.8, 8.1, 12.4)):
        with frame((x, mid[0], mid[1] + 0.01), (SLOPE_ANGLE, 0, 0)):
            box(f"vent_base{i}", (w, l, 0.12), (0, 0, 0.05), M.slate, bevel=0.03)
            for k in range(9):
                box(f"vent_slat{i}_{k}", (w - 0.7, 0.3, 0.07), (0, -2.2 + k * 0.55, 0.14), M.slate2, bevel=0.02)
            for k, y in enumerate((-1.375, 1.375)):
                box(f"vent_red{i}_{k}", (w - 0.7, 0.05, 0.05), (0, y, 0.16), M.red, bevel=0)
            for k, sx in enumerate((-1, 1)):
                box(f"vent_side{i}_{k}", (0.32, l, 0.34), (sx * (w / 2 - 0.16), 0, 0.17), M.green, bevel=0.05)
                box(f"vent_end{i}_{k}", (w, 0.32, 0.34), (0, sx * (l / 2 - 0.16), 0.17), M.green, bevel=0.05)
                a = math.atan2(l - 0.7, w - 0.7)
                box(f"vent_cross{i}_{k}", (math.hypot(l - 0.7, w - 0.7), 0.3, 0.26), (0, 0, 0.2), M.green, bevel=0.04,
                    rot=(0, 0, sx * a))
            box(f"vent_hub{i}", (0.8, 0.8, 0.34), (0, 0, 0.2), M.green2, bevel=0.06)
            # The actuator at the upper edge.
            box(f"vent_motor{i}", (1.1, 0.55, 0.55), (0, l / 2 + 0.15, 0.3), M.metal, bevel=0.07)
            for k, sx in enumerate((-1, 1)):
                cyl(f"vent_ram{i}_{k}", 0.13, 0.9, (sx * 0.3, l / 2 + 0.55, 0.42), M.metal2, rot=(rad(90), 0, 0),
                    segs=12)
                cyl(f"vent_tip{i}_{k}", 0.15, 0.16, (sx * 0.3, l / 2 + 1.0, 0.42), M.yellow, rot=(rad(90), 0, 0),
                    segs=12)


def annex(M):
    """The left part of the building: a block beside the hangar, a tier on the roof and the command module."""
    box2("annex", (-10.0, 0.5, 0.0), (-5.5, 11.0, 8.4), M.armor, bevel=0.4, segs=3)
    block(-10.0, 0.5, -5.5, 11.0)
    with frame((-7.75, 0.49, 0.0)):
        box("annex_panel", (2.4, 0.08, 2.4), (0, 0, 5.6), M.green, bevel=0.03)
        outline("annex_line", 2.9, 3.0, 0.04, M.red)
    with frame((-10.01, 6.0, 0.0), (0, 0, rad(-90))):
        box("annex_door", (2.0, 0.1, 2.8), (0, 0, 1.6), M.slate2, bevel=0.04)
        box("annex_door_light", (0.9, 0.06, 0.18), (0, -0.05, 3.25), M.amber, bevel=0)
        for i, y in enumerate((-3.4, 3.4)):
            box(f"annex_side_panel{i}", (2.0, 0.08, 2.0), (y, 0, 5.6), M.green, bevel=0.03)

    box2("tier", (-9.6, 2.2, 8.0), (-2.8, 10.6, 9.9), M.armor, bevel=0.42, segs=3)
    box2("tier_stripe", (-4.6, 2.19, 8.45), (-3.6, 2.3, 9.5), M.green, bevel=0.02)
    box2("tier_vent", (-9.0, 2.16, 8.6), (-6.2, 2.26, 9.3), M.slate, bevel=0.02)
    for i in range(5):
        box2(f"tier_vent_slat{i}", (-8.9, 2.12, 8.68 + i * 0.125), (-6.3, 2.2, 8.74 + i * 0.125), M.slate2, bevel=0)

    # The command module.
    x0, x1, y0, y1, z0, z1 = -9.0, -3.4, 3.4, 9.4, 10.35, 13.2
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    box2("module_base", (x0 + 0.5, y0 + 0.5, 9.85), (x1 - 0.5, y1 - 0.5, z0 + 0.3), M.slate, bevel=0.06)
    for i in range(6):  # the truss under it
        a = x0 + 0.75 + i * 0.85
        box(f"module_truss{i}", (0.9, 0.08, 0.1), (a, y0 + 0.47, 10.1), M.yellow, bevel=0,
            rot=(0, rad(35 if i % 2 else -35), 0))
        box(f"module_truss_s{i}", (0.08, 0.9, 0.1), (x1 - 0.47, y0 + 0.75 + i * 0.85, 10.1), M.yellow, bevel=0,
            rot=(rad(-35 if i % 2 else 35), 0, 0))
    box2("module", (x0, y0, z0), (x1, y1, z1), M.armor, bevel=0.55, segs=3)
    box2("module_band", (x0 - 0.04, y0 - 0.04, z0 + 0.5), (x1 + 0.04, y1 + 0.04, z0 + 1.0), M.green, bevel=0.3, segs=3)
    for i, sx in enumerate((-1, 1)):  # the two screens on the front
        with frame((cx + sx * 1.15, y0 - 0.02, z0 + 1.85)):
            box(f"screen_frame{i}", (1.5, 0.16, 1.25), (0, 0, 0), M.slate2, bevel=0.06)
            box(f"screen{i}", (1.2, 0.06, 0.95), (0, -0.08, 0), M.green2, bevel=0.02)
            for k in range(3):
                box(f"screen_line{i}_{k}", (0.9 - k * 0.25, 0.02, 0.07), (-0.1 - k * 0.05, -0.115, 0.25 - k * 0.22),
                    material("glow_green", "#1B3A1E", 0.4, 0.0, "#7DFF8A", 2.0), bevel=0)
        cyl(f"module_can{i}", 0.3, 0.9, (cx + sx * 1.15, y0 - 0.25, z0 + 0.35), M.yellow, rot=(0, rad(90), 0), segs=16,
            bevel=0.04)
    with frame((x1 + 0.02, cy, z0 + 1.7), (0, 0, rad(90))):  # the right side: a hatch and two horns
        box("module_side_panel", (2.6, 0.08, 1.3), (0.2, 0, 0.1), M.green, bevel=0.03)
        for i, sx in enumerate((-1, 1)):
            cyl(f"module_horn{i}", 0.42, 0.7, (sx * 0.75, -0.3, -0.05), M.slate2, rot=(rad(90), 0, 0), segs=20,
                bevel=0.05)
            cyl(f"module_horn_in{i}", 0.3, 0.1, (sx * 0.75, -0.66, -0.05), M.slate, rot=(rad(90), 0, 0), segs=20)
    with frame((cx, cy, z1)):  # the roof: a hatch, outlines, the mast
        cyl("hatch", 0.85, 0.22, (-0.4, -0.3, 0.08), M.green2, segs=24, bevel=0.05)
        cyl("hatch_top", 0.5, 0.12, (-0.4, -0.3, 0.24), M.metal2, segs=24, bevel=0.03)
        torus("hatch_ring", 1.15, 0.035, (-0.4, -0.3, 0.02), M.red)
        box("roof_unit", (1.2, 0.9, 0.4), (1.5, 1.6, 0.18), M.green, bevel=0.08)
        box("roof_unit_top", (0.8, 0.5, 0.15), (1.5, 1.6, 0.42), M.slate2, bevel=0.04)
        with frame((0, 0, 0.012), (rad(-90), 0, 0)):
            outline("roof_line", 4.3, 4.7, 0.045, M.red)
        with frame((1.9, 2.2, 0.0)):
            cyl("mast_foot", 0.22, 0.5, (0, 0, 0.25), M.metal2, segs=12)
            cyl("mast", 0.07, 5.4, (0, 0, 2.7), M.metal2, segs=10)
            sphere("mast_ball", 0.24, (0, 0, 3.3), M.armor, segs=16, rings=8)
            cyl("mast_dish", 0.2, 0.1, (0, 0, 4.2), M.metal, segs=12)
            box("mast_bar", (0.9, 0.05, 0.05), (0, 0, 4.7), M.metal2, bevel=0)
            sphere("mast_tip", 0.1, (0, 0, 5.45), M.redlight, segs=12, rings=6)
        for i in range(2):  # red pipes over the edge
            tube(f"module_pipe{i}", [(2.0 - i * 0.35, -2.0, 0.05), (2.95, -2.0 - i * 0.2, 0.0), (2.95, -2.1, -1.6),
                                     (2.95, -1.2 + i * 0.4, -2.75)], 0.07, M.red)

    # The machinery between the tower and the door.
    box2("machine", (-8.7, -2.7, 0.0), (-6.2, 0.6, 6.6), M.slate, bevel=0.15)
    box2("machine_cap", (-8.5, -2.5, 6.6), (-6.4, 0.4, 7.0), M.slate2, bevel=0.1)
    block(-8.9, -2.9, -6.0, 0.6)
    with frame((-7.45, -2.71, 0.0)):
        for i in range(4):
            box(f"machine_hazard{i}", (0.45, 0.05, 0.9), (-0.85 + i * 0.57, 0, 1.2), M.yellow if i % 2 == 0 else M.slate,
                bevel=0, rot=(0, rad(20), 0))
        box("machine_grill", (1.9, 0.08, 1.6), (0, 0, 3.4), M.slate2, bevel=0.04)
        for i in range(5):
            box(f"machine_slat{i}", (1.7, 0.06, 0.09), (0, -0.05, 2.8 + i * 0.3), M.metal2, bevel=0)
        box("machine_light", (1.2, 0.06, 0.18), (0, -0.02, 5.2), M.cyan, bevel=0)
        box("machine_stripe", (2.3, 0.05, 0.22), (0, 0, 5.9), M.yellow, bevel=0)
    # The cream hoses: from the module down to the tower, and over the roof's edge to the ground.
    tube("hose_tower", [(-8.6, 3.9, 11.0), (-10.4, 2.6, 11.2), (-11.4, 0.2, 10.2), (-11.6, -1.6, 9.0),
                        (-11.6, -2.3, 7.7)], 0.34, M.cream, segs=5)
    tube("hose_ground", [(-4.6, 1.4, 7.25), (-4.75, -0.4, 6.7), (-5.0, -1.75, 5.9), (-5.15, -1.95, 3.0),
                         (-5.15, -1.95, 0.25)], 0.3, M.cream, segs=5)
    for i, z in enumerate((0.5, 1.5, 2.5, 3.5, 4.5)):
        torus(f"hose_clip{i}", 0.31, 0.05, (-5.15, -1.95, z), M.metal2, segs=16, rsegs=6)
    block(-5.6, -2.4, -4.7, -1.5)


def tower(M):
    cx, cy = -13.2, -4.6
    block(cx - 4.5, cy - 4.1, cx + 4.5, cy + 4.1)

    def loops(spec):
        return [[(x, y, z) for x, y in rrect(hx, hy, r, 6, cx, cy)] for z, hx, hy, r in spec]

    loft("tower_plinth", loops(((0.0, 4.5, 4.1, 1.55), (0.45, 4.5, 4.1, 1.55), (0.6, 4.38, 3.98, 1.5))), M.metal2)
    loft("tower_skirt", loops(((0.3, 4.35, 3.95, 1.5), (3.7, 3.75, 3.35, 1.3))), M.armor)
    loft("tower_band", loops(((3.6, 3.45, 3.05, 1.1), (4.75, 3.45, 3.05, 1.1))), M.slate)
    loft("tower_cap", loops(((4.6, 3.9, 3.5, 1.3), (4.95, 4.3, 3.9, 1.5), (7.25, 4.3, 3.9, 1.5), (7.65, 4.1, 3.7, 1.4),
                             (7.9, 3.6, 3.2, 1.2))), M.armor)
    tilt = math.atan2(0.6, 3.4)
    faces = (((cx, cy - 3.65, 2.0), 0.0, 2.2), ((cx + 4.05, cy, 2.0), rad(90), 2.0), ((cx, cy + 3.65, 2.0), rad(180), 2.2),
             ((cx - 4.05, cy, 2.0), rad(-90), 2.0))
    for f, (origin, turn, half) in enumerate(faces):
        with frame(origin, (0, 0, turn)):
            with frame((0, 0, 0), (-tilt, 0, 0)):  # in the skirt's sloping face
                for i, sx in enumerate((-1, 1)):
                    box(f"skirt_plate{f}_{i}", (1.15, 0.14, 0.75), (sx * half * 0.62, -0.02, 0.55), M.green, bevel=0.05)
                    box(f"skirt_slot{f}_{i}", (0.7, 0.06, 0.14), (sx * half * 0.62, -0.1, 0.55), M.slate, bevel=0)
                outline(f"skirt_line{f}", half * 2.2, 2.3, 0.04, M.red, depth=0.03)
            for i, sx in enumerate((-1, 0, 1)):  # feet, with small lights between them
                with frame((sx * half * 0.8, -0.32, -2.0)):
                    foot(f"tower_foot{f}_{i}", M, (0.95, 0.85, 1.15))
            for i, sx in enumerate((-1, 1)):
                box(f"tower_light{f}_{i}", (0.55, 0.08, 0.14), (sx * half * 0.4, -0.29, -1.3), M.cyan, bevel=0)
        # The yellow truss in the band and the cap's stripe.
        r = (3.05 if f % 2 == 0 else 3.45) + 0.03
        o = (cx + (r if f == 1 else -r if f == 3 else 0), cy + (-r if f == 0 else r if f == 2 else 0), 4.17)
        with frame(o, (0, 0, turn)):
            n = 6 if f % 2 == 0 else 5
            for i in range(n):
                box(f"band_truss{f}_{i}", (1.05, 0.08, 0.11), ((i - (n - 1) / 2) * 0.78, 0, 0), M.yellow, bevel=0,
                    rot=(0, rad(50 if i % 2 else -50), 0))
    # The number, a stripe and a line on the cap's front.
    y = cy - 3.9
    text("tower_number", "10", 2.1, (cx + 1.0, y - 0.012, 6.1), (rad(90), 0, 0), M.green2)
    box2("cap_stripe0", (cx - 2.2, y - 0.03, 5.0), (cx - 1.75, y + 0.05, 7.2), M.green, bevel=0)
    box2("cap_stripe1", (cx - 1.55, y - 0.03, 5.0), (cx - 1.4, y + 0.05, 7.2), M.green, bevel=0)
    with frame((cx + 4.3 + 0.012, cy, 6.1), (0, 0, rad(90))):
        outline("cap_line", 3.6, 1.6, 0.045, M.red)
        box("cap_side_plate", (1.6, 0.06, 0.8), (0.5, 0, 0), M.green, bevel=0.03)
    # The dial on top.
    with frame((cx - 0.5, cy + 0.2, 7.9)):
        cyl("dial", 1.7, 0.16, (0, 0, 0.06), M.armor2, segs=32, bevel=0.04)
        torus("dial_ring", 1.3, 0.05, (0, 0, 0.15), M.red)
        box("dial_hand", (1.15, 0.09, 0.04), (0.42, 0.3, 0.16), M.red, bevel=0, rot=(0, 0, rad(35)))
        cyl("dial_hub", 0.22, 0.14, (0, 0, 0.2), M.slate2, segs=16)
    cyl("cap_beacon", 0.22, 0.3, (cx + 2.7, cy - 2.4, 8.0), M.metal2, segs=12)
    sphere("cap_beacon_light", 0.2, (cx + 2.7, cy - 2.4, 8.2), M.amber, segs=12, rings=6)
    # The link to the building.
    box2("tower_link", (cx + 3.4, cy + 1.6, 0.6), (-8.5, cy + 3.4, 3.4), M.metal2, bevel=0.1)


def tanks(M):
    for i, (x, y) in enumerate(((19.9, -4.0), (19.5, -7.6))):
        with frame((x, y, PAD_Z)):
            r, half = 1.3, 1.9
            cyl(f"tank{i}", r, half * 2, (0, 0, r + 0.35), M.armor, rot=(0, rad(90), 0), segs=28)
            sphere(f"tank_cap{i}", r, (half, 0, r + 0.35), M.slate, scale=(0.75, 1, 1), segs=28, rings=14)
            sphere(f"tank_back{i}", r, (-half, 0, r + 0.35), M.armor, scale=(0.75, 1, 1), segs=28, rings=14)
            for k, tx in enumerate((-1.0, 0.9)):
                cyl(f"tank_band{i}_{k}", r + 0.04, 0.22, (tx, 0, r + 0.35), M.metal, rot=(0, rad(90), 0), segs=28)
                box(f"tank_cradle{i}_{k}", (0.5, 2.3, 0.75), (tx, 0, 0.375), M.metal2, bevel=0.06)
            # A red "Z" on top, its bars across the tank.
            z = 2 * r + 0.36
            for k, (sx, px, a) in enumerate(((0.95, -0.42, 90.0), (0.95, 0.42, 90.0), (1.2, 0.0, -50.0))):
                box(f"tank_mark{i}_{k}", (sx, 0.16, 0.12), (px - 0.05, 0, z - 0.06), M.red, bevel=0, rot=(0, 0, rad(a)))
            cyl(f"tank_valve{i}", 0.2, 0.3, (-1.5, 0, z + 0.05), M.metal2, segs=12)
        block(x - 3.0, y - 1.45, x + 3.0, y + 1.45)


def lamp(M, i, x, y, turn, z=PAD_Z):
    with frame((x, y, z), (0, 0, rad(turn))):
        cyl(f"lamp_foot{i}", 0.3, 0.35, (0, 0, 0.17), M.metal2, segs=12, r2=0.18)
        tube(f"lamp_pole{i}", [(0, 0, 0.3), (0, 0, 2.2), (0, 0, 3.9), (-0.25, 0, 4.7), (0.2, 0, 5.3), (0.95, 0, 5.3),
                               (1.1, 0, 5.05)], 0.1, M.metal2, segs=3, res=10)
        cyl(f"lamp_cap{i}", 0.7, 0.16, (1.1, 0, 5.02), M.slate2, segs=24, bevel=0.04)
        cyl(f"lamp_glow{i}", 0.9, 0.24, (1.1, 0, 4.84), M.cyan, segs=24)
        cyl(f"lamp_plate{i}", 0.62, 0.08, (1.1, 0, 4.7), M.slate, segs=24)
        LEVEL["lamps"].append(game_point(world((1.1, 0, 4.6))))
    block(x - 0.3, y - 0.3, x + 0.3, y + 0.3)


def wall(M, i, x, y, turn, z=0.0):
    with frame((x, y, z), (0, 0, rad(turn))):
        box(f"wall{i}", (3.4, 0.6, 3.9), (0, 0, 1.95), M.wall, bevel=0.22, segs=2)
        box(f"wall_inset{i}", (2.3, 0.1, 2.7), (0, -0.28, 2.05), M.metal2, bevel=0.05)
        box(f"wall_slot{i}", (1.5, 0.08, 0.25), (0, -0.33, 2.9), M.slate, bevel=0)
        box(f"wall_base{i}", (3.7, 0.9, 0.3), (0, 0, 0.15), M.concrete2, bevel=0.05)
    c, s = abs(math.cos(rad(turn))), abs(math.sin(rad(turn)))
    hx, hy = 1.9 * c + 0.5 * s, 1.9 * s + 0.5 * c
    block(x - hx, y - hy, x + hx, y + hy)


def barrel(M, i, x, y, z=PAD_Z):
    with frame((x, y, z)):
        cyl(f"barrel{i}", 0.55, 1.35, (0, 0, 0.675), M.blue, segs=20, bevel=0.03)
        for k, h in enumerate((0.45, 0.9)):
            torus(f"barrel_rib{i}_{k}", 0.555, 0.04, (0, 0, h), M.blue, segs=20, rsegs=6)
        cyl(f"barrel_lid{i}", 0.42, 0.04, (0, 0, 1.36), M.slate2, segs=20)
    block(x - 0.6, y - 0.6, x + 0.6, y + 0.6)


def generator(M, i, x, y, turn, z=PAD_Z, round_body=False):
    with frame((x, y, z), (0, 0, rad(turn))):
        if round_body:
            cyl(f"gen{i}", 0.55, 2.8, (0, 0, 0.75), M.metal, rot=(0, rad(90), 0), segs=20, bevel=0.06)
            for k, tx in enumerate((-0.8, 0.0, 0.8)):
                cyl(f"gen_band{i}_{k}", 0.58, 0.28, (tx, 0, 0.75), M.yellow, rot=(0, rad(90), 0), segs=20)
        else:
            box(f"gen{i}", (2.4, 1.15, 0.95), (0, 0, 0.7), M.metal, bevel=0.14)
            for k, tx in enumerate((-0.65, 0.65)):
                box(f"gen_band{i}_{k}", (0.3, 1.19, 0.99), (tx, 0, 0.7), M.yellow, bevel=0.14)
            box(f"gen_top{i}", (0.9, 0.6, 0.18), (0, 0, 1.25), M.slate2, bevel=0.04)
        for k, tx in enumerate((-0.9, 0.9)):
            box(f"gen_leg{i}_{k}", (0.3, 1.2, 0.3), (tx, 0, 0.15), M.metal2, bevel=0.04)
    c, s = abs(math.cos(rad(turn))), abs(math.sin(rad(turn)))
    hx, hy = 1.4 * c + 0.7 * s, 1.4 * s + 0.7 * c
    block(x - hx, y - hy, x + hx, y + hy)


def bush(M, i, x, y, size=1.0, tree=False):
    with frame((x, y, 0.0)):
        if tree:
            cyl(f"tree_trunk{i}", 0.14 * size, 0.9 * size, (0, 0, 0.45 * size), M.trunk, segs=8)
            for k in range(3):
                cyl(f"tree{i}_{k}", (1.0 - k * 0.25) * size, 1.3 * size, (0, 0, (1.2 + k * 0.75) * size),
                    M.bush if k % 2 == 0 else M.bush2, segs=9, r2=0.05 * size)
        else:
            for k, (dx, dy, r, h) in enumerate(((0, 0, 0.9, 0.75), (0.75, 0.3, 0.65, 0.55), (-0.6, 0.45, 0.6, 0.5),
                                                (0.15, -0.65, 0.55, 0.45))):
                ico(f"bush{i}_{k}", r * size, (dx * size, dy * size, h * size * 0.6), M.bush if k % 2 == 0 else M.bush2,
                    scale=(1, 1, 0.8), rot=(0, 0, k * 1.3))
    block(x - 0.5 * size, y - 0.5 * size, x + 0.5 * size, y + 0.5 * size)


def props(M):
    tanks(M)
    for i, (x, y, turn, z) in enumerate(((-22.3, -6.0, -20, 0.0), (-10.6, -10.9, 20, PAD_Z), (11.5, -12.6, 160, PAD_Z),
                                         (23.6, -9.0, 150, PAD_Z), (25.9, 3.2, 200, 0.0), (15.5, 12.5, 250, PAD_Z),
                                         (-13.5, 12.3, -70, PAD_Z))):
        lamp(M, i, x, y, turn, z)
    for i, (x, y, turn, z) in enumerate(((-20.4, 1.2, 0, PAD_Z), (-16.6, 2.6, 0, PAD_Z), (23.2, -0.6, 0, PAD_Z),
                                         (20.0, 12.2, 180, PAD_Z), (-2.0, 14.9, 180, 0.0), (3.2, 14.9, 180, 0.0))):
        wall(M, i, x, y, turn, z)
    for i, (x, y, z) in enumerate(((-22.4, -2.6, 0.0), (-18.6, -1.6, PAD_Z), (8.4, -10.9, PAD_Z), (9.6, -11.6, PAD_Z),
                                   (9.5, -10.2, PAD_Z), (-18.3, -0.2, PAD_Z), (17.3, 9.8, PAD_Z), (18.5, 10.3, PAD_Z))):
        barrel(M, i, x, y, z)
    generator(M, 0, -18.2, -11.3, 30, 0.0, round_body=True)
    generator(M, 1, 15.6, -12.4, 25)
    generator(M, 2, 18.4, -11.4, 25)
    generator(M, 3, -12.5, 9.5, 90)
    bush(M, 0, 13.6, -15.4, 1.1)
    bush(M, 1, -9.8, -14.6, 0.7)
    bush(M, 2, 22.8, -1.9, 0.9, tree=True)
    bush(M, 3, -22.6, -13.6, 0.9, tree=True)
    bush(M, 4, -21.6, -14.6, 0.6)
    bush(M, 5, 25.6, -14.4, 0.8)
    bush(M, 6, -22.4, 5.6, 1.0, tree=True)
    bush(M, 7, 25.4, 14.4, 1.0)
    bush(M, 8, -8.0, 15.2, 0.8)
    bush(M, 9, 12.0, 15.0, 1.1, tree=True)


def base(M):
    ground(M)
    hangar(M)
    pods(M)
    ramp(M)
    vents(M)
    annex(M)
    tower(M)
    props(M)


# ---------------------------------------------------------------------------------------------------------------------
# The walker (forward is -y here: +z in the game) and the energy cell

HIP = (1.12, 0.1, -0.35)  # in the body's frame
THIGH, SHIN, ANKLE = 1.0, 1.1, 0.3


def walker_body(M):
    sphere("body", 1.0, (0, 0, 0), M.armor, segs=40, rings=20)
    torus("body_seam", 1.004, 0.014, (0, 0, -0.18), M.armor2, segs=40, rsegs=6)
    with frame((0, 0, 0), (rad(-12), 0, 0)):  # the eye, looking a little up
        sphere_cap("eye_glass", 1.03, 0.82, (0, 0, 0), M.visor, rot=(rad(90), 0, 0), segs=40, rings=32)
        torus("eye_rim", 0.63, 0.09, (0, -0.82, 0), M.navy, rot=(rad(90), 0, 0))
        torus("eye_iris", 0.33, 0.05, (0, -0.985, 0), M.cyan, rot=(rad(90), 0, 0), segs=24, rsegs=8)
        cyl("eye_pupil", 0.13, 0.04, (0, -1.03, 0), M.cyan, rot=(rad(90), 0, 0), segs=16)
    sphere_cap("body_cap", 1.025, 0.62, (0, 0, 0), M.navy, rot=(rad(-38), 0, 0))  # a blue cap over the back
    for i, sx in enumerate((-1, 1)):
        cyl(f"hip{i}", 0.38, 0.42, (sx * 0.9, HIP[1], HIP[2]), M.navy, rot=(0, rad(90), 0), segs=20, bevel=0.05)
    cyl("top_hatch", 0.36, 0.1, (0, -0.1, 0.97), M.metal, segs=20)
    cyl("antenna", 0.025, 0.7, (0.35, 0.3, 1.2), M.metal2, segs=8)
    sphere("antenna_tip", 0.06, (0.35, 0.3, 1.56), M.redlight, segs=10, rings=5)
    box("back_pack", (0.9, 0.35, 0.6), (0, 0.85, 0.0), M.slate2, bevel=0.08)
    box("back_light", (0.5, 0.05, 0.1), (0, 1.03, 0.1), M.amber, bevel=0)


def walker_thigh(M):
    cyl("thigh_joint", 0.27, 0.32, (0, 0, 0), M.navy, rot=(0, rad(90), 0), segs=18, bevel=0.04)
    box("thigh", (0.26, 0.36, THIGH), (0, 0, -THIGH / 2), M.navy, bevel=0.07)
    box("thigh_plate", (0.32, 0.12, THIGH * 0.62), (0, -0.19, -THIGH * 0.48), M.armor, bevel=0.05)
    cyl("knee", 0.21, 0.36, (0, 0, -THIGH), M.metal2, rot=(0, rad(90), 0), segs=16, bevel=0.03)


def walker_shin(M):
    box("shin", (0.2, 0.26, SHIN), (0, 0, -SHIN / 2), M.metal2, bevel=0.05)
    box("shin_guard", (0.36, 0.2, SHIN * 0.8), (0, -0.14, -SHIN * 0.5), M.armor, bevel=0.08)
    box("shin_stripe", (0.37, 0.05, 0.12), (0, -0.23, -SHIN * 0.3), M.navy, bevel=0)
    cyl("ankle", 0.17, 0.34, (0, 0, -SHIN), M.metal2, rot=(0, rad(90), 0), segs=14, bevel=0.03)


def walker_foot(M):
    box("foot", (0.5, 1.0, 0.2), (0, -0.12, -ANKLE + 0.1), M.navy, bevel=0.06)
    box("foot_toe", (0.54, 0.34, 0.24), (0, -0.52, -ANKLE + 0.12), M.armor, bevel=0.07)
    box("foot_heel", (0.3, 0.3, 0.16), (0, 0.42, -ANKLE + 0.08), M.metal2, bevel=0.04)
    box("foot_post", (0.2, 0.24, 0.24), (0, 0, -0.14), M.metal2, bevel=0.04)


def cell(M):
    cyl("cell_core", 0.2, 0.62, (0, 0, 0.45), M.cyan, segs=16)
    for i, z in enumerate((0.1, 0.8)):
        cyl(f"cell_cap{i}", 0.27, 0.2, (0, 0, z), M.slate2, segs=16, bevel=0.04)
    for i in range(3):
        a = rad(120 * i)
        box(f"cell_strut{i}", (0.06, 0.06, 0.62), (0.25 * math.cos(a), 0.25 * math.sin(a), 0.45), M.metal, bevel=0)
    cyl("cell_handle", 0.1, 0.1, (0, 0, 0.93), M.yellow, segs=12)


# ---------------------------------------------------------------------------------------------------------------------
# Output


def export(coll, name):
    for ob in bpy.context.scene.objects:
        ob.select_set(False)
    count = 0
    for ob in coll.objects:
        if ob in CUTTERS:
            continue
        ob.select_set(True)
        count += 1
    path = os.path.join(MODEL_DIR, name + ".glb")
    bpy.ops.export_scene.gltf(filepath=path, export_format="GLB", use_selection=True, export_apply=True,
                              export_yup=True, export_cameras=False, export_lights=False, export_animations=False,
                              export_tangents=False, export_materials="EXPORT")
    tris = 0
    dg = bpy.context.evaluated_depsgraph_get()
    for ob in coll.objects:
        if ob in CUTTERS:
            continue
        me = ob.evaluated_get(dg).data
        me.calc_loop_triangles()
        tris += len(me.loop_triangles)
    print(f"wrote {path}: {count} objects, {tris} triangles, {os.path.getsize(path) // 1024} KiB")


def outline_collection(coll, name, thickness):
    """The collection's shapes grown by `thickness` along their vertex normals and turned inside out, in black: drawn
    with back-face culling only their far side shows, as a rim around each shape (an inverted hull). Thin details
    (lines, markings, plates) get none."""
    black = material("outline", "#15171C", 1.0)
    black.use_backface_culling = True  # exported as single-sided: the engine culls the faces toward the camera
    dg = bpy.context.evaluated_depsgraph_get()
    with collection(name) as out:
        for ob in coll.objects:
            if ob in CUTTERS or min(ob.dimensions) < 0.1 or ob.name.startswith(("dirt", "tile_", "pad")):
                continue
            bm = bmesh.new()
            bm.from_mesh(ob.evaluated_get(dg).data)
            bm.normal_update()
            for v in bm.verts:
                v.co += v.normal * thickness
            for f in bm.faces:
                f.normal_flip()
            me = bpy.data.meshes.new(ob.name + "_outline")
            bm.to_mesh(me)
            bm.free()
            me.materials.append(black)
            me.shade_smooth()
            hull = bpy.data.objects.new(ob.name + "_outline", me)
            out.objects.link(hull)
            hull.matrix_world = ob.matrix_world.copy()
    return out


def preview(path, colls, size=(1600, 1200), scale=62.0, target=(1.5, 0.0, 3.0), azimuth=45.0):
    scene = bpy.context.scene
    for c in bpy.data.collections:
        hide = c not in colls
        for ob in c.objects:
            ob.hide_render = hide or ob in CUTTERS
    cam = bpy.data.objects.new("camera", bpy.data.cameras.new("camera"))
    cam.data.type = "ORTHO"
    cam.data.ortho_scale = scale
    scene.collection.objects.link(cam)
    a = rad(azimuth)
    direction = Vector((math.sin(a), -math.cos(a), math.tan(rad(33.0)))).normalized()
    cam.location = Vector(target) + direction * 120.0
    cam.rotation_euler = direction.to_track_quat("Z", "Y").to_euler()
    cam.data.clip_end = 400.0
    scene.camera = cam
    sun = bpy.data.objects.new("sun", bpy.data.lights.new("sun", "SUN"))
    sun.data.energy = 4.0
    sun.data.angle = rad(2.0)
    sun.rotation_euler = Vector((-0.75, -0.35, 0.9)).to_track_quat("Z", "Y").to_euler()
    scene.collection.objects.link(sun)
    world_ = bpy.data.worlds.new("world")
    world_.use_nodes = True
    world_.node_tree.nodes["Background"].inputs[0].default_value = (0.55, 0.62, 0.75, 1.0)
    world_.node_tree.nodes["Background"].inputs[1].default_value = 0.9
    scene.world = world_
    scene.render.resolution_x, scene.render.resolution_y = size
    scene.render.filepath = path
    scene.render.image_settings.file_format = "PNG"
    scene.view_settings.view_transform = "Standard"
    engines = [i.identifier for i in bpy.types.RenderSettings.bl_rna.properties["engine"].enum_items]
    for engine in ("BLENDER_EEVEE_NEXT", "BLENDER_EEVEE", "CYCLES"):
        try:
            scene.render.engine = engine
        except TypeError:
            continue
        try:
            if engine == "CYCLES":
                scene.cycles.samples = 24
            bpy.ops.render.render(write_still=True)
            print(f"preview {path} ({engine}; engines: {engines})")
            break
        except RuntimeError as error:
            print(f"preview with {engine} failed: {error}")
    bpy.data.objects.remove(cam)
    bpy.data.objects.remove(sun)


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    bpy.ops.wm.read_factory_settings(use_empty=True)
    M = materials()
    parts = (("base", base), ("pod_glow", pod_glow), ("walker_body", walker_body), ("walker_thigh", walker_thigh),
             ("walker_shin", walker_shin), ("walker_foot", walker_foot), ("cell", cell))
    colls = {}
    for name, build in parts:
        with collection(name) as c:
            build(M)
            colls[name] = c
    bpy.context.view_layer.update()

    os.makedirs(MODEL_DIR, exist_ok=True)
    for name, _ in parts:
        export(colls[name], name)
        if name != "pod_glow":
            export(outline_collection(colls[name], name + "_outline", 0.07 if name == "base" else 0.045),
                   name + "_outline")

    x0, y0, x1, y1 = TILE
    LEVEL["bounds"] = game_rect(x0 + 0.6, y0 + 0.6, x1 - 0.6, y1 - 0.6)
    LEVEL["pad"] = [[x, -y] for x, y in PAD]
    LEVEL["pad_y"] = PAD_Z
    LEVEL["door"] = game_point(((DOOR_X0 + DOOR_X1) / 2, HY0 - 0.2, FLOOR_Z + 0.06))
    LEVEL["door_half_width"] = (DOOR_X1 - DOOR_X0) / 2 - 0.8
    LEVEL["walker_start"] = game_point((4.5, -9.6, PAD_Z))
    LEVEL["walker"] = {"hip": [HIP[0], HIP[2], -HIP[1]], "thigh": THIGH, "shin": SHIN, "ankle": ANKLE}
    os.makedirs(os.path.dirname(LEVEL_FILE), exist_ok=True)
    with open(LEVEL_FILE, "w", encoding="utf-8") as f:
        json.dump(LEVEL, f, indent=1)
        f.write("\n")
    print(f"wrote {LEVEL_FILE}: {len(LEVEL['blockers'])} blockers, {len(LEVEL['lamps'])} lamps")

    if "--preview" in argv:
        out = argv[argv.index("--preview") + 1]
        with collection("walker_pose") as pose:  # the parts put together as the game does, standing
            height = ANKLE + THIGH * math.cos(rad(35)) + SHIN * math.cos(rad(30)) - HIP[2]
            with frame((0, 0, height)):
                walker_body(M)
                for sx in (-1, 1):
                    with frame((sx * HIP[0], HIP[1], HIP[2]), (rad(35), 0, 0)):
                        walker_thigh(M)
                    knee = Vector((sx * HIP[0], HIP[1] + THIGH * math.sin(rad(35)), HIP[2] - THIGH * math.cos(rad(35))))
                    with frame(knee, (rad(-30), 0, 0)):
                        walker_shin(M)
                    with frame(knee + Vector((0, -SHIN * math.sin(rad(30)), -SHIN * math.cos(rad(30))))):
                        walker_foot(M)
            with frame((2.3, -0.5, 0)):
                cell(M)
            box2("pose_floor", (-4, -4, -0.1), (4, 4, 0.0), M.concrete, bevel=0)
        bpy.context.view_layer.update()
        preview(out, [colls["base"]])
        stem, ext = os.path.splitext(out)
        preview(stem + "_back" + ext, [colls["base"]], azimuth=215.0)
        preview(stem + "_walker" + ext, [pose], size=(800, 800), scale=6.0, target=(0.5, 0, 1.6))
        preview(stem + "_front" + ext, [colls["base"]], size=(1600, 1000), scale=30.0, target=(2.0, -2.0, 3.5),
                azimuth=20.0)


main()
