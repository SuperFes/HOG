"""Every species by age, and a second seed at full age, each row to one scale."""
import sys
import time

import species
from render import Scene, render, sheet
from skeleton import tube_mesh

COLS = ((8, 1), (18, 1), (30, 1), (30, 2))
PALM_COLS = ((4, 1), (14, 1), (40, 1), (40, 2))

if __name__ == "__main__":
    which = sys.argv[1].split(",") if len(sys.argv) > 1 else species.names()
    rows, labels = [], []
    for name in which:
        t = time.time()
        sp = species.load(name)
        cols = PALM_COLS if sp.model == "palm" else COLS
        sks = {}
        for seed in sorted({s for _, s in cols}):
            ages = {a for a, s in cols if s == seed}
            for a, sk in sp.grow(seed, ages).items():
                sks[(a, seed)] = sk
        meshes = {k: tube_mesh(v) for k, v in sks.items()}
        fit = Scene()
        for k in meshes:
            if k[0] == max(a for a, _ in cols):
                fit.add(meshes[k])
        bounds = fit.bounds()
        rows.append([render(_s, fit=bounds, size=(400, 520))
                     for _s in [(lambda m: (lambda s: (s.add(m), s)[1])(Scene()))(meshes[k])
                                for k in cols]])
        labels.append(f"{name}\n({sp.model})")
        lo, hi = bounds
        print(f"{name}: {time.time() - t:.0f}s, height {hi[1]:.1f} m", flush=True)
    sheet(rows, "Species: ages 8, 18, 30 (palm 4, 14, 40), and a second seed at full age",
          labels, ["young", "middle", "full age", "full age, seed 2"], "out/species.png")
