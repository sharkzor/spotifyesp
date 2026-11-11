"""Parametric case for XIAO ESP32-S3 + PCM5102A breakout (jack on a long edge) + SMA bulkhead.

Run:  python3 make_case.py   (needs: pip install trimesh manifold3d numpy)
Writes case_base.stl and case_lid.stl next to this script. Units are mm.

Writes case_v2_base.stl and case_v2_lid.stl too: every connector (USB-C, jack, antenna)
on the -X end wall, boards in a column.

Layout of the first version (top view, X to the right, Y up):
  -X wall: USB-C of the XIAO         +X wall: SMA antenna hole
  low Y row: XIAO (USB end at -X)    high Y row: DAC, 3.5 mm jack pointing at the +Y wall
Each board sits in a pocket: a ledge it rests on, a ring of walls around it, and a
post on the lid that presses it down. The ring is left open where the connector sticks out.
"""
import os

import numpy as np
import trimesh

WALL = 2.0
FLOOR = 2.0
CLEAR = 0.3          # play between a board and its pocket walls
DAC_CLEAR_X = 0.7    # extra play along the length of the DAC board
RING = 1.2           # thickness of the pocket walls
LEDGE = 2.0          # height of the ledge the boards rest on (room for solder tails)
LEDGE_OVERLAP = 1.0  # how far the ledge reaches under the board edge
PCB_T = 1.6
LID_T = 2.0
LIP_H = 1.5
LIP_GAP = 0.25       # lip is this much smaller than the cavity, per side

XIAO = (21.0, 17.8)  # X, Y
DAC = (32.0, 17.0)   # PCB only; the jack sticks out JACK_OUT further in +Y
JACK_OUT = 0.0       # the jack passes through the wall, so no room is left for it
JACK_X = 4.3         # jack centre from the DAC end opposite the 6-pin header
JACK_Z = 2.6         # jack centre above the PCB top (estimated from the photo)
JACK_HOLE = 8.5
PART_H = 5.5         # tallest part above the PCB top (jack)
GAP_Y = 6.0          # room between the two pockets for wires
SMA_D = 6.5
SMA_ROOM = 12.0      # free X length right of the XIAO pocket for the connector nut

R = RING + CLEAR
RX = RING + DAC_CLEAR_X
inner_x = max(DAC[0] + 2 * RX, 0.5 + XIAO[0] + R + SMA_ROOM)
xiao_y0 = R                                  # board edge
dac_ring_y0 = R + XIAO[1] + R + GAP_Y
dac_y0 = dac_ring_y0 + R                     # board edge
inner_y = dac_y0 + DAC[1] + JACK_OUT
inner_z = LEDGE + PCB_T + PART_H + 3.0
outer = (inner_x + 2 * WALL, inner_y + 2 * WALL, FLOOR + inner_z)
top_of_pcb = FLOOR + LEDGE + PCB_T


def box(x0, x1, y0, y1, z0, z1):
    b = trimesh.creation.box(extents=(x1 - x0, y1 - y0, z1 - z0))
    b.apply_translation(((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2))
    return b


def cyl_x(d, x0, x1, y, z):
    c = trimesh.creation.cylinder(radius=d / 2, height=x1 - x0, sections=64)
    c.apply_transform(trimesh.transformations.rotation_matrix(np.pi / 2, [0, 1, 0]))
    c.apply_translation(((x0 + x1) / 2, y, z))
    return c


def cyl_y(d, y0, y1, x, z):
    c = trimesh.creation.cylinder(radius=d / 2, height=y1 - y0, sections=64)
    c.apply_transform(trimesh.transformations.rotation_matrix(np.pi / 2, [1, 0, 0]))
    c.apply_translation((x, (y0 + y1) / 2, z))
    return c


def pocket(bx0, by0, bw, bh, open_sides, clear_x=CLEAR, clear_y=CLEAR):
    """Ledge plus wall ring around a board. open_sides: any of 'xmin','xmax','ymin','ymax'."""
    ring_top = FLOOR + LEDGE + PCB_T
    x0, x1 = bx0 - clear_x, bx0 + bw + clear_x
    y0, y1 = by0 - clear_y, by0 + bh + (0 if "ymax" in open_sides else clear_y)
    parts = [
        box(bx0 + LEDGE_OVERLAP, bx0 + bw - LEDGE_OVERLAP, by0 + LEDGE_OVERLAP,
            by0 + bh - LEDGE_OVERLAP, FLOOR - 0.1, FLOOR + LEDGE),
    ]
    # ledge strips under every edge, so the board is supported all around
    parts += [
        box(bx0, bx0 + bw, by0, by0 + LEDGE_OVERLAP, FLOOR - 0.1, FLOOR + LEDGE),
        box(bx0, bx0 + bw, by0 + bh - LEDGE_OVERLAP, by0 + bh, FLOOR - 0.1, FLOOR + LEDGE),
        box(bx0, bx0 + LEDGE_OVERLAP, by0, by0 + bh, FLOOR - 0.1, FLOOR + LEDGE),
        box(bx0 + bw - LEDGE_OVERLAP, bx0 + bw, by0, by0 + bh, FLOOR - 0.1, FLOOR + LEDGE),
    ]
    sides = {
        "xmin": box(x0 - RING, x0, y0 - RING, y1 + RING, FLOOR - 0.1, ring_top),
        "xmax": box(x1, x1 + RING, y0 - RING, y1 + RING, FLOOR - 0.1, ring_top),
        "ymin": box(x0 - RING, x1 + RING, y0 - RING, y0, FLOOR - 0.1, ring_top),
        "ymax": box(x0 - RING, x1 + RING, y1, y1 + RING, FLOOR - 0.1, ring_top),
    }
    for k, v in sides.items():
        if k not in open_sides:
            parts.append(v)
    return parts


base = box(0, outer[0], 0, outer[1], 0, outer[2])
base = base.difference(box(WALL, WALL + inner_x, WALL, WALL + inner_y, FLOOR, outer[2] + 1))

xiao_x0 = WALL + 0.5          # board edge, 0.5 mm from the USB wall
xiao_by0 = WALL + xiao_y0
dac_bx0 = WALL + RX
dac_by0 = WALL + dac_y0

for p in pocket(xiao_x0, xiao_by0, XIAO[0], XIAO[1], {"xmin"}):
    base = base.union(p)
for p in pocket(dac_bx0, dac_by0, DAC[0], DAC[1], {"ymax"}, DAC_CLEAR_X):
    base = base.union(p)

# USB-C: opening sized for a plug housing, not just the connector
xiao_cy = xiao_by0 + XIAO[1] / 2
usb_z = top_of_pcb + 1.6
base = base.difference(box(-1, WALL + 1, xiao_cy - 6, xiao_cy + 6, usb_z - 3.5, usb_z + 3.5))

# 3.5 mm jack on the +Y wall
jack_cx = dac_bx0 + JACK_X
base = base.difference(cyl_y(JACK_HOLE, outer[1] - WALL - 1, outer[1] + 1, jack_cx, top_of_pcb + JACK_Z))

# SMA bulkhead hole on the +X wall, beside the XIAO pocket
base = base.difference(cyl_x(SMA_D, outer[0] - WALL - 1, outer[0] + 1, xiao_cy, FLOOR + inner_z / 2))

# lid: plate, lip that fits the cavity, and one post per board that presses it onto its ledge
lid = box(0, outer[0], 0, outer[1], 0, LID_T)
lid = lid.union(box(WALL + LIP_GAP, WALL + inner_x - LIP_GAP, WALL + LIP_GAP,
                    WALL + inner_y - LIP_GAP, LID_T - 0.1, LID_T + LIP_H))
post_len = outer[2] - top_of_pcb
xiao_end = xiao_x0 + XIAO[0]
lid = lid.union(box(xiao_end - 1.5, xiao_end, xiao_cy - 1.5, xiao_cy + 1.5, LID_T - 0.1, LID_T + post_len))
dac_cy_mid = dac_by0 + DAC[1] / 2
lid = lid.union(box(dac_bx0, dac_bx0 + 1.0, dac_cy_mid - 1.5, dac_cy_mid + 1.5, LID_T - 0.1, LID_T + post_len))

here = os.path.dirname(os.path.abspath(__file__))
for name, mesh in (("case_base", base), ("case_lid", lid)):
    mesh.merge_vertices()
    assert mesh.is_watertight, name
    mesh.export(os.path.join(here, name + ".stl"))
    print(name, mesh.bounds[1] - mesh.bounds[0], "volume mm3", round(mesh.volume))


# ---- version 2: all connectors on the -X end wall ----
SMA_GAP = 11.0                    # gap between the pockets, holds the antenna nut
DR = RING + DAC_CLEAR_X           # DAC pocket wall plus play along its length
x2_inner = 0.5 + XIAO[0] + R + 1.0
dac2_by0 = DR                     # DAC board, long side along Y, jack edge at the -X wall
dac2_ring_end = dac2_by0 + DAC[0] + DR
xiao2_ring0 = dac2_ring_end + SMA_GAP
xiao2_by0 = xiao2_ring0 + R
y2_inner = xiao2_by0 + XIAO[1] + R
outer2 = (x2_inner + 2 * WALL, y2_inner + 2 * WALL, outer[2])

b2 = box(0, outer2[0], 0, outer2[1], 0, outer2[2])
b2 = b2.difference(box(WALL, WALL + x2_inner, WALL, WALL + y2_inner, FLOOR, outer2[2] + 1))

dac2_x0 = WALL                    # jack edge flush with the wall
dac2_y0 = WALL + dac2_by0
xiao2_x0 = WALL + 0.5
xiao2_y0 = WALL + xiao2_by0
for p in pocket(dac2_x0, dac2_y0, DAC[1], DAC[0], {"xmin"}, clear_x=CLEAR, clear_y=DAC_CLEAR_X):
    b2 = b2.union(p)
for p in pocket(xiao2_x0, xiao2_y0, XIAO[0], XIAO[1], {"xmin"}):
    b2 = b2.union(p)

xiao2_cy = xiao2_y0 + XIAO[1] / 2
b2 = b2.difference(box(-1, WALL + 1, xiao2_cy - 6, xiao2_cy + 6, usb_z - 3.5, usb_z + 3.5))
# the jack end of the DAC (4.3 mm from its low-Y end) faces the wall
b2 = b2.difference(cyl_x(JACK_HOLE, -1, WALL + 1, dac2_y0 + JACK_X, top_of_pcb + JACK_Z))
sma2_y = WALL + dac2_ring_end + SMA_GAP / 2
b2 = b2.difference(cyl_x(SMA_D, -1, WALL + 1, sma2_y, FLOOR + inner_z / 2))

l2 = box(0, outer2[0], 0, outer2[1], 0, LID_T)
l2 = l2.union(box(WALL + LIP_GAP, WALL + x2_inner - LIP_GAP, WALL + LIP_GAP,
                  WALL + y2_inner - LIP_GAP, LID_T - 0.1, LID_T + LIP_H))
xiao2_end = xiao2_x0 + XIAO[0]
l2 = l2.union(box(xiao2_end - 1.5, xiao2_end, xiao2_cy - 1.5, xiao2_cy + 1.5, LID_T - 0.1, LID_T + post_len))
dac2_mx = dac2_x0 + DAC[1] / 2
l2 = l2.union(box(dac2_mx - 1.5, dac2_mx + 1.5, dac2_y0, dac2_y0 + 1.0, LID_T - 0.1, LID_T + post_len))

for name, mesh in (("case_v2_base", b2), ("case_v2_lid", l2)):
    mesh.merge_vertices()
    assert mesh.is_watertight, name
    mesh.export(os.path.join(here, name + ".stl"))
    print(name, mesh.bounds[1] - mesh.bounds[0], "volume mm3", round(mesh.volume))
