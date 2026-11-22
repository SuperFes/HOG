"""Continuous level of detail: a tree's pieces (leaves, needle ribbons, twigs) drop out with
distance in a fixed random order, and the ones left grow to keep the crown's area, so the
tree thins smoothly instead of switching between models.

A piece's size in pixels decides how many of its kind stay: under `target` pixels, a share
(size/target)^dims of them is kept, each grown to `target` pixels, so on average the same
area covers the screen. In an engine the pieces sit in rank order in the index buffer, so a
draw is one prefix of it, and the vertex shader grows the kept ones about their pivots."""

import math

import numpy as np

from render import Scene

SCREEN_H = 1080
FOV_DEG = 60.0

# Pixels a piece of each kind is kept at, at least; under it, its kind thins.
TARGET = {"leaf": 3.0, "spray": 2.0, "wood": 1.0}


def pixel_metres(distance, screen_h=SCREEN_H, fov_deg=FOV_DEG):
    """Metres one screen pixel spans at `distance`."""
    return distance * 2 * math.tan(math.radians(fov_deg) / 2) / screen_h


def kept(lod, px, target=None, area=1.0):
    """Per vertex: its scale about its pivot (0 where its piece has dropped out)."""
    t = (target or TARGET)[lod["kind"]]
    dims = lod["dims"]
    share = np.clip((lod["size"] / (px * t)) ** dims, 1e-6, 1.0)
    return np.where(lod["rank"] < share, share ** (-area / dims), 0.0)


def local_mean(points, values, cell=1.0):
    """Each point's `values` averaged over the points in its cell of a `cell`-metre grid."""
    key = np.floor(points / cell).astype(np.int64)
    _, inv = np.unique(key, axis=0, return_inverse=True)
    inv = inv.ravel()
    sums = np.zeros((inv.max() + 1, values.shape[1]))
    np.add.at(sums, inv, values)
    counts = np.bincount(inv)[:, None]
    return (sums / counts)[inv]


def _calmed(values, mean, share):
    """Fewer, larger pieces put more of their differences into each pixel; pulling each
    toward its neighbourhood's mean by sqrt(share) keeps a pixel's variance where it was."""
    return mean + (values - mean) * np.sqrt(share)[:, None]


def thinned(scene, px, target=None, area=1.0, calm=True):
    """`scene` as drawn at `px` metres a pixel, and its triangle count."""
    out = Scene()
    tris = 0
    for (mesh, mat), lod in zip(scene.parts, scene.lod):
        if lod is None:
            out.add(mesh, mat)
            tris += len(mesh[3])
            continue
        s = kept(lod, px, target, area)
        p = lod["pivot"] + (mesh[0] - lod["pivot"]) * s[:, None]
        n, c, autumn = mesh[1], mesh[2], mesh[5]
        if calm and lod["kind"] != "wood":
            if "mean" not in lod:
                lod["mean"] = [local_mean(lod["pivot"], a) if a is not None else None
                               for a in (n, c, autumn)]
            share = np.where(s > 0, np.maximum(s, 1e-6) ** (-lod["dims"] / area), 1.0)
            mn, mc, ma = lod["mean"]
            n = _calmed(n, mn, share)
            n = (n / np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-6)).astype("f4")
            c = _calmed(c, mc, share).astype("f4")
            if autumn is not None:
                autumn = _calmed(autumn, ma, share).astype("f4")
        tri = mesh[3]
        # A triangle stays while any of its corners does; an engine draws a prefix that ends
        # at the last kept piece.
        live = (s[tri] > 0).any(axis=1)
        tris += int(live.sum())
        out.add((p.astype("f4"), n, c, tri[live], mesh[4], autumn, lod), mat)
    return out, tris
