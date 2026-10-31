# blender -b -P desk_5deck_fbx.py -- <SM_WorkspaceDesk_Simple.uexp> <out.fbx> [--step deg] [--outer deg] [--inner deg]
#     [--uv0 decks|stretch] [--json out.json]
# Builds a five-deck desk from the stock four-deck one (SM_WorkspaceDesk_Simple, read from its cooked .uexp) without
# stretching it: the desk is a surface of revolution about an axis parallel to the mesh's Z (the work surface's
# edges are circles about it), so the arc is lengthened instead. Everything beyond the stock outer decks (--outer
# degrees from the centre line: the raised corners, the desk's ends and their edge fade) turns rigidly outwards by
# 2 * step - outer, and the uniform middle is spread over the widened gap, so five decks stand --step degrees apart
# with the outer two as far inside the ends as the stock outer decks are.
# UV0.u of the work surface runs across the arc (M_Desk_Unlit draws its four deck colours along it). With --uv0 decks
# (the default) it is remapped so each deck place gets the value under the matching stock deck: Decks[0-3] in the
# first four places (stock decks at -outer, -inner, inner, outer) and the fifth deck on the right holding Decks[3]'s.
# --uv0 stretch keeps the stock values, spread with the middle. UV1, the vertex colours and the edge's UVs are kept.
# Output: the FBX (Blender space = UE with Y negated, so the FBX exporter and UE's import undo the flip; metres, so
# modkit_import_mesh.py imports it at scale 0.01) with slots Worksurface2 and Desk_Edge, and <out>.obj, a preview in
# the same Blender space.
import math
import struct
import sys

argv = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else sys.argv[1:]
uexp, fbx = argv[0], argv[1]
opts = dict(zip(argv[2::2], argv[3::2]))
step = float(opts.get('--step', 19.8))
outer = float(opts.get('--outer', 29.7))
inner = float(opts.get('--inner', 10.1))
uv0_mode = opts.get('--uv0', 'decks')
json_out = opts.get('--json')

data = open(uexp, 'rb').read()


def i32(p):
    return struct.unpack_from('<i', data, p)[0]


# ---- the cooked LOD0 render data (UE 4.24 FStaticMeshLODResources) ----
# position buffer: Stride 12, NumVertices, then the bulk array (element size 12, count)
pos_at = next(p for p in range(len(data) - 16) if i32(p) == 12 and i32(p + 8) == 12 and 3 <= i32(p + 4) < 2000000
              and i32(p + 12) == i32(p + 4))
n = i32(pos_at + 4)
pos = [struct.unpack_from('<3f', data, pos_at + 16 + 12 * i) for i in range(n)]

# sections, just before it: MaterialIndex, FirstIndex, NumTriangles, MinVertexIndex, MaxVertexIndex, bEnableCollision,
# bCastShadow
sections = None
for start in range(pos_at - 400, pos_at):
    count = i32(start)
    if not 1 <= count <= 8:
        continue
    recs = [struct.unpack_from('<5I', data, start + 4 + 28 * k) for k in range(count)]
    if recs[0][1] == 0 and all(r[0] < 8 and r[3] <= r[4] < n and 0 < r[2] < 1000000 for r in recs):
        sections = recs
if not sections:
    sys.exit('no section table before the position buffer')

# StaticMeshVertexBuffer: NumTexCoords, NumVertices, bUseFullPrecisionUVs, bUseHighPrecisionTangentBasis, tangents
# (packed normals, 8 bytes), UVs
q = pos_at + 16 + 12 * n
s = next(s for s in range(q, q + 64) if 1 <= i32(s) <= 8 and i32(s + 4) == n)
ntc, full_uv, high_tangents = i32(s), i32(s + 8), i32(s + 12)
if full_uv or high_tangents:
    sys.exit('only half-float UVs and packed tangents are handled')
tan_at = s + 24
uv_at = tan_at + 8 * n + 8


def unpack_normal(b):
    # FPackedNormal: signed bytes over 127
    return [x / 127.0 for x in struct.unpack('<3b', b[:3])]


tangent_x = [unpack_normal(data[tan_at + 8 * i: tan_at + 8 * i + 4]) for i in range(n)]
tangent_z = [unpack_normal(data[tan_at + 8 * i + 4: tan_at + 8 * i + 8]) for i in range(n)]
uvs = [[tuple(float(x) for x in struct.unpack_from('<2e', data, uv_at + (i * ntc + t) * 4)) for t in range(ntc)]
       for i in range(n)]
r = uv_at + 4 * ntc * n
# colour buffer: Stride 4, NumVertices, bulk (element size 4, count), BGRA
c = next(c for c in range(r, r + 32) if i32(c) == 4 and i32(c + 4) == n)
colours = [tuple(data[c + 16 + 4 * i: c + 20 + 4 * i]) for i in range(n)]
r = c + 16 + 4 * n
# index buffer: b32Bit, bulk (element size 1, byte count)
for s in range(r, r + 200000):
    if i32(s) in (0, 1) and i32(s + 4) == 1:
        width = 4 if i32(s) else 2
        cnt = i32(s + 8) // width
        if cnt <= 0 or cnt % 3 or s + 12 + cnt * width > len(data):
            continue
        idx = struct.unpack_from('<%d%s' % (cnt, 'I' if width == 4 else 'H'), data, s + 12)
        if max(idx) < n:
            break
else:
    sys.exit('no index buffer')
print('stock desk: %d verts, sections %s' % (n, [(sec[0], sec[2]) for sec in sections]))

# ---- the arc's axis: a least-squares circle through the work surface's outer edge (UV0.v = 0) ----
surface = sorted(set(idx[sections[0][1]: sections[0][1] + 3 * sections[0][2]]))
rim = [i for i in surface if uvs[i][0][1] < 0.01]
sx = sy = sxx = syy = sxy = sz = sxz = syz = 0.0
for i in rim:
    x, y = pos[i][0], pos[i][1]
    z = x * x + y * y
    sx += x; sy += y; sxx += x * x; syy += y * y; sxy += x * y; sz += z; sxz += x * z; syz += y * z
m = len(rim)
# normal equations of x^2 + y^2 = 2ax + 2by + c
A = [[2 * sxx, 2 * sxy, sx], [2 * sxy, 2 * syy, sy], [2 * sx, 2 * sy, m]]
B = [sxz, syz, sz]


def solve3(A, B):
    det = lambda M: (M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0])
                     + M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]))
    d = det(A)
    return [det([[B[r] if col == k else A[r][col] for col in range(3)] for r in range(3)]) / d for k in range(3)]


cx, cy, cc = solve3(A, B)
print('arc axis at x %.3f y %.3f, outer radius %.3f' % (cx, cy, math.sqrt(cc + cx * cx + cy * cy)))

theta = [math.degrees(math.atan2(p[1] - cy, p[0] - cx)) for p in pos]

# UV0.u of the work surface against the angle (linear in the stock mesh)
st = sum(theta[i] for i in surface) / len(surface)
su = sum(uvs[i][0][0] for i in surface) / len(surface)
slope = (sum((theta[i] - st) * (uvs[i][0][0] - su) for i in surface)
         / sum((theta[i] - st) ** 2 for i in surface))
u_at = lambda t: su + slope * (t - st)
print('work surface UV0.u = %.6f * deg + %.4f' % (slope, su - slope * st))

# ---- the widened desk ----
place = 2.0 * step               # the outer deck places
turn = place - outer             # how far each end turns outwards
spread = place / outer           # the middle's angular spread
knots = [(-place, u_at(-outer)), (-step, u_at(-inner)), (0.0, u_at(inner)), (step, u_at(outer)), (place, u_at(outer))]


def new_angle(t):
    return t * spread if abs(t) <= outer else t + math.copysign(turn, t)


def u_decks(t2):
    for (a0, u0), (a1, u1) in zip(knots, knots[1:]):
        if t2 <= a1:
            return u0 + (u1 - u0) * (t2 - a0) / (a1 - a0)
    return knots[-1][1]


def rotate(v, d, about):
    co, si = math.cos(math.radians(d)), math.sin(math.radians(d))
    x, y = v[0] - about[0], v[1] - about[1]
    return [about[0] + x * co - y * si, about[1] + x * si + y * co, v[2]]


new_pos, new_tx, new_tz, new_uv = [], [], [], []
surface_set = set(surface)
for i in range(n):
    t2 = new_angle(theta[i])
    d = t2 - theta[i]
    new_pos.append(rotate(pos[i], d, (cx, cy)))
    new_tx.append(rotate(tangent_x[i], d, (0.0, 0.0)))
    new_tz.append(rotate(tangent_z[i], d, (0.0, 0.0)))
    row = [list(uv) for uv in uvs[i]]
    if i in surface_set and uv0_mode == 'decks' and abs(theta[i]) <= outer:
        row[0][0] = u_decks(t2)
    new_uv.append(row)
print('step %.2f deg: deck places at %s deg, ends turned %.2f deg, middle spread x%.4f, uv0 %s' % (
    step, [round(k * step, 2) for k in range(-2, 3)], turn, spread, uv0_mode))
xs, ys, zs = zip(*new_pos)
print('five-deck desk bounds X %.2f..%.2f Y %.2f..%.2f Z %.2f..%.2f' % (min(xs), max(xs), min(ys), max(ys), min(zs), max(zs)))

if json_out:
    import json
    json.dump(dict(sections=[dict(mat=sec[0], first=sec[1], tris=sec[2]) for sec in sections], pos=new_pos,
                   uvs=new_uv, cols=colours, idx=idx), open(json_out, 'w'))

try:
    import bpy
except ImportError:
    sys.exit(0)  # outside Blender: the --json output only

# ---- Blender mesh, in Blender space (UE with Y negated; the FBX exporter and UE's import undo it) ----
bpy.ops.wm.read_factory_settings(use_empty=True)
me = bpy.data.meshes.new('SM_WorkspaceDesk_5Deck')
faces, face_mats = [], []
for k, sec in enumerate(sections):
    for t in range(sec[1], sec[1] + 3 * sec[2], 3):
        faces.append((idx[t], idx[t + 1], idx[t + 2]))  # the FBX export and UE's import keep the winding through the Y flip
        face_mats.append(k)
me.from_pydata([(p[0], -p[1], p[2]) for p in new_pos], [], faces)
me.update()
if len(me.polygons) != len(faces):
    sys.exit('Blender dropped %d faces' % (len(faces) - len(me.polygons)))
for name in ('Worksurface2', 'Desk_Edge'):
    me.materials.append(bpy.data.materials.new(name))
me.polygons.foreach_set('material_index', face_mats)
# Adding a layer reallocates the mesh's layer data, so every layer is added before any is looked up
uv_names = ['UVMap' if t == 0 else 'UVMap%d' % (t + 1) for t in range(ntc)]
for name in uv_names:
    me.uv_layers.new(name=name)
me.color_attributes.new('Col', 'BYTE_COLOR', 'CORNER')
layers = [me.uv_layers[name] for name in uv_names]
colour = me.color_attributes['Col']
for poly in me.polygons:
    poly.use_smooth = True
for loop in me.loops:
    v = loop.vertex_index
    for t, layer in enumerate(layers):
        layer.data[loop.index].uv = (new_uv[v][t][0], 1.0 - new_uv[v][t][1])
    b, g, r_, a = colours[v]
    colour.data[loop.index].color_srgb = (r_ / 255.0, g / 255.0, b / 255.0, a / 255.0)
me.normals_split_custom_set_from_vertices([(tz[0], -tz[1], tz[2]) for tz in new_tz])
o = bpy.data.objects.new('SM_WorkspaceDesk_5Deck', me)
bpy.context.scene.collection.objects.link(o)
bpy.context.view_layer.objects.active = o
o.select_set(True)
bpy.ops.export_scene.fbx(filepath=fbx, use_selection=True, apply_unit_scale=True, global_scale=1.0,
                         axis_forward='-Z', axis_up='Y', mesh_smooth_type='FACE', use_mesh_modifiers=False,
                         colors_type='SRGB', add_leaf_bones=False, bake_anim=False)
bpy.ops.wm.obj_export(filepath=fbx.rsplit('.', 1)[0] + '.obj', export_selected_objects=True, forward_axis='Y',
                      up_axis='Z', export_materials=False, export_uv=True, export_normals=True)
print('wrote', fbx)
