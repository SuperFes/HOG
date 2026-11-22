"""Forests from a summit out to 20 km, at 1080p: every tree as its relit impostor (the truth,
supersampled) against cheaper ways of drawing far forest. Prints each one's difference from
the truth, near and far, and writes the frames and a flick GIF per candidate.

    far_sheet.py [candidate ...]    candidates: painted, layer, hybrid, crowns (default: all)"""

import os
import sys
import time

import numpy as np
from PIL import Image

import far
import terrain

SIZE = (1920, 1080)
EYE_AT = (0.0, 230.0)


def view(land):
    eye = np.array([EYE_AT[0], land.sample(land.height, *EYE_AT) + 2.0, EYE_AT[1]])
    return eye, eye + np.array([0.0, -150.0, 1000.0])


def truth(F, land, eye, target, path="out/far_truth.png"):
    if os.path.exists(path):
        return Image.open(path).convert("RGB")
    trees = far.visible(land.trees(), eye, target, SIZE)
    t = time.time()
    img = F.frame(eye, target, trees=trees, size=SIZE, supersample=2, chunks=32)
    print(f"truth: {len(trees['kind'])} trees, {time.time() - t:.0f}s", flush=True)
    img.save(path)
    return img


def detail(img, rows=(380, 500)):
    """Fine detail in the far forest: the spread of what a 1.5 pixel blur takes away."""
    from PIL import ImageFilter
    a = np.asarray(img, float).mean(-1)
    b = np.asarray(img.filter(ImageFilter.GaussianBlur(1.5)), float).mean(-1)
    return float((a - b)[rows[0]:rows[1]].std())


def compare(name, img, ref):
    a, b = np.asarray(img, float), np.asarray(ref, float)
    rows = np.abs(a - b).mean(axis=(1, 2))
    h = len(rows)
    # Bands of the frame: the far hills sit high, the nearer valley lower.
    print(f"{name}: mean difference {np.abs(a - b).mean():.2f}; by band "
          + " ".join(f"{rows[h * k // 8:h * (k + 1) // 8].mean():.1f}" for k in range(8))
          + f"; fine detail far {detail(img):.2f} (truth {detail(ref):.2f})", flush=True)
    img.save(f"out/far_{name}.png")
    # The forested band, truth over candidate.
    band = (0, 370, SIZE[0], 720)
    pair = Image.new("RGB", (SIZE[0], 2 * (band[3] - band[1])))
    pair.paste(ref.crop(band), (0, 0))
    pair.paste(img.crop(band), (0, band[3] - band[1]))
    pair.save(f"out/far_{name}_band.png")
    ref.save(f"out/far_{name}_flick.gif", save_all=True, append_images=[img], duration=700,
             loop=0)


CANDIDATES = {
    "painted": lambda F, eye, target: F.frame(eye, target, size=SIZE, mode=1),
    "layer": lambda F, eye, target: F.frame(eye, target, size=SIZE, layer=(0.0, 0.0),
                                            supersample=2, chunks=16),
}


def hybrid(near, far_m, stat=None, grain=0.0):
    """Impostors near, thinning by rank into the layer between `near` and `far_m`; in it,
    each tree's own crown until `stat` hands them to statistics."""
    def draw(F, eye, target):
        trees = far.thinned(far.visible(F.land.trees(), eye, target, SIZE), eye, (near, far_m))
        return F.frame(eye, target, trees=trees, size=SIZE, layer=(near, far_m), stat=stat,
                       grain=grain, supersample=2, chunks=32)
    return draw


# Impostors to 1.5-3 km, then the layer: as statistics only, or with each tree's own crown
# out to 6-12 km and grain in the statistics past it.
CANDIDATES["hybrid"] = hybrid(1500, 3000)
CANDIDATES["crowns"] = hybrid(1500, 3000, (6000, 12000), grain=1.0)


if __name__ == "__main__":
    names = sys.argv[1:] or list(CANDIDATES)
    land = terrain.Land()
    kinds = far.load_kinds()
    F = far.Far(land, kinds)
    eye, target = view(land)
    ref = truth(F, land, eye, target)
    for name in names:
        compare(name, CANDIDATES[name](F, eye, target), ref)
