"""Is the Arisen's current spot a good anchor for the Riverwood region?

The Skyrim terrain is placed so that the ground under the Arisen equals his height.
Every other point is then offset by the Skyrim relative height; the lowest point of the
region (the river, -18.3 m) must stay above DDDA's sea level (30013) + 3 m.
Reads tracker_state.json (tracker.py must be running).
"""
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
K = 100 / 70                  # Skyrim units -> DDDA cm
SKY_CENTER = (22528.0, -43008.0)  # Riverwood (cells 3..6, -13..-9)
DD_CENTER = (20000.0, 130000.0)   # centre of DD tiles m61..64 x n50..53
REGION = (0.0, 110000.0, 40000.0, 150000.0)
SEA = 30013.0
MIN_REL_M = -18.3

z = np.load(os.path.join(HERE, "out", "Tamriel_heights.npz"))
H, (cx0, cy0) = z["H"], z["cell_min"]


def sky_h(sx, sy):
    i, j = sx / 128 - cx0 * 32, sy / 128 - cy0 * 32
    i0, j0 = int(np.floor(i)), int(np.floor(j))
    fi, fj = i - i0, j - j0
    return (H[j0, i0] * (1 - fi) * (1 - fj) + H[j0, i0 + 1] * fi * (1 - fj)
            + H[j0 + 1, i0] * (1 - fi) * fj + H[j0 + 1, i0 + 1] * fi * fj)


def to_sky(gx, gz):
    return SKY_CENTER[0] + (gx - DD_CENTER[0]) / K, SKY_CENTER[1] - (gz - DD_CENTER[1]) / K


H0 = sky_h(*SKY_CENTER)


def rel_cm(gx, gz):
    return (sky_h(*to_sky(gx, gz)) - H0) * K


if __name__ == "__main__":
    s = json.load(open(os.path.join(HERE, "tracker_state.json")))
    gx, y, gz = s["global"]
    centre_ground = y - rel_cm(gx, gz)
    lowest = centre_ground + MIN_REL_M * 100
    inside = REGION[0] + 3000 <= gx <= REGION[2] - 3000 and REGION[1] + 3000 <= gz <= REGION[3] - 3000
    ok = inside and lowest >= SEA + 300
    print(f"pos ({gx:.0f}, {y:.0f}, {gz:.0f}) inside={inside}; Skyrim ground here is {rel_cm(gx, gz) / 100:+.1f} m "
          f"relative to Riverwood; lowest point would be {(lowest - SEA) / 100:+.1f} m above the sea "
          f"(needs +3.0) -> {'OK' if ok else 'NOT OK'}")
