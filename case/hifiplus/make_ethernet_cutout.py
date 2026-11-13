"""Adds an opening for the W5500 Ethernet module to the HiFi-ESP32-Plus case and
raises the USB-C slot.

Reads the original v3 STLs and writes *-eth.stl next to them. The module sits in the
middle of the +X wall and is taller than the case, so the base wall is notched from the
board level up and the lid gets a matching open-sided window.
"""
from pathlib import Path

import numpy as np
import trimesh

HERE = Path(__file__).parent

MODULE_WIDTH = 18.0     # along the wall
SIDE_CLEAR = 1.0        # extra room each side
LID_WINDOW_WIDTH = 26.0 # lid window width along the wall
WINDOW_DEPTH = 36.0     # how far the lid window reaches in from the +X edge
NOTCH_BOTTOM_Z = 3.0    # base: opening starts here (board surface)
USBC_RAISE = 3.0        # move the USB-C slot (front wall) up by this much
USBC_X = (51.98, 64.08)  # measured on the v3 base
USBC_Z = (2.7, 8.7)
USBC_CORNER = 1.2
WALL_Y = (25.78, 27.78)
LID_Y_OFFSET = 67.2     # lid is exported at +67.2 mm in Y relative to the base


def box(x0, x1, y0, y1, z0, z1):
    b = trimesh.creation.box(extents=[x1 - x0, y1 - y0, z1 - z0])
    b.apply_translation([(x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2])
    return b


def cut(mesh, tool):
    out = mesh.difference(tool, engine="manifold")
    assert out.is_watertight, "result is not watertight"
    return out


base = trimesh.load(HERE / "esp32-case-bottom-v3.stl")
lid = trimesh.load(HERE / "esp32-case-top-v3.stl")

x_max = base.bounds[1][0]
y_mid = (base.bounds[0][1] + base.bounds[1][1]) / 2
half = MODULE_WIDTH / 2 + SIDE_CLEAR
lid_half = LID_WINDOW_WIDTH / 2

def rounded_slot(x0, x1, z0, z1, r, y0, y1):
    # Rounded rectangle in the XZ plane, extruded along Y.
    parts = [box(x0 + r, x1 - r, y0, y1, z0, z1), box(x0, x1, y0, y1, z0 + r, z1 - r)]
    for cx in (x0 + r, x1 - r):
        for cz in (z0 + r, z1 - r):
            c = trimesh.creation.cylinder(radius=r, height=y1 - y0, sections=32)
            c.apply_transform(trimesh.transformations.rotation_matrix(np.pi / 2, [1, 0, 0]))
            c.apply_translation([cx, (y0 + y1) / 2, cz])
            parts.append(c)
    return trimesh.boolean.union(parts, engine="manifold")


# Close the old USB-C slot, then cut it again 3 mm higher.
fill = box(USBC_X[0] - 0.5, USBC_X[1] + 0.5, WALL_Y[0], WALL_Y[1], USBC_Z[0] - 0.5, USBC_Z[1] + 0.5)
base = base.union(fill, engine="manifold")
slot = rounded_slot(USBC_X[0], USBC_X[1], USBC_Z[0] + USBC_RAISE, USBC_Z[1] + USBC_RAISE,
                    USBC_CORNER, WALL_Y[0] - 1, WALL_Y[1] + 1)
base = cut(base, slot)

base = cut(base, box(x_max - 4, x_max + 1, y_mid - half, y_mid + half,
                     NOTCH_BOTTOM_Z, base.bounds[1][2] + 1))
lid = cut(lid, box(x_max - WINDOW_DEPTH, x_max + 1,
                   y_mid + LID_Y_OFFSET - lid_half, y_mid + LID_Y_OFFSET + lid_half,
                   lid.bounds[0][2] - 1, lid.bounds[1][2] + 1))

base.export(HERE / "esp32-case-bottom-v3-eth.stl")
lid.export(HERE / "esp32-case-top-v3-eth.stl")
print("wrote", base.extents.round(1), lid.extents.round(1))
