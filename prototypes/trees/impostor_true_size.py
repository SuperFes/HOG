"""Mesh and impostor at the size a 1080p screen with a 60 degree view shows them, enlarged
afterwards without smoothing so each screen pixel stays a pixel."""
import sys

import numpy as np
from PIL import Image

import species
from dress import dressed
from impostor import bake
from impostor_test import SUNS
from render import Scene, render, sheet

PANEL = 200  # screen pixels tall
FOV = PANEL / 1080 * 60.0

if __name__ == "__main__":
    name = sys.argv[1] if len(sys.argv) > 1 else "oak"
    sp = species.load(name)
    scene, _ = dressed(sp, 1, 30)
    imp = bake(scene, views=12, cell=256)
    lo, hi = scene.bounds()
    centre = (lo + hi) / 2
    rows, labels = [], []
    for dist in (40.0, 80.0, 150.0):
        row = []
        for _label, sun in SUNS:
            for kind in ("mesh", "impostor"):
                kw = dict(size=(PANEL, PANEL), yaw=35.0, pitch=4, sun_dir=sun,
                          view=(centre, dist), fov_deg=FOV)
                img = (render(scene, **kw) if kind == "mesh" else
                       render(Scene(), impostors=[imp], fit=(lo, hi), **kw))
                row.append(img.resize((PANEL * 2, PANEL * 2), Image.Resampling.NEAREST))
        rows.append(row)
        labels.append(f"{int(dist)} m")
    sheet(rows, f"{name} at true screen size (1080p, 60 degree view), shown 2x", labels,
          sum(([f"{l}: mesh", "impostor"] for l, _ in SUNS), []), "out/impostor_true.png")
