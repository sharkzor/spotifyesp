"""Variant with the audio jack and the ESP USB cable hole on the same (front) wall.

Starts from the resized v2 base: fills the round cable hole in the right wall and
cuts a rounded USB slot into the front wall above the ESP compartment. The ESP is
then mounted USB-end-to-the-front, next to the PCM jack.
"""
import numpy as np
import trimesh
from trimesh.boolean import union, difference

HOLE_X = 41.0           # ESP compartment centre (x 33..49)
SLOT_W, SLOT_H = 12.0, 7.0
SLOT_Z = 5.5            # same height as the old round hole
Z_LO, Z_HI = 2.5, 8.5   # old round hole extent

def box(x0, x1, y0, y1, z0, z1):
    return trimesh.creation.box(bounds=[[x0, y0, z0], [x1, y1, z1]])

def slot():
    # rounded rectangle extruded along Y
    r = SLOT_H / 2
    parts = [box(HOLE_X - SLOT_W / 2 + r, HOLE_X + SLOT_W / 2 - r, -1, 4.2, SLOT_Z - r, SLOT_Z + r)]
    for sx in (-1, 1):
        c = trimesh.creation.cylinder(radius=r, height=5.2, sections=48)
        c.apply_transform(trimesh.transformations.rotation_matrix(np.pi / 2, [1, 0, 0]))
        c.apply_translation([HOLE_X + sx * (SLOT_W / 2 - r), 1.6, SLOT_Z])
        parts.append(c)
    return union(parts, engine="manifold")

m = trimesh.load("waveshare-back-v2.stl")
m.merge_vertices()
m.update_faces(m.unique_faces())
trimesh.repair.fix_normals(m)
fill = box(50.9, 53.0, 32.4, 38.6, Z_LO - 0.1, Z_HI + 0.1)
m = union([m, fill], engine="manifold", check_volume=False)
m = difference([m, slot()], engine="manifold", check_volume=False)
m.export("waveshare-back-v2-oneside.stl")
print(m.bounds.round(2).tolist(), m.is_watertight)
