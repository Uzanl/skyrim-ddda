"""Surface points of the models of Skyrim's static objects, for real footprints.

Usage: py meshcache.py          -> out/Tamriel_meshpts.npz
       py meshcache.py check    -> compares each mesh's bounds with the ESM's OBND

For every model used by an obstacle candidate (obstacles.candidates()), the render
meshes (nif.py) are sampled every STEP units and the points are deduplicated on a
VOXEL grid, in model space (Skyrim units, int16). Saved as one array `pts` plus
`model` names and `start`/`count` per model.
"""
import os
import sys
import time

import numpy as np

import bsa
import nif
import obstacles
from skyland import DATA, OUT

ARCHIVES = ["Update.bsa", "Skyrim - Meshes1.bsa", "Skyrim - Meshes0.bsa"]
STEP = 12.0
VOXEL = 12.0


def archives():
    return bsa.Archives([os.path.join(DATA, a) for a in ARCHIVES if os.path.exists(os.path.join(DATA, a))])


def sample(T, step=STEP):
    """Points covering triangles T (n, 3, 3) at most `step` apart."""
    if not len(T):
        return np.zeros((0, 3), np.float32)
    a, b, c = T[:, 0], T[:, 1], T[:, 2]
    edge = np.maximum.reduce([np.linalg.norm(b - a, axis=1), np.linalg.norm(c - a, axis=1),
                              np.linalg.norm(c - b, axis=1)])
    n = np.clip(np.ceil(edge / step), 1, 300).astype(int)
    out = []
    for k in np.unique(n):
        sel = n == k
        u, v = np.meshgrid(np.arange(k + 1) / k, np.arange(k + 1) / k)
        m = u + v <= 1.0 + 1e-6
        u, v = u[m], v[m]
        A, B, C = a[sel], b[sel], c[sel]
        P = A[:, None] + u[None, :, None] * (B - A)[:, None] + v[None, :, None] * (C - A)[:, None]
        out.append(P.reshape(-1, 3))
    return np.concatenate(out)


def points(data):
    P = sample(nif.triangles(data))
    if not len(P):
        return np.zeros((0, 3), np.int16)
    q = np.unique(np.round(P / VOXEL).astype(np.int32), axis=0) * VOXEL
    return np.clip(q, -32767, 32767).astype(np.int16)


def build():
    models = sorted(set(obstacles.candidate_models()))
    ar = archives()
    pts, start, count, names = [], [], [], []
    t0, total, missing = time.time(), 0, 0
    for i, m in enumerate(models):
        d = ar.read("meshes\\" + m)
        if d is None:
            missing += 1
            continue
        try:
            p = points(d)
        except Exception as e:  # noqa: BLE001  (an odd file: skip it, the box stays)
            print("  skip", m, e)
            continue
        names.append(m.lower())
        start.append(total)
        count.append(len(p))
        pts.append(p)
        total += len(p)
        if i % 500 == 0:
            print(f"{i}/{len(models)} {time.time() - t0:.0f}s {total} points")
    out = os.path.join(OUT, "Tamriel_meshpts.npz")
    np.savez_compressed(out, pts=np.concatenate(pts), model=np.array(names), start=np.array(start, np.int64),
                        count=np.array(count, np.int64))
    print(f"{len(names)} models ({missing} missing), {total} points, {time.time() - t0:.0f}s -> {out}")


def check():
    """Mesh bounds vs OBND for the candidate models (catches transform mistakes)."""
    z = obstacles.raw()
    ar = archives()
    seen, bad = set(), 0
    for i in obstacles.candidates():
        m = str(z["model"][i]).lower()
        if m in seen or len(seen) > 300:
            continue
        seen.add(m)
        d = ar.read("meshes\\" + m)
        if d is None:
            continue
        T = nif.triangles(d).reshape(-1, 3)
        if not len(T):
            continue
        err = max(np.abs(T.min(0) - z["lo"][i]).max(), np.abs(T.max(0) - z["hi"][i]).max())
        if err > 20:
            bad += 1
            print(f"{m}: mesh {T.min(0).round()} {T.max(0).round()} OBND {z['lo'][i]} {z['hi'][i]}")
    print(f"{len(seen)} models checked, {bad} off by more than 20 units")


if __name__ == "__main__":
    check() if sys.argv[1:] == ["check"] else build()
