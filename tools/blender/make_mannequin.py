# Builds the default character: a grey mannequin in the spirit of UE's Manny.
#
# Run headless, no Blender UI needed:
#   blender --background --factory-startup --python tools/blender/make_mannequin.py -- <out.glb> [preview.png]
#
# The body is one smooth surface grown from a stick figure with Blender's Skin
# modifier. The detail is painted onto that surface rather than stuck on top:
# dark bands at the joints, lighter plates on the chest, shoulders and hips,
# dark boots - three greys with different roughness, so it reads as a designed
# object under ray-traced light rather than a flat 2006 placeholder.
#
# Blender is Z-up and faces -Y; the glTF exporter turns that into fumar's
# Y-up. The origin sits between the feet.

import math
import sys

import bmesh
import bpy
from mathutils import Vector

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
out_path = argv[0] if argv else "mannequin.glb"
preview_path = argv[1] if len(argv) > 1 else None

bpy.ops.wm.read_factory_settings(use_empty=True)


def material(name, colour, roughness, metallic=0.0):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value = (*colour, 1.0)
    bsdf.inputs["Roughness"].default_value = roughness
    bsdf.inputs["Metallic"].default_value = metallic
    return mat


# Linear reflectances: a mid grey body, near-black joints, lighter cool plates.
BODY = material("Mannequin_Body", (0.30, 0.30, 0.31), 0.5)
JOINT = material("Mannequin_Joint", (0.03, 0.03, 0.035), 0.4, 0.3)
PLATE = material("Mannequin_Plate", (0.55, 0.56, 0.59), 0.3)
BODY_SLOT, JOINT_SLOT, PLATE_SLOT = 0, 1, 2

# --- body: stick figure + Skin modifier -------------------------------------

# name: (position, (radius x, radius y)) - y is depth, so a flatter chest has a smaller y.
joints = {
    "pelvis": ((0.0, 0.0, 0.98), (0.15, 0.105)),
    "waist": ((0.0, 0.0, 1.12), (0.125, 0.09)),
    "chest": ((0.0, -0.005, 1.29), (0.18, 0.12)),
    "upperchest": ((0.0, 0.0, 1.42), (0.165, 0.10)),
    "neck": ((0.0, 0.0, 1.55), (0.05, 0.05)),
}
for side, s in (("L", 1.0), ("R", -1.0)):
    joints.update({
        f"shoulder{side}": ((0.20 * s, 0.0, 1.43), (0.065, 0.065)),
        f"upperarm{side}": ((0.28 * s, 0.0, 1.31), (0.052, 0.054)),
        f"elbow{side}": ((0.36 * s, 0.0, 1.19), (0.04, 0.042)),
        f"forearm{side}": ((0.43 * s, -0.005, 1.09), (0.044, 0.042)),
        f"wrist{side}": ((0.50 * s, 0.0, 0.99), (0.03, 0.026)),
        f"hip{side}": ((0.10 * s, 0.0, 0.92), (0.09, 0.09)),
        f"thigh{side}": ((0.105 * s, -0.005, 0.72), (0.076, 0.08)),
        f"knee{side}": ((0.11 * s, 0.0, 0.52), (0.052, 0.054)),
        f"calf{side}": ((0.11 * s, 0.012, 0.36), (0.054, 0.062)),
        f"ankle{side}": ((0.11 * s, 0.01, 0.11), (0.036, 0.04)),
        f"toe{side}": ((0.11 * s, -0.13, 0.045), (0.042, 0.032)),
    })

bones = [("pelvis", "waist"), ("waist", "chest"), ("chest", "upperchest"), ("upperchest", "neck")]
for side in "LR":
    chain = ["shoulder", "upperarm", "elbow", "forearm", "wrist"]
    bones.append(("upperchest", f"shoulder{side}"))
    bones += [(f"{a}{side}", f"{b}{side}") for a, b in zip(chain, chain[1:])]
    chain = ["hip", "thigh", "knee", "calf", "ankle", "toe"]
    bones.append(("pelvis", f"hip{side}"))
    bones += [(f"{a}{side}", f"{b}{side}") for a, b in zip(chain, chain[1:])]

names = list(joints)
mesh = bpy.data.meshes.new("body")
mesh.from_pydata([joints[n][0] for n in names], [(names.index(a), names.index(b)) for a, b in bones], [])
body = bpy.data.objects.new("Mannequin", mesh)
bpy.context.collection.objects.link(body)

skin = body.modifiers.new("skin", "SKIN")
skin.branch_smoothing = 0.7
skin.use_smooth_shade = True
for i, n in enumerate(names):
    # Limbs a touch thicker than their numbers: subdivision shrinks a thin tube most.
    k = 1.0 if n in ("pelvis", "waist", "chest", "upperchest", "neck") else 1.15
    mesh.skin_vertices[0].data[i].radius = (joints[n][1][0] * k, joints[n][1][1] * k)
mesh.skin_vertices[0].data[names.index("pelvis")].use_root = True
body.modifiers.new("subdiv", "SUBSURF").levels = 3


def bake(obj):
    """Replaces obj's modifiers with the mesh they produce."""
    depsgraph = bpy.context.evaluated_depsgraph_get()
    baked = bpy.data.meshes.new_from_object(obj.evaluated_get(depsgraph))
    obj.modifiers.clear()
    obj.data = baked


bake(body)
for mat in (BODY, JOINT, PLATE):
    body.data.materials.append(mat)

# --- painting the surface ---------------------------------------------------
#
# Each painted region is bounded by planes. The mesh is cut along those planes
# first, so a region's edge is a clean line instead of a staircase of whole
# faces, and then every face is coloured by which side of the planes it is on.



def point(name):
    return Vector(joints[name][0])


def reach(*names):
    return 2.0 * max(max(joints[n][1]) for n in names)


class Region:
    """Faces between two parallel planes, within some distance of a centre line."""

    def __init__(self, slot, centre, axis, half_width, radius, extra=None):
        self.slot, self.centre, self.axis = slot, centre, axis.normalized()
        self.half_width, self.radius, self.extra = half_width, radius, extra

    def contains(self, c):
        along = (c - self.centre).dot(self.axis)
        across = ((c - self.centre) - self.axis * along).length
        return (abs(along) < self.half_width and across < self.radius
                and (self.extra is None or self.extra(c)))

    def planes(self):
        for sign in (-1.0, 1.0):
            yield self.centre + self.axis * (sign * self.half_width), self.axis


def band(slot, a, b, t, half_width):
    pa, pb = point(a), point(b)
    return Region(slot, pa.lerp(pb, t), pb - pa, half_width, reach(a, b))


def slab(slot, z0, z1, extra, radius=0.4):
    return Region(slot, Vector((0, 0, (z0 + z1) / 2)), Vector((0, 0, 1)), (z1 - z0) / 2, radius, extra)


# Order matters: later regions paint over earlier ones.
regions = [
    slab(PLATE_SLOT, 1.21, 1.47, lambda c: abs(c.x) < 0.135 and c.y < -0.03),  # chest plate
    slab(PLATE_SLOT, 0.85, 1.05, lambda c: abs(c.x) < 0.26),  # hips
    slab(PLATE_SLOT, 1.39, 1.60, lambda c: abs(c.x) > 0.165, radius=0.45),  # shoulder caps
    band(JOINT_SLOT, "upperchest", "neck", 0.8, 0.018),
    band(JOINT_SLOT, "waist", "chest", 0.1, 0.016),
    slab(JOINT_SLOT, -0.1, 0.13, None),  # boots
]
for side in "LR":
    regions += [
        band(JOINT_SLOT, f"upperarm{side}", f"elbow{side}", 1.0, 0.016),
        band(JOINT_SLOT, f"forearm{side}", f"wrist{side}", 0.85, 0.012),
        band(JOINT_SLOT, f"thigh{side}", f"knee{side}", 1.0, 0.02),
        band(JOINT_SLOT, f"hip{side}", f"thigh{side}", 0.2, 0.012),
    ]

# Side planes of the chest plate and shoulder caps, cut like the rest.
side_cuts = [(Vector((x, 0, 0)), Vector((1, 0, 0))) for x in (-0.165, -0.135, 0.135, 0.165)]
side_cuts.append((Vector((0, -0.03, 0)), Vector((0, 1, 0))))

bm = bmesh.new()
bm.from_mesh(body.data)
cuts = [(r, co, no) for r in regions for co, no in r.planes()] + [(None, co, no) for co, no in side_cuts]
for region, co, no in cuts:
    # Only cut near the region, so a knee band does not slice the other leg.
    geom = [f for f in bm.faces if region is None or (f.calc_center_median() - region.centre).length < region.radius + 0.1]
    geom = list({e for f in geom for e in f.edges}) + list({v for f in geom for v in f.verts}) + geom
    bmesh.ops.bisect_plane(bm, geom=geom, plane_co=co, plane_no=no, dist=1e-5)
for face in bm.faces:
    c = face.calc_center_median()
    face.material_index = BODY_SLOT
    for region in regions:
        if region.contains(c):
            face.material_index = region.slot
bm.to_mesh(body.data)
bm.free()

# --- head and hands ---------------------------------------------------------

parts = []


def ellipsoid(name, centre, size, slot):
    bpy.ops.mesh.primitive_uv_sphere_add(segments=48, ring_count=24, location=centre)
    obj = bpy.context.active_object
    obj.name = name
    obj.scale = size
    for mat in (BODY, JOINT, PLATE):
        obj.data.materials.append(mat)
    for poly in obj.data.polygons:
        poly.use_smooth = True
        poly.material_index = slot
    parts.append(obj)
    return obj


ellipsoid("Head", (0.0, -0.012, 1.675), (0.094, 0.108, 0.122), BODY_SLOT)

for side in "LR":
    pe, pw = point(f"elbow{side}"), point(f"wrist{side}")
    direction = (pw - pe).normalized()
    hand = ellipsoid(f"Hand{side}", pw + direction * 0.07, (0.022, 0.042, 0.075), BODY_SLOT)
    hand.rotation_mode = "QUATERNION"
    hand.rotation_quaternion = Vector((0, 0, -1)).rotation_difference(direction)

# --- one object, origin at the feet -----------------------------------------

for obj in parts:
    obj.select_set(True)
body.select_set(True)
bpy.context.view_layer.objects.active = body
bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)
bpy.ops.object.join()

lowest = min((body.matrix_world @ v.co).z for v in body.data.vertices)
body.location.z -= lowest
bpy.ops.object.transform_apply(location=True, rotation=False, scale=False)
height = max(v.co.z for v in body.data.vertices)
assert 1.7 < height < 1.9, f"mannequin is {height:.2f} m tall"

bpy.ops.export_scene.gltf(filepath=out_path, export_format="GLB", use_selection=True, export_yup=True)
print(f"wrote {out_path}: {len(body.data.polygons)} faces, {height:.2f} m")

# --- optional preview render ------------------------------------------------

if preview_path:
    scene = bpy.context.scene
    for engine in ("BLENDER_EEVEE", "BLENDER_EEVEE_NEXT", "BLENDER_WORKBENCH"):
        try:
            scene.render.engine = engine
            break
        except TypeError:
            continue
    scene.render.resolution_x, scene.render.resolution_y = 900, 1200
    scene.render.filepath = preview_path

    world = bpy.data.worlds.new("World")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.05, 0.055, 0.06, 1)
    scene.world = world

    bpy.ops.mesh.primitive_plane_add(size=10)
    bpy.context.active_object.data.materials.append(material("Floor", (0.18, 0.18, 0.18), 0.8))

    bpy.ops.object.light_add(type="SUN", rotation=(math.radians(50), math.radians(15), math.radians(-35)))
    bpy.context.active_object.data.energy = 4.0
    bpy.ops.object.light_add(type="AREA", location=(-2.0, -1.5, 2.5))
    bpy.context.active_object.data.energy = 300.0

    bpy.ops.object.camera_add(location=(1.6, -3.4, 1.35))
    camera = bpy.context.active_object
    camera.data.lens = 50
    camera.rotation_euler = (Vector((0, 0, 0.95)) - camera.location).to_track_quat("-Z", "Y").to_euler()
    scene.camera = camera
    bpy.ops.render.render(write_still=True)
    print(f"wrote {preview_path}")
