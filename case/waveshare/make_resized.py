"""Stretch the Waveshare case: PCM compartment +1 mm wide / +3 mm deep, ESP compartment +4 mm deep.

Vertices at or beyond a threshold are shifted, so the geometry on each side of
the cut is kept intact. Reads the user's originals, writes the *-v2 files.
"""
import trimesh

X_CUT = 29.99      # PCM/ESP divider starts at x=30
X_GROW = 1.0       # PCM compartment wider
Y_GROW = 4.0       # ESP compartment deeper (PCM gets +3 of this)
Y_CUT = {"waveshare-back.stl": 28.1, "waveshare-top.stl": 20.0}

for src in Y_CUT:
    m = trimesh.load(src)
    v = m.vertices.copy()
    v[v[:, 0] >= X_CUT, 0] += X_GROW
    v[v[:, 1] >= Y_CUT[src], 1] += Y_GROW
    m.vertices = v
    out = src.replace(".stl", "-v2.stl")
    m.export(out)
    print(out, m.bounds.round(2).tolist(), m.is_watertight)
