"""Each species' trunk up close, at chest height, and the whole tree small beside it."""
import sys

import numpy as np

import species
from dress import dressed
from render import render, sheet

AGES = {"coconut": 40, "pine": 60}

if __name__ == "__main__":
    which = sys.argv[1].split(",") if len(sys.argv) > 1 else species.names()
    rows = []
    for name in which:
        sp = species.load(name)
        scene, _area = dressed(sp, 1, AGES.get(name, 30))
        wood = scene.parts[0][0][0]
        band = np.abs(wood[:, 1] - 1.4) < 0.15
        centre = wood[band].mean(axis=0) if band.any() else np.array([0.0, 1.4, 0.0])
        high = np.abs(wood[:, 1] - wood[:, 1].max() * 0.55) < 0.3
        upper = wood[high].mean(axis=0) if high.any() else centre
        rows.append([render(scene, size=(380, 500)),
                     render(scene, size=(380, 500), yaw=35, pitch=4, view=(centre, 1.6)),
                     render(scene, size=(380, 500), yaw=120, pitch=8, view=(upper, 5.0))])
    sheet(rows, "Bark: the tree, its trunk at chest height (1.6 m away), higher up", which,
          ["whole", "trunk close", "higher up"], "out/bark.png")
