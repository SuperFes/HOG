"""Branch cards against the full mesh, its thinning and the impostor: one tree at true size
over distances, each against the truth (rendered 4x larger and shrunk), over three views.

    card_sheet.py [species] [age]"""
import sys
import time

import numpy as np

import cards
import lod
import species
from dress import dressed
from impostor import bake
from lod_sheet import LOOKS, SUN, TRUTH, error, fov_for, panel_size, show, shrink
from render import Scene, render, sheet

DISTANCES = (30.0, 60.0, 120.0, 240.0, 480.0)
ACROSS = (1.0, 2.0)

if __name__ == "__main__":
    name = sys.argv[1] if len(sys.argv) > 1 else "oak"
    age = int(sys.argv[2]) if len(sys.argv) > 2 else 30
    scene, _ = dressed(species.load(name), 1, age)
    lo, hi = scene.bounds()
    centre = (lo + hi) / 2
    extent = float(max(hi - lo))
    full_tris = sum(len(m[3]) for m, _ in scene.parts)
    carded = {}
    for a in ACROSS:
        t = time.time()
        carded[a] = cards.carded(scene, across=a)
        tris = sum(len(m[3]) for m, _ in carded[a][0].parts)
        print(f"cards {a} m: {carded[a][1]} cards, {tris} tris, {time.time() - t:.0f}s", flush=True)
    imp = bake(scene, views=6, cell=256)
    cols = ["truth", "full mesh", "thinned"] + [f"cards {a:g} m" for a in ACROSS] + ["impostor"]
    rows, labels = [], []
    for dist in DISTANCES:
        t0 = time.time()
        panel = panel_size(dist, extent)
        thin, tris = lod.thinned(scene, lod.pixel_metres(dist))
        errs = {c: [] for c in cols[1:]}
        row = []
        for li, (yaw, pitch) in enumerate(LOOKS):
            kw = dict(yaw=yaw, pitch=pitch, sun_dir=SUN, view=(centre, dist),
                      fov_deg=fov_for(panel), fit=(lo, hi))
            truth = shrink(render(scene, size=(panel * TRUTH,) * 2, **kw), TRUTH)
            back = render(Scene(), size=(panel, panel), **kw)
            imgs = [render(scene, size=(panel, panel), **kw),
                    render(thin, size=(panel, panel), **kw)]
            imgs += [render(carded[a][0], size=(panel, panel), **kw) for a in ACROSS]
            imgs.append(render(Scene(), size=(panel, panel), impostors=[imp], **kw))
            for c, img in zip(cols[1:], imgs):
                errs[c].append(error(img, truth, back))
            if li == 0:
                row = [show(truth)] + [show(i) for i in imgs]
        rows.append(row)
        mean = {c: np.mean(e, axis=0) for c, e in errs.items()}
        labels.append(f"{int(dist)} m\n{panel} px\n\nerror (blurred):\n"
                      + "\n".join(f"{c}: {mean[c][1]:.1f}" for c in cols[1:]))
        print(f"{int(dist)} m ({time.time() - t0:.0f}s): thinned {tris // 1000}k of "
              f"{full_tris // 1000}k tris; "
              + ", ".join(f"{c} {mean[c][0]:.1f}/{mean[c][1]:.1f}/{mean[c][2]:+.1f}"
                          for c in cols[1:]), flush=True)
    sheet(rows, f"{name}, {age} years: branch cards at true size, shown enlarged; error is "
          "the mean difference from the truth (0-255) over three views", labels, cols,
          f"out/cards_{name}.png")
