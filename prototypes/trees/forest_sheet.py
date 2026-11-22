"""A field of trees from eye height, 15 to 600 metres out, drawn twice at 1080p: every tree
as its full mesh (the truth, rendered 2x larger and shrunk), and every tree at its level of
detail (thinned mesh near, impostor far, crossfaded between). Judged as a whole view, where
a far tree is a few pixels among many, not one tree enlarged.

Each tree is drawn on its own, with its own tight shadow map, and the views composited far to
near; a turned tree is drawn by turning the camera and sun the other way about it."""
import math
import sys
import time

import numpy as np
from PIL import Image

import lod
import species
import sunvis
from dress import dressed
from impostor import bake
from lod_sheet import shrink
from render import Scene, render

SIZE = (1920, 1080)
EYE = np.array([0.0, 1.7, 0.0])
TARGET = np.array([0.0, 1.7 - math.tan(math.radians(2.0)) * 100, 100.0])
SUN = np.array([0.45, 0.75, 0.35])
KINDS = (("oak", 30), ("birch", 30), ("spruce", 30), ("beech", 30))
COUNT = 34
NEAR, FAR = 15.0, 600.0
# Thinned mesh nearer than the first, impostor past the second, a crossfade between.
SWITCH = (120.0, 180.0)


def place(rng):
    trees = []
    half = math.tan(math.radians(lod.FOV_DEG / 2)) * SIZE[0] / SIZE[1]
    while len(trees) < COUNT:
        d = math.exp(rng.uniform(math.log(NEAR), math.log(FAR)))
        x = rng.uniform(-0.85, 0.85) * d * half
        p = np.array([x, 0.0, d])
        # Keep trunks apart so one tree never stands inside another.
        if all(np.linalg.norm(p - q[1]) > 9.0 for q in trees):
            trees.append((int(rng.integers(len(KINDS))), p, rng.uniform(0, 2 * math.pi)))
    return sorted(trees, key=lambda t: -np.linalg.norm(t[1] - EYE))


def local(v, yaw):
    """`v` in the frame of a tree turned by `yaw` about its upright axis."""
    c, s = math.cos(-yaw), math.sin(-yaw)
    return np.array([c * v[0] + s * v[2], v[1], -s * v[0] + c * v[2]])


def frame(tree, scene, impostor=None, scale=1):
    kind, p, yaw = tree
    lo, hi = scene.bounds() if scene is not None else (impostor.centre - impostor.radius,
                                                        impostor.centre + impostor.radius)
    cam = (local(EYE - p, yaw), local(TARGET - p, yaw))
    kw = dict(size=(SIZE[0] * scale, SIZE[1] * scale), camera=cam, fov_deg=lod.FOV_DEG,
              sun_dir=tuple(local(SUN, yaw)), fit=(lo, hi))
    if impostor is not None:
        return render(Scene(), impostors=[impostor], **kw)
    return render(scene, **kw)


def composite(background, layers):
    out = np.asarray(background, np.float32).copy()
    base = out.copy()
    for img in layers:
        a = np.asarray(img, np.float32)
        mask = np.abs(a - base).max(-1) > 1.5
        out[mask] = a[mask]
    return Image.fromarray(out.astype(np.uint8))


if __name__ == "__main__":
    views = int(sys.argv[1]) if len(sys.argv) > 1 else 12
    made = []
    for name, age in KINDS:
        t = time.time()
        sp = species.load(name)
        scene, _ = dressed(sp, 1, age)
        scene.sunvis = sunvis.sun_visibility(scene)
        imp = bake(scene, views=views, cell=256)
        made.append((scene, imp, sum(len(m[3]) for m, _ in scene.parts)))
        print(f"{name}: {time.time() - t:.0f}s", flush=True)
    trees = place(np.random.default_rng(4))
    dummy = (np.full(3, -1.0), np.ones(3))
    background = render(Scene(), size=SIZE, camera=(EYE, TARGET), fov_deg=lod.FOV_DEG,
                        sun_dir=tuple(SUN), fit=dummy)
    big_bg = shrink(render(Scene(), size=(SIZE[0] * 2, SIZE[1] * 2), camera=(EYE, TARGET),
                           fov_deg=lod.FOV_DEG, sun_dir=tuple(SUN), fit=dummy), 2)
    truth_layers, lod_layers = [], []
    truth_tris = lod_tris = impostors = 0
    t = time.time()
    for tree in trees:
        scene, imp, full = made[tree[0]]
        dist = float(np.linalg.norm(tree[1] + np.array([0, 6.0, 0]) - EYE))
        truth_layers.append(shrink(frame(tree, scene, scale=2), 2))
        truth_tris += full
        near_img = far_img = None
        if dist < SWITCH[1]:
            thin, tris = lod.thinned(scene, lod.pixel_metres(dist))
            near_img = frame(tree, thin)
            lod_tris += tris
        if dist > SWITCH[0]:
            far_img = frame(tree, None, impostor=imp)
            impostors += 1
        if near_img is not None and far_img is not None:
            # Stands in for a screen-door crossfade: on average the two blend.
            k = (dist - SWITCH[0]) / (SWITCH[1] - SWITCH[0])
            near_img = Image.blend(near_img, far_img, k)
        lod_layers.append(near_img if near_img is not None else far_img)
    print(f"{len(trees)} trees drawn in {time.time() - t:.0f}s", flush=True)
    truth = composite(big_bg, truth_layers)
    lodded = composite(background, lod_layers)
    truth.save("out/forest_truth.png")
    lodded.save(f"out/forest_lod_{views}.png")
    # Flicking between the two shows a change the way a switch would.
    truth.save(f"out/forest_flick_{views}.gif", save_all=True, append_images=[lodded],
               duration=700, loop=0)
    a, b = np.asarray(lodded, float), np.asarray(truth, float)
    print(f"triangles: truth {truth_tris / 1e6:.1f}M, lod {lod_tris / 1e6:.2f}M "
          f"+ {impostors} impostors; mean difference {np.abs(a - b).mean():.2f}")
