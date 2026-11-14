"""Variant with the audio jack and the ESP USB cable hole on the same (right) wall.

The PCM compartment (rails, standoff, back stop, jack opening) is rotated 90 degrees
and moved to the front of the right-hand column, so its jack faces the right wall like
the ESP hole. The ESP compartment is pushed back by PCM_BAND mm. The lid is stretched
the same way and its 3x3 hole grid follows the PCM compartment.

Reads waveshare-{back,top}-v2.stl (see make_resized.py), writes *-oneside.stl.
"""
import numpy as np
import trimesh
from trimesh.boolean import union, difference, intersection

Q = 24.0            # extra depth: PCM compartment now sits in front of the ESP one
Y_CUT = 6.0         # vertices at or beyond this y are pushed back by Q
C = 53.0            # outer face of the right wall
D = -5.4            # rotated y = x + D  (puts the old rail against the front lip)
RAIL_X0, PCM_X1 = 9.4, 30.95
PCM_Y0, PCM_Y1 = 4.0, 28.2
DIV_X0, DIV_X1 = 30.9, 33.1
CROSS_Y0, CROSS_Y1 = D + 31.0, D + 33.1   # old divider thickness becomes the cross wall


def box(x0, x1, y0, y1, z0, z1):
    return trimesh.creation.box(bounds=[[x0, y0, z0], [x1, y1, z1]])


def bool_(op, meshes):
    return op(meshes, engine="manifold", check_volume=False)


def rotate(m):
    m = m.copy()
    m.apply_transform(trimesh.transformations.rotation_matrix(np.pi / 2, [0, 0, 1]))
    m.apply_translation([C, D, 0])
    return m


def stretch(m, y_cut):
    v = m.vertices.copy()
    v[v[:, 1] >= y_cut, 1] += Q
    m.vertices = v
    return m


def load(name):
    m = trimesh.load(name)
    m.merge_vertices()
    m.update_faces(m.unique_faces())
    trimesh.repair.fix_normals(m)
    return m


# ---- base ----
b = load("waveshare-back-v2.stl")

# jack opening: wall part (15.5-25.5 x 4-9) and the wider opening in the inner lip (11.5-31 x 2-9)
void = bool_(difference, [
    bool_(union, [box(15.4, 25.6, 0.0, 2.0, 3.9, 9.1), box(11.4, 31.1, 2.0, 4.0, 1.99, 9.1)]),
    b,
])
internals = bool_(intersection, [b, box(RAIL_X0, PCM_X1, PCM_Y0, PCM_Y1, 1.99, 14)])

b = bool_(difference, [b, box(RAIL_X0, PCM_X1, PCM_Y0, PCM_Y1, 1.99, 14)])
b = bool_(union, [b, box(RAIL_X0, PCM_X1, PCM_Y0, PCM_Y1, 1.9, 2.0), void])
b = stretch(b, Y_CUT)

# drop the old divider in front of the ESP compartment, then rebuild it as the cross wall
b = bool_(difference, [b, box(DIV_X0, DIV_X1, 3.9, CROSS_Y1, 2.0, 10)])
b = bool_(difference, [b, rotate(void)])
b = bool_(union, [b, rotate(internals), box(DIV_X0, 51.0, CROSS_Y0, CROSS_Y1, 1.9, 9.0)])
b.export("waveshare-back-v2-oneside.stl")
print("base", b.bounds.round(2).tolist(), b.is_watertight)

# ---- lid ----
t = load("waveshare-top-v2.stl")
pitch = 1.92
old = [(18.83 + i * pitch, 31.35 + j * pitch) for i in range(3) for j in range(3)]
cx, cy = 20.75, 33.25 - 4.0                       # grid centre in the unstretched frame
ncx, ncy = C - cy, cx + D                         # same rotation as the PCM compartment


def cyl(x, y, r, z0, z1):
    c = trimesh.creation.cylinder(radius=r, height=z1 - z0, sections=24)
    c.apply_translation([x, y, (z0 + z1) / 2])
    return c


t = bool_(union, [t] + [cyl(x, y, 0.85, 0.0, 2.0) for x, y in old])
t = stretch(t, Y_CUT)
new = [(ncx + i * pitch, ncy + j * pitch) for i in (-1, 0, 1) for j in (-1, 0, 1)]
t = bool_(difference, [t] + [cyl(x, y, 0.58, -0.1, 2.1) for x, y in new])
t.export("waveshare-top-v2-oneside.stl")
print("lid", t.bounds.round(2).tolist(), t.is_watertight)
