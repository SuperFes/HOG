"""Broadleaves through autumn, whole and close up while turning."""
import sys

import species
from dress import close_target, dressed
from render import render, sheet

SEASONS = ((0.0, "summer"), (0.3, "turning"), (0.5, "peak"), (0.72, "falling"), (1.0, "bare"))
CLOSE = ((0.3, "close, turning"), (0.45, "close, peak"))

if __name__ == "__main__":
    which = sys.argv[1].split(",") if len(sys.argv) > 1 else ["oak", "birch"]
    rows = []
    for name in which:
        sp = species.load(name)
        scenes = [dressed(sp, 1, 30, season=s)[0] for s, _ in SEASONS]
        fit = scenes[0].bounds()
        row = [render(sc, fit=fit, size=(360, 480)) for sc in scenes]
        for s, _ in CLOSE:
            sc, area = dressed(sp, 1, 30, season=s)
            row.append(render(sc, size=(360, 480), view=(close_target(area, 35.0, 0.7), 1.6)))
        rows.append(row)
    sheet(rows, "Autumn: crowns turn by where leaves sit, each leaf by its own pattern", which,
          [label for _, label in SEASONS + CLOSE], "out/autumn.png")
