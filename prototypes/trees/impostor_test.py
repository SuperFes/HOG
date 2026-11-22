"""The oak as mesh and as a self-lit impostor side by side, under several suns and views."""
import math
import sys
import time

import numpy as np

import species
from dress import dressed
from impostor import bake
from render import Scene, render, sheet

SUNS = (("morning, side", (-0.85, 0.35, 0.35)), ("noon", (0.25, 0.95, 0.2)),
        ("evening, backlit", (-0.45, 0.28, -0.8)))

if __name__ == "__main__":
    name = sys.argv[1] if len(sys.argv) > 1 else "oak"
    sp = species.load(name)
    scene, _area = dressed(sp, 1, 30)
    t = time.time()
    imp = bake(scene, views=12, cell=256)
    print(f"baked in {time.time() - t:.1f}s", flush=True)
    lo, hi = scene.bounds()
    centre = (lo + hi) / 2
    radius = np.linalg.norm(hi - lo) / 2
    dist = 80.0
    fov = math.degrees(2 * math.atan(radius * 1.05 / dist))
    rows, labels = [], []
    for yaw in (35.0, 110.0):
        for label, sun in SUNS:
            mesh = render(scene, size=(320, 320), yaw=yaw, pitch=6, sun_dir=sun,
                          view=(centre, dist), fov_deg=fov)
            far = render(Scene(), size=(320, 320), yaw=yaw, pitch=6, sun_dir=sun,
                         view=(centre, dist), fov_deg=fov, impostors=[imp], fit=(lo, hi))
            rows.append([mesh, far])
            labels.append(f"{label}\nview {int(yaw)}")
    # Pairs side by side: mesh | impostor, three suns per row.
    grid = [sum((rows[r * 3 + k] for k in range(3)), []) for r in range(2)]
    sheet(grid, f"{name}: mesh (left of each pair) and self-lit impostor (right), 80 m",
          ["view 35", "view 110"],
          sum(([f"{l}: mesh", "impostor"] for l, _ in SUNS), []), "out/impostor.png")
