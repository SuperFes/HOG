"""Sheets of one growth model: archetype and seed by row, age by column, each row to one scale."""
import importlib
import sys
import time

from render import Scene, render, sheet
from skeleton import tube_mesh

AGES = (8, 18, 30)
TITLES = {
    "grow_sot": "Self-organising (Palubicki 2009)",
    "grow_param": "Rule sets (Weber-Penn 1995)",
    "grow_sc": "Space colonisation (Runions 2007)",
}


def row(model, params, seed, ages=AGES, size=(420, 560)):
    t = time.time()
    sks = model.grow(params, seed, set(ages))
    print(f"  seed {seed}: {time.time() - t:.1f}s, nodes "
          + ", ".join(str(len(sks[a])) for a in ages), flush=True)
    meshes = {a: tube_mesh(sks[a]) for a in ages}
    big = Scene()
    big.add(meshes[max(ages)])
    fit = big.bounds()
    lo, hi = fit
    print(f"  height {hi[1]:.1f} m, spread {max(hi[0]-lo[0], hi[2]-lo[2]):.1f} m", flush=True)
    out = []
    for a in ages:
        s = Scene()
        s.add(meshes[a])
        out.append(render(s, fit=fit, size=size))
    return out


if __name__ == "__main__":
    name = sys.argv[1]
    kinds = sys.argv[2].split(",") if len(sys.argv) > 2 else ["broadleaf", "conifer"]
    model = importlib.import_module(name)
    rows, labels = [], []
    for kind in kinds:
        print(kind, flush=True)
        for seed in (1, 2):
            rows.append(row(model, getattr(model, kind.upper()), seed))
            labels.append(f"{kind}\nseed {seed}")
    sheet(rows, f"{TITLES.get(name, name)}: wood only, ages to one scale per row", labels,
          [f"{a} years" for a in AGES], f"out/{name}.png")
