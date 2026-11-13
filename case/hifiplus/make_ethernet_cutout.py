"""Adds an opening for the W5500 Ethernet module to the HiFi-ESP32-Plus case.

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
LID_WINDOW_WIDTH = 24.0 # lid window width along the wall
WINDOW_DEPTH = 32.0     # how far the lid window reaches in from the +X edge
NOTCH_BOTTOM_Z = 3.0    # base: opening starts here (board surface)
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

base = cut(base, box(x_max - 4, x_max + 1, y_mid - half, y_mid + half,
                     NOTCH_BOTTOM_Z, base.bounds[1][2] + 1))
lid = cut(lid, box(x_max - WINDOW_DEPTH, x_max + 1,
                   y_mid + LID_Y_OFFSET - lid_half, y_mid + LID_Y_OFFSET + lid_half,
                   lid.bounds[0][2] - 1, lid.bounds[1][2] + 1))

base.export(HERE / "esp32-case-bottom-v3-eth.stl")
lid.export(HERE / "esp32-case-top-v3-eth.stl")
print("wrote", base.extents.round(1), lid.extents.round(1))
