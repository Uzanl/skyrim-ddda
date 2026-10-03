"""The tile overlay (docs/terrain-proxy.md, "Overlay"): generated archives never go into
DDDA's folder. They go into overlay/nativePC/..., mirroring the game's layout, and the
DDDA bridge opens them instead of the game's files only in a bridge session (started by
play_bridge.bat). DDDA started from Steam reads its own files.
"""
import os
import shutil

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = r"E:\SteamLibrary\steamapps\common\DDDA"
ROOT = os.path.join(HERE, "overlay")


def of(path):
    """The overlay's place for a game file (path under GAME)."""
    rel = os.path.relpath(os.path.abspath(path), GAME)
    if rel.startswith(".."):
        raise ValueError(f"not a game file: {path}")
    return os.path.join(ROOT, rel)


def current(path):
    """What DDDA reads in a bridge session: the overlay's copy if there is one."""
    o = of(path)
    return o if os.path.exists(o) else path


def put(path, data):
    """Writes data as the overlay's copy of the game file path (never a half-written file:
    the bridge may open it at any moment)."""
    dst = of(path)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    tmp = dst + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, dst)
    return dst


def remove(path):
    o = of(path)
    if os.path.exists(o):
        os.remove(o)


def clear():
    """Removes every generated archive; returns how many there were."""
    if not os.path.isdir(ROOT):
        return 0
    n = sum(len(files) for _, _, files in os.walk(ROOT))
    shutil.rmtree(ROOT)
    return n
