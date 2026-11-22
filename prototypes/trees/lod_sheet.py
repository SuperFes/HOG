"""A tree at true screen size (1080p, 60 degree view) over a range of distances: the truth
(rendered 4x larger and shrunk), the full mesh, the thinned mesh, and impostors baked from
fewer and fewer views; each against the truth, with the error over a few views."""
import math
import sys
import time

import numpy as np
from PIL import Image, ImageFilter

import lod
import species
from dress import dressed
from impostor import bake
from render import Scene, render, sheet

DISTANCES = (30.0, 60.0, 120.0, 240.0, 480.0)
VIEW_COUNTS = (12, 8, 6, 4)
LOOKS = ((35.0, 4.0), (80.0, 4.0), (55.0, 25.0))  # (yaw, pitch); the sheet shows the first
SUN = (0.45, 0.75, 0.35)
SHOW = 256  # display pixels per panel
TRUTH = 4


def linear(img):
    return (np.asarray(img, np.float32) / 255.0) ** 2.2


def srgb(a):
    return Image.fromarray((np.clip(a, 0, 1) ** (1 / 2.2) * 255 + 0.5).astype(np.uint8))


def shrink(img, k):
    """Averaged as an MSAA resolve averages: the shaded, display-encoded colours."""
    a = np.asarray(img, np.float32)
    h, w = a.shape[0] // k, a.shape[1] // k
    a = a[:h * k, :w * k].reshape(h, k, w, k, 3).mean(axis=(1, 3))
    return Image.fromarray((a + 0.5).astype(np.uint8))


def error(img, truth, background):
    """Mean difference (0-255) over the tree's pixels, sharp and blurred by 1.5 pixels; the
    blurred one is closer to what a change between the two would look like."""
    a, t, b = (np.asarray(x, np.float32) for x in (img, truth, background))
    tree = (np.abs(t - b).max(-1) > 6) | (np.abs(a - b).max(-1) > 6)
    sharp = np.abs(a - t).mean(-1)[tree].mean()
    blur = [np.asarray(x.filter(ImageFilter.GaussianBlur(1.5)), np.float32)
            for x in (img, truth)]
    soft = np.abs(blur[0] - blur[1]).mean(-1)[tree].mean()
    bias = (a - t).mean(-1)[tree].mean()
    return sharp, soft, bias


def panel_size(distance, extent):
    px = lod.pixel_metres(distance)
    return int(min(max(extent / px * 1.15, 24), SHOW))


def fov_for(panel):
    return math.degrees(2 * math.atan(panel / lod.SCREEN_H * math.tan(
        math.radians(lod.FOV_DEG) / 2)))


def show(img):
    k = max(SHOW // img.size[0], 1)
    big = img.resize((img.size[0] * k, img.size[1] * k), Image.Resampling.NEAREST)
    out = Image.new("RGB", (SHOW, SHOW), (24, 24, 26))
    out.paste(big, ((SHOW - big.size[0]) // 2, (SHOW - big.size[1]) // 2))
    return out


if __name__ == "__main__":
    name = sys.argv[1] if len(sys.argv) > 1 else "oak"
    age = int(sys.argv[2]) if len(sys.argv) > 2 else 30
    sp = species.load(name)
    scene, _ = dressed(sp, 1, age)
    lo, hi = scene.bounds()
    centre = (lo + hi) / 2
    extent = float(max(hi - lo))
    full_tris = sum(len(m[3]) for m, _ in scene.parts)
    density = float(sys.argv[3]) if len(sys.argv) > 3 else 1.5
    imps = {}
    for v in VIEW_COUNTS:
        t = time.time()
        imps[v] = bake(scene, views=v, cell=256)
        imps[v].leaf_density = density
        print(f"baked {v}x{v} views in {time.time() - t:.0f}s", flush=True)
    cols = ["truth", "full mesh", "thinned"] + [f"impostor {v}x{v}" for v in VIEW_COUNTS]
    rows, labels = [], []
    for dist in DISTANCES:
        t0 = time.time()
        panel = panel_size(dist, extent)
        fov = fov_for(panel)
        thin, tris = lod.thinned(scene, lod.pixel_metres(dist))
        errs = {c: [] for c in cols[1:]}
        row = []
        for li, (yaw, pitch) in enumerate(LOOKS):
            kw = dict(yaw=yaw, pitch=pitch, sun_dir=SUN, view=(centre, dist), fov_deg=fov,
                      fit=(lo, hi))
            truth = shrink(render(scene, size=(panel * TRUTH,) * 2, **kw), TRUTH)
            back = render(Scene(), size=(panel, panel), **kw)
            imgs = [render(scene, size=(panel, panel), **kw),
                    render(thin, size=(panel, panel), **kw)]
            imgs += [render(Scene(), size=(panel, panel), impostors=[imps[v]], **kw)
                     for v in VIEW_COUNTS]
            for c, img in zip(cols[1:], imgs):
                errs[c].append(error(img, truth, back))
            if li == 0:
                row = [show(truth)] + [show(i) for i in imgs]
        rows.append(row)
        mean = {c: np.mean(e, axis=0) for c, e in errs.items()}
        labels.append(f"{int(dist)} m\n{panel} px\nthinned\n{tris // 1000}k of "
                      f"{full_tris // 1000}k tris\n\nerror (blurred):\n"
                      + "\n".join(f"{c.replace('impostor ', 'imp ')}: {mean[c][1]:.1f}"
                                  for c in cols[1:]))
        print(f"{int(dist)} m ({time.time() - t0:.0f}s): tris {tris}/{full_tris}; "
              + ", ".join(f"{c} {mean[c][0]:.1f}/{mean[c][1]:.1f}/{mean[c][2]:+.1f}"
                           for c in cols[1:]),
              flush=True)
    sheet(rows, f"{name}, {age} years, at true size, shown enlarged: error is the mean "
          "difference from the truth (0-255) over three views", labels, cols,
          f"out/lod_{name}.png")
