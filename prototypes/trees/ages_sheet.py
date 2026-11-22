"""One species with foliage at several ages, to one scale, plus a close look at the oldest."""
import sys

import species
from dress import SUN_YAW, close_target, dressed
from render import render, sheet

if __name__ == "__main__":
    name = sys.argv[1]
    ages = [int(a) for a in sys.argv[2].split(",")]
    sp = species.load(name)
    scenes = [dressed(sp, 1, a) for a in ages]
    fit = scenes[-1][0].bounds()
    row = [render(s, fit=fit, size=(420, 600)) for s, _ in scenes]
    scene, area = scenes[-1]
    row.append(render(scene, size=(420, 600), view=(close_target(area, 35.0, 0.8), 3.0)))
    row.append(render(scene, size=(420, 600), yaw=SUN_YAW + 205, pitch=-15,
                      view=(close_target(area, SUN_YAW + 205, 0.6), 4.5)))
    sheet([row], f"{name} by age, to one scale", [name],
          [f"{a} years" for a in ages] + ["close", "backlit, below"], f"out/{name}_ages.png")
