"""Forests out to the horizon at 1080p from several places: every tree as its relit impostor
(the truth, supersampled) against cheaper ways of drawing far forest. Prints each one's
difference from the truth by distance, and writes the frames and a flick GIF per candidate.

    far_sheet.py [view ...] [candidate ...]
    views: summit, hillside, aerial, horizon, backlit, downsun, unshadowed (default: all)
    candidates: painted, layer, hybrid, crowns (default: crowns)"""

import os
import sys
import time

import numpy as np
from PIL import Image, ImageFilter

import far
import terrain

SIZE = (1920, 1080)
SUN = (0.45, 0.75, 0.35)


class View:
    """Where the eye stands (x, z), how high over the ground, which way it looks, and the
    sun; `unshadowed` draws everything in full sun and sky, to judge cover and colour apart
    from shadow."""

    def __init__(self, at, over, look, sun=SUN, unshadowed=False):
        self.at, self.over, self.look, self.sun = at, over, np.asarray(look, float), sun
        self.unshadowed = unshadowed

    def eye(self, land):
        return np.array([self.at[0], land.sample(land.height, *self.at) + self.over, self.at[1]])

    def target(self, land):
        return self.eye(land) + self.look * 1000.0 / np.linalg.norm(self.look)


VIEWS = {
    # Out from a summit over forested valleys to hills 20 km off.
    "summit": View((0.0, 230.0), 2.0, (0.0, -0.15, 1.0)),
    # Standing in a hillside clearing, over forest toward the summit 9 km off.
    "hillside": View((5750.0, 7500.0), 2.0, (-0.857, -0.04, -0.515)),
    # Flying 1.2 km up, looking down over the stands.
    "aerial": View((0.0, -1500.0), 1200.0, (0.0, -0.45, 1.0)),
    # Level from the summit, so far ridges and their treelines meet the sky.
    "horizon": View((0.0, 230.0), 2.0, (0.45, -0.03, 1.0)),
    # The summit view with a low sun ahead, and behind (looking down-sun).
    "backlit": View((0.0, 230.0), 2.0, (0.0, -0.15, 1.0), sun=(0.1, 0.22, 1.0)),
    "downsun": View((0.0, 230.0), 2.0, (0.0, -0.15, 1.0), sun=(-0.1, 0.35, -1.0)),
    "unshadowed": View((0.0, 230.0), 2.0, (0.0, -0.15, 1.0), unshadowed=True),
}

BANDS = (0, 500, 1500, 3000, 6000, 12000, 20000, 1e8, 2e9)
BAND_NAMES = ("<0.5", "0.5-1.5", "1.5-3", "3-6", "6-12", "12-20", ">20", "sky")


def truth(F, view, name):
    path = f"out/far_truth_{name}.png"
    if os.path.exists(path):
        return Image.open(path).convert("RGB")
    land = F.land
    eye, target = view.eye(land), view.target(land)
    trees = far.visible(land.trees(), eye, target, SIZE)
    t = time.time()
    img = F.frame(eye, target, trees=trees, size=SIZE, sun=view.sun, supersample=2, chunks=32,
                  unshadowed=view.unshadowed)
    print(f"truth {name}: {len(trees['kind'])} trees, {time.time() - t:.0f}s", flush=True)
    img.save(path)
    return img


def detail(img, mask):
    """Fine detail: the spread of what a 1.5 pixel blur takes away, over `mask`."""
    a = np.asarray(img, float).mean(-1)
    b = np.asarray(img.filter(ImageFilter.GaussianBlur(1.5)), float).mean(-1)
    return float((a - b)[mask].std()) if mask.any() else 0.0


def compare(name, img, ref, dist):
    a, b = np.asarray(img, float), np.asarray(ref, float)
    diff = np.abs(a - b).mean(-1)
    bias = (a - b).mean(-1)
    # Tone apart from grain: which crown lands on which pixel is free to differ.
    soft = np.abs(np.asarray(img.filter(ImageFilter.GaussianBlur(3)), float)
                  - np.asarray(ref.filter(ImageFilter.GaussianBlur(3)), float)).mean(-1)
    parts = []
    for lo, hi, label in zip(BANDS[:-1], BANDS[1:], BAND_NAMES):
        m = (dist >= lo) & (dist < hi)
        if m.mean() > 0.002:
            parts.append(f"{label} {diff[m].mean():.1f}/{soft[m].mean():.1f}{bias[m].mean():+.1f}")
    far_m = (dist > 8000) & (dist < 1e8)
    print(f"{name}: mean difference {diff.mean():.2f}; km {' | '.join(parts)}; fine detail >8 km "
          f"{detail(img, far_m):.2f} (truth {detail(ref, far_m):.2f})", flush=True)
    img.save(f"out/far_{name}.png")
    ref.save(f"out/far_{name}_flick.gif", save_all=True, append_images=[img], duration=700,
             loop=0)


def hybrid(near, far_m, stat=None, grain=0.0):
    """Impostors near, thinning by rank into the layer between `near` and `far_m`; in it,
    each tree's own crown until `stat` hands them to statistics."""
    def draw(F, view):
        land = F.land
        eye, target = view.eye(land), view.target(land)
        trees = far.thinned(far.visible(land.trees(), eye, target, SIZE), eye, (near, far_m))
        return F.frame(eye, target, trees=trees, size=SIZE, sun=view.sun, layer=(near, far_m),
                       stat=stat, grain=grain, supersample=2, chunks=32,
                       unshadowed=view.unshadowed)
    return draw


CANDIDATES = {
    "painted": lambda F, v: F.frame(v.eye(F.land), v.target(F.land), size=SIZE, sun=v.sun,
                                    mode=1),
    "layer": lambda F, v: F.frame(v.eye(F.land), v.target(F.land), size=SIZE, sun=v.sun,
                                  layer=(0.0, 0.0), supersample=2, chunks=16),
    # Impostors to 1.5-3 km, then the layer: as statistics only, or with each tree's own
    # crown out to 6-12 km and grain in the statistics past it.
    "hybrid": hybrid(1500, 3000),
    "crowns": hybrid(1500, 3000, (6000, 12000), grain=1.0),
}


if __name__ == "__main__":
    args = sys.argv[1:]
    views = [a for a in args if a in VIEWS] or list(VIEWS)
    names = [a for a in args if a in CANDIDATES] or ["crowns"]
    land = terrain.Land()
    F = far.Far(land, far.load_kinds())
    for vname in views:
        view = VIEWS[vname]
        ref = truth(F, view, vname)
        dist = F.distances(view.eye(land), view.target(land), SIZE)
        for name in names:
            t = time.time()
            img = CANDIDATES[name](F, view)
            print(f"  ({time.time() - t:.0f}s)", end=" ")
            compare(f"{vname}_{name}", img, ref, dist)
